---
date: 2026-06-02
module: firmware/ui
tags: [screen-cycle, ui, presence, provider, dynamic-hiding]
---

# Static screen cycle with "No account" panel vs dynamic page-hiding

## Context
Adding a second provider screen (Codex alongside Claude) raised the question:
for a single-account user, hide the absent provider's page from the cycle, or
keep it always present showing a placeholder? The initial plan called for
dynamic hiding gated on presence flags.

## Decision
Static screens: `Claude → Codex → WiFi` (forward, repeating), Splash toggled in/out
separately via `ui_toggle_splash()`. An absent provider renders a "No account"
panel; the cycle never changes. `ui_update_provider` handles all four states via a
flat if/return chain keyed on `ProviderUsage.present`.

## Why
Dynamic hiding needs four interacting pieces — `screen_enabled()`, skip-disabled in
`ui_cycle_screen`, redirect-on-show in `ui_show_screen`, and reconcile-on-payload
(snap off a screen that just became disabled, guard `prev_non_splash_screen`). Each
is simple alone, but together they form a web of edge cases (both providers absent;
account removed mid-cycle; `prev_non_splash_screen` pointing at a now-disabled screen
on a splash toggle). The static approach deletes all four — the only firmware change
is a flat branch in `ui_update_provider`.

> **Post-cutover note:** `present` was originally a daemon `sp`/`cp` wire flag. After
> the token-broker cutover the device fetches each provider directly with no wire-JSON
> layer, and `present` is now **always `true`** — a provider needing re-auth surfaces
> via the WiFi-page health verdict, not a blanked panel. The `present=false` branch is
> latent, kept for the absent-provider case the static design still supports.

## Alternatives considered
- **Dynamic hiding** (initial plan): cleaner UX for single-account users, but the
  four-piece machinery above (esp. reconcile-on-payload) was the deciding cost.
- **Hide on first boot only:** rejected — "first boot" is indistinguishable from
  "account just removed" device-side, so it needs daemon-side persistent state.

## Related
- `.claude/rules/ui.md`; `docs/plans/2026-06-02-codex-usage.md` §D.2 (screen plumbing).
