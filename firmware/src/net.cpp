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

// Last-good usage published to main.cpp (see publish_if_changed). The device
// writes ProviderUsage directly now — there is no wire-JSON round-trip.
static bool s_has_data = false;

// Diagnostics string buffers (stable const char* outliving WiFi String temporaries).
static char s_ssid_buf[64];
static char s_ip_buf[20];

// millis() of each provider's last successful fetch; 0 = never. Per-provider so
// the WiFi page can show each provider's own live/stale freshness independently.
static uint32_t s_last_update_ms[PROVIDER_COUNT] = {0};

// Staleness window for the health verdict: ~2.75× the fetch interval, derived
// from FETCH_INTERVAL_MS so it can't drift when the interval changes.
#define USAGE_STALE_MS ((uint32_t)FETCH_INTERVAL_MS * 11UL / 4UL)

// Per-provider live usage, written directly by the fetchers (no wire-JSON
// round-trip). session_pct/weekly_pct == -1 => no data yet (UI shows
// "Connecting…"); present is always true post-cutover (a provider needing
// re-auth is surfaced via the WiFi-page health verdict, not by blanking it).
static ProviderUsage s_usage[PROVIDER_COUNT];
// Last snapshot handed to main.cpp; a fetch publishes only when a field changes.
static UsageData s_pub;

// Per-provider credential cache (loaded from NVS, refreshed from the broker).
// Buffers are sized per provider — Codex's access_token is a ~2 KB JWT, Claude's
// is ~256 B — and reached through the Cred table so the fetch/token code is
// index-driven. account is nullptr/0 for providers without an account id.
struct Cred { char* token; size_t token_cap; char* account; size_t account_cap; };
static char s_claude_token[256];
static char s_codex_token[2400];
static char s_codex_account[64];
static Cred s_cred[PROVIDER_COUNT] = {
    { s_claude_token, sizeof(s_claude_token), nullptr,         0                       },
    { s_codex_token,  sizeof(s_codex_token),  s_codex_account, sizeof(s_codex_account) },
};

// Per-provider token lifecycle. need_fetch drives a broker /tokens round-trip.
enum TokenState { TOK_MISSING, TOK_OK, TOK_NEEDS_ACTION };
static TokenState s_tok[PROVIDER_COUNT];
static bool       s_need_fetch[PROVIDER_COUNT];

// Per-provider fetch scheduling. One blocking TLS call per tick (round-robin in
// provider-index order), so two providers never chain two handshakes in a single
// loop iteration. force[] refetches a provider on its next due tick regardless
// of the interval (armed on (re)association and on a freshly fetched token).
static uint32_t s_last_fetch[PROVIDER_COUNT] = {0};
static bool     s_force[PROVIDER_COUNT]      = {false};

// Per-provider descriptor: the broker JSON key its token arrives under, and the
// fetch routine that maps its (unique) API into s_usage. Adding a provider =
// one fetch_*() + one row here (+ a Cred buffer above and a data.h enum entry).
static void fetch_claude(int prov);
static void fetch_codex(int prov);
struct NetProvider { const char* broker_key; void (*fetch)(int prov); };
static const NetProvider PROVIDERS[PROVIDER_COUNT] = {
    { "claude", fetch_claude },
    { "codex",  fetch_codex  },
};

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

// Field-by-field ProviderUsage equality for the publish gate. Percentages are
// always integer-valued (Claude rounds, Codex's used_percent is an int), so the
// float == is exact here. NB main.cpp::usage_changed() is the idle-timer analogue
// of this — it deliberately ignores *_reset_mins (which tick every fetch); keep
// the two in sync if ProviderUsage gains a field.
static bool provider_eq(const ProviderUsage& a, const ProviderUsage& b) {
    return a.session_pct == b.session_pct &&
           a.session_reset_mins == b.session_reset_mins &&
           a.weekly_pct == b.weekly_pct &&
           a.weekly_reset_mins == b.weekly_reset_mins &&
           a.ok == b.ok && a.present == b.present &&
           strcmp(a.status, b.status) == 0;
}

// Publish s_usage to main.cpp only when a field actually changed — a failing
// provider re-emitting identical state every tick must not re-trigger a render.
// reset-minute changes DO count (they tick down each fetch), matching the old
// wire-string comparison; main.cpp's usage_changed() separately ignores resets
// for the idle timer. present is always true — the device always shows a panel
// per provider; a needs-action provider surfaces via the WiFi-page verdict.
static void publish_if_changed(void) {
    bool changed = false;
    for (int i = 0; i < PROVIDER_COUNT; i++)
        if (!provider_eq(s_usage[i], s_pub.providers[i])) { changed = true; break; }
    if (!changed) return;
    for (int i = 0; i < PROVIDER_COUNT; i++) s_pub.providers[i] = s_usage[i];
    s_pub.valid = true;
    s_has_data  = true;
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
static void apply_token(int prov, JsonVariantConst o) {
    Cred& c = s_cred[prov];
    const char* t = o["token"] | (const char*)nullptr;
    if (t && *t) {
        strlcpy(c.token, t, c.token_cap);
        if (c.account) strlcpy(c.account, o["account_id"] | "", c.account_cap);
        token_store_save(prov, c.token, c.account);  // cache to NVS for next boot
        s_tok[prov] = TOK_OK;
        s_need_fetch[prov] = false;
        s_force[prov] = true;
        Serial.printf("[net] broker token ok: prov=%d\n", prov);
    } else if (!o["needs_action"].isNull()) {
        // Explicit needs-action: the user must re-auth. Evict the dead credential
        // from RAM *and* NVS so a reboot can't reload it and replay a guaranteed
        // 401 before rediscovering this state.
        token_store_clear(prov);
        c.token[0] = '\0';
        if (c.account) c.account[0] = '\0';
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
    // Parseable reply = broker is reachable (clears USAGE_BROKER_DOWN).
    s_broker_reachable = true;
    for (int i = 0; i < PROVIDER_COUNT; i++)
        apply_token(i, doc[PROVIDERS[i].broker_key]);
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
static void fetch_claude(int prov) {
    ProviderUsage& u = s_usage[prov];
    WiFiClientSecure client;
    configure_tls(client);
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT);
    http.setTimeout(HTTP_READ_TIMEOUT);
    if (!http.begin(client, ANTHROPIC_URL)) {
        Serial.println("[net] claude begin() failed");
        u.ok = false;
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
    http.addHeader("Authorization", String("Bearer ") + s_cred[prov].token);
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("anthropic-beta", "oauth-2025-04-20");
    http.addHeader("Content-Type", "application/json");
    const String body =
        "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    int code = http.POST(body);
    if (code == 401 || code == 403) {
        on_auth_reject(prov, code);
        u.ok = false;
        http.end();
        return;
    }
    String u5 = http.header("anthropic-ratelimit-unified-5h-utilization");
    String u7 = http.header("anthropic-ratelimit-unified-7d-utilization");
    if (code == 200 && u5.length() && u7.length()) {
        u.session_pct        = roundf(u5.toFloat() * 100.0f);
        u.weekly_pct         = roundf(u7.toFloat() * 100.0f);
        u.session_reset_mins = epoch_to_mins(http.header("anthropic-ratelimit-unified-5h-reset").toInt());
        u.weekly_reset_mins  = epoch_to_mins(http.header("anthropic-ratelimit-unified-7d-reset").toInt());
        String st = http.header("anthropic-ratelimit-unified-status");
        bool limited = (st.length() && st != "allowed") || u.session_pct >= 100;
        strlcpy(u.status, limited ? "limited" : "allowed", sizeof(u.status));
        u.ok = true;
        s_last_update_ms[prov] = millis();
        Serial.printf("[net] claude 200: s=%d w=%d\n", (int)u.session_pct, (int)u.weekly_pct);
    } else {
        // 200-without-headers is a failure: keep last-good values, flag ok=false
        // (a missing/renamed header would otherwise scrape to 0% "allowed").
        u.ok = false;
        Serial.printf("[net] claude POST -> %d (hdrs %d/%d)\n", code, u5.length(), u7.length());
    }
    http.end();
}

// Codex: GET the usage JSON and map the two windows.
static void fetch_codex(int prov) {
    ProviderUsage& u = s_usage[prov];
    WiFiClientSecure client;
    configure_tls(client);
    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT);
    http.setTimeout(HTTP_READ_TIMEOUT);
    if (!http.begin(client, CODEX_URL)) {
        Serial.println("[net] codex begin() failed");
        u.ok = false;
        http.end();
        return;
    }
    http.addHeader("Authorization", String("Bearer ") + s_cred[prov].token);
    // account is the Codex Cred's buffer; guard the generic-prov contract so a
    // future table row whose Cred has account==nullptr can't hit String(nullptr).
    http.addHeader("ChatGPT-Account-Id", s_cred[prov].account ? s_cred[prov].account : "");
    http.addHeader("User-Agent", "clawdmeter");
    int code = http.GET();
    if (code == 401 || code == 403) {
        on_auth_reject(prov, code);
        u.ok = false;
        http.end();
        return;
    }
    if (code == 200) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getStream());
        JsonObject rl = doc["rate_limit"];
        if (err || rl.isNull()) {
            // A 200 without a rate_limit object is a failure, not healthy-with-no-data.
            u.ok = false;
            Serial.printf("[net] codex bad body: %s\n", err ? err.c_str() : "no rate_limit");
        } else {
            u.session_pct = rl["primary_window"]["used_percent"]   | -1;
            u.weekly_pct  = rl["secondary_window"]["used_percent"] | -1;
            long pr = rl["primary_window"]["reset_after_seconds"]   | -1L;
            long sr = rl["secondary_window"]["reset_after_seconds"] | -1L;
            u.session_reset_mins = pr >= 0 ? (int)(pr / 60) : -1;
            u.weekly_reset_mins  = sr >= 0 ? (int)(sr / 60) : -1;
            bool allowed = rl["allowed"] | true;
            bool reached = rl["limit_reached"] | false;
            bool limited = !allowed || reached || u.session_pct >= 100;
            strlcpy(u.status, limited ? "limited" : "allowed", sizeof(u.status));
            u.ok = true;
            s_last_update_ms[prov] = millis();
            Serial.printf("[net] codex 200: cs=%d cw=%d\n", (int)u.session_pct, (int)u.weekly_pct);
        }
    } else {
        u.ok = false;
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
    s_pub             = UsageData{};   // zero → the first fetch always publishes
    for (int i = 0; i < PROVIDER_COUNT; i++) {
        s_last_update_ms[i] = 0;
        s_force[i]          = false;
        s_last_fetch[i]     = 0;
        // Per-provider live slot starts at the "no data yet" sentinel (UI shows
        // "Connecting…"); present is always true post-cutover.
        s_usage[i].session_pct = -1; s_usage[i].session_reset_mins = -1;
        s_usage[i].weekly_pct  = -1; s_usage[i].weekly_reset_mins  = -1;
        strlcpy(s_usage[i].status, "unknown", sizeof(s_usage[i].status));
        s_usage[i].ok = false; s_usage[i].present = true;
    }
    s_broker_resolved   = false;
    s_broker_attempted  = false;
    s_broker_reachable  = false;
    s_broker_last_attempt = 0;
    s_ssid_buf[0]     = '\0';
    s_ip_buf[0]       = '\0';

    // Load cached credentials from NVS. A provider with a stored token starts
    // ready; a missing one starts MISSING and pulls from the broker once online.
    token_store_init();
    for (int i = 0; i < PROVIDER_COUNT; i++) {
        s_cred[i].token[0] = '\0';
        if (s_cred[i].account) s_cred[i].account[0] = '\0';
        s_tok[i] = TOK_MISSING;
        s_need_fetch[i] = true;
        if (token_store_load(i, s_cred[i].token, s_cred[i].token_cap,
                             s_cred[i].account, s_cred[i].account_cap)) {
            s_tok[i] = TOK_OK; s_need_fetch[i] = false;
        }
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
        for (int i = 0; i < PROVIDER_COUNT; i++) s_force[i] = true;
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
    bool need = false;
    for (int i = 0; i < PROVIDER_COUNT; i++) need = need || s_need_fetch[i];
    bool broker_due = need && (!s_broker_attempted ||
                               (now - s_broker_last_attempt) >= (uint32_t)FETCH_INTERVAL_MS);
    if (broker_due) {
        s_broker_attempted   = true;
        s_broker_last_attempt = now;
        fetch_tokens();
        return;
    }

    // --- Otherwise fetch at most ONE provider with a usable token ------------
    // Lower provider index takes priority on a tick where several are due (Claude
    // before Codex); the next gets serviced on a later tick. A provider without a
    // usable token is skipped (its data stays "Connecting…" / last-good); the
    // broker path above is what recovers it.
    for (int i = 0; i < PROVIDER_COUNT; i++) {
        if (s_tok[i] != TOK_OK) continue;
        if (!(s_force[i] || (now - s_last_fetch[i]) >= (uint32_t)FETCH_INTERVAL_MS)) continue;
        PROVIDERS[i].fetch(i);
        s_last_fetch[i] = now;  // measure the interval from fetch start, not end
        s_force[i] = false;
        publish_if_changed();
        break;  // at most one blocking TLS fetch per tick
    }
}

net_state_t net_get_state(void) { return s_state; }

bool net_get_usage(UsageData* out) {
    if (!s_has_data) return false;
    *out = s_pub;
    s_has_data = false;  // clear-on-read
    return true;
}

void net_request_refresh(void) {
    for (int i = 0; i < PROVIDER_COUNT; i++) s_force[i] = true;
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
uint32_t net_last_update_ms(void) {
    // Most-recent fetch across providers — drives the WiFi page's "Updated" line.
    uint32_t m = 0;
    for (int i = 0; i < PROVIDER_COUNT; i++)
        if (s_last_update_ms[i] > m) m = s_last_update_ms[i];
    return m;
}

usage_health_t net_provider_health(int prov) {
    // One provider's verdict, in priority order. Auth/token problems outrank the
    // data-freshness states: they're actionable and explain missing data. Each
    // provider is independent — one needing re-auth never affects the other's row.
    if (prov < 0 || prov >= PROVIDER_COUNT) return USAGE_OFFLINE;  // guard the array indexing
    if (s_state != NET_ONLINE) return USAGE_OFFLINE;
    if (s_tok[prov] == TOK_NEEDS_ACTION) return USAGE_NEEDS_ACTION;
    if (s_need_fetch[prov]) {
        if (s_broker_attempted && !s_broker_reachable) return USAGE_BROKER_DOWN;
        return USAGE_NO_TOKEN;
    }
    if (s_last_update_ms[prov] == 0)  return USAGE_NO_DATA;
    if (millis() - s_last_update_ms[prov] <= USAGE_STALE_MS) return USAGE_LIVE;
    return USAGE_STALE;
}
