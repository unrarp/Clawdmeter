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
import logging
import os
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Awaitable, Callable, TypedDict

import httpx

POLL_INTERVAL = 300   # /api/oauth/usage is rate-limited (429/529 above ~1 req/min)
POLL_FAIL_BACKOFF = 60  # after a failed/throttled poll, retry this soon, not the full interval
TICK = 5              # poll-loop granularity (responsive shutdown)

HTTP_TIMEOUT = 20.0    # per-request timeout for upstream usage API calls (seconds)
ERR_SNIPPET = 200      # max chars of an error response body to log
KEYCHAIN_TIMEOUT = 10  # `security find-generic-password` subprocess timeout (seconds)
USER_AGENT = "claude-code/2.1.5"

# Module logger. Handlers/level are configured in main() (the entry point), not
# at import time, so importing this module (e.g. from tests) doesn't reconfigure
# the root logger.
log = logging.getLogger("clawdmeter")

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
    "User-Agent": USER_AGENT,
}
CODEX_API_HEADERS_TEMPLATE = {
    "User-Agent": USER_AGENT,
}

class WirePayload(TypedDict):
    """The flat 14-key wire snapshot served at GET /usage.

    Annotation-only; at runtime this is a plain dict (json.dumps-able).
    """

    s: int
    sr: int
    w: int
    wr: int
    st: str
    ok: bool
    sp: bool
    cp: bool
    cs: int
    csr: int
    cw: int
    cwr: int
    cst: str
    cok: bool


# Sentinel values served when a provider has no data (absent, or present but
# never succeeded). The device renders these as "Connecting…".
CLAUDE_SENTINEL = {"s": -1, "sr": -1, "w": -1, "wr": -1, "st": "allowed"}
CODEX_SENTINEL = {"cs": -1, "csr": -1, "cw": -1, "cwr": -1, "cst": "unknown"}

# Module-level last-good caches: set to a dict of numeric fields on first
# successful poll; None until then (drives the -1 sentinel path).
_claude_last: dict | None = None
_codex_last: dict | None = None
# Latches True once Claude has been seen present (see claude_present()).
_claude_seen = False

# Latest full wire snapshot served over HTTP. None until the first poll
# completes; guarded by _payload_lock for the HTTP thread / poll loop hand-off.
_payload_lock = threading.Lock()
_last_payload: WirePayload | None = None


async def _async_none():
    """Awaitable resolving to None — placeholder in asyncio.gather for an absent
    provider so both provider slots can be gathered uniformly."""
    return None


def set_payload(payload: WirePayload) -> None:
    global _last_payload
    with _payload_lock:
        _last_payload = payload


def get_payload() -> WirePayload | None:
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
            timeout=KEYCHAIN_TIMEOUT,
        )
    except subprocess.CalledProcessError as e:
        log.error(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log.error(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log.error(f"Error reading credentials: {e}")
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
        log.warning(f"Codex: error reading auth.json: {e}")
        return None


def _claude_creds() -> tuple[str] | None:
    """Read the Claude creds tuple for poll_claude(*creds), or None.

    Reads the token once here for the poll creds. (On macOS, claude_present()
    also reads the Keychain for presence detection, so a full cycle still does
    two reads — a pre-existing cost, not introduced by this helper.)
    """
    t = read_token()
    return (t,) if t else None


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


async def _fetch_json(url: str, headers: dict, who: str) -> dict | None:
    """Shared HTTP GET → JSON skeleton for the provider pollers.

    Returns the parsed JSON body on a 200 response, or None on transport
    error / non-200 / non-JSON body (logging the reason). Field mapping is
    each poller's responsibility.
    """
    try:
        async with httpx.AsyncClient(timeout=HTTP_TIMEOUT) as http:
            resp = await http.get(url, headers=headers)
    except httpx.HTTPError as e:
        log.warning(f"{who} API call failed: {e}")
        return None
    if resp.status_code != 200:
        log.warning(f"{who} usage API HTTP {resp.status_code}: {resp.text[:ERR_SNIPPET]}")
        return None
    try:
        return resp.json()
    except ValueError as e:
        log.warning(f"{who} usage API returned non-JSON: {e}")
        return None


async def poll_claude(token: str) -> dict | None:
    """Fetch Claude usage; return dict of wire values on success, None on failure."""
    headers = dict(CLAUDE_API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    data = await _fetch_json(CLAUDE_API_URL, headers, "Claude")
    if data is None:
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
    data = await _fetch_json(CODEX_API_URL, headers, "Codex")
    if data is None:
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
    except (KeyError, TypeError, ValueError, AttributeError) as e:
        log.warning(f"Codex usage parse error: {e}")
        return None


@dataclass
class Provider:
    """One pollable usage provider — drives the data-driven poll dispatch."""

    name: str
    present: Callable[[], bool]
    read_creds: Callable[[], tuple | None]   # creds args for poll(), or None
    poll: Callable[..., Awaitable[dict | None]]  # awaited as poll(*creds)


# Order is load-bearing: poll_loop maps gathered results back by index and
# passes them positionally to build_payload(claude, codex, ...).
PROVIDERS = [
    Provider("claude", claude_present, _claude_creds, poll_claude),
    Provider("codex",  codex_present,  read_codex_creds, poll_codex),
]


def resolve_provider(
    present: bool,
    fresh: dict | None,
    last_good: dict | None,
    sentinel: dict,
) -> tuple[dict, dict | None]:
    """Resolve a provider's effective wire values and its new last-good cache.

    Return (effective_values, new_last_good). Absent or never-succeeded -> a
    fresh COPY of sentinel (never the shared module constant) + cleared cache;
    fresh -> fresh + updated cache; present-but-failing with last-good ->
    last_good (cache stays that same dict).
    """
    if not present:
        return dict(sentinel), None
    if fresh is not None:
        return fresh, fresh
    if last_good is not None:
        return last_good, last_good
    return dict(sentinel), None


def build_payload(
    sp: bool,
    cp: bool,
    claude_fresh: dict | None,
    codex_fresh: dict | None,
) -> WirePayload:
    """Merge per-provider fresh + last-good into the full wire snapshot.

    Updates the module-level _claude_last / _codex_last caches when fresh
    data is available.
    """
    global _claude_last, _codex_last

    c, _claude_last = resolve_provider(sp, claude_fresh, _claude_last, CLAUDE_SENTINEL)
    x, _codex_last = resolve_provider(cp, codex_fresh, _codex_last, CODEX_SENTINEL)

    return {
        "s":   c["s"],
        "sr":  c["sr"],
        "w":   c["w"],
        "wr":  c["wr"],
        "st":  c["st"],
        "ok":  claude_fresh is not None,
        "sp":  sp,
        "cp":  cp,
        "cs":  x["cs"],
        "csr": x["csr"],
        "cw":  x["cw"],
        "cwr": x["cwr"],
        "cst": x["cst"],
        "cok": codex_fresh is not None,
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
        log.info(f"HTTP {self.address_string()} {fmt % args}")


def start_http_server() -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), UsageHandler)
    t = threading.Thread(target=server.serve_forever, name="http", daemon=True)
    t.start()
    log.info(f"HTTP server: GET http://{HTTP_HOST}:{HTTP_PORT}/usage")
    return server


# --- Poll loop ---------------------------------------------------------------


def cycle_succeeded(n_present: int, n_ok: int) -> bool:
    """A poll cycle counts as success if any present provider polled OK, or if no
    provider is present (nothing to poll). Drives the 300s-vs-60s backoff: a
    failed cycle (some provider present, none OK) takes POLL_FAIL_BACKOFF."""
    return n_ok > 0 or n_present == 0


async def poll_loop(stop_event: asyncio.Event) -> None:
    """Poll present providers every POLL_INTERVAL and publish the merged snapshot.

    No device hand-shake: the gauge pulls /usage on its own cadence, so there is
    no on-demand refresh path. ``next_poll_at == 0.0`` forces an immediate first
    poll so /usage has data within seconds of startup.
    """
    next_poll_at = 0.0
    while not stop_event.is_set():
        now = time.time()
        if now >= next_poll_at:
            # ---- Presence detection (re-checked every cycle) -----------------
            present = {p.name: p.present() for p in PROVIDERS}
            sp = present["claude"]
            cp = present["codex"]

            # ---- Read creds for present providers ----------------------------
            creds = {p.name: (p.read_creds() if present[p.name] else None) for p in PROVIDERS}
            if sp and not creds["claude"]:
                log.warning("Claude: no token available")

            # ---- Poll present providers concurrently -------------------------
            fresh = await asyncio.gather(*[
                p.poll(*creds[p.name]) if creds[p.name] else _async_none()
                for p in PROVIDERS
            ])
            claude_fresh, codex_fresh = fresh[0], fresh[1]

            # ---- Build combined payload and publish --------------------------
            payload = build_payload(present["claude"], present["codex"], claude_fresh, codex_fresh)
            set_payload(payload)
            log.info(f"Updated: {json.dumps(payload, separators=(',', ':'))}")

            # ---- Backoff reconciliation --------------------------------------
            # Treat cycle as success if any present provider polled OK, or if no
            # provider is present (nothing to poll).
            n_present = sum(1 for p in PROVIDERS if present[p.name])
            n_ok = sum(1 for r in fresh if r is not None)
            cycle_ok = cycle_succeeded(n_present, n_ok)

            # All present providers failing → retry after POLL_FAIL_BACKOFF
            # instead of every TICK, so we don't storm the rate-limited endpoints.
            next_poll_at = time.time() + (POLL_INTERVAL if cycle_ok else POLL_FAIL_BACKOFF)

        try:
            await asyncio.wait_for(stop_event.wait(), timeout=TICK)
        except asyncio.TimeoutError:
            pass


async def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(message)s",
        datefmt="%H:%M:%S",
    )
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log.info("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log.info("=== Clawdmeter usage daemon (HTTP) ===")
    log.info(f"Poll interval: {POLL_INTERVAL}s")
    server = start_http_server()
    try:
        await poll_loop(stop_event)
    finally:
        server.shutdown()
        log.info("HTTP server stopped")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
