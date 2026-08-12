// GENERATED FILE — DO NOT EDIT BY HAND.
// Produced by Scripts/generate_railgun_curve.py from Art/Railgun/fire_curves.json.
//
// The emissive-intensity curve for the railgun's two glowing materials, as authored
// alongside the model. Values are MULTIPLIERS on each material's rest intensity:
// 1.0 is idle, and the discharge frame peaks at 5.20x (cyan) / 5.60x (amber).
#pragma once

#include "CoreMinimal.h"

namespace TraceRailgunFireCurve
{
	/** Clip length in seconds, as authored. */
	inline constexpr float ClipSeconds = 1.9000f;

	/** The frame the shot leaves the barrel. Everything before this is charge-up.
	  * Trace's gun has no windup, so gameplay starts playback HERE (see
	  * ATraceCharacter::NotifyWeaponFired). */
	inline constexpr float DischargeSeconds = 1.0500f;

	/** End of the flash; the tail from here to ClipSeconds is the decay. */
	inline constexpr float DecayStartSeconds = 1.1500f;

	/** Peak multipliers, at the discharge frame. The mechanical part of the fire
	  * animation is driven by (cyan - 1) / (PeakCyan - 1), so the rails are widest
	  * exactly when the flash is brightest. */
	inline constexpr float PeakCyan = 5.2000f;
	inline constexpr float PeakAmber = 5.6000f;

	inline constexpr int32 KeyCount = 115;

	/** { time, cyan multiplier, amber multiplier }, ascending in time. */
	inline constexpr float Keys[KeyCount][3] =
	{
		{ 0.0000f, 1.0000f, 1.0000f },
		{ 0.0167f, 1.0171f, 1.0306f },
		{ 0.0333f, 1.0381f, 1.0682f },
		{ 0.0500f, 1.0630f, 1.1128f },
		{ 0.0667f, 1.0915f, 1.1637f },
		{ 0.0833f, 1.1231f, 1.2202f },
		{ 0.1000f, 1.1573f, 1.2815f },
		{ 0.1167f, 1.1934f, 1.3460f },
		{ 0.1333f, 1.2302f, 1.4120f },
		{ 0.1500f, 1.2666f, 1.4771f },
		{ 0.1667f, 1.3009f, 1.5385f },
		{ 0.1833f, 1.3311f, 1.5926f },
		{ 0.2000f, 1.3551f, 1.6354f },
		{ 0.2167f, 1.3703f, 1.6626f },
		{ 0.2333f, 1.3742f, 1.6697f },
		{ 0.2500f, 1.3648f, 1.6528f },
		{ 0.2667f, 1.3404f, 1.6092f },
		{ 0.2833f, 1.3006f, 1.5379f },
		{ 0.3000f, 1.2467f, 1.4415f },
		{ 0.3167f, 1.1826f, 1.3268f },
		{ 0.3333f, 1.1150f, 1.2057f },
		{ 0.3500f, 1.0536f, 1.0959f },
		{ 0.3667f, 1.0110f, 1.0198f },
		{ 0.3833f, 1.0014f, 1.0026f },
		{ 0.4000f, 1.0378f, 1.0677f },
		{ 0.4167f, 1.1287f, 1.2303f },
		{ 0.4333f, 1.2735f, 1.4895f },
		{ 0.4500f, 1.4587f, 1.8209f },
		{ 0.4667f, 1.6551f, 2.1723f },
		{ 0.4833f, 1.8200f, 2.4673f },
		{ 0.5000f, 1.9048f, 2.6190f },
		{ 0.5167f, 1.8701f, 2.5571f },
		{ 0.5333f, 1.7052f, 2.2620f },
		{ 0.5500f, 1.4449f, 1.7962f },
		{ 0.5667f, 1.1746f, 1.3124f },
		{ 0.5833f, 1.0113f, 1.0203f },
		{ 0.6000f, 1.0578f, 1.1034f },
		{ 0.6167f, 1.3401f, 1.6086f },
		{ 0.6333f, 1.7612f, 2.3621f },
		{ 0.6500f, 2.1130f, 2.9916f },
		{ 0.6667f, 2.1731f, 3.0992f },
		{ 0.6833f, 1.8597f, 2.5384f },
		{ 0.7000f, 1.3466f, 1.6202f },
		{ 0.7167f, 1.0122f, 1.0218f },
		{ 0.7333f, 1.1780f, 1.3185f },
		{ 0.7500f, 1.7871f, 2.4085f },
		{ 0.7667f, 2.3352f, 3.3893f },
		{ 0.7833f, 2.2645f, 3.2628f },
		{ 0.8000f, 1.5811f, 2.0399f },
		{ 0.8167f, 1.0197f, 1.0353f },
		{ 0.8333f, 1.3162f, 1.5659f },
		{ 0.8500f, 2.2073f, 3.1603f },
		{ 0.8667f, 2.5321f, 3.7417f },
		{ 0.8833f, 1.7609f, 2.3616f },
		{ 0.9000f, 1.0109f, 1.0194f },
		{ 0.9167f, 1.5616f, 2.0050f },
		{ 0.9333f, 2.6060f, 3.8739f },
		{ 0.9500f, 2.2843f, 3.2982f },
		{ 0.9667f, 1.1237f, 1.2213f },
		{ 0.9833f, 1.4555f, 1.8151f },
		{ 1.0000f, 2.7268f, 4.0901f },
		{ 1.0167f, 2.1922f, 3.1334f },
		{ 1.0333f, 1.0065f, 1.0116f },
		{ 1.0500f, 5.2000f, 5.6000f },
		{ 1.0667f, 5.2000f, 5.6000f },
		{ 1.0833f, 5.2000f, 5.6000f },
		{ 1.1000f, 5.2000f, 5.6000f },
		{ 1.1167f, 5.2000f, 5.6000f },
		{ 1.1333f, 5.2000f, 5.6000f },
		{ 1.1500f, 5.2000f, 5.6000f },
		{ 1.1667f, 4.9974f, 5.3781f },
		{ 1.1833f, 4.8003f, 5.1622f },
		{ 1.2000f, 4.6085f, 4.9522f },
		{ 1.2167f, 4.4222f, 4.7481f },
		{ 1.2333f, 4.2413f, 4.5500f },
		{ 1.2500f, 4.0657f, 4.3576f },
		{ 1.2667f, 3.8954f, 4.1711f },
		{ 1.2833f, 3.7304f, 3.9904f },
		{ 1.3000f, 3.5707f, 3.8155f },
		{ 1.3167f, 3.4162f, 3.6463f },
		{ 1.3333f, 3.2669f, 3.4828f },
		{ 1.3500f, 3.1228f, 3.3250f },
		{ 1.3667f, 2.9839f, 3.1728f },
		{ 1.3833f, 2.8500f, 3.0262f },
		{ 1.4000f, 2.7213f, 2.8852f },
		{ 1.4167f, 2.5976f, 2.7497f },
		{ 1.4333f, 2.4789f, 2.6197f },
		{ 1.4500f, 2.3652f, 2.4952f },
		{ 1.4667f, 2.2564f, 2.3760f },
		{ 1.4833f, 2.1525f, 2.2623f },
		{ 1.5000f, 2.0535f, 2.1539f },
		{ 1.5167f, 1.9594f, 2.0507f },
		{ 1.5333f, 1.8700f, 1.9528f },
		{ 1.5500f, 1.7854f, 1.8601f },
		{ 1.5667f, 1.7054f, 1.7726f },
		{ 1.5833f, 1.6301f, 1.6902f },
		{ 1.6000f, 1.5595f, 1.6128f },
		{ 1.6167f, 1.4934f, 1.5404f },
		{ 1.6333f, 1.4318f, 1.4729f },
		{ 1.6500f, 1.3746f, 1.4103f },
		{ 1.6667f, 1.3219f, 1.3525f },
		{ 1.6833f, 1.2734f, 1.2995f },
		{ 1.7000f, 1.2293f, 1.2511f },
		{ 1.7167f, 1.1893f, 1.2074f },
		{ 1.7333f, 1.1535f, 1.1681f },
		{ 1.7500f, 1.1218f, 1.1334f },
		{ 1.7667f, 1.0940f, 1.1029f },
		{ 1.7833f, 1.0700f, 1.0767f },
		{ 1.8000f, 1.0499f, 1.0547f },
		{ 1.8167f, 1.0334f, 1.0366f },
		{ 1.8333f, 1.0205f, 1.0224f },
		{ 1.8500f, 1.0109f, 1.0119f },
		{ 1.8667f, 1.0045f, 1.0049f },
		{ 1.8833f, 1.0010f, 1.0011f },
		{ 1.9000f, 1.0000f, 1.0000f },
	};

	/** Linear sample of the table. Times outside the clip clamp to the end values. */
	inline void Sample(float Time, float& OutCyan, float& OutAmber)
	{
		if (Time <= Keys[0][0])
		{
			OutCyan = Keys[0][1];
			OutAmber = Keys[0][2];
			return;
		}
		if (Time >= Keys[KeyCount - 1][0])
		{
			OutCyan = Keys[KeyCount - 1][1];
			OutAmber = Keys[KeyCount - 1][2];
			return;
		}

		// The table is a fixed 60 Hz sampling, so the bracketing pair is an index
		// rather than a search.
		const float Span = Keys[KeyCount - 1][0] - Keys[0][0];
		const float Normalised = (Time - Keys[0][0]) / FMath::Max(Span, KINDA_SMALL_NUMBER);
		const int32 Lo = FMath::Clamp(
			FMath::FloorToInt32(Normalised * float(KeyCount - 1)), 0, KeyCount - 2);
		const int32 Hi = Lo + 1;

		const float T0 = Keys[Lo][0];
		const float T1 = Keys[Hi][0];
		const float Alpha = FMath::Clamp((Time - T0) / FMath::Max(T1 - T0, KINDA_SMALL_NUMBER), 0.f, 1.f);

		OutCyan = FMath::Lerp(Keys[Lo][1], Keys[Hi][1], Alpha);
		OutAmber = FMath::Lerp(Keys[Lo][2], Keys[Hi][2], Alpha);
	}
}
