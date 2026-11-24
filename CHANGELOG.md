# Changelog

All notable changes to this project are documented here.

## 1.4.3 — 2026-11-24

### Security

- The installer checks a firmware bundle's `flash_args` before esptool, which runs as
  root, gets it: only flash settings and the bundle's three images. A line naming any
  other file or option stops the install. The bundle's `SHA256SUMS` now covers
  `flash_args` too.
- A board name from the device (`board=` in its handshake) must be one of the boards in
  `firmware/boards/`, never a path.
- Every Python the installer and the service run is isolated (`-I`), including the tools
  and the agent itself.

### Fixed

- A Sparkline snapshot with a number too big to round (`1e999`) or a reply over 64 KB is
  now an unrecognized snapshot: the agent samples the Spark itself and asks Sparkline again
  after 30 s. Before, it raised on every tick and the display went offline.

### Changed

- SECURITY.md: any computer the display is plugged into can read its Wi-Fi password from
  flash (`esptool read_flash`), not just someone holding the board.

## 1.4.2 — 2026-11-23

### Security

Fixes for [GHSA-7jc8-39c8-wwv9](https://github.com/twidtwid/SparkESP32/security/advisories/GHSA-7jc8-39c8-wwv9),
reported and fixed by [@joeynyc](https://github.com/joeynyc).

- Sparkline's agent token, which opens all of Sparkline's API, stays with the service and
  on the Spark. The service runs as an unprivileged user of its own (systemd's
  `DynamicUser`) instead of the user who ran `sudo`, whose other programs could read the
  token from the agent's environment. With Sparkline's `bind = "auto"`, the agent looks up
  this Spark's address for every request instead of keeping the one from install time, and
  it sends the token only over a connection that ends on the Spark itself. Before, a new
  DHCP lease sent the token, in clear text, to whichever machine got the old address.
- The installer runs every `python` isolated (`-I`). Run from a directory someone else can
  write to, it imported `json.py`, `venv.py` and the like from there, as root.
- The agent's HTTP port is limited to the Spark and its subnet by the systemd unit
  (`IPAddressAllow`), whether or not ufw is active. It serves at most 16 requests at once:
  about a thousand idle connections used up its file descriptors and stopped the USB feed
  too. `/stat?v=` with a value `int()` can't read no longer leaves a traceback in the
  journal. `/healthz` keeps the serial port's path, Sparkline's URL and error texts for
  requests from the Spark itself.
- The agent asks a device that isn't a display once per plug-in. It used to send `status`
  to an ESP32 with native USB again every five minutes, and the "exclusive" open only kept
  out programs that lock the port the same way. `sudo ./install.sh --only-this-display`
  saves which device answered (`/etc/sparkesp32/display.env`), and the agent then opens
  that device only. The host tools lock the port like the agent does.
- Releases: every GitHub action is pinned to a commit, the firmware is built by a job that
  can't write to the repository, and the bundles carry a signed build provenance
  (`gh attestation verify <file> --repo twidtwid/SparkESP32`).
- The installer no longer flashes a device that doesn't run SparkESP32 without being told
  to. Updating with `sudo ./install.sh --yes` while some other ESP32 board was the only one
  plugged in replaced that board's program.
- The installer's Python packages are installed by hash: `agent/requirements.txt` names
  every package esptool pulls in, not just pyserial and esptool.
- The tools print what a device sends with every control character but tab and newline
  removed. Only ANSI colour codes were dropped before, so a device could send other
  escape sequences to root's terminal.
- The service's sandbox is tighter: no capabilities, no home directories, an ordinary
  service's system calls, only IP, Unix and netlink sockets
  (`systemd-analyze security sparkesp32`: 7.8 to 2.1).

### Changed

- A display on Wi-Fi in another subnet than the Spark needs `sudo ./install.sh --lan
  <its subnet>`, also where ufw isn't active. `--lan` adds that subnet for the service
  (the Spark's own stays, for Sparkline) and replaces it in the ufw rule, and must be a
  subnet such as `192.168.1.0/24`.
- `RUN_USER` is gone from the installer: the service no longer runs as a login user.
- After `--only-this-display`, another display (a second, a replacement, a CH340 board
  moved to another USB socket) isn't picked up by plugging it in: run `sudo ./install.sh
  --only-this-display` with it attached. `--any-display` goes back to asking every
  candidate device, once per plug-in. `/healthz` lists the devices the agent leaves alone
  under `display.ignoring`.
- With no display saved, an Espressif device that doesn't answer as a SparkESP32 is left
  alone until it's replugged, as bridges already were, instead of asked again every five
  minutes. A board flashed while the agent runs is picked up after a replug or
  `sudo systemctl restart sparkesp32`.
- `devctl.py` and `provision.py` refuse a port the agent has, instead of garbling its
  console: stop the agent first, as before.
- At a terminal, the installer asks before it flashes a board that isn't running
  SparkESP32 (Enter goes ahead for a new display). `--port` or `--board` answers the
  question in advance; `--yes` flashes one only on a first install (or when it's the display
  saved with `--only-this-display`).
- `agent/requirements.txt` is generated from `agent/requirements.in` (`uv pip compile`,
  the command is at the top of both).

## 1.4.1 — 2026-11-22

### Fixed

- E-paper: a rotation requested while a full refresh was running (the installer sets the
  rotation right after flashing, during the start-up refresh) went out as a partial
  refresh, leaving the old image ghosted until the hourly wipe. It's a full refresh now.
- E-paper: a refresh that never started (its command lost) counted as shown, and the next
  partial refresh compared against the wrong image. It's now an error, retried as a full
  refresh.
- The tools and the installer use the chip's own USB port when one is attached, so an
  unrelated CH340 or CP210x adapter (a Zigbee stick, say) no longer makes them stop with
  "several USB serial devices".
- The agent opens a port exclusively, so it never writes into a port another program
  holds, and it leaves a USB-UART bridge that's in use alone until it's replugged.
- A USB-UART bridge replugged within the agent's two-second poll is retried, rather than
  staying ignored until the agent restarts.

### Changed

- A board behind a USB-UART bridge that still runs 1.3.0 (console at 921600) is found
  again: when it doesn't answer at 115200, the agent and the tools ask at 921600. A plain
  `sudo ./install.sh` upgrades it; `--board` is no longer needed.
- The installer and the docs say that the udev rule keeps ModemManager off every CH340 and
  CP210x adapter on the Spark, not just the display.

## 1.4.0 — 2026-11-22

### Added

- A fifth board: the Elecrow CrowPanel ESP32 4.2" E-Paper, V1.2 (`elecrow-esp32s3-epaper-4.2`,
  the version with a green circular sticker on the back), a 400×300 black-and-white e-paper
  with an UltraChip-style controller. The same refresh policy as the 5.79": partial
  refreshes with no flash at most every 10 s, a full refresh once an hour. The older V1.0
  board (an SSD1683, no sticker) isn't supported.

### Changed

- Boards behind a USB-UART bridge (the Elecrow e-papers) run their console at the default
  115200 baud instead of 921600, and the agent and tools open ports at 115200. At 921600,
  Linux's CH340 driver damaged about 1 in 5 full-screen screenshots; the stat feed needs
  about 200 bytes a second. A 5.79" e-paper still on 1.3.0 doesn't answer at the new rate:
  upgrade it once with `sudo ./install.sh --board elecrow-esp32s3-epaper-5.79`.

### Fixed

- `devctl.py shot` retries a screenshot with a line damaged on the wire, as it does one with
  a bad CRC, instead of stopping with a base64 error.

## 1.3.0 — 2026-11-22

### Added

- A fourth board: the Elecrow CrowPanel ESP32 5.79" E-Paper
  (`elecrow-esp32s3-epaper-5.79`), a 792×272 black-and-white e-paper run by two cascaded
  SSD1683 controllers. Updates are partial refreshes, with no flash, at most every 10 s;
  a change of state (ONLINE to OFFLINE, CLOCK STUCK) still shows at once, and a full
  refresh wipes the ghosting once an hour. Wide by default; `--rotation ccw` stands it on
  its short end, USB-C on the right, with a tall layout of its own.
- Boards whose USB port is a USB-UART bridge (CH340, CH343, CH9102, CP210x): the agent,
  the installer and the host tools find them as well as the chip's own USB, and open every
  port at 921600 baud (native USB ignores the rate). A bridge that doesn't answer as a
  SparkESP32 is left alone until it's replugged, and the installer skips one unless
  `--board` or `--port` names it. ModemManager is kept off the bridges too.

### Changed

- A board can offer a wide and a tall canvas, one rotation each; the UI lays itself out
  again when the shape changes. Layouts give each ring and tile its own position.

## 1.2.0 — 2026-11-21

### Added

- The GPU ring shows the GPU clock, and catches the GB10's stuck-clock state: under load
  the GPU runs at 500–850 MHz instead of ~2,000–2,400 MHz, reports no throttle reason,
  and stays that way until the Spark's power is fully drained (a reboot doesn't clear it).
  When the GPU has run under load (80 %+) below 1000 MHz with no clock event reason for
  10 seconds, the GPU ring turns amber and the line under ONLINE reads **CLOCK STUCK**
  (on the black-and-white RLCD, that line and the GPU ring's title are inverted instead);
  5 seconds under load at 1400 MHz or more clears it. Nothing moves, and no text comes
  within 3 px of a ring on any board: on the 2.8" TFT's small rings, the lines under the
  percentages use a 12 px font. The
  thresholds are the community detector's from NVIDIA's forums. The agent logs a warning
  when it happens, and `/healthz` reports `gpu_mhz`, `gpu_clock_events` and
  `gpu_clock_stuck`. The clock comes from `nvidia-smi`, or from Sparkline's snapshot.
- Protocol v2: two fields at the end of the `stat` line, `gpu_mhz` and `gpu_stuck`. Old
  and new halves keep working together: the firmware still takes v1 lines, and the agent
  sends v2 only to a board that asks for it (`proto=2` in its `status` handshake over USB,
  `GET /stat?v=2` over Wi-Fi). Plain `/stat` is still v1.

### Fixed

- A long title, such as a hyphenated hostname (`dgx-spark-lab-bench-07`) or a saved name
  with spaces, no longer wraps onto a second line and into the rings. It stays on one
  line and ends in "…", as intended.

## 1.1.0 — 2026-11-21

### Added

- On a Spark that runs [Sparkline](https://github.com/sfxnz/sparkline), the agent reads
  Sparkline's samples (`GET /api/v1/snapshot`) instead of running `nvidia-smi` a second time,
  so the display and the Sparkline dashboard show the same numbers. `install.sh` finds a
  running Sparkline agent and keeps its token in a root-only file for the service; the token
  never goes to the display. When Sparkline doesn't answer, the agent samples by itself and
  asks again every 30 s. `/healthz` reports `source` and a `sparkline` state;
  `install.sh --no-sparkline` turns it off.

## 1.0.1 — 2026-11-21

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
