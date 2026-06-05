---
date: 2026-06-04
module: daemon
tags: [installer, macos, launchd, plist, security, sed, secrets, xml]
---

# macOS plist secret injection must use Python, not sed

## Context
`daemon/install-mac.sh` builds the launchd plist from a template with a
`__BROKER_KEY__` placeholder. The initial `sed -e "s|__BROKER_KEY__|$BROKER_KEY|g"`
was flagged during an xverify + Codex pass as corrupting secrets with certain
characters. Verified empirically.

## Decision
Substitute the secret with Python: read the (path-expanded) plist, XML-encode the key
via `xml.sax.saxutils.escape`, write back via `pathlib`, then `chmod 600`. Pass the
secret **via environment variables**, not shell interpolation — avoids quoting hazards
and keeps the key out of the `ps aux` argument list.

```bash
PLIST_DEST="$PLIST_DEST" BROKER_KEY="$BROKER_KEY" python3 -c \
  "import os,pathlib,xml.sax.saxutils as x; \
   p=pathlib.Path(os.environ['PLIST_DEST']); \
   p.write_text(p.read_text().replace('__BROKER_KEY__', x.escape(os.environ['BROKER_KEY'])))"
chmod 600 "$PLIST_DEST"
```

## Why
`sed`'s replacement string treats several characters as special, all producing a
silently wrong plist:

| Char in key | sed behaviour |
|---|---|
| `&` | expands to the full matched text |
| `|` (the delimiter) | closes the `s|||` expression → plist truncated |
| `\` | escape prefix → backslash silently dropped |
| `<` / `>` | valid in shell but breaks XML → launchd silently fails |

The broker then starts with the wrong key and every device `/tokens` gets `403` — a
silent security failure that's hard to diagnose because `launchctl load` succeeds.
`xml.sax.saxutils.escape` maps `& < >` to entities that launchd decodes back to the
literal char (verified via `plistlib.loads()` round-trip for `& | < > " \`). The Linux
path uses `EnvironmentFile` and never touches sed/XML; the plist approach is forced
because launchd embeds `EnvironmentVariables` in the plist at load time.

## Alternatives considered
- **Heredoc `cat` / `envsubst`** — still don't XML-encode; `&` produces invalid XML.
- **Source `broker.env` in the plist** — launchd has no `EnvironmentFile` equivalent.
- **Restrict allowed chars in the key** — fragile user-visible constraint, no benefit.

## Prevention
`install-mac.sh` `chmod 600`s the plist after writing (it holds the plaintext secret);
the Linux `broker.env` was already mode 600.
