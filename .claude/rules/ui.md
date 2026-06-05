---
paths:
  - "firmware/src/ui.h"
  - "firmware/src/ui.cpp"
  - "firmware/src/data.h"
  - "firmware/src/main.cpp"
---

# UI rules

- **Use a static screen cycle with a "No account" panel, not dynamic page-hiding.**
  Keep the cycle fixed and render "No account" for an absent provider rather than
  removing the page. `ui_update_provider` handles the absent state via
  `ProviderUsage.present` (always `true` post-cutover, so that branch is latent).
  Dynamic hiding needs four interacting state transitions; the static cycle deletes
  them. → see `docs/decisions/2026-06-02-static-screen-cycle.md`

- **Fixed-layout panels overflow silently — verify row count *and* text width on the
  narrowest display before committing a layout change.** Elements sit at absolute
  y-offsets with no scroll/reflow/wrap, so an extra row runs off-panel and an over-long
  label clips mid-string, both without any assertion. The binding constraint is the
  1.8″ / 368×448 (~6 rows). Before adding a row, drop or merge one; prefer a compact
  label (`Claude: live`) over an appended qualifier (`Usage: live (claude)`, which
  clipped at 368px). Always screenshot before/after (`screenshot.sh`).

## Related decisions

- `2026-06-02-static-screen-cycle` — static cycle + "No account" vs dynamic hiding.
