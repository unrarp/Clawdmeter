---
paths:
  - "firmware/src/splash.cpp"
  - "firmware/src/splash.h"
  - "firmware/src/usage_rate.cpp"
  - "firmware/src/usage_rate.h"
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

- **The tight-viewBox bbox probe (cairosvg) ignores a CSS-class `opacity: 0`; the
  capture (Chromium) honors it.** Decorations hidden at rest render *opaque* in the
  probe, inflating the viewBox so the creature is framed small and off-center, while
  the captured frames correctly hide them (seen: wizard `magic-star`, grooving
  `note-left/right`). Strip such elements from the probe only via `animations.json`
  `bbox_strip` (CSS class names) → `make_wrappers.py::strip_classes`, applied to the
  probe text but **not** the wrapper, so they still animate. Default path probes the
  on-disk SVG via `url=`, so untouched clips stay byte-identical.

- **Each bin captures only a fixed `period_ms` (1200 ms) slice of the clip's CSS
  timeline starting at t=0 — not the whole animation.** A key moment that lands later
  never appears (seen: builder's sweat at ~3.3 s of a 6 s loop, thinking's 3rd dot at
  1.22 s, conducting's stream once its staggered pixels launch ~1.8 s). Shift the
  window with `animations.json` `start_ms`: `capture_frames.mjs` sets
  `currentTime = start_ms + (i/N)*period`. WAAPI wraps per-iteration, so any offset
  (even > the clip's own duration) is valid.

- **Clips are framed independently (tight viewBox + `xMidYMid`), so baselines do NOT
  line up across the set.** To drop a clip onto the shared ground line (~y112 of 128)
  without rescaling, use `animations.json` `y_offset` (user units, positive = down);
  `make_wrappers.py` subtracts it from the viewBox origin `ny`, which translates
  content down without changing scale (seen: grooving, typing).

- **Animation rate tier is per-provider: feed each provider's `session_pct` into its
  own ring and take the max group — never collapse to the max-level provider first.**
  `usage_rate_sample(prov, pct)` keeps a ring per `PROV_*`; `usage_rate_group()`
  returns the max tier across them (`usage_rate.cpp`). Collapsing to `max(session_pct)`
  then tracking that one number's rate lets a flat-but-high provider (Codex idle at 55%)
  mask a climbing one (Claude) → splash stuck on idle. Two more load-bearing constraints:
  (1) the upstream 5h-utilization header is **whole-percent quantized**, so the window
  (`RING_SIZE`×~60s ≈ 9 min) must stay ≤~10 min — a single 1% crossing must clear Normal
  (1%÷9min ≈ 0.11); **widen the window, don't lower thresholds**. (2) A provider that
  stops reporting must age out to idle (`millis() - newest > 3×MIN_WINDOW` in `group_for`)
  or its ring freezes at the last tier. → see
  docs/decisions/2026-06-05-usage-rate-per-provider-tiering.md

## Related decisions

- `2026-06-03-splash-animations-build-artifact` — gitignored artifact, binary format,
  pre-build hook.
- `2026-06-05-usage-rate-per-provider-tiering` — per-provider rate rings, the
  quantization/window-length bound, and why thresholds weren't lowered.
- `2026-06-05-condition-driven-splash-animations` — name-keyed condition clips and the
  exclusion points they need.
