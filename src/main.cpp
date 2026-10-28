#include "bridge.hpp"
#include "nvs_flash.h"
#include "esp_log.h"
extern "C" void app_main() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    cer4all::startBridge();
    cer4all::startBle();
    cer4all::startUsb();
    cer4all::debug("cer4all 0.1.0-dev ready; USB host GPIO19/20; no LED probes");
}
