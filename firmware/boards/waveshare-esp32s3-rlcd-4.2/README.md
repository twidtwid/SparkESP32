# Waveshare ESP32-S3-RLCD-4.2 (not yet run on the real board)

[Product page](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm) ·
[docs](https://docs.waveshare.com/ESP32-S3-RLCD-4.2). Written from Waveshare's and
Zephyr's board files, and builds clean, but it has **not been run on a real
RLCD-4.2 yet** (see Verified, below). The C6 AMOLED stays the recommended board.

| | |
|---|---|
| Chip | ESP32-S3 (S3R8): 16 MB flash, 8 MB octal PSRAM |
| Display | 4.2" 300×400 reflective LCD, black and white, ST7306 over SPI (10 MHz), no backlight |
| USB | native USB-Serial/JTAG (303a:1001): the console and the agent's USB feed |
| Unused | PCF85063 RTC, SHTC3 sensor, audio, microSD, the 18650 holder (the PWR key and charger work in hardware) |

## Pins

| Signal | GPIO |
|---|---|
| LCD SCLK / MOSI | 11 / 12 |
| LCD DC / CS / RST | 5 / 40 / 41 |
| KEY / BOOT buttons (unused) | 18 / 0 |

## How the display works here

- No esp_lcd panel driver fits the ST7306, so `board.c` sends its power-up
  sequence directly over an esp_lcd SPI IO handle. The register values come from
  Waveshare's ESP-IDF examples for this board and the Zephyr project's devicetree
  (both Apache-2.0); the code is our own (see THIRD_PARTY_NOTICES.md).
- Waveshare's *factory* firmware programs four of those registers differently
  (`C1` and `C4` source voltages `0x41`, oscillator `D8 A6 E9`, frame rate `B2 05`)
  from its examples and Zephyr (`0x69`, `0x4B`, `80 E9`, `02`), which this driver
  uses. If the panel shows no contrast on a real board, try the factory values first.
- The controller packs a 4-column × 2-row block into each byte. LVGL renders a
  400×300 RGB565 canvas in PSRAM; `display.c` thresholds each pixel to black or
  white and writes it straight into the panel's 15 KB frame, rotated, then sends
  the whole frame (about 12 ms).
- Landscape only: `rot cw` (the default) and `rot ccw` are the two landscape
  orientations; `rot none` (portrait) answers `ERR rot none not supported on this board`.
- `devctl.py shot` shows LVGL's render before the 1-bit conversion.

## Verified

Not on hardware. The board first tested as "the RLCD" turned out to be a different
board (an ES3C28P, now [its own port](../lcdwiki-esp32s3-tft-2.8/)): its handshake,
screenshots and Wi-Fi checks passed with no RLCD attached, because none of them
looks at the glass. What's verified: the firmware builds clean for this board and
`tools/check_boards.py` passes. Before calling it supported, run the whole Verify
table in [docs/adding-a-device.md](../../../docs/adding-a-device.md#8-verify) on a
real RLCD-4.2, photo of the screen included.
