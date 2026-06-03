---
paths:
  - "firmware/src/net.cpp"
  - "firmware/src/net.h"
  - "firmware/tools/mdns_spike/**"
---

# Networking rules

- **`WiFi.hostByName()` blocks the Arduino loop for the full DNS timeout.** It calls `lwip_getaddrinfo` synchronously — no async path, no configurable timeout at the Arduino layer. For a `.local` mDNS name that never responds this can block for 10–15 s, freezing `lv_timer_handler()` and all display updates. Call it at most once per WiFi-association transition; never call it per-tick or on every failed fetch. → see `docs/decisions/2026-06-03-wifi-http-transport-gotchas.md`

- **`http.setConnectTimeout(N)` ≠ full request timeout.** It caps the TCP SYN-ACK phase only. The response-read phase is governed by `_tcpTimeout` (`HTTPCLIENT_DEFAULT_TCP_TIMEOUT = 5000 ms`), which is a separate setter: `http.setTimeout(N)`. A daemon that accepts the connection but stalls sending headers blocks `net_tick()` for up to `connectTimeout + tcpTimeout` ≈ 8 s without both setters. Always pair them. → see `docs/decisions/2026-06-03-wifi-http-transport-gotchas.md`

- **Clear *both* gate conditions on DNS-fail early-return in `do_fetch()`.** `net_tick()` triggers a fetch via `if (s_fetch_requested || interval_elapsed)`. Clearing only `s_last_fetch_ms` (resetting `interval_elapsed`) still leaves `s_fetch_requested = true`, so the next loop iteration immediately re-enters the blocking resolver — a DNS-retry storm. Set both `s_fetch_requested = false` and `s_last_fetch_ms = millis()` before returning from the DNS-fail path.

- **`WL_CONNECT_FAILED` and `WL_NO_SSID_AVAIL` are terminal WiFi states.** The ESP32 WiFi stack stops retrying after either; `WiFi.status()` stays at the failure code until `WiFi.begin()` is explicitly called again. Handle both in `net_tick()` with a time-gated retry (e.g. 15 s) or the device silently shows "Connecting…" forever on bad credentials.

## Related decisions

- `2026-06-03-wifi-http-transport-gotchas.md` — confirmed blocking/timeout constraints for ESP32 Arduino WiFi+HTTPClient, from xverify shell verification.
