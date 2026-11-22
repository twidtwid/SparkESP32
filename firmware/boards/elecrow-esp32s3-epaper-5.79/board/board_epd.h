#pragma once
// Private to this board: the 5.79" e-paper's frame, as board.c sends it.

#include <stdint.h>
#include "esp_err.h"

// 792x272, 1 bit a pixel, a set bit white, rows top to bottom, 99 bytes a row,
// the leftmost pixel in each byte's MSB. Two SSD1683 controllers share the panel
// (board.c splits each row between them).
#define EPD_W           792
#define EPD_H           272
#define EPD_ROW_BYTES   (EPD_W / 8)
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)

typedef enum {
    EPD_FULL,       // every pixel through black and white: ~2 s of flashing, clears ghosting
    EPD_PARTIAL,    // only the pixels that differ from `prev` change: ~0.5 s, no flash
} epd_mode_t;

// Show `frame` (EPD_FRAME_BYTES). `prev` is what the panel shows now (needed for
// EPD_PARTIAL; ignored for EPD_FULL). Returns once the panel has finished: both
// buffers may change straight away. Not thread-safe: call from one task.
esp_err_t epd_show(const uint8_t *frame, const uint8_t *prev, epd_mode_t mode);

// Reset and re-initialize the controllers, e.g. after epd_show() timed out.
esp_err_t epd_reinit(void);
