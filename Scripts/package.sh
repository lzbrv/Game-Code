#!/usr/bin/env bash
# ==============================================================================
# Trace — package.sh
#
# Cooks content and produces a standalone, runnable game build — the thing you
# send to a playtester, as opposed to the thing Scripts/build.sh produces, which
# is a binary that links.
#
# THE DIFFERENCE MATTERS AND HAS BITTEN THIS PROJECT ALREADY. Until this script
# existed, "both build configs green" meant Scripts/build.sh --prove-shipping had
# compiled and linked Binaries/Mac/Trace-Mac-Shipping. That binary CANNOT RUN: a
# UE game target with no cooked content on disk exits immediately. See
# docs/KNOWN_LIMITATIONS.md item 29. Linking is not shipping. This script is the
# step that was missing.
#
# It wraps exactly one command, which it prints first:
#
#   "$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
#       -project="<repo>/Trace.uproject" -noP4 -utf8output \
#       -platform=Mac -clientconfig=Shipping \
#       -build -cook -stage -pak -package -archive \
#       -archivedirectory="<output>" -nocompileeditor
#
# Nothing here is magic. If the script ever gets in your way, copy the printed
# command and run it yourself.
#
# WINDOWS IS NOT BUILDABLE FROM THIS MACHINE, and that is not a bug in this
# script. Unreal does not cross-compile a Windows game from macOS, and a
# macOS engine install ships no Win64 binaries at all — check for yourself:
#     ls "$UE_ROOT/Engine/Binaries/"      # DotNET, Mac, ThirdParty. No Win64.
# The Windows build has to be run from a Windows checkout with a Windows engine.
# Passing --platform Win64 here is refused up front rather than failing forty
# minutes into a cook.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

CONFIG="Shipping"
PLATFORM=""
OUTPUT=""
DO_PAK=1
DO_COOK=1
DO_BUILD=1
DO_ITERATE=0
EXTRA_ARGS=()

# Default output lives outside the repo tree's committed paths. Saved/ is already
# gitignored, so an accidental `git add -A` cannot pick up a 2 GB build.
DEFAULT_OUTPUT="${TRACE_PROJECT_ROOT}/Saved/Packaged"

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} package — cook content and produce a runnable game build

USAGE
  Scripts/package.sh [options] [-- <extra RunUAT args>]

OPTIONS
  -o, --output <dir>      Archive directory for the finished build.
                          Default: ${DEFAULT_OUTPUT}
                          The build lands in <dir>/<Platform>/.
  -c, --config <name>     Client configuration. Default: Shipping
                          One of: Debug | DebugGame | Development | Test | Shipping
                          Use Development when you need logs — Shipping compiles
                          logging out, which is why a broken Shipping build is
                          silent (docs/KNOWN_LIMITATIONS.md item 29).
  -p, --platform <name>   Target platform. Default: this host (${TRACE_HOST_PLATFORM})
      --no-pak            Stage loose cooked files instead of a .pak. Slower to
                          load, but you can see and diff what actually cooked.
      --skip-cook         Reuse the existing cook. Only valid if one exists.
      --skip-build        Do not compile the game target; use what is on disk.
      --iterate           Iterative cook — only recook what changed. Much faster
                          on a re-run, and occasionally wrong; if a packaged run
                          disagrees with the editor, re-cook without this first.
  -n, --dry-run           Print the command that would run; run nothing
  -h, --help              This text

ENVIRONMENT
  UE_ROOT                 Engine install to use. Default on macOS:
                          /Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}

WHAT COMES OUT
  macOS:  <output>/Mac/${TRACE_PROJECT_NAME}-Mac-<Config>.app   — double-clickable bundle
          (Development is the exception and gets the bare name ${TRACE_PROJECT_NAME}.app)
  The cooked content is INSIDE the bundle, at
          <bundle>/Contents/UE/${TRACE_PROJECT_NAME}/Content/Paks/*.pak + *.ucas + *.utoc
  The engine content it needs is in there too, so the bundle is self-contained and
  does not read anything out of your engine install or this repository.

HOW IT DECIDES IT WORKED
  Three gates, all of which must pass, because none of them is sufficient alone:
    1. RunUAT exits 0.
    2. RunUAT printed the literal string "BUILD SUCCESSFUL". UnrealBuildTool has
       been observed exiting 0 after a segfault with no verdict line at all (see
       the long comment in Scripts/build.sh), so an exit code is a hint.
    3. The finished bundle exists AND contains cooked content. This is the gate
       that matters: gates 1 and 2 both passed historically on builds that
       produced no cooked content whatsoever and therefore could not start.

EXAMPLES
  Scripts/package.sh                                # Shipping Mac build, default output
  Scripts/package.sh -o /tmp/trace-build            # somewhere else
  Scripts/package.sh -c Development                 # a build that can print a log
  Scripts/package.sh --iterate                      # fast re-cook after a content change
  Scripts/package.sh -n                             # show the RunUAT command, run nothing

AFTERWARDS
  Run it:     open <output>/Mac/${TRACE_PROJECT_NAME}.app
  Sending it to somebody? The bundle is UNSIGNED. Read docs/PLAYTEST.md — macOS
  quarantines it and the recipient needs one command to clear that.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -o|--output)   [ $# -ge 2 ] || trace_die "--output needs a value"; OUTPUT="$2"; shift 2 ;;
        -c|--config)   [ $# -ge 2 ] || trace_die "--config needs a value"; CONFIG="$2"; shift 2 ;;
        -p|--platform) [ $# -ge 2 ] || trace_die "--platform needs a value"; PLATFORM="$2"; shift 2 ;;
        --no-pak)      DO_PAK=0; shift ;;
        --skip-cook)   DO_COOK=0; shift ;;
        --skip-build)  DO_BUILD=0; shift ;;
        --iterate)     DO_ITERATE=1; shift ;;
        -n|--dry-run)  TRACE_DRY_RUN=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        --)            shift; while [ $# -gt 0 ]; do EXTRA_ARGS+=("$1"); shift; done ;;
        -*)            trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
        *)             trace_err "Unexpected argument: $1"; echo; usage; exit 2 ;;
    esac
done
export TRACE_DRY_RUN="${TRACE_DRY_RUN:-0}"

# Validate up front. A typo here otherwise surfaces forty minutes into a cook.
case "$CONFIG" in
    Debug|DebugGame|Development|Test|Shipping) : ;;
    *) trace_die "Unknown configuration '${CONFIG}'. Expected Debug, DebugGame, Development, Test or Shipping." ;;
esac
[ -n "$PLATFORM" ] || PLATFORM="$TRACE_HOST_PLATFORM"
[ -n "$OUTPUT" ] || OUTPUT="$DEFAULT_OUTPUT"

# ------------------------------------------------------------------------------
# Refuse the cross-compile that cannot work, with the reason, immediately.
#
# This is not defensiveness. A macOS engine install has no Win64 toolchain and no
# Win64 engine binaries, so BuildCookRun -platform=Win64 here fails — but it
# fails LATE and with a message about a missing target receipt, which reads like
# a project bug. Say the true thing instead.
# ------------------------------------------------------------------------------
if [ "$PLATFORM" != "$TRACE_HOST_PLATFORM" ]; then
    trace_err "Cannot package ${PLATFORM} from a ${TRACE_HOST_PLATFORM} host."
    trace_err ""
    trace_err "Unreal does not cross-compile a game for a desktop platform it is not running on."
    trace_err "A macOS engine install ships no ${PLATFORM} binaries at all — see for yourself:"
    trace_err "    ls \"\$UE_ROOT/Engine/Binaries/\""
    trace_err ""
    if [ "$PLATFORM" = "Win64" ]; then
        trace_err "For a Windows build, run this from a Windows checkout with a Windows engine:"
        trace_err "    Engine\\Build\\BatchFiles\\RunUAT.bat BuildCookRun ^"
        trace_err "        -project=C:\\path\\to\\Trace.uproject -noP4 -utf8output ^"
        trace_err "        -platform=Win64 -clientconfig=${CONFIG} ^"
        trace_err "        -build -cook -stage -pak -package -archive ^"
        trace_err "        -archivedirectory=C:\\path\\to\\out -nocompileeditor"
    fi
    exit 2
fi

trace_require_uproject
trace_resolve_engine
trace_check_toolchain

RUNUAT="${UE_ROOT}/Engine/Build/BatchFiles/RunUAT.sh"
[ -x "$RUNUAT" ] || trace_die "RunUAT.sh not found or not executable at: ${RUNUAT}"

# ------------------------------------------------------------------------------
# The maps have to exist BEFORE the cook, not after.
#
# Config/DefaultGame.ini lists three maps under MapsToCook. Two of them
# (Arena, MainMenu) are generated by Scripts/generate-map.sh rather than
# committed, so a fresh clone has no .umap for them. The cooker's behaviour when
# a MapsToCook entry is missing is to WARN and carry on, which produces a build
# that installs, launches, and then cannot open its own default map.
#
# So check here, where the message can still be useful.
# ------------------------------------------------------------------------------
MISSING_MAPS=""
for MapPath in $(grep -Eo '\+MapsToCook=\(FilePath="[^"]+"\)' "${TRACE_PROJECT_ROOT}/Config/DefaultGame.ini" 2>/dev/null \
                 | sed -E 's/.*FilePath="([^"]+)".*/\1/'); do
    case "$MapPath" in
        /Game/*) MapRel="${MapPath#/Game/}" ;;
        *) continue ;;
    esac
    if [ ! -f "${TRACE_PROJECT_ROOT}/Content/${MapRel}.umap" ]; then
        MISSING_MAPS="${MISSING_MAPS} ${MapPath}"
    fi
done
if [ -n "$MISSING_MAPS" ]; then
    trace_err "Maps listed in MapsToCook do not exist on disk:${MISSING_MAPS}"
    trace_err "The cook would warn about these and carry on, and the finished build would fail"
    trace_err "to open its own default map. Generate them first:"
    trace_err "    ${TRACE_SCRIPT_DIR}/generate-map.sh"
    exit 1
fi

# ------------------------------------------------------------------------------
# Build the RunUAT command line.
# ------------------------------------------------------------------------------
ARGS=(BuildCookRun
      -project="$TRACE_UPROJECT"
      -noP4
      -utf8output
      -platform="$PLATFORM"
      -clientconfig="$CONFIG"
      -stage
      -package
      -archive
      -archivedirectory="$OUTPUT"
      -nocompileeditor)

# `[ x = y ] && ARGS+=(...)` returns 1 on the false branch and `set -e` kills the
# script. Always use a full `if` for conditional appends. (Same trap build.sh
# documents; it is easy to reintroduce.)
if [ "$DO_BUILD" = "1" ]; then ARGS+=(-build); else ARGS+=(-skipbuild); fi
if [ "$DO_COOK"  = "1" ]; then ARGS+=(-cook);  else ARGS+=(-skipcook);  fi
if [ "$DO_PAK"   = "1" ]; then ARGS+=(-pak);   fi
if [ "$DO_ITERATE" = "1" ]; then ARGS+=(-iterate); fi
ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

trace_msg "Packaging ${TRACE_PROJECT_NAME} | ${PLATFORM} | ${CONFIG} client"
trace_msg "Output: ${TRACE_C_BOLD}${OUTPUT}${TRACE_C_OFF}"
trace_msg "This is slow — a first cook of this project takes tens of minutes. It is not hung."

mkdir -p "$OUTPUT" 2>/dev/null || true

START="$(date +%s)"

# ------------------------------------------------------------------------------
# GATE 1 and GATE 2 — exit code, and RunUAT's own verdict line.
#
# Same reasoning as Scripts/build.sh: Epic's exit code is one signal and Epic's
# own printed verdict is another, and this project has already been burned by a
# tool exiting 0 without ever reaching a verdict. Output is tee'd rather than
# captured so a 40-minute cook still streams live.
# ------------------------------------------------------------------------------
PACKAGE_LOG="$(mktemp -t trace-package)"
set +e
trace_run "$RUNUAT" "${ARGS[@]}" 2>&1 | tee "$PACKAGE_LOG"
PACKAGE_STATUS=${PIPESTATUS[0]}
set -e
ELAPSED=$(( $(date +%s) - START ))

if [ "$TRACE_DRY_RUN" = "1" ]; then
    rm -f "$PACKAGE_LOG"
    exit 0
fi

if [ "$PACKAGE_STATUS" != "0" ]; then
    trace_err "Packaging failed (exit ${PACKAGE_STATUS}) after ${ELAPSED}s. See the output above."
    grep -nE "ERROR:|Error:|AutomationException|Fatal|Segmentation fault" "$PACKAGE_LOG" | head -10 >&2 || true
    trace_err "Full log kept at: ${PACKAGE_LOG}"
    exit "$PACKAGE_STATUS"
fi

# "BUILD SUCCESSFUL" is AutomationTool's own verdict. Its absence on a zero exit
# means UAT never got as far as having one.
if ! grep -q "BUILD SUCCESSFUL" "$PACKAGE_LOG"; then
    trace_err "RunUAT exited 0 but never printed 'BUILD SUCCESSFUL'."
    trace_err "That means it did not finish. Anything in ${OUTPUT} is from an EARLIER run."
    grep -nE "ERROR:|AutomationException|Fatal|Segmentation fault" "$PACKAGE_LOG" | head -10 >&2 || true
    trace_err "Full log kept at: ${PACKAGE_LOG}"
    exit 1
fi

# ------------------------------------------------------------------------------
# GATE 3 — THE ONE THAT ACTUALLY MATTERS.
#
# Gates 1 and 2 are about whether the tool finished. This one is about whether
# what it produced can run, and it exists because of a specific measured failure:
# the project shipped a Shipping binary that linked cleanly, passed every check
# anyone had, and could not start, because there was no cooked content anywhere
# on disk (docs/KNOWN_LIMITATIONS.md item 29). A green tool is not a build.
#
# So: find the bundle, and require cooked content inside it. Under -pak that is
# a .pak/.utoc; under --no-pak it is loose .uasset files. Zero of both means the
# cooker produced nothing and the "successful" build is a shell that will exit
# at launch.
# ------------------------------------------------------------------------------
if [ "$PLATFORM" = "Mac" ]; then
    # THE BUNDLE IS NOT ALWAYS CALLED Trace.app, and hardcoding that name cost a
    # run. UBT names a game bundle after its TARGET RECEIPT, and the receipt name
    # carries the platform and configuration for every config except Development:
    #
    #     Shipping    -> Trace-Mac-Shipping.app
    #     Test        -> Trace-Mac-Test.app
    #     Development -> Trace.app          (the "default" config gets the bare name)
    #
    # So resolve it: try the suffixed spelling, then the bare one, then fall back
    # to whatever single .app is actually there. Listing the directory in the
    # failure arm matters more than the guess — that is what told us the real name.
    APP=""
    for Candidate in \
        "${OUTPUT}/Mac/${TRACE_PROJECT_NAME}-${PLATFORM}-${CONFIG}.app" \
        "${OUTPUT}/Mac/${TRACE_PROJECT_NAME}.app"; do
        if [ -d "$Candidate" ]; then APP="$Candidate"; break; fi
    done
    if [ -z "$APP" ]; then
        APP="$(find "${OUTPUT}/Mac" -maxdepth 1 -name '*.app' 2>/dev/null | head -1)"
    fi
    if [ -z "$APP" ] || [ ! -d "$APP" ]; then
        trace_err "RunUAT reported success but there is no .app bundle under:"
        trace_err "  ${OUTPUT}/Mac"
        trace_err "Contents of ${OUTPUT}:"
        ls -laR "$OUTPUT" >&2 2>/dev/null || trace_err "  (the output directory does not exist)"
        exit 1
    fi

    # The executable inside the bundle is named after the bundle, not the project.
    EXE="${APP}/Contents/MacOS/$(basename "${APP%.app}")"
    [ -x "$EXE" ] || trace_die "No executable inside the bundle at ${EXE}."

    # Cooked content, counted. Both spellings, because -pak and --no-pak produce
    # different ones and a check that only knows about the one you happened to
    # run is a check that passes by accident on the other.
    PAK_COUNT="$(find "$APP" \( -name '*.pak' -o -name '*.utoc' \) 2>/dev/null | wc -l | tr -d ' ')"
    UASSET_COUNT="$(find "$APP" -name '*.uasset' 2>/dev/null | wc -l | tr -d ' ')"
    if [ "${PAK_COUNT:-0}" = "0" ] && [ "${UASSET_COUNT:-0}" = "0" ]; then
        trace_err "The bundle contains NO COOKED CONTENT — zero .pak, zero .utoc, zero .uasset."
        trace_err "  ${APP}"
        trace_err "It will exit immediately at launch. This is exactly the failure recorded as"
        trace_err "item 29 in docs/KNOWN_LIMITATIONS.md: a build that links, reports success, and"
        trace_err "cannot start. Check the cook stage of the log for 'Cook failed' or a missing map."
        trace_err "Full log kept at: ${PACKAGE_LOG}"
        exit 1
    fi

    SIZE="$(du -sh "$APP" 2>/dev/null | cut -f1 | tr -d ' ')"
    trace_msg "Success in ${ELAPSED}s."
    trace_msg "Bundle:  ${TRACE_C_BOLD}${APP}${TRACE_C_OFF}  (${SIZE})"
    if [ "${PAK_COUNT:-0}" != "0" ]; then
        trace_msg "Cooked content: ${PAK_COUNT} pak/utoc file(s) inside the bundle."
    else
        trace_msg "Cooked content: ${UASSET_COUNT} loose .uasset file(s) inside the bundle."
    fi

    # Signing status is a distribution fact the recipient will hit within ten
    # seconds of double-clicking, so say it here rather than letting them find out.
    if codesign -dv "$APP" >/dev/null 2>&1; then
        SIGNER="$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=//p' | head -1)"
        if [ -n "$SIGNER" ]; then
            trace_msg "Code signature: signed by ${SIGNER}"
        else
            trace_msg "Code signature: ad-hoc only (no Developer ID). Gatekeeper WILL block this"
            trace_msg "                on another Mac. See docs/PLAYTEST.md."
        fi
    else
        trace_warn "The bundle is UNSIGNED. Gatekeeper will refuse to open it on another Mac."
        trace_warn "Recipients need:  xattr -dr com.apple.quarantine $(basename "$APP")"
        trace_warn "See docs/PLAYTEST.md for the full instructions to send with it."
    fi

    rm -f "$PACKAGE_LOG"
    trace_msg "Next: open \"${APP}\""
    trace_msg "      or host on this machine's LAN/Tailscale address with:"
    trace_msg "      \"${EXE}\" ${TRACE_DEFAULT_MAP}?listen -port=${TRACE_DEFAULT_PORT}"
else
    rm -f "$PACKAGE_LOG"
    trace_msg "Success in ${ELAPSED}s. Build archived under ${OUTPUT}."
fi
