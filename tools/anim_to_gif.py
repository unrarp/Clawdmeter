#!/usr/bin/env python3
"""Render a splash animation to an animated GIF, pixel-identical to the device.

The firmware draws each splash animation from the palette-indexed binaries in
`tools/svg_anim_data/` (one byte per pixel, frames concatenated, row-major) over
the manifest palette — see `splash.cpp::render_frame`. This reconstructs the same
frames on the host and writes a GIF with the manifest's per-frame hold timing, so
the result matches what the panel shows without a (slow, glare-prone) camera or
serial framebuffer capture.

Usage:
    tools/anim_to_gif.py <anim-name-or-bin> [-o out.gif] [--scale N] [--loops N]

    tools/anim_to_gif.py "working building" -o assets/demo.gif --scale 3
    tools/anim_to_gif.py clawd_eureka.bin --scale 4
"""
import argparse
import json
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "svg_anim_data")


def load_manifest():
    with open(os.path.join(DATA, "manifest.json")) as f:
        return json.load(f)


def find_anim(manifest, key):
    key_l = key.lower()
    for a in manifest["animations"]:
        if a["name"].lower() == key_l or a["bin"].lower() == key_l \
                or a["bin"].lower() == key_l + ".bin":
            return a
    names = ", ".join(a["name"] for a in manifest["animations"])
    sys.exit(f"animation '{key}' not found. Available: {names}")


def hex_to_rgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def render_frames(anim, scale):
    w, h, n = anim["width"], anim["height"], anim["frame_count"]
    palette = [hex_to_rgb(c) for c in anim["palette"]]
    with open(os.path.join(DATA, anim["bin"]), "rb") as f:
        raw = f.read()
    expect = w * h * n
    if len(raw) != expect:
        sys.exit(f"{anim['bin']}: expected {expect} bytes, got {len(raw)}")
    frames = []
    for i in range(n):
        cells = raw[i * w * h:(i + 1) * w * h]
        img = Image.new("RGB", (w, h))
        img.putdata([palette[c] if c < len(palette) else (0, 0, 0) for c in cells])
        if scale != 1:
            img = img.resize((w * scale, h * scale), Image.NEAREST)
        frames.append(img)
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("anim", help="animation name (e.g. 'working building') or bin filename")
    ap.add_argument("-o", "--out", help="output GIF path (default: <name>.gif in cwd)")
    ap.add_argument("--scale", type=int, default=3, help="integer upscale factor (nearest, default 3)")
    ap.add_argument("--pad-to", metavar="WxH", default=None,
                    help="center each frame on a black canvas of this size, e.g. 368x448 "
                         "to mimic the device panel (the firmware centers the art on black)")
    ap.add_argument("--loops", type=int, default=0, help="loop count, 0 = infinite (default)")
    args = ap.parse_args()

    manifest = load_manifest()
    anim = find_anim(manifest, args.anim)
    frames = render_frames(anim, args.scale)
    if args.pad_to:
        pw, ph = (int(v) for v in args.pad_to.lower().split("x"))
        padded = []
        for fr in frames:
            canvas = Image.new("RGB", (pw, ph), (0, 0, 0))
            canvas.paste(fr, ((pw - fr.width) // 2, (ph - fr.height) // 2))
            padded.append(canvas)
        frames = padded
    durations = anim["holds"]  # ms per frame, straight from the device's timing

    out = args.out or (anim["name"].replace(" ", "_") + ".gif")
    frames[0].save(
        out, save_all=True, append_images=frames[1:],
        duration=durations, loop=args.loops, disposal=2, optimize=True,
    )
    px = f"{frames[0].width}x{frames[0].height}"
    print(f"wrote {out}  ({anim['name']}, {len(frames)} frames, {px}, "
          f"{sum(durations)} ms loop)")


if __name__ == "__main__":
    main()
