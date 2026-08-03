#include "Gameplay/TraceHitZones.h"

#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"

#include "Trace.h"
#include "TraceTypes.h"   // FTraceLagCompFrame

// -------------------------------------------------------------------------------------------
// Settings
// -------------------------------------------------------------------------------------------

UTraceDamageSettings::UTraceDamageSettings()
{
	// Values live on the property declarations in the header, next to the reasoning for each.
	// Nothing to do here beyond existing, but the constructor is where a future derived default
	// would go, so keep it explicit rather than relying on the compiler's.
}

const UTraceDamageSettings& UTraceDamageSettings::Get()
{
	const UTraceDamageSettings* Settings = GetDefault<UTraceDamageSettings>();
	check(Settings != nullptr);
	return *Settings;
}

FName UTraceDamageSettings::GetCategoryName() const
{
	// Sits next to "Trace Gameplay" in Project Settings rather than in the default bucket.
	return FName(TEXT("Game"));
}

const TCHAR* TraceHitZoneToString(ETraceHitZone Zone)
{
	switch (Zone)
	{
	case ETraceHitZone::Head: return TEXT("HEAD");
	case ETraceHitZone::Body: return TEXT("BODY");
	case ETraceHitZone::Legs: return TEXT("LEGS");
	default:                  return TEXT("-");
	}
}

// -------------------------------------------------------------------------------------------
// The model
// -------------------------------------------------------------------------------------------

FTraceHitZoneModel FTraceHitZoneModel::FromCapsule(const FVector& InCenter, double InHalfHeight, double InRadius,
	double InPosture)
{
	const UTraceDamageSettings& Settings = UTraceDamageSettings::Get();

	FTraceHitZoneModel Model;
	Model.CapsuleCenter = InCenter;

	// Floors, not asserts: this is fed by a replicated/interpolated pose and by client-supplied
	// timing. A degenerate capsule must degrade to a small valid body, never to a divide by zero
	// or to a NaN that would poison the comparison below.
	Model.HalfHeight = FMath::Max(1.0, InHalfHeight);
	Model.Radius     = FMath::Max(1.0, InRadius);

	const double FullHeight = Model.HalfHeight * 2.0;
	const double FootZ      = Model.CapsuleCenter.Z - Model.HalfHeight;

	// POSTURE. The bands are laid out over the height the body actually occupies, not over the
	// collider. In this build a slide keeps the capsule at full size on purpose (the capsule is the
	// one source of truth for detection, rewind and the trail trip test) while the view and the mesh
	// drop; without this term a slider's head sphere would sit ~48uu above the head the shooter can
	// see. Detection below is still the FULL capsule, so exactly the same shots connect either way.
	Model.PostureScale = FMath::Clamp(InPosture, 0.5, 1.0);
	const double BodyHeight = FullHeight * Model.PostureScale;

	const double HipFrac  = FMath::Clamp(static_cast<double>(Settings.HipHeightFraction), 0.05, 0.90);
	const double HeadFrac = FMath::Clamp(static_cast<double>(Settings.HeadCenterHeightFraction), 0.50, 1.00);
	const double HeadRFrac = FMath::Clamp(static_cast<double>(Settings.HeadRadiusFraction), 0.01, 0.30);

	Model.HipZ = FootZ + BodyHeight * HipFrac;
	Model.HeadCenter = FVector(Model.CapsuleCenter.X, Model.CapsuleCenter.Y, FootZ + BodyHeight * HeadFrac);

	// The head keeps its STANDING size when the body compresses. A slide lowers where the head is,
	// it does not make the head smaller - scaling the radius here would quietly hand sliding players
	// a reduced headshot profile on top of an already harder-to-track target.
	Model.HeadRadius = FullHeight * HeadRFrac;

	// The head sphere must not poke out of the capsule: a sphere sticking out of the top would be
	// hittable by a ray that never touches the capsule, and since hit *detection* is the capsule
	// test, those shots would be classified Head but never resolve — an invisible dead zone.
	// Clamping keeps the sphere strictly inside the volume the capsule test can reach.
	Model.HeadRadius = FMath::Min(Model.HeadRadius, Model.Radius * 0.9);
	const double MaxHeadCenterZ = Model.CapsuleCenter.Z + Model.HalfHeight - Model.HeadRadius;
	Model.HeadCenter.Z = FMath::Min(Model.HeadCenter.Z, MaxHeadCenterZ);

	return Model;
}

FTraceHitZoneModel FTraceHitZoneModel::FromFrame(const FTraceLagCompFrame& Frame)
{
	return FromCapsule(Frame.CapsuleCenter, Frame.CapsuleHalfHeight, Frame.CapsuleRadius, Frame.PostureScale);
}

bool FTraceHitZoneModel::ResolveSegment(const FVector& Start, const FVector& End,
	double& OutEnterDist, FVector& OutImpactPoint, ETraceHitZone& OutZone) const
{
	OutEnterDist = 0.0;
	OutImpactPoint = Start;
	OutZone = ETraceHitZone::None;

	const FVector Delta = End - Start;
	const double SegmentLength = Delta.Size();
	if (SegmentLength < 1.e-4 || Start.ContainsNaN() || End.ContainsNaN())
	{
		return false;
	}
	const FVector Dir = Delta / SegmentLength;

	// ---- 1. Hit detection: segment vs the WHOLE capsule ------------------------------------
	// Byte-for-byte the test that shipped before zones existed, deliberately. Adding positional
	// damage must not change *which* shots connect, only what they are worth.
	const double SegHalf = FMath::Max(0.0, HalfHeight - Radius);
	const FVector CapsuleBottom = CapsuleCenter - FVector(0.0, 0.0, SegHalf);
	const FVector CapsuleTop    = CapsuleCenter + FVector(0.0, 0.0, SegHalf);

	FVector ClosestOnRay = FVector::ZeroVector;
	FVector ClosestOnCapsule = FVector::ZeroVector;
	FMath::SegmentDistToSegmentSafe(Start, End, CapsuleBottom, CapsuleTop, ClosestOnRay, ClosestOnCapsule);

	const double SeparationSq = FVector::DistSquared(ClosestOnRay, ClosestOnCapsule);
	if (SeparationSq > Radius * Radius)
	{
		return false;
	}

	// Step back from closest approach to where the ray crosses the capsule surface, so the impact
	// marker sits on the near side of the body and overlapping targets order correctly.
	const double BackOff = FMath::Sqrt(FMath::Max(0.0, Radius * Radius - SeparationSq));
	const double AlongClosest = FVector::DotProduct(ClosestOnRay - Start, Dir);
	OutEnterDist = FMath::Clamp(AlongClosest - BackOff, 0.0, SegmentLength);
	OutImpactPoint = Start + Dir * OutEnterDist;

	// ---- 2. Classification: head sphere first ----------------------------------------------
	// Asked as a true ray/sphere intersection rather than "is the capsule entry point near the
	// head", because the two differ exactly where it matters: a shot angled down through the
	// shoulder into the skull enters the capsule at chest height but is a head shot, and a shot
	// that grazes past the ear enters the capsule at head height but is not.
	{
		const FVector M = Start - HeadCenter;
		const double B = FVector::DotProduct(M, Dir);
		const double C = M.SizeSquared() - HeadRadius * HeadRadius;

		// C <= 0 means the muzzle is already inside the head sphere (point blank). Treat as a hit.
		if (C <= 0.0)
		{
			OutZone = ETraceHitZone::Head;
			return true;
		}

		const double Disc = B * B - C;
		if (Disc >= 0.0)
		{
			const double Root = FMath::Sqrt(Disc);
			double T = -B - Root;
			if (T < 0.0)
			{
				T = -B + Root;
			}
			if (T >= 0.0 && T <= SegmentLength)
			{
				OutZone = ETraceHitZone::Head;
				return true;
			}
		}
	}

	// ---- 3. Everything else: a height band on the capsule entry point -----------------------
	OutZone = (OutImpactPoint.Z < HipZ) ? ETraceHitZone::Legs : ETraceHitZone::Body;
	return true;
}

float FTraceHitZoneModel::DamageForZone(ETraceHitZone Zone)
{
	const UTraceDamageSettings& Settings = UTraceDamageSettings::Get();
	switch (Zone)
	{
	case ETraceHitZone::Head: return FMath::Max(0.f, Settings.HeadDamage);
	case ETraceHitZone::Body: return FMath::Max(0.f, Settings.BodyDamage);
	case ETraceHitZone::Legs: return FMath::Max(0.f, Settings.LegDamage);
	default:                  return 0.f;
	}
}

// -------------------------------------------------------------------------------------------
// Self test
//
// The client's predicted trace and the server's authoritative trace call the SAME function, so
// "do the two paths agree" is not a runtime question about two implementations — it is a question
// about ONE implementation being correct and being fed the same pose. This proves the first half
// offline and cheaply; UTraceWeaponComponent's Trace.DebugHitZones instrumentation proves the
// second half live.
// -------------------------------------------------------------------------------------------

namespace
{
	struct FZoneCase
	{
		const TCHAR* Name;
		FVector Start;
		FVector End;
		bool bExpectHit;
		ETraceHitZone ExpectedZone;
	};

	int32 RunCases(const FTraceHitZoneModel& Model, const FZoneCase* Cases, int32 Count)
	{
		int32 Failures = 0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FZoneCase& Case = Cases[Index];

			double Dist = 0.0;
			FVector Impact = FVector::ZeroVector;
			ETraceHitZone Zone = ETraceHitZone::None;
			const bool bHit = Model.ResolveSegment(Case.Start, Case.End, Dist, Impact, Zone);

			const bool bPass = (bHit == Case.bExpectHit) && (!bHit || Zone == Case.ExpectedZone);
			if (!bPass)
			{
				++Failures;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("HITZONE %s  %-28s hit=%d zone=%-4s (expected hit=%d zone=%s) impactZ=%.1f"),
				bPass ? TEXT("PASS") : TEXT("FAIL"),
				Case.Name,
				bHit ? 1 : 0, TraceHitZoneToString(Zone),
				Case.bExpectHit ? 1 : 0, TraceHitZoneToString(Case.ExpectedZone),
				Impact.Z);
		}
		return Failures;
	}
} // namespace

int32 TraceRunHitZoneSelfTest()
{
	const UTraceDamageSettings& Settings = UTraceDamageSettings::Get();

	// Stock standing pawn: feet at Z=0, crown at Z=176.
	const FTraceHitZoneModel Standing = FTraceHitZoneModel::FromCapsule(FVector(0, 0, 88), 88.0, 34.0);

	UE_LOG(LogTraceGame, Display,
		TEXT("HITZONE model (standing): hipZ=%.1f headCentreZ=%.1f headRadius=%.1f | damage head=%.0f body=%.0f legs=%.0f"),
		Standing.HipZ, Standing.HeadCenter.Z, Standing.HeadRadius,
		Settings.HeadDamage, Settings.BodyDamage, Settings.LegDamage);

	// All shots travel along -X from 500 uu away unless noted, i.e. straight at the body.
	const FZoneCase StandingCases[] =
	{
		{ TEXT("head, dead centre"),      FVector(500, 0, 159), FVector(-500, 0, 159), true,  ETraceHitZone::Head },
		{ TEXT("head, top of skull"),     FVector(500, 0, 171), FVector(-500, 0, 171), true,  ETraceHitZone::Head },
		{ TEXT("above skull, in capsule"),FVector(500, 0, 175), FVector(-500, 0, 175), true,  ETraceHitZone::Body },
		{ TEXT("beside head (y=+26)"),    FVector(500, 26, 159),FVector(-500, 26, 159),true,  ETraceHitZone::Body },
		{ TEXT("chest"),                  FVector(500, 0, 120), FVector(-500, 0, 120), true,  ETraceHitZone::Body },
		{ TEXT("just above hip"),         FVector(500, 0, 85),  FVector(-500, 0, 85),  true,  ETraceHitZone::Body },
		{ TEXT("just below hip"),         FVector(500, 0, 78),  FVector(-500, 0, 78),  true,  ETraceHitZone::Legs },
		{ TEXT("shin"),                   FVector(500, 0, 30),  FVector(-500, 0, 30),  true,  ETraceHitZone::Legs },
		{ TEXT("from below, into feet"),  FVector(0, 0, -300),  FVector(0, 0, 60),     true,  ETraceHitZone::Legs },
		{ TEXT("over the head, miss"),    FVector(500, 0, 200), FVector(-500, 0, 200), false, ETraceHitZone::None },
		{ TEXT("wide of the body, miss"), FVector(500, 90, 120),FVector(-500, 90, 120),false, ETraceHitZone::None },
		{ TEXT("under the feet, miss"),   FVector(500, 0, -20), FVector(-500, 0, -20), false, ETraceHitZone::None },
		{ TEXT("point blank at skull"),   FVector(20, 0, 159),  FVector(-500, 0, 159), true,  ETraceHitZone::Head },
	};

	int32 Failures = RunCases(Standing, StandingCases, UE_ARRAY_COUNT(StandingCases));

	// Crouched/sliding pawn: the capsule halves, so every zone must halve with it. This is the
	// case the whole proportional model exists to get right.
	const FTraceHitZoneModel Crouched = FTraceHitZoneModel::FromCapsule(FVector(0, 0, 44), 44.0, 34.0);
	UE_LOG(LogTraceGame, Display,
		TEXT("HITZONE model (crouched): hipZ=%.1f headCentreZ=%.1f headRadius=%.1f"),
		Crouched.HipZ, Crouched.HeadCenter.Z, Crouched.HeadRadius);

	const FZoneCase CrouchedCases[] =
	{
		{ TEXT("crouched head"),          FVector(500, 0, 76),  FVector(-500, 0, 76),  true,  ETraceHitZone::Head },
		{ TEXT("crouched chest"),         FVector(500, 0, 55),  FVector(-500, 0, 55),  true,  ETraceHitZone::Body },
		{ TEXT("crouched shin"),          FVector(500, 0, 15),  FVector(-500, 0, 15),  true,  ETraceHitZone::Legs },
		{ TEXT("standing head height"),   FVector(500, 0, 159), FVector(-500, 0, 159), false, ETraceHitZone::None },
	};

	Failures += RunCases(Crouched, CrouchedCases, UE_ARRAY_COUNT(CrouchedCases));

	// SLIDING pawn, which is the case this game actually produces: the capsule does NOT shrink (see
	// UTraceCharacterMovementComponent::CanCrouchInCurrentState) - only the posture term does. So
	// the collider is still the full standing capsule and the BANDS have moved down inside it.
	// Posture 0.776 is (88 + SlideEyeHeight 30) / (88 + EyeHeight 64), i.e. what
	// ATraceCharacter::GetHitZonePostureScale reports at the bottom of a slide.
	const FTraceHitZoneModel Sliding = FTraceHitZoneModel::FromCapsule(FVector(0, 0, 88), 88.0, 34.0, 0.776);
	UE_LOG(LogTraceGame, Display,
		TEXT("HITZONE model (sliding, posture %.3f): hipZ=%.1f headCentreZ=%.1f headRadius=%.1f  [slide eye is at Z=118]"),
		Sliding.PostureScale, Sliding.HipZ, Sliding.HeadCenter.Z, Sliding.HeadRadius);

	const FZoneCase SlidingCases[] =
	{
		// The head is where the player can SEE it - around the dropped eye line, not 48uu above it.
		{ TEXT("slide head (at eye line)"),  FVector(500, 0, 124), FVector(-500, 0, 124), true, ETraceHitZone::Head },
		{ TEXT("slide head, low edge"),      FVector(500, 0, 112), FVector(-500, 0, 112), true, ETraceHitZone::Head },
		// THE REGRESSION THIS TERM EXISTS FOR. Standing head height is still inside the un-shrunk
		// capsule, so it must still HIT - but it is empty air above a sliding player's head and it
		// must not pay out a one-shot kill.
		{ TEXT("standing head Z on slider"), FVector(500, 0, 159), FVector(-500, 0, 159), true, ETraceHitZone::Body },
		{ TEXT("slide chest"),               FVector(500, 0, 95),  FVector(-500, 0, 95),  true, ETraceHitZone::Body },
		{ TEXT("slide just above hip"),      FVector(500, 0, 70),  FVector(-500, 0, 70),  true, ETraceHitZone::Body },
		{ TEXT("slide just below hip"),      FVector(500, 0, 55),  FVector(-500, 0, 55),  true, ETraceHitZone::Legs },
		// Detection is unchanged by posture: the capsule is still full height, so this still misses
		// at exactly the same place a standing pawn does.
		{ TEXT("over the capsule, miss"),    FVector(500, 0, 200), FVector(-500, 0, 200), false, ETraceHitZone::None },
	};

	Failures += RunCases(Sliding, SlidingCases, UE_ARRAY_COUNT(SlidingCases));

	// Spec §6 arithmetic, stated as a test so a config edit that breaks the design shows up here.
	const float MaxHealthAssumed = 100.f;
	if (Settings.HeadDamage < MaxHealthAssumed)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("HITZONE FAIL  head damage %.0f does not one-shot %.0f health"),
			Settings.HeadDamage, MaxHealthAssumed);
		++Failures;
	}

	UE_LOG(LogTraceGame, Display, TEXT("HITZONE self test: %d failure(s)."), Failures);
	return Failures;
}

static FAutoConsoleCommand GTraceHitZoneTestCmd(
	TEXT("Trace.HitZoneTest"),
	TEXT("Runs the offline self test of the head/body/legs damage zone model and logs the result."),
	FConsoleCommandDelegate::CreateStatic(
		[]() { TraceRunHitZoneSelfTest(); }));
