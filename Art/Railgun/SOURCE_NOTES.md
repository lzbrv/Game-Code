# Railgun → Unreal Engine

## Files

- `railgun_fire.glb` — mesh + materials + the baked **`Fire`** animation clip. Get it from the
  **Export for Unreal (.glb)** button in the **top-right** of the viewer. (The stage's own
  bottom-right OBJ/GLB toolbar exports the mesh only — no `Fire` clip.)
- `fire_curves.json` — the emissive-intensity curve (glTF can't animate material emission, so
  this drives it engine-side).

## Import

1. Drag `railgun_fire.glb` into the Content Browser.
2. In the import dialog: **Skeletal Mesh = off**, **Import Animations = on**,
   **Import Rig = on**. The clip imports as a rigid/node animation on the node hierarchy.
   - If you'd rather have a skeletal weapon, tick Skeletal Mesh — the two pivot nodes
     (`wall_pivot_left/right`) become bones you can retarget.
3. Import Uniform Scale: the model is authored in metres (~1.22 m long overall).
   Unreal works in centimetres, so set **Import Uniform Scale = 100**.
4. Convert Scene Unit: on.

## Animated nodes

| Node | Motion |
|---|---|
| `wall_pivot_left` / `_right` | the two rail walls (rails, channel lights, muzzle prongs) — shiver during charge, throw ±75 mm (±7.5 units) apart on discharge with an outward cant |
| `railgun` | recoil: 45 mm back, −0.10 rad pitch |

Clip length **1.90 s** — charge 1.05 s, discharge 0.10 s, decay 0.75 s. The discharge frame
(t = 1.05 s) is where you spawn the beam / muzzle flash.

## Emissive

Two materials glow: `circuit_cyan` (base emissive #25E6FF) and `core_amber` (#FF8A1F).
Make each a Material Instance with a scalar **EmissiveIntensity** parameter, then drive it from
a Curve Float built from `fire_curves.json` (`time`, `cyan`, `amber` — multipliers on the base
intensity, peaking at 5.2× / 5.6× on discharge).

## Beam

The channel between the rails runs the length of the weapon. Spawn the beam at the muzzle
aperture (local **z ≈ −0.63 m**, y ≈ 0.045 m — **−63 units / +4.5 units** in Unreal after the
×100 import scale) and fire it along **−Z**.
