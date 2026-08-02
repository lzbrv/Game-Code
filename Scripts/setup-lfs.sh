#!/usr/bin/env bash
# ==============================================================================
# Trace — setup-lfs.sh
#
# One-time (per clone, per machine) Git LFS bootstrap. Run it right after you
# clone, before you commit anything binary.
#
# What it actually does, in raw commands:
#
#   git lfs version                 # is git-lfs installed at all?
#   git lfs install                 # install the clean/smudge filters + hooks
#   git lfs track                   # list the patterns from .gitattributes
#   git lfs pull                    # fetch the real bytes for this checkout
#
# It is deliberately NON-DESTRUCTIVE: it never rewrites history. If binaries were
# already committed outside LFS it prints the `git lfs migrate` command for you
# to run deliberately, after the team has agreed to a force-push.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

DO_PULL=1
VERIFY_ONLY=0

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} Git LFS setup

USAGE
  Scripts/setup-lfs.sh [options]

OPTIONS
      --no-pull       Do not run 'git lfs pull' at the end
      --verify        Report status only; change nothing
  -n, --dry-run       Print the commands; run nothing
  -h, --help          This text

WHY
  Unreal projects accumulate .uasset / .umap / .fbx / textures / audio. Git stores
  a full copy of every version of every one of them forever; a year of that turns
  a clone into tens of gigabytes. LFS keeps pointers in history and the bytes in a
  side store, so history stays small and clones stay fast.

FILE LOCKING
  .uasset and .umap CANNOT be merged. Two people editing one map is guaranteed
  data loss for one of them, and Git will not warn you. Use locks:

      git lfs lock   Content/Maps/Arena.umap
      # ... edit, commit, push ...
      git lfs unlock Content/Maps/Arena.umap
      git lfs locks                       # who holds what

  .gitattributes marks those types 'lockable', so everyone else's copy is
  read-only until you release the lock. That is the point — the filesystem stops
  them before Git has to.

INSTALLING git-lfs
  macOS:   brew install git-lfs
  Debian:  sudo apt-get install git-lfs
  Manual:  https://git-lfs.com
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-pull)    DO_PULL=0; shift ;;
        --verify)     VERIFY_ONLY=1; shift ;;
        -n|--dry-run) TRACE_DRY_RUN=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

# ------------------------------------------------------------------------------
# 1. git
# ------------------------------------------------------------------------------
command -v git >/dev/null 2>&1 || trace_die "git is not installed or not on PATH."

cd "$TRACE_PROJECT_ROOT"

if ! git rev-parse --git-dir >/dev/null 2>&1; then
    trace_err "${TRACE_PROJECT_ROOT} is not a Git repository yet."
    trace_err ""
    trace_err "Start one, then re-run this script:"
    trace_err "  cd \"${TRACE_PROJECT_ROOT}\""
    trace_err "  git init -b main"
    trace_err "  Scripts/setup-lfs.sh"
    trace_err "  git add . && git commit -m \"Initial commit\""
    exit 1
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
if [ "$REPO_ROOT" != "$TRACE_PROJECT_ROOT" ]; then
    trace_warn "Git root (${REPO_ROOT}) is not the project root (${TRACE_PROJECT_ROOT})."
    trace_warn "LFS patterns in ${TRACE_PROJECT_ROOT}/.gitattributes still apply, but paths in"
    trace_warn "the messages below are relative to the Git root."
fi

# ------------------------------------------------------------------------------
# 2. git-lfs
# ------------------------------------------------------------------------------
if ! git lfs version >/dev/null 2>&1; then
    trace_err "git-lfs is not installed."
    trace_err ""
    trace_err "Install it, then re-run this script:"
    trace_err "  macOS:   brew install git-lfs"
    trace_err "  Debian:  sudo apt-get install git-lfs"
    trace_err "  Manual:  https://git-lfs.com"
    exit 1
fi
trace_msg "Found $(git lfs version | head -n 1)"

# ------------------------------------------------------------------------------
# 3. .gitattributes
# ------------------------------------------------------------------------------
if [ ! -f "${TRACE_PROJECT_ROOT}/.gitattributes" ]; then
    trace_die ".gitattributes is missing from ${TRACE_PROJECT_ROOT}. It is committed to this repo — restore it before continuing (git checkout -- .gitattributes)."
fi
if ! grep -q 'filter=lfs' "${TRACE_PROJECT_ROOT}/.gitattributes"; then
    trace_die ".gitattributes exists but declares no 'filter=lfs' patterns. Something has overwritten it."
fi
trace_msg ".gitattributes declares $(grep -c 'filter=lfs' "${TRACE_PROJECT_ROOT}/.gitattributes") LFS patterns"

if [ "$VERIFY_ONLY" = "1" ]; then
    trace_msg "Verify-only: skipping 'git lfs install' and 'git lfs pull'."
else
    # ------------------------------------------------------------------------------
    # 4. Install the filters. Idempotent — safe to run on every clone.
    # ------------------------------------------------------------------------------
    trace_run git lfs install
fi

# ------------------------------------------------------------------------------
# 5. Report what is tracked and what is already stored in LFS.
# ------------------------------------------------------------------------------
trace_msg "Patterns Git LFS will intercept:"
git lfs track || true

if git rev-parse --verify HEAD >/dev/null 2>&1; then
    # `|| true` inside the braces: with `set -o pipefail` a failing first stage
    # would otherwise abort the whole script.
    LFS_COUNT="$( { git lfs ls-files 2>/dev/null || true; } | wc -l | tr -d ' ')"
    trace_msg "Files already stored in LFS at HEAD: ${LFS_COUNT}"

    # Detect binaries that were committed BEFORE LFS was configured. .gitattributes
    # only affects files as they are staged, so anything already in history keeps
    # its fat blob until the history is rewritten.
    STRAYS="$(git ls-files -- \
        '*.uasset' '*.umap' '*.fbx' '*.png' '*.jpg' '*.jpeg' '*.tga' '*.psd' \
        '*.wav' '*.mp3' '*.ogg' '*.ttf' '*.otf' '*.exr' '*.hdr' '*.bin' \
        '*.dll' '*.dylib' '*.so' '*.pdb' 2>/dev/null || true)"

    if [ -n "$STRAYS" ]; then
        IN_LFS="$(git lfs ls-files -n 2>/dev/null || true)"
        NEEDS_MIGRATE=""
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            case "
${IN_LFS}
" in
                *"
${f}
"*) : ;;
                *) NEEDS_MIGRATE="${NEEDS_MIGRATE}${f}
" ;;
            esac
        done <<EOF
${STRAYS}
EOF
        if [ -n "$NEEDS_MIGRATE" ]; then
            trace_warn "These tracked files match an LFS pattern but are stored as ordinary Git blobs:"
            printf '%s' "$NEEDS_MIGRATE" | head -n 20 | sed 's/^/    /' >&2 || true
            if [ "$(printf '%s' "$NEEDS_MIGRATE" | wc -l | tr -d ' ')" -gt 20 ]; then
                trace_warn "    ... and more"
            fi
            trace_warn ""
            trace_warn "They were committed before LFS was set up. Fixing this REWRITES HISTORY"
            trace_warn "and requires a force-push plus a re-clone by everyone else, so agree on it"
            trace_warn "with the team first, then run:"
            trace_warn ""
            trace_warn "  git lfs migrate import --everything \\"
            trace_warn "      --include=\"*.uasset,*.umap,*.fbx,*.png,*.jpg,*.tga,*.psd,*.wav,*.mp3,*.ogg,*.exr,*.hdr\""
            trace_warn "  git push --force-with-lease --all"
            trace_warn ""
            trace_warn "This script will not do that for you on purpose."
        fi
    fi
else
    trace_msg "No commits yet — nothing to migrate. LFS will catch everything from the first commit."
fi

# ------------------------------------------------------------------------------
# 6. Materialise LFS content for this checkout.
# ------------------------------------------------------------------------------
if [ "$VERIFY_ONLY" = "0" ] && [ "$DO_PULL" = "1" ]; then
    if git remote | grep -q . ; then
        trace_msg "Fetching LFS objects for the current checkout"
        trace_run git lfs pull || trace_warn "git lfs pull failed (no LFS server configured yet?) — harmless on a brand-new repo."
    else
        trace_msg "No Git remote configured — skipping 'git lfs pull'."
    fi
fi

trace_msg "Git LFS is ready."
trace_msg "Reminder: lock before you edit a .umap  ->  git lfs lock Content/Maps/Arena.umap"
