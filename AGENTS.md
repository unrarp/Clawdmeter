# Project context

ESP32-S3 firmware for a desk-side Claude Code usage monitor. Each supported
board lives in its own `firmware/src/boards/<name>/` folder and is selected
via PlatformIO's `build_src_filter`. Adding a board means dropping in a new
folder + a new `[env:...]` block — `main.cpp`, `ui.cpp`, and `splash.cpp`
never see board-specific code. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md).

Two reference ports today:

- `boards/waveshare_amoled_216/` — original Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300, 480×480 square, CST9220 touch, IMU rotation). Build env: `waveshare_amoled_216`.
- `boards/waveshare_amoled_18/` — Waveshare ESP32-S3-Touch-AMOLED-1.8 (SH8601, 368×448 portrait, FT3168 touch, XCA9554 IO expander). Build env: `waveshare_amoled_18`.

The shared code calls a small HAL (`firmware/src/hal/`) that each board implements: display, touch, input, power, IMU. Optional features are guarded by `BoardCaps` (runtime) and `BOARD_HAS_*` (compile-time) rather than `#ifdef BOARD_*`.

Connects to a host daemon over WiFi/HTTP; the device polls `GET http://<daemon-host>:8080/usage` (every ~60 s) and the daemon caches the latest combined snapshot from the Anthropic (Claude) and OpenAI (Codex) usage APIs. This file is for future Claude Code sessions to bootstrap quickly. Read this first.

## Architecture

Shared code (`main.cpp`, `ui.cpp`, `splash.cpp`, `net.cpp`, `data.h`) calls a small
HAL (`firmware/src/hal/`) that each `boards/<name>/` folder implements (`board.h`,
`board_init.cpp` — runs before display init —, the per-HAL `.cpp`s, `caps.cpp`).
`build_src_filter` compiles shared code + one board folder per env. **The HAL boundary
is load-bearing — no `#ifdef BOARD_*` in shared code; add a `BoardCaps` field or a
per-board file instead.** Per-board critical pins, I2C addresses, and bring-up order
live in [`.claude/rules/boards.md`](.claude/rules/boards.md) (auto-loads when editing
`firmware/src/boards/**`). Walk-through and the interfaces a port must implement:
[`docs/porting/`](docs/porting/).

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                     # build 2.16 (default original)
pio run -d firmware -e waveshare_amoled_18                                      # build 1.8 (new port)
pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/cu.usbmodem101   # flash 1.8 on macOS
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0         # flash 2.16 on Linux
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS pio install) or `brew install platformio` on macOS.

Device path differs by OS: `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM0` on Linux. Both expose the ESP32-S3 native USB-JTAG (no boot-mode dance needed).

## QA your own UI changes — don't ask the user

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. `./screenshot.sh out.png [port]` captures a PNG sized to the active display (480×480 or 368×448). **Use this on every UI iteration** — Read the PNG with the Read tool, verify the change visually, iterate. Script auto-picks the macOS/Linux default port and falls back to pio's bundled Python if pyserial isn't on the system Python.

The boot screen is `SCREEN_SPLASH` and only advances on a physical button press, so a fresh flash will sit on the splash. To screenshot the screen you're actually editing without asking the user to press a button, **temporarily change the default boot screen** in `main.cpp` (search for `ui_show_screen(SCREEN_SPLASH);`) to `SCREEN_USAGE` / `SCREEN_CODEX` / `SCREEN_WIFI`, do your iteration, then revert before committing. (`screenshot.sh` shells out to `ffmpeg` for the raw→PNG step — if it's not installed the capture succeeds but conversion fails; decode the raw RGB565LE yourself or install ffmpeg.)

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. We use **PARTIAL render mode with strip rotation** (small 480×40 strips, fast). On rotation change → AMOLED brightness flash → force redraw (handled inside `display_hal_tick`).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`), not the 2.x that standard `espressif32` ships. We pin `pioarduino/platform-espressif32` 55.03.38-1.
4. **Fonts are pre-baked bitmaps — no runtime rasterizer.** Adding a size means generating a new `font_<face>_<size>.c` (`lv_font_conv`) and adding its `LV_FONT_DECLARE` to `firmware/src/fonts.h` (the single font inventory, included by `ui.cpp` + `splash.cpp`). `lv_font_conv` ≥1.5.3 `--format lvgl` output is already LVGL-9-compatible — **do not hand-patch it**, and don't "fix" new files to match the older hand-patched `font_*.c` style. See `.claude/rules/fonts.md`.
5. **Touch reading is centralized inside each board's `touch.cpp`.** The HAL `touch_hal_read()` is called once per loop from `my_touch_cb`; the board's implementation owns its latched `touch_pressed/x/y` state. Don't call the underlying controller from anywhere else — CST9220's `getPoint()` etc. do a full I2C transaction and concurrent callers consume each other's data.
6. **Even-aligned flush regions.** `display_hal_round_area` (called from `rounder_cb`) is what each board uses to enforce this. Required on CO5300, harmless on SH8601.
7. **Touch axis swap/mirror is per-board.** The 2.16's CST9220 needs `setSwapXY(true)` + `setMirrorXY(true, false)` — applied inside `boards/waveshare_amoled_216/touch.cpp::touch_hal_init()`. New ports apply their own.
8. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Use `init_icon_dsc(dsc, w, h, data, LV_COLOR_FORMAT_RGB565A8)` for icons that overlap non-uniform backgrounds (e.g. battery over splash). Lucide source PNGs are black-on-transparent — converter must tint to white or icons render invisible. See `tools/png_to_lvgl.js`.
9. **Per-board pre-init is `board_init()`.** Each board's `board_init.cpp` brings up `Wire` and any reset-gating IO expander BEFORE `display_hal_init()`. Skipping the IO expander release on AMOLED-1.8 leaves SH8601 + FT3168 in reset and they silently fail to probe.
10. **No `#ifdef BOARD_*` in shared code.** The whole point of the refactor — if you're about to add one, you probably want a `BoardCaps` field or a per-board file instead. See `docs/porting/capability-flags.md`.

## Icons & splash regen (tooling-driven)

Both are regenerated by scripts and documented in `tools/README.md` + the root README — don't hand-edit the generated output. Two foot-guns worth knowing here:

- **Icons:** `tools/png_to_lvgl.js` → splice into `firmware/src/icons.h`, draw with `init_icon_dsc(..., LV_COLOR_FORMAT_RGB565A8)`. The RGB565A8 planar layout + white-tint requirement is gotcha #8. Only the 5 battery icons use RGB565A8; the rest are raw RGB565 baked over opaque panel zones.
- **Splash:** `firmware/src/splash_animations.h` is a **gitignored build artifact** — auto-generated by `firmware/scripts/gen_splash.py` as a PlatformIO pre-build hook on every `pio run`. No manual step needed to build/flash (node not required). To regenerate the committed binary source data (`tools/svg_anim_data/manifest.json` + `clawd_*.bin`) run `tools/svg_pipeline/build.sh` (requires Node/Playwright for the capture stage, Python for the rest). Source SVGs vendored in `tools/svg_pipeline/svg/` (clawd-tank set, MIT). Each animation: 128×128, 24-color palette, ~20 frames. Config: `tools/svg_pipeline/animations.json` (active 12 animations). The `group` tier name (`"idle"`/`"normal"`/`"active"`/`"heavy"`, matching the usage-rate tiers in `usage_rate.cpp`) is the single source of truth for rate-bucket assignment — propagated through `manifest.json` into the generated header's `splash_anim_def_t.group` string and mapped to a rate-group index by `splash.cpp`'s `RATE_TIERS` table.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context (user is an embedded-beginner senior dev, brand-conscious, prefers iterative UI refinement, dislikes me authoring my own art when third-party assets are intended). Always read those memory files at session start.

## Daemon / host side

Single cross-platform Python daemon (`daemon/claude_usage_daemon.py`, Linux + macOS) polls **two** providers — Anthropic (Claude) and OpenAI (Codex) — and serves the latest combined snapshot as JSON over HTTP. The device fetches `GET http://<daemon-host>:8080/usage` on the LAN; daemon binds `0.0.0.0:8080`, no auth, plain HTTP (trusted LAN only). `systemctl --user start claude-usage-daemon`. **All daemon implementation rules — API field-mapping, the 14-key wire format, poll constants (300s/5s tick/60s backoff), token handling, and the presence/cache/env-var foot-guns — live in [`.claude/rules/daemon.md`](.claude/rules/daemon.md)** (auto-loads when editing `daemon/**`; read it manually if you touch the firmware JSON parser or `data.h`) and `docs/decisions/2026-06-02-api-oauth-usage-rate-limit.md`.

**WiFi / mDNS model:** The device is a WiFi STA; it resolves the daemon host as `<hostname>.local` via mDNS (`WiFi.hostByName()`). No pairing, no MAC cache. Creds + daemon host/port live in `firmware/src/net_config.h` (gitignored; see committed `net_config.example.h` template — real creds are never committed). `install.sh` / `install-mac.sh` set up a venv with `httpx` only — no bluetooth prereqs.

**Operating foot-guns:**

- The unit's `ExecStart` points to the venv Python + `daemon/claude_usage_daemon.py` absolute path — repoint it when switching between the worktree and the main checkout.
- The daemon is a long-running `while` loop: editing the script changes nothing until `systemctl --user restart` — the old logic stays resident (this is how you get a stale daemon silently serving the old wire format).
