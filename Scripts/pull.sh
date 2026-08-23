#!/usr/bin/env bash
# ==============================================================================
# Trace - pull.sh
#
# `git pull`, but it survives the Unreal editor having rewritten Config/*.ini.
#
# THE FAILURE THIS REPLACES:
#
#     error: Your local changes to the following files would be overwritten by
#     merge:  Config/DefaultEngine.ini
#     Please commit your changes or stash them before you merge.
#
# You did not change that file. The editor did, when you opened the project: it
# tops the file up with platform sections and plugin defaults for whatever machine
# it is running on, and a Windows editor and a Mac editor want different ones. So
# the file is permanently dirty on somebody's machine and git refuses to pull.
#
# This asks Scripts/config-hygiene.py which kind of dirty it is, and ONLY throws
# away the kind that is noise. A real settings change is never discarded silently:
# the script stops, prints the lines, and leaves you to decide. That distinction is
# the whole point - "just discard Config before pulling" is the advice that
# eventually loses somebody's renderer settings.
#
#     ./Scripts/pull.sh            # classify, discard noise, then pull
#     ./Scripts/pull.sh --dry-run  # classify only, touch nothing
#
# THE PERMANENT FIX, once, on the machine that keeps dirtying the file:
#     python3 Scripts/config-hygiene.py --adopt
#     git add Config && git commit -m "Adopt editor-injected config sections"
# Once those sections are IN the tracked file, that editor stops adding them and
# nobody's pull is blocked by them again.
# ==============================================================================
set -uo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"

if command -v python3 >/dev/null 2>&1; then PY=python3
elif command -v python  >/dev/null 2>&1; then PY=python
else echo "pull.sh: no python found; falling back to a plain git pull." >&2; exec git pull "$@"; fi

DRY=0
for a in "$@"; do [ "$a" = "--dry-run" ] && DRY=1; done

echo "== Config/*.ini vs HEAD =="
"$PY" Scripts/config-hygiene.py --status
STATUS=$?

if [ "$DRY" -eq 1 ]; then
    echo "(dry run: nothing changed, nothing pulled)"
    exit $STATUS
fi

if [ "$STATUS" -eq 2 ]; then
    echo
    echo "STOPPING. At least one Config file has REAL changes in it, listed above."
    echo "They are yours, not the editor's, so this script will not throw them away."
    echo "Commit them, or stash them, then pull:"
    echo "    git add Config && git commit -m '...'   # or: git stash push Config"
    exit 2
fi

echo
echo "== discarding editor noise (if any) =="
"$PY" Scripts/config-hygiene.py --discard-benign

echo
echo "== git pull =="
# --no-rebase EXPLICITLY, so this works on a clone that has never set pull.rebase.
# Git refuses a divergent pull without a strategy ("Need to specify how to reconcile
# divergent branches") and that stops the script dead - which is exactly the kind of
# papercut it exists to remove. MERGE and not rebase, deliberately: this repo carries
# LFS binaries under `lockable` (see .gitattributes), and rebasing rewrites commits
# that carry those pointers. A merge commit is the shape this team's history already
# has. Pass --rebase yourself if you know you want it; your argument wins.
exec git pull --no-rebase "$@"
