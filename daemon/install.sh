#!/bin/bash
# Install the Clawdmeter usage daemon (HTTP) on Linux.
# Creates a Python venv, installs httpx, writes the systemd user unit, and
# enables the service. Bluetooth prerequisites are gone — no BLE, no bluez,
# no dbus priming.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_SCRIPT="$REPO_DIR/daemon/claude_usage_daemon.py"
VENV_DIR="$REPO_DIR/daemon/.venv"
VENV_PYTHON="$VENV_DIR/bin/python"

SYSTEMD_USER_DIR="$HOME/.config/systemd/user"
SERVICE_SRC="$REPO_DIR/daemon/claude-usage-daemon.service"
SERVICE_DEST="$SYSTEMD_USER_DIR/claude-usage-daemon.service"

log() { echo "[install] $*"; }

# ---- Python venv + dependencies ---------------------------------------------
log "Creating Python venv at $VENV_DIR ..."
python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --quiet httpx
log "httpx installed."

# ---- Remove stale BLE MAC-cache dir if present ------------------------------
if [ -d "$HOME/.config/claude-usage-monitor" ]; then
    log "Removing legacy BLE cache: $HOME/.config/claude-usage-monitor"
    rm -rf "$HOME/.config/claude-usage-monitor"
fi

# ---- Systemd user unit -------------------------------------------------------
mkdir -p "$SYSTEMD_USER_DIR"

# Expand placeholders: VENV_PYTHON and DAEMON_PATH
sed \
    -e "s|__VENV_PYTHON__|$VENV_PYTHON|g" \
    -e "s|__DAEMON_PATH__|$DAEMON_SCRIPT|g" \
    "$SERVICE_SRC" > "$SERVICE_DEST"

log "Systemd unit installed: $SERVICE_DEST"

# ---- Enable and start the service -------------------------------------------
systemctl --user daemon-reload
systemctl --user enable claude-usage-daemon
systemctl --user restart claude-usage-daemon

log "Done. Check status with: systemctl --user status claude-usage-daemon"
log "Daemon listens on http://0.0.0.0:8080/usage (LAN, unauthenticated)"
log "Point the device at http://\$(hostname).local:8080/usage"
