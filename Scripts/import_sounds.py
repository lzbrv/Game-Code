# =============================================================================
# Trace — import_sounds.py     (spec v26 §9)
#
# Turns the WAVs the owner uploaded into the game's sound bank, in two steps that
# are deliberately separate — the same shape as Scripts/import_font_atlas.py:
#
#   Art/Sounds/*.wav
#        |
#        |  step 1 — PLAIN PYTHON, no editor needed
#        v
#   a validated manifest printed to the terminal (rate, channels, seconds, side)
#
#   Art/Sounds/<Event>.wav
#        |
#        |  step 2 — INSIDE UnrealEditor-Cmd (-run=pythonscript)
#        v
#   Content/Trace/Audio/S_<Event>.uasset          (USoundWave, committed)
#   Content/Trace/Audio/DA_TraceSoundBank.uasset  (UTraceSoundBank, committed)
#
# Driven by Scripts/import-sounds.sh. Running it by hand is fine too: with no
# `unreal` module importable it does step 1 and says why it stopped.
#
# -----------------------------------------------------------------------------
# "MAKE SURE IT'S EASY TO SWAP SOUND EFFECTS IN AND OUT FOR NEW VERSIONS"
# -----------------------------------------------------------------------------
# That is the owner's requirement, so here is the entire procedure for a new
# version of any sound:
#
#     cp NewDash.wav Art/Sounds/Dash.wav
#     ./Scripts/import-sounds.sh --only Dash
#
# No C++ edit. No rebuild. Nothing to remember about which file references it.
#
# ONE CAVEAT SINCE DEMO 29: three events (DeathBurst, CountdownTick, CountdownGo)
# are UNWIRED — declared, imported and resolvable, but not allowed to sound. A
# replacement WAV for one of them imports exactly as above and is still silent in
# game until `Trace.Audio.UnwiredEvents 0`. The manifest flags them; the reasons
# are in Source/Trace/Audio/TraceSoundEvents.cpp and on the sound-test page.
#
# THE SET OF SOUNDS IS DISCOVERED, NOT LISTED. This script globs Art/Sounds
# RECURSIVELY rather than carrying a table of names, so a new wav is a new bank
# row with no edit here either. The event name is simply the file's stem, and it
# is also the key C++ asks for (Source/Trace/Audio/TraceSoundEvents.h).
#
# RECURSIVELY, since spec v29 §1b: the eleven footsteps ship in
# Art/Sounds/Footsteps/ and their events are Step1..Step11 — the STEM, never the
# folder. The folder is how a human keeps eleven near-identical files tidy; it is
# not part of the name, and two files with the same stem in different folders is
# an ERROR rather than a silent last-one-wins.
#
# WHAT IS NOT DATA: WHICH MACHINES HEAR A SOUND. Game-side vs client-side is a
# networking behaviour and lives in TraceSoundEvents.cpp — see the long comment
# at the top of TraceSoundEvents.h for why putting it in this asset would be a
# way to break the owner's explicit design without touching any code. The SIDES
# table below is a MIRROR of that C++ table, used only to print a manifest a
# human can check against the spec; nothing reads it at runtime.
#
# -----------------------------------------------------------------------------
# THESE WAVS ARE OURS AND THEY ARE COMMITTED
# -----------------------------------------------------------------------------
# Unlike Art/Fonts (licensed to the owner for desktop use, gitignored, see
# docs/FONTS.md), Art/Sounds is our own content. Both the sources and the
# imported .uasset files are tracked — .wav and .uasset both go through Git LFS
# per .gitattributes, and .uasset is `lockable`, so a RE-import of an existing
# sound needs Scripts/lock.sh first. Creating one for the first time does not.
# =============================================================================
import glob
import math
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Where the owner drops WAVs. One spelling, and the shell script prints it too.
SOURCE_DIR = os.path.join(ROOT, "Art", "Sounds")

# Where the assets land. C++ names the same three strings in
# Source/Trace/Audio/TraceSoundBank.cpp (TraceSoundBankPaths) — Trace.Audio.Report
# prints them at runtime, so a disagreement is visible rather than silent.
PACKAGE_DIR = os.environ.get("TRACE_SOUND_PACKAGE_DIR", "/Game/Trace/Audio")
SOUND_PREFIX = "S_"
BANK_NAME = "DA_TraceSoundBank"

# A MIRROR of TraceSoundEvents.cpp, for the printed manifest ONLY. If these ever
# disagree the manifest is what is wrong, not the game: C++ is the authority.
SIDES = {
    # --- spec v26 §9, UNCHANGED by v29 §1a: "keeping the same sounds client side
    #     vs global". Six of these got a NEW WAV this patch; not one of them
    #     changed side.
    "CoreTurnover": "game-side",
    "Dash": "game-side",
    "Parry": "game-side",
    "Bodyshot": "client-side",
    "Headshot": "client-side",
    "CorePickup": "client-side",
    "Jump": "client-side",
    "WallJump": "client-side",
    "ButtonPress": "client-side",

    # --- spec v29 §1e: "gunshots should be global".
    "PistolShoot1": "game-side",
    "PistolShoot2": "game-side",
    "PistolShoot3": "game-side",
    "PistolShoot4": "game-side",
    "SmgShoot1": "game-side",

    # --- spec v29 §1b: footsteps are a world sound, so other players hear yours.
    "Step1": "game-side",
    "Step2": "game-side",
    "Step3": "game-side",
    "Step4": "game-side",
    "Step5": "game-side",
    "Step6": "game-side",
    "Step7": "game-side",
    "Step8": "game-side",
    "Step9": "game-side",
    "Step10": "game-side",
    "Step11": "game-side",

    # --- spec v29 §1f. Inferred triggers; the reasoning is in TraceSoundEvents.cpp.
    "Goal": "game-side",       # a goal is the whole room's event
    "Kill": "client-side",     # the killer's own confirmation, like the hitmarker
    "RoccoRipple": "game-side",  # an ability going off in the world, like Dash

    # --- release FX/AUDIO plan §5.1: the synthesized palette (43 stems, rendered
    #     by Scripts/generate_sounds.py into Combat/ Abilities/ UI/ Music/).
    #     "client-side" here includes the plan's "(burst)"/"replicated-local"
    #     rows: they reach every machine through code that already runs
    #     everywhere, and are declared Client in C++ so a stray Play() cannot
    #     double-multicast them.
    # core combat
    "MeleeSwing": "game-side",
    "MeleeHit": "game-side",
    "MeleeBackstab": "game-side",
    "Reload": "game-side",
    "DryFire": "client-side",
    "WeaponSwitch": "client-side",
    "DamageTaken": "client-side",
    "DeathBurst": "game-side",
    "Respawn": "client-side",
    "ShieldBlock": "client-side",
    # per-kit abilities
    "ChutBash": "client-side",
    "MaceSpikeThrow": "game-side",
    "MaceSpikeEmbed": "client-side",
    "MacePullLoop": "client-side",
    "OysterPickler": "game-side",
    "OysterJarBreak": "client-side",
    "XSting": "client-side",
    "XStingLoad": "game-side",
    "RoxieRocketBurst": "client-side",
    "RoxieRocketLaunch": "game-side",
    "RoxieRocketLoop": "client-side",
    "RoxieModded": "game-side",
    "ElleTeleport": "client-side",
    "ElleSnap": "client-side",
    "ElleCloak": "game-side",
    "ElleDecloak": "game-side",
    "SlimeballWall": "game-side",
    "SlimeballStick": "game-side",
    "MortimerQuake": "game-side",
    "MortimerMantle": "client-side",
    "LilyZip": "game-side",
    "LilyZipLoop": "client-side",
    "RoccoRideLoop": "client-side",
    "RoccoJump": "game-side",
    # UI + countdown (2D)
    "UIHover": "client-side",
    "UIBack": "client-side",
    "UIDeny": "client-side",
    "CountdownTick": "client-side",
    "CountdownGo": "client-side",
    # music (family Music in C++ — the MusicVolumeScale hook)
    "StingerVictory": "client-side",
    "StingerDefeat": "client-side",
    "MusicTitle": "client-side",
    "AmbienceMatch": "client-side",
}

# Stems whose WAV is a seamless loop (release FX/AUDIO plan §5.6 step 3; the
# generator renders these on a circular buffer, so file-start -> file-end IS the
# loop point — plain looping, no crossfade). import_wav sets looping=True for
# these and SKIPS FORCE_INLINE: a looping sound is not the "0.03 s hit marker"
# the inline reasoning below is about, and the two music beds are megabytes
# (MusicTitle ~11.3 MB, AmbienceMatch ~8.5 MB) — force-inlining those would pin
# them whole in memory; left at the engine default they stream.
LOOPING_STEMS = {
    "MacePullLoop",
    "RoxieRocketLoop",
    "LilyZipLoop",
    "RoccoRideLoop",
    "MusicTitle",
    "AmbienceMatch",
}

# DEMO 29 items 9 and 11 — events that are DECLARED and DELIBERATELY SILENT.
#
# A MIRROR of TraceSoundEvents::Unwired(), for the printed manifest ONLY, exactly
# like SIDES above and for the same reason: C++ is the authority and nothing here
# is read at runtime. It earns its place because the manifest is what somebody
# reads after dropping in a replacement WAV, and "I swapped DeathBurst and the
# game still makes no noise" is the confusion this list creates.
#
#   DeathBurst      played at the body on every kill, on top of the owner's
#                   Kill.wav, and changed what a kill sounds like (item 9).
#   CountdownTick   ATraceCore drives the kickoff countdown off a field that is
#   CountdownGo     not a kickoff deadline, so both beeped with no kickoff (item 11).
#
# Their WAVs, their .uassets and their trigger sites are all untouched:
# `Trace.Audio.UnwiredEvents 0` in the console brings every one of them back.
UNWIRED_STEMS = {
    "DeathBurst",
    "CountdownTick",
    "CountdownGo",
}

# The FOOTSTEP family, spelled once. §1b gives footsteps their own volume knob,
# well below the bank default; this is the manifest's mirror of the C++ family
# table so the printed loudness column can flag which rows that knob applies to.
FOOTSTEP_STEMS = ["Step{0}".format(i) for i in range(1, 12)]

_failures = []


def log(msg):
    line = "[Trace] {0}".format(msg)
    try:
        import unreal
        unreal.log(line)
    except ImportError:
        print(line)
    sys.stdout.flush()


def fail(msg):
    _failures.append(msg)
    line = "[Trace] ERROR: {0}".format(msg)
    try:
        import unreal
        unreal.log_error(line)
    except ImportError:
        print(line, file=sys.stderr)
    sys.stderr.flush()


def wanted_stems():
    """
    Which sounds this run touches. All of them, unless TRACE_SOUNDS names a
    comma-separated subset by event name.

    This exists for the same reason import_font_atlas.py's weight filter does:
    .uasset is `lockable` in .gitattributes and therefore checked out READ-ONLY,
    so re-importing an asset nobody has locked fails on the write. Swapping ONE
    sound should not need nine locks.
    """
    requested = os.environ.get("TRACE_SOUNDS", "").strip()
    if not requested:
        return None
    return [w.strip() for w in requested.split(",") if w.strip()]


def discover():
    """
    Every Art/Sounds/**/*.wav, as (event_name, absolute_path), sorted by event.

    DISCOVERED, NOT LISTED — see the header. The event name is the stem, exactly,
    and the SUBFOLDER IS NOT PART OF IT: Art/Sounds/Footsteps/Step7.wav is the
    event `Step7`, which is the key C++ asks the bank for and the asset name
    S_Step7. A folder is filing, not naming.

    Two files with the same stem in different folders is a hard error. Silently
    letting the last one win is how a project ends up with a bank row nobody can
    explain, and the sort order that decided it is an implementation detail.
    """
    if not os.path.isdir(SOURCE_DIR):
        fail("{0} does not exist. The WAVs from the prompt live there.".format(SOURCE_DIR))
        return []

    by_stem = {}
    for path in sorted(glob.glob(os.path.join(SOURCE_DIR, "**", "*.wav"), recursive=True)):
        stem = os.path.splitext(os.path.basename(path))[0]
        if stem in by_stem:
            fail("two files both claim the event '{0}': {1} and {2}. An event name is a stem and "
                 "must be unique across Art/Sounds/ and every folder under it.".format(
                     stem, os.path.relpath(by_stem[stem], ROOT), os.path.relpath(path, ROOT)))
            continue
        by_stem[stem] = path

    found = sorted(by_stem.items())
    if not found:
        fail("no .wav files under {0}.".format(SOURCE_DIR))
    return found


def _dbfs(linear):
    """Full-scale decibels for a 0..1 amplitude. -inf floors at -120 so it prints."""
    if linear <= 0.0:
        return -120.0
    return 20.0 * math.log10(linear)


def loudness(path):
    """
    (peak_dBFS, rms_dBFS) of the actual samples, or (None, None).

    *** THIS IS THE HALF OF §1b THAT CANNOT BE TAKEN ON TRUST. *** "Ensure they
    are quieter than other sounds" is a claim about the AUDIBLE result, and a
    volume knob alone does not settle it: the loudest footstep here peaks at
    -20.1 dBFS while ButtonPress peaks at -27.2, so at equal gain the footsteps
    would be the LOUDER of the two. The knob has to beat that gap, and the only
    way to know it does is to measure the files.

    The game measures the same thing from the imported asset — see
    Trace.Audio.Loudness in Source/Trace/Audio/TraceAudioLoudness.cpp — so the
    two numbers can be compared and a bad import shows up as a disagreement
    rather than as a quiet mystery.
    """
    try:
        with wave.open(path, "rb") as handle:
            width = handle.getsampwidth()
            frames = handle.readframes(handle.getnframes())
    except Exception:                                   # pylint: disable=broad-except
        return (None, None)

    if width != 2 or not frames:
        # 8/24/32-bit is legal and imports fine; it is only the MEASUREMENT that
        # this function declines to guess at. Everything the game does still works.
        return (None, None)

    count = len(frames) // 2
    samples = struct.unpack("<{0}h".format(count), frames[:count * 2])
    peak = max(abs(sample) for sample in samples) / 32768.0
    mean_square = sum(float(sample) * sample for sample in samples) / float(count)
    rms = math.sqrt(mean_square) / 32768.0
    return (_dbfs(peak), _dbfs(rms))


def describe(path):
    """
    (rate, channels, seconds, peak_dBFS, rms_dBFS) read from the file itself, or None.

    Reading the header rather than trusting the file name catches the one import
    failure that is otherwise invisible: an .mp3 or an LFS pointer renamed to
    .wav imports as a zero-length sound that loads fine and plays nothing.
    """
    try:
        with open(path, "rb") as handle:
            magic = handle.read(4)
        if magic == b"vers":
            fail("{0} is an unfetched Git LFS pointer, not audio. Run: git lfs pull".format(path))
            return None
        if magic != b"RIFF":
            fail("{0} does not start with 'RIFF' - it is not a WAV.".format(path))
            return None

        with wave.open(path, "rb") as handle:
            rate = handle.getframerate()
            channels = handle.getnchannels()
            frames = handle.getnframes()
        seconds = (float(frames) / float(rate)) if rate else 0.0
        if seconds <= 0.0:
            fail("{0} is zero-length.".format(path))
            return None
        peak_db, rms_db = loudness(path)
        return (rate, channels, seconds, peak_db, rms_db)
    except Exception as error:                          # pylint: disable=broad-except
        fail("could not read {0}: {1}".format(path, error))
        return None


def manifest(entries, selected):
    """Step 1. Prints what is on disk and validates it. No editor needed."""
    log("=== import_sounds: step 1, the manifest ===")
    log("source: {0}".format(SOURCE_DIR))
    log("")
    log("  {0:<18}{1:<12}{2:>7}{3:>4}{4:>8}{5:>10}{6:>10}   {7}".format(
        "EVENT", "SIDE", "RATE", "CH", "SECONDS", "PEAK dB", "RMS dB", "FILE"))

    ok = True
    loudest_step = (-999.0, "")
    quietest_other = (999.0, "")

    for stem, path in entries:
        info = describe(path)
        if info is None:
            ok = False
            continue
        rate, channels, seconds, peak_db, rms_db = info

        side = SIDES.get(stem)
        if side is None:
            # NOT an error. A new WAV is a supported thing to add; it simply
            # defaults to client-side until somebody declares otherwise in
            # Source/Trace/Audio/TraceSoundEvents.cpp.
            side = "client-side*"

        if peak_db is not None:
            if stem in FOOTSTEP_STEMS:
                if peak_db > loudest_step[0]:
                    loudest_step = (peak_db, stem)
            elif peak_db < quietest_other[0]:
                quietest_other = (peak_db, stem)

        mark = "" if (selected is None or stem in selected) else "   (skipped this run)"
        if stem in UNWIRED_STEMS:
            mark += "   [UNWIRED - imports fine, does not sound; see UNWIRED_STEMS above]"
        rel = os.path.relpath(path, SOURCE_DIR)
        log("  {0:<18}{1:<12}{2:>7}{3:>4}{4:>8.2f}{5:>10}{6:>10}   {7}{8}".format(
            stem, side, rate, channels, seconds,
            "n/a" if peak_db is None else "{0:.2f}".format(peak_db),
            "n/a" if rms_db is None else "{0:.2f}".format(rms_db),
            rel, mark))

    present_unwired = sorted(UNWIRED_STEMS.intersection(stem for stem, _ in entries))
    if present_unwired:
        log("")
        log("  {0} event(s) are UNWIRED as of Demo 29 and will NOT sound in a match even after a "
            "clean import: {1}.".format(len(present_unwired), ", ".join(present_unwired)))
        log("  Replacing their WAVs still works and is still worth doing; "
            "`Trace.Audio.UnwiredEvents 0` is what makes them audible again.")

    # ---------------------------------------------------------------------
    # §1b, stated in numbers rather than in adjectives.
    # ---------------------------------------------------------------------
    if loudest_step[1] and quietest_other[1]:
        gap = loudest_step[0] - quietest_other[0]
        log("")
        log("  footsteps at UNITY gain: the loudest ({0}, {1:.2f} dBFS peak) sits {2:+.2f} dB against "
            "the quietest other sound ({3}, {4:.2f} dBFS).".format(
                loudest_step[1], loudest_step[0], gap, quietest_other[1], quietest_other[0]))
        if gap > 0.0:
            log("  so the FILES ALONE DO NOT SATISFY §1b — the footstep volume knob "
                "(Trace Audio > Footstep Volume) has to buy at least {0:.2f} dB, and the game "
                "asserts the result with Trace.Audio.Loudness.".format(gap))

    declared = set(SIDES.keys())
    present = set(stem for stem, _ in entries)
    for missing in sorted(declared - present):
        fail("{0} is declared in TraceSoundEvents.cpp but there is no Art/Sounds/{0}.wav. "
             "That event will be silent (it logs once and plays nothing).".format(missing))
        ok = False

    extra = sorted(present - declared)
    if extra:
        log("")
        log("  * {0} is not in the C++ event table yet, so it defaults to client-side. "
            "Declare it in Source/Trace/Audio/TraceSoundEvents.cpp to make it game-side."
            .format(", ".join(extra)))

    return ok


# -----------------------------------------------------------------------------
# Step 2 — the WAVs -> USoundWave assets -> the bank (editor only)
# -----------------------------------------------------------------------------

def set_prop(obj, snake, camel, value):
    """UE renames properties between versions; try both spellings, per import_font_atlas.py."""
    for name in (snake, camel):
        try:
            obj.set_editor_property(name, value)
            return True
        except Exception:                               # pylint: disable=broad-except
            continue
    # NOT a failure. Every property below is a nicety; the sound is already
    # imported and audible without any of them.
    log("  (could not set {0}/{1} on {2} - continuing)".format(snake, camel, obj.get_name()))
    return False


def import_wav(unreal, stem, path):
    """Imports ONE WAV as /Game/Trace/Audio/S_<stem>. Returns the asset or None."""
    asset_name = "{0}{1}".format(SOUND_PREFIX, stem)
    asset_path = "{0}/{1}".format(PACKAGE_DIR, asset_name)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", PACKAGE_DIR)
    task.set_editor_property("destination_name", asset_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    sound = unreal.EditorAssetLibrary.load_asset(asset_path)
    if sound is None:
        fail("{0} did not import.".format(asset_path))
        return None

    # These earn their place on a one-shot game SFX:
    #   looping False        a 0.03 s hit marker that loops is a stuck tone.
    #   SOUNDGROUP_EFFECTS   puts them in the group the SFX volume slider drives.
    #   FORCE_INLINE         they are tens of kilobytes and are wanted the instant a match starts;
    #                        streaming a 0.03 s hit marker costs a seek to save nothing, and a
    #                        first-play hitch on a hit marker is exactly the sound you notice.
    #                        (This is UE5's replacement for the old bStreaming flag.)
    #
    # A LOOPING_STEMS member is the opposite on both counts (FX/AUDIO plan §5.6):
    # its WAV is a seamless circular-buffer render, so looping=True and the file
    # plays forever from its own head — and it is NOT force-inlined; see the note
    # on LOOPING_STEMS for why (the music beds alone are ~20 MB).
    is_loop = stem in LOOPING_STEMS
    set_prop(sound, "looping", "bLooping", is_loop)
    try:
        set_prop(sound, "sound_group", "SoundGroup", unreal.SoundGroup.SOUNDGROUP_EFFECTS)
    except AttributeError:
        log("  (this engine build has no SoundGroup.SOUNDGROUP_EFFECTS - continuing)")
    try:
        # INHERITED (the engine default) for loops rather than simply not touching the
        # property: replace_existing re-uses the existing UObject, so a stem moved INTO
        # LOOPING_STEMS after an inline import would otherwise keep its stale FORCE_INLINE.
        set_prop(sound, "loading_behavior", "LoadingBehavior",
                 unreal.SoundWaveLoadingBehavior.INHERITED if is_loop
                 else unreal.SoundWaveLoadingBehavior.FORCE_INLINE)
    except AttributeError:
        log("  (this engine build has no SoundWaveLoadingBehavior - continuing)")

    unreal.EditorAssetLibrary.save_loaded_asset(sound, only_if_is_dirty=False)

    duration = 0.0
    try:
        duration = float(sound.get_editor_property("duration"))
    except Exception:                                   # pylint: disable=broad-except
        pass
    log("  imported {0}  ({1:.2f}s)".format(asset_path, duration))
    return sound


def make_or_load_bank(unreal):
    """The bank asset, creating it if absent. Same shape as generate-data-assets.py."""
    bank_path = "{0}/{1}".format(PACKAGE_DIR, BANK_NAME)

    if unreal.EditorAssetLibrary.does_asset_exist(bank_path):
        existing = unreal.EditorAssetLibrary.load_asset(bank_path)
        if existing is None:
            fail("{0} exists but would not load.".format(bank_path))
            return None
        if not isinstance(existing, unreal.TraceSoundBank):
            fail("{0} exists but is a {1}, not a TraceSoundBank. Delete it and re-run."
                 .format(bank_path, type(existing).__name__))
            return None
        log("  reusing {0}".format(bank_path))
        return existing

    tools = unreal.AssetToolsHelpers.get_asset_tools()

    # UDataAssetFactory has to be told which UDataAsset subclass to make; the
    # interactive path asks with a dialog and this is how the headless path
    # answers the same question. (Copied deliberately from generate-data-assets.py,
    # which is the only other place in this project that authors a data asset.)
    factory = unreal.DataAssetFactory()
    try:
        factory.set_editor_property("data_asset_class", unreal.TraceSoundBank)
    except Exception as error:                          # pylint: disable=broad-except
        log("  could not set DataAssetFactory.data_asset_class ({0}); trying the default "
            "factory instead".format(error))
        factory = None

    created = tools.create_asset(BANK_NAME, PACKAGE_DIR, unreal.TraceSoundBank, factory)
    if created is None:
        fail("create_asset returned None for {0}.".format(bank_path))
        return None

    log("  created {0}".format(bank_path))
    return created


def main():
    entries = discover()
    selected = wanted_stems()

    if selected is not None:
        known = [stem for stem, _ in entries]
        for name in selected:
            if name not in known:
                fail("TRACE_SOUNDS names '{0}', but there is no Art/Sounds/{0}.wav. "
                     "Present: {1}.".format(name, ", ".join(known) or "(nothing)"))

    if entries:
        manifest(entries, selected)

    try:
        import unreal
    except ImportError:
        log("")
        log("no `unreal` module in this interpreter, so NOTHING was imported. That step needs the "
            "editor: ./Scripts/import-sounds.sh does both halves.")
        sys.exit(1 if _failures else 0)
        return

    log("")
    log("=== import_sounds: step 2, into the engine ===")

    if not unreal.EditorAssetLibrary.does_directory_exist(PACKAGE_DIR):
        unreal.EditorAssetLibrary.make_directory(PACKAGE_DIR)
        log("created {0}".format(PACKAGE_DIR))

    imported = []
    for stem, path in entries:
        if selected is not None and stem not in selected:
            continue
        sound = import_wav(unreal, stem, path)
        if sound is not None:
            imported.append((stem, sound))

    # -------------------------------------------------------------------------
    # THE BANK — the one place a name maps to a sound.
    # -------------------------------------------------------------------------
    log("")
    log("bank: {0}/{1}".format(PACKAGE_DIR, BANK_NAME))
    bank = make_or_load_bank(unreal)
    if bank is None:
        log("FAILED: no bank. The game still plays every sound through the convention path "
            "{0}/S_<Event>, so this is degraded rather than silent - but fix it.".format(PACKAGE_DIR))
        sys.exit(1)
        return

    # Read-modify-write, NOT a wholesale replace. A --only run must not drop the
    # eight rows it was told not to touch, and a row somebody added by hand for a
    # sound with no WAV of its own is theirs to keep.
    events = {}
    try:
        existing_map = bank.get_editor_property("events")
        if existing_map:
            for key in existing_map.keys():
                events[key] = existing_map[key]
    except Exception as error:                          # pylint: disable=broad-except
        log("  (could not read the existing rows: {0} - writing a fresh map)".format(error))

    for stem, sound in imported:
        events[unreal.Name(stem)] = sound

    bank.set_editor_property("events", events)
    unreal.EditorAssetLibrary.save_loaded_asset(bank, only_if_is_dirty=False)
    log("  {0} row(s): {1}".format(len(events), ", ".join(sorted(str(k) for k in events.keys()))))

    # -------------------------------------------------------------------------
    # THE ROUND TRIP. Everything above wrote; this reads it back.
    # -------------------------------------------------------------------------
    reloaded = unreal.EditorAssetLibrary.load_asset("{0}/{1}".format(PACKAGE_DIR, BANK_NAME))
    if reloaded is None:
        fail("the bank did not reload after saving.")
    else:
        rows = reloaded.get_editor_property("events")
        for stem, _ in imported:
            key = unreal.Name(stem)
            if key not in rows or rows[key] is None:
                fail("the saved bank has no row for '{0}'.".format(stem))

    if _failures:
        log("")
        log("FAILED with {0} error(s):".format(len(_failures)))
        for message in _failures:
            log("  - {0}".format(message))
        sys.exit(1)
    else:
        log("")
        log("done - {0} sound(s) imported and the bank is current.".format(len(imported)))


main()
