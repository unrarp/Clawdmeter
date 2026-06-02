#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse

    // Codex usage (populated when daemon sends Codex data)
    float codex_session_pct;
    int   codex_session_reset_mins;
    float codex_weekly_pct;
    int   codex_weekly_reset_mins;
    char  codex_status[16];
    bool  codex_ok;

    // Provider presence flags
    bool  claude_present;    // true by default (backward-compat)
    bool  codex_present;     // false by default (absent on old payloads)
};
