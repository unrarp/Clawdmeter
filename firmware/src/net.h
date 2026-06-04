#pragma once
#include <Arduino.h>

typedef enum { NET_DISCONNECTED, NET_CONNECTING, NET_ONLINE } net_state_t;

// Derived per-provider usage-data/auth health, classified from net state, that
// provider's token state, broker reachability, and its last-good-fetch age.
// Polled cheaply each loop so the WiFi page repaints only on a transition (some
// verdicts are elapsed-time based, so they must be evaluated over time — but
// without a periodic UI redraw).
//
// One verdict per provider (priority order in net_provider_health): an auth/token
// problem outranks the data-freshness verdict, since it explains why data may be
// missing and needs the user. Providers are independent — one needing re-auth
// never changes the other's verdict.
//
// "Usage", not "daemon": post-cutover the device fetches usage DIRECTLY from the
// providers every FETCH_INTERVAL_MS; the broker (daemon) is only contacted to
// (re)fetch tokens (first boot / a 401), so this verdict is about provider data
// freshness, not a live connection to the host daemon.
typedef enum {
    USAGE_OFFLINE,      // WiFi not online — can't say anything about data/broker
    USAGE_NEEDS_ACTION, // broker said 409 — user must re-auth a provider (run claude setup-token / codex)
    USAGE_BROKER_DOWN,  // a token is needed but the broker is unreachable
    USAGE_NO_TOKEN,     // online, fetching tokens from the broker (none cached yet)
    USAGE_NO_DATA,      // tokens in hand, but no good provider fetch has ever landed
    USAGE_LIVE,         // last good provider fetch is within the staleness window
    USAGE_STALE,        // last good provider fetch is older than the staleness window
} usage_health_t;

void        net_init(void);            // WiFi.begin() with creds from net_config.h
void        net_tick(void);            // non-blocking: drive reconnect + periodic GET
net_state_t net_get_state(void);
bool        net_has_data(void);        // true after a successful GET; CLEARED on read by net_get_data()
const char* net_get_data(void);        // last-good JSON body; clears has_data
void        net_request_refresh(void); // force an immediate GET on next tick
const char* net_get_ssid(void);        // diagnostics
const char* net_get_ip(void);          // diagnostics (dotted string)
int         net_get_rssi(void);        // diagnostics (dBm)
uint32_t    net_last_update_ms(void);    // millis() of most-recent good GET across providers; 0 if none
usage_health_t  net_provider_health(int prov);  // derived per-provider usage/auth verdict (see enum)
