#include "hub_log.h"

#include <stdarg.h>

namespace {
constexpr int MAX_LINES = 60;
constexpr int LINE_LENGTH = 110;

char lines[MAX_LINES][LINE_LENGTH];
int nextLine = 0;
int lineCount = 0;
SemaphoreHandle_t lock = nullptr;
}  // namespace

void logEvent(const char* format, ...) {
  if (lock == nullptr) lock = xSemaphoreCreateMutex();

  char text[LINE_LENGTH - 16];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);

  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(lines[nextLine], LINE_LENGTH, "%10lu %s", millis(), text);
  Serial.println(lines[nextLine]);
  nextLine = (nextLine + 1) % MAX_LINES;
  if (lineCount < MAX_LINES) lineCount++;
  xSemaphoreGive(lock);
}

String logDump() {
  if (lock == nullptr) lock = xSemaphoreCreateMutex();

  String out;
  xSemaphoreTake(lock, portMAX_DELAY);
  int start = (nextLine - lineCount + MAX_LINES) % MAX_LINES;
  for (int i = 0; i < lineCount; i++) {
    out += lines[(start + i) % MAX_LINES];
    out += '\n';
  }
  xSemaphoreGive(lock);
  return out;
}
