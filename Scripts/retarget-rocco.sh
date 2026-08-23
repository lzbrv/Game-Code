#!/usr/bin/env bash
# ==============================================================================
# Trace — retarget-rocco.sh
#
# Makes Rocco MOVE. Run it after Scripts/import-rocco.sh.
#
#   Content/Trace/Characters/Rocco/SK_Rocco   (the import: right shape, no motion)
#        |
#        |  Scripts/retarget_rocco.py  (inside UnrealEditor-Cmd, IK Rig + IK Retargeter)
#        v
#   Content/Trace/Characters/Rocco/Retarget/IK_Manny            IKRigDefinition
#   Content/Trace/Characters/Rocco/Retarget/IK_Rocco            IKRigDefinition
#   Content/Trace/Characters/Rocco/Retarget/RTG_Manny_To_Rocco  IKRetargeter
#   Content/Trace/Characters/Rocco/Anims/ABP_Unarmed_Rocco      AnimBlueprint
#   Content/Trace/Characters/Rocco/Anims/*_Rocco                the 20 sequences
#                                                               and blend space it plays
#
# WHAT THIS FIXES. SK_Rocco is skinned to a rig whose bones are Hips1 / Spine021 /
# RightHand1; Epic's ABP_Unarmed drives root / pelvis / hand_r. Nothing in common,
# so before this script a Rocco player is drawn in his BIND POSE, gliding around
# the arena with his arms out. After it, the same blend space Epic's Mannequin
# runs has been baked onto Rocco's skeleton and he walks, jogs, jumps and lands.
#
# THE THREE ASSETS UNDER Retarget/ ARE EDITOR INPUTS, NOT SHIPPED CONTENT. The
# game loads ABP_Unarmed_Rocco and the sequences beside it — ordinary animation
# assets, no IK Rig at runtime, no second skeletal mesh component, no per-frame
# retarget. That is the whole reason this is a bake and not a Retarget Pose From
# Mesh node. See the header of Scripts/retarget_rocco.py for the rest of the
# reasoning, including why the op stack is two ops and not the engine's default
# five.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY — the assets are on disk. Run it after
# SK_Rocco is re-imported, or after ABP_Unarmed changes.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} retarget-rocco

Retargets Epic's ABP_Unarmed onto SK_Rocco's skeleton, so a player who picked
Rocco animates instead of sliding.

USAGE
  Scripts/retarget-rocco.sh [options]

OPTIONS
  -h, --help        This text

BEFORE
  Scripts/import-rocco.sh    (this needs SK_Rocco and SK_Rocco_Skeleton)

AFTER
  git status Content/Trace/Characters/Rocco

  Binary assets are LFS-tracked and cannot be merged, so lock them before
  editing if someone else might be doing the same: Scripts/lock.sh
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

trace_require_uproject
trace_resolve_engine
CMD_BIN="$(trace_editor_cmd_binary)"

ROCCO_CONTENT="${TRACE_PROJECT_ROOT}/Content/Trace/Characters/Rocco"
for REQUIRED in SK_Rocco SK_Rocco_Skeleton; do
    [ -f "${ROCCO_CONTENT}/${REQUIRED}.uasset" ] \
        || trace_die "${REQUIRED}.uasset is missing. Run Scripts/import-rocco.sh first."
done

# ------------------------------------------------------------------------------
# Wipe the outputs BEFORE the editor starts, for the reason import-rocco.sh
# records at length: EditorAssetLibrary.delete_asset frees the package but leaves
# the Asset Registry entry alive for the rest of the session, so a script that
# deletes and re-creates in one run can be handed back the PREVIOUS run's object
# and never notice. Names that have never existed in-session cannot do that.
#
# Only these two folders. SK_Rocco and friends are the INPUT and are left alone.
# ------------------------------------------------------------------------------
rm -rf "${ROCCO_CONTENT}/Retarget" "${ROCCO_CONTENT}/Anims"
trace_msg "Wiped Retarget/ and Anims/ before launch (SK_Rocco itself is the input and is untouched)"

# NOT -nullrhi: the batch retarget builds and compiles an AnimBlueprint, and the
# same trap documented in Scripts/import-rocco.sh applies.
ARGS=("$TRACE_UPROJECT"
      -run=pythonscript
      "-script=${TRACE_SCRIPT_DIR}/retarget_rocco.py"
      -unattended
      -nosplash
      -nopause
      -nosound
      -stdout
      -FullStdOutLogOutput)

trace_msg "Retargeting ABP_Unarmed onto SK_Rocco_Skeleton"

# THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN — -run=pythonscript
# returns non-zero if anything at all logged an error during the session. What is
# authoritative is the script's own problem count and what reached disk, below.
RUN_LOG="$(mktemp -t trace-retarget-rocco)"
set +e
"$CMD_BIN" "${ARGS[@]}" 2>&1 \
    | grep -E '\[Trace\]|LogPythonScriptCommandlet|LogPython: Error|Traceback|  File "|LogIKRig.*(Error|Warning)' \
    | grep -v '^\[[0-9.:-]*\]\[ *[0-9]*\]LogInit: Display: ' \
    | tee "$RUN_LOG"
set -e

trace_msg "Verifying what reached disk:"
MISSING=0

PROBLEMS="$(grep -E '\[Trace\] [0-9]+ PROBLEM\(S\)' "$RUN_LOG" || true)"
if [ -n "$PROBLEMS" ]; then
    trace_err "The retarget reported problems of its own:"
    grep -A20 'PROBLEM(S)' "$RUN_LOG" | sed 's/^/    /'
    MISSING=$((MISSING + 1))
fi
rm -f "$RUN_LOG"

check() {
    if [ -f "${TRACE_PROJECT_ROOT}/$1" ]; then
        printf '    ok      %s\n' "$1"
    else
        printf '    MISSING %s\n' "$1"
        MISSING=$((MISSING + 1))
    fi
}

check "Content/Trace/Characters/Rocco/Retarget/IK_Manny.uasset"
check "Content/Trace/Characters/Rocco/Retarget/IK_Rocco.uasset"
check "Content/Trace/Characters/Rocco/Retarget/RTG_Manny_To_Rocco.uasset"
check "Content/Trace/Characters/Rocco/Anims/ABP_Unarmed_Rocco.uasset"
check "Content/Trace/Characters/Rocco/Anims/BS_Idle_Walk_Run_Rocco.uasset"

# ABP_Unarmed_Rocco is worthless without the sequences its blend space plays, and
# "the blueprint exists" would not notice their absence.
SEQUENCES="$(find "${ROCCO_CONTENT}/Anims" -name 'M*_Rocco.uasset' 2>/dev/null | wc -l | tr -d ' ')"
printf '    %-7s %s\n' "$([ "$SEQUENCES" -ge 20 ] && echo ok || echo FEW)" \
       "${SEQUENCES} retargeted animation sequence(s)"
if [ "$SEQUENCES" -lt 20 ]; then
    trace_err "Expected at least 20 sequences beside the blueprint; ABP_Unarmed plays that many."
    MISSING=$((MISSING + 1))
fi

# Idempotence is a promise this script makes, so it is checked. A run that
# produced ABP_Unarmed_Rocco_1, or left a redirector behind, leaves everything
# above looking perfectly healthy.
STRAYS="$(find "${ROCCO_CONTENT}/Retarget" "${ROCCO_CONTENT}/Anims" -name '*_[0-9].uasset' 2>/dev/null || true)"
if [ -n "$STRAYS" ]; then
    trace_err "Uniquified asset names appeared -- the run was not idempotent:"
    printf '%s\n' "$STRAYS" | sed 's/^/    /'
    MISSING=$((MISSING + 1))
fi

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} problem(s). Search the output above for '[Trace]' lines."
    exit 1
fi

trace_msg "Rocco animates. The MOTION table above is the proof that matters: it samples"
trace_msg "each retargeted sequence and fails on one that is a bind pose with a duration."
trace_msg "In game: Trace.Characters.BodyMesh names the anim class on every pawn."
