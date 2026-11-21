#pragma once
// Private to this board: the panel handles board.c creates and display.c drives.

#include "esp_lcd_types.h"

#define BOARD_LCD_W 480
#define BOARD_LCD_H 480

esp_lcd_panel_handle_t    board_lcd_panel(void);
esp_lcd_panel_io_handle_t board_lcd_io(void);
