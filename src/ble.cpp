#include "ble.h"

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>

#include "hub_log.h"

namespace {
constexpr uint32_t PAIRING_WINDOW_MS = 60000;
constexpr uint32_t KEY_HOLD_MS = 15;
constexpr uint8_t REPORT_KEYBOARD = 1;
constexpr uint8_t REPORT_MEDIA = 2;
constexpr uint8_t REPORT_SYSTEM = 3;

const uint8_t REPORT_MAP[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_KEYBOARD,
    0x05, 0x07,        //   Usage Page (Key Codes), modifier keys
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,  //   Reserved byte
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,              // End Collection
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_MEDIA,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,              // End Collection
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x80,        // Usage (System Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_SYSTEM,
    0x15, 0x00, 0x25, 0x01,  //   Logical range (0-1)
    0x09, 0x82,        //   Usage (System Sleep)
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02,  //   One-bit sleep control
    0x75, 0x07, 0x95, 0x01, 0x81, 0x01,  //   Padding
    0xC0,              // End Collection
};

struct Key {
  const char* name;
  uint16_t code;
  bool media;
  uint8_t modifier;
  bool system;
};

const Key KEYS[] = {
    {"KEY_SHIFT", 0x00, false, 0x02, false},  // Types nothing, so it's safe for waking a PC.
    {"KEY_UP", 0x52, false, 0, false},
    {"KEY_DOWN", 0x51, false, 0, false},
    {"KEY_LEFT", 0x50, false, 0, false},
    {"KEY_RIGHT", 0x4F, false, 0, false},
    {"KEY_ENTER", 0x28, false, 0, false},
    {"KEY_ESC", 0x29, false, 0, false},
    {"KEY_BACKSPACE", 0x2A, false, 0, false},
    {"KEY_TAB", 0x2B, false, 0, false},
    {"KEY_SPACE", 0x2C, false, 0, false},
    {"MEDIA_PLAY_PAUSE", 0xCD, true, 0, false},
    {"MEDIA_STOP", 0xB7, true, 0, false},
    {"MEDIA_NEXT", 0xB5, true, 0, false},
    {"MEDIA_PREVIOUS", 0xB6, true, 0, false},
    {"MEDIA_MUTE", 0xE2, true, 0, false},
    {"MEDIA_VOLUME_UP", 0xE9, true, 0, false},
    {"MEDIA_VOLUME_DOWN", 0xEA, true, 0, false},
    {"SYSTEM_SLEEP", 0x82, false, 0, true},
};

Preferences preferences;
NimBLEServer* server = nullptr;
NimBLEHIDDevice* hid = nullptr;
NimBLECharacteristic* keyboardInput = nullptr;
NimBLECharacteristic* mediaInput = nullptr;
NimBLECharacteristic* systemInput = nullptr;

// Written by the Bluetooth task, read by the main loop.
volatile bool connected = false;
volatile bool needsAdvertising = false;
char connectedAddress[18] = "";
uint16_t connectionHandle = 0;

String pairingName;
uint32_t pairingUntil = 0;

String savedAddress(const String& name) {
  return preferences.getString(name.c_str(), "");
}

bool isSavedAddress(const char* address) {
  for (int i = 0; i < preferences.getInt("count", 0); i++) {
    String name = preferences.getString(("n" + String(i)).c_str(), "");
    if (!name.isEmpty() && savedAddress(name) == address) return true;
  }
  return false;
}

void rememberName(const String& name) {
  int count = preferences.getInt("count", 0);
  for (int i = 0; i < count; i++) {
    if (preferences.getString(("n" + String(i)).c_str(), "") == name) return;
  }
  preferences.putString(("n" + String(count)).c_str(), name);
  preferences.putInt("count", count + 1);
}

bool anyPaired() {
  for (int i = 0; i < preferences.getInt("count", 0); i++) {
    String name = preferences.getString(("n" + String(i)).c_str(), "");
    if (!name.isEmpty() && !savedAddress(name).isEmpty()) return true;
  }
  return false;
}

void updateAdvertising() {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  bool wanted = !connected && (!pairingName.isEmpty() || anyPaired());
  if (wanted && !advertising->isAdvertising()) {
    advertising->start();
  } else if (!wanted && advertising->isAdvertising()) {
    advertising->stop();
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    connectionHandle = desc->conn_handle;
    connected = true;
  }

  void onDisconnect(NimBLEServer*) override {
    connected = false;
    connectedAddress[0] = '\0';
    needsAdvertising = true;
    logEvent("Bluetooth disconnected");
  }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    NimBLEAddress address(desc->peer_id_addr);
    String text = address.toString().c_str();

    if (!desc->sec_state.encrypted) {
      logEvent("Bluetooth pairing failed for %s", text.c_str());
      server->disconnect(desc->conn_handle);
      return;
    }

    if (!pairingName.isEmpty()) {
      preferences.putString(pairingName.c_str(), text);
      rememberName(pairingName);
      logEvent("Bluetooth paired %s with %s", pairingName.c_str(), text.c_str());
      pairingName = "";
    } else if (!isSavedAddress(text.c_str())) {
      // Not paired through the hub, so it doesn't get to stay.
      logEvent("Bluetooth rejected unknown device %s", text.c_str());
      NimBLEDevice::deleteBond(address);
      server->disconnect(desc->conn_handle);
      return;
    }

    snprintf(connectedAddress, sizeof(connectedAddress), "%s", text.c_str());
    logEvent("Bluetooth connected to %s", text.c_str());
  }
};

const Key* findKey(const char* name) {
  if (name == nullptr) return nullptr;
  for (const Key& key : KEYS) {
    if (strcmp(key.name, name) == 0) return &key;
  }

  // Letters KEY_A to KEY_Z, lowercase.
  static Key letter = {"", 0, false, 0, false};
  if (strlen(name) == 5 && strncmp(name, "KEY_", 4) == 0 && name[4] >= 'A' && name[4] <= 'Z') {
    letter.name = name;
    letter.code = 0x04 + (name[4] - 'A');
    return &letter;
  }
  return nullptr;
}
}  // namespace

void bleBegin() {
  preferences.begin("ble", false);

  NimBLEDevice::init(HUB_NAME);
  NimBLEDevice::setSecurityAuth(true, false, true);  // Bonded, no passcode, secure connections.
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  hid = new NimBLEHIDDevice(server);
  keyboardInput = hid->inputReport(REPORT_KEYBOARD);
  mediaInput = hid->inputReport(REPORT_MEDIA);
  systemInput = hid->inputReport(REPORT_SYSTEM);
  hid->manufacturer()->setValue(HUB_NAME);
  hid->pnp(0x02, 0x303A, 0x0100, 0x0100);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap(const_cast<uint8_t*>(REPORT_MAP), sizeof(REPORT_MAP));
  hid->startServices();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->hidService()->getUUID());
  advertising->setScanResponse(true);

  updateAdvertising();
}

void bleLoop() {
  if (!pairingName.isEmpty() && int32_t(millis() - pairingUntil) >= 0) {
    logEvent("Bluetooth pairing timed out for %s", pairingName.c_str());
    pairingName = "";
  }
  if (needsAdvertising) {
    needsAdvertising = false;
    updateAdvertising();
  } else {
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck >= 1000) {
      lastCheck = millis();
      updateAdvertising();
    }
  }
}

bool blePair(const String& name, String& error) {
  if (!pairingName.isEmpty()) {
    error = "already pairing " + pairingName;
    return false;
  }

  String old = savedAddress(name);
  if (!old.isEmpty()) {
    error = name + " is already paired, forget it first";
    return false;
  }

  if (connected) server->disconnect(connectionHandle);
  pairingName = name;
  pairingUntil = millis() + PAIRING_WINDOW_MS;
  logEvent("Bluetooth pairing started for %s", name.c_str());
  updateAdvertising();
  return true;
}

void bleCancelPair() {
  if (pairingName.isEmpty()) return;
  logEvent("Bluetooth pairing cancelled for %s", pairingName.c_str());
  pairingName = "";
}

bool bleForget(const String& name, String& error) {
  String address = savedAddress(name);
  if (address.isEmpty()) {
    error = name + " is not paired";
    return false;
  }

  if (connected && address == connectedAddress) server->disconnect(connectionHandle);
  NimBLEDevice::deleteBond(NimBLEAddress(address.c_str(), BLE_ADDR_PUBLIC));
  NimBLEDevice::deleteBond(NimBLEAddress(address.c_str(), BLE_ADDR_RANDOM));
  preferences.remove(name.c_str());
  logEvent("Bluetooth forgot %s", name.c_str());
  return true;
}

bool bleKeyKnown(const char* key) {
  return findKey(key) != nullptr;
}

bool bleSendKey(const String& name, const char* keyName, String& error) {
  const Key* key = findKey(keyName);
  if (key == nullptr) {
    error = String("unknown key ") + (keyName != nullptr ? keyName : "(none)");
    return false;
  }
  if (!bleIsConnected(name)) {
    error = name + " is not connected";
    return false;
  }

  if (key->system) {
    uint8_t press = 0x01;
    uint8_t release = 0x00;
    systemInput->setValue(&press, sizeof(press));
    systemInput->notify();
    delay(KEY_HOLD_MS);
    systemInput->setValue(&release, sizeof(release));
    systemInput->notify();
  } else if (key->media) {
    uint8_t press[2] = {uint8_t(key->code & 0xFF), uint8_t(key->code >> 8)};
    uint8_t release[2] = {0, 0};
    mediaInput->setValue(press, sizeof(press));
    mediaInput->notify();
    delay(KEY_HOLD_MS);
    mediaInput->setValue(release, sizeof(release));
    mediaInput->notify();
  } else {
    uint8_t press[8] = {key->modifier, 0, uint8_t(key->code), 0, 0, 0, 0, 0};
    uint8_t release[8] = {0};
    keyboardInput->setValue(press, sizeof(press));
    keyboardInput->notify();
    delay(KEY_HOLD_MS);
    keyboardInput->setValue(release, sizeof(release));
    keyboardInput->notify();
  }
  logEvent("Bluetooth key %s to %s", keyName, name.c_str());
  return true;
}

bool bleIsPaired(const String& name) {
  return !savedAddress(name).isEmpty();
}

bool bleIsConnected(const String& name) {
  String address = savedAddress(name);
  return connected && !address.isEmpty() && address == connectedAddress;
}

String blePairingName() {
  return pairingName;
}

uint32_t blePairingSecondsLeft() {
  if (pairingName.isEmpty()) return 0;
  int32_t left = int32_t(pairingUntil - millis());
  return left > 0 ? left / 1000 : 0;
}
