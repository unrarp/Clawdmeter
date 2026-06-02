---
date: 2026-06-02
module: daemon, firmware
status: in progress
tags: [codex, openai, usage, wham-usage, ble-wire, ui, animations]
---

# Plan: add Codex (ChatGPT-plan) usage alongside Claude

## Goal

The device currently shows only Claude usage. Add **OpenAI Codex usage** as a
second provider: a dedicated, OpenAI-branded Codex screen mirroring the Claude
Usage screen (5-hour + weekly windows), fed by the same host daemon over the
same BLE channel.

**Single-account users are first-class.** Many people have only one of the two
accounts. The screen cycle is fixed and both provider pages always exist; a
provider the user doesn't have (no creds file) shows a clear **"No account"**
panel rather than blank numbers. This is distinct from "temporarily failing"
(present but mid-401/429/offline → keep the last-good numbers, dimmed). See §B.6
(presence) and §D.4 (within-page states).

## Decisions locked (user, 2026-06-02)

- **Separate screen** per provider (not a combined 4-panel or per-window-compare
  layout). The cycle is the fixed `Splash → Claude → Codex → Bluetooth`; both
  provider pages are always present (§D.2).
- **OpenAI-branded** Codex screen: the OpenAI Codex mark + accent **ChatGPT green
  `#10A37F`** (`THEME_ACCENT_CODEX`).
- **Animations v1 = spinner only.** Pixel-pet creatures deferred (§F).
- **Presence = creds file exists.** Drives the "No account" message on a page,
  not whether the page exists (§B.6, §D.4).
- **Stale data**: keep the last-good numbers and dim them once the latest poll
  for that provider fails (§D.4).
- **Splash creature driver**: max session % across present providers (§D.6).

## A. Usage API — verified working

`GET https://chatgpt.com/backend-api/wham/usage` (the same endpoint the Codex
CLI itself polls). Called with live credentials → HTTP 200. Auth mirrors the
Claude path almost exactly:

| | Claude (exists) | Codex (new) |
|---|---|---|
| Token file | `~/.claude/.credentials.json` → `accessToken` | `~/.codex/auth.json` → `tokens.access_token` |
| 2nd header | `anthropic-beta: oauth-2025-04-20` | `ChatGPT-Account-Id: <tokens.account_id>` |
| 5h window | `five_hour` | `rate_limit.primary_window` (18000s) |
| 7d window | `seven_day` | `rate_limit.secondary_window` (604800s) |
| % field | `utilization` (0–100) | `used_percent` (0–100) |
| Reset | `resets_at` (ISO-8601 → minutes math) | `reset_after_seconds` (minutes directly, ÷60) |
| Status | derived | `rate_limit.allowed` / `limit_reached` |

Verified response shape:

```jsonc
"rate_limit": {
  "allowed": true, "limit_reached": false,
  "primary_window":   { "used_percent": 31, "limit_window_seconds": 18000,  "reset_after_seconds": 8806   },
  "secondary_window": { "used_percent": 6,  "limit_window_seconds": 604800, "reset_after_seconds": 494504 }
}
```

This maps cleanly onto the existing session/weekly model — and is slightly
simpler than the Claude path (no ISO-8601 / timezone arithmetic; the reset is
already a relative seconds value).

**Token lifetime:** the Codex `access_token` is a ~10-day JWT. The daemon reads
it from disk each poll, relying on the Codex CLI to refresh `auth.json` before
expiry — the same assumption the Claude path makes about Claude Code. If the
CLI isn't run near expiry, the daemon would eventually need self-refresh via
`auth.openai.com/oauth/token` + the `refresh_token` (present). Deferred fallback,
not a v1 blocker — but the **failure modes are specced now** (§B.5).

**Rate limit:** `wham/usage` tolerance is unverified. Reuse the existing 300s
interval (see `docs/decisions/2026-06-02-api-oauth-usage-rate-limit.md`) and
relax later only if measurement justifies it.

## B. Daemon — two providers, independent failure, last-good merge

The wire payload (§C) is a **full snapshot**: every write replaces all fields on
the device. Two providers polled independently therefore require the daemon to
hold last-good state so one provider's failure never blanks the other's panel.

### B.1 Shared model (both daemons)

- Read Codex creds from `~/.codex/auth.json` (`tokens.access_token`,
  `tokens.account_id`); second request to `wham/usage` with
  `Authorization: Bearer …` + `ChatGPT-Account-Id: …`.
- Map `used_percent` → 0–100 directly; `reset_after_seconds // 60` → minutes
  (no ISO helper for Codex).
- **Each provider is optional.** The daemon runs with *either* provider alone, or
  neither: an unreadable/absent provider is marked not-present and the other still
  polls and writes. If neither is present the daemon still runs the BLE/HID side
  and emits both providers absent.
- **Independent poll + last-good cache:** keep an in-memory per-provider result.
  Each cycle, attempt every present provider; update only the one(s) that
  succeeded; always emit a combined payload built from the cached values. A
  present provider that succeeded before but failed this cycle keeps its
  last-good values (`…ok:false`, so the device dims them). A present provider that
  has **never** succeeded is emitted with `-1` sentinels and `…ok:false` (the
  device shows "Connecting…").
- Keep `POLL_INTERVAL=300`.

### B.6 Presence detection

- A provider is **present** iff its creds file exists:
  `~/.claude/.credentials.json` (Claude) / `~/.codex/auth.json` (Codex). The
  daemon sets the presence flags (`sp` / `cp`, §C) from this check each cycle.
- Presence is **independent of poll success** — a present provider that's
  throttled/expired/offline stays present; only an absent creds file clears the
  flag.
- Re-checked each cycle so adding/removing an account is picked up without a
  daemon restart.

### B.2 Bash daemon (`daemon/claude-usage-daemon.sh`)

- `poll()` gains a second `curl` for Codex; the inline `python3` block merges
  **two** JSON inputs into the combined wire dict. Both provider bodies, their
  per-provider HTTP codes, and the last-good tmp-file paths are passed into the
  Python block, which reads/rewrites the per-provider last-good files and prints
  the merged result.

### B.3 macOS Python daemon (`daemon/claude_usage_daemon.py`)

- Structure is `poll_api(token) -> dict` feeding `Session.write_payload()`. Add a
  `CODEX_API_URL` + a Codex token/account reader; split `poll_api` into
  `poll_claude()` / `poll_codex()`; merge the two dicts (with module-level
  last-good caches) before `write_payload`.

### B.4 Unavailable / missing creds

- Absent `~/.codex/auth.json` → `cp:false` and `-1` Codex fields. The firmware
  renders this as the "No account" panel (§D.4), not `0%`.

### B.5 Auth failure & backoff

- **429/5xx** (throttle) and **401** (expired token, CLI hasn't refreshed): mark
  that provider's `ok`/`cok` false this cycle and take the `POLL_FAIL_BACKOFF=60`
  path — do **not** retry every `TICK` (5s), or an expired token would hammer the
  auth endpoint. (Self-refresh remains the deferred fallback from §A.)
- **Single-timer reconciliation:** the loop has one `LAST_POLL`/backoff timer but
  two providers. It builds + writes a combined snapshot every poll cycle from the
  per-provider last-good cache, and treats the cycle as a **success** (advance the
  timer) if **any present provider** polled OK, or if no provider is present;
  it takes the `POLL_FAIL_BACKOFF` path only when **every present provider
  failed**. The macOS daemon mirrors this with its `last_poll` timer.

## C. Wire format (backward-compatible, full-snapshot)

Current `{"s","sr","w","wr","st","ok"}` → add Codex keys and per-provider presence
flags:

| key | meaning |
|---|---|
| `sp`  | Claude (session-provider) **present** — creds file exists |
| `cp`  | Codex **present** — creds file exists |
| `cs`  | Codex session (5h) used % |
| `csr` | Codex session reset, minutes |
| `cw`  | Codex weekly (7d) used % |
| `cwr` | Codex weekly reset, minutes |
| `cst` | Codex status (`allowed`/`limited`) |
| `cok` | Codex latest poll succeeded |

`sp`/`cp` select the "No account" message for an absent provider; `ok`/`cok`
decide fresh-vs-dimmed within a present page. **Backward-compatible defaults**
(firmware, §D.1): missing `sp` → `true`, missing `cp` → `false`, missing
`cs/csr/cw/cwr` → `-1`, missing `cst` → `"unknown"`. So an old daemon payload (no
presence keys, no Codex keys) behaves exactly as today: Claude page populated,
Codex page shows "No account".

**`-1` is the "no data yet" sentinel** for both providers' pct/reset fields: a
present provider that has never polled OK is sent with `-1`s (and `…ok:false`).
The device renders `pct < 0` as the "Connecting…" state (§D.4), distinct from a
genuine `0%`. The daemon applies this to Claude too (`s = -1` when
present-but-never-OK), giving the Claude page the same "Connecting…" state.

Payload grows ~55 → ~142 bytes. Existing keys untouched.

**Transport reality:** both daemons write with **Write-Without-Response** (bash
D-Bus `WriteValue`, macOS Bleak `write_gatt_char(..., response=False)`); the RX
characteristic is `WRITE | WRITE_NR`. There is **no** long-write / prepared-write
reassembly on this path — a write is a single ATT PDU capped at `MTU − 3`. It
works today only because the negotiated MTU (Linux ~517, macOS ~185) already
exceeds the payload, and `ble_init()` never calls `setMTU` (relies on the
peer-negotiated value). ~142 bytes is under both, so it's low-risk — but **must be
validated**, not assumed:

- **Validation step:** the full ~142-byte two-provider payload is sent via
  `daemon/test_macos_connect.py` (and a one-off Linux write) and the device is
  confirmed to parse it without truncation.
- Optional insurance: `NimBLEDevice::setMTU(247)` in `ble_init()` before
  `start_advertising()`.

## D. Firmware

### D.1 Data + parsing

- **`data.h`** — `UsageData` carries Codex fields, a per-provider poll-ok flag,
  and per-provider presence flags: `codex_session_pct`, `codex_session_reset_mins`,
  `codex_weekly_pct`, `codex_weekly_reset_mins`, `codex_status[16]`, `codex_ok`,
  plus `claude_present`, `codex_present`. The struct is **flat** (two providers
  don't justify a nested `Provider` struct).
- **`main.cpp` `parse_json()`** — parse the new keys with the same `| default`
  idiom and the §C defaults: `claude_present = doc["sp"] | true`,
  `codex_present = doc["cp"] | false`, `codex_ok = doc["cok"] | false`,
  `cs/csr/cw/cwr | -1`. Claude `s`/`w` default to `-1.0f` (the no-data sentinel).
  The existing `valid` flag stays "any successful parse".

### D.2 Screen plumbing — static cycle

The cycle is the fixed `Splash → Claude → Codex → Bluetooth`. Every provider
always has a page; an absent provider renders a "No account" panel (§D.4).

- **`ui.h`** — `SCREEN_CODEX` sits between `SCREEN_USAGE` and `SCREEN_BLUETOOTH`
  (before `SCREEN_COUNT`).
- **`ui_cycle_screen()`** — advances `USAGE → CODEX → BLUETOOTH → USAGE`.
  `SCREEN_SPLASH` is reached only via click/toggle, as today.
- **`ui_show_screen()`** — hides `codex_w.container` in the blanket-hide block and
  reveals it for `SCREEN_CODEX`; swaps `logo_img`'s source to the OpenAI mark on
  `SCREEN_CODEX`, the Claude mark otherwise (§E).
- **`ui_tick_anim()`** — runs on `SCREEN_USAGE` and `SCREEN_CODEX`, writing the
  spinner to the current screen's provider label (`claude_w.anim` / `codex_w.anim`).

### D.3 Screen construction — parameterized over a provider struct

The usage screen is factored over a `ProviderWidgets` struct (container, title,
the two bars, the pct/label/reset labels per window, the spinner label) with two
instances `claude_w` / `codex_w`:

- `init_provider_screen(scr, title, accent, ProviderWidgets* w)` builds one
  screen; `ui_init()` calls it twice — `("Claude", COL_ACCENT)` and
  `("Codex", COL_ACCENT_CODEX)`.
- `ui_update_provider(ProviderWidgets* w, session_pct, session_reset, weekly_pct,
  weekly_reset, present, ok, absent_msg)` renders one provider (§D.4); `ui_update()`
  calls it once per provider. `format_reset_time()` and `pct_color()` are reused.
- The Claude screen's title is **"Claude"** (matching the cycle).

### D.4 Within-page states — flat switch

Each page renders one of four states from a flat switch in `ui_update_provider`,
gating the provider-level absent/connecting decision on the session pct and
applying it to both windows. Every signal comes straight off the latest payload —
no device-side counters or thresholds:

- **Absent** (`present == false`): pct `"--%"` (`COL_DIM`, bar `0`); reset line =
  `absent_msg` (`"No Claude account"` / `"No OpenAI account"`).
- **Connecting** (present, `session_pct < 0`): pct `"--%"`, reset `"Connecting..."`,
  `COL_DIM`.
- **Stale** (present, `session_pct >= 0`, latest `ok`/`cok` false): last-good
  numbers with the reset line recolored `COL_STALE` (`0x6b6a64`). Never drops to
  `0%`.
- **Fresh** (present, `session_pct >= 0`, `ok`/`cok` true): pct color via
  `pct_color`, reset via `format_reset_time` (`COL_DIM`).

Strings are ASCII (bitmap fonts may lack `—`/`…`). `COL_STALE` / `COL_ACCENT_CODEX`
are ui.cpp aliases of `THEME_*` tokens (§E).

### D.5 Layout

The compact (368×448) `compute_layout()` usage-screen fields (`usage_panel_h`,
`usage_panel_gap`, `usage_bar_y`, `usage_reset_y`, fonts) are reused as-is for the
Codex screen — the geometry is identical. No new `Layout` fields unless the Codex
screen later needs distinct geometry.

### D.6 Splash creature driver (max of present providers)

The splash creature's intensity is fed by
`usage_rate_sample(max_present_session_pct(usage))`, where the helper returns the
greatest of `{session_pct if claude_present, codex_session_pct if codex_present}`
(both gated on `>= 0`), with an idle default when neither present provider has
data. The group-change → `splash_pick_for_current_rate()` logic is unchanged.

## E. Branding (OpenAI)

- OpenAI mark added to `logos.h` — the shared logo bitmap header holds both the
  Claude and OpenAI marks, each 80×80 **RGB565A8**. **Source:**
  `lobehub/lobe-icons` `static-svg/icons/codex-color.svg` (MIT), with the white
  background tile (`<path fill="#fff">`) stripped so the purple→blue gradient glyph
  (`#B1A7FF→#7A9DFF→#3941FF`) sits on transparent — matching the colored, tile-less
  Claude creature mark (`assets/logo_80.png`). Pipeline: `sharp` rasterizes the
  tile-stripped SVG to an 80×80 transparent PNG, then it is packed RGB565A8 with
  **no tint** (the gradient is preserved). `logos.h` must define both marks before
  ui.cpp `#include`s it.
- The per-screen logo swap adds a second `lv_image_dsc_t logo_openai_dsc`,
  initialized in `ui_init()` via **`init_icon_dsc_rgb565a8`** (the alpha-aware
  initializer — the non-alpha `init_icon_dsc` would corrupt the image via wrong
  stride/`data_size`). `ui_show_screen()` sets `logo_img`'s source: Claude mark on
  Claude/Bluetooth, OpenAI mark on Codex.
- **`theme.h`** exports `THEME_ACCENT_CODEX` = **ChatGPT green `#10A37F`** and
  `THEME_STALE` = `#6b6a64`; ui.cpp aliases them `COL_ACCENT_CODEX` / `COL_STALE`.
  The Codex accent drives the Codex screen's spinner; bars keep the semantic
  `pct_color()` (green/amber/red) on both screens.

## F. Animations

Two separate animations exist in the Claude UI, very different in cost:

1. **Usage-screen spinner** (`ui_tick_anim`). **Cheap:** the parameterized screen
   (§D.3) gives each provider its own spinner label, and the `ui_tick_anim` gate
   (§D.2) drives it. No assets. **Ship in v1.**
2. **Splash pixel-art creatures** (claudepix 20×20 / indexed-palette engine).
   Codex has an analog — **"Codex Pets" / Codemon** (`/hatch` skill at
   `github.com/openai/skills`, community archive `codex-arena.fun`) — but the
   format does **not** match. Claude art is one HTML page per animation exposing a
   20×20 `PRESET` (`tools/scrape_claudepix.js` ingests it directly). Codex pets are
   **192×208 full-color RGBA WebP atlases** (9 states), so they require an offline
   resize → quantize (≤10 colors) → frame-extract pipeline producing a generated
   `codex_animations.h`. **Deferred** — heavier than the Claude copy-paste flow and
   lossy at 20×20.

## G. Docs to update (don't ship the doc drift)

- **`CLAUDE.md`** — must reflect the 4-screen UI (splash, Claude, Codex,
  bluetooth), the 14-key wire format, the two-provider daemon, and the new
  `logos.h` / `THEME_ACCENT_CODEX`.

## Build / QA

- Build **all three** envs that ship the shared UI/BLE/data code:
  `waveshare_amoled_18` (live board), `waveshare_amoled_216`, and
  `waveshare_amoled_216_c6`.
- Per-screen visual verification via `screenshot.sh`: temporarily default-boot
  `SCREEN_CODEX` / `SCREEN_USAGE` in `main.cpp` (the `ui_show_screen(SCREEN_SPLASH)`
  line), capture, Read the PNG, iterate, **revert before commit**.
- Wire-size validation: the full ~142-byte payload check from §C
  (`daemon/test_macos_connect.py` + a Linux one-off) before relying on it.

## Open questions

All v1 product decisions are resolved (see "Decisions locked"). Remaining items
are implementation-time tuning or deferred follow-ups:

- Codex `wham/usage` rate-limit tolerance — start at 300s, measure, relax only if
  justified.
- Codex token self-refresh — deferred fallback; only needed if the CLI stops
  refreshing `auth.json` near expiry (§A / §B.5).
- **Deferred follow-up:** the pixel-pet pipeline + the exact OpenAI "codemon feed"
  URL (§F.2).

## Sources

- https://help.openai.com/en/articles/11369540-using-codex-with-your-chatgpt-plan
- https://github.com/openai/codex/issues/10869 (`wham/usage` is the CLI's usage endpoint)
- https://github.com/openai/skills/blob/main/skills/.curated/hatch-pet/SKILL.md (pet sprite format)
- https://codex-arena.fun/ (community Codemon archive)
- https://www.testingcatalog.com/openai-adds-animated-pets-and-config-imports-to-codex/
