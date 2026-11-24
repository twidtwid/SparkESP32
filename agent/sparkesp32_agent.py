#!/usr/bin/env python3
"""sparkesp32-agent: DGX Spark vitals for the SparkESP32 display.

Samples once a second:
    nvidia-smi     -> GPU %, temperature (C), power (W), SM clock (MHz) and the
                      clock event reasons                  [one long-lived child]
    /proc/stat     -> CPU % (aggregate + per core)
    /proc/meminfo  -> unified memory used/total (decimal MB, matches NVIDIA's GB)

or, on a Spark that runs a Sparkline agent (github.com/sfxnz/sparkline) and was set
up to use it (install.sh does that), reads the same values from Sparkline's
/api/v1/snapshot instead, and falls back to sampling itself if Sparkline stops
answering.

It watches the GPU clock for the GB10's stuck-clock state (see ClockWatch), and
publishes the same `stat` line (protocol v2, see docs/protocol.md) two ways:
    1. USB serial: pushed to the display while it's plugged in
    2. HTTP: GET /stat on :8787, polled by the display over Wi-Fi; GET /healthz is JSON
A display whose firmware predates protocol v2 gets the v1 line it understands.

The serial side streams only to a device that answers the `status` handshake with
`fw=sparkesp32`, and after install.sh --only-this-display it opens no other device
(see serial_loop). Installed by install.sh as a systemd service.
"""
import argparse
import http.client
import ipaddress
import json
import logging
import math
import os
import select
import signal
import socket
import subprocess
import sys
import threading
import time
import re
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # HTTP-only mode
    serial = None

__version__ = "1.4.2"
log = logging.getLogger("sparkesp32")

# The display's USB serial port: the chip's own USB-Serial/JTAG on most boards, or a
# USB-UART bridge in front of it on some (the Elecrow e-paper's CH340). tools/sparklink.py
# keeps the same lists.
NATIVE_IDS = {(0x303A, 0x1001)}       # Espressif USB-Serial/JTAG
BRIDGE_IDS = {(0x1A86, 0x7523),       # WCH CH340
              (0x1A86, 0x55D3),       # WCH CH343
              (0x1A86, 0x55D4),       # WCH CH9102
              (0x10C4, 0xEA60)}       # Silicon Labs CP210x
BAUD = 115200                         # native USB ignores it; a board behind a bridge runs its console at it
LEGACY_BAUD = 921600                  # 1.3.0 ran bridge boards at this: asked second, so they still connect
HANDSHAKE = b"fw=sparkesp32"
REJECT_S = 300.0                      # the display install.sh saw (or --port names) is asked again this
                                      # long after it fails the handshake; any other device, not until
                                      # it's replugged: it may be some other gadget
HANDSHAKE_S = 10.0                    # keep asking a just-plugged-in board this long: it's still
                                      # booting, then its console drops input for 0.5 s
STALE_S = 5.0                        # /stat answers 503 if the sampler is older than this
MAX_CLIENTS = 16                      # HTTP requests in flight; a display asks once a second
MAX_CORES = 32                        # what the display shows; also keeps a line < 256 bytes,
                                      # the firmware console's line buffer
GPU_STALE_S = 5.0                     # nvidia-smi values older than this are reported unknown
GPU_STALL_S = 10.0                    # it prints a line a second: this long silent, it's hung
NVSMI = ["nvidia-smi", "-i", "0", "--query-gpu=utilization.gpu,temperature.gpu,power.draw,"
         "clocks.sm,clocks_event_reasons.active", "--format=csv,noheader,nounits", "-lms", "1000"]
NVSMI_FIELDS = 5
PROTO = 2                             # the newest wire protocol this agent speaks
V1_TOKENS = 9                         # a v1 line is a v2 line without its last two fields
MHZ_MAX = 10000                       # the firmware's range for gpu_mhz

# The GB10 stuck-clock state (NVIDIA forum threads 386060, 387029 and 376039): under load the
# GPU runs at 500-850 MHz and ~20 W instead of ~2,000-2,400 MHz, reports no clock event
# reason, survives a reboot, and only a full power drain clears it. The thresholds are the
# ones the community's detector uses (github.com/lcasarin-maker/blackbox): stuck below
# 1000 MHz with load and no reason, healthy from 1400 MHz. Idle clocks float, so they say nothing.
LOADED_PCT = 80                       # GPU utilization from which the clock is telling
STUCK_MHZ = 1000                      # loaded, below this, no event reason: looks stuck
HEALTHY_MHZ = 1400                    # loaded, at or above this: not stuck
STUCK_AFTER = 10                      # loaded samples in a row that look stuck before saying so
HEALTHY_AFTER = 5                     # loaded samples in a row that look healthy to clear it
IDLE_REASON = 0x1                     # NVML's GpuIdle event reason: not a limit
SPARKLINE_TIMEOUT_S = 1.5             # one snapshot request; slower than this counts as a failure
SPARKLINE_MAX_BYTES = 65536           # a snapshot is a few KB; more than this is not one
SPARKLINE_RETRY_S = 30.0              # after a failure, sample ourselves this long before asking again


UNKNOWN_GPU = (None,) * NVSMI_FIELDS


class RateLimitedLog:
    """Log a recurring problem at most once per `every` seconds."""

    def __init__(self, every=60.0):
        self.every, self.last = every, {}

    def warn(self, key, msg, *args):
        now = time.monotonic()
        if now - self.last.get(key, -1e9) >= self.every:
            self.last[key] = now
            log.warning(msg, *args)


rl = RateLimitedLog()


# ---------------------------------------------------------------- sampling

def read_cpu_times():
    """[(idle, total)] for the aggregate line, then each core, from /proc/stat."""
    out = []
    with open("/proc/stat") as f:
        for line in f:
            if not line.startswith("cpu"):
                break
            v = [int(x) for x in line.split()[1:9]]   # user..steal; guest is inside user
            out.append((v[3] + v[4], sum(v)))         # idle + iowait, total
    return out


def pct(prev, cur):
    d_total = cur[1] - prev[1]
    d_idle = cur[0] - prev[0]
    if d_total <= 0:
        return 0
    return max(0, min(100, round(100 * (d_total - d_idle) / d_total)))


def read_mem_mb():
    """(used_mb, total_mb), decimal MB; used = MemTotal - MemAvailable."""
    kb = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, v = line.split(":", 1)
            if k in ("MemTotal", "MemAvailable"):
                kb[k] = int(v.split()[0])
    total = kb.get("MemTotal", 0)
    used = total - kb.get("MemAvailable", total)
    return round(used * 1024 / 1e6), round(total * 1024 / 1e6)


class GpuReader(threading.Thread):
    """Keeps one `nvidia-smi -lms 1000` child running and remembers its latest line.

    Sampling never waits on nvidia-smi: fields older than GPU_STALE_S go out as
    unknown ("-"). A reading is (gpu_pct, temp_c, power_dw, sm_mhz, event_mask). The
    child's state, published in /healthz as gpu_source:
      idle         not started: the readings come from Sparkline (see /healthz "source")
      starting     spawned, no line yet
      streaming    a line a second
      stalled      silent for GPU_STALL_S (a hung driver call): killed, then respawned
      exited       it quit (gpu_error says how); respawned after a backoff
      unavailable  it couldn't be started (gpu_error says why)
    """

    def __init__(self, stop):
        super().__init__(daemon=True, name="gpu")
        self.stop, self.state, self.error = stop, "idle", None
        self.reading = (UNKNOWN_GPU, 0.0)          # (values, monotonic time); replaced whole
        self.first = threading.Event()             # set by the first reading

    @staticmethod
    def _num(s, scale=1.0):
        try:
            return round(float(s.strip()) * scale)
        except ValueError:      # "[N/A]" for fields the GPU doesn't report
            return None

    @staticmethod
    def _mask(s):
        """clocks_event_reasons.active, "0x0000000000000000", as an int; None if not reported."""
        try:
            return int(s.strip(), 16)
        except ValueError:
            return None

    def ensure_started(self):
        """Start the child on first use: with Sparkline as the source it may never be needed."""
        if self.state == "idle":
            self.state = "starting"
            self.start()

    def run(self):
        backoff = 1.0
        while not self.stop.is_set():
            try:
                proc = subprocess.Popen(NVSMI, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            except OSError as e:
                self.state, self.error = "unavailable", str(e)
            else:
                with proc:                                 # closes its pipes on the way out
                    self.state = "starting"
                    if self._stream(proc):
                        backoff = 1.0
                    proc.kill()
                    code = proc.wait()
                    if self.state != "stalled":
                        err = proc.stderr.read().decode(errors="replace").strip().splitlines()
                        self.state, self.error = "exited", f"exit {code}: {err[-1] if err else 'no output'}"
            if self.stop.is_set():
                break
            rl.warn("gpu", "nvidia-smi %s (%s); GPU fields show -- until it's back", self.state, self.error)
            self.stop.wait(min(backoff, 60))
            backoff *= 2

    def _stream(self, proc):
        """Take lines until the child exits, goes quiet for GPU_STALL_S, or we stop.
        True if it gave at least one reading."""
        fd, buf, got, heard = proc.stdout.fileno(), b"", False, time.monotonic()
        while not self.stop.is_set():
            if not select.select([fd], [], [], 0.5)[0]:
                if time.monotonic() - heard > GPU_STALL_S:
                    self.state, self.error = "stalled", f"no output for {GPU_STALL_S:.0f}s"
                    return got
                continue
            chunk = os.read(fd, 4096)
            if not chunk:
                return got                        # it exited
            *lines, buf = (buf + chunk).split(b"\n")
            buf = buf[-4096:]
            for line in lines:
                heard = time.monotonic()
                f = line.decode(errors="replace").split(",")
                if len(f) == NVSMI_FIELDS:
                    values = (self._num(f[0]), self._num(f[1]), self._num(f[2], 10),
                              self._num(f[3]), self._mask(f[4]))
                    self.reading = (values, time.monotonic())
                    self.state, self.error, got = "streaming", None, True
                    self.first.set()
        return got

    def latest(self):
        """(gpu_pct, temp_c, power_dw, sm_mhz, event_mask), each None when unknown or stale."""
        values, at = self.reading
        return values if time.monotonic() - at <= GPU_STALE_S else UNKNOWN_GPU


class ClockWatch:
    """Spots the GB10 stuck-clock state from one GPU reading a second.

    A reading under load (>= LOADED_PCT) looks stuck when the clock is below STUCK_MHZ and
    no event reason other than idle explains it (a thermal or power slowdown, an application
    clock limit), and healthy from HEALTHY_MHZ. STUCK_AFTER stuck-looking loaded readings in
    a row make `stuck` True; HEALTHY_AFTER healthy ones make it False. Readings without load
    change nothing, so the verdict holds through idle stretches and a bursty workload adds
    up: the state lasts until the power is drained, which restarts this agent anyway.
    `stuck` is None until there's been enough load to tell.
    """

    def __init__(self):
        self.stuck, self._low, self._high = None, 0, 0

    def update(self, util, mhz, mask):
        if util is None or mhz is None or util < LOADED_PCT:
            return self.stuck
        explained = mask is not None and mask & ~IDLE_REASON
        self._low = self._low + 1 if mhz < STUCK_MHZ and not explained else 0
        self._high = self._high + 1 if mhz >= HEALTHY_MHZ else 0
        if self._low >= STUCK_AFTER and self.stuck is not True:
            self.stuck = True
            log.warning("GPU clock stuck: %d MHz at %d%% load with no clock event reason. This is "
                        "the GB10 stuck-clock state; a reboot won't clear it, a full power drain "
                        "will (docs/troubleshooting.md)", mhz, util)
        elif self._high >= HEALTHY_AFTER and self.stuck is not False:
            if self.stuck:
                log.info("GPU clock back to normal: %d MHz at %d%% load", mhz, util)
            self.stuck = False
        return self.stuck


def snapshot_fields(s):
    """Our sample fields from a Sparkline agent's /api/v1/snapshot (its Sample model). Raises
    KeyError, TypeError, IndexError, AttributeError or ValueError on one it doesn't recognize."""
    def pct(v):
        return max(0, min(100, round(v)))

    def num(v, scale=1):
        return None if v is None else round(float(v) * scale)

    cpu, mem = s["cpu"], s["mem"]
    gpu = s["gpu"][0] if s["gpu"] else {}
    mask = gpu.get("throttle_mask")             # NVML clock event reasons; older Sparklines lack it
    return dict(cpu=pct(cpu["util"]), gpu=None if gpu.get("util") is None else pct(gpu["util"]),
                mem_used_mb=round(mem["used"] / 1e6), mem_total_mb=round(mem["total"] / 1e6),
                temp_c=num(gpu.get("temp_c")), power_dw=num(gpu.get("power_w"), 10),
                gpu_mhz=num(gpu.get("sm_mhz")), gpu_events=mask if isinstance(mask, int) else None,
                cores=[pct(c) for c in cpu["per_core"]][:MAX_CORES])


def default_route_ip():
    """This host's IPv4 address on its default route: where a Sparkline whose `bind` is
    "auto" listens. Raises OSError when there's no such route. Nothing is sent: connecting
    a UDP socket only asks the kernel which address it would send from."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("1.1.1.1", 9))
        return s.getsockname()[0]


def is_this_host(sock):
    """True if `sock` is connected to this machine itself: the kernel reaches one of the
    host's own addresses from that same address, and loopback from loopback."""
    here, peer = sock.getsockname()[0], sock.getpeername()[0]
    return here == peer or ipaddress.ip_address(peer).is_loopback


class SparklineSource:
    """Readings from a Sparkline agent (github.com/sfxnz/sparkline) on this Spark.

    Asks GET <url>/api/v1/snapshot with Sparkline's agent token, once per tick. Any
    failure (refused, timeout, 401, a stale or unrecognized sample) makes read() return
    None, so the sampler uses its own readings, and Sparkline is asked again after
    SPARKLINE_RETRY_S. Published in /healthz as "sparkline": {url, state, error}.

    The host in <url> may be "auto", Sparkline's own default for `bind`: this host's
    address on its default route, looked up for every request, so a new DHCP lease is
    followed. The token opens all of Sparkline's API (updates and benchmarks too), so it
    is sent only once the connection is seen to end on this host: an address this Spark
    no longer has is some other machine's. It is never logged and never in /healthz.
    """

    def __init__(self, url, token):
        self.url = url.rstrip("/")
        u = urllib.parse.urlsplit(self.url)
        if u.scheme != "http" or not u.hostname:
            raise ValueError(f"Sparkline's URL must be http://host[:port], not {url!r}")
        self._host, self._port, self._path = u.hostname, u.port or 80, u.path + "/api/v1/snapshot"
        self._auth = {"Authorization": f"Bearer {token}"}
        self.state, self.error, self._retry_at = "starting", None, 0.0

    def _fetch(self):
        """Sparkline's snapshot, parsed. Raises OSError, ValueError or http.client.HTTPException."""
        host = default_route_ip() if self._host == "auto" else self._host
        conn = http.client.HTTPConnection(host, self._port, timeout=SPARKLINE_TIMEOUT_S)
        try:
            conn.connect()
            if not is_this_host(conn.sock):         # before anything is sent
                raise ValueError(f"{host} isn't this host, so it doesn't get Sparkline's token: "
                                 "re-run install.sh")
            conn.request("GET", self._path, headers=self._auth)
            r = conn.getresponse()
            if r.status != 200:                     # a redirect too: the token doesn't follow one
                raise ValueError(f"HTTP {r.status}" + (" (token rejected: re-run install.sh)"
                                                       if r.status in (401, 403) else ""))
            body = r.read(SPARKLINE_MAX_BYTES + 1)
            if len(body) > SPARKLINE_MAX_BYTES:
                raise ValueError(f"snapshot over {SPARKLINE_MAX_BYTES} bytes")
            return json.loads(body)
        finally:
            conn.close()

    def read(self):
        """Our sample fields, or None (self.error says why) to sample ourselves this tick."""
        if time.monotonic() < self._retry_at:
            return None
        try:
            s = self._fetch()
            age = time.time() - s["ts"]
            if age > STALE_S:
                raise ValueError(f"its latest sample is {age:.0f}s old")
            fields = snapshot_fields(s)
        except (KeyError, TypeError, IndexError, AttributeError, ArithmeticError, RecursionError) as e:
            why = f"unrecognized snapshot ({type(e).__name__}: {e})"
        except (OSError, ValueError, http.client.HTTPException) as e:   # refused, timeout, not this
            why = str(e) or type(e).__name__                            # host, an HTTP error, bad JSON, stale
        else:
            if self.state != "ok":
                log.info("reading from Sparkline at %s", self.url)
            self.state, self.error = "ok", None
            return fields
        self.state, self.error = "error", why
        self._retry_at = time.monotonic() + SPARKLINE_RETRY_S
        rl.warn("sparkline", "Sparkline at %s: %s; sampling ourselves, asking again every %.0fs",
                self.url, why, SPARKLINE_RETRY_S)
        return None

    def health(self):
        return {"url": self.url, "state": self.state, "error": self.error}


def fmt(v):
    return "-" if v is None else str(v)


def wire_line(line, proto):
    """The sample for a reader of protocol `proto`: v1 is the v2 line without its last two
    fields (gpu_mhz gpu_stuck), the form a display from before v2 accepts."""
    return line if proto >= 2 else " ".join(line.split()[:V1_TOKENS])


def short_hostname():
    """This host's short name for the display title, or "-" if it isn't wire-safe."""
    name = socket.gethostname().split(".")[0]
    ok = 0 < len(name) <= 63 and all(c.isascii() and (c.isalnum() or c in "-_") for c in name)
    return name if ok else "-"


class Sampler:
    """The latest stat line, refreshed once per interval, and the display link's
    status, both read by the HTTP handler. Each is replaced whole, never mutated,
    so readers need no lock."""

    def __init__(self, gpu, sparkline=None):
        self.gpu, self.sparkline, self.host = gpu, sparkline, short_hostname()
        self.clock = ClockWatch()
        self.snap = (None, 0.0, {})          # (line, monotonic time of it, /healthz fields)
        self.display = {"state": "searching"}    # the USB link, kept by serial_loop
        self._prev = read_cpu_times()

    def tick(self):
        cur = read_cpu_times()
        cpu = pct(self._prev[0], cur[0])
        # cores hot-plugged/offlined between reads -> only compare what lines up
        cores = [pct(p, c) for p, c in zip(self._prev[1:], cur[1:], strict=False)][:MAX_CORES]
        self._prev = cur
        memu, memt = read_mem_mb()
        got = self.sparkline.read() if self.sparkline else None
        if got:
            cpu, gpu, memu, memt = got["cpu"], got["gpu"], got["mem_used_mb"], got["mem_total_mb"]
            temp, pow_dw, cores = got["temp_c"], got["power_dw"], got["cores"]
            mhz, events = got["gpu_mhz"], got["gpu_events"]
        else:
            self.gpu.ensure_started()
            gpu, temp, pow_dw, mhz, events = self.gpu.latest()
        if mhz is not None and not 0 <= mhz <= MHZ_MAX:
            mhz = None                          # out of the firmware's range: it would drop the line
        stuck = self.clock.update(gpu, mhz, events)
        line = (f"stat {cpu} {fmt(gpu)} {memu} {memt} {fmt(temp)} {fmt(pow_dw)} "
                f"{','.join(map(str, cores)) or '-'} {self.host} "
                f"{fmt(mhz)} {fmt(None if stuck is None else int(stuck))}")
        sample = dict(host=self.host, cpu=cpu, gpu=gpu, mem_used_mb=memu, mem_total_mb=memt,
                      temp_c=temp, power_w=None if pow_dw is None else pow_dw / 10,
                      gpu_mhz=mhz, gpu_clock_events=events, gpu_clock_stuck=stuck,
                      cores=cores, source="sparkline" if got else "own",
                      gpu_source=self.gpu.state, gpu_error=self.gpu.error)
        if self.sparkline:
            sample["sparkline"] = self.sparkline.health()
        self.snap = (line, time.monotonic(), sample)

    def latest(self):
        """(line, at, sample); `at` is the monotonic time of that line, a protocol v2 line
        (wire_line() makes the v1 form)."""
        return self.snap


# ---------------------------------------------------------------- HTTP

def asked_proto(query):
    """The protocol a /stat request asks for with ?v=N, at most PROTO. 1 with no v, or with
    one that isn't a plain number: /stat is v1 for anything from before v2."""
    asked = urllib.parse.parse_qs(query).get("v", ["1"])[-1]
    if not (asked.isascii() and asked.isdigit()):      # isdigit() alone takes "²", and int() doesn't
        return 1
    return PROTO if len(asked) > 4 else min(int(asked), PROTO)     # longer: int() has a digit limit


def public_health(body):
    """/healthz as a client on another machine gets it: the readings and each state, without
    what only this Spark's admin needs (the serial port, where Sparkline is, error texts)."""
    out = {k: v for k, v in body.items() if k != "gpu_error"}
    out["display"] = {"state": body["display"].get("state")}
    if "sparkline" in body:
        out["sparkline"] = {"state": body["sparkline"].get("state")}
    return out


class Server(ThreadingHTTPServer):
    """A thread per request, at most MAX_CLIENTS of them at once: one more is hung up on.
    Clients that connect and then say nothing, or send a byte now and then, each hold a
    thread and a socket for as long as they like; without a limit, they add up."""
    daemon_threads = True

    def __init__(self, *args):
        super().__init__(*args)
        self._free = threading.BoundedSemaphore(MAX_CLIENTS)

    def process_request(self, request, client_address):
        if not self._free.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:                   # its thread didn't start
            self._free.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._free.release()


def make_handler(sampler):
    class Handler(BaseHTTPRequestHandler):
        timeout = 5                     # idle clients can't pin a thread forever
        server_version = f"sparkesp32-agent/{__version__}"

        def _send(self, code, body, ctype="text/plain; charset=utf-8"):
            data = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            line, at, sample = sampler.latest()
            age = time.monotonic() - at
            path, _, query = self.path.partition("?")
            if path == "/stat":
                if line is None or age > STALE_S:
                    self._send(503, "stale\n")
                else:
                    self._send(200, wire_line(line, asked_proto(query)) + "\n")
            elif path == "/healthz":
                body = dict(version=__version__, age_s=round(age, 2), display=sampler.display, **sample)
                if not ipaddress.ip_address(self.client_address[0]).is_loopback:
                    body = public_health(body)
                self._send(200, json.dumps(body) + "\n", "application/json")
            else:
                self._send(404, "try /stat or /healthz\n")

        def log_message(self, *args):   # one request a second, forever: keep journald quiet
            pass

    return Handler


# ---------------------------------------------------------------- serial

def node_id(path):
    """Which device node `path` is now: a replug makes a new one, even at the same path."""
    try:
        st = os.stat(path)
        return (st.st_ino, st.st_ctime_ns)
    except OSError:
        return None


def is_bridge(port):
    """True if `port` is a USB-UART bridge (anything there may be some other gadget)."""
    return any(p.device == port and (p.vid, p.pid) in BRIDGE_IDS
               for p in serial.tools.list_ports.comports())


def port_id(p):
    """What tells one USB serial device from another of its kind, as install.sh saves it for
    the display it saw: "vid:pid:serial" (an Espressif chip's serial number is its MAC), or
    "vid:pid@location", the USB socket it's in, for a bridge without one (the CH340). None
    if the device has neither, or the text wouldn't be safe in a settings file.
    tools/sparklink.py keeps the same rule."""
    if p.vid is None or p.pid is None:
        return None
    rest = f":{p.serial_number}" if p.serial_number else f"@{p.location}" if p.location else ""
    return f"{p.vid:04x}:{p.pid:04x}{rest}" if re.fullmatch(r"[:@][A-Za-z0-9_:.-]+", rest) else None


def candidate_ports(display=None):
    """[(device, is_bridge)]: native USB ports first, then USB-UART bridges. With `display`
    (the port_id install.sh saved), that device alone."""
    found = [(p.device, (p.vid, p.pid) in BRIDGE_IDS) for p in serial.tools.list_ports.comports()
             if ((p.vid, p.pid) in NATIVE_IDS or (p.vid, p.pid) in BRIDGE_IDS)
             and display in (None, port_id(p))]
    return sorted(found, key=lambda d: (d[1], d[0]))


HANDSHAKE_LINE = re.compile(re.escape(HANDSHAKE) + rb"[^\r\n]*[\r\n]")
PROTO_TOKEN = re.compile(rb" proto=(\d+)")


def handshake(ser, timeout=HANDSHAKE_S, every=1.0):
    """The protocol the device on `ser` reads if it's a SparkESP32 (answers status with a
    fw=sparkesp32 line): 1, or the line's proto=N (capped at PROTO). 0 if it isn't one.

    Asks again every `every` seconds: the port appears the moment the board is
    plugged in, before its console exists, and the console's terminal probe
    (IDF linenoiseProbe) then throws away everything typed in its first 0.5 s.
    """
    ser.reset_input_buffer()
    buf, deadline, ask_at = b"", time.monotonic() + timeout, 0.0
    while time.monotonic() < deadline:
        if time.monotonic() >= ask_at:
            ser.write(b"\nstatus\n")
            ask_at = time.monotonic() + every
        buf = (buf + ser.read(256))[-4096:]
        m = HANDSHAKE_LINE.search(buf)              # the whole line, to see its proto=
        if m:
            p = PROTO_TOKEN.search(m.group())
            return min(int(p.group(1)), PROTO) if p else 1
    return 0


def serial_loop(sampler, explicit_port, stop, display=None):
    """Push each fresh sample to the display; survive unplug/replug.

    Which devices are opened: the one at `explicit_port` (--port); else the one whose
    port_id is `display`, which install.sh saves when it has seen a display answer; else
    (no display saved) every Espressif or USB-UART bridge device, native ones first.

    The link's state goes to /healthz as sampler.display = {"state": ..., ...}:
      searching    no usable port ("error" says why if opening one failed; "ignoring"
                   lists the devices left unopened because they aren't the saved display)
      handshaking  asking `status` on "port", for up to HANDSHAKE_S
      streaming    pushing stat lines to "port" ("answered_in_s": handshake time,
                   "proto": the protocol its firmware reads)
      rejected     "port" didn't answer as a SparkESP32, or (a USB-UART bridge nobody
                   named) another program has it locked. The saved or named display is
                   asked again after REJECT_S ("retry_in_s"); a device that was only
                   found waits for a replug ("retry_in_s" null): it may be any gadget

    Opening a port and asking `status` is the only way to tell a display from another
    Espressif board or serial adapter, so a device that isn't one does get those few
    bytes: once per plug-in when no display is saved, never when one is. Ports are
    opened with an advisory lock (flock), which keeps the agent out of a port another
    program has locked, but not out of one a program has open without locking it.
    """
    named = bool(explicit_port or display)      # --port or install.sh says which one is the display
    if display and not explicit_port:
        log.info("USB: opening only the display install.sh saw (for another: sudo ./install.sh "
                 "--any-display)")
    # rejected: port -> (until, the device node it was), so a replug clears it however fast
    ser, last_at, rejected, proto = None, 0.0, {}, 1
    while not stop.is_set():
        try:
            if ser is None:
                now = time.monotonic()
                # a replug (a new device node) forgets a rejection, so it's always retried at once
                rejected = {p: (t, n) for p, (t, n) in rejected.items() if t > now and node_id(p) == n}
                ports = ([(explicit_port, is_bridge(explicit_port))] if explicit_port
                         else candidate_ports(display))
                ports = [(p, b) for p, b in ports if p not in rejected]
                if not ports:
                    others = [p for p, _ in candidate_ports()] if display and not explicit_port else []
                    if rejected:
                        port, (until, _) = min(rejected.items(), key=lambda kv: kv[1][0])
                        sampler.display = {"state": "rejected", "port": port,
                                           "retry_in_s": None if until == math.inf else round(until - now)}
                    elif others:
                        sampler.display = {"state": "searching", "ignoring": others}
                        rl.warn("ignoring", "not opening %s: not the display install.sh saw (to use it as "
                                "the display, run sudo ./install.sh --any-display)", ", ".join(others))
                    else:
                        sampler.display = {"state": "searching"}
                    stop.wait(2)
                    continue
                port, bridge = ports[0]
                sampler.display = {"state": "handshaking", "port": port}
                try:
                    ser = serial.Serial(port, BAUD, timeout=0.2, write_timeout=1, exclusive=True)
                except (OSError, serial.SerialException) as e:
                    if named or not bridge:
                        raise
                    # locked by another program, or unopenable: not ours to poke
                    log.warning("can't open %s (a USB-UART bridge): %s; leaving it alone until it's "
                                "replugged", port, e)
                    rejected[port] = (math.inf, node_id(port))
                    continue
                t0 = time.monotonic()
                proto = handshake(ser, HANDSHAKE_S)
                if not proto and bridge:    # a board from 1.3.0 still runs its console at 921600
                    ser.baudrate = LEGACY_BAUD
                    proto = handshake(ser, HANDSHAKE_S)
                    if proto:
                        log.warning("%s answers at %d baud: its firmware is older than 1.4.0; update it "
                                    "with install.sh", port, LEGACY_BAUD)
                if not proto:
                    ser.close()
                    ser = None
                    if named:
                        log.warning("%s didn't answer as a SparkESP32; asking again in %d minutes, or when "
                                    "it's replugged", port, REJECT_S // 60)
                        until = time.monotonic() + REJECT_S
                    else:           # only found, and it may be any gadget: asked once per plug-in
                        log.warning("%s (%s) didn't answer as a SparkESP32; leaving it alone until it's "
                                    "replugged", port,
                                    "a USB-UART bridge" if bridge else "an Espressif USB device")
                        until = math.inf
                    rejected[port] = (until, node_id(port))
                    continue
                took = round(time.monotonic() - t0, 1)
                log.info("display connected on %s (answered in %.1fs, protocol v%d)", port, took, proto)
                sampler.display = {"state": "streaming", "port": port, "answered_in_s": took,
                                   "proto": proto}
            line, at, _ = sampler.latest()
            if line and at != last_at:
                ser.write((wire_line(line, proto) + "\n").encode())
                last_at = at
            if ser.in_waiting:              # drain console echo/replies so the
                ser.read(ser.in_waiting)    # display's TX never backs up
            stop.wait(0.2)
        except Exception as e:              # unplug, permission, anything: retry, don't die
            if ser is not None:
                log.info("display disconnected (%s)", e)
                try:
                    ser.close()
                except Exception:
                    pass
            else:
                rl.warn("serial-open", "can't open the display's port: %s", e)
            ser = None
            sampler.display = {"state": "searching", "error": str(e)}
            stop.wait(2)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between samples")
    ap.add_argument("--bind", default="0.0.0.0",
                    help="HTTP bind address (default all interfaces; the installed service and ufw "
                         "limit it to the LAN)")
    ap.add_argument("--http-port", type=int, default=8787)
    ap.add_argument("--port", help="serial device (default: autodetect the display)")
    ap.add_argument("--no-serial", action="store_true", help="HTTP only")
    ap.add_argument("--no-http", action="store_true", help="USB only")
    ap.add_argument("--once", action="store_true", help="print one stat line and exit")
    ap.add_argument("--display", default=os.environ.get("SPARKESP32_DISPLAY"), metavar="ID",
                    help="open only the USB device with this id (install.sh saves the id of the "
                         "display it saw); default: ask every Espressif or bridge device once")
    ap.add_argument("--sparkline-url", default=os.environ.get("SPARKLINE_URL"),
                    help="read from the Sparkline agent at this URL, on this host (http://auto:PORT "
                         "for its default-route address; token in $SPARKLINE_TOKEN); install.sh "
                         "sets both when it finds one")
    ap.add_argument("--version", action="version", version=__version__)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())

    token = os.environ.pop("SPARKLINE_TOKEN", "")     # popped: nvidia-smi doesn't inherit it
    sparkline = SparklineSource(args.sparkline_url, token) if args.sparkline_url and token else None
    gpu = GpuReader(stop)
    sampler = Sampler(gpu, sparkline)
    if sparkline is None:
        gpu.ensure_started()
        gpu.first.wait(2)          # so the first line already has GPU values, not "-"
    sampler.tick()

    if args.once:
        print(sampler.latest()[0])
        stop.set()
        return 0

    if not args.no_http:
        httpd = Server((args.bind, args.http_port), make_handler(sampler))
        threading.Thread(target=httpd.serve_forever, daemon=True, name="http").start()
        log.info("serving /stat on http://%s:%d", args.bind, args.http_port)
    if not args.no_serial:
        if serial is None:
            log.warning("pyserial missing: USB output disabled")
        else:
            threading.Thread(target=serial_loop, args=(sampler, args.port, stop, args.display),
                             daemon=True, name="serial").start()

    while not stop.wait(args.interval):
        try:
            sampler.tick()
        except Exception:
            log.exception("sample failed")
    log.info("stopping")
    return 0


if __name__ == "__main__":
    sys.exit(main())
