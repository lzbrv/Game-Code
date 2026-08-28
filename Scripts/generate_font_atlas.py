#!/usr/bin/env python3
# =============================================================================
# Trace — generate_font_atlas.py
#
# Renders a typeface into a BITMAP GLYPH ATLAS: one texture holding every
# character, plus a JSON metrics file describing where each one sits. The engine
# side turns these into an Unreal "offline" UFont, which draws text from the
# texture instead of from font software.
#
# WHY A BITMAP ATLAS AND NOT AN EMBEDDED FONT
#   Sofachrome ships under Typodermic's FREE DESKTOP licence. Its terms allow
#   "app (not embedded)" and explicitly forbid "app (embedded)". This project's
#   repository is also PUBLIC, so committing the .otf would be redistribution on
#   top of embedding. An atlas is a rasterised image made with a licensed
#   desktop installation, which is the same route the artist's existing sprite
#   sheet took.
#
#   *** BE AWARE, AND THIS WAS FLAGGED TO THE PROJECT OWNER WHO CHOSE IT ANYWAY:
#   an atlas covering the whole character set reconstitutes the typeface well
#   enough to set arbitrary text, which is arguably the thing the licence means
#   to prevent. It is a greyer position than pre-rendering a fixed list of words.
#   If this game is ever distributed commercially, buy Typodermic's app licence
#   and switch to a real embedded font — the engine-side constant makes that a
#   small change. ***
#
# THE FONT FILE IS NOT IN THE REPOSITORY, BY DESIGN.
#   Put your own licensed copy at Art/Fonts/ (gitignored) and re-run this. The
#   pattern matches Scripts/import-mannequin.sh: art that we may use but may not
#   redistribute is imported per-developer, never committed.
#
# THE ONE EXCEPTION TO ALL OF THAT: Lato (--charset latin1, UI plan WP12).
#   Lato ships under the SIL Open Font License. Its .ttf IS in this repository
#   (Art/Fonts/Lato-Regular.ttf, licence text in Art/Fonts/README.md) and so may
#   its atlas be, without any of the grey area above. That sheet exists to give
#   the text stack a per-glyph FALLBACK for codepoints the licensed faces were
#   never rasterised for — accented player names, mostly — so that a "Björn"
#   arriving over the network draws its ö instead of a hole. See WP12 and
#   Source/Trace/UI/Text/TraceText.cpp.
#
# Usage:
#   python3 Scripts/generate_font_atlas.py [--font PATH] [--size PX] [--preview]
#                                          [--charset ascii|latin1] [--name NAME]
# =============================================================================
import argparse
import json
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError:
    sys.exit("[Trace] Pillow is required:  python3 -m pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFAULT_FONT = os.path.join(ROOT, "Art", "Fonts", "Sofachrome Rg.otf")
OUT_DIR = os.path.join(ROOT, "Content", "Trace", "UI", "Fonts", "Source")

# =============================================================================
# THE CHARSETS
# =============================================================================
# `ascii` is what every licensed sheet in this project is and must stay: printable
# ASCII, 95 cells, one contiguous block 32..126. The menu needs upper case, digits
# and punctuation; lower case and the rest are cheap and stop a stray string
# rendering as blanks. IT IS THE DEFAULT so that re-running this script for
# Sofachrome or Erbaum produces byte-identical output to what is committed.
#
# `latin1` (UI plan WP12) is for the OFL fallback sheet ONLY. It is deliberately
# NOT contiguous, and every gap in it is a decision:
#
#   32..126     printable ASCII, so the fallback can stand in for a whole string
#               (it is also the `-TraceNoFontAtlas` face, and a fallback that
#               could not draw "PLAY" would be no fallback at all).
#   127..159    SKIPPED. DEL and the C1 control block. They have no glyphs, and a
#               .notdef box drawn for a control character is worse than nothing.
#   160..255    the Latin-1 Supplement — the accented letters this whole work
#               package exists for, plus ¿ ¡ « » ° § © and the rest.
#   173         SKIPPED inside that range: U+00AD SOFT HYPHEN is an invisible
#               line-break opportunity, not a character. Rasterising it would put
#               a stray hyphen in any name that carried one.
#   EXTRAS      a short, explicitly listed set of TYPOGRAPHIC punctuation that
#               real UI strings carry and Latin-1 does not have: en/em dash,
#               curly quotes, bullet, ellipsis, euro. This is a deliberate,
#               documented extension of the plan's "chr(32..126) + chr(160..255)"
#               and it costs nine cells. The reason is concrete: WP12's own
#               acceptance specimen is "BJÖRN — ÀÉÎÕÜ ¿¡", whose dash is U+2014
#               and is NOT in Latin-1, so a strict Latin-1 sheet would render the
#               acceptance string with a '?' in the middle of it. The runtime's
#               lookup is a range table either way (the charset has holes in it
#               regardless), so extra runs are free.
#
# A non-contiguous charset is exactly why Scripts/import_font_atlas.py emits the
# fallback face as a RANGE TABLE instead of the (code - FirstCode) indexing the
# three licensed faces use. Keep that in step with this.
ASCII_CODES = list(range(32, 127))

LATIN1_EXTRA_CODES = [
    0x2013,   # – en dash
    0x2014,   # — em dash        <- WP12's acceptance specimen carries this one
    0x2018,   # ' left single quote
    0x2019,   # ' right single quote / apostrophe
    0x201C,   # " left double quote
    0x201D,   # " right double quote
    0x2022,   # • bullet
    0x2026,   # … ellipsis
    0x20AC,   # € euro
]

LATIN1_CODES = (
    ASCII_CODES
    + [c for c in range(160, 256) if c != 0x00AD]
    + LATIN1_EXTRA_CODES
)

CHARSETS = {
    "ascii": ASCII_CODES,
    "latin1": LATIN1_CODES,
}

# The default is what the licensed sheets were made with. Do not change it.
DEFAULT_CHARSET = "ascii"

# Faces whose OWN FONT FILE is committed to this repository, and the licence that
# makes that legal. Everything else this script rasterises is a licensed desktop
# install whose .otf/.ttf is gitignored (see the header), and its sheet inherits
# that grey position. Keyed by file name so the note is attached AUTOMATICALLY —
# a Lato sheet cannot be generated without it and then read later as if it carried
# the same restrictions as the Sofachrome ones.
OFL_FONTS = {
    "Lato-Regular.ttf":
        "SIL Open Font License 1.1 (Art/Fonts/README.md). BOTH this sheet and the font "
        "file it was rasterised from are committable, unlike the Sofachrome and Erbaum "
        "sheets in this directory.",
}

# The atlas may not grow past this. The packer flows rows at MAX_WIDTH and then
# rounds the height up to a power of two, so `latin1` lands on 2048x2048 where
# `ascii` lands on 2048x1024. Anything past 2048 would be a 16 MB+ uncompressed
# UI texture and is a decision somebody should have to make on purpose.
MAX_HEIGHT = 2048

# THE GUTTER IS 16 PX BECAUSE THE SHEET NEEDS A MIP CHAIN, and that is the whole
# reason this number changed from 2 (spec v23, integration pass).
#
# The HUD and the menu footers draw this 96 px em at about 10 px, an ~11x
# minification. Sampling a sheet with NO mips at 11x means each screen pixel takes
# essentially one arbitrary texel, and in an EXTRA LIGHT face the horizontal bars
# are one texel tall — so they are the strokes that get skipped. Photographed on
# the title screen before this change: "MOVE" drew as "MOVC", "ENTER SELECT" as
# "CNTCR SCLCCT" and "ESC" as "CSC". Every E, F and H lost its crossbars while the
# stems and diagonals survived, which is exactly the signature of undersampling
# rather than of a faint face.
#
# A mip chain fixes that by pre-averaging, but a mip AVERAGES ACROSS THE GUTTER:
# mip level N mixes 2^N texels, and a bilinear tap on it reaches ~2^N source texels
# each side. The importer used to refuse mips for that reason with PAD = 2, where
# even mip 2 reaches through into the next letter. 16 px covers mip 3 (8x, i.e.
# down to a 12 px em) with a texel to spare, which is past the smallest size
# anything in this game draws. Cells are FULL ADVANCE boxes and Sofachrome runs its
# ink flush to the advance on A, T, V, W and Y, so this gutter is the only
# separation there is — it cannot be inferred from the glyph's bearings.
#
# Cost: the light sheet grows 2048x512 -> 2048x1024, i.e. 8 MB uncompressed. Both
# sheets must be generated with the SAME value or their cell grids stop matching
# and one layout pass can no longer serve both weights.
PAD = 16
MAX_WIDTH = 2048      # atlas width; height grows in powers of two to fit


SS = 2                # supersample factor used when thinning


def runs_of(codes):
    """
    A sorted codepoint list collapsed into contiguous [first, last] runs.

    Used for reporting here and — the reason it exists — as the shape the runtime
    lookup takes: `ascii` is one run and can be indexed by (code - first), while
    `latin1` is nine and cannot. Scripts/import_font_atlas.py derives the fallback
    face's range table the same way, from the same .json.
    """
    runs = []
    for code in sorted(codes):
        if runs and code == runs[-1][1] + 1:
            runs[-1][1] = code
        else:
            runs.append([code, code])
    return [(a, b) for a, b in runs]


def build(font_path, px, out_dir, preview, thin, name="T_FontAtlas",
          charset=DEFAULT_CHARSET):
    if not os.path.isfile(font_path):
        sys.exit(
            "[Trace] font not found: {0}\n"
            "        Put your licensed copy there. It is gitignored on purpose —\n"
            "        see the header of this script.".format(font_path))

    if charset not in CHARSETS:
        sys.exit("[Trace] unknown --charset {0}. Known: {1}.".format(
            charset, ", ".join(sorted(CHARSETS))))
    codes = CHARSETS[charset]
    chars = "".join(chr(c) for c in codes)

    font = ImageFont.truetype(font_path, px)
    ascent, descent = font.getmetrics()
    line_h = ascent + descent

    # A face asked for a charset it does not cover would rasterise .notdef boxes and
    # nobody would find out until a name drew as a row of rectangles. Say so here.
    missing = []
    try:
        from fontTools.ttLib import TTFont
        cmap = TTFont(font_path).getBestCmap()
        missing = [c for c in codes if c not in cmap]
    except ImportError:
        pass                                    # fontTools is optional; the check is a bonus
    if missing:
        sys.exit("[Trace] {0} has no glyph for {1} of the {2} codepoints in --charset {3}: {4}\n"
                 "        Pick a face that covers it, or narrow the charset.".format(
                     os.path.basename(font_path), len(missing), len(codes), charset,
                     " ".join("U+{0:04X}".format(c) for c in missing[:12])))

    # --- measure -------------------------------------------------------------
    # Each cell is a FULL ADVANCE WIDTH by a FULL LINE HEIGHT. That wastes some
    # atlas space, but it means the engine can advance the pen by the cell width
    # and sit every glyph on a shared baseline with no per-glyph bearing table —
    # which is exactly what Unreal's offline font format can express.
    cells = []
    for ch in chars:
        adv = int(round(font.getlength(ch)))
        if adv <= 0:
            adv = int(round(px * 0.3))          # space and friends
        cells.append({"char": ch, "w": adv, "h": line_h})

    # --- pack (shelf) --------------------------------------------------------
    x = y = row_h = 0
    for c in cells:
        w = c["w"] + PAD * 2
        h = c["h"] + PAD * 2
        if x + w > MAX_WIDTH:
            x = 0
            y += row_h
            row_h = 0
        c["x"] = x + PAD
        c["y"] = y + PAD
        x += w
        row_h = max(row_h, h)
    total_h = y + row_h

    atlas_h = 1
    while atlas_h < total_h:
        atlas_h *= 2
    atlas_w = MAX_WIDTH

    # ASSERT-FIT. The rows flowed; whether the result is a texture anyone should ship
    # is a separate question, and it is answered here rather than by a 4096-tall sheet
    # appearing in Content/ without comment.
    if atlas_h > MAX_HEIGHT:
        sys.exit(
            "[Trace] {0} glyphs of {1} at em {2} need a {3}x{4} sheet, and the ceiling is "
            "{3}x{5}.\n"
            "        Drop the em size or narrow the charset — do NOT raise MAX_HEIGHT without "
            "checking\n"
            "        what an uncompressed UI texture that size costs.".format(
                len(cells), os.path.basename(font_path), px, atlas_w, atlas_h, MAX_HEIGHT))

    # --- render --------------------------------------------------------------
    #
    # THINNING. Sofachrome ships only as Regular (usWeightClass 400) — the free
    # download has no light cut and the face is not variable, so there is no
    # ExtraLight to select. The artist's own lettering is hairline, so the weight
    # is synthesised here: each glyph is rendered at SS times the final size, its
    # coverage is eroded by a square kernel, and it is then downsampled. Eroding
    # by k pixels at SS scale narrows every stroke by 2k/SS pixels at final size,
    # uniformly, which is exactly what a lighter weight of a geometric face looks
    # like. Downsampling last keeps the thin strokes anti-aliased instead of
    # crumbling them.
    #
    # This is a synthesis, not the real thing. If Typodermic sell a genuine
    # ExtraLight, that will always beat this — drop it in Art/Fonts/ and pass
    # --thin 0.
    # Render the WHOLE SHEET at SS scale, erode it in one pass, then downsample.
    #
    # The first version of this did it per glyph — drawing each letter into an
    # oversized padded cell, eroding that, and pasting it back at a negative
    # offset. It clipped glyphs at the sheet edges and mangled others: PLAY lost
    # the A's left diagonal and the Y became a stub. Doing it sheet-wide has no
    # offsets to get wrong.
    #
    # Eroding across the whole sheet cannot bleed between neighbours: a minimum
    # filter only ever SHRINKS coverage, so a neighbour's ink can never be added
    # to this glyph — at worst the transparent gutter between them eats a little
    # more of each edge, which is the thinning we are asking for anyway.
    if thin > 0:
        big = ImageFont.truetype(font_path, px * SS)
        sheet = Image.new("L", (atlas_w * SS, atlas_h * SS), 0)
        d = ImageDraw.Draw(sheet)
        for c in cells:
            d.text((c["x"] * SS, c["y"] * SS), c["char"], font=big, fill=255)

        # ADAPTIVE, PER GLYPH. Uniform erosion is not safe past about one pixel:
        # measured on this face, --thin 3 DELETES ( ) [ ] { } / and \\ outright,
        # and the HUD draws "[E]". Heavy letters can take far more thinning than
        # already-thin punctuation, so each glyph is eroded on its own and stops
        # as soon as it has lost too much of itself. Letters reach the requested
        # weight; brackets keep their shape.
        #
        # Each cell is processed IN PLACE at its own coordinates. The first
        # version of this rendered glyphs into padded cells and pasted them back
        # at an offset, which clipped them — PLAY lost the A's left diagonal and
        # the Y became a stub. There are no offsets here to get wrong.
        passes = max(1, int(round(thin * SS)))
        floor = 0.45          # never let a glyph fall below this share of its ink
        thinned = {}
        for c in cells:
            box = (c["x"] * SS, c["y"] * SS,
                   (c["x"] + c["w"]) * SS, (c["y"] + c["h"]) * SS)
            cell = sheet.crop(box)
            start_ink = sum(cell.point(lambda v: 255 if v > 110 else 0)
                            .convert("L").getdata()) / 255.0
            if start_ink <= 0:
                continue
            applied = 0
            for _ in range(passes):
                nxt = cell.filter(ImageFilter.MinFilter(3))
                ink = sum(nxt.point(lambda v: 255 if v > 110 else 0)
                          .convert("L").getdata()) / 255.0
                if ink < start_ink * floor:
                    break
                cell = nxt
                applied += 1
            sheet.paste(cell, box)
            thinned[c["char"]] = applied

        sheet = sheet.resize((atlas_w, atlas_h), Image.LANCZOS)
        atlas = Image.composite(Image.new("RGBA", (atlas_w, atlas_h), (255, 255, 255, 255)),
                                Image.new("RGBA", (atlas_w, atlas_h), (255, 255, 255, 0)),
                                sheet)
        held = sorted({ch for ch, n in thinned.items() if n < passes})
        if held:
            print("[Trace] protected from over-thinning ({0} of {1} passes not applied): {2}".format(
                passes, passes, "".join(held)))
    else:
        atlas = Image.new("RGBA", (atlas_w, atlas_h), (255, 255, 255, 0))
        d = ImageDraw.Draw(atlas)
        for c in cells:
            # Default "la" anchor: x is the pen position, y the top of the line.
            # The glyph's own bearings are therefore already inside the cell,
            # which is what makes a bearing table unnecessary.
            d.text((c["x"], c["y"]), c["char"], font=font, fill=(255, 255, 255, 255))

    os.makedirs(out_dir, exist_ok=True)
    png_path = os.path.join(out_dir, name + ".png")
    atlas.save(png_path)

    meta = {
        "source": os.path.basename(font_path),
        "note": "Generated by Scripts/generate_font_atlas.py. Do NOT hand-edit.",
        "pixelSize": px,
        "thin": thin,
        "ascent": ascent,
        "descent": descent,
        "lineHeight": line_h,
        "atlas": {"width": atlas_w, "height": atlas_h, "texture": name},
    }

    # BOTH OF THESE ARE EMITTED ONLY WHEN THEY SAY SOMETHING, and that is on purpose:
    # WP12 requires this script to stay ZERO-DIFF for the three sheets that already
    # exist, so a run with the default charset over a non-OFL face has to reproduce
    # their committed .json byte for byte — key order included, which is why they go
    # in HERE rather than after the cell list. Readers use meta.get("charset", "ascii").
    if charset != DEFAULT_CHARSET:
        meta["charset"] = charset
    licence = OFL_FONTS.get(os.path.basename(font_path))
    if licence is not None:
        meta["licence"] = licence

    meta["characters"] = [
        {"char": c["char"], "code": ord(c["char"]),
         "u": c["x"], "v": c["y"], "uSize": c["w"], "vSize": c["h"]}
        for c in cells
    ]
    json_path = os.path.join(out_dir, name + ".json")
    with open(json_path, "w") as f:
        json.dump(meta, f, indent=1)

    used = sum(c["w"] * c["h"] for c in cells)
    print("[Trace] atlas   {0}  ({1}x{2}, {3} glyphs, {4:.0f}% packed)".format(
        png_path, atlas_w, atlas_h, len(cells), 100.0 * used / (atlas_w * atlas_h)))
    print("[Trace] metrics {0}  (em {1}px, ascent {2}, descent {3})".format(
        json_path, px, ascent, descent))
    print("[Trace] charset {0}  ({1} codepoints in {2} contiguous run(s): {3})".format(
        charset, len(codes), len(runs_of(codes)),
        ", ".join("{0}..{1}".format(a, b) for a, b in runs_of(codes))))
    if licence is not None:
        print("[Trace] licence {0}".format(licence))

    if preview:
        # A sanity sheet: real strings set FROM THE ATLAS, not from the font, so
        # what you look at is what the engine will draw.
        sample = ["PLAY", "SETTINGS", "DIFFICULTY", "SCORING MODE",
                  "SELECT YOUR CHARACTER", "0123456789 - 20s CD"]
        if charset == "latin1":
            # WP12's acceptance specimen, plus the rest of the supplement, set FROM
            # THE SHEET. If the ö or the em dash is missing here it is missing in the
            # atlas, and that is visible before the engine is ever launched.
            sample += ["BJÖRN — ÀÉÎÕÜ ¿¡",
                       "ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖØÙÚÛÜÝÞß",
                       "àáâãäåæçèéêëìíîïðñòóôõöøùúûüýþÿ",
                       "¡¢£¤¥¦§¨©ª«¬®¯°±²³´µ¶·¸¹º»¼½¾¿×÷",
                       "– — ' ' \" \" • … €"]
        # Compose from the METRICS, not from `cells`, so the preview exercises
        # exactly the numbers the engine will be handed.
        by_char = {c["char"]: c for c in meta["characters"]}
        pw = max(sum(by_char[ch]["uSize"] for ch in s if ch in by_char) for s in sample) + 20
        ph = line_h * len(sample) + 20
        prev = Image.new("RGBA", (pw, ph), (10, 10, 12, 255))
        yy = 10
        for s in sample:
            xx = 10
            for ch in s:
                c = by_char.get(ch)
                if not c:
                    continue
                prev.alpha_composite(
                    atlas.crop((c["u"], c["v"], c["u"] + c["uSize"], c["v"] + c["vSize"])),
                    (xx, yy))
                xx += c["uSize"]
            yy += line_h
        prev_path = os.path.join(out_dir, name + "_preview.png")
        prev.save(prev_path)
        print("[Trace] preview {0}  (strings composed FROM the atlas)".format(prev_path))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font", default=DEFAULT_FONT)
    ap.add_argument("--size", type=int, default=96,
                    help="em size in pixels; menu text is 20-32 px on a 1080p screen, "
                         "so 96 leaves room to downscale cleanly (default: 96)")
    ap.add_argument("--out", default=OUT_DIR)
    ap.add_argument("--thin", type=float, default=0.0,
                    help="DO NOT USE. Synthesises a lighter weight by eroding each glyph by this "
                         "many FINAL-SIZE pixels per side. It exists only because Sofachrome once "
                         "shipped here as Regular alone; every face in this project now comes from "
                         "its own real font file and this MUST stay 0. Two attempts to synthesise a "
                         "weight damaged the letterforms — the second deleted ( ) [ ] { } and / "
                         "outright while the HUD draws '[E]'. 0 = as drawn.")
    ap.add_argument("--name", default="T_FontAtlas",
                    help="output basename (default T_FontAtlas). The game uses three licensed "
                         "faces — T_FontAtlas (Sofachrome ExtraLight, the default), T_FontAtlasBold "
                         "(Sofachrome Regular, character names) and T_FontAtlasHud (Erbaum Bold, "
                         "the in-match HUD and the ability descriptions) — plus T_FontAtlasNames, "
                         "the OFL Lato per-glyph FALLBACK sheet (UI plan WP12).")
    ap.add_argument("--charset", default=DEFAULT_CHARSET, choices=sorted(CHARSETS),
                    help="which codepoints to rasterise. 'ascii' (default) is printable ASCII, "
                         "32..126, and is what all three LICENSED sheets are — leaving this alone "
                         "keeps their output byte-identical. 'latin1' adds the Latin-1 Supplement "
                         "and a few typographic marks, and is for the OFL Lato fallback sheet only: "
                         "extending a licensed face's charset rasterises more of a font this repo "
                         "may not redistribute. See the CHARSETS block above.")
    ap.add_argument("--preview", action="store_true",
                    help="also write a sheet of real strings composed from the atlas")
    a = ap.parse_args()
    build(a.font, a.size, a.out, a.preview, a.thin, a.name, a.charset)


main()
