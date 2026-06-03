# mDNS / WiFi spike

Throwaway pre-validation for the BLE→WiFi transport plan
([`docs/plans/2026-06-02-wifi-transport.md`](../../../docs/plans/2026-06-02-wifi-transport.md)).
Isolated PlatformIO project — no display/LVGL/NimBLE, just the Arduino core's
`WiFi` + `ESPmDNS` + `HTTPClient`. **Not part of the firmware build.**

## What it answers

On *your* network: can the ESP32 resolve the daemon host's `<hostname>.local`
and open a TCP connection to it? This decides v1 discovery — mDNS if it works,
a static-IP string in `net_config.h` if it doesn't.

## Run

```bash
cp include/spike_config.example.h include/spike_config.h   # then edit it
pio run -d firmware/tools/mdns_spike -e mdns_spike_s3 -t upload -t monitor
#   ESP32-C6 board? use -e mdns_spike_c6
#   wrong port?      append --upload-port /dev/ttyACM0  (or /dev/cu.usbmodem101)
```

Optional full-path check: set `HTTP_TEST 1` and on the daemon host run
`python3 -m http.server 8080` first — any HTTP response (even 404) proves TCP.

## Reading the log

Each 10s round prints two resolution attempts:

- `hostByName(...) OK` → the plan's `WiFi.hostByName()` path works as-is.
- only `MDNS.queryHost(...) OK` → use `MDNS.queryHost()` in `net.cpp` instead.
- both FAILED across many rounds → mDNS is unreliable here; set `DAEMON_HOST`
  to the daemon's literal LAN IP in `net_config.h` (the locked fallback).
