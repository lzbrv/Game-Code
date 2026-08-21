# Gloved Hands → Unreal Engine

A rigged pair of low-poly gloved hands in the same Tron palette as the railgun pistol, SMG,
Core, and butterfly knife — same two emissive materials, so it reuses the identical setup.

## Files

- `gloved_hands.glb` — mesh + materials + 20 clips. Get it from the **Export for Unreal (.glb)**
  button at the top-right of the hands viewer. (The stage's own bottom-right OBJ/GLB toolbar
  exports the mesh only — no clips.)
- `hands_stats.json` — the rig map and clip list, machine-readable.

## Import

1. Drag `gloved_hands.glb` into the Content Browser.
2. Import dialog: **Skeletal Mesh = on**, **Import Animations = on**, **Import Rig = on** — this
   one you *do* want as a skeletal mesh: the 32 joints come through as a bone hierarchy you can
   retarget onto your character's arms.
3. **Import Uniform Scale = 100** (authored in metres — hand 0.19 m from wrist to fingertip,
   0.40 m including the forearm), **Convert Scene Unit = on**.

## Rig

Two mirrored hierarchies, `hand_right` and `hand_left`. Each is:

```
hand_<side>                (root — position + rotation, the only translated node)
├── forearm / cuff meshes
└── wrist_<side>           (wrist break)
    ├── palm meshes
    ├── index_<side>_0 → _1 → _2
    ├── middle_<side>_0 → _1 → _2
    ├── ring_<side>_0 → _1 → _2
    ├── pinky_<side>_0 → _1 → _2
    └── thumb_<side>_0 → _1
```

Finger joints bend about **local X** (negative = curl toward the palm). The `_0` joint of each
finger also carries the **spread** on local Y. The thumb's `_0` carries opposition on Y and Z.

Attach the weapon to a socket on `wrist_right` — every weapon is a right-handed one-hand hold.
The Core is the exception: a two-hand cradle, so parent it to the midpoint or to `wrist_right`
with the left hand IK'd on.

## Loadouts

Weapons are **one-handed** — the right hand carries, the left stays free. Only the Core is a
two-hand hold. Four carry states:

| Loadout | Right hand | Left hand |
|---|---|---|
| `knife` | knife handle | open, free |
| `pistol` | pistol grip | open, free |
| `smg` | SMG grip | open, free |
| `core` | Core cradle | Core cradle |

## Clips

Every clip is authored **per loadout**, because a hand shape is only meaningful against what
it's gripping. **Only combinations that exist in play are baked** — no shoot with the knife, no
stab with a gun, no reload for the knife or Core. Idles loop; actions are one-shots that start
and end on their loadout pose, so they can be additively layered or played straight.

| Loadout | Actions |
|---|---|
| `knife` | draw, stab, inspect, jump, wall jump |
| `pistol` / `smg` | shoot, reload, jump, wall jump |
| `core` | throw, jump, wall jump |

| Clip | Length | Loop | Notes |
|---|---|---|---|
| `Idle_Knife` | 2.40 s | yes | right closed on the handle (knife carried open), left open and free |
| `Idle_Pistol` | 2.40 s | yes | right on the grip, index off-trigger, left open and free |
| `Idle_Smg` | 2.40 s | yes | right on the grip, left open and free |
| `Idle_Core` | 2.40 s | yes | two-hand cradle, palms inward |
| `Draw_Knife` | 0.52 s | no | the wrist flip that snaps the balisong open — pairs with the knife's `Draw`, the only clip where it is shut |
| `Stab_Knife` | 0.30 s | no | 150 mm thrust, fist closes hard — timed to the knife's `Stab` |
| `Inspect_Knife` | 3.20 s | no | follows the knife's `Inspect` beat-for-beat: fingers release and re-grip on each catch, wrist drives the spin, off-hand drifts clear |
| `Shoot_Pistol` | 0.16 s | no | index pull, 30 mm recoil push, wrist break |
| `Shoot_Smg` | 0.16 s | no | same cadence — **loop it at 600 RPM** alongside the SMG's own `Fire` clip |
| `Reload_Pistol` | 0.80 s | no | left hand comes across, strips the cell and seats a new one |
| `Reload_Smg` | 0.80 s | no | same — cell drops 150 mm and returns; timed to the SMG's 0.80 s `Reload` |
| `Throw_Core` | 0.55 s | no | wind-up, release with both hands opening, recover |
| `Jump_{Knife,Pistol,Smg,Core}` | 0.70 s | no | hands drop and open on launch, float up and spread as the body rises |
| `Walljump_{Knife,Pistol,Smg,Core}` | 0.85 s | no | left palm plants flat on the wall, shoves off; right counter-swings |

### Pairing with the weapon clips

| Weapon clip | Hand clip | Alignment |
|---|---|---|
| SMG `Fire` (0.100 s, loop) | `Shoot_Smg` (0.16 s) | play both on trigger-hold; the hand clip overruns one shot by design, so the recoil reads as continuous chatter |
| SMG `Reload` (0.800 s) | `Reload_Smg` | frame-for-frame — the left hand's 0–0.24 s strip matches `mag_pivot` dropping |
| Knife `Stab` (0.30 s) | `Stab_Knife` | frame-for-frame |
| Knife `Draw` (0.52 s) | `Draw_Knife` | frame-for-frame — the wrist snap drives the flip open |
| Knife `Inspect` (3.20 s) | `Inspect_Knife` | frame-for-frame — the hand's release/re-grip flicks land on the knife's four flip beats |
| Core `Pickup` (0.55 s) | `Throw_Core` reversed, or a new grab clip | the Core cracks open on pickup — hands should close as the shell parts |

## Emissive

`circuit_cyan` (#25E6FF — knuckle rings, palm channel, wrist cuff) and `core_amber`
(#FF8A1F — the palm node). Material Instance with a scalar **EmissiveIntensity**:

| State | circuit_cyan | core_amber |
|---|---|---|
| Idle | 0.95–1.15× breathing | 0.9–1.1× breathing |
| Mid-action | up to 2.7× | up to 2.1× |

The action spike is what ties the hands to the weapon's own flare — drive it from the same
Curve Float you use for the weapon so they pulse together.
