#include "net.h"
#include "net_config.h"
#include "data.h"
#include "token_store.h"
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
// Pre-shared secret the broker requires (X-Broker-Key). Fallback keeps the build
// alive on a stale net_config.h; an empty key just earns a 403 at runtime.
#ifndef BROKER_KEY
#define BROKER_KEY ""
#endif

// Embedded full root-CA bundle (200 CAs) from the core's libmbedtls.a — used to
// validate TLS to api.anthropic.com and chatgpt.com. Two-arg setCACertBundle on
// this core; symbols verified in firmware/tools/tls_spike (Phase 1 gate).
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

// Provider endpoints (device-direct).
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

// Per-provider credential cache (loaded from NVS, refreshed from the broker).
// The Codex access_token is a JWT (~2 KB observed) — size the buffer generously.
static char s_claude_token[256];
static char s_codex_token[2400];
static char s_codex_account[64];

// Per-provider token lifecycle. need_fetch drives a broker /tokens round-trip.
enum TokenState { TOK_MISSING, TOK_OK, TOK_NEEDS_ACTION };
static TokenState s_tok[PROVIDER_COUNT];
static bool       s_need_fetch[PROVIDER_COUNT];

// Per-provider fetch scheduling. One blocking TLS call per tick (round-robin),
// so two providers never chain two handshakes in a single loop iteration.
static uint32_t s_claude_last_fetch = 0;
static uint32_t s_codex_last_fetch  = 0;
// Per-provider force flags so a refresh/association refetches BOTH (one per tick,
// cleared as each is serviced).
static bool     s_force_claude      = false;
static bool     s_force_codex       = false;

// Broker (token source) reachability. Resolved once per session; /tokens calls
// are throttled to FETCH_INTERVAL_MS so a rejected/expired token can't storm.
static IPAddress s_broker_ip;
static bool      s_broker_resolved   = false;
static bool      s_broker_attempted  = false;  // have we tried /tokens since boot?
static bool      s_broker_reachable  = false;  // did the last attempt get a reply?
static uint32_t  s_broker_last_attempt = 0;

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
// main.cpp to re-parse. present (sp/cp) is always true — the device always shows
// a panel per provider; a needs-action provider is surfaced via the WiFi-page
// health verdict, not by blanking its panel. ok/cok carry per-provider success.
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

// ---------------------------------------------------------------------------
// Token broker (plain HTTP on the trusted LAN)
// ---------------------------------------------------------------------------

// Resolve the broker host once and cache the IP. hostByName() blocks the loop
// for the full DNS timeout (see .claude/rules/networking.md) — only ever called
// here, and only when a token (re)fetch is actually due, never per-tick.
static bool resolve_broker(void) {
    if (s_broker_resolved) return true;
    IPAddress ip;
    if (WiFi.hostByName(DAEMON_HOST, ip) == 1) {
        s_broker_ip = ip;
        s_broker_resolved = true;
        Serial.printf("[net] broker %s -> %s\n", DAEMON_HOST, ip.toString().c_str());
        return true;
    }
    Serial.printf("[net] broker resolve failed: %s\n", DAEMON_HOST);
    return false;
}

// Apply one provider object from the /tokens response. A "token" field means the
// credential is usable (cache it, mark OK, kick a fetch); a "needs_action" field
// means the user must re-auth (mark NEEDS_ACTION and stop refetching). An absent
// provider key leaves prior state but clears need_fetch so we don't hammer.
static void apply_token(int prov, JsonVariantConst o,
                        char* tok, size_t tok_sz, char* acct, size_t acct_sz) {
    const char* t = o["token"] | (const char*)nullptr;
    if (t && *t) {
        strlcpy(tok, t, tok_sz);
        if (acct) strlcpy(acct, o["account_id"] | "", acct_sz);
        token_store_save(prov, tok, acct);  // cache to NVS for the next boot
        s_tok[prov] = TOK_OK;
        s_need_fetch[prov] = false;
        if (prov == PROV_CLAUDE) s_force_claude = true;
        else if (prov == PROV_CODEX) s_force_codex = true;
        Serial.printf("[net] broker token ok: prov=%d\n", prov);
    } else if (!o["needs_action"].isNull()) {
        // Explicit needs-action: the user must re-auth. Evict the dead credential
        // from RAM *and* NVS so a reboot can't reload it and replay a guaranteed
        // 401 before rediscovering this state.
        token_store_clear(prov);
        tok[0] = '\0';
        if (acct) acct[0] = '\0';
        s_tok[prov] = TOK_NEEDS_ACTION;
        s_need_fetch[prov] = false;
        Serial.printf("[net] broker needs_action prov=%d: %s\n",
                      prov, (const char*)(o["needs_action"] | "?"));
    } else {
        // Provider key absent or malformed (shouldn't happen per the broker
        // contract). Leave need_fetch as-is so the throttled retry tries again
        // rather than silently stranding the provider as needs-action.
        Serial.printf("[net] broker: no usable field for prov=%d\n", prov);
    }
}

// One blocking GET to the broker's /tokens. The caller gates cadence; this just
// performs the round-trip and updates per-provider token state. 200 and 409 both
// carry usable per-provider data (409 = at least one provider needs action).
static void fetch_tokens(void) {
    s_broker_reachable = false;
    if (!resolve_broker()) return;

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT);
    http.setTimeout(HTTP_READ_TIMEOUT);
    char url[80];  // "http://" + IP (≤39 for IPv6) + ":port" + "/tokens" + NUL
    snprintf(url, sizeof(url), "http://%s:%u/tokens",
             s_broker_ip.toString().c_str(), (unsigned)DAEMON_PORT);
    if (!http.begin(client, url)) {
        Serial.println("[net] broker begin() failed");
        http.end();
        return;
    }
    http.addHeader("X-Broker-Key", BROKER_KEY);
    int code = http.GET();
    if (code != 200 && code != 409) {
        Serial.printf("[net] broker /tokens -> %d\n", code);
        http.end();
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("[net] broker bad body: %s\n", err.c_str());
        return;
    }
    // Parseable reply = broker is reachable (clears DAEMON_BROKER_DOWN).
    s_broker_reachable = true;
    apply_token(PROV_CLAUDE, doc["claude"], s_claude_token, sizeof(s_claude_token), nullptr, 0);
    apply_token(PROV_CODEX,  doc["codex"],  s_codex_token,  sizeof(s_codex_token),
                s_codex_account, sizeof(s_codex_account));
}

// ---------------------------------------------------------------------------
// Provider fetches
// ---------------------------------------------------------------------------

// A provider token was rejected: flag it for a broker refetch. Cadence is gated
// by s_broker_last_attempt (see net_tick), so a persistently-bad token yields at
// most one broker call + one provider call per FETCH_INTERVAL_MS, never a storm.
static void on_auth_reject(int prov, int code) {
    // Mark the token unusable so the provider-due guard stops calling the API
    // with the rejected credential during the throttle window; the broker
    // refetch is what restores TOK_OK.
    s_tok[prov] = TOK_MISSING;
    s_need_fetch[prov] = true;
    Serial.printf("[net] prov=%d auth %d — will refetch token\n", prov, code);
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
    http.addHeader("Authorization", String("Bearer ") + s_claude_token);
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("anthropic-beta", "oauth-2025-04-20");
    http.addHeader("Content-Type", "application/json");
    const String body =
        "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    int code = http.POST(body);
    if (code == 401 || code == 403) {
        on_auth_reject(PROV_CLAUDE, code);
        s_claude.ok = false;
        http.end();
        return;
    }
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
    http.addHeader("Authorization", String("Bearer ") + s_codex_token);
    http.addHeader("ChatGPT-Account-Id", s_codex_account);
    http.addHeader("User-Agent", "clawdmeter");
    int code = http.GET();
    if (code == 401 || code == 403) {
        on_auth_reject(PROV_CODEX, code);
        s_codex.ok = false;
        http.end();
        return;
    }
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
    s_broker_resolved   = false;
    s_broker_attempted  = false;
    s_broker_reachable  = false;
    s_broker_last_attempt = 0;
    s_data_buf[0]     = '\0';
    s_ssid_buf[0]     = '\0';
    s_ip_buf[0]       = '\0';
    s_claude_token[0]  = '\0';
    s_codex_token[0]   = '\0';
    s_codex_account[0] = '\0';

    // Load cached credentials from NVS. A provider with a stored token starts
    // ready; a missing one starts MISSING and pulls from the broker once online.
    token_store_init();
    for (int i = 0; i < PROVIDER_COUNT; i++) { s_tok[i] = TOK_MISSING; s_need_fetch[i] = true; }
    if (token_store_load(PROV_CLAUDE, s_claude_token, sizeof(s_claude_token), nullptr, 0)) {
        s_tok[PROV_CLAUDE] = TOK_OK; s_need_fetch[PROV_CLAUDE] = false;
    }
    if (token_store_load(PROV_CODEX, s_codex_token, sizeof(s_codex_token),
                         s_codex_account, sizeof(s_codex_account))) {
        s_tok[PROV_CODEX] = TOK_OK; s_need_fetch[PROV_CODEX] = false;
    }

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
        // The cached broker IP is a per-association DNS result — drop it so a
        // reconnect (DHCP churn, host moved, roam) re-resolves DAEMON_HOST.
        s_broker_resolved = false;
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
        // Re-arm the broker so a pending token (re)fetch fires promptly after
        // (re)association instead of waiting out the pre-drop throttle window.
        s_broker_attempted = false;
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

    uint32_t now = millis();

    // --- Token (re)fetch takes priority for the tick -------------------------
    // If either provider needs a credential and the broker cadence allows, do
    // the one blocking /tokens call and bail — never chain it with a provider
    // TLS handshake in the same tick. The first attempt fires immediately; after
    // that it's throttled to FETCH_INTERVAL_MS so a rejected token can't storm.
    bool need = s_need_fetch[PROV_CLAUDE] || s_need_fetch[PROV_CODEX];
    bool broker_due = need && (!s_broker_attempted ||
                               (now - s_broker_last_attempt) >= (uint32_t)FETCH_INTERVAL_MS);
    if (broker_due) {
        s_broker_attempted   = true;
        s_broker_last_attempt = now;
        fetch_tokens();
        return;
    }

    // --- Otherwise fetch at most ONE provider with a usable token ------------
    // Claude takes priority on a tick where both are due; Codex gets the next.
    // A provider without a usable token is skipped (its data stays "Connecting…"
    // / last-good); the broker path above is what recovers it.
    bool claude_due = s_tok[PROV_CLAUDE] == TOK_OK &&
        (s_force_claude || (now - s_claude_last_fetch) >= (uint32_t)FETCH_INTERVAL_MS);
    bool codex_due  = s_tok[PROV_CODEX] == TOK_OK &&
        (s_force_codex  || (now - s_codex_last_fetch)  >= (uint32_t)FETCH_INTERVAL_MS);

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

void net_request_refresh(void) {
    s_force_claude = true;
    s_force_codex  = true;
    // A manual refresh also retries any provider that went needs-action (e.g.
    // after the user re-ran `claude setup-token` / codex) and lets the broker be
    // hit immediately rather than waiting out the throttle.
    for (int i = 0; i < PROVIDER_COUNT; i++)
        if (s_tok[i] == TOK_NEEDS_ACTION) s_need_fetch[i] = true;
    s_broker_attempted = false;
}

const char* net_get_ssid(void) { return s_ssid_buf; }
const char* net_get_ip(void)   { return s_ip_buf; }
int         net_get_rssi(void) { return WiFi.RSSI(); }
uint32_t    net_last_update_ms(void) { return s_last_update_ms; }

daemon_health_t net_daemon_health(void) {
    // Aggregate verdict across both providers. Auth/token problems outrank the
    // data-freshness states: they're actionable and explain missing data. The
    // per-provider ok/cok flags still carry each provider's own state to its
    // panel, so a one-provider problem doesn't blank the healthy one's numbers.
    if (s_state != NET_ONLINE) return DAEMON_OFFLINE;
    if (s_tok[PROV_CLAUDE] == TOK_NEEDS_ACTION || s_tok[PROV_CODEX] == TOK_NEEDS_ACTION)
        return DAEMON_NEEDS_ACTION;
    bool need = s_need_fetch[PROV_CLAUDE] || s_need_fetch[PROV_CODEX];
    if (need && s_broker_attempted && !s_broker_reachable) return DAEMON_BROKER_DOWN;
    if (need) return DAEMON_NO_TOKEN;
    if (s_last_update_ms == 0)  return DAEMON_NO_DATA;
    if (millis() - s_last_update_ms <= DAEMON_STALE_MS) return DAEMON_CONNECTED;
    return DAEMON_STALE;
}
