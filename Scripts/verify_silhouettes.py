#!/usr/bin/env python3
"""verify_silhouettes.py -- the CHARACTER_LANGUAGE SS8 T1/T4/T5 gate.

Stock python (LANGUAGE SS8 T1 requirement).  Run after
generate_characters.py; asserts, per generated body:

  T0  GLB re-read.  Each Intermediate/Characters/<name>.glb is re-parsed
      through the PROJECT'S OWN loader -- the `load` + `read_accessor`
      functions are lifted at runtime from Scripts/railgun_glb_to_obj.py via
      ast (that module runs main() on import, so it cannot be imported; the
      extraction executes its genuine code, not a copy that could drift).
      Asserts: container parses, 26 canonical joints (names, parents, IBM
      translations = -joint position), 5 material slots in the canonical
      order, per-primitive triangle sums == manifest, positions finite and
      rehydrating (via the MEASURED gl->UE map) to the manifest bounds.
      Also rebuilds the body from character_bodies and requires the rebuilt
      GLB byte-identical (sha1) to the one on disk -- so the masks below
      provably test the same geometry that will be imported.

  T1  46 px mask test.  Front/side/rear binary masks at 3.8 uu/px (~46 px
      tall body).  (a) every named signature feature contributes >= 8 px^2
      to >= 1 mask (contribution = pixels lost when the feature's parts are
      removed); (b) pairwise front-mask IoU <= 0.88, centroid-aligned, for
      every generated pair (wave 1 runs the Rocco/Lily pair; wave 2 all 45).
      Masks are written as PNGs (mask-test archive evidence).

  T2 is T1 by definition (geometry only == the emissive-off / carrier-white /
  cloak read -- LANGUAGE SS8), documented here rather than re-run.

  T4  Budgets.  accent area <= the character cap (8%% / 5%%), accent < team
      area, every accent op width in [8, 10] uu, open LINE/DASH ops
      node- or edge-terminated (SHEETS SS0.4 clauses), >= 2 node pads,
      exactly one service ring.

  T5  Envelopes.  bbox height 176 +/- 0.5 excluding crown_break parts; feet
      at 0 +/- 0.5; crown-break parts <= Z 208 with declared thickness <= 8;
      lateral envelope +/-40 (+/-44 Mortimer) for all non-arm-chain parts
      (the A-pose skeleton parks hands at +/-53, so the arm chain is exempt
      by construction -- the T-pose text this rule came from had arms at
      +/-74); anything beyond +/-36 or above Z 176 declares thickness <= 12;
      SS4.9/A1 swing envelope for tagged gear (back gear <= 26 uu proud of
      the Y=-12 back plane, within +/-30 lateral; front gear <= 10 uu proud
      of the chest plane; pelvis-side gear <= 26 lateral; thigh gear <= 20
      lateral).

Verdict lines per body/test and `[verify-silhouettes] EXIT=0/1`.

Usage:
    python3 Scripts/verify_silhouettes.py [--names Rocco,Lily]
        [--dir Intermediate/Characters] [--masks-dir <default: DIR/masks>]
"""
import argparse
import ast
import json
import os
import struct
import sys
import zlib

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)

import character_bodies as cb  # noqa: E402
from trace_glb import TraceGlbWriter  # noqa: E402

PX = 3.8               # uu per pixel: 46 px body at 176 uu (LANGUAGE SS1)
CANVAS_W, CANVAS_H = 56, 60   # px; covers X +/-106, Z -4..224
ORIGIN_X, ORIGIN_Z = -106.4, -4.0


# ---------------------------------------------------------------------------
# T0: the project's own GLB loader, lifted from railgun_glb_to_obj.py
# ---------------------------------------------------------------------------


def railgun_loader():
    """Extract `load`, `read_accessor` (+ their NCOMP/CTYPE tables) from
    Scripts/railgun_glb_to_obj.py without executing its main()."""
    path = os.path.join(_HERE, "railgun_glb_to_obj.py")
    tree = ast.parse(open(path).read(), filename=path)
    wanted = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name in ("load", "read_accessor"):
            wanted.append(node)
        if isinstance(node, ast.Assign):
            targets = [t.id for t in node.targets if isinstance(t, ast.Name)]
            if any(t in ("NCOMP", "CTYPE") for t in targets):
                wanted.append(node)
    mod = ast.Module(body=wanted, type_ignores=[])
    ns = {"json": json, "struct": struct}
    exec(compile(mod, path, "exec"), ns)  # the genuine loader code
    # the railgun rigs carry no skins, so the loader's component table never
    # needed MAT4; teach the extracted copy of the table about it (runtime
    # namespace only -- railgun_glb_to_obj.py itself is untouched)
    ns["NCOMP"]["MAT4"] = 16
    return ns["load"], ns["read_accessor"]


def gl_to_ue(p):
    """MEASURED axis map: UE(X,Y,Z) = (gl.x, gl.z, gl.y) * 100."""
    return (p[0] * 100.0, p[2] * 100.0, p[1] * 100.0)


def t0_reread(name, body, out_dir, problems):
    load, read_accessor = railgun_loader()
    glb_path = os.path.join(out_dir, name.lower() + ".glb")
    man_path = os.path.join(out_dir, name.lower() + "_manifest.json")
    if not (os.path.isfile(glb_path) and os.path.isfile(man_path)):
        problems.append("T0 %s: missing glb/manifest under %s" % (name, out_dir))
        return None
    manifest = json.load(open(man_path))
    js, bin_ = load(glb_path)

    # skeleton: 26 joints, canonical names/parents, IBM translations
    joints = js["skins"][0]["joints"]
    if len(joints) != 26:
        problems.append("T0 %s: %d joints != 26" % (name, len(joints)))
    node_names = [js["nodes"][j].get("name") for j in joints]
    canon = [n for (n, _p, _h) in cb.CANONICAL_SKELETON]
    if node_names != canon:
        problems.append("T0 %s: joint order/name mismatch vs CANONICAL_SKELETON"
                        % name)
    parent_of = {}
    for i, nd in enumerate(js["nodes"]):
        for c in nd.get("children", []):
            parent_of[c] = i
    for (bname, bparent, bhead) in cb.CANONICAL_SKELETON:
        ji = joints[canon.index(bname)]
        got_parent = parent_of.get(ji)
        want_parent = None if bparent is None else joints[canon.index(bparent)]
        if got_parent != want_parent:
            problems.append("T0 %s: bone %s parent mismatch" % (name, bname))
    ibms = read_accessor(js, bin_, js["skins"][0]["inverseBindMatrices"])
    for k, (bname, _bp, bhead) in enumerate(cb.CANONICAL_SKELETON):
        tx, ty, tz = ibms[k][12], ibms[k][13], ibms[k][14]
        back = gl_to_ue((-tx, -ty, -tz))
        if max(abs(back[i] - bhead[i]) for i in range(3)) > 0.01:
            problems.append("T0 %s: IBM translation off for %s" % (name, bname))
            break

    # slots in canonical order; tri counts match the manifest
    mat_names = [m["name"] for m in js["materials"]]
    if tuple(mat_names) != cb.SLOT_ORDER:
        problems.append("T0 %s: material order %s != %s"
                        % (name, mat_names, list(cb.SLOT_ORDER)))
    tri_sum = 0
    lo = [1e9] * 3
    hi = [-1e9] * 3
    for prim in js["meshes"][0]["primitives"]:
        idx = read_accessor(js, bin_, prim["indices"])
        slot = mat_names[prim["material"]]
        n_tris = len(idx) // 3
        tri_sum += n_tris
        if manifest["slots"].get(slot) != n_tris:
            problems.append("T0 %s: slot %s tris %d != manifest %s"
                            % (name, slot, n_tris, manifest["slots"].get(slot)))
        for p in read_accessor(js, bin_, prim["attributes"]["POSITION"]):
            q = gl_to_ue(p)
            if any(abs(c) > 1e6 or c != c for c in q):
                problems.append("T0 %s: non-finite position" % name)
                break
            lo = [min(lo[i], q[i]) for i in range(3)]
            hi = [max(hi[i], q[i]) for i in range(3)]
    if tri_sum != manifest["tris"]:
        problems.append("T0 %s: %d tris != manifest %d"
                        % (name, tri_sum, manifest["tris"]))
    mb = manifest["bounds_uu"]
    for i in range(3):
        if abs(lo[i] - mb["min"][i]) > 0.1 or abs(hi[i] - mb["max"][i]) > 0.1:
            problems.append("T0 %s: re-read bounds drift vs manifest" % name)
            break

    # deterministic rebuild == the file on disk (mask source == import source).
    # NB the rebuild keeps the same file stem in a temp subdir -- the stem is
    # embedded in the GLB (mesh/skin names), so a different name would change
    # the bytes and sha1 for no real reason.
    w = TraceGlbWriter()
    cb.feed_writer(w, body)
    tmp_dir = os.path.join(out_dir, ".verify_tmp")
    os.makedirs(tmp_dir, exist_ok=True)
    tmp = os.path.join(tmp_dir, name.lower() + ".glb")
    rebuilt = w.write(tmp)
    for junk in (tmp, os.path.join(tmp_dir, name.lower() + "_manifest.json")):
        if os.path.exists(junk):
            os.remove(junk)
    try:
        os.rmdir(tmp_dir)
    except OSError:
        pass
    if rebuilt["sha1"] != manifest["sha1"]:
        problems.append("T0 %s: rebuild sha1 %s != on-disk %s (recipe drifted "
                        "after generation -- rerun generate_characters.py)"
                        % (name, rebuilt["sha1"][:12], manifest["sha1"][:12]))
    return manifest


# ---------------------------------------------------------------------------
# T1: masks
# ---------------------------------------------------------------------------


def rasterize(parts, view, skip_names=()):
    """Binary orthographic mask.  view: 'front' projects (X, Z) looking down
    -Y; 'side' projects (Y, Z) looking down -X (left side view); 'rear' is
    the X-mirrored front projection (the silhouette seen from behind)."""
    grid = bytearray(CANVAS_W * CANVAS_H)
    for p in parts:
        if p["name"] in skip_names:
            continue
        mesh = p["mesh"]
        for (a, b, c) in mesh.tris:
            tri = []
            for vi in (a, b, c):
                x, y, z = mesh.positions[vi]
                if view == "front":
                    u = x
                elif view == "rear":
                    u = -x
                else:
                    u = y
                tri.append(((u - ORIGIN_X) / PX, (z - ORIGIN_Z) / PX))
            (x0, y0), (x1, y1), (x2, y2) = tri
            area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)
            if abs(area) < 1e-12:
                continue
            lo_px = max(0, int(min(x0, x1, x2)))
            hi_px = min(CANVAS_W - 1, int(max(x0, x1, x2)) + 1)
            lo_py = max(0, int(min(y0, y1, y2)))
            hi_py = min(CANVAS_H - 1, int(max(y0, y1, y2)) + 1)
            for py in range(lo_py, hi_py + 1):
                cy = py + 0.5
                for px_ in range(lo_px, hi_px + 1):
                    cx = px_ + 0.5
                    w0 = (x1 - x0) * (cy - y0) - (y1 - y0) * (cx - x0)
                    w1 = (x2 - x1) * (cy - y1) - (y2 - y1) * (cx - x1)
                    w2 = (x0 - x2) * (cy - y2) - (y0 - y2) * (cx - x2)
                    if area > 0:
                        inside = w0 >= -1e-9 and w1 >= -1e-9 and w2 >= -1e-9
                    else:
                        inside = w0 <= 1e-9 and w1 <= 1e-9 and w2 <= 1e-9
                    if inside:
                        grid[py * CANVAS_W + px_] = 1
    return grid


def write_png(path, grid):
    """Minimal 8-bit grayscale PNG (stock zlib), rows top-down."""
    def chunk(tag, payload):
        raw = tag + payload
        return (struct.pack(">I", len(payload)) + raw +
                struct.pack(">I", zlib.crc32(raw) & 0xFFFFFFFF))

    rows = b""
    for py in range(CANVAS_H - 1, -1, -1):  # +Z up -> top row first
        row = bytes(255 if grid[py * CANVAS_W + x] else 0
                    for x in range(CANVAS_W))
        rows += b"\x00" + row
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", CANVAS_W, CANVAS_H,
                                      8, 0, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows, 9)) +
           chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def write_png_scaled(path, grid, w, h, scale=1):
    """Grayscale PNG of an arbitrary w x h grid, nearest-neighbour upscaled.
    (write_png above stays the raw 56 x 60 archive form; this is the one the
    contact sheets use so a 46 px silhouette can actually be LOOKED at.)"""
    def chunk(tag, payload):
        raw = tag + payload
        return (struct.pack(">I", len(payload)) + raw +
                struct.pack(">I", zlib.crc32(raw) & 0xFFFFFFFF))

    ow, oh = w * scale, h * scale
    rows = b""
    for py in range(h - 1, -1, -1):
        row = bytes(255 if grid[py * w + x] else 0
                    for x in range(w) for _ in range(scale))
        rows += (b"\x00" + row) * scale
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 0, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows, 9)) +
           chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def compose(grids, gap=2, rule=1):
    """Lay masks left-to-right on one canvas with `gap` px of background and a
    `rule` px separator column, returning (grid, w, h)."""
    n = len(grids)
    w = n * CANVAS_W + (n - 1) * (gap * 2 + rule)
    out = bytearray(w * CANVAS_H)
    x0 = 0
    for k, g in enumerate(grids):
        for py in range(CANVAS_H):
            for px_ in range(CANVAS_W):
                if g[py * CANVAS_W + px_]:
                    out[py * w + x0 + px_] = 1
        x0 += CANVAS_W
        if k < n - 1:
            for py in range(CANVAS_H):
                for r in range(rule):
                    out[py * w + x0 + gap + r] = 1
            x0 += gap * 2 + rule
    return out, w, CANVAS_H


def mask_count(grid):
    return sum(grid)


def centroid(grid):
    n = xs = ys = 0
    for py in range(CANVAS_H):
        for px_ in range(CANVAS_W):
            if grid[py * CANVAS_W + px_]:
                n += 1
                xs += px_
                ys += py
    return (xs / float(n), ys / float(n)) if n else (0.0, 0.0)


def iou_centroid_aligned(a, b):
    """Pairwise IoU with mask b integer-shifted so centroids coincide."""
    (ax, ay), (bx, by) = centroid(a), centroid(b)
    dx, dy = int(round(ax - bx)), int(round(ay - by))
    inter = union = 0
    for py in range(CANVAS_H):
        for px_ in range(CANVAS_W):
            va = a[py * CANVAS_W + px_]
            qx, qy = px_ - dx, py - dy
            vb = (b[qy * CANVAS_W + qx]
                  if 0 <= qx < CANVAS_W and 0 <= qy < CANVAS_H else 0)
            if va or vb:
                union += 1
                if va and vb:
                    inter += 1
    return inter / float(union) if union else 1.0


def t1_masks(name, body, masks_dir, problems, sheets_dir=None, scale=8):
    views = {}
    for view in ("front", "side", "rear"):
        grid = rasterize(body["parts"], view)
        views[view] = grid
        write_png(os.path.join(masks_dir, "%s_%s.png" % (name.lower(), view)),
                  grid)
    if sheets_dir:
        # the three-view sheet is the deliverable a human judges: front |
        # side | rear at 46 px, upscaled so the crown/shoulder/hip breaks the
        # sheets promise are actually visible
        sheet, w, h = compose([views["front"], views["side"], views["rear"]])
        write_png_scaled(os.path.join(sheets_dir, "%s_sheet.png"
                                      % name.lower()), sheet, w, h, scale)
    # (a) signature features contribute >= 8 px^2 in >= 1 mask
    for feat, part_names in body["features"].items():
        best = 0
        for view in ("front", "side", "rear"):
            without = rasterize(body["parts"], view, skip_names=set(part_names))
            contrib = sum(1 for i in range(CANVAS_W * CANVAS_H)
                          if views[view][i] and not without[i])
            best = max(best, contrib)
        if best < 8:
            problems.append("T1 %s: feature %r contributes %d px^2 < 8"
                            % (name, feat, best))
    return views["front"]


# ---------------------------------------------------------------------------
# T4: budgets
# ---------------------------------------------------------------------------


def t4_budgets(name, body, manifest, problems):
    row = cb.CHARACTERS[name]
    if manifest["accent_area_pct"] > row["accent_cap_pct"]:
        problems.append("T4 %s: accent %.2f%% > cap %.1f%%"
                        % (name, manifest["accent_area_pct"],
                           row["accent_cap_pct"]))
    if manifest["accent_area_uu2"] >= manifest["team_area_uu2"]:
        problems.append("T4 %s: accent area >= team area" % name)
    rings = nodes = 0
    for op in body["ops"]:
        if not (8.0 <= float(op["width"]) <= 10.0):
            problems.append("T4 %s: op %r width %s outside [8, 10]"
                            % (name, op["op"], op["width"]))
        if op["kind"] in ("LINE", "DASH") and \
                op["termination"] not in ("node", "edge"):
            problems.append("T4 %s: open op %r not node/edge-terminated"
                            % (name, op["op"]))
        if op["kind"] == "SERVICE_RING":
            rings += 1
    part_names = [p["name"] for p in body["parts"]]
    nodes = sum(1 for n in part_names if "node" in n and n.startswith("acc_"))
    if rings != 1:
        problems.append("T4 %s: %d service rings != 1" % (name, rings))
    if nodes < 2:
        problems.append("T4 %s: %d node pads < 2" % (name, nodes))
    ring_side = row["service_ring"]
    if ring_side and not any(
            p["name"] == "acc_service_ring" for p in body["parts"]):
        problems.append("T4 %s: service ring part missing" % name)


# ---------------------------------------------------------------------------
# T5: envelopes
# ---------------------------------------------------------------------------

ARM_BONES = {"upperarm_l", "lowerarm_l", "hand_l",
             "upperarm_r", "lowerarm_r", "hand_r"}
BACK_PLANE_Y = -12.0     # torso back shell (chest D 24 about the spine)
CHEST_PLANE_Y = 12.0


def t5_envelopes(name, body, problems):
    lat_max = cb.LATERAL_ENVELOPE[name]
    min_z = 1e9
    max_z_body = -1e9
    for p in body["parts"]:
        (lo, hi) = p["mesh"].bounds()
        min_z = min(min_z, lo[2])
        if not p["crown_break"]:
            max_z_body = max(max_z_body, hi[2])
        lat = max(abs(lo[0]), abs(hi[0]))
        if p["crown_break"]:
            if hi[2] > 208.0 + 1e-6:
                problems.append("T5 %s: crown-break part %r above Z 208"
                                % (name, p["name"]))
            if p["thickness"] is None or p["thickness"] > 8.0:
                problems.append("T5 %s: crown-break part %r thickness %s "
                                "(must declare <= 8)"
                                % (name, p["name"], p["thickness"]))
        elif hi[2] > 176.0 + 0.5 and p["thickness"] is None:
            problems.append("T5 %s: part %r exceeds Z 176 without crown_break"
                            % (name, p["name"]))
        if p["bone"] not in ARM_BONES:
            if lat > lat_max + 1e-6:
                problems.append("T5 %s: part %r lateral %.1f > +/-%.0f"
                                % (name, p["name"], lat, lat_max))
        if (lat > 36.0 or hi[2] > 176.0) and not p["crown_break"]:
            if p["thickness"] is not None and p["thickness"] > 12.0:
                problems.append("T5 %s: part %r beyond +/-36/Z176 with "
                                "thickness %.1f > 12"
                                % (name, p["name"], p["thickness"]))
        # SS4.9 / A1 swing envelopes for tagged gear
        region = p["gear_region"]
        if region == "back":
            if lo[1] < BACK_PLANE_Y - 26.0 - 1e-6:
                problems.append("T5 %s: back gear %r %.1f uu proud > 26"
                                % (name, p["name"], BACK_PLANE_Y - lo[1]))
            if lat > 30.0 + 1e-6 and hi[2] <= 176.0:
                problems.append("T5 %s: back gear %r beyond +/-30 lateral"
                                % (name, p["name"]))
        elif region == "front":
            if hi[1] > CHEST_PLANE_Y + 10.0 + 1e-6:
                problems.append("T5 %s: front gear %r proud > 10 uu"
                                % (name, p["name"]))
        elif region == "pelvis_side":
            if lat > 26.0 + 1e-6:
                problems.append("T5 %s: pelvis-side gear %r beyond 26 lateral"
                                % (name, p["name"]))
        elif region == "thigh":
            if lat > 20.0 + 1e-6:
                problems.append("T5 %s: thigh gear %r beyond 20 lateral"
                                % (name, p["name"]))
    height = max_z_body - min_z
    if abs(height - 176.0) > 0.5:
        problems.append("T5 %s: height %.2f outside 176 +/- 0.5" % (name, height))
    if abs(min_z) > 0.5:
        problems.append("T5 %s: feet min Z %.2f outside 0 +/- 0.5"
                        % (name, min_z))


# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--names", default=None)
    ap.add_argument("--dir", default=os.path.join(_ROOT, "Intermediate",
                                                  "Characters"))
    ap.add_argument("--masks-dir", default=None)
    ap.add_argument("--sheets-dir", default=None,
                    help="also write <name>_sheet.png (front|side|rear, "
                         "upscaled) plus roster_front.png here")
    ap.add_argument("--sheet-scale", type=int, default=8)
    args = ap.parse_args()
    names = ([n.strip() for n in args.names.split(",")] if args.names
             else [n for n in cb.CHARACTER_ORDER if n in cb.BODY_BUILDERS])
    masks_dir = args.masks_dir or os.path.join(args.dir, "masks")
    os.makedirs(masks_dir, exist_ok=True)
    if args.sheets_dir:
        os.makedirs(args.sheets_dir, exist_ok=True)

    problems = []
    fronts = {}
    for name in names:
        body = cb.build_body(name)
        before = len(problems)
        manifest = t0_reread(name, body, args.dir, problems)
        print("[verify-silhouettes] %-10s T0 %s" %
              (name, "PASS" if len(problems) == before else "FAIL"))
        if manifest is None:
            continue
        before = len(problems)
        fronts[name] = t1_masks(name, body, masks_dir, problems,
                                sheets_dir=args.sheets_dir,
                                scale=args.sheet_scale)
        print("[verify-silhouettes] %-10s T1(features) %s" %
              (name, "PASS" if len(problems) == before else "FAIL"))
        before = len(problems)
        t4_budgets(name, body, manifest, problems)
        print("[verify-silhouettes] %-10s T4 %s  (accent %.2f%% cap %.1f%%; "
              "team %.0f uu2 > accent %.0f uu2)" %
              (name, "PASS" if len(problems) == before else "FAIL",
               manifest["accent_area_pct"],
               cb.CHARACTERS[name]["accent_cap_pct"],
               manifest["team_area_uu2"], manifest["accent_area_uu2"]))
        before = len(problems)
        t5_envelopes(name, body, problems)
        print("[verify-silhouettes] %-10s T5 %s" %
              (name, "PASS" if len(problems) == before else "FAIL"))

    # T1(b): pairwise front-mask IoU <= 0.88, centroid-aligned
    order = [n for n in names if n in fronts]
    for i in range(len(order)):
        for j in range(i + 1, len(order)):
            v = iou_centroid_aligned(fronts[order[i]], fronts[order[j]])
            ok = v <= 0.88
            if not ok:
                problems.append("T1 IoU %s/%s = %.3f > 0.88"
                                % (order[i], order[j], v))
            print("[verify-silhouettes] IoU %s/%s = %.3f (%s; limit 0.88)"
                  % (order[i], order[j], v, "PASS" if ok else "FAIL"))

    if args.sheets_dir and len(order) > 1:
        sheet, w, h = compose([fronts[n] for n in order])
        write_png_scaled(os.path.join(args.sheets_dir, "roster_front.png"),
                         sheet, w, h, max(2, args.sheet_scale // 2))
        print("[verify-silhouettes] sheets in %s" % args.sheets_dir)

    for p in problems:
        print("[verify-silhouettes] PROBLEM: %s" % p)
    print("[verify-silhouettes] masks in %s" % masks_dir)
    print("[verify-silhouettes] EXIT=%d" % (1 if problems else 0))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
