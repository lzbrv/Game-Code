# =============================================================================
# Trace - generate_map.py
#
# Creates and saves an EMPTY level at /Game/Maps/Arena, headlessly.
#
# Trace builds its entire playfield from C++ (ATraceArenaBuilder spawns the
# floor, walls, endzones, lights and player starts at BeginPlay), so the map
# asset needs no content at all. It exists only because Unreal must open *some*
# level. That makes it pure boilerplate, which is exactly the kind of thing
# nobody should have to click through the editor to produce.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
# Convenience wrapper (finds the engine for you):
#
#     Scripts/generate-map.sh
#
# The raw command it wraps, macOS:
#
#     "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
#         "/Users/gregorykaraev/trace/Trace.uproject" \
#         -run=pythonscript \
#         -script="/Users/gregorykaraev/trace/Scripts/generate_map.py" \
#         -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
# Windows (same switches, different binary):
#
#     "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
#         "C:\path\to\Trace.uproject" ^
#         -run=pythonscript -script="C:\path\to\Scripts\generate_map.py" ^
#         -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
# You can also paste it into the editor's Output Log Python prompt:
#
#     exec(open("/Users/gregorykaraev/trace/Scripts/generate_map.py").read())
#
# -----------------------------------------------------------------------------
# REQUIREMENTS
# -----------------------------------------------------------------------------
# * The "Python Editor Script Plugin" (PythonScriptPlugin) must be enabled in
#   Trace.uproject. Without it, -run=pythonscript fails with
#   "The 'pythonscript' commandlet could not be found".
# * -run=pythonscript boots the full editor in commandlet mode, so this needs an
#   editor build of the project (Scripts/build.sh) - not the server target.
# * Adding -nullrhi speeds it up on machines with no display, but some editor
#   subsystems behave differently without an RHI; if a strategy below fails,
#   drop -nullrhi first before debugging anything else.
#
# -----------------------------------------------------------------------------
# OPTIONS (environment variables - the pythonscript commandlet has no clean way
# to pass argv through -script=, so env vars are the reliable channel)
# -----------------------------------------------------------------------------
#   TRACE_MAP_PATH   package path to create. Default: /Game/Maps/Arena
#   TRACE_FORCE_MAP  "1" to delete and recreate an existing map. Default: off
# =============================================================================

import os
import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write(
        "generate_map.py must run inside Unreal Editor's Python environment.\n"
        "Use Scripts/generate-map.sh, or the -run=pythonscript command line in\n"
        "this file's header.\n"
    )
    raise SystemExit(2)


MAP_PACKAGE_PATH = os.environ.get("TRACE_MAP_PATH", "/Game/Maps/Arena").rstrip("/")
FORCE_RECREATE = os.environ.get("TRACE_FORCE_MAP", "0") == "1"


def log(message):
    unreal.log("[Trace] {0}".format(message))


def log_warning(message):
    unreal.log_warning("[Trace] {0}".format(message))


def log_error(message):
    unreal.log_error("[Trace] {0}".format(message))


def ensure_parent_directory(package_path):
    """Create /Game/Maps if it does not exist yet.

    A fresh clone has Content/Maps/ (kept alive by .gitkeep) but Unreal's asset
    registry only knows about directories that contain assets, so the API-level
    directory may still be 'missing' as far as EditorAssetLibrary is concerned.
    """
    parent = package_path.rsplit("/", 1)[0]
    if not parent or parent == "/Game":
        return
    try:
        if not unreal.EditorAssetLibrary.does_directory_exist(parent):
            unreal.EditorAssetLibrary.make_directory(parent)
            log("Created content directory {0}".format(parent))
    except Exception as exc:  # noqa: BLE001 - never let a nicety abort the run
        log_warning("Could not pre-create {0} ({1}); continuing anyway.".format(parent, exc))


# -----------------------------------------------------------------------------
# Creation strategies, tried in order. The Unreal Python API for "make me a new
# level" has moved twice across 5.x, and which of these exists depends on the
# engine version, so we probe rather than assume. Each returns True on success.
# -----------------------------------------------------------------------------

def strategy_level_editor_subsystem(package_path):
    """UE 5.0+ : the current, non-deprecated path."""
    subsystem_class = getattr(unreal, "LevelEditorSubsystem", None)
    if subsystem_class is None:
        return False
    subsystem = unreal.get_editor_subsystem(subsystem_class)
    if subsystem is None or not hasattr(subsystem, "new_level"):
        return False
    log("Trying unreal.LevelEditorSubsystem.new_level()")
    # new_level() both creates the level and writes the package to disk.
    return bool(subsystem.new_level(package_path))


def strategy_editor_level_library(package_path):
    """UE 4.x / early 5.x : deprecated but still present in most 5.x builds."""
    library = getattr(unreal, "EditorLevelLibrary", None)
    if library is None or not hasattr(library, "new_level"):
        return False
    log("Trying unreal.EditorLevelLibrary.new_level()")
    return bool(library.new_level(package_path))


def strategy_loading_and_saving_utils(package_path):
    """Lowest-level fallback: make a blank world, then save it where we want."""
    utils = getattr(unreal, "EditorLoadingAndSavingUtils", None)
    if utils is None or not hasattr(utils, "new_blank_map"):
        return False
    log("Trying unreal.EditorLoadingAndSavingUtils.new_blank_map() + save_map()")
    # save_existing_map=False: this is a throwaway commandlet process, and
    # prompting to save is exactly what "headless" is supposed to avoid.
    world = utils.new_blank_map(False)
    if world is None:
        return False
    if not hasattr(utils, "save_map"):
        return False
    return bool(utils.save_map(world, package_path))


STRATEGIES = (
    strategy_level_editor_subsystem,
    strategy_editor_level_library,
    strategy_loading_and_saving_utils,
)


def flush_to_disk(package_path):
    """Belt-and-braces save. new_level() normally saves already; this catches the
    case where a strategy created the level in memory but left it dirty."""
    saver = getattr(unreal, "LevelEditorSubsystem", None)
    if saver is not None:
        try:
            subsystem = unreal.get_editor_subsystem(saver)
            if subsystem is not None and hasattr(subsystem, "save_current_level"):
                subsystem.save_current_level()
        except Exception as exc:  # noqa: BLE001
            log_warning("save_current_level() failed ({0}); trying save_asset().".format(exc))
    try:
        if unreal.EditorAssetLibrary.does_asset_exist(package_path):
            unreal.EditorAssetLibrary.save_asset(package_path, only_if_is_dirty=False)
    except Exception as exc:  # noqa: BLE001
        log_warning("save_asset({0}) failed ({1}).".format(package_path, exc))


def main():
    log("Generating empty level at {0}".format(MAP_PACKAGE_PATH))

    already_exists = unreal.EditorAssetLibrary.does_asset_exist(MAP_PACKAGE_PATH)
    if already_exists and not FORCE_RECREATE:
        log("{0} already exists - nothing to do.".format(MAP_PACKAGE_PATH))
        log("Set TRACE_FORCE_MAP=1 to delete and recreate it.")
        return 0

    if already_exists and FORCE_RECREATE:
        log_warning("TRACE_FORCE_MAP=1 - deleting existing {0}".format(MAP_PACKAGE_PATH))
        unreal.EditorAssetLibrary.delete_asset(MAP_PACKAGE_PATH)

    ensure_parent_directory(MAP_PACKAGE_PATH)

    created = False
    for strategy in STRATEGIES:
        try:
            if strategy(MAP_PACKAGE_PATH):
                created = True
                break
            log_warning("{0} not available or returned false.".format(strategy.__name__))
        except Exception as exc:  # noqa: BLE001 - try every strategy before giving up
            log_warning("{0} raised: {1}".format(strategy.__name__, exc))

    flush_to_disk(MAP_PACKAGE_PATH)

    # does_asset_exist() is the only answer that matters - a strategy returning
    # True while writing nothing to disk would otherwise pass silently.
    if not unreal.EditorAssetLibrary.does_asset_exist(MAP_PACKAGE_PATH):
        log_error("Failed to create {0}.".format(MAP_PACKAGE_PATH))
        log_error("Every API strategy failed. Fall back to the editor:")
        log_error("  File > New Level > Empty Level, then Save As -> Content/Maps/Arena")
        return 1

    if not created:
        log_warning("The asset exists but no strategy reported success - verify it opens.")

    log("Created {0}".format(MAP_PACKAGE_PATH))
    log("On disk: Content/{0}.umap".format(MAP_PACKAGE_PATH[len("/Game/"):]))
    log("The level is intentionally EMPTY - ATraceArenaBuilder builds the arena at BeginPlay.")
    log("Commit it with Git LFS tracking active (Scripts/setup-lfs.sh).")
    return 0


if __name__ == "__main__":
    EXIT_CODE = main()
    if EXIT_CODE != 0:
        # Non-zero exit so the shell wrapper and any CI step see the failure.
        raise SystemExit(EXIT_CODE)
