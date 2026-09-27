#!/usr/bin/env python3
"""
Trace — write the complete game text document WITHOUT running the game.

WHY THIS EXISTS ALONGSIDE Trace.Text.Dump
=========================================
The in-game command is authoritative about what actually reached the screen, which is exactly what
you want when you are asking "is this string really used". It has one weakness, and it is structural
rather than a bug: a key registers the first time it is DRAWN, so a dump taken at the title screen
contains the title screen and nothing else. Getting a complete document that way means walking every
menu, every options page, the character select and a whole round — and quietly missing the death
panel because nobody died.

This reads the SOURCE instead. Every call site names its key and its default in the same expression:

    TRACE_TEXT("MENU.PLAY", "PLAY")
    TRACE_TEXTF("HUD.RESPAWN_IN", "RESPAWN IN {0}", { ... })

so the complete set is a fact about the code, available without a running game. That is what makes
the document complete BY CONSTRUCTION rather than by remembering to visit a screen.

The two are not redundant and neither replaces the other:

    this script        every string the code CAN show.        Complete, no game needed.
    Trace.Text.Dump    every string the session DID show.     Proves a key is really reachable.

IT NEVER LOSES AN EDIT. The existing document is read first and its wording wins for every key it
still contains; only genuinely new keys arrive at their compiled-in default. Keys the code no longer
has are kept in a clearly labelled section at the end rather than deleted, because throwing away
somebody's rewritten sentence is the one unrecoverable thing a generator can do.

"WINS" MEANS BYTE FOR BYTE. A line already in the document is written back exactly as it was typed:
a trailing space stays, an empty value stays empty (that is how a line is REMOVED from the screen),
and a line the game refuses because its {0} blanks do not match stays too, with a warning - it used
to be replaced with the built-in wording, which quietly threw the owner's sentence away.

WHAT A DELETED LINE MEANS. Deleting a line does not remove its text; the game falls back to the
wording built into the code, and this script writes that wording back. --check says so by name, so
a deletion that brought a sentence back to the screen cannot go unnoticed. To remove a line, keep it
and leave nothing after the "=".

    python3 Scripts/dump-game-text.py            # rewrite Config/TraceGameText.ini in place
    python3 Scripts/dump-game-text.py --check    # change nothing; exit 1 if it is out of date
    python3 Scripts/dump-game-text.py -o X.ini   # write a copy elsewhere, still keeping the edits
                                                 # in Config/TraceGameText.ini
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True,
                      check=False).stdout.strip() or os.getcwd()
SOURCE = os.path.join(ROOT, "Source")
DOCUMENT = os.path.join(ROOT, "Config", "TraceGameText.ini")

# TRACE_TEXT("KEY", "DEFAULT")  /  TRACE_TEXTF("KEY", "DEFAULT", {...})
#
# BARE LITERALS, NOT TEXT(...) — the macro is what adds the TEXT() wrapping, so the call site reads
# TRACE_TEXT("MENU.PLAY", "PLAY"). The first version of this pattern expected TEXT() here and
# therefore matched nothing at all, on a tree that had ninety call sites in it. A scanner that
# silently finds zero is worse than one that crashes, which is why main() treats an empty result as
# an error rather than writing an empty document over the owner's file.
#
# The default may be split across lines the way C++ concatenates adjacent literals:
#     TRACE_TEXT("K", "FIRST HALF "
#                     "SECOND HALF")
# so the value is captured as a run of quoted chunks and joined, which is what the compiler does.
# TraceGameText::Get(SomeVariable, SomeDefault) — a key that is NOT a literal, because it comes out
# of a data table row. Legitimate (TraceUserSettings' colour palette does it, so one table column
# carries the key beside the wording) but INVISIBLE TO THIS SCANNER, which can only read literals.
# Counted and reported so the incompleteness is stated rather than silent: those keys appear in the
# document only after Trace.Text.Dump has seen them drawn.
DYNAMIC = re.compile(r'TraceGameText::Get\s*\(\s*(?!TEXT\s*\(\s*")')

CALL = re.compile(
    r'TRACE_TEXTF?\s*\(\s*'
    r'"((?:[^"\\]|\\.)*)"\s*,\s*'                     # the key
    r'((?:"(?:[^"\\]|\\.)*"\s*)+)',                    # the default, one or more chunks
    re.S)

CHUNK = re.compile(r'"((?:[^"\\]|\\.)*)"')

# Block and line comments, so the EXAMPLES in a doc comment are not mistaken for call sites. This is
# not hypothetical: TraceGameText.h's own header explains the macro with
#     TRACE_TEXTF("HUD.RESPAWN_IN", "RESPAWN IN {0}", ...)
# and the first version of this script duly wrote HUD.RESPAWN_IN into the owner's document as though
# the game drew it. A generated document that lists strings the game cannot show is worse than one
# that is merely incomplete — the owner edits a line and nothing happens, with no way to find out why.
#
# Deliberately crude: it does not understand a // inside a string literal. That would matter if any
# call site had one, and none does; the cost of being wrong is a missed key, which --check reports.
COMMENTS = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)


def strip_comments(src):
    # Replace with newlines rather than nothing so that line-splitting behaviour elsewhere is unchanged.
    return COMMENTS.sub(lambda m: "\n" * m.group(0).count("\n"), src)


def unescape_cpp(text):
    """The few escapes that appear in these literals. \\n stays as the two characters, because the
    document's own escape for a line break is also \\n — a round trip must not turn one into the
    other."""
    return text.replace('\\"', '"').replace("\\\\", "\\")


def scan():
    """(key -> default) for every call site, plus the file each was found in, plus a count of the
    table-driven calls this scanner cannot see."""
    found = {}
    where = {}
    dynamic = []
    for base, dirs, files in os.walk(SOURCE):
        dirs[:] = [d for d in dirs if d not in (".git",)]
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(base, name)
            with open(path, encoding="utf-8", errors="replace") as handle:
                src = handle.read()
            if "TRACE_TEXT" not in src:
                continue
            src = strip_comments(src)
            for match in CALL.finditer(src):
                key = unescape_cpp(match.group(1)).strip().upper()
                default = "".join(unescape_cpp(c) for c in CHUNK.findall(match.group(2)))
                if not key:
                    continue
                if key in found and found[key] != default:
                    print(f"  !! {key} has two different defaults:\n"
                          f"       {where[key]}: {found[key]!r}\n"
                          f"       {os.path.relpath(path, ROOT)}: {default!r}",
                          file=sys.stderr)
                found[key] = default
                where.setdefault(key, os.path.relpath(path, ROOT))

            # The module's own header declares `Get(const TCHAR* Key, ...)`, which matches the
            # dynamic pattern and is not a call site. Skipping the module itself is the whole fix.
            n = 0 if name.startswith("TraceGameText") else len(DYNAMIC.findall(src))
            if n:
                dynamic.append((os.path.relpath(path, ROOT), n))
    return found, where, dynamic


def read_existing(path):
    """key -> value AS TYPED from a document, ignoring comments. Section headers prefix their keys.

    AS TYPED: only the spaces between the "=" and the first character are dropped. The game trims
    both ends when it READS a value (a trailing space never reaches the screen), but this is the
    copy that gets WRITTEN BACK, and a generator that "tidied" the owner's lines would turn every
    regenerate into a diff of lines nobody changed - BANNER_PARRIED ends in a space, and that space
    is his. Same rule as the game's own writer (GDocumentAsTyped in TraceGameText.cpp)."""
    values = {}
    if not os.path.isfile(path):
        return values
    section = ""
    with open(path, encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            raw = raw.rstrip("\r\n")
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(";"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                continue
            if "=" not in line:
                continue
            name, _, _ = line.partition("=")
            name = name.strip()
            if not name:
                continue
            value = raw.partition("=")[2].lstrip(" \t")
            full = f"{section}.{name}" if section else name
            values[full.upper()] = value
    return values


SLOT = re.compile(r"\{(\d+)\}")


def slots(text):
    return sorted(set(int(n) for n in SLOT.findall(text)))


HEADER = """\
# =============================================================================
#  TRACE - GAME TEXT
#
#  Every word the player reads is in this file. Edit the text on the RIGHT of the
#  "=", save, and run the game. That is the whole workflow - nothing to compile.
#
#  THE FOUR RULES
#    1. Leave the KEY (left of the "=") alone. It is how the game finds the line.
#    2. Keep every {0}, {1} ... exactly as they are. They are the blanks the game
#       fills in with a name or a number. Reword freely AROUND them, and you may
#       put them in a different order if the sentence reads better - but do not
#       add one the game will not fill or delete one it will. A line whose blanks
#       do not match is ignored, and the built-in wording is used instead.
#    3. Write \\n where you want a line break.
#    4. Stick to ordinary keyboard characters. Run Trace.Text.Verify to check.
#
#  TO REMOVE A LINE FROM THE SCREEN, leave the right side empty:
#       TAGLINE                            =
#  The game then shows nothing in its place.
#
#  DELETING a line does NOT remove it. It brings back the wording built into the
#  game, and the next regenerate writes that wording back into this file.
#
#  Regenerate with:  python3 Scripts/dump-game-text.py
#  It keeps every line you have written exactly as you wrote it and only adds
#  lines for text that is new.
# =============================================================================
"""


def compose(found, existing):
    """The document text, and the counts for the report."""
    kept = 0
    fresh = 0

    merged = {}
    for key, default in found.items():
        if key in existing:
            candidate = existing[key]
            # KEPT AS TYPED, WHATEVER IT SAYS. The game applies the blanks rule when it reads the file
            # (and an empty value is always allowed - it removes the line); a line it refuses is named
            # by Trace.Text.Verify and in the log. What this script must not do is decide for the
            # owner: it used to swap a refused line for the built-in wording, which deleted his
            # sentence to fix a typo in it. So it warns and keeps the line.
            shown = candidate.strip()
            if shown and slots(shown) != slots(default):
                print(f"  !! {key}: the game will IGNORE this line until its blanks match - it has "
                      f"{slots(shown)}, the code fills {slots(default)}. Kept as you wrote it.",
                      file=sys.stderr)
            merged[key] = candidate
            kept += 1
            continue
        merged[key] = default
        fresh += 1

    orphans = sorted(k for k in existing if k not in found)

    lines = [HEADER]
    section = None
    for key in sorted(merged):
        head, _, name = key.rpartition(".")
        if head != section:
            section = head
            lines.append(f"\n[{section}]\n" if section else "\n")
        lines.append(f"{name:<34} = {merged[key]}\n")

    if orphans:
        lines.append("\n\n# =============================================================================\n")
        lines.append("#  NOT FOUND BY THE SOURCE SCANNER\n")
        lines.append("#\n")
        lines.append("#  These lines still WORK - the game will use them. They are down here because\n")
        lines.append("#  this script could not find them in the code, which means one of two things:\n")
        lines.append("#\n")
        lines.append("#    * the key is built from a data table rather than written out in full, which\n")
        lines.append("#      is how the character cards work (CHARACTER.<NAME>.<FIELD>), or\n")
        lines.append("#    * the key was renamed and this really is a leftover.\n")
        lines.append("#\n")
        lines.append("#  Run Trace.Text.Verify in game to tell the two apart: a key the game asked for\n")
        lines.append("#  is not reported as unmatched. Edit them here exactly as you would above.\n")
        lines.append("# =============================================================================\n")
        # A BARE [] RESETS THE SECTION, and it has to. These are FULL keys; written under a named
        # section the loader would prefix them with it and they would stop matching - i.e. parking a
        # line here would silently break the very string it was trying to preserve. That is what the
        # first version of this did to all fifty character-card keys.
        lines.append("\n[]\n")
        for key in orphans:
            lines.append(f"{key:<34} = {existing[key]}\n")

    return "".join(lines), kept, fresh, len(orphans)


def missing_from_document(found, existing):
    """Keys the code can show that have no line in the document - sorted.

    Every one of these is on screen at its BUILT-IN wording. That is right for a label added since
    the last regenerate, and wrong for a line somebody deleted meaning to remove it, which is the
    mistake this list exists to catch: deleting a line never removed anything."""
    return sorted(k for k, default in found.items() if k not in existing and default != "")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="change nothing; exit 1 if the document is out of date")
    parser.add_argument("-o", "--output", default=DOCUMENT, help="where to write")
    parser.add_argument("-i", "--input", default=DOCUMENT,
                        help="the document whose edits are kept (default: Config/TraceGameText.ini)")
    args = parser.parse_args()

    found, _, dynamic = scan()
    if not found:
        print("No TRACE_TEXT call sites found under Source/. Nothing to write.", file=sys.stderr)
        return 1

    # THE EDITS COME FROM THE DOCUMENT, NOT FROM WHEREVER THE OUTPUT GOES. This read args.output,
    # so `-o preview.ini` started from an empty file and wrote a "preview" with every one of the
    # owner's edits reverted to the built-in wording - a copy that looked authoritative and was not.
    existing = read_existing(args.input)
    text, kept, fresh, orphans = compose(found, existing)

    if args.check:
        current = ""
        if os.path.isfile(args.output):
            with open(args.output, encoding="utf-8", errors="replace") as handle:
                current = handle.read()
        if current == text:
            print(f"up to date: {len(found)} string(s)")
            return 0
        print(f"OUT OF DATE: {len(found)} string(s) in the code, {len(existing)} in the document. "
              f"Run: python3 Scripts/dump-game-text.py", file=sys.stderr)
        missing = missing_from_document(found, existing)
        if missing:
            print(f"  {len(missing)} string(s) the game can show have NO line in the document, so the "
                  f"game shows its built-in wording for them. A line that was DELETED to remove it "
                  f"comes back like this - to remove a line, keep it and leave nothing after the "
                  f"\"=\":", file=sys.stderr)
            for key in missing:
                print(f"    {key:<44} {found[key]!r}", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as handle:
        handle.write(text)

    print(f"wrote {len(found)} string(s) to {os.path.relpath(args.output, ROOT)}")
    if dynamic:
        total = sum(n for _, n in dynamic)
        print(f"  NOTE: {total} call(s) in {len(dynamic)} file(s) build their key from a data table "
              f"rather than a literal, so this scanner cannot see them:")
        for path, n in sorted(dynamic):
            print(f"    {n:3}  {path}")
        print("  They are not missing from the game - only from THIS file. Run Trace.Text.Dump in a "
              "session that has shown those screens to add them.")
    print(f"  {kept} kept your existing wording, {fresh} at the wording built into the game"
          + (f", {orphans} no longer used and parked at the end" if orphans else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
