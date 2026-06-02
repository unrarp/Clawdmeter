---
paths:
  - "daemon/**"
---

# Daemon rules

- **`/api/oauth/usage` requires `POLL_INTERVAL ≥ 300s` and a fail-backoff.** The endpoint returns 429/529 above ~1 req/min. Use a fixed 300s interval plus a `POLL_FAIL_BACKOFF` (60s) so a failed poll retries after 60s rather than after the full 300s *or* at every 5s tick. Do NOT lower the interval — upstream tried 60s (PR #29), saw rate-limit storms, and reverted (PR #37). Even 5-min exponential backoff reportedly still hit limits for some users; 300s fixed has been stable in practice. → see `docs/decisions/2026-06-02-api-oauth-usage-rate-limit.md`

- **Codex usage = `GET https://chatgpt.com/backend-api/wham/usage`** (undocumented; verified 200). Auth mirrors the Claude path but with a *different* creds file and header: `Authorization: Bearer <tokens.access_token>` + `ChatGPT-Account-Id: <tokens.account_id>`, both read from `~/.codex/auth.json` (NOT `~/.claude/.credentials.json`). Response `rate_limit.primary_window` (18000s) → 5h/session, `secondary_window` (604800s) → 7d/weekly; `used_percent` is already 0–100; `reset_after_seconds` is minutes-from-now directly (÷60, no ISO math). Rate-limit tolerance is unverified — start at the same 300s and only relax after measuring. Full mapping/plan in `docs/plans/2026-06-02-codex-usage.md` §A.

## Related decisions

- `2026-06-02-api-oauth-usage-rate-limit` — why `/api/oauth/usage` requires 300s polling and a fail-backoff; upstream revert history and empirical rate-limit findings.
