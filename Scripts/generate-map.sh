#!/usr/bin/env bash
# ==============================================================================
# Trace — generate-map.sh
#
# Creates the empty /Game/Maps/Arena level headlessly by driving
# Scripts/generate_map.py through Unreal's pythonscript commandlet. Nobody should
# have to click File > New Level > Empty Level > Save As to bootstrap a clone.
#
# The command it runs (printed before every launch):
#
#   "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "<repo>/Trace.uproject" \
#       -run=pythonscript -script="<repo>/Scripts/generate_map.py" \
#       -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
# The arena itself is built in C++ by ATraceArenaBuilder at BeginPlay, so the
# .umap this produces is deliberately, permanently empty.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

MAP="$TRACE_DEFAULT_MAP"
FORCE=0
NULL_RHI=0
EXTRA_ARGS=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} map generation

USAGE
  Scripts/generate-map.sh [options] [-- <extra engine args>]

OPTIONS
  -m, --map <path>    Package path to create. Default: ${TRACE_DEFAULT_MAP}
  -f, --force         Delete and recreate the map if it already exists
      --nullrhi       Add -nullrhi (faster, headless; drop it if creation fails)
  -n, --dry-run       Print the command; run nothing
  -h, --help          This text

ENVIRONMENT
  UE_ROOT             Engine install. Default on macOS:
                      /Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}

REQUIRES
  * PythonScriptPlugin enabled in Trace.uproject (otherwise the commandlet does
    not exist and the run fails immediately).
  * A built editor target — run Scripts/build.sh first on a fresh clone.

EXAMPLES
  Scripts/generate-map.sh
  Scripts/generate-map.sh --force
  Scripts/generate-map.sh --map /Game/Maps/ArenaTest
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--map)     [ $# -ge 2 ] || trace_die "--map needs a value"; MAP="$2"; shift 2 ;;
        -f|--force)   FORCE=1; shift ;;
        --nullrhi)    NULL_RHI=1; shift ;;
        -n|--dry-run) TRACE_DRY_RUN=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; while [ $# -gt 0 ]; do EXTRA_ARGS+=("$1"); shift; done ;;
        -*)           trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)            trace_err "Unexpected argument: $1"; echo; usage; exit 2 ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

case "$MAP" in
    /Game/*) : ;;
    *) trace_die "--map must be a /Game/... package path, got '${MAP}'." ;;
esac

trace_require_uproject
trace_resolve_engine

PY_SCRIPT="${TRACE_SCRIPT_DIR}/generate_map.py"
[ -f "$PY_SCRIPT" ] || trace_die "generate_map.py not found at ${PY_SCRIPT}"

# Trace.uproject is owned by the build agent; only warn, never edit it here.
if ! grep -q 'PythonScriptPlugin' "$TRACE_UPROJECT" 2>/dev/null; then
    trace_warn "Trace.uproject does not mention PythonScriptPlugin."
    trace_warn "If this run fails with \"could not be found\" for the pythonscript commandlet,"
    trace_warn "enable Edit > Plugins > Python Editor Script Plugin (or add it to Trace.uproject)."
fi

CMD_BIN="$(trace_editor_cmd_binary)"

# generate_map.py reads these; -script= gives no reliable way to pass argv.
export TRACE_MAP_PATH="$MAP"
export TRACE_FORCE_MAP="$FORCE"

ARGS=("$TRACE_UPROJECT"
      -run=pythonscript
      "-script=${PY_SCRIPT}"
      -unattended
      -nosplash
      -nopause
      -stdout
      -FullStdOutLogOutput)
if [ "$NULL_RHI" = "1" ]; then
    ARGS+=(-nullrhi)
fi
ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

trace_msg "Creating ${TRACE_C_BOLD}${MAP}${TRACE_C_OFF} (TRACE_FORCE_MAP=${FORCE})"
trace_run "$CMD_BIN" "${ARGS[@]}"

if [ "$TRACE_DRY_RUN" = "1" ]; then
    exit 0
fi

REL="${MAP#/Game/}"
if [ -f "${TRACE_PROJECT_ROOT}/Content/${REL}.umap" ]; then
    trace_msg "Wrote Content/${REL}.umap"
    trace_msg "Next: Scripts/run-listen-server.sh"
else
    trace_err "Commandlet finished but Content/${REL}.umap is not on disk."
    trace_err "Scroll up for the [Trace] lines from generate_map.py — they name the API that failed."
    trace_err "Manual fallback: open the editor, File > New Level > Empty Level, Save As Content/${REL}."
    exit 1
fi
