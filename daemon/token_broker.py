#!/usr/bin/env python3
"""Clawdmeter token broker — Phase 2 of the self-sufficient device plan.

Hands the device fresh provider tokens over the LAN so it can call
api.anthropic.com / chatgpt.com directly; the device only contacts this broker
on first boot and on a provider 401. See
docs/plans/2026-06-04-token-broker-self-sufficient.md.

The broker does **no OAuth refresh of its own** (locked constraint). It passes
through the tokens already on this host — kept current by normal Claude Code /
Codex CLI use — and only decides "is this token usable right now?":

  GET /tokens  (X-Broker-Key required)
    200 {"claude":{"token":...},"codex":{"token":...,"account_id":...}}
        every provider has a usable token
    409 {"claude":{...},"codex":{"needs_action":"..."}}
        at least one provider can't be supplied; the others still carry tokens
  GET /healthz -> 200 {"ok":true}

Note: `codex exec` cannot refresh the token ("auth token refresh is not
supported in exec mode"), so Codex freshness relies on the ~10-day access_token
in ~/.codex/auth.json staying current through normal codex use. The broker reads
its JWT `exp` to gate 200 vs 409. Claude uses a `claude setup-token` (opaque,
~1yr) whose expiry can't be read locally, so its only gate is presence — a truly
expired setup-token surfaces as the device's retry still 401-ing, after which
the device shows needs-action.
"""

import base64
import hmac
import json
import logging
import os
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

log = logging.getLogger("clawdmeter-broker")

HTTP_HOST = "0.0.0.0"
HTTP_PORT = int(os.environ.get("CLAWDMETER_PORT", "8080"))

# Shared secret the device sends as X-Broker-Key. Required — the broker vends
# spendable credentials, so it refuses to start without one.
BROKER_KEY = os.environ.get("CLAWDMETER_BROKER_KEY")

# Claude: a `claude setup-token` (inference-scoped, ~1yr). From env, else a file.
CLAUDE_TOKEN_ENV = "CLAUDE_CODE_OAUTH_TOKEN"
CLAUDE_TOKEN_FILE = Path(
    os.environ.get("CLAWDMETER_CLAUDE_TOKEN_FILE")
    or Path.home() / ".config" / "clawdmeter" / "claude_setup_token"
)

# Codex: subscription OAuth access_token + account_id, refreshed by normal use.
CODEX_AUTH_PATH = Path.home() / ".codex" / "auth.json"
CODEX_EXP_MARGIN = 300  # treat a token within 5 min of expiry as unusable


def claude_token() -> str | None:
    """A raw `claude setup-token` from env, else the configured file. None if
    neither. Forwarded verbatim — must be a raw token, not a JSON credentials blob."""
    env = (os.environ.get(CLAUDE_TOKEN_ENV) or "").strip()
    if env:
        return env
    try:
        return CLAUDE_TOKEN_FILE.read_text().strip() or None
    except OSError:
        return None


def jwt_exp(token: str) -> int | None:
    """Unix `exp` from a JWT's payload segment, or None if not decodable."""
    try:
        seg = token.split(".")[1]
        seg += "=" * (-len(seg) % 4)  # restore base64url padding
        return int(json.loads(base64.urlsafe_b64decode(seg))["exp"])
    except (AttributeError, IndexError, KeyError, ValueError, TypeError):
        return None


def codex_creds() -> tuple[str, str] | None:
    """(access_token, account_id) from ~/.codex/auth.json, or None."""
    try:
        tokens = json.loads(CODEX_AUTH_PATH.read_text())["tokens"]
        at, acct = tokens["access_token"], tokens["account_id"]
        return (at, acct) if at and acct else None
    except (OSError, KeyError, ValueError, TypeError) as e:
        log.warning(f"Codex: error reading auth.json: {e}")
        return None


def build_tokens() -> tuple[int, dict]:
    """(http_status, body): 200 if every provider has a usable token, else 409
    with needs_action on the provider(s) that can't be supplied."""
    body: dict = {}
    ok = True

    ct = claude_token()
    if ct:
        body["claude"] = {"token": ct}
    else:
        body["claude"] = {"needs_action": "run `claude setup-token` and store it for the broker"}
        ok = False

    cc = codex_creds()
    if cc is None:
        body["codex"] = {"needs_action": "run codex login"}
        ok = False
    else:
        at, acct = cc
        exp = jwt_exp(at)
        if exp is not None and exp - time.time() > CODEX_EXP_MARGIN:
            body["codex"] = {"token": at, "account_id": acct}
        else:
            reason = "unparseable" if exp is None else "expired"
            body["codex"] = {"needs_action": f"codex token {reason} — run codex"}
            ok = False

    return (200 if ok else 409, body)


class BrokerHandler(BaseHTTPRequestHandler):
    server_version = "clawdmeter-broker/1.0"

    def _send(self, code: int, obj: dict) -> None:
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self) -> bool:
        if not BROKER_KEY:  # never allow-all, even if the startup guard is bypassed
            return False
        key = self.headers.get("X-Broker-Key") or ""
        return hmac.compare_digest(key, BROKER_KEY)

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/healthz":
            self._send(200, {"ok": True})
        elif path == "/tokens":
            if not self._authorized():
                self._send(403, {"error": "forbidden"})
                return
            code, body = build_tokens()
            self._send(code, body)
        else:
            self._send(404, {"error": "not found"})

    def log_message(self, fmt: str, *args) -> None:
        log.info(f"HTTP {self.address_string()} {fmt % args}")


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(message)s",
        datefmt="%H:%M:%S",
    )
    if not BROKER_KEY:
        log.error(
            "CLAWDMETER_BROKER_KEY is not set — refusing to start "
            "(the broker vends spendable credentials)."
        )
        sys.exit(1)
    log.info("=== Clawdmeter token broker ===")
    claude_src = "env" if os.environ.get(CLAUDE_TOKEN_ENV) else f"file {CLAUDE_TOKEN_FILE}"
    log.info(f"Claude token source: {claude_src}; Codex creds: {CODEX_AUTH_PATH}")
    server = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), BrokerHandler)
    log.info(f"GET http://{HTTP_HOST}:{HTTP_PORT}/tokens (X-Broker-Key required), /healthz")

    def _stop(*_args: object) -> None:
        log.info("Broker stopping")
        # shutdown() blocks until serve_forever() returns, so it must run off
        # the serving thread — calling it inline from the handler would deadlock.
        threading.Thread(target=server.shutdown, daemon=True).start()

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, _stop)
    try:
        server.serve_forever()
    finally:
        server.server_close()  # release the listening socket
        log.info("Broker stopped")


if __name__ == "__main__":
    main()
