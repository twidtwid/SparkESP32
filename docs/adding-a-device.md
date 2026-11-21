# Adding a device

SparkESP32 builds for more than one ESP32 board. Each board is a directory under
[`firmware/boards/`](../firmware/boards). The rest of the firmware is shared and
reaches the hardware only through a small contract. Porting to a new board means
writing that contract for your hardware, plus a UI layout if your screen is a new
size.

The [Waveshare ESP32-C6-Touch-AMOLED-2.16](../firmware/boards/waveshare-esp32c6-amoled-2.16)
is the reference board. It's the one to buy and the one every screenshot shows. Two
more boards show the two usual ways to drive a panel:

| Board | Shows | Status |
|---|---|---|
| [LCDWiki ES3C28P](../firmware/boards/lcdwiki-esp32s3-tft-2.8) | The common case: a color SPI TFT (ILI9341V) with a backlight, driven by Espressif's `esp_lcd_ili9341` component. Start here. | Verified on hardware |
| [Waveshare ESP32-S3-RLCD-4.2](../firmware/boards/waveshare-esp32s3-rlcd-4.2) | A panel with no `esp_lcd` driver: a hand-written ST7306 driver for a 1-bit reflective LCD, and a mono theme. | Verified on hardware |

AI agents: [AGENTS.md](../AGENTS.md#adding-a-new-device) has the same steps as a checklist.
To submit a board, see [CONTRIBUTING.md](../CONTRIBUTING.md#adding-a-new-device).

## Will your board work?

| Need | Why |
|---|---|
| An ESP32 with **Wi-Fi** and a **native USB-Serial/JTAG** controller: ESP32-C3, C5, C6 or S3 | The agent finds the display by that controller's USB ID, `303a:1001`, and the console runs on it. The original ESP32 and the S2 don't have one. |
| The board's USB-C port wired to the **chip's own USB pins**, not to a USB-UART bridge (CP2102, CH340) | A bridge shows up with another USB ID, and the agent won't find it. Plug the board in and run `python3 -m serial.tools.list_ports -v`: you want `VID:PID=303A:1001`. |
| **8 MB flash** or more | The app is about 1.7 MB and the default partition table has a 6 MB app slot. A 4 MB board works with a smaller slot in its own `partitions.csv`. |
| A display LVGL 9 can drive, roughly 300 px or more on each side | Smaller screens need a new UI layout, and very small ones may not fit the content. |

PSRAM is optional (the C6 has none). A touch panel, IMU or battery charger isn't
used. Leave those off.

## How the firmware is split

```
firmware/
  main/                     shared: protocol parser, Wi-Fi, console, UI
  components/board_api/     the contract main/ builds against: board.h, display.h
  boards/<id>/              one directory per board
    board/                  ESP-IDF component "board": implements the contract
      include/board_config.h  BOARD_ID, UI size, mono flag
    sdkconfig.defaults      chip, flash size, partition table, console, PSRAM
    partitions.csv
    dependencies.lock       component versions for this board (written by the first build)
    README.md               pins, sources, quirks, what's been verified
```

`make build BOARD=<id>` adds `boards/<id>` to the build, layers its `sdkconfig.defaults`
over `firmware/sdkconfig.defaults`, sets the chip from its `CONFIG_IDF_TARGET`, and
builds in `firmware/build/<id>/`. Nothing in `main/` changes per board except the UI
layout table.

## The contract

`firmware/components/board_api/include/board.h` and `display.h` declare it.

| You implement | What it must do |
|---|---|
| `board_init()` | Power rails, buses, panel reset and init. Called once at boot, before anything draws. |
| `display_start(rot)` | Start LVGL on the panel: its tick, its draw buffers, a flush callback, and a task that runs `lv_timer_handler()`. If the board can't show `rot`, use its default rotation. |
| `display_lock(ms)` / `display_unlock()` | A **recursive** mutex around LVGL. `main/` holds it around every `lv_*` call. `ms < 0` waits forever. |
| `display_set_rotation(rot)` | The caller holds the lock. Return `ESP_ERR_NOT_SUPPORTED` for a rotation the board can't show. The console then answers `ERR rot <r> not supported on this board`. |
| `display_get_rotation()` | The current rotation. |

`board/include/board_config.h` defines:

| Constant | Meaning |
|---|---|
| `BOARD_ID` | The directory name, e.g. `"waveshare-esp32s3-rlcd-4.2"`. It's reported in the handshake (`board=`), and the installer uses it to pick the matching firmware. |
| `BOARD_UI_W`, `BOARD_UI_H` | The canvas the UI lays out on, as the viewer sees it, in pixels. `ui.c` needs a layout for this size. |
| `BOARD_DISPLAY_MONO` | `1` for a black-and-white panel. The UI then draws only black on white, with outlines where the color theme uses fills. |

Two rules the shared code relies on:

- **Render RGB565.** The `shot` command tees LVGL's flush and expects 2 bytes per
  pixel. Convert to your panel's format (byte-swapped, 1-bit, whatever it needs)
  inside your flush callback, after LVGL hands you the pixels.
- **Be LVGL's default display.** `main/` draws on `lv_display_get_default()`.

Both existing boards keep the panel's registers and buses in `board.c` and the LVGL
glue in `display.c`, with a private header between them. That split isn't required,
but reviewers will find it familiar.

## Step by step

The examples use `acme-esp32s3-lcd-3.5`. Name yours `<vendor>-<chip>-<display>-<size>`,
lowercase.

### 1. Back up the factory firmware, and check which board you have

Before you flash anything, save the whole flash so you can put the board back the
way it came:

```bash
python3 -m esptool -p <port> read_flash 0 0x1000000 factory-16MB.bin   # 0x800000 for 8 MB
```

Keep that file out of the repo.

Then make sure the board is the one you think it is. Boards look alike, boxes get
swapped, and listings carry the wrong photo. Two checks:

- What the factory firmware was built for. Pull its app out and read its header:

  ```bash
  python3 -m esptool -p <port> read_flash 0x10000 0x300000 factory-app.bin   # the app's whole slot
  python3 -m esptool image_info --version 2 factory-app.bin   # "Project name", build date, ESP-IDF
  ```

  `strings factory-app.bin | less` often names the panel controller too.
- The screen's resolution and the controller printed on the board's back, against the
  vendor's page.

SparkESP32's second port was written for the wrong board this way: the board on the
bench was a 2.8" TFT, not the RLCD we took it for, and every check that doesn't
look at the glass passed anyway.

### 2. Gather the hardware facts

From the vendor's wiki, schematic and example code, collect:

- the pins for the panel bus (SPI, QSPI, I80 or RGB) and its reset, backlight and power-enable pins
- the panel controller and its init sequence: the register writes after reset
- whether a PMIC or an IO expander has to switch the panel's power on first
- the flash size and PSRAM type (quad or octal)

Read the license on the vendor's example code before copying any of it. Much
of it has no license at all, which means you can't redistribute it. Pin numbers and
register values are facts, and you can use them. Write the code yourself and credit
where the facts came from in your board's README and in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). Check for an ESP-IDF component
for your controller (`esp_lcd_<controller>` on the
[component registry](https://components.espressif.com)) before you write a driver.

### 3. Create the board directory

Start from whichever existing board is closer to yours: the ES3C28P for a color
panel with an `esp_lcd` driver, the RLCD for a panel you have to drive yourself.
`tools/new_board.py` copies it and does the mechanical part:

```bash
python3 tools/new_board.py acme-esp32s3-lcd-3.5 --from lcdwiki-esp32s3-tft-2.8
python3 tools/new_board.py acme-esp32c3-oled-1.3 --from lcdwiki-esp32s3-tft-2.8 --chip esp32c3
```

It sets `BOARD_ID`, points `sdkconfig.defaults` at the new board's own partition table
(and chip, with `--chip`), drops the copied `dependencies.lock` (your first build
writes one), replaces the README with a skeleton, and lists the board in
`firmware/boards/README.md`. Then edit:

- **`board/include/board_config.h`**: the UI size and the mono flag.
- **`sdkconfig.defaults`**: the flash size, and PSRAM if any (remove the copied PSRAM
  lines if your board has none). Keep `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`.
- **`partitions.csv`**: the shared layout fits 8 MB and up. For 4 MB, shrink the app slot to `3M`.
- **`board/CMakeLists.txt`**: your source files, `REQUIRES board_api`, and in
  `PRIV_REQUIRES` the drivers you use (`esp_lcd`, `esp_driver_spi`, `esp_driver_i2c`, ...).
- **`board/idf_component.yml`**: LVGL (`lvgl/lvgl: "^9.6.0"`) and any panel driver component.
- **`firmware/boards/README.md`**: the display column of your board's row.

`python3 tools/check_boards.py` (part of `make check`) lists whatever is still missing.

### 4. Write `board_init()`

Bring the panel up and draw nothing yet. Order matters on most boards: power first,
then reset, then the init sequence, then display on. Log each step with `ESP_LOGI`
while you work, and check every return code with `ESP_ERROR_CHECK` or `ESP_RETURN_ON_ERROR`.

A useful first test: fill the panel with one solid color (or all black) from `board_init()` and
check that the whole screen changes. That confirms the bus, the reset and the
address window before LVGL adds anything.

### 5. Write `display.c`

Pick by panel type and memory:

| Your board | Do what |
|---|---|
| A panel with an `esp_lcd` driver | Render in strips (`LV_DISPLAY_RENDER_MODE_PARTIAL`, a small DMA buffer) and send each with `esp_lcd_panel_draw_bitmap()`. Most SPI panels read RGB565 big-endian: `lv_draw_sw_rgb565_swap()` the strip first. The ES3C28P's `display.c` is the plainest example; the C6's adds software rotation. |
| A controller with no driver, or a 1-bit panel | Keep a frame buffer in the panel's own format. In the flush callback, convert LVGL's RGB565 pixels into it, and send the whole frame on the last flush (`lv_display_flush_is_last()`). The RLCD's `display.c` is this case. |
| A backlight | Keep it off until the first real frame has gone out, so the panel's power-on garbage never shows. |
| No PSRAM | Keep the draw buffers small (1/10 of the screen or less), in internal DMA-capable RAM. |

Rotation:

- On a square panel, all three rotations are layouts of the same canvas: rotate the
  pixels in the flush callback, or with the panel's own mirror and swap bits.
- On a rectangular panel, the UI is laid out landscape or portrait, not both.
  Support the two rotations that keep that shape, and return `ESP_ERR_NOT_SUPPORTED`
  for the third. The saved default is `cw`, so make `cw` work.

For a black-and-white panel, threshold each pixel on brightness
(`r*77 + g*150 + b*29`). The mono theme only draws black and white, so the threshold
only meets anti-aliased edges. Don't dither: dithering makes small text fuzzy.

### 6. Add a UI layout, if your screen is a new size

`firmware/main/ui.c` has one `layout_t` per canvas size, picked at compile time:

```c
#if BOARD_UI_W == 480 && BOARD_UI_H == 480
static const layout_t L = { ... };
#elif BOARD_UI_W == 400 && BOARD_UI_H == 300
static const layout_t L = { ... };
#else
#error "no UI layout for this board's BOARD_UI_W x BOARD_UI_H: add one"
#endif
```

Copy the closest one, add an `#elif` for your size, and scale the positions. Every
board has Montserrat 14, 16, 28 and 48. A small screen may need a size in between
for the title and the ring values: turn it on in your board's `sdkconfig.defaults`
(the ES3C28P adds `CONFIG_LV_FONT_MONTSERRAT_24=y`) and name it in your layout. Check
every element for overlaps with `tools/devctl.py demoshot`, at loads 0, 70 and 100
(the widest numbers).

### 7. Build, check, flash

```bash
. ~/esp/esp-idf/export.sh
make check                                          # includes tools/check_boards.py
make build BOARD=acme-esp32s3-lcd-3.5               # no warnings in your board or main/
make flash BOARD=acme-esp32s3-lcd-3.5 PORT=/dev/ttyACM0
```

Always build through `make`. It passes the chip and the sdkconfig path to `idf.py`,
so a stray `firmware/sdkconfig` from another board can't change your target. If the
build ever says the target doesn't match, delete `firmware/build/<id>` and build again.

If flashing fails because esptool reports that the image isn't for this chip, you
passed the wrong `BOARD`. esptool leaves the chip in its bootloader after that;
`python3 -m esptool -p <port> --after hard_reset chip_id` starts it again.

### 8. Verify

Run the whole list, on the real board, and keep the output for your pull request:

| Check | How | Pass |
|---|---|---|
| Handshake | `python3 tools/devctl.py status` | first data line `fw=sparkesp32 version=... board=<id>` |
| Render | `python3 tools/devctl.py demoshot assets/boards/<id>.png 70` | every element in place, nothing clipped or overlapping. This becomes the board's screenshot in the repo (for a 1-bit panel, store what the panel shows: your driver's threshold applied to it) |
| The physical screen | a photo of the board showing the demo | matches the screenshot: upright, not mirrored, not inverted, nothing shifted |
| Rotations | `devctl.py rot cw`, `rot ccw`, `rot none` | each one either redraws correctly or is refused with `ERR rot ... not supported` |
| Wi-Fi | `python3 tools/provision.py --ssids "<net>" --host <spark>.local`, then `devctl.py wifi` and `devctl.py status` | `link state=connected`, then `screen=online reason='via WI-FI'` with live values |
| USB feed | on a Spark: `sudo ./install.sh --board <id>` from your branch | the installer's checks pass and the screen reads ONLINE · via USB |
| Cold plug-in | unplug, wait 10 s, plug back in; five times | the agent (or `devctl.py status`) gets an answer every time |
| Settings survive | `make flash` again, then `devctl.py status` | the name and Wi-Fi from before are still there |

`shot` shows what LVGL rendered, before your flush callback converts it. A
screenshot can look perfect while the panel shows it mirrored or inverted, which is
why the photo is on the list.

## Things that will bite you

- **Flash split images only.** A merged image overwrites the settings partition and
  the board forgets its name and Wi-Fi. `make flash` and the installer do this right.
- **The USB erratum.** On the ESP32-C6, a cold power-on can leave USB dead until a
  reset ([espressif/esp-idf#17432](https://github.com/espressif/esp-idf/issues/17432)).
  `main.c` restarts the CPU once at every power-on, on every board. That reset keeps
  the USB device, so the host doesn't lose the port. Test cold plug-in anyway: your
  chip may have its own quirks.
- **The console needs the USB-Serial/JTAG port.** If `sdkconfig.defaults` moves the
  console to a UART, the board still boots, but the agent and tools never find it.
- **Big screenshots.** A full-screen `shot` is several hundred KB of console text. The
  shared code yields every 64 lines so the task watchdog doesn't fire; don't add long
  busy loops to your flush callback.
- **Lock everything.** LVGL calls from any task without `display_lock()` will
  crash eventually, usually days later. Your LVGL task must take the same lock around `lv_timer_handler()`.
- **Keep the SPI interrupt small.** esp_lcd's "transfer done" callback runs in an
  interrupt, which can land while flash is busy saving a setting. Code in flash
  (LVGL included) can't run then: give a semaphore from the callback and call
  `lv_display_flush_ready()` from the flush instead.
- **A black screen is usually the backlight.** A TFT with its backlight off looks
  powered down no matter what you send it. Find the backlight pin before you debug
  the panel.
- **Board IDs are permanent.** The ID ends up in release asset names and in the
  handshake, and the installer matches on it. Don't rename a board after it ships.
