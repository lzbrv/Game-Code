# =============================================================================
# Trace - generate-data-assets.py            (spec v17 section 5)
#
# Authors one UTraceCharacterDefinition asset per character under
#
#     /Game/Trace/Data/Characters/DA_Character_{Rocco,Chut,Mace,Oyster,X,
#                                                Roxie,Elle,Slimeball}
#
# Spec v18 section 2 took the roster from five to EIGHT. THE THREE NEW ONES ARE
# NOT OPTIONAL EXTRAS: the roster is all-or-none, so a build where five assets
# exist and three do not runs every character from the C++ table instead. Adding
# a name to CHARACTERS below without re-running this is how that happens.
#
# -----------------------------------------------------------------------------
# THE ONE THING TO UNDERSTAND ABOUT THIS SCRIPT
# -----------------------------------------------------------------------------
# IT CONTAINS NO CHARACTER DATA. Not a name, not a colour, not a cooldown, not a
# line of card prose. Every value it writes comes out of the C++ table in
# Source/Trace/Core/TraceCharacterRoster.cpp, through one call:
#
#     unreal.TraceCharacterDefinition.copy_fallback_values(asset, character_id)
#
# That is deliberate and it is the whole reason spec v17 section 0 can be
# satisfied here. A generator that held its own copy of the numbers would be a
# second place for them to live, and the first time somebody retuned the C++
# table the two would part company silently - which is the exact failure mode
# section 0 calls a BUG in this pass.
#
# The proof runs in the same pass: after saving, each asset is reloaded FROM
# DISK and compared field by field with the same C++ table, via
#
#     unreal.TraceCharacterDefinition.describe_fallback_mismatch(asset)
#
# and a single differing field is reported as FAIL.
#
# GREP FOR THE VERDICT, DO NOT TRUST THE PROCESS EXIT CODE. The pythonscript
# commandlet returns the engine's error COUNT, and this project logs one
# unrelated engine error on every headless start ("LoadConfig ... ProjectID"),
# so the process exits 1 on a completely successful run. The last line this
# script prints is `[generate-data-assets] EXIT=0` (or EXIT=1) and that is the
# verdict a wrapper or a human should read.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
# Close the editor first (it will hold the packages open otherwise), then:
#
#   "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
#       "/Users/gregorykaraev/trace/Trace.uproject" \
#       -run=pythonscript -script="/Users/gregorykaraev/trace/Scripts/generate-data-assets.py" \
#       -unattended -nosplash -nopause -stdout -FullStdOutLogOutput -nullrhi
#
# -nullrhi is SAFE here, unlike the arena bake: a data asset has no shader map
# to compile, so there is nothing that needs a real renderer.
#
# You can also paste it into the editor's Output Log Python prompt:
#
#     exec(open("/Users/gregorykaraev/trace/Scripts/generate-data-assets.py").read())
#
# -----------------------------------------------------------------------------
# RE-RUNNING IS SAFE, AND IT OVERWRITES
# -----------------------------------------------------------------------------
# There is no --force flag because there is nothing to force: an existing asset
# is loaded and rewritten from the C++ table. That means A HAND EDIT TO ONE OF
# THESE ASSETS DOES NOT SURVIVE A REGENERATE. If you want to change a
# character's card, change the C++ table and regenerate; if you want to change a
# character's NUMBERS, none of them are in here at all - they are on
# UTraceSettings (Project Settings > Game > Trace, category Abilities|<Name>).
#
# -----------------------------------------------------------------------------
# AFTERWARDS
# -----------------------------------------------------------------------------
#   * git lfs lock Content/Trace/Data/Characters/*.uasset   before editing them
#   * in game:  Trace.VerifyCharacterData                   the same proof, live
#               Trace.Data.DumpCharacters                   what the game serves
#               Trace.Data.UseCharacterAssets 0             force the C++ path
#
# If the assets are absent, malformed, or that toggle is 0, the game falls back
# to the C++ table and says so in the log. Nothing here is load-bearing for the
# game to run - that is spec v17 rule 1.
# =============================================================================

import sys

import unreal

PACKAGE_DIR = "/Game/Trace/Data/Characters"

# ETraceCharacterId, 1..8. The NAMES here are only used to build the asset name
# and to print a readable log line; the C++ side is asked for everything else.
# They are checked against C++ below rather than trusted.
#
# THE SPELLING MUST MATCH TraceCharacterIdToString EXACTLY, because C++ builds the
# package path from that function and this script builds it from this list. Two
# spellings of one character means an asset saved where the game never looks.
CHARACTERS = [
    (1, "Rocco"),
    (2, "Chut"),
    (3, "Mace"),
    (4, "Oyster"),
    (5, "X"),
    # --- spec v18 section 2 -------------------------------------------------
    (6, "Roxie"),
    (7, "Elle"),
    (8, "Slimeball"),
]


def log(message):
    unreal.log("[generate-data-assets] {}".format(message))


def warn(message):
    unreal.log_warning("[generate-data-assets] {}".format(message))


def fail(message):
    unreal.log_error("[generate-data-assets] {}".format(message))


def make_or_load(asset_name):
    """Return the asset at PACKAGE_DIR/asset_name, creating it if it is absent."""
    package_path = "{}/{}".format(PACKAGE_DIR, asset_name)

    if unreal.EditorAssetLibrary.does_asset_exist(package_path):
        existing = unreal.EditorAssetLibrary.load_asset(package_path)
        if existing is None:
            raise RuntimeError("{} exists but would not load".format(package_path))
        if not isinstance(existing, unreal.TraceCharacterDefinition):
            raise RuntimeError(
                "{} exists but is a {}, not a TraceCharacterDefinition. Delete it and re-run.".format(
                    package_path, type(existing).__name__))
        log("  reusing existing {}".format(package_path))
        return existing

    tools = unreal.AssetToolsHelpers.get_asset_tools()

    # UDataAssetFactory needs to be told which UDataAsset subclass to make. The
    # interactive path asks with a dialog; setting the property is how the
    # headless path answers the same question.
    factory = unreal.DataAssetFactory()
    try:
        factory.set_editor_property("data_asset_class", unreal.TraceCharacterDefinition)
    except Exception as error:                       # pylint: disable=broad-except
        warn("  could not set DataAssetFactory.data_asset_class ({}); trying the "
             "default factory instead".format(error))
        factory = None

    created = tools.create_asset(asset_name, PACKAGE_DIR, unreal.TraceCharacterDefinition, factory)
    if created is None:
        raise RuntimeError("create_asset returned None for {}".format(package_path))

    log("  created {}".format(package_path))
    return created


def main():
    log("spec v17 section 5 (+ v18 section 2) - authoring {} characters into {}".format(
        len(CHARACTERS), PACKAGE_DIR))

    if not unreal.EditorAssetLibrary.does_directory_exist(PACKAGE_DIR):
        unreal.EditorAssetLibrary.make_directory(PACKAGE_DIR)
        log("created the folder {}".format(PACKAGE_DIR))

    written = []
    packages = []

    for character_id, short_name in CHARACTERS:
        asset_name = "DA_Character_{}".format(short_name)
        log("{} (id {})".format(asset_name, character_id))

        asset = make_or_load(asset_name)

        # THE ONLY PLACE VALUES COME FROM. See the header.
        if not unreal.TraceCharacterDefinition.copy_fallback_values(asset, character_id):
            fail("  copy_fallback_values refused id {} - see the log above".format(character_id))
            return 1

        unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False)
        written.append((character_id, asset_name))
        packages.append(asset.get_outermost())
        log("  saved")

    # -------------------------------------------------------------------------
    # THE ROUND TRIP. Everything above wrote; this reads it all back off disk.
    # -------------------------------------------------------------------------
    log("")
    log("round trip - reloading each saved asset and comparing it with the C++ table")

    # Drop the packages from memory first, so the comparison below genuinely
    # reads the FILES that were just written rather than the objects that wrote
    # them. Without this the check would still pass if saving had silently
    # dropped a field, which is most of what it is here to catch.
    #
    # (unload_packages, not EditorAssetLibrary: that library has no unload -
    # its whole surface is "loaded asset" verbs.)
    from_disk = True
    try:
        if not unreal.EditorLoadingAndSavingUtils.unload_packages(packages):
            from_disk = False
    except Exception as unload_error:                 # pylint: disable=broad-except
        warn("  could not unload the packages ({})".format(unload_error))
        from_disk = False

    if from_disk:
        log("  unloaded all {} packages; the comparison below is reading the files".format(len(packages)))
    else:
        warn("  the packages stayed in memory - the comparison below is still worth having, but it "
             "does NOT prove the save round-tripped. Run Trace.VerifyCharacterData in the game, "
             "which always loads from disk in a fresh process.")

    failures = 0
    for character_id, asset_name in written:
        package_path = "{}/{}".format(PACKAGE_DIR, asset_name)
        reloaded = unreal.EditorAssetLibrary.load_asset(package_path)

        if reloaded is None:
            fail("  {} did not reload".format(package_path))
            failures += 1
            continue

        # An EMPTY string is the pass. See DescribeFallbackMismatch's comment in
        # TraceCharacterDefinition.h for why this is not the bool-returning form:
        # Unreal's Python glue would throw the mismatch text away on failure.
        mismatch = unreal.TraceCharacterDefinition.describe_fallback_mismatch(reloaded)
        if not mismatch:
            log("  PASS  {}  (id {}, printed cooldown {:.2f}s)".format(
                asset_name, character_id, reloaded.activated_slot.cooldown_seconds))
        else:
            fail("  FAIL  {}  {}".format(asset_name, mismatch))
            failures += 1

    log("")
    if failures:
        fail("{} of {} assets do not match the C++ table. NOTHING SHOULD SHIP IN THIS STATE - the "
             "game would play different numbers depending on whether the assets loaded.".format(
                 failures, len(written)))
        return 1

    log("PASS - all {} assets round-trip identically to the C++ table.".format(len(written)))
    log("Next: launch the game and run  Trace.VerifyCharacterData  for the same proof against the "
        "LIVE roster, plus the printed-vs-enforced cooldown check the C++ table cannot make on its own.")
    return 0


if __name__ == "__main__":
    EXIT_CODE = 1
    try:
        EXIT_CODE = main()
    except Exception as top_level_error:              # pylint: disable=broad-except
        fail("aborted: {}".format(top_level_error))
        EXIT_CODE = 1

    # The pythonscript commandlet does not propagate a Python return value, so
    # say the verdict in a form a wrapper script or a human can grep for.
    log("EXIT={}".format(EXIT_CODE))
    if EXIT_CODE != 0:
        sys.exit(EXIT_CODE)
