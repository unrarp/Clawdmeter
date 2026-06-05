---
date: 2026-06-05
module: splash / usage_rate
tags: [usage_rate, splash, animation, rate tier, per-provider, quantization, ring buffer, idle, masking]
---

# Per-provider usage-rate tiering for splash animations

## Context
The device showed only idle splash animations during active Claude use. The
splash picks an animation group from `usage_rate_group()`, which tiers off the
**rate of climb** of the 5-hour usage percentage (idle/normal/active/heavy), not
the absolute level. Two independent defects kept it pinned to idle:

1. The rate tracker was fed `max_present_session_pct(usage)` — the single highest
   session % across providers — then tracked that one number's rate. Live
   measurement: Claude was climbing ~0.5 %/min while Codex sat flat at 55%. Since
   55 > Claude's %, the tracker saw a flat 55 every sample → rate ≈ 0 → idle. The
   busy provider was invisible.
2. Even once Claude was the dominant provider (41%), it still read idle: the
   upstream `anthropic-ratelimit-unified-5h-utilization` header is a 2-decimal
   fraction (whole-percent resolution after ×100), and usage is bursty. With a
   ~5-min window (`RING_SIZE` 6) the rate often sampled a flat patch between
   bursts → idle.

## Decision / Solution
Track rate **per provider** and tier off the fastest-climbing one:
- `usage_rate_sample(int provider, float pct)` keeps a ring buffer per `PROV_*`;
  `usage_rate_group()` returns the **max** group across providers
  (`usage_rate.cpp`). `main.cpp` feeds every present provider each fetch.
- Widened the window: `RING_SIZE` 6 → 10 (≈9 min at ~60s fetch).
- Added a staleness guard in `group_for`: if `millis() - newest_sample >
  3×MIN_WINDOW`, return idle, so a provider that stops reporting decays instead of
  freezing its ring at the last tier.
- Removed `max_present_session_pct` (no longer needed).

## Why
- **Per-provider, max-group** is correct because "how heavily is *any* provider
  being used" is a max over independent signals, not a property of the single
  highest-level one. Each provider also detects its own session reset
  independently (Claude/Codex reset on different clocks).
- **Window length is bounded on both sides by the 1% quantization.** The smallest
  non-zero rate the tracker can see is `1% ÷ window`. For a single whole-percent
  crossing to register as Normal (≥0.10 %/min) the window must be ≤~10 min; going
  much longer (15 min → 0.067 %/min) drops a single crossing *below* Normal. At
  ~9 min: one crossing = Normal (≈0.11), two = Active, three = Heavy, and brief
  flat patches between bursts no longer collapse the rate to zero.

## Alternatives considered
- **Lower the rate thresholds** (e.g. Normal at 0.05 %/min). Rejected: with
  whole-percent source data the binding constraint is `1% ÷ window`, not the
  threshold values — sub-0.1 %/min is simply unreachable as a single crossing in
  a short window. Widening the window is the effective lever.
- **Keep the single shared ring, feed it the max.** This was the buggy original;
  it structurally cannot see a climbing non-max provider.

## Prevention
- Host equivalence test (`g++` compile of `usage_rate.cpp` with stub
  `Arduino.h`/`data.h`) pins the masking case: old max-collapse logic → group 0
  (idle), new per-provider → group 3 (heavy).
- Verified on hardware via serial capture: after a fresh flash and ~4-min
  warm-up, `usage rate: group 0 -> 2 -> 3` transitions fired while Claude climbed
  45 → 47%. The residual active⇄heavy oscillation is the quantization hovering at
  the Heavy threshold — both are "busy" tiers, so it never falls back to idle.

## Related
- `.claude/rules/splash.md` — animation rate-tier rules.
- `2026-06-05-condition-driven-splash-animations` — name-keyed condition clips, a
  sibling splash-selection mechanism.
