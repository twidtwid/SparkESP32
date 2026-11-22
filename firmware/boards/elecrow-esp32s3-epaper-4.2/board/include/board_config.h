#pragma once
// Elecrow CrowPanel ESP32 4.2" E-Paper HMI, V1.2 (a green circular sticker on the back):
// a 400x300 black-and-white e-paper panel with an UltraChip-style controller over SPI,
// shown landscape, with a hand-written driver; the C6 AMOLED is the reference (see
// firmware/boards/README.md).

#define BOARD_ID            "elecrow-esp32s3-epaper-4.2"
#define BOARD_UI_W          400
#define BOARD_UI_H          300
#define BOARD_DISPLAY_MONO  1

// E-paper: new readings change the screen at most every 10 s (a change of state,
// such as ONLINE to OFFLINE, still shows at once). Each change is a partial refresh;
// display.c wipes the ghosting with a full refresh once an hour.
#define BOARD_UI_MIN_REFRESH_MS  10000
