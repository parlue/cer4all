#include "bridge.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <atomic>
#include <cstdarg>
#include <cstdlib>

namespace cer4all {
namespace {
Ring<32768> toBle;
Ring<8192> logs;
struct Command {
    unsigned epoch;
    size_t n;
    uint8_t data[512];
};
MessageRing<16384> toUsb;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t bleTask = nullptr, usbTask = nullptr;
std::atomic<Model> physical{Model::Unknown}, model{Model::Unknown};
std::atomic<bool> attached{false}, faulted{false}, resetRx{true}, filterOn{true};
std::atomic<unsigned> receiveEpoch{0}, generation{0}, ledInput{unsigned(LedInput::Auto)};
std::atomic<uint8_t> part{0};
std::atomic<uint32_t> usbBytes{0}, bleBytes{0}, appBytes{0}, sentBytes{0}, overflows{0}, frames{0},
    noise{0};
PositionPipe upstream;
void signal(TaskHandle_t t) {
    if (t)
        xTaskNotifyGive(t);
}
void fault(const char *why) {
    if (!faulted.exchange(true)) {
        ++overflows;
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
    else
        sentBytes += n;
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
            bleBytes += n;
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
            leds.input = LedInput(ledInput.load());
            const auto previousErrors = leds.invalid;
            leds.feed(command.data, command.n, model.load(), emitUsb, &epoch);
            if (leds.invalid != previousErrors)
                debug("Invalid LED frames %lu", (unsigned long)leds.invalid);
        }
    }
}
void logWorker(void *) {
    uint8_t bytes[128];
    for (;;) {
        // Debug is deliberately lower priority and never waits in the application path.
        vTaskDelay(pdMS_TO_TICKS(50));
        portENTER_CRITICAL(&mux);
        size_t n = logs.peek(bytes, sizeof(bytes));
        portEXIT_CRITICAL(&mux);
        if (n) {
            size_t sent = notifyDebug(bytes, n);
            portENTER_CRITICAL(&mux);
            logs.consume(sent);
            portEXIT_CRITICAL(&mux);
        }
    }
}
} // namespace
void startBridge() {
    if (xTaskCreate(bleWorker, "ble_pipe", 4096, nullptr, 8, &bleTask) != pdTRUE)
        abort();
    if (xTaskCreate(usbWorker, "usb_pipe", 4096, nullptr, 8, &usbTask) != pdTRUE)
        abort();
    if (xTaskCreate(logWorker, "debug_pipe", 3072, nullptr, 2, nullptr) != pdTRUE)
        abort();
}
void debug(const char *fmt, ...) {
    char line[224];
    const size_t prefix =
        size_t(snprintf(line, sizeof(line), "[%llu ms] ",
                        static_cast<unsigned long long>(esp_timer_get_time() / 1000)));
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line + prefix, sizeof(line) - prefix - 2, fmt, args);
    va_end(args);
    size_t len = prefix + std::min(size_t(n < 0 ? 0 : n), sizeof(line) - prefix - 2);
    line[len++] = '\n';
    line[len] = 0;
    printf("%s", line);
    portENTER_CRITICAL(&mux);
    if (logs.free() < len)
        logs.consume(len - logs.free());
    logs.push(reinterpret_cast<uint8_t *>(line), len);
    portEXIT_CRITICAL(&mux);
}
void usbSession(Model m, uint8_t p) {
    physical = m;
    model = m;
    part = p;
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
    if (resetRx.exchange(false)) {
        upstream.reset(model.load());
        frames = 0;
    }
    usbBytes += n;
    EmitBlock block;
    const Model before = upstream.type();
    upstream.feed(p, n, filterOn.load(), stageOutput, &block);
    flushBlock(block);
    if (upstream.type() != before) {
        model = upstream.type();
        debug("8-value protocol: Sentio (no invented IDs)");
    }
    frames = upstream.frames();
    noise = upstream.filtered();
    return true;
}
bool receiveApp(const uint8_t *p, size_t n) {
    if (!attached || faulted)
        return false;
    portENTER_CRITICAL(&mux);
    bool ok = toUsb.push(p, n, generation.load());
    portEXIT_CRITICAL(&mux);
    if (!ok) {
        fault("USB queue full");
        return false;
    }
    appBytes += n;
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
bool applicationBacklog() {
    portENTER_CRITICAL(&mux);
    bool pending = toBle.size() != 0;
    portEXIT_CRITICAL(&mux);
    return pending;
}
size_t status(char *p, size_t n) {
    return size_t(snprintf(
        p, n,
        "{\"firmware\":\"0.1.0-dev\",\"board\":\"%s\",\"usb_part\":%u,\"usb\":%s,\"mtu_payload\":%"
        "u,\"usb_rx\":%lu,\"ble_tx\":%lu,\"ble_rx\":%lu,\"usb_tx\":%lu,\"frames320\":%lu,"
        "\"filtered\":%lu,\"faults\":%lu,\"filter\":%s,\"led_input\":%u}",
        name(model.load()), unsigned(part.load()), attached ? "true" : "false",
        unsigned(blePayload()), (unsigned long)usbBytes.load(), (unsigned long)bleBytes.load(),
        (unsigned long)appBytes.load(), (unsigned long)sentBytes.load(),
        (unsigned long)frames.load(), (unsigned long)noise.load(), (unsigned long)overflows.load(),
        filterOn ? "true" : "false", ledInput.load()));
}
void command(const char *p) {
    if (strcmp(p, "STATUS") == 0) {
        char text[512];
        status(text, sizeof(text));
        debug("%s", text);
    } else if (strcmp(p, "FILTER 0") == 0 || strcmp(p, "FILTER 1") == 0) {
        filterOn = p[7] == '1';
        resetRx = true;
        debug("Filter %s", filterOn ? "on" : "off");
    } else if (strcmp(p, "LEDS AUTO") == 0)
        ledInput = unsigned(LedInput::Auto);
    else if (strcmp(p, "LEDS MONO") == 0)
        ledInput = unsigned(LedInput::Mono);
    else if (strcmp(p, "LEDS RGB") == 0)
        ledInput = unsigned(LedInput::Rgb);
    else
        debug("Unknown debug command");
}
} // namespace cer4all
