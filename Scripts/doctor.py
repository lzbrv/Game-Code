#!/usr/bin/env python3
"""
Trace — "I pulled but I cannot see any changes" doctor.

WHY THIS EXISTS
===============
Three separate times on this project, work was genuinely pushed and genuinely
invisible on another machine, for three DIFFERENT reasons, and each time the
conversation cost a day of "it's pushed" / "I can't see it". The reasons were:

  1. The work was committed but the SHIPPING MAP was not. Every arena fix lives
     in the C++ builder, which fills /Game/Maps/Arena procedurally; the map a
     real match loads is /Game/Maps/Arena_Baked, a frozen snapshot. Pulling the
     code changed nothing visible until the map was re-baked and committed.
  2. A pull was silently REFUSED because local edits would be overwritten. Git
     said so and the message scrolled past. (Scripts/pull.sh exists for this.)
  3. New .cpp/.h files were added, so the compiled module was stale — the editor
     was running yesterday's binary against today's source.

This script checks all of those at once and tells you which one you have. It
changes nothing. Run it from anywhere in the repo:

    python3 Scripts/doctor.py          (macOS / Linux)
    py Scripts\\doctor.py              (Windows)
"""

import os
import subprocess
import sys
import time

def sh(*args, **kw):
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=120,
                              cwd=kw.get("cwd"), check=False).stdout.strip()
    except Exception as exc:  # noqa: BLE001 - a missing tool is a finding, not a crash
        return "!! {0}".format(exc)

ROOT = sh("git", "rev-parse", "--show-toplevel") or os.getcwd()
os.chdir(ROOT)

OK, WARN, BAD = "  ok  ", " WARN ", " !!!! "
findings = []

def say(state, line, fix=None):
    print("[{0}] {1}".format(state, line))
    if fix:
        print("         -> {0}".format(fix))
    if state != OK:
        findings.append((state, line, fix))

print("Trace doctor — repo at {0}\n".format(ROOT))

# ---------------------------------------------------------------- 1. commits --
branch = sh("git", "rev-parse", "--abbrev-ref", "HEAD")
local = sh("git", "rev-parse", "HEAD")
sh("git", "fetch", "-q", "origin")
remote = sh("git", "rev-parse", "origin/{0}".format(branch)) or ""
subject = sh("git", "log", "-1", "--format=%s")

print("branch      {0}".format(branch))
print("local HEAD  {0}  {1}".format(local[:9], subject))
print("origin      {0}\n".format(remote[:9] if remote else "(no upstream)"))

# The trap this check exists for: comparing against origin/<your branch> says
# "level with origin" and looks perfectly healthy while you sit on a branch that
# stopped receiving work. That is exactly what happened here — a collaborator was
# told to check out release/overhaul-2026-08, it was merged into main, work
# continued on main, and their faithful pulls brought nothing for days while every
# check they had reported success. So compare against the DEFAULT branch too.
default_branch = "main"
main_sha = sh("git", "rev-parse", "origin/{0}".format(default_branch))
if main_sha and branch != default_branch:
    gap = sh("git", "rev-list", "--count", "{0}..{1}".format(local, main_sha))
    if gap and gap != "0":
        say(BAD, "You are on '{0}', which is {1} commit(s) behind origin/{2}. A pull on this "
                 "branch succeeds and brings NOTHING, because the work is on {2}."
                 .format(branch, gap, default_branch),
            "git checkout {0}   then   Scripts/pull.sh  (Windows: Scripts\\pull.bat)"
            .format(default_branch))
    else:
        say(WARN, "You are on '{0}', not {1}. It is level with {1} today, but new work lands "
                  "on {1}.".format(branch, default_branch))

if remote and local != remote:
    behind = sh("git", "rev-list", "--count", "{0}..{1}".format(local, remote))
    ahead = sh("git", "rev-list", "--count", "{0}..{1}".format(remote, local))
    if behind and behind != "0":
        say(BAD, "You are {0} commit(s) BEHIND origin/{1}. You do not have the work yet."
                 .format(behind, branch),
            "Scripts/pull.sh   (Windows: Scripts\\pull.bat)  — do NOT use plain 'git pull' here, "
            "it trips over this project's config files")
    if ahead and ahead != "0":
        say(WARN, "You are {0} commit(s) AHEAD of origin (local work not pushed).".format(ahead))
else:
    say(OK, "You are level with origin/{0}.".format(branch))

# --------------------------------------------------- 2. is a pull blocked? ----
dirty = sh("git", "status", "--porcelain")
if dirty:
    n = len(dirty.splitlines())
    say(WARN, "{0} local modification(s). A pull can REFUSE with 'local changes would be "
              "overwritten' and that message is easy to miss.".format(n),
        "Scripts/pull.sh handles this. To see what is dirty: git status")
else:
    say(OK, "Working tree is clean, so nothing can block a pull.")

# ------------------------------------------------------- 3. LFS actually in ---
probe = "Content/Maps/Arena_Baked.umap"
if not os.path.isfile(probe):
    say(BAD, "{0} is missing entirely.".format(probe), "Scripts/pull.sh, then git lfs pull")
else:
    head = open(probe, "rb").read(120)
    if head.startswith(b"version https://git-lfs"):
        say(BAD, "Assets are LFS POINTERS, not real files. Unreal will load an empty/broken map.",
            "git lfs install && git lfs pull")
    else:
        say(OK, "LFS assets are real files ({0:,} bytes for the map).".format(os.path.getsize(probe)))

# ------------------------------------- 4. is the SHIPPING map actually new? ---
ext = "Content/__ExternalActors__/Maps/Arena_Baked"
if os.path.isdir(ext):
    names = {"SideRamp": 0, "Surf_Rail": 0, "Kit_Ramp_03": 0}
    total = 0
    for dp, _, fs in os.walk(ext):
        for f in fs:
            if not f.endswith(".uasset"):
                continue
            total += 1
            blob = open(os.path.join(dp, f), "rb").read()
            for key in names:
                if key.encode() in blob:
                    names[key] += 1
    print("\n         shipping map: {0} actor packages".format(total))
    print("         concave side ramps (SideRamp): {0}".format(names["SideRamp"]))
    print("         surf rails (Surf_Rail):        {0}".format(names["Surf_Rail"]))
    print("         old straight ramps (Kit_Ramp_03): {0}".format(names["Kit_Ramp_03"]))
    if names["SideRamp"] >= 4 and names["Kit_Ramp_03"] == 0:
        say(OK, "The shipping map on disk HAS the new concave side ramps.")
    else:
        say(BAD, "The shipping map on disk does NOT have the new ramps.",
            "You have not actually pulled the map. Scripts/pull.sh")
else:
    say(BAD, "{0} is missing.".format(ext), "Scripts/pull.sh")

# ------------------------------------------------- 5. is the binary stale? ----
newest_src, newest_name = 0, ""
for dp, _, fs in os.walk("Source"):
    for f in fs:
        if f.endswith((".cpp", ".h", ".cs")):
            p = os.path.join(dp, f)
            m = os.path.getmtime(p)
            if m > newest_src:
                newest_src, newest_name = m, p

bins = [
    "Binaries/Mac/libUnrealEditor-Trace.dylib",                       # macOS
    "Binaries/Win64/UnrealEditor-Trace.dll",                          # Windows
]
found = [b for b in bins if os.path.isfile(b)]
print()
if not found:
    say(BAD, "No compiled game module found. The editor is running without your C++.",
        "Windows: Scripts\\build.bat    macOS: Scripts/build.sh")
else:
    b = found[0]
    bt = os.path.getmtime(b)
    age = (newest_src - bt) / 60.0
    print("         newest source  {0}  ({1})".format(
        time.strftime("%Y-%m-%d %H:%M", time.localtime(newest_src)), newest_name))
    print("         compiled module {0}  ({1})".format(
        time.strftime("%Y-%m-%d %H:%M", time.localtime(bt)), b))
    if age > 1:
        say(BAD, "Your compiled module is OLDER than your source by {0:.0f} minutes. "
                 "The editor is running yesterday's code.".format(age),
            "Windows: Scripts\\build.bat    macOS: Scripts/build.sh")
    else:
        say(OK, "Compiled module is newer than the source — no rebuild needed.")

# ----------------------------------------------------------------- verdict ---
print("\n" + "=" * 72)
if not findings:
    print("Everything checks out. If you still see no change in game, say WHICH map you")
    print("opened and what you expected — the surf rails and the concave side ramps are")
    print("on /Game/Maps/Arena_Baked, which is what Scripts/run-listen-server and")
    print("run-practice-range open by default.")
else:
    print("{0} thing(s) to fix, worst first:\n".format(len(findings)))
    for state, line, fix in sorted(findings, key=lambda f: f[0] != BAD):
        print(" *{0} {1}".format(state, line))
        if fix:
            print("    -> {0}".format(fix))
print("=" * 72)
sys.exit(1 if any(s == BAD for s, _, _ in findings) else 0)
