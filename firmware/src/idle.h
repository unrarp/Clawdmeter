#pragma once
#include <stdbool.h>

// Inactivity timer (battery only): dims the panel after a short idle period,
// then powers the device fully off after a longer one. Any activity restores
// full brightness. See idle.cpp / idle_cfg.h.

void idle_init(void);
void idle_tick(void);            // call once per loop: dims, then powers off when idle expires
void idle_note_activity(void);   // reset the idle timer + un-dim (touch/button/usage change)
