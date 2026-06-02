---
paths:
  - "firmware/src/font_*.c"
  - "firmware/src/ui.cpp"
---

# Font rules

- **`lv_font_conv` ≥1.5.3 `--format lvgl` output is already LVGL-9-compatible — don't hand-patch it.** The manual patch in CLAUDE.md "Critical gotchas" #4 (strip `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache`, add `.release_glyph`/`.kerning`/`.static_bitmap`/`.fallback`/`.user_data`) was for *older* output. 1.5.3 emits version-guarded structs that gate `.cache` out and `.fallback` in for v9 automatically, and the v9-only `lv_font_t` fields zero-initialize correctly when absent — a font generated this way (`font_styrene_36.c`) compiled and rendered unpatched. The pre-existing `font_styrene_*`/`font_tiempos_*` files are the *old* hand-patched style (guards already stripped); both styles work, so don't "fix" the new one to match the old.
- **Adding a font size means generating a new `.c` file — there is no runtime rasterizer.** Bitmap fonts are pre-baked per pixel size (one `lv_font_conv` run per typeface+size); FreeType/TinyTTF are disabled, so you cannot "just use 36px". Generate `font_<face>_<size>.c`, add `LV_FONT_DECLARE` in `ui.cpp`, and add the size to BOTH the README regen loop and the CLAUDE.md font inventory (both have drifted before). → see docs/decisions/2026-06-02-bitmap-fonts-vs-runtime-ttf.md

## Related decisions

- `2026-06-02-bitmap-fonts-vs-runtime-ttf` — why pre-baked bitmaps over runtime TTF, and the per-size-file consequence.
