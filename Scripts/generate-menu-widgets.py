# =============================================================================
# Trace - generate-menu-widgets.py
#
# Runs INSIDE Unreal Editor's Python and does two jobs:
#
#   1. IMPORTS the sprites Scripts/slice-ui-assets.py cut out of the artist's
#      sheet, as textures under      /Game/Trace/UI/Art
#   2. AUTHORS the title screen out of them:
#          WBP_MenuRow     one menu row     (parent class UTraceMenuRow)
#          WBP_TitleMenu   the whole screen (parent class UTraceTitleMenuWidget)
#      under                          /Game/Trace/UI/Menu
#
# -----------------------------------------------------------------------------
# WHY THIS EXISTS  (spec v17 section 4, rebuilt for spec v19 section 5)
# -----------------------------------------------------------------------------
# ATraceMenuHUD::DrawHUD draws the title screen with AHUD::DrawRect / DrawText /
# DrawLine - about six hundred lines of pixel arithmetic that works, that nobody
# can open, and that a designer cannot touch without a C++ build. This script
# authors the same screen as a widget tree so that the LAYOUT and the STYLING
# live in an asset a person can open in the editor.
#
# Spec v19 section 5 changed WHAT it draws, not how it is organised: the screen is
# now made of the artist's sprites on black, instead of stroked rectangles on a
# neon grid.
#
# THE ARCHITECTURE IS NOT NEGOTIABLE, and this script is written to enforce it:
#   * All behaviour stays in C++ (UTraceTitleMenuWidget, UTraceMenuRow). This
#     script authors NO graph, NO event, NO binding - only widgets and styling.
#   * Every widget whose name matches a UPROPERTY(meta=(BindWidget)) in those
#     headers is REQUIRED. Rename one in the editor and the Blueprint fails to
#     compile with "A required widget binding <name> ... was not found". That
#     loud failure is the safety net; it is why the names below are fussy.
#
# -----------------------------------------------------------------------------
# THE ~40 SECOND LAUNCH STALL, AND WHAT FIXED IT
# -----------------------------------------------------------------------------
# `Trace.UI.UseUMG` shipped OFF in v17 because turning it on cost 12-47 seconds of
# frozen black window on launch. The cause was in THIS script: it cleared the tree
# by RENAMING every widget to TraceRetired_N and leaving it in the package, so
# WBP_TitleMenu was saved carrying 27 orphaned widgets whose variable GUIDs the
# Blueprint had to re-resolve, and complain about, on every single load.
#
# retire_descendants() now renames them into the TRANSIENT package instead. A
# transient outer is never serialised, so the orphans do not reach the .uasset at
# all. Measure it rather than believe it: the count is printed at the end of every
# run, and it must be zero.
#
# -----------------------------------------------------------------------------
# WHAT IS AND IS NOT AUTHORITATIVE  (read before editing an asset by hand)
# -----------------------------------------------------------------------------
# AUTHORITATIVE, and genuinely used by the running game:
#   * the widget TREE and its STYLING - fonts, colours, positions, padding, and
#     every EditAnywhere property on UTraceMenuRow, including the three plate
#     brushes. Change them in the editor and the game looks different on the next
#     launch. That is the point of this step.
# NOT AUTHORITATIVE:
#   * the TEXT in every text block. It is placeholder, overwritten on the first
#     frame by UTraceTitleMenuWidget::ApplyView from state ATraceMenuHUD owns.
#   * the WIDTH of ConsolePanel. UTraceTitleMenuWidget::SyncConsoleWidth
#     reproduces the shipped min(viewport * 0.52, 720) clamp every frame.
#   * the row label's SLOT on a row with a right-hand readout (JOIN, DIFFICULTY).
#     The asset authors the kit's centred label; UTraceMenuRow::PlaceLabel moves
#     it to the leading edge on those rows, and lifts every label so its caps,
#     not its line box, sit on the plate's centre line.
#
# RE-RUNNING IS SAFE and rewrites both assets in place. It does NOT delete and
# recreate them - delete_asset() + create_asset() in one editor session returns
# None for everything that follows (this bit the input-asset generator in step
# 6), and recreating WBP_MenuRow would break WBP_TitleMenu's hard reference to
# its generated class. Instead the existing ROOT widget is kept and everything
# under it is rebuilt.
#
# -----------------------------------------------------------------------------
# FOUR EDITOR-PYTHON FACTS THIS SCRIPT IS BUILT AROUND
# -----------------------------------------------------------------------------
# Learned by measurement, in UE 5.8, and all three are silent if you get them
# wrong - so they are written down rather than rediscovered:
#
#  1. UWidgetBlueprint::WidgetTree and UWidgetTree::RootWidget are NOT settable
#     from Python. Python's property access refuses any UPROPERTY that is not
#     EditAnywhere/BlueprintReadWrite ("... is protected and cannot be set"), and
#     both of those are bare UPROPERTY()s. The tree itself is still reachable as
#     an inner object - unreal.find_object(blueprint, "WidgetTree") - and can be
#     filled; only the RootWidget pointer is out of reach.
#  2. So the ROOT has to come from the factory, and the factory takes it from
#     UUMGEditorProjectSettings::DefaultRootWidget, which ships as None (hence
#     "a new widget blueprint has no root until you open it"). That CDO is
#     reachable by path, so this script sets it, creates the asset, and puts it
#     back. Nothing is written to any .ini.
#  3. Snake_case property names only exist for script-exposed properties. For
#     the rest, the EXACT C++ name works: `bOverride_HeightOverride`,
#     `DefaultRootWidget`. UCanvasPanelSlot's Anchors/Offsets/Alignment are not
#     properties at all any more - they live inside FAnchorData - so the
#     BlueprintCallable setters are used instead. set_prop() below tries both
#     spellings rather than betting on one.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     python3 Scripts/slice-ui-assets.py            # first: cut the sheet
#
#     UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
#     "$UE" "$PWD/Trace.uproject" \
#         -run=pythonscript -script="$PWD/Scripts/generate-menu-widgets.py" \
#         -unattended -nosplash -NullRHI -nosound \
#         -log -abslog="$PWD/Saved/Logs/trace-generate-menu-widgets.log"
#
# -NullRHI is safe: widget blueprints compile no shaders, and texture import does
# not need a swap chain.
#
# THE ONE THING THAT COMMAND LINE CANNOT DO IS IMPORT THE FONT. A .ttf import
# ends in UFontFace::PostEditChangeProperty, which calls FSlateApplication::Get()
# unconditionally, and a commandlet has no Slate application - so it does not
# fail, it asserts and SIGSEGVs the run. When the font asset does not exist yet
# (or you changed TraceMenuArtStyle::MenuFontSourceFile), run this from an editor
# instead, which has one:
#
#     "$UE" "$PWD/Trace.uproject" \
#         -ExecCmds="py $PWD/Scripts/generate-menu-widgets.py,QUIT_EDITOR" \
#         -unattended -nosplash -nosound -RenderOffScreen \
#         -log -abslog="$PWD/Saved/Logs/trace-generate-menu-widgets.log"
#
# Once /Game/Trace/UI/Fonts/F_TraceMenu exists, the commandlet form above works
# again: it re-uses the imported asset and never touches the importer.
#
# You must have built the game module first - the parent classes come out of it,
# and the script fails immediately rather than authoring an asset with the wrong
# parent.
#
# OPTIONS (environment variables; the pythonscript commandlet has no clean way
# to pass argv through -script=)
#   TRACE_MENU_UI_DIR   package dir for the widgets. Default /Game/Trace/UI/Menu
#   TRACE_MENU_ART_DIR  package dir for the sprites. Default /Game/Trace/UI/Art
#   TRACE_MENU_FONT_DIR package dir for the font.    Default /Game/Trace/UI/Fonts
#   TRACE_SKIP_IMPORT   set to 1 to re-author the widgets without re-importing
#   TRACE_IMPORT_ONLY   comma-separated sprite names (e.g. T_TraceWordmark,T_MenuSwoosh):
#                       re-import JUST those textures with the settings below and stop.
#                       No font import, no widget is touched. For a re-slice of one or
#                       two sprites, where re-authoring both widget blueprints would be a
#                       much bigger diff than the change. The .uasset files are Git LFS
#                       and checked out read-only: chmod u+w them first, u-w after.
#   TRACE_FONT_IMPORT   0 to never attempt the font import, 1 to force it (see
#                       slate_is_available(); forcing it in a commandlet crashes)
# =============================================================================

import os
import re
import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write(
        "generate-menu-widgets.py must run inside Unreal Editor's Python\n"
        "environment. Use the -run=pythonscript command line in this file's\n"
        "header comment.\n")
    raise

MENU_DIR = os.environ.get("TRACE_MENU_UI_DIR", "/Game/Trace/UI/Menu")
ART_DIR = os.environ.get("TRACE_MENU_ART_DIR", "/Game/Trace/UI/Art")
SKIP_IMPORT = os.environ.get("TRACE_SKIP_IMPORT", "0") == "1"
IMPORT_ONLY = [n.strip() for n in os.environ.get("TRACE_IMPORT_ONLY", "").split(",") if n.strip()]

PROJECT_DIR = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SPRITE_SOURCE_DIR = os.path.join(PROJECT_DIR, "Content", "Trace", "UI", "Art", "Source")

Failures = []


def log(message):
    unreal.log("[MenuWidgets] {0}".format(message))


def fail(message):
    Failures.append(message)
    unreal.log_error("[MenuWidgets] {0}".format(message))


def set_prop(obj, snake, cpp, value):
    """Set a property by its script name, falling back to the exact C++ name.

    See editor-python fact 3 in the header. Both spellings are tried because
    which one works depends on whether the property is script-exposed, and
    getting it wrong raises rather than silently doing nothing - so a bare
    set_editor_property is a coin toss written as an assertion."""
    try:
        obj.set_editor_property(snake, value)
        return True
    except Exception:
        pass
    try:
        obj.set_editor_property(cpp, value)
        return True
    except Exception as error:
        fail("could not set {0}/{1} on {2}: {3}".format(snake, cpp, obj.get_name(), error))
        return False


# =============================================================================
# THE SHEET'S OWN NUMBERS.
#
# Mirrored from Source/Trace/UI/Widgets/Menu/TraceMenuArtStyle.h (which mirrors
# them from Scripts/slice-ui-assets.py, which measured them). Three copies, and
# the reason there are three is that one lives in C++, one in a cutting script
# that never sees the engine, and one here - so they are stated in full in each
# and a change means a change in all three.
# =============================================================================

# The wide button, in SHEET pixels: plate, the glow kept around it, and the cap
# that contains a whole corner.
BTN_PLATE_W, BTN_PLATE_H = 4723.0, 1230.0
BTN_GLOW = 128.0
BTN_CAP = 428.0
BTN_SPRITE_W = BTN_PLATE_W + BTN_GLOW * 2.0
BTN_SPRITE_H = BTN_PLATE_H + BTN_GLOW * 2.0

# The value chip beside the artist's slider, used on the DIFFICULTY row because
# it is the same job: a framed box holding a value that changes. It was shared
# with SCORING MODE until that row was removed.
VAL_PLATE_W, VAL_PLATE_H = 1034.0, 538.0
VAL_GLOW = 60.0
VAL_CAP = 150.0
VAL_SPRITE_W = VAL_PLATE_W + VAL_GLOW * 2.0
VAL_SPRITE_H = VAL_PLATE_H + VAL_GLOW * 2.0


def frame_glow_inset(plate_h, glow, src_plate_h):
    """How far outside the plate rectangle the sprite has to be drawn."""
    return plate_h * (glow / src_plate_h)


def vec2_candidates(x, y):
    """Every two-component value this engine can build, most likely first.

    UE 5.8 declares FSlateBrush::ImageSize as FDeprecateSlateVector2D, and the
    Python bridge refuses a plain Vector2D for it outright ("Cannot nativize
    'Vector2D' as 'DeprecateSlateVector2D'").

    THE TWO-STEP CONSTRUCTION IS NOT DEFENSIVE PADDING, and leaving it out is what
    broke this script's first run: FDeprecateSlateVector2D takes NO constructor
    arguments through the bindings ("call() takes at most 0 arguments"), exactly
    like unreal.Key. A `maker(x, y)` therefore raises, the fallback hands over a
    Vector2D, and the property set dies with the nativize error above - after the
    sprites have imported and before a single widget is authored, which is a state
    that looks like success until you open the menu. It must be default-constructed
    and then written into. Same fact as Scripts/generate-hud-widgets.py's set_vec2,
    which learned it one struct along.
    """
    out = []
    for type_name in ("DeprecateSlateVector2D", "Vector2D", "Vector2f"):
        maker = getattr(unreal, type_name, None)
        if maker is None:
            continue
        try:
            out.append(maker(x, y))
        except Exception:
            try:
                value = maker()
                value.set_editor_property("x", x)
                value.set_editor_property("y", y)
                out.append(value)
            except Exception:
                pass
    return out


def set_vec2(obj, prop, x, y):
    """Set a two-component PROPERTY, whatever this engine calls the type today.

    *** PROPERTIES ONLY. NEVER FOR A FUNCTION ARGUMENT. ***
    set_editor_property checks the struct type and raises, so a wrong guess here is
    a caught exception and the next candidate runs. A UFUNCTION argument is not
    checked the same way: passing an FDeprecateSlateVector2D (two floats) where an
    FVector2D (two doubles) is expected is ACCEPTED, writes the wrong number of
    bytes and corrupts the heap. That is not a theory - it cost a five-way
    bisection in the HUD generator, where it surfaced as a SIGSEGV inside Python's
    garbage collector during an unrelated blueprint compile. Call setters (like
    UCanvasPanelSlot::SetAlignment, below) with the exact type."""
    last_error = None
    for value in vec2_candidates(x, y):
        try:
            obj.set_editor_property(prop, value)
            return
        except Exception as error:
            last_error = error
    if last_error is None:
        last_error = RuntimeError("no two-component type in this engine's bindings")
    raise last_error


def frame_image_size(plate_h, glow, src_plate_h, src_w, src_h):
    """Brush ImageSize for a plate drawn @plate_h tall, as a plain (w, h) pair.

    Slate draws a Box brush's corners at Margin * ImageSize, so this is the
    number that decides the corner's on-screen size. Deriving it from the
    sheet's aspect ratio is what keeps the corner circular rather than oval at
    every row width.

    Returned as a tuple rather than an engine struct because which struct the
    engine wants is set_vec2's problem, and it can only answer it against the
    property it is writing to."""
    height = plate_h + frame_glow_inset(plate_h, glow, src_plate_h) * 2.0
    return (height * (src_w / src_h), height)


def frame_margin(cap, src_w, src_h):
    return unreal.Margin(left=cap / src_w, top=cap / src_h,
                         right=cap / src_w, bottom=cap / src_h)


# =============================================================================
# Palette.
#
# The brief: "Keep the background black." So the screen is black, the artist's
# sprites carry every colour that is not type, and the type is white / grey /
# their amber. The neon-cyan grid the Canvas renderer draws is deliberately NOT
# reproduced here - see TraceTitleMenuWidget.h.
# =============================================================================

def C(r, g, b, a=1.0):
    return unreal.LinearColor(r, g, b, a)


def srgb(r, g, b, a=1.0):
    """A colour sampled off the sheet, in the 0-255 the sheet is measured in."""
    linear = unreal.LinearColor(0, 0, 0, 1).from_s_rgb_color(unreal.Color(r, g, b, 255)) \
        if hasattr(unreal.LinearColor, "from_s_rgb_color") else None
    if linear is not None:
        linear.a = a
        return linear
    # UE 5.8 python has no FromSRGBColor helper on LinearColor; do it by hand.
    def channel(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return C(channel(r), channel(g), channel(b), a)


BLACK     = C(0.0, 0.0, 0.0, 1.0)
INK       = C(0.95, 0.96, 1.00, 1.00)
INK_DIM   = C(0.52, 0.55, 0.62, 1.00)
INK_FAINT = C(0.38, 0.40, 0.46, 1.00)
AMBER     = C(1.00, 0.46, 0.08, 1.00)
CLEAR     = C(0.0, 0.0, 0.0, 0.0)
SCRIM     = C(0.0, 0.0, 0.0, 0.86)

# Reference pixels. The design space is 1080 tall; UMG's default DPI curve is
# (shortest side / 1080), which is exactly ATraceMenuHUD::UIScale.
ROW_HEIGHT   = 60.0
ROW_GAP      = 11.0        # RowSpacing 71 - RowHeight 60
ROW_PAD_X    = 30.0
PANEL_PAD_X  = 34.0
PANEL_PAD_T  = 22.0
PANEL_PAD_B  = 22.0
PANEL_TOP_Y  = 0.395 * 1080.0
PANEL_WIDTH  = 720.0 + PANEL_PAD_X * 2.0

# THE ROWS, IN ORDER, AND THIS LIST MUST MATCH ETraceMenuRow EXACTLY - same count,
# same order. ATraceMenuHUD::AdoptTitleWidget refuses the whole asset and falls
# back to the Canvas renderer when the counts differ, so a mismatch does not draw
# a wrong menu, it draws the old one. RowPractice is spec v19 section 2.
#
# Declared up here rather than beside build_title_menu() because PANEL_HEIGHT is
# derived from its length.
# RowMode (SCORING MODE) was between RowDifficulty and RowSettings until the
# endzone ruleset was removed. Deleting it here is half of that change; the other
# half is ETraceMenuRow, and the two MUST be deleted together or the widget is
# refused and the Canvas fallback silently draws the old seven-row menu instead.
ROW_NAMES = ["RowPlay", "RowJoin", "RowPractice", "RowDifficulty",
             "RowSettings", "RowQuit"]

# DERIVED FROM THE ROW COUNT, not a literal, because spec v19 §2 added a row
# (PRACTICE) and the old hard-coded 495.0 silently cropped it. Removing the
# SCORING MODE row shrinks the panel through the same expression.
#
# The first three terms are TraceMenuStyle::ComputeConsoleLayout's PanelH, copied
# term for term so the two renderers cannot disagree about where the panel ends:
#     PadTop + (N-1) * RowSpacing + RowHeight + PadBottom
# The last term is the blurb line, which lives inside this border on the UMG path
# and outside the Canvas panel rect, and is the only reason the two numbers differ.
BLURB_BLOCK  = 36.0
PANEL_HEIGHT = (PANEL_PAD_T
                + (len(ROW_NAMES) - 1) * (ROW_HEIGHT + ROW_GAP)
                + ROW_HEIGHT
                + PANEL_PAD_B
                + BLURB_BLOCK)

# =============================================================================
# THE ROW LABEL'S SIZE IS THE KIT'S - DERIVED, NOT CHOSEN  (UI QA finding F2)
#
# Every kit screen sets a button's label with its CAPS at
# TraceMenuKit::LabelCapFraction of the plate (0.37 - the sheet's 450/1230), and
# TraceMenuKit::LabelSize() turns that into a text size through the face's own
# cap height (TraceText::SizeForCapHeight). The pause menu, the settings page and
# the loadout screen all size their labels that way.
#
# The title row did not: it was a literal 25 from the v17 pass, set against the
# Lato stand-in before the rows typed in Sofachrome. In Sofachrome ExtraLight 25
# is a cap of 17 px on a 60 px plate (0.28 H), so the same button read about 30%
# smaller on the title than in the pause menu one Escape away.
#
# So the size is now COMPUTED from the three numbers the C++ computes it from,
# read out of the C++ headers rather than copied (the font block below explains
# why a copy here is worse than a parse):
#     LabelCapFraction                  Source/Trace/UI/Widgets/Menu/TraceMenuKit.h
#     EmSize, the Light face's cap      Source/Trace/UI/Text/TraceFontAtlasMetrics.h
#     size = (plate_h * LabelCapFraction) * EmSize / CapHeight
# Light because every title label is Light (TraceAtlasTextSwap::Install's default,
# the owner's v23 choice). A header that does not parse FAILS the run - a silent
# default here is exactly the drift this replaces.
#
# ONLY THE LABEL. The DIFFICULTY value keeps 25 (FS_ROW_VALUE): it sits in the
# 34 px value chip, not on the 60 px plate, and at the label's size its caps
# would fill two thirds of that chip.
# =============================================================================

KIT_HEADER = os.path.join(PROJECT_DIR, "Source", "Trace", "UI", "Widgets", "Menu", "TraceMenuKit.h")
ATLAS_METRICS_HEADER = os.path.join(PROJECT_DIR, "Source", "Trace", "UI", "Text", "TraceFontAtlasMetrics.h")


def header_number(path, pattern, what):
    """The first capture of @pattern in @path, as a float. None (and a recorded failure) if absent."""
    try:
        with open(path, "r") as handle:
            text = handle.read()
    except IOError as error:
        fail("could not read {0} for {1}: {2}".format(path, what, error))
        return None
    match = re.search(pattern, text)
    if match is None:
        fail("{0} no longer declares {1} in the form this script parses ({2}). The title row's label "
             "size is derived from it; fix the pattern here rather than typing a number in.".format(
                 path, what, pattern))
        return None
    return float(match.group(1))


def kit_label_size(plate_h):
    """TraceMenuKit::LabelSize(plate_h, Light): the size whose Light caps are 0.37 of @plate_h."""
    number = r"([0-9]+\.?[0-9]*)f?"   # 0.37f, 96.f, 65.f
    cap_fraction = header_number(KIT_HEADER, r"\bLabelCapFraction\s*=\s*" + number + r"\s*;",
                                 "TraceMenuKit::LabelCapFraction")
    em_size = header_number(ATLAS_METRICS_HEADER, r"\bEmSize\s*=\s*" + number + r"\s*;",
                            "TraceFontAtlasMetrics::EmSize")
    # Faces[] row: { Name, Source, TextureAsset, Erosion, AtlasW, AtlasH, Ascent, Descent, CapHeight, Cells }
    face_cap = header_number(
        ATLAS_METRICS_HEADER,
        r"\{\s*TEXT\(\"Light\"\)\s*,\s*TEXT\(\"[^\"]*\"\)\s*,\s*TEXT\(\"[^\"]*\"\)\s*,"
        r"\s*[0-9.]+f?\s*,\s*\d+\s*,\s*\d+\s*,\s*[0-9.]+f?\s*,\s*[0-9.]+f?\s*,\s*" + number + r"\s*,",
        "the Light face's CapHeight (TraceFontAtlasMetrics::Faces[0])")
    if cap_fraction is None or em_size is None or not face_cap:
        return None
    return round(plate_h * cap_fraction * em_size / face_cap, 2)


# Font sizes, in the 1080 design space. All but the row label unchanged from the
# v17 pass, which set them against a 1280x720 capture of both renderers side by side.
FS_ROW_LABEL   = kit_label_size(ROW_HEIGHT)   # 32.79 today: caps 22.2 px on the 60 px plate
FS_ROW_VALUE   = 25        # the DIFFICULTY value, in its 34 px chip - see the block above
FS_ROW_STATUS  = 13
FS_ROW_ARROW   = 20
FS_TAGLINE     = 15
FS_CHIP_CAP    = 12
FS_CHIP_VALUE  = 18
FS_BLURB       = 15
FS_FOOTER      = 13
FS_BANNER_HEAD = 25        # caps 0.38 of the 44-tall plate, about the kit's 0.37
FS_BANNER_BODY = 15

# The failure banner's plate: a kit button's height, the one line at the kit's label size on it.
BANNER_PLATE_H = 44.0
BANNER_PAD_X   = 22.0
BANNER_PAD_Y   = 7.0
FS_TRAVEL_CAP  = 22
FS_TRAVEL_HINT = 13
FS_WARNING     = 13

# =============================================================================
# THE FONT (spec v20 section 1). NOT DECLARED HERE - PARSED OUT OF THE C++.
#
# This script bakes an FSlateFontInfo into every text block of WBP_TitleMenu, so
# whatever is named here IS the font on screen. Until v20 it named its own copy
# of the same two strings that TraceMenuArtStyle.h names, and the two drifted in
# the worst possible direction: the C++ constant fed a log line and NOTHING on
# screen, so an agent could change the font in C++, watch the log say Lato, and
# photograph a menu still in Roboto.
#
# So there is now exactly ONE declaration and it is the C++ one. This reads it.
# The parse is deliberately dumb (a regex over `static const TCHAR* const NAME =
# TEXT("...")`) and it FAILS LOUDLY rather than defaulting, because a silent
# default here is precisely the drift it exists to end.
# =============================================================================

ART_STYLE_HEADER = os.path.join(PROJECT_DIR, "Source", "Trace", "UI", "Widgets",
                                "Menu", "TraceMenuArtStyle.h")

# Where the .ttf lives in source control, and where its imported UFont lands.
FONT_SOURCE_DIR = os.path.join(PROJECT_DIR, "Art", "Fonts")
FONT_DIR = os.environ.get("TRACE_MENU_FONT_DIR", "/Game/Trace/UI/Fonts")


def art_style_constant(name):
    """`static const TCHAR* const <name> = TEXT("value");` out of TraceMenuArtStyle.h."""
    try:
        with open(ART_STYLE_HEADER, "r") as handle:
            header = handle.read()
    except IOError as error:
        fail("could not read {0}: {1}. That header is the ONLY place this project names a font; "
             "this script has no copy to fall back on, by design.".format(ART_STYLE_HEADER, error))
        return None

    match = re.search(r"\b" + re.escape(name) + r"\s*=\s*TEXT\(\s*\"([^\"]*)\"\s*\)", header)
    if match is None:
        fail("{0} does not declare {1}. Spec v20 section 1 requires the font to be named in exactly "
             "one constant, and this script reads that constant rather than keeping its own copy."
             .format(ART_STYLE_HEADER, name))
        return None
    return match.group(1)


def package_path(object_path):
    """/Game/X/Y.Y -> /Game/X/Y. EditorAssetLibrary wants the package, C++ wants the object."""
    return object_path.split(".")[0] if object_path else object_path


MENU_FONT_SOURCE_FILE = art_style_constant("MenuFontSourceFile")
MENU_FONT_ASSET = package_path(art_style_constant("MenuFontAsset"))
MENU_FONT_TYPEFACE = art_style_constant("MenuFontTypeface")
MENU_FONT_FALLBACK_ASSET = package_path(art_style_constant("MenuFontFallbackAsset"))
MENU_FONT_FALLBACK_TYPEFACE = art_style_constant("MenuFontFallbackTypeface")

# The UFontFace the .ttf imports to. A UFont is a wrapper around one or more of
# these; only the UFont is named in C++, because only the UFont can be handed to
# Slate (UFontFace does not implement IFontProviderInterface, so an
# FSlateFontInfo pointed at one silently draws the last-resort face).
MENU_FONT_FACE_ASSET = MENU_FONT_ASSET + "_Face" if MENU_FONT_ASSET else None

MENU_FONT = None
MENU_FONT_IS_FALLBACK = False


def live_typeface():
    """The typeface name to bake into a text block: it follows whichever font we
    actually got. Baking 'Default' into a Roboto fallback would quietly draw
    Roboto Regular instead of Roboto Light, which is a second wrong font on top
    of the first."""
    return MENU_FONT_FALLBACK_TYPEFACE if MENU_FONT_IS_FALLBACK else MENU_FONT_TYPEFACE


def slate_is_available():
    """Can this process import a font without taking the editor down?

    IT CANNOT IN A COMMANDLET, and the failure is not an exception - it is an
    assert. UFontFace::PostEditChangeProperty (Engine/Source/Runtime/Engine/
    Private/FontFace.cpp:224) ends with an unconditional

        FSlateApplication::Get().GetRenderer()->FlushFontCache(...)

    and FEngineLoop only creates a Slate application for a regular client or the
    editor token (LaunchEngineLoop.cpp:3109), never for `-run=<commandlet>`. So
    importing a .ttf from `-run=pythonscript` hits
    `Assertion failed: CurrentApplication.IsValid()` and SIGSEGVs the whole run
    AFTER the sprites have imported - measured, in Saved/Logs/v20font-probe.log.

    There is no script-exposed IsRunningCommandlet, so this asks the command line
    the same question the engine did. Being wrong in the safe direction costs one
    clear error message; being wrong in the other direction costs the editor."""
    if os.environ.get("TRACE_FONT_IMPORT") == "0":
        return False
    if os.environ.get("TRACE_FONT_IMPORT") == "1":
        return True
    try:
        return "-run=" not in unreal.SystemLibrary.get_command_line()
    except Exception:
        return False


def import_menu_font():
    """Art/Fonts/<MenuFontSourceFile> -> a UFont at MENU_FONT_ASSET.

    Returns the UFont, or the /Engine/EngineFonts/Roboto fallback, or None. The
    fallback matters: a menu in the wrong font beats a menu with no text, and it
    is the same fallback TraceMenuArtStyle.cpp takes at runtime, so the two
    cannot disagree about what a missing font means."""

    def fall_back(reason):
        global MENU_FONT_IS_FALLBACK
        MENU_FONT_IS_FALLBACK = True
        log("  falling back to {0} '{1}': {2}".format(
            MENU_FONT_FALLBACK_ASSET, MENU_FONT_FALLBACK_TYPEFACE, reason))
        return unreal.EditorAssetLibrary.load_asset(MENU_FONT_FALLBACK_ASSET)

    if not MENU_FONT_ASSET or not MENU_FONT_SOURCE_FILE:
        return fall_back("the font constants did not parse out of TraceMenuArtStyle.h")

    existing = unreal.EditorAssetLibrary.load_asset(MENU_FONT_ASSET)
    source = os.path.join(FONT_SOURCE_DIR, MENU_FONT_SOURCE_FILE)

    if not os.path.isfile(source):
        if isinstance(existing, unreal.Font):
            log("  {0} is not on disk, but {1} is already imported - using it.".format(
                source, MENU_FONT_ASSET))
            return existing
        fail("{0} does not exist. TraceMenuArtStyle::MenuFontSourceFile names it and this script "
             "imports it; put the file there or change that one constant.".format(source))
        return fall_back("the source file is missing")

    if isinstance(existing, unreal.Font) and not slate_is_available():
        # Already imported and we are in a commandlet: use it rather than crash.
        log("  {0} already imported; not re-importing (no Slate application in this process).".format(
            MENU_FONT_ASSET))
        return existing

    if not slate_is_available():
        fail("{0} is not imported and this process cannot import a font: a .ttf import asserts "
             "without a Slate application (see slate_is_available). Re-run this script from an "
             "editor rather than `-run=pythonscript`:\n"
             "    UnrealEditor Trace.uproject -ExecCmds=\"py <this script>,QUIT_EDITOR\" "
             "-unattended -nosplash -nosound -RenderOffScreen".format(MENU_FONT_ASSET))
        return fall_back("no Slate application in this process")

    # A .ttf imports as a UFontFace. BatchCreateFontAsset asks the factory to
    # build the UFont wrapper around it in the same step - the same thing the
    # content browser's "create a Font asset too?" prompt does, minus the prompt.
    factory = unreal.FontFileImportFactory()
    set_prop(factory, "batch_create_font_asset", "BatchCreateFontAsset",
             unreal.BatchCreateFontAsset.YES)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", source)
    task.set_editor_property("destination_path", FONT_DIR)
    task.set_editor_property("destination_name", os.path.basename(MENU_FONT_FACE_ASSET))
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    task.set_editor_property("factory", factory)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    # The factory names the UFont itself - in UE 5.8 it appends "_Font" to the
    # face, i.e. F_TraceMenu_Face_Font - and that name is an engine detail nobody
    # should have to know. Find whatever Font came out and move it to the name
    # C++ asked for, so the one constant in the header stays the truth even if a
    # future engine names it something else.
    font = unreal.EditorAssetLibrary.load_asset(MENU_FONT_ASSET)
    if not isinstance(font, unreal.Font):
        for path in unreal.EditorAssetLibrary.list_assets(FONT_DIR, recursive=True):
            candidate = unreal.EditorAssetLibrary.load_asset(path)
            if isinstance(candidate, unreal.Font):
                stripped = package_path(path)
                log("  factory created the UFont as {0}; renaming to {1}".format(
                    stripped, MENU_FONT_ASSET))
                unreal.EditorAssetLibrary.rename_asset(stripped, MENU_FONT_ASSET)
                font = unreal.EditorAssetLibrary.load_asset(MENU_FONT_ASSET)
                break

    if not isinstance(font, unreal.Font):
        fail("{0} imported no UFont. A UFontFace on its own cannot be handed to Slate - an "
             "FSlateFontInfo pointing at one draws the last-resort face and says nothing."
             .format(source))
        return fall_back("the import produced no UFont")

    for path in (MENU_FONT_ASSET, MENU_FONT_FACE_ASSET):
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            unreal.EditorAssetLibrary.save_asset(path)

    log("  {0} -> {1} (typeface '{2}')".format(
        os.path.relpath(source, PROJECT_DIR), MENU_FONT_ASSET, MENU_FONT_TYPEFACE))
    return font

UMG_SETTINGS_PATH = "/Script/UMGEditor.Default__UMGEditorProjectSettings"


# =============================================================================
# Step 1 - import the sprites
# =============================================================================

# Every sprite the sliced set contains, and whether the title screen places it.
# The two marked False are imported anyway and deliberately: they are the slider
# parts, they belong to the SETTINGS screen, and that screen is still drawn on a
# Canvas by FTraceOptionsMenu, which is not this pass's file to change. Importing
# them now means the settings work is a layout job, not an art job.
SPRITES = [
    ("T_MenuBtn_Default", True),
    ("T_MenuBtn_Hover", True),
    ("T_MenuBtn_Disabled", True),
    ("T_MenuWord_Play", True),
    ("T_MenuWord_Settings", True),
    ("T_MenuWord_Keybind", False),
    ("T_MenuWord_Key", False),
    ("T_MenuValueBox", True),
    ("T_MenuSliderTrack", False),
    ("T_MenuSliderHandle", False),
    ("T_TraceWordmark", True),
    ("T_MenuSwoosh", True),
    ("T_MenuCursor", True),
    ("T_MenuBack", True),
]

Textures = {}


def import_sprites():
    """PNG on disk -> UTexture2D in /Game/Trace/UI/Art, set up for UI."""
    if not os.path.isdir(SPRITE_SOURCE_DIR):
        fail("{0} does not exist. Run `python3 Scripts/slice-ui-assets.py` first - it cuts the "
             "artist's sheet into the sprites this script imports.".format(SPRITE_SOURCE_DIR))
        return

    wanted = [entry for entry in SPRITES if not IMPORT_ONLY or entry[0] in IMPORT_ONLY]
    for name in IMPORT_ONLY:
        if name not in [entry[0] for entry in SPRITES]:
            fail("TRACE_IMPORT_ONLY names {0}, which is not one of the sliced sprites.".format(name))

    tasks = []
    for name, _placed in wanted:
        source = os.path.join(SPRITE_SOURCE_DIR, name + ".png")
        if not os.path.isfile(source):
            fail("{0} is missing from {1}. Re-run Scripts/slice-ui-assets.py.".format(name, SPRITE_SOURCE_DIR))
            continue
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", source)
        task.set_editor_property("destination_path", ART_DIR)
        task.set_editor_property("destination_name", name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        tasks.append(task)

    if not tasks:
        return

    if SKIP_IMPORT:
        log("TRACE_SKIP_IMPORT=1: not re-importing, only re-authoring the widgets.")
    else:
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    for name, _placed in wanted:
        path = "{0}/{1}".format(ART_DIR, name)
        texture = unreal.EditorAssetLibrary.load_asset(path)
        if texture is None:
            fail("{0} did not import.".format(path))
            continue

        # UI settings, and each one earns its place:
        #   TEXTUREGROUP_UI + no mips   a menu sprite is never seen at a distance, and a mip chain on
        #                               a 9-sliced frame bleeds the corner into the stretch.
        #   TC_EDITOR_ICON              this is the "UserInterface2D (RGBA)" setting: uncompressed
        #                               RGBA. Block compression puts visible blotches in exactly the
        #                               kind of smooth glow this whole sheet is made of.
        #   never_stream                a title screen must not pop in.
        set_prop(texture, "lod_group", "LODGroup", unreal.TextureGroup.TEXTUREGROUP_UI)
        set_prop(texture, "compression_settings", "CompressionSettings",
                 unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        set_prop(texture, "mip_gen_settings", "MipGenSettings",
                 unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        set_prop(texture, "srgb", "SRGB", True)
        set_prop(texture, "never_stream", "NeverStream", True)
        unreal.EditorAssetLibrary.save_loaded_asset(texture)

        Textures[name] = texture
        log("  {0:22s} {1} x {2}".format(name,
                                         texture.blueprint_get_size_x(),
                                         texture.blueprint_get_size_y()))


def brush(texture_name, image_size=None, margin=None, tint=None):
    """A Slate brush over one of the imported sprites.

    With a margin it is drawn as a BOX (9-slice); without one, as a plain
    stretched IMAGE. A brush whose texture is missing is returned EMPTY rather
    than half-set, because Slate draws a brush with no texture as a solid white
    rectangle - which does not look broken, it looks like a different design."""
    out = unreal.SlateBrush()
    texture = Textures.get(texture_name)
    if texture is None:
        return out
    out.set_editor_property("resource_object", texture)
    if image_size is None:
        image_size = (texture.blueprint_get_size_x(), texture.blueprint_get_size_y())
    set_vec2(out, "image_size", image_size[0], image_size[1])
    if margin is not None:
        out.set_editor_property("draw_as", unreal.SlateBrushDrawType.BOX)
        out.set_editor_property("margin", margin)
    else:
        out.set_editor_property("draw_as", unreal.SlateBrushDrawType.IMAGE)
    if tint is not None:
        out.set_editor_property("tint_color", unreal.SlateColor(specified_color=tint))
    return out


def button_brush(texture_name, plate_height):
    return brush(texture_name,
                 frame_image_size(plate_height, BTN_GLOW, BTN_PLATE_H, BTN_SPRITE_W, BTN_SPRITE_H),
                 frame_margin(BTN_CAP, BTN_SPRITE_W, BTN_SPRITE_H))


def word_brush(texture_name, cap_height):
    """A baked word, scaled to a cap height. Aspect comes from the texture."""
    texture = Textures.get(texture_name)
    if texture is None:
        return unreal.SlateBrush()
    width = texture.blueprint_get_size_x()
    height = texture.blueprint_get_size_y()
    return brush(texture_name, (cap_height * (width / float(height)), cap_height))


def sprite_brush(texture_name, height):
    return word_brush(texture_name, height)


# =============================================================================
# Widget-tree helpers
# =============================================================================

def mk(tree, cls, name):
    """Create a widget owned by @tree. The NAME is the whole BindWidget contract."""
    return unreal.new_object(cls, tree, name)


def anchors(min_x, min_y, max_x=None, max_y=None):
    """unreal.Anchors, built with whichever two-component type FAnchors holds here.

    FAnchors' members went the same way FSlateBrush::ImageSize did, so this is
    built by the same try-each-candidate route rather than by naming one type and
    hoping. Both members must come from the SAME candidate - a half-Vector2D,
    half-DeprecateSlateVector2D FAnchors would be two different memory layouts in
    one struct."""
    if max_x is None:
        max_x = min_x
    if max_y is None:
        max_y = min_y

    last_error = None
    minimums = vec2_candidates(min_x, min_y)
    maximums = vec2_candidates(max_x, max_y)
    for index in range(min(len(minimums), len(maximums))):
        try:
            out = unreal.Anchors()
            out.set_editor_property("minimum", minimums[index])
            out.set_editor_property("maximum", maximums[index])
            return out
        except Exception as error:
            last_error = error
    raise last_error if last_error is not None else RuntimeError("cannot build unreal.Anchors")


def slot_on_canvas(panel, child, anc, offsets, alignment=(0.0, 0.0),
                   auto_size=False, z_order=0):
    """
    Add @child to a canvas panel.

    UMG's offset convention is worth stating because getting it backwards is
    silent: when the anchor is a POINT (min == max) the offsets read
    (Left, Top, Width, Height); when the anchor SPANS, they read
    (Left, Top, Right, Bottom) as insets from the anchor box - so a NEGATIVE
    value there grows the widget outwards, which is exactly what the plate
    sprite's glow overhang needs.
    """
    panel.add_child(child)
    canvas_slot = child.slot
    canvas_slot.set_anchors(anc)
    canvas_slot.set_offsets(unreal.Margin(
        left=offsets[0], top=offsets[1], right=offsets[2], bottom=offsets[3]))
    canvas_slot.set_alignment(unreal.Vector2D(alignment[0], alignment[1]))
    canvas_slot.set_auto_size(auto_size)
    canvas_slot.set_z_order(z_order)
    return canvas_slot


def make_text(tree, name, placeholder, size, color, justify=unreal.TextJustify.CENTER):
    block = mk(tree, unreal.TextBlock, name)
    block.set_editor_property("text", placeholder)
    font = block.get_editor_property("font")
    font.set_editor_property("font_object", MENU_FONT)
    font.set_editor_property("typeface_font_name", live_typeface())
    font.set_editor_property("size", size)
    block.set_editor_property("font", font)
    block.set_editor_property("color_and_opacity", unreal.SlateColor(specified_color=color))
    block.set_editor_property("justification", justify)
    return block


def make_image(tree, name, color=None, image_brush=None):
    """
    A sprite, or - with no brush - a plain coloured rectangle.

    UImage's default brush has no texture, which Slate renders as solid white,
    so the tint IS the colour. That is what the black backdrop uses, and it is
    also why a sprite that failed to import does not look absent: it looks like
    a white box. `Trace.UI.VerifyMenuArt` exists to catch exactly that.
    """
    image = mk(tree, unreal.Image, name)
    if image_brush is not None:
        image.set_editor_property("brush", image_brush)
    if color is not None:
        image.set_editor_property("color_and_opacity", color)
    return image


def make_border(tree, name, color, padding, background=None):
    brd = mk(tree, unreal.Border, name)
    if background is not None:
        brd.set_editor_property("background", background)
    brd.set_editor_property("brush_color", color)
    brd.set_editor_property("padding", unreal.Margin(
        left=padding[0], top=padding[1], right=padding[2], bottom=padding[3]))
    brd.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    brd.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_FILL)
    return brd


def vbox_slot(box, child, padding=(0.0, 0.0, 0.0, 0.0),
              h_align=unreal.HorizontalAlignment.H_ALIGN_FILL,
              v_align=unreal.VerticalAlignment.V_ALIGN_TOP):
    box.add_child(child)
    box_slot = child.slot
    box_slot.set_editor_property("padding", unreal.Margin(
        left=padding[0], top=padding[1], right=padding[2], bottom=padding[3]))
    box_slot.set_editor_property("size", unreal.SlateChildSize(
        value=1.0, size_rule=unreal.SlateSizeRule.AUTOMATIC))
    box_slot.set_editor_property("horizontal_alignment", h_align)
    box_slot.set_editor_property("vertical_alignment", v_align)
    return box_slot


def hbox_slot(box, child, padding=(0.0, 0.0, 0.0, 0.0)):
    box.add_child(child)
    box_slot = child.slot
    box_slot.set_editor_property("padding", unreal.Margin(
        left=padding[0], top=padding[1], right=padding[2], bottom=padding[3]))
    box_slot.set_editor_property("size", unreal.SlateChildSize(
        value=1.0, size_rule=unreal.SlateSizeRule.AUTOMATIC))
    box_slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_CENTER)
    return box_slot


def overlay_slot(overlay, child, padding=(0.0, 0.0, 0.0, 0.0),
                 h_align=unreal.HorizontalAlignment.H_ALIGN_FILL,
                 v_align=unreal.VerticalAlignment.V_ALIGN_FILL):
    overlay.add_child(child)
    child_slot = child.slot
    child_slot.set_editor_property("padding", unreal.Margin(
        left=padding[0], top=padding[1], right=padding[2], bottom=padding[3]))
    child_slot.set_editor_property("horizontal_alignment", h_align)
    child_slot.set_editor_property("vertical_alignment", v_align)
    return child_slot


# =============================================================================
# Asset creation - see "THREE EDITOR-PYTHON FACTS" in the header
# =============================================================================

RetiredCount = [0]
RetiredToTransient = [0]


def resolve_parent_class(class_name):
    """The C++ class, or None with a loud complaint. Never silently substitutes."""
    parent = getattr(unreal, class_name, None)
    if parent is None:
        fail("unreal.{0} does not exist. The Trace game module is not built, or the class was "
             "renamed. Build with ./Scripts/build.sh and re-run; do NOT let this script author an "
             "asset with the wrong parent class.".format(class_name))
    return parent


def transient_package():
    for path in ("/Engine/Transient", "/Temp/Transient"):
        package = unreal.find_object(None, path)
        if package is not None:
            return package
    return None


def retire_descendants(widget, transient):
    """
    Get everything under @widget out of the asset so a re-run can reuse the names.

    THIS IS THE ~40 SECOND LAUNCH STALL. The old version renamed each orphan to
    TraceRetired_N and left it in the package, so WBP_TitleMenu shipped with 27
    dead widgets in it and the Blueprint re-resolved and complained about every
    one on every load. Renaming them into the TRANSIENT package instead means
    they are never serialised, so they do not reach the .uasset at all.

    The root itself is kept: its pointer is the one thing in the tree Python
    cannot set, so losing it would mean losing the asset.
    """
    if not isinstance(widget, unreal.PanelWidget):
        return
    for child in list(widget.get_all_children()):
        retire_descendants(child, transient)
        RetiredCount[0] += 1
        moved = False
        if transient is not None:
            try:
                child.rename(name="TraceRetired_{0}".format(RetiredCount[0]), outer=transient)
                moved = True
                RetiredToTransient[0] += 1
            except Exception as error:
                log("  (could not move {0} to the transient package: {1})".format(child.get_name(), error))
        if not moved:
            try:
                child.rename("TraceRetired_{0}".format(RetiredCount[0]))
            except Exception as error:
                log("  (could not retire {0}: {1})".format(child.get_name(), error))
    widget.clear_children()


def open_widget_blueprint(name, parent_class, root_class, root_name):
    """
    Load-and-reset, or create with @root_class as the tree root.

    Returns (blueprint, tree, root) or (None, None, None).
    """
    package_path = "{0}/{1}".format(MENU_DIR, name)
    existing = unreal.EditorAssetLibrary.load_asset(package_path)

    if existing is None:
        log("{0}: creating".format(name))
        settings = unreal.load_object(None, UMG_SETTINGS_PATH)
        if settings is None:
            fail("Could not reach {0}. Without it the new asset has no root widget and nothing "
                 "can be placed in it.".format(UMG_SETTINGS_PATH))
            return None, None, None

        previous_root = settings.get_editor_property("DefaultRootWidget")
        try:
            settings.set_editor_property("DefaultRootWidget", root_class)
            factory = unreal.WidgetBlueprintFactory()
            factory.set_editor_property("parent_class", parent_class)
            asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
            existing = asset_tools.create_asset(name, MENU_DIR, unreal.WidgetBlueprint, factory)
        finally:
            # Put the editor's own setting back. It is an in-memory CDO change and
            # nothing here writes an .ini, but leaving it changed would silently
            # alter what every OTHER new widget blueprint in this session gets.
            settings.set_editor_property("DefaultRootWidget", previous_root)

        if existing is None:
            fail("{0}: create_asset returned None".format(name))
            return None, None, None
    else:
        log("{0}: rewriting in place".format(name))
        unreal.BlueprintEditorLibrary.reparent_blueprint(existing, parent_class)

    tree = unreal.find_object(existing, "WidgetTree")
    if tree is None:
        fail("{0}: has no WidgetTree inner object".format(name))
        return None, None, None

    # The root was named by the factory (e.g. "CanvasPanel_0") on the first run
    # and renamed to root_name; on a re-run it is already root_name.
    root = unreal.find_object(tree, root_name)
    if root is None:
        for candidate in ["{0}_0".format(root_class.__name__), root_class.__name__]:
            root = unreal.find_object(tree, candidate)
            if root is not None:
                root.rename(root_name)
                break
    if root is None:
        fail("{0}: no root widget in the tree. UUMGEditorProjectSettings::DefaultRootWidget did "
             "not take effect, and Python cannot set UWidgetTree::RootWidget directly.".format(name))
        return None, None, None

    retire_descendants(root, transient_package())
    return existing, tree, root


def finish(asset, name):
    unreal.BlueprintEditorLibrary.compile_blueprint(asset)
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset):
        fail("{0}: save failed".format(name))
    else:
        log("{0}: compiled and saved".format(name))


# =============================================================================
# WBP_MenuRow  -  spec v19 section 5: the row IS the artist's button
#
# The root is a SizeBox forcing the 60-reference-pixel row height, so the six
# rows in a vertical box land on the 71-pixel pitch the Canvas renderer uses
# (60 tall + an 11 gap). The row's own geometry is what ATraceMenuHUD hit-tests
# against, so this height is load-bearing, not styling - and it is why the row
# is still 60 tall and 720 wide rather than the sheet's chunkier 3.8:1 button:
# moving it would move every hit rectangle, every harness and both renderers'
# agreement at once. The SPRITE is 9-sliced onto that rectangle instead.
# =============================================================================

def build_menu_row():
    parent = resolve_parent_class("TraceMenuRow")
    if parent is None:
        return None

    asset, tree, root = open_widget_blueprint("WBP_MenuRow", parent, unreal.SizeBox, "RowRoot")
    if asset is None:
        return None

    root.set_editor_property("height_override", ROW_HEIGHT)
    root.set_editor_property("bOverride_HeightOverride", True)

    canvas = mk(tree, unreal.CanvasPanel, "RowCanvas")
    root.add_child(canvas)

    full = anchors(0.0, 0.0, 1.0, 1.0)

    # ---- The plate --------------------------------------------------------------------------------
    #
    # Drawn OUTSIDE the row rectangle by the glow overhang on every side. The sprite carries 128 sheet
    # pixels of halo around the plate; inside the row rect the plate itself would come out a fifth
    # short and every row would look like it had shrunk.
    inset = frame_glow_inset(ROW_HEIGHT, BTN_GLOW, BTN_PLATE_H)
    slot_on_canvas(canvas, make_image(tree, "PlateSprite", image_brush=button_brush("T_MenuBtn_Default", ROW_HEIGHT)),
                   full, (-inset, -inset, -inset, -inset), z_order=0)

    # ---- The chevron on the leading edge, OUTSIDE the row -----------------------------------------
    #
    # The artist's crescent, which is also the back glyph on the sheet. It replaces a ">" typed in
    # whatever font happened to be loaded - the one mark on this screen that used to be a character
    # and is now a drawing.
    chevron_h = 26.0
    chevron = make_image(tree, "ChevronSprite", image_brush=sprite_brush("T_MenuBack", chevron_h))
    slot_on_canvas(canvas, chevron, anchors(0.0, 0.5), (-14.0, 0.0, 0.0, 0.0),
                   alignment=(1.0, 0.5), auto_size=True, z_order=4)

    # ---- The label: sprite where the sheet has the word, type where it does not -------------------
    #
    # Both are authored; UTraceMenuRow shows one and collapses the other from bLabelIsSprite, which is
    # set PER INSTANCE in build_title_menu. Auto-sized, so the per-instance brush decides the width -
    # PLAY and SETTINGS are different lengths and the slot cannot know which row it is on.
    word = make_image(tree, "WordSprite")
    slot_on_canvas(canvas, word, anchors(0.0, 0.5), (ROW_PAD_X, 0.0, 0.0, 0.0),
                   alignment=(0.0, 0.5), auto_size=True, z_order=4)

    # CENTRED ON THE PLATE, as the kit sets every label (UI QA F2). It was left-aligned 30 px in, the
    # one kit button in the game whose word did not sit in the middle of it. The authored slot is the
    # kit's case; UTraceMenuRow::PlaceLabel moves the word to the leading edge (ROW_PAD_X in, the same
    # TraceMenuStyle::RowPadX) on a row that carries a right-hand readout - JOIN's address, the
    # DIFFICULTY value chip - where a centred word would run into it. The Canvas twin,
    # ATraceMenuHUD::DrawRow, makes the same call from the same row view.
    label = make_text(tree, "LabelText", "PLAY", FS_ROW_LABEL, INK, unreal.TextJustify.CENTER)
    slot_on_canvas(canvas, label, anchors(0.5, 0.5), (0.0, 0.0, 0.0, 0.0),
                   alignment=(0.5, 0.5), auto_size=True, z_order=4)

    status = make_text(tree, "StatusText", "HOST  0.0.0.0:7777", FS_ROW_STATUS, INK_DIM,
                       unreal.TextJustify.RIGHT)
    slot_on_canvas(canvas, status, anchors(1.0, 0.5), (-ROW_PAD_X, 0.0, 0.0, 0.0),
                   alignment=(1.0, 0.5), auto_size=True, z_order=4)

    # ---- The value readout, in the artist's chip ---------------------------------------------------
    #
    # Value and its two arrows travel together, right-aligned, so the arrows sit a fixed distance from
    # the value however wide the value happens to be - which is what the Canvas path spends three width
    # measurements achieving. The chip is an overlay UNDER them, so it takes whatever size they need.
    value_group = mk(tree, unreal.Overlay, "ValueGroup")
    slot_on_canvas(canvas, value_group, anchors(1.0, 0.5), (-8.0, 0.0, 0.0, 0.0),
                   alignment=(1.0, 0.5), auto_size=True, z_order=3)

    chip_h = 34.0
    chip = make_image(tree, "ValueChip", image_brush=brush(
        "T_MenuValueBox",
        frame_image_size(chip_h, VAL_GLOW, VAL_PLATE_H, VAL_SPRITE_W, VAL_SPRITE_H),
        frame_margin(VAL_CAP, VAL_SPRITE_W, VAL_SPRITE_H)))
    overlay_slot(value_group, chip)

    value_box = mk(tree, unreal.HorizontalBox, "ValueBox")
    overlay_slot(value_group, value_box, padding=(16.0, 2.0, 16.0, 2.0),
                 h_align=unreal.HorizontalAlignment.H_ALIGN_CENTER,
                 v_align=unreal.VerticalAlignment.V_ALIGN_CENTER)

    hbox_slot(value_box, make_text(tree, "LeftArrowText", "<", FS_ROW_ARROW, INK),
              padding=(0.0, 0.0, 14.0, 0.0))
    hbox_slot(value_box, make_text(tree, "ValueText", "NORMAL", FS_ROW_VALUE, INK,
                                   unreal.TextJustify.RIGHT))
    hbox_slot(value_box, make_text(tree, "RightArrowText", ">", FS_ROW_ARROW, INK),
              padding=(14.0, 0.0, 0.0, 0.0))

    # ---- The three states, as brushes the ASSET carries -------------------------------------------
    #
    # On the Blueprint's class defaults, so every one of the six instances gets them and a designer can
    # override any single row in the editor without touching C++.
    defaults = unreal.get_default_object(asset.generated_class()) if asset.generated_class() else None
    if defaults is None:
        # The class does not exist until the first compile. Compile, then stamp.
        unreal.BlueprintEditorLibrary.compile_blueprint(asset)
        defaults = unreal.get_default_object(asset.generated_class()) if asset.generated_class() else None
    if defaults is None:
        fail("WBP_MenuRow: no generated class to put the plate brushes on.")
    else:
        set_prop(defaults, "plate_default_brush", "PlateDefaultBrush",
                 button_brush("T_MenuBtn_Default", ROW_HEIGHT))
        set_prop(defaults, "plate_hover_brush", "PlateHoverBrush",
                 button_brush("T_MenuBtn_Hover", ROW_HEIGHT))
        set_prop(defaults, "plate_disabled_brush", "PlateDisabledBrush",
                 button_brush("T_MenuBtn_Disabled", ROW_HEIGHT))

    finish(asset, "WBP_MenuRow")
    return asset


# =============================================================================
# WBP_TitleMenu
# =============================================================================

# Which rows draw the artist's own lettering, because the sheet contains the word.
# Everything else is typed in the stand-in font - see MENU_FONT_ASSET above and
# Source/Trace/UI/Widgets/Menu/TraceMenuArtStyle.h.
ROW_WORD_SPRITES = {
    "RowPlay": "T_MenuWord_Play",
    "RowSettings": "T_MenuWord_Settings",
}

def build_title_menu(row_asset):
    parent = resolve_parent_class("TraceTitleMenuWidget")
    if parent is None:
        return None
    if row_asset is None:
        fail("WBP_TitleMenu: no WBP_MenuRow to place. Refusing to author a title screen with no rows.")
        return None

    row_class = row_asset.generated_class()
    if row_class is None:
        fail("WBP_MenuRow has no generated class - it did not compile.")
        return None

    asset, tree, root = open_widget_blueprint("WBP_TitleMenu", parent, unreal.CanvasPanel, "RootCanvas")
    if asset is None:
        return None

    full = anchors(0.0, 0.0, 1.0, 1.0)

    # ---- 0. Black. The brief's words, not an interpretation ---------------------------------------
    slot_on_canvas(root, make_image(tree, "Backdrop", color=BLACK), full, (0, 0, 0, 0), z_order=0)

    # ---- 1. The swoosh, then the wordmark over it -------------------------------------------------
    #
    # In the artist's own layout the sweep passes under and behind the title, which is what these two
    # rectangles reproduce. The swoosh is held at 78% so it stays a backdrop rather than competing
    # with the six things on this screen a player has to read.
    swoosh_w = 940.0
    swoosh_h = swoosh_w * (3856.0 / 18600.0)
    swoosh = make_image(tree, "SwooshImage", image_brush=brush("T_MenuSwoosh"))
    swoosh.set_editor_property("render_opacity", 0.78)
    slot_on_canvas(root, swoosh, anchors(0.5, 0.0), (0.0, 150.0, swoosh_w, swoosh_h),
                   alignment=(0.5, 0.0), z_order=1)

    mark_w = 660.0
    mark_h = mark_w * (2392.0 / 14480.0)
    slot_on_canvas(root, make_image(tree, "Wordmark", image_brush=brush("T_TraceWordmark")),
                   anchors(0.5, 0.0), (0.0, 72.0, mark_w, mark_h), alignment=(0.5, 0.0), z_order=2)

    tagline = make_text(tree, "TaglineText",
                        "5 V 5    -    ONE CORE    -    DASH THE TRAIL TO KILL THE CARRIER",
                        FS_TAGLINE, INK_DIM)
    slot_on_canvas(root, tagline, anchors(0.5, 0.0), (0.0, 359.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=3)

    # ---- 2. Address chip, wearing the artist's plate ----------------------------------------------
    #
    # It used to be two nested borders faking an outline, because a plain colour brush has no outline
    # in UMG and there was no texture to 9-slice. There is one now, so the chip is literally the same
    # button frame the rows are, at a smaller height - which is the whole reason the frame is sliced.
    chip_h = 34.0
    chip_inset = frame_glow_inset(chip_h, BTN_GLOW, BTN_PLATE_H)
    chip = make_border(tree, "AddressChip", C(1, 1, 1, 1),
                       (20.0, 5.0 + chip_inset, 20.0, 5.0 + chip_inset),
                       background=button_brush("T_MenuBtn_Default", chip_h))
    slot_on_canvas(root, chip, anchors(0.5, 0.0), (0.0, 392.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=3)

    chip_box = mk(tree, unreal.HorizontalBox, "AddressBox")
    chip.add_child(chip_box)
    hbox_slot(chip_box, make_text(tree, "AddressCaptionText", "YOUR ADDRESS", FS_CHIP_CAP, INK_DIM),
              padding=(0.0, 0.0, 14.0, 0.0))
    hbox_slot(chip_box, make_text(tree, "AddressValueText", "0.0.0.0:7777", FS_CHIP_VALUE, INK))

    warning = make_text(tree, "PortWarningText",
                        "PORT 7777 IS BUSY ON THIS MACHINE - THE HUD WILL SHOW THE REAL PORT IN-GAME",
                        FS_WARNING, AMBER)
    warning.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)
    slot_on_canvas(root, warning, anchors(0.5, 0.0), (0.0, 436.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=3)

    # ---- 3. The console ----------------------------------------------------------------------------
    #
    # Now an invisible frame: on black, with every row wearing its own plate, a panel behind them would
    # be a grey box around six buttons. It stays in the tree because
    # UTraceTitleMenuWidget::SyncConsoleWidth drives its WIDTH every frame, which is what keeps the
    # rows where the Canvas layout maths says they are, and `Trace.UI.VerifyMenu` measures that.
    console = make_border(tree, "ConsolePanel", CLEAR, (PANEL_PAD_X, PANEL_PAD_T, PANEL_PAD_X, PANEL_PAD_B))
    slot_on_canvas(root, console, anchors(0.5, 0.0),
                   (0.0, PANEL_TOP_Y, PANEL_WIDTH, PANEL_HEIGHT),
                   alignment=(0.5, 0.0), z_order=4)

    console_content = mk(tree, unreal.VerticalBox, "ConsoleContent")
    console.add_child(console_content)

    row_box = mk(tree, unreal.VerticalBox, "RowBox")
    vbox_slot(console_content, row_box)

    for index, row_name in enumerate(ROW_NAMES):
        row_widget = unreal.new_object(row_class, tree, row_name)

        # PER INSTANCE: the two rows whose word the sheet actually contains draw it. The other four
        # cannot - JOIN, PRACTICE, DIFFICULTY and QUIT are not on the sheet and no PNG
        # can supply them - so they type their label in the stand-in font.
        sprite_name = ROW_WORD_SPRITES.get(row_name)
        if sprite_name is not None and Textures.get(sprite_name) is not None:
            set_prop(row_widget, "word_brush", "WordBrush", word_brush(sprite_name, 26.0))
            set_prop(row_widget, "label_is_sprite", "bLabelIsSprite", True)

        # The gap is BELOW every row but the last, which is what turns a 60-tall
        # row into the 71-pixel pitch the Canvas renderer walks down.
        gap = 0.0 if index == len(ROW_NAMES) - 1 else ROW_GAP
        vbox_slot(row_box, row_widget, padding=(0.0, 0.0, 0.0, gap))

    blurb = make_text(tree, "BlurbText", "HOSTS A GAME.", FS_BLURB, INK_DIM)
    vbox_slot(console_content, blurb, padding=(0.0, 22.0, 0.0, 0.0),
              h_align=unreal.HorizontalAlignment.H_ALIGN_CENTER)

    # ---- 4. Footer ---------------------------------------------------------------------------------
    #
    # No dark strip any more: the background IS dark. One hint line, and an empty block above it.
    #
    # D30 - THE KEY LEGEND IS GONE. FooterKeysText used to be authored with
    #     "W / S  OR  ARROWS   MOVE   A / D   CHANGE   ENTER   SELECT   ESC   QUIT"
    # and the owner asked for that line off the screen. It is authored EMPTY rather than dropped from
    # the tree: FooterKeysText is a required UPROPERTY(meta=(BindWidget)) on UTraceTitleMenuWidget
    # (see TITLE_BIND_NAMES below, which this script verifies against the reloaded asset), so deleting
    # it here would produce an asset that fails to bind at runtime. Its slot still anchors the footer
    # stack - UTraceTitleMenuWidget::PlaceFooterBelowBlurb puts the hint one FooterLineGap under it.
    #
    # The authored string is only what a designer sees in the editor preview: ATraceMenuHUD::ApplyView
    # overwrites it from FTraceTitleMenuView::FooterKeys every frame, and that is empty too. Both
    # sides are changed so the asset does not go on carrying a string the game will never print.
    footer_keys = make_text(tree, "FooterKeysText", "", FS_FOOTER, INK_DIM)
    slot_on_canvas(root, footer_keys, anchors(0.5, 0.0), (0.0, 958.4, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=6)

    footer_hint = make_text(
        tree, "FooterHintText",
        "PLAY ALSO HOSTS - EVERY MATCH IS JOINABLE   -   OTHERS PICK JOIN AND TYPE YOUR ADDRESS ABOVE",
        FS_FOOTER, INK_FAINT)
    slot_on_canvas(root, footer_hint, anchors(0.5, 0.0), (0.0, 982.4, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=6)

    # ---- 5. The cursor -----------------------------------------------------------------------------
    #
    # The artist's pointer. Its ANCHOR is what moves (see UTraceTitleMenuWidget::ApplyView), so the
    # offsets here are only its size, and the alignment is (0,0) because the sprite's tip is its
    # top-left corner - that is the pixel the player believes they are pointing with.
    cursor_h = 34.0
    cursor_w = cursor_h * (512.0 / 696.0)
    slot_on_canvas(root, make_image(tree, "MenuCursor", image_brush=brush("T_MenuCursor")),
                   anchors(0.5, 0.5), (0.0, 0.0, cursor_w, cursor_h), alignment=(0.0, 0.0), z_order=8)

    # ---- 6. Travel overlay -------------------------------------------------------------------------
    travel = make_border(tree, "TravelOverlay", SCRIM, (0.0, 0.0, 0.0, 0.0))
    travel.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)
    slot_on_canvas(root, travel, full, (0, 0, 0, 0), z_order=9)

    travel_canvas = mk(tree, unreal.CanvasPanel, "TravelCanvas")
    travel.add_child(travel_canvas)

    travel_mark_w = 420.0
    travel_mark = make_image(tree, "TravelWordmark", image_brush=brush("T_TraceWordmark"))
    travel_mark.set_editor_property("render_opacity", 0.55)
    slot_on_canvas(travel_canvas, travel_mark, anchors(0.5, 0.36),
                   (0.0, 0.0, travel_mark_w, travel_mark_w * (2392.0 / 14480.0)),
                   alignment=(0.5, 0.5), z_order=0)

    travel_caption = make_text(tree, "TravelCaptionText", "ENTERING THE ARENA", FS_TRAVEL_CAP, INK)
    slot_on_canvas(travel_canvas, travel_caption, anchors(0.5, 0.55), (0.0, 0.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=1)

    travel_hint = make_text(
        tree, "TravelHintText",
        "THIS CAN TAKE A FEW SECONDS.  A FAILURE WILL BE REPORTED, NOT SWALLOWED.",
        FS_TRAVEL_HINT, INK_DIM)
    slot_on_canvas(travel_canvas, travel_hint, anchors(0.5, 0.55), (0.0, 34.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=1)

    # ---- 7. Failure banner, on the handmade kit --------------------------------------------------
    #
    # A minute is a long time for a banner and it is deliberate: the failure that matters happens while
    # the player is looking at a DIFFERENT screen.
    #
    # ON THE KIT (P09): the artist's HOVER plate - the amber ring is the kit's "this one is about you" -
    # centred, with the one line in white. It used to be a full-width brown bar with pre-kit amber rails
    # and amber type, pinned at y 54 straight across the TRACE wordmark. It now sits UNDER the menu, in
    # the bottom stack UTraceTitleMenuWidget::PlaceFooterBelowBlurb flows below the blurb, clear of the
    # wordmark, the swoosh, the address chip and the rows; the authored y below is only the first-frame
    # fallback. ATraceMenuHUD::DrawFailureBanner draws the same plate at the same place on the Canvas.
    banner_inset = frame_glow_inset(BANNER_PLATE_H, BTN_GLOW, BTN_PLATE_H)
    banner = make_border(tree, "FailureBanner", C(1, 1, 1, 1),
                         (BANNER_PAD_X + banner_inset, BANNER_PAD_Y + banner_inset,
                          BANNER_PAD_X + banner_inset, BANNER_PAD_Y + banner_inset),
                         background=button_brush("T_MenuBtn_Hover", BANNER_PLATE_H))
    banner.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)
    slot_on_canvas(root, banner, anchors(0.5, 0.0), (0.0, 930.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=10)

    banner_box = mk(tree, unreal.VerticalBox, "FailureBox")
    banner.add_child(banner_box)
    vbox_slot(banner_box, make_text(tree, "FailureHeadlineText", "NO RESPONSE FROM HOST",
                                    FS_BANNER_HEAD, INK),
              h_align=unreal.HorizontalAlignment.H_ALIGN_CENTER)
    # Bound but never shown: the engine's own error text is log-only (TraceNet::GetLastFailure).
    detail = make_text(tree, "FailureDetailText", " ", FS_BANNER_BODY, INK_DIM)
    detail.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)
    vbox_slot(banner_box, detail, h_align=unreal.HorizontalAlignment.H_ALIGN_CENTER)

    finish(asset, "WBP_TitleMenu")
    return asset


# =============================================================================
# Verification - the assets are only worth anything if the names line up
# =============================================================================

# Mirrors the UPROPERTY(meta=(BindWidget)) members of UTraceMenuRow and
# UTraceTitleMenuWidget. Deliberately duplicated here so that this script fails
# on a rename rather than shipping an asset whose Blueprint compile error nobody
# reads.
ROW_BIND_NAMES = [
    "PlateSprite", "WordSprite", "ChevronSprite", "ValueChip",
    "LabelText", "StatusText", "ValueText", "LeftArrowText", "RightArrowText",
]

TITLE_BIND_NAMES = [
    "Backdrop", "SwooshImage", "Wordmark", "TaglineText", "AddressChip",
    "AddressCaptionText", "AddressValueText", "PortWarningText", "ConsolePanel",
    # Must stay in step with the BindWidget properties on UTraceTitleMenuWidget AND with
    # ETraceMenuRow. RowPractice is spec v19 section 2; RowMode (SCORING MODE) was removed with
    # the endzone ruleset, in all three places at once.
    "RowPlay", "RowJoin", "RowPractice", "RowDifficulty", "RowSettings", "RowQuit",
    "BlurbText", "FooterKeysText", "FooterHintText", "MenuCursor",
    "TravelOverlay", "TravelWordmark", "TravelCaptionText", "TravelHintText",
    "FailureBanner", "FailureHeadlineText", "FailureDetailText",
]


def verify(name, required_names, root_name):
    reloaded = unreal.EditorAssetLibrary.load_asset("{0}/{1}".format(MENU_DIR, name))
    if reloaded is None:
        fail("{0}: did not reload from disk".format(name))
        return

    tree = unreal.find_object(reloaded, "WidgetTree")
    root = unreal.find_object(tree, root_name) if tree is not None else None
    if root is None:
        fail("{0}: reloaded with no '{1}' root".format(name, root_name))
        return

    present = set()

    def walk(widget):
        if widget is None:
            return
        present.add(widget.get_name())
        if isinstance(widget, unreal.PanelWidget):
            for child in widget.get_all_children():
                walk(child)

    walk(root)

    missing = [n for n in required_names if n not in present]
    if missing:
        fail("{0}: {1} BindWidget name(s) missing from the tree: {2}".format(
            name, len(missing), ", ".join(missing)))
    else:
        log("  {0}: all {1} BindWidget names present ({2} widgets in the tree)".format(
            name, len(required_names), len(present)))

    # THE LAUNCH-STALL CHECK. Any TraceRetired_* left inside the asset is one more
    # dead widget the Blueprint re-resolves on every single load - which is what
    # cost 12 to 47 seconds of black screen before this pass. It must be zero.
    orphans = [n for n in present if n.startswith("TraceRetired_")]
    package_orphans = 0
    if tree is not None:
        for candidate in range(1, 200):
            if unreal.find_object(tree, "TraceRetired_{0}".format(candidate)) is not None:
                package_orphans += 1
    if orphans or package_orphans:
        fail("{0}: {1} retired widget(s) still in the tree and {2} still in the package. That is "
             "the launch stall; retire_descendants did not move them to the transient package."
             .format(name, len(orphans), package_orphans))
    else:
        log("  {0}: no retired widgets left in the package (this is the ~40 s launch stall's cause)".format(name))

    if reloaded.generated_class() is None:
        fail("{0}: no generated class - the Blueprint did not compile".format(name))


# =============================================================================

def main():
    log("=" * 70)
    log("Trace - spec v19 section 5: the title screen, built from the artist's sheet")
    log("Sprites: {0}      Widgets: {1}".format(ART_DIR, MENU_DIR))
    log("=" * 70)

    if IMPORT_ONLY:
        log("TRACE_IMPORT_ONLY={0}: re-importing only those sprites; no font, no widget.".format(
            ",".join(IMPORT_ONLY)))
        import_sprites()
        for name in IMPORT_ONLY:
            if Textures.get(name) is None:
                fail("{0} did not re-import.".format(name))
        if Failures:
            for message in Failures:
                unreal.log_error("[MenuWidgets] FAILED: {0}".format(message))
            unreal.log_error("[MenuWidgets] VERDICT: {0} failure(s) re-importing.".format(len(Failures)))
            return 1
        log("VERDICT: re-imported {0} sprite(s).".format(len(IMPORT_ONLY)))
        return 0

    global MENU_FONT

    log("Importing the font named by TraceMenuArtStyle::MenuFontSourceFile ({0}):".format(
        MENU_FONT_SOURCE_FILE))
    MENU_FONT = import_menu_font()

    if MENU_FONT is None:
        fail("no font loaded at all - neither {0} nor the {1} fallback. Every text block would take "
             "Slate's last-resort face, and the whole point of naming the font in one place is that "
             "it is the one thing we are substituting."
             .format(MENU_FONT_ASSET, MENU_FONT_FALLBACK_ASSET))
        return 1

    # Whether '<typeface>' actually exists in that composite font is checked at RUNTIME, in
    # TraceMenuArtStyle.cpp, and reported by `Trace.UI.VerifyMenuArt`. It is not checked here because
    # FCompositeFont's DefaultTypeface is not script-exposed in UE 5.8 - asking for it throws. One
    # check in the place that can actually do it beats two, one of which is a guess.
    log("Font for every non-sprite label: {0} '{1}'{2}. A SUBSTITUTE: the art arrived as a PNG and a "
        "PNG cannot carry a typeface. Only PLAY and SETTINGS draw the artist's real letterforms."
        .format(MENU_FONT_ASSET if not MENU_FONT_IS_FALLBACK else MENU_FONT_FALLBACK_ASSET,
                live_typeface(),
                " (THE FALLBACK - the intended font did not import)" if MENU_FONT_IS_FALLBACK else ""))

    log("Importing the sliced sheet:")
    import_sprites()

    placed = [name for name, is_placed in SPRITES if is_placed]
    got = [name for name in placed if Textures.get(name) is not None]
    if len(got) != len(placed):
        fail("only {0} of the {1} sprites the title screen places are importable. The screen would "
             "draw white boxes where the missing ones are.".format(len(got), len(placed)))

    if FS_ROW_LABEL is None:
        fail("the row label's size could not be derived from the C++ headers (see the failure above). "
             "Refusing to author the rows at a guessed size.")
        for message in Failures:
            unreal.log_error("[MenuWidgets] FAILED: {0}".format(message))
        return 1
    log("Row label size {0} = TraceMenuKit::LabelSize({1:.0f}) in Light, parsed from the C++ headers."
        .format(FS_ROW_LABEL, ROW_HEIGHT))

    row_asset = build_menu_row()
    title_asset = build_title_menu(row_asset)

    log("-" * 70)
    log("Verifying the BindWidget contract against the reloaded assets")
    if row_asset is not None:
        verify("WBP_MenuRow", ROW_BIND_NAMES, "RowRoot")
    if title_asset is not None:
        verify("WBP_TitleMenu", TITLE_BIND_NAMES, "RootCanvas")

    log("Retired {0} widget(s) this run; {1} of them went to the transient package.".format(
        RetiredCount[0], RetiredToTransient[0]))

    log("=" * 70)
    if Failures:
        for message in Failures:
            unreal.log_error("[MenuWidgets] FAILED: {0}".format(message))
        unreal.log_error(
            "[MenuWidgets] VERDICT: {0} failure(s). The assets are NOT trustworthy; the game will "
            "fall back to the Canvas title screen and say so in the log.".format(len(Failures)))
        return 1

    log("VERDICT: {0} sprites imported to {1}; WBP_MenuRow and WBP_TitleMenu written and verified "
        "under {2}.".format(len(Textures), ART_DIR, MENU_DIR))
    log("Next, in the game's console:")
    log("  Trace.UI.UseUMG 1")
    log("  Trace.UI.VerifyMenuArt          every sprite resolved, and what font the type is in")
    log("  Trace.UI.VerifyMenuArt redarm   must FAIL - that is what proves the check can")
    log("  Trace.UI.VerifyMenu             the rows are still where the mouse expects them")
    log("=" * 70)
    return 0


EXIT_CODE = main()
if EXIT_CODE != 0:
    raise SystemExit(EXIT_CODE)
