# Changelog

All notable changes to this project are documented here.

## Unreleased

### Fixed

- `shot` no longer loses whole lines of a screenshot. On the C6 reference
  board, 6 of 31 full-screen dumps came back with a strip that failed its CRC
  check, and `devctl.py shot` had to retry. ESP-IDF's console throws away
  output, and still reports success, while its USB connection monitor thinks the
  host is gone. On the C6 that monitor is sometimes wrong for a few milliseconds
  with the host attached and reading, and each time the dump lost every line it
  wrote meanwhile (17 to 61 lines). The dump now writes straight to the
  USB-Serial/JTAG driver, which waits for room and doesn't ask the monitor; if
  the host stops reading, the dump ends in an `ERR` line. Measured after the fix:
  148 of 148 dumps intact, including 4 that hit the monitor's false alarm. A
  full-screen C6 screenshot also takes 1.0 s instead of 5.7 s.
- `devctl.py` reports the board's own `ERR` when a dump stops partway, instead
  of a CRC or coverage complaint.

## 1.0.0 — 2026-11-21

First public release.

- Firmware for the Waveshare ESP32-C6-Touch-AMOLED-2.16 shows CPU, GPU and
  unified-memory rings, a per-core heatmap, device temperature and power, and an
  ONLINE / OFFLINE / NO WI-FI / SET HOST / WAITING state.
- Two transports carry one line format (protocol v1, [docs/protocol.md](docs/protocol.md)):
  - USB serial when the board is plugged into the Spark.
  - A 1 Hz HTTP poll of the Spark's agent over a LAN Wi-Fi network, with WPA2/WPA3
    and up to 4 SSIDs tried in order.
- The display's title is the Spark's hostname, sent live by the agent, unless a name is set.
- When the screen isn't ONLINE, its second line says why: agent stopped, Spark
  unreachable, agent stale, joining, no network, wrong password.
- `/healthz` reports the USB link (`display.state`: searching, handshaking, streaming,
  rejected) and nvidia-smi's state (`gpu_source`); a hung nvidia-smi is killed and
  restarted.
- The `sparkesp32` agent is a systemd service that samples `nvidia-smi` and `/proc`
  once a second. It pushes samples over USB to a device that passes the
  firmware handshake, and serves `/stat` and `/healthz`.
- `install.sh` runs on the Spark. It flashes the firmware (split images that
  keep the board's settings), configures name, host and Wi-Fi (password typed at
  a hidden prompt), installs the service, the udev rule and the ufw rule, and
  verifies the result end to end.
- Host tools: `tools/provision.py` for setup from any computer and
  `tools/devctl.py` for QA and CRC-checked screenshots.
- Tests: the firmware's protocol parser and its Wi-Fi link and screen-state machines
  run on the host, along with the agent, the serial tools and screenshot reassembly.
- Builds for more than one board. Each lives in `firmware/boards/<id>/` behind a small
  contract (`firmware/components/board_api`); `make build BOARD=<id>` picks one, and the
  Waveshare ESP32-C6-Touch-AMOLED-2.16 stays the default and the reference.
- The LCDWiki 2.8inch ESP32-S3 Display (ES3C28P): a 2.8" 240×320 color SPI TFT on
  Espressif's `esp_lcd_ili9341` driver, with a compact landscape layout. Verified on
  hardware.
- The Waveshare ESP32-S3-RLCD-4.2: a 4.2" black-and-white reflective LCD with a
  hand-written ST7306 driver and a mono layout. Verified on hardware.
- Releases carry firmware for every board. The handshake reports `board=<id>`, and the
  installer picks the board from it (or from the chip, or `--board`) and flashes that
  board's images at the offsets in its `flash_args`.
- [docs/adding-a-device.md](docs/adding-a-device.md) walks through a port, and
  CONTRIBUTING.md and a new-device issue template cover submitting one.
  `tools/new_board.py` starts a board from the closest existing one, and
  `tools/check_boards.py` (part of `make check`) checks a board directory has everything,
  its demo screenshot (`assets/boards/<id>.png`) included.
- `install.sh --board` reflashes a board whose firmware reports a different board.
- Works around an ESP32-C6 USB erratum ([espressif/esp-idf#17432](https://github.com/espressif/esp-idf/issues/17432)):
  a power-on boot can leave the USB serial link dead until the chip is reset, so the
  firmware restarts its CPU once at power-on. The host keeps the port through that restart.
- The agent and host tools keep asking `status` for 10 s, since a board that was just
  plugged in only answers once its console is up, about 2 s after boot. Replugging a
  board the agent gave up on makes it try again at once.
