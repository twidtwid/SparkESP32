# Waveshare ESP32-C6-Touch-AMOLED-2.16 (reference board)

[Product page](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm). The board
SparkESP32 was built on and is verified against end to end; the default build.

| | |
|---|---|
| Chip | ESP32-C6, 16 MB flash, no PSRAM |
| Display | 2.16" 480×480 AMOLED, SH8601-family controller on QSPI (40 MHz) |
| Power | AXP2101 PMIC on I2C (`pmic.cpp`, via the vendored XPowersLib) |
| USB | native USB-Serial/JTAG (303a:1001): the console and the agent's USB feed |
| Unused | CST9217 touch, IMU, the rest of the PMIC's features |

## Pins

| Signal | GPIO |
|---|---|
| I2C SCL / SDA (PMIC) | 7 / 8 |
| LCD SCLK | 0 |
| LCD D0–D3 | 1, 2, 3, 4 |
| LCD CS | 15 (the vendor header says 5; 5 is the touch IRQ) |

## Quirks

- The SH8601 can't swap rows and columns, and the LVGL adapter doesn't rotate SPI
  panels without PSRAM. `display.c` rotates each rendered strip in software
  into a DMA buffer (all three rotations work: the panel is square).
- esp_lvgl_adapter 0.7.x compacts strips using the draw buffer's stride;
  `display.c` reports the rotated strip's stride for the hand-off and restores it.
- The panel needs window starts on even and ends on odd coordinates
  (`round_area_cb`).
- ESP32-C6 USB erratum ([espressif/esp-idf#17432](https://github.com/espressif/esp-idf/issues/17432)):
  a cold power-on can leave USB dead until a reset. `main.c` restarts the CPU once
  at power-on; verified on hardware.

## Verified

Everything: flashing over USB from the Spark, the USB and Wi-Fi feeds, cold
plug-in, all three rotations, and the screen states (2026-11-20, firmware 0.1.0,
ESP-IDF 5.5.6).
