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

New boards are welcome. A board is accepted once someone has run it on the real
hardware, so the pull request carries the evidence. The full how-to is
[docs/adding-a-device.md](docs/adding-a-device.md); this is the path from idea to merge.

### 1. Open an issue

Use the **New device** issue template. It asks for the board, its chip and
display, and whether it passes
[Will your board work?](docs/adding-a-device.md#will-your-board-work). Someone
will confirm it fits before you spend an evening on it, or tell you why it
doesn't (most often: a chip SparkESP32 doesn't build for, or a USB-UART bridge the
agent doesn't know yet).

### 2. Start the board

On a branch named `board/<id>`, with `<id>` as `<vendor>-<chip>-<display>-<size>`:

```sh
python3 tools/new_board.py <id> --from <closest board> [--chip esp32c3]
```

That copies the closest board, renames it, points it at its own partition table,
writes a README skeleton and lists it in `firmware/boards/README.md`. Then follow
the guide: pins and panel driver in `firmware/boards/<id>/board/`, a layout in
`firmware/main/ui.c` if your screen size is new.

### 3. Make it work on the board, and show it

Run the guide's [Verify](docs/adding-a-device.md#8-verify) table on the real board
and attach the results to the pull request:

| Evidence | From |
|---|---|
| `devctl.py status` output, showing `board=<id>` | `python3 tools/devctl.py status` |
| The demo screenshot, committed as `assets/boards/<id>.png` | `python3 tools/devctl.py demoshot assets/boards/<id>.png 70` |
| A photo of the physical screen showing the same demo | your phone. The screenshot is LVGL's render; only a photo catches a dark, mirrored, inverted or shifted panel |
| Which rotations work, and the refusal for the others | `devctl.py rot cw`, `rot ccw`, `rot none` |
| Wi-Fi: `screen=online reason='via WI-FI'` | `devctl.py status` after `tools/provision.py --ssids ... --host ...` |
| Five cold plug-ins in a row, each answered | unplug, wait 10 s, plug in, `devctl.py status` |
| The installer on a Spark, if you have one | `sudo ./install.sh --board <id>` from your branch |

Remove private data before you attach anything: Wi-Fi names, hostnames,
addresses and MAC addresses appear in `status` output, and screenshots show the
board's title (`demoshot` uses the neutral `spark-demo`). No Spark? Say so; a
maintainer runs the installer check before merging.

### 4. Write it up, with credit

The pull request adds, besides the code:

- `firmware/boards/<id>/README.md`: chip, display, pins, how the display is
  driven, the quirks you hit, and a **Verified** section that says what was run on
  the real board and what wasn't
- `assets/boards/<id>.png`: the demo screenshot (for a 1-bit panel, what the
  panel shows)
- a row in the README's supported-boards table and in `firmware/boards/README.md`
- credit, with links, for everything you built on: the vendor's docs and
  schematics, any example code or register values you used and its license, other
  projects' board definitions, and any new component, in your board README and in
  `THIRD_PARTY_NOTICES.md`. Don't copy code whose license doesn't allow it; facts
  like pins and register values are fine, credited
- a `CHANGELOG.md` entry

It doesn't change the existing boards or the default board. If your board needs a
change in `firmware/main/`, say why in the pull request; a change there must build
and work on every board.

### 5. Open the pull request

`make check` passes (it runs `tools/check_boards.py`, which names anything
missing), and `make dist` builds every board with no new warnings. Open the pull
request and fill in the template's **New device** section with the evidence from
step 3.

CI builds every board on every pull request; a new board directory joins the build
matrix automatically. Review looks at the contract
(`firmware/components/board_api/`), locking around LVGL, the license and credit of
anything from the vendor, and whether the **Verified** section matches the evidence.

The C6 AMOLED stays the default board and the one in the README's hero image.
Other boards are listed with the status their evidence supports.

By participating, you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
