// Copy this file to spike_config.h (same dir) and fill in your values.
// spike_config.h is gitignored — your WiFi password never gets committed.
#pragma once

// --- WiFi (station mode) ---
#define WIFI_SSID      "your-ssid"
#define WIFI_PASSWORD  "your-password"

// --- Daemon host to resolve ---
// Bare hostname, WITHOUT the ".local" suffix. The spike tries both:
//   1. WiFi.hostByName("<DAEMON_HOSTNAME>.local")  (lwIP DNS+mDNS path)
//   2. MDNS.queryHost("<DAEMON_HOSTNAME>")          (explicit mDNS query)
// Find it on the daemon host with `hostname` (Linux) or `scutil --get
// LocalHostName` (macOS) — e.g. "my-laptop" → resolves "my-laptop.local".
#define DAEMON_HOSTNAME "my-laptop"

// --- Optional raw HTTP reachability check ---
// Proves the full TCP path, not just name resolution. Set HTTP_TEST to 1 and
// on the daemon host run a throwaway server first:  python3 -m http.server 8080
// Any HTTP response (even 404) confirms TCP works. 0 = resolution-only.
#define HTTP_TEST       1
#define HTTP_TEST_PORT  8080
#define HTTP_TEST_PATH  "/healthz"
