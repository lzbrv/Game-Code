#!/usr/bin/env python3
# =============================================================================
# Trace - rebake-translate-census.py
#
# CARRIES A REBAKE CENSUS ACROSS A RESIZE OF THE ARENA, so that
# Scripts/rebake-arena-preserving.py can re-bake Arena_Baked at a NEW field size
# without dragging builder geometry back to where the OLD size had it.
#
#   python3 Scripts/rebake-translate-census.py CENSUS_A P_OLD P_NEW OUT \
#       [--policy=spread|fixed] [--declared=Top_Centre_Tower_2,...] \
#       [--oracle=PREDICTED.json] [--max-age-hours=6]
#
#   CENSUS_A  census of the live map (rebake phase=census) at the OLD size.
#   P_OLD     census of a FRESH force-bake of the OLD builder onto a throwaway
#             probe map (bake-arena.sh --force --map /Game/Maps/<probe>): what
#             CENSUS_A's pieces came out of.
#   P_NEW     census of a fresh force-bake of the NEW builder onto a probe, made
#             by the SAME binary the real bake will use.
#   OUT       the translated census to hand the restore (TRACE_REBAKE_CENSUS).
#
# -----------------------------------------------------------------------------
# WHY THIS EXISTS - WHAT AN UNTRANSLATED CENSUS DOES TO A RESIZED ARENA
# -----------------------------------------------------------------------------
# The restore honours HAND EDITS AS TRANSFORMS: it pairs every census piece
# with the nearest fresh piece of its family and, if the pair is more than
# 0.5 uu apart and the move is not shared by most of the family, it puts the
# piece BACK where the census had it ("isolated" = a person's drag). That rule
# is right for a builder change of materials or a densified family. It is wrong
# for a resize, and measured against the 2026-10-04 x1.10 (38400 x 9600 ->
# 42240 x 10560) an untranslated restore would have:
#
#   * dragged Wall_East/West 1920 uu and Wall_North/South 480 uu back inward -
#     every ONE-MEMBER family is "isolated" by arithmetic - leaving the end wall
#     480 uu behind the goal and the spawn fan outside the arena;
#   * dragged Tower_Beacon 260 uu off its tower;
#   * DELETED the 8 pieces the longer field adds (Grid_X x4, Wall_Rib_Y x4) as
#     "hand-deleted", because no census sibling stands within 400 uu of them;
#   * lost the nine hand-copied top-centre tower pieces as "vanished" (their
#     mirror distance grows past the 250 uu proof);
#   * put the hand-placed side ramps back 480 uu off the new wall;
# and still printed BALANCED: YES.
#
# The cure is to subtract the builder's own displacement before the restore
# sees the census: for every census piece, find what it was baked FROM in P_OLD
# (with the restore's OWN matcher, imported below), take the piece with the
# same label in P_NEW, and carry only the HAND OFFSET (census - P_OLD) onto it.
# Every builder displacement becomes 0, and the restore's rules then see only
# genuine human edits - exactly what they were written for.
#
# EVERY ASSUMPTION THAT IS NOT CHECKED HERE IS A STOP:
#   * a P_OLD piece with no census counterpart that is not a corner bank
#     (an unknown hand deletion);
#   * census pieces with no P_OLD counterpart other than --declared;
#   * a family whose MEMBER COUNT changes that also carries a hand edit;
#   * a rotation that differs between P_OLD and P_NEW for one label;
#   * with --oracle, any P_OLD -> P_NEW displacement more than 1 uu from an
#     independently PREDICTED position (labels are assumed stable across the
#     resize because emission order is; the oracle is what proves it);
#   * an input census older than --max-age-hours (the restore's own guard,
#     enforced here too, and captured_unix is COPIED, never refreshed, so a
#     translation cannot launder a stale census).
#
# THE HAND LAYER is moved by --policy:
#   spread (default)  each centre-kit actor's VISUAL centre (bounds origin) is
#                     scaled about the arena centre by the wall ratio; KitLip
#                     strips move rigidly with the platform their label names;
#                     lights scale by their location. Sizes never change.
#   fixed             the centre kit stays exactly where it is.
#   The side ramps (label SideRamp_*) are always RE-DERIVED from the new side
#   wall - toe at HalfWidth - kCrestOutFromWallUU - kDepthUU, both read out of
#   Source/Trace/World/TraceSideRampProfile.h - whatever the policy.
#
# Pure Python: no editor, no `unreal`. It never writes anywhere but OUT.
# =============================================================================

import copy
import importlib.util
import json
import os
import re
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
PROFILE_HEADER = os.path.join(PROJECT_ROOT, "Source", "Trace", "World", "TraceSideRampProfile.h")


def load_restore_module():
    """The restore's own matcher. Imported, never copied: if its pairing rules
    change, this translation changes with them."""
    path = os.path.join(SCRIPT_DIR, "rebake-arena-preserving.py")
    spec = importlib.util.spec_from_file_location("trace_rebake_preserving", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for name in ("label_stem", "match_pieces", "donor_for_duplicate", "is_allowlisted"):
        if not hasattr(module, name):
            stop("{0} has no module-scope {1}() - the matcher has moved".format(path, name))
    return module


def stop(message):
    sys.stderr.write("TRANSLATE STOP: {0}\n".format(message))
    raise SystemExit(2)


def read_profile_constant(name):
    text = open(PROFILE_HEADER, "r", encoding="utf-8").read()
    match = re.search(r"inline\s+constexpr\s+\w+\s+" + name + r"\s*=\s*([0-9.eE+-]+)\s*;", text)
    if not match:
        stop("could not read {0} from {1}".format(name, PROFILE_HEADER))
    return float(match.group(1))


def wall_half_extent(pieces, stem, axis, stem_of):
    """Inner face of a wall family: |centre| - half its thickness."""
    walls = [p for p in pieces if stem_of(p["label"]) == stem]
    if not walls:
        stop("no {0} piece in the probe census".format(stem))
    wall = max(walls, key=lambda p: abs(p["location"][axis]))
    return abs(wall["location"][axis]) - wall["bounds_extent"][axis]


def main(argv):
    positional = [a for a in argv[1:] if not a.startswith("--")]
    if len(positional) != 4:
        sys.stderr.write(__doc__ or "usage: CENSUS_A P_OLD P_NEW OUT [options]\n")
        stop("need exactly four paths: CENSUS_A P_OLD P_NEW OUT")
    a_path, o_path, n_path, out_path = positional
    opts = {"policy": "spread", "declared": "", "oracle": "", "max-age-hours": "6"}
    for arg in argv[1:]:
        if not arg.startswith("--"):
            continue
        key, _, value = arg[2:].partition("=")
        if key not in opts:
            stop("unknown option --{0}".format(key))
        opts[key] = value
    if opts["policy"] not in ("spread", "fixed"):
        stop("--policy must be spread or fixed")

    rb = load_restore_module()
    stem_of = rb.label_stem

    census_a = json.load(open(a_path))
    probe_old = json.load(open(o_path))
    probe_new = json.load(open(n_path))
    a, o, n = census_a["census_a"], probe_old["census_a"], probe_new["census_a"]
    declared = set(x.strip() for x in opts["declared"].split(",") if x.strip())
    report = []

    # ---- 0. the census must be fresh, and stays stamped with ITS OWN time ----
    captured = census_a.get("captured_unix")
    if captured is None:
        stop("{0} has no captured_unix; re-run the census phase".format(a_path))
    age_hours = (time.time() - captured) / 3600.0
    if age_hours > float(opts["max-age-hours"]):
        stop("{0} was captured {1} ({2:.1f} h ago, limit {3}); re-run the census phase".format(
            a_path, census_a.get("captured_utc"), age_hours, opts["max-age-hours"]))
    report.append("census A captured {0} ({1:.2f} h ago)".format(census_a.get("captured_utc"), age_hours))

    # ---- 1. census A against the OLD fresh bake: what are the hand edits? ---
    pairs, a_only, o_only = rb.match_pieces(a["pieces"], o["pieces"])
    moved = [(pa, po, d) for pa, po, d in pairs if d > 0.5]
    report.append("A vs P_old: {0} pairs ({1} at 0 uu), {2} census-only, {3} P_old-only".format(
        len(pairs), len(pairs) - len(moved), len(a_only), len(o_only)))
    for pa, po, d in moved:
        report.append("  hand offset  {0:<26} vs {1:<26} {2:.1f} uu".format(pa["label"], po["label"], d))
    unknown_deleted = [p["label"] for p in o_only if rb.is_allowlisted(p["label"]) != "BANK"]
    if unknown_deleted:
        stop("P_old pieces with no census counterpart that are not corner banks (an unknown hand "
             "deletion): {0}".format(unknown_deleted[:20]))
    if set(p["label"] for p in a_only) != declared:
        stop("census-only pieces {0} != --declared hand duplicates {1}".format(
            sorted(p["label"] for p in a_only), sorted(declared)))

    # ---- 2. P_old -> P_new by label, family by family -----------------------
    o_by, n_by = {}, {}
    for p in o["pieces"]:
        o_by.setdefault(stem_of(p["label"]), []).append(p)
    for p in n["pieces"]:
        n_by.setdefault(stem_of(p["label"]), []).append(p)
    n_label = {p["label"]: p for p in n["pieces"]}
    resized = sorted(s for s in set(o_by) | set(n_by) if len(o_by.get(s, [])) != len(n_by.get(s, [])))
    report.append("families resized by the builder (P_old -> P_new): {0}".format(
        ", ".join("{0} {1}->{2}".format(s, len(o_by.get(s, [])), len(n_by.get(s, []))) for s in resized)
        or "none"))
    for s in resized:
        if rb.is_allowlisted(s) == "BANK":
            continue
        if any(stem_of(pa["label"]) == s for pa, _po, _d in moved):
            stop("resized family {0} carries a hand edit; it cannot be carried member by member".format(s))

    oracle = None
    if opts["oracle"]:
        oracle = json.load(open(opts["oracle"]))
        oracle_misses = []
        oracle_checked = 0
        for p in o["pieces"]:
            s = stem_of(p["label"])
            if s in resized or rb.is_allowlisted(p["label"]) == "BANK":
                continue
            want = oracle.get(p["label"])
            got = n_label.get(p["label"])
            if want is None or got is None:
                oracle_misses.append("{0}: {1}".format(p["label"], "no oracle row" if want is None else "not in P_new"))
                continue
            oracle_checked += 1
            off = max(abs(got["location"][i] - want[i]) for i in range(3))
            if off > 1.0:
                oracle_misses.append("{0}: P_new {1} vs predicted {2} ({3:.1f} uu)".format(
                    p["label"], [round(v, 1) for v in got["location"]], [round(v, 1) for v in want], off))
        report.append("oracle: {0} P_old->P_new label pairs checked, {1} off by more than 1 uu".format(
            oracle_checked, len(oracle_misses)))
        if oracle_misses:
            stop("P_new disagrees with the predicted layout:\n  " + "\n  ".join(oracle_misses[:40]))

    out = copy.deepcopy(census_a)
    out_a = out["census_a"]
    new_pieces = []
    for pa, po, _d in pairs:
        s = stem_of(po["label"])
        if s in resized:
            continue
        pn = n_label.get(po["label"])
        if pn is None:
            stop("P_new has no {0}".format(po["label"]))
        if [round(v, 3) for v in pn["rotation"]] != [round(v, 3) for v in po["rotation"]]:
            stop("rotation of {0} changed between P_old and P_new".format(po["label"]))
        rec = copy.deepcopy(pa)
        offset = [pa["location"][i] - po["location"][i] for i in range(3)]
        rec["location"] = [pn["location"][i] + offset[i] for i in range(3)]
        rec["bounds_origin"] = [pn["bounds_origin"][i] + (pa["bounds_origin"][i] - po["bounds_origin"][i])
                                for i in range(3)]
        rec["bounds_extent"] = list(pn["bounds_extent"])
        rec["_translation"] = {"p_old": po["label"], "p_new": pn["label"], "hand_offset": offset}
        new_pieces.append(rec)
    for s in resized:
        if rb.is_allowlisted(s) == "BANK":
            continue
        for pn in n_by.get(s, []):
            rec = copy.deepcopy(pn)
            rec["_translation"] = {"resized_family_taken_from_p_new": True}
            new_pieces.append(rec)

    # Hand DUPLICATES follow the displacement of the piece they copy, mirrored the
    # same way the restore's donor search mirrors them.
    mirrors = {name: (sx, sy) for name, sx, sy in rb.MIRRORS}
    for pa in a_only:
        donor, mirror, distance = rb.donor_for_duplicate(pa, o_by)
        if donor is None:
            stop("no donor for declared duplicate {0}".format(pa["label"]))
        pn = n_label.get(donor["label"])
        if pn is None:
            stop("P_new has no {0} (donor of {1})".format(donor["label"], pa["label"]))
        mx, my = mirrors[mirror]
        delta = [pn["location"][i] - donor["location"][i] for i in range(3)]
        rec = copy.deepcopy(pa)
        rec["location"] = [pa["location"][0] + mx * delta[0], pa["location"][1] + my * delta[1],
                           pa["location"][2] + delta[2]]
        rec["bounds_origin"] = [pa["bounds_origin"][0] + mx * delta[0], pa["bounds_origin"][1] + my * delta[1],
                                pa["bounds_origin"][2] + delta[2]]
        rec["_translation"] = {"hand_duplicate_of": donor["label"], "mirror": mirror,
                               "mirror_distance_old": distance}
        new_pieces.append(rec)
        report.append("  duplicate    {0:<26} donor {1:<22} {2} {3:.1f} uu, moved ({4:.1f}, {5:.1f})".format(
            pa["label"], donor["label"], mirror, distance, mx * delta[0], my * delta[1]))
    out_a["pieces"] = new_pieces

    # ---- 3. the hand layer, by policy ---------------------------------------
    old_hx = wall_half_extent(o["pieces"], "Wall_East", 0, stem_of)
    new_hx = wall_half_extent(n["pieces"], "Wall_East", 0, stem_of)
    old_hy = wall_half_extent(o["pieces"], "Wall_North", 1, stem_of)
    new_hy = wall_half_extent(n["pieces"], "Wall_North", 1, stem_of)
    scale_x, scale_y = new_hx / old_hx, new_hy / old_hy
    report.append("inner wall faces: |X| {0:.1f} -> {1:.1f} (x{2:.4f}), |Y| {3:.1f} -> {4:.1f} (x{5:.4f})".format(
        old_hx, new_hx, scale_x, old_hy, new_hy, scale_y))
    crest_out = read_profile_constant("kCrestOutFromWallUU")
    depth = read_profile_constant("kDepthUU")
    ramp_length = read_profile_constant("kLengthUU")
    if abs(ramp_length - 2.0 * new_hx) > 1.0:
        stop("TraceSideRampProfile.h kLengthUU {0} != the new field length {1}".format(ramp_length, 2.0 * new_hx))

    by_label = {h["label"]: h for h in a["hand"]}
    lip = re.compile(r"^KitLip_(Kit_Platform_\d\d)_\d+$")
    hands = []
    for h in a["hand"]:
        rec = copy.deepcopy(h)
        label = h["label"]
        if label.startswith("SideRamp_"):
            sign = -1.0 if h["location"][1] < 0 else 1.0
            toe = new_hy - crest_out - depth
            delta = [0.0, sign * toe - h["location"][1], 0.0]
            rule = "side ramp: toe re-derived from the new wall"
        elif opts["policy"] == "fixed":
            delta = [0.0, 0.0, 0.0]
            rule = "fixed"
        elif lip.match(label):
            parent = by_label.get(lip.match(label).group(1))
            if parent is None:
                stop("{0} names a parent platform that is not in the census".format(label))
            delta = [(scale_x - 1.0) * parent["bounds_origin"][0], (scale_y - 1.0) * parent["bounds_origin"][1], 0.0]
            rule = "rigid with " + parent["label"]
        elif h["class"] == "PointLight":
            delta = [(scale_x - 1.0) * h["location"][0], (scale_y - 1.0) * h["location"][1], 0.0]
            rule = "light: location spread"
        else:
            delta = [(scale_x - 1.0) * h["bounds_origin"][0], (scale_y - 1.0) * h["bounds_origin"][1], 0.0]
            rule = "visual centre spread"
        rec["location"] = [h["location"][i] + delta[i] for i in range(3)]
        rec["bounds_origin"] = [h["bounds_origin"][i] + delta[i] for i in range(3)]
        if label.startswith("SideRamp_"):
            rec["bounds_extent"] = [new_hx + (h["bounds_extent"][0] - old_hx), h["bounds_extent"][1],
                                    h["bounds_extent"][2]]
        rec["_translation"] = {"delta": delta, "rule": rule}
        hands.append(rec)
        report.append("  hand         {0:<26} ({1:.1f}, {2:.1f}) -> ({3:.1f}, {4:.1f})  [{5}]".format(
            label, h["location"][0], h["location"][1], rec["location"][0], rec["location"][1], rule))
    out_a["hand"] = hands

    out["translated"] = {
        "tool": "Scripts/rebake-translate-census.py",
        "translated_utc": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
        "census_a": os.path.abspath(a_path), "p_old": os.path.abspath(o_path),
        "p_new": os.path.abspath(n_path), "oracle": opts["oracle"] or None,
        "policy": opts["policy"], "declared": sorted(declared),
        "note": "captured_unix/captured_utc are census A's own, copied verbatim. census_a.others is "
                "census A's, untouched (the restore never reads it).",
        "report": report,
    }
    with open(out_path, "w") as handle:
        json.dump(out, handle, indent=1, sort_keys=True)
    print("\n".join(report))
    print("translated census: {0} pieces (census A had {1}), {2} hand actors -> {3}".format(
        len(new_pieces), len(a["pieces"]), len(hands), out_path))


if __name__ == "__main__":
    main(sys.argv)
