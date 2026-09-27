# Editing the game's words

Every word the player reads lives in one file:

```
Config/TraceGameText.ini
```

Open it in any text editor, change the text to the right of the `=`, save, run the game. That is
the whole workflow. Nothing to compile, no editor to open, no asset to re-save.

---

## The file

```ini
[CHARACTER.ROCCO]
NAME                               = ROCCO
ACTIVATED_NAME                     = RIPPLE
ACTIVATED_DESC                     = DASH ON ITS OWN COOLDOWN, LEAVING A RIPPLE FOR 4S.
MOVEMENT                           = A VERY SMALL SECOND JUMP. THE POINT IS THE INSTANT MIDAIR DIRECTION CHANGE.
PASSIVE                            = HEADSHOT KILLS GIVE +3% SPEED FOR 3S.
```

The thing on the **left** of the `=` is the key — it is how the game finds the line, so leave it
alone. Everything on the **right** is yours.

### The four rules

1. **Leave the key alone.** Rename a key and the game stops finding it, and falls back to the
   wording built into the build.

2. **Keep the `{0}` blanks.** Some lines have a slot the game fills in:

   ```ini
   RESPAWN_IN                         = RESPAWN IN {0}
   ```

   `{0}` is where the game puts a number or a name. Reword freely around it, and you may reorder
   slots if the sentence reads better the other way round —

   ```ini
   KILL_LINE                          = {1} WAS KILLED BY {0}
   ```

   — but do not invent a blank the game will not fill, or delete one it will. A line whose blanks
   do not match is ignored and the built-in wording is used instead, with an error in the log
   naming the line.

3. **Write `\n` for a line break.**

4. **Stick to ordinary keyboard characters.** The game draws with a custom typeface that covers the
   normal ASCII set. A curly quote or an em dash still appears, but in a different, plainer typeface
   that will not match the letters around it. `Trace.Text.Verify` tells you if you have used one.

### One thing the file cannot check for you

Some of these lines are **key prompts** — "PRESS ENTER TO LOCK IN", "1 / 2 OR ARROWS", the `[E]`
on an ability. The words are yours to change; the keys they describe are set elsewhere (in the
controls, and in the code). So you can make a prompt say something the keyboard does not do.

Nothing will break — it is the same class of mistake as a typo — but it is the one edit here that
can mislead a player rather than just read differently. If you rewrite a prompt, check the key it
names is still the key that works.

### Removing a line: leave it empty, do not delete it

To take a line off the screen, keep the line and leave nothing after the `=`:

```ini
TAGLINE                            =
```

The game then shows nothing there. Whatever was drawn around the words goes too: a note row on the
options pages, the dark pill behind a banner, a status chip, the title's footer strip. This works on
every line, including one with a `{0}` in it.

**Deleting the line does not remove it.** A line that is not in the file shows the wording built
into the game, and the next regenerate writes that wording back in. That is how twenty lines
deleted in the September text pass all stayed on screen. Those twenty have since been taken out of
the game itself, so they are gone for good. `Trace.Text.Verify` lists every string on screen that
has no line in the file, and `python3 Scripts/dump-game-text.py --check` names them too, so a
deletion that brought a sentence back is easy to spot.

Some lines sit in a fixed layout (the title's tagline, a column heading), and there the spot simply
stays empty; nothing moves up into it. A row's label is the one to leave alone: emptying it leaves
the control on screen with no name.

### Nothing here can be broken beyond repair

Delete a line, delete the whole file, mistype a key — every one of those just means that string goes
back to the wording built into the game. No edit can cause a crash. The only way to get an empty
label is to ask for one, with an empty value.

---

## The three commands

Press `` ` `` in game for the console.

| command | what it does |
|---|---|
| `Trace.Text.Dump` | Writes `Config/TraceGameText.ini` from every string the game has shown. **Keeps every line you have written exactly as you wrote it** (trailing spaces, empty values, even a line the game is refusing) and only adds lines for text that is new. |
| `Trace.Text.Reload` | Re-reads the file without restarting. Edit, alt-tab, reload, look. |
| `Trace.Text.Verify` | Checks the file against the game: lines that were ignored and why, keys that match nothing, strings on screen with no line in the file (a deleted line shows up here), lines you have emptied, and characters the typeface cannot draw. |
| `Trace.Text.SelfTest` | Proves the safety rule (which edits are accepted and which are refused), that an empty value removes a line, and that a dump keeps every line as written. Needs no match. |

Without the game: `python3 Scripts/dump-game-text.py` regenerates the file from the source code, with
the same rules as `Trace.Text.Dump`. With `--check` it changes nothing and exits 1 if the file is out
of date, naming every string that has no line.

### One thing to know about Dump

A string is registered the first time it is **drawn**. So a dump taken at the title screen contains
the title screen and nothing else. To get a complete file: open the menus, the options screen and
the character select, play a round, *then* dump. The command says how many strings it wrote so a
short file cannot be mistaken for a complete one.

---

## Where the file goes in a build you send out

The project's `Config` directory is packaged inside the game, so whatever wording you build with is
what your playtesters see. They cannot edit it, which is the right way round.

If you want to try different wording in an already-packaged build without repackaging, drop a copy
of `TraceGameText.ini` next to the game's `Binaries` folder — inside `Trace.app/Contents/UE/Trace/`
on Mac — and it wins over the packaged one.

---

## For programmers: adding a new string

One line, at the call site:

```cpp
#include "UI/Text/TraceGameText.h"

TraceCanvasText::Draw(HUD, TRACE_TEXT("MENU.PLAY", "PLAY"), X, Y, Style);
```

The second argument is the wording that ships, and it is what the game uses if the document has
nothing for that key. That is why nothing goes blank by accident, and why the document can be
generated rather than maintained by hand.

**The result can be empty.** An empty value in the document (`KEY =`) is how a line is removed, and
`TRACE_TEXT` / `TRACE_TEXTF` then return `""`. If your code draws anything around the words (a pill
behind a banner, a panel, a row in a list, a band), skip it when the string is empty. The options
menu's `AddNote` / `AddHeader`, the HUD's status chips, core and parry banners and ability toast
already do. Use a real spacer (`AddSpacer`), not an empty header, when a layout wants a blank row.

**To remove a string for good**, delete its call site. Emptying its line only hides it, and a
deleted line comes back at the built-in wording.

For a string with a blank in it, use `TRACE_TEXTF` and numbered braces:

```cpp
TRACE_TEXTF("HUD.ABILITY_HOLD_KEY", "HOLD [{0}]", { KeyLabel })
TRACE_TEXTF("HUD.RESPAWN_IN", "RESPAWN IN {0}", { FString::Printf(TEXT("%.0f"), Seconds) })
```

**Not `FString::Printf` with a document string.** UE 5.8's `Printf` takes a `consteval`
`TCheckedFormatString` that only binds to a compile-time array, so a format string read from a file
cannot be passed to it — it does not compile. `TRACE_TEXTF` uses `FString::Format`, which takes a
runtime pointer and, unlike `Printf`, cannot read an argument that was never passed: a slot with no
argument renders as a visible `{1}` rather than dereferencing whatever was on the stack. Keep any
precision or width in the code, in its own `Printf` with a literal format, as above.

Then run `Trace.Text.Dump` to add it to the document.

---

## The ability names and descriptions

**The names are not in this file yet.** JET BOOTS, SUSPEND, RIPPLE and the rest come from the
ability table in the code (`Source/Trace/Abilities/TraceAbilityTypes.cpp`), so renaming one is a code
change for now. Three abilities have no name on purpose, and their cards show the description
instead.

The `ABILITY.MOVEMENT.*`, `ABILITY.PASSIVE.*` and `ABILITY.ACTIVATED.*` lines at the bottom of the
file are left over from an earlier naming pass (HOP, POP, BRACE...). The game no longer reads them,
so editing them changes nothing; `Trace.Text.Verify` lists them as matching nothing.

**The descriptions are here.** A card's text comes from the `CHARACTER.<NAME>.MOVEMENT`, `.PASSIVE`
and `.ACTIVATED_DESC` lines at the bottom of the file. `<NAME>` says which kit the ability came from;
the player never sees it. Ten descriptions have no line (they were taken out so the Demo 35 wording
built into the game would show). `Trace.Text.Verify` lists them once the loadout screen has been
shown, and `Trace.Text.Dump` from that session adds them.
