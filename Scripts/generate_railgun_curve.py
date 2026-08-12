#!/usr/bin/env python3
# =============================================================================
# Trace — generate_railgun_curve.py
#
# Bakes Art/Railgun/fire_curves.json into a C++ header.
#
# WHY A HEADER AND NOT A CURVE ASSET
#   The source notes suggest driving the glow from a UCurveFloat. That would
#   work, but the glow is a purely cosmetic, client-side effect that has to be
#   correct on the very first shot of a match: a missing or unloaded curve asset
#   would silently flatten it, and that is exactly the class of failure this
#   project has been bitten by before. A baked table cannot fail to load, costs
#   ~2 KB, and stays diffable in review.
#
#   Re-run after editing fire_curves.json:  python3 Scripts/generate_railgun_curve.py
# =============================================================================
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "Art", "Railgun", "fire_curves.json")
DST = os.path.join(ROOT, "Source", "Trace", "Gameplay", "TraceRailgunFireCurve.h")

data = json.load(open(SRC))
keys = data["keys"]
charge = data["phases"]["charge"]
discharge = data["phases"]["discharge"]

lines = []
lines.append("// GENERATED FILE — DO NOT EDIT BY HAND.")
lines.append("// Produced by Scripts/generate_railgun_curve.py from Art/Railgun/fire_curves.json.")
lines.append("//")
lines.append("// The emissive-intensity curve for the railgun's two glowing materials, as authored")
lines.append("// alongside the model. Values are MULTIPLIERS on each material's rest intensity:")
lines.append("// 1.0 is idle, and the discharge frame peaks at %.2fx (cyan) / %.2fx (amber)."
             % (max(k["cyan"] for k in keys), max(k["amber"] for k in keys)))
lines.append("#pragma once")
lines.append("")
lines.append("#include \"CoreMinimal.h\"")
lines.append("")
lines.append("namespace TraceRailgunFireCurve")
lines.append("{")
lines.append("\t/** Clip length in seconds, as authored. */")
lines.append("\tinline constexpr float ClipSeconds = %.4ff;" % data["duration"])
lines.append("")
lines.append("\t/** The frame the shot leaves the barrel. Everything before this is charge-up.")
lines.append("\t  * Trace's gun has no windup, so gameplay starts playback HERE (see")
lines.append("\t  * ATraceCharacter::NotifyWeaponFired). */")
lines.append("\tinline constexpr float DischargeSeconds = %.4ff;" % charge[1])
lines.append("")
lines.append("\t/** End of the flash; the tail from here to ClipSeconds is the decay. */")
lines.append("\tinline constexpr float DecayStartSeconds = %.4ff;" % discharge[1])
lines.append("")
lines.append("\t/** Peak multipliers, at the discharge frame. The mechanical part of the fire")
lines.append("\t  * animation is driven by (cyan - 1) / (PeakCyan - 1), so the rails are widest")
lines.append("\t  * exactly when the flash is brightest. */")
lines.append("\tinline constexpr float PeakCyan = %.4ff;" % max(k["cyan"] for k in keys))
lines.append("\tinline constexpr float PeakAmber = %.4ff;" % max(k["amber"] for k in keys))
lines.append("")
lines.append("\tinline constexpr int32 KeyCount = %d;" % len(keys))
lines.append("")
lines.append("\t/** { time, cyan multiplier, amber multiplier }, ascending in time. */")
lines.append("\tinline constexpr float Keys[KeyCount][3] =")
lines.append("\t{")
for k in keys:
    lines.append("\t\t{ %.4ff, %.4ff, %.4ff }," % (k["time"], k["cyan"], k["amber"]))
lines.append("\t};")
lines.append("")
lines.append("\t/** Linear sample of the table. Times outside the clip clamp to the end values. */")
lines.append("\tinline void Sample(float Time, float& OutCyan, float& OutAmber)")
lines.append("\t{")
lines.append("\t\tif (Time <= Keys[0][0])")
lines.append("\t\t{")
lines.append("\t\t\tOutCyan = Keys[0][1];")
lines.append("\t\t\tOutAmber = Keys[0][2];")
lines.append("\t\t\treturn;")
lines.append("\t\t}")
lines.append("\t\tif (Time >= Keys[KeyCount - 1][0])")
lines.append("\t\t{")
lines.append("\t\t\tOutCyan = Keys[KeyCount - 1][1];")
lines.append("\t\t\tOutAmber = Keys[KeyCount - 1][2];")
lines.append("\t\t\treturn;")
lines.append("\t\t}")
lines.append("")
lines.append("\t\t// The table is a fixed 60 Hz sampling, so the bracketing pair is an index")
lines.append("\t\t// rather than a search.")
lines.append("\t\tconst float Span = Keys[KeyCount - 1][0] - Keys[0][0];")
lines.append("\t\tconst float Normalised = (Time - Keys[0][0]) / FMath::Max(Span, KINDA_SMALL_NUMBER);")
lines.append("\t\tconst int32 Lo = FMath::Clamp(")
lines.append("\t\t\tFMath::FloorToInt32(Normalised * float(KeyCount - 1)), 0, KeyCount - 2);")
lines.append("\t\tconst int32 Hi = Lo + 1;")
lines.append("")
lines.append("\t\tconst float T0 = Keys[Lo][0];")
lines.append("\t\tconst float T1 = Keys[Hi][0];")
lines.append("\t\tconst float Alpha = FMath::Clamp((Time - T0) / FMath::Max(T1 - T0, KINDA_SMALL_NUMBER), 0.f, 1.f);")
lines.append("")
lines.append("\t\tOutCyan = FMath::Lerp(Keys[Lo][1], Keys[Hi][1], Alpha);")
lines.append("\t\tOutAmber = FMath::Lerp(Keys[Lo][2], Keys[Hi][2], Alpha);")
lines.append("\t}")
lines.append("}")
lines.append("")

open(DST, "w").write("\n".join(lines))
print("wrote {0} ({1} keys, clip {2}s, discharge at {3}s)".format(
    DST, len(keys), data["duration"], charge[1]))
