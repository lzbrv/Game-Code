# =============================================================================
# Trace - author_mics.py
#
# Authors the four HAND-AUTHORED MaterialInstanceConstants of the release
# overhaul (MAP plan sections 2.2 and 6.3) into /Game/Trace/Materials/Authored/,
# and ASSERTS every parameter back after saving.
#
# -----------------------------------------------------------------------------
# WHY /Game/Trace/Materials/Authored/ AND NOT Instances/
# -----------------------------------------------------------------------------
# A TRACE_BAKE_FORCE=1 re-bake DELETES /Game/Trace/Materials/Instances/ wholesale
# (Scripts/bake-arena.py - the pre-wipe before the bake re-emits its MICs), so
# nothing hand-made may live there. Parents/ is reserved for the two generated
# parent materials that Scripts/generate_content.py owns. Authored/ is the third
# folder: hand-made instances that survive every re-bake and every regen.
#
# -----------------------------------------------------------------------------
# WHAT IT MAKES (MAP plan section 2.2 table; Floor_Grid values from section 6.3)
# -----------------------------------------------------------------------------
#   MI_Surface_KitBase   centre-kit body (SM_KitRamp/SM_KitRampAlt/SM_KitOctagon
#                        slot 0; those three were ramp1/ramp/octagon before MAP
#                        section 10.4's renames landed in W5-MAPFINISH). StructureColor
#                        body, cover-block emissive lift 0.030 - the value measured
#                        on the cover-block BodyMID in TraceArenaBuilder.cpp
#                        (BuildCoverField: NeonNeutral * 0.030).
#   MI_Surface_KitTop    SM_KitPlatform slot 0 (platform1 before the section 10.4
#                        rename; dominant visible face is the walkable
#                        top). PedestalColor body, EmissiveStrength 0.012 - the
#                        up-facing terrace/dais value (an up-facing surface also
#                        catches the most key light; the two terms stack, so tops
#                        get the small number - see the bank-terrace comment in
#                        TraceArenaBuilder.cpp).
#   MI_Neon_KitLip       the kit's inlaid contour strips (W2 spawns them).
#                        NeonNeutral at Glow 3.2 (GlowLip) - T1, over the bloom
#                        threshold, under the territory reads.
#   MI_Surface_Floor_Grid  the micro-grid floor (section 6.3). bUseGrid TRUE is a
#                        STATIC SWITCH: this one instance buys its own shader
#                        permutation and nothing else does. GridWidth 0.012 is
#                        3 uu of line per 256 uu cell.
#
# The kit is in the neutral centre third: its neon is neutral cyan, never team
# colored, so NONE of these carry FTraceBakedSideTint entries and the half-time
# repaint never touches them.
#
# CONSUMERS (do not rename these assets without updating both):
#   - W2 theme_center_kit.py assigns KitBase/KitTop onto the four MapGeometry
#     meshes and dresses platform/octagon edges with KitLip strips. Its MESH_TO_MIC
#     carries both the pre-rename and the post-rename mesh names.
#   - W2's builder change resolves MI_Surface_Floor_Grid as FloorGridMaterial in
#     ResolveArenaMaterials (committed-first, falls back to SurfaceMaterial).
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \
#         "/path/to/Trace.uproject" \
#         -run=pythonscript -script="/path/to/Scripts/author_mics.py" \
#         -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
# Do NOT add -nullrhi: MI_Surface_Floor_Grid flips a static switch, which means
# compiling a new shader permutation, and without a real RHI the instance saves
# with no shader map and renders as the default checkerboard (same trap
# generate_content.py documents).
#
# IDEMPOTENT: existing instances are re-parented/re-valued in place, never
# deleted - later waves hold references to these paths.
#
# The .uassets are read-only until unlocked (LFS `lockable`); chmod u+w or
# `Scripts/lock.sh` first - see generate_content.py's header.
# =============================================================================

import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write("author_mics.py must run inside Unreal Editor's Python environment.\n")
    raise SystemExit(2)

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary

AUTHORED_DIR = "/Game/Trace/Materials/Authored"
PARENT_DIR = "/Game/Trace/Materials/Parents"

# name, parent, vectors {param: (r,g,b,a)}, scalars {param: v}, switches {param: bool}
# Values are the MAP plan section 2.2 table verbatim (Floor_Grid: section 6.3).
# KitTop's Emissive colour is NeonNeutral: the 0.012 strength is the dais/terrace
# value, and the dais/terrace MID pairs that strength with NeonNeutral - a
# strength with no colour would multiply the parent's black default and do
# nothing at all.
MIC_TABLE = (
    ("MI_Surface_KitBase", "M_TraceSurface",
     {"BaseColor": (0.0155, 0.0185, 0.0250, 1.0),      # StructureColor
      "Emissive":  (0.18, 0.78, 1.00, 1.0)},           # NeonNeutral
     {"Roughness": 0.5, "Metallic": 0.0, "EmissiveStrength": 0.030},
     {}),
    ("MI_Surface_KitTop", "M_TraceSurface",
     {"BaseColor": (0.0140, 0.0170, 0.0240, 1.0),      # PedestalColor
      "Emissive":  (0.18, 0.78, 1.00, 1.0)},           # NeonNeutral
     {"Roughness": 0.35, "EmissiveStrength": 0.012},
     {}),
    ("MI_Neon_KitLip", "M_TraceNeon",
     {"Color": (0.18, 0.78, 1.00, 1.0)},               # NeonNeutral
     {"Glow": 3.2},                                    # GlowLip
     {}),
    ("MI_Surface_Floor_Grid", "M_TraceSurface",
     {"BaseColor": (0.0110, 0.0135, 0.0190, 1.0),      # FloorColor
      "GridColor": (0.18, 0.78, 1.00, 1.0)},           # NeonNeutral
     {"Roughness": 0.16, "Metallic": 0.0,
      "GridScale": 256.0, "GridWidth": 0.012, "GridStrength": 0.06},
     {"bUseGrid": True}),
)

TOLERANCE = 1e-4


def log(msg):
    unreal.log("[Trace][MIC] {0}".format(msg))


def fail(msg):
    raise RuntimeError("[MIC] " + msg)


def get_or_create(name):
    path = "{0}/{1}".format(AUTHORED_DIR, name)
    if EAL.does_asset_exist(path):
        inst = EAL.load_asset(path)
        if not isinstance(inst, unreal.MaterialInstanceConstant):
            fail("{0} exists but is not a MaterialInstanceConstant".format(path))
        log("{0} already exists; re-applying parameters in place.".format(path))
        return inst
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    inst = tools.create_asset(name, AUTHORED_DIR, unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
    if inst is None:
        fail("AssetTools refused to create {0}".format(path))
    return inst


def author_all():
    if not EAL.does_directory_exist(AUTHORED_DIR):
        EAL.make_directory(AUTHORED_DIR)
        log("Created {0}".format(AUTHORED_DIR))

    for name, parent_name, vectors, scalars, switches in MIC_TABLE:
        parent_path = "{0}/{1}".format(PARENT_DIR, parent_name)
        parent = EAL.load_asset(parent_path)
        if parent is None:
            fail("parent {0} is missing - run generate_content.py first".format(parent_path))

        inst = get_or_create(name)
        MEL.set_material_instance_parent(inst, parent)
        for pname, v in vectors.items():
            MEL.set_material_instance_vector_parameter_value(
                inst, pname, unreal.LinearColor(v[0], v[1], v[2], v[3]))
        for pname, v in scalars.items():
            MEL.set_material_instance_scalar_parameter_value(inst, pname, float(v))
        for pname, v in switches.items():
            MEL.set_material_instance_static_switch_parameter_value(inst, pname, bool(v))
        # A static-switch flip changes the shader permutation; update before save
        # so the permutation is compiled and serialized in this (real-RHI) session.
        MEL.update_material_instance(inst)
        EAL.save_loaded_asset(inst, only_if_is_dirty=False)
        log("Authored {0}/{1} (parent {2})".format(AUTHORED_DIR, name, parent_name))


def read_back_and_assert():
    """Reloads every MIC and asserts parent + every parameter it set."""
    problems = []
    for name, parent_name, vectors, scalars, switches in MIC_TABLE:
        path = "{0}/{1}".format(AUTHORED_DIR, name)
        inst = EAL.load_asset(path)
        if inst is None:
            problems.append("{0}: did not load back".format(path))
            continue

        parent = inst.get_editor_property("parent")
        wanted_parent = "{0}/{1}".format(PARENT_DIR, parent_name)
        got_parent = parent.get_path_name().split(".")[0] if parent else "<none>"
        if got_parent != wanted_parent:
            problems.append("{0}: parent {1}, wanted {2}".format(name, got_parent, wanted_parent))

        for pname, want in vectors.items():
            got = MEL.get_material_instance_vector_parameter_value(inst, pname)
            got_t = (got.r, got.g, got.b, got.a)
            if any(abs(g - w) > TOLERANCE for g, w in zip(got_t, want)):
                problems.append("{0}.{1}: {2}, wanted {3}".format(name, pname, got_t, want))
        for pname, want in scalars.items():
            got = float(MEL.get_material_instance_scalar_parameter_value(inst, pname))
            if abs(got - want) > TOLERANCE:
                problems.append("{0}.{1}: {2}, wanted {3}".format(name, pname, got, want))
        for pname, want in switches.items():
            got = bool(MEL.get_material_instance_static_switch_parameter_value(inst, pname))
            if got != want:
                problems.append("{0}.{1}: {2}, wanted {3}".format(name, pname, got, want))

        log("READBACK {0}: parent={1} vectors={2} scalars={3} switches={4}".format(
            name, got_parent,
            {p: tuple(round(float(c), 6) for c in (lambda g: (g.r, g.g, g.b, g.a))(
                MEL.get_material_instance_vector_parameter_value(inst, p))) for p in vectors},
            {p: round(float(MEL.get_material_instance_scalar_parameter_value(inst, p)), 6)
             for p in scalars},
            {p: bool(MEL.get_material_instance_static_switch_parameter_value(inst, p))
             for p in switches}))

    if problems:
        for p in problems:
            unreal.log_error("[Trace][MIC] FAIL " + p)
        fail("{0} readback assertion(s) failed".format(len(problems)))
    log("READBACK PASS: all {0} authored MICs verified.".format(len(MIC_TABLE)))


def main():
    author_all()
    read_back_and_assert()
    log("author_mics.py finished. Authored dir: {0}".format(AUTHORED_DIR))


try:
    main()
except Exception as exc:  # noqa: BLE001 - a commandlet needs the reason in the log
    unreal.log_error("[Trace][MIC] author_mics.py failed: {0}".format(exc))
    raise
