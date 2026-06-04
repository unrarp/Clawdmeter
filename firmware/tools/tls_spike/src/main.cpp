// TLS / provider-API pre-validation spike — see ../platformio.ini for the why.
//
// Boots, joins WiFi, then every 30s runs the exact device-direct calls the
// token-broker plan depends on, over cert-validated TLS:
//   [claude]  POST https://api.anthropic.com/v1/messages   -> scrape rate-limit headers
//   [codex]   GET  https://chatgpt.com/backend-api/wham/usage -> parse rate_limit JSON
//   [401]     the Claude call with a corrupted token        -> must return 401/403
// All three (+ TLS=bundle-validated) must PASS before the Phase-3 net.cpp rewrite.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#if __has_include("spike_config.h")
  #include "spike_config.h"
#else
  #error "Copy include/spike_config.example.h to include/spike_config.h (or run the generator in README.md)."
#endif

// Embedded full root-CA bundle (200 CAs) from the core's libmbedtls.a. The
// pioarduino 55.03.38-1 core has only the TWO-arg setCACertBundle(ptr, size) —
// no single-arg overload. Symbols verified against libmbedtls.a (esp32s3).
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

// 1 = validate the chain against the embedded bundle (the production path we are
//     here to prove). 0 = setInsecure(), diagnostic only — isolates "handshake
//     vs cert" if validation fails. Never ship 0.
#ifndef TLS_VALIDATE
#define TLS_VALIDATE 1
#endif

static void configure_tls(WiFiClientSecure &c) {
#if TLS_VALIDATE
  c.setCACertBundle(x509_crt_bundle_start,
                    (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
#else
  c.setInsecure();
#endif
}

static void wait_for_wifi() {
  Serial.printf("[wifi] connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
    if (millis() - start > 30000) {
      Serial.println("\n[wifi] TIMEOUT after 30s — check SSID/password. Retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start = millis();
    }
  }
  Serial.println();
  Serial.printf("[wifi] CONNECTED  ip=%s  rssi=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

// POST a 1-token message; on 200 dump the unified rate-limit headers. Returns
// the HTTP status (>0) or a negative HTTPClient/spike error.
static int claude_usage(const char *token, bool dump) {
  WiFiClientSecure client;
  configure_tls(client);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);  // pair both — a stalled response otherwise hangs net_tick
  if (!http.begin(client, "https://api.anthropic.com/v1/messages")) {
    Serial.println("[claude]   begin() failed");
    return -1000;
  }
  static const char *keep[] = {
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-5h-reset",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-7d-reset",
    "anthropic-ratelimit-unified-status",
  };
  http.collectHeaders(keep, sizeof(keep) / sizeof(keep[0]));
  http.addHeader("Authorization", String("Bearer ") + token);
  http.addHeader("anthropic-version", "2023-06-01");
  http.addHeader("anthropic-beta", "oauth-2025-04-20");
  http.addHeader("Content-Type", "application/json");
  const String body =
    "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,"
    "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
  int code = http.POST(body);
  if (code > 0 && dump) {
    if (code == 200) {
      Serial.printf("[claude]   5h util=%s reset=%s | 7d util=%s reset=%s | status=%s\n",
                    http.header("anthropic-ratelimit-unified-5h-utilization").c_str(),
                    http.header("anthropic-ratelimit-unified-5h-reset").c_str(),
                    http.header("anthropic-ratelimit-unified-7d-utilization").c_str(),
                    http.header("anthropic-ratelimit-unified-7d-reset").c_str(),
                    http.header("anthropic-ratelimit-unified-status").c_str());
    } else {
      String b = http.getString();
      if (b.length() > 160) b = b.substring(0, 160) + "...";
      Serial.printf("[claude]   body=%s\n", b.c_str());
    }
  }
  http.end();
  return code;
}

// GET the Codex usage JSON and parse the two windows. Returns HTTP status (>0)
// or a negative error (-2000 = parse failure).
static int codex_usage(const char *token, const char *account) {
  WiFiClientSecure client;
  configure_tls(client);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, "https://chatgpt.com/backend-api/wham/usage")) {
    Serial.println("[codex]    begin() failed");
    return -1000;
  }
  http.addHeader("Authorization", String("Bearer ") + token);
  http.addHeader("ChatGPT-Account-Id", account);
  http.addHeader("User-Agent", "clawdmeter-spike");
  int code = http.GET();
  if (code == 200) {
    String b = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, b);
    if (err) {
      Serial.printf("[codex]    JSON parse FAILED: %s\n", err.c_str());
      code = -2000;
    } else {
      JsonObject rl = doc["rate_limit"];
      int  p  = rl["primary_window"]["used_percent"]        | -1;
      long pr = rl["primary_window"]["reset_after_seconds"]  | -1L;
      int  s  = rl["secondary_window"]["used_percent"]       | -1;
      long sr = rl["secondary_window"]["reset_after_seconds"]| -1L;
      Serial.printf("[codex]    5h used=%d%% reset_in=%lds | 7d used=%d%% reset_in=%lds | plan=%s\n",
                    p, pr, s, sr, (const char *)(doc["plan_type"] | "?"));
    }
  } else if (code > 0) {
    String b = http.getString();
    if (b.length() > 160) b = b.substring(0, 160) + "...";
    Serial.printf("[codex]    body=%s\n", b.c_str());
  }
  http.end();
  return code;
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB-CDC enumerate so we don't miss early lines
  Serial.println("\n=== TLS / provider-API spike ===");
  Serial.printf("[tls] mode=%s\n", TLS_VALIDATE ? "bundle-validated" : "INSECURE (diagnostic)");
  wait_for_wifi();
}

void loop() {
  Serial.println("\n---- TLS round ----");
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] dropped — reconnecting");
    wait_for_wifi();
  }

  int c = claude_usage(CLAUDE_TOKEN, true);
  bool claude_ok = (c == 200);
  Serial.printf("[claude]   POST /v1/messages -> %s (%d)\n", claude_ok ? "PASS" : "FAIL", c);

  int x = codex_usage(CODEX_ACCESS_TOKEN, CODEX_ACCOUNT_ID);
  bool codex_ok = (x == 200);
  Serial.printf("[codex]    GET wham/usage -> %s (%d)\n", codex_ok ? "PASS" : "FAIL", x);

  // forced-401: corrupt the token. The device's whole refetch trigger is "did a
  // provider call come back 401/403?" — prove we can actually see it.
  String bad = String(CLAUDE_TOKEN) + "BUSTED";
  int f = claude_usage(bad.c_str(), false);
  bool f401 = (f == 401 || f == 403);
  Serial.printf("[401]      corrupted-token -> %s (%d, want 401/403)\n", f401 ? "PASS" : "FAIL", f);

  Serial.printf("[summary]  tls=%s  claude=%s  codex=%s  401-detect=%s\n",
                TLS_VALIDATE ? "bundle" : "INSECURE",
                claude_ok ? "ok" : "X", codex_ok ? "ok" : "X", f401 ? "ok" : "X");
  delay(30000);
}
