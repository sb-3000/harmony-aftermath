#include "config.h"

#include <IPAddress.h>
#include <LittleFS.h>

#include "ble.h"
#include "hub_log.h"

JsonDocument gConfig;

namespace {
const char* CONFIG_PATH = "/config.json";
const char* TEMP_PATH = "/config.tmp";

bool isHex(const char* text, size_t length) {
  if (text == nullptr || strlen(text) != length) return false;
  for (size_t i = 0; i < length; i++) {
    if (!isxdigit(text[i])) return false;
  }
  return true;
}

bool validateCommand(JsonObject devices, JsonObject step, const String& where, String& error) {
  const char* deviceName = step["device"];
  if (deviceName == nullptr) {
    error = where + ": missing device";
    return false;
  }
  JsonObject device = devices[deviceName];
  if (device.isNull()) {
    error = where + ": unknown device " + deviceName;
    return false;
  }

  const char* command = step["command"];
  const char* power = step["power"];
  if (!step["settle_ms"].isNull()) {
    if (power == nullptr || strcmp(power, "on") != 0 || !step["settle_ms"].is<unsigned int>()) {
      error = where + ": settle_ms needs a number and \"power\": \"on\"";
      return false;
    }
  }
  String transport = device["transport"] | "";
  if (transport == "tasmota") {
    String action = power != nullptr ? power : (command != nullptr ? command : "");
    if (action != "on" && action != "off") {
      error = where + ": tasmota device needs on or off";
      return false;
    }
  } else if (power != nullptr) {
    String action = power;
    if (action != "on" && action != "off") {
      error = where + ": power must be on or off";
      return false;
    }
    JsonObject commands = device["commands"];
    bool canToggle = action == "on" && !commands["toggle"].isNull() && !device["state"].isNull();
    if (commands[power].isNull() && !canToggle) {
      error = where + ": " + deviceName + " has no way to turn " + action;
      return false;
    }
  } else if (command != nullptr && strcmp(command, "wake_by_network") == 0) {
    if (device["wake_on_lan"].isNull()) {
      error = where + ": " + deviceName + " has no wake_on_lan settings";
      return false;
    }
  } else if (command != nullptr && strcmp(command, "wake_by_bluetooth") == 0) {
    if (transport != "bluetooth") {
      error = where + ": wake_by_bluetooth needs a Bluetooth device";
      return false;
    }
  } else if (command == nullptr || device["commands"][command].isNull()) {
    error = where + ": unknown command " + (command != nullptr ? command : "(none)") + " for " + deviceName;
    return false;
  }
  return true;
}

bool validate(JsonDocument& doc, String& error) {
  JsonObject remote = doc["remote"];
  if (remote.isNull()) {
    error = "missing remote";
    return false;
  }
  const char* remoteAddress = remote["address"];
  if (remoteAddress != nullptr && remoteAddress[0] != '\0' && !isHex(remoteAddress, 10)) {
    error = "remote.address must be 10 hex digits";
    return false;
  }
  int channel = remote["channel"] | -1;
  if (channel < 0 || channel > 125) {
    error = "remote.channel must be 0-125";
    return false;
  }
  for (JsonPair button : remote["buttons"].as<JsonObject>()) {
    if (!isHex(button.key().c_str(), 6)) {
      error = String("remote.buttons: code must be 6 hex digits: ") + button.key().c_str();
      return false;
    }
  }

  JsonObject devices = doc["devices"];
  if (devices.isNull()) {
    error = "missing devices";
    return false;
  }
  for (JsonPair pair : devices) {
    String where = String("device ") + pair.key().c_str();
    JsonObject device = pair.value();

    JsonObject wol = device["wake_on_lan"];
    if (!wol.isNull()) {
      String mac = wol["mac"] | "";
      mac.replace(":", "");
      mac.replace("-", "");
      IPAddress broadcast;
      if (!isHex(mac.c_str(), 12)) {
        error = where + ": wake_on_lan.mac must be 6 hex bytes";
        return false;
      }
      if (!broadcast.fromString(wol["broadcast"] | "")) {
        error = where + ": wake_on_lan.broadcast must be an IP address";
        return false;
      }
    }

    String transport = device["transport"] | "";
    if (transport == "ir_nec") {
      if (!isHex(device["id"], 4)) {
        error = where + ": id must be 4 hex digits";
        return false;
      }
      for (JsonPair command : device["commands"].as<JsonObject>()) {
        if (!isHex(command.value(), 2)) {
          error = where + ": command " + command.key().c_str() + " must be 2 hex digits";
          return false;
        }
      }
    } else if (transport == "tasmota") {
      if (device["host"].isNull()) {
        error = where + ": missing host";
        return false;
      }
    } else if (transport == "bluetooth") {
      // The name is the key the pairing is saved under, and those keys hold at most 15 characters.
      if (strlen(pair.key().c_str()) > 15) {
        error = where + ": a Bluetooth device name can be at most 15 characters";
        return false;
      }
      for (JsonPair command : device["commands"].as<JsonObject>()) {
        if (!bleKeyKnown(command.value())) {
          error = where + ": command " + command.key().c_str() + " has an unknown key";
          return false;
        }
      }
    } else {
      error = where + ": unknown transport " + transport;
      return false;
    }
  }

  JsonObject activities = doc["activities"];
  if (activities.isNull()) {
    error = "missing activities";
    return false;
  }
  for (JsonPair pair : activities) {
    String where = String("activity ") + pair.key().c_str();
    JsonObject activity = pair.value();

    const char* parent = activity["extends"];
    if (parent != nullptr && activities[parent].isNull()) {
      error = where + ": extends unknown activity " + parent;
      return false;
    }

    for (const char* list : {"start", "stop"}) {
      int index = 0;
      for (JsonObject step : activity[list].as<JsonArray>()) {
        String stepName = where + " " + list + "[" + index++ + "]";
        if (!step["delay_ms"].isNull()) continue;
        if (!validateCommand(devices, step, stepName, error)) return false;
      }
    }

    for (JsonPair button : activity["buttons"].as<JsonObject>()) {
      String buttonName = where + " button " + button.key().c_str();
      if (!validateCommand(devices, button.value(), buttonName, error)) return false;
    }
  }

  for (const char* list : {"activity_buttons", "activity_buttons_long"}) {
    for (JsonPair button : remote[list].as<JsonObject>()) {
      const char* target = button.value();
      if (target == nullptr || activities[target].isNull()) {
        error = String("remote.") + list + ": unknown activity for " + button.key().c_str();
        return false;
      }
    }
  }
  return true;
}
}  // namespace

bool configBegin() {
  if (!LittleFS.begin(true)) {
    logEvent("Storage failed to mount");
    return false;
  }
  if (!LittleFS.exists(CONFIG_PATH)) {
    logEvent("No config saved yet");
    return false;
  }

  String json = configText();
  JsonDocument doc;
  String error;
  DeserializationError parseError = deserializeJson(doc, json);
  if (parseError) {
    logEvent("Saved config is not valid JSON: %s", parseError.c_str());
    return false;
  }
  if (!validate(doc, error)) {
    logEvent("Saved config is invalid: %s", error.c_str());
    return false;
  }
  gConfig = doc;
  logEvent("Config loaded");
  return true;
}

bool configReplace(const String& json, String& error) {
  JsonDocument doc;
  DeserializationError parseError = deserializeJson(doc, json);
  if (parseError) {
    error = String("invalid JSON: ") + parseError.c_str();
    return false;
  }
  if (!validate(doc, error)) return false;

  File file = LittleFS.open(TEMP_PATH, "w");
  if (!file || file.print(json) != json.length()) {
    if (file) file.close();
    LittleFS.remove(TEMP_PATH);
    error = "could not write to storage";
    return false;
  }
  file.close();

  LittleFS.remove(CONFIG_PATH);
  if (!LittleFS.rename(TEMP_PATH, CONFIG_PATH)) {
    error = "could not save config";
    return false;
  }

  gConfig = doc;
  logEvent("Config replaced (%u bytes)", json.length());
  return true;
}

String configText() {
  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) return "";
  String text = file.readString();
  file.close();
  return text;
}
