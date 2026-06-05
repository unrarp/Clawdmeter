---
date: 2026-06-02
module: daemon
tags: [rate-limit, oauth, api-oauth-usage, poll-interval, backoff]
---

> **Status: historical / rejected alternative (2026-06-04).** Nothing in the
> repo hits `/api/oauth/usage` anymore — the polling daemon described below was
> deleted at the token-broker cutover (`284c496`). The device now scrapes
> `anthropic-ratelimit-unified-*` headers off a `POST /v1/messages`
> (`max_tokens:1`) call, which has generous limits. Kept for the empirical
> evidence that ruled the oauth endpoint out — re-read before anyone
> reintroduces it.

# `/api/oauth/usage` is rate-limited (~1 req/min) — rejected

## Context

Upstream PR #29 proposed switching usage polling from a 1-token
`POST /v1/messages` header-scrape to the purpose-built `GET /api/oauth/usage`
(structured JSON, no token cost). It merged, then reverted in PR #37 after
rate-limiting showed up in practice. Our commit `9ac470b` made the same switch
with a fixed 300s interval + 60s fail-backoff that upstream never landed.

## Why it was rejected

The endpoint rate-limits at roughly 1 req/min; faster returns `429`
(`retry-after`) or `529` (overload):

| Cadence | Observed |
|---|---|
| ~1 req/min (7 rapid) | 200, 200, 529, 529, 429, 429, 429 |
| 60s spacing | 200, 200, 529, 529 |
| 300s spacing | two 200s; stable across 24h |

Even exponential backoff to a 5-minute ceiling (BrianPugh, post-revert) still
hit limits — suggesting the constraint is cumulative session history, not burst
rate. A fixed 300s stays under the empirical limit from the first call. The
fail-backoff was load-bearing: without it a single 429 let the 5s inner tick
loop storm the endpoint — the "5-second storm" that caused the upstream revert.

## Alternatives considered

- **60s fixed (PR #29, original daemon):** hit limits within 2–4 polls.
- **Exponential backoff to 5min (PR #37 follow-up):** still hit limits at the
  ceiling — cumulative-history problem, not burst.
- **`POST /v1/messages` header-scrape (current approach):** generous limits, but
  burns one token per poll, spoofs the `claude-code/*` UA, and yields only Unix
  reset timestamps (no ISO-8601).

## Prevention

If anyone reintroduces `/api/oauth/usage`, do not poll below 300s without
re-verifying the limit relaxed — the 429 `retry-after` (~101s observed) is the
burst penalty, a lower bound, not a safe sustained cadence.

## Related

- Upstream PR [#29](https://github.com/HermannBjorgvin/Clawdmeter/pull/29) /
  [#37](https://github.com/HermannBjorgvin/Clawdmeter/pull/37)
- `.claude/rules/daemon.md`
