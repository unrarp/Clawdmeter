#!/bin/bash
# Install the Clawdmeter usage daemon (HTTP) on macOS.
# Creates a Python venv, installs httpx, writes the launchd plist, and loads
# the service. Bluetooth/bleak prerequisites are removed — no BLE, no Keychain
# BLE priming, no CoreBluetooth.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_SCRIPT="$REPO_DIR/daemon/claude_usage_daemon.py"
VENV_DIR="$REPO_DIR/daemon/.venv"
VENV_PYTHON="$VENV_DIR/bin/python"

LAUNCHD_DIR="$HOME/Library/LaunchAgents"
PLIST_SRC="$REPO_DIR/daemon/com.user.claude-usage-daemon.plist"
PLIST_DEST="$LAUNCHD_DIR/com.user.claude-usage-daemon.plist"
PLIST_LABEL="com.user.claude-usage-daemon"

LOG_DIR="$HOME/Library/Logs"
LOG_OUT="$LOG_DIR/clawdmeter.stdout.log"
LOG_ERR="$LOG_DIR/clawdmeter.stderr.log"

log() { echo "[install] $*"; }

# ---- Sanity check: Claude token reachable ------------------------------------
if ! security find-generic-password -s "Claude Code-credentials" -a "$(id -un)" -w &>/dev/null; then
    echo "[install] WARNING: Claude credentials not found in Keychain."
    echo "          Sign in to Claude Code first, then re-run this script if the"
    echo "          daemon shows sp:false on first poll. Installation continues."
fi

# ---- Python venv + dependencies ----------------------------------------------
log "Creating Python venv at $VENV_DIR ..."
python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --quiet httpx
log "httpx installed."

# ---- Remove stale BLE MAC-cache dir if present -------------------------------
if [ -d "$HOME/.config/claude-usage-monitor" ]; then
    log "Removing legacy BLE cache: $HOME/.config/claude-usage-monitor"
    rm -rf "$HOME/.config/claude-usage-monitor"
fi

# ---- Launchd plist -----------------------------------------------------------
mkdir -p "$LAUNCHD_DIR"

# Unload existing agent if loaded (ignore failure when not loaded)
launchctl unload "$PLIST_DEST" 2>/dev/null || true

# Expand placeholders in the plist template
sed \
    -e "s|__PYTHON_BIN__|$VENV_PYTHON|g" \
    -e "s|__DAEMON_PATH__|$DAEMON_SCRIPT|g" \
    -e "s|__REPO_DIR__|$REPO_DIR|g" \
    -e "s|__HOME__|$HOME|g" \
    -e "s|__LOG_OUT__|$LOG_OUT|g" \
    -e "s|__LOG_ERR__|$LOG_ERR|g" \
    "$PLIST_SRC" > "$PLIST_DEST"

log "Launchd plist installed: $PLIST_DEST"

# ---- Load and start the agent ------------------------------------------------
launchctl load "$PLIST_DEST"

log "Done. Check logs with: tail -f $LOG_OUT"
log "Daemon listens on http://0.0.0.0:8080/usage (LAN, unauthenticated)"
log "Point the device at http://\$(hostname -s).local:8080/usage"
