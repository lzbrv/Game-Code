#!/usr/bin/env python3
# =============================================================================
# Trace — regularize_physics.py
#
# The §4.8 / T6 pass of the character pipeline (CHARACTER_LANGUAGE.md §4.8,
# acceptance test T6): after import_characters.py has built ten skeletal meshes
# on the shared skeleton, this replaces their auto-generated physics assets with
# ONE uniform hit model — the same fifteen bodies, the same dimensions, on every
# character.  "Ten characters, one hit model — the roster differentiates in
# shape, never in hittable volume, and headshot feel is identical on every
# helmet."
#
# WHY THIS NEEDS A DONOR MESH (read before simplifying it away)
#
#   ◆MEASURED 2026-08-24 (UE 5.8, probe archived at
#   scratchpad/release-impl/frames-W3-CHARPIPE/phys_probe{2,3}.py):
#   `UPhysicsAsset::SkeletalBodySetups`, `ConstraintSetup` and `BoundsBodies`
#   are plain `UPROPERTY()`s with no CPF_Edit flag, so editor python cannot see
#   them at all — `get_editor_property('skeletal_body_setups')` raises
#   "Failed to find property".  `USkeletalBodySetup::BoneName` is VisibleAnywhere
#   and therefore READ-ONLY to python.  `FPhysicsAssetUtils::CreateNewBody` /
#   `DestroyBody` are C++ only (no UFUNCTION).  Net effect: **python can rewrite
#   the geometry of a physics body but can neither add nor remove one, nor move
#   one to a different bone.**
#
#   And the auto-generated tables are NOT uniform — the same measurement run:
#       Rocco/Mace/Oyster/X/Roxie/Elle/Slimeball  14 bodies
#       Chut 15 (an extra spine_01), Mortimer 15 (clavicles, no spine_05),
#       Lily 12 (no upperarms, no feet)
#       none of the ten had spine_03, hand_l or hand_r
#   because `FPhysicsAssetUtils` only bodies a bone whose weighted vertex cloud
#   measures > MinBoneSize (20 uu, PhysicsAssetUtils.h:44-50), and the generated
#   bodies hang their geometry off whichever bone the recipe chose.  That is
#   precisely the "hit volume follows the art" failure §4.8 exists to forbid.
#
#   So the hit model is authored as GEOMETRY instead: a donor body — one 30 uu
#   cube rigidly bound to each of the fifteen bones the table names, and nothing
#   bound anywhere else.  `create_physics_asset` on that mesh therefore produces
#   exactly fifteen bodies, on exactly the right bones, for any character,
#   because the donor's geometry (not the character's) decided them.  Every
#   body's `agg_geom` is then overwritten with the §4.8 numbers, and the finished
#   asset is DUPLICATED onto the ten `SK_<Name>_PhysicsAsset` contract paths.
#   Duplicates of one master is the strongest possible form of "byte-identical
#   across all ten": there is only ever one table.
#
#   The donor mesh is KEPT at /Game/Trace/Characters/Shared/SK_TraceBodyHitDonor
#   on purpose.  `FPhysicsAssetUtils::CreateFromSkeletalMesh` writes
#   `PreviewSkeletalMesh = SkelMesh` unconditionally (PhysicsAssetUtils.cpp:608)
#   and that property is not python-writable either, so deleting the donor would
#   leave eleven physics assets pointing at a package that no longer exists.  It
#   is editor-only data (WITH_EDITORONLY_DATA), so it is never cooked; and it
#   doubles as documentation — open it and you are looking at the hit model.
#
# THE TABLE (CHARACTER_LANGUAGE §4.8, verbatim)
#
#   head sphere r 15 at head +10; pelvis sphyl r 19 x 14; spine_03 sphyl
#   r 18 x 20; upperarm sphyl r 7 x 22; lowerarm r 7 x 20; hand sphere r 7;
#   thigh sphyl r 10 x 34; calf sphyl r 8 x 30; foot box 14 x 30 x 10.
#
#   Two things the prose leaves to the implementer, decided here and stated so
#   the next reader does not have to guess:
#     * "r R x L" is read in ENGINE terms, because "sphyl" is an engine word:
#       FKSphylElem.Radius = R, FKSphylElem.Length = L, and Length is the
#       CYLINDER segment, so the capsule's total length is L + 2R
#       (SphylElem.h:35-40).  Limb capsules therefore overhang their bone a
#       little, which is what keeps the joint cores gapless (T8).
#     * Placement.  Limb capsules (upperarm/lowerarm/thigh/calf) are centred on
#       the midpoint of bone->child and aligned to that direction — the standard
#       PhAT layout, and the only one that cannot open a hole at a joint.  The
#       torso capsules (pelvis, spine_03) sit ON their joint, aligned to their
#       child.  Hand spheres sit on the hand joint.  Foot boxes are centred on
#       the midpoint foot->ball, axis-aligned (X lateral 14, Y fore-aft 30,
#       Z 10) because the rig authors +Y forward.  Head is the one the prose
#       fixes outright: sphere centre 10 uu above the head joint.
#
#   Every number is derived from CANONICAL_SKELETON, which all ten share
#   byte-identically, so the table is the same for all ten by construction.
#
# CONSTRAINTS.  The donor's constraints are whatever `create_physics_asset`
# builds between the fifteen bodies (bCreateConstraints defaults true) and they
# are duplicated with everything else, so the ten are identical there too.  They
# are inert in this game: ATraceCharacter puts the body mesh on
# ECollisionEnabled::NoCollision (TraceCharacter.cpp:1968, 2068) and nothing in
# Source/ ever simulates a physics asset, so hits are capsule hits today.  The
# table is the forward-looking contract §4.8 asks for, not live behaviour.
#
# USAGE (the shell pre-wipe is load-bearing — the Asset-Registry-ghost rule that
# Scripts/import-rocco.sh and import_characters.py both document):
#
#   # 1. pre-wipe, BEFORE the editor starts
#   find Content/Trace/Characters -name 'SK_*_PhysicsAsset.uasset' -delete
#   rm -f Content/Trace/Characters/Shared/SK_TraceBodyHitDonor.uasset
#   # 2. write pass
#   UnrealEditor-Cmd Trace.uproject -run=pythonscript \
#       -script=Scripts/regularize_physics.py -unattended -nosplash -nopause \
#       -nosound -stdout -FullStdOutLogOutput -RenderOffScreen -abslog=...
#   # 3. verify pass — fresh editor, reads the SAVED assets off disk
#   TRACE_PHYSICS_VERIFY=1 UnrealEditor-Cmd ... (same line)
#
#   python3 Scripts/regularize_physics.py --emit-donor
#       No editor: writes the donor GLB and prints the resolved hit table.
#
# VERDICT: grep for "[regularize-physics] EXIT=0" — the process exit code is the
# engine's error count, not this script's verdict (generate-data-assets.py:38-43).
# =============================================================================

import hashlib
import math
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
from character_bodies import CANONICAL_SKELETON, CHARACTER_ORDER, SLOT_ORDER  # noqa: E402
from trace_glb import TraceGlbWriter, box  # noqa: E402

try:
    import unreal                                     # editor mode
except ImportError:
    unreal = None                                     # --emit-donor under python3

PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
GLB_DIR = os.path.join(PROJECT_ROOT, "Intermediate", "Characters")
DONOR_GLB = os.path.join(GLB_DIR, "_hitdonor.glb")

CHAR_ROOT = "/Game/Trace/Characters"
SHARED_DIR = CHAR_ROOT + "/Shared"
SHARED_SKELETON_PATH = SHARED_DIR + "/SK_TraceBody_Skeleton"
DONOR_MESH_PATH = SHARED_DIR + "/SK_TraceBodyHitDonor"
DONOR_PHYS_PATH = DONOR_MESH_PATH + "_PhysicsAsset"
DONOR_STAGING = SHARED_DIR + "/_HitDonorImport"
PIPELINE_DIR = CHAR_ROOT + "/_Pipelines"
DONOR_PIPELINE = PIPELINE_DIR + "/TraceHitDonorGLTF"
DEFAULT_GLTF_ASSETS_PIPELINE = "/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline"
DEFAULT_GLTF_PIPELINE = "/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline"

SKEL = {name: pos for (name, _p, pos) in CANONICAL_SKELETON}
PARENT = {name: parent for (name, parent, _pos) in CANONICAL_SKELETON}

# The donor cube's edge.  CalcBoneInfoLength (PhysicsAssetUtils.cpp:160-172)
# measures a bone's vertex cloud as |BoxExtent|, so a cube of edge E scores
# E/2 * sqrt(3); MinBoneSize is 20, and 30 -> 25.98 clears it with room to
# spare while staying small enough that fifteen of them are a ~360-tri mesh.
DONOR_CUBE_UU = 30.0

# --- the §4.8 hit table, as placement rules over CANONICAL_SKELETON ----------
# kind:   "sphere"  (radius, offset)         offset is component-space uu
#         "sphyl"   (radius, length, child)  centred/aligned per the header
#         "box"     (x, y, z, child)         axis-aligned, centred bone->child
HIT_TABLE = {
    "head":       ("sphere", {"radius": 15.0, "offset": (0.0, 0.0, 10.0)}),
    "pelvis":     ("sphyl",  {"radius": 19.0, "length": 14.0, "child": "spine_01",
                              "at_joint": True}),
    "spine_03":   ("sphyl",  {"radius": 18.0, "length": 20.0, "child": "spine_04",
                              "at_joint": True}),
    "upperarm_l": ("sphyl",  {"radius": 7.0, "length": 22.0, "child": "lowerarm_l"}),
    "upperarm_r": ("sphyl",  {"radius": 7.0, "length": 22.0, "child": "lowerarm_r"}),
    "lowerarm_l": ("sphyl",  {"radius": 7.0, "length": 20.0, "child": "hand_l"}),
    "lowerarm_r": ("sphyl",  {"radius": 7.0, "length": 20.0, "child": "hand_r"}),
    "hand_l":     ("sphere", {"radius": 7.0, "offset": (0.0, 0.0, 0.0)}),
    "hand_r":     ("sphere", {"radius": 7.0, "offset": (0.0, 0.0, 0.0)}),
    "thigh_l":    ("sphyl",  {"radius": 10.0, "length": 34.0, "child": "calf_l"}),
    "thigh_r":    ("sphyl",  {"radius": 10.0, "length": 34.0, "child": "calf_r"}),
    "calf_l":     ("sphyl",  {"radius": 8.0, "length": 30.0, "child": "foot_l"}),
    "calf_r":     ("sphyl",  {"radius": 8.0, "length": 30.0, "child": "foot_r"}),
    "foot_l":     ("box",    {"x": 14.0, "y": 30.0, "z": 10.0, "child": "ball_l"}),
    "foot_r":     ("box",    {"x": 14.0, "y": 30.0, "z": 10.0, "child": "ball_r"}),
}
HIT_BONES = tuple(sorted(HIT_TABLE))

# Material slots the runtime binds by name.  The donor is never rendered, so it
# carries one slot; it is named for what it is.
DONOR_SLOT = "hit"

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
        print("[Trace] ERROR: {0}".format(msg))


# ---------------------------------------------------------------------------
# the table, resolved against the canonical skeleton (pure python — the same
# arithmetic in both modes, so --emit-donor prints exactly what the editor writes)
# ---------------------------------------------------------------------------

def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _mid(a, b):
    return (0.5 * (a[0] + b[0]), 0.5 * (a[1] + b[1]), 0.5 * (a[2] + b[2]))


def _len(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def rot_from_z(d):
    """(pitch, yaw, roll) in degrees for the rotation taking +Z onto d.

    FRotator's own convention: a rotator's +Z (up) axis is
    (-sin(pitch)*cos(yaw)... ) — rather than re-derive that, build the rotation
    as yaw about Z then pitch about Y, which is exactly how FRotationMatrix
    composes, and which UKismetMathLibrary::MakeRotFromZ produces for a rig with
    no roll about the bone.  Roll is 0: a capsule is rotationally symmetric
    about its own axis, so any roll is unobservable in the hit volume.
    """
    n = _len(d)
    if n < 1e-9:
        return (0.0, 0.0, 0.0)
    x, y, z = d[0] / n, d[1] / n, d[2] / n
    # UE: yaw rotates +X toward +Y about Z; pitch rotates +X toward +Z about Y.
    # A rotator's local +Z is what we must land on d.  Local +Z starts at world
    # +Z; pitching by -90 puts it on +X, so pitch = -90 + elevation, and yaw is
    # the compass angle of the horizontal part.
    yaw = math.degrees(math.atan2(y, x))
    horiz = math.sqrt(x * x + y * y)
    elev = math.degrees(math.atan2(z, horiz))         # +90 when d == +Z
    pitch = elev - 90.0
    return (pitch, yaw, 0.0)


def resolve_table():
    """bone -> dict describing the primitive in COMPONENT space.

    Component space is bone space here: the canonical rig is authored with
    identity local rotations (trace_glb.TraceGlbWriter.add_bone) so every bone's
    component transform is a pure translation.  The editor pass ASSERTS that on
    the imported skeleton before it uses any of these numbers.
    """
    out = {}
    for bone in HIT_BONES:
        kind, spec = HIT_TABLE[bone]
        origin = SKEL[bone]
        if kind == "sphere":
            off = spec["offset"]
            out[bone] = {"kind": "sphere",
                         "center": (origin[0] + off[0], origin[1] + off[1], origin[2] + off[2]),
                         "radius": spec["radius"]}
        elif kind == "sphyl":
            child = SKEL[spec["child"]]
            axis = _sub(child, origin)
            center = origin if spec.get("at_joint") else _mid(origin, child)
            out[bone] = {"kind": "sphyl", "center": center,
                         "rotation": rot_from_z(axis),
                         "radius": spec["radius"], "length": spec["length"],
                         "bone_span": _len(axis)}
        elif kind == "box":
            child = SKEL[spec["child"]]
            out[bone] = {"kind": "box", "center": _mid(origin, child),
                         "rotation": (0.0, 0.0, 0.0),
                         "x": spec["x"], "y": spec["y"], "z": spec["z"]}
        else:                                          # pragma: no cover
            raise AssertionError("unknown primitive kind {0!r}".format(kind))
    return out


def local_of(bone, component_point):
    """Component -> bone-local, valid because bone rotations are identity."""
    o = SKEL[bone]
    return (component_point[0] - o[0], component_point[1] - o[1], component_point[2] - o[2])


def table_lines(resolved):
    """Canonical text form of the hit table — the thing T6 compares."""
    lines = []
    for bone in HIT_BONES:
        e = resolved[bone]
        c = local_of(bone, e["center"])
        if e["kind"] == "sphere":
            lines.append("{0}|sphere|c=({1:.3f},{2:.3f},{3:.3f})|r={4:.3f}".format(
                bone, c[0], c[1], c[2], e["radius"]))
        elif e["kind"] == "sphyl":
            r = e["rotation"]
            lines.append("{0}|sphyl|c=({1:.3f},{2:.3f},{3:.3f})|rot=({4:.3f},{5:.3f},{6:.3f})"
                         "|r={7:.3f}|len={8:.3f}".format(
                             bone, c[0], c[1], c[2], r[0], r[1], r[2],
                             e["radius"], e["length"]))
        else:
            r = e["rotation"]
            lines.append("{0}|box|c=({1:.3f},{2:.3f},{3:.3f})|rot=({4:.3f},{5:.3f},{6:.3f})"
                         "|xyz=({7:.3f},{8:.3f},{9:.3f})".format(
                             bone, c[0], c[1], c[2], r[0], r[1], r[2],
                             e["x"], e["y"], e["z"]))
    return lines


def digest(lines):
    return hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()[:16]


# ---------------------------------------------------------------------------
# the donor GLB
# ---------------------------------------------------------------------------

class _DonorWriter(TraceGlbWriter):
    """The donor is not a character body, so TraceGlbWriter's §2.5 character
    self-checks (176 uu height, feet at 0, 1 200-tri floor, accent budget,
    mandatory team-glow back panel) do not describe it.  They are bypassed
    explicitly — faking geometry to satisfy checks that do not apply would be
    the worse lie — but the ones that are about the FILE rather than the
    character are re-run here: no NaNs, no degenerate triangles, winding agrees
    with the normals."""

    def self_check(self):
        problems = []
        tri_total = 0
        for (_b, mesh, _mi, pname, _cb) in self.parts:
            tri_total += len(mesh.tris)
            for p in mesh.positions:
                if any(not math.isfinite(c) for c in p):
                    problems.append("NaN/inf position in part {0!r}".format(pname))
                    break
            for (a, b, c) in mesh.tris:
                pa, pb, pc = mesh.positions[a], mesh.positions[b], mesh.positions[c]
                ux, uy, uz = (pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2])
                vx, vy, vz = (pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2])
                w = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
                if 0.5 * _len(w) <= 1e-6:
                    problems.append("degenerate triangle in part {0!r}".format(pname))
                    break
                na, nb, nc = mesh.normals[a], mesh.normals[b], mesh.normals[c]
                nsum = (na[0] + nb[0] + nc[0], na[1] + nb[1] + nc[1], na[2] + nb[2] + nc[2])
                if w[0] * nsum[0] + w[1] * nsum[1] + w[2] * nsum[2] <= 0:
                    problems.append("winding/normal disagreement in part {0!r}".format(pname))
                    break
        if problems:
            raise AssertionError("; ".join(problems))
        return tri_total


def write_donor_glb():
    """One cube per hit bone, rigidly bound to it, and nothing anywhere else.

    The cube's SIZE is the only thing that matters: it has to measure more than
    MinBoneSize so FPhysicsAssetUtils bodies the bone.  Its shape is thrown away
    — every body's geometry is overwritten from the table immediately after.
    """
    w = _DonorWriter()
    w.add_material(DONOR_SLOT, (0.5, 0.5, 0.5), roughness=0.5)
    for (name, parent, head) in CANONICAL_SKELETON:
        w.add_bone(name, parent, head)
    for bone in HIT_BONES:
        cube = box(DONOR_CUBE_UU, DONOR_CUBE_UU, DONOR_CUBE_UU)
        origin = SKEL[bone]
        w.add_part(bone, cube.translate(*origin), DONOR_SLOT, name="hit_" + bone)
    if not os.path.isdir(GLB_DIR):
        os.makedirs(GLB_DIR)
    manifest = w.write(DONOR_GLB)
    log("donor GLB {0} ({1} tris, {2} cubes of {3:.0f} uu on {4} bones)".format(
        os.path.basename(DONOR_GLB), manifest["tris"], len(HIT_BONES),
        DONOR_CUBE_UU, len(HIT_BONES)))
    return manifest


# ---------------------------------------------------------------------------
# editor side
# ---------------------------------------------------------------------------

def _prop(obj, name, value):
    try:
        obj.set_editor_property(name, value)
    except Exception as exc:                          # noqa: BLE001
        fail("could not set {0!r}: {1}".format(name, exc))


def build_donor_pipeline():
    """§6.1's pipeline object, skeleton-bound (the donor rides the shared
    skeleton like characters 2-10 do).  Duplicated from the engine default, as
    import_pack.py:178-283 does, and deleted at the end of the run."""
    EAL = unreal.EditorAssetLibrary
    src = unreal.load_asset(DEFAULT_GLTF_ASSETS_PIPELINE)
    if src is None:
        fail("engine default GLTF assets pipeline missing")
        return None
    pipe = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(
        DONOR_PIPELINE.rsplit("/", 1)[-1], PIPELINE_DIR, src)
    if pipe is None:
        fail("could not duplicate the GLTF pipeline to {0}".format(DONOR_PIPELINE))
        return None
    _prop(pipe, "import_offset_uniform_scale", 1.0)
    _prop(pipe, "use_source_name_for_asset", True)
    _prop(pipe, "scene_name_sub_folder", False)
    _prop(pipe, "asset_type_sub_folders", False)
    common = pipe.get_editor_property("common_meshes_properties")
    _prop(common, "force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_NONE)
    _prop(common, "bake_meshes", True)
    _prop(common, "keep_sections_separate", False)
    skelcommon = pipe.get_editor_property("common_skeletal_meshes_and_animations_properties")
    _prop(skelcommon, "import_only_animations", False)
    _prop(skelcommon, "use_t0_as_ref_pose", False)
    skeleton = unreal.load_asset(SHARED_SKELETON_PATH)
    if skeleton is None:
        fail("{0} missing — run the import stage first".format(SHARED_SKELETON_PATH))
        return None
    _prop(skelcommon, "skeleton", skeleton)
    mesh = pipe.get_editor_property("mesh_pipeline")
    _prop(mesh, "import_static_meshes", False)
    _prop(mesh, "import_skeletal_meshes", True)
    _prop(mesh, "combine_skeletal_meshes_behavior",
          unreal.InterchangeCombineSkeletalMeshesBehavior.BY_SKELETON)
    _prop(mesh, "create_physics_asset", False)
    _prop(mesh, "build_nanite", False)
    anim = pipe.get_editor_property("animation_pipeline")
    _prop(anim, "import_animations", False)
    mat = pipe.get_editor_property("material_pipeline")
    _prop(mat, "import_materials", False)
    tex = mat.get_editor_property("texture_pipeline")
    if tex is not None:
        _prop(tex, "import_textures", False)
    unreal.EditorAssetLibrary.save_loaded_asset(pipe, only_if_is_dirty=False)
    return pipe


def import_donor():
    """Import the donor GLB and rename it onto the contract path."""
    EAL = unreal.EditorAssetLibrary
    pipe = build_donor_pipeline()
    if pipe is None:
        return None
    if EAL.does_directory_exist(DONOR_STAGING):
        EAL.delete_directory(DONOR_STAGING)

    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    source = unreal.InterchangeManager.create_source_data(DONOR_GLB)
    params = unreal.ImportAssetParameters()
    params.set_editor_property("is_automated", True)
    params.set_editor_property("override_pipelines",
                               [unreal.SoftObjectPath(DONOR_PIPELINE),
                                unreal.SoftObjectPath(DEFAULT_GLTF_PIPELINE)])
    im.import_asset(DONOR_STAGING, source, params)

    # Classify the destination folder — the return value is an opaque Array
    # (◆MEASURED, import_pack.py:import_model).
    meshes = [p for p in EAL.list_assets(DONOR_STAGING, recursive=True, include_folder=False)
              if isinstance(unreal.load_asset(p), unreal.SkeletalMesh)]
    skeletons = [p for p in EAL.list_assets(DONOR_STAGING, recursive=True, include_folder=False)
                 if isinstance(unreal.load_asset(p), unreal.Skeleton)]
    if len(meshes) != 1:
        fail("donor import produced {0} SkeletalMesh(es), expected 1".format(len(meshes)))
        return None
    if skeletons:
        fail("donor import produced {0} Skeleton(s) — its joints drifted from the shared "
             "skeleton, which would mean the canonical table moved".format(len(skeletons)))
        return None
    if EAL.does_asset_exist(DONOR_MESH_PATH):
        fail("{0} already exists — pre-wipe it before this run (Asset-Registry ghost rule)"
             .format(DONOR_MESH_PATH))
        return None
    if not EAL.rename_asset(meshes[0].split(".")[0], DONOR_MESH_PATH):
        fail("could not rename the donor mesh onto {0}".format(DONOR_MESH_PATH))
        return None
    donor = unreal.load_asset(DONOR_MESH_PATH)
    EAL.save_loaded_asset(donor, only_if_is_dirty=False)

    # sweep the staging folder + its rename redirector
    for path in EAL.list_assets(DONOR_STAGING, recursive=True, include_folder=False):
        EAL.delete_asset(path.split(".")[0])
    if EAL.does_directory_exist(DONOR_STAGING):
        EAL.delete_directory(DONOR_STAGING)
    EAL.delete_asset(DONOR_PIPELINE)
    if EAL.does_directory_exist(PIPELINE_DIR) and not EAL.list_assets(
            PIPELINE_DIR, recursive=True, include_folder=False):
        EAL.delete_directory(PIPELINE_DIR)
    log("donor mesh imported -> {0}".format(DONOR_MESH_PATH))
    return donor


def bone_transforms(mesh):
    """bone -> (component position, rotator, scale), off a throwaway
    SkeletalMeshActor (import_rocco.py:907-925 — the only headless way)."""
    sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = sub.spawn_actor_from_class(unreal.SkeletalMeshActor,
                                       unreal.Vector(0, 0, 0),
                                       unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    if actor is None:
        fail("could not spawn the probe SkeletalMeshActor")
        return {}
    out = {}
    try:
        comp = actor.skeletal_mesh_component
        comp.set_skeletal_mesh_asset(mesh)
        for i in range(comp.get_num_bones()):
            name = str(comp.get_bone_name(i))
            t = comp.get_socket_transform(name, unreal.RelativeTransformSpace.RTS_COMPONENT)
            r = t.rotation.rotator()
            out[name] = (t.translation, (r.pitch, r.yaw, r.roll),
                         (t.scale3d.x, t.scale3d.y, t.scale3d.z))
    except Exception as exc:                          # noqa: BLE001
        fail("bone probe failed: {0}".format(exc))
    finally:
        sub.destroy_actor(actor)
    return out


def assert_identity_rig(mesh, label):
    """The table's arithmetic treats bone space as component space translated.
    That is true only while every bone's ref-pose rotation is identity and its
    scale is 1 — which is how trace_glb authors them.  Checked, not assumed:
    a rig with baked bone rotations would put every capsule somewhere else."""
    table = bone_transforms(mesh)
    worst_rot, worst_bone = 0.0, ""
    for bone in HIT_BONES:
        if bone not in table:
            fail("{0}: no bone named {1!r}".format(label, bone))
            continue
        pos, rot, scale = table[bone]
        want = SKEL[bone]
        err = max(abs(pos.x - want[0]), abs(pos.y - want[1]), abs(pos.z - want[2]))
        if err > 0.5:
            fail("{0}: bone {1} is {2:.3f} uu off CANONICAL_SKELETON".format(label, bone, err))
        turn = max(abs(rot[0]), abs(rot[1]), abs(rot[2]))
        if turn > worst_rot:
            worst_rot, worst_bone = turn, bone
        if max(abs(scale[0] - 1.0), abs(scale[1] - 1.0), abs(scale[2] - 1.0)) > 1e-3:
            fail("{0}: bone {1} has non-unit ref scale {2}".format(label, bone, scale))
    if worst_rot > 0.01:
        fail("{0}: bone {1} has a non-identity ref rotation ({2:.4f} deg). The hit table's "
             "component-space arithmetic assumes identity bone rotations (trace_glb.add_bone) "
             "— it must be redone in bone space before this run can be trusted."
             .format(label, worst_bone, worst_rot))
    else:
        log("{0}: {1} hit bones on CANONICAL_SKELETON, ref rotations identity ({2})".format(
            label, len(HIT_BONES),
            "exactly, on every bone" if worst_bone == ""
            else "worst {0:.5f} deg on {1}".format(worst_rot, worst_bone)))
    return table


def body_setups(asset_path, asset_name):
    """The body setups, reached one at a time by subobject path.

    `UPhysicsAsset::SkeletalBodySetups` is not python-visible (see the header),
    but each USkeletalBodySetup IS reachable as a named subobject, and its
    `agg_geom` is EditAnywhere and therefore writable.  Names are the engine's
    NewObject defaults, SkeletalBodySetup_<n>, contiguous from 0; the loop reads
    past a few gaps before giving up so a future engine that skips a number does
    not silently truncate the table."""
    out = []
    i, misses = 0, 0
    while misses < 8:
        obj = unreal.load_object(None, "{0}.{1}:SkeletalBodySetup_{2}".format(
            asset_path, asset_name, i))
        if obj is None:
            misses += 1
        else:
            misses = 0
            out.append(obj)
        i += 1
    return out


def read_geom(setup):
    """One body's geometry in its own bone space, as the canonical text line."""
    bone = str(setup.get_editor_property("bone_name"))
    g = setup.get_editor_property("agg_geom")
    spheres = g.get_editor_property("sphere_elems")
    boxes = g.get_editor_property("box_elems")
    sphyls = g.get_editor_property("sphyl_elems")
    convex = g.get_editor_property("convex_elems")
    total = len(spheres) + len(boxes) + len(sphyls) + len(convex)
    if total != 1:
        return bone, "{0}|{1} primitive(s)".format(bone, total), total
    if spheres:
        e = spheres[0]
        c = e.get_editor_property("center")
        return bone, "{0}|sphere|c=({1:.3f},{2:.3f},{3:.3f})|r={4:.3f}".format(
            bone, c.x, c.y, c.z, e.get_editor_property("radius")), total
    if sphyls:
        e = sphyls[0]
        c = e.get_editor_property("center")
        r = e.get_editor_property("rotation")
        return bone, ("{0}|sphyl|c=({1:.3f},{2:.3f},{3:.3f})|rot=({4:.3f},{5:.3f},{6:.3f})"
                      "|r={7:.3f}|len={8:.3f}".format(
                          bone, c.x, c.y, c.z, r.pitch, r.yaw, r.roll,
                          e.get_editor_property("radius"),
                          e.get_editor_property("length"))), total
    e = boxes[0]
    c = e.get_editor_property("center")
    r = e.get_editor_property("rotation")
    return bone, ("{0}|box|c=({1:.3f},{2:.3f},{3:.3f})|rot=({4:.3f},{5:.3f},{6:.3f})"
                  "|xyz=({7:.3f},{8:.3f},{9:.3f})".format(
                      bone, c.x, c.y, c.z, r.pitch, r.yaw, r.roll,
                      e.get_editor_property("x"), e.get_editor_property("y"),
                      e.get_editor_property("z"))), total


def stamp_geometry(setup, bone, resolved):
    """Overwrite one body's geometry with the §4.8 entry for its bone."""
    e = resolved[bone]
    c = local_of(bone, e["center"])
    agg = unreal.KAggregateGeom()
    if e["kind"] == "sphere":
        el = unreal.KSphereElem()
        el.set_editor_property("center", unreal.Vector(c[0], c[1], c[2]))
        el.set_editor_property("radius", e["radius"])
        el.set_editor_property("name", "trace_{0}".format(bone))
        agg.set_editor_property("sphere_elems", [el])
        agg.set_editor_property("sphyl_elems", [])
        agg.set_editor_property("box_elems", [])
    elif e["kind"] == "sphyl":
        el = unreal.KSphylElem()
        el.set_editor_property("center", unreal.Vector(c[0], c[1], c[2]))
        el.set_editor_property("rotation", unreal.Rotator(pitch=e["rotation"][0],
                                                          yaw=e["rotation"][1],
                                                          roll=e["rotation"][2]))
        el.set_editor_property("radius", e["radius"])
        el.set_editor_property("length", e["length"])
        el.set_editor_property("name", "trace_{0}".format(bone))
        agg.set_editor_property("sphyl_elems", [el])
        agg.set_editor_property("sphere_elems", [])
        agg.set_editor_property("box_elems", [])
    else:
        el = unreal.KBoxElem()
        el.set_editor_property("center", unreal.Vector(c[0], c[1], c[2]))
        el.set_editor_property("rotation", unreal.Rotator(pitch=e["rotation"][0],
                                                          yaw=e["rotation"][1],
                                                          roll=e["rotation"][2]))
        el.set_editor_property("x", e["x"])
        el.set_editor_property("y", e["y"])
        el.set_editor_property("z", e["z"])
        el.set_editor_property("name", "trace_{0}".format(bone))
        agg.set_editor_property("box_elems", [el])
        agg.set_editor_property("sphere_elems", [])
        agg.set_editor_property("sphyl_elems", [])
    agg.set_editor_property("convex_elems", [])
    agg.set_editor_property("tapered_capsule_elems", [])
    setup.set_editor_property("agg_geom", agg)


def regularize(resolved):
    """Build the donor asset, stamp the table onto it, duplicate onto the ten."""
    EAL = unreal.EditorAssetLibrary

    for path in [DONOR_MESH_PATH, DONOR_PHYS_PATH] + [
            "{0}/{1}/SK_{1}_PhysicsAsset".format(CHAR_ROOT, c) for c in CHARACTER_ORDER]:
        if EAL.does_asset_exist(path):
            fail("{0} still exists — the shell pre-wipe did not run. Delete the physics "
                 "assets and the donor on disk BEFORE the editor starts (the Asset-Registry "
                 "ghost rule); nothing has been changed.".format(path))
            return None
    write_donor_glb()
    donor = import_donor()
    if donor is None or _failures:
        return None
    assert_identity_rig(donor, "donor")
    if _failures:
        return None

    physics = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem).create_physics_asset(donor)
    if physics is None:
        fail("create_physics_asset on the donor returned nothing")
        return None
    landed = physics.get_path_name().split(".")[0]
    if landed != DONOR_PHYS_PATH:
        fail("donor physics asset landed at {0}, not {1}".format(landed, DONOR_PHYS_PATH))
        return None

    setups = body_setups(DONOR_PHYS_PATH, "SK_TraceBodyHitDonor_PhysicsAsset")
    got = sorted(str(s.get_editor_property("bone_name")) for s in setups)
    if got != list(HIT_BONES):
        fail("the donor produced bodies on {0}, not the fifteen §4.8 bones {1}. "
             "The donor cube size ({2} uu) or MinBoneSize changed."
             .format(got, list(HIT_BONES), DONOR_CUBE_UU))
        return None
    log("donor physics: {0} bodies, exactly the §4.8 bone set".format(len(setups)))

    for s in setups:
        stamp_geometry(s, str(s.get_editor_property("bone_name")), resolved)
    EAL.save_loaded_asset(physics, only_if_is_dirty=False)
    donor.set_editor_property("physics_asset", physics)
    EAL.save_loaded_asset(donor, only_if_is_dirty=False)

    for name in CHARACTER_ORDER:
        mesh_path = "{0}/{1}/SK_{1}".format(CHAR_ROOT, name)
        phys_path = "{0}/{1}/SK_{1}_PhysicsAsset".format(CHAR_ROOT, name)
        mesh = unreal.load_asset(mesh_path)
        if mesh is None:
            fail("{0} missing — run the import stage first".format(mesh_path))
            continue
        dup = EAL.duplicate_asset(DONOR_PHYS_PATH, phys_path)
        if dup is None:
            fail("{0}: could not duplicate the hit table onto {1}".format(name, phys_path))
            continue
        EAL.save_loaded_asset(dup, only_if_is_dirty=False)
        mesh.set_editor_property("physics_asset", dup)
        EAL.save_loaded_asset(mesh, only_if_is_dirty=False)
        log("{0}: hit table duplicated onto {1}".format(name, phys_path.rsplit("/", 1)[-1]))
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(False, True)
    return physics


# ---------------------------------------------------------------------------
# verification (T6 + the §6.4/§6.5 readback this stage is asked to re-print)
# ---------------------------------------------------------------------------

def verify(resolved):
    EAL = unreal.EditorAssetLibrary
    want_lines = table_lines(resolved)
    want_digest = digest(want_lines)
    log("§4.8 hit table (bone space), digest {0}:".format(want_digest))
    for line in want_lines:
        log("    {0}".format(line))

    digests = {}
    for name in CHARACTER_ORDER:
        mesh_path = "{0}/{1}/SK_{1}".format(CHAR_ROOT, name)
        phys_path = "{0}/{1}/SK_{1}_PhysicsAsset".format(CHAR_ROOT, name)
        mesh = unreal.load_asset(mesh_path)
        phys = unreal.load_asset(phys_path)
        if mesh is None or phys is None:
            fail("{0}: mesh or physics asset missing".format(name))
            continue

        setups = body_setups(phys_path, "SK_{0}_PhysicsAsset".format(name))
        lines, bones, bad = [], [], 0
        for s in setups:
            bone, line, count = read_geom(s)
            bones.append(bone)
            lines.append(line)
            if count != 1:
                bad += 1
        lines.sort()
        got = digest(lines)
        digests[name] = got
        ornaments = sorted(set(bones) - set(HIT_BONES))
        missing = [b for b in HIT_BONES if b not in bones]
        if ornaments:
            fail("{0}: physics bodies on non-table bone(s) {1} — ornament geometry is "
                 "hittable".format(name, ornaments))
        if missing:
            fail("{0}: no physics body for {1}".format(name, missing))
        if bad:
            fail("{0}: {1} body/bodies do not carry exactly one primitive".format(name, bad))
        if got != want_digest:
            fail("{0}: hit-table digest {1} != §4.8 {2}".format(name, got, want_digest))
            for a, b in zip(lines, want_lines):
                if a != b:
                    fail("    {0}   (want {1})".format(a, b))

        # the physics asset is the one the mesh actually points at
        wired = mesh.get_editor_property("physics_asset")
        if wired is None or wired.get_path_name().split(".")[0] != phys_path:
            fail("{0}: mesh points at physics asset {1}, not {2}".format(
                name, wired.get_path_name() if wired else None, phys_path))

        # §6.4/§6.5 readback, re-printed off the SAVED assets so this stage's
        # log carries the whole per-character contract in one place.
        slots = [(str(s.get_editor_property("material_slot_name")),
                  s.get_editor_property("material_interface"))
                 for s in mesh.get_editor_property("materials")]
        slot_txt = ", ".join("{0}->{1}".format(n, mi.get_name() if mi else "NONE")
                             for n, mi in slots)
        if sorted(n for n, _mi in slots) != sorted(SLOT_ORDER):
            fail("{0}: slot names {1} != {2}".format(
                name, [n for n, _ in slots], list(SLOT_ORDER)))
        for n, mi in slots:
            if mi is None:
                fail("{0}: slot {1} has no material instance".format(name, n))
        skel = mesh.get_editor_property("skeleton")
        skel_ok = skel is not None and skel.get_path_name().split(".")[0] == SHARED_SKELETON_PATH
        if not skel_ok:
            fail("{0}: skeleton is {1}, expected the shared one".format(
                name, skel.get_path_name() if skel else None))
        b = mesh.get_bounds()
        min_z = b.origin.z - b.box_extent.z
        max_z = b.origin.z + b.box_extent.z
        log("{0:<10} bodies {1:2d} digest {2} | bounds Z [{3:6.2f},{4:6.2f}] | skeleton {5} | "
            "{6}".format(name, len(setups), got, min_z, max_z,
                         "shared" if skel_ok else "WRONG", slot_txt))

    if len(set(digests.values())) == 1 and len(digests) == len(CHARACTER_ORDER):
        log("T6: all {0} hit tables byte-identical, digest {1} == §4.8".format(
            len(digests), want_digest))
    else:
        fail("T6: hit tables differ across the roster: {0}".format(digests))

    donor = unreal.load_asset(DONOR_MESH_PATH)
    if donor is None:
        fail("{0} missing — the ten physics assets' PreviewSkeletalMesh would dangle"
             .format(DONOR_MESH_PATH))
    else:
        log("donor mesh present at {0} (editor-only preview target, never cooked)"
            .format(DONOR_MESH_PATH))
    # strays: nothing this stage does not own may sit under Shared/
    strays = [p for p in EAL.list_assets(SHARED_DIR, recursive=False, include_folder=False)
              if p.split(".")[0].rsplit("/", 1)[-1] not in
              ("SK_TraceBody_Skeleton", "SK_TraceBodyHitDonor", "SK_TraceBodyHitDonor_PhysicsAsset")]
    if strays:
        fail("unexpected assets under {0}: {1}".format(SHARED_DIR, strays))


def report():
    log("-" * 72)
    log("================ HIT-TABLE REGULARIZATION REPORT ================")
    if _failures:
        log("{0} PROBLEM(S):".format(len(_failures)))
        for f in _failures:
            log("  - {0}".format(f))
    else:
        log("no problems reported.")
    log("================ END REPORT ================")
    log("[regularize-physics] EXIT={0}".format(1 if _failures else 0))


def main():
    resolved = resolve_table()
    if "--emit-donor" in sys.argv[1:] or unreal is None:
        if unreal is None and "--emit-donor" not in sys.argv[1:]:
            print("[Trace] no unreal module and no --emit-donor: refusing to guess.")
            sys.exit(2)
        write_donor_glb()
        for line in table_lines(resolved):
            log("    {0}".format(line))
        log("table digest {0}".format(digest(table_lines(resolved))))
        report()
        sys.exit(1 if _failures else 0)

    verify_only = os.environ.get("TRACE_PHYSICS_VERIFY") == "1"
    log("=" * 72)
    log("§4.8 / T6 HIT-TABLE REGULARIZATION ({0})".format(
        "VERIFY ONLY — reading the saved assets" if verify_only else "WRITE PASS"))
    log("=" * 72)
    if not verify_only:
        regularize(resolved)
    if not _failures:
        verify(resolved)
    report()


main()
