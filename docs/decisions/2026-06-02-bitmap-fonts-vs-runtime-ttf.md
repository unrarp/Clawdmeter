---
date: 2026-06-02
module: firmware/ui/fonts
tags: [fonts, lvgl, lv_font_conv, bitmap-font, freetype, tiny_ttf, flash, psram, esp32-s3]
---

# Pre-baked bitmap fonts vs runtime TTF rasterization

## Context
The UI ships 9 generated `font_*.c` files (~1.2 MB of source). Fixing the
cramped compact layout needed a new headline size, which meant *generating*
`font_styrene_36.c` rather than scaling an existing font — raising the question
of why each size needs its own file and whether the device could rasterize
`.otf`/`.ttf` at runtime instead.

## Decision
Keep pre-baked, per-size LVGL bitmap fonts (one `.c` per typeface+size, declared
in `firmware/src/fonts.h`). Do not enable a runtime rasterizer.

## Why
Bitmap fonts store rasterized glyphs, so size scales with px² and a 28px font
can't be derived from a 48px one — hence one file per size. They live in flash
(`const`), cost zero RAM and zero CPU at render (straight blits), and look
identical every boot. LVGL *can* rasterize at runtime (FreeType / TinyTTF, both
disabled), collapsing all ~1.35 MB to two OTFs — but that needs a PSRAM glyph
cache and per-glyph CPU. On the ESP32-S3 flash is abundant (firmware is ~20% of
the 6.5 MB partition) while RAM/CPU are scarce, so for a 3-screen dashboard with
a fixed set of sizes, paying flash for zero-cost glyphs is the right trade.

## Alternatives considered
- **Runtime FreeType** — most capable, heaviest library + PSRAM cache; overkill.
- **Runtime TinyTTF** — lighter; revisit only *if* the size set must become
  dynamic or many typefaces are added.

## Prevention
- Adding a size = generate a new `.c` (`.claude/rules/fonts.md`), then update the
  README regen loop and the CLAUDE.md font inventory — both drifted before (the
  README loop was missing 36/16/14/12).

## Related
- `.claude/rules/fonts.md`; CLAUDE.md "Critical gotchas" #4 (LVGL 9 font patching).
