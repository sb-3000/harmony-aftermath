#include "tasmota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <map>

#include "hub_log.h"

namespace {
constexpr uint32_t POLL_INTERVAL_MS = 5000;
constexpr uint16_t HTTP_TIMEOUT_MS = 1500;

struct Request {
  char host[40];
  char command[40];
};

QueueHandle_t requests = nullptr;
SemaphoreHandle_t lock = nullptr;
std::vector<String> watched;
std::map<String, TasmotaState> states;

// The switch closes the connection unless the request has a Referer header.
bool fetch(const String& host, const String& command, JsonDocument& reply) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin("http://" + host + "/cm?cmnd=" + command)) return false;
  http.addHeader("Referer", "http://" + host + "/");

  int status = http.GET();
  bool ok = false;
  if (status == 200) {
    ok = !deserializeJson(reply, http.getString());
  }
  http.end();
  return ok;
}

void poll(const String& host) {
  TasmotaState state;
  JsonDocument power;
  JsonDocument sensors;
  if (fetch(host, "Power", power) && fetch(host, "Status%2010", sensors)) {
    state.known = true;
    state.power = power["POWER"] | "";
    state.switch1 = sensors["StatusSNS"]["Switch1"] | "";
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  states[host] = state;
  xSemaphoreGive(lock);
}

void task(void*) {
  uint32_t lastPoll = 0;
  for (;;) {
    Request request;
    if (xQueueReceive(requests, &request, pdMS_TO_TICKS(200)) == pdTRUE) {
      JsonDocument reply;
      bool ok = fetch(request.host, request.command, reply);
      logEvent("Tasmota %s %s: %s", request.host, request.command, ok ? "ok" : "failed");
      poll(request.host);
    }

    if (millis() - lastPoll >= POLL_INTERVAL_MS) {
      lastPoll = millis();
      xSemaphoreTake(lock, portMAX_DELAY);
      std::vector<String> hosts = watched;
      xSemaphoreGive(lock);
      for (const String& host : hosts) poll(host);
    }
  }
}
}  // namespace

void tasmotaBegin() {
  lock = xSemaphoreCreateMutex();
  requests = xQueueCreate(8, sizeof(Request));
  xTaskCreate(task, "tasmota", 8192, nullptr, 1, nullptr);
}

void tasmotaWatch(const std::vector<String>& hosts) {
  xSemaphoreTake(lock, portMAX_DELAY);
  watched = hosts;
  xSemaphoreGive(lock);
}

bool tasmotaCommand(const String& host, const String& command) {
  Request request;
  snprintf(request.host, sizeof(request.host), "%s", host.c_str());
  snprintf(request.command, sizeof(request.command), "%s", command.c_str());
  if (xQueueSend(requests, &request, 0) != pdTRUE) {
    logEvent("Tasmota queue full, dropped %s", command.c_str());
    return false;
  }
  return true;
}

TasmotaState tasmotaState(const String& host) {
  xSemaphoreTake(lock, portMAX_DELAY);
  auto found = states.find(host);
  TasmotaState state = found == states.end() ? TasmotaState() : found->second;
  xSemaphoreGive(lock);
  return state;
}
