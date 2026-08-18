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
# THE SET OF SOUNDS IS DISCOVERED, NOT LISTED. This script globs Art/Sounds/*.wav
# rather than carrying a table of nine names, so a TENTH wav is a tenth bank row
# with no edit here either. The event name is simply the file's stem, and it is
# also the key C++ asks for (Source/Trace/Audio/TraceSoundEvents.h).
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
import os
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
    "CoreTurnover": "game-side",
    "Dash": "game-side",
    "Parry": "game-side",
    "Bodyshot": "client-side",
    "Headshot": "client-side",
    "CorePickup": "client-side",
    "Jump": "client-side",
    "WallJump": "client-side",
    "ButtonPress": "client-side",
}

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
    Every Art/Sounds/*.wav, as (event_name, absolute_path), sorted.

    DISCOVERED, NOT LISTED — see the header. The event name is the stem, exactly.
    """
    if not os.path.isdir(SOURCE_DIR):
        fail("{0} does not exist. The nine WAVs from the prompt live there.".format(SOURCE_DIR))
        return []

    found = []
    for path in sorted(glob.glob(os.path.join(SOURCE_DIR, "*.wav"))):
        stem = os.path.splitext(os.path.basename(path))[0]
        found.append((stem, path))

    if not found:
        fail("no .wav files in {0}.".format(SOURCE_DIR))
    return found


def describe(path):
    """
    (rate, channels, seconds) read from the WAV header itself, or None.

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
        return (rate, channels, seconds)
    except Exception as error:                          # pylint: disable=broad-except
        fail("could not read {0}: {1}".format(path, error))
        return None


def manifest(entries, selected):
    """Step 1. Prints what is on disk and validates it. No editor needed."""
    log("=== import_sounds: step 1, the manifest ===")
    log("source: {0}".format(SOURCE_DIR))
    log("")
    log("  {0:<16}{1:<12}{2:>8}{3:>6}{4:>9}   {5}".format(
        "EVENT", "SIDE", "RATE", "CH", "SECONDS", "FILE"))

    ok = True
    for stem, path in entries:
        info = describe(path)
        if info is None:
            ok = False
            continue
        rate, channels, seconds = info

        side = SIDES.get(stem)
        if side is None:
            # NOT an error. A tenth WAV is a supported thing to add; it simply
            # defaults to client-side until somebody declares otherwise in
            # Source/Trace/Audio/TraceSoundEvents.cpp.
            side = "client-side*"

        mark = "" if (selected is None or stem in selected) else "   (skipped this run)"
        log("  {0:<16}{1:<12}{2:>8}{3:>6}{4:>9.2f}   {5}{6}".format(
            stem, side, rate, channels, seconds, os.path.basename(path), mark))

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
    set_prop(sound, "looping", "bLooping", False)
    try:
        set_prop(sound, "sound_group", "SoundGroup", unreal.SoundGroup.SOUNDGROUP_EFFECTS)
    except AttributeError:
        log("  (this engine build has no SoundGroup.SOUNDGROUP_EFFECTS - continuing)")
    try:
        set_prop(sound, "loading_behavior", "LoadingBehavior",
                 unreal.SoundWaveLoadingBehavior.FORCE_INLINE)
    except AttributeError:
        log("  (this engine build has no SoundWaveLoadingBehavior.FORCE_INLINE - continuing)")

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
