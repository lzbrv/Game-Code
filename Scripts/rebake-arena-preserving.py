# =============================================================================
# Trace - rebake-arena-preserving.py            (release overhaul, MAP plan §10)
#
# RE-BAKES /Game/Maps/Arena_Baked FROM THE BUILDER WITHOUT THROWING AWAY THE
# HAND LAYER.
#
# -----------------------------------------------------------------------------
# WHY THIS EXISTS
# -----------------------------------------------------------------------------
# Scripts/bake-arena.sh --force is a destructive re-bake: it deletes the .umap
# and all ~570 One-File-Per-Actor packages and emits a brand new level straight
# out of ATraceArenaBuilder. That is the ONLY way builder changes (new geometry
# families, new widths, new side-tint registrations, new material parents) reach
# the shipping map — but it also erases every hand edit the level has ever had:
#
#   * the collaborator's centre kit — 11 hand-placed StaticMeshActors plus the
#     16 KitLip_* dress strips W2's theme_center_kit.py inlays on them;
#   * five hand-copied centre floor lamps (Floor_Lamp_3/4/6/7/8);
#   * ~44 procedural pieces a human DELETED to clear room for that kit;
#   * any piece a human MOVED.
#
# MAP plan §10.1 fixes the governance so those two facts can coexist:
#
#   * the BUILDER is the source of truth for every procedural family. Builder
#     changes are never hand-copied into Arena_Baked; they arrive via re-bake.
#   * Arena_Baked is the shipping map and the ONLY home of the hand layer.
#   * piece-level hand edits are honoured as TRANSFORMS, not as materials: a
#     moved wall keeps its move, a re-materialed wall is superseded by the
#     builder's theming (that is the intended outcome, not a loss).
#   * deletions of procedural pieces are replayed — what a human deleted stays
#     deleted — with two exceptions: brand-new families this plan adds, and the
#     corner banks, which are restored deliberately (§10.3).
#
# This script is the mechanism. It censuses the live map, runs the standard
# force bake, replays the deletions and the transform edits onto the fresh
# result, re-spawns the hand layer, and prints a reconciliation that has to
# balance against the census. If it does not balance, NOTHING has been lost:
# the caller's filesystem backup of the .umap + __ExternalActors__ tree is the
# recovery (risk-register item #1 — take it before running this).
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
# As an ORCHESTRATOR, from a normal shell — this is the intended entry point.
# It runs all three steps and checks each one:
#
#     python3 Scripts/rebake-arena-preserving.py
#     python3 Scripts/rebake-arena-preserving.py --dry-run   # plan only, no writes
#     python3 Scripts/rebake-arena-preserving.py --census    # step 1 only
#     python3 Scripts/rebake-arena-preserving.py --no-census # bake + restore,
#                                                    re-using the census on disk
#     python3 Scripts/rebake-arena-preserving.py --restore-only   # step 3 only
#
# Inside the editor it runs ONE phase, selected by TRACE_REBAKE_PHASE:
#
#     UnrealEditor Trace.uproject -run=pythonscript \
#         -script=.../rebake-arena-preserving.py -unattended -nosplash -stdout
#     env: TRACE_REBAKE_PHASE=census | restore
#
# The three steps, in order, and why they cannot be one editor session:
#
#   1. census   (editor)  read the live map, write Saved/rebake_census.json
#   2. bake     (shell)   Scripts/bake-arena.sh --force
#   3. restore  (editor)  read the fresh map, replay, restore, save, reconcile
#
# Step 2 deletes the .umap and the whole __ExternalActors__ directory FROM THE
# FILESYSTEM before the editor starts (bake-arena.sh:129-183 — measured: the
# asset-registry delete reports success and leaves the file on disk, and
# new_level then refuses with "An asset already exists at this location"). A
# process that already has those packages loaded cannot do that to itself, so
# the census and the restore are separate editor sessions with the destructive
# shell step between them. That is also what makes the census durable: it is on
# disk as JSON before anything is deleted.
#
# -----------------------------------------------------------------------------
# OPTIONS (environment variables; -script= has no argv channel)
# -----------------------------------------------------------------------------
#   TRACE_REBAKE_PHASE    census | restore          (required inside the editor)
#   TRACE_REBAKE_MAP      package path of the level. Default /Game/Maps/Arena_Baked
#   TRACE_REBAKE_CENSUS   census JSON path. Default <project>/Saved/rebake_census.json
#   TRACE_REBAKE_DRYRUN   "1": restore computes and PRINTS the whole plan and
#                         saves nothing. The plan is written into the census
#                         JSON either way, so a dry run is reviewable.
#   TRACE_REBAKE_TRANSFORM_MODE   isolated (default) | all | none — see the
#                         constant of the same name.
#   TRACE_REBAKE_BANK_CONFLICT    delete (default) | report — see the constant
#                         of the same name.
#   TRACE_REBAKE_RECREATE "1" (default) to re-create hand-DUPLICATED baked
#                         pieces. W5-MAPFINISH replaced the engine call that used
#                         to crash the commandlet; "0" goes back to reporting
#                         them as a documented loss. See clone_baked_piece.
#   TRACE_REBAKE_RECREATE_Z  donor (default) | census — which Z a re-created
#                         duplicate comes back at. The argument is written out
#                         above plan_recreate.
#   TRACE_REBAKE_HAND_DUPLICATES  comma-separated labels DECLARED to be hand
#                         duplicates, for the case where the non-unit-scale
#                         proof no longer applies to them. Empty by default. A
#                         declared label still has to pass the mirror-donor test.
#                         See HAND_DUPLICATE_LABELS for why it exists.
#
# MAP §10.4's kit asset renames are carried by MESH_RENAMES: the census records
# the mesh a hand actor wore by path, those four assets are renamed between the
# census and this restore, and renamed_mesh_path moves each path explicitly and
# logs it rather than relying on a redirector still being there.
# =============================================================================

import json
import os
import time
import re
import sys

IN_EDITOR = False
try:
    import unreal  # noqa: F401
    IN_EDITOR = True
except ImportError:
    unreal = None


# -----------------------------------------------------------------------------
# Shared configuration
# -----------------------------------------------------------------------------

MAP_PATH = os.environ.get("TRACE_REBAKE_MAP", "/Game/Maps/Arena_Baked").rstrip("/")
PHASE = os.environ.get("TRACE_REBAKE_PHASE", "").strip().lower()
DRY_RUN = os.environ.get("TRACE_REBAKE_DRYRUN", "0") == "1"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
CENSUS_PATH = os.environ.get(
    "TRACE_REBAKE_CENSUS", os.path.join(PROJECT_ROOT, "Saved", "rebake_census.json"))

# How old a census may be before the restore phase refuses it. Six hours is chosen to be
# comfortably longer than any full census->bake->restore run (the longest measured is well
# under an hour) and far shorter than "yesterday's file is still lying around".
CENSUS_MAX_AGE_HOURS = float(os.environ.get("TRACE_REBAKE_CENSUS_MAX_AGE_HOURS", "6"))
ALLOW_STALE_CENSUS = os.environ.get("TRACE_REBAKE_ALLOW_STALE_CENSUS", "") not in ("", "0")

# The bake's own vocabulary. Changing any of these in TraceArenaBake.cpp without
# changing them here would make the census mis-sort the hand layer, so they are
# quoted with their source line rather than re-derived.
BAKED_TAG = "TraceBakedArena"          # TraceArenaBake.cpp:111
FLOOR_LAMP_TAG = "TraceBakedFloorLamp" # TraceArenaBake.cpp:118
KIT_TAG = "TraceCenterKit"             # Scripts/theme_center_kit.py (W2-KIT)

# A lamp the bake emitted is labelled Floor_Lamp_%02d (TraceArenaBake.cpp:1241).
# ANY OTHER POINT LIGHT IS HAND-PLACED, and this is not a cosmetic distinction:
# W2-KIT measured that the collaborator's centre lamp copies carry BOTH
# TraceBakedArena and TraceBakedFloorLamp (they were copy-pasted from a lattice
# lamp, tags and all), so the tag alone calls them bake output and a re-bake
# would silently delete five lights somebody placed. The zero-padding is the
# only thing that separates them: Floor_Lamp_03 is the bake's, Floor_Lamp_3 is
# a copy. MAP §10.1's mechanical definition of the hand layer ("any point light
# lacking the floor-lamp tag") is therefore TIGHTENED here, not loosened —
# see the deviation note in the tranche report.
BAKED_LAMP_LABEL = re.compile(r"^Floor_Lamp_\d{2}$")

# MAP §10.2.3's allowlist: labels in the FRESH bake that are absent from the
# census are normally replayed deletions, EXCEPT these. The first five are
# families §4 adds (they cannot have been "deleted" — they never existed), and
# Bank_* is the deliberate restoration argued in §10.3.
#
# Matched against the label with underscores removed, because the census sees
# the bake's PRETTY labels (PrettyPieceName splits BankSkin -> Bank_Skin) while
# the plan wrote the raw debug names.
DELETION_ALLOWLIST = ("SKYLINE", "HORIZONBAND", "GOALBEACON", "GATEFINIAL", "TOWERBEACON", "BANK")

# MAP §10.2.4: a hand transform edit counts as one when it exceeds this.
TRANSLATION_TOLERANCE_UU = 0.5
ROTATION_TOLERANCE_DEG = 0.1

# How close a surviving piece of the same family has to be before "this family is
# still standing here" is true — the last deletion rule, applied to a fresh piece
# that no census piece claimed.
#
# WHY THE MATCHING IS BY POSITION AND NOT BY LABEL, which is the single most
# important design decision in this script. MAP §10.2.3-4 keys everything on the
# label, and the bake's labels are `<Name>_%02d` handed out by a per-name counter
# in emission order — so a label is only stable while the family's SIZE and ORDER
# are. Both moved under this overhaul, and the census proves it:
#
#   * Goal_Ring_Rim went 32 → 34 pieces. Label-keyed, the two extra bars read as
#     "labels the census never saw" = replayed deletions, and the goal ring would
#     come out of the re-bake with two bars missing.
#   * The cover field lost one member (153 → 152) while a human had ALSO deleted
#     `Cover_39` and left a duplicate at `Cover_153`. Label-keyed, `Cover_115`
#     in the census (a hand-moved block at −2970,−2040) pairs with a fresh
#     `Cover_115` 6,210 uu away in the OPPOSITE quadrant, and "replaying" that
#     transform would fling a cover block across the field and leave a hole.
#
# Matching each census piece to the NEAREST fresh piece of the same family,
# one-to-one, greedily by ascending distance, is immune to all of it: identical
# pieces pair at distance 0, the four hand-nudged centre covers pair with the four
# fresh ones at ~380 uu, and only a genuine surplus is left over. A leftover fresh
# piece is then a deletion candidate, and this radius is the test that separates
# "the family densified in place" (a new sibling lands ~170 uu from an old one)
# from "a human cleared this region" (nothing of the family within thousands of uu).
NEIGHBOUR_RADIUS_UU = 400.0

# What to do with an A ∩ B piece whose transform moved (MAP §10.2.4).
#
#   isolated  replay a move only when it is NOT shared by most of its family.
#             §10.2.4 assumes every A ∩ B delta is a human drag, but §10.1 says
#             the builder owns procedural geometry — and W2-WORLD's §7 width
#             changes (EndTrimSize 46→72, GoalLineWidth 44→64, centre line
#             48→72) move those pieces' pivots by construction. Replaying THOSE
#             would silently revert a builder change through the back door, so a
#             delta shared by ≥ FAMILY_MOVE_FRACTION of a family is read as the
#             builder's and left alone. An isolated one is a person's drag.
#   all       §10.2.4 literally: replay every delta.
#   none      replay nothing.
TRANSFORM_MODE = os.environ.get("TRACE_REBAKE_TRANSFORM_MODE", "isolated").strip().lower()
FAMILY_MOVE_FRACTION = 0.5

# A BAKED PIECE'S ACTOR SCALE IS 1 BY CONSTRUCTION, and that makes it a proof
# rather than a heuristic: EmitBakedActors builds every piece transform as
# `FTransform(BuilderRotation, ..., FVector::OneVector)` (TraceArenaBake.cpp:1013)
# and puts all the geometry in component transforms. Measured on this map: the
# fresh bake has 0 pieces with a non-unit scale, and the census has 22 — four
# cover blocks squashed in Z and the whole eighteen-piece top-centre tower group
# at 0.75. So "scale != 1" identifies a hand edit with no false positives, which
# is what lets the rules below tell a person's drag from the builder's own
# re-geometry without guessing.
SCALE_TOLERANCE = 1e-4

# MAP §10.2.5's conflict rule: a restored bank piece that intersects a hand actor
# is deleted, because the centre layout is absolute.
#
#   delete   apply it (the spec's behaviour, and the default)
#   report   measure and log every conflict, delete nothing — for the case where
#            the conflict set is large enough that applying the rule silently
#            cancels the whole §10.3 restoration and somebody has to LOOK first.
BANK_CONFLICT_MODE = os.environ.get("TRACE_REBAKE_BANK_CONFLICT", "delete").strip().lower()

# Re-create hand-DUPLICATED baked pieces (see plan_recreate). ON since
# W5-MAPFINISH replaced the engine call that used to crash the commandlet;
# TRACE_REBAKE_RECREATE=0 goes back to reporting them as a documented loss.
RECREATE_DUPLICATES = os.environ.get("TRACE_REBAKE_RECREATE", "1") == "1"

# Where a re-created duplicate's Z comes from. `donor` (default) puts it level
# with the sibling it mirrors; `census` puts it at the logged Z, which on this
# map is that same Z multiplied by a hand squash the restore deliberately does
# not replay. The whole argument is written out above plan_recreate.
RECREATE_Z_SOURCE = os.environ.get("TRACE_REBAKE_RECREATE_Z", "donor").strip().lower()

# Labels DECLARED to be hand duplicates, comma-separated. Empty by default, so
# nothing about a default run changes.
#
# WHY THIS HAD TO EXIST (measured 2026-09-01, second re-bake). plan_recreate's
# proof that a census piece with no fresh counterpart is a hand COPY rather than
# a label the builder retired was "its actor scale is not 1". That proof was
# sound when it was written and IT IS NOW VACUOUS, because the previous restore
# consumed it: W5-MAPFINISH deliberately brought the nine top-centre-tower
# copies back at the DONOR's unit scale rather than the census's 0.75 (see the
# argument above plan_recreate), so the map those pieces now live in has
#
#     census pieces with a non-unit actor scale: 0     (was 22)
#
# and `hand_scaled` is False for every one of the nine. Simulated against a real
# fresh bake before this run: the unmodified script planned 0 re-creations and
# 9 "vanished" - it would have dropped the whole mirrored half of the tower and
# still printed BALANCED: YES with restore_failures [], because `vanished` is a
# legitimate accounting bucket and `recreated == len(plan_recreate)` is 0 == 0.
# That is precisely the failure W5-MAPFINISH §2.5 warned a balanced
# reconciliation cannot catch.
#
# WHY A DECLARED LIST AND NOT A CLEVERER GEOMETRIC RULE. The obvious repair is
# to promote the mirror-donor test (`usable`) to a proof of its own. It is not
# one: this arena is symmetric in both axes, so for a family that genuinely LOST
# members - the goal ring went 32 spokes -> 26 - a retired member reflects onto
# a surviving sibling at ~0 uu and would be "re-created" as a duplicate of a
# piece the builder deliberately stopped emitting. A rule that fires on the real
# case and on that one too is not a rule. So the list is explicit and auditable,
# the same way DELETION_ALLOWLIST and MESH_RENAMES carry map-specific facts, and
# a declared label STILL has to pass the mirror-donor test before it is acted on
# - declaration alone re-creates nothing, so a stale or mistyped label degrades
# to the documented-loss path with a log line rather than resurrecting junk.
HAND_DUPLICATE_LABELS = tuple(
    part.strip() for part in os.environ.get("TRACE_REBAKE_HAND_DUPLICATES", "").split(",")
    if part.strip())

# Which piece TRACE_REBAKE_PHASE=probe clones as its throwaway. Default is the
# top-centre tower's BODY, because it is the piece the nine re-creations most
# need to get right and the only one of them with more than one material.
PROBE_DONOR_LABEL = os.environ.get("TRACE_REBAKE_PROBE_DONOR", "Top_Centre_Tower_01")

# MAP §10.4's kit asset renames, as (old package path, new package path). The
# census records the mesh a hand actor wore by PATH, and these four assets were
# renamed after that census was taken, so the restore has to move the path when
# it re-spawns the kit. Scripts/rename-kit-assets.py owns the rename itself and
# carries the same four pairs; changing one means changing both.
_MAPGEO = "/Game/Trace/Art/Pack/MapGeometry"
MESH_RENAMES = [
    ("{0}/octagon".format(_MAPGEO),   "{0}/SM_KitOctagon".format(_MAPGEO)),
    ("{0}/platform1".format(_MAPGEO), "{0}/SM_KitPlatform".format(_MAPGEO)),
    ("{0}/ramp1".format(_MAPGEO),     "{0}/SM_KitRamp".format(_MAPGEO)),
    ("{0}/ramp".format(_MAPGEO),      "{0}/SM_KitRampAlt".format(_MAPGEO)),
]


def log(message):
    if IN_EDITOR:
        unreal.log("[Rebake] {0}".format(message))
    else:
        sys.stdout.write("[Rebake] {0}\n".format(message))
        sys.stdout.flush()


def log_error(message):
    if IN_EDITOR:
        unreal.log_error("[Rebake] {0}".format(message))
    else:
        sys.stderr.write("[Rebake][ERROR] {0}\n".format(message))
        sys.stderr.flush()


def label_stem(label):
    """`Bank_Skin_13` -> `Bank_Skin`. The bake numbers every label `_%02d`."""
    return re.sub(r"_\d+$", "", label)


def normalised(label):
    return label.replace("_", "").upper()


def is_allowlisted(label):
    key = normalised(label)
    for prefix in DELETION_ALLOWLIST:
        if key.startswith(prefix):
            return prefix
    return None


# =============================================================================
# EDITOR SIDE
# =============================================================================

if IN_EDITOR:

    EAL = unreal.EditorAssetLibrary

    def actor_subsystem():
        return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    def level_subsystem():
        return unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

    def open_map():
        if not EAL.does_asset_exist(MAP_PATH):
            raise RuntimeError("{0} does not exist.".format(MAP_PATH))
        if not level_subsystem().load_level(MAP_PATH):
            raise RuntimeError("could not open {0}".format(MAP_PATH))
        log("Opened {0}".format(MAP_PATH))

    def tags_of(actor):
        try:
            return [str(t) for t in actor.get_editor_property("tags")]
        except Exception:  # noqa: BLE001 - an actor with no tags array is still an actor
            return []

    def folder_of(actor):
        try:
            return str(actor.get_folder_path())
        except Exception:  # noqa: BLE001
            return ""

    def transform_of(actor):
        loc = actor.get_actor_location()
        rot = actor.get_actor_rotation()
        scale = actor.get_actor_scale3d()
        return {
            "location": [float(loc.x), float(loc.y), float(loc.z)],
            "rotation": [float(rot.pitch), float(rot.yaw), float(rot.roll)],
            "scale": [float(scale.x), float(scale.y), float(scale.z)],
        }

    def renamed_mesh_path(path, label):
        """A census mesh path, moved to wherever MAP §10.4 renamed that asset to.

        The census is the durable record of the map AS A HUMAN LAST LEFT IT and is
        never rewritten, so it still names `octagon`, `platform1`, `ramp1` and
        `ramp`. Scripts/rename-kit-assets.py renamed those four, which leaves
        redirectors that LoadObject would quietly follow - and quietly is the
        problem: a redirector deleted between the rename and a later restore would
        turn a silent success into a hard failure months from now, with nothing in
        the log to say what changed. So the move is applied EXPLICITLY and logged.
        MESH_RENAMES and rename-kit-assets.py's RENAMES are the same four pairs.
        """
        if not path:
            return path
        # `/Game/.../ramp1.ramp1` -> package `/Game/.../ramp1`, object `ramp1`.
        # Split rather than str.replace: `.../ramp1` starts with `.../ramp`, and a
        # replace on the whole path would rename the package and leave the OBJECT
        # called `ramp1` inside `SM_KitRamp`, which loads nothing.
        package, dot, _object = path.partition(".")
        for old, new in MESH_RENAMES:
            if package != old:
                continue
            moved = new + (dot + new.rsplit("/", 1)[-1] if dot else "")
            log("hand actor '{0}': mesh {1} -> {2} (MAP §10.4 rename)".format(label, path, moved))
            return moved
        return path

    def load_by_path(path):
        """`/Game/X/Y.Y` -> the object. load_object is the direct route; the asset
        library is the fallback for a path the object loader will not resolve."""
        if not path:
            return None
        try:
            obj = unreal.load_object(None, path)
            if obj is not None:
                return obj
        except Exception:  # noqa: BLE001
            pass
        return EAL.load_asset(path.split(".")[0])

    def asset_path(obj):
        if obj is None:
            return ""
        try:
            return str(obj.get_path_name())
        except Exception:  # noqa: BLE001
            return ""

    def static_mesh_component(actor):
        for prop in ("static_mesh_component",):
            try:
                comp = actor.get_editor_property(prop)
                if comp is not None:
                    return comp
            except Exception:  # noqa: BLE001 - not every actor has one
                pass
        comps = actor.get_components_by_class(unreal.StaticMeshComponent)
        return comps[0] if comps else None

    def light_component(actor):
        for prop in ("point_light_component", "light_component"):
            try:
                comp = actor.get_editor_property(prop)
                if comp is not None:
                    return comp
            except Exception:  # noqa: BLE001
                pass
        comps = actor.get_components_by_class(unreal.PointLightComponent)
        return comps[0] if comps else None

    def describe_mesh_component(comp):
        """Everything the restore step has to put back. Collision lives on the
        component's FBodyInstance and is NOT reachable through
        get_editor_property in 5.8 — the getters are (W2-KIT measured it)."""
        if comp is None:
            return None
        mesh = comp.get_editor_property("static_mesh")
        materials = []
        try:
            for index in range(comp.get_num_materials()):
                materials.append(asset_path(comp.get_material(index)))
        except Exception:  # noqa: BLE001
            pass
        out = {
            "mesh": asset_path(mesh),
            "materials": materials,
            "relative_location": None,
            "relative_scale": None,
        }
        try:
            rel = comp.get_relative_transform()
            out["relative_location"] = [float(v) for v in
                                        (rel.translation.x, rel.translation.y, rel.translation.z)]
            out["relative_scale"] = [float(v) for v in
                                     (rel.scale3d.x, rel.scale3d.y, rel.scale3d.z)]
        except Exception:  # noqa: BLE001
            pass
        for key, getter in (("mobility", "mobility"),):
            try:
                out[key] = str(comp.get_editor_property(getter))
            except Exception:  # noqa: BLE001
                out[key] = ""
        for key, call in (("collision_enabled", "get_collision_enabled"),
                          ("collision_profile", "get_collision_profile_name"),
                          ("collision_object_type", "get_collision_object_type")):
            try:
                out[key] = str(getattr(comp, call)())
            except Exception:  # noqa: BLE001
                out[key] = ""
        try:
            out["cast_shadow"] = bool(comp.get_editor_property("cast_shadow"))
        except Exception:  # noqa: BLE001
            out["cast_shadow"] = True
        return out

    def describe_light_component(comp):
        if comp is None:
            return None
        out = {}
        for key in ("intensity", "attenuation_radius", "volumetric_scattering_intensity",
                    "cast_shadows", "mobility", "intensity_units", "source_radius"):
            try:
                value = comp.get_editor_property(key)
                out[key] = float(value) if isinstance(value, (int, float)) else (
                    bool(value) if isinstance(value, bool) else str(value))
            except Exception:  # noqa: BLE001
                pass
        try:
            colour = comp.get_editor_property("light_color")
            out["light_color"] = [int(colour.r), int(colour.g), int(colour.b)]
        except Exception:  # noqa: BLE001
            out["light_color"] = None
        return out

    # -------------------------------------------------------------------------
    # CLONING A BAKED PIECE WITHOUT UEditorActorSubsystem::DuplicateActor
    # -------------------------------------------------------------------------
    #
    # WHY THE OBVIOUS CALL CANNOT BE USED, from the engine source rather than
    # from the crash alone. UEditorActorSubsystem::DuplicateActors ends in
    #
    #     GUnrealEd->DuplicateActors(ActorsToDuplicate, NewActors, ToLevel, Offset);
    #
    # (UE_5.8 Engine/Source/Editor/UnrealEd/Private/Subsystems/EditorActorSubsystem.cpp:2671).
    # `GUnrealEd` is the UUnrealEdEngine global, and a `-run=pythonscript`
    # commandlet runs under a plain UEditorEngine, so it is NULL - which is
    # exactly the SIGSEGV W3-REBAKE captured: "invalid attempt to access memory
    # at address 0x0" with DuplicateActors on top of the stack
    # (Saved/Logs/release/W3-REBAKE-restore-backup-2026.08.25-04.01.56.log).
    # Nothing about the actors, the level or the transaction is wrong; the call
    # simply has no editor engine to do the work. It cannot be made to work
    # headless, so this script builds the copy itself.
    #
    # WHAT A BAKED PIECE IS MADE OF, and therefore what has to be copied:
    # ATraceBakedPiece is a plain AActor with a USceneComponent named PieceRoot
    # and everything else added through AddInstanceComponent
    # (TraceArenaBake.cpp:1100/1135/1162) - UBoxComponents for collision and
    # pawn standoff, UInstancedStaticMeshComponents for repetition, and loose
    # UStaticMeshComponents below the collapse threshold. So the clone spawns a
    # fresh ATraceBakedPiece and re-adds each of those, copying the same
    # properties EmitBakedActors sets, in the same order.
    #
    # THE ROUTE THAT WORKS HEADLESS is USubobjectDataSubsystem::AddNewSubobject.
    # Its non-Blueprint branch (Engine/Source/Editor/SubobjectDataInterface/
    # Private/SubobjectDataSubsystem.cpp:1093-1200) does precisely what the bake
    # does by hand - NewObject on the actor, attach to the root, AddInstanceComponent,
    # OnComponentCreated, RegisterComponent - and it touches no editor-engine
    # global. It is BlueprintCallable, so Python can reach it; AActor::AddInstanceComponent
    # and AddComponentByClass cannot be called directly (no UFUNCTION / ScriptNoExport).
    def subobject_subsystem():
        try:
            return unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
        except Exception:  # noqa: BLE001 - the module may not be loaded in this configuration
            return None

    def add_component(actor, component_class):
        """A new instance component of @p component_class on @p actor, or None.

        The new component is identified by DIFFING the actor's component list
        rather than by resolving the returned FSubobjectDataHandle: the diff
        needs no handle API at all, and it is correct even if a future engine
        returns the handle in a different shape.
        """
        subsystem = subobject_subsystem()
        if subsystem is None:
            return None

        try:
            handles = subsystem.k2_gather_subobject_data_for_instance(actor)
        except Exception as exc:  # noqa: BLE001
            log_error("gather subobject data failed on {0}: {1}".format(actor.get_actor_label(), exc))
            return None
        if not handles:
            log_error("no subobject handles for {0}".format(actor.get_actor_label()))
            return None

        before = set()
        for comp in actor.get_components_by_class(unreal.ActorComponent):
            before.add(comp.get_name())

        params = unreal.AddNewSubobjectParams()
        params.set_editor_property("parent_handle", handles[0])
        params.set_editor_property("new_class", component_class)
        params.set_editor_property("blueprint_context", None)

        try:
            result = subsystem.add_new_subobject(params)
        except Exception as exc:  # noqa: BLE001
            log_error("add_new_subobject({0}) failed: {1}".format(component_class.get_name(), exc))
            return None

        # The FailReason out-parameter makes this a tuple in Python; a build that
        # returns the bare handle is handled by not caring which we got.
        if isinstance(result, tuple) and len(result) > 1 and result[1] is not None:
            reason = str(result[1])
            if reason:
                log("add_new_subobject reported: {0}".format(reason))

        for comp in actor.get_components_by_class(unreal.ActorComponent):
            if comp.get_name() not in before:
                return comp
        return None

    def copy_scene_component_basics(source, target):
        target.set_editor_property("mobility", source.get_editor_property("mobility"))
        target.set_relative_transform(source.get_relative_transform(), False, False)

    # The primitive flags EmitBakedActors sets on every component it makes
    # (TraceArenaBake.cpp:1088-1099 for boxes, :1122-1128 and :1152-1158 for
    # meshes). Each is tried under the spellings the Python bindings have used -
    # UE strips the `b` from a UPROPERTY name, but not consistently across
    # versions - and ANYTHING THAT COULD NOT BE CARRIED IS LOGGED rather than
    # swallowed: a clone that silently lost `cast_shadow` would look right in
    # every count in the reconciliation and wrong in the frame.
    PRIMITIVE_FLAGS = (
        ("cast_shadow",),
        ("generate_overlap_events", "b_generate_overlap_events"),
        ("can_ever_affect_navigation", "b_can_ever_affect_navigation"),
        ("hidden_in_game", "b_hidden_in_game"),
    )

    def copy_primitive_flags(source, target, label):
        for names in PRIMITIVE_FLAGS:
            carried = False
            for name in names:
                try:
                    target.set_editor_property(name, source.get_editor_property(name))
                    carried = True
                    break
                except Exception:  # noqa: BLE001 - not this component class, or not this spelling
                    continue
            if not carried:
                log("clone '{0}': component flag {1} could not be carried".format(label, names[0]))
        try:
            target.set_collision_profile_name(source.get_collision_profile_name())
        except Exception as exc:  # noqa: BLE001
            log("clone '{0}': collision profile could not be carried: {1}".format(label, exc))

        # THE PER-CHANNEL RESPONSES, AFTER THE PROFILE, AND THIS IS THE ONE THAT
        # ACTUALLY MATTERED. The bake's collision boxes carry the profile name
        # "Custom" (TraceArenaBake.cpp:1092-1095 copies the profile AND the
        # response container from the source component), and setting a profile
        # called "Custom" restores nothing - it is the name a component wears
        # when its responses were set channel by channel. A clone that copied
        # only the profile came back with QUERY_AND_PHYSICS enabled, the right
        # box extent and the right transform, and did not block anything:
        # measured on the first pass, a round fired down onto the re-created
        # tower read 1320.6 uu - the bare floor - while the surviving half read
        # 705.2 uu. Structurally identical, functionally dead.
        copy_collision_responses(source, target, label)

    def collision_enabled_of(component):
        try:
            return str(component.get_collision_enabled()).split(".")[-1].split(":")[0].strip()
        except Exception:  # noqa: BLE001
            return "NO_COLLISION"

    def collision_channels():
        """Every ECollisionChannel this build exposes, or the standard set."""
        try:
            return list(unreal.CollisionChannel)
        except Exception:  # noqa: BLE001 - not iterable in this binding
            names = ["ECC_WORLD_STATIC", "ECC_WORLD_DYNAMIC", "ECC_PAWN", "ECC_VISIBILITY",
                     "ECC_CAMERA", "ECC_PHYSICS_BODY", "ECC_VEHICLE", "ECC_DESTRUCTIBLE"]
            names += ["ECC_GAME_TRACE_CHANNEL{0}".format(i) for i in range(1, 19)]
            out = []
            for name in names:
                channel = getattr(unreal.CollisionChannel, name, None)
                if channel is not None:
                    out.append(channel)
            return out

    def collision_response_table(component):
        table = {}
        for channel in collision_channels():
            try:
                table[str(channel)] = str(component.get_collision_response_to_channel(channel))
            except Exception:  # noqa: BLE001
                pass
        return table

    def copy_collision_responses(source, target, label):
        carried = 0
        for channel in collision_channels():
            try:
                target.set_collision_response_to_channel(
                    channel, source.get_collision_response_to_channel(channel))
                carried += 1
            except Exception:  # noqa: BLE001
                continue
        if carried == 0:
            log_error("clone '{0}': NO collision responses could be carried - the clone will "
                      "not block anything".format(label))
        return carried

    def clone_mesh_component(source, target):
        target.set_static_mesh(source.get_editor_property("static_mesh"))
        try:
            for index in range(source.get_num_materials()):
                material = source.get_material(index)
                if material is not None:
                    target.set_material(index, material)
        except Exception:  # noqa: BLE001
            pass

    def clone_baked_piece(donor, location, rotation, scale, label, folder):
        """A fresh ATraceBakedPiece carrying @p donor's geometry, at a new transform.

        Returns the new actor, or None if this engine cannot do it (in which case
        the caller reports the piece as lost, exactly as before).
        """
        if subobject_subsystem() is None:
            log_error("USubobjectDataSubsystem is not available to Python in this build; "
                      "cannot re-create '{0}' - see the note above clone_baked_piece".format(label))
            return None

        copy = actor_subsystem().spawn_actor_from_class(
            unreal.TraceBakedPiece, unreal.Vector(*location),
            unreal.Rotator(rotation[2], rotation[0], rotation[1]))
        if copy is None:
            log_error("spawn of a TraceBakedPiece for '{0}' failed".format(label))
            return None
        copy.set_actor_scale3d(unreal.Vector(*scale))

        # The two pieces of state ATraceBakedPiece carries for the runtime builder
        # (TraceBakedPiece.h): which scoring shape presents it, and which of its
        # materials the half-time switch repaints. A copy that dropped SideTints
        # would be a piece the side switch silently stopped reaching.
        for key in ("scoring_mode_tag", "side_tints"):
            try:
                copy.set_editor_property(key, donor.get_editor_property(key))
            except Exception as exc:  # noqa: BLE001
                log_error("clone '{0}': could not carry {1}: {2}".format(label, key, exc))

        tags = donor.get_editor_property("tags")
        copy.set_editor_property("tags", [unreal.Name(str(tag)) for tag in tags])

        root = copy.get_editor_property("piece_root")
        copied = 0
        for source in donor.get_components_by_class(unreal.SceneComponent):
            # PieceRoot itself is created by the constructor on both actors.
            if root is not None and source.get_name() == root.get_name():
                continue
            if isinstance(source, unreal.InstancedStaticMeshComponent):
                target = add_component(copy, unreal.InstancedStaticMeshComponent)
                if target is None:
                    continue
                copy_scene_component_basics(source, target)
                clone_mesh_component(source, target)
                copy_primitive_flags(source, target, label)
                for index in range(source.get_instance_count()):
                    ok, xform = source.get_instance_transform(index, world_space=False)
                    if ok:
                        target.add_instance(xform, False)
            elif isinstance(source, unreal.BoxComponent):
                target = add_component(copy, unreal.BoxComponent)
                if target is None:
                    continue
                copy_scene_component_basics(source, target)
                target.set_box_extent(source.get_unscaled_box_extent(), False)
                copy_primitive_flags(source, target, label)
                try:
                    target.set_collision_enabled(source.get_collision_enabled())
                    target.set_collision_object_type(source.get_collision_object_type())
                except Exception:  # noqa: BLE001
                    pass
            elif isinstance(source, unreal.StaticMeshComponent):
                target = add_component(copy, unreal.StaticMeshComponent)
                if target is None:
                    continue
                copy_scene_component_basics(source, target)
                clone_mesh_component(source, target)
                copy_primitive_flags(source, target, label)
            else:
                continue
            copied += 1

        copy.set_actor_label(label)
        if folder and folder != "None":
            copy.set_folder_path(unreal.Name(folder))

        # THREE ASSERTIONS BEFORE THIS COUNTS AS A CLONE.
        #
        # (1) The root must still be PieceRoot. AddNewSubobject makes the first
        #     component it adds the actor's ROOT if the parent handle reports no
        #     children (SubobjectDataSubsystem.cpp:1147) - which would silently
        #     produce a piece whose transform belongs to a collision box.
        # (2) The component count must match the donor's. A clone that landed at
        #     the right transform with half its geometry would pass every count in
        #     the reconciliation and be wrong only in the frame.
        # (3) EVERY COLLIDING COMPONENT'S RESPONSE TABLE must match the donor's,
        #     channel for channel. (1) and (2) both passed on the first run of
        #     this while the re-created tower blocked nothing at all, because the
        #     shape, the extent, the transform and the enabled flag were all
        #     right and the response table was default. An assertion about
        #     structure cannot see that; this one can, and it is the reason the
        #     collision probe in the tranche report is a NUMBER and not a frame.
        wanted = max(0, len(donor.get_components_by_class(unreal.SceneComponent)) - 1)
        root_now = copy.get_editor_property("root_component")
        if root_now is None or root_now.get_name() != root.get_name():
            log_error("clone '{0}': root component is {1}, not PieceRoot - discarding the clone".format(
                label, "None" if root_now is None else root_now.get_name()))
            actor_subsystem().destroy_actor(copy)
            return None
        if copied != wanted:
            log_error("clone '{0}': {1} of {2} components came across - discarding the clone".format(
                label, copied, wanted))
            actor_subsystem().destroy_actor(copy)
            return None

        donor_boxes = [c for c in donor.get_components_by_class(unreal.PrimitiveComponent)
                       if collision_enabled_of(c) != "NO_COLLISION"]
        copy_boxes = [c for c in copy.get_components_by_class(unreal.PrimitiveComponent)
                      if collision_enabled_of(c) != "NO_COLLISION"]
        if len(donor_boxes) != len(copy_boxes):
            log_error("clone '{0}': {1} colliding components, donor has {2} - discarding".format(
                label, len(copy_boxes), len(donor_boxes)))
            actor_subsystem().destroy_actor(copy)
            return None
        for index, donor_box in enumerate(donor_boxes):
            want_table = collision_response_table(donor_box)
            have_table = collision_response_table(copy_boxes[index])
            if not want_table or want_table != have_table:
                differing = sorted(k for k in want_table
                                   if have_table.get(k) != want_table[k]) or ["<empty table>"]
                log_error("clone '{0}': collision responses differ on {1} channel(s) "
                          "({2}) - discarding the clone".format(
                              label, len(differing), ", ".join(differing[:6])))
                actor_subsystem().destroy_actor(copy)
                return None

        log("cloned {0} -> {1}: {2} of {3} components".format(
            donor.get_actor_label(), label, copied, wanted))
        return copy

    def bounds_of(actor):
        try:
            origin, extent = actor.get_actor_bounds(False)
            return ([float(origin.x), float(origin.y), float(origin.z)],
                    [float(extent.x), float(extent.y), float(extent.z)])
        except Exception:  # noqa: BLE001
            return ([0.0, 0.0, 0.0], [0.0, 0.0, 0.0])

    # -------------------------------------------------------------------------
    # Classification: which side of the governance line is this actor on?
    # -------------------------------------------------------------------------

    def classify(actor):
        """'hand', 'piece', or 'bake' (the bake's gameplay/lighting actors)."""
        tags = tags_of(actor)

        if KIT_TAG in tags:
            return "hand"

        if isinstance(actor, unreal.TraceBakedPiece):
            return "piece"

        if isinstance(actor, unreal.StaticMeshActor):
            comp = static_mesh_component(actor)
            mesh = asset_path(comp.get_editor_property("static_mesh")) if comp is not None else ""
            if BAKED_TAG not in tags and not mesh.startswith("/Engine/"):
                return "hand"
            return "bake"

        if isinstance(actor, unreal.PointLight):
            label = actor.get_actor_label()
            if FLOOR_LAMP_TAG not in tags or not BAKED_LAMP_LABEL.match(label):
                return "hand"
            return "bake"

        return "bake"

    def census_world(what):
        """Walk the level once and sort it. `what` only names the census in the log."""
        actors = actor_subsystem().get_all_level_actors()

        hand = []
        pieces = []
        others = []
        per_class = {}

        for actor in actors:
            if not actor:
                continue
            class_name = actor.get_class().get_name()
            per_class[class_name] = per_class.get(class_name, 0) + 1

            kind = classify(actor)
            label = actor.get_actor_label()
            origin, extent = bounds_of(actor)

            record = {
                "class": class_name,
                "label": label,
                "folder": folder_of(actor),
                "tags": tags_of(actor),
                "bounds_origin": origin,
                "bounds_extent": extent,
            }
            record.update(transform_of(actor))

            if kind == "hand":
                if isinstance(actor, unreal.PointLight):
                    record["light"] = describe_light_component(light_component(actor))
                else:
                    record["mesh_component"] = describe_mesh_component(static_mesh_component(actor))
                hand.append(record)
            elif kind == "piece":
                try:
                    record["scoring_mode_tag"] = str(actor.get_editor_property("scoring_mode_tag"))
                except Exception:  # noqa: BLE001
                    record["scoring_mode_tag"] = ""
                try:
                    record["side_tints"] = len(actor.get_editor_property("side_tints"))
                except Exception:  # noqa: BLE001
                    record["side_tints"] = 0
                mats = set()
                for comp in actor.get_components_by_class(unreal.MeshComponent):
                    try:
                        for index in range(comp.get_num_materials()):
                            path = asset_path(comp.get_material(index))
                            if path:
                                mats.add(path)
                    except Exception:  # noqa: BLE001
                        pass
                record["materials"] = sorted(mats)
                pieces.append(record)
            else:
                others.append(record)

        side_tint_total = sum(p.get("side_tints", 0) for p in pieces)
        log("{0}: {1} actors = {2} baked pieces + {3} hand actors + {4} bake gameplay/lighting actors"
            .format(what, len(actors), len(pieces), len(hand), len(others)))
        log("{0}: {1} FTraceBakedSideTint entries across the pieces".format(what, side_tint_total))
        for name in sorted(per_class, key=lambda k: (-per_class[k], k)):
            log("{0}:   {1:<28} {2}".format(what, name, per_class[name]))

        return {
            "actor_count": len(actors),
            "per_class": per_class,
            "pieces": pieces,
            "hand": hand,
            "others": others,
            "side_tint_total": side_tint_total,
        }

    # -------------------------------------------------------------------------
    # PHASE 1 - census
    # -------------------------------------------------------------------------

    def phase_census():
        open_map()
        data = census_world("CENSUS-A")

        for record in data["hand"]:
            log("CENSUS-A hand: {0:<20} {1:<18} at ({2:.1f}, {3:.1f}, {4:.1f}) tags={5}".format(
                record["label"], record["class"],
                record["location"][0], record["location"][1], record["location"][2],
                ",".join(record["tags"]) or "-"))

        stems = {}
        for piece in data["pieces"]:
            stem = label_stem(piece["label"])
            stems[stem] = stems.get(stem, 0) + 1
        for stem in sorted(stems):
            log("CENSUS-A stem: {0:<28} {1}".format(stem, stems[stem]))

        out = {
            "map": MAP_PATH,
            "engine": str(unreal.SystemLibrary.get_engine_version()),
            # Stamped so phase_restore can refuse a STALE census. A census records the
            # hand layer's transforms as they were when it ran; restoring from an old one
            # silently reverts every hand edit made since. That is not hypothetical — the
            # side ramps were re-scaled and re-sunk to make them surfable, and the census
            # sitting on disk still held their old shallow transforms, so a bare restore
            # would have quietly undone the whole change and still reconciled clean.
            "captured_unix": time.time(),
            "captured_utc": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
            "census_a": data,
            "stems_a": stems,
        }
        directory = os.path.dirname(CENSUS_PATH)
        if directory and not os.path.isdir(directory):
            os.makedirs(directory)
        with open(CENSUS_PATH, "w") as handle:
            json.dump(out, handle, indent=1, sort_keys=True)
        log("Wrote {0}".format(CENSUS_PATH))
        log("PHASE census OK")

    # -------------------------------------------------------------------------
    # PHASE 3 - replay + restore + reconcile
    # -------------------------------------------------------------------------

    def separation(left, right):
        return sum((left["location"][axis] - right["location"][axis]) ** 2
                   for axis in range(3)) ** 0.5

    def nearest_same_stem(record, by_stem):
        stem = label_stem(record["label"])
        best = None
        best_distance = None
        for other in by_stem.get(stem, ()):
            distance = separation(other, record)
            if best_distance is None or distance < best_distance:
                best_distance = distance
                best = other
        return best, best_distance

    # The four rigid transforms a person can use to copy a cluster to the other
    # side of an arena that is symmetric in both axes. A hand DUPLICATE is a copy
    # of some existing piece under one of them, so the donor search tries all four
    # and keeps the best - see donor_for_duplicate.
    MIRRORS = (
        ("identity", 1.0, 1.0),
        ("mirror-X", -1.0, 1.0),
        ("mirror-Y", 1.0, -1.0),
        ("mirror-XY", -1.0, -1.0),
    )

    # How close the mirrored position has to land on a fresh sibling before the
    # pairing is called a proof rather than a guess. Measured on this map: under
    # mirror-Y all nine lost top-centre-tower copies land 10.4-15.0 uu from their
    # counterpart, one to one, while the identity transform's nearest candidate is
    # 4,059-5,190 uu away. Two orders of magnitude is not a threshold that needs
    # tuning; 250 uu sits in the empty middle of it.
    DUPLICATE_MIRROR_RADIUS_UU = 250.0

    def recreate_location(record, donor):
        """Where a re-created hand duplicate is put: the census X and Y, and the
        donor's Z unless TRACE_REBAKE_RECREATE_Z says otherwise. See plan_recreate."""
        z = record["location"][2] if RECREATE_Z_SOURCE == "census" else donor["location"][2]
        return [record["location"][0], record["location"][1], z]

    def donor_for_duplicate(record, by_stem):
        """The fresh piece a hand duplicate was copied FROM, and how it was copied.

        WHY NOT nearest_same_stem. That answers "which sibling is closest", which
        for a MIRRORED cluster is the wrong question and gives a wrong answer with
        real consequences here: `Top_Centre_Tower_2` is the cluster's 1,205 uu-wide
        body (three materials, bounds extent 602 uu), and its nearest same-stem
        neighbour is `Top_Centre_Tower_02`, a 37 uu mast. Cloning that donor would
        put a thin post where the tower body belongs and nobody would see it in a
        reconciliation that still balanced.

        Reflecting the duplicate back across the arena's own axes and asking which
        sibling it lands ON pairs body with body and mast with mast, one to one.

        Returns (donor, transform_name, distance) or (None, None, None).
        """
        stem = label_stem(record["label"])
        candidates = by_stem.get(stem, ())
        best = (None, None, None)
        for name, sign_x, sign_y in MIRRORS:
            probe = {
                "location": [record["location"][0] * sign_x,
                             record["location"][1] * sign_y,
                             record["location"][2]],
            }
            for other in candidates:
                distance = separation(other, probe)
                if best[2] is None or distance < best[2]:
                    best = (other, name, distance)
        return best

    def match_pieces(old_pieces, new_pieces):
        """Pair census pieces with fresh pieces, one-to-one, WITHIN a family, by
        ascending distance. See NEIGHBOUR_RADIUS_UU for why this is not done by
        label.

        Greedy-by-distance is the right shape here and not just the easy one: the
        pieces of a family are spread far apart compared with how far any of them
        moved, so the globally optimal assignment and the greedy one agree, and
        greedy makes the leftovers meaningful - the fresh pieces left unclaimed
        are exactly the ones furthest from anything the census still had, which is
        the definition of "a human cleared this spot".

        Returns (pairs, unmatched_old, unmatched_new).
        """
        old_by_stem = {}
        new_by_stem = {}
        for piece in old_pieces:
            old_by_stem.setdefault(label_stem(piece["label"]), []).append(piece)
        for piece in new_pieces:
            new_by_stem.setdefault(label_stem(piece["label"]), []).append(piece)

        pairs = []
        unmatched_old = []
        unmatched_new = []

        for stem in sorted(set(old_by_stem) | set(new_by_stem)):
            olds = old_by_stem.get(stem, [])
            news = new_by_stem.get(stem, [])
            if not olds:
                unmatched_new.extend(news)
                continue
            if not news:
                unmatched_old.extend(olds)
                continue

            candidates = []
            for old_index, old in enumerate(olds):
                for new_index, new in enumerate(news):
                    candidates.append((separation(old, new), old_index, new_index))
            candidates.sort()

            taken_old = set()
            taken_new = set()
            for distance, old_index, new_index in candidates:
                if old_index in taken_old or new_index in taken_new:
                    continue
                taken_old.add(old_index)
                taken_new.add(new_index)
                pairs.append((olds[old_index], news[new_index], distance))
                if len(taken_old) == len(olds) or len(taken_new) == len(news):
                    break

            unmatched_old.extend(olds[i] for i in range(len(olds)) if i not in taken_old)
            unmatched_new.extend(news[i] for i in range(len(news)) if i not in taken_new)

        return pairs, unmatched_old, unmatched_new

    def boxes_overlap(origin_a, extent_a, origin_b, extent_b, slack=0.0):
        for axis in range(3):
            if abs(origin_a[axis] - origin_b[axis]) > (extent_a[axis] + extent_b[axis] + slack):
                return False
        return True

    def spawn_hand_actor(record):
        subsystem = actor_subsystem()
        location = unreal.Vector(*record["location"])
        rotation = unreal.Rotator(record["rotation"][2], record["rotation"][0], record["rotation"][1])

        class_name = record["class"]
        if class_name == "PointLight":
            actor = subsystem.spawn_actor_from_class(unreal.PointLight, location, rotation)
        elif class_name == "StaticMeshActor":
            actor = subsystem.spawn_actor_from_class(unreal.StaticMeshActor, location, rotation)
        else:
            cls = getattr(unreal, class_name, None)
            if cls is None:
                raise RuntimeError("no Python class for hand actor '{0}' ({1})".format(
                    record["label"], class_name))
            actor = subsystem.spawn_actor_from_class(cls, location, rotation)

        if actor is None:
            raise RuntimeError("spawn failed for hand actor '{0}'".format(record["label"]))

        actor.set_actor_scale3d(unreal.Vector(*record["scale"]))

        if class_name == "StaticMeshActor":
            comp = static_mesh_component(actor)
            spec = record.get("mesh_component") or {}
            if comp is not None:
                # Mobility FIRST: a Static component refuses a mesh swap in some
                # paths, and every one of these actors is Static in the census.
                mobility = spec.get("mobility", "")
                if "MOVABLE" in mobility.upper():
                    comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
                elif "STATIONARY" in mobility.upper():
                    comp.set_editor_property("mobility", unreal.ComponentMobility.STATIONARY)
                else:
                    comp.set_editor_property("mobility", unreal.ComponentMobility.STATIC)

                mesh_path = renamed_mesh_path(spec.get("mesh", ""), record["label"])
                if mesh_path:
                    mesh = load_by_path(mesh_path)
                    if mesh is None:
                        raise RuntimeError("hand actor '{0}': mesh {1} did not load".format(
                            record["label"], mesh_path))
                    comp.set_static_mesh(mesh)

                # Only genuine OVERRIDES are re-applied. Re-setting a slot to the
                # asset's own default would turn an inherited material into a
                # per-actor override, which is a real difference in the .uasset
                # and would defeat the point of theming the mesh asset.
                defaults = []
                mesh_obj = comp.get_editor_property("static_mesh")
                if mesh_obj is not None:
                    try:
                        for slot in mesh_obj.get_editor_property("static_materials"):
                            defaults.append(asset_path(slot.get_editor_property("material_interface")))
                    except Exception:  # noqa: BLE001
                        defaults = []
                for index, path in enumerate(spec.get("materials", [])):
                    if not path:
                        continue
                    if index < len(defaults) and defaults[index] == path:
                        continue
                    material = load_by_path(path)
                    if material is not None:
                        comp.set_material(index, material)

                profile = spec.get("collision_profile", "")
                if profile:
                    comp.set_collision_profile_name(profile)
                comp.set_editor_property("cast_shadow", spec.get("cast_shadow", True))

        elif class_name == "PointLight":
            comp = light_component(actor)
            spec = record.get("light") or {}
            if comp is not None:
                comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
                for key in ("intensity", "attenuation_radius", "volumetric_scattering_intensity",
                            "cast_shadows", "source_radius"):
                    if key in spec:
                        try:
                            comp.set_editor_property(key, spec[key])
                        except Exception:  # noqa: BLE001 - a dial the build does not expose
                            pass
                colour = spec.get("light_color")
                if colour:
                    # RED AND BLUE SWAPPED HERE ON EVERY RESTORE UNTIL 2026-09-01.
                    #
                    # unreal.Color's POSITIONAL constructor is FColor(B, G, R, A) — blue first —
                    # while the census at the top of this file stores [r, g, b] in that order. So
                    # `unreal.Color(colour[0], colour[1], colour[2], 255)` assigned r->B and b->R and
                    # inverted the five hand-placed centre floor lamps on every single bake. Verified
                    # in the editor rather than inferred: unreal.Color(10, 20, 30, 40) reads back
                    # r=30 g=20 b=10.
                    #
                    # It went unnoticed because it is self-cancelling across an EVEN number of bakes,
                    # so the lamps flip-flopped between correct and inverted and any given inspection
                    # had even odds of looking right. The bake that found it happened to land on the
                    # correct parity, which is exactly why it had to be fixed rather than left alone —
                    # the next bake would have inverted them again.
                    #
                    # Setting the channels by NAME instead of by position so the order cannot be
                    # misread again, and so this line says what it means.
                    fixed = unreal.Color()
                    fixed.set_editor_property("r", int(colour[0]))
                    fixed.set_editor_property("g", int(colour[1]))
                    fixed.set_editor_property("b", int(colour[2]))
                    fixed.set_editor_property("a", 255)
                    comp.set_editor_property("light_color", fixed)

        for tag in record.get("tags", []):
            tags = actor.get_editor_property("tags")
            if unreal.Name(tag) not in tags:
                tags.append(unreal.Name(tag))
                actor.set_editor_property("tags", tags)
        actor.set_actor_label(record["label"])
        # "None" is what get_folder_path() stringifies an EMPTY FName to, and six
        # of the eleven collaborator actors genuinely sit at the outliner root.
        # Setting a folder literally called "None" would invent a folder nobody
        # asked for, which is a visible change to the layout this script exists
        # to preserve.
        folder = record.get("folder") or ""
        if folder and folder != "None":
            actor.set_folder_path(unreal.Name(folder))
        return actor

    def phase_restore():
        if not os.path.isfile(CENSUS_PATH):
            raise RuntimeError("{0} not found - run the census phase first".format(CENSUS_PATH))
        with open(CENSUS_PATH, "r") as handle:
            census = json.load(handle)

        # STALE-CENSUS GUARD. The full run does census -> bake -> restore in one go, so the
        # file is fresh by construction. But TRACE_REBAKE_PHASE lets restore run on its own,
        # and then whatever census happens to be on disk is treated as the truth about the
        # hand layer. An old one reverts every hand edit made since it was taken, reconciles
        # BALANCED, and reports success — the same shape as the unit-scale trap this script
        # already carries a guard for.
        captured = census.get("captured_unix")
        age_hours = (time.time() - captured) / 3600.0 if captured else None
        if captured is None:
            log("*** CENSUS HAS NO TIMESTAMP (written by an older version). Cannot tell whether "
                "it describes the CURRENT hand layer. Re-run the census phase unless you are "
                "certain nothing has moved since it was written. ***")
        else:
            log("Census was captured {0} ({1:.2f} hours ago).".format(
                census.get("captured_utc", "?"), age_hours))
            if age_hours > CENSUS_MAX_AGE_HOURS and not ALLOW_STALE_CENSUS:
                raise RuntimeError(
                    "census at {0} is {1:.1f} hours old (limit {2}). Restoring from it would "
                    "revert any hand edit made since it was taken. Re-run the census phase, or "
                    "set TRACE_REBAKE_ALLOW_STALE_CENSUS=1 if you have checked that nothing "
                    "in the hand layer has moved.".format(
                        CENSUS_PATH, age_hours, CENSUS_MAX_AGE_HOURS))

        census_a = census["census_a"]

        open_map()
        fresh = census_world("CENSUS-B")

        a_by_stem = {}
        for piece in census_a["pieces"]:
            a_by_stem.setdefault(label_stem(piece["label"]), []).append(piece)

        subsystem = actor_subsystem()
        live_pieces = {}
        for actor in subsystem.get_all_level_actors():
            if actor and isinstance(actor, unreal.TraceBakedPiece):
                live_pieces[actor.get_actor_label()] = actor

        b_by_stem_lookup = {}
        for piece in fresh["pieces"]:
            b_by_stem_lookup.setdefault(label_stem(piece["label"]), []).append(piece)

        pairs, unmatched_old, unmatched_new = match_pieces(census_a["pieces"], fresh["pieces"])

        # --- step 3: replay deletions ----------------------------------------
        plan_delete = []
        plan_keep = []
        for piece in unmatched_new:
            label = piece["label"]
            allow = is_allowlisted(label)
            stem = label_stem(label)
            neighbour, distance = nearest_same_stem(piece, a_by_stem)
            annotation = ("no same-stem piece survives in the census"
                          if neighbour is None else
                          "nearest surviving {0} is {1:.0f} uu away".format(
                              neighbour["label"], distance))

            if allow is not None:
                plan_keep.append((label, "allowlist:{0}".format(allow), annotation))
            elif stem not in a_by_stem:
                # The whole family is new. It cannot have been deleted by hand -
                # it never existed in the map the census read. This generalises
                # the allowlist so it cannot go stale the day the builder gains
                # another family, and every hit is logged as its own rule.
                plan_keep.append((label, "new-family", "stem '{0}' absent from the census".format(stem)))
            elif distance is not None and distance <= NEIGHBOUR_RADIUS_UU:
                # The family is still standing right here, so nothing was cleared
                # at this spot: this piece is a DENSIFIED family's extra member,
                # not a hole somebody made. See NEIGHBOUR_RADIUS_UU.
                plan_keep.append((label, "family-densified", annotation))
            else:
                plan_delete.append((label, "hand-deleted", annotation))

        # Census pieces the fresh bake has no counterpart for. Two different
        # things end up here and they must NOT be treated alike:
        #
        #   * pieces the builder retired (the goal ring dropped from 32 spokes to
        #     26). Nothing to do - the builder owns the family.
        #   * pieces a HUMAN duplicated. The census found nine: the collaborator
        #     copied the whole nine-piece top-centre tower cluster, mirrored it to
        #     Y -2590, and the editor labelled the copies _2.._10 by its own
        #     duplicate rule rather than the bake's _%02d. They carry
        #     TraceBakedArena (a copy carries its original's tags), so §10.1's
        #     mechanical hand-layer test - "any actor that does not carry the
        #     baked tag" - does not see them, and a re-bake would silently delete
        #     a whole structure a person built at mid-field.
        #
        # The non-unit actor scale separates them with no false positives (see
        # SCALE_TOLERANCE): all nine copies carry 0.75, the six retired spokes
        # carry 1. A copy is re-created by CLONING THE SIBLING IT WAS COPIED FROM,
        # so it arrives wearing the builder's new theming and its side-tint
        # entries - §10.1's rule that hand edits are honoured as layout while the
        # builder owns the materials. donor_for_duplicate finds that sibling by
        # reflecting the copy back across the arena's axes; nearest_same_stem is
        # NOT used here and the comment on donor_for_duplicate says what it got
        # wrong.
        #
        # ON BY DEFAULT SINCE W5-MAPFINISH, because the crash that turned it off
        # was in one engine call and that call is gone. UEditorActorSubsystem::
        # DuplicateActor ends in `GUnrealEd->DuplicateActors(...)` and GUnrealEd
        # is null in a `-run=pythonscript` commandlet (EditorActorSubsystem.cpp:2671;
        # the SIGSEGV at address 0x0 is in
        # Saved/Logs/release/W3-REBAKE-restore-backup-2026.08.25-04.01.56.log).
        # clone_baked_piece spawns the actor and re-adds its components through
        # USubobjectDataSubsystem instead, which touches no editor-engine global.
        # TRACE_REBAKE_RECREATE=0 restores the old behaviour - the pieces are then
        # reported as lost, with their transforms, exactly as W3-REBAKE reported
        # them.
        #
        # WHICH Z THE COPIES COME BACK AT, because the census and the builder do
        # not agree and the difference is not noise. The census has the whole
        # eighteen-piece cluster at actor scale (1,1,0.75) - a person selected it
        # and squashed it in Z about the world origin, which also multiplied every
        # actor's Z LOCATION by 0.75: the masts read 28.5 in the census and 38.0
        # in the fresh bake, and 38.0 x 0.75 = 28.5 exactly (the body reads 0.0 in
        # both, and 0.0 x 0.75 = 0.0). Rule R4 deliberately did NOT replay that
        # squash onto the nine SURVIVING pieces - the tower beacon the builder
        # starts at TowerHeight would float above a squashed tower - so putting the
        # copies back at the census Z with a unit scale would leave the re-created
        # half 9.5 uu below its own mirror image. The copies therefore take X and Y
        # from the census (the human's own plan positions, unaffected by a Z-only
        # squash) and Z and scale from the donor. TRACE_REBAKE_RECREATE_Z=census
        # takes the logged Z instead, for anyone who wants the other reading.
        vanished = []
        plan_recreate = []
        for piece in unmatched_old:
            hand_scaled = max(abs(value - 1.0) for value in piece["scale"]) > SCALE_TOLERANCE
            declared = piece["label"] in HAND_DUPLICATE_LABELS
            claimed = hand_scaled or declared
            donor, mirror, distance = donor_for_duplicate(piece, b_by_stem_lookup)
            usable = (donor is not None and distance is not None
                      and distance <= DUPLICATE_MIRROR_RADIUS_UU)
            if declared and not hand_scaled:
                log("DECLARED hand duplicate {0} (TRACE_REBAKE_HAND_DUPLICATES); its actor scale "
                    "is {1}, so the non-unit-scale proof does not apply - see "
                    "HAND_DUPLICATE_LABELS".format(
                        piece["label"], [round(v, 3) for v in piece["scale"]]))
            if RECREATE_DUPLICATES and claimed and usable:
                plan_recreate.append((piece, donor, mirror, distance))
            elif claimed:
                vanished.append(piece["label"])
                why = ("re-creation is off; see TRACE_REBAKE_RECREATE" if not RECREATE_DUPLICATES
                       else "no sibling within {0:.0f} uu of any mirror of it (best {1} at {2}) - "
                            "re-creating it would be a guess".format(
                                DUPLICATE_MIRROR_RADIUS_UU, mirror,
                                "none" if distance is None else "{0:.0f} uu".format(distance)))
                log("LOST hand-duplicated piece {0} (census transform loc={1} scale={2}) - {3}".format(
                    piece["label"], [round(v, 1) for v in piece["location"]],
                    [round(v, 3) for v in piece["scale"]], why))
            else:
                vanished.append(piece["label"])
        vanished = sorted(vanished)

        # --- step 4: replay transform edits ----------------------------------
        b_by_stem_count = {}
        for piece in fresh["pieces"]:
            stem = label_stem(piece["label"])
            b_by_stem_count[stem] = b_by_stem_count.get(stem, 0) + 1

        moved_candidates = []
        for old, new, _distance in pairs:
            d_loc = max(abs(old["location"][i] - new["location"][i]) for i in range(3))
            d_rot = max(abs(((old["rotation"][i] - new["rotation"][i]) + 180.0) % 360.0 - 180.0)
                        for i in range(3))
            d_scale = max(abs(old["scale"][i] - new["scale"][i]) for i in range(3))
            if (d_loc > TRANSLATION_TOLERANCE_UU or d_rot > ROTATION_TOLERANCE_DEG
                    or d_scale > SCALE_TOLERANCE):
                moved_candidates.append((new["label"], d_loc, d_rot, old))

        moved_per_stem = {}
        for label, _, _, _ in moved_candidates:
            stem = label_stem(label)
            moved_per_stem[stem] = moved_per_stem.get(stem, 0) + 1

        a_by_stem_count = {}
        for piece in census_a["pieces"]:
            stem = label_stem(piece["label"])
            a_by_stem_count[stem] = a_by_stem_count.get(stem, 0) + 1

        plan_transform = []
        plan_transform_skipped = []
        for label, d_loc, d_rot, old in moved_candidates:
            stem = label_stem(label)
            family = max(1, b_by_stem_count.get(stem, 1))
            fraction = moved_per_stem[stem] / float(family)
            family_wide = fraction >= FAMILY_MOVE_FRACTION
            resized = a_by_stem_count.get(stem, 0) != b_by_stem_count.get(stem, 0)
            hand_scaled = max(abs(value - 1.0) for value in old["scale"]) > SCALE_TOLERANCE
            if TRANSFORM_MODE == "none":
                plan_transform_skipped.append((label, d_loc, d_rot, "mode=none"))
            elif TRANSFORM_MODE == "all":
                plan_transform.append((label, d_loc, d_rot, old))
            elif resized:
                # THE OUTER GATE, and it is outside even the hand-scale proof on
                # purpose: a family whose MEMBER COUNT changed cannot be reasoned
                # about member by member, because neither side's members are the
                # same set of things any more.
                #
                # Two measured consequences, both of which this prevents:
                #   * the goal ring went 32 spokes -> 26, and four new spokes land
                #     41.9 uu from their nearest old one purely because 26 points
                #     do not sit on 32. Replaying that would pull four spokes off
                #     the ring's even spacing - a defect manufactured by the
                #     preservation step itself.
                #   * the top-centre tower group is 18 pieces in the census and 9
                #     fresh (a human mirrored the cluster to -Y and squashed all
                #     eighteen to Z 0.75). Replaying that 0.75 would leave
                #     W2-WORLD's new Tower_Beacon - which the builder starts at
                #     TowerHeight = 616 - floating 154 uu above a 462 uu tower.
                #     The family's definition changed under the hand edit, so the
                #     builder's definition wins, exactly as §10.1 says.
                plan_transform_skipped.append((label, d_loc, d_rot,
                                               "family '{0}' resized {1} -> {2}; the family's definition "
                                               "changed, so the builder's transform wins".format(
                                                   stem, a_by_stem_count.get(stem, 0),
                                                   b_by_stem_count.get(stem, 0))))
            elif hand_scaled:
                # PROOF, not a guess - see SCALE_TOLERANCE. The four centre cover
                # blocks are pitched 2 deg, sunk 10-20 uu and stretched 1.31/1.56
                # in Z; no bake produces that.
                plan_transform.append((label, d_loc, d_rot, old))
            elif family > 1 and family_wide:
                plan_transform_skipped.append((label, d_loc, d_rot,
                                               "{0}/{1} of family '{2}' moved together - "
                                               "builder geometry change, not a hand drag".format(
                                                   moved_per_stem[stem], family, stem)))
            else:
                # Isolated. A one-member family lands here too, and must: the
                # centre dais is a single piece the collaborator sank 130 uu so it
                # sits BELOW the floor (census Z -130..-5) and stops a 2,721 uu
                # slab rising through the middle of their kit. "1 of 1 moved
                # together" is not evidence of a builder change, it is arithmetic.
                plan_transform.append((label, d_loc, d_rot, old))

        # --- step 5 preview: bank vs kit conflicts ---------------------------
        hand_records = census_a["hand"]
        plan_bank_conflict = []
        for piece in fresh["pieces"]:
            if is_allowlisted(piece["label"]) != "BANK":
                continue
            if piece["label"] in {label for label, _, _ in plan_delete}:
                continue
            for hand in hand_records:
                if boxes_overlap(piece["bounds_origin"], piece["bounds_extent"],
                                 hand["bounds_origin"], hand["bounds_extent"]):
                    plan_bank_conflict.append((piece["label"], hand["label"]))
                    break

        # --- print the plan ---------------------------------------------------
        log("PLAN: {0} fresh pieces, {1} in the census; {2} matched by family+position, "
            "{3} fresh unclaimed, {4} census pieces with no counterpart".format(
                len(fresh["pieces"]), len(census_a["pieces"]), len(pairs),
                len(unmatched_new), len(unmatched_old)))
        far_pairs = sorted((d, o["label"], n["label"]) for o, n, d in pairs if d > 0.5)
        for distance, old_label, new_label in far_pairs[-12:]:
            log("PLAN match    census {0:<22} -> fresh {1:<22} {2:.0f} uu".format(
                old_label, new_label, distance))
        for label, rule, note in plan_delete:
            log("PLAN delete   {0:<28} [{1}] {2}".format(label, rule, note))
        for label, rule, note in plan_keep:
            log("PLAN keep     {0:<28} [{1}] {2}".format(label, rule, note))
        for label, d_loc, d_rot, _ in plan_transform:
            log("PLAN transform {0:<28} d_loc={1:.2f}uu d_rot={2:.3f}deg".format(label, d_loc, d_rot))
        for label, d_loc, d_rot, why in plan_transform_skipped:
            log("PLAN keep-fresh{0:<28} d_loc={1:.2f}uu d_rot={2:.3f}deg [{3}]".format(
                label, d_loc, d_rot, why))
        for label, hand_label in plan_bank_conflict:
            log("PLAN bank-conflict {0} overlaps hand actor {1} -> the kit wins".format(label, hand_label))
        for piece, donor, mirror, distance in plan_recreate:
            log("PLAN re-create {0:<24} hand duplicate (scale {1}) -> clone of fresh {2} "
                "under {3}, {4:.1f} uu off; target loc {5}".format(
                    piece["label"], [round(value, 3) for value in piece["scale"]],
                    donor["label"], mirror, distance,
                    [round(v, 1) for v in recreate_location(piece, donor)]))
        for label in vanished:
            log("PLAN vanished {0:<28} (the builder no longer emits this label)".format(label))
        log("PLAN hand actors to restore: {0}".format(len(hand_records)))

        reconciliation = {
            "pieces_emitted": len(fresh["pieces"]),
            "pieces_in_census": len(census_a["pieces"]),
            "pieces_matched": len(pairs),
            "pieces_matched_exactly": len([1 for _, _, d in pairs if d <= 0.5]),
            "fresh_pieces_unclaimed": len(unmatched_new),
            "census_pieces_unmatched": len(unmatched_old),
            "deletions_replayed": len(plan_delete),
            "kept_new_or_allowlisted": len(plan_keep),
            "kept_by_allowlist": len([1 for _, rule, _ in plan_keep if rule.startswith("allowlist")]),
            "kept_as_new_family": len([1 for _, rule, _ in plan_keep if rule == "new-family"]),
            "kept_as_family_densified": len([1 for _, rule, _ in plan_keep if rule == "family-densified"]),
            "transform_mode": TRANSFORM_MODE,
            "hand_duplicates_to_recreate": len(plan_recreate),
            "recreate_z_source": RECREATE_Z_SOURCE,
            "plan_recreate": [{"label": piece["label"], "donor": donor["label"],
                               "mirror": mirror, "distance_uu": distance,
                               "census_location": piece["location"],
                               "census_scale": piece["scale"],
                               "target_location": recreate_location(piece, donor),
                               "target_scale": donor["scale"]}
                              for piece, donor, mirror, distance in plan_recreate],
            "transforms_replayed": len(plan_transform),
            "transforms_left_at_builder_value": len(plan_transform_skipped),
            "plan_transform_skipped": [{"label": l, "d_loc_uu": d, "d_rot_deg": r, "why": w}
                                       for l, d, r, w in plan_transform_skipped],
            "hand_actors_in_census": len(hand_records),
            "vanished_labels": vanished,
            "bank_pieces_restored": len([1 for p in fresh["pieces"]
                                         if is_allowlisted(p["label"]) == "BANK"]),
            "bank_pieces_skipped_for_kit": len(plan_bank_conflict),
            "plan_delete": [{"label": l, "rule": r, "note": n} for l, r, n in plan_delete],
            "plan_keep": [{"label": l, "rule": r, "note": n} for l, r, n in plan_keep],
            "plan_transform": [{"label": l, "d_loc_uu": d, "d_rot_deg": r} for l, d, r, _ in plan_transform],
            "plan_bank_conflict": [{"bank": b, "hand": h} for b, h in plan_bank_conflict],
            "dry_run": DRY_RUN,
        }

        if DRY_RUN:
            census["census_b"] = fresh
            census["reconciliation"] = reconciliation
            with open(CENSUS_PATH, "w") as handle:
                json.dump(census, handle, indent=1, sort_keys=True)
            log("DRY RUN: plan written to {0}; the level was not modified and not saved.".format(CENSUS_PATH))
            log("PHASE restore OK (dry run)")
            return

        # --- execute ----------------------------------------------------------
        deleted = 0
        for label, _, _ in plan_delete:
            actor = live_pieces.get(label)
            if actor is None:
                log_error("delete: {0} is not in the level".format(label))
                continue
            if subsystem.destroy_actor(actor):
                deleted += 1
            else:
                log_error("delete: destroy_actor refused {0}".format(label))

        moved = 0
        for label, _, _, old in plan_transform:
            actor = live_pieces.get(label)
            if actor is None:
                log_error("transform: {0} is not in the level".format(label))
                continue
            actor.set_actor_location_and_rotation(
                unreal.Vector(*old["location"]),
                unreal.Rotator(old["rotation"][2], old["rotation"][0], old["rotation"][1]),
                False, True)
            actor.set_actor_scale3d(unreal.Vector(*old["scale"]))
            moved += 1

        recreated = 0
        recreate_failures = []
        for piece, donor, _mirror, _distance in plan_recreate:
            source = live_pieces.get(donor["label"])
            if source is None:
                log_error("re-create: donor {0} is not in the level".format(donor["label"]))
                recreate_failures.append(piece["label"])
                continue
            # The DONOR's scale, not the census's, and X/Y from the census with Z
            # from the donor - see recreate_location and the plan_recreate note.
            copy = clone_baked_piece(source,
                                     recreate_location(piece, donor),
                                     piece["rotation"],
                                     donor["scale"],
                                     piece["label"],
                                     piece.get("folder", ""))
            if copy is None:
                log_error("re-create: could not clone {0} for {1}".format(
                    donor["label"], piece["label"]))
                recreate_failures.append(piece["label"])
                continue
            recreated += 1
        if recreated:
            log("Re-created {0} hand-duplicated baked pieces".format(recreated))
        for label in recreate_failures:
            log("LOST hand-duplicated piece {0} - the clone did not come back".format(label))

        restored = []
        for record in hand_records:
            actor = spawn_hand_actor(record)
            restored.append((record, actor))
        log("Restored {0} hand actors".format(len(restored)))

        # --- step 5 conflict rule: the kit always wins ------------------------
        # Re-derived from the LIVE restored actors rather than from the plan, so
        # a hand actor that came back at a corrected transform is judged where it
        # actually is.
        restored_bounds = []
        for record, actor in restored:
            origin, extent = bounds_of(actor)
            restored_bounds.append((record["label"], origin, extent))

        banks_removed = 0
        banks_conflicting = []
        for actor in list(subsystem.get_all_level_actors()):
            if not actor or not isinstance(actor, unreal.TraceBakedPiece):
                continue
            label = actor.get_actor_label()
            if is_allowlisted(label) != "BANK":
                continue
            origin, extent = bounds_of(actor)
            for hand_label, hand_origin, hand_extent in restored_bounds:
                if boxes_overlap(origin, extent, hand_origin, hand_extent):
                    banks_conflicting.append((label, hand_label))
                    if BANK_CONFLICT_MODE == "delete":
                        log("Bank conflict: {0} overlaps restored hand actor {1} - deleting the bank "
                            "piece (MAP §10.2.5: the centre layout is absolute)".format(label, hand_label))
                        if subsystem.destroy_actor(actor):
                            banks_removed += 1
                    else:
                        log("Bank conflict: {0} overlaps restored hand actor {1} - REPORTED ONLY "
                            "(TRACE_REBAKE_BANK_CONFLICT={2}); the bank stays in the level".format(
                                label, hand_label, BANK_CONFLICT_MODE))
                    break

        if not level_subsystem().save_current_level():
            raise RuntimeError("save_current_level() failed")
        log("Saved {0}".format(MAP_PATH))

        after = census_world("CENSUS-C")

        # --- verify the restore, actor by actor -------------------------------
        after_hand = {r["label"]: r for r in after["hand"]}
        failures = []
        for record in hand_records:
            got = after_hand.get(record["label"])
            if got is None:
                failures.append("{0}: missing after restore".format(record["label"]))
                continue
            for axis in range(3):
                if abs(got["location"][axis] - record["location"][axis]) > 0.5:
                    failures.append("{0}: location axis {1} {2:.2f} != {3:.2f}".format(
                        record["label"], axis, got["location"][axis], record["location"][axis]))
                if abs(got["scale"][axis] - record["scale"][axis]) > 1e-3:
                    failures.append("{0}: scale axis {1} {2:.4f} != {3:.4f}".format(
                        record["label"], axis, got["scale"][axis], record["scale"][axis]))
            for axis in range(3):
                delta = abs(((got["rotation"][axis] - record["rotation"][axis]) + 180.0) % 360.0 - 180.0)
                if delta > 0.1:
                    failures.append("{0}: rotation axis {1} off by {2:.3f} deg".format(
                        record["label"], axis, delta))
            if set(record["tags"]) - set(got["tags"]):
                failures.append("{0}: tags lost {1}".format(
                    record["label"], sorted(set(record["tags"]) - set(got["tags"]))))
            # The census mesh path is checked THROUGH the §10.4 rename table, or
            # every kit actor would report a failure for having come back wearing
            # the asset's new name - which is the whole point of the rename.
            want = renamed_mesh_path((record.get("mesh_component") or {}).get("mesh"), record["label"])
            have = (got.get("mesh_component") or {}).get("mesh")
            if want and want != have:
                failures.append("{0}: mesh {1} != {2}".format(record["label"], have, want))
            want_mats = (record.get("mesh_component") or {}).get("materials")
            have_mats = (got.get("mesh_component") or {}).get("materials")
            if want_mats and want_mats != have_mats:
                failures.append("{0}: materials {1} != {2}".format(record["label"], have_mats, want_mats))

        # --- verify the re-created hand duplicates, piece by piece ------------
        #
        # Same discipline as the hand layer above, and it needs its own loop
        # because these are PIECES, not hand actors, and because the transform
        # they are asserted against is the computed one (census X/Y, donor Z)
        # rather than the census row - see recreate_location. Reported by
        # LABEL, LOCATION and COMPONENT COUNT: a clone that landed in the right
        # place with no components would otherwise pass every count in the
        # reconciliation and be invisible until somebody looked at the tower.
        after_pieces = {p["label"]: p for p in after["pieces"]}
        # Re-read the level rather than reusing live_pieces: that map was built
        # before the clones existed and does not contain them.
        saved_pieces = {}
        for actor in subsystem.get_all_level_actors():
            if actor and isinstance(actor, unreal.TraceBakedPiece):
                saved_pieces[actor.get_actor_label()] = actor
        recreate_failures_verified = []
        for piece, donor, _mirror, _distance in plan_recreate:
            got = after_pieces.get(piece["label"])
            if got is None:
                recreate_failures_verified.append("{0}: missing after restore".format(piece["label"]))
                continue
            want_loc = recreate_location(piece, donor)
            for axis in range(3):
                if abs(got["location"][axis] - want_loc[axis]) > 0.5:
                    recreate_failures_verified.append(
                        "{0}: location axis {1} {2:.2f} != {3:.2f}".format(
                            piece["label"], axis, got["location"][axis], want_loc[axis]))
            live = saved_pieces.get(piece["label"])
            source = saved_pieces.get(donor["label"])
            if live is not None and source is not None:
                mine = len(live.get_components_by_class(unreal.SceneComponent))
                theirs = len(source.get_components_by_class(unreal.SceneComponent))
                if mine != theirs:
                    recreate_failures_verified.append(
                        "{0}: {1} components, donor {2} has {3}".format(
                            piece["label"], mine, donor["label"], theirs))
            if got.get("materials") != donor.get("materials"):
                recreate_failures_verified.append("{0}: materials {1} != donor's {2}".format(
                    piece["label"], got.get("materials"), donor.get("materials")))
        failures.extend(recreate_failures_verified)

        reconciliation["hand_duplicates_recreated"] = recreated
        reconciliation["hand_actors_restored"] = len(restored)
        reconciliation["deletions_executed"] = deleted
        reconciliation["transforms_executed"] = moved
        reconciliation["bank_conflict_mode"] = BANK_CONFLICT_MODE
        reconciliation["bank_pieces_conflicting"] = [{"bank": b, "hand": h} for b, h in banks_conflicting]
        reconciliation["bank_pieces_deleted_for_kit"] = banks_removed
        reconciliation["restore_failures"] = failures
        reconciliation["final_actor_count"] = after["actor_count"]
        reconciliation["final_piece_count"] = len(after["pieces"])
        reconciliation["final_hand_count"] = len(after["hand"])
        reconciliation["final_side_tint_total"] = after["side_tint_total"]

        # --- the reconciliation, MAP §10.2.6 ----------------------------------
        expected_pieces = (len(fresh["pieces"]) + recreated - deleted - banks_removed)
        balanced = (expected_pieces == len(after["pieces"])
                    and len(restored) == len(hand_records)
                    and recreated == len(plan_recreate)
                    and deleted == len(plan_delete)
                    and moved == len(plan_transform)
                    and not failures)
        reconciliation["expected_final_piece_count"] = expected_pieces
        reconciliation["balanced"] = balanced

        log("=" * 78)
        log("RECONCILIATION ({0})".format(MAP_PATH))
        log("  pieces in the census (A)          {0}".format(len(census_a["pieces"])))
        log("  pieces the fresh bake emitted (B) {0}".format(len(fresh["pieces"])))
        log("  deletions replayed                {0} (planned {1})".format(deleted, len(plan_delete)))
        log("  kept: allowlisted new families    {0}".format(reconciliation["kept_by_allowlist"]))
        log("  kept: stem absent from census     {0}".format(reconciliation["kept_as_new_family"]))
        log("  kept: family densified in place   {0}".format(reconciliation["kept_as_family_densified"]))
        log("  transform edits replayed          {0} (planned {1}, mode={2})".format(
            moved, len(plan_transform), TRANSFORM_MODE))
        log("  transforms left at builder value  {0}".format(len(plan_transform_skipped)))
        log("  hand actors restored              {0} of {1}".format(len(restored), len(hand_records)))
        log("  bank pieces restored              {0}".format(reconciliation["bank_pieces_restored"]))
        log("  bank pieces overlapping a hand actor {0} (mode={1})".format(
            len(banks_conflicting), BANK_CONFLICT_MODE))
        log("  bank pieces deleted for the kit   {0}".format(banks_removed))
        log("  pieces matched (family+position)  {0}, of which {1} at distance 0".format(
            len(pairs), reconciliation["pieces_matched_exactly"]))
        log("  hand-duplicated pieces re-created {0} of {1} (Z from the {2})".format(
            recreated, len(plan_recreate), RECREATE_Z_SOURCE))
        log("  labels the builder retired        {0}".format(len(vanished)))
        log("  final: {0} actors = {1} pieces + {2} hand + {3} other".format(
            after["actor_count"], len(after["pieces"]), len(after["hand"]),
            after["actor_count"] - len(after["pieces"]) - len(after["hand"])))
        log("  expected final piece count        {0}  (B {1} + {2} re-created - {3} deleted "
            "- {4} bank-conflict)".format(expected_pieces, len(fresh["pieces"]), recreated,
                                          deleted, banks_removed))
        log("  FTraceBakedSideTint entries       {0} (census had {1})".format(
            after["side_tint_total"], census_a["side_tint_total"]))
        log("  BALANCED: {0}".format("YES" if balanced else "NO"))
        for failure in failures:
            log_error("  restore check: {0}".format(failure))
        log("=" * 78)

        census["census_b"] = fresh
        census["census_c"] = after
        census["reconciliation"] = reconciliation
        with open(CENSUS_PATH, "w") as handle:
            json.dump(census, handle, indent=1, sort_keys=True)
        log("Wrote {0}".format(CENSUS_PATH))

        if not balanced:
            raise RuntimeError(
                "the reconciliation does not balance - see the RECONCILIATION block above. "
                "The map on disk is the re-baked one; restore the filesystem backup of "
                "Content/Maps/Arena_Baked.umap and Content/__ExternalActors__/Maps/Arena_Baked/ "
                "to go back.")
        log("PHASE restore OK")

    # -------------------------------------------------------------------------
    # PHASE probe - does clone_baked_piece work in THIS engine, on THIS map?
    # -------------------------------------------------------------------------

    def phase_probe():
        """Clone one piece, check it, delete it, and DO NOT SAVE.

        This exists because of what it replaces. The route this script used to
        take crashed the commandlet outright, and it did so in the middle of the
        restore - after the force bake had already deleted the level. Proving the
        replacement on a throwaway actor first costs two minutes and means the
        restore is never the place a new engine call is tried for the first time.
        Nothing is written: the level is opened, one actor is spawned and
        destroyed, and the phase returns without saving.
        """
        open_map()
        subsystem = actor_subsystem()
        donor = None
        for actor in subsystem.get_all_level_actors():
            if actor and isinstance(actor, unreal.TraceBakedPiece) \
                    and actor.get_actor_label() == PROBE_DONOR_LABEL:
                donor = actor
                break
        if donor is None:
            raise RuntimeError("probe: no piece labelled '{0}' in {1}".format(
                PROBE_DONOR_LABEL, MAP_PATH))

        wanted = max(0, len(donor.get_components_by_class(unreal.SceneComponent)) - 1)
        origin, extent = bounds_of(donor)
        log("probe donor {0}: {1} components, bounds origin {2} extent {3}".format(
            donor.get_actor_label(), wanted,
            [round(v, 1) for v in origin], [round(v, 1) for v in extent]))

        target = [origin[0], origin[1], donor.get_actor_location().z + 4000.0]
        copy = clone_baked_piece(donor, target, [0.0, 0.0, 0.0], [1.0, 1.0, 1.0],
                                 "ProbeClone_DELETE_ME", "")
        if copy is None:
            raise RuntimeError("probe: clone_baked_piece returned nothing - re-creation would "
                               "not work in this engine, so leave TRACE_REBAKE_RECREATE off")

        copy_origin, copy_extent = bounds_of(copy)
        log("probe clone: {0} components, bounds origin {1} extent {2}".format(
            len(copy.get_components_by_class(unreal.SceneComponent)) - 1,
            [round(v, 1) for v in copy_origin], [round(v, 1) for v in copy_extent]))
        for axis in range(3):
            if abs(copy_extent[axis] - extent[axis]) > 1.0:
                raise RuntimeError("probe: clone bounds extent {0} != donor's {1}".format(
                    [round(v, 1) for v in copy_extent], [round(v, 1) for v in extent]))
        log("probe: the clone has the donor's component count AND the donor's bounds")

        # Said out loud, because this is the check that was missing the first time
        # and a passing clone_baked_piece is the only place it is enforced.
        for source in donor.get_components_by_class(unreal.PrimitiveComponent):
            if collision_enabled_of(source) == "NO_COLLISION":
                continue
            table = collision_response_table(source)
            blocking = sorted(k for k, v in table.items() if v.endswith("ECR_BLOCK"))
            log("probe: donor's {0} ({1}) blocks {2} of {3} channels".format(
                source.get_name(), collision_enabled_of(source), len(blocking), len(table)))
        log("probe: every colliding component's response table matched, channel for channel "
            "(clone_baked_piece discards the clone otherwise)")

        subsystem.destroy_actor(copy)
        log("probe: clone destroyed; NOTHING WAS SAVED")
        log("PHASE probe OK")

    def main_editor():
        if PHASE == "census":
            phase_census()
        elif PHASE == "restore":
            phase_restore()
        elif PHASE == "probe":
            phase_probe()
        else:
            raise RuntimeError(
                "set TRACE_REBAKE_PHASE to 'census', 'restore' or 'probe' (got '{0}'). Normally you "
                "do not run this inside the editor by hand - run "
                "`python3 Scripts/rebake-arena-preserving.py` and let it drive.".format(PHASE))

    try:
        main_editor()
    except Exception as exc:  # noqa: BLE001 - a commandlet needs the reason in the log
        log_error("rebake-arena-preserving.py failed: {0}".format(exc))
        raise


# =============================================================================
# ORCHESTRATOR SIDE (plain python3, outside the editor)
# =============================================================================

else:

    import subprocess
    import time

    ENGINE_ROOT = os.environ.get("TRACE_ENGINE_ROOT", "/Users/Shared/Epic Games/UE_5.8")
    UPROJECT = os.path.join(PROJECT_ROOT, "Trace.uproject")
    LOG_DIR = os.path.join(PROJECT_ROOT, "Saved", "Logs", "release")

    # WHICH TRANCHE'S LOGS THESE ARE. The names used to be hard-coded "W3-REBAKE-*",
    # which meant the second tranche to run this overwrote (or, via UE's -abslog
    # rotation, renamed) the evidence the first one's report cites by path. Every
    # run names its own; the default keeps the original names so an existing
    # instruction to "re-run it the W3 way" still produces the W3 filenames.
    LOG_PREFIX = os.environ.get("TRACE_REBAKE_LOG_PREFIX", "W3-REBAKE")

    def editor_binary():
        candidates = [
            os.path.join(ENGINE_ROOT, "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"),
            os.path.join(ENGINE_ROOT, "Engine/Binaries/Mac/UnrealEditor"),
        ]
        for path in candidates:
            if os.path.isfile(path):
                return path
        raise RuntimeError("no UnrealEditor under {0}".format(ENGINE_ROOT))

    def wait_for_stable_dylib(seconds=45, timeout=1800):
        """A sibling tranche relinking libUnrealEditor-Trace.dylib DELETES it, and
        an editor launched into that window dies with 'Incompatible or missing
        module: Trace' (W2-KIT measured it). Refuse to start until the dylib has
        been untouched for a while."""
        dylib = os.path.join(PROJECT_ROOT, "Binaries", "Mac", "libUnrealEditor-Trace.dylib")
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.isfile(dylib) and (time.time() - os.path.getmtime(dylib)) >= seconds:
                log("dylib stable for {0:.0f}s: {1}".format(time.time() - os.path.getmtime(dylib), dylib))
                return
            log("waiting for {0} to be stable for {1}s...".format(os.path.basename(dylib), seconds))
            time.sleep(15)
        raise RuntimeError("libUnrealEditor-Trace.dylib never settled")

    def run_editor_phase(phase, log_name, dry_run=False, timeout=2400):
        if not os.path.isdir(LOG_DIR):
            os.makedirs(LOG_DIR)
        log_path = os.path.join(LOG_DIR, log_name)
        env = dict(os.environ)
        env["TRACE_REBAKE_PHASE"] = phase
        env["TRACE_REBAKE_MAP"] = MAP_PATH
        env["TRACE_REBAKE_CENSUS"] = CENSUS_PATH
        env["TRACE_REBAKE_DRYRUN"] = "1" if dry_run else "0"
        args = [editor_binary(), UPROJECT,
                "-run=pythonscript", "-script={0}".format(os.path.abspath(__file__)),
                "-unattended", "-nosplash", "-nopause", "-stdout", "-FullStdOutLogOutput",
                "-abslog={0}".format(log_path)]
        log("phase '{0}' -> {1}".format(phase, log_path))
        # The commandlet's exit code is NOT the result: UnrealEditor -run=pythonscript
        # returns non-zero if ANY error was logged during the session, including
        # engine ensures raised at startup by unrelated modules (bake-arena.sh:220).
        # The authoritative check is the PHASE ... OK line the phase prints.
        status = subprocess.call(args, env=env, timeout=timeout)
        marker = "PHASE {0} OK".format(phase)
        ok = False
        if os.path.isfile(log_path):
            with open(log_path, "r", errors="replace") as handle:
                ok = marker in handle.read()
        log("phase '{0}': editor exit {1}, marker {2}".format(
            phase, status, "FOUND" if ok else "MISSING"))
        if not ok:
            raise RuntimeError("phase '{0}' did not reach '{1}' - read {2}".format(
                phase, marker, log_path))
        return log_path

    def run_force_bake(timeout=3600):
        """The STANDARD force bake, unmodified: it is the proven path (it clears
        the LFS read-only bit, removes the external-actor packages and the .umap
        from the filesystem in that order, then bakes and checks what landed).
        Deliberately NOT --nullrhi: this bake re-creates every committed material
        instance under Instances/, and a material saved without a real RHI has no
        shader map."""
        if not os.path.isdir(LOG_DIR):
            os.makedirs(LOG_DIR)
        log_path = os.path.join(LOG_DIR, "{0}-bake.log".format(LOG_PREFIX))
        script = os.path.join(SCRIPT_DIR, "bake-arena.sh")
        args = [script, "--force", "--",
                "-RenderOffScreen", "-abslog={0}".format(log_path)]
        log("force bake: {0} (log {1})".format(" ".join(args), log_path))
        status = subprocess.call(args, cwd=PROJECT_ROOT, timeout=timeout)
        if status != 0:
            raise RuntimeError("bake-arena.sh --force exited {0} - read {1}".format(status, log_path))
        log("force bake finished")

    def main_shell():
        census_only = "--census" in sys.argv
        dry_run = "--dry-run" in sys.argv
        restore_only = "--restore-only" in sys.argv
        # --no-census: bake + restore against the census JSON already on disk.
        # The census must describe the map as a HUMAN last left it, so a second
        # pass at the pipeline has to re-use the first pass's census rather than
        # censusing its own output - otherwise the audit trail silently rebases
        # onto a machine-made map and every "hand edit" it records is one this
        # script itself made.
        no_census = "--no-census" in sys.argv

        if not restore_only:
            if not no_census:
                wait_for_stable_dylib()
                run_editor_phase("census", "{0}-census.log".format(LOG_PREFIX))
                if census_only:
                    log("--census: stopping after the census. {0}".format(CENSUS_PATH))
                    return
            elif not os.path.isfile(CENSUS_PATH):
                raise RuntimeError("--no-census needs an existing {0}".format(CENSUS_PATH))
            run_force_bake()

        wait_for_stable_dylib()
        run_editor_phase("restore", "{0}-restore{1}.log".format(
            LOG_PREFIX, "-dry" if dry_run else ""), dry_run=dry_run)
        log("done. Reconciliation is in {0} and in the restore log.".format(CENSUS_PATH))

    if __name__ == "__main__":
        try:
            main_shell()
        except Exception as exc:  # noqa: BLE001
            log_error(str(exc))
            raise SystemExit(1)
