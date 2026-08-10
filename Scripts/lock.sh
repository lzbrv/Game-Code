#!/usr/bin/env bash
# ==============================================================================
# Trace — lock.sh
#
# Takes a Git LFS file lock BEFORE you open an asset in the Unreal editor.
#
# The command it runs (printed before it runs):
#
#   git lfs lock Content/Maps/Arena_Baked.umap
#
# Why this wrapper exists at all: /Game/Maps/Arena_Baked uses One File Per Actor,
# so every actor in the arena is its own .uasset with an unreadable GUID name:
#
#   Content/__ExternalActors__/Maps/Arena_Baked/3/NV/E0T93XXVAQZHVSCVX0S44T.uasset
#
# Nobody can type that. What a person actually knows is the LABEL they see in the
# World Outliner — Cover_37, Wall_North_01, Goal_Ring_Rim_12 — so this script
# accepts a label, finds the one package that contains it, and locks that.
#
#   Scripts/lock.sh Cover_37                     # by actor label
#   Scripts/lock.sh Content/Maps/Arena_Baked.umap  # by path
#   Scripts/lock.sh --list                       # who holds what
#
# It refuses to guess. If a label matches more than one package it prints them
# all and stops, because locking the wrong actor is worse than locking nothing.
#
# See docs/GITHUB.md §4 for the workflow this belongs to.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

# Named after this file, per the project's no-collisions rule.
TRACE_LOCK_SH_EXTERNAL_ACTORS="Content/__ExternalActors__"

TARGETS=()
DO_LIST=0

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} — take a Git LFS lock before editing an asset

USAGE
  Scripts/lock.sh <path-or-actor-label> [more...]
  Scripts/lock.sh --list

ARGUMENTS
  <path>                A file in the repo, e.g. Content/Maps/Arena_Baked.umap
  <actor-label>         A World Outliner label of an actor in /Game/Maps/Arena_Baked,
                        e.g. Cover_37, Wall_North_01, Goal_Ring_Rim_12.
                        Resolved to the One File Per Actor package holding it.

OPTIONS
  -l, --list            List all locks held across the team, then exit
  -n, --dry-run         Print the git commands; run nothing
  -h, --help            This text

WHY YOU LOCK
  A .uasset is an opaque binary. Git cannot merge two edits to one — the loser's
  work is deleted, not merged. So the team locks BEFORE editing rather than
  resolving after. Files marked 'lockable' in .gitattributes are checked out
  read-only (-r--r--r--); taking the lock is what makes yours writable.

THE FULL LOOP
  git pull                       # always first — lock the newest version
  Scripts/lock.sh Cover_37       # BEFORE you open it in the editor
  ... edit in Unreal, save ...
  git add -A && git commit -m "Arena: nudge Cover_37 out of the sightline"
  git push                       # push BEFORE you unlock
  Scripts/unlock.sh Cover_37

NOTES
  * Write access to the GitHub repo is required to take a lock. Read-only
    collaborators cannot lock, which defeats the whole workflow.
  * Unlock as soon as you have pushed. A forgotten lock blocks a teammate
    silently — they just see a read-only file and no explanation.
  * Windows: there is no lock.bat. Use 'git lfs lock <path>' directly, or run
    this from Git Bash.

EXAMPLES
  Scripts/lock.sh Cover_37
  Scripts/lock.sh Content/Trace/Materials/Parents/M_TraceNeon.uasset
  Scripts/lock.sh Wall_North_01 Goal_Ring_Rim_12
  Scripts/lock.sh --list
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -l|--list)    DO_LIST=1; shift ;;
        -n|--dry-run) TRACE_DRY_RUN=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        -*)           trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)            TARGETS+=("$1"); shift ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

command -v git >/dev/null 2>&1 || trace_die "git is not installed."
git lfs version >/dev/null 2>&1 || trace_die "git-lfs is not installed. See docs/SETUP.md §6."

cd "$TRACE_PROJECT_ROOT"

if [ "$DO_LIST" = "1" ]; then
    trace_msg "Locks currently held across the team"
    trace_run git lfs locks
    exit 0
fi

if [ "${#TARGETS[@]}" -eq 0 ]; then
    trace_err "Nothing to lock."
    echo
    usage
    exit 2
fi

# Resolve one argument to a repo-relative path. Echoes the path on success.
trace_lock_sh_resolve() {
    local arg="$1"

    # A real path wins over a label, always.
    if [ -e "$arg" ]; then
        printf '%s\n' "${arg#./}"
        return 0
    fi

    # Otherwise treat it as an actor label and search the OFPA packages. The
    # trailing [^A-Za-z0-9_] is what stops "Cover_1" matching "Cover_110" — the
    # label is stored NUL-delimited inside the package, so the byte after it is
    # never a word character. -a forces binary files to be searched as text.
    if [ ! -d "$TRACE_LOCK_SH_EXTERNAL_ACTORS" ]; then
        trace_err "'${arg}' is not a file, and ${TRACE_LOCK_SH_EXTERNAL_ACTORS}/ does not exist,"
        trace_err "so it cannot be an actor label either. Bake the arena first: Scripts/bake-arena.sh"
        return 1
    fi

    local matches
    matches="$(grep -rlaE "${arg}[^A-Za-z0-9_]" "$TRACE_LOCK_SH_EXTERNAL_ACTORS" 2>/dev/null || true)"

    local count
    count="$(printf '%s' "$matches" | grep -c . || true)"

    if [ "$count" -eq 0 ]; then
        trace_err "No file and no actor labelled '${arg}'."
        trace_err "Check the spelling in the World Outliner. Note the numbering is"
        trace_err "zero-padded to two digits: Cover_37, not Cover_037."
        return 1
    fi

    if [ "$count" -gt 1 ]; then
        trace_err "'${arg}' matches ${count} packages — refusing to guess:"
        printf '%s\n' "$matches" | sed 's/^/    /' >&2
        trace_err "Give the full label, or the path of the one you want."
        return 1
    fi

    printf '%s\n' "$matches"
}

trace_msg "Reminder: 'git pull' first, so you lock the newest version."

STATUS=0
for arg in "${TARGETS[@]}"; do
    if ! path="$(trace_lock_sh_resolve "$arg")"; then
        STATUS=1
        continue
    fi

    if [ "$path" != "$arg" ]; then
        trace_msg "Actor ${TRACE_C_BOLD}${arg}${TRACE_C_OFF} lives in ${path}"
    fi

    # Locking a file that is not 'lockable' technically works but buys nothing:
    # it stays writable for everyone else, so nobody is stopped. Say so.
    if ! git check-attr lockable -- "$path" | grep -q 'lockable: set'; then
        trace_warn "${path} is not marked 'lockable' in .gitattributes."
        trace_warn "The lock will be recorded but will NOT make the file read-only for"
        trace_warn "anyone else, so it protects nothing. Check .gitattributes."
    fi

    trace_run git lfs lock "$path" || STATUS=1
done

if [ "$STATUS" = "0" ] && [ "$TRACE_DRY_RUN" != "1" ]; then
    trace_msg "Locked. The file is now writable for you and read-only for everyone else."
    trace_msg "Push before you unlock, and unlock as soon as you have pushed."
fi

exit "$STATUS"
