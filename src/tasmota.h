#pragma once

#include <Arduino.h>
#include <vector>

struct TasmotaState {
  bool known = false;
  String power;
  String switch1;
};

// Talks to Tasmota devices from a background task, so the remote never waits on Wi-Fi.
void tasmotaBegin();
void tasmotaWatch(const std::vector<String>& hosts);
bool tasmotaCommand(const String& host, const String& command);
TasmotaState tasmotaState(const String& host);
