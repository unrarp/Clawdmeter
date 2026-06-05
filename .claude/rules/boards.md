---
paths:
  - "firmware/src/boards/**"
---

# Board / hardware rules

Per-board critical pins, I2C addresses, and bring-up order. Adding a board = new
`boards/<name>/` folder + `[env:...]` block; see
[`docs/porting/adding-a-board.md`](../../docs/porting/adding-a-board.md).

## Common to all boards
- **PWR button** (the AXP `PKEY`/`PWRON`-wired one): short press cycles screens / on
  splash cycles animations; **long hold = hardware power-off via the AXP**, press again
  to power on.
- **Refresh button** (BOOT/GPIO0 unless noted): forces an immediate usage refresh via
  `net_request_refresh()`.
- **Battery idle (where a battery is fitted):** panel dims after ~6 min idle,
  auto-powers-off after ~12 min via `pmu.shutdown()` (`idle.cpp`).
- Below, each board lists only its **deltas** from the above.

## Build env and flash — confirm the board before acting
**Always pass `-e <env>` explicitly; never rely on the PlatformIO default** (which is
`waveshare_amoled_216`). The user's current device is the **1.8″ board
(`waveshare_amoled_18`)**. Flashing the wrong env gives a **dark screen with no error**
— the MCU runs and USB enumerates but the panel stays in reset. Verify first:
`screenshot.sh` returns `480×480` for `_216` and `368×448` for `_18`.
Correct flash: `pio run -d firmware -e waveshare_amoled_18 -t upload --upload-port /dev/ttyACM0`.

## AMOLED-2.16 (original)
- Display: **CO5300** AMOLED via QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- Touch: **CST9220** via I2C (SDA=15, SCL=14, INT=11, addr=0x5A)
- PMU: **AXP2101** @ 0x34 (battery, USB VBUS, PWR button IRQ). IMU: **QMI8658** @ 0x6B
  (accelerometer for auto-rotation)
- Buttons: GPIO0 (refresh), GPIO18 (reserved/unused), AXP PKEY (PWR)

## AMOLED-1.8 (newer port)
- Display: **SH8601** AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7,
  RST via XCA9554 EXIO1)
- Touch: **FT3168** via I2C (SDA=15, SCL=14, INT=21, addr=0x38). Driven by a minimal
  inline reader in this board's `touch.cpp` (FocalTech standard registers — avoids
  vendoring the GPLv3 `Arduino_DriveBus`).
- PMU: AXP2101 @ 0x34 (battery is an optional kit add-on; PMU + charging populated).
  IMU: QMI8658 @ 0x6B (init for I2C bus health; rotation disabled)
- IO expander: **XCA9554 / PCA9554** @ 0x20 — gates LCD_RST, TP_RST, audio amp enable,
  reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` /
  `touch_hal_init()`** or display/touch stay in reset and silently fail. PWR is on EXIO4,
  active HIGH (polarity verified empirically, not from a schematic).
- Orientation: **fixed at 0°** via the HAL, **not** an `#ifdef` (gotcha #10):
  `BOARD_HAS_ROTATION 0` and `imu_hal_rotation_quadrant()` returns 0; the strip-remap
  (`rotate_strip()`) lives only in the 2.16's `display.cpp`.
- Buttons: GPIO0 (refresh), XCA9554 EXIO4 (PWR). **No third button** (no GPIO18).
  Delta: PWR is read on a short *release* (held < 1 s) so a power-off hold doesn't also
  flip a page.

## AMOLED-2.16-C6 (C6 SoC sibling of the 2.16)
Build env: `waveshare_amoled_216_c6` (`-DBOARD_AMOLED_216_C6`). Same 480×480 form factor
but a **single-core RISC-V ESP32-C6, no PSRAM, BLE 5.3 only**, and an **SH8601** panel
(`Arduino_SH8601`, GFX lib ≥1.6.4), not the 2.16's CO5300.
- Display: **SH8601** via QSPI (CS=15, **SCLK=0**, SDIO0..3=1..4). **LCD reset is the
  AXP2101 `ALDO3` rail, not a MCU GPIO** — `board_init.cpp` pulses ALDO3
  HIGH→LOW→HIGH (100 ms holds) *before* `display_hal_init()`. **Required:** skip it and
  the SH8601 stays indeterminate (black screen) even though QSPI init + brightness
  writes succeed. (`board.h`'s "internal power-on reset" comment is misleading — the
  ALDO3 pulse is what resets the panel.)
- Touch: **CST9217** via I2C (SDA=8, SCL=7, INT=5, RST=11, addr=0x5A). Register-compatible
  with the 2.16's CST9220 — same SensorLib `CST92xx` driver.
- PMU: AXP2101 @ 0x34. IMU: QMI8658 @ 0x6B (init for bus health; rotation off).
- IO expander: TCA9554 present but only services the audio amp — `BOARD_HAS_IO_EXPANDER 0`
  (does **not** gate display/touch reset, so no pre-init release needed).
- Orientation: fixed — `BOARD_HAS_ROTATION 0` (no PSRAM headroom for the strip).
- Buttons: GPIO9 (refresh), AXP2101 PKEY (PWR, in `power.cpp`), GPIO10 (KEY → secondary,
  reserved; GPIO undocumented by Waveshare, found empirically).
- **No-PSRAM foot-guns:** no `-DBOARD_HAS_PSRAM`; shared code gates on it to use
  `MALLOC_CAP_INTERNAL` and shrink LVGL buffers + splash canvas. `LV_USE_SNAPSHOT=0`
  and `screenshot` prints `SCREENSHOT_UNSUPPORTED` (a ~460 KB full-frame RGB565 buffer
  doesn't fit in C6 internal SRAM) — QA UI changes on the 2.16 or 1.8 instead.
- **USB serial:** C6 has only USB-Serial-JTAG (HWCDC). Serial maps to HWCDCSerial **only**
  with both `-DARDUINO_USB_CDC_ON_BOOT=1` and `-DARDUINO_USB_MODE=1`; `CDC_ON_BOOT`
  alone selects TinyUSB (absent on C6) and breaks the build.
- Flash: 16 MB part needed (`default_16MB.csv`) — firmware (~1.43 MB) overflows the
  default 1.25 MB app partition.

## Related decisions

- `2026-06-03-axp-power-off-vs-deep-sleep` — why the AXP2101 PWRON power-off path
  (not ESP32 deep sleep) drives the PWR button + battery auto-off.
