#pragma once

// Tracks the short-term rate of change in each provider's session_pct (%/min) so
// the UI can react to *how heavily* a provider is being used right now, not just
// its current level.
//
// Rate is tracked PER PROVIDER and the splash tiers off the fastest-climbing one
// (usage_rate_group returns the max across providers). This matters: a provider
// sitting at a high but static utilization (e.g. Codex idle at 55%) must not mask
// another provider actively climbing (e.g. Claude rising at 0.5 %/min). Collapsing
// to the max *level* first and tracking that single number's rate did exactly that
// — the busy provider stayed invisible and the splash showed idle.

// Feed a provider's latest session percentage every time fresh data arrives for
// it. `provider` is a PROV_* index (0 .. PROVIDER_COUNT-1); out-of-range is ignored.
void usage_rate_sample(int provider, float session_pct);

// 0 = idle, 1 = normal, 2 = active, 3 = heavy. Returns the MAX group across all
// providers. Each provider defaults to 0 until its own buffer has enough samples.
int usage_rate_group(void);
