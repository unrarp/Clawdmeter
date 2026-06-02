#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls the Claude OAuth usage endpoint and the Codex wham/usage endpoint
independently, merges per-provider last-good caches, and writes a combined
JSON payload to the ESP32 "Claude Controller" peripheral over a custom GATT service.
Uses bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 300   # /api/oauth/usage is rate-limited (429/529 above ~1 req/min)
POLL_FAIL_BACKOFF = 60  # after a failed/throttled poll, retry this soon, not the full interval
TICK = 5
SCAN_TIMEOUT = 8.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
CODEX_AUTH_PATH = Path.home() / ".codex" / "auth.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

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


async def _async_none():
    """Awaitable resolving to None — placeholder in asyncio.gather for an absent
    provider so both provider slots can be gathered uniformly."""
    return None


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


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


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Claude Controller', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


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


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()

                # ---- Presence detection (re-checked every cycle) -------------
                sp = claude_present()
                cp = codex_present()

                # ---- Poll present providers concurrently ---------------------
                token = read_token() if sp else None
                creds = read_codex_creds() if cp else None
                if sp and not token:
                    log("Claude: no token available")
                claude_fresh, codex_fresh = await asyncio.gather(
                    poll_claude(token) if token else _async_none(),
                    poll_codex(creds[0], creds[1]) if creds else _async_none(),
                )

                # ---- Build combined payload and write ------------------------
                payload = build_payload(sp, cp, claude_fresh, codex_fresh)
                write_ok = await session.write_payload(payload)

                # ---- Backoff reconciliation ----------------------------------
                # Treat cycle as success if any present provider polled OK,
                # or if no provider is present (nothing to poll).
                n_present = (1 if sp else 0) + (1 if cp else 0)
                n_ok = (1 if claude_fresh is not None else 0) + (1 if codex_fresh is not None else 0)
                cycle_ok = write_ok and (n_ok > 0 or n_present == 0)

                if cycle_ok:
                    last_poll = time.time()
                    used_successfully = True
                else:
                    # All present providers failed (or write failed): back off
                    # POLL_FAIL_BACKOFF instead of retrying every TICK, so we
                    # don't storm the rate-limited usage endpoints.
                    last_poll = time.time() - POLL_INTERVAL + POLL_FAIL_BACKOFF

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


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

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
