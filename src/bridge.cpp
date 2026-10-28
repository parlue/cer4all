#include "bridge.hpp"
#include "gateway_log.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <atomic>
#include <cstdarg>
#include <cstdlib>

namespace cer4all {
namespace {
Ring<32768> toBle;
struct Command {
    unsigned epoch;
    size_t n;
    uint8_t data[512];
};
MessageRing<16384> toUsb;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t bleTask = nullptr, usbTask = nullptr;
std::atomic<Model> model{Model::Unknown};
std::atomic<bool> attached{false}, faulted{false}, resetRx{true};
std::atomic<unsigned> receiveEpoch{0}, generation{0};
PositionPipe upstream;
void signal(TaskHandle_t t) {
    if (t)
        xTaskNotifyGive(t);
}
void fault(const char *why) {
    if (!faulted.exchange(true)) {
        debug("FAULT %s; disconnect instead of forwarding an incomplete stream", why);
        disconnectApp();
    }
}
void queueToBle(const uint8_t *p, size_t n, void *) {
    if (!bleSubscribed() || faulted)
        return;
    portENTER_CRITICAL(&mux);
    bool ok = toBle.push(p, n);
    portEXIT_CRITICAL(&mux);
    if (!ok)
        fault("BLE queue full");
    else
        signal(bleTask);
}
struct EmitBlock {
    uint8_t bytes[64];
    size_t used = 0;
};
void flushBlock(EmitBlock &block) {
    if (block.used) {
        queueToBle(block.bytes, block.used, nullptr);
        block.used = 0;
    }
}
void stageOutput(const uint8_t *p, size_t n, void *ctx) {
    auto &block = *static_cast<EmitBlock *>(ctx);
    while (n) {
        size_t take = std::min(n, sizeof(block.bytes) - block.used);
        std::memcpy(block.bytes + block.used, p, take);
        block.used += take;
        p += take;
        n -= take;
        if (block.used == sizeof(block.bytes))
            flushBlock(block);
    }
}
void emitUsb(const uint8_t *p, size_t n, void *ctx) {
    if (*static_cast<unsigned *>(ctx) != generation.load())
        return;
    if (!writeUsb(p, n))
        fault("USB write failed");
}
void bleWorker(void *) {
    uint8_t bytes[253];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (bleSubscribed() && !faulted) {
            size_t max = std::min(sizeof(bytes), blePayload());
            portENTER_CRITICAL(&mux);
            size_t n = toBle.peek(bytes, max);
            unsigned epoch = receiveEpoch.load();
            portEXIT_CRITICAL(&mux);
            if (!n)
                break;
            int rc = notifyApp(bytes, n, epoch);
            if (rc != 0)
                break; // Ownership stays in our queue until NimBLE accepts it; retry on TX events.
            portENTER_CRITICAL(&mux);
            if (epoch == receiveEpoch.load())
                toBle.consume(n);
            portEXIT_CRITICAL(&mux);
        }
    }
}
void usbWorker(void *) {
    Command command{};
    Leds leds;
    unsigned epoch = ~0u;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (attached && !faulted) {
            uint32_t packetEpoch = 0;
            portENTER_CRITICAL(&mux);
            bool got = toUsb.pop(command.data, sizeof(command.data), command.n, packetEpoch);
            portEXIT_CRITICAL(&mux);
            if (!got)
                break;
            command.epoch = packetEpoch;
            if (command.epoch != generation.load())
                continue;
            if (command.epoch != epoch) {
                leds.reset();
                epoch = command.epoch;
            }
            const auto previousErrors = leds.invalid;
            leds.feed(command.data, command.n, model.load(), emitUsb, &epoch);
            if (leds.invalid != previousErrors)
                debug("Invalid LED frames %lu", (unsigned long)leds.invalid);
        }
    }
}
} // namespace
void startBridge() {
    if (xTaskCreate(bleWorker, "ble_pipe", 4096, nullptr, 8, &bleTask) != pdTRUE)
        abort();
    if (xTaskCreate(usbWorker, "usb_pipe", 4096, nullptr, 8, &usbTask) != pdTRUE)
        abort();
    startGatewayLog();
}
void debug(const char *fmt, ...) {
    char line[224];
    const size_t prefix =
        size_t(snprintf(line, sizeof(line), "%llu ",
                        static_cast<unsigned long long>(esp_timer_get_time() / 1000)));
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line + prefix, sizeof(line) - prefix - 2, fmt, args);
    va_end(args);
    size_t len = prefix + std::min(size_t(n < 0 ? 0 : n), sizeof(line) - prefix - 2);
    line[len++] = '\n';
    line[len] = 0;
    printf("%s", line);
    appendGatewayLog(line, len);
}
void usbSession(Model m, uint8_t p) {
    model = m;
    resetRx = true;
    faulted = false;
    portENTER_CRITICAL(&mux);
    ++generation;
    ++receiveEpoch;
    toBle.clear();
    portEXIT_CRITICAL(&mux);
    portENTER_CRITICAL(&mux);
    toUsb.clear();
    portEXIT_CRITICAL(&mux);
    attached = true;
    debug("USB connected part=0x%02x model=%s", p, name(m));
    bleBoardChanged();
}
void usbDisconnected() {
    attached = false;
    resetRx = true;
    disconnectApp();
    portENTER_CRITICAL(&mux);
    ++generation;
    ++receiveEpoch;
    toBle.clear();
    portEXIT_CRITICAL(&mux);
    portENTER_CRITICAL(&mux);
    toUsb.clear();
    portEXIT_CRITICAL(&mux);
    model = Model::Unknown;
    debug("USB disconnected; application session closed");
    bleBoardChanged();
}
bool receiveUsb(const uint8_t *p, size_t n) {
    gatewayLogTraffic();
    if (resetRx.exchange(false)) {
        upstream.reset(model.load());
    }
    EmitBlock block;
    const Model before = upstream.type();
    upstream.feed(p, n, stageOutput, &block);
    flushBlock(block);
    if (upstream.type() != before) {
        model = upstream.type();
        debug("8-value protocol: Sentio (no invented IDs)");
    }
    return true;
}
bool receiveApp(const uint8_t *p, size_t n) {
    gatewayLogTraffic();
    if (!attached || faulted)
        return false;
    portENTER_CRITICAL(&mux);
    bool ok = toUsb.push(p, n, generation.load());
    portEXIT_CRITICAL(&mux);
    if (!ok) {
        fault("USB queue full");
        return false;
    }
    signal(usbTask);
    return true;
}
void appSubscribed(bool on) {
    if (on) {
        // Do not discard LED writes submitted before CCCD registration completed.
        portENTER_CRITICAL(&mux);
        ++receiveEpoch;
        toBle.clear();
        portEXIT_CRITICAL(&mux);
        faulted = false;
        signal(bleTask);
    } else {
        // Old client commands and partial RGB frames cannot leak into a new session.
        portENTER_CRITICAL(&mux);
        ++generation;
        ++receiveEpoch;
        toBle.clear();
        portEXIT_CRITICAL(&mux);
        portENTER_CRITICAL(&mux);
        toUsb.clear();
        portEXIT_CRITICAL(&mux);
    }
    debug("Application notifications %s", on ? "on" : "off");
}
unsigned rxEpoch() {
    return receiveEpoch.load();
}
void wakeBleTx() {
    signal(bleTask);
}
} // namespace cer4all
