#!/usr/bin/env bash
# ==============================================================================
# Trace — import-railgun.sh
#
# Rebuilds the railgun's game assets from the artist's source export.
#
#   Art/Railgun/railgun.glb          the model, as exported            (source of truth)
#   Art/Railgun/fire_curves.json     the emissive curve                (source of truth)
#        |
#        |  Scripts/railgun_glb_to_obj.py      (plain Python, no editor)
#        v
#   Intermediate/Railgun/SM_Railgun_{Body,RailL,RailR}.obj             (derived, gitignored)
#        |
#        |  Scripts/import_railgun.py          (inside UnrealEditor-Cmd)
#        v
#   Content/Trace/Weapons/Meshes/*      three StaticMeshes             (COMMITTED, via LFS)
#   Content/Trace/Weapons/Materials/*   one master + five instances    (COMMITTED, via LFS)
#
# The curve takes a separate path because it ends up in code rather than in an
# asset — Scripts/generate_railgun_curve.py bakes it into
# Source/Trace/Gameplay/TraceRailgunFireCurve.h, which this script also refreshes.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY. Unlike the Mannequin, this is our own art,
# so the finished assets ARE in the repository. Run it only after editing
# something under Art/Railgun, then commit what changes under Content/Trace/Weapons.
#
# WHY THE MODEL IS SPLIT IN THREE
#   The fire animation throws the two rail walls apart. One static mesh cannot do
#   that, so the converter separates the walls from the body and bakes each wall
#   around its own hinge. See the header of Scripts/import_railgun.py.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

FORCE=0
DRY_RUN=0

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-railgun

Rebuilds Content/Trace/Weapons from Art/Railgun/railgun.glb, and regenerates
Source/Trace/Gameplay/TraceRailgunFireCurve.h from Art/Railgun/fire_curves.json.

USAGE
  Scripts/import-railgun.sh [options]

OPTIONS
      --force         Rebuild assets that already exist (otherwise they are kept)
  -n, --dry-run       Run the conversion only; do not start the editor
  -h, --help          This text

AFTER RUNNING
  git status Content/Trace/Weapons Source/Trace/Gameplay/TraceRailgunFireCurve.h

  Binary assets are LFS-tracked and cannot be merged, so lock them before
  editing if someone else might be doing the same: Scripts/lock.sh
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --force)      FORCE=1 ;;
        -n|--dry-run) DRY_RUN=1 ;;
        -h|--help)    usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

trace_require_uproject

GLB="${TRACE_PROJECT_ROOT}/Art/Railgun/railgun.glb"
CURVE_JSON="${TRACE_PROJECT_ROOT}/Art/Railgun/fire_curves.json"

[ -f "$GLB" ] || trace_die "Source model missing: ${GLB}
It is LFS-tracked; on a fresh clone run 'git lfs pull' first."
[ -f "$CURVE_JSON" ] || trace_die "Source curve missing: ${CURVE_JSON}"

# An LFS pointer is a ~130-byte text file that starts with 'version https://'.
# Handing that to the converter produces a baffling parse error instead of a
# useful one, so name the real problem here.
if head -c 16 "$GLB" | grep -q '^version https'; then
    trace_die "${GLB} is an unfetched Git LFS pointer, not the model. Run: git lfs pull"
fi

# ------------------------------------------------------------------------------
# 1. The curve -> a generated header
# ------------------------------------------------------------------------------
trace_msg "Baking the emissive curve into TraceRailgunFireCurve.h"
python3 "${TRACE_SCRIPT_DIR}/generate_railgun_curve.py"

# ------------------------------------------------------------------------------
# 2. The model -> Unreal-space OBJ
# ------------------------------------------------------------------------------
trace_msg "Converting ${TRACE_C_BOLD}railgun.glb${TRACE_C_OFF} to Unreal-space OBJ"
python3 "${TRACE_SCRIPT_DIR}/railgun_glb_to_obj.py" > /dev/null

OBJ_DIR="${TRACE_PROJECT_ROOT}/Intermediate/Railgun"
for Name in Body RailL RailR; do
    [ -f "${OBJ_DIR}/SM_Railgun_${Name}.obj" ] \
        || trace_die "Converter did not produce SM_Railgun_${Name}.obj"
done
trace_msg "Wrote $(ls "${OBJ_DIR}"/*.obj | wc -l | tr -d ' ') OBJ files to Intermediate/Railgun"

if [ "$DRY_RUN" = "1" ]; then
    trace_msg "dry run -- the editor was not started."
    exit 0
fi

# ------------------------------------------------------------------------------
# 3. OBJ -> StaticMesh + materials, inside the editor
# ------------------------------------------------------------------------------
trace_resolve_engine
CMD_BIN="$(trace_editor_cmd_binary)"

export TRACE_FORCE_RAILGUN="$FORCE"

# NOT -nullrhi. Creating a UMaterial compiles its shader map, and that needs a
# real RHI on the first run -- the same trap documented at length in
# Scripts/bake-arena.sh.
ARGS=("$TRACE_UPROJECT"
      -run=pythonscript
      "-script=${TRACE_SCRIPT_DIR}/import_railgun.py"
      -unattended
      -nosplash
      -nopause
      -nosound
      -stdout
      -FullStdOutLogOutput)

trace_msg "Importing (TRACE_FORCE_RAILGUN=${FORCE})"

# THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN. UnrealEditor
# -run=pythonscript returns non-zero if ANY error was logged during the whole
# session, including engine warnings raised at startup that have nothing to do
# with this script. The authoritative check is what reached disk, below.
set +e
"$CMD_BIN" "${ARGS[@]}" 2>&1 | grep -E '\[Trace\]|LogPythonScriptCommandlet' || true
set -e

# ------------------------------------------------------------------------------
# 4. Verify what landed
# ------------------------------------------------------------------------------
EXPECTED=(
    "Content/Trace/Weapons/Meshes/SM_Railgun_Body.uasset"
    "Content/Trace/Weapons/Meshes/SM_Railgun_RailL.uasset"
    "Content/Trace/Weapons/Meshes/SM_Railgun_RailR.uasset"
    "Content/Trace/Weapons/Materials/M_TraceRailgun.uasset"
    "Content/Trace/Weapons/Materials/MI_Railgun_shell.uasset"
    "Content/Trace/Weapons/Materials/MI_Railgun_carbon.uasset"
    "Content/Trace/Weapons/Materials/MI_Railgun_plating.uasset"
    "Content/Trace/Weapons/Materials/MI_Railgun_circuit_cyan.uasset"
    "Content/Trace/Weapons/Materials/MI_Railgun_core_amber.uasset"
)

MISSING=0
trace_msg "Verifying:"
for Rel in "${EXPECTED[@]}"; do
    if [ -f "${TRACE_PROJECT_ROOT}/${Rel}" ]; then
        printf '    ok      %s\n' "$Rel"
    else
        printf '    MISSING %s\n' "$Rel"
        MISSING=$((MISSING + 1))
    fi
done

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} asset(s) did not land. Search the output above for '[Trace]' lines,"
    trace_err "which name the API that failed. Re-run with --force to rebuild from scratch."
    exit 1
fi

trace_msg "Railgun assets are up to date."
trace_msg "In game: Trace.Railgun.Probe reports the rig; -TraceNoRailgun forces the fallback gun."
