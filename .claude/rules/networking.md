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

- **Wall-clock time comes from NTP, kicked off on each WiFi association.** `net_tick()` calls `configTzTime(NTP_TZ, NTP_SERVER)` on the connect transition — this is **non-blocking** (background SNTP), so unlike `hostByName()` it's safe in the tick. There is **no RTC**, and SNTP lands a few seconds *after* associating, so an early GET can predate the clock being set. The WiFi page's "Updated: HH:MM" line therefore does **not** store an epoch at fetch time; it reconstructs the fetch instant at render as `time(nullptr) - age_ms` (age from the always-valid monotonic `net_last_update_ms()`), which is stable across repaints and self-heals once the clock syncs — even for a GET that landed pre-sync. The render guards on `time(nullptr) >= 1700000000` (≈2023-11-14, a "clock is plausibly real" floor; unsynced newlib starts at 1970) and shows an em-dash until both a fetch and a synced clock exist. `NTP_SERVER`/`NTP_TZ` live in `net_config.h` (gitignored) with `#ifndef` fallbacks in `net.cpp`.

- **The daemon-health verdict is event-driven, not ticked.** `net_daemon_health()` derives `DAEMON_OFFLINE/NO_DATA/CONNECTED/STALE` from net state + elapsed time since the last good GET (`DAEMON_STALE_MS`, ~2.75× the fetch interval, derived from `FETCH_INTERVAL_MS`). Because "stale" is an elapsed-time transition that no GET will announce, `loop()` polls `net_daemon_health()` every iteration but repaints the WiFi page **only when the verdict (or net state) changes** — so the staleness label goes live without a periodic redraw. Don't reintroduce a fixed-interval WiFi refresh to drive this; the transition latch (`last_daemon_health` in `main.cpp`) is what keeps it current.

- **WiFi runs in `WIFI_PS_MAX_MODEM` (deepest modem-sleep), set once in `net_init()` after `WiFi.begin()`.** The device is pull-only — it polls `/usage` every `FETCH_INTERVAL_MS` and has no incoming traffic to wait on — so the radio can skip most DTIM beacons to save battery (the largest single draw while displaying). This is **not** the Arduino default (`WIFI_PS_MIN_MODEM`); don't drop the `setSleep()` call back to default. A button-forced `net_request_refresh()` still TXes immediately (TX wakes the radio regardless of PS mode), so the deeper sleep only adds latency to *unsolicited inbound* packets, of which there are none. The trade-off is purely battery-vs-nothing here.

## Related decisions

- `2026-06-03-wifi-http-transport-gotchas.md` — confirmed blocking/timeout constraints for ESP32 Arduino WiFi+HTTPClient, from xverify shell verification.
