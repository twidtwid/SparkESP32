# AGENTS.md

Instructions for AI coding agents working in this repo: setting SparkESP32 up for
someone, or changing it. Humans: [README.md](README.md) is the friendlier version.

## Hard rules

1. **Never handle the Wi-Fi password.** Don't ask for it in chat, don't put it in a
   command line, a file, or an environment variable you set. The installer and
   `tools/provision.py` prompt for it on a hidden TTY prompt; let the human type it. If
   you have no TTY, set up USB-only and tell the human the exact command to run themselves.
2. **Run the installer on the Spark, with sudo, from the repo root.** It needs root for
   systemd/udev/ufw, and it runs the agent as the invoking (non-root) user.
3. **Flash split images only** (bootloader, partition table and app, at the offsets in
   the bundle's `flash_args`). The merged image overwrites the NVS partition and the
   board forgets its name and Wi-Fi. The installer and `make flash` already do this right.
4. **Don't change settings the human didn't ask for.** The installer only fills in unset
   values unless given a flag. Keep it that way.

## Setting it up for someone

Preconditions: you have a shell on the DGX Spark (or can ask the human to run commands
there), and the display is plugged into one of the Spark's USB-C ports with a data cable.

```bash
git clone https://github.com/twidtwid/SparkESP32.git && cd SparkESP32
lsusb | grep -i 303a:1001          # the display: "Espressif USB JTAG/serial debug unit"
```

Decide with the human (ask; don't guess):

- **Display name.** By default the title is the Spark's hostname, sent live by the agent;
  pass nothing. Only use `--name` if they want something else (up to 24 printable ASCII
  characters). `--name ""` returns to the hostname.
- **Wi-Fi or USB-only.** USB-only needs nothing else. For Wi-Fi you need the network
  name(s); the board is 2.4 GHz only.

Then run **one** of these. If you can't type into a TTY yourself, hand the human the
exact line:

```bash
# USB only, no questions
sudo ./install.sh --yes --no-wifi

# with Wi-Fi: the human types the password at the hidden prompt
sudo ./install.sh --ssids "HomeNet"

# fully unattended with Wi-Fi: the HUMAN exports the variable in their own shell first
sudo --preserve-env=WIFI_PSK ./install.sh --yes --ssids "HomeNet" --wifi-password-env WIFI_PSK
```

Useful flags: `--host <ip>` when mDNS (`<hostname>.local`) doesn't resolve on their LAN;
`--port /dev/ttyACMn` if several Espressif boards are attached; `--board <id>` if the
installer can't tell which supported board it is (it asks the firmware, then the chip);
`--no-flash` to leave the firmware alone; `--lan <cidr>` to override the subnet ufw
allows. `./install.sh --help` lists everything.

### Definition of done

All of these, checked by you, not assumed:

```bash
systemctl is-active sparkesp32                         # -> active
curl -fsS localhost:8787/stat                          # -> stat <cpu> <gpu> ... (8 fields)
curl -fsS localhost:8787/healthz | python3 -m json.tool   # "display": "connected on /dev/ttyACM0"
```

- The screen reads **ONLINE · via USB** (ask the human to look, or check
  `"display": "connected ..."` in `/healthz` and that `age_s` < 2).
- If Wi-Fi was set up: unplug from the Spark, power the board from any USB charger, and
  within ~20 s it reads **ONLINE · via WI-FI**. If it can't, `tools/devctl.py wifi` on a
  machine it's plugged into shows `last_err` (see docs/troubleshooting.md).
- Tell the human: the name shown, how it's connected, and the change command
  (`sudo ./install.sh --name ...`).

## Repo map

```
install.sh                 Spark-side installer (flash, provision, service, firewall, verify)
VERSION                    the one version: firmware (CMake reads it), agent, installer
agent/
  sparkesp32_agent.py      sampler + USB push + HTTP /stat,/healthz (stdlib + pyserial)
  sparkesp32.service       systemd unit template (@RUN_USER@)
  99-sparkesp32.rules      udev: ModemManager ignore for 303a:1001
  requirements.txt         pinned pyserial + esptool for /opt/sparkesp32/venv
tools/
  sparklink.py             serial plumbing: find by USB ID, handshake, run-until-terminator
  provision.py             set name/host/Wi-Fi/rotation over USB (never prints the password)
  devctl.py                bench/QA: status, demo, CRC-checked screenshots, raw console
  check_boards.py          every firmware/boards/<id>/ has what a board needs (make check)
firmware/                  ESP-IDF 5.5 project; the board picks the chip
  main/spark_data.c        the protocol parser both transports share
  main/net.c               Wi-Fi supervisor + 1 Hz HTTP poll
  main/link.c, status.c    the Wi-Fi link and screen-state machines (pure C, host-tested)
  main/ui.c                LVGL screen; one layout per board canvas size
  main/console_cmds.c      USB console commands (the agent's and the tools' interface)
  components/board_api/    the board contract main/ builds against: board.h, display.h
  components/xpowerslib/   vendored XPowersLib (MIT), unmodified
  boards/<id>/             one per board: board/ component, sdkconfig.defaults,
                           partitions.csv, dependencies.lock, README.md
    waveshare-esp32c6-amoled-2.16/   the reference board, and the default
    lcdwiki-esp32s3-tft-2.8/         ESP32-S3, 2.8" color SPI TFT (esp_lcd_ili9341)
    waveshare-esp32s3-rlcd-4.2/      ESP32-S3, 1-bit reflective LCD (hand-written driver)
tests/                     host tests: agent + tools (Python); parser and state machines (C)
docs/                      protocol.md, troubleshooting.md, adding-a-device.md
```

## Changing the code

- `make check` must pass. It needs only python3 and a C compiler, and covers py_compile,
  unit tests, the C parser and state-machine tests, `tools/check_boards.py`, ruff (via
  `uvx` if not installed) and shellcheck.
- Firmware: `. ~/esp/esp-idf/export.sh` (ESP-IDF **v5.5.x**), then `make build` (the C6,
  the default) or `make build BOARD=<id>`. Shared code (`firmware/main`) must build on
  every board: run `make dist` before you call a change done. Treat any new warning in
  `firmware/main` or `firmware/boards/` as an error.
- Always build through `make`. It passes the board's chip and sdkconfig path to `idf.py`;
  plain `idf.py build` guesses the chip and can build for the wrong one.
- Hardware check after firmware changes: `make flash`, then
  `python3 tools/devctl.py status` (first data line must be `fw=sparkesp32 version=...
  board=<id>`), then `python3 tools/devctl.py demoshot /tmp/s.png 70` and look at the PNG.
- **Protocol changes** touch four places at once: `firmware/main/spark_data.c`,
  `agent/sparkesp32_agent.py`, `tests/`, and `docs/protocol.md`. The console splits on
  spaces with a 32-token cap, so keep each sample to a few tokens.
- Console replies always end in a line starting with `OK` or `ERR` (or a `*-END` marker
  for multi-line output, which an `ERR ...` line can replace when the command can't run).
  `tools/sparklink.py` depends on that.
- Bump `VERSION` and `__version__` in the agent together (`make check` enforces it), and
  add a `CHANGELOG.md` entry.

## Adding a new device

The human guide is [docs/adding-a-device.md](docs/adding-a-device.md); read it first.
This is the order to work in and what to prove at each step.

1. **Confirm the board can work** before writing code: an ESP32-C3/C5/C6/S3 (Wi-Fi plus
   native USB-Serial/JTAG), USB wired to the chip, `VID:PID=303A:1001` in
   `python3 -m serial.tools.list_ports -v`, 8 MB+ flash. If any of these fail, stop and
   tell the human why.
2. **Back up the factory flash** (`esptool read_flash 0 <flash size> factory.bin`) to a
   path outside the repo, and tell the human where it is. Then **prove which board it
   is**: `esptool image_info --version 2` on its app (see the guide's step 1), and the
   resolution and controller printed on the board against the vendor's page. Don't take
   the box's word for it: a board that isn't what it says passes every check except
   the human looking at the screen.
3. **Collect facts, not code.** Pins, panel controller, init sequence, power-enable
   order, from the vendor's docs. Check the license of any vendor example before
   reusing it; with no license, write the driver yourself and cite the source in the
   board README and `THIRD_PARTY_NOTICES.md`.
4. **Copy the closer existing board** to `firmware/boards/<vendor>-<chip>-<display>-<size>/`,
   delete the copy's `dependencies.lock`, then edit `board_config.h`, `sdkconfig.defaults`,
   `partitions.csv` (if flash < 8 MB), `board/CMakeLists.txt`, `board/idf_component.yml`, and add
   the board to `firmware/boards/README.md`. `python3 tools/check_boards.py` must pass.
5. **Panel first, LVGL second.** Get a solid fill on the whole panel from `board_init()`,
   then write `display.c` against the contract in `firmware/components/board_api/`.
   Render RGB565; convert in the flush callback.
6. **A new canvas size gets a new `#elif` layout in `firmware/main/ui.c`**, not a change to
   the existing ones. The C6's 480×480 layout must stay byte-for-byte the same.
7. **Build every board** (`make dist`), zero new warnings. Flash yours with
   `make flash BOARD=<id> PORT=...`.
8. **Verify on hardware** with the table in the guide's "Verify" step. You can run the
   handshake, demoshot, rotations, Wi-Fi and reflash checks yourself. Only the human can
   check the physical screen (lit, upright, not mirrored, not inverted) and do the cold
   plug-ins. Ask for those in one batch, once, at the end, and don't report the board
   as working until the screen check is in.
9. **Write the board README**: chip, display, pins, sources, quirks, and a "Verified"
   section that says what was checked on the real board and what wasn't. Don't claim a
   check nobody ran.
10. **Open the pull request** following [CONTRIBUTING.md](CONTRIBUTING.md#adding-a-new-device),
    with the evidence it lists.

Don't touch the reference board's files or make the new board the default. The C6
AMOLED stays the recommended board in the README and in every screenshot.

## Releasing

Tag `vX.Y.Z` matching `VERSION`. `.github/workflows/release.yml` builds every board
(`make dist`) and attaches, per board, `sparkesp32-<board>-firmware-X.Y.Z.tar.gz` (split
images + `flash_args` + `SHA256SUMS`) and `sparkesp32-<board>-X.Y.Z-merged.bin`, plus one
`SHA256SUMS` over all of them. `install.sh` picks the board from the handshake's `board=`
(or the chip, or `--board`) and downloads that board's tarball when the repo has no local
build.
