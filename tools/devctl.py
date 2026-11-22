#!/usr/bin/env python3
"""Bench/QA driver for a SparkESP32 display on USB: commands and screenshots.

  devctl.py status                   firmware, settings, heap/stack, last sample
  devctl.py demo [load]              synthetic sample (load 0-100, default 42)
  devctl.py shot out.png             CRC-verified framebuffer dump -> PNG (needs Pillow)
  devctl.py demoshot out.png [load]  demo + shot in one session, so it stays ONLINE
  devctl.py wifi [show|scan|clear|ssids <a,b>|url <url>]
  devctl.py name "<text>"            header title
  devctl.py rot [none|ccw|cw]        screen rotation
  devctl.py send "<console line>"    anything else, raw
  devctl.py log [seconds]            passively capture the console (default 15 s)

Wi-Fi passwords go through provision.py, which never prints them.
Port: --port PATH, $SPARKESP32_PORT, or autodetect (Espressif USB 303a:1001, or a
CH340/CP210x USB-UART bridge).
"""
import argparse
import base64
import os
import re
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sparklink import Link, LinkError, clean_lines, connect, console_quote  # noqa: E402

AREA = re.compile(r"AREA (-?\d+) (-?\d+) (-?\d+) (-?\d+) (\d+) ([0-9a-f]{8})")
B64 = re.compile(r"[A-Za-z0-9+/=]+")


def parse_shot(raw):
    """Reassemble the SHOT-BEGIN..SHOT-END dump. Returns (w, h, fb) or raises LinkError."""
    lines = clean_lines(raw)
    try:
        start = next(i for i, ln in enumerate(lines) if ln.startswith("SHOT-BEGIN"))
    except StopIteration:   # e.g. "ERR lock": report the firmware's own words
        raise LinkError(next((ln for ln in lines if ln.startswith("ERR")), "no SHOT-BEGIN")) from None
    # the board gave up mid-dump (a write timed out): its words, not a CRC or coverage
    # complaint. Base64 never holds a space, so no data line starts "ERR ".
    err = next((ln.strip() for ln in lines[start:] if ln.startswith("ERR ")), None)
    if err:
        raise LinkError(err)
    w, h = map(int, lines[start].split()[1:3])
    fb, covered, i = bytearray(w * h * 2), 0, start + 1
    while i < len(lines) and not lines[i].startswith("SHOT-END"):
        m = AREA.match(lines[i].strip())
        i += 1
        if not m:
            continue
        x1, y1, x2, y2, nbytes = map(int, m.groups()[:5])
        data = bytearray()
        while i < len(lines) and len(data) < nbytes and B64.fullmatch(lines[i].strip() or "!"):
            data += base64.b64decode(lines[i].strip())
            i += 1
        if len(data) != nbytes or format(zlib.crc32(bytes(data)), "08x") != m.group(6):
            raise LinkError(f"strip at {x1},{y1} failed its size/CRC check")
        sw = x2 - x1 + 1
        for row in range(y2 - y1 + 1):
            dst = ((y1 + row) * w + x1) * 2
            fb[dst:dst + sw * 2] = data[row * sw * 2:(row + 1) * sw * 2]
        covered += sw * (y2 - y1 + 1)
    if covered != w * h:
        raise LinkError(f"frame coverage {covered}/{w * h} px")
    return w, h, fb


def save_png(path, w, h, fb):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("shot needs Pillow: pip install pillow  (or: uv run --with pyserial --with pillow ...)")
    Image.frombytes("RGB", (w, h), bytes(fb), "raw", "BGR;16").save(path)   # little-endian RGB565
    print("saved", path)


def shot(port, path, pre=None):
    for attempt in range(1, 4):
        try:
            with connect(port) as link:
                if pre:
                    link.check(pre)
                w, h, fb = parse_shot(link.raw("shot", timeout=90))
            return save_png(path, w, h, fb)
        except LinkError as e:
            print(f"shot attempt {attempt}/3: {e}", file=sys.stderr)
            time.sleep(1)
    sys.exit("shot failed")


def main():
    ap = argparse.ArgumentParser(usage="devctl.py [--port PATH] COMMAND [ARGS...]", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.environ.get("SPARKESP32_PORT"), help=argparse.SUPPRESS)
    ap.add_argument("cmd", help=argparse.SUPPRESS)
    ap.add_argument("args", nargs="*", help=argparse.SUPPRESS)
    a = ap.parse_intermixed_args()
    port, cmd, args = a.port, a.cmd, a.args

    if cmd == "shot":
        return shot(port, args[0] if args else "shot.png")
    if cmd == "demoshot":
        return shot(port, args[0] if args else "shot.png",
                    pre="demo " + (args[1] if len(args) > 1 else "60"))
    if cmd == "log":
        secs = float(args[0]) if args else 15
        with (Link(port) if port else connect()) as link:
            t0, buf = time.monotonic(), b""
            while time.monotonic() - t0 < secs:
                buf += link.ser.read(65536)
        print("\n".join(clean_lines(buf)))
        return 0

    if cmd in ("status", "demo", "rot"):
        line = " ".join([cmd, *args[:1]])
    elif cmd == "wifi":
        if args and args[0] == "pass":
            sys.exit("use tools/provision.py --ssids ... for the password; it never prints it")
        line = "wifi " + " ".join(args[:1] + [console_quote(x) for x in args[1:]])
    elif cmd == "name":
        if len(args) != 1:
            sys.exit('usage: devctl.py name "<text>"')
        line = "name " + console_quote(args[0])
    elif cmd == "send":
        line = " ".join(args)
    else:
        ap.error(f"unknown command '{cmd}'")

    try:
        with connect(port) as link:
            out = link.cmd(line, timeout=30)
    except LinkError as e:
        sys.exit(f"error: {e}")
    print("\n".join(out))
    return 1 if out and out[-1].startswith("ERR") else 0


if __name__ == "__main__":
    sys.exit(main())
