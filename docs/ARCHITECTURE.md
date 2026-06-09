# clawdmeter Architecture

## Overview

clawdmeter is a desk-side hardware monitor that displays Claude and Codex API
usage on a small AMOLED screen. It has two halves:

- **Firmware** — runs on an ESP32-S3/C6 (PlatformIO + Arduino, LVGL UI). Fetches
  usage directly from each provider's API over TLS (~60s) and renders it.
- **Host token broker** (`daemon/token_broker.py`) — the device queries it only to
  (re)fetch credentials, on first boot and after a provider 401/403.

Three boards are supported today, each living in its own
`firmware/src/boards/<name>/` folder selected by PlatformIO `build_src_filter`, so
shared code never sees board-specific code:

- **AMOLED-2.16** on ESP32-S3
- **AMOLED-1.8** on ESP32-S3
- **AMOLED-2.16-C6** on ESP32-C6

## Key Decisions

| Decision            | Choice                                  | Rationale |
|---------------------|-----------------------------------------|-----------|
| MCU framework       | Arduino via `pioarduino/platform-espressif32` 55.03.38-1 | GFX Library needs Arduino Core 3.x; stock `espressif32` ships 2.x |
| UI toolkit          | LVGL 9.2, config via `-D` build flags   | Mature embedded UI; per-env flags (e.g. `LV_USE_SNAPSHOT=0` on the PSRAM-less C6) |
| Board abstraction   | Runtime `BoardCaps` + compile-time `BOARD_HAS_*` HAL | One firmware, many boards, no `#ifdef BOARD_*` in shared code |
| Board selection     | `build_src_filter` per `[env:...]`      | Compiles shared code + exactly one `boards/<name>/` folder |
| Usage transport     | Device fetches providers directly over TLS; host broker vends only credentials | Broker does no polling/field-mapping; avoids a cumulative-history rate limit (see ADR) |
| Provider model      | Table-driven (`PROV_*` enum + parallel tables) | Add a provider = one enum entry + one row per table + one `fetch_*()` |
| Claude usage source | `POST /v1/messages` (`max_tokens:1`) header scrape | `/api/oauth/usage` 403s for the inference-scoped token (and rate-limits ~1 req/min); rejected (ADR 2026-06-02) |
| Fonts               | Pre-baked LVGL bitmap fonts (no runtime rasterizer) | Smaller flash/RAM; sizes generated offline via `lv_font_conv` |
| Splash animations   | Build-time generated `splash_animations.h` (gitignored artifact) | SVG → frames → palette `.bin` pipeline; pre-build hook regenerates header |
| Power-off (1.8)     | AXP2101 hardware shutdown, not ESP deep sleep | True ~µA off; PWR→PWRON wiring guarantees wake (ADR 2026-06-03) |
| Daemon runtime      | Python stdlib only                      | Zero runtime deps on the host; install scripts make a venv anyway |

## System

Three actors, with one rule that explains the whole shape: **usage data comes
directly from each provider's API over TLS, and the host broker is consulted only
for credentials** — on first boot (empty NVS) or after a provider 401/403. No usage
data ever passes through the broker.

```
                              ┌───────────────────────────────┐
        credentials only      │       Host token broker       │
   ┌──── (first boot, or ─────┤    daemon/token_broker.py     │
   │      provider 401/403)   │  GET /tokens · X-Broker-Key   │
   │      200 ok · 409 reauth └───────────────────────────────┘
   ▼
┌──────────────────────────┐    usage fetch ~60s    ┌───────────────────────────────┐
│     clawdmeter device    │─── direct TLS, per ───▶│         Provider APIs         │
│  ESP32-S3 / C6 + AMOLED  │    provider; mapped    │ api.anthropic.com/v1/messages │
│  net.cpp → ProviderUsage │    → ProviderUsage     │ chatgpt.com/backend-api/wham  │
│  ui.cpp · splash (LVGL)  │◀──── usage figures ────┤            (Codex)            │
│  token_store (NVS cache) │                        └───────────────────────────────┘
└──────────────────────────┘
   WiFi STA · resolves <broker>.local via mDNS · no pairing, no MAC cache
```

## Project Structure

```
clawdmeter/
├── firmware/
│   ├── platformio.ini      # one [env:…] per board; build_src_filter selects the folder
│   ├── scripts/            # gen_splash.py — PlatformIO pre-build hook
│   └── src/
│       ├── main.cpp        # entry point + main loop (boot order: Data & Control Flow)
│       ├── *.cpp / *.h     # shared app: net, ui, splash, usage_rate, idle, token_store
│       ├── data.h          # ProviderUsage/UsageData + PROV_* enum — provider source of truth
│       ├── hal/            # HAL interface (display/touch/input/power/imu); shared code calls only this
│       └── boards/<name>/  # per-board HAL impl, one compiled per env (216 · 18 · 216_c6 · template)
├── daemon/                 # host token broker (stdlib-only) + install scripts
├── tools/                  # asset pipelines: svg_pipeline/ (splash) · png_to_lvgl.js (icons)
├── docs/                   # ARCHITECTURE.md · decisions/ (ADRs) · porting/ · plans/
└── .claude/rules/          # path-scoped rules: boards · daemon · networking · fonts · ui
```

Module responsibilities live in the headers themselves. The two things you extend
— **boards** and **providers** — each have a section below.

## Data & Control Flow

```
boot: board_init() → display → idle/power/imu/touch HAL → lv_init() → net_init() → input HAL → ui_init()
                                                     │
WiFi STA connects → resolve <broker>.local (mDNS)    │
                                                     ▼
   on empty NVS or provider 401/403 ──► GET /tokens (X-Broker-Key)
                                          200 all-ok | 409 some need re-auth
                                                     │
   tokens cached in NVS (token_store) ◄──────────────┘
                                                     │
   every ~60s: fetch_claude / fetch_codex (TLS, direct provider API)
        map result → ProviderUsage (s_usage[prov])   │
                                                     ▼
   usage_rate_sample(prov, pct) per provider → usage_rate_group() = max tier
                                                     │
   splash_pick_for_current_rate(): low-battery clip if battery_is_low(),
        else a clip from the current rate tier       │
                                                     ▼
   the UI renders provider panels + WiFi-page health verdicts;
   idle_tick() dims after ~6 min, powers off after ~12 min (battery only)
```

Two details the diagram glosses over:

- **Token persistence** — tokens are cached in NVS (`token_store`, keyed by
  `prov`), so reboots don't re-hit the broker until a token is actually rejected.
- **TLS** — the embedded CA bundle, connect/read timeouts, and mDNS resolution are
  documented in `.claude/rules/networking.md`.

## Boards & the HAL boundary

The point of the per-board layout is that **shared app code never sees
board-specific code.** It reaches hardware through one small interface — the HAL
(`firmware/src/hal/*.h`: display, touch, input, power, imu) — and each board folder
supplies an implementation. The dependency runs one way:

```
  shared code (main · ui · splash · net · usage_rate · idle)
        │  calls only hal/*.h + board_caps()
        ▼
  hal/*.h   (interface — no implementation)
        ▲
        │  implements
  boards/<name>/*.cpp   (one folder compiled per env)
```

`build_src_filter` compiles shared code plus exactly one `boards/<name>/` folder per
`[env:…]`, so only that board's HAL implementation is ever in the build — shared
code that tried to call into another board wouldn't link.

Hardware differences are handled two ways, **never by branching on *which* board**
(no `#ifdef BOARD_<name>` in shared code):

- **Runtime** — `board_caps()` returns a `BoardCaps` descriptor the shared code
  queries (e.g. the UI reads the second button only when `button_count >= 2`).
- **Compile-time** — a `BOARD_HAS_*` capability macro (e.g. `BOARD_HAS_PSRAM`)
  dead-strips code for peripherals a board lacks. It gates by *capability*, not
  board identity, so shared files (`main.cpp`, `splash.cpp`) may use it too.

A board folder holds:

- `board.h` — pins, I2C addresses, `BOARD_HAS_*` flags
- `board_init.cpp` — pre-display bring-up (Wire + any reset-gating IO expander)
- `caps.cpp` — the `BoardCaps` instance
- one `.cpp` per HAL — display, touch, input, power, imu

**Adding a board** means dropping a new `boards/<name>/` folder (copy
`boards/template/`) and adding an `[env:…]` block — no shared file changes. For the
details:

- The interface a port must implement, the bring-up order, and the capability-flag
  pattern: `docs/porting/`.
- Per-board pins and I2C addresses: `.claude/rules/boards.md` (auto-loads when
  editing `firmware/src/boards/**`).

## Providers

Each usage source (Claude, Codex) is a **row in parallel tables** keyed by the
`PROV_*` enum in `data.h` — there is no per-provider branching anywhere. Everything
that iterates providers (fetch scheduling, UI cycling, WiFi diagnostic rows,
animation catalogs) loops over `PROVIDER_COUNT`. The parallel tables are:

| Table / hook     | Lives in                 | Holds |
|------------------|--------------------------|-------|
| `PROVIDERS[]`    | `net.cpp`                | broker key + the `fetch_*()` that scrapes the provider API into `ProviderUsage` |
| `UI_PROVIDERS[]` | `ui.cpp`                 | display name, logo, accent color, "absent" message, animation catalog |
| broker key       | `daemon/token_broker.py` | where the host reads that provider's credentials |

Two things worth knowing:

- **On-device mapping.** The device fetches each provider directly over TLS and
  maps the response straight into a `ProviderUsage` — there's no intermediate
  wire-JSON format, and the broker never sees usage data (it only vends
  credentials).
- **The build catches a missing row.** A `static_assert(PROVIDER_COUNT == 2, …)` in
  `ui.cpp` guards the one hand-tuned per-board layout array (`wifi_prov_y[]`), so
  adding a third provider fails the build until you give it a layout row — a
  deliberate tripwire, not a bug.

**Adding a provider** means one `PROV_*` enum entry + one row in each table above +
its `fetch_*()`. Walk-through: `docs/porting/adding-a-provider.md`.
