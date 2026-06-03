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

**Mitigation in `net.cpp`:** resolve once per WiFi-association transition (inside the
`WL_CONNECTED` branch of `net_tick()`), cache the `IPAddress`, and re-resolve only when a
connection error clears `s_daemon_ip_resolved`. Do not call from `do_fetch()` on every failed
attempt.

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

### 3. DNS-fail early-return must clear both fetch-gate conditions

`net_tick()` enters `do_fetch()` when `s_fetch_requested || interval_elapsed`. If
`resolve_daemon()` fails and only `s_last_fetch_ms` is updated (clearing `interval_elapsed`),
`s_fetch_requested` stays `true` and the *next loop iteration* immediately re-enters the
blocking DNS call — a tight retry storm. Clearing both on the early-return path is required.
This was caught by Codex reviewing the post-fix tree (the first applied fix had only cleared
`s_last_fetch_ms`).

### 4. `WL_CONNECT_FAILED` and `WL_NO_SSID_AVAIL` require explicit `WiFi.begin()` retry

The ESP32 WiFi driver stops association attempts after either status. `WiFi.status()` stays at
the failure code; the Arduino reconnect logic only fires on `WL_DISCONNECTED`, not on these
terminal codes. Without explicit retry the device displays "Connecting…" until power-cycled.
Fixed by adding a 15 s debounced `WiFi.begin()` call in the `NET_CONNECTING`/`NET_DISCONNECTED`
branch of `net_tick()`.

## Prevention

- Pair `http.setConnectTimeout(N)` and `http.setTimeout(N)` whenever setting HTTP timeouts.
- On any DNS/connection-error early-return in a fetch helper, grep for all gate-condition
  variables (`s_fetch_requested`, `s_last_fetch_ms`) and reset all of them.
- Check `WL_CONNECT_FAILED` and `WL_NO_SSID_AVAIL` explicitly in any WiFi state machine that
  has a "connecting" state — `WL_DISCONNECTED` alone does not cover terminal failures.

## Related

- `networking.md` — actionable rule bullets.
- `docs/plans/2026-06-02-wifi-transport.md` — the transport design and mDNS pre-validation
  approach (spike at `firmware/tools/mdns_spike/`).
