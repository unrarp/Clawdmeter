#include "net.h"
#include "net_config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

// NTP defaults — fall back if an older net_config.h (gitignored) predates these
// keys, so the build doesn't break on a stale config. Real values belong in
// net_config.h (see net_config.example.h).
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef NTP_TZ
#define NTP_TZ "GMT0BST,M3.5.0/1,M10.5.0"  // UK (GMT/BST)
#endif

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static net_state_t s_state = NET_DISCONNECTED;

// Data buffer — 512 bytes matches the ~150-byte daemon payload with headroom
static char s_data_buf[512];
static bool s_has_data = false;

// Diagnostics string buffers (WiFi returns String objects; we need stable
// const char* pointers that outlive the String temporaries)
static char s_ssid_buf[64];
static char s_ip_buf[20];

// Timestamp (millis()) of the last successful GET; 0 = never
static uint32_t s_last_update_ms = 0;

// Staleness window for the daemon-health verdict: ~2.75× the device fetch
// interval, so it trips only after ~3 missed fetches — not on normal
// inter-fetch quiet. Derived from FETCH_INTERVAL_MS (net_config.h) so it can't
// drift out of sync when the interval changes (e.g. 60 s -> 165 s here).
#define DAEMON_STALE_MS ((uint32_t)FETCH_INTERVAL_MS * 11UL / 4UL)

// Resolved daemon IP (cached after first mDNS lookup to avoid blocking tick)
static IPAddress s_daemon_ip;
static bool      s_daemon_ip_resolved = false;

// mDNS started flag — run MDNS.begin() exactly once per WiFi connection
static bool s_mdns_started = false;

// Fetch scheduling
static uint32_t s_last_fetch_ms   = 0;   // millis() of last attempt
static bool     s_fetch_requested = false; // net_request_refresh() sets this

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Attempt mDNS + hostByName resolution; returns true on success.
static bool resolve_daemon(void) {
    if (!s_mdns_started) {
        MDNS.begin("clawdmeter");
        s_mdns_started = true;
    }

    IPAddress resolved;
    // WiFi.hostByName resolves both mDNS .local names and literal IPs.
    int ret = WiFi.hostByName(DAEMON_HOST, resolved);
    if (ret == 1) {
        s_daemon_ip          = resolved;
        s_daemon_ip_resolved = true;
        Serial.printf("[net] resolved %s → %s\n",
                      DAEMON_HOST, resolved.toString().c_str());
        return true;
    }
    Serial.printf("[net] hostByName(%s) failed (ret=%d)\n", DAEMON_HOST, ret);
    return false;
}

// Perform a single blocking HTTP GET and update state.  Must only be called
// when WiFi.status() == WL_CONNECTED.  Uses a short connect timeout so a
// dead daemon doesn't stall the LVGL loop.
static void do_fetch(void) {
    // Re-resolve if we don't have a cached IP (e.g. after a WiFi drop)
    if (!s_daemon_ip_resolved) {
        if (!resolve_daemon()) {
            // Can't reach host; leave last-good buffer intact. Consume the
            // fetch request AND record the attempt time, so neither half of the
            // gate (s_fetch_requested || interval_elapsed) re-enters the
            // blocking resolver on the very next tick (DNS retry storm).
            s_fetch_requested = false;
            s_last_fetch_ms   = millis();
            return;
        }
    }

    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d/usage",
             s_daemon_ip.toString().c_str(), DAEMON_PORT);

    HTTPClient http;
    http.setConnectTimeout(3000); // cap TCP SYN-ACK wait
    http.setTimeout(3000);        // cap response-read wait (default 5000 ms)
    http.begin(url);              // plain HTTP, no WiFiClientSecure

    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        // Copy into static buffer; truncate silently if oversized (shouldn't
        // happen — daemon payload is ~150 bytes)
        strncpy(s_data_buf, body.c_str(), sizeof(s_data_buf) - 1);
        s_data_buf[sizeof(s_data_buf) - 1] = '\0';
        s_has_data       = true;
        s_last_update_ms = millis();
        Serial.printf("[net] GET %s → 200 (%d bytes)\n", url, body.length());
    } else {
        // Non-200 or connection error: leave last-good buffer untouched.
        // If code < 0 the connection itself failed — drop the cached IP so the
        // next attempt re-resolves (handles daemon restart / IP change).
        Serial.printf("[net] GET %s → %d\n", url, code);
        if (code < 0) {
            s_daemon_ip_resolved = false;
        }
    }

    http.end();
    s_last_fetch_ms   = millis();
    s_fetch_requested = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void net_init(void) {
    s_state              = NET_CONNECTING;
    s_has_data           = false;
    s_last_update_ms     = 0;
    s_daemon_ip_resolved = false;
    s_mdns_started       = false;
    s_fetch_requested    = false;
    s_last_fetch_ms      = 0;
    s_data_buf[0]        = '\0';
    s_ssid_buf[0]        = '\0';
    s_ip_buf[0]          = '\0';

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // Deepest modem-sleep: this is a pull-only device (polls /usage every
    // FETCH_INTERVAL_MS, no incoming traffic to wait on), so the radio can skip
    // most DTIM beacons. A button-forced refresh still TXes immediately. Saves
    // the largest single chunk of battery draw while displaying. See
    // .claude/rules/networking.md.
    WiFi.setSleep(WIFI_PS_MAX_MODEM);
    Serial.printf("[net] WiFi.begin(%s)\n", WIFI_SSID);
}

void net_tick(void) {
    wl_status_t wifi_status = WiFi.status();

    // --- Handle state transitions ------------------------------------------

    if (s_state == NET_ONLINE && wifi_status != WL_CONNECTED) {
        // WiFi dropped
        Serial.println("[net] WiFi lost — NET_DISCONNECTED");
        s_state              = NET_DISCONNECTED;
        s_mdns_started       = false; // re-run MDNS.begin() on next connect
        s_daemon_ip_resolved = false;
        s_ip_buf[0]          = '\0';
        return; // nothing more to do this tick
    }

    if (s_state != NET_ONLINE && wifi_status == WL_CONNECTED) {
        // Freshly connected (or reconnected)
        Serial.printf("[net] WiFi connected, IP=%s\n",
                      WiFi.localIP().toString().c_str());
        s_state = NET_ONLINE;

        // Kick off SNTP so the WiFi page can show a wall-clock "Updated" time.
        // Non-blocking: the sync runs in the background and lands a few seconds
        // later (so the first fetch may still predate it — handled in the UI).
        configTzTime(NTP_TZ, NTP_SERVER);

        // Cache diagnostics strings
        strncpy(s_ssid_buf, WiFi.SSID().c_str(), sizeof(s_ssid_buf) - 1);
        s_ssid_buf[sizeof(s_ssid_buf) - 1] = '\0';
        strncpy(s_ip_buf, WiFi.localIP().toString().c_str(),
                sizeof(s_ip_buf) - 1);
        s_ip_buf[sizeof(s_ip_buf) - 1] = '\0';

        // mDNS + resolve (blocking — acceptable once per association)
        resolve_daemon();

        // Schedule an immediate fetch; do NOT call do_fetch() this tick so we
        // don't chain two blocking calls (hostByName + HTTP GET) in one loop
        // iteration and stall lv_timer_handler() for the combined duration.
        s_fetch_requested = true;
        return;
    }

    if (s_state == NET_CONNECTING && wifi_status == WL_DISCONNECTED) {
        // Still waiting; WiFi stack handles auto-reconnect internally
        return;
    }

    // WL_CONNECT_FAILED (wrong password) and WL_NO_SSID_AVAIL (wrong SSID) are
    // terminal — the stack stops retrying.  Kick WiFi.begin() again after a
    // brief debounce so the device recovers if the AP becomes available later.
    if ((s_state == NET_CONNECTING || s_state == NET_DISCONNECTED) &&
        (wifi_status == WL_CONNECT_FAILED || wifi_status == WL_NO_SSID_AVAIL)) {
        static uint32_t s_last_retry_ms = 0;
        if (millis() - s_last_retry_ms >= 15000) {
            Serial.printf("[net] WiFi terminal status %d — retrying WiFi.begin()\n",
                          (int)wifi_status);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            s_last_retry_ms = millis();
        }
        return;
    }

    // --- Periodic / on-demand fetch ----------------------------------------

    if (s_state == NET_ONLINE) {
        bool interval_elapsed =
            (millis() - s_last_fetch_ms) >= (uint32_t)FETCH_INTERVAL_MS;

        if (s_fetch_requested || interval_elapsed) {
            do_fetch();
        }
    }
}

net_state_t net_get_state(void) {
    return s_state;
}

bool net_has_data(void) {
    return s_has_data;
}

const char* net_get_data(void) {
    s_has_data = false; // clear-on-read — main.cpp gates on net_has_data() to avoid double-processing
    return s_data_buf;
}

void net_request_refresh(void) {
    s_fetch_requested = true;
}

const char* net_get_ssid(void) {
    return s_ssid_buf;
}

const char* net_get_ip(void) {
    return s_ip_buf;
}

int net_get_rssi(void) {
    return WiFi.RSSI();
}

uint32_t net_last_update_ms(void) {
    return s_last_update_ms;
}

daemon_health_t net_daemon_health(void) {
    if (s_state != NET_ONLINE) return DAEMON_OFFLINE;
    if (s_last_update_ms == 0)  return DAEMON_NO_DATA;
    if (millis() - s_last_update_ms <= DAEMON_STALE_MS) return DAEMON_CONNECTED;
    return DAEMON_STALE;
}
