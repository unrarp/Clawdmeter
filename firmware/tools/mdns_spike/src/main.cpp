// mDNS / WiFi pre-validation spike — see ../platformio.ini for the why.
//
// Boots, joins WiFi, then every 10s tries to resolve the daemon host two ways
// and (optionally) does a raw HTTP GET. Read the serial log to decide whether
// v1 can use mDNS `<host>.local` or must fall back to a static IP.
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>

#if __has_include("spike_config.h")
  #include "spike_config.h"
#else
  #error "Copy include/spike_config.example.h to include/spike_config.h and fill it in."
#endif

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
  Serial.printf("[wifi] CONNECTED  ssid=%s  ip=%s  gw=%s  rssi=%d dBm\n",
                WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.RSSI());
}

// Path 1: lwIP resolver. On Arduino-ESP32 this resolves ".local" ONLY if the
// core was built with mDNS-query support in lwIP; if it returns 0.0.0.0 here
// but path 2 succeeds, that's the signal to prefer MDNS.queryHost() in net.cpp.
static void try_hostbyname() {
  String fqdn = String(DAEMON_HOSTNAME) + ".local";
  IPAddress ip;
  bool ok = WiFi.hostByName(fqdn.c_str(), ip);
  if (ok && ip != INADDR_NONE && uint32_t(ip) != 0) {
    Serial.printf("[resolve] hostByName(\"%s\") -> %s  OK\n", fqdn.c_str(), ip.toString().c_str());
  } else {
    Serial.printf("[resolve] hostByName(\"%s\") -> FAILED\n", fqdn.c_str());
  }
}

// Path 2: explicit mDNS query (hostname WITHOUT ".local"). Returns the IP.
static IPAddress try_queryhost() {
  IPAddress ip = MDNS.queryHost(DAEMON_HOSTNAME, 2000);
  if (uint32_t(ip) != 0) {
    Serial.printf("[resolve] MDNS.queryHost(\"%s\") -> %s  OK\n", DAEMON_HOSTNAME, ip.toString().c_str());
  } else {
    Serial.printf("[resolve] MDNS.queryHost(\"%s\") -> FAILED\n", DAEMON_HOSTNAME);
  }
  return ip;
}

#if HTTP_TEST
static void try_http(IPAddress ip) {
  if (uint32_t(ip) == 0) {
    Serial.println("[http] skipped — no resolved IP this round");
    return;
  }
  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d%s", ip.toString().c_str(), HTTP_TEST_PORT, HTTP_TEST_PATH);
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.begin(url);
  int code = http.GET();
  if (code > 0) {
    String body = http.getString();
    if (body.length() > 120) body = body.substring(0, 120) + "...";
    Serial.printf("[http] GET %s -> %d  (TCP OK)  body=%s\n", url, code, body.c_str());
  } else {
    Serial.printf("[http] GET %s -> ERR %d (%s)\n", url, code, http.errorToString(code).c_str());
  }
  http.end();
}
#endif

void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB-CDC enumerate so we don't miss early lines
  Serial.println("\n=== mDNS / WiFi spike ===");
  wait_for_wifi();
  // MDNS.begin() registers us on the .local namespace; required before queryHost().
  if (MDNS.begin("clawdmeter-spike")) {
    Serial.println("[mdns] responder started");
  } else {
    Serial.println("[mdns] MDNS.begin() FAILED — queryHost may not work");
  }
}

void loop() {
  Serial.println("---- resolve round ----");
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] dropped — reconnecting");
    wait_for_wifi();
  }
  try_hostbyname();
  IPAddress ip = try_queryhost();
#if HTTP_TEST
  try_http(ip);
#endif
  delay(10000);
}
