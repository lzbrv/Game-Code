#!/usr/bin/env python3
"""generate_characters.py -- stage 0 of the character pipeline (no editor).

Spec: PIPELINE_DESIGN.md SS5.2.  For each character with a recipe in
`character_bodies.BODY_BUILDERS`: build_body -> writer self-checks (SS2.5)
-> write `Intermediate/Characters/<lowername>.glb` + `<lowername>_manifest.json`.

Prints one verdict line per character and `[generate-characters] EXIT=0/1`
(the grep-verdict convention, generate-data-assets.py:38-43 -- wrappers grep
the verdict, never the exit code of an editor; this script also exits
non-zero on failure since it is plain python).

Intermediate/ is derived output, like Intermediate/Railgun (git-ignored).
The manifest carries exactly the SS5.2 keys plus the recipe metadata
(accent_ops, features, part table) that `verify_silhouettes.py` asserts T4/T5
against and that the wave-3 import stage cross-checks (PIPELINE SS6.5).

Usage:
    python3 Scripts/generate_characters.py [--names Rocco,Lily] [--out DIR]
"""
import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, _HERE)

import character_bodies as cb  # noqa: E402
from trace_glb import TraceGlbWriter, GlbCheckError  # noqa: E402


def generate_one(name, out_dir):
    body = cb.build_body(name)
    row = cb.CHARACTERS[name]
    writer = TraceGlbWriter()
    cb.feed_writer(writer, body)
    path = os.path.join(out_dir, name.lower() + ".glb")
    manifest = writer.write(path)  # SS2.5 self-checks run inside

    # recipe-level assertions the writer cannot know (per-character ceilings)
    problems = []
    if manifest["tris"] > row["tri_ceiling"]:
        problems.append("tris %d > ceiling %d" % (manifest["tris"],
                                                  row["tri_ceiling"]))
    if manifest["accent_area_pct"] > row["accent_cap_pct"]:
        problems.append("accent %.2f%% > cap %.1f%%"
                        % (manifest["accent_area_pct"], row["accent_cap_pct"]))
    if manifest["accent_area_uu2"] >= manifest["team_area_uu2"]:
        problems.append("accent area %.0f >= team area %.0f"
                        % (manifest["accent_area_uu2"],
                           manifest["team_area_uu2"]))
    if manifest["bones"] != 26:
        problems.append("bones %d != 26" % manifest["bones"])
    if problems:
        raise GlbCheckError("; ".join(problems))

    # extend the writer manifest with the recipe metadata (SS5.2 keys stay)
    manifest.update({
        "name": name,
        "height_uu": manifest["height_uu"],
        "tri_ceiling": row["tri_ceiling"],
        "accent_cap_pct": row["accent_cap_pct"],
        "accent_linear": list(row["accent"]),
        "accent_hex": row["hex"],
        "roughness": row["roughness"],
        "service_ring": row["service_ring"],
        "accent_ops": body["ops"],
        "features": body["features"],
        "parts": [{"name": p["name"], "bone": p["bone"], "slot": p["slot"],
                   "tris": len(p["mesh"].tris),
                   "crown_break": p["crown_break"],
                   "thickness": p["thickness"],
                   "gear_region": p["gear_region"]}
                  for p in body["parts"]],
    })
    mpath = os.path.join(out_dir, name.lower() + "_manifest.json")
    with open(mpath, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    return manifest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--names", default=None,
                    help="comma-separated subset (default: every recipe)")
    ap.add_argument("--out", default=os.path.join(_ROOT, "Intermediate",
                                                  "Characters"))
    args = ap.parse_args()
    names = ([n.strip() for n in args.names.split(",")] if args.names
             else [n for n in cb.CHARACTER_ORDER if n in cb.BODY_BUILDERS])
    os.makedirs(args.out, exist_ok=True)

    failed = False
    for name in names:
        try:
            m = generate_one(name, args.out)
            print("[generate-characters] %-10s OK  tris=%d verts=%d "
                  "height=%.2f accent=%.2f%% (cap %.1f%%) sha1=%s"
                  % (name, m["tris"], m["verts"], m["height_uu"],
                     m["accent_area_pct"], m["accent_cap_pct"],
                     m["sha1"][:12]))
        except (GlbCheckError, KeyError, ValueError) as e:
            failed = True
            print("[generate-characters] %-10s FAIL %s" % (name, e))
    print("[generate-characters] EXIT=%d" % (1 if failed else 0))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
