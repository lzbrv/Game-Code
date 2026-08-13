# =============================================================================
# Trace — import_font_atlas.py
#
# Takes the glyph atlas that Scripts/generate_font_atlas.py produced and makes
# it usable by the game, in two steps that are deliberately separate:
#
#   Content/Trace/UI/Fonts/Source/T_FontAtlas.json   (metrics, committed)
#        |
#        |  step 1 — PLAIN PYTHON, no editor needed
#        v
#   Source/Trace/UI/Text/TraceFontAtlasMetrics.h     (generated, committed)
#
#   Content/Trace/UI/Fonts/Source/T_FontAtlas.png    (the sheet, committed)
#        |
#        |  step 2 — INSIDE UnrealEditor-Cmd (-run=pythonscript)
#        v
#   Content/Trace/UI/Fonts/T_FontAtlas.uasset        (UTexture2D, committed)
#
# Driven by Scripts/import-font-atlas.sh. Running it by hand is fine too: with
# no `unreal` module importable it does step 1 and says why it stopped.
#
# -----------------------------------------------------------------------------
# WHY THE METRICS BECOME A HEADER AND NOT A DATA ASSET
# -----------------------------------------------------------------------------
# Spec v22 §A1 requires ONE metrics source shared by the Canvas blitter and the
# UMG widget. A header wins over a UDataAsset for three reasons that are about
# failure modes, not taste:
#
#   * A missing/uncooked data asset is a RUNTIME failure that lands on the same
#     day someone ships. A stale header is a COMPILE-time diff in `git status`.
#   * The Canvas HUD draws before any asset the menu owns is guaranteed loaded.
#   * The atlas changes only when somebody re-runs generate_font_atlas.py, which
#     is a source-control event, not a gameplay event.
#
# The texture still has to be a real asset, and that is the one runtime lookup
# TraceText makes — see the STALENESS GUARD in Source/Trace/UI/Text/TraceText.cpp
# (TraceTextFile::Resolve): the imported texture's dimensions are checked against
# the numbers baked here, so re-generating the atlas without re-running this
# script is caught and refused rather than drawn as garbage.
#
# -----------------------------------------------------------------------------
# THE LICENCE POSITION IS UNCHANGED AND IT IS NOT MINE TO MOVE
# -----------------------------------------------------------------------------
# Sofachrome ships under Typodermic's free DESKTOP licence. The .otf is NOT in
# this repository and must not be; the atlas is a rasterisation made with a
# licensed desktop install, which is the same route the artist's sprite sheet
# took. See the header of Scripts/generate_font_atlas.py and docs/FONTS.md —
# both of which flag that a whole-charset atlas is a grey position, and that a
# commercial release should buy the app licence and embed a real font instead.
# This script only moves already-generated pixels into the engine.
# =============================================================================
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SOURCE_DIR = os.path.join(ROOT, "Content", "Trace", "UI", "Fonts", "Source")
JSON_PATH = os.path.join(SOURCE_DIR, "T_FontAtlas.json")
PNG_PATH = os.path.join(SOURCE_DIR, "T_FontAtlas.png")

HEADER_PATH = os.path.join(ROOT, "Source", "Trace", "UI", "Text", "TraceFontAtlasMetrics.h")

# Where the texture lands. C++ names the same path in the generated header, from
# this constant, so there is one spelling of it in the project.
TEXTURE_DIR = os.environ.get("TRACE_FONT_ATLAS_DIR", "/Game/Trace/UI/Fonts")
TEXTURE_NAME = "T_FontAtlas"

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


# -----------------------------------------------------------------------------
# Step 1 — metrics -> a generated header
# -----------------------------------------------------------------------------

def measure_cap_height(meta):
    """
    Cap height in atlas pixels, measured off the sheet's own 'H'.

    The JSON carries ascent/descent/lineHeight because FreeType reports those,
    and NOT cap height, because nothing in the font file states it. Cap height
    is what a UI actually aligns to — the artist's baked word sprites are placed
    by cap height (see TraceMenuArtStyle::word_brush in generate-menu-widgets.py),
    so a caller that wants live text to sit exactly where a sprite word sat needs
    this number and cannot derive it from ascent.

    Falls back to a documented ratio if Pillow is absent, and says so in the
    header rather than pretending the number was measured.
    """
    try:
        from PIL import Image
    except ImportError:
        est = int(round(meta["ascent"] * 0.684))
        return est, "ESTIMATED (Pillow not installed; 0.684 x ascent)"

    if not os.path.isfile(PNG_PATH):
        est = int(round(meta["ascent"] * 0.684))
        return est, "ESTIMATED (the PNG was not on disk)"

    by_char = {c["char"]: c for c in meta["characters"]}
    cell = by_char.get("H")
    if cell is None:
        est = int(round(meta["ascent"] * 0.684))
        return est, "ESTIMATED (the atlas has no 'H')"

    image = Image.open(PNG_PATH).convert("RGBA")
    px = image.load()
    top = None
    bottom = None
    for y in range(cell["v"], cell["v"] + cell["vSize"]):
        row_has_ink = False
        for x in range(cell["u"], cell["u"] + cell["uSize"]):
            if px[x, y][3] > 8:
                row_has_ink = True
                break
        if row_has_ink:
            if top is None:
                top = y
            bottom = y

    if top is None:
        est = int(round(meta["ascent"] * 0.684))
        return est, "ESTIMATED (the 'H' cell is empty)"

    return (bottom - top + 1), "MEASURED off the 'H' cell's ink rows"


def digest(path):
    with open(path, "rb") as handle:
        return hashlib.sha1(handle.read()).hexdigest()


def write_header():
    if not os.path.isfile(JSON_PATH):
        fail("{0} is missing. Run `python3 Scripts/generate_font_atlas.py` first — it needs a "
             "licensed copy of the .otf at Art/Fonts/ (gitignored, per-developer).".format(JSON_PATH))
        return False

    with open(JSON_PATH) as handle:
        meta = json.load(handle)

    chars = sorted(meta["characters"], key=lambda c: c["code"])
    if not chars:
        fail("{0} lists no characters.".format(JSON_PATH))
        return False

    first = chars[0]["code"]
    last = chars[-1]["code"]
    if last - first + 1 != len(chars):
        # The C++ indexes by (code - FirstCode), which is only correct for a
        # contiguous block. Refuse rather than emit a table that silently maps
        # the wrong glyph for every character after the first gap.
        fail("the atlas charset is not contiguous ({0} glyphs spanning {1}..{2}). "
             "TraceFontAtlasMetrics indexes by code and cannot express a gap; either keep the "
             "charset contiguous in generate_font_atlas.py or teach this script to emit a "
             "sparse lookup.".format(len(chars), first, last))
        return False

    cap_height, cap_note = measure_cap_height(meta)
    atlas = meta["atlas"]

    lines = []
    add = lines.append
    add("// GENERATED FILE — DO NOT EDIT BY HAND.")
    add("// Produced by Scripts/import_font_atlas.py from")
    add("// Content/Trace/UI/Fonts/Source/T_FontAtlas.json (sha1 {0}).".format(digest(JSON_PATH)))
    add("//")
    add("// THE ONE METRICS SOURCE (spec v22 §A1). Both renderers — the Canvas blitter in")
    add("// TraceCanvasText.cpp and the Slate leaf in TraceAtlasTextWidget.cpp — lay text out through")
    add("// TraceText::LayoutString(), and TraceText.cpp is the ONLY file in the project that includes")
    add("// this header. That is what makes \"one source\" structural rather than a promise in a")
    add("// comment: the two paths cannot drift because there is only one layout pass.")
    add("//")
    add("// THE CELL MODEL, because it is what makes this table so small:")
    add("//   Every cell is one FULL ADVANCE WIDTH by one FULL LINE HEIGHT, and the glyph is drawn")
    add("//   inside it with its own bearings already applied. So the pen advances by uSize and every")
    add("//   glyph sits on a shared baseline — there is no bearing table and there is no kerning.")
    add("//   Sofachrome is a wide squared face whose ink runs flush to the advance on A, T, V, W and")
    add("//   Y; that is the typeface, not clipping.")
    add("//")
    add("// Units are ATLAS PIXELS at an em of {0}. On screen everything is multiplied by".format(int(meta["pixelSize"])))
    add("// (Size / EmSize), where Size means the same thing it means in FSlateFontInfo::Size.")
    add("#pragma once")
    add("")
    add("#include \"CoreMinimal.h\"")
    add("")
    add("namespace TraceFontAtlasMetrics")
    add("{")
    add("\t/** The typeface this was rasterised from. NOT in the repository — see docs/FONTS.md. */")
    add("\tinline constexpr const TCHAR* SourceFont = TEXT(\"{0}\");".format(meta["source"]))
    add("")
    add("\t/** The imported sheet. Written by Scripts/import_font_atlas.py; loaded once at runtime. */")
    add("\tinline constexpr const TCHAR* TextureAsset =")
    add("\t\tTEXT(\"{0}/{1}.{1}\");".format(TEXTURE_DIR, TEXTURE_NAME))
    add("")
    add("\t/** Checked against the imported texture at runtime — a re-generated atlas that nobody")
    add("\t  * re-imported is caught here and the whole atlas path stands down. */")
    add("\tinline constexpr int32 AtlasWidth  = {0};".format(atlas["width"]))
    add("\tinline constexpr int32 AtlasHeight = {0};".format(atlas["height"]))
    add("")
    add("\t/** Em size the sheet was rasterised at. Everything below is in these pixels. */")
    add("\tinline constexpr float EmSize     = {0}.f;".format(int(meta["pixelSize"])))
    add("\tinline constexpr float Ascent     = {0}.f;".format(int(meta["ascent"])))
    add("\tinline constexpr float Descent    = {0}.f;".format(int(meta["descent"])))
    add("\tinline constexpr float LineHeight  = {0}.f;".format(int(meta["lineHeight"])))
    add("")
    add("\t/** Cap height — {0}. Align to THIS to sit type where a baked word sprite sat. */".format(cap_note))
    add("\tinline constexpr float CapHeight  = {0}.f;".format(int(cap_height)))
    add("")
    add("\tinline constexpr int32 FirstCode = {0};".format(first))
    add("\tinline constexpr int32 LastCode  = {0};".format(last))
    add("\tinline constexpr int32 NumGlyphs = {0};".format(len(chars)))
    add("")
    add("\t/** One cell, in atlas pixels. Index by (code - FirstCode); the charset is contiguous. */")
    add("\tstruct FCell")
    add("\t{")
    add("\t\tuint16 U;")
    add("\t\tuint16 V;")
    add("\t\tuint16 USize;")
    add("\t\tuint16 VSize;")
    add("\t};")
    add("")
    add("\tinline constexpr FCell Cells[NumGlyphs] =")
    add("\t{")
    for c in chars:
        label = c["char"]
        if label == " ":
            label = "space"
        elif label == "\\":
            label = "backslash"
        add("\t\t{{ {0:4d}, {1:4d}, {2:4d}, {3:4d} }},   // {4:3d}  {5}".format(
            c["u"], c["v"], c["uSize"], c["vSize"], c["code"], label))
    add("\t};")
    add("}")
    add("")

    text = "\n".join(lines)

    os.makedirs(os.path.dirname(HEADER_PATH), exist_ok=True)
    if os.path.isfile(HEADER_PATH):
        with open(HEADER_PATH) as handle:
            if handle.read() == text:
                log("{0} is already up to date ({1} glyphs).".format(
                    os.path.relpath(HEADER_PATH, ROOT), len(chars)))
                return True

    with open(HEADER_PATH, "w") as handle:
        handle.write(text)
    log("wrote {0}  ({1} glyphs, em {2}px, cap {3}px — {4})".format(
        os.path.relpath(HEADER_PATH, ROOT), len(chars), int(meta["pixelSize"]), int(cap_height), cap_note))
    return True


# -----------------------------------------------------------------------------
# Step 2 — the sheet -> a UTexture2D (editor only)
# -----------------------------------------------------------------------------

def set_prop(obj, snake, camel, value):
    """UE 5.8 renamed several texture properties; try both spellings, per generate-menu-widgets.py."""
    for name in (snake, camel):
        try:
            obj.set_editor_property(name, value)
            return True
        except Exception:
            continue
    fail("could not set {0}/{1} on {2}".format(snake, camel, obj.get_name()))
    return False


def import_texture(unreal):
    if not os.path.isfile(PNG_PATH):
        fail("{0} is missing. Run `python3 Scripts/generate_font_atlas.py` first.".format(PNG_PATH))
        return False

    path = "{0}/{1}".format(TEXTURE_DIR, TEXTURE_NAME)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", PNG_PATH)
    task.set_editor_property("destination_path", TEXTURE_DIR)
    task.set_editor_property("destination_name", TEXTURE_NAME)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    texture = unreal.EditorAssetLibrary.load_asset(path)
    if texture is None:
        fail("{0} did not import.".format(path))
        return False

    # Every one of these earns its place, and a glyph sheet is stricter than a sprite:
    #
    #   TEXTUREGROUP_UI            never seen at a distance; no streaming pop on a title screen.
    #   TC_EDITOR_ICON             uncompressed RGBA. This is the setting the content browser
    #                              calls "UserInterface2D (RGBA)". Block compression works in 4x4
    #                              blocks and would smear every letter's edge into its neighbour —
    #                              on a glyph sheet that is not a subtle artefact, it is fringing
    #                              on every stroke.
    #   TMGS_NO_MIPMAPS            the cells are separated by a 2 px gutter. Mip level 2 averages
    #                              4x4, which reaches straight through that gutter and pulls the
    #                              NEXT LETTER into this one. A downscaled mip would look tidier
    #                              than bilinear minification and would occasionally draw a ghost
    #                              of the letter beside it; that trade is not worth taking.
    #   SRGB                       the sheet is white ink in the alpha channel; the tint is applied
    #                              in linear space by the shader, so the source must be flagged sRGB
    #                              or every colour comes out too dark.
    #   never_stream               a menu must not appear before its letters do.
    set_prop(texture, "lod_group", "LODGroup", unreal.TextureGroup.TEXTUREGROUP_UI)
    set_prop(texture, "compression_settings", "CompressionSettings",
             unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    set_prop(texture, "mip_gen_settings", "MipGenSettings",
             unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    set_prop(texture, "srgb", "SRGB", True)
    set_prop(texture, "never_stream", "NeverStream", True)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)

    width = texture.blueprint_get_size_x()
    height = texture.blueprint_get_size_y()
    log("imported {0}  ({1} x {2})".format(path, width, height))

    with open(JSON_PATH) as handle:
        atlas = json.load(handle)["atlas"]
    if (width, height) != (atlas["width"], atlas["height"]):
        fail("the imported texture is {0}x{1} but the metrics say {2}x{3}. The runtime guard in "
             "TraceTextAtlas.cpp will refuse the atlas and fall back to Lato. Re-run "
             "Scripts/generate_font_atlas.py and this script together."
             .format(width, height, atlas["width"], atlas["height"]))
        return False

    return True


def main():
    log("=== import_font_atlas ===")

    ok = True
    if os.environ.get("TRACE_SKIP_HEADER", "0") != "1":
        ok = write_header() and ok
    else:
        log("TRACE_SKIP_HEADER=1: leaving TraceFontAtlasMetrics.h alone.")

    try:
        import unreal
    except ImportError:
        log("no `unreal` module in this interpreter, so the TEXTURE was not imported. That step "
            "needs the editor: Scripts/import-font-atlas.sh does both halves.")
        if _failures:
            sys.exit(1)
        return

    ok = import_texture(unreal) and ok

    if _failures:
        log("FAILED with {0} error(s):".format(len(_failures)))
        for message in _failures:
            log("  - {0}".format(message))
        if not hasattr(unreal, "SystemLibrary"):
            sys.exit(1)
    else:
        log("done — the atlas is imported and the metrics header is current.")


main()
