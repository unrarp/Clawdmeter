---
date: 2026-06-03
module: firmware/src/net.cpp
tags: [wifi, mdns, httpclient, blocking, timeout, WL_CONNECT_FAILED, lwip, arduino-esp32]
---

# ESP32 Arduino WiFi+HTTPClient: confirmed blocking and timeout constraints

## Context

Implementing `net.{h,cpp}` for the BLE→WiFi/HTTP transport migration. `net_tick()` is called
every Arduino loop iteration alongside `lv_timer_handler()` — any blocking call freezes the
display. Five distinct constraints were missed in the initial implementation and caught during
xverify shell verification (confirmed by grepping platformio package sources, not inferred).

## Decision / Solution

Four constraints fixed in `firmware/src/net.cpp`. See `networking.md` rules for the directives.

## Why

### 1. `WiFi.hostByName()` is fully synchronous

Confirmed by grepping `~/.platformio/packages/`:

```
NetworkManager.cpp: lwip_getaddrinfo(hostname, ...)  // blocking, no timeout param at this layer
```

There is no non-blocking path and no configurable timeout in the Arduino ESP32 `WiFi.hostByName()`
wrapper. For a `.local` mDNS name with no responder the call blocks for the lwip DNS retry
timeout (~10–15 s by default).

**Mitigation in `net.cpp`:** `resolve_broker()` resolves lazily — it's called from
`fetch_tokens()` (not the `WL_CONNECTED` branch), early-returns when `s_broker_resolved` is set,
and only sets that flag on a *successful* `hostByName()`. So the blocking lookup runs at most
once per association once it succeeds, and a *failed* resolve retries only on the next
broker-due fetch (gated by `s_broker_last_attempt`/`FETCH_INTERVAL_MS`), never per loop tick.
`net_tick()` clears `s_broker_resolved` on WiFi drop so the next association re-resolves. Only
the **broker** host is resolved this way; the public provider APIs are resolved by the TLS client.

### 2. `setConnectTimeout` and `setTimeout` are independent

Confirmed by reading `HTTPClient.h`:

```cpp
#define HTTPCLIENT_DEFAULT_TCP_TIMEOUT (5000)  // ms, applies to response-read phase
```

`http.setConnectTimeout(N)` sets `_connectTimeout` (used in `client->connect()`).
`http.setTimeout(N)` sets `_tcpTimeout` (used in `client->setTimeout()` before `http.GET()`
awaits headers/body). Both must be set to bound a hung daemon that accepts the TCP connection
but delays sending headers. Without `setTimeout`, a connect-bound 3 s + read-bound 5 s = 8 s
worst-case stall.

### 3. A failed broker fetch must still arm the throttle (no retry storm)

The original fix cleared two gate flags (`s_fetch_requested`/`s_last_fetch_ms`) on the
DNS-fail early-return; clearing only one left the other set and the *next loop iteration*
immediately re-entered the blocking DNS call — a tight retry storm (caught by Codex on the
post-fix tree). The provider-table cutover replaced clear-both with **arm-before-call**:
`net_tick()` sets `s_broker_attempted = true` and `s_broker_last_attempt = millis()` *before*
calling `fetch_tokens()`, so even a failure that returns before sending a byte still arms the
`FETCH_INTERVAL_MS` throttle. The invariant is the same — a blocking resolve/connect failure
must not be re-enterable on the next loop — but it now holds by construction (the attempt is
recorded up front) rather than by remembering to reset every gate variable.

### 4. `WL_CONNECT_FAILED` and `WL_NO_SSID_AVAIL` require explicit `WiFi.begin()` retry

The ESP32 WiFi driver stops association attempts after either status. `WiFi.status()` stays at
the failure code; the Arduino reconnect logic only fires on `WL_DISCONNECTED`, not on these
terminal codes. Without explicit retry the device displays "Connecting…" until power-cycled.
Fixed by adding a 15 s debounced `WiFi.begin()` call in the `NET_CONNECTING`/`NET_DISCONNECTED`
branch of `net_tick()`.

## Prevention

- Pair `http.setConnectTimeout(N)` and `http.setTimeout(N)` whenever setting HTTP timeouts.
- Arm the fetch throttle *before* a blocking resolve/connect, not after success — record the
  attempt up front (`s_broker_attempted`/`s_broker_last_attempt`) so an early-return failure
  can't be re-entered on the next loop. (Supersedes the older "reset every gate variable on the
  early-return path" approach, which was fragile to a forgotten flag.)
- Check `WL_CONNECT_FAILED` and `WL_NO_SSID_AVAIL` explicitly in any WiFi state machine that
  has a "connecting" state — `WL_DISCONNECTED` alone does not cover terminal failures.

## Related

- `networking.md` — actionable rule bullets.
- `docs/plans/2026-06-02-wifi-transport.md` — the transport design and mDNS pre-validation
  approach (spike at `firmware/tools/mdns_spike/`).
