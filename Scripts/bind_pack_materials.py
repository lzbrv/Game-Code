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
problems = []

instances = {}
for asset in EAL.list_assets(MI_DIR, recursive=False, include_folder=False):
    obj = unreal.load_asset(asset.split(".")[0])
    if isinstance(obj, unreal.MaterialInstanceConstant):
        instances[obj.get_name().replace("MI_Pack_", "").lower()] = obj
unreal.log("[Trace] found %d pack material instances: %s" % (len(instances), sorted(instances)))

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
    unreal.log("[Trace] every pack slot now wears its MI_Pack_* instance.")
