#!/usr/bin/env bash
# ==============================================================================
# Trace — run-listen-server.sh
#
# Launches a listen server: one process that is both the authoritative server and
# a playing client. This is the fastest loop for testing multiplayer by hand —
# start this, then point Scripts/run-client.sh at it.
#
# The command it runs (printed before every launch):
#
#   "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
#       "<repo>/Trace.uproject" "/Game/Maps/Arena?listen" \
#       -game -log -windowed -ResX=1280 -ResY=720 -port=7777
#
# The "?listen" suffix is what makes the map open in listen-server mode; it is a
# URL option, not a command-line switch, so it must stay glued to the map path.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

MAP="$TRACE_DEFAULT_MAP"
PORT="$TRACE_DEFAULT_PORT"
RES_X="1280"
RES_Y="720"
WIN_X=""
WIN_Y=""
WINDOWED=1
DO_BUILD=0
EXTRA_ARGS=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} listen server

USAGE
  Scripts/run-listen-server.sh [options] [-- <extra engine args>]

OPTIONS
  -m, --map <path>      Map to open. Default: ${TRACE_DEFAULT_MAP}
      --port <n>        Listen port. Default: ${TRACE_DEFAULT_PORT}
      --res <WxH>       Window size. Default: ${RES_X}x${RES_Y}
      --pos <X,Y>       Window position on screen (handy when tiling server +
                        clients on one machine)
      --fullscreen      Launch fullscreen instead of windowed
  -b, --build           Build TraceEditor first (Scripts/build.sh)
  -n, --dry-run         Print the command; run nothing
  -h, --help            This text

ENVIRONMENT
  UE_ROOT               Engine install. Default on macOS:
                        /Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}

NOTES
  * This runs the game with the EDITOR binary (-game), so no packaging step is
    needed. It loads your compiled TraceEditor module.
  * Other machines connect with:  Scripts/run-client.sh <this-machine-ip>
  * Same machine, second window:  Scripts/run-client.sh 127.0.0.1 --pos 700,0
  * Firewall: macOS will ask to allow incoming connections the first time. Say
    yes, or nobody can join.

EXAMPLES
  Scripts/run-listen-server.sh
  Scripts/run-listen-server.sh --port 7778 --res 1600x900 --pos 0,0
  Scripts/run-listen-server.sh -- -NetDriverDebug -LogCmds="LogNet Verbose"
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--map)     [ $# -ge 2 ] || trace_die "--map needs a value"; MAP="$2"; shift 2 ;;
        --port)       [ $# -ge 2 ] || trace_die "--port needs a value"; PORT="$2"; shift 2 ;;
        --res)        [ $# -ge 2 ] || trace_die "--res needs a value"; RES_X="${2%%x*}"; RES_Y="${2##*x}"; shift 2 ;;
        --pos)        [ $# -ge 2 ] || trace_die "--pos needs a value"; WIN_X="${2%%,*}"; WIN_Y="${2##*,}"; shift 2 ;;
        --fullscreen) WINDOWED=0; shift ;;
        -b|--build)   DO_BUILD=1; shift ;;
        -n|--dry-run) TRACE_DRY_RUN=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; while [ $# -gt 0 ]; do EXTRA_ARGS+=("$1"); shift; done ;;
        -*)           trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)            trace_err "Unexpected argument: $1"; echo; usage; exit 2 ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

case "$PORT" in
    ''|*[!0-9]*) trace_die "--port must be a number, got '${PORT}'" ;;
esac

trace_require_uproject
trace_resolve_engine

if [ "$DO_BUILD" = "1" ]; then
    trace_msg "Building TraceEditor before launch"
    "${TRACE_SCRIPT_DIR}/build.sh" --target TraceEditor --config Development
fi

trace_warn_if_map_missing "$MAP"

EDITOR="$(trace_editor_binary)"

# Append the listen option to the URL unless the caller already supplied options.
case "$MAP" in
    *\?*) URL="$MAP" ;;
    *)    URL="${MAP}?listen" ;;
esac

ARGS=("$TRACE_UPROJECT" "$URL" -game -log -nosplash "-port=${PORT}")
if [ "$WINDOWED" = "1" ]; then
    ARGS+=(-windowed "-ResX=${RES_X}" "-ResY=${RES_Y}")
    # Window placement is meaningless fullscreen, so only pass it when windowed.
    if [ -n "$WIN_X" ]; then
        ARGS+=("-WinX=${WIN_X}" "-WinY=${WIN_Y}")
    fi
else
    ARGS+=(-fullscreen)
fi
ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

trace_msg "Listen server on port ${TRACE_C_BOLD}${PORT}${TRACE_C_OFF}, map ${TRACE_C_BOLD}${URL}${TRACE_C_OFF}"
trace_run "$EDITOR" "${ARGS[@]}"
