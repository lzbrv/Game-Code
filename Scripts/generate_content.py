# =============================================================================
# Trace - generate_content.py
#
# Authors the two PARENT materials the Tron arena needs, headlessly, into
# /Game/Trace/Materials/Parents/ - and re-points the committed material
# INSTANCES at them.
#
# -----------------------------------------------------------------------------
# WHY THIS EXISTS
# -----------------------------------------------------------------------------
# ATraceArenaBuilder builds the whole playfield from C++, and for a Tron look it
# needs surfaces that EMIT. Nothing shipped with the engine does the job:
#
#   /Engine/BasicShapes/BasicShapeMaterial   DefaultLit, exposes only Color and
#                                            Roughness. No emissive input at all,
#                                            so an "emissive" line drawn with it
#                                            is just a light-grey box.
#   /Engine/EngineMaterials/EmissiveMeshMaterial
#                                            Unlit, but BLEND_Additive and its
#                                            emissive is Color * a *grid texture*
#                                            (DefaultWhiteGrid_Low). Additive
#                                            geometry writes no depth, so every
#                                            neon block would be see-through and
#                                            sort badly against the floor, and the
#                                            grid texture patterns everything.
#   /Engine/EngineMaterials/EmissiveTexturedMaterial
#                                            Unlit, but driven entirely by a
#                                            texture parameter - no colour param.
#
# So we author two small materials of our own.
#
# -----------------------------------------------------------------------------
# WHERE THEY LIVE, AND WHY THAT CHANGED  (spec v17 section 3)
# -----------------------------------------------------------------------------
# THE OLD RULE IS RETIRED. This script's header used to say "no binary art is
# committed, every developer regenerates the same bytes from the same pinned
# engine", and the old default output directory was /Game/Generated/Materials,
# which .gitignore excludes. Both statements were true when they were written and
# both are now FALSE: the arena bake (spec v15) committed a 639-file .umap and 66
# material assets, and "build contract 2: this project cannot author .uassets" no
# longer holds either. Nothing the game ships may depend on a gitignored asset, so
# the DEFAULT OUTPUT IS NOW THE COMMITTED DIRECTORY:
#
#   /Game/Trace/Materials/Parents/     M_TraceSurface, M_TraceNeon   (committed)
#   /Game/Trace/Materials/Instances/   MI_Surface_*, MI_Neon_*       (committed,
#                                      written by the C++ bake, re-pointed here)
#
# This script is therefore no longer "the thing that makes the materials exist on
# your machine". It is the AUTHORING TOOL for the two parents: run it when you
# want to change the parent graph, then commit the result like any other asset.
#
# THE RUNTIME GENERATOR PATH IS STILL A LIVE FALLBACK (spec v17 section 0 rule 1).
# ATraceArenaBuilder resolves its materials in this order:
#     1. /Game/Trace/Materials/Parents/M_*        the committed source of truth
#     2. /Game/Generated/Materials/M_*            what this script writes when it
#                                                 is pointed there by hand
#     3. /Engine/BasicShapes/BasicShapeMaterial   flat and lit, but it plays
# and it LOGS which one it got. To exercise arm 2, point this script at the old
# directory:  TRACE_CONTENT_DIR=/Game/Generated/Materials
#
# -----------------------------------------------------------------------------
# WHAT IT MAKES
# -----------------------------------------------------------------------------
# M_TraceNeon
#     Unlit, opaque, single-sided. THE EDGE MATERIAL: every line that defines a
#     shape in the arena wears this.
#     EmissiveColor = (Color * Tint) * (Glow * GlowScale).
#     Opaque + unlit is the important combination: it writes depth so neon blocks
#     occlude properly, and it ignores every light in the scene so a neon line is
#     exactly the colour asked for regardless of where the arena's lights point.
#     Glow > 1 pushes the value past the bloom threshold, which is what turns a
#     coloured line into an actual glowing tube on screen.
#
# M_TraceSurface
#     DefaultLit, opaque, single-sided. THE STRUCTURAL MATERIAL: near-black, low
#     roughness so screen-space reflections mirror the neon back off the floor,
#     with an optional emissive term so a surface can carry a faint self-lit tint
#     without a second draw.
#     BaseColor = BaseColor * Tint
#     Metallic  = Metallic
#     Roughness = Roughness
#     EmissiveColor = Emissive * EmissiveStrength
#                     + (HIGH/EPIC only) RimColor * Fresnel(4) * RimStrength
#                     + (bUseGrid only)  GridColor * gridline mask * GridStrength
#
# -----------------------------------------------------------------------------
# THE INSTANCE LAYER IS THE POINT  (spec v17 section 3)
# -----------------------------------------------------------------------------
# "Expose the values a designer would want as Material Instance parameters rather
# than baking them into the graph. The instance layer is what lets someone retune
# the look without touching C++ or the parent."
#
# Every knob below is a PARAMETER, so it can be overridden per instance in the
# editor with no recompile of anything. The ones added by spec v17 all default to
# IDENTITY, so opening this pass's materials renders exactly what shipped:
#
#   M_TraceNeon    Tint         (1,1,1,1)  team/family tint over Color
#                  GlowScale    1.0        master emissive strength over Glow
#   M_TraceSurface Tint         (1,1,1,1)  team/family tint over BaseColor
#                  bUseGrid     false      STATIC SWITCH - see below
#                  GridColor    cyan       gridline colour
#                  GridScale    256.0      uu between gridlines
#                  GridWidth    0.03       line thickness, fraction of a cell
#                  GridStrength 1.0        gridline emissive strength
#
# WHY Tint AND GlowScale COST NOTHING. A vector parameter times a scalar parameter
# is a UNIFORM EXPRESSION: the material compiler folds the whole chain into one
# value evaluated on the CPU once per frame, not per pixel. Color * Tint * Glow *
# GlowScale is the same single uniform the old Color * Glow was.
#
# WHY THE GRID IS A StaticSwitchParameter AND NOT A SCALAR. A scalar "GridStrength
# = 0" would still compile the gridline maths into the shader and pay for it on
# every pixel of every structural surface in the arena - and spec v11 section 0
# measures this frame as per-pixel bound. A static switch is resolved at SHADER
# COMPILE time: with bUseGrid false (the default, and what every shipped instance
# uses) the branch does not exist in the shader at all. A material instance can
# still flip it, which costs that instance its own shader permutation and nothing
# else. Same reasoning as the quality switch on the rim, one step further.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \
#         "/path/to/Trace.uproject" \
#         -run=pythonscript \
#         -script="/path/to/Scripts/generate_content.py" \
#         -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
# Windows: same switches, Engine\Binaries\Win64\UnrealEditor.exe.
# Or paste into the editor's Python prompt:
#     exec(open(".../Scripts/generate_content.py").read())
#
# Scripts/bake-arena.sh runs this for you as step 1 of a bake.
#
# Do NOT add -nullrhi. Material compilation needs a real RHI to build shaders;
# without one the assets save with no shader map and render as the default
# checkerboard at runtime.
#
# THE FILES ARE READ-ONLY UNTIL YOU LOCK THEM. .gitattributes marks *.uasset
# `lockable`, so Git LFS checks them out read-only and a save from the editor
# will fail with "file is read only" until you run `git lfs lock <file>`.
#
# -----------------------------------------------------------------------------
# OPTIONS (environment variables)
# -----------------------------------------------------------------------------
#   TRACE_CONTENT_DIR    package dir for the PARENTS.
#                        Default: /Game/Trace/Materials/Parents
#   TRACE_INSTANCE_DIR   package dir of the instances to re-point at them.
#                        Default: /Game/Trace/Materials/Instances
#   TRACE_FORCE_CONTENT  "1" to delete and rebuild parents that already exist.
#                        Anything that changes the parent graph needs this.
#   TRACE_RETIRE_DIRS    comma-separated dirs to sweep for SUPERSEDED copies of
#                        the parents (assets named M_TraceSurface/M_TraceNeon
#                        that nothing references any more) and delete.
#                        Default: /Game/Trace/Materials
# =============================================================================

import os
import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write(
        "generate_content.py must run inside Unreal Editor's Python environment.\n"
        "See the -run=pythonscript command line in this file's header.\n"
    )
    raise SystemExit(2)


MATERIAL_DIR = os.environ.get("TRACE_CONTENT_DIR", "/Game/Trace/Materials/Parents").rstrip("/")
INSTANCE_DIR = os.environ.get("TRACE_INSTANCE_DIR", "/Game/Trace/Materials/Instances").rstrip("/")
FORCE = os.environ.get("TRACE_FORCE_CONTENT", "0") == "1"
RETIRE_DIRS = [d.strip().rstrip("/") for d in
               os.environ.get("TRACE_RETIRE_DIRS", "/Game/Trace/Materials").split(",") if d.strip()]

PARENT_NAMES = ("M_TraceSurface", "M_TraceNeon")


def log(message):
    unreal.log("[Trace] {0}".format(message))


def log_warning(message):
    unreal.log_warning("[Trace] {0}".format(message))


def log_error(message):
    unreal.log_error("[Trace] {0}".format(message))


# -----------------------------------------------------------------------------
# Small wrappers over MaterialEditingLibrary.
#
# The Python names of these calls have been stable since 4.2x, but the *pin*
# names have not always been, so every connection is checked and a failure is
# reported loudly rather than silently producing a black material.
# -----------------------------------------------------------------------------

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary


def new_expression(material, klass, x, y):
    return MEL.create_material_expression(material, klass, x, y)


def link(from_expr, from_pin, to_expr, to_pin):
    if not MEL.connect_material_expressions(from_expr, from_pin, to_expr, to_pin):
        raise RuntimeError("could not connect '{0}' -> '{1}'".format(from_pin or "out", to_pin))


def link_any(from_expr, from_pin, to_expr, candidate_pins):
    """Connects to the first pin name that works.

    UMaterialExpressionStaticSwitchParameter reports its two inputs as "True"/"False"
    in some engine versions and "A"/"B" in others (GetInputName has moved), and a
    wrong guess here is a silent black material rather than a compile error. Trying
    the candidates and reporting the whole list on failure is cheaper than pinning
    this project to one engine minor.
    """
    for pin in candidate_pins:
        if MEL.connect_material_expressions(from_expr, from_pin, to_expr, pin):
            return pin
    raise RuntimeError("none of the pins {0} accepted a connection".format(list(candidate_pins)))


def link_property(expr, pin, prop):
    if not MEL.connect_material_property(expr, pin, prop):
        raise RuntimeError("could not connect '{0}' to material property {1}".format(pin or "out", prop))


def vector_param(material, name, value, x, y, group="Trace", priority=0):
    expr = new_expression(material, unreal.MaterialExpressionVectorParameter, x, y)
    expr.set_editor_property("parameter_name", name)
    expr.set_editor_property("default_value", value)
    expr.set_editor_property("group", group)
    expr.set_editor_property("sort_priority", priority)
    return expr


def scalar_param(material, name, value, x, y, group="Trace", priority=0):
    expr = new_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
    expr.set_editor_property("parameter_name", name)
    expr.set_editor_property("default_value", float(value))
    expr.set_editor_property("group", group)
    expr.set_editor_property("sort_priority", priority)
    return expr


def constant(material, value, x, y):
    expr = new_expression(material, unreal.MaterialExpressionConstant, x, y)
    expr.set_editor_property("r", float(value))
    return expr


def multiply(material, a, b, x, y):
    expr = new_expression(material, unreal.MaterialExpressionMultiply, x, y)
    link(a, "", expr, "A")
    link(b, "", expr, "B")
    return expr


def add(material, a, b, x, y):
    expr = new_expression(material, unreal.MaterialExpressionAdd, x, y)
    link(a, "", expr, "A")
    link(b, "", expr, "B")
    return expr


def subtract(material, a, b, x, y):
    expr = new_expression(material, unreal.MaterialExpressionSubtract, x, y)
    link(a, "", expr, "A")
    link(b, "", expr, "B")
    return expr


def divide(material, a, b, x, y):
    expr = new_expression(material, unreal.MaterialExpressionDivide, x, y)
    link(a, "", expr, "A")
    link(b, "", expr, "B")
    return expr


def minimum(material, a, b, x, y):
    expr = new_expression(material, unreal.MaterialExpressionMin, x, y)
    link(a, "", expr, "A")
    link(b, "", expr, "B")
    return expr


def unary(material, klass, source, x, y):
    expr = new_expression(material, klass, x, y)
    link(source, "", expr, "")
    return expr


def mask(material, source, x, y, r=False, g=False, b=False, a=False):
    expr = new_expression(material, unreal.MaterialExpressionComponentMask, x, y)
    expr.set_editor_property("r", r)
    expr.set_editor_property("g", g)
    expr.set_editor_property("b", b)
    expr.set_editor_property("a", a)
    link(source, "", expr, "")
    return expr


def create_material(name):
    """Creates (or replaces) an empty UMaterial and returns it, or None to skip."""
    package_path = "{0}/{1}".format(MATERIAL_DIR, name)

    if EAL.does_asset_exist(package_path):
        if not FORCE:
            log("{0} already exists; skipping (set TRACE_FORCE_CONTENT=1 to rebuild).".format(package_path))
            return None
        EAL.delete_asset(package_path)
        log("Deleted existing {0}.".format(package_path))

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(name, MATERIAL_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("AssetTools refused to create {0}".format(package_path))
    return material


def finish_material(material):
    MEL.recompile_material(material)
    EAL.save_loaded_asset(material, only_if_is_dirty=False)
    log("Saved {0}".format(material.get_path_name()))


# -----------------------------------------------------------------------------
# M_TraceNeon
# -----------------------------------------------------------------------------

def build_neon():
    material = create_material("M_TraceNeon")
    if material is None:
        return

    # Unlit: a neon edge must be exactly the colour it was told to be. If it
    # responded to the arena's lights it would go dark on the walls the key light
    # cannot reach, which is precisely the failure this whole pass is fixing.
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    # Opaque, not additive: neon geometry has to write depth so it occludes and
    # sorts like solid matter. Additive is what the engine's EmissiveMeshMaterial
    # does and it makes every glowing block transparent.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("two_sided", False)
    # REQUIRED, and its absence is silent in the editor and fatal in a packaged build.
    # Spec v4 section 2 draws the trace as posed Mannequins (UPoseableMeshComponent), so this
    # material is applied to a SKINNED mesh. Without MATUSAGE_SkeletalMesh the skeletal
    # vertex-factory shader permutation is never compiled and the renderer substitutes the default
    # grey checkerboard - the after-images stop being team-coloured silhouettes and become grey
    # blocks. The editor auto-repairs this flag on first use ONLY when it is not running as a game,
    # and every run of this project is -game, so the repair never happens here.
    material.set_editor_property("used_with_skeletal_mesh", True)
    # REQUIRED FOR THE SAME REASON, ONE VERTEX FACTORY OVER. Spec v7 section 8 rebuilt the arena on
    # UInstancedStaticMeshComponent, and an ISM is its own vertex factory: without
    # MATUSAGE_InstancedStaticMeshes the permutation is never compiled, the log says
    # "Material ... missing usage flag InstancedStaticMeshes! Default Material will be used in game",
    # and THE ENTIRE ARENA RENDERS AS GREY DEFAULT MATERIAL. Same silent-in-editor, fatal-in-game
    # trap as the line above, and for the same reason the editor's auto-repair never fires: it is
    # gated on not running as a game, and every run of this project is -game.
    material.set_editor_property("used_with_instanced_static_meshes", True)

    # --- The instance layer ------------------------------------------------------
    # Color and Glow are the two the arena build writes per piece (MakeNeonMID).
    # Tint and GlowScale are the two a DESIGNER retunes across a family of pieces
    # without touching either - both identity by default, so nothing moves until
    # somebody asks it to.
    colour = vector_param(material, "Color", unreal.LinearColor(0.0, 0.8, 1.0, 1.0), -700, -140,
                          group="Trace|Neon", priority=0)
    tint = vector_param(material, "Tint", unreal.LinearColor(1.0, 1.0, 1.0, 1.0), -700, 60,
                        group="Trace|Neon", priority=1)
    glow = scalar_param(material, "Glow", 6.0, -700, 260, group="Trace|Neon", priority=2)
    glow_scale = scalar_param(material, "GlowScale", 1.0, -700, 400, group="Trace|Neon", priority=3)

    tinted = multiply(material, colour, tint, -420, -60)
    strength = multiply(material, glow, glow_scale, -420, 320)
    product = multiply(material, tinted, strength, -200, 80)
    link_property(product, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    finish_material(material)


# -----------------------------------------------------------------------------
# M_TraceSurface
# -----------------------------------------------------------------------------

def build_surface():
    material = create_material("M_TraceSurface")
    if material is None:
        return

    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property("two_sided", False)
    # REQUIRED. This is the material almost every arena block wears, and since spec v7 section 8 the
    # arena is built from UInstancedStaticMeshComponents. An ISM is its own vertex factory, so
    # without MATUSAGE_InstancedStaticMeshes the permutation is never compiled and the renderer
    # substitutes the grey default for THE WHOLE ARENA - the log line to search for is
    # "missing usage flag InstancedStaticMeshes". The editor's auto-repair is gated on not running
    # as a game, and every run of this project is -game, so it never happens here.
    material.set_editor_property("used_with_instanced_static_meshes", True)

    # --- Base colour, with the designer tint over it -----------------------------
    base_colour = vector_param(material, "BaseColor", unreal.LinearColor(0.01, 0.012, 0.016, 1.0),
                               -760, -420, group="Trace|Surface", priority=0)
    tint = vector_param(material, "Tint", unreal.LinearColor(1.0, 1.0, 1.0, 1.0),
                        -760, -240, group="Trace|Surface", priority=1)
    # Uniform x uniform: folded to a single CPU-side value, so this multiply is free
    # per pixel and the default (white) is bit-identical to feeding BaseColor straight in.
    tinted_base = multiply(material, base_colour, tint, -480, -340)
    link_property(tinted_base, "", unreal.MaterialProperty.MP_BASE_COLOR)

    metallic = scalar_param(material, "Metallic", 0.0, -760, -80, group="Trace|Surface", priority=2)
    link_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)

    roughness = scalar_param(material, "Roughness", 0.35, -760, 40, group="Trace|Surface", priority=3)
    link_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    # Optional self-lit term. Defaults to black so a plain structural surface
    # costs nothing; the builder dials it up for panels that should read as
    # backlit rather than as a separate neon strip.
    emissive = vector_param(material, "Emissive", unreal.LinearColor(0.0, 0.0, 0.0, 1.0),
                            -760, 180, group="Trace|Emissive", priority=0)
    strength = scalar_param(material, "EmissiveStrength", 1.0, -760, 380,
                            group="Trace|Emissive", priority=1)
    emissive_base = multiply(material, emissive, strength, -480, 260)

    # -------------------------------------------------------------------------
    # SPEC v11 SECTION 3.6 - THE MATERIAL QUALITY SWITCH, AND THE RIM IT GATES
    #
    # THE QUESTION SPEC v11 SECTION 3.6 ASKS is whether the generated materials
    # have quality switches and whether a higher tier is worth adding. They did
    # not; this is the answer, and it is deliberately the SMALLEST thing that
    # earns its instructions rather than the most impressive.
    #
    # WHAT IT ADDS AT HIGH AND EPIC: a Fresnel rim. The arena's structural
    # albedos are 0.011-0.07 and its three directional lights all come from high
    # angles, so a cover block seen HEAD ON is a near-black rectangle with no
    # edge - the exact defect the AddNeonBlock comment in TraceArenaBuilder.h
    # spends a paragraph on and answers with FOUR EXTRA PIECES OF GEOMETRY per
    # block. A Fresnel term brightens a surface as it turns away from the eye,
    # which on a box means the silhouette edges of whichever faces you can see,
    # i.e. it draws the block's outline for about six shader instructions and no
    # draw calls at all.
    #
    # WHY IT IS BEHIND A QUALITY SWITCH rather than always on: those six
    # instructions are paid PER PIXEL, on the material that almost every opaque
    # pixel in the arena wears, on a frame that spec v11 section 0 measures as
    # per-pixel bound. Low and Medium get the Default pin, which is the material
    # exactly as it shipped, so the whole branch - the Fresnel, the multiply and
    # the add - is compiled out of the Low and Medium shader maps. The switch is
    # driven by r.MaterialQualityLevel, which the Shading scalability group
    # sets, so it follows the video settings menu's Shading row.
    #
    # WHY THE STRENGTH DEFAULTS TO ZERO: the builder is what decides which
    # surfaces get a rim (see MakeSurfaceMID - the near-mirror FLOOR must not,
    # because a floor is viewed at a grazing angle over most of the screen and a
    # Fresnel term there is a white wash across the lower half of the frame, not
    # an edge). A material that defaults to zero is a material that cannot
    # change the look of anything that has not asked for it, including the
    # BasicShapeMaterial fallback path.
    #
    # The pin names are "Default", "Low", "High", "Medium", "Epic" - note that
    # the engine's EMaterialQualityLevel really does order them Low, High,
    # Medium, Epic (MaterialShared.cpp), so connecting by NAME rather than by
    # index is not fussiness, it is the only way to get this right.
    rim_colour = vector_param(material, "RimColor", unreal.LinearColor(0.30, 0.62, 0.95, 1.0),
                              -760, 560, group="Trace|Emissive", priority=2)
    rim_strength = scalar_param(material, "RimStrength", 0.0, -760, 760,
                                group="Trace|Emissive", priority=3)

    fresnel = new_expression(material, unreal.MaterialExpressionFresnel, -760, 940)
    fresnel.set_editor_property("exponent", 4.0)
    fresnel.set_editor_property("base_reflect_fraction", 0.0)

    rim = multiply(material, rim_colour, multiply(material, fresnel, rim_strength, -580, 800), -420, 640)
    emissive_high = add(material, emissive_base, rim, -320, 420)

    quality = new_expression(material, unreal.MaterialExpressionQualitySwitch, -160, 320)
    link(emissive_base, "", quality, "Default")
    link(emissive_high, "", quality, "High")
    link(emissive_high, "", quality, "Epic")

    # -------------------------------------------------------------------------
    # SPEC v17 SECTION 3 - THE OPTIONAL GRIDLINE OVERLAY, BEHIND A STATIC SWITCH
    #
    # "Expose the values a designer would retune as Material INSTANCE parameters
    # (emissive strength, team tint, grid scale, glow) rather than baking them
    # into the parent graph."
    #
    # Three of those four already existed as parameters. GRID SCALE did not,
    # because the arena's grid is GEOMETRY - ATraceArenaBuilder::BuildGrid lays
    # real neon strips - and there is no gridline in this material at all. So one
    # is added here as an OPT-IN overlay: a designer who wants a lit floor plate,
    # a hazard deck or a panelled wall can tick one box on an instance and get
    # gridlines at a spacing they choose, without a new material and without C++.
    #
    # OFF BY DEFAULT AND FREE WHEN OFF. bUseGrid is a StaticSwitchParameter, which
    # the compiler resolves before the shader exists: with it false the ten-odd
    # instructions below are not in the shader map. Every instance the bake wrote
    # leaves it false, so THE SHIPPED ARENA'S SHADER IS THE ONE IT ALWAYS HAD.
    # This is the same argument as the quality switch above, and it is the reason
    # this is a static switch rather than "GridStrength = 0".
    #
    # WORLD SPACE, NOT UVs, and that is not laziness: the arena is built from
    # scaled 100 uu engine cubes, so a 900 uu cover block and a 40 uu trim strip
    # have wildly different UV densities and a UV-based grid would be a different
    # size on every piece. Off world XY, GridScale means uu between lines
    # everywhere, which is the only definition a designer can reason about.
    grid_switch = new_expression(material, unreal.MaterialExpressionStaticSwitchParameter, 300, 320)
    grid_switch.set_editor_property("parameter_name", "bUseGrid")
    grid_switch.set_editor_property("default_value", False)
    grid_switch.set_editor_property("group", "Trace|Grid")
    grid_switch.set_editor_property("sort_priority", 0)

    grid_colour = vector_param(material, "GridColor", unreal.LinearColor(0.0, 0.55, 0.75, 1.0),
                               -760, 1140, group="Trace|Grid", priority=1)
    grid_scale = scalar_param(material, "GridScale", 256.0, -760, 1340, group="Trace|Grid", priority=2)
    grid_width = scalar_param(material, "GridWidth", 0.03, -760, 1460, group="Trace|Grid", priority=3)
    grid_strength = scalar_param(material, "GridStrength", 1.0, -760, 1580, group="Trace|Grid", priority=4)

    # distance-to-nearest-gridline, per axis, in cells:
    #     d = abs(frac(worldXY / GridScale + 0.5) - 0.5)      0 on the line, 0.5 mid-cell
    # then the line mask is 1 - saturate(min(dx, dy) / GridWidth).
    world_position = new_expression(material, unreal.MaterialExpressionWorldPosition, -760, 1720)
    world_xy = mask(material, world_position, -560, 1720, r=True, g=True)
    cells = divide(material, world_xy, grid_scale, -400, 1720)
    shifted = add(material, cells, constant(material, 0.5, -560, 1880), -260, 1760)
    fractional = unary(material, unreal.MaterialExpressionFrac, shifted, -120, 1760)
    centred = subtract(material, fractional, constant(material, 0.5, -260, 1960), 20, 1800)
    distance = unary(material, unreal.MaterialExpressionAbs, centred, 160, 1800)
    distance_x = mask(material, distance, 300, 1720, r=True)
    distance_y = mask(material, distance, 300, 1880, g=True)
    nearest = minimum(material, distance_x, distance_y, 440, 1800)
    normalised = divide(material, nearest, grid_width, 580, 1800)
    line_mask = unary(material, unreal.MaterialExpressionOneMinus,
                      unary(material, unreal.MaterialExpressionSaturate, normalised, 720, 1800), 860, 1800)

    grid_emissive = multiply(material, multiply(material, grid_colour, line_mask, 1000, 1600),
                             grid_strength, 1140, 1640)
    emissive_with_grid = add(material, quality, grid_emissive, 1280, 1400)

    # "True" is the pin taken when bUseGrid is set. The name has moved between
    # engine versions, hence link_any - see its comment.
    link_any(emissive_with_grid, "", grid_switch, ("True", "A"))
    link_any(quality, "", grid_switch, ("False", "B"))

    link_property(grid_switch, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    finish_material(material)


# -----------------------------------------------------------------------------
# Re-pointing the committed instances  (spec v17 section 3)
#
# THE MIGRATION THIS PERFORMS, ONCE. The bake wrote 64 MaterialInstanceConstants
# whose parent was /Game/Trace/Materials/M_TraceSurface or .../M_TraceNeon - the
# flat layout. Spec v17 section 3 asks for Materials/{Parents,Instances}, so the
# parents moved down one level and every instance has to follow them or it loads
# with a null parent and renders as the grey default.
#
# RE-PARENTING RATHER THAN RENAMING WITH A REDIRECTOR, deliberately: a redirector
# is another committed binary file that has to be fixed up later and that nobody
# ever remembers to clean, and this pass is also REBUILDING the parents (they gain
# Tint/GlowScale/the grid), so a fresh asset at the new path is what exists anyway.
#
# THE OVERRIDES SURVIVE BECAUSE THEY ARE KEYED BY NAME. UMaterialInstance stores
# ScalarParameterValues/VectorParameterValues against FMaterialParameterInfo, and
# SetParentEditorOnly prunes only the entries whose names the new parent does not
# have. The new parents are a strict SUPERSET of the old parameter names -
# nothing was renamed or removed - so nothing is pruned. Verified by dumping every
# instance's parameters before and after; see the log lines this prints.
# -----------------------------------------------------------------------------

def parent_objects():
    found = {}
    for name in PARENT_NAMES:
        path = "{0}/{1}".format(MATERIAL_DIR, name)
        if not EAL.does_asset_exist(path):
            raise RuntimeError("{0} was not created".format(path))
        found[name] = EAL.load_asset(path)
    return found


def describe_parameters(instance):
    scalars = {str(p.parameter_info.name): round(float(p.parameter_value), 6)
               for p in instance.get_editor_property("scalar_parameter_values")}
    vectors = {str(p.parameter_info.name): tuple(round(float(c), 6) for c in
                                                 (p.parameter_value.r, p.parameter_value.g,
                                                  p.parameter_value.b, p.parameter_value.a))
               for p in instance.get_editor_property("vector_parameter_values")}
    return scalars, vectors


def repoint_instances(parents):
    if not EAL.does_directory_exist(INSTANCE_DIR):
        log("No {0}; nothing to re-point.".format(INSTANCE_DIR))
        return 0

    moved = 0
    checked = 0
    drifted = []

    for path in EAL.list_assets(INSTANCE_DIR, recursive=True, include_folder=False):
        instance = EAL.load_asset(path)
        if not isinstance(instance, unreal.MaterialInstanceConstant):
            continue

        checked += 1
        current = instance.get_editor_property("parent")
        if current is None:
            log_warning("{0} has NO parent; leaving it alone rather than guessing.".format(path))
            continue

        wanted = parents.get(current.get_name())
        if wanted is None or current == wanted:
            continue

        before_scalars, before_vectors = describe_parameters(instance)
        MEL.set_material_instance_parent(instance, wanted)
        EAL.save_loaded_asset(instance, only_if_is_dirty=False)
        after_scalars, after_vectors = describe_parameters(instance)

        if before_scalars != after_scalars or before_vectors != after_vectors:
            drifted.append((path, before_scalars, before_vectors, after_scalars, after_vectors))
        moved += 1

    log("Re-pointed {0} of {1} material instances in {2} at {3}.".format(
        moved, checked, INSTANCE_DIR, MATERIAL_DIR))

    if drifted:
        for path, bs, bv, as_, av in drifted:
            log_error("{0} did NOT round-trip: scalars {1} -> {2}, vectors {3} -> {4}".format(
                path, bs, as_, bv, av))
        raise RuntimeError(
            "{0} instance(s) lost or changed a parameter override while being re-parented. The new "
            "parents must be a superset of the old parameter NAMES; something was renamed.".format(len(drifted)))

    if moved:
        log("All {0} re-parented instances kept every scalar and vector override exactly.".format(moved))
    return moved


def retire_superseded_parents(parents):
    """Deletes old copies of the parents that nothing references any more."""
    kept = {obj.get_path_name().split(".")[0] for obj in parents.values()}
    removed = 0

    for directory in RETIRE_DIRS:
        if directory == MATERIAL_DIR or not EAL.does_directory_exist(directory):
            continue

        for name in PARENT_NAMES:
            path = "{0}/{1}".format(directory, name)
            if path in kept or not EAL.does_asset_exist(path):
                continue

            referencers = [r for r in EAL.find_package_referencers_for_asset(path, load_assets_to_confirm=False)
                           if r.split(".")[0] not in kept]
            if referencers:
                log_warning(
                    "{0} is superseded by {1}/{2} but is still referenced by {3} package(s) "
                    "({4}...). Leaving it in place - re-run after those are re-pointed.".format(
                        path, MATERIAL_DIR, name, len(referencers), referencers[0]))
                continue

            if EAL.delete_asset(path):
                log("Retired the superseded {0}.".format(path))
                removed += 1
            else:
                log_warning("Could not delete {0} (read-only? `git lfs lock` it first).".format(path))

    return removed


def main():
    if not EAL.does_directory_exist(MATERIAL_DIR):
        EAL.make_directory(MATERIAL_DIR)
        log("Created {0}".format(MATERIAL_DIR))

    build_neon()
    build_surface()

    parents = parent_objects()
    repoint_instances(parents)
    retire_superseded_parents(parents)

    log("generate_content.py finished. Parents: {0}. Instances: {1}.".format(MATERIAL_DIR, INSTANCE_DIR))


try:
    main()
except Exception as exc:  # noqa: BLE001 - a commandlet needs the reason in the log
    log_error("generate_content.py failed: {0}".format(exc))
    raise
