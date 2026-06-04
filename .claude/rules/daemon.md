---
paths:
  - "daemon/**"
---

# Daemon rules (token broker)

The daemon is **`daemon/token_broker.py`** — a credential broker, **not** a usage
poller. It does no polling, no usage field-mapping, and no OAuth refresh. The
device fetches usage **directly** from the provider APIs and synthesises the
14-key wire JSON on-device (see `.claude/rules/networking.md` "Device-side provider
usage mapping"); the broker only hands the device fresh tokens when it asks for
them (first boot, or after a provider `401`). (The old `claude_usage_daemon.py`
poller + `/usage` endpoint are retired by this broker — deleted at the cutover
commit; see `docs/plans/2026-06-04-token-broker-self-sufficient.md`.)

- **Endpoints + status code = the device's whole decision tree.** `GET /tokens`
  (requires the `X-Broker-Key` header) answers in one round-trip:
  - `200` `{"claude":{"token":...},"codex":{"token":...,"account_id":...}}` — every
    provider has a token that works *right now*.
  - `409` — at least one provider can't be supplied; that provider's value is
    `{"needs_action":"<human string>"}` (no `token`). Usable siblings still carry
    their token. The device caches the usable ones, marks the `409` provider
    needs-action, and stops refetching it until the next successful `/tokens`.

  `GET /healthz` → `200 {"ok":true}` (no auth). `expires_at` is deliberately
  omitted — the broker guarantees freshness before answering; the device's
  `401`-refetch is the freshness mechanism.

- **The broker NEVER refreshes or mints tokens (locked constraint).** It only
  reads credentials already on the host (kept current by normal Claude Code /
  Codex CLI use) and decides usable-vs-needs-action. No `grant_type=refresh_token`,
  no CLI subprocess. `codex exec` *cannot* refresh ("auth token refresh is not
  supported in exec mode"), so pass-through is the only viable shape.

- **Token sourcing, per provider:**
  - **Claude** = a `claude setup-token` (inference-scoped, ~1 yr). Read from
    `CLAUDE_CODE_OAUTH_TOKEN` env, else the file
    `~/.config/clawdmeter/claude_setup_token` (override:
    `CLAWDMETER_CLAUDE_TOKEN_FILE`). Forwarded **verbatim** — must be a raw token,
    not a JSON credentials blob. Expiry is *not* locally readable (opaque token),
    so **presence is the only gate**; a truly-expired setup-token surfaces only as
    the device's retry still 401-ing → device then shows needs-action.
  - **Codex** = `tokens.access_token` + `tokens.account_id` from
    `~/.codex/auth.json`. The access_token is a JWT (~10-day life); `jwt_exp()`
    decodes its `exp` locally (zero cost) and the broker serves it while >5 min
    from expiry (`CODEX_EXP_MARGIN`), else `409`/needs-action.

- **`CLAWDMETER_BROKER_KEY` is required — the broker refuses to start without it**
  (it vends spendable credentials). `_authorized()` also returns `False` when the
  key is unset even if the startup guard were bypassed, and compares with
  `hmac.compare_digest` against the `X-Broker-Key` header — there is never an
  allow-all path. Transport is plain HTTP on the trusted LAN: the shared secret
  stops casual other-device access but a LAN sniffer still sees tokens in transit
  (accepted; TLS-to-broker is out of scope).

- **Stdlib-only — no `httpx`/`asyncio`/subprocess.** `ThreadingHTTPServer`; the
  SIGINT/SIGTERM handler runs `server.shutdown()` **off** the serving thread
  (calling it inline would deadlock) then `server_close()` releases the socket.
  Binds `0.0.0.0:8080` (override `CLAWDMETER_PORT`). Tests: `daemon/test_token_broker.py`.

- **Editing the script changes nothing until restart.** Long-running process —
  `systemctl --user restart clawdmeter-broker` (Linux) or reload the LaunchAgent
  (macOS), or the old logic stays resident. The unit's `ExecStart` points at the
  venv python + `daemon/token_broker.py` (absolute) — repoint when switching
  between the worktree and the main checkout. Secrets live under
  `~/.config/clawdmeter/` (`broker.env` = `CLAWDMETER_BROKER_KEY`,
  `claude_setup_token`), mode 600, never committed.

- **Never inject an arbitrary secret into a launchd plist with `sed`.** A `BROKER_KEY`
  containing `&`, `|`, `\`, `<`, or `>` silently corrupts the plist: `&` expands to the
  matched text, `|` closes the `s|||` expression early, `<>` breaks XML. Use python with
  `xml.sax.saxutils.escape`: `python3 -c "import os,pathlib,xml.sax.saxutils as x; p=pathlib.Path(os.environ['PLIST_DEST']); p.write_text(p.read_text().replace('__KEY__', x.escape(os.environ['KEY'])))"` — pass the secret via env, not shell interpolation. The systemd/EnvironmentFile path avoids this entirely (Linux install).
  → see `docs/decisions/2026-06-04-plist-secret-injection.md`

- **Use `read -s` (silent mode) when prompting for secrets in install scripts.**
  Plain `read -r -p` echoes the typed characters to the terminal (visible over the
  shoulder) and captures them in shell history (`~/.bash_history`). Use `read -r -s -p
  "prompt: " VAR; echo` — the trailing `echo` adds the newline the silent read suppresses.
  Validate non-empty immediately after: `[ -n "$VAR" ] || { echo "must not be empty" >&2; exit 1; }`.

- **On cutover/rename: run `git grep <old-name>` repo-wide before closing the task.**
  A diff-scoped review (including all /xverify arms) only flags issues in the changed
  lines — stale references in *unchanged* files are structurally invisible to it. At the
  Phase-4 cutover, the README's `./install*.sh` commands pointed at dead BLE-era root
  scripts that still referenced the deleted `claude_usage_daemon.py`; caught only at
  commit time via `git grep`. Make repo-wide grep a mandatory last step for any rename or
  removal, separate from the /xverify pass.

## Related decisions

- `2026-06-04-token-broker-self-sufficient` (plan) — the device-direct + broker design.
- `2026-06-04-plist-secret-injection` — why `sed` corrupts secrets with `&|<>\` and how to fix with python+xml.sax.saxutils.
- `2026-06-02-api-oauth-usage-rate-limit` — the 300s `/api/oauth/usage` constraint;
  now **moot for the device's path** (it scrapes `/v1/messages` response headers,
  not `/api/oauth/usage`). Still applies to anything that hits `/api/oauth/usage`.
