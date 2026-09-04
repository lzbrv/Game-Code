#!/usr/bin/env python3
# =============================================================================
# Trace - generate_side_ramp.py
#
# SWEEPS THE OWNER'S CONCAVE PROFILE ALONG BOTH SIDELINES AND WRITES TWO .OBJ
# FILES. Stock Python 3, no Unreal, no dependencies - run it from anywhere.
#
# -----------------------------------------------------------------------------
# WHY THIS EXISTS INSTEAD OF SCALING THE OWNER'S MESH
# -----------------------------------------------------------------------------
# The owner supplied `tron-concave-ramp.obj` - a 2.60 x 4.20 x 1.70 unit ramp
# with base plinth, side panels, five back ribs, two edge lights, four lane
# lights, a takeoff lip light and an entry threshold - and said:
#
#     "use that to replace the side ramps (you can stretch out/remodel to match
#      what's needed, but that concavity is what is crucial)."
#
# The sideline run is 38,400 uu. Scaling a 2.6-unit-wide asset by ~15,000x in
# one axis would smear its ribs, lane lights and side panels into unreadable
# streaks. So this does what the owner explicitly allowed instead: it takes the
# PROFILE and re-sweeps it.
#
# -----------------------------------------------------------------------------
# THE PROFILE IS NOT APPROXIMATED. IT IS EXACT.
# -----------------------------------------------------------------------------
# `ramp_shell`'s centreline was read station by station out of the OBJ and
# normalised (u = along-run in [0,1], h = rise in [0,1]). Every one of its 65
# stations satisfies
#
#     h = u^2
#
# to within a maximum deviation of 2.301e-5. The owner's curve IS a parabola,
# and this script sweeps that same parabola at the same 64-span tessellation.
# --verify re-reads the source OBJ and prints the deviation between the shipped
# profile and the owner's, normalised, so the report never has to take it on
# trust. THAT CHECK CAN FAIL: point --owner-obj at a different mesh and it says
# so instead of quietly passing.
#
# -----------------------------------------------------------------------------
# WHAT MAPS TO WHAT WHEN A RAMP BECOMES 384 METRES LONG
# -----------------------------------------------------------------------------
# The owner's ramp has two families of neon and the sweep sends them to
# different places, which is the whole reason the dressing survives:
#
#   ACROSS the ramp (takeoff_lip_light, entry_threshold, plinth_underglow)
#       -> CONTINUOUS STRIPS running the full length, at the crest and the toe.
#          This is also the arena's own language for a ride surface: its surf
#          rails carry MI_Neon_Surf_Rail_Crest_Line and _Toe_Line.
#
#   UP the ramp (edge_light_l/r, lane_light_1..4)
#       -> A REPEATING MOTIF. One repeat is one owner-ramp-width, so a repeat
#          seen from above is exactly the owner's ramp: two edge lines and four
#          lane lines at their authored fractions of the width.
#
# NOT CARRIED, with reasons:
#   base_plinth       - the owner's deck starts 0.16 (of 1.86) ABOVE its base.
#                       At this scale that is a 57 uu step at the toe, and the
#                       toe is THE ENTRANCE. A ramp you have to climb a kerb to
#                       reach is the exact defect this design exists to remove.
#                       The plinth's underglow survives as the toe line.
#   back_rib_1..5,
#   back_sill_light   - they dress the BACK face, which sits against the side
#                       wall behind the pawn standoff shell. Geometry nobody can
#                       ever see, on a mesh that ships twice.
#   side_panel_l/r    - they dress the ramp's two ENDS, which here are the two
#                       end walls of the arena. Same reason.
#
# -----------------------------------------------------------------------------
# ONE SOURCE OF TRUTH FOR THE SHAPE
# -----------------------------------------------------------------------------
# kDepthUU / kHeightUU / kFacetCount / kLengthUU / kCrestOutFromWallUU are PARSED
# OUT OF Source/Trace/World/TraceSideRampProfile.h, never re-typed here. That
# header is where the build-time asserts live - "the ramp must start walkable",
# "the ramp must become surfable", "the ramp must never become a wall" - so the
# numbers this script sweeps are the numbers the compiler has already checked
# against the surf band. If the header is missing or a constant cannot be found,
# this script FAILS rather than falling back to a default.
#
# -----------------------------------------------------------------------------
# HOW TO RUN IT
# -----------------------------------------------------------------------------
#     python3 Scripts/generate_side_ramp.py --out Art/SideRamp --verify
#
# Writes:
#     SM_SideRampConcave.obj      the ride shell - ONE material, gets collision
#     SM_SideRampConcave.mtl
#     SM_SideRampConcaveTrim.obj  all the neon - TWO materials, NO collision
#     SM_SideRampConcaveTrim.mtl
#
# Two meshes and not one, because they need different collision. The shell is
# imported complex-as-simple (per-triangle) and is what a pawn walks and rides;
# the trim is ~40k triangles of thin proud strips and must never be part of a
# collision query at all.
# =============================================================================
import argparse
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(HERE)
HEADER = os.path.join(PROJECT_ROOT, "Source", "Trace", "World", "TraceSideRampProfile.h")
OWNER_OBJ = os.path.join(PROJECT_ROOT, "Art", "SideRamp", "tron-concave-ramp.obj")


# -----------------------------------------------------------------------------
# The design constants, read out of the C++ header that asserts them.
# -----------------------------------------------------------------------------
def read_design(header_path):
    if not os.path.isfile(header_path):
        raise SystemExit("FATAL: {0} is missing. The design constants live there and are "
                         "deliberately not duplicated here.".format(header_path))
    text = open(header_path, "r", encoding="utf-8").read()
    wanted = {
        "kDepthUU": float, "kHeightUU": float, "kLengthUU": float,
        "kCrestOutFromWallUU": float, "kFacetCount": int, "kProfileExponent": int,
        "kProfileStartFrac": float,
        "kOwnerRun": float, "kOwnerRise": float, "kOwnerWidth": float,
    }
    out = {}
    for name, caster in wanted.items():
        m = re.search(r"inline\s+constexpr\s+\w+\s+" + name + r"\s*=\s*([0-9.eE+-]+)\s*;", text)
        if not m:
            raise SystemExit("FATAL: could not find {0} in {1}. Refusing to guess it."
                             .format(name, header_path))
        out[name] = caster(float(m.group(1)))
    return out


# -----------------------------------------------------------------------------
# THE BUILT FACE IS A SEGMENT OF THE OWNER'S CURVE, NOT THE WHOLE OF IT.
#
# kProfileStartFrac says where on the owner's parabola the built face starts. The
# whole curve runs u = 0..1; this sweeps u = kProfileStartFrac..1 and translates
# it so its toe is on the floor. See the long comment in TraceSideRampProfile.h
# for WHY (the owner asked for a face that is not walkable anywhere, and a
# parabola from u = 0 is walkable at its toe by construction).
#
# One function returns everything the sweep needs, so the shell, the trim and the
# report cannot each hold their own copy of the arithmetic.
# -----------------------------------------------------------------------------
def face_curve(design):
    """(y, z, slope) of the BUILT face at s in [0,1] along it, plus the full curve."""
    D = design["kDepthUU"]
    H = design["kHeightUU"]
    P = int(design["kProfileExponent"])
    u0 = design["kProfileStartFrac"]
    if not (0.0 < u0 < 1.0):
        raise SystemExit("FATAL: kProfileStartFrac {0} is not in (0,1); the built face would not be "
                         "a segment of the owner's curve.".format(u0))
    full_d = D / (1.0 - u0)
    full_h = H / (1.0 - u0 ** P)
    z_unbuilt = full_h * (u0 ** P)

    def u_at(s):
        return u0 + (1.0 - u0) * s

    def y_at(s):
        # full_d * (u - u0) collapses to D * s exactly, because full_d * (1 - u0) == D.
        return D * s

    def z_at(s):
        return full_h * (u_at(s) ** P) - z_unbuilt

    def slope_at(s):
        # dz/dy on the FULL curve at this u. The face's own frame is a translation,
        # so the slope is untouched by the truncation.
        return P * full_h * (u_at(s) ** (P - 1)) / full_d

    return y_at, z_at, slope_at, u_at, full_d, full_h, z_unbuilt


# -----------------------------------------------------------------------------
# The owner's curve, and the check that the shipped one is the same curve.
# -----------------------------------------------------------------------------
def owner_profile(obj_path):
    """The 65-station centreline of `ramp_shell`, normalised to u,h in [0,1]."""
    verts, cur, faces = [], None, []
    with open(obj_path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("v "):
                _, x, y, z = line.split()
                verts.append((float(x), float(y), float(z)))
            elif line.startswith("o "):
                cur = line[2:].strip()
            elif line.startswith("f ") and cur == "ramp_shell":
                faces.append([int(t.split("/")[0]) - 1 for t in line.split()[1:]])
    if not faces:
        raise SystemExit("FATAL: no object named 'ramp_shell' in {0}".format(obj_path))
    used = set(i for f in faces for i in f)
    rows = {}
    for i in used:
        x, y, z = verts[i]
        if abs(z - (-1.30)) < 1e-4:            # one edge of the ruled surface; it is straight in Z
            rows.setdefault(round(x, 4), []).append(y)
    xs = sorted(rows)
    prof = [(x, max(rows[x])) for x in xs]     # max: the lip station also carries the back face
    x0, x1 = prof[0][0], prof[-1][0]
    y0 = min(v for _, v in prof)
    y1 = max(v for _, v in prof)
    return [((x - x0) / (x1 - x0), (y - y0) / (y1 - y0)) for x, y in prof], (x1 - x0), (y1 - y0)


def verify_profile(design, owner_obj):
    owner, run, rise = owner_profile(owner_obj)
    n = int(design["kFacetCount"])
    p = int(design["kProfileExponent"])
    u0 = design["kProfileStartFrac"]

    # ---- (1) IS THE CURVE THIS SCRIPT SWEEPS THE OWNER'S CURVE? -------------
    # Unaffected by the truncation: h = u^p is asserted over the WHOLE of the
    # owner's measured run, so a change to kProfileExponent still fails here.
    worst = 0.0
    worst_u = 0.0
    for u, h in owner:
        shipped = u ** p                       # what this script sweeps, normalised
        if abs(h - shipped) > worst:
            worst, worst_u = abs(h - shipped), u
    print("VERIFY  owner ramp_shell: run {0:.4f} rise {1:.4f}, {2} stations"
          .format(run, rise, len(owner)))
    print("VERIFY  shipped profile : h = u^{0}, {1} spans, face D {2:.1f} uu x H {3:.1f} uu"
          .format(p, n, design["kDepthUU"], design["kHeightUU"]))
    print("VERIFY  MAX NORMALISED DEVIATION shipped-vs-owner: {0:.4e}  (at u = {1:.4f})"
          .format(worst, worst_u))
    # A check that cannot fail proves nothing: 1e-3 normalised is 1.1 uu at this
    # height, i.e. smaller than one facet's rise, and a genuinely different curve
    # (a circular arc through the same endpoints peaks 6e-2 away) fails it by 60x.
    if worst > 1e-3:
        raise SystemExit("FATAL: the shipped profile is NOT the owner's curve "
                         "(deviation {0:.4e} > 1e-3).".format(worst))
    print("VERIFY  PASS - the curve this script sweeps is the owner's curve.")

    # ---- (2) AND IS THE *BUILT SEGMENT* A PIECE OF IT, IN PLACE? ------------
    # The face is a TRUNCATION now, so (1) alone no longer covers the shipped
    # geometry: the sweep could be mapped back onto the curve wrongly and (1)
    # would still pass. So the ACTUAL BUILT HEIGHTS are compared against the
    # OWNER'S MEASURED STATIONS, interpolated and re-normalised over the built
    # segment only:
    #
    #     z_built(s) / kHeightUU   must equal   (h_owner(u) - h_owner(u0))
    #                                           / (h_owner(1) - h_owner(u0))
    #
    # NOTHING ON THE LEFT COMES FROM face_curve's own derived quantities, which is
    # the whole point: an earlier draft of this check divided by full_h and added
    # z_unbuilt back, so the two cancelled and it passed with the truncation term
    # deleted — a check that could not fail. MEASURED after the fix: setting
    # z_unbuilt to 0 reports 5.2e-1, i.e. 516x the tolerance, and the run aborts.
    y_at, z_at, _slope, u_at, full_d, full_h, z_unbuilt = face_curve(design)
    face_h = design["kHeightUU"]
    owner_u = [u for u, _h in owner]
    owner_h = [h for _u, h in owner]

    def owner_at(u):
        if u <= owner_u[0]:
            return owner_h[0]
        for k in range(1, len(owner_u)):
            if u <= owner_u[k]:
                span = owner_u[k] - owner_u[k - 1]
                t = 0.0 if span <= 0.0 else (u - owner_u[k - 1]) / span
                return owner_h[k - 1] + t * (owner_h[k] - owner_h[k - 1])
        return owner_h[-1]

    owner_lo = owner_at(u0)
    owner_span = owner_at(1.0) - owner_lo
    if owner_span <= 0.0:
        raise SystemExit("FATAL: the owner's curve does not rise over the built segment.")
    seg_worst = 0.0
    seg_worst_u = 0.0
    for i in range(n + 1):
        s_frac = i / float(n)
        u = u_at(s_frac)
        h_built = z_at(s_frac) / face_h                       # the geometry, as built
        h_owner = (owner_at(u) - owner_lo) / owner_span       # the owner, over the same segment
        d = abs(h_built - h_owner)
        if d > seg_worst:
            seg_worst, seg_worst_u = d, u
    print("VERIFY  built segment   : u {0:.4f}..1.0000 of a full parabola {1:.1f} x {2:.1f} uu; "
          "its lower {3:.1f} uu of rise is not built".format(u0, full_d, full_h, z_unbuilt))
    print("VERIFY  MAX NORMALISED DEVIATION built-segment-vs-owner: {0:.4e}  (at u = {1:.4f})"
          .format(seg_worst, seg_worst_u))
    if seg_worst > 1e-3:
        raise SystemExit("FATAL: the BUILT FACE does not lie on the owner's curve "
                         "(deviation {0:.4e} > 1e-3).".format(seg_worst))
    print("VERIFY  PASS - the built face is a segment of the owner's curve, in place.")

    # ---- (3) the control, so the tolerance is shown to discriminate ---------
    arc_worst = max(abs(h - (1.0 - math.sqrt(max(0.0, 1.0 - u * u)))) for u, h in owner)
    print("VERIFY  (control) a circular arc through the same endpoints would deviate "
          "{0:.4e} - {1:.0f}x the tolerance. The tolerance discriminates."
          .format(arc_worst, arc_worst / 1e-3))
    return worst


# -----------------------------------------------------------------------------
# A tiny OBJ writer. Positions, normals, UVs; one group per material.
# -----------------------------------------------------------------------------
class Obj(object):
    def __init__(self):
        self.v = []
        self.vn = []
        self.vt = []
        self.groups = []          # (material, [ (vi,ti,ni) x3 ])

    def group(self, material):
        self.groups.append((material, []))
        return self.groups[-1][1]

    def add_v(self, p):
        self.v.append(p)
        return len(self.v)        # OBJ is 1-based

    def add_n(self, n):
        self.vn.append(n)
        return len(self.vn)

    def add_t(self, t):
        self.vt.append(t)
        return len(self.vt)

    def write(self, path, mtl_name, materials):
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("# Trace - generated by Scripts/generate_side_ramp.py. Do not hand-edit.\n")
            fh.write("mtllib {0}\n".format(mtl_name))
            for p in self.v:
                fh.write("v {0:.5f} {1:.5f} {2:.5f}\n".format(*p))
            for t in self.vt:
                fh.write("vt {0:.5f} {1:.5f}\n".format(*t))
            for n in self.vn:
                fh.write("vn {0:.6f} {1:.6f} {2:.6f}\n".format(*n))
            # ONE `o` GROUP FOR THE WHOLE FILE, MATERIALS AS `usemtl` RUNS INSIDE IT.
            #
            # MEASURED, not stylistic. Interchange's OBJ translator makes ONE MESH NODE
            # PER `o` GROUP, so the first draft of this writer - which emitted `o` per
            # material, as the material names are the slot contract - imported the trim
            # as TWO separate StaticMeshes (sideramp_neon_lane1 + sideramp_neon_lip1)
            # and import_side_ramp.py refused it. A `usemtl` run inside a single object
            # is the OBJ format's own way of saying "one mesh, several material slots",
            # and it is what the shell (one group, one material) was already getting by
            # accident. The slot names are unchanged, so SLOT_TO_MIC still binds.
            stem = os.path.splitext(os.path.basename(path))[0]
            fh.write("o {0}\n".format(stem))
            for mat, tris in self.groups:
                if not tris:
                    continue
                fh.write("usemtl {0}\n".format(mat))
                for tri in tris:
                    fh.write("f {0}/{1}/{2} {3}/{4}/{5} {6}/{7}/{8}\n".format(
                        tri[0][0], tri[0][1], tri[0][2],
                        tri[1][0], tri[1][1], tri[1][2],
                        tri[2][0], tri[2][1], tri[2][2]))
        mtl_path = os.path.join(os.path.dirname(path), mtl_name)
        with open(mtl_path, "w", encoding="utf-8") as fh:
            fh.write("# Trace - generated. Slot NAMES are the contract; the arena's own authored\n"
                     "# MICs are bound over these by Scripts/import_side_ramp.py.\n")
            for mat, kd in materials:
                fh.write("newmtl {0}\nKd {1:.3f} {2:.3f} {3:.3f}\nd 1.0\nillum 2\n\n"
                         .format(mat, kd[0], kd[1], kd[2]))


def quad(group, obj, corners, uvs, normals):
    """One quad as two triangles, wound so the normal points along `normals`."""
    idx = [(obj.add_v(c), obj.add_t(t), obj.add_n(n)) for c, t, n in zip(corners, uvs, normals)]
    group.append([idx[0], idx[1], idx[2]])
    group.append([idx[0], idx[2], idx[3]])


# -----------------------------------------------------------------------------
# The sweep.
#
# MESH-LOCAL AXES, and they are chosen so the placement is two numbers:
#     +X  along the sideline (length),  centred on 0
#     +Y  from the toe toward the wall (depth),  toe at Y = 0
#     +Z  up (height),  toe at Z = 0
# The +Y wall gets yaw 0, the -Y wall gets yaw 180. Nothing is mirrored or
# negatively scaled: a negative scale flips triangle winding and UE reports the
# resulting inside-out collision as a surface a pawn falls through.
# -----------------------------------------------------------------------------
def build_shell(design):
    D = design["kDepthUU"]
    H = design["kHeightUU"]
    L = design["kLengthUU"]
    N = int(design["kFacetCount"])

    y_at, z_at, slope_at, _u_at, _fd, _fh, _z0 = face_curve(design)

    obj = Obj()
    deck = obj.group("sideramp_deck")

    x0, x1 = -L * 0.5, L * 0.5

    def prof_y(i):
        return y_at(i / float(N))

    def prof_z(i):
        return z_at(i / float(N))

    def prof_normal(i):
        # ANALYTIC normal of the parabola at station i, not the facet's own.
        # Facet normals would give 64 hard bands down a surface whose entire
        # point is that it reads as a curve; the collision still uses the facet
        # planes (complex-as-simple traces geometry, not vertex normals), so this
        # changes how it LOOKS and not one degree of how it RIDES.
        slope = slope_at(i / float(N))
        inv = 1.0 / math.sqrt(1.0 + slope * slope)
        return (0.0, -slope * inv, inv)

    # ---- the deck: the ride surface -----------------------------------------
    for i in range(N):
        y0, z0 = prof_y(i), prof_z(i)
        y1, z1 = prof_y(i + 1), prof_z(i + 1)
        n0, n1 = prof_normal(i), prof_normal(i + 1)
        quad(deck, obj,
             [(x0, y0, z0), (x1, y0, z0), (x1, y1, z1), (x0, y1, z1)],
             [(x0 / 100.0, y0 / 100.0), (x1 / 100.0, y0 / 100.0),
              (x1 / 100.0, y1 / 100.0), (x0 / 100.0, y1 / 100.0)],
             [n0, n0, n1, n1])

    # ---- the underside, flat on the floor -----------------------------------
    quad(deck, obj,
         [(x0, D, 0.0), (x1, D, 0.0), (x1, 0.0, 0.0), (x0, 0.0, 0.0)],
         [(x0 / 100.0, D / 100.0), (x1 / 100.0, D / 100.0), (x1 / 100.0, 0.0), (x0 / 100.0, 0.0)],
         [(0.0, 0.0, -1.0)] * 4)

    # ---- the back, buried in the wall ---------------------------------------
    quad(deck, obj,
         [(x0, D, 0.0), (x0, D, H), (x1, D, H), (x1, D, 0.0)],
         [(x0 / 100.0, 0.0), (x0 / 100.0, H / 100.0), (x1 / 100.0, H / 100.0), (x1 / 100.0, 0.0)],
         [(0.0, 1.0, 0.0)] * 4)

    # ---- the two end caps, buried in the end walls --------------------------
    for x, nx in ((x0, -1.0), (x1, 1.0)):
        for i in range(N):
            y0, z0 = prof_y(i), prof_z(i)
            y1, z1 = prof_y(i + 1), prof_z(i + 1)
            corners = [(x, y0, 0.0), (x, y1, 0.0), (x, y1, z1), (x, y0, z0)]
            if nx > 0:
                corners = list(reversed(corners))
            quad(deck, obj, corners,
                 [(c[1] / 100.0, c[2] / 100.0) for c in corners],
                 [(nx, 0.0, 0.0)] * 4)

    return obj


def build_trim(design, report):
    """Every neon line, as one mesh with two material slots and no collision."""
    D = design["kDepthUU"]
    L = design["kLengthUU"]

    y_at, z_at, slope_at, _u_at, _fd, _fh, _z0 = face_curve(design)

    # ---- the owner's own dressing, in their units ---------------------------
    OWNER_RUN = design["kOwnerRun"]        # 4.20
    OWNER_WIDTH = design["kOwnerWidth"]    # 2.60
    # Stripe centres across the width, as fractions of the width. Read off the
    # OBJ: edge_light_l/r at Z -+1.300, lane_light_1..4 at -+0.914 / -+0.321.
    EDGE_W = 0.070 / OWNER_WIDTH           # edge_light thickness / width
    LANE_W = 0.050 / OWNER_WIDTH
    STRIPES = [(0.0, EDGE_W), ((-0.914 + 1.3) / OWNER_WIDTH, LANE_W),
               ((-0.321 + 1.3) / OWNER_WIDTH, LANE_W), ((0.321 + 1.3) / OWNER_WIDTH, LANE_W),
               ((0.914 + 1.3) / OWNER_WIDTH, LANE_W)]
    # The owner's lane lights stop short of both ends of the run: they span
    # X -1.789..2.082 of a -2.05..2.15 run. Kept, because the toe line and the
    # crest line are what terminate them and that is the owner's own composition.
    RIB_U0 = (-1.789 + 2.05) / OWNER_RUN
    RIB_U1 = (2.082 + 2.05) / OWNER_RUN
    # takeoff_lip_light spans 0.110 of the run; entry_threshold spans 0.090.
    LIP_U = 0.110 / OWNER_RUN
    TOE_U = 0.090 / OWNER_RUN

    PROUD = 3.0            # uu the neon stands off the deck along its normal
    RIB_SPANS = 16         # profile spans per rib (see the sag note below)

    # ONE REPEAT = ONE OWNER RAMP WIDTH, so a repeat seen from above is exactly
    # the owner's ramp. The count is rounded so the tiling closes on the length.
    ideal_period = D * OWNER_WIDTH / OWNER_RUN
    repeats = max(1, int(round(L / ideal_period)))
    period = L / repeats
    report["period"] = period
    report["ideal_period"] = ideal_period
    report["repeats"] = repeats

    obj = Obj()
    lane = obj.group("sideramp_neon_lane")
    lip = obj.group("sideramp_neon_lip")

    def surf(s):
        """Point on the deck at fraction s ALONG THE BUILT FACE, plus its outward normal."""
        y, z = y_at(s), z_at(s)
        slope = slope_at(s)
        inv = 1.0 / math.sqrt(1.0 + slope * slope)
        return (y, z), (0.0, -slope * inv, inv)

    def strip_along_profile(group, xc, half_w, u0, u1, spans):
        """A proud strip climbing the face at along-wall position xc."""
        us = [u0 + (u1 - u0) * k / float(spans) for k in range(spans + 1)]
        pts = []
        for u in us:
            (y, z), n = surf(u)
            pts.append((y + n[1] * PROUD, z + n[2] * PROUD, n))
        for k in range(spans):
            ya, za, na = pts[k]
            yb, zb, nb = pts[k + 1]
            # top
            quad(group, obj,
                 [(xc - half_w, ya, za), (xc + half_w, ya, za),
                  (xc + half_w, yb, zb), (xc - half_w, yb, zb)],
                 [(0.0, us[k]), (1.0, us[k]), (1.0, us[k + 1]), (0.0, us[k + 1])],
                 [na, na, nb, nb])
            # the two sides, so the strip is not invisible edge-on from the ramp
            for sx, sn in ((-half_w, -1.0), (half_w, 1.0)):
                corners = [(xc + sx, ya, za - PROUD), (xc + sx, yb, zb - PROUD),
                           (xc + sx, yb, zb), (xc + sx, ya, za)]
                if sn > 0:
                    corners = list(reversed(corners))
                quad(group, obj, corners,
                     [(c[1] / 100.0, c[2] / 100.0) for c in corners],
                     [(sn, 0.0, 0.0)] * 4)

    def strip_along_wall(group, u_lo, u_hi):
        """A continuous line running the whole length, at a fixed depth band."""
        x0, x1 = -L * 0.5, L * 0.5
        (ya, za), na = surf(u_lo)
        (yb, zb), nb = surf(u_hi)
        ya, za = ya + na[1] * PROUD, za + na[2] * PROUD
        yb, zb = yb + nb[1] * PROUD, zb + nb[2] * PROUD
        quad(group, obj,
             [(x0, ya, za), (x1, ya, za), (x1, yb, zb), (x0, yb, zb)],
             [(x0 / 100.0, 0.0), (x1 / 100.0, 0.0), (x1 / 100.0, 1.0), (x0 / 100.0, 1.0)],
             [na, na, nb, nb])
        # the two edges of the line, so it has thickness from a low angle
        for (yy, zz, nn), flip in (((ya, za, na), False), ((yb, zb, nb), True)):
            corners = [(x0, yy, zz - PROUD), (x1, yy, zz - PROUD), (x1, yy, zz), (x0, yy, zz)]
            if flip:
                corners = list(reversed(corners))
            quad(group, obj, corners,
                 [(c[0] / 100.0, c[2] / 100.0) for c in corners],
                 [(0.0, -1.0 if not flip else 1.0, 0.0)] * 4)

    # ---- the repeating motif: the owner's edge and lane lights --------------
    for r in range(repeats):
        base = -L * 0.5 + period * r
        for frac, width_frac in STRIPES:
            xc = base + period * frac
            strip_along_profile(lane, xc, period * width_frac * 0.5, RIB_U0, RIB_U1, RIB_SPANS)
    report["ribs"] = repeats * len(STRIPES)

    # ---- the continuous lines: takeoff lip at the crest, threshold at the toe
    strip_along_wall(lip, 1.0 - LIP_U, 1.0)
    strip_along_wall(lip, 0.0, TOE_U)

    return obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(PROJECT_ROOT, "Art", "SideRamp"))
    ap.add_argument("--owner-obj", default=OWNER_OBJ)
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()

    design = read_design(HEADER)
    print("DESIGN  (read from {0})".format(os.path.relpath(HEADER, PROJECT_ROOT)))
    for k in sorted(design):
        print("        {0:22s} {1}".format(k, design[k]))

    if args.verify:
        verify_profile(design, args.owner_obj)

    os.makedirs(args.out, exist_ok=True)

    shell = build_shell(design)
    shell_path = os.path.join(args.out, "SM_SideRampConcave.obj")
    shell.write(shell_path, "SM_SideRampConcave.mtl",
                [("sideramp_deck", (0.10, 0.11, 0.13))])
    shell_tris = sum(len(t) for _, t in shell.groups)

    report = {}
    trim = build_trim(design, report)
    trim_path = os.path.join(args.out, "SM_SideRampConcaveTrim.obj")
    trim.write(trim_path, "SM_SideRampConcaveTrim.mtl",
               [("sideramp_neon_lane", (0.18, 0.78, 1.00)),
                ("sideramp_neon_lip", (0.55, 0.92, 1.00))])
    trim_tris = sum(len(t) for _, t in trim.groups)

    # The numbers a report has to quote, computed here rather than restated. The facet
    # angles are CHORD angles, because complex-as-simple collision reports chord normals
    # and those are what the C++ header's asserts and the in-game probe both measure.
    D, H, N = design["kDepthUU"], design["kHeightUU"], int(design["kFacetCount"])
    _y, _z, _slope, u_at, full_d, full_h, z_unbuilt = face_curve(design)
    facet_deg = []
    for i in range(N):
        ya, za = _y(i / float(N)), _z(i / float(N))
        yb, zb = _y((i + 1) / float(N)), _z((i + 1) / float(N))
        facet_deg.append(math.degrees(math.atan((zb - za) / (yb - ya))))
    print("")
    print("SHELL   {0}  {1} triangles, {2} verts".format(shell_path, shell_tris, len(shell.v)))
    print("TRIM    {0}  {1} triangles, {2} verts".format(trim_path, trim_tris, len(trim.v)))
    print("MOTIF   {0} repeats of {1:.2f} uu (the owner's proportion asks {2:.2f} uu; "
          "{3:+.3f}% to close on the length), {4} ribs total"
          .format(report["repeats"], report["period"], report["ideal_period"],
                  100.0 * (report["period"] / report["ideal_period"] - 1.0), report["ribs"]))
    print("FACETS  {0} facets, {1:.3f} deg at the toe to {2:.3f} deg at the lip (CHORDS)"
          .format(N, facet_deg[0], facet_deg[-1]))
    print("SEGMENT u {0:.4f}..1.0000 of a full parabola {1:.1f} uu x {2:.1f} uu; its lower "
          "{3:.1f} uu of rise is NOT built (that is the part that would be walkable)"
          .format(design["kProfileStartFrac"], full_d, full_h, z_unbuilt))
    print("SIZE    {0:.0f} uu long x {1:.0f} uu deep x {2:.0f} uu tall".format(
        design["kLengthUU"], D, H))


if __name__ == "__main__":
    sys.exit(main())
