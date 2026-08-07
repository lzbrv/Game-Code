#!/usr/bin/env bash
# ==============================================================================
# Trace — build.sh
#
# Thin, honest wrapper around UnrealBuildTool. It resolves the engine, checks the
# toolchain, and then runs exactly one command, which it prints first:
#
#   "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
#       TraceEditor Mac Development "<repo>/Trace.uproject" -waitmutex
#
# Nothing here is magic. If the script ever gets in your way, copy the printed
# command and run it yourself.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

TARGET="TraceEditor"
CONFIG="Development"
PLATFORM=""
DO_CLEAN=0
DO_PROJECTFILES=0
# Import Epic's Mannequin automatically if it is missing. See the art block below.
DO_IMPORT_ART="${TRACE_SKIP_ART_IMPORT:+0}"
DO_IMPORT_ART="${DO_IMPORT_ART:-1}"
EXTRA_ARGS=()

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} build

USAGE
  Scripts/build.sh [options] [-- <extra UnrealBuildTool args>]

OPTIONS
  -t, --target <name>     Build target. Default: TraceEditor
                          One of: Trace | TraceEditor | TraceServer
  -c, --config <name>     Build configuration. Default: Development
                          One of: Debug | DebugGame | Development | Test | Shipping
  -p, --platform <name>   Target platform. Default: this host (${TRACE_HOST_PLATFORM})
      --clean             Clean the target instead of building it (-clean)
      --projectfiles      Regenerate IDE project files (Xcode workspace / Makefile)
                          instead of building
      --no-art            Skip the automatic Mannequin import. Characters will
                          render as fallback shapes unless already imported.
  -n, --dry-run           Print the command that would run; run nothing
  -h, --help              This text

ENVIRONMENT
  UE_ROOT                 Engine install to use. Default on macOS:
                          /Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}
                          A per-developer override can also live in <repo>/.ue-root

WHAT IT WRAPS
  build:
    "\$UE_ROOT/Engine/Build/BatchFiles/${TRACE_HOST_PLATFORM}/Build.sh" \\
        TraceEditor ${TRACE_HOST_PLATFORM} Development "${TRACE_UPROJECT}" -waitmutex

  clean:
    ... same, plus -clean

  project files:
    "\$UE_ROOT/Engine/Build/BatchFiles/${TRACE_HOST_PLATFORM}/Build.sh" \\
        -projectfiles -project="${TRACE_UPROJECT}" -game -progress

  On Windows the equivalent is Engine\\Build\\BatchFiles\\Build.bat with Win64
  in place of ${TRACE_HOST_PLATFORM}.

EXAMPLES
  Scripts/build.sh                          # editor target, Development
  Scripts/build.sh -t TraceServer           # dedicated server target
  Scripts/build.sh -c DebugGame             # game code with optimisations off
  Scripts/build.sh --clean -t TraceEditor   # force a full rebuild next time
  Scripts/build.sh --projectfiles           # after adding/removing a .cpp file
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -t|--target)    [ $# -ge 2 ] || trace_die "--target needs a value"; TARGET="$2"; shift 2 ;;
        -c|--config)    [ $# -ge 2 ] || trace_die "--config needs a value"; CONFIG="$2"; shift 2 ;;
        -p|--platform)  [ $# -ge 2 ] || trace_die "--platform needs a value"; PLATFORM="$2"; shift 2 ;;
        --clean)        DO_CLEAN=1; shift ;;
        --projectfiles) DO_PROJECTFILES=1; shift ;;
        --no-art)       DO_IMPORT_ART=0; shift ;;
        -n|--dry-run)   TRACE_DRY_RUN=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        --)             shift; while [ $# -gt 0 ]; do EXTRA_ARGS+=("$1"); shift; done ;;
        -*)             trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)              trace_err "Unexpected argument: $1"; echo; usage; exit 2 ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

# Validate up front — a typo here otherwise surfaces as a 200-line UBT stack trace.
case "$TARGET" in
    Trace|TraceEditor|TraceServer) : ;;
    *) trace_die "Unknown target '${TARGET}'. Expected Trace, TraceEditor or TraceServer." ;;
esac
case "$CONFIG" in
    Debug|DebugGame|Development|Test|Shipping) : ;;
    *) trace_die "Unknown configuration '${CONFIG}'. Expected Debug, DebugGame, Development, Test or Shipping." ;;
esac
[ -n "$PLATFORM" ] || PLATFORM="$TRACE_HOST_PLATFORM"

trace_require_uproject
trace_resolve_engine
trace_check_toolchain

BUILD_SH="$(trace_build_script)"

# ------------------------------------------------------------------------------
# Character art — imported, not committed
#
# Epic's Mannequin is deliberately NOT in the repo: .gitignore excludes
# /Content/Characters/, and Scripts/import-mannequin.sh copies it out of each
# developer's own UE install instead. That keeps the repo ~1.3 MB and off
# GitHub's LFS quota (~1 GiB storage AND ~1 GiB/month bandwidth, which a team of
# four cloning would burn through fast).
#
# The cost of that choice is that a fresh clone builds with fallback primitives
# until somebody remembers the extra command — which is exactly how a
# collaborator came to report "there don't seem to be any models in the project".
# So do it for them. The engine ships the source art, so this needs no network.
#
# DELIBERATELY NON-FATAL. The game runs fine with fallback shapes and now warns
# loudly on screen about it, so a partial engine install (no "Templates and
# Feature Packs") must degrade the visuals, never block the build.
#
# Skip with --no-art, or TRACE_SKIP_ART_IMPORT=1 for CI.
# ------------------------------------------------------------------------------
if [ "$DO_IMPORT_ART" = "1" ] && [ "$TRACE_DRY_RUN" != "1" ]; then
    IMPORT_SH="${TRACE_SCRIPT_DIR}/import-mannequin.sh"
    if [ ! -x "$IMPORT_SH" ]; then
        trace_warn "Missing ${IMPORT_SH}; skipping the character-art check."
    elif "$IMPORT_SH" --verify >/dev/null 2>&1; then
        : # Already imported and complete — say nothing, this is the common case.
    else
        trace_msg "Character art missing or incomplete — importing Epic's Mannequin"
        if "$IMPORT_SH"; then
            trace_msg "Character art imported."
        else
            trace_warn "Mannequin import failed. Building anyway — characters will render as"
            trace_warn "fallback shapes and the game will say so on screen."
            trace_warn "Run ${IMPORT_SH} on its own to see why."
        fi
    fi
fi

if [ "$DO_PROJECTFILES" = "1" ]; then
    # Installed (launcher) engines ship Build.sh but not always GenerateProjectFiles.sh,
    # which is a source-build convenience. Prefer it when present, otherwise drive
    # UnrealBuildTool's -projectfiles mode directly — same result either way.
    GPF="${UE_ROOT}/Engine/Build/BatchFiles/${TRACE_HOST_PLATFORM}/GenerateProjectFiles.sh"
    if [ -x "$GPF" ]; then
        trace_msg "Regenerating IDE project files"
        trace_run "$GPF" -project="$TRACE_UPROJECT" -game -progress
    else
        trace_msg "Regenerating IDE project files (via UnrealBuildTool -projectfiles)"
        # No -rocket: that switch is UE4-era. Modern UBT detects an installed
        # engine from Engine/Build/InstalledBuild.txt on its own, and passing an
        # argument UBT does not recognise is a hard error in project-files mode.
        trace_run "$BUILD_SH" -projectfiles -project="$TRACE_UPROJECT" -game -progress
    fi
    trace_msg "Done. Open ${TRACE_PROJECT_NAME}.xcworkspace (macOS) or the generated Makefile (Linux)."
    exit 0
fi

ARGS=("$TARGET" "$PLATFORM" "$CONFIG" "$TRACE_UPROJECT" -waitmutex)
# Note: `[ x = y ] && ARGS+=(...)` would return 1 on the false branch and `set -e`
# would kill the script. Always use a full `if` for conditional appends.
if [ "$DO_CLEAN" = "1" ]; then
    ARGS+=(-clean)
fi
# ${ARR[@]+...} keeps `set -u` happy when EXTRA_ARGS is empty under bash 3.2.
ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

if [ "$DO_CLEAN" = "1" ]; then
    trace_msg "Cleaning ${TARGET} | ${PLATFORM} | ${CONFIG}"
else
    trace_msg "Building ${TARGET} | ${PLATFORM} | ${CONFIG}"
fi

START="$(date +%s)"
trace_run "$BUILD_SH" "${ARGS[@]}"
ELAPSED=$(( $(date +%s) - START ))

if [ "$TRACE_DRY_RUN" = "1" ]; then
    exit 0
fi

trace_msg "Success in ${ELAPSED}s."

# ------------------------------------------------------------------------------
# Guard against the stale-metadata trap
#
# UnrealEditor.modules tells the engine WHICH dylib to load. Writing it is a
# SEPARATE UnrealBuildTool action (WriteMetadata) that frequently does not run in
# a build that only compiled and linked. Measured repeatedly on this project: a
# build ends "[2/2] Link ...-0042.dylib" with .modules still naming 0041.
#
# The consequence is the nastiest failure mode this project has: the build says
# "Succeeded", the game launches happily, and you are running the PREVIOUS
# binary. It has already cost real time twice — once as "the game keeps
# restarting and nothing changes", and once as a collaborator reporting missing
# features that were in fact never in the build they were running.
#
# So: check it, and repair it by re-running the build (a second invocation has
# nothing to compile, so WriteMetadata is the only action left and it runs).
# ------------------------------------------------------------------------------
trace_check_module_metadata() {
    local BinDir="${TRACE_PROJECT_ROOT}/Binaries/${TRACE_HOST_PLATFORM}"
    local Modules="${BinDir}/UnrealEditor.modules"
    [ -f "$Modules" ] || return 0

    local Newest Named
    Newest="$(ls -t "${BinDir}"/libUnrealEditor-${TRACE_PROJECT_NAME}-*.dylib 2>/dev/null | head -1)"
    [ -n "$Newest" ] || return 0
    Newest="$(basename "$Newest")"
    Named="$(tr ',' '\n' < "$Modules" | grep -o "libUnrealEditor-${TRACE_PROJECT_NAME}-[0-9]*\.dylib" | head -1)"
    [ -n "$Named" ] || return 0

    [ "$Named" = "$Newest" ] && return 0

    trace_warn "Module metadata is STALE: UnrealEditor.modules names ${Named}, but ${Newest} is on disk."
    trace_warn "The engine would load the OLD binary. Re-running the build to force WriteMetadata..."
    if trace_run "$BUILD_SH" "${ARGS[@]}" >/dev/null 2>&1; then
        Named="$(tr ',' '\n' < "$Modules" | grep -o "libUnrealEditor-${TRACE_PROJECT_NAME}-[0-9]*\.dylib" | head -1)"
        if [ "$Named" = "$Newest" ]; then
            trace_msg "Module metadata repaired — now names ${Named}."
            return 0
        fi
    fi
    trace_err "Module metadata is STILL stale (${Named} vs ${Newest} on disk)."
    trace_err "You would be running an OLD binary. Delete Binaries/ and Intermediate/ and rebuild."
    return 1
}

if [ "$TRACE_DRY_RUN" != "1" ] && [ "$DO_CLEAN" != "1" ]; then
    trace_check_module_metadata || exit 1
fi

case "$TARGET" in
    TraceEditor)
        trace_msg "Next: open the project, or run a listen server with Scripts/run-listen-server.sh"
        ;;
    TraceServer)
        trace_msg "Next: Scripts/run-dedicated-server.sh --no-build"
        ;;
esac
