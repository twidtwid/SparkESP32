"""Shared serial plumbing for the host tools (provision.py, devctl.py).

Finds the display by USB ID, proves it's running SparkESP32 firmware with the
`status` handshake, and runs console commands until their terminating line, so
a tool never acts on some other serial gadget and never acts on a half-read
reply. Works on Linux (/dev/ttyACM*, /dev/ttyUSB*) and macOS (/dev/cu.usbmodem*,
/dev/cu.usbserial-*).
"""
import re
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is missing: pip install pyserial  (or: uv run --with pyserial ...)")

# A display's USB serial port: the chip's own USB-Serial/JTAG on most boards, or a
# USB-UART bridge in front of the chip on some (the Elecrow e-paper's CH340).
# agent/sparkesp32_agent.py keeps the same lists (tests/test_agent.py checks).
NATIVE_IDS = {(0x303A, 0x1001)}              # Espressif USB-Serial/JTAG
BRIDGE_IDS = {(0x1A86, 0x7523),              # WCH CH340
              (0x1A86, 0x55D3),              # WCH CH343
              (0x1A86, 0x55D4),              # WCH CH9102
              (0x10C4, 0xEA60)}              # Silicon Labs CP210x
BAUD = 115200       # native USB ignores it; a board behind a bridge runs its console at this rate
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
ESP_LOG = re.compile(r"[EWIDV] \(\d+\) ")   # "I (4378) tag: ...": the board logging, never a reply
PROMPT = "spark>"

# Multi-line replies end with a marker line, keyed by the command's first two
# words; an "ERR ..." line ends them early (`shot` -> "ERR lock"). Every other
# command's reply is its OK/ERR verdict line.
_ENDS = {"status": "STATUS-END", "shot": "SHOT-END", "wifi": "NET-END",
         "wifi show": "NET-END", "wifi scan": "SCAN-END"}
_VERDICT = r"^(OK|ERR)\b"


class LinkError(RuntimeError):
    pass


def find_ports():
    """Every attached port a display could be on: native USB first, then USB-UART
    bridges; on macOS the cu.* device of each, not its tty.* twin."""
    native, bridge = [], []
    for p in serial.tools.list_ports.comports():
        if "/tty." in p.device:
            continue
        if (p.vid, p.pid) in NATIVE_IDS:
            native.append(p.device)
        elif (p.vid, p.pid) in BRIDGE_IDS:
            bridge.append(p.device)
    return sorted(native) + sorted(bridge)


def is_bridge(port):
    """True if `port` is a USB-UART bridge (anything there may be some other gadget)."""
    return any(p.device == port and (p.vid, p.pid) in BRIDGE_IDS
               for p in serial.tools.list_ports.comports())


def end_pattern(cmd):
    marker = _ENDS.get(" ".join(cmd.split()[:2]))
    return rf"^({marker}$|ERR\s)" if marker else _VERDICT


def clean_lines(raw):
    """Decode, drop ANSI colour codes and the REPL prompt, split into lines."""
    txt = ANSI.sub("", raw.decode("utf-8", "replace")).replace("\r", "")
    return [ln[len(PROMPT):] if ln.startswith(PROMPT) else ln for ln in txt.split("\n")]


class Link:
    def __init__(self, port):
        self.port = port
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.2, write_timeout=2)
        except serial.SerialException as e:
            raise LinkError(f"can't open {port}: {e}") from None
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def raw(self, cmd, timeout=10.0):
        """Send one command; return the raw bytes up to and including its end line."""
        end = re.compile(end_pattern(cmd), re.M)
        self.ser.reset_input_buffer()
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()
        buf, deadline = b"", time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(65536)
            if not chunk:
                continue
            buf += chunk
            done = buf[:buf.rfind(b"\n") + 1]     # only a whole line can end the reply
            if done and end.search("\n".join(self._reply(done, cmd))):
                return done
        raise LinkError(f"no reply to '{cmd.split()[0]}' within {timeout:.0f}s")

    @staticmethod
    def _reply(buf, cmd):
        out = []
        for ln in clean_lines(buf):
            s = ln.strip()
            # the console echoes what it's sent; never treat that echo as the reply
            if not s or s == cmd.strip() or s.startswith("Command returned non-zero") or ESP_LOG.match(s):
                continue   # ^ esp_console's footer after our own ERR line; log lines from other tasks
            out.append("ERR unrecognized command" if s == "Unrecognized command" else ln)
        return out

    def cmd(self, cmd, timeout=10.0, echo_safe=True):
        """Run a command and return its reply lines (the echoed command is dropped).

        echo_safe=False is for commands carrying a secret: the console echoes typed
        input, so only the final OK/ERR line is returned and nothing else is kept.
        """
        lines = self._reply(self.raw(cmd, timeout), cmd)
        if not echo_safe:
            lines = [ln for ln in lines if re.match(r"^(OK|ERR)\b", ln)][-1:]
        return lines

    def check(self, cmd, timeout=10.0, echo_safe=True):
        """Like cmd() but raise LinkError unless the reply ends with OK."""
        lines = self.cmd(cmd, timeout, echo_safe)
        last = lines[-1] if lines else ""
        if not last.startswith("OK"):
            raise LinkError(last or f"no OK from '{cmd.split()[0]}'")
        return lines

    def handshake(self, timeout=10.0, attempt=2.0):
        """Return the status lines; raise LinkError unless this is SparkESP32 firmware.

        Keeps asking for `timeout` seconds: a board that was just plugged in or
        reset is still booting, and its console's terminal probe (IDF
        linenoiseProbe) throws away everything typed in its first 0.5 s.
        """
        deadline, why = time.monotonic() + timeout, None
        while True:
            try:
                lines = self.cmd("status", timeout=attempt)
                if any(ln.startswith("fw=sparkesp32") for ln in lines):
                    return lines
                why = f"{self.port} is not running SparkESP32 firmware"
            except LinkError:
                pass
            if time.monotonic() >= deadline:
                raise LinkError(why or f"{self.port} didn't answer 'status' within {timeout:.0f}s: "
                                       "is it running SparkESP32 firmware?")


def connect(port=None):
    """Open the display (explicit port, or the single autodetected one) and handshake."""
    ports = [port] if port else find_ports()
    if not ports:
        raise LinkError("no SparkESP32 display found on USB (Espressif 303a:1001, or a CH340/CP210x "
                        "USB-UART bridge). Is it plugged in with a data-capable USB-C cable?")
    if len(ports) > 1:
        raise LinkError(f"several USB serial devices that could be a display ({', '.join(ports)}); "
                        "pick one with --port")
    link = Link(ports[0])
    try:
        link.handshake()
    except Exception:
        link.close()
        raise
    return link


def console_quote(s):
    """Quote for esp_console's argv splitter: double quotes, \\ escapes \" and \\."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
