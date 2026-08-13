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
# Usage:
#   python3 Scripts/generate_font_atlas.py [--font PATH] [--size PX] [--preview]
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

# Printable ASCII. The menu needs upper case, digits and punctuation; lower case
# and the rest are cheap and stop a stray string rendering as blanks.
CHARSET = "".join(chr(c) for c in range(32, 127))

PAD = 2               # transparent gutter, so bilinear filtering cannot bleed
MAX_WIDTH = 2048      # atlas width; height grows in powers of two to fit


SS = 2                # supersample factor used when thinning


def build(font_path, px, out_dir, preview, thin, name="T_FontAtlas"):
    if not os.path.isfile(font_path):
        sys.exit(
            "[Trace] font not found: {0}\n"
            "        Put your licensed copy there. It is gitignored on purpose —\n"
            "        see the header of this script.".format(font_path))

    font = ImageFont.truetype(font_path, px)
    ascent, descent = font.getmetrics()
    line_h = ascent + descent

    # --- measure -------------------------------------------------------------
    # Each cell is a FULL ADVANCE WIDTH by a FULL LINE HEIGHT. That wastes some
    # atlas space, but it means the engine can advance the pen by the cell width
    # and sit every glyph on a shared baseline with no per-glyph bearing table —
    # which is exactly what Unreal's offline font format can express.
    cells = []
    for ch in CHARSET:
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
    atlas = Image.new("RGBA", (atlas_w, atlas_h), (255, 255, 255, 0))
    if thin > 0:
        big = ImageFont.truetype(font_path, px * SS)
        # `thin` is in FINAL-SIZE pixels per side; at SS supersampling that is
        # thin*SS erosion passes.
        kernel = max(1, int(round(thin * SS)))
    for c in cells:
        if thin <= 0:
            # Draw with the default "la" anchor: x is the pen position, y is the
            # top of the line. The glyph's own bearings are therefore already
            # inside the cell, which is what makes a bearing table unnecessary.
            ImageDraw.Draw(atlas).text(
                (c["x"], c["y"]), c["char"], font=font, fill=(255, 255, 255, 255))
            continue

        pad = px * SS // 2
        cw, ch = c["w"] * SS + pad * 2, c["h"] * SS + pad * 2
        cell = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(cell).text((pad, pad), c["char"], font=big, fill=255)
        # k passes of a 3x3 minimum erode by k pixels and cost 9k operations per
        # pixel; one MinFilter(2k+1) erodes the same amount but costs (2k+1)^2,
        # which for the amounts needed here is slow enough to time out.
        for _ in range(kernel):
            cell = cell.filter(ImageFilter.MinFilter(3))
        cell = cell.resize((c["w"] + (pad * 2) // SS, c["h"] + (pad * 2) // SS),
                           Image.LANCZOS)
        glyph = Image.new("RGBA", cell.size, (255, 255, 255, 0))
        glyph.putalpha(cell)
        glyph = Image.composite(
            Image.new("RGBA", cell.size, (255, 255, 255, 255)),
            Image.new("RGBA", cell.size, (255, 255, 255, 0)),
            cell)
        atlas.alpha_composite(glyph, (c["x"] - pad // SS, c["y"] - pad // SS))

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
        "characters": [
            {"char": c["char"], "code": ord(c["char"]),
             "u": c["x"], "v": c["y"], "uSize": c["w"], "vSize": c["h"]}
            for c in cells
        ],
    }
    json_path = os.path.join(out_dir, name + ".json")
    with open(json_path, "w") as f:
        json.dump(meta, f, indent=1)

    used = sum(c["w"] * c["h"] for c in cells)
    print("[Trace] atlas   {0}  ({1}x{2}, {3} glyphs, {4:.0f}% packed)".format(
        png_path, atlas_w, atlas_h, len(cells), 100.0 * used / (atlas_w * atlas_h)))
    print("[Trace] metrics {0}  (em {1}px, ascent {2}, descent {3})".format(
        json_path, px, ascent, descent))

    if preview:
        # A sanity sheet: real strings set FROM THE ATLAS, not from the font, so
        # what you look at is what the engine will draw.
        sample = ["PLAY", "SETTINGS", "DIFFICULTY", "SCORING MODE",
                  "SELECT YOUR CHARACTER", "0123456789 - 20s CD"]
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
                    help="synthesise a lighter weight by eroding each glyph by this many "
                         "FINAL-SIZE pixels per side (strokes narrow by 2x this). Sofachrome "
                         "ships Regular only; the artist's lettering is hairline. 0 = as drawn.")
    ap.add_argument("--name", default="T_FontAtlas",
                    help="output basename (default T_FontAtlas). The menu uses two weights: "
                         "T_FontAtlas (light, general UI) and T_FontAtlasBold (character names).")
    ap.add_argument("--preview", action="store_true",
                    help="also write a sheet of real strings composed from the atlas")
    a = ap.parse_args()
    build(a.font, a.size, a.out, a.preview, a.thin, a.name)


main()
