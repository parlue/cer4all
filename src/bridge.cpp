#include "bridge.hpp"
#include "gateway_log.hpp"
#include "queue_probe.hpp"
#include "orientation.hpp"
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
OrientedPositionPipe upstream;
std::atomic<bool> physicalRotated{false};
std::atomic<unsigned> orientationNotice{0};
std::atomic<unsigned> usbLifetime{0};
QueueProbe bleProbe, usbProbe;
uint64_t usbRxBytes = 0, appRxBytes = 0, bleAcceptedBytes = 0, usbWrittenBytes = 0;
uint32_t bleBusy = 0, usbWriteMaxUs = 0;
uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }
void performanceWorker(void *) {
    uint64_t lastRx = 0, lastApp = 0, lastBle = 0, lastUsb = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        portENTER_CRITICAL(&mux);
        auto b = bleProbe.window(nowMs());
        auto u = usbProbe.window(nowMs());
        uint64_t rx = usbRxBytes, appIn = appRxBytes;
        uint64_t ble = bleAcceptedBytes, usb = usbWrittenBytes;
        uint32_t busy = bleBusy, writeUs = usbWriteMaxUs;
        bleBusy = usbWriteMaxUs = 0;
        portEXIT_CRITICAL(&mux);
        if (rx != lastRx || appIn != lastApp || b.pending || u.pending) {
            debug("PERF 5s U>B in=%llu accepted=%llu q=%u hi=%u sample_ms=%lu busy=%lu",
                  (unsigned long long)(rx-lastRx), (unsigned long long)(ble-lastBle),
                  unsigned(b.pending), unsigned(b.high), (unsigned long)b.waitMs,
                  (unsigned long)busy);
            debug("PERF 5s B>U in=%llu written=%llu q=%u hi=%u sample_ms=%lu write_us=%lu",
                  (unsigned long long)(appIn-lastApp), (unsigned long long)(usb-lastUsb),
                  unsigned(u.pending), unsigned(u.high), (unsigned long)u.waitMs,
                  (unsigned long)writeUs);
        }
        lastRx = rx; lastApp = appIn; lastBle = ble; lastUsb = usb;
    }
}
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
    if (ok) bleProbe.push(n, nowMs());
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
void orientationChanged(bool rotated, void *ctx) {
    flushBlock(*static_cast<EmitBlock *>(ctx));
    portENTER_CRITICAL(&mux);
    physicalRotated = rotated;
    // No pre-switch LED command or partial notification may cross the boundary.
    ++generation;
    ++receiveEpoch;
    toUsb.clear(); usbProbe.clear();
    toBle.clear(); bleProbe.clear();
    ++orientationNotice;
    portEXIT_CRITICAL(&mux);
    signal(usbTask);
    debug("ORIENTATION %s; coordinate epoch changed; two centre flashes",
          rotated ? "black at physical bottom (180 degrees)" : "white at physical bottom (normal)");
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
    const int64_t begin = esp_timer_get_time();
    bool ok = writeUsb(p, n);
    const uint32_t elapsed = uint32_t(esp_timer_get_time() - begin);
    portENTER_CRITICAL(&mux);
    usbWriteMaxUs = std::max(usbWriteMaxUs, elapsed);
    if (ok) usbWrittenBytes += n;
    portEXIT_CRITICAL(&mux);
    if (!ok) fault("USB write failed");
}
void confirmationLeds(bool on, unsigned epoch) {
    uint8_t mask[8]{}, rgb[247];
    if (on) mask[3] = mask[4] = 24;
    if (model.load() == Model::Spectrum) {
        rgbFromMask(mask, rgb);
        emitUsb(rgb, sizeof(rgb), &epoch);
    } else emitUsb(mask, sizeof(mask), &epoch);
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
            if (rc != 0) {
                portENTER_CRITICAL(&mux);
                ++bleBusy;
                portEXIT_CRITICAL(&mux);
                break; // Ownership stays in our queue until NimBLE accepts it; retry on TX events.
            }
            portENTER_CRITICAL(&mux);
            if (epoch == receiveEpoch.load()) {
                toBle.consume(n);
                bleProbe.complete(n, nowMs());
                bleAcceptedBytes += n;
            }
            portEXIT_CRITICAL(&mux);
        }
    }
}
void usbWorker(void *) {
    Command command{};
    Leds leds;
    unsigned epoch = ~0u, notice = 0, lifetime = usbLifetime.load();
    OrientationBlink blink;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, blink.active() ? pdMS_TO_TICKS(10) : portMAX_DELAY);
        if (lifetime != usbLifetime.load()) {
            lifetime = usbLifetime.load();
            blink.reset(); leds.reset(); notice = 0; epoch = ~0u;
        }
        if (!attached) { blink.reset(); continue; }
        if (faulted) {
            if (blink.active()) { blink.reset(); confirmationLeds(false, generation.load()); }
            continue;
        }
        // A BLE reconnect must not strand an ON phase on the physical board.
        // Finish the brief sequence with OFF, even if the client disconnected.
        if (blink.active() && epoch != generation.load()) {
            epoch = generation.load();
            leds.reset();
        }
        if (notice != orientationNotice.load()) {
            notice = orientationNotice.load();
            epoch = generation.load();
            leds.reset();
            blink.start(nowMs());
        }
        if (blink.active()) {
            bool on = false;
            if (blink.next(nowMs(), on)) confirmationLeds(on, epoch);
            if (blink.active()) continue;
            // Final OFF was just sent. Resume only the post-switch app FIFO.
        }
        while (attached && !faulted) {
            if (notice != orientationNotice.load()) break;
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
            leds.feed(command.data, command.n, model.load(), emitUsb, &epoch, physicalRotated.load());
            portENTER_CRITICAL(&mux);
            if (command.epoch == generation.load()) usbProbe.complete(command.n + 6, nowMs());
            portEXIT_CRITICAL(&mux);
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
    if (xTaskCreate(performanceWorker, "pipe_stats", 3072, nullptr, 2, nullptr) != pdTRUE)
        abort();
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
    physicalRotated = false;
    resetRx = true;
    faulted = false;
    portENTER_CRITICAL(&mux);
    ++usbLifetime;
    orientationNotice = 0;
    ++generation;
    ++receiveEpoch;
    toBle.clear();
    bleProbe.clear();
    portEXIT_CRITICAL(&mux);
    portENTER_CRITICAL(&mux);
    toUsb.clear();
    usbProbe.clear();
    portEXIT_CRITICAL(&mux);
    attached = true;
    signal(usbTask);
    debug("USB connected part=0x%02x model=%s", p, name(m));
    bleBoardChanged();
}
void usbDisconnected() {
    attached = false;
    resetRx = true;
    disconnectApp();
    portENTER_CRITICAL(&mux);
    ++usbLifetime;
    orientationNotice = 0;
    ++generation;
    ++receiveEpoch;
    toBle.clear();
    bleProbe.clear();
    portEXIT_CRITICAL(&mux);
    portENTER_CRITICAL(&mux);
    toUsb.clear();
    usbProbe.clear();
    portEXIT_CRITICAL(&mux);
    model = Model::Unknown;
    signal(usbTask);
    debug("USB disconnected; application session closed");
    bleBoardChanged();
}
bool receiveUsb(const uint8_t *p, size_t n) {
    gatewayLogTraffic();
    portENTER_CRITICAL(&mux);
    usbRxBytes += n;
    portEXIT_CRITICAL(&mux);
    if (resetRx.exchange(false)) {
        upstream.reset(model.load());
    }
    EmitBlock block;
    const Model before = upstream.type();
    const bool learnedBefore = upstream.learned();
    upstream.feed(p, n, stageOutput, &block, orientationChanged);
    if (upstream.overflow()) {
        fault("rotated position record exceeds bounded buffer");
        return false;
    }
    flushBlock(block);
    if (!learnedBefore && upstream.learned())
        debug("ORIENTATION reference learned: 3 complete initial positions; white initially at physical bottom");
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
    if (ok) { usbProbe.push(n + 6, nowMs()); appRxBytes += n; }
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
        bleProbe.clear();
        portEXIT_CRITICAL(&mux);
        faulted = false;
        signal(bleTask);
    } else {
        // Old client commands and partial RGB frames cannot leak into a new session.
        portENTER_CRITICAL(&mux);
        ++generation;
        ++receiveEpoch;
        toBle.clear();
        bleProbe.clear();
        portEXIT_CRITICAL(&mux);
        portENTER_CRITICAL(&mux);
        toUsb.clear();
        usbProbe.clear();
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
