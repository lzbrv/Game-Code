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

# *** RE-INSTALL GIT LFS'S HOOKS INTO THE NEW DIRECTORY, AND THIS IS NOT OPTIONAL. ***
#
# core.hooksPath makes git ignore .git/hooks ENTIRELY. Git LFS installs post-checkout,
# post-commit, post-merge and pre-push in there, and pre-push is what uploads LFS objects
# when you push. Set the path without doing this and LFS silently stops running on a repo
# that keeps 641 files in LFS - you would push commits whose binaries never arrive, which
# is a far worse failure than the config churn this hook directory exists to stop.
# `git lfs install --force` detects core.hooksPath and writes its hooks there instead.
if command -v git-lfs >/dev/null 2>&1; then
    git lfs install --force >/dev/null
    echo "hooks: git-lfs hooks re-installed into .githooks"
else
    echo "hooks: WARNING - git-lfs not found. Install it and re-run this script, or LFS" >&2
    echo "hooks:           will not run at all now that core.hooksPath is set." >&2
fi

chmod +x .githooks/* 2>/dev/null || true
echo "hooks: core.hooksPath = .githooks"
echo "installed:"
ls -1 .githooks | sed 's/^/  /'
