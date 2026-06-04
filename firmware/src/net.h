#pragma once
#include <Arduino.h>

typedef enum { NET_DISCONNECTED, NET_CONNECTING, NET_ONLINE } net_state_t;

// Derived data/auth health, classified from net state, token state, broker
// reachability, and last-good-fetch age. Polled cheaply each loop so the WiFi
// page repaints only on a transition (some verdicts are elapsed-time based, so
// they must be evaluated over time — but without a periodic UI redraw).
//
// Aggregate across both providers (priority order in net_daemon_health): an
// auth/token problem on either provider outranks the data-freshness verdict,
// since it explains why data may be missing and needs the user. The per-provider
// ok/cok flags on the wire still carry each provider's own state to its panel.
typedef enum {
    DAEMON_OFFLINE,      // WiFi not online — can't say anything about data/broker
    DAEMON_NEEDS_ACTION, // broker said 409 — user must re-auth a provider (run claude setup-token / codex)
    DAEMON_BROKER_DOWN,  // a token is needed but the broker is unreachable
    DAEMON_NO_TOKEN,     // online, fetching tokens from the broker (none cached yet)
    DAEMON_NO_DATA,      // tokens in hand, but no good provider fetch has ever landed
    DAEMON_CONNECTED,    // last good provider fetch is within the staleness window
    DAEMON_STALE,        // last good provider fetch is older than the staleness window
} daemon_health_t;

void        net_init(void);            // WiFi.begin() with creds from net_config.h
void        net_tick(void);            // non-blocking: drive reconnect + periodic GET
net_state_t net_get_state(void);
bool        net_has_data(void);        // true after a successful GET; CLEARED on read by net_get_data()
const char* net_get_data(void);        // last-good JSON body; clears has_data
void        net_request_refresh(void); // force an immediate GET on next tick
const char* net_get_ssid(void);        // diagnostics
const char* net_get_ip(void);          // diagnostics (dotted string)
int         net_get_rssi(void);        // diagnostics (dBm)
uint32_t    net_last_update_ms(void);    // millis() of last good GET; 0 if never
daemon_health_t net_daemon_health(void); // derived reachability verdict (see enum)
