# TLS / provider-API spike

Throwaway Phase-1 gate for the token-broker plan
([`docs/plans/2026-06-04-token-broker-self-sufficient.md`](../../../docs/plans/2026-06-04-token-broker-self-sufficient.md)).
Isolated PlatformIO project — no display/LVGL/board HAL, just the Arduino core's
`WiFi` + `WiFiClientSecure` + `HTTPClient` and `ArduinoJson`. **Not part of the
firmware build.**

## What it answers

Can the ESP32-S3 do **cert-validated** HTTPS to the two providers and get the
usage data the device-direct design needs? Four checks per round:

- `[tls]` — chain validated against the embedded 200-CA bundle (not `setInsecure`)
- `[claude]` — `POST /v1/messages` returns 200 and the `anthropic-ratelimit-unified-*` headers are readable
- `[codex]` — `GET wham/usage` returns 200 and the `rate_limit` JSON parses
- `[401]` — a corrupted token comes back 401/403 (the refetch trigger)

All four must PASS before starting the Phase-3 `net.cpp` rewrite. If `[tls]`
fails but the rest pass with `-DTLS_VALIDATE=0` (`setInsecure`), the problem is
cert handling, not connectivity — report that.

## Run

```bash
# Generate include/spike_config.h from your local creds (WiFi + Codex from
# ~/.codex/auth.json + Claude from ~/.claude/.credentials.json). Writes the
# file directly; prints no secrets:
python3 - <<'PY'
import json, os, re, pathlib
root = pathlib.Path(__file__).resolve().parents[0] if '__file__' in dir() else pathlib.Path('firmware/tools/tls_spike')
nc = pathlib.Path('firmware/src/net_config.h').read_text()
g  = lambda k: re.search(rf'#define\s+{k}\s+"([^"]*)"', nc).group(1)
claude = json.load(open(os.path.expanduser('~/.claude/.credentials.json')))['claudeAiOauth']['accessToken']
cx = json.load(open(os.path.expanduser('~/.codex/auth.json')))['tokens']
out = f'''#pragma once
#define WIFI_SSID          "{g("WIFI_SSID")}"
#define WIFI_PASSWORD      "{g("WIFI_PASSWORD")}"
#define CLAUDE_TOKEN       "{claude}"
#define CODEX_ACCESS_TOKEN "{cx["access_token"]}"
#define CODEX_ACCOUNT_ID   "{cx["account_id"]}"
'''
pathlib.Path('firmware/tools/tls_spike/include/spike_config.h').write_text(out)
print("wrote include/spike_config.h (Claude=live accessToken, Codex=auth.json)")
PY

# Build + flash + monitor (from repo root):
pio run -d firmware/tools/tls_spike -e tls_spike_s3 -t upload -t monitor
#   ESP32-C6 board? use -e tls_spike_c6
#   wrong port?      append --upload-port /dev/ttyACM0  (or /dev/cu.usbmodem101)
```

The generator uses the **live** `~/.claude/.credentials.json` accessToken for
convenience — it works for `/v1/messages` and expires in hours, which is fine
for a spike run now. Production uses a `claude setup-token` instead (1-year,
inference-scoped). To test the real setup-token path, run `claude setup-token`
and replace `CLAUDE_TOKEN` in `include/spike_config.h`.

## Reading the log

A passing round ends with:

```
[summary]  tls=bundle  claude=ok  codex=ok  401-detect=ok
```

Anything other than `ok` (or `tls=INSECURE`) is a gate failure — capture the
preceding `[claude]`/`[codex]`/`[401]` lines and stop before the firmware work.
