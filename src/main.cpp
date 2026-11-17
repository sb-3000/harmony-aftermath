// Harmony Aftermath hub firmware.
//
// Wiring (XIAO ESP32-C3 pin labels):
//   nRF24L01 radio: VCC -> 3.3V regulator, GND -> GND, CE -> D1, CSN -> D5, SCK -> D2, MOSI -> D4, MISO -> D3
//   IR LEDs: switched by SS8050 transistors, hub LEDs on D10, blaster on D7 (high = LEDs on)

#include <Arduino.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "activities.h"
#include "ble.h"
#include "config.h"
#include "hub_log.h"
#include "ir.h"
#include "remote.h"
#include "tasmota.h"
#include "web.h"
#include "wifi_setup.h"

// New firmware has to prove itself before it's kept. See checkRollback().
extern "C" bool verifyRollbackLater() {
  return true;
}

namespace {
constexpr uint32_t ROLLBACK_CONFIRM_MS = 30000;  // Connected and still running after this long.
constexpr uint32_t ROLLBACK_GIVE_UP_MS = 180000;  // Never reached Wi-Fi, so go back to the old firmware.
bool rollbackChecked = false;

void checkRollback() {
  if (rollbackChecked) return;

  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) {
    rollbackChecked = true;
    return;
  }

  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED && now >= ROLLBACK_CONFIRM_MS) {
    esp_ota_mark_app_valid_cancel_rollback();
    rollbackChecked = true;
    logEvent("New firmware confirmed");
  } else if (now >= ROLLBACK_GIVE_UP_MS) {
    logEvent("New firmware never reached Wi-Fi, rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);  // Gives USB serial time to reconnect after a reset.
  logEvent("%s %s starting, reset reason %d", HUB_NAME, HUB_VERSION, int(esp_reset_reason()));

  bool configLoaded = configBegin();
  irBegin();
  tasmotaBegin();
  remoteBegin(activitiesHandleRemote);
  activitiesBegin();

  if (configLoaded) {
    const char* address = gConfig["remote"]["address"];
    if (address != nullptr && strlen(address) == 10) {
      remoteListen(strtoull(address, nullptr, 16), gConfig["remote"]["channel"] | 5);
    }
  }

  wifiBegin();

  bleBegin();
  webBegin();
}

void loop() {
  remoteLoop();
  activitiesLoop();
  irLoop();
  bleLoop();
  webLoop();
  wifiLoop();
  checkRollback();
  delay(1);
}
