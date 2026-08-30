#!/usr/bin/env python3
# =============================================================================
# Trace — pose_hands.py   (Demo 29 item 2, stage 2 of 2)
#
# Poses the owner's first-person arms rig (SK_TraceArms, imported by
# Scripts/import_hands.py) onto the three weapons the first-person view actually
# draws, and emits the result as AnimSequences under
# /Game/Trace/Characters/Hands/Poses.
#
#   stage   runs                                     output
#   build   python3 pose_hands.py                    Intermediate/Hands/TraceArmsPoses.glb
#                                                    Intermediate/Hands/TraceArmsPoses_manifest.json
#                                                    Intermediate/Hands/TraceArmsPose_<Name>.glb  (bake previews)
#   import  editor  pose_hands.py                    /Game/Trace/Characters/Hands/Poses/A_TraceArms_<Name>
#                                                    /Game/Trace/Temp/Preview/SK_TraceArmsPose_<Name>
#
# =============================================================================
# THE POSES AIM AT WHERE THE GUNS ACTUALLY ARE.  Every target in this file is
# derived from the SAME constants TraceCharacterInternal.h places the weapons
# with, and from the SAME art the .uassets were baked from:
#
#   pistol   SM_Railgun_*      origin = ViewModelRightHand - RailgunScale * RailgunGripLocal
#   SMG      SM_RailgunSmg_*   origin = ViewModelRightHand - SmgScale     * SmgGripLocal
#   knife    SK_TraceKnife     TraceKnifeViewFile::RestLocation + HeldCant(), scale 1.0
#
# so a hand posed here closes on the grip the shipped viewmodel puts in front of
# the camera, not on an invented origin.  The landmarks inside each weapon (the
# grip's long axis, the trigger's face, the foregrip, the magazine) are MEASURED
# out of the artist's GLB with the same node-transform rules
# Scripts/railgun_glb_to_obj.py bakes the meshes with — pure translation for the
# pistol, full node matrices for the SMG, because that is what those two
# committed .uassets contain.  Nothing here is typed from a screenshot.
#
# WHAT "CORRECTLY" IS CHECKED AGAINST, numerically, every run:
#   TRIGGER   the index fingertip's distance to the trigger's own face.
#   WRAP      every finger is curled by BINARY SEARCH until its medial axis sits
#             exactly one finger-radius off the weapon's triangles, i.e. until
#             the flesh touches and no further.  There is no typed curl angle.
#   CLEARANCE the deepest penetration of any of the 336 skinned vertices into
#             the weapon's triangle soup, reported per pose.  A pose that pushes
#             a knuckle through the grip fails the run.
#   REACH     the arm IK's extension as a fraction of the arm's own length, so a
#             hyperextended (or folded-double) arm cannot pass silently.
# The frames are still the deliverable — these numbers only stop a pose that is
# obviously wrong from costing a render.
#
# THE RING FINGERS ARE NOT THIS FILE'S PROBLEM ANY MORE.  They were a BIND-POSE
# defect and Scripts/import_hands.py::apply_ring_roll_fix corrects them at the
# source: on the asset this file loads, "+X curls the finger" is true of all four
# fingers on both hands (measured 1.0 / 0.9 degrees of disagreement with the
# middle finger, from 179.1).  So there is no ring special case below, which is
# exactly the property fixing it at the source was for.  The build stage
# RE-ASSERTS it (assert_ring_consistent) so a regressed rig fails here too
# instead of quietly producing a pose with one finger standing up.
#
# AXES AND SPACES — three, and they are kept apart on purpose
#   G   the GLB's own space: metres, Y-up, right-handed.  Bone matrices live
#       here on disk.  Row-vector convention throughout: p' = p * M, so a
#       matrix's ROWS are the images of the basis vectors.
#   C   the arms' UE component space: centimetres, Z-up.  C = (G.x, G.z, G.y)*100
#       — the mapping Interchange uses and import_hands.py measured end to end.
#       Every number import_hands.py's bone table prints is in C.  All the
#       posing below happens here.
#   R   RIG space: ViewModelRoot's frame, where TraceCharacterLayout puts the
#       weapons.  C -> R is ArmsPlacement: uniform scale, yaw, translation.
#       IT IS PART OF THE DELIVERABLE, not scratch: the arms are a body rig
#       (T-posed, 1.83 m across, standing at Z 136.7..161.1 uu) and the
#       first-person view needs them half that size with the hands together in
#       front of the lens.  See ArmsPlacement for the two measurements it is
#       derived from.
#
# VERDICTS — grep lines, not exit codes:
#     build    [pose-hands] BUILD EXIT=0
#     import   [pose-hands] EXIT=0
# =============================================================================
import json
import math
import os
import struct
import sys

try:
    import unreal
except ImportError:                                   # plain python3 stage
    unreal = None

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT_DIR = os.path.join(PROJECT_ROOT, "Scripts")

ARMS_GLB = os.path.join(PROJECT_ROOT, "Intermediate", "Hands", "TraceArms.glb")
ARMS_MANIFEST = os.path.join(PROJECT_ROOT, "Intermediate", "Hands", "TraceArms_manifest.json")
OUT_DIR = os.path.join(PROJECT_ROOT, "Intermediate", "Hands")
POSES_GLB = os.path.join(OUT_DIR, "TraceArmsPoses.glb")
POSES_MANIFEST = os.path.join(OUT_DIR, "TraceArmsPoses_manifest.json")

PISTOL_GLB = os.path.join(PROJECT_ROOT, "Art", "Railgun", "railgun.glb")
SMG_GLB = os.path.join(PROJECT_ROOT, "Art", "Smg", "railgun_smg.glb")
KNIFE_GLB = os.path.join(PROJECT_ROOT, "Art", "Pack", "models", "butterfly_knife.glb")

ROOT = "/Game/Trace/Characters/Hands"
POSE_DIR = ROOT + "/Poses"
STAGING = ROOT + "/_PoseImport"
PIPELINE_DIR = ROOT + "/_PosePipelines"
PREVIEW_DIR = "/Game/Trace/Temp/Preview"
ARMS_MESH = ROOT + "/SK_TraceArms"
ARMS_SKELETON = ROOT + "/SK_TraceArms_Skeleton"
SHELL_MI = "/Game/Trace/Art/Pack/Materials/MI_Pack_shell"
# *** THE PREVIEW MESHES ARE DELIBERATELY THE WRONG COLOUR. ***
# They are throwaway render subjects, and in MI_Pack_shell they are the same
# dark blue as the guns: the first four study frames came back as an unreadable
# wall of blue in which a hand and a receiver were indistinguishable, which is
# not a picture anybody can judge a grip from.  Amber against the weapons' blue
# separates them instantly.  The DELIVERABLE — SK_TraceArms itself — keeps
# MI_Pack_shell, which is what the shipped hands wear.
PREVIEW_MI = "/Game/Trace/Art/Pack/Materials/MI_Pack_core_amber"

DEFAULT_GLTF_ASSETS_PIPELINE = ("/Interchange/Pipelines/DefaultGLTFAssetsPipeline"
                                ".DefaultGLTFAssetsPipeline")
DEFAULT_GLTF_PIPELINE = "/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline"

STAGE = os.environ.get("TRACE_POSE_STAGE") or ("import" if unreal else "build")

_failures = []


def log(msg):
    line = "[Trace] {0}".format(msg)
    if unreal:
        unreal.log(line)
    else:
        print(line)
        sys.stdout.flush()


def fail(msg):
    _failures.append(msg)
    log("PROBLEM: {0}".format(msg))


# =============================================================================
# *** THE WEAPONS, AS THE FIRST-PERSON VIEW PLACES THEM. ***
#
# Copied — with their derivations, not their results — from
# TraceCharacterInternal.h.  They are written as the same arithmetic the C++
# does rather than as pre-multiplied literals, for the reason that file gives:
# "a weapon can only ever be placed where the hand already is, and resizing one
# can never slide it out of the fist."  Retune the drawn lengths there and this
# file follows without being touched.
#
# WHAT WOULD CATCH A DRIFT.  If TraceCharacterInternal.h changes and this table
# does not, the poses would aim at the old gun.  assert_weapon_placement() below
# re-derives each origin from the same landmarks and checks the muzzle lands
# where that file's own comments say it does (24.5 uu out for the pistol, 27.6
# for the SMG) — the one number both files state independently.
# =============================================================================
VIEWMODEL_RIGHT_HAND = (-0.8, 0.0, -4.6)      # TraceCharacterLayout::ViewModelRightHand

RAILGUN_BODY_LENGTH_CM = 185.1                # railgun_manifest.json, Body x span
SMG_BODY_LENGTH_CM = 128.5                    # railgun_smg_manifest.json, Body x span
RAILGUN_DRAWN_UU = 34.0
SMG_DRAWN_UU = 41.1
RAILGUN_SCALE = RAILGUN_DRAWN_UU / RAILGUN_BODY_LENGTH_CM     # 0.18369
SMG_SCALE = SMG_DRAWN_UU / SMG_BODY_LENGTH_CM                 # 0.31984
RAILGUN_GRIP_LOCAL = (-30.0, 0.0, -9.0)
RAILGUN_MUZZLE_LOCAL = (107.4, 0.0, 4.5)
SMG_GRIP_LOCAL = (-30.0, 0.0, -8.5)
SMG_MUZZLE_LOCAL = (58.8, 0.0, 4.5)

# TraceKnifeViewFile::RestLocation / HeldCant() / PackScale — the pose the blade
# hangs in when it is NOT parented to a hand rig, which is exactly the case
# these arms are in (nothing in C++ knows about them yet).
KNIFE_REST_LOCATION = (6.0, 1.2, -4.0)
KNIFE_HELD_CANT = (7.0, -10.0, 4.0)           # pitch, yaw, roll (degrees)
KNIFE_SCALE = 1.0


# =============================================================================
# Row-vector linear algebra.  p' = p * M, so M's ROWS are the images of the
# basis vectors — the same convention import_hands.py uses for the bind
# matrices, so nothing is ever transposed between the two files.
# =============================================================================
def v_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def v_len(a):
    return math.sqrt(v_dot(a, a))


def v_norm(a):
    l = v_len(a)
    if l < 1e-12:
        return (0.0, 0.0, 0.0)
    return (a[0] / l, a[1] / l, a[2] / l)


def m3_mul(a, b):
    """3x3 row-vector product: (a*b) applies a first, then b."""
    return tuple(tuple(sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3))
                 for r in range(3))


def m3_t(a):
    return tuple(tuple(a[c][r] for c in range(3)) for r in range(3))


def v_xform3(v, m):
    return tuple(sum(v[k] * m[k][c] for k in range(3)) for c in range(3))


M3_ID = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def rot_axis(axis, degrees):
    """ROW-vector rotation of `degrees` about `axis`, right-hand rule.

    Rodrigues gives the column-vector matrix; the row-vector one is its
    transpose, which is what the sign of the [n]x term below is.  Checked at
    import time by _selftest(): +90 about +Z takes +X to +Y.
    """
    n = v_norm(axis)
    a = math.radians(degrees)
    c, s = math.cos(a), math.sin(a)
    x, y, z = n
    t = 1.0 - c
    return ((t * x * x + c,     t * x * y + s * z, t * x * z - s * y),
            (t * x * y - s * z, t * y * y + c,     t * y * z + s * x),
            (t * x * z + s * y, t * y * z - s * x, t * z * z + c))


def rot_between(a, b):
    """Smallest rotation taking unit vector a onto unit vector b (row-vector)."""
    a, b = v_norm(a), v_norm(b)
    d = max(-1.0, min(1.0, v_dot(a, b)))
    if d > 1.0 - 1e-12:
        return M3_ID
    if d < -1.0 + 1e-12:
        # antiparallel: any perpendicular axis will do
        axis = v_cross(a, (1.0, 0.0, 0.0))
        if v_len(axis) < 1e-6:
            axis = v_cross(a, (0.0, 1.0, 0.0))
        return rot_axis(axis, 180.0)
    return rot_axis(v_cross(a, b), math.degrees(math.acos(d)))


def rot_ypr(pitch, yaw, roll):
    """UE's FRotator(pitch, yaw, roll) as a row-vector 3x3.

    UE composes roll about +X, then pitch about +Y, then yaw about +Z, and TWO
    of the three are left-handed about their axis:

      pitch  positive is NOSE-UP, i.e. left-handed about +Y.
      roll   positive takes +Y to -Z, i.e. left-handed about +X.  ◆MEASURED
             against UE's own FRotationMatrix rows, and this file had it the
             other way round for a while: the Euler angles it handed to
             unreal.Rotator were then a DIFFERENT rotation from the matrix it
             had solved with, and every study frame at a large pitch came back
             empty because the subject had been swung a few hundred uu behind
             the camera.  A round-trip check does not catch it — the file was
             self-consistently wrong.
      yaw    positive takes +X to +Y, right-handed about +Z.

    _selftest() checks all three.
    """
    return m3_mul(m3_mul(rot_axis((1.0, 0.0, 0.0), -roll),
                         rot_axis((0.0, 1.0, 0.0), -pitch)),
                  rot_axis((0.0, 0.0, 1.0), yaw))


def m4(rot3, trans):
    """4x4 row-vector rigid transform as (rows, translation)."""
    return (rot3, tuple(trans))


def m4_mul(a, b):
    """a then b."""
    return (m3_mul(a[0], b[0]), v_add(v_xform3(a[1], b[0]), b[1]))


def m4_point(p, m):
    return v_add(v_xform3(p, m[0]), m[1])


def m4_inv(m):
    ri = m3_t(m[0])
    return (ri, v_mul(v_xform3(m[1], ri), -1.0))


def about(rot3, pivot):
    """A rotation applied about `pivot` instead of about the origin."""
    return (rot3, v_sub(pivot, v_xform3(pivot, rot3)))


def m3_to_quat(m):
    """Row-vector 3x3 -> glTF quaternion [x, y, z, w].

    Identical to import_hands.py::m_to_trs's branch structure, on purpose: the
    two files write nodes into the same file format and must agree exactly.
    """
    r = m
    tr = r[0][0] + r[1][1] + r[2][2]
    if tr > 0.0:
        s = math.sqrt(tr + 1.0) * 2.0
        w, x, y, z = 0.25 * s, (r[1][2] - r[2][1]) / s, (r[2][0] - r[0][2]) / s, (r[0][1] - r[1][0]) / s
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        s = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0
        w, x, y, z = (r[1][2] - r[2][1]) / s, 0.25 * s, (r[1][0] + r[0][1]) / s, (r[2][0] + r[0][2]) / s
    elif r[1][1] > r[2][2]:
        s = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0
        w, x, y, z = (r[2][0] - r[0][2]) / s, (r[1][0] + r[0][1]) / s, 0.25 * s, (r[2][1] + r[1][2]) / s
    else:
        s = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0
        w, x, y, z = (r[0][1] - r[1][0]) / s, (r[2][0] + r[0][2]) / s, (r[2][1] + r[1][2]) / s, 0.25 * s
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    return [x / n, y / n, z / n, w / n]


def quat_to_m3(q):
    x, y, z, w = q
    return ((1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)),
            (2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)),
            (2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)))


def _selftest():
    """The three sign conventions this file would silently get wrong."""
    def close(a, b, tol=1e-9):
        return all(abs(x - y) < tol for x, y in zip(a, b))
    if not close(v_xform3((1, 0, 0), rot_axis((0, 0, 1), 90.0)), (0, 1, 0)):
        fail("rot_axis is not right-handed in the row-vector convention")
    if not close(v_xform3((1, 0, 0), rot_ypr(0, 90, 0)), (0, 1, 0)):
        fail("rot_ypr's yaw does not take +X to +Y")
    if not close(v_xform3((1, 0, 0), rot_ypr(90, 0, 0)), (0, 0, 1), 1e-9):
        fail("rot_ypr's pitch is not nose-up")
    if not close(v_xform3((0, 1, 0), rot_ypr(0, 0, 90)), (0, 0, -1), 1e-9):
        fail("rot_ypr's roll does not match FRotator's (roll 90 must take +Y to -Z)")
    m = rot_ypr(11.0, -23.0, 7.0)
    back = quat_to_m3(m3_to_quat(m))
    if max(abs(back[r][c] - m[r][c]) for r in range(3) for c in range(3)) > 1e-9:
        fail("m3_to_quat and quat_to_m3 disagree — a pose would be written to the GLB rotated")


# =============================================================================
# glTF / GLB reading
# =============================================================================
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
_CTYPE = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
          5125: ("I", 4), 5126: ("f", 4)}


def glb_load(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"glTF":
        raise ValueError("{0} is not a binary glTF".format(path))
    off, js, bin_ = 12, None, b""
    while off < len(raw):
        ln, ty = struct.unpack_from("<II", raw, off)
        off += 8
        if ty == 0x4E4F534A:
            js = json.loads(raw[off:off + ln].decode("utf-8"))
        elif ty == 0x004E4942:
            bin_ = raw[off:off + ln]
        off += ln
    return js, bin_


def accessor(js, bin_, idx):
    a = js["accessors"][idx]
    n = _NCOMP[a["type"]]
    fmt, size = _CTYPE[a["componentType"]]
    bv = js["bufferViews"][a["bufferView"]]
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride") or n * size
    return [struct.unpack_from("<" + fmt * n, bin_, base + k * stride) for k in range(a["count"])]


def node_local(nd, linear=True):
    """A glTF node's local transform as a row-vector 4x4.

    `linear=False` reproduces railgun_glb_to_obj.py's PISTOL rule, which drops
    every node rotation.  That is not what glTF means and that file says so —
    but the pistol's committed .uassets were baked that way, so measuring its
    landmarks any other way would describe a mesh that is not in the project.
    """
    if "matrix" in nd:
        m = nd["matrix"]                                  # column-major
        if not linear:
            return (M3_ID, (m[12], m[13], m[14]))
        return (((m[0], m[1], m[2]), (m[4], m[5], m[6]), (m[8], m[9], m[10])),
                (m[12], m[13], m[14]))
    t = tuple(nd.get("translation", (0.0, 0.0, 0.0)))
    if not linear:
        return (M3_ID, t)
    r = quat_to_m3(nd.get("rotation", [0.0, 0.0, 0.0, 1.0]))
    s = nd.get("scale")
    if s:
        r = tuple(tuple(r[i][c] * s[i] for c in range(3)) for i in range(3))
    return (r, t)


def node_parents(nodes):
    par = {}
    for i, nd in enumerate(nodes):
        for c in nd.get("children", []):
            par[c] = i
    return par


def node_worlds(nodes, linear=True, override=None):
    """Every node's world transform (row-vector), parent-first."""
    par = node_parents(nodes)
    out = {}

    def walk(i):
        if i in out:
            return out[i]
        loc = (override or {}).get(i) or node_local(nodes[i], linear)
        out[i] = loc if i not in par else m4_mul(loc, walk(par[i]))
        return out[i]

    for i in range(len(nodes)):
        walk(i)
    return out


# G (glTF metres, Y-up) <-> C (UE centimetres, Z-up).  C = (G.x, G.z, G.y)*100,
# i.e. the axis permutation S that swaps indices 1 and 2, times 100.  A rotation
# transforms as S*R*S, which for a permutation is "swap rows 1,2 and columns
# 1,2" — det is preserved, so it is still a rotation.
def g2c_point(p):
    return (p[0] * 100.0, p[2] * 100.0, p[1] * 100.0)


def c2g_point(p):
    return (p[0] / 100.0, p[2] / 100.0, p[1] / 100.0)


def _swap12(m):
    order = (0, 2, 1)
    return tuple(tuple(m[order[r]][order[c]] for c in range(3)) for r in range(3))


def g2c_rot(m):
    return _swap12(m)


def c2g_rot(m):
    return _swap12(m)


def g2c(x):
    return (g2c_rot(x[0]), g2c_point(x[1]))


def c2g(x):
    return (c2g_rot(x[0]), c2g_point(x[1]))


# =============================================================================
# THE ARMS RIG
# =============================================================================
FINGERS = ("thumb", "index", "middle", "ring", "pinky")


class Arms(object):
    """SK_TraceArms' bind pose and skin, in C (UE component centimetres)."""

    def __init__(self, path):
        self.js, self.bin = glb_load(path)
        self.nodes = self.js["nodes"]
        self.parent = node_parents(self.nodes)
        self.by_name = {}
        for i, nd in enumerate(self.nodes):
            if "name" in nd:
                self.by_name[nd["name"]] = i
        self.skin = self.js["skins"][0]
        self.joints = list(self.skin["joints"])
        self.jslot = {j: k for k, j in enumerate(self.joints)}
        worlds = node_worlds(self.nodes)
        self.bind = {i: g2c(worlds[i]) for i in self.joints}
        self.children = {}
        for j in self.joints:
            p = self.parent.get(j)
            if p is not None:
                self.children.setdefault(p, []).append(j)

        prim = self.js["meshes"][0]["primitives"][0]
        attrs = prim["attributes"]
        self.pos = [g2c_point(p) for p in accessor(self.js, self.bin, attrs["POSITION"])]
        self.inf = [[] for _ in self.pos]
        for jset, wset in (("JOINTS_0", "WEIGHTS_0"), ("JOINTS_1", "WEIGHTS_1")):
            if jset not in attrs:
                continue
            jj = accessor(self.js, self.bin, attrs[jset])
            ww = accessor(self.js, self.bin, attrs[wset])
            for v in range(len(self.pos)):
                for slot, w in zip(jj[v], ww[v]):
                    if w > 0.0:
                        self.inf[v].append((self.joints[slot], float(w)))

    def idx(self, name):
        if name not in self.by_name:
            raise KeyError("SK_TraceArms has no bone {0!r}".format(name))
        return self.by_name[name]

    def subtree(self, name):
        out, stack = [], [self.idx(name)]
        while stack:
            k = stack.pop()
            out.append(k)
            stack.extend(self.children.get(k, ()))
        return out

    def pos_of(self, W, name):
        return W[self.idx(name)][1]

    def axis_of(self, W, name, k):
        return W[self.idx(name)][0][k]

    def fresh(self):
        return dict(self.bind)

    def rotate(self, W, bone, rot3, pivot=None):
        """Rotate a bone and everything under it, about `pivot` (its own origin
        by default).  The ONE primitive every pose below is built out of."""
        piv = self.pos_of(W, bone) if pivot is None else pivot
        A = about(rot3, piv)
        for j in self.subtree(bone):
            W[j] = m4_mul(W[j], A)

    def skin_points(self, W):
        """The 336 (well, 1340 split) skinned vertices, C space."""
        cache = {j: m4_mul(m4_inv(self.bind[j]), W[j]) for j in self.joints}
        out = []
        for v, p in enumerate(self.pos):
            acc = (0.0, 0.0, 0.0)
            for j, w in self.inf[v]:
                acc = v_add(acc, v_mul(m4_point(p, cache[j]), w))
            out.append(acc)
        return out

    def finger_radius(self, bone, threshold=0.35):
        """Half-thickness of the flesh around one finger bone, MEASURED.

        The FURTHEST any vertex this bone owns at least `threshold` of sits from
        the bone's own axis.  Used as the stand-off the wrap solver curls each
        finger down to, so "the fingers touch the grip" is the mesh's own
        thickness and not a number somebody liked — and the OUTERMOST vertex is
        the right one, because a mean would leave the far half of a low-poly
        finger inside the gun.
        """
        b = self.bind[self.idx(bone)]
        origin, axis = b[1], v_norm(b[0][2])              # local +Z runs down the bone in C
        ds = []
        for v, p in enumerate(self.pos):
            for j, w in self.inf[v]:
                if j == self.idx(bone) and w >= threshold:
                    d = v_sub(p, origin)
                    ds.append(v_len(v_sub(d, v_mul(axis, v_dot(d, axis)))))
        return max(ds) if ds else 0.0


def assert_ring_consistent(arms):
    """The owner's bug, re-checked on the asset this file is about to pose.

    import_hands.py fixes it in the bind pose; if a rebuild ever stops doing
    that, every pose below would come out with one finger standing up, and the
    frames would be the first place anyone noticed.  This is cheaper.
    """
    W = arms.bind
    worst = {}
    for side in ("left", "right"):
        d = 0.0
        for j in range(3):
            ring = v_norm(arms.axis_of(W, "ring_{0}_{1}".format(side, j), 0))
            mid = v_norm(arms.axis_of(W, "middle_{0}_{1}".format(side, j), 0))
            d = max(d, math.degrees(math.acos(max(-1.0, min(1.0, v_dot(ring, mid))))))
        worst[side] = d
        if d > 30.0:
            fail("the {0} ring chain's flexion axis is {1:.1f} deg from the middle finger's on "
                 "SK_TraceArms. import_hands.py::apply_ring_roll_fix is not reaching the asset; "
                 "every pose here would curl that finger the wrong way.".format(side, d))
    return worst


# =============================================================================
# THE WEAPONS
# =============================================================================
class Weapon(object):
    """One weapon's geometry in its OWN mesh centimetres, plus the transform
    that puts it in rig space."""

    def __init__(self, name, glb, root_name, linear, scale, origin_rig,
                 rot_rig=M3_ID, anim=None):
        self.name = name
        self.scale = scale
        self.origin = origin_rig
        self.rot = rot_rig
        js, bin_ = glb_load(glb)
        nodes = js["nodes"]
        override = {}
        if anim:
            override = self._anim_override(js, bin_, nodes, anim)
        worlds = node_worlds(nodes, linear, override)
        root = next(i for i, n in enumerate(nodes) if n.get("name") == root_name)
        rt = worlds[root][1]

        def to_mesh(p):
            x, y, z = (p[0] - rt[0]) * 100.0, (p[1] - rt[1]) * 100.0, (p[2] - rt[2]) * 100.0
            return (-z, x, y)          # UE.X = -gl.z, UE.Y = gl.x, UE.Z = gl.y

        self.nodes = {}
        self.node_tris = {}
        self.tris = []
        for i, nd in enumerate(nodes):
            if nd.get("mesh") is None:
                continue
            W = worlds[i]
            pts = []
            mine = []
            for prim in js["meshes"][nd["mesh"]]["primitives"]:
                pos = accessor(js, bin_, prim["attributes"]["POSITION"])
                base = [to_mesh(m4_point(p, W)) for p in pos]
                pts.extend(base)
                if "indices" in prim:
                    idx = [t[0] for t in accessor(js, bin_, prim["indices"])]
                else:
                    idx = list(range(len(base)))
                for k in range(0, len(idx) - 2, 3):
                    mine.append((base[idx[k]], base[idx[k + 1]], base[idx[k + 2]]))
            self.node_tris[nd.get("name", "node%d" % i)] = mine
            self.tris.extend(mine)
            self.nodes[nd.get("name", "node%d" % i)] = pts

    @staticmethod
    def _anim_override(js, bin_, nodes, anim_name):
        """Sample one glTF animation at t = 0 and return per-node locals.

        The balisong's REFERENCE pose is the SHUT knife — TraceKnifeView.cpp's
        EnsureBladeBuilt says it: "an unanimated blade is a SHUT blade, and the
        knife is carried open".  Idle_Open is what swings the handles out, so
        the OPEN knife is the only pose worth posing a hand against, and it only
        exists inside that clip.
        """
        anim = next(a for a in js["animations"] if a.get("name") == anim_name)
        out = {}
        for ch in anim["channels"]:
            node = ch["target"]["node"]
            path = ch["target"]["path"]
            samp = anim["samplers"][ch["sampler"]]
            vals = accessor(js, bin_, samp["output"])
            base = out.get(node) or node_local(nodes[node])
            if path == "rotation":
                out[node] = (quat_to_m3(list(vals[0])), base[1])
            elif path == "translation":
                out[node] = (base[0], tuple(vals[0]))
        return out

    def to_rig(self, p):
        return v_add(v_xform3(v_mul(p, self.scale), self.rot), self.origin)

    def dir_to_rig(self, d):
        return v_norm(v_xform3(d, self.rot))

    def rig_points(self, node):
        return [self.to_rig(p) for p in self.nodes[node]]

    def rig_tris(self):
        return [tuple(self.to_rig(v) for v in t) for t in self.tris]

    def rig_node_tris(self):
        return {n: [tuple(self.to_rig(v) for v in t) for t in ts]
                for n, ts in self.node_tris.items()}

    def bounds(self):
        pts = [p for pts in self.nodes.values() for p in pts]
        return ([min(p[i] for p in pts) for i in range(3)],
                [max(p[i] for p in pts) for i in range(3)])


def build_weapons():
    pistol_origin = v_sub(VIEWMODEL_RIGHT_HAND, v_mul(RAILGUN_GRIP_LOCAL, RAILGUN_SCALE))
    smg_origin = v_sub(VIEWMODEL_RIGHT_HAND, v_mul(SMG_GRIP_LOCAL, SMG_SCALE))
    knife_rot = rot_ypr(*KNIFE_HELD_CANT)
    return {
        "pistol": Weapon("pistol", PISTOL_GLB, "railgun", False, RAILGUN_SCALE, pistol_origin),
        "smg": Weapon("smg", SMG_GLB, "railgun_smg", True, SMG_SCALE, smg_origin),
        "knife": Weapon("knife", KNIFE_GLB, "butterfly_knife", True, KNIFE_SCALE,
                        KNIFE_REST_LOCATION, knife_rot, anim="Idle_Open"),
    }


def assert_weapon_placement(weapons, lines):
    """Each gun's muzzle must land where TraceCharacterInternal.h says it does.

    That comment quotes 24.5 uu for the pistol and 27.6 for the SMG — the one
    number this file and that file state independently, so it is the one that
    catches a drift between them.
    """
    for name, local, want in (("pistol", RAILGUN_MUZZLE_LOCAL, 24.5),
                              ("smg", SMG_MUZZLE_LOCAL, 27.6)):
        got = weapons[name].to_rig(local)[0]
        lines.append("  {0:<7} muzzle at rig x {1:6.2f} uu (TraceCharacterInternal.h says {2:.1f})"
                     .format(name, got, want))
        if abs(got - want) > 0.15:
            fail("{0}: muzzle lands at rig x {1:.2f}, not the {2:.1f} uu "
                 "TraceCharacterInternal.h documents — the two files have drifted apart"
                 .format(name, got, want))


# =============================================================================
# Point / triangle distance — the only geometry primitive the wrap solver needs
# =============================================================================
def point_tri_dist2(p, tri):
    a, b, c = tri
    ab = v_sub(b, a)
    ac = v_sub(c, a)
    ap = v_sub(p, a)
    d1, d2 = v_dot(ab, ap), v_dot(ac, ap)
    if d1 <= 0.0 and d2 <= 0.0:
        q = a
    else:
        bp = v_sub(p, b)
        d3, d4 = v_dot(ab, bp), v_dot(ac, bp)
        if d3 >= 0.0 and d4 <= d3:
            q = b
        else:
            vc = d1 * d4 - d3 * d2
            if vc <= 0.0 <= d1 and d3 <= 0.0:
                q = v_add(a, v_mul(ab, d1 / (d1 - d3) if d1 != d3 else 0.0))
            else:
                cp = v_sub(p, c)
                d5, d6 = v_dot(ab, cp), v_dot(ac, cp)
                if d6 >= 0.0 and d5 <= d6:
                    q = c
                else:
                    vb = d5 * d2 - d1 * d6
                    if vb <= 0.0 <= d2 and d6 <= 0.0:
                        q = v_add(a, v_mul(ac, d2 / (d2 - d6) if d2 != d6 else 0.0))
                    else:
                        va = d3 * d6 - d5 * d4
                        if va <= 0.0 and (d4 - d3) >= 0.0 and (d5 - d6) >= 0.0:
                            den = (d4 - d3) + (d5 - d6)
                            q = v_add(b, v_mul(v_sub(c, b), (d4 - d3) / den if den else 0.0))
                        else:
                            den = va + vb + vc
                            if den <= 0.0:
                                q = a
                            else:
                                q = v_add(a, v_add(v_mul(ab, vb / den), v_mul(ac, vc / den)))
    d = v_sub(p, q)
    return v_dot(d, d)


class TriSoup(object):
    """A weapon's triangles with a cheap sphere reject, in rig space."""

    def __init__(self, tris):
        self.tris = tris
        self.centre = []
        self.radius = []
        self.normal = []
        for t in tris:
            c = v_mul(v_add(v_add(t[0], t[1]), t[2]), 1.0 / 3.0)
            self.centre.append(c)
            self.radius.append(max(v_len(v_sub(v, c)) for v in t))
            self.normal.append(v_norm(v_cross(v_sub(t[1], t[0]), v_sub(t[2], t[0]))))

    def near(self, p, cutoff):
        """A sub-soup of everything within `cutoff` of p — built once per grip
        so the binary search below is not O(all triangles) per step."""
        keep = [i for i in range(len(self.tris))
                if v_len(v_sub(self.centre[i], p)) <= cutoff + self.radius[i]]
        return TriSoup([self.tris[i] for i in keep])

    def hits(self, p, direction):
        """How many of these triangles a ray from p crosses.  Moller-Trumbore."""
        n = 0
        for t in self.tris:
            e1 = v_sub(t[1], t[0])
            e2 = v_sub(t[2], t[0])
            h = v_cross(direction, e2)
            a = v_dot(e1, h)
            if abs(a) < 1e-12:
                continue
            f = 1.0 / a
            s = v_sub(p, t[0])
            u = f * v_dot(s, h)
            if u < 0.0 or u > 1.0:
                continue
            q = v_cross(s, e1)
            v = f * v_dot(direction, q)
            if v < 0.0 or u + v > 1.0:
                continue
            if f * v_dot(e2, q) > 1e-9:
                n += 1
        return n

    def dist(self, p, best=1e18):
        b2 = best * best
        for i, t in enumerate(self.tris):
            dc = v_len(v_sub(self.centre[i], p)) - self.radius[i]
            if dc > 0.0 and dc * dc >= b2:
                continue
            d2 = point_tri_dist2(p, t)
            if d2 < b2:
                b2 = d2
        return math.sqrt(b2)

    def min_dist(self, pts):
        best = 1e18
        for p in pts:
            best = min(best, self.dist(p, best))
        return best


# Three directions, deliberately irrational-looking, so a ray cannot run down an
# edge of an axis-aligned box and be counted twice or not at all.  The majority
# vote makes a single unlucky ray harmless.
_PARITY_RAYS = (v_norm((0.7431, 0.2113, 0.6349)),
                v_norm((-0.3117, 0.8329, 0.4561)),
                v_norm((0.5233, -0.6011, 0.6041)))


class Solids(object):
    """The weapon as a SET of closed parts, which is what it actually is.

    *** WHY NOT "IS THE VERTEX BEHIND ITS NEAREST TRIANGLE". ***  That was the
    first version and it was wrong in a way worth recording: the pistol's coils
    are thin square RINGS, so a point sitting comfortably outside a coil but past
    its edge takes its sign from a face pointing the other way and is reported
    2 uu inside a gun it is nowhere near.  Every pose "failed" by exactly that
    amount.

    Each NODE of these weapons is a closed solid on its own (they are built out
    of boxes and rings), so ray parity against ONE node at a time is exact, and
    a point is inside the weapon when it is inside any single part.  Parity
    across the whole soup would not work — a point inside two overlapping boxes
    crosses an even number of faces and would read as outside.
    """

    def __init__(self, node_tris):
        self.parts = []
        for name, tris in node_tris.items():
            if not tris:
                continue
            pts = [v for t in tris for v in t]
            lo = tuple(min(p[i] for p in pts) for i in range(3))
            hi = tuple(max(p[i] for p in pts) for i in range(3))
            self.parts.append((name, TriSoup(tris), lo, hi))

    def depth(self, p):
        """(name, depth) of the deepest part p is inside, or (None, 0.0)."""
        best, who = 0.0, None
        for name, soup, lo, hi in self.parts:
            if any(p[i] < lo[i] or p[i] > hi[i] for i in range(3)):
                continue
            votes = sum(1 for d in _PARITY_RAYS if soup.hits(p, d) % 2 == 1)
            if votes < 2:
                continue
            d = soup.dist(p)
            if d > best:
                best, who = d, name
        return who, best


# =============================================================================
# *** WHERE THE ARMS SIT — the C -> R placement, and it is a deliverable. ***
#
# The imported rig is a BODY rig: T-posed, hands 142.83 uu apart, standing at
# Z 136.7..161.1 uu, with a hand 1.95x the size of the one the first-person
# camera is framed around (import_hands.py measures all four numbers every run).
# Two of those measurements fix this transform and the third is a choice:
#
#   SCALE, MEASURED.  0.5126 = 9.14 / 17.83, the ratio of the SHIPPED hands'
#   hand_right -> index_right_2 span to this rig's.  At that scale the fingers
#   are exactly the length the pistol's 34 uu and the SMG's 41.1 uu were framed
#   against, so a grip that closes here closes on the gun the player sees.  It
#   is stored as the ratio, not as 0.5126, so a re-export follows.
#
#   YAW, MEASURED.  The rig lays its arms along component +/-X (hand_left at
#   +71.4, hand_right at -71.4) and rig space wants them along +/-Y, so the
#   whole component is yawed -90: comp +X (the character's left) -> rig -Y.
#
#   SHOULDERS, CHOSEN, AND CHEATED FORWARD ON PURPOSE.  An anatomical shoulder
#   sits ~10 uu behind the eye; the eye is 56 uu behind ViewModelRoot
#   (ViewModelRestLocation.X) and the grip is another 0.8 in front of that, so an
#   anatomically placed shoulder is ~66 uu from the grip while this rig's arm is
#   57.9 * 0.5126 = 29.7 uu long.  The arm cannot reach, and growing it to reach
#   would double the hands.  Every first-person view model resolves that the same
#   way and so does this one: the shoulders are cheated forward until the arm
#   reaches with a plausible bend, and they are then OFF THE BOTTOM OF THE FRAME
#   (checked below), so nothing anybody sees is anatomically wrong.
# =============================================================================
ARMS_SCALE = 9.14 / 17.83                # pack hand / these arms, hand -> index tip
ARMS_YAW = -90.0
SHOULDER_MID_RIG = (-11.0, 0.0, -16.0)   # midpoint of the two upperarm joints, rig space


class ArmsPlacement(object):
    def __init__(self, arms, scale=ARMS_SCALE, yaw=ARMS_YAW, shoulder_mid=SHOULDER_MID_RIG):
        self.scale = scale
        self.rot = rot_axis((0.0, 0.0, 1.0), yaw)
        mid = v_mul(v_add(arms.pos_of(arms.bind, "upperarm_left"),
                          arms.pos_of(arms.bind, "upperarm_right")), 0.5)
        self.trans = v_sub(shoulder_mid, v_xform3(v_mul(mid, scale), self.rot))

    def to_rig(self, p):
        return v_add(v_xform3(v_mul(p, self.scale), self.rot), self.trans)

    def to_comp(self, p):
        return v_mul(v_xform3(v_sub(p, self.trans), m3_t(self.rot)), 1.0 / self.scale)

    def dir_to_comp(self, d):
        return v_norm(v_xform3(d, m3_t(self.rot)))

    def dir_to_rig(self, d):
        return v_norm(v_xform3(d, self.rot))

    def as_dict(self):
        return {"scale": self.scale, "yaw_deg": ARMS_YAW,
                "translation_rig": [round(c, 4) for c in self.trans]}


# =============================================================================
# THE POSE TABLE — the knobs, and the only place a frame sends you back to
#
# Everything below is bracketable: the numbers here are what the renders were
# judged on, and each one says what moving it does.  Directions are unit-ish
# vectors in the WEAPON'S OWN mesh frame (X toward the muzzle, Y right, Z up),
# because that is the frame the landmark they describe was measured in.
# =============================================================================
# WHERE THE ELBOWS WANT TO BE, in rig space: behind the shoulder, further out
# than it, and well below the gun — which is where a first-person player's
# elbows are, and off the bottom corners of the frame.  See two_bone_ik for why
# this is a point rather than a direction.
ELBOW_RIGHT = (-20.0, 20.0, -30.0)
ELBOW_LEFT = (-20.0, -20.0, -30.0)

POSES = {
    "Pistol": {
        "weapon": "pistol",
        "right": {
            # THE GRIP POST.  `grip` is the pistol's own node: mesh
            # X -34.40..-25.60, Y +/-4.20, Z -32.50..-7.00, i.e. a vertical
            # slab.  `along` orients its principal axis so it runs INDEX-TO-
            # PINKY, which down a pistol grip is downward.
            "post": "grip", "along": (0.0, 0.0, -1.0),
            # PALM DIRECTION: from the back of the hand THROUGH the palm, at the
            # grip.  The hand goes on the OPPOSITE side (-palm), so a right-
            # handed hold — hand on the gun's right flank, heel of the palm on
            # the backstrap — faces the palm LEFT and a little forward.  It also
            # sets which way the metacarpals run: wrist-to-knuckles comes out
            # perpendicular to this and to the grip, so more -Y here is more
            # "knuckles point at the target", which is what a pistol hold is.
            "palm": (0.30, -0.95, 0.0),
            # WHERE DOWN THE POST THE MIDDLE KNUCKLE SITS, 0 = top.  The index
            # knuckle lands one knuckle-spacing above it; too high and the hand
            # ends up inside the receiver, which sits directly on top of this
            # grip (measured: rig Z -4.51..-0.28 against the grip's top at -4.23).
            "u": 0.45,
            "pad": 1.0,           # knuckle stand-off, in measured finger radii
        },
        "left": {
            # The pistol's own `foregrip` node: mesh X 26.50..33.50, Y +/-3.60,
            # Z -22.00..-4.50 — a second vertical post, and the point
            # RailgunLeftHandLocal was authored against.  The support hand takes
            # it the same way the strong hand takes the grip, mirrored.
            "post": "foregrip", "along": (0.0, 0.0, -1.0),
            "palm": (0.25, 0.97, 0.0),
            "u": 0.45,
            "pad": 1.0,
        },
        "trigger": "trigger",
        "pole_right": ELBOW_RIGHT,
        "pole_left": ELBOW_LEFT,
    },
    "Smg": {
        "weapon": "smg",
        "right": {
            "post": "grip", "along": (0.0, 0.0, -1.0),   # the cloud's own axis is raked 18 deg
            "palm": (0.30, -0.95, 0.0),
            "u": 0.40,
            "pad": 1.0,
        },
        "left": {
            # *** THE MAGWELL, AND IT IS A MEASUREMENT THAT PICKED IT. ***
            # This gun has no foregrip node.  Its handguard is the coil stack,
            # and measured in rig space that is 8.5 uu across and 8.0 uu tall
            # while this hand's knuckle row is 3.6 uu and its fingers reach
            # 3.5 uu — a hand cannot close on it, it can only be parked beside
            # it, which is the "hand floating near the gun" defect.  The
            # magazine and its well ARE a post: 7.7 uu long, 4.2 x 2.4 uu in
            # section, exactly the shape the strong hand's grip is.  A magwell
            # hold is also what an SMG is held by.
            "post": "cell_magazine", "along": (0.0, 0.0, -1.0),
            "post_extra": ("cell_well",),
            "palm": (0.25, 0.97, 0.0),        # right and slightly forward
            "u": 0.30,
            "pad": 1.0,
        },
        "trigger": "trigger",
        "pole_right": ELBOW_RIGHT,
        "pole_left": ELBOW_LEFT,
    },
    "Knife": {
        "weapon": "knife",
        "right": {
            # THE SAFE HANDLE, in the OPEN pose.  A balisong is held by one
            # handle and everything else turns around the pin at its end; the
            # `_safe` chain is the one without the edge on it (TraceKnifeView.cpp
            # names the same bone).  Held in a hammer grip: the handle lies down
            # the channel the curled fingers make, so the pivot — the guard, and
            # the widest part of the knife — stands clear of the index knuckle.
            # MEASURED in the OPEN pose: both handles lie BEHIND the pivot
            # (mesh X -10.6..3.6) and the blade in front of it (X -1.0..12.6),
            # so the thing a fist closes on is the two handle plates together —
            # a bar 14.2 cm long and 1.6 x 2.8 cm in section.  The four fingers
            # stack ALONG it, index nearest the pivot, so index-to-pinky runs
            # toward the pommel: mesh -X.
            "post": "handle_safe", "along": (-1.0, 0.0, 0.0),
            "post_extra": ("handle_bite", "handle_cap_safe", "handle_cap_bite"),
            # SABRE GRIP.  See the swing note in place_hand: at 0 this handle
            # would lie on the knuckle line and the blade would point across the
            # screen instead of where the gun barrels do.
            "swing": 50.0,
            # Palm facing left and a little up: the knife rides on the thumb
            # side of the fist with the edge (mesh -Z) clear underneath, and the
            # blade stands above the knuckles instead of through them.
            "palm": (0.10, -0.86, 0.50),
            # 0.45 down a 14.2 cm handle puts the fist on its middle, which is
            # the same thing Trace.Knife.HoldGrip's 0.5 buys on the pack rig:
            # the pivot — the guard, and the widest part of a balisong — ends up
            # clear of the fingertips rather than inside them.
            "u": 0.45,
            "pad": 1.0,
        },
        "left": None,                # one-handed; the off hand is posed clear
        "trigger": None,
        "pole_right": ELBOW_RIGHT,
        "pole_left": ELBOW_LEFT,
        # The left arm has nothing to hold, so it is given a rest attitude
        # instead of an IK target: dropped and swung out, so it leaves the frame
        # at the bottom-left instead of hanging in shot beside the knife.
        # (bone, axis in RIG space, degrees) applied in order.
        "rest": [("upperarm_left", (0.0, 1.0, 0.0), -42.0),
                 ("upperarm_left", (1.0, 0.0, 0.0), 18.0),
                 ("forearm_left", (0.0, 1.0, 0.0), -38.0)],
    },
    # *** THE RING-FINGER PROOF, AND IT IS AN ASSET RATHER THAN A SCREENSHOT. ***
    #
    # The owner's complaint was "the ring fingers bend opposite the rest".  The
    # fix is in the BIND POSE (Scripts/import_hands.py::apply_ring_roll_fix), so
    # the thing that proves it is a closed fist: every finger of both hands given
    # THE SAME curl about its own local X, with no per-finger correction of any
    # kind anywhere in this file.  If the ring finger were still inverted it
    # would stand straight up out of the fist, which is exactly what
    # frames-D29H-IMPORT/03-right-hand-curled.png showed before the fix.
    "Fist": {
        "weapon": None,
        "trigger": None,
        "pole_right": ELBOW_RIGHT,
        "pole_left": ELBOW_LEFT,
        "free_curl": 88.0,
        # Both fists side by side in front of the lens, knuckle rows across the
        # frame and palms DOWN, so all four fingers of both hands are folded
        # under the knuckles and visible at once.  An inverted ring finger
        # cannot hide in that view: it would stand up out of the back of the
        # hand, which is exactly what the before picture shows.
        "right": {"free": True, "axis": (0.0, 1.0, 0.0), "palm": (0.0, 0.0, -1.0),
                  "anchor": (9.0, 5.5, -11.0)},
        "left": {"free": True, "axis": (0.0, -1.0, 0.0), "palm": (0.0, 0.0, -1.0),
                 "anchor": (9.0, -5.5, -11.0)},
    },
}

# Per-joint share of a finger's curl.  A real finger does not bend equally at
# all three joints; the proximal takes the most and the distal the least.  The
# wrap solver scales this vector, it does not re-shape it.
CURL_SHAPE = (1.0, 1.05, 0.75)
# How far a skin vertex may end up behind the weapon's surface before the pose
# is called wrong.  0.25 uu is a quarter of a finger radius on this rig — the
# amount a low-poly finger's flat facet cuts a corner by when its medial axis is
# correctly one radius out, and visible in a frame at about twice that.
PENETRATION_LIMIT = 0.25
# How near the surface a vertex has to be before "inside or outside" is asked at
# all.  See the note where it is used.
PENETRATION_PROBE = 2.0
# How far a hand may be slid out along its own palm normal to clear a
# penetration before the cure is worse than the disease.  See solve_pose.
BACKOFF_MAX = 0.8
# Past this, a hand inside the gun is visible in a frame and the run fails
# rather than shipping it.
PENETRATION_FAIL = 1.2
CURL_MAX = 105.0                 # degrees at the proximal joint before it folds through itself
# The curl a relaxed closed hand already has, before any weapon is involved.
# The wrap solver starts here and moves outward until the geometry stops it —
# see the note in wrap_finger for why starting from a flat hand cannot work.
FIST_START = 55.0
THUMB_SHAPE = (0.55, 0.85, 0.75)
THUMB_MAX = 70.0
THUMB_START = 45.0
# Which hand's index finger goes on the trigger.  Both guns are right-handed
# here (RailgunOrigin and SmgOrigin are both derived from ViewModelRightHand),
# and the support hand has no business near the trigger guard.
TRIGGER_HAND = "right"


# =============================================================================
# THE SOLVER
# =============================================================================
def principal_axis(points, hint):
    """The long axis of a point cloud, oriented to agree with `hint`.

    Power iteration on the covariance — three lines, no numpy, and the cloud is
    never more than a couple of hundred points.
    """
    n = float(len(points))
    mean = v_mul([sum(p[i] for p in points) for i in range(3)], 1.0 / n)
    cov = [[sum((p[r] - mean[r]) * (p[c] - mean[c]) for p in points) / n
            for c in range(3)] for r in range(3)]
    v = v_norm(hint)
    for _ in range(64):
        v = v_norm(tuple(sum(cov[r][c] * v[c] for c in range(3)) for r in range(3)))
    if v_dot(v, hint) < 0.0:
        v = v_mul(v, -1.0)
    return v, tuple(mean)


def hand_frame(arms, W, side):
    """(knuckle, long, palm) — the hand's own anatomy, measured, not typed.

    knuckle  index MCP -> pinky MCP.  The tube a fist closes around; the same
             axis TraceKnifeView.cpp builds the pack rig's hold out of.
    long     wrist -> middle MCP, down the back of the hand.
    palm     their cross product, SIGNED BY THE THUMB.  An opposable thumb is
             on the palm side of the hand by definition, so thumb tip minus
             middle knuckle settles the sign off the rig's own anatomy.

    *** WHY NOT SIGN IT WITH A CURL PROBE, WHICH IS WHAT THIS DID FIRST. ***
    Rotating a finger and taking the side the tip moves to defines the palm as
    "wherever +X curls", which makes curl_sign() below a tautology (it always
    answered +1) and got the frame's HANDEDNESS wrong on the right hand — which
    came out as a pose whose wrist-to-knuckle axis pointed backwards, i.e. a hand
    on the gun the wrong way round.  The thumb cannot lie about which side the
    palm is on.
    """
    kn = v_norm(v_sub(arms.pos_of(W, "pinky_{0}_0".format(side)),
                      arms.pos_of(W, "index_{0}_0".format(side))))
    lng = v_norm(v_sub(arms.pos_of(W, "middle_{0}_0".format(side)),
                       arms.pos_of(W, "hand_{0}".format(side))))
    lng = v_norm(v_sub(lng, v_mul(kn, v_dot(lng, kn))))
    palm = v_norm(v_cross(kn, lng))
    thumbward = v_sub(arms.pos_of(W, "thumb_{0}_2".format(side)),
                      arms.pos_of(W, "middle_{0}_0".format(side)))
    if v_dot(thumbward, palm) < 0.0:
        palm = v_mul(palm, -1.0)
    return kn, lng, palm


def curl_sign(arms, W, side):
    """+1 or -1: which sign of rotation about a finger bone's local +X curls it
    INTO the palm on this side.

    A real measurement now that hand_frame's palm comes from the thumb: rotate
    the middle finger a little and see whether the tip goes toward the palm or
    away from it.  It is per-hand because the two hands are mirrored, and it is
    measured because the ring-roll fix in import_hands.py could in principle be
    joined by a source re-export that flips the convention again."""
    _kn, _lng, palm = hand_frame(arms, W, side)
    tip = arms.pos_of(W, "middle_{0}_2".format(side))
    probe = dict(W)
    for j in range(3):
        bone = "middle_{0}_{1}".format(side, j)
        arms.rotate(probe, bone, rot_axis(arms.axis_of(probe, bone, 0), 20.0))
    travel = v_sub(arms.pos_of(probe, "middle_{0}_2".format(side)), tip)
    return 1.0 if v_dot(travel, palm) > 0.0 else -1.0


def two_bone_ik(arms, W, side, target, pole_point):
    """Put hand_<side> on `target`, elbow reaching toward `pole_point`.

    *** THE POLE IS A PLACE, NOT A DIRECTION, AND THAT IS NOT A STYLE CHOICE. ***
    A direction has to be projected perpendicular to the shoulder-to-hand axis
    to be usable, and on a first-person hold that axis points from a shoulder at
    the edge of the frame to a hand in the middle of it — very nearly ANTI-
    parallel to any sensible "elbow goes down and out".  The perpendicular part
    of it is then a rounding error pointing wherever, and the measured result was
    a right elbow 6 uu to the LEFT of the body's centre line, through the chest.
    A point is stable: it is projected too, but what survives is the part of
    "toward that place" that the arm can actually do.

    The chain carries a twist bone in each segment (upperarm_*_twist,
    forearm_*_twist); they are left at their bind locals and ride along, which is
    what a twist bone is for.  Returns the extension as a fraction of the arm's
    own length so a hyperextended arm is a number and not a surprise in a frame.
    """
    a = arms.pos_of(W, "upperarm_{0}".format(side))
    e = arms.pos_of(W, "forearm_{0}".format(side))
    h = arms.pos_of(W, "hand_{0}".format(side))
    l1, l2 = v_len(v_sub(e, a)), v_len(v_sub(h, e))
    d = v_len(v_sub(target, a))
    reach = d / (l1 + l2)
    d = max(abs(l1 - l2) + 1e-3, min(l1 + l2 - 1e-3, d))
    u = v_norm(v_sub(target, a))
    toward = v_sub(pole_point, a)
    perp = v_sub(toward, v_mul(u, v_dot(toward, u)))
    if v_len(perp) < 1e-6:
        perp = v_cross(u, (0.0, 0.0, 1.0))
    perp = v_norm(perp)
    cos_a = max(-1.0, min(1.0, (l1 * l1 + d * d - l2 * l2) / (2.0 * l1 * d)))
    ang = math.acos(cos_a)
    elbow = v_add(a, v_mul(v_add(v_mul(u, math.cos(ang)), v_mul(perp, math.sin(ang))), l1))

    arms.rotate(W, "upperarm_{0}".format(side),
                rot_between(v_sub(e, a), v_sub(elbow, a)))
    e2 = arms.pos_of(W, "forearm_{0}".format(side))
    h2 = arms.pos_of(W, "hand_{0}".format(side))
    arms.rotate(W, "forearm_{0}".format(side),
                rot_between(v_sub(h2, e2), v_sub(target, e2)))
    return reach


def place_hand(arms, W, side, spec, weapon, place, lines):
    """Close one hand on one post.  Returns the diagnostics the report prints.

    With `free` set there is no post: the knuckle axis, the palm direction and
    the anchor are given outright in rig space.  That is how the Fist pose puts
    two hands in front of the lens with nothing in them — the ring-finger proof
    needs a hand shaped by nothing but the curl.
    """
    if spec.get("free"):
        axis = v_norm(spec["axis"])
        _ = spec.get("pad")
        palm_r = v_norm(spec["palm"])
        palm_r = v_norm(v_sub(palm_r, v_mul(axis, v_dot(palm_r, axis))))
        anchor_rig = spec["anchor"]
        return _orient_hand(arms, W, side, spec, place, axis, palm_r, anchor_rig, "free", lines)

    post_pts = list(weapon.rig_points(spec["post"]))
    for extra in spec.get("post_extra", ()):
        post_pts.extend(weapon.rig_points(extra))
    axis_hint = weapon.dir_to_rig(spec["along"])
    # ACROSS THE WHOLE POST, extras included: an SMG handguard is four separate
    # coil nodes and a spine, and any one of them on its own has a principal
    # axis across the barrel rather than along it.
    axis, centre = principal_axis(post_pts, axis_hint)
    palm_r = weapon.dir_to_rig(spec["palm"])
    palm_r = v_norm(v_sub(palm_r, v_mul(axis, v_dot(palm_r, axis))))

    # Where down the post, and how far off its surface, the middle knuckle goes.
    ts = [v_dot(v_sub(p, centre), axis) for p in post_pts]
    t0 = min(ts) + spec["u"] * (max(ts) - min(ts))
    band = [p for p, t in zip(post_pts, ts) if abs(t - t0) <= 0.35 * (max(ts) - min(ts))] or post_pts
    half = max(-v_dot(v_sub(p, centre), palm_r) for p in band)     # surface on the hand's side
    # THE STAND-OFF IS THE HAND'S OWN THICKNESS, not a typed clearance.  The
    # knuckle the anchor places is a BONE; the palm's flesh is between it and the
    # weapon, and on this rig that flesh is one measured finger radius deep.  A
    # pose that put the bone on the surface would bury the palm in the grip,
    # which is what the first run measured (1.1 uu of hand inside the SMG's).
    pad = spec["pad"] * spec["_flesh"]
    anchor_rig = v_add(v_add(centre, v_mul(axis, t0)), v_mul(palm_r, -(half + pad)))
    return _orient_hand(arms, W, side, spec, place, axis, palm_r, anchor_rig,
                        spec["post"], lines)


def _orient_hand(arms, W, side, spec, place, axis, palm_r, anchor_rig, label, lines):
    # ---- the hand's target attitude, in component space ----------------------
    #
    # *** SWING: WHICH LINE ACROSS THE FIST THE POST LIES ON. ***  Borrowed
    # wholesale from Trace.Knife.HoldSwing, which had to answer this same
    # question for the pack hands and is worth not re-deriving:
    #
    #   0    HAMMER GRIP.  The post lies along the KNUCKLE LINE — index knuckle
    #        at one end, pinky at the other.  This is how a pistol grip is held
    #        and it is what both guns use.
    #   ~50  SABRE GRIP.  The post lies diagonally across the fist, from the heel
    #        of the palm to the web of the thumb.  A knife whose blade is meant
    #        to point where the forearm points HAS to be held this way: the
    #        knuckle line and the metacarpals are perpendicular by anatomy, so a
    #        handle laid on the knuckle line alone comes out across the screen.
    #
    # Only the SOURCE frame's first axis changes; the palm still faces the post
    # and the handedness is still the rig's own, so the hand stays a hand.
    kn, lng, palm = hand_frame(arms, arms.bind, side)
    sw = math.radians(spec.get("swing", 0.0))
    a1 = v_norm(v_add(v_mul(kn, math.cos(sw)), v_mul(lng, math.sin(sw))))
    hand = 1.0 if v_dot(v_cross(kn, lng), palm) > 0.0 else -1.0
    a2 = v_mul(v_cross(palm, a1), hand)
    t_kn = place.dir_to_comp(axis)
    t_palm = place.dir_to_comp(palm_r)
    t_palm = v_norm(v_sub(t_palm, v_mul(t_kn, v_dot(t_palm, t_kn))))
    t_lng = v_mul(v_cross(t_palm, t_kn), hand)
    src = (a1, a2, palm)
    dst = (t_kn, t_lng, t_palm)
    delta = m3_mul(m3_t(src), dst)

    anchor_c = place.to_comp(anchor_rig)
    off = v_sub(arms.pos_of(arms.bind, "middle_{0}_0".format(side)),
                arms.pos_of(arms.bind, "hand_{0}".format(side)))
    hand_target = v_sub(anchor_c, v_xform3(off, delta))

    reach = two_bone_ik(arms, W, side, hand_target, place.to_comp(spec["pole"]))
    cur = W[arms.idx("hand_{0}".format(side))][0]
    want = m3_mul(arms.bind[arms.idx("hand_{0}".format(side))][0], delta)
    arms.rotate(W, "hand_{0}".format(side), m3_mul(m3_t(cur), want))

    err = v_len(v_sub(arms.pos_of(W, "hand_{0}".format(side)), hand_target))
    long_rig = place.dir_to_rig(t_lng)
    lines.append("    {0:<5} hand on {1:<14} reach {2:5.1%}, IK residual {3:.3f} uu; wrist->"
                 "knuckles runs rig ({4:+.2f} {5:+.2f} {6:+.2f})"
                 .format(side, label, reach, err, *long_rig))
    if reach > 0.995:
        fail("the {0} arm is {1:.1%} extended reaching {2} — straighter than the arm is long"
             .format(side, reach, spec["post"]))
    if err > 0.05:
        fail("the {0} hand missed its target by {1:.3f} uu".format(side, err))
    return {"reach": reach, "anchor_rig": anchor_rig, "axis_rig": axis,
            "palm_rig": palm_r, "long_rig": long_rig, "post": label}


def wrap_finger(arms, W, side, finger, soup, radius, shape, cap, sign, start=0.0):
    """Curl one finger until its flesh touches the weapon, and no further.

    BINARY SEARCH ON CONTACT, not a typed angle.  The finger's medial axis is
    sampled along all three phalanges; the curl grows until the closest of those
    samples sits exactly `radius` — the MEASURED half-thickness of that finger —
    from the weapon's triangles.  If the finger never reaches the weapon it stops
    at `cap` instead, so a finger over thin air ends up in a natural relaxed
    curl rather than folded through itself.
    """
    bones = ["{0}_{1}_{2}".format(finger, side, j) for j in range(3)]

    def samples(state):
        # *** FROM THE MIDDLE PHALANX ONWARD, AND THAT IS THE WHOLE TRICK. ***
        # The PROXIMAL phalanx of a gripping finger LIES ON the thing it grips —
        # that is what a grip is — so including it in the stop test makes every
        # finger "already touching" at zero curl and the solver returns a hand
        # with four dead-straight fingers laid on the weapon.  (Measured: it did,
        # on every finger of all three poses.)  What has to wrap without going
        # through is everything past the first knuckle, so that is what is
        # sampled.  The proximal is still covered — by the whole-mesh
        # penetration check at the end, which sees every vertex.
        pts = []
        for j, b in enumerate(bones):
            if j == 0:
                continue
            p0 = arms.pos_of(state, b)
            nxt = bones[j + 1] if j + 1 < len(bones) else None
            p1 = arms.pos_of(state, nxt) if nxt else v_add(
                p0, v_mul(v_norm(arms.axis_of(state, b, 2)), 1.6 * radius))
            for k in range(0, 5):
                pts.append(v_add(p0, v_mul(v_sub(p1, p0), k / 4.0)))
        return pts

    def at(angle):
        state = dict(W)
        for j, b in enumerate(bones):
            arms.rotate(state, b, rot_axis(arms.axis_of(state, b, 0), sign * angle * shape[j]))
        return state

    if soup is None or not soup.tris:
        return at(start), start, float("nan")

    # *** SCANNED, NOT BISECTED, BECAUSE CLEARANCE IS NOT MONOTONE IN THE CURL.
    #
    # Two earlier versions failed here and both failures are worth keeping:
    #   * closing from ZERO stops instantly, because a gripping finger's
    #     PROXIMAL phalanx lies on the weapon from the start — it returned four
    #     dead-straight fingers laid across the grip, on every pose;
    #   * bisecting from a fist assumes "clear" is an interval, and it is not: a
    #     straight finger points THROUGH the trigger guard and the front of the
    #     frame, is clear again once it has curled past them, and is blocked
    #     again when it closes into the palm.  Bisection landed in the first
    #     blocked stretch and gave up at zero.
    #
    # So the whole range is sampled, the clear stretches are found, the one
    # nearest a relaxed fist is chosen, and the finger closes to the far end of
    # it — "curl until it touches, having got past whatever was in the way".
    step = 1.5
    n = int(cap / step) + 1
    angles = [k * step for k in range(n)]
    gaps = [soup.min_dist(samples(at(a))) for a in angles]
    runs, k = [], 0
    while k < n:
        if gaps[k] > radius:
            j = k
            while j + 1 < n and gaps[j + 1] > radius:
                j += 1
            runs.append((k, j))
            k = j + 1
        else:
            k += 1
    if not runs:
        # Nowhere on the finger's whole range is it out of the weapon: the HAND
        # is in the wrong place, which is an anchor problem and not a curl one.
        # Say so by leaving the finger where a fist would put it and letting the
        # whole-mesh check at the end fail the run with the real numbers.
        return at(start), start, gaps[min(range(n), key=lambda i: abs(angles[i] - start))]

    def cost(run):
        lo_a, hi_a = angles[run[0]], angles[run[1]]
        if lo_a <= start <= hi_a:
            return 0.0
        return min(abs(lo_a - start), abs(hi_a - start))

    run = min(runs, key=cost)
    # The far end of the chosen stretch, then a bisection against the next 1.5
    # degrees so the reported gap is a real contact and not a sampling artefact.
    lo_a = angles[run[1]]
    hi_a = min(cap, lo_a + step)
    for _ in range(12):
        mid = 0.5 * (lo_a + hi_a)
        if soup.min_dist(samples(at(mid))) > radius:
            lo_a = mid
        else:
            hi_a = mid
    state = at(lo_a)
    return state, lo_a, soup.min_dist(samples(state))


def place_trigger_finger(arms, W, side, trigger_pts, place, radius, sign, lines):
    """Put the index fingertip on the trigger's own face.

    The trigger is a real node in both guns (pistol mesh X -24.10..-22.90,
    Z -16.00..-11.00; SMG X -24.34..-22.26, Z -14.96..-10.23), so "the trigger
    finger is on the trigger" is a distance to a measured surface, not a look.

    TWO knobs, not one, and the second was earned: with curl alone the SMG's
    fingertip stopped 2.2 uu short, because that trigger sits forward of the
    grip's rake and a finger that can only curl in its own plane sweeps past it.
    A real index finger ABDUCTS at the knuckle to reach across; allowing the same
    +/-25 degrees at the MCP brings it onto the trigger.  Both are searched
    coarsely and then refined, because the distance is not convex in either.
    """
    bones = ["index_{0}_{1}".format(side, j) for j in range(3)]
    target = place.to_comp(tuple(sum(p[i] for p in trigger_pts) / len(trigger_pts)
                                 for i in range(3)))

    def at(angle, abduct):
        state = dict(W)
        arms.rotate(state, bones[0], rot_axis(arms.axis_of(state, bones[0], 1), abduct))
        for j, b in enumerate(bones):
            arms.rotate(state, b, rot_axis(arms.axis_of(state, b, 0),
                                           sign * angle * CURL_SHAPE[j]))
        return state

    def tip(state):
        p = arms.pos_of(state, bones[2])
        return v_add(p, v_mul(v_norm(arms.axis_of(state, bones[2], 2)), 1.1 * radius))

    best, best_a, best_b = None, 0.0, 0.0
    for step, span_a, span_b in ((4.0, (-20.0, 140.0), (-25.0, 25.0)),
                                 (0.5, None, None)):
        if span_a is None:
            span_a = (best_a - 6.0, best_a + 6.0)
            span_b = (best_b - 6.0, best_b + 6.0)
        a = span_a[0]
        while a <= span_a[1]:
            b = span_b[0]
            while b <= span_b[1]:
                d = v_len(v_sub(tip(at(a, b)), target))
                if best is None or d < best:
                    best, best_a, best_b = d, a, b
                b += step
            a += step
    state = at(best_a, best_b)
    err_uu = best * place.scale
    lines.append("    {0:<5} trigger finger: curl {1:5.1f} deg, abduct {2:+5.1f} deg, fingertip "
                 "{3:.2f} uu from the trigger's centre (finger radius {4:.2f} uu)"
                 .format(side, best_a, best_b, err_uu, radius * place.scale))
    return state, best_a, err_uu


def solve_pose(name, spec, arms, weapons, place, lines):
    """Solve one pose, BACKING THE HAND OFF until nothing is inside the weapon.

    The grip solver places the palm one measured flesh-thickness off the post
    and the fingers close until they touch, and that is right for the post
    itself — but a weapon is not just its grip.  A pistol's receiver sits
    directly on top of its grip and an SMG's magazine well sits directly on top
    of its magazine, so a hand correctly placed on the post can still have a
    thumb or a knuckle 0.7-1.4 uu inside the part ABOVE it (both measured, on
    the support hand of both guns).  Rather than hand-tune two more numbers per
    pose, the whole hand slides out along its own palm normal by the depth that
    was measured, and the fingers re-close from there.  It converges in one or
    two passes and it makes "nothing interpenetrates" true by construction
    instead of true by inspection.
    """
    weapon = weapons.get(spec.get("weapon"))
    lines.append("  {0}  ({1})".format(name, weapon.name if weapon else "no weapon"))
    extra_pad = {"right": 0.0, "left": 0.0}
    for attempt in range(4):
        W, out, deepest, deep_bone, _q = _solve_once(
            name, spec, arms, weapon, place, extra_pad)
        if deepest <= PENETRATION_LIMIT or deep_bone is None:
            break
        side = "left" if "_left" in deep_bone else "right"
        # HALF THE MEASURED DEPTH, AND NEVER MORE THAN BACKOFF_MAX IN TOTAL.
        # Backing a hand all the way out of a receiver it is only grazing costs
        # more than it buys: at the full depth the pistol's support hand came
        # off its foregrip by 2.2 uu and closed into a fist around thin air,
        # which is a worse picture than a thumb pressed 0.4 uu into the frame.
        # What is left over is REPORTED rather than hidden.
        extra_pad[side] = min(BACKOFF_MAX, extra_pad[side] + 0.5 * deepest + 0.05)
    if attempt:
        lines.append("    backed off after {0} pass(es): right +{1:.2f} uu, left +{2:.2f} uu "
                     "along the palm normal".format(attempt, extra_pad["right"],
                                                    extra_pad["left"]))
    for line in out["report"]:
        lines.append(line)
    if deepest > PENETRATION_LIMIT:
        lines.append("    STILL {0:.2f} uu of {1} inside {2} after the back-off — under the "
                     "{3:.2f} uu that shows in a frame, but it is there"
                     .format(deepest, deep_bone, out.get("deepest_penetration_part"),
                             PENETRATION_FAIL))
    if deepest > PENETRATION_FAIL:
        fail("{0}: {1} is {2:.2f} uu inside {3} (limit {4:.2f}) and backing the hand off did not "
             "clear it".format(name, deep_bone, deepest, out.get("deepest_penetration_part"),
                               PENETRATION_FAIL))
    return W, out


def _solve_once(name, spec, arms, weapon, place, extra_pad):
    lines = []
    W = arms.fresh()
    soup_all = TriSoup(weapon.rig_tris() if weapon else [])
    # THE WRAP SOLVER WORKS IN COMPONENT SPACE, because that is where the bones
    # are; the weapon lives in rig space.  Converting the triangles once here is
    # what stops the two frames being compared against each other — which they
    # were on the first run, and every finger duly reported an 85 uu gap to a
    # weapon 40 uu long and curled to its cap instead of onto the grip.
    soup_comp = TriSoup([tuple(place.to_comp(v) for v in t) for t in weapon.rig_tris()]
                        if weapon else [])

    out = {"weapon": weapon.name if weapon else None, "hands": {}, "fingers": {}}

    # ---- the two hands -------------------------------------------------------
    for side, key in (("right", "right"), ("left", "left")):
        s = spec.get(key)
        if s is None:
            continue
        s = dict(s)
        s["pole"] = spec["pole_{0}".format(side)]
        flesh = arms.finger_radius("middle_{0}_0".format(side)) * place.scale
        s["_flesh"] = flesh
        s["pad"] = s.get("pad", 1.0) + extra_pad[side] / flesh
        out["hands"][side] = place_hand(arms, W, side, s, weapon, place, lines)

    for bone, axis_rig, deg in spec.get("rest") or ():
        arms.rotate(W, bone, rot_axis(place.dir_to_comp(axis_rig), deg))

    # ---- the fingers ---------------------------------------------------------
    for side in ("right", "left"):
        if spec.get(side) is None and not spec.get("rest"):
            continue
        sign = curl_sign(arms, arms.bind, side)
        holding = spec.get(side) is not None
        for finger in FINGERS:
            radius = max(arms.finger_radius("{0}_{1}_{2}".format(finger, side, j))
                         for j in range(3)) * place.scale
            shape = THUMB_SHAPE if finger == "thumb" else CURL_SHAPE
            cap = THUMB_MAX if finger == "thumb" else CURL_MAX
            if not holding:
                soup = None
            elif finger == "index" and spec.get("trigger") and side == TRIGGER_HAND:
                W2, ang, err = place_trigger_finger(
                    arms, W, side, weapon.rig_points(spec["trigger"]), place,
                    radius / place.scale, sign, lines)
                W.update(W2)
                out["fingers"]["{0}_{1}".format(finger, side)] = {
                    "curl_deg": ang, "trigger_err_uu": err}
                continue
            else:
                probe = arms.pos_of(W, "{0}_{1}_0".format(finger, side))
                soup = soup_comp.near(probe, 12.0 / place.scale)
            W2, ang, gap = wrap_finger(
                arms, W, side, finger, soup, radius / place.scale, shape, cap, sign,
                start=(spec.get("free_curl", FIST_START)
                       * (THUMB_START / FIST_START if finger == "thumb" else 1.0)))
            W.update(W2)
            out["fingers"]["{0}_{1}".format(finger, side)] = {
                "curl_deg": ang, "gap_uu": None if gap != gap else gap * place.scale}

    for side in ("right", "left"):
        got = [(f, out["fingers"].get("{0}_{1}".format(f, side))) for f in FINGERS]
        if not any(g for _f, g in got):
            continue
        lines.append("    {0:<5} curl/gap: {1}".format(
            side, "  ".join("{0} {1:.0f}d/{2}".format(
                f[:2], g["curl_deg"],
                "trig" if "trigger_err_uu" in g else
                ("-" if g.get("gap_uu") is None else "{0:.2f}".format(g["gap_uu"])))
                for f, g in got if g)))

    # ---- what it came out like ----------------------------------------------
    #
    # THE CHECK THE OWNER'S "not through it" ACTUALLY NEEDS.  Every skinned
    # vertex against the weapon's whole triangle soup: the closest is expected to
    # be ~0 (a grip that touches is the point), and what must not happen is a
    # vertex ending up BEHIND the surface it is nearest to.
    pts = [place.to_rig(p) for p in arms.skin_points(W)]
    solids = Solids(weapon.rig_node_tris() if weapon else {})
    nearest, deepest, deep_part, touching = 1e18, 0.0, None, 0
    deep_bone = None
    for i, p in enumerate(pts):
        d = soup_all.dist(p)
        nearest = min(nearest, d)
        if d <= 0.5:
            touching += 1
        if d > PENETRATION_PROBE:
            continue                       # far outside; nothing to ask
        part, depth = solids.depth(p)
        if depth > deepest:
            deepest, deep_part = depth, part
            # WHICH BONE, because "0.6 uu inside the receiver" is a fact and
            # "the thumb is 0.6 uu inside the receiver" is an instruction.
            owner = max(arms.inf[i], key=lambda t: t[1])[0] if arms.inf[i] else None
            deep_bone = arms.nodes[owner]["name"] if owner is not None else None
    out["closest_vertex_uu"] = nearest
    out["deepest_penetration_uu"] = deepest
    out["deepest_penetration_part"] = deep_part
    out["deepest_penetration_bone"] = deep_bone
    out["vertices_touching"] = touching
    if weapon is None:
        lines.append("    no weapon in this pose, so nothing to touch or go through")
    else:
        lines.append("    skin vs weapon: closest vertex {0:.3f} uu, {1} of {2} vertices within "
                     "0.5 uu (contact), deepest penetration {3:.3f} uu ({4} into {5})"
                     .format(nearest, touching, len(pts), deepest,
                             deep_bone or "-", deep_part or "nothing"))
    out["report"] = lines
    lo = [min(p[i] for p in pts) for i in range(3)]
    hi = [max(p[i] for p in pts) for i in range(3)]
    out["bounds_rig"] = [lo, hi]
    lines.append("    arms occupy rig X {0:7.1f}..{1:7.1f}  Y {2:7.1f}..{3:7.1f}  "
                 "Z {4:7.1f}..{5:7.1f} uu".format(lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    for tag in ("hand_right", "hand_left", "index_right_2", "thumb_right_2"):
        out.setdefault("bones_rig", {})[tag] = [round(c, 3) for c in
                                                place.to_rig(arms.pos_of(W, tag))]
    return W, out, deepest, deep_bone, None


# =============================================================================
# Writing: the poses GLB (animations) and one baked preview GLB per pose
# =============================================================================
def local_from_world(arms, W):
    """World (C) -> per-node local (G), the inverse of what Arms.__init__ did."""
    out = {}
    for j in arms.joints:
        p = arms.parent.get(j)
        wg = c2g(W[j])
        pg = c2g(W[p]) if p is not None and p in W else None
        out[j] = wg if pg is None else m4_mul(wg, m4_inv(pg))
    return out


def write_poses_glb(arms, solved, path):
    """TraceArms.glb + one two-key animation per pose.

    The mesh, the skin, the joints and the inverse bind matrices are the ones
    already on disk, byte for byte — this only APPENDS animations, so the
    sequences cannot drift from the SkeletalMesh they are meant to play on.
    """
    sys.path.insert(0, SCRIPT_DIR)
    from trace_glb import _Buf, _pad4, JSON_CHUNK, BIN_CHUNK

    js = json.loads(json.dumps(arms.js))          # deep copy
    buf = _Buf()
    buf.data = arms.bin
    buf.views = list(js["bufferViews"])
    buf.accessors = list(js["accessors"])

    times = [0.0, 1.0 / 30.0]
    t_acc = buf.add(b"".join(struct.pack("<f", t) for t in times), len(times), 5126,
                    "SCALAR", None, ([times[0]], [times[-1]]))

    anims = []
    for name in sorted(solved):
        W = solved[name]["world"]
        locals_ = local_from_world(arms, W)
        channels, samplers = [], []
        for j in arms.joints:
            q = m3_to_quat(locals_[j][0])
            t = locals_[j][1]
            for path_name, vals in (("rotation", [q, q]),
                                    ("translation", [list(t), list(t)])):
                acc = buf.vec4f(vals) if path_name == "rotation" else buf.vec3(
                    [tuple(v) for v in vals])
                samplers.append({"input": t_acc, "output": acc, "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1,
                                 "target": {"node": j, "path": path_name}})
        anims.append({"name": name, "channels": channels, "samplers": samplers})

    js["animations"] = anims
    js["bufferViews"] = buf.views
    js["accessors"] = buf.accessors
    js["buffers"] = [{"byteLength": len(buf.data)}]
    raw = _pad4(json.dumps(js, separators=(",", ":")).encode("utf-8"), b" ")
    bn = _pad4(buf.data, b"\x00")
    blob = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(raw) + 8 + len(bn))
    blob += struct.pack("<II", len(raw), JSON_CHUNK) + raw
    blob += struct.pack("<II", len(bn), BIN_CHUNK) + bn
    with open(path, "wb") as handle:
        handle.write(blob)
    return len(blob), len(anims)


def write_baked_glb(arms, W, path):
    """The same pose baked into a BIND pose, as a second, independent witness.

    A commandlet cannot render, and a SkeletalMeshActor carrying an AnimSequence
    is one more thing that has to work before a frame means anything.  A mesh
    whose REST pose is the pose renders with nothing playing at all, so the
    photograph and the animation asset fail independently: if they ever disagree
    the frames say so instead of both being wrong in the same way.
    """
    sys.path.insert(0, SCRIPT_DIR)
    from trace_glb import _Buf, _pad4, JSON_CHUNK, BIN_CHUNK

    js = json.loads(json.dumps(arms.js))
    locals_ = local_from_world(arms, W)
    for j in arms.joints:
        nd = js["nodes"][j]
        nd.pop("matrix", None)
        nd["rotation"] = m3_to_quat(locals_[j][0])
        nd["translation"] = list(locals_[j][1])

    buf = _Buf()
    buf.data = arms.bin
    buf.views = list(js["bufferViews"])
    buf.accessors = list(js["accessors"])
    prim = js["meshes"][0]["primitives"][0]
    attrs = prim["attributes"]

    pts = arms.skin_points(W)
    gl = [c2g_point(p) for p in pts]
    attrs["POSITION"] = buf.vec3(gl, minmax=True)
    # Normals ride the same blend the positions do, so a baked pose is lit like
    # the posed rig would be rather than like the T-pose it came from.
    nrm = accessor(arms.js, arms.bin, arms.js["meshes"][0]["primitives"][0]
                   ["attributes"]["NORMAL"])
    cache = {j: m4_mul(m4_inv(arms.bind[j]), W[j]) for j in arms.joints}
    out_n = []
    for v, n in enumerate(nrm):
        nc = g2c_point(n)
        acc = (0.0, 0.0, 0.0)
        for j, w in arms.inf[v]:
            acc = v_add(acc, v_mul(v_xform3(nc, cache[j][0]), w))
        out_n.append(c2g_point(v_norm(acc)))
    attrs["NORMAL"] = buf.vec3(out_n)
    js["skins"][0]["inverseBindMatrices"] = buf.mat4(
        [_flat16(m4_inv(c2g(W[j]))) for j in arms.joints])

    js["bufferViews"] = buf.views
    js["accessors"] = buf.accessors
    js["buffers"] = [{"byteLength": len(buf.data)}]
    js.pop("animations", None)
    raw = _pad4(json.dumps(js, separators=(",", ":")).encode("utf-8"), b" ")
    bn = _pad4(buf.data, b"\x00")
    blob = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(raw) + 8 + len(bn))
    blob += struct.pack("<II", len(raw), JSON_CHUNK) + raw
    blob += struct.pack("<II", len(bn), BIN_CHUNK) + bn
    with open(path, "wb") as handle:
        handle.write(blob)
    return len(blob)


def _flat16(m):
    """Row-vector 4x4 -> glTF's column-major 16 floats.  M_col = M_row^T, and
    'columns of the transpose' are 'rows of the original', so the sixteen floats
    are the same sixteen floats — the note import_hands.py's m_mul carries."""
    r, t = m
    return [r[0][0], r[0][1], r[0][2], 0.0,
            r[1][0], r[1][1], r[1][2], 0.0,
            r[2][0], r[2][1], r[2][2], 0.0,
            t[0], t[1], t[2], 1.0]


# =============================================================================
# STAGE 1 — build
# =============================================================================
def build():
    _selftest()
    lines = []
    if not os.path.isfile(ARMS_GLB):
        fail("{0} is missing — run Scripts/import-hands.sh first".format(ARMS_GLB))
        return lines, {}
    arms = Arms(ARMS_GLB)
    lines.append("ARMS  {0}".format(os.path.relpath(ARMS_GLB, PROJECT_ROOT)))
    lines.append("  {0} bones, {1} split vertices".format(len(arms.joints), len(arms.pos)))
    ring = assert_ring_consistent(arms)
    lines.append("  ring vs middle flexion axis: left {0:.1f} deg, right {1:.1f} deg  "
                 "(import_hands.py fixed this in the bind pose; > 30 fails the run)"
                 .format(ring["left"], ring["right"]))

    weapons = build_weapons()
    lines.append("")
    lines.append("WEAPONS, placed exactly as TraceCharacterLayout places them:")
    assert_weapon_placement(weapons, lines)
    for n in ("pistol", "smg", "knife"):
        lo, hi = weapons[n].bounds()
        lines.append("  {0:<7} {1} nodes, {2} triangles, mesh bounds X {3:7.1f}..{4:7.1f} cm"
                     .format(n, len(weapons[n].nodes), len(weapons[n].tris), lo[0], hi[0]))

    place = ArmsPlacement(arms)
    lines.append("")
    lines.append("PLACEMENT  scale {0:.4f}, yaw {1:.0f}, translation rig ({2:.2f} {3:.2f} {4:.2f})"
                 .format(place.scale, ARMS_YAW, *place.trans))
    for b in ("upperarm_right", "upperarm_left", "hand_right", "hand_left"):
        p = place.to_rig(arms.pos_of(arms.bind, b))
        lines.append("  bind {0:<16} -> rig ({1:7.2f} {2:7.2f} {3:7.2f}) uu".format(b, *p))

    lines.append("")
    lines.append("POSES:")
    solved = {}
    for name in sorted(POSES):
        W, info = solve_pose(name, POSES[name], arms, weapons, place, lines)
        info["world"] = W
        solved[name] = info

    os.makedirs(OUT_DIR, exist_ok=True)
    size, n = write_poses_glb(arms, solved, POSES_GLB)
    lines.append("")
    lines.append("WROTE  {0}  ({1} bytes, {2} animations)"
                 .format(os.path.relpath(POSES_GLB, PROJECT_ROOT), size, n))
    for name in sorted(solved):
        p = os.path.join(OUT_DIR, "TraceArmsPose_{0}.glb".format(name))
        b = write_baked_glb(arms, solved[name]["world"], p)
        lines.append("       {0}  ({1} bytes, bind pose = the pose)"
                     .format(os.path.relpath(p, PROJECT_ROOT), b))

    manifest = {
        "arms_glb": os.path.relpath(ARMS_GLB, PROJECT_ROOT),
        "placement": place.as_dict(),
        "ring_vs_middle_deg": ring,
        # The hierarchy, so the import stage can walk a bone up to the root
        # without asking a Skeleton, which cannot answer (import_hands.py's note).
        "bone_parents": {arms.nodes[j]["name"]:
                         (arms.nodes[arms.parent[j]]["name"] if j in arms.parent else None)
                         for j in arms.joints},
        "poses": {},
    }
    for name in sorted(solved):
        info = dict(solved[name])
        W = info.pop("world")
        info["bone_pose_comp"] = {
            arms.nodes[j]["name"]: {"pos": [round(c, 4) for c in W[j][1]],
                                    "x": [round(c, 5) for c in W[j][0][0]],
                                    "z": [round(c, 5) for c in W[j][0][2]]}
            for j in arms.joints}
        manifest["poses"][name] = info
    with open(POSES_MANIFEST, "w") as handle:
        json.dump(manifest, handle, indent=1, sort_keys=True, default=list)
    lines.append("       {0}".format(os.path.relpath(POSES_MANIFEST, PROJECT_ROOT)))
    return lines, manifest


# =============================================================================
# STAGE 2 — import (editor)
# =============================================================================
def _eal():
    return unreal.EditorAssetLibrary


def prop(obj, name, value):
    try:
        obj.set_editor_property(name, value)
        return True
    except Exception as exc:                          # noqa: BLE001
        fail("pipeline property {0} = {1!r} rejected: {2}".format(name, value, exc))
        return False


def build_pipeline(name, animations_only, skeleton=None):
    """A configured copy of the engine's glTF pipeline, on disk.

    OverridePipelines is a TArray<FSoftObjectPath>, so the override has to be a
    real asset — the shape import_pack.py, import_rocco.py and import_hands.py
    all use.  Deleted again at the end of the run.
    """
    path = "{0}/{1}".format(PIPELINE_DIR, name)
    if _eal().does_asset_exist(path):
        _eal().delete_asset(path)
    src = unreal.load_asset(DEFAULT_GLTF_ASSETS_PIPELINE)
    if src is None:
        fail("engine pipeline {0} did not load".format(DEFAULT_GLTF_ASSETS_PIPELINE))
        return None
    pipe = _eal().duplicate_asset(DEFAULT_GLTF_ASSETS_PIPELINE, path)
    if pipe is None:
        fail("could not duplicate {0} -> {1}".format(DEFAULT_GLTF_ASSETS_PIPELINE, path))
        return None
    common = pipe.get_editor_property("common_skeletal_meshes_and_animations_properties")
    mesh_pipe = pipe.get_editor_property("mesh_pipeline")
    anim_pipe = pipe.get_editor_property("animation_pipeline")
    prop(common, "import_only_animations", animations_only)
    if skeleton is not None:
        prop(common, "skeleton", skeleton)
    prop(mesh_pipe, "bone_influence_limit", 8)
    prop(mesh_pipe, "create_physics_asset", False)
    if anim_pipe is not None:
        prop(anim_pipe, "import_animations", True)
        prop(anim_pipe, "import_bone_tracks", True)
    _eal().save_asset(path)
    return path


def run_import(source_file, dest, pipeline_path):
    data = unreal.AutomatedAssetImportData()
    data.set_editor_property("destination_path", dest)
    data.set_editor_property("filenames", [source_file])
    data.set_editor_property("replace_existing", True)
    params = unreal.ImportAssetParameters()
    params.set_editor_property("is_automated", True)
    try:
        params.set_editor_property("override_pipelines",
                                   [unreal.SoftObjectPath(pipeline_path),
                                    unreal.SoftObjectPath(DEFAULT_GLTF_PIPELINE)])
    except Exception:                                 # noqa: BLE001
        params.set_editor_property("override_pipelines",
                                   [pipeline_path, DEFAULT_GLTF_PIPELINE])
    src = unreal.InterchangeManager.get_interchange_manager_scripted()
    src_data = unreal.InterchangeManager.create_source_data(source_file)
    src.import_asset(dest, src_data, params)


def landed(folder):
    out = {}
    if not _eal().does_directory_exist(folder):
        return out
    for path in _eal().list_assets(folder, recursive=True, include_folder=False):
        out[path.split(".")[0]] = unreal.load_asset(path)
    return out


def do_import():
    lines = []
    if not os.path.isfile(POSES_GLB):
        fail("{0} is missing — run the build stage first".format(POSES_GLB))
        return lines
    with open(POSES_MANIFEST) as handle:
        manifest = json.load(handle)

    skeleton = unreal.load_asset(ARMS_SKELETON)
    if skeleton is None:
        fail("{0} did not load — run Scripts/import-hands.sh".format(ARMS_SKELETON))
        return lines

    for folder in (STAGING, POSE_DIR, PREVIEW_DIR):
        if _eal().does_directory_exist(folder):
            _eal().delete_directory(folder)

    # ---- the animations ------------------------------------------------------
    pipe = build_pipeline("TraceArmsPoseAnim", True, skeleton)
    if pipe is None:
        return lines
    run_import(POSES_GLB, STAGING, pipe)
    got = landed(STAGING)
    seqs = sorted(p for p, a in got.items() if isinstance(a, unreal.AnimSequence))
    lines.append("IMPORT — animations:")
    for p, a in sorted(got.items()):
        lines.append("  landed  {0:<66} {1}".format(p, type(a).__name__ if a else "<none>"))
    want = sorted(manifest["poses"])
    if len(seqs) != len(want):
        fail("expected {0} AnimSequences from {1}, got {2}"
             .format(len(want), os.path.basename(POSES_GLB), len(seqs)))
        return lines

    named = {}
    for src in seqs:
        stem = src.rsplit("/", 1)[-1]
        pose = next((w for w in want if stem.endswith(w) or w in stem), None)
        if pose is None:
            fail("cannot tell which pose {0} is".format(src))
            continue
        dst = "{0}/A_TraceArms_{1}".format(POSE_DIR, pose)
        if _eal().does_asset_exist(dst):
            _eal().delete_asset(dst)
        if not _eal().rename_asset(src, dst):
            fail("could not rename {0} -> {1}".format(src, dst))
            continue
        named[pose] = dst
    if _eal().does_directory_exist(STAGING):
        _eal().delete_directory(STAGING)

    # ---- the previews (throwaway, /Game/Trace/Temp) --------------------------
    lines.append("")
    lines.append("IMPORT — baked previews (throwaway, they are the render subject):")
    for pose in want:
        src_file = os.path.join(OUT_DIR, "TraceArmsPose_{0}.glb".format(pose))
        if not os.path.isfile(src_file):
            fail("{0} is missing".format(src_file))
            continue
        stage = "{0}/_stage_{1}".format(PREVIEW_DIR, pose)
        pipe2 = build_pipeline("TraceArmsPoseMesh_{0}".format(pose), False)
        if pipe2 is None:
            continue
        run_import(src_file, stage, pipe2)
        g = landed(stage)
        meshes = sorted(p for p, a in g.items() if isinstance(a, unreal.SkeletalMesh))
        if len(meshes) != 1:
            fail("preview {0}: expected one SkeletalMesh, got {1}".format(pose, len(meshes)))
            continue
        dst = "{0}/SK_TraceArmsPose_{1}".format(PREVIEW_DIR, pose)
        if _eal().does_asset_exist(dst):
            _eal().delete_asset(dst)
        _eal().rename_asset(meshes[0], dst)
        bind_shell(dst, lines, PREVIEW_MI)
        _eal().delete_directory(stage)
        lines.append("  {0}".format(dst))

    # ---- the two assertions that make any of this mean something ------------
    #
    # The DELIVERABLE is the AnimSequences; the PHOTOGRAPHS are of the baked
    # preview meshes.  Both are checked against the same solver output, so if
    # they ever disagree the frames stop being evidence for the animations and
    # the run says so, instead of the two quietly being different poses.
    parents = manifest["bone_parents"]
    lib, lib_name = _anim_library()
    lines.append("")
    lines.append("FRAME 0 OF EACH SEQUENCE, against the manifest the poses were solved into:")
    lines.append("  reading bone poses through unreal.{0}".format(lib_name or "<NOT AVAILABLE>"))
    if lib is None:
        fail("no AnimationLibrary/AnimationBlueprintLibrary with get_bone_pose_for_frame on this "
             "engine, so the imported AnimSequences are UNVERIFIED. The preview meshes below are "
             "still checked, but nothing proves the clips carry the same pose.")
    for pose in want:
        path = named.get(pose)
        if path is None or lib is None:
            continue
        anim = unreal.load_asset(path)
        if anim is None:
            fail("{0} did not load back".format(path))
            continue
        lines.append("  {0:<48} {1:.3f} s, {2} keys"
                     .format(path, anim.get_editor_property("sequence_length"),
                             anim.get_editor_property("number_of_sampled_keys")))
        worst, worst_bone = 0.0, ""
        for bone, ref in manifest["poses"][pose]["bone_pose_comp"].items():
            try:
                comp = anim_bone_component(lib, anim, bone, parents)
            except Exception as exc:                  # noqa: BLE001
                fail("cannot read {0} out of {1}: {2}".format(bone, path, exc))
                break
            d = max(abs(a - b) for a, b in zip(comp, ref["pos"]))
            if d > worst:
                worst, worst_bone = d, bone
        lines.append("     worst bone position error vs the solver: {0:.4f} uu ({1})"
                     .format(worst, worst_bone or "-"))
        if worst > 0.05:
            fail("{0}: frame 0 is {1:.3f} uu away from the pose the solver wrote ({2}). The "
                 "AnimSequence and the frames would be showing different poses."
                 .format(path, worst, worst_bone))

    lines.append("")
    lines.append("THE PREVIEW MESHES' REFERENCE POSES, against the same manifest:")
    for pose in want:
        mesh = unreal.load_asset("{0}/SK_TraceArmsPose_{1}".format(PREVIEW_DIR, pose))
        if mesh is None:
            fail("preview mesh for {0} did not load".format(pose))
            continue
        got = mesh_bone_positions(mesh)
        worst, worst_bone = 0.0, ""
        for bone, ref in manifest["poses"][pose]["bone_pose_comp"].items():
            if bone not in got:
                fail("preview {0} has no bone {1}".format(pose, bone))
                continue
            d = max(abs(a - b) for a, b in zip(got[bone], ref["pos"]))
            if d > worst:
                worst, worst_bone = d, bone
        lines.append("  {0:<48} worst {1:.4f} uu ({2})"
                     .format("SK_TraceArmsPose_" + pose, worst, worst_bone or "-"))
        if worst > 0.05:
            fail("SK_TraceArmsPose_{0}: its rest pose is {1:.3f} uu from the solved pose ({2}); "
                 "the frames would not be showing the pose that was solved."
                 .format(pose, worst, worst_bone))

    if _eal().does_directory_exist(PIPELINE_DIR):
        _eal().delete_directory(PIPELINE_DIR)
    _eal().save_directory(ROOT, only_if_is_dirty=False, recursive=True)
    if _eal().does_directory_exist(PREVIEW_DIR):
        _eal().save_directory(PREVIEW_DIR, only_if_is_dirty=False, recursive=True)
    return lines


def _anim_library():
    """UAnimationBlueprintLibrary, whichever name this engine exposes it under.

    5.x has shipped it as BOTH unreal.AnimationLibrary and
    unreal.AnimationBlueprintLibrary depending on the minor; the note in
    import_hands.py::prop about Interchange renaming things between minors
    applies here too.  Resolved once, reported, never assumed.
    """
    for name in ("AnimationLibrary", "AnimationBlueprintLibrary"):
        lib = getattr(unreal, name, None)
        if lib is not None and hasattr(lib, "get_bone_pose_for_frame"):
            return lib, name
    return None, None


def anim_bone_component(lib, anim, bone, parents):
    """A bone's COMPONENT-space position at frame 0 of `anim`.

    get_bone_pose_for_frame answers in the bone's PARENT'S space, which is not a
    number anything here can compare, so the chain is walked — the same thing
    TraceCharacterViewModel.cpp's RefPoseComponentSpace does, for the same
    reason.  The hierarchy comes out of the manifest rather than off the
    Skeleton, because a Skeleton's bone_tree gives names and nothing else from
    Python (import_hands.py::bone_table's note).
    """
    xf = unreal.Transform()
    cur = bone
    for _ in range(64):
        local = lib.get_bone_pose_for_frame(anim, cur, 0, False)
        xf = xf.multiply(local)
        cur = parents.get(cur)
        if not cur:
            break
    loc = xf.translation
    return (loc.x, loc.y, loc.z)


def mesh_bone_positions(mesh):
    """Every bone's component-space position on a SkeletalMesh.

    One SkeletalMeshActor is spawned, read and destroyed — the pattern
    import_hands.py::bone_table records as the only way to reach a reference
    pose from Python.  The level is never saved.
    """
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(unreal.SkeletalMeshActor,
                                             unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    if actor is None:
        fail("could not spawn a SkeletalMeshActor; no bones measured")
        return {}
    out = {}
    try:
        comp = actor.skeletal_mesh_component
        comp.set_skeletal_mesh_asset(mesh)
        for i in range(comp.get_num_bones()):
            name = str(comp.get_bone_name(i))
            t = comp.get_socket_transform(name, unreal.RelativeTransformSpace.RTS_COMPONENT)
            out[name] = (t.translation.x, t.translation.y, t.translation.z)
    except Exception as exc:                          # noqa: BLE001
        fail("bone read failed on {0}: {1}".format(mesh.get_path_name(), exc))
    finally:
        subsystem.destroy_actor(actor)
    return out


def bind_shell(mesh_path, lines, mi_path=None):
    """One slot, bound to the material the shipped hands wear, and READ BACK.

    get_editor_property('materials') hands back COPIES of the structs, so a loop
    that mutates them in place is a silent no-op — the trap import_pack.py's
    assign_materials fell into.  Each slot is written back BY INDEX and the asset
    is then re-loaded from disk before it is reported.
    """
    mi_path = mi_path or SHELL_MI
    mesh = unreal.load_asset(mesh_path)
    mi = unreal.load_asset(mi_path)
    if mesh is None or mi is None:
        fail("cannot bind {0}: mesh or {1} missing".format(mesh_path, mi_path))
        return
    slots = list(mesh.get_editor_property("materials"))
    for i, slot in enumerate(slots):
        slot.set_editor_property("material_interface", mi)
        slots[i] = slot
    mesh.set_editor_property("materials", slots)
    _eal().save_loaded_asset(mesh)
    reread = unreal.load_asset(mesh_path)
    got = [s.get_editor_property("material_interface") for s in reread.get_editor_property("materials")]
    if not got or any(g is None or g.get_path_name().split(".")[0] != mi_path for g in got):
        fail("{0}: material did not read back as {1}".format(mesh_path, mi_path))
    else:
        lines.append("    material -> {0}".format(mi_path))


# =============================================================================
def main():
    log("pose_hands.py stage={0}".format(STAGE))
    if STAGE == "build":
        lines, _manifest = build()
        log("")
        log("================ ARMS POSE BUILD REPORT ================")
        for line in lines:
            log(line)
        log("================ END REPORT ================")
        log("[pose-hands] BUILD EXIT={0}".format(1 if _failures else 0))
    elif STAGE == "import":
        lines = do_import()
        log("")
        log("================ ARMS POSE IMPORT REPORT ================")
        for line in lines:
            log(line)
        log("================ END REPORT ================")
        log("[pose-hands] EXIT={0}".format(1 if _failures else 0))
    else:
        fail("unknown TRACE_POSE_STAGE {0!r}; expected build or import".format(STAGE))
        log("[pose-hands] EXIT=1")

    if _failures:
        log("{0} PROBLEM(S):".format(len(_failures)))
        for f in _failures:
            log("  - {0}".format(f))
    else:
        log("no problems reported.")
    if unreal is None and _failures:
        raise SystemExit(1)


main()
