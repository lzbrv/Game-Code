#!/usr/bin/env python3
"""import_portraits.py -- stage 4c of the character pipeline (editor).

Spec: PIPELINE_DESIGN.md SS8.5.

Imports the ten composited busts
    Content/Trace/UI/Art/Source/Portraits/T_Portrait_<Name>.png   (512^2, committed source)
into
    /Game/Trace/UI/Art/Portraits/T_Portrait_<Name>                (UTexture2D)
and reads every one of them back off the loaded asset before saying so.

RUN IT THROUGH THE EDITOR, AND -nullrhi IS FINE HERE
----------------------------------------------------
    "$UE/UnrealEditor-Cmd" Trace.uproject -run=pythonscript \\
        -script=Scripts/import_portraits.py -unattended -nosplash -nopause \\
        -nosound -NullRHI -stdout -FullStdOutLogOutput

A .png import compiles no shader and creates no material, so unlike
Scripts/import-railgun.sh this needs no swap chain; and it is not a .ttf, so
unlike a font import it never calls FSlateApplication::Get() and cannot SIGSEGV
a commandlet. Both of those are import-font-atlas.sh's measured findings, not
guesses, and this script is that script's recipe with one setting changed.

THE ONE SETTING THAT DIFFERS FROM THE ATLAS RECIPE, AND WHY
-----------------------------------------------------------
The glyph sheets import TC_EDITOR_ICON (uncompressed RGBA) because block
compression smears a letter's edge into its neighbour. A portrait has no such
edges: it is a photographic gradient -- a dark radial ground with a lit bust on
it -- and the failure mode there is BANDING in the ground, which is exactly what
DXT1's 5:6:5 endpoints produce. So portraits take TC_BC7: 4x4 blocks with
7-bit-ish endpoints, no banding at this size, 256 KB per texture against 1 MB
uncompressed. SS8.5 records TC_EDITOR_ICON as the fallback if BC7 banding is
ever visible in review; --uncompressed takes it without editing this file.

The rest is the atlas recipe verbatim, and each line still earns its place:
TEXTUREGROUP_UI (never seen at a distance), TMGS_SIMPLE_AVERAGE (the select card
draws these smaller than 512 and the detail panel crops them 2x, so a mip chain
is doing real work), SRGB (the source is an sRGB PNG), never_stream (a character
select screen must not appear before its faces do).

Usage (outside the editor it explains itself and exits non-zero):
    python3 Scripts/import_portraits.py [--src-dir DIR] [--names A,B]
                                        [--uncompressed]
"""
import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)

import character_bodies as cb  # noqa: E402

TEXTURE_DIR = "/Game/Trace/UI/Art/Portraits"
SRC_DIR = os.path.join(_ROOT, "Content", "Trace", "UI", "Art", "Source",
                       "Portraits")
EXPECTED_SIZE = 512

_failures = []


def log(msg):
    # unreal.log when we have it, so the [Trace] filter in the wrappers catches
    # these lines the same way it catches every other pipeline script's.
    line = "[Trace] [import-portraits] {0}".format(msg)
    try:
        import unreal
        unreal.log(line)
    except Exception:
        print(line)


def fail(msg):
    _failures.append(msg)
    log("FAIL  {0}".format(msg))


def set_prop(unreal, obj, snake, camel, value):
    """UE 5.8 renamed several texture properties; try both spellings (the
    generate-menu-widgets.py / import_font_atlas.py convention)."""
    for name in (snake, camel):
        try:
            obj.set_editor_property(name, value)
            return True
        except Exception:
            continue
    fail("could not set {0}/{1} on {2}".format(snake, camel, obj.get_name()))
    return False


def compression_setting(unreal, uncompressed):
    """TC_BC7, or the recorded fallback. Resolved by name rather than assumed to
    exist: a python binding that does not expose BC7 must produce a NAMED
    failure, not a texture that silently imported as DXT1 and bands."""
    want = "TC_EDITOR_ICON" if uncompressed else "TC_BC7"
    value = getattr(unreal.TextureCompressionSettings, want, None)
    if value is None:
        fail("unreal.TextureCompressionSettings has no {0} in this build; "
             "nothing was imported.".format(want))
        return None
    return value


def import_one(unreal, name, compression):
    src = os.path.join(SRC_DIR, "T_Portrait_{0}.png".format(name))
    if not os.path.isfile(src):
        fail("{0} is missing. Run Scripts/compose_portraits.py first (and before "
             "that, Trace.Portrait.CaptureAll in a game run).".format(src))
        return False

    asset_name = "T_Portrait_{0}".format(name)
    path = "{0}/{1}".format(TEXTURE_DIR, asset_name)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", src)
    task.set_editor_property("destination_path", TEXTURE_DIR)
    task.set_editor_property("destination_name", asset_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    texture = unreal.EditorAssetLibrary.load_asset(path)
    if texture is None:
        fail("{0} did not import.".format(path))
        return False

    set_prop(unreal, texture, "lod_group", "LODGroup",
             unreal.TextureGroup.TEXTUREGROUP_UI)
    set_prop(unreal, texture, "compression_settings", "CompressionSettings",
             compression)
    set_prop(unreal, texture, "mip_gen_settings", "MipGenSettings",
             unreal.TextureMipGenSettings.TMGS_SIMPLE_AVERAGE)
    set_prop(unreal, texture, "srgb", "SRGB", True)
    set_prop(unreal, texture, "never_stream", "NeverStream", True)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)

    # READ BACK OFF A RELOADED ASSET, not off the object we just mutated. The
    # generate_content.py:600-640 round-trip rule: the only readback that proves
    # anything is one that went through the package.
    reloaded = unreal.EditorAssetLibrary.load_asset(path)
    if reloaded is None:
        fail("{0} vanished between save and reload.".format(path))
        return False
    width = reloaded.blueprint_get_size_x()
    height = reloaded.blueprint_get_size_y()
    if (width, height) != (EXPECTED_SIZE, EXPECTED_SIZE):
        fail("{0} imported at {1}x{2}, not {3}^2 -- the select card crops a "
             "fixed rectangle out of this, so a different size is a different "
             "portrait.".format(path, width, height, EXPECTED_SIZE))
        return False

    got_group = reloaded.get_editor_property("lod_group")
    got_srgb = reloaded.get_editor_property("srgb")
    got_comp = reloaded.get_editor_property("compression_settings")
    log("imported {0}  ({1}x{2})  group={3} srgb={4} compression={5}".format(
        path, width, height, got_group, got_srgb, got_comp))
    return True


def main():
    ap = argparse.ArgumentParser(description="Import the ten portrait busts.")
    ap.add_argument("--names", default=None)
    ap.add_argument("--src-dir", default=None)
    ap.add_argument("--uncompressed", action="store_true",
                    help="TC_EDITOR_ICON instead of TC_BC7 (SS8.5's recorded "
                         "fallback if BC7 banding shows up in review)")
    # -run=pythonscript hands the whole engine command line to sys.argv on some
    # paths, so unknown arguments are ignored rather than fatal -- and if argparse
    # still finds something it hates in there, the defaults win rather than the run
    # dying four minutes into an editor session. The env vars are the editor-side
    # channel (the convention import_font_atlas.py uses with TRACE_SKIP_HEADER).
    try:
        args, _ = ap.parse_known_args()
    except SystemExit:
        args = ap.parse_args([])
    if os.environ.get("TRACE_PORTRAIT_NAMES"):
        args.names = os.environ["TRACE_PORTRAIT_NAMES"]
    if os.environ.get("TRACE_PORTRAIT_UNCOMPRESSED", "0") == "1":
        args.uncompressed = True

    global SRC_DIR
    if args.src_dir:
        SRC_DIR = args.src_dir

    names = ([n.strip() for n in args.names.split(",") if n.strip()]
             if args.names else list(cb.CHARACTER_ORDER))

    log("=== import_portraits ===")
    log("source {0}".format(SRC_DIR))
    log("dest   {0}".format(TEXTURE_DIR))

    try:
        import unreal
    except ImportError:
        log("no `unreal` module in this interpreter, so nothing was imported. "
            "This step needs the editor -- see the header of this file for the "
            "exact command line.")
        return 2

    compression = compression_setting(unreal, args.uncompressed)
    if compression is None:
        log("EXIT=1")
        return 1

    if not unreal.EditorAssetLibrary.does_directory_exist(TEXTURE_DIR):
        unreal.EditorAssetLibrary.make_directory(TEXTURE_DIR)

    done = 0
    for name in names:
        if import_one(unreal, name, compression):
            done += 1

    log("{0}/{1} portrait textures imported.".format(done, len(names)))
    if _failures or done != len(names):
        for f in _failures:
            log("    {0}".format(f))
        log("EXIT=1")
        return 1
    log("EXIT=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
