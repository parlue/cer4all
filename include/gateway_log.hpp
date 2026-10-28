#pragma once
#include <cstddef>
namespace cer4all {
void startGatewayLog();
void gatewayLogReady(bool ready);
void appendGatewayLog(const char *text, size_t length);
void gatewayLogTraffic();
bool applicationConnected();
}
