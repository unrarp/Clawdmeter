# Dev tooling targets. `make setup` is the one-time install; `make lint` is the
# pre-commit gate (hooks/pre-commit); `make format` applies fixes in bulk.
# (This is dev tooling only — the broker host install is daemon/install*.sh.)
VENV         := .venv-dev
RUFF         := $(VENV)/bin/ruff
CLANG_FORMAT := $(VENV)/bin/clang-format

# Tracked C++ sources only (git ls-files) — so gitignored local files like
# net_config.h (secrets) and splash_animations.h (build artifact) are never
# linted/formatted. Generated-but-tracked data (font_*.c / icons.h / logos.h) is
# still skipped by .clang-format-ignore, which clang-format honors automatically.
CXX_FILES = $(shell git ls-files firmware/src | grep -E '\.(cpp|h|c)$$')

.PHONY: setup lint format
# One-time dev setup: pinned lint/format tools in a local venv + the git hook.
# Idempotent — safe to re-run (e.g. after requirements-dev.txt changes).
setup:
	@test -d $(VENV) || python3 -m venv $(VENV)
	$(VENV)/bin/pip install --quiet --upgrade pip
	$(VENV)/bin/pip install --quiet -r requirements-dev.txt
	git config core.hooksPath hooks
	@echo "✓ dev tooling installed; pre-commit gate active (core.hooksPath=hooks)"
# Check-only gate — verifies, never modifies (formatting is applied by the
# Claude Code edit hook / `make format`).
lint:
	$(RUFF) check .
	$(RUFF) format --check .
	$(CLANG_FORMAT) --dry-run -Werror $(CXX_FILES)

# Apply all fixes + formatting in bulk.
format:
	$(RUFF) check --fix .
	$(RUFF) format .
	$(CLANG_FORMAT) -i $(CXX_FILES)
