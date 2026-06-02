#!/bin/bash
# Claude Usage Tracker Daemon (BLE)
# Reads Claude Code OAuth token, polls the OAuth usage endpoint, sends to ESP32 over BLE GATT.
# Auto-connects and reconnects to the Claude Controller BLE device.
# Dependencies: curl, awk, python3, bluetoothctl

DEVICE_NAME="Claude Controller"
DEVICE_MAC="${DEVICE_MAC:-}"  # auto-discovered if empty
SERVICE_UUID="4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID="4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID="4c41555a-4465-7669-6365-000000000004"
POLL_INTERVAL=300    # /api/oauth/usage is rate-limited (429/529 above ~1 req/min); 5 min is safe and plenty fresh
POLL_FAIL_BACKOFF=60 # after a failed/throttled poll, retry this soon instead of waiting the full interval
TICK=5
SAVED_MAC_FILE="$HOME/.config/claude-usage-monitor/ble-address"
REFRESH_FLAG="/tmp/claude-usage-refresh-$$"
DBUS_DEST="org.bluez"
NOTIFY_PID=""

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

read_token() {
    grep -o '"accessToken":"[^"]*"' "$HOME/.claude/.credentials.json" | cut -d'"' -f4
}

# Convert MAC to D-Bus path: AA:BB:CC:DD:EE:FF -> dev_AA_BB_CC_DD_EE_FF
mac_to_dbus_path() {
    local adapter
    adapter=$(busctl call org.bluez / org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null | grep -o '/org/bluez/hci[0-9]' | head -1)
    adapter=${adapter:-/org/bluez/hci0}
    echo "${adapter}/dev_$(echo "$1" | tr ':' '_')"
}

# Check if device is connected via D-Bus
is_connected() {
    local path
    path=$(mac_to_dbus_path "$DEVICE_MAC")
    busctl get-property "$DBUS_DEST" "$path" org.bluez.Device1 Connected 2>/dev/null | grep -q "true"
}

# Load saved MAC address
load_mac() {
    if [ -n "$DEVICE_MAC" ]; then return 0; fi
    if [ -f "$SAVED_MAC_FILE" ]; then
        DEVICE_MAC=$(head -1 "$SAVED_MAC_FILE" | tr -d '\r\n ')
        if [[ "$DEVICE_MAC" =~ ^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$ ]]; then
            return 0
        fi
        log "Cached MAC is malformed, discarding"
        rm -f "$SAVED_MAC_FILE"
        DEVICE_MAC=""
    fi
    return 1
}

# Save MAC for fast reconnect
save_mac() {
    mkdir -p "$(dirname "$SAVED_MAC_FILE")"
    echo "$DEVICE_MAC" > "$SAVED_MAC_FILE"
}

# Scan for Claude Controller
scan_for_device() {
    log "Scanning for '$DEVICE_NAME'..."
    # Start LE scan
    bluetoothctl scan le &>/dev/null &
    local scan_pid=$!
    sleep 8
    kill "$scan_pid" 2>/dev/null
    wait "$scan_pid" 2>/dev/null

    # Pick the first matching device. Multiple matches happen when bluez
    # remembers old hardware (e.g. after swapping ESP boards). Stale entries
    # are removed on connect failure (see connect_device), so a few retry
    # cycles will converge on the live device.
    local found
    found=$(bluetoothctl devices 2>/dev/null | grep "$DEVICE_NAME" | head -1 | awk '{print $2}')
    if [ -n "$found" ]; then
        DEVICE_MAC="$found"
        save_mac
        log "Found: $DEVICE_MAC"
        return 0
    fi
    return 1
}

# Connect to the device
connect_device() {
    log "Connecting to $DEVICE_MAC..."

    # Trust first (allows auto-reconnect)
    bluetoothctl trust "$DEVICE_MAC" &>/dev/null

    # Connect
    bluetoothctl connect "$DEVICE_MAC" &>/dev/null
    sleep 2

    if is_connected; then
        log "Connected"
        return 0
    fi
    log "Connection failed"
    if [ -f "$SAVED_MAC_FILE" ] && [ "$(cat "$SAVED_MAC_FILE")" = "$DEVICE_MAC" ]; then
        log "Invalidating cached MAC, will rescan by name"
        rm -f "$SAVED_MAC_FILE"
    fi
    # Remove from bluez so the next scan won't re-pick this dead MAC.
    # If the device comes back online it'll re-advertise and be re-discovered.
    bluetoothctl remove "$DEVICE_MAC" &>/dev/null
    DEVICE_MAC=""
    return 1
}

# Find a GATT characteristic path by UUID via D-Bus
find_char_path_by_uuid() {
    local target_uuid="$1"
    local dev_path
    dev_path=$(mac_to_dbus_path "$DEVICE_MAC")

    busctl tree "$DBUS_DEST" 2>/dev/null | grep -o "${dev_path}/service[0-9a-f]*/char[0-9a-f]*" | while read -r char_path; do
        local uuid
        uuid=$(busctl get-property "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 UUID 2>/dev/null | tr -d '"' | awk '{print $2}')
        if [ "$uuid" = "$target_uuid" ]; then
            echo "$char_path"
            return 0
        fi
    done
}

# Subscribe to refresh-request notifications. The ESP fires this when it
# has no usage data yet (e.g. after a fresh boot). Daemon awk drops a flag
# file that the inner loop picks up on its next 5s tick.
#
# Implementation notes:
# - dbus-monitor must be running BEFORE we call StartNotify, because busctl
#   exits immediately, the subscription tears down within milliseconds, and
#   the ESP's notify fires inside that brief window.
# - stdbuf -oL forces line-buffered stdout on dbus-monitor; without it,
#   glibc switches to block buffering when stdout is a pipe and signals
#   never reach awk until ~4KB accumulates.
# - The pipeline runs in a setsid'd child so we can kill the whole process
#   group (dbus-monitor + awk) atomically. Killing only awk leaves
#   dbus-monitor orphaned, and `wait $!` in bash waits on the whole job
#   until every pipeline member exits, hanging the daemon.
start_notify_subscriber() {
    local req_path
    req_path=$(find_char_path_by_uuid "$REQ_CHAR_UUID")
    if [ -z "$req_path" ]; then
        log "Refresh char not found, skipping notify subscriber"
        return 1
    fi

    setsid bash -c "stdbuf -oL dbus-monitor --system \"type='signal',interface='org.freedesktop.DBus.Properties',path='$req_path',member='PropertiesChanged'\" 2>/dev/null | awk -v flag='$REFRESH_FLAG' '/Value/ { system(\"touch \" flag); fflush() }'" &
    NOTIFY_PID=$!

    # Give dbus-monitor a moment to register its match rule, then trigger
    # the GATT subscription that causes the ESP to fire its notify.
    sleep 0.3
    busctl call "$DBUS_DEST" "$req_path" org.bluez.GattCharacteristic1 StartNotify >/dev/null 2>&1

    log "Refresh subscriber started (pgid=$NOTIFY_PID)"
}

stop_notify_subscriber() {
    if [ -n "$NOTIFY_PID" ]; then
        # Kill the whole process group (setsid made NOTIFY_PID the leader).
        # Don't wait — we don't care about exit status and waiting can hang
        # if any group member is slow to exit.
        kill -TERM -"$NOTIFY_PID" 2>/dev/null
        NOTIFY_PID=""
    fi
    rm -f "$REFRESH_FLAG"
}

# Write data to the RX characteristic via D-Bus
write_gatt() {
    local char_path="$1"
    local data="$2"

    # Convert string to byte array for D-Bus: "hi" -> 0x68 0x69
    local bytes=""
    for ((i = 0; i < ${#data}; i++)); do
        local byte
        byte=$(printf "0x%02x" "'${data:$i:1}")
        bytes="$bytes $byte"
    done
    local count=${#data}

    busctl call "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 \
        WriteValue "aya{sv}" "$count" $bytes 0 2>/dev/null
}

poll() {
    local token
    token=$(read_token) || { log "Error: could not read token"; return 1; }

    # Pull live usage straight from the OAuth usage endpoint. Unlike the old
    # /v1/messages header-scrape, this is a purpose-built JSON endpoint: no
    # throwaway inference call, utilization is already a percentage (0-100),
    # and resets come back as ISO-8601 timestamps. It IS rate-limited, though
    # (429/529 above ~1 req/min) — hence POLL_INTERVAL above.
    local resp http body
    resp=$(curl -s -w $'\n%{http_code}' \
        "https://api.anthropic.com/api/oauth/usage" \
        -H "Authorization: Bearer $token" \
        -H "anthropic-beta: oauth-2025-04-20" \
        -H "User-Agent: claude-code/2.1.5" \
        2>/dev/null) || { log "Error: API call failed"; return 1; }
    http=$(printf '%s' "$resp" | tail -n1)
    body=$(printf '%s' "$resp" | sed '$d')

    if [ "$http" != "200" ]; then
        log "Usage API HTTP $http: $(printf '%s' "$body" | tr -d '\n' | head -c 160)"
        return 1
    fi

    # Map the endpoint JSON to the compact wire format the firmware parses:
    # five_hour -> session (s/sr), seven_day -> weekly (w/wr). Utilization is
    # already 0-100, so no *100. resets_at (ISO-8601) -> minutes-from-now.
    local payload
    payload=$(printf '%s' "$body" | python3 -c '
import sys, json
from datetime import datetime, timezone

def mins(iso):
    if not iso:
        return -1
    try:
        dt = datetime.fromisoformat(iso)
        if dt.tzinfo is None:               # naive timestamp -> assume UTC
            dt = dt.replace(tzinfo=timezone.utc)
        return max(0, round((dt - datetime.now(timezone.utc)).total_seconds() / 60))
    except Exception:                       # unparseable / arithmetic error -> unknown
        return -1

d = json.load(sys.stdin)
fh = d.get("five_hour") or {}
sd = d.get("seven_day") or {}
# Utilization is already 0-100; round once and derive st from the rounded value
# so "s":100 can never pair with "st":"allowed" (raw 99.6 rounds up to 100).
s = round(fh.get("utilization") or 0)
w = round(sd.get("utilization") or 0)
print(json.dumps({
    "s": s,
    "sr": mins(fh.get("resets_at")),
    "w": w,
    "wr": mins(sd.get("resets_at")),
    "st": "limited" if s >= 100 else "allowed",
    "ok": True,
}, separators=(",", ":")))
') || { log "Error: failed to parse usage JSON"; return 1; }

    [ -n "$payload" ] || { log "Error: empty payload after parse"; return 1; }

    log "Sending: $payload"
    write_gatt "$RX_CHAR_PATH" "$payload" || { log "Write failed"; return 1; }
    return 0
}

cleanup() {
    stop_notify_subscriber
    log "Daemon stopped"
    exit 0
}

trap cleanup INT TERM

log "=== Claude Usage Tracker Daemon (BLE) ==="
log "Poll interval: ${POLL_INTERVAL}s"

BACKOFF=1

while true; do
    # Find the device
    if ! load_mac; then
        scan_for_device || {
            log "Device not found, retrying in ${BACKOFF}s..."
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
            continue
        }
    fi

    # Connect if not connected
    if ! is_connected; then
        connect_device || {
            log "Retrying in ${BACKOFF}s..."
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
            continue
        }
    fi

    # Find the GATT characteristic
    RX_CHAR_PATH=$(find_char_path_by_uuid "$RX_CHAR_UUID")
    if [ -z "$RX_CHAR_PATH" ]; then
        log "Error: RX characteristic not found, retrying..."
        sleep 5
        continue
    fi
    log "GATT RX path: $RX_CHAR_PATH"

    BACKOFF=1  # reset backoff on successful connection

    start_notify_subscriber

    # Poll loop: tick every $TICK seconds. Poll Anthropic when the interval has
    # elapsed OR when the ESP requested a refresh. On a failed/throttled poll,
    # back off by POLL_FAIL_BACKOFF (not the full interval and not every tick)
    # so we recover quickly without storming the rate-limited usage endpoint.
    LAST_POLL=0
    while is_connected; do
        NOW=$(date +%s)
        if [ -f "$REFRESH_FLAG" ] || (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            if [ -f "$REFRESH_FLAG" ]; then
                log "Refresh requested by device"
                rm -f "$REFRESH_FLAG"
            fi
            if poll; then
                LAST_POLL=$NOW
            else
                LAST_POLL=$(( NOW - POLL_INTERVAL + POLL_FAIL_BACKOFF ))
            fi
        fi
        sleep "$TICK"
    done

    stop_notify_subscriber
    log "Device disconnected, reconnecting..."
    sleep 2
done
