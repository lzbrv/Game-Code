#!/usr/bin/env bash
# ==============================================================================
# Trace — shared shell helpers.
#
# SOURCE this file, do not execute it:
#     . "$(dirname "$0")/_trace_common.sh"
#
# Responsibilities:
#   * locate the project root and Trace.uproject
#   * locate the Unreal Engine installation (UE_ROOT), with a clear, actionable
#     error instead of "No such file or directory" when it is missing
#   * echo every command before running it, so reading the terminal teaches you
#     the raw UnrealBuildTool / editor command line the wrapper is hiding
#
# Compatibility: written for bash 3.2, which is what /bin/bash still is on macOS.
# No associative arrays, no ${var,,}, no `local -n`.
# ==============================================================================

# Guard against being executed rather than sourced.
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    echo "_trace_common.sh is a library; source it, don't run it." >&2
    exit 64
fi

# ------------------------------------------------------------------------------
# Paths
# ------------------------------------------------------------------------------
TRACE_SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
TRACE_PROJECT_ROOT="$(cd -- "${TRACE_SCRIPT_DIR}/.." && pwd -P)"
TRACE_PROJECT_NAME="Trace"
TRACE_UPROJECT="${TRACE_PROJECT_ROOT}/${TRACE_PROJECT_NAME}.uproject"
TRACE_DEFAULT_MAP="/Game/Maps/Arena"
TRACE_DEFAULT_PORT="7777"
# Engine version this project is pinned to. Kept in sync with
# "EngineAssociation" in Trace.uproject by hand — if you bump one, bump both.
TRACE_ENGINE_VERSION="5.8"

export TRACE_SCRIPT_DIR TRACE_PROJECT_ROOT TRACE_UPROJECT

# ------------------------------------------------------------------------------
# Pretty output (colour only when stdout is a terminal)
# ------------------------------------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    TRACE_C_DIM=$'\033[2m'; TRACE_C_BOLD=$'\033[1m'
    TRACE_C_RED=$'\033[31m'; TRACE_C_YEL=$'\033[33m'; TRACE_C_CYN=$'\033[36m'
    TRACE_C_OFF=$'\033[0m'
else
    TRACE_C_DIM=''; TRACE_C_BOLD=''; TRACE_C_RED=''; TRACE_C_YEL=''
    TRACE_C_CYN=''; TRACE_C_OFF=''
fi

trace_msg()  { printf '%s\n' "${TRACE_C_CYN}[trace]${TRACE_C_OFF} $*"; }
trace_warn() { printf '%s\n' "${TRACE_C_YEL}[trace] warning:${TRACE_C_OFF} $*" >&2; }
trace_err()  { printf '%s\n' "${TRACE_C_RED}[trace] error:${TRACE_C_OFF} $*" >&2; }

# trace_die <message...> — print to stderr and exit 1.
trace_die() { trace_err "$@"; exit 1; }

# ------------------------------------------------------------------------------
# Command echoing
# ------------------------------------------------------------------------------

# trace_quote <arg> — shell-quote a single argument only when it needs it, so the
# echoed command lines stay readable but remain copy-paste safe.
trace_quote() {
    case "$1" in
        ''|*[!A-Za-z0-9_@%+=:,./-]*)
            # Wrap in single quotes, escaping any embedded single quote.
            printf "'%s'" "${1//\'/\'\\\'\'}"
            ;;
        *) printf '%s' "$1" ;;
    esac
}

# trace_print_cmd <cmd> [args...] — show exactly what is about to run.
trace_print_cmd() {
    local out='' a
    for a in "$@"; do
        out="${out} $(trace_quote "$a")"
    done
    printf '%s\n' "${TRACE_C_DIM}==> ${out# }${TRACE_C_OFF}"
}

# trace_run <cmd> [args...] — echo, then run. Honours TRACE_DRY_RUN=1.
trace_run() {
    trace_print_cmd "$@"
    if [ "${TRACE_DRY_RUN:-0}" = "1" ]; then
        trace_msg "dry run — not executed"
        return 0
    fi
    "$@"
}

# ------------------------------------------------------------------------------
# Host platform → Unreal's platform name
# ------------------------------------------------------------------------------
trace_host_platform() {
    case "$(uname -s)" in
        Darwin) printf 'Mac' ;;
        Linux)  printf 'Linux' ;;
        MINGW*|MSYS*|CYGWIN*)
            # Git Bash / MSYS on Windows. Native .bat equivalents live beside this
            # file, so send people there rather than making them hand-type commands.
            trace_err "These shell scripts are for macOS and Linux."
            trace_err "You are on Windows -- use the .bat equivalents from cmd.exe instead:"
            trace_err "  build          : Scripts\\build.bat"
            trace_err "  generate map   : Scripts\\generate-map.bat"
            trace_err "  listen server  : Scripts\\run-listen-server.bat"
            trace_err "  client         : Scripts\\run-client.bat <host-ip>"
            trace_err "  git-lfs setup  : Scripts\\setup-lfs.bat"
            trace_err ""
            trace_err "NOTE: there is deliberately no working dedicated-server path here."
            trace_err "A launcher-installed engine refuses to build server targets:"
            trace_err "  \"Server targets are not currently supported from this engine distribution.\""
            trace_err "That needs a source build of Unreal. Host a listen server instead --"
            trace_err "see docs/NETWORKING.md."
            exit 2
            ;;
        *) trace_die "Unsupported host OS: $(uname -s)" ;;
    esac
}

TRACE_HOST_PLATFORM="$(trace_host_platform)"

# ------------------------------------------------------------------------------
# Engine discovery
# ------------------------------------------------------------------------------

# An engine root is valid if it contains the BatchFiles UnrealBuildTool wrappers.
trace_is_engine_root() {
    [ -n "${1:-}" ] && [ -d "$1/Engine/Build/BatchFiles" ]
}

# List UE_5.x directories under a launcher root, newest minor version first.
# `sort -t. -k2 -n -r` keys on the digits after the dot, so UE_5.10 correctly
# sorts ahead of UE_5.8 (a plain lexical sort gets that backwards).
_trace_list_engine_dirs() {
    local root="$1" name
    [ -d "$root" ] || return 0
    ls -1 "$root" 2>/dev/null \
        | grep -E '^UE_5\.[0-9]+$' \
        | sort -t. -k2 -n -r \
        | while IFS= read -r name; do printf '%s\n' "${root}/${name}"; done
}

# Candidate engine roots for this host, in priority order, deduplicated (the
# pinned version is named explicitly AND found by the launcher-root scan).
_trace_engine_candidates() {
    _trace_engine_candidates_raw | awk '!seen[$0]++'
}

_trace_engine_candidates_raw() {
    if [ "$TRACE_HOST_PLATFORM" = "Mac" ]; then
        # Pinned version first, then any other launcher install, then source builds.
        printf '%s\n' "/Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}"
        _trace_list_engine_dirs "/Users/Shared/Epic Games"
        _trace_list_engine_dirs "${HOME}/Epic Games"
        _trace_list_engine_dirs "/Applications/Epic Games"
        printf '%s\n' "${HOME}/UnrealEngine"
    else
        _trace_list_engine_dirs "${HOME}"
        printf '%s\n' "${HOME}/UnrealEngine"
        printf '%s\n' "/opt/UnrealEngine"
        printf '%s\n' "/usr/local/UnrealEngine"
    fi
}

# trace_resolve_engine — sets UE_ROOT. Resolution order:
#   1. $UE_ROOT from the environment
#   2. <repo>/.ue-root  (single line, gitignored, per-developer override)
#   3. the well-known install locations for this host
trace_resolve_engine() {
    local source_desc candidate

    if [ -n "${UE_ROOT:-}" ]; then
        source_desc="the UE_ROOT environment variable"
        if ! trace_is_engine_root "$UE_ROOT"; then
            trace_err "UE_ROOT is set to a path that is not an Unreal Engine install:"
            trace_err "  UE_ROOT=${UE_ROOT}"
            trace_err "Expected to find: ${UE_ROOT}/Engine/Build/BatchFiles"
            _trace_engine_help
            exit 1
        fi
    elif [ -f "${TRACE_PROJECT_ROOT}/.ue-root" ]; then
        source_desc="${TRACE_PROJECT_ROOT}/.ue-root"
        # Strip trailing whitespace/newline; tolerate a trailing slash.
        UE_ROOT="$(sed -e 's/[[:space:]]*$//' -e 's:/*$::' "${TRACE_PROJECT_ROOT}/.ue-root" | head -n 1)"
        if ! trace_is_engine_root "$UE_ROOT"; then
            trace_err ".ue-root points at a path that is not an Unreal Engine install:"
            trace_err "  ${UE_ROOT}"
            _trace_engine_help
            exit 1
        fi
    else
        source_desc="auto-detection"
        UE_ROOT=""
        while IFS= read -r candidate; do
            [ -n "$candidate" ] || continue
            if trace_is_engine_root "$candidate"; then UE_ROOT="$candidate"; break; fi
        done <<EOF
$(_trace_engine_candidates)
EOF
        if [ -z "$UE_ROOT" ]; then
            trace_err "Could not find an Unreal Engine ${TRACE_ENGINE_VERSION} installation."
            _trace_engine_looked_in
            _trace_engine_help
            exit 1
        fi
    fi

    export UE_ROOT
    trace_msg "Engine: ${TRACE_C_BOLD}${UE_ROOT}${TRACE_C_OFF} ${TRACE_C_DIM}(from ${source_desc})${TRACE_C_OFF}"

    case "$UE_ROOT" in
        *"UE_${TRACE_ENGINE_VERSION}"*) : ;;
        *) trace_warn "This project is pinned to UE ${TRACE_ENGINE_VERSION}. The engine above does not look like that version — expect compile errors or an engine-association prompt." ;;
    esac
}

_trace_engine_looked_in() {
    local candidate
    trace_err "Looked in (in order):"
    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        if [ -d "$candidate" ]; then
            trace_err "  - ${candidate}   [directory exists but has no Engine/Build/BatchFiles]"
        else
            trace_err "  - ${candidate}"
        fi
    done <<EOF
$(_trace_engine_candidates)
EOF
}

_trace_engine_help() {
    trace_err ""
    trace_err "Fix it with ONE of:"
    trace_err "  1. export UE_ROOT=\"/Users/Shared/Epic Games/UE_${TRACE_ENGINE_VERSION}\""
    trace_err "  2. echo \"/path/to/UE_${TRACE_ENGINE_VERSION}\" > \"${TRACE_PROJECT_ROOT}/.ue-root\"   (per-developer, gitignored)"
    trace_err "  3. Install Unreal Engine ${TRACE_ENGINE_VERSION} from the Epic Games Launcher"
    trace_err "     (Library -> + -> pick ${TRACE_ENGINE_VERSION}). Budget ~60 GB, more with debug symbols."
    trace_err "     No launcher yet?   brew install --cask epic-games"
    trace_err ""
    trace_err "If the launcher is still downloading, the UE_${TRACE_ENGINE_VERSION} folder exists but only"
    trace_err "contains .egstore — that is the case this check is catching. Let it finish."
}

# trace_require_uproject — the scripts are useless without the project file.
trace_require_uproject() {
    [ -f "$TRACE_UPROJECT" ] || trace_die \
        "Trace.uproject not found at ${TRACE_UPROJECT}. Run these scripts from inside the Trace repository."
}

# ------------------------------------------------------------------------------
# Engine executables
# ------------------------------------------------------------------------------

# trace_find_first_executable <path>... — echo the first candidate that exists
# and is executable. Returns 1 if none do.
trace_find_first_executable() {
    local p
    for p in "$@"; do
        if [ -x "$p" ]; then printf '%s' "$p"; return 0; fi
    done
    return 1
}

# trace_editor_binary — the GUI/editor executable (also used to run -game).
trace_editor_binary() {
    local bin
    if [ "$TRACE_HOST_PLATFORM" = "Mac" ]; then
        bin="$(trace_find_first_executable \
            "${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
            "${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor")" || true
    else
        bin="$(trace_find_first_executable \
            "${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor")" || true
    fi
    if [ -z "${bin:-}" ]; then
        trace_err "No UnrealEditor executable under ${UE_ROOT}/Engine/Binaries/${TRACE_HOST_PLATFORM}."
        trace_err "The engine install looks incomplete. Verify it in the Epic Games Launcher"
        trace_err "(Library -> UE_${TRACE_ENGINE_VERSION} -> Options -> Verify)."
        exit 1
    fi
    printf '%s' "$bin"
}

# trace_editor_cmd_binary — the console/commandlet executable (-run=..., headless).
trace_editor_cmd_binary() {
    local bin
    if [ "$TRACE_HOST_PLATFORM" = "Mac" ]; then
        bin="$(trace_find_first_executable \
            "${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd" \
            "${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor-Cmd.app/Contents/MacOS/UnrealEditor-Cmd" \
            "${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
            "${UE_ROOT}/Engine/Binaries/Mac/UnrealEditor")" || true
    else
        bin="$(trace_find_first_executable \
            "${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor-Cmd" \
            "${UE_ROOT}/Engine/Binaries/Linux/UnrealEditor")" || true
    fi
    if [ -z "${bin:-}" ]; then
        trace_err "No UnrealEditor-Cmd executable under ${UE_ROOT}/Engine/Binaries/${TRACE_HOST_PLATFORM}."
        trace_err "The engine install looks incomplete — verify it in the Epic Games Launcher."
        exit 1
    fi
    printf '%s' "$bin"
}

# trace_build_script — Engine/Build/BatchFiles/<Host>/Build.sh
trace_build_script() {
    local s="${UE_ROOT}/Engine/Build/BatchFiles/${TRACE_HOST_PLATFORM}/Build.sh"
    [ -x "$s" ] || trace_die "Build.sh not found or not executable at: ${s}"
    printf '%s' "$s"
}

# ------------------------------------------------------------------------------
# Toolchain sanity (macOS): the single most common "cannot build / cannot compile
# shaders for Metal" cause is xcode-select pointing at the Command Line Tools,
# which Homebrew and other installers silently repoint it to.
# ------------------------------------------------------------------------------
trace_check_toolchain() {
    [ "$TRACE_HOST_PLATFORM" = "Mac" ] || return 0
    local dev
    if ! command -v xcode-select >/dev/null 2>&1; then
        trace_warn "xcode-select not found. Unreal needs the full Xcode app (not just the Command Line Tools) to compile Metal shaders."
        return 0
    fi
    dev="$(xcode-select --print-path 2>/dev/null || true)"
    case "$dev" in
        *Xcode*.app/Contents/Developer|*Xcode.app/Contents/Developer) : ;;
        '')
            trace_warn "xcode-select has no active developer directory."
            trace_warn "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer"
            ;;
        *)
            trace_warn "xcode-select points at: ${dev}"
            trace_warn "That is the Command Line Tools, not the full Xcode app. Unreal needs Xcode itself"
            trace_warn "for the Metal shader toolchain ('requires Xcode to compile shaders for Metal')."
            trace_warn "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer"
            ;;
    esac
}

# ------------------------------------------------------------------------------
# Map / URL helpers
# ------------------------------------------------------------------------------

# trace_warn_if_map_missing <'/Game/Maps/Arena'> — the map is generated, not
# committed as a .uasset, so a fresh clone will not have it until you run
# Scripts/generate-map.sh.
trace_warn_if_map_missing() {
    local map="$1" rel
    case "$map" in
        /Game/*) rel="${map#/Game/}" ;;
        *) return 0 ;;
    esac
    rel="${rel%%\?*}"   # drop any ?listen / ?options suffix
    if [ ! -f "${TRACE_PROJECT_ROOT}/Content/${rel}.umap" ]; then
        trace_warn "Content/${rel}.umap does not exist yet."
        trace_warn "Generate it headlessly:  ${TRACE_SCRIPT_DIR}/generate-map.sh"
    fi
}
