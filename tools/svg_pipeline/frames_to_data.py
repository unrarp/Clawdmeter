#!/usr/bin/env python3
"""Stage 3: Convert captured frames to palette-indexed binary animation data.

Reads animations.json + .cache/frames/<name>/, writes:
  <OUT_DIR>/manifest.json          (catalog — order follows animations.json)
  <OUT_DIR>/clawd_<name>.bin       (one per animation; frame_count*H*W bytes)

OUT_DIR is taken from the OUT_DIR env var, or --out-dir CLI arg, defaulting to
tools/svg_anim_data/ (sibling of tools/svg_pipeline/).

Image math must NOT be changed: it must match the reference rasterization used
to produce the committed .bin data in tools/svg_anim_data/. Changing any of
these constants (grid size, near-black threshold, quantization method/dither,
hold calculation) would silently make the generator incompatible with existing
committed binary data:
  - on_panel(): full-frame scale to GRID×GRID (NO per-frame getbbox crop)
  - near-black snap: max(r,g,b) < 26  -> (0,0,0)
  - quantize(colors=PAL, method=MEDIANCUT, dither=NONE) over the whole strip
  - hold = period_ms / frames (integer ms per frame)
"""
import os, sys, glob, json
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CFG_PATH   = os.path.join(SCRIPT_DIR, "animations.json")
CACHE_DIR  = os.path.join(SCRIPT_DIR, ".cache")
FRAMES_DIR = os.path.join(CACHE_DIR, "frames")

# --- output dir: env var > --out-dir arg > default ---
def _resolve_out_dir():
    if "OUT_DIR" in os.environ:
        return os.environ["OUT_DIR"]
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a in ("--out-dir", "--out") and i + 1 < len(args):
            return args[i + 1]
    # default: tools/svg_anim_data/ (sibling of tools/svg_pipeline/)
    return os.path.join(os.path.dirname(SCRIPT_DIR), "svg_anim_data")

OUT_DIR = _resolve_out_dir()
os.makedirs(OUT_DIR, exist_ok=True)

# --- load config ---
with open(CFG_PATH, encoding="utf-8") as _f:
    cfg = json.load(_f)
params = cfg["params"]
GRID   = params["grid"]        # 128
PAL    = params["palette"]     # 24
HOLD   = params["period_ms"] // params["frames"]   # 60 ms (1200/20)

# Valid rate-bucket tiers. Mirrors RATE_TIERS in firmware/src/splash.cpp and the
# usage-rate tiers in usage_rate.cpp. Validated below so a typo fails the build
# loudly here instead of silently dropping the animation at runtime.
VALID_GROUPS = ("idle", "normal", "active", "heavy")

# --- image math: must match the reference rasterization the committed .bin data
# was generated with — do NOT alter these constants or the framing logic. ---
def on_panel(im, size):
    canv = Image.new("RGB", im.size, (0, 0, 0))
    canv.paste(im, (0, 0), im)
    canv = canv.resize((size, size), Image.LANCZOS)
    px = canv.load()
    for y in range(size):
        for x in range(size):
            r, g, b = px[x, y]
            if max(r, g, b) < 26: px[x, y] = (0, 0, 0)
    return canv

# --- process each animation ---
index = []
for anim in cfg["animations"]:
    svg_name  = anim["svg"]                    # e.g. "clawd-idle-living"
    name      = anim["name"]                   # e.g. "idle living"
    if anim.get("group") not in VALID_GROUPS:
        raise SystemExit(f"ERROR: animation '{name}' has invalid group {anim.get('group')!r} "
                         f"in animations.json — must be one of {VALID_GROUPS}")
    group     = anim["group"]                  # e.g. "idle"
    stem      = svg_name.replace("clawd-", "") # e.g. "idle-living"

    paths = sorted(glob.glob(os.path.join(FRAMES_DIR, stem, "f*.png")))
    if not paths:
        raise SystemExit(f"ERROR: no captured frames for {stem} at {os.path.join(FRAMES_DIR, stem)}")

    panels = [on_panel(Image.open(p).convert("RGBA"), GRID) for p in paths]
    strip  = Image.new("RGB", (GRID * len(panels), GRID))
    for i, p in enumerate(panels):
        strip.paste(p, (i * GRID, 0))

    q = strip.quantize(colors=PAL, method=Image.MEDIANCUT, dither=Image.NONE)
    pal_raw     = q.getpalette()[:PAL * 3]
    palette_hex = ['#%02x%02x%02x' % tuple(pal_raw[i*3:i*3+3]) for i in range(PAL)]

    grids = []
    for i in range(len(panels)):
        px = list(q.crop((i * GRID, 0, i * GRID + GRID, GRID)).getdata())
        grids.append([px[r * GRID:(r + 1) * GRID] for r in range(GRID)])

    fname    = "clawd_" + stem.replace("-", "_")   # e.g. "clawd_idle_living"
    bin_name = fname + ".bin"
    bin_path = os.path.join(OUT_DIR, bin_name)

    # Write binary: frame_count * GRID * GRID bytes (uint8 palette indices),
    # frame-major then row-major.
    buf = bytearray()
    for g in grids:
        for row in g:
            buf.extend(row)
    with open(bin_path, "wb") as bf:
        bf.write(buf)

    index.append({
        "name":        name,
        "group":       group,
        "width":       GRID,
        "height":      GRID,
        "frame_count": len(grids),
        "holds":       [HOLD] * len(grids),
        "palette":     palette_hex,
        "bin":         bin_name,
    })
    colors_used = len(set(q.convert("RGB").getdata()))
    print(f"{stem:25s}  {len(grids)} frames, {colors_used} colors used  ->  {bin_path}")

manifest = {"palette_size": PAL, "animations": index}
manifest_path = os.path.join(OUT_DIR, "manifest.json")
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
print(f"\nwrote manifest.json with {len(index)} animations  ->  {manifest_path}")
