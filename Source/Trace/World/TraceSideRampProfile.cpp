// =================================================================================================
// TraceSideRampProfile.cpp — the RUNTIME half of the side ramp's design check.
//
// The header's static_asserts fail the build if a SOURCE change moves the shipped profile out of the
// surf band. They cannot see an .ini. Both edges of the band are reachable from DefaultGame.ini —
// SurfMinNormalZ is a named knob and WalkableFloorZ is a component property — so this re-derives the
// two band tangents from the LIVE numbers and reports the comparison either way.
//
// It reports on the way through rather than only on failure, because the pass line is the evidence:
// "the shipped face spans 45.50..59.87 deg inside a live band of 44.77..63.26" is a measurement a
// report can quote, and "no error was logged" is not.
// =================================================================================================
#include "World/TraceSideRampProfile.h"

#include "Trace.h"

namespace TraceSideRampProfile
{
	namespace
	{
		/** acos in degrees, clamped — the same conversion GetSurfSlopeBandDegrees does. */
		double DegreesFromNormalZ(double NormalZ)
		{
			return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(NormalZ, -1.0, 1.0)));
		}
	}

	bool ValidateAgainstLiveBand(float LiveWalkableFloorZ, float LiveSurfMinNormalZ, FString& OutReason)
	{
		// A DEGENERATE BAND IS A FAILURE, NOT A PASS. If the two edges have crossed there is no band
		// at all, and silently returning true here would be exactly the "check that cannot fail" this
		// file exists to avoid.
		if (!(LiveSurfMinNormalZ > 0.f) || !(LiveWalkableFloorZ > LiveSurfMinNormalZ))
		{
			OutReason = FString::Printf(
				TEXT("the live band is degenerate: WalkableFloorZ %.4f is not above SurfMinNormalZ %.4f, "
				     "so nothing on any surface can be surfed."),
				LiveWalkableFloorZ, LiveSurfMinNormalZ);
			return false;
		}

		const double LiveLoDeg = DegreesFromNormalZ(LiveWalkableFloorZ);
		const double LiveHiDeg = DegreesFromNormalZ(LiveSurfMinNormalZ);
		const double LiveLoTan = FMath::Tan(FMath::DegreesToRadians(LiveLoDeg));
		const double LiveHiTan = FMath::Tan(FMath::DegreesToRadians(LiveHiDeg));

		const double ShallowDeg = FMath::RadiansToDegrees(FMath::Atan(kShallowestFacetTangent));
		const double SteepDeg = FMath::RadiansToDegrees(FMath::Atan(kSteepestFacetTangent));

		// Where the handoff falls under the LIVE band, recomputed rather than quoted: the header's
		// kFirstSurfableFacet was resolved against the literals, and the whole point of this function
		// is that the literals may no longer be the band.
		int32 LiveFirstSurfable = kFacetCount;
		for (int32 Facet = 0; Facet < kFacetCount; ++Facet)
		{
			if (FacetTangent(Facet) > LiveLoTan)
			{
				LiveFirstSurfable = Facet;
				break;
			}
		}

		const bool bStartsWalkable = kShallowestFacetTangent < LiveLoTan;
		const bool bBecomesSurfable = kSteepestFacetTangent > LiveLoTan;
		const bool bNeverAWall = kSteepestFacetTangent < LiveHiTan;
		const bool bDriftedFromLiterals =
			FMath::Abs(LiveLoTan - kBandLoTangent) > 1e-3 || FMath::Abs(LiveHiTan - kBandHiTangent) > 1e-3;

		const double LiveWalkDepth =
			kDepthUU * static_cast<double>(LiveFirstSurfable) / static_cast<double>(kFacetCount);
		const double LiveHandoffZ =
			HeightAt(static_cast<double>(LiveFirstSurfable) / static_cast<double>(kFacetCount));

		OutReason = FString::Printf(
			TEXT("live band %.4f..%.4f deg (Nz %.4f..%.4f); shipped facets %.4f..%.4f deg; handoff at "
			     "facet %d of %d = %.1f uu deep / %.1f uu up; walk-up %.1f uu deep, face %.1f uu deep and "
			     "%.1f uu of vertical; headroom below the surf ceiling %.3f deg%s"),
			LiveLoDeg, LiveHiDeg, LiveWalkableFloorZ, LiveSurfMinNormalZ,
			ShallowDeg, SteepDeg, LiveFirstSurfable, kFacetCount, LiveWalkDepth, LiveHandoffZ,
			LiveWalkDepth, kDepthUU - LiveWalkDepth, kHeightUU - LiveHandoffZ,
			LiveHiDeg - SteepDeg,
			bDriftedFromLiterals
				? TEXT("  [!! the band has MOVED from the tangents TraceSideRampProfile.h asserts "
				       "against — update kBandLoTangent / kBandHiTangent and re-check the build asserts]")
				: TEXT(""));

		if (!bStartsWalkable)
		{
			OutReason += TEXT("  FAIL: the ramp's first facet is not walkable — there is no way on to it.");
		}
		if (!bBecomesSurfable)
		{
			OutReason += TEXT("  FAIL: the ramp's steepest facet is still walkable — it is a run-up only.");
		}
		if (!bNeverAWall)
		{
			OutReason += TEXT("  FAIL: the ramp's steepest facet is past the surf ceiling — its top is a wall.");
		}

		return bStartsWalkable && bBecomesSurfable && bNeverAWall && !bDriftedFromLiterals;
	}

	FString Describe()
	{
		return FString::Printf(
			TEXT("owner's parabola z = H*(y/D)^2 (h=u^2 measured to 2.3e-5 on ramp_shell's 65 stations) "
			     "at D %.0f uu x H %.0f uu x L %.0f uu, %d facets: walk-up %.1f uu deep climbing to %.1f uu "
			     "(%.2f..%.2f deg), face %.1f uu deep with %.1f uu of vertical (%.2f..%.2f deg); crest at "
			     "out %.0f (the pawn standoff's own face), toe at out %.0f; deck %.1f uu over the fillet's "
			     "%.0f uu terrace edge; a capsule can reach Z %.1f, so %.1f uu of the face is ridable"),
			kDepthUU, kHeightUU, kLengthUU, kFacetCount,
			kWalkableDepthUU, kHandoffHeightUU,
			FMath::RadiansToDegrees(FMath::Atan(kShallowestFacetTangent)),
			FMath::RadiansToDegrees(FMath::Atan(FacetTangent(kFirstSurfableFacet - 1))),
			kSurfableDepthUU, kSurfableVerticalUU,
			FMath::RadiansToDegrees(FMath::Atan(FacetTangent(kFirstSurfableFacet))),
			FMath::RadiansToDegrees(FMath::Atan(kSteepestFacetTangent)),
			kCrestOutFromWallUU, kToeOutFromWallUU,
			kHeightOverFilletEdgeUU, kFilletOuterTerraceTopUU,
			kReachableTopUU, kReachableSurfableVerticalUU);
	}
}
