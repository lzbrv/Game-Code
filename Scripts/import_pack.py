# =============================================================================
# Trace — import_pack.py
#
# Runs INSIDE the editor (UnrealEditor-Cmd -run=pythonscript). Imports the
# artist's five-model pack from Art/Pack/models/ through Interchange, WITH the
# 31 baked animation clips, and stamps the pack's six materials as instances of
# the master the weapons already ship, M_TraceRailgun.
#
# Driven by Scripts/import-pack.sh — do not invoke this by hand.
#
# THE QUESTION THIS SCRIPT ANSWERS
#   The hands README says "32 joints, bring it in as a Skeletal Mesh", but every
#   GLB in the pack reports `skins: 0`: there is no skin binding, only a node
#   hierarchy with rigidly-parented meshes. Interchange has two switches for
#   exactly this shape — bConvertStaticsWithAnimatedTransformToSkeletals and
#   bConvertStaticsInBoneHierarchyToSkeletals, both on by default — which turn a
#   rigid hierarchy into a one-bone-per-node skeletal mesh so the clips can land
#   as UAnimSequences. This script tries the honest default first and reports
#   what it got; only if that yields no SkeletalMesh does it retry with
#   ForceAllMeshAsType = SkeletalMesh, and it says which path it took.
#
# WHY THE PACK IS IMPORTED ALONGSIDE, NOT OVER, THE SHIPPED ART
#   Content/Trace/Weapons/{Meshes,Materials} is committed, working art built by
#   Scripts/import-railgun.sh from Art/Railgun and Art/Smg. The pack's pistol is
#   a DIFFERENT model (the shipped one is a carbine with a stock and a foregrip;
#   the pack's is a pistol with real pivot nodes and two clips) and the pack's
#   SMG is the same geometry re-origined with two clips added. Overwriting
#   either in place would destroy working art to gain animation, so the pack
#   lands in its own tree, /Game/Trace/Art/Pack/**, and nothing under
#   /Game/Trace/Weapons is written or deleted by this script.
#
# WHY THE MATERIALS ARE STAMPED AND NOT IMPORTED
#   glTF material import would author a second master with its own parameter
#   names. The five glowing assets need the SAME scalar EmissiveIntensity knob
#   the pistol and SMG already use, so material import is switched OFF in the
#   pipeline and six instances of the shipped M_TraceRailgun are stamped from
#   the values read out of the GLBs. EmissiveIntensity is 1.0 at rest; the FX
#   notes drive it from there.
# =============================================================================
import json
import os
import unreal


PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL_DIR = os.path.join(PROJECT_ROOT, "Art", "Pack", "models")

PACK_ROOT = "/Game/Trace/Art/Pack"
PIPELINE_DIR = PACK_ROOT + "/_Pipelines"
PIPELINE_NAME = "TracePackGLTF"
MATERIAL_DIR = PACK_ROOT + "/Materials"

# The master the weapons already ship. NOT owned by this script: it is read,
# never written and never deleted.
MASTER_PATH = "/Game/Trace/Weapons/Materials/M_TraceRailgun"

DEFAULT_GLTF_ASSETS_PIPELINE = "/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline"
DEFAULT_GLTF_PIPELINE = "/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline"

FORCE = os.environ.get("TRACE_FORCE_PACK", "0") == "1"
ONLY = os.environ.get("TRACE_PACK_ONLY", "").strip()

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

_failures = []


def log(msg):
    unreal.log("[Trace] {0}".format(msg))


def fail(msg):
    _failures.append(msg)
    unreal.log_error("[Trace] {0}".format(msg))


# -----------------------------------------------------------------------------
# What we import, and what each thing is called afterwards.
#
# Interchange names assets off the source file and the clip names inside it, so
# every asset is renamed to a stable scheme here. Downstream code refers to
# these names, so they are the contract:
#
#   SK_Trace<Thing>          the skeletal mesh
#   SK_Trace<Thing>_Skeleton the skeleton Interchange creates alongside it
#   A_<Thing>_<Clip>         one AnimSequence per clip, in Anims/
# -----------------------------------------------------------------------------
#
# sample_rate is the rate the clips are BAKED at, and it must not be left to
# Interchange to choose. Its automatic choice here is 30 Hz, which silently
# re-times the short clips: Draw_Knife's authored 0.517 s came back as 0.500 s
# and Walljump's 0.850 s as 0.867 s. These clips are as short as 0.167 s and the
# hand and weapon pairs are authored to match frame for frame, so a 17 ms drift
# on one side of a pair is not acceptable. Every clip is authored at 60 fps
# except the SMG's 0.100 s Fire, which is authored at 120.
#
MODELS = [
    {
        "key": "hands",
        "glb": "gloved_hands.glb",
        "folder": PACK_ROOT + "/Hands",
        "mesh": "SK_TraceHands",
        "sample_rate": 60,
        "anim_prefix": "A_Hands_",
        "clips": ["Idle_Knife", "Idle_Pistol", "Idle_Smg", "Idle_Core",
                  "Draw_Knife", "Stab_Knife", "Inspect_Knife",
                  "Shoot_Pistol", "Reload_Pistol", "Shoot_Smg", "Reload_Smg",
                  "Throw_Core",
                  "Jump_Knife", "Jump_Pistol", "Jump_Smg", "Jump_Core",
                  "Walljump_Knife", "Walljump_Pistol", "Walljump_Smg", "Walljump_Core"],
    },
    {
        "key": "knife",
        "glb": "butterfly_knife.glb",
        "folder": PACK_ROOT + "/Knife",
        "mesh": "SK_TraceKnife",
        "sample_rate": 60,
        "anim_prefix": "A_Knife_",
        "clips": ["Idle_Open", "Draw", "Stab", "Inspect"],
    },
    {
        "key": "core",
        "glb": "core.glb",
        "folder": PACK_ROOT + "/Core",
        "mesh": "SK_TraceCore",
        "sample_rate": 60,
        "anim_prefix": "A_Core_",
        "clips": ["Idle", "Pickup", "Throw"],
    },
    {
        "key": "pistol",
        "glb": "railgun_pistol.glb",
        "folder": PACK_ROOT + "/Pistol",
        "mesh": "SK_TracePistolPack",
        "sample_rate": 60,
        "anim_prefix": "A_Pistol_",
        "clips": ["Fire", "Reload"],
    },
    {
        "key": "smg",
        "glb": "railgun_smg.glb",
        "folder": PACK_ROOT + "/Smg",
        "mesh": "SK_TraceSmgPack",
        "sample_rate": 120,
        "anim_prefix": "A_Smg_",
        "clips": ["Fire", "Reload"],
    },
]

# Read straight out of the GLBs (all five files declare identical values).
# base_color / emissive are linear; emissive_strength is glTF's KHR multiplier,
# folded into the colour so EmissiveIntensity stays a clean "1.0 = at rest".
MATERIALS = {
    "shell":        {"base": (0.040915197, 0.056128490, 0.078187422), "metallic": 0.28, "roughness": 0.52,
                     "emissive": (0.0, 0.0, 0.0), "strength": 1.0},
    "carbon":       {"base": (0.008568126, 0.010960094, 0.015208514), "metallic": 0.15, "roughness": 0.75,
                     "emissive": (0.0, 0.0, 0.0), "strength": 1.0},
    "seam":         {"base": (0.008568126, 0.010960094, 0.015208514), "metallic": 0.15, "roughness": 0.75,
                     "emissive": (0.0, 0.0, 0.0), "strength": 1.0},
    "plating":      {"base": (0.254152094, 0.309468923, 0.391572478), "metallic": 0.40, "roughness": 0.35,
                     "emissive": (0.0, 0.0, 0.0), "strength": 1.0},
    "circuit_cyan": {"base": (0.003035270, 1.0, 1.0), "metallic": 0.0, "roughness": 0.30,
                     "emissive": (0.018500220, 0.791297940, 1.0), "strength": 1.5},
    "core_amber":   {"base": (1.0, 0.450785783, 0.063010018), "metallic": 0.0, "roughness": 0.30,
                     "emissive": (1.0, 0.254152094, 0.013702083), "strength": 1.4},
}
MI_PREFIX = "MI_Pack_"


# -----------------------------------------------------------------------------
# The pipeline
#
# FImportAssetParameters::OverridePipelines is a TArray<FSoftObjectPath>, so an
# override has to be a real asset on disk, not an object built in memory. A
# configured copy of the engine's glTF pipeline is duplicated into the pack
# folder, and removed again at the end of the run.
# -----------------------------------------------------------------------------
def build_pipeline(name, force_skeletal, sample_rate):
    path = "{0}/{1}".format(PIPELINE_DIR, name)
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)

    src = unreal.load_asset(DEFAULT_GLTF_ASSETS_PIPELINE)
    if src is None:
        fail("could not load {0}".format(DEFAULT_GLTF_ASSETS_PIPELINE))
        return None

    pipe = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(
        name, PIPELINE_DIR, src)
    if pipe is None:
        fail("AssetTools refused to duplicate the glTF pipeline into {0}".format(PIPELINE_DIR))
        return None

    def prop(obj, name, value):
        try:
            obj.set_editor_property(name, value)
            return True
        except Exception as exc:                      # noqa: BLE001 - report, do not abort
            fail("pipeline property {0} = {1!r} rejected: {2}".format(name, value, exc))
            return False

    # IMPORT UNIFORM SCALE IS 1, AND THAT IS NOT US IGNORING THE PACK.
    #   PACK_README says "Import Uniform Scale = 100, Convert Scene Unit = on",
    #   which is the right instruction for the legacy FBX-style import dialog.
    #   Interchange's glTF translator ALREADY converts the file's metres to
    #   Unreal centimetres, so ImportOffsetUniformScale multiplies on top of a
    #   conversion that has already happened. Measured: with it at 100 the
    #   0.162 m knife imported at 1620 uu instead of 16.2, every model ten times
    #   too big. At 1.0 the knife measures 16.2 uu, the core 37.0, the hand
    #   40.1 with the forearm — the authored metres, in centimetres.
    prop(pipe, "import_offset_uniform_scale", 1.0)
    # Land the assets flat in the destination folder. Left alone the glTF
    # pipeline buries them under <source name>/SkeletalMeshes/<node name>.
    prop(pipe, "use_source_name_for_asset", True)
    prop(pipe, "scene_name_sub_folder", False)
    prop(pipe, "asset_type_sub_folders", False)

    common = pipe.get_editor_property("common_meshes_properties")
    if force_skeletal:
        # WHY THIS AND NOT THE DEFAULT.
        #   Left at IFMT_None the pipeline does honour the rigid hierarchy — the
        #   clips arrive as AnimSequences either way — but it applies
        #   bConvertStaticsInBoneHierarchyToSkeletals per mesh node, so
        #   gloved_hands.glb's 72 mesh nodes came back as 72 separate
        #   SkeletalMeshes sharing one Skeleton. Forcing the type makes the whole
        #   hierarchy ONE skeletal mesh, which is what "bring it in as a Skeletal
        #   Mesh" means and what a first-person view model has to be.
        prop(common, "force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_SKELETAL_MESH)
        # One bone per node, NOT one bone at the origin: the node hierarchy is
        # the whole point — it is what the clips animate.
        prop(common, "single_bone_skeleton", False)
    else:
        prop(common, "force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_NONE)
        prop(common, "convert_statics_with_animated_transform_to_skeletals", True)
        prop(common, "convert_statics_in_bone_hierarchy_to_skeletals", True)
    prop(common, "bake_meshes", True)
    prop(common, "import_sockets", True)
    # One slot per MATERIAL, not one per mesh node. Kept separate, the hands come
    # back with 72 slots called shell_Section3, circuit_cyan_Section5 and so on,
    # and every one of them has to be driven individually to make the glow pulse.
    prop(common, "keep_sections_separate", False)

    skelcommon = pipe.get_editor_property("common_skeletal_meshes_and_animations_properties")
    prop(skelcommon, "import_only_animations", False)
    prop(skelcommon, "use_t0_as_ref_pose", False)

    mesh = pipe.get_editor_property("mesh_pipeline")
    prop(mesh, "import_static_meshes", True)
    prop(mesh, "import_skeletal_meshes", True)
    # THE ONE THAT DECIDES WHETHER YOU GET A VIEW MODEL OR A PARTS BIN.
    #   The engine's own default for this is BySkeleton, but the glTF pipeline
    #   asset ships it as DoNotCombine, so a 72-mesh-node hand came back as 72
    #   separate SkeletalMeshes sharing one Skeleton — with ForceAllMeshAsType =
    #   SkeletalMesh set as well. Forcing the type is not enough; this is the
    #   switch that makes the hierarchy one asset.
    prop(mesh, "combine_skeletal_meshes_behavior",
         unreal.InterchangeCombineSkeletalMeshesBehavior.BY_SKELETON)
    # 107 rigid bones would make a useless and slow ragdoll.
    prop(mesh, "create_physics_asset", False)
    prop(mesh, "build_nanite", False)

    anim = pipe.get_editor_property("animation_pipeline")
    prop(anim, "import_animations", True)
    prop(anim, "import_bone_tracks", True)
    # Left to itself Interchange bakes at 30 Hz and re-times the short clips.
    prop(anim, "use30_hz_to_bake_bone_animation", False)
    prop(anim, "custom_bone_animation_sample_rate", int(sample_rate))

    mat = pipe.get_editor_property("material_pipeline")
    prop(mat, "import_materials", False)
    tex = mat.get_editor_property("texture_pipeline")
    if tex is not None:
        prop(tex, "import_textures", False)

    EAL.save_loaded_asset(pipe, only_if_is_dirty=False)
    log("Pipeline {0} ready (force_skeletal={1}, sample_rate={2})"
        .format(path, force_skeletal, sample_rate))
    return path


def list_folder(folder):
    if not EAL.does_directory_exist(folder):
        return {}
    out = {}
    for p in EAL.list_assets(folder, recursive=True, include_folder=False):
        p = p.split(".")[0]
        asset = unreal.load_asset(p)
        out[p] = type(asset).__name__ if asset is not None else "<unloadable>"
    return out


def run_import(glb, dest, pipeline_path):
    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    source = unreal.InterchangeManager.create_source_data(glb)
    params = unreal.ImportAssetParameters()
    params.set_editor_property("is_automated", True)
    overrides = [unreal.SoftObjectPath(pipeline_path),
                 unreal.SoftObjectPath(DEFAULT_GLTF_PIPELINE)]
    try:
        params.set_editor_property("override_pipelines", overrides)
    except Exception as exc:                          # noqa: BLE001
        fail("override_pipelines rejected ({0}); falling back to plain strings".format(exc))
        params.set_editor_property("override_pipelines", [pipeline_path, DEFAULT_GLTF_PIPELINE])

    result = im.import_asset(dest, source, params)
    log("  import_asset returned {0!r}".format(result))
    return result


# -----------------------------------------------------------------------------
# Import one model, then rename what landed to the contract names.
# -----------------------------------------------------------------------------
def classify(folder):
    kinds = {}
    for path, cls in list_folder(folder).items():
        kinds.setdefault(cls, []).append(path)
    return kinds


def summarise(kinds):
    return ", ".join("{0} x{1}".format(t, len(p))
                     for t, p in sorted(kinds.items())) or "NOTHING"


def import_model(cfg, pipeline_default, pipeline_forced):
    """Import one model. Probe with the default pipeline, then take the forced
    one, and report both results — the difference between them IS the answer to
    the skins:0 question, so it is measured rather than assumed."""
    glb = os.path.join(MODEL_DIR, cfg["glb"])
    if not os.path.isfile(glb):
        fail("source model missing: {0}".format(glb))
        return None

    dest = cfg["folder"]
    if EAL.does_directory_exist(dest):
        EAL.delete_directory(dest)

    log("=== {0}: {1} -> {2}".format(cfg["key"], cfg["glb"], dest))

    run_import(glb, dest, pipeline_default)
    probe = classify(dest)
    log("  PROBE  default pipeline (ForceAllMeshAsType = None) produced: {0}"
        .format(summarise(probe)))

    EAL.delete_directory(dest)
    run_import(glb, dest, pipeline_forced)
    kinds = classify(dest)
    log("  KEPT   forced pipeline (ForceAllMeshAsType = SkeletalMesh) produced: {0}"
        .format(summarise(kinds)))

    meshes = kinds.get("SkeletalMesh", [])
    if len(meshes) != 1:
        fail("{0}: expected exactly one SkeletalMesh, got {1}"
             .format(cfg["key"], len(meshes)))

    return {"cfg": cfg, "kinds": kinds, "probe": summarise(probe),
            "path": "forced (ForceAllMeshAsType = SkeletalMesh)"}


def rename(src, dst):
    if src == dst:
        return dst
    if EAL.does_asset_exist(dst):
        EAL.delete_asset(dst)
    if EAL.rename_asset(src, dst):
        return dst
    fail("could not rename {0} -> {1}".format(src, dst))
    return src


def tidy_names(res):
    """Give what landed the contract names, and report the mapping."""
    cfg = res["cfg"]
    folder = cfg["folder"]
    out = {"mesh": None, "skeleton": None, "physics": None, "anims": {}, "other": []}

    # Never collapse several assets onto one name: a second mesh would silently
    # delete the first. Extras keep their own names and are reported.
    for i, path in enumerate(sorted(res["kinds"].get("SkeletalMesh", []))):
        if i == 0:
            out["mesh"] = rename(path, "{0}/{1}".format(folder, cfg["mesh"]))
        else:
            out["other"].append(path)
    for i, path in enumerate(sorted(res["kinds"].get("Skeleton", []))):
        if i == 0:
            out["skeleton"] = rename(path, "{0}/{1}_Skeleton".format(folder, cfg["mesh"]))
        else:
            out["other"].append(path)
    for i, path in enumerate(sorted(res["kinds"].get("PhysicsAsset", []))):
        if i == 0:
            out["physics"] = rename(path, "{0}/{1}_PhysicsAsset".format(folder, cfg["mesh"]))
        else:
            out["other"].append(path)
    for path in sorted(res["kinds"].get("StaticMesh", [])):
        out["other"].append(path)

    # Animations: match the imported asset's trailing name to a declared clip.
    # Longest match first so Idle_Knife never gets claimed by Idle.
    clips = sorted(cfg["clips"], key=len, reverse=True)
    for path in sorted(res["kinds"].get("AnimSequence", [])):
        leaf = path.rsplit("/", 1)[-1]
        hit = None
        for clip in clips:
            if leaf == clip or leaf.endswith("_" + clip) or leaf.endswith(clip):
                hit = clip
                break
        if hit is None:
            fail("{0}: imported clip {1} matches none of the declared clip names"
                 .format(cfg["key"], leaf))
            out["other"].append(path)
            continue
        dst = "{0}/Anims/{1}{2}".format(folder, cfg["anim_prefix"], hit)
        out["anims"][hit] = rename(path, dst)

    missing = [c for c in cfg["clips"] if c not in out["anims"]]
    if missing:
        fail("{0}: {1} of {2} clips did not arrive: {3}".format(
            cfg["key"], len(missing), len(cfg["clips"]), ", ".join(missing)))
    return out


# -----------------------------------------------------------------------------
# Materials — six instances of the shipped master.
# -----------------------------------------------------------------------------
def build_materials():
    master = unreal.load_asset(MASTER_PATH)
    if master is None:
        fail("master material {0} is missing; run Scripts/import-railgun.sh first"
             .format(MASTER_PATH))
        return {}

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    built = {}
    for name, spec in sorted(MATERIALS.items()):
        asset_name = MI_PREFIX + name
        path = "{0}/{1}".format(MATERIAL_DIR, asset_name)
        if EAL.does_asset_exist(path):
            if not FORCE:
                built[name] = unreal.load_asset(path)
                continue
            EAL.delete_asset(path)

        inst = tools.create_asset(asset_name, MATERIAL_DIR,
                                  unreal.MaterialInstanceConstant,
                                  unreal.MaterialInstanceConstantFactoryNew())
        if inst is None:
            fail("AssetTools refused to create {0}".format(path))
            continue

        MEL.set_material_instance_parent(inst, master)
        b = spec["base"]
        MEL.set_material_instance_vector_parameter_value(
            inst, "BaseColor", unreal.LinearColor(b[0], b[1], b[2], 1.0))
        MEL.set_material_instance_scalar_parameter_value(inst, "Metallic", spec["metallic"])
        MEL.set_material_instance_scalar_parameter_value(inst, "Roughness", spec["roughness"])
        e = spec["emissive"]
        s = spec["strength"]
        MEL.set_material_instance_vector_parameter_value(
            inst, "EmissiveColor", unreal.LinearColor(e[0] * s, e[1] * s, e[2] * s, 1.0))
        MEL.set_material_instance_scalar_parameter_value(inst, "EmissiveIntensity", 1.0)
        EAL.save_loaded_asset(inst, only_if_is_dirty=False)
        built[name] = inst
        log("Built {0}".format(path))
    return built


def assign_materials(mesh_path, mis):
    """Point every material slot at the matching MI_Pack_* instance."""
    mesh = unreal.load_asset(mesh_path)
    if mesh is None:
        return []
    try:
        slots = mesh.get_editor_property("materials")
    except Exception as exc:                          # noqa: BLE001
        fail("{0}: cannot read material slots: {1}".format(mesh_path, exc))
        return []

    report = []
    changed = False
    for slot in slots:
        try:
            slot_name = str(slot.get_editor_property("material_slot_name"))
        except Exception:                             # noqa: BLE001
            slot_name = ""
        key = slot_name
        # Interchange may decorate the slot name; find the material it means.
        if key not in mis:
            key = next((m for m in mis if m in slot_name), None)
        if key is None:
            report.append((slot_name, "<no pack material of that name>"))
            continue
        slot.set_editor_property("material_interface", mis[key])
        report.append((slot_name, MI_PREFIX + key))
        changed = True
    if changed:
        mesh.set_editor_property("materials", slots)
        EAL.save_loaded_asset(mesh, only_if_is_dirty=False)
    return report


# -----------------------------------------------------------------------------
# The report. Everything downstream reads this, so it prints real measurements,
# not restated intentions.
# -----------------------------------------------------------------------------
def describe(cfg, names, slots):
    lines = []
    lines.append("ASSET {0}".format(cfg["key"]))
    mesh = unreal.load_asset(names["mesh"]) if names["mesh"] else None
    if mesh is None:
        lines.append("  mesh: NONE")
    else:
        lines.append("  mesh: {0}  ({1})".format(names["mesh"], type(mesh).__name__))
        try:
            box = mesh.get_bounds().box_extent
            org = mesh.get_bounds().origin
            lines.append("    bounds origin=({0:.2f}, {1:.2f}, {2:.2f}) "
                         "extent=({3:.2f}, {4:.2f}, {5:.2f}) uu  -> size ({6:.2f}, {7:.2f}, {8:.2f}) uu"
                         .format(org.x, org.y, org.z, box.x, box.y, box.z,
                                 box.x * 2, box.y * 2, box.z * 2))
        except Exception as exc:                      # noqa: BLE001
            lines.append("    bounds unavailable: {0}".format(exc))
    lines.append("  skeleton: {0}".format(names["skeleton"] or "NONE"))

    skel = unreal.load_asset(names["skeleton"]) if names["skeleton"] else None
    if skel is not None:
        try:
            lines.append("    bones: {0}".format(len(skel.get_editor_property("bone_tree"))))
        except Exception:                             # noqa: BLE001
            pass

    for slot_name, mi in slots:
        lines.append("  slot {0:<16} -> {1}".format(slot_name, mi))

    lines.append("  clips: {0} of {1}".format(len(names["anims"]), len(cfg["clips"])))
    tracks = None
    for clip in cfg["clips"]:
        path = names["anims"].get(clip)
        if path is None:
            lines.append("    {0:<18} MISSING".format(clip))
            continue
        a = unreal.load_asset(path)
        length = -1.0
        frames = -1
        try:
            length = a.get_play_length()
            frames = a.get_editor_property("number_of_sampled_frames")
        except Exception:                             # noqa: BLE001
            pass
        fps = (frames - 1) / length if length > 0 and frames > 1 else 0.0
        lines.append("    {0:<18} {1:>7.4f}s  {2:>4} frames  ~{3:>5.1f} fps   {4}"
                     .format(clip, length, frames, fps, path))
        if tracks is None:
            try:
                tracks = list(unreal.AnimationLibrary.get_animation_track_names(a))
            except Exception:                         # noqa: BLE001
                try:
                    tracks = list(unreal.AnimationBlueprintLibrary
                                  .get_animation_track_names(a))
                except Exception:                     # noqa: BLE001
                    tracks = []
    if tracks:
        lines.append("  animated bones ({0}): {1}".format(
            len(tracks), ", ".join(str(t) for t in tracks)))
    for extra in names["other"]:
        lines.append("  ALSO LANDED: {0}".format(extra))
    return lines


def main():
    log("import_pack.py starting (FORCE={0}, ONLY={1!r})".format(FORCE, ONLY or "all"))

    mis = build_materials()

    report = []
    built = {}
    for cfg in MODELS:
        if ONLY and cfg["key"] not in ONLY.split(","):
            continue

        rate = cfg["sample_rate"]
        if rate not in built:
            built[rate] = (
                build_pipeline("{0}_{1}_Probe".format(PIPELINE_NAME, rate), False, rate),
                build_pipeline("{0}_{1}".format(PIPELINE_NAME, rate), True, rate),
            )
        pipeline_default, pipeline_forced = built[rate]
        if pipeline_default is None or pipeline_forced is None:
            fail("no usable pipeline at {0} Hz; {1} not imported".format(rate, cfg["key"]))
            continue

        res = import_model(cfg, pipeline_default, pipeline_forced)
        if res is None:
            continue
        names = tidy_names(res)
        slots = assign_materials(names["mesh"], mis) if names["mesh"] else []
        report.append((cfg, names, slots, res))

    log("")
    log("================ PACK IMPORT REPORT ================")
    for cfg, names, slots, res in report:
        log("  bake rate: {0} Hz".format(cfg["sample_rate"]))
        log("  default pipeline would have produced: {0}".format(res["probe"]))
        log("  kept: {0}".format(res["path"]))
        for line in describe(cfg, names, slots):
            log(line)
        log("")

    log("MATERIALS (all instances of {0}):".format(MASTER_PATH))
    for name in sorted(MATERIALS):
        log("  {0}/{1}{2}".format(MATERIAL_DIR, MI_PREFIX, name))

    # The configured pipelines are scaffolding, not art.
    if EAL.does_directory_exist(PIPELINE_DIR):
        EAL.delete_directory(PIPELINE_DIR)

    unreal.EditorAssetLibrary.save_directory(PACK_ROOT, only_if_is_dirty=False, recursive=True)

    if _failures:
        log("{0} PROBLEM(S):".format(len(_failures)))
        for f in _failures:
            log("  - {0}".format(f))
    else:
        log("no problems reported.")
    log("================ END REPORT ================")


main()
