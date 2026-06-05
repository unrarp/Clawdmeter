---
paths:
  - "daemon/**"
---

# Daemon rules (token broker)

The daemon is **`daemon/token_broker.py`** — a credential broker, **not** a usage
poller. No polling, no usage field-mapping, no OAuth refresh. The device fetches usage
**directly** from the provider APIs and maps each response into a `ProviderUsage`
on-device (see `.claude/rules/networking.md`); the broker only hands over fresh tokens
when asked (first boot, or after a provider `401`). The old `claude_usage_daemon.py`
poller + `/usage` endpoint were deleted at the cutover (see
`docs/plans/2026-06-04-token-broker-self-sufficient.md`).

- **Endpoints + status code = the device's whole decision tree.** `GET /tokens`
  (requires the `X-Broker-Key` header) answers in one round-trip:
  - `200` `{"claude":{"token":...},"codex":{"token":...,"account_id":...}}` — every
    provider has a token that works *right now*.
  - `409` — at least one provider can't be supplied; its value is
    `{"needs_action":"<human string>"}` (no `token`), while usable siblings still carry
    their token. The device caches the usable ones, marks the `409` provider
    needs-action, and stops refetching it until the next successful `/tokens`.

  `GET /healthz` → `200 {"ok":true}` (no auth). `expires_at` is deliberately omitted —
  the broker guarantees freshness before answering; the device's `401`-refetch is the
  freshness mechanism.

- **The broker NEVER refreshes or mints tokens (locked constraint).** It only reads
  credentials already on the host (kept current by normal Claude Code / Codex CLI use)
  and decides usable-vs-needs-action. No `grant_type=refresh_token`, no CLI subprocess.
  `codex exec` *cannot* refresh ("auth token refresh is not supported in exec mode"),
  so pass-through is the only viable shape.

- **Token sourcing, per provider:**
  - **Claude** = a `claude setup-token` (inference-scoped, ~1 yr). Read from
    `CLAUDE_CODE_OAUTH_TOKEN` env, else `~/.config/clawdmeter/claude_setup_token`
    (override: `CLAWDMETER_CLAUDE_TOKEN_FILE`). Forwarded **verbatim** — a raw token,
    not a JSON blob. Expiry isn't locally readable, so **presence is the only gate**; a
    truly-expired token surfaces only as the device's retry still 401-ing.
  - **Codex** = `tokens.access_token` + `tokens.account_id` from `~/.codex/auth.json`.
    The access_token is a JWT (~10-day life); `jwt_exp()` decodes its `exp` locally and
    the broker serves it while >5 min from expiry (`CODEX_EXP_MARGIN`), else `409`.

- **`CLAWDMETER_BROKER_KEY` is required — the broker refuses to start without it** (it
  vends spendable credentials). As defense-in-depth, `_authorized()` independently
  returns `False` when the key is unset (even if the startup guard were bypassed) and
  otherwise compares with `hmac.compare_digest` against the `X-Broker-Key` header — no
  allow-all path. Transport is plain HTTP on the trusted LAN: the shared secret stops
  casual access but a LAN sniffer still sees tokens in transit (accepted; TLS-to-broker
  is out of scope).

- **Stdlib-only — no `httpx`/`asyncio`/subprocess.** `ThreadingHTTPServer`; the
  SIGINT/SIGTERM handler runs `server.shutdown()` **off** the serving thread (inline
  would deadlock) then `server_close()`. Binds `0.0.0.0:8080` (override
  `CLAWDMETER_PORT`). Tests: `daemon/test_token_broker.py`.

- **Editing the script changes nothing until restart** — `systemctl --user restart
  clawdmeter-broker` (Linux) or reload the LaunchAgent (macOS), or the old logic stays
  resident. The unit's `ExecStart` points at the venv python + `daemon/token_broker.py`
  (absolute) — repoint when switching between the worktree and the main checkout.
  Secrets under `~/.config/clawdmeter/` (`broker.env`, `claude_setup_token`), mode 600,
  never committed.

- **Never inject a secret into a launchd plist with `sed`.** A `BROKER_KEY` containing
  `&`, `|`, `\`, `<`, or `>` silently corrupts the plist. Use python with
  `xml.sax.saxutils.escape`, passing the secret via env not shell interpolation; the
  systemd/EnvironmentFile path (Linux) avoids this entirely. → see
  `docs/decisions/2026-06-04-plist-secret-injection.md`

- **Use `read -s` when prompting for secrets in install scripts.** Plain `read -r -p`
  echoes the secret and stores it in `~/.bash_history`. Use `read -r -s -p "prompt: "
  VAR; echo` (trailing `echo` adds the suppressed newline), then validate non-empty.

- **On cutover/rename: run `git grep <old-name>` repo-wide before closing the task.** A
  diff-scoped review (incl. /xverify) only flags changed lines — stale references in
  *unchanged* files are invisible to it. At the Phase-4 cutover the README's install
  commands still pointed at deleted BLE-era scripts; caught only by `git grep` at commit
  time. Make repo-wide grep a mandatory last step for any rename or removal.

## Related decisions

- `2026-06-04-token-broker-self-sufficient` (plan) — device-direct + broker design.
- `2026-06-04-plist-secret-injection` — why `sed` corrupts secrets, the python fix.
- `2026-06-02-api-oauth-usage-rate-limit` — the 300s `/api/oauth/usage` constraint, now
  moot for the device (it scrapes `/v1/messages` headers); still applies to anything
  hitting `/api/oauth/usage`.
