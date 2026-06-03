#pragma once
#include <Arduino.h>

typedef enum { NET_DISCONNECTED, NET_CONNECTING, NET_ONLINE } net_state_t;

void        net_init(void);            // WiFi.begin() with creds from net_config.h
void        net_tick(void);            // non-blocking: drive reconnect + periodic GET
net_state_t net_get_state(void);
bool        net_has_data(void);        // true after a successful GET; CLEARED on read by net_get_data()
const char* net_get_data(void);        // last-good JSON body; clears has_data
void        net_request_refresh(void); // force an immediate GET on next tick
const char* net_get_ssid(void);        // diagnostics
const char* net_get_ip(void);          // diagnostics (dotted string)
int         net_get_rssi(void);        // diagnostics (dBm)
uint32_t    net_last_update_ms(void);  // millis() of last good GET; 0 if never
