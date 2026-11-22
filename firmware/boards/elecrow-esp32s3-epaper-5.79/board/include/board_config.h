#pragma once
// Elecrow CrowPanel ESP32 5.79" E-Paper HMI (DIS08792E): a 792x272 black-and-white
// e-paper panel driven by two cascaded SSD1683 controllers over SPI, shown landscape,
// with a hand-written driver; the C6 AMOLED is the reference (see firmware/boards/README.md).

#define BOARD_ID            "elecrow-esp32s3-epaper-5.79"
#define BOARD_UI_W          792     // wide (rot cw, the default); rot ccw stands it on its short
#define BOARD_UI_H          272     // end, 272x792, with a layout of its own in ui.c
#define BOARD_DISPLAY_MONO  1

// E-paper: new readings change the screen at most every 10 s (a change of state,
// such as ONLINE to OFFLINE, still shows at once). Each change is a partial refresh;
// display.c wipes the ghosting with a full refresh once an hour.
#define BOARD_UI_MIN_REFRESH_MS  10000
