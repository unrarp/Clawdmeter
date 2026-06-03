# Clawdmeter

A small ESP32 dashboard I made for my desk to keep an eye on my Claude Code (and Codex) usage at a glance.

It runs on a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) plus a few other boards (see [Hardware](#hardware)). The device connects to your WiFi network and pulls usage data from a small host daemon over plain HTTP. The daemon polls your Claude and Codex usage every 5 minutes and caches the result; the display fetches and renders it roughly every 45 seconds. The splash screen plays pixel-art Clawd animations that get busier as your usage rate climbs.

|              Usage meter              |              Clawd animation screen              |
| :-----------------------------------: | :----------------------------------------------: |
| ![Usage meter](assets/demo.jpeg) | ![Clawd animation screen](assets/demo.gif) |

The Clawd animations come from [claudepix](https://claudepix.vercel.app), [@amaanbuilds](https://x.com/amaanbuilds)'s library of pixel-art Clawd sprites, check it out, it's lovely.

## Screens

The device boots into the splash and stays there until you press the middle (PWR) button, which cycles through the usage screens — Claude, then Codex — and WiFi. Tap the screen anywhere to flip back to the splash; tap again to dismiss it.

|              Splash               |              Claude             |              Codex              |              WiFi               |
| :-------------------------------: | :-----------------------------: | :-----------------------------: | :-----------------------------: |
| ![Splash](screenshots/amoled_18/splash.png) | ![Claude](screenshots/amoled_18/usage.png) | ![Codex](screenshots/amoled_18/codex.png) | ![WiFi](screenshots/amoled_18/wifi.png) |
|   Splash; touch-toggle anytime    | Claude session & weekly utilization | Codex session & weekly utilization | Connection diagnostics (SSID / IP / RSSI / data age) |

While the splash is up, the middle button cycles animations instead of screens. The firmware also auto-rotates every 20 s within the current usage-rate group, so a long stretch on the splash isn't just one Clawd on loop.

## Hardware

Boards supported out of the box:

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786)
- [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm?&aff_id=149786) 
- [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm?&aff_id=149786)

> Please check if a pull request exists for your alternative hardware port before opening a new one, providing QA feedback and testing on the same hardware is more valuable than duplicate pull requests.

**Porting to another board:** the firmware is a thin HAL with per-board folders under `firmware/src/boards/`. Drop in a new folder and a new PlatformIO env — `main.cpp`, `ui.cpp`, and `splash.cpp` never need to change. See [`docs/porting/adding-a-board.md`](docs/porting/adding-a-board.md) for the walk-through and [`docs/porting/hal-contract.md`](docs/porting/hal-contract.md) for the interfaces a port must implement.

## Prerequisites

- Linux (tested on Ubuntu) or macOS
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- `python3` (the installer sets up a venv with `httpx`)
- Claude Code with an active subscription (and/or the Codex CLI signed in, if you want the Codex screen)
- The device and the host machine must be on the same WiFi network

## macOS installation

The macOS host pieces — Python daemon, LaunchAgent, and flash helper — were ported by [Chris Davidson (@lorddavidson)](https://github.com/lorddavidson). Thanks Chris!

### Configure WiFi credentials

Before flashing, copy `firmware/src/net_config.example.h` to `firmware/src/net_config.h` and fill in your network details:

```c
#define WIFI_SSID      "YourNetwork"
#define WIFI_PASSWORD  "YourPassword"
#define DAEMON_HOST    "my-macbook.local"   // your machine's mDNS hostname
#define DAEMON_PORT    8080
#define FETCH_INTERVAL_MS  45000
```

`DAEMON_HOST` is your machine's mDNS hostname (typically `<computer-name>.local`) — no static IP needed.

### Flash the firmware

```bash
./flash-mac.sh waveshare_amoled_216                       # auto-detects /dev/cu.usbmodem*
./flash-mac.sh waveshare_amoled_18  /dev/cu.usbmodem1101  # or pass an explicit USB serial port
```

The board env name is required. Run `./flash-mac.sh` with no args to see the available envs (scraped from `firmware/platformio.ini`).

### Install the daemon

The daemon reads your Claude OAuth token from the macOS Keychain (service `Claude Code-credentials`) — and your Codex token from `~/.codex/auth.json` if present — polls usage every 5 minutes, and serves the result over HTTP on port 8080.

```bash
./install-mac.sh
```

The installer creates a Python venv in `daemon/.venv/`, installs `httpx`, and renders a LaunchAgent into `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`.

Useful commands:

```bash
launchctl list | grep claude-usage                                          # check it's running
tail -F ~/Library/Logs/clawdmeter.stdout.log                                # live logs
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist  # stop
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist # start
```

## Linux installation

### Configure WiFi credentials

Before flashing, copy `firmware/src/net_config.example.h` to `firmware/src/net_config.h` and fill in your network details:

```c
#define WIFI_SSID      "YourNetwork"
#define WIFI_PASSWORD  "YourPassword"
#define DAEMON_HOST    "my-laptop.local"   // your machine's mDNS hostname
#define DAEMON_PORT    8080
#define FETCH_INTERVAL_MS  45000
```

`DAEMON_HOST` is your machine's mDNS hostname (typically `<computer-name>.local`) — no static IP needed.

### Flash the firmware

```bash
./flash.sh waveshare_amoled_216                  # defaults to /dev/ttyACM0
./flash.sh waveshare_amoled_18  /dev/ttyACM1     # or pass an explicit USB serial port
```

The board env name is required. Run `./flash.sh` with no args to see the available envs (scraped from `firmware/platformio.ini`).

### Install the daemon

The daemon polls your Claude (and Codex, if configured) usage every 5 minutes and serves the result over HTTP on port 8080. The device fetches it over your local network — no pairing, no Bluetooth.

```bash
./install.sh
systemctl --user start claude-usage-daemon
```

Check status: `systemctl --user status claude-usage-daemon`

View logs: `journalctl --user -u claude-usage-daemon -f`

## How it works

1. The daemon reads your Claude Code OAuth token — from the macOS Keychain (service `Claude Code-credentials`) on macOS, or from `~/.claude/.credentials.json` on Linux — and, if present, your Codex token from `~/.codex/auth.json`.
2. It polls each provider's read-only usage endpoint — Anthropic's `api.anthropic.com/api/oauth/usage` and OpenAI's Codex `chatgpt.com/backend-api/wham/usage` — every 5 minutes (both are rate-limited).
3. The session/weekly percentages and reset times come straight out of those JSON responses. Each provider is independently optional — one you haven't set up just shows a "No account" screen rather than disappearing.
4. The daemon caches the latest result and serves it as JSON over HTTP (`GET http://<host>:8080/usage`) on your local network.
5. The firmware connects to your WiFi network, resolves the daemon host via mDNS (`<hostname>.local`), and fetches the payload roughly every 45 seconds.
6. `parse_json()` maps the 14-key compact JSON to the `UsageData` struct and the LVGL dashboard repaints.
7. The firmware also tracks the rate of change of session % over a 5-minute window and picks splash animations from the matching mood group.

## Physical buttons

The 2.16″ board has three side buttons (the 1.8″ port has only Left/BOOT and Middle/PWR — no right button). The middle button is screen-aware; the left button forces an immediate data refresh; the right is unused. The GPIOs below are for the S3 2.16; pins differ per board (e.g. the C6 2.16 uses GPIO 9/10 for Left/Right).

| Button           | GPIO         | Function                                                       |
| ---------------- | ------------ | -------------------------------------------------------------- |
| **Left**         | GPIO 0       | Force an immediate `/usage` refresh                            |
| **Middle** (PWR) | AXP2101 PKEY | Cycle screens (Claude → Codex → WiFi); on splash, cycle anims  |
| **Right**        | GPIO 18      | Currently unused                                               |

HID keyboard output (Space / Shift+Tab) has been removed along with Bluetooth. The physical buttons retain only their local screen-cycling roles.

## HTTP protocol

The daemon exposes a single HTTP endpoint:

| Endpoint      | Method | Description                              |
| ------------- | ------ | ---------------------------------------- |
| `/usage`      | GET    | Latest 14-key usage JSON (`200`) or `503` if no poll has succeeded yet |
| `/healthz`    | GET    | Daemon liveness check                    |

The daemon binds to `0.0.0.0:8080` (unauthenticated — trusted home LAN). The device resolves it via mDNS as `<hostname>.local`.

JSON payload format:

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": true,
  "sp": true, "cp": true,
  "cs": 32, "csr": 90, "cw": 15, "cwr": 5400, "cst": "allowed", "cok": true }
```

Fields (Claude): `s` = session %, `sr` = session reset (minutes), `w` = weekly %, `wr` = weekly reset (minutes), `st` = status, `ok` = success flag. `sp` / `cp` = Claude / Codex account present (booleans). Codex mirrors Claude with a `c` prefix: `cs`, `csr`, `cw`, `cwr`, `cst`, `cok`. The daemon always serves all 14 keys; each provider has its own success flag (`ok` for Claude, `cok` for Codex), so a present-but-failing provider keeps its last-good numbers with that flag `false` while the other panel stays live.

## Development

The sections below are for contributors regenerating bundled assets — none of it is needed just to flash and run the device.

### Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate each one (one at a time — `lv_font_conv` doesn't like loop-driven invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (titles, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (numbers 48/36, panel labels 28, small text 24/20, fine print 16/14/12)
for size in 48 36 28 24 20 16 14 12; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done

# DejaVu Sans Mono (32px, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 32 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_32.c --lv-include "lvgl.h"
```

**Note:** `lv_font_conv` ≥1.5.3 `--format lvgl` output is already LVGL-9-compatible — **no hand-patching required.** (Older versions emitted LVGL-8 structs that rendered invisible until patched; if you must process pre-1.5.3 output, the legacy fixes were: strip the `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache`, and add `.release_glyph`/`.kerning`/`.static_bitmap`/`.fallback`/`.user_data`.) The existing hand-patched `font_*.c` files predate 1.5.3 — don't "fix" newly generated files to match them.

### Adding CJK support (optional, not built in)

The shipped fonts are ASCII-only. To render Chinese/Japanese/Korean glyphs
you can generate a CJK font from [Noto Sans CJK SC](https://github.com/notofonts/noto-cjk)
(SIL OFL 1.1). This example covers the full CJK Unified Ideographs basic
block (U+4E00–U+9FFF, ~20k glyphs) plus ASCII, CJK punctuation, and
halfwidth/fullwidth forms at 16px, 2bpp:

```bash
lv_font_conv --font NotoSansCJKsc-Regular.otf --size 16 --bpp 2 \
  --no-compress --format lvgl --lv-include 'lvgl.h' \
  -r '0x20-0x7E,0xB7,0x2014,0x2018-0x2019,0x201C-0x201D,0x2026,0x3000-0x303F,0x4E00-0x9FFF,0xFF00-0xFFEF' \
  -o firmware/src/font_cjk_16.c
```

Because the font carries >65k of glyph bitmap data, add
`-DLV_FONT_FMT_TXT_LARGE=1` to the `platformio.ini` build flags so font
descriptor offsets switch from 16-bit to 32-bit. Declare it with
`LV_FONT_DECLARE` and point whichever label needs non-Latin text at it —
the brand headline/title fonts stay ASCII-only, so CJK text in those slots
renders as empty boxes.

### Converting Lucide icons

The UI uses a small set of [Lucide](https://lucide.dev) icons (wifi + battery states) converted to RGB565 / RGB565A8 C arrays for LVGL.

```bash
node tools/png_to_lvgl.js assets/icon_wifi_48.png icon_wifi_data ICON_WIFI_WIDTH ICON_WIFI_HEIGHT
```

Default tint is white (`0xFFFFFF`); Lucide PNGs ship as black-on-transparent and would render invisible against the dark UI without it. Pass `--no-tint` for pre-coloured artwork like the logo. Battery icons use RGB565A8 (alpha plane) so they blend cleanly over the splash; the rest are baked RGB565 over the panel colour. Paste the converter output into `firmware/src/icons.h`.

### Splash animations

The animations come from [claudepix.vercel.app](https://claudepix.vercel.app),
a library of Clawd sprites. `tools/scrape_claudepix.js` evaluates the
site's JavaScript in a Node VM to pull out frame data and palettes, then
`tools/convert_to_c.js` turns everything into RGB565 C arrays and writes
`firmware/src/splash_animations.h`.

To re-pull (e.g. when the source library updates):

```bash
node tools/scrape_claudepix.js
node tools/convert_to_c.js
pio run -d firmware -t upload
```

See `tools/README.md` for details.

## Credits

- Pixel-art Clawd animation by [@amaanbuilds](https://x.com/amaanbuilds), sourced from [claudepix.vercel.app](https://claudepix.vercel.app). Frame data and palettes scraped + converted by the tooling in `tools/`.
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for wifi and battery UI glyphs.
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing warning below.

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a license for but this software uses without permission as well as using assets from Anthropic such as the copyrighted Clawd mascot so even though the code in this repo is non-proprietary I will not license it myself under a copyleft license since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
