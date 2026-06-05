---
paths:
  - "firmware/src/net.cpp"
  - "firmware/src/net.h"
  - "firmware/tools/mdns_spike/**"
---

# Networking rules

- **`WiFi.hostByName()` blocks the Arduino loop for the full DNS timeout** (calls
  `lwip_getaddrinfo` synchronously; ~10–15 s for a `.local` name that never responds,
  freezing `lv_timer_handler()`). Only the **broker** host is resolved this way —
  `resolve_broker()` is called only when a token fetch is due and caches the IP on
  success in `s_broker_resolved` (cleared on WiFi drop) — so a *successful* resolve runs
  once per association and a *failed* one retries only at the `FETCH_INTERVAL_MS` fetch
  cadence, **never per-tick**. Provider APIs
  (`api.anthropic.com`, `chatgpt.com`) are resolved by the TLS client. → see
  `docs/decisions/2026-06-03-wifi-http-transport-gotchas.md`

- **`http.setConnectTimeout(N)` ≠ full request timeout** — it caps the TCP connect phase
  only; the response-read phase is `_tcpTimeout` (default 5000 ms), set by the separate
  `http.setTimeout(N)`. A daemon that accepts then stalls before sending headers blocks
  `net_tick()` for `connectTimeout + tcpTimeout` ≈ 8 s without both. Always pair them.
  → see `docs/decisions/2026-06-03-wifi-http-transport-gotchas.md`

- **The broker fetch is gated so a resolve/connect failure can't storm.** `net_tick()`
  sets `s_broker_attempted`/`s_broker_last_attempt = millis()` *before* calling
  `fetch_tokens()`, so even a failure that returns before sending a byte arms the
  `FETCH_INTERVAL_MS` throttle. A provider `401/403` sets `s_need_fetch[prov]` *and*
  `s_tok[prov] = TOK_MISSING` (so the rejected token isn't re-POSTed during the window).
  Clear `s_broker_resolved` and re-arm `s_broker_attempted` on WiFi drop/reconnect so
  the next association re-resolves and refetches.

- **`WL_CONNECT_FAILED` and `WL_NO_SSID_AVAIL` are terminal** — the stack stops retrying
  and `WiFi.status()` stays there until `WiFi.begin()` is called again. Handle both in
  `net_tick()` with a time-gated retry (~15 s) or the device shows "Connecting…" forever
  on bad credentials.

- **Wall-clock time comes from NTP, kicked off on each WiFi association** via
  `configTzTime(NTP_TZ, NTP_SERVER)` on the connect transition (**non-blocking**, unlike
  `hostByName()`). There is **no RTC**, and SNTP lands a few seconds *after* associating.
  So the WiFi page's "Updated: HH:MM" does **not** store an epoch at fetch time; it
  reconstructs the instant at render as `time(nullptr) - age_ms` (age from the monotonic
  `net_last_update_ms()`) — stable across repaints, self-heals once the clock syncs. The
  render guards on `time(nullptr) >= 1700000000` (a "clock is plausibly real" floor;
  unsynced newlib starts at 1970) and shows an em-dash until both a fetch and a synced
  clock exist. `NTP_SERVER`/`NTP_TZ` in `net_config.h` (gitignored) with `#ifndef`
  fallbacks in `net.cpp`.

- **The usage-health verdict is per-provider and event-driven, not ticked.**
  `net_provider_health(prov)` returns one verdict per provider (the WiFi page shows a
  `Claude:` and a `Codex:` row), in priority order: `USAGE_OFFLINE` → `USAGE_NEEDS_ACTION`
  → `USAGE_BROKER_DOWN` → `USAGE_NO_TOKEN` → then freshness states
  `USAGE_NO_DATA`/`USAGE_LIVE`/`USAGE_STALE` (off elapsed time since *that provider's*
  last good fetch — `s_last_update_ms[PROVIDER_COUNT]`; `USAGE_STALE_MS` ≈2.75×
  `FETCH_INTERVAL_MS`). Auth/token states outrank freshness because they're actionable;
  providers are independent, so one needing re-auth never changes the other's row.
  Several verdicts are elapsed-time transitions no fetch announces, so `loop()` polls
  both every iteration but repaints the WiFi page **only on a verdict/net-state change**
  (the `last_health[]` latch in `main.cpp`) **plus** one repaint per fresh GET to advance
  the `Updated` line. Don't reintroduce a fixed-interval WiFi refresh. (The Signal/RSSI
  row was dropped for the two provider rows; `net_get_rssi()` still exists as a
  diagnostic but isn't rendered.)

- **WiFi runs in `WIFI_PS_MAX_MODEM` (deepest modem-sleep), set once in `net_init()`
  after `WiFi.begin()`.** The device is pull-only (no inbound traffic to wait on), so the
  radio skips most DTIM beacons to save battery — the largest single draw while
  displaying. This is **not** the Arduino default (`WIFI_PS_MIN_MODEM`); don't drop the
  `setSleep()` call. A button-forced `net_request_refresh()` still TXes immediately (TX
  wakes the radio regardless of PS mode), so the deeper sleep only adds latency to
  unsolicited inbound packets, of which there are none.

## Device-side provider usage mapping

The device fetches usage **directly** from both providers (moved off the host at the
2026-06-04 cutover). Each `fetch_*()` writes its mapped result straight into
`s_usage[prov]` (a `ProviderUsage`); `publish_if_changed()` hands the array to `main.cpp`
via `net_get_usage()` — no wire-JSON round-trip (`synthesize_payload`/`parse_json` were
deleted in the provider-table refactor). Tradeoff: an upstream shape change now needs a
reflash, not a daemon restart. Fetchers dispatch through `PROVIDERS[]` (broker key + fn
pointer); `ui.cpp`/`main.cpp` loop over `PROVIDER_COUNT`, so adding a provider is a fetch
fn + table rows (see `data.h`), not edits per call site.

- **Claude** (`fetch_claude`): `POST https://api.anthropic.com/v1/messages` (`max_tokens:1`
  Haiku, `anthropic-beta: oauth-2025-04-20`), then scrape **response headers**:
  `anthropic-ratelimit-unified-5h-utilization`/`-7d-utilization` are a **0–1 fraction**
  (×100 → `session_pct`/`weekly_pct`); `-5h-reset`/`-7d-reset` are **absolute epoch
  seconds** (`epoch_to_mins`, clock-guarded → `*_reset_mins`); `-status` ≠ `allowed` (or
  `session_pct ≥ 100`) → `status = "limited"`. The setup-token is inference-scoped so
  `/api/oauth/usage` 403s — `/v1/messages` is the only usable path (generous limits; the
  300s rule doesn't apply).
- **Codex** (`fetch_codex`): `GET https://chatgpt.com/backend-api/wham/usage`
  (`Authorization: Bearer <access_token>` + `ChatGPT-Account-Id: <account_id>`). JSON
  `rate_limit.primary_window` → 5h, `secondary_window` → 7d; `used_percent` is **already
  0–100**; `reset_after_seconds` is **relative** (÷60 → minutes, no epoch math);
  `!allowed`/`limit_reached`/`session_pct ≥ 100` → `status = "limited"`. A `200` without
  a `rate_limit` object is a failure, not healthy-empty.
- **Tokens** come from the broker, cached in NVS by `token_store` (see
  `.claude/rules/daemon.md`), reached through the per-provider `s_cred[]` table.
  `ProviderUsage.present` is always `true` — a provider needing re-auth surfaces via the
  WiFi-page health verdict, not by blanking its panel.

## Related decisions

- `2026-06-03-wifi-http-transport-gotchas` — confirmed blocking/timeout constraints.
- `2026-06-04-token-broker-self-sufficient` (plan) — device-direct usage + token broker.
