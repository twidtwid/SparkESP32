# Security Policy

## Supported versions

Security fixes are made against the latest release and the default branch.

## Reporting a vulnerability

Please do not open a public issue for a suspected vulnerability.

Use GitHub's private vulnerability reporting on this repository:

1. Open the repository's **Security** tab.
2. Choose **Advisories**.
3. Select **Report a vulnerability**.

Include reproduction steps, the affected version (`curl localhost:8787/healthz`
on the Spark, or the `fw=` line from `tools/devctl.py status`), impact, and any
suggested mitigation. Do not include Wi-Fi passwords, hostnames or addresses
from your network, or other private data.

You should receive an initial response within seven days. A fix and disclosure
timeline will be coordinated based on severity and complexity.

## Design boundaries

Things this project deliberately does, so you can judge reports against them:

- The agent's HTTP endpoint is **read-only machine vitals** over plain HTTP
  on port 8787. The installer limits it to this host and the local subnet (`--lan` for
  another): in the systemd unit (`IPAddressAllow`), and in ufw when ufw is active. It warns
  when it can do neither. At most 16 requests are served at once. `/healthz` gives the
  serial port's path, Sparkline's URL and error texts only to a client on the Spark
  itself. It is not meant to be exposed to the internet.
- The Wi-Fi password is stored **unencrypted** in the display's flash (NVS),
  like on most consumer IoT devices. Anyone with physical access to the board
  can read it. It is never printed to the console, logged, or kept in the
  console's command history, and the Wi-Fi driver's own persistent copy is disabled.
- The agent runs as an unprivileged user of its own (systemd's `DynamicUser`, nobody's
  login account) under a hardened systemd unit.
- Only asking tells a display from another Espressif board or serial adapter, so a host
  opens a candidate's port and sends it `status`. The installer does that to the device you
  plugged in and saves which one answered; from then on the agent opens that device only.
  Before a display has been saved (or after `--any-display`), the agent asks each
  Espressif or USB-UART device once when it's plugged in. `stat` lines only ever go to a
  device that answered the handshake. The lock on the port is advisory (`flock`).
- On a Spark that runs Sparkline, the agent holds Sparkline's agent token, which opens
  all of Sparkline's API. It is kept in a root-only file, handed to the service by
  systemd, and sent only over a connection that ends on the Spark itself.

## Scope

Particularly useful reports include:

- anything that lets a network peer do more than read `/stat` or `/healthz`;
- a way for the agent or the host tools to write to a serial device other than a
  SparkESP32 display;
- the Wi-Fi password appearing anywhere other than the board's NVS (console
  output, logs, history, crash dumps);
- console or HTTP input that crashes the firmware or corrupts its settings;
- installer behavior that weakens the host's security (permissions, firewall
  rules, services running as root).
