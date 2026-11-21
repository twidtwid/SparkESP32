# LCDWiki 2.8inch ESP32-S3 Display, ES3C28P (third board)

[LCDWiki page](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display). Sold under several
brand names; the board's SKU is ES3C28P (with touch) or ES3N28P (without). The cheap,
common case: a color SPI TFT with a backlight. The C6 AMOLED stays the recommended board.

| | |
|---|---|
| Chip | ESP32-S3 (N16R8): 16 MB flash, 8 MB octal PSRAM |
| Display | 2.8" 240×320 color IPS TFT, ILI9341V over SPI (40 MHz), LED backlight |
| USB | native USB-Serial/JTAG (303a:1001): the console and the agent's USB feed |
| Unused | FT6336G capacitive touch, ES8311 audio and speaker amp, microphone, RGB LED, microSD, battery charger |

## Pins

| Signal | GPIO |
|---|---|
| LCD SCLK / MOSI | 12 / 11 |
| LCD CS / DC | 10 / 46 |
| LCD backlight | 45 (high = on) |
| LCD reset | the ESP32-S3's EN: no GPIO |
| Touch I2C SCL / SDA, reset (unused) | 15 / 16, 18 |

## How the display works here

- `board.c` uses Espressif's `esp_lcd_ili9341` driver and passes the panel maker's
  power, VCOM and gamma values (LCDWiki's ILI9341V init code, used as facts; the
  code is our own). The panel is IPS: it needs color inversion on and BGR order.
- The LCD shares its reset with the chip's EN pin, so there is no reset GPIO: the
  driver sends a software reset. `esp_restart()` resets only the CPU, so the panel
  keeps running through the once-per-power-on restart in `main.c`.
- LVGL renders 320×240 in 40-row strips. The panel's memory-access control swaps
  rows and columns for landscape, so strips go out as they are, byte-swapped to
  big-endian RGB565. The flush waits for each strip's DMA; the SPI interrupt only
  signals, because an interrupt that lands while flash is busy (saving a setting)
  can't call code in flash.
- The backlight stays off until the first real frame is on the glass.
- Landscape only: `rot cw` (the default) and `rot ccw`; `rot none` (portrait)
  answers `ERR rot none not supported on this board`.

## Verified

On the real board, 2026-11-21 (firmware 0.1.0, ESP-IDF 5.5.6), driven from a Mac:

- flashes over USB, boots, answers the handshake (`board=lcdwiki-esp32s3-tft-2.8`)
- demo screenshots at loads 70 and 100: every element in place, nothing clipped
- `rot cw` and `rot ccw` work and persist; `rot none` is refused
- on Wi-Fi it joins, polls a Spark's agent, and reads `screen=online reason='via WI-FI'`
  with live values
- a reflash keeps the name and Wi-Fi settings
- the physical screen, looked at by a person: lit, showing the live UI

Not yet: a cold plug-in, and the installer on a Spark.
