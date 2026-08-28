# What changed in the migration, and what you need to do differently

Steps 1–6 turned Trace from "a game built entirely by code at runtime" into "a normally structured
Unreal project, with the code path kept as a live fallback".

**Nothing about playing the game changed.** Same keys, same numbers, same feel. If you notice a
difference in how the game *plays*, that is a bug — report it, because this pass was explicitly a
reorganisation and not a redesign.

Read §1 and §2 before you next open the editor. The rest is reference.

---

## 1. The first thing you will hit: assets are read-only until you lock them

Everything under `Content/` is now checked out **read-only**, and Unreal will refuse to save it.
That is deliberate.

A `.uasset` is a binary blob. Git cannot merge two people's edits to one — it can only keep one and
throw the other away, silently. So the rule is **lock before you edit**:

```bash
git pull                     # always first
Scripts/lock.sh <thing>      # take the lock
# ...edit in the editor, save...
git commit -am "..."
git push                     # PUSH BEFORE YOU UNLOCK
Scripts/unlock.sh <thing>    # release it
```

**Set this up once, and everyone should:** in the editor, Revision Control (bottom-right) → Connect
to Revision Control → Provider: **Git**, then tick **Use Git LFS 2 File Locks workflow**. After
that, checking out an asset takes the lock automatically, and anything someone else holds shows a
badge. It is much easier than the command line and it is what you should teach a designer.

If Unreal says it cannot save an asset, you have not lost work — you just do not hold the lock. Take
the lock, then save. **Do not `chmod` the file**; that defeats the whole mechanism and will lose
somebody's work eventually.

**The payoff nobody on the team knows about yet:** two of you *can* edit the arena at the same time.
`Arena_Baked` is not one big file — it is a 6.6 KB level plus **one separate file per actor**, 572
of them. You lock the *actor*, not the map. One person can move `Cover_37` while another retints
`Wall_North_01`.

**If someone goes on holiday still holding a lock:** `git lfs unlock --force <path>`. Agree between
yourselves who is allowed to do that.

---

## 2. Where each number lives — the one that will bite you

**The asset holds words and colours. Project Settings holds every number the game obeys.**

| You want to change | Where |
|---|---|
| A character's name, accent colour, or the text on its select card | `Content/Trace/Data/Characters/DA_Character_*` |
| Any number the game actually obeys — cooldowns, damage, speeds, ranges | **Edit → Project Settings → Game → Trace** |
| The look of a surface or a neon line | `Content/Trace/Materials/Instances/MI_*` |
| Where the Core starts | the `Core_Spawn` actor in `Arena_Baked` |
| Your own key bindings | the in-game Options menu (**not** `IMC_Trace` — see §4) |

**The exception, and it is genuinely confusing:** an ability's cooldown appears in *two* places. The
data asset's `Cooldown Seconds` is only what the select card prints and what the HUD ring draws.
What the **E** key actually waits for is the value in Project Settings. If the card and the game
disagree, Project Settings is the truth. Change both, or change Project Settings and ignore the card.

---

## 3. What you can now open and edit

| Folder | What it is |
|---|---|
| `Content/Maps/Arena_Baked` | **The arena the game now ships.** 573 real actors you can select and drag |
| `Content/Maps/Arena` | The same arena built from code. Empty in the editor. Kept as the control |
| `Content/Trace/Materials/Parents` | The two hand-authored shader graphs |
| `Content/Trace/Materials/Instances` | **The 64 you actually want.** Colour, Glow, Tint, GridScale… |
| `Content/Trace/Data/Characters` | The five character definition assets |
| `Content/Trace/Input` | 14 Input Actions + `IMC_Trace` |
| `Content/Trace/UI/Menu`, `UI/HUD` | The converted UMG widgets |

**Two rules for all of it:**

1. **Do not hand-edit a generated asset and expect it to survive.** The generator scripts
   (`Scripts/generate-*.py`) delete and rewrite their assets from scratch every run. If you want a
   change to stick permanently, it goes in the script or in the C++ table, not in the asset.
   Materials instances and the baked map's actors are safe to edit by hand; the data, input and UI
   assets are regenerated.
2. **Do not rename or move these assets.** The game finds them by exact name in an exact folder.
   Rename one and the game quietly reverts to the built-in values, still plays perfectly, and writes
   one line in the log saying which file it could not find.

The character assets are **all-five-or-none**: if one is missing or renamed, the game ignores the
other four too and uses the C++ values for everybody. That is deliberate — a half-data roster would
be miserable to debug — but it means one accidental rename silently reverts all five.

---

## 4. Two things that look like they should work and do not

**Editing keys inside `IMC_Trace` does nothing.** It looks like the place to change what W does. It
is not. Every time you spawn, the game wipes that asset's key list and rewrites it from your own
saved binds. Change keys in the **Options menu**.

**The arena's terraced pieces are not individually clickable.** 659 of 835 instanced pieces are
grouped: each *group* is one labelled actor you can select and move as a unit, but you cannot grab
tile 12 of a bank terrace on its own. That is a deliberate performance trade — exploding every block
into its own component would hand back the draw-call cost that instancing exists to avoid.

---

## 5. Switches, and how to undo any of this

| Switch | Effect |
|---|---|
| `Trace.UI.UseUMG 1` | Turn the new UMG UI on (**off by default** — see §6) |
| `Trace.UI.HUD.UseUMG 0\|1\|-1` | Override just the HUD corner; −1 follows the master |
| `-TraceNoInputAssets` | Build input in code, as before |
| `-TraceNoMenuUMG` | Force the Canvas title screen |
| `Trace.Input.VerifyAssets` | Says in plain English whether assets or C++ are live |
| `Trace.HUD.Corner.Verify` | Says which corner renderer is drawing |

**To go back to the procedural arena**, `Config/DefaultEngine.ini` names the map; `docs/EDITOR.md`
lists all four places a default arena appears, which you need if you want a clean revert.

**Every asset system falls back.** This was tested by deleting all four new asset folders and playing
a match: 20 kills, zero crashes, and the log announced each fallback by name. You cannot brick the
game by deleting an asset — worst case it reverts to exactly what it did before this migration.

---

## 6. Known issues — what is NOT done

- **~~The UMG UI is off by default~~ — NO LONGER TRUE (2026-08-28).** UMG is now the shipped
  default: `Trace.UI.UseUMG` registers with a default of 1 (`Source/Trace/UI/TraceMenuHUD.cpp:217`),
  both blockers this bullet named (the launch stall and the mis-stroked wordmark) are closed, and the
  UMG corner was observed active at boot with no flag set. One real caveat survives: a second, dead
  default of 0 still exists in `Source/Trace/UI/TraceHUD.cpp:4355`, so the answer technically depends
  on a static-init race — see `KNOWN_LIMITATIONS.md` item 25.
- **Nothing has been cooked or packaged, and this is now measured rather than assumed.** The game
  runs from the editor binary. There is no `Saved/Cooked` directory anywhere in the project, and the
  Shipping binary was run for the first time on 2026-08-28 and does not start on this install layout.
  Whether the 74 material assets and 647 actor packages cook cleanly is still untested. See
  `KNOWN_LIMITATIONS.md` item 29 for the reproduction.
- **Windows is unverified for this pass.** macOS structurally cannot catch the MSVC shadowing errors
  that have broken your build four times — a Windows build is the only real gate.
- The LFS repo is now **~70 MB across ~1,650 files** (measured 2026-08-28 on the release branch;
  it was ~30 MB across ~1,427 files at commit `36401e2`); a clone costs ~70 MB. GitHub's included quota
  is 10 GiB storage and 10 GiB/month bandwidth, so you are far from trouble, but it is no longer
  free-forever the way an asset-free repo was.
