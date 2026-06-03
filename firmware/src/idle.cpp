#include <Arduino.h>
#include "idle.h"
#include "idle_cfg.h"
#include "hal/display_hal.h"
#include "hal/power_hal.h"

// Inactivity -> full power-off. After IDLE_TIMEOUT_MS with no activity, and only
// while on battery, the device powers itself off via the PMU (power_hal_shutdown).
// Waking is a PWR-key press — a full reboot. There is no intermediate
// screen-off/sleep state: the panel stays lit until shutdown.

static uint32_t last_activity_ms = 0;

void idle_init(void) {
    last_activity_ms = millis();
    display_hal_set_brightness(DISPLAY_DEFAULT_BRIGHTNESS);
}

void idle_note_activity(void) {
    last_activity_ms = millis();
}

void idle_tick(void) {
    // Never auto-off on USB power: keep a desk-plugged device on, and start a
    // fresh idle window from the moment USB is unplugged.
    if (!IDLE_POWEROFF_WHEN_CHARGING && power_hal_is_vbus_in()) {
        last_activity_ms = millis();
        return;
    }
    if (millis() - last_activity_ms >= IDLE_TIMEOUT_MS) {
        power_hal_shutdown();
    }
}
