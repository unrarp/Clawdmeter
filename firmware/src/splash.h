#pragma once
#include <lvgl.h>
#include <stdint.h>

// Initialize splash module. Creates the canvas widget inside `parent` and
// allocates the 480x480 pixel buffer (PSRAM).
void splash_init(lv_obj_t* parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// Feed the latest battery state in (percent 0..100 or -1, charging flag). Below
// a threshold (and not charging) the splash shows a dedicated low-battery clip
// instead of the usual rate-driven pick, re-picking immediately when the
// condition flips. Call whenever the battery reading changes.
void splash_set_battery(int pct, bool charging);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);
