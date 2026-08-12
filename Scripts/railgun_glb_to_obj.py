#!/usr/bin/env python3
"""Split the railgun GLB into Unreal-space OBJ meshes: body + two rail walls.

Run by Scripts/import-railgun.sh before the editor stage; it needs nothing but
stock Python, so it is cheap to re-run and easy to diff.  Source of truth is
Art/Railgun/railgun.glb (the artist's export); everything under out/ is derived.

glTF is Y-up / right-handed / muzzle down -Z.  Unreal is Z-up / left-handed /
muzzle down +X.  Mapping:  UE.X = -gl.z ,  UE.Y = gl.x ,  UE.Z = gl.y
That matrix has determinant -1 (which is what converts right- to left-handed),
so triangle winding must be reversed or every normal points inward.
Units: metres -> centimetres (x100).
"""
import json, struct, sys, os

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
SRC = os.environ.get("TRACE_RAILGUN_GLB", os.path.join(_ROOT, "Art", "Railgun", "railgun.glb"))
OUT = os.environ.get("TRACE_RAILGUN_OBJ_DIR", os.path.join(_ROOT, "Intermediate", "Railgun"))

# The rail "walls" that throw apart on discharge.  The README names
# "rails, channel lights, muzzle prongs"; in this export the only genuinely
# per-side members of the rail assembly are the rail and its prong.  The
# glowing accelerator_core sits on the centreline and stays with the body,
# so parting the walls exposes it -- which is the intended read.
WALL_L = {"rail_left", "prong_left"}
WALL_R = {"rail_right", "prong_right"}

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

def main():
    if not os.path.isfile(SRC):
        sys.exit("[Trace] railgun source not found: {0}".format(SRC))
    os.makedirs(OUT, exist_ok=True)
    js, bin_ = load(SRC)
    nodes = js["nodes"]
    parent = {}
    for i, nd in enumerate(nodes):
        for c in nd.get("children", []): parent[c] = i

    def world_offset(i):
        x = y = z = 0.0
        while True:
            m = nodes[i].get("matrix")
            if m: x += m[12]; y += m[13]; z += m[14]
            if i in parent: i = parent[i]
            else: return x, y, z

    # local frame = the `railgun` node, so vertices are relative to the weapon
    # root rather than to wherever the model happened to sit in the scene.
    root = next(i for i, n in enumerate(nodes) if n.get("name") == "railgun")
    ROOT = world_offset(root)

    # hinge for each wall: the REAR end of the rail, so the muzzle end cants out.
    def hinge(rail_name):
        i = next(k for k, n in enumerate(nodes) if n.get("name") == rail_name)
        wx, wy, wz = world_offset(i)
        a = js["accessors"][js["meshes"][nodes[i]["mesh"]]["primitives"][0]["attributes"]["POSITION"]]
        return (wx - ROOT[0], wy - ROOT[1], wz + a["max"][2] - ROOT[2])

    HINGE = {"RailL": hinge("rail_left"), "RailR": hinge("rail_right")}

    groups = {"Body": [], "RailL": [], "RailR": []}
    for i, nd in enumerate(nodes):
        if nd.get("mesh") is None: continue
        nm = nd.get("name", "")
        g = "RailL" if nm in WALL_L else "RailR" if nm in WALL_R else "Body"
        groups[g].append(i)

    def to_ue(p, origin):
        """glTF metres -> Unreal centimetres, relative to `origin`."""
        x = (p[0] - origin[0]) * 100.0
        y = (p[1] - origin[1]) * 100.0
        z = (p[2] - origin[2]) * 100.0
        return (-z, x, y)          # UE.X = -gl.z, UE.Y = gl.x, UE.Z = gl.y

    manifest = {"source": os.path.basename(SRC), "unit": "cm", "meshes": {}}
    for gname, node_ids in groups.items():
        origin = (0.0, 0.0, 0.0)
        if gname == "Body":
            origin = ROOT
        else:
            h = HINGE[gname]
            origin = (ROOT[0] + h[0], ROOT[1] + h[1], ROOT[2] + h[2])

        verts, norms, uvs, faces_by_mat = [], [], [], {}
        lo = [9e9]*3; hi = [-9e9]*3
        for ni in node_ids:
            nd = nodes[ni]
            wx, wy, wz = world_offset(ni)
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
                    wp = (p[0] + wx, p[1] + wy, p[2] + wz)
                    v = to_ue(wp, origin)
                    verts.append(v)
                    lo = [min(lo[c], v[c]) for c in range(3)]
                    hi = [max(hi[c], v[c]) for c in range(3)]
                    if nrm:
                        n = nrm[k]
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

            
        path = os.path.join(OUT, f"SM_Railgun_{gname}.obj")
        with open(path, "w") as f:
            f.write(f"# Trace railgun - {gname} - Unreal space (cm, +X forward)\n")
            f.write("mtllib railgun.mtl\n")
            f.write(f"o SM_Railgun_{gname}\n")
            for v in verts: f.write(f"v {v[0]:.5f} {v[1]:.5f} {v[2]:.5f}\n")
            for t in uvs:  f.write(f"vt {t[0]:.5f} {t[1]:.5f}\n")
            for n in norms: f.write(f"vn {n[0]:.5f} {n[1]:.5f} {n[2]:.5f}\n")
            for mat, fl in faces_by_mat.items():
                # No `g` line: some OBJ importers split one static mesh per
                # group, and we want exactly one mesh with five material slots.
                f.write(f"usemtl {mat}\n")
                for a, b, c in fl:
                    f.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")
        manifest["meshes"][gname] = {
            "obj": os.path.basename(path),
            "tris": sum(len(v) for v in faces_by_mat.values()),
            "verts": len(verts),
            "materials": sorted(faces_by_mat),
            "bounds_min": [round(x, 3) for x in lo],
            "bounds_max": [round(x, 3) for x in hi],
        }
        if gname != "Body":
            h = HINGE[gname]
            manifest["meshes"][gname]["attach_to_body_cm"] = [
                round(v, 3) for v in to_ue((ROOT[0]+h[0], ROOT[1]+h[1], ROOT[2]+h[2]), ROOT)]

    # muzzle tip, in body-local Unreal space
    mi = next(k for k, n in enumerate(nodes) if n.get("name") == "muzzle_aperture")
    wx, wy, wz = world_offset(mi)
    a = js["accessors"][js["meshes"][nodes[mi]["mesh"]]["primitives"][0]["attributes"]["POSITION"]]
    manifest["muzzle_cm"] = [round(v, 3) for v in to_ue((wx, wy, wz + a["min"][2]), ROOT)]

    with open(os.path.join(OUT, "railgun.mtl"), "w") as f:
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

    with open(os.path.join(OUT, "railgun_manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(json.dumps(manifest, indent=2))

main()
