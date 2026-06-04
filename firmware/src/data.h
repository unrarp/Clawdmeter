#pragma once
#include <Arduino.h>

// Per-provider usage window. session_pct/weekly_pct == -1 means "no data yet"
// (device shows "Connecting…"); present==false means the provider is absent
// (device shows the "No <provider> account" placeholder).
struct ProviderUsage {
    float session_pct;        // 5-hour window utilization (0-100), -1 = no data
    int   session_reset_mins; // minutes until session resets
    float weekly_pct;         // 7-day window utilization (0-100), -1 = no data
    int   weekly_reset_mins;  // minutes until weekly resets
    char  status[16];         // "allowed" / "limited" (parsed; not currently rendered)
    bool  ok;                 // last poll for this provider succeeded
    bool  present;            // provider is configured on the daemon side
};

// Provider order is the UI/fetch order, and the index into every per-provider
// table. Adding a provider is table-driven — add an enum entry (before
// PROVIDER_COUNT) and then one row in each table:
//   - net.cpp:  PROVIDERS[] (broker key + fetch fn) + a fetch_*() + a Cred buffer
//   - ui.cpp:   UI_PROVIDERS[] (name, logo, accent, absent msg, anim catalog)
//   - ui.cpp:   a WiFi-page row Y in compute_layout()'s wifi_prov_y[]
// The screen cycle, widgets, logos, WiFi rows, health array and animation all
// loop over PROVIDER_COUNT, so nothing else needs touching. The device fetches
// each provider directly and writes ProviderUsage straight into the UI — there
// is no wire-JSON layer (the daemon is a token broker, not a usage mapper).
// Full walkthrough: docs/porting/adding-a-provider.md.
enum { PROV_CLAUDE = 0, PROV_CODEX, PROVIDER_COUNT };

struct UsageData {
    ProviderUsage providers[PROVIDER_COUNT];
    bool valid;               // false until first successful parse
};
