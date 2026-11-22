#!/usr/bin/env python3
"""Host tests for multi-board support: tools/check_boards.py, tools/new_board.py,
and the installer's board detection (install.sh's own functions, run in bash
with a fake esptool). Stdlib only.

    python3 tests/test_boards.py
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import check_boards  # noqa: E402
import new_board  # noqa: E402

C6 = "waveshare-esp32c6-amoled-2.16"
TFT = "lcdwiki-esp32s3-tft-2.8"
RLCD = "waveshare-esp32s3-rlcd-4.2"
EPD = "elecrow-esp32s3-epaper-5.79"


def scratch_tree():
    """The parts of the repo the board tools read, copied into a temp dir."""
    tmp = pathlib.Path(tempfile.mkdtemp())
    shutil.copytree(ROOT / "firmware" / "boards", tmp / "firmware" / "boards")
    (tmp / "firmware" / "main").mkdir(parents=True)
    shutil.copy(ROOT / "firmware" / "main" / "ui.c", tmp / "firmware" / "main" / "ui.c")
    shutil.copytree(ROOT / "assets" / "boards", tmp / "assets" / "boards")
    return tmp


class RealBoards(unittest.TestCase):
    def test_every_board_passes(self):
        names = check_boards.boards()
        self.assertIn(C6, names)
        for b in names:
            self.assertEqual(check_boards.check(b), [], b)


class CheckBoards(unittest.TestCase):
    def setUp(self):
        self.root = scratch_tree()
        self.addCleanup(shutil.rmtree, self.root)
        self.d = self.root / "firmware" / "boards" / TFT

    def problems(self):
        return "\n".join(check_boards.check(TFT, self.root))

    def test_wrong_board_id(self):
        cfg = self.d / "board" / "include" / "board_config.h"
        cfg.write_text(cfg.read_text().replace(f'"{TFT}"', '"something-else"'))
        self.assertIn("BOARD_ID must be", self.problems())

    def test_needs_a_verified_section(self):
        readme = self.d / "README.md"
        readme.write_text(readme.read_text().replace("## Verified", "## Notes"))
        self.assertIn("'## Verified' section", self.problems())

    def test_needs_a_screenshot(self):
        (self.root / "assets" / "boards" / f"{TFT}.png").unlink()
        self.assertIn(f"assets/boards/{TFT}.png", self.problems())

    def test_console_on_native_usb_or_a_uart_at_the_tools_rate(self):
        sdk = self.d / "sdkconfig.defaults"
        native = sdk.read_text()
        sdk.write_text(native.replace("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y",
                                      "CONFIG_ESP_CONSOLE_UART_DEFAULT=y"))
        self.assertIn("the console on", self.problems())          # UART, but at the default 115200
        bridge = (self.root / "firmware" / "boards" / EPD / "sdkconfig.defaults").read_text()
        self.assertIn("CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600", bridge)
        sdk.write_text(native.replace("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y",
                                      "CONFIG_ESP_CONSOLE_UART_CUSTOM=y\nCONFIG_ESP_CONSOLE_UART_CUSTOM_NUM_0=y\n"
                                      "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600"))
        self.assertNotIn("the console on", self.problems())

    def test_needs_a_layout_for_its_size(self):
        cfg = self.d / "board" / "include" / "board_config.h"
        cfg.write_text(re.sub(r"BOARD_UI_W\s+\d+", "BOARD_UI_W          123", cfg.read_text()))
        self.assertIn("no layout for 123x240", self.problems())


class NewBoard(unittest.TestCase):
    NEW = "acme-esp32c3-oled-1.3"

    def setUp(self):
        self.root = scratch_tree()
        self.addCleanup(shutil.rmtree, self.root)

    def test_scaffold_needs_only_a_build_and_a_screenshot(self):
        d, target = new_board.create(self.root, self.NEW, TFT, chip="esp32c3")
        self.assertEqual(target, "esp32c3")
        cfg = (d / "board" / "include" / "board_config.h").read_text()
        self.assertIn(f'#define BOARD_ID            "{self.NEW}"', cfg)
        self.assertNotIn("LCDWiki", cfg)                       # the old board's description is gone
        sdk = (d / "sdkconfig.defaults").read_text()
        self.assertIn(f'"boards/{self.NEW}/partitions.csv"', sdk)
        self.assertIn('CONFIG_IDF_TARGET="esp32c3"', sdk)
        self.assertFalse((d / "dependencies.lock").exists())
        self.assertIn(f"`{self.NEW}`", (self.root / "firmware" / "boards" / "README.md").read_text())
        problems = check_boards.check(self.NEW, self.root)
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(problems[0].startswith("missing dependencies.lock"))
        self.assertIn(f"assets/boards/{self.NEW}.png", problems[1])

    def test_keeps_the_source_chip_without_chip(self):
        d, target = new_board.create(self.root, "acme-esp32s3-ips-3.5", TFT)
        self.assertEqual(target, "esp32s3")

    def test_refuses_bad_input(self):
        for args in [("Acme Board", TFT), ("acme-esp32s3-ips", TFT), (TFT, C6),
                     (self.NEW, "no-such-board-1.0"), (self.NEW, TFT, "esp32")]:
            with self.subTest(args=args), self.assertRaises(new_board.BoardError):
                new_board.create(self.root, *args)


def installer_functions(*names):
    """Pull the named shell functions out of install.sh, as written."""
    lines = (ROOT / "install.sh").read_text().splitlines()
    out = []
    for name in names:
        start = next(i for i, ln in enumerate(lines) if ln.startswith(f"{name}() {{"))
        for ln in lines[start:]:
            out.append(ln)
            code = ln.split(" #")[0].rstrip()
            if code == "}" or code.endswith("; }"):
                break
    return "\n".join(out)


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class InstallerBoardDetection(unittest.TestCase):
    FUNCS = installer_functions("boards", "board_target", "status_board", "pick_board")
    NEW_FW = "fw=sparkesp32 version=0.1.0 idf=v5.5.6 board={}"
    OLD_FW = "fw=sparkesp32 version=0.1.0 idf=v5.5.6"           # before boards: no board=

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.fakepy = os.path.join(self.tmp, "fakepy")          # stands in for `python -m esptool chip_id`
        with open(self.fakepy, "w") as f:
            f.write('#!/bin/sh\n'
                    '[ -n "$FAKE_CHIP" ] && echo "Chip is $FAKE_CHIP (QFN56) (revision v0.2)"\n'
                    'exit 0\n')
        os.chmod(self.fakepy, 0o755)

    def pick(self, status, board="", chip=""):
        script = "\n".join([
            "set -u",
            f"HERE='{ROOT}'; PY='{self.fakepy}'; PORT=/dev/fake",
            'die() { echo "DIE: $*"; exit 1; }',
            self.FUNCS,
            f"DEV_STATUS='{status}'; BOARD='{board}'",
            'pick_board; echo "BOARD=$BOARD"',
        ])
        r = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                           env={**os.environ, "FAKE_CHIP": chip}, timeout=20)
        return r.stdout.strip()

    def unclaimed(self, status, bridge, board="", port_given=""):
        script = "\n".join([
            "set -u",
            installer_functions("board_version", "unclaimed_bridge"),
            f"sparklink() {{ echo {int(bridge)}; }}",           # stands in for sparklink.is_bridge()
            f"DEV_STATUS='{status}'; BOARD='{board}'; PORT=/dev/ttyUSB0; PORT_GIVEN='{port_given}'",
            'if unclaimed_bridge; then echo skip; else echo use; fi',
        ])
        r = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=20)
        return r.stdout.strip()

    def test_an_unknown_bridge_is_left_alone_unless_named(self):
        self.assertEqual(self.unclaimed("", bridge=True), "skip")            # some other CH340 gadget
        self.assertEqual(self.unclaimed("", bridge=True, board=EPD), "use")  # --board: flash a new display
        self.assertEqual(self.unclaimed("", bridge=True, port_given="/dev/ttyUSB0"), "use")   # --port
        self.assertEqual(self.unclaimed(self.NEW_FW.format(EPD), bridge=True), "use")   # runs SparkESP32
        self.assertEqual(self.unclaimed("", bridge=False), "use")           # native USB: as before

    def test_firmware_prints_the_field_the_installer_reads(self):
        src = (ROOT / "firmware" / "main" / "console_cmds.c").read_text()
        self.assertIn('"fw=sparkesp32 version=%s idf=%s board=%s', src)

    def test_board_from_the_handshake(self):
        self.assertEqual(self.pick(self.NEW_FW.format(RLCD), chip="ESP32-S3"), f"BOARD={RLCD}")

    def test_old_firmware_falls_back_to_the_chip(self):
        self.assertEqual(self.pick(self.OLD_FW, chip="ESP32-C6"), f"BOARD={C6}")

    def test_ambiguous_chip_lists_only_its_boards(self):
        out = self.pick(self.OLD_FW, chip="ESP32-S3")
        self.assertTrue(out.startswith("DIE:"), out)
        self.assertIn(TFT, out)
        self.assertIn(RLCD, out)
        self.assertNotIn(C6, out)

    def test_board_flag_wins(self):
        self.assertEqual(self.pick(self.NEW_FW.format(RLCD), board=TFT), f"BOARD={TFT}")

    def test_unknown_board_flag(self):
        self.assertIn("DIE: unknown board", self.pick(self.OLD_FW, board="nope-esp32-x-1"))

    def test_unidentifiable(self):
        self.assertIn("DIE: couldn't identify", self.pick("", chip=""))


if __name__ == "__main__":
    unittest.main(verbosity=1)
