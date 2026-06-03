---
date: 2026-06-03
module: power
tags: [power-off, deep-sleep, axp2101, pwron, gpio0, strapping-pin, battery, idle, pmu-shutdown, amoled-18]
---

# AXP2101 hardware power-off, not ESP32 deep sleep, for the AMOLED-1.8

## Context

The device had no off switch, and the old idle path only faded the screen to
black while leaving the ESP, WiFi, AXP rails, and AMOLED panel fully powered —
no battery saving. We wanted (1) a manual power button and (2) an inactivity
auto-shutoff that actually saves battery on the AMOLED-1.8 kit.

## Decision / Solution

Use the AXP2101's hardware power-key path, driven by the **PWR button** (the
one that cycles screens), not the refresh button.

- The 1.8 PWR button (schematic `Key3`) is wired to the **AXP2101 PWRON pin**
  *and* read by firmware via XCA9554 `EXIO4` (through a BSS138 inverter →
  `SYS_OUT`, active-HIGH). The PWRON wiring means a long hold powers the board
  off in hardware (AXP `OFFLEVEL`, chip default ~6 s) and a press powers it back
  on — **no firmware config needed** (confirmed empirically on battery: hold off,
  press on).
- Firmware adds `power_hal_shutdown()` → `pmu.shutdown()` (`hal/power_hal.h`, all
  board ports) for the 5-min battery auto-off in `idle.cpp`.
- The 1.8's `power_hal_pwr_pressed()` fires on a **short release** (held < 1 s),
  not the press edge, so a long power-off hold doesn't also cycle a page on the
  way down (`boards/waveshare_amoled_18/power.cpp`).
- Wake from off is a full reboot, not a resume.

## Why

`pmu.shutdown()` cuts every rail except VRTC. Since `DCDC1 = VCC3V3` supplies the
ESP, cutting it kills the whole board → draw collapses to ~µA. That is the
lowest-power state the hardware can reach, and the PWR→PWRON wiring makes the
wake path hardware-guaranteed (works even if firmware hangs).

## Alternatives considered

- **ESP32 deep sleep woken by the refresh button (GPIO0)** — the user's initial
  preference. Rejected for three reasons a code reader wouldn't independently
  hit:
  1. **GPIO0 is the ESP32-S3 BOOT strapping pin.** A deep-sleep wake is a reset;
     if GPIO0 is held low (the wake press) when the ROM samples straps, the chip
     can come up in **USB download mode (dead screen)** instead of running the
     app. An unverified hazard that would need a bench test before relying on it.
  2. **Deep sleep only powers down the ESP (~10 µA).** The AXP rails and the
     AMOLED panel stay energized (tens of mA) unless separately cut — a "fake
     off," far higher draw than an AXP shutdown.
  3. **GPIO0 cannot power the AXP back on** (only PWRON can), so making the
     refresh button the power button *forces* deep sleep, which can never reach
     the true-off state.
- **Deep sleep woken by the PWR button** — impossible: PWR is behind the XCA9554
  IO expander (EXIO4), not a GPIO, so it can't be an `ext0`/`ext1` wake source.

## Prevention

- **Auto-off is battery-only.** With USB present the AXP won't *stay* off (VBUS
  bounces it back on), so `idle.cpp` gates shutdown on `!power_hal_is_vbus_in()`
  (`IDLE_POWEROFF_WHEN_CHARGING` defaults false).
- **The data-change idle reset excludes `*_reset_mins`.** Those countdowns tick
  every poll; including them in `usage_changed()` (`main.cpp`) would reset the
  idle timer forever and defeat auto-off.

## Related

- `CLAUDE.md` — AMOLED-1.8 / 2.16 button descriptions (short press vs long-hold).
- `firmware/src/idle.cpp`, `firmware/src/hal/power_hal.h`,
  `firmware/src/boards/waveshare_amoled_18/power.cpp`.
