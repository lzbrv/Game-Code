#!/usr/bin/env bash
# ==============================================================================
# Trace — import-railgun.sh
#
# Rebuilds a railgun's game assets from the artist's source export. Two rigs go
# through the same pipeline; choose with --rig (default: railgun, the pistol).
#
#   Art/Railgun/railgun.glb          the pistol model                  (source of truth)
#   Art/Railgun/fire_curves.json     the emissive curve                (source of truth)
#   Art/Smg/railgun_smg.glb          the SMG model                     (source of truth)
#        |
#        |  Scripts/railgun_glb_to_obj.py [rig]     (plain Python, no editor)
#        v
#   Intermediate/Railgun/SM_Railgun_{Body,RailL,RailR}.obj             (derived, gitignored)
#   Intermediate/Smg/SM_RailgunSmg_{Body,WallLeft,WallRight,Mag}.obj   (derived, gitignored)
#        |
#        |  Scripts/import_railgun.py               (inside UnrealEditor-Cmd)
#        v
#   Content/Trace/Weapons/Meshes/*      three + four StaticMeshes      (COMMITTED, via LFS)
#   Content/Trace/Weapons/Materials/*   one master + two x five MIs    (COMMITTED, via LFS)
#
# The pistol's curve takes a separate path because it ends up in code rather than
# in an asset — Scripts/generate_railgun_curve.py bakes it into
# Source/Trace/Gameplay/TraceRailgunFireCurve.h, which this script also refreshes.
# The SMG export ships NO animation clips (`animations: []`), so it has no curve
# to bake: its motion is reproduced in code from the numbers in the spec.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY. Unlike the Mannequin, this is our own art,
# so the finished assets ARE in the repository. Run it only after editing
# something under Art/, then commit what changes under Content/Trace/Weapons.
#
# WHY EACH MODEL IS SPLIT
#   Parts of the weapon move relative to the rest, and one static mesh cannot do
#   that. The pistol's two rail walls are baked around their own hinges; the
#   SMG's export authors real pivot nodes, so its split is read off the hierarchy
#   and every group attaches at (0,0,0). See Scripts/import_railgun.py's header.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

FORCE=0
DRY_RUN=0
RIG=railgun

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-railgun

Rebuilds Content/Trace/Weapons from the models under Art/, and (for the pistol)
regenerates Source/Trace/Gameplay/TraceRailgunFireCurve.h from
Art/Railgun/fire_curves.json.

USAGE
  Scripts/import-railgun.sh [options]

OPTIONS
      --rig NAME      railgun (the pistol, default), smg, or all
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
        --rig)        shift; [ $# -gt 0 ] || { trace_err "--rig needs a value"; exit 2; }; RIG="$1" ;;
        --rig=*)      RIG="${1#--rig=}" ;;
        --force)      FORCE=1 ;;
        -n|--dry-run) DRY_RUN=1 ;;
        -h|--help)    usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

case "$RIG" in
    railgun|smg|all) ;;
    *) trace_err "Unknown rig: ${RIG} (want railgun, smg or all)"; exit 2 ;;
esac

trace_require_uproject

CMD_BIN=""
if [ "$DRY_RUN" != "1" ]; then
    trace_resolve_engine
    CMD_BIN="$(trace_editor_cmd_binary)"
fi

# ------------------------------------------------------------------------------
# One rig, end to end.
#   $1 rig name   $2 GLB   $3 OBJ dir   $4 OBJ prefix   $5.. group names
# The expected asset list is derived from those same group names, so adding a
# moving part to a model does not mean editing a second list down here.
# ------------------------------------------------------------------------------
run_rig() {
    local Rig="$1" Glb="$2" ObjDir="$3" Prefix="$4"
    shift 4
    local Groups=("$@")

    trace_msg "=== rig ${TRACE_C_BOLD}${Rig}${TRACE_C_OFF} ==="

    [ -f "$Glb" ] || trace_die "Source model missing: ${Glb}
It is LFS-tracked; on a fresh clone run 'git lfs pull' first."

    # An LFS pointer is a ~130-byte text file that starts with 'version https://'.
    # Handing that to the converter produces a baffling parse error instead of a
    # useful one, so name the real problem here.
    if head -c 16 "$Glb" | grep -q '^version https'; then
        trace_die "${Glb} is an unfetched Git LFS pointer, not the model. Run: git lfs pull"
    fi

    # -- the pistol's curve -> a generated header ------------------------------
    if [ "$Rig" = "railgun" ]; then
        local CurveJson="${TRACE_PROJECT_ROOT}/Art/Railgun/fire_curves.json"
        [ -f "$CurveJson" ] || trace_die "Source curve missing: ${CurveJson}"
        trace_msg "Baking the emissive curve into TraceRailgunFireCurve.h"
        python3 "${TRACE_SCRIPT_DIR}/generate_railgun_curve.py"
    fi

    # -- the model -> Unreal-space OBJ -----------------------------------------
    trace_msg "Converting ${TRACE_C_BOLD}$(basename "$Glb")${TRACE_C_OFF} to Unreal-space OBJ"
    python3 "${TRACE_SCRIPT_DIR}/railgun_glb_to_obj.py" "$Rig" > /dev/null

    local Name
    for Name in "${Groups[@]}"; do
        [ -f "${ObjDir}/${Prefix}${Name}.obj" ] \
            || trace_die "Converter did not produce ${Prefix}${Name}.obj"
    done
    trace_msg "Wrote $(ls "${ObjDir}"/*.obj | wc -l | tr -d ' ') OBJ files to ${ObjDir#"${TRACE_PROJECT_ROOT}/"}"

    if [ "$DRY_RUN" = "1" ]; then
        trace_msg "dry run -- the editor was not started."
        return 0
    fi

    # -- OBJ -> StaticMesh + materials, inside the editor ----------------------
    export TRACE_FORCE_RAILGUN="$FORCE"
    export TRACE_RIG="$Rig"

    # NOT -nullrhi. Creating a UMaterial compiles its shader map, and that needs a
    # real RHI on the first run -- the same trap documented at length in
    # Scripts/bake-arena.sh.
    local ARGS=("$TRACE_UPROJECT"
                -run=pythonscript
                "-script=${TRACE_SCRIPT_DIR}/import_railgun.py"
                -unattended
                -nosplash
                -nopause
                -nosound
                -stdout
                -FullStdOutLogOutput)

    trace_msg "Importing (TRACE_RIG=${Rig}, TRACE_FORCE_RAILGUN=${FORCE})"

    # THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN. UnrealEditor
    # -run=pythonscript returns non-zero if ANY error was logged during the whole
    # session, including engine warnings raised at startup that have nothing to do
    # with this script. The authoritative check is what reached disk, below.
    set +e
    "$CMD_BIN" "${ARGS[@]}" 2>&1 | grep -E '\[Trace\]|LogPythonScriptCommandlet' || true
    set -e

    # -- verify what landed ----------------------------------------------------
    local MiPrefix="MI_Railgun_"
    [ "$Rig" = "smg" ] && MiPrefix="MI_RailgunSmg_"

    local EXPECTED=("Content/Trace/Weapons/Materials/M_TraceRailgun.uasset")
    for Name in "${Groups[@]}"; do
        EXPECTED+=("Content/Trace/Weapons/Meshes/${Prefix}${Name}.uasset")
    done
    local Mat
    for Mat in shell carbon plating circuit_cyan core_amber; do
        EXPECTED+=("Content/Trace/Weapons/Materials/${MiPrefix}${Mat}.uasset")
    done

    local MISSING=0 Rel
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
        return 1
    fi

    trace_msg "${Rig} assets are up to date."
    return 0
}

RC=0
if [ "$RIG" = "railgun" ] || [ "$RIG" = "all" ]; then
    run_rig railgun \
        "${TRACE_PROJECT_ROOT}/Art/Railgun/railgun.glb" \
        "${TRACE_PROJECT_ROOT}/Intermediate/Railgun" \
        "SM_Railgun_" Body RailL RailR || RC=1
fi
if [ "$RIG" = "smg" ] || [ "$RIG" = "all" ]; then
    run_rig smg \
        "${TRACE_PROJECT_ROOT}/Art/Smg/railgun_smg.glb" \
        "${TRACE_PROJECT_ROOT}/Intermediate/Smg" \
        "SM_RailgunSmg_" Body Mag WallLeft WallRight || RC=1
fi

[ "$RC" = "0" ] || exit 1

trace_msg "In game: Trace.Railgun.Probe reports the pistol rig; -TraceNoRailgun forces the fallback gun."
