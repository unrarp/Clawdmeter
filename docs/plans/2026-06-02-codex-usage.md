---
date: 2026-06-02
module: daemon, firmware
status: approved, not started
tags: [codex, openai, usage, wham-usage, ble-wire, ui, animations]
---

# Plan: add Codex (ChatGPT-plan) usage alongside Claude

## Goal

The device currently shows only Claude usage. Add **OpenAI Codex usage** as a
second provider: a dedicated, OpenAI-branded Codex screen mirroring the Claude
Usage screen (5-hour + weekly windows), fed by the same host daemon over the
same BLE channel.

**Single-account users are first-class.** Many people have only one of the two
accounts. A provider the user doesn't have (no creds file) must have its page
**omitted from the screen cycle entirely** — not shown as an empty/"unavailable"
panel. This is symmetric: a Codex-only user sees no Claude page; a Claude-only
user sees no Codex page (today's behavior). "Not used" (creds absent → hide page)
is distinct from "temporarily failing" (present but mid-401/429/offline → keep
page, show last-good). See §B.6 (presence detection) and §D.2 (dynamic cycle).

## Decisions locked (user, 2026-06-02)

- **Separate screen** per provider (not a combined 4-panel or per-window-compare
  layout). Cycle becomes `Splash → Claude → Codex → Bluetooth`, with provider
  pages hidden when that provider is absent (§D.2).
- **OpenAI-branded** Codex screen: OpenAI logo + accent **ChatGPT green
  `#10A37F`** (`THEME_ACCENT_CODEX`).
- **Animations v1 = spinner only.** Pixel-pet creatures deferred (§F).
- **Presence = creds file exists**, page shown immediately ("Connecting…" until
  first data) — §B.6.
- **Stale data**: keep last-good numbers + a subtle stale hint after several
  consecutive failures (§D.4).
- **Splash creature driver**: max session % across *present* providers (§D.6).
- **Rebrand to `clawdmeter`** — advertised identity only (BLE name, manufacturer
  string, daemons' connect-by-name). Config paths / systemd unit / splash art
  unchanged (§H).

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
- **Each provider is optional.** The daemon must run with *either* provider
  alone (today it aborts a poll if the Claude token is unreadable — that becomes
  per-provider: an unreadable/absent provider is simply marked not-present, the
  other still polls and writes). If neither is present the daemon still runs the
  BLE/HID side; it just emits both providers absent.
- **Independent poll + last-good cache:** keep an in-memory per-provider result.
  Each cycle, attempt every *present* provider; update only the one(s) that
  succeeded; always emit a combined payload built from the cached values. A
  present provider that succeeded before but failed this cycle keeps its
  last-good values (the device shows the last known numbers, not 0%). A present
  provider that has **never** succeeded is emitted present-but-not-ok (page shown
  as "Connecting…").
- Keep `POLL_INTERVAL=300`.

### B.6 Presence detection (drives page hide/show)

- A provider is **present** iff its creds file exists:
  `~/.claude/.credentials.json` (Claude) / `~/.codex/auth.json` (Codex). The
  daemon sets the presence flags (`sp` / `cp`, §C) from this check each cycle.
  **Decided:** creds-file existence is the presence signal — the page appears as
  soon as the file is there and shows "Connecting…" until the first successful
  poll (no "require ≥1 successful poll" gate).
- Presence is **independent of poll success** — a present provider that's
  throttled/expired/offline stays present (its page stays in the cycle, showing
  last-good or "Connecting…"). Only an absent creds file hides the page.
- Re-checked each cycle so adding/removing an account is picked up without a
  daemon restart (the firmware reconciles the visible cycle on each payload, §D.2).

### B.2 Bash daemon (`daemon/claude-usage-daemon.sh`)

- `poll()` (currently builds the whole payload from one inline `python3` block,
  ~lines 223–253) gains a second `curl` and the inline Python merges **two**
  JSON inputs into the combined wire dict. Pass both provider bodies (and a
  per-provider success flag) into the Python block; have it emit the cached/
  merged result. Last-good caching across calls lives in the bash loop (e.g.
  write each provider's last-good JSON to a tmp file the Python block reads).

### B.3 macOS Python daemon (`daemon/claude_usage_daemon.py`)

- **No "merge block" exists here** — the structure is `poll_api(token) -> dict`
  (one `API_URL`, a `reset_minutes` helper) feeding `Session.write_payload()`.
  Concrete edits: add a `CODEX_API_URL` + a Codex token/account reader; turn
  `poll_api` into two independent fetches (or a second `poll_codex()`); merge
  the two dicts (with module-level last-good caches) before `write_payload`.
  This is a real refactor, not a line-for-line port.

### B.4 Unavailable / missing creds

- Absent `~/.codex/auth.json` → emit `cok:false` and placeholder Codex fields.
  The firmware renders this as an explicit unavailable state (§D), not 0%.

### B.5 Auth failure handling (401 vs 429)

- **429/5xx** (throttle): existing `POLL_FAIL_BACKOFF=60` path, per provider.
- **401** (expired token, CLI hasn't refreshed): emit `cok:false` + take the
  `POLL_FAIL_BACKOFF` path — do **not** retry every `TICK` (5s), or an expired
  token would hammer the auth endpoint. (Self-refresh remains the deferred
  fallback from §A.)

## C. Wire format (backward-compatible, full-snapshot)

Current `{"s","sr","w","wr","st","ok"}` → add Codex keys **and per-provider
presence flags**:

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

`sp`/`cp` decide **page hide/show**; `ok`/`cok` decide fresh-vs-last-good *within*
a shown page. **Backward-compatible defaults** (firmware, §D.1): missing `sp` →
`true`, missing `cp` → `false`. So an old daemon payload (no presence keys, no
Codex keys) behaves exactly as today: Claude page shown, Codex page absent.

Payload grows ~55 → ~115 bytes. Existing keys untouched.

**Transport reality (corrected):** both daemons write with **Write-Without-
Response** (bash D-Bus `WriteValue`, macOS Bleak `write_gatt_char(..., response
=False)`); the RX characteristic is `WRITE | WRITE_NR`. There is **no** long-
write / prepared-write reassembly on this path — a write is a single ATT PDU
capped at `MTU − 3`. It works today only because the negotiated MTU (Linux ~517,
macOS ~185) already exceeds the ~55-byte payload, and `ble_init()` never calls
`setMTU` (relies on the peer-negotiated value). 110 bytes is under both, so it's
low-risk — but **must be validated**, not assumed:

- **Validation step (do before firmware UI work):** extend
  `daemon/test_macos_connect.py` (currently writes the old 6-key payload) to send
  a full ~110-byte two-provider payload and confirm the device parses it without
  truncation. Add the same as a one-off check on Linux.
- Optional insurance: `NimBLEDevice::setMTU(247)` in `ble_init()` before
  `start_advertising()`.

## D. Firmware

### D.1 Data + parsing

- **`data.h`** — extend `UsageData` with Codex fields, a per-provider poll-ok
  flag, and **per-provider presence flags**: `codex_session_pct`,
  `codex_session_reset_mins`, `codex_weekly_pct`, `codex_weekly_reset_mins`,
  `codex_status[16]`, `codex_ok`, plus `claude_present`, `codex_present`. Keep the
  struct **flat** (two providers don't justify a nested `Provider` struct).
- **`main.cpp` `parse_json()`** (lines 107–113) — parse the new keys with the
  same `| default` idiom; presence uses the backward-compat defaults from §C:
  `claude_present = doc["sp"] | true`, `codex_present = doc["cp"] | false`,
  `codex_ok = doc["cok"] | false`. The existing `valid` flag stays "any
  successful parse".

### D.2 Screen plumbing — presence-aware, three hardcoded call sites (the enum count does NOT auto-wire these)

The cycle is **dynamic**: a provider page is in the cycle only when that provider
is present. `SCREEN_SPLASH` and `SCREEN_BLUETOOTH` are always present, so the
worst case (no provider) still cycles Splash + Bluetooth.

- **`ui.h`** — insert `SCREEN_CODEX` between `SCREEN_USAGE` and `SCREEN_BLUETOOTH`.
- **`screen_enabled(screen)` helper** — `SPLASH`/`BLUETOOTH` always true;
  `SCREEN_USAGE` ⇔ `claude_present`; `SCREEN_CODEX` ⇔ `codex_present`. Driven by
  the latest parsed `UsageData`. Before the first payload, presence falls back to
  the §C defaults (Claude shown, Codex hidden) so boot behavior is unchanged.
- **`ui_cycle_screen()`** (584) — currently hardcodes `USAGE ↔ BLUETOOTH`; rewrite
  to advance to the **next `screen_enabled` non-splash screen**, skipping disabled
  provider pages.
- **`ui_show_screen()`** (562) — add a `case SCREEN_CODEX:` (reveal container +
  logo swap, §E). If asked to show a disabled screen, redirect to the next enabled
  one.
- **`ui_tick_anim()`** (521) — the spinner gate `current_screen != SCREEN_USAGE`
  must also allow `SCREEN_CODEX`, writing to that screen's own spinner label.
- **Reconcile on each payload** — when new data changes presence, recompute; if
  the **currently shown** screen just became disabled (e.g. account removed), snap
  to the next enabled screen. Likewise `prev_non_splash_screen` must never be left
  pointing at a disabled screen.

### D.3 Screen construction — parameterize, don't clone

`init_usage_screen()` is already factored over `make_usage_panel()`; the only
per-provider differences are the title, the accent color, and which widget
pointers receive the panels. **Refactor first, then add the second screen:**

- Replace the flat usage-screen widget globals with a small `ProviderWidgets`
  struct (container, the two bars, the pct/label/reset labels per window, the
  spinner label). Keep two instances: `claude_w`, `codex_w`.
- Extract `init_provider_screen(scr, title, accent, ProviderWidgets* w)` doing
  what `init_usage_screen` does today; call it twice in `ui_init()`.
- Extract `ui_update_provider(const ProviderWidgets* w, session_pct,
  session_reset, weekly_pct, weekly_reset, available)`; `ui_update()` calls it
  once per provider. Reuse `format_reset_time()` and `pct_color()` verbatim.
- **Rename the existing screen's title "Usage" → "Claude"** to match the stated
  cycle (otherwise the UX reads `Usage → Codex → Bluetooth`).

### D.4 Within-page states (for a *present* provider only)

An absent provider has **no page** (§D.2), so there's no "unavailable panel" to
render. For a present provider whose latest poll hasn't succeeded:

- **Never succeeded** (present-but-not-ok, no cached data): dimmed `—%` + a
  "Connecting…" reset line.
- **Succeeded before, failing now** (last-good cached, §B): show the last-good
  numbers, and after several consecutive failed polls add a **subtle stale hint**
  (e.g. dim the reset line / a small stale marker) so old data is distinguishable
  from fresh. Do not drop to `0% / ---`. The failure threshold for the hint is a
  daemon-side count surfaced via a per-provider flag (or inferred device-side from
  unchanged data + elapsed time — pick one; daemon-side count is simpler).

Define the exact strings/colors here.

### D.5 Layout

The compact (368×448) `compute_layout()` usage-screen fields (`usage_panel_h`,
`usage_panel_gap`, `usage_bar_y`, `usage_reset_y`, fonts) are **reused as-is** for
the Codex screen — a conscious decision, the geometry is identical. No new
`Layout` fields unless the Codex screen later needs distinct geometry.

### D.6 Splash creature driver (max of present providers)

The splash creature's intensity is fed by `usage_rate_sample()` — today
`main.cpp` (~line 310) calls `usage_rate_sample(usage.session_pct)` (Claude
session % only). Change it to feed the **max session % across present providers**
so it works for Claude-only, Codex-only, and both: e.g.
`usage_rate_sample(max_present_session_pct(usage))`, where the helper returns the
greatest of `{session_pct if claude_present, codex_session_pct if codex_present}`
(and a sensible idle default when neither is present). The group-change →
`splash_pick_for_current_rate()` logic is otherwise unchanged.

## E. Branding (OpenAI)

- New asset `logo_openai.h` — 80×80 **RGB565A8** (same format as `logo.h`),
  generated from an official OpenAI mark PNG via `tools/png_to_lvgl.js`.
  **Build-order dependency:** generate this header *before* ui.cpp `#include`s it,
  or the build fails on an undefined symbol.
- The per-screen logo swap is **new code** (today `ui_show_screen` only hides/
  shows the single `logo_img`; it never calls `lv_image_set_src`). Add a second
  module-level `lv_image_dsc_t logo_openai_dsc`, initialized in `ui_init()` via
  **`init_icon_dsc_rgb565a8`** (the alpha-aware initializer — using the non-alpha
  `init_icon_dsc` would corrupt the image via wrong stride/`data_size`). In
  `ui_show_screen()` set `logo_img`'s source: Claude mark on Claude/Bluetooth,
  OpenAI mark on Codex.
- **`theme.h`** — add `THEME_ACCENT_CODEX` = **ChatGPT green `#10A37F`** (theme.h
  exports `THEME_*`; the `COL_*` names are ui.cpp-local aliases). Add `#define
  COL_ACCENT_CODEX THEME_ACCENT_CODEX` alongside the other aliases in ui.cpp, used
  for the Codex screen title accent + spinner.
- Bars keep the semantic `pct_color()` (green/amber/red) on both screens.

## F. Animations

Two separate animations exist in the Claude UI, very different in cost:

1. **Usage-screen spinner** (`lbl_anim`, `ui_tick_anim`). **Cheap:** the
   parameterized screen (§D.3) gives each provider its own spinner label, and the
   `ui_tick_anim` gate change (§D.2) drives it. No assets. **Ship in v1.**
2. **Splash pixel-art creatures** (claudepix 20×20 / indexed-palette engine).
   Codex has an analog — **"Codex Pets" / Codemon** (`/hatch` skill at
   `github.com/openai/skills`, community archive `codex-arena.fun`) — but the
   format does **not** match. Claude art is one HTML page per animation exposing
   a 20×20 `PRESET` (`tools/scrape_claudepix.js` ingests it directly). Codex pets
   are **192×208 full-color RGBA WebP atlases** (9 states), so they require an
   offline resize → quantize (≤10 colors) → frame-extract pipeline producing a
   generated `codex_animations.h`. **Deferred** — heavier than the Claude
   copy-paste flow and lossy at 20×20.

## G. Docs to update (don't ship the doc drift)

- **`CLAUDE.md`** — currently describes `ui.{h,cpp}` as a "3-screen UI (splash,
  usage, bluetooth)" and documents the 6-key wire format and daemon behavior.
  Update to 4 screens, the 12-key wire format, the two-provider daemon, and the
  new `logo_openai.h` / `THEME_ACCENT_CODEX`. Also reflect the `clawdmeter`
  rebrand (§H) — the device name appears in CLAUDE.md and the daemon docs.

## H. Rebrand to `clawdmeter` (advertised identity only)

Scope is the **advertised identity**, not a full rename — config paths, the
systemd unit (`claude-usage-daemon`), the cached-MAC path
(`~/.config/claude-usage-monitor/ble-address`), and the existing Anthropic splash
art stay as-is.

- **`firmware/src/ble.cpp`** — `#define DEVICE_NAME "Claude Controller"` →
  `"clawdmeter"` (drives `NimBLEDevice::init()` + the advertising `setName()`).
  `hid_dev->setManufacturer("Anthropic")` → `"clawdmeter"` (neutral).
- **Both daemons** — `DEVICE_NAME="Claude Controller"` → `"clawdmeter"` (the
  connect-by-name constant used for first-run discovery).
- **Re-pairing note:** the firmware BLE MAC is factory-burned and unchanged, so
  MAC-cached reconnects still work; but the *name* changes, so OS-side HID
  pairings may show the new name / need a one-time re-pair, and any name-based
  discovery before a cache exists now matches `clawdmeter`. Call this out in the
  daemon docs.
- The per-screen logos already brand each provider screen (Claude mark / OpenAI
  mark); the only remaining Anthropic-specific visual is the splash creature art,
  which stays for now (neutral splash art is a deferred follow-up, tied to §F.2).

## Build / QA

- Build **all three** envs that ship the shared UI/BLE/data code:
  `waveshare_amoled_18` (live board), `waveshare_amoled_216`, and
  `waveshare_amoled_216_c6`.
- Per-screen visual verification via `screenshot.sh`: temporarily default-boot
  `SCREEN_CODEX` in `main.cpp` (the `ui_show_screen(SCREEN_SPLASH)` line), capture,
  Read the PNG, iterate, **revert before commit**.
- Wire-size validation: the full ~110-byte payload check from §C
  (`daemon/test_macos_connect.py` + a Linux one-off) before relying on it.

## Open questions

All v1 product decisions are resolved (see "Decisions locked"). Remaining items
are implementation-time tuning or deferred follow-ups:

- Stale-hint threshold — how many consecutive failed polls before the stale hint
  shows, and whether it's a daemon-side count or device-side inference (§D.4).
  Pick during implementation.
- Codex `wham/usage` rate-limit tolerance — start at 300s, measure, relax only if
  justified.
- Codex token self-refresh — deferred fallback; only needed if the CLI stops
  refreshing `auth.json` near expiry (§A / §B.5).
- **Deferred follow-ups:** the pixel-pet pipeline + the exact OpenAI "codemon
  feed" URL (§F.2), and neutral splash art for the rebrand (§H).

## Sources

- https://help.openai.com/en/articles/11369540-using-codex-with-your-chatgpt-plan
- https://github.com/openai/codex/issues/10869 (`wham/usage` is the CLI's usage endpoint)
- https://github.com/openai/skills/blob/main/skills/.curated/hatch-pet/SKILL.md (pet sprite format)
- https://codex-arena.fun/ (community Codemon archive)
- https://www.testingcatalog.com/openai-adds-animated-pets-and-config-imports-to-codex/
