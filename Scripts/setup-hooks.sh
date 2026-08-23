#!/usr/bin/env bash
# ==============================================================================
# Trace - setup-hooks.sh
#
# Points git at the in-repo .githooks/ directory. Run once per clone:
#
#     ./Scripts/setup-hooks.sh
#
# Git does NOT clone hooks - .git/hooks is local to each machine - so there is no
# way to ship a hook that installs itself. core.hooksPath is the supported way to
# keep them in the repo where they can be reviewed and versioned like anything
# else. Windows: run this from Git Bash, or use Scripts\setup-hooks.bat.
# ==============================================================================
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"
git config core.hooksPath .githooks
chmod +x .githooks/* 2>/dev/null || true
echo "hooks: core.hooksPath = .githooks"
echo "installed:"
ls -1 .githooks | sed 's/^/  /'
