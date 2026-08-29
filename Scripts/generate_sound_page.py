# =============================================================================
# Trace - generate_sound_page.py        (DEMO 29 item 10, owner's deliverable)
#
#   "Can you create a list of all the sounds in the game with playable samples
#    next to each action, so that I can go through and test every single one
#    added and replace specific ones?"
#
# Renders ONE self-contained HTML page - no server, no network, no assets beside
# it - with a play button for every sound the game can make, grouped the way a
# person auditions sounds (combat, movement, the objective, each character's
# abilities, UI, music), and a "replace this one" mark + note per row that
# survives a page reload.
#
#   python3 Scripts/generate_sound_page.py
#       -> Art/Sounds/sound-test.html
#
# Re-run it whenever a WAV changes; the page is a build product and nothing
# reads it back.
#
# -----------------------------------------------------------------------------
# WHERE EVERY COLUMN COMES FROM, SO NONE OF IT CAN GO STALE SILENTLY
# -----------------------------------------------------------------------------
#   event name / side / trigger  Source/Trace/Audio/TraceSoundEvents.cpp, the
#                                Table[] literal, parsed. That file is the
#                                authority on which machines hear a sound, so
#                                the page cannot disagree with the game about it.
#   the WAV                      Art/Sounds/**.wav, globbed recursively and
#                                keyed by STEM, exactly as Scripts/import_sounds.py
#                                keys the bank. Same rule, so the page shows the
#                                same file the engine imports.
#   duration / peak / RMS        measured from the WAV, here, now.
#   call site                    grepped out of Source/ - the file:line that
#                                actually calls TraceAudio::* with this event.
#                                An event with NO call site is printed as such,
#                                because "the WAV exists" and "anything plays
#                                it" are different facts.
#
# A stem on disk with no row in the C++ table, and a row with no WAV, are both
# reported as errors rather than quietly dropped: either one is a sound the
# owner would go looking for and not find.
#
# -----------------------------------------------------------------------------
# THE TWO MUSIC BEDS ARE PREVIEWS, AND THE PAGE SAYS SO ON THE ROW
# -----------------------------------------------------------------------------
# MusicTitle is 64 s / 11.3 MB and AmbienceMatch is 48 s / 8.5 MB. Embedded
# whole they would be 26 MB of base64 on their own and the page would take
# seconds to open. Each is embedded as the first PREVIEW_SECONDS, decimated to
# 22.05 kHz (two-tap average first, so the decimation is not raw aliasing).
# The row carries the full duration, the full path and the word PREVIEW; the
# shipping file is untouched.
# =============================================================================
import argparse
import array
import base64
import datetime
import glob
import html
import io
import math
import os
import re
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOUND_DIR = os.path.join(ROOT, "Art", "Sounds")
EVENTS_CPP = os.path.join(ROOT, "Source", "Trace", "Audio", "TraceSoundEvents.cpp")
GENERATOR_PY = os.path.join(HERE, "generate_sounds.py")
SOURCE_DIR = os.path.join(ROOT, "Source")
DEFAULT_OUT = os.path.join(SOUND_DIR, "sound-test.html")

# Stems embedded as a short preview instead of whole. See the header.
PREVIEW_STEMS = {"MusicTitle", "AmbienceMatch"}
PREVIEW_SECONDS = 12.0
PREVIEW_DECIMATE = 2          # 44100 -> 22050

# The ten characters, spelled once (Source/Trace/Abilities/TraceAbilityTypes.h).
CHARACTERS = ["Rocco", "Chut", "Mace", "Oyster", "X", "Roxie", "Elle",
              "Slimeball", "Mortimer", "Lily"]

# Files whose mention of an event name is ABOUT the audio system rather than a
# trigger: the table itself, the verifiers, the console harnesses. A call site
# found only in these is not a call site.
CALLSITE_EXCLUDE = (
    os.path.join("Audio", "TraceSoundEvents."),
    os.path.join("Audio", "TraceAudioVerify."),
    os.path.join("Audio", "TraceAudioWatch."),
    os.path.join("Audio", "TraceAudioInteg."),
    os.path.join("Audio", "TraceAudioV29Integ."),
    os.path.join("Audio", "TraceAudioLoudness."),
    os.path.join("Audio", "TraceGunLadderVerify."),
    os.path.join("Audio", "TraceAudio."),
)

_problems = []


def problem(msg):
    _problems.append(msg)
    print("  !! " + msg)


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
    with open(EVENTS_CPP, "r", encoding="utf-8") as fh:
        text = fh.read()

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
    return rows


# ---------------------------------------------------------------------------
# 1b. the unwire list (DEMO 29 items 9 and 11)
# ---------------------------------------------------------------------------

def parse_unwired():
    """
    {event: reason} for the events TraceSoundEvents::Unwired() lists.

    An unwired event is still declared, still imported and still resolvable - it is simply not
    allowed to sound. The page MUST say so on the row: auditioning a clip and then wondering why
    the game never makes that noise is precisely the confusion this list creates, and the owner
    is the person it would happen to.
    """
    with open(EVENTS_CPP, "r", encoding="utf-8") as fh:
        text = fh.read()

    # ONE table in the C++ (FTraceUnwiredRow), so one parse here. Each row is
    # `{ EventName, TEXT("a" "b") },`; the reason is read as "every string literal in the row,
    # concatenated" rather than as TEXT("...") because the C++ wraps only the FIRST fragment and
    # lets the compiler splice the continuation lines. A TEXT()-shaped regex silently reads those
    # as an empty reason, which would drop the SILENT badge off the row this parse exists to mark.
    names, reasons = [], {}
    m = re.search(r"static const FTraceUnwiredRow Table\[\]\s*=\s*\{(.*?)\n\t\t\};", text, re.S)
    if m:
        for entry in re.finditer(r"\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*,(.*?)\}\s*,", m.group(1), re.S):
            name = entry.group(1)
            pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', entry.group(2))
            names.append(name)
            joined = "".join(pieces).replace('\\"', '"').strip()
            if joined:
                reasons[name] = joined
    else:
        problem("could not find the FTraceUnwiredRow table in TraceSoundEvents.cpp - "
                "no sound will be shown as SILENT even if the game refuses to play it")

    for n in names:
        if n not in reasons:
            problem("'{0}' is on TraceSoundEvents::Unwired() but UnwiredReason() gave no "
                    "sentence for it".format(n))
    return {n: reasons.get(n, "unwired by Demo 29 (no reason recorded in C++)") for n in names}


# ---------------------------------------------------------------------------
# 1c. which sounds are NEW
# ---------------------------------------------------------------------------

def parse_pre_existing():
    """
    The 28 stems that existed before the release overhaul, read out of
    Scripts/generate_sounds.py's EXISTING_STEMS.

    This is the owner's actual question - "test every single one ADDED" - so the page has to be
    able to tell the twenty-eight sounds they already knew from the forty-three the overhaul
    invented. EXISTING_STEMS is the right source because it is not a note about history: it is the
    live freeze list the generator refuses to write over, so if it were wrong the generator would
    already have overwritten something.
    """
    try:
        with open(GENERATOR_PY, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        problem("Scripts/generate_sounds.py is missing - every sound will be shown as pre-existing")
        return set()

    m = re.search(r"EXISTING_STEMS\s*=\s*frozenset\(\[(.*?)\]\)", text, re.S)
    if not m:
        problem("could not find EXISTING_STEMS in Scripts/generate_sounds.py - "
                "every sound will be shown as pre-existing")
        return set()
    return set(re.findall(r'"([A-Za-z0-9_]+)"', m.group(1)))


# ---------------------------------------------------------------------------
# 2. call sites: who actually plays this event
# ---------------------------------------------------------------------------

def index_call_sites():
    """
    {event: ['Path/File.cpp:123', ...]} -- every place GAMEPLAY code names the event.

    Deliberately NOT "the line that also says TraceAudio::". Half the real triggers are
    indirect: the FX burst's type->event switch (TraceFxBurst.cpp), a ternary that picks
    backstab-or-front two lines above the call, `Music->Play(...)` on the music subsystem.
    Requiring the call and the name on ONE line reported thirteen genuinely-wired events as
    dead, which is worse than the occasional harness array slipping through.

    The audio system's own files are excluded (CALLSITE_EXCLUDE) because a name appearing in
    the table, a verifier or a console command is not a thing that happens in a match.
    """
    hits = {}
    pattern = re.compile(r"TraceSoundEvents::([A-Za-z0-9_]+)")
    for dirpath, _dirs, files in os.walk(SOURCE_DIR):
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, ROOT)
            if any(x in full for x in CALLSITE_EXCLUDE):
                continue
            try:
                with open(full, "r", encoding="utf-8", errors="replace") as fh:
                    lines = fh.readlines()
            except OSError:
                continue
            for i, line in enumerate(lines, 1):
                stripped = line.lstrip()
                if stripped.startswith("//") or stripped.startswith("*"):
                    continue          # a comment naming the event is not a trigger
                for m in pattern.finditer(line):
                    hits.setdefault(m.group(1), []).append("{0}:{1}".format(rel, i))
    return hits


# ---------------------------------------------------------------------------
# 3. the WAVs
# ---------------------------------------------------------------------------

def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("{0}: expected 16-bit, got {1}".format(path, w.getsampwidth() * 8))
        nch = w.getnchannels()
        sr = w.getframerate()
        frames = w.getnframes()
        raw = w.readframes(frames)
    samples = array.array("h")
    samples.frombytes(raw)
    if sys.byteorder == "big":
        samples.byteswap()
    return nch, sr, frames, samples


def levels(samples):
    """(peak dBFS, rms dBFS). Empty -> (-inf, -inf)."""
    if not samples:
        return float("-inf"), float("-inf")
    try:
        import numpy as np
        a = np.frombuffer(samples.tobytes(), dtype="<i2").astype("float64") / 32768.0
        peak = float(np.max(np.abs(a)))
        rms = float(math.sqrt(float(np.mean(a * a))))
    except ImportError:
        peak_i = max(max(samples), -min(samples))
        peak = peak_i / 32768.0
        acc = 0.0
        for s in samples:
            v = s / 32768.0
            acc += v * v
        rms = math.sqrt(acc / len(samples))
    to_db = lambda x: (20.0 * math.log10(x)) if x > 1e-12 else float("-inf")
    return to_db(peak), to_db(rms)


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
# 4. grouping
# ---------------------------------------------------------------------------

COMBAT = ["PistolShoot1", "PistolShoot2", "PistolShoot3", "PistolShoot4", "SmgShoot1",
          "Reload", "DryFire", "WeaponSwitch", "Bodyshot", "Headshot", "ShieldBlock",
          "MeleeSwing", "MeleeHit", "MeleeBackstab", "Parry",
          "DamageTaken", "Kill", "DeathBurst", "Respawn"]
MOVEMENT = ["Jump", "WallJump", "Dash"] + ["Step{0}".format(i) for i in range(1, 12)]
OBJECTIVE = ["CorePickup", "CoreTurnover", "Goal"]
UI = ["ButtonPress", "UIHover", "UIBack", "UIDeny", "CountdownTick", "CountdownGo"]
MUSIC = ["MusicTitle", "AmbienceMatch", "StingerVictory", "StingerDefeat"]

GROUP_NOTES = {
    "Combat": "Shooting, melee, and what happens when somebody dies. Kill is the "
              "killer's own confirmation and is the sound this project has had since Demo 25.",
    "Movement": "Jump, wall-jump, dash, and the eleven footstep clips the game picks between. "
                "Footsteps carry their own volume knob and are deliberately far quieter than everything else.",
    "The Core": "The objective: picking it up, losing it, scoring with it.",
    "UI and the kickoff countdown": "Menu sounds, and the two 2D kickoff cues.",
    "Music and ambience": "The two long beds and the two match-end stingers. "
                          "The beds below are PREVIEWS, not the shipping files - see the note on each row.",
}


def group_of(name):
    if name in COMBAT:
        return "Combat"
    if name in MOVEMENT:
        return "Movement"
    if name in OBJECTIVE:
        return "The Core"
    if name in UI:
        return "UI and the kickoff countdown"
    if name in MUSIC:
        return "Music and ambience"
    for c in CHARACTERS:
        if name.startswith(c):
            return "Abilities - " + c
    return "Unsorted"


def group_order(rows):
    order = ["Combat", "Movement", "The Core"]
    order += ["Abilities - " + c for c in CHARACTERS]
    order += ["UI and the kickoff countdown", "Music and ambience", "Unsorted"]
    present = {r["group"] for r in rows}
    return [g for g in order if g in present]


# ---------------------------------------------------------------------------
# 5. the page
# ---------------------------------------------------------------------------

CSS = """
:root{
  --bg:#12141a; --panel:#191c24; --panel2:#1f232d; --line:#2b303c;
  --ink:#e8eaf0; --dim:#98a0b4; --accent:#6fd3c7; --accent2:#f0a35e;
  --world:#7aa2f7; --client:#c3a0f0; --flag:#f0a35e;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
     font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
header{padding:28px 28px 18px;border-bottom:1px solid var(--line);background:var(--panel)}
h1{margin:0 0 6px;font-size:24px;letter-spacing:.2px}
.sub{color:var(--dim);font-size:13px;max-width:74ch}
.bar{display:flex;flex-wrap:wrap;gap:10px;align-items:center;padding:14px 28px;
     border-bottom:1px solid var(--line);background:var(--panel2);position:sticky;top:0;z-index:5}
.bar input[type=search]{flex:1 1 260px;min-width:200px;padding:8px 10px;border-radius:7px;
     border:1px solid var(--line);background:#12151c;color:var(--ink);font-size:13px}
button{font:inherit;cursor:pointer;border-radius:7px;border:1px solid var(--line);
     background:#252a35;color:var(--ink);padding:7px 12px}
button:hover{background:#2e3441}
button.primary{background:var(--accent);color:#0e1116;border-color:var(--accent);font-weight:600}
.count{color:var(--dim);font-size:12px}
.toggle{display:flex;gap:6px;align-items:center;font-size:12.5px;color:var(--dim);
        white-space:nowrap;cursor:pointer}
.toggle input{width:15px;height:15px;accent-color:var(--accent)}
main{padding:0 28px 60px}
section{margin:26px 0 0}
h2{font-size:17px;margin:0 0 4px;padding-top:14px;border-top:1px solid var(--line)}
.gnote{color:var(--dim);font-size:12.5px;margin:0 0 10px;max-width:88ch}
table{width:100%;border-collapse:collapse}
th{text-align:left;font-size:11px;text-transform:uppercase;letter-spacing:.6px;
   color:var(--dim);font-weight:600;padding:6px 8px;border-bottom:1px solid var(--line)}
td{padding:9px 8px;border-bottom:1px solid #232833;vertical-align:top}
tr.flagged td{background:#2a2119}
.play{width:34px;height:34px;padding:0;border-radius:50%;font-size:13px;line-height:1}
.play.playing{background:var(--accent);color:#0e1116;border-color:var(--accent)}
.ev{font-weight:600;font-size:14px}
.side{display:inline-block;font-size:10px;padding:1px 6px;border-radius:4px;margin-left:7px;
      vertical-align:2px;border:1px solid}
.side.world{color:var(--world);border-color:#33406a}
.side.client{color:var(--client);border-color:#4a3d68}
.trig{color:var(--dim);font-size:12.5px}
.path{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11.5px;color:#8b93a8}
.call{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11px;color:#6f7891}
.nocall{color:var(--accent2)}
.num{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;
     white-space:nowrap;color:#b9c0d0}
.meter{height:5px;width:74px;background:#242936;border-radius:3px;overflow:hidden;margin-top:5px}
.meter i{display:block;height:100%;background:linear-gradient(90deg,#5ec8b6,#f0a35e)}
.mark{display:flex;gap:6px;align-items:flex-start}
.mark input[type=checkbox]{width:17px;height:17px;margin-top:3px;accent-color:var(--flag)}
.mark textarea{width:100%;min-width:150px;height:34px;resize:vertical;border-radius:6px;
     border:1px solid var(--line);background:#12151c;color:var(--ink);
     font:12px/1.4 inherit;padding:5px 7px}
.prev{display:inline-block;font-size:10px;padding:1px 6px;border-radius:4px;margin-left:7px;
      vertical-align:2px;color:var(--accent2);border:1px solid #5a4327}
.new{display:inline-block;font-size:10px;padding:1px 6px;border-radius:4px;margin-left:7px;
     vertical-align:2px;color:#7fd8a0;border:1px solid #2f5e42;letter-spacing:.4px}
.orig{display:inline-block;font-size:10px;padding:1px 6px;border-radius:4px;margin-left:7px;
      vertical-align:2px;color:#78809a;border:1px solid #333a4a;letter-spacing:.4px}
.unw{display:inline-block;font-size:10px;padding:1px 6px;border-radius:4px;margin-left:7px;
     vertical-align:2px;color:#f0736e;border:1px solid #6a3330;letter-spacing:.4px}
tr.silent .ev{opacity:.75}
.unwreason{display:block;margin-top:4px;font-size:12px;color:#d59a8f}
.unwreason code{background:#2a1f1e;padding:1px 5px;border-radius:4px;font-size:11.5px}
.how{background:#171d22;border:1px solid #24313a;border-left:3px solid var(--accent);
     border-radius:8px;padding:12px 16px;margin:20px 0 4px;font-size:13px;color:#c3ccd8;
     max-width:110ch}
.how code{background:#0e1116;padding:2px 6px;border-radius:4px;font-size:12px;color:var(--accent)}
.warn{background:#2a1c1c;border:1px solid #573030;border-radius:8px;padding:12px 14px;margin:18px 0}
#out{width:100%;height:220px;margin-top:10px;border-radius:8px;border:1px solid var(--line);
     background:#0e1116;color:var(--ink);font:12px/1.5 ui-monospace,Menlo,monospace;padding:10px}
dialog{background:var(--panel);color:var(--ink);border:1px solid var(--line);
       border-radius:12px;max-width:820px;width:92vw;padding:20px}
dialog::backdrop{background:rgba(0,0,0,.6)}
@media (max-width:820px){
  .hide-narrow{display:none}
  main,header,.bar{padding-left:14px;padding-right:14px}
}
"""

JS = r"""
const KEY = 'trace-sound-test-marks';
let marks = {};
try { marks = JSON.parse(localStorage.getItem(KEY) || '{}'); } catch (e) { marks = {}; }
let current = null;

function save(){ try { localStorage.setItem(KEY, JSON.stringify(marks)); } catch(e){} }

function refreshRow(tr){
  const ev = tr.dataset.event;
  const m = marks[ev];
  tr.classList.toggle('flagged', !!(m && m.flag));
}

// rewind() and not `a.currentTime = 0`: every <audio> here is preload="none", so until something
// has been played its readyState is 0 and assigning currentTime throws InvalidStateError. Stop
// pressed before anything played would then throw 71 times and leave the buttons stuck.
function rewind(a){
  if (a.readyState > 0 && a.currentTime !== 0){ try { a.currentTime = 0; } catch (e) {} }
}

function stopAll(){
  document.querySelectorAll('audio').forEach(a => { if (!a.paused) a.pause(); rewind(a); });
  document.querySelectorAll('.play.playing').forEach(b => {
    b.classList.remove('playing'); b.textContent = '▶';
  });
  current = null;
}

function toggle(btn){
  const tr = btn.closest('tr');
  const a  = tr.querySelector('audio');
  if (current === a && !a.paused){ stopAll(); return; }
  stopAll();
  current = a;
  btn.classList.add('playing'); btn.textContent = '■';
  rewind(a);
  a.play().catch(() => { btn.classList.remove('playing'); btn.textContent = '▶'; current = null; });
  a.onended = () => { btn.classList.remove('playing'); btn.textContent = '▶'; current = null; };
}

function onFlag(cb){
  const tr = cb.closest('tr'); const ev = tr.dataset.event;
  marks[ev] = marks[ev] || {};
  marks[ev].flag = cb.checked;
  if (!marks[ev].flag && !marks[ev].note) delete marks[ev];
  save(); refreshRow(tr); updateCount();
}

function onNote(ta){
  const tr = ta.closest('tr'); const ev = tr.dataset.event;
  marks[ev] = marks[ev] || {};
  marks[ev].note = ta.value;
  if (!marks[ev].flag && !marks[ev].note) delete marks[ev];
  save(); updateCount();
}

function updateCount(){
  const n = Object.values(marks).filter(m => m.flag).length;
  document.getElementById('count').textContent =
    n ? (n + ' marked for replacement') : 'nothing marked yet';
}

function filter(){
  const q = document.getElementById('q').value.trim().toLowerCase();
  const onlyNew = document.getElementById('onlynew').checked;
  document.querySelectorAll('tbody tr').forEach(tr => {
    const hit = (!q || tr.dataset.search.includes(q)) &&
                (!onlyNew || tr.classList.contains('isnew'));
    tr.style.display = hit ? '' : 'none';
  });
  document.querySelectorAll('section').forEach(s => {
    const any = [...s.querySelectorAll('tbody tr')].some(tr => tr.style.display !== 'none');
    s.style.display = any ? '' : 'none';
  });
}

function report(){
  const rows = [...document.querySelectorAll('tbody tr')];
  const lines = ['TRACE - sounds marked for replacement', ''];
  let n = 0;
  rows.forEach(tr => {
    const ev = tr.dataset.event; const m = marks[ev];
    if (!m || !m.flag) return;
    n++;
    lines.push('- ' + ev + '   ' + tr.dataset.path);
    lines.push('      trigger: ' + tr.dataset.trigger);
    if (m.note) lines.push('      note:    ' + m.note);
  });
  if (!n) lines.push('(nothing marked)');
  lines.push('');
  lines.push('Replace a sound: drop the new WAV over the path above, keeping the file name,');
  lines.push('then run  ./Scripts/import-sounds.sh --only <EventName>   - no code change needed.');
  const out = document.getElementById('out');
  out.value = lines.join('\n');
  document.getElementById('dlg').showModal();
  out.focus(); out.select();
}

function copyOut(){
  const out = document.getElementById('out');
  out.select();
  navigator.clipboard.writeText(out.value).catch(() => { try { document.execCommand('copy'); } catch(e){} });
}

function clearAll(){
  if (!confirm('Clear every mark and note on this page?')) return;
  marks = {}; save();
  document.querySelectorAll('tbody tr').forEach(tr => {
    tr.querySelector('input[type=checkbox]').checked = false;
    tr.querySelector('textarea').value = '';
    refreshRow(tr);
  });
  updateCount();
}

window.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('tbody tr').forEach(tr => {
    const m = marks[tr.dataset.event];
    if (m){
      if (m.flag) tr.querySelector('input[type=checkbox]').checked = true;
      if (m.note) tr.querySelector('textarea').value = m.note;
    }
    refreshRow(tr);
  });
  updateCount();
  document.addEventListener('keydown', e => { if (e.key === 'Escape') stopAll(); });
});
"""


def fmt_db(v):
    return "-inf" if v == float("-inf") else "{0:+.1f}".format(v)


def build_page(rows, generated_at, page_bytes_note):
    total = len(rows)
    new_count = sum(1 for r in rows if r["new"])
    new_count_label = "{0} sound{1}".format(new_count, "" if new_count == 1 else "s")
    parts = []
    A = parts.append
    A("<title>Trace Sound Test</title>")
    A("<style>{0}</style>".format(CSS))
    A("<header>")
    A("<h1>Trace &mdash; every sound in the game</h1>")
    A('<p class="sub">{0} sound events, each with the clip the engine actually plays, where it '
      'fires in a match, and the file to drop a replacement over. Everything is embedded in this '
      'one file &mdash; no server, no network, no folder beside it. Tick <b>replace</b> on anything '
      'you want re-done, type a note, then press <b>Show my list</b>. Marks are saved in this '
      'browser.</p>'.format(total))
    silent = [r["name"] for r in rows if r["unwired"]]
    if silent:
        A('<p class="sub" style="margin-top:8px;color:#d59a8f"><b>{0} sound{1} marked SILENT</b> '
          '({2}). Demo 29 switched {3} off because of a bug in when {3} fired &mdash; the clip is '
          'still here and still playable below, it just does not sound in a match right now. '
          'Each row says why.</p>'.format(
              len(silent), "" if len(silent) == 1 else "s", ", ".join(silent),
              "it" if len(silent) == 1 else "them"))
    A('<p class="sub" style="margin-top:8px">Generated {0} by '
      '<code>Scripts/generate_sound_page.py</code>. {1}</p>'.format(html.escape(generated_at),
                                                                    html.escape(page_bytes_note)))
    A("</header>")

    A('<div class="bar">')
    A('<input type="search" id="q" placeholder="filter by name, trigger, file or character&hellip;" '
      'oninput="filter()">')
    A('<label class="toggle"><input type="checkbox" id="onlynew" onchange="filter()"> '
      'only the {0} the overhaul added</label>'.format(new_count_label))
    A('<button onclick="stopAll()">Stop</button>')
    A('<button class="primary" onclick="report()">Show my list</button>')
    A('<button onclick="clearAll()">Clear marks</button>')
    A('<span class="count" id="count"></span>')
    A("</div>")

    A("<main>")
    A('<div class="how"><b>To replace one:</b> drop your new WAV over the path in the '
      '<i>File</i> column, keeping the file name, then run '
      '<code>./Scripts/import-sounds.sh --only &lt;EventName&gt;</code>. No code change and no '
      'rebuild &mdash; the event name is the file name. 44.1&nbsp;kHz 16-bit; mono for anything that '
      'happens at a place in the world, stereo for UI and music. '
      'Re-run <code>python3 Scripts/generate_sound_page.py</code> to rebuild this page.</div>')
    if _problems:
        A('<div class="warn"><b>Generator warnings</b><ul>')
        for p in _problems:
            A("<li>{0}</li>".format(html.escape(p)))
        A("</ul></div>")

    for grp in group_order(rows):
        members = [r for r in rows if r["group"] == grp]
        A("<section>")
        A("<h2>{0} <span class='count'>&nbsp;{1} sound{2}</span></h2>".format(
            html.escape(grp), len(members), "" if len(members) == 1 else "s"))
        note = GROUP_NOTES.get(grp)
        if note is None and grp.startswith("Abilities - "):
            note = "Everything {0} makes.".format(grp.split(" - ", 1)[1])
        if note:
            A('<p class="gnote">{0}</p>'.format(html.escape(note)))
        A("<table><thead><tr>")
        A("<th></th><th>Event &amp; when it fires</th><th class='hide-narrow'>File</th>"
          "<th>Length / level</th><th>Replace?</th>")
        A("</tr></thead><tbody>")
        for r in members:
            search = " ".join([r["name"], r["trigger"], r["relpath"], grp,
                               "silent unwired" if r["unwired"] else "",
                               "new added" if r["new"] else "original"]).lower()
            A("<tr class=\"{4}\" data-event=\"{0}\" data-path=\"{1}\" data-trigger=\"{2}\" "
              "data-search=\"{3}\">".format(
                html.escape(r["name"], quote=True), html.escape(r["relpath"], quote=True),
                html.escape(r["trigger"], quote=True), html.escape(search, quote=True),
                " ".join(filter(None, ["silent" if r["unwired"] else "",
                                       "isnew" if r["new"] else "isold"]))))
            A('<td><button class="play" onclick="toggle(this)" '
              'aria-label="play {0}">&#9654;</button>'
              '<audio preload="none" src="data:audio/wav;base64,{1}"></audio></td>'.format(
                  html.escape(r["name"], quote=True), r["b64"]))

            side_cls = "world" if r["side"] == "game-side" else "client"
            prev = '<span class="prev">PREVIEW</span>' if r["preview"] else ""
            unw = '<span class="unw">SILENT</span>' if r["unwired"] else ""
            neu = ('<span class="new">NEW</span>' if r["new"]
                   else '<span class="orig">ORIGINAL</span>')
            unw_line = ('<br><span class="unwreason">Not currently played: {0}. '
                        'The WAV, the asset and the trigger are all still here &mdash; '
                        '<code>Trace.Audio.UnwiredEvents 0</code> turns it back on.</span>'
                        .format(html.escape(r["unwired"]))) if r["unwired"] else ""
            A('<td><span class="ev">{0}</span>{8}'
              '<span class="side {1}">{2}</span>{3}{4}<br>'
              '<span class="trig">{5}</span>{6}{7}</td>'.format(
                  html.escape(r["name"]), side_cls, r["side"], prev, unw,
                  html.escape(r["trigger"]), unw_line,
                  ('<br><span class="call">' + html.escape(r["call"]) + '</span>')
                  if r["call"] else
                  '<br><span class="call nocall">nothing in Source/ names this event &mdash; '
                  'it cannot fire in a match</span>', neu))

            A('<td class="hide-narrow"><span class="path">{0}</span></td>'.format(
                html.escape(r["relpath"])))

            bar = 0.0 if r["peak"] == float("-inf") else max(0.0, min(1.0, (r["peak"] + 40.0) / 40.0))
            A('<td><span class="num">{0:.2f} s &middot; {1} &middot; {2} Hz</span><br>'
              '<span class="num">peak {3} dBFS &middot; RMS {4} dBFS</span>'
              '<div class="meter"><i style="width:{5:.0f}%"></i></div>{6}</td>'.format(
                  r["duration"], "mono" if r["channels"] == 1 else "stereo", r["rate"],
                  fmt_db(r["peak"]), fmt_db(r["rms"]), bar * 100.0,
                  ('<br><span class="num" style="color:#f0a35e">full file {0:.1f} s, {1:.1f} MB</span>'
                   .format(r["full_duration"], r["full_mb"])) if r["preview"] else ""))

            A('<td><div class="mark"><input type="checkbox" onchange="onFlag(this)" '
              'aria-label="mark {0} for replacement">'
              '<textarea placeholder="what is wrong with it?" oninput="onNote(this)"></textarea>'
              "</div></td>".format(html.escape(r["name"], quote=True)))
            A("</tr>")
        A("</tbody></table></section>")

    A("</main>")
    A('<dialog id="dlg"><h2 style="border:0;padding:0">Sounds you marked</h2>'
      '<textarea id="out" readonly></textarea>'
      '<div style="margin-top:12px;display:flex;gap:10px">'
      '<button class="primary" onclick="copyOut()">Copy</button>'
      '<button onclick="document.getElementById(\'dlg\').close()">Close</button></div></dialog>')
    A("<script>{0}</script>".format(JS))
    return "\n".join(parts)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Render the self-contained sound-test page.")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output HTML (default Art/Sounds/sound-test.html)")
    ap.add_argument("--no-preview", action="store_true",
                    help="embed the two music beds whole instead of as previews (page grows ~26 MB)")
    args = ap.parse_args()

    print("Trace sound-test page")
    print("  events from : {0}".format(os.path.relpath(EVENTS_CPP, ROOT)))
    print("  wavs from   : {0}".format(os.path.relpath(SOUND_DIR, ROOT)))

    table = parse_event_table()
    print("  {0} rows in the C++ event table".format(len(table)))

    pre_existing = parse_pre_existing()
    print("  {0} stem(s) pre-date the release overhaul; the rest are new".format(len(pre_existing)))

    unwired = parse_unwired()
    if unwired:
        print("  {0} event(s) UNWIRED by Demo 29: {1}".format(len(unwired), ", ".join(sorted(unwired))))

    wavs = {}
    for path in sorted(glob.glob(os.path.join(SOUND_DIR, "**", "*.wav"), recursive=True)):
        stem = os.path.splitext(os.path.basename(path))[0]
        if stem in wavs:
            problem("two WAVs share the stem '{0}': {1} and {2}".format(
                stem, os.path.relpath(wavs[stem], ROOT), os.path.relpath(path, ROOT)))
            continue
        wavs[stem] = path
    print("  {0} WAVs on disk".format(len(wavs)))

    calls = index_call_sites()

    for name in sorted(set(wavs) - {r["name"] for r in table}):
        problem("Art/Sounds has '{0}.wav' but TraceSoundEvents.cpp has no row for it - "
                "nothing can play it".format(name))

    rows = []
    for row in table:
        name = row["name"]
        path = wavs.get(name)
        if path is None:
            problem("event '{0}' has no WAV in Art/Sounds - it will be silent in game".format(name))
            continue

        nch, sr, frames, samples = read_wav(path)
        full_duration = frames / float(sr)
        full_mb = os.path.getsize(path) / (1024.0 * 1024.0)
        peak, rms = levels(samples)

        preview = (name in PREVIEW_STEMS) and not args.no_preview
        if preview:
            p_nch, p_sr, p_samples = make_preview(nch, sr, samples, PREVIEW_SECONDS, PREVIEW_DECIMATE)
            blob = wav_bytes(p_nch, p_sr, p_samples)
            duration = (len(p_samples) / p_nch) / float(p_sr)
            out_nch, out_sr = p_nch, p_sr
        else:
            blob = wav_bytes(nch, sr, samples)
            duration = full_duration
            out_nch, out_sr = nch, sr

        # call sites: the trigger file:line, deduplicated, at most three
        sites = calls.get(name, [])
        if not sites and re.fullmatch(r"Step\d+", name):
            sites = calls.get("FootstepAt", []) or ["played through TraceSoundEvents::FootstepAt()"]
        if not sites and re.fullmatch(r"PistolShoot\d", name):
            sites = calls.get("PistolShotEvent", []) or ["played through TraceSoundEvents::PistolShotEvent()"]
        seen, uniq = set(), []
        for s in sites:
            f = s.rsplit(":", 1)[0]
            if f in seen:
                continue
            seen.add(f)
            uniq.append(s)
        call = "  ".join(uniq[:3])

        rows.append({
            "name": name, "side": row["side"], "trigger": row["trigger"],
            "unwired": unwired.get(name, ""),
            "new": name not in pre_existing,
            "family": row["family"], "group": group_of(name),
            "relpath": os.path.relpath(path, ROOT),
            "duration": duration, "full_duration": full_duration, "full_mb": full_mb,
            "channels": out_nch, "rate": out_sr,
            "peak": peak, "rms": rms, "preview": preview,
            "call": call,
            "b64": base64.b64encode(blob).decode("ascii"),
        })
        print("    {0:<20} {1:>6.2f}s  {2}  peak {3:>6}  {4}".format(
            name, duration, "mono  " if out_nch == 1 else "stereo",
            fmt_db(peak), "PREVIEW" if preview else ""))

    if not rows:
        raise SystemExit("no rows survived - nothing to render")

    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    page = build_page(rows, stamp, "")
    size_mb = len(page.encode("utf-8")) / (1024.0 * 1024.0)
    page = build_page(rows, stamp, "{0} rows, {1:.1f} MB, entirely offline.".format(len(rows), size_mb))

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(page)

    print("")
    print("  wrote {0}".format(os.path.relpath(out, ROOT)))
    print("  {0} rows, {1:.1f} MB".format(len(rows), len(page.encode("utf-8")) / (1024.0 * 1024.0)))
    groups = {}
    for r in rows:
        groups[r["group"]] = groups.get(r["group"], 0) + 1
    for g in group_order(rows):
        print("    {0:<28} {1}".format(g, groups[g]))
    if _problems:
        print("")
        print("  {0} warning(s) - they are printed on the page too".format(len(_problems)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
