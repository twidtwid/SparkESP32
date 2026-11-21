# Contributing

Thanks for helping improve SparkESP32.

## Before opening an issue

- Search existing issues first.
- Use the bug-report template for reproducible problems and include the output
  it asks for. Most issues are answered by `curl localhost:8787/healthz` and
  `tools/devctl.py status`.
- Don't include Wi-Fi passwords, your network's hostnames or addresses, or
  other private data.

## Development setup

Requirements:

- python3 (3.10+) and a C compiler for `make check`. Nothing else is needed for
  the agent, tools and tests.
- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32c6/get-started/),
  for firmware. One install builds every board; each board sets its own chip.
- A supported board for anything you want to see on a screen. The
  [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm)
  is the reference board ([all boards](firmware/boards/README.md)).

```sh
make check                      # always: unit tests, C tests, board checks, ruff, shellcheck
. ~/esp/esp-idf/export.sh
make build                      # firmware for the reference board
make build BOARD=<id>           # or another board (make boards lists them)
make flash PORT=/dev/ttyACM0    # split images: keeps the board's saved settings
make dist                       # every board: what CI and releases build
python3 tools/devctl.py demoshot /tmp/s.png 70   # synthetic load + screenshot
```

Read [AGENTS.md](AGENTS.md) before changing the wire protocol, the console
replies, or the installer. It lists what has to change together.

## Pull requests

- Keep each pull request focused.
- Add or update tests for behavior changes. `tests/test_parser.c` covers the
  firmware parser, `tests/test_states.c` the Wi-Fi link and screen-state
  machines, and `tests/test_agent.py` the agent and tools.
- Firmware changes: build every board (`make dist`) with no new warnings in
  `firmware/main` or `firmware/boards/`, and check the result on hardware
  (include a `devctl.py shot` screenshot for UI changes).
- Update the README or `docs/` when user-visible behavior changes, and add a
  `CHANGELOG.md` entry.
- Run `make check` before submitting.
- Don't copy vendor sample code without a license that allows it. The board
  code under `firmware/boards/` is original for that reason.

## Adding a new device

New boards are welcome. A board is accepted when someone has run it, so the
pull request carries the evidence. The how-to is
[docs/adding-a-device.md](docs/adding-a-device.md).

### 1. Open an issue first

Use the **New device** issue template. It asks for the board, its chip and
display, and whether it passes the checks in
[Will your board work?](docs/adding-a-device.md#will-your-board-work). Someone
will confirm it fits before you spend an evening on a port, or tell you why it
doesn't (most often: a USB-UART bridge instead of the chip's own USB).

### 2. Make the port

One branch, one board. The pull request adds:

- `firmware/boards/<id>/` with everything `tools/check_boards.py` asks for,
  including a `README.md` with the chip, display, pins, where the facts came
  from, the quirks you hit, and a **Verified** section
- a row in `firmware/boards/README.md`
- a UI layout in `firmware/main/ui.c`, only if your canvas size is new
- an entry in `THIRD_PARTY_NOTICES.md` for any new component or source of
  register values
- a `CHANGELOG.md` entry

It doesn't change the existing boards, the default board, or the shared code
beyond a new layout. If your board needs a change in `firmware/main/`, say why
in the pull request; a change there must build and work on every board.

### 3. Show it works

Fill in the device checklist in the pull request template. Attach:

| Evidence | From |
|---|---|
| `devctl.py status` output, showing `board=<id>` | `python3 tools/devctl.py status` |
| A screenshot | `python3 tools/devctl.py demoshot <id>.png 70` |
| A photo of the physical screen showing the same demo | your phone. The screenshot is LVGL's render; only a photo catches a mirrored, inverted or shifted panel |
| Which rotations work, and the refusal for the others | `devctl.py rot cw`, `rot ccw`, `rot none` |
| Wi-Fi: `screen=online reason='via WI-FI'` | `devctl.py status` after `tools/provision.py --ssids ... --host ...` |
| Five cold plug-ins in a row, each answered | unplug, wait 10 s, plug in, `devctl.py status` |
| The installer on a Spark, if you have one | `sudo ./install.sh --board <id>` from your branch |

Remove private data before you attach anything: Wi-Fi names, hostnames,
addresses and MAC addresses appear in `status` output, and `devctl.py shot`
shows the board's title.

No Spark? Say so. A maintainer runs the installer check before merging.

### 4. Review and CI

CI builds every board on every pull request, and a new board directory joins
the build matrix automatically. Expect review to look at the contract
(`firmware/components/board_api/`), locking around LVGL, the license of
anything that came from the vendor, and whether the board README's
**Verified** section matches the evidence.

The reference board stays the default and the one in the main README and
screenshots. Additional boards are listed in
[firmware/boards/README.md](firmware/boards/README.md) and the README's
supported-boards table, with the status their evidence supports.

By participating, you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
