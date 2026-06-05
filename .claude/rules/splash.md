---
paths:
  - "firmware/src/splash.cpp"
  - "firmware/src/splash.h"
  - "tools/svg_pipeline/**"
  - "tools/svg_anim_data/**"
  - "firmware/scripts/gen_splash.py"
---

# Splash animation rules

- **`splash_animations.h` is a gitignored build artifact — never commit or hand-edit
  it.** The PlatformIO pre-build hook (`firmware/scripts/gen_splash.py`) regenerates it
  from `tools/svg_anim_data/` (`.bin` + `manifest.json`, the source of truth) before
  every `pio run`; a fresh checkout needs no manual step. → see
  `docs/decisions/2026-06-03-splash-animations-build-artifact.md`

- **`gen_splash_header.generate()` must read `manifest["palette_size"]`, not the
  hardcoded `PALETTE_SIZE = 24`.** `frames_to_data.py` writes `palette_size` into the
  manifest; using the constant while `params.palette` in `animations.json` differs
  writes wrong-sized palette arrays and a mismatched `#define SPLASH_PALETTE_SIZE`
  against the new `.bin` indices — silent color corruption at runtime.

- **Condition-driven clips are keyed by their `name` string, not by `group`.** The
  low-battery clip (`LOW_BATT_ANIM_NAME = "idle low battery"`) is matched by name in
  `resolve_group_lists()`, pulled out of the rate-tier rotation, and shown only while
  `battery_is_low()`. It must be excluded from **every** selection path (tier rotation
  *and* `splash_next()`'s manual cycle), with battery state seeded before the first
  `ui_show_screen(SCREEN_SPLASH)`. **Foot-gun:** renaming it in `animations.json`
  without updating `LOW_BATT_ANIM_NAME` silently disables the override — boot logs
  `low-battery clip '…' not in set`; check the serial log if it stops appearing. → see
  `docs/decisions/2026-06-05-condition-driven-splash-animations.md`

- **Capture frames with a fixed-viewBox wrapper; never crop each frame to its own
  bounding box.** CSS-animated elements (data bits, sparks) shift the per-frame bbox,
  so a per-frame `getbbox` crop re-centers the creature and the body visually drifts.
  Rasterize the whole fixed wrapper (e.g. 288×288) and scale that. See
  `tools/svg_pipeline/make_wrappers.py` + `frames_to_data.py` (`on_panel`).

## Related decisions

- `2026-06-03-splash-animations-build-artifact` — gitignored artifact, binary format,
  pre-build hook.
- `2026-06-05-condition-driven-splash-animations` — name-keyed condition clips and the
  exclusion points they need.
