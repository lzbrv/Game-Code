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
# THREE FACES, ONE LAYOUT PASS (spec v23 §A3, third face added by v25 §4)
# -----------------------------------------------------------------------------
# The owner asked for "extra light, not regular/bold" everywhere, for the
# character NAMES on the select screen to stay bold, and — spec v25 §4 — for the
# in-match HUD and the character ability DESCRIPTIONS to be Erbaum Bold. Each
# face is rasterised from ITS OWN REAL FONT FILE:
#
#   Light  Sofachrome W05 ExtraLight.ttf   the DEFAULT: menus and everything else
#   Bold   Sofachrome Rg.otf               character NAMES only
#   Hud    Erbaum-Bold.otf                 the in-match HUD + ability descriptions
#
# "Weight" is now a slight misnomer — Hud is a different FAMILY, not a heavier cut
# of Sofachrome — but it is the axis the runtime already selects on and splitting
# it into family+weight would double every signature for one caller. The enum is
# ETraceTextWeight and Hud is a member of it.
#
# `--thin` (eroding a face to fake a lighter cut) is 0 for all three and must stay
# there. Two attempts to synthesise a weight that way damaged the letterforms —
# the second deleted ( ) [ ] { } / and \ outright while the HUD draws '[E]'.
#
# This script emits ONE header describing ALL of them, as a table of FFace records
# — each with its own texture, its own source file, its own vertical metrics, its
# own measured cap height and its own cell table. The runtime indexes that table
# by weight. What it must NOT do is let the faces disagree about anything ONE
# LAYOUT PASS depends on, so it refuses to emit a header unless the em, the LINE
# HEIGHT and the charset match exactly (see check_weights_agree).
#
# -----------------------------------------------------------------------------
# ...AND A FOURTH SHEET THAT IS NOT ONE OF THEM (UI plan WP12)
# -----------------------------------------------------------------------------
#   Names  Lato-Regular.ttf (OFL)  T_FontAtlasNames  the per-glyph FALLBACK
#
# Everything above this line is about the three faces a CALLER can ask for. The
# fourth sheet is not askable: it is reached one GLYPH at a time, for a codepoint
# the chosen face has no cell for — an accented player name, in practice. It is
# rasterised over Latin-1 rather than printable ASCII, it does NOT share the
# licensed faces' line box or charset, it is NOT in ETraceTextWeight, and it is
# guarded by check_fallback() rather than by check_weights_agree(). See the
# FALLBACK block further down, and the banner it emits into the header.
#
# *** WHAT IS NOT SHARED: THE ADVANCES. *** Three different font files have three
# different sets of widths — measured here, 94 of the 95 cells differ between the
# two Sofachrome cuts and Erbaum is narrower than either. While the light cut was
# eroded Regular they WERE identical, and code was written on that assumption; it
# is false now. Layout and measurement are both face-relative, so the rule for
# callers is simply that anything MEASURING a string must pass the weight it will
# DRAW it in.
#
# *** ALSO NOT SHARED, AND THIS IS NEW IN v25: ASCENT AND DESCENT. *** Erbaum-Bold
# splits the same 116 px line box as 93/23 where both Sofachrome cuts split it
# 95/21. The LINE HEIGHT is what multi-line layout advances by and it still has to
# agree — it does, exactly — but the baseline inside that box is a property of the
# face, so it is emitted per face and TraceText::Ascent() takes a weight. A single
# shared ascent would sit an EVAlign::Baseline draw 2 atlas px (0.6 px at size 28)
# off in the HUD face, and every CapTop alignment with it.
#
# CAP HEIGHT is likewise per face and measured off each sheet's own 'H'. It is
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
#
# APPEND, never insert. The index is the enum value and the enum value is what a
# saved knob, a data-asset column and Blueprint all carry; putting Hud in the
# middle would silently re-point every one of them at a different sheet.
WEIGHTS = [
    ("Light", "T_FontAtlas"),
    ("Bold", "T_FontAtlasBold"),
    ("Hud", "T_FontAtlasHud"),
]

DEFAULT_WEIGHT = WEIGHTS[0][0]

# -----------------------------------------------------------------------------
# THE FALLBACK FACE — AND WHY IT IS NOT IN THE TABLE ABOVE (UI plan WP12)
# -----------------------------------------------------------------------------
# T_FontAtlasNames is Lato, rasterised over Latin-1 rather than printable ASCII.
# It exists because a PLAYER NAME is the one string this game does not author:
# "Björn" arriving over the network had no cell for its ö and drew a hole in the
# kill feed. Extending the Sofachrome or Erbaum sheets to cover it was ruled out —
# that would rasterise more of two faces this public repo may not redistribute.
# Lato is OFL, its .ttf is already committed, and so its sheet may be.
#
# IT IS DELIBERATELY NOT A FOURTH WEIGHT, and that distinction is the whole design:
#
#   * A weight is something a CALLER ASKS FOR. Nothing should ever ask to draw in
#     the fallback — it is reached per GLYPH, by a codepoint the chosen face has no
#     cell for, and never by a style. Adding it to ETraceTextWeight would put "Lato"
#     in a UMG dropdown and in WeightFromName(), inviting exactly the two-typefaces
#     defect spec v23 §A4 removed.
#   * A weight must satisfy check_weights_agree(): same em, same LINE HEIGHT, same
#     contiguous charset. This face satisfies NONE of the last two. Lato splits a
#     137 px line box at em 96 where the licensed faces split a 116 px one, and its
#     charset has nine holes in it (the C1 block, the soft hyphen, the gaps between
#     Latin-1 and the typographic marks). It is emitted with its own RANGE TABLE and
#     its own baseline, and the runtime shifts it onto the drawing face's baseline.
#
# So it gets its own section of the generated header and its own guard below.
FALLBACK = ("Names", "T_FontAtlasNames")

# Every sheet that has to reach /Game/Trace/UI/Fonts. The fallback goes LAST so that
# WEIGHTS keeps being indexable by ETraceTextWeight — the enum's values are what saved
# knobs, data-asset columns and Blueprint pins carry, and "Names" is not one of them.
ALL_SHEETS = WEIGHTS + [FALLBACK]


def sheets_to_import():
    """
    Which sheets step 2 actually imports. All of them, unless
    TRACE_FONT_ATLAS_WEIGHTS names a comma-separated subset by name.

    This exists because .uasset is `lockable` in .gitattributes and therefore
    checked out READ-ONLY (see the header of Scripts/lock.sh). Re-importing a
    weight whose asset nobody has locked fails on the write, and re-importing one
    while another session has the project open is worse than failing. Adding a
    NEW weight's sheet touches no existing asset, so it needs neither — hence
    being able to say "just the bold one", or "just Names" the day WP12 landed.

    The HEADER is always regenerated from every sheet, because it describes the
    whole table and a partial one would put the runtime out of step with itself.
    """
    requested = os.environ.get("TRACE_FONT_ATLAS_WEIGHTS", "").strip()
    if not requested:
        return list(ALL_SHEETS)

    wanted = [w.strip().lower() for w in requested.split(",") if w.strip()]
    chosen = [(n, b) for (n, b) in ALL_SHEETS if n.lower() in wanted]

    known = ", ".join(n for n, _ in ALL_SHEETS)
    for name in wanted:
        if not any(n.lower() == name for n, _ in ALL_SHEETS):
            fail("TRACE_FONT_ATLAS_WEIGHTS names '{0}', which is not a sheet. Known: {1}."
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


def runs_of(codes):
    """
    A sorted codepoint list collapsed into contiguous [first, last] runs.

    The same function, for the same reason, as in Scripts/generate_font_atlas.py:
    one run means the C++ can index by (code - FirstCode); more than one means it
    needs the range table this emits for the fallback face.
    """
    runs = []
    for code in sorted(codes):
        if runs and code == runs[-1][1] + 1:
            runs[-1][1] = code
        else:
            runs.append([code, code])
    return [(a, b) for a, b in runs]


def measure_ink_extent(meta, sheet_png):
    """
    The topmost and bottommost INK row any cell on this sheet uses, relative to the
    top of its cell. Returns (top, bottom, note), or (None, None, note).

    This is what proves the fallback face can be baseline-shifted onto a licensed
    face's line box WITHOUT its ink leaving that box — see check_fallback(). Doing it
    by measurement rather than by argument matters: Lato's 137 px line box is 21 px
    taller than the 116 px one the licensed faces share, and "the diacritics probably
    fit" is not something a reader of the header should have to take on trust.

    PIL's getbbox() on a cropped cell does the scan in C. The per-pixel loop in
    measure_cap_height above is fine for one cell and would not be for 199.
    """
    try:
        from PIL import Image
    except ImportError:
        return None, None, "NOT MEASURED (Pillow not installed)"

    if not os.path.isfile(sheet_png):
        return None, None, "NOT MEASURED (the PNG was not on disk)"

    image = Image.open(sheet_png).convert("RGBA")
    alpha = image.getchannel("A")

    top = None
    bottom = None
    for c in meta["characters"]:
        box = alpha.crop((c["u"], c["v"], c["u"] + c["uSize"], c["v"] + c["vSize"])).getbbox()
        if box is None:
            continue                        # space and friends: no ink, nothing to bound
        if top is None or box[1] < top:
            top = box[1]
        if bottom is None or box[3] > bottom:
            bottom = box[3]

    if top is None:
        return None, None, "NOT MEASURED (the sheet has no ink at all)"
    return top, bottom, "MEASURED across all {0} cells".format(len(meta["characters"]))


def load_face(name, basename, sparse=False):
    """
    One sheet's JSON, validated on its own terms. Returns a dict or None.

    @param sparse  False (every WEIGHT) demands a CONTIGUOUS charset, because the
                   runtime indexes a weight's cells by (code - FirstCode). True (the
                   fallback face only) allows holes and reports the runs instead —
                   Latin-1 has a C1 block and a soft hyphen in the middle of it and
                   cannot be contiguous by construction.
    """
    path = json_path(basename)
    if not os.path.isfile(path):
        # NO --thin in this message, deliberately. It used to suggest `--thin 4` for
        # anything that was not Bold, which is the exact flag that ate ( ) [ ] { }
        # and / off a sheet while the HUD draws '[E]'. Every face here is rendered
        # as drawn, from its own real font file.
        fail("{0} is missing (the {1} sheet). Run `python3 Scripts/generate_font_atlas.py "
             "--font \"Art/Fonts/<the face>\" --name {2}` first — for the three LICENSED faces "
             "that needs your own copy at Art/Fonts/ (gitignored, per-developer) and no --thin; "
             "for the OFL fallback it is `--font Art/Fonts/Lato-Regular.ttf --charset latin1`."
             .format(path, name, basename))
        return None

    with open(path) as handle:
        meta = json.load(handle)

    chars = sorted(meta["characters"], key=lambda c: c["code"])
    if not chars:
        fail("{0} lists no characters.".format(path))
        return None

    first = chars[0]["code"]
    last = chars[-1]["code"]
    runs = runs_of([c["code"] for c in chars])
    if not sparse and len(runs) != 1:
        # The C++ indexes a weight by (code - FirstCode), which is only correct for a
        # contiguous block. Refuse rather than emit a table that silently maps
        # the wrong glyph for every character after the first gap.
        fail("the {0} atlas charset is not contiguous ({1} glyphs spanning {2}..{3} in {4} runs). "
             "TraceFontAtlasMetrics indexes a WEIGHT by code and cannot express a gap. Keep the "
             "licensed sheets on `--charset ascii`; a holey charset belongs to the FALLBACK face, "
             "which is emitted with a range table instead."
             .format(name, len(chars), first, last, len(runs)))
        return None

    cap_height, cap_note = measure_cap_height(meta, png_path(basename))

    return {
        "name": name,
        "basename": basename,
        "meta": meta,
        "chars": chars,
        "first": first,
        "last": last,
        "runs": runs,
        "charset": meta.get("charset", "ascii"),
        "cap_height": cap_height,
        "cap_note": cap_note,
        "sha1": digest(path),
    }


def check_weights_agree(weights):
    """
    THE GUARD THAT KEEPS ONE LAYOUT PASS HONEST.

    A face may differ from the others in ink, in advances, in cap height and — as
    of v25 — in where its baseline sits inside the line box. It may NOT differ in
    anything the layout PASS consumes, which is exactly two numbers and a charset:

        pixelSize    the em every cell is expressed in. Everything on screen is
                     (Size / EmSize) x these pixels, so two ems means two scales
                     for one string and the second face draws at the wrong size.
        lineHeight   what LayoutString advances by between lines of one string.
                     Two line heights means a two-line label in the wrong face
                     overlaps itself, and nothing logs.
        the charset  the runtime indexes cells by (code - FirstCode) against ONE
                     pair of bounds, so a face covering a different range would
                     read the wrong cell — or off the end — for every character.

    ASCENT AND DESCENT ARE NO LONGER IN THAT LIST, and that is the v25 change.
    They are how the shared line box is SPLIT, not how big it is: Erbaum-Bold
    splits 116 px as 93/23 where both Sofachrome cuts split it 95/21. Only
    EVAlign::Baseline and EVAlign::CapTop read them, per draw, and both are
    per-face questions — "where is this face's baseline" has no shared answer.
    They are emitted per face and TraceText::Ascent() takes a weight. Requiring
    them to match would have refused a face whose layout is perfectly compatible.

    Cell geometry may differ: the emitter handles that by writing a second table
    rather than aliasing. Advances differing is legal but WORTH KNOWING, so it
    is reported rather than accepted silently.
    """
    base = weights[0]
    ok = True
    for other in weights[1:]:
        for key in ("pixelSize", "lineHeight"):
            if base["meta"][key] != other["meta"][key]:
                fail("the {0} and {1} atlases disagree about {2} ({3} vs {4}). Every face "
                     "shares ONE layout pass in TraceText::LayoutString, so they must agree on "
                     "the em and on the line height; ink, advances, cap height and the "
                     "ascent/descent SPLIT of that line box may differ. Re-generate both at the "
                     "same --size."
                     .format(base["name"], other["name"], key,
                             base["meta"][key], other["meta"][key]))
                ok = False

        for key in ("ascent", "descent"):
            if base["meta"][key] != other["meta"][key]:
                log("note: {0} and {1} split the {2} px line box differently ({3} {4} vs {5} {4}). "
                    "That is legal and per-face from v25 on — TraceText::Ascent() takes a weight — "
                    "but a caller drawing with EVAlign::Baseline or CapTop MUST pass the weight it "
                    "is drawing in, or the row sits off by the difference."
                    .format(base["name"], other["name"], int(base["meta"]["lineHeight"]),
                            base["meta"][key], key, other["meta"][key]))

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


def check_fallback(weights, fallback):
    """
    THE GUARD FOR THE FALLBACK FACE, and it asks a different question.

    check_weights_agree() enforces what one LAYOUT PASS needs of a weight. The
    fallback face is never laid out — it is reached one GLYPH at a time, dropped
    into a line that some other face is setting. So the em must still match (the
    whole thing is scaled by Size/EmSize and there is only one EmSize), but the line
    height and the charset are explicitly allowed to differ, and DO:
    Lato splits a 137 px box at em 96 against the licensed faces' 116, and its
    charset has holes where the C1 controls and the soft hyphen were skipped.

    What replaces those two checks is THE ONE THING A DROPPED-IN GLYPH CAN GET WRONG:
    once it is shifted so its baseline sits on the drawing face's baseline, does its
    INK still fit inside the drawing face's line box? If it does not, an accented
    capital collides with the line above it and a descender with the line below, in a
    multi-line label, and nothing logs. Measured here against EVERY weight rather
    than argued: the shift is (weight ascent - fallback ascent), which is most
    negative for the face with the smallest ascent, so the top is checked against the
    smallest and the bottom against the largest.
    """
    ok = True
    fmeta = fallback["meta"]
    base = weights[0]

    if fmeta["pixelSize"] != base["meta"]["pixelSize"]:
        fail("the {0} fallback sheet is em {1} but the weights are em {2}. Everything on screen "
             "is (Size / EmSize) x atlas pixels and there is ONE EmSize, so a fallback glyph would "
             "draw at the wrong size inside a correct line. Re-generate it at --size {2}."
             .format(fallback["name"], fmeta["pixelSize"], base["meta"]["pixelSize"]))
        ok = False

    for w in weights:
        if w["meta"]["atlas"]["texture"] == fmeta["atlas"]["texture"]:
            fail("the {0} weight and the {1} fallback both name texture '{2}'. One would overwrite "
                 "the other on import.".format(w["name"], fallback["name"],
                                               fmeta["atlas"]["texture"]))
            ok = False

    ink_top, ink_bottom, ink_note = measure_ink_extent(fmeta, png_path(fallback["basename"]))
    fallback["ink_top"] = ink_top
    fallback["ink_bottom"] = ink_bottom
    fallback["ink_note"] = ink_note

    if ink_top is None:
        log("note: the {0} fallback sheet's ink extent was {1}, so the line-box fit below is "
            "UNVERIFIED this run. It is checked whenever Pillow is available."
            .format(fallback["name"], ink_note))
        return ok

    line_h = base["meta"]["lineHeight"]
    for w in weights:
        shift = w["meta"]["ascent"] - fmeta["ascent"]
        top = ink_top + shift
        bottom = ink_bottom + shift
        if top < 0 or bottom > line_h:
            fail("a {0} glyph dropped into a {1} line would put ink at {2}..{3} inside a 0..{4} px "
                 "line box (shift {5} px, from ascent {6} onto ascent {7}). Multi-line labels would "
                 "collide. Rasterise the fallback from a face with tighter diacritics, or stop "
                 "sharing one line box."
                 .format(fallback["name"], w["name"], top, bottom, line_h, shift,
                         fmeta["ascent"], w["meta"]["ascent"]))
            ok = False

    return ok


def cells_of(weight):
    return [(c["u"], c["v"], c["uSize"], c["vSize"]) for c in weight["chars"]]


def emit_fallback(add, weights, fallback, base_meta):
    """
    The FALLBACK FACE section of the generated header (UI plan WP12).

    Everything the runtime needs to drop ONE Lato glyph into a line the licensed
    faces are setting: a range table (the charset has holes), a cell table, the
    face record, and the index of '?' for a codepoint even Lato has no cell for.
    """
    fmeta = fallback["meta"]
    fchars = fallback["chars"]
    runs = fallback["runs"]
    line_h = int(base_meta["lineHeight"])

    add("\t// =============================================================================")
    add("\t// THE FALLBACK FACE — one glyph at a time, and NOT a weight (UI plan WP12)")
    add("\t// =============================================================================")
    add("\t//")
    add("\t// THE PROBLEM IT SOLVES. The three sheets above are printable ASCII. Every string this")
    add("\t// game AUTHORS is inside that set; the one class of string it does not author is a PLAYER")
    add("\t// NAME, and a \"Björn\" arriving over the network used to draw a hole where its ö should be")
    add("\t// (it advanced by a space, so at least the row's box stayed honest — but a hole is a hole).")
    add("\t//")
    add("\t// WHY THE FIX IS A FOURTH SHEET AND NOT MORE CELLS IN THE FIRST THREE. Sofachrome and")
    add("\t// Erbaum are licensed for desktop use and this repository is public; rasterising ANOTHER")
    add("\t// 96 codepoints of them would deepen exactly the licensing exposure docs/FONTS.md flags.")
    add("\t// {0} is OFL, its .ttf is already committed, and so is this sheet.".format(fmeta["source"]))
    add("\t//")
    add("\t// WHY IT IS NOT A FOURTH ETraceTextWeight. A weight is something a CALLER ASKS FOR, and")
    add("\t// nothing should ever ask for this one — it is reached per GLYPH by a codepoint the chosen")
    add("\t// face has no cell for. Putting it in the enum would put \"Lato\" in a UMG dropdown and in")
    add("\t// WeightFromName(), which is the two-typefaces defect spec v23 §A4 removed.")
    add("\t//")
    add("\t// WHAT IT SHARES WITH THE WEIGHTS, AND WHAT IT DOES NOT:")
    add("\t//   SHARED:      EmSize. Everything is scaled by (Size / EmSize) and there is one EmSize,")
    add("\t//                so a fallback glyph in a correct line comes out the correct size. Enforced")
    add("\t//                by import_font_atlas.py's check_fallback().")
    add("\t//   NOT SHARED:  the LINE BOX. {0} splits {1} px at this em where the weights split {2}.".format(
        fmeta["source"], int(fmeta["lineHeight"]), line_h))
    add("\t//                A fallback glyph is therefore drawn at (weight ascent - fallback ascent)")
    add("\t//                px from the line top, which puts its baseline on the line's baseline.")
    add("\t//   NOT SHARED:  the CHARSET. {0} codepoints in {1} runs — the C1 block, U+00AD and the".format(
        len(fchars), len(runs)))
    add("\t//                gaps before the typographic marks are all skipped on purpose. That is why")
    add("\t//                this face has a RANGE TABLE and the weights have a FirstCode.")
    add("\t//")
    if fallback.get("ink_top") is not None:
        add("\t// AND THE ONE THING THAT COULD GO WRONG, MEASURED RATHER THAN ARGUED: a dropped-in glyph")
        add("\t// whose ink left the shared line box would collide with the line above or below it in a")
        add("\t// multi-line label, silently. This sheet's ink spans rows {0}..{1} of its own {2} px cell".format(
            fallback["ink_top"], fallback["ink_bottom"], int(fmeta["lineHeight"])))
        add("\t// ({0}), so after the baseline shift it occupies:".format(fallback["ink_note"]))
        for w in weights:
            shift = int(w["meta"]["ascent"]) - int(fmeta["ascent"])
            add("\t//     in a {0:<5s} line (ascent {1}): shift {2:+d} px  ->  ink {3}..{4}  inside 0..{5}".format(
                w["name"], int(w["meta"]["ascent"]), shift,
                fallback["ink_top"] + shift, fallback["ink_bottom"] + shift, line_h))
        add("\t// import_font_atlas.py refuses to write this file if any of those rows leaves the box.")
    else:
        add("\t// NOTE: the ink-extent fit could not be measured this run ({0}).".format(
            fallback.get("ink_note", "reason unrecorded")))
    add("")
    add("\tinline constexpr int32 NumFallbackGlyphs = {0};".format(len(fchars)))
    add("\tinline constexpr int32 NumFallbackRanges = {0};".format(len(runs)))
    add("")
    add("\t/** One contiguous run of the fallback charset. Cells[FirstIndex + (Code - First)] is the")
    add("\t  * cell for Code, for any Code in [First, Last]. */")
    add("\tstruct FCodeRange")
    add("\t{")
    add("\t\tint32 First;")
    add("\t\tint32 Last;")
    add("\t\tint32 FirstIndex;")
    add("\t};")
    add("")
    add("\tinline constexpr FCodeRange FallbackRanges[NumFallbackRanges] =")
    add("\t{")
    index_by_code = {c["code"]: i for i, c in enumerate(fchars)}
    for (lo, hi) in runs:
        add("\t\t{{ {0:6d}, {1:6d}, {2:4d} }},   // U+{0:04X}..U+{1:04X}, {3} glyph(s)".format(
            lo, hi, index_by_code[lo], hi - lo + 1))
    add("\t};")
    add("")
    add("\tinline constexpr FCell FallbackCells[NumFallbackGlyphs] =")
    add("\t{")
    for c in fchars:
        label = c["char"]
        if label == " ":
            label = "space"
        elif label == "\\":
            label = "backslash"
        elif c["code"] == 0xA0:
            label = "no-break space"
        elif not (32 < c["code"] < 127):
            label = "U+{0:04X}  {1}".format(c["code"], label)
        add("\t\t{{ {0:4d}, {1:4d}, {2:4d}, {3:4d} }},   // {4:5d}  {5}".format(
            c["u"], c["v"], c["uSize"], c["vSize"], c["code"], label))
    add("\t};")
    add("")
    question = index_by_code.get(ord("?"))
    add("\t/** '?' in the table above — what a codepoint MISSING FROM BOTH sheets draws. A visible")
    add("\t  * question mark is the honest answer there; advancing silently is what produced the hole")
    add("\t  * this face exists to remove. */")
    add("\tinline constexpr int32 FallbackQuestionIndex = {0};".format(question))
    add("")
    add("\t// {0} — {1}, ascent {2}/descent {3}, cap {4} px {5}".format(
        fallback["name"], fmeta["source"], int(fmeta["ascent"]), int(fmeta["descent"]),
        int(fallback["cap_height"]), fallback["cap_note"]))
    add("\tinline constexpr FFace FallbackFace =")
    add("\t\t{{ TEXT(\"{0}\"), TEXT(\"{1}\"), TEXT(\"{2}/{3}.{3}\"), {4}f, {5}, {6}, {7}.f, {8}.f, {9}.f, FallbackCells }};".format(
        fallback["name"], fmeta["source"], TEXTURE_DIR, fallback["basename"],
        fmeta.get("thin", 0.0), fmeta["atlas"]["width"], fmeta["atlas"]["height"],
        int(fmeta["ascent"]), int(fmeta["descent"]), int(fallback["cap_height"])))
    add("")
    add("\t/** The index into FallbackCells for @p Code, or INDEX_NONE. Linear over {0} ranges — it".format(len(runs)))
    add("\t  * is only ever reached for a codepoint the DRAWING face already failed to supply, which")
    add("\t  * is a handful of glyphs in a player name and never a whole authored string. */")
    add("\tinline constexpr int32 FallbackIndexOf(int32 Code)")
    add("\t{")
    add("\t\tfor (const FCodeRange& Range : FallbackRanges)")
    add("\t\t{")
    add("\t\t\tif (Code >= Range.First && Code <= Range.Last)")
    add("\t\t\t{")
    add("\t\t\t\treturn Range.FirstIndex + (Code - Range.First);")
    add("\t\t\t}")
    add("\t\t}")
    add("\t\treturn INDEX_NONE;")
    add("\t}")


def write_header():
    weights = []
    for name, basename in WEIGHTS:
        loaded = load_face(name, basename)
        if loaded is None:
            return False
        weights.append(loaded)

    if not check_weights_agree(weights):
        return False

    fallback = load_face(FALLBACK[0], FALLBACK[1], sparse=True)
    if fallback is None:
        return False
    if not check_fallback(weights, fallback):
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
    add("//   Content/Trace/UI/Fonts/Source/{0}.json   ({1}, the FALLBACK face, sha1 {2})".format(
        fallback["basename"], fallback["name"], fallback["sha1"]))
    add("//")
    add("// THE ONE METRICS SOURCE (spec v22 §A1, two weights by v23 §A3, a third face by v25 §4). Both")
    add("// renderers — the Canvas blitter in TraceCanvasText.cpp and the Slate leaf in")
    add("// TraceAtlasTextWidget.cpp — lay text out through TraceText::LayoutString(), and")
    add("// TraceText.cpp is the ONLY file in the project that includes this header. That is what")
    add("// makes \"one source\" structural rather than a promise in a comment: the two paths cannot")
    add("// drift because there is only one layout pass.")
    add("//")
    add("// THE FACES, AND WHAT IS AND IS NOT ALLOWED TO DIFFER BETWEEN THEM")
    add("//   Each face is rasterised from ITS OWN REAL FONT FILE — the 'source' recorded in each")
    add("//   sheet's .json, carried per face in the table below. --thin (glyph erosion) is 0 for every")
    add("//   one of them and must stay 0: two attempts to SYNTHESISE a light cut by eroding Regular")
    add("//   damaged the letterforms, the second deleting ( ) [ ] { } / and \\ outright while the HUD")
    add("//   draws '[E]'. \"Light\" is the default everywhere; \"Bold\" is the character NAMES on the")
    add("//   selection screen; \"Hud\" (spec v25 §4) is the in-match HUD and the ability DESCRIPTIONS,")
    add("//   and it is a different FAMILY rather than a heavier cut — the enum axis is called weight")
    add("//   because that is what the runtime already selected on.")
    add("//")
    add("//   *** WHAT ONE LAYOUT PASS ACTUALLY REQUIRES, and it is only this: *** the em, the LINE")
    add("//   HEIGHT and the charset. Those three are emitted once, below, and enforced by")
    add("//   import_font_atlas.py's check_weights_agree(), which refuses to write this file if the")
    add("//   WEIGHTS ever disagree about them. *** THE FALLBACK FACE AT THE BOTTOM OF THIS FILE IS")
    add("//   NOT A WEIGHT AND IS NOT BOUND BY THOSE THREE *** — it is never laid out, only dropped")
    add("//   into a line one glyph at a time, so it shares the em and nothing else. See its own")
    add("//   section for what replaces the other two guarantees.")
    add("//")
    add("//   *** ADVANCES ARE NOT SHARED. *** Each face has its own cell table below and its own")
    add("//   widths, because each is rasterised from its own font file. An earlier pass synthesised")
    add("//   the light cut by eroding Regular, where the advances WERE identical, and code was")
    add("//   written on that assumption; it is false now and it made a bold name overrun its")
    add("//   column. Anything that MEASURES a string must pass the weight it will DRAW it in.")
    add("//")
    add("//   *** ASCENT AND DESCENT ARE NOT SHARED EITHER (v25). *** They are how the shared line box")
    add("//   is SPLIT, not how tall it is: Erbaum-Bold splits 116 px as 93/23 where both Sofachrome")
    add("//   cuts split it 95/21. Only EVAlign::Baseline and EVAlign::CapTop read them. The module")
    add("//   constants below are the DEFAULT face's, kept so code with no opinion reads what it always")
    add("//   read; TraceText::Ascent() takes a weight and answers per face.")
    add("//")
    add("//   CAP HEIGHT is per face and is measured off each sheet's own 'H'. A caller aligning live")
    add("//   type to one of the artist's baked word sprites must ask for the cap height OF THE WEIGHT")
    add("//   IT IS DRAWING.")
    add("//")
    add("// THE CELL MODEL, because it is what makes this table so small:")
    add("//   Every cell is one FULL ADVANCE WIDTH by one FULL LINE HEIGHT, and the glyph is drawn")
    add("//   inside it with its own bearings already applied. So the pen advances by uSize and every")
    add("//   glyph sits on a shared baseline — there is no bearing table and there is no kerning.")
    add("//   Sofachrome is a wide squared face whose ink runs flush to the advance on A, T, V, W and")
    add("//   Y; that is the typeface, not clipping. Erbaum is a narrower squared grotesque and does")
    add("//   the same on its own diagonals.")
    add("//")
    add("// Units are ATLAS PIXELS at an em of {0}. On screen everything is multiplied by".format(int(meta["pixelSize"])))
    add("// (Size / EmSize), where Size means the same thing it means in FSlateFontInfo::Size.")
    add("#pragma once")
    add("")
    add("#include \"CoreMinimal.h\"")
    add("")
    add("namespace TraceFontAtlasMetrics")
    add("{")
    add("\t/** The DEFAULT face's typeface. NOT in the repository — see docs/FONTS.md. Per-face")
    add("\t  * sources are in FFace::Source below; this one is what names the family in a caption. */")
    add("\tinline constexpr const TCHAR* SourceFont = TEXT(\"{0}\");".format(meta["source"]))
    add("")
    add("\t/** Indexed by ETraceTextWeight, in the same order; index 0 ({0}) is the DEFAULT.".format(base["name"]))
    add("\t  * Keep in step with ETraceTextWeight in TraceTextWeight.h — TraceText.cpp static_asserts it. */")
    add("\tinline constexpr int32 NumWeights    = {0};".format(len(weights)))
    add("\tinline constexpr int32 DefaultWeight = {0};   // {1}".format(0, base["name"]))
    add("")
    add("\t// =============================================================================")
    add("\t// SHARED BY EVERY WEIGHT — enforced by the generator, so layout can rely on it")
    add("\t// (the FALLBACK face at the bottom shares only EmSize; see its own banner)")
    add("\t// =============================================================================")
    add("")
    add("\t/** Em size the sheets were rasterised at. Everything below is in these pixels. */")
    add("\tinline constexpr float EmSize     = {0}.f;".format(int(meta["pixelSize"])))
    add("\t/** What one line of a multi-line string advances by. THE shared vertical number. */")
    add("\tinline constexpr float LineHeight  = {0}.f;".format(int(meta["lineHeight"])))
    add("")
    add("\t/** The DEFAULT face's split of that line box. PER FACE from v25 — see FFace::Ascent.")
    add("\t  * Kept at module scope so callers with no opinion about weight read what they always")
    add("\t  * read; anything aligning to a baseline in a NON-default face must ask FFace. */")
    add("\tinline constexpr float Ascent     = {0}.f;".format(int(meta["ascent"])))
    add("\tinline constexpr float Descent    = {0}.f;".format(int(meta["descent"])))
    add("")
    add("\t/** The WEIGHTS' charset — one contiguous block, which is what lets them be indexed by")
    add("\t  * subtraction. The fallback face's charset is wider and has holes; it carries its own")
    add("\t  * range table rather than a First/Last pair. */")
    add("\tinline constexpr int32 FirstCode = {0};".format(first))
    add("\tinline constexpr int32 LastCode  = {0};".format(last))
    add("\tinline constexpr int32 NumGlyphs = {0};".format(len(chars)))
    add("")
    add("\t/** One cell, in atlas pixels. For a WEIGHT, index by (code - FirstCode); the weights'")
    add("\t  * charset is contiguous. The fallback face uses FallbackCell() instead. */")
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
    add("\t\t/** \"Light\" / \"Bold\" / \"Hud\". The name TraceText::WeightFromName() matches. The")
    add("\t\t  * fallback face reuses this struct and calls itself \"{0}\", but it is NOT in Faces[]".format(fallback["name"]))
    add("\t\t  * and WeightFromName() must never return it — nothing may ASK to draw in it. */")
    add("\t\tconst TCHAR* Name;")
    add("")
    add("\t\t/** The font file this face was rasterised from. This is what a screenshot caption has to")
    add("\t\t  * print to IDENTIFY the face rather than assert a flag — see spec v25 §4. */")
    add("\t\tconst TCHAR* Source;")
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
    add("\t\t/** THIS face's split of ITS line box. Baseline and CapTop alignment read these.")
    add("\t\t  * For every WEIGHT, Ascent + Descent == LineHeight — that is what check_weights_agree()")
    add("\t\t  * enforces. THE FALLBACK FACE IS THE EXCEPTION: its box is taller ({0} px against {1}),".format(
        int(fallback["meta"]["lineHeight"]), int(meta["lineHeight"])))
    add("\t\t  * so its Ascent + Descent does NOT equal LineHeight, and the difference in Ascent is")
    add("\t\t  * exactly the shift that puts a fallback glyph on the drawing face's baseline. */")
    add("\t\tfloat Ascent;")
    add("\t\tfloat Descent;")
    add("")
    add("\t\t/** Cap height for THIS face. Align to it to sit type where a baked word sprite sat. */")
    add("\t\tfloat CapHeight;")
    add("")
    add("\t\tconst FCell* Cells;")
    add("\t};")
    add("")
    add("\tinline constexpr FFace Faces[NumWeights] =")
    add("\t{")
    for w in weights:
        wm = w["meta"]
        add("\t\t// {0} — {1}, thin {2}, ascent {3}/descent {4}, cap {5} px {6}".format(
            w["name"], wm["source"], wm.get("thin", 0.0),
            int(wm["ascent"]), int(wm["descent"]), int(w["cap_height"]), w["cap_note"]))
        add("\t\t{{ TEXT(\"{0}\"), TEXT(\"{1}\"), TEXT(\"{2}/{3}.{3}\"), {4}f, {5}, {6}, {7}.f, {8}.f, {9}.f, {10} }},".format(
            w["name"], wm["source"], TEXTURE_DIR, w["basename"], wm.get("thin", 0.0),
            wm["atlas"]["width"], wm["atlas"]["height"],
            int(wm["ascent"]), int(wm["descent"]), int(w["cap_height"]),
            cell_symbol[w["name"]]))
    add("\t};")
    add("")
    add("\t/** Clamped, so a weight index that came in off a knob or a save game cannot walk off")
    add("\t  * the end of the table — it draws in the default weight instead. */")
    add("\tinline constexpr const FFace& Face(int32 WeightIndex)")
    add("\t{")
    add("\t\treturn Faces[(WeightIndex >= 0 && WeightIndex < NumWeights) ? WeightIndex : DefaultWeight];")
    add("\t}")
    add("")
    emit_fallback(add, weights, fallback, meta)
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
    log("wrote {0}  ({1} glyphs, em {2}px, {3} weights + 1 fallback face)".format(
        os.path.relpath(HEADER_PATH, ROOT), len(chars), int(meta["pixelSize"]), len(weights)))
    for w in weights:
        log("    {0:<6s} {1:<20s} thin {2:<4} cap {3} px — {4}".format(
            w["name"], w["basename"], w["meta"].get("thin", 0.0),
            int(w["cap_height"]), w["cap_note"]))
    log("    {0:<6s} {1:<20s} FALLBACK, charset {2}: {3} glyphs in {4} range(s), line box {5} px "
        "(the weights' is {6})".format(
            fallback["name"], fallback["basename"], fallback["charset"],
            len(fallback["chars"]), len(fallback["runs"]),
            int(fallback["meta"]["lineHeight"]), int(meta["lineHeight"])))
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
    """Imports ONE sheet. Called once per entry in ALL_SHEETS (the weights, then the fallback)."""
    sheet = png_path(basename)
    if not os.path.isfile(sheet):
        fail("{0} is missing (the {1} sheet). Run `python3 Scripts/generate_font_atlas.py` first."
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

    # Every sheet, not just the default. A missing bold sheet is the failure this
    # loop exists to make loud: the runtime would quietly draw the character names
    # in light and nobody would see a log line unless they looked for one. The
    # fallback sheet is in this list too — a missing one is quieter still, because
    # it only shows up on a name nobody on this machine is called.
    selected = sheets_to_import()
    if len(selected) != len(ALL_SHEETS):
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
        log("done — {0} sheet(s) imported ({1} weights + the {2} fallback face) and the metrics "
            "header is current.".format(len(selected), len(WEIGHTS), FALLBACK[0]))


main()
