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

    def test_console_on_native_usb_or_the_default_uart(self):
        sdk = self.d / "sdkconfig.defaults"
        native = sdk.read_text()
        sdk.write_text(native.replace("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", "CONFIG_ESP_CONSOLE_NONE=y"))
        self.assertIn("the console on", self.problems())
        sdk.write_text(native.replace("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y",       # UART1, custom pins
                                      "CONFIG_ESP_CONSOLE_UART_CUSTOM=y\nCONFIG_ESP_CONSOLE_UART_CUSTOM_NUM_1=y"))
        self.assertIn("the console on", self.problems())
        bridge = (self.root / "firmware" / "boards" / EPD / "sdkconfig.defaults").read_text()
        self.assertIn("CONFIG_ESP_CONSOLE_UART_DEFAULT=y", bridge)
        sdk.write_text(native.replace("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y",
                                      "CONFIG_ESP_CONSOLE_UART_DEFAULT=y"))
        self.assertNotIn("the console on", self.problems())

    def test_needs_a_layout_for_its_size(self):
        cfg = self.d / "board" / "include" / "board_config.h"
        cfg.write_text(re.sub(r"BOARD_UI_W\s+\d+", "BOARD_UI_W          123", cfg.read_text()))
        self.assertIn("no layout for 123x240", self.problems())


class ReferenceLayout(unittest.TestCase):
    """AGENTS.md: the C6's 480x480 layout must stay the same. Its values, as of 1.2.0."""

    FROZEN = {
        "w": "480", "h": "480", "margin": "22", "title_y": "14", "title_w": "280",
        "title_font": "&lv_font_montserrat_28", "status_y": "22", "reason_y": "46", "dot": "14",
        "ring_r": "58", "ring_w": "10", "ring_cx": "{92,240,388}", "ring_cy": "{150,150,150}",
        "ring_val_dy": "-6", "ring_sub_dy": "16", "ring_title_dy": "23",
        "ring_val_font": "&lv_font_montserrat_28", "ring_sub_font": "&lv_font_montserrat_14",
        "gpu_mhz_fmt": '"%dMHz"', "cores_y": "250", "cells_y": "276", "cells_h": "30",
        "cells_gap": "3", "tile_x": "{16,248}", "tile_y": "{322,322}", "tile_w": "216",
        "tile_h": "142", "tile_radius": "14", "tile_pad": "16", "tile_label_y": "14",
        "tile_value_y": "-12", "tile_label_font": "&lv_font_montserrat_16",
        "tile_value_font": "&lv_font_montserrat_48",
    }

    def test_the_c6_layout_is_unchanged(self):
        src = (ROOT / "firmware" / "main" / "ui.c").read_text()
        block = re.search(r"#if BOARD_UI_W == 480 && BOARD_UI_H == 480.*?\n(.*?)\n#elif", src, re.S).group(1)
        found = {k: re.sub(r"\s+", "", v) for k, v in
                 re.findall(r"\.(\w+)\s*=\s*(\{[^}]*\}|\"[^\"]*\"|&?[\w-]+)", block)}
        self.assertEqual(found, self.FROZEN)


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

    def test_a_path_from_the_device_isnt_a_board(self):
        self.assertIn("DIE: unknown board", self.pick(self.NEW_FW.format("../boards/" + C6)))

    def test_unknown_board_flag(self):
        self.assertIn("DIE: unknown board", self.pick(self.OLD_FW, board="nope-esp32-x-1"))

    def test_unidentifiable(self):
        self.assertIn("DIE: couldn't identify", self.pick("", chip=""))


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class InstallerFlashArgs(unittest.TestCase):
    """flash_args_ok: esptool gets only flash settings and the bundle's three images."""
    GOOD = ("--flash_mode dio --flash_freq 80m --flash_size 16MB\n0x0 bootloader.bin\n"
            "0x10000 sparkesp32.bin\n0x8000 partition-table.bin\n")

    def ok(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            f = pathlib.Path(tmp, "flash_args")
            f.write_text(text)
            script = installer_functions("flash_args_ok") + f"\nflash_args_ok '{f}'"
            r = subprocess.run(["bash", "-c", script], timeout=20)
            return r.returncode == 0

    def test_a_real_bundles_flash_args(self):
        self.assertTrue(self.ok(self.GOOD))

    def test_anything_else_is_refused(self):
        for bad in ["0x200000 /etc/sparkesp32/sparkline.env\n", "0x0 ../bootloader.bin\n",
                    "--erase-all\n", "0x0 bootloader.bin --no-stub\n", ""]:
            with self.subTest(bad=bad):
                self.assertFalse(self.ok(self.GOOD + bad if bad else bad))


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class InstallerFlashConsent(unittest.TestCase):
    """may_flash: whether install.sh flashes a device that doesn't run SparkESP32."""
    THIS, ANOTHER = "303a:1001:F4:12:FA:00:00:01", "303a:1001:F4:12:FA:00:00:02"

    def ask(self, saved="", tty=False, answer="", port_given="", board="", installed=False):
        with tempfile.TemporaryDirectory() as tmp:
            env, unit = os.path.join(tmp, "display.env"), os.path.join(tmp, "sparkesp32.service")
            if installed:
                pathlib.Path(unit).write_text("")
            if saved:
                pathlib.Path(env).write_text(f"SPARKESP32_DISPLAY={saved}\n")
            script = "\n".join([
                "set -u",
                installer_functions("saved_display", "may_flash"),
                f"interactive() {{ [ {int(tty)} = 1 ]; }}",
                f"usb_id() {{ echo '{self.THIS}'; }}",              # the device on $PORT
                f"DISPLAY_ENV='{env}'; UNIT='{unit}'; PORT=/dev/ttyACM0; PORT_GIVEN='{port_given}'; "
                f"BOARD='{board}'",
                "if may_flash; then echo flash; else echo leave; fi",
            ])
            r = subprocess.run(["bash", "-c", script], input=answer, capture_output=True, text=True,
                               timeout=20)
            return r.stdout.strip()

    def test_a_named_device_is_flashed(self):
        self.assertEqual(self.ask(saved=self.ANOTHER, port_given="/dev/ttyACM0"), "flash")
        self.assertEqual(self.ask(saved=self.ANOTHER, board=C6), "flash")

    def test_with_nobody_to_ask_only_another_display_stops_it(self):
        self.assertEqual(self.ask(), "flash")                        # --yes on a first install
        self.assertEqual(self.ask(saved=self.THIS), "flash")         # the display itself, wiped since
        self.assertEqual(self.ask(saved=self.ANOTHER), "leave")      # an update, another board attached

    def test_an_update_with_nothing_saved_leaves_it_alone(self):
        self.assertEqual(self.ask(installed=True), "leave")          # display on Wi-Fi, a project on USB
        self.assertEqual(self.ask(installed=True, saved=self.THIS), "flash")
        self.assertEqual(self.ask(installed=True, board=C6), "flash")

    def test_at_a_terminal_it_asks(self):
        self.assertEqual(self.ask(tty=True, answer="\n"), "flash")   # a new display: Enter goes ahead
        self.assertEqual(self.ask(tty=True, answer="n\n"), "leave")
        # not the display set up here before: Enter doesn't flash it
        self.assertEqual(self.ask(tty=True, saved=self.ANOTHER, answer="\n"), "leave")
        self.assertEqual(self.ask(tty=True, saved=self.ANOTHER, answer="yes\n"), "flash")


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class InstallerUnitFile(unittest.TestCase):
    """The systemd unit install.sh writes: who may talk to the agent."""

    def unit(self, allow):
        script = "\n".join([f"HERE='{ROOT}'; ALLOW='{allow}'", installer_functions("unit_file"), "unit_file"])
        return subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=20).stdout

    def test_limited_to_this_host_and_the_subnets(self):
        unit = self.unit("192.168.1.0/24 10.9.0.0/24")
        self.assertIn("\nIPAddressDeny=any\nIPAddressAllow=localhost 192.168.1.0/24 10.9.0.0/24\n", unit)
        self.assertNotIn("@LAN@", unit)

    def test_no_known_subnet_means_no_limit_rather_than_no_access(self):
        unit = self.unit("")
        self.assertIn("\nExecStart=", unit)
        self.assertNotRegex(unit, r"(?m)^IPAddress|@LAN@")     # a Deny without its Allow shuts everyone out


if __name__ == "__main__":
    unittest.main(verbosity=1)
