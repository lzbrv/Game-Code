# Railgun SMG → Unreal Engine

Companion to the pistol kit (`unreal/`). Same art style, same wall-expansion rig — but the
animation is a **loopable single-shot cycle** rather than one long charge, because the weapon
is full auto.

## Files

- `railgun_smg.glb` — mesh + materials + two clips, **`Fire`** and **`Reload`**. Get it from the
  **Export for Unreal (.glb)** button at the top-right of the SMG viewer. (The stage's own
  bottom-right OBJ/GLB toolbar exports the mesh only — no clips.)
- `smg_stats.json` — the weapon numbers below, machine-readable for a data table.

## Import

1. Drag `railgun_smg.glb` into the Content Browser.
2. Import dialog: **Skeletal Mesh = off**, **Import Animations = on**, **Import Rig = on**.
   - Tick Skeletal Mesh instead if you want bones — `wall_pivot_left/right` and `mag_pivot`
     come through as joints you can retarget.
3. **Import Uniform Scale = 100** (authored in metres, ~1.28 m long overall), **Convert Scene
   Unit = on**.

## Clips

| Clip | Length | Notes |
|---|---|---|
| `Fire` | **0.100 s** | one shot at 600 RPM. **Set to loop** — play it on repeat while the trigger is held. First and last keys match, so it cycles without a hitch. |
| `Reload` | **0.800 s** | cell drops out (0–0.26 s), new cell rides up (0.26–0.66 s), seats with a bump (0.66–0.80 s). |

## Animated nodes

| Node | Motion |
|---|---|
| `wall_pivot_left` / `_right` | the two rail walls — snap ±42 mm (±4.2 units) apart per shot, then elastic-settle |
| `mag_pivot` | the 40-round cell — drops 300 mm (30 units) on `Reload` |
| `railgun_smg` | recoil: 20 mm back, −0.045 rad pitch per shot |

## Weapon stats

| | |
|---|---|
| Fire mode | full auto |
| Fire rate | 600 RPM (0.100 s between shots) |
| Magazine | 40 |
| Reload | 0.80 s |
| Damage — head | 33 |
| Damage — body | 18 |
| Damage — leg | 12 |

Shots to kill (100 HP): 3 head / 6 body / 9 leg. At 600 RPM that's 0.20 s, 0.50 s, 0.80 s
time-to-kill respectively.

**Weapon swap:** reuse the pistol's existing pull-out time — no new equip clip here, so drive
the swap from your current weapon-switch state machine and let `Fire` start on the frame the
SMG becomes active.

## Emissive

Same two materials as the pistol: `circuit_cyan` (#25E6FF) and `core_amber` (#FF8A1F). Make
each a Material Instance with a scalar **EmissiveIntensity** parameter:

- **circuit_cyan** — 1.8× base at rest, spiking to **4.8×** on the shot frame and falling back
  across the 0.1 s cycle. A short Curve Float looped with `Fire` does it.
- **core_amber** — scales with remaining ammo: **1.4× at full, 0.35× at empty**. Drive it from
  your ammo count, not from the clip, so the cell visibly drains as the magazine empties.

## Beam

Spawn at the muzzle aperture: local **z ≈ −0.59 m, y ≈ 0.045 m** (**−59 / +4.5 units** after
the ×100 import scale), firing along **−Z**. The channel between the rails is clear end to end
at every frame of both clips.
