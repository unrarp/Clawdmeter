"""
PlatformIO pre-build hook — regenerates firmware/src/splash_animations.h
from the committed binary data in tools/svg_anim_data/ when needed.

Registered in platformio.ini as:
    extra_scripts = pre:scripts/gen_splash.py
"""
import sys
import os

Import("env")  # noqa: F821  (SCons injects this)

# SCons execs extra_scripts without setting __file__, so resolve paths from the
# PlatformIO project dir (= firmware/) instead. Repo root is its parent.
_PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
_REPO_ROOT = os.path.abspath(os.path.join(_PROJECT_DIR, ".."))

_PIPELINE_DIR = os.path.join(_REPO_ROOT, "tools", "svg_pipeline")
_IN_DIR = os.path.join(_REPO_ROOT, "tools", "svg_anim_data")
_OUT_FILE = os.path.join(_REPO_ROOT, "firmware", "src", "splash_animations.h")

if _PIPELINE_DIR not in sys.path:
    sys.path.insert(0, _PIPELINE_DIR)

import gen_splash_header  # noqa: E402

if not os.path.exists(os.path.join(_IN_DIR, "manifest.json")):
    print("[splash] no animation data in tools/svg_anim_data — run tools/svg_pipeline/build.sh")
elif gen_splash_header.needs_regen(_IN_DIR, _OUT_FILE):
    print("[splash] regenerating splash_animations.h")
    gen_splash_header.generate(_IN_DIR, _OUT_FILE)
else:
    print("[splash] splash_animations.h up to date")
