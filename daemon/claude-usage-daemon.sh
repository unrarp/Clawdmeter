#!/bin/bash
# Claude Usage Tracker Daemon (BLE)
# Reads Claude Code OAuth token + Codex token, polls the OAuth usage endpoints,
# sends a combined JSON payload to the ESP32 over BLE GATT.
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

# Per-provider last-good cache files (written by the inline Python, read next cycle)
CLAUDE_CACHE="/tmp/claude-usage-claude-last-$$"
CODEX_CACHE="/tmp/claude-usage-codex-last-$$"

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
    # ---- Presence detection (re-checked every cycle) -------------------------
    # sp: Claude present iff creds file exists.
    # cp: Codex present iff auth.json exists.
    local sp=false cp=false
    [ -f "$HOME/.claude/.credentials.json" ] && sp=true
    [ -f "$HOME/.codex/auth.json" ] && cp=true

    # Drop a provider's last-good cache the moment it goes absent, so re-adding
    # an account starts at "Connecting..." rather than resurrecting the previous
    # account's numbers as stale.
    [ "$sp" = "true" ] || rm -f "$CLAUDE_CACHE"
    [ "$cp" = "true" ] || rm -f "$CODEX_CACHE"

    # ---- Claude poll (only when present) -------------------------------------
    local claude_http="" claude_body=""
    if [ "$sp" = "true" ]; then
        local token
        token=$(read_token 2>/dev/null)
        if [ -n "$token" ]; then
            local claude_resp
            claude_resp=$(curl -s -w $'\n%{http_code}' \
                "https://api.anthropic.com/api/oauth/usage" \
                -H "Authorization: Bearer $token" \
                -H "anthropic-beta: oauth-2025-04-20" \
                -H "User-Agent: claude-code/2.1.5" \
                2>/dev/null)
            claude_http=$(printf '%s' "$claude_resp" | tail -n1)
            claude_body=$(printf '%s' "$claude_resp" | sed '$d')
        else
            log "Claude: could not read token"
            claude_http="0"
            claude_body=""
        fi
    fi

    # ---- Codex poll (only when present) --------------------------------------
    local codex_http="" codex_body=""
    if [ "$cp" = "true" ]; then
        local codex_access_token codex_account_id codex_creds
        # Read both creds in ONE python invocation — two separate reads could
        # straddle a Codex CLI token refresh and pair a new access_token with an
        # old account_id.
        codex_creds=$(python3 -c "
import json, sys
try:
    d = json.load(open('$HOME/.codex/auth.json'))
    print(d['tokens']['access_token'] + '\t' + d['tokens']['account_id'])
except Exception:
    sys.exit(1)
" 2>/dev/null)
        IFS=$'\t' read -r codex_access_token codex_account_id <<<"$codex_creds"
        if [ -n "$codex_access_token" ] && [ -n "$codex_account_id" ]; then
            local codex_resp
            codex_resp=$(curl -s -w $'\n%{http_code}' \
                "https://chatgpt.com/backend-api/wham/usage" \
                -H "Authorization: Bearer $codex_access_token" \
                -H "ChatGPT-Account-Id: $codex_account_id" \
                -H "User-Agent: claude-code/2.1.5" \
                2>/dev/null)
            codex_http=$(printf '%s' "$codex_resp" | tail -n1)
            codex_body=$(printf '%s' "$codex_resp" | sed '$d')
        else
            log "Codex: could not read auth.json tokens"
            codex_http="0"
            codex_body=""
        fi
    fi

    # ---- Merge both providers into a combined wire payload -------------------
    # The inline Python:
    #   - reads per-provider HTTP codes, bodies, and presence flags
    #   - applies the last-good cache (reads/writes the tmp files)
    #   - emits the combined compact JSON on stdout
    #   - exits 0 if >=1 present provider succeeded (or neither present), 1 if all-present failed
    local payload
    payload=$(SP="$sp" CP="$cp" \
        CLAUDE_HTTP="$claude_http" CLAUDE_BODY="$claude_body" \
        CODEX_HTTP="$codex_http"   CODEX_BODY="$codex_body" \
        CLAUDE_CACHE="$CLAUDE_CACHE" CODEX_CACHE="$CODEX_CACHE" \
        python3 -c '
import sys, json, os

sp = (os.environ.get("SP") == "true")
cp = (os.environ.get("CP") == "true")
claude_http = os.environ.get("CLAUDE_HTTP", "")
claude_body = os.environ.get("CLAUDE_BODY", "")
codex_http  = os.environ.get("CODEX_HTTP", "")
codex_body  = os.environ.get("CODEX_BODY", "")
claude_cache_file = os.environ.get("CLAUDE_CACHE", "")
codex_cache_file  = os.environ.get("CODEX_CACHE", "")

def load_cache(path):
    try:
        return json.loads(open(path).read())
    except Exception:
        return None

def save_cache(path, obj):
    try:
        open(path, "w").write(json.dumps(obj))
    except Exception:
        pass

def mins_from_seconds(s):
    try:
        return max(0, int(s) // 60)
    except Exception:
        return -1

# ---- Parse Claude response ---------------------------------------------------
claude_ok = False
claude_fresh = None
if sp and claude_http == "200" and claude_body:
    try:
        from datetime import datetime, timezone
        d = json.loads(claude_body)
        fh = d.get("five_hour") or {}
        sd = d.get("seven_day") or {}
        def iso_mins(iso):
            if not iso:
                return -1
            try:
                dt = datetime.fromisoformat(iso)
                if dt.tzinfo is None:
                    dt = dt.replace(tzinfo=timezone.utc)
                return max(0, round((dt - datetime.now(timezone.utc)).total_seconds() / 60))
            except Exception:
                return -1
        sv = round(fh.get("utilization") or 0)
        wv = round(sd.get("utilization") or 0)
        claude_fresh = {
            "s":  sv,
            "sr": iso_mins(fh.get("resets_at")),
            "w":  wv,
            "wr": iso_mins(sd.get("resets_at")),
            "st": "limited" if sv >= 100 else "allowed",
        }
        claude_ok = True
    except Exception as e:
        pass

# ---- Parse Codex response ---------------------------------------------------
codex_ok = False
codex_fresh = None
if cp and codex_http == "200" and codex_body:
    try:
        d = json.loads(codex_body)
        rl = d.get("rate_limit") or {}
        pw = rl.get("primary_window") or {}
        sw = rl.get("secondary_window") or {}
        cs = round(pw.get("used_percent") or 0)
        cw = round(sw.get("used_percent") or 0)
        allowed  = rl.get("allowed", True)
        lim_reachd = rl.get("limit_reached", False)
        cst = "limited" if (not allowed or lim_reachd or cs >= 100) else "allowed"
        codex_fresh = {
            "cs":  cs,
            "csr": mins_from_seconds(pw.get("reset_after_seconds")),
            "cw":  cw,
            "cwr": mins_from_seconds(sw.get("reset_after_seconds")),
            "cst": cst,
        }
        codex_ok = True
    except Exception as e:
        pass

# ---- Update last-good caches ------------------------------------------------
if claude_ok and claude_fresh is not None:
    save_cache(claude_cache_file, claude_fresh)
if codex_ok and codex_fresh is not None:
    save_cache(codex_cache_file, codex_fresh)

# ---- Resolve effective values per provider ----------------------------------
claude_cache = load_cache(claude_cache_file) if sp else None
codex_cache  = load_cache(codex_cache_file)  if cp else None

# Claude effective values
if sp:
    if claude_ok and claude_fresh:
        c_vals = claude_fresh
    elif claude_cache:
        c_vals = claude_cache          # last-good, ok=False signals stale
    else:
        c_vals = {"s": -1, "sr": -1, "w": -1, "wr": -1, "st": "allowed"}
else:
    c_vals = {"s": -1, "sr": -1, "w": -1, "wr": -1, "st": "allowed"}

# Codex effective values
if cp:
    if codex_ok and codex_fresh:
        x_vals = codex_fresh
    elif codex_cache:
        x_vals = codex_cache
    else:
        x_vals = {"cs": -1, "csr": -1, "cw": -1, "cwr": -1, "cst": "unknown"}
else:
    x_vals = {"cs": -1, "csr": -1, "cw": -1, "cwr": -1, "cst": "unknown"}

# ---- Build and emit combined payload ----------------------------------------
payload = {
    "s":   c_vals["s"],
    "sr":  c_vals["sr"],
    "w":   c_vals["w"],
    "wr":  c_vals["wr"],
    "st":  c_vals["st"],
    "ok":  claude_ok,
    "sp":  sp,
    "cp":  cp,
    "cs":  x_vals["cs"],
    "csr": x_vals["csr"],
    "cw":  x_vals["cw"],
    "cwr": x_vals["cwr"],
    "cst": x_vals["cst"],
    "cok": codex_ok,
}
print(json.dumps(payload, separators=(",", ":")))

# ---- Exit code: 0 = success-or-none-present; 1 = all-present failed ---------
n_present  = (1 if sp else 0) + (1 if cp else 0)
n_ok       = (1 if claude_ok else 0) + (1 if codex_ok else 0)
if n_present > 0 and n_ok == 0:
    sys.exit(1)
sys.exit(0)
')
    local py_exit=$?

    [ -n "$payload" ] || { log "Error: empty payload after merge"; return 1; }

    log "Sending: $payload"
    write_gatt "$RX_CHAR_PATH" "$payload" || { log "Write failed"; return 1; }

    # Return the Python exit code so the caller can distinguish
    # "all-present-failed" (1) from "success-or-none-present" (0).
    return $py_exit
}

cleanup() {
    stop_notify_subscriber
    rm -f "$CLAUDE_CACHE" "$CODEX_CACHE"
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

    # Poll loop: tick every $TICK seconds. Poll both providers when the interval
    # has elapsed OR when the ESP requested a refresh. Always write a combined
    # snapshot. On success-or-none-present, advance LAST_POLL normally; if every
    # present provider failed, take POLL_FAIL_BACKOFF instead of retrying every
    # tick (avoids storming the rate-limited usage endpoints).
    LAST_POLL=0
    while is_connected; do
        NOW=$(date +%s)
        if [ -f "$REFRESH_FLAG" ] || (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            if [ -f "$REFRESH_FLAG" ]; then
                log "Refresh requested by device"
                rm -f "$REFRESH_FLAG"
            fi
            if poll; then
                # 0 = at least one present provider succeeded, or none present
                LAST_POLL=$NOW
            else
                # 1 = all present providers failed — back off
                LAST_POLL=$(( NOW - POLL_INTERVAL + POLL_FAIL_BACKOFF ))
            fi
        fi
        sleep "$TICK"
    done

    stop_notify_subscriber
    log "Device disconnected, reconnecting..."
    sleep 2
done
