---
paths:
  - "daemon/**"
---

# Daemon rules

- **`/api/oauth/usage` requires `POLL_INTERVAL ≥ 300s` and a fail-backoff.** The endpoint returns 429/529 above ~1 req/min. Use a fixed 300s interval plus a `POLL_FAIL_BACKOFF` (60s) so a failed poll retries after 60s rather than after the full 300s *or* at every 5s tick. Do NOT lower the interval — upstream tried 60s (PR #29), saw rate-limit storms, and reverted (PR #37). Even 5-min exponential backoff reportedly still hit limits for some users; 300s fixed has been stable in practice. → see `docs/decisions/2026-06-02-api-oauth-usage-rate-limit.md`

- **Codex usage = `GET https://chatgpt.com/backend-api/wham/usage`** (undocumented; verified 200). Auth mirrors the Claude path but with a *different* creds file and header: `Authorization: Bearer <tokens.access_token>` + `ChatGPT-Account-Id: <tokens.account_id>`, both read from `~/.codex/auth.json` (NOT `~/.claude/.credentials.json`). Response `rate_limit.primary_window` (18000s) → 5h/session, `secondary_window` (604800s) → 7d/weekly; `used_percent` is already 0–100; `reset_after_seconds` is minutes-from-now directly (÷60, no ISO math). Rate-limit tolerance is unverified — start at the same 300s and only relax after measuring. Full mapping/plan in `docs/plans/2026-06-02-codex-usage.md` §A.

- **Claude field mapping:** `/api/oauth/usage` returns `{five_hour,seven_day,...}` with `utilization` already a **0–100 percentage** (no ×100) and `resets_at` as ISO-8601. `five_hour`→session (`s`/`sr`), `seven_day`→weekly (`w`/`wr`); `st` is derived (`limited` if session util ≥ 100). Bearer = subscription OAuth token, `anthropic-beta: oauth-2025-04-20`, needs `user:profile` scope. (Pre-Codex versions scraped rate-limit *headers* off a throwaway 1-token `/v1/messages` POST — generous limits but burned a token + spoofed the client; don't reintroduce that.)

- **Wire = a full 14-key snapshot every cycle, never partial.** Keys: `s/sr/w/wr/st/ok` (Claude) + `sp/cp` (presence: creds file exists, re-checked every cycle) + `cs/csr/cw/cwr/cst/cok` (Codex). Poll every present provider, hold a per-provider last-good cache, and always write all 14 keys so one provider's failure never blanks the other's panel: a present-but-failing provider keeps its last-good numbers with `ok:false` (device dims them); a never-succeeded provider sends `-1` sentinels (device shows "Connecting…"). The cycle counts as success (advances the poll timer) if *any* present provider polled OK, else it takes `POLL_FAIL_BACKOFF`.

- **The daemon reads the OAuth token, never refreshes it — and the poll must stay host-side.** `read_token()` greps `accessToken` out of `~/.claude/.credentials.json` (Keychain on macOS) every cycle; that access token is short-lived (~1h) and rotates, so the daemon piggybacks on Claude Code keeping the creds fresh. Don't try to drop the daemon by moving the Anthropic poll onto the device: a platform `sk-ant-…` key is a *different account* and can't see subscription usage, and a device refreshing from the *same* refresh-token lineage fights the laptop's Claude Code (rotating refresh tokens → one side gets logged out). Real device self-sufficiency needs a *device-dedicated* OAuth login + on-device TLS. → see `docs/plans/2026-06-02-wifi-transport.md` "Rejected: on-device OAuth".

- **`POLL_INTERVAL=300`, `TICK=5`, `POLL_FAIL_BACKOFF=60`.** The inner loop wakes every 5s to detect BLE disconnects fast; it polls upstream only when 300s have elapsed OR the ESP fires a refresh request. A failed/throttled poll backs off 60s — not the full interval, not every tick.

- **Bash inline-python env-var assignments must prefix the command, not follow it.** In `claude-usage-daemon.sh` the merged payload is built with `SP="$sp" CP="$cp" … python3 -c '…'`. If those assignments appear *after* the closing `'` (i.e. `python3 -c '…' SP="$sp"`) they become positional args, not environment — `os.environ.get("SP")` returns `None` and every provider defaults to absent. This is silent: `bash -n` passes, the script runs, the payload looks correct to the caller. Always write `KEY=val command`, never `command KEY=val`.

- **Per-provider last-good cache must be cleared when a provider goes absent.** Both daemons hold `_claude_last`/`_codex_last` (python) and `CLAUDE_CACHE`/`CODEX_CACHE` tmp files (bash). If a provider's creds file disappears and then reappears — e.g. the user removes and re-adds a Codex account — the cache from the previous account is served as "stale" data (`ok:false`, last-good numbers) rather than the `-1`/Connecting sentinel the plan requires. Fix: `[ "$cp" = "true" ] || rm -f "$CODEX_CACHE"` (bash) and `if not cp: _codex_last = None` (python) at the top of each poll cycle, before any HTTP calls.

- **macOS `claude_present()` must use a sticky latch, not a bare Keychain read.** On macOS the Claude token lives in the Keychain, not a file, so `claude_present()` calls `read_token()` which calls `security find-generic-password`. A transient Keychain denial or timeout returns `None`, which propagates as `sp:false` — the device shows "No Claude account" even though the account is still configured. Fix: set a module-level `_claude_seen = True` the first time the token is successfully read, and return `_claude_seen` as a fallback when the Keychain call fails. A real account removal requires a daemon restart; transient denials should be absorbed.

## Related decisions

- `2026-06-02-api-oauth-usage-rate-limit` — why `/api/oauth/usage` requires 300s polling and a fail-backoff; upstream revert history and empirical rate-limit findings.
