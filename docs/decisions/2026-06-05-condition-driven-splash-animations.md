---
date: 2026-06-05
module: splash
tags: [splash, animation, low-battery, condition-driven, power, name-match, usage-rate]
---

# Condition-driven splash animations (low-battery), keyed by name in firmware

## Context
The splash picker selects animations purely by usage-rate tier
(`usage_rate_group()` → idle/normal/active/heavy). The `idle-low-battery` clip
shipped as just another `idle`-tier member, so it played at full charge —
meaningless. We wanted it to appear *only* on a real low battery, i.e. a clip whose
trigger is a device condition, not a usage rate.

## Decision
A condition-driven clip is identified in firmware by its `name` string, pulled out of
the rate rotation, and shown only while its condition holds. For low battery
(`firmware/src/splash.cpp`):

- `LOW_BATT_ANIM_NAME = "idle low battery"`, matched in `resolve_group_lists()`,
  recorded as `low_batt_anim`, and **skipped** when building the tier lists.
- `battery_is_low()` gates on `has_battery && !charging && 0 ≤ pct ≤ 15%`, reading a
  cached value pushed via `splash_set_battery()` (mirrors `ui_update_battery()`), not
  its own I2C read.
- `splash_pick_for_current_rate()` checks `battery_is_low()` first; `splash_set_battery()`
  re-picks immediately on a condition flip, so engage/clear doesn't wait for the
  rotation timer.

A condition clip must be excluded from **every** selection path: the tier lists
(`resolve_group_lists()`), the manual button cycle (`splash_next()`), and boot — seed
`splash_set_battery()` *before* the first `ui_show_screen(SCREEN_SPLASH)` in `main.cpp`
so a low-battery boot opens on the right clip.

## Why
Name-match keeps the feature firmware-local: no pipeline/schema change, no
`.bin`/header regen, and the animation set stays freely editable (`animations.json`)
without a parallel "role" registry. A condition clip's `group` value is simply ignored.

## Alternatives considered
- **Dedicated `group`/`role` field in `animations.json`** — rejected: forces widening
  `frames_to_data.py`'s `VALID_GROUPS` validation, forces a data/header regen for a
  pure selection-logic change, and spreads one concept across four pipeline stages.
  Revisit only if condition clips multiply enough that name strings get unwieldy.
- **Leaving it in the idle tier** — the original bug; plays at full charge.

## Prevention
The name coupling fails silently: rename the clip in `animations.json` without updating
`LOW_BATT_ANIM_NAME` and the override just stops. `resolve_group_lists()` logs
`low-battery clip '…' not in set` at boot when the name no longer matches — check the
serial log if a condition clip stops appearing.

## Related
- `.claude/rules/splash.md`. A future "sleeping before power-off" clip would follow this
  pattern but needs a pre-shutdown grace window in `idle.cpp` (today `idle_tick()` calls
  `power_hal_shutdown()` instantly at timeout). See `2026-06-03-axp-power-off-vs-deep-sleep`.
