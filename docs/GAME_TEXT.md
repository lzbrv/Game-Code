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

### Nothing here can be broken beyond repair

Delete a line, delete the whole file, mistype a key — every one of those just means that string goes
back to the wording built into the game. There is no edit that produces a blank label or a crash.

---

## The three commands

Press `` ` `` in game for the console.

| command | what it does |
|---|---|
| `Trace.Text.Dump` | Writes `Config/TraceGameText.ini` from every string the game has shown. **Keeps every edit you have already made** and only adds lines for text that is new. |
| `Trace.Text.Reload` | Re-reads the file without restarting. Edit, alt-tab, reload, look. |
| `Trace.Text.Verify` | Checks the file against the game: lines that were ignored and why, keys that match nothing, characters the typeface cannot draw. |
| `Trace.Text.SelfTest` | Proves the safety rule — which edits are accepted and which are refused. Needs no match. |

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
nothing for that key. That is why nothing can ever be blank, and why the document can be generated
rather than maintained by hand.

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

## The ability names

The loadout rework gave all thirty abilities names of their own, and they live in the same file as
every other word in the game:

```ini
[ABILITY.MOVEMENT]
ROCCO                              = HOP
OYSTER                             = POP

[ABILITY.PASSIVE]
SLIMEBALL                          = PERCH

[ABILITY.ACTIVATED]
ROCCO                              = RIPPLE
```

The key on the left is `<SLOT>.<WHOSE ABILITY IT IS>`. That second half is a leftover from when the
game had characters, and it is deliberately still there: it is how the code identifies which ability
is which, so renaming it would break the link the same way renaming any other key does. The player
never sees it — only the name on the right.

Three activated abilities were renamed away from character names when the characters went away:
CHUD became BRACE, PICKLER became LOB, and SLIMEWALL became SCREEN. The other seven were already
neutral and were left alone.

**The descriptions are not here.** An ability's one-line explanation still comes from the character
roster (`CHARACTER.<NAME>.MOVEMENT`, `.PASSIVE`, `.ACTIVATED_DESC`), which is the same prose the old
character screen showed. Both screens read it, so retuning an ability is still one line to edit.
