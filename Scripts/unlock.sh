#!/usr/bin/env bash
# ==============================================================================
# Trace — unlock.sh
#
# Releases a Git LFS file lock AFTER you have pushed.
#
# The command it runs (printed before it runs):
#
#   git lfs unlock Content/Maps/Arena_Baked.umap
#
# Takes the same arguments as Scripts/lock.sh: a path, or the World Outliner
# LABEL of an actor in /Game/Maps/Arena_Baked (Cover_37, Wall_North_01), which
# it resolves to the One File Per Actor package holding that actor.
#
#   Scripts/unlock.sh Cover_37
#   Scripts/unlock.sh --list
#   Scripts/unlock.sh --force Cover_37     # someone else's lock. Read §4 first.
#
# ORDER MATTERS: push, THEN unlock. Unlocking first lets someone take the lock,
# edit the version you have already superseded, and produce two divergent
# binaries that cannot be merged.
#
# See docs/GITHUB.md §4 for the workflow this belongs to, including who is
# allowed to force.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

# Named after this file, per the project's no-collisions rule.
TRACE_UNLOCK_SH_EXTERNAL_ACTORS="Content/__ExternalActors__"

TARGETS=()
DO_LIST=0
DO_FORCE=0
ASSUME_YES=0

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} — release a Git LFS lock after pushing

USAGE
  Scripts/unlock.sh <path-or-actor-label> [more...]
  Scripts/unlock.sh --list
  Scripts/unlock.sh --force <path-or-actor-label>

ARGUMENTS
  <path>                A file in the repo, e.g. Content/Maps/Arena_Baked.umap
  <actor-label>         A World Outliner label of an actor in /Game/Maps/Arena_Baked,
                        e.g. Cover_37. Resolved to its One File Per Actor package.

OPTIONS
  -l, --list            List all locks held across the team, then exit
  -f, --force           Break a lock held by SOMEONE ELSE. See below.
  -y, --yes             Skip the confirmation prompt for --force
  -n, --dry-run         Print the git commands; run nothing
  -h, --help            This text

BEFORE YOU UNLOCK
  Push first. Always. The lock is what guarantees the copy on the server is the
  only copy anyone will build on; releasing it before your work is pushed is how
  two divergent .uassets get created.

--force, AND WHO IS ALLOWED TO USE IT
  Force breaks a lock somebody else is holding. It does not merge anything and it
  does not warn them — it just makes the file writable for everyone again, so two
  people can now edit it and one of them will lose the work.

  The rule for this team: post in chat first and wait. If they are on holiday or
  otherwise unreachable, whoever is unblocking the team may force it — and must
  say in chat which file they forced, so the person who comes back knows to pull
  before they open anything.

  If they are merely asleep, work on something else. That is the correct answer
  and it is why locks are meant to be short.

EXAMPLES
  Scripts/unlock.sh Cover_37
  Scripts/unlock.sh Content/Maps/Arena_Baked.umap
  Scripts/unlock.sh --list
  Scripts/unlock.sh --force Content/Maps/Arena_Baked.umap
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -l|--list)    DO_LIST=1; shift ;;
        -f|--force)   DO_FORCE=1; shift ;;
        -y|--yes)     ASSUME_YES=1; shift ;;
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
    trace_err "Nothing to unlock."
    echo
    usage
    exit 2
fi

# Same resolution rule as lock.sh: a real path wins, otherwise treat the argument
# as an actor label and find the single OFPA package containing it.
trace_unlock_sh_resolve() {
    local arg="$1"

    if [ -e "$arg" ]; then
        printf '%s\n' "${arg#./}"
        return 0
    fi

    if [ ! -d "$TRACE_UNLOCK_SH_EXTERNAL_ACTORS" ]; then
        trace_err "'${arg}' is not a file, and ${TRACE_UNLOCK_SH_EXTERNAL_ACTORS}/ does not exist."
        return 1
    fi

    local matches
    matches="$(grep -rlaE "${arg}[^A-Za-z0-9_]" "$TRACE_UNLOCK_SH_EXTERNAL_ACTORS" 2>/dev/null || true)"

    local count
    count="$(printf '%s' "$matches" | grep -c . || true)"

    if [ "$count" -eq 0 ]; then
        trace_err "No file and no actor labelled '${arg}'."
        trace_err "Numbering is zero-padded to two digits: Cover_37, not Cover_037."
        return 1
    fi

    if [ "$count" -gt 1 ]; then
        trace_err "'${arg}' matches ${count} packages — refusing to guess:"
        printf '%s\n' "$matches" | sed 's/^/    /' >&2
        return 1
    fi

    printf '%s\n' "$matches"
}

if [ "$DO_FORCE" = "1" ] && [ "$ASSUME_YES" != "1" ] && [ "$TRACE_DRY_RUN" != "1" ]; then
    if [ -t 0 ]; then
        trace_warn "--force breaks a lock held by someone else."
        trace_warn "They will not be told. If they are mid-edit, their work is about to be lost."
        trace_warn "Have you posted in chat and waited?"
        printf '%s' "Type 'force' to continue: "
        read -r TRACE_UNLOCK_SH_REPLY || TRACE_UNLOCK_SH_REPLY=""
        [ "$TRACE_UNLOCK_SH_REPLY" = "force" ] || trace_die "Aborted. Good."
    else
        trace_die "--force needs a terminal to confirm on, or --yes if you are certain."
    fi
fi

STATUS=0
for arg in "${TARGETS[@]}"; do
    if ! path="$(trace_unlock_sh_resolve "$arg")"; then
        STATUS=1
        continue
    fi

    if [ "$path" != "$arg" ]; then
        trace_msg "Actor ${TRACE_C_BOLD}${arg}${TRACE_C_OFF} lives in ${path}"
    fi

    if [ "$DO_FORCE" = "1" ]; then
        trace_run git lfs unlock --force "$path" || STATUS=1
    else
        trace_run git lfs unlock "$path" || STATUS=1
    fi
done

if [ "$STATUS" = "0" ] && [ "$TRACE_DRY_RUN" != "1" ]; then
    trace_msg "Unlocked. The file is read-only again for everyone, including you."
fi

exit "$STATUS"
