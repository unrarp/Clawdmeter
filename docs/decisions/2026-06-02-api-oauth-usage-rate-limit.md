---
date: 2026-06-02
module: daemon
tags: [rate-limit, oauth, api-oauth-usage, poll-interval, backoff]
---

# `/api/oauth/usage` requires a ≥300s poll interval and a fail-backoff

## Context

The daemon (`daemon/claude_usage_daemon.py`, a single cross-platform Python
file) polls Anthropic for the user's session/weekly usage and serves it over
HTTP; the ESP32 pulls `GET /usage` on its own cadence. The original approach
(pre-commit `9ac470b`) made a throwaway `POST /v1/messages` (1-token Haiku
call) and scraped rate-limit headers from the response. Upstream PR #29
(BrianPugh, 2026-05-24) proposed switching to the purpose-built
`GET /api/oauth/usage` endpoint, which returns structured JSON
(`five_hour`/`seven_day` with `utilization` and `resets_at`) at no token
cost. PR #29 was merged, then reverted in PR #37 (2026-05-24) after
rate-limiting problems were observed in practice.

This project's commit `9ac470b` makes the same switch with a specific
interval and backoff that upstream never landed.

## Decision / Solution

- Poll `GET https://api.anthropic.com/api/oauth/usage` at `POLL_INTERVAL=300`
  seconds (5 minutes) fixed — not adaptive.
- On a failed cycle (some provider present, none polled OK), schedule the next
  poll at `now + POLL_FAIL_BACKOFF` (retry in 60s) rather than `now +
  POLL_INTERVAL` (the full 300s) or spinning at TICK (5s) rate.
- The inner loop wakes every `TICK=5s` for responsive shutdown; this is
  separate from the poll cadence.

Reference: `daemon/claude_usage_daemon.py` — `POLL_INTERVAL`, `POLL_FAIL_BACKOFF`,
and the backoff reconciliation
`next_poll_at = time.time() + (POLL_INTERVAL if cycle_ok else POLL_FAIL_BACKOFF)`.

## Why

The endpoint is rate-limited by Anthropic, empirically at roughly 1 req/min.
Hitting it faster produces `429` (hard rate limit, with `retry-after`) or
`529` (transient overload). In testing:

| Cadence | Observed |
|---|---|
| ~1 req/min (7 calls rapid) | 200, 200, 529, 529, 429, 429, 429 |
| 60s spacing (original daemon interval) | 200, 200, 529, 529 |
| 300s spacing | two consecutive 200s; stable across 24h observation |

Without a fail-backoff, a single 429 would schedule the next poll at roughly
`now` (or worse), so the inner 5s tick loop retries on the very next tick —
the "5-second storm" that caused upstream to revert PR #29, where one failure
self-perpetuates. `POLL_FAIL_BACKOFF` is what spaces the retries out to 60s.

There is no device-initiated path that can bypass the poll timer. The device
pulls the daemon's cached snapshot via `GET /usage` at whatever rate it likes;
the daemon polls upstream strictly on its own `next_poll_at` schedule, so device
behavior (boot, reconnect, manual refresh) cannot compound the upstream rate.

## Alternatives considered

- **60s fixed (upstream PR #29 and original daemon):** Too fast; hit
  rate-limits within 2–4 consecutive polls in testing. Upstream PR #29 author
  (BrianPugh) initially reported no issues at 60s, then found them in logs.
  Reverted in PR #37.
- **Exponential backoff up to 5 minutes:** BrianPugh tried this after the
  revert. Per the PR thread, users still reported hitting limits even at the
  5-minute ceiling. Suggests the issue is cumulative session history, not
  just burst rate. Fixed 300s sidesteps the accumulation problem by staying
  well under the empirical limit from the first call.
- **`POST /v1/messages` header-scrape (old approach):** Generous rate limits
  (not the rate-limited oauth endpoint), but burns one inference token per
  poll, spoofs the `claude-code/2.1.5` User-Agent, and can't get ISO-8601
  reset times — only Unix timestamps from response headers.

## Prevention

- Do not lower `POLL_INTERVAL` below 300 without first verifying the rate
  limit has been relaxed. The 429 `retry-after` header value (observed:
  ~101s after a small burst) is a lower bound, not a safe cadence — it
  reflects the burst penalty, not the sustained limit.
- The fail-backoff is load-bearing: without it, any transient failure causes
  the 5s inner loop to storm the endpoint until the next success.

## Related

- Upstream PR #29 (switch): https://github.com/HermannBjorgvin/Clawdmeter/pull/29
- Upstream PR #37 (revert): https://github.com/HermannBjorgvin/Clawdmeter/pull/37
- `CLAUDE.md` "Daemon / host side" — runtime behavior docs
- `.claude/rules/daemon.md` — rule bullet
