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

- **Capture SVG frames with a fixed-viewBox wrapper; never crop each frame to its own bounding box.** CSS-animated SVGs have floating elements (data bits, sparks) whose bounding box shifts per frame. Per-frame `getbbox` crop re-centers the creature on each frame's content, causing the body to visually drift. The correct approach: rasterize the whole fixed-size wrapper (e.g. 288×288) and scale that fixed frame to the target grid — creature stays anchored, only the intended animation moves. See `tools/svg_pipeline/make_wrappers.py` (tight-viewBox wrapper) and `tools/svg_pipeline/frames_to_data.py` (`on_panel` function).

## Related decisions

- `2026-06-03-splash-animations-build-artifact` — why splash_animations.h is a gitignored artifact, the binary data format, and the pre-build hook design.
