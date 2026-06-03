#!/usr/bin/env python3
"""Clawdmeter usage daemon (HTTP) — cross-platform (Linux + macOS).

Polls the Claude OAuth usage endpoint and the Codex wham/usage endpoint
independently, merges per-provider last-good caches into one combined JSON
snapshot, and serves it over plain HTTP on the LAN. The ESP32 gauge pulls
`GET /usage` and renders it.

Transport is HTTP, not BLE: there is no scanning, no pairing, no GATT, and no
MAC cache. Discovery is mDNS — the device targets this host's `<hostname>.local`,
which the OS already advertises (Avahi/systemd-resolved on Linux, Bonjour on
macOS), so this daemon does not register anything itself.

The poll/merge/presence/last-good logic is unchanged from the prior BLE daemon —
only the delivery changed (GATT write → HTTP serve).
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import httpx

POLL_INTERVAL = 300   # /api/oauth/usage is rate-limited (429/529 above ~1 req/min)
POLL_FAIL_BACKOFF = 60  # after a failed/throttled poll, retry this soon, not the full interval
TICK = 5              # poll-loop granularity (responsive shutdown)

# HTTP server: bind all interfaces, unauthenticated (trusted home LAN, usage %
# only). Port overridable via env for testing; defaults to 8080.
HTTP_HOST = "0.0.0.0"
HTTP_PORT = int(os.environ.get("CLAWDMETER_PORT", "8080"))

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
CODEX_AUTH_PATH = Path.home() / ".codex" / "auth.json"

CLAUDE_API_URL = "https://api.anthropic.com/api/oauth/usage"
CODEX_API_URL = "https://chatgpt.com/backend-api/wham/usage"

CLAUDE_API_HEADERS_TEMPLATE = {
    "anthropic-beta": "oauth-2025-04-20",
    "User-Agent": "claude-code/2.1.5",
}
CODEX_API_HEADERS_TEMPLATE = {
    "User-Agent": "claude-code/2.1.5",
}

# Module-level last-good caches: set to a dict of numeric fields on first
# successful poll; None until then (drives the -1 sentinel path).
_claude_last: dict | None = None
_codex_last: dict | None = None
# Latches True once Claude has been seen present (see claude_present()).
_claude_seen = False

# Latest full wire snapshot served over HTTP. None until the first poll
# completes; guarded by _payload_lock for the HTTP thread / poll loop hand-off.
_payload_lock = threading.Lock()
_last_payload: dict | None = None


async def _async_none():
    """Awaitable resolving to None — placeholder in asyncio.gather for an absent
    provider so both provider slots can be gathered uniformly."""
    return None


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def set_payload(payload: dict) -> None:
    global _last_payload
    with _payload_lock:
        _last_payload = payload


def get_payload() -> dict | None:
    with _payload_lock:
        return _last_payload


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def read_codex_creds() -> tuple[str, str] | None:
    """Read (access_token, account_id) from ~/.codex/auth.json, or None."""
    try:
        data = json.loads(CODEX_AUTH_PATH.read_text())
        access_token = data["tokens"]["access_token"]
        account_id = data["tokens"]["account_id"]
        if not access_token or not account_id:
            return None
        return (access_token, account_id)
    except (OSError, KeyError, ValueError, TypeError) as e:
        log(f"Codex: error reading auth.json: {e}")
        return None


def claude_present() -> bool:
    """Claude is present if we can obtain a token OR the creds file exists.

    On macOS the token lives in the Keychain rather than a file, so we try
    read_token() first. On Linux the creds file is the definitive signal.
    Once Claude has been seen present, presence latches True so a *transient*
    Keychain denial/timeout doesn't flip the page to "No Claude account" while
    the account is still configured (a real removal needs a daemon restart).
    """
    global _claude_seen
    if CREDENTIALS_PATH.exists():
        _claude_seen = True
        return True
    # macOS only: token may be in Keychain with no on-disk file
    if sys.platform == "darwin":
        if read_token() is not None:
            _claude_seen = True
            return True
        return _claude_seen
    return False


def codex_present() -> bool:
    """Codex is present iff ~/.codex/auth.json exists."""
    return CODEX_AUTH_PATH.exists()


async def poll_claude(token: str) -> dict | None:
    """Fetch Claude usage; return dict of wire values on success, None on failure."""
    headers = dict(CLAUDE_API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.get(CLAUDE_API_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"Claude API call failed: {e}")
        return None
    if resp.status_code != 200:
        log(f"Claude usage API HTTP {resp.status_code}: {resp.text[:200]}")
        return None
    try:
        data = resp.json()
    except ValueError as e:
        log(f"Claude usage API returned non-JSON: {e}")
        return None

    now = datetime.now(timezone.utc)

    def reset_minutes(iso: str | None) -> int:
        """ISO-8601 -> whole minutes from now; -1 if absent/unparseable."""
        if not iso:
            return -1
        try:
            dt = datetime.fromisoformat(iso)
            if dt.tzinfo is None:               # naive timestamp -> assume UTC
                dt = dt.replace(tzinfo=timezone.utc)
            return max(0, round((dt - now).total_seconds() / 60))
        except (ValueError, TypeError):
            return -1

    # Utilization is already 0-100; round once and derive st from the rounded
    # value so "s":100 can never pair with "st":"allowed" (raw 99.6 rounds up).
    fh = data.get("five_hour") or {}
    sd = data.get("seven_day") or {}
    s = round(fh.get("utilization") or 0)
    w = round(sd.get("utilization") or 0)
    return {
        "s": s,
        "sr": reset_minutes(fh.get("resets_at")),
        "w": w,
        "wr": reset_minutes(sd.get("resets_at")),
        "st": "limited" if s >= 100 else "allowed",
    }


async def poll_codex(access_token: str, account_id: str) -> dict | None:
    """Fetch Codex wham/usage; return dict of wire values on success, None on failure."""
    headers = dict(CODEX_API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {access_token}"
    headers["ChatGPT-Account-Id"] = account_id
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.get(CODEX_API_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"Codex API call failed: {e}")
        return None
    if resp.status_code != 200:
        log(f"Codex usage API HTTP {resp.status_code}: {resp.text[:200]}")
        return None
    try:
        data = resp.json()
    except ValueError as e:
        log(f"Codex usage API returned non-JSON: {e}")
        return None

    try:
        rl = data.get("rate_limit") or {}
        pw = rl.get("primary_window") or {}
        sw = rl.get("secondary_window") or {}
        cs = round(pw.get("used_percent") or 0)
        cw = round(sw.get("used_percent") or 0)
        allowed = rl.get("allowed", True)
        lim_reached = rl.get("limit_reached", False)
        cst = "limited" if (not allowed or lim_reached or cs >= 100) else "allowed"
        # reset_after_seconds is a relative integer — no ISO math needed
        def secs_to_mins(v) -> int:
            try:
                return max(0, int(v) // 60)
            except (TypeError, ValueError):
                return -1
        return {
            "cs": cs,
            "csr": secs_to_mins(pw.get("reset_after_seconds")),
            "cw": cw,
            "cwr": secs_to_mins(sw.get("reset_after_seconds")),
            "cst": cst,
        }
    except Exception as e:
        log(f"Codex usage parse error: {e}")
        return None


def build_payload(
    sp: bool,
    cp: bool,
    claude_fresh: dict | None,
    codex_fresh: dict | None,
) -> dict:
    """Merge per-provider fresh + last-good into the full wire snapshot.

    Updates the module-level _claude_last / _codex_last caches when fresh
    data is available.
    """
    global _claude_last, _codex_last

    # An absent provider must not serve cached numbers from a previous account —
    # drop its last-good so a re-add starts at the -1/"Connecting" sentinel.
    if not sp:
        _claude_last = None
    if not cp:
        _codex_last = None

    claude_ok = claude_fresh is not None
    codex_ok = codex_fresh is not None

    if claude_ok:
        _claude_last = claude_fresh
    if codex_ok:
        _codex_last = codex_fresh

    # Resolve effective Claude values
    if sp:
        if claude_ok and claude_fresh:
            c = claude_fresh
        elif _claude_last:
            c = _claude_last        # last-good; ok=False signals stale to device
        else:
            c = {"s": -1, "sr": -1, "w": -1, "wr": -1, "st": "allowed"}
    else:
        c = {"s": -1, "sr": -1, "w": -1, "wr": -1, "st": "allowed"}

    # Resolve effective Codex values
    if cp:
        if codex_ok and codex_fresh:
            x = codex_fresh
        elif _codex_last:
            x = _codex_last
        else:
            x = {"cs": -1, "csr": -1, "cw": -1, "cwr": -1, "cst": "unknown"}
    else:
        x = {"cs": -1, "csr": -1, "cw": -1, "cwr": -1, "cst": "unknown"}

    return {
        "s":   c["s"],
        "sr":  c["sr"],
        "w":   c["w"],
        "wr":  c["wr"],
        "st":  c["st"],
        "ok":  claude_ok,
        "sp":  sp,
        "cp":  cp,
        "cs":  x["cs"],
        "csr": x["csr"],
        "cw":  x["cw"],
        "cwr": x["cwr"],
        "cst": x["cst"],
        "cok": codex_ok,
    }


# --- HTTP server -------------------------------------------------------------


class UsageHandler(BaseHTTPRequestHandler):
    """Serves the cached wire snapshot. GET /usage -> 200 payload | 503 if none."""

    server_version = "clawdmeter/1.0"

    def _send_json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/usage":
            payload = get_payload()
            if payload is None:
                self._send_json(503, {"error": "no data yet"})
            else:
                self._send_json(200, payload)
        elif path == "/healthz":
            self._send_json(200, {"ok": True})
        else:
            self._send_json(404, {"error": "not found"})

    def log_message(self, fmt: str, *args) -> None:
        # Route the access log through our timestamped logger; keep it terse.
        log(f"HTTP {self.address_string()} {fmt % args}")


def start_http_server() -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), UsageHandler)
    t = threading.Thread(target=server.serve_forever, name="http", daemon=True)
    t.start()
    log(f"HTTP server: GET http://{HTTP_HOST}:{HTTP_PORT}/usage")
    return server


# --- Poll loop ---------------------------------------------------------------


async def poll_loop(stop_event: asyncio.Event) -> None:
    """Poll present providers every POLL_INTERVAL and publish the merged snapshot.

    No device hand-shake: the gauge pulls /usage on its own cadence, so there is
    no on-demand refresh path. ``last_poll == 0.0`` forces an immediate first
    poll so /usage has data within seconds of startup.
    """
    last_poll = 0.0
    while not stop_event.is_set():
        now = time.time()
        if last_poll == 0.0 or now - last_poll >= POLL_INTERVAL:
            # ---- Presence detection (re-checked every cycle) -----------------
            sp = claude_present()
            cp = codex_present()

            # ---- Poll present providers concurrently -------------------------
            token = read_token() if sp else None
            creds = read_codex_creds() if cp else None
            if sp and not token:
                log("Claude: no token available")
            claude_fresh, codex_fresh = await asyncio.gather(
                poll_claude(token) if token else _async_none(),
                poll_codex(creds[0], creds[1]) if creds else _async_none(),
            )

            # ---- Build combined payload and publish --------------------------
            payload = build_payload(sp, cp, claude_fresh, codex_fresh)
            set_payload(payload)
            log(f"Updated: {json.dumps(payload, separators=(',', ':'))}")

            # ---- Backoff reconciliation --------------------------------------
            # Treat cycle as success if any present provider polled OK, or if no
            # provider is present (nothing to poll).
            n_present = (1 if sp else 0) + (1 if cp else 0)
            n_ok = (1 if claude_fresh is not None else 0) + (1 if codex_fresh is not None else 0)
            cycle_ok = n_ok > 0 or n_present == 0

            if cycle_ok:
                last_poll = time.time()
            else:
                # All present providers failed: back off POLL_FAIL_BACKOFF
                # instead of retrying every TICK, so we don't storm the
                # rate-limited usage endpoints.
                last_poll = time.time() - POLL_INTERVAL + POLL_FAIL_BACKOFF

        try:
            await asyncio.wait_for(stop_event.wait(), timeout=TICK)
        except asyncio.TimeoutError:
            pass


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Clawdmeter usage daemon (HTTP) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")
    server = start_http_server()
    try:
        await poll_loop(stop_event)
    finally:
        server.shutdown()
        log("HTTP server stopped")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
