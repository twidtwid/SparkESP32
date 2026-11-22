#pragma once
// Private to this board: the 4.2" e-paper's frame, as board.c sends it.

#include <stdint.h>
#include "esp_err.h"

// 400x300, 1 bit a pixel, a set bit white, rows top to bottom, 50 bytes a row,
// the leftmost pixel in each byte's MSB.
#define EPD_W           400
#define EPD_H           300
#define EPD_ROW_BYTES   (EPD_W / 8)
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)

typedef enum {
    EPD_FULL,       // every pixel through black and white: flashes, clears ghosting
    EPD_PARTIAL,    // a short waveform that moves only the pixels that change: no flash
} epd_mode_t;

// Show `frame` (EPD_FRAME_BYTES). `prev` is what the panel shows now. Returns once
// the panel has finished: both buffers may change straight away. Not thread-safe:
// call from one task.
esp_err_t epd_show(const uint8_t *frame, const uint8_t *prev, epd_mode_t mode);

// Reset the controller, e.g. after epd_show() timed out. (Every refresh starts from
// a reset on this board anyway: the controller sleeps between refreshes.)
esp_err_t epd_reinit(void);
