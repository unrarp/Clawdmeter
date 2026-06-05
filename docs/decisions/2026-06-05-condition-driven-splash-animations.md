---
date: 2026-06-05
module: splash
tags: [splash, animation, low-battery, condition-driven, power, name-match, usage-rate]
---

# Condition-driven splash animations (low-battery), keyed by name in firmware

## Context
The splash picker selects animations purely by usage-rate tier
(`usage_rate_group()` → idle/normal/active/heavy). The `idle-low-battery`
clip shipped as just another member of the `idle` tier, so it played at full
charge — meaningless. We wanted it to appear *only* on a real low battery,
i.e. a clip whose trigger is a device condition, not a usage rate.

## Decision / Solution
A condition-driven clip is identified in firmware by its `name` string and
pulled out of the rate rotation, then shown only while its condition holds.
For low battery (`firmware/src/splash.cpp`):

- `LOW_BATT_ANIM_NAME = "idle low battery"`, matched in `resolve_group_lists()`
  → recorded as `low_batt_anim` and **skipped** when building the tier lists.
- `battery_is_low()` gates on `has_battery && !charging && 0 ≤ pct ≤ LOW_BATT_PCT`
  (15%). It reads a cached battery value pushed in via `splash_set_battery()`
  (mirrors the existing `ui_update_battery()` push from the main loop), not its
  own I2C read.
- `splash_pick_for_current_rate()` checks `battery_is_low()` first and forces
  `low_batt_anim`; `splash_set_battery()` re-picks immediately when the
  condition flips, so engage/clear doesn't wait for the 20 s rotation timer.

A condition clip must be excluded from **every** selection path, not just the
rate rotation:
1. `resolve_group_lists()` — keep it out of the tier lists.
2. `splash_next()` — the manual button cycle on `SCREEN_SPLASH` must skip it
   (otherwise a press shows it out of context / cycles away from it when low).
3. Boot — seed `splash_set_battery()` *before* the first `ui_show_screen(SCREEN_SPLASH)`
   in `main.cpp` so a low-battery boot opens on the right clip instead of
   flashing a rate clip until the first loop re-pick.

## Why
Name-match keeps the whole feature firmware-local: no pipeline/schema change,
no `.bin`/header regeneration, and the animation set stays freely editable
(`animations.json`) without a parallel "role" registry to maintain. The
`group` value of a condition clip is simply ignored.

## Alternatives considered
- **Dedicated `group`/`role` field in `animations.json`** (e.g. `"group":
  "low_battery"`), propagated through `make_wrappers.py` → `frames_to_data.py`
  → `gen_splash_header.py` into a struct field. Rejected: `frames_to_data.py`
  validates `group` against `VALID_GROUPS` and would need widening; it forces a
  data/header regen for what is purely a selection-logic change; and it spreads
  one concept across four pipeline stages. Worth revisiting only if condition
  clips multiply enough that name strings become unwieldy.
- **Leaving it in the idle tier** — the original behavior; rejected because the
  clip then plays at full charge.

## Prevention
The name coupling fails silently: rename the clip in `animations.json` without
updating `LOW_BATT_ANIM_NAME` and the override just stops. `resolve_group_lists()`
logs `low-battery clip '…' not in set` at boot when the name no longer
matches — check the serial log if a condition clip stops appearing.

## Related
- `.claude/rules/splash.md` — the condition-clip rule bullet.
- Future "sleeping before power-off" clip (discussed, not built) would follow
  this same pattern, but needs a new pre-shutdown grace window in `idle.cpp`
  (today `idle_tick()` calls `power_hal_shutdown()` instantly at
  `IDLE_TIMEOUT_MS`, leaving nothing to animate). See
  `2026-06-03-axp-power-off-vs-deep-sleep`.
