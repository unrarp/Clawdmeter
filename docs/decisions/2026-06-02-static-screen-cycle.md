---
date: 2026-06-02
module: firmware/ui
tags: [screen-cycle, ui, presence, provider, dynamic-hiding]
---

# Static screen cycle with "No account" panel vs dynamic page-hiding

## Context

Adding a second provider screen (Codex alongside Claude) raised the question: when a user has only one account, should the absent provider's page be hidden from the cycle entirely, or always present but showing a placeholder?

The initial plan spec called for dynamic hiding: `sp`/`cp` presence flags would gate whether `SCREEN_USAGE`/`SCREEN_CODEX` appeared in the cycle at all.

## Decision / Solution

Static screens: `Claude → Codex → WiFi` cycle (forward, repeating), with Splash toggled in/out separately via `ui_toggle_splash()`. An absent provider renders a "No account" panel (`ui_update_provider` with `present=false` shows `"--%"` and `"No Claude account"` / `"No OpenAI account"`). The cycle never changes; `ui_update_provider` handles all four states via a flat if/return chain.

## Why

Dynamic hiding requires four distinct pieces of machinery that interact:

1. **`screen_enabled(screen)`** — a helper returning whether a given screen is in the cycle, driven by the latest `UsageData` presence flags.
2. **Skip-disabled in `ui_cycle_screen`** — advance to the next *enabled* screen, skipping disabled ones.
3. **Redirect-on-show in `ui_show_screen`** — if asked to show a disabled screen, redirect to the next enabled one.
4. **Reconcile-on-payload** — when new data changes presence, if the *currently shown* screen just became disabled, snap to the next enabled screen; also guard `prev_non_splash_screen` to never point at a disabled screen.

Each of these is individually simple, but together they form a web of edge cases: what if both providers are absent (only WiFi in the forward cycle, both provider screens show "No account")? What if the user is on the Codex screen and removes their account mid-cycle? What if `prev_non_splash_screen` points at a now-disabled screen on a click-to-toggle-splash action?

The static approach deletes all four: the cycle is unconditional, the only firmware change is a flat branch in `ui_update_provider`. The `sp`/`cp` presence flags are still emitted by the daemon and parsed by the firmware — they select the "No account" message text, nothing else.

## Alternatives considered

**Dynamic hiding** (initial plan): pages removed from the cycle when the provider's creds file is absent. Cleaner UX for single-account users (no extra page to click past), but the implementation complexity was the deciding factor against it. The machinery listed above was prototyped mentally; the reconcile-on-payload step alone has multiple edge cases.

**Hybrid: hide only on first boot, show once account is added**: considered briefly and rejected — "first boot" is indistinguishable from "account recently removed" from the device's perspective, so this would require daemon-side persistent state.

## Related

- `.claude/rules/ui.md` — standing rule to use static cycle.
- `docs/plans/2026-06-02-codex-usage.md` §D.2 — full screen plumbing spec.
