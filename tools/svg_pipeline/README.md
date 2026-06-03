# svg_pipeline

Build pipeline that converts the vendored clawd-tank SVG animations into the
binary data committed in `tools/svg_anim_data/`, which the PlatformIO pre-build
hook then uses to regenerate `firmware/src/splash_animations.h` at build time.

## Four-stage flow

```
make_wrappers.py  →  capture_frames.mjs  →  frames_to_data.py  →  gen_splash_header.py
     (1)                   (2)                    (3)                      (4)
```

| Stage | Script | Reads | Writes |
|-------|--------|-------|--------|
| 1 | `make_wrappers.py` | `animations.json`, `svg/*.svg` | `.cache/*.html`, `.cache/manifest.json` |
| 2 | `capture_frames.mjs` | `.cache/manifest.json` | `.cache/frames/<name>/f00…f19.png` |
| 3 | `frames_to_data.py` | `animations.json`, `.cache/frames/` | `tools/svg_anim_data/manifest.json`, `tools/svg_anim_data/clawd_*.bin` |
| 4 | `gen_splash_header.py` | `tools/svg_anim_data/` | `firmware/src/splash_animations.h` |

Stage 4 is also invoked automatically by the PlatformIO pre-build hook
(`firmware/scripts/gen_splash.py`) on every `pio run` — node is **not** required
to build or flash firmware.

## How to run

```bash
# Full build (from repo root or from this directory):
cd tools/svg_pipeline
./build.sh

# Test run to a temp dir — stages 1-3 only, stage 4 skipped:
OUT_DIR=/tmp/svg_pipeline_test ./build.sh
```

The capture stage (2) takes 1–3 minutes.

## Config

All parameters and the animation list live in **`animations.json`**:

```jsonc
{
  "params": {
    "grid":       128,   // output pixel dimension (square)
    "palette":    24,    // colors per animation
    "frames":     20,    // frames per loop
    "period_ms":  1200,  // loop duration in ms  (hold = period_ms / frames)
    "capture_px": 288    // Playwright viewport side in px
  },
  "animations": [
    { "svg": "clawd-idle-living", "name": "idle living", "category": "Idle", "group": 0 },
    ...
  ]
}
```

The `group` field is a human-readable annotation of intended usage-rate bucket.
It is NOT propagated to the manifest or the generated header and has no
compile-time/runtime effect. Runtime group membership is resolved by matching
each animation's `name` string against `GROUP_NAMES` in
`firmware/src/splash.cpp` — that name match is the authoritative sync point.

## Dependencies

**Python** (run `pip install pillow cairosvg`):
- `pillow` — image processing, palette quantization
- `cairosvg` — SVG rasterization for tight viewBox computation

**Node.js** (capture stage only — not needed to build/flash firmware):
- `playwright` — headless browser automation
- A Chromium browser installed via `npx playwright install chromium`

### Playwright / Chromium resolution

`capture_frames.mjs` resolves Playwright in order:

1. `require('playwright')` — works if installed locally (`npm i playwright`) or
   in `tools/node_modules`.
2. `import(process.env.PLAYWRIGHT_DIR + '/playwright')` — set `PLAYWRIGHT_DIR`
   to override.
3. Hard-coded global fallback:
   `/home/rarp/.nvm/versions/node/v22.12.0/lib/node_modules/@playwright/cli/node_modules/playwright`

The Chromium executable is resolved by scanning
`~/.cache/ms-playwright/chromium-*/chrome-linux64/chrome` and picking the
newest entry that does not include "headless" in the directory name.

## Attribution

SVG animations: [clawd-tank](https://github.com/marciogranzotto/clawd-tank) by
Marcio Granzotto, MIT license — see `LICENSE.clawd-tank`.
