#pragma once

// Inactivity dim + auto power-off (battery only). "Activity" is a physical
// touch/button OR a real change in the usage numbers — so the device stays lit
// while your usage is moving. Those numbers only refresh as fast as the daemon
// polls upstream (~5 min), so these thresholds are sized against that 5-min data
// cadence, NOT the 60 s device fetch interval: dim just past one data cycle (so
// live data keeps the panel bright), power off at roughly two.

#define IDLE_DIM_MS                 (6UL  * 60UL * 1000UL)  // 6 min idle  -> dim panel
#define IDLE_TIMEOUT_MS             (12UL * 60UL * 1000UL)  // 12 min idle -> power off

#define DISPLAY_DEFAULT_BRIGHTNESS  200      // active-screen brightness
// AMOLED current scales ~linearly with brightness, so dimming trims idle draw.
#define DISPLAY_DIM_BRIGHTNESS      40       // dimmed brightness (IDLE_DIM_MS .. IDLE_TIMEOUT_MS)

// When false (default), auto power-off only fires on battery — never while USB
// is connected (a desk-plugged device stays on, and the AXP won't stay off with
// VBUS present anyway). Set true to power off regardless of power source.
#define IDLE_POWEROFF_WHEN_CHARGING false
