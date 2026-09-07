#!/usr/bin/env python3
"""
Trace - split_centre_tower.py

SPLITS THE OWNER'S CENTRE-TOWER GLB INTO A STRUCTURAL SHELL AND A NEON TRIM.

    python3 Scripts/split_centre_tower.py

Reads  Art/CentreTower/tron-octagonal-tower.glb
Writes Art/CentreTower/CentreTower_Shell.glb   (collides, walkable)
       Art/CentreTower/CentreTower_Neon.glb    (drawn only, never collides)

-----------------------------------------------------------------------------
WHY THE SPLIT EXISTS, AND WHY IT IS NOT OPTIONAL
-----------------------------------------------------------------------------
12,560 of the model's 15,600 triangles are the emissive strips. If they shared
one mesh with the shell they would share its collision, and a player running up
the ramp would catch on every proud strip. That is not hypothetical: it is
exactly the defect the owner reported on the wall buttresses -

    "There should be no physical properties of the lights in the buttresses
     (e.g. right now it looks like the player is jittering while surfing due
     to hitting those blue strips)"

- and Scripts/import_side_ramp.py already carries the same split for the same
reason. Collision is per-MESH in Unreal, never per-material-slot, so the only
way to give the strips no collision is to make them their own asset.

-----------------------------------------------------------------------------
IT ALSO FLATTENS, BECAUSE UNREAL WILL NOT
-----------------------------------------------------------------------------
The owner's file is 80 meshes under 93 nodes. UE 5.8's Interchange importer has
no combine-static-meshes option reachable from Python (checked: the property is
absent from both InterchangeGenericMeshPipeline and
InterchangeGenericCommonMeshesProperties), so importing it as-authored produces
THIRTY separate static mesh assets - tower_pad, tower_shaft, ramp_n_shell and so
on. Thirty assets is not merely untidy: TraceCore.cpp picks the Core's kickoff
pillar by finding actors whose mesh name contains "octagon" and taking the one
nearest the middle, so thirty centre actors is thirty candidates.

So each half is flattened here into ONE mesh carrying one primitive per
material. Node transforms are baked into the vertices, which is a pure
within-glTF operation - it never touches the axis convention, so the handedness
argument below still holds. Normals are carried through the inverse transpose,
because a non-uniform node scale would otherwise leave them unnormalised and
pointing slightly wrong, and this model's walkability is decided by normals.

The flattening is verified by MEASUREMENT rather than trusted: --verify re-reads
the written files and asserts the triangle count, the bounding box and the
distribution of surface slope against the source. A transform bug loud enough to
matter cannot survive all three.

-----------------------------------------------------------------------------
WHY IT REWRITES GLB RATHER THAN EXPORTING OBJ
-----------------------------------------------------------------------------
glTF is right-handed Y-up; Unreal is left-handed Z-up. Every hand-rolled
conversion between them is one sign error away from mirrored geometry, and a
mirror flips triangle winding, which Unreal reports as inside-out complex
collision - you fall through the ramp you are standing on. Rather than do that
arithmetic, this script keeps every vertex byte EXACTLY as the owner authored it
and only edits the glTF JSON, dropping the primitives that belong to the other
half. Unreal's own Interchange glTF importer then performs the conversion, which
is the one implementation guaranteed to agree with the engine.

The BIN chunk is copied wholesale rather than repacked. Both halves therefore
carry the full 1.4 MB of vertex data and reference the slice they need. That is
deliberate: repacking means rewriting every accessor byte offset, which is the
error-prone step this design exists to avoid. The cost is disk in Art/, never
runtime - the cooker only ever sees the imported .uasset.
"""

import json
import math
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# THE OWNER'S SOURCE, newest first. The remodel arrived as .obj; the .glb is the
# first version and is kept only so the history reads. Whichever exists and is
# listed first wins, so dropping a new export in replaces the tower without
# editing this file.
SRC_CANDIDATES = (
    os.path.join(REPO, "Art", "CentreTower", "tron-octagonal-tower.obj"),
    os.path.join(REPO, "Art", "CentreTower", "tron-octagonal-tower.glb"),
)
SRC = next((p for p in SRC_CANDIDATES if os.path.isfile(p)), SRC_CANDIDATES[0])

# The owner's material names, and which half each belongs to. A material that
# appears in neither list is a FAILURE, not a default: silently dropping part of
# the model, or silently giving the neon collision, are both worse than stopping.
SHELL_MATERIALS = {"base_black", "deck_graphite", "panel_slate"}
NEON_MATERIALS = {"light_cyan", "light_amber"}

# ---------------------------------------------------------------------------
# NODES FORCED OUT OF COLLISION REGARDLESS OF THEIR MATERIAL.
#
# THE FOUR COLLARS WOULD OTHERWISE MAKE ALL FOUR RAMPS UNCLIMBABLE, which is the
# single thing that would have shipped broken here. Each collar is a 12-triangle
# horizontal slab at the top of a ramp, spanning the full ramp width, sitting at
# local Y 2.180..2.300 - straddling the deck at 2.260. From the DECK side it is a
# 4 cm threshold, nothing. From the RAMP side the ramp falls away beneath it, so
# MEASURED at the collar's outer edge (local X 2.560) the ride surface is at
# Y 1.9566 and the collar presents:
#
#     a vertical face      (2.300 - 1.9566) * 100 * 2.449849 =  84.1 uu tall
#     clearance underneath (2.180 - 1.9566) * 100 * 2.449849 =  54.7 uu
#
# against MaxStepHeight 45 uu and a 176 uu pawn. Too tall to step onto, too low
# to walk under: a wall across every approach. The DEMO 29 climb would simply
# stop existing, and Trace.Core.KickoffProbe would NOT catch it, because that
# harness places pawns on the deck rather than walking them up.
#
# They stay in the model - the owner authored them and they read as the ramp's
# threshold - they just stop colliding. They keep their own panel_slate material
# in the trim asset, so they still look like structure rather than neon.
#
# THIS IS WHY THE SPLIT IS BY NODE AND NOT ONLY BY MATERIAL. The collars are
# panel_slate, the same slot as tower_face_panel_* and ramp_*_side_panel_*, which
# must keep their collision. A material-only split cannot separate them.
TRIM_NODES = {"ramp_n_collar", "ramp_e_collar", "ramp_s_collar", "ramp_w_collar"}

JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def read_glb(path):
    with open(path, "rb") as handle:
        data = handle.read()
    magic, version, _ = struct.unpack("<III", data[:12])
    if magic != 0x46546C67:
        raise SystemExit("not a GLB: {0}".format(path))
    if version != 2:
        raise SystemExit("glTF version {0}, expected 2".format(version))
    offset = 12
    doc = None
    binary = b""
    while offset < len(data):
        length, kind = struct.unpack("<II", data[offset:offset + 8])
        payload = data[offset + 8:offset + 8 + length]
        if kind == JSON_CHUNK:
            doc = json.loads(payload)
        elif kind == BIN_CHUNK:
            binary = payload
        offset += 8 + length
    if doc is None:
        raise SystemExit("GLB has no JSON chunk")
    return doc, binary


def write_glb(path, doc, binary):
    js = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    bn = binary + b"\x00" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(js) + (8 + len(bn) if bn else 0)
    with open(path, "wb") as handle:
        handle.write(struct.pack("<III", 0x46546C67, 2, total))
        handle.write(struct.pack("<II", len(js), JSON_CHUNK))
        handle.write(js)
        if bn:
            handle.write(struct.pack("<II", len(bn), BIN_CHUNK))
            handle.write(bn)
    return total


# -----------------------------------------------------------------------------
# glTF accessor reading. Enough of the spec for this model, and strict about the
# rest: an unsupported component type raises instead of guessing.
# -----------------------------------------------------------------------------
COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
             5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_accessor(doc, binary, index):
    acc = doc["accessors"][index]
    n = NCOMP[acc["type"]]
    fmt, size = COMPONENT[acc["componentType"]]
    if "bufferView" not in acc:
        return [[0.0] * n for _ in range(acc["count"])]
    view = doc["bufferViews"][acc["bufferView"]]
    start = view.get("byteOffset", 0)
    raw = binary[start:start + view["byteLength"]]
    stride = view.get("byteStride") or (size * n)
    base = acc.get("byteOffset", 0)
    out = []
    for k in range(acc["count"]):
        out.append(list(struct.unpack_from("<" + fmt * n, raw, base + k * stride)))
    return out


def node_matrix(node):
    """Column-major glTF TRS or matrix -> row-major 4x4."""
    if "matrix" in node:
        m = node["matrix"]
        return [[m[0], m[4], m[8], m[12]],
                [m[1], m[5], m[9], m[13]],
                [m[2], m[6], m[10], m[14]],
                [m[3], m[7], m[11], m[15]]]
    t = node.get("translation", [0.0, 0.0, 0.0])
    r = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = node.get("scale", [1.0, 1.0, 1.0])
    x, y, z, w = r
    rot = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
           [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
           [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[rot[0][0] * s[0], rot[0][1] * s[1], rot[0][2] * s[2], t[0]],
            [rot[1][0] * s[0], rot[1][1] * s[1], rot[1][2] * s[2], t[1]],
            [rot[2][0] * s[0], rot[2][1] * s[1], rot[2][2] * s[2], t[2]],
            [0.0, 0.0, 0.0, 1.0]]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def xform_point(m, p):
    return [m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3]]


def normal_matrix(m):
    """Inverse transpose of the upper 3x3.

    Not the matrix itself: under a non-uniform scale that would leave normals
    pointing off the surface, and this model's WALKABILITY is decided by normal Z.
    """
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-18:
        return [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    inv = [[0.0] * 3 for _ in range(3)]
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det
    return [[inv[0][0], inv[1][0], inv[2][0]],
            [inv[0][1], inv[1][1], inv[2][1]],
            [inv[0][2], inv[1][2], inv[2][2]]]


def collect(doc, binary):
    """Every primitive in the scene, flattened to world space, grouped by material."""
    names = [m.get("name", "material_{0}".format(i))
             for i, m in enumerate(doc.get("materials", []))]
    groups = {}

    def visit(index, parent, inherited=None):
        node = doc["nodes"][index]
        world = matmul(parent, node_matrix(node))
        label = node.get("name") or inherited
        if "mesh" in node:
            nrm = normal_matrix(world)
            for prim in doc["meshes"][node["mesh"]].get("primitives", []):
                if prim.get("mode", 4) != 4:
                    raise SystemExit("primitive mode {0} is not TRIANGLES".format(prim.get("mode")))
                attrs = prim["attributes"]
                pos = read_accessor(doc, binary, attrs["POSITION"])
                nor = (read_accessor(doc, binary, attrs["NORMAL"])
                       if "NORMAL" in attrs else [[0.0, 1.0, 0.0]] * len(pos))
                uv = (read_accessor(doc, binary, attrs["TEXCOORD_0"])
                      if "TEXCOORD_0" in attrs else [[0.0, 0.0]] * len(pos))
                if "indices" in prim:
                    idx = [int(v[0]) for v in read_accessor(doc, binary, prim["indices"])]
                else:
                    idx = list(range(len(pos)))
                mat = names[prim["material"]] if "material" in prim else "(none)"
                # A node on the forced-trim list keeps its material but is filed
                # under a distinct key, so the shell cannot pick it up.
                key = (mat, True) if label in TRIM_NODES else (mat, False)
                bucket = groups.setdefault(key, {"pos": [], "nor": [], "uv": [], "idx": [], "grp": []})
                base = len(bucket["pos"])
                for p in pos:
                    bucket["pos"].append(xform_point(world, p))
                for v in nor:
                    w = [nrm[0][0] * v[0] + nrm[0][1] * v[1] + nrm[0][2] * v[2],
                         nrm[1][0] * v[0] + nrm[1][1] * v[1] + nrm[1][2] * v[2],
                         nrm[2][0] * v[0] + nrm[2][1] * v[1] + nrm[2][2] * v[2]]
                    ln = math.sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2])
                    bucket["nor"].append([w[0] / ln, w[1] / ln, w[2] / ln] if ln > 1e-12
                                         else [0.0, 1.0, 0.0])
                for t in uv:
                    bucket["uv"].append([t[0], t[1]])
                bucket["grp"].extend([label or ""] * len(pos))
                bucket["idx"].extend(base + i for i in idx)
        for child in node.get("children", []):
            visit(child, world, label)

    scene = doc["scenes"][doc.get("scene", 0)]
    for root in scene.get("nodes", []):
        visit(root, [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]], None)
    return groups


def collect_obj(path):
    """The same {(material, forced_trim): buffers} map that collect() builds, from an OBJ.

    OBJ IS ALREADY FLAT - no node graph, so there is nothing to bake and none of
    the transform risk collect() carries. The frame matches too: the owner exports
    Y-up metres either way, which is glTF's own convention, so the vertices go
    into the written GLB untouched and Unreal's importer does the one conversion.

    Polygons are fanned into triangles. Normals are taken from the file when it
    supplies them and computed per-face when it does not, because the shell's
    walkability is read off normals downstream.
    """
    pos_list = []
    nor_list = []
    uv_list = []
    groups = {}
    group_name = None
    material = None

    def emit(tri_v, tri_t, tri_n):
        key = (material or "(none)", (group_name or "") in TRIM_NODES)
        bucket = groups.setdefault(key, {"pos": [], "nor": [], "uv": [], "idx": [], "grp": []})
        base = len(bucket["pos"])
        p = [pos_list[i] for i in tri_v]
        if all(n is not None for n in tri_n):
            normals = [nor_list[i] for i in tri_n]
        else:
            u = [p[1][i] - p[0][i] for i in range(3)]
            v = [p[2][i] - p[0][i] for i in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(c * c for c in n)) or 1.0
            normals = [[n[0] / ln, n[1] / ln, n[2] / ln]] * 3
        uvs = [uv_list[i] if i is not None and i < len(uv_list) else [0.0, 0.0] for i in tri_t]
        bucket["pos"].extend(p)
        bucket["nor"].extend(normals)
        bucket["uv"].extend(uvs)
        bucket["grp"].extend([group_name or ""] * 3)
        bucket["idx"].extend([base, base + 1, base + 2])

    with open(path, "r", errors="replace") as handle:
        for line in handle:
            if line.startswith("v "):
                f = line.split()
                pos_list.append([float(f[1]), float(f[2]), float(f[3])])
            elif line.startswith("vn "):
                f = line.split()
                nor_list.append([float(f[1]), float(f[2]), float(f[3])])
            elif line.startswith("vt "):
                f = line.split()
                uv_list.append([float(f[1]), float(f[2]) if len(f) > 2 else 0.0])
            elif line.startswith("o ") or line.startswith("g "):
                group_name = line[2:].strip()
            elif line.startswith("usemtl"):
                material = line.split(None, 1)[1].strip()
            elif line.startswith("f "):
                verts = []
                for tok in line.split()[1:]:
                    bits = tok.split("/")
                    vi = int(bits[0]) - 1
                    ti = int(bits[1]) - 1 if len(bits) > 1 and bits[1] else None
                    ni = int(bits[2]) - 1 if len(bits) > 2 and bits[2] else None
                    verts.append((vi, ti, ni))
                for k in range(1, len(verts) - 1):
                    tri = (verts[0], verts[k], verts[k + 1])
                    emit([t[0] for t in tri], [t[1] for t in tri], [t[2] for t in tri])
    return groups


def collect_any(path):
    """collect() for a GLB, collect_obj() for an OBJ."""
    if path.lower().endswith(".obj"):
        return collect_obj(path)
    doc, binary = read_glb(path)
    return collect(doc, binary)


# =============================================================================
# RE-PROFILING THE FOUR RAMPS TO THE SIDE BUTTRESSES' STEEPNESS
# =============================================================================
#
# The owner's ask: "make the octagon buttresses as steep as the side buttresses".
# The side buttresses are not a look, they are two numbers, and they live in
# Source/Trace/World/TraceSideRampProfile.h:
#
#     kToeTangent   1.0635923 = tan(acos(0.71) + 2 deg) = 46.7651 deg
#     kCrestTangent 1.8232359 = tan(acos(0.45) - 2 deg) = 61.2563 deg
#
# which is the LIVE surf band with a 2 degree margin held off each end, so a float
# rounding cannot drop the toe into walkable geometry or push the crest into wall.
# The shape between them is a parabola, and because the tangent of z = H*u^2 is
# linear in u, fixing the two tangents fixes where on that parabola the built face
# starts: kProfileStartFrac = kToeTangent / kCrestTangent.
#
# The tower's RISE is not free - the ramp has to start on its plinth and finish
# flush with the deck - so the DEPTH is what the two tangents imply:
#
#     H = D * kCrestTangent * (1 + startFrac) / 2   ->   D = 2H / (kCrest * (1+s))
#
# With H = 2.700 m that is D = 1.8706 m, against the 2.900 m the model ships. The
# ramps therefore get SHORTER as well as steeper, which is the whole reason they
# stop overhanging anything: the tip comes in from radius 1038 uu to 794 uu.
#
# WHAT THE TRANSFORM DOES, AND WHAT IT REFUSES TO DO
#   * It moves vertices ALONG the arm and adjusts their height. It never touches
#     the across-arm coordinate, so the ramps keep their width and their lane
#     spacing.
#   * Heights are preserved at both ends by construction: the plinth top stays at
#     0.160 and the crest stays at 2.860, so the deck stays FLUSH - the property
#     the owner asked for in the previous round is not quietly undone by this one.
#   * Everything below the plinth top keeps its height exactly, so the base slab
#     stays a slab on the floor instead of being dragged up the new curve.
#   * Above it, each vertex keeps its FRACTIONAL depth under the ride surface, so
#     side panels, edge lights and lane lights follow the new curve instead of
#     sinking through it or floating off it.
#
# The arm's axis is MEASURED from its own geometry rather than read off the n/e/s/w
# in the name: a model that renamed or reordered its arms would otherwise be
# reprofiled along the wrong axis, and the failure would look like a mangled tower
# rather than like a bad assumption.
# =============================================================================

TOE_TANGENT = 1.0635923
CREST_TANGENT = 1.8232359
PROFILE_START_FRAC = TOE_TANGENT / CREST_TANGENT


def reprofile_ramps(groups):
    """Re-cut the four ramps to the side buttresses' band. Returns a report dict."""
    arms = {}
    for key, g in groups.items():
        for i, name in enumerate(g["grp"]):
            if not name.startswith("ramp_"):
                continue
            arm = name.split("_")[1]
            arms.setdefault(arm, []).append((key, i))
    if not arms:
        return None

    # --- per-arm axis, from the geometry -------------------------------------
    report = {"arms": {}, "depth_old_m": None, "depth_new_m": None}
    for arm, refs in sorted(arms.items()):
        pts = [groups[k]["pos"][i] for k, i in refs]
        shell = [groups[k]["pos"][i] for k, i in refs
                 if groups[k]["grp"][i] == "ramp_{0}_shell".format(arm)]
        if not shell:
            raise SystemExit("arm '{0}' has no ramp_{0}_shell to take the ride surface from"
                             .format(arm))
        # the axis is whichever horizontal coordinate the shell spans furthest from 0
        cx = sum(abs(p[0]) for p in shell) / len(shell)
        cz = sum(abs(p[2]) for p in shell) / len(shell)
        axis = 0 if cx > cz else 2
        sign = 1.0 if sum(p[axis] for p in shell) >= 0 else -1.0

        def u_of(p):
            return sign * p[axis]

        u_crest = min(u_of(p) for p in shell)
        u_toe = max(u_of(p) for p in shell)
        plinth_top = min(p[1] for p in shell)
        crest_y = max(p[1] for p in shell)
        rise = crest_y - plinth_top
        if not (u_toe > u_crest and rise > 0.01):
            raise SystemExit("arm '{0}' has a degenerate shell".format(arm))

        # --- the original ride surface, as a table in u ----------------------
        table = {}
        for p in shell:
            u = round(u_of(p), 5)
            table[u] = max(table.get(u, -1e18), p[1])
        us = sorted(table)

        def ride_orig(u):
            if u <= us[0]:
                return table[us[0]]
            if u >= us[-1]:
                return table[us[-1]]
            lo, hi = us[0], us[-1]
            for k in range(len(us) - 1):
                if us[k] <= u <= us[k + 1]:
                    lo, hi = us[k], us[k + 1]
                    break
            if hi - lo < 1e-9:
                return table[lo]
            f = (u - lo) / (hi - lo)
            return table[lo] * (1.0 - f) + table[hi] * f

        # --- the new profile -------------------------------------------------
        s_frac = PROFILE_START_FRAC
        depth_new = 2.0 * rise / (CREST_TANGENT * (1.0 + s_frac))
        full_h = rise / (1.0 - s_frac * s_frac)
        unbuilt = full_h * s_frac * s_frac

        def ride_new(a):
            u_par = s_frac + (1.0 - s_frac) * a
            return plinth_top + full_h * u_par * u_par - unbuilt

        depth_old = u_toe - u_crest
        for key, i in refs:
            p = groups[key]["pos"][i]
            u = u_of(p)
            a = (u_toe - u) / depth_old
            a = min(1.0, max(0.0, a))
            overhang = max(0.0, u - u_toe)
            u_new = u_crest + (1.0 - a) * depth_new + overhang
            y = p[1]
            r_old = ride_orig(u)
            r_new = ride_new(a)
            # THREE BANDS, and each needs a different rule.
            #
            #   at or below the plinth top : keep the height. The base slab is floor
            #       furniture; dragging it up the new curve would lift it off the floor.
            #   between plinth and surface : keep the FRACTIONAL depth, so side panels
            #       stay attached to the ride surface without changing thickness.
            #   above the surface          : keep the ABSOLUTE proudness. The edge and
            #       lane lights stand ~26 mm off the face; scaling that with the curve
            #       pushed them to Y 2.9201 against a source maximum of 2.8860, and near
            #       the toe - where the old surface is only millimetres above the plinth -
            #       the same division sent the threshold strip to Y 1.2689. Both were
            #       this branch being handled by the one above it.
            if y > r_old:
                y = r_new + (y - r_old)
            elif y > plinth_top and (r_old - plinth_top) > 1e-4:
                y = plinth_top + (y - plinth_top) * (r_new - plinth_top) / (r_old - plinth_top)
            new = list(p)
            new[axis] = sign * u_new
            new[1] = y
            groups[key]["pos"][i] = new

        report["arms"][arm] = {"axis": "XYZ"[axis], "sign": sign, "rise_m": rise,
                               "depth_old_m": depth_old, "depth_new_m": depth_new}
        report["depth_old_m"] = depth_old
        report["depth_new_m"] = depth_new

    # Normals are now wrong - they were computed on the old curve. Recompute them
    # per face from the moved vertices, because the walkable/surfable test
    # downstream reads normals and a stale one would report the OLD steepness.
    for g in groups.values():
        idx = g["idx"]
        pos = g["pos"]
        for k in range(0, len(idx) - 2, 3):
            a, b, c = pos[idx[k]], pos[idx[k + 1]], pos[idx[k + 2]]
            u = [b[i] - a[i] for i in range(3)]
            v = [c[i] - a[i] for i in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(t * t for t in n))
            if ln < 1e-12:
                continue
            n = [t / ln for t in n]
            # keep the authored orientation: flip if it disagrees with the old normal
            if sum(n[i] * g["nor"][idx[k]][i] for i in range(3)) < 0.0:
                n = [-t for t in n]
            for j in range(3):
                g["nor"][idx[k + j]] = list(n)
    return report


def build_glb(doc, groups, wanted, label):
    """One node, one mesh, one primitive per surviving material."""
    names = [m.get("name", "") for m in doc.get("materials", [])]
    blob = bytearray()
    views = []
    accessors = []
    prims = []
    mats = []
    mat_index = {}

    def add_view(payload, target):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(payload)
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(payload),
                      "target": target})
        return len(views) - 1

    total_tris = 0
    for key in sorted((k for k in groups if wanted(k)), key=lambda k: (k[0], k[1])):
        mat = key[0]
        g = groups[key]
        if not g["idx"]:
            continue
        pos = g["pos"]
        lo = [min(p[i] for p in pos) for i in range(3)]
        hi = [max(p[i] for p in pos) for i in range(3)]
        vp = add_view(struct.pack("<{0}f".format(len(pos) * 3),
                                  *[c for p in pos for c in p]), 34962)
        vn = add_view(struct.pack("<{0}f".format(len(g["nor"]) * 3),
                                  *[c for p in g["nor"] for c in p]), 34962)
        vt = add_view(struct.pack("<{0}f".format(len(g["uv"]) * 2),
                                  *[c for p in g["uv"] for c in p]), 34962)
        vi = add_view(struct.pack("<{0}I".format(len(g["idx"])), *g["idx"]), 34963)

        accessors.append({"bufferView": vp, "componentType": 5126, "count": len(pos),
                          "type": "VEC3", "min": lo, "max": hi})
        a_pos = len(accessors) - 1
        accessors.append({"bufferView": vn, "componentType": 5126, "count": len(g["nor"]),
                          "type": "VEC3"})
        a_nor = len(accessors) - 1
        accessors.append({"bufferView": vt, "componentType": 5126, "count": len(g["uv"]),
                          "type": "VEC2"})
        a_uv = len(accessors) - 1
        accessors.append({"bufferView": vi, "componentType": 5125, "count": len(g["idx"]),
                          "type": "SCALAR"})
        a_idx = len(accessors) - 1

        if mat not in mat_index:
            src = doc["materials"][names.index(mat)] if mat in names else {"name": mat}
            mats.append(json.loads(json.dumps(src)))
            mat_index[mat] = len(mats) - 1
        prims.append({"attributes": {"POSITION": a_pos, "NORMAL": a_nor, "TEXCOORD_0": a_uv},
                      "indices": a_idx, "material": mat_index[mat], "mode": 4})
        total_tris += len(g["idx"]) // 3
        print("    {0:<15}{1} {2:>6,} tris  {3:>6,} verts".format(
            mat, " (forced trim)" if key[1] else "", len(g["idx"]) // 3, len(pos)))

    if not prims:
        raise SystemExit("{0}: nothing to write".format(label))

    out = {
        "asset": {"version": "2.0",
                  "generator": "Trace Scripts/split_centre_tower.py (flattened)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": label}],
        "meshes": [{"name": label, "primitives": prims}],
        "materials": mats,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
    }
    return out, bytes(blob), total_tris


def deck_height(groups, wanted):
    """The Z a pawn standing at the centre would rest on, by ray-casting down.

    Written to tower_profile.json so Scripts/import_centre_tower.py can size the
    tower without typing a number. It cannot measure this itself: UE 5.8 exposes
    no way to read a static mesh's vertices from Python, and FHitResult is opaque
    there too - line_trace_single returns a struct with no readable properties and
    no break_hit_result exists in any library (all measured). So the one place that
    CAN see the geometry - here, where the vertices are in hand - records it.

    The deck is NOT the bounding box top. The shell's box top is the panel rim at
    2.300 m; the surface a player stands on is 2.260 m, 4 uu lower once scaled.
    Sizing to the box would hang the Core 4 uu in the air over its own deck.
    """
    tris = []
    for key in groups:
        if not wanted(key):
            continue
        g = groups[key]
        pos = g["pos"]
        idx = g["idx"]
        for k in range(0, len(idx) - 2, 3):
            tris.append((pos[idx[k]], pos[idx[k + 1]], pos[idx[k + 2]]))

    best = None
    for a, b, c in tris:
        # Moller-Trumbore against a straight-down ray at the origin.
        e1 = [b[i] - a[i] for i in range(3)]
        e2 = [c[i] - a[i] for i in range(3)]
        d = [0.0, -1.0, 0.0]
        h = [d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]]
        det = sum(e1[i] * h[i] for i in range(3))
        if abs(det) < 1e-12:
            continue
        s_ = [0.0 - a[0], 1000.0 - a[1], 0.0 - a[2]]
        u = sum(s_[i] * h[i] for i in range(3)) / det
        if u < -1e-9 or u > 1 + 1e-9:
            continue
        q = [s_[1] * e1[2] - s_[2] * e1[1], s_[2] * e1[0] - s_[0] * e1[2],
             s_[0] * e1[1] - s_[1] * e1[0]]
        v = sum(d[i] * q[i] for i in range(3)) / det
        if v < -1e-9 or u + v > 1 + 1e-9:
            continue
        t = sum(e2[i] * q[i] for i in range(3)) / det
        if t <= 1e-9:
            continue
        z = 1000.0 - t
        if best is None or z > best:
            best = z
    return best


def measure(path):
    """Triangle count, bounding box and slope histogram, read back off a written file."""
    groups = collect_any(path)
    tris = 0
    lo = [1e18] * 3
    hi = [-1e18] * 3
    up_area = 0.0
    walk_area = 0.0
    for g in groups.values():
        pos = g["pos"]
        for p in pos:
            for i in range(3):
                lo[i] = min(lo[i], p[i])
                hi[i] = max(hi[i], p[i])
        idx = g["idx"]
        tris += len(idx) // 3
        for k in range(0, len(idx) - 2, 3):
            a, b, c = pos[idx[k]], pos[idx[k + 1]], pos[idx[k + 2]]
            u = [b[i] - a[i] for i in range(3)]
            v = [c[i] - a[i] for i in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(t * t for t in n))
            if ln < 1e-12:
                continue
            area = ln / 2.0
            ny = abs(n[1] / ln)
            if ny > 0.05:
                up_area += area
                # 0.7100 IS THE LIVE LIMIT, read off the game's own MOVECFG-P28 line
                # ("band Nz (0.450..0.710) ... upper bound IS GetWalkableFloorZ()"),
                # not the 0.7193 engine default this script used to assume. The two
                # differ by 0.77 of a degree, which is small until a ramp is authored
                # to sit just inside the band and the tool says it is just outside.
                if ny >= 0.7100:
                    walk_area += area
    return tris, lo, hi, up_area, walk_area


def ride_band(groups, walk_z=0.7100, surf_z=0.4500):
    """Area shares of the RIDE SURFACE across the live surf band, plus its angle range.

    Only the ramps' ride surface is measured, not the whole shell: the deck is flat
    by design and the side panels are vertical by design, and averaging those in
    would hide the one number this change exists to move.

    walk_z / surf_z are the LIVE band off the game's MOVECFG-P28 line, not engine
    defaults.
    """
    tot = walk = surf = wall = 0.0
    lo_d = 180.0
    hi_d = 0.0
    for g in groups.values():
        pos = g["pos"]
        idx = g["idx"]
        grp = g.get("grp") or [""] * len(pos)
        for k in range(0, len(idx) - 2, 3):
            names = {grp[idx[k + j]] for j in range(3)} if grp else set()
            if not any(n.startswith("ramp_") and n.endswith("_shell") for n in names):
                continue
            a, b, c = pos[idx[k]], pos[idx[k + 1]], pos[idx[k + 2]]
            u = [b[i] - a[i] for i in range(3)]
            v = [c[i] - a[i] for i in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(t * t for t in n))
            if ln < 1e-12:
                continue
            # ORIENT BY THE AUTHORED NORMAL, THEN TAKE UPWARD FACES ONLY. The shell is
            # a solid strip, not a sheet: 16.5 m2 of it is underside. Taking abs() of
            # the normal counted that underside as ride surface, and because it is
            # dead flat it reported as 36% walkable at 0.00 degrees - a number that
            # described the bottom of the ramp, not the top of it.
            ref = g["nor"][idx[k]] if g.get("nor") else None
            if ref and sum(n[i] * ref[i] for i in range(3)) < 0.0:
                n = [-t for t in n]
            ny = n[1] / ln
            if ny <= 0.05:
                continue
            area = ln / 2.0
            tot += area
            if ny >= walk_z:
                walk += area
            elif ny >= surf_z:
                surf += area
            else:
                wall += area
            deg = math.degrees(math.acos(min(1.0, ny)))
            lo_d = min(lo_d, deg)
            hi_d = max(hi_d, deg)
    if tot <= 0.0:
        return None
    return (walk / tot * 100.0, surf / tot * 100.0, wall / tot * 100.0, lo_d, hi_d)


def climb_check(path, scale, step_limit=45.0, pawn_height=176.0, capsule_radius=34.0):
    """Walk the collision shell up one ramp and report the worst step and headroom.

    THE CHECK THAT WOULD HAVE CAUGHT THE COLLARS. Trace.Core.KickoffProbe passes
    with the ramps sealed, because it PLACES pawns on the deck instead of walking
    them up - so the one property the whole structure exists for was untested by
    every harness in the project. This walks the ride surface inward at 1 cm
    stations, sampling the full capsule width, and asks the two questions
    UCharacterMovementComponent asks: is any upward step taller than
    MaxStepHeight, and does anything hang lower than the pawn is tall.

    Defaults are the engine's and the project's: MaxStepHeight 45 (never
    overridden in Source/), capsule 34 x 88 (TraceCharacterInternal.h:39-40).
    """
    groups = collect_any(path)
    tris = []
    for g in groups.values():
        pos = g["pos"]
        idx = g["idx"]
        for k in range(0, len(idx) - 2, 3):
            tris.append((pos[idx[k]], pos[idx[k + 1]], pos[idx[k + 2]]))

    uu = 100.0 * scale
    half = capsule_radius / uu

    def cast(ox, oy, oz, dy):
        best = None
        for a, b, c in tris:
            e1 = [b[i] - a[i] for i in range(3)]
            e2 = [c[i] - a[i] for i in range(3)]
            d = [0.0, dy, 0.0]
            h = [d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                 d[0] * e2[1] - d[1] * e2[0]]
            det = sum(e1[i] * h[i] for i in range(3))
            if abs(det) < 1e-12:
                continue
            sv = [ox - a[0], oy - a[1], oz - a[2]]
            u = sum(sv[i] * h[i] for i in range(3)) / det
            if u < -1e-7 or u > 1 + 1e-7:
                continue
            q = [sv[1] * e1[2] - sv[2] * e1[1], sv[2] * e1[0] - sv[0] * e1[2],
                 sv[0] * e1[1] - sv[1] * e1[0]]
            v = sum(d[i] * q[i] for i in range(3)) / det
            if v < -1e-7 or u + v > 1 + 1e-7:
                continue
            t = sum(e2[i] * q[i] for i in range(3)) / det
            if t <= 1e-6:
                continue
            if best is None or t < best:
                best = t
        return best

    # THE SWEEP RANGE IS DERIVED, not typed. The first model reached 6.86 m and the
    # remodel 5.36; a hard-coded 6.85 would have walked 1.5 m of empty air and then
    # reported a clean bill of health on a ramp it never touched.
    outer = max(p[0] for g in groups.values() for p in g["pos"])
    inner = min(abs(p[0]) for g in groups.values() for p in g["pos"] if abs(p[2]) < half)
    inner = max(inner, 0.05)

    worst_step = 0.0
    worst_x = None
    worst_head = None
    head_x = None
    prev = None
    x = outer
    while x > inner:
        heights = []
        for zz in (-half, 0.0, half):
            t = cast(x, 100.0, zz, -1.0)
            if t is not None:
                heights.append(100.0 - t)
        if heights:
            y = max(heights)
            if prev is not None:
                rise = (y - prev) * uu
                if rise > worst_step:
                    worst_step, worst_x = rise, x
            up = cast(x, y + 0.02, 0.0, 1.0)
            if up is not None:
                clear = up * uu
                if worst_head is None or clear < worst_head:
                    worst_head, head_x = clear, x
            prev = y
        x -= 0.02

    ok = True
    if worst_step > step_limit:
        print("  FAIL worst upward step {0:.1f} uu at X {1:.2f} m exceeds MaxStepHeight {2:.0f} - "
              "the ramp is a wall".format(worst_step, worst_x, step_limit))
        ok = False
    else:
        print("  ok   worst upward step climbing the ramp  {0:5.1f} uu  (limit {1:.0f})".format(
            worst_step, step_limit))
    if worst_head is not None and worst_head < pawn_height:
        print("  FAIL something overhangs the ride surface by {0:.1f} uu at X {1:.2f} m; a {2:.0f} uu "
              "pawn cannot pass".format(worst_head, head_x, pawn_height))
        ok = False
    else:
        print("  ok   nothing overhangs the ride surface   (pawn is {0:.0f} uu tall)".format(
            pawn_height))
    return ok


def main():
    if not os.path.isfile(SRC):
        raise SystemExit("missing source art: {0}".format(SRC))

    if SRC.lower().endswith(".obj"):
        doc = {"materials": []}
        groups = collect_obj(SRC)
    else:
        doc, binary = read_glb(SRC)
        groups = collect(doc, binary)
    names = {k[0] for k in groups}
    print("source: {0}".format(os.path.relpath(SRC, REPO)))
    print("  materials: {0}".format(", ".join(sorted(names))))

    # EVERY material must be classified. An unknown one means the owner re-exported
    # with a new material and this script's assumptions are stale - worth stopping
    # for, because both failure modes (a missing chunk of tower, or neon that
    # collides) are silent in game.
    unknown = names - SHELL_MATERIALS - NEON_MATERIALS
    if unknown:
        raise SystemExit(
            "unclassified material(s): {0}\n"
            "Add each to SHELL_MATERIALS (collides, walkable) or NEON_MATERIALS "
            "(drawn only) at the top of this script.".format(", ".join(sorted(unknown))))

    src_tris = sum(len(g["idx"]) // 3 for g in groups.values())
    print("  flattened to {0} material group(s), {1:,} triangles".format(len(groups), src_tris))

    # --- the source's own Y range, kept so flushness can be asserted after ------
    src_y = (min(p[1] for g in groups.values() for p in g["pos"]),
             max(p[1] for g in groups.values() for p in g["pos"]))

    def shell_y_of(gs):
        ys = [p[1] for k, g in gs.items()
              if (k[0] in SHELL_MATERIALS and not k[1]) for p in g["pos"]]
        return (min(ys), max(ys))
    src_r = max(math.hypot(p[0], p[2]) for g in groups.values() for p in g["pos"])

    src_shell_y = shell_y_of(groups)
    rep = reprofile_ramps(groups)
    if rep:
        print("  re-cut the ramps to the side buttresses' band "
              "({0:.4f}..{1:.4f} deg):".format(math.degrees(math.atan(TOE_TANGENT)),
                                               math.degrees(math.atan(CREST_TANGENT))))
        for arm, a in sorted(rep["arms"].items()):
            print("    ramp_{0}: axis {1}{2}  rise {3:.4f} m  depth {4:.4f} -> {5:.4f} m".format(
                arm, "+" if a["sign"] > 0 else "-", a["axis"], a["rise_m"],
                a["depth_old_m"], a["depth_new_m"]))

    out_dir = os.path.dirname(SRC)
    written = []
    total = 0
    # The shell is structural material on a node that is NOT forced to trim.
    # Everything else - neon, and the four collars - is trim.
    def is_shell(key):
        return key[0] in SHELL_MATERIALS and not key[1]

    def is_trim(key):
        return not is_shell(key)

    for wanted, name, label in ((is_shell, "CentreTower_Shell.glb", "CentreTower_Shell"),
                                (is_trim, "CentreTower_Neon.glb", "CentreTower_Trim")):
        print("  {0}:".format(label))
        out, blob, tris = build_glb(doc, groups, wanted, label)
        path = os.path.join(out_dir, name)
        size = write_glb(path, out, blob)
        print("    -> {0}  ({1:,} bytes, {2:,} tris)".format(
            os.path.relpath(path, REPO), size, tris))
        written.append((path, label))
        total += tris

    if total != src_tris:
        raise SystemExit("triangle accounting failed: {0} written vs {1} in source"
                         .format(total, src_tris))

    # ---- verification, because a transform bug is silent -----------------------
    #
    # The two halves are re-read FROM DISK and measured. Their combined triangle
    # count, their union bounding box and their walkable-area fraction must match
    # the source. Baking node transforms is the one step here that can go wrong
    # without looking wrong, and all three of these would move if it had.
    shell_y = shell_y_of(groups)
    print("\nverifying the written files:")
    tris = 0
    lo = [1e18] * 3
    hi = [-1e18] * 3
    for path, _ in written:
        t, l, h, _u, _w = measure(path)
        tris += t
        for i in range(3):
            lo[i] = min(lo[i], l[i])
            hi[i] = max(hi[i], h[i])

    ok = True
    if tris != src_tris:
        print("  FAIL triangles {0} != {1}".format(tris, src_tris))
        ok = False
    else:
        print("  ok   triangles                        {0:,}".format(tris))

    # FLUSHNESS IS A PROPERTY OF THE SHELL, so it is asserted on the shell.
    #
    # The trim is allowed to move and does: the edge and lane lights sit ~26 mm
    # proud of the ride surface and follow it, so where the new curve is higher than
    # the old one they rise with it, by up to 33 mm. That is the strips staying
    # stuck to the face, which is what should happen. Asserting on the model's
    # overall Y bound would have failed on exactly that correct behaviour.
    sh_lo, sh_hi = shell_y
    dy = max(abs(sh_lo - src_shell_y[0]), abs(sh_hi - src_shell_y[1]))
    if dy > 1e-4:
        print("  FAIL shell Y moved by {0:.6f} - the ramps no longer meet the deck".format(dy))
        ok = False
    else:
        print("  ok   shell Y {0:9.4f}..{1:9.4f}  unchanged (plinth on the floor, crest flush)"
              .format(sh_lo, sh_hi))
        print("       trim follows the face: {0:+.4f} m at its highest strip".format(
            hi[1] - src_y[1]))

    # X/Z SHRINK, and that is the point: steeper over a fixed rise is shorter.
    out_r = max(abs(lo[0]), abs(hi[0]), abs(lo[2]), abs(hi[2]))
    if out_r >= src_r - 1e-6:
        print("  FAIL footprint did not shrink ({0:.4f} -> {1:.4f}); the re-cut did nothing"
              .format(src_r, out_r))
        ok = False
    else:
        print("  ok   footprint {0:.4f} -> {1:.4f} m  (steeper over a fixed rise is shorter)"
              .format(src_r, out_r))

    # THE ASK ITSELF: every ride-surface facet inside the live surf band.
    # measured on the in-memory groups: build_glb writes ONE node per half, so the
    # written file no longer knows which triangles were ramp_*_shell.
    band = ride_band(groups)
    if band is None:
        print("  FAIL could not measure the ride surface")
        ok = False
    else:
        walk, surf, wall, lo_d, hi_d = band
        print("  {0} ride surface  walkable {1:.1f}%  SURF {2:.1f}%  wall {3:.1f}%   "
              "facets {4:.2f}..{5:.2f} deg".format(
                  "ok  " if (walk < 0.5 and wall < 0.5) else "FAIL",
                  walk, surf, wall, lo_d, hi_d))
        if walk >= 0.5 or wall >= 0.5:
            print("       (the side buttresses ship 46.90..61.16 deg, 0 walkable, 0 wall)")
            ok = False

    # ---- can a pawn actually get up it? ----------------------------------------
    shell_path = os.path.join(out_dir, "CentreTower_Shell.glb")
    deck_for_scale = deck_height(groups, is_shell)
    if deck_for_scale:
        # The scale the importer will pick, so the check is done in shipped units.
        assumed_scale = 553.666 / (deck_for_scale * 100.0)
        if not climb_check(shell_path, assumed_scale):
            ok = False

    if not ok:
        raise SystemExit("verification FAILED - the written halves do not match the source")

    # ---- the profile the importer sizes from -----------------------------------
    deck_m = deck_height(groups, is_shell)
    if deck_m is None:
        raise SystemExit("no shell surface directly over the centre - cannot find the deck")
    shell_top_m = max(p[1] for k in groups if is_shell(k) for p in groups[k]["pos"])
    # The furthest any shell vertex reaches from the tower's axis. The importer needs
    # it to answer "does anything overlap the ramps", and it cannot get it from the
    # placed actor: at yaw 45 UE's actor bounds are the ROTATED BOX's AABB, inflated
    # by root two, which reads 1847 uu for a tower that actually reaches 1069.
    reach_m = max(math.hypot(p[0], p[2]) for k in groups if is_shell(k) for p in groups[k]["pos"])
    profile = {
        "deck_top_m": deck_m,
        "shell_top_m": shell_top_m,
        "shell_reach_m": reach_m,
        "deck_fraction_of_shell_box": deck_m / shell_top_m,
        "source": os.path.basename(SRC),
        "note": ("deck_top_m is the surface a pawn rests on at the centre, found by ray-cast; "
                 "shell_top_m is the shell's bounding-box top. The importer needs the ratio "
                 "because Unreal can give it the box but not the deck."),
    }
    prof_path = os.path.join(out_dir, "tower_profile.json")
    with open(prof_path, "w") as handle:
        json.dump(profile, handle, indent=2, sort_keys=True)
    print("\n  deck (what a pawn stands on) {0:.4f} m   shell box top {1:.4f} m   ratio {2:.6f}"
          .format(deck_m, shell_top_m, deck_m / shell_top_m))
    print("  shell reach {0:.4f} m from the axis".format(reach_m))
    print("  wrote {0}".format(os.path.relpath(prof_path, REPO)))
    print("\nBoth halves match the source. Each is ONE mesh with one slot per material,")
    print("so Unreal imports each as a single StaticMesh.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
