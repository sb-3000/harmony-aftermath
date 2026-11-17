#include "activities.h"

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <vector>

#include "ble.h"
#include "config.h"
#include "hub_log.h"
#include "ir.h"
#include "tasmota.h"

namespace {
constexpr int MAX_EXTENDS_DEPTH = 8;
constexpr uint32_t LONG_PRESS_MS = 800;
constexpr uint32_t REPEAT_DELAY_MS = 500;
constexpr uint32_t REPEAT_INTERVAL_MS = 150;

struct Step {
  String device;
  String command;
  uint32_t delayMs;
  bool isPower;
};

Preferences preferences;
String current;
std::vector<Step> steps;
size_t nextStep = 0;
uint32_t waitUntil = 0;
bool waiting = false;
String lastButton;
String heldButton;
uint32_t heldSince = 0;
uint32_t lastRepeat = 0;
String pendingButton;  // Waiting to find out if this is a short or long press.
uint32_t pendingSince = 0;

void startFromButton(const char* activityId) {
  if (activityId == nullptr) return;
  String error;
  if (!activityStart(activityId, error)) logEvent("%s", error.c_str());
}

// Sets a device's power, using its real state when its only power command is a toggle.
bool devicePower(const String& name, bool on, String& error) {
  JsonObject device = gConfig["devices"][name];
  if (device.isNull()) {
    error = "unknown device " + name;
    return false;
  }

  const char* action = on ? "on" : "off";
  if (device["transport"] == "tasmota" || !device["commands"][action].isNull()) {
    return deviceCommand(name, action, error);
  }
  if (!on || device["commands"]["toggle"].isNull()) {
    error = name + " has no way to turn " + action;
    return false;
  }

  TasmotaState state = tasmotaState(device["state"]["tasmota"] | "");
  if (!state.known) {
    error = name + " state is unknown, so it wasn't toggled";
    return false;
  }
  if (state.switch1 == (device["state"]["on_value"] | "")) {
    logEvent("%s is already on", name.c_str());
    return true;
  }
  return deviceCommand(name, "toggle", error);
}

// Finds a value in an activity, or in the activities it extends.
JsonVariant inherited(const String& id, const char* key, const char* subKey = nullptr) {
  JsonObject activities = gConfig["activities"];
  String name = id;
  for (int depth = 0; depth < MAX_EXTENDS_DEPTH; depth++) {
    JsonObject activity = activities[name];
    if (activity.isNull()) break;
    if (subKey == nullptr) {
      JsonVariant value = activity[key];
      if (!value.isNull()) return value;
    } else {
      JsonVariant value = activity[key][subKey];
      if (!value.isNull()) return value;
    }
    const char* parent = activity["extends"];
    if (parent == nullptr) break;
    name = parent;
  }
  return JsonVariant();
}

void addSteps(JsonArray list) {
  for (JsonObject item : list) {
    Step step;
    step.delayMs = item["delay_ms"] | 0;
    step.device = item["device"] | "";
    const char* power = item["power"];
    step.isPower = power != nullptr;
    step.command = power != nullptr ? power : (item["command"] | "");
    steps.push_back(step);
  }
}

bool runAction(JsonObject action, String& error) {
  String device = action["device"] | "";
  const char* power = action["power"];
  if (power != nullptr) return devicePower(device, strcmp(power, "on") == 0, error);
  return deviceCommand(device, action["command"] | "", error);
}

void runButton(const String& name, JsonObject action) {
  String error;
  if (!runAction(action, error)) logEvent("Button %s failed: %s", name.c_str(), error.c_str());
}

// Wake-on-LAN: the sleeping device wakes when it sees its own MAC repeated 16 times.
bool sendWakeOnLan(const String& name, JsonObject wol, String& error) {
  if (wol.isNull()) {
    error = name + " has no wake_on_lan settings";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    error = "Wi-Fi is not connected";
    return false;
  }

  String macText = wol["mac"] | "";
  macText.replace(":", "");
  macText.replace("-", "");
  uint8_t mac[6];
  for (int i = 0; i < 6; i++) mac[i] = strtoul(macText.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);

  IPAddress target;
  target.fromString(wol["broadcast"] | "");
  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(packet + 6 + i * 6, mac, 6);

  WiFiUDP udp;
  int sent = 0;
  for (uint16_t port : {9, 7}) {
    for (int repeat = 0; repeat < 3; repeat++) {
      if (udp.beginPacket(target, port) && udp.write(packet, sizeof(packet)) == sizeof(packet) && udp.endPacket()) sent++;
    }
  }
  logEvent("Wake-on-LAN for %s to %s: %d of 6 sent", name.c_str(), target.toString().c_str(), sent);
  if (sent == 0) {
    error = "could not send the wake packet";
    return false;
  }
  return true;
}
}  // namespace

void activitiesBegin() {
  preferences.begin("hub", false);
  current = preferences.getString("activity", "");
  activitiesConfigChanged();
}

void activitiesConfigChanged() {
  steps.clear();
  nextStep = 0;
  waiting = false;

  if (!current.isEmpty() && gConfig["activities"][current].isNull()) {
    logEvent("Activity %s no longer exists", current.c_str());
    current = "";
    preferences.putString("activity", current);
  }

  std::vector<String> hosts;
  for (JsonPair pair : gConfig["devices"].as<JsonObject>()) {
    const char* host = pair.value()["host"];
    const char* stateHost = pair.value()["state"]["tasmota"];
    for (const char* candidate : {host, stateHost}) {
      if (candidate == nullptr) continue;
      bool known = false;
      for (const String& existing : hosts) known = known || existing == candidate;
      if (!known) hosts.push_back(candidate);
    }
  }
  tasmotaWatch(hosts);
}

bool activityStart(const String& id, String& error) {
  JsonObject activities = gConfig["activities"];
  if (activities[id].isNull()) {
    error = "unknown activity " + id;
    return false;
  }

  steps.clear();
  nextStep = 0;
  waiting = false;

  if (!current.isEmpty() && current != id) {
    JsonVariant stop = inherited(current, "stop");
    if (!stop.isNull()) addSteps(stop.as<JsonArray>());
  }
  JsonVariant start = inherited(id, "start");
  if (!start.isNull()) addSteps(start.as<JsonArray>());

  current = id;
  preferences.putString("activity", current);
  logEvent("Activity %s starting, %u steps", id.c_str(), steps.size());
  return true;
}

bool deviceCommand(const String& name, const String& command, String& error) {
  JsonObject device = gConfig["devices"][name];
  if (device.isNull()) {
    error = "unknown device " + name;
    return false;
  }

  if (command == "wake_by_network") return sendWakeOnLan(name, device["wake_on_lan"], error);

  String transport = device["transport"] | "";
  if (transport == "ir_nec") {
    const char* code = device["commands"][command];
    if (code == nullptr) {
      error = "unknown command " + command + " for " + name;
      return false;
    }
    uint16_t id = strtoul(device["id"] | "0", nullptr, 16);
    if (!irEnqueue(id, strtoul(code, nullptr, 16))) {
      error = "IR queue full";
      return false;
    }
    return true;
  }

  if (transport == "bluetooth") {
    const char* key = device["commands"][command];
    if (key == nullptr) {
      error = "unknown command " + command + " for " + name;
      return false;
    }
    return bleSendKey(name, key, error);
  }

  if (transport == "tasmota") {
    if (command != "on" && command != "off") {
      error = "tasmota device needs on or off";
      return false;
    }
    if (!tasmotaCommand(device["host"] | "", command == "on" ? "Power%20On" : "Power%20Off")) {
      error = "Tasmota queue full";
      return false;
    }
    return true;
  }

  error = "unknown transport for " + name;
  return false;
}

void activitiesHandleRemote(RemoteEvent event, uint32_t code) {
  if (event == RemoteEvent::Released) {
    heldButton = "";
    if (!pendingButton.isEmpty()) {
      String button = pendingButton;
      pendingButton = "";
      startFromButton(gConfig["remote"]["activity_buttons"][button]);
    }
    return;
  }

  if (event == RemoteEvent::Held) {
    if (heldButton.isEmpty() || current.isEmpty()) return;
    // The remote resends quickly even on a short tap, so repeats wait for a real hold.
    uint32_t now = millis();
    if (now - heldSince < REPEAT_DELAY_MS || now - lastRepeat < REPEAT_INTERVAL_MS) return;
    lastRepeat = now;
    JsonVariant action = inherited(current, "buttons", heldButton.c_str());
    if (!action.isNull() && (action["repeat"] | false)) runButton(heldButton, action.as<JsonObject>());
    return;
  }

  char key[7];
  snprintf(key, sizeof(key), "%06lX", code);
  const char* mapped = gConfig["remote"]["buttons"][key];
  if (mapped == nullptr) {
    lastButton = String(key) + " (not mapped)";
    logEvent("Button %s is not mapped", key);
    return;
  }

  String name = mapped;
  lastButton = String(key) + " " + name;
  heldButton = name;
  heldSince = millis();
  lastRepeat = heldSince;
  logEvent("Button %s (%s)", name.c_str(), key);

  if (!gConfig["remote"]["activity_buttons_long"][name].isNull()) {
    pendingButton = name;
    pendingSince = millis();
    return;
  }

  const char* activityId = gConfig["remote"]["activity_buttons"][name];
  if (activityId != nullptr) {
    startFromButton(activityId);
    return;
  }

  if (current.isEmpty()) return;
  JsonVariant action = inherited(current, "buttons", name.c_str());
  if (!action.isNull()) runButton(name, action.as<JsonObject>());
}

bool activitiesPressButton(const String& name, String& error) {
  lastButton = "api " + name;
  logEvent("Button %s (api)", name.c_str());

  const char* activityId = gConfig["remote"]["activity_buttons"][name];
  if (activityId != nullptr) return activityStart(activityId, error);

  if (current.isEmpty()) {
    error = "no activity is running";
    return false;
  }
  JsonVariant action = inherited(current, "buttons", name.c_str());
  if (action.isNull()) {
    error = "button " + name + " does nothing in " + current;
    return false;
  }
  return runAction(action.as<JsonObject>(), error);
}

void activitiesLoop() {
  if (!pendingButton.isEmpty() && millis() - pendingSince >= LONG_PRESS_MS) {
    String button = pendingButton;
    pendingButton = "";
    logEvent("Long press %s", button.c_str());
    startFromButton(gConfig["remote"]["activity_buttons_long"][button]);
  }

  if (waiting) {
    if (int32_t(millis() - waitUntil) < 0) return;
    waiting = false;
  }
  if (nextStep >= steps.size()) return;

  const Step step = steps[nextStep++];
  if (step.device.isEmpty()) {
    waitUntil = millis() + step.delayMs;
    waiting = true;
    return;
  }

  String error;
  bool ok = step.isPower ? devicePower(step.device, step.command == "on", error)
                         : deviceCommand(step.device, step.command, error);
  if (!ok) logEvent("Step failed: %s", error.c_str());
}

String activityCurrent() {
  return current;
}

bool activityBusy() {
  return waiting || nextStep < steps.size();
}

String lastButtonText() {
  return lastButton;
}
