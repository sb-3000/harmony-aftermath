#pragma once

#include <Arduino.h>

// Joins the Wi-Fi network saved in the chip's own storage. With none saved, or when it can't
// connect for a minute, it also opens an open setup hotspot named after the hub, whose
// captive portal leads to the /wifi page.
void wifiBegin();
void wifiLoop();

bool wifiHotspotActive();

// Network the hub will join, from its saved settings. Empty when none is saved.
String wifiSavedNetwork();

// Checks the new network details, then switches to them a moment later so the HTTP reply
// can go out first. The chip saves them, so they survive restarts and firmware updates.
bool wifiChangeNetwork(const String& ssid, const String& password, String& error);

// {"connected":..,"ssid":..,"ip":..,"rssi":..,"saved":..,"hotspot":..,"hotspot_name":..}
String wifiStatusJson();

// {"scanning":true} while a scan runs, then {"networks":[{"ssid","rssi","secure"}]}.
// refresh starts a new scan instead of returning the last results.
String wifiScanJson(bool refresh);
