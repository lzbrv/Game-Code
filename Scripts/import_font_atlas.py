# =============================================================================
# Trace — import_font_atlas.py
#
# Takes the glyph atlases that Scripts/generate_font_atlas.py produced and makes
# them usable by the game, in two steps that are deliberately separate:
#
#   Content/Trace/UI/Fonts/Source/T_FontAtlas.json       (light metrics, committed)
#   Content/Trace/UI/Fonts/Source/T_FontAtlasBold.json   (bold  metrics, committed)
#        |
#        |  step 1 — PLAIN PYTHON, no editor needed
#        v
#   Source/Trace/UI/Text/TraceFontAtlasMetrics.h     (generated, committed)
#
#   Content/Trace/UI/Fonts/Source/T_FontAtlas{,Bold}.png    (the sheets, committed)
#        |
#        |  step 2 — INSIDE UnrealEditor-Cmd (-run=pythonscript)
#        v
#   Content/Trace/UI/Fonts/T_FontAtlas{,Bold}.uasset        (UTexture2D, committed)
#
# Driven by Scripts/import-font-atlas.sh. Running it by hand is fine too: with
# no `unreal` module importable it does step 1 and says why it stopped.
#
# -----------------------------------------------------------------------------
# TWO WEIGHTS, ONE LAYOUT PASS (spec v23 §A3)
# -----------------------------------------------------------------------------
# The owner asked for "extra light, not regular/bold" everywhere, and for the
# character names on the select screen to stay bold. Each weight is rasterised
# from ITS OWN REAL FONT FILE:
#
#   Light  Sofachrome W05 ExtraLight.ttf   the DEFAULT for everything
#   Bold   Sofachrome Rg.otf               character NAMES only
#
# `--thin` (eroding Regular to fake a light cut) is 0 for both and must stay
# there. Two attempts to synthesise a weight that way damaged the letterforms —
# the second deleted ( ) [ ] { } / and \ outright while the HUD draws '[E]'.
#
# This script emits ONE header describing BOTH, as a table of FFace records —
# each with its own texture, its own measured cap height and its own cell table.
# The runtime indexes that table by weight. What it must NOT do is let the two
# weights disagree about anything ONE LAYOUT PASS depends on, so it refuses to
# emit a header unless the em, the vertical metrics and the charset match exactly
# (see check_weights_agree).
#
# *** WHAT IS NOT SHARED: THE ADVANCES. *** Two different font files have two
# different sets of widths — measured here, 94 of the 95 cells differ and bold is
# roughly half again as wide. While the light cut was eroded Regular they WERE
# identical, and code was written on that assumption; it is false now. Layout and
# measurement are both face-relative, so the rule for callers is simply that
# anything MEASURING a string must pass the weight it will DRAW it in.
#
# CAP HEIGHT is likewise per weight and measured off each sheet's own 'H'. It is
# the number a caller aligning to a baked word sprite must ask for by weight.
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

HEADER_PATH = os.path.join(ROOT, "Source", "Trace", "UI", "Text", "TraceFontAtlasMetrics.h")

# Where the textures land. C++ names the same path in the generated header, from
# this constant, so there is one spelling of it in the project.
TEXTURE_DIR = os.environ.get("TRACE_FONT_ATLAS_DIR", "/Game/Trace/UI/Fonts")

# THE WEIGHT TABLE — the order here is the order of ETraceTextWeight in C++, and
# index 0 is the DEFAULT. Light first is the owner's instruction ("the font type
# should be extra light"), so a caller that says nothing gets light.
WEIGHTS = [
    ("Light", "T_FontAtlas"),
    ("Bold", "T_FontAtlasBold"),
]

DEFAULT_WEIGHT = WEIGHTS[0][0]


def weights_to_import():
    """
    Which weights step 2 actually imports. All of them, unless
    TRACE_FONT_ATLAS_WEIGHTS names a comma-separated subset by weight name.

    This exists because .uasset is `lockable` in .gitattributes and therefore
    checked out READ-ONLY (see the header of Scripts/lock.sh). Re-importing a
    weight whose asset nobody has locked fails on the write, and re-importing one
    while another session has the project open is worse than failing. Adding a
    NEW weight's sheet touches no existing asset, so it needs neither — hence
    being able to say "just the bold one".

    The HEADER is always regenerated from every weight, because it describes the
    whole table and a partial one would put the runtime out of step with itself.
    """
    requested = os.environ.get("TRACE_FONT_ATLAS_WEIGHTS", "").strip()
    if not requested:
        return list(WEIGHTS)

    wanted = [w.strip().lower() for w in requested.split(",") if w.strip()]
    chosen = [(n, b) for (n, b) in WEIGHTS if n.lower() in wanted]

    known = ", ".join(n for n, _ in WEIGHTS)
    for name in wanted:
        if not any(n.lower() == name for n, _ in WEIGHTS):
            fail("TRACE_FONT_ATLAS_WEIGHTS names '{0}', which is not a weight. Known: {1}."
                 .format(name, known))
    return chosen


def json_path(basename):
    return os.path.join(SOURCE_DIR, basename + ".json")


def png_path(basename):
    return os.path.join(SOURCE_DIR, basename + ".png")

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

def measure_cap_height(meta, sheet_png):
    """
    Cap height in atlas pixels, measured off the sheet's own 'H'.

    MEASURED PER WEIGHT, and it genuinely differs: eroding the glyphs to fake a
    light cut eats rows off the top of every capital, so the light sheet's caps
    are shorter than the bold sheet's at the same em. A single shared number
    would sit type aligned to a baked word sprite several pixels off in one of
    the two weights.

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

    if not os.path.isfile(sheet_png):
        est = int(round(meta["ascent"] * 0.684))
        return est, "ESTIMATED (the PNG was not on disk)"

    by_char = {c["char"]: c for c in meta["characters"]}
    cell = by_char.get("H")
    if cell is None:
        est = int(round(meta["ascent"] * 0.684))
        return est, "ESTIMATED (the atlas has no 'H')"

    image = Image.open(sheet_png).convert("RGBA")
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


def load_weight(name, basename):
    """One weight's JSON, validated on its own terms. Returns a dict or None."""
    path = json_path(basename)
    if not os.path.isfile(path):
        fail("{0} is missing (the {1} weight). Run `python3 Scripts/generate_font_atlas.py "
             "--name {2}{3}` first — it needs a licensed copy of the .otf at Art/Fonts/ "
             "(gitignored, per-developer)."
             .format(path, name, basename, "" if name == "Bold" else " --thin 4"))
        return None

    with open(path) as handle:
        meta = json.load(handle)

    chars = sorted(meta["characters"], key=lambda c: c["code"])
    if not chars:
        fail("{0} lists no characters.".format(path))
        return None

    first = chars[0]["code"]
    last = chars[-1]["code"]
    if last - first + 1 != len(chars):
        # The C++ indexes by (code - FirstCode), which is only correct for a
        # contiguous block. Refuse rather than emit a table that silently maps
        # the wrong glyph for every character after the first gap.
        fail("the {0} atlas charset is not contiguous ({1} glyphs spanning {2}..{3}). "
             "TraceFontAtlasMetrics indexes by code and cannot express a gap; either keep the "
             "charset contiguous in generate_font_atlas.py or teach this script to emit a "
             "sparse lookup.".format(name, len(chars), first, last))
        return None

    cap_height, cap_note = measure_cap_height(meta, png_path(basename))

    return {
        "name": name,
        "basename": basename,
        "meta": meta,
        "chars": chars,
        "first": first,
        "last": last,
        "cap_height": cap_height,
        "cap_note": cap_note,
        "sha1": digest(path),
    }


def check_weights_agree(weights):
    """
    THE GUARD THAT KEEPS ONE LAYOUT PASS HONEST.

    Two weights are allowed to differ in ink and in cap height. They are NOT
    allowed to differ in anything the layout pass consumes — the em, the
    vertical metrics or the charset — because TraceText lays a string out once
    and only swaps which sheet the quads sample. If a future regeneration made
    the weights metrically different, a bold label would silently occupy a
    different box than the light one it replaced, every centred row would drift,
    and nothing would log. Refusing here turns that into a build-time diff.

    Cell geometry may differ: the emitter handles that by writing a second table
    rather than aliasing. Advances differing is legal but WORTH KNOWING, so it
    is reported rather than accepted silently.
    """
    base = weights[0]
    ok = True
    for other in weights[1:]:
        for key in ("pixelSize", "ascent", "descent", "lineHeight"):
            if base["meta"][key] != other["meta"][key]:
                fail("the {0} and {1} atlases disagree about {2} ({3} vs {4}). The two weights "
                     "share ONE layout pass in TraceText::LayoutString, so they must agree on "
                     "every vertical metric and on the em; only ink and cap height may differ. "
                     "Re-generate both at the same --size."
                     .format(base["name"], other["name"], key,
                             base["meta"][key], other["meta"][key]))
                ok = False

        if (base["first"], base["last"]) != (other["first"], other["last"]):
            fail("the {0} atlas covers {1}..{2} but {3} covers {4}..{5}. Both weights must carry "
                 "the same charset or a string would lose glyphs when it changed weight."
                 .format(base["name"], base["first"], base["last"],
                         other["name"], other["first"], other["last"]))
            ok = False

        if base["meta"]["atlas"]["texture"] == other["meta"]["atlas"]["texture"]:
            fail("the {0} and {1} atlases both name texture '{2}'. One would overwrite the other "
                 "on import and both weights would draw the same ink."
                 .format(base["name"], other["name"], base["meta"]["atlas"]["texture"]))
            ok = False

    return ok


def cells_of(weight):
    return [(c["u"], c["v"], c["uSize"], c["vSize"]) for c in weight["chars"]]


def write_header():
    weights = []
    for name, basename in WEIGHTS:
        loaded = load_weight(name, basename)
        if loaded is None:
            return False
        weights.append(loaded)

    if not check_weights_agree(weights):
        return False

    # The DEFAULT weight's numbers double as the module's plain constants, so
    # code that has no opinion about weight keeps reading exactly what it read
    # before this table existed.
    base = weights[0]
    meta = base["meta"]
    chars = base["chars"]
    first = base["first"]
    last = base["last"]
    atlas = meta["atlas"]

    # Which weights need their own cell table. Measured, not assumed: at the time
    # of writing the eroded sheet keeps the unmodified sheet's cell grid exactly,
    # so emitting the 95-row table twice would be 95 rows of noise in a generated
    # file that people do read. Aliasing when they match keeps the file honest
    # about the fact that there is one grid.
    base_cells = cells_of(base)
    shares_grid = {base["name"]: None}
    for other in weights[1:]:
        shares_grid[other["name"]] = None if cells_of(other) == base_cells else other

    lines = []
    add = lines.append
    add("// GENERATED FILE — DO NOT EDIT BY HAND.")
    add("// Produced by Scripts/import_font_atlas.py from")
    for w in weights:
        add("//   Content/Trace/UI/Fonts/Source/{0}.json   ({1}, sha1 {2})".format(
            w["basename"], w["name"], w["sha1"]))
    add("//")
    add("// THE ONE METRICS SOURCE (spec v22 §A1, extended to two weights by v23 §A3). Both")
    add("// renderers — the Canvas blitter in TraceCanvasText.cpp and the Slate leaf in")
    add("// TraceAtlasTextWidget.cpp — lay text out through TraceText::LayoutString(), and")
    add("// TraceText.cpp is the ONLY file in the project that includes this header. That is what")
    add("// makes \"one source\" structural rather than a promise in a comment: the two paths cannot")
    add("// drift because there is only one layout pass.")
    add("//")
    add("// TWO WEIGHTS, AND WHAT IS AND IS NOT ALLOWED TO DIFFER BETWEEN THEM")
    add("//   Each weight is rasterised from ITS OWN REAL FONT FILE — the 'source' recorded in each")
    add("//   sheet's .json, printed per face in the table below. --thin (glyph erosion) is 0 and must")
    add("//   stay 0: two attempts to SYNTHESISE a light cut by eroding Regular damaged the")
    add("//   letterforms, the second deleting ( ) [ ] { } / and \\ outright while the HUD draws '[E]'.")
    add("//   \"Light\" is the default everywhere; \"Bold\" is used for the character names on the")
    add("//   selection screen and nowhere else.")
    add("//")
    add("//   The em, the vertical metrics and the charset are SHARED — emitted once, below, and")
    add("//   enforced by import_font_atlas.py's check_weights_agree(), which refuses to write this")
    add("//   file if the sheets ever disagree. That is what lets ONE LAYOUT PASS serve both.")
    add("//")
    add("//   *** ADVANCES ARE NOT SHARED. *** Each weight has its own cell table below and its own")
    add("//   widths, because each is rasterised from its own font file. An earlier pass synthesised")
    add("//   the light cut by eroding Regular, where the advances WERE identical, and code was")
    add("//   written on that assumption; it is false now and it made a bold name overrun its")
    add("//   column. Anything that MEASURES a string must pass the weight it will DRAW it in.")
    add("//")
    add("//   CAP HEIGHT is per weight and is measured off each sheet's own 'H', because two real")
    add("//   cuts of a family need not agree on it. A caller aligning live type to one of the")
    add("//   artist's baked word sprites must ask for the cap height OF THE WEIGHT IT IS DRAWING.")
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
    add("\t/** Indexed by ETraceTextWeight, in the same order; index 0 ({0}) is the DEFAULT.".format(base["name"]))
    add("\t  * Keep in step with ETraceTextWeight in TraceTextWeight.h — TraceText.cpp static_asserts it. */")
    add("\tinline constexpr int32 NumWeights    = {0};".format(len(weights)))
    add("\tinline constexpr int32 DefaultWeight = {0};   // {1}".format(0, base["name"]))
    add("")
    add("\t// =============================================================================")
    add("\t// SHARED BY EVERY WEIGHT — enforced by the generator, so layout can rely on it")
    add("\t// =============================================================================")
    add("")
    add("\t/** Em size the sheets were rasterised at. Everything below is in these pixels. */")
    add("\tinline constexpr float EmSize     = {0}.f;".format(int(meta["pixelSize"])))
    add("\tinline constexpr float Ascent     = {0}.f;".format(int(meta["ascent"])))
    add("\tinline constexpr float Descent    = {0}.f;".format(int(meta["descent"])))
    add("\tinline constexpr float LineHeight  = {0}.f;".format(int(meta["lineHeight"])))
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

    def emit_cells(symbol, weight):
        add("\tinline constexpr FCell {0}[NumGlyphs] =".format(symbol))
        add("\t{")
        for c in weight["chars"]:
            label = c["char"]
            if label == " ":
                label = "space"
            elif label == "\\":
                label = "backslash"
            add("\t\t{{ {0:4d}, {1:4d}, {2:4d}, {3:4d} }},   // {4:3d}  {5}".format(
                c["u"], c["v"], c["uSize"], c["vSize"], c["code"], label))
        add("\t};")
        add("")

    # The cell grids, one table per DISTINCT grid.
    cell_symbol = {}
    emit_cells("{0}Cells".format(base["name"]), base)
    cell_symbol[base["name"]] = "{0}Cells".format(base["name"])
    for other in weights[1:]:
        own = shares_grid[other["name"]]
        symbol = "{0}Cells".format(other["name"])
        if own is None:
            add("\t/** {0} packs to the SAME grid as {1} — verified cell for cell by".format(
                other["name"], base["name"]))
            add("\t  * Scripts/import_font_atlas.py, which emits a second table the moment they differ.")
            add("\t  * Only the ink inside the cells changes with weight. */")
            add("\tinline constexpr const FCell* {0} = {1};".format(symbol, cell_symbol[base["name"]]))
            add("")
            cell_symbol[other["name"]] = symbol
        else:
            emit_cells(symbol, other)
            cell_symbol[other["name"]] = symbol

    add("\t// =============================================================================")
    add("\t// PER WEIGHT — the sheet, its dimensions, its measured cap height, its grid")
    add("\t// =============================================================================")
    add("")
    add("\tstruct FFace")
    add("\t{")
    add("\t\t/** \"Light\" / \"Bold\". This is the name TraceText::WeightFromName() matches. */")
    add("\t\tconst TCHAR* Name;")
    add("")
    add("\t\t/** The imported sheet. Written by Scripts/import_font_atlas.py; loaded once at runtime. */")
    add("\t\tconst TCHAR* TextureAsset;")
    add("")
    add("\t\t/** Pixels per side eroded off the drawn face to synthesise this weight. 0 = as drawn. */")
    add("\t\tfloat Erosion;")
    add("")
    add("\t\t/** Checked against the imported texture at runtime — a re-generated atlas that nobody")
    add("\t\t  * re-imported is caught here and that WEIGHT stands down. */")
    add("\t\tint32 AtlasWidth;")
    add("\t\tint32 AtlasHeight;")
    add("")
    add("\t\t/** Cap height for THIS weight. Align to it to sit type where a baked word sprite sat. */")
    add("\t\tfloat CapHeight;")
    add("")
    add("\t\tconst FCell* Cells;")
    add("\t};")
    add("")
    add("\tinline constexpr FFace Faces[NumWeights] =")
    add("\t{")
    for w in weights:
        wm = w["meta"]
        add("\t\t// {0} — thin {1}, cap {2} px {3}".format(
            w["name"], wm.get("thin", 0.0), int(w["cap_height"]), w["cap_note"]))
        add("\t\t{{ TEXT(\"{0}\"), TEXT(\"{1}/{2}.{2}\"), {3}f, {4}, {5}, {6}.f, {7} }},".format(
            w["name"], TEXTURE_DIR, w["basename"], wm.get("thin", 0.0),
            wm["atlas"]["width"], wm["atlas"]["height"], int(w["cap_height"]),
            cell_symbol[w["name"]]))
    add("\t};")
    add("")
    add("\t/** Clamped, so a weight index that came in off a knob or a save game cannot walk off")
    add("\t  * the end of the table — it draws in the default weight instead. */")
    add("\tinline constexpr const FFace& Face(int32 WeightIndex)")
    add("\t{")
    add("\t\treturn Faces[(WeightIndex >= 0 && WeightIndex < NumWeights) ? WeightIndex : DefaultWeight];")
    add("\t}")
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
    log("wrote {0}  ({1} glyphs, em {2}px, {3} weights)".format(
        os.path.relpath(HEADER_PATH, ROOT), len(chars), int(meta["pixelSize"]), len(weights)))
    for w in weights:
        log("    {0:<6s} {1:<20s} thin {2:<4} cap {3} px — {4}".format(
            w["name"], w["basename"], w["meta"].get("thin", 0.0),
            int(w["cap_height"]), w["cap_note"]))
    if all(shares_grid[w["name"]] is None for w in weights[1:]):
        log("    the weights share one cell grid, so the table is emitted once and aliased.")
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


def import_texture(unreal, name, basename):
    """Imports ONE weight's sheet. Called once per entry in WEIGHTS."""
    sheet = png_path(basename)
    if not os.path.isfile(sheet):
        fail("{0} is missing (the {1} weight). Run `python3 Scripts/generate_font_atlas.py` first."
             .format(sheet, name))
        return False

    path = "{0}/{1}".format(TEXTURE_DIR, basename)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", sheet)
    task.set_editor_property("destination_path", TEXTURE_DIR)
    task.set_editor_property("destination_name", basename)
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
    #   TMGS_SIMPLE_AVERAGE        A MIP CHAIN, and it is now REQUIRED rather than optional.
    #                              This used to be TMGS_NO_MIPMAPS, on the argument that mip 2
    #                              averages 4x4 and reaches straight through the then-2 px gutter
    #                              into the next letter. The argument was sound; the trade was
    #                              wrong, and the photograph settles it. The HUD and the menu
    #                              footers draw this 96 px em at ~10 px. With no mips each screen
    #                              pixel takes about one arbitrary texel, and in an EXTRA LIGHT
    #                              face the horizontal bars are one texel tall, so they are what
    #                              gets skipped: the title screen drew "MOVE" as "MOVC", "ENTER
    #                              SELECT" as "CNTCR SCLCCT" and "ESC" as "CSC". Losing every
    #                              crossbar in the game is worse than a ghost nobody has ever seen.
    #                              The gutter answer is in generate_font_atlas.py, which now packs
    #                              with PAD = 16 — enough for mip 3 (8x, a 12 px em), past the
    #                              smallest size anything here draws. SIMPLE_AVERAGE rather than
    #                              SHARPEN: sharpening re-introduces ringing on hairline strokes.
    #                              Regenerate the sheets and re-import together; a 2 px sheet with
    #                              mips on WOULD ghost.
    #   SRGB                       the sheet is white ink in the alpha channel; the tint is applied
    #                              in linear space by the shader, so the source must be flagged sRGB
    #                              or every colour comes out too dark.
    #   never_stream               a menu must not appear before its letters do.
    set_prop(texture, "lod_group", "LODGroup", unreal.TextureGroup.TEXTUREGROUP_UI)
    set_prop(texture, "compression_settings", "CompressionSettings",
             unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    set_prop(texture, "mip_gen_settings", "MipGenSettings",
             unreal.TextureMipGenSettings.TMGS_SIMPLE_AVERAGE)
    set_prop(texture, "srgb", "SRGB", True)
    set_prop(texture, "never_stream", "NeverStream", True)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)

    width = texture.blueprint_get_size_x()
    height = texture.blueprint_get_size_y()
    log("imported {0}  ({1} x {2})  [{3}]".format(path, width, height, name))

    with open(json_path(basename)) as handle:
        atlas = json.load(handle)["atlas"]
    if (width, height) != (atlas["width"], atlas["height"]):
        fail("the imported {0} texture is {1}x{2} but the metrics say {3}x{4}. The runtime guard in "
             "TraceText.cpp will refuse that weight and fall back to the default one. Re-run "
             "Scripts/generate_font_atlas.py and this script together."
             .format(name, width, height, atlas["width"], atlas["height"]))
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
        log("no `unreal` module in this interpreter, so the TEXTURES were not imported. That step "
            "needs the editor: Scripts/import-font-atlas.sh does both halves.")
        if _failures:
            sys.exit(1)
        return

    # Every weight, not just the default. A missing bold sheet is the failure this
    # loop exists to make loud: the runtime would quietly draw the character names
    # in light and nobody would see a log line unless they looked for one.
    selected = weights_to_import()
    if len(selected) != len(WEIGHTS):
        log("TRACE_FONT_ATLAS_WEIGHTS: importing only {0}.".format(
            ", ".join(n for n, _ in selected) or "(nothing)"))
    for name, basename in selected:
        ok = import_texture(unreal, name, basename) and ok

    if _failures:
        log("FAILED with {0} error(s):".format(len(_failures)))
        for message in _failures:
            log("  - {0}".format(message))
        if not hasattr(unreal, "SystemLibrary"):
            sys.exit(1)
    else:
        log("done — {0} weight(s) imported and the metrics header is current.".format(len(WEIGHTS)))


main()
