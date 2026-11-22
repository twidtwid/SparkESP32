#pragma once
// The board contract. Every board under firmware/boards/<id>/board implements this
// file and display.h, and main/ uses nothing else from a board. The board's own
// include/board_config.h supplies the constants; docs/adding-a-device.md walks
// through a port.

#include "esp_err.h"
#include "board_config.h"

// board_config.h must define:
//   BOARD_ID            the board's directory name under firmware/boards
//   BOARD_UI_W/H        the canvas the UI lays out on, in pixels, as the viewer sees it
//                       (ui.c has a layout for each size it supports)
//   BOARD_DISPLAY_MONO  1 for a black-and-white panel: the UI uses its mono theme
// and may define:
//   BOARD_UI_MIN_REFRESH_MS  for a slow screen (e-paper): new readings reach the canvas
//                       at most this often; a change of state still shows at once
#if !defined(BOARD_ID) || !defined(BOARD_UI_W) || !defined(BOARD_UI_H) || !defined(BOARD_DISPLAY_MONO)
#error "the board's board_config.h must define BOARD_ID, BOARD_UI_W, BOARD_UI_H and BOARD_DISPLAY_MONO"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Power rails, buses and the panel, ready for display_start(). Call once, first thing.
esp_err_t board_init(void);

#ifdef __cplusplus
}
#endif
