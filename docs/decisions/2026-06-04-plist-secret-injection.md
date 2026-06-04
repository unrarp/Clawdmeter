---
date: 2026-06-04
module: daemon
tags: [installer, macos, launchd, plist, security, sed, secrets, xml]
---

# macOS plist secret injection must use Python, not sed

## Context

`daemon/install-mac.sh` builds `com.user.clawdmeter-broker.plist` from a template
containing the placeholder `__BROKER_KEY__`. The initial implementation used `sed`:

```bash
sed -e "s|__BROKER_KEY__|$BROKER_KEY|g" "$PLIST_SRC" > "$PLIST_DEST"
```

During an `/xverify-agent` + Codex review pass the `sed` approach was flagged as
corrupting secrets with certain characters. Verified empirically.

## Decision / Solution

Replace the `sed` substitution for the secret with a Python one-liner that:
1. reads the partially-expanded plist (after path placeholders are substituted by `sed`),
2. uses `xml.sax.saxutils.escape` to XML-encode the secret before string substitution,
3. writes the result back via `pathlib`.

```bash
PLIST_DEST="$PLIST_DEST" BROKER_KEY="$BROKER_KEY" python3 -c \
  "import os,pathlib,xml.sax.saxutils as x; \
   p=pathlib.Path(os.environ['PLIST_DEST']); \
   p.write_text(p.read_text().replace('__BROKER_KEY__', x.escape(os.environ['BROKER_KEY'])))"
chmod 600 "$PLIST_DEST"
```

Pass the secret **via environment variables**, not shell interpolation — this avoids
shell quoting hazards and keeps the key out of the `ps aux` process table argument list.

## Why

`sed`'s `s/find/replace/` syntax treats several characters in the *replacement* string
as special:

| Character in key | sed behaviour | Result |
|---|---|---|
| `&` | expands to the full matched text | `abc&def` → `abcabc__BROKER_KEY__defdef` |
| `|` (when used as delimiter) | closes the `s|||` expression | `abc|def` → plist truncated at `|` |
| `\` | escape prefix for next char | `abc\def` → `abcdef` (backslash silently dropped) |
| `<` or `>` | valid in shell but breaks XML | plist is invalid XML; launchd silently fails |

All of these produce a silently wrong plist. The broker then starts with the wrong
key and every device `/tokens` request gets `403 forbidden` — a silent security failure
that is hard to diagnose because `launchctl load` succeeds.

`xml.sax.saxutils.escape` converts `&`→`&amp;`, `<`→`&lt;`, `>`→`&gt;`, which launchd
correctly decodes back to the literal character when reading the plist. Verified via
`plistlib.loads()` round-trip for keys containing `& | < > " \`.

The Linux path (`EnvironmentFile=%h/.config/clawdmeter/broker.env`) avoids this entirely
— the key never touches sed substitution or XML. The macOS plist approach is necessary
because launchd's `EnvironmentVariables` dict must be embedded in the plist at load time.

## Alternatives considered

- **Heredoc `cat`** — the same shell-interpolation quoting hazards apply to `$BROKER_KEY`
  in any shell context; doesn't fix the XML-encoding problem.
- **`envsubst`** — handles shell-special chars better than sed, but still doesn't
  XML-encode; `&` in the key produces invalid XML.
- **Source `broker.env` in the plist** — launchd has no `EnvironmentFile` equivalent;
  the vars must be embedded in the plist.
- **Restrict allowed chars in the key** — adds fragile user-visible constraints with no
  benefit over the Python fix.

## Prevention

`daemon/install-mac.sh` now also `chmod 600 "$PLIST_DEST"` after writing, since the plist
contains the plaintext secret. The Linux `broker.env` file was already mode 600.
