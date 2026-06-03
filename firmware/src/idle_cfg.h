#pragma once

// Inactivity auto power-off configuration. After IDLE_TIMEOUT_MS with no
// activity, and only on battery, the device powers fully off via the PMU.

#define IDLE_TIMEOUT_MS             (5UL * 60UL * 1000UL)  // 5 min idle -> power off

#define DISPLAY_DEFAULT_BRIGHTNESS  200      // active-screen brightness

// When false (default), auto power-off only fires on battery — never while USB
// is connected (a desk-plugged device stays on, and the AXP won't stay off with
// VBUS present anyway). Set true to power off regardless of power source.
#define IDLE_POWEROFF_WHEN_CHARGING false
