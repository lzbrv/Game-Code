# =============================================================================
# Trace - theme_center_kit.py   (MAP plan section 2.3 - the centre-kit theming pass)
#
# Converts the collaborator's centre set piece (5-6x ramp1/SM_KitRamp,
# 4x platform1/SM_KitPlatform, 1x octagon/SM_KitOctagon placed as plain
# StaticMeshActors in Arena_Baked) from
# engine-default WorldGrid/Wireframe grey into the arena's own language,
# WITHOUT moving a single actor or remodelling a mesh. The layout is KEPT.
#
# WHAT IT DOES, IN ORDER (MAP section 2.3 steps):
#   1. LOADS the four authored MICs from /Game/Trace/Materials/Authored/.
#      They are AUTHORED BY Scripts/author_mics.py (W1-MATPULSE) and already
#      exist; this script never creates or mutates them - if one is missing it
#      fails loudly with the instruction to run author_mics.py (idempotent).
#   2. Assigns them on the MESH ASSETS (not per-actor), so every placement is
#      fixed at once (each mesh has a single slot and the placed actors carry
#      no overrides):  the two ramps and the octagon -> MI_Surface_KitBase;
#      the platform -> MI_Surface_KitTop. See MESH_TO_MIC, which carries both
#      the pre-rename and the post-rename asset names.
#      StaticMesh.set_material(index, mi) - the INDEX write-back - never a
#      mutate of the struct copies that get_editor_property("static_materials")
#      hands out: assigning that list back is silently a no-op (the
#      import_pack.py / import_railgun.py struct-copy trap). Slot 0 is then
#      READ BACK off the saved asset and asserted.
#   3. Collision census + fix: prints each mesh's body_setup simple-primitive
#      counts and trace flag (the BEFORE census), and where a mesh has zero
#      simple primitives sets CTF_USE_COMPLEX_AS_SIMPLE - correct for static
#      walkable architecture at this tri count (content-inventory.md section 2).
#      The AFTER census is printed too, so both are on record in one log.
#   4. Loads /Game/Maps/Arena_Baked and enumerates the placed kit actors
#      (StaticMeshActors whose mesh lives under /Game/Trace/Art/Pack/MapGeometry).
#   5. Relabels them Kit_Octagon_01 / Kit_Platform_01..04 / Kit_Ramp_01..06
#      (numbering by ascending X then Y, so re-runs are deterministic) and adds
#      the actor tag TraceCenterKit. Deliberately NOT TraceBakedArenaTag: that
#      tag means "this level is pre-built" (TraceArenaBuilder.h) and the kit is
#      a HAND layer, not bake output - the distinction is what lets the
#      preserving re-bake (MAP section 10) treat these actors correctly.
#   6. THE LIP PASS - inlaid contour strips. The kit's single material slot
#      cannot carry the arena's lip language, and the arena's precedent for
#      neon on WALKABLE tops is the inlaid bank contour (banks protrude 1.5 uu;
#      walkable pieces explicitly refuse standoff shells - TraceArenaBuilder).
#      Per platform: four thin /Engine/BasicShapes/Cube strips wearing
#      MI_Neon_KitLip - length edge-16 uu, width 24 uu, height 8 uu, centred
#      on the edge, protruding 2 uu above the top face. FLAT platforms get all
#      four edges; the four PLACED platforms are pitched +/-10 (inclined
#      approaches, per the live census), so they get only their two SIDE RAILS
#      - a lit line ACROSS a run surface reads as a hazard line, the same
#      drawn==lethal discipline (bible section 6.2) that gives RAMPS NO STRIPS
#      at all. Octagon: one strip per top-ring edge, the ring read exactly
#      from the mesh's own convex hull (no scene queries - a commandlet has no
#      physics scene to trace against); bounds-derived regular-octagon
#      fallback if the hull is unreadable. 24 uu is under the 36 uu
#      pawn-reach cap and over the 8 uu same-room AA floor; Glow 3.2 is T1.
#      Strips are labelled KitLip_<parentlabel>_<n>, tagged TraceCenterKit.
#      Re-runs first delete every existing KitLip_* so the pass is idempotent.
#   7. Deletes the stray collaborator point light (MAP section 1.5) by its
#      external-actor PACKAGE identity, HOSK4IQX0RWVAVF7LQ6DV3. One correction
#      to the plan's rationale, measured live by this script's own [LIGHT]
#      census: the actor is 'Floor_Lamp_2' and DOES carry the
#      TraceBakedArena/TraceBakedFloorLamp tags - it is a collaborator
#      copy-paste of a lattice lamp (non-zero-padded name; the bake's own 32
#      lamps are Floor_Lamp_01..32), one of SIX such centre-edit copies
#      (Floor_Lamp_2/3/4/6/7/8). So it WAS adopted and scaled with the
#      lattice, not orphaned as MAP 1.5 believed - but the package the plan
#      names is unambiguous and the delete decision stands: the lattice plus
#      the kit's own neon are the designed light at centre. The other five
#      copies are NOT deleted (the plan names exactly one; the rest are an
#      owner call, flagged in the W2-KIT report). Its transform/intensity are
#      logged before deletion, and the caller is expected to have taken the
#      BEFORE screenshots already (W2-KIT does).
#   8. Saves everything (meshes were saved in step 2/3; the level + dirty
#      external-actor packages here) and prints a final [KIT][DONE] census.
#
# Asset renames (octagon -> SM_KitOctagon etc.) HAVE HAPPENED, in W5-MAPFINISH,
# under MAP section 10.4 - they were deferred until the preserving re-bake
# existed so the actor packages would only churn once. Scripts/rename-kit-assets.py
# is the pass that did it; the tables in this file carry both sets of names so it
# runs on a renamed tree and on an older checkout alike.
#
# The kit sits in the neutral centre third: its neon is neutral cyan, never
# team-colored, so it needs NO FTraceBakedSideTint entries and is invisible to
# the half-time repaint.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor" \
#         "/path/to/Trace.uproject" \
#         -run=pythonscript -script="/path/to/Scripts/theme_center_kit.py" \
#         -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
#
# Real RHI (no -nullrhi), one editor instance, alarm-wrapped - same discipline
# as generate_content.py. The working set must be writable first (LFS checks
# uassets out read-only):
#     chmod -R u+w Content/Trace/Art/Pack/MapGeometry Content/Trace/Materials \
#         Content/Maps/Arena_Baked.umap Content/__ExternalActors__/Maps/Arena_Baked
#
# Greppable evidence lines, all prefixed [KIT]:
#   [KIT][SLOT-BEFORE] / [KIT][SLOT-AFTER]   per-mesh slot 0 material readback
#   [KIT][COLL-BEFORE] / [KIT][COLL-AFTER]   per-mesh simple-primitive census
#   [KIT][CENSUS]                            per placed actor: label, mesh,
#                                            world loc/rot/scale, bounds, top Z
#   [KIT][COLL-ACTOR]                        per placed actor: collision enabled,
#                                            profile, object type, mobility - the
#                                            per-ACTOR half of the collision
#                                            census (step 3 fixes the ASSET; a
#                                            component override could still turn
#                                            one placement into a ghost, and only
#                                            this line would show it)
#   [KIT][STRIP]                             per spawned lip strip
#   [KIT][LIGHT]                             the stray point light, then DELETED
#   [KIT][DONE]                              summary counts (the final census)
#   [KIT][FAIL]                              any hard failure (script raises)
# =============================================================================

import math
import os
import sys

try:
    import unreal
except ImportError:  # pragma: no cover - only possible outside the editor
    sys.stderr.write("theme_center_kit.py must run inside Unreal Editor's Python environment.\n")
    raise SystemExit(2)

EAL = unreal.EditorAssetLibrary

AUTHORED_DIR = "/Game/Trace/Materials/Authored"
MAPGEO_DIR = "/Game/Trace/Art/Pack/MapGeometry"
LEVEL_PATH = "/Game/Maps/Arena_Baked"
CUBE_PATH = "/Engine/BasicShapes/Cube.Cube"
KIT_TAG = "TraceCenterKit"
STRAY_LIGHT_PKG = "HOSK4IQX0RWVAVF7LQ6DV3"  # MAP section 1.5, code-world.md section 3.1

# mesh basename -> MIC basename (MAP section 2.3 step 2)
#
# BOTH THE PRE-RENAME AND THE POST-RENAME NAMES. W5-MAPFINISH ran MAP section
# 10.4's renames (Scripts/rename-kit-assets.py): octagon -> SM_KitOctagon,
# platform1 -> SM_KitPlatform, ramp1 -> SM_KitRamp, ramp -> SM_KitRampAlt. This
# script has to keep working on both a renamed tree and an older checkout that
# still has the collaborator's import names, and the mapping is the same either
# way, so the table simply carries eight keys instead of four.
MESH_TO_MIC = {
    "ramp1": "MI_Surface_KitBase",
    "ramp": "MI_Surface_KitBase",
    "octagon": "MI_Surface_KitBase",
    "platform1": "MI_Surface_KitTop",  # dominant visible face is the walkable top
    "SM_KitRamp": "MI_Surface_KitBase",
    "SM_KitRampAlt": "MI_Surface_KitBase",
    "SM_KitOctagon": "MI_Surface_KitBase",
    "SM_KitPlatform": "MI_Surface_KitTop",
}
LIP_MIC = "MI_Neon_KitLip"

STRIP_WIDTH = 24.0     # uu - under the 36 uu pawn-reach cap, over the 8 uu AA floor
STRIP_HEIGHT = 8.0     # uu
STRIP_PROTRUDE = 2.0   # uu above the top face (inlaid, not a standoff)
STRIP_EDGE_INSET = 16.0  # length = edge length - 16 uu
CUBE_UU = 100.0        # the engine basic cube is 100 uu across


def log(msg):
    unreal.log("[KIT] {0}".format(msg))


def fail(msg):
    unreal.log_error("[KIT][FAIL] {0}".format(msg))
    raise RuntimeError("[KIT] " + msg)


def fmt_v(v):
    return "({0:.1f}, {1:.1f}, {2:.1f})".format(v.x, v.y, v.z)


# -----------------------------------------------------------------------------
# Step 1 - load the authored MICs (created by author_mics.py; never duplicated)
# -----------------------------------------------------------------------------

def load_mics():
    mics = {}
    for name in sorted(set(MESH_TO_MIC.values()) | {LIP_MIC}):
        path = "{0}/{1}".format(AUTHORED_DIR, name)
        mic = EAL.load_asset(path) if EAL.does_asset_exist(path) else None
        if not isinstance(mic, unreal.MaterialInstanceConstant):
            fail("{0} missing or not a MaterialInstanceConstant - run "
                 "Scripts/author_mics.py (idempotent) first".format(path))
        mics[name] = mic
        log("MIC loaded: {0}".format(path))
    return mics


# -----------------------------------------------------------------------------
# Steps 2-3 - assign on the mesh assets, census + fix collision, save, read back
# -----------------------------------------------------------------------------

def collision_census(mesh):
    bs = mesh.get_editor_property("body_setup")
    if bs is None:
        return ("<no body_setup>", 0)
    agg = bs.get_editor_property("agg_geom")
    counts = {
        "convex": len(agg.get_editor_property("convex_elems")),
        "box": len(agg.get_editor_property("box_elems")),
        "sphere": len(agg.get_editor_property("sphere_elems")),
        "sphyl": len(agg.get_editor_property("sphyl_elems")),
    }
    total = sum(counts.values())
    flag = str(bs.get_editor_property("collision_trace_flag"))
    return ("simple={0} {1} flag={2}".format(total, counts, flag), total)


def slot_readback(mesh):
    out = []
    for i, sm in enumerate(mesh.get_editor_property("static_materials")):
        mi = sm.get_editor_property("material_interface")
        out.append("slot {0} '{1}' -> {2}".format(
            i, sm.get_editor_property("material_slot_name"),
            mi.get_path_name() if mi else "NONE"))
    return "; ".join(out)


def theme_meshes(mics):
    for base, mic_name in sorted(MESH_TO_MIC.items()):
        path = "{0}/{1}".format(MAPGEO_DIR, base)
        mesh = EAL.load_asset(path)
        if not isinstance(mesh, unreal.StaticMesh):
            fail("{0} missing or not a StaticMesh".format(path))

        log("[SLOT-BEFORE] {0}: {1}".format(base, slot_readback(mesh)))
        before, simple_before = collision_census(mesh)
        log("[COLL-BEFORE] {0}: {1}".format(base, before))

        mic = mics[mic_name]
        statics = mesh.get_editor_property("static_materials")
        if len(statics) == 0:
            fail("{0} has no material slots at all".format(base))
        for i in range(len(statics)):
            # Index write-back (StaticMesh.set_material), NOT a struct-copy
            # mutate - see the header. Every slot of these meshes gets the one
            # MIC (each has exactly one slot today; a future re-import that
            # grows a second slot still ends up fully themed, never grey).
            mesh.set_material(i, mic)

        # Collision fix (MAP section 2.3 step 3): only when there is no simple
        # collision - complex-as-simple makes scene queries (walking, hitscan)
        # use the render tris, which is correct and cheap for static walkable
        # architecture of this size.
        if simple_before == 0:
            bs = mesh.get_editor_property("body_setup")
            if bs is None:
                fail("{0} has no body_setup to fix".format(base))
            bs.set_editor_property(
                "collision_trace_flag",
                unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
            log("{0}: zero simple primitives -> CTF_USE_COMPLEX_AS_SIMPLE set".format(base))

        if not EAL.save_loaded_asset(mesh, only_if_is_dirty=False):
            fail("could not save {0}".format(path))

        # Saved-asset readback: what is on the asset that just went to disk.
        after, _ = collision_census(mesh)
        log("[COLL-AFTER] {0}: {1}".format(base, after))
        got = slot_readback(mesh)
        log("[SLOT-AFTER] {0}: {1}".format(base, got))
        want = "{0}/{1}".format(AUTHORED_DIR, mic_name)
        for i, sm in enumerate(mesh.get_editor_property("static_materials")):
            mi = sm.get_editor_property("material_interface")
            got_path = mi.get_path_name().split(".")[0] if mi else "NONE"
            if got_path != want:
                fail("{0} slot {1} reads back {2}, wanted {3}".format(base, i, got_path, want))
    log("all {0} MapGeometry meshes themed, collision-checked and saved".format(len(MESH_TO_MIC)))


# -----------------------------------------------------------------------------
# Steps 4-5 - enumerate, relabel, tag
# -----------------------------------------------------------------------------

def mesh_base_of(actor):
    comp = actor.get_editor_property("static_mesh_component")
    mesh = comp.get_editor_property("static_mesh") if comp else None
    if mesh is None:
        return None
    p = mesh.get_path_name()
    if not p.startswith(MAPGEO_DIR + "/"):
        return None
    return p.split("/")[-1].split(".")[0]


def actor_collision_census(actor):
    """The per-ACTOR collision facts, which the per-asset census cannot see.

    Step 3 repairs the MESH's body setup, and the mesh is shared by every
    placement - but a component-level override (collision_enabled NoCollision,
    a profile of NoCollision/OverlapAll, a Movable body left dynamic) would
    make one placement a ghost while the asset census still read green. That
    failure is exactly what the walk/shoot pass is meant to catch, so the
    static half of the same question is recorded here, per actor, in the same
    log. Query collision is what a hitscan asks; physics collision is what a
    pawn capsule pushes against; both are printed.
    """
    comp = actor.get_editor_property("static_mesh_component")
    if comp is None:
        return "<no static_mesh_component>"
    bits = []
    # GETTERS, not get_editor_property: collision_enabled / collision_profile_name
    # live inside the component's FBodyInstance and are NOT properties of the
    # component itself - asking for them by name raises (measured in 5.8).
    for getter, label in (("get_collision_enabled", "enabled"),
                          ("get_collision_profile_name", "profile"),
                          ("get_collision_object_type", "objectType")):
        try:
            bits.append("{0}={1}".format(label, getattr(comp, getter)()))
        except Exception as exc:  # noqa: BLE001 - a missing getter must not stop the census
            bits.append("{0}=<unreadable: {1}>".format(label, exc))
    try:
        bits.append("mobility={0}".format(comp.get_editor_property("mobility")))
    except Exception as exc:  # noqa: BLE001
        bits.append("mobility=<unreadable: {0}>".format(exc))
    try:
        mesh = comp.get_editor_property("static_mesh")
        bs = mesh.get_editor_property("body_setup") if mesh else None
        if bs is not None:
            bits.append("assetTraceFlag={0}".format(
                bs.get_editor_property("collision_trace_flag")))
    except Exception as exc:  # noqa: BLE001
        bits.append("assetTraceFlag=<unreadable: {0}>".format(exc))
    return " ".join(bits)


def ensure_tag(actor, tag_name):
    tags = list(actor.get_editor_property("tags"))
    if any(str(t) == tag_name for t in tags):
        return False
    tags.append(unreal.Name(tag_name))
    actor.set_editor_property("tags", tags)
    return True


def enumerate_and_relabel(actor_subsys):
    # Keyed by SHAPE, not by asset name, so the MAP section 10.4 renames do not
    # change what this function counts: octagon/SM_KitOctagon are one bucket.
    SHAPE_OF = {"octagon": "octagon", "SM_KitOctagon": "octagon",
                "platform1": "platform1", "SM_KitPlatform": "platform1",
                "ramp1": "ramp1", "SM_KitRamp": "ramp1",
                "ramp": "ramp", "SM_KitRampAlt": "ramp"}
    kit = {"octagon": [], "platform1": [], "ramp1": [], "ramp": []}
    for actor in actor_subsys.get_all_level_actors():
        if not isinstance(actor, unreal.StaticMeshActor):
            continue
        shape = SHAPE_OF.get(mesh_base_of(actor))
        if shape is not None:
            kit[shape].append(actor)

    total = sum(len(v) for v in kit.values())
    log("placed kit actors found: octagon={0} platform1={1} ramp1={2} ramp={3} (total {4})".format(
        len(kit["octagon"]), len(kit["platform1"]), len(kit["ramp1"]), len(kit["ramp"]), total))
    if len(kit["octagon"]) != 1 or len(kit["platform1"]) != 4 or not (5 <= len(kit["ramp1"]) <= 6):
        # The strings census says ramp1 x6, platform1 x4, octagon x1; trust the
        # live enumeration but refuse silently-odd worlds.
        log("WARNING: census differs from the content-inventory count "
            "(expected 1/4/6) - proceeding on the live enumeration")

    def keyed(actors):
        return sorted(actors, key=lambda a: (round(a.get_actor_location().x, 1),
                                             round(a.get_actor_location().y, 1)))

    relabelled = []
    for base, prefix in (("octagon", "Kit_Octagon"), ("platform1", "Kit_Platform"),
                         ("ramp1", "Kit_Ramp")):
        for n, actor in enumerate(keyed(kit[base]), start=1):
            old = actor.get_actor_label()
            new = "{0}_{1:02d}".format(prefix, n)
            if old != new:
                actor.set_actor_label(new)
            ensure_tag(actor, KIT_TAG)
            relabelled.append(actor)
            loc = actor.get_actor_location()
            rot = actor.get_actor_rotation()
            scale = actor.get_actor_scale3d()
            origin, extent = actor.get_actor_bounds(False)
            log("[CENSUS] {0} (was '{1}') mesh={2} loc={3} rot=({4:.1f},{5:.1f},{6:.1f}) "
                "scale={7} bounds_origin={8} extent={9} topZ={10:.1f}".format(
                    new, old, base, fmt_v(loc), rot.pitch, rot.yaw, rot.roll,
                    fmt_v(scale), fmt_v(origin), fmt_v(extent), origin.z + extent.z))
            log("[COLL-ACTOR] {0}: {1}".format(new, actor_collision_census(actor)))
    return kit, relabelled


# -----------------------------------------------------------------------------
# Step 6 - the lip pass
# -----------------------------------------------------------------------------

def delete_existing_strips(actor_subsys):
    doomed = [a for a in actor_subsys.get_all_level_actors()
              if a.get_actor_label().startswith("KitLip_")]
    for a in doomed:
        actor_subsys.destroy_actor(a)
    if doomed:
        log("idempotency: deleted {0} existing KitLip_* strips before respawning".format(len(doomed)))


def spawn_strip(actor_subsys, cube, lip_mi, label, center, rotation, length):
    # spawn_actor_from_class, NOT spawn_actor_from_object: the object/factory
    # path returns None under -run=pythonscript (no placement factory context
    # in a commandlet - measured here); the class path is the one bake-arena.py
    # proves out in exactly this environment.
    actor = actor_subsys.spawn_actor_from_class(unreal.StaticMeshActor, center, rotation)
    if actor is None:
        fail("could not spawn strip {0}".format(label))
    comp = actor.get_editor_property("static_mesh_component")
    comp.set_editor_property("mobility", unreal.ComponentMobility.STATIC)
    comp.set_editor_property("static_mesh", cube)
    comp.set_material(0, lip_mi)
    actor.set_actor_scale3d(unreal.Vector(
        max(length, 8.0) / CUBE_UU, STRIP_WIDTH / CUBE_UU, STRIP_HEIGHT / CUBE_UU))
    actor.set_actor_label(label)
    ensure_tag(actor, KIT_TAG)
    actor.set_folder_path("Arena/CenterKit")
    log("[STRIP] {0} at {1} rot=({2:.1f},{3:.1f},{4:.1f}) len={5:.1f}".format(
        label, fmt_v(center), rotation.pitch, rotation.yaw, rotation.roll, length))
    return actor


def platform_strips(actor_subsys, cube, lip_mi, platforms):
    """Lip strips for the platform1 placements.

    The live census (this script's own [CENSUS] lines) says all four placed
    platforms are PITCHED +/-10 deg with yaw 90: they are inclined approach
    ramps onto the octagon deck, not flat decks. The drawn==lethal discipline
    (bible section 6.2, the reason plain ramps get no strips) forbids a lit
    line ACROSS a run surface - so a tilted platform gets only its two SIDE
    RAILS: strips along the local-X (slope) direction at the local +/-Y edges,
    riding the incline with the actor's own rotation. The two level end edges
    (downhill and uphill lips, which the player runs straight over) get
    nothing. A FLAT platform (|pitch| and |roll| <= 1 deg) still gets the full
    four-edge treatment of the MAP section 2.3 spec.
    """
    spawned = 0
    for actor in platforms:
        comp = actor.get_editor_property("static_mesh_component")
        mesh = comp.get_editor_property("static_mesh")
        box = mesh.get_bounding_box()
        t = actor.get_actor_transform()
        rot = actor.get_actor_rotation()
        scale = actor.get_actor_scale3d()
        sx = (box.max.x - box.min.x) * abs(scale.x)
        sy = (box.max.y - box.min.y) * abs(scale.y)
        cx = (box.min.x + box.max.x) / 2.0
        cy = (box.min.y + box.max.y) / 2.0
        label = actor.get_actor_label()
        flat = abs(rot.pitch) <= 1.0 and abs(rot.roll) <= 1.0
        if not flat and (abs(rot.roll) > 1.0 or abs(rot.pitch) > 15.0):
            log("WARNING: {0} rotation (pitch {1:.1f} roll {2:.1f}) is beyond the "
                "side-rail policy; strips skipped".format(label, rot.pitch, rot.roll))
            continue

        if flat:
            edges = (
                (unreal.Vector(cx, box.min.y, box.max.z), 0.0, sx - STRIP_EDGE_INSET),
                (unreal.Vector(cx, box.max.y, box.max.z), 0.0, sx - STRIP_EDGE_INSET),
                (unreal.Vector(box.min.x, cy, box.max.z), 90.0, sy - STRIP_EDGE_INSET),
                (unreal.Vector(box.max.x, cy, box.max.z), 90.0, sy - STRIP_EDGE_INSET),
            )
        else:
            # Side rails only: along local X (the pitched axis) at local +/-Y.
            edges = (
                (unreal.Vector(cx, box.min.y, box.max.z), 0.0, sx - STRIP_EDGE_INSET),
                (unreal.Vector(cx, box.max.y, box.max.z), 0.0, sx - STRIP_EDGE_INSET),
            )
        n = 0
        for local, yaw_add, length in edges:
            n += 1
            world = t.transform_location(local)
            center = unreal.Vector(world.x, world.y,
                                   world.z + STRIP_PROTRUDE - STRIP_HEIGHT / 2.0)
            if yaw_add == 0.0:
                # Full actor rotation: on a tilted platform the rail rides the
                # incline; on a flat one pitch/roll are ~0 and this is the yaw.
                strip_rot = unreal.Rotator(roll=rot.roll, pitch=rot.pitch, yaw=rot.yaw)
            else:
                strip_rot = unreal.Rotator(roll=0.0, pitch=0.0, yaw=rot.yaw + yaw_add)
            spawn_strip(actor_subsys, cube, lip_mi, "KitLip_{0}_{1}".format(label, n),
                        center, strip_rot, length)
            spawned += 1
    return spawned


def octagon_top_ring(octagon):
    """The octagon's flat-top boundary polygon, from its own convex hull.

    The mesh's body_setup carries one KConvexElem ([COLL] census); its
    vertex_data IS the mesh's exact silhouette, so the top-face ring is read
    straight off the asset - no scene queries (line traces have no physics
    scene to ask under -run=pythonscript; measured). Returns a list of world
    corner Vectors ordered around the centroid, or None to use the bounds
    fallback.
    """
    comp = octagon.get_editor_property("static_mesh_component")
    mesh = comp.get_editor_property("static_mesh")
    bs = mesh.get_editor_property("body_setup")
    if bs is None:
        return None
    convex = bs.get_editor_property("agg_geom").get_editor_property("convex_elems")
    if len(convex) == 0:
        return None
    try:
        verts = list(convex[0].get_editor_property("vertex_data"))
    except Exception as exc:  # noqa: BLE001
        log("WARNING: convex vertex_data not readable ({0}); bounds fallback".format(exc))
        return None
    if not verts:
        return None
    top_z = max(v.z for v in verts)
    ring = [v for v in verts if v.z > top_z - 4.0]
    # Dedupe in XY (hull data may repeat corners across faces).
    uniq = {}
    for v in ring:
        uniq[(round(v.x, 1), round(v.y, 1))] = v
    ring = list(uniq.values())
    if len(ring) < 6:
        log("WARNING: top ring has {0} verts; bounds fallback".format(len(ring)))
        return None
    cx = sum(v.x for v in ring) / len(ring)
    cy = sum(v.y for v in ring) / len(ring)
    ring.sort(key=lambda v: math.atan2(v.y - cy, v.x - cx))
    t = octagon.get_actor_transform()
    world_ring = [t.transform_location(v) for v in ring]
    log("octagon top ring: {0} corners at local topZ {1:.1f}: {2}".format(
        len(world_ring), top_z, ["({0:.0f},{1:.0f})".format(v.x, v.y) for v in world_ring]))
    return world_ring


def octagon_strips(actor_subsys, cube, lip_mi, octagon):
    origin, extent = octagon.get_actor_bounds(False)
    top_z = origin.z + extent.z
    label = octagon.get_actor_label()
    ring = octagon_top_ring(octagon)

    if ring is None:
        # Bounds fallback: regular octagon, flats on the world axes (the
        # placed actor is unrotated), apothem a conservative 88% of the
        # smaller half-extent (inside any bevel).
        apothem = 0.88 * min(extent.x, extent.y)
        log("octagon strips from BOUNDS FALLBACK: apothem={0:.1f}".format(apothem))
        face_len = 2.0 * apothem * math.tan(math.radians(22.5)) - STRIP_EDGE_INSET
        strips = 0
        for k in range(8):
            theta = k * 45.0
            a = math.radians(theta)
            center = unreal.Vector(origin.x + apothem * math.cos(a),
                                   origin.y + apothem * math.sin(a),
                                   top_z + STRIP_PROTRUDE - STRIP_HEIGHT / 2.0)
            rotation = unreal.Rotator(roll=0.0, pitch=0.0, yaw=theta + 90.0)
            spawn_strip(actor_subsys, cube, lip_mi, "KitLip_{0}_{1}".format(label, k + 1),
                        center, rotation, face_len)
            strips += 1
        return strips

    # One strip per top-ring edge, centred on the edge, protruding 2 uu.
    strips = 0
    n = len(ring)
    for i in range(n):
        a = ring[i]
        b = ring[(i + 1) % n]
        ex, ey = b.x - a.x, b.y - a.y
        edge_len = math.hypot(ex, ey)
        if edge_len < STRIP_EDGE_INSET + 8.0:
            log("octagon edge {0} is only {1:.0f} uu; skipped".format(i + 1, edge_len))
            continue
        mid = unreal.Vector((a.x + b.x) / 2.0, (a.y + b.y) / 2.0,
                            top_z + STRIP_PROTRUDE - STRIP_HEIGHT / 2.0)
        yaw = math.degrees(math.atan2(ey, ex))
        rotation = unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw)
        strips += 1
        spawn_strip(actor_subsys, cube, lip_mi, "KitLip_{0}_{1}".format(label, strips),
                    mid, rotation, edge_len - STRIP_EDGE_INSET)
    return strips


# -----------------------------------------------------------------------------
# Step 7 - the stray centre point light (MAP section 1.5)
# -----------------------------------------------------------------------------

def actor_pkg_name(actor):
    """The actor's OWN package (external-actor file), '' when not exposed."""
    for getter in ("get_package", "get_outermost"):
        try:
            pkg = getattr(actor, getter)()
            if pkg is not None:
                return pkg.get_name()
        except Exception:  # noqa: BLE001
            continue
    return ""


def delete_stray_light(actor_subsys):
    stray = None
    for actor in actor_subsys.get_all_level_actors():
        if not isinstance(actor, unreal.PointLight):
            continue
        pkg = actor_pkg_name(actor)
        tags = [str(t) for t in actor.get_editor_property("tags")]
        loc = actor.get_actor_location()
        log("[LIGHT] point light '{0}' pkg={1} tags={2} loc={3}".format(
            actor.get_actor_label(), pkg, tags, fmt_v(loc)))
        if STRAY_LIGHT_PKG in pkg:
            stray = actor
    if stray is None:
        log("[LIGHT] stray light package {0} not present (already deleted?) - nothing to do"
            .format(STRAY_LIGHT_PKG))
        return None
    comp = stray.get_editor_property("point_light_component")
    intensity = comp.get_editor_property("intensity") if comp else -1.0
    loc = stray.get_actor_location()
    log("[LIGHT] DELETING stray collaborator light '{0}' at {1} intensity={2} "
        "(MAP 1.5's named package; the live census above shows it carries "
        "copied lattice-lamp tags - see the header's step-7 correction. Its "
        "contribution is on record in the W1-LIGHT B_V2/B_V3 baselines and "
        "this tranche's before-shots)".format(
            stray.get_actor_label(), fmt_v(loc), intensity))
    pkg_path = actor_pkg_name(stray)
    actor_subsys.destroy_actor(stray)
    return pkg_path or "<package-name-unavailable>"


# -----------------------------------------------------------------------------
# main
# -----------------------------------------------------------------------------

def main():
    mics = load_mics()
    theme_meshes(mics)

    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not level_editor.load_level(LEVEL_PATH):
        fail("could not load {0}".format(LEVEL_PATH))
    log("level loaded: {0}".format(LEVEL_PATH))

    actor_subsys = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    kit, relabelled = enumerate_and_relabel(actor_subsys)

    cube = unreal.load_asset(CUBE_PATH)
    if cube is None:
        fail("engine cube {0} did not load".format(CUBE_PATH))

    delete_existing_strips(actor_subsys)
    strips = platform_strips(actor_subsys, cube, mics[LIP_MIC], kit["platform1"])
    for octo in kit["octagon"]:
        strips += octagon_strips(actor_subsys, cube, mics[LIP_MIC], octo)

    deleted_pkg = delete_stray_light(actor_subsys)

    if not level_editor.save_current_level():
        fail("save_current_level() failed")
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(
        save_map_packages=True, save_content_packages=True)
    log("level + dirty packages saved (external actors included)")

    # A deleted external actor's package occasionally survives the save as an
    # on-disk orphan; external actors are discovered by folder scan, so an
    # orphan would resurrect the light on next load. Remove it if it survived.
    if deleted_pkg and deleted_pkg.startswith("/Game/"):
        rel = deleted_pkg.replace("/Game/", "Content/") + ".uasset"
        proj = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
        abs_path = os.path.normpath(os.path.join(proj, rel))
        if os.path.isfile(abs_path):
            os.remove(abs_path)
            log("[LIGHT] orphaned actor package removed from disk: {0}".format(abs_path))
        else:
            log("[LIGHT] actor package gone from disk with the save: {0}".format(abs_path))
    elif deleted_pkg:
        log("[LIGHT] actor deleted; package path was not resolvable from Python - "
            "verify Content/__ExternalActors__/Maps/Arena_Baked/2/3U/{0}.uasset is "
            "gone after the save".format(STRAY_LIGHT_PKG))

    log("[DONE] meshes themed: {0}; kit actors relabelled+tagged: {1}; lip strips: {2}; "
        "stray light deleted: {3}".format(
            len(MESH_TO_MIC), len(relabelled), strips, "yes" if deleted_pkg else "no/absent"))


try:
    main()
except Exception as exc:  # noqa: BLE001 - a commandlet needs the reason in the log
    unreal.log_error("[KIT][FAIL] theme_center_kit.py failed: {0}".format(exc))
    raise
