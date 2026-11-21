# Wire protocol (v1)

One sample is one line of ASCII text. The same line travels both ways to the display:
written to its USB serial console, or returned as the body of `GET /stat`.

```
stat <cpu> <gpu> <mem_used_mb> <mem_total_mb> <temp_c> <power_dw> <cores> <host>
```

| Field | Unit | Range | Source on the Spark |
|---|---|---|---|
| `cpu` | % | 0–100 | `/proc/stat` aggregate, (busy) / (total) between samples; idle includes iowait |
| `gpu` | % | 0–100 | `nvidia-smi --query-gpu=utilization.gpu` |
| `mem_used_mb` | decimal MB | 0–2³¹−1 | `/proc/meminfo`: MemTotal − MemAvailable |
| `mem_total_mb` | decimal MB | 0–2³¹−1 | `/proc/meminfo`: MemTotal |
| `temp_c` | °C | −50–150 | `nvidia-smi --query-gpu=temperature.gpu` (the dashboard's "Device Temp") |
| `power_dw` | deciwatts | 0–100000 | `nvidia-smi --query-gpu=power.draw` × 10 (the dashboard's "Device Power") |
| `cores` | % each | 0–100 | `/proc/stat` per-core, comma-separated, no spaces: `3,0,12,…` |
| `host` | text | 1–63 chars of `A–Z a–z 0–9 - _ .` | the short hostname (`hostname -s`); the display's title unless a name is saved on the board |

- **Unknown is `-`.** Any field may be `-` when the host can't measure it (no
  `nvidia-smi`, a GB10 field that reports `[N/A]`, a stale reading). The display shows
  `--`. For `cores`, `-` means "no per-core data".
- **Memory is decimal** (1 MB = 10⁶ bytes) so the panel agrees with NVIDIA's dashboard
  (e.g. 99 GB of 131 GB) rather than showing GiB.
- **Power is in deciwatts** so every field is an integer: `410` = 41.0 W.
- **Cores** beyond 32 are accepted and ignored (the heatmap is one row). The GB10 has 20.

Example from a DGX Spark under GPU load:

```
stat 6 91 46328 130661 60 410 1,1,1,2,0,0,1,0,1,1,0,0,1,1,1,1,99,1,0,0 spark-1a2b
```

## Validation

The firmware parser (`firmware/main/spark_data.c`, tested by `tests/test_parser.c`) is
strict and all-or-nothing:

- exactly 9 space-separated tokens, the first being `stat`;
- every number is a complete base-10 integer within its range (`5x`, `0x10`, `1e3` are rejected);
- `cores` has no empty entries (`5,,5`, `,5`, `5,` are rejected);
- if any field fails, the previous sample is kept untouched.

- `host` contains only hostname characters.

Per-core values share one token because ESP-IDF's console splits a line into at most 31
arguments, and a token per core would run out on bigger machines.

## USB transport

The display enumerates as an Espressif USB-Serial/JTAG device, **VID:PID `303a:1001`**
(`/dev/ttyACM*` on Linux, `/dev/cu.usbmodem*` on macOS), 115200 baud (ignored by USB CDC).

The agent writes one `stat` line per second, terminated by `\n`. The board replies
`OK stat` or `ERR stat <field>`. The console echoes input, and the agent discards
everything it reads back.

Before writing anything, a host must confirm it's talking to SparkESP32 firmware. Send
`status` and look for a line starting `fw=sparkesp32` before `STATUS-END`:

```
STATUS-BEGIN
fw=sparkesp32 version=1.0.0 idf=v5.5.6 board=waveshare-esp32c6-amoled-2.16
name='Lab Spark' rot=cw
heap_free=81160 min=72272
stack_free repl=6344 net=7428 tick=4852
valid=1 age_ms=412 src=usb host=spark-1a2b cpu=6 gpu=91 mem=46328/130661MB temp=60 power_dw=410 cores=20
screen=online reason='via USB'
wifi configured=1 connected=1 url=1
STATUS-END
```

`board=` is the board's directory under `firmware/boards/`. The installer uses it to
pick the firmware to flash. Firmware from before boards (and factory firmware) has no
`board=`: the installer then asks the chip with esptool, and when more than one board
uses that chip it asks for `--board`.

**Ask more than once.** The serial port appears the moment the board is plugged in or
reset, about 2 s before its console is running, and the console's terminal probe (IDF's
`linenoiseProbe`) then discards everything typed in its first 0.5 s. A single `status`
sent to a board that's still booting just vanishes. Resend it every second or two until it
answers; the agent and `tools/sparklink.py` keep trying for 10 s.

A sample over USB counts as fresh for 3 s. While it's fresh, the board skips its Wi-Fi poll.

**Keep every console line under 256 bytes.** ESP-IDF's console line editor (linenoise,
in the "dumb terminal" mode a program talking to it gets) reads into a 256-byte buffer.
A line that fills it isn't terminated, and the rest of it is read as a second
command. That's an upstream limitation. The agent stays well clear by sending at most 32
per-core values (all the heatmap shows), which bounds a `stat` line at about 240 bytes.

## HTTP transport

The agent serves, on port 8787 by default:

| Request | Response |
|---|---|
| `GET /stat` | `200`, `text/plain`, the line + `\n`, with `Content-Length`. `503 stale` if the sampler hasn't produced a line in 5 s. |
| `GET /healthz` | `200`, JSON: version, sample age, display connection, each value broken out, and where the readings come from (`source`: `own` or `sparkline`) |
| anything else | `404` |

The board polls `GET /stat` once a second with a 2.5 s timeout. It needs status 200, a
`Content-Length`, and the full body. Plain `http://` only, LAN only: the payload is
read-only machine vitals, and the installer's ufw rule only allows the local subnet.

## Console commands

The rest of the console, used by `tools/provision.py` and `tools/devctl.py`. Every
command's reply ends with a line starting `OK` or `ERR`, or with an `*-END` marker for
multi-line output. A multi-line command that can't run says `ERR ...` instead and stops
there (`shot` answers `ERR lock` when the UI is busy, and ends in
`ERR shot usb write timed out` instead of `SHOT-END` if the host stops reading mid-dump).

| Command | Reply ends with | |
|---|---|---|
| `stat …` | `OK stat` / `ERR stat <field>` | one sample |
| `name "<text>"` | `OK name` | fixed header title, ≤24 printable ASCII, saved; `name ""` = use the feed's `host` |
| `rot [none\|ccw\|cw]` | `OK rot [<current>]` | rotation; saved. `ERR rot <r> not supported on this board` for a rotation the board's layout can't show |
| `wifi [show]` | `NET-END` | config + link + poll counters (never the password) |
| `wifi ssids "<a,b>"` | `OK wifi ssids` | ≤4 SSIDs, each 1–32 bytes, tried in order |
| `wifi pass "<pw>"` | `OK wifi pass` | 8–63 chars, or `""` for an open network; dropped from console history |
| `wifi url "<url>"` | `OK wifi url` | `http://host[:port]/stat`, ≤127 chars |
| `wifi scan` | `SCAN-END` | visible 2.4 GHz networks |
| `wifi clear` | `OK wifi clear` | forget SSIDs, password, URL |
| `status` | `STATUS-END` | see above; line 1 is the handshake |
| `demo [load]` | `OK demo <n>` | synthetic sample, for bench QA |
| `shot` | `SHOT-END` | CRC'd base64 RGB565 framebuffer dump (`devctl.py shot` reassembles it) |

Arguments with spaces or quotes use the console's quoting: wrap the argument in `"…"`,
and escape `\"` and `\\` inside it.
