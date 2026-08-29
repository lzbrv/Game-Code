# =============================================================================
# Trace — import_characters.py
#
# Stage 2 of the character pipeline (PIPELINE_DESIGN.md §6): imports the ten
# generated GLB bodies from Intermediate/Characters/ through Interchange,
# renames them onto the contract paths, builds physics assets, binds the
# Shared/Materials instances per slot, and asserts the §6.5 readback contract.
#
#   Intermediate/Characters/<lowername>.glb (+ _manifest.json)   x10
#        |
#        v  (this script, inside UnrealEditor-Cmd, real RHI)
#   /Game/Trace/Characters/<Name>/SK_<Name>                      x10
#   /Game/Trace/Characters/<Name>/SK_<Name>_PhysicsAsset         x10
#   /Game/Trace/Characters/Shared/SK_TraceBody_Skeleton          x1
#
# Driven by Scripts/import-characters.sh --stage import — do not invoke the
# EDITOR path by hand (the shell pre-wipe is what makes renames land; see
# import_rocco.py sweep() for the Asset-Registry-ghost failure it prevents).
#
# TWO MODES:
#
#   python3 Scripts/import_characters.py --dry-run [--names A,B]
#       No editor, no unreal module, writes nothing. Validates everything
#       that can be validated from disk: manifest keys (§5.2), sha1, GLB
#       structure (one mesh / 5 primitives / material names in slot order /
#       one skin), the 26 joints against character_bodies.CANONICAL_SKELETON
#       (names, parents, positions within 0.5 uu through the MEASURED gl->UE
#       axis map), and that all 26 Shared/Materials .uasset files this
#       import binds are on disk. Exit code is real in this mode.
#
#   (editor) -run=pythonscript -script=Scripts/import_characters.py
#       The full §6 import. Runs the same dry-run validation first — the GLB
#       side of the contract is checked before any asset is touched.
#
# ORDER (§6.2): Rocco imports FIRST and creates the skeleton, which is renamed
# to Shared/SK_TraceBody_Skeleton; characters 2-10 import with the pipeline's
# `skeleton` property bound to it and must land ZERO new Skeleton assets
# (◆MEASURED behavior). The GLB joint tables are identical by construction —
# character_bodies.py forbids recipes from moving joints — and asserted here.
#
# SLOT BINDING (§6.4): the slot NAMES in the GLB are the binding contract:
#   suit      -> MI_Body_<Name>_Suit      (per-character roughness identity)
#   inset     -> MI_Body_Inset
#   team_glow -> MI_Body_Glow
#   accent    -> MI_Body_<Name>_Accent    (per-character hue)
#   suit_head -> MI_Body_SuitHead
# written back BY INDEX (unreal.Array iteration yields copies — the B1 trap,
# import_rocco.py:1196-1241) and read back off the SAVED asset.
#
# VERDICT: grep for "[import-characters] EXIT=0". In editor mode the process
# exit code is the engine's error count, not this script's verdict
# (generate-data-assets.py:38-43).
# =============================================================================

import hashlib
import json
import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
from character_bodies import CANONICAL_SKELETON, CHARACTER_ORDER, SLOT_ORDER  # noqa: E402

try:
    import unreal                                     # editor mode
except ImportError:
    unreal = None                                     # --dry-run under python3

PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
GLB_DIR = os.path.join(PROJECT_ROOT, "Intermediate", "Characters")
MATERIALS_FS_DIR = os.path.join(PROJECT_ROOT, "Content", "Trace", "Characters",
                                "Shared", "Materials")

CHAR_ROOT = "/Game/Trace/Characters"
MATERIAL_DIR = CHAR_ROOT + "/Shared/Materials"
SHARED_SKELETON_PATH = CHAR_ROOT + "/Shared/SK_TraceBody_Skeleton"
PIPELINE_DIR = CHAR_ROOT + "/_Pipelines"
DEFAULT_GLTF_ASSETS_PIPELINE = "/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline"
DEFAULT_GLTF_PIPELINE = "/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline"
MANNY_PATH = "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"

MANIFEST_KEYS = ("height_uu", "tris", "verts", "slots", "bones", "bounds_uu", "sha1")

SHARED_MI = {"suit_head": "MI_Body_SuitHead",
             "inset": "MI_Body_Inset",
             "team_glow": "MI_Body_Glow"}

PARENT_NAMES = ("M_TraceBodySuit", "M_TraceBodyGlow", "M_TraceBodyAccent")

# name -> (parent, component-space position) — the import-side contract copy.
CANONICAL = {name: (parent, pos) for (name, parent, pos) in CANONICAL_SKELETON}

BONE_POS_TOL_UU = 0.5
FACING_TOL_DEG = 2.0

_failures = []


def log(msg):
    if unreal is not None:
        unreal.log("[Trace] {0}".format(msg))
    else:
        print("[Trace] {0}".format(msg))


def fail(msg):
    _failures.append(msg)
    if unreal is not None:
        unreal.log_error("[Trace] {0}".format(msg))
    else:
        print("[Trace] PROBLEM: {0}".format(msg))


def mi_for_slot(character, slot):
    """§4.3 table. Unknown slot = hard FAIL upstream (GLB and script disagree)."""
    if slot == "suit":
        return "MI_Body_{0}_Suit".format(character)
    if slot == "accent":
        return "MI_Body_{0}_Accent".format(character)
    return SHARED_MI.get(slot)


def material_asset_names():
    names = list(PARENT_NAMES) + sorted(SHARED_MI.values())
    for c in CHARACTER_ORDER:
        names.append("MI_Body_{0}_Suit".format(c))
        names.append("MI_Body_{0}_Accent".format(c))
    return names                                       # 26


def glb_path(character):
    return os.path.join(GLB_DIR, character.lower() + ".glb")


def manifest_path(character):
    return os.path.join(GLB_DIR, character.lower() + "_manifest.json")


# -----------------------------------------------------------------------------
# GLB structural validation (pure python — shared by both modes).
#
# Only the 12-byte header and the JSON chunk are read; the binary chunk's
# geometry is already covered by verify_silhouettes.py's T0 (which re-reads it
# through railgun_glb_to_obj.py's own loader). What matters HERE is the part
# of the file the IMPORT contract hangs on: slots, skin, joints.
# -----------------------------------------------------------------------------

def read_glb_json(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 20:
        raise ValueError("truncated GLB ({0} bytes)".format(len(data)))
    magic, version, total = struct.unpack("<III", data[:12])
    if magic != 0x46546C67:
        raise ValueError("bad magic 0x{0:08X} (not glTF)".format(magic))
    if version != 2:
        raise ValueError("glTF version {0}, expected 2".format(version))
    if total != len(data):
        raise ValueError("header says {0} bytes, file has {1}".format(total, len(data)))
    clen, ctype = struct.unpack("<II", data[12:20])
    if ctype != 0x4E4F534A:
        raise ValueError("first chunk is not JSON")
    return json.loads(data[20:20 + clen].decode("utf-8"))


def glb_skeleton(gltf):
    """[(joint name, parent joint name, absolute UE-space position)] in skin order.

    Node translations are LOCAL; absolutes are accumulated down the node tree
    and mapped through the MEASURED axis conversion UE(X,Y,Z) = (gl.x, gl.z,
    gl.y) * 100 (trace_glb.py §2.1).
    """
    nodes = gltf["nodes"]
    parent_of = {}
    for i, node in enumerate(nodes):
        for child in node.get("children", ()):
            parent_of[child] = i

    def absolute(i):
        t = (0.0, 0.0, 0.0)
        while True:
            local = nodes[i].get("translation", (0.0, 0.0, 0.0))
            t = (t[0] + local[0], t[1] + local[1], t[2] + local[2])
            if i not in parent_of:
                return t
            i = parent_of[i]

    joints = gltf["skins"][0]["joints"]
    joint_set = set(joints)
    out = []
    for i in joints:
        parent_idx = parent_of.get(i)
        parent_name = (nodes[parent_idx]["name"]
                       if parent_idx is not None and parent_idx in joint_set else None)
        gl = absolute(i)
        out.append((nodes[i]["name"], parent_name,
                    (gl[0] * 100.0, gl[2] * 100.0, gl[1] * 100.0)))
    return out


def check_glb(character, manifest):
    """The import-side contract for one GLB. Appends to _failures; returns the
    parsed JSON on success (None on a parse-level failure)."""
    path = glb_path(character)
    prefix = "{0}: ".format(character)

    # sha1 first: the manifest describes THIS file or the pair is stale.
    digest = hashlib.sha1()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    if digest.hexdigest() != manifest.get("sha1"):
        fail(prefix + "GLB sha1 {0}.. != manifest sha1 {1}.. (stale pair — re-run "
             "generate_characters.py)".format(digest.hexdigest()[:12],
                                              str(manifest.get("sha1"))[:12]))
        return None

    try:
        gltf = read_glb_json(path)
    except (ValueError, KeyError, json.JSONDecodeError) as exc:
        fail(prefix + "GLB does not parse: {0}".format(exc))
        return None

    # One mesh, five primitives, material names ARE the slot names, in the
    # integrator slot order (the binding contract for mi_for_slot).
    meshes = gltf.get("meshes", ())
    if len(meshes) != 1:
        fail(prefix + "{0} meshes, expected 1".format(len(meshes)))
        return gltf
    prims = meshes[0].get("primitives", ())
    mat_names = [m.get("name") for m in gltf.get("materials", ())]
    if mat_names != list(SLOT_ORDER):
        fail(prefix + "material slots {0} != canonical order {1}".format(mat_names, list(SLOT_ORDER)))
    if [p.get("material") for p in prims] != list(range(len(SLOT_ORDER))):
        fail(prefix + "primitives do not reference materials 0..{0} in order".format(len(SLOT_ORDER) - 1))
    for slot in mat_names:
        if slot is not None and mi_for_slot(character, slot) is None:
            fail(prefix + "slot {0!r} has no MI mapping — the GLB and this script "
                 "disagree; someone edited one side".format(slot))

    if "KHR_materials_emissive_strength" not in gltf.get("extensionsUsed", ()):
        fail(prefix + "KHR_materials_emissive_strength missing from extensionsUsed")

    # One skin; 26 joints equal to CANONICAL_SKELETON: names, parents, and
    # absolute positions within 0.5 uu.
    skins = gltf.get("skins", ())
    if len(skins) != 1:
        fail(prefix + "{0} skins, expected 1".format(len(skins)))
        return gltf
    joints = glb_skeleton(gltf)
    if len(joints) != len(CANONICAL):
        fail(prefix + "{0} joints, expected {1}".format(len(joints), len(CANONICAL)))
    names = [j[0] for j in joints]
    if set(names) != set(CANONICAL):
        fail(prefix + "joint names differ from CANONICAL_SKELETON: extra={0} missing={1}"
             .format(sorted(set(names) - set(CANONICAL)), sorted(set(CANONICAL) - set(names))))
        return gltf
    for jname, jparent, pos in joints:
        want_parent, want_pos = CANONICAL[jname]
        if jparent != want_parent:
            fail(prefix + "joint {0}: parent {1!r} != {2!r}".format(jname, jparent, want_parent))
        err = max(abs(pos[k] - want_pos[k]) for k in range(3))
        if err > BONE_POS_TOL_UU:
            fail(prefix + "joint {0}: position off by {1:.3f} uu (limit {2})"
                 .format(jname, err, BONE_POS_TOL_UU))

    if manifest.get("bones") != len(CANONICAL):
        fail(prefix + "manifest bones={0!r}, expected {1}".format(manifest.get("bones"), len(CANONICAL)))
    return gltf


def check_manifest(character):
    path = manifest_path(character)
    try:
        with open(path, "r") as fh:
            manifest = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        fail("{0}: manifest does not parse: {1}".format(character, exc))
        return None
    missing = [k for k in MANIFEST_KEYS if k not in manifest]
    if missing:
        fail("{0}: manifest missing key(s) {1}".format(character, ", ".join(missing)))
        return None
    slots = manifest["slots"]
    if set(slots) != set(SLOT_ORDER):
        fail("{0}: manifest slots {1} != {2}".format(character, sorted(slots), sorted(SLOT_ORDER)))
    bounds = manifest["bounds_uu"]
    min_z = bounds["min"][2]
    max_z = bounds["max"][2]
    # Feet on the ground. NOTE: §6.5's literal "Z size within [174,178]"
    # predates crown-break geometry (Rocco's crest tops at 187.3, Lily's fins
    # at 207.7, ceiling Z 208 per LANGUAGE §4.8) — the honest gate is feet at
    # 0, height_uu (the non-crown height) at 176, and the imported bounds
    # matching THIS manifest (checked in editor mode).
    if not (-1.0 <= min_z <= 1.0):
        fail("{0}: bounds min Z {1:.2f} outside [-1, 1]".format(character, min_z))
    if not (174.0 <= max_z <= 208.5):
        fail("{0}: bounds max Z {1:.2f} outside [174, 208.5]".format(character, max_z))
    if not (175.5 <= float(manifest["height_uu"]) <= 176.7):
        fail("{0}: height_uu {1} not ~176".format(character, manifest["height_uu"]))
    return manifest


def check_material_files():
    """The 26 Shared/Materials assets this import binds must be on disk
    (stage `materials` runs before stage `import` — see import-characters.sh)."""
    missing = [n for n in material_asset_names()
               if not os.path.isfile(os.path.join(MATERIALS_FS_DIR, n + ".uasset"))]
    if missing:
        fail("Shared/Materials is missing {0} asset(s): {1} — run "
             "import-characters.sh --stage materials first"
             .format(len(missing), ", ".join(missing)))
    else:
        log("all 26 Shared/Materials assets present on disk")


def validate_from_disk(names, require_all):
    """The dry-run body. Returns {character: manifest} for the characters that
    validated; missing pairs are failures only under require_all."""
    check_material_files()
    validated = {}
    for character in names:
        have_glb = os.path.isfile(glb_path(character))
        have_manifest = os.path.isfile(manifest_path(character))
        if not (have_glb and have_manifest):
            msg = ("{0}: not generated yet (glb={1}, manifest={2})"
                   .format(character, have_glb, have_manifest))
            if require_all:
                fail(msg)
            else:
                log(msg + " — skipped (stage-0/W2-BODIES pending)")
            continue
        before = len(_failures)
        manifest = check_manifest(character)
        if manifest is not None:
            check_glb(character, manifest)
        if len(_failures) == before:
            log("{0}: manifest+GLB contract OK (tris={1} verts={2} bones={3} sha1={4}..)"
                .format(character, manifest["tris"], manifest["verts"],
                        manifest["bones"], manifest["sha1"][:12]))
            validated[character] = manifest
    return validated


# -----------------------------------------------------------------------------
# Editor mode — the §6 import. Everything below requires `unreal`.
# -----------------------------------------------------------------------------

def editor_list(folder):
    EAL = unreal.EditorAssetLibrary
    if not EAL.does_directory_exist(folder):
        return []
    return [p.split(".")[0] for p in
            EAL.list_assets(folder, recursive=True, include_folder=False)]


def sweep_check():
    """The shell wrapper must have wiped every .uasset under Characters/ except
    Shared/Materials BEFORE this session (Asset-Registry ghost rule — an asset
    surviving here means the renames below silently keep last run's mesh)."""
    EAL = unreal.EditorAssetLibrary
    survivors = [p for p in editor_list(CHAR_ROOT)
                 if not p.startswith(MATERIAL_DIR + "/")]
    if survivors:
        fail("{0} asset(s) survived the shell pre-wipe (e.g. {1}); run through "
             "import-characters.sh --stage import, not by hand"
             .format(len(survivors), survivors[0]))
    # In-session scratch a crashed earlier run may have left.
    if EAL.does_directory_exist(PIPELINE_DIR):
        EAL.delete_directory(PIPELINE_DIR)


def build_pipeline(name, skeleton_path):
    """§6.1: a configured duplicate of the engine glTF pipeline. All property
    names ◆MEASURED accepted (import_pack.py:178-283 is the proven shape)."""
    EAL = unreal.EditorAssetLibrary
    path = "{0}/{1}".format(PIPELINE_DIR, name)
    src = unreal.load_asset(DEFAULT_GLTF_ASSETS_PIPELINE)
    if src is None:
        fail("could not load {0}".format(DEFAULT_GLTF_ASSETS_PIPELINE))
        return None
    pipe = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(name, PIPELINE_DIR, src)
    if pipe is None:
        fail("could not duplicate the glTF pipeline to {0}".format(path))
        return None

    def prop(obj, pname, value):
        try:
            obj.set_editor_property(pname, value)
        except Exception as exc:                      # noqa: BLE001
            fail("pipeline property {0} = {1!r} rejected: {2}".format(pname, value, exc))

    prop(pipe, "import_offset_uniform_scale", 1.0)    # glTF metres already convert
    prop(pipe, "use_source_name_for_asset", True)
    prop(pipe, "scene_name_sub_folder", False)
    prop(pipe, "asset_type_sub_folders", False)
    common = pipe.get_editor_property("common_meshes_properties")
    prop(common, "force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_NONE)  # real skin
    prop(common, "bake_meshes", True)
    prop(common, "keep_sections_separate", False)     # one slot per material
    skelcommon = pipe.get_editor_property("common_skeletal_meshes_and_animations_properties")
    prop(skelcommon, "import_only_animations", False)
    prop(skelcommon, "use_t0_as_ref_pose", False)
    if skeleton_path is not None:
        skeleton = unreal.load_asset(skeleton_path)
        if skeleton is None:
            fail("shared skeleton {0} not loadable for pipeline binding".format(skeleton_path))
            return None
        prop(skelcommon, "skeleton", skeleton)
    mesh = pipe.get_editor_property("mesh_pipeline")
    prop(mesh, "import_static_meshes", True)
    prop(mesh, "import_skeletal_meshes", True)
    prop(mesh, "combine_skeletal_meshes_behavior",
         unreal.InterchangeCombineSkeletalMeshesBehavior.BY_SKELETON)
    prop(mesh, "create_physics_asset", False)         # built post-import (§6.3)
    prop(mesh, "build_nanite", False)
    anim = pipe.get_editor_property("animation_pipeline")
    prop(anim, "import_animations", False)
    mat = pipe.get_editor_property("material_pipeline")
    prop(mat, "import_materials", False)              # stamped, not imported
    tex = mat.get_editor_property("texture_pipeline")
    if tex is not None:
        prop(tex, "import_textures", False)
    EAL.save_loaded_asset(pipe, only_if_is_dirty=False)
    return path


def run_import(glb, dest, pipeline_path):
    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    source = unreal.InterchangeManager.create_source_data(glb)
    params = unreal.ImportAssetParameters()
    params.set_editor_property("is_automated", True)
    params.set_editor_property("override_pipelines",
                               [unreal.SoftObjectPath(pipeline_path),
                                unreal.SoftObjectPath(DEFAULT_GLTF_PIPELINE)])
    im.import_asset(dest, source, params)
    # The return value is an opaque Array (◆MEASURED) — classify the folder.


def classify(folder):
    kinds = {}
    for path in editor_list(folder):
        asset = unreal.load_asset(path)
        kinds.setdefault(type(asset).__name__ if asset else "<unloadable>", []).append(path)
    return kinds


def rename_asset(src, dst):
    if not unreal.EditorAssetLibrary.rename_asset(src, dst):
        fail("rename {0} -> {1} failed".format(src, dst))
        return False
    return True


def editor_world():
    try:
        return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    except Exception:                                 # noqa: BLE001
        return None


def probe_bones(mesh):
    """Bone table off a throwaway SkeletalMeshActor component — the only
    headless way to read names/parents/component-space positions
    (import_rocco.py:907-925, 960-1045)."""
    if editor_world() is None:
        fail("no editor world — bone probe impossible")
        return {}, []
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(
        unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    if actor is None:
        fail("could not spawn probe SkeletalMeshActor")
        return {}, []
    table, order = {}, []
    try:
        comp = actor.skeletal_mesh_component
        comp.set_skeletal_mesh_asset(mesh)
        for i in range(comp.get_num_bones()):
            name = str(comp.get_bone_name(i))
            t = comp.get_socket_transform(name, unreal.RelativeTransformSpace.RTS_COMPONENT)
            table[name] = (t.translation, str(comp.get_parent_bone(name)))
            order.append(name)
        table["__hand_r_socket__"] = comp.does_socket_exist("hand_r")
    except Exception as exc:                          # noqa: BLE001
        fail("bone probe failed: {0}".format(exc))
    finally:
        subsystem.destroy_actor(actor)
    return table, order


def facing_heading(table):
    """Heading (deg off +X) via L = left hand - right hand, forward = Z x L —
    the rig-agnostic measurement of import_rocco.py:1058-1097. MeshYaw needed
    is -heading; the generated bodies author facing +Y, so expect -90."""
    if "hand_l" not in table or "hand_r" not in table:
        return None
    lv, rv = table["hand_l"][0], table["hand_r"][0]
    lx, ly = lv.x - rv.x, lv.y - rv.y
    fx, fy = -ly, lx
    span = (fx * fx + fy * fy) ** 0.5
    if span < 1e-3:
        return None
    import math
    return math.degrees(math.atan2(fy / span, fx / span))


def facing_harness_selfcheck():
    """The harness proves itself on the Mannequin (known answer: MeshYaw -90)
    before it is trusted on a generated body. Manny may legitimately be absent
    on a fresh clone (§11 row 13) — that skips the self-check, loudly."""
    manny = unreal.load_asset(MANNY_PATH)
    if manny is None:
        log("NOTE: {0} absent — facing harness self-check skipped".format(MANNY_PATH))
        return
    table, _ = probe_bones(manny)
    heading = facing_heading(table)
    if heading is None or abs(-heading - (-90.0)) > FACING_TOL_DEG:
        fail("facing harness self-check on Manny returned {0!r}, expected MeshYaw -90"
             .format(None if heading is None else -heading))
    else:
        log("facing harness self-check on Manny: MeshYaw {0:+.1f} (expected -90)".format(-heading))


def bind_slots(character, mesh_path):
    """§6.4 index-write-back + saved-asset readback."""
    EAL = unreal.EditorAssetLibrary
    mesh = unreal.load_asset(mesh_path)
    arr = mesh.get_editor_property("materials")
    wanted = []
    for i in range(len(arr)):
        slot = arr[i]
        name = str(slot.get_editor_property("material_slot_name"))
        mi_name = mi_for_slot(character, name)
        if mi_name is None:
            fail("{0}: unknown slot {1!r} — GLB and script disagree".format(character, name))
            return
        mi = unreal.load_asset("{0}/{1}".format(MATERIAL_DIR, mi_name))
        if mi is None:
            fail("{0}: {1} not loadable".format(character, mi_name))
            return
        slot.set_editor_property("material_interface", mi)
        arr[i] = slot                                 # <- the load-bearing line (B1)
        wanted.append((name, mi))
    mesh.set_editor_property("materials", arr)
    EAL.save_loaded_asset(mesh, only_if_is_dirty=False)

    landed = unreal.load_asset(mesh_path).get_editor_property("materials")
    wrong = [wanted[i][0] for i in range(len(landed))
             if landed[i].get_editor_property("material_interface") != wanted[i][1]]
    if wrong:
        fail("{0}: {1} slot(s) did not take their instance: {2}"
             .format(character, len(wrong), ", ".join(wrong)))


def assert_character(character, manifest, expect_skeleton):
    """§6.5 readback assertions for one imported character."""
    EAL = unreal.EditorAssetLibrary
    folder = "{0}/{1}".format(CHAR_ROOT, character)
    mesh_path = "{0}/SK_{1}".format(folder, character)
    mesh = unreal.load_asset(mesh_path)
    if mesh is None:
        fail("{0}: {1} missing".format(character, mesh_path))
        return

    # Bones == CANONICAL_SKELETON: 26 names, same parents, positions <= 0.5 uu.
    table, order = probe_bones(mesh)
    bone_names = [n for n in order]
    if set(bone_names) != set(CANONICAL):
        fail("{0}: bone set differs: extra={1} missing={2}".format(
            character, sorted(set(bone_names) - set(CANONICAL)),
            sorted(set(CANONICAL) - set(bone_names))))
    else:
        for name in bone_names:
            pos, parent = table[name]
            want_parent, want_pos = CANONICAL[name]
            parent_normalized = parent if parent not in ("", "None") else None
            if parent_normalized != want_parent:
                fail("{0}: bone {1} parent {2!r} != {3!r}".format(character, name, parent, want_parent))
            err = max(abs(pos.x - want_pos[0]), abs(pos.y - want_pos[1]), abs(pos.z - want_pos[2]))
            if err > BONE_POS_TOL_UU:
                fail("{0}: bone {1} off by {2:.3f} uu".format(character, name, err))

    # Knife attach (TraceWeaponComponent.cpp:4004-4015 looks up "hand_r").
    if not table.get("__hand_r_socket__", False):
        fail("{0}: does_socket_exist('hand_r') is FALSE — the knife would be invisible".format(character))

    # Facing: authored +Y => MeshYaw -90 +/- 2.
    heading = facing_heading(table)
    if heading is None:
        fail("{0}: facing unmeasurable".format(character))
    elif abs(-heading - (-90.0)) > FACING_TOL_DEG:
        fail("{0}: MeshYaw needed {1:+.1f}, expected -90 +/- {2}".format(character, -heading, FACING_TOL_DEG))

    # Bounds: feet at 0, and the imported box matches the generator's manifest
    # (crown-break parts legally top out above 176 — see check_manifest note).
    try:
        b = mesh.get_bounds()
        min_z = b.origin.z - b.box_extent.z
        z_size = b.box_extent.z * 2.0
        want = manifest["bounds_uu"]
        want_size = want["max"][2] - want["min"][2]
        if not (-1.0 <= min_z <= 1.0):
            fail("{0}: bounds min Z {1:.2f} outside [-1, 1]".format(character, min_z))
        if abs(z_size - want_size) > 1.0:
            fail("{0}: bounds Z size {1:.2f} != manifest {2:.2f} (+/- 1)".format(character, z_size, want_size))
    except Exception as exc:                          # noqa: BLE001
        fail("{0}: bounds unreadable: {1}".format(character, exc))

    # Manifest cross-check: triangles (exact) + verts (weld window) + slot census.
    #
    # W3-CHARPIPE ADJUDICATION (2026-08-24; this replaces the exact-equal vert
    # test §6.5 asked for, which failed 10/10 on the real import — the case
    # W2-CHARPREP's deviation #4 left for this wave to decide with evidence):
    #
    #   USkeletalMeshEditorSubsystem::GetNumVerts returns
    #   LODRenderData[0].GetNumVertices() (SkeletalMeshEditorSubsystem.cpp:79-86)
    #   — the count AFTER the skeletal-mesh builder packs render vertices. The
    #   manifest counts the GLB's attribute vertices. They are different
    #   quantities and the builder is allowed to weld, so exact equality was
    #   never the right assertion. ◆MEASURED on all ten bodies (probe archived at
    #   scratchpad/release-impl/frames-W3-CHARPIPE/weld_probe.py): the render
    #   count lands strictly between the GLB's attribute-UNIQUE count and its raw
    #   count — Rocco 2296 < 2560 < 2706 — i.e. the builder welds exact duplicate
    #   corners but keeps corners whose computed (MikkTSpace) tangents differ.
    #   Nothing is lost: the TRIANGLE count is identical to the manifest on all
    #   ten, and the imported bounds already match the manifest to ±1 uu above.
    #
    # So the honest pair of assertions is: triangles EXACTLY equal (no geometry
    # may vanish), and verts may only ever go DOWN (a builder welds; it never
    # invents). Triangles come off the asset-registry "Triangles" tag, which
    # USkeletalMesh fills from LODData.GetTotalFaces() (SkeletalMesh.cpp:4568-4575)
    # — the only triangle count reachable from python.
    try:
        verts = int(unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem).get_num_verts(mesh, 0))
        want_verts = int(manifest["verts"])
        if verts > want_verts:
            fail("{0}: imported verts {1} EXCEEDS manifest {2} — the builder cannot "
                 "invent vertices, so the GLB and the manifest disagree"
                 .format(character, verts, want_verts))
        elif verts <= 0:
            fail("{0}: imported verts {1} — nothing was built".format(character, verts))
        else:
            log("  {0}: verts {1} of {2} after the builder's weld ({3:.1f}%), "
                "tris below".format(character, verts, want_verts, 100.0 * verts / want_verts))
    except Exception as exc:                          # noqa: BLE001
        fail("{0}: vert count unreadable: {1}".format(character, exc))
    try:
        data = unreal.AssetRegistryHelpers.get_asset_registry().get_asset_by_object_path(
            "{0}.SK_{1}".format(mesh_path, character))
        tag = data.get_tag_value("Triangles")
        if tag is None or str(tag) == "":
            fail("{0}: asset-registry 'Triangles' tag is empty — cannot confirm the "
                 "geometry survived the import".format(character))
        elif int(tag) != int(manifest["tris"]):
            fail("{0}: imported tris {1} != manifest {2} — geometry was lost or added"
                 .format(character, int(tag), manifest["tris"]))
        else:
            log("  {0}: tris {1} == manifest".format(character, int(tag)))
    except Exception as exc:                          # noqa: BLE001
        fail("{0}: triangle count unreadable: {1}".format(character, exc))
    slots = [str(s.get_editor_property("material_slot_name"))
             for s in mesh.get_editor_property("materials")]
    if sorted(slots) != sorted(SLOT_ORDER):
        fail("{0}: slot names {1} != {2}".format(character, slots, list(SLOT_ORDER)))

    # Physics asset at the contract path, wired to the mesh.
    physics_path = "{0}/SK_{1}_PhysicsAsset".format(folder, character)
    if not EAL.does_asset_exist(physics_path):
        fail("{0}: {1} missing".format(character, physics_path))
    elif mesh.get_editor_property("physics_asset") is None:
        fail("{0}: mesh has no physics_asset wired".format(character))

    # Skeleton census: Rocco created THE skeleton; the other nine bind to it.
    skel = mesh.get_editor_property("skeleton")
    if skel is None or skel.get_path_name().split(".")[0] != SHARED_SKELETON_PATH:
        fail("{0}: skeleton is {1}, expected {2}".format(
            character, skel.get_path_name().split(".")[0] if skel else None, SHARED_SKELETON_PATH))


def import_one(character, pipeline_path, expect_skeleton):
    """Import + §6.2 renames + §6.3 physics for one character."""
    EAL = unreal.EditorAssetLibrary
    folder = "{0}/{1}".format(CHAR_ROOT, character)
    staging = folder + "/_Import"
    if EAL.does_directory_exist(staging):
        EAL.delete_directory(staging)

    run_import(glb_path(character), staging, pipeline_path)
    kinds = classify(staging)
    meshes = kinds.get("SkeletalMesh", [])
    skeletons = kinds.get("Skeleton", [])
    if len(meshes) != 1:
        fail("{0}: import produced {1} SkeletalMesh(es), expected 1 ({2})"
             .format(character, len(meshes),
                     ", ".join("{0} x{1}".format(k, len(v)) for k, v in sorted(kinds.items())) or "NOTHING"))
        return False
    if expect_skeleton and len(skeletons) != 1:
        fail("{0}: import produced {1} Skeleton(s), expected 1 (creates the shared skeleton)"
             .format(character, len(skeletons)))
        return False
    if not expect_skeleton and skeletons:
        # ◆MEASURED: a bound `skeleton` property must land ZERO new Skeletons.
        fail("{0}: import produced {1} extra Skeleton(s) despite the bound skeleton "
             "property — the GLB's joints drifted from the shared skeleton"
             .format(character, len(skeletons)))
        return False

    mesh_path = "{0}/SK_{1}".format(folder, character)
    if not rename_asset(meshes[0], mesh_path):
        return False
    if expect_skeleton:
        if not rename_asset(skeletons[0], SHARED_SKELETON_PATH):
            return False
        # Re-save the mesh now the skeleton has moved: an interrupted run
        # otherwise leaves the mesh pointing at a dead path and it loads with
        # a null Skeleton (import_rocco.py:1160-1167, learned the hard way).
        EAL.save_loaded_asset(unreal.load_asset(mesh_path), only_if_is_dirty=False)

    # §6.3 physics after the renames, so its bodies are named for the final asset.
    mesh = unreal.load_asset(mesh_path)
    physics = None
    try:
        physics = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem).create_physics_asset(mesh)
    except Exception as exc:                          # noqa: BLE001
        fail("{0}: create_physics_asset failed: {1}".format(character, exc))
    if physics is not None:
        landed = physics.get_path_name().split(".")[0]
        wanted = "{0}/SK_{1}_PhysicsAsset".format(folder, character)
        if landed != wanted:
            fail("{0}: physics asset landed at {1}, not {2}".format(character, landed, wanted))
        mesh.set_editor_property("physics_asset", physics)
        # W3-CHARPIPE FIX (2026-08-24): save the PHYSICS package too, not just
        # the mesh that points at it. create_physics_asset() registers the new
        # asset and leaves it dirty in memory, so every in-session check
        # (does_asset_exist, the mesh's physics_asset property) passed while
        # NOTHING reached disk — ◆MEASURED: the first full import run landed ten
        # meshes and zero SK_<Name>_PhysicsAsset.uasset files, and only the
        # wrapper's disk census caught it. The mesh save must come after, so the
        # reference it stores points at a package that exists.
        EAL.save_loaded_asset(physics, only_if_is_dirty=False)
        EAL.save_loaded_asset(mesh, only_if_is_dirty=False)
    return True


def sweep_strays(character):
    """Delete rename redirectors; anything else unexpected is a failure
    (the import-rocco.sh idempotence promise, kept per folder)."""
    EAL = unreal.EditorAssetLibrary
    folder = "{0}/{1}".format(CHAR_ROOT, character)
    keep = {"{0}/SK_{1}".format(folder, character),
            "{0}/SK_{1}_PhysicsAsset".format(folder, character)}
    for path in editor_list(folder):
        if path in keep:
            continue
        if EAL.delete_asset(path):
            log("{0}: swept stray {1}".format(character, path.rsplit("/", 1)[-1]))
        else:
            fail("{0}: could not delete stray {1}".format(character, path))


def main_editor(names):
    validated = validate_from_disk(names, require_all=True)
    if _failures:
        report()
        return

    sweep_check()
    facing_harness_selfcheck()
    if _failures:
        report()
        return

    order = [c for c in CHARACTER_ORDER if c in validated]
    if order[0] != "Rocco":
        fail("Rocco must import first (creates the shared skeleton); got {0}".format(order))
        report()
        return

    # Rocco: unbound pipeline; creates + donates the skeleton.
    pipe_rocco = build_pipeline("TraceBodyGLTF", None)
    if pipe_rocco is None or not import_one("Rocco", pipe_rocco, expect_skeleton=True):
        report()
        return

    # Characters 2..10: skeleton-bound pipeline (a SECOND asset name — deleting
    # and recreating one name in-session trips the Asset Registry ghost rule).
    pipe_bound = build_pipeline("TraceBodyGLTF_Bound", SHARED_SKELETON_PATH)
    if pipe_bound is None:
        report()
        return
    for character in order[1:]:
        import_one(character, pipe_bound, expect_skeleton=False)

    for character in order:
        bind_slots(character, "{0}/{1}/SK_{1}".format(CHAR_ROOT, character))

    for character in order:
        assert_character(character, validated[character],
                         expect_skeleton=(character == "Rocco"))
        sweep_strays(character)

    # Shared/ census: the skeleton plus the 26 materials, nothing else.
    EAL = unreal.EditorAssetLibrary
    if EAL.does_directory_exist(PIPELINE_DIR):
        EAL.delete_directory(PIPELINE_DIR)
    shared = set(editor_list(CHAR_ROOT + "/Shared"))
    expected_shared = {SHARED_SKELETON_PATH} | {
        "{0}/{1}".format(MATERIAL_DIR, n) for n in material_asset_names()}
    for stray in sorted(shared - expected_shared):
        if EAL.delete_asset(stray):
            log("Shared: swept stray {0}".format(stray.rsplit("/", 1)[-1]))
        else:
            fail("Shared: unexpected asset {0}".format(stray))
    for missing in sorted(expected_shared - shared):
        fail("Shared: missing {0}".format(missing))

    report()


def report():
    log("")
    log("================ CHARACTER IMPORT REPORT ================")
    if _failures:
        log("{0} PROBLEM(S):".format(len(_failures)))
        for f in _failures:
            log("  - {0}".format(f))
    else:
        log("no problems reported.")
    log("================ END REPORT ================")
    log("[import-characters] EXIT={0}".format(1 if _failures else 0))


def main():
    args = sys.argv[1:]
    names = list(CHARACTER_ORDER)
    if "--names" in args:
        picked = args[args.index("--names") + 1].split(",")
        unknown = [n for n in picked if n not in CHARACTER_ORDER]
        if unknown:
            print("[Trace] unknown character name(s): {0}".format(", ".join(unknown)))
            sys.exit(2)
        names = [c for c in CHARACTER_ORDER if c in picked]

    dry = "--dry-run" in args or os.environ.get("TRACE_IMPORT_DRY_RUN") == "1"
    if dry or unreal is None:
        if not dry:
            print("[Trace] no unreal module and no --dry-run: refusing to guess. "
                  "Run inside UnrealEditor-Cmd or pass --dry-run.")
            sys.exit(2)
        validated = validate_from_disk(names, require_all=False)
        log("DRY-RUN validated {0}/{1} character(s): {2}".format(
            len(validated), len(names), ", ".join(sorted(validated)) or "none"))
        if not validated:
            fail("no character validated — nothing in {0}".format(GLB_DIR))
        report()
        sys.exit(1 if _failures else 0)

    main_editor(names)


main()
