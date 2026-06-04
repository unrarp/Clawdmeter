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
- Buttons: GPIO 0 (left → force immediate `/usage` refresh via `net_request_refresh()`), GPIO 18 (right — currently unused/reserved), AXP PKEY (middle → short press cycles screens / on splash cycles animations; **long hold = hardware power-off** via the AXP, press again to power on). On battery the panel dims after ~6 min idle and the firmware auto-powers-off after ~12 min via `pmu.shutdown()` (`idle.cpp`).

## AMOLED-1.8 (newer port)
- Display: **SH8601** AMOLED via QSPI (CS=12, **SCLK=11** ← different!, SDIO0..3=4..7, RST routed via XCA9554 EXIO1)
- Touch: **FT3168** via I2C (SDA=15, SCL=14, INT=21, addr=0x38). Driven by minimal inline reader in `main.cpp` (FocalTech standard register layout — avoids vendoring the GPLv3 `Arduino_DriveBus` library).
- PMU: AXP2101 @ 0x34 (same chip as 2.16 — `XPowersLib` reused; battery is an optional kit add-on but PMU + charging circuitry are populated)
- IMU: QMI8658 @ 0x6B (same chip — initialized for I2C bus health, rotation logic disabled)
- IO expander: **XCA9554 / PCA9554** @ I2C 0x20. Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button. **`io_expander_init()` MUST run before `gfx->begin()` or `ft3168_init()`** — otherwise display/touch stay in reset and silently fail. PWR button is on EXIO4, active HIGH (verified empirically with the deleted `iox` serial debug command).
- Orientation: **fixed at 0°**. IMU auto-rotation is disabled; `rotate_strip()` / `handle_rotation_change()` are excluded via `#ifndef BOARD_AMOLED_18`.
- Buttons: GPIO 0 (BOOT → force immediate `/usage` refresh via `net_request_refresh()`), XCA9554 EXIO4 (PWR → **short** press cycles screens / on splash cycles animations; **long hold = hardware power-off** via the AXP PWRON pin, press again to power on). The firmware reads EXIO4 on a short *release* (held < 1 s) so a power-off hold doesn't also flip a page. On battery the panel dims after ~6 min idle and the firmware auto-powers-off after ~12 min via `pmu.shutdown()` (`idle.cpp`). **No third button** (GPIO 18 button doesn't exist on this board).
