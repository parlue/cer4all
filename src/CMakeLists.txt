idf_component_register(SRCS "main.cpp" "ble_bridge.cpp" "usb_bridge.cpp" "bridge.cpp"
    INCLUDE_DIRS "." "../include" REQUIRES bt nvs_flash usb esp_timer)
