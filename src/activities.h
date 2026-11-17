#pragma once

#include <Arduino.h>

#include "remote.h"

// Runs activities, button maps and device commands from the active config.
void activitiesBegin();
void activitiesLoop();

// Call after the config changes.
void activitiesConfigChanged();

bool activityStart(const String& id, String& error);
bool deviceCommand(const String& device, const String& command, String& error);
void activitiesHandleRemote(RemoteEvent event, uint32_t code);

// Presses a button by name, as if it came from the remote (short press only).
bool activitiesPressButton(const String& name, String& error);

String activityCurrent();
bool activityBusy();
String lastButtonText();
