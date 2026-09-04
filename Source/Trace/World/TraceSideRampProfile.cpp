// =================================================================================================
// TraceSideRampProfile.cpp — the RUNTIME half of the side ramp's design check.
//
// The header's static_asserts fail the build if a SOURCE change moves the shipped profile out of the
// surf band. They cannot see an .ini. Both edges of the band are reachable from DefaultGame.ini —
// SurfMinNormalZ is a named knob and WalkableFloorZ is a component property — so this re-derives the
// two band tangents from the LIVE numbers and reports the comparison either way.
//
// It reports on the way through rather than only on failure, because the pass line is the evidence:
// "the shipped face spans 46.90..61.16 deg inside a live band of 44.77..63.26" is a measurement a
// report can quote, and "no error was logged" is not.
//
// WHAT THIS FILE HAD TO STOP CHECKING. Before the buttress rewrite the shipped ramp started at u = 0
// and its lower half was walkable, so this function's headline test was "does it START walkable" —
// the walk-up WAS the entrance and a ramp without one was unreachable. The owner has since asked for
// a face that is not walkable anywhere, and the entrance is now the ground-entry rule in
// UTraceCharacterMovementComponent::HandleImpact rather than a strip of geometry. So the sign of that
// test is INVERTED here, not deleted: a toe facet that has become walkable is now the failure, and it
// is the same failure it always was — an .ini that quietly moves the walkable limit up past 46.9
// degrees turns this surface back into a staircase and nothing else would say so.
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

		// HOW MUCH OF THE FACE THE LIVE BAND STILL ACCEPTS, COUNTED FACET BY FACET rather than
		// asserted from the two ends. A band that has narrowed from the middle — somebody raises the
		// walkable limit AND lowers SurfMinNormalZ — could leave both ends outside it while the count
		// below is what says how much ride is actually left.
		int32 SurfableFacets = 0;
		int32 WalkableFacets = 0;
		int32 WallFacets = 0;
		for (int32 Facet = 0; Facet < kFacetCount; ++Facet)
		{
			const double Tangent = FacetTangent(Facet);
			if (Tangent <= LiveLoTan)
			{
				++WalkableFacets;
			}
			else if (Tangent >= LiveHiTan)
			{
				++WallFacets;
			}
			else
			{
				++SurfableFacets;
			}
		}

		const bool bNeverWalkable = kShallowestFacetTangent > LiveLoTan;
		const bool bNeverAWall = kSteepestFacetTangent < LiveHiTan;
		const bool bAllSurfable = (SurfableFacets == kFacetCount);
		const bool bDriftedFromLiterals =
			FMath::Abs(LiveLoTan - kBandLoTangent) > 1e-3 || FMath::Abs(LiveHiTan - kBandHiTangent) > 1e-3;

		OutReason = FString::Printf(
			TEXT("live band %.4f..%.4f deg (Nz %.4f..%.4f); shipped facets %.4f..%.4f deg over %.1f uu "
			     "of depth and %.1f uu of rise; %d of %d facets surfable (%d walkable, %d wall); "
			     "margins: toe %+.3f deg above the walkable limit, crest %+.3f deg below the ceiling%s"),
			LiveLoDeg, LiveHiDeg, LiveWalkableFloorZ, LiveSurfMinNormalZ,
			ShallowDeg, SteepDeg, kDepthUU, kHeightUU,
			SurfableFacets, kFacetCount, WalkableFacets, WallFacets,
			ShallowDeg - LiveLoDeg, LiveHiDeg - SteepDeg,
			bDriftedFromLiterals
				? TEXT("  [!! the band has MOVED from the tangents TraceSideRampProfile.h asserts "
				       "against — update kBandLoTangent / kBandHiTangent and re-check the build asserts]")
				: TEXT(""));

		if (!bNeverWalkable)
		{
			OutReason += TEXT("  FAIL: the toe facet is WALKABLE under the live band — the face is a "
			                  "staircase, which is the owner's item 5 broken by an .ini.");
		}
		if (!bNeverAWall)
		{
			OutReason += TEXT("  FAIL: the ramp's steepest facet is past the surf ceiling — its top is a wall.");
		}
		if (bNeverWalkable && bNeverAWall && !bAllSurfable)
		{
			OutReason += TEXT("  FAIL: both ends are inside the band but some facet in the middle is not, "
			                  "which means the live band is narrower than the face.");
		}

		return bAllSurfable && !bDriftedFromLiterals;
	}

	FString Describe()
	{
		return FString::Printf(
			TEXT("owner's parabola z = Hfull*u^2 (h=u^2 measured to 2.3e-5 on ramp_shell's 65 stations), "
			     "ridden over u %.4f..1 of itself so no part of it is walkable: face %.0f uu deep x %.0f uu "
			     "high x %.0f uu long, %d facets at %.2f..%.2f deg (the surf rails' own band edges); cut "
			     "from a full parabola %.0f x %.0f with its lower %.0f uu of rise not built; crest at out "
			     "%.0f (the pawn standoff's own face), toe at out %.0f; deck %.1f uu over the fillet's "
			     "%.0f uu terrace edge; a capsule can reach Z %.1f, and all %.1f uu of that is ride"),
			kProfileStartFrac, kDepthUU, kHeightUU, kLengthUU, kFacetCount,
			FMath::RadiansToDegrees(FMath::Atan(kShallowestFacetTangent)),
			FMath::RadiansToDegrees(FMath::Atan(kSteepestFacetTangent)),
			kFullDepthUU, kFullHeightUU, kUnbuiltRiseUU,
			kCrestOutFromWallUU, kToeOutFromWallUU,
			kHeightOverFilletEdgeUU, kFilletOuterTerraceTopUU,
			kReachableTopUU, kReachableSurfableVerticalUU);
	}
}
