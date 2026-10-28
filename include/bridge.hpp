#pragma once
#include "core.hpp"
#include <cstddef>
#include <cstdint>
namespace cer4all {
void startBridge();
void usbSession(Model model, uint8_t part);
void usbDisconnected();
bool receiveUsb(const uint8_t *, size_t);
bool receiveApp(const uint8_t *, size_t);
void appSubscribed(bool);
void wakeBleTx();
void debug(const char *fmt, ...);
size_t status(char *, size_t);
void command(const char *);
void startBle();
bool bleSubscribed();
size_t blePayload();
unsigned rxEpoch();
int notifyApp(const uint8_t *, size_t, unsigned);
void disconnectApp();
void startUsb();
bool writeUsb(const uint8_t *, size_t);
void bleBoardChanged();
bool applicationBacklog();
size_t notifyDebug(const uint8_t *, size_t);
} // namespace cer4all
