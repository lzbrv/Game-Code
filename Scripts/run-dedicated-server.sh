#!/usr/bin/env bash
# ==============================================================================
# Trace — run-dedicated-server.sh
#
# Builds (unless --no-build) and runs the TraceServer target: a headless,
# authoritative server with no local player. This is the configuration that
# catches the bugs a listen server hides — anything that only works because the
# server also happened to be a client (local pawn assumptions, cosmetic code
# running on authority, missing HasAuthority() guards) breaks here first.
#
# The commands it runs (printed before every step):
#
#   "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
#       TraceServer Mac Development "<repo>/Trace.uproject" -waitmutex
#
#   "<repo>/Binaries/Mac/TraceServer" "<repo>/Trace.uproject" \
#       /Game/Maps/Arena -log -port=7777
#
# With --editor it instead runs the editor binary in dedicated-server mode, which
# needs no TraceServer build:
#
#   "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "<repo>/Trace.uproject" \
#       /Game/Maps/Arena -server -log -port=7777
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

MAP="$TRACE_DEFAULT_MAP"
PORT="$TRACE_DEFAULT_PORT"
CONFIG="Development"
DO_BUILD=1
USE_EDITOR=0
EXTRA_ARGS=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} dedicated server

USAGE
  Scripts/run-dedicated-server.sh [options] [-- <extra engine args>]

OPTIONS
  -m, --map <path>      Map to open. Default: ${TRACE_DEFAULT_MAP}
      --port <n>        Listen port. Default: ${TRACE_DEFAULT_PORT}
  -c, --config <name>   Build configuration. Default: Development
                        One of: Debug | DebugGame | Development | Test | Shipping
      --no-build        Skip the build step, run whatever binary is already there
      --editor          Do not use the TraceServer binary; run the editor binary
                        with -server instead (no server target build needed)
  -n, --dry-run         Print the commands; run nothing
  -h, --help            This text

ENVIRONMENT
  UE_ROOT               Engine install. Default on macOS:
                        /Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}

NOTES
  * Connect to it with:  Scripts/run-client.sh <server-ip>
  * A dedicated server never renders. Do not expect a window; watch the log.
  * Stop it with Ctrl-C.
  * Server builds strip client-only content. If something is visible on a listen
    server but missing here, the code that creates it is almost certainly not
    guarded for NM_DedicatedServer.

EXAMPLES
  Scripts/run-dedicated-server.sh
  Scripts/run-dedicated-server.sh --no-build --port 7778
  Scripts/run-dedicated-server.sh --editor          # quickest, no extra build
  Scripts/run-dedicated-server.sh -- -LogCmds="LogNet Verbose"
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--map)     [ $# -ge 2 ] || trace_die "--map needs a value"; MAP="$2"; shift 2 ;;
        --port)       [ $# -ge 2 ] || trace_die "--port needs a value"; PORT="$2"; shift 2 ;;
        -c|--config)  [ $# -ge 2 ] || trace_die "--config needs a value"; CONFIG="$2"; shift 2 ;;
        --no-build)   DO_BUILD=0; shift ;;
        --editor)     USE_EDITOR=1; DO_BUILD=0; shift ;;
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
case "$CONFIG" in
    Debug|DebugGame|Development|Test|Shipping) : ;;
    *) trace_die "Unknown configuration '${CONFIG}'. Expected Debug, DebugGame, Development, Test or Shipping." ;;
esac

trace_require_uproject
trace_resolve_engine
trace_warn_if_map_missing "$MAP"

# ------------------------------------------------------------------------------
# Path A — editor binary in dedicated-server mode.
# ------------------------------------------------------------------------------
if [ "$USE_EDITOR" = "1" ]; then
    CMD_BIN="$(trace_editor_cmd_binary)"
    ARGS=("$TRACE_UPROJECT" "$MAP" -server -log -nosplash "-port=${PORT}")
    ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})
    trace_msg "Dedicated server (editor binary) on port ${TRACE_C_BOLD}${PORT}${TRACE_C_OFF}, map ${TRACE_C_BOLD}${MAP}${TRACE_C_OFF}"
    trace_run "$CMD_BIN" "${ARGS[@]}"
    exit 0
fi

# ------------------------------------------------------------------------------
# Path B — the real TraceServer target.
# ------------------------------------------------------------------------------
if [ "$DO_BUILD" = "1" ]; then
    # build.sh announces the target/platform/config itself — no need to duplicate it.
    "${TRACE_SCRIPT_DIR}/build.sh" --target TraceServer --config "$CONFIG"
fi

# UnrealBuildTool names the output "TraceServer" for the Development config and
# "TraceServer-<Platform>-<Config>" for every other one. On macOS a server target
# is normally a plain executable, but probe the .app layout too rather than
# assuming — a wrong guess here would surface as a confusing "not found".
BIN_DIR="${TRACE_PROJECT_ROOT}/Binaries/${TRACE_HOST_PLATFORM}"
if [ "$CONFIG" = "Development" ]; then
    BASE="TraceServer"
else
    BASE="TraceServer-${TRACE_HOST_PLATFORM}-${CONFIG}"
fi

SERVER_BIN="$(trace_find_first_executable \
    "${BIN_DIR}/${BASE}" \
    "${BIN_DIR}/${BASE}.app/Contents/MacOS/${BASE}" \
    "${BIN_DIR}/${BASE}-Cmd")" || SERVER_BIN=""

if [ -z "$SERVER_BIN" ]; then
    if [ "$TRACE_DRY_RUN" = "1" ]; then
        # Nothing was built in a dry run, so an absent binary is expected.
        SERVER_BIN="${BIN_DIR}/${BASE}"
    else
        trace_err "No dedicated-server binary found. Looked for:"
        trace_err "  ${BIN_DIR}/${BASE}"
        trace_err "  ${BIN_DIR}/${BASE}.app/Contents/MacOS/${BASE}"
        trace_err ""
        trace_err "Build it with:  Scripts/build.sh --target TraceServer --config ${CONFIG}"
        trace_err "Or skip the server target entirely:  Scripts/run-dedicated-server.sh --editor"
        exit 1
    fi
fi

ARGS=("$TRACE_UPROJECT" "$MAP" -log -nosplash "-port=${PORT}")
ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

trace_msg "Dedicated server on port ${TRACE_C_BOLD}${PORT}${TRACE_C_OFF}, map ${TRACE_C_BOLD}${MAP}${TRACE_C_OFF}"
trace_run "$SERVER_BIN" "${ARGS[@]}"
