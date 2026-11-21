#pragma once
// Private to this board: the ST7306 reflective LCD's frame, as board.c sends it.

#include <stdint.h>
#include "esp_err.h"

// The panel is portrait in its own terms: 300 columns x 400 rows, 1 bit a pixel,
// a set bit white. Its RAM packs a 4-column x 2-row block into each byte, MSB
// first: bit 7 (c0,r0), bit 6 (c0,r1), bit 5 (c1,r0) ... bit 0 (c3,r1); 75 bytes
// cover a pair of rows, and pairs run top to bottom.
#define RLCD_COLS        300
#define RLCD_ROWS        400
#define RLCD_FRAME_BYTES (RLCD_COLS * RLCD_ROWS / 8)

// Send a whole frame (RLCD_FRAME_BYTES, DMA-capable memory); returns once the
// panel has it, so the caller may change the buffer straight away.
esp_err_t rlcd_send_frame(const uint8_t *frame);
