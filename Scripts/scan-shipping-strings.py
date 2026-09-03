#!/usr/bin/env python3
"""
Scan a packaged/linked Shipping artefact for strings that should not be in it.

WHY THIS EXISTS, AND WHY IT IS NOT A ONE-LINE `strings | grep`
=============================================================
Three verification passes on this project "proved" the Shipping binary was free of
dev-only switches with a scan that silently searched nothing, and the passes read
the empty output as a pass. Two separate traps stacked up:

  1. Apple's strings(1) REJECTS `-el`. The passes used it for the UTF-16 half of
     their scan, so that half exited without searching and printed nothing. An
     empty result from a command that never ran looks exactly like an empty result
     from a command that found nothing.

  2. On macOS, Unreal's TEXT("...") literals are UTF-16LE in the artefact, NOT
     ASCII. So the ASCII half of the scan could not have found a TEXT() string
     either, even when one was there. Measured on Trace-Mac-Shipping:
         "Trace.Arena.SurfRails"  ascii=0  utf16le=1  utf32le=0
     Every hit that matters lives in the utf16le column.

  A third trap is about MEANING rather than encoding, and this tool cannot solve
  it for you: a switch name can appear in the artefact because a CVar's HELP TEXT
  mentions it, while the switch itself is properly compiled out. That is exactly
  what `-TraceNoSurfRails` looked like after it was correctly guarded. STRING
  PRESENCE IS A PROXY FOR REACHABILITY, NOT A MEASUREMENT OF IT. When this script
  reports a hit, read the surrounding bytes (it prints them) and decide whether
  you are looking at a live switch or a sentence about one.

USAGE
    Scripts/scan-shipping-strings.py [--binary PATH] [--quiet]
    Scripts/scan-shipping-strings.py --check "SomeSwitch" "Another.CVar"

Exit status is 1 if any needle in the DEFAULT_FORBIDDEN list is found in a form
that is not obviously help text, so this can be wired into a gate. A needle given
with --check is always reported, never fatal.
"""

import argparse
import os
import sys

DEFAULT_BINARY = "Binaries/Mac/Trace-Mac-Shipping"

# Switches and commands that must not be REACHABLE in a shipped build. Presence in
# help text is tolerated (and reported); presence as a live literal is not.
DEFAULT_FORBIDDEN = [
    "TraceNoSurfRails",
    "TraceSurfRailLegacyRun",
    "TraceSurfRailNoJunctionLap",
    "TraceSurfRailNoCrestSink",
    "TraceSurfLegacyAirLimit",
    "TraceLegacyTuning",
    "TraceLegacyWallJump",
    "TraceV24LegacySlide",
    "TraceV26LegacySlideJump",
    "TraceLegacyAirReverse",
    "TraceLegacyLanding",
    "TraceLegacyGroundedTurnover",
    "TraceLegacyKnife",
    "Trace.Verif.GrantCore",
    "Trace.Portrait.CaptureAll",
    "Trace.Fx.BurstTest",
    "Trace.Fx.LegacyAccents",
    "Trace.Arena.SurfProfile",
    # The side-ramp instruments. Both are guarded — the command lives inside this file's
    # #if !UE_BUILD_SHIPPING block and the harness arm returns false outright in Shipping — and both
    # are listed here so the gate would SEE it if either guard were ever removed. A previous pass
    # added Trace.Arena.SurfBankProfile beside SurfProfile and did not list it, which left exactly
    # this hole for that command; the same mistake is not worth making twice.
    "Trace.Arena.SideRamp",
    "TraceSurfSideRampTest",
    # The concave side ramp's walk-up rig and the approach rig's speed knob. Same guards, same
    # reason: TraceSurfWalkUpTest's arm returns false outright under UE_BUILD_SHIPPING and
    # TraceSurfApproachSpeed= is parsed inside the same block, so neither can be reached in a
    # shipped build - and both are listed so the gate would SEE it if a guard were ever removed.
    "TraceSurfWalkUpTest",
    "TraceSurfApproachSpeed",
]

# A string we KNOW ships, used as a positive control. Without one of these, "zero
# hits everywhere" is indistinguishable from "the scan is broken" — which is the
# precise failure this script was written to stop.
POSITIVE_CONTROLS = ["Trace.Arena.WallCove", "PRACTICE", "Trace.Arena.SurfRails"]

# ADJUDICATED FALSE POSITIVES. Each is a string that IS in the artefact for a
# reason that is not a reachable cheat, checked by hand once so nobody has to
# re-derive it. Keep the reason with the entry: an unexplained allowlist is how a
# real leak eventually gets waved through.
KNOWN_BENIGN = {
    "Trace.Verif.GrantCore":
        "An Exec() ARGUMENT in shipped code (TraceHUD.cpp:2112, TraceAudioInteg.cpp:696), not a "
        "registration. The command itself is compiled out, so those Exec calls are inert — which "
        "TraceHUD.cpp:2039 already documents. Verified by absence: a registered command carries its "
        "own help text, and no help string for this one is in the artefact. (The phrase 'Grant the "
        "Core' IS present, but it belongs to Trace.HUD.ThrowRings.Demo's help.)",
}

ENCODINGS = [("ascii", "ascii"), ("utf16le", "utf-16-le"), ("utf32le", "utf-32-le")]


def find_all(blob, needle, codec):
    try:
        pat = needle.encode(codec)
    except UnicodeEncodeError:
        return []
    out, start = [], 0
    while True:
        i = blob.find(pat, start)
        if i < 0:
            return out
        out.append(i)
        start = i + 1


def context(blob, offset, codec, span=260):
    lo = max(0, offset - span)
    try:
        return blob[lo:offset + span].decode(codec, errors="replace").replace("\x00", " ")
    except Exception:
        return "<undecodable>"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=DEFAULT_BINARY)
    ap.add_argument("--check", nargs="*", default=[],
                    help="extra needles to report on (never fatal)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(args.binary):
        print(f"[scan-shipping] no artefact at {args.binary}", file=sys.stderr)
        print("[scan-shipping] build it first: Scripts/build.sh Trace -c Shipping", file=sys.stderr)
        return 2

    blob = open(args.binary, "rb").read()
    print(f"[scan-shipping] {args.binary} ({len(blob):,} bytes)")

    # ---- positive control, first, always -------------------------------------
    control_ok = False
    for ctrl in POSITIVE_CONTROLS:
        for label, codec in ENCODINGS:
            if find_all(blob, ctrl, codec):
                print(f"[scan-shipping] positive control OK: '{ctrl}' found as {label}")
                control_ok = True
                break
        if control_ok:
            break
    if not control_ok:
        print("[scan-shipping] *** THE POSITIVE CONTROL FAILED. This scan is not searching "
              "anything, and a clean result from it means NOTHING. Do not report a pass. ***",
              file=sys.stderr)
        return 2

    # ---- the actual scan ------------------------------------------------------
    live_hits = 0
    for needle in DEFAULT_FORBIDDEN + list(args.check):
        fatal = needle in DEFAULT_FORBIDDEN
        per_enc = {label: find_all(blob, needle, codec) for label, codec in ENCODINGS}
        total = sum(len(v) for v in per_enc.values())
        if total == 0:
            if not args.quiet:
                print(f"  clean   {needle}")
            continue

        where = ", ".join(f"{lab}={len(v)}" for lab, v in per_enc.items() if v)
        if needle in KNOWN_BENIGN:
            print(f"  known   {needle}  ({where}) — adjudicated benign:")
            print(f"          {KNOWN_BENIGN[needle]}")
            continue
        print(f"  PRESENT {needle}  ({where})")
        for label, codec in ENCODINGS:
            for off in per_enc[label][:2]:
                snippet = context(blob, off, codec)
                print(f"          ...{snippet.strip()[:300]}...")
                # A hit whose neighbourhood reads like prose is almost certainly a
                # help string. Say so, but do not decide it for the reader.
                looks_like_help = any(w in snippet for w in
                                      (" = ", "default", "Set with", "use -", "arena is built"))
                if looks_like_help:
                    print("          ^ reads like CVar help text — verify by hand whether the "
                          "switch itself is compiled out")
                elif fatal:
                    live_hits += 1

    if live_hits:
        print(f"[scan-shipping] *** {live_hits} forbidden string(s) present in a form that does "
              f"not look like help text ***", file=sys.stderr)
        return 1

    print("[scan-shipping] no forbidden switch appears as a live literal.")
    print("[scan-shipping] NOTE: this is a string proxy, not a reachability proof. A guarded "
          "switch can still be named in help text, and an unguarded one could be built from "
          "fragments this scan cannot see.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
