# =============================================================================
# Trace - slice-ui-assets.py            (spec v19 section 5)
#
# Cuts the artist's ONE sheet - "UI Test Export_2.png", 32055 x 18006, 11 MB -
# into the individual menu sprites, and writes them DOWNSCALED as RGBA PNGs to
#
#     Content/Trace/UI/Art/Source/
#
# Those PNGs are the import source. Scripts/generate-menu-widgets.py (which runs
# INSIDE the editor) turns them into /Game/Trace/UI/Art/T_* textures and builds
# the menu's widget tree out of them.
#
# -----------------------------------------------------------------------------
# WHY THIS RUNS OUTSIDE UNREAL
# -----------------------------------------------------------------------------
# Unreal's embedded Python has no PIL, and nothing in the editor will cut a
# 577-megapixel sheet into fourteen alpha-keyed sprites. So the cutting is done
# here, with the system Python, and the editor script only imports the result.
# Run this first; it is fast (~20 s) and idempotent.
#
#     python3 Scripts/slice-ui-assets.py
#
# -----------------------------------------------------------------------------
# THE SIX THINGS THIS SCRIPT DOES THAT ARE NOT OBVIOUS
# -----------------------------------------------------------------------------
# 1. IT KEYS BLACK TO ALPHA. The sheet is RGB with no alpha channel and a black
#    background, and every sprite on it is a GLOW - a bright core with a soft
#    halo. Shipping them opaque would paint black rectangles over anything they
#    overlap. So alpha = min(1, max(R,G,B) / ALPHA_KNEE) and the colour is
#    un-premultiplied by that alpha. Composited over black the result is
#    mathematically identical to the sheet, whatever ALPHA_KNEE is; the knee only
#    decides how the halo behaves over a NON-black background, and the artist
#    asked for black.
#
# 2. IT ERASES THE BAKED WORD FROM THE BUTTON FRAMES. The sheet has no empty
#    button - every plate has a word painted into it, and the words run nearly
#    edge to edge (DEFAULT starts 252 px in on a 4723 px plate, DISABLED 274).
#    The menu needs JOIN, DIFFICULTY, SCORING MODE and QUIT, which the sheet does
#    not contain. So each button state is rebuilt as an EMPTY frame by flooding
#    the plate's FLAT INTERIOR BAND - the rows between the top and bottom corner
#    curves, where the plate is full width and the fill is one measured colour -
#    with that colour, and crossfading back into the original over 40 rows at
#    each end so there is no seam. Everything outside that band, which is every
#    pixel of the rounded corner, the outline and the glow, is untouched.
#
#    An earlier attempt copied the left and right CAPS verbatim and tiled the
#    middle. It shipped a sprite with the stem of the D still in it, because the
#    lettering starts inside the cap. That is the failure this version is written
#    against, and the assertion at the end of build_empty_frame is what catches
#    it: it re-measures the band and refuses to write a frame that still has a
#    bright pixel where the plate should be flat.
#
#    The empty frame is then 9-sliced by the widget, which is what lets one
#    512 px texture be a 720-wide menu row AND a 90-wide KEY chip without the
#    corner radius stretching.
#
# 3. IT LIFTS THE WORDS OFF THEIR PLATES. PLAY / SETTINGS / KEYBIND / KEY are
#    extracted from the BLUE (default) buttons, where the lettering is white, as
#    white-on-transparent sprites: alpha = (luminance - fill) / (255 - fill).
#    That one sprite then serves all three states, because the artist's states
#    differ only in the colour of the word - white, then amber, then grey - which
#    is a tint, not a different glyph.
#
# 4. IT DELETES THE STRAY ANCHOR MARK ON THE CURSOR. The export left a drawing-tool
#    anchor at the cursor's tail: a flat mid-grey DISC inside a thin BLUE RING.
#    Neither is part of the arrow, and they have to be removed by two different
#    tests because they are two different colours. The disc goes by a saturation
#    test; the ring goes by a BLUE-DOMINANCE test (b > r), which no pixel of the
#    arrow satisfies - the whole mark is white with an amber halo, i.e. r >= b
#    everywhere - so the test cannot eat the pointer. Measured on the sheet: 821
#    blue-dominant pixels, all inside one 87 x 94 box at the tail, of which 7 touch
#    the blade's anti-aliased edge and are kept by the luminance guard.
#
#    An earlier version removed only the disc, and shipped a cursor with a blue
#    circle drawn on its tail. That is the failure this version is written against.
#
# 5. IT TAKES THE HANDLE BACK OUT OF THE TRACK. The slider is ONE connected mark
#    on the sheet - a long thin rail with the angular handle sitting on it - and
#    the two are cut apart here. build_slider_handle() has always subtracted the
#    rail from the HANDLE; nothing subtracted the handle from the TRACK, so the
#    track sprite shipped with the blade still baked into it, at columns 37-78 of
#    512 and 6.1x the trough's own luminance. A track is STRETCHED to whatever
#    width the setting needs, so that blade came out as a phantom second handle
#    frozen a fifth of the way along the bar, wherever the real handle happened to
#    be. The rail is uniform along its length (measured: the column-mean varies by
#    a standard deviation of 0.34 counts over 4000 columns), so a clean column of
#    it is an exact substitution. See build_slider_track.
#
# 6. IT LIFTS THE TRACE WORDMARK, WHICH IS DRAWN DARK ON BLACK. Every other mark
#    on this sheet is a bright core with a halo, so black-keying it (note 1) is
#    the whole job. The wordmark is the exception: the artist drew the letters as
#    a flat NAVY stroke, RGB(29,41,81), and put the bright part - an amber rim at
#    up to luminance 129 - AROUND them. Black-keyed verbatim the rim therefore
#    comes out brighter than the letters and the mark reads as two offset words,
#    which is precisely what shipped. build_wordmark() lifts it the way
#    build_word() lifts PLAY off its plate: the stroke's OWN luminance is the
#    ceiling for alpha, the stroke is white and the rim keeps the artist's amber.
#    The letterforms are never re-cut, re-shaped or moved - only re-toned.
#
# -----------------------------------------------------------------------------
# SIZES - and why each one
# -----------------------------------------------------------------------------
# Nothing here ships at sheet resolution. Each sprite is downscaled to about 2x
# the pixels it will ever occupy on a 1080p screen, so it stays sharp at 1440p
# and does not cost 11 MB to do it. The whole set is under 1 MB of PNG. Sizes are
# in the table at TARGETS below, one line of reasoning each.
# =============================================================================

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.stderr.write(
        "slice-ui-assets.py needs Pillow.  python3 -m pip install --user Pillow\n")
    raise

import numpy as np

Image.MAX_IMAGE_PIXELS = None

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SHEET = os.path.join(PROJECT, "UI Test Export_2.png")
OUT_DIR = os.path.join(PROJECT, "Content", "Trace", "UI", "Art", "Source")

# Luminance at which a pixel becomes fully opaque. See note 1 in the header: over
# black the output is identical for any value, so this is only the halo's
# behaviour over a non-black background.
ALPHA_KNEE = 70.0

# The glow margin carried around every button plate, in SHEET pixels. Measured:
# the amber hover ring sits 34 px outside the plate and its halo dies out by
# ~108 px, so 128 keeps the whole hover state and clips nothing.
GLOW = 128

# Plate geometry, measured off the sheet and identical for every wide button.
PLATE_W, PLATE_H = 4723, 1230

# Corner radius, in sheet pixels. The plate reaches its full width 300 px down
# from its top edge - measured row by row, not eyeballed.
CORNER = 300

# 9-slice cap, in sheet pixels: the glow plus the corner. This is the smallest
# cap that contains a whole corner, and it is what the widget's brush margin is
# computed from.
CAP = GLOW + CORNER

# The plate's flat interior, in plate-local rows: below the top corner curve and
# above the bottom one. Every baked word measured on this sheet lives inside it
# (DEFAULT 357..831, DISABLED 388..831, HOVER 578..664 of a 1230-tall plate), and
# it is the only region this script is allowed to repaint.
BAND_TOP = 290
BAND_BOTTOM = PLATE_H - 290

# Rows over which the repaint fades back into the artist's pixels, so the band
# cannot leave a horizontal seam. No lettering reaches into either fade.
BAND_FADE = 40

# The same idea one axis over, for the slider rail: COLUMNS over which the rebuilt
# span fades back into the artist's own rail. See build_slider_track.
TRACK_FADE = 30

# Peak luminance over black at which the TRACE wordmark counts as legible, i.e. as
# a white word rather than a navy one inside an amber rim. It is not a number this
# script gets to choose: it is UTraceTitleMenuWidget's own runtime threshold
# (TraceTitleMenuWidgetLocal::WordmarkLiftThreshold), and a sprite that clears it
# is a sprite whose runtime repair stands down. Change one and change both.
WORDMARK_LEGIBLE = 0.62

Failures = []


def fail(message):
    Failures.append(message)
    sys.stderr.write("[SliceUI] FAILED: {0}\n".format(message))


def log(message):
    print("[SliceUI] {0}".format(message))


# =============================================================================
# Where everything is on the sheet.
#
# Every rectangle below is the PLATE - the blue fill - not the glow. It was
# measured, not eyeballed: a luminance threshold sweep gave the outline ring and
# the halo separately for each button, and the plate is the common 4723 x 1230
# box at the centre of both. The crop actually taken is the plate grown by GLOW.
# =============================================================================

def plate(cx, cy, w=PLATE_W, h=PLATE_H):
    """Plate box from its measured CENTRE. The artist placed these by eye, so the
    centres differ by up to 90 px between states; cropping from the centre is what
    keeps the plate in the same place inside every sprite, which is what stops the
    button jumping when it changes state."""
    return (int(round(cx - w / 2.0)), int(round(cy - h / 2.0)),
            int(round(cx + w / 2.0)), int(round(cy + h / 2.0)))


PLATES = {
    # The three generic states. These are the ones that become empty frames.
    "btn_default":  plate(4485.0, 11262.5),
    "btn_hover":    plate(4399.0,  9722.5),
    "btn_disabled": plate(4442.0, 12761.5),

    # The four buttons with a word baked in. Used only as a source of WORDS.
    "word_settings": plate(4485.0,  3493.5),
    "word_play":     plate(4485.0,  6687.5),
    "word_keybind":  plate(20847.5, 10441.5),
    "word_key":      plate(24767.5, 10441.5, 2060, PLATE_H),
}

# Everything that is not a button. Sheet boxes, (x0, y0, x1, y1), already
# including whatever halo the mark has.
MARKS = {
    "wordmark": (15536, 1304, 30016, 3696),
    "swoosh":   (13056, 4008, 31656, 7864),
    "cursor":   (15872, 12736, 16384, 13432),
    "back":     (30560, 15664, 31992, 17536),
    "value_box": (13725, 8611, 14879, 9267),
}

# The slider is one connected mark on the sheet and has to be cut in two: a thin
# track that will be stretched to any width, and the angular handle that rides on
# it. Split at the column where the mark stops being 211 px tall.
SLIDER_TRACK = (8357, 8823, 13449, 9054)
SLIDER_HANDLE = (8672, 8600, 9180, 9290)

# -----------------------------------------------------------------------------
# Output size for each sprite, and the reason for it. "on screen" figures are
# reference pixels in the menu's 1080-tall design space, so a 1080p window draws
# them 1:1 and a 4K window draws them at 2x.
# -----------------------------------------------------------------------------
TARGETS = {
    # 9-sliced onto a 720x60 menu row, and onto a ~90x34 KEY chip. Only the caps
    # are ever drawn 1:1, and at 512 wide a cap is 44 px for a 21 px corner.
    "T_MenuBtn_Default":  (512, None),
    "T_MenuBtn_Hover":    (512, None),
    "T_MenuBtn_Disabled": (512, None),

    # Words sit at roughly 26 px cap height on a row. 256 wide is ~4x that.
    "T_MenuWord_Play":     (256, None),
    "T_MenuWord_Settings": (256, None),
    "T_MenuWord_Keybind":  (256, None),
    "T_MenuWord_Key":      (128, None),

    # The value chip beside a slider: ~54x30 on screen.
    "T_MenuValueBox": (160, None),

    # Track is stretched horizontally, so its width is arbitrary; its HEIGHT is
    # what has to survive - 21 px for a 9 px rail plus halo.
    "T_MenuSliderTrack":  (512, None),
    "T_MenuSliderHandle": (64, None),

    # The title. Drawn ~760 px wide at 1080p, ~1520 at 4K.
    "T_TraceWordmark": (1024, None),

    # Decorative sweep behind the title, ~980 px wide at 1080p.
    "T_MenuSwoosh": (1024, None),

    # The pointer, drawn ~30 px tall. 64 is ~2x with room for the halo.
    "T_MenuCursor": (64, None),

    # Back chevron, drawn ~40 px tall.
    "T_MenuBack": (96, None),
}


# =============================================================================
# Pixel work
# =============================================================================

def load_sheet():
    if not os.path.isfile(SHEET):
        fail("{0} is not there. It is the artist's sheet and this script has nothing to cut "
             "without it.".format(SHEET))
        return None
    sheet = Image.open(SHEET)
    if sheet.size != (32055, 18006):
        log("NOTE: sheet is {0}, not the 32055x18006 these coordinates were measured "
            "against. Every rectangle below is now suspect.".format(sheet.size))
    return sheet


def to_rgba(rgb, knee=None):
    """Black-keyed RGBA. See note 1 in the header.

    @param knee  overrides ALPHA_KNEE for a sprite whose SOLID part is darker than
                 the knee. The disabled button is the case: the artist painted its
                 plate RGB(13,14,14), so the default knee would ship a solid plate
                 at 20% opacity and let the screen behind show through it."""
    a = rgb.astype(np.float32)
    lum = a.max(axis=2)
    alpha = np.clip(lum / (ALPHA_KNEE if knee is None else float(knee)), 0.0, 1.0)
    safe = np.where(alpha > 0.0, alpha, 1.0)[:, :, None]
    colour = np.clip(a / safe, 0.0, 255.0)
    out = np.zeros((a.shape[0], a.shape[1], 4), dtype=np.uint8)
    out[:, :, :3] = colour.astype(np.uint8)
    out[:, :, 3] = (alpha * 255.0).astype(np.uint8)
    return out


def downscale(rgba, target_w):
    """LANCZOS on PREMULTIPLIED colour.

    Resampling un-premultiplied RGBA mixes the colour of transparent pixels into
    opaque ones and haloes every edge. Premultiply, resample, un-premultiply."""
    a = rgba.astype(np.float32)
    alpha = a[:, :, 3:4] / 255.0
    pm = np.concatenate([a[:, :, :3] * alpha, a[:, :, 3:4]], axis=2)
    src_h, src_w = rgba.shape[:2]
    target_h = max(1, int(round(target_w * src_h / float(src_w))))
    img = Image.fromarray(pm.astype(np.uint8), "RGBA").resize((target_w, target_h), Image.LANCZOS)
    b = np.asarray(img).astype(np.float32)
    alpha2 = b[:, :, 3:4] / 255.0
    safe = np.where(alpha2 > 0.0, alpha2, 1.0)
    rgb = np.clip(b[:, :, :3] / safe, 0.0, 255.0)
    out = np.zeros_like(b, dtype=np.uint8)
    out[:, :, :3] = rgb.astype(np.uint8)
    out[:, :, 3] = b[:, :, 3].astype(np.uint8)
    return out


def write(name, rgba):
    target_w = TARGETS[name][0]
    small = downscale(rgba, target_w)
    path = os.path.join(OUT_DIR, name + ".png")
    Image.fromarray(small, "RGBA").save(path, optimize=True)
    log("  {0:22s} {1:5d}x{2:<5d} <- {3}x{4}   {5:6.1f} KB".format(
        name, small.shape[1], small.shape[0], rgba.shape[1], rgba.shape[0],
        os.path.getsize(path) / 1024.0))
    return small.shape[1], small.shape[0]


# =============================================================================
# 2 - the empty button frame
# =============================================================================

def build_empty_frame(sheet, key, out_name):
    """The button state with its baked word erased. See note 2 in the header."""
    x0, y0, x1, y1 = PLATES[key]
    rgb = np.asarray(sheet.crop((x0 - GLOW, y0 - GLOW, x1 + GLOW, y1 + GLOW))).astype(np.float32)

    top = GLOW + BAND_TOP
    bottom = GLOW + BAND_BOTTOM

    # The fill colour, taken from a strip of plate beside the left corner that no
    # word on this sheet reaches into (the earliest is DEFAULT's D at 252 px).
    sample = rgb[top:bottom, GLOW + 60:GLOW + 200]
    fill = np.median(sample.reshape(-1, 3), axis=0)

    # RED ARM, before anything is repainted: if that strip is not flat it is not a
    # fill, and one colour must not be smeared across the whole plate.
    spread = float(np.percentile(sample.max(axis=2), 98) - np.percentile(sample.max(axis=2), 2))
    if spread > 12.0:
        fail("{0}: the plate's fill sample is not flat (luminance spread {1:.0f}). Flooding the "
             "band with one colour would visibly change the artwork.".format(out_name, spread))
        return None

    rebuilt = rgb.copy()
    # Only the plate's own columns are repainted. Six pixels of inset keeps the
    # plate's anti-aliased edge, which is the artist's, not this script's.
    left = GLOW + 6
    right = GLOW + PLATE_W - 6
    rebuilt[top:bottom, left:right] = fill

    # Fade back into the original over BAND_FADE rows at each end of the band, so
    # any vertical shading the plate has cannot break at the band edge.
    for index in range(BAND_FADE):
        blend = index / float(BAND_FADE)
        for row in (top + index, bottom - 1 - index):
            rebuilt[row, left:right] = (blend * rebuilt[row, left:right]
                                        + (1.0 - blend) * rgb[row, left:right])

    # RED ARM: the whole point of this function is that the word is gone. Measure
    # the band that held it and refuse to write a frame that still has lettering.
    check = rebuilt[top + BAND_FADE:bottom - BAND_FADE, left:right].max(axis=2)
    deviation = float(np.abs(check - fill.max()).max())
    if deviation > 6.0:
        fail("{0}: the plate's interior still deviates from its fill by {1:.0f} after the erase - "
             "there is still a word in it.".format(out_name, deviation))
        return None

    was = np.asarray(sheet.crop((x0, y0 + BAND_TOP, x1, y0 + BAND_BOTTOM))).astype(np.int32).max(axis=2)
    log("    {0}: fill RGB({1},{2},{3}); band peak was {4}, now {5}".format(
        out_name, *(int(round(c)) for c in fill), int(was.max()), int(check.max())))
    # The plate is a SOLID, so it has to come out opaque whatever its brightness.
    return to_rgba(np.clip(rebuilt, 0.0, 255.0).astype(np.uint8),
                   knee=min(ALPHA_KNEE, float(fill.max())))


# =============================================================================
# 3 - the word, lifted off its plate
# =============================================================================

def build_word(sheet, key, out_name):
    """White-on-transparent lettering. See note 3 in the header."""
    x0, y0, x1, y1 = PLATES[key]
    # Inset past the plate's own rounded edge so no corner pixel is mistaken for
    # a letter. 200 px is well inside the 300 px corner and well outside any word.
    inset = 200
    rgb = np.asarray(sheet.crop((x0 + inset, y0 + inset, x1 - inset, y1 - inset))).astype(np.float32)
    lum = rgb.max(axis=2)

    fill = float(np.median(lum))
    alpha = np.clip((lum - fill) / max(1.0, 255.0 - fill), 0.0, 1.0)

    ys, xs = np.where(alpha > 0.15)
    if len(ys) == 0:
        fail("{0}: no lettering found on the plate.".format(out_name))
        return None
    pad = 12
    y_lo = max(0, ys.min() - pad)
    y_hi = min(alpha.shape[0], ys.max() + 1 + pad)
    x_lo = max(0, xs.min() - pad)
    x_hi = min(alpha.shape[1], xs.max() + 1 + pad)

    a = alpha[y_lo:y_hi, x_lo:x_hi]
    out = np.zeros((a.shape[0], a.shape[1], 4), dtype=np.uint8)
    out[:, :, :3] = 255                      # white, so a tint is the only state change
    out[:, :, 3] = (a * 255.0).astype(np.uint8)
    log("    {0}: {1}x{2} of lettering lifted off a fill of luminance {3:.0f}".format(
        out_name, a.shape[1], a.shape[0], fill))
    return out


# =============================================================================
# 4 - the cursor's stray anchor dot
# =============================================================================

def build_cursor(sheet):
    rgb = np.asarray(sheet.crop(MARKS["cursor"])).astype(np.int32)
    lum = rgb.max(axis=2)
    mn = rgb.min(axis=2)

    # THE DISC. Flat mid-grey: bright enough to see, too dim to be the white arrow,
    # and with none of the amber saturation the arrow's halo has. It measures
    # RGB(68,76,56) - saturation 20 - against a halo that runs 50 to 60.
    disc = (lum > 18) & (lum < 130) & ((lum - mn) < 32)

    # THE RING around it, which the saturation test cannot see because the ring is
    # not grey - it is BLUE, and blue is the one hue this mark does not contain.
    # The arrow is white with an amber halo, so red >= blue in every pixel of it;
    # requiring blue to LEAD by 8 counts therefore selects the ring and nothing
    # else. The luminance guard keeps the blade's own anti-aliased edge, where a
    # handful of pixels tip blue by a count or two.
    ring = (rgb[:, :, 2] > rgb[:, :, 0] + 8) & (lum > 12) & (lum < 200)

    stray = disc | ring
    removed = int(stray.sum())
    cleaned = rgb.copy()
    cleaned[stray] = 0

    log("    T_MenuCursor: removed {0} px of the export's leftover anchor mark "
        "({1} of grey disc, {2} of blue ring)".format(removed, int(disc.sum()), int(ring.sum())))

    # RED ARM: the point of this function is that the anchor is GONE. Re-measure
    # rather than trust the masks - a cursor with a blue circle on its tail is
    # exactly what shipped last time, and it looked deliberate enough that nobody
    # queried it.
    left = np.asarray(np.where((cleaned[:, :, 2] > cleaned[:, :, 0] + 8)
                               & (cleaned.max(axis=2) > 12)
                               & (cleaned.max(axis=2) < 200))).shape[1]
    if left > 0:
        fail("T_MenuCursor: {0} blue px still on the pointer after the erase - the anchor ring is "
             "still there.".format(left))
    if removed == 0:
        log("    NOTE: no anchor mark found. Either the export changed or the test no longer "
            "matches it - check the sprite before shipping.")
    return to_rgba(cleaned.astype(np.uint8))


# =============================================================================
# The value chip: same erase as a button, but it is a number that changes, so the
# baked "13" has to go the same way the baked words do.
# =============================================================================

def build_value_box(sheet):
    """The chip beside the slider, with the artist's "13" taken out of it.

    Same argument as the buttons: the number on screen is the player's setting,
    so a baked one is worse than useless. Its flat interior runs rows 150..506 of
    a 656-row crop (the corner curve is open by row 130) and the digits peak at
    luminance 107 against a fill of 81, which is what the check below measures."""
    rgb = np.asarray(sheet.crop(MARKS["value_box"])).astype(np.float32)
    top, bottom = 150, 506
    left, right = 60, 1094

    sample = rgb[top:bottom, left:left + 140]
    fill = np.median(sample.reshape(-1, 3), axis=0)
    before = int(rgb[top:bottom, left:right].max())

    rebuilt = rgb.copy()
    rebuilt[top:bottom, left:right] = fill
    for index in range(BAND_FADE):
        blend = index / float(BAND_FADE)
        for row in (top + index, bottom - 1 - index):
            rebuilt[row, left:right] = (blend * rebuilt[row, left:right]
                                        + (1.0 - blend) * rgb[row, left:right])

    check = rebuilt[top + BAND_FADE:bottom - BAND_FADE, left:right].max(axis=2)
    if float(np.abs(check - fill.max()).max()) > 6.0:
        fail("T_MenuValueBox: digits still present after the erase.")
        return None

    log("    T_MenuValueBox: fill RGB({0},{1},{2}); interior peak was {3}, now {4}".format(
        *(int(round(c)) for c in fill), before, int(check.max())))
    return to_rgba(np.clip(rebuilt, 0.0, 255.0).astype(np.uint8),
                   knee=min(ALPHA_KNEE, float(fill.max())))


def build_slider_track(sheet):
    """The rail, with the HANDLE that was sitting on it taken back out.

    See note 5 in the header. The failure this replaces: the crop simply included
    the handle, and since the track is stretched to whatever width a setting needs,
    the blade came out as a phantom second handle painted into the bar - measured
    on the shipped sprite at columns 37-78 of 512 and 6.1x the trough's own
    luminance.

    The substitution is exact rather than approximate because the rail IS uniform:
    its column-mean varies by a standard deviation of 0.34 counts over the 4000
    columns nothing sits on. So the repair is a median column of clean rail,
    stamped across the contaminated span, with the span found by MEASURING which
    columns are too bright rather than by trusting SLIDER_HANDLE's box."""
    rgb = np.asarray(sheet.crop(SLIDER_TRACK)).astype(np.float32)
    width = rgb.shape[1]

    # A span of rail the handle cannot reach: the handle is at the left-hand end
    # (SLIDER_HANDLE starts 315 px into a 5092-px crop), so the clean rail is the
    # right-hand half. Taken as a MEDIAN column so a stray pixel cannot become the
    # profile the whole span is rebuilt from.
    clean_lo = int(width * 0.55)
    clean_hi = int(width * 0.95)
    profile = np.median(rgb[:, clean_lo:clean_hi, :], axis=1)

    # Where the handle actually is, per column, by peak luminance. The rail's ENDS
    # are darker than its middle, not brighter, so a brightness test cannot mistake
    # them for the blade.
    peak = rgb.max(axis=2).max(axis=0)
    baseline = float(np.median(peak[clean_lo:clean_hi]))
    hot = np.where(peak > baseline * 1.05)[0]
    if len(hot) == 0:
        log("    T_MenuSliderTrack: no handle found on the rail - the crop or the sheet changed. "
            "Check the sprite before shipping.")
        return to_rgba(rgb.astype(np.uint8))

    pad = 60
    lo = max(0, int(hot.min()) - pad)
    hi = min(width, int(hot.max()) + 1 + pad)

    rebuilt = rgb.copy()
    rebuilt[:, lo:hi, :] = profile[:, None, :]

    # Crossfade back into the artist's own columns at each end of the span, for the
    # same reason the button band does: if the rail ever gains a length-wise shade,
    # one stamped column must not break it at a hard edge.
    fade = min(TRACK_FADE, (hi - lo) // 2)
    for index in range(fade):
        blend = index / float(fade)
        for column in (lo + index, hi - 1 - index):
            rebuilt[:, column, :] = (blend * rebuilt[:, column, :]
                                     + (1.0 - blend) * rgb[:, column, :])

    # RED ARM: the point of this function is that the blade is GONE. Re-measure the
    # rail rather than trust the mask - a phantom handle frozen in the bar is
    # exactly what shipped last time, and on a still screenshot it looked like the
    # real one.
    after = rebuilt.max(axis=2).max(axis=0)
    worst = float(after[lo:hi].max())
    if worst > baseline * 1.05:
        fail("T_MenuSliderTrack: the rail still peaks at {0:.0f} inside the repaired span against a "
             "trough baseline of {1:.0f} - the handle blade is still in the track.".format(
                 worst, baseline))
        return None

    log("    T_MenuSliderTrack: handle blade removed from sheet columns {0}-{1} of {2}; the span "
        "peaked at {3:.0f} against a trough baseline of {4:.0f} ({5:.1f}x), now {6:.0f} ({7:.2f}x)"
        .format(lo, hi, width, float(peak[lo:hi].max()), baseline,
                float(peak[lo:hi].max()) / baseline, worst, worst / baseline))
    return to_rgba(rebuilt.astype(np.uint8))


def build_wordmark(sheet):
    """TRACE, lifted off the black the way build_word lifts PLAY off its plate.

    See note 6 in the header. The artist drew this word DARK - a flat navy stroke -
    with the bright part, an amber rim, around it. Black-keying it verbatim makes
    the rim the brightest thing in the sprite and the mark reads as two offset
    words. build_word's lift does not transfer unchanged, because it keys UP from a
    fill toward white and this word is the other way round; what transfers is the
    idea, which is that ALPHA carries the letterform and COLOUR carries the state.

    So: alpha ramps from the black ground to the stroke's own luminance, the stroke
    is white, and the rim keeps the artist's amber at the hue they drew it. No
    letterform is re-cut, re-shaped or moved.

    This is the ROOT of the repair UTraceTitleMenuWidget::LiftWordmarkFromSprite
    used to do at runtime, on the already-downscaled texture. That code measures
    the sprite's peak luminance over black and stands down at 0.62; this sprite
    ships at 1.00, so it stands down, and the lift now happens once at sheet
    resolution instead of every launch at a sixteenth of it."""
    rgb = np.asarray(sheet.crop(MARKS["wordmark"])).astype(np.float32)
    lum = rgb.max(axis=2)

    # The two tones are unambiguous and need no threshold to tune: the stroke is
    # navy (blue leads red) and the rim is amber (red leads blue).
    body = rgb[:, :, 2] > rgb[:, :, 0]

    ground = float(np.median(lum))                       # the black the mark sits on
    lit = body & (lum > ground + 20.0)
    if not lit.any():
        fail("T_TraceWordmark: no navy lettering found in the crop - the sheet or MARKS['wordmark'] "
             "changed, and this would ship the mark inverted.")
        return None
    stroke = float(np.median(lum[lit]))                   # the flat fill of the letters

    # The artist's amber, sampled off the brightest part of their own rim and
    # normalised to full brightness - the same move build_word makes when it writes
    # the letters as pure white. Un-normalised it would ship at the sheet's 84/255
    # and the glow would stay the dim brown it is on a dark sheet.
    halo = (~body) & (lum > ground + 8.0)
    bright = halo & (lum > 0.6 * float(lum[halo].max()))
    amber = np.median(rgb[bright], axis=0)
    amber = amber * 255.0 / max(1.0, float(amber.max()))

    alpha = np.clip((lum - ground) / max(1.0, stroke - ground), 0.0, 1.0)

    out = np.zeros((rgb.shape[0], rgb.shape[1], 4), dtype=np.uint8)
    out[:, :, :3] = np.where(body[:, :, None],
                             np.array([255.0, 255.0, 255.0], dtype=np.float32),
                             amber[None, None, :]).astype(np.uint8)
    out[:, :, 3] = (alpha * 255.0).astype(np.uint8)

    # RED ARM: the defect was measurable - the crop peaked at 0.28 over black and the
    # widget's own threshold for "legible" is 0.62.
    #
    # IT MUST BE THE WIDGET'S OWN FORMULA, not a convenient stand-in. UTraceTitleMenuWidget's
    # ByteLuminance is REC. 709 WEIGHTED - 0.2126 R + 0.7152 G + 0.0722 B - and an earlier
    # version of this check used max(R,G,B) instead. On this sprite the two agree, because
    # the lifted stroke is pure white and every formula returns 1.00 for white; on an
    # AMBER-dominant mark they do not, and that is the case that matters here, because the
    # thing this function is guarding against is precisely a mark whose brightest pixels are
    # the artist's amber rim. A flat RGB(255,127,0) scores 1.00 under max() and 0.57 under
    # Rec. 709 - so the old check would have passed a sprite the widget would then have gone
    # on to repair at runtime anyway, i.e. a green arm that could not fail. Rec. 709 it is,
    # and the agreement is checkable: this measures the shipped crop at 0.28 and
    # TraceTitleMenuWidgetLocal::WordmarkLiftThreshold's own comment records 0.28.
    grey = (0.2126 * out[:, :, 0].astype(np.float32)
            + 0.7152 * out[:, :, 1].astype(np.float32)
            + 0.0722 * out[:, :, 2].astype(np.float32))
    peak_over_black = float((grey * (out[:, :, 3].astype(np.float32) / 255.0)).max() / 255.0)
    if peak_over_black < WORDMARK_LEGIBLE:
        fail("T_TraceWordmark: the lifted mark still peaks at only {0:.2f} over black; "
             "UTraceTitleMenuWidget wants {1:.2f} and would repair it at runtime again."
             .format(peak_over_black, WORDMARK_LEGIBLE))
        return None

    log("    T_TraceWordmark: stroke fill luminance {0:.0f} on a ground of {1:.0f}; {2} px of navy "
        "stroke are now white, {3} px of rim keep the artist's amber RGB({4}). Peak over black "
        "{5:.2f}, Rec. 709 (the verbatim crop was 0.28; legible at {6:.2f})".format(
            stroke, ground, int(lit.sum()), int(halo.sum()),
            ",".join(str(int(round(c))) for c in amber),
            peak_over_black, WORDMARK_LEGIBLE))
    return out


def build_slider_handle(sheet):
    """The angular handle, with the rail it is sitting on taken out from under it.

    The handle is drawn ON the track at runtime and at a different scale, so a
    stub of rail baked into the handle sprite would cross the real one at the
    wrong height. The rail is uniform along its length, so a single column of it
    from a handle-free part of the sheet is an exact subtraction."""
    rgb = np.asarray(sheet.crop(SLIDER_HANDLE)).astype(np.int32)
    rail = np.asarray(sheet.crop((11000, SLIDER_HANDLE[1], 11001, SLIDER_HANDLE[3]))).astype(np.int32)

    lum = rgb.max(axis=2)
    core = lum > 130                      # the white blade itself, which is not the rail
    without = np.clip(rgb - rail, 0, 255)
    out = np.where(core[:, :, None], rgb, without)

    log("    T_MenuSliderHandle: rail subtracted (peak {0} outside the blade, was {1})".format(
        int(np.where(core, 0, out.max(axis=2)).max()), int(np.where(core, 0, lum).max())))
    return to_rgba(out.astype(np.uint8))


# =============================================================================

def main():
    log("=" * 74)
    log("Trace - spec v19 section 5: cutting the artist's sheet into menu sprites")
    log("=" * 74)

    sheet = load_sheet()
    if sheet is None:
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)

    log("Button frames, with the baked word erased:")
    for key, name in (("btn_default", "T_MenuBtn_Default"),
                      ("btn_hover", "T_MenuBtn_Hover"),
                      ("btn_disabled", "T_MenuBtn_Disabled")):
        rgba = build_empty_frame(sheet, key, name)
        if rgba is not None:
            write(name, rgba)

    log("Words, lifted off the blue plates:")
    for key, name in (("word_play", "T_MenuWord_Play"),
                      ("word_settings", "T_MenuWord_Settings"),
                      ("word_keybind", "T_MenuWord_Keybind"),
                      ("word_key", "T_MenuWord_Key")):
        rgba = build_word(sheet, key, name)
        if rgba is not None:
            write(name, rgba)

    log("Slider, value chip, cursor and the rest:")
    rgba = build_value_box(sheet)
    if rgba is not None:
        write("T_MenuValueBox", rgba)

    rgba = build_slider_track(sheet)
    if rgba is not None:
        write("T_MenuSliderTrack", rgba)
    write("T_MenuSliderHandle", build_slider_handle(sheet))
    write("T_MenuCursor", build_cursor(sheet))

    rgba = build_wordmark(sheet)
    if rgba is not None:
        write("T_TraceWordmark", rgba)

    # The two marks that ARE what note 1 assumes: a bright core with a halo, on
    # black. Black-keying them is the whole job.
    for key, name in (("swoosh", "T_MenuSwoosh"),
                      ("back", "T_MenuBack")):
        write(name, to_rgba(np.asarray(sheet.crop(MARKS[key]))))

    log("-" * 74)
    if Failures:
        for message in Failures:
            sys.stderr.write("[SliceUI] {0}\n".format(message))
        sys.stderr.write("[SliceUI] VERDICT: {0} failure(s). Do NOT import this set.\n".format(len(Failures)))
        return 1

    total = sum(os.path.getsize(os.path.join(OUT_DIR, f))
                for f in os.listdir(OUT_DIR) if f.endswith(".png"))
    log("VERDICT: {0} sprites in {1} ({2:.0f} KB total, from an 11 MB sheet).".format(
        len(TARGETS), OUT_DIR, total / 1024.0))
    log("Next: Scripts/generate-menu-widgets.py inside the editor imports these and")
    log("      rebuilds /Game/Trace/UI/Menu from them.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
