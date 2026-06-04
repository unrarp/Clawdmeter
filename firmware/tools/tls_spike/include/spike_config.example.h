// Copy this file to spike_config.h (same dir) and fill in your values, OR run
// the generator in README.md which fills it from your local creds.
// spike_config.h is gitignored — your secrets never get committed.
#pragma once

// --- WiFi (station mode) ---
#define WIFI_SSID      "your-ssid"
#define WIFI_PASSWORD  "your-password"

// --- Claude ---
// Any inference-scoped OAuth token works for the /v1/messages header-scrape:
// a `claude setup-token` (production path, 1-year) or the accessToken from
// ~/.claude/.credentials.json (handy for the spike). NOT a platform sk-ant-api
// key — that's a different account and can't see subscription usage.
#define CLAUDE_TOKEN   "sk-ant-oat01-..."

// --- Codex ---
// tokens.access_token and tokens.account_id from ~/.codex/auth.json.
#define CODEX_ACCESS_TOKEN  "eyJhbG..."
#define CODEX_ACCOUNT_ID    "00000000-0000-0000-0000-000000000000"
