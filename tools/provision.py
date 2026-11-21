#!/usr/bin/env python3
"""Configure a SparkESP32 display over USB: name, agent host, Wi-Fi, rotation.

Settings live in the board's flash and survive reboots and firmware updates.
Run on whatever machine the board is plugged into (the Spark, a Mac, a Linux box).

    # name + agent host (the Spark's mDNS name or IP); no Wi-Fi yet
    tools/provision.py --name "Lab Spark" --host spark-1234.local

    # join Wi-Fi: prompts for the password (hidden); try SSIDs in order
    tools/provision.py --ssids "HomeNet,HomeNet-5G"

    # non-interactive: password from an environment variable or stdin
    WIFI_PSK=... tools/provision.py --ssids HomeNet --password-env WIFI_PSK
    some-secret-tool | tools/provision.py --ssids HomeNet --password-stdin

The password is never printed or logged. The console echoes what it's sent, so
for that one command only the board's OK/ERR verdict is kept. Exits non-zero
on the first command the board rejects.
"""
import argparse
import getpass
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sparklink import LinkError, connect, console_quote  # noqa: E402

AGENT_PORT = 8787


def read_password(a):
    if a.open:
        return ""
    if a.password_env:
        pw = os.environ.get(a.password_env)
        if pw is None:
            sys.exit(f"--password-env: ${a.password_env} is not set")
    elif a.password_stdin:
        pw = sys.stdin.readline().rstrip("\r\n")
    elif sys.stdin.isatty():
        pw = getpass.getpass(f"Wi-Fi password for {a.ssids.split(',')[0]} (hidden): ")
    else:
        sys.exit("no password source: run in a terminal, or use --password-env VAR, "
                 "--password-stdin, or --open for a network without a password")
    if not 8 <= len(pw) <= 63:
        sys.exit("Wi-Fi passwords are 8-63 characters (WPA2/WPA3 passphrase); "
                 "use --open for a network without one")
    return pw


def main():
    ap = argparse.ArgumentParser(description="Configure a SparkESP32 display over USB.",
                                 epilog="Run with no options to just print the board's state.")
    ap.add_argument("--port", help="serial port (default: autodetect the display)")
    ap.add_argument("--name", help='fixed header title, e.g. "Lab Spark" (max 24 chars); '
                                   '"" = show the Spark\'s hostname (the default)')
    where = ap.add_mutually_exclusive_group()
    where.add_argument("--host", help=f"Spark hostname or IP for Wi-Fi mode; sets the agent URL to "
                                      f"http://HOST:{AGENT_PORT}/stat")
    where.add_argument("--url", help="full agent URL instead of --host (http:// only)")
    ap.add_argument("--ssids", help="Wi-Fi network name(s), comma-separated, tried in order (max 4)")
    pw = ap.add_mutually_exclusive_group()
    pw.add_argument("--password-env", metavar="VAR", help="read the Wi-Fi password from $VAR")
    pw.add_argument("--password-stdin", action="store_true", help="read it from the first stdin line")
    pw.add_argument("--open", action="store_true", help="the network has no password")
    ap.add_argument("--rotation", choices=["none", "ccw", "cw"],
                    help="screen rotation (default cw: the board's upright position)")
    ap.add_argument("--clear-wifi", action="store_true", help="forget SSIDs, password and URL")
    a = ap.parse_args()

    if (a.password_env or a.password_stdin or a.open) and not a.ssids:
        ap.error("a password option needs --ssids")
    if a.clear_wifi and (a.ssids or a.host or a.url):
        ap.error("--clear-wifi can't be combined with --ssids/--host/--url")
    if a.name is not None and not (len(a.name) <= 24 and a.name.isprintable() and a.name.isascii()):
        ap.error("--name must be at most 24 printable ASCII characters")

    password = read_password(a) if a.ssids else None
    url = a.url or (f"http://{a.host}:{AGENT_PORT}/stat" if a.host else None)

    try:
        with connect(a.port) as link:
            print(f"display: {link.port}")
            if a.clear_wifi:
                link.check("wifi clear")
                print("wifi:    cleared")
            if a.name is not None:
                link.check("name " + console_quote(a.name))
                print(f"name:    {a.name or '(the Spark hostname, from the agent)'}")
            if a.rotation:
                link.check("rot " + a.rotation)
                print(f"rot:     {a.rotation}")
            if url:
                link.check("wifi url " + console_quote(url))
                print(f"url:     {url}")
            if a.ssids:
                link.check("wifi ssids " + console_quote(a.ssids))
                print(f"ssids:   {a.ssids}")
                # echo_safe=False: only the OK/ERR verdict survives, never the echo
                link.check("wifi pass " + console_quote(password), echo_safe=False)
                print("pass:    set" if password else "pass:    (open network)")
            print()
            for line in link.cmd("wifi show"):
                if line not in ("NET-BEGIN", "NET-END"):
                    print(line)
    except LinkError as e:
        sys.exit(f"error: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
