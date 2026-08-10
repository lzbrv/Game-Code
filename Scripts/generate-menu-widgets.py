# =============================================================================
# Trace - generate-menu-widgets.py
#
# Writes the TITLE SCREEN's widget tree out of C++ and into real .uassets at
# /Game/Trace/UI/Menu:
#
#     WBP_MenuRow     one menu row     (parent class UTraceMenuRow)
#     WBP_TitleMenu   the whole screen (parent class UTraceTitleMenuWidget)
#
# -----------------------------------------------------------------------------
# WHY THIS EXISTS  (spec v17 section 4)
# -----------------------------------------------------------------------------
# ATraceMenuHUD::DrawHUD draws the title screen with AHUD::DrawRect / DrawText /
# DrawLine - about six hundred lines of pixel arithmetic that works, that nobody
# can open, and that a designer cannot touch without a C++ build. This script
# authors the same screen as a widget tree so that the LAYOUT and the STYLING
# live in an asset a person can open in the editor.
#
# THE ARCHITECTURE IS NOT NEGOTIABLE, and this script is written to enforce it:
#   * All behaviour stays in C++ (UTraceTitleMenuWidget, UTraceMenuRow). This
#     script authors NO graph, NO event, NO binding - only widgets and styling.
#   * Every widget whose name matches a UPROPERTY(meta=(BindWidget)) in those
#     headers is REQUIRED. Rename one in the editor and the Blueprint fails to
#     compile with "A required widget binding <name> ... was not found". That
#     loud failure is the safety net; it is why the names below are fussy.
#
# THIS IS A REORGANISATION, NOT A REDESIGN. Every number below is the same
# number the Canvas path uses, expressed in the same reference-pixel space (a
# 1080-tall design; see Source/Trace/UI/Widgets/Menu/TraceMenuPalette.h).
# `Trace.UI.VerifyMenu` measures the result against the Canvas layout maths and
# reports the worst row-rectangle error in pixels.
#
# -----------------------------------------------------------------------------
# WHAT IS AND IS NOT AUTHORITATIVE  (read before editing an asset by hand)
# -----------------------------------------------------------------------------
# AUTHORITATIVE, and genuinely used by the running game:
#   * the widget TREE and its STYLING - fonts, colours, positions, padding, and
#     every EditAnywhere property on UTraceMenuCanvasArt / UTraceStrokeText /
#     UTraceMenuRow, including `bOpaqueLines`. Change them in the editor and the
#     game looks different on the next launch. That is the point of this step.
# NOT AUTHORITATIVE:
#   * the TEXT in every text block. It is placeholder, overwritten on the first
#     frame by UTraceTitleMenuWidget::ApplyView from state ATraceMenuHUD owns.
#     Typing a new label into WBP_TitleMenu changes the editor preview and
#     nothing at all in the game.
#   * the WIDTH of ConsolePanel. UTraceTitleMenuWidget::SyncConsoleWidth
#     reproduces the shipped min(viewport * 0.52, 720) clamp every frame.
#
# RE-RUNNING IS SAFE and rewrites both assets in place. It does NOT delete and
# recreate them - delete_asset() + create_asset() in one editor session returns
# None for everything that follows (this bit the input-asset generator in step
# 6), and recreating WBP_MenuRow would break WBP_TitleMenu's hard reference to
# its generated class. Instead the existing ROOT widget is kept and everything
# under it is rebuilt.
#
# -----------------------------------------------------------------------------
# THREE EDITOR-PYTHON FACTS THIS SCRIPT IS BUILT AROUND
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
#     `bOpaqueLines`, `bDrawRule`, `DefaultRootWidget`. UCanvasPanelSlot's
#     Anchors/Offsets/Alignment are not properties at all any more - they live
#     inside FAnchorData - so the BlueprintCallable setters are used instead.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
#     "$UE" "$PWD/Trace.uproject" \
#         -run=pythonscript -script="$PWD/Scripts/generate-menu-widgets.py" \
#         -unattended -nosplash -NullRHI -nosound \
#         -log -abslog="$PWD/Saved/Logs/trace-generate-menu-widgets.log"
#
# -NullRHI is safe: widget blueprints compile no shaders. Or paste it into the
# editor's Output Log Python prompt:
#
#     exec(open("/path/to/Scripts/generate-menu-widgets.py").read())
#
# You must have built the game module first - the parent classes come out of it,
# and the script fails immediately rather than authoring an asset with the wrong
# parent.
#
# OPTIONS (environment variables; the pythonscript commandlet has no clean way
# to pass argv through -script=)
#   TRACE_MENU_UI_DIR   package dir. Default /Game/Trace/UI/Menu
# =============================================================================

import os
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

Failures = []


def log(message):
    unreal.log("[MenuWidgets] {0}".format(message))


def fail(message):
    Failures.append(message)
    unreal.log_error("[MenuWidgets] {0}".format(message))


# =============================================================================
# The palette. Mirrors TraceMenuStyle in
# Source/Trace/UI/Widgets/Menu/TraceMenuPalette.h, which is where the Canvas
# renderer reads its colours from. Nothing enforces the tie automatically; if
# you change a colour there, change it here and re-run.
# =============================================================================

def C(r, g, b, a=1.0):
    return unreal.LinearColor(r, g, b, a)


VOID      = C(0.006, 0.011, 0.022, 1.00)
CYAN      = C(0.16,  0.88,  1.00,  1.00)
CYAN_DEEP = C(0.04,  0.34,  0.48,  1.00)
AMBER     = C(1.00,  0.46,  0.08,  1.00)
INK       = C(0.90,  0.97,  1.00,  1.00)
INK_DIM   = C(0.42,  0.58,  0.66,  1.00)
PANEL     = C(0.004, 0.014, 0.026, 0.94)
FOOTER    = C(0.004, 0.014, 0.026, 0.92)
CHIP_FILL = C(0.004, 0.014, 0.026, 0.90)
PLATE     = C(0.0,   0.02,  0.04,  0.55)
BANNER    = C(0.18,  0.05,  0.00,  0.90)
SCRIM     = C(0.0,   0.0,   0.0,   0.72)

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
PANEL_HEIGHT = 495.0
EDGE         = 1.2

# Font sizes, in the 1080 design space.
#
# NOT GUESSED. The Canvas renderer scales the engine's BITMAP fonts by
# (UIScale * a per-element factor); Slate renders Roboto as an outline font at a
# point size. There is no formula between the two, so these were set against a
# 1280x720 capture of the Canvas title screen and then re-measured against a
# capture of this one. The first pass ran two to four points heavy across the
# board, which pushed the address chip's auto-sized box down into the top edge
# of the console panel - the sort of collision that only appears on screen.
#
# If you change one, re-capture both renderers and look at them side by side;
# `Trace.UI.VerifyMenu` measures the ROW RECTANGLES, which fonts do not move.
FS_ROW_LABEL   = 25
FS_ROW_STATUS  = 13
FS_ROW_ARROW   = 20
FS_TAGLINE     = 15
FS_CHIP_CAP    = 12
FS_CHIP_VALUE  = 18
FS_CHIP_PAD_Y  = 5.0
FS_BLURB       = 15
FS_FOOTER      = 13
FS_BANNER_HEAD = 23
FS_BANNER_BODY = 15
FS_TRAVEL_CAP  = 22
FS_TRAVEL_HINT = 13
FS_WARNING     = 13

ROBOTO = unreal.EditorAssetLibrary.load_asset("/Engine/EngineFonts/Roboto")

UMG_SETTINGS_PATH = "/Script/UMGEditor.Default__UMGEditorProjectSettings"


# =============================================================================
# Widget-tree helpers
# =============================================================================

def mk(tree, cls, name):
    """Create a widget owned by @tree. The NAME is the whole BindWidget contract."""
    return unreal.new_object(cls, tree, name)


def anchors(min_x, min_y, max_x=None, max_y=None):
    if max_x is None:
        max_x = min_x
    if max_y is None:
        max_y = min_y
    return unreal.Anchors(minimum=unreal.Vector2D(min_x, min_y),
                          maximum=unreal.Vector2D(max_x, max_y))


def slot_on_canvas(panel, child, anc, offsets, alignment=(0.0, 0.0),
                   auto_size=False, z_order=0):
    """
    Add @child to a canvas panel.

    UMG's offset convention is worth stating because getting it backwards is
    silent: when the anchor is a POINT (min == max) the offsets read
    (Left, Top, Width, Height); when the anchor SPANS, they read
    (Left, Top, Right, Bottom) as insets from the anchor box.
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
    font.set_editor_property("font_object", ROBOTO)
    font.set_editor_property("typeface_font_name", "Regular")
    font.set_editor_property("size", size)
    block.set_editor_property("font", font)
    block.set_editor_property("color_and_opacity", unreal.SlateColor(specified_color=color))
    block.set_editor_property("justification", justify)
    return block


def make_image(tree, name, color):
    """
    A plain coloured rectangle.

    UImage's default brush has no texture, which Slate renders as solid white -
    so the tint IS the colour. That is the same thing AHUD::DrawRect does, and
    it means these rectangles need no art asset at all.
    """
    image = mk(tree, unreal.Image, name)
    image.set_editor_property("color_and_opacity", color)
    return image


def make_border(tree, name, color, padding):
    brd = mk(tree, unreal.Border, name)
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


# =============================================================================
# Asset creation - see "THREE EDITOR-PYTHON FACTS" in the header
# =============================================================================

def resolve_parent_class(class_name):
    """The C++ class, or None with a loud complaint. Never silently substitutes."""
    parent = getattr(unreal, class_name, None)
    if parent is None:
        fail("unreal.{0} does not exist. The Trace game module is not built, or the class was "
             "renamed. Build with ./Scripts/build.sh and re-run; do NOT let this script author an "
             "asset with the wrong parent class.".format(class_name))
    return parent


def retire_descendants(widget, counter):
    """
    Rename everything under @widget out of the way so a re-run can reuse the
    names without NewObject having to collide with an orphan.

    The root itself is kept: its pointer is the one thing in the tree Python
    cannot set, so losing it would mean losing the asset.
    """
    if not isinstance(widget, unreal.PanelWidget):
        return
    for child in list(widget.get_all_children()):
        retire_descendants(child, counter)
        counter[0] += 1
        try:
            child.rename("TraceRetired_{0}".format(counter[0]))
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

    retire_descendants(root, [0])
    return existing, tree, root


def finish(asset, name):
    unreal.BlueprintEditorLibrary.compile_blueprint(asset)
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset):
        fail("{0}: save failed".format(name))
    else:
        log("{0}: compiled and saved".format(name))


# =============================================================================
# WBP_MenuRow
#
# The root is a SizeBox forcing the 60-reference-pixel row height, so the six
# rows in a vertical box land on the 71-pixel pitch the Canvas renderer uses
# (60 tall + an 11 gap). The row's own geometry is what ATraceMenuHUD hit-tests
# against, so this height is load-bearing, not styling.
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

    # Plate, then the selection wash over it. Both fill the row.
    slot_on_canvas(canvas, make_image(tree, "RowPlate", PLATE), full, (0, 0, 0, 0), z_order=0)
    slot_on_canvas(canvas, make_image(tree, "SelectionWash", C(0.16, 0.88, 1.0, 0.0)),
                   full, (0, 0, 0, 0), z_order=1)

    # Top and bottom rails only - the Canvas row draws no side edges.
    slot_on_canvas(canvas, make_image(tree, "TopEdge", C(0.16, 0.88, 1.0, 0.16)),
                   anchors(0.0, 0.0, 1.0, 0.0), (0, 0, 0, EDGE), z_order=2)
    slot_on_canvas(canvas, make_image(tree, "BottomEdge", C(0.16, 0.88, 1.0, 0.16)),
                   anchors(0.0, 1.0, 1.0, 1.0), (0, -EDGE, 0, 0), z_order=2)

    # The breathing bar on the leading edge, 5 reference pixels wide.
    slot_on_canvas(canvas, make_image(tree, "SelectionBar", C(0.16, 0.88, 1.0, 0.0)),
                   anchors(0.0, 0.0, 0.0, 1.0), (0, 0, 5.0, 0), z_order=3)

    # OUTSIDE the row's left edge, which is why nothing in this tree clips.
    chevron = make_text(tree, "ChevronText", ">", FS_ROW_ARROW, CYAN, unreal.TextJustify.RIGHT)
    slot_on_canvas(canvas, chevron, anchors(0.0, 0.5), (-18.0, 0.0, 0.0, 0.0),
                   alignment=(1.0, 0.5), auto_size=True, z_order=4)

    label = make_text(tree, "LabelText", "PLAY", FS_ROW_LABEL, INK, unreal.TextJustify.LEFT)
    slot_on_canvas(canvas, label, anchors(0.0, 0.5), (ROW_PAD_X, 0.0, 0.0, 0.0),
                   alignment=(0.0, 0.5), auto_size=True, z_order=4)

    status = make_text(tree, "StatusText", "HOST  0.0.0.0:7777", FS_ROW_STATUS, INK_DIM,
                       unreal.TextJustify.RIGHT)
    slot_on_canvas(canvas, status, anchors(1.0, 0.5), (-ROW_PAD_X, 0.0, 0.0, 0.0),
                   alignment=(1.0, 0.5), auto_size=True, z_order=4)

    # Value and its two arrows travel together, right-aligned, so the arrows sit
    # a fixed distance from the value however wide the value happens to be -
    # which is what the Canvas path spends three width measurements achieving.
    value_box = mk(tree, unreal.HorizontalBox, "ValueBox")
    slot_on_canvas(canvas, value_box, anchors(1.0, 0.5), (-8.0, 0.0, 0.0, 0.0),
                   alignment=(1.0, 0.5), auto_size=True, z_order=4)

    hbox_slot(value_box, make_text(tree, "LeftArrowText", "<", FS_ROW_ARROW, CYAN),
              padding=(0.0, 0.0, 14.0, 0.0))
    hbox_slot(value_box, make_text(tree, "ValueText", "NORMAL", FS_ROW_LABEL, CYAN,
                                   unreal.TextJustify.RIGHT))
    hbox_slot(value_box, make_text(tree, "RightArrowText", ">", FS_ROW_ARROW, CYAN),
              padding=(14.0, 0.0, 0.0, 0.0))

    finish(asset, "WBP_MenuRow")
    return asset


# =============================================================================
# WBP_TitleMenu
# =============================================================================

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
    top_span = anchors(0.0, 0.0, 1.0, 0.0)

    # ---- 0. Backdrop: void, horizon glow, grid floor, scanlines -------------
    backdrop = mk(tree, unreal.TraceMenuCanvasArt, "Backdrop")
    backdrop.set_editor_property("kind", unreal.TraceMenuArtKind.BACKDROP)
    backdrop.set_editor_property("void_color", VOID)
    backdrop.set_editor_property("line_color", CYAN)
    backdrop.set_editor_property("glow_color", CYAN_DEEP)
    slot_on_canvas(root, backdrop, full, (0, 0, 0, 0), z_order=0)

    # ---- 1. Wordmark + rule -------------------------------------------------
    wordmark = mk(tree, unreal.TraceStrokeText, "Wordmark")
    wordmark.set_editor_property("text", "TRACE")
    wordmark.set_editor_property("color", CYAN)
    slot_on_canvas(root, wordmark, full, (0, 0, 0, 0), z_order=1)

    tagline = make_text(tree, "TaglineText",
                        "5 V 5    -    ONE CORE    -    DASH THE TRAIL TO KILL THE CARRIER",
                        FS_TAGLINE, INK_DIM)
    slot_on_canvas(root, tagline, anchors(0.5, 0.0), (0.0, 359.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=2)

    # ---- 2. Address chip ----------------------------------------------------
    #
    # Two nested borders: the outer one IS the 1.2px edge (its padding is the
    # rail thickness), the inner one is the fill. A plain colour brush has no
    # outline in UMG, and a 9-slice would need a texture this project does not
    # have. The inner padding is (18 - edge) so the content lands exactly 18
    # reference pixels in, which is where the Canvas renderer puts it.
    chip_edge = make_border(tree, "AddressChip", C(0.16, 0.88, 1.0, 0.45),
                            (EDGE, EDGE, EDGE, EDGE))
    slot_on_canvas(root, chip_edge, anchors(0.5, 0.0), (0.0, 392.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=2)

    chip_fill = make_border(tree, "AddressChipFill", CHIP_FILL,
                            (18.0 - EDGE, FS_CHIP_PAD_Y - EDGE, 18.0 - EDGE, FS_CHIP_PAD_Y - EDGE))
    chip_edge.add_child(chip_fill)

    chip_box = mk(tree, unreal.HorizontalBox, "AddressBox")
    chip_fill.add_child(chip_box)
    hbox_slot(chip_box, make_text(tree, "AddressCaptionText", "YOUR ADDRESS", FS_CHIP_CAP, INK_DIM),
              padding=(0.0, 0.0, 14.0, 0.0))
    hbox_slot(chip_box, make_text(tree, "AddressValueText", "0.0.0.0:7777", FS_CHIP_VALUE, CYAN))

    warning = make_text(tree, "PortWarningText",
                        "PORT 7777 IS BUSY ON THIS MACHINE - THE HUD WILL SHOW THE REAL PORT IN-GAME",
                        FS_WARNING, AMBER)
    warning.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)
    slot_on_canvas(root, warning, anchors(0.5, 0.0), (0.0, 436.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=2)

    # ---- 3. The console -----------------------------------------------------
    console = make_border(tree, "ConsolePanel", C(0.16, 0.88, 1.0, 0.28),
                          (EDGE, EDGE, EDGE, EDGE))
    slot_on_canvas(root, console, anchors(0.5, 0.0),
                   (0.0, PANEL_TOP_Y, PANEL_WIDTH, PANEL_HEIGHT),
                   alignment=(0.5, 0.0), z_order=3)

    console_fill = make_border(tree, "ConsoleFill", PANEL,
                               (PANEL_PAD_X - EDGE, PANEL_PAD_T - EDGE,
                                PANEL_PAD_X - EDGE, PANEL_PAD_B - EDGE))
    console.add_child(console_fill)

    console_content = mk(tree, unreal.VerticalBox, "ConsoleContent")
    console_fill.add_child(console_content)

    row_box = mk(tree, unreal.VerticalBox, "RowBox")
    vbox_slot(console_content, row_box)

    row_names = ["RowPlay", "RowJoin", "RowDifficulty", "RowMode", "RowSettings", "RowQuit"]
    for index, row_name in enumerate(row_names):
        row_widget = unreal.new_object(row_class, tree, row_name)
        # The gap is BELOW every row but the last, which is what turns a 60-tall
        # row into the 71-pixel pitch the Canvas renderer walks down.
        gap = 0.0 if index == len(row_names) - 1 else ROW_GAP
        vbox_slot(row_box, row_widget, padding=(0.0, 0.0, 0.0, gap))

    blurb = make_text(tree, "BlurbText", "HOSTS A GAME.", FS_BLURB, INK_DIM)
    vbox_slot(console_content, blurb, padding=(0.0, 22.0, 0.0, 0.0),
              h_align=unreal.HorizontalAlignment.H_ALIGN_CENTER)

    # ---- 4. Footer ----------------------------------------------------------
    #
    # The grid runs all the way to the bottom edge, so the key hints get their
    # own dark strip. Same reasoning as the console panel: legibility beats
    # atmosphere every time.
    footer_band = make_image(tree, "FooterBand", FOOTER)
    slot_on_canvas(root, footer_band, full, (0.0, 937.4, 0.0, 0.0), z_order=4)

    footer_rail = make_image(tree, "FooterRail", C(0.16, 0.88, 1.0, 0.24))
    slot_on_canvas(root, footer_rail, top_span, (0.0, 936.4, 0.0, 1.0), z_order=5)

    footer_keys = make_text(
        tree, "FooterKeysText",
        "W / S  OR  ARROWS   MOVE          A / D   CHANGE          ENTER   SELECT          ESC   QUIT",
        FS_FOOTER, INK_DIM)
    slot_on_canvas(root, footer_keys, anchors(0.5, 0.0), (0.0, 958.4, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=6)

    footer_hint = make_text(
        tree, "FooterHintText",
        "PLAY ALSO HOSTS - EVERY MATCH IS JOINABLE   -   OTHERS PICK JOIN AND TYPE YOUR ADDRESS ABOVE",
        FS_FOOTER, C(0.42, 0.58, 0.66, 0.6))
    slot_on_canvas(root, footer_hint, anchors(0.5, 0.0), (0.0, 982.4, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=6)

    # ---- 5. Bezel, ABOVE the footer -----------------------------------------
    #
    # Not decoration order: the footer's dark strip runs to the bottom edge and
    # would otherwise swallow the frame's bottom rail and two of its corner
    # ticks. Measured on the Canvas renderer; reproduced here by z-order.
    bezel = mk(tree, unreal.TraceMenuCanvasArt, "Bezel")
    bezel.set_editor_property("kind", unreal.TraceMenuArtKind.BEZEL)
    bezel.set_editor_property("line_color", CYAN)
    slot_on_canvas(root, bezel, full, (0, 0, 0, 0), z_order=7)

    cursor = mk(tree, unreal.TraceMenuCanvasArt, "MenuCursor")
    cursor.set_editor_property("kind", unreal.TraceMenuArtKind.CURSOR)
    cursor.set_editor_property("line_color", CYAN)
    slot_on_canvas(root, cursor, full, (0, 0, 0, 0), z_order=8)

    # ---- 6. Travel overlay --------------------------------------------------
    travel = make_border(tree, "TravelOverlay", SCRIM, (0.0, 0.0, 0.0, 0.0))
    travel.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)
    slot_on_canvas(root, travel, full, (0, 0, 0, 0), z_order=9)

    travel_canvas = mk(tree, unreal.CanvasPanel, "TravelCanvas")
    travel.add_child(travel_canvas)

    travel_mark = mk(tree, unreal.TraceStrokeText, "TravelWordmark")
    travel_mark.set_editor_property("text", "TRACE")
    travel_mark.set_editor_property("color", C(0.16, 0.88, 1.0, 0.55))
    travel_mark.set_editor_property("cap_height_fraction", 0.10)
    travel_mark.set_editor_property("top_fraction", 0.36)
    travel_mark.set_editor_property("bDrawRule", False)
    slot_on_canvas(travel_canvas, travel_mark, full, (0, 0, 0, 0), z_order=0)

    travel_caption = make_text(tree, "TravelCaptionText", "ENTERING THE ARENA", FS_TRAVEL_CAP, INK)
    slot_on_canvas(travel_canvas, travel_caption, anchors(0.5, 0.55), (0.0, 0.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=1)

    travel_hint = make_text(
        tree, "TravelHintText",
        "THIS CAN TAKE A FEW SECONDS.  A FAILURE WILL BE REPORTED, NOT SWALLOWED.",
        FS_TRAVEL_HINT, INK_DIM)
    slot_on_canvas(travel_canvas, travel_hint, anchors(0.5, 0.55), (0.0, 34.0, 0.0, 0.0),
                   alignment=(0.5, 0.0), auto_size=True, z_order=1)

    # ---- 7. Failure banner, over absolutely everything -----------------------
    #
    # A minute is a long time for a banner and it is deliberate: the failure that
    # matters happens while the player is looking at a DIFFERENT screen. It has
    # to outrank every modal, which is why it is last in the tree.
    banner = make_border(tree, "FailureBanner", C(1.0, 0.46, 0.08, 0.85), (0.0, 2.0, 0.0, 2.0))
    banner.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)
    slot_on_canvas(root, banner, top_span, (0.0, 54.0, 0.0, 0.0), auto_size=True, z_order=10)

    banner_fill = make_border(tree, "FailureFill", BANNER, (0.0, 11.0, 0.0, 11.0))
    banner.add_child(banner_fill)

    banner_box = mk(tree, unreal.VerticalBox, "FailureBox")
    banner_fill.add_child(banner_box)
    vbox_slot(banner_box, make_text(tree, "FailureHeadlineText", "COULD NOT CONNECT",
                                    FS_BANNER_HEAD, AMBER),
              h_align=unreal.HorizontalAlignment.H_ALIGN_CENTER)
    vbox_slot(banner_box, make_text(tree, "FailureDetailText", " ", FS_BANNER_BODY,
                                    C(0.90, 0.97, 1.00, 0.8)),
              padding=(0.0, 4.0, 0.0, 0.0),
              h_align=unreal.HorizontalAlignment.H_ALIGN_CENTER)

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
    "RowPlate", "TopEdge", "BottomEdge", "SelectionWash", "SelectionBar",
    "ChevronText", "LabelText", "StatusText", "ValueText",
    "LeftArrowText", "RightArrowText",
]

TITLE_BIND_NAMES = [
    "Backdrop", "Wordmark", "TaglineText", "AddressChip", "AddressCaptionText",
    "AddressValueText", "PortWarningText", "ConsolePanel",
    "RowPlay", "RowJoin", "RowDifficulty", "RowMode", "RowSettings", "RowQuit",
    "BlurbText", "FooterKeysText", "FooterHintText", "Bezel", "MenuCursor",
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

    if reloaded.generated_class() is None:
        fail("{0}: no generated class - the Blueprint did not compile".format(name))


# =============================================================================

def main():
    log("=" * 70)
    log("Trace - spec v17 section 4: authoring the title screen as UMG assets")
    log("Target: {0}".format(MENU_DIR))
    log("=" * 70)

    if ROBOTO is None:
        fail("/Engine/EngineFonts/Roboto did not load. Every text block would fall back to a "
             "default font and the screen would not match the Canvas renderer.")
        return 1

    row_asset = build_menu_row()
    title_asset = build_title_menu(row_asset)

    log("-" * 70)
    log("Verifying the BindWidget contract against the reloaded assets")
    if row_asset is not None:
        verify("WBP_MenuRow", ROW_BIND_NAMES, "RowRoot")
    if title_asset is not None:
        verify("WBP_TitleMenu", TITLE_BIND_NAMES, "RootCanvas")

    log("=" * 70)
    if Failures:
        for message in Failures:
            unreal.log_error("[MenuWidgets] FAILED: {0}".format(message))
        unreal.log_error(
            "[MenuWidgets] VERDICT: {0} failure(s). The assets are NOT trustworthy; the game will "
            "fall back to the Canvas title screen and say so in the log.".format(len(Failures)))
        return 1

    log("VERDICT: WBP_MenuRow and WBP_TitleMenu written and verified under {0}.".format(MENU_DIR))
    log("Next: launch the game and run `Trace.UI.VerifyMenu` in the console. It measures the")
    log("widget's row rectangles against the Canvas layout maths, in the pixel space the mouse")
    log("works in. `Trace.UI.VerifyMenu redarm` must FAIL - that is what proves it can.")
    log("=" * 70)
    return 0


EXIT_CODE = main()
if EXIT_CODE != 0:
    raise SystemExit(EXIT_CODE)
