---
date: 2026-06-04
module: daemon, firmware
status: accepted
supersedes: 2026-06-02-wifi-transport.md "Rejected: on-device OAuth (self-sufficiency)"
tags: [self-sufficiency, oauth, token-broker, daemon, tls, firmware, usage]
---

# Plan: make the device (mostly) self-sufficient via a token-broker daemon

## Progress

Build in units, not one go (see [Implementation phases](#implementation-phases)).
Tick as each lands with its verification.

- [ ] **1. TLS spike** (gate, throwaway) — `WiFiClientSecure` to both hosts,
      Claude header-scrape, Codex GET, forced-`401`, on real ESP32-S3
- [ ] **2. Broker** (no hardware) — `/tokens` + `/healthz` + `409` + `X-Broker-Key`;
      `curl`-verified against real laptop creds
- [ ] **3a. Firmware data path** — TLS provider clients + on-device 14-key synthesis,
      **hardcoded** tokens; real usage renders (verify via `screenshot.sh`)
- [ ] **3b. Firmware token plumbing** — `token_store` (NVS) + `/tokens` fetch +
      `401`-refetch / back-off / per-source health states
- [ ] **4. Docs + lockstep cutover** — doc sweep + combined daemon+firmware flash

Ordering: 1 before 3 (gate); 2 before 3b; 4 last. 2 and 3a are independent
(parallelizable). Lands as ~3 reviewable changes (broker; 3a; 3b+cutover) plus
the spike as a pre-step.

## Goal

Stop the daemon from polling usage. The **device** fetches usage directly from
both providers; the daemon is demoted to a small LAN **token broker** that hands
the device a fresh credential only when its local one expires. The laptop can be
off the rest of the time.

This reverses the 2026-06-02 "Rejected: on-device OAuth" decision — that
rejection assumed there was *no static-key escape* and that on-device OAuth
refresh + a dedicated token lineage + flash-encrypted secrets were mandatory.
Empirical testing (below) found a static escape for Claude and a forgiving
refresh window for Codex, so the device-direct path is now cheap.

## Verified findings (2026-06-04, this machine)

Confirmed by live calls with the user's real credentials:

- **Claude long-lived token works for usage via header-scrape.** `claude
  setup-token` mints a **1-year** OAuth token (`sk-ant-oat01-…`,
  `CLAUDE_CODE_OAUTH_TOKEN`). It is **inference-scoped only**:
  - `GET /api/oauth/usage` → **403** `OAuth token does not meet scope
    requirement user:profile`. (So the structured-JSON endpoint is unusable.)
  - `POST https://api.anthropic.com/v1/messages` (1-token Haiku call) → **200**,
    and the response headers carry everything we need:
    `anthropic-ratelimit-unified-5h-utilization` / `-5h-reset`,
    `anthropic-ratelimit-unified-7d-utilization` / `-7d-reset`, plus
    `-overage-*` and `-status` fields. This is the *original* (pre-`9ac470b`)
    header-scrape approach, and `/v1/messages` has **generous** rate limits (the
    300s constraint on `/api/oauth/usage` does not apply).
  - Cost: 1 inference token per poll (negligible). Watch: from 2026-06-15,
    Agent-SDK / `claude -p` usage draws from a separate monthly credit; whether a
    raw `/v1/messages` OAuth call counts against it is unconfirmed.
- **Codex has no long-lived token, by design.** openai/codex #2636 (closed
  2025-11-27): *"We're unlikely to add long-lived tokens for ChatGPT. We've
  invested in making the token refresh logic more robust."* The usable path is
  the subscription OAuth `access_token` from `~/.codex/auth.json`:
  - `GET https://chatgpt.com/backend-api/wham/usage` with
    `Authorization: Bearer <access_token>` + `ChatGPT-Account-Id: <account_id>`
    → **200**, JSON: `rate_limit.primary_window.used_percent` (5h),
    `rate_limit.secondary_window.used_percent` (7d), each with
    `reset_after_seconds` (relative int — match the daemon's existing
    `poll_codex`/`.claude/rules/daemon.md` mapping; there is no `reset_at`).
  - The `access_token` is a JWT that lives **~10 days** (observed 9d remaining).
    `id_token` is short-lived and irrelevant; the daemon uses `access_token`.

## Architecture

```
Device ──TLS──> api.anthropic.com  POST /v1/messages          (scrape unified rate-limit headers)
Device ──TLS──> chatgpt.com        GET  /backend-api/wham/usage
Device ──HTTP─> laptop daemon      GET  /tokens                (first boot, and on provider 401)
```

The device is **stateless by design**: the only thing baked into firmware is
bootstrap config (WiFi creds, broker `.local` name, `X-Broker-Key`) — never a
token. It pulls both tokens from the broker in one `GET /tokens` round-trip,
caches them in NVS, and polls the two provider APIs directly on its own cadence.
It contacts the daemon **only** to (re)fetch tokens — Claude ~yearly, Codex
~weekly. (One combined endpoint, not two, so cold boot is a single broker call —
chaining blocking calls in one tick freezes the LVGL loop, per
`.claude/rules/networking.md`.)

### Device state machine (no expiry math)

The device never decodes token lifetimes. A provider `401`/`403` is the single
refetch trigger — uniform for both providers, self-healing against expiry,
rotation, and revocation:

```
boot:        creds = NVS.get()  ||  broker.GET(/tokens)
poll:        call provider API with its token
on 401/403:  creds = broker.GET(/tokens); NVS.put(creds); retry once
on 429/5xx:  leave token unchanged (not an auth failure) → show last data as stale
```

**Failure handling (the cases the happy path skips):**
- **Broker unreachable** (boot with empty NVS, or on a `401` refetch): do *not*
  hammer. Back off (reuse the existing fetch-interval cadence) and surface a new
  health state rather than retrying every tick. A `401` whose refetch fails must
  not re-trigger the blocking broker call on the very next poll.
- **Broker returns "can't supply a usable token"** (`409`, see broker contract):
  treat as a terminal-until-user-acts state — stop refetching that provider,
  show the needs-action state, keep polling the other provider.
- **New health state.** Extend the existing daemon-health model (`net.h`
  `daemon_health_t`, today keyed on the single `/usage` fetch) into a per-source
  notion that also covers "no token yet" / "broker offline" / "needs action".
  The WiFi-page labels and `ui_update_wifi_status()` follow from that. This is
  part of the `net.cpp` restructure below, not a bolt-on.

Persistent device footprint = **one opaque record per provider in NVS**: `token`
(+ `account_id` for Codex). No clocks, no scheduled refresh, no `expires_at` on
the MCU.

## Constraint (user, 2026-06-04)

**The daemon never performs OAuth refresh itself.** It only (a) reads the
tokens already on the laptop (kept current by normal CLI use), and (b) when a
token is stale, invokes the provider CLI to mint/refresh it. No `grant_type=
refresh_token` HTTP exchange is implemented in the daemon.

## Daemon design (token broker)

Replaces the polling loop (`/usage` and the poller go away). Same transport
posture as today (mDNS, LAN bind), gated on `X-Broker-Key`. Endpoints:

- `GET /tokens` → both providers in one response
- `GET /healthz` (keep the existing name)

**Contract — status code is the device's whole decision tree:**
- `200` → every provider key present carries a token that *works right now*.
  Shape: `{ "claude": { "token" }, "codex": { "token", "account_id" } }`.
- `409` → at least one provider can't be supplied; that provider's value is
  `{ "needs_action": "<human string>" }` (no `token`). Any sibling provider that
  *is* usable still appears with its token. The device caches usable tokens,
  marks the `409` provider needs-action, and stops refetching it until the next
  successful `/tokens`.

`expires_at` is omitted on purpose — the broker guarantees freshness before
responding, and the device's `401`-refetch is the freshness mechanism. The
device never parses `needs_action` beyond "this provider is unusable."

Per-provider behaviour — read local, CLI-assisted on staleness:

| | Claude | Codex |
|---|---|---|
| Local source | stored `setup-token` string (captured from the user's `claude setup-token` run) | `~/.codex/auth.json` → `tokens.access_token` + `tokens.account_id` |
| Lifetime | ~1 year | ~10 days |
| If valid | return it (`200`) | return it (`200`) |
| If stale/expired | **cannot auto-mint** — `claude setup-token` is interactive-only (verified: no flags, opens OAuth browser flow) → `409` + `needs_action:"run claude setup-token"` | `codex login status`; if stale, `codex exec "ok"` forces a CLI refresh of `auth.json`, re-read and return `200`. If refresh fails (`rt_` dead, or `codex` errors) → `409` + `needs_action:"run codex login"` |
| Refresh cost | 0 | one trivial model turn (~weekly) |

Notes:
- The broker does **not** implement OAuth — Codex refresh is delegated to the
  Codex CLI subprocess; Claude re-mint is a manual yearly user action the broker
  only signals via `409`.
- Sync handlers fit the existing `ThreadingHTTPServer`/`BaseHTTPRequestHandler`;
  the `httpx`/`asyncio` poll machinery is deleted, entrypoint becomes
  `serve_forever()`.
- The unit needs `claude` + `codex` on `PATH` (CLI invocation) and a path to the
  stored Claude `setup-token` — see Config + install below; the `ExecStart`/venv
  foot-gun from `CLAUDE.md` still applies.

## Firmware design (coupled work)

This is a **rewrite of `net.cpp`**, not an extension. Today it's one plain
`HTTPClient`, one body buffer, one `do_fetch()` against a single `/usage` URL,
with `main.cpp`'s `parse_json()` doing all interpretation. The new shape: three
HTTP lifecycles (two TLS providers + one plain-HTTP broker), per-source state,
the `401`→refetch intercept, and the per-source health model above. Scope it as
a restructure up front.

- **TLS:** `WiFiClientSecure` for `api.anthropic.com` and `chatgpt.com`. The
  Arduino-core default CA set is stripped — enable the full ESP-IDF cert bundle
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`) or pin the two roots via `setCACert()`.
  Never `setInsecure()`. Pair `setConnectTimeout()` **and** `setTimeout()` on
  every client (per `.claude/rules/networking.md`).
- **Claude client:** `POST /v1/messages` (`max_tokens:1`), read the
  `anthropic-ratelimit-unified-5h/7d-utilization` + `-reset` *response headers*
  (new use of `HTTPClient::header()` — collect them).
- **Codex client:** `GET wham/usage`, parse
  `rate_limit.primary_window`/`secondary_window` `used_percent` +
  `reset_after_seconds`.
- **Wire mapping:** synthesize the existing 14-key `UsageData` on-device from the
  two upstream shapes so `ui.cpp` is untouched. (Accepted tradeoff: parsing moves
  MCU-side, so an upstream shape change now needs a reflash, not a daemon
  restart.)
- **Token store:** isolate NVS (`Preferences`) behind a small `token_store`
  module — first persistent-storage use in the firmware; keep it out of
  `net.cpp`. One opaque record per provider (`token`, +`account_id` for Codex).
- **No manual provisioning** — pull on first boot (empty NVS), re-pull on `401`.

## Provisioning / lifecycle

1. **Laptop setup (once):** `claude setup-token` → store the string where the
   broker reads it. Normal `codex` use keeps `auth.json` fresh.
2. **Device first boot:** empty NVS → `GET /tokens` → cache and run. No
   per-device flashing step.
3. **Steady state:** device polls providers directly; never needs the laptop.
4. **Codex token dies (~weekly):** provider `401` → `GET /tokens`; broker
   refreshes via the CLI and returns a fresh token. Laptop must be reachable then.
5. **Claude token dies (~yearly):** provider `401` → `GET /tokens` →
   `409`/needs-action (stored `setup-token` expired); user re-runs
   `claude setup-token`.

## Security

This is the load-bearing change: the daemon now serves **credentials**, not
low-sensitivity usage percentages. Today it is unauthenticated on the LAN —
acceptable for usage %, **not** for bearer tokens that can spend the user's
Claude/Codex quota.

- Add a pre-shared secret (`X-Broker-Key`) the device sends; broker rejects
  requests without it. (Decision Q2.)
- Tokens at rest on the device live in plain NVS (no flash encryption / secure
  boot). Accepted risk for a personal desk gadget (decision Q4), but it is a
  real theft/dump exposure if the device is lost.
- Transport remains plain HTTP on the trusted LAN; tokens traverse it in clear.
  A LAN attacker who can sniff also sees them. Shared-secret does not fix
  sniffing; only TLS-to-broker would, which we are explicitly avoiding for
  simplicity.

## Decisions locked (user, 2026-06-04)

1. **Claude data path = setup-token + `/v1/messages` header-scrape.** Device
   holds the 1-year token; laptop needed ~yearly. Accept the 1 inference
   token/poll cost. (`/api/oauth/usage` rejected — its hourly token would tie
   the device to an always-on laptop.)
2. **Broker auth = `X-Broker-Key` shared secret.** Device sends it on every
   `/tokens` request; broker rejects otherwise. Accepted limitation: plain HTTP
   means a LAN sniffer still sees tokens in transit — the secret only stops
   casual other-device access, not passive capture. TLS-to-broker remains out of
   scope.
3. **Codex refresh = auto `codex exec "ok"`.** On a stale token the broker shells
   to the CLI to refresh `auth.json`, then serves it. Accept the ~weekly trivial
   billable turn; falls back to `needs_action:"run codex login"` if `rt_` is dead.
4. **Secret-at-rest = plain NVS.** No flash encryption / secure boot. Accepted
   risk: a flash dump yields spendable tokens (Claude's valid ~1 year);
   mitigated only by the user's willingness to re-provision / revoke.

## Config + install

- **Firmware** (`net_config.h` / `.example.h`): add `BROKER_KEY`; the existing
  daemon host/port are reused for the broker.
- **Daemon**: `X-Broker-Key` value + path to the stored Claude `setup-token`
  (env or config file). Update `install.sh` / `install-mac.sh` to prompt/store
  both and to put `claude`+`codex` on the unit's `PATH`.

## Implementation phases

Each unit is independently verifiable; don't merge them as one big-bang change.

1. **TLS validation spike** *(gate — throwaway code)*. Before any `net.cpp`
   work — `AGENTS.md` "validate API capabilities before depending on them";
   mirrors the mDNS spike of the 2026-06-02 plan. On real ESP32-S3:
   `WiFiClientSecure` + CA bundle to both hosts, the Claude header-scrape, the
   Codex GET, and a forced-`401`. Confirm header collection + cert handling.
   **If it fails, stop and rethink the firmware approach — do not start the
   rewrite.**
2. **Broker** *(no hardware)*. Swap the poller for `/tokens` + `/healthz` +
   the `409`/needs-action paths, gated on `X-Broker-Key`. Verify end-to-end with
   `curl` against the real laptop creds.
3. **Firmware** — split to de-risk:
   - 3a. TLS provider clients + on-device 14-key synthesis, with **hardcoded**
     tokens. Goal: real usage renders on screen (QA with `screenshot.sh`). Proves
     the data path before any token plumbing.
   - 3b. `token_store` (NVS) + `/tokens` fetch + the `401`-refetch / back-off /
     per-source health states.
4. **Docs + lockstep cutover**. The doc sweep (below) plus the `/usage`→broker
   switch. Single-device project, so this is one combined daemon + firmware
   flash — no staged rollout, no need to keep `/usage` alive.

Ordering: 1 before 3 (gate); 2 before 3b; 4 last. 2 and 3a are independent and
can run in parallel. Net: ~3 reviewable changes (broker; 3a; 3b + cutover) plus
the spike as a pre-step.

## Docs to update (part of the change, not afterthoughts)

These currently describe the old poller and will be wrong/misleading:

- `.claude/rules/daemon.md` — "poll must stay host-side" bullet, the "don't
  reintroduce `/v1/messages` header-scrape" bullet (this plan deliberately does),
  and the `/usage` wire-format/endpoint contract.
- `docs/plans/2026-06-02-wifi-transport.md` — "Rejected: on-device OAuth" /
  "Drop self-sufficiency" (now superseded by this plan).
- `docs/decisions/2026-06-02-api-oauth-usage-rate-limit.md` — note the 300s
  constraint is moot for the `/v1/messages` path.
- `README.md`, `AGENTS.md`, `CLAUDE.md` — `/usage` poller / HTTP protocol table.
- `daemon/install.sh`, `daemon/install-mac.sh` — printed `/usage` URL.
- `firmware/tools/mdns_spike/include/spike_config.h` — `HTTP_TEST_PATH "/usage"`.

## Related

- `docs/plans/2026-06-02-wifi-transport.md` — the WiFi+daemon design this revises.
- `docs/decisions/2026-06-02-api-oauth-usage-rate-limit.md` — 300s rationale.
- openai/codex #2636 — OpenAI's "no long-lived tokens for ChatGPT" stance.
