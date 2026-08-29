#!/usr/bin/env python3
"""compose_portraits.py -- stage 4b of the character pipeline (no editor).

Spec: PIPELINE_DESIGN.md SS8.4, acceptance gate ART_BIBLE SS7.5.

Takes the ten 1024^2 raw frames the in-game portrait rig wrote
(`Saved/Portraits/raw_<Name>.png`, from `Trace.Portrait.CaptureAll` --
Source/Trace/Debug/TracePortraitRig.cpp), grounds them, dresses them in the
menu's own frame language and downsamples them to the committed 512^2 sources
at `Content/Trace/UI/Art/Source/Portraits/T_Portrait_<Name>.png`, which
`Scripts/import_portraits.py` then imports.

WHY THIS IS A SEPARATE SCRIPT FROM THE CAPTURE
----------------------------------------------
The capture needs the game; the composite needs PIL and nothing else. Splitting
them means the ground/frame/underline treatment can be re-judged and re-run in
under a second against frames that took four minutes to shoot, and it means the
VERDICTS below are computed by a program that has never met the renderer -- so
"the body did not load" and "the mesh is inside out" are caught by measurement
rather than by somebody remembering to look.

THE VERDICTS (SS8.4; this script exits 1 and prints EXIT=1 if any fails)
-----------------------------------------------------------------------
  luma       centre region (256..768)^2 mean luma in [12, 120].
             Too dark  = a black render, a body that did not load, or a mesh
                         whose winding is inverted (you photograph the inside
                         of the far wall of the bust, which is unlit -- this is
                         PIPELINE SS11 risk row 2's detector).
             Too bright = the emissive team slabs are clipping and the frame has
                         no tonal range left to read a character out of.
  coverage   non-black fraction of the same region >= 35%: a bust that is not
             there, or is framed off-screen, fails here even when the backdrop
             keeps the luma respectable.
  accent     the per-character colour check SS8.4 asks for, rebuilt around a
             measurement of what its literal box actually contains at the tuned
             framing (96% backdrop) -- see the ACCENT block in the constants.
             Two halves: PRESENCE (this frame carries its own accent hue at real
             saturation, over a minimum area) and DISCRIMINATION (it carries
             MORE of its own accent than the other nine frames do, which is what
             catches ten portraits rendered with one cached accent).
  head       the ART_BIBLE SS7.5 framing gate: head = 38% of frame height.
             Measured, not eyeballed -- see the FRAMING section below.

FRAMING IS ARITHMETIC ON TWO MEASURED NUMBERS, NOT A GUESS ABOUT PIXELS
----------------------------------------------------------------------
Finding "the head" in a rendered frame by looking at pixels is unreliable in
exactly the cases that matter (a dark helmet against a dark ground, a crest that
merges with a shoulder fin). Both halves of the fraction are available exactly
instead:

  * the numerator, per character, from `character_bodies.py`: the Z extent of
    the geometry rigidly bound to the `head` bone, from the head core's floor to
    the top of whatever the recipe stacked on it. That IS the head as authored.
  * the denominator, from the engine itself: TracePortraitRig prints
    `[Portrait] proj <Name>: ... frameZ=[lo,hi] frameH=N` next to every capture,
    measured by projecting two points on the subject's own axis through the live
    view. Pass that log with --log and the head fraction is exact.

Without --log the geometry is still printed (so the numerator is on the record)
and the head verdict is reported as UNMEASURED rather than guessed.

Usage:
    python3 Scripts/compose_portraits.py [--raw-dir DIR] [--out-dir DIR]
                                         [--log PATH] [--contact-sheet PATH]
                                         [--names Rocco,Lily]
"""
import argparse
import colorsys
import json
import math
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)

import character_bodies as cb  # noqa: E402

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.stderr.write("compose_portraits.py needs Pillow (the same dependency "
                     "generate_font_atlas.py has): python3 -m pip install pillow\n")
    raise

# -----------------------------------------------------------------------------
# Constants -- SS8.4's numbers, at the 1024 working size
# -----------------------------------------------------------------------------
RAW_SIZE = 1024                 # the capture resolution (-ResX/-ResY)
OUT_SIZE = 512                  # T_Portrait_<Name>, SS8.5

# 1. radial ground
GROUND_CENTER = (512, 410)
GROUND_EDGE_R = 720.0
GROUND_EDGE_F = 0.55            # value AT r = GROUND_EDGE_R
GROUND_FLOOR = 0.45             # the corners are at r ~ 798; do not run away

# 2. accent underline (the select card's own name-underline language)
UNDERLINE_Y = (928, 940)        # [top, bottom), 12 px == 6 px at 512
UNDERLINE_X = (205, 819)        # 60% of the width, centred

# 3. frame + corner brackets (the menu bracket language)
FRAME_INSET = 16
FRAME_WIDTH = 2                 # -> 1 px at 512, ART_BIBLE SS7.5
BRACKET_WIDTH = 3
BRACKET_LEN = 64

# verdict regions
CENTER_BOX = (256, 256, 768, 768)

LUMA_MIN, LUMA_MAX = 12.0, 120.0
COVERAGE_MIN = 0.35
COVERAGE_BLACK = 12             # 0..255; below this a pixel is "not the subject"

# --- the accent verdict --------------------------------------------------------
#
# *** SS8.4 SPECIFIES "mean hue over x in [700,900], y in [200,600] within 35 deg of the
#     accent". THAT BOX IS MEASURED, ON THE TUNED FRAMING, TO BE 96% BACKDROP, AND THE
#     CHECK IS DEGENERATE FOR THREE OF THE TEN EVEN WHERE IT IS NOT. ***
#
# Three measurements, all on the tuned rig's own frames:
#   * the box was chosen against SS8.2's STARTING camera (240 uu back, a much wider shot);
#     at the framing the 38% gate actually needs, x 700..900 / y 200..600 maps to world
#     Y +17..+35 uu, Z 155..191 -- beside the head, not on it. It reads the backdrop.
#   * the backdrop plate is PlateFill, and PlateFill's hue is 227 deg. Mortimer's accent
#     is 219 deg. His portrait therefore passes a 35 deg hue test on EVERY pixel of empty
#     background, including on somebody else's body.
#   * the team_glow slabs are NeonNeutral at hue 193 deg, and Oyster's accent is 187.
#     His check passes on the team emissive, which is the one thing in the frame that is
#     deliberately NOT per character.
#
# So the check is rebuilt to measure what SS8.4 says it is for -- "catches wrong accent MI
# binding per character" -- rather than where it says to look:
#
#   PRESENCE      the character's own accent hue must actually be present, over a minimum
#                 area, at real saturation and above the backdrop's brightness.
#   DISCRIMINATION the ten accents are scored against EVERY frame (a 10x10 matrix). A frame
#                 whose own accent is not competitive with the best-scoring accent is
#                 wearing somebody else's -- which is exactly the failure "ten portraits
#                 rendered with one cached accent" produces, and which no single-frame
#                 check can see.
#
# Both are measured on the RAW frame, never the composite: the composite draws the frame,
# brackets and underline IN THE ACCENT, which would hand every frame a guaranteed pass.
# The score is an EXCESS, not a raw coverage, and that is the whole trick.
#
# Raw coverage cannot discriminate: the team_glow slabs are NeonNeutral at hue 191 deg and
# fill ~16% of EVERY frame, so measured raw, every portrait "matches" Oyster's 187 deg accent
# better than its own. Subtracting the MEDIAN coverage of that same accent hue across the
# other nine frames removes exactly that shared floor -- team glow, plate, cool key, all of
# it -- and what is left is what THIS body has that the others do not. That residue is the
# accent material and the accent rim, which is what SS8.4 is actually asking about.
#
# Two accents 11 deg apart (Oyster 187, Lily 198) still overlap inside the hue window, so the
# test is "own excess is competitive with the best excess", not "own excess is the maximum".
ACCENT_HUE_TOL_DEG = 30.0
ACCENT_SAT_MIN = 0.25
ACCENT_VAL_MIN = 0.30           # the backdrop measures v 0.12-0.18; this clears it
ACCENT_EXCESS_MIN = 0.002       # >= 0.2% of the frame is accent this frame alone carries
ACCENT_DISCRIMINATION = 0.60    # own excess >= 60% of the best-scoring accent's excess
ACCENT_SAMPLE_STEP = 2          # every other pixel in both axes; 262 144 samples

# A NEAR-WHITE ACCENT HAS NO HUE TO DISCRIMINATE. THERE IS NO LONGER ONE, AND THE ESCAPE HATCH
# BELOW IS THEREFORE DORMANT RATHER THAN DELETED.
#
# *** THIS BLOCK USED TO SAY "EXACTLY ONE OF THE TEN IS NEAR-WHITE" AND NAME LILY AT .12. THE
# TEN-ACCENT RE-SPACE ENDED THAT. *** Lily moved from ice white #E1F6FF to glacier ice #B8F8FF and
# her saturation went .12 -> .28, which puts her ABOVE Mace. Measured sRGB (HSV) saturations of the
# ten LIVE roster accents, high to low:
#
#   Roxie .62  Slimeball .55  Oyster .51  Mortimer .48  Rocco .46
#   Chut  .36  X         .33  Elle   .32  Lily     .28  Mace  .23
#
# So the floor at .18 now sits below ALL TEN and exempts nobody: every character is held to the
# full test, including the "does this frame carry more of its own accent than of anyone else's"
# half that Lily used to be excused. That is a STRICTER gate than the one this comment described,
# and it is the correct one — the exemption existed because a hue test cannot separate an
# achromatic accent from a cool-white key, and no accent is achromatic any more.
#
# THE THRESHOLD IS NOT MOVED. It is the value the argument above justifies for a hue test in
# general, not a number tuned to Lily; leaving it in place means a future re-tune that produces
# another near-white accent is handled the same way, and the excusal is still PRINTED rather than
# hidden when it fires. Re-derive this table rather than trusting it: the saturations come from
# Source/Trace/Core/TraceCharacterRoster.cpp, which is the one place the accents live.
ACCENT_DISCRIMINABLE_SAT = 0.18

HEAD_TARGET_PCT = 38.0          # ART_BIBLE SS7.5
HEAD_TOL_PCT = 3.0              # +/- ; the gate is judged on frame 1 and frozen

DEFAULT_RAW_DIR = os.path.join(_ROOT, "Saved", "Portraits")
DEFAULT_OUT_DIR = os.path.join(_ROOT, "Content", "Trace", "UI", "Art",
                               "Source", "Portraits")

_failures = []


def log(msg):
    print("[compose-portraits] {0}".format(msg))


def fail(msg):
    _failures.append(msg)
    log("FAIL  {0}".format(msg))


# -----------------------------------------------------------------------------
# The accent table and the head geometry -- both read from character_bodies.py,
# which SS8.4 names as the ONE table (it in turn mirrors the C++ roster rows).
# -----------------------------------------------------------------------------
def accent_srgb(name):
    """The character accent as 0..255 sRGB. `hex` in the table is the sRGB of the
    linear triple the roster row carries; both are asserted equal here so a hand
    edit to one of them cannot pass silently."""
    row = cb.CHARACTERS[name]
    lin = row["accent"]
    derived = tuple(linear_to_srgb8(c) for c in lin)
    stated = tuple(int(row["hex"][1 + 2 * i:3 + 2 * i], 16) for i in range(3))
    if max(abs(a - b) for a, b in zip(derived, stated)) > 1:
        fail("{0}: accent linear {1} converts to {2} but the table says {3} ({4}) -- "
             "character_bodies.py disagrees with itself".format(
                 name, lin, derived, stated, row["hex"]))
    return derived


def linear_to_srgb8(c):
    c = max(0.0, min(1.0, float(c)))
    s = 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1.0 / 2.4)) - 0.055
    return int(round(s * 255.0))


def head_extent_uu(name):
    """dict(core=(z0, z1), full=(z0, top), crown=top-of-body).

    THE DEFINITION THE 38% GATE IS MEASURED AGAINST IS `full`, AND THAT IS AN
    ADJUDICATION, SO HERE IS THE ARGUMENT.

    `core` is the helmet/skull lathe -- the part literally named `head` in the
    recipe, Z 152..176 on six of the ten. `full` extends it to the top of
    EVERYTHING rigidly bound to the `head` bone, which on Rocco means his crest:
    a crest bound to the head bone moves with the head, reads as head in a
    silhouette, and a portrait that crops it has cropped part of the head.

    Why the gate cannot use `core`: at head-core 38%, frame height is 24/0.38 =
    63 uu. X's masts reach Z 206 and Lily's fins Z 208 while their chins are at
    152 -- 54 to 56 uu of crown-to-chin in a 63 uu frame, leaving no chest at
    all and still clipping. The bible asks for a CHEST-UP BUST in the same
    sentence as the 38%, and the two are only simultaneously satisfiable at a
    frame height near 90 uu, which is what `full` on frame 1 produces. Both
    numbers are reported per character so the spread is on the record either way.

    The floor is the head CORE's floor (the jaw line) in both cases: it ignores
    small parts hanging below it, such as Rocco's crest-tip accent bead at Z
    148.4, which is not what anybody means by "the chin"."""
    body = cb.build_body(name)
    core = None
    top = None
    for part in body["parts"]:
        if part["bone"] != "head":
            continue
        lo, hi = part["mesh"].bounds()
        top = hi[2] if top is None else max(top, hi[2])
        if part["name"] == "head":
            core = (lo[2], hi[2])
    if core is None or top is None:
        return None
    crown = max(p["mesh"].bounds()[1][2] for p in body["parts"])
    return {"core": core, "full": (core[0], top), "crown": crown}


# -----------------------------------------------------------------------------
# The projection line the rig prints next to every capture
# -----------------------------------------------------------------------------
_PROJ_RE = re.compile(
    r"\[Portrait\] proj (\w+): viewport=([\d.]+)x([\d.]+) pxPerUu=([\d.]+) "
    r"yZ[\d.]+=([-\d.]+) yZ[\d.]+=([-\d.]+) frameZ=\[([-\d.]+),([-\d.]+)\] "
    r"frameH=([\d.]+)uu")


def read_projection(path):
    """name -> dict(viewport, px_per_uu, frame_lo, frame_hi, frame_h). Last line
    per character wins, so a log with two runs in it describes the later one."""
    found = {}
    if not path:
        return found
    if not os.path.isfile(path):
        fail("--log {0} does not exist; the head-height gate cannot be measured "
             "from it.".format(path))
        return found
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            m = _PROJ_RE.search(line)
            if m:
                found[m.group(1)] = {
                    "viewport": (float(m.group(2)), float(m.group(3))),
                    "px_per_uu": float(m.group(4)),
                    "frame_lo": float(m.group(7)),
                    "frame_hi": float(m.group(8)),
                    "frame_h": float(m.group(9)),
                }
    return found


# -----------------------------------------------------------------------------
# The composite
# -----------------------------------------------------------------------------
_ground_cache = {}


def ground_mask(size):
    """The radial multiply of SS8.4 step 1, as a float list per pixel row."""
    if size in _ground_cache:
        return _ground_cache[size]
    cx, cy = GROUND_CENTER
    slope = (1.0 - GROUND_EDGE_F) / GROUND_EDGE_R
    rows = []
    for y in range(size):
        dy = float(y - cy)
        row = []
        for x in range(size):
            dx = float(x - cx)
            f = 1.0 - slope * math.sqrt(dx * dx + dy * dy)
            row.append(max(GROUND_FLOOR, min(1.0, f)))
        rows.append(row)
    _ground_cache[size] = rows
    return rows


def compose_one(raw, accent):
    """raw: RGB Image at RAW_SIZE. Returns the dressed 1024 image."""
    img = raw.convert("RGB")
    if img.size != (RAW_SIZE, RAW_SIZE):
        img = img.resize((RAW_SIZE, RAW_SIZE), Image.LANCZOS)

    # 1. radial ground -----------------------------------------------------
    mask = ground_mask(RAW_SIZE)
    px = img.load()
    for y in range(RAW_SIZE):
        row = mask[y]
        for x in range(RAW_SIZE):
            f = row[x]
            r, g, b = px[x, y]
            px[x, y] = (int(r * f), int(g * f), int(b * f))

    draw = ImageDraw.Draw(img)

    # 2. accent underline --------------------------------------------------
    draw.rectangle([UNDERLINE_X[0], UNDERLINE_Y[0],
                    UNDERLINE_X[1] - 1, UNDERLINE_Y[1] - 1], fill=accent)

    # 3. frame + corner brackets -------------------------------------------
    lo = FRAME_INSET
    hi = RAW_SIZE - 1 - FRAME_INSET
    draw.rectangle([lo, lo, hi, hi], outline=accent, width=FRAME_WIDTH)

    w = BRACKET_WIDTH
    n = BRACKET_LEN
    for (x0, y0, sx, sy) in ((lo, lo, 1, 1), (hi, lo, -1, 1),
                             (lo, hi, 1, -1), (hi, hi, -1, -1)):
        # horizontal arm
        ax0, ax1 = sorted((x0, x0 + sx * (n - 1)))
        ay0, ay1 = sorted((y0, y0 + sy * (w - 1)))
        draw.rectangle([ax0, ay0, ax1, ay1], fill=accent)
        # vertical arm
        bx0, bx1 = sorted((x0, x0 + sx * (w - 1)))
        by0, by1 = sorted((y0, y0 + sy * (n - 1)))
        draw.rectangle([bx0, by0, bx1, by1], fill=accent)

    return img


# -----------------------------------------------------------------------------
# Measurement
# -----------------------------------------------------------------------------
def luma_and_coverage(img, box):
    x0, y0, x1, y1 = box
    px = img.load()
    total = 0.0
    lit = 0
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y]
            # Rec.709 luma, the same weighting the rest of this project measures
            # brightness with.
            l = 0.2126 * r + 0.7152 * g + 0.0722 * b
            total += l
            if l >= COVERAGE_BLACK:
                lit += 1
            n += 1
    return (total / n if n else 0.0), (float(lit) / n if n else 0.0)


def sample_hues(img):
    """[(hue_deg, sat)] for every sampled pixel that is saturated and bright
    enough to be carrying a colour rather than noise. Computed ONCE per frame and
    then scored against all ten accents -- the alternative is ten passes over a
    megapixel in pure python."""
    px = img.load()
    w, h = img.size
    out = []
    for y in range(0, h, ACCENT_SAMPLE_STEP):
        for x in range(0, w, ACCENT_SAMPLE_STEP):
            r, g, b = px[x, y]
            hh, ss, vv = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            if ss >= ACCENT_SAT_MIN and vv >= ACCENT_VAL_MIN:
                out.append((hh * 360.0, ss))
    return out, ((w + ACCENT_SAMPLE_STEP - 1) // ACCENT_SAMPLE_STEP
                 * ((h + ACCENT_SAMPLE_STEP - 1) // ACCENT_SAMPLE_STEP))


def accent_score(samples, total, accent_hue):
    """(coverage fraction of the whole frame, saturation-weighted circular mean
    hue of the matching pixels, matching pixel count)."""
    sx = sy = sat = 0.0
    n = 0
    for hue, s in samples:
        if hue_delta(hue, accent_hue) > ACCENT_HUE_TOL_DEG:
            continue
        a = math.radians(hue)
        # Weight by saturation: a barely-tinted pixel should not vote as loudly
        # as a saturated one on what colour the accent is.
        sx += math.cos(a) * s
        sy += math.sin(a) * s
        sat += s
        n += 1
    if n == 0:
        return 0.0, None, 0
    return (float(n) / total,
            math.degrees(math.atan2(sy, sx)) % 360.0,
            n)


def accent_saturation(name):
    """HSV saturation of the character's accent, 0..1."""
    r, g, b = accent_srgb(name)
    return colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)[1]


def hue_of(rgb):
    h, _, _ = colorsys.rgb_to_hsv(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0)
    return h * 360.0


def hue_delta(a, b):
    d = abs(a - b) % 360.0
    return min(d, 360.0 - d)


# -----------------------------------------------------------------------------
# Contact sheet -- SS8.6's "the implementing agent MUST eyeball all ten"
# -----------------------------------------------------------------------------
def write_contact_sheet(path, images, names, cols=5, cell=None):
    if not images:
        return
    cell = cell or (OUT_SIZE // 2)
    rows = (len(images) + cols - 1) // cols
    label = max(14, cell // 16)
    sheet = Image.new("RGB", (cols * cell, rows * (cell + label)), (8, 10, 16))
    draw = ImageDraw.Draw(sheet)
    for i, (img, name) in enumerate(zip(images, names)):
        cx = (i % cols) * cell
        cy = (i // cols) * (cell + label)
        sheet.paste(img.resize((cell, cell), Image.LANCZOS), (cx, cy))
        draw.text((cx + 5, cy + cell + max(2, label // 5)), name.upper(),
                  fill=(190, 205, 230))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    sheet.save(path)
    log("contact sheet -> {0}".format(path))


# -----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Compose the ten portrait busts.")
    ap.add_argument("--raw-dir", default=DEFAULT_RAW_DIR)
    ap.add_argument("--out-dir", default=DEFAULT_OUT_DIR)
    ap.add_argument("--log", default=None,
                    help="a portraits capture log, for the framing gate")
    ap.add_argument("--contact-sheet", default=None)
    ap.add_argument("--contact-cell", type=int, default=None,
                    help="pixels per cell in the contact sheet (default 256; "
                         "512 gives the owner a full-size sheet)")
    ap.add_argument("--names", default=None,
                    help="comma-separated subset (the full ten by default)")
    ap.add_argument("--json", default=None, help="write the verdict table here")
    args = ap.parse_args()

    names = ([n.strip() for n in args.names.split(",") if n.strip()]
             if args.names else list(cb.CHARACTER_ORDER))
    log("=== compose_portraits ===")
    log("raw {0}".format(args.raw_dir))
    log("out {0}".format(args.out_dir))

    proj = read_projection(args.log)
    if args.log:
        log("projection lines read from the capture log: {0}/{1}".format(
            len(proj), len(names)))

    os.makedirs(args.out_dir, exist_ok=True)
    sheet_images = []
    sheet_names = []
    table = []

    # --- pass 1: the accent matrix -----------------------------------------
    # Every present frame scored against every one of the ten accent hues, so the
    # shared floor (team glow, plate, key) can be subtracted per accent in pass 2.
    hues = {n: hue_of(accent_srgb(n)) for n in cb.CHARACTER_ORDER}
    present = []
    cover = {}
    mean_hue = {}
    for name in names:
        raw_path = os.path.join(args.raw_dir, "raw_{0}.png".format(name))
        if not os.path.isfile(raw_path):
            fail("{0}: {1} is missing -- the capture run did not write it. "
                 "Grep the run log for '[Portrait] DONE'.".format(name, raw_path))
            continue
        with Image.open(raw_path) as handle:
            samples, total = sample_hues(handle.convert("RGB"))
        cover[name] = {}
        mean_hue[name] = {}
        for other in cb.CHARACTER_ORDER:
            c, mh, _n = accent_score(samples, total, hues[other])
            cover[name][other] = c
            mean_hue[name][other] = mh
        present.append(name)

    def excess(frame, accent_name):
        """How much more of `accent_name`'s hue this frame carries than a typical
        OTHER frame does. The median (not the mean) so one genuinely accent-heavy
        sibling cannot drag the floor up."""
        others = sorted(cover[k][accent_name] for k in present if k != frame)
        if not others:
            return cover[frame][accent_name]
        mid = len(others) // 2
        med = (others[mid] if len(others) % 2
               else 0.5 * (others[mid - 1] + others[mid]))
        return cover[frame][accent_name] - med

    # --- pass 2: compose and judge -----------------------------------------
    for name in present:
        raw_path = os.path.join(args.raw_dir, "raw_{0}.png".format(name))
        accent = accent_srgb(name)
        acc_hue = hues[name]
        with Image.open(raw_path) as handle:
            composed = compose_one(handle.convert("RGB"), accent)

        # luma/coverage on the COMPOSED frame -- that is the art that ships, and
        # neither the frame line nor the underline touches the centre box.
        luma, coverage = luma_and_coverage(composed, CENTER_BOX)

        ex = {other: excess(name, other) for other in cb.CHARACTER_ORDER}
        own_ex = ex[name]
        best_name = max(ex, key=lambda k: ex[k])
        best_ex = ex[best_name]
        own_hue = mean_hue[name][name]

        row = {"name": name, "luma": round(luma, 2),
               "coverage": round(coverage, 4), "accent_hue": round(acc_hue, 1),
               "accent_cover": round(cover[name][name], 5),
               "accent_excess": round(own_ex, 5),
               "accent_mean_hue": None if own_hue is None else round(own_hue, 1),
               "accent_best_match": best_name,
               "accent_excess_matrix": {k: round(v, 5) for k, v in ex.items()}}

        ok = True
        if not (LUMA_MIN <= luma <= LUMA_MAX):
            fail("{0}: centre luma {1:.1f} outside [{2:.0f}, {3:.0f}]".format(
                name, luma, LUMA_MIN, LUMA_MAX))
            ok = False
        if coverage < COVERAGE_MIN:
            fail("{0}: non-black coverage {1:.1%} below {2:.0%} -- no bust in "
                 "the middle of the frame".format(name, coverage, COVERAGE_MIN))
            ok = False
        if own_ex < ACCENT_EXCESS_MIN:
            fail("{0}: this frame carries only {1:+.3%} more of its own accent "
                 "hue ({2:.1f} deg) than the other nine do -- the accent MI is "
                 "not on this body, or the rim is not lighting it".format(
                     name, own_ex, acc_hue))
            ok = False
        elif own_ex < ACCENT_DISCRIMINATION * best_ex:
            if accent_saturation(name) < ACCENT_DISCRIMINABLE_SAT:
                log("NOTE  {0}: accent saturation {1:.2f} is a near-white, so the "
                    "hue-discrimination half of the accent check does not apply "
                    "(presence passed at {2:+.3%}); the per-character binding for "
                    "this body rests on the import slot readback.".format(
                        name, accent_saturation(name), own_ex))
                row["accent_discrimination"] = "exempt (near-white accent)"
            else:
                fail("{0}: excess {1:+.3%} on its own accent but {2:+.3%} on {3}'s "
                     "-- this body is wearing the wrong accent".format(
                         name, own_ex, best_ex, best_name))
                ok = False
        if own_hue is not None:
            row["accent_delta"] = round(hue_delta(own_hue, acc_hue), 1)

        # --- the framing gate ------------------------------------------------
        head = head_extent_uu(name)
        if head is not None:
            row["head_z"] = [round(head["full"][0], 2), round(head["full"][1], 2)]
            row["head_uu"] = round(head["full"][1] - head["full"][0], 2)
            row["head_core_uu"] = round(head["core"][1] - head["core"][0], 2)
            row["crown_z"] = round(head["crown"], 2)
        p = proj.get(name)
        if p is not None and head is not None:
            frac = 100.0 * row["head_uu"] / p["frame_h"]
            core_frac = 100.0 * row["head_core_uu"] / p["frame_h"]
            row["head_pct"] = round(frac, 2)
            row["head_core_pct"] = round(core_frac, 2)
            row["frame_h_uu"] = round(p["frame_h"], 2)
            row["frame_z"] = [round(p["frame_lo"], 2), round(p["frame_hi"], 2)]
            row["crown_clip_uu"] = round(max(0.0, head["crown"] - p["frame_hi"]), 2)
            head_note = "head {0:.1f}% (core {1:.1f}%)".format(frac, core_frac)
        else:
            head_note = "head UNMEASURED (no --log projection line)"

        log("{0:<10s} luma {1:6.1f}  cover {2:5.1%}  accent {3:5.1f}deg "
            "excess {4:+.2%} (best {5} {6:+.2%}, measured {7})  {8}  {9}".format(
                name, luma, coverage, acc_hue, own_ex, best_name, best_ex,
                "n/a" if own_hue is None else "{0:.1f}deg".format(own_hue),
                head_note, "ok" if ok else "FAILED"))

        out = composed.resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS)
        out_path = os.path.join(args.out_dir, "T_Portrait_{0}.png".format(name))
        out.save(out_path)
        row["png"] = out_path
        table.append(row)
        sheet_images.append(out)
        sheet_names.append(name)

    # --- the set-level checks ---------------------------------------------
    if len(table) != len(names):
        fail("{0} of {1} portraits composed".format(len(table), len(names)))

    measured = [r for r in table if "head_pct" in r]
    if measured:
        first = measured[0]
        log("FRAMING GATE (ART_BIBLE SS7.5): {0} head {1:.2f} uu in a {2:.2f} uu "
            "frame = {3:.2f}% (target {4:.0f}% +/- {5:.0f})".format(
                first["name"], first["head_uu"], first["frame_h_uu"],
                first["head_pct"], HEAD_TARGET_PCT, HEAD_TOL_PCT))
        if abs(first["head_pct"] - HEAD_TARGET_PCT) > HEAD_TOL_PCT:
            fail("frame 1 ({0}) measures {1:.2f}% head height, not {2:.0f}% "
                 "+/- {3:.0f} -- re-tune Trace.Portrait.CamDist/CamZ and "
                 "re-capture".format(first["name"], first["head_pct"],
                                     HEAD_TARGET_PCT, HEAD_TOL_PCT))
        # One camera for all ten is the other half of "provably one set".
        hs = set((r["frame_h_uu"], tuple(r["frame_z"])) for r in measured)
        if len(hs) != 1:
            fail("the ten frames did not come from one camera: {0} distinct "
                 "framings in the log".format(len(hs)))
        else:
            log("one camera for all {0} frames: frameZ={1} frameH={2} uu".format(
                len(measured), measured[0]["frame_z"], measured[0]["frame_h_uu"]))
        clipped = [r for r in measured if r.get("crown_clip_uu", 0.0) > 0.5]
        for r in clipped:
            log("NOTE  {0}: {1:.2f} uu of crown geometry sits above the frame "
                "top ({2:.2f})".format(r["name"], r["crown_clip_uu"],
                                       r["frame_z"][1]))

    if args.contact_sheet:
        write_contact_sheet(args.contact_sheet, sheet_images, sheet_names,
                            cell=args.contact_cell)

    if args.json:
        with open(args.json, "w") as handle:
            json.dump(table, handle, indent=1)
        log("verdict table -> {0}".format(args.json))

    if _failures:
        log("{0} problem(s):".format(len(_failures)))
        for f in _failures:
            log("    {0}".format(f))
        log("EXIT=1")
        return 1
    log("{0} portrait(s) composed, every verdict green.".format(len(table)))
    log("EXIT=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
