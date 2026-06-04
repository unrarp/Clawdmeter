---
paths:
  - "firmware/src/font_*.c"
  - "firmware/src/fonts.h"
  - "firmware/src/ui.cpp"
---

# Font rules

- **`lv_font_conv` ≥1.5.3 `--format lvgl` output is already LVGL-9-compatible — don't hand-patch it.** The manual patch in AGENTS.md "Critical gotchas" #4 (strip `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache`, add `.release_glyph`/`.kerning`/`.static_bitmap`/`.fallback`/`.user_data`) was for *older* output. 1.5.3 emits version-guarded structs that gate `.cache` out and `.fallback` in for v9 automatically, and the v9-only `lv_font_t` fields zero-initialize correctly when absent — a font generated this way (`font_styrene_36.c`) compiled and rendered unpatched. The pre-existing `font_styrene_*`/`font_tiempos_*` files are the *old* hand-patched style (guards already stripped); both styles work, so don't "fix" the new one to match the old.
- **Adding a font size means generating a new `.c` file — there is no runtime rasterizer.** Bitmap fonts are pre-baked per pixel size (one `lv_font_conv` run per typeface+size); FreeType/TinyTTF are disabled, so you cannot "just use 36px". Generate `font_<face>_<size>.c`, add its `LV_FONT_DECLARE` to `firmware/src/fonts.h` (the single font inventory, included by `ui.cpp` + `splash.cpp`), and add the size to the README regen loop (both have drifted before). → see docs/decisions/2026-06-02-bitmap-fonts-vs-runtime-ttf.md

- **Bitmap fonts ship a fixed glyph set — don't assume non-ASCII typographic characters render; prefer ASCII or verify on-screen.** Each `font_*.c` contains only the glyphs it was generated with, and a missing glyph renders as a tofu box (□) silently — no compile error, no fallback. Common offenders in copied/pasted UI strings: ellipsis `…` (U+2026), em/en dash `—`/`–`, smart quotes `“”’`, `×`, `•`. Use ASCII equivalents (`...`, `-`, `"`) or screenshot the screen to confirm the glyph exists before relying on it. Coverage is per generated file, so a glyph present at one size may be absent at another. (Seen here: `font_styrene_20` lacked `…` and `—`.)

## Related decisions

- `2026-06-02-bitmap-fonts-vs-runtime-ttf` — why pre-baked bitmaps over runtime TTF, and the per-size-file consequence.
