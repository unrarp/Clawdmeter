#!/usr/bin/env bash
# Claude Code PostToolUse hook — auto-formats the just-edited file. Mirrors the
# pattern in the sibling workspace projects (deals, wa-relay). Non-blocking:
# every formatter is `|| true` so a format failure never fails the edit.
# Tools live in .venv-dev (see AGENTS.md "Lint / format").
set -uo pipefail
fp=$(jq -r '.tool_input.file_path // ""')
[[ -n "$fp" ]] || exit 0

cd "$CLAUDE_PROJECT_DIR" || exit 0

case "$fp" in
  *.py)
    .venv-dev/bin/ruff check --fix --extend-unfixable=F401 "$fp" || true
    .venv-dev/bin/ruff format "$fp" || true
    ;;
  *.c | *.cpp | *.h)
    # clang-format honors .clang-format-ignore, so generated font_*.c / icons.h /
    # logos.h / splash_animations.h are skipped automatically.
    .venv-dev/bin/clang-format -i "$fp" || true
    ;;
esac
