idf_component_register(
    SRCS "board.c" "pmic.cpp" "display.c"
    INCLUDE_DIRS "include"
    REQUIRES board_api
    PRIV_REQUIRES
        esp_lcd
        esp_driver_i2c
        esp_driver_spi
        xpowerslib
        espressif__esp_lcd_sh8601
        espressif__esp_lvgl_adapter
        lvgl__lvgl)
