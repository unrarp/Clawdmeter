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

// Provider order is the wire/UI order. Adding a provider: add an enum entry
// (before PROVIDER_COUNT), a PROVIDER_KEYS row in main.cpp, a widget bundle +
// ABSENT_MSG entry in ui.cpp, and a Provider entry (+ wire keys) in the
// PROVIDERS list in daemon/claude_usage_daemon.py.
enum { PROV_CLAUDE = 0, PROV_CODEX, PROVIDER_COUNT };

struct UsageData {
    ProviderUsage providers[PROVIDER_COUNT];
    bool valid;               // false until first successful parse
};
