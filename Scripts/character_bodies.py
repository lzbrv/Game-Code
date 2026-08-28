#!/usr/bin/env python3
"""character_bodies.py -- the ten Trace body recipes + the canonical skeleton.

Spec: PIPELINE_DESIGN.md SS3.4 (CANONICAL_SKELETON) and SS5, with the
INTEGRATOR rulings applied (MASTER_PLAN SS1): the generic SS5.1 recipe menu is
SUPERSEDED by CHARACTER_SHEETS.md -- this file implements the sheets' exact
per-character part lists, dims and accent programs.  CHARACTER_LANGUAGE.md
SS2-SS4 carry the shared grammar (team layout, circuit-trim accent rules,
chassis proportions).

This file is the artistic surface of the pipeline (PIPELINE SS5.1): recipes
are tuned against ART_BIBLE SS4.4 -- the pipeline (trace_glb.py) guarantees
everything else.

BINDING LAWS encoded here (do not relax):
  * Recipes may NOT move joints -- all ten bodies share CANONICAL_SKELETON
    byte-identically (PIPELINE SS3.3; per-character variation lives in the
    geometry only).  Height is exactly 176 uu excluding crown_break parts.
  * Rigid one-bone skinning, 3 uu articulation air gaps (Lily 5) bridged by
    inset joint cores (LANGUAGE SS4.3 / SHEETS SS3 rule A2).
  * INTEGRATOR bone remap for sheet text: gear/team-bar sections written
    "skin spine_03" at Z >= 118 skin spine_05; chest-block parts skin
    spine_04; abdomen spine_02; everything else exists verbatim.
  * Slot order (glTF material indices, the integrator numbering carried in
    the CHARACTER_LANGUAGE/SHEETS top blocks): suit(0) inset(1) team_glow(2)
    accent(3) suit_head(4).  Import binds MIs by slot NAME (PIPELINE SS6.4),
    so the names are the contract; the numbering is kept for the record.
  * Team layout is IDENTICAL on all ten (LANGUAGE SS2.2): chest twin bars
    12x44 at centers +/-11 (Z 114-158, top 12 wrapping the chest-top
    chamfer), back spine bar 24x64 built as the SS0.2 two-section split
    (torso Z 112-146 + helmet-back Z 152-176, neck gap dark), pad top strips
    8x28, visor band per sheet.  Deviations only where a sheet canonizes them
    (Rocco: helmet-back section split around the crest root).
  * Accent linears mirror the C++ roster rows EXACTLY
    (TraceCharacterRoster.cpp, line anchors in the table) -- this is the ONE
    python-side table PIPELINE SS8.4 allows, and the census stage checks it
    against the roster.  Widths 8-10 uu, floor absolute; area caps per
    character; every open line node-terminated or edge-terminated (SHEETS
    SS0.4); exactly one service ring per body (LANGUAGE SS3.3).

Coordinate conventions: UE uu, Z up from soles = 0; the canonical rig faces
+Y with LEFT = +X (Manny convention -- so "lateral +/-N" in sheet prose is
X = +/-N here, "fwd/aft" is +/-Y).
"""
import math

from trace_glb import (TriMesh, box, extrude, lathe, limb, prism,
                       v_add, v_sub, v_scale, v_dot, v_cross, v_len, v_norm)

# ---------------------------------------------------------------------------
# canonical skeleton (PIPELINE SS3.4 -- MEASURED Manny x 176/180.54 = 0.97486)
# 26 bones, UE component space, facing +Y, left = +X; right side X-negated.
# import_characters.py (wave 2/3) asserts the imported bone list equals this
# table exactly.  RECIPES MAY NOT MOVE JOINTS.
# ---------------------------------------------------------------------------

_LEFT_CHAIN = [
    ("clavicle_l", "spine_05", (1.4, -1.7, 142.6)),
    ("upperarm_l", "clavicle_l", (18.5, -2.5, 140.0)),
    ("lowerarm_l", "upperarm_l", (34.1, -1.9, 117.9)),
    ("hand_l", "lowerarm_l", (46.6, 15.3, 101.8)),
    ("thigh_l", "pelvis", (9.7, 2.5, 91.2)),
    ("calf_l", "thigh_l", (12.0, 2.5, 49.0)),
    ("foot_l", "calf_l", (13.7, -1.0, 8.0)),
    ("ball_l", "foot_l", (15.4, 13.6, 0.7)),
]

CANONICAL_SKELETON = [
    ("root", None, (0.0, 0.0, 0.0)),
    ("pelvis", "root", (0.0, 2.2, 93.5)),
    ("spine_01", "pelvis", (0.0, 2.0, 97.1)),
    ("spine_02", "spine_01", (0.0, 3.2, 103.6)),
    ("spine_03", "spine_02", (0.0, 4.1, 110.6)),
    ("spine_04", "spine_03", (0.0, 3.6, 118.9)),
    ("spine_05", "spine_04", (0.0, 0.5, 137.6)),
    ("neck_01", "spine_05", (0.0, -1.5, 149.0)),
    ("neck_02", "neck_01", (0.0, -0.3, 153.8)),
    ("head", "neck_02", (0.0, 0.6, 158.5)),
] + _LEFT_CHAIN + [
    (name.replace("_l", "_r"),
     parent if parent in ("spine_05", "pelvis") else parent.replace("_l", "_r"),
     (-pos[0], pos[1], pos[2]))
    for (name, parent, pos) in _LEFT_CHAIN
]

SKEL = {name: pos for (name, _p, pos) in CANONICAL_SKELETON}

# glTF material slot order -- the integrator numbering (see module docstring).
SLOT_ORDER = ("suit", "inset", "team_glow", "accent", "suit_head")

# ---------------------------------------------------------------------------
# per-character constants (CHARACTER_SHEETS SS0.5, copied EXACTLY; accent
# linears re-verified against TraceCharacterRoster.cpp on 2026-08-25).
# accent_cap_pct: <= 8%% of body surface, <= 5%% for Roxie and Mortimer.
#
# *** THE ACCENT HUES ARE NOT THE SHEETS' ORIGINAL TEN (wave 5, W5-BODYVAR).
# *** The sheets' palette was authored without measuring it against the TEAM
# palette, and measured it fails: with team Blue #5B81FF (sRGB hue 226.1) and
# team Amber #FF8000 (hue 30.1), SIX of the ten sat inside 40 deg of a team
# hue -- Mortimer 7.2 deg, Roxie 1.0, Rocco 21.7, Lily 28.1, Mace 29.2,
# Oyster 38.9 -- so on one team each of those six had no colour identity
# channel at all (W4-CENSUS SS7, J-2: Mortimer measured 83.4%% of his lit body
# pixels inside his "own" hue because his own hue IS team Blue).
#
# The hue metric is sRGB, not linear, and that is load-bearing: the accent is
# an emissive material, the frame buffer the player looks at is sRGB, the
# `hex` column below IS the sRGB encoding of the `accent` linear column, and
# the team colours are authored as sRGB bytes.  Comparing linear-space hues
# would answer a question nobody is asking.
#
# Every one of the ten now sits >= 40 deg from BOTH team hues, and the
# tightest accent-vs-accent separation went 10.8 deg -> 18.9 deg.  The two
# arcs that are legal under that rule are only 116 deg + 84 deg wide, so the
# four accents that did NOT collide had to move as well to make room -- the
# arithmetic and the per-character reasoning are in the W5-BODYVAR report.
#
#   name       hex      hue    nearest team   was      reads as
#   Rocco      #E7FF89   72.2  Amber  42.1     51.9    acid gold (hi-vis)
#   Slimeball  #9BF66F  100.4  Amber  70.3     75.1    toxic slime green
#   Chut       #A0F9A4  122.7  Amber  92.6    144.3    signal green
#   Oyster     #6FE5A2  145.9  Blue   80.2    187.2    deep sea green
#   Mortimer   #5DB5A2  167.0  Blue   59.1    218.9    cold patinated steel
#   Lily       #B8F8FF  185.9  Blue   40.2    198.0    pale glacier ice
#   Mace       #DFC4FE  267.9  Blue   41.8    255.3    violet
#   Elle       #FAADFF  296.3  Blue   70.2    286.8    orchid
#   X          #FFAADD  324.0  Amber  66.1    341.6    rose
#   Roxie      #FF617C  349.7  Amber  40.4     31.1    ember (crimson)
#
# THREE saturation/value changes ride along, each with a measured reason:
# Lily 0.118 -> 0.278 saturation (below ~0.18 a hue is not perceptible at all,
# which is why W4-PORTRAITS had to grant her a near-white EXEMPTION from the
# portrait accent-discrimination check and why W4-CENSUS measured 41.9%% of her
# body "in her own hue" -- near-white pixel hue is noise); Mortimer to
# sat 0.486 / val 0.710, a dark saturated steel rather than a pale blue-white,
# so his emissive keeps its hue instead of blowing out to white at
# EmissivePower 8 (W4-CENSUS SS7's second colour finding); Oyster to
# sat 0.515 / val 0.898 so his deep sea green cannot be confused with Chut's
# pale signal green 23 deg away.
# ---------------------------------------------------------------------------

CHARACTERS = {
    # name: (accent linear, hex, roster line, roughness, panel insets,
    #        tri ceiling, service ring, visor width, accent cap %)
    "Rocco": {"accent": (0.80, 1.00, 0.25), "hex": "#E7FF89", "roster_line": 100,
              "roughness": 0.40, "insets": 7, "tri_ceiling": 1360,
              "service_ring": "wrist_r", "visor_w": 16, "accent_cap_pct": 8.0},
    "Chut": {"accent": (0.35, 0.95, 0.37), "hex": "#A0F9A4", "roster_line": 149,
             "roughness": 0.45, "insets": 10, "tri_ceiling": 1440,
             "service_ring": "wrist_l", "visor_w": 18, "accent_cap_pct": 8.0},
    "Mace": {"accent": (0.74, 0.55, 0.99), "hex": "#DFC4FE", "roster_line": 163,
             "roughness": 0.40, "insets": 8, "tri_ceiling": 1600,
             "service_ring": "ankle_r", "visor_w": 16, "accent_cap_pct": 8.0},
    "Oyster": {"accent": (0.16, 0.78, 0.36), "hex": "#6FE5A2", "roster_line": 188,
               "roughness": 0.50, "insets": 11, "tri_ceiling": 1720,
               "service_ring": "ankle_l", "visor_w": 14, "accent_cap_pct": 8.0},
    "X": {"accent": (1.00, 0.40, 0.72), "hex": "#FFAADD", "roster_line": 213,
          "roughness": 0.35, "insets": 9, "tri_ceiling": 1610,
          "service_ring": "wrist_r", "visor_w": 14, "accent_cap_pct": 8.0},
    "Roxie": {"accent": (1.00, 0.12, 0.20), "hex": "#FF617C", "roster_line": 243,
              "roughness": 0.45, "insets": 10, "tri_ceiling": 1580,
              "service_ring": "ankle_r", "visor_w": 16, "accent_cap_pct": 5.0},
    "Elle": {"accent": (0.96, 0.42, 1.00), "hex": "#FAADFF", "roster_line": 257,
             "roughness": 0.30, "insets": 6, "tri_ceiling": 1560,
             "service_ring": "wrist_l", "visor_w": None,  # rim band instead
             "accent_cap_pct": 8.0},
    "Slimeball": {"accent": (0.33, 0.92, 0.16), "hex": "#9BF66F",
                  "roster_line": 271, "roughness": 0.25, "insets": 12,
                  "tri_ceiling": 1700, "service_ring": "ankle_l", "visor_w": 14,
                  "accent_cap_pct": 8.0},
    "Mortimer": {"accent": (0.11, 0.46, 0.36), "hex": "#5DB5A2",
                 "roster_line": 328, "roughness": 0.60, "insets": 13,
                 "tri_ceiling": 1740, "service_ring": "forearm_cuff_r",
                 "visor_w": 16, "accent_cap_pct": 5.0},
    "Lily": {"accent": (0.48, 0.94, 1.00), "hex": "#B8F8FF", "roster_line": 348,
             "roughness": 0.35, "insets": 6, "tri_ceiling": 1480,
             "service_ring": "wrist_l", "visor_w": 14, "accent_cap_pct": 8.0},
}

# Character ids 1..10 in roster order (TraceCharacterRoster.h:13-14).
CHARACTER_ORDER = ("Rocco", "Chut", "Mace", "Oyster", "X",
                   "Roxie", "Elle", "Slimeball", "Mortimer", "Lily")

# Lateral envelope (LANGUAGE SS4.8): +/-40, Mortimer +/-44 -- the single
# canonical exception; the sheets grant no second one.
LATERAL_ENVELOPE = {name: (44.0 if name == "Mortimer" else 40.0)
                    for name in CHARACTER_ORDER}


def body_materials(writer, name):
    """Declare the five slots on a writer, in SLOT_ORDER, with honest default
    values matching the MIs the import stage will stamp (PIPELINE SS4.2-4.3).
    The GLB values are cosmetic-only (materials are stamped, not imported)."""
    row = CHARACTERS[name]
    writer.add_material("suit", (0.10, 0.10, 0.10), roughness=row["roughness"])
    writer.add_material("inset", (0.006, 0.007, 0.010), roughness=0.6)
    writer.add_material("team_glow", (0.005, 0.005, 0.008), roughness=0.3,
                        emissive=(0.18, 0.78, 1.0), emissive_strength=1.7)
    acc = row["accent"]
    writer.add_material("accent", tuple(c * 0.02 for c in acc), roughness=0.3,
                        emissive=acc, emissive_strength=1.7)
    writer.add_material("suit_head", (0.14, 0.14, 0.14), roughness=0.45)


# ---------------------------------------------------------------------------
# geometry helpers built on the writer primitives
# ---------------------------------------------------------------------------


def _perp(d, hint):
    """Component of `hint` perpendicular to unit vector d, normalized."""
    return v_norm(v_sub(hint, v_scale(d, v_dot(hint, d))))


def _solid_from_faces(faces):
    """Convex solid from a face list (each face = list of coplanar corners in
    any order); normals oriented away from the solid centroid."""
    from trace_glb import _add_face
    all_pts = [p for f in faces for p in f]
    n = float(len(all_pts))
    centroid = (sum(p[0] for p in all_pts) / n,
                sum(p[1] for p in all_pts) / n,
                sum(p[2] for p in all_pts) / n)
    m = TriMesh()
    for f in faces:
        fc = (sum(p[0] for p in f) / len(f), sum(p[1] for p in f) / len(f),
              sum(p[2] for p in f) / len(f))
        nrm = v_cross(v_sub(f[1], f[0]), v_sub(f[2], f[0]))
        if v_len(nrm) < 1e-9:
            nrm = v_cross(v_sub(f[2], f[1]), v_sub(f[3], f[1]))
        nrm = v_norm(nrm)
        if v_dot(nrm, v_sub(fc, centroid)) < 0:
            nrm = v_scale(nrm, -1.0)
        _add_face(m, f, nrm)
    return m


def frame_box(center, length_dir, proud_dir, length, width, thick):
    """Oriented box: `length` along length_dir, `thick` along proud_dir,
    `width` along their cross."""
    d = v_norm(length_dir)
    t = _perp(d, proud_dir)
    w = v_norm(v_cross(d, t))
    corners = []
    for sl in (-0.5, 0.5):
        for sw in (-0.5, 0.5):
            for st in (-0.5, 0.5):
                corners.append(v_add(center,
                                     v_add(v_scale(d, sl * length),
                                           v_add(v_scale(w, sw * width),
                                                 v_scale(t, st * thick)))))
    c = corners  # index bits: (sl<<2)|(sw<<1)|st
    faces = [
        [c[0], c[1], c[3], c[2]],  # -length
        [c[4], c[5], c[7], c[6]],  # +length
        [c[0], c[1], c[5], c[4]],  # -width
        [c[2], c[3], c[7], c[6]],  # +width
        [c[0], c[2], c[6], c[4]],  # -thick
        [c[1], c[3], c[7], c[5]],  # +thick
    ]
    return _solid_from_faces(faces)


def edge_strip(p0, p1, width, proud_dir, thick=2.5, embed=1.0):
    """Raised ribbon along the segment p0->p1: outer face `thick - embed`
    proud of the line, `embed` sunk into the host (kills coplanar z-fighting
    and floating panels).  The circuit-trim physical form: flat ribbons raised
    1.5 uu off the host face (LANGUAGE SS3.4).  Ribbons riding a silhouette
    EDGE (crest/fin edges) pass thick=1.5, embed=0.0 -- fully proud, and the
    thinner slab keeps the measured accent area inside the T4 caps (the
    sheet areas are face-area estimates; TriMesh.area() counts every face)."""
    d = v_sub(p1, p0)
    t = _perp(v_norm(d), proud_dir)
    center = v_add(v_scale(v_add(p0, p1), 0.5), v_scale(t, thick / 2.0 - embed))
    return frame_box(center, d, t, v_len(d), width, thick)


def node_pad(center, out_dir, size=8.0, thick=2.5, embed=1.0):
    """8x8 chamfer-free node pad, raised 1.5 uu (LANGUAGE SS3.2: every open
    accent line terminates in one; pads are 8x8)."""
    out = v_norm(out_dir)
    ref = (0.0, 0.0, 1.0) if abs(v_dot(out, (0.0, 0.0, 1.0))) < 0.98 else (0.0, 1.0, 0.0)
    ldir = _perp(out, ref)
    return frame_box(v_add(center, v_scale(out, thick / 2.0 - embed)),
                     ldir, out, size, size, thick)


def ring_band(center, axis_dir, radius, band_w=8.0, sides=8):
    """8-gon service/accent ring: a band `band_w` wide along the limb axis,
    standing proud of the limb surface (LANGUAGE SS3.1: rings are 8-gon)."""
    d = v_norm(axis_dir)
    return limb(v_sub(center, v_scale(d, band_w / 2.0)),
                v_add(center, v_scale(d, band_w / 2.0)), sides, radius, radius)


def blade(root_center, tip_center, root_chord, tip_chord, thick,
          chord_hint=(0.0, 1.0, 0.0)):
    """Tapered swept blade (two rectangular cross-sections lofted root->tip).
    Returns (mesh, leading_edge_root, leading_edge_tip) where the leading
    edge is the +chord_hint side."""
    a = v_norm(v_sub(tip_center, root_center))
    cd = _perp(a, chord_hint)
    td = v_norm(v_cross(a, cd))

    def rect(center, chord):
        return [v_add(center, v_add(v_scale(cd, sc * chord / 2.0),
                                    v_scale(td, st * thick / 2.0)))
                for (sc, st) in ((-1, -1), (1, -1), (1, 1), (-1, 1))]

    r = rect(root_center, root_chord)
    t = rect(tip_center, tip_chord)
    faces = [r, t,
             [r[0], r[1], t[1], t[0]],
             [r[1], r[2], t[2], t[1]],
             [r[2], r[3], t[3], t[2]],
             [r[3], r[0], t[0], t[3]]]
    le_root = v_add(root_center, v_scale(cd, root_chord / 2.0))
    le_tip = v_add(tip_center, v_scale(cd, tip_chord / 2.0))
    return _solid_from_faces(faces), le_root, le_tip


def sphere(r, sides=8, rows=3):
    """8-side lathe sphere -- the joint core (LANGUAGE SS4.3)."""
    profile = [(0.0, -r)]
    for k in range(1, rows):
        th = math.pi * k / rows
        profile.append((r * math.sin(th), -r * math.cos(th)))
    profile.append((0.0, r))
    return lathe(profile, sides, smooth=True)


def yz_extrude(profile_yz, width, x_center=0.0):
    """Convex profile given in (Y, Z), extruded across X (`width`), centered
    on x_center.  Cyclic axis permutation keeps winding intact."""
    m = extrude(profile_yz, width)
    m = m._mapped(lambda p: (p[2], p[0], p[1]), lambda n: (n[2], n[0], n[1]))
    return m.translate(x_center - width / 2.0, 0.0, 0.0)


def lerp(a, b, t):
    return v_add(a, v_scale(v_sub(b, a), t))


def limb_line(bone_a, bone_b, t0, t1, out_hint, r_off, width=4.0,
              thick=2.0, embed=1.7):
    """Engraving/accent line on a limb surface: from param t0 to t1 along the
    bone_a->bone_b axis, offset radially by r_off toward out_hint."""
    a, b = SKEL[bone_a], SKEL[bone_b]
    d = v_norm(v_sub(b, a))
    out = _perp(d, out_hint)
    p0 = v_add(lerp(a, b, t0), v_scale(out, r_off))
    p1 = v_add(lerp(a, b, t1), v_scale(out, r_off))
    return edge_strip(p0, p1, width, out, thick=thick, embed=embed)


# --- wave-2 additions (the eight remaining sheets need five more idioms;
# every one of them is built out of the SS2 writer primitives -- trace_glb.py
# is NOT touched by this tranche) --------------------------------------------


def oriented_box(center, length_dir, proud_dir, length, width, thick,
                 chamfer=0.0):
    """frame_box with ART_BIBLE SS4.2 chamfers: `length` along length_dir,
    `thick` along proud_dir, `width` along their cross.  Used for signature
    boxes that must ride an arbitrary axis (Chut's gauntlets, X's hive,
    Roxie's knee guards)."""
    from trace_glb import _rot_axis_angle
    m = box(width, length, thick, chamfer=chamfer)   # local X=w, Y=len, Z=thk
    d = v_norm(length_dir)
    zimg = (0.0, 0.0, 1.0)
    ax = v_cross((0.0, 1.0, 0.0), d)
    if v_len(ax) > 1e-9:
        ang = math.degrees(math.acos(max(-1.0, min(1.0,
                                                   v_dot((0.0, 1.0, 0.0), d)))))
        ax = v_norm(ax)
        m = m.rotate(ax, ang)
        zimg = _rot_axis_angle(zimg, ax, ang)
    elif v_dot((0.0, 1.0, 0.0), d) < 0.0:
        m = m.rotate((1.0, 0.0, 0.0), 180.0)
        zimg = (0.0, 0.0, -1.0)
    t = _perp(d, proud_dir)
    roll = math.degrees(math.acos(max(-1.0, min(1.0, v_dot(zimg, t)))))
    if v_dot(v_cross(zimg, t), d) < 0.0:
        roll = -roll
    if abs(roll) > 1e-9:
        m = m.rotate(d, roll)
    return m.translate(*center)


def torus_arc(center, axis, R, r, arc_deg, start_deg=0.0, seg=6, sides=6):
    """Swept n-gon tube along a circular arc of radius R about `axis` -- the
    rope/coil/portal idiom (SHEETS SS6 Mace coils, SS9 Roxie heel springs,
    SS10 Elle's gate arc).  Open arcs are capped at both ends."""
    a = v_norm(axis)
    u = _perp(a, (0.0, 0.0, 1.0) if abs(v_dot(a, (0.0, 0.0, 1.0))) < 0.9
              else (1.0, 0.0, 0.0))
    w = v_norm(v_cross(a, u))
    from trace_glb import _add_face
    centers, rings = [], []
    for k in range(seg + 1):
        th = math.radians(start_deg + arc_deg * k / float(seg))
        c = v_add(center, v_add(v_scale(u, R * math.cos(th)),
                                v_scale(w, R * math.sin(th))))
        radial = v_norm(v_sub(c, center))
        ring = []
        for i in range(sides):
            ph = 2.0 * math.pi * i / sides
            ring.append(v_add(c, v_add(v_scale(radial, r * math.cos(ph)),
                                       v_scale(a, r * math.sin(ph)))))
        centers.append(c)
        rings.append(ring)
    m = TriMesh()
    for k in range(seg):
        mid = v_scale(v_add(centers[k], centers[k + 1]), 0.5)
        for i in range(sides):
            j = (i + 1) % sides
            quad = [rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]]
            fc = v_scale(v_add(v_add(quad[0], quad[1]),
                               v_add(quad[2], quad[3])), 0.25)
            _add_face(m, quad, v_norm(v_sub(fc, mid)))
    if abs(arc_deg) < 359.9:
        _add_face(m, rings[0], v_norm(v_sub(centers[0], centers[1])))
        _add_face(m, rings[-1], v_norm(v_sub(centers[-1], centers[-2])))
    return m


def arc_shell(center, axis, r_in, r_out, lo, hi, sides, i0=0, count=None,
              phase=0.0):
    """Closed shell of `count` n-gon segments of an annular band about `axis`:
    radial extent r_in..r_out, axial extent lo..hi.  Two idioms in one --
    a collar (band wide along the axis, thin radially: Oyster's jar rims) and
    a face ring (thin along the axis, 8 uu wide radially: X's monocle, the
    LINE(rim) ops).  Partial spans are how the SS0.4 G2 clause is BUILT: the
    segments that come within 12 uu of a team panel are emitted as a second
    shell bound to slot 1."""
    from trace_glb import _add_face
    a = v_norm(axis)
    u = _perp(a, (0.0, 0.0, 1.0) if abs(v_dot(a, (0.0, 0.0, 1.0))) < 0.9
              else (1.0, 0.0, 0.0))
    w = v_norm(v_cross(a, u))
    n = sides if count is None else count

    def pt(rad, k, h):
        th = phase + 2.0 * math.pi * k / sides
        return v_add(center, v_add(v_scale(u, rad * math.cos(th)),
                                   v_add(v_scale(w, rad * math.sin(th)),
                                         v_scale(a, h))))

    def radial_dir(k):
        th = phase + 2.0 * math.pi * (k + 0.5) / sides
        return v_add(v_scale(u, math.cos(th)), v_scale(w, math.sin(th)))

    m = TriMesh()
    for k in range(i0, i0 + n):
        rd = radial_dir(k)
        _add_face(m, [pt(r_out, k, lo), pt(r_out, k + 1, lo),
                      pt(r_out, k + 1, hi), pt(r_out, k, hi)], rd)
        _add_face(m, [pt(r_in, k, lo), pt(r_in, k + 1, lo),
                      pt(r_in, k + 1, hi), pt(r_in, k, hi)], v_scale(rd, -1.0))
        _add_face(m, [pt(r_in, k, hi), pt(r_in, k + 1, hi),
                      pt(r_out, k + 1, hi), pt(r_out, k, hi)], a)
        _add_face(m, [pt(r_in, k, lo), pt(r_in, k + 1, lo),
                      pt(r_out, k + 1, lo), pt(r_out, k, lo)],
                  v_scale(a, -1.0))
    if n < sides:
        for (k, sgn) in ((i0, -1.0), (i0 + n, 1.0)):
            th = phase + 2.0 * math.pi * k / sides
            tang = v_add(v_scale(u, -math.sin(th)), v_scale(w, math.cos(th)))
            _add_face(m, [pt(r_in, k, lo), pt(r_out, k, lo),
                          pt(r_out, k, hi), pt(r_in, k, hi)],
                      v_scale(v_norm(tang), sgn))
    return m


def stud_grid(center, out_dir, cols, rows, stud=3.0, pitch=5.5, depth=2.0,
              up_hint=(0.0, 0.0, 1.0)):
    """cols x rows raised studs on a flat host face -- Slimeball's wall-stick
    grip pads (SHEETS SS11).  Merged into ONE part (rigid skinning is
    per-part, and a 6-stud pad is one physical object).  Studs are square
    pyramids -- 6 tris each instead of a box's 12, which is what keeps the
    full 2 x 3 grid inside Slimeball's 1,700 tri ceiling, and a nub reads
    better than a cube at grip scale anyway."""
    out = v_norm(out_dir)
    up = _perp(out, up_hint)
    side = v_norm(v_cross(up, out))
    m = None
    for cx in range(cols):
        for cy in range(rows):
            off = v_add(v_scale(side, (cx - (cols - 1) / 2.0) * pitch),
                        v_scale(up, (cy - (rows - 1) / 2.0) * pitch))
            base = v_add(v_add(center, off), v_scale(out, -0.6))
            h = stud / 2.0
            quad = [v_add(base, v_add(v_scale(side, su * h),
                                      v_scale(up, sv * h)))
                    for (su, sv) in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            apex = v_add(base, v_scale(out, depth))
            b = _solid_from_faces([quad, [quad[0], quad[1], apex],
                                   [quad[1], quad[2], apex],
                                   [quad[2], quad[3], apex],
                                   [quad[3], quad[0], apex]])
            m = b if m is None else m.merged(b)
    return m


def chevron(apex, point_dir, out_dir, size=12.0, width=8.0, thick=1.5,
            embed=0.0):
    """A service hash: two legs meeting at `apex` and opening away from
    point_dir (SHEETS SS5 -- Chut's gauntlet maintenance markings).  Closed
    form: no node pad needed (SS0.4)."""
    d = v_norm(point_dir)
    out = _perp(d, out_dir)
    side = v_norm(v_cross(d, out))
    m = None
    for sgn in (1.0, -1.0):
        tail = v_add(apex, v_add(v_scale(d, -size * 0.72),
                                 v_scale(side, sgn * size * 0.5)))
        leg = edge_strip(apex, tail, width, out, thick=thick, embed=embed)
        m = leg if m is None else m.merged(leg)
    return m


def face_shape(center, out_dir, poly2d, thick, embed=1.0,
               up_hint=(0.0, 0.0, 1.0)):
    """Convex 2-D polygon extruded `thick` along a host face's outward normal,
    sunk `embed` into it.  The frame (side, up, out) is right-handed, so the
    writer's winding law survives the change of basis untouched.  Slimeball's
    5-gon splats and X's quarter-cone dish are the users."""
    out = v_norm(out_dir)
    up = _perp(out, up_hint)
    side = v_norm(v_cross(up, out))
    m = extrude(poly2d, thick)

    def fp(p):
        return v_add(center, v_add(v_scale(side, p[0]),
                                   v_add(v_scale(up, p[1]),
                                         v_scale(out, p[2] - embed))))

    def fn(n):
        return v_add(v_scale(side, n[0]),
                     v_add(v_scale(up, n[1]), v_scale(out, n[2])))

    return m._mapped(fp, fn)


def wedge_solid(base_center, point_dir, out_dir, length, width, thick):
    """Tapered wedge: rectangular root, chisel tip.  Claws (Slimeball) and
    knee guards (Roxie); 8 tris, the cheapest honest taper in the kit."""
    d = v_norm(point_dir)
    out = _perp(d, out_dir)
    side = v_norm(v_cross(d, out))
    hw, ht = width / 2.0, thick / 2.0

    def c(su, st):
        return v_add(base_center, v_add(v_scale(side, su * hw),
                                        v_scale(out, st * ht)))

    r00, r01 = c(-1, -1), c(-1, 1)
    r11, r10 = c(1, 1), c(1, -1)
    tip = v_add(base_center, v_scale(d, length))
    t0 = v_add(tip, v_add(v_scale(side, -hw * 0.2), v_scale(out, -ht * 0.1)))
    t1 = v_add(tip, v_add(v_scale(side, hw * 0.2), v_scale(out, -ht * 0.1)))
    return _solid_from_faces([[r00, r01, r11, r10],
                              [r01, r11, t1, t0],
                              [r00, r10, t1, t0],
                              [r00, r01, t0],
                              [r10, r11, t1]])


def ankle_ring_part(name, side, radius, slot="accent", t=0.80):
    """The ankle flavour of the one service ring (LANGUAGE SS3.3): same
    8-gon band, seated on the calf just above the boot cuff."""
    ca, fo = SKEL["calf_" + side], SKEL["foot_" + side]
    d = v_norm(v_sub(fo, ca))
    return part(name, "calf_" + side, slot,
                ring_band(lerp(ca, fo, t), d, radius, band_w=8.0),
                thickness=8.0)


def drop_parts(parts, names):
    """Chassis parts a sheet replaces outright (Chut's forearms -> gauntlets,
    Mortimer's boot wedges -> anvils, Oyster's pads -> caps)."""
    names = set(names)
    kept = [p for p in parts if p["name"] not in names]
    missing = names - set(p["name"] for p in parts)
    if missing:
        raise KeyError("drop_parts: no such chassis part(s): %s"
                       % ", ".join(sorted(missing)))
    return kept


# ---------------------------------------------------------------------------
# chassis (LANGUAGE SS4.1 proportion grid + SS4.4 part inventory; the sheets'
# baseline.  All numbers uu; overrides per character via ChassisSpec kwargs.)
# ---------------------------------------------------------------------------


class ChassisSpec(object):
    DEFAULTS = dict(
        pelvis_w=30.0, pelvis_d=22.0, pelvis_z0=86.0, pelvis_z1=108.0,
        chest_w_waist=26.0, chest_w_top=36.0, chest_d=24.0,
        chest_z0=112.0, chest_z1=146.0, chest_y_bias=0.0,
        neck=True,
        pad_w=16.0, pad_d=20.0, pad_h=10.0, pad_outer=40.0, pad_top=150.0,
        pad_chamfer=3.0,
        head_w=20.0, head_d=24.0, head_z0=152.0, head_z1=176.0,
        upperarm_r0=5.5, upperarm_r1=4.5,
        forearm_r0=5.0, forearm_r1=4.2, cuff_r=6.0, cuff_len=8.0,
        mitt=(12.0, 14.0, 8.0),
        thigh_r0=7.0, thigh_r1=6.0, calf_r0=6.0, calf_r1=4.5,
        boot_w=16.0, boot_heel=-12.0, boot_toe=22.0, boot_h=12.0,
        core_r_major=8.0, core_r_minor=6.0, core_rows=3, joint_gap=3.0,
    )

    def __init__(self, **overrides):
        for k, v in self.DEFAULTS.items():
            setattr(self, k, v)
        for k, v in overrides.items():
            if k not in self.DEFAULTS:
                raise KeyError("unknown chassis field %r" % k)
            setattr(self, k, v)


def part(name, bone, slot, mesh, crown_break=False, thickness=None,
         gear_region=None):
    """One rigid part record.  `thickness` is the declared minimum thickness
    for parts outside the +/-36 lateral band / above Z 176 (T5 checks it
    against the LANGUAGE SS4.8 <=12 / crown-break <=8 rules -- an axis-
    aligned bbox cannot measure the true thickness of tilted blades, so the
    recipe declares it and T5 trusts the declaration)."""
    return {"name": name, "bone": bone, "slot": slot, "mesh": mesh,
            "crown_break": bool(crown_break), "thickness": thickness,
            "gear_region": gear_region}


def _octagon(w, d, facet_half=12.0, cy=5.0):
    a, b = w / 2.0, d / 2.0
    fh = min(facet_half, a - 1.0)
    return [(fh, b), (-fh, b), (-a, b - cy), (-a, -(b - cy)),
            (-fh, -b), (fh, -b), (a, -(b - cy)), (a, b - cy)]


def chest_loft(ch):
    """Extruded-octagon chest, 3 loops, tapered W waist->top (LANGUAGE
    SS4.1/4.4).  Front/back facets pinned near +/-12 lateral so the 24-wide
    back bar and the +/-11-center chest bars mount flush."""
    z0, z1 = ch.chest_z0, ch.chest_z1
    zm = (z0 + z1) / 2.0
    rings = []
    for (z, w) in ((z0, ch.chest_w_waist), (zm, (ch.chest_w_waist + ch.chest_w_top) / 2.0),
                   (z1, ch.chest_w_top)):
        rings.append([(x, y + ch.chest_y_bias, z)
                      for (x, y) in _octagon(w, ch.chest_d)])
    faces = [list(reversed(rings[0])), rings[-1]]
    for k in range(len(rings) - 1):
        lo, hi = rings[k], rings[k + 1]
        n = len(lo)
        for i in range(n):
            faces.append([lo[i], lo[(i + 1) % n], hi[(i + 1) % n], hi[i]])
    return _solid_from_faces(faces)


def head_capsule(ch):
    """Standard head core: 8-side lathe capsule W x D (LANGUAGE SS4.1)."""
    r = ch.head_w / 2.0
    profile = [(0.0, 0.0), (r * 0.6, 2.0), (r * 0.9, 6.0), (r, 12.0),
               (r * 0.9, 19.0), (r * 0.6, 22.5), (0.0, 24.0)]
    m = lathe(profile, 8, smooth=True)
    return m.scale(1.0, ch.head_d / ch.head_w, 1.0).translate(0, 0, ch.head_z0)


def head_teardrop(ch):
    """Lily: 8-side lathe teardrop O/-18, small and smooth (SHEETS SS13)."""
    r = ch.head_w / 2.0
    profile = [(0.0, 0.0), (r * 0.78, 3.0), (r, 8.0), (r * 0.94, 14.0),
               (r * 0.67, 19.0), (r * 0.39, 22.5), (0.0, 24.0)]
    m = lathe(profile, 8, smooth=True)
    return m.scale(1.0, 1.15, 1.0).translate(0, 0, ch.head_z0)


def _limb_chain(ch, side):
    """Arms + legs for one side ('l'/'r'): rigid segments with joint-gap air
    gaps, inset joint cores bridging them (SS4.3), cuff flare, mitt, boot."""
    s = side
    g = ch.joint_gap
    parts = []
    ua, la, ha = SKEL["upperarm_" + s], SKEL["lowerarm_" + s], SKEL["hand_" + s]
    th, ca, fo = SKEL["thigh_" + s], SKEL["calf_" + s], SKEL["foot_" + s]

    # shoulder + elbow cores
    parts.append(part("core_shoulder_" + s, "upperarm_" + s, "inset",
                      sphere(ch.core_r_major, rows=ch.core_rows)
                      .translate(*ua)))
    parts.append(part("core_elbow_" + s, "lowerarm_" + s, "inset",
                      sphere(ch.core_r_minor, rows=ch.core_rows)
                      .translate(*la)))
    d_ua = v_norm(v_sub(la, ua))
    parts.append(part("upperarm_" + s, "upperarm_" + s, "suit",
                      limb(v_add(ua, v_scale(d_ua, g)),
                           v_sub(la, v_scale(d_ua, g)), 6,
                           ch.upperarm_r0, ch.upperarm_r1)))
    d_fa = v_norm(v_sub(ha, la))
    fa_end = v_sub(ha, v_scale(d_fa, g + ch.cuff_len))
    parts.append(part("forearm_" + s, "lowerarm_" + s, "suit",
                      limb(v_add(la, v_scale(d_fa, g)), fa_end, 6,
                           ch.forearm_r0, ch.forearm_r1)))
    # cuff flare at the wrist (O/ 2*cuff_r -- LANGUAGE SS4.1 forearm cuff)
    parts.append(part("cuff_" + s, "lowerarm_" + s, "suit",
                      limb(fa_end, v_sub(ha, v_scale(d_fa, 2.0)), 6,
                           ch.cuff_r, ch.cuff_r)))
    # mitt hand, long axis down the forearm direction
    mx, my, mz = ch.mitt
    mitt = box(mx, my, mz, chamfer=2.0)
    axis = v_cross((0.0, 1.0, 0.0), d_fa)
    ang = math.degrees(math.acos(max(-1.0, min(1.0, v_dot((0.0, 1.0, 0.0), d_fa)))))
    if v_len(axis) > 1e-6 and abs(ang) > 1e-3:
        mitt = mitt.rotate(axis, ang)
    mitt = mitt.translate(*v_add(ha, v_scale(d_fa, my / 2.0 - 2.0)))
    parts.append(part("mitt_" + s, "hand_" + s, "suit", mitt, thickness=mz))

    # legs
    parts.append(part("core_hip_" + s, "thigh_" + s, "inset",
                      sphere(ch.core_r_major, rows=ch.core_rows)
                      .translate(*th)))
    parts.append(part("core_knee_" + s, "calf_" + s, "inset",
                      sphere(ch.core_r_minor, rows=ch.core_rows)
                      .translate(*ca)))
    parts.append(part("core_ankle_" + s, "foot_" + s, "inset",
                      sphere(ch.core_r_minor, rows=ch.core_rows)
                      .translate(*fo)))
    d_th = v_norm(v_sub(ca, th))
    parts.append(part("thigh_" + s, "thigh_" + s, "suit",
                      limb(v_add(th, v_scale(d_th, g)),
                           v_sub(ca, v_scale(d_th, g)), 6,
                           ch.thigh_r0, ch.thigh_r1)))
    d_ca = v_norm(v_sub(fo, ca))
    parts.append(part("calf_" + s, "calf_" + s, "suit",
                      limb(v_add(ca, v_scale(d_ca, g)),
                           v_sub(fo, v_scale(d_ca, g)), 6,
                           ch.calf_r0, ch.calf_r1)))
    # boot: wedge foot, toe forward +Y, sole exactly Z 0, 4 uu toe chamfer
    profile = [(ch.boot_heel, 0.0), (ch.boot_toe, 0.0), (ch.boot_toe, 4.0),
               (ch.boot_toe - 8.0, ch.boot_h), (ch.boot_heel, ch.boot_h)]
    parts.append(part("boot_" + s, "foot_" + s, "suit",
                      yz_extrude(profile, ch.boot_w, x_center=fo[0])))
    return parts


def build_chassis(ch, head_builder=head_capsule):
    """The shared machine chassis (baseline ~= the LANGUAGE SS4.4 inventory).
    Per-character helmets replace `head_builder`; signature parts stack on
    top in the character functions."""
    parts = []
    # pelvis block
    parts.append(part("pelvis", "pelvis", "suit",
                      box(ch.pelvis_w, ch.pelvis_d,
                          ch.pelvis_z1 - ch.pelvis_z0, chamfer=4.0)
                      .translate(0, 0, (ch.pelvis_z0 + ch.pelvis_z1) / 2.0)))
    # waist gap: 4 uu dark joint band Z 108-112 (abdomen -> spine_02 per the
    # integrator remap), oversized in Z to tuck under both blocks
    parts.append(part("waist_band", "spine_02", "inset",
                      box(min(ch.pelvis_w, ch.chest_w_waist) - 4.0,
                          min(ch.pelvis_d, ch.chest_d) - 4.0, 7.0)
                      .translate(0, 0, 109.5)))
    # chest block (spine_04 per the integrator remap)
    parts.append(part("chest", "spine_04", "suit", chest_loft(ch)))
    # neck: dark hex, the SS0.2 "neck gap dark" band (Mortimer omits it --
    # "zero neck", SHEETS SS12 chassis deviations)
    if ch.neck:
        parts.append(part("neck", "neck_01", "inset",
                          prism(6, 5.0, 8.0).translate(0, 0, 149.0)))
    # head core / helmet
    parts.append(part("head", "head", "suit_head", head_builder(ch)))
    # shoulder pads (ride the shrug on clavicle_l/r -- SHEETS SS0.1)
    for s, sign in (("l", 1.0), ("r", -1.0)):
        px = sign * (ch.pad_outer - ch.pad_w / 2.0)
        parts.append(part("pad_" + s, "clavicle_" + s, "suit",
                          box(ch.pad_w, ch.pad_d, ch.pad_h,
                              chamfer=ch.pad_chamfer)
                          .translate(px, 0, ch.pad_top - ch.pad_h / 2.0),
                          thickness=ch.pad_h))
    parts.extend(_limb_chain(ch, "l"))
    parts.extend(_limb_chain(ch, "r"))
    return parts


# ---------------------------------------------------------------------------
# team layout (LANGUAGE SS2.2 / SHEETS SS0.2 -- identical on all ten)
# ---------------------------------------------------------------------------


def team_layout(ch, visor_w, helm_back_parts=None, torso_back_parts=None,
                chest_bar_dy=0.0, back_bar_w=24.0, visor_center=None,
                visor_depth=6.0, pad_strip_len=28.0):
    """The one team kit players learn ONE location for.  All panels are deep
    boxes whose outer face stands 1.5 uu proud of the host shell and whose
    body embeds into it (no floating panels, no coplanar z-fighting).

    Every keyword below defaults to the SS0.2 baseline, so a body that passes
    none of them gets byte-identical geometry to Rocco's/Lily's.  The
    overrides exist for the deviations the sheets canonize, and ONLY those:
      helm_back_parts   helmet-back bar section (Rocco split, Mace on-hood,
                        Oyster on-bell, Chut/Slimeball crown-clipped)
      torso_back_parts  torso bar section (X narrowed over the hive,
                        Slimeball facet-conformed over the hump)
      chest_bar_dy      chest twin bars pushed out to a proud host face
                        (Mortimer's middle slab, Slimeball's +Y chest)
      back_bar_w        24 baseline, 28 for Mortimer (canonical)
      visor_center      band center when the helmet is not the standard
                        capsule (bell/dome/block/flat-top/offset)
    """
    parts = []
    # chest twin bars 12 x 44, centers +/-11, Z 114-158; the top 12 uu wraps
    # the chest-top chamfer as a tilted segment (SS0.2)
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("team_chestbar_" + s, "spine_04", "team_glow",
                          box(12.0, 7.5, 33.0)
                          .translate(sign * 11.0, 9.75 + chest_bar_dy, 130.5)))
        wrap = box(12.0, 5.0, 12.0).rotate((1.0, 0.0, 0.0), -28.0)
        parts.append(part("team_chestbar_wrap_" + s, "spine_04", "team_glow",
                          wrap.translate(sign * 11.0, 9.0 + chest_bar_dy,
                                         152.0)))
    # back spine bar, torso section Z 112-146 (skins with the torso block)
    if torso_back_parts is None:
        parts.append(part("team_backbar_torso", "spine_04", "team_glow",
                          box(back_bar_w, 7.5, 34.0).translate(0, -9.75, 129.0)))
    else:
        parts.extend(torso_back_parts)
    # helmet-back section Z 152-176 (skins head), neck gap 146-152 dark
    if helm_back_parts is None:
        parts.append(part("team_backbar_helm", "head", "team_glow",
                          box(back_bar_w, 8.5, 24.0).translate(0, -9.25, 164.0)))
    else:
        parts.extend(helm_back_parts)
    # shoulder-pad top strips 8 x 28, fore-aft on pad tops
    for s, sign in (("l", 1.0), ("r", -1.0)):
        px = sign * (ch.pad_outer - ch.pad_w / 2.0)
        parts.append(part("team_padstrip_" + s, "clavicle_" + s, "team_glow",
                          box(8.0, pad_strip_len, 3.0)
                          .translate(px, 0, ch.pad_top + 0.5)))
    # visor band, 8 uu tall, front face at the eye line (center Z 162)
    if visor_w:
        vc = visor_center or (0.0, 10.5, 162.0)
        parts.append(part("team_visor", "head", "team_glow",
                          box(float(visor_w), visor_depth, 8.0)
                          .translate(*vc)))
    return parts


def service_ring_part(name, wrist_side, cuff_r):
    """The shared roster signature: exactly one lit wrist/ankle ring per body
    (LANGUAGE SS3.3).  This helper does wrist rings; ankle rings for later
    characters follow the same ring_band recipe on the calf."""
    la, ha = SKEL["lowerarm_" + wrist_side], SKEL["hand_" + wrist_side]
    d = v_norm(v_sub(ha, la))
    center = lerp(la, ha, 0.8)
    return part(name, "lowerarm_" + wrist_side, "accent",
                ring_band(center, d, cuff_r + 1.5, band_w=8.0),
                thickness=8.0)


# ---------------------------------------------------------------------------
# ROCCO -- the sprinter, acid gold #E7FF89 (CHARACTER_SHEETS SS4; the template
# body, built FIRST; replaces the committed FBX at .../Rocco/SK_Rocco)
# ---------------------------------------------------------------------------


def build_rocco():
    ch = ChassisSpec()  # baseline SS4.1 verbatim, no deviations
    parts = build_chassis(ch)

    # Team deviation (canonical): the helmet-back bar section splits into two
    # 9 x 24 strips flanking the crest root (lateral +/-3.5..+/-12.5,
    # Z 152-176); torso section standard.  T3's 8,000 uu back-read frame is
    # the acceptance gate for the split (wave 4).
    helm_back = [
        part("team_backbar_helm_" + s, "head", "team_glow",
             box(9.0, 8.5, 24.0).translate(sign * 8.0, -9.25, 164.0))
        for (s, sign) in (("l", 1.0), ("r", -1.0))
    ]
    parts.extend(team_layout(ch, CHARACTERS["Rocco"]["visor_w"],
                             helm_back_parts=helm_back))

    # --- signature: crest blade (extrude, pentagon profile, 6 thick; root at
    # brow Z 170, peak Z 186 (crown_break), sweeping aft-down to a collar
    # mount at Z 150, endpoint 18 aft of head center = 6 uu proud of the back
    # shell).  Sagittal blade: profile in (Y, Z), thickness in X.
    crest_profile = [(10.0, 170.0), (2.0, 186.0), (-16.0, 176.0),
                     (-18.0, 150.0), (-13.0, 150.0)]
    crest = extrude(crest_profile, 6.0)
    crest = crest._mapped(lambda p: (p[2], p[0], p[1]),
                          lambda n: (n[2], n[0], n[1])).translate(-3.0, 0, 0)
    parts.append(part("sig_crest", "head", "suit_head", crest,
                      crown_break=True, thickness=6.0))

    # --- signature: calf thruster fins x2 (wedge 22 L x 10 H x 4 T, raked
    # ~30deg, calf rear faces, root Z 18-40)
    fin_profile = [(-5.0, 42.0), (-15.0, 24.0), (-5.0, 18.0)]
    fin_l = yz_extrude(fin_profile, 4.0, x_center=12.8)
    parts.append(part("sig_fin_l", "calf_l", "suit", fin_l, thickness=4.0))
    parts.append(part("sig_fin_r", "calf_r", "suit", fin_l.mirrored_x(),
                      thickness=4.0))

    # --- accent program (widths 8; est ~4.6% -- SS4 table)
    ops = []
    # LINE(crest trailing edge): peak down the aft silhouette.  G2 ruling
    # applied (SS0.4): the trailing-edge segment descending beside the split
    # helmet-back strips binds slot 1 (inset) -- geodesic distance to the
    # team strips falls under 12 uu on the lower descent; the over-crown
    # segment and the upper descent stay accent; the NODE rides the blade's
    # proud aft tip at the collar mount (accent -- adjudicated by the sheet,
    # the dark blade is the intervening surface).
    segA = edge_strip((0.0, 2.0, 186.0), (0.0, -16.0, 176.0), 8.0,
                      (0.0, -0.486, 0.874), thick=1.5, embed=0.0)
    parts.append(part("acc_crest_edge_hi", "head", "accent", segA,
                      crown_break=True, thickness=3.0))
    segB = edge_strip((0.0, -16.0, 176.0), (0.0, -16.8, 165.6), 8.0,
                      (0.0, -0.997, 0.0767), thick=1.5, embed=0.0)
    parts.append(part("acc_crest_edge_mid", "head", "accent", segB,
                      thickness=3.0))
    segC = edge_strip((0.0, -16.8, 165.6), (0.0, -18.0, 150.0), 8.0,
                      (0.0, -0.997, 0.0767))
    parts.append(part("ins_crest_edge_lo", "head", "inset", segC,
                      thickness=3.0))
    parts.append(part("acc_node_cresttip", "head", "accent",
                      node_pad((0.0, -17.4, 152.5), (0.0, -0.997, 0.0767)),
                      thickness=2.5))
    ops.append({"op": "LINE(crest trailing edge)", "kind": "LINE", "width": 8.0,
                "termination": "node",
                "parts": ["acc_crest_edge_hi", "acc_crest_edge_mid",
                          "ins_crest_edge_lo", "acc_node_cresttip"]})
    # LINE(fin trailing edges) x2 -- ~22 uu along each raked aft edge,
    # edge-terminated (closed; both ends land on the fin's silhouette edges)
    fe_l = edge_strip((12.8, -5.0, 42.0), (12.8, -15.0, 24.0), 8.0,
                      (0.0, -0.874, 0.485), thick=1.5, embed=0.0)
    parts.append(part("acc_finedge_l", "calf_l", "accent", fe_l, thickness=3.0))
    parts.append(part("acc_finedge_r", "calf_r", "accent", fe_l.mirrored_x(),
                      thickness=3.0))
    ops.append({"op": "LINE(fin trailing edges) x2", "kind": "LINE",
                "width": 8.0, "termination": "edge",
                "parts": ["acc_finedge_l", "acc_finedge_r"]})
    # RING(wrist_r) service ring at the right cuff flare (O/ 12)
    parts.append(service_ring_part("acc_service_ring", "r", ch.cuff_r))
    ops.append({"op": "RING(wrist_r)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})
    # NODE x1 at the left fin root -- the second mandatory node pad
    parts.append(part("acc_node_finroot_l", "calf_l", "accent",
                      node_pad((12.8, -4.6, 44.0), (0.0, -1.0, 0.0)),
                      thickness=2.5))
    ops.append({"op": "NODE(left fin root)", "kind": "NODE", "width": 8.0,
                "termination": "closed", "parts": ["acc_node_finroot_l"]})

    # --- panel insets (7, slot 1): thigh speed lines x2, forearm tops x2,
    # torso 45deg side rakes x2, pelvis chamfer notch (SS4 spec)
    parts.append(part("ins_thighline_l", "thigh_l", "inset",
                      limb_line("thigh_l", "calf_l", 0.10, 0.90,
                                (1.0, 0.0, 0.0), 6.4)))
    parts.append(part("ins_thighline_r", "thigh_r", "inset",
                      limb_line("thigh_r", "calf_r", 0.10, 0.90,
                                (-1.0, 0.0, 0.0), 6.4)))
    parts.append(part("ins_forearmline_l", "lowerarm_l", "inset",
                      limb_line("lowerarm_l", "hand_l", 0.12, 0.62,
                                (0.0, 0.0, 1.0), 4.6)))
    parts.append(part("ins_forearmline_r", "lowerarm_r", "inset",
                      limb_line("lowerarm_r", "hand_r", 0.12, 0.62,
                                (0.0, 0.0, 1.0), 4.6)))
    rake_l = edge_strip((17.0, 6.0, 141.0), (14.6, -6.0, 129.0), 4.0,
                        (1.0, 0.0, 0.0), thick=2.0, embed=1.7)
    parts.append(part("ins_sideRake_l", "spine_04", "inset", rake_l))
    parts.append(part("ins_sideRake_r", "spine_04", "inset",
                      rake_l.mirrored_x()))
    parts.append(part("ins_pelvis_notch", "pelvis", "inset",
                      box(8.0, 2.4, 4.0).translate(0, 10.3, 99.0)))

    features = {
        "crest": ["sig_crest", "acc_crest_edge_hi", "acc_crest_edge_mid",
                  "ins_crest_edge_lo", "acc_node_cresttip"],
        "calf_fins": ["sig_fin_l", "sig_fin_r", "acc_finedge_l",
                      "acc_finedge_r", "acc_node_finroot_l"],
    }
    return {"name": "Rocco", "parts": parts, "ops": ops, "features": features}


# ---------------------------------------------------------------------------
# LILY -- the wing, glacier ice #B8F8FF (CHARACTER_SHEETS SS13; the slight-frame
# variant, built second: the -80 chassis proves the parameterization)
# ---------------------------------------------------------------------------


def build_lily():
    # Chassis deviations (SS13): shoulders narrowed, pads shaved to 6 H
    # (outer +/-26, Z 144-150), chest W 28, joint cores with 5 uu air gaps
    # (rule A2 exception -- the exposed-frame read).
    #
    # *** WAVE 5: SHE IS THE ROSTER'S SLIGHTEST BODY AND NOW ACTUALLY MEASURES
    # *** THAT WAY.  She carried the BASELINE pelvis (30 x 22), the baseline
    # mitt and near-baseline legs, so "slight" lived in the chest and the arms
    # only; nine of ten bodies shared one torso block and one limb gauge and
    # the 46 px read had no low-frequency channel at all (W4-CENSUS SS7, J-1
    # and J-4 item 4).  Pelvis 25 x 18, thighs/calves/arms one gauge thinner,
    # mitt 10 x 12 x 6.5, boots 11 W x 10 H.
    #
    # *** AND THE CORES GREW, WHICH IS THE ELBOW FIX (W3-CHARPIPE's T8). ***
    # Her elbows were the roster's only joint without margin -- 1 px of
    # silhouette overlap at the animation set's own -34.1 deg where every
    # other body sits near 400.  The cause is arithmetic, not art: a joint
    # core BRIDGES its gap only when core radius > gap, because the segment
    # ends `joint_gap` uu short of the joint and the sphere has to reach back
    # into that segment's own axial extent.  Every other body is r 6.0 against
    # a 3 uu gap (2x margin); Lily was r 4.5 against a 5 uu gap, i.e. 0.5 uu
    # SHORT of touching, and the sheet's own two numbers ("cores O/ 9" +
    # "5 uu gaps", SS3 rule A2) are therefore mutually impossible.
    # The 5 uu gap is the canonical frailty read and it is KEPT; the core is
    # what moves, to O/ 13 -- the smallest that bridges with margin.  On limbs
    # this thin the bigger knuckle READS more exposed-frame, not less.
    ch = ChassisSpec(
        pelvis_w=25.0, pelvis_d=18.0,
        chest_w_waist=20.0, chest_w_top=28.0, chest_d=20.0,
        pad_w=16.0, pad_h=6.0, pad_outer=26.0, pad_chamfer=2.0,
        head_w=18.0, head_d=20.7,  # teardrop O/18, y-scale 1.15 in the builder
        upperarm_r0=4.0, upperarm_r1=3.5,
        forearm_r0=3.5, forearm_r1=3.3, cuff_r=4.5,
        mitt=(10.0, 12.0, 6.5),
        thigh_r0=4.4, thigh_r1=3.8, calf_r0=4.0, calf_r1=3.1,
        boot_w=11.0, boot_toe=20.0, boot_h=10.0,
        core_r_major=6.5, core_r_minor=6.5, core_rows=3, joint_gap=5.0,
    )
    parts = build_chassis(ch, head_builder=head_teardrop)
    # Team layout standard -- the fins flank, never cover, the back bar.
    parts.extend(team_layout(ch, CHARACTERS["Lily"]["visor_w"]))

    ops = []
    # --- signature: wing fins x2 -- mirrored extruded blades 4 T, chord
    # 14 -> 6, roots at the scapulae (Z 120-140, lateral +/-14), sweeping
    # up-back to tips at Z 208 (crown_break, physics-excluded), +/-20 out.
    # Sheet bone spine_03 -> spine_05 per the integrator remap (Z >= 118).
    # Tip plane at Z 206 so the HIGHEST corner (tip chord + thickness + the
    # tip node bead) stays under the hard Z 208 crown-break ceiling (LANGUAGE
    # SS4.8 -- the ceiling is a cap the sheet's 'tips at Z 208' reaches, and
    # the cap wins; T5 measures corners, not centerlines).
    fin_l, le_root, le_tip = blade((14.0, -10.0, 130.0), (20.0, -22.0, 206.0),
                                   14.0, 6.0, 4.0)
    parts.append(part("sig_fin_l", "spine_05", "suit", fin_l,
                      crown_break=True, thickness=4.0, gear_region="back"))
    parts.append(part("sig_fin_r", "spine_05", "suit", fin_l.mirrored_x(),
                      crown_break=True, thickness=4.0, gear_region="back"))

    # --- accent: LINE(fin leading edges x2) up each leading edge to the
    # tip -- the wing light; NODE at each tip (Z 208, on the blade -- the two
    # mandatory pads; crown-break parts may carry accent).  The sheet's
    # canonical drop-order IS APPLIED: lines start at Z 165, not Z 150 --
    # the full-length lines put measured accent area over the 8%% cap
    # (SS13 'Drop-order if over 8%%: shorten the fin lines to start at
    # Z 165'; implementers apply drop-orders mechanically, never invent).
    t165 = (165.0 - le_root[2]) / (le_tip[2] - le_root[2])
    p_start = lerp(le_root, le_tip, t165)
    le_dir = v_norm(v_sub(le_tip, le_root))
    chord_out = _perp(le_dir, (0.0, 1.0, 0.3))
    fe_l = edge_strip(p_start, le_tip, 8.0, chord_out, thick=1.5, embed=0.0)
    parts.append(part("acc_finedge_l", "spine_05", "accent", fe_l,
                      crown_break=True, thickness=3.0, gear_region="back"))
    parts.append(part("acc_finedge_r", "spine_05", "accent", fe_l.mirrored_x(),
                      crown_break=True, thickness=3.0, gear_region="back"))
    tip_axis = v_norm(v_sub((20.0, -22.0, 206.0), (14.0, -10.0, 130.0)))
    # bead embedded into the tip face (embed 1.75 -> pad center 0.5 uu inside
    # the tip) so its corners also respect the 208 ceiling
    node_l = node_pad((20.0, -22.0, 206.0), tip_axis, embed=1.75)
    parts.append(part("acc_node_fintip_l", "spine_05", "accent", node_l,
                      crown_break=True, thickness=2.5, gear_region="back"))
    parts.append(part("acc_node_fintip_r", "spine_05", "accent",
                      node_l.mirrored_x(), crown_break=True, thickness=2.5,
                      gear_region="back"))
    ops.append({"op": "LINE(fin leading edges) x2", "kind": "LINE",
                "width": 8.0, "termination": "node",
                "parts": ["acc_finedge_l", "acc_finedge_r",
                          "acc_node_fintip_l", "acc_node_fintip_r"]})
    # LINE(shin x2): one 12 uu vertical line per shin front, width 8 -- NOT
    # filaments (SS2.3 floor); edge-terminated at the boot cuff chamfer.
    parts.append(part("acc_shinline_l", "calf_l", "accent",
                      limb_line("calf_l", "foot_l", 0.42, 0.71,
                                (0.0, 1.0, 0.0), 3.9, width=8.0, thick=3.0,
                                embed=1.5)))
    parts.append(part("acc_shinline_r", "calf_r", "accent",
                      limb_line("calf_r", "foot_r", 0.42, 0.71,
                                (0.0, 1.0, 0.0), 3.9, width=8.0, thick=3.0,
                                embed=1.5)))
    ops.append({"op": "LINE(shin) x2", "kind": "LINE", "width": 8.0,
                "termination": "edge",
                "parts": ["acc_shinline_l", "acc_shinline_r"]})
    # RING(wrist_l) service ring
    parts.append(service_ring_part("acc_service_ring", "l", ch.cuff_r))
    ops.append({"op": "RING(wrist_l)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})

    # --- panel insets (6 -- sleek): one full-length line per limb pair
    # (delivered as the mirrored thigh pair), two torso side lines, two
    # fin-root plates (SS13)
    parts.append(part("ins_thighline_l", "thigh_l", "inset",
                      limb_line("thigh_l", "calf_l", 0.08, 0.92,
                                (1.0, 0.0, 0.0), 4.0)))
    parts.append(part("ins_thighline_r", "thigh_r", "inset",
                      limb_line("thigh_r", "calf_r", 0.08, 0.92,
                                (-1.0, 0.0, 0.0), 4.0)))
    side_l = edge_strip((13.4, 0.0, 144.0), (11.4, 0.0, 114.0), 4.0,
                        (1.0, 0.0, 0.0), thick=2.0, embed=1.7)
    parts.append(part("ins_sideline_l", "spine_04", "inset", side_l))
    parts.append(part("ins_sideline_r", "spine_04", "inset",
                      side_l.mirrored_x()))
    root_plate = box(10.0, 2.4, 14.0).translate(14.0, -11.3, 130.0)
    parts.append(part("ins_finroot_l", "spine_04", "inset", root_plate))
    parts.append(part("ins_finroot_r", "spine_04", "inset",
                      root_plate.mirrored_x()))

    features = {
        "wing_fins": ["sig_fin_l", "sig_fin_r", "acc_finedge_l",
                      "acc_finedge_r", "acc_node_fintip_l",
                      "acc_node_fintip_r"],
    }
    return {"name": "Lily", "parts": parts, "ops": ops, "features": features}


# ---------------------------------------------------------------------------
# per-sheet helmets that REPLACE the head lathe (each sheet's (b) table names
# the primitive; ch.head_* carries the dims so the chassis spec stays the
# single source).  CROWN LAW (deviation, see the tranche report): three sheets
# canonize crowns below Z 176 (Chut 174, Mortimer 172, Slimeball 168) while
# LANGUAGE SS4.1/SS4.8 + test T5 + the writer's own self-check require the
# bbox height to be 176 +/- 0.5 excluding crown breaks.  The height law is
# code-enforced and wins; those three helmets are raised so their crowns land
# at 176 and every other number on their sheets is kept verbatim.
# ---------------------------------------------------------------------------


def head_flattop(ch):
    """Chut / Mortimer: chamfered block helmet, W x D x (z1 - z0)."""
    return box(ch.head_w, ch.head_d, ch.head_z1 - ch.head_z0, chamfer=3.0) \
        .translate(0, 0, (ch.head_z0 + ch.head_z1) / 2.0)


def head_bell(ch):
    """Oyster: 10-side lathe diving-bell dome (the widest head, SHEETS SS7)."""
    r = ch.head_w / 2.0
    h = ch.head_z1 - ch.head_z0
    profile = [(r, 0.0), (r, h * 0.35), (r * 0.94, h * 0.58),
               (r * 0.69, h * 0.81), (0.0, h)]
    return lathe(profile, 10, smooth=True).translate(0, 0, ch.head_z0)


def head_dome(ch):
    """Slimeball: squat low dome (SHEETS SS11)."""
    r = ch.head_w / 2.0
    h = ch.head_z1 - ch.head_z0
    profile = [(r, 0.0), (r * 0.95, h * 0.31), (r * 0.77, h * 0.69), (0.0, h)]
    return lathe(profile, 8, smooth=True).translate(0, 0, ch.head_z0)


# ---------------------------------------------------------------------------
# CHUT -- the ram, signal green #A0F9A4 (CHARACTER_SHEETS SS5)
# ---------------------------------------------------------------------------


def build_chut():
    # Flat-top helmet 26 W x 22 D x 18 H; sheet crown 174 -> 176 (crown law
    # above), so the block sits Z 158-176 and the brow lip rides Z 169-173.
    #
    # *** WAVE 5: THE BRAWLER IS BUILT LIKE ONE.  *** His sheet calls him "the
    # ram" and the differentiation matrix gives him a flat crown and one hip
    # diagonal -- both of which are HEAD-and-hip cues on a baseline chassis,
    # so at 3,000 uu W4-CENSUS could not separate him from the roster at all
    # (SS7, J-3).  He now carries the mass the word "ram" promises: chest
    # 31 -> 39 W over a 27-deep block, pelvis 34 x 25, thighs O/ 16.4 at the
    # hip, calves O/ 14, upper arms O/ 13.6 feeding the gauntlets.  He is the
    # SECOND-heaviest body now and Mortimer is still visibly heavier, which is
    # the ordering CHARACTER_LANGUAGE SS5 asks for.  Pads stay inside the
    # +/-40 envelope (the +/-44 exception is Mortimer's alone): 19 W at outer
    # 40 rather than 16 at 40.
    ch = ChassisSpec(head_w=26.0, head_d=22.0, head_z0=158.0, head_z1=176.0,
                     pelvis_w=32.0, pelvis_d=24.0,
                     chest_w_waist=31.0, chest_w_top=40.0, chest_d=27.0,
                     pad_w=22.0, pad_d=24.0, pad_h=12.0, pad_outer=40.0,
                     upperarm_r0=6.6, upperarm_r1=5.6,
                     thigh_r0=7.8, thigh_r1=6.6, calf_r0=6.6, calf_r1=5.0,
                     mitt=(13.0, 15.0, 8.5),
                     boot_w=17.0, boot_toe=23.0,
                     core_r_major=8.5, core_r_minor=6.5)
    parts = build_chassis(ch, head_builder=head_flattop)
    # Gauntlets REPLACE the forearm prisms + cuff flares (SS5 chassis
    # deviation); the mitts stay -- the gauntlet is the delivery system, the
    # mitt is the hand inside it.
    parts = drop_parts(parts, ["forearm_l", "forearm_r", "cuff_l", "cuff_r"])

    ops = []
    gaunt = {}
    for s, sign in (("l", 1.0), ("r", -1.0)):
        la, ha = SKEL["lowerarm_" + s], SKEL["hand_" + s]
        d = v_norm(v_sub(ha, la))
        out = _perp(d, (sign, 0.0, 0.0))
        gaunt[s] = (la, ha, d, out)
        parts.append(part("sig_gauntlet_" + s, "lowerarm_" + s, "suit",
                          oriented_box(lerp(la, ha, 0.56), d, out,
                                       26.0, 12.0, 18.0, chamfer=2.0),
                          thickness=12.0))
        # 4 short blocky plates per gauntlet (top + outer), slot 1
        side = v_norm(v_cross(d, out))
        for k, t in enumerate((0.34, 0.50, 0.66, 0.80)):
            c = v_add(lerp(la, ha, t), v_scale(side, (9.0 - 0.6) * (1 if k % 2
                                                                    else -1)))
            parts.append(part("ins_gaunt_%s_%d" % (s, k), "lowerarm_" + s,
                              "inset",
                              frame_box(c, d, v_scale(side, 1 if k % 2 else -1),
                                        6.0, 5.0, 2.0)))

    # --- signature: chest keel (wedge prow ridge, 10 W, 8 proud, Z 116-142,
    # on the sternum centerline between the twin chest bars: the bars sit at
    # +/-11 so G1's 12 uu of bare surface is satisfied trivially -- the keel
    # is slot 0, not accent).
    keel = yz_extrude([(10.0, 116.0), (20.0, 129.0), (10.0, 142.0)], 10.0)
    parts.append(part("sig_keel", "spine_04", "suit", keel,
                      gear_region="front"))

    # --- signature: brow lip, 4 uu proud above the visor (SS5)
    parts.append(part("sig_neckcollar", "neck_02", "inset",
                      prism(6, 5.6, 7.0).translate(0.0, 0.0, 155.5)))
    parts.append(part("sig_browlip", "head", "suit_head",
                      box(26.0, 6.0, 4.0).translate(0, 12.0, 171.0)))
    parts.append(part("ins_browlip_under", "head", "inset",
                      box(24.0, 3.0, 2.0).translate(0, 12.5, 168.6)))

    # --- signature: knife sheath, plate 30 x 8 x 4 on the diagonal lower back
    sh0, sh1 = (17.0, -18.0, 117.0), (-17.0, -18.0, 95.0)
    sheath_d = v_norm(v_sub(sh1, sh0))
    parts.append(part("sig_sheath", "spine_01", "inset",
                      frame_box(v_scale(v_add(sh0, sh1), 0.5), sheath_d,
                                (0.0, -1.0, 0.0), 40.0, 8.0, 6.0),
                      gear_region="back"))

    # --- team layout: visor 18 x 8 under the brow lip; helmet-back section on
    # the flat-top's rear face, clipped to the crown (SS5).
    helm_back = [part("team_backbar_helm", "head", "team_glow",
                      box(24.0, 8.5, 18.0).translate(0, -8.75, 167.0))]
    parts.extend(team_layout(ch, CHARACTERS["Chut"]["visor_w"],
                             helm_back_parts=helm_back,
                             visor_center=(0.0, 9.5, 162.0)))

    # --- accent program (SS5 table).  Measured area decides the sheet's own
    # drop-order ("reduce chevron count to 2 per gauntlet first"); CHEVRONS
    # below is that knob and nothing else.
    CHEVRONS = 2      # sheet drop-order applied: measured area at 3 per
                      # gauntlet reads 8.49%% > the 8%% cap (SS5: "reduce
                      # chevron count to 2 per gauntlet first")
    for s, sign in (("l", 1.0), ("r", -1.0)):
        la, ha, d, out = gaunt[s]
        names = []
        for k in range(CHEVRONS):
            t = 0.42 + 0.16 * k
            apex = v_add(lerp(la, ha, t), v_scale(out, 8.0))
            nm = "acc_chevron_%s_%d" % (s, k)
            parts.append(part(nm, "lowerarm_" + s, "accent",
                              chevron(apex, d, out, size=12.0, width=8.0)))
            names.append(nm)
        ops.append({"op": "CHEVRON(gauntlet outer face, %d, 12) %s"
                    % (CHEVRONS, s), "kind": "CHEVRON", "width": 8.0,
                    "termination": "closed", "parts": names})

    # LINE(sheath edge): the 30 uu lower silhouette edge.  G2 (SS0.4): the top
    # 8 uu, nearest the torso back-bar corner, binds slot 1.
    w_dir = v_norm(v_cross(sheath_d, (0.0, -1.0, 0.0)))
    e0 = v_add(sh0, v_scale(w_dir, -4.0))
    e1 = v_add(sh1, v_scale(w_dir, -4.0))
    g2 = lerp(e0, e1, 8.0 / 40.0)
    parts.append(part("ins_sheath_edge_top", "spine_01", "inset",
                      edge_strip(e0, g2, 8.0, (0.0, -1.0, 0.0),
                                 thick=1.5, embed=0.0)))
    parts.append(part("acc_sheath_edge", "spine_01", "accent",
                      edge_strip(g2, e1, 8.0, (0.0, -1.0, 0.0),
                                 thick=1.5, embed=0.0)))
    parts.append(part("acc_node_sheathtip", "spine_01", "accent",
                      node_pad(v_add(sh1, v_scale((0.0, -1.0, 0.0), 2.0)),
                               (0.0, -1.0, 0.0))))
    ops.append({"op": "LINE(sheath edge)", "kind": "LINE", "width": 8.0,
                "termination": "node",
                "parts": ["ins_sheath_edge_top", "acc_sheath_edge",
                          "acc_node_sheathtip"]})

    # RING(wrist_l): 8 uu band at the left gauntlet's wrist step
    la, ha, d, _out = gaunt["l"]
    parts.append(part("acc_service_ring", "lowerarm_l", "accent",
                      ring_band(lerp(la, ha, 0.90), d, 10.0, band_w=8.0),
                      thickness=8.0))
    ops.append({"op": "RING(wrist_l)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})

    # NODE x1 at the chest keel base.  G2, not G1: the pad rides the keel's
    # proud lower facet, and the intervening surface back to either chest bar
    # runs 8 uu down the keel flank + 5 across the chest = 13 >= 12.
    parts.append(part("acc_node_keelbase", "spine_04", "accent",
                      node_pad((0.0, 11.6, 118.5), (0.0, 0.86, -0.51))))
    ops.append({"op": "NODE(chest keel base)", "kind": "NODE", "width": 8.0,
                "termination": "closed", "parts": ["acc_node_keelbase"]})

    # --- panel insets: 8 gauntlet plates above + 2 torso side plates = 10
    side_l = edge_strip((17.5, 4.0, 140.0), (15.0, 4.0, 118.0), 4.0,
                        (1.0, 0.0, 0.0), thick=2.0, embed=1.7)
    parts.append(part("ins_torso_l", "spine_04", "inset", side_l))
    parts.append(part("ins_torso_r", "spine_04", "inset", side_l.mirrored_x()))

    features = {
        "gauntlets": ["sig_gauntlet_l", "sig_gauntlet_r"],
        "flat_top": ["head", "sig_browlip"],
        "sheath": ["sig_sheath", "acc_sheath_edge", "ins_sheath_edge_top",
                   "acc_node_sheathtip"],
    }
    return {"name": "Chut", "parts": parts, "ops": ops, "features": features}


# ---------------------------------------------------------------------------
# MACE -- the hunter, violet #DFC4FE (CHARACTER_SHEETS SS6)
# ---------------------------------------------------------------------------


def build_mace():
    # *** WAVE 5: TALL-LEAN, AND ON A ROSTER WHERE HEIGHT IS A CONSTANT THAT
    # *** HAS TO BE A PROPORTION.  Every body is exactly 176 uu and no recipe
    # may move a joint (PIPELINE SS3.3), so "tall" cannot be built by making
    # her taller -- it is built by making everything that reads as WIDTH
    # smaller and lifting the shoulder line: chest 23 -> 32 W (baseline
    # 26 -> 36) over a 21-deep block, pelvis 26 x 20, limbs one gauge under
    # baseline, pads narrowed to 14 W and pulled in to outer +/-36, and boots
    # 14 W x 20 L x 9 H (baseline 16 x 22 x 12) -- a smaller foot under a
    # narrow figure is the oldest "tall" cue there is.  The hood is untouched
    # and still hems onto the pad tops at Z 150, so pad_top stays 150.
    ch = ChassisSpec(
        pelvis_w=28.0, pelvis_d=20.0,
        chest_w_waist=23.0, chest_w_top=32.0, chest_d=21.0,
        pad_w=14.0, pad_d=18.0, pad_h=8.0, pad_outer=36.0, pad_top=150.0,
        upperarm_r0=4.8, upperarm_r1=4.0,
        forearm_r0=4.4, forearm_r1=3.8, cuff_r=5.2,
        mitt=(11.0, 13.0, 7.0),
        thigh_r0=6.0, thigh_r1=5.2, calf_r0=5.2, calf_r1=4.0,
        boot_w=14.0, boot_toe=20.0, boot_h=9.0,
        core_r_major=7.0, core_r_minor=5.5)
    parts = build_chassis(ch)
    ops = []

    # --- signature: the cowl hood.  Hard pentagonal shell, 4 thick, peak
    # Z 186 (crown_break) leaning 6 fwd, hem riding both pad tops and draping
    # to Z 140 beside the face, SPLIT AT THE SPINE (back edges at +/-6 -> the
    # canonical "split at the spine so the back bar stays clear").  Built as
    # five tapered plates converging on the peak; the face aperture is the gap
    # between the front drapes and under the brow plate, which is how the
    # visor "glints from darkness".  A2: inner faces stay >= 4 uu off the
    # head shell (drape inner edges at +/-14.5 vs the Ø 20 head).
    peak = (0.0, 6.0, 186.0)
    hemA = (14.5, 14.0, 140.0)      # front drape, beside the face
    hemB = (25.0, -2.0, 150.0)      # ON the shoulder pad top (Z 150)
    hemC = (7.0, -19.0, 146.0)      # spine split edge
    brow = ((13.0, 15.0, 172.0), (-13.0, 15.0, 172.0))
    hood_segs = [("brow", brow[0], brow[1])]
    for s, sign in (("l", 1.0), ("r", -1.0)):
        mk = lambda p: (sign * p[0], p[1], p[2])  # noqa: E731
        hood_segs.append(("front_" + s, mk(hemA), mk(hemB)))
        hood_segs.append(("back_" + s, mk(hemB), mk(hemC)))
    hem_edges = []
    for (nm, p0, p1) in hood_segs:
        root = v_scale(v_add(p0, p1), 0.5)
        chord = v_len(v_sub(p1, p0))
        tip = lerp(root, peak, 0.96)
        m, _le0, _le1 = blade(root, tip, chord, 5.0, 4.0,
                              chord_hint=v_sub(p1, p0))
        parts.append(part("sig_hood_" + nm, "spine_05", "suit", m,
                          crown_break=True, thickness=4.0))
        if nm != "brow":
            hem_edges.append((nm, p0, p1))

    # --- signature: rope coils, 3 stacked 270-degree arcs on the RIGHT hip
    # (right = -X on this rig).  Rule A1's pelvis envelope is not granted a
    # 27 uu reading: the outermost loop's outer face lands at -25.5.
    coil_names = []
    # WAVE 5: the stack moved in 1 uu with the narrower pelvis (28 W now) so
    # the inboard loop still touches the hip -- outer face -24.5, still inside
    # A1's 26 uu pelvis envelope.
    for k, cx in enumerate((-17.0, -19.5, -22.0)):
        m = torus_arc((cx, 6.0, 90.0), (-1.0, 0.0, 0.0), 6.5, 2.5, 270.0,
                      start_deg=135.0, seg=5, sides=6)
        nm = "sig_coil_%d" % k
        slot = "accent" if k == 2 else "inset"
        if k == 2:
            nm = "acc_coil_ring"
        parts.append(part(nm, "pelvis", slot, m, gear_region="pelvis_side"))
        coil_names.append(nm)
    # RING(coil top): the sheet's "8 uu band along the top coil's outer face,
    # full 270 arc".  Delivered as the outboard loop itself: its visible outer
    # half is a 9 uu band of tube surface following the coil's own silhouette
    # edge, and the two inboard loops are the dark rope behind it.
    ops.append({"op": "RING(coil top)", "kind": "RING", "width": 9.1,
                "termination": "edge", "parts": ["acc_coil_ring"]})

    # --- signature: spike holster on the left forearm outer face
    la, ha = SKEL["lowerarm_l"], SKEL["hand_l"]
    d_fa = v_norm(v_sub(ha, la))
    out_l = _perp(d_fa, (1.0, 0.0, 0.0))
    hol_c = v_add(lerp(la, ha, 0.55), v_scale(out_l, 6.5))
    parts.append(part("sig_holster", "lowerarm_l", "inset",
                      frame_box(hol_c, d_fa, out_l, 18.0, 8.0, 6.0)))
    sp0 = v_add(hol_c, v_scale(d_fa, -8.0))
    sp1 = v_add(hol_c, v_scale(d_fa, 8.0))
    parts.append(part("sig_spike", "lowerarm_l", "inset",
                      limb(sp0, sp1, 6, 2.0, 2.0)))

    # --- team deviation: the helmet-back bar section renders on the hood's
    # rear faces as two 9-wide strips flanking the spine split; torso section
    # standard and fully exposed by the split.
    helm_back = []
    for s, sign in (("l", 1.0), ("r", -1.0)):
        a = (sign * 10.5, -18.0, 152.0)
        b = (sign * 6.5, -12.5, 174.5)
        helm_back.append(part("team_backbar_helm_" + s, "spine_05",
                              "team_glow",
                              frame_box(v_scale(v_add(a, b), 0.5),
                                        v_sub(b, a), (0.0, -1.0, 0.0),
                                        v_len(v_sub(b, a)), 9.0, 4.0)))
    parts.extend(team_layout(ch, CHARACTERS["Mace"]["visor_w"],
                             helm_back_parts=helm_back))

    # --- accent program.  HEM_FULL is the sheet's drop-order knob ("shorten
    # the arm line to 30 uu, then reduce the hem line to the front arc only").
    ARM_T1 = 0.86
    HEM_FULL = True
    # ARM_T0 is the sheet's FIRST drop-order step ("shorten the arm line to
    # 30 uu"), applied mechanically in wave 5 for the reason the drop-order
    # exists: the tall-lean chassis has less surface than the baseline one, so
    # the same accent programme measured 7.98% against her 8.0% cap -- inside
    # the cap but with 0.02% of margin, which is not a margin.  Shortened from
    # the SHOULDER end, never the wrist end, because the op is
    # NODE-terminated at the wrist and a line that stops short of its own node
    # is an open op (SHEETS SS0.4).  Measured run: 30.6 uu, was 40.9.
    ARM_T0 = 0.50
    hem_parts = []
    for (nm, p0, p1) in hem_edges:
        if not HEM_FULL and not nm.startswith("front"):
            continue
        mid = v_scale(v_add(p0, p1), 0.5)
        out = v_norm((mid[0], mid[1] - 2.0, 0.0))
        pnm = "acc_hem_" + nm
        parts.append(part(pnm, "spine_05", "accent",
                          edge_strip(p0, p1, 8.0, out, thick=1.5, embed=0.0)))
        hem_parts.append(pnm)
    ops.append({"op": "LINE(hem edge, full contour)", "kind": "LINE",
                "width": 8.0, "termination": "edge", "parts": hem_parts})

    # LINE(right arm outer face, shoulder->wrist): two runs crossing the elbow
    # gap, NODE at the wrist -- "her kit is a line under tension".
    parts.append(part("acc_armline_hi", "upperarm_r", "accent",
                      limb_line("upperarm_r", "lowerarm_r", ARM_T0, 0.88,
                                (-1.0, 0.0, 0.0), 5.0, width=8.0, thick=3.0,
                                embed=1.5)))
    parts.append(part("acc_armline_lo", "lowerarm_r", "accent",
                      limb_line("lowerarm_r", "hand_r", 0.10, ARM_T1,
                                (-1.0, 0.0, 0.0), 4.8, width=8.0, thick=3.0,
                                embed=1.5)))
    lar, har = SKEL["lowerarm_r"], SKEL["hand_r"]
    d_r = v_norm(v_sub(har, lar))
    out_r = _perp(d_r, (-1.0, 0.0, 0.0))
    parts.append(part("acc_node_wrist_r", "lowerarm_r", "accent",
                      node_pad(v_add(lerp(lar, har, 0.90),
                                     v_scale(out_r, 5.5)), out_r)))
    ops.append({"op": "LINE(right arm outer face, shoulder->wrist)",
                "kind": "LINE", "width": 8.0, "termination": "node",
                "parts": ["acc_armline_hi", "acc_armline_lo",
                          "acc_node_wrist_r"]})

    parts.append(ankle_ring_part("acc_service_ring", "r", 6.0))
    ops.append({"op": "RING(ankle_r)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})
    parts.append(part("acc_node_spiketip", "lowerarm_l", "accent",
                      node_pad(v_add(sp1, v_scale(out_l, 2.0)), out_l)))
    ops.append({"op": "NODE(spike holster tip)", "kind": "NODE", "width": 8.0,
                "termination": "closed", "parts": ["acc_node_spiketip"]})

    # --- panel insets (8): two long rakes per thigh, one per shin, two hood
    # shell creases
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("ins_thighrake_a_" + s, "thigh_" + s, "inset",
                          limb_line("thigh_" + s, "calf_" + s, 0.12, 0.88,
                                    (sign, 0.0, 0.35), 5.6)))
        parts.append(part("ins_thighrake_b_" + s, "thigh_" + s, "inset",
                          limb_line("thigh_" + s, "calf_" + s, 0.12, 0.88,
                                    (sign, 0.0, -0.55), 5.6)))
        parts.append(part("ins_shinline_" + s, "calf_" + s, "inset",
                          limb_line("calf_" + s, "foot_" + s, 0.15, 0.70,
                                    (0.0, 1.0, 0.0), 4.6)))
        crease0 = (sign * 13.0, 4.0, 166.0)
        crease1 = (sign * 17.0, -4.0, 152.0)
        parts.append(part("ins_hoodcrease_" + s, "spine_05", "inset",
                          edge_strip(crease0, crease1, 4.0,
                                     (sign, 0.4, 0.0), thick=2.0, embed=1.7)))

    features = {
        "hood": [p["name"] for p in parts if p["name"].startswith("sig_hood")],
        "coils": coil_names,
    }
    return {"name": "Mace", "parts": parts, "ops": ops, "features": features}


# ---------------------------------------------------------------------------
# OYSTER -- the alchemist, deep sea green #6FE5A2 (CHARACTER_SHEETS SS7)
# ---------------------------------------------------------------------------


def build_oyster():
    ch = ChassisSpec(head_w=32.0, head_d=32.0, head_z0=150.0, head_z1=176.0)
    parts = build_chassis(ch, head_builder=head_bell)
    # rounded shoulder caps REPLACE the pad boxes (SS7 chassis deviation)
    parts = drop_parts(parts, ["pad_l", "pad_r"])
    ops = []

    cap_profile = [(10.0, 0.0), (9.4, 4.0), (7.5, 7.5), (0.0, 10.0)]
    for s, sign in (("l", 1.0), ("r", -1.0)):
        cap = lathe(cap_profile, 8, smooth=True).scale(1.0, 0.8, 1.0)
        parts.append(part("sig_shouldercap_" + s, "clavicle_" + s, "suit",
                          cap.translate(sign * 30.0, 0.0, ch.pad_top - 10.0),
                          thickness=10.0))

    # --- signature: jar rack (4 uu strut frame, struts proud 6) + three jars
    for k, (px, pz) in enumerate(((13.0, 133.0), (-13.0, 133.0))):
        parts.append(part("sig_rackstrut_%d" % k, "spine_05", "inset",
                          box(4.0, 6.0, 50.0).translate(px, -16.0, pz),
                          gear_region="back"))
    parts.append(part("sig_rackcross", "spine_05", "inset",
                      box(30.0, 6.0, 4.0).translate(0.0, -16.0, 130.0),
                      gear_region="back"))

    jars = (("l", 21.0, 146.0, -16.5, "spine_05"),
            ("r", -21.0, 146.0, -16.5, "spine_05"),
            ("lo", 19.0, 124.0, -19.0, "spine_04"))
    for (tag, jx, jz, jy, bone) in jars:
        parts.append(part("sig_jar_" + tag, bone, "suit",
                          prism(8, 7.0, 20.0).translate(jx, jy, jz),
                          gear_region="back"))
        # RING(jar collar): 8 uu 8-gon band at the top rim -- the world
        # jar+collar rhyme.  G2: the three segments facing the spine bar bind
        # slot 1 (they pass within 12 uu of the back bar).
        cz = jz + 6.0
        parts.append(part("acc_collar_" + tag, bone, "accent",
                          arc_shell((jx, jy, cz), (0.0, 0.0, 1.0),
                                    7.0, 8.6, -4.0, 4.0, 8,
                                    i0=(5 if jx > 0 else 0), count=5)))
        parts.append(part("ins_collar_" + tag, bone, "inset",
                          arc_shell((jx, jy, cz), (0.0, 0.0, 1.0),
                                    7.0, 8.6, -4.0, 4.0, 8,
                                    i0=(2 if jx > 0 else 5), count=3)))
    ops.append({"op": "RING(jar collars x3)", "kind": "RING", "width": 8.0,
                "termination": "closed",
                "parts": ["acc_collar_l", "acc_collar_r", "acc_collar_lo"]})

    # --- signature: porthole on the bell's RIGHT side face, 60 degrees right
    # of front (right = -X), Z 164 -- it shows at the portrait yaw.
    ang = math.radians(60.0)
    pdir = (-math.sin(ang), math.cos(ang), 0.0)
    psurf = (pdir[0] * 15.1, pdir[1] * 15.1, 164.0)
    up = (0.0, 0.0, 1.0)
    tang = v_norm(v_cross(pdir, up))
    parts.append(part("ins_porthole", "head", "inset",
                      frame_box(v_add(psurf, v_scale(pdir, -1.0)), up, pdir,
                                10.0, 10.0, 3.0)))
    rim_parts = []
    rim = ((tang, up, 7.5, 22.0, "top", "accent"),
           (tang, v_scale(up, -1.0), 7.5, 22.0, "bot", "accent"),
           (up, tang, 7.5, 9.0, "far", "accent"),
           (up, v_scale(tang, -1.0), 7.5, 9.0, "near", "inset"))
    for (ldir, odir, off, length, tag, slot) in rim:
        c = v_add(v_add(psurf, v_scale(odir, off)), v_scale(pdir, 0.75))
        nm = ("acc_" if slot == "accent" else "ins_") + "porthole_" + tag
        parts.append(part(nm, "head", slot,
                          frame_box(c, ldir, pdir, length, 8.0, 2.5)))
        if slot == "accent":
            rim_parts.append(nm)
    # G2: the rim strip nearest the front visor band binds slot 1 -- the
    # crescent adjudication the sheet uses for X's monocle, same clause.
    ops.append({"op": "LINE(porthole rim)", "kind": "LINE", "width": 8.0,
                "termination": "edge", "parts": rim_parts})

    # --- team: visor 14 x 8 on the bell's front face (center Z 160); the
    # helmet-back section rides the bell's rear face.
    helm_back = [part("team_backbar_helm_lo", "head", "team_glow",
                      box(24.0, 8.0, 14.0).translate(0, -12.5, 159.0)),
                 part("team_backbar_helm_hi", "head", "team_glow",
                      box(16.0, 7.0, 10.0).translate(0, -10.0, 171.0))]
    parts.extend(team_layout(ch, CHARACTERS["Oyster"]["visor_w"],
                             helm_back_parts=helm_back,
                             visor_center=(0.0, 14.3, 160.0)))

    # --- DASH(shin fronts, 8, 12, 14): the drip motif.  LOWER_DRIPS is the
    # sheet's drop-order knob ("drop the lower shin segment pair first").
    LOWER_DRIPS = False   # sheet drop-order applied: with both segment
    #                       pairs the measured accent reads 9.07%% > 8%%
    #                       (SS7: "drop the lower shin segment pair first")
    dash_parts = []
    for s in ("l", "r"):
        parts.append(part("acc_drip_hi_" + s, "calf_" + s, "accent",
                          limb_line("calf_" + s, "foot_" + s, 0.16, 0.45,
                                    (0.0, 1.0, 0.0), 5.2, width=8.0,
                                    thick=3.0, embed=1.5)))
        dash_parts.append("acc_drip_hi_" + s)
        if LOWER_DRIPS:
            parts.append(part("acc_drip_lo_" + s, "calf_" + s, "accent",
                              limb_line("calf_" + s, "foot_" + s, 0.62, 0.90,
                                        (0.0, 1.0, 0.0), 4.6, width=8.0,
                                        thick=3.0, embed=1.5)))
            dash_parts.append("acc_drip_lo_" + s)
        fo = SKEL["foot_" + s]
        parts.append(part("acc_node_bootcuff_" + s, "foot_" + s, "accent",
                          node_pad((fo[0], fo[1] + 6.0, fo[2] + 3.0),
                                   (0.0, 1.0, 0.0))))
        dash_parts.append("acc_node_bootcuff_" + s)
    ops.append({"op": "DASH(shin fronts, 8, 12, 14)", "kind": "DASH",
                "width": 8.0, "termination": "node", "parts": dash_parts})

    parts.append(ankle_ring_part("acc_service_ring", "l", 6.0, t=0.62))
    ops.append({"op": "RING(ankle_l)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})

    # --- panel insets (11): rack strut bolts (4), two belt plates, two chest
    # side plates, three boot plates
    for k, (bx, bz) in enumerate(((13.0, 155.0), (-13.0, 155.0),
                                  (13.0, 111.0), (-13.0, 111.0))):
        parts.append(part("ins_rackbolt_%d" % k, "spine_05", "inset",
                          box(6.0, 2.4, 5.0).translate(bx, -19.3, bz)))
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("ins_belt_" + s, "pelvis", "inset",
                          box(11.0, 2.4, 5.0).translate(sign * 7.0, 11.3,
                                                        104.0)))
        parts.append(part("ins_chestside_" + s, "spine_04", "inset",
                          edge_strip((sign * 17.4, 3.0, 140.0),
                                     (sign * 14.6, 3.0, 120.0), 4.0,
                                     (sign, 0.0, 0.0), thick=2.0, embed=1.7)))
        fo = SKEL["foot_" + s]
        parts.append(part("ins_boot_" + s, "foot_" + s, "inset",
                          box(10.0, 6.0, 2.4).translate(fo[0], fo[1] + 6.0,
                                                        11.3)))
    parts.append(part("ins_boot_toe", "foot_l", "inset",
                      box(10.0, 5.0, 2.4).translate(SKEL["foot_l"][0],
                                                    SKEL["foot_l"][1] + 15.0,
                                                    7.0)))

    features = {
        "bell": ["head", "ins_porthole", "acc_porthole_top",
                 "acc_porthole_bot", "acc_porthole_far",
                 "ins_porthole_near"],
        "jar_rack": ["sig_jar_l", "sig_jar_r", "sig_jar_lo",
                     "sig_rackstrut_0", "sig_rackstrut_1", "sig_rackcross",
                     "acc_collar_l", "acc_collar_r", "acc_collar_lo",
                     "ins_collar_l", "ins_collar_r", "ins_collar_lo"],
        "shoulder_caps": ["sig_shouldercap_l", "sig_shouldercap_r"],
    }
    return {"name": "Oyster", "parts": parts, "ops": ops, "features": features}


# ---------------------------------------------------------------------------
# X -- the antenna, rose #FFAADD (CHARACTER_SHEETS SS8)
# ---------------------------------------------------------------------------


def build_x():
    ch = ChassisSpec(head_w=18.0, head_d=24.0)   # narrowest helmet
    parts = build_chassis(ch)
    ops = []

    # --- signature: hive unit (chamfered box 26 x 10 x 18, upper back)
    parts.append(part("sig_hive", "spine_05", "suit",
                      box(26.0, 10.0, 18.0, chamfer=2.0)
                      .translate(0.0, -17.0, 131.0), gear_region="back"))

    # --- team deviation (canonical): the back bar narrows to 20 uu across the
    # hive's Z 122-140 span, rendered on the hive's rear face and perforated
    # by the five cells; full 24 above and below.
    torso_back = [
        part("team_backbar_torso_lo", "spine_04", "team_glow",
             box(24.0, 7.5, 10.0).translate(0, -9.75, 117.0)),
        part("team_backbar_torso_hive", "spine_05", "team_glow",
             box(20.0, 6.0, 18.0).translate(0, -23.5, 131.0)),
        part("team_backbar_torso_hi", "spine_05", "team_glow",
             box(24.0, 7.5, 6.0).translate(0, -9.75, 143.0)),
    ]
    parts.extend(team_layout(ch, CHARACTERS["X"]["visor_w"],
                             torso_back_parts=torso_back,
                             visor_center=(3.0, 10.5, 162.0)))

    # hive cells: five hex punches through the team light, quincunx.  NO
    # accent (canonical -- they read via the bee FX); count the cells, count
    # the bees.
    for k, (cx, cz) in enumerate(((-5.0, 126.5), (5.0, 126.5), (0.0, 131.0),
                                  (-5.0, 135.5), (5.0, 135.5))):
        parts.append(part("ins_hivecell_%d" % k, "spine_05", "inset",
                          prism(6, 3.5, 2.0).rotate((1.0, 0.0, 0.0), 90.0)
                          .translate(cx, -25.8, cz), gear_region="back"))

    # --- signature: three antenna masts + finial beads.  Sheet tips are
    # Z 196/204/198; the beads are Ø 8 and SS4.8's crown-break ceiling is a
    # hard cap on GEOMETRY, so the tip planes drop 2 uu and the bead corners
    # land at 198/206/200 (same adjudication as Lily's fin tips, W1 dev. 3).
    mast_names, bead_names = [], []
    for k, (rx, tx, tz) in enumerate(((9.0, 20.5, 194.0), (0.0, 0.0, 202.0),
                                      (-9.0, -20.5, 196.0))):
        tip = (tx, -21.0, tz)
        m, _a, _b = blade((rx, -18.0, 140.0), tip, 6.0, 4.5, 4.0,
                          chord_hint=(1.0, 0.0, 0.0))
        parts.append(part("sig_mast_%d" % k, "spine_05", "suit", m,
                          crown_break=True, thickness=4.0))
        parts.append(part("acc_bead_%d" % k, "spine_05", "accent",
                          sphere(4.0, sides=4, rows=2).translate(*tip),
                          crown_break=True, thickness=8.0))
        mast_names.append("sig_mast_%d" % k)
        bead_names.append("acc_bead_%d" % k)
    ops.append({"op": "finial beads x3", "kind": "NODE", "width": 8.0,
                "termination": "closed", "parts": bead_names})
    # the two mandatory node pads: roots of the two outer masts, on the hive
    for s, rx in (("l", 9.0), ("r", -9.0)):
        parts.append(part("acc_node_mastroot_" + s, "spine_05", "accent",
                          node_pad((rx, -17.0, 140.5), (0.0, 0.0, 1.0)),
                          gear_region="back"))
    ops.append({"op": "NODE(outer mast roots) x2", "kind": "NODE",
                "width": 8.0, "termination": "closed",
                "parts": ["acc_node_mastroot_l", "acc_node_mastroot_r"]})

    # --- signature: monocle on the helmet front-right (right = -X).  8-gon
    # ring, centerline Ø 16, band 8 wide.  G2: the three segments facing the
    # visor bind slot 1 -- a CRESCENT-lit monocle, exactly as canonized.
    mc = (-7.0, 8.2, 162.0)
    parts.append(part("ins_monocle_lens", "head", "inset",
                      prism(8, 4.0, 2.0).rotate((1.0, 0.0, 0.0), 90.0)
                      .translate(*mc)))
    parts.append(part("acc_monocle_ring", "head", "accent",
                      arc_shell(mc, (0.0, 1.0, 0.0), 4.0, 12.0, 0.0, 3.0, 8,
                                i0=4, count=5)))
    parts.append(part("ins_monocle_ring", "head", "inset",
                      arc_shell(mc, (0.0, 1.0, 0.0), 4.0, 12.0, 0.0, 3.0, 8,
                                i0=1, count=3)))
    ops.append({"op": "LINE(monocle rim)", "kind": "LINE", "width": 8.0,
                "termination": "edge", "parts": ["acc_monocle_ring"]})

    # --- signature: quarter-cone dish on the LEFT pad, rim arc outboard
    quarter = [(0.0, 0.0), (0.0, -9.0), (-3.4, -8.3), (-6.4, -6.4),
               (-8.3, -3.4), (-9.0, 0.0)]
    dish_c = (31.0, 10.0, 142.0)
    parts.append(part("sig_dish", "clavicle_l", "suit",
                      face_shape(dish_c, (0.0, 1.0, 0.0), quarter, 4.0,
                                 embed=2.0), thickness=4.0))
    # LINE(dish rim): a ribbon wrapping the disc's silhouette edge -- 2.3 uu
    # onto each face + the 4.6 uu rim = 9.2 uu of surface, inside the [8, 10]
    # width law.  G2: the innermost segment (nearest the pad-top team strip)
    # binds slot 1.
    parts.append(part("acc_dish_rim", "clavicle_l", "accent",
                      arc_shell(dish_c, (0.0, 1.0, 0.0), 7.5, 9.8, -2.3, 2.3,
                                16, i0=9, count=3), thickness=4.6))
    parts.append(part("ins_dish_rim", "clavicle_l", "inset",
                      arc_shell(dish_c, (0.0, 1.0, 0.0), 7.5, 9.8, -2.3, 2.3,
                                16, i0=12, count=1), thickness=4.6))
    ops.append({"op": "LINE(dish rim)", "kind": "LINE", "width": 9.2,
                "termination": "edge", "parts": ["acc_dish_rim"]})

    parts.append(service_ring_part("acc_service_ring", "r", ch.cuff_r))
    ops.append({"op": "RING(wrist_r)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})

    # --- panel insets (9): hive seam lines (3), two chest side plates, one
    # per forearm, two shin ticks
    for k, cz in enumerate((123.5, 131.0, 138.5)):
        parts.append(part("ins_hiveseam_%d" % k, "spine_05", "inset",
                          box(24.0, 2.4, 3.0).translate(0.0, -22.3, cz)))
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("ins_chestside_" + s, "spine_04", "inset",
                          edge_strip((sign * 17.4, 3.0, 141.0),
                                     (sign * 14.6, 3.0, 119.0), 4.0,
                                     (sign, 0.0, 0.0), thick=2.0, embed=1.7)))
        parts.append(part("ins_forearmline_" + s, "lowerarm_" + s, "inset",
                          limb_line("lowerarm_" + s, "hand_" + s, 0.14, 0.64,
                                    (0.0, 0.0, 1.0), 4.6)))
        parts.append(part("ins_shintick_" + s, "calf_" + s, "inset",
                          limb_line("calf_" + s, "foot_" + s, 0.30, 0.46,
                                    (0.0, 1.0, 0.0), 4.8)))

    features = {
        "masts": mast_names + bead_names,
        "shoulder_rig": ["sig_hive", "team_backbar_torso_hive", "sig_dish",
                         "acc_dish_rim", "ins_dish_rim"],
        "monocle_helm": ["head", "acc_monocle_ring", "ins_monocle_ring",
                         "ins_monocle_lens"],
    }
    return {"name": "X", "parts": parts, "ops": ops, "features": features}


# ---------------------------------------------------------------------------
# ROXIE -- the rocketeer, ember #FF617C (CHARACTER_SHEETS SS9; 5%% accent cap)
# ---------------------------------------------------------------------------


def build_roxie():
    ch = ChassisSpec()          # helmet PLAIN (canonical: no crown ornament)
    parts = build_chassis(ch)
    parts.extend(team_layout(ch, CHARACTERS["Roxie"]["visor_w"]))
    ops = []

    # --- signature: launch pod over the RIGHT pad (right = -X), muzzle
    # up-back.  Sheet reads Ø 20 x 36 L at -15 deg; that trio cannot satisfy
    # A1's "underside >= 6 uu clear of the pad top (Z 150)" AND SS4.8's
    # Z 176 ceiling at the same time (the arithmetic is in the report), so
    # the tube is Ø 14 x 32 L at -10 deg -- the shoulder asymmetry, which is
    # what the sheet protects, is untouched.
    #
    # WAVE 5 re-derives the trio and gets the sheet's 36 L back.  The binding
    # pair is A1's "underside >= 6 uu clear of the pad top" (Z >= 156) and
    # T5's <= 12 uu thickness clause above Z 176, which together give the tube
    # a 20 uu Z window; a O/ 14 tube of length L at pitch t occupies
    # L/2*sin(t) + r*cos(t) either side of its centre, so 36 L fits at 9 deg
    # (9.73 either side of Z 166.0) where it did not fit at 10.  Diameter is
    # the one number that stays deviated: at O/ 20 the window is 10.25 uu of
    # half-extent before any length or pitch at all.
    pod_c = (-26.0, -4.0, 166.0)
    pod_d = (0.0, -math.cos(math.radians(9.0)), math.sin(math.radians(9.0)))
    p_fwd = v_add(pod_c, v_scale(pod_d, -18.0))
    p_muzzle = v_add(pod_c, v_scale(pod_d, 18.0))
    parts.append(part("sig_pod", "spine_05", "suit",
                      limb(p_fwd, p_muzzle, 6, 7.0, 7.0), thickness=14.0))
    parts.append(part("ins_pod_bore", "spine_05", "inset",
                      limb(v_add(p_muzzle, v_scale(pod_d, -4.0)),
                           v_add(p_muzzle, v_scale(pod_d, 0.4)), 6, 5.5, 5.5)))
    parts.append(part("acc_muzzle_ring", "spine_05", "accent",
                      ring_band(v_add(p_muzzle, v_scale(pod_d, -3.0)), pod_d,
                                6.0, band_w=8.0), thickness=8.0))
    ops.append({"op": "RING(muzzle)", "kind": "RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_muzzle_ring"]})

    # --- signature: THE BLAST RAIL (wave 5, new).  W4-CENSUS ranked Roxie
    # second-weakest at 1,500 uu: "a single small disc on the right shoulder;
    # the rest is baseline" (SS7, J-3), and its fix (J-4 item 3) is to raise
    # the pod read INTO the crown band "where the eye is already looking",
    # exactly as Oyster's jars were raised in wave 2 for the same reason.
    #
    # The pod ITSELF cannot go there: LANGUAGE SS4.8 caps a crown break at
    # 8 uu THICK and the tube is O/ 14, so a raised pod is illegal by two
    # clauses at once (that arithmetic is why W2-BODIES already had to shrink
    # it from the sheet's O/ 20).  What CAN go there is a plate: a 5 uu blast
    # rail rooted inside the tube and sweeping up-back to Z 196, which is the
    # ROSTER'S ONLY OFF-CENTRE CROWN BREAK -- Rocco's crest, X's masts, Lily's
    # fins and Mace's peak are all on the spine, so an asymmetric break above
    # one shoulder is a shape no other body can make, and it doubles down on
    # SS0.4's protected read (she is the only asymmetric silhouette).
    # A1: untagged, for the same reason sig_pod is untagged -- the sheet's own
    # A1 clause exempts this assembly ("Roxie's pod sits above the swing arc").
    rail_root = (-26.0, -6.0, 168.5)
    rail_tip = (-26.0, -24.0, 194.0)
    rail_chord = (0.0, 0.795, 0.606)     # perpendicular to the rail axis, in YZ
    rail, _r0, _r1 = blade(rail_root, rail_tip, 14.0, 7.0, 5.0,
                           chord_hint=rail_chord)
    parts.append(part("sig_rail", "spine_05", "suit", rail,
                      crown_break=True, thickness=5.0))

    # --- signature: spring boots -- cuffs raised to Z 34, three heel coils
    coil_names = []
    for s, sign in (("l", 1.0), ("r", -1.0)):
        fo, ca = SKEL["foot_" + s], SKEL["calf_" + s]
        parts.append(part("sig_bootcuff_" + s, "calf_" + s, "suit",
                          limb((fo[0], fo[1] + 1.0, 12.0),
                               (fo[0] + (ca[0] - fo[0]) * 0.55,
                                fo[1] + 1.0, 36.0), 6, 10.5, 7.5)))
        for k, cz in enumerate((6.0, 11.0, 16.0)):
            nm = "sig_heelcoil_%s_%d" % (s, k)
            parts.append(part(nm, "foot_" + s, "inset",
                              torus_arc((fo[0], fo[1] - 8.0, cz),
                                        (0.0, 0.0, 1.0), 6.0, 2.5, 300.0,
                                        start_deg=120.0, seg=3, sides=4)))
            coil_names.append(nm)
        # heel-coil highlight: one 16 uu line along the MIDDLE coil's outer
        # face, following the coil silhouette (edge-terminated)
        parts.append(part("acc_heelline_" + s, "foot_" + s, "accent",
                          edge_strip((fo[0] + sign * 2.0, fo[1] - 16.0, 11.0),
                                     (fo[0] + sign * 8.0, fo[1] - 6.0, 11.0),
                                     8.0, (sign, -0.6, 0.0), thick=1.5,
                                     embed=0.0)))
        # knee guards
        parts.append(part("sig_kneeguard_" + s, "calf_" + s, "suit",
                          wedge_solid((fo[0] + (ca[0] - fo[0]) * 0.92,
                                       ca[1] + 4.0, ca[2] - 3.0),
                                      (0.0, 1.0, 0.0), (0.0, 1.0, 0.3),
                                      10.0, 12.0, 6.0)))
    ops.append({"op": "heel-coil highlight x2", "kind": "LINE", "width": 8.0,
                "termination": "edge",
                "parts": ["acc_heelline_l", "acc_heelline_r"]})

    # --- DASH(right forearm, 8, 20, 8): one-sided speed ticks.  UPPER_TICK is
    # the sheet's drop-order knob ("drop the upper forearm dash segment
    # first") for the 5%% cap.
    UPPER_TICK = False    # sheet drop-order applied: with both segments the
    #                       measured accent reads 5.56% > her 5% cap
    #                       (SS9: "drop the upper forearm dash segment first")
    dash_parts = []
    if UPPER_TICK:
        parts.append(part("acc_tick_hi", "upperarm_r", "accent",
                          limb_line("upperarm_r", "lowerarm_r", 0.22, 0.82,
                                    (-1.0, 0.0, 0.0), 5.0, width=8.0,
                                    thick=3.0, embed=1.5)))
        dash_parts.append("acc_tick_hi")
    parts.append(part("acc_tick_lo", "lowerarm_r", "accent",
                      limb_line("lowerarm_r", "hand_r", 0.14, 0.72,
                                (-1.0, 0.0, 0.0), 4.8, width=8.0, thick=3.0,
                                embed=1.5)))
    dash_parts.append("acc_tick_lo")
    lar, har = SKEL["lowerarm_r"], SKEL["hand_r"]
    d_r = v_norm(v_sub(har, lar))
    out_r = _perp(d_r, (-1.0, 0.0, 0.2))
    parts.append(part("acc_node_wrist_r", "lowerarm_r", "accent",
                      node_pad(v_add(lerp(lar, har, 0.90),
                                     v_scale(out_r, 5.5)), out_r)))
    dash_parts.append("acc_node_wrist_r")
    ops.append({"op": "DASH(right forearm, 8, 20, 8)", "kind": "DASH",
                "width": 8.0, "termination": "node", "parts": dash_parts})

    parts.append(ankle_ring_part("acc_service_ring", "r", 8.8, t=0.90))
    ops.append({"op": "RING(ankle_r)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})
    # NODE moved from the pod's forward cap to the RAIL TIP (wave 5).  Not an
    # addition -- a relocation, because her 5% accent cap has 0.23% of head
    # room and the sheet's own drop-order (UPPER_TICK) is already spent.  The
    # forward cap pointed down and away from the camera at every third-person
    # pose; the rail tip is the highest point on her silhouette and sits in
    # the crown band the crown break was built for.
    rail_axis = v_norm(v_sub(rail_tip, rail_root))
    parts.append(part("acc_node_railtip", "spine_05", "accent",
                      node_pad(v_add(rail_tip, v_scale(rail_axis, -1.25)),
                               rail_axis, embed=1.75),
                      crown_break=True, thickness=2.5))
    ops.append({"op": "NODE(rail tip)", "kind": "NODE", "width": 8.0,
                "termination": "closed", "parts": ["acc_node_railtip"]})

    # --- panel insets (10): pod strap plates (2), three per boot cuff (6),
    # two pelvis plates
    for k, t in enumerate((-9.0, 5.0)):
        parts.append(part("ins_podstrap_%d" % k, "spine_05", "inset",
                          box(3.0, 6.0, 16.0)
                          .translate(-26.0 + t, -4.0, 158.0)))
    for s, sign in (("l", 1.0), ("r", -1.0)):
        fo = SKEL["foot_" + s]
        for k, (dx, dy) in enumerate(((0.0, 8.2), (7.0, 2.0), (-7.0, 2.0))):
            parts.append(part("ins_bootcuff_%s_%d" % (s, k), "calf_" + s,
                              "inset",
                              box(4.0, 4.0, 9.0)
                              .translate(fo[0] + dx, fo[1] + dy, 26.0)))
        parts.append(part("ins_pelvis_" + s, "pelvis", "inset",
                          box(9.0, 2.4, 6.0).translate(sign * 8.0, 11.3,
                                                       98.0)))

    # "launch_pod" names the whole ASSEMBLY, rail included, because that is
    # what the read is: the rail is rooted inside the tube and covers its top
    # in every view, so measuring the tube with the rail left standing
    # measures the shadow of one part behind another, not a feature.
    # "blast_rail" then measures the crown break on its own.
    features = {
        "launch_pod": ["sig_pod", "ins_pod_bore", "acc_muzzle_ring",
                       "sig_rail", "acc_node_railtip"],
        "blast_rail": ["sig_rail", "acc_node_railtip"],
        "spring_boots": coil_names + ["sig_bootcuff_l", "sig_bootcuff_r"],
    }
    return {"name": "Roxie", "parts": parts, "ops": ops, "features": features}


# ---------------------------------------------------------------------------
# ELLE -- the ghost, orchid #FAADFF (CHARACTER_SHEETS SS10)
# ---------------------------------------------------------------------------


def build_elle():
    ch = ChassisSpec(head_w=18.0, head_d=20.7)
    parts = build_chassis(ch, head_builder=head_teardrop)
    ops = []

    # --- team deviation (canonical): NO VISOR.  Her near-field team read is
    # the 8 uu band at the mask base rim (LANGUAGE SS2.4); back bar standard.
    parts.extend(team_layout(ch, CHARACTERS["Elle"]["visor_w"]))
    parts.append(part("team_maskrim", "head", "team_glow",
                      ring_band((0.0, 0.6, 153.5), (0.0, 0.0, 1.0), 9.8,
                                band_w=8.0)))

    # --- signature: half-cape.  Hard-edged stealth panel, never animates;
    # 6 uu air gap off the body throughout (rules A2/A3).
    # The plate is canted 30 deg off the sagittal plane (the sheet's "canted
    # 10 deg" read as a lean about the long axis, which leaves the panel
    # EDGE-ON in the front/rear masks -- 4 uu, one pixel).  Rolled 30 deg its
    # chord projects 13 uu, so the asymmetric half-cape actually breaks the
    # shoulder line, which is the whole point of it (LANGUAGE SS5).  Inner
    # edge holds the >= 6 uu air gap off the torso and thigh cone (A2/A3).
    cape_root, cape_tip = (30.0, 2.0, 148.0), (28.0, 4.0, 88.0)
    cape_axis = v_norm(v_sub(cape_tip, cape_root))
    cape_cd = _perp(cape_axis, (-0.5, 0.866, 0.0))
    cape_td = v_norm(v_cross(cape_axis, cape_cd))
    cape, _le0, _le1 = blade(cape_root, cape_tip, 26.0, 18.0, 4.0,
                             chord_hint=cape_cd)
    parts.append(part("sig_cape", "spine_05", "suit", cape, thickness=4.0))

    # --- signature: gate arc.  240-degree tube flat on the back, opening up,
    # straddling the back bar; the whole tube is slot 1 (G2 -- the crossing
    # reads as dark frame over team light, never accent-on-team).
    arc_c = (0.0, -16.0, 133.0)
    parts.append(part("sig_gatearc", "spine_05", "inset",
                      torus_arc(arc_c, (0.0, 1.0, 0.0), 17.0, 3.0, 240.0,
                                start_deg=60.0, seg=8, sides=6),
                      gear_region="back"))

    def arc_pt(deg, rad):
        th = math.radians(deg)
        return (rad * math.sin(th), -16.0, 133.0 + rad * math.cos(th))

    # --- accent program: everything interrupted (the cloak made topology)
    cape_dashes = []
    aft_root = v_add(cape_root, v_scale(cape_cd, -13.0))
    aft_tip = v_add(cape_tip, v_scale(cape_cd, -9.0))
    for k, (t0, t1) in enumerate(((0.04, 0.20), (0.40, 0.56), (0.76, 0.92))):
        nm = "acc_capedash_%d" % k
        parts.append(part(nm, "spine_05", "accent",
                          edge_strip(lerp(aft_root, aft_tip, t0),
                                     lerp(aft_root, aft_tip, t1), 8.0,
                                     cape_td, thick=1.5, embed=0.0)))
        cape_dashes.append(nm)
    parts.append(part("acc_node_capetip", "spine_05", "accent",
                      node_pad(v_add(aft_tip, v_scale(cape_cd, -1.5)),
                               cape_td)))
    cape_dashes.append("acc_node_capetip")
    ops.append({"op": "DASH(cape edge contour, 8, 10, 16)", "kind": "DASH",
                "width": 8.0, "termination": "node", "parts": cape_dashes})

    arc_dashes = []
    for k, (d0, d1) in enumerate(((68.0, 86.0), (96.0, 114.0),
                                  (246.0, 264.0), (274.0, 292.0))):
        nm = "acc_arcdash_%d" % k
        p0, p1 = arc_pt(d0, 20.4), arc_pt(d1, 20.4)
        parts.append(part(nm, "spine_05", "accent",
                          edge_strip(p0, p1, 8.0, (0.0, -1.0, 0.0),
                                     thick=1.5, embed=0.0),
                          gear_region="back"))
        arc_dashes.append(nm)
    for s, deg in (("l", 60.0), ("r", 300.0)):
        nm = "acc_node_arcend_" + s
        parts.append(part(nm, "spine_05", "accent",
                          node_pad(arc_pt(deg, 17.0), (0.0, -1.0, 0.0)),
                          gear_region="back"))
        arc_dashes.append(nm)
    ops.append({"op": "DASH(arc inner edge, 8, 10, 16)", "kind": "DASH",
                "width": 8.0, "termination": "node", "parts": arc_dashes})

    parts.append(service_ring_part("acc_service_ring", "l", ch.cuff_r))
    ops.append({"op": "RING(wrist_l)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})

    # --- panel insets (6 -- sleek): two long full-torso side lines, one per
    # thigh, two cape-plate creases
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("ins_sideline_" + s, "spine_04", "inset",
                          edge_strip((sign * 16.8, 2.0, 143.0),
                                     (sign * 13.8, 2.0, 114.0), 4.0,
                                     (sign, 0.0, 0.0), thick=2.0, embed=1.7)))
        parts.append(part("ins_thighline_" + s, "thigh_" + s, "inset",
                          limb_line("thigh_" + s, "calf_" + s, 0.10, 0.90,
                                    (sign, 0.0, 0.0), 6.4)))
    for k, off in enumerate((6.0, -6.0)):
        c0 = v_add(lerp(cape_root, cape_tip, 0.07), v_scale(cape_cd, off))
        c1 = v_add(lerp(cape_root, cape_tip, 0.93), v_scale(cape_cd, off))
        parts.append(part("ins_capecrease_%d" % k, "spine_05", "inset",
                          edge_strip(v_add(c0, v_scale(cape_td, 2.0)),
                                     v_add(c1, v_scale(cape_td, 2.0)), 4.0,
                                     cape_td, thick=2.0, embed=1.7)))

    features = {
        "half_cape": ["sig_cape"] + cape_dashes,
        "gate_arc": ["sig_gatearc"] + arc_dashes,
        "blank_mask": ["head", "team_maskrim"],
    }
    return {"name": "Elle", "parts": parts, "ops": ops, "features": features}


# ---------------------------------------------------------------------------
# SLIMEBALL -- the crawler, slime #9BF66F (CHARACTER_SHEETS SS11)
# ---------------------------------------------------------------------------


def build_slimeball():
    # Chest depth 28 biased forward (the front-heavy crawler).  Crown law:
    # the sheet's dome crown 168 cannot coexist with the 176 +/- 0.5 height
    # law, so the dome rises to Z 160-176 and the LOW-crown read is carried
    # instead by its 16 H squat profile (everyone else's head core is 24 H)
    # and by the reservoir hump, whose crest is lifted to Z 150 -- still
    # inside rule A3's Z 100-150 band for gear proud > 12 uu -- so the head
    # sits in a notch between hump and shoulders.
    #
    # *** WAVE 5: BOTTOM-HEAVY, WHICH IS WHAT "CRAWLER" MEANS.  *** After the
    # other four mass passes he and Rocco were the tightest remaining T1 pair
    # (0.865) for the plain reason that they were the same chassis with
    # different luggage.  His sheet's read is a low, hunched, front-heavy
    # animal, so the mass goes DOWNWARD and the shoulders come IN: pelvis
    # 34 x 26, thighs O/ 17 at the hip, calves O/ 14.4, 19 W boots with a
    # 25 uu toe, against pads narrowed to 15 W at outer +/-37 and upper arms
    # only a little over baseline.  Mortimer and Chut are heavy in the
    # SHOULDERS; he is heavy in the HIPS, and at 46 px those are opposite
    # shapes rather than two sizes of the same one.
    ch = ChassisSpec(chest_d=28.0, chest_y_bias=2.0, head_w=26.0,
                     head_d=26.0, head_z0=160.0, head_z1=176.0,
                     pelvis_w=37.0, pelvis_d=27.0,
                     chest_w_waist=28.0, chest_w_top=34.0,
                     pad_w=13.0, pad_d=19.0, pad_h=8.0, pad_outer=34.0,
                     upperarm_r0=6.2, upperarm_r1=5.2,
                     forearm_r0=5.6, forearm_r1=4.8, cuff_r=6.4,
                     mitt=(13.0, 15.0, 9.0),
                     thigh_r0=9.0, thigh_r1=7.6, calf_r0=7.6, calf_r1=6.0,
                     boot_w=20.0, boot_toe=26.0, boot_heel=-13.0,
                     core_r_major=8.5, core_r_minor=6.5)
    parts = build_chassis(ch, head_builder=head_dome)
    ops = []

    # --- signature: reservoir hump astride the upper back
    hump = lathe([(28.0, 0.0), (26.4, 5.0), (20.2, 11.0), (0.0, 16.0)], 8,
                 smooth=True).scale(1.0, 0.786, 1.0) \
        .rotate((1.0, 0.0, 0.0), 90.0).translate(0.0, -12.0, 126.0)
    parts.append(part("sig_hump", "spine_05", "suit", hump,
                      gear_region="back"))
    parts.append(part("sig_neckcolumn", "neck_02", "inset",
                      prism(6, 7.5, 13.0).translate(0.0, 1.0, 155.5)))

    # --- team deviations (canonical): the torso bar section conforms to the
    # hump facets, planar per facet; the helmet-back section clips to the low
    # crown and narrows to the Ø 26 dome.
    torso_back = [
        part("team_backbar_torso_lo", "spine_04", "team_glow",
             box(24.0, 6.0, 8.0).translate(0, -20.5, 113.0)),
        part("team_backbar_torso_crest", "spine_05", "team_glow",
             box(24.0, 6.0, 20.0).translate(0, -26.5, 126.0)),
        part("team_backbar_torso_f2", "spine_05", "team_glow",
             box(24.0, 6.0, 10.0).translate(0, -20.0, 141.0)),
    ]
    helm_back = [part("team_backbar_helm", "head", "team_glow",
                      box(20.0, 7.0, 14.0).translate(0, -10.0, 167.0))]
    parts.extend(team_layout(ch, CHARACTERS["Slimeball"]["visor_w"],
                             torso_back_parts=torso_back,
                             helm_back_parts=helm_back, chest_bar_dy=4.0,
                             visor_center=(0.0, 10.5, 166.0)))

    # --- signature: claws (3 per mitt leading edge, 2 per boot toe) and the
    # wall-stick grip pads
    claw_bodies, claw_tips = [], []
    for s, sign in (("l", 1.0), ("r", -1.0)):
        la, ha = SKEL["lowerarm_" + s], SKEL["hand_" + s]
        d = v_norm(v_sub(ha, la))
        side = v_norm(v_cross(d, (0.0, 0.0, 1.0)))
        out = _perp(d, (sign, 0.0, 0.0))
        base = v_add(ha, v_scale(d, 11.0))
        for k, off in enumerate((-6.0, 0.0, 6.0)):
            c = v_add(base, v_scale(side, off))
            parts.append(part("sig_claw_%s_%d" % (s, k), "hand_" + s, "inset",
                              wedge_solid(c, d, out, 14.0, 6.5, 6.5),
                              thickness=6.5))
            parts.append(part("acc_clawtip_%s_%d" % (s, k), "hand_" + s,
                              "accent",
                              wedge_solid(v_add(c, v_scale(d, 8.8)), d, out,
                                          5.6, 4.6, 4.6), thickness=4.6))
            claw_bodies.append("sig_claw_%s_%d" % (s, k))
            claw_tips.append("acc_clawtip_%s_%d" % (s, k))
        # grip pads (forearm outer faces).  The palm pads the sheet also
        # lists are dropped: the mitt palm faces the body at every pose the
        # third-person camera sees, and the FP glove is the shared pack asset
        # (SS1) -- see the tri-budget note in the report.
        parts.append(part("ins_grippad_" + s, "lowerarm_" + s, "inset",
                          stud_grid(v_add(lerp(la, ha, 0.45),
                                          v_scale(out, 4.6)), out, 2, 3,
                                    stud=3.0, pitch=5.5, depth=2.0,
                                    up_hint=d)))
        fo = SKEL["foot_" + s]
        for k, off in enumerate((-5.5, 5.5)):
            c = (fo[0] + off, fo[1] + 19.0, 3.2)
            parts.append(part("sig_toeclaw_%s_%d" % (s, k), "foot_" + s,
                              "inset",
                              wedge_solid(c, (0.0, 1.0, 0.0),
                                          (0.0, 0.0, 1.0), 15.0, 6.4, 6.4),
                              thickness=6.4))
            parts.append(part("acc_toetip_%s_%d" % (s, k), "foot_" + s,
                              "accent",
                              wedge_solid((c[0], c[1] + 9.4, c[2]),
                                          (0.0, 1.0, 0.0), (0.0, 0.0, 1.0),
                                          6.0, 4.0, 4.0), thickness=4.0))
            claw_bodies.append("sig_toeclaw_%s_%d" % (s, k))
            claw_tips.append("acc_toetip_%s_%d" % (s, k))
    ops.append({"op": "claw tips", "kind": "NODE", "width": 8.0,
                "termination": "closed", "parts": claw_tips})

    # --- LINE(hump apex ridge): fore-aft run on the hump's RIGHT shoulder
    # facet.  The crest centerline belongs to the team bar riding it; this
    # facet is a different host face, so G1's 12 uu coplanar rule is met by
    # construction (SS11).
    parts.append(part("acc_hump_ridge", "spine_05", "accent",
                      edge_strip((-19.5, -10.0, 140.0), (-9.5, -27.0, 130.0),
                                 8.0, (-0.55, -0.3, 0.78), thick=1.5,
                                 embed=0.0), gear_region="back"))
    ops.append({"op": "LINE(hump apex ridge)", "kind": "LINE", "width": 8.0,
                "termination": "edge", "parts": ["acc_hump_ridge"]})

    # --- PATCH: hard-edged 5-gon splats + a drip DASH beading into a NODE.
    # SHIN_PATCHES is the sheet's drop-order knob ("drop the shin patches
    # first if over budget" -- canonical, and load-bearing here).
    SHIN_PATCHES = True
    pent = [(0.0, 6.5), (6.2, 2.0), (3.8, -5.3), (-3.8, -5.3), (-6.2, 2.0)]
    patch_parts, drip_parts = [], []
    for s, sign in (("l", 1.0), ("r", -1.0)):
        la, ha = SKEL["lowerarm_" + s], SKEL["hand_" + s]
        d = v_norm(v_sub(ha, la))
        out = _perp(d, (sign, 0.0, 0.0))
        c = v_add(lerp(la, ha, 0.72), v_scale(out, 4.4))
        parts.append(part("acc_patch_arm_" + s, "lowerarm_" + s, "accent",
                          face_shape(c, out, pent, 2.5, embed=1.0,
                                     up_hint=d)))
        patch_parts.append("acc_patch_arm_" + s)
        drip0 = v_add(c, v_scale(d, 5.6))
        parts.append(part("acc_drip_arm_" + s, "lowerarm_" + s, "accent",
                          edge_strip(drip0, v_add(drip0, v_scale(d, 9.0)),
                                     8.0, out, thick=2.0, embed=0.6)))
        parts.append(part("acc_node_drip_arm_" + s, "lowerarm_" + s, "accent",
                          node_pad(v_add(drip0, v_scale(d, 11.5)), out)))
        drip_parts += ["acc_drip_arm_" + s, "acc_node_drip_arm_" + s]
        if SHIN_PATCHES:
            parts.append(part("acc_patch_shin_" + s, "calf_" + s, "accent",
                              face_shape(v_add(lerp(SKEL["calf_" + s],
                                                    SKEL["foot_" + s], 0.34),
                                               (0.0, 5.2, 0.0)),
                                         (0.0, 1.0, 0.0), pent, 2.5,
                                         embed=1.0)))
            patch_parts.append("acc_patch_shin_" + s)
            parts.append(part("acc_drip_shin_" + s, "calf_" + s, "accent",
                              limb_line("calf_" + s, "foot_" + s, 0.52, 0.74,
                                        (0.0, 1.0, 0.0), 4.8, width=8.0,
                                        thick=2.0, embed=0.6)))
            parts.append(part("acc_node_drip_shin_" + s, "calf_" + s,
                              "accent",
                              node_pad(v_add(lerp(SKEL["calf_" + s],
                                                  SKEL["foot_" + s], 0.80),
                                             (0.0, 4.8, 0.0)),
                                       (0.0, 1.0, 0.0))))
            drip_parts += ["acc_drip_shin_" + s, "acc_node_drip_shin_" + s]
    ops.append({"op": "PATCH(forearm outer x2, shin x2, 12 across)",
                "kind": "PATCH", "width": 8.0, "termination": "closed",
                "parts": patch_parts})
    ops.append({"op": "DASH(below each patch, 8, 12, 8)", "kind": "DASH",
                "width": 8.0, "termination": "node", "parts": drip_parts})

    parts.append(ankle_ring_part("acc_service_ring", "l", 6.0, t=0.62))
    ops.append({"op": "RING(ankle_l)", "kind": "SERVICE_RING", "width": 8.0,
                "termination": "closed", "parts": ["acc_service_ring"]})

    # --- panel insets (12 -- industrial): hump facet seams (4), grip-pad
    # frames (2 -- see the palm note), two belt plates, four boot plates
    for k, (sx, sz) in enumerate(((22.0, 136.0), (-22.0, 136.0),
                                  (22.0, 116.0), (-22.0, 116.0))):
        parts.append(part("ins_humpseam_%d" % k, "spine_05", "inset",
                          box(4.0, 3.0, 12.0).translate(sx, -21.0, sz)))
    for s, sign in (("l", 1.0), ("r", -1.0)):
        la, ha = SKEL["lowerarm_" + s], SKEL["hand_" + s]
        d = v_norm(v_sub(ha, la))
        out = _perp(d, (sign, 0.0, 0.0))
        parts.append(part("ins_gripframe_" + s, "lowerarm_" + s, "inset",
                          frame_box(v_add(lerp(la, ha, 0.45),
                                          v_scale(out, 4.2)), d, out,
                                    18.0, 11.0, 2.0)))
        parts.append(part("ins_belt_" + s, "pelvis", "inset",
                          box(11.0, 2.4, 5.0).translate(sign * 7.0, 11.3,
                                                        104.0)))
        fo = SKEL["foot_" + s]
        parts.append(part("ins_boot_a_" + s, "foot_" + s, "inset",
                          box(10.0, 6.0, 2.4).translate(fo[0], fo[1] + 4.0,
                                                        11.3)))
        parts.append(part("ins_boot_b_" + s, "foot_" + s, "inset",
                          box(3.0, 2.4, 8.0).translate(fo[0] + sign * 6.0,
                                                       fo[1] + 12.0, 6.0)))

    features = {
        "hump": ["sig_hump", "acc_hump_ridge"],
        "low_dome": ["head"],
        "claws": claw_bodies + claw_tips,
    }
    return {"name": "Slimeball", "parts": parts, "ops": ops,
            "features": features}


# ---------------------------------------------------------------------------
# MORTIMER -- the siege engine, patinated steel #5DB5A2 (CHARACTER_SHEETS SS12; 5%% cap)
# ---------------------------------------------------------------------------


def build_mortimer():
    # Pads out at +/-44 (the single canonical envelope exception, LANGUAGE
    # SS9.4); pad height 12 rather than 14 so SS4.8's "beyond +/-36 => <= 12
    # uu thick" clause still holds (T5 reads the declaration).  Zero neck.
    # Crown law: block 20 H at Z 152-172 becomes 24 H at Z 152-176, which
    # deepens the canonical read rather than fighting it -- no neck gap at
    # all between the chest top and the helmet block.
    #
    # *** WAVE 5: HE IS THE HEAVIEST BODY AND UNTIL NOW ONLY HIS PADS SAID SO.
    # *** CHARACTER_LANGUAGE SS5's differentiation matrix cites exactly two
    # MASSES on the whole roster -- Mortimer "widest +/-44" and Lily
    # "narrowest +/-26" -- and W4-CENSUS measured the consequence in the
    # arena: nine of ten bodies shared ONE torso block and ONE limb gauge, so
    # his "widest" was a pad-only cue that needs another character standing
    # beside him to work at all (SS7, J-3: "his one differentiator is a
    # COMPARATIVE cue").  The torso, pelvis and every limb gauge now carry it
    # too: chest 32 -> 42 W over a 26-deep block, pelvis 36 x 26, thighs
    # O/ 18 at the hip, calves O/ 15, upper arms O/ 15.  His silhouette is now
    # heavy from the knees up rather than heavy at the shoulders only.
    ch = ChassisSpec(pelvis_w=36.0, pelvis_d=26.0,
                     chest_w_waist=32.0, chest_w_top=42.0, chest_d=26.0,
                     pad_w=32.0, pad_d=24.0, pad_h=12.0, pad_outer=44.0,
                     pad_top=154.0, neck=False,
                     upperarm_r0=7.5, upperarm_r1=6.5, forearm_r0=7.0,
                     forearm_r1=6.5, cuff_r=7.5, mitt=(14.0, 16.0, 9.0),
                     thigh_r0=9.0, thigh_r1=7.5, calf_r0=7.5, calf_r1=6.0,
                     core_r_major=9.0, core_r_minor=7.0,
                     head_w=24.0, head_d=20.0,
                     head_z0=152.0, head_z1=176.0)
    parts = build_chassis(ch, head_builder=head_flattop)
    parts = drop_parts(parts, ["boot_l", "boot_r"])
    ops = []
    parts.append(part("sig_noneck", "spine_05", "suit",
                      box(26.0, 20.0, 10.0, chamfer=2.0)
                      .translate(0.0, 1.0, 148.0)))

    # --- signature: THE LINTEL (wave 5, new).  He was the only character on
    # the roster with NOTHING above the shoulder line -- W4-CENSUS SS7 J-4
    # ranks giving him a crown break "the single highest-value change on the
    # roster", because at 1,500 uu the eye is already looking at the crown
    # band and his half of it was empty.  Every other crown break on the
    # roster is VERTICAL (Rocco's crest, X's three masts, Lily's twin fins,
    # Mace's hood peak); his is a HORIZONTAL capstone carried on two short
    # posts, which is a shape nobody else has and which reads as architecture
    # -- the "no neck at all" block gains a lintel, it does not gain a head.
    # LANGUAGE SS4.8: crown-break parts are <= Z 208 and declare <= 8 uu
    # thickness (the posts are 8 x 8, the slab is 8 deep); physics-excluded by
    # the SS4.8/T6 table, which bodies fifteen named bones and nothing else.
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("sig_lintel_post_" + s, "head", "suit_head",
                          box(8.0, 8.0, 8.0, chamfer=1.5)
                          .translate(sign * 13.0, -1.0, 178.0),
                          crown_break=True, thickness=8.0))
    parts.append(part("sig_lintel", "head", "suit_head",
                      box(36.0, 8.0, 10.0, chamfer=2.0)
                      .translate(0.0, -1.0, 187.0),
                      crown_break=True, thickness=8.0))

    # --- signature: three stacked chest slabs (chest twin bars ride the
    # middle slab -- canonical).  WAVE 5: widened 40/44/40 -> 46/50/46 and
    # deepened 16 -> 20 because the chest block underneath them grew.  The
    # slabs are a STRATA read: they only exist in the silhouette as the amount
    # they stand PROUD of the torso, so a wider torso with the old slab dims
    # simply swallows them (T1 measured the feature at 2 px2 the first time
    # this chassis was built).  Depth 20 keeps hi.y at 20 <= A1's 22 uu
    # front-gear plane; the twin bars move out with them (chest_bar_dy 8.5).
    for k, (w, z0, z1) in enumerate(((52.0, 108.0, 120.0),
                                     (56.0, 124.0, 134.0),
                                     (52.0, 138.0, 146.0))):
        parts.append(part("sig_slab_%d" % k, "spine_04", "suit",
                          box(w, 20.0, z1 - z0, chamfer=2.0)
                          .translate(0.0, 10.0, (z0 + z1) / 2.0),
                          gear_region="front"))
        parts.append(part("ins_slab_under_%d" % k, "spine_04", "inset",
                          box(w - 8.0, 3.0, 2.4)
                          .translate(0.0, 18.6, z0 - 1.0)))

    # --- signature: armor skirt (four trapezoid plates, <= 6 proud, rule A3)
    # WAVE 5: the plate ring moved OUT with the pelvis (36 x 26 now, was the
    # baseline 30 x 22) -- at the old radii the skirt sat inside the hip block
    # and stopped existing in the silhouette.  Still <= 6 uu proud of the
    # pelvis face, which is the A3 hip-line clause it has to satisfy.
    skirt = ((0.0, 15.0, 96.0, 0.0, 16.0, 74.0, (1.0, 0.0, 0.0), "front"),
             (0.0, -15.0, 96.0, 0.0, -16.0, 74.0, (1.0, 0.0, 0.0), "back"),
             (18.0, 0.0, 96.0, 20.0, 0.0, 74.0, (0.0, 1.0, 0.0), "side_l"),
             (-18.0, 0.0, 96.0, -20.0, 0.0, 74.0, (0.0, 1.0, 0.0), "side_r"))
    for (rx, ry, rz, tx, ty, tz, hint, tag) in skirt:
        m, _a, _b = blade((rx, ry, rz), (tx, ty, tz), 22.0, 17.0, 6.0,
                          chord_hint=hint)
        parts.append(part("sig_skirt_" + tag, "pelvis", "suit", m,
                          thickness=6.0))
        parts.append(part("ins_skirtframe_" + tag, "pelvis", "inset",
                          frame_box(((rx + tx) / 2.0 * 1.18,
                                     (ry + ty) / 2.0 * 1.18, 85.0),
                                    (0.0, 0.0, 1.0),
                                    (rx + tx, ry + ty, 0.0) if (rx or ry)
                                    else (0.0, 1.0, 0.0), 18.0, 4.0, 2.0)))

    # --- signature: anvil boots (replace the boot wedges)
    for s in ("l", "r"):
        fo = SKEL["foot_" + s]
        parts.append(part("sig_anvilboot_" + s, "foot_" + s, "suit",
                          box(20.0, 38.0, 14.0, chamfer=3.0)
                          .translate(fo[0], fo[1] + 5.0, 7.0)))
        for k, dy in enumerate((-8.0, 16.0)):
            parts.append(part("ins_boot_%s_%d" % (s, k), "foot_" + s, "inset",
                              box(14.0, 4.0, 2.4).translate(fo[0],
                                                            fo[1] + dy, 13.3)))

    # --- signature: PanelDark harness straps, shoulder->pelvis diagonals,
    # routed on the chest's side facets (G1: a different host face from the
    # twin bars, which live on the slab fronts)
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("sig_harness_" + s, "spine_04", "inset",
                          edge_strip((sign * 20.0, 2.0, 150.0),
                                     (sign * 15.0, 4.0, 98.0), 6.0,
                                     (sign, 0.0, 0.0), thick=2.5, embed=1.0)))
        parts.append(part("ins_padframe_" + s, "clavicle_" + s, "inset",
                          box(24.0, 3.0, 2.4).translate(sign * 29.0, 0.0,
                                                        153.3)))

    # --- team deviations (canonical): back bar widens to 28; chest bars ride
    # the middle slab; helmet-back section on the block's rear face.
    helm_back = [part("team_backbar_helm", "head", "team_glow",
                      box(24.0, 8.0, 24.0).translate(0, -8.5, 164.0))]
    parts.extend(team_layout(ch, CHARACTERS["Mortimer"]["visor_w"],
                             helm_back_parts=helm_back, back_bar_w=28.0,
                             chest_bar_dy=8.5,
                             visor_center=(0.0, 8.5, 162.0)))

    # --- accent program: STRATA only.  Quiet by design; 5%% cap.
    strata = []
    for s, sign in (("l", 1.0), ("r", -1.0)):
        for k, sz in enumerate((126.5, 132.0)):
            nm = "acc_strata_slab_%s_%d" % (s, k)
            parts.append(part(nm, "spine_04", "accent",
                              edge_strip((sign * 28.0, 0.5, sz),
                                         (sign * 28.0, 19.5, sz), 8.0,
                                         (sign, 0.0, 0.0), thick=1.5,
                                         embed=0.0)))
            strata.append(nm)
    ops.append({"op": "STRATA(chest slab side edges, 2, 8)", "kind": "STRATA",
                "width": 8.0, "termination": "edge", "parts": strata})
    skirt_strata = []
    for s, sign in (("l", 1.0), ("r", -1.0)):
        nm = "acc_strata_skirt_" + s
        parts.append(part(nm, "pelvis", "accent",
                          edge_strip((sign * 21.4, -8.0, 86.0),
                                     (sign * 21.4, 8.0, 86.0), 8.0,
                                     (sign, 0.0, 0.0), thick=1.5, embed=0.0)))
        skirt_strata.append(nm)
    ops.append({"op": "STRATA(skirt plate edges, 2, 8)", "kind": "STRATA",
                "width": 8.0, "termination": "edge", "parts": skirt_strata})

    parts.append(service_ring_part("acc_service_ring", "r", ch.cuff_r))
    ops.append({"op": "RING(forearm cuff_r)", "kind": "SERVICE_RING",
                "width": 8.0, "termination": "closed",
                "parts": ["acc_service_ring"]})
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("acc_node_rivet_" + s, "pelvis", "accent",
                          node_pad((sign * 5.5, 17.4, 84.0),
                                   (0.0, 1.0, 0.0))))
    ops.append({"op": "NODE(front skirt rivets) x2", "kind": "NODE",
                "width": 8.0, "termination": "closed",
                "parts": ["acc_node_rivet_l", "acc_node_rivet_r"]})
    # NODE(lintel ends) x2 (wave 5) -- the crown break needs to be findable at
    # range, and his accent programme is STRATA + NODE, "quiet by design".  A
    # LINE along the lintel would be the loudest thing on a body whose whole
    # read is silence; two 8 x 8 pads on the capstone's END faces put two lit
    # points at the extreme corners of the crown band instead, which is where
    # the eye is already looking at 1,500 uu, and cost 128 uu2 against 0.9%
    # of unused cap.
    for s, sign in (("l", 1.0), ("r", -1.0)):
        parts.append(part("acc_node_lintel_" + s, "head", "accent",
                          node_pad((sign * 18.5, -1.0, 187.0),
                                   (sign, 0.0, 0.0)),
                          crown_break=True, thickness=2.5))
    ops.append({"op": "NODE(lintel ends) x2", "kind": "NODE", "width": 8.0,
                "termination": "closed",
                "parts": ["acc_node_lintel_l", "acc_node_lintel_r"]})

    features = {
        "wide_pads": ["pad_l", "pad_r", "ins_padframe_l", "ins_padframe_r"],
        "slab_chest": ["sig_slab_0", "sig_slab_1", "sig_slab_2"],
        "skirt": ["sig_skirt_front", "sig_skirt_back", "sig_skirt_side_l",
                  "sig_skirt_side_r"],
        "anvil_boots": ["sig_anvilboot_l", "sig_anvilboot_r"],
        "lintel": ["sig_lintel", "sig_lintel_post_l", "sig_lintel_post_r",
                   "acc_node_lintel_l", "acc_node_lintel_r"],
    }
    return {"name": "Mortimer", "parts": parts, "ops": ops,
            "features": features}


# ---------------------------------------------------------------------------
# registry -- wave 2's eight-body agent adds build_<name> functions here and
# nothing else changes: generate_characters.py and verify_silhouettes.py
# iterate this dict.
# ---------------------------------------------------------------------------

BODY_BUILDERS = {
    "Rocco": build_rocco,
    "Chut": build_chut,
    "Mace": build_mace,
    "Oyster": build_oyster,
    "X": build_x,
    "Roxie": build_roxie,
    "Elle": build_elle,
    "Slimeball": build_slimeball,
    "Mortimer": build_mortimer,
    "Lily": build_lily,
}


def build_body(name):
    """Build one body; returns {"name", "parts", "ops", "features"}."""
    if name not in BODY_BUILDERS:
        raise KeyError("no builder for %r (have: %s)"
                       % (name, ", ".join(sorted(BODY_BUILDERS))))
    return BODY_BUILDERS[name]()


def feed_writer(writer, body):
    """Declare skeleton + materials + parts on a TraceGlbWriter."""
    body_materials(writer, body["name"])
    for (bname, parent, head) in CANONICAL_SKELETON:
        writer.add_bone(bname, parent, head)
    for p in body["parts"]:
        writer.add_part(p["bone"], p["mesh"], p["slot"], name=p["name"],
                        crown_break=p["crown_break"])
