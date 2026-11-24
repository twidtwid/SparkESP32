#!/usr/bin/env bash
# SparkESP32 installer. Run ON the DGX Spark, from a clone of this repo:
#
#   sudo ./install.sh                      # interactive: flash, name, Wi-Fi, service
#   sudo ./install.sh --name "Lab Spark"   # change the display title later
#   sudo ./install.sh --ssids "HomeNet"    # set up / change Wi-Fi (password prompted)
#   sudo ./install.sh --uninstall
#
# Plug the display into one of the Spark's USB-C ports first. Safe to re-run:
# it only reflashes when the board's firmware differs from this release, and it
# only fills in settings that are unset unless you pass them explicitly.
# Run ./install.sh --help for every option.
set -Eeuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(head -n1 "$HERE/VERSION")"
REPO_URL="https://github.com/twidtwid/SparkESP32"
OPT=/opt/sparkesp32
VENV="$OPT/venv"
UNIT=/etc/systemd/system/sparkesp32.service
RULES=/etc/udev/rules.d/99-sparkesp32.rules
SL_ENV=/etc/sparkesp32/sparkline.env      # Sparkline's agent URL + token, when we read from it
DISPLAY_ENV=/etc/sparkesp32/display.env   # which USB device is the display, once we've seen it
HTTP_PORT=8787

boards() { (cd "$HERE/firmware/boards" 2>/dev/null && printf '%s ' */ | tr -d /); }
board_target() { sed -n 's/^CONFIG_IDF_TARGET="\{0,1\}\([a-z0-9]*\)"\{0,1\}$/\1/p' \
                     "$HERE/firmware/boards/$1/sdkconfig.defaults" 2>/dev/null; }

usage() {
    cat <<EOF
SparkESP32 $VERSION installer. Run on the DGX Spark with sudo.

  --name NAME              fixed display title, up to 24 chars (default: this Spark's
                           hostname, sent live by the agent; --name "" goes back to it)
  --host HOST              hostname/IP the display polls over Wi-Fi
                           (default: $(hostname -s 2>/dev/null || echo spark).local, set once)
  --ssids "A,B"            Wi-Fi network(s) for the display, tried in order; the
                           password is prompted for (hidden)
  --wifi-password-env VAR  take the Wi-Fi password from \$VAR instead of prompting
                           (sudo --preserve-env=VAR ./install.sh ...)
  --open-wifi              the Wi-Fi network has no password
  --clear-wifi             make the display forget its Wi-Fi settings
  --no-wifi                don't ask about Wi-Fi
  --rotation none|ccw|cw   screen rotation (default cw: the board's upright position;
                           landscape-only boards take ccw or cw)
  --board ID               which board is on USB (default: what its firmware says, or
                           its chip); one of: $(boards)
  --firmware DIR           flash a firmware bundle from DIR (the images + flash_args)
  --no-flash               never flash, even if the board needs it
  --no-sparkline           sample this host ourselves even if a Sparkline agent runs here
  --reflash                flash even if the board already runs $VERSION
  --port DEV               the display's serial port (default: autodetect)
  --any-display            forget which USB device is the display: the agent asks every
                           Espressif or USB-UART device once when it's plugged in
  --lan CIDR               subnet whose displays may reach :$HTTP_PORT (default: this host's)
  -y, --yes                no questions (for scripts and agents)
  --uninstall              remove the service, rules and $OPT (display settings stay)
EOF
}

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
# set -e alone exits without a word; say where (once: not again from inside a $(...)).
trap '[ "$BASH_SUBSHELL" -gt 0 ] || die "unexpected failure at line $LINENO: $BASH_COMMAND"' ERR

ARGS=""; [ $# -eq 0 ] || printf -v ARGS ' %q' "$@"    # for the "run with sudo" hint

NAME="" NAME_SET="" HOST="" SSIDS="" PW_ENV="" OPEN_WIFI="" CLEAR_WIFI="" NO_WIFI="" ROTATION=""
FW_DIR="" NO_FLASH="" REFLASH="" PORT="" LAN_CIDR="" YES="" UNINSTALL="" BOARD="" NO_SPARKLINE=""
ANY_DISPLAY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="${2?--name needs a value (\"\" for the hostname)}"; NAME_SET=1; shift ;;
        --host) HOST="${2?--host needs a value}"; shift ;;
        --ssids) SSIDS="${2?--ssids needs a value}"; shift ;;
        --wifi-password-env) PW_ENV="${2?--wifi-password-env needs a variable name}"; shift ;;
        --open-wifi) OPEN_WIFI=1 ;;
        --clear-wifi) CLEAR_WIFI=1 ;;
        --no-wifi) NO_WIFI=1 ;;
        --rotation) ROTATION="${2?--rotation needs none|ccw|cw}"; shift ;;
        --board) BOARD="${2?--board needs a board id}"; shift ;;
        --firmware) FW_DIR="${2?--firmware needs a directory}"; shift ;;
        --no-flash) NO_FLASH=1 ;;
        --no-sparkline) NO_SPARKLINE=1 ;;
        --reflash) REFLASH=1 ;;
        --port) PORT="${2?--port needs a device}"; shift ;;
        --lan) LAN_CIDR="${2?--lan needs a CIDR}"; shift ;;
        -y|--yes) YES=1 ;;
        --any-display) ANY_DISPLAY=1 ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

[ "$(id -u)" -eq 0 ] || die "run with sudo: sudo $0$ARGS"
interactive() { [ -z "$YES" ] && [ -t 0 ]; }

# ------------------------------------------------------------------ cleanup

ufw_active() { command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q "^Status: active"; }

ufw_delete_ours() {   # the rule this installer tags "sparkesp32"
    ufw_active || return 0
    local n
    for n in $(ufw status numbered | sed -En 's/^\[ *([0-9]+)\].*# sparkesp32 *$/\1/p' | sort -rn); do
        ufw --force delete "$n" >/dev/null
    done
}

if [ -n "$UNINSTALL" ]; then
    say "uninstalling SparkESP32"
    systemctl disable --now sparkesp32.service 2>/dev/null || true
    rm -f "$UNIT" "$RULES"
    rm -rf "$(dirname "$SL_ENV")"
    ufw_delete_ours
    rm -rf "$OPT"
    systemctl daemon-reload
    udevadm control --reload-rules 2>/dev/null || true
    say "done. The display keeps its settings; it will show OFFLINE until fed again."
    exit 0
fi

# ------------------------------------------------------------------ preflight

command -v python3 >/dev/null || die "python3 is required"
# Every python here runs isolated (-I). Without it, -c, -m and a script on stdin put the
# current directory first on the module path, and this runs as root from wherever it's called.
python3 -I -c 'import ensurepip, venv' 2>/dev/null || die "python3-venv is required: sudo apt install python3-venv"
command -v nvidia-smi >/dev/null || warn "nvidia-smi not found: GPU, temperature and power will show --"
case "$ROTATION" in ""|none|ccw|cw) ;; *) die "--rotation must be none, ccw or cw" ;; esac
if [ -n "$LAN_CIDR" ]; then
    LAN_CIDR="$(python3 -I -c 'import ipaddress, sys; print(ipaddress.ip_network(sys.argv[1], strict=False))' \
                "$LAN_CIDR" 2>/dev/null)" || die "--lan must be a subnet, like 192.168.1.0/24"
fi
if [ -n "$PW_ENV" ] && [ -z "${!PW_ENV+x}" ]; then
    die "\$$PW_ENV is not set inside sudo; use: sudo --preserve-env=$PW_ENV $0 ..."
fi

TMPD="$(mktemp -d)"
RESTORE_AGENT=""
cleanup() {
    rm -rf "$TMPD"
    # a run that fails part-way must not leave the display dark: restart the agent we stopped
    [ -z "$RESTORE_AGENT" ] || systemctl start sparkesp32.service 2>/dev/null || true
}
trap cleanup EXIT

say "SparkESP32 $VERSION for $(hostname -s)"
if systemctl is-active --quiet sparkesp32.service; then
    RESTORE_AGENT=1
    systemctl stop sparkesp32.service       # frees the serial port + :$HTTP_PORT
fi

# ------------------------------------------------------------------ files + venv

say "installing to $OPT"
install -d -m 0755 "$OPT" "$OPT/tools"
install -m 0644 "$HERE/VERSION" "$OPT/VERSION"
install -m 0755 "$HERE/agent/sparkesp32_agent.py" "$OPT/sparkesp32_agent.py"
install -m 0644 "$HERE/tools/sparklink.py" "$OPT/tools/sparklink.py"
install -m 0755 "$HERE/tools/provision.py" "$HERE/tools/devctl.py" "$OPT/tools/"
[ -x "$VENV/bin/python" ] || python3 -I -m venv "$VENV"
"$VENV/bin/python" -I -m pip install --quiet --disable-pip-version-check -r "$HERE/agent/requirements.txt" ||
    die "pip install failed (needs internet access to PyPI)"
PY="$VENV/bin/python"
devctl() { "$PY" "$OPT/tools/devctl.py" ${PORT:+--port "$PORT"} "$@"; }

# ------------------------------------------------------------------ display

sparklink() { "$PY" -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import sparklink; '"$1" "$OPT/tools" "${@:2}"; }
PORT_GIVEN="$PORT"
if [ -z "$PORT" ]; then
    mapfile -t PORTS < <(sparklink 'print("\n".join(sparklink.find_ports()))')
    [ "${#PORTS[@]}" -gt 1 ] && die "several USB serial devices that could be the display (${PORTS[*]}); pick one with --port"
    PORT="${PORTS[0]:-}"
fi

firmware_dir() {     # echo a dir holding $BOARD's bundle for $VERSION, fetching if needed
    local d pkg="sparkesp32-$BOARD-firmware-$VERSION"
    for d in "$FW_DIR" "$HERE/dist/$pkg"; do
        [ -n "$d" ] && [ -f "$d/sparkesp32.bin" ] && [ -f "$d/flash_args" ] && { echo "$d"; return; }
    done
    [ -n "$FW_DIR" ] && die "--firmware $FW_DIR: no sparkesp32.bin + flash_args there"
    local b="$HERE/firmware/build/$BOARD"
    d="$TMPD/fw"
    mkdir -p "$d"
    if [ -f "$b/sparkesp32.bin" ]; then          # a local build (developers): same shape as a bundle
        cp "$b/bootloader/bootloader.bin" "$b/partition_table/partition-table.bin" "$b/sparkesp32.bin" "$d/"
        sed 's|[^ ]*/||' "$b/flash_args" > "$d/flash_args"
        echo "$d"; return
    fi
    local url="$REPO_URL/releases/download/v$VERSION/$pkg.tar.gz"
    say "downloading firmware $VERSION for $BOARD" >&2
    curl -fsSL "$url" | tar -xz -C "$d" --strip-components=1 || die "couldn't download $url"
    # the bundle's own list: this catches a damaged download, not a release someone replaced
    # (README: gh attestation verify)
    (cd "$d" && sha256sum --quiet -c SHA256SUMS) || die "firmware checksum mismatch"
    echo "$d"
}

pick_board() {       # which board is on $PORT: --board, what its firmware says, or its chip
    [ -n "$BOARD" ] || BOARD="$(status_board)"
    if [ -z "$BOARD" ]; then      # factory firmware, or SparkESP32 from before boards: ask the chip
        local chip b found=()
        chip="$("$PY" -I -m esptool -p "$PORT" chip_id 2>/dev/null | sed -n 's/^Chip is \(ESP32-[A-Z0-9]*\).*/\1/p' |
                tr '[:upper:]' '[:lower:]' | tr -d -)" || chip=""
        [ -n "$chip" ] || die "couldn't identify the board on $PORT; name it with --board ($(boards))"
        for b in $(boards); do [ "$(board_target "$b")" = "$chip" ] && found+=("$b"); done
        [ "${#found[@]}" -eq 1 ] ||
            die "the board on $PORT is an $chip; pick its board with --board (one of: ${found[*]:-$(boards)})"
        BOARD="${found[0]}"
    fi
    [ -n "$(board_target "$BOARD")" ] || die "unknown board '$BOARD'; one of: $(boards)"
}

DEV_STATUS=""
read_status() { DEV_STATUS="$(devctl status 2>/dev/null)" || DEV_STATUS=""; }
board_version() { sed -n 's/^fw=sparkesp32 version=\([^ ]*\).*/\1/p' <<<"$DEV_STATUS"; }
status_board() { sed -n 's/^fw=sparkesp32 .*board=\([^ ]*\).*/\1/p' <<<"$DEV_STATUS"; }   # "" before boards

# A USB-UART bridge (CH340, CP210x) found on its own may be some other gadget: flash it
# only when it already runs SparkESP32 or --board/--port says a new board is there.
unclaimed_bridge() {
    [ -z "$(board_version)" ] && [ -z "$BOARD" ] && [ -z "$PORT_GIVEN" ] &&
        [ "$(sparklink 'print(int(sparklink.is_bridge(sys.argv[2])))' "$PORT")" = 1 ]; }

[ -n "$PORT" ] && read_status
if [ -n "$PORT" ] && unclaimed_bridge; then
    warn "$PORT is a USB-UART bridge that doesn't answer as a SparkESP32: leaving it alone."
    warn "To flash a new display behind one (the Elecrow e-paper), name it: --board <id>"
    PORT="" DEV_STATUS=""
fi

# A device that doesn't answer as a SparkESP32 is a new display, or it's somebody's other
# ESP32 project, which flashing would replace. Flash it when --port or --board names it, when
# you say so at the prompt, or, with nobody to ask, unless another device is this Spark's display.
usb_id() { sparklink 'print(sparklink.port_id(sys.argv[2]) or "")' "$PORT" 2>/dev/null || true; }
saved_display() { sed -n 's/^SPARKESP32_DISPLAY=//p' "$DISPLAY_ENV" 2>/dev/null || true; }
may_flash() {
    [ -z "$BOARD" ] && [ -z "$PORT_GIVEN" ] || return 0
    local saved other="" answer=""
    saved="$(saved_display)"
    [ -z "$saved" ] || [ "$saved" = "$(usb_id)" ] || other=1
    if ! interactive; then
        [ -z "$other" ]
    elif [ -n "$other" ]; then
        read -r -p "$PORT doesn't run SparkESP32, and it isn't the display set up here before. Flash it anyway? That replaces the program on it. [y/N] " answer || true
        [[ "$answer" == [yY]* ]]
    else
        read -r -p "$PORT doesn't run SparkESP32 yet. Flash it? That replaces the program on it. [Y/n] " answer || true
        [[ "$answer" != [nN]* ]]
    fi
}

if [ -n "$PORT" ] && [ -z "$NO_FLASH" ] && [ -z "$(board_version)" ] && ! may_flash; then
    warn "$PORT doesn't run SparkESP32, and nothing says it's the display: leaving it alone."
    warn "To flash it, name it: --port $PORT (or --board <id>)"
    PORT="" DEV_STATUS=""
fi

if [ -z "$PORT" ]; then
    warn "no display on USB: skipping flash + setup. Plug it into this Spark and re-run,"
    warn "or set it up from another computer (README: 'Setting up from a Mac or PC')."
else
    say "display found on $PORT"
    BOARD_VER="$(board_version)"
    if [ -n "$NO_FLASH" ]; then
        [ -n "$BOARD_VER" ] || warn "the board isn't running SparkESP32 firmware and --no-flash was given"
    elif [ -n "$REFLASH" ] || [ "$BOARD_VER" != "$VERSION" ] ||
         { [ -n "$BOARD" ] && [ "$(status_board)" != "$BOARD" ]; }; then   # --board names another board
        pick_board
        FW="$(firmware_dir)"
        say "flashing firmware $VERSION for $BOARD (board had: ${BOARD_VER:-other firmware})"
        # Split images at the offsets in the bundle's flash_args, never a merged image:
        # that overwrites the NVS partition and the board would forget its Wi-Fi and
        # name. esptool reads the chip itself and refuses an image built for another.
        if ! (cd "$FW" && "$PY" -I -m esptool -p "$PORT" -b 460800 --before default_reset --after hard_reset \
                write_flash @flash_args); then
            # a refused image leaves the chip in its bootloader: reset it back (best effort)
            "$PY" -I -m esptool -p "$PORT" --after hard_reset chip_id >/dev/null 2>&1 || true
            die "flashing failed: see esptool's message above (\"not an ESP32-... image\": wrong --board;
       can't connect: hold BOOT while plugging the board in, then re-run)"
        fi
        for _ in 1 2 3; do sleep 1; read_status; [ -n "$DEV_STATUS" ] && break; done   # each try waits 10 s
        [ -n "$DEV_STATUS" ] || die "the board didn't answer after flashing"
        [ "$(board_version)" = "$VERSION" ] ||
            warn "the board now runs '$(board_version)', not $VERSION: was $FW built from another version?"
        [ "$(status_board)" = "$BOARD" ] ||
            warn "the board reports board='$(status_board)', not $BOARD: was $FW built for another board?"
    else
        say "board already runs $VERSION"
    fi
fi

# ------------------------------------------------------------------ settings

if [ -n "$PORT" ] && [ -n "$DEV_STATUS" ]; then
    URL_SET="$(sed -n 's/^wifi configured=[01] connected=[01] url=\([01]\)$/\1/p' <<<"$DEV_STATUS")"
    WIFI_SET="$(sed -n 's/^wifi configured=\([01]\).*/\1/p' <<<"$DEV_STATUS")"
    args=()

    # no saved name = the display shows this Spark's hostname, live from the agent
    [ -n "$NAME_SET" ] && args+=(--name "$NAME")
    [ -n "$ROTATION" ] && args+=(--rotation "$ROTATION")

    if [ -z "$HOST" ] && [ "$URL_SET" != 1 ]; then
        if systemctl is-active --quiet avahi-daemon 2>/dev/null; then
            HOST="$(hostname -s).local"
        else
            HOST="$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')" || HOST=""
            if [ -n "$HOST" ]; then
                warn "avahi (mDNS) isn't running, so the display will poll $HOST directly;"
                warn "give this Spark a DHCP reservation or re-run with --host if that address changes"
            else
                warn "couldn't work out this Spark's LAN address; re-run with --host for Wi-Fi mode"
            fi
        fi
    fi
    [ -n "$HOST" ] && args+=(--host "$HOST")

    if [ -n "$CLEAR_WIFI" ]; then
        args+=(--clear-wifi)
    elif [ -z "$SSIDS" ] && [ -z "$NO_WIFI" ] && [ "$WIFI_SET" != 1 ] && interactive; then
        echo
        echo "Wi-Fi is optional: with it, the display keeps working when it isn't plugged"
        echo "into this Spark (it polls this Spark's agent on your LAN). The ESP32-C6 only"
        echo "sees 2.4 GHz networks. Networks it can see right now:"
        devctl wifi scan 2>/dev/null | sed -n "s/^ap ssid='\(.\{1,\}\)' chan=.* rssi=\(-\{0,1\}[0-9]*\).*/\1|\2/p" |
            sort -t'|' -k2,2nr | awk -F'|' '!seen[$1]++ { printf "    %-32s %s dBm\n", $1, $2 }' | head -12 || true
        HERE_SSID="$(nmcli -t -f active,ssid dev wifi 2>/dev/null | sed -n 's/^yes://p' | head -n1 || true)"
        [ -n "$HERE_SSID" ] && echo "(this Spark is on \"$HERE_SSID\")"
        read -r -p "Network name(s) for the display, comma-separated (Enter to skip): " SSIDS
    fi
    if [ -n "$SSIDS" ]; then
        args+=(--ssids "$SSIDS")
        if [ -n "$OPEN_WIFI" ]; then args+=(--open)
        elif [ -n "$PW_ENV" ]; then args+=(--password-env "$PW_ENV")
        elif ! interactive; then die "--ssids needs a password: run interactively, or use --wifi-password-env VAR / --open-wifi"
        fi
    fi

    say "configuring the display"
    "$PY" "$OPT/tools/provision.py" ${PORT:+--port "$PORT"} "${args[@]}" ||
        die "display setup failed (see above); nothing else was changed on the board"
fi

# ------------------------------------------------------------------ Sparkline

# A Sparkline agent (github.com/sfxnz/sparkline) already samples this Spark once a second. When
# one runs here, our agent reads its /api/v1/snapshot instead of running nvidia-smi a second
# time, so the display and the Sparkline dashboard show the same numbers. Its token goes in a
# root-only file that systemd hands to our service, which runs as a user of its own; it never
# goes to the display.
sparkline_config() {     # prints "URL TOKEN" for a Sparkline agent running here, or nothing
    systemctl is-active --quiet sparkline-agent.service 2>/dev/null || return 0
    [ -r /etc/sparkline/agent.toml ] || return 0
    "$PY" -I - <<'EOF' 2>/dev/null || true
import tomllib
c = tomllib.load(open("/etc/sparkline/agent.toml", "rb"))
# bind "auto" (Sparkline's default: this host's default-route address) stays "auto". Our agent
# looks the address up for each request, so the token never follows an address this host had.
token, bind, port = c.get("token", ""), c.get("bind", "auto"), c.get("port", 9750)
if ":" in bind:
    bind = f"[{bind}]"
if token and bind:
    print(f"http://{bind}:{port} {token}")
EOF
}

SL_URL="" SL_TOKEN=""
[ -n "$NO_SPARKLINE" ] || read -r SL_URL SL_TOKEN <<<"$(sparkline_config)" || true
if [ -n "$SL_URL" ]; then
    say "Sparkline runs here ($SL_URL): the display gets its numbers (--no-sparkline to sample ourselves)"
    install -d -m 0755 "$(dirname "$SL_ENV")"
    (umask 077 && printf 'SPARKLINE_URL=%s\nSPARKLINE_TOKEN=%s\n' "$SL_URL" "$SL_TOKEN" > "$SL_ENV")
else
    rm -f "$SL_ENV"
fi

# ------------------------------------------------------------------ which device is the display

# Telling a display from another Espressif board or serial adapter takes opening its port and
# asking it. We just did that for the display on $PORT, with you there; the agent shouldn't
# keep doing it to whatever else gets plugged into this Spark. So save what identifies this
# device (its USB serial number, or for a bridge without one its USB socket), and the agent
# opens that device only. No display this time: what was saved stays.
if [ -n "$ANY_DISPLAY" ]; then
    rm -f "$DISPLAY_ENV"
    say "the agent will ask every Espressif or USB-UART device once when it's plugged in"
elif [ -n "$PORT" ] && [ -n "$DEV_STATUS" ]; then
    DISPLAY_ID="$(usb_id)"
    if [ -n "$DISPLAY_ID" ]; then
        install -d -m 0755 "$(dirname "$DISPLAY_ENV")"
        printf 'SPARKESP32_DISPLAY=%s\n' "$DISPLAY_ID" > "$DISPLAY_ENV"
        say "the agent will open this display only (for another one: plug it in and re-run this)"
    else
        rm -f "$DISPLAY_ENV"      # a display we can't tell apart: don't hold the agent to an older one
        warn "this display has no USB serial number or location to know it by: the agent will ask"
        warn "every Espressif or USB-UART device once when it's plugged in"
    fi
fi

# ------------------------------------------------------------------ service

say "udev: keep ModemManager off the display (and off any CH340/CP210x serial adapter: the rule goes by chip)"
install -m 0644 "$HERE/agent/99-sparkesp32.rules" "$RULES"
udevadm control --reload-rules
udevadm trigger --subsystem-match=tty || true

# Who may talk to the agent: this host's own subnet (its default route's interface), where the
# displays on Wi-Fi are, and where the agent reaches a Sparkline that runs here. --lan names
# another subnet for the displays. The unit limits the agent to them whether or not a firewall
# does; the ufw rule lets the displays' subnet in.
own_subnet() {
    local dev addr
    dev="$(ip -4 route show default 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit }}')" || dev=""
    [ -n "$dev" ] || return 0
    addr="$(ip -4 -o addr show dev "$dev" scope global 2>/dev/null | awk '{print $4; exit}')" || addr=""
    python3 -I -c 'import ipaddress, sys; print(ipaddress.ip_interface(sys.argv[1]).network)' "$addr" 2>/dev/null || true
}
OWN_CIDR="$(own_subnet)"
ALLOW="$OWN_CIDR"
[ -z "$LAN_CIDR" ] || [ "$LAN_CIDR" = "$OWN_CIDR" ] || ALLOW="${ALLOW:+$ALLOW }$LAN_CIDR"
[ -n "$LAN_CIDR" ] || LAN_CIDR="$OWN_CIDR"

unit_file() {        # the unit, limited to $ALLOW, or without that limit when no subnet is known
    if [ -n "$ALLOW" ]; then
        sed "s|@LAN@|$ALLOW|" "$HERE/agent/sparkesp32.service"
    else
        sed '/^IPAddress/d' "$HERE/agent/sparkesp32.service"
    fi
}

say "systemd: sparkesp32.service, as an unprivileged user of its own"
unit_file > "$UNIT"
chmod 0644 "$UNIT"
systemctl daemon-reload
systemctl enable --quiet sparkesp32.service
systemctl restart sparkesp32.service
RESTORE_AGENT=""
[ -z "$ALLOW" ] || say "systemd: only this host and $ALLOW can reach the agent"

if ufw_active; then
    ufw_delete_ours
    [ -n "$LAN_CIDR" ] || die "couldn't work out this host's LAN subnet; re-run with --lan CIDR"
    say "ufw: allow $LAN_CIDR -> :$HTTP_PORT/tcp"
    ufw allow from "$LAN_CIDR" to any port "$HTTP_PORT" proto tcp comment sparkesp32 >/dev/null
elif [ -z "$ALLOW" ]; then
    warn "couldn't work out this host's LAN subnet, and no ufw firewall is active: :$HTTP_PORT/stat"
    warn "(read-only vitals) is open to anything that can reach this host. Re-run with --lan CIDR."
fi

# ------------------------------------------------------------------ verify

say "checking"
# the retries cover the agent still starting (connection refused) and a stale-sample 503
line="$(curl -fsS -m2 --retry 15 --retry-delay 1 --retry-connrefused "http://127.0.0.1:$HTTP_PORT/stat" 2>/dev/null)" || line=""
case "$line" in
    "stat "*) echo "    $line" ;;
    *) systemctl --no-pager --lines=15 status sparkesp32.service >&2 || true; die "the agent isn't serving /stat" ;;
esac
# the agent's USB link state (/healthz "display"): searching, handshaking, streaming, rejected
link="searching"
if [ -n "$PORT" ]; then
    for _ in $(seq 1 15); do          # the handshake alone may take up to 10 s
        link="$(curl -fsS -m2 "http://127.0.0.1:$HTTP_PORT/healthz" 2>/dev/null | "$PY" -I -c \
            'import json, sys; d = json.load(sys.stdin)["display"]; print(d["state"], d.get("port", ""))' \
            2>/dev/null)" || link="searching"
        [ "${link%% *}" = streaming ] && break
        sleep 1
    done
fi

echo
say "SparkESP32 $VERSION is running"
echo "    agent:    http://$(hostname -s).local:$HTTP_PORT/stat  (and /healthz)"
case "${link%% *}" in
    streaming) echo "    display:  connected on ${link#* }"
               echo "    the screen should read ONLINE via USB within a couple of seconds" ;;
    rejected)  echo "    display:  ${link#* } didn't answer as a SparkESP32; replug it to retry" ;;
    *)         echo "    display:  not connected"
               echo "    plug the display into this Spark's USB-C, or put it on Wi-Fi (README)" ;;
esac
if [ -n "$SL_URL" ]; then
    src="$(curl -fsS -m2 "http://127.0.0.1:$HTTP_PORT/healthz" 2>/dev/null | "$PY" -I -c \
        'import json, sys; d = json.load(sys.stdin); print(d.get("source", "?"), (d.get("sparkline") or {}).get("error") or "")' \
        2>/dev/null)" || src="?"
    case "$src" in
        sparkline*) echo "    readings: from Sparkline at $SL_URL" ;;
        *) echo "    readings: sampled here; Sparkline didn't answer (${src#* }), retried every 30 s" ;;
    esac
fi
echo "    logs:     journalctl -u sparkesp32 -f"
echo "    change:   sudo $0 --name ... | --ssids ... | --host ...   (re-run any time)"
