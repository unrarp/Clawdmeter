---
paths:
  - "firmware/src/net.cpp"
  - "firmware/src/net.h"
  - "firmware/tools/mdns_spike/**"
---

# Networking rules

- **`WiFi.hostByName()` blocks the Arduino loop for the full DNS timeout.** It calls `lwip_getaddrinfo` synchronously — no async path, no configurable timeout at the Arduino layer. For a `.local` mDNS name that never responds this can block for 10–15 s, freezing `lv_timer_handler()` and all display updates. Only the **broker** host is resolved this way — `resolve_broker()` caches the IP in `s_broker_resolved` so it runs at most once per association (cleared on WiFi drop); never per-tick or per failed fetch. The provider APIs (`api.anthropic.com`, `chatgpt.com`) are public names the TLS client resolves itself. → see `docs/decisions/2026-06-03-wifi-http-transport-gotchas.md`

- **`http.setConnectTimeout(N)` ≠ full request timeout.** It caps the TCP SYN-ACK phase only. The response-read phase is governed by `_tcpTimeout` (`HTTPCLIENT_DEFAULT_TCP_TIMEOUT = 5000 ms`), which is a separate setter: `http.setTimeout(N)`. A daemon that accepts the connection but stalls sending headers blocks `net_tick()` for up to `connectTimeout + tcpTimeout` ≈ 8 s without both setters. Always pair them. → see `docs/decisions/2026-06-03-wifi-http-transport-gotchas.md`

- **The broker fetch is gated so a resolve/connect failure can't storm.** `net_tick()` sets `s_broker_attempted = true` and `s_broker_last_attempt = millis()` *before* calling `fetch_tokens()`, so even a failure that returns before sending a byte (DNS resolve fail, `begin()` fail, non-`200`/`409`) still arms the `FETCH_INTERVAL_MS` throttle — the next `/tokens` attempt waits the full interval rather than re-entering the blocking resolver next loop. A provider `401/403` sets `s_need_fetch[prov]` *and* `s_tok[prov] = TOK_MISSING` (so the rejected token isn't re-POSTed during the window); the same throttle bounds recovery to one broker call + one provider call per interval. Clear `s_broker_resolved` on WiFi drop and re-arm `s_broker_attempted` on reconnect so the next association re-resolves and refetches promptly.

- **`WL_CONNECT_FAILED` and `WL_NO_SSID_AVAIL` are terminal WiFi states.** The ESP32 WiFi stack stops retrying after either; `WiFi.status()` stays at the failure code until `WiFi.begin()` is explicitly called again. Handle both in `net_tick()` with a time-gated retry (e.g. 15 s) or the device silently shows "Connecting…" forever on bad credentials.

- **Wall-clock time comes from NTP, kicked off on each WiFi association.** `net_tick()` calls `configTzTime(NTP_TZ, NTP_SERVER)` on the connect transition — this is **non-blocking** (background SNTP), so unlike `hostByName()` it's safe in the tick. There is **no RTC**, and SNTP lands a few seconds *after* associating, so an early GET can predate the clock being set. The WiFi page's "Updated: HH:MM" line therefore does **not** store an epoch at fetch time; it reconstructs the fetch instant at render as `time(nullptr) - age_ms` (age from the always-valid monotonic `net_last_update_ms()`), which is stable across repaints and self-heals once the clock syncs — even for a GET that landed pre-sync. The render guards on `time(nullptr) >= 1700000000` (≈2023-11-14, a "clock is plausibly real" floor; unsynced newlib starts at 1970) and shows an em-dash until both a fetch and a synced clock exist. `NTP_SERVER`/`NTP_TZ` live in `net_config.h` (gitignored) with `#ifndef` fallbacks in `net.cpp`.

- **The usage-health verdict is per-provider and event-driven, not ticked.** `net_provider_health(prov)` returns one verdict **per provider** (the WiFi page shows a `Claude:` row and a `Codex:` row), in priority order: `USAGE_OFFLINE` (WiFi down) → `USAGE_NEEDS_ACTION` (that provider's token needs user re-auth) → `USAGE_BROKER_DOWN` (it needs a token but the broker didn't reply) → `USAGE_NO_TOKEN` (online, fetching its token) → then the data-freshness states `USAGE_NO_DATA`/`USAGE_LIVE`/`USAGE_STALE` (off elapsed time since *that provider's* last good fetch — `s_last_update_ms[PROVIDER_COUNT]`; `USAGE_STALE_MS` ~2.75× `FETCH_INTERVAL_MS`). It's named "usage", not "daemon": post-cutover the device fetches usage **directly** from the providers and the broker is only hit to (re)fetch tokens, so each row (green "live" = a fetch landed within the staleness window) tracks that provider's data freshness + token/auth state, not a live host connection. Auth/token states outrank freshness because they're actionable and explain missing data; providers are independent, so one needing re-auth never changes the other's row. Because several verdicts are elapsed-time transitions no fetch announces, `loop()` polls both every iteration but repaints the WiFi page **only on a verdict/net-state change** — the `last_claude_health`/`last_codex_health` latches in `main.cpp` — **plus** one repaint per fresh GET to advance the `Updated` line (which then resyncs those latches). Don't reintroduce a fixed-interval WiFi refresh. The Signal/RSSI row was dropped to make room for the two provider rows (`net_get_rssi()` still exists as a diagnostic but is no longer rendered).

- **WiFi runs in `WIFI_PS_MAX_MODEM` (deepest modem-sleep), set once in `net_init()` after `WiFi.begin()`.** The device is pull-only — it fetches each provider's usage directly every `FETCH_INTERVAL_MS` (and the broker only on first boot / a provider `401`) and has no incoming traffic to wait on — so the radio can skip most DTIM beacons to save battery (the largest single draw while displaying). This is **not** the Arduino default (`WIFI_PS_MIN_MODEM`); don't drop the `setSleep()` call back to default. A button-forced `net_request_refresh()` still TXes immediately (TX wakes the radio regardless of PS mode), so the deeper sleep only adds latency to *unsolicited inbound* packets, of which there are none. The trade-off is purely battery-vs-nothing here.

## Device-side provider usage mapping

The device fetches usage **directly** from both providers (moved off the host at
the 2026-06-04 cutover) and `synthesize_payload()` in `net.cpp` builds the 14-key
wire JSON `main.cpp::parse_json()` consumes — so `ui.cpp` is unchanged. Tradeoff:
an upstream shape change now needs a reflash, not a daemon restart.

- **Claude** (`fetch_claude`): `POST https://api.anthropic.com/v1/messages`
  (`max_tokens:1` Haiku call, `anthropic-beta: oauth-2025-04-20`), then scrape the
  **response headers** — `anthropic-ratelimit-unified-5h-utilization` /
  `-7d-utilization` are a **0–1 fraction** (×100 → `s`/`w`); `-5h-reset` /
  `-7d-reset` are **absolute epoch seconds** (`epoch_to_mins`, guarded on a synced
  clock → `sr`/`wr`); `-status` ≠ `allowed` (or `s ≥ 100`) → `st = "limited"`.
  This is the original pre-Codex header-scrape; the setup-token is inference-scoped
  so `/api/oauth/usage` 403s — `/v1/messages` is the only usable path and has
  generous limits (the 300s `/api/oauth/usage` rule does not apply).
- **Codex** (`fetch_codex`): `GET https://chatgpt.com/backend-api/wham/usage`
  (`Authorization: Bearer <access_token>` + `ChatGPT-Account-Id: <account_id>`).
  JSON `rate_limit.primary_window` → 5h (`cs`/`csr`), `secondary_window` → 7d
  (`cw`/`cwr`); `used_percent` is **already 0–100**; `reset_after_seconds` is
  relative (÷60 → minutes, no epoch math); `!allowed` / `limit_reached` / `cs ≥ 100`
  → `cst = "limited"`. A `200` without a `rate_limit` object is a failure, not
  healthy-empty.
- **Tokens** come from the broker, cached in NVS by `token_store` (see
  `.claude/rules/daemon.md`); `sp`/`cp` (presence) are always `true` — a provider
  that needs re-auth is surfaced via the WiFi-page health verdict, not by blanking
  its panel.

## Related decisions

- `2026-06-03-wifi-http-transport-gotchas.md` — confirmed blocking/timeout constraints for ESP32 Arduino WiFi+HTTPClient, from xverify shell verification.
- `2026-06-04-token-broker-self-sufficient.md` (plan) — device-direct usage + token broker.
