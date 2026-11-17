#pragma once

#include <Arduino.h>

// Sends NEC codes from the hub LEDs (D10) and the blaster jack (D7), one frame at a time.
void irBegin();
void irLoop();
bool irEnqueue(uint16_t id, uint8_t code);
