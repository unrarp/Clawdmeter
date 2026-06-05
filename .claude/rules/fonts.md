---
paths:
  - "firmware/src/font_*.c"
  - "firmware/src/fonts.h"
  - "firmware/src/ui.cpp"
---

# Font rules

- **`lv_font_conv` ≥1.5.3 `--format lvgl` output is already LVGL-9-compatible — don't
  hand-patch it.** The manual patch in AGENTS.md gotcha #4 was for *older* output;
  1.5.3 emits version-guarded structs that gate v9 fields correctly (confirmed:
  `font_styrene_36.c` compiled and rendered unpatched). The pre-existing
  `font_styrene_*`/`font_tiempos_*` files are the old hand-patched style — both work,
  so don't "fix" the new one to match the old.

- **Adding a size means generating a new `.c` — there is no runtime rasterizer.**
  FreeType/TinyTTF are disabled, so you can't "just use 36px". Generate
  `font_<face>_<size>.c`, add its `LV_FONT_DECLARE` to `firmware/src/fonts.h` (the
  single inventory), and add the size to the README regen loop (both have drifted
  before). → see `docs/decisions/2026-06-02-bitmap-fonts-vs-runtime-ttf.md`

- **Bitmap fonts ship a fixed glyph set — a missing glyph renders as a silent tofu box
  (□), no compile error.** Coverage is per generated file, so a glyph present at one
  size may be absent at another. Prefer ASCII (`...`, `-`, `"`) over `…` `—` `–` `“”’`
  `×` `•`, or screenshot to confirm. (Seen: `font_styrene_20` lacked `…` and `—`.)

## Related decisions

- `2026-06-02-bitmap-fonts-vs-runtime-ttf` — pre-baked bitmaps vs runtime TTF.
