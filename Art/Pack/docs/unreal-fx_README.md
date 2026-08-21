# Effects — what to rebuild in-engine

The GLBs carry mesh + animation only. Every glow, beam and flash in the original viewers was
driven by material emissive and a few throwaway meshes, none of which survive a glTF export.
This file is the recipe for putting them back, so the assets look in-game the way they did in
the previews.

## Two materials do all the glowing

| Material | Base colour | Where it appears |
|---|---|---|
| `circuit_cyan` | **#25E6FF** | rail channel lights, coil rings, cap banks, spine stripes, grip/cuff lights, knife edge + fuller, core cage + seams, glove knuckle rings |
| `core_amber` | **#FF8A1F** | power cells, knife pivot pin + handle caps, the core's heart and nose caps, the glove palm node |

Make each one a Material Instance with a scalar **EmissiveIntensity** parameter. Everything
below is a multiplier on the base intensity (cyan 1.5, amber 1.4).

## Railgun pistol

- **Charge pulse** — over the 1.05 s that precedes the shot, cyan ramps `1 → 3.9×` with a
  quickening flutter (`0.5 + 0.5·sin(t·(8 + 34k²))`, k = charge progress); amber ramps `1 → 4.4×`.
  `fire_curves.json` in this pack has the whole curve sampled at 60 fps — build a Curve Float
  from it rather than re-deriving.
- **Discharge flash** — on the shot frame, cyan **5.2×**, amber **5.6×**, decaying over 0.75 s.
- **Beam** — a 6-sided tapered cylinder (r 0.030 → 0.013, length ~2.6 m) from the muzzle
  socket along **−Z**, plus a wider translucent halo (r 0.052, double-sided, ~55% opacity).
  Spawn at discharge, hold 0.10 s, fade over the decay.
- **Muzzle flash** — a 6-sided cone (r 0.16, h 0.30) at the muzzle, visible for the first
  0.28 s of decay, scaling 0.55 → 3.2×.

## Railgun SMG

Same beam and flash, scaled to r 0.024 / cone r 0.13. Per shot at 600 rpm:

- cyan `1.8 → 4.8×` spike on the shot frame, back to 1.8 across the 0.1 s cycle
- amber tracks **remaining ammo**, `1.4×` at full → `0.35×` at empty — drive from the ammo
  count, not the clip, so the cell visibly drains

## The Core

- **Idle** (on the ground): cyan and amber both breathe `0.85–1.2×` on a slow ~2 s cycle.
- **Carried**: cyan `1.7–2.2×`, amber `2.4–3.2×` pulsing about 2.5 Hz. Add a point light at
  the heart tinted #FF8A1F — it's what tells the other team who has the objective.
- **Pickup**: amber flares to **4.6×** as the shell cracks, plus a one-shot icosahedron halo
  (r 0.20) expanding 0.6 → 2.1× and fading out over 0.55 s.
- **Thrown**: cyan to **3.4×**; a tapered trail cylinder (r 0.055 → 0.012) streams behind the
  ball, peaking mid-flight. The ball spins about its **long axis** — nose forward, ~10 rev/s.

## Butterfly knife

- Closed idle `0.8–1.1×`, open idle `1.5–1.9×`.
- **Flip / inspect**: cyan spikes to **3.6×** on each of the four catch beats — the flare is
  what sells the trick. Drive it from a Curve Float on the clip.
- **Stab**: cyan **4.4×**, amber **3.0×**; add a short streak plane (0.26 × 0.10 m) at the
  blade tip for the thrust, ~0.9 opacity at peak.

## Gloved hands

Idle `0.95–1.15×`, rising to **2.7×** cyan / **2.1×** amber at the peak of any action. Drive it
from the same curve as the weapon so hands and weapon pulse together.

## Bloom

All of the above assumes a bloom pass. Without one the emissive reads as flat bright colour
and the whole Tron look collapses — set the threshold low enough that the 1.5× base intensity
already blooms slightly, so the 5× discharge really punches.
