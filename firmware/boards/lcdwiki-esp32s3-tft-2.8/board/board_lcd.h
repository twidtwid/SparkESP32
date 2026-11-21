#pragma once
// Private to this board: the panel board.c brings up, for display.c.

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// The ILI9341V is 240x320 portrait in its own terms; the UI is 320x240 landscape.
#define BOARD_LCD_W  240
#define BOARD_LCD_H  320

esp_lcd_panel_handle_t    board_lcd_panel(void);
esp_lcd_panel_io_handle_t board_lcd_io(void);
void                      board_backlight(bool on);
