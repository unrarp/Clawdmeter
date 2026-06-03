#pragma once
#include <stdbool.h>

// Inactivity timer that powers the device fully off after a period with no
// activity (battery only). No screen-sleep state — the panel stays lit until
// shutdown. See idle.cpp / idle_cfg.h.

void idle_init(void);
void idle_tick(void);            // call once per loop: powers off when idle expires
void idle_note_activity(void);   // reset the idle timer (button/touch/data change)
