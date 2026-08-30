#!/usr/bin/env bash
# ==============================================================================
# Trace — pose-hands.sh
#
# Poses the owner's first-person arms rig (SK_TraceArms, brought in by
# Scripts/import-hands.sh) onto the three weapons the first-person view draws,
# and emits the result as AnimSequences.
#
#   stage    runs                                     output
#   build    python3 pose_hands.py                    Intermediate/Hands/TraceArmsPoses.glb
#                                                     Intermediate/Hands/TraceArmsPose_<Name>.glb
#                                                     Intermediate/Hands/TraceArmsPoses_manifest.json
#   import   editor  pose_hands.py                    /Game/Trace/Characters/Hands/Poses/
#                                                       A_TraceArms_Pistol
#                                                       A_TraceArms_Smg
#                                                       A_TraceArms_Knife
#                                                       A_TraceArms_Fist
#                                                     /Game/Trace/Temp/Preview/SK_TraceArmsPose_*
#
#   Intermediate/Hands/TraceArms.glb   +   Art/Railgun/railgun.glb
#        |                                 Art/Smg/railgun_smg.glb
#        |                                 Art/Pack/models/butterfly_knife.glb
#        |
#        |  build — plain python3, no editor.  Solves each hold against the
#        |  weapon's OWN measured landmarks (grip, trigger, foregrip, magazine,
#        |  balisong handle), places the arms with a two-bone IK, closes every
#        |  finger by binary search until its measured flesh touches the weapon,
#        |  and then checks all 1340 skinned vertices against the weapon's
#        |  triangles for penetration.
#        v
#   Intermediate/Hands/TraceArmsPoses.glb   (the same skin, plus 4 animations)
#        |
#        |  import — Interchange, animations only, onto the existing
#        |  SK_TraceArms_Skeleton.  Frame 0 of every clip is then read back and
#        |  checked against the pose the solver wrote.
#        v
#   /Game/Trace/Characters/Hands/Poses/A_TraceArms_*
#
# WHY THERE ARE ALSO PREVIEW MESHES.  A commandlet cannot render, so the frames
# come from `-game`, and a SkeletalMeshActor playing an AnimSequence is one more
# thing that has to work before a photograph means anything.  The build stage
# also bakes each pose into a mesh whose REST pose IS the pose; those are the
# render subject, they live in /Game/Trace/Temp (throwaway), and both they and
# the AnimSequences are checked against the same solver output — so if the two
# ever disagree the run says so instead of both being wrong the same way.
#
# WHAT THIS DOES NOT DO
#   It does not change the first-person view.  Nothing in C++ knows about these
#   clips yet: they are assets on the arms rig, with the component transform
#   they were solved against recorded in the manifest and in the report.  The
#   owner asked for this "only in the practice range"; wiring it there is the
#   next step and it needs the numbers this stage prints.
#
# VERDICTS — grep lines, not exit codes.  `-run=pythonscript` returns non-zero
# on a healthy run if anything logged an error during start-up:
#     build    [pose-hands] BUILD EXIT=0
#     import   [pose-hands] EXIT=0    + the disk census printed at the end
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

STAGES=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} pose-hands

Poses SK_TraceArms on the pistol, the SMG and the knife and imports the result
as AnimSequences under /Game/Trace/Characters/Hands/Poses.

USAGE
  Scripts/pose-hands.sh [options]

OPTIONS
      --stage <name>  Run one stage: build | import. Repeatable; the order is
                      fixed regardless of flag order. Default: both.
  -h, --help          This text

REQUIRES
  Scripts/import-hands.sh must have run first — this stage poses the rig that
  one imports, and reads Intermediate/Hands/TraceArms.glb directly.
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

[ ${#STAGES[@]} -gt 0 ] || STAGES=(build import)
for s in "${STAGES[@]}"; do
    case "$s" in
        build|import) ;;
        *) trace_die "Unknown stage: ${s}" ;;
    esac
done

trace_require_uproject

ARMS_GLB="${TRACE_PROJECT_ROOT}/Intermediate/Hands/TraceArms.glb"
POSES_GLB="${TRACE_PROJECT_ROOT}/Intermediate/Hands/TraceArmsPoses.glb"
POSES_MANIFEST="${TRACE_PROJECT_ROOT}/Intermediate/Hands/TraceArmsPoses_manifest.json"
DEST="${TRACE_PROJECT_ROOT}/Content/Trace/Characters/Hands/Poses"
LOGDIR="${TRACE_PROJECT_ROOT}/Saved/Logs/pose-hands"
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

# The editor cannot open this project without the compiled game module, and
# Scripts/build.sh DELETES that dylib while it relinks — so a sibling build in
# flight leaves a window where the import stage dies at start-up. Checked BEFORE
# anything is wiped: the rule import-characters.sh paid for on 2026-08-24.
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

stage_build() {
    trace_msg "STAGE build: TraceArms.glb + the weapon art -> TraceArmsPoses.glb"
    [ -f "$ARMS_GLB" ] || trace_die "${ARMS_GLB} is missing. Run Scripts/import-hands.sh first."
    RUN_LOG="${LOGDIR}/build.log"
    set +e
    perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' 300 \
        env TRACE_POSE_STAGE=build python3 "${TRACE_SCRIPT_DIR}/pose_hands.py" 2>&1 \
        | tee "$RUN_LOG"
    set -e
    grep_verdict '\[pose-hands\] BUILD EXIT=0' "pose_hands.py build verdict"
    check_file "$POSES_GLB"
    check_file "$POSES_MANIFEST"
}

stage_import() {
    trace_msg "STAGE import: TraceArmsPoses.glb -> /Game/Trace/Characters/Hands/Poses"
    [ -f "$POSES_GLB" ] || trace_die "${POSES_GLB} is missing. Run the build stage first."
    trace_resolve_engine
    require_editor_module
    local cmd_bin
    cmd_bin="$(trace_editor_cmd_binary)"

    # THE WIPE HAPPENS OUT HERE, BEFORE THE EDITOR STARTS.
    # EditorAssetLibrary.delete_asset removes the file but the Asset Registry
    # keeps the entry for the rest of the session, so a rename onto a
    # just-deleted name fails and the run silently re-measures LAST run's asset.
    # (The same thing sinks LevelEditorSubsystem.new_level onto an existing map,
    # which is why the frame harness wipes its .umap files from the shell too.)
    rm -rf "$DEST"

    RUN_LOG="${LOGDIR}/import.filtered.log"
    set +e
    # NOT -nullrhi: the preview import builds a skeletal mesh's render data.
    perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' 900 \
        env TRACE_POSE_STAGE=import \
        "$cmd_bin" "$TRACE_UPROJECT" \
        -run=pythonscript "-script=${TRACE_SCRIPT_DIR}/pose_hands.py" \
        -unattended -nosplash -nopause -nosound -stdout -FullStdOutLogOutput \
        -RenderOffScreen "-abslog=${LOGDIR}/import.log" 2>&1 \
        | grep -E '\[Trace\]|LogPythonScriptCommandlet|LogPython: Error|Traceback|  File "|LogInterchange.*(Error|Warning)' \
        | tee "$RUN_LOG"
    set -e
    grep_verdict '\[pose-hands\] EXIT=0' "pose_hands.py import verdict"

    trace_msg "Verifying what reached disk:"
    for name in Pistol Smg Knife Fist; do
        check_file "${DEST}/A_TraceArms_${name}.uasset"
    done
    # Scaffolding, not art: the configured pipelines are deleted by the script.
    if [ -d "${DEST}/../_PosePipelines" ] || [ -d "${DEST}/../_PoseImport" ]; then
        trace_err "import scaffolding survived — _PosePipelines/_PoseImport should both be gone"
        MISSING=$((MISSING + 1))
    fi
    # The two things this script must never have done.
    if [ ! -f "${TRACE_PROJECT_ROOT}/Content/Trace/Characters/Hands/SK_TraceArms.uasset" ]; then
        trace_err "SK_TraceArms is gone. This script only ever READS it."
        MISSING=$((MISSING + 1))
    fi
    if [ ! -f "${TRACE_PROJECT_ROOT}/Content/Trace/Art/Pack/Hands/SK_TraceHands.uasset" ]; then
        trace_err "SK_TraceHands is gone. This script never touches the shipped hands."
        MISSING=$((MISSING + 1))
    fi
}

for s in build import; do
    for want in "${STAGES[@]}"; do
        [ "$s" = "$want" ] && { "stage_${s}"; break; }
    done
done

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} problem(s). Search the output above for '[Trace]' lines, which"
    trace_err "name the API or the assertion that failed. Logs: ${LOGDIR}/"
    exit 1
fi

trace_msg "Done. The build report above gives, per pose: where each hand was put and"
trace_msg "how far the arm had to reach, every finger's curl and the gap left to the"
trace_msg "weapon, the trigger finger's distance to the trigger, and the deepest any"
trace_msg "skin vertex ended up inside the gun — which is what 'holds it correctly'"
trace_msg "was graded on, alongside the frames."
