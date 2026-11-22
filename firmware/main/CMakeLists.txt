idf_component_register(
    SRCS "main.c" "ui.c" "spark_data.c" "settings.c" "net.c" "console_cmds.c" "link.c" "status.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        board
        board_api
        console
        esp_driver_usb_serial_jtag
        esp_driver_uart
        mbedtls
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_client
        esp_timer
        esp_app_format)
