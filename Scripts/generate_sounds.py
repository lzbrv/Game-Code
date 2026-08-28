#!/usr/bin/env python3
"""generate_sounds.py -- the synthesized release audio palette.

Renders the 43 new sound events of FX_AUDIO_PLAN.md SS5.1 (24 ability + 10 combat
+ 5 UI + 2 stingers + 2 music beds) into Art/Sounds/{Combat,Abilities,UI,Music}/
per the exact recipes of SS5.3-5.4.  Folders are filing, not naming -- stems are
globally unique and the importer (Scripts/import_sounds.py) enforces that, so this
script refuses to run if any of its stems would collide with a WAV already living
anywhere under Art/Sounds/ (in particular the 28 pre-existing events, which are
frozen and never touched here).

Format doctrine (SS5.0, verified against the existing bank):
  * 44,100 Hz, 16-bit PCM everywhere.
  * MONO for every spatialized (World-side) and client hit-feedback sound
    (Combat/ and Abilities/).
  * STEREO for music, ambience, stingers and UI 2D (Music/ and UI/).

Loop doctrine (SS5.2): every *Loop / Music* / Ambience* file is seamless by
construction -- event-based layers are composed on a circular buffer (indices
mod N, tails wrap), continuous noise beds are rendered N+overlap samples long
and crossfade-wrapped, and every periodic modulator (tremolo/vibrato/flutter/
pump) is snapped to an integer number of cycles per loop.  One-shots get a 5 ms
cosine fade-out.

Determinism: the palette is seeded from the single project seed 2026; each stem
derives its own random stream from it (random.Random("trace-audio-2026:<stem>"))
so `--only <Stem>` re-renders that one file byte-identically regardless of render
order.  `--verify` re-renders everything in memory, asserts the bytes match the
files on disk, and prints the per-file duration/peak/RMS analysis table
(duration, peak dBFS, RMS dBFS, DC offset, loop-seam continuity).

Stdlib only (wave, struct, math, random, array) -- runs on any machine that can
run import_sounds.py.  No engine, no Content/ writes: Wave-2's W2-AUDIOBANK owns
the import and the C++ event-table rows.

Usage:
  python3 Scripts/generate_sounds.py            # render all 43 WAVs
  python3 Scripts/generate_sounds.py --only LilyZip
  python3 Scripts/generate_sounds.py --verify   # byte-identity + analysis table
  python3 Scripts/generate_sounds.py --verify --only MusicTitle
"""

import argparse
import array
import glob
import math
import os
import random
import sys
import wave

SR = 44100
TWO_PI = 2.0 * math.pi
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOUND_DIR = os.path.join(ROOT, "Art", "Sounds")
PROJECT_SEED = 2026  # SS5.2: deterministic random.seed(2026)

# The 28 pre-existing bank WAVs (17 flat + 11 Footsteps/Step*).  Frozen: this
# generator must never overwrite them and refuses on any stem collision.
EXISTING_STEMS = frozenset([
    "Bodyshot", "ButtonPress", "CorePickup", "CoreTurnover", "Dash", "Goal",
    "Headshot", "Jump", "Kill", "Parry", "PistolShoot1", "PistolShoot2",
    "PistolShoot3", "PistolShoot4", "RoccoRipple", "SmgShoot1", "WallJump",
    "Step1", "Step2", "Step3", "Step4", "Step5", "Step6", "Step7", "Step8",
    "Step9", "Step10", "Step11",
])


# ---------------------------------------------------------------------------
# small math helpers
# ---------------------------------------------------------------------------

def db(g):
    """dB -> linear gain."""
    return 10.0 ** (g / 20.0)


def lin_to_db(a):
    if a <= 1e-12:
        return float("-inf")
    return 20.0 * math.log10(a)


def cents(f, c):
    return f * (2.0 ** (c / 1200.0))


def zeros(n):
    return [0.0] * n


# ---------------------------------------------------------------------------
# oscillators (SS5.2 vocabulary: osc with linear/exponential glide, polyBLEP
# band-limiting on saw/square so the stacks stay clean at 44.1k)
# ---------------------------------------------------------------------------

def _blep(t, dt):
    """polyBLEP residual around a discontinuity at phase 0."""
    if t < dt:
        x = t / dt
        return x + x - x * x - 1.0
    if t > 1.0 - dt:
        x = (t - 1.0) / dt
        return x * x + x + x + 1.0
    return 0.0


def osc_fn(n, shape, f_fn, phase0=0.0):
    """Oscillator with per-sample frequency from f_fn(t seconds)."""
    out = zeros(n)
    ph = phase0 % 1.0
    inv = 1.0 / SR
    s = math.sin
    for i in range(n):
        f = f_fn(i * inv)
        dt = f * inv
        ph += dt
        if ph >= 1.0:
            ph -= 1.0
        if shape == "sine":
            out[i] = s(TWO_PI * ph)
        elif shape == "saw":
            # phase-shifted to start at 0 (no onset click)
            x = ph + 0.5
            if x >= 1.0:
                x -= 1.0
            out[i] = 2.0 * x - 1.0 - _blep(x, dt)
        elif shape == "square":
            v = 1.0 if ph < 0.5 else -1.0
            x2 = ph + 0.5
            if x2 >= 1.0:
                x2 -= 1.0
            out[i] = v + _blep(ph, dt) - _blep(x2, dt)
        elif shape == "tri":
            # phase-shifted to start at 0 rising
            x = ph + 0.25
            if x >= 1.0:
                x -= 1.0
            out[i] = 1.0 - 4.0 * abs(x - 0.5)
        else:
            raise ValueError("unknown osc shape " + shape)
    return out


def glide(f0, f1=None, T=None, curve="lin"):
    """Frequency function: f0, or f0->f1 over T seconds (then hold f1)."""
    if f1 is None:
        return lambda t: f0
    if curve == "exp":
        r = f1 / f0
        return lambda t: f0 * (r ** min(1.0, t / T))
    return lambda t: f0 + (f1 - f0) * min(1.0, t / T)


def osc(n, shape, f0, f1=None, glide_s=None, curve="lin", phase0=0.0):
    return osc_fn(n, shape, glide(f0, f1, glide_s, curve), phase0)


def fm_osc(n, ratio, i0, i1, idx_s, f_fn):
    """2-op FM (SS5.2): carrier f from f_fn, modulator at ratio*f, index i0->i1
    over idx_s seconds (then holds i1)."""
    out = zeros(n)
    phc = 0.0
    phm = 0.0
    inv = 1.0 / SR
    s = math.sin
    for i in range(n):
        t = i * inv
        f = f_fn(t)
        u = 1.0 if idx_s <= 0 else min(1.0, t / idx_s)
        idx = i0 + (i1 - i0) * u
        phm += f * ratio * inv
        phc += f * inv
        out[i] = s(TWO_PI * phc + idx * s(TWO_PI * phm))
    return out


def ring(f, tau, dur=None):
    """Decaying sine partial (SS5.2) -- metallic hits are sums of these."""
    n = int(round((dur if dur is not None else tau * 7.0) * SR))
    k = 1.0 / (tau * SR)
    return [math.sin(TWO_PI * f * i / SR) * math.exp(-i * k) for i in range(n)]


def white(n, rng):
    u = rng.uniform
    return [u(-1.0, 1.0) for _ in range(n)]


def pink(n, rng):
    """Voss-McCartney, 8 rows (SS5.2)."""
    u = rng.uniform
    rows = [u(-1.0, 1.0) for _ in range(8)]
    total = sum(rows)
    out = zeros(n)
    for i in range(1, n + 1):
        k = (i & -i).bit_length() - 1  # trailing zeros of the frame counter
        if k < 8:
            total -= rows[k]
            rows[k] = u(-1.0, 1.0)
            total += rows[k]
        out[i - 1] = (total + u(-1.0, 1.0)) * 0.125
    return out


# ---------------------------------------------------------------------------
# envelopes
# ---------------------------------------------------------------------------

def env_seg(n, points):
    """Piecewise-linear envelope from [(t_seconds, level), ...]; holds ends."""
    out = zeros(n)
    inv = 1.0 / SR
    j = 0
    for i in range(n):
        t = i * inv
        while j + 1 < len(points) and points[j + 1][0] <= t:
            j += 1
        if j + 1 >= len(points):
            out[i] = points[-1][1]
        else:
            t0, v0 = points[j]
            t1, v1 = points[j + 1]
            u = 0.0 if t1 <= t0 else (t - t0) / (t1 - t0)
            out[i] = v0 + (v1 - v0) * max(0.0, min(1.0, u))
    return out


def env_adsr(n, a, d, s_level, r):
    """Linear ADSR (SS5.2) sized to n samples."""
    dur = n / SR
    return env_seg(n, [(0.0, 0.0), (a, 1.0), (a + d, s_level),
                       (max(a + d, dur - r), s_level), (dur, 0.0)])


def expdecay(n, tau, attack_s=0.0):
    """e^(-t/tau), with an optional short linear attack ramp for click safety."""
    k = 1.0 / (tau * SR)
    out = [math.exp(-i * k) for i in range(n)]
    an = int(attack_s * SR)
    for i in range(an):
        out[i] *= i / max(1, an)
    return out


def apply(sig, env):
    return [a * b for a, b in zip(sig, env)]


def gained(sig, g):
    return [a * g for a in sig]


def fade_out(sig, seconds=0.005):
    """5 ms cosine fade-out (SS5.2: every one-shot gets one). In place."""
    k = min(len(sig), int(seconds * SR))
    n = len(sig)
    for i in range(k):
        w = 0.5 - 0.5 * math.cos(math.pi * (k - 1 - i) / max(1, k - 1)) if k > 1 else 0.0
        sig[n - k + i] *= w
    return sig


# ---------------------------------------------------------------------------
# filters / effects
# ---------------------------------------------------------------------------

def biquad(sig, kind, fc=None, q=0.707, fc_fn=None, block=64):
    """RBJ cookbook LP/HP/BP (SS5.2).  fc_fn(t) sweeps the cutoff; coefficients
    are recomputed every `block` samples (the plan explicitly allows 64)."""
    n = len(sig)
    out = zeros(n)
    x1 = x2 = y1 = y2 = 0.0
    i = 0
    inv = 1.0 / SR
    while i < n:
        f = fc_fn((i + block * 0.5) * inv) if fc_fn is not None else fc
        f = max(10.0, min(f, SR * 0.45))
        w = TWO_PI * f * inv
        cw = math.cos(w)
        sw = math.sin(w)
        alpha = sw / (2.0 * q)
        if kind == "lp":
            b0 = (1.0 - cw) * 0.5
            b1 = 1.0 - cw
            b2 = b0
        elif kind == "hp":
            b0 = (1.0 + cw) * 0.5
            b1 = -(1.0 + cw)
            b2 = b0
        elif kind == "bp":  # constant 0 dB peak gain
            b0 = alpha
            b1 = 0.0
            b2 = -alpha
        else:
            raise ValueError("unknown biquad kind " + kind)
        a0 = 1.0 + alpha
        a1 = -2.0 * cw
        a2 = 1.0 - alpha
        b0 /= a0
        b1 /= a0
        b2 /= a0
        a1 /= a0
        a2 /= a0
        end = min(i + block, n)
        for j in range(i, end):
            x = sig[j]
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2 = x1
            x1 = x
            y2 = y1
            y1 = y
            out[j] = y
        i = end
    return out


def bp_band(sig, lo, hi, fc_fn=None):
    """Bandpass expressed as a lo..hi band: center = geometric mean, Q from BW."""
    fc = math.sqrt(lo * hi)
    q = fc / max(1.0, (hi - lo))
    return biquad(sig, "bp", fc=fc, q=q, fc_fn=fc_fn)


def softclip(sig, drive):
    """tanh(drive*x)/tanh(drive) (SS5.2) -- the palette's gentle saturation."""
    d = math.tanh(drive)
    return [math.tanh(drive * x) / d for x in sig]


def delay_fx(sig, delay_s, fb, mix, n_out=None):
    """Feedback delay (SS5.2).  n_out extends the render so the tail fits."""
    d = int(round(delay_s * SR))
    n = n_out if n_out is not None else len(sig)
    wet = zeros(n)
    m = len(sig)
    for i in range(n):
        w = (sig[i - d] if 0 <= i - d < m else 0.0)
        if i >= d:
            w += fb * wet[i - d]
        wet[i] = w
    return [(sig[i] if i < m else 0.0) + mix * wet[i] for i in range(n)]


def add_into(dest, src, at=0.0, g=1.0, wrap=False):
    """Mix src into dest at `at` seconds.  wrap=True composes on the circular
    buffer (indices mod N) so loop tails wrap -- the SS5.2 loop mechanism."""
    off = int(round(at * SR))
    n = len(dest)
    if wrap:
        for i, v in enumerate(src):
            dest[(off + i) % n] += v * g
    else:
        for i, v in enumerate(src):
            j = off + i
            if 0 <= j < n:
                dest[j] += v * g
    return dest


def loop_wrap(ext, n, fade_n):
    """Continuous-material looper: ext is n+fade_n samples rendered in one
    unbroken pass; the overlap is crossfaded back over the head so the wrap
    from sample N-1 to sample 0 continues the stream seamlessly."""
    out = ext[:n]
    for i in range(fade_n):
        w = (i + 1) / (fade_n + 1)  # 0->1: hand over from tail-continuation to head
        out[i] = ext[n + i] * (1.0 - w) + ext[i] * w
    return out


def pan_lr(p):
    """Constant-power pan gains; p in [0,1], 0.5 = center."""
    return math.cos(p * math.pi / 2.0), math.sin(p * math.pi / 2.0)


# ===========================================================================
# THE RECIPES (SS5.3 one-shots, SS5.4 loops/music) -- parameters transcribed
# verbatim from FX_AUDIO_PLAN.md; every deviation is a snap-to-loop-period or a
# balance gain and is commented where it happens.
# ===========================================================================

# ---- Combat (mono) --------------------------------------------------------

def r_melee_swing(rng, n):
    # BP-noise whoosh: white -> BP fc 1200->3500 Hz (Q 1.2) over 0.12 s;
    # env attack 60 ms decay 100 ms; + sine 220 Hz thump at t=0, -18 dB, tau 40 ms.
    w = biquad(white(n, rng), "bp", q=1.2,
               fc_fn=glide(1200.0, 3500.0, 0.12, "exp"))
    sig = apply(w, env_adsr(n, 0.060, 0.100, 0.0, 0.0))
    add_into(sig, apply(osc(int(0.12 * SR), "sine", 220.0),
                        expdecay(int(0.12 * SR), 0.040, attack_s=0.002)),
             0.0, db(-18) * 4.0)  # -18 dB against the unit-peak noise layer
    return sig


def r_melee_hit(rng, n):
    # tri 180->90 Hz over 80 ms (thump) + 2 ms noise click HP 2 kHz
    # + ring(450, 60ms) body knock.  Softclip drive 2.
    thump = apply(osc(n, "tri", 180.0, 90.0, 0.080),
                  expdecay(n, 0.050, attack_s=0.003))
    click = biquad(white(int(0.002 * SR), rng), "hp", fc=2000.0)
    sig = thump
    add_into(sig, click, 0.0, 0.8)
    add_into(sig, ring(450.0, 0.060), 0.0, 0.5)
    return softclip(sig, 2.0)


def r_melee_backstab(rng, n):
    # MeleeHit pitched -12 st (tri 90->45, knock ring 225) + delay(40 ms,
    # fb 0.5, mix 0.35) tail + ring(1800, 120ms) blade sing at -14 dB.
    base_n = int(0.14 * SR)
    thump = apply(osc(base_n, "tri", 90.0, 45.0, 0.080),
                  expdecay(base_n, 0.050, attack_s=0.003))
    click = biquad(white(int(0.002 * SR), rng), "hp", fc=2000.0)
    base = thump
    add_into(base, click, 0.0, 0.8)
    add_into(base, ring(225.0, 0.060), 0.0, 0.5)
    base = softclip(base, 2.0)
    sig = delay_fx(base, 0.040, 0.5, 0.35, n_out=n)
    add_into(sig, ring(1800.0, 0.120), 0.0, db(-14) * 2.0)
    return sig


def r_reload(rng, n):
    # three gestures: t=0 clip-out click ring(1800)+ring(2700)+ring(4100) each
    # tau 25 ms; t=0.28 clip-in = same set x2^(-2/12); t=0.42 rack = noise
    # BP 800-2500 Hz 90 ms with 8 ms attack.
    def clickset(scale):
        s = ring(1800.0 * scale, 0.025)
        add_into(s, ring(2700.0 * scale, 0.025), 0.0, 0.8)
        add_into(s, ring(4100.0 * scale, 0.025), 0.0, 0.6)
        return s

    sig = zeros(n)
    add_into(sig, clickset(1.0), 0.0)
    add_into(sig, clickset(2.0 ** (-2.0 / 12.0)), 0.28)
    rack_n = int(0.090 * SR)
    rack = apply(bp_band(white(rack_n, rng), 800.0, 2500.0),
                 env_adsr(rack_n, 0.008, 0.082, 0.0, 0.0))
    add_into(sig, rack, 0.42, 1.2)
    return sig


def r_dry_fire(rng, n):
    # 4 ms noise impulse HP 600 Hz + ring(1200, 60ms).
    sig = zeros(n)
    add_into(sig, biquad(white(int(0.004 * SR), rng), "hp", fc=600.0), 0.0, 0.9)
    add_into(sig, ring(1200.0, 0.060), 0.0, 0.7)
    return sig


def r_weapon_switch(rng, n):
    # noise LP 1500->500 Hz 80 ms (shuck) + terminal click ring(2200, 20ms).
    sh_n = int(0.080 * SR)
    shuck = apply(biquad(white(sh_n, rng), "lp",
                         fc_fn=glide(1500.0, 500.0, 0.080, "exp")),
                  env_adsr(sh_n, 0.010, 0.070, 0.0, 0.0))
    sig = zeros(n)
    add_into(sig, shuck, 0.0)
    add_into(sig, ring(2200.0, 0.020), 0.12, 0.8)  # the terminal click
    return sig


def r_damage_taken(rng, n):
    # sine 100 Hz tau 60 ms thump + square 300 Hz 30 ms grit -12 dB, LP 2 kHz.
    # Deliberately dull -- must never mask gunshots.
    sig = apply(osc(n, "sine", 100.0), expdecay(n, 0.060, attack_s=0.002))
    grit_n = int(0.030 * SR)
    grit = apply(osc(grit_n, "square", 300.0),
                 env_adsr(grit_n, 0.003, 0.027, 0.0, 0.0))
    add_into(sig, grit, 0.0, db(-12))
    return biquad(sig, "lp", fc=2000.0)


def r_death_burst(rng, n):
    # the de-rez: fm(900->110 over 0.4 s, ratio 2.01, index 4->0), amplitude
    # gated by a 30 Hz square (pixel dissolve), + noise LP sweep 6k->400 Hz;
    # last 0.2 s is gate-fragments only.
    f_fn = glide(900.0, 110.0, 0.40, "exp")
    body = fm_osc(n, 2.01, 4.0, 0.0, 0.40, f_fn)
    noise = biquad(white(int(0.40 * SR), rng), "lp",
                   fc_fn=glide(6000.0, 400.0, 0.40, "exp"))
    sig = body
    add_into(sig, noise, 0.0, 0.5)
    # pixel-dissolve gate; after 0.4 s windows drop out at random and the
    # remaining fragments decay -- the corpse "de-rezzes" to silence.
    frag_amp = 1.0
    out = zeros(n)
    win = SR // 30  # one 30 Hz gate period
    for w0 in range(0, n, win):
        t = w0 / SR
        keep = True
        amp = 1.0
        if t >= 0.40:
            keep = rng.random() < 0.55
            frag_amp *= 0.72
            amp = frag_amp
        if keep:
            half = win // 2  # square gate: open first half of the period
            for i in range(w0, min(w0 + half, n)):
                out[i] = sig[i] * amp
    return out


def r_respawn(rng, n):
    # sines 220+330 gliding to 440+660 over 0.35 s (a rising fifth), attack
    # 80 ms, + detuned copies +-6 cents for shimmer, HP 150 Hz.  The copies
    # start in quadrature (phase 0.25/0.75): all-zero phases would let the
    # +-6 cent pair drift to simultaneous antiphase right around t=0.33 s and
    # null the fundamental exactly when the rising fifth arrives.
    sig = zeros(n)
    for f0, f1, g in ((220.0, 440.0, 1.0), (330.0, 660.0, 0.8)):
        for c, cg, p0 in ((0.0, 1.0, 0.0), (-6.0, 0.5, 0.25), (6.0, 0.5, 0.75)):
            v = osc(n, "sine", cents(f0, c), cents(f1, c), 0.35, "exp", phase0=p0)
            add_into(sig, v, 0.0, g * cg)
    sig = apply(sig, env_adsr(n, 0.080, 0.0, 1.0, 0.15))
    return biquad(sig, "hp", fc=150.0)


def r_shield_block(rng, n):
    # ring(3400,50ms)+ring(5100,35ms) glassy clink, HP 1 kHz -- no low end at
    # all (that absence is what says "no damage").
    sig = zeros(n)
    add_into(sig, ring(3400.0, 0.050), 0.0, 1.0)
    add_into(sig, ring(5100.0, 0.035), 0.0, 0.8)
    return biquad(sig, "hp", fc=1000.0)


# ---- Abilities (mono) -----------------------------------------------------

def r_chut_bash(rng, n):
    # sine 140->70 tau 120 ms boom + noise crack BP 1-3 kHz 30 ms + whoosh
    # tail BP 600->250 Hz 200 ms.  Softclip drive 3.
    sig = apply(osc(n, "sine", 140.0, 70.0, 0.12, "exp"),
                expdecay(n, 0.120, attack_s=0.003))
    crack_n = int(0.030 * SR)
    add_into(sig, apply(bp_band(white(crack_n, rng), 1000.0, 3000.0),
                        env_adsr(crack_n, 0.002, 0.028, 0.0, 0.0)), 0.0, 0.9)
    wh_n = int(0.200 * SR)
    whoosh = apply(biquad(white(wh_n, rng), "bp", q=1.0,
                          fc_fn=glide(600.0, 250.0, 0.20, "exp")),
                   env_adsr(wh_n, 0.040, 0.160, 0.0, 0.0))
    add_into(sig, whoosh, 0.06, 0.6)
    return softclip(sig, 3.0)


def r_mace_spike_throw(rng, n):
    # whip: noise BP swept 800->2400->600 Hz over 0.2 s + ring(3100,40ms)
    # +ring(4700,30ms) metal ching at t=0.05.
    def whip_fc(t):
        u = min(1.0, t / 0.20)
        if u < 0.5:
            return 800.0 * ((2400.0 / 800.0) ** (u / 0.5))
        return 2400.0 * ((600.0 / 2400.0) ** ((u - 0.5) / 0.5))

    wn = int(0.20 * SR)
    sig = zeros(n)
    add_into(sig, apply(biquad(white(wn, rng), "bp", q=1.4, fc_fn=whip_fc),
                        env_adsr(wn, 0.015, 0.16, 0.0, 0.0)), 0.0)
    add_into(sig, ring(3100.0, 0.040), 0.05, 0.7)
    add_into(sig, ring(4700.0, 0.030), 0.05, 0.5)
    return sig


def r_mace_spike_embed(rng, n):
    # sine 120 Hz tau 50 ms thunk + ring(2200, 150ms) embed ring.
    sig = apply(osc(n, "sine", 120.0), expdecay(n, 0.050, attack_s=0.002))
    add_into(sig, ring(2200.0, 0.150), 0.0, 0.45)
    return sig


def r_mace_pull_loop(rng, n):
    # BP-noise 400-900 Hz with 6 Hz tremolo (depth 0.4).  Tremolo snapped to
    # 10 cycles / 1.6 s = 6.25 Hz so the modulator is loop-periodic.
    fade = 4096
    ext = bp_band(white(n + fade, rng), 400.0, 900.0)
    ftr = 10.0 / (n / SR)
    for i in range(len(ext)):
        t = i / SR
        ext[i] *= 1.0 - 0.2 + 0.2 * math.sin(TWO_PI * ftr * t)  # depth 0.4
    return loop_wrap(ext, n, fade)


def r_oyster_pickler(rng, n):
    # wet bloop: sine bends 180->320 (0-0.12 s) then 320->140 (0.12-0.25 s)
    # + 30 ms noise splash LP 1 kHz at t=0.
    def bloop_f(t):
        if t < 0.12:
            return 180.0 + (320.0 - 180.0) * (t / 0.12)
        if t < 0.25:
            return 320.0 + (140.0 - 320.0) * ((t - 0.12) / 0.13)
        return 140.0

    sig = apply(osc_fn(n, "sine", bloop_f),
                env_seg(n, [(0.0, 0.0), (0.01, 1.0), (0.25, 0.9), (0.30, 0.0)]))
    spl_n = int(0.030 * SR)
    add_into(sig, apply(biquad(white(spl_n, rng), "lp", fc=1000.0),
                        env_adsr(spl_n, 0.003, 0.027, 0.0, 0.0)), 0.0, 0.6)
    return sig


def r_oyster_jar_break(rng, n):
    # 12 random partials ring(f in [1500,6000], tau in [5,40]ms) (shatter)
    # + fizz: HP-noise 3 kHz expdecay tau 120 ms + low bloop sine 200->90 -10 dB.
    sig = zeros(n)
    for _ in range(12):
        f = rng.uniform(1500.0, 6000.0)
        tau = rng.uniform(0.005, 0.040)
        at = rng.uniform(0.0, 0.020)
        add_into(sig, ring(f, tau), at, rng.uniform(0.35, 0.85))
    fizz = apply(biquad(white(n, rng), "hp", fc=3000.0),
                 expdecay(n, 0.120))
    add_into(sig, fizz, 0.0, 0.5)
    bl_n = int(0.15 * SR)
    bloop = apply(osc(bl_n, "sine", 200.0, 90.0, 0.12, "exp"),
                  expdecay(bl_n, 0.060, attack_s=0.005))
    add_into(sig, bloop, 0.02, db(-10) * 3.0)
    return sig


def r_x_sting(rng, n):
    # zap: fm(2800, ratio 7, index 6->0, 100ms) + saw 180 Hz 60 ms buzz -10 dB.
    zap_n = int(0.10 * SR)
    zap = apply(fm_osc(zap_n, 7.0, 6.0, 0.0, 0.10, glide(2800.0)),
                env_adsr(zap_n, 0.002, 0.098, 0.0, 0.0))
    sig = zeros(n)
    add_into(sig, zap, 0.0)
    buzz_n = int(0.060 * SR)
    add_into(sig, apply(osc(buzz_n, "saw", 180.0),
                        env_adsr(buzz_n, 0.005, 0.055, 0.0, 0.0)),
             0.0, db(-10))
    return sig


def r_x_sting_load(rng, n):
    # five clicks ring(1200..2000 ascending, tau 25ms) 40 ms apart + wing-buzz:
    # saw 100 Hz with 25 Hz tremolo swelling 0 -> -8 dB.
    sig = zeros(n)
    for k in range(5):
        add_into(sig, ring(1200.0 + 200.0 * k, 0.025), 0.040 * k, 0.8)
    buzz = osc(n, "saw", 100.0)
    for i in range(n):
        t = i / SR
        trem = 1.0 - 0.35 + 0.35 * math.sin(TWO_PI * 25.0 * t)
        swell = db(-8) * min(1.0, t / 0.38)
        buzz[i] *= trem * swell
    add_into(sig, biquad(buzz, "lp", fc=3000.0), 0.0)
    return sig


def r_roxie_rocket_burst(rng, n):
    # sine 100->40 tau 250 ms boom + dense noise LP sweep 8k->300 over 0.5 s
    # + crackle: Poisson impulse train (lambda 60/s, decaying) through BP 2 kHz.
    # Softclip drive 4.
    sig = apply(osc(n, "sine", 100.0, 40.0, 0.25, "exp"),
                expdecay(n, 0.250, attack_s=0.002))
    add_into(sig, apply(biquad(white(n, rng), "lp",
                               fc_fn=glide(8000.0, 300.0, 0.50, "exp")),
                        expdecay(n, 0.300, attack_s=0.002)), 0.0, 0.9)
    # crackle: arrival rate 60/s decaying (rate halves every ~0.25 s)
    imp = zeros(n)
    t = 0.0
    while t < n / SR:
        lam = 60.0 * math.exp(-t / 0.35)
        if lam < 2.0:
            break
        t += rng.expovariate(lam)
        j = int(t * SR)
        if j < n:
            a = rng.uniform(0.4, 1.0) * math.exp(-t / 0.30)
            for k in range(int(0.002 * SR)):
                if j + k < n:
                    imp[j + k] += a * rng.uniform(-1.0, 1.0)
    add_into(sig, biquad(imp, "bp", fc=2000.0, q=1.0), 0.0, 1.2)
    return softclip(sig, 4.0)


def r_roxie_rocket_launch(rng, n):
    # sine 90->45 Hz thump + noise roar LP 2 kHz expdecay tau 200 ms.
    sig = apply(osc(n, "sine", 90.0, 45.0, 0.20, "exp"),
                expdecay(n, 0.150, attack_s=0.003))
    add_into(sig, apply(biquad(white(n, rng), "lp", fc=2000.0),
                        expdecay(n, 0.200, attack_s=0.005)), 0.0, 0.7)
    return sig


def r_roxie_rocket_loop(rng, n):
    # pink noise BP 300-1800 Hz + sine 55 Hz rumble, 8 Hz flutter.  Flutter
    # snapped to 10 cycles / 1.2 s = 8.333 Hz (loop-periodic); the 55 Hz sine
    # is exactly 66 cycles per loop.
    fade = 4096
    ext = bp_band(pink(n + fade, rng), 300.0, 1800.0)
    ffl = 10.0 / (n / SR)
    for i in range(len(ext)):
        ext[i] *= 1.0 - 0.15 + 0.15 * math.sin(TWO_PI * ffl * i / SR)
    sig = loop_wrap(gained(ext, 2.2), n, fade)
    add_into(sig, osc(n, "sine", 55.0), 0.0, 0.5, wrap=True)
    return sig


def r_roxie_modded(rng, n):
    # two mech clicks (ring(1400)+ring(900), 60 ms apart) + rising 3-step
    # square arp 300/560/900 Hz, 70 ms each, LP 3 kHz.
    sig = zeros(n)
    for at in (0.0, 0.060):
        add_into(sig, ring(1400.0, 0.030), at, 0.8)
        add_into(sig, ring(900.0, 0.030), at, 0.7)
    arp = zeros(n)
    for k, f in enumerate((300.0, 560.0, 900.0)):
        nn = int(0.070 * SR)
        note = apply(osc(nn, "square", f), env_adsr(nn, 0.005, 0.0, 1.0, 0.020))
        add_into(arp, note, 0.130 + 0.070 * k, 0.6)
    add_into(sig, biquad(arp, "lp", fc=3000.0), 0.0)
    return sig


def r_elle_teleport(rng, n):
    # warp: sine glide 400->1600 (0-0.15) -> 200 (0.15-0.3) with FM shimmer
    # (ratio 1.41, index 3) + HP-noise whoosh 2 kHz tau 150 ms.
    def warp_f(t):
        if t < 0.15:
            return 400.0 * ((1600.0 / 400.0) ** (t / 0.15))
        if t < 0.30:
            return 1600.0 * ((200.0 / 1600.0) ** ((t - 0.15) / 0.15))
        return 200.0

    body = fm_osc(n, 1.41, 3.0, 3.0, 1.0, warp_f)
    sig = apply(body, env_seg(n, [(0.0, 0.0), (0.01, 1.0), (0.30, 0.8), (0.35, 0.0)]))
    add_into(sig, apply(biquad(white(n, rng), "hp", fc=2000.0),
                        expdecay(n, 0.150, attack_s=0.005)), 0.0, 0.4)
    return sig


def r_elle_snap(rng, n):
    # reverse-swell: noise+sine 500 Hz, amplitude ramp 0->1 over 0.25 s
    # (cosine), ending in a soft pop (10 ms sine 700 Hz).
    bed = osc(n, "sine", 500.0)
    nz = biquad(white(n, rng), "lp", fc=3000.0)
    for i in range(n):
        t = i / SR
        w = 0.5 - 0.5 * math.cos(math.pi * min(1.0, t / 0.25))
        if t > 0.25:  # after the swell peaks, duck fast so the pop reads
            w *= max(0.0, 1.0 - (t - 0.25) / 0.02)
        bed[i] = (bed[i] * 0.7 + nz[i] * 0.35) * w
    pop_n = int(0.010 * SR)
    add_into(bed, apply(osc(pop_n, "sine", 700.0),
                        env_adsr(pop_n, 0.001, 0.009, 0.0, 0.0)), 0.25, 0.8)
    return bed


def _elle_arp(freqs, n, blip_gap):
    sig = zeros(n)
    bn = int(0.040 * SR)  # 40 ms blips
    for k, f in enumerate(freqs):
        blip = apply(osc(bn, "sine", f), env_adsr(bn, 0.005, 0.0, 1.0, 0.015))
        add_into(sig, blip, k * blip_gap, 1.0)
    return sig


def r_elle_cloak(rng, n):
    # descending 5-blip arp: sines 2400->800 Hz, 40 ms each, LP closing 4k->1k.
    fr = [2400.0 * ((800.0 / 2400.0) ** (k / 4.0)) for k in range(5)]
    return biquad(_elle_arp(fr, n, 0.055), "lp",
                  fc_fn=glide(4000.0, 1000.0, 0.30, "exp"))


def r_elle_decloak(rng, n):
    # the same arp reversed (800->2400), HP 500 Hz; the +3 dB "brighter" lives
    # in the peak target (-10 vs cloak's -12).
    fr = [800.0 * ((2400.0 / 800.0) ** (k / 4.0)) for k in range(5)]
    return biquad(_elle_arp(fr, n, 0.055), "hp", fc=500.0)


def r_slimeball_wall(rng, n):
    # splat: noise LP 600 Hz 80 ms burst + three gloop sine bends 240->90
    # staggered 60 ms + drip: 2 tiny blips at 0.28/0.34 s.
    sig = zeros(n)
    sp_n = int(0.080 * SR)
    add_into(sig, apply(biquad(white(sp_n, rng), "lp", fc=600.0),
                        env_adsr(sp_n, 0.005, 0.075, 0.0, 0.0)), 0.0, 1.0)
    for k in range(3):
        f0 = 240.0 * rng.uniform(0.92, 1.08)
        gl_n = int(0.15 * SR)
        gloop = apply(osc(gl_n, "sine", f0, f0 * 90.0 / 240.0, 0.12, "exp"),
                      expdecay(gl_n, 0.070, attack_s=0.005))
        add_into(sig, gloop, 0.04 + 0.060 * k, 0.7)
    for at in (0.28, 0.34):
        bn = int(0.015 * SR)
        blip = apply(osc(bn, "sine", 450.0, 800.0, 0.015), env_adsr(bn, 0.002, 0.013, 0.0, 0.0))
        add_into(sig, blip, at, 0.3)
    return sig


def r_slimeball_stick(rng, n):
    # one gloop: sine 300->120 Hz over 120 ms + 20 ms squelch noise.
    sig = apply(osc(n, "sine", 300.0, 120.0, 0.12, "exp"),
                expdecay(n, 0.080, attack_s=0.004))
    sq_n = int(0.020 * SR)
    add_into(sig, apply(bp_band(white(sq_n, rng), 400.0, 1600.0),
                        env_adsr(sq_n, 0.003, 0.017, 0.0, 0.0)), 0.0, 0.5)
    return sig


def r_mortimer_quake(rng, n):
    # sine 80->35 tau 400 ms slam + pink-noise LP 400 Hz expdecay tau 350 ms
    # rumble + gravel: impulse train through BP 300 Hz.  Softclip drive 3.
    sig = apply(osc(n, "sine", 80.0, 35.0, 0.30, "exp"),
                expdecay(n, 0.400, attack_s=0.003))
    add_into(sig, apply(biquad(pink(n, rng), "lp", fc=400.0),
                        expdecay(n, 0.350, attack_s=0.005)), 0.0, 2.2)
    imp = zeros(n)
    t = 0.0
    while t < n / SR:
        lam = 45.0 * math.exp(-t / 0.45)
        if lam < 2.0:
            break
        t += rng.expovariate(lam)
        j = int(t * SR)
        if j < n:
            a = rng.uniform(0.5, 1.0) * math.exp(-t / 0.30)
            for k in range(int(0.003 * SR)):
                if j + k < n:
                    imp[j + k] += a * rng.uniform(-1.0, 1.0)
    add_into(sig, biquad(imp, "bp", fc=300.0, q=1.0), 0.0, 1.5)
    return softclip(sig, 3.0)


def r_mortimer_mantle(rng, n):
    # noise scrape BP 300-900 Hz 150 ms, env attack 30 ms.
    sc_n = int(0.150 * SR)
    sig = zeros(n)
    add_into(sig, apply(bp_band(white(sc_n, rng), 300.0, 900.0),
                        env_adsr(sc_n, 0.030, 0.120, 0.0, 0.0)), 0.0)
    return sig


def r_lily_zip(rng, n):
    # spool-up: sine 300->900 Hz over 0.3 s + noise swell HP 1 kHz, ends at
    # sustained hover level (the game crossfades into LilyZipLoop; only the
    # standard 5 ms fade guards the file edge).
    sig = osc(n, "sine", 300.0, 900.0, 0.30)
    nz = biquad(white(n, rng), "hp", fc=1000.0)
    for i in range(n):
        t = i / SR
        a = min(1.0, t / 0.03)  # 30 ms attack
        swell = 0.5 * min(1.0, t / 0.30)
        sig[i] = sig[i] * a * 0.8 + nz[i] * swell
    return sig


def r_lily_zip_loop(rng, n):
    # tri 220 Hz + sine 110 Hz, 5 Hz vibrato +-3 cents, + faint HP-noise air
    # 3 kHz at -26 dB.  Vibrato snapped to 8 cycles / 1.5 s = 5.333 Hz; both
    # carriers are integer-cycle (330 / 165 per loop).
    fade = 4096
    fv = 8.0 / (n / SR)

    def vib(f0):
        return lambda t: f0 * (2.0 ** ((3.0 / 1200.0) * math.sin(TWO_PI * fv * t)))

    ext_n = n + fade
    ext = osc_fn(ext_n, "tri", vib(220.0))
    ext2 = osc_fn(ext_n, "sine", vib(110.0))
    air = biquad(white(ext_n, rng), "hp", fc=3000.0)
    for i in range(ext_n):
        ext[i] = ext[i] * 0.55 + ext2[i] * 0.5 + air[i] * db(-26)
    return loop_wrap(ext, n, fade)


def r_rocco_ride_loop(rng, n):
    # BP-noise 500-1200 Hz, 10 Hz flutter (wind) -- 12 cycles per 1.2 s loop.
    fade = 4096
    ext = bp_band(white(n + fade, rng), 500.0, 1200.0)
    for i in range(len(ext)):
        ext[i] *= 1.0 - 0.25 + 0.25 * math.sin(TWO_PI * 10.0 * i / SR)
    return loop_wrap(gained(ext, 1.6), n, fade)


def r_rocco_jump(rng, n):
    # air puff: HP-noise 1.5 kHz 60 ms + sine blip 500->700 Hz 80 ms.
    pf_n = int(0.060 * SR)
    sig = zeros(n)
    add_into(sig, apply(biquad(white(pf_n, rng), "hp", fc=1500.0),
                        env_adsr(pf_n, 0.005, 0.055, 0.0, 0.0)), 0.0, 0.9)
    bl_n = int(0.080 * SR)
    add_into(sig, apply(osc(bl_n, "sine", 500.0, 700.0, 0.080),
                        env_adsr(bl_n, 0.010, 0.0, 1.0, 0.030)), 0.0, 0.6)
    return sig


# ---- UI (stereo 2D -- rendered mono, duplicated; SS5.0 stereo rule) --------

def r_ui_hover(rng, n):
    # sine 1800 Hz, attack 5 ms, decay 45 ms.
    return apply(osc(n, "sine", 1800.0), env_adsr(n, 0.005, 0.045, 0.0, 0.0))


def r_ui_back(rng, n):
    # sine 1400->900 Hz, decay 100 ms.
    return apply(osc(n, "sine", 1400.0, 900.0, 0.12),
                 env_adsr(n, 0.005, 0.100, 0.0, 0.0))


def r_ui_deny(rng, n):
    # squares 220+233 Hz (13 Hz beat), LP 1.2 kHz, hard gate at 150 ms (the
    # file IS the gate; the standard 5 ms edge fade keeps it click-free).
    sig = osc(n, "square", 220.0)
    add_into(sig, osc(n, "square", 233.0), 0.0, 1.0)
    sig = biquad(gained(sig, 0.5), "lp", fc=1200.0)
    return apply(sig, env_seg(n, [(0.0, 0.0), (0.005, 1.0), (0.150, 1.0)]))


def r_countdown_tick(rng, n):
    # sine 1000 Hz + partial 2000 Hz (-8 dB), tau 50 ms.
    sig = apply(osc(n, "sine", 1000.0), expdecay(n, 0.050, attack_s=0.002))
    add_into(sig, apply(osc(n, "sine", 2000.0), expdecay(n, 0.050, attack_s=0.002)),
             0.0, db(-8))
    return sig


def r_countdown_go(rng, n):
    # saw 600->1200 Hz through LP 800->4000 Hz + sine fifth (900/1350 Hz)
    # sustain 200 ms.
    sig = biquad(osc(n, "saw", 600.0, 1200.0, 0.35),
                 fc_fn=glide(800.0, 4000.0, 0.35, "exp"), kind="lp")
    sig = apply(sig, env_adsr(n, 0.010, 0.0, 1.0, 0.12))
    fif = zeros(n)
    add_into(fif, osc(int(0.28 * SR), "sine", 900.0), 0.0, 0.45)
    add_into(fif, osc(int(0.28 * SR), "sine", 1350.0), 0.0, 0.35)
    fif = apply(fif, env_adsr(n, 0.010, 0.0, 1.0, 0.15))
    add_into(sig, fif, 0.0)
    return sig


# ---- Music / stingers (stereo) --------------------------------------------

# note frequencies used by the stingers / title loop
_N = {
    "D2": 73.42, "Bb2": 116.54, "D3": 146.83, "F3": 174.61, "G3": 196.00,
    "A3": 220.00, "B3": 246.94, "C4": 261.63, "D4": 293.66, "E4": 329.63,
    "F4": 349.23, "G4": 392.00, "A4": 440.00, "D5": 587.33, "E3": 164.81,
    "C3": 130.81, "F1": 43.65, "A1": 55.00, "C2": 65.41, "G1": 49.00,
}


def _saw_stack_chord(tones, dur_n, det_cents, seg_env, extra_sine=0.0):
    """3-voice-per-tone detuned saw stack -> (L, R) with the detunes panned."""
    L = zeros(dur_n)
    R = zeros(dur_n)
    for f in tones:
        for c in det_cents:
            v = osc(dur_n, "saw", cents(f, c))
            if extra_sine > 0.0:
                s = osc(dur_n, "sine", f)
                v = [a + extra_sine * b for a, b in zip(v, s)]
            gl, gr = pan_lr(0.5 + c / 40.0)
            g = 1.0 / (len(tones) * len(det_cents))
            for i, val in enumerate(v):
                e = seg_env[i]
                L[i] += val * gl * g * e
                R[i] += val * gr * g * e
    return L, R


def r_stinger_victory(rng, n):
    # saw stack x3 voices +-8 cents, triad rise D3+A3+D4 -> A3+E4+A4 ->
    # D4+A4+D5 held; LP opens 1.2->6 kHz; HP-noise shimmer 6 kHz fading over
    # the last second; delay(3/16 @ 110 BPM, fb 0.3, mix 0.2), ping-pong L/R.
    segs = [((0.0, 0.9), ("D3", "A3", "D4")),
            ((0.9, 1.8), ("A3", "E4", "A4")),
            ((1.8, 2.8), ("D4", "A4", "D5"))]
    L = zeros(n)
    R = zeros(n)
    for (t0, t1), names in segs:
        dur = t1 - t0 + 0.25  # release tail overlaps the next chord
        dn = min(int(dur * SR), n - int(t0 * SR))
        e = env_seg(dn, [(0.0, 0.0), (0.040, 1.0), (t1 - t0, 1.0), (dur, 0.0)])
        cl, cr = _saw_stack_chord([_N[x] for x in names], dn, (-8.0, 0.0, 8.0), e)
        add_into(L, cl, t0)
        add_into(R, cr, t0)
    # ping-pong delay: 3/16 of a bar at 110 BPM = 0.4091 s
    d = (60.0 / 110.0) * 4.0 * 3.0 / 16.0
    mono = [(a + b) * 0.5 for a, b in zip(L, R)]
    for k in range(1, 6):
        g = 0.2 * (0.3 ** (k - 1))
        dst = L if k % 2 else R
        add_into(dst, mono, d * k, g)
    lp = glide(1200.0, 6000.0, 2.8, "exp")
    L = biquad(L, "lp", fc_fn=lp, q=0.8)
    R = biquad(R, "lp", fc_fn=lp, q=0.8)
    # shimmer over the last second, decorrelated
    for ch, r2 in ((L, rng), (R, rng)):
        sh = biquad(white(int(1.0 * SR), r2), "hp", fc=6000.0)
        sh = apply(sh, env_seg(len(sh), [(0.0, 1.0), (1.0, 0.0)]))
        add_into(ch, sh, 1.8, 0.10)
    # gentle overall fade on the held final chord
    out_env = env_seg(n, [(0.0, 1.0), (2.1, 1.0), (2.8, 0.0)])
    return apply(L, out_env), apply(R, out_env)


def r_stinger_defeat(rng, n):
    # dark saw+sine, D minor fall Dm -> Bb triad -> low D2 alone; LP closes
    # 3k->500 Hz; 4 Hz amplitude wobble on the tail.
    segs = [((0.0, 0.9), ("D3", "F3", "A3")),
            ((0.9, 1.8), ("Bb2", "D3", "F3")),
            ((1.8, 2.8), ("D2",))]
    L = zeros(n)
    R = zeros(n)
    for (t0, t1), names in segs:
        dur = t1 - t0 + 0.30
        dn = min(int(dur * SR), n - int(t0 * SR))
        e = env_seg(dn, [(0.0, 0.0), (0.060, 1.0), (t1 - t0, 1.0), (dur, 0.0)])
        cl, cr = _saw_stack_chord([_N[x] for x in names], dn, (-8.0, 0.0, 8.0),
                                  e, extra_sine=0.7)
        add_into(L, cl, t0)
        add_into(R, cr, t0)
    lp = glide(3000.0, 500.0, 2.8, "exp")
    L = biquad(L, "lp", fc_fn=lp, q=0.8)
    R = biquad(R, "lp", fc_fn=lp, q=0.8)
    for ch in (L, R):  # 4 Hz wobble on the tail
        for i in range(int(1.8 * SR), n):
            t = i / SR
            ch[i] *= 0.7 + 0.3 * math.sin(TWO_PI * 4.0 * (t - 1.8))
    out_env = env_seg(n, [(0.0, 1.0), (2.2, 1.0), (2.8, 0.0)])
    return apply(L, out_env), apply(R, out_env)


def r_music_title(rng, n):
    """64.0 s = 32 bars of 4/4 at 120 BPM, stereo, A minor (SS5.4).
    Four 8-bar layers, all wrap-safe by event composition mod N:
      1 bass (A-F-C-G roots, eighth notes, side-chain pump)  bars 1-32
      2 pad  (7-voice saw stack, Am-F-C-G, 16 s LP sweep)    bars 1-32
      3 arp  (16th FM plucks, pentatonic, ping-pong delay)   bars 9-24
      4 hats (off-beat HP-noise ticks)                       bars 9-32
    Bars 25-32 drop the arp so the wrap into bar 1 is a natural breakdown."""
    L = zeros(n)
    R = zeros(n)
    bar = 2.0  # seconds at 120 BPM

    # -- layer 1: bass -- sine+square blend 0.7/0.3, eighths, roots per 2 bars
    bass = zeros(n)
    roots = [_N["A1"], _N["F1"], _N["C2"], _N["G1"]]
    note_n = int(0.29 * SR)  # 0.25 s note + release into the next
    note_env = env_seg(note_n, [(0.0, 0.0), (0.006, 1.0), (0.20, 0.85),
                                (0.24, 0.7), (0.29, 0.0)])
    note_cache = {}
    for b in range(32):
        f = roots[(b // 2) % 4]
        if f not in note_cache:
            s1 = osc(note_n, "sine", f)
            s2 = osc(note_n, "square", f)
            # LP 1.2 kHz on the note: the raw square's upper harmonics put a
            # constant fizz in the 6 kHz band that buries the -22 dB hats;
            # a filtered blend keeps the growl and leaves that band to them.
            note = biquad([0.7 * a + 0.3 * c for a, c in zip(s1, s2)],
                          "lp", fc=1200.0)
            note_cache[f] = apply(note, note_env)
        for e in range(8):
            add_into(bass, note_cache[f], b * bar + e * 0.25, 1.0, wrap=True)
    # pump: 120 ms dip after beats 1 and 3 of every bar (loop-periodic: the
    # modulator's period is exactly one bar, 32 bars per loop).  The dip falls
    # over 8 ms rather than instantly -- a zero-width drop on a 55 Hz bass
    # clicks audibly at every beat (and at the loop seam, which lands on one).
    for i in range(n):
        tb = (i / SR) % bar
        dt = tb if tb < 1.0 else tb - 1.0
        if dt < 0.008:
            bass[i] *= 1.0 - 0.65 * (dt / 0.008)
        elif dt < 0.12:
            bass[i] *= 0.35 + 0.65 * ((dt - 0.008) / 0.112)
    add_into(L, bass, 0.0, 0.707)
    add_into(R, bass, 0.0, 0.707)

    # -- layer 2: pad -- 7-voice saw stack +-12 cents, Am-F-C-G 2 bars each,
    # LP swept 400->2400->400 per 8 bars, -16 dB
    chords = {"Am": ("A3", "C4", "E4"), "F": ("F3", "A3", "C4"),
              "C": ("C4", "E4", "G4"), "G": ("G3", "B3", "D4")}
    seq = ["Am", "F", "C", "G"]
    dets = [-12.0, -8.0, -4.0, 0.0, 4.0, 8.0, 12.0]

    def pad_lp(t):
        u = (t % 16.0) / 16.0
        return 400.0 * (6.0 ** (0.5 - 0.5 * math.cos(TWO_PI * u)))

    pad_gain = db(-16) * 7.0  # -16 dB against the bass, pre 1/7 voice split
    ev_n = int(4.6 * SR)  # 4 s chord + 0.6 s release into the next
    ev_env = env_seg(ev_n, [(0.0, 0.0), (0.25, 1.0), (4.0, 1.0), (4.6, 0.0)])
    for ci in range(16):
        t0 = ci * 4.0
        tones = [_N[x] for x in chords[seq[ci % 4]]]
        chL = zeros(ev_n)
        chR = zeros(ev_n)
        for k, c in enumerate(dets):
            v = osc(ev_n, "saw", cents(tones[k % 3], c))
            gl, gr = pan_lr(0.5 + c / 60.0)
            for i, val in enumerate(v):
                e = ev_env[i] / 7.0
                chL[i] += val * gl * e
                chR[i] += val * gr * e
        fcf = (lambda off: (lambda t: pad_lp(off + t)))(t0)
        chL = biquad(chL, "lp", fc_fn=fcf, q=0.8)
        chR = biquad(chR, "lp", fc_fn=fcf, q=0.8)
        add_into(L, chL, t0, pad_gain, wrap=True)
        add_into(R, chR, t0, pad_gain, wrap=True)

    # -- layer 3: arp -- 16th FM plucks (ratio 3, index 5->0 in 60 ms) over
    # A-minor-pentatonic [A3 C4 D4 E4 G4 E4 D4 C4], delay 3/16 fb .35 mix .3,
    # ping-pong L/R; bars 9-24 only
    pat = ("A3", "C4", "D4", "E4", "G4", "E4", "D4", "C4")
    dly = bar * 3.0 / 16.0  # 0.375 s
    arp_gain = db(-10)
    pl_n = int(0.15 * SR)
    pl_env = expdecay(pl_n, 0.055, attack_s=0.002)
    pluck_cache = {}
    idx = 0
    for b in range(8, 24):
        for s16 in range(16):
            name = pat[idx % 8]
            if name not in pluck_cache:
                pluck_cache[name] = apply(
                    fm_osc(pl_n, 3.0, 5.0, 0.0, 0.060, glide(_N[name])), pl_env)
            pl = pluck_cache[name]
            base = 0.32 if idx % 2 == 0 else 0.68  # ping-pong note placement
            t0 = b * bar + s16 * 0.125
            for k in range(6):  # dry + 5 delay repeats, sides alternating
                g = arp_gain * (1.0 if k == 0 else 0.3 * (0.35 ** (k - 1)))
                p = base if k % 2 == 0 else 1.0 - base
                gl, gr = pan_lr(p)
                add_into(L, pl, t0 + dly * k, g * gl, wrap=True)
                add_into(R, pl, t0 + dly * k, g * gr, wrap=True)
            idx += 1

    # -- layer 4: hats -- HP-noise 6 kHz 30 ms ticks on off-beats, -22 dB;
    # bars 9-32 (the 25-32 breakdown keeps them so time stays audible)
    tick_n = int(0.030 * SR)
    ticks = []
    for _ in range(4):  # four variants, cycled, so the ride isn't machine-gun
        tk = apply(biquad(white(tick_n, rng), "hp", fc=6000.0),
                   env_adsr(tick_n, 0.002, 0.028, 0.0, 0.0))
        ticks.append(tk)
    hat_gain = db(-22) * 3.0
    ti = 0
    for b in range(8, 32):
        for bt in range(4):
            t0 = b * bar + bt * 0.5 + 0.25
            gl, gr = pan_lr(0.45 if ti % 2 == 0 else 0.55)
            add_into(L, ticks[ti % 4], t0, hat_gain * gl, wrap=True)
            add_into(R, ticks[ti % 4], t0, hat_gain * gr, wrap=True)
            ti += 1

    return L, R


def r_ambience_match(rng, n):
    """48.0 s stereo near-silent machine-room bed (SS5.4).  Relative levels
    carry the plan's absolute intents (bed -30 / pink -36 / chirps -28); the
    final RMS normalization pins the whole file at -30 dBFS."""
    L = zeros(n)
    R = zeros(n)
    dur = n / SR

    # bed: sines 50+100 Hz beating at 0.3 Hz -- snapped to 14 cycles / 48 s
    # (0.2917 Hz) so the beat is loop-periodic; carriers are integer-cycle.
    s50 = osc(n, "sine", 50.0)
    s100 = osc(n, "sine", 100.0)
    fb = 14.0 / dur
    bed = zeros(n)
    for i in range(n):
        am = 0.75 + 0.25 * math.sin(TWO_PI * fb * i / SR)
        bed[i] = (0.6 * s50[i] + 0.4 * s100[i]) * am
    add_into(L, bed, 0.0, 0.707)
    add_into(R, bed, 0.0, 0.707)

    # pink noise LP 400 Hz at -6 dB rel, decorrelated per channel, seam-wrapped
    fade = 8192
    for ch in (L, R):
        pl = loop_wrap(biquad(pink(n + fade, rng), "lp", fc=400.0), n, fade)
        add_into(ch, pl, 0.0, db(-6) * 2.5, wrap=True)

    # sparse data chirps: every 3-7 s (seeded), FM blip f in [1200,2400],
    # 90 ms, +2 dB rel, random constant pan
    t = rng.uniform(3.0, 7.0)
    ch_n = int(0.090 * SR)
    while t < dur - 0.2:
        f = rng.uniform(1200.0, 2400.0)
        blip = apply(fm_osc(ch_n, 1.5, 2.0, 0.0, 0.090, glide(f)),
                     env_adsr(ch_n, 0.010, 0.0, 1.0, 0.040))
        gl, gr = pan_lr(rng.random())
        g = db(2.0)
        add_into(L, blip, t, g * gl, wrap=True)
        add_into(R, blip, t, g * gr, wrap=True)
        t += rng.uniform(3.0, 7.0)

    # 35 Hz sub swell, 4 s cosine window, at t=16 and t=40 (140 cycles each)
    sw_n = int(4.0 * SR)
    swell = osc(sw_n, "sine", 35.0)
    for i in range(sw_n):
        swell[i] *= 0.5 - 0.5 * math.cos(TWO_PI * i / sw_n)
    for at in (16.0, 40.0):
        add_into(L, swell, at, 0.85, wrap=True)
        add_into(R, swell, at, 0.85, wrap=True)

    return L, R


# ===========================================================================
# The palette table -- SS5.1, in table order.
# fields: stem, folder, seconds, channels, norm, loop, side (documentation for
# W2-AUDIOBANK's TraceSoundEvents rows; C++ stays the authority).
# norm: ("peak", dBFS)  or  ("rms", dBFS, peak_cap_dBFS)
# ===========================================================================

SPEC = [
    # -- core combat --
    ("MeleeSwing",       "Combat",    0.16, 1, ("peak",  -4.0), False, "W (excluding)", r_melee_swing),
    ("MeleeHit",         "Combat",    0.14, 1, ("peak",  -3.0), False, "W",             r_melee_hit),
    ("MeleeBackstab",    "Combat",    0.35, 1, ("peak",  -3.0), False, "W",             r_melee_backstab),
    ("Reload",           "Combat",    0.50, 1, ("peak",  -8.0), False, "W (excluding)", r_reload),
    ("DryFire",          "Combat",    0.09, 1, ("peak", -10.0), False, "C",             r_dry_fire),
    ("WeaponSwitch",     "Combat",    0.18, 1, ("peak", -12.0), False, "C (repl-local)", r_weapon_switch),
    ("DamageTaken",      "Combat",    0.12, 1, ("peak",  -6.0), False, "C",             r_damage_taken),
    ("DeathBurst",       "Combat",    0.60, 1, ("peak",  -4.0), False, "W",             r_death_burst),
    ("Respawn",          "Combat",    0.50, 1, ("peak", -12.0), False, "C",             r_respawn),
    ("ShieldBlock",      "Combat",    0.12, 1, ("peak", -10.0), False, "C",             r_shield_block),
    # -- abilities --
    ("ChutBash",         "Abilities", 0.30, 1, ("peak",  -3.0), False, "C (burst)",     r_chut_bash),
    ("MaceSpikeThrow",   "Abilities", 0.25, 1, ("peak",  -6.0), False, "W",             r_mace_spike_throw),
    ("MaceSpikeEmbed",   "Abilities", 0.20, 1, ("peak",  -6.0), False, "C (burst)",     r_mace_spike_embed),
    ("MacePullLoop",     "Abilities", 1.60, 1, ("peak", -14.0), True,  "C (loop)",      r_mace_pull_loop),
    ("OysterPickler",    "Abilities", 0.30, 1, ("peak",  -6.0), False, "W",             r_oyster_pickler),
    ("OysterJarBreak",   "Abilities", 0.35, 1, ("peak",  -5.0), False, "C (repl-local)", r_oyster_jar_break),
    ("XSting",           "Abilities", 0.15, 1, ("peak",  -8.0), False, "C (burst)",     r_x_sting),
    ("XStingLoad",       "Abilities", 0.40, 1, ("peak",  -8.0), False, "W",             r_x_sting_load),
    ("RoxieRocketBurst", "Abilities", 0.70, 1, ("peak",  -2.0), False, "C (burst, Big)", r_roxie_rocket_burst),
    ("RoxieRocketLaunch", "Abilities", 0.40, 1, ("peak", -5.0), False, "W",             r_roxie_rocket_launch),
    ("RoxieRocketLoop",  "Abilities", 1.20, 1, ("peak", -12.0), True,  "C (loop)",      r_roxie_rocket_loop),
    ("RoxieModded",      "Abilities", 0.35, 1, ("peak",  -8.0), False, "W",             r_roxie_modded),
    ("ElleTeleport",     "Abilities", 0.35, 1, ("peak",  -6.0), False, "C (burst x2)",  r_elle_teleport),
    ("ElleSnap",         "Abilities", 0.30, 1, ("peak",  -8.0), False, "C (repl-local)", r_elle_snap),
    ("ElleCloak",        "Abilities", 0.30, 1, ("peak", -12.0), False, "W",             r_elle_cloak),
    ("ElleDecloak",      "Abilities", 0.30, 1, ("peak", -10.0), False, "W",             r_elle_decloak),
    ("SlimeballWall",    "Abilities", 0.40, 1, ("peak",  -4.0), False, "W",             r_slimeball_wall),
    ("SlimeballStick",   "Abilities", 0.20, 1, ("peak", -14.0), False, "W",             r_slimeball_stick),
    ("MortimerQuake",    "Abilities", 0.80, 1, ("peak",  -2.0), False, "W (Big)",       r_mortimer_quake),
    ("MortimerMantle",   "Abilities", 0.20, 1, ("peak", -14.0), False, "C",             r_mortimer_mantle),
    ("LilyZip",          "Abilities", 0.40, 1, ("peak",  -6.0), False, "W",             r_lily_zip),
    ("LilyZipLoop",      "Abilities", 1.50, 1, ("peak", -14.0), True,  "C (loop)",      r_lily_zip_loop),
    ("RoccoRideLoop",    "Abilities", 1.20, 1, ("peak", -14.0), True,  "C (loop)",      r_rocco_ride_loop),
    ("RoccoJump",        "Abilities", 0.15, 1, ("peak", -12.0), False, "W",             r_rocco_jump),
    # -- UI (2D => stereo per SS5.0) --
    ("UIHover",          "UI",        0.06, 2, ("peak", -14.0), False, "C (2D)",        r_ui_hover),
    ("UIBack",           "UI",        0.12, 2, ("peak", -12.0), False, "C (2D)",        r_ui_back),
    ("UIDeny",           "UI",        0.15, 2, ("peak", -10.0), False, "C (2D)",        r_ui_deny),
    ("CountdownTick",    "UI",        0.08, 2, ("peak",  -8.0), False, "C (2D)",        r_countdown_tick),
    ("CountdownGo",      "UI",        0.35, 2, ("peak",  -6.0), False, "C (2D)",        r_countdown_go),
    # -- music / stingers --
    ("StingerVictory",   "Music",     2.80, 2, ("peak",  -6.0), False, "C (2D stereo)", r_stinger_victory),
    ("StingerDefeat",    "Music",     2.80, 2, ("peak",  -6.0), False, "C (2D stereo)", r_stinger_defeat),
    ("MusicTitle",       "Music",    64.00, 2, ("rms", -12.0, -3.0), True, "C (2D loop)", r_music_title),
    ("AmbienceMatch",    "Music",    48.00, 2, ("rms", -30.0, -1.0), True, "C (2D loop)", r_ambience_match),
]

LOOPING_STEMS = frozenset(s[0] for s in SPEC if s[5])


# ---------------------------------------------------------------------------
# post-processing, quantization, WAV I/O
# ---------------------------------------------------------------------------

def _dc_remove(sig):
    m = sum(sig) / len(sig)
    if abs(m) > 1e-9:
        for i in range(len(sig)):
            sig[i] -= m
    return sig


def _peak(chans):
    return max(max(abs(v) for v in ch) for ch in chans)


def _rms(chans):
    total = 0.0
    count = 0
    for ch in chans:
        total += sum(v * v for v in ch)
        count += len(ch)
    return math.sqrt(total / count) if count else 0.0


def render_stem(entry):
    """Render one SPEC entry -> list of float channels, fully post-processed
    (exact length, DC-free, faded, normalized)."""
    stem, folder, secs, nch, norm, loop, _side, fn = entry
    n = int(round(secs * SR))
    rng = random.Random("trace-audio-%d:%s" % (PROJECT_SEED, stem))
    sig = fn(rng, n)
    chans = list(sig) if isinstance(sig, tuple) else [sig]
    # exact length (renderers already size to n; this is a hard guarantee)
    for ci in range(len(chans)):
        ch = chans[ci]
        if len(ch) < n:
            ch = ch + [0.0] * (n - len(ch))
        chans[ci] = ch[:n]
    if nch == 2 and len(chans) == 1:
        chans = [chans[0], list(chans[0])]  # UI 2D: dual-mono stereo
    assert len(chans) == nch, stem

    for ch in chans:
        _dc_remove(ch)
    if not loop:
        for ch in chans:
            fade_out(ch, 0.005)  # SS5.2: one-shots get a 5 ms cosine fade-out

    if norm[0] == "peak":
        target = db(norm[1])
        pk = _peak(chans)
        if pk > 0.0:
            g = target / pk
            for ch in chans:
                for i in range(len(ch)):
                    ch[i] *= g
    else:  # ("rms", dBFS, peak_cap)
        target = db(norm[1])
        cap = db(norm[2])
        r = _rms(chans)
        if r > 0.0:
            g = target / r
            for ch in chans:
                for i in range(len(ch)):
                    ch[i] *= g
        # tame stray peaks with gentle saturation, then re-pin the RMS; if the
        # cap still binds, scale under it (reported RMS then tells the truth)
        for _ in range(2):
            if _peak(chans) <= cap:
                break
            chans = [softclip(ch, 1.3) for ch in chans]
            r = _rms(chans)
            g = target / r
            for ch in chans:
                for i in range(len(ch)):
                    ch[i] *= g
        pk = _peak(chans)
        if pk > cap:
            g = cap / pk
            for ch in chans:
                for i in range(len(ch)):
                    ch[i] *= g
    return chans


def encode_wav(chans, stem):
    """Float channels -> 16-bit PCM frames with deterministic TPDF dither."""
    drng = random.Random("trace-dither-%d:%s" % (PROJECT_SEED, stem))
    n = len(chans[0])
    nch = len(chans)
    ints = array.array("h", bytes(2 * n * nch))
    rr = drng.random
    k = 0
    for i in range(n):
        for ch in chans:
            v = ch[i] * 32767.0 + (rr() - rr())  # TPDF, 1 LSB
            iv = int(round(v))
            if iv > 32767:
                iv = 32767
            elif iv < -32768:
                iv = -32768
            ints[k] = iv
            k += 1
    if sys.byteorder == "big":
        ints.byteswap()
    return ints.tobytes()


def wav_bytes(chans, stem):
    import io
    buf = io.BytesIO()
    w = wave.open(buf, "wb")
    w.setnchannels(len(chans))
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(encode_wav(chans, stem))
    w.close()
    return buf.getvalue()


def target_path(entry):
    return os.path.join(SOUND_DIR, entry[1], entry[0] + ".wav")


# ---------------------------------------------------------------------------
# analysis (for the printed table and --verify)
# ---------------------------------------------------------------------------

def read_wav(path):
    w = wave.open(path, "rb")
    nch = w.getnchannels()
    n = w.getnframes()
    raw = w.readframes(n)
    rate = w.getframerate()
    width = w.getsampwidth()
    w.close()
    ints = array.array("h")
    ints.frombytes(raw)
    if sys.byteorder == "big":
        ints.byteswap()
    chans = [[0.0] * n for _ in range(nch)]
    for i in range(n):
        for c in range(nch):
            chans[c][i] = ints[i * nch + c] / 32768.0
    return rate, width, chans


def analyze(entry, chans):
    """Returns (row_dict, failures list)."""
    stem, folder, secs, nch, norm, loop, _side, _fn = entry
    n = len(chans[0])
    fails = []
    dur = n / SR
    if n != int(round(secs * SR)):
        fails.append("duration %d != spec %d samples" % (n, int(round(secs * SR))))
    pk = _peak(chans)
    pk_db = lin_to_db(pk)
    r_db = lin_to_db(_rms(chans))
    dc = max(abs(sum(ch) / len(ch)) for ch in chans)
    if pk_db > -1.0 + 0.05:
        fails.append("peak %.2f dBFS above the -1 dBFS ship ceiling" % pk_db)
    if norm[0] == "peak":
        if abs(pk_db - norm[1]) > 0.5:
            fails.append("peak %.2f dBFS off target %.1f" % (pk_db, norm[1]))
    else:
        if abs(r_db - norm[1]) > 0.7:
            fails.append("RMS %.2f dBFS off target %.1f" % (r_db, norm[1]))
        if pk_db > norm[2] + 0.1:
            fails.append("peak %.2f dBFS above cap %.1f" % (pk_db, norm[2]))
    if dc > 0.002:
        fails.append("DC offset %.4f" % dc)
    seam = ""
    if loop:
        worst = 0.0
        for ch in chans:
            d_seam = abs(ch[0] - ch[-1])
            d_max = max(abs(ch[i + 1] - ch[i]) for i in range(n - 1))
            worst = max(worst, d_seam)
            if d_seam > max(d_max, 0.02):
                fails.append("loop seam step %.4f exceeds interior max %.4f"
                             % (d_seam, d_max))
        # windowed RMS continuity across the wrap (20 ms tail vs 20 ms head)
        wn = int(0.020 * SR)
        head = _rms([ch[:wn] for ch in chans])
        tail = _rms([ch[-wn:] for ch in chans])
        if head > 1e-6 and tail > 1e-6:
            if abs(lin_to_db(head) - lin_to_db(tail)) > 6.0:
                fails.append("seam RMS jump %.1f dB" %
                             abs(lin_to_db(head) - lin_to_db(tail)))
        seam = "%.4f" % worst
    else:
        if abs(chans[0][-1]) > 0.01:
            fails.append("one-shot does not end at silence")
    row = {"stem": stem, "folder": folder, "ch": len(chans), "dur": dur,
           "peak": pk_db, "rms": r_db, "dc": dc, "seam": seam}
    return row, fails


def print_row(row, spec_entry, extra=""):
    norm = spec_entry[4]
    tgt = ("peak %5.1f" % norm[1]) if norm[0] == "peak" else ("rms %5.1f" % norm[1])
    print("  %-17s %-9s %dch %7.2fs  peak %7.2f dBFS  rms %7.2f dBFS  dc %.4f  %s %s"
          % (row["stem"], row["folder"], row["ch"], row["dur"], row["peak"],
             row["rms"], row["dc"],
             ("seam " + row["seam"]) if row["seam"] else ("[" + tgt + "]"),
             extra))


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def collision_check(selected):
    """Refuse stem collisions: our stems must not exist anywhere under
    Art/Sounds/ except at their own target path, and must not shadow the 28
    frozen bank events (importer keys by stem, folders are filing only)."""
    ours = {e[0]: target_path(e) for e in selected}
    for stem in ours:
        if stem in EXISTING_STEMS:
            sys.exit("FATAL: stem %s collides with a frozen pre-existing bank WAV" % stem)
    for path in glob.glob(os.path.join(SOUND_DIR, "**", "*.wav"), recursive=True):
        stem = os.path.splitext(os.path.basename(path))[0]
        if stem in ours and os.path.abspath(path) != os.path.abspath(ours[stem]):
            sys.exit("FATAL: stem %s already exists at %s (would shadow %s)"
                     % (stem, path, ours[stem]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--only", metavar="STEM", action="append",
                    help="render (or verify) just this stem; repeatable")
    ap.add_argument("--verify", action="store_true",
                    help="re-render in memory, assert byte-identity with the "
                         "files on disk, print the analysis table")
    args = ap.parse_args()

    random.seed(PROJECT_SEED)  # SS5.2 project seed; per-stem streams derive from it

    selected = SPEC
    if args.only:
        want = set(args.only)
        selected = [e for e in SPEC if e[0] in want]
        missing = want - set(e[0] for e in selected)
        if missing:
            sys.exit("unknown stem(s): %s\nknown: %s"
                     % (", ".join(sorted(missing)),
                        ", ".join(e[0] for e in SPEC)))

    collision_check(selected)

    failures = 0
    if args.verify:
        print("verify: re-rendering %d stem(s), checking byte-identity + specs" % len(selected))
        for entry in selected:
            path = target_path(entry)
            if not os.path.exists(path):
                print("  %-17s MISSING %s" % (entry[0], path))
                failures += 1
                continue
            data = wav_bytes(render_stem(entry), entry[0])
            with open(path, "rb") as f:
                disk = f.read()
            ident = data == disk
            rate, width, chans = read_wav(path)
            row, fails = analyze(entry, chans)
            if rate != SR:
                fails.append("sample rate %d != %d" % (rate, SR))
            if width != 2:
                fails.append("sample width %d != 16-bit" % (width * 8))
            if not ident:
                fails.append("re-render is NOT byte-identical to disk")
            print_row(row, entry, "re-render=IDENTICAL" if ident else "re-render=DIFFERS")
            for f2 in fails:
                print("      FAIL: %s" % f2)
                failures += 1
        print("verify: %d stem(s), %d failure(s)" % (len(selected), failures))
        sys.exit(1 if failures else 0)

    print("rendering %d stem(s) into %s" % (len(selected), SOUND_DIR))
    for entry in selected:
        chans = render_stem(entry)
        data = wav_bytes(chans, entry[0])
        path = target_path(entry)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        row, fails = analyze(entry, chans)
        print_row(row, entry, "%d bytes" % len(data))
        for f2 in fails:
            print("      FAIL: %s" % f2)
            failures += 1
    if failures:
        print("%d spec failure(s)" % failures)
        sys.exit(1)
    print("done: %d files" % len(selected))


if __name__ == "__main__":
    main()
