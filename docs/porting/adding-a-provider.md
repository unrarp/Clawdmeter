# Adding a usage provider

The firmware is **provider-table driven**. Claude and Codex are two rows
in a set of parallel tables indexed by a `PROV_*` enum; the screen cycle,
widgets, logos, WiFi-page rows, health verdicts, fetch scheduling, and
animations all loop over `PROVIDER_COUNT`. Adding a third provider means
adding **one enum entry and one row to each table** plus the one piece
that is genuinely unique per provider: a `fetch_*()` that maps that
provider's API into the shared `ProviderUsage` struct.

You should not need to touch the screen-cycling logic, the WiFi-page
render loop, `main.cpp`'s loop, or `ui_update()` — those are all generic.
If you find yourself special-casing a provider by index outside the
tables, that's a smell; prefer a new table field.

See also: the terse checklist in `firmware/src/data.h` (the enum comment),
and the on-device API contracts in
[`.claude/rules/networking.md`](../../.claude/rules/networking.md)
("Device-side provider usage mapping").

## What you need

- **An API the device can reach over TLS** that exposes the provider's
  rate-limit / usage state, reachable with a bearer token. Two shapes are
  already handled as references:
  - **Response-header scrape** (Claude): the numbers come back as HTTP
    response headers on an otherwise-throwaway request.
  - **JSON body** (Codex): a GET returns a JSON object you parse with
    ArduinoJson.
- **A way for the host token broker to supply the credential.** The
  device never ships provider tokens; it fetches them from
  `daemon/token_broker.py` on first boot / after a `401`. The broker must
  return your provider's token under a JSON key (see step 5). Token
  sourcing + the `200`/`409` contract live in
  [`.claude/rules/daemon.md`](../../.claude/rules/daemon.md).
- **An 80×80 logo.** Same pixel dimensions as the existing marks (the UI
  centers the top row against `LOGO_CLAUDE_HEIGHT`); a different size will
  misalign. RGB565A8, white-tinted unless it's a colored mark — generate
  with `tools/png_to_lvgl.js` (see `tools/README.md`).

## Step-by-step

Worked references throughout: Claude is `PROV_CLAUDE`, Codex is
`PROV_CODEX`. Grep either name to see every site you're about to mirror.

1. **Add the enum entry** in `firmware/src/data.h`, before
   `PROVIDER_COUNT`:

   ```c
   enum { PROV_CLAUDE = 0, PROV_CODEX, PROV_FOO, PROVIDER_COUNT };
   ```

   `PROVIDER_COUNT` grows automatically; everything indexed by it
   resizes. `ProviderUsage` itself doesn't change — it's already generic
   (session %, session reset mins, weekly %, weekly reset mins, status,
   ok, present).

2. **Write the fetch function** in `firmware/src/net.cpp`. Signature is
   `static void fetch_foo(int prov)`. Its job: do one blocking request and
   map the result into `s_usage[prov]`. Contract (copy the closest
   reference — `fetch_claude` for header-scrape, `fetch_codex` for JSON):

   - Read the credential from `s_cred[prov].token` (and
     `s_cred[prov].account` if your API needs an account id — guard for
     `nullptr` if it might not).
   - For HTTPS, `configure_tls(client)` before `http.begin()` (uses the
     embedded CA bundle). Set **both** `setConnectTimeout` and
     `setTimeout` (see the networking rule on why both are needed).
   - On HTTP `401`/`403`, call `on_auth_reject(prov, code)` and set
     `s_usage[prov].ok = false` — that flags the token for a broker
     refetch. Do **not** map any numbers.
   - On success, write `session_pct` / `weekly_pct` as **0–100 floats**,
     the two `*_reset_mins` as whole minutes from now (use
     `epoch_to_mins()` if your API gives absolute epochs; divide by 60 if
     it gives relative seconds), `status` ("allowed"/"limited"), set
     `ok = true`, and **`s_last_update_ms[prov] = millis();`** (this drives
     the WiFi-page freshness verdict).
   - On any other failure (non-200, missing/garbled body), set
     `ok = false` and **return without overwriting** the last-good numbers
     — a failing provider keeps showing its last values dimmed, not zeros.
   - Leave `present = true` (set at init). A provider needing re-auth is
     surfaced on the WiFi page via the health verdict, not by blanking its
     panel — see [`.claude/rules/ui.md`](../../.claude/rules/ui.md).

3. **Add the net + credential rows** in `firmware/src/net.cpp`:

   - A backing token buffer sized for your token, plus an account buffer
     if needed, and a `s_cred[]` row pointing at them
     (`{ buf, sizeof(buf), nullptr, 0 }` if there's no account id):

     ```c
     static char s_foo_token[1024];
     // ... in s_cred[] ...
     { s_foo_token, sizeof(s_foo_token), nullptr, 0 },
     ```

   - A `PROVIDERS[]` row — the broker JSON key its token arrives under,
     and the fetch fn (forward-declare `fetch_foo` near the others):

     ```c
     { "foo", fetch_foo },
     ```

   NVS persistence is already index-generic (`token_store` keys by
   `prov`), so cached-token load/save needs no change.

4. **Add the UI row** in `firmware/src/ui.cpp`:

   - A `UI_PROVIDERS[]` row: display name (used for the screen title
     **and** the WiFi-page row label), logo bitmap + dimensions, accent
     color (add one to `theme.h`), "no account" caption, and the
     spinner-message catalog + its count:

     ```c
     { "Foo", logo_foo_data, LOGO_FOO_WIDTH, LOGO_FOO_HEIGHT,
       COL_ACCENT_FOO, "No Foo account", foo_messages, FOO_MSG_COUNT },
     ```

   - A **WiFi-page row Y in *both* board branches** of
     `compute_layout()` — `wifi_prov_y[2] = ...;`. This is the one spot
     that isn't auto-looped (the layout values are per-board hand-tuned),
     and a `static_assert(PROVIDER_COUNT == 2, ...)` will fail the build
     to remind you. Bump that assert to `== 3` once you've added the Ys.
     Mind the narrowest board: the AMOLED-1.8 WiFi panel holds ~6 rows
     (status, SSID, IP, the provider rows, "Updated") — see
     [`.claude/rules/ui.md`](../../.claude/rules/ui.md) on fixed-layout
     overflow before adding a row.

   - Extend the logo `static_assert` at the top of `ui.cpp` to include
     your mark's dimensions (all provider logos must share a size).

5. **Teach the broker about it** in `daemon/token_broker.py`: source the
   credential from the host and return it under the same JSON key you put
   in the `PROVIDERS[]` row (`"foo"`), following the `200`/`409` contract
   in [`.claude/rules/daemon.md`](../../.claude/rules/daemon.md). Restart
   the unit (`systemctl --user restart clawdmeter-broker`) — the running
   loop won't pick up edits otherwise.

6. **Build both envs.** `pio run -d firmware -e waveshare_amoled_216 -e
   waveshare_amoled_18`. The link step + the `static_assert`s are the real
   gate — a missing table row, an un-bumped assert, or a wrong-sized logo
   shows up here.

7. **Flash + visual QA.** `./screenshot.sh out.png` over USB serial. Step
   through the cycle (Splash → providers → WiFi) and confirm: the new
   provider's panel renders with its accent + logo + spinner catalog; the
   bar colors track `pct_color` thresholds; and the WiFi page shows a
   `Foo: live` row once a fetch lands. The boot screen sits on the splash
   until a button press — to capture a specific page without a button, see
   the QA note in [`AGENTS.md`](../../AGENTS.md) on temporarily changing
   the default boot screen.

## Common pitfalls

- **Build fails at `static_assert(PROVIDER_COUNT == 2, ...)`.** Working as
  intended — you added a provider but didn't add its `wifi_prov_y[]` in
  both `compute_layout()` branches. Add them, then bump the assert.
- **New panel shows "Connecting…" forever.** The fetch never set
  `s_last_update_ms[prov]` / `ok = true`, or the broker isn't returning a
  token under your `PROVIDERS[]` key (check `journalctl --user -u
  clawdmeter-broker` and the WiFi page — a `Foo: getting tokens…` /
  `needs login` row tells you which). Remember to restart the broker after
  editing it.
- **Panel renders but numbers are wrong / 0%.** Map percentages as 0–100
  (Claude's headers are a 0–1 fraction → ×100; Codex's `used_percent` is
  already 0–100). For resets: `epoch_to_mins()` for absolute epochs,
  `/ 60` for relative seconds. A 200 with a missing/renamed field must be
  treated as a failure (keep last-good, `ok = false`), not scraped to 0.
- **Logo is invisible or misaligned.** Source PNGs are black-on-
  transparent — the converter must tint to white (colored marks use
  `--no-tint`). A logo of the wrong dimensions misaligns the top row;
  keep it 80×80 like the others and extend the `ui.cpp` logo
  `static_assert`.
- **WiFi row clips on the 1.8.** The 20px row font is narrow; keep the
  provider name short (it prefixes every WiFi row as `Name: state`). See
  the fixed-layout-overflow rule in `.claude/rules/ui.md`.
