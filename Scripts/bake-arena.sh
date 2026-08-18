#!/usr/bin/env bash
# ==============================================================================
# Trace — bake-arena.sh
#
# Bakes the procedural arena into a real, editable level at /Game/Maps/Arena_Baked
# by driving Scripts/bake-arena.py through Unreal's pythonscript commandlet
# (spec v15 §1).
#
# WHAT YOU GET
#   Content/Maps/Arena_Baked.umap                     the level, One File Per Actor
#   Content/__ExternalActors__/Maps/Arena_Baked/...   one .uasset per actor
#   Content/Trace/Materials/M_TraceSurface.uasset     the two generated materials,
#   Content/Trace/Materials/M_TraceNeon.uasset        COMMITTED this time
#   Content/Trace/Materials/Instances/...             one instance per arena tint
#
# WHY: /Game/Maps/Arena is empty and the arena is built from C++ at BeginPlay, so
# nobody can open it, click a wall and move it. This produces a level where they
# can. The procedural path is untouched and still shippable — the baked map is
# opt-in, so the two can be compared.
#
# HOW TO PLAY IT
#   Scripts/run-listen-server.sh --map /Game/Maps/Arena_Baked
#   Scripts/run-client.sh <host-ip>
# or open Content/Maps/Arena_Baked.umap in the editor and press Play.
#
# The command this wraps (printed before every launch):
#
#   "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor" "<repo>/Trace.uproject" \
#       -run=pythonscript -script="<repo>/Scripts/bake-arena.py" \
#       -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
# NOTE ON THE BINARY. This uses the full UnrealEditor rather than UnrealEditor-Cmd
# on macOS, because a first bake has to COMPILE the two materials' shader maps and
# that needs a real RHI. Once the materials exist, --nullrhi is a fast re-bake.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

MAP="/Game/Maps/Arena_Baked"
MATERIALS="/Game/Trace/Materials"
FORCE=0
NULL_RHI=0
ADD_CORE_SPAWN=0
EXTRA_ARGS=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} arena bake — procedural arena -> editable .umap

USAGE
  Scripts/bake-arena.sh [options] [-- <extra engine args>]

OPTIONS
  -m, --map <path>       Level to write. Default: ${MAP}
      --materials <path> Package dir for the promoted materials.
                         Default: ${MATERIALS}
  -f, --force            Replace an existing baked level (and its external
                         actor packages) instead of refusing
      --add-core-spawn   Do NOT bake. Open the existing level, add the placed
                         Core spawn marker if it has none, save, exit. Writes
                         ONE new actor package and leaves everything else alone,
                         which is why it exists: a --force re-bake would rewrite
                         all ~570 packages and discard every hand edit. Safe to
                         run twice; a level that already has a marker keeps it
                         exactly where it is. (spec v17 section 2)
      --nullrhi          Add -NullRHI. FASTER, but only safe once
                         Content/Trace/Materials already exists: material shader
                         compilation needs a real RHI, and without one the
                         materials save with no shader map and the whole arena
                         renders as the grey default in game.
  -n, --dry-run          Print the command; run nothing
  -h, --help             This text

ENVIRONMENT
  UE_ROOT                Engine install. Default on macOS:
                         /Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}

REQUIRES
  * A built editor target — run Scripts/build.sh first.
  * PythonScriptPlugin enabled in Trace.uproject (it is).

AFTERWARDS
  Scripts/run-listen-server.sh --map ${MAP}
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--map)      [ $# -ge 2 ] || trace_die "--map needs a value"; MAP="$2"; shift 2 ;;
        --materials)   [ $# -ge 2 ] || trace_die "--materials needs a value"; MATERIALS="$2"; shift 2 ;;
        -f|--force)    FORCE=1; shift ;;
        --add-core-spawn) ADD_CORE_SPAWN=1; shift ;;
        --nullrhi)     NULL_RHI=1; shift ;;
        -n|--dry-run)  TRACE_DRY_RUN=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        --)            shift; while [ $# -gt 0 ]; do EXTRA_ARGS+=("$1"); shift; done ;;
        -*)            trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)             trace_err "Unexpected argument: $1"; echo; usage; exit 2 ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

for PACKAGE_PATH in "$MAP" "$MATERIALS"; do
    case "$PACKAGE_PATH" in
        /Game/*) : ;;
        *) trace_die "package paths must start with /Game/, got '${PACKAGE_PATH}'." ;;
    esac
done

trace_require_uproject
trace_resolve_engine

PY_SCRIPT="${TRACE_SCRIPT_DIR}/bake-arena.py"
[ -f "$PY_SCRIPT" ] || trace_die "bake-arena.py not found at ${PY_SCRIPT}"

REL_MAP="${MAP#/Game/}"
MAP_FILE="${TRACE_PROJECT_ROOT}/Content/${REL_MAP}.umap"
EXTERNAL_DIR="${TRACE_PROJECT_ROOT}/Content/__ExternalActors__/${REL_MAP}"

if [ "$ADD_CORE_SPAWN" = "1" ] && [ "$FORCE" = "1" ]; then
    trace_die "--add-core-spawn and --force are opposites: one adds a single actor to the level you already have, the other throws that level away. Pick one."
fi

if [ "$ADD_CORE_SPAWN" = "1" ] && [ ! -f "$MAP_FILE" ]; then
    trace_die "Content/${REL_MAP}.umap does not exist; there is nothing to top up. Bake it first."
fi

if [ "$FORCE" = "1" ] && [ "$TRACE_DRY_RUN" != "1" ]; then
    # ------------------------------------------------------------------------
    # CLEAR THE LFS READ-ONLY BIT FIRST. .gitattributes marks *.uasset and *.umap
    # `lockable`, so git-lfs checks them out MODE 444 until somebody takes a lock.
    # The editor's own delete goes through the asset registry, hits that, and pops
    # a modal — which under -unattended is auto-answered "No":
    #
    #   Message dialog closed, result: No, title: Message, text: This file is
    #   read-only on disk: .../MI_Surface_Wall_North.uasset  Delete it anyway?
    #   LevelEditorSubsystem: Error: NewLevel. Failed to validate the destination.
    #   An asset already exists at this location.
    #
    # MEASURED, and it cost a bake: the delete silently failed, new_level refused
    # because the old .umap was still there, and the run aborted AFTER this script
    # had already removed the external actor packages below — i.e. it left the map
    # on disk pointing at 570 actors that no longer existed. A --force re-bake has
    # to be able to finish, so the writable bit is part of --force.
    #
    # Only the two things a bake rewrites are touched (this level and the material
    # instances it authors), never the parents, never anything else in Content/.
    for LOCKED_PATH in "$MAP_FILE" "${TRACE_PROJECT_ROOT}/Content/${MATERIALS#/Game/}/Instances"; do
        if [ -e "$LOCKED_PATH" ]; then
            trace_msg "Clearing the read-only bit on $(basename "$LOCKED_PATH") so the editor can replace it"
            chmod -R u+w "$LOCKED_PATH"
        fi
    done

    # The per-actor packages are just files and the editor will happily load a
    # stale one into the new level — that is how a "clean" re-bake ends up with
    # two floors. Remove them here, where the filesystem is visible.
    if [ -d "$EXTERNAL_DIR" ]; then
        trace_msg "Removing stale external actor packages: Content/__ExternalActors__/${REL_MAP}"
        chmod -R u+w "$EXTERNAL_DIR" 2>/dev/null || true
        rm -rf "$EXTERNAL_DIR"
    fi

    # AND THE .umap ITSELF, HERE RATHER THAN THROUGH THE ASSET REGISTRY.
    #
    # bake-arena.py calls EditorAssetLibrary.delete_asset() first and that is still
    # the tidy path — it unregisters the asset. MEASURED on this pass: with the
    # read-only bit cleared it reports SUCCESS and leaves the file on disk
    # untouched (same size, same mtime), and new_level then refuses with "Failed
    # to validate the destination. An asset already exists at this location."
    # Two bakes died there.
    #
    # A --force re-bake replaces this file wholesale, so removing it outright is
    # both safe and the only step that is actually guaranteed to work: the editor
    # process that follows scans the content directory at startup and simply never
    # sees it. Deliberately after the external-actor sweep, so a failure between
    # the two cannot leave a map pointing at actors that are already gone.
    if [ -f "$MAP_FILE" ]; then
        trace_msg "Removing the existing Content/${REL_MAP}.umap so the bake can write a new one"
        rm -f "$MAP_FILE"
    fi
fi

# bake-arena.py reads these; -script= gives no reliable way to pass argv.
export TRACE_BAKE_MAP="$MAP"
export TRACE_BAKE_MATERIALS="$MATERIALS"
export TRACE_BAKE_FORCE="$FORCE"
export TRACE_BAKE_ADD_CORE_SPAWN="$ADD_CORE_SPAWN"

EDITOR_BIN="$(trace_editor_binary)"

ARGS=("$TRACE_UPROJECT"
      -run=pythonscript
      "-script=${PY_SCRIPT}"
      -unattended
      -nosplash
      -nopause
      -stdout
      -FullStdOutLogOutput)
if [ "$NULL_RHI" = "1" ]; then
    ARGS+=(-NullRHI)
fi
ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

if [ "$ADD_CORE_SPAWN" = "1" ]; then
    trace_msg "Topping up ${TRACE_C_BOLD}${MAP}${TRACE_C_OFF}: placing the Core spawn marker if it is missing"
else
    trace_msg "Baking ${TRACE_C_BOLD}${MAP}${TRACE_C_OFF} (materials ${MATERIALS}, force=${FORCE})"
fi

# THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE BAKE, and treating it
# as one under `set -e` cost an afternoon here. UnrealEditor -run=pythonscript
# returns non-zero if ANY error was logged during the whole session, including
# engine ensures and config warnings raised at startup by code that has nothing
# to do with this script (a "Handled ensure" from an unrelated module's CDO will
# do it). The authoritative check is what landed on disk, below; the status is
# reported so a real failure is still visible.
set +e
trace_run "$EDITOR_BIN" "${ARGS[@]}"
BAKE_STATUS=$?
set -e

if [ "$TRACE_DRY_RUN" = "1" ]; then
    exit 0
fi

if [ "$BAKE_STATUS" != "0" ]; then
    trace_warn "The editor exited ${BAKE_STATUS}. That means errors were LOGGED, not necessarily that"
    trace_warn "the bake failed — checking what actually reached disk. Search the output above for"
    trace_warn "'[Bake]' lines: the bake reports its own failures there and refuses rather than half-works."
fi

if [ ! -f "$MAP_FILE" ]; then
    trace_err "The commandlet finished but Content/${REL_MAP}.umap is not on disk."
    trace_err "Scroll up for the [Bake] lines — they name the step that failed and why."
    exit 1
fi

MAP_BYTES="$(wc -c < "$MAP_FILE" | tr -d ' ')"
trace_msg "Wrote Content/${REL_MAP}.umap (${MAP_BYTES} bytes)"

# THE .umap EXISTING IS NOT PROOF OF ANYTHING, and this check earned its keep: a crash midway
# through the bake still leaves the empty level new_level() saved, so "Content/Maps/Arena_Baked.umap
# is on disk" was reported as success over a level with nothing in it. With One File Per Actor the
# .umap is DELIBERATELY tiny (~6 KB) and every actor is a separate package — so the external actor
# directory is where the arena actually is, and its absence is a failure, not a note.
if [ ! -d "$EXTERNAL_DIR" ]; then
    trace_err "No Content/__ExternalActors__/${REL_MAP} directory."
    trace_err "The .umap saved but contains no actors — the bake did not finish. Search the output"
    trace_err "above for 'Critical error', 'appError' or '[Bake]'; the failure is named there."
    exit 1
fi

EXTERNAL_COUNT="$(find "$EXTERNAL_DIR" -name '*.uasset' | wc -l | tr -d ' ')"
EXTERNAL_BYTES="$(find "$EXTERNAL_DIR" -name '*.uasset' -exec wc -c {} + | tail -n 1 | awk '{print $1}')"
trace_msg "One File Per Actor: ${EXTERNAL_COUNT} external actor packages, ${EXTERNAL_BYTES} bytes"

if [ "$EXTERNAL_COUNT" -lt 100 ]; then
    trace_err "Only ${EXTERNAL_COUNT} actors were written. A complete bake of this arena is ~570."
    exit 1
fi

# THE AUTHORITATIVE CHECK FOR BOTH MODES IS WHAT LANDED ON DISK, not the log. An
# actor package carries its class name in its name table, so grep answers "is the
# Core spawn marker really in this level" without opening the editor again.
CORE_SPAWN_COUNT="$(grep -ral 'TraceCoreSpawn' "$EXTERNAL_DIR" 2>/dev/null | wc -l | tr -d ' ')"
if [ "$ADD_CORE_SPAWN" = "1" ] || [ "$FORCE" = "1" ]; then
    if [ "$CORE_SPAWN_COUNT" = "0" ]; then
        trace_err "No ATraceCoreSpawn package in Content/__ExternalActors__/${REL_MAP}."
        trace_err "The Core spawn marker was NOT written. The level still plays - the builder falls back"
        trace_err "to the derived spawn point and logs that it did - but the spawn is not editable."
        exit 1
    fi
    trace_msg "Core spawn marker: ${CORE_SPAWN_COUNT} placed ATraceCoreSpawn actor(s) (World Outliner: Arena/Spawns)"
fi

if [ "$ADD_CORE_SPAWN" = "1" ]; then
    trace_msg "Nothing else in the level was rewritten. ${TRACE_C_BOLD}git status${TRACE_C_OFF} should show one new package."
fi

trace_msg "Play it:  ${TRACE_C_BOLD}Scripts/run-listen-server.sh --map ${MAP}${TRACE_C_OFF}"
