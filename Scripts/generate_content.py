# =============================================================================
# Trace - generate_content.py
#
# Generates the small set of MATERIALS the Tron arena needs, headlessly, into
# /Game/Generated/Materials/.
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
# So we generate two tiny materials of our own. They are a few KB each, and the
# GENERATOR is what lives in the repo as text - which is exactly the project's
# asset policy: no binary art is committed, every developer regenerates the same
# bytes from the same pinned engine (5.8.1).
#
# Content/Generated/ is gitignored. If it is missing, ATraceArenaBuilder falls
# back to BasicShapeMaterial and the arena renders flat but still plays - see
# ATraceArenaBuilder::MakeMID.
#
# -----------------------------------------------------------------------------
# WHAT IT MAKES
# -----------------------------------------------------------------------------
# /Game/Generated/Materials/M_TraceNeon
#     Unlit, opaque, single-sided.
#     EmissiveColor = Color (vector param) * Glow (scalar param).
#     Opaque + unlit is the important combination: it writes depth so neon blocks
#     occlude properly, and it ignores every light in the scene so a neon line is
#     exactly the colour asked for regardless of where the arena's lights point.
#     Glow > 1 pushes the value past the bloom threshold, which is what turns a
#     coloured line into an actual glowing tube on screen.
#
# /Game/Generated/Materials/M_TraceSurface
#     DefaultLit, opaque, single-sided.
#     BaseColor = BaseColor (vector param)
#     Metallic  = Metallic  (scalar param)
#     Roughness = Roughness (scalar param)
#     EmissiveColor = Emissive (vector param) * EmissiveStrength (scalar param)
#     The structural material: near-black, low roughness so screen-space
#     reflections mirror the neon back off the floor, with an optional emissive
#     term so a surface can carry a faint self-lit tint without a second draw.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
#         "/path/to/Trace.uproject" \
#         -run=pythonscript \
#         -script="/path/to/Scripts/generate_content.py" \
#         -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
# Windows: same switches, Engine\Binaries\Win64\UnrealEditor-Cmd.exe.
# Or paste into the editor's Python prompt:
#     exec(open(".../Scripts/generate_content.py").read())
#
# Do NOT add -nullrhi. Material compilation needs a real RHI to build shaders;
# without one the assets save with no shader map and render as the default
# checkerboard at runtime.
#
# -----------------------------------------------------------------------------
# OPTIONS (environment variables)
# -----------------------------------------------------------------------------
#   TRACE_CONTENT_DIR    package dir. Default: /Game/Generated/Materials
#   TRACE_FORCE_CONTENT  "1" to delete and rebuild materials that already exist.
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


MATERIAL_DIR = os.environ.get("TRACE_CONTENT_DIR", "/Game/Generated/Materials").rstrip("/")
FORCE = os.environ.get("TRACE_FORCE_CONTENT", "0") == "1"


def log(message):
    unreal.log("[Trace] {0}".format(message))


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


def new_expression(material, klass, x, y):
    return MEL.create_material_expression(material, klass, x, y)


def link(from_expr, from_pin, to_expr, to_pin):
    if not MEL.connect_material_expressions(from_expr, from_pin, to_expr, to_pin):
        raise RuntimeError("could not connect '{0}' -> '{1}'".format(from_pin or "out", to_pin))


def link_property(expr, pin, prop):
    if not MEL.connect_material_property(expr, pin, prop):
        raise RuntimeError("could not connect '{0}' to material property {1}".format(pin or "out", prop))


def vector_param(material, name, value, x, y, group="Trace"):
    expr = new_expression(material, unreal.MaterialExpressionVectorParameter, x, y)
    expr.set_editor_property("parameter_name", name)
    expr.set_editor_property("default_value", value)
    expr.set_editor_property("group", group)
    return expr


def scalar_param(material, name, value, x, y, group="Trace"):
    expr = new_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
    expr.set_editor_property("parameter_name", name)
    expr.set_editor_property("default_value", float(value))
    expr.set_editor_property("group", group)
    return expr


def multiply(material, a, b, x, y):
    expr = new_expression(material, unreal.MaterialExpressionMultiply, x, y)
    link(a, "", expr, "A")
    link(b, "", expr, "B")
    return expr


def create_material(name):
    """Creates (or replaces) an empty UMaterial and returns it, or None to skip."""
    package_path = "{0}/{1}".format(MATERIAL_DIR, name)

    if unreal.EditorAssetLibrary.does_asset_exist(package_path):
        if not FORCE:
            log("{0} already exists; skipping (set TRACE_FORCE_CONTENT=1 to rebuild).".format(package_path))
            return None
        unreal.EditorAssetLibrary.delete_asset(package_path)
        log("Deleted existing {0}.".format(package_path))

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(name, MATERIAL_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("AssetTools refused to create {0}".format(package_path))
    return material


def finish_material(material):
    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
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

    colour = vector_param(material, "Color", unreal.LinearColor(0.0, 0.8, 1.0, 1.0), -560, -80)
    glow = scalar_param(material, "Glow", 6.0, -560, 140)
    product = multiply(material, colour, glow, -280, 0)
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

    base_colour = vector_param(material, "BaseColor", unreal.LinearColor(0.01, 0.012, 0.016, 1.0), -560, -320)
    link_property(base_colour, "", unreal.MaterialProperty.MP_BASE_COLOR)

    metallic = scalar_param(material, "Metallic", 0.0, -560, -120)
    link_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)

    roughness = scalar_param(material, "Roughness", 0.35, -560, 0)
    link_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    # Optional self-lit term. Defaults to black so a plain structural surface
    # costs nothing; the builder dials it up for panels that should read as
    # backlit rather than as a separate neon strip.
    emissive = vector_param(material, "Emissive", unreal.LinearColor(0.0, 0.0, 0.0, 1.0), -560, 160)
    strength = scalar_param(material, "EmissiveStrength", 1.0, -560, 380)
    link_property(multiply(material, emissive, strength, -280, 240), "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    finish_material(material)


def main():
    if not unreal.EditorAssetLibrary.does_directory_exist(MATERIAL_DIR):
        unreal.EditorAssetLibrary.make_directory(MATERIAL_DIR)
        log("Created {0}".format(MATERIAL_DIR))

    build_neon()
    build_surface()
    log("generate_content.py finished.")


try:
    main()
except Exception as exc:  # noqa: BLE001 - a commandlet needs the reason in the log
    log_error("generate_content.py failed: {0}".format(exc))
    raise
