// =================================================================================================
// TraceSideRampProfile.h — THE OWNER'S CONCAVE RAMP, AS A NUMBER THE BUILD CAN CHECK.
//
// -------------------------------------------------------------------------------------------------
// WHAT THIS FILE IS
// -------------------------------------------------------------------------------------------------
// The owner supplied `tron-concave-ramp.obj` and said: "use that to replace the side ramps (you can
// stretch out/remodel to match what's needed, but that concavity is what is crucial)."
//
// Its ride surface, `ramp_shell`, was measured station by station off the OBJ and it is not merely
// "concave-ish": it is an EXACT PARABOLA. Normalise its 65 centreline stations to u = along-run in
// [0,1] and h = rise in [0,1] and every one of them satisfies h = u^2 to within 2.301e-5. That is
// the single fact this whole file rests on, and it is why the profile can live here as three numbers
// instead of as a table: the shipped mesh is not an approximation of the owner's curve, it IS the
// owner's curve, evaluated at whatever scale the physics asks for.
//
//     z(y) = ProfileHeightUU * (y / ProfileDepthUU)^2,    y in [0, ProfileDepthUU]
//
// Scripts/generate_side_ramp.py PARSES THE THREE CONSTANTS BELOW out of this header and sweeps that
// curve along the sideline. There is therefore one source of truth for the shape, and a change here
// changes the mesh the next time it is generated — it cannot drift.
//
// -------------------------------------------------------------------------------------------------
// WHY THE SCALE IS A PHYSICS DECISION AND NOT AN ART ONE
// -------------------------------------------------------------------------------------------------
// AS AUTHORED THE OWNER'S RAMP CAN NEVER BE SURFED. It rises 1.70 over 4.20; its steepest facet is
// 38.782 degrees, which is 5.983 degrees short of even the WALKABLE limit, let alone the surf band.
// Dropped into the arena at its authored aspect it is a run-up you walk and nothing else.
//
// But scaling the height changes every local slope — theta -> atan(k * tan theta) — and the parabola
// sweeps its slope monotonically from 0 to atan(2H/D). So there is exactly one aspect ratio H/D at
// which the curve starts flat, passes continuously through the walkable limit somewhere in its
// middle, and tops out inside the surf band without ever becoming a wall. THAT is the design:
//
//     the shallow lower half IS THE ENTRANCE, and it becomes surfable as you climb, with no seam.
//
// This matters more than anything else about the asset. Every previous side-ramp attempt in this
// project died the same death — geometry that measured surfable but had NO WAY ON (see the w10
// SIDERAMP-RIDE report: the side-wall band scored 0 of 6 approaches because its toe sat 600 uu up a
// terraced bank). A constant-angle face has the same flaw by construction: surfable everywhere,
// enterable nowhere. The concavity is not decoration. It is the entrance.
//
// -------------------------------------------------------------------------------------------------
// THE ASSERTS, AND WHAT EACH ONE CAN CATCH
// -------------------------------------------------------------------------------------------------
// The static_asserts below fail the BUILD if a knob change moves the design out of band. They are
// written in TANGENTS, not degrees, because atan is not constexpr — and the tangent form is the
// exact one anyway: a facet is surfable iff its chord slope lies strictly between tan(BandLo) and
// tan(BandHi). The two band tangents are literals, and ValidateAgainstLiveBand() below re-derives
// them from the LIVE movement component at runtime and shouts if they have moved, so the literals
// cannot silently go stale either. Build-time catches a source change; run-time catches an .ini one.
//
// A CHECK THAT CANNOT FAIL PROVES NOTHING, so every one of them was made to fail on purpose before
// this shipped, with `Scripts/build.sh` and nothing else changed. MEASURED, not asserted:
//
//   kHeightUU 660 -> 300   "THE RAMP MUST BECOME SURFABLE" fires (steepest facet drops to 33.5 deg,
//                          under the 44.765 deg walkable limit), and so do the handoff-inside-the-
//                          profile, worth-riding and clear-the-fillet asserts.
//   kHeightUU 660 -> 1200  "THE RAMP MUST NEVER BECOME A WALL" fires (steepest facet 72.3 deg, past
//                          the 63.256 deg surf ceiling), and so does "THE RUN-UP MUST BE A RUN-UP".
//   kCrestOutFromWallUU
//        40 -> -20         "THE DECK MUST CLEAR THE FILLET", "THE CREST MUST NOT PUSH PAST THE
//                          STANDOFF SHELL" and "THE RIDE A BODY CAN ACTUALLY TAKE MUST BE WORTH
//                          TAKING" all fire — that value is the FIRST DRAFT of this file, and these
//                          three asserts are what rejected it.
//
// Restoring every constant builds `Result: Succeeded` again, so the asserts are discriminating and
// not merely loud.
// =================================================================================================
#pragma once

#include "CoreMinimal.h"

namespace TraceSideRampProfile
{
	// ---------------------------------------------------------------------------------------------
	// THE OWNER'S CURVE. Read out of tron-concave-ramp.obj / ramp_shell, not chosen here.
	// ---------------------------------------------------------------------------------------------

	/**
	 * The exponent of the owner's profile. TWO, and it is measured, not assumed: the 65 stations of
	 * `ramp_shell`'s centreline fit h = u^2 with a maximum normalised deviation of 2.301e-5.
	 *
	 * It is a constant rather than a knob on purpose. Change it and the shipped ramp stops being the
	 * owner's ramp, which is the one thing this task was told not to do.
	 */
	inline constexpr int32 kProfileExponent = 2;

	/** The owner's authored run and rise, in their own units, kept so the report can quote them. */
	inline constexpr double kOwnerRun = 4.20;
	inline constexpr double kOwnerRise = 1.70;

	/** The owner's authored ramp width, which becomes the ALONG-THE-WALL repeat of the neon motif. */
	inline constexpr double kOwnerWidth = 2.60;

	// ---------------------------------------------------------------------------------------------
	// THE SCALE THE ARENA ASKS FOR. These three are the mesh.
	// ---------------------------------------------------------------------------------------------

	/**
	 * Depth of the ramp, toe on the floor to crest at the wall, uu.
	 *
	 * 760 is picked from the asserts below and from what the lane can afford, in that order. The side
	 * wall's inner face is at |Y| 4800 and the crest sits 40 uu out from it, so the toe lands at
	 * |Y| 4000 and 8000 uu of clear floor is left between the two toes.
	 *
	 * READ THAT AGAINST THE RIGHT BASELINE. The floor was never 9600 wide, or even 8920: the wall
	 * fillet's lowest terrace already leaves the floor at |Y| 4195 (measured, six stations, both
	 * walls), so today's genuinely flat floor is 8390 uu. This costs 390 uu of it — 4.6% — and buries
	 * the fillet's two 20/39.5 uu terraces under a surface you can walk up instead of step onto. For
	 * scale, the collaborator's ORIGINAL hand-placed side ramps were 946 uu deep.
	 */
	inline constexpr double kDepthUU = 760.0;

	/**
	 * Height of the ramp, floor to crest, uu.
	 *
	 * 660 with the depth above gives 439.4 uu of SURFABLE VERTICAL — within 1% of the 443.6 uu the
	 * w10 straight ramp delivered, which is the only ride on these walls anyone has ever measured, so
	 * it is the number to match rather than guess past. It also leaves 439.4 uu of WALKABLE RUN-UP,
	 * i.e. the split is almost exactly half and half by depth.
	 *
	 * The honest version of that figure is kReachableSurfableVerticalUU, which subtracts the part of
	 * the face the wall's pawn standoff shell keeps a capsule off: 430.7 uu, against ~394 uu on the
	 * w10 ramp measured the same way.
	 */
	inline constexpr double kHeightUU = 660.0;

	/**
	 * How many facets the profile is cut into.
	 *
	 * 64, which is the owner's own tessellation (their 65 centreline stations are 64 spans), so the
	 * shipped mesh samples their curve at exactly the stations they authored it at. It also sets the
	 * PHYSICAL angles: complex-as-simple collision reports facet chord normals, not the analytic
	 * tangent, so every assert below is written against the CHORD slope of a facet and not against
	 * atan(2H/D). At 64 facets each is ~11.9 uu deep and the two are 0.19 degrees apart at the lip.
	 */
	inline constexpr int32 kFacetCount = 64;

	/** Length of the extrusion along the sideline, uu. The field is 38400 long (FieldLength). */
	inline constexpr double kLengthUU = 38400.0;

	/**
	 * Where the crest sits, as a distance OUT from the side wall's inner face, uu.
	 *
	 * 40, AND IT IS THE STANDOFF SHELL'S OWN DEPTH, not a clearance picked by eye.
	 *
	 * Each side wall carries a PAWN-ONLY standoff box, TraceArenaConstants::WallNeonStandoff deep
	 * (40 uu), running the full length and the full height of the wall — it exists to keep bodies off
	 * the wall's neon trim. Measured on the baked map this run: `Trace.Arena.SideRamp`'s body sweep
	 * reports a capsule stopped 74 uu out on `Wall_Standoff_Y_Standoff_0`, and 74 = 40 + the 34 uu
	 * capsule radius, so the shell's inner face really is at 40.
	 *
	 * Landing the crest exactly on that face does two things at once:
	 *
	 *   1. NO SLOT OPENS. The w10 placement buried its crest 20 uu INSIDE the wall for this reason —
	 *      a crest short of the wall leaves a gap a player can be dropped into. The standoff shell is
	 *      already a pawn-impassable box filling exactly this gap, so the gap cannot be entered, and
	 *      the shell doubles as the lip's guard rail.
	 *   2. THE WHOLE FACE STAYS REACHABLE. Anything the crest is pushed PAST the shell is face a body
	 *      can never touch: a pawn pressed on a ~60 degree face has its capsule centre about 29 uu
	 *      out along the normal, so it can reach a surface point at out 45 and no further. Burying
	 *      the crest 20 uu inside the wall (the w10 value) would put the top 100 uu of a 660 uu face
	 *      behind that shell and cost 99 uu of ride: reachable surfable vertical 331 uu instead of
	 *      431 uu. Measured off this same geometry, not argued.
	 */
	inline constexpr double kCrestOutFromWallUU = 40.0;

	// ---------------------------------------------------------------------------------------------
	// WHAT IS ALREADY ON THAT WALL. All three measured with Trace.Arena.SideRamp on Arena_Baked, at
	// six stations, +Y and -Y identical to the uu. They are here because the ramp has to CLEAR them.
	// ---------------------------------------------------------------------------------------------

	/** Depth of the wall's pawn-only standoff shell (TraceArenaConstants::WallNeonStandoff). */
	inline constexpr double kWallStandoffDepthUU = 40.0;

	/** How far out a capsule can press a ~60 degree face before the standoff stops it, uu. */
	inline constexpr double kReachableSurfaceOutUU = 45.0;

	/**
	 * The wall fillet's OUTER edge — where its lowest terrace leaves the floor — as a distance out
	 * from the wall face, and that terrace's height. Measured: flat floor at Z 0 from out 1400 in to
	 * out 605, a 20 uu terrace from 605 to 385, a 39.5 uu terrace from 385 to 345.
	 *
	 * The ramp's toe lands OUTBOARD of this (out 800 against out 605), so the ramp's deck passes over
	 * the fillet rather than butting into it, and the static_assert below is what guarantees that.
	 */
	inline constexpr double kFilletOuterEdgeOutUU = 605.0;
	inline constexpr double kFilletOuterTerraceTopUU = 20.0;

	// ---------------------------------------------------------------------------------------------
	// THE BAND. Literals here; re-derived live by ValidateAgainstLiveBand().
	// ---------------------------------------------------------------------------------------------

	/**
	 * tan(acos(WalkableFloorZ)). UCharacterMovementComponent's default WalkableFloorAngle is 44.765
	 * degrees (WalkableFloorZ 0.71) and this project does not override it. Anything shallower than
	 * this is FLOOR — you stand on it and walk up it.
	 */
	inline constexpr double kBandLoTangent = 0.9918327;

	/**
	 * tan(acos(SurfMinNormalZ)). The knob's shipped default is 0.45 (DefaultGame.ini), i.e. 63.2563
	 * degrees. Anything steeper than this is WALL — the surf rule refuses it.
	 */
	inline constexpr double kBandHiTangent = 1.9845083;

	// ---------------------------------------------------------------------------------------------
	// DERIVED — all constexpr, all in tangents, no trig.
	// ---------------------------------------------------------------------------------------------

	/** Height of the profile at normalised depth u, uu. */
	inline constexpr double HeightAt(double U) { return kHeightUU * U * U; }

	/**
	 * The CHORD slope (dz/dy) of facet i, i in [0, kFacetCount).
	 *
	 * For z = H u^2 sampled at u_i = i/N, the chord over [u_i, u_{i+1}] has slope
	 * (H/D) * (u_i + u_{i+1}) = (H/D) * (2i + 1) / N. This is what a trace against the shipped
	 * collision actually reports, and it is what every assert below is written against.
	 */
	inline constexpr double FacetTangent(int32 Facet)
	{
		return (kHeightUU / kDepthUU) * static_cast<double>(2 * Facet + 1) / static_cast<double>(kFacetCount);
	}

	/** The shallowest facet (the toe) and the steepest (the lip). */
	inline constexpr double kShallowestFacetTangent = FacetTangent(0);
	inline constexpr double kSteepestFacetTangent = FacetTangent(kFacetCount - 1);

	/** Index of the FIRST facet that is surfable, i.e. the first one steeper than the walkable limit. */
	inline constexpr int32 FirstSurfableFacet()
	{
		for (int32 Facet = 0; Facet < kFacetCount; ++Facet)
		{
			if (FacetTangent(Facet) > kBandLoTangent)
			{
				return Facet;
			}
		}
		return kFacetCount;   // nothing surfable — the asserts below turn this into a build failure
	}

	inline constexpr int32 kFirstSurfableFacet = FirstSurfableFacet();

	/** Depth of the walkable run-up, uu: everything outboard of the first surfable facet. */
	inline constexpr double kWalkableDepthUU =
		kDepthUU * static_cast<double>(kFirstSurfableFacet) / static_cast<double>(kFacetCount);

	/** Depth of the surfable face, uu. */
	inline constexpr double kSurfableDepthUU = kDepthUU - kWalkableDepthUU;

	/** Height the walkable run-up climbs to, uu — the Z at which walking stops and surfing starts. */
	inline constexpr double kHandoffHeightUU =
		HeightAt(static_cast<double>(kFirstSurfableFacet) / static_cast<double>(kFacetCount));

	/** Vertical extent of the surfable face, uu. The number that decides how long a ride lasts. */
	inline constexpr double kSurfableVerticalUU = kHeightUU - kHandoffHeightUU;

	/** The toe's distance out from the wall face, uu — where the ramp meets the floor. */
	inline constexpr double kToeOutFromWallUU = kCrestOutFromWallUU + kDepthUU;

	/** Height of the deck directly above the wall fillet's outer edge, uu. Must clear the terrace. */
	inline constexpr double kHeightOverFilletEdgeUU =
		HeightAt((kToeOutFromWallUU - kFilletOuterEdgeOutUU) / kDepthUU);

	/**
	 * The highest point on the face a BODY can actually touch, uu.
	 *
	 * Not kHeightUU: the wall's pawn-only standoff shell stops a capsule at out
	 * kReachableSurfaceOutUU, so anything above the deck height at that station is face nobody can
	 * ride. This is the number a ride table should be read against.
	 */
	inline constexpr double kReachableTopUU =
		HeightAt((kToeOutFromWallUU - kReachableSurfaceOutUU) / kDepthUU);

	/** Surfable vertical a body can actually reach, uu. The honest version of kSurfableVerticalUU. */
	inline constexpr double kReachableSurfableVerticalUU = kReachableTopUU - kHandoffHeightUU;

	// ---------------------------------------------------------------------------------------------
	// THE ASSERTS
	// ---------------------------------------------------------------------------------------------

	static_assert(kProfileExponent == 2,
		"The owner's profile is a parabola (measured: h = u^2 to 2.3e-5). Changing the exponent ships "
		"a different curve from the one the owner supplied.");

	static_assert(kShallowestFacetTangent < kBandLoTangent,
		"THE RAMP MUST START WALKABLE. Its first facet is steeper than the walkable limit, so there is "
		"no way on to it from the floor — which is the exact failure this whole design exists to fix.");

	static_assert(kSteepestFacetTangent > kBandLoTangent,
		"THE RAMP MUST BECOME SURFABLE. Its steepest facet is still walkable, so it is a run-up and "
		"nothing more — which is what the owner's asset does at its AUTHORED aspect ratio (38.782 deg "
		"against a 44.765 deg limit). Raise kHeightUU or lower kDepthUU.");

	static_assert(kSteepestFacetTangent < kBandHiTangent,
		"THE RAMP MUST NEVER BECOME A WALL. Its steepest facet is past the surf ceiling, so the top of "
		"the face refuses a ride instead of giving one. Lower kHeightUU or raise kDepthUU.");

	static_assert(kFirstSurfableFacet > 0 && kFirstSurfableFacet < kFacetCount,
		"The walk-to-surf handoff must happen strictly INSIDE the profile, not at either end.");

	static_assert(kWalkableDepthUU > 300.0,
		"THE RUN-UP MUST BE A RUN-UP. Under ~3 m of walkable depth a pawn meets the surf band before "
		"it is on the ramp at all, and the entrance stops being an entrance.");

	static_assert(kSurfableVerticalUU > 400.0,
		"THE FACE MUST BE WORTH RIDING. The w10 straight ramp delivered 443.6 uu of surfable vertical "
		"for a 3.3-4.3 s ride; below ~400 the ride is shorter than the one this replaces.");

	static_assert(kDepthUU < 1000.0,
		"THE LANE MUST SURVIVE. Two ramps this deep eat 2 x kDepthUU of a 9600 uu wide field; the "
		"collaborator's own original side ramps were 946 uu and that was the widest anyone has gone.");

	static_assert(kToeOutFromWallUU > kFilletOuterEdgeOutUU + 100.0,
		"THE TOE MUST LAND ON FLAT FLOOR. The wall fillet's lowest terrace leaves the floor at "
		"kFilletOuterEdgeOutUU; a toe inboard of that lands ON a 20 uu step, and a 20 uu step at the "
		"entrance is exactly the kerb this design exists to remove.");

	static_assert(kHeightOverFilletEdgeUU > kFilletOuterTerraceTopUU + 15.0,
		"THE DECK MUST CLEAR THE FILLET. Where the fillet's outer terrace starts, the ramp's deck has "
		"to be comfortably ABOVE it or the terrace's 20 uu riser stands proud through the ride "
		"surface. At D 760 / crest-out 40 the deck is 43.5 uu there against a 20 uu terrace; at the "
		"first draft's crest-out -20 it was 20.8 uu, i.e. 0.8 uu of clearance and a z-fighting band.");

	static_assert(kCrestOutFromWallUU >= kWallStandoffDepthUU,
		"THE CREST MUST NOT PUSH PAST THE STANDOFF SHELL. Past it is face no capsule can reach, so "
		"every uu of burial is a uu of ride thrown away — and the shell is already what stops a player "
		"falling into the gap behind the crest.");

	static_assert(kReachableSurfableVerticalUU > 380.0,
		"THE RIDE A BODY CAN ACTUALLY TAKE MUST BE WORTH TAKING. The w10 straight ramp's 443.6 uu of "
		"band vertical was only ~394 uu reachable for the same standoff reason; this must beat that.");

	/**
	 * RUNTIME HALF OF THE SAME CHECK: are the band literals above still what the movement component
	 * says the band is?
	 *
	 * The static_asserts can only see this header. The band's shallow edge is
	 * UCharacterMovementComponent::WalkableFloorZ and its steep edge is the SurfMinNormalZ knob, and
	 * BOTH can be moved by an .ini without touching a line of C++. So this re-derives the two
	 * tangents from the numbers handed in and compares them to the literals.
	 *
	 * @param LiveWalkableFloorZ  GetWalkableFloorZ() off a live movement component (or its CDO).
	 * @param LiveSurfMinNormalZ  GetSurfMinNormalZ() off the same.
	 * @param OutReason           filled in with what moved and by how much, whether it passes or not.
	 * @return true if the shipped profile is still inside the live band.
	 */
	TRACE_API bool ValidateAgainstLiveBand(float LiveWalkableFloorZ, float LiveSurfMinNormalZ,
		FString& OutReason);

	/** One line describing the shipped design, for a log or a report. Never a restated intention. */
	TRACE_API FString Describe();
}
