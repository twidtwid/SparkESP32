# Boards

Every directory here is one board SparkESP32 builds for. The rest of the
firmware (`firmware/main/`) is shared and knows a board only through the
contract in [`firmware/components/board_api`](../components/board_api/include).

| Board | Chip | Display | Status |
|---|---|---|---|
| `waveshare-esp32c6-amoled-2.16` | ESP32-C6 | 2.16" 480×480 AMOLED, color | **Reference board.** Recommended, the default build, and every screenshot in the docs |
| `lcdwiki-esp32s3-tft-2.8` | ESP32-S3 | 2.8" 240×320 color IPS TFT (ILI9341V), shown landscape | Verified on hardware. The common SPI-TFT case, and the worked example in [docs/adding-a-device.md](../../docs/adding-a-device.md) |
| `waveshare-esp32s3-rlcd-4.2` | ESP32-S3 | 4.2" 300×400 reflective LCD, black and white, shown landscape | **Not yet run on the real board.** Written from Waveshare's and Zephyr's board files |

```sh
make boards                                   # list them
make build BOARD=waveshare-esp32s3-rlcd-4.2   # build one (the C6 is the default)
make flash BOARD=<id> PORT=/dev/ttyACM0
make dist                                     # every board's release bundle
```

A board directory holds:

| Path | What |
|---|---|
| `board/` | the ESP-IDF component that implements the contract: `board_init()` and the `display_*` functions |
| `board/include/board_config.h` | `BOARD_ID`, the UI canvas size, and whether the panel is black and white |
| `sdkconfig.defaults` | the chip, flash, partition table, console and PSRAM settings for this board |
| `partitions.csv` | its partition table |
| `dependencies.lock` | the component versions it builds with (written by the first build) |
| `README.md` | hardware notes: pins, sources, quirks, and what has been verified on the real thing |

`make check` runs `tools/check_boards.py`, which checks all of the above.
