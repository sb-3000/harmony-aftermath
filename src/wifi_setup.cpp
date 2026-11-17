#include "wifi_setup.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <vector>

#include "hub_log.h"

namespace {
const char* HOSTNAME = "harmony-aftermath";
constexpr uint32_t HOTSPOT_AFTER_MS = 60000;    // Offline this long before the hotspot opens.
constexpr uint32_t RETRY_EVERY_MS = 60000;      // How often to retry the saved network meanwhile.
constexpr uint32_t HOTSPOT_LINGER_MS = 120000;  // Close it this long after connecting, even with a phone on it.
constexpr uint32_t SWITCH_DELAY_MS = 1000;

DNSServer dns;
bool hotspot = false;
wl_status_t lastStatus = WL_NO_SHIELD;
uint32_t offlineSince = 0;
uint32_t connectedSince = 0;
uint32_t lastAttempt = 0;

bool switchPending = false;
uint32_t switchRequested = 0;
String pendingSsid;
String pendingPassword;

String scanCache;

void startHotspot() {
  // Retries are paced by wifiLoop, because a station that keeps searching makes the hotspot unusable.
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(HUB_NAME);
  dns.start(53, "*", WiFi.softAPIP());
  hotspot = true;
  logEvent("Setup hotspot \"%s\" open at %s", HUB_NAME, WiFi.softAPIP().toString().c_str());
}

void stopHotspot() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(true);
  hotspot = false;
  logEvent("Setup hotspot closed");
}

void logStatusChange(wl_status_t status) {
  if (status == lastStatus) return;
  lastStatus = status;

  if (status == WL_CONNECTED) {
    logEvent("Wi-Fi connected to %s: %s, %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
             WiFi.RSSI());
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  } else {
    logEvent("Wi-Fi status %d", status);
  }
}
}  // namespace

void wifiBegin() {
  WiFi.setHostname(HOSTNAME);  // Ignored if it's set after WiFi.mode().
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // Required when Wi-Fi and Bluetooth share the radio.
  WiFi.setAutoReconnect(true);

  String saved = wifiSavedNetwork();
  if (saved.isEmpty()) {
    logEvent("No Wi-Fi network saved");
    startHotspot();
    return;
  }
  logEvent("Joining Wi-Fi %s", saved.c_str());
  WiFi.begin();
  lastAttempt = millis();
}

void wifiLoop() {
  uint32_t now = millis();

  if (switchPending && now - switchRequested >= SWITCH_DELAY_MS) {
    switchPending = false;
    logEvent("Joining Wi-Fi %s", pendingSsid.c_str());
    WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());  // Also saves them.
    pendingPassword = "";
    lastAttempt = now;
    offlineSince = now;
  }

  wl_status_t status = WiFi.status();
  bool connected = status == WL_CONNECTED;
  if (connected && lastStatus != WL_CONNECTED) connectedSince = now;
  logStatusChange(status);
  if (connected) offlineSince = now;

  if (!hotspot) {
    if (!connected && now - offlineSince >= HOTSPOT_AFTER_MS) startHotspot();
    return;
  }

  dns.processNextRequest();
  bool phoneOnHotspot = WiFi.softAPgetStationNum() > 0;
  if (connected) {
    if (!phoneOnHotspot || now - connectedSince >= HOTSPOT_LINGER_MS) stopHotspot();
  } else if (!phoneOnHotspot && !switchPending && now - lastAttempt >= RETRY_EVERY_MS &&
             !wifiSavedNetwork().isEmpty()) {
    lastAttempt = now;
    WiFi.begin();
  }
}

bool wifiHotspotActive() {
  return hotspot;
}

String wifiSavedNetwork() {
  wifi_config_t config;
  if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) return "";
  char ssid[33] = {};
  memcpy(ssid, config.sta.ssid, 32);
  return ssid;
}

bool wifiChangeNetwork(const String& ssid, const String& password, String& error) {
  if (ssid.isEmpty() || ssid.length() > 32) {
    error = "Network name must be 1 to 32 characters";
    return false;
  }
  if (!password.isEmpty() && (password.length() < 8 || password.length() > 64)) {
    error = "Password must be 8 to 64 characters, or empty for an open network";
    return false;
  }
  pendingSsid = ssid;
  pendingPassword = password;
  switchPending = true;
  switchRequested = millis();
  return true;
}

String wifiStatusJson() {
  JsonDocument doc;
  bool connected = WiFi.status() == WL_CONNECTED;
  doc["connected"] = connected;
  if (connected) {
    doc["ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
  }
  doc["saved"] = switchPending ? pendingSsid : wifiSavedNetwork();
  doc["hotspot"] = hotspot;
  doc["hotspot_name"] = HUB_NAME;
  String body;
  serializeJson(doc, body);
  return body;
}

String wifiScanJson(bool refresh) {
  int16_t count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) return "{\"scanning\":true}";

  if (count >= 0) {
    struct Network {
      String ssid;
      int32_t rssi;
      bool secure;
    };
    std::vector<Network> networks;
    for (int16_t i = 0; i < count; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) continue;  // Hidden networks can still be typed in.
      auto found = std::find_if(networks.begin(), networks.end(),
                                [&ssid](const Network& n) { return n.ssid == ssid; });
      if (found == networks.end()) {
        networks.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
      } else if (WiFi.RSSI(i) > found->rssi) {
        found->rssi = WiFi.RSSI(i);
      }
    }
    WiFi.scanDelete();
    std::sort(networks.begin(), networks.end(), [](const Network& a, const Network& b) { return a.rssi > b.rssi; });

    JsonDocument doc;
    JsonArray list = doc["networks"].to<JsonArray>();
    for (const Network& network : networks) {
      JsonObject item = list.add<JsonObject>();
      item["ssid"] = network.ssid;
      item["rssi"] = network.rssi;
      item["secure"] = network.secure;
    }
    scanCache = "";
    serializeJson(doc, scanCache);
    return scanCache;
  }

  if (refresh || scanCache.isEmpty()) {
    WiFi.scanNetworks(true);
    return "{\"scanning\":true}";
  }
  return scanCache;
}
