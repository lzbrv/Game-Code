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
#     cp NewDash.wav Art/Sounds/Dash.wav
#     ./Scripts/import-sounds.sh --only Dash
#
# No C++ edit. No rebuild. The game asks the BANK for a name and gets whatever the
# bank now points at — see Source/Trace/Audio/TraceSoundBank.h.
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
# `Scripts/lock.sh Content/Trace/Audio/S_Dash.uasset` first, which is exactly why
# --only exists — swapping one sound should not need nine locks.
#
# YOU DO NOT NEED TO RUN THIS TO PLAY. All outputs are committed, exactly like the
# font atlas and the railgun.
# ==============================================================================
set -euo pipefail

. "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/_trace_common.sh"

DRY_RUN=0
LIST_ONLY=0
ONLY_SOUNDS=""

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

AFTER RUNNING
  ./Scripts/build.sh
  git status Art/Sounds Content/Trace/Audio

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
        trace_warn "The manifest reported a problem (exit ${MANIFEST_STATUS}). Continuing — the"
        trace_warn "'Verifying' block at the end is what decides whether this run worked."
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

if [ -n "$ONLY_SOUNDS" ]; then
    export TRACE_SOUNDS="$ONLY_SOUNDS"
    trace_msg "--only ${ONLY_SOUNDS}: no other sound asset will be written."
fi

# THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN — it is non-zero if
# ANY error was logged in the whole session, including engine warnings raised at
# startup that have nothing to do with this. What reached disk, below, is the
# authoritative check. (Same reasoning as Scripts/import-font-atlas.sh and
# Scripts/import-railgun.sh.)
set +e
"$CMD_BIN" "${ARGS[@]}" 2>&1 | grep -E '\[Trace\]|LogPythonScriptCommandlet' || true
set -e

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
    exit 1
fi

trace_msg "Sound is imported (${WAV_COUNT} wav(s) seen, bank at /Game/Trace/Audio/DA_TraceSoundBank)."
trace_msg "Next: ./Scripts/build.sh, then in game (WITHOUT -nosound): Trace.Audio.Report / Trace.Audio.Probe"
