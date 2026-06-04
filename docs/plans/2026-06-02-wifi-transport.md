---
date: 2026-06-02
module: daemon, firmware
status: implemented
depends-on: 2026-06-02-codex-usage.md (lands first)
tags: [wifi, transport, ble-removal, http, daemon, ui]
---

> **Partially superseded (2026-06-04):** The "Rejected: on-device OAuth /
> self-sufficiency" conclusion below has been reversed by
> [`docs/plans/2026-06-04-token-broker-self-sufficient.md`](2026-06-04-token-broker-self-sufficient.md).
> Empirical testing found a static Claude `setup-token` escape and a
> forgiving Codex refresh window, making device-direct TLS + a lightweight
> token broker cheap enough to be practical. The original reasoning is
> preserved as history; the current architecture is described in the
> 2026-06-04 plan.

# Plan: move the data transport from BLE to WiFi (local HTTP)

## Goal

Replace the Bluetooth link between the host daemon and the device with **plain
HTTP over the LAN**. The device becomes a **WiFi-only usage gauge**: it joins
your network, fetches the usage JSON from the daemon over HTTP, and renders it.
Bluetooth is removed entirely.

The daemon **stays** on a host machine — it still reads Claude Code's OAuth
token from `~/.claude/.credentials.json`, still polls `/api/oauth/usage` every
300s, still respects the rate limit. Only the *delivery* changes: instead of
writing GATT characteristics over BLE, it serves the cached usage JSON over
HTTP, and the device pulls it.

**Why not self-sufficient (device polls Anthropic directly)?** Evaluated and
rejected as too complex for the payoff — see [§ Rejected: on-device
OAuth](#rejected-on-device-oauth-self-sufficiency). The decisive fact: the
subscription OAuth access token expires ~hourly and rotates, with no static-key
escape (the usage endpoint needs the `user:profile` subscription scope; a
platform `sk-ant-…` key can't see subscription usage). Going daemon-free would
require an on-device OAuth refresh client, a *device-dedicated* token lineage
(so it doesn't fight the laptop's Claude Code for the rotating refresh token),
on-device TLS, and a long-lived account credential stored in flash — plus a ToS
question. The WiFi-only-with-daemon design below sidesteps **all** of that: the
device only ever talks to the *local daemon* over plain HTTP, so there is **no
on-device TLS, no OAuth, no secret at rest**.

## Decisions locked (user, 2026-06-02)

- **Drop self-sufficiency.** Daemon stays on a host; device never contacts
  `api.anthropic.com`. (Self-sufficiency reconsidered only if the daemon
  becomes a burden — then move the daemon to a small always-on host like a Pi,
  not onto the device.)
- **Drop BLE entirely**, including the HID button-controller (voice-mode Space /
  mode-toggle Shift+Tab). The device is "just a usage gauge" to the user, so the
  keyboard half is removed, not preserved. Reversible via git history.
- **Transport: device pulls.** Daemon runs a dead-simple HTTP server serving its
  last-cached JSON; the device is a pure HTTP **client**. Rejected: daemon-push
  (device-as-server + mDNS responder on the MCU) — more firmware, slower first
  paint, no benefit here.
- **WiFi credentials: hardcoded + recompile** for v1 (simplest working version).
  Captive-portal provisioning is a fast-follow (§G).
- **Wire format unchanged.** The compact 14-key JSON the firmware already parses
  (`{"s","sr","w","wr","st","ok","sp","cp","cs","csr","cw","cwr","cst","cok"}`, §C.4 of the daemon) is reused verbatim over HTTP.
  This keeps `parse_json()` and `UsageData` untouched.
- **Daemon auth: none.** Bind `0.0.0.0`, serve `/usage` unauthenticated on the
  LAN — usage percentages are low-sensitivity and this is a trusted home
  network. No token, no per-interface binding (§B.1).
- **Discovery: mDNS `<host>.local`.** Device resolves the daemon host's mDNS
  name (DHCP-proof, no manual reconfig). No static-IP fallback in v1 (§F).
- **One daemon.** Retire `claude-usage-daemon.sh`; a single platform-agnostic
  Python HTTP daemon runs on both Linux and macOS (§B.3).
- **WiFi screen tap-zone = diagnostics**, not an action. Read-only view: SSID,
  IP, RSSI, last-update age. No forget/reconnect button in v1 (§D).
- **Trivial defaults** (not separately litigated): daemon port `8080`;
  device→daemon fetch cadence `45s`; remove the now-unused
  `~/.config/claude-usage-monitor/` BLE-MAC-cache dir on install.
- **Sequencing: lands after Codex** (`2026-06-02-codex-usage.md`). This plan
  assumes the as-built Codex end state — 4 screens (`SPLASH/USAGE/CODEX/WIFI`),
  the 14-key two-provider wire payload, a **static** cycle (`ui_cycle_screen`:
  `USAGE → CODEX → WIFI`; presence flags drive within-page "Connecting…"
  state and the splash max-%, **not** page hide/show — there is no
  `screen_enabled()`), and the parameterized provider screen
  (`init_provider_screen`/`ui_update_provider`/`claude_w`/`codex_w`). It
  **deletes** the BLE-specific work Codex adds (see §E): the `ble.cpp`
  "clawdmeter" rebrand, the §C BLE-MTU validation + `setMTU` insurance, and the
  `test_macos_connect.py` extension — all moot once BLE is gone. References
  below are by symbol, not line number (Codex is actively shifting these files).

## A. Architecture / data flow

```text
  ┌─────────────────────────── host machine ───────────────────────────┐
  │  Claude Code  ──refreshes──▶  ~/.claude/.credentials.json           │
  │                                      │ read accessToken             │
  │                                      ▼                              │
  │  daemon: poll api.anthropic.com/api/oauth/usage every 300s (HTTPS)  │
  │                                      │ cache compact JSON           │
  │                                      ▼                              │
  │  daemon HTTP server:  GET /usage  ──▶ {"s":..,"sr":..,"w":..,...}   │
  └──────────────────────────────────────┬──────────────────────────────┘
                                          │  plain HTTP over LAN
                                          ▼
  ┌──────────────────────────────── device ────────────────────────────┐
  │  WiFi STA  →  HTTP GET http://<daemon-host>:<port>/usage            │
  │     on boot (instant first paint) + every ~30–60s                  │
  │  parse_json() → UsageData → existing UI render                     │
  └─────────────────────────────────────────────────────────────────────┘
```

Two independent cadences, decoupled by the daemon's cache:

- **Daemon → Anthropic: 300s** (rate-limit bound, unchanged).
- **Device → daemon: ~30–60s** (cheap, local; never hits Anthropic). The device
  can also refetch on demand (e.g. on WiFi reconnect) with no rate-limit concern
  — this replaces the BLE "refresh request" (REQ characteristic) for free.

**WiFi is SoC-level, not board-specific.** Both reference boards are ESP32-S3,
which has WiFi built in. The networking code lives in **shared** firmware
(`net.{h,cpp}`), not under `boards/<name>/`, and needs **no HAL and no
`#ifdef BOARD_*`** — consistent with rule #10. (If a future board were ever
WiFi-less, gate it behind a `BoardCaps.has_wifi` flag then; not needed now.)

## B. Daemon — serve HTTP, delete BLE, collapse the two platform ports

Post-Codex there are **two** daemons, each now polling **two providers** (Claude
+ Codex) with presence flags and per-provider last-good caching, delivered over
**two BLE stacks**:

- `daemon/claude-usage-daemon.sh` — Linux, bluez/dbus (`dbus-monitor`,
  `bluetoothctl`, GATT writes).
- `daemon/claude_usage_daemon.py` — macOS port, `bleak`/CoreBluetooth.

**The WiFi switch collapses these into one platform-agnostic HTTP server.** The
two-provider poll/merge/presence/last-good logic is **transport-agnostic and
kept verbatim** — only the delivery changes (BLE GATT write → HTTP serve). All
the OS-specific Bluetooth glue (MAC cache, `dbus-monitor` pipe, `bluetoothctl
remove`, the bleak CoreBluetooth scan-exclusion workaround — documented in
`CLAUDE.md` §Daemon) is **deleted**, not ported.

### B.1 New canonical daemon

Recommend consolidating on **one Python daemon** (`claude_usage_daemon.py`),
since Python's `http.server` makes serving trivial and it already runs on both
OSes. Structure:

- **Poll loop** (asyncio or background thread): every `POLL_INTERVAL=300`, read
  token, GET `/api/oauth/usage` (HTTPS, `httpx`), map to compact wire JSON, store
  in a module-level `last_payload` guarded by a `threading.Lock`. Keep
  `POLL_FAIL_BACKOFF=60`. (Note: the macOS daemon reads from Keychain, not
  `~/.claude/.credentials.json` — the Python daemon already handles both; keep
  that logic intact.)
- **HTTP server** (`http.server.ThreadingHTTPServer`, separate thread): `GET
  /usage` → `200 application/json` with `last_payload` (lock-protected read), or
  `503` if `last_payload` is `None` (device shows "connecting"). Optionally `GET
  /healthz`.
- **Bind**: `0.0.0.0:8080`, **unauthenticated** (decision: trusted home LAN,
  usage % only). No token header, no per-NIC binding.
- **Advertise mDNS**: the daemon registers its host under mDNS so the device can
  reach it as `<host>.local` (§F) — on Linux this is Avahi (already running on
  most desktops); confirm the hostname the device should target.
- **Delete**: all BLE code paths, `SAVED_ADDR_FILE` MAC cache, scan/connect,
  the refresh-notify subscriber.

### B.2 systemd / launchd

- `daemon/claude-usage-daemon.service`: drop `Requires=bluetooth.target` /
  `After=bluetooth.target`; repoint `ExecStart` to the Python daemon. (Today's
  `ExecStart=DAEMON_PATH` placeholder is filled at install — unchanged
  mechanism.)
- `daemon/com.user.claude-usage-daemon.plist`: unchanged except it now launches
  the HTTP daemon.

### B.3 Retire the bash daemon

`claude-usage-daemon.sh` is **deleted** (decision: one daemon). The
compact-JSON mapping logic
(`five_hour→s/sr`, `seven_day→w/wr`, `st = limited if s>=100`) moves verbatim
into the Python poll loop — it already exists there for the macOS port, so this
is mostly deletion on the bash side.

Also rewrite `install.sh` (Linux) and `install-mac.sh` (macOS): both currently
install the bash daemon and set up Bluetooth dependencies/permission priming.
Replace with Python daemon install steps and drop all BLE prerequisites.

## C. Firmware — `net.{h,cpp}` (new shared module)

Mirror the **data-side** shape of `ble.h` so `main.cpp` / `ui.cpp` change
minimally. New `firmware/src/net.h`:

```c
// net_state_t: NET_DISCONNECTED, NET_CONNECTING, NET_ONLINE
void        net_init(void);              // WiFi.begin() with stored creds
void        net_tick(void);              // drive reconnect + periodic GET
net_state_t net_get_state(void);         // current WiFi/HTTP state
bool        net_has_data(void);          // true after first successful GET; resets on read
const char* net_get_data(void);          // last good JSON body (fed to parse_json)
void        net_request_refresh(void);   // force an immediate GET (e.g. reconnect)
const char* net_get_ssid(void);          // diagnostics view
const char* net_get_ip(void);            // diagnostics view
int         net_get_rssi(void);          // diagnostics view (dBm)
uint32_t    net_last_update_ms(void);    // millis() of last good GET → staleness/age display
```

- **WiFi**: `WiFi.mode(WIFI_STA)` + `WiFi.begin(ssid, pass)`; non-blocking
  reconnect in `net_tick()` (no busy-wait — the loop already runs LVGL ticks).
- **HTTP GET**: `HTTPClient` against `http://<DAEMON_HOST>:<PORT>/usage`. Plain
  HTTP — **no `WiFiClientSecure`, no certs** (local daemon). On `200`, copy body
  into a buffer and latch `has_data`. On non-200 / timeout, leave last-good in
  place and surface a stale/offline state.
- **Cadence**: GET immediately on `ONLINE` transition (instant first paint),
  then every `FETCH_INTERVAL_MS` (~30–60s).

### C.4 Data path — unchanged

`parse_json()` and the `UsageData` struct (`data.h`) are **not touched** — the
14-key two-provider wire format is just a JSON body, carried over HTTP verbatim. Only the *source* swaps (by symbol — Codex is shifting these
lines):

- `ble_init()` → `net_init()`
- `ui_update_ble_status(ble_get_state(), …)` → `ui_update_wifi_status(…)`
- `ble_tick()` → `net_tick()`
- `if (ble_has_data())` → `if (net_has_data())`
- `parse_json(ble_get_data(), …)` → `parse_json(net_get_data(), …)`
- `ble_send_ack()` / `ble_send_nack()` → remove (no equivalent; HTTP pull doesn't ack)
- `#include "ble.h"` in `ui.h` → `#include "net.h"` (ui.h currently pulls in ble.h)

## D. Firmware — screen: `SCREEN_BLUETOOTH` → `SCREEN_WIFI`

Post-Codex the cycle is a static 4-stop rotation (`Splash → Claude → Codex →
Bluetooth`). This plan only renames the last stop; **the `SCREEN_CODEX` page
stays untouched** — the new cycle is `Splash → Claude → Codex → WiFi`.

- Rename `SCREEN_BLUETOOTH` → `SCREEN_WIFI` everywhere it appears: the `ui.h`
  enum, the `ui_cycle_screen()` cases (`USAGE → CODEX → BLUETOOTH → USAGE`), the
  `ui_show_screen()` case, and the `prev_non_splash_screen` default. Leave the
  `SCREEN_CODEX` cases alone.
- Rewrite (not just rename) `init_bluetooth_screen()` → `init_wifi_screen()`: the
  current function builds static labels and wires a `ble_clear_bonds()` click
  handler; the WiFi screen needs mutable labels (SSID/IP/RSSI/age) and no click
  action. Budget accordingly.
- Status fields: replace the MAC line (`lbl_ble_mac`) with the **diagnostics
  view** (decision): SSID, IP, RSSI, and last-update age.
- `ui_update_ble_status(state, name, mac)` → `ui_update_wifi_status(state, ssid,
  ip, rssi, last_update_age)` (declared in `ui.h`, called from `main.cpp`'s
  status-update sites).
- The "Reset Bluetooth" tap-zone (`reset_zone`) becomes **read-only /
  non-acting** in v1 — the screen *is* the diagnostics view; there is no
  forget/reconnect button yet. When the captive portal lands (§G), revisit
  adding a "Forget network" action here. (Simplest: drop the `reset_zone` event
  handler and let the labels render statically.)
- Bluetooth icon (`icon_bluetooth_data`) → a WiFi icon (generate via
  `tools/png_to_lvgl.js` from a Lucide `wifi` PNG, white tint — see CLAUDE.md
  Icons section). Keep RGB565 (opaque zone) unless it overlaps the splash.

## E. Firmware — deletions (BLE / HID)

- Delete `firmware/src/ble.{h,cpp}` (or exclude from `build_src_filter`). This
  also discards Codex's §H `ble.cpp` rebrand (`DEVICE_NAME`/`setManufacturer` →
  "clawdmeter") — moot once there's no BLE advertisement. The "clawdmeter"
  identity now lives only in branding/docs, not a broadcast name (the pull-model
  device advertises nothing).
- Remove the HID button wiring (`ble_keyboard_press(0x2C…)` Space,
  `ble_keyboard_press(0x2B,0x02)` Shift+Tab) and the `ble_keyboard_*` calls. The
  physical buttons keep their **local** roles (cycle screens / cycle splash
  animations); only the *HID-keystroke-to-host* behavior is removed.
- Drop the now-moot Codex BLE work: §C BLE-MTU validation + the optional
  `setMTU(247)` insurance, and the `test_macos_connect.py` payload-size check
  (HTTP has no ATT-PDU cap; the file is deleted anyway, below).
- `firmware/platformio.ini`: drop `h2zero/NimBLE-Arduino` from **all three envs**
  (`waveshare_amoled_216`, `waveshare_amoled_18`, `waveshare_amoled_216_c6`) plus
  any `CONFIG_BT_NIMBLE_*` build flags. Confirm nothing else pulls NimBLE in.
  Frees flash/RAM.
- Grep for stragglers: `ble_`, `NimBLE`, `HID`, `Claude Controller`, `0x1812`,
  `keyboard` across `firmware/src/`.

## F. Config — what the firmware needs baked in (v1)

A single `firmware/src/net_config.h` (gitignored or with placeholder values):

```c
#define WIFI_SSID      "..."
#define WIFI_PASSWORD  "..."
#define DAEMON_HOST    "my-laptop.local"   // mDNS name, or a static IP
#define DAEMON_PORT    8080
#define FETCH_INTERVAL_MS  45000
```

- `DAEMON_HOST` is the host's mDNS `<hostname>.local` (decision: mDNS, no
  static-IP fallback in v1) — survives DHCP IP changes. ESP32 resolves `.local`
  via `<ESPmDNS.h>`. If mDNS proves flaky on the target network, the value can be
  set to a literal IP without code changes (the field is just a host string).
- **Do not commit real WiFi credentials.** Add `net_config.h` to `.gitignore`
  with a checked-in `net_config.example.h`.

## G. Provisioning (fast-follow, not v1)

Replace hardcoded creds with a **captive portal** (WiFiManager-style): on first
boot / no stored creds, the device starts a SoftAP; you connect from a phone and
enter SSID/password (+ optionally `DAEMON_HOST`); creds persist to NVS. The
"Reset WiFi" tap-zone (§D) clears NVS and re-enters the portal. Deferred to keep
v1 small.

## Build / QA

- **mDNS pre-validation**: before integrating into firmware, confirm ESP32
  resolves `<host>.local` on the target network with a standalone sketch that
  does `WiFi.begin()` + `MDNS.begin()` + `WiFi.hostByName("my-laptop.local",
  ip)` and prints the result over serial. If resolution fails, fall back to a
  static IP in `net_config.h`.
- Build all three envs: `pio run -d firmware -e waveshare_amoled_216`,
  `-e waveshare_amoled_18`, `-e waveshare_amoled_216_c6` (BLE deletion and
  NimBLE removal touch all three).
- **Daemon, standalone first** (per "validate before integration"): run the
  Python daemon, `curl http://localhost:8080/usage`, confirm the 14-key
  two-provider JSON matches the expected payload byte-for-byte (same
  keys/presence/rounding/`st` logic).
- **Device**: flash, watch serial for WiFi join + first GET; confirm the WiFi
  screen shows SSID/IP and both provider screens (Claude + Codex, presence-gated)
  paint real numbers.
- **Screenshot QA** the WiFi + Claude + Codex screens via `./screenshot.sh`
  (temporarily default-boot each via the `ui_show_screen(SCREEN_SPLASH)` line per
  CLAUDE.md, then revert).
- **Failure paths**: kill the daemon → device shows stale/offline, recovers when
  daemon returns; pull WiFi → reconnect works without reboot.
- **Docs cleanup**: update `CLAUDE.md` §Daemon, §Architecture, and §Critical
  gotchas for the new transport; update `README.md` (remove BLE pairing steps,
  HID button section, GATT UUIDs); update `install.sh` / `install-mac.sh`;
  delete `daemon/test_macos_connect.py` (BLE-only test tool).

## Open questions

All v1 design questions are resolved (see Decisions locked). Remaining items are
deferred work, not blockers:

- **mDNS target hostname** — confirm the exact `<hostname>.local` the device
  should resolve (the host running the daemon). Settled at implementation time.
- **Captive-portal provisioning (§G)** — the only deferred feature; lands as a
  fast-follow after the hardcoded-creds v1 works end-to-end. Adds a "Forget
  network" action to the diagnostics screen (§D).

## Rejected: on-device OAuth (self-sufficiency)

> **This rejection has been reversed by the 2026-06-04 token-broker plan.**
> A static Claude `setup-token` eliminates the hourly-rotation problem, and
> the Codex token refresh window proved forgiving enough for device-direct
> use with a token broker. See
> [`docs/plans/2026-06-04-token-broker-self-sufficient.md`](2026-06-04-token-broker-self-sufficient.md).
> The original analysis below is preserved as history.

Captured so it isn't re-litigated. Making the device daemon-free requires it to
poll `api.anthropic.com` itself, which forces **all** of:

1. An on-device OAuth **refresh client** (token expires ~hourly, rotates).
2. A **device-dedicated** OAuth session (its own refresh-token lineage via a
   separate login), or the device and the laptop's Claude Code fight over the
   shared rotating refresh token and one gets logged out.
3. On-device **TLS** (`WiFiClientSecure` + CA cert) to reach Anthropic.
4. A long-lived **account credential stored in flash** (theft/dump risk unless
   flash encryption + secure boot are enabled).
5. A **ToS** posture question (standalone appliance holding subscription creds).

The WiFi-with-daemon design here avoids 1–5 entirely: the device's only network
peer is the local daemon, over plain HTTP. Revisit self-sufficiency only by
relocating the *daemon* to a small always-on host, never by putting credentials
on the device.

## Sources

- Live token shape: `~/.claude/.credentials.json` → `claudeAiOauth`
  {accessToken, refreshToken, expiresAt (~1h), scopes incl. `user:profile`,
  subscriptionType: max}. Confirms hourly rotation + no static-key path.
- Current transport: `firmware/src/ble.{h,cpp}` (data service + HID),
  `firmware/src/main.cpp` (init/tick/consume), `daemon/claude-usage-daemon.sh`
  (Linux/bluez), `daemon/claude_usage_daemon.py` (macOS/bleak); bluez fragility
  to be deleted is documented in `CLAUDE.md` §Daemon.
- Wire format: the daemon's compact-JSON map ↔ `parse_json()` in
  `firmware/src/main.cpp` / `firmware/src/data.h`.
