# Wire protocol (v2)

One sample is one line of ASCII text. The same line travels both ways to the display:
written to its USB serial console, or returned as the body of `GET /stat`.

```
stat <cpu> <gpu> <mem_used_mb> <mem_total_mb> <temp_c> <power_dw> <cores> <host> <gpu_mhz> <gpu_stuck>
```

A **v1** line is the first 9 tokens, without `gpu_mhz` and `gpu_stuck`. Agents and
firmware from before 1.2 speak only v1; see [Versions](#versions) for how each side
picks the form the other understands.

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
| `gpu_mhz` | MHz | 0–10000 | `nvidia-smi --query-gpu=clocks.sm` (v2) |
| `gpu_stuck` | flag | 0 or 1 | the agent's verdict on the GB10 stuck-clock state (v2): `1` stuck, `0` seen healthy under load, `-` not enough load yet to tell. See below. |

- **Unknown is `-`.** Any field may be `-` when the host can't measure it (no
  `nvidia-smi`, a GB10 field that reports `[N/A]`, a stale reading). The display shows
  `--`. For `cores`, `-` means "no per-core data".
- **Memory is decimal** (1 MB = 10⁶ bytes) so the panel agrees with NVIDIA's dashboard
  (e.g. 99 GB of 131 GB) rather than showing GiB.
- **Power is in deciwatts** so every field is an integer: `410` = 41.0 W.
- **Cores** beyond 32 are accepted and ignored (the heatmap is one row). The GB10 has 20.

Example from a DGX Spark under GPU load:

```
stat 6 91 46328 130661 60 410 1,1,1,2,0,0,1,0,1,1,0,0,1,1,1,1,99,1,0,0 spark-1a2b 2405 0
```

### The stuck-clock verdict

Some GB10 machines (DGX Spark and its OEM versions) get into a state where the GPU runs at
500–850 MHz under full load instead of ~2,000–2,400 MHz, draws ~15–20 W instead of ~90 W,
and reports no clock event reason. It survives a reboot; a full power drain clears it
(NVIDIA forum threads [386060](https://forums.developer.nvidia.com/t/386060),
[387029](https://forums.developer.nvidia.com/t/387029) and
[376039](https://forums.developer.nvidia.com/t/376039)).

The agent decides `gpu_stuck` from one reading a second of GPU utilization, SM clock and
`clocks_event_reasons.active`, with the thresholds of the community's detector
([blackbox](https://github.com/lcasarin-maker/blackbox)). Only readings at 80 %
utilization or more count, because GB10 idle clocks float and say nothing:

- a reading looks **stuck** below 1000 MHz when no event reason other than idle is active
  (a thermal or power slowdown, a power cap or an application clock limit explains a low
  clock); 10 in a row make `gpu_stuck` `1`;
- a reading looks **healthy** at 1400 MHz or more; 5 in a row make it `0`;
- anything in between restarts both counts; readings without load change nothing, so the
  verdict holds through idle stretches, and a bursty workload adds up.

The verdict starts as `-` when the agent starts, which a power drain implies.

## Validation

The firmware parser (`firmware/main/spark_data.c`, tested by `tests/test_parser.c`) is
strict and all-or-nothing:

- exactly 9 (v1) or 11 (v2) space-separated tokens, the first being `stat`;
- every number is a complete base-10 integer within its range (`5x`, `0x10`, `1e3` are rejected);
- `cores` has no empty entries (`5,,5`, `,5`, `5,` are rejected);
- if any field fails, the previous sample is kept untouched.

- `host` contains only hostname characters.

A v1 line leaves the board's `gpu_mhz` and `gpu_stuck` unknown: the GPU ring then shows no
clock, as before v2.

## Versions

Boards and agents are updated separately (a board on Wi-Fi somewhere else keeps its
firmware when the Spark's agent is updated), so each side sends the newer form only to a
reader that asks for it:

- **USB.** The board's handshake line ends in `proto=2`. The agent sends v2 lines to a
  board that says so, and v1 lines to one that doesn't (firmware before 1.2).
- **HTTP.** `GET /stat` answers v1; `GET /stat?v=2` answers v2. Firmware from 1.2 on
  appends `?v=2` (or `&v=2`) to its configured URL. Every agent release ignores a query
  it doesn't know and answers v1, which the new parser still takes.

Per-core values share one token because ESP-IDF's console splits a line into at most 31
arguments, and a token per core would run out on bigger machines.

## USB transport

Most boards enumerate as an Espressif USB-Serial/JTAG device, **VID:PID `303a:1001`**
(`/dev/ttyACM*` on Linux, `/dev/cu.usbmodem*` on macOS). A board whose USB port is a
USB-UART bridge enumerates as the bridge: CH340 `1a86:7523`, CH343 `1a86:55d3`, CH9102
`1a86:55d4` or CP210x `10c4:ea60` (`/dev/ttyUSB*`, `/dev/cu.usbserial-*`), with its
console on UART0. Hosts open every port at **115200** baud: a bridge needs it, and USB
CDC ignores it. On a bridge that doesn't answer at 115200 they ask again at 921600, the
rate 1.3.0 used, so an older board is still found and can be upgraded.

Hosts look at native ports first: the tools and the installer consider bridges only when
no native port is attached, and the agent tries native ports before bridges.

Nothing in a device's USB descriptors tells a display from another Espressif board or
another serial adapter: a host has to open the port and ask (the handshake below). So that
the agent doesn't do that to whatever is plugged into the Spark, `install.sh
--only-this-display` saves what identifies the display it has seen answer, in
`/etc/sparkesp32/display.env`: its USB IDs
and serial number (`303a:1001:<the chip's MAC>`), or for a bridge without a serial number
(the CH340) the USB socket it's in (`1a86:7523@1-2.3:1.0`). With a display saved, the agent
opens that device and no other; `/healthz` lists the ones it leaves alone under
`"ignoring"`. A display moved to the Spark from elsewhere, a replacement, or a CH340 board
moved to another socket needs `sudo ./install.sh --only-this-display` once. `--any-display` forgets the saved
one.

With no display saved, the agent asks each Espressif or bridge device once when it's
plugged in (and when the agent starts). One that doesn't answer is left alone until it's
replugged (a new device node, even at the same path), since it may be some other gadget.
The saved display, or the one `--port` names, is asked again every 5 minutes.

Hosts open a port with an advisory lock (`flock`). That keeps them out of a port another
program has locked the same way, and out of each other's way; it doesn't show that a
program has the port open without locking it.

The agent writes one `stat` line per second, terminated by `\n`. The board replies
`OK stat` or `ERR stat <field>`. The console echoes input, and the agent discards
everything it reads back.

Before writing anything, a host must confirm it's talking to SparkESP32 firmware. Send
`status` and look for a line starting `fw=sparkesp32` before `STATUS-END`:

```
STATUS-BEGIN
fw=sparkesp32 version=1.2.0 idf=v5.5.6 board=waveshare-esp32c6-amoled-2.16 proto=2
name='Lab Spark' rot=cw
heap_free=81160 min=72272
stack_free repl=6344 net=7428 tick=4852
valid=1 age_ms=412 src=usb host=spark-1a2b cpu=6 gpu=91 mem=46328/130661MB temp=60 power_dw=410 cores=20 gpu_mhz=2405 gpu_stuck=0
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
command. That's an upstream limitation. The agent stays under it by sending at most 32
per-core values (all the heatmap shows), which bounds a v2 `stat` line at 245 bytes
(`tests/test_agent.py` checks the worst case).

## HTTP transport

The agent serves, on port 8787 by default:

| Request | Response |
|---|---|
| `GET /stat` | `200`, `text/plain`, the v1 line + `\n`, with `Content-Length`. `503 stale` if the sampler hasn't produced a line in 5 s. |
| `GET /stat?v=2` | the same, with the v2 line |
| `GET /healthz` | `200`, JSON: version, sample age, display connection, each value broken out, and where the readings come from (`source`: `own` or `sparkline`). Asked from another machine, `display` and `sparkline` hold only their `state`, and `gpu_error` is left out: the serial port's path, Sparkline's URL and error texts are for the Spark itself (`curl localhost:8787/healthz`). |
| anything else | `404` |

The board polls `GET /stat` once a second with a 2.5 s timeout. It needs status 200, a
`Content-Length`, and the full body. Plain `http://` only, LAN only: the payload is
read-only machine vitals, and the installed service answers only this host and the local
subnet (`install.sh --lan` for another), as does the installer's ufw rule. The agent serves
at most 16 requests at once and hangs up on one more.

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
