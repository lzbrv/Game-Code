#!/usr/bin/env bash
# ==============================================================================
# Trace — import-rocco.sh
#
# Brings the test character model for Rocco into the project as a skeletal mesh.
#
#   Art/Characters/Rocco/RoccoTest.fbx
#        |
#        |  Scripts/import_rocco.py  (inside UnrealEditor-Cmd, via Interchange)
#        v
#   Content/Trace/Characters/Rocco/SK_Rocco            SkeletalMesh, 25 bones
#   Content/Trace/Characters/Rocco/SK_Rocco_Skeleton   Skeleton
#   Content/Trace/Characters/Rocco/SK_Rocco_PhysicsAsset  PhysicsAsset
#   Content/Trace/Characters/Rocco/M_RoccoPlaceholder  Material
#   Content/Trace/Characters/Rocco/MI_Rocco_*          one instance of it per
#                                                      material slot, ten of them
#
# TWO THINGS TO KNOW BEFORE YOU LOOK AT THE RESULT
#
#   THE MODEL HAS NO COLOUR ON IT, and that is the file's doing, not the
#   importer's. The FBX contains no embedded images at all; its six texture
#   references point into C:\Users\ranen\OneDrive\Documents\Blender\textures\packed\
#   on the artist's own machine, without file extensions, and all ten of its
#   materials carry the same untouched 0.8 grey DiffuseColor, because the albedo
#   was coming from those textures. So SK_Rocco has the right silhouette, the
#   right skin weights, ten correctly-named material slots — and no albedo
#   anywhere on it. WHAT OTHER PLAYERS SEE IS A FLAT, SINGLE-COLOUR BODY.
#
#   The import neither hides that nor invents anything to cover it. It recovers
#   the only per-slot data the file does carry — Shininess and ReflectionFactor,
#   which span roughness 0.52 to 1.00 and put metal on two of the ten slots —
#   onto one MI_Rocco_* instance per slot, and it MEASURES the missing images
#   rather than asserting them: the report prints how many are embedded (0) and
#   lists the paths the file points at instead. To fix the colour the artist
#   re-exports with Path Mode = Copy and "Embed Textures" ticked, or sends the
#   six PNGs; the slot names survive, so a textured re-import lands on the same
#   slots and the report's verdict flips on its own.
#
#   THE SKELETON IS NOT THE MANNEQUIN'S. Hips / Spine / LeftHand / RightHand /
#   neck / Head — a generic Mixamo-shaped rig with none of pelvis, spine_01,
#   thigh_l, hand_r or root, and every bone carries a trailing "1" that the FBX
#   translator added to break a name collision inside the file (Hips1, Spine011).
#   ABP_Unarmed cannot drive this rig, and the knife attach in
#   TraceWeaponComponent.cpp, which looks up a socket called "hand_r" behind a
#   DoesSocketExist guard, finds nothing — so a Rocco player would carry an
#   invisible knife. Neither renaming bones nor adding sockets is possible from a
#   commandlet in 5.8 (the reasons are in the header of Scripts/import_rocco.py,
#   written down so nobody spends the afternoon twice).
#
#   SO THIS IMPORT ALONE LEAVES ROCCO IN HIS BIND POSE. What closes that gap is
#   ***  Scripts/retarget-rocco.sh  ***, which builds an IK Rig for each skeleton
#   and bakes Epic's own ABP_Unarmed onto this one. RUN IT AFTER THIS SCRIPT —
#   this one imports the art and MEASURES the gap (the ATTACH POINTS table below
#   is those measurements); that one closes it.
#
# The run is idempotent. Everything under /Game/Trace/Characters/Rocco except
# the authored materials is deleted before the import, so running this twice
# gives you SK_Rocco twice and never SK_Rocco_1.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY — the imported assets are on disk. Run it
# only after Art/Characters/Rocco/RoccoTest.fbx changes.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

FORCE=0

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-rocco

Imports Art/Characters/Rocco/RoccoTest.fbx into Content/Trace/Characters/Rocco.

USAGE
  Scripts/import-rocco.sh [options]

OPTIONS
      --force       Also rebuild M_RoccoPlaceholder and its ten MI_Rocco_*
                    per-slot instances, which are otherwise left alone across
                    runs (they are authored here, not imported, and rebuilding
                    churns eleven binary assets' GUIDs for nothing -- their
                    parameters are rewritten every run either way)
  -h, --help        This text

AFTER RUNNING
  git status Content/Trace/Characters/Rocco

  Binary assets are LFS-tracked and cannot be merged, so lock them before
  editing if someone else might be doing the same: Scripts/lock.sh
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --force)   FORCE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

trace_require_uproject
trace_resolve_engine
CMD_BIN="$(trace_editor_cmd_binary)"

FBX="${TRACE_PROJECT_ROOT}/Art/Characters/Rocco/RoccoTest.fbx"
[ -f "$FBX" ] || trace_die "Source model missing: ${FBX}"
# An LFS pointer is a ~130-byte text file starting 'version https://'. Handing
# that to Interchange produces a baffling parse error instead of a useful one.
if head -c 16 "$FBX" | grep -q '^version https'; then
    trace_die "${FBX} is an unfetched Git LFS pointer, not the model. Run: git lfs pull"
fi

export TRACE_FORCE_ROCCO="$FORCE"

# ------------------------------------------------------------------------------
# Wipe the destination BEFORE the editor starts. This is what makes the run
# idempotent, and it has to happen out here rather than in the Python.
#
# EditorAssetLibrary.delete_asset removes the file but the Asset Registry keeps
# the entry for the rest of the session -- does_asset_exist still answers True
# immediately after a successful delete -- so renaming the freshly imported mesh
# onto that name fails. The failure mode was silent and nasty: the rename failed,
# the still-loaded previous asset was picked up instead, and the run measured and
# re-saved LAST run's mesh while reporting success. Deleting the packages before
# anything is loaded leaves the folder exactly as a fresh clone would have it.
#
# M_RoccoPlaceholder and the ten MI_Rocco_* instances of it are authored by the
# script rather than imported, so they survive unless --force: rebuilding them
# every run churns eleven binary LFS assets' GUIDs for nothing. The Python
# rewrites their parameters in place on every run, so a re-export with different
# surface values still lands.
# ------------------------------------------------------------------------------
ROCCO_CONTENT="${TRACE_PROJECT_ROOT}/Content/Trace/Characters/Rocco"
if [ -d "$ROCCO_CONTENT" ]; then
    if [ "$FORCE" = "1" ]; then
        find "$ROCCO_CONTENT" -name '*.uasset' -delete
    else
        find "$ROCCO_CONTENT" -name '*.uasset' \
            ! -name 'M_RoccoPlaceholder.uasset' ! -name 'MI_Rocco_*.uasset' -delete
    fi
    # Empty _Import / _Pipelines directories a crashed run may have left.
    find "$ROCCO_CONTENT" -mindepth 1 -type d -empty -delete
    trace_msg "Wiped $(basename "$ROCCO_CONTENT") before launch (kept the authored materials: $([ "$FORCE" = "1" ] && echo no || echo yes))"
fi

# NOT -nullrhi. The placeholder material is compiled on the first run, and that
# needs a real RHI -- the same trap documented in Scripts/bake-arena.sh and
# Scripts/import-pack.sh.
ARGS=("$TRACE_UPROJECT"
      -run=pythonscript
      "-script=${TRACE_SCRIPT_DIR}/import_rocco.py"
      -unattended
      -nosplash
      -nopause
      -nosound
      -stdout
      -FullStdOutLogOutput)

trace_msg "Importing Rocco (TRACE_FORCE_ROCCO=${FORCE})"

# THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN. -run=pythonscript
# returns non-zero if ANY error was logged during the whole session, including
# engine warnings raised at startup. The authoritative check is what reached
# disk, below.
# 'LogPython: Error' and 'Traceback' are in this filter because they were once
# missing from it: the script raised, the commandlet printed a perfectly good
# traceback, and all that reached the terminal was "Python script executed with
# errors" with no hint of where. A filter that can hide the stack trace is worse
# than no filter.
RUN_LOG="$(mktemp -t trace-import-rocco)"
set +e
"$CMD_BIN" "${ARGS[@]}" 2>&1 \
    | grep -E '\[Trace\]|LogPythonScriptCommandlet|LogPython: Error|Traceback|  File "|LogInterchange.*(Error|Warning)' \
    | tee "$RUN_LOG"
set -e

trace_msg "Verifying what reached disk:"
MISSING=0

# The python side ends its report with a problem count. Checking only for files
# would call a run healthy that had, say, failed to rename the imported mesh and
# quietly kept the previous run's asset -- which is a thing that happened.
PROBLEMS="$(grep -E '\[Trace\] [0-9]+ PROBLEM\(S\)' "$RUN_LOG" || true)"
if [ -n "$PROBLEMS" ]; then
    trace_err "The import reported problems of its own:"
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

check "Content/Trace/Characters/Rocco/SK_Rocco.uasset"
check "Content/Trace/Characters/Rocco/SK_Rocco_Skeleton.uasset"
check "Content/Trace/Characters/Rocco/SK_Rocco_PhysicsAsset.uasset"
check "Content/Trace/Characters/Rocco/M_RoccoPlaceholder.uasset"

# One instance per material slot, each carrying that slot's own Roughness and
# Metallic as the FBX authored them. Ten is the finished model's slot count and
# is asserted rather than counted loosely: nine would mean a slot silently fell
# back to the flat placeholder, which is the state this step exists to end and
# which looks like success from every other angle.
INSTANCES="$(find "${TRACE_PROJECT_ROOT}/Content/Trace/Characters/Rocco" \
    -name 'MI_Rocco_*.uasset' 2>/dev/null | wc -l | tr -d ' ')"
if [ "$INSTANCES" = "10" ]; then
    printf '    ok      %s\n' "10 x Content/Trace/Characters/Rocco/MI_Rocco_*.uasset"
else
    printf '    MISSING %s\n' "expected 10 MI_Rocco_*.uasset per-slot instances, found ${INSTANCES}"
    MISSING=$((MISSING + 1))
fi

# Idempotence is a promise this script makes in its header, so it is checked
# rather than asserted: a second run that produced SK_Rocco_1 would leave the
# four files above present and everything looking fine.
STRAYS="$(find "${TRACE_PROJECT_ROOT}/Content/Trace/Characters/Rocco" -name '*.uasset' 2>/dev/null \
    | grep -vE '/(SK_Rocco|SK_Rocco_Skeleton|SK_Rocco_PhysicsAsset|M_RoccoPlaceholder|MI_Rocco_[A-Za-z0-9_]+)\.uasset$' || true)"
if [ -n "$STRAYS" ]; then
    trace_err "Assets landed that this script does not own -- the run was not idempotent:"
    printf '%s\n' "$STRAYS" | sed 's/^/    /'
    MISSING=$((MISSING + 1))
fi

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} problem(s). Search the output above for '[Trace]' lines, which name"
    trace_err "the API that failed. Re-run with --force to rebuild from scratch."
    exit 1
fi

trace_msg "Rocco is imported. The ROCCO IMPORT REPORT above carries the numbers the"
trace_msg "gameplay side needs: bounds in uu, the full bone list in component space,"
trace_msg "which way the model faces, and the same figures for the Mannequin beside it."
