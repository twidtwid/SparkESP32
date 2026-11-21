#!/usr/bin/env python3
"""Host tests for the agent and the serial tools. Stdlib only: pyserial is
stubbed when it isn't installed, so `make check` runs anywhere.

    python3 tests/test_agent.py
"""
import base64
import json
import os
import re
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.error
import urllib.request
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = [os.path.join(ROOT, "agent"), os.path.join(ROOT, "tools")]

try:
    import serial  # noqa: F401
except ImportError:                       # minimal stand-in so the modules import
    fake = types.ModuleType("serial")
    fake.SerialException = OSError

    def _no_port(*a, **k):
        raise OSError("no such port")
    fake.Serial = _no_port
    fake.tools = types.ModuleType("serial.tools")
    fake.tools.list_ports = types.ModuleType("serial.tools.list_ports")
    fake.tools.list_ports.comports = lambda: []
    sys.modules.update({"serial": fake, "serial.tools": fake.tools,
                        "serial.tools.list_ports": fake.tools.list_ports})

import devctl  # noqa: E402
import sparkesp32_agent as agent  # noqa: E402
import sparklink  # noqa: E402


# ------------------------------------------------------------------ agent

class FakeGpu:
    def __init__(self, values=(91, 60, 410), state="streaming"):
        self.values, self.state, self.error, self.started = values, state, None, False

    def ensure_started(self):
        self.started = True

    def latest(self):
        return self.values


class SamplerTest(unittest.TestCase):
    def make(self, gpu, times, mem=(46328, 130661)):
        seq = iter(times)
        orig = agent.read_cpu_times, agent.read_mem_mb
        agent.read_cpu_times = lambda: next(seq)
        agent.read_mem_mb = lambda: mem
        self.addCleanup(lambda: (setattr(agent, "read_cpu_times", orig[0]),
                                 setattr(agent, "read_mem_mb", orig[1])))
        return agent.Sampler(gpu)

    def test_line_is_protocol_v1(self):
        # aggregate + 3 cores: (idle, total) pairs, 1 s apart
        s = self.make(FakeGpu(), [
            [(100, 200), (10, 50), (10, 50), (10, 50)],
            [(150, 300), (60, 100), (35, 100), (10, 100)],
        ])
        s.tick()
        line, at, sample = s.latest()
        self.assertEqual(line, "stat 50 91 46328 130661 60 410 0,50,100 " + agent.short_hostname())
        self.assertEqual(len(line.split()), 9)
        self.assertEqual(sample["power_w"], 41.0)
        self.assertGreater(at, 0)

    def test_unknown_gpu_fields_are_dashes(self):
        s = self.make(FakeGpu((None, None, None), "nvidia-smi unavailable"),
                      [[(0, 0), (0, 0)], [(0, 100), (50, 100)]])
        s.tick()
        self.assertEqual(s.latest()[0], "stat 100 - 46328 130661 - - 50 " + agent.short_hostname())

    def test_big_machine_line_fits_the_console_buffer(self):
        # 144 cores, every value 3 digits, a 63-char hostname: worst case
        n = 144
        s = self.make(FakeGpu((100, 150, 100000)),
                      [[(0, 0)] * (n + 1), [(0, 100)] * (n + 1)], mem=(2**31 - 1, 2**31 - 1))
        s.host = "h" * 63
        s.tick()
        line = s.latest()[0]
        self.assertEqual(len(line.split()[7].split(",")), agent.MAX_CORES)
        self.assertLess(len(line) + 1, 256)       # + "\n"; firmware console buffer is 256

    def test_core_count_change_compares_only_matching_cores(self):
        s = self.make(FakeGpu(), [[(0, 0), (0, 0), (0, 0)], [(0, 100), (0, 100)]])
        s.tick()
        self.assertEqual(s.latest()[0].split()[7], "100")

    def test_hostname_is_wire_safe(self):
        orig = agent.socket.gethostname
        self.addCleanup(setattr, agent.socket, "gethostname", orig)
        for raw, want in [("spark-1a2b", "spark-1a2b"), ("spark-1a2b.lan", "spark-1a2b"),
                          ("my_box", "my_box"), ("caf\u00e9", "-"), ("", "-"), ("x" * 64, "-")]:
            agent.socket.gethostname = lambda raw=raw: raw
            self.assertEqual(agent.short_hostname(), want, raw)

    def test_sparkline_values_replace_our_own(self):
        class Up:
            def read(self):
                return dict(cpu=7, gpu=33, mem_used_mb=99646, mem_total_mb=130661, temp_c=36,
                            power_dw=107, cores=[1, 2, 3])

            def health(self):
                return {"url": "http://x:9750", "state": "ok", "error": None}
        gpu = FakeGpu()
        s = self.make(gpu, [[(0, 0), (0, 0)], [(0, 100), (0, 100)]])
        s.sparkline = Up()
        s.tick()
        line, _, sample = s.latest()
        self.assertEqual(line, "stat 7 33 99646 130661 36 107 1,2,3 " + agent.short_hostname())
        self.assertEqual((sample["source"], sample["sparkline"]["state"]), ("sparkline", "ok"))
        self.assertFalse(gpu.started)              # no nvidia-smi while Sparkline answers

    def test_sparkline_failure_falls_back_to_our_own(self):
        class Down:
            def read(self):
                return None

            def health(self):
                return {"url": "http://x:9750", "state": "error", "error": "HTTP 401"}
        gpu = FakeGpu()
        s = self.make(gpu, [[(0, 0), (0, 0)], [(0, 100), (50, 100)]])
        s.sparkline = Down()
        s.tick()
        line, _, sample = s.latest()
        self.assertEqual(line, "stat 100 91 46328 130661 60 410 50 " + agent.short_hostname())
        self.assertEqual(sample["source"], "own")
        self.assertTrue(gpu.started)

    def test_pct_clamps_and_handles_no_progress(self):
        self.assertEqual(agent.pct((5, 10), (5, 10)), 0)
        self.assertEqual(agent.pct((0, 0), (0, 100)), 100)
        self.assertEqual(agent.pct((0, 0), (100, 100)), 0)
        self.assertEqual(agent.pct((0, 0), (200, 100)), 0)    # bogus counters: clamp

    def test_gpu_reader_parses_nvidia_smi_fields(self):
        self.assertEqual(agent.GpuReader._num(" 42 "), 42)
        self.assertEqual(agent.GpuReader._num("10.6", 10), 106)
        self.assertIsNone(agent.GpuReader._num("[N/A]"))

    def test_gpu_reader_goes_stale(self):
        g = agent.GpuReader(threading.Event())
        g.reading = ((1, 2, 3), time.monotonic())
        self.assertEqual(g.latest(), (1, 2, 3))
        g.reading = ((1, 2, 3), time.monotonic() - agent.GPU_STALE_S - 1)
        self.assertEqual(g.latest(), (None, None, None))


class GpuReaderLifecycleTest(unittest.TestCase):
    """The real reader against stand-in nvidia-smi children."""

    def watch(self, argv, done, timeout=6.0):
        """Run a GpuReader on `argv` until done(states seen, in order) or timeout."""
        saved = agent.NVSMI, agent.GPU_STALL_S
        agent.NVSMI, agent.GPU_STALL_S = argv, 0.5
        stop, seen = threading.Event(), []
        g = agent.GpuReader(stop)
        g.start()
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline and not done(seen):
                if not seen or seen[-1] != g.state:
                    seen.append(g.state)
                time.sleep(0.01)
            return g, seen
        finally:
            stop.set()
            g.join(3)
            agent.NVSMI, agent.GPU_STALL_S = saved

    @staticmethod
    def child(code):
        return [sys.executable, "-c", code]

    def test_a_hung_nvidia_smi_is_killed_and_respawned(self):
        hangs = self.child("import time; print('7, 41, 10.6', flush=True); time.sleep(60)")

        def respawned(seen):
            return "stalled" in seen and "streaming" in seen[seen.index("stalled"):]
        g, seen = self.watch(hangs, respawned)
        self.assertTrue(respawned(seen), seen)
        self.assertEqual(g.reading[0], (7, 41, 106))

    def test_an_exit_is_reported_with_its_stderr(self):
        g, seen = self.watch(self.child("import sys; print('boom', file=sys.stderr); sys.exit(3)"),
                             lambda seen: "exited" in seen)
        self.assertIn("exited", seen)
        self.assertEqual(g.error, "exit 3: boom")

    def test_a_missing_nvidia_smi_is_unavailable(self):
        g, seen = self.watch(["/nonexistent/nvidia-smi"], lambda seen: "unavailable" in seen)
        self.assertIn("unavailable", seen)
        self.assertIn("nvidia-smi", g.error)


class HttpTest(unittest.TestCase):
    def setUp(self):
        from http.server import ThreadingHTTPServer
        self.sampler = agent.Sampler.__new__(agent.Sampler)
        self.sampler.snap = ("stat 1 2 3 4 5 6 7 h", time.monotonic(), {"cpu": 1})
        self.sampler.display = "not connected"
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), agent.make_handler(self.sampler))
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def get(self, path):
        try:
            with urllib.request.urlopen(self.base + path, timeout=3) as r:
                return r.status, r.read().decode(), r.headers
        except urllib.error.HTTPError as e:
            with e:
                return e.code, e.read().decode(), e.headers

    def test_stat(self):
        code, body, headers = self.get("/stat")
        self.assertEqual((code, body), (200, "stat 1 2 3 4 5 6 7 h\n"))
        self.assertEqual(headers["Content-Length"], str(len(body)))   # firmware requires it

    def test_stale_is_503(self):
        line, at, sample = self.sampler.snap
        self.sampler.snap = (line, at - agent.STALE_S - 1, sample)
        self.assertEqual(self.get("/stat")[0], 503)

    def test_healthz_json(self):
        code, body, _ = self.get("/healthz")
        h = json.loads(body)
        self.assertEqual((code, h["version"], h["display"], h["cpu"]),
                         (200, agent.__version__, "not connected", 1))

    def test_unknown_path_404(self):
        self.assertEqual(self.get("/nope")[0], 404)


# ------------------------------------------------------------------ serial tools

class FakeSerial:
    """Echoes what's written (like the REPL in dumb mode), then replies."""

    def __init__(self, replies):
        self.replies, self.out = replies, b""

    def reset_input_buffer(self):
        self.out = b""

    def write(self, data):
        cmd = data.decode().strip()
        self.out += b"spark>" + data.replace(b"\n", b"\r\n") + self.replies(cmd).encode()

    def flush(self):
        pass

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk

    def close(self):
        pass


class BootingBoard:
    """A display whose console ignores its first `deaf` writes, like one still booting."""

    def __init__(self, deaf, fw=b"fw=sparkesp32 version=0.1.0"):
        self.deaf, self.fw, self.out, self.writes, self.sent = deaf, fw, b"", 0, b""

    def reset_input_buffer(self):
        self.out = b""

    def write(self, data):
        self.writes += 1
        self.sent += data
        if self.writes > self.deaf:
            self.out += b"STATUS-BEGIN\r\n" + self.fw + b"\r\nSTATUS-END\r\n"

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        if not chunk:
            time.sleep(0.005)
        return chunk

    @property
    def in_waiting(self):
        return len(self.out)

    def close(self):
        pass


class AgentHandshakeTest(unittest.TestCase):
    def test_asks_again_while_the_board_boots(self):
        board = BootingBoard(deaf=3)
        self.assertTrue(agent.handshake(board, timeout=2, every=0.05))
        self.assertEqual(board.writes, 4)

    def test_rejects_other_firmware(self):
        self.assertFalse(agent.handshake(BootingBoard(deaf=0, fw=b"fw=other"), timeout=0.3, every=0.05))

    def test_rejects_a_silent_device(self):
        self.assertFalse(agent.handshake(BootingBoard(deaf=10**6), timeout=0.3, every=0.05))


class ChunkedSerial(FakeSerial):
    """Hands its output over a few bytes per read, the way USB packets arrive."""

    def read(self, n):
        return super().read(min(n, 5))


class SerialLoopTest(unittest.TestCase):
    """The link states serial_loop publishes for /healthz, with stand-in ports and boards."""

    def run_loop(self, open_port, done, timeout=4.0):
        with tempfile.NamedTemporaryFile() as dev:     # a port path that exists, like a real node
            saved = agent.candidate_ports, agent.serial.Serial, agent.HANDSHAKE_S
            agent.candidate_ports, agent.serial.Serial, agent.HANDSHAKE_S = (
                lambda: [dev.name]), open_port, 0.3
            sampler = types.SimpleNamespace(display={"state": "searching"},
                                            latest=lambda: ("stat 1 2 3 4 5 6 7 h", time.monotonic(), {}))
            stop = threading.Event()
            loop = threading.Thread(target=agent.serial_loop, args=(sampler, None, stop), daemon=True)
            loop.start()
            try:
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline and not done(sampler.display):
                    time.sleep(0.01)
                return sampler.display, dev.name
            finally:
                stop.set()
                loop.join(3)
                agent.candidate_ports, agent.serial.Serial, agent.HANDSHAKE_S = saved

    def test_a_display_that_answers_is_streaming(self):
        board = BootingBoard(deaf=0)
        d, port = self.run_loop(lambda *a, **k: board, lambda d: d["state"] == "streaming")
        self.assertEqual((d["state"], d["port"]), ("streaming", port))
        self.assertGreaterEqual(d["answered_in_s"], 0)
        self.assertIn(b"stat 1 2 3 4 5 6 7 h", board.sent)

    def test_a_silent_device_is_reported_as_rejected(self):
        d, port = self.run_loop(lambda *a, **k: BootingBoard(deaf=10**6), lambda d: d["state"] == "rejected")
        self.assertEqual((d["state"], d["port"]), ("rejected", port))
        self.assertGreater(d["retry_in_s"], agent.REJECT_S - 10)

    def test_a_port_that_wont_open_says_why(self):
        def denied(*a, **k):
            raise OSError("permission denied")
        d, _ = self.run_loop(denied, lambda d: "error" in d)
        self.assertEqual((d["state"], d["error"]), ("searching", "permission denied"))


def link_with(replies):
    link = sparklink.Link.__new__(sparklink.Link)
    link.port, link.ser = "/dev/fake", FakeSerial(replies)
    return link


class LinkTest(unittest.TestCase):
    def test_end_markers(self):
        def ends(cmd, line):
            return bool(re.search(sparklink.end_pattern(cmd), line, re.M))
        self.assertTrue(ends("status", "STATUS-END"))
        self.assertTrue(ends("wifi", "NET-END"))
        self.assertTrue(ends("wifi scan", "SCAN-END"))
        self.assertFalse(ends("wifi scan", "NET-END"))
        self.assertTrue(ends('wifi ssids "a"', "OK wifi ssids"))
        self.assertTrue(ends("shot", "ERR lock"))          # a multi-line reply can end early...
        self.assertFalse(ends("shot", "ERR+r0w/base64"))   # ...but a base64 row never ends it

    def test_shot_reports_the_firmwares_error_at_once(self):
        link = link_with(lambda c: "ERR lock\r\n")
        with self.assertRaises(sparklink.LinkError) as e:
            devctl.parse_shot(link.raw("shot", timeout=2))
        self.assertEqual(str(e.exception), "ERR lock")

    def test_verdict_split_across_reads_is_read_whole(self):
        link = sparklink.Link.__new__(sparklink.Link)
        link.port = "/dev/fake"
        link.ser = ChunkedSerial(lambda c: "ERR wifi pass invalid (8-63 chars)\r\n")
        self.assertEqual(link.cmd("wifi pass x"), ["ERR wifi pass invalid (8-63 chars)"])

    def test_unopenable_port_is_a_link_error(self):
        with self.assertRaises(sparklink.LinkError):
            sparklink.Link("/dev/sparkesp32-no-such-port")

    def test_echo_and_console_footer_are_not_the_reply(self):
        link = link_with(lambda c: "ERR stat cpu\r\nCommand returned non-zero error code: 0x1 (ERROR)\r\n")
        self.assertEqual(link.cmd("stat 999 1 1 1 1 1 1"), ["ERR stat cpu"])
        with self.assertRaises(sparklink.LinkError) as e:
            link.check("stat 999 1 1 1 1 1 1")
        self.assertIn("ERR stat cpu", str(e.exception))

    def test_secret_command_keeps_only_the_verdict(self):
        link = link_with(lambda c: "I (123) wifi: hunter2-ish log noise\r\nOK wifi pass\r\n")
        lines = link.check('wifi pass "s3cret pass"', echo_safe=False)
        self.assertEqual(lines, ["OK wifi pass"])
        self.assertNotIn("s3cret", " ".join(lines))

    def test_handshake(self):
        good = link_with(lambda c: "STATUS-BEGIN\r\nfw=sparkesp32 version=0.1.0 idf=v5.5.6\r\nSTATUS-END\r\n")
        self.assertTrue(any(ln.startswith("fw=") for ln in good.handshake()))
        bad = link_with(lambda c: "STATUS-BEGIN\r\nfw=other\r\nSTATUS-END\r\n")
        with self.assertRaises(sparklink.LinkError) as e:
            bad.handshake(timeout=0.2, attempt=0.05)
        self.assertIn("not running SparkESP32", str(e.exception))

    def test_handshake_asks_again_while_the_board_boots(self):
        calls = []

        def booting(c):                    # the first two vanish, as they do mid-boot
            calls.append(c)
            return "" if len(calls) < 3 else "STATUS-BEGIN\r\nfw=sparkesp32 version=0.1.0\r\nSTATUS-END\r\n"
        self.assertTrue(link_with(booting).handshake(timeout=2, attempt=0.05))
        self.assertEqual(len(calls), 3)

    def test_handshake_gives_up_on_a_silent_port(self):
        with self.assertRaises(sparklink.LinkError) as e:
            link_with(lambda c: "").handshake(timeout=0.2, attempt=0.05)
        self.assertIn("didn't answer", str(e.exception))

    def test_board_log_lines_are_not_the_reply(self):
        link = link_with(lambda c: "configured=1 connected=0\r\n I (4378) net: joined 'x'\r\n"
                                   "E (5184) esp-tls: delayed connect error\r\nNET-END\r\n")
        self.assertEqual(link.cmd("wifi show"), ["configured=1 connected=0", "NET-END"])

    def test_unrecognized_command_is_an_error(self):
        link = link_with(lambda c: "Unrecognized command\r\n")
        self.assertEqual(link.cmd("bogus"), ["ERR unrecognized command"])

    def test_console_quote(self):
        self.assertEqual(sparklink.console_quote('a "b" \\c'), r'"a \"b\" \\c"')


class ShotTest(unittest.TestCase):
    @staticmethod
    def dump(w, h, strips, corrupt=False):
        out = [f"SHOT-BEGIN {w} {h} rgb565"]
        for (x1, y1, x2, y2) in strips:
            n = (x2 - x1 + 1) * (y2 - y1 + 1) * 2
            data = bytes((x1 + y1 + i) & 0xFF for i in range(n))
            crc = zlib.crc32(data) ^ (1 if corrupt else 0)
            out.append(f"AREA {x1} {y1} {x2} {y2} {n} {crc:08x}")
            out += [base64.b64encode(data[i:i + 72]).decode() for i in range(0, n, 72)]
        return ("\r\n".join(out + ["SHOT-END"]) + "\r\n").encode()

    def test_reassembles_strips(self):
        w, h, fb = devctl.parse_shot(self.dump(4, 4, [(0, 0, 3, 1), (0, 2, 3, 3)]))
        self.assertEqual((w, h, len(fb)), (4, 4, 32))
        self.assertEqual(fb[16], 2)            # first byte of the second strip (x1+y1 = 2)

    def test_rejects_bad_crc_and_gaps(self):
        with self.assertRaises(sparklink.LinkError):
            devctl.parse_shot(self.dump(4, 4, [(0, 0, 3, 3)], corrupt=True))
        with self.assertRaises(sparklink.LinkError):
            devctl.parse_shot(self.dump(4, 4, [(0, 0, 3, 1)]))

    def test_rejects_a_strip_missing_whole_lines(self):
        # What 1.0.0's C6 sent now and then: whole base64 lines gone from the
        # middle of a strip, every remaining line intact.
        lines = self.dump(48, 4, [(0, 0, 47, 3)]).split(b"\r\n")
        del lines[4:6]                        # 2 of the strip's 6 lines
        with self.assertRaises(sparklink.LinkError) as e:
            devctl.parse_shot(b"\r\n".join(lines))
        self.assertIn("strip at 0,0 failed its size/CRC check", str(e.exception))

    def test_reports_the_firmwares_error_mid_dump(self):
        # The board stops a dump the host isn't reading with an ERR line in place
        # of SHOT-END: show those words, not a coverage count.
        lines = self.dump(48, 4, [(0, 0, 47, 1), (0, 2, 47, 3)]).split(b"\r\n")
        between = next(i for i, ln in enumerate(lines) if ln.startswith(b"AREA 0 2"))
        for cut in (between, between - 1):    # after a whole strip, and inside one
            raw = b"\r\n".join(lines[:cut] + [b"ERR shot usb write timed out", b""])
            with self.assertRaises(sparklink.LinkError) as e:
                devctl.parse_shot(raw)
            self.assertEqual(str(e.exception), "ERR shot usb write timed out")


# ------------------------------------------------------------------ Sparkline

FIXTURE = os.path.join(ROOT, "tests", "fixtures", "sparkline-snapshot.json")


def snapshot(**changes):
    with open(FIXTURE) as f:
        s = json.load(f)
    s["ts"] = time.time()
    s.update(changes)
    return s


class SnapshotFieldsTest(unittest.TestCase):
    def test_maps_a_real_snapshot(self):
        # recorded from a Spark next to the agent's own line: stat 0 0 99646 130661 36 107 ...
        got = agent.snapshot_fields(snapshot())
        self.assertEqual((got["cpu"], got["gpu"], got["mem_used_mb"], got["mem_total_mb"]),
                         (0, 0, 99646, 130661))
        self.assertEqual((got["temp_c"], got["power_dw"]), (36, 107))
        self.assertEqual(len(got["cores"]), 20)

    def test_clamps_and_unknowns(self):
        s = snapshot()
        s["cpu"]["util"], s["cpu"]["per_core"][0] = 100.4, -0.2
        s["gpu"][0].update(util=None, temp_c=None, power_w=None)
        got = agent.snapshot_fields(s)
        self.assertEqual((got["cpu"], got["cores"][0]), (100, 0))
        self.assertEqual((got["gpu"], got["temp_c"], got["power_dw"]), (None, None, None))
        self.assertEqual(agent.snapshot_fields(snapshot(gpu=[]))["gpu"], None)   # no GPU at all

    def test_rejects_what_it_doesnt_recognize(self):
        for bad in ({"cpu": {}}, snapshot(mem=None), snapshot(gpu="oops"), snapshot(cpu={"util": "x"})):
            with self.subTest(bad=str(bad)[:40]), self.assertRaises((KeyError, TypeError, AttributeError)):
                agent.snapshot_fields(bad)


class SparklineSourceTest(unittest.TestCase):
    """SparklineSource against a stand-in Sparkline agent on 127.0.0.1."""

    TOKEN = "t" * 43

    def serve(self, status=200, body=None):
        test, seen = self, []

        class Handler(agent.BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append((self.path, self.headers.get("Authorization")))
                data = (json.dumps(snapshot()) if body is None else body).encode()
                self.send_response(status)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass
        httpd = agent.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        test.addCleanup(httpd.server_close)
        test.addCleanup(httpd.shutdown)
        return f"http://127.0.0.1:{httpd.server_port}", seen

    def test_reads_with_the_bearer_token(self):
        url, seen = self.serve()
        src = agent.SparklineSource(url, self.TOKEN)
        self.assertEqual(src.read()["mem_total_mb"], 130661)
        self.assertEqual(seen, [("/api/v1/snapshot", "Bearer " + self.TOKEN)])
        self.assertEqual(src.health(), {"url": url, "state": "ok", "error": None})

    def test_token_rejected(self):
        url, _ = self.serve(status=401, body='{"detail": "Missing or invalid bearer token"}')
        src = agent.SparklineSource(url, self.TOKEN)
        self.assertIsNone(src.read())
        self.assertIn("token rejected", src.error)
        self.assertNotIn(self.TOKEN, json.dumps(src.health()))

    def test_stale_garbled_and_unrecognized_snapshots(self):
        for body, why in [(json.dumps(snapshot(ts=time.time() - 60)), "old"),
                          ("not json", "Expecting value"),
                          (json.dumps({"ts": time.time()}), "unrecognized"),
                          (json.dumps(snapshot(gpu="oops")), "unrecognized")]:
            with self.subTest(why=why):
                url, _ = self.serve(body=body)
                src = agent.SparklineSource(url, self.TOKEN)
                self.assertIsNone(src.read())
                self.assertIn(why, src.error)

    def test_unreachable_then_waits_before_asking_again(self):
        url, seen = self.serve()
        dead = agent.SparklineSource("http://127.0.0.1:9", self.TOKEN)     # nothing listens on 9
        self.assertIsNone(dead.read())
        self.assertEqual(dead.state, "error")
        src = agent.SparklineSource(url, self.TOKEN)
        src._retry_at = time.monotonic() + 60       # as if it had just failed
        self.assertIsNone(src.read())
        self.assertEqual(seen, [])                  # it didn't ask during the backoff


if __name__ == "__main__":
    unittest.main(verbosity=1)
