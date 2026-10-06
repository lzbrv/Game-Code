#!/usr/bin/env bash
# ==============================================================================
# Trace — import-sounds.sh     (spec v26 §9)
#
# Puts the game's SOUND on the wire. Runs the two halves of Scripts/import_sounds.py:
#
#   1. manifest  Art/Sounds/**/*.wav  (recursive: the footsteps are in Footsteps/)
#                  -> validated and printed (rate, channels, seconds, side,
#                     and the MEASURED peak/RMS dBFS of the actual samples)    (plain python)
#   2. assets    Art/Sounds/<Event>.wav
#                  -> Content/Trace/Audio/S_<Event>.uasset                    (in the editor)
#                  -> Content/Trace/Audio/DA_TraceSoundBank.uasset            (in the editor)
#
# ------------------------------------------------------------------------------
# "MAKE SURE IT'S EASY TO SWAP SOUND EFFECTS IN AND OUT FOR NEW VERSIONS"
# ------------------------------------------------------------------------------
# The owner's requirement, and the whole procedure for a new version of a sound:
#
#     git pull
#     Scripts/lock.sh Content/Trace/Audio/S_Dash.uasset Content/Trace/Audio/DA_TraceSoundBank.uasset
#     cp NewDash.wav Art/Sounds/Dash.wav
#     ./Scripts/import-sounds.sh --only Dash
#
# No C++ edit. No rebuild. The game asks the BANK for a name and gets whatever the
# bank now points at — see Source/Trace/Audio/TraceSoundBank.h.
#
# On Windows the same steps run from plain cmd.exe: Scripts\lock.bat,
# copy /Y, Scripts\import-sounds.bat --only Dash. Same options, same checks.
#
# THE SET OF SOUNDS IS DISCOVERED, NOT LISTED. import_sounds.py globs Art/Sounds
# rather than carrying nine names, so a TENTH wav needs no edit to this script
# either. Its event name is its file stem, and that is the key C++ asks for.
#
# WHAT IS *NOT* DATA: WHICH MACHINES HEAR IT. Game-side (CoreTurnover, Dash,
# Parry, every gunshot, every footstep, Goal, RoccoRipple — everyone nearby)
# versus client-side (Bodyshot, Headshot, CorePickup, Jump, WallJump, ButtonPress,
# Kill — only you) is a networking behaviour and lives in
# Source/Trace/Audio/TraceSoundEvents.cpp. See
# the long comment at the top of TraceSoundEvents.h: an asset that could silently
# turn a multicast into a local play is a way to break the owner's explicit design
# without touching any code.
#
# ------------------------------------------------------------------------------
# THESE WAVS *ARE* IN THIS REPOSITORY, UNLIKE THE FONTS
# ------------------------------------------------------------------------------
# Sofachrome and Erbaum are licensed to the owner for desktop use and are
# gitignored (docs/FONTS.md). These sounds are ours: Art/Sounds/**/*.wav and
# the imported Content/Trace/Audio/*.uasset are BOTH tracked, both through Git
# LFS per .gitattributes.
#
# .uasset is `lockable`, so it is checked out READ-ONLY. Creating a sound asset
# for the first time is fine; RE-importing one over an existing asset needs
# `Scripts/lock.sh Content/Trace/Audio/S_Dash.uasset` first — plus
# Content/Trace/Audio/DA_TraceSoundBank.uasset, which every run re-saves — and that
# is exactly why --only exists: swapping one sound should not need nine locks.
#
# A SAVE THAT FAILS LEAVES THE OLD FILE ON DISK, so "the file exists" proves
# nothing on a re-import. This script therefore checks before the editor starts
# that every .uasset this run re-saves is locked (writable), and afterwards
# believes import_sounds.py's own verdict in the editor's log, not just the
# files: no "done" line, or any error it reported (a save that failed, a
# PCM_LOOP_STEMS sound whose compression could not be set to PCM, an --only name
# with no WAV), is a failed run and exits 1. Scripts/import-sounds.bat does the same.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY. All outputs are committed, exactly like the
# font atlas and the railgun.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

DRY_RUN=0
LIST_ONLY=0
ONLY_SOUNDS=""

# The editor's full output, every line. One fixed name that each run overwrites, like
# %TEMP%\trace-import-sounds.log in Scripts/import-sounds.bat.
IMPORT_LOG="${TMPDIR:-/tmp}"
IMPORT_LOG="${IMPORT_LOG%/}/trace-import-sounds.log"

usage() {
    cat <<EOF
${TRACE_PROJECT_NAME} import-sounds

Imports Art/Sounds/**/*.wav (recursively - the footsteps live in Footsteps/) into
Content/Trace/Audio as USoundWave assets and writes the one asset that maps an
event name to a sound: DA_TraceSoundBank. An event name is the file's STEM; the
folder is filing, not naming.

USAGE
  Scripts/import-sounds.sh [options]

OPTIONS
      --only NAME   Import only this event (repeatable, or comma-separated). The BANK
                    still keeps every other row — it is read-modify-write, never a
                    wholesale replace. Use this to swap ONE sound without touching
                    eight .uasset files you have not locked (.uasset is lockable in
                    .gitattributes, so it is read-only until Scripts/lock.sh says
                    otherwise).
      --list        Validate and print the manifest only. No editor, instant.
  -n, --dry-run     Print what would run; run nothing
  -h, --help        This text

AFTER RUNNING (no C++ rebuild: the game asks the bank, and the bank is data)
  git status Art/Sounds Content/Trace/Audio
  The editor and the Scripts/run-*.sh games pick it up on their next launch;
  Trace.Audio.Reload picks it up in one that is running. Packaged builds keep
  the old sound until ./Scripts/package.sh re-cooks them.

THE RESULT
  Exit 0 only when import_sounds.py says it finished with no errors AND every
  file is on disk. Any '[Trace] ERROR' line means exit 1, even if every file is
  listed ok: on a re-import the file can be the OLD one. The terminal shows the
  [Trace] lines and the editor's own errors; the full output is in
  ${IMPORT_LOG} (each run overwrites it).

IN GAME (drop -nosound, or none of this is audible)
  Trace.Audio.Report      every event: side, which asset it resolved to, the device
  Trace.Audio.Test Dash   fire one event through the real API
  Trace.Audio.Probe       THE evidence: reads the mixer's source count back
  Trace.Audio.Reload      pick up a re-import without relaunching

  spec v29 §1:
  Trace.Audio.Sides       §1a: the nine v26 events still route as v26 shipped them
  Trace.Audio.Loudness    §1b: MEASURES every clip and proves footsteps are quieter
                          (red arm: Trace.Audio.FootstepVolume 1)
  Trace.Audio.Footsteps   §1b: the randomiser and the stride, driven
                          (red arm: Trace.Audio.FootstepRepeatGuard 0)
  Trace.Audio.GunLadder   §1c/§1d/§1e: the clip name per shot, fast and gapped
                          (red arms: Trace.Audio.PistolResetFloor 0, Trace.Audio.ShotWatch 0)
  Trace.Audio.V29Integ    §1f: Goal, Kill and RoccoRipple from their real triggers
  Trace.Audio.Integ       §9: the nine v26 call sites (run it in a MATCH, not the range)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --only)     shift; [ $# -gt 0 ] || trace_die "--only needs an event name (e.g. Dash)"
                    ONLY_SOUNDS="${ONLY_SOUNDS:+${ONLY_SOUNDS},}$1" ;;
        --list)     LIST_ONLY=1 ;;
        -n|--dry-run) DRY_RUN=1 ;;
        -h|--help)  usage; exit 0 ;;
        *) trace_err "Unknown option: $1"; echo; usage; exit 2 ;;
    esac
    shift
done

trace_require_uproject

SRC_DIR="${TRACE_PROJECT_ROOT}/Art/Sounds"
OUT_DIR="${TRACE_PROJECT_ROOT}/Content/Trace/Audio"

[ -d "$SRC_DIR" ] || trace_die "Missing ${SRC_DIR}
That is where the WAVs live. Spec v29 §1 stages twenty-eight there:
    Bodyshot ButtonPress CorePickup CoreTurnover Dash Headshot Jump Parry WallJump
    Goal Kill RoccoRipple PistolShoot1..4 SmgShoot1
    Footsteps/Step1..Step11"

# THE FOOTSTEPS LIVE IN A SUBFOLDER (spec v29 §1b), so every scan below is recursive. The event name
# is still the STEM and never the folder: Art/Sounds/Footsteps/Step7.wav is the event Step7 and the
# asset S_Step7. `find`, not a glob, because bash 3.2 (the macOS system shell this script has to run
# under) has no globstar.
#
# An LFS pointer is a ~130-byte text file starting with 'version https://'. Handing that to the
# importer produces a baffling 'not a valid sound' instead of a useful error. Same guard as
# Scripts/import-font-atlas.sh, which learned it the hard way.
WAV_COUNT=0
while IFS= read -r WAV; do
    [ -e "$WAV" ] || continue
    WAV_COUNT=$((WAV_COUNT + 1))
    if head -c 16 "$WAV" | grep -q '^version https'; then
        trace_die "${WAV} is an unfetched Git LFS pointer, not audio. Run: git lfs pull"
    fi
done <<EOF
$(find "$SRC_DIR" -type f -name '*.wav' | sort)
EOF
[ "$WAV_COUNT" -gt 0 ] || trace_die "No .wav files under ${SRC_DIR}."

# ------------------------------------------------------------------------------
# 1. The manifest — plain python, and it is a real validation pass: it reads each
#    RIFF header rather than trusting the extension, which is the only way to
#    catch a renamed .mp3 (it imports as a zero-length sound that loads fine and
#    plays nothing).
# ------------------------------------------------------------------------------
trace_msg "Manifest ${TRACE_C_BOLD}${WAV_COUNT} wav(s)${TRACE_C_OFF} in Art/Sounds"
if [ "$DRY_RUN" = "1" ]; then
    trace_print_cmd python3 "${TRACE_SCRIPT_DIR}/import_sounds.py"
else
    # It exits non-zero when a declared event has no WAV; that is worth seeing but
    # it must not stop the eight that ARE fine from importing.
    set +e
    TRACE_SOUNDS="${ONLY_SOUNDS}" python3 "${TRACE_SCRIPT_DIR}/import_sounds.py"
    MANIFEST_STATUS=$?
    set -e
    if [ "$MANIFEST_STATUS" != "0" ]; then
        trace_warn "The manifest reported a problem (exit ${MANIFEST_STATUS}). Continuing: the editor"
        trace_warn "still imports every sound that is fine. But the editor run makes the same checks,"
        trace_warn "so expect it to report the same problem and this run to end with exit 1."
    fi
fi

if [ "$LIST_ONLY" = "1" ]; then
    trace_msg "--list: nothing was imported."
    exit 0
fi

# ------------------------------------------------------------------------------
# 2. WAV -> USoundWave -> the bank, inside the editor
#
# -NullRHI is safe here for the same reason it is in Scripts/import-font-atlas.sh:
# importing a sound builds no shader map, so this needs no swap chain. -nosound is
# safe too and is NOT a contradiction — the editor is IMPORTING audio here, not
# playing it, and the import path never opens an output device. (Playing it is
# what the game does, and the game must run WITHOUT -nosound: see Trace.Audio.Probe.)
# ------------------------------------------------------------------------------
trace_resolve_engine
CMD_BIN="$(trace_editor_cmd_binary)"

ARGS=("$TRACE_UPROJECT"
      -run=pythonscript
      "-script=${TRACE_SCRIPT_DIR}/import_sounds.py"
      -unattended
      -nosplash
      -nopause
      -nosound
      -NullRHI
      -stdout
      -FullStdOutLogOutput)

trace_msg "Assets   ${TRACE_C_BOLD}Art/Sounds/*.wav${TRACE_C_OFF} -> /Game/Trace/Audio/"

if [ "$DRY_RUN" = "1" ]; then
    trace_print_cmd "$CMD_BIN" "${ARGS[@]}"
    exit 0
fi

# A re-import over a .uasset nobody has locked cannot save — git-lfs checks every `lockable` file
# out read-only — and that failure is invisible afterwards: the old file is still on disk, so the
# Verifying block below would print ok for a sound that never changed. So check before the editor
# starts. Only what this run re-saves: S_<Event> for each sound it imports, and the bank, which
# every run re-saves. A file that does not exist yet is fine; the first import creates it.
# (A string, not an array: bash 3.2 under `set -u` calls an empty array unbound.)
READ_ONLY=""
while IFS= read -r WAV; do
    [ -e "$WAV" ] || continue
    STEM="$(basename "$WAV" .wav)"
    if [ -z "$ONLY_SOUNDS" ] || printf '%s' ",${ONLY_SOUNDS}," | grep -q ",${STEM},"; then
        Rel="Content/Trace/Audio/S_${STEM}.uasset"
        if [ -e "${TRACE_PROJECT_ROOT}/${Rel}" ] && [ ! -w "${TRACE_PROJECT_ROOT}/${Rel}" ]; then
            READ_ONLY="${READ_ONLY} ${Rel}"
        fi
    fi
done <<EOF
$(find "$SRC_DIR" -type f -name '*.wav' | sort)
EOF
Rel="Content/Trace/Audio/DA_TraceSoundBank.uasset"
if [ -e "${TRACE_PROJECT_ROOT}/${Rel}" ] && [ ! -w "${TRACE_PROJECT_ROOT}/${Rel}" ]; then
    READ_ONLY="${READ_ONLY} ${Rel}"
fi
if [ -n "$READ_ONLY" ]; then
    trace_err "Read-only until you lock them, so the editor could not save over them. After"
    trace_err "git pull, lock them — this is the command — then run this script again:"
    printf '    Scripts/lock.sh%s\n' "$READ_ONLY" >&2
    if [ -z "$ONLY_SOUNDS" ]; then
        trace_err "Without --only this re-imports EVERY sound, so every one needs a lock. To swap"
        trace_err "one sound, lock only it and the bank, and name it:"
        printf '    %s\n' "Scripts/lock.sh Content/Trace/Audio/S_Dash.uasset Content/Trace/Audio/DA_TraceSoundBank.uasset" \
                          "./Scripts/import-sounds.sh --only Dash" >&2
    fi
    exit 1
fi

if [ -n "$ONLY_SOUNDS" ]; then
    export TRACE_SOUNDS="$ONLY_SOUNDS"
    trace_msg "--only ${ONLY_SOUNDS}: no other sound asset will be written."
fi

# THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN — it is non-zero if
# ANY error was logged in the whole session, including engine warnings raised at
# startup that have nothing to do with this. So it is ignored. What decides is what
# import_sounds.py says in the log, and then what reached disk, below.
#
# All of the editor's output goes to IMPORT_LOG, and the terminal shows the lines
# that matter: import_sounds.py's own [Trace] lines, and the editor's errors that
# mean it never got that far — a Python traceback (LogPython: Error), a crash, a
# Trace module missing or built for another engine version (a pull changed C++ and
# Scripts/build.sh has not run), a commandlet it cannot find. These are the same
# lines Scripts/import-sounds.bat shows.
: > "$IMPORT_LOG" || trace_die "Cannot write the editor's log: ${IMPORT_LOG}"
trace_msg "The editor runs headless now. Its full output: ${IMPORT_LOG}"
set +e
"$CMD_BIN" "${ARGS[@]}" 2>&1 | tee "$IMPORT_LOG" \
    | grep -F -e '[Trace]' -e 'LogPythonScriptCommandlet' -e 'LogPython: Error' \
              -e 'Fatal error' -e 'Critical error' -e 'missing module' \
              -e 'built with a different engine version' -e 'looked like a commandlet'
set -e

# import_sounds.py's verdict, read from the log. It prints "done - N sound(s)
# imported" only when nothing failed, "FAILED" when anything did, and "ERROR: could
# not save" for each asset that did not reach the disk. Those three strings are a
# contract with import_sounds.py (see the comment above its "done" line), shared
# with Scripts/import-sounds.bat.
PY_DONE=0
PY_FAILED=0
PY_UNSAVED=0
if grep -qF '[Trace] done - ' "$IMPORT_LOG"; then PY_DONE=1; fi
if grep -qF '[Trace] FAILED' "$IMPORT_LOG"; then PY_FAILED=1; fi
if grep -qF '[Trace] ERROR: could not save' "$IMPORT_LOG"; then PY_UNSAVED=1; fi

# ------------------------------------------------------------------------------
# 3. Verify what landed
# ------------------------------------------------------------------------------
EXPECTED=("Content/Trace/Audio/DA_TraceSoundBank.uasset")
while IFS= read -r WAV; do
    [ -e "$WAV" ] || continue
    STEM="$(basename "$WAV" .wav)"
    # With --only, the other sounds were deliberately not touched this run, so only
    # the ones that were asked for are evidence of anything.
    if [ -z "$ONLY_SOUNDS" ] || printf '%s' ",${ONLY_SOUNDS}," | grep -q ",${STEM},"; then
        EXPECTED+=("Content/Trace/Audio/S_${STEM}.uasset")
    fi
done <<EOF
$(find "$SRC_DIR" -type f -name '*.wav' | sort)
EOF

MISSING=0
trace_msg "Verifying:"
for Rel in "${EXPECTED[@]}"; do
    if [ -f "${TRACE_PROJECT_ROOT}/${Rel}" ]; then
        printf '    ok      %s\n' "$Rel"
    else
        printf '    MISSING %s\n' "$Rel"
        MISSING=$((MISSING + 1))
    fi
done

if [ "$MISSING" != "0" ]; then
    trace_err "${MISSING} output(s) did not land. Search the output above for '[Trace]' lines."
    trace_err "Two things cause this more than anything else:"
    trace_err "  * the editor is already open on this project — close it and re-run; two processes"
    trace_err "    cannot both write Content/Trace/Audio."
    trace_err "  * the .uasset is checked out read-only (it is 'lockable' in .gitattributes) —"
    trace_err "    run Scripts/lock.sh on the file first."
    trace_err "Full editor log: ${IMPORT_LOG}"
    exit 1
fi
# The files are all there, but on a re-import that proves nothing: a save that failed
# leaves the OLD file in place. So the log's verdict decides from here.
if [ "$PY_UNSAVED" = "1" ]; then
    trace_err "The editor could not save every asset - the 'could not save' lines above. A file"
    trace_err "listed ok may still be the OLD one. Lock the file (Scripts/lock.sh), close any"
    trace_err "Unreal editor or game that has it loaded, and run this again. (A 'SystemExit: 1'"
    trace_err "traceback above is import_sounds.py's own exit 1, not a crash.)"
    trace_err "Full editor log: ${IMPORT_LOG}"
    exit 1
fi
if [ "$PY_FAILED" = "1" ]; then
    trace_err "import_sounds.py reported errors - the '[Trace] ERROR' lines above. A file listed"
    trace_err "ok may still be the OLD one. (A 'SystemExit: 1' traceback above is import_sounds.py's"
    trace_err "own exit 1, not a crash.) Full editor log: ${IMPORT_LOG}"
    exit 1
fi
if [ "$PY_DONE" = "0" ]; then
    trace_err "import_sounds.py never said it finished, so the files listed ok are most likely the"
    trace_err "OLD ones: the editor stopped before or during the import. The usual causes:"
    trace_err "  * the Trace editor module is missing or out of date - a pull changed C++."
    trace_err "    Run ./Scripts/build.sh, then this again."
    trace_err "  * a Python error or an editor crash - see any 'LogPython: Error', 'Fatal error'"
    trace_err "    or 'Critical error' lines above."
    trace_err "Full editor log: ${IMPORT_LOG}"
    exit 1
fi

trace_msg "Sound is imported (${WAV_COUNT} wav(s) seen, bank at /Game/Trace/Audio/DA_TraceSoundBank)."
trace_msg "Next launch picks it up (Trace.Audio.Reload in a running game). No rebuild. Check in game (WITHOUT -nosound): Trace.Audio.Report / Trace.Audio.Probe"
