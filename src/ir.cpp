#include "ir.h"

#include <driver/rmt.h>

#include "hub_log.h"

// IR LEDs are switched by SS8050 transistors, so a high pin turns the LEDs on.
// The RMT peripheral builds the NEC timing and the 38 kHz carrier in hardware.
namespace {
struct Output {
  gpio_num_t pin;
  rmt_channel_t channel;
};

const Output OUTPUTS[] = {
    {GPIO_NUM_10, RMT_CHANNEL_0},  // D10, the hub's own LEDs
    {GPIO_NUM_20, RMT_CHANNEL_1},  // D7, the blaster
};

constexpr uint32_t CARRIER_HZ = 38000;
constexpr uint8_t CARRIER_DUTY_PERCENT = 33;
constexpr uint32_t GAP_MS = 140;  // A NEC frame takes about 70 ms, so this leaves room between frames.
constexpr int QUEUE_SIZE = 16;

// NEC timing in microseconds (the RMT clock is set to 1 MHz).
constexpr uint16_t LEADER_MARK = 9000;
constexpr uint16_t LEADER_SPACE = 4500;
constexpr uint16_t BIT_MARK = 560;
constexpr uint16_t ZERO_SPACE = 560;
constexpr uint16_t ONE_SPACE = 1690;

struct Frame {
  uint16_t id;
  uint8_t code;
};

Frame queue[QUEUE_SIZE];
int head = 0;
int count = 0;
uint32_t lastSend = 0;
bool ready = false;

rmt_item32_t item(uint16_t mark, uint16_t space) {
  rmt_item32_t value;
  value.level0 = 1;
  value.duration0 = mark;
  value.level1 = 0;
  value.duration1 = space;
  return value;
}

// Bytes go out in config order (for id "04FB": 04, FB), each least significant bit first.
void buildFrame(const Frame& frame, rmt_item32_t* items) {
  const uint8_t bytes[4] = {uint8_t(frame.id >> 8), uint8_t(frame.id & 0xFF), frame.code, uint8_t(~frame.code)};
  int index = 0;
  items[index++] = item(LEADER_MARK, LEADER_SPACE);
  for (uint8_t value : bytes) {
    for (int bit = 0; bit < 8; bit++) {
      items[index++] = item(BIT_MARK, (value >> bit) & 1 ? ONE_SPACE : ZERO_SPACE);
    }
  }
  items[index] = item(BIT_MARK, 0);  // Final mark ends the frame.
}
}  // namespace

void irBegin() {
  ready = true;
  for (const Output& output : OUTPUTS) {
    // Drive the pin low first, so the LEDs stay off while the driver starts.
    pinMode(output.pin, OUTPUT);
    digitalWrite(output.pin, LOW);

    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(output.pin, output.channel);
    config.clk_div = 80;  // 80 MHz / 80 = 1 tick per microsecond.
    config.tx_config.carrier_en = true;
    config.tx_config.carrier_freq_hz = CARRIER_HZ;
    config.tx_config.carrier_duty_percent = CARRIER_DUTY_PERCENT;
    config.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

    if (rmt_config(&config) != ESP_OK || rmt_driver_install(output.channel, 0, 0) != ESP_OK) {
      logEvent("IR output on GPIO %d failed to start", output.pin);
      ready = false;
    }
  }
}

bool irEnqueue(uint16_t id, uint8_t code) {
  if (count == QUEUE_SIZE) {
    logEvent("IR queue full, dropped %04X %02X", id, code);
    return false;
  }
  queue[(head + count) % QUEUE_SIZE] = {id, code};
  count++;
  return true;
}

void irLoop() {
  if (count == 0 || millis() - lastSend < GAP_MS) return;

  Frame frame = queue[head];
  head = (head + 1) % QUEUE_SIZE;
  count--;
  if (!ready) return;

  rmt_item32_t items[34];
  buildFrame(frame, items);
  for (const Output& output : OUTPUTS) {
    rmt_write_items(output.channel, items, 34, false);
  }
  lastSend = millis();
  logEvent("IR sent %04X %02X", frame.id, frame.code);
}
