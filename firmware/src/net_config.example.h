#pragma once
#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-password"
#define DAEMON_HOST        "my-laptop.local"     // mDNS <hostname>.local (incl. .local suffix), or a literal IP
#define DAEMON_PORT        8080
#define FETCH_INTERVAL_MS  60000              // device → daemon poll cadence; daemon refreshes upstream every ~5 min regardless

// Wall-clock time for the WiFi page's "Updated: HH:MM" line. The device has no
// RTC, so it learns the time from NTP after WiFi associates (non-blocking SNTP).
// NTP_TZ is a POSIX TZ string (handles DST); the default below is UK (GMT/BST).
// Examples: US Eastern "EST5EDT,M3.2.0,M11.1.0", Central Europe "CET-1CEST,M3.5.0,M10.5.0/3".
#define NTP_SERVER         "pool.ntp.org"
#define NTP_TZ             "GMT0BST,M3.5.0/1,M10.5.0"
