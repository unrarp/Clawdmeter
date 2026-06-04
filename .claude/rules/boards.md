---
paths:
  - "firmware/src/boards/**"
---

# Board / hardware rules

Per-board critical pins, I2C addresses, and bring-up order. Adding a board = new
`boards/<name>/` folder + `[env:...]` block; see
[`docs/porting/adding-a-board.md`](../../docs/porting/adding-a-board.md).

## AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** on same I2C bus (addr=0x34) — battery, USB VBUS, PWR button IRQ
- IMU: **QMI8658** on same I2C bus (addr=0x6B) — accelerometer for auto-rotation
- Buttons: GPIO 0 (left → force immediate usage refresh via `net_request_refresh()`), GPIO 18 (right — currently unused/reserved), AXP PKEY (middle → short press cycles screens / on splash cycles animations; **long hold = hardware power-off** via the AXP, press again to power on). On battery the panel dims after ~6 min idle and the firmware auto-powers-off after ~12 min via `pmu.shutdown()` (`idle.cpp`).

## Build env and flash — confirm the board before acting

**Always pass `-e <env>` explicitly when building or flashing; never rely on the PlatformIO default.**
The default env in `platformio.ini` is `waveshare_amoled_216` (the 2.16″ board). The user's
current device is the **1.8″ board (`waveshare_amoled_18`)**. Flashing the wrong env produces a
dark screen with no error — the MCU runs and USB enumerates (serial may output data) but the panel
stays in reset. Verify the board before flashing: `screenshot.sh` returns a `480×480` PNG for
`_216` and `368×448` for `_18`; a dark panel with a running MCU is the wrong-env symptom.
Correct flash: `pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/ttyACM0`.

## AMOLED-1.8 (newer port)
- Display: **SH8601** AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1)
- Touch: **FT3168** via I2C (SDA=15, SCL=14, INT=21, addr=0x38). Driven by minimal inline reader in `main.cpp` (FocalTech standard register layout — avoids vendoring the GPLv3 `Arduino_DriveBus` library).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. Rotation is disabled through the HAL, **not** an `#ifdef` (per gotcha #10 — no `#ifdef BOARD_*` in shared code): `BOARD_HAS_ROTATION 0` and this board's `imu_hal_rotation_quadrant()` returns `0`. The strip-remap (`rotate_strip()`) lives only in the 2.16's `display.cpp`, so `build_src_filter` never compiles it for this board.
- Buttons: GPIO 0 (BOOT → force immediate usage refresh via `net_request_refresh()`), XCA9554 EXIO4 (PWR → **short** press cycles screens / on splash cycles animations; **long hold = hardware power-off** via the AXP PWRON pin, press again to power on). The firmware reads EXIO4 on a short *release* (held < 1 s) so a power-off hold doesn't also flip a page. On battery the panel dims after ~6 min idle and the firmware auto-powers-off after ~12 min via `pmu.shutdown()` (`idle.cpp`). **No third button** (GPIO 18 button doesn't exist on this board).

## AMOLED-2.16-C6 (C6 SoC sibling of the 2.16)

Build env: `waveshare_amoled_216_c6` (`-DBOARD_AMOLED_216_C6`). Same 480×480 AMOLED form factor as the 2.16 but a **single-core RISC-V ESP32-C6, no PSRAM, BLE 5.3 only** — and an **SH8601** panel (Arduino_GFX `Arduino_SH8601`, GFX lib ≥1.6.4), not the 2.16's CO5300.
- Display: **SH8601** AMOLED via QSPI (CS=15, **SCLK=0**, SDIO0..3=1..4). **LCD reset is the AXP2101 `ALDO3` rail, not a MCU GPIO** — `board_init.cpp` pulses ALDO3 HIGH→LOW→HIGH (100 ms holds) *before* `display_hal_init()`. This pulse is **required**: skip it and the SH8601 stays in an indeterminate state — black screen even though QSPI init and brightness writes succeed. The GFX driver gets `GFX_NOT_DEFINED` for reset because it doesn't drive the line (the PMU rail does). (Note: `board.h`'s comment calling this "internal power-on reset" is misleading — the ALDO3 pulse in `board_init.cpp` is what actually resets the panel.)
- Touch: **CST9217** via I2C (SDA=8, SCL=7, INT=5, RST=11, addr=0x5A). Register-compatible with the 2.16's CST9220 — same SensorLib `CST92xx` driver.
- PMU: AXP2101 @ 0x34. IMU: QMI8658 @ 0x6B (present + initialized for I2C bus health; rotation off).
- IO expander: TCA9554 present but only services the audio amp — `BOARD_HAS_IO_EXPANDER 0` (unlike the 1.8, the expander does **not** gate display/touch reset, so no pre-init release needed).
- Buttons: **GPIO 9** (BOOT → refresh via `net_request_refresh()`), **AXP2101 PKEY** (PWR → short press cycles screens / on splash cycles animations; **long hold = hardware power-off** via the AXP, press again to power on — handled in `power.cpp`), **GPIO 10** (KEY → secondary, currently unused/reserved; the KEY GPIO is undocumented by Waveshare, identified empirically).
- Orientation: fixed — `BOARD_HAS_ROTATION 0` (no PSRAM headroom for the rotation strip).
- **No PSRAM foot-guns:** no `-DBOARD_HAS_PSRAM`; shared code gates on it to use `MALLOC_CAP_INTERNAL` and shrink LVGL buffers + splash canvas. `LV_USE_SNAPSHOT=0` and the `screenshot` command prints `SCREENSHOT_UNSUPPORTED` — a full-frame RGB565 buffer (~460 KB) doesn't fit in C6 internal SRAM, so QA UI changes on the 2.16 or 1.8 instead.
- **USB serial:** C6 has only USB-Serial-JTAG (HWCDC), no native USB-OTG. Serial maps to HWCDCSerial **only** with both `-DARDUINO_USB_CDC_ON_BOOT=1` and `-DARDUINO_USB_MODE=1`; `CDC_ON_BOOT` alone selects TinyUSB (absent on C6) and breaks the build.
- Flash: 16 MB part needed (`default_16MB.csv`) — the firmware (~1.43 MB) overflows the default 1.25 MB app partition.
