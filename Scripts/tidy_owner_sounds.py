#!/usr/bin/env python3
"""tidy_owner_sounds.py -- the owner's "trim and fade" pass over seven of his own sounds.

The owner's eleven new WAVs ("Sounds folder.zip") went in byte for byte in 5cf7947.
He then asked for exactly three kinds of tidying and nothing else:

  LEAD-IN    CorePickup, Respawn
             The quiet start is cut to 2 ms before the first sample above -50 dBFS
             (the same -50 dBFS "lead-in" 5cf7947's message measured: 45 ms and
             60 ms). The new first 1 ms gets a raised-cosine fade-in from exact
             zero, so the cut cannot click. The fade lies inside the 2 ms pre-roll,
             so it never touches a sample at or above the threshold. Every sample
             from 1 ms on is the original, unchanged. The lead-in is not digital
             silence. It is the bottom of each sound's swell, rising from about
             -90 dBFS. The part that is cut peaks at about -51 dBFS, roughly 40 dB
             under each sound's peak.

  ENDINGS    CoreTurnover, Goal, Kill, MeleeBackstab
             A 5 ms (220-sample) raised-cosine fade-out over the last samples,
             ending on exactly 0 (Kill used to stop at -51 dBFS). Every earlier
             sample is unchanged.

  LOOP       MusicTitle
             An equal-power overlap-add loop crossfade. The last L ms (default
             35) are blended into the first L ms with sin/cos gains and then
             dropped, so the file gets L ms shorter. Sample 0 of the result is the
             original sample that followed the new last sample, so the wrap is a
             step the track already makes: no tick. The crossfade carries the
             tail's level across the first ~25 ms of the head, where the delivered
             file's low end (below ~300 Hz) is still coming up from ~20 dB down
             (the higher bands start at full level), so the level no longer dips at
             the wrap. Every sample from L on is unchanged.
             Why equal-power: the end of bar 64 and the start of bar 1 are
             different material (their correlation over the crossfade is 0.26),
             and for nearly uncorrelated signals sin/cos gains keep the power
             constant.
             Why 35 ms. `--sweep` measures every length from 15 to 80 ms, 1 ms
             apart, against the music's own behaviour in the 20 s either side of
             the wrap: the 20 ms RMS change across the wrap (must sit inside the
             music's 10th-90th percentile); the deeper of the first two 10 ms
             windows, and the deepest 5 ms window of the low band (< 300 Hz, where
             the defect is) in the first 30 ms (each no deeper than the music's
             10th percentile); and the sample step (no bigger than 95% of the
             track's own). The verdict wobbles from one length to the next,
             because each length lines up different music: 15-25, 29-32, 44,
             46-47 and 60 ms fail; 26-28 ms pass, but as an island between
             failures. 35 ms is the shortest length that passes with every length
             within 2 ms of it passing too (33-43 all pass), so it is not a lucky
             alignment. There: RMS change -0.18 dB, 10 ms dip -2.1 dB, low-band
             dip -5.6 dB (percentiles 47, 28 and 21 of the music's own), wrap step
             25/45. The delivered seam: -7.0 dB, -10.3 dB, -18.5 dB (percentiles
             0.1, 1.1 and 0.6) and a step of 476/1572.
             The cost is timing: once per loop, the last beat of bar 64 is 35 ms
             short, so the loop is 35 ms shorter than 64 bars at 132 BPM.
             Not length-preserving, because no length-preserving way measured as
             clean. Keeping 116.36 s means inventing the L ms that would follow the
             end, by borrowing a lag-matched earlier copy of the end of bar 64 and
             jumping into it at the wrap. The best match (53.7 ms back, correlation
             0.87 over the last 10 ms) jumps 847/1070 at the wrap, the 98th
             percentile of the track's own sample steps, as bad as the delivered
             seam. Nudging the lag by half a millisecond to where the sample values
             happen to meet (53.2 ms, 97/67) is a coincidence of two values, not a
             continuation of the waveform. The overlap-add steps 25/45 (15th/25th
             percentile), and that is a step the track itself makes.
             The track has no repeated bar to borrow from either. The best match
             anywhere else in the track for bar 1's opening half second (50-550 ms)
             correlates 0.57, and for the last 0.5 s of bar 64, 0.70.
             Every number above describes the WAV. The game plays the seam as
             written only because S_MusicTitle is stored as PCM
             (PCM_LOOP_STEMS in Scripts/import_sounds.py). With the project's
             default codec, BINKA, the decoder starts the loop again from zero at
             every wrap. That is a tick and a dip in game, whatever the file's
             seam is like.

NOTHING ELSE CHANGES: no normalising, no limiting, no resampling, no dither. The
channel count, sample rate and bit depth stay the same, and every non-audio
chunk (MusicTitle's LIST/INFO) is copied byte for byte. A processed sample that
would leave the 16-bit range is a hard error, never a clip. MeleeSwing's own
18.5 ms lead-in and the other four owner files (MeleeHit, Reload, WeaponSwitch,
MeleeSwing) are not touched.

DETERMINISTIC AND NON-COMPOUNDING. The inputs are always the ORIGINALS as
committed in 5cf7947 (SOURCE_COMMIT), read through git and git-lfs, and each one
is checked against the SHA-256 the owner's zip had (ORIGINALS below). Running it
twice writes the same bytes twice. It never reads its own output.

Usage:
  python3 Scripts/tidy_owner_sounds.py            # write the 7 processed WAVs into Art/Sounds/
  python3 Scripts/tidy_owner_sounds.py --check    # process in memory, compare with Art/Sounds/, write nothing
  python3 Scripts/tidy_owner_sounds.py --only Kill --only Goal
  python3 Scripts/tidy_owner_sounds.py --out-dir /tmp/tidy   # write somewhere else (same sub-folders)
  python3 Scripts/tidy_owner_sounds.py --sweep    # MusicTitle: seam numbers for crossfade lengths 15..80 ms

After writing: ./Scripts/import-sounds.sh --only CorePickup,Respawn,CoreTurnover,Goal,Kill,MeleeBackstab,MusicTitle
(the editor must be closed; the seven S_<stem>.uasset and DA_TraceSoundBank must be locked).

Stdlib only, like Scripts/generate_sounds.py. Scripts/generate_sounds.py still
lists Respawn, MeleeBackstab and MusicTitle in OWNER_SUPPLIED_STEMS. They are
still the owner's sounds, tidied, and that script must never render over them.
"""

import argparse
import array
import hashlib
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The commit that holds the owner's files exactly as delivered (sha256 = the zip's).
SOURCE_COMMIT = "5cf7947"

# stem -> (repo path, SHA-256 of the original as delivered, operation)
ORIGINALS = {
    "CorePickup":    ("Art/Sounds/CorePickup.wav",
                      "1adb60bb7a9a40b2a75c3d312c7aa15b6cf43f0fa36c041faa777fe645588297", "leadin"),
    "Respawn":       ("Art/Sounds/Combat/Respawn.wav",
                      "7b371b77be8505736a6e5fe7a72597c7e7041c91a926319b8718b71e0f150029", "leadin"),
    "CoreTurnover":  ("Art/Sounds/CoreTurnover.wav",
                      "7c9c3ee5859f9f3d461766a48aff0142790c3d65296fc4ddec44d8602982d3d9", "fadeout"),
    "Goal":          ("Art/Sounds/Goal.wav",
                      "5dccc169d45b450e3cb175635903df17ae1e14d3bc2cb42ba8273a8d23a53db3", "fadeout"),
    "Kill":          ("Art/Sounds/Kill.wav",
                      "b953738eb1d69cbcb88d4cf9d84b4a1db64d4b5b32c1c1ca00194dd8240e0de8", "fadeout"),
    "MeleeBackstab": ("Art/Sounds/Combat/MeleeBackstab.wav",
                      "afc3a9f82349221362936c962320787dd86d12479fdde1aef7a3a59efce7766e", "fadeout"),
    "MusicTitle":    ("Art/Sounds/Music/MusicTitle.wav",
                      "b98fbe377de278c901d8a11918d35b26744845c5b39a2904f10e9137e2ddb386", "loop"),
}
ORDER = ["CorePickup", "Respawn", "CoreTurnover", "Goal", "Kill", "MeleeBackstab", "MusicTitle"]

LEADIN_THRESHOLD_DBFS = -50.0   # 5cf7947's own definition of "lead-in"
LEADIN_PREROLL_MS = 2.0         # keep this much before the first sample above the threshold
LEADIN_FADE_MS = 1.0            # raised-cosine fade-in on the new first samples (inside the pre-roll)
FADEOUT_MS = 5.0                # raised-cosine fade-out to exact zero
LOOP_XFADE_MS = 35.0            # MusicTitle loop crossfade (see the docstring and --sweep)
NEAR_SEAM_S = 20.0              # the music either side of the wrap that the seam is judged against
LOW_BAND_HZ = 300.0             # the band the delivered head is missing (seam measurement only)

# For reporting only: the loop was delivered as 64 bars of 4/4 at 132 BPM.
MUSIC_BPM, MUSIC_BARS, BEATS_PER_BAR = 132.0, 64, 4


# ---------------------------------------------------------------------------
# reading the originals out of git
# ---------------------------------------------------------------------------

def git_bytes(args, stdin=None):
    proc = subprocess.run(["git", "-C", ROOT] + args, input=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.exit("FATAL: git %s failed:\n%s" % (" ".join(args), proc.stderr.decode("utf-8", "replace")))
    return proc.stdout


def read_original(stem):
    """The owner's file as committed in SOURCE_COMMIT, checked against its SHA-256."""
    rel, want_sha, _op = ORIGINALS[stem]
    blob = git_bytes(["cat-file", "blob", "%s:%s" % (SOURCE_COMMIT, rel)])
    if blob.startswith(b"version https://git-lfs"):
        blob = git_bytes(["lfs", "smudge", "--", rel], stdin=blob)
        if blob.startswith(b"version https://git-lfs"):
            sys.exit("FATAL: git lfs smudge returned a pointer for %s. Run: git lfs fetch" % rel)
    got = hashlib.sha256(blob).hexdigest()
    if got != want_sha:
        sys.exit("FATAL: %s at %s has sha256 %s, expected the owner's %s.\n"
                 "       Refusing to process anything but the delivered original."
                 % (rel, SOURCE_COMMIT, got, want_sha))
    return blob


# ---------------------------------------------------------------------------
# RIFF/WAVE: keep every chunk byte for byte, replace only the data payload
# ---------------------------------------------------------------------------

class Wav(object):
    def __init__(self, raw):
        if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
            raise ValueError("not a RIFF/WAVE file")
        self.chunks = []          # [(id, payload bytes)], in file order
        i = 12
        while i + 8 <= len(raw):
            cid = raw[i:i + 4]
            size = int.from_bytes(raw[i + 4:i + 8], "little")
            self.chunks.append((cid, raw[i + 8:i + 8 + size]))
            i += 8 + size + (size & 1)
        fmt = self.chunk(b"fmt ")
        self.format_tag = int.from_bytes(fmt[0:2], "little")
        self.nch = int.from_bytes(fmt[2:4], "little")
        self.rate = int.from_bytes(fmt[4:8], "little")
        self.bits = int.from_bytes(fmt[14:16], "little")
        if self.format_tag != 1 or self.bits != 16:
            raise ValueError("expected 16-bit PCM, got format %d / %d-bit" % (self.format_tag, self.bits))
        self.samples = array.array("h")
        self.samples.frombytes(self.chunk(b"data"))
        if sys.byteorder == "big":
            self.samples.byteswap()

    def chunk(self, cid):
        for c, payload in self.chunks:
            if c == cid:
                return payload
        raise ValueError("no %r chunk" % cid)

    @property
    def frames(self):
        return len(self.samples) // self.nch

    def to_bytes(self, samples):
        """Same chunks in the same order, with `samples` as the data payload."""
        pcm = array.array("h", samples)
        if sys.byteorder == "big":
            pcm.byteswap()
        out = []
        for cid, payload in self.chunks:
            if cid == b"data":
                payload = pcm.tobytes()
            out.append(cid + len(payload).to_bytes(4, "little") + payload + (b"\0" if len(payload) & 1 else b""))
        body = b"WAVE" + b"".join(out)
        return b"RIFF" + len(body).to_bytes(4, "little") + body


def q16(v):
    """Round half up to an int16 sample. Leaving the 16-bit range is an error, never a clip."""
    r = int(math.floor(v + 0.5))
    if r > 32767 or r < -32768:
        raise OverflowError("processed sample %r is outside 16-bit range; refusing to clip" % v)
    return r


def ms_to_frames(ms, rate):
    return int(round(ms * rate / 1000.0))


# ---------------------------------------------------------------------------
# measurements (used by the report and by the self-checks)
# ---------------------------------------------------------------------------

def dbfs(x):
    return 20.0 * math.log10(x / 32768.0) if x > 0 else float("-inf")


def first_frame_above(samples, nch, threshold_dbfs):
    thr = 32768.0 * 10.0 ** (threshold_dbfs / 20.0)
    for i in range(len(samples) // nch):
        for c in range(nch):
            if abs(samples[i * nch + c]) > thr:
                return i
    return None


def peak_dbfs(samples):
    return dbfs(max(abs(v) for v in samples)) if len(samples) else float("-inf")


def rms_dbfs(samples):
    if not len(samples):
        return float("-inf")
    return dbfs(math.sqrt(sum(float(v) * v for v in samples) / len(samples)))


def frames_slice(samples, nch, a, b):
    return samples[a * nch:b * nch]


# ---------------------------------------------------------------------------
# the three operations
# ---------------------------------------------------------------------------

def trim_lead_in(w):
    nch, s = w.nch, w.samples
    first = first_frame_above(s, nch, LEADIN_THRESHOLD_DBFS)
    if first is None:
        raise ValueError("nothing above %.0f dBFS" % LEADIN_THRESHOLD_DBFS)
    preroll = ms_to_frames(LEADIN_PREROLL_MS, w.rate)
    fade = ms_to_frames(LEADIN_FADE_MS, w.rate)
    must(fade <= preroll, "the fade-in must stay inside the pre-roll")
    cut = max(0, first - preroll)
    out = array.array("h", s[cut * nch:])
    for i in range(fade):
        g = 0.5 - 0.5 * math.cos(math.pi * i / fade)      # 0 at i=0, ~1 at i=fade-1
        for c in range(nch):
            out[i * nch + c] = q16(out[i * nch + c] * g)
    info = {"cut_frames": cut, "first_above": first, "preroll": preroll, "fade": fade,
            "unchanged_from": fade}  # out[fade:] == s[cut+fade:]
    return out, info


def fade_out(w):
    nch, s = w.nch, w.samples
    n = w.frames
    f = ms_to_frames(FADEOUT_MS, w.rate)
    out = array.array("h", s)
    for k in range(f):
        g = 0.5 + 0.5 * math.cos(math.pi * (k + 1) / f)  # ~1 at k=0, exactly 0 at k=f-1 (the last sample)
        i = n - f + k
        for c in range(nch):
            out[i * nch + c] = q16(s[i * nch + c] * g)
    return out, {"fade": f}


def loop_crossfade(w, xfade_ms):
    nch, s = w.nch, w.samples
    n = w.frames
    L = ms_to_frames(xfade_ms, w.rate)
    if not 0 < L < n // 2:
        raise ValueError("crossfade of %d frames does not fit a %d-frame loop" % (L, n))
    head = array.array("h", [0] * (L * nch))
    for t in range(L):
        u = t / float(L)
        gi = math.sin(0.5 * math.pi * u)                  # head comes in: 0 at t=0
        go = math.cos(0.5 * math.pi * u)                  # tail goes out: 1 at t=0
        for c in range(nch):
            head[t * nch + c] = q16(gi * s[t * nch + c] + go * s[(n - L + t) * nch + c])
    out = head + s[L * nch:(n - L) * nch]
    return out, {"xfade": L}


# ---------------------------------------------------------------------------
# loop-seam measurements (MusicTitle)
# ---------------------------------------------------------------------------

def window_db(samples, nch, win):
    """dBFS RMS of consecutive non-overlapping windows of `win` frames (all channels together)."""
    n = len(samples) // nch
    out = []
    for k in range(n // win):
        seg = samples[k * win * nch:(k + 1) * win * nch]
        out.append(dbfs(math.sqrt(sum(float(v) * v for v in seg) / len(seg))))
    return out


def median(vals):
    v = sorted(vals)
    m = len(v) // 2
    return v[m] if len(v) % 2 else 0.5 * (v[m - 1] + v[m])


def lowpass(x, rate, fc):
    """2nd-order Butterworth low-pass (RBJ biquad, direct form I) over one channel. Measurement
    only: nothing that is written to disk is ever filtered."""
    w0 = 2.0 * math.pi * fc / rate
    alpha = math.sin(w0) / (2.0 * math.sqrt(0.5))
    cw = math.cos(w0)
    a0 = 1.0 + alpha
    b0 = b2 = (1.0 - cw) / 2.0 / a0
    b1 = (1.0 - cw) / a0
    a1 = -2.0 * cw / a0
    a2 = (1.0 - alpha) / a0
    y = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def low_band_db(samples, nch, rate, win):
    """dBFS RMS of the < LOW_BAND_HZ band in consecutive `win`-frame windows (all channels together)."""
    chans = [lowpass(samples[c::nch], rate, LOW_BAND_HZ) for c in range(nch)]
    out = []
    for k in range(len(chans[0]) // win):
        acc = 0.0
        for ch in chans:
            acc += sum(v * v for v in ch[k * win:(k + 1) * win])
        out.append(dbfs(math.sqrt(acc / (win * nch))))
    return out


LOW_PRE_WINDOWS, LOW_POST_WINDOWS = 40, 6   # 200 ms reference before, first 30 ms after (5 ms windows)


def low_band_dip(env, t):
    """Deepest low-band 5 ms window in the 30 ms from window t, minus the median of the 200 ms before."""
    return min(env[t:t + LOW_POST_WINDOWS]) - median(env[t - LOW_PRE_WINDOWS:t])


def seam_numbers(samples, nch, rate):
    """What the ear gets at the wrap (last sample -> first sample):
         steps   |first - last| per channel, the click;
         change  RMS of the first 20 ms minus RMS of the last 20 ms, in dB;
         dip     the quieter of the first two 10 ms windows minus the median 10 ms window of the
                 200 ms before the wrap, in dB: the brief hole a 20 ms average can smear over;
         low     the same idea in the band the delivered head is missing (< LOW_BAND_HZ): the
                 deepest 5 ms window of the first 30 ms after the wrap minus the median 5 ms window
                 of the 200 ms before it, measured on the loop as it plays (end, then start)."""
    n = len(samples) // nch
    w20, w10, w5 = ms_to_frames(20, rate), ms_to_frames(10, rate), ms_to_frames(5, rate)
    steps = [abs(samples[c] - samples[(n - 1) * nch + c]) for c in range(nch)]
    before = rms_dbfs(frames_slice(samples, nch, n - w20, n))
    after = rms_dbfs(frames_slice(samples, nch, 0, w20))
    pre = [rms_dbfs(frames_slice(samples, nch, n - (k + 1) * w10, n - k * w10)) for k in range(20)]
    post = [rms_dbfs(frames_slice(samples, nch, k * w10, (k + 1) * w10)) for k in range(2)]
    # 1 s of the end (a whole number of 5 ms windows, so the wrap falls on a window edge), then the
    # start: the filter runs straight through the wrap, the way the player hears it.
    k0 = (rate // w5)
    pre_frames = k0 * w5
    played = frames_slice(samples, nch, n - pre_frames, n) + frames_slice(samples, nch, 0, LOW_POST_WINDOWS * w5)
    env = low_band_db(played, nch, rate, w5)
    return {"steps": steps, "before": before, "after": after, "change": after - before,
            "dip": min(post) - median(pre), "low": low_band_dip(env, k0)}


class NearSeam(object):
    """The music's own behaviour in the NEAR_SEAM_S seconds either side of the wrap (the start and
    the end of the ORIGINAL file), as the yardstick for the seam: the same three measurements taken
    at every 20 ms / 10 ms / 5 ms position in that material. The loop's quiet outro and intro live
    there; the loud middle section, with its drums, would make any seam look ordinary. The sample
    steps are the whole track's."""

    def __init__(self, w):
        nch, rate, s = w.nch, w.rate, w.samples
        span = ms_to_frames(NEAR_SEAM_S * 1000.0, rate)
        segs = [s[:span * nch], s[(w.frames - span) * nch:]]
        self.change, self.dip, self.low = [], [], []
        for seg in segs:
            r20 = window_db(seg, nch, ms_to_frames(20, rate))
            self.change += [b - a for a, b in zip(r20, r20[1:]) if math.isfinite(b - a)]
            r10 = window_db(seg, nch, ms_to_frames(10, rate))
            for t in range(20, len(r10) - 1):
                d = min(r10[t], r10[t + 1]) - median(r10[t - 20:t])
                if math.isfinite(d):
                    self.dip.append(d)
            env = low_band_db(seg, nch, rate, ms_to_frames(5, rate))
            for t in range(LOW_PRE_WINDOWS, len(env) - LOW_POST_WINDOWS):
                d = low_band_dip(env, t)
                if math.isfinite(d):
                    self.low.append(d)
        self.change.sort()
        self.dip.sort()
        self.low.sort()
        self.steps = [sorted_steps(s, nch, c) for c in range(nch)]

    def judge(self, sn):
        """(change pct, dip pct, low-band dip pct, step pcts, passes). A seam passes when its 20 ms
        change sits inside the music's own 10th..90th percentile, neither dip is deeper than the
        music's own 10th percentile, and no channel's step is bigger than 95% of the track's own
        steps: when it can no longer be told apart from the music."""
        pc = pct_below(self.change, sn["change"])
        pd = pct_below(self.dip, sn["dip"])
        pl = pct_below(self.low, sn["low"])
        ps = [pct_below(self.steps[c], sn["steps"][c]) for c in range(len(self.steps))]
        return pc, pd, pl, ps, (10.0 <= pc <= 90.0 and pd >= 10.0 and pl >= 10.0 and max(ps) <= 95.0)


def sorted_steps(samples, nch, c):
    ch = samples[c::nch]
    return sorted(abs(b - a) for a, b in zip(ch, ch[1:]))


def pct_below(sorted_vals, x):
    """Percentage of sorted_vals strictly below x."""
    lo, hi = 0, len(sorted_vals)
    while lo < hi:
        mid = (lo + hi) // 2
        if sorted_vals[mid] < x:
            lo = mid + 1
        else:
            hi = mid
    return 100.0 * lo / len(sorted_vals)


def grid_frames(rate):
    return MUSIC_BARS * BEATS_PER_BAR * 60.0 / MUSIC_BPM * rate


# ---------------------------------------------------------------------------
# process one stem, with self-checks that only the intended region changed
# ---------------------------------------------------------------------------

def must(ok, what):
    """The self-checks. Not `assert`, so that `python -O` cannot switch them off."""
    if not ok:
        sys.exit("FATAL: self-check failed: %s. Nothing more was written." % what)


def process(stem, xfade_ms):
    rel, _sha, op = ORIGINALS[stem]
    w = Wav(read_original(stem))
    nch, s = w.nch, w.samples
    if op == "leadin":
        out, info = trim_lead_in(w)
        cut, keep = info["cut_frames"], info["unchanged_from"]
        must(out[keep * nch:] == s[(cut + keep) * nch:], "%s: the trim changed samples after the fade-in" % stem)
        must(all(v == 0 for v in out[:nch]), "%s: the first frame is not 0" % stem)
    elif op == "fadeout":
        out, info = fade_out(w)
        f = info["fade"]
        must(len(out) == len(s), "%s: the fade-out changed the length" % stem)
        must(out[:-f * nch] == s[:-f * nch], "%s: the fade-out changed samples before its window" % stem)
        must(all(v == 0 for v in out[-nch:]), "%s: the last frame is not exactly 0" % stem)
    else:
        out, info = loop_crossfade(w, xfade_ms)
        L = info["xfade"]
        must(len(out) == len(s) - L * nch, "%s: the loop is not exactly L frames shorter" % stem)
        must(out[L * nch:] == s[L * nch:(w.frames - L) * nch],
             "%s: the crossfade changed samples outside the first L frames" % stem)
        must(list(out[:nch]) == list(s[(w.frames - L) * nch:(w.frames - L + 1) * nch]),
             "%s: the first frame is not the original frame that followed the new last frame" % stem)
        near = NearSeam(w)
        info["near"] = near
        info["seam_before"] = seam_numbers(s, nch, w.rate)
        info["seam_after"] = seam_numbers(out, nch, w.rate)
        must(near.judge(info["seam_after"])[-1],
             "%s: the new seam still stands out from the music (%s)"
             % (stem, describe_seam(info["seam_after"], near)))
    data = w.to_bytes(out)
    check = Wav(data)
    must((check.nch, check.rate, check.bits, check.format_tag) == (w.nch, w.rate, w.bits, w.format_tag),
         "%s: the format changed" % stem)
    must([c for c, _ in check.chunks] == [c for c, _ in w.chunks], "%s: the chunk list changed" % stem)
    must(all(p1 == p2 for (c1, p1), (c2, p2) in zip(check.chunks, w.chunks) if c1 != b"data"),
         "%s: a non-audio chunk changed" % stem)
    must(check.samples == out, "%s: the written data does not read back" % stem)
    return rel, w, out, data, info


def report(stem, w, out, info):
    nch, rate, s = w.nch, w.rate, w.samples
    ms = lambda frames: 1000.0 * frames / rate
    op = ORIGINALS[stem][2]
    head = "  %-14s %dch %d Hz %d-bit  %.4f s -> %.4f s" % (
        stem, nch, rate, w.bits, w.frames / float(rate), len(out) // nch / float(rate))
    print(head)
    if op == "leadin":
        new_first = first_frame_above(out, nch, LEADIN_THRESHOLD_DBFS)
        removed = s[:info["cut_frames"] * nch]
        print("      lead-in (first sample above %.0f dBFS): %.2f ms -> %.2f ms; cut %d frames (%.2f ms) "
              "that peaked at %.1f dBFS (rms %.1f); 1 ms raised-cosine fade-in from 0; "
              "unchanged from frame %d on"
              % (LEADIN_THRESHOLD_DBFS, ms(info["first_above"]), ms(new_first), info["cut_frames"],
                 ms(info["cut_frames"]), peak_dbfs(removed), rms_dbfs(removed), info["unchanged_from"]))
    elif op == "fadeout":
        f = info["fade"]
        m1 = ms_to_frames(1, rate)
        before = frames_slice(s, nch, w.frames - f, w.frames)
        after = frames_slice(out, nch, w.frames - f, w.frames)
        print("      last 5 ms: peak %.1f -> %.1f dBFS, rms %.1f -> %.1f dBFS; last 1 ms peak %.1f -> %.1f dBFS; "
              "last sample %s -> %s; %d-frame raised-cosine fade, everything before it unchanged"
              % (peak_dbfs(before), peak_dbfs(after), rms_dbfs(before), rms_dbfs(after),
                 peak_dbfs(s[-m1 * nch:]), peak_dbfs(out[-m1 * nch:]),
                 list(s[-nch:]), list(out[-nch:]), f))
    else:
        L = info["xfade"]
        n_new = len(out) // nch
        grid = grid_frames(rate)
        print("      loop crossfade %d frames (%.2f ms, equal-power); length %d -> %d frames; "
              "64 bars at %.0f BPM = %.2f frames, so the loop is now %+.2f ms off the grid (was %+.2f ms)"
              % (L, ms(L), w.frames, n_new, MUSIC_BPM, grid, ms(n_new - grid), ms(w.frames - grid)))
        for label, key in (("before", "seam_before"), ("after ", "seam_after")):
            print("      %s: %s" % (label, describe_seam(info[key], info["near"])))


def describe_seam(sn, near):
    pc, pd, pl, ps, ok = near.judge(sn)
    return ("wrap step %s (bigger than %s of the track's own steps); RMS last 20 ms %.2f, first 20 ms %.2f dBFS, "
            "change %+.2f dB (percentile %.1f near the seam); deeper of the first two 10 ms windows %+.1f dB "
            "vs the 200 ms before (percentile %.1f); low band (< %.0f Hz) deepest 5 ms window in the first "
            "30 ms %+.1f dB vs the 200 ms before (percentile %.1f) -> %s"
            % ("/".join(str(v) for v in sn["steps"]), "/".join("%.1f%%" % p for p in ps),
               sn["before"], sn["after"], sn["change"], pc, sn["dip"], pd, LOW_BAND_HZ, sn["low"], pl,
               "indistinguishable from the music" if ok else "STANDS OUT"))


def sweep(lengths_ms):
    w = Wav(read_original("MusicTitle"))
    nch, rate, s = w.nch, w.rate, w.samples
    near = NearSeam(w)
    c, d, lo = near.change, near.dip, near.low
    print("MusicTitle loop crossfade sweep (equal-power overlap-add; the file gets L ms shorter)")
    print("  the music in the %.0f s either side of the wrap: 20 ms RMS change 10th..90th percentile "
          "%+.2f..%+.2f dB; 10 ms dip 10th percentile %+.2f dB; low-band dip 10th percentile %+.2f dB" % (
              NEAR_SEAM_S, c[int(0.10 * len(c))], c[int(0.90 * len(c))], d[int(0.10 * len(d))],
              lo[int(0.10 * len(lo))]))
    grid = grid_frames(rate)
    passed = {}
    for L_ms in [0] + list(lengths_ms):
        out = s if L_ms == 0 else loop_crossfade(w, L_ms)[0]
        n = len(out) // nch
        sn = seam_numbers(out, nch, rate)
        if L_ms:
            passed[L_ms] = near.judge(sn)[-1]
        print("  L=%3s ms  %.5f s (%+.2f ms vs 64 bars)  %s" % (
            "--" if L_ms == 0 else L_ms, n / float(rate), 1000.0 * (n - grid) / rate,
            describe_seam(sn, near)))
    robust = [L for L in sorted(passed) if all(passed.get(L + d, False) for d in range(-2, 3))]
    print("  fail: %s" % ", ".join(str(L) for L in sorted(passed) if not passed[L]))
    print("  shortest length that passes with every length within 2 ms of it passing too: %s"
          % ("%d ms" % robust[0] if robust else "none"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--only", action="append", metavar="STEM", help="process just this stem; repeatable")
    ap.add_argument("--check", action="store_true",
                    help="process in memory and compare with the files in Art/Sounds; write nothing")
    ap.add_argument("--out-dir", help="write under this directory instead of the repository root")
    ap.add_argument("--loop-xfade-ms", type=float, default=LOOP_XFADE_MS,
                    help="MusicTitle crossfade length (default %(default)s)")
    ap.add_argument("--sweep", action="store_true",
                    help="print MusicTitle seam numbers for crossfade lengths 15..80 ms, 1 ms apart; "
                         "write nothing")
    args = ap.parse_args()

    if args.sweep:
        sweep(range(15, 81))
        return

    stems = ORDER
    if args.only:
        unknown = sorted(set(args.only) - set(ORDER))
        if unknown:
            sys.exit("unknown stem(s): %s (this script handles: %s)" % (", ".join(unknown), ", ".join(ORDER)))
        stems = [s for s in ORDER if s in args.only]

    base = os.path.abspath(args.out_dir) if args.out_dir else ROOT
    mismatches = 0
    print("tidy_owner_sounds: originals from %s, %s" % (SOURCE_COMMIT, "CHECK (writing nothing)" if args.check
                                                         else "writing under %s" % base))
    for stem in stems:
        rel, w, out, data, info = process(stem, args.loop_xfade_ms)
        report(stem, w, out, info)
        path = os.path.join(base, rel)
        if args.check:
            try:
                with open(path, "rb") as f:
                    same = f.read() == data
            except IOError:
                same = False
            print("      %s %s" % ("IDENTICAL to" if same else "DIFFERS from", rel))
            mismatches += 0 if same else 1
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            print("      wrote %s (%d bytes, sha256 %s)" % (rel, len(data), hashlib.sha256(data).hexdigest()))
    if args.check:
        print("check: %d of %d identical" % (len(stems) - mismatches, len(stems)))
        sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
