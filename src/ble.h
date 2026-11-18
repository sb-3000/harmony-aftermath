#pragma once

#include <Arduino.h>

// Makes the hub act as a Bluetooth keyboard for paired devices.
void bleBegin();
void bleLoop();

bool blePair(const String& name, String& error);
void bleCancelPair();
bool bleForget(const String& name, String& error);

bool bleKeyKnown(const char* key);
bool bleSendKey(const String& name, const char* key, String& error);

// Calls a paired device that isn't connected, which can wake it from sleep.
bool bleWake(const String& name, String& error);

bool bleIsPaired(const String& name);
bool bleIsConnected(const String& name);
String blePairingName();
uint32_t blePairingSecondsLeft();
