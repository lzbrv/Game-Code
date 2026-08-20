#!/usr/bin/env python3
"""Split a railgun GLB into Unreal-space OBJ meshes, one per moving part.

Two rigs share this converter:

    railgun   Art/Railgun/railgun.glb      -> Intermediate/Railgun   (the pistol)
    smg       Art/Smg/railgun_smg.glb      -> Intermediate/Smg       (the SMG)

Run by Scripts/import-railgun.sh before the editor stage; it needs nothing but
stock Python, so it is cheap to re-run and easy to diff.  Source of truth is the
artist's export under Art/; everything under Intermediate/ is derived.

glTF is Y-up / right-handed / muzzle down -Z.  Unreal is Z-up / left-handed /
muzzle down +X.  Mapping:  UE.X = -gl.z ,  UE.Y = gl.x ,  UE.Z = gl.y
That matrix has determinant -1 (which is what converts right- to left-handed),
so triangle winding must be reversed or every normal points inward.
Units: metres -> centimetres (x100).

WHY THE RIGS DIFFER, AND WHY THE DIFFERENCES ARE OPTIONS RATHER THAN TWO SCRIPTS
  - SPLIT.  The pistol's export has no pivot nodes, so its two rail walls can
    only be found by name.  The SMG's export DOES have them (wall_pivot_left,
    wall_pivot_right, mag_pivot), so its split is read off the hierarchy: any
    direct child of the weapon root that carries children but no mesh of its own
    is a pivot, and its whole subtree becomes one mesh.  Nothing is hardcoded.
  - ORIGIN.  A pistol wall is baked around its hinge (the rear of the rail) so
    rotating the component swings the muzzle end out.  Every SMG pivot already
    sits on the weapon root, and its authored motion is a translation, so each
    SMG group is baked around the root and attaches at (0,0,0).
  - TRANSFORM.  See the block comment on Xform below.  The pistol is pinned to
    the translation-only behaviour it shipped with; the SMG composes the full
    node matrices, which is what glTF actually means.
"""
import json, struct, sys, os

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)


# -----------------------------------------------------------------------------
# The rigs
#
# `transform`:
#   "translation"  accumulate node matrix translations only, ignoring any
#                  rotation or scale.  THIS IS WHAT THE PISTOL SHIPPED WITH and
#                  the pistol's committed .uassets were baked from it, so the
#                  pistol stays on it: changing it would silently rewrite art
#                  that is already in the repository.  It is not what glTF
#                  means (18 of the pistol's 55 nodes carry a rotation that is
#                  being dropped) -- see the note at the end of this file.
#   "matrix"       compose the full 4x4 chain, and carry normals through the
#                  inverse transpose.  Correct, and what new art uses.
# -----------------------------------------------------------------------------
RIGS = {
    "railgun": {
        "stem": "railgun",
        "prefix": "SM_Railgun_",
        "glb": ("Art", "Railgun", "railgun.glb"),
        "out": ("Intermediate", "Railgun"),
        "glb_env": "TRACE_RAILGUN_GLB",
        "out_env": "TRACE_RAILGUN_OBJ_DIR",
        "root": "railgun",
        "transform": "translation",
        # The rail "walls" that throw apart on discharge.  The README names
        # "rails, channel lights, muzzle prongs"; in this export the only
        # genuinely per-side members of the rail assembly are the rail and its
        # prong.  The glowing accelerator_core sits on the centreline and stays
        # with the body, so parting the walls exposes it -- the intended read.
        "split": ("names", [
            ("RailL", {"rail_left", "prong_left"}, "rail_left"),
            ("RailR", {"rail_right", "prong_right"}, "rail_right"),
        ]),
        "origin": "hinge",
        "muzzle": ("muzzle_aperture", "tip"),
        "extras": False,
    },
    "smg": {
        "stem": "railgun_smg",
        "prefix": "SM_RailgunSmg_",
        "glb": ("Art", "Smg", "railgun_smg.glb"),
        "out": ("Intermediate", "Smg"),
        "glb_env": "TRACE_SMG_GLB",
        "out_env": "TRACE_SMG_OBJ_DIR",
        "root": "railgun_smg",
        "transform": "matrix",
        "split": ("pivots", None),
        "origin": "pivot",
        "muzzle": ("muzzle_aperture", "aperture_centre"),
        "extras": True,
    },
}


def load(path):
    d = open(path, "rb").read()
    assert d[:4] == b"glTF", "not a binary glTF"
    off, js, bin_ = 12, None, None
    while off < len(d):
        ln, ty = struct.unpack_from("<II", d, off); off += 8
        if ty == 0x4E4F534A: js = json.loads(d[off:off+ln].decode("utf-8"))
        elif ty == 0x004E4942: bin_ = d[off:off+ln]
        off += ln
    return js, bin_

NCOMP = {"SCALAR":1, "VEC2":2, "VEC3":3, "VEC4":4}
CTYPE = {5120:("b",1), 5121:("B",1), 5122:("h",2), 5123:("H",2), 5125:("I",4), 5126:("f",4)}

def read_accessor(js, bin_, idx):
    a = js["accessors"][idx]
    n = NCOMP[a["type"]]
    fmt, sz = CTYPE[a["componentType"]]
    bv = js["bufferViews"][a["bufferView"]]
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride") or n * sz
    out = []
    for i in range(a["count"]):
        o = base + i * stride
        out.append(struct.unpack_from("<" + fmt * n, bin_, o))
    return out


# -----------------------------------------------------------------------------
# Node transforms
#
# An Xform is a 3x3 linear part L (column-major, as glTF stores it) plus a
# translation t.  L is None for the pure-translation rig, and that is not just
# an optimisation: `p + t` and `I*p + t` are not the same floating-point
# expression when a coordinate is -0.0, and the pistol's baked output contains
# -0.0 values.  Keeping the identity case as a literal add is what makes the
# pistol byte-reproducible.
# -----------------------------------------------------------------------------
class Xform:
    __slots__ = ("L", "t")

    def __init__(self, L, t):
        self.L = L
        self.t = t

    @staticmethod
    def identity(linear):
        return Xform([1.0,0.0,0.0, 0.0,1.0,0.0, 0.0,0.0,1.0] if linear else None, (0.0, 0.0, 0.0))

    def compose(self, child):
        """self o child -- apply `child` first, then `self`."""
        t = child.t
        if self.L is None:
            return Xform(None, (self.t[0] + t[0], self.t[1] + t[1], self.t[2] + t[2]))
        a, b = self.L, child.L
        L = [sum(a[k*3+row] * b[col*3+k] for k in range(3))
             for col in range(3) for row in range(3)]
        return Xform(L, tuple(sum(a[k*3+row] * t[k] for k in range(3)) + self.t[row]
                              for row in range(3)))

    def point(self, p):
        t = self.t
        if self.L is None:
            return (p[0] + t[0], p[1] + t[1], p[2] + t[2])
        L = self.L
        return tuple(L[0*3+r]*p[0] + L[1*3+r]*p[1] + L[2*3+r]*p[2] + t[r] for r in range(3))

    def normal_matrix(self):
        """Inverse transpose of L, so non-uniform scale does not skew normals."""
        if self.L is None:
            return None
        m = self.L
        def g(r, c): return m[c*3 + r]
        c00 = g(1,1)*g(2,2) - g(1,2)*g(2,1)
        c01 = g(1,2)*g(2,0) - g(1,0)*g(2,2)
        c02 = g(1,0)*g(2,1) - g(1,1)*g(2,0)
        det = g(0,0)*c00 + g(0,1)*c01 + g(0,2)*c02
        if abs(det) < 1e-20:
            return None
        # cofactor matrix / det == inverse transpose
        cof = [
            c00, c01, c02,
            g(0,2)*g(2,1) - g(0,1)*g(2,2), g(0,0)*g(2,2) - g(0,2)*g(2,0), g(0,1)*g(2,0) - g(0,0)*g(2,1),
            g(0,1)*g(1,2) - g(0,2)*g(1,1), g(0,2)*g(1,0) - g(0,0)*g(1,2), g(0,0)*g(1,1) - g(0,1)*g(1,0),
        ]
        return [v / det for v in cof]      # row-major

    def normal(self, n):
        N = self.normal_matrix()
        if N is None:
            return n
        v = (N[0]*n[0] + N[1]*n[1] + N[2]*n[2],
             N[3]*n[0] + N[4]*n[1] + N[5]*n[2],
             N[6]*n[0] + N[7]*n[1] + N[8]*n[2])
        ln = (v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) ** 0.5
        return v if ln == 0.0 else (v[0]/ln, v[1]/ln, v[2]/ln)


def node_xform(nodes, i, linear):
    m = nodes[i].get("matrix")
    if not m:
        return Xform.identity(linear)
    if not linear:
        return Xform(None, (m[12], m[13], m[14]))
    return Xform([m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10]], (m[12], m[13], m[14]))


def pivot_group_name(node_name):
    """wall_pivot_left -> WallLeft, mag_pivot -> Mag.  Derived, never typed in."""
    toks = [t for t in node_name.split("_") if t and t.lower() != "pivot"]
    return "".join(t[:1].upper() + t[1:] for t in toks) or "Group"


def main():
    rig_name = sys.argv[1] if len(sys.argv) > 1 else "railgun"
    if rig_name not in RIGS:
        sys.exit("[Trace] unknown rig '{0}'; known: {1}".format(rig_name, ", ".join(sorted(RIGS))))
    rig = RIGS[rig_name]
    LINEAR = rig["transform"] == "matrix"

    SRC = os.environ.get(rig["glb_env"], os.path.join(_ROOT, *rig["glb"]))
    OUT = os.environ.get(rig["out_env"], os.path.join(_ROOT, *rig["out"]))
    STEM, PREFIX = rig["stem"], rig["prefix"]

    if not os.path.isfile(SRC):
        sys.exit("[Trace] {0} source not found: {1}".format(rig_name, SRC))
    os.makedirs(OUT, exist_ok=True)
    js, bin_ = load(SRC)
    nodes = js["nodes"]
    parent = {}
    for i, nd in enumerate(nodes):
        for c in nd.get("children", []): parent[c] = i

    def world(i):
        x = node_xform(nodes, i, LINEAR)
        while i in parent:
            i = parent[i]
            x = node_xform(nodes, i, LINEAR).compose(x)
        return x

    def world_offset(i):
        return world(i).t

    # local frame = the weapon root node, so vertices are relative to the weapon
    # rather than to wherever the model happened to sit in the scene.
    root = next(i for i, n in enumerate(nodes) if n.get("name") == rig["root"])
    ROOT = world_offset(root)

    # ---- the split -----------------------------------------------------------
    group_of = {}                      # node index -> group name
    groups = {"Body": []}
    hinges = {}
    pivot_node = {}

    mode, cfg = rig["split"]
    if mode == "names":
        # hinge for each wall: the REAR end of the rail, so the muzzle end cants out.
        for gname, members, hinge_node in cfg:
            groups[gname] = []
            for i, nd in enumerate(nodes):
                if nd.get("name") in members:
                    group_of[i] = gname
            h = next(k for k, n in enumerate(nodes) if n.get("name") == hinge_node)
            wx, wy, wz = world_offset(h)
            a = js["accessors"][js["meshes"][nodes[h]["mesh"]]["primitives"][0]["attributes"]["POSITION"]]
            hinges[gname] = (wx - ROOT[0], wy - ROOT[1], wz + a["max"][2] - ROOT[2])
    else:
        # PIVOT-DRIVEN.  A pivot is a direct child of the root that carries
        # children but no mesh of its own; its entire subtree is one group.
        for c in nodes[root].get("children", []):
            if nodes[c].get("mesh") is not None or not nodes[c].get("children"):
                continue
            gname = pivot_group_name(nodes[c].get("name", ""))
            groups[gname] = []
            pivot_node[gname] = nodes[c].get("name", "")
            hinges[gname] = tuple(world_offset(c)[k] - ROOT[k] for k in range(3))
            stack = [c]
            while stack:
                k = stack.pop()
                group_of[k] = gname
                stack.extend(nodes[k].get("children", []))
        if len(groups) == 1:
            sys.exit("[Trace] {0}: no pivot nodes under '{1}' -- nothing to split on."
                     .format(rig_name, rig["root"]))

    for i, nd in enumerate(nodes):
        if nd.get("mesh") is None: continue
        groups[group_of.get(i, "Body")].append(i)

    def to_ue(p, origin):
        """glTF metres -> Unreal centimetres, relative to `origin`."""
        x = (p[0] - origin[0]) * 100.0
        y = (p[1] - origin[1]) * 100.0
        z = (p[2] - origin[2]) * 100.0
        return (-z, x, y)          # UE.X = -gl.z, UE.Y = gl.x, UE.Z = gl.y

    manifest = {"source": os.path.basename(SRC), "unit": "cm", "meshes": {}}
    for gname, node_ids in groups.items():
        if gname == "Body":
            origin = ROOT
        else:
            h = hinges[gname]
            origin = (ROOT[0] + h[0], ROOT[1] + h[1], ROOT[2] + h[2])

        verts, norms, uvs, faces_by_mat = [], [], [], {}
        lo = [9e9]*3; hi = [-9e9]*3
        for ni in node_ids:
            nd = nodes[ni]
            W = world(ni)
            wx, wy, wz = W.t
            for prim in js["meshes"][nd["mesh"]]["primitives"]:
                mat = js["materials"][prim["material"]]["name"] if "material" in prim else "shell"
                pos = read_accessor(js, bin_, prim["attributes"]["POSITION"])
                nrm = read_accessor(js, bin_, prim["attributes"]["NORMAL"]) if "NORMAL" in prim["attributes"] else None
                if "indices" in prim:
                    idx = [t[0] for t in read_accessor(js, bin_, prim["indices"])]
                else:
                    idx = list(range(len(pos)))      # non-indexed primitive
                base = len(verts)
                for k, p in enumerate(pos):
                    wp = W.point(p) if LINEAR else (p[0] + wx, p[1] + wy, p[2] + wz)
                    v = to_ue(wp, origin)
                    verts.append(v)
                    lo = [min(lo[c], v[c]) for c in range(3)]
                    hi = [max(hi[c], v[c]) for c in range(3)]
                    if nrm:
                        n = W.normal(nrm[k]) if LINEAR else nrm[k]
                        n = (-n[2], n[0], n[1])
                    else:
                        n = (0.0, 0.0, 1.0)
                    norms.append(n)
                    # The materials are untextured solid colours, so UVs only
                    # have to be non-degenerate -- a box projection onto
                    # whichever axis the normal points down is enough, and it
                    # keeps the lightmap unwrapper from choking on zero-area
                    # charts. 100 cm per UV tile.
                    ax = max(range(3), key=lambda c: abs(n[c]))
                    u, vv = [(v[1], v[2]), (v[0], v[2]), (v[0], v[1])][ax]
                    uvs.append((u / 100.0, vv / 100.0))
                fl = faces_by_mat.setdefault(mat, [])
                for t in range(0, len(idx), 3):
                    a, b, c = idx[t] + base + 1, idx[t+1] + base + 1, idx[t+2] + base + 1
                    fl.append((a, c, b))          # reversed winding: det(M) = -1


        path = os.path.join(OUT, "{0}{1}.obj".format(PREFIX, gname))
        with open(path, "w") as f:
            f.write(f"# Trace {STEM} - {gname} - Unreal space (cm, +X forward)\n")
            f.write(f"mtllib {STEM}.mtl\n")
            f.write(f"o {PREFIX}{gname}\n")
            for v in verts: f.write(f"v {v[0]:.5f} {v[1]:.5f} {v[2]:.5f}\n")
            for t in uvs:  f.write(f"vt {t[0]:.5f} {t[1]:.5f}\n")
            for n in norms: f.write(f"vn {n[0]:.5f} {n[1]:.5f} {n[2]:.5f}\n")
            for mat, fl in faces_by_mat.items():
                # No `g` line: some OBJ importers split one static mesh per
                # group, and we want exactly one mesh with five material slots.
                f.write(f"usemtl {mat}\n")
                for a, b, c in fl:
                    f.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")
        entry = {
            "obj": os.path.basename(path),
            "tris": sum(len(v) for v in faces_by_mat.values()),
            "verts": len(verts),
            "materials": sorted(faces_by_mat),
            "bounds_min": [round(x, 3) for x in lo],
            "bounds_max": [round(x, 3) for x in hi],
        }
        manifest["meshes"][gname] = entry
        if gname != "Body":
            h = hinges[gname]
            entry["attach_to_body_cm"] = [
                round(v, 3) for v in to_ue((ROOT[0]+h[0], ROOT[1]+h[1], ROOT[2]+h[2]), ROOT)]
        if rig["extras"]:
            entry["nodes"] = [nodes[i].get("name", "") for i in node_ids]
            if gname in pivot_node:
                entry["pivot_node"] = pivot_node[gname]

    # ---- the muzzle, MEASURED from the mesh ----------------------------------
    mname, mmode = rig["muzzle"]
    mi = next(k for k, n in enumerate(nodes) if n.get("name") == mname)
    W = world(mi)
    prim0 = js["meshes"][nodes[mi]["mesh"]]["primitives"][0]
    a = js["accessors"][prim0["attributes"]["POSITION"]]
    if mmode == "tip":
        wx, wy, wz = W.t
        manifest["muzzle_cm"] = [round(v, 3) for v in to_ue((wx, wy, wz + a["min"][2]), ROOT)]
    else:
        # The aperture is a ring whose local bounding box is centred on the node
        # origin; that centre, carried through the node chain, is where the beam
        # leaves. Taken from the mesh's own bounds, not from the kit's prose.
        centre = W.point(tuple((a["min"][c] + a["max"][c]) * 0.5 for c in range(3)))
        manifest["muzzle_cm"] = [round(v, 3) for v in to_ue(centre, ROOT)]
        pts = [to_ue(W.point(p), ROOT) for p in read_accessor(js, bin_, prim0["attributes"]["POSITION"])]
        tip = max(pts, key=lambda q: q[0])
        manifest["muzzle_tip_cm"] = [round(v, 3) for v in tip]
        manifest["muzzle_bounds_min"] = [round(min(q[c] for q in pts), 3) for c in range(3)]
        manifest["muzzle_bounds_max"] = [round(max(q[c] for q in pts), 3) for c in range(3)]

    if rig["extras"]:
        # Landmarks the viewmodel places the rig from, in the same mesh-local cm.
        marks = {}
        for nm in ("grip", "muzzle_aperture"):
            k = next((j for j, n in enumerate(nodes) if n.get("name") == nm), None)
            if k is not None:
                marks[nm] = [round(v, 3) for v in to_ue(world(k).t, ROOT)]
        manifest["landmarks_cm"] = marks

    with open(os.path.join(OUT, "{0}.mtl".format(STEM)), "w") as f:
        for m in js["materials"]:
            c = m["pbrMetallicRoughness"]["baseColorFactor"]
            f.write(f"newmtl {m['name']}\nKd {c[0]:.5f} {c[1]:.5f} {c[2]:.5f}\n")
            e = m.get("emissiveFactor")
            if e: f.write(f"Ke {e[0]:.5f} {e[1]:.5f} {e[2]:.5f}\n")
            f.write("\n")

    manifest["materials"] = {
        m["name"]: {
            "base_color": [round(v, 5) for v in m["pbrMetallicRoughness"]["baseColorFactor"][:3]],
            "metallic": m["pbrMetallicRoughness"].get("metallicFactor", 1.0),
            "roughness": m["pbrMetallicRoughness"].get("roughnessFactor", 1.0),
            "emissive": [round(v, 5) for v in m.get("emissiveFactor", [0, 0, 0])],
            "emissive_strength": m.get("extensions", {}).get(
                "KHR_materials_emissive_strength", {}).get("emissiveStrength", 1.0),
        } for m in js["materials"]}

    if rig["extras"]:
        manifest["mesh_prefix"] = PREFIX
        manifest["animations"] = js.get("animations", [])

    with open(os.path.join(OUT, "{0}_manifest.json".format(STEM)), "w") as f:
        json.dump(manifest, f, indent=2)
    print(json.dumps(manifest, indent=2))

main()

# NOTE, recorded rather than acted on: the pistol rig drops every node rotation
# (18 of its 55 nodes carry one, e.g. trigger_guard's 90 degrees about X).  Its
# committed meshes were baked that way, so "fix" it only together with a
# deliberate re-import and a fresh look at the gun -- flipping the pistol to
# "matrix" here is a one-word change but it rewrites art already in the repo.
