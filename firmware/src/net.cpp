#include "net.h"
#include "net_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// NTP defaults — fall back if an older net_config.h (gitignored) predates these
// keys, so the build doesn't break on a stale config. Real values belong in
// net_config.h (see net_config.example.h).
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef NTP_TZ
#define NTP_TZ "GMT0BST,M3.5.0/1,M10.5.0"  // UK (GMT/BST)
#endif

// Embedded full root-CA bundle (200 CAs) from the core's libmbedtls.a — used to
// validate TLS to api.anthropic.com and chatgpt.com. Two-arg setCACertBundle on
// this core; symbols verified in firmware/tools/tls_spike (Phase 1 gate).
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

// Provider endpoints (device-direct, Phase 3a).
#define ANTHROPIC_URL "https://api.anthropic.com/v1/messages"
#define CODEX_URL     "https://chatgpt.com/backend-api/wham/usage"
#define HTTP_CONNECT_TIMEOUT 6000  // TCP SYN-ACK cap (ms)
#define HTTP_READ_TIMEOUT    6000  // response-read cap (ms) — pair with connect

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static net_state_t s_state = NET_DISCONNECTED;

// Synthesized 14-key wire JSON (so main.cpp's parse_json + the UI stay unchanged).
static char s_data_buf[512];
static bool s_has_data = false;

// Diagnostics string buffers (stable const char* outliving WiFi String temporaries).
static char s_ssid_buf[64];
static char s_ip_buf[20];

// millis() of last successful provider fetch (any provider); 0 = never.
static uint32_t s_last_update_ms = 0;

// Staleness window for the health verdict: ~2.75× the fetch interval, derived
// from FETCH_INTERVAL_MS so it can't drift when the interval changes.
#define DAEMON_STALE_MS ((uint32_t)FETCH_INTERVAL_MS * 11UL / 4UL)

// Per-provider last-good slot. -1 = no data yet (UI shows "Connecting…").
struct Slot {
    int  s, sr, w, wr;   // session %, session reset (min), weekly %, weekly reset (min)
    char st[12];         // "allowed" / "limited"
    bool ok;             // last fetch for this provider succeeded
};
static Slot s_claude = {-1, -1, -1, -1, "allowed", false};
static Slot s_codex  = {-1, -1, -1, -1, "unknown", false};

// Per-provider fetch scheduling. One blocking TLS call per tick (round-robin),
// so two providers never chain two handshakes in a single loop iteration.
static uint32_t s_claude_last_fetch = 0;
static uint32_t s_codex_last_fetch  = 0;
// Per-provider force flags so a refresh/association refetches BOTH (one per tick,
// cleared as each is serviced).
static bool     s_force_claude      = false;
static bool     s_force_codex       = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Absolute epoch (seconds) -> whole minutes from now, or -1 if the clock isn't
// synced yet (newlib starts at 1970; guard on a "plausibly real" floor).
static int epoch_to_mins(long epoch) {
    time_t now = time(nullptr);
    if (now < 1700000000L || epoch <= 0) return -1;
    long d = epoch - (long)now;
    return d > 0 ? (int)(d / 60) : 0;
}

// Build the compact 14-key JSON from both slots into s_data_buf and flag it for
// main.cpp to re-parse. present (sp/cp) is always true in 3a — tokens are
// configured locally; ok/cok carry per-provider fetch success.
static void synthesize_payload(void) {
    char buf[sizeof(s_data_buf)];
    snprintf(buf, sizeof(buf),
             "{\"s\":%d,\"sr\":%d,\"w\":%d,\"wr\":%d,\"st\":\"%s\",\"ok\":%s,\"sp\":true,"
             "\"cs\":%d,\"csr\":%d,\"cw\":%d,\"cwr\":%d,\"cst\":\"%s\",\"cok\":%s,\"cp\":true}",
             s_claude.s, s_claude.sr, s_claude.w, s_claude.wr, s_claude.st,
             s_claude.ok ? "true" : "false",
             s_codex.s, s_codex.sr, s_codex.w, s_codex.wr, s_codex.st,
             s_codex.ok ? "true" : "false");
    // Only flag a re-parse when the wire actually changed — a failing provider
    // re-emitting identical state every tick must not re-trigger main.cpp's
    // parse/render. A genuine change (new numbers, or an ok flag flip) does.
    if (strcmp(buf, s_data_buf) != 0) {
        strcpy(s_data_buf, buf);
        s_has_data = true;
    }
}

static void configure_tls(WiFiClientSecure &c) {
    c.setCACertBundle(x509_crt_bundle_start,
                      (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
}

// Claude: POST a 1-token message and scrape the unified rate-limit headers.
// utilization headers are a 0-1 fraction; -reset headers are absolute epochs.
static void fetch_claude(void) {
    WiFiClientSecure client;
    configure_tls(client);
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT);
    http.setTimeout(HTTP_READ_TIMEOUT);
    if (!http.begin(client, ANTHROPIC_URL)) {
        Serial.println("[net] claude begin() failed");
        s_claude.ok = false;
        http.end();
        return;
    }
    static const char *keep[] = {
        "anthropic-ratelimit-unified-5h-utilization",
        "anthropic-ratelimit-unified-5h-reset",
        "anthropic-ratelimit-unified-7d-utilization",
        "anthropic-ratelimit-unified-7d-reset",
        "anthropic-ratelimit-unified-status",
    };
    http.collectHeaders(keep, sizeof(keep) / sizeof(keep[0]));
    http.addHeader("Authorization", String("Bearer ") + CLAUDE_TOKEN);
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("anthropic-beta", "oauth-2025-04-20");
    http.addHeader("Content-Type", "application/json");
    const String body =
        "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    int code = http.POST(body);
    String u5 = http.header("anthropic-ratelimit-unified-5h-utilization");
    String u7 = http.header("anthropic-ratelimit-unified-7d-utilization");
    if (code == 200 && u5.length() && u7.length()) {
        s_claude.s  = (int)lroundf(u5.toFloat() * 100.0f);
        s_claude.w  = (int)lroundf(u7.toFloat() * 100.0f);
        s_claude.sr = epoch_to_mins(http.header("anthropic-ratelimit-unified-5h-reset").toInt());
        s_claude.wr = epoch_to_mins(http.header("anthropic-ratelimit-unified-7d-reset").toInt());
        String st = http.header("anthropic-ratelimit-unified-status");
        bool limited = (st.length() && st != "allowed") || s_claude.s >= 100;
        strlcpy(s_claude.st, limited ? "limited" : "allowed", sizeof(s_claude.st));
        s_claude.ok = true;
        s_last_update_ms = millis();
        Serial.printf("[net] claude 200: s=%d w=%d\n", s_claude.s, s_claude.w);
    } else {
        // 200-without-headers is a failure: keep last-good values, flag ok=false
        // (a missing/renamed header would otherwise scrape to 0% "allowed").
        s_claude.ok = false;
        Serial.printf("[net] claude POST -> %d (hdrs %d/%d)\n", code, u5.length(), u7.length());
    }
    http.end();
}

// Codex: GET the usage JSON and map the two windows.
static void fetch_codex(void) {
    WiFiClientSecure client;
    configure_tls(client);
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT);
    http.setTimeout(HTTP_READ_TIMEOUT);
    if (!http.begin(client, CODEX_URL)) {
        Serial.println("[net] codex begin() failed");
        s_codex.ok = false;
        http.end();
        return;
    }
    http.addHeader("Authorization", String("Bearer ") + CODEX_ACCESS_TOKEN);
    http.addHeader("ChatGPT-Account-Id", CODEX_ACCOUNT_ID);
    http.addHeader("User-Agent", "clawdmeter");
    int code = http.GET();
    if (code == 200) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getStream());
        JsonObject rl = doc["rate_limit"];
        if (err || rl.isNull()) {
            // A 200 without a rate_limit object is a failure, not healthy-with-no-data.
            s_codex.ok = false;
            Serial.printf("[net] codex bad body: %s\n", err ? err.c_str() : "no rate_limit");
        } else {
            s_codex.s  = rl["primary_window"]["used_percent"]   | -1;
            s_codex.w  = rl["secondary_window"]["used_percent"] | -1;
            long pr = rl["primary_window"]["reset_after_seconds"]   | -1L;
            long sr = rl["secondary_window"]["reset_after_seconds"] | -1L;
            s_codex.sr = pr >= 0 ? (int)(pr / 60) : -1;
            s_codex.wr = sr >= 0 ? (int)(sr / 60) : -1;
            bool allowed = rl["allowed"] | true;
            bool reached = rl["limit_reached"] | false;
            bool limited = !allowed || reached || s_codex.s >= 100;
            strlcpy(s_codex.st, limited ? "limited" : "allowed", sizeof(s_codex.st));
            s_codex.ok = true;
            s_last_update_ms = millis();
            Serial.printf("[net] codex 200: cs=%d cw=%d\n", s_codex.s, s_codex.w);
        }
    } else {
        s_codex.ok = false;
        Serial.printf("[net] codex GET -> %d\n", code);
    }
    http.end();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void net_init(void) {
    s_state           = NET_CONNECTING;
    s_has_data        = false;
    s_last_update_ms  = 0;
    s_force_claude    = false;
    s_force_codex     = false;
    s_claude_last_fetch = 0;
    s_codex_last_fetch  = 0;
    s_data_buf[0]     = '\0';
    s_ssid_buf[0]     = '\0';
    s_ip_buf[0]       = '\0';

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // Deepest modem-sleep: pull-only device, no inbound traffic to wait on.
    // See .claude/rules/networking.md.
    WiFi.setSleep(WIFI_PS_MAX_MODEM);
    Serial.printf("[net] WiFi.begin(%s)\n", WIFI_SSID);
}

void net_tick(void) {
    wl_status_t wifi_status = WiFi.status();

    if (s_state == NET_ONLINE && wifi_status != WL_CONNECTED) {
        Serial.println("[net] WiFi lost — NET_DISCONNECTED");
        s_state     = NET_DISCONNECTED;
        s_ip_buf[0] = '\0';
        return;
    }

    if (s_state != NET_ONLINE && wifi_status == WL_CONNECTED) {
        Serial.printf("[net] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
        s_state = NET_ONLINE;
        // Non-blocking SNTP for the WiFi page's wall-clock + epoch→minutes math.
        configTzTime(NTP_TZ, NTP_SERVER);
        strlcpy(s_ssid_buf, WiFi.SSID().c_str(), sizeof(s_ssid_buf));
        strlcpy(s_ip_buf, WiFi.localIP().toString().c_str(), sizeof(s_ip_buf));
        // Defer the first fetch to a later tick so we don't run a TLS handshake
        // in the association-transition tick.
        s_force_claude = true;
        s_force_codex  = true;
        return;
    }

    // Terminal WiFi states (bad SSID/password): re-kick begin() on a debounce.
    if ((s_state == NET_CONNECTING || s_state == NET_DISCONNECTED) &&
        (wifi_status == WL_CONNECT_FAILED || wifi_status == WL_NO_SSID_AVAIL)) {
        static uint32_t s_last_retry_ms = 0;
        if (millis() - s_last_retry_ms >= 15000) {
            Serial.printf("[net] WiFi terminal status %d — retrying WiFi.begin()\n", (int)wifi_status);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            s_last_retry_ms = millis();
        }
        return;
    }

    if (s_state != NET_ONLINE) return;

    // --- Fetch at most ONE provider this tick (one blocking TLS call max) -----
    // Claude takes priority on a tick where both are due; Codex gets the next
    // tick. After the initial both-due burst they settle one tick apart, each
    // still polling every FETCH_INTERVAL_MS.
    uint32_t now = millis();
    bool claude_due = s_force_claude || (now - s_claude_last_fetch) >= (uint32_t)FETCH_INTERVAL_MS;
    bool codex_due  = s_force_codex  || (now - s_codex_last_fetch)  >= (uint32_t)FETCH_INTERVAL_MS;

    if (claude_due) {
        fetch_claude();
        s_claude_last_fetch = now;  // measure the interval from fetch start, not end
        s_force_claude = false;
        synthesize_payload();
    } else if (codex_due) {
        fetch_codex();
        s_codex_last_fetch = now;
        s_force_codex = false;
        synthesize_payload();
    }
}

net_state_t net_get_state(void) { return s_state; }

bool net_has_data(void) { return s_has_data; }

const char* net_get_data(void) {
    s_has_data = false;  // clear-on-read
    return s_data_buf;
}

void net_request_refresh(void) { s_force_claude = true; s_force_codex = true; }

const char* net_get_ssid(void) { return s_ssid_buf; }
const char* net_get_ip(void)   { return s_ip_buf; }
int         net_get_rssi(void) { return WiFi.RSSI(); }
uint32_t    net_last_update_ms(void) { return s_last_update_ms; }

daemon_health_t net_daemon_health(void) {
    // NOTE (Phase 3b): single verdict off the last *any-provider* success — if one
    // provider succeeds and the other is dead forever, this still reports
    // CONNECTED (the per-provider `ok`/`cok` flags carry the real state to the UI).
    // Phase 3b adds a per-source health model.
    if (s_state != NET_ONLINE) return DAEMON_OFFLINE;
    if (s_last_update_ms == 0)  return DAEMON_NO_DATA;
    if (millis() - s_last_update_ms <= DAEMON_STALE_MS) return DAEMON_CONNECTED;
    return DAEMON_STALE;
}
