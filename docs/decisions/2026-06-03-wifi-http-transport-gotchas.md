---
date: 2026-06-03
module: firmware/src/net.cpp
tags: [wifi, mdns, httpclient, blocking, timeout, WL_CONNECT_FAILED, lwip, arduino-esp32]
---

# ESP32 Arduino WiFi+HTTPClient: confirmed blocking and timeout constraints

## Context
Implementing `net.{h,cpp}` for the BLE→WiFi/HTTP migration. `net_tick()` runs every
loop alongside `lv_timer_handler()`, so any blocking call freezes the display. Four
constraints were missed initially and caught during xverify shell verification — each
**confirmed by grepping the platformio package sources, not inferred.** The actionable
directives live in `networking.md`; this records the evidence behind them.

## 1. `WiFi.hostByName()` is fully synchronous
`NetworkManager.cpp` calls `lwip_getaddrinfo()` blocking, with no timeout param at the
Arduino wrapper layer. For a `.local` name with no responder it blocks for the lwip DNS
retry timeout (~10–15 s).

**Mitigation:** `resolve_broker()` resolves lazily from `fetch_tokens()`, early-returns
when `s_broker_resolved` is set, and only sets that flag on success — so the blocking
lookup runs at most once per association, and a *failed* resolve retries only on the next
broker-due fetch (gated by `FETCH_INTERVAL_MS`), never per tick. `net_tick()` clears the
flag on WiFi drop. Only the broker host is resolved this way; provider APIs are resolved
by the TLS client.

## 2. `setConnectTimeout` and `setTimeout` are independent
From `HTTPClient.h`: `setConnectTimeout(N)` sets `_connectTimeout` (the `connect()` call);
`setTimeout(N)` sets `_tcpTimeout` (the response-read phase, default 5000 ms). Both must
be set to bound a daemon that accepts the TCP connection but stalls before sending headers
— otherwise connect-bound 3 s + read-bound 5 s = 8 s worst-case freeze.

## 3. A failed broker fetch must still arm the throttle (no retry storm)
The original fix cleared two gate flags on the DNS-fail early-return; clearing only one
left the next loop iteration to immediately re-enter the blocking DNS call (caught by
Codex). The cutover replaced clear-both with **arm-before-call**: `net_tick()` sets
`s_broker_attempted`/`s_broker_last_attempt` *before* calling `fetch_tokens()`, so even a
failure that returns before sending a byte arms the throttle. Same invariant, now holding
by construction rather than by remembering to reset every gate variable.

## 4. `WL_CONNECT_FAILED` / `WL_NO_SSID_AVAIL` require explicit `WiFi.begin()` retry
The driver stops association attempts after either status and `WiFi.status()` stays there;
Arduino's auto-reconnect only fires on `WL_DISCONNECTED`. Without explicit retry the device
shows "Connecting…" until power-cycled. Fixed with a 15 s debounced `WiFi.begin()` in the
`NET_CONNECTING`/`NET_DISCONNECTED` branch.

## Prevention
- Always pair `setConnectTimeout` + `setTimeout`.
- Arm the fetch throttle *before* a blocking resolve/connect, not after success.
- Handle `WL_CONNECT_FAILED` / `WL_NO_SSID_AVAIL` explicitly — `WL_DISCONNECTED` alone
  doesn't cover terminal failures.

## Related
- `.claude/rules/networking.md`; `docs/plans/2026-06-02-wifi-transport.md` (design +
  mDNS spike at `firmware/tools/mdns_spike/`).
