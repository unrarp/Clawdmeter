#!/usr/bin/env bash
# Full animation build pipeline.
# Can be run from any directory; always operates relative to its own location.
#
# Stages:
#   1. make_wrappers.py       — inline SVGs into .cache/ HTML wrappers + manifest.json
#   2. capture_frames.mjs     — Playwright headless capture -> .cache/frames/
#   3. frames_to_data.py      — frames -> binary manifest.json + clawd_*.bin -> tools/svg_anim_data/
#   4. gen_splash_header.py   — binary data -> firmware/src/splash_animations.h
#                               (also auto-runs as PlatformIO pre-build hook on every pio build)
#
# Usage:
#   ./build.sh                        # full run, writes to tools/svg_anim_data/
#   OUT_DIR=/tmp/test ./build.sh      # test run to a temp dir (skips header gen step)
set -euo pipefail

cd "$(dirname "$0")"

echo "=== Stage 1: make_wrappers.py ==="
python3 make_wrappers.py

echo ""
echo "=== Stage 2: capture_frames.mjs ==="
node capture_frames.mjs

echo ""
echo "=== Stage 3: frames_to_data.py ==="
python3 frames_to_data.py

# Stage 4 is only meaningful when writing to the real svg_anim_data dir.
# Skip if OUT_DIR is set to something other than the default.
DEFAULT_OUT="$(cd .. && pwd)/svg_anim_data"
ACTUAL_OUT="${OUT_DIR:-$DEFAULT_OUT}"

if [ "$ACTUAL_OUT" = "$DEFAULT_OUT" ]; then
    echo ""
    echo "=== Stage 4: gen_splash_header.py ==="
    python3 gen_splash_header.py
else
    echo ""
    echo "=== Stage 4: skipped (OUT_DIR=$ACTUAL_OUT is not the default svg_anim_data) ==="
fi

echo ""
echo "=== Build complete ==="
