#!/usr/bin/env python3
# ==============================================================================
# Trace - config-hygiene.py
#
# THE PROBLEM THIS EXISTS FOR, stated exactly, because the obvious fix is wrong.
#
# `git pull` refuses with "Your local changes to the following files would be
# overwritten by merge: Config/DefaultEngine.ini". The file is dirty because the
# UNREAL EDITOR WROTE TO IT, not because anybody edited it. Opening the project
# makes the editor top up DefaultEngine.ini with whatever it thinks is missing:
#
#   * PLATFORM SECTIONS the running machine cares about. A Windows editor appends
#     [/Script/WindowsTargetPlatform.WindowsTargetSettings] with its shader-format
#     array; a Mac editor never does, so the two machines fight forever.
#   * PLUGIN DEFAULT SECTIONS. This repo already carries the scar: the committed
#     [/Script/AndroidFileServerEditor.AndroidFileServerRuntimeSettings] block was
#     injected by a plugin nobody on this project uses.
#   * PER-MACHINE GENERATED VALUES, e.g. that plugin's SecurityToken, which is a
#     random hex string with no business being shared between developers.
#   * DUPLICATE ARRAY ENTRIES: the engine re-appends `+Key=Value` lines it already
#     has, because it matches on the whole line rather than on the key.
#
# WHY .gitignore IS NOT THE ANSWER: DefaultEngine.ini also holds real decisions the
# team shares - the renderer settings, the net driver, the map list. Ignoring the
# file loses those. WHY .gitattributes IS NOT THE ANSWER: a merge driver only runs
# during an actual merge of two branches. The failure here happens BEFORE any merge
# is attempted - git stops because the working tree is dirty - so a merge driver
# never gets a turn.
#
# WHAT THIS DOES INSTEAD. It knows the difference between the two kinds of change
# that land in the same file, and it is that classification, not the rewriting,
# that is the useful part:
#
#   BENIGN      duplicate `+` entries, and edits confined to machine-local keys or
#               editor-injected sections. Safe to throw away before a pull.
#   SUBSTANTIVE anything else - a real settings decision somebody made on purpose.
#               Never discarded silently.
#
# MODES
#   --check            exit 1 if any tracked Config ini has hygiene problems.
#                      This is what the pre-commit hook runs, so noise cannot
#                      enter history in the first place.
#   --fix              rewrite the files, removing duplicate `+` entries.
#   --status           classify the WORKING TREE against HEAD: BENIGN / SUBSTANTIVE
#                      / CLEAN, per file. Exit 0 clean-or-benign, 2 substantive.
#   --discard-benign   revert only the files --status calls BENIGN. This is what
#                      unblocks a pull, and it refuses to touch a substantive one.
#   --adopt            promote whatever the local editor injected into the tracked
#                      file, in canonical de-duplicated form, so that machine's
#                      editor stops re-adding it FOR EVERYONE once committed. Run
#                      once on the machine that keeps dirtying the file - this is
#                      the step that actually ends the problem rather than papering
#                      over it, and it is why this script is not just a linter.
# ==============================================================================

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_DIR = os.path.join(ROOT, "Config")

# Sections known to be written by the editor or a plugin rather than by a person.
# Matched case-insensitively as a prefix of the section name.
INJECTED_SECTION_PREFIXES = (
    "/script/androidfileservereditor.",
    "/script/windowstargetplatform.",
    "/script/mactargetplatform.",
    "/script/linuxtargetplatform.",
    "/script/iostargetplatform.",
    "/script/androidruntimesettings.",
    "/script/hardwaretargeting.",
)

# Keys whose VALUE is generated per machine and is never a team decision.
MACHINE_LOCAL_KEYS = ("securitytoken",)


def section_is_injected(name):
    low = name.strip().lower()
    return any(low.startswith(p) for p in INJECTED_SECTION_PREFIXES)


def key_of(line):
    """'+Foo=Bar' -> 'foo'; returns None for comments, blanks and section heads."""
    s = line.strip()
    if not s or s[0] in (";", "#", "["):
        return None
    body = s[1:] if s[0] in ("+", "-", "!") else s
    return body.split("=", 1)[0].strip().lower() if "=" in body else None


def parse(text):
    """[(section_name, [lines])]. The preamble before any [section] is section ''."""
    out = [("", [])]
    for line in text.splitlines():
        if line.strip().startswith("["):
            out.append((line.strip().strip("[]"), [line]))
        else:
            out[-1][1].append(line)
    return out


def duplicate_array_lines(text):
    """Exact-duplicate `+Key=Value` lines inside one section. (section, line, n)."""
    dupes = []
    for name, lines in parse(text):
        seen = {}
        for line in lines:
            s = line.strip()
            if not s.startswith("+"):
                continue
            seen[s] = seen.get(s, 0) + 1
        for s, n in seen.items():
            if n > 1:
                dupes.append((name, s, n))
    return dupes


def strip_duplicate_arrays(text):
    kept, seen_per_section, current = [], set(), ""
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("["):
            current = stripped
            seen_per_section = set()
            kept.append(line)
            continue
        if stripped.startswith("+"):
            token = (current, stripped)
            if token in seen_per_section:
                continue
            seen_per_section.add(token)
        kept.append(line)
    return "".join(kept)


def tracked_config_files():
    try:
        out = subprocess.run(["git", "ls-files", "Config"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    return [p for p in out.split() if p.lower().endswith(".ini")]


def head_version(relpath):
    r = subprocess.run(["git", "show", "HEAD:" + relpath], cwd=ROOT,
                       capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def classify(relpath):
    """CLEAN / BENIGN / SUBSTANTIVE, plus the substantive lines that decided it."""
    disk_path = os.path.join(ROOT, relpath)
    if not os.path.isfile(disk_path):
        return "SUBSTANTIVE", ["file is missing from the working tree"]
    with open(disk_path, "r", encoding="utf-8", errors="replace") as f:
        now = f.read()
    was = head_version(relpath)
    if was is None:
        return "SUBSTANTIVE", ["not in HEAD"]
    if was == now:
        return "CLEAN", []

    # Compare as SETS OF LINES PER SECTION, because the editor also reorders. A
    # line that exists on both sides is not a change however far it moved.
    def index(text):
        d = {}
        for name, lines in parse(text):
            d.setdefault(name, set()).update(
                l.strip() for l in lines
                if l.strip() and not l.strip().startswith((";", "#", "["))
            )
        return d

    a, b = index(was), index(now)
    substantive = []
    for name in set(a) | set(b):
        added = b.get(name, set()) - a.get(name, set())
        removed = a.get(name, set()) - b.get(name, set())
        if not added and not removed:
            continue
        if section_is_injected(name):
            continue                      # whole section is editor/plugin territory
        for line in sorted(added | removed):
            k = key_of(line)
            if k and k in MACHINE_LOCAL_KEYS:
                continue
            sign = "+" if line in added else "-"
            substantive.append("[{0}] {1}{2}".format(name or "<top>", sign, line))

    return ("SUBSTANTIVE", substantive) if substantive else ("BENIGN", [])


def cmd_check():
    bad = 0
    for rel in tracked_config_files():
        p = os.path.join(ROOT, rel)
        if not os.path.isfile(p):
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            dupes = duplicate_array_lines(f.read())
        for section, line, n in dupes:
            bad += 1
            print("  {0}: [{1}] repeated {2}x: {3}".format(rel, section, n, line[:90]))
    if bad:
        print()
        print("config-hygiene: FAILED - {0} duplicate array entr(ies).".format(bad))
        print("The engine re-appends `+Key=Value` lines it already has. Fix with:")
        print("    python3 Scripts/config-hygiene.py --fix")
        return 1
    print("config-hygiene: clean ({0} config file(s)).".format(len(tracked_config_files())))
    return 0


def cmd_fix():
    changed = []
    for rel in tracked_config_files():
        p = os.path.join(ROOT, rel)
        if not os.path.isfile(p):
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            before = f.read()
        after = strip_duplicate_arrays(before)
        if after != before:
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write(after)
            changed.append(rel)
    print("config-hygiene: rewrote {0} file(s).".format(len(changed)))
    for c in changed:
        print("  " + c)
    return 0


def cmd_status(verbose=True):
    worst = 0
    for rel in tracked_config_files():
        verdict, detail = classify(rel)
        if verbose:
            print("  {0:<45} {1}".format(rel, verdict))
            for d in detail[:12]:
                print("        " + d[:110])
            if len(detail) > 12:
                print("        ... and {0} more".format(len(detail) - 12))
        if verdict == "SUBSTANTIVE":
            worst = 2
    return worst


def cmd_discard_benign():
    reverted, held = [], []
    for rel in tracked_config_files():
        verdict, _ = classify(rel)
        if verdict == "BENIGN":
            subprocess.run(["git", "checkout", "--", rel], cwd=ROOT, check=False)
            reverted.append(rel)
        elif verdict == "SUBSTANTIVE":
            held.append(rel)
    for r in reverted:
        print("  discarded editor noise: " + r)
    for h in held:
        print("  KEPT (real changes, decide yourself): " + h)
    return 2 if held else 0


def cmd_adopt():
    """Canonicalise what the editor injected here so nobody's editor re-adds it."""
    touched = []
    for rel in tracked_config_files():
        p = os.path.join(ROOT, rel)
        if not os.path.isfile(p):
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            before = f.read()
        after = strip_duplicate_arrays(before)
        if after != before:
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write(after)
            touched.append(rel)
    print("config-hygiene --adopt: the editor's injected sections are now IN the")
    print("tracked files, de-duplicated. Commit them and every other machine stops")
    print("being asked to add them:")
    print()
    print("    git add Config && git commit -m 'Adopt editor-injected config sections'")
    print()
    if touched:
        print("De-duplicated on the way through: " + ", ".join(touched))
    return 0


def main(argv):
    if len(argv) != 2 or argv[1] not in (
            "--check", "--fix", "--status", "--discard-benign", "--adopt"):
        print(__doc__ or "")
        print("usage: config-hygiene.py "
              "--check | --fix | --status | --discard-benign | --adopt")
        return 64
    return {
        "--check": cmd_check,
        "--fix": cmd_fix,
        "--status": cmd_status,
        "--discard-benign": cmd_discard_benign,
        "--adopt": cmd_adopt,
    }[argv[1]]()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
