#pragma once
#include "data.h"
#include "net.h"

// Screen order: Splash, then one screen per provider (SCREEN_PROVIDER_BASE + i
// for provider index i in [0, PROVIDER_COUNT)), then WiFi. Keep this range
// contiguous — ui.cpp maps a provider screen back to its index as
// (screen - SCREEN_PROVIDER_BASE). Adding a provider grows the cycle here for
// free; no per-screen enum entry to add.
enum screen_t {
    SCREEN_SPLASH,
    SCREEN_PROVIDER_BASE,
    SCREEN_WIFI = SCREEN_PROVIDER_BASE + PROVIDER_COUNT,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_wifi_status(net_state_t state, const char* ssid, const char* ip,
                           uint32_t last_update_ms,
                           const usage_health_t* health);  // health[PROVIDER_COUNT]
void ui_update_battery(int percent, bool charging);
