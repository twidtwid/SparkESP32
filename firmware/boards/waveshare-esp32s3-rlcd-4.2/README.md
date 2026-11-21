# Waveshare ESP32-S3-RLCD-4.2 (second board)

[Product page](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm) ·
[docs](https://docs.waveshare.com/ESP32-S3-RLCD-4.2). Added to prove the port
path in [docs/adding-a-device.md](../../../docs/adding-a-device.md); the C6 AMOLED
stays the recommended board.

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
  the Zephyr project's devicetree for this board (Apache-2.0) and agree with
  Waveshare's documentation; the code is our own (see THIRD_PARTY_NOTICES.md).
- The controller packs a 4-column × 2-row block into each byte. LVGL renders a
  400×300 RGB565 canvas in PSRAM; `display.c` thresholds each pixel to black or
  white and writes it straight into the panel's 15 KB frame, rotated, then sends
  the whole frame (about 12 ms).
- Landscape only: `rot cw` (the default) and `rot ccw` are the two landscape
  orientations; `rot none` (portrait) answers `ERR rot none not supported on this board`.
- `devctl.py shot` shows LVGL's render before the 1-bit conversion.

## Verified

On the real board, 2026-11-20 (firmware 0.1.0, ESP-IDF 5.5.6), driven from a Mac:

- flashes over USB, boots, answers the handshake (`board=waveshare-esp32s3-rlcd-4.2`)
- demo samples render; `rot ccw` / `rot cw` work and `rot none` is refused
- on Wi-Fi it joins, polls a Spark's agent, and reads `screen=online reason='via WI-FI'`
  with live values

Not yet checked by a person looking at it: that the image is upright in
`rot cw` and black-on-white rather than inverted, and a cold plug-in (the C6's
USB erratum; `main.c` restarts once at power-on on every board, which is harmless here).
