idf_component_register(
    SRCS "main.c" "wifi_scan.c" "ble_scan.c" "thread_scan.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif bt json vfs
                  esp_driver_usb_serial_jtag openthread hal soc)
