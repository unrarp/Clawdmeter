---
date: 2026-06-03
module: power
tags: [power-off, deep-sleep, axp2101, pwron, gpio0, strapping-pin, battery, idle, pmu-shutdown, amoled-18]
---

# AXP2101 hardware power-off, not ESP32 deep sleep, for the AMOLED-1.8

## Context
The device had no off switch, and the old idle path only faded the screen to
black while leaving the ESP, WiFi, AXP rails, and AMOLED panel fully powered — no
battery saving. We wanted a manual power button plus an inactivity auto-shutoff
that actually saves battery.

## Decision
Use the AXP2101 hardware power-key path, driven by the **PWR button** (the one that
cycles screens), not the refresh button.

- The PWR button is wired to the **AXP2101 PWRON pin** and also read by firmware via
  the XCA9554 expander. PWRON wiring means a long hold powers off in hardware (AXP
  `OFFLEVEL`, ~6 s default) and a press powers back on — no firmware config needed
  (confirmed on battery). Wake is a full reboot, not a resume.
- Firmware adds `power_hal_shutdown()` → `pmu.shutdown()` for the battery inactivity
  auto-off in `idle.cpp` (6 min dim → 12 min power-off).
- The 1.8's `power_hal_pwr_pressed()` fires on a **short release** (held < 1 s), so a
  long power-off hold doesn't also cycle a page on the way down.

## Why
`pmu.shutdown()` cuts every rail except VRTC; since `DCDC1` (= `VCC3V3`) supplies the
ESP, the whole board dies → draw collapses to ~µA, the lowest-power state the hardware
can reach. The PWR→PWRON wiring makes wake hardware-guaranteed (works even if firmware
hangs).

## Alternatives considered
- **ESP32 deep sleep woken by the refresh button (GPIO0)** — the user's initial
  preference. Rejected for three non-obvious reasons:
  1. **GPIO0 is the ESP32-S3 BOOT strapping pin.** A wake is a reset; GPIO0 held low
     (the wake press) when the ROM samples straps can boot into **USB download mode
     (dead screen)** instead of the app. Unverified hazard, would need a bench test.
  2. **Deep sleep only powers down the ESP (~10 µA).** AXP rails + panel stay
     energized (tens of mA) — a "fake off," far higher draw than an AXP shutdown.
  3. **GPIO0 cannot power the AXP back on** (only PWRON can), so making refresh the
     power button forces deep sleep, which can never reach true-off.
- **Deep sleep woken by the PWR button** — impossible: PWR is behind the XCA9554
  expander, not a GPIO, so it can't be an `ext0`/`ext1` wake source.

## Prevention
- **Auto-off is battery-only.** With USB present the AXP bounces back on, so `idle.cpp`
  gates shutdown on `!power_hal_is_vbus_in()`.
- **The data-change idle reset excludes `*_reset_mins`.** Those countdowns tick every
  poll; including them in `usage_changed()` would reset the idle timer forever and
  defeat auto-off.

## Related
- CLAUDE.md (button descriptions); `.claude/rules/boards.md`;
  `firmware/src/idle.cpp`, `hal/power_hal.h`, `boards/waveshare_amoled_18/power.cpp`.
