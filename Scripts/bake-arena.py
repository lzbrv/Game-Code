# =============================================================================
# Trace - bake-arena.py
#
# Turns the PROCEDURAL arena into a REAL, EDITABLE LEVEL at /Game/Maps/Arena_Baked.
#
# -----------------------------------------------------------------------------
# WHY THIS EXISTS  (spec v15 section 1)
# -----------------------------------------------------------------------------
# Trace builds its entire playfield from C++: ATraceArenaBuilder spawns the
# floor, the walls, the cover, the endzones, the lights and the player starts at
# BeginPlay, and /Game/Maps/Arena is a deliberately empty .umap. That is a good
# property for a prototype and a bad one for a game anybody else has to work on.
# Nobody can move a wall. Nobody can try a cover block 200 uu to the left.
# Nobody can even LOOK at the arena without launching the game.
#
# This script runs the builder once, headlessly, and writes down what it built:
# every logical piece of the arena becomes its own actor, with a readable label
# and its own transform, in a level a human can open and edit.
#
# THE PROCEDURAL PATH IS NOT TOUCHED. /Game/Maps/Arena still builds itself at
# BeginPlay exactly as it always did, and the baked map is opt-in - launch it by
# name - so the two can be compared. That is spec v15 section 1.7 verbatim.
#
# -----------------------------------------------------------------------------
# WHAT IT PRODUCES
# -----------------------------------------------------------------------------
#   /Game/Trace/Materials/Parents/M_TraceSurface  the two authored parent
#   /Game/Trace/Materials/Parents/M_TraceNeon      materials, COMMITTED (spec v17
#                                          section 3 - Content/Generated/ is
#                                          gitignored, so a baked map that
#                                          referenced it would render as grey
#                                          default material on every machine but
#                                          the one that baked it)
#   /Game/Trace/Materials/Instances/...    one MaterialInstanceConstant per
#                                          distinct tint the build asked for,
#                                          written by the C++ bake. THIS is the
#                                          layer a designer retunes.
#   /Game/Maps/Arena_Baked                 the level, with One File Per Actor on
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
# Use the wrapper, which finds the engine and checks the result:
#
#     Scripts/bake-arena.sh
#
# The raw command it wraps, macOS:
#
#     "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
#         "/path/to/Trace.uproject" \
#         -run=pythonscript -script="/path/to/Scripts/bake-arena.py" \
#         -unattended -nosplash -stdout -FullStdOutLogOutput
#
# You can also paste it into the editor's Output Log Python prompt:
#
#     exec(open("/path/to/Scripts/bake-arena.py").read())
#
# DO NOT ADD -nullrhi ON A FIRST RUN. The two materials have to compile their
# shader maps, and material compilation needs a real RHI; without one they save
# with no shader map and the arena renders as the default checkerboard. Once the
# materials exist, -nullrhi is fine for re-baking the geometry (the wrapper's
# --nullrhi flag).
#
# -----------------------------------------------------------------------------
# HOW TO PLAY THE RESULT
# -----------------------------------------------------------------------------
#     Scripts/run-listen-server.sh --map /Game/Maps/Arena_Baked
#     Scripts/run-client.sh <host-ip>            (joins whatever the host runs)
#
# Or open Content/Maps/Arena_Baked.umap in the editor and press Play.
#
# -----------------------------------------------------------------------------
# OPTIONS (environment variables - the pythonscript commandlet has no clean way
# to pass argv through -script=, so env vars are the reliable channel)
# -----------------------------------------------------------------------------
#   TRACE_BAKE_MAP        package path of the level. Default /Game/Maps/Arena_Baked
#   TRACE_BAKE_MATERIALS  package dir for the promoted materials.
#                         Default /Game/Trace/Materials
#   TRACE_BAKE_FORCE      "1" to delete and rebuild an existing baked map and
#                         its material instances. Default off. The PARENT
#                         materials are never touched by a bake.
#   TRACE_BAKE_ADD_CORE_SPAWN
#                         "1" to skip the bake entirely and instead add the
#                         placed Core spawn marker to an existing baked level
#                         (spec v17 section 2). One new package; nothing else in
#                         the level is rewritten. Idempotent.
# =============================================================================

import os
import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write(
        "bake-arena.py must run inside Unreal Editor's Python environment.\n"
        "Use Scripts/bake-arena.sh, or the -run=pythonscript command line in\n"
        "this file's header.\n"
    )
    raise SystemExit(2)


MAP_PATH = os.environ.get("TRACE_BAKE_MAP", "/Game/Maps/Arena_Baked").rstrip("/")
MATERIAL_DIR = os.environ.get("TRACE_BAKE_MATERIALS", "/Game/Trace/Materials").rstrip("/")
FORCE = os.environ.get("TRACE_BAKE_FORCE", "0") == "1"

# Top-up mode: do not bake, just add the Core spawn marker to an already baked
# level and save. See add_core_spawn() for why this is not "just re-bake".
TOPUP = os.environ.get("TRACE_BAKE_ADD_CORE_SPAWN", "0") == "1"

# Spec v17 section 3's layout. The two halves are separated because they have
# different lifetimes: the PARENTS are authored by generate_content.py and change
# when somebody edits the shader graph, the INSTANCES are rewritten by the C++
# bake on every re-bake. Keeping them in one folder made "which of these do I
# lock before I edit it" unanswerable.
PARENT_DIR = "{0}/Parents".format(MATERIAL_DIR)
INSTANCE_DIR = "{0}/Instances".format(MATERIAL_DIR)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def log(message):
    unreal.log("[Bake] {0}".format(message))


def log_error(message):
    unreal.log_error("[Bake] {0}".format(message))


# -----------------------------------------------------------------------------
# 1. Make sure the committed parent materials exist
#
# THE POINT OF THIS STEP, and it is the one that decides whether the baked map is
# usable by anybody else: NOTHING THE GAME SHIPS MAY REFERENCE Content/Generated/,
# which .gitignore excludes. A .umap that did would be fine here and broken
# everywhere else - grey default material on every machine but the one that baked.
#
# Since spec v17 section 3 the committed copies at /Game/Trace/Materials/Parents
# ARE the source of truth rather than a promoted duplicate, so on a normal bake
# this step finds them already present and does nothing. It still runs the SAME
# generator rather than reimplementing the two materials, because two copies of a
# material definition is two things to keep in step and the day they drift the
# baked arena is a different colour from the procedural one and nobody knows why.
# -----------------------------------------------------------------------------

def promote_materials():
    generator = os.path.join(SCRIPT_DIR, "generate_content.py")
    if not os.path.isfile(generator):
        raise RuntimeError("generate_content.py not found next to this script ({0})".format(generator))

    previous_dir = os.environ.get("TRACE_CONTENT_DIR")
    previous_instances = os.environ.get("TRACE_INSTANCE_DIR")
    previous_force = os.environ.get("TRACE_FORCE_CONTENT")
    os.environ["TRACE_CONTENT_DIR"] = PARENT_DIR
    os.environ["TRACE_INSTANCE_DIR"] = INSTANCE_DIR
    # NOT forced by a --force bake. --force means "replace the LEVEL"; the parent
    # materials are hand-authored assets that a re-bake has no business rebuilding
    # (and rebuilding them would silently discard a graph edit somebody committed).
    # Rebuild them deliberately with TRACE_FORCE_CONTENT=1 on generate_content.py.
    os.environ["TRACE_FORCE_CONTENT"] = "0"

    try:
        # exec, not import: generate_content.py reads its configuration from the
        # environment at module scope and does its work on import, so a plain
        # import would be a one-shot that could never be re-pointed.
        namespace = {"__name__": "trace_generate_content", "__file__": generator}
        with open(generator, "r") as handle:
            exec(compile(handle.read(), generator, "exec"), namespace)  # noqa: S102
    finally:
        if previous_dir is None:
            os.environ.pop("TRACE_CONTENT_DIR", None)
        else:
            os.environ["TRACE_CONTENT_DIR"] = previous_dir
        if previous_instances is None:
            os.environ.pop("TRACE_INSTANCE_DIR", None)
        else:
            os.environ["TRACE_INSTANCE_DIR"] = previous_instances
        if previous_force is None:
            os.environ.pop("TRACE_FORCE_CONTENT", None)
        else:
            os.environ["TRACE_FORCE_CONTENT"] = previous_force

    for name in ("M_TraceSurface", "M_TraceNeon"):
        path = "{0}/{1}".format(PARENT_DIR, name)
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            raise RuntimeError("{0} was not created; the bake would produce a broken map".format(path))
        log("Committed parent present: {0}".format(path))


# -----------------------------------------------------------------------------
# 2. One File Per Actor  (spec v15 section 1.6)
#
# "This is the thing that lets four people edit one map without destroying each
# other's work, and it is why this migration is worth doing at all."
#
# The flag lives on ULevel and it has to be set BEFORE the actors are created:
# an actor's external package is assigned when it is spawned into a level that
# already has external actors turned on. Turning it on afterwards works too (the
# engine externalises on save) but leaves a window where a crash loses the lot.
#
# There is no single blessed Python entry point for this, so the strategies are
# tried in order and the one that worked is logged - and if none does, the bake
# STOPS rather than quietly producing a monolithic .umap that looks fine until
# the second person edits it.
# -----------------------------------------------------------------------------

def persistent_level(world):
    """The ULevel, the long way round.

    UWorld::PersistentLevel and UWorld::Levels are both reflected but PROTECTED,
    so Python cannot read either ("Property 'PersistentLevel' ... is protected
    and cannot be read"). Two public routes exist; both are tried because which
    one a given 5.x build exposes has moved before.
    """
    try:
        level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).get_current_level()
        if level is not None:
            return level
    except Exception:  # noqa: BLE001 - fall through to the other route
        pass

    levels = unreal.EditorLevelUtils.get_levels(world)
    if not levels:
        raise RuntimeError("the world reports no levels")
    return levels[0]


def enable_one_file_per_actor(world):
    level = persistent_level(world)

    errors = []

    try:
        level.set_editor_property("use_external_actors", True)
        if level.get_editor_property("use_external_actors"):
            log("One File Per Actor: ON (ULevel.use_external_actors)")
            return level
        errors.append("ULevel.use_external_actors: set silently did not stick")
    except Exception as exc:  # noqa: BLE001 - report which route failed and try the next
        errors.append("ULevel.use_external_actors: {0}".format(exc))

    raise RuntimeError(
        "could not enable One File Per Actor on the new level. Tried:\n  - "
        + "\n  - ".join(errors)
        + "\nRefusing to bake a monolithic .umap: spec v15 section 1.6 asks for OFPA specifically, "
          "and a map that silently is not OFPA is worse than no map, because the failure only shows "
          "up when the second person edits it."
    )


# -----------------------------------------------------------------------------
# 3. The bake itself
# -----------------------------------------------------------------------------

def bake():
    if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
        if not FORCE:
            raise RuntimeError(
                "{0} already exists. Set TRACE_BAKE_FORCE=1 (or pass --force to Scripts/bake-arena.sh) "
                "to replace it. Refusing to overwrite a level somebody may have edited by hand.".format(MAP_PATH)
            )
        # Delete the external-actor packages with the map. EditorAssetLibrary
        # handles the .umap; the __ExternalActors__ folder is cleaned by the
        # wrapper, which can see the filesystem.
        unreal.EditorAssetLibrary.delete_asset(MAP_PATH)
        log("Deleted the existing {0}".format(MAP_PATH))

        # The per-tint material instances the C++ bake writes. It overwrites them
        # in place if they survive, but a --force re-bake is also the moment a
        # RENAMED tint would otherwise leave an orphan behind, and an orphaned
        # MI_Neon_* that nothing references is exactly the kind of thing that
        # sits in a repo for a year.
        #
        # THE PARENTS ARE NOT TOUCHED. They live one folder over, in Parents/,
        # precisely so that this line cannot reach them: they are authored assets
        # with hand-tuned graphs, and a --force re-bake deleting them would be a
        # silent loss of work.
        if unreal.EditorAssetLibrary.does_directory_exist(INSTANCE_DIR):
            unreal.EditorAssetLibrary.delete_directory(INSTANCE_DIR)
            log("Deleted the existing {0}".format(INSTANCE_DIR))

    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_editor.new_level(MAP_PATH):
        raise RuntimeError("LevelEditorSubsystem.new_level({0}) failed".format(MAP_PATH))
    log("Created an empty level at {0}".format(MAP_PATH))

    # UnrealEditorSubsystem is the 5.x home of get_editor_world; EditorLevelLibrary
    # still forwards to it but is deprecated and logs about it on every call.
    try:
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    except Exception:  # noqa: BLE001 - older builds only have the library
        world = unreal.EditorLevelLibrary.get_editor_world()
    enable_one_file_per_actor(world)

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    # AT THE ORIGIN, and it matters: the whole arena is built in this actor's
    # local space, so the builder's transform IS the arena's transform, and the
    # C++ bake refuses outright if it is scaled.
    builder = actor_subsystem.spawn_actor_from_class(
        unreal.TraceArenaBuilder, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0.0, 0.0, 0.0))
    if builder is None:
        raise RuntimeError("could not spawn ATraceArenaBuilder")
    builder.set_actor_label("Arena_Builder")

    log("Running the build and exploding it into actors. This takes a minute; watch for [Bake] lines.")
    builder.bake_arena_into_level()

    if not builder.get_editor_property("level_is_pre_baked"):
        raise RuntimeError(
            "BakeArenaIntoLevel did not complete - the builder is still marked not-baked. "
            "Scroll up for the [Bake] error line; it names what it refused to do and why."
        )

    # Save the materials FIRST. If the level saved and the material instances did
    # not, every baked block would load with a null material reference.
    unreal.EditorAssetLibrary.save_directory(MATERIAL_DIR, only_if_is_dirty=False, recursive=True)

    if not level_editor.save_current_level():
        raise RuntimeError("save_current_level() failed")

    actors = actor_subsystem.get_all_level_actors()
    log("Saved {0}: {1} actors in the level.".format(MAP_PATH, len(actors)))

    # A per-class census, because "how many actors" is the first question and
    # "of what" is immediately the second.
    census = {}
    for actor in actors:
        name = actor.get_class().get_name()
        census[name] = census.get(name, 0) + 1
    for name in sorted(census, key=lambda key: (-census[key], key)):
        log("  {0:<28} {1}".format(name, census[name]))

    return len(actors)


# -----------------------------------------------------------------------------
# 4. Top-up: add the Core spawn marker to a level that is ALREADY baked
#    (spec v17 section 2)
#
# WHY THIS IS NOT "JUST RE-BAKE". A --force re-bake deletes and rewrites all ~572
# external actor packages - about 5.4 MB of LFS churn, a new version of every
# single actor for everyone to pull, and it throws away every hand edit anybody
# has made to the level. Adding the ONE actor spec v15 section 1.4 asked for
# should not cost that. This opens the existing level, presses the builder's
# EnsureCoreSpawnActor button and saves: one new package, nothing else touched.
#
# Idempotent. A level that already has a marker is left alone, marker included -
# so this cannot move a Core spawn somebody has deliberately dragged.
# -----------------------------------------------------------------------------

def add_core_spawn():
    if not unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
        raise RuntimeError("{0} does not exist; there is nothing to top up. Bake it first.".format(MAP_PATH))

    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_editor.load_level(MAP_PATH):
        raise RuntimeError("could not open {0}".format(MAP_PATH))
    log("Opened {0}".format(MAP_PATH))

    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    builders = [a for a in actor_subsystem.get_all_level_actors()
                if isinstance(a, unreal.TraceArenaBuilder)]
    if not builders:
        raise RuntimeError(
            "{0} has no ATraceArenaBuilder. The builder is the arena's oracle - it is what knows "
            "where the Core spawn goes - so a baked level without one is broken for more reasons "
            "than this.".format(MAP_PATH))
    if len(builders) > 1:
        log_error("{0} contains {1} arena builders; using the first.".format(MAP_PATH, len(builders)))

    before = len([a for a in actor_subsystem.get_all_level_actors()
                  if isinstance(a, unreal.TraceCoreSpawn)])
    builders[0].ensure_core_spawn_actor()
    after = len([a for a in actor_subsystem.get_all_level_actors()
                 if isinstance(a, unreal.TraceCoreSpawn)])

    if after == 0:
        raise RuntimeError("EnsureCoreSpawnActor ran but the level still has no ATraceCoreSpawn.")

    if after == before:
        log("{0} already had a Core spawn marker; nothing changed, nothing saved.".format(MAP_PATH))
        return 0

    if not level_editor.save_current_level():
        raise RuntimeError("save_current_level() failed")

    log("Saved {0} with a placed Core spawn marker ({1} -> {2}).".format(MAP_PATH, before, after))
    return after - before


def main():
    if TOPUP:
        log("Topping up {0}: placing the Core spawn marker if it is missing.".format(MAP_PATH))
        added = add_core_spawn()
        log("bake-arena.py finished: {0} actor(s) added.".format(added))
        return

    log("Baking {0} (materials -> {1}, force={2})".format(MAP_PATH, MATERIAL_DIR, FORCE))
    promote_materials()
    count = bake()
    log("bake-arena.py finished: {0} actors.".format(count))


try:
    main()
except Exception as exc:  # noqa: BLE001 - a commandlet needs the reason in the log
    log_error("bake-arena.py failed: {0}".format(exc))
    raise
