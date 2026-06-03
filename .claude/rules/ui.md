---
paths:
  - "firmware/src/ui.h"
  - "firmware/src/ui.cpp"
  - "firmware/src/data.h"
  - "firmware/src/main.cpp"
---

# UI rules

- **Use a static screen cycle with a "No account" panel, not dynamic page-hiding.** When adding a new provider screen, keep the cycle fixed and render "No account" for an absent provider rather than removing the page from the cycle. Dynamic hiding requires `screen_enabled()`, skip-disabled logic in `ui_cycle_screen`, redirect-on-show in `ui_show_screen`, reconcile-on-payload (snap to next enabled screen when the current one disappears), and a guard on `prev_non_splash_screen` — four fiddly state transitions. The static approach deletes all of this: the cycle is unconditional, `ui_update_provider` handles the absent state via the `present` flag already on the wire. → see `docs/decisions/2026-06-02-static-screen-cycle.md`

## Related decisions

- `2026-06-02-static-screen-cycle` — why a fixed Splash→Claude→Codex→WiFi cycle with "No account" panels beats dynamic page-hiding.
