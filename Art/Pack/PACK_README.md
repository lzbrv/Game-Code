# Tron Railgun — complete asset pack

Everything in one import: five models, each with its animation clips baked in, plus the
per-asset Unreal notes.

## models/

| File | Clips |
|---|---|
| railgun_pistol.glb | Fire (1.90 s), Reload (0.80 s) |
| railgun_smg.glb | Fire (0.10 s, loop @ 600 rpm), Reload (0.80 s) |
| butterfly_knife.glb | Idle_Open (2.40 s, loop), Draw (0.52 s), Stab (0.30 s), Inspect (3.20 s) |
| core.glb | Idle (3.60 s, loop), Pickup (0.55 s), Throw (0.50 s, loop) |
| gloved_hands.glb | see below |
| gloved_hands.glb | 4 idles + 16 actions, one per valid loadout/action pair |

## Import settings (all files)

- **Import Animations = on**, **Import Rig = on**, **Convert Scene Unit = on**
- **Import Uniform Scale = 100** — everything is authored in metres
- `gloved_hands.glb` is the one to bring in as a **Skeletal Mesh** (32 joints); the weapons
  are rigid-body hierarchies and import fine as static meshes with node animation

## Scale note

The weapons are authored larger than a human hand (pistol ~1.19 m, SMG ~1.25 m, Core 0.37 m
against a 0.19 m hand). The first-person preview scales them to 0.34 m / 0.50 m / 0.30 m to
read correctly. If you want the hand-relative proportion in engine, apply those same factors
on import — or ask and they can be baked into the models.

## Knife state

The knife is **carried open**. `Draw` is the only clip that starts shut — play it on weapon
switch and hold the last frame; every other knife clip begins and ends open.

## Materials

Two emissive materials across every asset: `circuit_cyan` (#25E6FF) and `core_amber`
(#FF8A1F). Make each a Material Instance with a scalar **EmissiveIntensity** and drive it from
gameplay state — the per-asset docs list the intensity ranges.

## Effects — read this one

**`docs/unreal-fx_README.md` is the important file.** glTF cannot carry emissive animation,
beam meshes or muzzle flashes, so none of the glow, beams, flashes, halos or trails from the
previews are inside these GLBs. That file is the full recipe: exact intensity multipliers per
state, beam and flash dimensions, the amber-tracks-ammo rule, and the bloom requirement. Skip
it and the assets will import as grey plastic.

`docs/unreal_fire_curves.json` has the pistol's charge/discharge emissive curve sampled at
60 fps, ready to become a Curve Float.

## docs/

Per-asset Unreal notes: socket positions, node maps, clip pairings, weapon stats
(SMG: 600 rpm, 40 rounds, 0.80 s reload, 33/18/12 damage).
