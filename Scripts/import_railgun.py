# =============================================================================
# Trace — import_railgun.py
#
# Runs INSIDE the editor (UnrealEditor-Cmd -run=pythonscript). Turns the OBJ
# files that Scripts/railgun_glb_to_obj.py derived from Art/Railgun/railgun.glb
# into committed StaticMesh assets, and builds the five materials the artist's
# glTF declares.
#
# Driven by Scripts/import-railgun.sh — do not invoke this by hand.
#
# WHY THREE MESHES AND NOT ONE
#   The weapon's fire animation throws the two rail walls apart and cants them
#   outward. A single static mesh cannot do that, so the source is split into a
#   body plus a left and right rail wall, each baked around ITS OWN HINGE: the
#   rear of the rail. Rotating a wall component about its own origin then swings
#   the muzzle end out, which is the motion the source notes describe. The
#   attach offsets that put each wall back in the right place are read out of
#   railgun_manifest.json rather than typed in here.
#
# WHY THE MATERIALS ARE BUILT AND NOT IMPORTED
#   An OBJ carries base colour and nothing else. The glTF's real PBR values
#   (metallic, roughness, emissive colour and emissive strength) live in the
#   manifest, and the two glowing materials need a scalar EmissiveIntensity
#   parameter that gameplay code animates on discharge. So one master material
#   is authored here and five instances are stamped from the manifest.
# =============================================================================
import json
import os

import unreal


PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBJ_DIR = os.environ.get(
    "TRACE_RAILGUN_OBJ_DIR", os.path.join(PROJECT_ROOT, "Intermediate", "Railgun"))
MANIFEST = os.path.join(OBJ_DIR, "railgun_manifest.json")

MESH_DIR = "/Game/Trace/Weapons/Meshes"
MATERIAL_DIR = "/Game/Trace/Weapons/Materials"
MASTER_NAME = "M_TraceRailgun"

FORCE = os.environ.get("TRACE_FORCE_RAILGUN", "0") == "1"

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary

_failures = []
_strays = []


def log(msg):
    unreal.log("[Trace] {0}".format(msg))


def fail(msg):
    _failures.append(msg)
    unreal.log_error("[Trace] {0}".format(msg))


# -----------------------------------------------------------------------------
# Master material
#
#   BaseColor        (vector)                       -> Base Color
#   Metallic         (scalar)                       -> Metallic
#   Roughness        (scalar)                       -> Roughness
#   EmissiveColor    (vector) * EmissiveIntensity   -> Emissive Color
#
# EmissiveIntensity is the knob the fire animation drives (1.0 at rest, up to
# 5.6x on discharge — see Art/Railgun/fire_curves.json).
# -----------------------------------------------------------------------------
def build_master():
    path = "{0}/{1}".format(MATERIAL_DIR, MASTER_NAME)
    if EAL.does_asset_exist(path):
        if not FORCE:
            log("{0} exists; keeping it (TRACE_FORCE_RAILGUN=1 rebuilds).".format(path))
            return unreal.load_asset(path)
        EAL.delete_asset(path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    mat = tools.create_asset(MASTER_NAME, MATERIAL_DIR, unreal.Material,
                             unreal.MaterialFactoryNew())
    if mat is None:
        fail("AssetTools refused to create {0}".format(path))
        return None

    def vec(name, value, x, y):
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
        e.set_editor_property("parameter_name", name)
        e.set_editor_property("default_value", value)
        e.set_editor_property("group", "Railgun")
        return e

    def scal(name, value, x, y):
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
        e.set_editor_property("parameter_name", name)
        e.set_editor_property("default_value", float(value))
        e.set_editor_property("group", "Railgun")
        return e

    def to_prop(expr, prop, label):
        if not MEL.connect_material_property(expr, "", prop):
            fail("could not connect {0} to material property".format(label))

    base = vec("BaseColor", unreal.LinearColor(0.5, 0.5, 0.5, 1.0), -600, -300)
    metal = scal("Metallic", 0.3, -600, -80)
    rough = scal("Roughness", 0.5, -600, 20)
    ecol = vec("EmissiveColor", unreal.LinearColor(0.0, 0.0, 0.0, 1.0), -600, 160)
    eint = scal("EmissiveIntensity", 1.0, -600, 280)

    mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -300, 200)
    if not MEL.connect_material_expressions(ecol, "", mul, "A"):
        fail("could not connect EmissiveColor into the emissive multiply")
    if not MEL.connect_material_expressions(eint, "", mul, "B"):
        fail("could not connect EmissiveIntensity into the emissive multiply")

    to_prop(base, unreal.MaterialProperty.MP_BASE_COLOR, "BaseColor")
    to_prop(metal, unreal.MaterialProperty.MP_METALLIC, "Metallic")
    to_prop(rough, unreal.MaterialProperty.MP_ROUGHNESS, "Roughness")
    to_prop(mul, unreal.MaterialProperty.MP_EMISSIVE_COLOR, "Emissive")

    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat, only_if_is_dirty=False)
    log("Built {0}".format(path))
    return mat


def build_instance(master, name, spec):
    path = "{0}/MI_Railgun_{1}".format(MATERIAL_DIR, name)
    if EAL.does_asset_exist(path):
        if not FORCE:
            return unreal.load_asset(path)
        EAL.delete_asset(path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    inst = tools.create_asset("MI_Railgun_{0}".format(name), MATERIAL_DIR,
                              unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
    if inst is None:
        fail("AssetTools refused to create {0}".format(path))
        return None

    MEL.set_material_instance_parent(inst, master)

    c = spec["base_color"]
    MEL.set_material_instance_vector_parameter_value(
        inst, "BaseColor", unreal.LinearColor(c[0], c[1], c[2], 1.0))
    MEL.set_material_instance_scalar_parameter_value(inst, "Metallic", float(spec["metallic"]))
    MEL.set_material_instance_scalar_parameter_value(inst, "Roughness", float(spec["roughness"]))

    e = spec["emissive"]
    # glTF splits glow into a colour and a separate KHR strength multiplier.
    # Fold the strength into the colour so EmissiveIntensity stays a clean
    # "1.0 = at rest" animation knob.
    s = float(spec.get("emissive_strength", 1.0))
    MEL.set_material_instance_vector_parameter_value(
        inst, "EmissiveColor", unreal.LinearColor(e[0] * s, e[1] * s, e[2] * s, 1.0))
    MEL.set_material_instance_scalar_parameter_value(inst, "EmissiveIntensity", 1.0)

    EAL.save_loaded_asset(inst, only_if_is_dirty=False)
    log("Built {0}".format(path))
    return inst


# -----------------------------------------------------------------------------
# Mesh import
# -----------------------------------------------------------------------------
def import_mesh(obj_name):
    obj_path = os.path.join(OBJ_DIR, obj_name + ".obj")
    if not os.path.isfile(obj_path):
        fail("missing OBJ: {0}".format(obj_path))
        return None

    game_path = "{0}/{1}".format(MESH_DIR, obj_name)
    if EAL.does_asset_exist(game_path):
        if not FORCE:
            log("{0} exists; keeping it (TRACE_FORCE_RAILGUN=1 rebuilds).".format(game_path))
            return unreal.load_asset(game_path)
        EAL.delete_asset(game_path)

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", obj_path)
    task.set_editor_property("destination_path", MESH_DIR)
    task.set_editor_property("destination_name", obj_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    produced = list(task.get_editor_property("imported_object_paths") or [])
    log("{0}: importer produced {1}".format(obj_name, produced))

    # The OBJ's companion .mtl makes Interchange author one throwaway UMaterial
    # per `usemtl` name, straight into the mesh folder. We overwrite every slot
    # with our own instances, so those are dead weight that would otherwise get
    # committed. Queue them for deletion AFTER the slots are repointed -- delete
    # them while the mesh still references them and the slots go null instead.
    for p in produced:
        asset_path = p.split(".")[0]
        stray = unreal.load_asset(asset_path)
        if stray is not None and not isinstance(stray, unreal.StaticMesh):
            _strays.append(asset_path)

    mesh = unreal.load_asset(game_path)
    if mesh is None:
        # Some importer versions name the asset after the OBJ's `o` line and
        # ignore destination_name; fall back to whatever it actually produced.
        for p in produced:
            candidate = unreal.load_asset(p.split(".")[0])
            if isinstance(candidate, unreal.StaticMesh):
                mesh = candidate
                break
    if not isinstance(mesh, unreal.StaticMesh):
        fail("import of {0} did not yield a StaticMesh".format(obj_name))
        return None
    return mesh


def assign_materials(mesh, instances):
    """Points every material slot at the instance whose name matches the slot."""
    statics = mesh.get_editor_property("static_materials")
    changed = 0
    for i, sm in enumerate(statics):
        slot = str(sm.get_editor_property("material_slot_name"))
        # Interchange decorates slot names ("shell", "M_shell", "shell_ncl1_1"),
        # so match on containment rather than equality.
        hit = None
        for key in instances:
            if key.lower() in slot.lower():
                hit = key
                break
        if hit is None:
            fail("{0} slot {1} '{2}' matched none of {3}".format(
                mesh.get_name(), i, slot, sorted(instances)))
            continue
        # StaticMesh.set_material(), not a write-back of the static_materials
        # array: mutating the struct copies that array hands out and setting the
        # list back is silently a no-op, which leaves every slot pointing at the
        # importer's throwaway materials.
        mesh.set_material(i, instances[hit])
        changed += 1
    EAL.save_loaded_asset(mesh, only_if_is_dirty=False)
    log("{0}: {1}/{2} slots assigned".format(mesh.get_name(), changed, len(statics)))
    return changed == len(statics) and len(statics) > 0


def main():
    if not os.path.isfile(MANIFEST):
        fail("manifest not found at {0} — run Scripts/railgun_glb_to_obj.py first".format(MANIFEST))
        return
    manifest = json.load(open(MANIFEST))

    master = build_master()
    if master is None:
        return

    instances = {}
    for name, spec in manifest["materials"].items():
        inst = build_instance(master, name, spec)
        if inst is not None:
            instances[name] = inst

    meshes = []
    for mesh_name in sorted(manifest["meshes"]):
        mesh = import_mesh("SM_Railgun_" + mesh_name)
        if mesh is not None:
            assign_materials(mesh, instances)
            meshes.append(mesh)

    for path in _strays:
        if EAL.does_asset_exist(path):
            EAL.delete_asset(path)
            log("removed importer-generated {0}".format(path))

    # Report what a reader of the log can check against the game: every slot,
    # the material actually on it, and the mesh's bounds.
    for mesh in meshes:
        box = mesh.get_bounding_box()
        log("{0}: bounds {1} .. {2}".format(mesh.get_name(), box.min, box.max))
        for i, sm in enumerate(mesh.get_editor_property("static_materials")):
            mi = sm.get_editor_property("material_interface")
            log("    slot {0} '{1}' -> {2}".format(
                i, sm.get_editor_property("material_slot_name"),
                mi.get_path_name() if mi else "NONE"))
            if mi is None:
                fail("{0} slot {1} ended up with no material".format(mesh.get_name(), i))

    if _failures:
        unreal.log_error("[Trace] import_railgun FAILED with {0} problem(s):".format(len(_failures)))
        for f in _failures:
            unreal.log_error("[Trace]   - {0}".format(f))
    else:
        log("import_railgun: all meshes and materials written.")


main()
