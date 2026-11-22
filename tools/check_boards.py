#!/usr/bin/env python3
"""Check every firmware/boards/<id>/ has what a board needs, without ESP-IDF.

Run by `make check`, so a new-board pull request learns what's missing in
seconds instead of in a firmware build. See docs/adding-a-device.md; a new
board starts with tools/new_board.py.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

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


def check(board, root=ROOT):
    boards = root / "firmware" / "boards"
    d = boards / board
    first_build = " (the first `make build BOARD=...` writes it)"
    problems = [f"missing {f}" + (first_build if f == "dependencies.lock" else "")
                for f in REQUIRED if not (d / f).exists()]
    if any(not p.startswith("missing dependencies.lock") for p in problems):
        return problems

    cfg = (d / "board/include/board_config.h").read_text()
    if define(cfg, "BOARD_ID") != board:
        problems.append(f'board_config.h: BOARD_ID must be "{board}" (the directory name)')
    w, h, mono = define(cfg, "BOARD_UI_W"), define(cfg, "BOARD_UI_H"), define(cfg, "BOARD_DISPLAY_MONO")
    ui_c = (root / "firmware" / "main" / "ui.c").read_text()
    if not (w and w.isdigit() and h and h.isdigit()):
        problems.append("board_config.h: BOARD_UI_W and BOARD_UI_H must be pixel counts")
    elif not re.search(rf"BOARD_UI_W == {w} && BOARD_UI_H == {h}", ui_c):
        problems.append(f"firmware/main/ui.c has no layout for {w}x{h}: add one")
    if mono not in ("0", "1"):
        problems.append("board_config.h: BOARD_DISPLAY_MONO must be 0 or 1")

    sdk = (d / "sdkconfig.defaults").read_text()
    if not re.search(r'^CONFIG_IDF_TARGET="?esp32[a-z0-9]*"?$', sdk, re.M):
        problems.append("sdkconfig.defaults: set CONFIG_IDF_TARGET (the board's chip)")
    want = f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/{board}/partitions.csv"'
    if want not in sdk:
        problems.append(f"sdkconfig.defaults: {want}")
    # The console is the agent's USB transport: the chip's own USB-Serial/JTAG, or for a
    # board whose USB port is a USB-UART bridge, UART0 at the rate the agent and tools open.
    usj = "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y" in sdk
    uart = ("CONFIG_ESP_CONSOLE_UART_CUSTOM=y" in sdk and "CONFIG_ESP_CONSOLE_UART_CUSTOM_NUM_0=y" in sdk
            and "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600" in sdk)
    if not (usj or uart):
        problems.append("sdkconfig.defaults: the console on CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y, or, "
                        "behind a USB-UART bridge, on UART0 at 921600 (CONFIG_ESP_CONSOLE_UART_CUSTOM=y, "
                        "CONFIG_ESP_CONSOLE_UART_CUSTOM_NUM_0=y, CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600); "
                        "see docs/adding-a-device.md")

    cmake = (d / "board/CMakeLists.txt").read_text()
    if not re.search(r"REQUIRES[^)]*\bboard_api\b", cmake):
        problems.append("board/CMakeLists.txt: REQUIRES board_api (the board contract)")

    readme = (d / "README.md").read_text()
    if not re.search(r"^## Verified\s*$", readme, re.M):
        problems.append("README.md: a '## Verified' section "
                        "(what was run on the real board, and what wasn't)")

    index = boards / "README.md"
    if f"`{board}`" not in (index.read_text() if index.exists() else ""):
        problems.append(f"firmware/boards/README.md: list `{board}`")
    if not (root / "assets" / "boards" / f"{board}.png").exists():
        problems.append(f"assets/boards/{board}.png: a demo screenshot "
                        f"(python3 tools/devctl.py demoshot assets/boards/{board}.png 70)")
    return problems


def boards(root=ROOT):
    return sorted(p.name for p in (root / "firmware" / "boards").iterdir() if p.is_dir())


def main():
    names = boards()
    failed = {b: p for b in names if (p := check(b))}
    for b, problems in failed.items():
        for p in problems:
            print(f"firmware/boards/{b}: {p}")
    if failed:
        return 1
    print(f"boards: {len(names)} ok ({', '.join(names)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
