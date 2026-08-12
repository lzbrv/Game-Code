#!/usr/bin/env bash
# ==============================================================================
# Trace — verify-practice-range.sh   (spec v19 §2)
#
# THREE HEADLESS RUNS, IN THE ORDER THAT MAKES THE EVIDENCE MEAN SOMETHING.
#
#   1. LEAK, RED ARM      a REAL match with Trace.Practice.LeakArm 1.
#                         The range's gate is forced open, so its cheats really
#                         do apply in the match: Trace.Practice.LeakTest must
#                         report assertions FAILING. If this run comes back
#                         clean, the harness cannot see a leak and run 2 proves
#                         nothing — that is the whole point of doing it first.
#
#   2. LEAK, GREEN ARM    the same REAL match, arm off. Every assertion must
#                         pass: no range furniture, the planted cooldown still
#                         running, the character still locked.
#
#   3. THE RANGE ITSELF   the practice game mode. Trace.Practice.Verify drives a
#                         target's damage and respawn-in-place, racks the Core
#                         and watches it not move, flips infinite abilities
#                         against a live cooldown, and reopens character select.
#
# Every run is headless, unattended and BATCHED — it opens, runs, and quits on
# its own. Logs land in Saved/Logs/practice-verify-*.log, namespaced so a
# parallel session's logs are never overwritten.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

MAP="$TRACE_DEFAULT_MAP"
DO_BUILD=0
ONLY=""

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} practice-range verification (spec v19 §2)

USAGE
  Scripts/verify-practice-range.sh [options]

OPTIONS
  -m, --map <path>   Map to use. Default: ${TRACE_DEFAULT_MAP}
      --only <n>     Run only arm N: 1 = leak/red, 2 = leak/green, 3 = the range
  -b, --build        Build TraceEditor first
  -h, --help         This text
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--map)   [ $# -ge 2 ] || trace_die "--map needs a value"; MAP="$2"; shift 2 ;;
        --only)     [ $# -ge 2 ] || trace_die "--only needs a value"; ONLY="$2"; shift 2 ;;
        -b|--build) DO_BUILD=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *)          trace_err "Unexpected argument: $1"; echo; usage; exit 2 ;;
    esac
done

trace_require_uproject
trace_resolve_engine

if [ "$DO_BUILD" = "1" ]; then
    "${TRACE_SCRIPT_DIR}/build.sh"
fi

EDITOR="$(trace_editor_binary)"
LOG_DIR="${TRACE_PROJECT_ROOT}/Saved/Logs"
mkdir -p "$LOG_DIR"

# ------------------------------------------------------------------------------
# run_arm <name> <log-suffix> <url> <execcmds> <budget-seconds>
#
# -ExecCmds runs on the FIRST TICK of the loaded map — before the player has
# logged in and before the range has furnished itself. Both harnesses know that
# and wait for the world; this function's job is only to stop the process once
# they have printed a VERDICT, or when the budget runs out.
#
# *** SEPARATE COMMANDS WITH A COMMA. *** UE splits -ExecCmds on commas and on
# nothing else (Engine/Source/Runtime/Engine/Private/ParseExecCommands.cpp,
# ParseExecCmds: the only delimiter is ',' outside single quotes). A pipe is NOT
# a separator, and this cost us the whole red arm on the first run: the string
# "Trace.Practice.LeakArm 1|Trace.Practice.LeakTest" arrived as ONE command, the
# console set the cvar from its first token and dropped the rest, and the leak
# test never ran at all. The log printed no [PRACTICELEAK] line whatsoever — so
# the green arm that followed was measuring against a red arm that had never
# happened. Hence the NO VERDICT guard below: a run that prints no verdict is
# reported as loudly as a failing one, because silence is the shape this project
# has twice mistaken for a pass.
#
# WE ONLY EVER KILL THE PID WE STARTED. There is deliberately no `pkill -f
# UnrealEditor` anywhere in this file: other agents run live rigs on this
# machine and a broad kill takes theirs down with ours.
# ------------------------------------------------------------------------------
run_arm() {
    ARM_NAME="$1"; ARM_SUFFIX="$2"; ARM_URL="$3"; ARM_CMDS="$4"; ARM_BUDGET="$5"
    ARM_LOG="${LOG_DIR}/practice-verify-${ARM_SUFFIX}.log"
    rm -f "$ARM_LOG"

    trace_msg "ARM: ${ARM_NAME}"
    trace_msg "  log: ${ARM_LOG}"

    "$EDITOR" "$TRACE_UPROJECT" "$ARM_URL" \
        -game -log -nullrhi -RenderOffScreen -unattended -nosound -NoLoadingScreen \
        "-ExecCmds=${ARM_CMDS}" \
        "-abslog=${ARM_LOG}" >/dev/null 2>&1 &
    ARM_PID=$!

    ARM_WAITED=0
    while [ "$ARM_WAITED" -lt "$ARM_BUDGET" ]; do
        if ! kill -0 "$ARM_PID" 2>/dev/null; then
            break
        fi
        if [ -f "$ARM_LOG" ] && grep -q "VERDICT:" "$ARM_LOG" 2>/dev/null; then
            sleep 1
            break
        fi
        sleep 1
        ARM_WAITED=$((ARM_WAITED + 1))
    done

    kill "$ARM_PID" 2>/dev/null || true
    wait "$ARM_PID" 2>/dev/null || true

    echo
    grep -E "PRACTICELEAK\]|\[PRACTICE\]" "$ARM_LOG" 2>/dev/null | sed -e 's/^\[[^]]*\]\[[ 0-9]*\]//' || true

    if ! grep -q "VERDICT:" "$ARM_LOG" 2>/dev/null; then
        trace_err "*** NO VERDICT from arm '${ARM_NAME}'. ***"
        trace_err "    The harness did not run to completion, so this arm measured NOTHING."
        trace_err "    Check that -ExecCmds is COMMA separated and that the budget was enough:"
        trace_err "      ${ARM_LOG}"
    fi
    echo
}

REAL_URL="${MAP}?bots=2"
RANGE_URL="${MAP}?game=/Script/Trace.TracePracticeGameMode"

if [ -z "$ONLY" ] || [ "$ONLY" = "1" ]; then
    run_arm "1/3  LEAK TEST, RED ARM (a real match with the gate forced OPEN — must FAIL)" \
            "leak-red" \
            "$REAL_URL" \
            "Trace.Practice.LeakArm 1,Trace.Practice.LeakTest" \
            75
fi

if [ -z "$ONLY" ] || [ "$ONLY" = "2" ]; then
    run_arm "2/3  LEAK TEST, GREEN ARM (the same real match, shipped gate — must PASS)" \
            "leak-green" \
            "$REAL_URL" \
            "Trace.Practice.LeakTest" \
            75
fi

if [ -z "$ONLY" ] || [ "$ONLY" = "3" ]; then
    run_arm "3/3  THE RANGE ITSELF (practice game mode — must PASS)" \
            "range" \
            "$RANGE_URL" \
            "Trace.Practice.Verify" \
            140
fi

trace_msg "Done. Read the VERDICT lines above; the logs are in ${LOG_DIR}."
