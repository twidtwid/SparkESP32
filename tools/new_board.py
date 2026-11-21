#!/usr/bin/env python3
"""Start a new board from the closest existing one.

    tools/new_board.py acme-esp32s3-ips-3.5 --from lcdwiki-esp32s3-tft-2.8
    tools/new_board.py acme-esp32c3-oled-1.3 --from lcdwiki-esp32s3-tft-2.8 --chip esp32c3

Copies firmware/boards/<from>/ to firmware/boards/<id>/ and does the mechanical
part of a port: sets BOARD_ID, points sdkconfig.defaults at the new board's own
partition table (and chip, with --chip), drops the copied dependencies.lock (the
first build writes the new board's own), replaces the README with a skeleton to
fill in, and lists the board in firmware/boards/README.md.

Then continue with docs/adding-a-device.md, step 4. tools/check_boards.py (part of
`make check`) lists what the new board still needs.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
# <vendor>-<chip>-<display>-<size>, lowercase: waveshare-esp32c6-amoled-2.16
ID_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9.]+){3,}$")
# chips with Wi-Fi and the native USB-Serial/JTAG the agent finds a display by
CHIPS = ("esp32c3", "esp32c5", "esp32c6", "esp32c61", "esp32s3")

README = """\
# {id}

<!-- Product page and docs, as links. One line on what's different about this board. -->

| | |
|---|---|
| Chip | {chip}: flash size, PSRAM |
| Display | size, resolution, color or black and white, controller, bus |
| USB | native USB-Serial/JTAG (303a:1001): the console and the agent's USB feed |
| Unused | touch, audio, sensors... |

## Pins

| Signal | GPIO |
|---|---|
| | |

## How the display works here

<!-- The driver, where the init sequence came from (and its license), rotation,
     anything that surprised you. -->

## Verified

Nothing yet. Run the Verify table in docs/adding-a-device.md on the real board,
including a look at the physical screen, and record here what passed and what
wasn't checked.
"""


class BoardError(Exception):
    pass


def create(root, new, src, chip=None):
    """Copy firmware/boards/<src> to firmware/boards/<new> under `root`, and adapt it."""
    boards = root / "firmware" / "boards"
    s, d = boards / src, boards / new
    if not ID_RE.match(new):
        raise BoardError(f"'{new}' isn't a board id: <vendor>-<chip>-<display>-<size>, lowercase "
                         "(e.g. waveshare-esp32c6-amoled-2.16)")
    if not s.is_dir():
        known = ", ".join(sorted(p.name for p in boards.iterdir() if p.is_dir()))
        raise BoardError(f"no board '{src}' to start from; one of: {known}")
    if d.exists():
        raise BoardError(f"firmware/boards/{new} already exists")
    if chip and chip not in CHIPS:
        raise BoardError(f"--chip {chip}: SparkESP32 needs Wi-Fi and USB-Serial/JTAG: "
                         f"one of {', '.join(CHIPS)}")

    files = [p for p in s.rglob("*") if p.is_file() and p.name != "dependencies.lock"]
    for f in files:                          # copy by hand: no build output, nothing hidden
        out = d / f.relative_to(s)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(f.read_bytes())

    cfg = d / "board" / "include" / "board_config.h"
    text = cfg.read_text()
    text = re.sub(r'(#define\s+BOARD_ID\s+)"[^"]*"', rf'\g<1>"{new}"', text)
    text = re.sub(r"(#pragma once\n)(//[^\n]*\n)+",
                  rf"\g<1>// {new}: describe the board and its screen in a line or two.\n", text)
    cfg.write_text(text)

    sdk = d / "sdkconfig.defaults"
    text = sdk.read_text().replace(f"boards/{src}/partitions.csv", f"boards/{new}/partitions.csv")
    text = re.sub(r"\A#[^\n]*\n", f"# {new}: chip, flash and PSRAM for this board\n", text)
    if chip:
        text = re.sub(r'^CONFIG_IDF_TARGET=.*$', f'CONFIG_IDF_TARGET="{chip}"', text, flags=re.M)
    sdk.write_text(text)
    target = re.search(r'^CONFIG_IDF_TARGET="?([a-z0-9]+)"?$', text, re.M).group(1)

    (d / "README.md").write_text(README.format(id=new, chip=target.upper().replace("ESP32", "ESP32-")))

    index = boards / "README.md"
    rows = index.read_text().splitlines(keepends=True)
    last = max(i for i, line in enumerate(rows) if line.startswith("| `"))
    rows.insert(last + 1, f"| `{new}` | {target.upper().replace('ESP32', 'ESP32-')} | (describe the display) "
                          f"| In progress: not yet run on the real board |\n")
    index.write_text("".join(rows))
    return d, target


def main(argv=None):
    ap = argparse.ArgumentParser(description="Start a new SparkESP32 board from the closest existing one.")
    ap.add_argument("id", help="the new board's id: <vendor>-<chip>-<display>-<size>, lowercase")
    ap.add_argument("--from", dest="src", required=True, help="the existing board to copy")
    ap.add_argument("--chip", help="the new board's chip, if it differs (" + ", ".join(CHIPS) + ")")
    a = ap.parse_args(argv)
    try:
        d, target = create(ROOT, a.id, a.src, a.chip)
    except BoardError as e:
        sys.exit(f"new_board: {e}")
    rel = d.relative_to(ROOT)
    print(f"created {rel}/ from {a.src} ({target}). Next (docs/adding-a-device.md):")
    print(f"  1. {rel}/board/include/board_config.h: BOARD_UI_W/H and BOARD_DISPLAY_MONO for your screen")
    chip_note = "" if a.chip else "; the chip too, if it differs"
    print(f"  2. {rel}/sdkconfig.defaults: flash size and PSRAM{chip_note}")
    print(f"  3. {rel}/board/board.c and display.c: your pins and panel driver")
    print("  4. firmware/main/ui.c: a layout, if your canvas size is new")
    print(f"  5. make build BOARD={a.id}, then the Verify table; fill in {rel}/README.md")
    print(f"  6. a demo screenshot: python3 tools/devctl.py demoshot assets/boards/{a.id}.png 70")
    print("  python3 tools/check_boards.py lists what's still missing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
