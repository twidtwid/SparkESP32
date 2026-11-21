#!/usr/bin/env python3
"""Check every firmware/boards/<id>/ has what a board needs, without ESP-IDF.

Run by `make check`, so a new-board pull request learns what's missing in
seconds instead of in a firmware build. See docs/adding-a-device.md.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BOARDS = ROOT / "firmware" / "boards"
UI_C = (ROOT / "firmware" / "main" / "ui.c").read_text()
INDEX = (BOARDS / "README.md").read_text() if (BOARDS / "README.md").exists() else ""

REQUIRED = [
    "board/CMakeLists.txt",
    "board/include/board_config.h",
    "board/idf_component.yml",
    "sdkconfig.defaults",
    "partitions.csv",
    "dependencies.lock",
    "README.md",
]


def define(text, name):
    m = re.search(rf"^#define\s+{name}\s+(\S+)", text, re.M)
    return m.group(1).strip('"') if m else None


def check(board):
    d = BOARDS / board
    problems = [f"missing {f}" for f in REQUIRED if not (d / f).exists()]
    if problems:
        return problems

    cfg = (d / "board/include/board_config.h").read_text()
    if define(cfg, "BOARD_ID") != board:
        problems.append(f'board_config.h: BOARD_ID must be "{board}" (the directory name)')
    w, h, mono = define(cfg, "BOARD_UI_W"), define(cfg, "BOARD_UI_H"), define(cfg, "BOARD_DISPLAY_MONO")
    if not (w and w.isdigit() and h and h.isdigit()):
        problems.append("board_config.h: BOARD_UI_W and BOARD_UI_H must be pixel counts")
    elif not re.search(rf"BOARD_UI_W == {w} && BOARD_UI_H == {h}", UI_C):
        problems.append(f"firmware/main/ui.c has no layout for {w}x{h}: add one")
    if mono not in ("0", "1"):
        problems.append("board_config.h: BOARD_DISPLAY_MONO must be 0 or 1")

    sdk = (d / "sdkconfig.defaults").read_text()
    if not re.search(r'^CONFIG_IDF_TARGET="?esp32[a-z0-9]*"?$', sdk, re.M):
        problems.append("sdkconfig.defaults: set CONFIG_IDF_TARGET (the board's chip)")
    want = f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/{board}/partitions.csv"'
    if want not in sdk:
        problems.append(f"sdkconfig.defaults: {want}")
    if "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y" not in sdk:
        problems.append("sdkconfig.defaults: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y (the agent and tools "
                        "find the display by the USB-Serial/JTAG ID; see docs/adding-a-device.md)")

    cmake = (d / "board/CMakeLists.txt").read_text()
    if not re.search(r"REQUIRES[^)]*\bboard_api\b", cmake):
        problems.append("board/CMakeLists.txt: REQUIRES board_api (the board contract)")
    if f"`{board}`" not in INDEX:
        problems.append(f"firmware/boards/README.md: list `{board}`")
    return problems


def main():
    boards = sorted(p.name for p in BOARDS.iterdir() if p.is_dir())
    failed = {b: p for b in boards if (p := check(b))}
    for b, problems in failed.items():
        for p in problems:
            print(f"firmware/boards/{b}: {p}")
    if failed:
        return 1
    print(f"boards: {len(boards)} ok ({', '.join(boards)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
