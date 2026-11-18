#pragma once

#include <Arduino.h>

#define HUB_NAME "Harmony Aftermath"
#define HUB_VERSION "0.15.2"

// Records an event in the recent-events list and on USB serial.
void logEvent(const char* format, ...);

// Returns recent events, oldest first.
String logDump();
