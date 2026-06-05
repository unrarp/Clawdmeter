---
paths:
  - "firmware/src/splash.cpp"
  - "firmware/src/splash.h"
  - "tools/svg_pipeline/**"
  - "tools/svg_anim_data/**"
  - "firmware/scripts/gen_splash.py"
---

# Splash animation rules

- **`splash_animations.h` is a gitignored build artifact — never commit or hand-edit it.** It is regenerated automatically by the PlatformIO pre-build hook (`firmware/scripts/gen_splash.py`) from the binary data in `tools/svg_anim_data/` before every `pio run`. Source of truth is the `.bin` files + `manifest.json`. On a fresh checkout the first build regenerates the header with no manual step. → see `docs/decisions/2026-06-03-splash-animations-build-artifact.md`

- **`gen_splash_header.generate()` must read `manifest["palette_size"]`, not use the module constant.** `frames_to_data.py` writes `palette_size` into `manifest.json`. If `generate()` uses the hardcoded `PALETTE_SIZE = 24` instead of `manifest["palette_size"]`, changing `params.palette` in `animations.json` will silently write wrong-sized palette arrays and a mismatched `#define SPLASH_PALETTE_SIZE` while the `.bin` indices use the new count — silent color corruption at runtime.

- **Condition-driven clips are keyed by their `name` string in `splash.cpp`, not by `group`.** The low-battery animation (`LOW_BATT_ANIM_NAME = "idle low battery"`) is matched by name in `resolve_group_lists()`: it is pulled out of the usage-rate tier rotation and shown only while the battery is ≤ `LOW_BATT_PCT` and discharging (`battery_is_low()`). Its `group` in `animations.json` is therefore ignored. **Foot-gun:** renaming that animation in `animations.json` without updating `LOW_BATT_ANIM_NAME` silently disables the override (no build error). `resolve_group_lists()` logs `low-battery clip '…' not in set` at boot when the name no longer matches — check the serial log if the clip stops appearing on low battery. A condition clip must be excluded from every selection path — tier rotation *and* `splash_next()`'s manual cycle — and the battery state seeded before the first `ui_show_screen(SCREEN_SPLASH)`. → see docs/decisions/2026-06-05-condition-driven-splash-animations.md

- **Capture SVG frames with a fixed-viewBox wrapper; never crop each frame to its own bounding box.** CSS-animated SVGs have floating elements (data bits, sparks) whose bounding box shifts per frame. Per-frame `getbbox` crop re-centers the creature on each frame's content, causing the body to visually drift. The correct approach: rasterize the whole fixed-size wrapper (e.g. 288×288) and scale that fixed frame to the target grid — creature stays anchored, only the intended animation moves. See `tools/svg_pipeline/make_wrappers.py` (tight-viewBox wrapper) and `tools/svg_pipeline/frames_to_data.py` (`on_panel` function).

## Related decisions

- `2026-06-03-splash-animations-build-artifact` — why splash_animations.h is a gitignored artifact, the binary data format, and the pre-build hook design.
- `2026-06-05-condition-driven-splash-animations` — why low-battery is keyed by name in firmware (not a pipeline `group`/`role` field), and the exclusion points a condition clip needs.
