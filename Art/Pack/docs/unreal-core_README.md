# The Core → Unreal Engine

The carry-and-throw objective. Same low-poly Tron palette and pivot-rig idiom as the railgun
pistol and SMG, so it drops into the same material setup.

## Files

- `core.glb` — mesh + materials + three clips: **`Idle`**, **`Pickup`**, **`Throw`**. Get it from
  the **Export for Unreal (.glb)** button at the top-right of the core viewer. (The stage's own
  bottom-right OBJ/GLB toolbar exports the mesh only — no clips.)
- `core_stats.json` — the numbers below, machine-readable for a data table.

## Import

1. Drag `core.glb` into the Content Browser.
2. Import dialog: **Skeletal Mesh = off**, **Import Animations = on**, **Import Rig = on**.
   - Tick Skeletal Mesh instead if you want bones — `shell_pivot_left/right` and
     `ring_pivot_a/b` come through as joints.
3. **Import Uniform Scale = 100** (authored in metres: 0.37 m long × 0.21 m across including
   the cage rings — a touch larger than a real football so it reads at distance),
   **Convert Scene Unit = on**.

## Clips

| Clip | Length | Notes |
|---|---|---|
| `Idle` | **3.6 s** | **loop.** Sitting on the ground: slow tumble, breathing glow, shell shivers a few mm. Rings complete whole turns so it tiles seamlessly. |
| `Pickup` | **0.55 s** | one-shot. Shell cracks open ~30 mm, amber heart flares, halo pops outward. Play on grab, then hold the last frame (or blend to your carry state). |
| `Throw` | **0.50 s** | **loop.** In-flight rifle spin — exactly 4 whole turns on the long axis, so it tiles for any flight time. |

## Animated nodes

| Node | Motion |
|---|---|
| `shell_pivot_left` / `_right` | the two shell halves — crack ±30 mm (±3.0 units) apart on `Pickup`, exposing the heart |
| `ring_pivot_a` / `_b` | the two cage rings — counter-rotate about the long axis on every clip |
| `core` | tumble, rifle spin, and vertical bob |

## Sockets

| Socket | Local position | Use |
|---|---|---|
| nose | **+0.185 m X** (+18.5 units) | forward/aim axis — throw along **+X** |
| rear | **−0.185 m X** (−18.5 units) | trail VFX attach |
| heart | **origin** | point light + score-flare attach; visible once `Pickup` cracks the shell |

## Emissive

Same two materials as the weapons: `circuit_cyan` (#25E6FF) and `core_amber` (#FF8A1F). Make
each a Material Instance with a scalar **EmissiveIntensity** parameter and drive it by state,
not by the clip — the core needs to read differently across the map:

| State | circuit_cyan | core_amber |
|---|---|---|
| Idle (dropped) | 0.85–1.2× breathing | 0.7–1.2× breathing |
| Carried | 1.7–2.2× | 2.4–3.2× (fast pulse, ~2.5 Hz) |
| Thrown | up to 3.4× peak | up to 2.6× peak |

A point light at the `heart` socket tinted #FF8A1F sells the carrier's position to the other
team — worth exposing as a gameplay-tunable radius.

## Gameplay notes

Ball is inert until touched, so drive `Idle` on the world actor and swap to `Pickup` → carry
pose on possession. Team tinting: override `circuit_cyan`'s base colour per team (cyan /
amber-red) and leave `core_amber` alone, so the heart always reads as the objective.
