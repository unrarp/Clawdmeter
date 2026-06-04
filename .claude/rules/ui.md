---
paths:
  - "firmware/src/ui.h"
  - "firmware/src/ui.cpp"
  - "firmware/src/data.h"
  - "firmware/src/main.cpp"
---

# UI rules

- **Use a static screen cycle with a "No account" panel, not dynamic page-hiding.** When adding a new provider screen, keep the cycle fixed and render "No account" for an absent provider rather than removing the page from the cycle. Dynamic hiding requires `screen_enabled()`, skip-disabled logic in `ui_cycle_screen`, redirect-on-show in `ui_show_screen`, reconcile-on-payload (snap to next enabled screen when the current one disappears), and a guard on `prev_non_splash_screen` — four fiddly state transitions. The static approach deletes all of this: the cycle is unconditional, `ui_update_provider` handles the absent state via the `present` flag already on the wire. → see `docs/decisions/2026-06-02-static-screen-cycle.md`

- **Fixed-layout panels overflow silently — verify row count *and* text width on the narrowest display before committing a layout change.** Screens place elements at absolute y-offsets in a fixed-height panel with no scroll, reflow, or text wrap, so going over budget runs off-panel / into neighbours, and an over-long label clips mid-string — both with no assertion. The narrowest, shortest board is the binding constraint (here the 1.8″ / 368×448, whose info panels hold ~6 rows). Before adding a row, drop or merge one; prefer a compact label form over an appended qualifier (e.g. make an entity's name the row prefix — `Claude: live` — rather than a parenthetical suffix — `Usage: live (claude)` — which clipped at 368px). Always screenshot before/after with `screenshot.sh` (see AGENTS.md "QA your own UI changes").

## Related decisions

- `2026-06-02-static-screen-cycle` — why a fixed Splash→Claude→Codex→WiFi cycle with "No account" panels beats dynamic page-hiding.
