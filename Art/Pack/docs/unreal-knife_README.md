# Butterfly Knife → Unreal Engine

Melee weapon. Same low-poly Tron palette and pivot-rig idiom as the railgun pistol, SMG, and
the Core, so it reuses the identical material setup.

## Files

- `butterfly_knife.glb` — mesh + materials + four clips: **`Idle_Open`**, **`Draw`**,
  **`Stab`**, **`Inspect`**. Get it from the **Export for Unreal (.glb)** button at the
  top-right of the knife viewer. (The stage's own bottom-right OBJ/GLB toolbar exports the mesh
  only — no clips.)
- `knife_stats.json` — the numbers below, machine-readable for a data table.

## Import

1. Drag `butterfly_knife.glb` into the Content Browser.
2. Import dialog: **Skeletal Mesh = off**, **Import Animations = on**, **Import Rig = on**.
   - Tick Skeletal Mesh instead if you want bones — `handle_pivot_safe`, `handle_pivot_bite`
     and `latch_pivot` come through as joints.
3. **Import Uniform Scale = 100** (authored in metres: 0.162 m closed, 0.248 m open,
   0.020 m across the flats),
   **Convert Scene Unit = on**.

## Clips

| Clip | Length | Notes |
|---|---|---|
| `Idle_Open` | **2.40 s** | **loop.** The knife is **carried open** — this is the resting state. Slow bob, breathing glow. |
| `Draw` | **0.52 s** | one-shot, the **only** clip that starts shut. Plays on weapon switch: the balisong comes up folded and flips open. Ends open — hold the last frame. |
| `Stab` | **0.30 s** | one-shot from the open pose. 130 mm thrust with a −0.22 rad pitch, snapping back. |
| `Inspect` | **3.20 s** | one-shot flourish, **starts and ends open**. Six beats: opening flourish → hold → aerial close → reopen → double twirl → settle. Three whole rolls on Z so the pose returns to true. |

### Inspect beats

Hand animation will need to match these — timings in seconds from clip start. The knife starts
and ends this clip open; the aerial beat is the only moment it folds:

| Beat | Window | What the knife does |
|---|---|---|
| 1 | 0.06–0.51 s | opening flip, bite handle leads, ½ roll |
| 2 | 0.51–1.09 s | held open, slow drift |
| 3 | 1.09–1.47 s | aerial: handles fold shut, ¾ roll, rises 50 mm |
| 4 | 1.66–2.05 s | reopening flip, ½ roll |
| 5 | 2.24–2.88 s | double twirl, 1¼ roll |
| 6 | 2.88–3.20 s | settles open, lift returns to zero |

The safe handle trails the bite handle by **0.11 s** throughout — that offset is the catch, and
it's what the off-hand should be timed against.

## Sockets

| Socket | Local position | Use |
|---|---|---|
| tip | **−0.126 m Z** (−12.6 units) | trace start for the stab hit test; slash VFX |
| pivot | **origin** | hand attach point (the pins) |
| pommel | **+0.122 m Z** (+12.2 units) | trailing VFX when open (handle caps at the open pose) |

Blade points along **−Z**; run melee traces from `pivot` to `tip`.

## Emissive

Same two materials: `circuit_cyan` (#25E6FF, the cutting edge, fuller and handle channels) and
`core_amber` (#FF8A1F, the pivot pin and handle end caps). Material Instance with a scalar
**EmissiveIntensity**, driven by state:

| State | circuit_cyan | core_amber |
|---|---|---|
| Closed idle | 0.8–1.1× breathing | 0.7–1.0× breathing |
| Open idle | 1.5–1.9× | 1.6–2.1× |
| Mid-flip | up to 3.6× peak | up to 2.8× peak |
| Stab | up to 4.4× peak | up to 3.0× peak |

The flip spike is what sells the trick — drive it from a Curve Float on the flip clips so the
edge flares as the handles swing and settles as they catch.

## Gameplay notes

The knife is carried open, so `Stab` and `Inspect` can fire at any time. `Draw` is the equip
animation — the only place the closed pose appears. The flip has no hitbox —
it's pure flourish, so leave the melee trace disabled outside `Stab`.
