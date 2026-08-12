#!/usr/bin/env bash
# ==============================================================================
# Trace — run-practice-range.sh   (spec v19 §2)
#
# Opens THE PRACTICE RANGE: the ordinary arena, with the practice game mode.
#
#   "$UE_ROOT/.../UnrealEditor" "<repo>/Trace.uproject" \
#       "/Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode" \
#       -game -log -windowed -ResX=1280 -ResY=720
#
# The "?game=" suffix is a URL option, not a command-line switch, so it must stay
# glued to the map path. It is also the ONLY way into the range: there is no
# setting, cvar or .ini that turns a real match into one, which is exactly what
# makes the range's cheats unable to reach a real match.
#
# WHAT YOU GET
#   * five stationary targets in a row across the field. Shoot them, knife them,
#     hit them with abilities; they take damage and come back where they fell.
#   * a CORE RACK pad on the centre pedestal. Walk onto it carrying the Core and
#     it stays there; walk off and back on to pick it up again.
#   * an INFINITE ABILITIES pad. Walk onto it to flip it; walk on again to flip
#     it back.
#   * a CHANGE CHARACTER pad. Walk onto it and the normal select screen reopens.
#
# The three pads all sit near the centre circle. Console equivalents, for a
# headless run: Trace.Practice.InfiniteAbilities, Trace.Practice.SwitchCharacter,
# Trace.Practice.Status.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

MAP="$TRACE_DEFAULT_MAP"
RES_X="1280"
RES_Y="720"
DO_BUILD=0
HEADLESS=0
EXTRA_ARGS=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} practice range (spec v19 §2)

USAGE
  Scripts/run-practice-range.sh [options] [-- <extra engine args>]

OPTIONS
  -m, --map <path>      Map to furnish. Default: ${TRACE_DEFAULT_MAP}
      --res <WxH>       Window size. Default: ${RES_X}x${RES_Y}
      --headless        -nullrhi -RenderOffScreen -unattended -nosound
  -b, --build           Build TraceEditor first (Scripts/build.sh)
  -n, --dry-run         Print the command; run nothing
  -h, --help            This text

NOTES
  The range is single-player and local. Everything it adds is gated on the game
  mode being ATracePracticeGameMode; Scripts/verify-practice-range.sh proves the
  gate holds by trying to break it.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--map)    [ $# -ge 2 ] || trace_die "--map needs a value"; MAP="$2"; shift 2 ;;
        --res)       [ $# -ge 2 ] || trace_die "--res needs a value"
                     RES_X="${2%%x*}"; RES_Y="${2##*x}"; shift 2 ;;
        --headless)  HEADLESS=1; shift ;;
        -b|--build)  DO_BUILD=1; shift ;;
        -n|--dry-run) TRACE_DRY_RUN=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        --)          shift; while [ $# -gt 0 ]; do EXTRA_ARGS+=("$1"); shift; done ;;
        -*)          trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)           trace_err "Unexpected argument: $1"; echo; usage; exit 2 ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

trace_require_uproject
trace_resolve_engine

if [ "$DO_BUILD" = "1" ]; then
    "${TRACE_SCRIPT_DIR}/build.sh"
fi

EDITOR="$(trace_editor_binary)"

# ?game= is the whole mechanism. Kept as one token with the map on purpose.
URL="${MAP}?game=/Script/Trace.TracePracticeGameMode"

ARGS=("$TRACE_UPROJECT" "$URL" -game -log)
if [ "$HEADLESS" = "1" ]; then
    ARGS+=(-nullrhi -RenderOffScreen -unattended -nosound -NoLoadingScreen)
else
    ARGS+=(-windowed "-ResX=${RES_X}" "-ResY=${RES_Y}")
fi
ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

trace_msg "Opening the PRACTICE RANGE on ${MAP}"
trace_run "$EDITOR" "${ARGS[@]}"
