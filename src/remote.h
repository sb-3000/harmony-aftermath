#pragma once

#include <Arduino.h>

enum class RemoteEvent { Pressed, Held, Released };

typedef void (*RemoteCallback)(RemoteEvent event, uint32_t code);

struct RemoteStatus {
  bool radioOk = false;
  uint32_t packets = 0;
  uint32_t badChecksums = 0;
  uint32_t lastCode = 0;
  bool pairing = false;
  uint8_t pairingStep = 0;
  uint8_t pairingProgress = 0;
  String pairingState = "idle";
  String pairingMessage;
};

// Receives the Harmony remote's packets through the nRF24L01+.
void remoteBegin(RemoteCallback callback);
void remoteListen(uint64_t address, uint8_t channel);
void remoteStopListening();
bool remotePairingStart(bool replace, String& error);
void remotePairingCancel();
void remotePairingClearStatus();
void remoteLoop();
RemoteStatus remoteStatus();
