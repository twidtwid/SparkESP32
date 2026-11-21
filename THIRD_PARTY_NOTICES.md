# Third-party notices

SparkESP32's own code is MIT-licensed ([LICENSE](LICENSE)). It builds on the projects
below. Where a project's license asks for its notice to accompany binaries, that
applies to the firmware images published under Releases.

## Vendored in this repository

| Project | Version | License | Where |
|---|---|---|---|
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) (Lewis He) | 0.2.7 | MIT | `firmware/components/xpowerslib/`, unmodified; full text in its `LICENSE` |

## Linked into the firmware

Fetched at build time by the ESP-IDF component manager (pinned per board in
`firmware/boards/<id>/dependencies.lock`) and compiled into `sparkesp32.bin`:

| Project | Version | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) (Espressif) | 5.5.6 | Apache-2.0. Its bundled components (FreeRTOS, lwIP, mbedTLS, newlib, wpa_supplicant and others) carry their own permissive licenses, listed in [ESP-IDF's copyright notice](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32c6/COPYRIGHT.html) |
| [LVGL](https://github.com/lvgl/lvgl) | 9.6.0 | MIT |
| [esp_lvgl_adapter](https://components.espressif.com/components/espressif/esp_lvgl_adapter) (Espressif) | 0.7.9 | Apache-2.0. ESP32-C6 AMOLED only |
| [esp_lcd_sh8601](https://components.espressif.com/components/espressif/esp_lcd_sh8601) (Espressif) | 2.0.1 | Apache-2.0. ESP32-C6 AMOLED only |

The ESP32-S3 reflective-LCD build uses only ESP-IDF and LVGL from this table.

On the C6 build, the adapter also pulls in, as build-time dependencies, Espressif's `esp_lv_fs`,
`esp_lv_decoder`, `esp_mmap_assets`, `button`, `knob`, `esp_lcd_touch`, `esp_new_jpeg`
and their `freetype`, `libpng`, `zlib` and `qoi` ports. SparkESP32 calls none of them,
and the linker drops them: none of their symbols are in `sparkesp32.elf`. They're listed
here for completeness. Their licenses (Apache-2.0, MIT, the FreeType License, the libpng
and zlib licenses, and the Espressif MIT License) are in each component's directory under
`firmware/managed_components/` after a build.

## Used by the host tools

| Project | Version | License | Used by |
|---|---|---|---|
| [pySerial](https://github.com/pyserial/pyserial) | 3.5 | BSD-3-Clause | agent, `tools/` |
| [esptool](https://github.com/espressif/esptool) | 4.14.0 | GPL-2.0-or-later | `install.sh` runs it as a separate program to flash; not distributed here |
| [Pillow](https://github.com/python-pillow/Pillow) | any | MIT-CMU | optional, `tools/devctl.py shot` |

## Hardware

Pin assignments and panel initialization values come from the board makers'
published material. The board-support code in `firmware/boards/` is original to this
project.

| Board | Source of the hardware facts |
|---|---|
| Waveshare ESP32-C6-Touch-AMOLED-2.16 | Waveshare's [documentation](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16) and schematic |
| Waveshare ESP32-S3-RLCD-4.2 | Waveshare's [documentation](https://docs.waveshare.com/ESP32-S3-RLCD-4.2), and the [Zephyr project's devicetree](https://github.com/zephyrproject-rtos/zephyr/tree/main/boards/waveshare/esp32s3_rlcd_4_2) for the board (Apache-2.0) for the ST7306 register values. No code was copied |
