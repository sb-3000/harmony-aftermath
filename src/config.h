#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// The active configuration, stored as one JSON file on the board's flash.
extern JsonDocument gConfig;

// Mounts storage and loads the saved file. Returns false if there's no usable config.
bool configBegin();

// Checks new JSON and, if it's valid, saves and activates it. Otherwise nothing changes.
bool configReplace(const String& json, String& error);

String configText();
