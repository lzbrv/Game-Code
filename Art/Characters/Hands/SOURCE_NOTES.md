# HandModel2.fbx — the owner's first-person arms rig

## Provenance

| | |
|---|---|
| File | `Art/Characters/Hands/HandModel2.fbx` |
| Arrived as | `Hand Model2.fbx`, **at the repo root**, 2026-08-30 |
| Moved here | 2026-08-30, tranche D29H-IMPORT, plain `mv` (space dropped from the name) |
| Size / sha256 | 1,804,780 bytes / `c70514138f3baa9ced0521096a7e2fd6077bec0d1a437f509d2ac81b583716f5`[^1] |
| Format | binary FBX 7400, written by Blender's `io_scene_fbx` from `Hand Model2.blend` |
| Owner's words | *"I need to test this first person arms rig i created. See if you can animate it to hold the knife and guns correctly. The ring fingers bend opposite the rest, so you'll have to invert that. Implement this only in the practice range, for testing purposes."* |

[^1]: `shasum -a 256 Art/Characters/Hands/HandModel2.fbx`

Root files are a documented hazard in this repo (see `README.md` and the pile of
zips still sitting up there); source art belongs under `Art/`. `Art/Characters/`
already held `Rocco/RoccoTest.fbx`, so `Art/Characters/Hands/` is where this went.

**It is `.fbx`, so it is LFS-tracked and lockable** (`.gitattributes:71`). Lock it
before editing if anybody else might be: `Scripts/lock.sh`.

---

## What is in the file — MEASURED, 2026-08-30

Everything below was read out of the file itself and is re-measured on every run
of `Scripts/import-hands.sh` (the `rebuild` stage prints the same numbers, and
fails if the ones it depends on stop holding).

### The mesh — one object, tiny, and genuinely skinned
* One `Model` of class `Mesh`, `Arm 2`; geometry `Circle.001`.
* **336 vertices**, 1,340 polygon-vertex indices, **338 polygons**
  (320 quads, 16 triangles, 2 hexagons) → 664 triangles.
* Normals (`ByPolygonVertex`/`IndexToDirect`, 1,014 distinct) and one UV set
  (`UVMap`, 336 distinct) are present. **No smoothing groups** — the FBX import
  path warns about it; the glTF rebuild path carries the authored normals
  directly and does not.
* **Zero `Material` objects.** There is no surface in this file at all — not a
  grey placeholder, not a texture reference. Nothing to import, nothing thrown
  away. (Contrast `Rocco/RoccoTest.fbx`, which has ten grey materials and six
  dead texture paths.)
* Every one of the 336 vertices is skinned. Influences per vertex: 1×30, 2×192,
  3×62, 4×30, 5×14, 6×4, 7×4 — **max 7**. Cluster weights are NOT normalised
  (sums 0.888 … 1.002, mean 0.978); UE renormalises on import.

### Units and axes
`GlobalSettings`: `UnitScaleFactor = 1.0`, Y-up (`UpAxis=1`), right-handed.
The geometry array is in **metres, Blender Z-up**; `Model "Arm 2"` carries
`Lcl Rotation (-90, 0, 0)` and `Lcl Scaling (100, 100, 100)`, so bind-world is
**centimetres, Y-up**. In Unreal that is, exactly:

```
X  -91.44 ..  91.44 uu     (span 182.89 — a T-posed PAIR of arms)
Y  -26.04 ..   6.43 uu     (span  32.46)
Z  136.71 .. 161.09 uu     (span  24.38 — shoulder height, NOT the origin)
```

### The skeleton — a whole Rigify human, of which 4% is used
* **1,251 `Model` nodes.** Families: `DEF-` 272, `ORG-` 266, `MCH-` 212, plus
  controls. Includes an **entire face** — brow, lid, lip, jaw, chin, ear, nose,
  cheek, tongue, eye — and a full spine (`spine` … `spine.006`).
* **705 skin clusters, of which only 47 carry `Weights`/`Indexes`.** The other
  658 are empty. Those 47 are the whole of the rig that does anything:

  | chain | left | right |
  |---|---|---|
  | `DEF-shoulder` | ✓ | ✓ |
  | `DEF-upper_arm`, `DEF-upper_arm.001` | ✓ ✓ | ✓ ✓ |
  | `DEF-forearm`, `DEF-forearm.001` | ✓ ✓ | ✓ ✓ |
  | `DEF-hand` | ✓ | ✓ |
  | `DEF-palm.01/.02/.03` | ✓ ✓ ✓ | ✓ ✓ ✓ |
  | `DEF-palm.04` | **absent** | ✓ |
  | `DEF-thumb.01/.02/.03` | ✓ ✓ ✓ | ✓ ✓ ✓ |
  | `DEF-f_index.01/.02/.03` | ✓ ✓ ✓ | ✓ ✓ ✓ |
  | `DEF-f_middle.01/.02/.03` | ✓ ✓ ✓ | ✓ ✓ ✓ |
  | `DEF-f_ring.01/.02/.03` | ✓ ✓ ✓ | ✓ ✓ ✓ |
  | `DEF-f_pinky.01/.03` | ✓ ✓ | ✓ ✓ |
  | `DEF-f_pinky.02` | **unweighted** | **unweighted** |

  23 left + 24 right = 47.
* Imported as-is through Interchange the file yields a SkeletalMesh with
  **1051 bones**, 435 of them face bones, and one material slot on
  `WorldGridMaterial`. UE 5.8's Interchange exposes **no bone filter of any
  kind** (the full property list of the three pipeline property objects was
  dumped and searched — `bone_influence_limit`, `skeletal_mesh_import_content_type`
  and `update_skeleton_reference_pose` are the closest things and none of them
  include or exclude bones), and bones cannot be renamed or removed after import
  from a commandlet. That is why the rig is rebuilt rather than imported —
  see `Scripts/import_hands.py`.

### The skinning identity this project relies on
Blender does **not** write the cluster `Transform` as the mesh's global matrix
(473 distinct values across the 705 clusters). It writes it per-cluster such that

```
Transform_j * TransformLink_j  ==  MeshGlobal      for all 47 weighted clusters
                                                   (max element error 3.3e-5)
```

`TransformLink` and the file's `BindPose` agree **exactly** — 0 of 705 bones
differ by more than 1e-6 — so the "BindPose Matrix generation acquired 2
different Matrices from FbxCluster vs FbxPose" warning the FBX importer prints
is about the mesh node, not about any bone.

---

## Two defects in the source rig

### 1. The ring finger's bone roll is inverted — the owner's own complaint
`DEF-f_ring.01/.02/.03` on **both** hands have their flexion axis (Blender's
bone-local X) rolled ~180° from `f_index`, `f_middle` and `f_pinky`. Measured on
the bind pose, as the signed angle about each bone's own axis from a hand-plane
reference:

| joint | roll, left hand | roll, right hand |
|---|---|---|
| `index.01/.02/.03` | −88.5° | +88.5° |
| `middle.01/.02/.03` | −90.1° | +90.1° |
| `pinky.01/.02/.03` | −80.6° | +80.6° |
| **`ring.01/.02/.03`** | **+88.9°** | **−88.9°** |
| `thumb.01/.02/.03` | +0.3° … +1.8° | −0.3° … −1.8° |

`ring − middle` = **179.0°** on the left, **−179.1°** on the right, at every one
of the three joints. That is a bone-roll flip in the source rig, not a posing
mistake, and it is preserved verbatim by the import so the pose stage can see
it. **The thumb is not part of this**: its roll sits ~90° from the fingers',
which is what an opposable thumb is supposed to look like — do not "fix" it.

### 2. The LEFT pinky is parented to the world, not to the hand
`ORG-palm.04.L` — the left pinky's metacarpal and the parent of the whole left
pinky chain — is parented to the rig's `root`. Its mirror `ORG-palm.04.R` is
parented to `DEF-hand.R` correctly. Left as authored, the left pinky does not
follow the left hand.

`Scripts/import_hands.py` reparents it to `hand_left`, matching its own mirror,
and says so in its report. **That is the only hierarchy edit the import makes.**
If the rig is ever re-exported from Blender, fix it there and this note stops
being true.

---

## What the import produces

`Scripts/import-hands.sh` → `Content/Trace/Characters/Hands/SK_TraceArms`
(+ `SK_TraceArms_Skeleton`), **51 bones**, 47 of them weighted, one material slot
`shell` bound to the shipped `MI_Pack_shell`. Bone names follow the naming the
shipped first-person hands already use (`hand_right`, `index_right_0`,
`ring_left_2`, …) so the C++ that poses those — `TraceKnifeView.cpp`'s knuckle
basis, `TraceCharacterInternal.h`'s hidden-bone list — keeps meaning something.
The full mapping and the reasoning are in the header of `Scripts/import_hands.py`.

**Scale, against the shipped hands** (`SK_TraceHands`, measured side by side):

| | pack hands | these arms | ratio |
|---|---|---|---|
| `hand_right` → `index_right_2` | 9.14 uu | 17.83 uu | **1.95×** |
| `hand_right` → `index_right_0` | 5.33 uu | 12.41 uu | 2.33× |
| `index_right_0` → `pinky_right_0` | 5.40 uu | 7.04 uu | 1.30× |
| `hand_left` → `hand_right` | 18.46 uu | 142.83 uu | 7.74× (T-pose vs a held pose) |
| asset bounds | 22.9 × 40.1 × 25.2 uu | 182.9 × 32.5 × 24.4 uu | |

So the arms are about **twice the size** of the hands the first-person camera is
built around, they are T-posed, and they stand at Z 137–161 uu rather than at the
origin. None of that is a defect — it is a body rig, not a view model — but it
means the pose stage owns a transform as well as a pose.
