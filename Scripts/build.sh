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

# ------------------------------------------------------------------------------
# Guard: anonymous-namespace collisions under the unity/jumbo build
#
# ("unity build" here is the C++ compilation technique -- UBT concatenates many
# .cpp files into one translation unit to compile faster. Nothing to do with the
# Unity engine.)
#
# Two files may each legally define a private `namespace { UWorld* FindWorld(); }`.
# Concatenated, that is one namespace with two definitions -- MSVC C2084. Which
# files get grouped depends on file count and ordering, so this builds clean on
# macOS and breaks on Windows, which is exactly what happened to a collaborator.
# Catch it here instead.
#
# Skip with TRACE_SKIP_COLLISION_CHECK=1.
# ------------------------------------------------------------------------------
COLLISION_PY="${TRACE_SCRIPT_DIR}/check-jumbo-build-collisions.py"
if [ "${TRACE_SKIP_COLLISION_CHECK:-0}" != "1" ] && [ -f "$COLLISION_PY" ]; then
    if ! python3 "$COLLISION_PY"; then
        trace_die "Anonymous-namespace collisions would break the Windows unity build (see above).
Fix by naming the namespace after its file, e.g. 'namespace TraceFooVerify', rather than
renaming the individual symbol."
    fi
fi

# ------------------------------------------------------------------------------
# The SECOND Windows-only trap this build gates on: a preprocessor directive
# inside a function-like macro's argument list. The standard says undefined;
# clang does the friendly thing, MSVC emits C5101 and then fails the file with
# C2760/C3553/C2059. So it builds clean here and breaks every Windows developer —
# which is how a collaborator found it, not us.
#
# Skip with TRACE_SKIP_PREPROCESSOR_CHECK=1.
# ------------------------------------------------------------------------------
PREPROC_PY="${TRACE_SCRIPT_DIR}/check-preprocessor-in-macro-args.py"
if [ "${TRACE_SKIP_PREPROCESSOR_CHECK:-0}" != "1" ] && [ -f "$PREPROC_PY" ]; then
    if ! python3 "$PREPROC_PY"; then
        trace_die "A preprocessor directive sits inside a macro argument list (see above).
MSVC rejects this and macOS does not, so it would break the Windows build.
Hoist the conditional value into a variable above the call and pass the variable."
    fi
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

# ------------------------------------------------------------------------------
# EPIC'S EXIT CODE IS NOT ENOUGH ON ITS OWN.
#
# Observed on 2026-08-13: UnrealBuildTool died with "ERROR: Segmentation fault"
# and a 19-frame libcoreclr callstack, ran ZERO compile actions, never printed a
# "Result:" line at all -- and Build.sh still returned 0, so this script printed
# "Success" and exited 0. The stale-metadata guard did not catch it either,
# because the PREVIOUS run's dylib was still on disk under the expected name.
# That is exactly the run-the-old-binary trap this file's own comments say has
# cost the project real time twice already.
#
# So: keep the exit code as one signal, and require UBT to have actually said it
# succeeded as another. Output is tee'd rather than captured so the build still
# streams live.
# ------------------------------------------------------------------------------
BUILD_LOG="$(mktemp -t trace-build)"
set +e
trace_run "$BUILD_SH" "${ARGS[@]}" 2>&1 | tee "$BUILD_LOG"
BUILD_STATUS=${PIPESTATUS[0]}
set -e
ELAPSED=$(( $(date +%s) - START ))

if [ "$TRACE_DRY_RUN" = "1" ]; then
    rm -f "$BUILD_LOG"
    exit 0
fi

if [ "$BUILD_STATUS" != "0" ]; then
    trace_err "Build failed (exit ${BUILD_STATUS}). See the output above."
    rm -f "$BUILD_LOG"
    exit "$BUILD_STATUS"
fi

# "Result: Succeeded" is UBT's own verdict. Its absence on a zero exit means UBT
# never got as far as having one -- a crash, a killed process, a segfault.
if ! grep -q "Result: Succeeded" "$BUILD_LOG"; then
    trace_err "UnrealBuildTool exited 0 but never printed 'Result: Succeeded'."
    trace_err "That means it did not finish -- typically a crash or a segfault -- and the"
    trace_err "binary on disk is the PREVIOUS build. Do not trust anything you run now."
    grep -nE "Segmentation fault|ERROR:|Fatal|Unhandled exception" "$BUILD_LOG" | head -5 >&2 || true
    rm -f "$BUILD_LOG"
    exit 1
fi
rm -f "$BUILD_LOG"

trace_msg "Success in ${ELAPSED}s."

# ------------------------------------------------------------------------------
# A GREEN BUILD HERE DOES NOT MEAN A GREEN BUILD FOR ANYBODY ELSE.
#
# On 2026-08-13 a commit shipped TraceAbilitySetLily.cpp — which calls
# SetJumpHeld() / IsJumpHeld() — while leaving Source/Trace/Movement out of the
# staging list. This machine stayed green because the working tree still had the
# uncommitted header. Every Windows developer got five copies of
#     error C2039: 'SetJumpHeld': is not a member of UTraceCharacterMovementComponent
# and could not build the game at all.
#
# The build cannot tell you what you forgot to commit, but it CAN tell you that
# what it just proved is not what your collaborators will pull. That is the whole
# warning: it is about the gap between the two, not about tidy working trees.
# ------------------------------------------------------------------------------
if command -v git >/dev/null 2>&1 && git -C "$TRACE_PROJECT_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    # `|| true` is load-bearing. Under `set -o pipefail` the pipeline takes grep's exit status,
    # and grep exits 1 when it filters EVERYTHING out — i.e. exactly when Source/ and Config/ are
    # CLEAN. `set -e` then killed build.sh here, after it had already printed "Success", so the
    # exit code was inverted: dirty tree = 0, clean tree = 1. Found by the v23 §A4 agent; the
    # spec's claim that "exit code IS trustworthy" was false in precisely this case.
    DIRTY_SOURCE="$(git -C "$TRACE_PROJECT_ROOT" status --porcelain -- Source Config 2>/dev/null \
        | { grep -vE '^\?\?' || true; } | wc -l | tr -d ' ')"
    if [ "${DIRTY_SOURCE:-0}" != "0" ]; then
        trace_warn "This build included ${DIRTY_SOURCE} uncommitted change(s) under Source/ or Config/."
        trace_warn "It therefore proves nothing about what is on the branch. Before you push, commit"
        trace_warn "them and build again — a caller committed without its declaration compiles here"
        trace_warn "and fails for everyone else (see the comment above this check in build.sh)."
    fi
fi

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

    # The hot-reload SUFFIX IS OPTIONAL. A clean build links the unnumbered
    # libUnrealEditor-Trace.dylib; only an in-editor hot reload produces -0042. Globbing for the
    # numbered form ALONE — which this guard did until the v23 integration pass — makes $Newest
    # empty on every clean build, so the whole check returned early and proved nothing. Match both
    # spellings so the guard is live on the layout this project actually has.
    local Newest Named
    Newest="$(ls -t "${BinDir}"/libUnrealEditor-${TRACE_PROJECT_NAME}-[0-9]*.dylib \
                     "${BinDir}"/libUnrealEditor-${TRACE_PROJECT_NAME}.dylib 2>/dev/null | head -1)"
    [ -n "$Newest" ] || return 0
    Newest="$(basename "$Newest")"
    Named="$(tr ',' '\n' < "$Modules" | grep -Eo "libUnrealEditor-${TRACE_PROJECT_NAME}(-[0-9]+)?\.dylib" | head -1)"
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
