#!/usr/bin/env bash
# ==============================================================================
# Trace — import-characters.sh
#
# Drives the generated-character pipeline (PIPELINE_DESIGN.md §10), one stage
# per editor invocation:
#
#   stage       runs                                   output
#   generate    python3 generate_characters.py         Intermediate/Characters/*.glb + manifests
#   materials   editor  generate_body_materials.py     Content/Trace/Characters/Shared/Materials (26 assets)
#   import      editor  import_characters.py           10x SK_<Name> (+physics) + SK_TraceBody_Skeleton
#   retarget    editor  retarget_body.py               Shared/Retarget + Shared/Anims        [lands W3-CHARPIPE]
#   portraits   game CaptureAll + compose/import_portraits.py   10x T_Portrait_<Name>
#   census      game runs per PIPELINE §9.3            Saved/Logs verdicts + screenshots     [lands W4-CENSUS]
#
# Default is the stages that exist so far, in order: generate materials import.
# `retarget` and `portraits` have since landed and run for real; `census` is still
# a guarded stub that dies with its owning tranche's name. Note what the default
# does NOT include: portraits is opt-in via --stage, because re-running it re-shoots
# ten frames through the game and there is never a reason to do that as a side
# effect of asking for something else. Manual step order for the full pipeline
# (PIPELINE §10): generate -> materials -> import -> retarget; then the C++
# edits (§4.4, §8.2, §9.1, §9.3) + Scripts/build.sh + DA regen; then
# portraits -> census.
#
# EVERY STAGE'S SHELL PRE-WIPE HAPPENS OUT HERE, BEFORE THE EDITOR STARTS.
# EditorAssetLibrary.delete_asset removes the file but the Asset Registry
# keeps the entry for the rest of the session, so renaming a fresh import onto
# a deleted name fails and the run silently re-measures LAST run's asset. The
# full story is in Scripts/import-rocco.sh (the wipe block) and
# import_rocco.py sweep() — do not relitigate it.
#
# Shared/Materials survives the import stage's wipe and normal materials runs:
# the 26 material assets keep their packages (same paths, same GUIDs, no
# redirectors) and have their parameters rewritten in place every run, so the
# import stage never has to re-point anything. Their BYTES do change on every
# materials run (◆MEASURED: parents recompile, instances cache the new parent
# state) — a materials re-run leaves 26 dirty binaries even though every value
# is identical. --force additionally wipes the files first, for a GUID-fresh
# rebuild (the import-rocco.sh --force rule).
#
# NOT -nullrhi ANYWHERE AN ASSET COMPILES: the materials stage compiles three
# master materials and a null RHI leaves them broken on disk (the trap
# documented in Scripts/import-rocco.sh and Scripts/bake-arena.sh).
#
# Each editor/game launch is wrapped in a hard alarm (a hung commandlet
# otherwise outlives the session) and writes its own log under
# Saved/Logs/import-characters/. Keep `caffeinate -dimsu` running for long
# multi-stage runs — sleep kills in-flight editors.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

FORCE=0
DRY_RUN=0
STAGES=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-characters

Drives the generated-character pipeline, stage by stage (PIPELINE_DESIGN.md §10).

USAGE
  Scripts/import-characters.sh [options]

OPTIONS
      --stage <name>  Run one stage: generate | materials | import | retarget |
                      portraits | census. Repeatable; order is fixed regardless
                      of flag order. Default: generate materials import (the
                      stages that exist as of wave 2).
      --dry-run       import stage only: validate GLBs/manifests/material assets
                      from disk with plain python3 — no editor, writes nothing.
      --force         materials stage: wipe Content/Trace/Characters/Shared/
                      Materials/*.uasset before the editor starts (GUID-fresh
                      rebuild). Without it the 26 assets are kept and their
                      parameters are rewritten in place (the import-rocco.sh
                      MI-churn rule).
  -h, --help          This text

VERDICTS
  Grep lines, not exit codes (the pythonscript commandlet exits nonzero on
  healthy runs — generate-data-assets.py:38-43):
    generate   [generate-characters] EXIT=0
    materials  [generate-body-materials] EXIT=0  + 26 assets on disk
    import     [import-characters] EXIT=0        + disk census below
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --stage)   shift; [ $# -gt 0 ] || trace_die "--stage needs a name"; STAGES+=("$1") ;;
        --force)   FORCE=1 ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help) usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

[ ${#STAGES[@]} -gt 0 ] || STAGES=(generate materials import)

for s in "${STAGES[@]}"; do
    case "$s" in
        generate|materials|import|retarget|portraits|census) ;;
        *) trace_die "Unknown stage: ${s}" ;;
    esac
done

trace_require_uproject

CHAR_CONTENT="${TRACE_PROJECT_ROOT}/Content/Trace/Characters"
MATERIALS_DIR="${CHAR_CONTENT}/Shared/Materials"
INTERMEDIATE="${TRACE_PROJECT_ROOT}/Intermediate/Characters"
LOGDIR="${TRACE_PROJECT_ROOT}/Saved/Logs/import-characters"
# Stage `portraits` (PIPELINE §8): the raw 1024² frames the in-game rig writes, and
# the composited 512² sources that are COMMITTED (the menu art set's convention —
# Content/Trace/UI/Art/Source, Scripts/slice-ui-assets.py:7-10). Saved/ is gitignored,
# so only the second of these survives a clone; both are regenerated by this stage.
PORTRAIT_RAW="${TRACE_PROJECT_ROOT}/Saved/Portraits"
PORTRAIT_SRC="${TRACE_PROJECT_ROOT}/Content/Trace/UI/Art/Source/Portraits"
mkdir -p "$LOGDIR"

# The ten characters (TraceCharacterRoster.h ids 1-10) and the 26 material
# assets (PIPELINE §4.3, integrator conflict #2: ten per-character suit MIs +
# ten accent MIs + three shared MIs + three parents — NOT the pre-integration
# 17 some PIPELINE tables still carry).
CHARACTERS=(Rocco Chut Mace Oyster X Roxie Elle Slimeball Mortimer Lily)
MATERIAL_ASSETS=(M_TraceBodySuit M_TraceBodyGlow M_TraceBodyAccent
                 MI_Body_SuitHead MI_Body_Inset MI_Body_Glow)
for c in "${CHARACTERS[@]}"; do
    MATERIAL_ASSETS+=("MI_Body_${c}_Suit" "MI_Body_${c}_Accent")
done

MISSING=0

check_file() {
    if [ -f "${TRACE_PROJECT_ROOT}/$1" ]; then
        printf '    ok      %s\n' "$1"
    else
        printf '    MISSING %s\n' "$1"
        MISSING=$((MISSING + 1))
    fi
}

# run_editor <stage> <script.py> <alarm-seconds>
#   Editor invocation per PIPELINE §10: real RHI (no -nullrhi), off-screen,
#   full stdout, own abslog, hard alarm. THE EXIT CODE IS NOT THE VERDICT —
#   callers grep the tee'd RUN_LOG for the script's EXIT= line.
run_editor() {
    local stage="$1" script="$2" alarm="$3"
    trace_resolve_engine
    local cmd_bin
    cmd_bin="$(trace_editor_cmd_binary)"
    RUN_LOG="${LOGDIR}/${stage}.filtered.log"
    set +e
    perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' "$alarm" \
        "$cmd_bin" "$TRACE_UPROJECT" \
        -run=pythonscript "-script=${TRACE_SCRIPT_DIR}/${script}" \
        -unattended -nosplash -nopause -nosound -stdout -FullStdOutLogOutput \
        -RenderOffScreen "-abslog=${LOGDIR}/${stage}.log" 2>&1 \
        | grep -E '\[Trace\]|LogPythonScriptCommandlet|LogPython: Error|Traceback|  File "|LogInterchange.*(Error|Warning)' \
        | tee "$RUN_LOG"
    set -e
}

# require_editor_module
#   The editor cannot open this project without the compiled game module: it
#   pops "The game module 'Trace' could not be found" and exits before the
#   python script runs. Scripts/build.sh DELETES that dylib while it relinks, so
#   a sibling build in flight (or a failed one) leaves a window where every
#   editor stage here dies at start-up — and a stage that pre-wipes first would
#   have destroyed its assets for nothing. ◆MEASURED 2026-08-24: --force wiped
#   the 26 materials, the editor then refused to start mid-sibling-build, and
#   the folder stayed empty until a green build came back. So: check BEFORE
#   wiping anything.
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

# grep_verdict <verdict-regex> <what>
grep_verdict() {
    if grep -qE "$1" "$RUN_LOG"; then
        printf '    ok      %s\n' "$2"
    else
        trace_err "verdict line '$1' not found — $2 FAILED. Full log: see ${LOGDIR}/"
        MISSING=$((MISSING + 1))
    fi
    if grep -qE '\[Trace\] [0-9]+ PROBLEM\(S\)' "$RUN_LOG"; then
        trace_err "the run reported problems of its own:"
        grep -A30 'PROBLEM(S)' "$RUN_LOG" | sed 's/^/    /'
        MISSING=$((MISSING + 1))
    fi
}

stage_generate() {
    trace_msg "STAGE generate: GLBs + manifests -> Intermediate/Characters/"
    rm -rf "$INTERMEDIATE"
    RUN_LOG="${LOGDIR}/generate.log"
    set +e
    perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!"' 60 \
        python3 "${TRACE_SCRIPT_DIR}/generate_characters.py" 2>&1 | tee "$RUN_LOG"
    set -e
    grep_verdict '\[generate-characters\] EXIT=0' "generate_characters.py verdict"
}

stage_materials() {
    trace_msg "STAGE materials: 3 parents + 23 MIs -> Content/Trace/Characters/Shared/Materials/"
    trace_resolve_engine
    require_editor_module
    if [ "$FORCE" = "1" ] && [ -d "$MATERIALS_DIR" ]; then
        find "$MATERIALS_DIR" -name '*.uasset' -delete
        trace_msg "wiped Shared/Materials (--force: GUID-fresh rebuild)"
    fi
    run_editor materials generate_body_materials.py 480
    grep_verdict '\[generate-body-materials\] EXIT=0' "generate_body_materials.py verdict"
    local n
    n="$(find "$MATERIALS_DIR" -name '*.uasset' 2>/dev/null | wc -l | tr -d ' ')"
    if [ "$n" = "26" ]; then
        printf '    ok      26 material assets on disk\n'
    else
        trace_err "expected 26 material assets in Shared/Materials, found ${n}"
        MISSING=$((MISSING + 1))
    fi
    for a in "${MATERIAL_ASSETS[@]}"; do
        [ -f "${MATERIALS_DIR}/${a}.uasset" ] || { trace_err "missing ${a}.uasset"; MISSING=$((MISSING + 1)); }
    done
}

stage_import() {
    if [ "$DRY_RUN" = "1" ]; then
        trace_msg "STAGE import (DRY RUN): disk-side contract check, no editor"
        set +e
        python3 "${TRACE_SCRIPT_DIR}/import_characters.py" --dry-run
        local rc=$?
        set -e
        [ "$rc" = "0" ] || MISSING=$((MISSING + 1))
        return
    fi
    trace_msg "STAGE import: ten GLBs -> SK_<Name> + SK_TraceBody_Skeleton"
    trace_resolve_engine
    require_editor_module
    # Pre-wipe: EVERYTHING under Characters/ except Shared/Materials (§10).
    if [ -d "$CHAR_CONTENT" ]; then
        find "$CHAR_CONTENT" -name '*.uasset' ! -path "${MATERIALS_DIR}/*" -delete
        find "$CHAR_CONTENT" -mindepth 1 -type d -empty -delete
        trace_msg "wiped Characters/ (kept Shared/Materials)"
    fi
    run_editor import import_characters.py 900
    grep_verdict '\[import-characters\] EXIT=0' "import_characters.py verdict"

    trace_msg "Verifying what reached disk:"
    for c in "${CHARACTERS[@]}"; do
        check_file "Content/Trace/Characters/${c}/SK_${c}.uasset"
        check_file "Content/Trace/Characters/${c}/SK_${c}_PhysicsAsset.uasset"
    done
    check_file "Content/Trace/Characters/Shared/SK_TraceBody_Skeleton.uasset"
    for a in "${MATERIAL_ASSETS[@]}"; do
        [ -f "${MATERIALS_DIR}/${a}.uasset" ] || { trace_err "missing ${a}.uasset"; MISSING=$((MISSING + 1)); }
    done
    # Idempotence: nothing this stage does not own (the import-rocco.sh
    # strays check, replicated per folder).
    local strays
    strays="$(find "$CHAR_CONTENT" -name '*.uasset' 2>/dev/null \
        | grep -vE "/(SK_[A-Za-z]+|SK_[A-Za-z]+_PhysicsAsset|SK_TraceBody_Skeleton|M_TraceBody[A-Za-z]+|MI_Body_[A-Za-z_]+)\.uasset$" || true)"
    if [ -n "$strays" ]; then
        trace_err "assets landed that this stage does not own — the run was not idempotent:"
        printf '%s\n' "$strays" | sed 's/^/    /'
        MISSING=$((MISSING + 1))
    fi
}

stage_retarget() {
    if [ ! -f "${TRACE_SCRIPT_DIR}/retarget_body.py" ]; then
        trace_die "retarget_body.py not present yet — it lands with W3-CHARPIPE (PIPELINE §7). Nothing wiped."
    fi
    trace_msg "STAGE retarget: IK rigs + ABP_Unarmed_Body -> Shared/{Retarget,Anims}"
    trace_resolve_engine
    require_editor_module
    for d in "${CHAR_CONTENT}/Shared/Retarget" "${CHAR_CONTENT}/Shared/Anims"; do
        [ -d "$d" ] && find "$d" -name '*.uasset' -delete
    done
    run_editor retarget retarget_body.py 600
    grep_verdict '\[retarget-body\] EXIT=0' "retarget_body.py verdict"
}

# THE PORTRAIT CAPTURE IS A GAME RUN, NOT AN EDITOR RUN, AND THAT IS MEASURED.
# PIPELINE §8.1: editor-python SceneCapture renders NOTHING in a commandlet and both
# export APIs are dead there. The shipped -game screenshot path demonstrably writes
# real files headlessly — it produced the entire release visual audit — so the
# portraits ride it. -ResX/-ResY set the viewport, and the viewport IS the screenshot
# size, which is why the raws come out 1024² (a 2x supersample of the final 512).
#
# The framing/lighting numbers are NOT passed here: they are the frozen defaults in
# Source/Trace/Debug/TracePortraitRig.cpp, tuned once against ART_BIBLE §7.5's 38%
# head-height gate and then written back into the file so this stage reproduces THAT
# shoot. Trace.Portrait.* remain live as -dpcvars for anyone re-judging them.
run_portrait_capture() {
    trace_resolve_engine
    local game_bin
    game_bin="$(trace_editor_binary)"
    local log="${LOGDIR}/portraits-capture.log"
    rm -f "$log"
    set +e
    perl -e 'alarm 240; exec @ARGV' \
        "$game_bin" "$TRACE_UPROJECT" /Game/Maps/MainMenu \
        -game -RenderOffScreen -nosplash -nosound -unattended -NoLoadingScreen \
        -ResX=1024 -ResY=1024 -WINDOWED \
        "-TraceExec=Trace.Portrait.CaptureAll ${PORTRAIT_RAW}" \
        -TraceExecOn=Menu -TraceExecAt=8 \
        "-abslog=${log}" >/dev/null 2>&1
    set -e
    # Mac -game respawns DETACHED, so the alarm above does not bind the real process:
    # kill by log path, never by pid (the release-wave rule, learned the hard way).
    sleep 2; pkill -f "portraits-capture.log" 2>/dev/null || true
    sleep 2; pkill -9 -f "portraits-capture.log" 2>/dev/null || true
    RUN_LOG="$log"
}

stage_portraits() {
    if [ ! -f "${TRACE_SCRIPT_DIR}/compose_portraits.py" ] || [ ! -f "${TRACE_SCRIPT_DIR}/import_portraits.py" ]; then
        trace_die "compose_portraits.py / import_portraits.py not present yet — they land with W4-PORTRAITS (PIPELINE §8). Nothing wiped."
    fi
    require_editor_module
    trace_msg "STAGE portraits: CaptureAll -> compose -> import (10x T_Portrait_<Name>)"

    # Pre-wipe per PIPELINE §10: the raw frames and the composited committed sources.
    # The /Game textures are deliberately NOT wiped — import_portraits.py replaces them
    # in place (replace_existing=True), which keeps their package GUIDs and leaves no
    # redirector for the select screen to chase. (Deleting a .uasset and re-creating it
    # in one session is the Asset Registry ghost this whole wrapper pre-wipes to avoid;
    # here there is nothing to gain from it.)
    mkdir -p "$PORTRAIT_RAW" "$PORTRAIT_SRC"
    rm -f "${PORTRAIT_RAW}"/raw_*.png "${PORTRAIT_SRC}"/T_Portrait_*.png

    run_portrait_capture
    grep_verdict '\[Portrait\] DONE 10/10' "Trace.Portrait.CaptureAll"

    trace_msg "  composite -> ${PORTRAIT_SRC}"
    RUN_LOG="${LOGDIR}/portraits-compose.log"
    set +e
    python3 "${TRACE_SCRIPT_DIR}/compose_portraits.py" \
        --raw-dir "$PORTRAIT_RAW" --out-dir "$PORTRAIT_SRC" \
        --log "${LOGDIR}/portraits-capture.log" \
        --contact-sheet "${LOGDIR}/portraits-contact-sheet.png" \
        --json "${LOGDIR}/portraits-verdicts.json" 2>&1 | tee "$RUN_LOG"
    set -e
    grep_verdict '\[compose-portraits\] EXIT=0' "compose_portraits.py verdicts"

    # .uasset is `lockable` in .gitattributes, so a texture imported by an EARLIER
    # run is read-only on disk and replace_existing=True would fail on a permission
    # error rather than on anything to do with the image. W3-CHARWIRE hit exactly
    # this on the data-asset regen. Making our own outputs writable is not the same
    # as unlocking them in git — Scripts/lock.sh still owns that.
    chmod u+w "${TRACE_PROJECT_ROOT}"/Content/Trace/UI/Art/Portraits/T_Portrait_*.uasset 2>/dev/null || true

    run_editor portraits import_portraits.py 300
    grep_verdict '\[import-portraits\] EXIT=0' "import_portraits.py readback"

    trace_msg "Verifying:"
    for c in "${CHARACTERS[@]}"; do
        check_file "Content/Trace/UI/Art/Portraits/T_Portrait_${c}.uasset"
    done
}

stage_census() {
    trace_die "census stage lands with W4-CENSUS (PIPELINE §9.3; needs W3-CHARWIRE's roster rows and -TraceExec2)."
}

for s in generate materials import retarget portraits census; do
    for want in "${STAGES[@]}"; do
        [ "$s" = "$want" ] || continue
        "stage_${s}"
    done
done

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} problem(s). Logs: ${LOGDIR}/"
    exit 1
fi
trace_msg "All requested stages passed: ${STAGES[*]}"
