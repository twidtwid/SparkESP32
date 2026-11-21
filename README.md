# SparkESP32

**A status display and power light for the NVIDIA DGX Spark, on an ESP32 AMOLED board that costs under $40.**

The DGX Spark has no power LED and nothing on the outside that tells you what it's doing.
SparkESP32 puts a 2.16" 480×480 AMOLED next to it that shows CPU, GPU and unified-memory
load, a per-core heatmap, device temperature and power draw, and a big **ONLINE / OFFLINE**
state that doubles as the power light.

![SparkESP32 on a DGX Spark](assets/social-preview.png)

- **Plug-and-go over USB-C.** Plug the board into the Spark; a small agent pushes a sample
  every second. No network setup.
- **Or over Wi-Fi.** Put the board anywhere on your LAN and it polls the Spark once a second.
- **Honest numbers.** GPU %, temperature and power come from `nvidia-smi`; CPU and memory
  from `/proc`. Memory is shown in decimal GB, so it matches NVIDIA's dashboard (99 GB of 131 GB).
- **Tells you when it doesn't know.** A missing reading shows as `--`, never as a fake zero.
  A feed that stops turns the screen to OFFLINE within 5 seconds.

> Unofficial hobby project. Not affiliated with or endorsed by NVIDIA. "NVIDIA" and
> "DGX Spark" are trademarks of NVIDIA Corporation.

## What you need

| | |
|---|---|
| Display | [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm) (SKU 34202 or 34201), the recommended board. See [Supported boards](#supported-boards) for the others. |
| Cable | USB-C to USB-C **data** cable (some charge-only cables won't enumerate) |
| Host | A DGX Spark (DGX OS / Ubuntu 24.04, aarch64). Any Linux box with an NVIDIA GPU should work too; that's untested. |

No ESP-IDF install is needed. The installer flashes prebuilt firmware from this repo's
releases.

### Supported boards

| Board | |
|---|---|
| [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm) | **Recommended.** 2.16" 480×480 color AMOLED. The reference board: everything here is shown and tested on it. |
| [Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm) | 4.2" black-and-white reflective LCD, landscape, no backlight. Same features in a mono layout. |

The installer recognizes the board and flashes the right firmware. To port SparkESP32 to
another ESP32 board, see [docs/adding-a-device.md](docs/adding-a-device.md); to submit
one, [CONTRIBUTING.md](CONTRIBUTING.md#adding-a-new-device).

## Quick start (on the Spark)

```bash
git clone https://github.com/twidtwid/SparkESP32.git
cd SparkESP32
sudo ./install.sh
```

Plug the display into one of the Spark's USB-C ports first. The installer:

1. flashes the firmware (only if the board isn't already on this version),
2. points the display at this Spark's agent. The display's title is the Spark's hostname,
   sent live by the agent, unless you pick a name,
3. offers to set up Wi-Fi. It lists the 2.4 GHz networks the board can see, and you type
   the password at a hidden prompt,
4. installs the agent as the `sparkesp32` systemd service (runs as you, not root), plus a
   udev rule that keeps ModemManager off the board, and a ufw rule allowing your LAN to reach port 8787,
5. checks the whole thing end to end.

Within a couple of seconds the screen reads **ONLINE · via USB**. The installer is
safe to re-run. It never overwrites a setting you didn't ask it to change.

## Configuring

All settings live in the board's flash. They survive power cycles and firmware updates.
Re-run the installer with what you want to change:

```bash
sudo ./install.sh --name "Lab Spark"          # fixed title, up to 24 characters
sudo ./install.sh --name ""                   # back to the Spark's hostname (the default)
sudo ./install.sh --ssids "HomeNet"           # join Wi-Fi; password prompted, hidden
sudo ./install.sh --ssids "HomeNet,HomeNet-IoT"   # several, tried in order (max 4)
sudo ./install.sh --host 192.168.1.50         # what the board polls over Wi-Fi
sudo ./install.sh --rotation ccw              # none | ccw | cw (default cw: USB-C on the right;
                                              # landscape-only boards take ccw or cw)
sudo ./install.sh --clear-wifi                # forget Wi-Fi; USB keeps working
```

**Wi-Fi notes**

- The ESP32-C6 and ESP32-S3 are **2.4 GHz only**. If your router splits bands, use the 2.4 GHz name.
  WPA2 and WPA3 both work. Captive portals and enterprise (802.1X) networks don't.
- The board finds the Spark by name (`<hostname>.local`, via the Spark's avahi/mDNS). If
  that doesn't resolve on your network, use `--host <ip>` and give the Spark a DHCP
  reservation.
- The board and the Spark must be able to reach each other. Some routers isolate their
  "IoT" or guest networks.
- USB always wins: while the board is plugged into the Spark it uses USB, even if Wi-Fi is set up.

**Setting up from a Mac or PC.** To configure the board without plugging it into the
Spark, connect it to your computer and use the same tool the installer uses:

```bash
pip install pyserial
python3 tools/provision.py --name "Lab Spark" --host spark-1234.local --ssids "HomeNet"
```

To flash a board from a computer, download `sparkesp32-<board>-firmware-<version>.tar.gz`
for your board from [Releases](https://github.com/twidtwid/SparkESP32/releases) and run
`python3 -m esptool write_flash @flash_args` inside the unpacked folder. esptool refuses
an image built for a different chip.
The release also has a single merged image for browser flashers like
[ESP Web Tool](https://espressif.github.io/esptool-js/). Use the merged image only on a
board you haven't set up yet, because it erases the saved settings.

## Reading the screen

| Top right | Second line | Meaning |
|---|---|---|
| ● **ONLINE** | via USB / via WI-FI | a fresh sample arrived in the last 5 s |
| ● **OFFLINE** | agent stopped | the Spark answers, but nothing listens on :8787 |
| | Spark unreachable | no answer at all: the Spark is asleep, off, or off the network |
| | agent stale | the agent is up but its samples stopped |
| | bad reply | something answered that isn't the agent (or an old one) |
| ● **NO WI-FI** | joining... / no network / wrong password / can't join | Wi-Fi is configured but the board isn't on it |
| ● **SET HOST** | | on Wi-Fi, but no agent address is set (`--host`) |
| ● **WAITING** | | nothing on USB and no Wi-Fi configured yet |

Over USB alone, OFFLINE has no second line: the board can't tell why samples stopped.

Rings and the heatmap dim to grey whenever the data is stale. A reading the Spark can't
provide (e.g. `nvidia-smi` missing) shows `--`.

## For agents

If you're an AI coding agent asked to set this up for someone, follow
**[AGENTS.md](AGENTS.md)**. It's a deterministic runbook with exact commands, a definition
of done, and the one rule that matters: **the human types the Wi-Fi password, never you.**

## How it works

```
DGX Spark                                         ESP32-C6 AMOLED
┌──────────────────────────────────┐   USB-C    ┌──────────────────────────┐
│ sparkesp32-agent (systemd, 1 Hz) │ ─────────► │ console `stat` line      │
│   nvidia-smi   GPU %, temp, power│   serial   │          │               │
│   /proc/stat   CPU % + each core │            │          ▼  USB wins     │
│   /proc/meminfo unified memory   │            │  one strict parser       │
│                                  │   Wi-Fi    │          ▲  while fresh  │
│ GET :8787/stat  (same line)      │ ◄───────── │  1 Hz HTTP poll          │
└──────────────────────────────────┘    LAN     └──────────────────────────┘
```

Both transports carry the same one-line text format
(`stat 6 91 46328 130661 60 410 1,1,0,... spark-1a2b`), described in
[docs/protocol.md](docs/protocol.md). The agent is ~300 lines of stdlib Python plus pyserial.
The firmware is ESP-IDF 5.5 + LVGL 9.

## Troubleshooting

See [docs/troubleshooting.md](docs/troubleshooting.md). The first three things to check:

```bash
systemctl status sparkesp32          # is the agent running?
curl -s localhost:8787/healthz       # what is it sampling, and is the display connected?
journalctl -u sparkesp32 -n 50       # why not?
```

## Development

```bash
make check        # unit tests (agent, serial tools, firmware parser and state machines
                  # on the host), board checks, ruff, shellcheck
make build        # firmware; needs ESP-IDF v5.5.x exported (. ~/esp/esp-idf/export.sh)
make build BOARD=waveshare-esp32s3-rlcd-4.2   # another board (make boards lists them)
make flash PORT=/dev/ttyACM0
make dist         # every board's release bundle in dist/
python3 tools/devctl.py demoshot shot.png 70   # synthetic load + CRC-checked screenshot
```

Repo map, conventions and the release process are in [AGENTS.md](AGENTS.md) and
[CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) © 2026 Todd Dailey. Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
