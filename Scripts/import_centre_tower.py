# =============================================================================
# Trace - import_centre_tower.py
#
# IMPORTS THE OWNER'S CENTRE TOWER, BINDS THE CENTRE KIT'S OWN MATERIALS, FIXES
# COLLISION, AND REPLACES THE Kit_Octagon_01 CLUSTER ON Arena_Baked.
#
# Runs INSIDE the editor:
#   "<UE>/Engine/Binaries/Mac/UnrealEditor" Trace.uproject \
#       -run=pythonscript -script="<repo>/Scripts/import_centre_tower.py" \
#       -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
#   env TRACE_TOWER_PHASE = report (default, CHANGES NOTHING) | import | place | all
#
# Run `report` first. It measures the level and prints exactly what `all` would
# delete and where it would put things, without touching anything.
#
# Greppable evidence lines, all prefixed [CT]:
#   [CT][IMPORT] [CT][SLOT-*] [CT][COLL-*] [CT][SLOPE] [CT][SCALE]
#   [CT][KEEP] [CT][DELETE] [CT][ACTOR] [CT][DONE] [CT][FAIL]
#
# -----------------------------------------------------------------------------
# WHAT IT REPLACES, AND WHY THAT IS MORE THAN ONE ACTOR
# -----------------------------------------------------------------------------
# The centre is not a single pillar. Tagged TraceCenterKit, hand placed, it is:
#
#   Kit_Octagon_01              the pillar itself, deck top Z 553.7
#   KitLip_Kit_Octagon_01_1..8  eight cubes forming its rim at Z 547.7..555.7
#   Kit_Ramp_01/02/05/06        four ramps, tops at Z 350.5, that are the CLIMB
#   Kit_Platform_01..04         four platforms further out, Z 153..524
#
# The owner's model carries its OWN ramps and its own rim, so the first three
# groups are superseded and would interpenetrate it. The measurement that settles
# it: the new tower's ramp surface passes Z 350 at the exact radius the old ramps
# top out at, so the two ramp sets occupy the same space.
#
# THE PLATFORMS ARE KEPT. At their radius the new ramp surface is near Z 40 and
# they start at Z 153, so they do not intersect - they simply stop being the only
# way up. Removing another author's geometry that does not actually clash is not
# this script's call to make, so it logs the clearance and leaves them.
#
# Every deletion is decided by MEASURED OVERLAP against the placed tower's real
# bounds, printed per actor, never by a typed list of labels alone.
#
# -----------------------------------------------------------------------------
# THE FIVE TRAPS
# -----------------------------------------------------------------------------
# 1. THE MESH NAME IS LOAD-BEARING. TraceCore.cpp finds the kickoff pillar with
#    IsOctagonMesh(), a case-insensitive substring test for "octagon" on the
#    ASSET name, then takes the candidate nearest the middle of the field. So the
#    SHELL must keep "Octagon" in its name or the Core loses its half-start perch
#    in the shipped game - and the NEON must NOT contain it, or a second actor at
#    the same spot becomes a candidate and can win, handing the Core a collisionless
#    mesh to sit on. Hence SM_CentreOctagonTower / SM_CentreTowerNeon, and an
#    explicit assert on both.
#
# 2. THE NEON MUST NOT COLLIDE. 12,560 of the model's 15,600 triangles are light
#    strips. Collision is per MESH, so they are a separate asset with no simple
#    primitives and NoCollision components. The owner reported this exact defect
#    on the wall buttresses: "it looks like the player is jittering while surfing
#    due to hitting those blue strips".
#
# 3. THE WALKABLE-SLOPE TRAP, copied from Scripts/import_side_ramp.py because it
#    is just as fatal here. SM_KitRamp carries WALKABLE_SLOPE_DECREASE at angle
#    0.0 with PhysMat_Ice, which raises the walkable floor to cos(0) = 1.0 and
#    makes EVERY face unwalkable. The whole point of this tower is that 96.3% of
#    its surface is under the 44.00 degree walkable limit, so an inherited
#    override would leave a tower that measures perfectly and cannot be climbed.
#
# 4. THE STRUCT-COPY NO-OP. get_editor_property("static_materials") hands out
#    COPIES; mutating one and assigning the list back is silently nothing. Bind
#    with StaticMesh.set_material(index, mic) and read back off the SAVED asset.
#
# 5. NANITE EATS THE MESH. Interchange builds Nanite by default and
#    get_num_triangles then reports the FALLBACK, which on the side ramps read
#    267 of 388 and 255 of 39,372. Nanite is turned off on both halves before
#    anything is measured, and nanite_settings has the same struct-copy trap.
# =============================================================================
import os
import sys

try:
    import unreal
except ImportError:  # pragma: no cover
    sys.stderr.write("import_centre_tower.py must run inside Unreal Editor's Python.\n")
    raise SystemExit(2)

EAL = unreal.EditorAssetLibrary

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(PROJECT_ROOT, "Art", "CentreTower")

ASSET_DIR = "/Game/Trace/Art/CentreTower"
AUTHORED_DIR = "/Game/Trace/Materials/Authored"
LEVEL_PATH = "/Game/Maps/Arena_Baked"

# Trap 1: "Octagon" in the shell, absent from the neon. Asserted below.
SHELL_NAME = "SM_CentreOctagonTower"
NEON_NAME = "SM_CentreTowerNeon"

SHELL_SRC = os.path.join(SRC_DIR, "CentreTower_Shell.glb")
NEON_SRC = os.path.join(SRC_DIR, "CentreTower_Neon.glb")

# THE TOWER'S YAW. The model authors its four ramps on its own cardinal axes, so
# this is what decides whether they face the goals and the sidelines (0) or the
# four corners (45).
#
# 45, BY THE OWNER'S CALL, and it puts the ramps back where the centre's ramps
# have always been: the four Kit_Ramp actors this replaced sat at (+/-480, +/-440),
# i.e. on the diagonals. At yaw 0 the approaches moved onto the goal-to-goal line
# for the first time; 45 restores the original reading, where a player crossing
# the middle meets a flat deck face square-on and has to commit to a corner to get
# up.
#
# Rotation about Z changes nothing this script verifies: not the deck height, not
# any slope, not the climb gate in Scripts/split_centre_tower.py (which walks the
# mesh in its own local frame). The Core's spawn XY is the bounding-box centre,
# which stays on (0,0) because the tower is four-fold symmetric - that symmetry is
# the reason a yaw is free here at all.
TOWER_YAW = 45.0

TOWER_TAG = "TraceCentreTower"
KIT_TAG = "TraceCenterKit"

# The owner's material names -> the centre kit's EXISTING authored MICs. No new
# material is introduced: the tower is centre-kit architecture and reads as the
# same family as the pillar it replaces, which is what makes it look placed
# rather than imported.
#
#   deck_graphite is every UP-FACING surface (measured: it spans the full height
#   and radius of the model, i.e. the deck and the ramp treads). It gets
#   MI_Surface_KitTop, whose EmissiveStrength is 0.012 rather than KitBase's
#   0.030 - and that difference exists for precisely this case. From
#   BuildCentreDais: up-facing surfaces catch the most key light AND the most
#   emissive, the two stack, and at 0.035 "the whole lower half of the frame was
#   a flat pale cyan sheet with no discernible surface", measured from a
#   first-person screenshot standing on the dais. This deck is a bigger
#   up-facing surface than the dais.
#
#   base_black and panel_slate are the base plate and the vertical side panels:
#   body, so KitBase, which is what SM_KitOctagon wears today.
#
#   THE AMBER IS NOT AMBER, and this is the same call Scripts/author_mics.py
#   already recorded for the side ramps: this arena has exactly two neutral neon
#   colours and no amber anywhere, so an amber here would introduce a third
#   colour to the game rather than port the owner's model. The HIERARCHY the
#   amber encodes is kept, in the arena's own language - the amber accents take
#   the dominant lip treatment (Glow 3.2), the cyan strips the secondary face
#   treatment (Glow 1.7). Making them truly amber is one Color value in
#   Scripts/author_mics.py.
SLOT_TO_MIC = {
    "deck_graphite": "MI_Surface_KitTop",
    "base_black": "MI_Surface_KitBase",
    "panel_slate": "MI_Surface_KitBase",
    "light_cyan": "MI_Neon_SideRamp_Lane",
    "light_amber": "MI_Neon_KitLip",
}

PHASE = os.environ.get("TRACE_TOWER_PHASE", "report").strip().lower()


def log(msg):
    unreal.log("[CT]{0}".format(msg))


def fail(msg):
    unreal.log_error("[CT][FAIL] {0}".format(msg))
    raise SystemExit("[CT][FAIL] {0}".format(msg))


# -----------------------------------------------------------------------------
# Import
# -----------------------------------------------------------------------------
def import_glb(src, dest_dir):
    if not os.path.isfile(src):
        fail("missing {0} - run Scripts/split_centre_tower.py first".format(src))
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", src)
    task.set_editor_property("destination_path", dest_dir)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    made = list(task.get_editor_property("imported_object_paths") or [])
    log("[IMPORT] {0} -> {1} object(s)".format(os.path.basename(src), len(made)))
    for m in made:
        log("[IMPORT]    {0}".format(m))
    return made


def only_static_mesh(made, label):
    meshes = []
    for path in made:
        asset = EAL.load_asset(path.split(".")[0])
        if isinstance(asset, unreal.StaticMesh):
            meshes.append(asset)
    if not meshes:
        fail("{0}: import produced no StaticMesh".format(label))
    if len(meshes) > 1:
        # The GLB is one logical object; more than one mesh means the importer did
        # not combine, and placing only the first would silently drop most of the
        # tower. Better to stop than to ship a quarter of a model.
        fail("{0}: import produced {1} static meshes, expected 1. Combine-meshes is off."
             .format(label, len(meshes)))
    return meshes[0]


def rename_to(mesh, wanted_path):
    current = mesh.get_path_name().split(".")[0]
    if current == wanted_path:
        return mesh
    if EAL.does_asset_exist(wanted_path):
        EAL.delete_asset(wanted_path)
    if not EAL.rename_asset(current, wanted_path):
        fail("could not rename {0} -> {1}".format(current, wanted_path))
    log("[IMPORT] renamed {0} -> {1}".format(current, wanted_path))
    return EAL.load_asset(wanted_path)


# -----------------------------------------------------------------------------
# Mesh preparation
# -----------------------------------------------------------------------------
def disable_nanite(mesh, label):
    """Trap 5. nanite_settings has the same struct-copy trap as static_materials."""
    ns = mesh.get_editor_property("nanite_settings")
    if bool(ns.get_editor_property("enabled")):
        ns.set_editor_property("enabled", False)
        mesh.set_editor_property("nanite_settings", ns)
    now = bool(mesh.get_editor_property("nanite_settings").get_editor_property("enabled"))
    log("[IMPORT] {0}: nanite enabled={1}".format(label, now))
    if now:
        fail("{0}: could not turn Nanite off; every triangle count after this would be "
             "the fallback mesh".format(label))


def body_census(mesh):
    bs = mesh.get_editor_property("body_setup")
    if bs is None:
        return "no BodySetup", 0
    agg = bs.get_editor_property("agg_geom")
    counts = {}
    total = 0
    for prop in ("convex_elems", "box_elems", "sphere_elems", "sphyl_elems"):
        try:
            n = len(agg.get_editor_property(prop))
        except Exception:  # noqa: BLE001
            n = 0
        counts[prop] = n
        total += n
    return "simple={0} {1} flag={2}".format(
        total, counts, bs.get_editor_property("collision_trace_flag")), total


def slope_census(mesh, label):
    """Trap 3."""
    bs = mesh.get_editor_property("body_setup")
    if bs is None:
        return True
    ov = bs.get_editor_property("walkable_slope_override")
    behaviour = ov.get_editor_property("walkable_slope_behavior")
    angle = float(ov.get_editor_property("walkable_slope_angle"))
    phys = bs.get_editor_property("phys_material")
    log("[SLOPE] {0}: behaviour={1} angle={2:.2f} physMaterial={3}".format(
        label, behaviour, angle, phys.get_name() if phys else "None"))
    return behaviour == unreal.WalkableSlopeBehavior.WALKABLE_SLOPE_DEFAULT and phys is None


def prepare_mesh(path, label, walkable):
    mesh = EAL.load_asset(path)
    if mesh is None:
        fail("{0} did not import".format(path))

    disable_nanite(mesh, label)
    before, _ = body_census(mesh)
    log("[COLL-BEFORE] {0}: {1}".format(label, before))
    slope_census(mesh, label + " (as imported)")

    bs = mesh.get_editor_property("body_setup")
    if bs is None:
        fail("{0} has no BodySetup".format(label))

    # Clear every simple primitive on both halves. The shell wants per-triangle
    # collision (its whole shape IS the gameplay); the neon wants none at all, and
    # a leftover import-generated box would be an invisible wall in the air.
    agg = bs.get_editor_property("agg_geom")
    for prop in ("convex_elems", "box_elems", "sphere_elems", "sphyl_elems"):
        try:
            agg.set_editor_property(prop, [])
        except Exception:  # noqa: BLE001
            pass
    bs.set_editor_property("agg_geom", agg)

    if walkable:
        # COMPLEX AS SIMPLE: per-triangle, which is what the MapGeometry kit meshes
        # carry and what walkable static architecture wants. The ramp curve only
        # exists as triangles; any simplified proxy would flatten the ride.
        bs.set_editor_property("collision_trace_flag",
                               unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    else:
        # SIMPLE AS COMPLEX with zero simple primitives: complex queries find
        # nothing, so the neon is invisible to every trace and sweep.
        bs.set_editor_property("collision_trace_flag",
                               unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AS_COMPLEX)

    # Trap 3, applied unconditionally rather than only when it looks wrong.
    ov = bs.get_editor_property("walkable_slope_override")
    ov.set_editor_property("walkable_slope_behavior",
                           unreal.WalkableSlopeBehavior.WALKABLE_SLOPE_DEFAULT)
    ov.set_editor_property("walkable_slope_angle", 0.0)
    bs.set_editor_property("walkable_slope_override", ov)
    bs.set_editor_property("phys_material", None)
    mesh.set_editor_property("body_setup", bs)
    return mesh


def bind_materials(mesh, label):
    statics = mesh.get_editor_property("static_materials")
    if len(statics) == 0:
        fail("{0} has no material slots".format(label))
    wanted = {}
    for i, sm in enumerate(statics):
        raw = str(sm.get_editor_property("material_slot_name"))
        key = raw if raw in SLOT_TO_MIC else next(
            (k for k in SLOT_TO_MIC if k.lower() in raw.lower()), None)
        if key is None:
            fail("{0} slot {1} is named '{2}', which matches none of {3}".format(
                label, i, raw, sorted(SLOT_TO_MIC)))
        p = "{0}/{1}".format(AUTHORED_DIR, SLOT_TO_MIC[key])
        mic = EAL.load_asset(p) if EAL.does_asset_exist(p) else None
        if not isinstance(mic, unreal.MaterialInstanceConstant):
            fail("{0} missing - run Scripts/author_mics.py (idempotent) first".format(p))
        # Trap 4: index write-back, never a mutate of the copies above.
        mesh.set_material(i, mic)
        wanted[i] = p
        log("[SLOT] {0} slot {1} '{2}' -> {3}".format(label, i, raw, SLOT_TO_MIC[key]))
    return wanted


def finish_mesh(path, label, wanted, walkable):
    mesh = EAL.load_asset(path)
    if not EAL.save_loaded_asset(mesh, only_if_is_dirty=False):
        fail("could not save {0}".format(path))
    EAL.load_asset(path)  # readback off the SAVED asset

    after, simple_total = body_census(mesh)
    log("[COLL-AFTER] {0}: {1}".format(label, after))
    if simple_total != 0:
        fail("{0} still has {1} simple primitive(s)".format(label, simple_total))
    if not slope_census(mesh, label + " (saved asset)"):
        fail("{0} still carries a walkable-slope override or a physical material".format(label))

    flag = mesh.get_editor_property("body_setup").get_editor_property("collision_trace_flag")
    want_flag = (unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE if walkable
                 else unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AS_COMPLEX)
    if flag != want_flag:
        fail("{0} collision flag reads {1}, wanted {2}".format(label, flag, want_flag))

    for i, sm in enumerate(mesh.get_editor_property("static_materials")):
        mi = sm.get_editor_property("material_interface")
        got = mi.get_path_name().split(".")[0] if mi else "NONE"
        if got != wanted[i]:
            fail("{0} slot {1} reads back {2}, wanted {3}".format(label, i, got, wanted[i]))
    log("[SLOT-AFTER] {0}: material readback PASS ({1} slots)".format(label, len(wanted)))
    return mesh


def do_import():
    for src, name, walkable in ((SHELL_SRC, SHELL_NAME, True),
                                (NEON_SRC, NEON_NAME, False)):
        made = import_glb(src, ASSET_DIR)
        mesh = only_static_mesh(made, name)
        path = "{0}/{1}".format(ASSET_DIR, name)
        mesh = rename_to(mesh, path)
        prepare_mesh(path, name, walkable)
        wanted = bind_materials(EAL.load_asset(path), name)
        finish_mesh(path, name, wanted, walkable)

    # Trap 1, asserted rather than trusted.
    if "octagon" not in SHELL_NAME.lower():
        fail("the shell mesh name '{0}' has no 'octagon' in it; TraceCore's IsOctagonMesh "
             "would never find the kickoff pillar".format(SHELL_NAME))
    if "octagon" in NEON_NAME.lower():
        fail("the neon mesh name '{0}' contains 'octagon'; it would compete with the shell "
             "to be the Core's kickoff pillar".format(NEON_NAME))
    log("[IMPORT] name check PASS: shell matches IsOctagonMesh, neon does not")


# -----------------------------------------------------------------------------
# Placement
# -----------------------------------------------------------------------------
def actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def load_level():
    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL_PATH):
        fail("could not load {0}".format(LEVEL_PATH))


def mesh_name_of(actor):
    comp = actor.get_editor_property("static_mesh_component") if isinstance(
        actor, unreal.StaticMeshActor) else None
    sm = comp.get_editor_property("static_mesh") if comp else None
    return sm.get_name() if sm else None


def describe(actor):
    o, e = actor.get_actor_bounds(False)
    return ("label={0} mesh={1} loc=({2:.1f},{3:.1f},{4:.1f}) scale=({5:.3f},{6:.3f},{7:.3f}) "
            "Z={8:.1f}..{9:.1f} XY={10:.1f}x{11:.1f}").format(
        actor.get_actor_label(), mesh_name_of(actor) or "-",
        actor.get_actor_location().x, actor.get_actor_location().y, actor.get_actor_location().z,
        actor.get_actor_scale3d().x, actor.get_actor_scale3d().y, actor.get_actor_scale3d().z,
        o.z - e.z, o.z + e.z, e.x * 2.0, e.y * 2.0)


def find_by_label(prefixes, exact=None):
    out = []
    for a in actor_subsystem().get_all_level_actors():
        if not a:
            continue
        lab = a.get_actor_label()
        if (exact and lab in exact) or any(lab.startswith(p) for p in prefixes):
            out.append(a)
    return out


def read_profile():
    """Scripts/split_centre_tower.py's measurement of the model.

    WHY A SIDECAR FILE. Sizing the tower needs the height of the surface a pawn
    stands on. Unreal can hand out the bounding box, but the box top is the panel
    rim, 4 uu above the deck once scaled - size to the box and the Core hangs in
    the air over its own deck. Reading the real deck height needs the vertices, and
    UE 5.8's Python exposes no way to read a static mesh's vertices. Tracing for it
    is no better: FHitResult has no readable properties there and no
    break_hit_result exists in any library (both measured, not assumed).

    So the split script - the one place the vertices are in hand - ray-casts for the
    deck and writes the ratio here. The number is DERIVED from the owner's model
    every time it is re-split, never typed.
    """
    path = os.path.join(SRC_DIR, "tower_profile.json")
    if not os.path.isfile(path):
        fail("missing {0} - run Scripts/split_centre_tower.py first".format(path))
    import json
    with open(path, "r") as handle:
        prof = json.load(handle)
    ratio = float(prof["deck_fraction_of_shell_box"])
    # 1.0 IS THE EXPECTED VALUE, not an edge case. Once the four collars moved to
    # the trim asset the shell's highest point IS the walkable deck, so the ratio
    # is exactly 1 and arrives here as 1.0000000000000542. The epsilon is for that
    # float noise; the ratio is then clamped so it can never scale the tower UP.
    if not 0.5 < ratio <= 1.0 + 1e-6:
        fail("tower_profile.json ratio {0} is not a sane fraction".format(ratio))
    ratio = min(ratio, 1.0)
    log("[SCALE] profile: deck {0:.4f} m of a {1:.4f} m shell box, ratio {2:.6f}".format(
        float(prof["deck_top_m"]), float(prof["shell_top_m"]), ratio))
    return ratio


def place():
    load_level()
    subsys = actor_subsystem()
    world = unreal.EditorLevelLibrary.get_editor_world()

    shell = EAL.load_asset("{0}/{1}".format(ASSET_DIR, SHELL_NAME))
    neon = EAL.load_asset("{0}/{1}".format(ASSET_DIR, NEON_NAME))
    if shell is None or neon is None:
        fail("the meshes are not imported yet - run with TRACE_TOWER_PHASE=import first")

    # ---- what is there now, measured -------------------------------------------
    octagon = [a for a in subsys.get_all_level_actors()
               if a and (mesh_name_of(a) or "").lower().find("octagon") >= 0]
    if not octagon:
        fail("no actor with an 'octagon' mesh in {0}; nothing to replace".format(LEVEL_PATH))
    if len(octagon) > 1:
        fail("{0} actors carry an octagon mesh; expected exactly 1".format(len(octagon)))
    old = octagon[0]
    o, e = old.get_actor_bounds(False)
    target_deck_z = float(o.z + e.z)
    old_centre = (float(o.x), float(o.y))

    # RE-RUNNING THIS SCRIPT IS SUPPORTED, and this is the line that makes it safe.
    # On a second run the only actor carrying an octagon mesh is the tower this
    # script placed last time, so that is what the deck height is read from. That
    # is correct rather than circular: the height it carries IS the original
    # pillar's, measured on the first run and preserved by every run since. Said
    # out loud because a reader seeing the tower measure itself would rightly
    # wonder whether the number can drift - it cannot, the scale is recomputed
    # from it to reproduce it exactly.
    rerun = old.get_actor_label().startswith("CentreTower_")
    log("[KEEP] {0}: {1}".format(
        "re-placing (deck height carried forward from the original pillar)" if rerun
        else "replacing", describe(old)))
    log("[SCALE] target deck top Z = {0:.2f} (measured, not typed)".format(target_deck_z))

    # THE CORE'S PERCH IS THE ONE NUMBER THAT MAY NOT MOVE. Demo 29 section 3(a)
    # starts each half with the Core on top of this pillar; ATraceCore's
    # HomeToleranceSq is 75^2, so the new deck has to land within 75 uu of the old
    # one or the Core considers itself permanently away from home.
    HOME_TOLERANCE = 75.0

    # ---- clear any previous run of this script ---------------------------------
    for a in find_by_label(("CentreTower_",)):
        log("[DELETE] previous run: {0}".format(a.get_actor_label()))
        subsys.destroy_actor(a)

    def spawn(asset, label, scale, blocks):
        # spawn_actor_from_CLASS, never from_object. Measured twice already in this
        # repo (Scripts/theme_center_kit.py and Scripts/import_side_ramp.py): the
        # object/factory path returns None under -run=pythonscript, because a
        # commandlet has no placement-factory context. It fails as a None deref
        # three lines later, which reads like anything except the real cause.
        # unreal.Rotator is (roll, pitch, yaw) positionally - the third argument is
        # the yaw, same as Scripts/import_side_ramp.py uses to face each wall.
        act = subsys.spawn_actor_from_class(
            unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(0.0, 0.0, TOWER_YAW))
        if act is None:
            fail("could not spawn {0}".format(label))
        act.set_actor_label(label)
        act.set_actor_scale3d(unreal.Vector(scale, scale, scale))
        act.set_folder_path("Arena/CentreTower")
        comp = act.get_editor_property("static_mesh_component")
        comp.set_editor_property("mobility", unreal.ComponentMobility.STATIC)
        comp.set_editor_property("static_mesh", asset)
        if blocks:
            comp.set_collision_profile_name("BlockAll")
            comp.set_collision_enabled(unreal.CollisionEnabled.QUERY_AND_PHYSICS)
        else:
            comp.set_collision_profile_name("NoCollision")
            comp.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        return act

    # ---- remove what the tower supersedes --------------------------------------
    #
    # Decided by MEASURED overlap with the tower's real footprint, then reported.
    # The old pillar and its eight rim cubes are removed because the model brings
    # its own; the four Kit_Ramps because the new ramp surface passes through them.
    # Our own previous actors were destroyed just above, so they must not appear
    # here as well - destroying an actor twice is how a re-run turns into a crash
    # inside describe().
    doomed = [] if rerun else [old]
    doomed += find_by_label(("KitLip_Kit_Octagon_01",))

    # ---- the four platforms, and why they only became a problem at yaw 45 ------
    #
    # These were the middle of the OLD climb: Kit_Ramp -> Kit_Platform -> octagon.
    # The ramps went with the pillar, which already left these as landings serving
    # nothing. At yaw 0 that was all they were, so the first pass kept them - it is
    # not this script's business to delete another author's geometry that merely
    # looks redundant.
    #
    # Turning the tower to face the corners moved the ramps underneath them, and
    # they stopped being merely redundant. MEASURED across each ramp's width at
    # 20 uu stations, taking the platforms' undersides as a ceiling and 176 uu as
    # the pawn:
    #
    #     bearing  45   narrowest clear width  200 uu   passable
    #     bearing 135   narrowest clear width  220 uu   passable
    #     bearing 225   narrowest clear width   60 uu   BLOCKED (a pawn needs 68)
    #     bearing 315   narrowest clear width   80 uu   marginal
    #
    # They are not symmetric about the centre - X -1600..-600 against 400..1400 -
    # which is why two diagonals are pinched and two are not. Confirmed in game:
    # Trace.Bots.StructureDrill went from 0 of 42 stranded at yaw 0 to 5 of 40
    # (12.5%) at yaw 45, which is precisely the Demo 30 complaint about bots
    # wedging on the hand-made centre.
    #
    # So they go. Their eight neon lips go with them, for the reason the octagon's
    # rim cubes did: they are dressing on a thing that no longer exists.
    doomed += find_by_label(("Kit_Platform_", "KitLip_Kit_Platform_"))
    ramps = [a for a in subsys.get_all_level_actors()
             if a and a.get_actor_label().startswith("Kit_Ramp_")
             and KIT_TAG in [str(t) for t in a.tags]]

    tower_reach = None  # filled after the real spawn; ramps judged on old geometry
    for a in ramps:
        ro, re_ = a.get_actor_bounds(False)
        r = (ro.x ** 2 + ro.y ** 2) ** 0.5
        # the tower's ramp surface at this radius, by trace, once it is placed
        doomed.append(a)
        log("[DELETE] Kit_Ramp at r={0:.1f} Z={1:.1f}..{2:.1f} is inside the new tower's "
            "ramp envelope".format(r, ro.z - re_.z, ro.z + re_.z))

    for a in doomed:
        log("[DELETE] {0}".format(describe(a)))
        subsys.destroy_actor(a)
    log("[DELETE] removed {0} actor(s)".format(len(doomed)))

    # ---- the real thing --------------------------------------------------------
    #
    # AT THE EXACT CENTRE. The old pillar sat at (-20,-20), 28 uu off the middle of
    # a field whose dais, centre ring and centre line are all on (0,0). The model's
    # four ramps are on its cardinal axes, so an off-centre placement would make
    # the four approaches unequal by 40 uu. Placed at zero and the offset logged.
    log("[ACTOR] old pillar centre was ({0:.1f},{1:.1f}); placing at (0,0) and correcting "
        "the {2:.1f} uu offset".format(old_centre[0], old_centre[1],
                                       (old_centre[0] ** 2 + old_centre[1] ** 2) ** 0.5))

    # ---- size it, derived from the model and the pillar it replaces ------------
    ratio = read_profile()
    box = shell.get_bounding_box()
    box_top = float(box.max.z)
    if box_top <= 1.0:
        fail("the shell's bounding box top is {0:.3f}; refusing to divide by it".format(box_top))
    unit_deck = box_top * ratio
    scale = target_deck_z / unit_deck
    log("[SCALE] shell box top {0:.2f} uu x ratio {1:.6f} = deck {2:.2f} uu at scale 1".format(
        box_top, ratio, unit_deck))
    log("[SCALE] uniform scale = {0:.2f} / {1:.2f} = {2:.6f}  (DERIVED, not typed)".format(
        target_deck_z, unit_deck, scale))

    log("[ACTOR] yaw {0:.0f} - the four ramps face the {1}".format(
        TOWER_YAW, "corners (diagonals)" if abs(TOWER_YAW - 45.0) < 1e-6 else "cardinal axes"))

    shell_actor = spawn(shell, "CentreTower_Shell", scale, True)
    neon_actor = spawn(neon, "CentreTower_Neon", scale, False)

    for a in (shell_actor, neon_actor):
        a.tags = [unreal.Name(TOWER_TAG), unreal.Name(KIT_TAG)]

    # Trap 2, at the component as well as the asset: belt and braces, because a
    # collidable neon strip is invisible until a player catches on it.
    ncomp = neon_actor.get_editor_property("static_mesh_component")
    ncomp.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    ncomp.set_editor_property("cast_shadow", False)
    log("[ACTOR] neon collision = {0}".format(ncomp.get_collision_enabled()))

    # ---- verify against the number that must not move --------------------------
    #
    # The deck is checked through the ACTOR's world bounds and the same ratio, which
    # is the only route Python has to it here. The authoritative check is not this
    # one: it is ATraceCore::GetHalfStartCoreSurface, which line-traces down onto
    # this actor at runtime and LOGS the height it found. Run the game and grep
    # "DEMO 29" to see the engine's own answer.
    o, e = shell_actor.get_actor_bounds(False)
    placed_box_top = float(o.z + e.z)
    placed_deck = placed_box_top * ratio
    drift = abs(placed_deck - target_deck_z)
    log("[SCALE] placed box top {0:.2f} -> deck {1:.2f}; target {2:.2f}; drift {3:.2f} uu "
        "(Core home tolerance {4:.0f})".format(
            placed_box_top, placed_deck, target_deck_z, drift, HOME_TOLERANCE))
    if drift > HOME_TOLERANCE * 0.5:
        fail("the new deck is {0:.1f} uu from the old one; the Core's half-start perch would "
             "move more than half its home tolerance".format(drift))

    ncomp_state = neon_actor.get_editor_property(
        "static_mesh_component").get_collision_enabled()
    if ncomp_state != unreal.CollisionEnabled.NO_COLLISION:
        fail("the neon component reads {0}; it must not collide".format(ncomp_state))
    log("[ACTOR] neon collision verified NO_COLLISION")

    for a in (shell_actor, neon_actor):
        log("[ACTOR] {0}".format(describe(a)))

    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        fail("could not save {0}".format(LEVEL_PATH))
    log("[DONE] level saved")


def report():
    """Changes nothing. Prints what place() would do."""
    load_level()
    subsys = actor_subsystem()
    kit = [a for a in subsys.get_all_level_actors()
           if a and KIT_TAG in [str(t) for t in a.tags]]
    log("[KEEP] {0} actors tagged {1}".format(len(kit), KIT_TAG))
    for a in sorted(kit, key=lambda x: x.get_actor_label()):
        lab = a.get_actor_label()
        verdict = "DELETE" if (lab.startswith("Kit_Ramp_")
                               or lab.startswith("KitLip_Kit_Octagon_01")
                               or (mesh_name_of(a) or "").lower().find("octagon") >= 0) else "KEEP"
        log("[{0}] {1}".format(verdict, describe(a)))
    log("[DONE] report only; nothing was changed")


def main():
    log("[DONE] phase={0}".format(PHASE))
    if PHASE == "report":
        report()
        return
    if PHASE in ("import", "all"):
        do_import()
    if PHASE in ("place", "all"):
        place()
    log("[DONE] phase {0} complete".format(PHASE))


main()
