#!/usr/bin/env python3
"""Stage 1: Write per-animation capture wrappers.

Reads animations.json + svg/, writes per-animation HTML wrappers with
tight-cropped inlined SVGs into .cache/, then writes .cache/manifest.json.

Do NOT alter the tight_viewbox / inline_svg math: it must match the reference
rasterization used to produce the committed .bin data in tools/svg_anim_data/.
Changing the viewBox computation would shift the crop window and make newly
captured frames spatially inconsistent with existing committed data.
"""
import os, json, io, re
import cairosvg
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CFG_PATH   = os.path.join(SCRIPT_DIR, "animations.json")
SVG_DIR    = os.path.join(SCRIPT_DIR, "svg")
CACHE_DIR  = os.path.join(SCRIPT_DIR, ".cache")
os.makedirs(CACHE_DIR, exist_ok=True)

# --- load config ---
with open(CFG_PATH, encoding="utf-8") as _f:
    cfg = json.load(_f)
params = cfg["params"]
CAPTURE_PX = params["capture_px"]
FRAMES     = params["frames"]
PERIOD_MS  = params["period_ms"]

# --- tight_viewbox / inline_svg (verbatim from build_preview_html.py) ---
R      = 512    # bbox probe resolution
MARGIN = 0.10   # fraction of content size as padding

def tight_viewbox(svg_path, orig_vb):
    """Compute a tight viewBox (user units) from the rendered content bbox."""
    png = cairosvg.svg2png(url=svg_path, output_width=R, output_height=R,
                           background_color=None)
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    bb = im.getbbox()
    ox, oy, ow, oh = orig_vb
    sx, sy = ow / R, oh / R          # user units per pixel
    if not bb:
        return orig_vb
    x0, y0, x1, y1 = bb
    ux0, uy0 = ox + x0 * sx, oy + y0 * sy
    uw, uh = (x1 - x0) * sx, (y1 - y0) * sy
    m = MARGIN * max(uw, uh)
    return (ux0 - m, uy0 - m, uw + 2 * m, uh + 2 * m)

def parse_vb(txt):
    m = re.search(r'viewBox\s*=\s*"([^"]+)"', txt)
    if not m:
        return (0, 0, 100, 100)
    return tuple(float(v) for v in m.group(1).replace(",", " ").split())

def inline_svg(svg_path):
    with open(svg_path, encoding="utf-8") as _f:
        txt = _f.read()
    orig = parse_vb(txt)
    nx, ny, nw, nh = tight_viewbox(svg_path, orig)
    # rewrite the opening <svg ...> tag: tight viewBox, fill container, keep all defs/styles
    def fix(m):
        attrs = m.group(1)
        attrs = re.sub(r'\s(width|height)\s*=\s*"[^"]*"', "", attrs)
        attrs = re.sub(r'viewBox\s*=\s*"[^"]*"', "", attrs)
        return (f'<svg {attrs.strip()} viewBox="{nx:.3f} {ny:.3f} {nw:.3f} {nh:.3f}" '
                f'width="100%" height="100%" preserveAspectRatio="xMidYMid meet">')
    return re.sub(r'<svg\s+([^>]*)>', fix, txt, count=1)

# --- generate wrappers ---
manifest_items = []
for anim in cfg["animations"]:
    svg_name = anim["svg"]   # e.g. "clawd-idle-living"
    name     = svg_name.replace("clawd-", "")  # e.g. "idle-living"
    svg_path = os.path.join(SVG_DIR, f"{svg_name}.svg")
    try:
        svg = inline_svg(svg_path)
    except Exception as e:
        raise SystemExit(f"ERROR inlining {svg_name}: {e}")

    html = (f'<!doctype html><html><head><meta charset="utf-8"><style>\n'
            f'  html,body{{margin:0;background:transparent}}\n'
            f'  #box{{width:{CAPTURE_PX}px;height:{CAPTURE_PX}px;display:flex;\n'
            f'        align-items:center;justify-content:center}}\n'
            f'  #box svg{{width:100%;height:100%;display:block}}\n'
            f'</style></head><body><div id="box">{svg}</div></body></html>')

    html_path = os.path.join(CACHE_DIR, f"{name}.html")
    with open(html_path, "w", encoding="utf-8") as _f:
        _f.write(html)
    manifest_items.append({"name": name, "html": html_path})
    print(f"wrapper  {name}")

manifest = {
    "capture_px": CAPTURE_PX,
    "frames":     FRAMES,
    "period_ms":  PERIOD_MS,
    "items":      manifest_items,
}
manifest_path = os.path.join(CACHE_DIR, "manifest.json")
with open(manifest_path, "w", encoding="utf-8") as _f:
    json.dump(manifest, _f, indent=2)
print(f"manifest -> {manifest_path}  ({len(manifest_items)} animations)")
