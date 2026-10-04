# =============================================================================
# Trace - generate_sound_page.py      (Demo 29 item 10; reworked after Demo 35)
#
#   Demo 29: "a list of all the sounds in the game with playable samples next to
#             each action, so that I can go through and test every single one
#             added and replace specific ones"
#   later:   "make sure the sound testing/switching html is up to date and
#             functional"
#
# Renders ONE self-contained HTML page - no server, no network, no assets beside
# it - with a play button for every sound the game can make. Per row:
#
#   A   the exact WAV the importer turns into the game's asset, with its length
#       and level;
#   B   "Try a file..." - any WAV from your disk, auditioned against A with the
#       same controls, its length and level beside A's, warnings where it breaks
#       the project's format;
#   a "replace" mark and a note.
#
# Marks, notes and tried files survive a reload (localStorage + IndexedDB, in
# that browser only). "Export list" writes the marked rows out with the exact
# file to overwrite, the candidate's file name, and the commands that do it -
# once for a Mac (Scripts/*.sh, cp, afconvert) and once for Windows (plain
# cmd.exe: Scripts\*.bat, copy /Y, and for a file that needs converting an
# ffmpeg line and the Audacity settings as rem notes, since afconvert is
# Mac-only). The how-to on the page shows both.
#
#   python3 Scripts/generate_sound_page.py           -> Art/Sounds/sound-test.html
#   python3 Scripts/generate_sound_page.py --check   validate only, write nothing
#
# EXIT 1 ON ANY ERROR, and nothing is written (--allow-errors writes the page
# anyway, with the errors printed at its top). Every check below is an error
# rather than a warning because each one is a sound the owner would look for on
# the page and either not find or find described wrongly.
#
# Re-run it whenever a WAV, an event, a trigger or an ability name changes; the
# page is a build product and nothing reads it back.
#
# -----------------------------------------------------------------------------
# WHERE EVERY COLUMN COMES FROM, SO NONE OF IT CAN GO STALE SILENTLY
# -----------------------------------------------------------------------------
#   event name / side / trigger  Source/Trace/Audio/TraceSoundEvents.cpp, the
#                                Table[] literal, parsed. A row with no WAV, and
#                                a WAV with no row, are ERRORS.
#   the WAV                      Art/Sounds/**.wav, keyed by STEM exactly as
#                                Scripts/import_sounds.py keys the bank.
#   duration / peak / RMS        measured from the WAV, here, now.
#   call sites                   grepped out of Source/ - every place gameplay
#                                code names the event, minus harnesses (verify /
#                                test / debug scopes and play-count lookups). An
#                                event nothing plays is printed as NOT WIRED.
#   ability names and slots      GAbilityTable in
#                                Source/Trace/Abilities/TraceAbilityTypes.cpp,
#                                parsed. A rename there flows through on the next
#                                run; no display name is spelled in this file.
#   which ability plays a sound  ABILITY_SOUNDS below - a reading of the code,
#                                written down as the trigger line plus every
#                                route to it from an ability: a real IsAbility /
#                                IsSlot / IsKitIn check, a kit's ActivateAbility,
#                                or a hook with NO check (then every ability of
#                                that kit plays it, and the row says so). Each
#                                link is re-found on every run, the listed
#                                abilities must be exactly what the routes
#                                reach, and any caller of a walked function that
#                                no route accounts for is an ERROR - a second
#                                route nobody has read.
#   SILENT                       TraceSoundEvents::Unwired() (Demo 29 items 9
#                                and 11), parsed with its reasons.
#   RETIRED                      ABILITY_SOUNDS' 'retired' entries: a sound only
#                                a Demo 35-retired code path plays, with the
#                                cvar that brings the path back. The cvar must
#                                exist in Source/ or generation fails.
#
# An "ability-looking" sound - its WAV is under Art/Sounds/Abilities/, its stem
# starts with a kit name (TraceCharacterIdToString), or every one of its call
# sites is under Source/Trace/Abilities/ - with no ABILITY_SOUNDS entry is an
# ERROR. So is an unmapped trigger site for a mapped sound.
#
# TWO ROUTES WITH NO ABILITY CHECK, AS OF THIS WRITING (owner's call whether
# they are bugs): X's bee sweep (TickAbilities -> SweepBeeContacts) runs for any
# X pick, so LEECH or STING alone still stings with orbiting bees; and the kit's
# dash poll (TickAbilities -> PollDashForBash -> TryBash) knocks players for any
# pick of that kit, so CUSTOM STEEL or CHUD alone still bashes. Both rows carry
# the extra abilities and a note. Add the check in C++ and this script FAILS
# until the entry says G(...) - by design.
#
# NO CHARACTER NAMES IN WHAT THE PAGE SAYS. Abilities are freestanding: the page
# names them by GAbilityTable's Name column and groups by ability and slot.
# Character names survive only inside file paths (the path is the thing you
# overwrite), and in the Export list's event names (the import command takes
# them). Everything a person reads or hears on the finished page - headings,
# notes, rows, screen-reader labels - is checked against the kit names, event
# identifiers like RoccoJump included, before the page is written.
#
# -----------------------------------------------------------------------------
# THE TWO MUSIC BEDS ARE PREVIEWS, AND THE PAGE SAYS SO ON THE ROW
# -----------------------------------------------------------------------------
# MusicTitle is 64 s / 11.3 MB and AmbienceMatch is 48 s / 8.5 MB. Embedded
# whole they would be 26 MB of base64 on their own. Each is embedded as the
# first PREVIEW_SECONDS, decimated to 22.05 kHz (two-tap average first, so the
# decimation is not raw aliasing). The row carries the full duration, the full
# path and the word PREVIEW; the shipping file is untouched. (--no-preview
# embeds them whole.)
# =============================================================================
import argparse
import array
import base64
import datetime
import glob
import html
from html.parser import HTMLParser
import io
import json
import math
import os
import re
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOUND_DIR = os.path.join(ROOT, "Art", "Sounds")
EVENTS_CPP = os.path.join(ROOT, "Source", "Trace", "Audio", "TraceSoundEvents.cpp")
ABILITY_TYPES_CPP = os.path.join(ROOT, "Source", "Trace", "Abilities", "TraceAbilityTypes.cpp")
GENERATOR_PY = os.path.join(HERE, "generate_sounds.py")
SOURCE_DIR = os.path.join(ROOT, "Source")
DEFAULT_OUT = os.path.join(SOUND_DIR, "sound-test.html")

# Stems embedded as a short preview instead of whole. See the header.
PREVIEW_STEMS = {"MusicTitle", "AmbienceMatch"}
PREVIEW_SECONDS = 12.0
PREVIEW_DECIMATE = 2          # 44100 -> 22050

# Bins in the little level envelope drawn on each row (and on a tried file).
ENVELOPE_BINS = 64
ENVELOPE_FLOOR_DB = -60.0

# Files that are ABOUT the audio system rather than triggers of it: the table, the play API itself,
# and the console verifiers. Any file whose name says Verify/Test/Integ is skipped as well (below).
# Audio/TraceAudioWatch.cpp is deliberately NOT here: it is the runtime driver that plays every
# footstep and every observed gunshot. Its console commands are filtered by scope like any harness.
CALLSITE_EXCLUDE = (
    "Source/Trace/Audio/TraceSoundEvents.",
    "Source/Trace/Audio/TraceAudio.cpp",
    "Source/Trace/Audio/TraceAudio.h",
    "Source/Trace/Audio/TraceAudioVerify.",
    "Source/Trace/Audio/TraceAudioLoudness.",
    "Source/Trace/Audio/TraceMusicPlayer.h",
)
HARNESS_FILE_RE = re.compile(r"(Verify|Test|Integ|Harness)[^/]*\.(cpp|h)$")
# A scope (namespace / function / console command) whose name says it is a harness.
HARNESS_SCOPE_RE = re.compile(r"(Verify|Test|Harness|Debug|Integ|Probe|Audit|Cmd|Command)")
# A line that only LOOKS UP a play count or a property of an event is not a trigger.
LOOKUP_ONLY_RE = re.compile(r"(PlaysOf|AudioPlays|GetPlaysByEvent|IsBigWorldEvent|CountBursts|Plays\.Find)\s*\(")

_errors = []
_warnings = []


def error(msg):
    _errors.append(msg)
    print("  ERROR: " + msg)


def warn(msg):
    _warnings.append(msg)
    print("  warning: " + msg)


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


# ---------------------------------------------------------------------------
# 1. the event table, parsed out of the C++ that IS the authority
# ---------------------------------------------------------------------------

ROW_RE = re.compile(
    r"\{\s*(?:FName\(TEXT\(\"(?P<qname>[A-Za-z0-9_]+)\"\)\)|(?P<name>[A-Za-z0-9_]+))\s*,"
    r"\s*ETraceSoundSide::(?P<side>Client|World)\s*,"
    r"\s*TEXT\(\"(?P<trigger>(?:[^\"\\]|\\.)*)\"\)"
    r"(?:\s*,\s*ETraceSoundFamily::(?P<family>[A-Za-z]+))?\s*\}",
    re.S)


def parse_event_table():
    """[(name, side, trigger, family)] in the table's own order."""
    text = read_text(EVENTS_CPP)
    start = text.find("static const FTraceSoundEvent Table[] =")
    if start < 0:
        raise SystemExit("could not find the FTraceSoundEvent table in " + EVENTS_CPP)
    end = text.find("return TConstArrayView<FTraceSoundEvent>", start)
    body = text[start:end if end > 0 else len(text)]

    rows = []
    for m in ROW_RE.finditer(body):
        name = m.group("qname") or m.group("name")
        rows.append({
            "name": name,
            "side": "game-side" if m.group("side") == "World" else "client-side",
            "trigger": m.group("trigger").replace('\\"', '"'),
            "family": m.group("family") or "Default",
        })
    if not rows:
        raise SystemExit("the table parsed to zero rows - the literal's shape changed")
    # A row written in a shape ROW_RE does not know would otherwise just vanish from the page.
    declared = len(re.findall(r"ETraceSoundSide::(?:Client|World)", body))
    if declared != len(rows):
        error("TraceSoundEvents.cpp's table has {0} rows naming a side but only {1} parsed - a row's "
              "shape changed; fix ROW_RE".format(declared, len(rows)))
    names = [r["name"] for r in rows]
    for n in sorted({n for n in names if names.count(n) > 1}):
        error("TraceSoundEvents.cpp declares '{0}' more than once".format(n))
    return rows


def parse_unwired():
    """
    {event: reason} for the events TraceSoundEvents::Unwired() lists (Demo 29 items 9 and 11).

    An unwired event is still declared, imported and resolvable - it is simply not allowed to sound.
    The page MUST say so on the row: auditioning a clip and then wondering why the game never makes
    that noise is precisely the confusion this list creates.

    Each row is `{ EventName, TEXT("a" "b") },`; the reason is "every string literal in the row,
    concatenated", because the C++ wraps only the FIRST fragment in TEXT() and lets the compiler
    splice the continuation lines.
    """
    text = read_text(EVENTS_CPP)
    names, reasons = [], {}
    m = re.search(r"static const FTraceUnwiredRow Table\[\]\s*=\s*\{(.*?)\n\t\t\};", text, re.S)
    if not m:
        error("could not find the FTraceUnwiredRow table in TraceSoundEvents.cpp - "
              "no sound could be shown as SILENT even if the game refuses to play it")
        return {}
    for entry in re.finditer(r"\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*,(.*?)\}\s*,", m.group(1), re.S):
        name = entry.group(1)
        pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', entry.group(2))
        names.append(name)
        joined = "".join(pieces).replace('\\"', '"').strip()
        if joined:
            reasons[name] = joined
        else:
            error("'{0}' is on TraceSoundEvents::Unwired() with no reason sentence".format(name))
    cvar = re.search(r'TEXT\("(Trace\.Audio\.UnwiredEvents)"\)', text)
    if names and not cvar:
        error("Trace.Audio.UnwiredEvents is no longer declared in TraceSoundEvents.cpp - the page "
              "tells the owner to use it to bring the SILENT sounds back")
    return {n: reasons.get(n, "") for n in names}


def parse_pre_existing():
    """
    The stems that existed before the release overhaul, from Scripts/generate_sounds.py's
    EXISTING_STEMS - the live freeze list that generator refuses to write over. The owner's
    original question was "test every single one ADDED", so the page tells them apart.
    """
    try:
        text = read_text(GENERATOR_PY)
    except OSError:
        warn("Scripts/generate_sounds.py is missing - every sound is shown as ORIGINAL")
        return None
    m = re.search(r"EXISTING_STEMS\s*=\s*frozenset\(\[(.*?)\]\)", text, re.S)
    if not m:
        warn("could not find EXISTING_STEMS in Scripts/generate_sounds.py - every sound is shown "
             "as ORIGINAL")
        return None
    return set(re.findall(r'"([A-Za-z0-9_]+)"', m.group(1)))


# ---------------------------------------------------------------------------
# 1b. the ability table: names, slots and kits, all read from the C++
# ---------------------------------------------------------------------------

def parse_abilities():
    """
    ({enum id: {"id", "slot", "slot_label", "kit", "name", "order"}}, [kit names]).

    GAbilityTable is THE place a player-facing ability name lives (Demo 35 and the renames after
    it), so the page reads it rather than carrying a copy. The slot labels come from
    TraceLoadoutSlotToString and the kit names from TraceCharacterIdToString - the latter are used
    ONLY to recognise ability sounds by their stems and to keep those names off the page.
    """
    text = read_text(ABILITY_TYPES_CPP)

    slot_labels = dict(re.findall(
        r"case\s+ETraceLoadoutSlot::(\w+)\s*:\s*return\s+TEXT\(\"([^\"]*)\"\)", text))
    kits = [n for _id, n in re.findall(
        r"case\s+ETraceCharacterId::(\w+)\s*:\s*return\s+TEXT\(\"([^\"]*)\"\)", text)
        if _id != "None"]
    if not kits:
        error("could not read the kit names out of TraceCharacterIdToString")

    m = re.search(r"const FTraceAbilityDef GAbilityTable\[\]\s*=\s*\{(.*?)\n\t\};", text, re.S)
    if not m:
        raise SystemExit("could not find GAbilityTable in " + ABILITY_TYPES_CPP)
    abilities = {}
    for order, row in enumerate(re.finditer(
            r"\{\s*ETraceAbilityId::(\w+)\s*,\s*ETraceLoadoutSlot::(\w+)\s*,\s*"
            r"ETraceCharacterId::(\w+)\s*,\s*TEXT\(\"([^\"]*)\"\)\s*\}", m.group(1))):
        aid, slot, kit, name = row.groups()
        abilities[aid] = {
            "id": aid, "slot": slot, "slot_label": slot_labels.get(slot, slot.upper()),
            "kit": kit, "name": name.strip() or aid, "order": order,
        }
    if not abilities:
        raise SystemExit("GAbilityTable parsed to zero rows - the literal's shape changed")
    return abilities, kits


def find_cvars(prefix):
    """Every console variable named TEXT("<prefix>...") anywhere under Source/."""
    found = set()
    pat = re.compile(r'TEXT\("(' + re.escape(prefix) + r'[A-Za-z0-9_.]+)"\)')
    for dirpath, _dirs, files in os.walk(SOURCE_DIR):
        for fn in files:
            if fn.endswith((".cpp", ".h")):
                found.update(pat.findall(read_text(os.path.join(dirpath, fn))))
    return found


# ---------------------------------------------------------------------------
# 2. call sites: who actually plays this event
# ---------------------------------------------------------------------------

_HEAD_RE = re.compile(
    r"^(?P<ind>\t?)(?:"
    r"namespace\s+(?P<ns>\w+)"
    r"|(?:static\s+)?FAuto\w+\s+(?P<cmd>\w+)\s*\("
    r"|(?:struct|class)\s+(?:\w+_API\s+)?(?P<type>\w+)\b[^;]*$"
    r"|(?!(?:if|for|while|switch|return|else|case|do)\b)[A-Za-z_][\w:<>,\*&\s]*?\b(?P<fn>[A-Za-z_]\w*(?:::\w+)*)\s*\([^;]*$"
    r")")


def enclosing_scopes(lines, index):
    """
    The names of the nearest column-0 head above lines[index], and the nearest one-tab head between
    it and the line. Unreal style indents a namespace's contents by one tab, so between them these
    are "which namespace / function / console command is this line in".
    """
    names = []
    seen_inner = False
    for j in range(index, -1, -1):
        line = lines[j]
        if not line.strip() or line.lstrip().startswith(("//", "*", "/*", "#")):
            continue
        if line.startswith("\t\t") or line.startswith("    "):
            continue
        m = _HEAD_RE.match(line.rstrip("\n"))
        if not m:
            continue
        name = m.group("ns") or m.group("cmd") or m.group("type") or m.group("fn")
        if m.group("ind"):
            if not seen_inner:
                names.append(name)
                seen_inner = True
            continue
        names.append(name)
        break
    return names


def index_call_sites():
    """
    ({event: ['Source/...cpp:123', ...]}, {event: [harness sites]}) - every place GAMEPLAY code
    names the event, and separately every harness mention (kept only for the report).

    Deliberately NOT "the line that also says TraceAudio::". Half the real triggers are indirect:
    the FX burst's type->event switch (TraceFxBurst.cpp), a ternary that picks backstab-or-front
    two lines above the call, `Music->Play(...)` on the music subsystem, a stinger chosen into a
    local. What is filtered is the harness: files named *Verify*/*Test*/*Integ*, scopes named the
    same way (or Debug/Probe/Audit/console commands), and play-count lookups.
    """
    hits, harness = {}, {}
    pattern = re.compile(r"TraceSoundEvents::([A-Za-z0-9_]+)")
    for dirpath, _dirs, files in os.walk(SOURCE_DIR):
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".h")):
                continue
            full = os.path.join(dirpath, fn)
            relpath = rel(full)
            if relpath.startswith(CALLSITE_EXCLUDE):
                continue
            lines = read_text(full).splitlines()
            file_is_harness = bool(HARNESS_FILE_RE.search(relpath))
            for i, line in enumerate(lines):
                stripped = line.lstrip()
                if stripped.startswith(("//", "*", "/*")):
                    continue          # a comment naming the event is not a trigger
                code = line.split("//", 1)[0]
                found = pattern.findall(code)
                if not found:
                    continue
                site = "{0}:{1}".format(relpath, i + 1)
                is_harness = (file_is_harness or LOOKUP_ONLY_RE.search(code) is not None
                              or any(HARNESS_SCOPE_RE.search(s or "") for s in enclosing_scopes(lines, i)))
                for ev in found:
                    (harness if is_harness else hits).setdefault(ev, []).append(site)
    return hits, harness


def find_anchor_span(relpath, regex):
    """(relpath, first line, last line) of the first match of `regex` (multi-line allowed), or None."""
    path = os.path.join(ROOT, relpath)
    try:
        text = read_text(path)
    except OSError:
        return None
    m = re.search(regex, text, re.S)
    if not m:
        return None
    return relpath, text.count("\n", 0, m.start()) + 1, text.count("\n", 0, m.end() - 1) + 1


def find_anchor(relpath, regex):
    """
    'path:line' of the first match, or None. The match's LAST line when the pattern spans lines:
    that is the line the pattern was written to land on (a guard after its function head).
    """
    span = find_anchor_span(relpath, regex)
    return None if span is None else "{0}:{1}".format(span[0], span[2])


# Indirect plays: events no line names, because a picker returns them.
INDIRECT = [
    (re.compile(r"Step\d+$"), [
        ("Source/Trace/Audio/TraceAudioWatch.cpp", r"const FName Clip = Record\.Steps\.Next\(\);"),
    ], "one of the eleven, picked per stride"),
    (re.compile(r"PistolShoot\d$"), [
        ("Source/Trace/Gameplay/TraceWeaponComponent.cpp", r"LocalShotLadder\.NextShot\("),
        ("Source/Trace/Audio/TraceAudioWatch.cpp", r"Entry\.Event = Record\.Ladder\.NextShot\("),
    ], "picked by the burst ladder"),
]


# ---------------------------------------------------------------------------
# 2b. functions, for the ability-path checks in section 3
# ---------------------------------------------------------------------------
#
# A function here is a column-0 head (`bool UTraceAbilitySetRocco::OnJumpPressed()`) whose body
# opens with a column-0 '{' and ends at the next column-0 '}' - Unreal style for every kit and actor
# method. Only .cpp files are scanned: a call from an inline header body would be missed (none of the
# walked functions has one today - checked when this was written, not on every run).

_FN_HEAD_RE = re.compile(
    r"^(?![\s#/{}])(?!(?:if|for|while|switch|return|else|case|do|namespace|class|struct|enum|template|"
    r"using|typedef|static_assert)\b)[^;=(]*?\b(?P<name>[A-Za-z_]\w*(?:::~?\w+)+)\s*\(")

# The narrower harness test for "who else calls this": test files, console commands and verify /
# probe / audit scopes. NOT "Debug": several kits route real gameplay through a Debug* method
# (UTraceAbilitySetOyster::DebugDropDashJar drops every dash jar), so a Debug* caller has to be named
# in the entry's `dev` list on purpose, never skipped by its name.
PATH_HARNESS_SCOPE_RE = re.compile(r"(Verify|Test|Harness|Integ|Probe|Audit|Cmd|Command)")

_lines_cache = {}
_fn_index = None          # {qualified name: [(relpath, head, open, close)]}, 0-based line numbers
_fn_ranges = {}           # {relpath: [(head, open, close, qualified name)]}


def source_lines(relpath):
    if relpath not in _lines_cache:
        _lines_cache[relpath] = read_text(os.path.join(ROOT, relpath)).splitlines()
    return _lines_cache[relpath]


def code_of(line):
    """The code on a line, comments removed ('' for a comment line)."""
    if line.lstrip().startswith(("//", "*", "/*")):
        return ""
    return line.split("//", 1)[0]


def function_index():
    global _fn_index
    if _fn_index is not None:
        return _fn_index
    _fn_index = {}
    for dirpath, _dirs, files in os.walk(SOURCE_DIR):
        for fn in sorted(files):
            if not fn.endswith(".cpp"):
                continue
            relpath = rel(os.path.join(dirpath, fn))
            lines = source_lines(relpath)
            ranges, i, n = [], 0, len(lines)
            while i < n:
                m = _FN_HEAD_RE.match(lines[i])
                if not m:
                    i += 1
                    continue
                opened = None
                for j in range(i, min(n, i + 16)):
                    if lines[j].startswith("{"):
                        opened = j
                        break
                    if code_of(lines[j]).rstrip().endswith(";") or (j > i and lines[j][:1] not in ("", "\t", " ")):
                        break          # a statement (a definition, a declaration), not a function
                if opened is None:
                    i += 1
                    continue
                close = opened + 1
                while close < n and not lines[close].startswith("}"):
                    close += 1
                _fn_index.setdefault(m.group("name"), []).append((relpath, i, opened, close))
                ranges.append((i, opened, close, m.group("name")))
                i = close + 1
            _fn_ranges[relpath] = ranges
    return _fn_index


def function_at(relpath, index):
    """(qualified name, is the head line) of the indexed function holding line `index`, or (None, False)."""
    function_index()
    for head, opened, close, name in _fn_ranges.get(relpath, []):
        if head <= index <= close:
            return name, index < opened
    return None, False


def qual(name):
    """'Rocco::OnJumpPressed' -> 'UTraceAbilitySetRocco::OnJumpPressed'. Full names pass through."""
    cls, _sep, meth = name.rpartition("::")
    if cls and not re.match(r"[AUF]?Trace", cls):
        return "UTraceAbilitySet" + cls + "::" + meth
    return name


def kit_of(qualname):
    m = re.match(r"UTraceAbilitySet(\w+)::", qualname)
    return m.group(1) if m else None


def _place_matches(place, allowed):
    for a in allowed:
        if a.endswith("::*") and place.startswith(a[:-1]):
            return True
        if place == a:
            return True
    return False


def find_uses(rx, relpaths=None):
    """[(relpath, 0-based line, where)] - every non-harness code line under Source/*.cpp matching rx,
    minus function heads (a definition is not a call). `where` is the enclosing function, or the
    column-0 scope (a namespace) for code outside an indexed function."""
    function_index()
    out = []
    for relpath in (relpaths or sorted(_fn_ranges)):
        lines = source_lines(relpath)
        if not rx.search("\n".join(lines)):
            continue
        file_is_harness = bool(HARNESS_FILE_RE.search(relpath))
        for i, line in enumerate(lines):
            code = code_of(line)
            if not code or not rx.search(code):
                continue
            if file_is_harness or any(PATH_HARNESS_SCOPE_RE.search(s or "") for s in enclosing_scopes(lines, i)):
                continue
            where, is_head = function_at(relpath, i)
            if is_head:
                continue
            if where is None:
                scopes = enclosing_scopes(lines, i)
                where = scopes[-1] if scopes else "<file scope>"
            out.append((relpath, i, where))
    return out


GUARD_TOKEN_RE = re.compile(
    r"\bIsAbility\(\s*ETraceAbilityId::(?P<ab>\w+)\s*\)"
    r"|\bIsSlot\(\s*ETraceLoadoutSlot::(?P<slot>\w+)\s*\)"
    r"|\bIsKitIn\(\s*ETraceCharacterId::(?P<kit>\w+)\s*,\s*ETraceLoadoutSlot::(?P<kslot>\w+)\s*\)")


def resolve_guard(m, fn, abilities):
    """(ability ids, words for the page) for one guard token, or (None, why)."""
    if m.group("ab"):
        aid = m.group("ab")
        if aid not in abilities:
            return None, "IsAbility({0}) names no GAbilityTable row".format(aid)
        return {aid}, abilities[aid]["name"]
    slot = m.group("slot") or m.group("kslot")
    kit = m.group("kit") or kit_of(fn)
    if kit is None:
        return None, "IsSlot in {0}, which is not a kit's method - say which kit".format(fn)
    ids = {a["id"] for a in abilities.values() if a["kit"] == kit and a["slot"] == slot}
    if len(ids) != 1:
        return None, ("the {0} slot check in {1} matches {2} abilities of that kit ({3}) - only "
                      "IsAbility can say which".format(slot, fn, len(ids), ", ".join(sorted(ids)) or "none"))
    label = next(a["slot_label"] for a in abilities.values() if a["slot"] == slot)
    return ids, label + " slot"


def call_rx(qualname):
    """A call of this function: the bare name for a kit method (called on `this` or a kit pointer),
    Class::Name for anything else (actor statics share short names - every actor has a Burst)."""
    cls, meth = qualname.rsplit("::", 1)
    if kit_of(qualname):
        return re.compile(r"(?<![\w~:])" + re.escape(meth) + r"\s*\(|(?:->|\.)" + re.escape(meth) + r"\s*\(")
    return re.compile(re.escape(cls) + r"::" + re.escape(meth) + r"\s*\(")


def check_path(ev, path, trigger, abilities):
    """
    Verify one path of ABILITY_SOUNDS[ev] against the code. Returns
    {"kind", "cover": ability ids, "evidence": words for the page} or None (errors printed).

    Every link is re-found: a call (the next function's name followed by '('), a spawn
    (SpawnActor<Class>, after which the path goes on in a method of Class) or a state (a regex the
    function writes, another regex the next function reads). The last function must hold the trigger.
    Guards are looked for in each function from its '{' to the line where it goes on.
    """
    kind = path["kind"]
    steps = [s if isinstance(s, tuple) else qual(s) for s in path["steps"]]
    head_name = steps[0] if not isinstance(steps[0], tuple) else "?"
    idx = function_index()
    trig_rel, trig_i = trigger
    guards, entry, links = [], None, []

    def fail(msg):
        error("ABILITY_SOUNDS '{0}', the path from {1}: {2}".format(ev, head_name, msg))
        return None

    for i, step in enumerate(steps):
        if isinstance(step, tuple):
            if i == 0 or isinstance(steps[i - 1], tuple):
                return fail("a {0} link must follow a function".format(step[0]))
            if i + 1 >= len(steps) or isinstance(steps[i + 1], tuple):
                return fail("a {0} link must lead to a function".format(step[0]))
            continue
        hits = idx.get(step, [])
        if len(hits) != 1:
            return fail("{0} is {1} in Source/".format(
                step, "not defined" if not hits else "defined {0} times".format(len(hits))))
        relpath, head, opened, close = hits[0]
        lines = source_lines(relpath)
        prev = steps[i - 1] if i else None
        nxt = steps[i + 1] if i + 1 < len(steps) else None

        if nxt is None:
            if trig_rel != relpath or not (opened < trig_i <= close):
                return fail("it ends in {0}, but the trigger {1}:{2} is not inside it".format(
                    step, trig_rel, trig_i + 1))
            onward = trig_i
        else:
            if isinstance(nxt, tuple) and nxt[0] == "spawn":
                rx = re.compile(r"SpawnActor<" + re.escape(nxt[1]) + r">")
                what = "SpawnActor<{0}>".format(nxt[1])
                links.append((rx.pattern, step, what))
            elif isinstance(nxt, tuple) and nxt[0] == "state":
                rx, what = re.compile(nxt[1]), "/{0}/".format(nxt[1])
            else:
                rx, what = call_rx(nxt), nxt.rsplit("::", 1)[1] + "()"
                links.append((rx.pattern, step, nxt))
            onward = next((j for j in range(opened + 1, close) if rx.search(code_of(lines[j]))), None)
            if onward is None:
                return fail("{0} no longer reaches {1}".format(step, what))

        if isinstance(prev, tuple) and prev[0] == "spawn" and not step.startswith(prev[1] + "::"):
            return fail("after spawning {0} the path must go on in a {0} method, not {1}".format(prev[1], step))
        if isinstance(prev, tuple) and prev[0] == "state":
            if not any(re.search(prev[2], code_of(lines[j])) for j in range(opened + 1, onward + 1)):
                return fail("{0} no longer reads /{1}/ before going on".format(step, prev[2]))
            # ...and only the function before it writes that state in its own file (bar named dev-only
            # writers, which no match reaches)
            writer = steps[i - 2]
            wrel = idx[writer][0][0]
            for urel, ui, where in find_uses(re.compile(prev[1]), [wrel]):
                if where != writer and where not in prev[3]:
                    return fail("/{0}/ is also written at {1}:{2} ({3}), not only in {4}".format(
                        prev[1], urel, ui + 1, where, writer))

        for j in range(opened + 1, onward + 1):
            for m in GUARD_TOKEN_RE.finditer(code_of(lines[j])):
                ids, words = resolve_guard(m, step, abilities)
                if ids is None:
                    return fail(words)
                guards.append((ids, words, "{0}:{1}".format(os.path.basename(relpath), j + 1)))
        if i == 0:
            entry = (relpath, head, onward)

    relpath, head, onward = entry
    if kind == "guard":
        if not guards:
            return fail("it is listed as checked (G), but no IsAbility / IsSlot / IsKitIn is on it")
        cover = set().union(*(g[0] for g in guards))
        ids, words, where = guards[0]
        return {"kind": kind, "cover": cover, "links": links,
                "evidence": "checks {0} at {1}".format(words, where)}
    if kind == "activate":
        m = re.match(r"UTraceAbilitySet(\w+)::ActivateAbility$", steps[0])
        if not m:
            return fail("an ACT() path must start at a kit's ActivateAbility")
        acts = [a["id"] for a in abilities.values() if a["kit"] == m.group(1) and a["slot"] == "Activated"]
        if len(acts) != 1:
            return fail("kit {0} has {1} ACTIVATED abilities in GAbilityTable".format(m.group(1), len(acts)))
        for ids, words, where in guards:
            if not ids <= set(acts):
                return fail("{0} checks {1}, which is not that kit's ACTIVATED ability".format(where, words))
        return {"kind": kind, "cover": set(acts), "links": links,
                "evidence": "ActivateAbility at {0}:{1}".format(os.path.basename(relpath), head + 1)}
    if kind == "any":
        kit = kit_of(steps[0])
        if kit is None:
            return fail("an ANY() path must start at a kit's method")
        if guards:
            return fail("it is listed as unchecked (ANY), but {0} checks {1} now - re-read the code and "
                        "make it a G() path".format(guards[0][2], guards[0][1]))
        cover = {a["id"] for a in abilities.values() if a["kit"] == kit}
        return {"kind": kind, "cover": cover, "links": links,
                "evidence": "no ability check at {0}:{1}".format(os.path.basename(relpath), onward + 1)}
    return fail("unknown path kind '{0}'".format(kind))


# ---------------------------------------------------------------------------
# 3. which ability plays each ability sound - a reading of the code, checked
# ---------------------------------------------------------------------------
#
# event -> {
#   abilities  ETraceAbilityId enum names, primary first (the row sits under the first). Display
#              names and slots come from GAbilityTable, never from here. MUST equal what `paths`
#              cover, so a sound cannot be filed under an ability the code does not tie it to.
#   label      what the sound IS, in a few words. No character names (checked).
#   when       when it fires. {AbilityId} is replaced by that ability's display name.
#   sites      [(file, regex)] - the trigger line(s), cited on the row. The FIRST is where the
#              paths must end.
#   paths      how the code gets from an ability to that trigger, one entry per route:
#                G(...)    a route with a real IsAbility / IsSlot / IsKitIn on it, which decides the
#                          ability (an IsSlot shared by two abilities of a kit is an ERROR);
#                ACT(...)  a route from a kit's ActivateAbility - that kit's ACTIVATED ability
#                          (UTraceAbilityComponent::TryActivate calls it on the activated kit only);
#                ANY(...)  a route from a kit hook with NO ability check - every ability of that
#                          kit, because the component ticks / notifies each equipped kit whatever slot
#                          it was picked for. If a check appears on it later, generation FAILS.
#              Steps are functions ('Kit::Method' is UTraceAbilitySet<Kit>::Method), SPAWN(Class)
#              (the path goes on in a method of Class) and STATE(write regex, read regex, dev): the
#              function before it is the only one in its file that writes the state (dev-only
#              writers named), the one after it reads it. Each link is re-found every run.
#   dev        functions / namespaces that also call a walked function or spawn a walked actor but
#              exist only for testing or screenshots. Accepted, never followed.
#   gated      {caller: why} - callers of a walked function that cannot get to the sound (the
#              reason should be backed by `needs`). Any OTHER caller is an ERROR: see
#              check_callers, which is what catches a second, unchecked route.
#   needs      [(file, regex, why)] - any other fact the reading rests on, re-found every run.
#   retired    {cvar, replaced_by, gate} - a sound only a Demo 35-retired path plays; `gate` is the
#              (file, regex) of the switch that keeps it off.
# }
CH = "Source/Trace/Abilities/Characters/"
FXB = "Source/Trace/Gameplay/TraceFxBurst.cpp"


def G(*steps):
    return {"kind": "guard", "steps": list(steps)}


def ACT(*steps):
    return {"kind": "activate", "steps": list(steps)}


def ANY(*steps):
    return {"kind": "any", "steps": list(steps)}


def SPAWN(cls):
    return ("spawn", cls)


def STATE(write_rx, read_rx, dev=()):
    return ("state", write_rx, read_rx, tuple(qual(d) for d in dev))


SPEC_KEYS = {"abilities", "label", "when", "sites", "paths", "dev", "gated", "needs", "retired"}

_ROCKET = ["Roxie::OnSecondaryPressed", "Roxie::SpawnRocket", SPAWN("ATraceRoxieRocket")]
_JAR_TO_CLOUD = [SPAWN("ATraceOysterJar"), "ATraceOysterJar::Burst",
                 "ATraceOysterPoisonCloud::ServerSpawnForBurst", SPAWN("ATraceOysterPoisonCloud"),
                 "ATraceOysterPoisonCloud::BeginPlay"]
_DASH_JAR = ["Oyster::NoteDashBegan",
             STATE(r"\bbDashJarOwedForThisDash\s*=\s*true", r"!bDashJarOwedForThisDash"),
             "Oyster::DropOwedDashJar", "Oyster::DebugDropDashJar", "Oyster::SpawnJar"]
_THROWN_JAR = ["Oyster::ActivateAbility", "Oyster::ThrowPickler", "Oyster::SpawnJar"]
_JAR_DEV = ["Oyster::DebugSpawnJarAt", "Oyster::DebugThrowPickler", "TraceOysterFxParade"]
_SPIKE = ["Mace::ActivateAbility", SPAWN("ATraceMaceSpike")]
_GATE = ["Elle::ActivateAbility", "Elle::PlaceGate", SPAWN("ATraceElleGate")]
_CLOAK = ["Elle::TickCloakTrigger", "Elle::StartCloak"]

ABILITY_SOUNDS = {
    # ---- movement -----------------------------------------------------------------------------
    "RoccoJump": dict(
        abilities=["JetBoots"], label="Air jump",
        when="the second jump, in mid-air; everyone nearby hears it, on top of your own Jump",
        sites=[(CH + "TraceAbilitySetRocco.cpp", r"TraceAudio::PlayAt\(MyPawn, TraceSoundEvents::RoccoJump")],
        paths=[G("Rocco::OnJumpPressed")]),
    "SlimeballStick": dict(
        abilities=["StickyGloves"], label="Wall stick",
        when="you stick to a wall; everyone nearby hears it",
        sites=[(CH + "TraceSlimewall.cpp", r"TraceAudio::PlayAt\(WorldPtr, TraceSoundEvents::SlimeballStick")],
        paths=[G("UTraceSlimeStickSubsystem::Tick")]),
    "RoxieRocketLaunch": dict(
        abilities=["RockJump"], label="Rocket launch",
        when="the rocket leaves the launcher",
        sites=[(CH + "TraceAbilitySetRoxie.cpp", r"TraceAudio::PlayAt\(this, TraceSoundEvents::RoxieRocketLaunch")],
        paths=[G("Roxie::OnSecondaryPressed")]),
    "RoxieRocketLoop": dict(
        abilities=["RockJump"], label="Rocket flight loop",
        when="loops on the rocket while it flies",
        sites=[(CH + "TraceRoxieRocket.cpp", r"StartLoopOn\(GetRootComponent\(\), TraceSoundEvents::RoxieRocketLoop\)")],
        paths=[G(*_ROCKET, "ATraceRoxieRocket::BeginPlay")], dev=["Roxie::DebugFireRocket"]),
    "RoxieRocketBurst": dict(
        abilities=["RockJump"], label="Rocket blast",
        when="the rocket detonates (big attenuation)",
        sites=[(CH + "TraceRoxieRocket.cpp", r"ATraceFxBurst::Burst\(GetWorld\(\), ETraceFxBurstType::RocketBurst"),
               (FXB, r"case ETraceFxBurstType::RocketBurst:\s*return TraceSoundEvents::RoxieRocketBurst")],
        paths=[G(*_ROCKET, "ATraceRoxieRocket::DetonateAndDestroy")], dev=["Roxie::DebugFireRocket"]),

    # ---- passive ------------------------------------------------------------------------------
    # BASH is checked on the movement component's dash-hit route, but the kit's own 20 Hz dash poll
    # (TickAbilities -> PollDashForBash -> TryBash) checks nothing, so a kit picked only for CUSTOM
    # STEEL or CHUD knocks players too. The row says so until the code changes.
    "ChutBash": dict(
        abilities=["Bash", "CustomSteel", "Chud"], label="Dash knock",
        when="your dash knocks a player back; plays at them",
        sites=[(CH + "TraceAbilitySetChut.cpp", r"ATraceFxBurst::Burst\(MyPawn->GetWorld\(\), ETraceFxBurstType::ChutBash"),
               (FXB, r"case ETraceFxBurstType::ChutBash:\s*return TraceSoundEvents::ChutBash")],
        paths=[G("Chut::OnDashHitCharacter", "Chut::TryBash"),
               ANY("Chut::TickAbilities", "Chut::PollDashForBash", "Chut::TryBash")]),
    "ElleCloak": dict(
        abilities=["Shimmer"], label="Cloak on",
        when="passing or throwing the Core cloaks you",
        sites=[(CH + "TraceAbilitySetElle.cpp", r"TraceAudio::Play\(MyPawn, TraceSoundEvents::ElleCloak\)")],
        paths=[G(*_CLOAK)]),
    "ElleDecloak": dict(
        abilities=["Shimmer"], label="Cloak off",
        when="the cloak ends (not on death)",
        sites=[(CH + "TraceAbilitySetElle.cpp", r"TraceAudio::Play\(MyPawn, TraceSoundEvents::ElleDecloak\)")],
        paths=[G(*_CLOAK, STATE(r"Flags\s*\|=\s*TraceAbilityFlags::EffectActive",
                                r"Flags\s*&\s*TraceAbilityFlags::EffectActive\)\s*==\s*0",
                                dev=["Elle::DebugStartCloak"]),
                 "Elle::EndCloak")]),
    "OysterJarBreak": dict(
        abilities=["PickleJar", "Pickler"], label="Jar break",
        when="a poison jar bursts into a cloud: {PickleJar} dash jars and {Pickler} jars alike",
        sites=[(CH + "TraceOysterPoison.cpp", r"TraceAudio::PlayReplicatedLocal\(this, TraceSoundEvents::OysterJarBreak")],
        paths=[G(*_DASH_JAR, *_JAR_TO_CLOUD), ACT(*_THROWN_JAR, *_JAR_TO_CLOUD)],
        dev=_JAR_DEV),
    # The bee sweep runs from TickAbilities with no ability check, and the component ticks X's kit
    # whatever it was picked for: LEECH alone, or STING alone, still orbits bees that sting.
    "XSting": dict(
        abilities=["XMechs", "Leech", "Sting"], label="Bee sting",
        when="an orbiting bee stings an enemy and marks them",
        sites=[(CH + "TraceAbilitySetX.cpp", r"ATraceFxBurst::Burst\(CurrentWorld, ETraceFxBurstType::BeeSting"),
               (FXB, r"case ETraceFxBurstType::BeeSting:\s*return TraceSoundEvents::XSting")],
        paths=[ANY("X::TickAbilities", "X::SweepBeeContacts")]),

    # ---- activated ----------------------------------------------------------------------------
    "RoccoRipple": dict(
        abilities=["Ripple"], label="Ripple laid",
        when="the ripple goes down; everyone nearby hears it",
        sites=[(CH + "TraceAbilitySetRocco.cpp", r"TraceAudio::PlayAt\(MyPawn, TraceSoundEvents::RoccoRipple")],
        paths=[ACT("Rocco::ActivateAbility")]),
    "RoccoRideLoop": dict(
        abilities=["Ripple"], label="Ripple ride loop",
        when="loops while you ride a ripple; each machine plays its own",
        sites=[(CH + "TraceRippleActor.cpp", r"StartLoopOn\(Pawn->GetRootComponent\(\), TraceSoundEvents::RoccoRideLoop\)")],
        paths=[ACT("Rocco::ActivateAbility", SPAWN("ATraceRippleActor"), "ATraceRippleActor::UpdateRideFx")]),
    "MaceSpikeThrow": dict(
        abilities=["Spike"], label="Spike throw",
        when="the spike is thrown",
        sites=[(CH + "TraceAbilitySetMace.cpp", r"TraceAudio::PlayAt\(this, TraceSoundEvents::MaceSpikeThrow")],
        paths=[ACT("Mace::ActivateAbility")]),
    "MaceSpikeEmbed": dict(
        abilities=["Spike"], label="Spike lands",
        when="the spike sticks into a surface",
        sites=[(CH + "TraceMaceSpike.cpp", r"ATraceFxBurst::Burst\(GetWorld\(\), ETraceFxBurstType::SpikeEmbed"),
               (FXB, r"case ETraceFxBurstType::SpikeEmbed:\s*return TraceSoundEvents::MaceSpikeEmbed")],
        paths=[ACT(*_SPIKE, "ATraceMaceSpike::FireEmbedBurstIfNeeded")], dev=["Mace::DebugThrowSpikeAt"]),
    "MacePullLoop": dict(
        abilities=["Spike"], label="Reel-in loop",
        when="loops while the spike reels you in",
        sites=[(CH + "TraceAbilitySetMace.cpp", r"StartLoopOn\(AttachTo, TraceSoundEvents::MacePullLoop\)")],
        paths=[ACT(*_SPIKE, "ATraceMaceSpike::Tick", "Mace::NotifySpikeEmbedded", "Mace::StartPull",
                   STATE(r"\bbPulling\s*=\s*true", r"TraceMaceFlags::Pulling"), "Mace::ApplyKitFx",
                   "Mace::SetPullFxAttached")],
        dev=["Mace::DebugThrowSpikeAt"],
        gated={"Mace::RequestSpikePull": "StartPull refuses without an embedded spike",
               "Mace::TickAbilities": "StartPull refuses without an embedded spike",
               "Mace::DetachAllKitFx": "passes false: it only stops the loop"},
        needs=[(CH + "TraceAbilitySetMace.cpp",
                r"void UTraceAbilitySetMace::StartPull\(\)\s*\{[^}]*?if \(!HasSpikeEmbedded\(\)",
                "StartPull refuses without an embedded spike, whoever calls it"),
               (CH + "TraceAbilitySetMace.cpp",
                r"void UTraceAbilitySetMace::DetachAllKitFx\(\)\s*\{[^}]*?SetPullFxAttached\(false\);[^}]*\}",
                "DetachAllKitFx only ever detaches")]),
    "OysterPickler": dict(
        abilities=["Pickler"], label="Jar lob",
        when="the jar is thrown",
        sites=[(CH + "TraceOysterJar.cpp", r"TraceAudio::Play\(this, TraceSoundEvents::OysterPickler\)")],
        paths=[ACT(*_THROWN_JAR, SPAWN("ATraceOysterJar"), "ATraceOysterJar::Initialise")],
        dev=_JAR_DEV,
        gated={"Oyster::DebugDropDashJar": "a dash jar has no velocity: it lands and returns first"},
        needs=[(CH + "TraceOysterJar.cpp", r"if \(FlightVelocity\.IsNearlyZero\(\)\)\s*\{[^}]*?Land\(\);\s*return;",
                "a jar with no velocity lands and returns before the lob sound"),
               (CH + "TraceAbilitySetOyster.cpp",
                r"UTraceAbilitySetOyster::DebugDropDashJar\(\)[\s\S]{0,1500}?SpawnJar\([^;]*FVector::ZeroVector",
                "dash jars are spawned with no velocity")]),
    "XStingLoad": dict(
        abilities=["Sting"], label="Bees into the gun",
        when="the five bee rounds load",
        sites=[(CH + "TraceAbilitySetX.cpp", r"TraceAudio::PlayAt\(this, TraceSoundEvents::XStingLoad")],
        paths=[ACT("X::ActivateAbility")]),
    "RoxieModded": dict(
        abilities=["Modded"], label="Modded on",
        when="{Modded} starts",
        sites=[(CH + "TraceAbilitySetRoxie.cpp", r"TraceAudio::PlayAt\(this, TraceSoundEvents::RoxieModded")],
        paths=[ACT("Roxie::ActivateAbility")]),
    "ElleSnap": dict(
        abilities=["Snap"], label="Gate opens",
        when="a gate snaps open",
        sites=[(CH + "TraceElleGate.cpp", r"TraceAudio::PlayReplicatedLocal\(this, TraceSoundEvents::ElleSnap")],
        paths=[ACT(*_GATE, "ATraceElleGate::TickGateFx")], dev=["Elle::DebugPlaceGatePair"]),
    "ElleTeleport": dict(
        abilities=["Snap"], label="Gate teleport",
        when="someone goes through a gate; plays at both ends",
        sites=[(CH + "TraceElleGate.cpp", r"ATraceFxBurst::Burst\(GetWorld\(\), ETraceFxBurstType::ElleTeleport"),
               (FXB, r"case ETraceFxBurstType::ElleTeleport:\s*return TraceSoundEvents::ElleTeleport")],
        paths=[ACT(*_GATE, "ATraceElleGate::CommitTeleport")], dev=["Elle::DebugPlaceGatePair"]),
    "SlimeballWall": dict(
        abilities=["Slimewall"], label="Wall up",
        when="the wall goes up",
        sites=[(CH + "TraceSlimewall.cpp", r"TraceAudio::PlayAt\(WorldPtr, TraceSoundEvents::SlimeballWall")],
        paths=[ACT("Slimeball::ActivateAbility", "ATraceSlimewall::ServerSpawn")], dev=["TraceSlimeFxParade"]),
    "MortimerQuake": dict(
        abilities=["Quake"], label="Quake blast",
        when="the blast (big attenuation)",
        sites=[(CH + "TraceAbilitySetMortimer.cpp", r"TraceAudio::PlayAt\(CasterPawn, TraceSoundEvents::MortimerQuake")],
        paths=[ACT("Mortimer::ActivateAbility")]),
    "LilyZip": dict(
        abilities=["Zip"], label="Zip start",
        when="the zip fires",
        sites=[(CH + "TraceAbilitySetLily.cpp", r"TraceAudio::Play\(MyPawn, TraceSoundEvents::LilyZip\)")],
        paths=[ACT("Lily::ActivateAbility", "Lily::StartZip")]),
    "LilyZipLoop": dict(
        abilities=["Zip"], label="Zip flight loop",
        when="loops while you zip",
        sites=[(CH + "TraceAbilitySetLily.cpp", r"StartLoopOn\(Pawn->GetRootComponent\(\), TraceSoundEvents::LilyZipLoop")],
        paths=[ACT("Lily::ActivateAbility", "Lily::StartZip", STATE(r"\bbZipping\s*=\s*true", r"TraceLilyFlags::Zipping"),
                   "Lily::OnClientStateEdge", "Lily::AttachZipFx")],
        gated={"Lily::SyncClientFx": "the same Zipping state, for a machine that joins mid-zip"}),

    # ---- retired in Demo 35 -------------------------------------------------------------------
    "MortimerMantle": dict(
        abilities=[], label="Mantle scuff",
        when="the old mantle carried you over a ledge",
        sites=[(CH + "TraceAbilitySetMortimer.cpp", r"TraceAudio::Play\(MyPawn, TraceSoundEvents::MortimerMantle\)")],
        retired=dict(cvar="Trace.Demo35.LegacyMantle", replaced_by="Blink",
                     gate=(CH + "TraceAbilitySetMortimer.cpp",
                           r"if \(CVarMortimerLegacyMantle\.GetValueOnAnyThread\(\) == 0\)\s*\{\s*return false;"))),
}


def check_callers(ev, spec, links):
    """
    THE SECOND-ROUTE CHECK. For every call and spawn the paths walk, every gameplay caller in Source/
    must be a step of one of this sound's paths, a named dev-only function or namespace (`dev`), or a
    caller that cannot get to the sound for a reason written down (`gated`). A guard on one caller
    says nothing about another, so an unlisted caller is an ERROR: it is a route nobody has read.
    """
    dev = [qual(d) for d in spec.get("dev", [])]
    gated = {qual(k): v for k, v in spec.get("gated", {}).items()}
    seen, used = {}, set()
    for rx, caller, target in links:
        seen.setdefault((rx, target), set()).add(caller)
    for (rx, target), callers in sorted(seen.items()):
        uses = find_uses(re.compile(rx))
        for urel, ui, where in uses:
            if where in callers:
                continue
            excuse = where if where in gated else next((d for d in dev if _place_matches(where, [d])), None)
            if excuse:
                used.add(excuse)
                continue
            error("ABILITY_SOUNDS '{0}': {1} is also reached from {2} ({3}:{4}) - another route to the "
                  "sound. Read it and add it as a path, or to `dev` / `gated` with the reason".format(
                      ev, target, where, urel, ui + 1))
    walked = {c for _rx, c, _t in links}
    for name in sorted((set(gated) | set(dev)) - used):
        error("ABILITY_SOUNDS '{0}' excuses {1}, which no longer reaches anything its paths walk - "
              "drop it, or it may hide a real route later".format(ev, name))
    for name in list(gated) + dev:
        if not name.endswith("::*") and "::" in name and name not in function_index():
            error("ABILITY_SOUNDS '{0}' excuses {1}, which is not defined in Source/ any more".format(ev, name))
        if name in walked:
            error("ABILITY_SOUNDS '{0}' excuses {1}, which one of its own paths walks".format(ev, name))


def fx_burst_types():
    """{event: [ETraceFxBurstType names]} from ATraceFxBurst::SoundEventFor's switch."""
    out = {}
    for t, ev in re.findall(r"case ETraceFxBurstType::(\w+):\s*return TraceSoundEvents::(\w+);",
                            read_text(os.path.join(ROOT, FXB))):
        out.setdefault(ev, []).append(t)
    return out


# ---------------------------------------------------------------------------
# 4. the WAVs
# ---------------------------------------------------------------------------

def read_wav(path):
    """
    (channels, rate, frames, samples as 16-bit, source bits).

    8/24/32-bit PCM is LEGAL - Scripts/import_sounds.py imports it ("8/24/32-bit is legal and
    imports fine") - so a replacement WAV in one of those depths must not stop this page being
    rebuilt. It is reduced to 16-bit for the page (the top 16 bits; the measurement moves by far
    less than the 0.1 dB the page prints) and the row says what the file really is.
    """
    with open(path, "rb") as fh:
        magic = fh.read(4)
    if magic == b"vers":
        raise SystemExit("{0} is an unfetched Git LFS pointer, not audio. Run: git lfs pull".format(rel(path)))
    with wave.open(path, "rb") as w:
        width = w.getsampwidth()
        nch = w.getnchannels()
        sr = w.getframerate()
        frames = w.getnframes()
        raw = w.readframes(frames)
    if width == 2:
        pcm16 = raw
    elif width == 1:                       # unsigned 8-bit
        pcm16 = array.array("h", [(b - 128) << 8 for b in raw]).tobytes()
        if sys.byteorder == "big":
            a = array.array("h")
            a.frombytes(pcm16)
            a.byteswap()
            pcm16 = a.tobytes()
    elif width in (3, 4):                  # little-endian signed: keep the top two bytes
        n = len(raw) // width
        b = bytearray(n * 2)
        b[0::2] = raw[width - 2::width]
        b[1::2] = raw[width - 1::width]
        pcm16 = bytes(b)
    else:
        raise wave.Error("{0}-bit samples".format(width * 8))
    samples = array.array("h")
    samples.frombytes(pcm16)
    if sys.byteorder == "big":
        samples.byteswap()
    return nch, sr, frames, samples, width * 8


def to_db(x):
    return (20.0 * math.log10(x)) if x > 1e-12 else float("-inf")


def levels(samples):
    """(peak dBFS, rms dBFS). Empty -> (-inf, -inf)."""
    if not samples:
        return float("-inf"), float("-inf")
    try:
        import numpy as np
        a = np.frombuffer(samples.tobytes(), dtype="<i2" if sys.byteorder == "little" else ">i2")
        a = a.astype("float64") / 32768.0
        peak = float(np.max(np.abs(a)))
        rms = float(math.sqrt(float(np.mean(a * a))))
    except ImportError:
        peak = max(max(samples), -min(samples)) / 32768.0
        rms = math.sqrt(sum((s / 32768.0) ** 2 for s in samples) / len(samples))
    return to_db(peak), to_db(rms)


def envelope(samples, nch):
    """ENVELOPE_BINS peak values over time, as 0..100 on a dB scale (ENVELOPE_FLOOR_DB..0)."""
    frames = len(samples) // max(1, nch)
    out = []
    for b in range(ENVELOPE_BINS):
        lo = (b * frames) // ENVELOPE_BINS
        hi = max(lo + 1, ((b + 1) * frames) // ENVELOPE_BINS)
        chunk = samples[lo * nch: hi * nch]
        peak = (max(max(chunk), -min(chunk)) / 32768.0) if len(chunk) else 0.0
        db = to_db(peak)
        out.append(0 if db == float("-inf") else
                   int(round(max(0.0, min(1.0, (db - ENVELOPE_FLOOR_DB) / -ENVELOPE_FLOOR_DB)) * 100)))
    return out


def wav_bytes(nch, sr, samples):
    """A minimal 16-bit PCM WAV in memory."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(nch)
        w.setsampwidth(2)
        w.setframerate(sr)
        data = array.array("h", samples)
        if sys.byteorder == "big":
            data.byteswap()
        w.writeframes(data.tobytes())
    return buf.getvalue()


def make_preview(nch, sr, samples, seconds, decimate):
    """First `seconds`, two-tap averaged then decimated. Returns (nch, sr, samples)."""
    keep_frames = min(len(samples) // nch, int(seconds * sr))
    head = samples[: keep_frames * nch]
    if decimate <= 1:
        return nch, sr, head
    out = array.array("h")
    step = nch * decimate
    for base in range(0, len(head) - step + 1, step):
        for c in range(nch):
            acc = 0
            for k in range(decimate):
                acc += head[base + k * nch + c]
            out.append(int(acc / decimate))
    return nch, sr // decimate, out


# ---------------------------------------------------------------------------
# 5. grouping
# ---------------------------------------------------------------------------

COMBAT = ["PistolShoot1", "PistolShoot2", "PistolShoot3", "PistolShoot4", "SmgShoot1",
          "Reload", "DryFire", "WeaponSwitch", "Bodyshot", "Headshot", "ShieldBlock",
          "MeleeSwing", "MeleeHit", "MeleeBackstab", "Parry",
          "DamageTaken", "Kill", "DeathBurst", "Respawn"]
MOVEMENT = ["Jump", "WallJump", "Dash"] + ["Step{0}".format(i) for i in range(1, 12)]
OBJECTIVE = ["CorePickup", "CoreTurnover", "Goal"]
UI = ["ButtonPress", "UIHover", "UIBack", "UIDeny", "CountdownTick", "CountdownGo"]
MUSIC = ["MusicTitle", "AmbienceMatch", "StingerVictory", "StingerDefeat"]

STATIC_GROUPS = [
    ("combat", "Combat", COMBAT, "Shots, melee, hits, deaths."),
    ("movement", "Movement", MOVEMENT,
     "Jump, wall-jump, dash, and eleven footsteps the game picks between. Footsteps are mixed far "
     "quieter than everything else."),
    ("core", "The Core", OBJECTIVE, "Pick-up, turnover, goal."),
]
TAIL_GROUPS = [
    ("ui", "UI and countdown", UI, "Menus and the kickoff countdown. All 2D."),
    ("music", "Music", MUSIC,
     "Two looping beds (12 s previews here) and the two match-end stingers."),
]


# ---------------------------------------------------------------------------
# 6. the page
# ---------------------------------------------------------------------------

CSS = r"""
:root{
  --bg:#111318; --panel:#171a21; --panel2:#1c2029; --line:#2a2f3b; --line2:#21252f;
  --ink:#e6e8ee; --dim:#9aa1b2; --faint:#717a8f; --accent:#6fd3c7; --b:#e8c06a;
  --warn:#f0a35e; --bad:#f0736e; --ok:#7fd8a0; --world:#7aa2f7; --client:#c3a0f0;
  --field:#0f1116;
}
*{box-sizing:border-box}
[hidden]{display:none !important}
html{scroll-padding-top:64px}
body{margin:0;background:var(--bg);color:var(--ink);
     font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
code,.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
code{background:var(--field);padding:1px 5px;border-radius:4px;font-size:12px;color:var(--accent)}
header{padding:24px 28px 16px;border-bottom:1px solid var(--line);background:var(--panel)}
h1{margin:0 0 6px;font-size:22px;letter-spacing:.2px}
.sub{color:var(--dim);font-size:13px;max-width:92ch;margin:4px 0 0}
.meta{color:var(--faint);font-size:12px;margin-top:8px}
.toc{display:flex;flex-wrap:wrap;gap:6px 14px;margin-top:10px;font-size:12.5px}
.toc a{color:var(--accent);text-decoration:none}
.toc a:hover{text-decoration:underline}
.bar{display:flex;flex-wrap:wrap;gap:8px 10px;align-items:center;padding:10px 28px;
     border-bottom:1px solid var(--line);background:var(--panel2);position:sticky;top:0;z-index:5}
.bar input[type=search]{flex:1 1 240px;min-width:180px;padding:7px 10px;border-radius:7px;
     border:1px solid var(--line);background:var(--field);color:var(--ink);font-size:13px}
button,.btn{font:inherit;font-size:13px;cursor:pointer;border-radius:7px;border:1px solid var(--line);
     background:#252a35;color:var(--ink);padding:6px 11px;display:inline-block;line-height:1.3}
button:hover,.btn:hover{background:#2e3441}
button:focus-visible,.btn:focus-within,input:focus-visible,textarea:focus-visible{outline:2px solid var(--accent);outline-offset:1px}
button.primary{background:var(--accent);color:#0e1116;border-color:var(--accent);font-weight:600}
button.danger{border-color:#6a3330;color:#f3b0ab}
.count{color:var(--dim);font-size:12px}
#status{color:var(--ok);font-size:12px;min-height:1em}
#status.err{color:var(--bad)}
.toggle{display:flex;gap:6px;align-items:center;font-size:12.5px;color:var(--dim);white-space:nowrap;cursor:pointer}
.toggle input{width:15px;height:15px;accent-color:var(--accent)}
main{padding:0 28px 80px}
.how{background:#151b20;border:1px solid #24313a;border-left:3px solid var(--accent);border-radius:8px;
     padding:10px 16px;margin:18px 0 4px;font-size:13px;color:#c3ccd8;max-width:118ch}
.how summary{cursor:pointer;font-weight:600;color:var(--ink)}
.how ol{margin:8px 0 4px;padding-left:20px}
.how li{margin:3px 0}
.how p{margin:6px 0}
.how .os{display:inline-block;min-width:4.9em;color:var(--faint);font-size:11.5px}
.errors{background:#2a1c1c;border:1px solid #573030;border-radius:8px;padding:10px 14px;margin:16px 0}
.notes{margin:12px 0 0;display:grid;gap:4px;font-size:12.5px;color:var(--dim)}
.notes b{color:var(--ink)}
section.grp{margin:26px 0 0}
h2{font-size:17px;margin:0 0 3px;padding-top:14px;border-top:1px solid var(--line)}
h3{font-size:15px;margin:18px 0 2px;letter-spacing:.3px}
.gnote{color:var(--dim);font-size:12.5px;margin:0 0 8px;max-width:100ch}
.slot{display:inline-block;font-size:10px;font-weight:600;letter-spacing:.6px;padding:1px 6px;border-radius:4px;
      margin-left:8px;vertical-align:2px;color:var(--dim);border:1px solid var(--line)}
.cols,.row{display:grid;grid-template-columns:40px minmax(0,2.3fr) minmax(0,1.15fr) minmax(0,1.6fr) minmax(0,1.25fr);
           gap:14px;align-items:start}
.cols{font-size:10.5px;text-transform:uppercase;letter-spacing:.6px;color:var(--faint);font-weight:600;
      padding:6px 6px;border-bottom:1px solid var(--line)}
.row{padding:11px 6px;border-bottom:1px solid var(--line2)}
.row.flagged{background:#241e16}
.play{width:34px;height:34px;padding:0;border-radius:50%;font-size:12px;line-height:1}
.play.playing{background:var(--accent);color:#0e1116;border-color:var(--accent)}
.play.b{width:28px;height:28px;font-size:11px;border-color:#5b4a23}
.play.b.playing{background:var(--b);border-color:var(--b)}
.ev{font-weight:600;font-size:14px}
.tag{display:inline-block;font-size:10px;padding:1px 6px;border-radius:4px;margin-left:6px;vertical-align:2px;
     border:1px solid;letter-spacing:.3px;white-space:nowrap}
.tag.world{color:var(--world);border-color:#33406a}
.tag.client{color:var(--client);border-color:#4a3d68}
.tag.new{color:var(--ok);border-color:#2f5e42}
.tag.orig{color:#78809a;border-color:#333a4a}
.tag.silent,.tag.retired,.tag.nowire{color:var(--bad);border-color:#6a3330}
.tag.prev{color:var(--warn);border-color:#5a4327}
.tag.also{color:var(--dim);border-color:var(--line)}
.when{color:var(--dim);font-size:12.5px;margin-top:2px}
.path{font-size:11.5px;color:#8b93a8;margin-top:3px;overflow-wrap:anywhere}
.site{font-size:11px;color:var(--faint);margin-top:2px;overflow-wrap:anywhere}
.gnote a,.notes a{color:var(--accent)}
.note{display:block;margin-top:5px;font-size:12px}
.note.bad{color:#e3a197}
.note.warn{color:var(--warn)}
.num{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11.5px;white-space:nowrap;color:#b9c0d0}
.ab{display:inline-block;width:15px;height:15px;border-radius:3px;font-size:10px;font-weight:700;text-align:center;
    line-height:15px;color:#0e1116;margin-right:5px;vertical-align:1px}
.ab.a{background:var(--accent)}
.ab.b{background:var(--b)}
svg.env{display:block;width:128px;height:22px;margin-top:4px}
svg.env path{stroke:none}
.c-a svg.env path{fill:var(--accent);opacity:.75}
.c-b svg.env path{fill:var(--b);opacity:.8}
.cand{margin-top:2px}
.cand .name{font-size:12.5px;font-weight:600;overflow-wrap:anywhere}
.cand .hd{display:flex;gap:7px;align-items:center}
.cand .x{border:none;background:none;color:var(--faint);padding:0 4px;font-size:14px;cursor:pointer}
.cand .x:hover{color:var(--bad)}
.delta{font-size:11.5px;color:var(--dim);margin-top:3px}
.fmtwarn{font-size:11.5px;color:var(--warn);margin-top:3px}
.cand .err{color:var(--bad);font-size:12px}
#nomatch{margin:20px 0 0;font-size:13.5px;color:var(--ink)}
.miss{font-size:12px;color:var(--warn);margin-top:3px}
.abrow{display:flex;gap:6px;margin-top:6px;flex-wrap:wrap}
.abrow button{font-size:12px;padding:3px 8px}
.try{font-size:12.5px;padding:5px 10px;position:relative}
.try input{position:absolute;inset:0;width:100%;height:100%;opacity:0;cursor:pointer}
svg.env path.base{fill:var(--line)}
.mark label{display:flex;gap:6px;align-items:center;font-size:12.5px;cursor:pointer}
.mark input[type=checkbox]{width:16px;height:16px;accent-color:var(--warn)}
.mark textarea{width:100%;height:34px;margin-top:5px;resize:vertical;border-radius:6px;border:1px solid var(--line);
     background:var(--field);color:var(--ink);font-family:inherit;font-size:12px;line-height:1.4;padding:5px 7px}
#out{width:100%;height:300px;margin-top:10px;border-radius:8px;border:1px solid var(--line);background:var(--field);
     color:var(--ink);font:12px/1.5 ui-monospace,Menlo,monospace;padding:10px;white-space:pre}
dialog{background:var(--panel);color:var(--ink);border:1px solid var(--line);border-radius:12px;max-width:900px;
       width:94vw;padding:18px 20px}
dialog::backdrop{background:rgba(0,0,0,.6)}
dialog h2{border:0;padding:0}
.dlgbar{margin-top:12px;display:flex;gap:10px;align-items:center;flex-wrap:wrap}
@media (max-width:900px){
  main,header,.bar{padding-left:16px;padding-right:16px}
  .cols{display:none}
  .row{grid-template-columns:40px minmax(0,1fr);gap:8px 12px}
  .row > .c-a,.row > .c-b,.row > .c-mark{grid-column:2}
}
"""

JS = r"""
'use strict';
const KEY = 'trace-sound-test-marks';
const DB_NAME = 'trace-sound-test', DB_STORE = 'candidates';
let marks = {};
try { const v = JSON.parse(localStorage.getItem(KEY) || '{}'); if (v && typeof v === 'object') marks = v; } catch (e) {}
let current = null;               // { btn, audio }
const candUrls = {};              // event -> object URL of the tried file

function $(s, el){ return (el || document).querySelector(s); }
function $$(s, el){ return [...(el || document).querySelectorAll(s)]; }
function rowOf(el){ return el.closest('.row'); }
function esc(s){ return String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }
let statusTimer = 0;
function status(msg, bad){
  const s = $('#status'); s.textContent = msg || ''; s.classList.toggle('err', !!bad);
  clearTimeout(statusTimer); if (msg) statusTimer = setTimeout(() => { s.textContent = ''; }, 8000);
}

function save(){
  try { localStorage.setItem(KEY, JSON.stringify(marks)); return true; }
  catch (e) { status('This browser is not saving marks (storage blocked).', true); return false; }
}
function entry(ev){ return marks[ev] || (marks[ev] = {}); }
function prune(ev){ const m = marks[ev]; if (m && !m.flag && !m.note && !m.cand) delete marks[ev]; }

// ---- IndexedDB: the tried files themselves, so B still plays after a reload -------------------
let dbp = null;
function db(){
  if (!dbp) dbp = new Promise((res, rej) => {
    try {
      const r = indexedDB.open(DB_NAME, 1);
      r.onupgradeneeded = () => r.result.createObjectStore(DB_STORE);
      r.onsuccess = () => res(r.result);
      r.onerror = () => rej(r.error);
    } catch (e) { rej(e); }
  });
  return dbp;
}
async function idb(mode, fn){
  const d = await db();
  return new Promise((res, rej) => {
    const tx = d.transaction(DB_STORE, mode); const req = fn(tx.objectStore(DB_STORE));
    tx.oncomplete = () => res(req ? req.result : undefined);
    tx.onerror = () => rej(tx.error); tx.onabort = () => rej(tx.error);
  });
}
const idbPut = (k, v) => idb('readwrite', s => s.put(v, k));
const idbGet = k => idb('readonly', s => s.get(k));
const idbDel = k => idb('readwrite', s => s.delete(k));
const idbClear = () => idb('readwrite', s => s.clear());

// ---- playback: A (the game's file) and B (a tried file) share these ---------------------------
// rewind() and not `a.currentTime = 0`: with preload="none" an unplayed <audio> has readyState 0
// and assigning currentTime throws InvalidStateError.
function rewind(a){ if (a.readyState > 0 && a.currentTime !== 0){ try { a.currentTime = 0; } catch (e) {} } }
function setBtn(b, on){ b.classList.toggle('playing', on); b.textContent = on ? '■' : '▶'; }
function stopAll(){
  $$('audio').forEach(a => { if (!a.paused) a.pause(); rewind(a); a.onended = null; });
  $$('.play.playing').forEach(b => setBtn(b, false));
  current = null;
}
function audioFor(btn){ const tr = rowOf(btn); return btn.classList.contains('b') ? $('audio.cb', tr) : $('audio.ca', tr); }
function start(btn, a, then){
  current = { btn, audio: a };
  setBtn(btn, true); rewind(a);
  a.onended = () => { setBtn(btn, false); if (current && current.audio === a) current = null; if (then) then(); };
  const p = a.play();
  if (p && p.catch) p.catch(err => {
    setBtn(btn, false); if (current && current.audio === a) current = null;
    if (err && err.name !== 'AbortError') status('Could not play: ' + err.message, true);
  });
}
function toggle(btn){
  const a = audioFor(btn); if (!a) return;
  if (current && current.audio === a && !a.paused){ stopAll(); return; }
  stopAll(); start(btn, a);
}
function playAB(btn){
  const tr = rowOf(btn); const ba = $('.play.a', tr), bb = $('.play.b', tr);
  if (!bb) return;
  stopAll(); start(ba, $('audio.ca', tr), () => setTimeout(() => { if (!current) start(bb, $('audio.cb', tr)); }, 250));
}

// ---- level maths shared by A and B -------------------------------------------------------------
const ENV_FLOOR = -60;
// The level over time, peak per bin on a dB scale. `frac` < 1 draws it over that fraction of the
// width, so when a row has a B, A and B share one time axis and a length difference is visible.
function envSvg(env, frac){
  if (!env || !env.length) return '';
  const n = env.length, h = 20, mid = h / 2, W = 100, f = (frac > 0 && frac < 1) ? frac : 1;
  const x = i => (i / (n - 1) * W * f).toFixed(2);
  let top = '', bot = '';
  env.forEach((v, i) => { const y = Math.max(0.4, v / 100 * mid); top += (i ? 'L' : 'M') + x(i) + ',' + (mid - y).toFixed(2); bot = 'L' + x(i) + ',' + (mid + y).toFixed(2) + bot; });
  return '<svg class="env" viewBox="0 0 ' + W + ' ' + h + '" preserveAspectRatio="none" aria-hidden="true">' +
         '<path class="base" d="M0,' + (mid - 0.3) + 'H' + W + 'V' + (mid + 0.3) + 'H0Z"/><path d="' + top + bot + 'Z"/></svg>';
}
function envA(tr){ try { return JSON.parse(tr.dataset.env || '[]'); } catch (e) { return []; } }
function db20(x){ return x > 1e-12 ? 20 * Math.log10(x) : -Infinity; }
function fdb(x){ return isFinite(x) ? (x >= 0 ? '+' : '−') + Math.abs(x).toFixed(1) : '−inf'; }
function fdelta(x, unit, digits){ if (!isFinite(x)) return 'n/a'; const s = x > 0 ? '+' : (x < 0 ? '−' : '±'); return s + Math.abs(x).toFixed(digits) + unit; }
function khz(r){ return (r / 1000).toFixed(r % 1000 ? 1 : 0) + ' kHz'; }
function chans(c){ return c === 1 ? 'mono' : (c === 2 ? 'stereo' : c + ' ch'); }

function str4(v, o){ return String.fromCharCode(v.getUint8(o), v.getUint8(o + 1), v.getUint8(o + 2), v.getUint8(o + 3)); }
function parseWav(buf){
  if (buf.byteLength < 12) return null;
  const v = new DataView(buf);
  if (str4(v, 0) !== 'RIFF' || str4(v, 8) !== 'WAVE') return null;
  let p = 12, fmt = null, dataBytes = null;
  while (p + 8 <= buf.byteLength){
    const id = str4(v, p), size = v.getUint32(p + 4, true);
    if (id === 'fmt ' && size >= 16){
      let tag = v.getUint16(p + 8, true);
      if (tag === 0xFFFE && size >= 26) tag = v.getUint16(p + 32, true);
      fmt = { tag, ch: v.getUint16(p + 10, true), rate: v.getUint32(p + 12, true), bits: v.getUint16(p + 22, true) };
    } else if (id === 'data'){
      dataBytes = Math.min(size, buf.byteLength - p - 8);
    }
    p += 8 + size + (size & 1);
  }
  if (!fmt) return null;
  const fb = fmt.ch * fmt.bits / 8;
  fmt.frames = (dataBytes !== null && fb > 0) ? Math.floor(dataBytes / fb) : null;
  return fmt;
}
function decode(buf, rate){
  const C = window.OfflineAudioContext || window.webkitOfflineAudioContext;
  const r = (rate >= 8000 && rate <= 384000) ? rate : 44100;
  const ctx = new C(1, 1, r);
  return new Promise((res, rej) => { const p = ctx.decodeAudioData(buf.slice(0), res, rej); if (p && p.then) p.then(res, rej); });
}
async function analyse(buf){
  const hdr = parseWav(buf);
  const ab = await decode(buf, hdr ? hdr.rate : 0);
  let peak = 0, sum = 0, n = 0;
  const chs = []; for (let c = 0; c < ab.numberOfChannels; c++) chs.push(ab.getChannelData(c));
  for (const d of chs){ for (let i = 0; i < d.length; i++){ const x = d[i], ax = x < 0 ? -x : x; if (ax > peak) peak = ax; sum += x * x; } n += d.length; }
  const env = [], frames = ab.length, bins = 64;
  for (let b = 0; b < bins; b++){
    const lo = Math.floor(b * frames / bins), hi = Math.max(lo + 1, Math.floor((b + 1) * frames / bins));
    let pk = 0; for (const d of chs){ for (let i = lo; i < hi && i < d.length; i++){ const ax = Math.abs(d[i]); if (ax > pk) pk = ax; } }
    const dbv = db20(pk); env.push(isFinite(dbv) ? Math.round(Math.max(0, Math.min(1, (dbv - ENV_FLOOR) / -ENV_FLOOR)) * 100) : 0);
  }
  const dur = (hdr && hdr.frames !== null && hdr.rate) ? hdr.frames / hdr.rate : ab.duration;
  return { dur, peak: db20(peak), rms: n ? db20(Math.sqrt(sum / n)) : -Infinity,
           ch: hdr ? hdr.ch : ab.numberOfChannels, rate: hdr ? hdr.rate : ab.sampleRate,
           bits: hdr ? hdr.bits : null, tag: hdr ? hdr.tag : null, wav: !!hdr, env };
}

function fmtWarnings(c, tr){
  const w = [];
  const curCh = +tr.dataset.ch;
  if (!c.wav) w.push('not a WAV — the importer reads .wav only; convert it first');
  else {
    if (c.tag !== 1) w.push(c.tag === 3 ? 'float WAV — game files are 16-bit PCM' : 'compressed WAV — game files are 16-bit PCM');
    else if (c.bits !== 16) w.push(c.bits + '-bit — game files are 16-bit');
    if (c.rate !== 44100) w.push(khz(c.rate) + ' — game files are 44.1 kHz');
  }
  if (c.ch !== curCh) w.push(chans(c.ch) + ' — this sound is ' + chans(curCh) + (curCh === 1 ? ' (it plays at a place in the world)' : ''));
  if (isFinite(c.peak) && c.peak > -0.1) w.push('peaks at 0 dBFS — may clip');
  return w;
}

// ---- B: try a file ----------------------------------------------------------------------------
async function onPick(input){
  const f = input.files && input.files[0];
  if (!f) return;
  const tr = rowOf(input), ev = tr.dataset.event;
  input.value = '';
  let buf, info;
  try { buf = await f.arrayBuffer(); info = await analyse(buf); }
  catch (e) { renderCandError(tr, f.name); status('Could not decode ' + f.name + ' as audio.', true); return; }
  const m = entry(ev);
  m.cand = { name: f.name, bytes: f.size, dur: info.dur, peak: info.peak, rms: info.rms, ch: info.ch,
             rate: info.rate, bits: info.bits, tag: info.tag, wav: info.wav, env: info.env, at: new Date().toISOString() };
  m.flag = true;
  save();
  let kept = true;
  try { await idbPut(ev, { name: f.name, type: f.type || 'audio/wav', data: buf }); }
  catch (e) { kept = false; }
  attachAudio(tr, buf, f.type || 'audio/wav');
  syncRow(tr);
  updateCount();
  const lab = tr.dataset.label;
  status(kept ? ('B for ' + lab + ': ' + f.name) : ('B for ' + lab + ': ' + f.name + ' (this browser will not keep the file after a reload)'), !kept);
}
function attachAudio(tr, buf, type){
  const ev = tr.dataset.event;
  if (candUrls[ev]) URL.revokeObjectURL(candUrls[ev]);
  candUrls[ev] = URL.createObjectURL(new Blob([buf], { type: type || 'audio/wav' }));
  let a = $('audio.cb', tr);
  if (!a){ a = document.createElement('audio'); a.className = 'cb'; a.preload = 'auto'; tr.querySelector('.c-b').appendChild(a); }
  a.src = candUrls[ev];
}
function renderCandError(tr, name){
  $('.cand', tr).innerHTML = '<div class="err">' + esc(name) + ' is not audio this browser can decode.</div>';
}
function renderCand(tr){
  const ev = tr.dataset.event, m = marks[ev], box = $('.cand', tr), envBox = $('.c-a .envbox', tr);
  if (!m || !m.cand){
    box.innerHTML = '';
    envBox.innerHTML = envSvg(envA(tr), 1);
    const a = $('audio.cb', tr); if (a){ a.pause(); a.remove(); }
    return;
  }
  const c = m.cand, hasAudio = !!$('audio.cb', tr);
  const aDur = +tr.dataset.dur, span = Math.max(aDur, c.dur) || 1;
  envBox.innerHTML = envSvg(envA(tr), aDur / span);
  // Length against the REAL file (a music bed's A is a 12 s preview); levels against what A plays.
  const dDur = c.dur - (+tr.dataset.fulldur || aDur), dPk = c.peak - (+tr.dataset.peak), dRms = c.rms - (+tr.dataset.rms);
  const warns = fmtWarnings(c, tr);
  box.innerHTML =
    '<div class="hd">' + (hasAudio ? '<button class="play b" onclick="toggle(this)" aria-label="Play B">▶</button>' : '') +
    '<span class="ab b">B</span><span class="name">' + esc(c.name) + '</span>' +
    '<button class="x" onclick="removeCand(this)" title="Remove B" aria-label="Remove B">✕</button></div>' +
    '<div class="num">' + c.dur.toFixed(2) + ' s · ' + chans(c.ch) + ' · ' + khz(c.rate) + (c.bits ? ' ' + c.bits + '-bit' : '') + '</div>' +
    '<div class="num">peak ' + fdb(c.peak) + ' · RMS ' + fdb(c.rms) + ' dBFS</div>' + envSvg(c.env, c.dur / span) +
    '<div class="delta">B−A: ' + fdelta(dDur, ' s', 2) + ' · peak ' + fdelta(dPk, ' dB', 1) + ' · RMS ' + fdelta(dRms, ' dB', 1) + '</div>' +
    warns.map(w => '<div class="fmtwarn">⚠ ' + esc(w) + '</div>').join('') +
    (hasAudio ? '<div class="abrow"><button onclick="playAB(this)">A then B</button></div>'
              : '<div class="miss">Not kept by this browser — pick it again to listen.</div>');
}
async function removeCand(btn){
  const tr = rowOf(btn), ev = tr.dataset.event;
  stopAll();
  if (marks[ev]) { delete marks[ev].cand; prune(ev); save(); }
  if (candUrls[ev]) { URL.revokeObjectURL(candUrls[ev]); delete candUrls[ev]; }
  try { await idbDel(ev); } catch (e) {}
  const a = $('audio.cb', tr); if (a) a.remove();
  syncRow(tr); updateCount(); status('Removed B for ' + tr.dataset.label + '.');
}

// ---- marks and notes ----------------------------------------------------------------------------
function syncRow(tr){
  const m = marks[tr.dataset.event];
  tr.classList.toggle('flagged', !!(m && m.flag));
  $('.c-mark input[type=checkbox]', tr).checked = !!(m && m.flag);
  const ta = $('.c-mark textarea', tr); if (document.activeElement !== ta) ta.value = (m && m.note) || '';
  renderCand(tr);
}
function onFlag(cb){ const tr = rowOf(cb), ev = tr.dataset.event; entry(ev).flag = cb.checked; prune(ev); save(); tr.classList.toggle('flagged', cb.checked); updateCount(); filter(); }
function onNote(ta){
  const tr = rowOf(ta), ev = tr.dataset.event, m = entry(ev), had = !!(m.note && m.note.trim());
  m.note = ta.value;
  if (!had && m.note.trim() && !m.flag){
    m.flag = true; $('.c-mark input[type=checkbox]', tr).checked = true; tr.classList.add('flagged'); updateCount();
  }
  prune(ev); save();
}
function updateCount(){
  const n = Object.values(marks).filter(m => m.flag).length;
  $('#count').textContent = n ? (n + ' marked') : 'nothing marked';
}
let clearArmed = 0;
async function clearAll(btn){
  if (Date.now() - clearArmed > 4000){ clearArmed = Date.now(); btn.textContent = 'Click again to clear all'; setTimeout(() => { btn.textContent = 'Clear marks'; }, 4000); return; }
  clearArmed = 0; btn.textContent = 'Clear marks';
  stopAll(); marks = {}; save();
  try { await idbClear(); } catch (e) {}
  Object.keys(candUrls).forEach(k => { URL.revokeObjectURL(candUrls[k]); delete candUrls[k]; });
  $$('audio.cb').forEach(a => a.remove());
  $$('.c-mark textarea').forEach(t => { t.value = ''; });     // the focused one too, which syncRow leaves alone
  $$('.row').forEach(syncRow); updateCount(); filter(); status('Cleared every mark, note and tried file.');
}

function filter(){
  const q = $('#q').value.trim().toLowerCase();
  const onlyNew = $('#onlynew').checked, onlyMarked = $('#onlymarked').checked;
  $$('.row').forEach(tr => {
    const m = marks[tr.dataset.event];
    const hit = (!q || tr.dataset.search.includes(q)) && (!onlyNew || tr.classList.contains('isnew')) &&
                (!onlyMarked || !!(m && (m.flag || m.cand || m.note)));
    tr.hidden = !hit;
  });
  const active = !!q || onlyNew || onlyMarked;
  $$('.blk').forEach(b => { const rs = $$('.row', b); b.hidden = rs.length ? !rs.some(tr => !tr.hidden) : active; });
  $$('section.grp').forEach(s => { s.hidden = !$$('.row', s).some(tr => !tr.hidden); });
  const nm = $('#nomatch'), shown = $$('.row').some(tr => !tr.hidden), say = [];
  let none = []; try { none = JSON.parse(nm.dataset.nosound || '[]'); } catch (e) {}
  const silent = q.length >= 3 ? none.filter(a => a[0].toLowerCase().includes(q)) : [];
  if (silent.length) say.push(silent.map(a => a[0] + ' (' + a[1] + ')').join(', ') + ': no sound of its own.');
  else if (active && !shown) say.push(onlyMarked && !q && !onlyNew ? 'Nothing marked.' : 'No sounds match.');
  nm.textContent = say.join(' '); nm.hidden = !say.length;
}

// ---- export -------------------------------------------------------------------------------------
function shq(x){ return "'" + String(x).replace(/'/g, "'\\''") + "'"; }
// The project's format is 16-bit PCM WAV at 44.1 kHz with the row's channel count. A tried file in
// that format is copied; anything else is converted by afconvert (macOS), never copied over the
// game's .wav as-is - a .m4a or a float WAV renamed .wav breaks the import.
function inFormat(c, d){ return c.wav && c.tag === 1 && c.bits === 16 && c.rate === 44100 && c.ch === +d.ch; }
function switchCmd(c, d){
  const src = shq('<folder>/' + c.name);
  return inFormat(c, d) ? 'cp ' + src + ' ' + d.path
                        : 'afconvert -f WAVE -d LEI16@44100 -c ' + d.ch + ' --mix ' + src + ' ' + d.path;
}
// The same switch for Windows, run from cmd.exe. Double quotes are cmd's only quoting (a Windows file
// name cannot hold one), and the destination takes backslashes: copy reads "/Sounds" after a space as a
// switch. afconvert is Mac-only and Windows ships no converter, so a tried file that is not already
// in the project's format gets NOTES - ffmpeg if it is installed, Audacity otherwise - as rem lines:
// the block stays safe to paste, and nothing copies a .m4a or a 24-bit WAV over the game's file.
function cq(x){ return '"' + String(x) + '"'; }
function winPath(p){ return String(p).replace(/\//g, '\\'); }
function winSwitch(c, d){
  const src = cq('<folder>\\' + c.name), dst = cq(winPath(d.path));
  if (inFormat(c, d)) return ['copy /Y ' + src + ' ' + dst];
  return ['rem CONVERT ' + cq(c.name) + ' by hand first - it is not 16-bit 44.1 kHz ' + chans(+d.ch) + ' PCM WAV. Either:',
          'rem   ffmpeg -y -i ' + src + ' -ac ' + d.ch + ' -ar 44100 -c:a pcm_s16le ' + dst,
          'rem   or in Audacity: open it, File, Export Audio, WAV, ' + chans(+d.ch) + ', 44100 Hz, Signed 16-bit PCM, saved as ' + dst];
}
function rowTitle(d){ return d.label + ' · ' + d.where + (d.label !== d.event ? '   [' + d.event + ']' : ''); }
function reportText(){
  const rows = $$('.row').filter(tr => { const m = marks[tr.dataset.event]; return m && m.flag; });
  const noted = $$('.row').filter(tr => { const m = marks[tr.dataset.event]; return m && !m.flag && m.note && m.note.trim(); });
  const L = [];
  L.push('TRACE — sounds to replace (' + rows.length + ')');
  L.push('From Art/Sounds/sound-test.html, built ' + document.body.dataset.built + '.');
  L.push('');
  if (!rows.length) L.push('(nothing marked)', '');
  const cmds = [], wcmds = [];
  rows.forEach((tr, i) => {
    const d = tr.dataset, m = marks[d.event];
    L.push((i + 1) + '. ' + rowTitle(d));
    L.push('   replace  ' + d.path + '   (' + (+d.fulldur).toFixed(2) + ' s, ' + chans(+d.ch) + ', ' + d.rate + ' Hz' +
           (d.bits !== '16' ? ' ' + d.bits + '-bit' : '') + ')');
    if (m.cand){
      const c = m.cand;
      L.push('   with     ' + c.name + '   (' + c.dur.toFixed(2) + ' s, ' + chans(c.ch) + ', ' + c.rate + ' Hz' + (c.bits ? ' ' + c.bits + '-bit' : '') +
             ', peak ' + fdb(c.peak) + ' dBFS, RMS ' + fdb(c.rms) + ' dBFS)');
      fmtWarnings(c, tr).forEach(w => L.push('            ! ' + w));
      if (!inFormat(c, d)) L.push('            the afconvert line below makes it 16-bit 44.1 kHz ' + chans(+d.ch) +
                                  ' (on Windows: convert it by hand, as the rem lines say)');
      cmds.push(switchCmd(c, d));
      wcmds.push(...winSwitch(c, d));
    } else {
      L.push('   with     (no file tried yet)');
      cmds.push('cp ' + shq('<new file>.wav') + ' ' + d.path + '    # 16-bit 44.1 kHz ' + chans(+d.ch) + ' WAV');
      wcmds.push('copy /Y ' + cq('<new file>.wav') + ' ' + cq(winPath(d.path)) + '   & rem 16-bit 44.1 kHz ' + chans(+d.ch) + ' WAV');
    }
    if (m.note && m.note.trim()) L.push('   note     ' + m.note.trim().replace(/\n+/g, ' / '));
    if (d.state) L.push('   status   ' + d.state);
    L.push('');
  });
  if (noted.length){
    L.push('NOTES ON SOUNDS NOT MARKED (' + noted.length + ')');
    noted.forEach(tr => L.push('- ' + rowTitle(tr.dataset) + ': ' + marks[tr.dataset.event].note.trim().replace(/\n+/g, ' / ')));
    L.push('');
  }
  if (!rows.length) return L.join('\n').replace(/\n+$/, '');
  const evs = rows.map(tr => tr.dataset.event);
  const locks = evs.map(e => 'Content/Trace/Audio/S_' + e + '.uasset').concat(['Content/Trace/Audio/DA_TraceSoundBank.uasset']);
  L.push('TO SWITCH THEM — in the main checkout, Unreal editor closed:');
  L.push('  git pull');
  L.push('  Scripts/lock.sh ' + locks.join(' '));
  cmds.forEach(c => L.push('  ' + c));
  L.push('  ./Scripts/import-sounds.sh --only ' + evs.join(','));
  L.push('  git add ' + rows.map(tr => tr.dataset.path).join(' ') + ' Content/Trace/Audio');
  // A comment, not prose: pasted into a shell, 'git commit, git push, then: ...' runs `git commit,`.
  L.push('  # after git commit and git push: Scripts/unlock.sh ' + locks.join(' '));
  L.push('The editor and the Scripts/run-*.sh games play the new file on their next launch (no C++ rebuild);');
  L.push('Trace.Audio.Reload picks it up in a running game.');
  L.push('Packaged builds keep the old one until re-packaged: ./Scripts/package.sh (--iterate re-cooks only what changed).');
  L.push('Then: python3 Scripts/generate_sound_page.py to refresh this page.');
  L.push('');
  // Command Prompt, named: Windows Terminal opens PowerShell by default, and there copy /Y and rem
  // are errors.
  L.push('ON WINDOWS — the same steps in Command Prompt (cmd.exe, not PowerShell), in the main checkout folder, Unreal editor closed:');
  L.push('  git pull');
  L.push('  Scripts\\lock.bat ' + locks.join(' '));
  wcmds.forEach(c => L.push('  ' + c));
  L.push('  Scripts\\import-sounds.bat --only ' + cq(evs.join(',')));
  L.push('  git add ' + rows.map(tr => tr.dataset.path).join(' ') + ' Content/Trace/Audio');
  L.push('  rem after git commit and git push: Scripts\\unlock.bat ' + locks.join(' '));
  if (wcmds.some(c => c.startsWith('rem CONVERT')))
    L.push('Convert every "rem CONVERT" file before import-sounds.bat runs: it imports whatever WAV is in place.');
  L.push('The editor and the Scripts\\run-*.bat games play the new file on their next launch (no C++ rebuild);');
  L.push('Trace.Audio.Reload picks it up in a running game.');
  L.push('Packaged builds keep the old one until re-packaged: Scripts\\package.bat (--iterate re-cooks only what changed).');
  L.push('Then: python Scripts\\generate_sound_page.py (py -3 on some machines) to refresh this page.');
  return L.join('\n');
}
function report(){
  const out = $('#out'); out.value = reportText();
  $('#dlgstatus').textContent = '';
  $('#dlg').showModal(); out.focus(); out.select();
}
async function copyOut(){
  const out = $('#out'), s = $('#dlgstatus');
  try { await navigator.clipboard.writeText(out.value); s.textContent = 'Copied.'; return true; }
  catch (e) {
    out.focus(); out.select();
    let ok = false; try { ok = document.execCommand('copy'); } catch (e2) {}
    s.textContent = ok ? 'Copied.' : 'Copy blocked — the text is selected: press ⌘C / Ctrl+C.';
    return ok;
  }
}
function downloadOut(){
  const blob = new Blob([$('#out').value + '\n'], { type: 'text/plain' });
  const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = 'trace-sound-replacements.txt';
  document.body.appendChild(a); a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  $('#dlgstatus').textContent = 'Saved trace-sound-replacements.txt to your downloads.';
}

// ---- boot ---------------------------------------------------------------------------------------
async function restoreCandidates(){
  for (const tr of $$('.row')){
    const ev = tr.dataset.event, m = marks[ev];
    if (!m || !m.cand) continue;
    try { const v = await idbGet(ev); if (v && v.data){ attachAudio(tr, v.data, v.type); } } catch (e) {}
    renderCand(tr);
  }
  document.body.dataset.restored = '1';
}
window.addEventListener('DOMContentLoaded', () => {
  $$('.row').forEach(syncRow);
  updateCount(); filter();
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$('#dlg').open) stopAll(); });
  restoreCandidates();
});
"""


def fmt_db(v):
    return "-inf" if v == float("-inf") else "{0:+.1f}".format(v).replace("-", "−")


def js_num(v):
    return "-Infinity" if v == float("-inf") else "{0:.2f}".format(v)


def build_page(ctx):
    rows, groups = ctx["rows"], ctx["groups"]
    e = html.escape
    parts = []
    A = parts.append
    A("<!DOCTYPE html>")
    A('<html lang="en"><head><meta charset="utf-8">')
    A('<meta name="viewport" content="width=device-width,initial-scale=1">')
    A("<title>Trace Sound Test</title>")
    A("<style>{0}</style></head>".format(CSS))
    A('<body data-built="{0}">'.format(e(ctx["built"], quote=True)))
    A("<header>")
    A("<h1>Trace &mdash; every sound in the game</h1>")
    A('<p class="sub">{0} sounds. <b>&#9654;</b> plays the exact file the game imports (A). '
      '<b>Try a file&hellip;</b> plays your own WAV beside it (B) with its length and level. Tick '
      '<b>replace</b>, add a note, then <b>Export list</b> for the exact paths and commands. Marks, '
      'notes and tried files stay in this browser.</p>'.format(len(rows)))
    notes = []
    silent = [r for r in rows if r["unwired"]]
    if silent:
        notes.append('<span><b>{0} SILENT</b> &mdash; {1}: switched off in Demo 29, still playable here. '
                     '<code>Trace.Audio.UnwiredEvents 0</code> brings them back.</span>'.format(
                         len(silent), ", ".join(e(r["label"]) for r in silent)))
    retired = [r for r in rows if r["retired"]]
    if retired:
        notes.append('<span><b>{0} RETIRED</b> &mdash; {1}: only a feature Demo 35 retired plays {2}; '
                     'the row names the switch that brings it back.</span>'.format(
                         len(retired), ", ".join(e(r["label"]) for r in retired),
                         "it" if len(retired) == 1 else "them"))
    nowire = [r for r in rows if not r["sites"]]
    if nowire:
        notes.append('<span><b>{0} NOT WIRED</b> &mdash; {1}: nothing in Source/ plays {2}.</span>'.format(
            len(nowire), ", ".join(e(r["label"]) for r in nowire), "it" if len(nowire) == 1 else "them"))
    if notes:
        A('<div class="notes">{0}</div>'.format("".join(notes)))
    A('<p class="meta">Built {0} by <code>Scripts/generate_sound_page.py</code>. {1}</p>'.format(
        e(ctx["built"]), e(ctx["size_note"])))
    A('<nav class="toc">{0}</nav>'.format(" ".join(
        '<a href="#g-{0}">{1}</a>'.format(g["id"], e(g["title"])) for g in groups)))
    A("</header>")

    A('<div class="bar">')
    A('<input type="search" id="q" placeholder="Filter: sound, ability, file&hellip;" '
      'aria-label="Filter sounds" oninput="filter()">')
    A('<label class="toggle"><input type="checkbox" id="onlynew" onchange="filter()"> '
      'only the {0} added in the overhaul</label>'.format(ctx["new_count"]))
    A('<label class="toggle"><input type="checkbox" id="onlymarked" onchange="filter()"> only marked</label>')
    A('<button onclick="stopAll()" title="Esc">Stop</button>')
    A('<button class="primary" onclick="report()">Export list</button>')
    A('<button class="danger" onclick="clearAll(this)">Clear marks</button>')
    A('<span class="count" id="count"></span><span id="status" role="status"></span>')
    A("</div>")

    A("<main>")
    if ctx["errors"]:
        A('<div class="errors"><b>Generator errors &mdash; this page may be wrong</b><ul>')
        for p in ctx["errors"]:
            A("<li>{0}</li>".format(e(p)))
        A("</ul></div>")
    # Mac (Terminal) and Windows (plain cmd.exe, Scripts\*.bat) side by side: the steps are the same,
    # only the script names, the copy command and the converter differ.
    lock_args = ("Content/Trace/Audio/S_&lt;Event&gt;.uasset "
                 "Content/Trace/Audio/DA_TraceSoundBank.uasset")

    def two(mac, win):
        return ('<span class="os">Mac</span> <code>{0}</code><br>'
                '<span class="os">Windows</span> <code>{1}</code>'.format(mac, win))

    A('<details class="how" open><summary>Switching a sound</summary><ol>'
      '<li>In the main checkout, Unreal editor closed: <code>git pull</code>, then lock the sound and the '
      'bank &mdash; both are read-only until locked, and the import rewrites both.<br>' +
      two("Scripts/lock.sh " + lock_args, "Scripts\\lock.bat " + lock_args) + '</li>'
      '<li>Put the new sound over the row&rsquo;s file &mdash; same name, same folder &mdash; as 44.1&nbsp;kHz '
      '16-bit PCM WAV, mono or stereo to match the row. B warns when a file isn&rsquo;t. '
      '<b>Export list</b> gives the commands: <code>cp</code> or the <code>afconvert</code> line that fixes it '
      'on a Mac; on Windows <code>copy /Y</code>, or for a file that needs converting, an '
      '<code>ffmpeg</code> line (if it is installed) and the Audacity export settings &mdash; '
      '<code>afconvert</code> is Mac-only.</li>'
      '<li>Import it (several: <code>--only A,B</code>; <code>--list</code> checks the WAVs without the editor):<br>' +
      two("./Scripts/import-sounds.sh --only &lt;Event&gt;", "Scripts\\import-sounds.bat --only &lt;Event&gt;") +
      '</li>'
      '<li>The editor and the <code>Scripts/run-*.sh</code> / <code>Scripts\\run-*.bat</code> games play it on '
      'their next launch &mdash; no C++ rebuild. <code>Trace.Audio.Reload</code> picks it up in a running game; '
      '<code>Trace.Audio.Test &lt;Event&gt;</code> fires it.</li>'
      '<li>Packaged builds keep the old sound until re-packaged (<code>--iterate</code> re-cooks only what '
      'changed):<br>' + two("./Scripts/package.sh", "Scripts\\package.bat") + '</li>'
      '<li>Commit the WAV and what changed in <code>Content/Trace/Audio</code>, push, then unlock the same '
      'files:<br>' + two("Scripts/unlock.sh &hellip;", "Scripts\\unlock.bat &hellip;") + '</li>'
      '</ol><p><b>Export list</b> writes these commands out for every marked row, Mac first, then Windows. '
      'Run the Windows ones in Command Prompt (<code>cmd.exe</code>), not PowerShell: there '
      '<code>copy /Y</code> and <code>rem</code> are errors. '
      '<code>python3 Scripts/generate_sound_page.py</code> rebuilds this page '
      '(Windows: <code>python Scripts\\generate_sound_page.py</code>).</p></details>')

    A('<p class="gnote" id="nomatch" hidden data-nosound="{0}"></p>'.format(e(json.dumps(
        ctx["nosound"], separators=(",", ":")), quote=True)))
    for g in groups:
        A('<section class="grp" id="g-{0}">'.format(g["id"]))
        A("<h2>{0} <span class='count'>&nbsp;{1} sound{2}</span></h2>".format(
            e(g["title"]), g["count"], "" if g["count"] == 1 else "s"))
        if g.get("note"):
            A('<p class="gnote">{0}</p>'.format(g["note"]))
        A('<div class="cols" aria-hidden="true"><span></span><span>Sound</span><span>A &middot; in the game'
          '</span><span>B &middot; your file</span><span>Replace?</span></div>')
        for blk in g["blocks"]:
            A('<div class="blk">')
            if blk.get("title"):
                A('<h3 id="a-{0}">{1}<span class="slot">{2}</span></h3>'.format(
                    e(blk["anchor"]), e(blk["title"]), e(blk["slot"])))
            if blk.get("note"):
                A('<p class="gnote">{0}</p>'.format(blk["note"]))
            for r in blk["rows"]:
                A(render_row(r))
            A("</div>")
        A("</section>")

    A("</main>")
    A('<dialog id="dlg"><h2>Sounds to replace</h2>'
      '<textarea id="out" readonly spellcheck="false"></textarea>'
      '<div class="dlgbar"><button class="primary" onclick="copyOut()">Copy</button>'
      '<button onclick="downloadOut()">Download .txt</button>'
      '<button onclick="document.getElementById(\'dlg\').close()">Close</button>'
      '<span class="count" id="dlgstatus" role="status"></span></div></dialog>')
    A("<script>{0}</script>".format(JS))
    A("</body></html>")
    return "\n".join(parts)


def render_row(r):
    e = html.escape
    classes = ["row", "isnew" if r["new"] else "isold"]
    side_cls = "world" if r["side"] == "game-side" else "client"
    tags = ['<span class="tag {0}">{1}</span>'.format(side_cls, r["side"])]
    tags.append('<span class="tag new">NEW</span>' if r["new"] else '<span class="tag orig">ORIGINAL</span>')
    if r["unwired"]:
        tags.append('<span class="tag silent">SILENT</span>')
    if r["retired"]:
        tags.append('<span class="tag retired">RETIRED</span>')
    if not r["sites"]:
        tags.append('<span class="tag nowire">NOT WIRED</span>')
    if r["preview"]:
        tags.append('<span class="tag prev">PREVIEW</span>')
    for extra in r["also"]:
        tags.append('<span class="tag also">also {0}</span>'.format(e(extra)))

    state = []
    notes = []
    if r["unwired"]:
        notes.append('<span class="note bad">Silent in matches: {0}. <code>Trace.Audio.UnwiredEvents 0</code> '
                     'brings it back.</span>'.format(e(r["unwired"])))
        state.append("SILENT in matches (Trace.Audio.UnwiredEvents 0 brings it back)")
    if r["retired"]:
        notes.append('<span class="note warn">Retired in Demo 35 &mdash; {0} replaced the feature that played '
                     'it. <code>{1} 1</code> brings it back.</span>'.format(
                         e(r["retired"]["replaced_by"]), e(r["retired"]["cvar"])))
        state.append("RETIRED (Demo 35); {0} 1 brings it back".format(r["retired"]["cvar"]))
    if not r["sites"]:
        notes.append('<span class="note warn">Nothing in Source/ plays this event.</span>')
        state.append("NOT WIRED: nothing in Source/ plays it")
    if r["unchecked_note"]:
        notes.append('<span class="note warn">{0}</span>'.format(e(r["unchecked_note"])))
        state.append(r["unchecked_note"])

    shown = r["sites"][:3]
    more = len(r["sites"]) - len(shown)
    site_line = ""
    if shown:
        site_line = "plays at " + " &nbsp;".join(e(s) for s in shown)
        if more > 0:
            site_line += " &nbsp;+{0} more".format(more)
        if r["checks"]:
            site_line += " &middot; {0}".format("; ".join(e(c) for c in r["checks"]))
        elif r["indirect_note"]:
            site_line += " &middot; {0}".format(e(r["indirect_note"]))

    data = {
        "event": r["name"], "path": r["relpath"], "label": r["label"], "where": r["where"],
        "search": r["search"], "dur": "{0:.4f}".format(r["duration"]), "peak": js_num(r["peak"]),
        "rms": js_num(r["rms"]), "ch": str(r["channels"]), "rate": str(r["src_rate"]), "bits": str(r["bits"]),
        "env": json.dumps(r["env"], separators=(",", ":")), "state": "; ".join(state),
        "fulldur": "{0:.4f}".format(r["full_duration"]),
    }
    attrs = " ".join('data-{0}="{1}"'.format(k, e(v, quote=True)) for k, v in data.items())
    out = ['<div class="{0}" id="r-{1}" {2}>'.format(" ".join(classes), e(r["name"], quote=True), attrs)]
    out.append('<div class="c-play"><button class="play a" onclick="toggle(this)" aria-label="Play {0}">&#9654;'
               '</button><audio class="ca" preload="none" src="data:audio/wav;base64,{1}"></audio></div>'.format(
                   e(r["label"], quote=True), r["b64"]))
    out.append('<div class="c-sound"><div><span class="ev">{0}</span>{1}</div>'
               '<div class="when">{2}</div><div class="path mono">{3}</div>{4}{5}</div>'.format(
                   e(r["label"]), "".join(tags), e(r["when"]), e(r["relpath"]),
                   '<div class="site mono">{0}</div>'.format(site_line) if site_line else "",
                   "".join(notes)))
    full = ('<div class="num" style="color:var(--warn)">full file {0:.1f} s &middot; {1:g} kHz &middot; {2:.1f} MB'
            '</div>'.format(r["full_duration"], r["src_rate"] / 1000.0, r["full_mb"]) if r["preview"] else "")
    out.append('<div class="c-a"><div class="num"><span class="ab a">A</span>{0:.2f} s{1} &middot; {2} &middot; {3}'
               '</div><div class="num">peak {4} &middot; RMS {5} dBFS</div><div class="envbox"></div>{6}</div>'.format(
                   r["duration"], " preview" if r["preview"] else "", "mono" if r["channels"] == 1 else "stereo",
                   "{0:g} kHz".format(r["rate"] / 1000.0) + ("" if r["bits"] == 16 else " {0}-bit".format(r["bits"])),
                   fmt_db(r["peak"]), fmt_db(r["rms"]), full))
    out.append('<div class="c-b"><label class="btn try">Try a file&hellip;<input type="file" '
               'accept=".wav,audio/wav,audio/x-wav,audio/*" onchange="onPick(this)" '
               'aria-label="Try a file for {0}"></label><div class="cand"></div></div>'.format(
                   e(r["label"], quote=True)))
    out.append('<div class="c-mark mark"><label><input type="checkbox" onchange="onFlag(this)" '
               'aria-label="Mark {0} for replacement"> replace</label>'
               '<textarea placeholder="What is wrong with it?" oninput="onNote(this)" '
               'aria-label="Note for {0}"></textarea></div>'.format(e(r["label"], quote=True)))
    out.append("</div>")
    return "".join(out)


# ---------------------------------------------------------------------------
# 7. checks that keep character names off the page
# ---------------------------------------------------------------------------

def name_leak_checker(kits, abilities):
    names = sorted(kits, key=len, reverse=True)
    # A name followed by a capital is an identifier (RoccoJump) and counts; a lowercase continuation
    # is a different word and does not.
    pats = [(n, re.compile(r"(?<![A-Za-z0-9_.\-])" + re.escape(n) + r"(?![a-z0-9_\-])")) for n in names]
    display = sorted({a["name"] for a in abilities.values()}, key=len, reverse=True)

    def check(where, text):
        t = text
        for d in display:
            t = t.replace(d, " ")
        for n, p in pats:
            if p.search(t):
                error("{0} shows the character name '{1}': \"{2}\" - abilities are freestanding, so "
                      "player-facing text must not name a character (file paths may)".format(where, n, text))
    return check


class _VisibleText(HTMLParser):
    """Everything a person can read or hear on the page: text outside <script>/<style>, plus the
    attributes a screen reader or a tooltip speaks."""
    SPOKEN = ("aria-label", "title", "placeholder", "alt")

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.skip, self.chunks, self.where = 0, [], []

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self.skip += 1
        a = dict(attrs)
        self.where.append(tag + ("#" + a["id"] if a.get("id") else "") +
                          ("." + a["class"].split()[0] if a.get("class") else ""))
        for k, v in attrs:
            if v and k in self.SPOKEN:
                self.chunks.append(("{0}[{1}]".format(self.where[-1], k), v))
        if tag in ("input", "meta", "br", "img", "link"):
            self.where.pop()

    def handle_endtag(self, tag):
        if tag in ("script", "style"):
            self.skip = max(0, self.skip - 1)
        if self.where:
            self.where.pop()

    def handle_data(self, data):
        if not self.skip and data.strip():
            self.chunks.append((" > ".join(self.where[-3:]), data))


# A file path, a source file or a console variable may carry a kit name: the path is the thing you
# overwrite and the cvar is what you type. Everything else on the page may not.
_PATHLIKE_RE = re.compile(r"\S*/\S*|\S+\.(?:cpp|h|wav|uasset|py|sh|html|txt)\b|\bTrace(?:\.\w+)+")


def check_page_names(page, leak):
    parser = _VisibleText()
    parser.feed(page)
    for where, text in parser.chunks:
        leak("the page text at {0}".format(where), _PATHLIKE_RE.sub(" ", text))


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def git_describe():
    try:
        sha = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                             text=True, timeout=10).stdout.strip()
        dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--", "Source", "Art/Sounds",
                                ":!Art/Sounds/sound-test.html", rel(__file__), rel(GENERATOR_PY)],
                               capture_output=True, text=True, timeout=30).stdout.strip()
        if not sha:
            return ""
        return sha + (" + uncommitted changes" if dirty else "")
    except Exception:                                   # pylint: disable=broad-except
        return ""


def main():
    ap = argparse.ArgumentParser(description="Render the self-contained sound-test page.")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output HTML (default Art/Sounds/sound-test.html)")
    ap.add_argument("--no-preview", action="store_true",
                    help="embed the two music beds whole instead of as previews (page grows ~26 MB)")
    ap.add_argument("--check", action="store_true", help="validate everything, write nothing")
    ap.add_argument("--allow-errors", action="store_true",
                    help="write the page even when a check fails (the errors are printed at its top)")
    args = ap.parse_args()

    print("Trace sound-test page")
    print("  events from    : {0}".format(rel(EVENTS_CPP)))
    print("  abilities from : {0}".format(rel(ABILITY_TYPES_CPP)))
    print("  wavs from      : {0}".format(rel(SOUND_DIR)))

    table = parse_event_table()
    print("  {0} rows in the C++ event table".format(len(table)))
    pre_existing = parse_pre_existing()
    unwired = parse_unwired()
    if unwired:
        print("  {0} event(s) SILENT since Demo 29: {1}".format(len(unwired), ", ".join(sorted(unwired))))
    abilities, kits = parse_abilities()
    print("  {0} abilities in GAbilityTable".format(len(abilities)))
    demo35_cvars = find_cvars("Trace.Demo35.")

    wavs = {}
    for path in sorted(glob.glob(os.path.join(SOUND_DIR, "**", "*.wav"), recursive=True)):
        stem = os.path.splitext(os.path.basename(path))[0]
        if stem in wavs:
            error("two WAVs share the stem '{0}': {1} and {2}".format(stem, rel(wavs[stem]), rel(path)))
            continue
        wavs[stem] = path
    print("  {0} WAVs on disk".format(len(wavs)))

    table_names = {r["name"] for r in table}
    for name in sorted(set(wavs) - table_names):
        error("Art/Sounds has '{0}' but TraceSoundEvents.cpp has no row for it - nothing can play it"
              .format(rel(wavs[name])))
    for name in sorted(table_names - set(wavs)):
        error("event '{0}' has no WAV under Art/Sounds - it is silent in game".format(name))
    for name in sorted(set(unwired) - table_names):
        error("Unwired() lists '{0}', which is not in the event table".format(name))

    calls, _harness = index_call_sites()

    # --- the ability mapping, validated against the code ------------------------------------------
    for ev in sorted(set(ABILITY_SOUNDS) - table_names):
        error("ABILITY_SOUNDS maps '{0}', which is not in the event table - remove the entry".format(ev))
    ability_rows = {}
    burst_types = fx_burst_types()
    for ev, spec in ABILITY_SOUNDS.items():
        if ev not in table_names:
            continue
        for key in sorted(set(spec) - SPEC_KEYS):
            error("ABILITY_SOUNDS '{0}' has '{1}', which this script does not read (a typo, or a key "
                  "from an older version) - nothing checks it".format(ev, key))
        for aid in spec["abilities"]:
            if aid not in abilities:
                error("ABILITY_SOUNDS maps '{0}' to ETraceAbilityId::{1}, which has no row in GAbilityTable"
                      .format(ev, aid))
        if not spec["abilities"] and not spec.get("retired"):
            error("ABILITY_SOUNDS '{0}' names no ability and is not marked retired".format(ev))
        sites, spans = [], []
        for (f, rx) in spec["sites"]:
            span = find_anchor_span(f, rx)
            if span is None:
                error("ABILITY_SOUNDS '{0}': the trigger /{1}/ is no longer in {2} - re-read the code "
                      "and fix the entry".format(ev, rx, f))
            else:
                spans.append(span)
                sites.append("{0}:{1}".format(span[0], span[2]))
        retired = spec.get("retired")
        checks, cover, any_cover, firm, walked = [], set(), set(), set(), []
        if retired:
            if find_anchor(*retired["gate"]) is None:
                error("ABILITY_SOUNDS '{0}': the switch /{1}/ is no longer in {2} - the retired path may be "
                      "live again".format(ev, retired["gate"][1], retired["gate"][0]))
            if retired["cvar"] not in demo35_cvars:
                error("ABILITY_SOUNDS '{0}' says '{1}' brings it back, but no such cvar is declared in "
                      "Source/".format(ev, retired["cvar"]))
            if retired["replaced_by"] not in abilities:
                error("ABILITY_SOUNDS '{0}': replaced_by '{1}' has no row in GAbilityTable".format(
                    ev, retired["replaced_by"]))
        elif not spec.get("paths"):
            error("ABILITY_SOUNDS '{0}' gives no path from an ability to its trigger".format(ev))
        elif spans:
            trigger = (spans[0][0], spans[0][1] - 1)
            for path in spec["paths"]:
                got = check_path(ev, path, trigger, abilities)
                if got is None:
                    continue
                checks.append(got["evidence"])
                walked += got["links"]
                cover |= got["cover"]
                if got["kind"] == "any":
                    any_cover |= got["cover"]
                else:
                    firm |= got["cover"]
            listed = set(spec["abilities"])
            if checks and cover != listed:
                error("ABILITY_SOUNDS '{0}' lists {1}, but its paths reach it from {2} - list exactly what "
                      "the code ties it to".format(ev, ", ".join(sorted(listed)), ", ".join(sorted(cover))))
            # a sound an FX burst carries: every Burst() of that type must be in the trigger's function
            if spans[0][0] != FXB:
                trig_fn, _head = function_at(spans[0][0], spans[0][1] - 1)
                for btype in burst_types.get(ev, []):
                    for urel, ui, where in find_uses(re.compile(r"Burst\s*\([^;]*ETraceFxBurstType::" + btype + r"\b")):
                        if where != trig_fn:
                            error("ABILITY_SOUNDS '{0}': ETraceFxBurstType::{1} is also burst at {2}:{3} ({4}) - "
                                  "another route to the sound".format(ev, btype, urel, ui + 1, where))
        if walked:
            check_callers(ev, spec, walked)
        for f, rx, why in spec.get("needs", []):
            if find_anchor(f, rx) is None:
                error("ABILITY_SOUNDS '{0}' rests on \"{1}\", and /{2}/ is no longer in {3}".format(ev, why, rx, f))
        # every gameplay site the grep finds must be a line the mapping cites
        for s in calls.get(ev, []):
            f, line = s.rsplit(":", 1)
            if not any(f == sf and lo <= int(line) <= hi for sf, lo, hi in spans):
                error("'{0}' is also played at {1}, which ABILITY_SOUNDS does not account for - check "
                      "which ability that is".format(ev, s))
        ability_rows[ev] = {"sites": sites, "checks": checks,
                            "unchecked": [a for a in spec["abilities"] if a in any_cover and a not in firm]}

    # --- rows -------------------------------------------------------------------------------------
    leak = name_leak_checker(kits, abilities)
    kit_prefix = re.compile(r"^(" + "|".join(re.escape(k) for k in sorted(kits, key=len, reverse=True)) +
                            r")[A-Z0-9]") if kits else None
    rows = []
    for row in table:
        name = row["name"]
        path = wavs.get(name)
        if path is None:
            continue
        try:
            nch, sr, frames, samples, bits = read_wav(path)
        except (wave.Error, EOFError, ValueError) as exc:
            error("{0} could not be read as a PCM WAV ({1}) - re-save it as 16-bit PCM".format(rel(path), exc))
            continue
        if bits != 16 or sr != 44100:
            warn("{0} is {1} Hz {2}-bit; the project's files are 44100 Hz 16-bit (it still imports)".format(
                rel(path), sr, bits))
        full_duration = frames / float(sr)
        full_mb = os.path.getsize(path) / (1024.0 * 1024.0)
        peak, rms = levels(samples)

        preview = (name in PREVIEW_STEMS) and not args.no_preview
        if preview:
            p_nch, p_sr, p_samples = make_preview(nch, sr, samples, PREVIEW_SECONDS, PREVIEW_DECIMATE)
            blob = wav_bytes(p_nch, p_sr, p_samples)
            duration = (len(p_samples) / p_nch) / float(p_sr)
            out_nch, out_sr, env_src = p_nch, p_sr, p_samples
        else:
            blob = wav_bytes(nch, sr, samples)
            duration = full_duration
            out_nch, out_sr, env_src = nch, sr, samples

        relpath = rel(path)
        spec = ABILITY_SOUNDS.get(name)
        indirect_note = ""
        if spec:
            sites = ability_rows[name]["sites"]
        else:
            sites = []
            seen = set()
            for s in calls.get(name, []):
                if s not in seen:
                    seen.add(s)
                    sites.append(s)
            if not sites:
                for rx, anchors, note in INDIRECT:
                    if rx.match(name):
                        for f, arx in anchors:
                            hit = find_anchor(f, arx)
                            if hit is None:
                                error("'{0}' is played through /{1}/ in {2}, which is gone - fix INDIRECT"
                                      .format(name, arx, f))
                            else:
                                sites.append(hit)
                        indirect_note = note

        # an ability-looking sound must be mapped
        looks_ability = ("/Abilities/" in "/" + relpath or
                         (kit_prefix is not None and kit_prefix.match(name) is not None) or
                         (sites and all(s.startswith("Source/Trace/Abilities/") for s in sites)))
        if looks_ability and not spec:
            error("'{0}' looks like an ability sound ({1}) but ABILITY_SOUNDS does not say which ability "
                  "plays it - read its call site and add an entry".format(name, relpath))

        if not sites and not (spec and spec.get("retired")):
            warn("'{0}' is NOT WIRED: nothing in Source/ plays it".format(name))

        also = []
        retired = None
        checks, unchecked_note = [], ""
        if spec:
            names = {aid: abilities[aid]["name"] for aid in spec["abilities"] if aid in abilities}
            label = spec["label"]
            when = spec["when"]
            for aid, disp in names.items():
                when = when.replace("{" + aid + "}", disp)
            if "{" in when:
                error("ABILITY_SOUNDS '{0}': unresolved placeholder in \"{1}\"".format(name, spec["when"]))
            if spec.get("retired"):
                r = spec["retired"]
                repl = abilities.get(r["replaced_by"], {}).get("name", r["replaced_by"])
                retired = {"cvar": r["cvar"], "replaced_by": repl}
                group, where = "retired", "Retired"
                switch = find_anchor(*r["gate"])
                if switch:
                    f, line = switch.rsplit(":", 1)
                    checks = ["switched off at {0}:{1}".format(os.path.basename(f), line)]
            else:
                primary = abilities.get(spec["abilities"][0])
                group = "ability:" + spec["abilities"][0]
                where = "{0} ({1})".format(primary["name"], primary["slot_label"]) if primary else "?"
                for aid in spec["abilities"][1:]:
                    if aid in abilities:
                        also.append("{0} ({1})".format(abilities[aid]["name"], abilities[aid]["slot_label"]))
                checks = ability_rows[name]["checks"]
                others = [abilities[a]["name"] for a in ability_rows[name]["unchecked"]
                          if a != spec["abilities"][0] and a in abilities]
                if others:
                    unchecked_note = ("Also plays with {0} picked: no ability check on that route."
                                      .format(" or ".join([", ".join(others[:-1]), others[-1]] if len(others) > 1 else others)))
        else:
            label, when = name, row["trigger"]
            group, where = None, None
            for gid, title, members, _note in STATIC_GROUPS + TAIL_GROUPS:
                if name in members:
                    group, where = gid, title
            if group is None:
                group, where = "other", "Other"

        leak("the label of '{0}'".format(name), label)
        leak("the description of '{0}'".format(name), when)
        if row["name"] in unwired:
            leak("the SILENT reason of '{0}'".format(name), unwired[name])
        for a in also:
            leak("an ability tag on '{0}'".format(name), a)
        leak("the note on '{0}'".format(name), unchecked_note)

        search = " ".join([name, label, when, relpath, where or "", " ".join(also),
                           "silent unwired" if name in unwired else "",
                           "retired legacy" if retired else "",
                           "not wired" if not sites else "",
                           "new added" if (pre_existing is not None and name not in pre_existing) else "original"]).lower()
        rows.append({
            "name": name, "side": row["side"], "label": label, "when": when, "where": where,
            "unwired": (unwired.get(name) or "no reason recorded in C++") if name in unwired else "",
            "retired": retired, "also": also,
            "new": pre_existing is not None and name not in pre_existing,
            "group": group, "relpath": relpath,
            "duration": duration, "full_duration": full_duration, "full_mb": full_mb,
            "channels": out_nch, "rate": out_sr, "peak": peak, "rms": rms, "preview": preview,
            "bits": bits, "src_rate": sr,
            "env": envelope(env_src, out_nch),
            "sites": [s.replace("Source/Trace/", "", 1) for s in sites],
            "checks": checks, "unchecked_note": unchecked_note, "indirect_note": indirect_note, "search": search,
            "b64": base64.b64encode(blob).decode("ascii"),
        })
        print("    {0:<18} {1:>6.2f}s  {2}  peak {3:>6}  {4:<22} {5}".format(
            name, duration, "mono  " if out_nch == 1 else "stereo", fmt_db(peak), where,
            "PREVIEW" if preview else ""))

    if not rows:
        raise SystemExit("no rows survived - nothing to render")

    # --- groups -------------------------------------------------------------------------------------
    by_group = {}
    for r in rows:
        by_group.setdefault(r["group"], []).append(r)

    def static_group(gid, title, order, note):
        # In the group's own listed order (shots before melee before deaths), not the C++ table's.
        members = sorted(by_group.get(gid, []), key=lambda r: order.index(r["name"]))
        return {"id": gid, "title": title, "note": html.escape(note), "count": len(members),
                "blocks": [{"rows": members}]} if members else None

    groups = [g for g in (static_group(gid, t, m, n) for gid, t, m, n in STATIC_GROUPS) if g]

    slot_order = {"Movement": 0, "Passive": 1, "Activated": 2}
    ordered = sorted(abilities.values(), key=lambda a: (slot_order.get(a["slot"], 9), a["order"]))
    blocks, silent_abilities = [], []
    for a in ordered:
        members = by_group.get("ability:" + a["id"], [])
        also_here = [r for r in rows if a["id"] in ABILITY_SOUNDS.get(r["name"], {}).get("abilities", [])[1:]]

        def shared(r, aid=a["id"]):
            # A sound this ability reaches only by a route with no ability check says so.
            unchecked = aid in ability_rows.get(r["name"], {}).get("unchecked", [])
            return '<a href="#r-{0}">{1}</a>{2}'.format(html.escape(r["name"]), html.escape(r["label"]),
                                                         " (no ability check)" if unchecked else "")
        if not members:
            if also_here:
                blocks.append({"title": a["name"], "slot": a["slot_label"], "anchor": a["id"], "rows": [],
                               "note": "Shares " + ", ".join(shared(r) for r in also_here) +
                                       "; no sound of its own."})
            else:
                silent_abilities.append(a)
            continue
        note = ""
        if also_here:
            note = "Also plays " + ", ".join(shared(r) for r in also_here) + "."
        blocks.append({"title": a["name"], "slot": a["slot_label"], "anchor": a["id"], "rows": members,
                       "note": note})
    ability_count = sum(len(b["rows"]) for b in blocks)
    if blocks:
        silent_txt = ""
        if silent_abilities:
            silent_txt = " No sound of their own: " + ", ".join(
                "{0} ({1})".format(html.escape(a["name"]), html.escape(a["slot_label"].lower()))
                for a in silent_abilities) + "."
        groups.append({"id": "abilities", "title": "Abilities", "count": ability_count,
                       "note": "By ability and slot (names from GAbilityTable). Each row cites the line that "
                               "plays it and the ability check on each route to it, re-read from the code on "
                               "every build." + silent_txt,
                       "blocks": [b for b in blocks if b["rows"]] + [b for b in blocks if not b["rows"]]})
    if by_group.get("retired"):
        groups.append({"id": "retired", "title": "Retired", "count": len(by_group["retired"]),
                       "note": "Only a feature Demo 35 retired plays these. Imported, never heard, unless "
                               "the row&rsquo;s switch is on.",
                       "blocks": [{"rows": by_group["retired"]}]})
    groups += [g for g in (static_group(gid, t, m, n) for gid, t, m, n in TAIL_GROUPS) if g]
    if by_group.get("other"):
        groups.append({"id": "other", "title": "Other", "count": len(by_group["other"]),
                       "note": "Not in any group above - add it to one in Scripts/generate_sound_page.py.",
                       "blocks": [{"rows": by_group["other"]}]})
        warn("{0} sound(s) fall in no group: {1}".format(
            len(by_group["other"]), ", ".join(r["name"] for r in by_group["other"])))
    for g in groups:
        leak("the heading '{0}'".format(g["title"]), g["title"])
        leak("the note under '{0}'".format(g["title"]), re.sub(r"<[^>]+>", " ", g.get("note") or ""))

    placed = sum(len(b["rows"]) for g in groups for b in g["blocks"])
    if placed != len(rows):
        error("{0} rows but {1} placed on the page - a sound is missing or doubled".format(len(rows), placed))
    print("  ability sounds: {0} across {1} abilities; {2} retired; abilities with no sound: {3}".format(
        ability_count, len([b for b in blocks if b["rows"]]), len(by_group.get("retired", [])),
        ", ".join(a["name"] for a in silent_abilities) or "none"))

    sha = git_describe()
    built = datetime.datetime.now().strftime("%Y-%m-%d %H:%M") + (" from " + sha if sha else "")
    ctx = {"rows": rows, "groups": groups, "built": built, "size_note": "",
           "errors": list(_errors), "new_count": sum(1 for r in rows if r["new"]),
           "nosound": [[a["name"], a["slot_label"]] for a in silent_abilities]}

    # Built before deciding, so the finished page's own text can be checked for character names
    # (headings, notes, screen-reader labels - everything but paths and cvars).
    page = build_page(ctx)
    check_page_names(page, leak)
    ctx["size_note"] = "{0} sounds, {1:.1f} MB, entirely offline.".format(
        len(rows), len(page.encode("utf-8")) / (1024.0 * 1024.0))
    ctx["errors"] = list(_errors)
    page = build_page(ctx)

    if _errors and not args.allow_errors:
        print("")
        print("  {0} ERROR(S) - nothing written. Fix them, or pass --allow-errors to write the page "
              "with the errors printed on it.".format(len(_errors)))
        return 1
    if args.check:
        print("")
        print("  --check: {0} rows OK, {1} warning(s); nothing written.".format(len(rows), len(_warnings)))
        return 0

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(page)

    print("")
    print("  wrote {0}".format(rel(out)))
    print("  {0} rows, {1:.1f} MB".format(len(rows), len(page.encode("utf-8")) / (1024.0 * 1024.0)))
    for g in groups:
        print("    {0:<28} {1}".format(g["title"], g["count"]))
    if _errors:
        print("  {0} ERROR(S) - written anyway (--allow-errors); they are printed on the page".format(len(_errors)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
