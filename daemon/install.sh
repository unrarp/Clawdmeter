#!/bin/bash
# Install the Clawdmeter token broker (HTTP) on Linux.
# Creates a Python venv, writes the systemd user unit, and
# enables the service. Bluetooth prerequisites are gone — no BLE, no bluez,
# no dbus priming.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_SCRIPT="$REPO_DIR/daemon/token_broker.py"
VENV_DIR="$REPO_DIR/daemon/.venv"
VENV_PYTHON="$VENV_DIR/bin/python"

SYSTEMD_USER_DIR="$HOME/.config/systemd/user"
SERVICE_SRC="$REPO_DIR/daemon/clawdmeter-broker.service"
SERVICE_DEST="$SYSTEMD_USER_DIR/clawdmeter-broker.service"

log() { echo "[install] $*"; }

# ---- Python venv (broker is stdlib-only; venv kept for an isolated python) ---
log "Creating Python venv at $VENV_DIR ..."
python3 -m venv "$VENV_DIR"

# ---- Remove stale BLE MAC-cache dir if present ------------------------------
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
else
    read -r -s -p "[install] Enter broker shared secret (BROKER_KEY): " BROKER_KEY; echo
    [ -n "$BROKER_KEY" ] || { echo "[install] BROKER_KEY must not be empty — the broker refuses to start without it." >&2; exit 1; }
    printf 'CLAWDMETER_BROKER_KEY=%s\n' "$BROKER_KEY" > "$BROKER_ENV"
    chmod 600 "$BROKER_ENV"
    log "Wrote $BROKER_ENV (mode 600)"
fi

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

# ---- Retire the old usage-poller unit if present (pre-broker installs) -------
if [ -f "$SYSTEMD_USER_DIR/claude-usage-daemon.service" ]; then
    log "Retiring old claude-usage-daemon unit (replaced by clawdmeter-broker)"
    systemctl --user disable --now claude-usage-daemon 2>/dev/null || true
    rm -f "$SYSTEMD_USER_DIR/claude-usage-daemon.service"
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
systemctl --user enable clawdmeter-broker
systemctl --user restart clawdmeter-broker

log "Done. Check status with: systemctl --user status clawdmeter-broker"
log "Broker listens on http://0.0.0.0:8080  (GET /tokens requires X-Broker-Key; GET /healthz is open)"
log "Use this same secret as BROKER_KEY in the firmware's net_config.h"
