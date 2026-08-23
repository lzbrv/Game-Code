# Rocco → Unreal Engine

Test character model for **Rocco**, one of the ten characters in
`Content/Trace/Data/Characters/`. This is the source art; the imported assets live at
`Content/Trace/Characters/Rocco/` as `SK_Rocco`, `SK_Rocco_Skeleton`, `SK_Rocco_PhysicsAsset`,
`M_RoccoPlaceholder` and ten `MI_Rocco_*` per-slot instances of it, with the retarget that animates
them in `Retarget/` and `Anims/` beside them.

## Files

- `RoccoTest.fbx` — Kaydara FBX binary 7400, 513 KB, exported from Blender. Skinned skeletal
  mesh, **no animation** (the file contains zero `AnimStack` nodes).

## Import

    Scripts/import-rocco.sh
    Scripts/retarget-rocco.sh

In that order: the first imports the mesh, the second makes it move. Both are idempotent — run
them as often as you like; neither ever produces `SK_Rocco_1` or `ABP_Unarmed_Rocco_1`. Read the
header of `Scripts/import_rocco.py` for why each pipeline switch is set the way it is, and of
`Scripts/retarget_rocco.py` for why the retarget is baked offline rather than run per frame.

## What is actually in the file

**The character is in here twice.** Two skinned meshes, two 24-bone armatures, same 4172-polygon
topology, rotated 90° from each other about the up axis:

| FBX model | geometry | verts | UV set | materials | armature |
|---|---|---|---|---|---|
| `Rocco Model 1` | `char1.001` | 2515 | `UVMap` | **10 slots** | `target_character` |
| `char1` | `char1` | 2220 | `uv` | none at all | `Armature` |

`Rocco Model 1` is the finished model — it is the one with the materials, the UV split and a
greyscale vertex-colour layer. The importer keeps it and deletes the other.

Because both armatures use the **same 24 bone names**, the FBX translator uniquifies whichever it
reaches second — Rocco's — so **every imported bone carries a trailing `1`**: `Hips1`, `LeftHand1`,
`Spine011`, `head_end1`, under a root bone called `target_character`.

That cannot be undone from a commandlet, and both obvious repairs are dead ends in 5.8:
`USkeletonModifier` renames bones but its commit path opens dialogs and **hangs** an `-unattended`
run indefinitely; and sockets cannot be authored from script either, because `USkeleton::Sockets`
and `USkeletalMesh::Sockets` are protected and `USkeletalMeshSocket::SocketName` is
`BlueprintReadOnly`. Skeleton naming is an interactive-editor job. So the importer reports the
mapping instead — see the ATTACH POINTS table it prints — and anything that names a bone must use
`RightHand1`, not `hand_r`.

**None of that stops him animating.** `Scripts/retarget-rocco.sh` builds an IK Rig for each
skeleton and an IK Retargeter between them, then bakes Epic's `ABP_Unarmed` — the same blend space
every Mannequin runs — onto `SK_Rocco_Skeleton`, as
`Content/Trace/Characters/Rocco/Anims/ABP_Unarmed_Rocco` plus the twenty sequences it plays.
Rocco's retarget chains are written by hand against the suffixed names, so the trailing `1` costs
one table in that script and nothing else. What it does still cost is bone LOOKUPS by name from
gameplay code — the knife's `hand_r`, the anim probe's `foot_l` — and that is what
`ATraceCharacter::ResolveBodyBoneName` exists for.

## Scale and orientation

Authored in **centimetres** (`UnitScaleFactor = 1.0`), Y-up, so Import Uniform Scale is **1.0** —
do not set it to 100 the way `Art/Railgun/SOURCE_NOTES.md` says for the metre-authored weapons.

- **164.0 uu tall**, origin at the feet (Z range −0.03 … 164.00).
- **Faces +X** in mesh space, which is already Unreal's forward. The Mannequin faces +Y and needs
  `MeshYaw = −90`; Rocco needs **0**.
- The Mannequin's mesh is 180.5 uu and the capsule is 176, so Rocco is a head shorter than the
  body every other measurement in this project was tuned against.

## The colour is missing, and that is the export's doing

**Nothing is embedded.** There is not one `Content` block anywhere in the FBX. Its six texture
references are external paths on the artist's own machine, without file extensions:

    C:\Users\ranen\OneDrive\Documents\Blender\textures\packed\Material_004_Base_Color
    C:\Users\ranen\OneDrive\Documents\Blender\textures\packed\Metal_Color
    ... and four more

All ten FBX materials carry the **same** DiffuseColor, `(0.80, 0.80, 0.80)` — Blender's untouched
default, which means only that the albedo was coming from those missing textures. So there is no
per-slot colour in the file to reconstruct the look from either.

**What other players see is a flat, single-colour body**: the right silhouette, the right skin
weights, the right motion, and no albedo anywhere on it. `Scripts/import_rocco.py` does not hide
that — it *counts* the embedded images (0) and lists the external references, and prints the
verdict in its report, so this section cannot quietly go stale and a re-export flips the report by
itself.

The mesh's UVs are intact and laid out in 0–1, so the art is one export away from being right. To
fix, the artist re-exports with **Path Mode = Copy** and **Embed Textures** ticked, or hands over
the six PNGs. The ten material slots keep their FBX names (`Material_001` … `Material_010`), so a
textured re-import lands on the same slots.

### What the importer *can* recover, and how much it is worth

Colour is not quite the only per-slot data in the file. `Shininess` and `ReflectionFactor` differ
material to material, and those are Blender's `roughness` and `metallic` on the way out. Each slot
therefore gets its own `MI_Rocco_<slot>` instance of `M_RoccoPlaceholder` carrying its own two
numbers, instead of all ten pointing at one flat material:

| slot | FBX material | Shininess | Roughness | Metallic |
|---|---|---:|---:|---:|
| Material_004 | Material.004 | 18.407 | 0.571 | 0.000 |
| Material_002 | Material.002 |  8.042 | 0.716 | **0.773** |
| Material_001 | Material.001 | 20.000 | 0.553 | 0.000 |
| Material_003 | Material.003 |  0.000 | **1.000** | 0.000 |
| Material_005 | Material.005 |  2.035 | 0.857 | 0.000 |
| Material_006 | Material.006 | 22.947 | **0.521** | 0.177 |
| Material_007 | Material.007 |  2.303 | 0.848 | 0.000 |
| Material_008 | Material.008 | 20.000 | 0.553 | 0.000 |
| Material_009 | Material.009 | 20.000 | 0.553 | 0.000 |
| Material_010 | Material.010 | 18.407 | 0.571 | 0.000 |

**Be honest about the size of that.** It is correct, it is the artist's own data, and it gives a
later textured re-import ten per-slot handles instead of one — but on screen it is a small effect.
The metal is on 177 and 544 of the mesh's 4172 polygons, and a roughness spread under a strong flat
team tint reads as slightly different highlight width, not as different materials. **It does not
make Rocco look textured, and nothing in this file can.** Measured both ways: with all ten slots
forced back onto the one flat material the body reads the same at a glance. The glossy dark harness
you can see in any Rocco screenshot is the knife rig, a separate component, not one of these slots.

### The vertex-colour layer is not ambient occlusion

The mesh carries a greyscale layer named `Attribute`, and it is tempting to multiply into base
colour to break up the flatness. Don't: it is ByPolygonVertex / IndexToDirect, 12516 indices into
48 entries, all `r == g == b`, spread **evenly** from 0.00 to 1.00, mean 0.498, near-uniform
histogram. Occlusion clusters near white; a flat 0-to-1 spread is a mask or a per-island id. Using
it would darken half the body to black. It is imported and kept, and deliberately unread.

Two other things the exporter should be asked for while it is open: **Export Smoothing Groups**
(Interchange warns that none were found, so normals come from the file's per-vertex normals
alone) and a single armature per file.
