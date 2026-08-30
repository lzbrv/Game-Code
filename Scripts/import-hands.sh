#!/usr/bin/env bash
# ==============================================================================
# Trace — import-hands.sh
#
# Brings the owner's first-person arms rig into the project, in two stages:
#
#   stage     runs                                          output
#   rebuild   python3 import_hands.py (TRACE_HANDS_STAGE=rebuild)
#                                                           Intermediate/Hands/TraceArms.glb
#                                                           Intermediate/Hands/TraceArms_manifest.json
#   import    editor  import_hands.py (TRACE_HANDS_STAGE=import)
#                                                           Content/Trace/Characters/Hands/
#                                                             SK_TraceArms.uasset
#                                                             SK_TraceArms_Skeleton.uasset
#
#   Art/Characters/Hands/HandModel2.fbx
#        |   rebuild — plain python3, no editor: 1,251-bone Rigify rig with a
#        |   whole face on it, reduced to the 51 bones that carry (or parent)
#        |   the 47 weighted clusters, renamed to the naming the shipped
#        |   first-person hands already use, re-emitted as a skinned GLB.
#        v
#   Intermediate/Hands/TraceArms.glb
#        |   import — Interchange, into its own folder. Nothing under
#        |   Content/Trace/Art/Pack is written: SK_TraceHands stays exactly as
#        |   it is and is only READ, as the scale yardstick and as the source of
#        |   the material the one slot is bound to (MI_Pack_shell).
#        v
#   /Game/Trace/Characters/Hands/SK_TraceArms
#
# WHY THE FBX IS NOT IMPORTED DIRECTLY, and why there is no import setting that
# would have done this instead, is in the header of Scripts/import_hands.py —
# with the numbers, all of them re-measured on every run. The short version:
# imported as-is the file lands 1051 bones (467 of them face bones) on a
# 336-vertex pair of arms, UE 5.8's Interchange has no bone filter of any kind,
# and bones cannot be renamed or removed after import from a commandlet.
#
# WHAT THIS DOES NOT DO
#   It does not touch the first-person view in a normal match, it does not
#   change SK_TraceHands, and it does not pose anything. The asset it produces
#   is a T-posed pair of arms standing at chest height (Z 136.7..161.1 uu,
#   182.9 uu across) — roughly TWICE the size of the pack hands and nowhere near
#   the camera. Placing, scaling and posing it is the next stage's job.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY — the imported assets are on disk. Run it
# only after Art/Characters/Hands/HandModel2.fbx changes.
#
# VERDICTS — grep lines, not exit codes. `-run=pythonscript` returns non-zero on
# a perfectly healthy run if anything logged an error during start-up
# (generate-data-assets.py:38-43), so the exit code is not the result:
#     rebuild   [import-hands] REBUILD EXIT=0
#     import    [import-hands] EXIT=0    + the disk census printed at the end
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

STAGES=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-hands

Imports Art/Characters/Hands/HandModel2.fbx into
Content/Trace/Characters/Hands as SK_TraceArms.

USAGE
  Scripts/import-hands.sh [options]

OPTIONS
      --stage <name>  Run one stage: rebuild | import. Repeatable; the order is
                      fixed regardless of flag order. Default: both.
  -h, --help          This text

AFTER RUNNING
  git status Content/Trace/Characters/Hands

  Binary assets are LFS-tracked and cannot be merged, so lock them before
  editing if someone else might be doing the same: Scripts/lock.sh
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --stage)   shift; [ $# -gt 0 ] || trace_die "--stage needs a name"; STAGES+=("$1") ;;
        --stage=*) STAGES+=("${1#--stage=}") ;;
        -h|--help) usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

[ ${#STAGES[@]} -gt 0 ] || STAGES=(rebuild import)
for s in "${STAGES[@]}"; do
    case "$s" in
        rebuild|import) ;;
        *) trace_die "Unknown stage: ${s}" ;;
    esac
done

trace_require_uproject

FBX="${TRACE_PROJECT_ROOT}/Art/Characters/Hands/HandModel2.fbx"
GLB="${TRACE_PROJECT_ROOT}/Intermediate/Hands/TraceArms.glb"
MANIFEST="${TRACE_PROJECT_ROOT}/Intermediate/Hands/TraceArms_manifest.json"
DEST="${TRACE_PROJECT_ROOT}/Content/Trace/Characters/Hands"
LOGDIR="${TRACE_PROJECT_ROOT}/Saved/Logs/import-hands"
mkdir -p "$LOGDIR"

MISSING=0
RUN_LOG=""

check_file() {
    if [ -f "$1" ]; then
        printf '    ok      %s\n' "${1#${TRACE_PROJECT_ROOT}/}"
    else
        printf '    MISSING %s\n' "${1#${TRACE_PROJECT_ROOT}/}"
        MISSING=$((MISSING + 1))
    fi
}

# grep_verdict <verdict-regex> <what>   — the import-characters.sh contract.
grep_verdict() {
    if grep -qE "$1" "$RUN_LOG"; then
        printf '    ok      %s\n' "$2"
    else
        trace_err "verdict line '$1' not found — $2 FAILED. Full log: ${LOGDIR}/"
        MISSING=$((MISSING + 1))
    fi
    if grep -qE '\[Trace\] [0-9]+ PROBLEM\(S\)' "$RUN_LOG"; then
        trace_err "the run reported problems of its own:"
        grep -A30 'PROBLEM(S)' "$RUN_LOG" | sed 's/^/    /'
        MISSING=$((MISSING + 1))
    fi
}

# The editor cannot open this project without the compiled game module: it pops
# "The game module 'Trace' could not be found" and exits before the python
# script runs. Scripts/build.sh DELETES that dylib while it relinks, so a
# sibling build in flight (or a failed one) leaves a window where the import
# stage dies at start-up. Check BEFORE wiping anything — the rule
# import-characters.sh paid for on 2026-08-24.
require_editor_module() {
    local lib
    case "$TRACE_HOST_PLATFORM" in
        Mac)     lib="${TRACE_PROJECT_ROOT}/Binaries/Mac/libUnrealEditor-${TRACE_PROJECT_NAME}.dylib" ;;
        Linux)   lib="${TRACE_PROJECT_ROOT}/Binaries/Linux/libUnrealEditor-${TRACE_PROJECT_NAME}.so" ;;
        Windows) lib="${TRACE_PROJECT_ROOT}/Binaries/Win64/UnrealEditor-${TRACE_PROJECT_NAME}.dll" ;;
        *)       return 0 ;;
    esac
    [ -f "$lib" ] || trace_die "the editor game module is missing ($(basename "$lib")) — \
a build is in flight or the last one failed. Run Scripts/build.sh until it prints \
'Result: Succeeded', then re-run this stage. NOTHING WAS WIPED."
}

stage_rebuild() {
    trace_msg "STAGE rebuild: HandModel2.fbx -> Intermediate/Hands/TraceArms.glb"
    [ -f "$FBX" ] || trace_die "Source model missing: ${FBX}"
    # An LFS pointer is a ~130-byte text file starting 'version https://'.
    # Handing that to the FBX reader produces a baffling parse error instead of
    # a useful one (the import-pack.sh guard).
    if head -c 16 "$FBX" | grep -q '^version https'; then
        trace_die "${FBX} is an unfetched Git LFS pointer, not the model. Run: git lfs pull"
    fi
    rm -rf "$(dirname "$GLB")"
    RUN_LOG="${LOGDIR}/rebuild.log"
    set +e
    perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' 120 \
        env TRACE_HANDS_STAGE=rebuild python3 "${TRACE_SCRIPT_DIR}/import_hands.py" 2>&1 \
        | tee "$RUN_LOG"
    set -e
    grep_verdict '\[import-hands\] REBUILD EXIT=0' "import_hands.py rebuild verdict"
    check_file "$GLB"
    check_file "$MANIFEST"
}

stage_import() {
    trace_msg "STAGE import: TraceArms.glb -> Content/Trace/Characters/Hands/"
    [ -f "$GLB" ] || trace_die "${GLB} is missing. Run the rebuild stage first."
    trace_resolve_engine
    require_editor_module
    local cmd_bin
    cmd_bin="$(trace_editor_cmd_binary)"

    # THE WIPE HAPPENS OUT HERE, BEFORE THE EDITOR STARTS.
    # EditorAssetLibrary.delete_asset removes the file but the Asset Registry
    # keeps the entry for the rest of the session, so renaming a fresh import
    # onto a deleted name fails and the run silently re-measures LAST run's
    # asset. The full story is in Scripts/import-rocco.sh and import_rocco.py
    # sweep() — do not relitigate it.
    rm -rf "$DEST"

    RUN_LOG="${LOGDIR}/import.filtered.log"
    set +e
    # NOT -nullrhi: the import builds a skeletal mesh's render data.
    perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' 900 \
        env TRACE_HANDS_STAGE=import \
        "$cmd_bin" "$TRACE_UPROJECT" \
        -run=pythonscript "-script=${TRACE_SCRIPT_DIR}/import_hands.py" \
        -unattended -nosplash -nopause -nosound -stdout -FullStdOutLogOutput \
        -RenderOffScreen "-abslog=${LOGDIR}/import.log" 2>&1 \
        | grep -E '\[Trace\]|LogPythonScriptCommandlet|LogPython: Error|Traceback|  File "|LogInterchange.*(Error|Warning)' \
        | tee "$RUN_LOG"
    set -e
    grep_verdict '\[import-hands\] EXIT=0' "import_hands.py import verdict"

    trace_msg "Verifying what reached disk:"
    check_file "${DEST}/SK_TraceArms.uasset"
    check_file "${DEST}/SK_TraceArms_Skeleton.uasset"
    # Scaffolding, not art: the configured pipelines are deleted by the script.
    if [ -d "${DEST}/_Pipelines" ] || [ -d "${DEST}/_Import" ] || [ -d "${DEST}/_ProbeFbxAsIs" ]; then
        trace_err "import scaffolding survived under ${DEST#${TRACE_PROJECT_ROOT}/} — \
_Pipelines/_Import/_ProbeFbxAsIs should all be gone"
        MISSING=$((MISSING + 1))
    fi
    # The one thing this script must never have done.
    if [ ! -f "${TRACE_PROJECT_ROOT}/Content/Trace/Art/Pack/Hands/SK_TraceHands.uasset" ]; then
        trace_err "SK_TraceHands is gone. This script only ever READS it; something else \
deleted the shipped first-person hands."
        MISSING=$((MISSING + 1))
    fi
}

for s in rebuild import; do
    for want in "${STAGES[@]}"; do
        [ "$s" = "$want" ] && { "stage_${s}"; break; }
    done
done

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} problem(s). Search the output above for '[Trace]' lines, which"
    trace_err "name the API or the assertion that failed. Logs: ${LOGDIR}/"
    exit 1
fi

trace_msg "Done. The import report above lists every bone with its parent and its"
trace_msg "component-space position, the material slot as re-read from disk, the size"
trace_msg "against SK_TraceHands, and the ring-finger axis table — that is the contract"
trace_msg "the pose stage builds on."
