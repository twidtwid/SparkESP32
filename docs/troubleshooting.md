# Troubleshooting

Start with the state in the top-right corner of the screen, then the three commands that
explain almost everything:

```bash
systemctl status sparkesp32                       # on the Spark: is the agent running?
curl -s localhost:8787/healthz | python3 -m json.tool   # what it samples; is the display connected?
journalctl -u sparkesp32 -n 50 --no-pager         # agent log
```

To see the display's side of things, plug it into any machine with this repo and
`pip install pyserial`, stop the agent first if that machine is the Spark
(`sudo systemctl stop sparkesp32`), and run:

```bash
python3 tools/devctl.py status     # firmware version, settings, last sample
python3 tools/devctl.py wifi       # Wi-Fi link, poll counters, last_err
```

Start the agent again afterwards (`sudo systemctl start sparkesp32`): only one program
can talk to the display's serial port at a time.

## The screen is black

- Try another cable. Many USB-C cables are charge-only, and the board needs data to be
  flashed and fed (it powers up on either).
- `lsusb | grep 303a:1001` on the Spark. If it's missing, the board isn't enumerating:
  check the cable and the port, then `dmesg | tail`.
- A brand-new board runs Waveshare's demo firmware until you install. Run `sudo ./install.sh`.

## WAITING, with the board plugged into the Spark

Nothing has arrived over USB yet.

- `systemctl status sparkesp32`. If it isn't running, `journalctl -u sparkesp32` says why.
- `/healthz` → `"display"` → `"state"`:
  - `searching`: the agent can't see the board on USB. An `"error"` says why opening a
    port failed. Make sure no other program has the port open (`screen`, `minicom`,
    `devctl.py`, an IDE monitor) with `sudo fuser -v /dev/ttyACM*`. Then re-run
    `sudo ./install.sh`, which also reflashes if the board isn't running SparkESP32.
  - `handshaking`: it's asking the board who it is (up to 10 s after it's plugged in).
  - `rejected`: the board on `"port"` didn't answer as SparkESP32 within 10 s, so the
    agent skips it for `"retry_in_s"` more seconds. Unplug it and plug it back in (a
    replugged board is retried at once). If it's rejected again, it's running different
    firmware: re-run `sudo ./install.sh` to flash it.
  - `streaming`: all good; `"answered_in_s"` is how long the board took to answer.
- ModemManager probing the port garbles the console. The installer's udev rule stops that.
  Check it's present: `cat /etc/udev/rules.d/99-sparkesp32.rules`.

## OFFLINE

The link is up but samples stopped. The screen's second line says why (over Wi-Fi):

| Second line | Meaning | Fix |
|---|---|---|
| agent stopped | the Spark refused the connection: nothing listens on :8787 | `systemctl status sparkesp32`; `sudo systemctl restart sparkesp32` |
| Spark unreachable | no answer, or `<host>.local` doesn't resolve | is the Spark on? Same LAN? Try `--host <ip>`; is ufw allowing your subnet? |
| agent stale | HTTP 503: the agent is up but its sampler stalled | `journalctl -u sparkesp32` |
| bad reply | another service answers, or an old agent | check the URL (`devctl.py wifi`); update the agent |

- Over USB there is no second line: the agent stopped (`systemctl status sparkesp32`) or the Spark is off.
- For the raw details, run `devctl.py wifi` on a machine the board is plugged into. Its
  `link` line shows the link state and the last poll (`feed=`), and `last_err` the exact error:

| `last_err` | Meaning | Fix |
|---|---|---|
| `connect ESP_ERR_HTTP_CONNECT` | can't reach the agent | Is `<host>.local` resolvable on your LAN? Try `--host <ip>`. Is ufw allowing your subnet (`sudo ufw status`)? Is the board on an isolated guest/IoT network? |
| `http 503` | agent is up but its sampler stalled | `journalctl -u sparkesp32`; `sudo systemctl restart sparkesp32` |
| `http 404` | something else answers on that port | check the URL: `devctl.py wifi` shows it; fix with `--host` |
| `bad stat line (too many fields: update the agent)` | the agent speaks an older protocol | `git pull && sudo ./install.sh` on the Spark |
| `bad stat line (<field>)` | a value out of range | open an issue with the `/stat` output |

From another machine on the same LAN, `curl http://<spark>.local:8787/stat` should
print one `stat ...` line.

## NO WI-FI

Wi-Fi is configured but the board can't join. The second line is `joining...` while it
tries, then `no network` (reason 201/212), `wrong password` (15/202/204) or `can't join`
(anything else). `devctl.py wifi` shows the `link` state and `last_err` as
`join '<ssid>' reason <n>`:

| reason | Meaning | Usually |
|---|---|---|
| 201 | no AP found | wrong name, or the network is 5/6 GHz only (the ESP32-C6 and S3 are **2.4 GHz only**) |
| 15, 204 | 4-way handshake timeout | wrong password |
| 202 | authentication failed | wrong password, or WPA3-only network with incompatible settings |
| 210, 211 | no AP with compatible security | the network needs something other than WPA2/WPA3-Personal (e.g. 802.1X), or it's WEP/open-only with odd settings |
| 2, 200 | auth expired / beacon timeout | weak signal; move the board closer |

`python3 tools/devctl.py wifi scan` lists what the board can see, with signal strength
and security. To fix the settings, re-run `sudo ./install.sh --ssids "<name>"` (the
password is prompted). The board rotates through up to 4 SSIDs, moving to the next
after 3 failed joins.

## SET HOST

The board is on Wi-Fi but has no agent address. Run `sudo ./install.sh --host <spark-name>.local`,
or `--host <ip>`.

## GPU, temperature or power show `--`

The agent couldn't read them from `nvidia-smi`. Check `/healthz` → `"gpu_source"`
(and `"gpu_error"` for the details):

- `unavailable`: the NVIDIA driver/tools aren't installed or aren't on `PATH` for the
  service.
- `exited`: it crashed or the driver isn't loaded; `nvidia-smi` by hand will say why.
  The agent restarts it with backoff.
- `stalled`: it stopped printing without exiting (a hung driver call). The agent kills
  and restarts it; if this keeps happening, the driver itself is stuck.

CPU, memory and the core heatmap don't depend on `nvidia-smi`.

## The image is sideways

`sudo ./install.sh --rotation none|ccw|cw`. The default `cw` is upright with the board's
USB-C port on the right. A board with a rectangular screen is laid out for one shape and
takes only the two rotations that keep it: the S3 reflective LCD is landscape, `ccw` or
`cw`, and answers `ERR rot none not supported on this board` to the third.

## Flashing fails

- Hold the board's **BOOT** button while plugging it in (forces the ROM bootloader), then
  re-run `sudo ./install.sh --reflash`.
- `several Espressif USB devices attached`: pick one with `--port /dev/ttyACMn`.
- `couldn't identify the board` or `pick its board with --board`: the board isn't running
  SparkESP32 yet and its chip matches more than one supported board, or none. Name it:
  `sudo ./install.sh --board <id>` (`./install.sh --help` lists the ids).
- esptool says the image `is not an ESP32-...` image: the firmware is for a different
  board than the one plugged in. Check `--board`. The installer resets the board
  afterwards, so it isn't left stuck in its bootloader.

## Starting over

```bash
sudo ./install.sh --uninstall         # service, udev rule, ufw rule, /opt/sparkesp32
python3 tools/devctl.py wifi clear    # optional: make the board forget its Wi-Fi
```

The board keeps its name and Wi-Fi settings across reinstalls and firmware updates.
Only a merged-image flash or `wifi clear` erases them.
