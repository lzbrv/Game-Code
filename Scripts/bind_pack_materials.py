# =============================================================================
# Trace — bind_pack_materials.py
#
# Points every material slot on the imported asset-pack skeletal meshes at the
# MI_Pack_* instance whose name matches the slot.
#
# WHY THIS EXISTS
#   Interchange imported the five pack models and separately created the six
#   material instances, but bound NEITHER: every slot on SK_TraceCore and
#   SK_TraceHands came through on /Engine/EngineMaterials/WorldGridMaterial, the
#   grey developer checkerboard. The art was in the game and looked like nothing.
#   A verifier caught it; it is exactly the "the mesh resolved" versus "the right
#   thing is on screen" gap that a probe exists to tell apart.
#
#   Slot names are the raw glTF material names (shell, carbon, circuit_cyan,
#   plating, core_amber, seam), which is also how MI_Pack_* are named, so the
#   match is by name and never by index — slot order is an artefact of the
#   exporter, not a contract.
#
# AND IT SETS THE SKELETAL-MESH USAGE FLAG — see ensure_skeletal_usage() below.
#   Binding the slot was necessary and not sufficient: the renderer threw the
#   bound material away again for want of one flag, and did it silently enough
#   that every probe in the project still printed the right material name.
# =============================================================================
import unreal

PACK = "/Game/Trace/Art/Pack"
MI_DIR = PACK + "/Materials"
MESHES = [
    PACK + "/Hands/SK_TraceHands",
    PACK + "/Core/SK_TraceCore",
    PACK + "/Knife/SK_TraceKnife",
    PACK + "/Pistol/SK_TracePistolPack",
    PACK + "/Smg/SK_TraceSmgPack",
]

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
SKELETAL_USAGE = unreal.MaterialUsage.MATUSAGE_SKELETAL_MESH
problems = []

instances = {}
for asset in EAL.list_assets(MI_DIR, recursive=False, include_folder=False):
    obj = unreal.load_asset(asset.split(".")[0])
    if isinstance(obj, unreal.MaterialInstanceConstant):
        instances[obj.get_name().replace("MI_Pack_", "").lower()] = obj
unreal.log("[Trace] found %d pack material instances: %s" % (len(instances), sorted(instances)))


# -----------------------------------------------------------------------------
# THE USAGE FLAG — the other half of "is the right thing on screen?"
#
# Every mesh in MESHES is SKELETAL. The six MI_Pack_* are instances of
# M_TraceRailgun, a master authored for the railgun's STATIC meshes, and nobody
# ever told it that it would also be worn by skinned geometry. A material with
# no SkeletalMesh usage flag has no skinned shader permutation compiled, so
# FSkeletalMeshSceneProxy throws the bound material away at proxy-build time and
# draws UMaterial::GetDefaultMaterial() instead. The engine says so, at load:
#
#   LogMaterial:     Warning: Material .../MI_Pack_circuit_cyan missing usage
#                    flag SkeletalMesh! Default Material will be used in game.
#   LogSkeletalMesh: Warning: Material with missing usage flag was applied to
#                    skeletal mesh /Game/Trace/Art/Pack/Core/SK_TraceCore
#
# That is this script's own bug one layer down, and it is the worse of the two
# because it HIDES FROM A PROBE. With WorldGridMaterial the readback said
# "WorldGridMaterial" and a verifier caught it in a day. Here the component
# genuinely owns a MID of MI_Pack_circuit_cyan — CreateDynamicMaterialInstance
# succeeds, GetMaterial() returns it, Trace.Hands.Probe prints its name and
# "readback OK" — while the renderer draws the default material and every
# EmissiveIntensity write lands on something nobody can see. The gloves', the
# Core's and the knife's whole emissive drive was a no-op on screen.
#
# TWO THINGS MAKE THE FIX STICK, AND BOTH ARE EASY TO GET WRONG:
#
#   It goes on the MASTER, not on the instances. An instance's UsageFlags are
#   recomputed from its parent's every load (UMaterialInstance::
#   UpdateOverridableBaseProperties), and it is the master's flag that makes the
#   engine compile the skinned permutation the proxy is looking for. A
#   per-instance BasePropertyOverride would claim the usage without the shaders
#   behind it.
#
#   And it has to be SAVED. In an editor session the engine quietly sets the
#   flag in memory and marks the package dirty — "The material instance will
#   recompile every editor launch until resaved" — so an editor run looks
#   correct while the asset on disk is not. That courtesy does not happen in
#   -game or in a cook: FApp::IsGame() is true, and the branch taken there is
#   the warning and the default material. Which is why this must be a persisted
#   asset change and not something the editor does for us.
#
# The flag costs the railgun's static-mesh instances a skinned permutation they
# will not use. That is the documented price of one shared master, and it is
# far cheaper than a second master to keep in sync with the first.
# -----------------------------------------------------------------------------
def ensure_skeletal_usage(pack_instances):
    masters = {}
    for key in sorted(pack_instances):
        master = pack_instances[key].get_base_material()
        if master is None:
            problems.append("MI_Pack_%s has no base material" % key)
            continue
        masters[master.get_path_name()] = master

    for path in sorted(masters):
        master = masters[path]
        if MEL.has_material_usage(master, SKELETAL_USAGE):
            unreal.log("[Trace] %-22s already carries SkeletalMesh usage" % master.get_name())
            continue
        # SetBaseMaterialUsage recompiles the master and everything under it.
        MEL.set_base_material_usage(master, SKELETAL_USAGE, True)
        if not EAL.save_loaded_asset(master, only_if_is_dirty=False):
            problems.append("could not save %s after setting SkeletalMesh usage" % path)
        unreal.log("[Trace] %-22s SkeletalMesh usage SET and saved" % master.get_name())

    # Read the flag back off the INSTANCES, because the instance is what the
    # mesh slot actually wears and what the proxy actually asks. Then resave
    # them, so the derived flag is on disk and the engine stops re-deriving and
    # re-warning about it on every launch.
    ok = 0
    for key in sorted(pack_instances):
        inst = pack_instances[key]
        if not MEL.has_material_usage(inst, SKELETAL_USAGE):
            problems.append("MI_Pack_%s still reports no SkeletalMesh usage" % key)
            continue
        if not EAL.save_loaded_asset(inst, only_if_is_dirty=False):
            problems.append("could not resave MI_Pack_%s" % key)
            continue
        ok += 1
    unreal.log("[Trace] SkeletalMesh usage verified and saved on %d/%d pack instances"
               % (ok, len(pack_instances)))


ensure_skeletal_usage(instances)

for path in MESHES:
    mesh = unreal.load_asset(path)
    if mesh is None:
        problems.append("missing mesh %s" % path)
        continue

    materials = mesh.get_editor_property("materials")
    bound = 0
    rebuilt = []
    for index, entry in enumerate(materials):
        slot = str(entry.get_editor_property("material_slot_name")).lower()
        pick = instances.get(slot)
        if pick is None:
            # Interchange sometimes decorates a slot name; fall back to containment.
            for key, value in instances.items():
                if key in slot:
                    pick = value
                    break
        if pick is None:
            problems.append("%s slot %d '%s' matched no MI_Pack_*" % (mesh.get_name(), index, slot))
            rebuilt.append(entry)
            continue

        # A FRESH STRUCT PER SLOT, not a mutated one. Iterating the array yields
        # COPIES, so `entry.set_editor_property(...)` writes to a temporary and
        # assigning the same list back changes nothing — the exact silent no-op
        # that left the railgun's slots empty when its importer was first written,
        # and the reason this script reported "5/5 bound" while every slot was
        # still WorldGridMaterial.
        fresh = unreal.SkeletalMaterial()
        fresh.set_editor_property("material_interface", pick)
        fresh.set_editor_property("material_slot_name",
                                  entry.get_editor_property("material_slot_name"))
        rebuilt.append(fresh)
        bound += 1

    mesh.set_editor_property("materials", rebuilt)
    EAL.save_loaded_asset(mesh, only_if_is_dirty=False)
    unreal.log("[Trace] %-22s %d/%d slots bound" % (mesh.get_name(), bound, len(materials)))

    for index, entry in enumerate(mesh.get_editor_property("materials")):
        mat = entry.get_editor_property("material_interface")
        name = mat.get_name() if mat else "NONE"
        if mat is None or "WorldGrid" in name:
            problems.append("%s slot %d still %s" % (mesh.get_name(), index, name))

if problems:
    unreal.log_error("[Trace] bind_pack_materials FAILED:")
    for p in problems:
        unreal.log_error("[Trace]   - %s" % p)
else:
    unreal.log("[Trace] every pack slot now wears its MI_Pack_* instance, and the master "
               "behind them is flagged for skeletal meshes, so the renderer keeps it.")
