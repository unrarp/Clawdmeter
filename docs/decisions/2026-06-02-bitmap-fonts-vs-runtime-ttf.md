---
date: 2026-06-02
module: firmware/ui/fonts
tags: [fonts, lvgl, lv_font_conv, bitmap-font, freetype, tiny_ttf, flash, psram, esp32-s3]
---

# Pre-baked bitmap fonts vs runtime TTF rasterization

## Context
The UI ships 12 generated `font_*.c` files (Styrene ×8, Tiempos ×2, Mono ×2), ~1.35 MB of source. Fixing the cramped compact layout needed a new headline size, which meant *generating* `font_styrene_36.c` rather than scaling an existing font — raising the question of why each size needs its own file and whether the device could render `.otf`/`.ttf` directly.

## Decision / Solution
Keep pre-baked, per-size LVGL bitmap fonts (`lv_font_conv --format lvgl --bpp 4 --no-compress`, one `.c` per typeface+size; declared via `LV_FONT_DECLARE` in `firmware/src/fonts.h`, the single inventory included by `ui.cpp` + `splash.cpp`). Do not enable a runtime rasterizer.

## Why
Bitmap fonts store rasterized glyphs, so size scales with px² (Styrene 12 = 41 KB → 48 = 230 KB → Tiempos 56 = 315 KB) and a 28px font cannot be derived from a 48px one — hence one file per size. They live in flash (`const`), cost zero RAM and zero CPU at render (straight blits), and render identically every boot.

LVGL *does* support runtime TTF via FreeType and TinyTTF — a single `StyreneB-Regular.otf` (72 KB) + `TiemposText.otf` (91 KB) could replace all ~1.35 MB and render any size on demand. Both are disabled (`LV_USE_FREETYPE`/`LV_USE_TINY_TTF` off). The trade is deliberate: on the ESP32-S3, **flash is abundant** (firmware is ~20% of the 6.5 MB partition) but **RAM and CPU are scarce** — runtime rasterization needs a PSRAM glyph cache and per-glyph CPU. For a 3-screen dashboard with a fixed, known set of sizes, paying abundant flash for crisp, zero-cost glyphs is correct.

## Alternatives considered
- **Runtime FreeType** — most capable, but the heaviest library plus a PSRAM cache; overkill for a static UI.
- **Runtime TinyTTF** — lighter; the option to revisit *if* the size set ever needs to be dynamic or many typefaces are added. Would collapse the 12 files to ~2 OTFs, trading flash (abundant) for RAM/CPU (scarce).

## Prevention
- Adding a size = generate a new `.c` (see `.claude/rules/fonts.md`), then update the README regen loop and the CLAUDE.md font inventory so a future regen doesn't drop it — both drifted before (the README loop was missing 36/16/14/12).
- `lv_font_conv` ≥1.5.3 emits LVGL-9-clean output; the manual patch in CLAUDE.md "Critical gotchas" #4 applies only to pre-1.5.3 output.

## Related
- `.claude/rules/fonts.md`
- CLAUDE.md "Critical gotchas" #4 (LVGL 9 font patching)
