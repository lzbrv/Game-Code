#!/usr/bin/env bash
# ==============================================================================
# Trace — run-client.sh
#
# Connects a client to a running Trace server (listen or dedicated).
#
# The command it runs (printed before every launch):
#
#   "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
#       "<repo>/Trace.uproject" 127.0.0.1:7777 \
#       -game -log -windowed -ResX=1280 -ResY=720
#
# The bare "IP:PORT" positional argument IS the travel URL — passing an address
# where a map path would normally go is how an Unreal client joins a server.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

HOST=""
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
${TRACE_PROJECT_NAME} client

USAGE
  Scripts/run-client.sh [<ip>[:<port>]] [options] [-- <extra engine args>]

ARGUMENTS
  <ip>[:<port>]         Server address. Default: 127.0.0.1:${TRACE_DEFAULT_PORT}
                        A port in the address wins over --port.

OPTIONS
      --port <n>        Server port. Default: ${TRACE_DEFAULT_PORT}
      --res <WxH>       Window size. Default: ${RES_X}x${RES_Y}
      --pos <X,Y>       Window position (tile several clients on one screen)
      --fullscreen      Launch fullscreen instead of windowed
  -b, --build           Build TraceEditor first (Scripts/build.sh)
  -n, --dry-run         Print the command; run nothing
  -h, --help            This text

ENVIRONMENT
  UE_ROOT               Engine install. Default on macOS:
                        /Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}

NOTES
  * Find the server machine's LAN address with:  ipconfig getifaddr en0
  * Two clients on one Mac is fine — give each a different --pos so the windows
    do not stack. Each is a separate process with its own prediction state, which
    is exactly what you want when eyeballing dash prediction and the trail.
  * If it hangs on "Pending connection", the server is not listening, the port is
    wrong, or macOS firewall blocked the server process.

EXAMPLES
  Scripts/run-client.sh
  Scripts/run-client.sh 192.168.1.42
  Scripts/run-client.sh 192.168.1.42:7778 --res 1600x900
  Scripts/run-client.sh 127.0.0.1 --pos 700,0
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --port)       [ $# -ge 2 ] || trace_die "--port needs a value"; PORT="$2"; shift 2 ;;
        --res)        [ $# -ge 2 ] || trace_die "--res needs a value"; RES_X="${2%%x*}"; RES_Y="${2##*x}"; shift 2 ;;
        --pos)        [ $# -ge 2 ] || trace_die "--pos needs a value"; WIN_X="${2%%,*}"; WIN_Y="${2##*,}"; shift 2 ;;
        --fullscreen) WINDOWED=0; shift ;;
        -b|--build)   DO_BUILD=1; shift ;;
        -n|--dry-run) TRACE_DRY_RUN=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; while [ $# -gt 0 ]; do EXTRA_ARGS+=("$1"); shift; done ;;
        -*)           trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)
            [ -z "$HOST" ] || trace_die "Only one server address may be given (got '${HOST}' and '$1')."
            HOST="$1"; shift
            ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

[ -n "$HOST" ] || HOST="127.0.0.1"

# An explicit port inside the address argument takes precedence over --port.
case "$HOST" in
    *:*) ADDRESS="$HOST" ;;
    *)   ADDRESS="${HOST}:${PORT}" ;;
esac

case "${ADDRESS##*:}" in
    ''|*[!0-9]*) trace_die "Bad port in address '${ADDRESS}'." ;;
esac

trace_require_uproject
trace_resolve_engine

if [ "$DO_BUILD" = "1" ]; then
    trace_msg "Building TraceEditor before launch"
    "${TRACE_SCRIPT_DIR}/build.sh" --target TraceEditor --config Development
fi

# The client still loads the map locally after travel, so a missing Arena.umap
# fails at connect time rather than at launch — warn early.
trace_warn_if_map_missing "$TRACE_DEFAULT_MAP"

EDITOR="$(trace_editor_binary)"

ARGS=("$TRACE_UPROJECT" "$ADDRESS" -game -log -nosplash)
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

trace_msg "Connecting to ${TRACE_C_BOLD}${ADDRESS}${TRACE_C_OFF}"
trace_run "$EDITOR" "${ARGS[@]}"
