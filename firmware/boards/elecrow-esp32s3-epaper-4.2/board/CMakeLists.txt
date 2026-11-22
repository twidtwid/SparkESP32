idf_component_register(
    SRCS "board.c" "display.c"
    INCLUDE_DIRS "include"
    REQUIRES board_api
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_lcd
        esp_timer
        lvgl__lvgl)
