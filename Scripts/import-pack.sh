#!/usr/bin/env bash
# ==============================================================================
# Trace — import-pack.sh
#
# Brings the artist's five-model pack into the project WITH its 31 baked
# animation clips. Unlike the last three kits, the motion is authored — none of
# it should be re-written in code.
#
#   Art/Pack/models/gloved_hands.glb      20 clips   (source of truth)
#   Art/Pack/models/butterfly_knife.glb    4 clips
#   Art/Pack/models/core.glb               3 clips
#   Art/Pack/models/railgun_pistol.glb     2 clips
#   Art/Pack/models/railgun_smg.glb        2 clips
#        |
#        |  Scripts/import_pack.py  (inside UnrealEditor-Cmd, via Interchange)
#        v
#   Content/Trace/Art/Pack/<Thing>/SK_Trace<Thing>            SkeletalMesh
#   Content/Trace/Art/Pack/<Thing>/SK_Trace<Thing>_Skeleton    Skeleton
#   Content/Trace/Art/Pack/<Thing>/Anims/A_<Thing>_<Clip>      AnimSequence
#   Content/Trace/Art/Pack/Materials/MI_Pack_*                 six instances of
#                                                              M_TraceRailgun
#
# NOTHING UNDER Content/Trace/Weapons IS TOUCHED. The shipped pistol and SMG
# static meshes stay exactly as they are; the pack's versions of those two
# weapons land beside them as SK_TracePistolPack / SK_TraceSmgPack. See the
# header of Scripts/import_pack.py for why.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY — the imported assets are on disk. Run it
# only after the pack under Art/Pack/ changes.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

FORCE=0
ONLY=""

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-pack

Imports Art/Pack/models/*.glb into Content/Trace/Art/Pack, animations included.

USAGE
  Scripts/import-pack.sh [options]

OPTIONS
      --only LIST   Comma-separated subset: hands,knife,core,pistol,smg
      --force       Delete and rebuild assets that already exist
  -h, --help        This text

AFTER RUNNING
  git status Content/Trace/Art/Pack

  Binary assets are LFS-tracked and cannot be merged, so lock them before
  editing if someone else might be doing the same: Scripts/lock.sh
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --only)   shift; [ $# -gt 0 ] || { trace_err "--only needs a value"; exit 2; }; ONLY="$1" ;;
        --only=*) ONLY="${1#--only=}" ;;
        --force)  FORCE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

trace_require_uproject
trace_resolve_engine
CMD_BIN="$(trace_editor_cmd_binary)"

MODEL_DIR="${TRACE_PROJECT_ROOT}/Art/Pack/models"
for Glb in gloved_hands butterfly_knife core railgun_pistol railgun_smg; do
    F="${MODEL_DIR}/${Glb}.glb"
    [ -f "$F" ] || trace_die "Source model missing: ${F}"
    # An LFS pointer is a ~130-byte text file starting 'version https://'. Handing
    # that to Interchange produces a baffling parse error instead of a useful one.
    if head -c 16 "$F" | grep -q '^version https'; then
        trace_die "${F} is an unfetched Git LFS pointer, not the model. Run: git lfs pull"
    fi
done

[ -f "${TRACE_PROJECT_ROOT}/Content/Trace/Weapons/Materials/M_TraceRailgun.uasset" ] \
    || trace_die "M_TraceRailgun is missing. The pack reuses the shipped master;
run Scripts/import-railgun.sh first."

export TRACE_FORCE_PACK="$FORCE"
export TRACE_PACK_ONLY="$ONLY"

# NOT -nullrhi. Creating material instances compiles shader maps, and that needs
# a real RHI on the first run -- the same trap documented in Scripts/bake-arena.sh.
ARGS=("$TRACE_UPROJECT"
      -run=pythonscript
      "-script=${TRACE_SCRIPT_DIR}/import_pack.py"
      -unattended
      -nosplash
      -nopause
      -nosound
      -stdout
      -FullStdOutLogOutput)

trace_msg "Importing the pack (TRACE_FORCE_PACK=${FORCE}, TRACE_PACK_ONLY=${ONLY:-all})"

# THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN. -run=pythonscript
# returns non-zero if ANY error was logged during the whole session, including
# engine warnings raised at startup. The authoritative check is what reached
# disk, below.
set +e
"$CMD_BIN" "${ARGS[@]}" 2>&1 | grep -E '\[Trace\]|LogPythonScriptCommandlet|LogInterchange.*(Error|Warning)' || true
set -e

trace_msg "Verifying what reached disk:"
MISSING=0
check() {
    if [ -f "${TRACE_PROJECT_ROOT}/$1" ]; then
        printf '    ok      %s\n' "$1"
    else
        printf '    MISSING %s\n' "$1"
        MISSING=$((MISSING + 1))
    fi
}

if [ -z "$ONLY" ] || [[ ",$ONLY," == *",hands,"* ]]; then
    check "Content/Trace/Art/Pack/Hands/SK_TraceHands.uasset"
    for C in Idle_Knife Idle_Pistol Idle_Smg Idle_Core Draw_Knife Stab_Knife \
             Inspect_Knife Shoot_Pistol Reload_Pistol Shoot_Smg Reload_Smg \
             Throw_Core Jump_Knife Jump_Pistol Jump_Smg Jump_Core \
             Walljump_Knife Walljump_Pistol Walljump_Smg Walljump_Core; do
        check "Content/Trace/Art/Pack/Hands/Anims/A_Hands_${C}.uasset"
    done
fi
if [ -z "$ONLY" ] || [[ ",$ONLY," == *",knife,"* ]]; then
    check "Content/Trace/Art/Pack/Knife/SK_TraceKnife.uasset"
    for C in Idle_Open Draw Stab Inspect; do
        check "Content/Trace/Art/Pack/Knife/Anims/A_Knife_${C}.uasset"
    done
fi
if [ -z "$ONLY" ] || [[ ",$ONLY," == *",core,"* ]]; then
    check "Content/Trace/Art/Pack/Core/SK_TraceCore.uasset"
    for C in Idle Pickup Throw; do
        check "Content/Trace/Art/Pack/Core/Anims/A_Core_${C}.uasset"
    done
fi
if [ -z "$ONLY" ] || [[ ",$ONLY," == *",pistol,"* ]]; then
    check "Content/Trace/Art/Pack/Pistol/SK_TracePistolPack.uasset"
    for C in Fire Reload; do
        check "Content/Trace/Art/Pack/Pistol/Anims/A_Pistol_${C}.uasset"
    done
fi
if [ -z "$ONLY" ] || [[ ",$ONLY," == *",smg,"* ]]; then
    check "Content/Trace/Art/Pack/Smg/SK_TraceSmgPack.uasset"
    for C in Fire Reload; do
        check "Content/Trace/Art/Pack/Smg/Anims/A_Smg_${C}.uasset"
    done
fi
for M in shell carbon seam plating circuit_cyan core_amber; do
    check "Content/Trace/Art/Pack/Materials/MI_Pack_${M}.uasset"
done

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} asset(s) did not land. Search the output above for '[Trace]'"
    trace_err "lines, which name the API that failed. Re-run with --force to rebuild."
    exit 1
fi

trace_msg "The pack is imported. The report above lists every path, clip length"
trace_msg "and material slot; that is the contract the other agents build on."
