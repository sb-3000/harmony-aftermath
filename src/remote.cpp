#include "remote.h"

#include <RF24.h>
#include <SPI.h>

#include "config.h"
#include "hub_log.h"

namespace {
constexpr uint8_t PIN_CE = 3;     // D1
constexpr uint8_t PIN_CSN = 7;    // D5
constexpr uint8_t PIN_SCK = 4;    // D2
constexpr uint8_t PIN_MISO = 5;   // D3
constexpr uint8_t PIN_MOSI = 6;   // D4
constexpr uint32_t SPI_SPEED_HZ = 4000000;
constexpr uint32_t RADIO_CHECK_MS = 5000;
constexpr uint8_t REMOTE_CHANNEL = 5;
constexpr uint32_t PAIR_STEP_TIMEOUT_MS = 120000;
constexpr uint32_t PAIR_VERIFY_TIMEOUT_MS = 180000;
constexpr uint32_t PREAMBLE_DWELL_MS = 4000;
constexpr uint32_t CANDIDATE_DWELL_MS = 1500;
constexpr uint32_t ADDRESS_CONFIRM_MS = 15000;
constexpr uint8_t ADDRESS_CONFIRMATIONS = 2;
constexpr uint8_t MIN_PAYLOAD_LENGTH = 9;
constexpr uint8_t MAX_PAYLOAD_LENGTH = 11;
constexpr uint16_t CRC_POLY = 0x1021;
constexpr uint16_t CRC_INIT = 0xFFFF;
constexpr uint8_t CANDIDATE_PIPES = 4;

constexpr uint32_t CODE_IDLE = 0x40044C;
constexpr uint32_t CODE_SLEEP = 0x4F0300;
constexpr uint32_t CODE_WOKE = 0x4F0700;
constexpr uint32_t CODE_HELD = 0x400028;
constexpr uint32_t CODE_ALL_RELEASED = 0x4F0004;
constexpr uint32_t CODE_RELEASED_1 = 0xC10000;
constexpr uint32_t CODE_RELEASED_2 = 0xC30000;

RF24 radio(PIN_CE, PIN_CSN, SPI_SPEED_HZ);
RemoteCallback onEvent = nullptr;
RemoteStatus status;
uint32_t activeCode = 0;
uint32_t lastCheck = 0;
bool listening = false;

enum class PairPhase { Idle, SharedAddress, CandidateSweep, ProveCandidate, VerifyPair };
PairPhase pairPhase = PairPhase::Idle;
uint8_t sharedAddress[4] = {};
uint8_t candidateBatch[CANDIDATE_PIPES] = {};
uint8_t candidateBatchSize = 0;
uint8_t nextCandidate = 1;
uint8_t targetCandidate = 0;
uint8_t proofCount = 0;
uint8_t sharedSightings = 0;
bool triedCandidates[256] = {};
uint8_t preambleIndex = 0;
uint32_t pairStarted = 0;
uint32_t candidateStarted = 0;
uint32_t pairDeadline = 0;
uint32_t proofDeadline = 0;
uint32_t roundDeadline = 0;
uint32_t lastPreambleSwitch = 0;
uint32_t controlPackets = 0;

template <typename T>
T minimum(T left, T right) {
  return left < right ? left : right;
}

bool timeReached(uint32_t deadline) {
  return int32_t(millis() - deadline) >= 0;
}

uint8_t captureBit(const uint8_t* capture, uint16_t bit) {
  return (capture[bit / 8] >> (7 - (bit % 8))) & 1;
}

uint16_t crcExtendBit(uint16_t crc, uint8_t bit) {
  bool top = (crc & 0x8000) != 0;
  crc = uint16_t(crc << 1);
  if (top != (bit != 0)) crc ^= CRC_POLY;
  return crc;
}

uint16_t captureBitsAsInt(const uint8_t* capture, uint16_t start, uint8_t count) {
  uint16_t value = 0;
  for (uint8_t i = 0; i < count; i++) value = uint16_t((value << 1) | captureBit(capture, start + i));
  return value;
}

bool recoverSharedAddress(const uint8_t* capture, uint8_t recovered[4]) {
  constexpr uint16_t captureBits = 32 * 8;
  constexpr uint16_t prefixBits = 40 + 9;  // address plus packet control field
  for (uint16_t start = 0; start < 192 && start + prefixBits < captureBits; start++) {
    uint16_t crc = CRC_INIT;
    for (uint16_t i = start; i < start + prefixBits; i++) crc = crcExtendBit(crc, captureBit(capture, i));

    uint16_t crcPosition = start + prefixBits;
    for (uint8_t payloadLength = 0; payloadLength <= MAX_PAYLOAD_LENGTH; payloadLength++) {
      if (payloadLength >= MIN_PAYLOAD_LENGTH && crcPosition + 16 <= captureBits &&
          crc == captureBitsAsInt(capture, crcPosition, 16)) {
        uint8_t address[5] = {};
        for (uint8_t byte = 0; byte < 5; byte++) {
          address[4 - byte] = uint8_t(captureBitsAsInt(capture, start + byte * 8, 8));
        }
        // The short sync address only admits frames whose first byte is zero.
        if (address[0] == 0) {
          memcpy(recovered, address + 1, 4);
          return true;
        }
      }
      if (crcPosition + 8 > captureBits) break;
      for (uint8_t bit = 0; bit < 8; bit++) crc = crcExtendBit(crc, captureBit(capture, crcPosition + bit));
      crcPosition += 8;
    }
  }
  return false;
}

uint64_t addressValue(uint8_t first, const uint8_t shared[4]) {
  // RF24's numeric address uses the first on-air byte as its least-significant byte.
  return uint64_t(first) | (uint64_t(shared[0]) << 8) | (uint64_t(shared[1]) << 16) |
         (uint64_t(shared[2]) << 24) | (uint64_t(shared[3]) << 32);
}

void setPairStatus(const char* state, uint8_t step, uint8_t progress, const String& message) {
  status.pairingState = state;
  status.pairingStep = step;
  status.pairingProgress = progress;
  status.pairingMessage = message;
}

bool configureMatchedRadio() {
  if (!radio.begin(&SPI)) return false;
  radio.stopListening();
  radio.setAddressWidth(5);
  radio.setChannel(REMOTE_CHANNEL);
  radio.setDataRate(RF24_2MBPS);
  radio.setAutoAck(true);
  radio.enableDynamicPayloads();
  radio.setCRCLength(RF24_CRC_16);
  return true;
}

void restorePreviousListener() {
  if (!status.radioOk || !configureMatchedRadio()) return;
  String address = gConfig["remote"]["address"] | "";
  if (address.length() == 10) remoteListen(strtoull(address.c_str(), nullptr, 16), gConfig["remote"]["channel"] | 5);
  else remoteStopListening();
}

void failPairing(const String& message) {
  pairPhase = PairPhase::Idle;
  status.pairing = false;
  setPairStatus("failed", status.pairingStep, status.pairingProgress, message);
  restorePreviousListener();
}

bool configurePromiscuous(uint8_t preamble) {
  if (!radio.begin(&SPI)) return false;
  radio.stopListening();
  radio.setChannel(REMOTE_CHANNEL);
  radio.setDataRate(RF24_2MBPS);
  radio.setAutoAck(false);
  radio.disableCRC();
  radio.setAddressWidth(2);  // The Harmony remote discovery sync trick uses an illegal nRF24 width.
  radio.setPayloadSize(32);
  radio.openReadingPipe(1, uint64_t(preamble) << 8);  // On-air bytes: 00 AA or 00 55.
  radio.startListening();
  return true;
}

bool selectCandidateBatch() {
  candidateBatchSize = 0;
  for (uint16_t checked = 0; checked < 255 && candidateBatchSize < CANDIDATE_PIPES; checked++) {
    uint8_t value = nextCandidate++;
    if (nextCandidate == 0) nextCandidate = 1;
    if (triedCandidates[value]) continue;
    candidateBatch[candidateBatchSize++] = value;
  }
  if (candidateBatchSize == 0) {
    memset(triedCandidates, 0, sizeof(triedCandidates));
    nextCandidate = 1;
    for (uint8_t value = 1; value <= CANDIDATE_PIPES; value++) candidateBatch[candidateBatchSize++] = value;
    nextCandidate = CANDIDATE_PIPES + 1;
  }
  return candidateBatchSize > 0;
}

void openCandidatePipes(const uint8_t* candidates, uint8_t count) {
  radio.stopListening();
  for (uint8_t pipe = 1; pipe <= 5; pipe++) radio.closeReadingPipe(pipe);
  radio.openReadingPipe(1, addressValue(0, sharedAddress));
  for (uint8_t i = 0; i < count; i++) radio.openReadingPipe(uint8_t(i + 2), addressValue(candidates[i], sharedAddress));
  radio.startListening();
}

void resumeCandidateSweep() {
  pairPhase = PairPhase::CandidateSweep;
  selectCandidateBatch();
  openCandidatePipes(candidateBatch, candidateBatchSize);
  roundDeadline = millis() + CANDIDATE_DWELL_MS;
  setPairStatus("searching", 2, status.pairingProgress < 20 ? 20 : status.pairingProgress,
                "Step 2/3: confirming the remote address. Press and release a button.");
}

void beginCandidateSweep() {
  memset(triedCandidates, 0, sizeof(triedCandidates));
  nextCandidate = 1;
  controlPackets = 0;
  candidateStarted = millis();
  pairDeadline = millis() + PAIR_STEP_TIMEOUT_MS;
  status.pairingProgress = 20;
  resumeCandidateSweep();
}

void beginProof(uint8_t candidate, bool alreadySeen) {
  targetCandidate = candidate;
  triedCandidates[candidate] = true;
  proofCount = alreadySeen ? 1 : 0;
  pairPhase = PairPhase::ProveCandidate;
  proofDeadline = millis() + ADDRESS_CONFIRM_MS;
  uint8_t one[] = {candidate};
  openCandidatePipes(one, 1);
  setPairStatus("confirming", 2, status.pairingProgress, "Confirming the candidate address. Press the remote again.");
}

bool persistDiscoveredAddress(uint8_t assignedByte) {
  uint64_t address = addressValue(assignedByte, sharedAddress);
  char text[11];
  snprintf(text, sizeof(text), "%010llX", static_cast<unsigned long long>(address));
  JsonDocument updated;
  updated.set(gConfig);
  updated["remote"]["address"] = text;
  updated["remote"]["channel"] = REMOTE_CHANNEL;
  String serialized;
  serializeJson(updated, serialized);
  String error;
  if (!configReplace(serialized, error)) {
    failPairing("Remote was found, but saving config failed: " + error);
    return false;
  }
  remoteListen(address, REMOTE_CHANNEL);
  pairPhase = PairPhase::Idle;
  status.pairing = false;
  setPairStatus("paired", 3, 100, "Harmony Companion paired. Press a button to confirm reception.");
  logEvent("Harmony remote paired at %s", text);
  return true;
}

void beginFinalCheck(uint8_t assignedByte) {
  pairPhase = PairPhase::VerifyPair;
  targetCandidate = assignedByte;
  pairDeadline = millis() + PAIR_VERIFY_TIMEOUT_MS;
  if (!configureMatchedRadio()) {
    failPairing("Radio stopped responding during final address check");
    return;
  }
  uint64_t assigned = addressValue(assignedByte, sharedAddress);
  radio.openReadingPipe(1, assigned);
  radio.openReadingPipe(2, assigned & ~uint64_t(0xFF));
  radio.startListening();
  setPairStatus("checking", 3, 85, "Step 3/3: press and release any button to verify the remote.");
}

void handleSharedScan() {
  if (timeReached(pairDeadline)) {
    failPairing("Couldn't find the remote. Move it closer and try again.");
    return;
  }

  if (sharedSightings == 0 && millis() - lastPreambleSwitch >= PREAMBLE_DWELL_MS) {
    lastPreambleSwitch = millis();
    preambleIndex ^= 1;
    radio.stopListening();
    radio.openReadingPipe(1, uint64_t(preambleIndex == 0 ? 0xAA : 0x55) << 8);
    radio.startListening();
  }

  while (radio.available()) {
    uint8_t capture[32];
    radio.read(capture, sizeof(capture));
    uint8_t found[4];
    if (!recoverSharedAddress(capture, found)) continue;
    if (sharedSightings == 0 || memcmp(sharedAddress, found, sizeof(found)) != 0) {
      memcpy(sharedAddress, found, sizeof(found));
      sharedSightings = 1;
    } else {
      sharedSightings++;
    }
    char text[9];
    snprintf(text, sizeof(text), "%02X%02X%02X%02X", found[0], found[1], found[2], found[3]);
    setPairStatus("searching", 1, 10, String("Found shared address bytes ") + text + " (" + String(sharedSightings) + "/2)");
    if (sharedSightings >= ADDRESS_CONFIRMATIONS) {
      if (!configureMatchedRadio()) {
        failPairing("Radio stopped responding after shared address discovery");
        return;
      }
      beginCandidateSweep();
      return;
    }
  }
  uint8_t progress = uint8_t(minimum<uint32_t>(19, (millis() - pairStarted) * 19 / PAIR_STEP_TIMEOUT_MS));
  status.pairingProgress = progress;
  if (status.pairingMessage.isEmpty() || sharedSightings == 0) {
    setPairStatus("searching", 1, progress, "Step 1/3: press and release a remote button every few seconds.");
  }
}

void handleCandidateSweep() {
  if (timeReached(pairDeadline)) {
    failPairing(controlPackets ? "Heard the remote, but couldn't confirm its assigned address." :
                                "Heard nothing from the remote. Keep it nearby and try again.");
    return;
  }

  uint8_t hinted = 0;
  uint8_t hit = 0;
  while (radio.available()) {
    uint8_t pipe = 0;
    if (!radio.available(&pipe)) break;
    uint8_t size = radio.getDynamicPayloadSize();
    if (size == 0 || size > 32) {
      radio.flush_rx();
      break;
    }
    uint8_t payload[32] = {};
    radio.read(payload, size);
    if (pipe == 1) {
      controlPackets++;
      if (size > 0 && payload[0] != 0 && hinted == 0 && !triedCandidates[payload[0]]) hinted = payload[0];
    } else if (pipe >= 2 && pipe <= 5) {
      uint8_t position = pipe - 2;
      if (position < candidateBatchSize) hit = candidateBatch[position];
    }
  }

  if (hinted != 0) {
    beginProof(hinted, false);
    return;
  }
  if (hit != 0) {
    beginProof(hit, true);
    return;
  }

  uint32_t elapsed = millis() - candidateStarted;
  uint8_t progress = uint8_t(20 + minimum<uint32_t>(50, (elapsed * 50) / (PAIR_STEP_TIMEOUT_MS + 1)));
  status.pairingProgress = progress;
  if (timeReached(roundDeadline)) {
    selectCandidateBatch();
    openCandidatePipes(candidateBatch, candidateBatchSize);
    roundDeadline = millis() + CANDIDATE_DWELL_MS;
    uint8_t sweepProgress = uint8_t(20 + minimum<uint16_t>(50, uint16_t(nextCandidate) * 50 / 255));
    setPairStatus("searching", 2, sweepProgress, "Step 2/3: address sweep in progress. Press a button again.");
  }
}

void handleCandidateProof() {
  if (timeReached(proofDeadline)) {
    if (timeReached(pairDeadline)) {
      failPairing("Heard the remote, but couldn't confirm its assigned address.");
      return;
    }
    resumeCandidateSweep();
    setPairStatus("searching", 2, status.pairingProgress, "Candidate did not confirm; continuing the address sweep.");
    return;
  }

  while (radio.available()) {
    uint8_t pipe = 0;
    if (!radio.available(&pipe)) break;
    uint8_t size = radio.getDynamicPayloadSize();
    if (size == 0 || size > 32) {
      radio.flush_rx();
      break;
    }
    uint8_t payload[32];
    radio.read(payload, size);
    if (pipe == 2) proofCount++;
    if (proofCount >= ADDRESS_CONFIRMATIONS) {
      beginFinalCheck(targetCandidate);
      return;
    }
  }
}

void handleFinalCheck() {
  if (timeReached(pairDeadline)) {
    failPairing("Address found, but the remote didn't verify. Press any button and try again.");
    return;
  }
  while (radio.available()) {
    uint8_t pipe = 0;
    if (!radio.available(&pipe)) break;
    uint8_t size = radio.getDynamicPayloadSize();
    if (size == 0 || size > 32) {
      radio.flush_rx();
      break;
    }
    uint8_t payload[32];
    radio.read(payload, size);
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < size; i++) checksum = uint8_t(checksum + payload[i]);
    if (checksum != 0 || size < 4) continue;
    uint32_t code = (uint32_t(payload[1]) << 16) | (uint32_t(payload[2]) << 8) | payload[3];
    char buttonCode[7];
    snprintf(buttonCode, sizeof(buttonCode), "%06lX", static_cast<unsigned long>(code));
    if (!gConfig["remote"]["buttons"][buttonCode].isNull()) {
      persistDiscoveredAddress(targetCandidate);
      return;
    }
  }
}

void pairingLoop() {
  switch (pairPhase) {
    case PairPhase::SharedAddress: handleSharedScan(); break;
    case PairPhase::CandidateSweep: handleCandidateSweep(); break;
    case PairPhase::ProveCandidate: handleCandidateProof(); break;
    case PairPhase::VerifyPair: handleFinalCheck(); break;
    case PairPhase::Idle: break;
  }
}

void handlePacket(const uint8_t* payload, uint8_t size) {
  status.packets++;

  uint8_t sum = 0;
  for (uint8_t i = 0; i < size; i++) sum += payload[i];
  if (sum != 0) {
    status.badChecksums++;
    return;
  }
  if (size < 4) return;

  uint32_t code = (uint32_t(payload[1]) << 16) | (uint32_t(payload[2]) << 8) | payload[3];
  switch (code) {
    case CODE_IDLE:
    case CODE_SLEEP:
    case CODE_WOKE:
      return;
    case CODE_HELD:
      onEvent(RemoteEvent::Held, status.lastCode);
      return;
    case CODE_ALL_RELEASED:
    case CODE_RELEASED_1:
    case CODE_RELEASED_2:
      activeCode = 0;
      onEvent(RemoteEvent::Released, status.lastCode);
      return;
    default:
      // The remote can resend a button's code while it's down. Only a release starts a new press.
      if (code == activeCode) {
        onEvent(RemoteEvent::Held, code);
        return;
      }
      activeCode = code;
      status.lastCode = code;
      onEvent(RemoteEvent::Pressed, code);
  }
}
}  // namespace

void remoteBegin(RemoteCallback callback) {
  onEvent = callback;
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
  status.radioOk = radio.begin(&SPI);
  if (!status.radioOk) {
    logEvent("Radio not responding");
    return;
  }
  radio.setPALevel(RF24_PA_LOW);  // Only used for the acknowledgments sent back to the remote.
  radio.setDataRate(RF24_2MBPS);
  radio.enableDynamicPayloads();
  radio.setCRCLength(RF24_CRC_16);
}

void remoteListen(uint64_t address, uint8_t channel) {
  if (!status.radioOk) return;
  radio.stopListening();
  radio.setChannel(channel);
  radio.openReadingPipe(1, address);
  radio.openReadingPipe(2, address & ~uint64_t(0xFF));
  radio.startListening();
  listening = true;
  logEvent("Radio listening on channel %u", channel);
}

void remoteStopListening() {
  if (status.radioOk && listening) radio.stopListening();
  listening = false;
  activeCode = 0;
}

bool remotePairingStart(bool replace, String& error) {
  if (status.pairing) {
    error = "Remote discovery is already running";
    return false;
  }
  if (!status.radioOk) {
    error = "The nRF24 radio is not responding";
    return false;
  }
  if (gConfig["remote"].isNull()) {
    error = "Upload a valid hub config before pairing a remote";
    return false;
  }
  if (!replace && String(gConfig["remote"]["address"] | "").length() == 10) {
    error = "A remote is already paired; use Replace remote";
    return false;
  }

  listening = false;
  pairStarted = millis();
  pairDeadline = pairStarted + PAIR_STEP_TIMEOUT_MS;
  lastPreambleSwitch = pairStarted;
  preambleIndex = 0;
  sharedSightings = 0;
  status.pairing = true;
  memset(sharedAddress, 0, sizeof(sharedAddress));
  setPairStatus("searching", 1, 0, "Step 1/3: press and release a remote button every few seconds.");
  pairPhase = PairPhase::SharedAddress;
  if (!configurePromiscuous(0xAA)) {
    error = "The nRF24 radio stopped responding";
    failPairing(error);
    return false;
  }
  return true;
}

void remotePairingCancel() {
  if (!status.pairing) return;
  pairPhase = PairPhase::Idle;
  status.pairing = false;
  setPairStatus("cancelled", status.pairingStep, status.pairingProgress, "Remote discovery cancelled.");
  restorePreviousListener();
}

void remotePairingClearStatus() {
  if (status.pairing) return;
  setPairStatus("idle", 0, 0, "");
}

void remoteLoop() {
  uint32_t now = millis();
  if (now - lastCheck >= RADIO_CHECK_MS) {
    lastCheck = now;
    bool connected = radio.isChipConnected();
    if (connected != status.radioOk) {
      logEvent(connected ? "Radio responding again" : "Radio stopped responding");
      status.radioOk = connected;
      if (!connected && status.pairing) {
        pairPhase = PairPhase::Idle;
        status.pairing = false;
        setPairStatus("failed", status.pairingStep, status.pairingProgress, "Radio stopped responding during discovery");
      }
    }
  }
  if (!status.radioOk) return;
  if (status.pairing) {
    pairingLoop();
    return;
  }
  if (!listening) return;

  while (radio.available()) {
    uint8_t size = radio.getDynamicPayloadSize();
    if (size == 0 || size > 32) {
      radio.flush_rx();
      return;
    }
    uint8_t payload[32];
    radio.read(payload, size);
    handlePacket(payload, size);
  }
}

RemoteStatus remoteStatus() {
  return status;
}
