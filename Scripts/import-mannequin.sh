#!/usr/bin/env bash
# ==============================================================================
# Trace — import-mannequin.sh
#
# Copies Epic's Mannequin character art out of THIS developer's own Unreal
# install into the project's Content folder, so the repository can stay
# text-only. Nothing here is authored by us and nothing here is committed:
# every developer is pinned to UE 5.8 (see TRACE_ENGINE_VERSION), so everybody
# gets byte-identical files from their own engine.
#
#   source:  $UE_ROOT/Templates/TemplateResources/High/Characters/Content/Mannequins
#   dest:    <repo>/Content/Characters/Mannequins        (gitignored)
#
# WHY THAT DESTINATION, EXACTLY
#   .uasset files store hard references as absolute /Game paths. The template
#   assets were saved from a project where they lived at
#   /Game/Characters/Mannequins/..., and SKM_Manny_Simple.uasset literally
#   contains the strings
#       /Game/Characters/Mannequins/Meshes/SK_Mannequin
#       /Game/Characters/Mannequins/Materials/Manny/MI_Manny_01_New
#       /Game/Characters/Mannequins/Rigs/PA_Mannequin
#   Drop the folder anywhere else and the mesh loads with a null skeleton, null
#   materials and null physics asset. So the destination is not a preference,
#   it is dictated by the files. This script does not hardcode it on faith --
#   it reads the reference back out of the mesh and derives the destination
#   from it (see trace_derive_game_path below), and refuses to run if what it
#   finds disagrees with what this project expects.
#
# The script is idempotent: run it as often as you like. It only writes files
# that differ, and it prints what it changed.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

# The subpath of the engine install that holds the art, and the /Game path the
# assets were cooked to expect. EXPECTED_GAME_PATH is an assertion, not an
# input: it is checked against what the .uasset actually references.
TEMPLATE_SUBPATH="Templates/TemplateResources/High/Characters/Content/Mannequins"
EXPECTED_GAME_PATH="/Game/Characters/Mannequins"

# The mesh we probe for the embedded reference, and the files that must exist
# afterwards for ATraceCharacter to render anything (Source/Trace/Core/TraceCharacter.cpp
# loads exactly these two, softly, and falls back to a coloured capsule if they
# are missing).
PROBE_MESH_RELPATH="Meshes/SKM_Manny_Simple.uasset"
REQUIRED_ASSETS=(
    "Meshes/SKM_Manny_Simple.uasset"
    "Meshes/SKM_Quinn_Simple.uasset"
    "Meshes/SK_Mannequin.uasset"
    "Anims/Unarmed/ABP_Unarmed.uasset"
    "Anims/Unarmed/BS_Idle_Walk_Run.uasset"
    "Materials/M_Mannequin.uasset"
)

DO_DRY_RUN=0
DO_FORCE=0
DO_VERIFY_ONLY=0

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-mannequin

Copies Epic's Mannequin (skeletal meshes, skeleton, animations, the unarmed
anim blueprint, materials and textures) from your own UE ${TRACE_ENGINE_VERSION}
install into <repo>/Content/Characters/Mannequins.

That folder is gitignored on purpose: binary art is not committed, it is
imported. Run this once after cloning, and again after changing engine
versions.

USAGE
  Scripts/import-mannequin.sh [options]

OPTIONS
      --verify        Check what is already imported and report; copy nothing
      --force         Recopy every file even if it is already identical
  -n, --dry-run       Show what would be copied; write nothing
  -h, --help          This text

ENVIRONMENT
  UE_ROOT             Engine install to import from. Default on macOS:
                      /Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}
                      A per-developer override can also live in <repo>/.ue-root

AFTER RUNNING
  Assets resolve in-game at ${EXPECTED_GAME_PATH}/...
  If you skip this step the game still runs: ATraceCharacter falls back to a
  plain team-coloured capsule and logs a warning pointing back here.

  To see that fallback deliberately, without deleting anything, launch the game
  with -TraceNoCharacterArt.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --verify)        DO_VERIFY_ONLY=1 ;;
        --force)         DO_FORCE=1 ;;
        -n|--dry-run)    DO_DRY_RUN=1 ;;
        -h|--help)       usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

# ------------------------------------------------------------------------------
# trace_derive_game_path <uasset> <relpath-under-mannequins-without-extension>
#
# Reads the /Game path that the asset believes it lives at, straight out of the
# file, and prints the root of the Mannequins folder implied by it.
#
# `strings` is a binutils tool and is not guaranteed to exist everywhere, so
# this uses tr/grep, which are POSIX: turn every non-path byte into a newline,
# then keep lines that look like the reference we are after.
# ------------------------------------------------------------------------------
trace_derive_game_path() {
    local uasset="$1" suffix="$2" found
    found="$(LC_ALL=C tr -c 'A-Za-z0-9_/\-' '\n' < "$uasset" \
        | grep -E "^/Game/.+/${suffix}$" \
        | head -n 1 || true)"
    [ -n "$found" ] || return 1
    # Strip the known suffix to leave the folder root.
    printf '%s' "${found%/${suffix}}"
}

# ------------------------------------------------------------------------------
# Resolve the engine and the source folder
# ------------------------------------------------------------------------------
trace_resolve_engine

SRC="${UE_ROOT}/${TEMPLATE_SUBPATH}"
DEST_CONTENT_SUBPATH="Characters/Mannequins"   # replaced below by the derived value
DEST=""

if [ ! -d "$SRC" ]; then
    trace_err "Epic's Mannequin art is not in this engine install."
    trace_err "  looked for: ${SRC}"
    trace_err ""
    trace_err "That folder ships with the engine's template resources. If it is missing,"
    trace_err "the install is partial: open the Epic Games Launcher, find Unreal Engine"
    trace_err "${TRACE_ENGINE_VERSION}, choose Options, and make sure \"Templates and Feature"
    trace_err "Packs\" (or \"Starter Content\") is ticked, then Apply."
    trace_err ""
    trace_err "The game runs without it -- characters fall back to plain team-coloured"
    trace_err "capsules -- but they will not be animated."
    exit 1
fi

PROBE="${SRC}/${PROBE_MESH_RELPATH}"
if [ ! -f "$PROBE" ]; then
    trace_die "Source folder exists but ${PROBE_MESH_RELPATH} is missing from it: ${SRC}"
fi

# Derive the destination from the asset itself rather than trusting a constant.
DERIVED="$(trace_derive_game_path "$PROBE" "Meshes/SKM_Manny_Simple" || true)"

if [ -z "$DERIVED" ]; then
    trace_warn "Could not read a /Game reference out of ${PROBE_MESH_RELPATH}."
    trace_warn "Falling back to the documented path: ${EXPECTED_GAME_PATH}"
    DERIVED="$EXPECTED_GAME_PATH"
elif [ "$DERIVED" != "$EXPECTED_GAME_PATH" ]; then
    trace_err "This engine's Mannequin assets reference a different /Game path than"
    trace_err "Trace's C++ expects, so importing them would produce broken references:"
    trace_err "  assets say:       ${DERIVED}"
    trace_err "  TraceCharacter.cpp expects: ${EXPECTED_GAME_PATH}"
    trace_err ""
    trace_err "Fix the paths in Source/Trace/Core/TraceCharacter.cpp (TraceCharacterAssets)"
    trace_err "and in EXPECTED_GAME_PATH at the top of this script, together."
    exit 1
fi

# /Game maps to <repo>/Content.
DEST_CONTENT_SUBPATH="${DERIVED#/Game/}"
DEST="${TRACE_PROJECT_ROOT}/Content/${DEST_CONTENT_SUBPATH}"

trace_msg "Source : ${TRACE_C_BOLD}${SRC}${TRACE_C_OFF}"
trace_msg "Dest   : ${TRACE_C_BOLD}${DEST}${TRACE_C_OFF}"
trace_msg "Mounts at ${TRACE_C_BOLD}${DERIVED}${TRACE_C_OFF} (read out of ${PROBE_MESH_RELPATH}, not guessed)"

# ------------------------------------------------------------------------------
# --verify: report and stop
# ------------------------------------------------------------------------------
trace_report_state() {
    local missing=0 a
    for a in "${REQUIRED_ASSETS[@]}"; do
        if [ -f "${DEST}/${a}" ]; then
            printf '    ok      %s\n' "$a"
        else
            printf '    MISSING %s\n' "$a"
            missing=$((missing + 1))
        fi
    done
    return $missing
}

if [ "$DO_VERIFY_ONLY" = "1" ]; then
    if [ ! -d "$DEST" ]; then
        trace_warn "Not imported yet -- ${DEST} does not exist."
        trace_warn "Run: Scripts/import-mannequin.sh"
        exit 1
    fi
    trace_msg "Checking required assets:"
    if trace_report_state; then
        trace_msg "$(find "$DEST" -name '*.uasset' | wc -l | tr -d ' ') .uasset files, $(du -sh "$DEST" | cut -f1) on disk."
        trace_msg "Mannequin import looks complete."
        exit 0
    else
        trace_err "Import is incomplete. Re-run: Scripts/import-mannequin.sh --force"
        exit 1
    fi
fi

# ------------------------------------------------------------------------------
# Copy
# ------------------------------------------------------------------------------
SRC_COUNT="$(find "$SRC" -type f -name '*.uasset' | wc -l | tr -d ' ')"
SRC_SIZE="$(du -sh "$SRC" | cut -f1)"
trace_msg "Importing ${SRC_COUNT} .uasset files (${SRC_SIZE})..."

RSYNC_FLAGS=(-rlt --itemize-changes)
[ "$DO_FORCE" = "1" ] && RSYNC_FLAGS+=(--ignore-times)
[ "$DO_DRY_RUN" = "1" ] && RSYNC_FLAGS+=(--dry-run)

if command -v rsync >/dev/null 2>&1; then
    mkdir -p "$DEST"
    # Trailing slashes matter: copy the CONTENTS of SRC into DEST, so re-running
    # never nests Mannequins/Mannequins.
    trace_print_cmd rsync "${RSYNC_FLAGS[@]}" "${SRC}/" "${DEST}/"

    # rsync's status is captured rather than piped straight into grep: under `set -o pipefail` a
    # failing rsync would still be masked by the `|| true` that grep needs (grep exits 1 when
    # nothing changed, which is the normal idempotent case). Keeping them separate means a real
    # copy failure is reported as a copy failure.
    RSYNC_LOG="$(mktemp -t trace-import-mannequin)"
    if ! rsync "${RSYNC_FLAGS[@]}" "${SRC}/" "${DEST}/" > "$RSYNC_LOG"; then
        trace_err "rsync failed (exit $?). See ${RSYNC_LOG}"
        exit 1
    fi
    # Drop rsync's "directory timestamp only" noise; it appears on every re-run and means nothing.
    CHANGED="$(grep -vE '^\.d\.\.t' "$RSYNC_LOG" || true)"
    rm -f "$RSYNC_LOG"

    if [ -n "$CHANGED" ]; then
        printf '%s\n' "$CHANGED" | sed 's/^/    /'
        trace_msg "$(printf '%s\n' "$CHANGED" | wc -l | tr -d ' ') entries written."
    else
        trace_msg "Everything was already up to date -- nothing written."
    fi
else
    # rsync is standard on macOS and every Linux distro that can build Unreal,
    # but do not fail outright if a stripped container is missing it.
    trace_warn "rsync not found; falling back to cp -R (copies everything, every time)."
    if [ "$DO_DRY_RUN" = "1" ]; then
        trace_msg "dry run -- would cp -R '${SRC}/' -> '${DEST}/'"
    else
        rm -rf "$DEST"
        mkdir -p "$(dirname "$DEST")"
        cp -R "$SRC" "$DEST"
        trace_msg "Copied ${SRC_COUNT} .uasset files."
    fi
fi

if [ "$DO_DRY_RUN" = "1" ]; then
    trace_msg "dry run -- nothing was written."
    exit 0
fi

# ------------------------------------------------------------------------------
# Verify what landed
# ------------------------------------------------------------------------------
trace_msg "Verifying:"
if ! trace_report_state; then
    trace_err "Import finished but required assets are missing. Something interrupted the copy."
    exit 1
fi

trace_msg "Imported ${TRACE_C_BOLD}$(find "$DEST" -name '*.uasset' | wc -l | tr -d ' ')${TRACE_C_OFF} .uasset files, ${TRACE_C_BOLD}$(du -sh "$DEST" | cut -f1)${TRACE_C_OFF} on disk."
trace_msg "In-game paths:"
printf '    %s/Meshes/SKM_Manny_Simple\n'          "$DERIVED"
printf '    %s/Meshes/SKM_Quinn_Simple\n'          "$DERIVED"
printf '    %s/Anims/Unarmed/ABP_Unarmed\n'        "$DERIVED"
trace_msg "This folder is gitignored -- do not commit it. Re-run this script instead."
