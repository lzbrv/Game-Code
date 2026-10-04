#!/usr/bin/env bash
# ==============================================================================
# Trace — dump-stats.sh
#
# Regenerates docs/TraceStats.csv: every stat in the game, read out of the
# running game by Trace.DumpStats (Source/Trace/Debug/TraceStatsDump.cpp).
#
# RUN IT FROM A TERMINAL, IN THE REPO:
#
#     Scripts/dump-stats.sh                       writes docs/TraceStats.csv
#     Scripts/dump-stats.sh ~/Desktop/stats.csv   writes there instead
#     Scripts/dump-stats.sh --build               build the editor first, then dump
#
# It runs this (and prints it first, with your engine path filled in):
#
#   "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor" "<repo>/Trace.uproject" \
#       "/Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode" \
#       -game -log -nullrhi -RenderOffScreen -unattended -nosound -nosplash \
#       -TraceExec="Trace.DumpStats docs/TraceStats.csv|Trace.VerifyStats docs/TraceStats.csv|quit" \
#       -TraceExecAt=6 -TraceExecOn=Match -abslog="<repo>/Saved/Logs/dump-stats.log"
#
# That is the practice range, headless, with no window. Six seconds into the
# match it writes the sheet, re-reads and parses it, and quits. Under a minute.
#
# DEV / EDITOR BUILDS ONLY. It runs your EDITOR build of Trace (what
# Scripts/build.sh makes), not a packaged game: Trace.DumpStats is compiled out
# of Shipping. "Trace.DumpStats" typed at a shell prompt does nothing - it is a
# console command inside the game, and this script is how a terminal reaches it.
#
# It stops, with the reason, when:
#   * no Unreal Engine is found         (UE_ROOT, .ue-root, the launcher folders)
#   * there is no editor build          (Scripts/build.sh, or pass --build)
#   * the baked arena is missing, or Content/ still holds Git LFS pointer files
#                                       (git lfs pull)
#   * the run does not end with both verdicts PASS (log: Saved/Logs/dump-stats.log)
# and it warns when the editor build is older than Source/, because the sheet
# shows the numbers of the build that RAN, not of the tree.
#
# Windows twin: Scripts\dump-stats.bat.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

MAP="$TRACE_DEFAULT_MAP"
OUT=""
DO_BUILD=0
BUDGET=300

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} stat sheet: every stat in the game, as a CSV

USAGE
  Scripts/dump-stats.sh [options] [<output.csv>]

  <output.csv>     Where to write it. Default: docs/TraceStats.csv (the committed
                   copy). A relative path is relative to where you run this.

OPTIONS
  -b, --build      Build the editor target first (Scripts/build.sh), so the sheet
                   has the numbers of the code you have checked out
      --budget <s> Give up and stop the game after this many seconds. Default: ${BUDGET}
  -n, --dry-run    Print the command; run nothing
  -h, --help       This text

NOTES
  * Run it from a terminal, not from inside the game.
  * Dev/editor builds only: it runs the editor build of Trace with -game.
    A packaged Shipping build has no Trace.DumpStats at all.
  * Inside a running dev build, the console (\`) takes the command directly:
    Trace.DumpStats [<path>], then Trace.VerifyStats [<path>]. On its own it
    writes Saved/Stats/TraceStats.csv.
  * Windows: Scripts\\dump-stats.bat, same arguments.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--build)   DO_BUILD=1; shift ;;
        --budget)     [ $# -ge 2 ] || trace_die "--budget needs a value"; BUDGET="$2"; shift 2 ;;
        -n|--dry-run) TRACE_DRY_RUN=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        -*)           trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)
            if [ -n "$OUT" ]; then
                trace_err "One output path only; got '${OUT}' and '$1'."; echo; usage; exit 2
            fi
            OUT="$1"; shift ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"
case "$BUDGET" in
    ''|*[!0-9]*) trace_die "--budget takes whole seconds, got '${BUDGET}'." ;;
esac

# ------------------------------------------------------------------------------
# Where the sheet goes
#
# A relative path means relative to where you ran this, as with any other
# command. (Unreal would read it against the project folder instead, so it is
# made absolute here.) A path inside the repo is then handed to the game
# repo-relative, which keeps the sheet's own "Written to" row free of whose
# machine ran it, and keeps spaces in the clone's location out of -TraceExec.
# ------------------------------------------------------------------------------
[ -n "$OUT" ] || OUT="${TRACE_PROJECT_ROOT}/docs/TraceStats.csv"
case "$OUT" in
    /*) : ;;
    *)  OUT="$(pwd -P)/${OUT}" ;;
esac
OUT_DIR="$(dirname -- "$OUT")"
mkdir -p -- "$OUT_DIR" || trace_die "Cannot create the folder for the sheet: ${OUT_DIR}"
OUT="$(cd -- "$OUT_DIR" && pwd -P)/$(basename -- "$OUT")"
[ -d "$OUT" ] && trace_die "${OUT} is a folder. Give a file name, e.g. ${OUT}/TraceStats.csv"

case "$OUT" in
    "${TRACE_PROJECT_ROOT}/"*) GAME_PATH="${OUT#"${TRACE_PROJECT_ROOT}/"}" ;;
    *)                         GAME_PATH="$OUT" ;;
esac
# -TraceExec="a|b|c" is split on | and ends at the next double quote.
case "$GAME_PATH" in
    *'|'*|*'"'*) trace_die "The output path cannot contain | or a double quote: ${OUT}" ;;
esac

# ------------------------------------------------------------------------------
# Pre-flight. Each of these otherwise fails INSIDE the game, as a headless run
# that never reaches the dump and leaves nothing on screen to explain why.
# ------------------------------------------------------------------------------
trace_require_uproject
trace_resolve_engine

if [ "$DO_BUILD" = "1" ]; then
    trace_msg "Building the editor target first (--build)"
    "${TRACE_SCRIPT_DIR}/build.sh"
fi

UE_EDITOR_BIN="$(trace_editor_binary)"

# --- the map, and Git LFS ------------------------------------------------------
# The baked arena is a small .umap plus one .uasset per actor, and every one of
# them is in Git LFS. A clone made without git-lfs, or not yet pulled, has
# ~130-byte text POINTER files in their place; the engine reads those as broken
# packages, the map never loads as a match, and the dump never runs.
MAP_REL="${MAP#/Game/}"
MAP_FILE="${TRACE_PROJECT_ROOT}/Content/${MAP_REL}.umap"
if [ ! -f "$MAP_FILE" ]; then
    trace_err "The map is missing: Content/${MAP_REL}.umap"
    trace_err "It is committed (in Git LFS), so this clone is incomplete. From the repo:"
    trace_err "    git lfs install && git lfs pull        (first time: Scripts/setup-lfs.sh)"
    exit 1
fi
LFS_POINTERS="$(find "${TRACE_PROJECT_ROOT}/Content" -type f \( -name '*.uasset' -o -name '*.umap' \) \
    -size -400c -exec grep -l 'git-lfs.github.com/spec' {} + 2>/dev/null || true)"
if [ -n "$LFS_POINTERS" ]; then
    LFS_COUNT="$(printf '%s\n' "$LFS_POINTERS" | grep -c . || true)"
    trace_err "${LFS_COUNT} asset(s) under Content/ are Git LFS POINTER files, not the real assets. For example:"
    printf '%s\n' "$LFS_POINTERS" | head -3 | sed -e "s|^${TRACE_PROJECT_ROOT}/|    |" >&2 || true
    trace_err "The game cannot load the arena from those. Fetch the real files, from the repo:"
    trace_err "    git lfs install && git lfs pull        (first time: Scripts/setup-lfs.sh)"
    exit 1
fi

# --- the editor build ------------------------------------------------------------
# UnrealEditor.modules names the library the engine will load (the same file
# Scripts/build.sh checks for staleness), so that is the one to look for.
BIN_DIR="${TRACE_PROJECT_ROOT}/Binaries/${TRACE_HOST_PLATFORM}"
MODULES_FILE="${BIN_DIR}/UnrealEditor.modules"
MODULE_LIB=""
if [ -f "$MODULES_FILE" ]; then
    MODULE_NAME="$(tr ',' '\n' < "$MODULES_FILE" \
        | grep -Eo "(lib)?UnrealEditor-${TRACE_PROJECT_NAME}(-[0-9]+)?\.(dylib|so)" | head -1 || true)"
    if [ -n "$MODULE_NAME" ] && [ -f "${BIN_DIR}/${MODULE_NAME}" ]; then
        MODULE_LIB="${BIN_DIR}/${MODULE_NAME}"
    fi
fi
if [ -z "$MODULE_LIB" ]; then
    if [ "$TRACE_DRY_RUN" = "1" ]; then
        trace_warn "No editor build of ${TRACE_PROJECT_NAME} yet (${MODULES_FILE#"${TRACE_PROJECT_ROOT}/"}); a real run would stop here."
    else
        trace_err "There is no editor build of ${TRACE_PROJECT_NAME} on this machine:"
        trace_err "    ${MODULES_FILE#"${TRACE_PROJECT_ROOT}/"} is missing, or the library it names is."
        trace_err "The stats are read out of the running game, so build it first:"
        trace_err "    Scripts/build.sh                (or: Scripts/dump-stats.sh --build)"
        exit 1
    fi
fi

STALE_SOURCE=""
if [ -n "$MODULE_LIB" ]; then
    STALE_SOURCE="$(find "${TRACE_PROJECT_ROOT}/Source" -type f -newer "$MODULE_LIB" -print 2>/dev/null | head -1 || true)"
fi
warn_stale() {
    trace_warn "The editor build is OLDER than the source: ${STALE_SOURCE#"${TRACE_PROJECT_ROOT}/"}"
    trace_warn "changed after ${MODULE_LIB#"${TRACE_PROJECT_ROOT}/"} was built. The sheet shows the numbers of"
    trace_warn "the build that runs, not of the code you have. For the current numbers:"
    trace_warn "    Scripts/dump-stats.sh --build"
}
[ -z "$STALE_SOURCE" ] || warn_stale

# ------------------------------------------------------------------------------
# The run
# ------------------------------------------------------------------------------
LOG_DIR="${TRACE_PROJECT_ROOT}/Saved/Logs"
LOG="${LOG_DIR}/dump-stats.log"
CONSOLE_LOG="${LOG_DIR}/dump-stats.console.log"

# ?game= is what makes this the practice range; it stays glued to the map path.
URL="${MAP}?game=/Script/Trace.TracePracticeGameMode"
EXEC_LIST="Trace.DumpStats ${GAME_PATH}|Trace.VerifyStats ${GAME_PATH}|quit"
ARGS=("$TRACE_UPROJECT" "$URL" -game -log -nullrhi -RenderOffScreen -unattended -nosound -nosplash
      "-TraceExec=${EXEC_LIST}" -TraceExecAt=6 -TraceExecOn=Match "-abslog=${LOG}")

trace_msg "Writing every stat in the game to ${TRACE_C_BOLD}${OUT}${TRACE_C_OFF}"
trace_print_cmd "$UE_EDITOR_BIN" "${ARGS[@]}"
if [ "$TRACE_DRY_RUN" = "1" ]; then
    trace_msg "dry run — not executed"
    exit 0
fi

mkdir -p "$LOG_DIR"
rm -f "$LOG" "$CONSOLE_LOG"

# In the background, so a hang has a budget and Ctrl+C can stop it: a background
# job of a script ignores Ctrl+C itself, so without the trap an interrupted run
# would leave a headless game running with no window to close.
#
# Only ever OUR pid, never a pkill: another editor or game may be open on this machine.
"$UE_EDITOR_BIN" "${ARGS[@]}" >"$CONSOLE_LOG" 2>&1 &
GAME_PID=$!

# stop_game — ask politely (SIGTERM), then insist. Measured on this Mac: a SIGTERM that lands
# while the engine is still loading modules logs "Engine exit requested" and then never exits,
# so a plain kill + wait hung this script for good.
stop_game() {
    kill -0 "$GAME_PID" 2>/dev/null || return 0
    kill "$GAME_PID" 2>/dev/null || true
    local Waited=0
    while kill -0 "$GAME_PID" 2>/dev/null && [ "$Waited" -lt 15 ]; do
        sleep 1
        Waited=$((Waited + 1))
    done
    if kill -0 "$GAME_PID" 2>/dev/null; then
        trace_warn "The game ignored a polite stop for 15 s; killing it (pid ${GAME_PID})."
        kill -9 "$GAME_PID" 2>/dev/null || true
    fi
}
trap 'trace_err "Interrupted - stopping the game (pid ${GAME_PID})."; stop_game; wait "$GAME_PID" 2>/dev/null || true; exit 130' INT TERM
trace_msg "Game started (pid ${GAME_PID}). Log: ${LOG#"${TRACE_PROJECT_ROOT}/"}"

WAITED=0
TIMED_OUT=0
ARMED_SAID=0
DONE_AT=-1
while kill -0 "$GAME_PID" 2>/dev/null; do
    if [ "$WAITED" -ge "$BUDGET" ]; then
        TIMED_OUT=1
        break
    fi
    if [ "$ARMED_SAID" = "0" ] && grep -q '\[AutoExec\] Armed' "$LOG" 2>/dev/null; then
        trace_msg "Match is up; the dump runs 6 s in."
        ARMED_SAID=1
    fi
    # Both verdicts are in and 'quit' is next. Allow shutdown a while, then stop waiting on it:
    # the file is already written and checked.
    if [ "$DONE_AT" -lt 0 ] && grep -q '\[VerifyStats\] VERDICT:' "$LOG" 2>/dev/null; then
        DONE_AT="$WAITED"
    fi
    if [ "$DONE_AT" -ge 0 ] && [ $((WAITED - DONE_AT)) -ge 30 ]; then
        trace_warn "The game did not quit within 30 s of finishing; stopping it."
        break
    fi
    if [ "$WAITED" -gt 0 ] && [ $((WAITED % 15)) -eq 0 ]; then
        trace_msg "  still running (${WAITED}s)"
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done
stop_game
GAME_RC=0
wait "$GAME_PID" 2>/dev/null || GAME_RC=$?
trap - INT TERM

# ------------------------------------------------------------------------------
# The verdict
# ------------------------------------------------------------------------------
# "[2026.10.04-14.36.38:879][875]LogTraceGame: Display: [DumpStats] ..." -> "[DumpStats] ..."
strip_log_prefix() {
    sed -e 's/^\[[^]]*\]\[[ 0-9]*\]//' -e 's/^LogTraceGame: [A-Za-z]*: //'
}
log_line() {
    { grep -E "$1" "$LOG" 2>/dev/null || true; } | tail -1 | strip_log_prefix
}

DUMP_VERDICT="$(log_line '\[DumpStats\] VERDICT:')"
VERIFY_VERDICT="$(log_line '\[VerifyStats\] VERDICT:')"
ROWS_LINE="$(log_line '\[DumpStats\] [0-9]+ rows in [0-9]+ sections -> ')"

echo
DUMP_SHOW="${DUMP_VERDICT#"[DumpStats] "}"
VERIFY_SHOW="${VERIFY_VERDICT#"[VerifyStats] "}"
trace_msg "Trace.DumpStats    ${DUMP_SHOW:-(no verdict)}"
trace_msg "Trace.VerifyStats  ${VERIFY_SHOW:-(no verdict)}"
{ grep -E '\[DumpStats\]   MISSING' "$LOG" 2>/dev/null || true; } | strip_log_prefix | sed -e 's/^/    /' || true

case "$DUMP_VERDICT" in *"VERDICT: PASS"*) DUMP_OK=1 ;; *) DUMP_OK=0 ;; esac
case "$VERIFY_VERDICT" in *"VERDICT: PASS"*) VERIFY_OK=1 ;; *) VERIFY_OK=0 ;; esac

if [ "$DUMP_OK" = "1" ] && [ "$VERIFY_OK" = "1" ] && [ -f "$OUT" ]; then
    ROWS="$(printf '%s' "$ROWS_LINE" | grep -Eo '[0-9]+ rows' | head -1 || true)"
    echo
    trace_msg "${TRACE_C_BOLD}Wrote ${OUT}${TRACE_C_OFF}${ROWS:+  (${ROWS})}"
    if [ "$GAME_PATH" = "docs/TraceStats.csv" ]; then
        trace_msg "That is the committed copy: 'git diff --stat docs/TraceStats.csv' shows whether anything moved."
    fi
    trace_msg "Google Sheets: File > Import > Upload, then Replace spreadsheet."
    if [ -n "$STALE_SOURCE" ]; then
        echo
        warn_stale
    fi
    exit 0
fi

# ------------------------------------------------------------------------------
# It failed. Say why, as specifically as the log allows.
# ------------------------------------------------------------------------------
echo
trace_err "No usable sheet. ${OUT} was not written and checked by this run."
if [ "$TIMED_OUT" = "1" ]; then
    trace_err "The game was still running after ${BUDGET}s and was stopped (raise it with --budget)."
fi
if [ ! -s "$LOG" ]; then
    trace_err "The game never got as far as writing a log (exit code ${GAME_RC}). What it printed:"
    tail -n 20 "$CONSOLE_LOG" 2>/dev/null | sed -e 's/^/    /' >&2 || true
elif grep -qE "could not be found|Incompatible or missing module|modules are missing or built with a different engine" "$LOG" 2>/dev/null; then
    trace_err "The editor build of ${TRACE_PROJECT_NAME} would not load - missing, or built for another engine."
    trace_err "    Scripts/build.sh                (or: Scripts/dump-stats.sh --build)"
elif grep -qE "Failed to load map|Couldn't find file for package ${MAP}|Failed to load package '${MAP}'" "$LOG" 2>/dev/null; then
    trace_err "The map ${MAP} did not load. If git has it as LFS pointers: git lfs pull"
elif ! grep -q '\[AutoExec\] Armed' "$LOG" 2>/dev/null; then
    trace_err "The match never started, so the dump was never run."
elif [ -z "$DUMP_VERDICT" ]; then
    trace_err "The match started but Trace.DumpStats did not run. Is this a Shipping or Test build? The command"
    trace_err "exists in Development/DebugGame editor builds only."
elif [ "$DUMP_OK" = "0" ]; then
    trace_err "The dump ran but is INCOMPLETE: a knob or settings class it reads by name is gone (listed above)."
    trace_err "The cells that needed it say <MISSING KNOB> instead of a number. Fix the name in"
    trace_err "Source/Trace/Debug/TraceStatsDump.cpp and run this again."
elif [ "$VERIFY_OK" = "0" ]; then
    trace_err "The file was written but does not parse as a clean table:"
    { grep -E '\[VerifyStats\]' "$LOG" 2>/dev/null || true; } | strip_log_prefix | head -n 10 | sed -e 's/^/    /' >&2 || true
fi
ERRORS="$({ grep -E '(Error|Fatal):' "$LOG" 2>/dev/null || true; } | grep -v 'UnifiedErrorTest' | head -n 8 | strip_log_prefix || true)"
if [ -n "$ERRORS" ]; then
    trace_err "First errors in the log:"
    printf '%s\n' "$ERRORS" | sed -e 's/^/    /' >&2 || true
fi
trace_err "Full log: ${LOG}"
exit 1
