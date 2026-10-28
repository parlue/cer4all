#include "bridge.hpp"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_cp210x.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_intr_alloc.h"
#include <memory>
#include <atomic>
#include <cstdlib>
namespace cer4all {
namespace {
SemaphoreHandle_t disconnected = nullptr, deviceLock = nullptr;
std::unique_ptr<esp_usb::CP210x> device;
std::atomic<bool> online{false};
std::atomic<unsigned> detachCount{0};
bool rx(const uint8_t *p, size_t n, void *) {
    return receiveUsb(p, n);
}
void event(const cdc_acm_host_dev_event_data_t *e, void *) {
    if (e->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ++detachCount;
        online = false;
        usbDisconnected();
        xSemaphoreGive(disconnected);
    } else if (e->type == CDC_ACM_HOST_ERROR) {
        debug("USB driver error %d", e->data.error);
        ++detachCount;
        online = false;
        usbDisconnected();
        xSemaphoreGive(disconnected);
    }
}
void host(void *) {
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}
void manager(void *) {
    for (;;) {
        xSemaphoreTake(disconnected, 0);
        const unsigned before = detachCount.load();
        cdc_acm_host_device_config_t config{};
        config.connection_timeout_ms = 1000;
        config.out_buffer_size = 64;
        config.in_buffer_size = 64;
        config.event_cb = event;
        config.data_cb = rx;
        try {
            auto next = std::unique_ptr<esp_usb::CP210x>(new esp_usb::CP210x(0xEA60, &config));
            // Silicon Labs vendor-specific GET_PARTNUM. No LED bytes sent for detection.
            uint8_t chip = 0;
            esp_err_t probe = next->send_custom_request(0xC0, 0xFF, 0x370B, 0, 1, &chip);
            if (probe != ESP_OK) {
                chip = 0;
                debug("USB GET_PARTNUM failed %s; native forwarding only", esp_err_to_name(probe));
            }
            cdc_acm_line_coding_t line{};
            line.dwDTERate = 38400;
            line.bDataBits = 8;
            esp_err_t rc = next->line_coding_set(&line);
            if (rc == ESP_OK)
                rc = next->set_control_line_state(true, true);
            if (rc != ESP_OK) {
                debug("USB setup failed %s", esp_err_to_name(rc));
                continue;
            }
            if (detachCount.load() != before)
                continue;
            xSemaphoreTake(deviceLock, portMAX_DELAY);
            device = std::move(next);
            online = true;
            usbSession(fromPart(chip), chip);
            xSemaphoreGive(deviceLock);
            xSemaphoreTake(disconnected, portMAX_DELAY);
            xSemaphoreTake(deviceLock, portMAX_DELAY);
            online = false;
            device.reset();
            xSemaphoreGive(deviceLock);
        } catch (esp_err_t err) {
            if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_NOT_FOUND)
                debug("USB open error %s", esp_err_to_name(err));
        }
    }
}
} // namespace
bool writeUsb(const uint8_t *p, size_t n) {
    if (!online)
        return false;
    xSemaphoreTake(deviceLock, portMAX_DELAY);
    bool ok = online && bool(device);
    for (size_t offset = 0; ok && offset < n;) {
        size_t length = std::min(size_t(64), n - offset);
        esp_err_t rc = device->tx_blocking(const_cast<uint8_t *>(p + offset), length, 1000);
        if (rc != ESP_OK) {
            debug("USB write error %s", esp_err_to_name(rc));
            ok = false;
        } else
            offset += length;
    }
    xSemaphoreGive(deviceLock);
    if (!ok) {
        online = false;
        usbDisconnected();
        xSemaphoreGive(disconnected);
    }
    return ok;
}
void startUsb() {
    disconnected = xSemaphoreCreateBinary();
    deviceLock = xSemaphoreCreateMutex();
    assert(disconnected && deviceLock);
    usb_host_config_t hostConfig{};
    hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&hostConfig));
    if (xTaskCreate(host, "usb_host", 4096, nullptr, 10, nullptr) != pdTRUE)
        abort();
    ESP_ERROR_CHECK(cdc_acm_host_install(nullptr));
    if (xTaskCreate(manager, "cp210x", 6144, nullptr, 6, nullptr) != pdTRUE)
        abort();
}
} // namespace cer4all
