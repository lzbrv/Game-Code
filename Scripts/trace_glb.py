#!/usr/bin/env python3
"""trace_glb.py -- pure-python skinned-GLB writer for the Trace character bodies.

Spec: PIPELINE_DESIGN.md SS2 (release-design corpus).  Stock Python 3, zero
dependencies -- the mirror of the reader `railgun_glb_to_obj.py:93-118`.  The
binary layout (12-byte header; JSON chunk 0x4E4F534A padded with 0x20; BIN
chunk 0x004E4942 padded with 0x00; 4-byte-aligned bufferViews; POSITION
min/max) is carried over from the proven smoke writer
`scratchpad/glb_smoke/make_smoke_glbs.py`, whose files imported cleanly
through UE 5.8 Interchange (PIPELINE_DESIGN SS0, all MEASURED).

AUTHORING SPACE (SS2.1): everything user-facing is authored in UE units and
axes (uu, Z-up, character faces +Y like the Mannequin; left = +X).  The
writer converts on output:

    gl.x = ue.x / 100      # MEASURED inverse of UE(X,Y,Z) = (gl.x, gl.z, gl.y) * 100
    gl.y = ue.z / 100
    gl.z = ue.y / 100

That map is a y/z swap (det = -1), therefore the writer reverses triangle
winding exactly once on output (emits indices a,c,b) and converts normals
with the same component swap (n_gl = (n.x, n.z, n.y), unit length preserved).
Authoring-side convention that makes this correct: every triangle is wound so
that cross(b-a, c-a), computed on the raw UE coordinate tuples, points
OUTWARD, and the stored corner normals agree with it (checked at write time
-- this is the guard against the inverted-winding risk, PIPELINE SS11 row 2).

SKINNING (SS2.3): rigid per-part binding by design.  Every vertex is 100%%
bound to exactly one bone (JOINTS_0 ubyte, WEIGHTS_0 float vec4); the bodies
are segmented machine suits, so no weight painting exists or is needed.
Inverse bind matrices are identity rotation + translate(-joint_world_gl),
column-major, exactly as smoke-tested.

OUTPUT LAYOUT (SS2.4): one skin (all joints, skeleton = root node index), one
mesh with ONE PRIMITIVE PER MATERIAL SLOT, one node carrying mesh+skin, scene
lists the root bone + that node.  Material names pass through verbatim as UE
material slot names (MEASURED).  UVs: box projection at 100 uu/tile (the
railgun precedent, railgun_glb_to_obj.py:326-331 -- flat-color materials only
need non-degenerate UVs); the writer regenerates them at output so builders
never have to author UVs by hand.

Indices are uint16 with auto-promotion to uint32 above 65535 verts (a
compliant body never gets there: budget 1200-6000 tris, writer check SS2.5).
"""
import hashlib
import json
import math
import os
import struct

JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942

# ---------------------------------------------------------------------------
# small vector helpers (UE-space tuples)
# ---------------------------------------------------------------------------


def v_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def v_len(a):
    return math.sqrt(v_dot(a, a))


def v_norm(a):
    l = v_len(a)
    if l < 1e-12:
        raise ValueError("zero-length vector cannot be normalized")
    return (a[0] / l, a[1] / l, a[2] / l)


def _rot_axis_angle(p, axis, deg):
    """Rodrigues rotation of point/vector p about unit axis by deg."""
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    k = v_norm(axis)
    return v_add(v_add(v_scale(p, c), v_scale(v_cross(k, p), s)),
                 v_scale(k, v_dot(k, p) * (1.0 - c)))


# ---------------------------------------------------------------------------
# TriMesh
# ---------------------------------------------------------------------------


class TriMesh(object):
    """Triangle soup with per-corner normals (+ optional UVs), in UE uu.

    Winding law (see module docstring): cross(b-a, c-a) points outward and
    agrees with the stored corner normals.  All primitive helpers below emit
    split vertices per face (hard normals -- ART_BIBLE SS4.2 rule 2); lathes
    smooth only around their axis, the one permitted smoothing.
    """

    __slots__ = ("positions", "normals", "uvs", "tris")

    def __init__(self, positions=None, normals=None, uvs=None, tris=None):
        self.positions = list(positions or [])
        self.normals = list(normals or [])
        self.uvs = list(uvs or [])
        self.tris = list(tris or [])

    # -- transforms (each returns a NEW TriMesh) ----------------------------

    def _mapped(self, fpos, fnrm, flip_winding=False):
        m = TriMesh([fpos(p) for p in self.positions],
                    [fnrm(n) for n in self.normals],
                    list(self.uvs), list(self.tris))
        if flip_winding:
            m.tris = [(a, c, b) for (a, b, c) in m.tris]
        return m

    def translate(self, dx, dy, dz):
        d = (dx, dy, dz)
        return self._mapped(lambda p: v_add(p, d), lambda n: n)

    def rotate(self, axis, deg, pivot=(0.0, 0.0, 0.0)):
        return self._mapped(
            lambda p: v_add(_rot_axis_angle(v_sub(p, pivot), axis, deg), pivot),
            lambda n: _rot_axis_angle(n, axis, deg))

    def rotate_z(self, deg):
        return self.rotate((0.0, 0.0, 1.0), deg)

    def scale(self, sx, sy=None, sz=None):
        """Axis-aligned scale about the origin.  Normals go through the
        inverse-transpose (n/s per axis, renormalized) so non-uniform scale
        does not skew them.  Negative factors are refused -- mirroring must go
        through mirrored_x() so winding is handled deliberately."""
        if sy is None:
            sy = sx
        if sz is None:
            sz = sx
        if sx <= 0 or sy <= 0 or sz <= 0:
            raise ValueError("scale factors must be positive (use mirrored_x)")
        return self._mapped(
            lambda p: (p[0] * sx, p[1] * sy, p[2] * sz),
            lambda n: v_norm((n[0] / sx, n[1] / sy, n[2] / sz)))

    def mirrored_x(self):
        """Left<->right (flip UE X).  det = -1, so winding is re-reversed here
        to keep the outward-winding law true on the mirrored copy."""
        return self._mapped(lambda p: (-p[0], p[1], p[2]),
                            lambda n: (-n[0], n[1], n[2]),
                            flip_winding=True)

    def merged(self, other):
        base = len(self.positions)
        m = TriMesh(self.positions + other.positions,
                    self.normals + other.normals,
                    self.uvs + other.uvs,
                    list(self.tris))
        m.tris.extend((a + base, b + base, c + base) for (a, b, c) in other.tris)
        return m

    # -- queries ------------------------------------------------------------

    def bounds(self):
        if not self.positions:
            raise ValueError("bounds() on empty mesh")
        xs = [p[0] for p in self.positions]
        ys = [p[1] for p in self.positions]
        zs = [p[2] for p in self.positions]
        return ((min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs)))

    def area(self):
        """Summed triangle area, uu^2 (the accent-budget check, ART_BIBLE
        SS2.3.3 / CHARACTER_LANGUAGE SS8 T4)."""
        total = 0.0
        for a, b, c in self.tris:
            pa, pb, pc = self.positions[a], self.positions[b], self.positions[c]
            total += 0.5 * v_len(v_cross(v_sub(pb, pa), v_sub(pc, pa)))
        return total


# ---------------------------------------------------------------------------
# hard-normal primitive helpers (all emit split verts per face)
# ---------------------------------------------------------------------------


def _add_face(mesh, corners, normal):
    """Fan-triangulate a planar CONVEX polygon with one shared hard normal.
    Winding is normalized against `normal` so callers can list corners in
    either order."""
    n = v_norm(normal)
    if len(corners) < 3:
        raise ValueError("face needs >= 3 corners")
    w = v_cross(v_sub(corners[1], corners[0]), v_sub(corners[2], corners[0]))
    pts = list(corners) if v_dot(w, n) >= 0 else list(reversed(corners))
    base = len(mesh.positions)
    mesh.positions.extend(pts)
    mesh.normals.extend([n] * len(pts))
    for i in range(1, len(pts) - 1):
        mesh.tris.append((base, base + i, base + i + 1))


def box(sx, sy, sz, chamfer=0.0):
    """Chamfered axis-aligned box centered at origin.  chamfer > 0 cuts
    one-segment 45-degree bevels on all 12 edges plus 8 corner triangles
    (ART_BIBLE SS4.2: chamfers are single 45deg bevels, one segment; bodies
    use the 2-6 uu hand-scale class)."""
    hx, hy, hz = sx / 2.0, sy / 2.0, sz / 2.0
    c = float(chamfer)
    if c < 0 or c * 2.0 >= min(sx, sy, sz):
        if c != 0.0:
            raise ValueError("chamfer too large for box dims")
    m = TriMesh()
    if c == 0.0:
        for axis in range(3):
            for sign in (1.0, -1.0):
                n = tuple(sign if i == axis else 0.0 for i in range(3))
                u_ax, v_ax = [i for i in range(3) if i != axis]
                h = (hx, hy, hz)
                quad = []
                for du, dv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                    p = [0.0, 0.0, 0.0]
                    p[axis] = sign * h[axis]
                    p[u_ax] = du * h[u_ax]
                    p[v_ax] = dv * h[v_ax]
                    quad.append(tuple(p))
                _add_face(m, quad, n)
        return m

    h = (hx, hy, hz)

    def corner_pts(sx_, sy_, sz_):
        """The three chamfer points nearest corner (sx_,sy_,sz_) (signs)."""
        s = (sx_, sy_, sz_)
        pts = []
        for axis in range(3):  # the point ON the face perpendicular to `axis`
            p = [s[i] * (h[i] - c) for i in range(3)]
            p[axis] = s[axis] * h[axis]
            pts.append(tuple(p))
        return pts  # [on +-x face, on +-y face, on +-z face]

    # main faces (inset by chamfer)
    for axis in range(3):
        for sign in (1.0, -1.0):
            n = tuple(sign if i == axis else 0.0 for i in range(3))
            u_ax, v_ax = [i for i in range(3) if i != axis]
            quad = []
            for du, dv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                p = [0.0, 0.0, 0.0]
                p[axis] = sign * h[axis]
                p[u_ax] = du * (h[u_ax] - c)
                p[v_ax] = dv * (h[v_ax] - c)
                quad.append(tuple(p))
            _add_face(m, quad, n)
    # edge bevels: for each pair of axes, 4 edges
    for a1 in range(3):
        for a2 in range(a1 + 1, 3):
            a3 = 3 - a1 - a2
            for s1 in (1.0, -1.0):
                for s2 in (1.0, -1.0):
                    p00 = [0.0, 0.0, 0.0]
                    p00[a1] = s1 * h[a1]
                    p00[a2] = s2 * (h[a2] - c)
                    p01 = [0.0, 0.0, 0.0]
                    p01[a1] = s1 * (h[a1] - c)
                    p01[a2] = s2 * h[a2]
                    quad = []
                    for e3 in (-(h[a3] - c), (h[a3] - c)):
                        q0, q1 = list(p00), list(p01)
                        q0[a3] = e3
                        q1[a3] = e3
                        quad = quad + [tuple(q0), tuple(q1)] if not quad else \
                            [quad[0], quad[1], tuple(q1), tuple(q0)]
                    n = [0.0, 0.0, 0.0]
                    n[a1] = s1
                    n[a2] = s2
                    _add_face(m, quad, v_norm(tuple(n)))
    # corner triangles
    for s1 in (1.0, -1.0):
        for s2 in (1.0, -1.0):
            for s3 in (1.0, -1.0):
                pts = corner_pts(s1, s2, s3)
                _add_face(m, pts, v_norm((s1, s2, s3)))
    return m


def prism(sides, radius, height, cap=True):
    """n-gon column about +Z, centered at the origin (z in [-h/2, +h/2]),
    hard side facets."""
    hz = height / 2.0
    m = TriMesh()
    ring = [(radius * math.cos(2 * math.pi * i / sides),
             radius * math.sin(2 * math.pi * i / sides)) for i in range(sides)]
    for i in range(sides):
        x0, y0 = ring[i]
        x1, y1 = ring[(i + 1) % sides]
        n = v_norm((x0 + x1, y0 + y1, 0.0))
        _add_face(m, [(x0, y0, -hz), (x1, y1, -hz), (x1, y1, hz), (x0, y0, hz)], n)
    if cap:
        _add_face(m, [(x, y, hz) for (x, y) in ring], (0.0, 0.0, 1.0))
        _add_face(m, [(x, y, -hz) for (x, y) in ring], (0.0, 0.0, -1.0))
    return m


def extrude(points2d, height, cap=True):
    """Arbitrary closed CONVEX polygon in the XY plane, extruded +Z from 0 to
    `height`.  Point order may be CW or CCW (auto-detected via signed area);
    convexity is asserted because caps are fan-triangulated."""
    pts = list(points2d)
    if len(pts) < 3:
        raise ValueError("extrude needs >= 3 points")
    area2 = sum(pts[i][0] * pts[(i + 1) % len(pts)][1] -
                pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))
    if area2 < 0:
        pts = list(reversed(pts))  # force CCW viewed from +Z
    # convexity check
    for i in range(len(pts)):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % len(pts)]
        cx, cy = pts[(i + 2) % len(pts)]
        if (bx - ax) * (cy - by) - (by - ay) * (cx - bx) < -1e-6:
            raise ValueError("extrude() requires a convex polygon")
    m = TriMesh()
    for i in range(len(pts)):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % len(pts)]
        edge_n = v_norm((y1 - y0, -(x1 - x0), 0.0))  # outward for CCW
        _add_face(m, [(x0, y0, 0.0), (x1, y1, 0.0),
                      (x1, y1, height), (x0, y0, height)], edge_n)
    if cap:
        _add_face(m, [(x, y, height) for (x, y) in pts], (0.0, 0.0, 1.0))
        _add_face(m, [(x, y, 0.0) for (x, y) in pts], (0.0, 0.0, -1.0))
    return m


def lathe(profile, sides, smooth=True):
    """profile = [(radius, z), ...] revolved about +Z.  smooth=True shares
    normals AROUND the axis only (the one permitted smoothing: ART_BIBLE
    SS4.2 'cylindrical surfaces smooth only around their axis'); rings
    between profile rows stay hard (each band has split vertices)."""
    if len(profile) < 2:
        raise ValueError("lathe needs >= 2 profile rows")
    m = TriMesh()

    def pos(r, z, i):
        a = 2 * math.pi * i / sides
        return (r * math.cos(a), r * math.sin(a), z)

    for k in range(len(profile) - 1):
        r0, z0 = profile[k]
        r1, z1 = profile[k + 1]
        if r0 < 1e-9 and r1 < 1e-9:
            continue
        # band normal in the (radial, z) plane: perpendicular to the profile edge
        dr, dz = r1 - r0, z1 - z0
        nl = math.sqrt(dr * dr + dz * dz)
        nr, nz = dz / nl, -dr / nl  # outward for increasing-z profiles
        if nr < 0:  # keep radially outward
            nr, nz = -nr, -nz
        for i in range(sides):
            j = (i + 1) % sides
            a_i = 2 * math.pi * (i + 0.0) / sides
            # NOTE: computed from i+1, NOT from the modulo'd j -- on the
            # wrap-around segment the mid angle must be ~(2pi - pi/sides),
            # not pi/2 of the wrong hemisphere (pole normals + the flip test
            # both derive from it).
            a_j = 2 * math.pi * (i + 1.0) / sides

            def ring_n(angle):
                return v_norm((nr * math.cos(angle), nr * math.sin(angle), nz))

            quad = []
            if r0 >= 1e-9:
                quad.extend([pos(r0, z0, i), pos(r0, z0, j)])
            else:
                quad.append((0.0, 0.0, z0))
            if r1 >= 1e-9:
                quad.extend([pos(r1, z1, j), pos(r1, z1, i)])
            else:
                quad.append((0.0, 0.0, z1))
            if smooth:
                # per-corner normals around the axis
                base = len(m.positions)
                norms = []
                for p in quad:
                    ang = math.atan2(p[1], p[0]) if (abs(p[0]) > 1e-12 or abs(p[1]) > 1e-12) \
                        else (a_i + a_j) * 0.5
                    norms.append(ring_n(ang))
                w = v_cross(v_sub(quad[1], quad[0]), v_sub(quad[2], quad[0]))
                if v_dot(w, norms[0]) < 0:
                    quad = list(reversed(quad))
                    norms = list(reversed(norms))
                m.positions.extend(quad)
                m.normals.extend(norms)
                for t in range(1, len(quad) - 1):
                    m.tris.append((base, base + t, base + t + 1))
            else:
                mid = (a_i + a_j) * 0.5
                _add_face(m, quad, ring_n(mid))
    # flat caps where the profile ends at r > 0
    r_first, z_first = profile[0]
    r_last, z_last = profile[-1]
    if r_first >= 1e-9:
        _add_face(m, [pos(r_first, z_first, i) for i in range(sides)],
                  (0.0, 0.0, -1.0))
    if r_last >= 1e-9:
        _add_face(m, [pos(r_last, z_last, i) for i in range(sides)],
                  (0.0, 0.0, 1.0))
    return m


def limb(p0, p1, sides, r0, r1, chamfer=0.0):
    """Tapered n-gon prism from point p0 to p1 (both uu) -- the workhorse for
    A-pose arms/legs.  Hard side facets, flat end caps; chamfer > 0 bevels
    both end rims by that amount (single 45-degree segment)."""
    p0, p1 = tuple(p0), tuple(p1)
    axis = v_sub(p1, p0)
    length = v_len(axis)
    if length < 1e-9:
        raise ValueError("limb needs distinct endpoints")
    d = v_scale(axis, 1.0 / length)
    ref = (0.0, 0.0, 1.0) if abs(v_dot(d, (0.0, 0.0, 1.0))) < 0.98 else (1.0, 0.0, 0.0)
    u = v_norm(v_cross(ref, d))
    w = v_cross(d, u)

    def ring(center, radius):
        out = []
        for i in range(sides):
            a = 2 * math.pi * i / sides
            off = v_add(v_scale(u, radius * math.cos(a)),
                        v_scale(w, radius * math.sin(a)))
            out.append(v_add(center, off))
        return out

    m = TriMesh()
    c = float(chamfer)
    if c > 0:
        a0, a1 = v_add(p0, v_scale(d, c)), v_sub(p1, v_scale(d, c))
        rings = [(p0, max(r0 - c, 0.5)), (a0, r0), (a1, r1), (p1, max(r1 - c, 0.5))]
    else:
        rings = [(p0, r0), (p1, r1)]
    ring_pts = [ring(ct, rr) for (ct, rr) in rings]
    for k in range(len(rings) - 1):
        for i in range(sides):
            j = (i + 1) % sides
            quad = [ring_pts[k][i], ring_pts[k][j],
                    ring_pts[k + 1][j], ring_pts[k + 1][i]]
            e1 = v_sub(quad[1], quad[0])
            e2 = v_sub(quad[3], quad[0])
            n = v_norm(v_cross(e1, e2))
            # orient the facet normal away from the limb axis: project the
            # quad center onto the axis and require n to point outward
            center = v_scale(v_add(v_add(quad[0], quad[1]),
                                   v_add(quad[2], quad[3])), 0.25)
            t = v_dot(v_sub(center, p0), d)
            radial = v_sub(center, v_add(p0, v_scale(d, t)))
            if v_dot(n, radial) < 0:
                n = v_scale(n, -1.0)
            _add_face(m, quad, n)
    _add_face(m, ring_pts[-1], d)
    _add_face(m, ring_pts[0], v_scale(d, -1.0))
    return m


# ---------------------------------------------------------------------------
# the writer
# ---------------------------------------------------------------------------


class _Buf(object):
    """Binary buffer builder -- carried over from the proven smoke writer
    (make_smoke_glbs.py), 4-byte-aligned views."""

    def __init__(self):
        self.data = b""
        self.views = []
        self.accessors = []

    def add(self, raw, count, ctype, atype, target=None, minmax=None):
        while len(self.data) % 4:
            self.data += b"\x00"
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(raw)}
        if target:
            view["target"] = target
        self.data += raw
        self.views.append(view)
        acc = {"bufferView": len(self.views) - 1, "componentType": ctype,
               "count": count, "type": atype}
        if minmax:
            acc["min"], acc["max"] = minmax
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def vec3(self, vals, minmax=False):
        raw = b"".join(struct.pack("<3f", *v) for v in vals)
        mm = None
        if minmax:
            mm = ([min(v[i] for v in vals) for i in range(3)],
                  [max(v[i] for v in vals) for i in range(3)])
        return self.add(raw, len(vals), 5126, "VEC3", 34962, mm)

    def vec4f(self, vals):
        raw = b"".join(struct.pack("<4f", *v) for v in vals)
        return self.add(raw, len(vals), 5126, "VEC4", 34962)

    def vec4ub(self, vals):
        raw = b"".join(struct.pack("<4B", *v) for v in vals)
        return self.add(raw, len(vals), 5121, "VEC4", 34962)

    def vec2(self, vals):
        raw = b"".join(struct.pack("<2f", *v) for v in vals)
        return self.add(raw, len(vals), 5126, "VEC2", 34962)

    def indices(self, vals):
        if max(vals) < 65536:
            raw = b"".join(struct.pack("<H", v) for v in vals)
            return self.add(raw, len(vals), 5123, "SCALAR", 34963)
        raw = b"".join(struct.pack("<I", v) for v in vals)
        return self.add(raw, len(vals), 5125, "SCALAR", 34963)

    def mat4(self, mats):
        raw = b"".join(struct.pack("<16f", *m) for m in mats)
        return self.add(raw, len(mats), 5126, "MAT4")


def _pad4(b, fill):
    while len(b) % 4:
        b += fill
    return b


class GlbCheckError(RuntimeError):
    """A writer self-check failed.  Fails the GENERATION, not the import
    (PIPELINE SS2.5)."""


class TraceGlbWriter(object):
    """Assembles bones + rigid parts + materials into one skinned GLB.

    add_part() accepts two keyword extensions over the SS2.2 signature:
      name        part name (used by the team-glow layout checks and by the
                  silhouette harness for feature attribution);
      crown_break marks parts allowed above Z 176 (Lily fins, Rocco crest...)
                  which the height self-check must exclude (LANGUAGE SS8 T5).
    """

    def __init__(self):
        self.materials = []          # dicts (glTF material objects)
        self.mat_names = []
        self.bones = []              # (name, parent_name_or_None, head_uu)
        self.bone_index = {}
        self.parts = []              # (bone, TriMesh, mat_index, name, crown_break)
        self._uses_emissive_strength = False

    # -- declarations -------------------------------------------------------

    def add_material(self, name, base_color, metallic=0.0, roughness=0.5,
                     emissive=None, emissive_strength=1.0):
        """`name` IS the future UE material slot name (MEASURED verbatim
        pass-through).  base_color/emissive are linear floats; emissive writes
        emissiveFactor + KHR_materials_emissive_strength so the file is honest
        even though import stamps materials externally (import_pack.py
        precedent: 'materials are stamped, not imported')."""
        mat = {"name": name,
               "pbrMetallicRoughness": {
                   "baseColorFactor": [float(base_color[0]), float(base_color[1]),
                                       float(base_color[2]), 1.0],
                   "metallicFactor": float(metallic),
                   "roughnessFactor": float(roughness)}}
        if emissive is not None:
            mat["emissiveFactor"] = [min(1.0, float(emissive[0])),
                                     min(1.0, float(emissive[1])),
                                     min(1.0, float(emissive[2]))]
            mat["extensions"] = {"KHR_materials_emissive_strength":
                                 {"emissiveStrength": float(emissive_strength)}}
            self._uses_emissive_strength = True
        self.materials.append(mat)
        self.mat_names.append(name)
        return len(self.materials) - 1

    def add_bone(self, name, parent, head_uu):
        """Identity local rotation; local translation = head - parent_head
        (converted at write).  parent=None exactly once, for 'root' at
        (0,0,0)."""
        if name in self.bone_index:
            raise GlbCheckError("duplicate bone %r" % name)
        if parent is None:
            if any(p is None for (_n, p, _h) in self.bones):
                raise GlbCheckError("second root bone %r" % name)
        elif parent not in self.bone_index:
            raise GlbCheckError("bone %r declared before its parent %r" % (name, parent))
        self.bone_index[name] = len(self.bones)
        self.bones.append((name, parent, tuple(float(c) for c in head_uu)))

    def add_part(self, bone_name, mesh, material, name=None, crown_break=False):
        """RIGID skinning: every vertex of this part gets JOINTS_0=(bone,0,0,0),
        WEIGHTS_0=(1,0,0,0).  See SS2.3 for why rigid binding is the design."""
        if bone_name not in self.bone_index:
            raise GlbCheckError("add_part: unknown bone %r" % bone_name)
        if isinstance(material, str):
            if material not in self.mat_names:
                raise GlbCheckError("add_part: unknown material %r" % material)
            material = self.mat_names.index(material)
        if not (0 <= material < len(self.materials)):
            raise GlbCheckError("add_part: material index %r out of range" % material)
        if not mesh.tris:
            raise GlbCheckError("add_part: empty mesh for %r" % (name or bone_name))
        self.parts.append((bone_name, mesh, material,
                           name or ("part_%d" % len(self.parts)), bool(crown_break)))

    # -- self-checks (SS2.5: fail the generation, not the import) -----------

    def _slot_area(self, mat_index):
        return sum(mesh.area() for (_b, mesh, mi, _n, _cb) in self.parts
                   if mi == mat_index)

    def _slot_index(self, slot_name):
        return self.mat_names.index(slot_name) if slot_name in self.mat_names else None

    def self_check(self):
        problems = []
        # geometry sanity: NaNs, degenerate tris, winding/normal agreement
        tri_total = 0
        for (_b, mesh, _mi, pname, _cb) in self.parts:
            tri_total += len(mesh.tris)
            for p in mesh.positions:
                if any(not math.isfinite(c) for c in p):
                    problems.append("NaN/inf position in part %r" % pname)
                    break
            for (a, b, c) in mesh.tris:
                pa, pb, pc = mesh.positions[a], mesh.positions[b], mesh.positions[c]
                w = v_cross(v_sub(pb, pa), v_sub(pc, pa))
                if 0.5 * v_len(w) <= 1e-6:
                    problems.append("degenerate triangle in part %r" % pname)
                    break
                nsum = v_add(v_add(mesh.normals[a], mesh.normals[b]), mesh.normals[c])
                if v_dot(w, nsum) <= 0:
                    problems.append("winding/normal disagreement in part %r" % pname)
                    break
        if not (1200 <= tri_total <= 6000):
            problems.append("tri count %d outside [1200, 6000]" % tri_total)
        # height / feet (crown_break parts excluded -- LANGUAGE SS8 T5)
        zs_lo, zs_hi = [], []
        for (_b, mesh, _mi, _n, cb) in self.parts:
            (lo, hi) = mesh.bounds()
            zs_lo.append(lo[2])
            if not cb:
                zs_hi.append(hi[2])
        if zs_lo and zs_hi:
            min_z, max_z = min(zs_lo), max(zs_hi)
            height = max_z - min_z
            if abs(height - 176.0) > 0.5:
                problems.append("height %.2f outside 176 +/- 0.5 (crown breaks excluded)"
                                % height)
            if abs(min_z) > 0.5:
                problems.append("feet min Z %.2f outside 0 +/- 0.5" % min_z)
        # accent budget (<= 8%% of total surface area; per-character tighter
        # caps are asserted by T4 in verify_silhouettes.py)
        acc_i = self._slot_index("accent")
        total_area = sum(mesh.area() for (_b, mesh, _mi, _n, _cb) in self.parts)
        if acc_i is not None and total_area > 0:
            pct = 100.0 * self._slot_area(acc_i) / total_area
            if pct > 8.0:
                problems.append("accent area %.2f%% > 8%% of surface" % pct)
        # team_glow: combined read extent >= 120 uu summed over element
        # groups, matching the LANGUAGE SS2.2 arithmetic exactly: back bar 64
        # (both sections spanning Z 112-176) + chest bars 44 + pad strips 28.
        # The pad strips are horizontal (8 x 28 fore-aft on pad tops, read
        # from overhead), so a group's contribution is max(Z extent, Y
        # extent).  Groups = the first two '_'-separated name tokens, which
        # folds l/r mirrors and bar sections ('team_backbar_torso' +
        # 'team_backbar_helm_l/r' -> 'team_backbar').  The back panel is
        # mandatory (bible SS4.4), checkable because the recipe schema names
        # it: some team part name must contain 'back'.
        team_i = self._slot_index("team_glow")
        if team_i is not None:
            groups = {}
            has_back = False
            for (_b, mesh, mi, pname, _cb) in self.parts:
                if mi != team_i:
                    continue
                key = "_".join(pname.split("_")[:2])
                (lo, hi) = mesh.bounds()
                y0, y1, z0, z1 = groups.get(key, (1e9, -1e9, 1e9, -1e9))
                groups[key] = (min(y0, lo[1]), max(y1, hi[1]),
                               min(z0, lo[2]), max(z1, hi[2]))
                if "back" in pname:
                    has_back = True
            extent = sum(max(y1 - y0, z1 - z0)
                         for (y0, y1, z0, z1) in groups.values())
            if extent < 120.0:
                problems.append("team_glow combined read extent %.1f < 120" % extent)
            if not has_back:
                problems.append("no back-side team_glow part (back panel is mandatory)")
        if problems:
            raise GlbCheckError("; ".join(problems))
        return tri_total

    # -- output -------------------------------------------------------------

    @staticmethod
    def _to_gl(p):
        return (p[0] / 100.0, p[2] / 100.0, p[1] / 100.0)

    @staticmethod
    def _n_to_gl(n):
        return (n[0], n[2], n[1])

    def write(self, path):
        """Writes the GLB and a `<stem>_manifest.json` alongside; returns the
        manifest dict."""
        tri_total = self.self_check()

        # bone world positions
        world = {name: head for (name, _p, head) in self.bones}
        root_name = next(n for (n, p, _h) in self.bones if p is None)

        buf = _Buf()
        # nodes 0..len(bones)-1 are the bones, in declaration order
        nodes = []
        children = {}
        for i, (name, parent, head) in enumerate(self.bones):
            if parent is not None:
                children.setdefault(self.bone_index[parent], []).append(i)
        for i, (name, parent, head) in enumerate(self.bones):
            local = head if parent is None else v_sub(head, world[parent])
            nd = {"name": name, "translation": list(self._to_gl(local))}
            if i in children:
                nd["children"] = children[i]
            nodes.append(nd)

        ibms = []
        for (name, _p, head) in self.bones:
            gx, gy, gz = self._to_gl(head)
            ibms.append([1, 0, 0, 0,
                         0, 1, 0, 0,
                         0, 0, 1, 0,
                         -gx, -gy, -gz, 1])
        ibm_acc = buf.mat4(ibms)

        # one primitive per material slot, in material declaration order
        prims = []
        slot_tris = {}
        for mi in range(len(self.materials)):
            pos, nrm, uv, jnt, wgt, idx = [], [], [], [], [], []
            for (bone, mesh, pmi, _pn, _cb) in self.parts:
                if pmi != mi:
                    continue
                ji = self.bone_index[bone]
                base = len(pos)
                for k, p in enumerate(mesh.positions):
                    gp = self._to_gl(p)
                    pos.append(gp)
                    n = mesh.normals[k]
                    nrm.append(self._n_to_gl(n))
                    # box projection at 100 uu/tile off the UE-space position
                    # and normal (railgun_glb_to_obj.py:326-331 precedent)
                    ax = max(range(3), key=lambda c: abs(n[c]))
                    u_, v_ = [(p[1], p[2]), (p[0], p[2]), (p[0], p[1])][ax]
                    uv.append((u_ / 100.0, v_ / 100.0))
                    jnt.append((ji, 0, 0, 0))
                    wgt.append((1.0, 0.0, 0.0, 0.0))
                for (a, b, c) in mesh.tris:
                    # winding reversed exactly once here: det(UE->gl) = -1
                    idx.extend((base + a, base + c, base + b))
            if not pos:
                raise GlbCheckError("material slot %r has no geometry"
                                    % self.mat_names[mi])
            slot_tris[self.mat_names[mi]] = len(idx) // 3
            prims.append({
                "attributes": {
                    "POSITION": buf.vec3(pos, minmax=True),
                    "NORMAL": buf.vec3(nrm),
                    "TEXCOORD_0": buf.vec2(uv),
                    "JOINTS_0": buf.vec4ub(jnt),
                    "WEIGHTS_0": buf.vec4f(wgt),
                },
                "indices": buf.indices(idx),
                "material": mi,
                "mode": 4,
            })

        body_node = len(nodes)
        nodes.append({"name": "body", "mesh": 0, "skin": 0})
        stem = os.path.splitext(os.path.basename(path))[0]
        gltf = {
            "asset": {"version": "2.0", "generator": "trace_glb"},
            "scene": 0,
            "scenes": [{"name": "scene",
                        "nodes": [self.bone_index[root_name], body_node]}],
            "nodes": nodes,
            "meshes": [{"name": stem + "_body", "primitives": prims}],
            "skins": [{"name": stem + "_skin",
                       "joints": list(range(len(self.bones))),
                       "skeleton": self.bone_index[root_name],
                       "inverseBindMatrices": ibm_acc}],
            "materials": self.materials,
            "buffers": [{"byteLength": len(buf.data)}],
            "bufferViews": buf.views,
            "accessors": buf.accessors,
        }
        if self._uses_emissive_strength:
            gltf["extensionsUsed"] = ["KHR_materials_emissive_strength"]

        js = _pad4(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), b" ")
        bn = _pad4(buf.data, b"\x00")
        total = 12 + 8 + len(js) + 8 + len(bn)
        blob = struct.pack("<4sII", b"glTF", 2, total)
        blob += struct.pack("<II", len(js), JSON_CHUNK) + js
        blob += struct.pack("<II", len(bn), BIN_CHUNK) + bn
        with open(path, "wb") as f:
            f.write(blob)

        # writer-level manifest (generate_characters.py extends it)
        lo = [1e9] * 3
        hi = [-1e9] * 3
        hi_body_z = -1e9
        for (_b, mesh, _mi, _n, cb) in self.parts:
            (blo, bhi) = mesh.bounds()
            lo = [min(lo[i], blo[i]) for i in range(3)]
            hi = [max(hi[i], bhi[i]) for i in range(3)]
            if not cb:
                hi_body_z = max(hi_body_z, bhi[2])
        acc_i = self._slot_index("accent")
        team_i = self._slot_index("team_glow")
        total_area = sum(mesh.area() for (_b, mesh, _mi, _n, _cb) in self.parts)
        manifest = {
            "tris": tri_total,
            "verts": sum(len(mesh.positions) for (_b, mesh, _mi, _n, _cb) in self.parts),
            "slots": slot_tris,
            "bones": len(self.bones),
            "height_uu": round(hi_body_z - lo[2], 3),
            "bounds_uu": {"min": [round(v, 3) for v in lo],
                          "max": [round(v, 3) for v in hi]},
            "total_area_uu2": round(total_area, 1),
            "accent_area_uu2": round(self._slot_area(acc_i), 1) if acc_i is not None else 0.0,
            "team_area_uu2": round(self._slot_area(team_i), 1) if team_i is not None else 0.0,
            "accent_area_pct": round(100.0 * self._slot_area(acc_i) / total_area, 2)
            if (acc_i is not None and total_area > 0) else 0.0,
            "sha1": hashlib.sha1(blob).hexdigest(),
        }
        mpath = os.path.join(os.path.dirname(path), stem + "_manifest.json")
        with open(mpath, "w") as f:
            json.dump(manifest, f, indent=2, sort_keys=True)
        return manifest


# ---------------------------------------------------------------------------
# --selftest: emit and re-read a minimal skinned body (writer smoke)
# ---------------------------------------------------------------------------


def _selftest(out_dir):
    """Small end-to-end check of the writer plumbing itself (not a body):
    builds a 5-bone stick figure, writes it, re-parses the container, and
    verifies chunk layout + accessor sanity.  Bodies get the full checks in
    generate_characters.py / verify_silhouettes.py."""
    w = TraceGlbWriter()
    w.add_material("suit", (0.10, 0.10, 0.10), roughness=0.45)
    w.add_material("team_glow", (0.005, 0.005, 0.008),
                   emissive=(0.18, 0.78, 1.0), emissive_strength=1.7)
    w.add_bone("root", None, (0, 0, 0))
    w.add_bone("pelvis", "root", (0, 0, 90))
    w.add_bone("spine_01", "pelvis", (0, 0, 100))
    w.add_bone("head", "spine_01", (0, 0, 155))
    w.add_bone("thigh_l", "pelvis", (12, 0, 85))
    for i in range(90):  # bulk to satisfy the 1200-tri floor
        w.add_part("spine_01", box(30, 18, 0.5).translate(0, 0, 105 + i * 0.5),
                   "suit", name="torso_%d" % i)
    w.add_part("pelvis", box(30, 20, 14, chamfer=3).translate(0, 0, 90), "suit",
               name="pelvis")
    w.add_part("thigh_l", limb((12, 0, 85), (12, 0, 2), 6, 7, 5), "suit",
               name="thigh")
    w.add_part("head", lathe([(0, 0), (8, 4), (9, 12), (7, 19), (0, 21)], 8)
               .translate(0, 0, 155), "suit", name="head")
    w.add_part("spine_01", box(20, 2, 60).translate(0, -10, 140), "team_glow",
               name="team_backbar_torso")
    w.add_part("spine_01", box(20, 2, 62).translate(0, 10, 140), "team_glow",
               name="team_chestbar")
    # deliberately wrong feet first: the 0 +/- 0.5 sole law must trip
    w.add_part("thigh_l", box(16, 30, 4).translate(12, 5, 4), "suit", name="boot")
    try:
        w.self_check()
        raise SystemExit("selftest: expected the feet check to fail at min Z 2")
    except GlbCheckError:
        pass  # expected -- now put the sole on the floor
    w.parts = [p for p in w.parts if p[3] != "boot"]
    w.add_part("thigh_l", box(16, 30, 4).translate(12, 5, 2), "suit", name="boot")
    path = os.path.join(out_dir, "trace_glb_selftest.glb")
    man = w.write(path)
    data = open(path, "rb").read()
    magic, ver, total = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF" and ver == 2 and total == len(data)
    ln, ty = struct.unpack_from("<II", data, 12)
    assert ty == JSON_CHUNK
    js = json.loads(data[20:20 + ln].decode("utf-8"))
    assert len(js["skins"][0]["joints"]) == 5
    assert [m["name"] for m in js["materials"]] == ["suit", "team_glow"]
    assert js["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"] is not None
    print("[trace-glb] selftest OK: %s (%d tris, sha1 %s)"
          % (path, man["tris"], man["sha1"][:12]))


if __name__ == "__main__":
    import sys
    if "--selftest" in sys.argv:
        here = os.path.dirname(os.path.abspath(__file__))
        out = os.path.join(os.path.dirname(here), "Intermediate", "Characters")
        os.makedirs(out, exist_ok=True)
        _selftest(out)
    else:
        print("trace_glb.py is a library; run with --selftest for the writer smoke.")
