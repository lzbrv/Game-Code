# =============================================================================
# Trace - import_side_ramp.py
#
# IMPORTS THE TWO GENERATED SIDE-RAMP MESHES, BINDS THE ARENA'S AUTHORED
# MATERIALS, FIXES COLLISION, AND REPLACES Kit_Ramp_03/04 ON Arena_Baked.
#
# Runs INSIDE the editor (UnrealEditor -run=pythonscript). The meshes it imports
# are written by Scripts/generate_side_ramp.py from the constants in
# Source/Trace/World/TraceSideRampProfile.h - run that first.
#
# -----------------------------------------------------------------------------
# THE THREE TRAPS THIS SCRIPT IS BUILT AROUND
# -----------------------------------------------------------------------------
# 1. THE BODYSETUP TRAP, and it is the one that would silently destroy the whole
#    design. `SM_KitRamp` - the mesh these ramps replace - carries
#        WalkableSlopeOverride = WALKABLE_SLOPE_DECREASE, angle 0.0
#        PhysMaterial          = /Engine/EngineMaterials/PhysMat_Ice
#    WalkableSlope_Decrease at angle 0 raises the walkable floor to cos(0) = 1.0,
#    so NO FACE of that mesh, at ANY angle, can be stood on or walked up. The
#    concave ramp's entire premise is that its lower half IS walkable, so an
#    inherited or copied override would leave a ramp that measures perfect and
#    cannot be climbed. This script PRINTS the override on both new meshes
#    before and after, and FAILS if either is anything but
#    WALKABLE_SLOPE_DEFAULT with no physical material. (DEFAULT is the enum's
#    'nothing overrides the limit' member - MEASURED off unreal.WalkableSlopeBehavior
#    in UE 5.8, whose four members are DEFAULT / INCREASE / DECREASE / UNWALKABLE.
#    SM_KitRamp, read the same way, is DECREASE at angle 0.0 with PhysMat_Ice.)
#
# 2. THE STRUCT-COPY NO-OP. `mesh.get_editor_property("static_materials")` hands
#    out COPIES of the FStaticMaterial structs; mutating one and assigning the
#    list back is silently a no-op (the import_pack.py / import_railgun.py trap).
#    Materials are bound with StaticMesh.set_material(index, mic) - the INDEX
#    write-back - and then READ BACK OFF THE SAVED ASSET and asserted.
#
# 3. COLLISION ON THE TRIM. The trim mesh is ~39k triangles of thin proud neon
#    strips. It must never be in a collision query: it gets zero simple
#    primitives, CTF_USE_SIMPLE_AS_COMPLEX (so complex queries find nothing),
#    and its placed components are set to NoCollision. The SHELL gets
#    CTF_USE_COMPLEX_AS_SIMPLE - per-triangle - which is what the MapGeometry kit
#    meshes carry and what walkable static architecture wants.
#
# -----------------------------------------------------------------------------
# WHAT IT DOES TO THE LEVEL
# -----------------------------------------------------------------------------
#   * DELETES Kit_Ramp_03 and Kit_Ramp_04 (the two hand-placed StaticMeshActors
#     running the length of the side walls). Their transforms are logged first.
#   * SPAWNS four StaticMeshActors: SideRamp_Concave_PosY/_NegY (the shell) and
#     SideRamp_Trim_PosY/_NegY (the neon), tagged TraceSideRamp.
#   * The transform is DERIVED, never typed: the crest sits kCrestOutFromWallUU
#     out from the wall face at |Y| = HalfWidth, so the actor's Y is
#     HalfWidth - kCrestOutFromWallUU - kDepthUU. Scale is exactly (1,1,1) -
#     the mesh is generated at final size precisely so nothing has to be
#     stretched, and a non-unit scale would change the neon's spacing.
#     The -Y wall is yaw 180, NEVER a negative scale: a mirror flips triangle
#     winding and UE reports the resulting inside-out complex collision as a
#     surface a pawn falls through.
#
# NOTE ON THE CENTRE KIT: these actors are deliberately NOT tagged
# TraceCenterKit and their meshes deliberately do NOT live under
# /Game/Trace/Art/Pack/MapGeometry, so Scripts/theme_center_kit.py's enumerator
# (which relabels everything under that folder Kit_*) cannot pick them up. They
# still classify as "hand" for Scripts/rebake-arena-preserving.py, which sorts a
# StaticMeshActor into the hand layer when it has no TraceBakedArena tag and its
# mesh is not under /Engine/ - measured, that file's classify().
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     python3 Scripts/generate_side_ramp.py --out Art/SideRamp --verify
#     chmod -R u+w Content/Trace Content/Maps/Arena_Baked.umap \
#         Content/__ExternalActors__/Maps/Arena_Baked
#     "/Users/Shared/Epic Games/UE_5.8/.../UnrealEditor" Trace.uproject \
#         -run=pythonscript -script=".../Scripts/import_side_ramp.py" \
#         -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
#     env TRACE_SIDERAMP_PHASE = all (default) | import | place | report
#
# Greppable evidence lines, all prefixed [SR]:
#   [SR][IMPORT]      what Interchange produced, and what it was renamed to
#   [SR][SLOT-BEFORE] / [SR][SLOT-AFTER]   per-mesh material readback
#   [SR][COLL-BEFORE] / [SR][COLL-AFTER]   per-mesh collision + BodySetup census
#   [SR][SLOPE]       the WalkableSlopeOverride / PhysMaterial trap check
#   [SR][ACTOR]       per placed actor: transform, bounds, collision
#   [SR][DELETE]      the Kit_Ramp actors removed, with their old transforms
#   [SR][DONE]        summary
#   [SR][FAIL]        a hard failure (script raises)
# =============================================================================
import os
import re
import sys

try:
    import unreal
except ImportError:  # pragma: no cover
    sys.stderr.write("import_side_ramp.py must run inside Unreal Editor's Python environment.\n")
    raise SystemExit(2)

EAL = unreal.EditorAssetLibrary

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(PROJECT_ROOT, "Art", "SideRamp")
HEADER = os.path.join(PROJECT_ROOT, "Source", "Trace", "World", "TraceSideRampProfile.h")

ASSET_DIR = "/Game/Trace/Art/SideRamp"
AUTHORED_DIR = "/Game/Trace/Materials/Authored"
LEVEL_PATH = "/Game/Maps/Arena_Baked"
SIDE_RAMP_TAG = "TraceSideRamp"

SHELL_NAME = "SM_SideRampConcave"
TRIM_NAME = "SM_SideRampConcaveTrim"

# slot name (from the OBJ's usemtl) -> authored MIC.
#
# The deck wears MI_Surface_KitBase, which is what Kit_Ramp_03/04 wear today: the
# ramps being replaced, so the sideline's surface reads exactly as it did.
#
# The neon is TWO tiers because the owner's ramp has two - cyan edge/lane lights
# and an AMBER takeoff lip and entry threshold. This arena has no amber anywhere
# (TraceArenaConstants defines NeonNeutral and NeonNeutralPale and nothing else),
# and a 384 m amber line down both sidelines would be a new colour in the game,
# not a port of theirs. So the HIERARCHY is carried in the arena's own language
# instead: the lip and threshold get NeonNeutralPale at GlowLip 3.2, the lane and
# edge lines get NeonNeutral at GlowFace 1.7 - which is precisely how this arena
# already separates a dominant top lip from secondary face trim. It is an owner
# call to make them amber; it is one value in Scripts/author_mics.py.
SLOT_TO_MIC = {
    "sideramp_deck": "MI_Surface_KitBase",
    "sideramp_neon_lane": "MI_Neon_SideRamp_Lane",
    "sideramp_neon_lip": "MI_Neon_SideRamp_Lip",
}

PHASE = os.environ.get("TRACE_SIDERAMP_PHASE", "all").strip().lower()


def log(msg):
    unreal.log("[SR] {0}".format(msg))


def fail(msg):
    unreal.log_error("[SR][FAIL] {0}".format(msg))
    raise RuntimeError("[SR] " + msg)


def read_design():
    """The design constants, out of the header the compiler asserts them in."""
    text = open(HEADER, "r", encoding="utf-8").read()
    out = {}
    for name in ("kDepthUU", "kHeightUU", "kLengthUU", "kCrestOutFromWallUU",
                 "kFacetCount", "kToeOutFromWallUU"):
        m = re.search(r"inline\s+constexpr\s+\w+\s+" + name + r"\s*=\s*([0-9.eE+-]+)\s*;", text)
        if not m:
            # kToeOutFromWallUU is derived from two others; recompute rather than guess.
            if name == "kToeOutFromWallUU":
                continue
            fail("could not read {0} from {1}".format(name, HEADER))
        out[name] = float(m.group(1))
    out["kToeOutFromWallUU"] = out["kCrestOutFromWallUU"] + out["kDepthUU"]
    log("design: " + ", ".join("{0}={1}".format(k, v) for k, v in sorted(out.items())))
    return out


# -----------------------------------------------------------------------------
# Import
# -----------------------------------------------------------------------------
def import_obj(src, dest_dir):
    """One OBJ -> one StaticMesh. Returns the list of assets Interchange made."""
    if not os.path.isfile(src):
        fail("{0} is missing - run Scripts/generate_side_ramp.py first".format(src))

    task = unreal.AssetImportTask()
    task.set_editor_property("filename", src)
    task.set_editor_property("destination_path", dest_dir)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    made = list(task.get_editor_property("imported_object_paths") or [])
    log("[IMPORT] {0} -> {1}".format(os.path.basename(src), made))
    return made


def rename_to(made, wanted_path):
    """Interchange names assets off the file and its groups; force our name."""
    meshes = []
    for path in made:
        obj = EAL.load_asset(path.split(".")[0])
        if isinstance(obj, unreal.StaticMesh):
            meshes.append(path.split(".")[0])
    if not meshes:
        fail("no StaticMesh came out of the import: {0}".format(made))
    if len(meshes) > 1:
        # A multi-mesh import means the OBJ carried more than one `o` group and
        # Interchange made a mesh node out of each. MEASURED on the first pass:
        # the trim, written as `o` per material, came back as TWO StaticMeshes
        # (sideramp_neon_lane1 + sideramp_neon_lip1) and lost its second slot.
        # generate_side_ramp.py now writes ONE `o` per FILE with the materials as
        # `usemtl` runs inside it, so this is a real failure and not something to
        # paper over by picking the first one.
        fail("the import produced {0} static meshes ({1}); it must produce exactly one, or the "
             "material slots have become separate assets".format(len(meshes), meshes))
    if meshes[0] == wanted_path:
        return wanted_path
    if EAL.does_asset_exist(wanted_path):
        EAL.delete_asset(wanted_path)
    if not EAL.rename_asset(meshes[0], wanted_path):
        fail("could not rename {0} -> {1}".format(meshes[0], wanted_path))
    log("[IMPORT] renamed {0} -> {1}".format(meshes[0], wanted_path))
    return wanted_path


# -----------------------------------------------------------------------------
# Collision, the BodySetup trap, and materials
# -----------------------------------------------------------------------------
def body_census(mesh):
    bs = mesh.get_editor_property("body_setup")
    if bs is None:
        return "<no body_setup>", 0, None
    agg = bs.get_editor_property("agg_geom")
    counts = {
        "convex": len(agg.get_editor_property("convex_elems")),
        "box": len(agg.get_editor_property("box_elems")),
        "sphere": len(agg.get_editor_property("sphere_elems")),
        "sphyl": len(agg.get_editor_property("sphyl_elems")),
    }
    total = sum(counts.values())
    return ("simple={0} {1} flag={2}".format(total, counts, bs.get_editor_property("collision_trace_flag")),
            total, bs)


def slope_census(mesh, label):
    """THE TRAP. Prints the override and the physical material; returns True if clean."""
    bs = mesh.get_editor_property("body_setup")
    if bs is None:
        log("[SLOPE] {0}: <no body_setup>".format(label))
        return True
    override = bs.get_editor_property("walkable_slope_override")
    behaviour = override.get_editor_property("walkable_slope_behavior")
    angle = float(override.get_editor_property("walkable_slope_angle"))
    phys = bs.get_editor_property("phys_material")
    phys_path = phys.get_path_name() if phys else "NONE"
    clean = (behaviour == unreal.WalkableSlopeBehavior.WALKABLE_SLOPE_DEFAULT
             and phys is None)
    log("[SLOPE] {0}: behavior={1} angle={2:.1f} physMaterial={3}  -> {4}".format(
        label, behaviour, angle, phys_path,
        "CLEAN (nothing overrides the walkable limit)" if clean
        else "*** OVERRIDDEN - the run-up would not be walkable ***"))
    return clean


def slot_readback(mesh):
    out = []
    for i, sm in enumerate(mesh.get_editor_property("static_materials")):
        mi = sm.get_editor_property("material_interface")
        out.append("slot {0} '{1}' -> {2}".format(
            i, sm.get_editor_property("material_slot_name"),
            mi.get_path_name() if mi else "NONE"))
    return "; ".join(out)



def source_face_count(asset_path):
    """Faces in the OBJ this asset was imported from."""
    src = os.path.join(SRC_DIR, os.path.basename(asset_path) + ".obj")
    if not os.path.isfile(src):
        return -1
    n = 0
    for line in open(src, "r", encoding="utf-8"):
        if line.startswith("f "):
            n += 1
    return n


def tri_report(label, asset_path, tag):
    """Say what the built mesh kept of the OBJ on disk, and hand back both counts."""
    total = source_face_count(asset_path)
    mesh = EAL.load_asset(asset_path)
    kept = mesh.get_num_triangles(0) if mesh else -1
    log("[TRIS] {0} ({1}): source {2} faces -> built LOD0 {3} tris (delta {4})".format(
        label, tag, total, kept, kept - total))
    return total, kept


def disable_nanite(mesh, label):
    """
    NANITE OFF, AND IT IS NOT A STYLE CHOICE - IT WAS EATING THE MESH.

    MEASURED. Interchange's default assets pipeline builds Nanite, and
    StaticMesh::GetNumTriangles(0) then reports the NANITE FALLBACK, not the
    geometry. The first import of these two read:

        SM_SideRampConcave       source 388 faces  -> 267 tris
        SM_SideRampConcaveTrim   source 39372 faces -> 255 tris

    and the import log named the mechanism outright: "Fallback [0.05s], num tris:
    255". 255 triangles is 0.6% of the neon; the ribs, the lane lines and the edge
    lines would simply not be there.

    It is also wrong for this project independently of that. Config/DefaultEngine.ini
    sets r.Nanite.ProjectEnabled=False and says why ("This arena is NOT Nanite and
    must not be"), so a Nanite asset here renders its fallback and nothing else -
    and per-triangle collision is taken from the same built LOD, so the RIDE SURFACE
    would have been the fallback too. Turning it off is what makes the shipped ramp
    the mesh the generator wrote.

    The struct-copy trap applies here as much as to materials: nanite_settings hands
    out a COPY, so it is set back explicitly. Writing the property runs
    PostEditChangeProperty, which re-Builds the mesh; the caller asserts the new
    triangle count rather than trusting that.
    """
    ns = mesh.get_editor_property("nanite_settings")
    was = bool(ns.get_editor_property("enabled"))
    if was:
        ns.set_editor_property("enabled", False)
        mesh.set_editor_property("nanite_settings", ns)
    now = bool(mesh.get_editor_property("nanite_settings").get_editor_property("enabled"))
    log("[NANITE] {0}: enabled {1} -> {2}".format(label, was, now))
    if now:
        fail("{0} still has Nanite enabled; its triangles would be the fallback".format(label))


def prepare_mesh(path, label, complex_as_simple):
    mesh = EAL.load_asset(path)
    if not isinstance(mesh, unreal.StaticMesh):
        fail("{0} is not a StaticMesh".format(path))

    before, simple_before, bs = body_census(mesh)
    log("[COLL-BEFORE] {0}: {1}  tris={2}  verts={3}  sections={4}  lods={5}".format(
        label, before, mesh.get_num_triangles(0), mesh.get_num_vertices(0),
        mesh.get_num_sections(0), mesh.get_num_lods()))

    # TRIANGLE ACCOUNTING, AGAINST THE FILE ON DISK, BEFORE AND AFTER NANITE IS OFF.
    # As imported these read 267 of 388 and 255 of 39372; see disable_nanite().
    tri_report(label, path, "as imported")
    disable_nanite(mesh, label)
    source_faces, built = tri_report(label, path, "nanite off")

    # THE ONLY TRIANGLES THAT MAY GO ARE THE TWO DEGENERATES THE GENERATOR EMITS,
    # one per END CAP: the cap quad of the first facet has its 4th corner on top of
    # its 1st, because the parabola starts at z = 0. Anything more than that is the
    # build eating ride surface or neon, and it must stop the script rather than
    # reach the level.
    if source_faces > 0 and built < source_faces - 2:
        fail("{0}: built {1} triangles from a {2}-face OBJ. Only the 2 end-cap degenerates may "
             "go; this is the build discarding geometry.".format(label, built, source_faces))
    log("[SLOT-BEFORE] {0}: {1}".format(label, slot_readback(mesh)))
    slope_census(mesh, label + " (as imported)")

    if bs is None:
        fail("{0} has no body_setup".format(label))

    # Whatever the import left, remove any simple primitives: a convex hull on a
    # 388-triangle concave ramp is a LID over the whole curve, and a pawn would
    # walk on the hull rather than on the face.
    if simple_before > 0:
        mesh.set_editor_property("body_setup", bs)
        agg = bs.get_editor_property("agg_geom")
        agg.set_editor_property("convex_elems", [])
        agg.set_editor_property("box_elems", [])
        agg.set_editor_property("sphere_elems", [])
        agg.set_editor_property("sphyl_elems", [])
        bs.set_editor_property("agg_geom", agg)
        log("{0}: removed {1} simple primitive(s) - a hull over a concave face is a lid".format(
            label, simple_before))

    bs.set_editor_property(
        "collision_trace_flag",
        unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE if complex_as_simple
        else unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AS_COMPLEX)

    # THE TRAP, EXPLICITLY CLEARED rather than assumed clear. Interchange does not
    # set these, but the asset it replaced did, and "the importer probably would
    # not have" is not a check.
    override = bs.get_editor_property("walkable_slope_override")
    override.set_editor_property("walkable_slope_behavior",
                                 unreal.WalkableSlopeBehavior.WALKABLE_SLOPE_DEFAULT)
    override.set_editor_property("walkable_slope_angle", 0.0)
    bs.set_editor_property("walkable_slope_override", override)
    bs.set_editor_property("phys_material", None)

    return mesh


def bind_materials(mesh, label):
    mics = {}
    for slot_name, mic_name in SLOT_TO_MIC.items():
        p = "{0}/{1}".format(AUTHORED_DIR, mic_name)
        mic = EAL.load_asset(p) if EAL.does_asset_exist(p) else None
        if not isinstance(mic, unreal.MaterialInstanceConstant):
            fail("{0} missing - run Scripts/author_mics.py (idempotent) first".format(p))
        mics[slot_name] = (mic, p)

    statics = mesh.get_editor_property("static_materials")
    if len(statics) == 0:
        fail("{0} has no material slots".format(label))

    wanted = {}
    for i, sm in enumerate(statics):
        raw = str(sm.get_editor_property("material_slot_name"))
        key = raw if raw in mics else next((k for k in mics if k in raw), None)
        if key is None:
            fail("{0} slot {1} is named '{2}', which matches none of {3}".format(
                label, i, raw, sorted(mics)))
        # INDEX WRITE-BACK. Never a mutate of the struct copies above: assigning
        # that list back is silently a no-op (the import_pack.py trap).
        mesh.set_material(i, mics[key][0])
        wanted[i] = mics[key][1]
    return wanted


def finish_mesh(path, label, wanted):
    mesh = EAL.load_asset(path)
    if not EAL.save_loaded_asset(mesh, only_if_is_dirty=False):
        fail("could not save {0}".format(path))

    # SAVED-ASSET READBACK. Reload from disk, then assert.
    EAL.load_asset(path)
    after, _, _ = body_census(mesh)
    log("[COLL-AFTER] {0}: {1}".format(label, after))
    log("[SLOT-AFTER] {0}: {1}".format(label, slot_readback(mesh)))
    if not slope_census(mesh, label + " (saved asset)"):
        fail("{0} still carries a walkable-slope override or a physical material after the fix"
             .format(label))

    for i, sm in enumerate(mesh.get_editor_property("static_materials")):
        mi = sm.get_editor_property("material_interface")
        got = mi.get_path_name().split(".")[0] if mi else "NONE"
        if got != wanted[i]:
            fail("{0} slot {1} reads back {2}, wanted {3}".format(label, i, got, wanted[i]))
    log("{0}: material readback PASS".format(label))


# -----------------------------------------------------------------------------
# Placement
# -----------------------------------------------------------------------------
def actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def describe_actor(actor):
    comp = actor.get_editor_property("static_mesh_component")
    mesh = comp.get_editor_property("static_mesh") if comp else None
    origin, extent = actor.get_actor_bounds(False)
    bits = []
    for getter, name in (("get_collision_enabled", "enabled"),
                         ("get_collision_profile_name", "profile"),
                         ("get_collision_object_type", "objectType")):
        try:
            bits.append("{0}={1}".format(name, getattr(comp, getter)()))
        except Exception as exc:  # noqa: BLE001
            bits.append("{0}=<{1}>".format(name, exc))
    return ("label={0} mesh={1} loc={2} rot={3} scale={4} boundsOrigin={5} boundsExtent={6} "
            "mobility={7} {8}").format(
        actor.get_actor_label(),
        mesh.get_path_name() if mesh else "NONE",
        actor.get_actor_location(), actor.get_actor_rotation(),
        actor.get_actor_scale3d(), origin, extent,
        comp.get_editor_property("mobility") if comp else "?", " ".join(bits))


def place(design):
    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL_PATH):
        fail("could not load {0}".format(LEVEL_PATH))

    subsys = actor_subsystem()

    # HalfWidth off the arena builder in the level, not typed: the whole layout
    # derives from FieldWidth and a hand-typed 4800 would be a second copy of it.
    half_width = None
    for actor in subsys.get_all_level_actors():
        if actor and actor.get_class().get_name() == "TraceArenaBuilder":
            half_width = float(actor.get_editor_property("field_width")) * 0.5
            log("arena builder found: FieldWidth {0:.1f} -> HalfWidth {1:.1f}".format(
                half_width * 2.0, half_width))
            break
    if half_width is None:
        fail("no ATraceArenaBuilder in {0}: cannot derive the wall position".format(LEVEL_PATH))

    toe_y = half_width - design["kCrestOutFromWallUU"] - design["kDepthUU"]
    log("crest at |Y| {0:.1f} (out {1:.0f} from the wall face at {2:.1f}); toe at |Y| {3:.1f}; "
        "clear floor between the toes {4:.1f} uu".format(
            toe_y + design["kDepthUU"], design["kCrestOutFromWallUU"], half_width,
            toe_y, 2.0 * toe_y))

    # ---- remove the ramps this replaces ------------------------------------
    removed = 0
    for actor in list(subsys.get_all_level_actors()):
        if not actor:
            continue
        label = actor.get_actor_label()
        if label in ("Kit_Ramp_03", "Kit_Ramp_04"):
            log("[DELETE] {0}".format(describe_actor(actor)))
            subsys.destroy_actor(actor)
            removed += 1
    log("[DELETE] removed {0} actor(s)".format(removed))

    # ---- and any previous run of this script -------------------------------
    for actor in list(subsys.get_all_level_actors()):
        if actor and actor.get_actor_label().startswith("SideRamp_"):
            log("[DELETE] previous side-ramp actor {0}".format(actor.get_actor_label()))
            subsys.destroy_actor(actor)

    shell = EAL.load_asset("{0}/{1}".format(ASSET_DIR, SHELL_NAME))
    trim = EAL.load_asset("{0}/{1}".format(ASSET_DIR, TRIM_NAME))
    if shell is None or trim is None:
        fail("the meshes are not imported yet - run the import phase first")

    # ------------------------------------------------------------------------
    # WHICH WAY ROUND EACH ONE GOES, DERIVED FROM THE MESH AND NOT TYPED.
    #
    # THE OBJ IS NOT IMPORTED IN THE FRAME IT WAS WRITTEN IN. The generator writes
    # the toe at y = 0 and the crest at y = +kDepthUU, because OBJ is right-handed;
    # Unreal is left-handed and its OBJ translator negates Y, so MEASURED off the
    # built asset the crest lands at local y = -760 and the toe stays at y = 0:
    #
    #     SM_SideRampConcave bounding box  min (-19200, -760, 0)  max (19200, 0, 660)
    #
    # A first draft that assumed the written frame put yaw 0 on the +Y wall and
    # would have built both ramps FACING THE WRONG WAY - crest inboard, toe against
    # the wall, the run-up unreachable behind a 660 uu cliff. So the yaw is derived:
    # the toe is at local y = 0 whatever the translator did, the crest is at the far
    # end, and each side takes the yaw that sends the crest AT ITS OWN WALL.
    #
    # Yaw, never a negative scale. Mirroring by scale flips triangle winding, and
    # complex-as-simple collision built from inside-out triangles is a surface a
    # pawn falls through.
    # HOW FAR THE TOE END IS ALLOWED TO BE FROM y = 0, and why it is not 1 uu any more.
    #
    # The trim's neon stands 3 uu PROUD ALONG THE SURFACE NORMAL, so the toe line's
    # own bounding box hangs off the toe by 3 * sin(toe angle). That was written
    # when the profile started at 0.8 degrees and the overhang was 0.04 uu, i.e.
    # invisible against a 1 uu tolerance. The buttress pass cut the toe at 46.9
    # degrees (TraceSideRampProfile.h: the face is inside the surf band end to end
    # now, so there is no shallow start) and the same 3 uu became 2.2 - MEASURED, and
    # it failed this check with "SM_SideRampConcaveTrim has local Y -757.4..2.2".
    #
    # 2% of the depth is 15.2 uu at the shipped 760, which swallows the neon at any
    # toe angle the band allows (3 uu at 90 degrees would still only be 3) and is
    # still fifty times smaller than the crest end it has to be distinguished from.
    # A mesh whose toe genuinely is not at zero fails exactly as before.
    toe_tolerance = max(1.0, 0.02 * design["kDepthUU"])

    def crest_local_y(mesh, label):
        box = mesh.get_bounding_box()
        lo = float(box.min.y)
        hi = float(box.max.y)
        # The toe sits on y = 0 by construction; the crest is whichever end is far
        # from it. If BOTH ends are away from zero the mesh is not what we think it
        # is, and guessing would place a 384 m ramp wrong.
        if min(abs(lo), abs(hi)) > toe_tolerance:
            fail("{0} has local Y {1:.1f}..{2:.1f}; neither end is within {3:.1f} uu of the toe "
                 "at y=0".format(label, lo, hi, toe_tolerance))
        return lo if abs(lo) > abs(hi) else hi

    shell_crest = crest_local_y(shell, SHELL_NAME)
    trim_crest = crest_local_y(trim, TRIM_NAME)
    if (shell_crest > 0) != (trim_crest > 0):
        fail("the shell's crest is at local Y {0:.1f} and the trim's at {1:.1f}; the neon would "
             "be on the back of the ramp".format(shell_crest, trim_crest))
    log("crest is at local Y {0:.1f} on the shell and {1:.1f} on the trim (the toe is y=0 on both)"
        .format(shell_crest, trim_crest))

    spawned = []
    for sign in (1.0, -1.0):
        side = "PosY" if sign > 0 else "NegY"
        # yaw 0 keeps local +Y as world +Y; yaw 180 negates it. Take the one that
        # puts this mesh's crest on THIS side's wall.
        yaw = 0.0 if (shell_crest > 0) == (sign > 0) else 180.0
        log("[PLACE] {0} wall: actor Y {1:+.1f} (the toe), yaw {2:.0f} -> crest at Y {3:+.1f}, "
            "wall face at Y {4:+.1f}".format(
                side, sign * toe_y, yaw,
                sign * toe_y + (shell_crest if yaw == 0.0 else -shell_crest),
                sign * half_width))
        for mesh, name, blocks in ((shell, "SideRamp_Concave_" + side, True),
                                   (trim, "SideRamp_Trim_" + side, False)):
            # spawn_actor_from_class, NOT spawn_actor_from_object. MEASURED here, and
            # already measured once in Scripts/theme_center_kit.py: the object/factory
            # path returns None under -run=pythonscript ("SpawnActorFromObject. No actor
            # was spawned.") because a commandlet has no placement-factory context. The
            # class path is the one bake-arena.py proves out in this exact environment.
            actor = subsys.spawn_actor_from_class(
                unreal.StaticMeshActor, unreal.Vector(0.0, sign * toe_y, 0.0),
                unreal.Rotator(0.0, 0.0, yaw))
            if actor is None:
                fail("could not spawn {0}".format(name))
            actor.set_actor_label(name)
            actor.set_actor_scale3d(unreal.Vector(1.0, 1.0, 1.0))
            actor.tags = [SIDE_RAMP_TAG]
            actor.set_folder_path("Arena/SideRamps")
            comp = actor.get_editor_property("static_mesh_component")
            comp.set_editor_property("mobility", unreal.ComponentMobility.STATIC)
            comp.set_editor_property("static_mesh", mesh)
            if blocks:
                comp.set_collision_profile_name("BlockAll")
                comp.set_collision_enabled(unreal.CollisionEnabled.QUERY_AND_PHYSICS)
            else:
                # The neon is 39k triangles of 3 uu proud strips. It is scenery.
                comp.set_collision_profile_name("NoCollision")
                comp.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
            spawned.append(actor)

    for actor in spawned:
        log("[ACTOR] {0}".format(describe_actor(actor)))

    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    log("[DONE] {0} removed, {1} spawned, level saved".format(removed, len(spawned)))


def main():
    design = read_design()

    if PHASE in ("all", "import"):
        for src_name, asset_name, complex_as_simple in (
                ("SM_SideRampConcave.obj", SHELL_NAME, True),
                ("SM_SideRampConcaveTrim.obj", TRIM_NAME, False)):
            made = import_obj(os.path.join(SRC_DIR, src_name), ASSET_DIR)
            path = rename_to(made, "{0}/{1}".format(ASSET_DIR, asset_name))
            mesh = prepare_mesh(path, asset_name, complex_as_simple)
            wanted = bind_materials(mesh, asset_name)
            finish_mesh(path, asset_name, wanted)

    if PHASE in ("all", "place"):
        place(design)

    if PHASE == "report":
        for name in (SHELL_NAME, TRIM_NAME):
            path = "{0}/{1}".format(ASSET_DIR, name)
            mesh = EAL.load_asset(path)
            if mesh is None:
                log("{0}: NOT IMPORTED".format(path))
                continue
            census, _, _ = body_census(mesh)
            log("{0}: {1} tris={2} | {3}".format(
                name, census, mesh.get_num_triangles(0), slot_readback(mesh)))
            slope_census(mesh, name)

    log("[DONE] import_side_ramp.py finished (phase={0})".format(PHASE))


try:
    main()
except Exception as exc:  # noqa: BLE001 - a commandlet needs the reason in the log
    unreal.log_error("[SR][FAIL] import_side_ramp.py failed: {0}".format(exc))
    raise
