# =============================================================================
# Trace — retarget_body.py
#
# Stage 3 of the character pipeline (PIPELINE_DESIGN.md §7). Runs INSIDE the
# editor (UnrealEditor-Cmd -run=pythonscript). Makes ALL TEN generated bodies
# move, with ONE bake:
#
#   /Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed    (Epic's, untouched)
#        |
#        |   IK_Manny -> RTG_Manny_To_TraceBody -> IK_TraceBody
#        v
#   /Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body     AnimBlueprint on
#                                                            SK_TraceBody_Skeleton
#   ...plus every sequence and blend space it refers to, duplicated and
#      retargeted beside it.
#
# Driven by Scripts/import-characters.sh --stage retarget, which pre-wipes
# Shared/{Retarget,Anims}/*.uasset BEFORE the editor starts and greps this
# script's verdict line. Do not invoke it by hand.
#
# WHAT CHANGED FROM retarget_rocco.py (which this is a parameterisation of, and
# which stays on disk as the superseded single-character original)
#
#   ONE BAKE, TEN CHARACTERS. The ten generated bodies are skinned to a single
#   shared skeleton (PIPELINE §3.1) whose joints are identical by construction —
#   character_bodies.py forbids recipes from moving joints, and
#   import_characters.py asserts the imported bone table against
#   CANONICAL_SKELETON. A USkeleton stores one reference skeleton, so one bake
#   against it drives all ten, and every roster row can name the same
#   BodyAnimClassPath. `TARGET_MESH_PATH` is therefore any of the ten (Rocco,
#   the skeleton donor); it is only the preview/alignment mesh, not the subject.
#
#   MANNEQUIN BONE NAMES. Rocco's old FBX rig carried Mixamo names with a
#   trailing "1" (Hips1 / LeftHand1), which is why retarget_rocco.py needed a
#   hand-written chain table and why auto-characterisation could not work: the
#   engine's matcher strips a common PREFIX, and a suffix is not a prefix. The
#   generated bodies use Manny's own names, so this script ASKS the
#   auto-characteriser first and keeps its answer when it is complete
#   (PIPELINE §7, risk row 6). Hand-adding a chain over a template that already
#   produced one uniquifies the name to "Spine_0", the mapping-by-exact-name
#   then finds the wrong chain, and the bake silently comes out as a bind pose —
#   so the two arms are mutually exclusive, never mixed, and the read-back after
#   either arm is the same.
#
#   Everything else is the proven code path, kept: two retarget ops (PelvisMotion
#   + FKChains, not the engine's default five — RunIKRig has no goals to solve,
#   RootMotion would key a root that every sequence deliberately leaves in place,
#   CurveRemap has no curves), AutoAlignAllBones(TARGET, CHAIN_TO_CHAIN) as the
#   step that makes an A-pose rig comparable to a T-pose one, the batch with
#   include_referenced_assets, and the motion sampler.
#
#   THE OFFSETS SHOULD BE ~ZERO HERE, AND THAT IS THE POINT. CANONICAL_SKELETON
#   is a uniformly scaled Manny in the same A-pose (×176/180.54), so
#   AutoAlignAllBones has almost nothing to correct. Every chain's align offset
#   is printed; anything past ALIGN_WARN_DEG is called out, because a large
#   offset on this rig means the canonical table drifted, not that the aligner
#   worked hard.
#
# WHAT IS CHECKED RATHER THAN ASSUMED
#
#   The chain bones exist on the mesh before any setter runs; the chains land on
#   the rig with the names asked for; the op stack is the one asked for; every
#   target chain resolves to a source chain; the batch produces files; the
#   duplicated AnimBlueprint is bound to the SHARED skeleton and compiled to a
#   generated class; and — the check this whole script exists for — the
#   retargeted sequences MOVE. A retarget that quietly produced the bind pose on
#   every frame passes every other test in this file.
#
# VERDICT: grep for "[retarget-body] EXIT=0". The commandlet's exit code is the
# engine's error count, not this script's verdict (generate-data-assets.py:38-43).
# =============================================================================
import os
import unreal


TARGET_MESH_PATH = "/Game/Trace/Characters/Rocco/SK_Rocco"
TARGET_SKELETON_PATH = "/Game/Trace/Characters/Shared/SK_TraceBody_Skeleton"
MANNY_MESH_PATH = "/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"
SOURCE_ABP_PATH = "/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"

RIG_DIR = "/Game/Trace/Characters/Shared/Retarget"
ANIM_DIR = "/Game/Trace/Characters/Shared/Anims"

IK_MANNY_NAME = "IK_Manny"
IK_BODY_NAME = "IK_TraceBody"
RTG_NAME = "RTG_Manny_To_TraceBody"

IK_MANNY_PATH = "{0}/{1}".format(RIG_DIR, IK_MANNY_NAME)
IK_BODY_PATH = "{0}/{1}".format(RIG_DIR, IK_BODY_NAME)
RTG_PATH = "{0}/{1}".format(RIG_DIR, RTG_NAME)

# The suffix every duplicated animation asset gets. TraceCharacterRoster's ten
# rows spell the resulting ABP path out, so changing it here is a two-file
# change (PIPELINE §9.1).
NAME_SUFFIX = "_Body"
RESULT_ABP_PATH = "{0}/ABP_Unarmed{1}".format(ANIM_DIR, NAME_SUFFIX)

# PIPELINE §3.5. Chain names are Epic's FCharacterizationStandard constants
# (IKRigAutoCharacterizer.cpp) because the retargeter maps source to target BY
# CHAIN NAME and IK_Manny's chains come from that same table. The bone names are
# CANONICAL_SKELETON's — Manny's names, scaled rig, no suffix.
#
# The rig has no finger, twist or ik_* bones (PIPELINE §3.4 says why: rigid
# mitten hands need no fingers, rigid parts need no twist, and the retargeter
# maps neither), so these eleven chains are all of it that can be retargeted.
# Manny's finger chains find no counterpart and are left alone, which is
# correct — the hands stay in their bind pose, and these hands are mittens.
#
#   chain name        start bone    end bone
BODY_CHAINS = (
    ("Spine",         "spine_01",   "spine_05"),
    ("Neck",          "neck_01",    "neck_02"),
    ("Head",          "head",       "head"),
    ("LeftClavicle",  "clavicle_l", "clavicle_l"),
    ("RightClavicle", "clavicle_r", "clavicle_r"),
    ("LeftArm",       "upperarm_l", "hand_l"),
    ("RightArm",      "upperarm_r", "hand_r"),
    ("LeftLeg",       "thigh_l",    "foot_l"),
    ("RightLeg",      "thigh_r",    "foot_r"),
    ("LeftFoot",      "ball_l",     "ball_l"),
    ("RightFoot",     "ball_r",     "ball_r"),
)
BODY_PELVIS = "pelvis"

# The chains IK_Manny MUST end up with for the mapping above to find anything.
# Checked, not assumed: ApplyAutoGeneratedRetargetDefinition is engine code and
# a renamed constant would otherwise leave a retargeter that maps nothing.
REQUIRED_CHAINS = tuple(name for name, _s, _e in BODY_CHAINS)

# Full UStruct paths — AddRetargetOp resolves them with FindObject<UScriptStruct>.
OP_PELVIS = "/Script/IKRig.IKRetargetPelvisMotionOp"
OP_FK_CHAINS = "/Script/IKRig.IKRetargetFKChainsOp"

# Motion sampler thresholds (PIPELINE §7, retarget_rocco.py:383-460 verbatim).
FROZEN_DEGREES = 0.05
WALK_DEGREES = 20.0
SAMPLED_BONES = ("thigh_l", "thigh_r", "upperarm_l", "spine_01")

# A scaled Manny in the same A-pose should need almost no alignment; anything
# past this is worth a human reading the canonical table again (§3.5).
ALIGN_WARN_DEG = 10.0

EAL = unreal.EditorAssetLibrary

_failures = []
_warnings = []


def log(msg):
    unreal.log("[Trace] {0}".format(msg))


def fail(msg):
    _failures.append(msg)
    unreal.log_error("[Trace] {0}".format(msg))


def warn(msg):
    _warnings.append(msg)
    unreal.log_warning("[Trace] {0}".format(msg))


def load(path, what):
    asset = EAL.load_asset(path)
    if asset is None:
        fail("{0} is missing: {1}".format(what, path))
    return asset


def create_asset(name, folder, cls, factory):
    """Create, replacing whatever is there. The package files are deleted by the
    shell BEFORE the editor starts (import-characters.sh stage_retarget) for the
    reason Scripts/import-rocco.sh spells out: delete_asset frees the file but
    leaves the Asset Registry entry behind for the rest of the session, so a
    same-run create-over-delete silently hands back the OLD object."""
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    asset = tools.create_asset(name, folder, cls, factory)
    if asset is None:
        fail("could not create {0}/{1}".format(folder, name))
    return asset


def chain_table(controller):
    """name -> (start bone, end bone), read off the asset."""
    out = {}
    for chain in controller.get_retarget_chains():
        out[str(chain.get_editor_property("chain_name"))] = (
            str(chain.get_editor_property("start_bone").get_editor_property("bone_name")),
            str(chain.get_editor_property("end_bone").get_editor_property("bone_name")))
    return out


# -----------------------------------------------------------------------------
# IK_Manny — the source rig.
# -----------------------------------------------------------------------------
def build_manny_rig(manny_mesh):
    rig = create_asset(IK_MANNY_NAME, RIG_DIR, unreal.IKRigDefinition,
                       unreal.IKRigDefinitionFactory())
    if rig is None:
        return None

    controller = unreal.IKRigController.get_controller(rig)
    if not controller.set_skeletal_mesh(manny_mesh):
        fail("IK_Manny rejected SKM_Manny_Simple")
        return None

    # Engine code, and it knows this skeleton by name: SKM_Manny_Simple matches
    # the "UE5 Mannequin" template exactly, so the chains, the pelvis and the
    # root bone all come from Epic's own characterisation rather than from a
    # table in this file that could drift from it.
    matched = controller.apply_auto_generated_retarget_definition()
    log("  IK_Manny       auto-characterise -> {0}".format(
        "MATCHED" if matched else "no template matched"))
    if not matched:
        fail("the auto-characteriser did not recognise SKM_Manny_Simple; IK_Manny has no chains")
        return None

    got = chain_table(controller)
    missing = [c for c in REQUIRED_CHAINS if c not in got]
    if missing:
        fail("IK_Manny is missing chain(s) this retarget maps: {0}".format(", ".join(missing)))
    log("  IK_Manny       {0} chain(s), pelvis {1!r}, root-motion bone {2!r}"
        .format(len(got), str(controller.get_retarget_root()),
                str(controller.get_root_motion_bone())))
    return rig


# -----------------------------------------------------------------------------
# IK_TraceBody — the target rig. Auto-characterised if the engine will do it,
# hand-built from BODY_CHAINS if it will not. Never both (see the header).
# -----------------------------------------------------------------------------
def build_body_rig(body_mesh):
    rig = create_asset(IK_BODY_NAME, RIG_DIR, unreal.IKRigDefinition,
                       unreal.IKRigDefinitionFactory())
    if rig is None:
        return None

    controller = unreal.IKRigController.get_controller(rig)
    if not controller.set_skeletal_mesh(body_mesh):
        fail("IK_TraceBody rejected {0}".format(TARGET_MESH_PATH))
        return None

    matched = controller.apply_auto_generated_retarget_definition()
    landed = chain_table(controller)
    complete = matched and all(c in landed for c in REQUIRED_CHAINS)
    log("  IK_TraceBody   auto-characterise -> {0} ({1} chain(s): {2})".format(
        "MATCHED" if matched else "no template matched",
        len(landed), ", ".join(sorted(landed)) or "none"))

    if complete:
        # KEEP the engine's own answer. Adding BODY_CHAINS on top would produce
        # "Spine_0" and the exact-name mapping would then bind the wrong chain.
        log("  IK_TraceBody   keeping the matched template: all {0} required chain(s) "
            "present, so the hand table is not used (PIPELINE §7 arm 1)"
            .format(len(REQUIRED_CHAINS)))
        for name, start, end in BODY_CHAINS:
            got = landed[name]
            if got != (start, end):
                log("  IK_TraceBody   note: template's {0!r} is {1} -> {2}; the hand table "
                    "would have said {3} -> {4}".format(name, got[0], got[1], start, end))
    else:
        if landed:
            # A partial template match is the one state neither arm can handle:
            # hand-adding over it uniquifies, keeping it leaves chains missing.
            fail("IK_TraceBody auto-characterise produced a PARTIAL definition ({0}), which "
                 "is neither arm of PIPELINE §7: adding the hand table over it would "
                 "uniquify names to Spine_0 and the exact-name mapping would bind the wrong "
                 "chain. Missing: {1}".format(sorted(landed),
                                              [c for c in REQUIRED_CHAINS if c not in landed]))
            return None
        log("  IK_TraceBody   building the hand table (PIPELINE §7 arm 2)")
        for name, start, end in BODY_CHAINS:
            given = str(controller.add_retarget_chain(name, start, end, ""))
            if given != name:
                fail("IK_TraceBody chain {0!r} came back named {1!r}".format(name, given))

    if not controller.set_retarget_root(BODY_PELVIS):
        fail("IK_TraceBody rejected pelvis bone {0!r}".format(BODY_PELVIS))

    # Read back off the asset rather than trusting the setters.
    by_name = chain_table(controller)
    for name, _s, _e in BODY_CHAINS:
        if name not in by_name:
            fail("IK_TraceBody chain {0!r} did not land".format(name))
    log("  IK_TraceBody   {0} chain(s), pelvis {1!r}: {2}".format(
        len(by_name), str(controller.get_retarget_root()),
        ", ".join("{0}({1}->{2})".format(n, by_name[n][0], by_name[n][1])
                  for n, _s, _e in BODY_CHAINS if n in by_name)))
    return rig


# -----------------------------------------------------------------------------
# The retargeter.
# -----------------------------------------------------------------------------
def build_retargeter(manny_rig, body_rig, manny_mesh, body_mesh):
    rtg = create_asset(RTG_NAME, RIG_DIR, unreal.IKRetargeter, unreal.IKRetargetFactory())
    if rtg is None:
        return None

    controller = unreal.IKRetargeterController.get_controller(rtg)
    controller.set_ik_rig(unreal.RetargetSourceOrTarget.SOURCE, manny_rig)
    controller.set_ik_rig(unreal.RetargetSourceOrTarget.TARGET, body_rig)
    controller.set_preview_mesh(unreal.RetargetSourceOrTarget.SOURCE, manny_mesh)
    controller.set_preview_mesh(unreal.RetargetSourceOrTarget.TARGET, body_mesh)

    # Two ops, added AFTER the rigs so each one's OnAddedToStack sees them. The
    # header says why these two and not the engine's default five.
    for op_type in (OP_PELVIS, OP_FK_CHAINS):
        if controller.add_retarget_op(op_type) < 0:
            fail("retargeter rejected op type {0}".format(op_type))
    controller.assign_ik_rig_to_all_ops(unreal.RetargetSourceOrTarget.SOURCE, manny_rig)
    controller.assign_ik_rig_to_all_ops(unreal.RetargetSourceOrTarget.TARGET, body_rig)

    # EXACT, not FUZZY. Both rigs' chains are named for Epic's own constants
    # precisely so an exact match is possible; a fuzzy match would paper over a
    # typo in BODY_CHAINS instead of failing on it.
    controller.auto_map_chains(unreal.AutoMapChainType.EXACT, True)

    unmapped = []
    for name, _s, _e in BODY_CHAINS:
        source = str(controller.get_source_chain(name))
        if source != name:
            unmapped.append("{0} -> {1!r}".format(name, source))
    if unmapped:
        fail("chain(s) did not map onto IK_Manny: {0}".format(", ".join(unmapped)))

    ops = [str(controller.get_op_name(i)) for i in range(controller.get_num_retarget_ops())]
    log("  RTG            op stack: {0}".format(", ".join(ops) if ops else "EMPTY"))
    if len(ops) != 2:
        fail("expected exactly 2 retarget ops, got {0}".format(len(ops)))

    # THE STEP THAT DECIDES WHETHER IT LOOKS RIGHT. Rotates each target bone
    # until its chains point the way Manny's do, so the two bind poses become
    # comparable. Without it an A-pose rig driven by a T-pose rig's animation
    # wears the difference for the whole match.
    controller.auto_align_all_bones(unreal.RetargetSourceOrTarget.TARGET,
                                    unreal.RetargetAutoAlignMethod.CHAIN_TO_CHAIN)

    # Printed per chain rather than counted, because "5 of 11" is not a fact
    # anyone can act on. On THIS rig they should all read ~0 (§3.5): the target
    # is a uniformly scaled Manny in the same A-pose, so a large offset means the
    # canonical joint table drifted, not that the aligner worked hard.
    offsets = []
    worst, worst_name = 0.0, ""
    for name, start, _end in BODY_CHAINS:
        offset = controller.get_rotation_offset_for_retarget_pose_bone(
            start, unreal.RetargetSourceOrTarget.TARGET)
        # As a rotator, not as an angle against a default-constructed FQuat:
        # unreal.Quat() comes back ZEROED rather than identity, and comparing
        # against it reports a flat 180 degrees for every bone that moved at all.
        rot = offset.rotator()
        biggest = max(abs(rot.pitch), abs(rot.yaw), abs(rot.roll))
        if biggest > worst:
            worst, worst_name = biggest, name
        offsets.append("{0} ({1:.1f},{2:.1f},{3:.1f})".format(
            name, rot.pitch, rot.yaw, rot.roll))
    log("  RTG            retarget pose {0!r}, per-chain align offset as (pitch,yaw,roll): {1}"
        .format(str(controller.get_current_retarget_pose_name(unreal.RetargetSourceOrTarget.TARGET)),
                ", ".join(offsets)))
    if worst > ALIGN_WARN_DEG:
        warn("align offset {0:.1f} deg on chain {1!r} exceeds {2:.0f} deg. CANONICAL_SKELETON "
             "is meant to be a uniformly scaled Manny in the same A-pose, so the aligner "
             "should have had nothing to do (PIPELINE §3.5) — check the joint table before "
             "trusting these poses.".format(worst, worst_name, ALIGN_WARN_DEG))
    else:
        log("  RTG            widest align offset {0:.2f} deg ({1}) — the near-identity the "
            "scaled-Manny table predicts".format(worst, worst_name or "none"))
    return rtg


# -----------------------------------------------------------------------------
# The batch.
# -----------------------------------------------------------------------------
def run_batch(rtg, manny_mesh, body_mesh):
    inputs = unreal.IKRetargetBatchOperationInputs()
    inputs.set_editor_property("assets_to_retarget",
                               [unreal.AssetRegistryHelpers.get_asset_registry()
                                .get_asset_by_object_path(SOURCE_ABP_PATH + ".ABP_Unarmed")])
    inputs.set_editor_property("source_mesh", manny_mesh)
    inputs.set_editor_property("target_mesh", body_mesh)
    inputs.set_editor_property("ik_retarget_asset", rtg)
    inputs.set_editor_property("suffix", NAME_SUFFIX)
    inputs.set_editor_property("target_path", ANIM_DIR)
    inputs.set_editor_property("use_source_path", False)
    # The blueprint is only the entry point: what has to be retargeted is every
    # sequence and blend space it plays.
    inputs.set_editor_property("include_referenced_assets", True)
    inputs.set_editor_property("overwrite_existing_files", True)

    created = unreal.IKRetargetBatchOperation.run_batch_retarget(inputs)
    names = sorted(str(a.get_editor_property("package_name")) for a in created)
    log("  batch          {0} asset(s) written".format(len(names)))
    for n in names:
        log("                   {0}".format(n))
    if not names:
        fail("the batch retarget produced nothing at all")
    return names


# -----------------------------------------------------------------------------
# The checks that matter.
# -----------------------------------------------------------------------------
def bone_names(mesh):
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(
        unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0),
        unreal.Rotator(pitch=0.0, yaw=0.0, roll=0.0))
    if actor is None:
        return []
    try:
        comp = actor.skeletal_mesh_component
        comp.set_skeletal_mesh_asset(mesh)
        return [str(comp.get_bone_name(i)) for i in range(comp.get_num_bones())]
    finally:
        subsystem.destroy_actor(actor)


def check_result(body_skeleton):
    """The blueprint is on the SHARED skeleton, and the animation actually moves."""
    abp = EAL.load_asset(RESULT_ABP_PATH)
    if abp is None:
        fail("the retargeted anim blueprint is not at {0}".format(RESULT_ABP_PATH))
        return

    target = abp.get_editor_property("target_skeleton")
    if target != body_skeleton:
        fail("{0} targets skeleton {1}, not {2}"
             .format(RESULT_ABP_PATH, target.get_path_name() if target else "None",
                     body_skeleton.get_path_name()))
    else:
        # This is the whole point of the shared-skeleton decision: ONE anim class
        # for all ten roster rows (PIPELINE §3.3).
        log("  ABP            ABP_Unarmed{0} targets {1} — the one skeleton all ten bodies "
            "share".format(NAME_SUFFIX, target.get_name()))

    # The class, not the blueprint, is what ATraceCharacter's BodyAnimClassPath
    # resolves, and a blueprint that failed to compile still loads perfectly well
    # as an asset. Ask for the generated class by the name the game will use.
    generated = EAL.load_blueprint_class(RESULT_ABP_PATH)
    if generated is None:
        fail("{0} has no generated class — it did not compile, so nothing can run it"
             .format(RESULT_ABP_PATH))
    else:
        log("  ABP            generated class {0}".format(generated.get_name()))

    # *** THE CHECK THIS WHOLE SCRIPT EXISTS FOR. *** Every step above can succeed
    # and still leave sequences that are the bind pose on every frame — which is
    # exactly the state the Rocco task was handed, and it is invisible to every
    # other test in this file. So the pose is SAMPLED, and two different things
    # are asked of it:
    #
    #   1. NOTHING IS FROZEN. A sequence whose sampled pose never changes at all
    #      is a bind pose wearing a duration. The threshold is deliberately near
    #      zero, because MM_Idle legitimately barely moves — an idle that breathes
    #      is 1 degree of spine, and demanding more of it would be a harness
    #      measuring the wrong thing.
    #   2. SOMETHING REALLY WALKS. Across the whole set, at least one sequence has
    #      to swing a leg like a locomotion cycle does. This is the arm that would
    #      catch a retarget that produced correct-but-tiny noise everywhere.
    checked = 0
    loudest = 0.0
    loudest_name = ""
    for path in sorted(EAL.list_assets(ANIM_DIR, recursive=True, include_folder=False)):
        anim = EAL.load_asset(path)
        if not isinstance(anim, unreal.AnimSequence):
            continue
        frames = unreal.AnimationLibrary.get_num_frames(anim)
        if frames < 2:
            continue
        moved = 0.0
        for bone in SAMPLED_BONES:
            samples = [unreal.AnimationLibrary.get_bone_pose_for_frame(anim, bone, f, False).rotation
                       for f in sorted(set((frames - 1) * i // 8 for i in range(9)))]
            for a in range(len(samples)):
                for b in range(a + 1, len(samples)):
                    moved = max(moved, abs(samples[a].angular_distance(samples[b])))
        checked += 1
        degrees = moved * 180.0 / 3.14159265358979
        if degrees > loudest:
            loudest, loudest_name = degrees, anim.get_name()
        log("  motion         {0:<44} {1:>3} frames, {2:6.2f} deg  {3}"
            .format(anim.get_name(), frames, degrees,
                    "moves" if degrees > FROZEN_DEGREES else "*** FROZEN ***"))
        if degrees <= FROZEN_DEGREES:
            fail("{0} is frozen: across 9 samples of {1} bones the pose never changes by more "
                 "than {2:.3f} degrees. That is a bind pose with a duration, which is exactly "
                 "what this retarget exists to replace."
                 .format(anim.get_name(), len(SAMPLED_BONES), degrees))
    if loudest < WALK_DEGREES:
        fail("nothing in {0} swings a limb: the widest rotation anywhere in {1} sequence(s) is "
             "{2:.2f} degrees ({3}), and a walk cycle is tens of degrees. The retarget ran but "
             "carried almost no animation.".format(ANIM_DIR, checked, loudest, loudest_name))
    else:
        log("  motion         WALK ok — widest swing {0:.1f} deg in {1}; FROZEN ok — all {2} "
            "sequence(s) move".format(loudest, loudest_name, checked))
    if checked == 0:
        fail("no retargeted AnimSequence was found under {0} to check for motion".format(ANIM_DIR))


# -----------------------------------------------------------------------------
def main():
    log("=" * 78)
    log("TRACE BODY RETARGET — one bake for all ten generated bodies")
    log("=" * 78)

    body_mesh = load(TARGET_MESH_PATH, "the alignment body mesh")
    body_skeleton = load(TARGET_SKELETON_PATH, "SK_TraceBody_Skeleton")
    manny_mesh = load(MANNY_MESH_PATH, "SKM_Manny_Simple")
    if EAL.load_asset(SOURCE_ABP_PATH) is None:
        fail("ABP_Unarmed is missing: {0}. Only the RETARGET stage needs the Mannequin — "
             "run Scripts/import-mannequin.sh (PIPELINE §11 row 13); the baked output is "
             "committed, so players and CI never need it.".format(SOURCE_ABP_PATH))
    if _failures:
        report()
        return

    # The chain table names bones. If the import changed a bone name, every
    # setter below quietly does nothing and the retarget comes out as a bind
    # pose — so the names are checked against the mesh FIRST.
    present = set(bone_names(body_mesh))
    wanted = set([BODY_PELVIS]) | set(SAMPLED_BONES)
    for _n, s, e in BODY_CHAINS:
        wanted.add(s)
        wanted.add(e)
    absent = sorted(b for b in wanted if b not in present)
    if absent:
        fail("{0} has no bone(s) named: {1}. BODY_CHAINS in this script is out of date with "
             "CANONICAL_SKELETON / the import.".format(TARGET_MESH_PATH, ", ".join(absent)))
        report()
        return
    log("  bones          all {0} bone(s) BODY_CHAINS and the motion sampler name are present "
        "on {1} ({2} bones total)".format(len(wanted), TARGET_MESH_PATH.rsplit("/", 1)[-1],
                                          len(present)))

    manny_rig = build_manny_rig(manny_mesh)
    body_rig = build_body_rig(body_mesh)
    if _failures:
        report()
        return

    rtg = build_retargeter(manny_rig, body_rig, manny_mesh, body_mesh)
    if _failures:
        report()
        return

    EAL.save_loaded_asset(manny_rig, False)
    EAL.save_loaded_asset(body_rig, False)
    EAL.save_loaded_asset(rtg, False)

    run_batch(rtg, manny_mesh, body_mesh)
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(False, True)
    check_result(body_skeleton)
    report()


def report():
    log("-" * 78)
    if _warnings:
        log("{0} WARNING(S):".format(len(_warnings)))
        for w in _warnings:
            log("  ! {0}".format(w))
    if _failures:
        log("{0} PROBLEM(S)".format(len(_failures)))
        for f in _failures:
            log("  - {0}".format(f))
    else:
        log("no problems reported")
    log("=" * 78)
    log("[retarget-body] EXIT={0}".format(1 if _failures else 0))


main()
