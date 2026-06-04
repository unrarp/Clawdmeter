#!/bin/bash
# Install the Clawdmeter token broker (HTTP) on macOS.
# Creates a Python venv, writes the launchd plist, and loads
# the service. Bluetooth/bleak prerequisites are removed — no BLE, no Keychain
# BLE priming, no CoreBluetooth.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_SCRIPT="$REPO_DIR/daemon/token_broker.py"
VENV_DIR="$REPO_DIR/daemon/.venv"
VENV_PYTHON="$VENV_DIR/bin/python"

LAUNCHD_DIR="$HOME/Library/LaunchAgents"
PLIST_SRC="$REPO_DIR/daemon/com.user.clawdmeter-broker.plist"
PLIST_DEST="$LAUNCHD_DIR/com.user.clawdmeter-broker.plist"
PLIST_LABEL="com.user.clawdmeter-broker"

LOG_DIR="$HOME/Library/Logs"
LOG_OUT="$LOG_DIR/clawdmeter.stdout.log"
LOG_ERR="$LOG_DIR/clawdmeter.stderr.log"

log() { echo "[install] $*"; }

# ---- Python venv (broker is stdlib-only; venv kept for an isolated python) ---
log "Creating Python venv at $VENV_DIR ..."
python3 -m venv "$VENV_DIR"

# ---- Remove stale BLE MAC-cache dir if present -------------------------------
if [ -d "$HOME/.config/claude-usage-monitor" ]; then
    log "Removing legacy BLE cache: $HOME/.config/claude-usage-monitor"
    rm -rf "$HOME/.config/claude-usage-monitor"
fi

# ---- Clawdmeter config dir + secrets ----------------------------------------
CLAWDMETER_CFG="$HOME/.config/clawdmeter"
mkdir -p "$CLAWDMETER_CFG"

BROKER_ENV="$CLAWDMETER_CFG/broker.env"
if [ -f "$BROKER_ENV" ]; then
    log "broker.env already exists — reusing $BROKER_ENV"
    BROKER_KEY="$(grep -m1 '^CLAWDMETER_BROKER_KEY=' "$BROKER_ENV" | cut -d= -f2-)"
else
    read -r -s -p "[install] Enter broker shared secret (BROKER_KEY): " BROKER_KEY; echo
    printf 'CLAWDMETER_BROKER_KEY=%s\n' "$BROKER_KEY" > "$BROKER_ENV"
    chmod 600 "$BROKER_ENV"
    log "Wrote $BROKER_ENV (mode 600)"
fi
[ -n "$BROKER_KEY" ] || { echo "[install] BROKER_KEY must not be empty (broker.env present but unreadable, or empty input)." >&2; exit 1; }

CLAUDE_TOKEN_FILE="$CLAWDMETER_CFG/claude_setup_token"
if [ -f "$CLAUDE_TOKEN_FILE" ]; then
    log "claude_setup_token already exists — reusing $CLAUDE_TOKEN_FILE"
else
    read -r -s -p "[install] Paste your 'claude setup-token' value: " CLAUDE_TOKEN; echo
    [ -n "$CLAUDE_TOKEN" ] || { echo "[install] setup-token must not be empty." >&2; exit 1; }
    printf '%s\n' "$CLAUDE_TOKEN" > "$CLAUDE_TOKEN_FILE"
    chmod 600 "$CLAUDE_TOKEN_FILE"
    log "Wrote $CLAUDE_TOKEN_FILE (mode 600)"
fi

log "Codex credentials are read straight from ~/.codex/auth.json (no setup needed here)."

# ---- Launchd plist -----------------------------------------------------------
mkdir -p "$LAUNCHD_DIR"

# Retire the old usage-poller agent if present (pre-broker installs)
OLD_PLIST="$LAUNCHD_DIR/com.user.claude-usage-daemon.plist"
if [ -f "$OLD_PLIST" ]; then
    log "Retiring old claude-usage-daemon agent (replaced by clawdmeter-broker)"
    launchctl unload "$OLD_PLIST" 2>/dev/null || true
    rm -f "$OLD_PLIST"
fi

# Unload existing agent if loaded (ignore failure when not loaded)
launchctl unload "$PLIST_DEST" 2>/dev/null || true

# Expand path placeholders (controlled values) with sed; inject the secret
# separately via python so a BROKER_KEY containing & | \ can't corrupt the plist.
sed \
    -e "s|__PYTHON_BIN__|$VENV_PYTHON|g" \
    -e "s|__DAEMON_PATH__|$DAEMON_SCRIPT|g" \
    -e "s|__REPO_DIR__|$REPO_DIR|g" \
    -e "s|__HOME__|$HOME|g" \
    -e "s|__LOG_OUT__|$LOG_OUT|g" \
    -e "s|__LOG_ERR__|$LOG_ERR|g" \
    "$PLIST_SRC" > "$PLIST_DEST"
PLIST_DEST="$PLIST_DEST" BROKER_KEY="$BROKER_KEY" python3 -c "import os,pathlib,xml.sax.saxutils as x; p=pathlib.Path(os.environ['PLIST_DEST']); p.write_text(p.read_text().replace('__BROKER_KEY__', x.escape(os.environ['BROKER_KEY'])))"
chmod 600 "$PLIST_DEST"   # plist embeds the broker secret — keep it owner-only

log "Launchd plist installed: $PLIST_DEST"

# ---- Load and start the agent ------------------------------------------------
launchctl load "$PLIST_DEST"

log "Done. Check logs with: tail -f $LOG_OUT"
log "Broker listens on http://0.0.0.0:8080  (GET /tokens requires X-Broker-Key; GET /healthz is open)"
log "Use this same secret as BROKER_KEY in the firmware's net_config.h"
