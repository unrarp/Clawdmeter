#include <Arduino.h>
#include "idle.h"
#include "idle_cfg.h"
#include "hal/display_hal.h"
#include "hal/power_hal.h"

// Inactivity ladder, battery only:
//   < IDLE_DIM_MS      : full brightness
//   >= IDLE_DIM_MS     : panel dimmed (DISPLAY_DIM_BRIGHTNESS) to cut draw
//   >= IDLE_TIMEOUT_MS : full power-off via the PMU (power_hal_shutdown)
//
// "Activity" resets the timer and is either a physical touch/button OR a real
// change in the usage numbers — both call idle_note_activity(). Because the dim
// threshold (IDLE_DIM_MS, see idle_cfg.h) sits just past the ~5-min daemon data
// cadence, a live usage change keeps the panel bright without any extra timer.
// Waking from dim is any activity; waking from power-off is a PWR-key press (a
// full reboot). On USB nothing dims or powers off, and the window restarts when
// USB is unplugged. Brightness is only written on a transition (no per-tick I2C).

static uint32_t last_activity_ms = 0;
static bool     dimmed = false;

// Restore full brightness if we'd dimmed. Cheap no-op when already bright.
static void undim(void) {
    if (dimmed) {
        display_hal_set_brightness(DISPLAY_DEFAULT_BRIGHTNESS);
        dimmed = false;
    }
}

void idle_init(void) {
    last_activity_ms = millis();
    dimmed = false;
    display_hal_set_brightness(DISPLAY_DEFAULT_BRIGHTNESS);
}

void idle_note_activity(void) {
    last_activity_ms = millis();
    undim();
}

void idle_tick(void) {
    // Never dim/auto-off on USB power: keep a desk-plugged device fully lit, and
    // start a fresh idle window from the moment USB is unplugged.
    if (!IDLE_POWEROFF_WHEN_CHARGING && power_hal_is_vbus_in()) {
        last_activity_ms = millis();
        undim();
        return;
    }
    uint32_t idle_ms = millis() - last_activity_ms;
    if (idle_ms >= IDLE_TIMEOUT_MS) {
        power_hal_shutdown();
    } else if (idle_ms >= IDLE_DIM_MS && !dimmed) {
        display_hal_set_brightness(DISPLAY_DIM_BRIGHTNESS);
        dimmed = true;
    }
}
