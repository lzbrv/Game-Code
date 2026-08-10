# =============================================================================
# Trace - generate-hud-widgets.py
#
# Authors the two Widget Blueprints behind the in-match HUD's bottom-right
# ammo + status corner (spec v17 section 4, step 4b):
#
#     /Game/Trace/UI/HUD/WBP_TraceHudCorner       parent UTraceHudCornerWidget
#     /Game/Trace/UI/HUD/WBP_TraceHudStatusChip   parent UTraceHudStatusChipWidget
#
# -----------------------------------------------------------------------------
# WHAT IS IN THE ASSET AND WHAT IS IN THE C++  (read this before editing either)
# -----------------------------------------------------------------------------
# THE ASSET carries the widget TREE and the STYLING - sizes, paddings, fonts,
# the panel fill, the trough behind the magazine strip. Open WBP_TraceHudCorner
# in the editor and move things around; nothing in C++ will mind, and nothing in
# the asset can change what the HUD SAYS.
#
# THE C++ carries every behaviour: which statuses exist, what their words are,
# what their numbers are, which colours change with state, and the decision to
# show or hide the ammo block at all. It reaches the asset through
# UPROPERTY(meta=(BindWidget)) properties, which bind BY NAME - so the names in
# the MANIFEST below are a contract, and they are stated twice on purpose:
# here, and in RequiredWidgetNames() in the two C++ classes. Two independent
# statements of the same list is what catches a generator that forgot one; a
# list derived from the C++ could not.
#
# HAND EDITS SURVIVE ONLY UNTIL THE NEXT RUN. This script REWRITES both trees
# from scratch. If you want a change to stick, change it here.
#
# -----------------------------------------------------------------------------
# THIS IS A REORGANISATION, NOT A REDESIGN  (spec v17 section 0)
# -----------------------------------------------------------------------------
# Every number below was read out of ATraceHUD's Canvas passes
# (DrawAmmoAndStatuses / DrawAmmoBlock / DrawStatusChip in
# Source/Trace/UI/TraceHUD.cpp) and is authored in DESIGN PIXELS - the units the
# Canvas HUD uses at UIScale 1.0, i.e. against a 1080p-tall viewport. C++ applies
# one uniform scale at runtime so the two paths come out the same size. If you
# change a size here you are changing the game's HUD; that is allowed, but it is
# a design change and not a migration.
#
# -----------------------------------------------------------------------------
# WHY IT NEEDS A C++ HELPER
# -----------------------------------------------------------------------------
# UE 5.8's Python cannot reach a Widget Blueprint's widget tree at all - checked
# headlessly against this engine, not inferred:
#
#     unreal.WidgetBlueprint.get_editor_property('widget_tree')
#         -> Exception: Failed to find property 'widget_tree'
#     dir(unreal.WidgetTree)  ->  no root_widget, no find_widget, no properties
#
# So three primitives live in C++ (Source/Trace/UI/Widgets/HUD/
# TraceHudWidgetAuthoring.h): get the tree, add a widget, clear the tree.
# EVERYTHING ELSE - the shape of the tree, every size, every colour - is here.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd"
#     "$UE" "$PWD/Trace.uproject" \
#         -run=pythonscript -script="$PWD/Scripts/generate-hud-widgets.py" \
#         -unattended -nosplash -NullRHI -nosound \
#         -log -abslog="$PWD/Saved/Logs/trace-generate-hud-widgets.log"
#
# -NullRHI is safe: widget blueprints compile no shaders. Or paste it into the
# editor's Output Log Python prompt:
#
#     exec(open("/path/to/Scripts/generate-hud-widgets.py").read())
#
# Then, in a running game:  Trace.HUD.Corner.Verify
#
# OPTIONS (environment variables - the pythonscript commandlet has no clean way
# to pass argv through -script=)
#   TRACE_HUD_UI_DIR   package dir. Default /Game/Trace/UI/HUD
# =============================================================================

import gc
import os
import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write(
        "generate-hud-widgets.py must run inside Unreal Editor's Python\n"
        "environment. Use the -run=pythonscript command line in this file's\n"
        "header.\n"
    )
    raise SystemExit(2)


UI_DIR = os.environ.get("TRACE_HUD_UI_DIR", "/Game/Trace/UI/HUD").rstrip("/")

CORNER_ASSET = "WBP_TraceHudCorner"
CHIP_ASSET = "WBP_TraceHudStatusChip"

Failures = []


def log(message):
    unreal.log("[HudWidgets] {0}".format(message))


def fail(message):
    Failures.append(message)
    unreal.log_error("[HudWidgets] {0}".format(message))


# =============================================================================
# 1. THE PALETTE
#
# Copied from TraceHUDStyle / TraceHUDStatusStyle in Source/Trace/UI/TraceHUD.cpp.
# ONLY the colours that never change at runtime are here - they are STYLING, and
# styling is what the asset is for. The three that DO change (the rounds colour,
# the count's colour, a chip's tint) are pushed in from C++ every frame through
# FTraceHudCornerState, so that the palette they come from keeps exactly one
# definition. See TraceHudCornerData.h.
# =============================================================================

def color(r, g, b, a=1.0):
    return unreal.LinearColor(r, g, b, a)


INK = color(0.95, 0.96, 1.00, 1.00)          # chip labels
INK_DIM = color(0.68, 0.72, 0.78, 1.00)      # "/30", and the default label ink
PANEL_FILL = color(0.02, 0.03, 0.05, 0.72)   # behind the chips and the ammo plate
TROUGH = color(0.06, 0.07, 0.09, 0.85)       # under the magazine strip
DRAIN_TRACK = color(0.00, 0.00, 0.00, 0.50)  # WithAlpha(TraceHUDStyle::Shadow, 0.5)
NEUTRAL_ROUNDS = color(0.90, 0.93, 1.00, 1.00)  # the ammo plate's resting border tint

# =============================================================================
# 2. THE LAYOUT, in design pixels
#
# Read out of the Canvas passes. The names on the right are the literals they
# came from, so the two can be diffed by eye.
# =============================================================================

BLOCK_W = 260.0        # DrawAmmoAndStatuses: BlockW
PLATE_PAD = 6.0        # DrawAmmoBlock: PlatePad
EDGE = 1.5             # DrawPanel: T = max(1, 1.5 * UIScale) - the panel border
PLATE_BOX_W = BLOCK_W + (PLATE_PAD * 2.0)   # 272: the plate overhangs the chips

LABEL_H = 14.0         # DrawAmmoBlock: LabelH
STRIP_H = 12.0         # DrawAmmoBlock: StripH
NUMBER_H = 40.0        # DrawAmmoBlock: NumberH
ROW_GAP = 5.0          # DrawAmmoBlock: Gap
COUNT_GAP = 4.0        # DrawAmmoBlock: the 4 px between "26" and "/30"

CHIP_H = 24.0          # DrawStatusChip: ChipH
CHIP_TAB_W = 4.0       # DrawStatusChip: TabW
CHIP_DRAIN_H = 3.0     # DrawStatusChip: DrainH
CHIP_TEXT_INSET = 6.0  # DrawStatusChip: the 6 px either side of the text

# Font sizes are the ONE thing here with no Canvas literal to copy: the Canvas
# HUD draws with the engine's bitmap UFonts (GEngine->GetSmallFont()) and UMG
# draws with Slate's Roboto at a point size, so there is no number to port. These
# were chosen by rendering the same fixture both ways at 1280x720 and comparing
# the two frames - see the step's report. Change them in the editor if you
# disagree; nothing in C++ reads them.
FONT_SMALL = 9
FONT_COUNT = 26

# =============================================================================
# 3. THE BINDING CONTRACT
#
# Every name a BindWidget property in C++ will look for. Stated here as well as
# in UTraceHudCornerWidget::RequiredWidgetNames() and
# UTraceHudStatusChipWidget::RequiredWidgetNames(), deliberately: two
# independent lists catch a generator that drops one, a derived list cannot.
# The verification pass at the bottom checks the asset against THIS list, and
# the Widget Blueprint compiler checks it against the C++ one.
# =============================================================================

CORNER_BOUND = [
    "CornerStack", "StatusStack", "PlateOutline", "PlateFill", "AmmoLabelText",
    "ReloadLabelText", "MagazineTrough", "MagazineStrip", "ReloadBar",
    "CountText", "CapacityText",
]

CHIP_BOUND = [
    "ChipOutline", "ChipFill", "ColorTab", "LabelText", "ReadoutText", "DrainBar",
]


# =============================================================================
# 4. Helpers over the authoring primitives
# =============================================================================

def package_path(name):
    return "{0}/{1}".format(UI_DIR, name)


def margin(left=0.0, top=0.0, right=0.0, bottom=0.0):
    m = unreal.Margin()
    m.set_editor_property("left", left)
    m.set_editor_property("top", top)
    m.set_editor_property("right", right)
    m.set_editor_property("bottom", bottom)
    return m


def set_vec2(obj, prop, x, y):
    """
    Sets a two-component property, whatever UE 5.8 decided to call the type today.

    Slate's structs moved from FVector2D to FDeprecateSlateVector2D during the
    5.x line, and the two are NOT interchangeable through the Python bindings:
    FSlateBrush::ImageSize now refuses an unreal.Vector2D outright
    ("Cannot nativize 'Vector2D' as 'DeprecateSlateVector2D'"), while
    UCanvasPanelSlot::Alignment still wants the plain one.

    *** THIS TRY-EACH-TYPE TRICK IS FOR PROPERTIES ONLY. NEVER FOR FUNCTION
    *** ARGUMENTS. ***
    set_editor_property CHECKS the struct type and raises, so a wrong guess here
    is a caught exception and the next candidate runs. A UFUNCTION argument is
    not checked the same way: passing an FDeprecateSlateVector2D (two floats)
    where FVector2D (two doubles) is expected is ACCEPTED, writes the wrong
    number of bytes, and corrupts the heap. That is not a theory -
    `call_with_vec2(slot.set_alignment, 1.0, 1.0)` did exactly this, and the
    damage surfaced later as a SIGSEGV inside Python's garbage collector during
    the blueprint compile, four steps and one subsystem away from the cause. It
    took a five-way bisection to find. Call setters with the exact type.
    """
    last_error = None
    for value in vec2_candidates(x, y):
        try:
            obj.set_editor_property(prop, value)
            return
        except Exception as error:  # noqa: BLE001 - the next candidate may work
            last_error = error
    raise last_error


def vec2_candidates(x, y):
    """
    Every two-component value this engine can build, most likely first.

    Two-step construction is not defensive padding: FDeprecateSlateVector2D takes
    NO constructor arguments through the bindings ("call() takes at most 0
    arguments"), exactly like unreal.Key, so it can only be default-constructed
    and then written into. Same lesson the input-asset generator learned about
    FKey, one struct along.
    """
    out = []
    for type_name in ("DeprecateSlateVector2D", "Vector2D", "Vector2f"):
        maker = getattr(unreal, type_name, None)
        if maker is None:
            continue
        try:
            out.append(maker(x, y))
        except Exception:  # noqa: BLE001
            try:
                value = maker()
                value.set_editor_property("x", x)
                value.set_editor_property("y", y)
                out.append(value)
            except Exception:  # noqa: BLE001
                pass
    return out


def make_anchors(min_x, min_y, max_x, max_y):
    """unreal.Anchors, built with whichever vector type this engine's FAnchors holds."""
    last_error = None
    minimums = vec2_candidates(min_x, min_y)
    maximums = vec2_candidates(max_x, max_y)
    for index in range(min(len(minimums), len(maximums))):
        try:
            anchors = unreal.Anchors()
            anchors.set_editor_property("minimum", minimums[index])
            anchors.set_editor_property("maximum", maximums[index])
            return anchors
        except Exception as error:  # noqa: BLE001
            last_error = error
    raise last_error


def flat_brush(tint, size_x=16.0, size_y=16.0):
    """
    A plain filled rectangle in @tint.

    A default-constructed FSlateBrush has no resource object and renders as a
    white box - which is exactly what UImage looks like when you drop one into a
    fresh widget - so tinting it is the whole story. It is the UMG equivalent of
    the Canvas HUD's DrawRect, which is what every one of these replaces.
    """
    brush = unreal.SlateBrush()
    brush.set_editor_property("draw_as", unreal.SlateBrushDrawType.IMAGE)
    set_vec2(brush, "image_size", size_x, size_y)
    brush.set_editor_property("tint_color", unreal.SlateColor(tint))
    return brush


# NOTE ON SIZE BOXES, and it cost a capture to learn: a USizeBox's WidthOverride
# and HeightOverride each have a SEPARATE bOverride_ flag, and
# set_editor_property("width_override", 260) writes the number WITHOUT setting the
# flag - so the box constrains nothing and quietly sizes to its content. The
# UFUNCTION setters (set_width_override / set_height_override) set both. The first
# generated corner used the property form and came out two and a half times too
# wide, because the magazine strip's thirty ticks were then free to ask for their
# brushes' default 32 px each. Always use the setters here.


def set_font(text_widget, size, tint):
    """Keeps whatever font asset the engine defaults to; only size and colour move."""
    font = text_widget.get_editor_property("font")
    font.set_editor_property("size", size)
    text_widget.set_editor_property("font", font)
    text_widget.set_editor_property("color_and_opacity", unreal.SlateColor(tint))
    # Never wrap: every string in this corner is short, and a wrapped ammo count
    # would change the plate's height mid-fight.
    text_widget.set_editor_property("auto_wrap_text", False)


def add(asset, widget_class, name, parent):
    """
    Constructs one widget into @asset's tree. Aborts the whole run on failure.

    Deliberately fatal: half a tree that still compiles is an asset whose
    BindWidget properties come back null at runtime, and the C++ side would then
    correctly refuse to adopt it - reporting a fault whose real cause was up
    here. Better to say so now.
    """
    widget = unreal.TraceHudWidgetAuthoring.add_widget(asset, widget_class, name, parent)
    if widget is None:
        fail("could not add '{0}' ({1}) to {2}".format(name, widget_class.get_name(), asset.get_name()))
        raise RuntimeError("add_widget failed for {0}".format(name))
    return widget


def open_asset(name, parent_class):
    """
    A brand new, empty asset at UI_DIR/name - deleting whatever was there.

    *** DELETE-AND-RECREATE, AND THE FIRST VERSION DID THE OPPOSITE. *** Rewriting
    the tree in place is the obvious approach and it is what this project's
    input-asset generator has to do (IMC_Trace holds hard references to its
    fourteen IA_ assets, so recreating one would leave the context pointing at
    nothing). Here it CRASHES THE EDITOR, reproducibly:

        clear the tree (19 / 45 widgets evicted)  ->  compile the blueprint
        -> SIGSEGV in _PyWeakref_ClearRef, inside PyGC_Collect, inside
           PyUtil::CollectGarbage, inside FBlueprintCompilationManagerImpl::
           CompileSynchronouslyImpl

    Twice, at the same line, on the SECOND run of this script and never on the
    first - because a first run has nothing to evict. Collecting garbage
    explicitly right after the eviction did not help: the crash simply moved back
    to the compile. Whatever the underlying bug is, it belongs to the combination
    of "destroy a package's worth of widgets" and "then compile in the same
    Python session", and this script does not have to be in that business.

    Delete-and-recreate is safe HERE, specifically, and it is worth saying why:
    nothing holds a hard reference to either asset. The corner loads the chip by
    PATH at runtime and C++ loads the corner by PATH; that indirection was chosen
    so a missing asset degrades to the Canvas corner instead of failing to load,
    and this is the second thing it buys.
    """
    path = package_path(name)

    if unreal.EditorAssetLibrary.does_asset_exist(path):
        log("replacing existing {0}".format(path))
        if not unreal.EditorAssetLibrary.delete_asset(path):
            fail("{0} exists and could not be deleted - is it open in the editor, "
                 "or checked out?".format(path))
            return None

    factory = unreal.WidgetBlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    asset = tools.create_asset(name, UI_DIR, unreal.WidgetBlueprint, factory)
    if asset is None:
        fail("create_asset returned None for {0}".format(path))
        return None

    log("created {0}".format(path))
    # The factory leaves a default root behind; the tree below is authored from
    # nothing so that a fresh asset and a regenerated one are byte-comparable.
    unreal.TraceHudWidgetAuthoring.clear_widget_tree(asset)
    return asset


# =============================================================================
# 5. WBP_TraceHudStatusChip
#
#     RootSize (SizeBox, 24 tall)
#       ChipOutline (Border)          <- 1.5 px of padding IS the outline
#         ChipFill (Border)           <- the dark panel
#           ChipStack (Overlay)
#             ChipRow (HorizontalBox)
#               ColorTab (Image)      <- 4 px of saturated tint down the edge
#               LabelText (TextBlock)
#               ReadoutText (TextBlock)
#             DrainSize (SizeBox, 3 tall, bottom)
#               DrainBar (ProgressBar)
#
# The Canvas pass draws exactly this: a panel, a four-rect border, a tab, two
# strings and a two-rect drain. Slate has no stroked-rect primitive either, so
# the border is two nested Borders - the outer one's PADDING is the stroke.
# =============================================================================

def build_chip(asset):
    root = add(asset, unreal.SizeBox, "RootSize", None)
    root.set_height_override(CHIP_H)
    root.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    outline = add(asset, unreal.Border, "ChipOutline", root)
    outline.set_editor_property("padding", margin(EDGE, EDGE, EDGE, EDGE))
    # Tinted from C++ every frame (the status's own hue at 45%); this is only what
    # it looks like sitting in the editor.
    outline.set_editor_property("brush_color", INK_DIM)
    outline.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    fill = add(asset, unreal.Border, "ChipFill", outline)
    fill.set_editor_property("padding", margin(0.0, 0.0, 0.0, 0.0))
    fill.set_editor_property("brush_color", PANEL_FILL)
    fill.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    stack = add(asset, unreal.Overlay, "ChipStack", fill)
    stack.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    row = add(asset, unreal.HorizontalBox, "ChipRow", stack)
    row.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    row_slot = row.slot
    row_slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    row_slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_FILL)
    # The text centres in the space ABOVE the drain, exactly as DrawStatusChip
    # centres it in (ChipH - DrainH).
    row_slot.set_editor_property("padding", margin(0.0, 0.0, 0.0, CHIP_DRAIN_H))

    tab = add(asset, unreal.Image, "ColorTab", row)
    tab.set_editor_property("brush", flat_brush(unreal.LinearColor.WHITE, CHIP_TAB_W, CHIP_H))
    tab.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)
    tab_slot = tab.slot
    tab_slot.set_editor_property("size", unreal.SlateChildSize(1.0, unreal.SlateSizeRule.AUTOMATIC))
    tab_slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_FILL)

    label = add(asset, unreal.TextBlock, "LabelText", row)
    set_font(label, FONT_SMALL, INK)
    label.set_editor_property("text", unreal.Text("STATUS"))
    label.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)
    label_slot = label.slot
    label_slot.set_editor_property("size", unreal.SlateChildSize(1.0, unreal.SlateSizeRule.FILL))
    label_slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_CENTER)
    label_slot.set_editor_property("padding", margin(CHIP_TEXT_INSET, 0.0, 0.0, 0.0))

    readout = add(asset, unreal.TextBlock, "ReadoutText", row)
    set_font(readout, FONT_SMALL, INK_DIM)
    readout.set_editor_property("text", unreal.Text("0.0s"))
    readout.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)
    readout_slot = readout.slot
    readout_slot.set_editor_property("size", unreal.SlateChildSize(1.0, unreal.SlateSizeRule.AUTOMATIC))
    readout_slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_CENTER)
    readout_slot.set_editor_property("padding", margin(0.0, 0.0, CHIP_TEXT_INSET, 0.0))

    drain_size = add(asset, unreal.SizeBox, "DrainSize", stack)
    drain_size.set_height_override(CHIP_DRAIN_H)
    drain_size.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    drain_slot = drain_size.slot
    drain_slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    drain_slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_BOTTOM)

    drain = add(asset, unreal.ProgressBar, "DrainBar", drain_size)
    style_progress_bar(drain, DRAIN_TRACK)
    drain.set_editor_property("percent", 0.65)
    drain.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)


def style_progress_bar(bar, track_color):
    """
    Flat track, flat fill, no border - the two DrawRects the Canvas pass makes.

    The stock UMG progress bar style is a rounded, bordered thing that would read
    as a different widget entirely, so both brushes are replaced. The FILL brush
    stays white: C++ multiplies it by the live colour every frame
    (SetFillColorAndOpacity), and a tinted fill brush would multiply twice.
    """
    style = bar.get_editor_property("widget_style")
    style.set_editor_property("background_image", flat_brush(track_color))
    style.set_editor_property("fill_image", flat_brush(unreal.LinearColor.WHITE))
    bar.set_editor_property("widget_style", style)
    set_vec2(bar, "border_padding", 0.0, 0.0)
    bar.set_editor_property("bar_fill_type", unreal.ProgressBarFillType.LEFT_TO_RIGHT)


# =============================================================================
# 6. WBP_TraceHudCorner
#
#     RootCanvas (CanvasPanel)
#       CornerStack (VerticalBox)      <- anchored bottom-right; C++ sets its
#         StatusBox (SizeBox, 260)        offset and its render scale, nothing else
#           StatusStack (VerticalBox)  <- chips are added here at runtime
#         PlateBox (SizeBox, 272)      <- slot padding -6 all round: the plate
#           PlateOutline (Border)         overhangs the chips by PlatePad, which
#             PlateFill (Border)          is exactly what DrawPanel does
#               PlateRows (VerticalBox)
#                 LabelSize (SizeBox 14) -> LabelRow -> AmmoLabelText | spacer | ReloadLabelText
#                 StripSize (SizeBox 12) -> StripStack (Overlay) -> MagazineTrough
#                                                                   MagazineStrip (HorizontalBox)
#                                                                   ReloadBar (ProgressBar)
#                 NumberSize (SizeBox 40) -> NumberRow -> spacer | CountText | CapacityText
# =============================================================================

def build_corner(asset):
    root = add(asset, unreal.CanvasPanel, "RootCanvas", None)
    root.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    corner = add(asset, unreal.VerticalBox, "CornerStack", root)
    corner.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    corner_slot = corner.slot
    # Bottom-right, aligned by its own bottom-right corner, sized by its content.
    # The OFFSET is set from C++ every frame because it is the HUD's margin and
    # has to scale with the viewport; everything else about this slot is fixed.
    # Through the SETTERS, not set_editor_property: a canvas slot keeps its anchor,
    # alignment and offsets inside one FAnchorData ("layout_data"), so there is no
    # 'anchors' property to write - only UCanvasPanelSlot::SetAnchors, which also
    # keeps the cached layout in step.
    corner_slot.set_anchors(make_anchors(1.0, 1.0, 1.0, 1.0))
    # EXACTLY unreal.Vector2D, never the try-each-type helper - see set_vec2's
    # note. UCanvasPanelSlot::SetAlignment takes an FVector2D and the binding will
    # not stop you handing it something else.
    corner_slot.set_alignment(unreal.Vector2D(1.0, 1.0))
    corner_slot.set_auto_size(True)
    corner_slot.set_offsets(margin(-34.0, -34.0, 0.0, 0.0))

    # ---- the status stack ---------------------------------------------------
    # The stack's width is the PLATE's width, and the chips are inset inside it by
    # C++ (TraceHudCornerLayout::ChipInsetDesignPx) - see the note on
    # StackMarginDesignPx. The plate is the outermost thing in this corner and it
    # is what the anchor pins.
    status_box = add(asset, unreal.SizeBox, "StatusBox", corner)
    status_box.set_width_override(PLATE_BOX_W)
    status_box.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    status_box_slot = status_box.slot
    status_box_slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)

    status = add(asset, unreal.VerticalBox, "StatusStack", status_box)
    status.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    # ---- the ammo plate -----------------------------------------------------
    plate_box = add(asset, unreal.SizeBox, "PlateBox", corner)
    plate_box.set_width_override(PLATE_BOX_W)
    plate_box.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    plate_box_slot = plate_box.slot
    plate_box_slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    # No padding at all: the plate IS the stack's outer edge, on all three sides.
    # The first version gave it a NEGATIVE -6 padding so it could overhang a stack
    # anchored at the chips' 40 px margin - measured four screen pixels out at the
    # bottom, so the whole arrangement was turned inside out: the stack anchors at
    # 34 and the CHIPS carry a positive 6 px inset instead (set by C++, since chips
    # are made at runtime). Same pixels, no negative numbers.
    plate_box_slot.set_editor_property("padding", margin(0.0, 0.0, 0.0, 0.0))

    plate_outline = add(asset, unreal.Border, "PlateOutline", plate_box)
    plate_outline.set_editor_property("padding", margin(EDGE, EDGE, EDGE, EDGE))
    plate_outline.set_editor_property("brush_color", NEUTRAL_ROUNDS)
    plate_outline.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    plate_fill = add(asset, unreal.Border, "PlateFill", plate_outline)
    # 1.5 of outline + 4.5 here = the 6 px inset the Canvas plate has always had.
    plate_fill.set_editor_property("padding", margin(PLATE_PAD - EDGE, PLATE_PAD - EDGE,
                                                     PLATE_PAD - EDGE, PLATE_PAD - EDGE))
    plate_fill.set_editor_property("brush_color", PANEL_FILL)
    plate_fill.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    rows = add(asset, unreal.VerticalBox, "PlateRows", plate_fill)
    rows.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    # ---- row 1: the words ---------------------------------------------------
    label_size = add(asset, unreal.SizeBox, "LabelSize", rows)
    label_size.set_height_override(LABEL_H)
    label_size.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    label_size.slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)

    label_row = add(asset, unreal.HorizontalBox, "LabelRow", label_size)
    label_row.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    ammo_label = add(asset, unreal.TextBlock, "AmmoLabelText", label_row)
    set_font(ammo_label, FONT_SMALL, INK_DIM)
    ammo_label.set_editor_property("text", unreal.Text("AMMO"))
    ammo_label.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)
    ammo_label.slot.set_editor_property("size",
        unreal.SlateChildSize(1.0, unreal.SlateSizeRule.AUTOMATIC))
    ammo_label.slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_CENTER)

    label_spacer = add(asset, unreal.Spacer, "LabelSpacer", label_row)
    label_spacer.slot.set_editor_property("size", unreal.SlateChildSize(1.0, unreal.SlateSizeRule.FILL))

    reload_label = add(asset, unreal.TextBlock, "ReloadLabelText", label_row)
    set_font(reload_label, FONT_SMALL, INK_DIM)
    reload_label.set_editor_property("text", unreal.Text("[R]  RELOAD"))
    reload_label.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)
    reload_label.slot.set_editor_property("size",
        unreal.SlateChildSize(1.0, unreal.SlateSizeRule.AUTOMATIC))
    reload_label.slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_CENTER)

    # ---- row 2: the magazine ------------------------------------------------
    strip_size = add(asset, unreal.SizeBox, "StripSize", rows)
    strip_size.set_height_override(STRIP_H)
    strip_size.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    strip_size.slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    strip_size.slot.set_editor_property("padding", margin(0.0, ROW_GAP, 0.0, 0.0))

    strip_stack = add(asset, unreal.Overlay, "StripStack", strip_size)
    strip_stack.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    trough = add(asset, unreal.Image, "MagazineTrough", strip_stack)
    trough.set_editor_property("brush", flat_brush(TROUGH, BLOCK_W, STRIP_H))
    trough.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)
    trough.slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    trough.slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_FILL)

    strip = add(asset, unreal.HorizontalBox, "MagazineStrip", strip_stack)
    strip.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    strip.slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    strip.slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_FILL)

    reload_bar = add(asset, unreal.ProgressBar, "ReloadBar", strip_stack)
    style_progress_bar(reload_bar, unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    reload_bar.set_editor_property("percent", 0.0)
    # Collapsed at rest: the strip is the resting state and the bar only appears
    # mid-reload. C++ swaps the two; this is just where it starts.
    reload_bar.set_editor_property("visibility", unreal.SlateVisibility.COLLAPSED)
    reload_bar.slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    reload_bar.slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_FILL)

    # ---- row 3: the count ---------------------------------------------------
    number_size = add(asset, unreal.SizeBox, "NumberSize", rows)
    number_size.set_height_override(NUMBER_H)
    number_size.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)
    number_size.slot.set_editor_property("horizontal_alignment", unreal.HorizontalAlignment.H_ALIGN_FILL)
    number_size.slot.set_editor_property("padding", margin(0.0, ROW_GAP, 0.0, 0.0))

    number_row = add(asset, unreal.HorizontalBox, "NumberRow", number_size)
    number_row.set_editor_property("visibility", unreal.SlateVisibility.SELF_HIT_TEST_INVISIBLE)

    number_spacer = add(asset, unreal.Spacer, "NumberSpacer", number_row)
    number_spacer.slot.set_editor_property("size", unreal.SlateChildSize(1.0, unreal.SlateSizeRule.FILL))

    count = add(asset, unreal.TextBlock, "CountText", number_row)
    set_font(count, FONT_COUNT, NEUTRAL_ROUNDS)
    count.set_editor_property("text", unreal.Text("26"))
    count.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)
    count.slot.set_editor_property("size", unreal.SlateChildSize(1.0, unreal.SlateSizeRule.AUTOMATIC))
    # Bottom-aligned, so "26" and "/30" sit on one line instead of stepping down -
    # the same fix the Canvas pass made by baseline-aligning the capacity.
    count.slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_BOTTOM)

    capacity = add(asset, unreal.TextBlock, "CapacityText", number_row)
    set_font(capacity, FONT_SMALL, INK_DIM)
    capacity.set_editor_property("text", unreal.Text("/30"))
    capacity.set_editor_property("visibility", unreal.SlateVisibility.HIT_TEST_INVISIBLE)
    capacity.slot.set_editor_property("size", unreal.SlateChildSize(1.0, unreal.SlateSizeRule.AUTOMATIC))
    capacity.slot.set_editor_property("vertical_alignment", unreal.VerticalAlignment.V_ALIGN_BOTTOM)
    capacity.slot.set_editor_property("padding", margin(COUNT_GAP, 0.0, 0.0, 4.0))


# =============================================================================
# 7. Verification - from disk, and against the binding contract
# =============================================================================

def verify(name, required):
    """
    Re-loads the asset BY PATH and checks the tree the game will actually see.

    By path rather than by holding on to the object above, for the same reason
    the input generator does it: what the running game does is load a path, and
    an asset that exists in memory but did not serialise is precisely the failure
    this must not ship silently.
    """
    path = package_path(name)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        fail("{0} did not reload from disk".format(path))
        return

    tree = unreal.TraceHudWidgetAuthoring.get_widget_tree(asset)
    if tree is None:
        fail("{0} reloaded without a widget tree".format(path))
        return

    generated = unreal.BlueprintEditorLibrary.generated_class(asset)
    if generated is None:
        fail("{0} has no generated class - it did not compile".format(path))
        return

    # The class the game loads is "<path>.<name>_C". Checked explicitly, because
    # that string is written down in C++ (CornerBlueprintPath) and a mismatch
    # there is a silent, permanent fall back to Canvas.
    class_path = "{0}.{1}_C".format(path, name)
    loaded_class = unreal.load_object(None, class_path)
    if loaded_class is None:
        fail("{0} does not resolve - the C++ path constant would not load".format(class_path))

    # Asked THROUGH UWidgetTree::FindWidget, which is the same lookup BindWidget
    # performs at runtime - not by walking the tree this script just built, which
    # would only prove the script agrees with itself.
    found = {}
    missing = []
    for widget_name in required:
        obj = unreal.TraceHudWidgetAuthoring.find_widget(asset, widget_name)
        if obj is None:
            missing.append(widget_name)
        else:
            found[widget_name] = obj.get_class().get_name()

    if missing:
        fail("{0} is MISSING bound widget(s): {1}".format(name, ", ".join(missing)))
    else:
        log("  {0}: all {1} bound widget(s) present".format(name, len(required)))
        for widget_name in required:
            log("      {0:<18} {1}".format(widget_name, found[widget_name]))

    status = asset.get_editor_property("status")
    log("  {0}: blueprint status = {1}".format(name, status))
    if status in (unreal.BlueprintStatus.BS_ERROR,):
        fail("{0} compiled WITH ERRORS - the BindWidget contract is not satisfied. "
             "The game will refuse to adopt it and draw the Canvas corner.".format(name))


# =============================================================================
# 8. Generate
# =============================================================================

def main():
    log("=" * 70)
    log("spec v17 s4 (step 4b) - the HUD's bottom-right corner becomes UMG")
    log("target directory: {0}".format(UI_DIR))
    log("=" * 70)

    if not unreal.EditorAssetLibrary.does_directory_exist(UI_DIR):
        unreal.EditorAssetLibrary.make_directory(UI_DIR)
        log("created {0}".format(UI_DIR))

    # The chip first: the corner has no hard reference to it (it is loaded by
    # path at runtime), but a person reading the log should see the part before
    # the whole.
    chip = open_asset(CHIP_ASSET, unreal.TraceHudStatusChipWidget)
    if chip is None:
        return 1
    build_chip(chip)

    corner = open_asset(CORNER_ASSET, unreal.TraceHudCornerWidget)
    if corner is None:
        return 1
    build_corner(corner)

    # *** DROP EVERY WIDGET WRAPPER BEFORE COMPILING. *** Compiling a Widget
    # Blueprint rebuilds its tree template and destroys the UWidgets this script
    # just constructed - and it does so through a full CollectGarbage, which UE's
    # Python plugin answers by running PYTHON's cyclic collector. With this
    # script's ~60 widget wrappers still pending collection, that combination
    # segfaults inside _PyWeakref_ClearRef, reproducibly, at the first compile.
    # Collecting here retires the wrappers while nothing is mid-compile.
    #
    # It is one line and it looks superstitious, which is exactly why it says so
    # here: without it this script crashes the editor every time.
    gc.collect()

    for asset in (chip, corner):
        unreal.BlueprintEditorLibrary.compile_blueprint(asset)
        if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
            fail("failed to save {0}".format(asset.get_path_name()))

    log("-" * 70)
    log("verifying from disk")
    verify(CHIP_ASSET, CHIP_BOUND)
    verify(CORNER_ASSET, CORNER_BOUND)

    log("=" * 70)
    if Failures:
        for message in Failures:
            unreal.log_error("[HudWidgets] FAILED: {0}".format(message))
        unreal.log_error("[HudWidgets] VERDICT: {0} failure(s). The assets are NOT trustworthy; the "
                         "game will fall back to the Canvas corner (which is fine, and says so in "
                         "the log).".format(len(Failures)))
        return 1

    log("VERDICT: {0} and {1} written and verified under {2}.".format(
        CORNER_ASSET, CHIP_ASSET, UI_DIR))
    log("Next: launch the game and run `Trace.HUD.Corner.Verify` in the console.")
    log("=" * 70)
    return 0


EXIT_CODE = main()
if EXIT_CODE != 0:
    raise SystemExit(EXIT_CODE)
