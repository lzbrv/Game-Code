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
// the single fact this whole file rests on, and it is why the profile can live here as a handful of
// numbers instead of as a table: the shipped surface is not an approximation of the owner's curve,
// it IS the owner's curve, evaluated over whatever stretch of itself the physics asks for.
//
//     z(u) = Hfull * u^2,    u in [kProfileStartFrac, 1]
//
// Scripts/generate_side_ramp.py PARSES THE CONSTANTS BELOW out of this header and sweeps that curve
// along the sideline. There is therefore one source of truth for the shape, and a change here
// changes the mesh the next time it is generated — it cannot drift.
//
// -------------------------------------------------------------------------------------------------
// WHAT CHANGED, AND WHY THE PREVIOUS VERSION OF THIS FILE WAS WRONG FOR THE JOB
// -------------------------------------------------------------------------------------------------
// THE RAMP SHIPPED BEFORE THIS PASS RODE FROM u = 0. That meant its lower half was WALKABLE — 428 uu
// of run-up at 0.8..44 degrees, then a surfable face above it — and the walk-up was defended here as
// "the entrance". The owner has now looked at it and asked for the opposite, in as many words:
//
//     "The buttresses should now be steep enough that players can A/D into them to move up, and will
//      automatically slide down them if they do not."
//     "They should not be walkable."
//     "Change the buttresses to match the shape and steepness of the surf ramps."
//
// A parabola cannot be entirely inside the surf band while it starts at u = 0: its chord slopes run
// from (H/D)/N at the toe to (H/D)(2N-1)/N at the lip, a ratio of 2N-1 = 127, and the band's own
// ratio is only tan(63.256)/tan(44.765) = 2.00. So the shape and the requirement are incompatible
// AT THAT PLACEMENT and at no other — which is what kProfileStartFrac fixes. The build rides the
// UPPER SEGMENT of the same parabola, from u = kProfileStartFrac to u = 1, translated so its toe
// sits on the floor. Every point of it is still a point of the owner's curve; the part that was
// walkable is simply not built.
//
// The steepness the segment is cut to is NOT invented here either. ATraceArenaBuilder's surf rails —
// the structure the owner says "work very well" — cut their arc from the LIVE surf band with a two
// degree margin at each end (TraceArenaConstants::SurfRailBandMarginDegrees). kToeTangent and
// kCrestTangent are the tangents of those same two angles, so "match the steepness of the surf
// ramps" is a shared pair of numbers rather than a resemblance.
//
// -------------------------------------------------------------------------------------------------
// HOW A BODY GETS ON, WHICH IS THE REQUIREMENT THAT HAS KILLED EVERY PREVIOUS ATTEMPT
// -------------------------------------------------------------------------------------------------
// Every side-ramp attempt in this project before the concave one died the same death: geometry that
// measured surfable but had NO WAY ON (the w10 side-wall band scored 0 of 6 approaches because its
// toe sat 600 uu up a terraced bank). The concave ramp answered that with a walkable lower half. That
// answer is now forbidden, so the answer has to be a different one, and it has to be stated:
//
//     THE ENTRANCE IS THE GROUND-ENTRY RULE, AND THE ENTRANCE IS 38400 uu WIDE.
//
// UTraceCharacterMovementComponent::HandleImpact already implements DEMO 29 item 4(b): a pawn that is
// ON ITS FEET and leans into a surf plane with at least GetSurfGroundEntryMinApproachSpeed() of
// into-the-face speed LEAVES THE GROUND and has its velocity clipped against that plane — the same
// two operations PhysFalling performs on a surfer. At the shipped 800 uu/s ground limit that
// threshold is 160 uu/s, which a running pawn reaches at 11.5 degrees off parallel. It was measured
// on the surf rails at five approach angles: 5 of 5 gained speed, 800 -> 1001..1089 uu/s.
//
// So the requirement this profile has to meet is not "leave a walkable strip", it is:
//
//   1. THE TOE MUST BE ON THE FLOOR, over the whole run, so the contact exists at all. That is what
//      kToeOutFromWallUU and the fillet asserts below are for.
//   2. THE TOE FACET MUST BE INSIDE THE BAND — steeper than walkable so it is not a staircase, and
//      shallower than the ceiling so the rule accepts it. That is kShallowestFacetTangent.
//
// Both are asserted. What a player does is run down the wall and lean in, anywhere along 38.4 km of
// toe line — not find one access ramp. That is strictly more entrance than the walk-up it replaces,
// which could only be entered where it was not blocked by a buttress pier.
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
// A CHECK THAT CANNOT FAIL PROVES NOTHING, so six perturbations were built ON PURPOSE after this
// rewrite, with `Scripts/build.sh` and nothing else changed. Every one of them returned
// `Result: Failed (OtherCompilationError)`. The list below is COPIED OFF THE COMPILER, not predicted
// — the full logs are BUTTRESS-BUILD.md §6 — and the angles quoted are the perturbed build's own:
//
//   kProfileStartFrac -> 0.30   toe 34.13 / lip 65.62 deg. FOUR fire: the quotient check (0.30 is not
//                               kToeTangent / kCrestTangent), the derived-height check, NEVER BE
//                               WALKABLE (34.13 is under the 44.765 limit — a staircase, i.e. the
//                               owner's item 5 failing silently) and NEVER BECOME A WALL (65.62 is
//                               past the 63.256 ceiling). One knob, both ends of the band lost.
//   kHeightUU -> 1400           toe 53.77 / lip 66.68. NEVER BECOME A WALL and the derived-height
//                               check (1400 against the 1097.0 the tangents and kDepthUU imply).
//   kHeightUU -> 800            toe 37.95 / lip 52.96. NEVER BE WALKABLE and the derived-height check.
//                               Note it is the TOE that fails here, not the lip: a face can be short
//                               enough to be a staircase while its top is still nowhere near a wall.
//   kCrestOutFromWallUU -> -20  THE CREST MUST NOT PUSH PAST THE STANDOFF SHELL, alone. The one
//                               perturbation of the six that fires exactly one assert.
//   kDepthUU -> 600             toe 53.54 / lip 66.50, toe at out 640. THE TOE MUST LAND ON FLAT
//                               FLOOR (640 against the fillet's out 605 plus a 100 uu margin), NEVER
//                               BECOME A WALL, and the derived-height check.
//   kDepthUU -> 1200            toe 34.09 / lip 48.99, toe at out 1240. THE LANE MUST SURVIVE,
//                               NEVER BE WALKABLE, and the derived-height check.
//
// Restoring every constant builds `Result: Succeeded` again and the header is byte-identical to this
// one, so the asserts are discriminating and not merely loud.
//
// FIVE OF THE THIRTEEN CONDITIONS WERE NOT INDIVIDUALLY MADE TO FIRE, and saying which is part of the
// same honesty: kProfileExponent == 2 and kProfileStartFrac in (0,1) are shape guards with no
// interesting neighbouring value; kShallowestFacetTangent < kSteepestFacetTangent cannot fail at all
// for a positive H/D and u0 in (0,1), so it is a structural statement rather than a test; and
// kReachableSurfableVerticalUU > 600 and kHeightOverFilletEdgeUU > terrace + 15 are both DOMINATED by
// the toe assert once the derived-height check holds — H is then D x 1.44227, so the ride only drops
// under 600 uu when kDepthUU falls under ~422, and a toe at out 462 has already failed "THE TOE MUST
// LAND ON FLAT FLOOR". They are still worth keeping: they stop being dominated the moment somebody
// widens the derived-height tolerance or moves the fillet.
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
	// THE STEEPNESS, TAKEN FROM THE SURF RAILS RATHER THAN INVENTED
	// ---------------------------------------------------------------------------------------------

	/**
	 * tan of the angle the TOE is cut at: the walkable limit plus the surf rails' own band margin.
	 *
	 * acos(0.71) = 44.7651 deg + TraceArenaConstants::SurfRailBandMarginDegrees (2) = 46.7651 deg,
	 * tan = 1.0635923. This is literally the shallowest facet ATraceArenaBuilder::BuildSurfRails cuts,
	 * which is what "match the steepness of the surf ramps" means as a number.
	 */
	inline constexpr double kToeTangent = 1.0635923;

	/**
	 * tan of the angle the CREST is cut at: the surf ceiling minus the same margin.
	 *
	 * acos(0.45) = 63.2563 deg - 2 = 61.2563 deg, tan = 1.8232359. Again the surf rails' own steepest
	 * facet. The margin at both ends is what stops a float rounding on either edge of the band from
	 * putting the toe into walkable geometry or the crest into wall.
	 */
	inline constexpr double kCrestTangent = 1.8232359;

	/**
	 * WHERE ON THE OWNER'S PARABOLA THE BUILT FACE STARTS, as a fraction of the full run.
	 *
	 * The tangent of z = Hfull*u^2 at u is 2*Hfull*u/Dfull, i.e. it is LINEAR in u. So the ratio of
	 * the toe's slope to the crest's slope IS the ratio of their u, and the start fraction is forced:
	 *
	 *     kProfileStartFrac = kToeTangent / kCrestTangent = 1.0635923 / 1.8232359 = 0.5833542
	 *
	 * It is written out as a literal rather than as that quotient because Scripts/generate_side_ramp.py
	 * parses these constants with a number regex, and a constant it cannot parse is one it would have
	 * to guess. The static_assert below re-derives it and fails the build if the literal drifts from
	 * the quotient by more than 1e-6, so there is no second opinion about it.
	 *
	 * The 41.7% of the owner's curve above this point is what gets built. The 58.3% below it — the
	 * shallow part, which is the part that used to be walkable — is not built at all.
	 */
	inline constexpr double kProfileStartFrac = 0.5833542;

	// ---------------------------------------------------------------------------------------------
	// THE SCALE THE ARENA ASKS FOR. These are the mesh.
	// ---------------------------------------------------------------------------------------------

	/**
	 * Depth of the BUILT face, toe on the floor to crest at the wall, uu.
	 *
	 * 760, unchanged across this rewrite, and that is deliberate: it is what the lane can afford and
	 * what the fillet demands, and neither of those moved. The side wall's inner face is at |Y| 4800
	 * and the crest sits 40 uu out from it, so the toe lands at |Y| 4000 and 8000 uu of clear floor is
	 * left between the two toes.
	 *
	 * READ THAT AGAINST THE RIGHT BASELINE. The floor was never 9600 wide, or even 8920: the wall
	 * fillet's lowest terrace already leaves the floor at |Y| 4195 (measured, six stations, both
	 * walls), so today's genuinely flat floor is 8390 uu. This costs 390 uu of it — 4.6% — and buries
	 * the fillet's two 20/39.5 uu terraces under the ride surface. For scale, the collaborator's
	 * ORIGINAL hand-placed side ramps were 946 uu deep.
	 */
	inline constexpr double kDepthUU = 760.0;

	/**
	 * Height of the BUILT face, floor to crest, uu.
	 *
	 * 1096, and it is DERIVED rather than chosen — see the static_assert against
	 * kDerivedHeightUU below. Fixing the toe and crest tangents fixes the ratio of rise to run for
	 * the truncated segment, so once kDepthUU is 760 the height is not free:
	 *
	 *     H = D * kCrestTangent * (1 + kProfileStartFrac) / 2 = 1096.99
	 *
	 * and 1096 is that, rounded to a whole uu (the rounding moves the toe facet by 0.04 deg and the
	 * crest facet by 0.07 deg, both of which the band asserts still pass with ~2.1 deg to spare).
	 *
	 * It is 436 uu taller than the 660 the walkable-entrance version shipped, and ALL 1096 of it is
	 * ride: the previous build spent 439 uu of its height on a walk-up that the owner has now
	 * refused. Measured reachable vertical goes from 430.7 uu to 1086.9 uu, i.e. the ride is 2.5x
	 * longer, on the same 760 uu of floor.
	 */
	inline constexpr double kHeightUU = 1096.0;

	/**
	 * How many facets the profile is cut into.
	 *
	 * 64, which is the owner's own tessellation (their 65 centreline stations are 64 spans), so the
	 * shipped mesh samples their curve at exactly the stations they authored it at. It also sets the
	 * PHYSICAL angles: complex-as-simple collision reports facet chord normals, not the analytic
	 * tangent, so every assert below is written against the CHORD slope of a facet and not against the
	 * tangent at a point. At 64 facets each is ~11.9 uu deep and the chord and the tangent are 0.13
	 * degrees apart at the toe.
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
	 * the wall's neon trim. Measured on the baked map: `Trace.Arena.SideRamp`'s body sweep reports a
	 * capsule stopped 74 uu out on `Wall_Standoff_Y_Standoff_0`, and 74 = 40 + the 34 uu capsule
	 * radius, so the shell's inner face really is at 40.
	 *
	 * Landing the crest exactly on that face does two things at once:
	 *
	 *   1. NO SLOT OPENS. A crest short of the wall leaves a gap a player can be dropped into. The
	 *      standoff shell is already a pawn-impassable box filling exactly this gap, so the gap cannot
	 *      be entered, and the shell doubles as the lip's guard rail.
	 *   2. THE WHOLE FACE STAYS REACHABLE. Anything the crest is pushed PAST the shell is face a body
	 *      can never touch: a pawn pressed on a ~61 degree face has its capsule centre about 29 uu out
	 *      along the normal, so it can reach a surface point at out 45 and no further. At crest-out 40
	 *      that costs 9.1 uu of a 1096 uu face; burying the crest inside the wall would cost the whole
	 *      of whatever was buried.
	 */
	inline constexpr double kCrestOutFromWallUU = 40.0;

	// ---------------------------------------------------------------------------------------------
	// WHAT IS ALREADY ON THAT WALL. All three measured with Trace.Arena.SideRamp on Arena_Baked, at
	// six stations, +Y and -Y identical to the uu. They are here because the ramp has to CLEAR them.
	// ---------------------------------------------------------------------------------------------

	/** Depth of the wall's pawn-only standoff shell (TraceArenaConstants::WallNeonStandoff). */
	inline constexpr double kWallStandoffDepthUU = 40.0;

	/** How far out a capsule can press a ~61 degree face before the standoff stops it, uu. */
	inline constexpr double kReachableSurfaceOutUU = 45.0;

	/**
	 * The wall fillet's OUTER edge — where its lowest terrace leaves the floor — as a distance out
	 * from the wall face, and that terrace's height. Measured: flat floor at Z 0 from out 1400 in to
	 * out 605, a 20 uu terrace from 605 to 385, a 39.5 uu terrace from 385 to 345.
	 *
	 * The ramp's toe lands OUTBOARD of this (out 800 against out 605), so the ramp's deck passes over
	 * the fillet rather than butting into it, and the static_assert below is what guarantees that.
	 * THIS IS ALSO HALF THE ENTRANCE: the ground-entry rule needs a pawn on the floor to be able to
	 * touch the toe facet, and a toe standing on a 20 uu terrace is a kerb between the two.
	 */
	inline constexpr double kFilletOuterEdgeOutUU = 605.0;
	inline constexpr double kFilletOuterTerraceTopUU = 20.0;

	// ---------------------------------------------------------------------------------------------
	// THE BAND. Literals here; re-derived live by ValidateAgainstLiveBand().
	// ---------------------------------------------------------------------------------------------

	/**
	 * tan(acos(WalkableFloorZ)). UCharacterMovementComponent's default WalkableFloorAngle is 44.765
	 * degrees (WalkableFloorZ 0.71) and this project does not override it. Anything shallower than
	 * this is FLOOR — you stand on it and walk up it, which is exactly what the owner's item 5 forbids
	 * for this surface.
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

	/**
	 * The FULL parabola the built face is a segment of. The face's own D and H are the constants
	 * above; these are the curve they were cut from, and they are what the generator sweeps.
	 */
	inline constexpr double kFullDepthUU = kDepthUU / (1.0 - kProfileStartFrac);
	inline constexpr double kFullHeightUU = kHeightUU / (1.0 - kProfileStartFrac * kProfileStartFrac);

	/** The height the full parabola has already reached at the built toe — the part not built, uu. */
	inline constexpr double kUnbuiltRiseUU = kFullHeightUU * kProfileStartFrac * kProfileStartFrac;

	/** u at the near edge of facet i, i in [0, kFacetCount]. u is on the FULL parabola. */
	inline constexpr double FacetU(int32 Facet)
	{
		return kProfileStartFrac
			+ (1.0 - kProfileStartFrac) * static_cast<double>(Facet) / static_cast<double>(kFacetCount);
	}

	/**
	 * The CHORD slope (dz/dy) of facet i, i in [0, kFacetCount).
	 *
	 * For z = Hfull u^2 the chord over [u_a, u_b] has slope (Hfull/Dfull) * (u_a + u_b). This is what
	 * a trace against the shipped collision actually reports — complex-as-simple traces geometry, not
	 * the analytic tangent — and it is what every assert below is written against.
	 */
	inline constexpr double FacetTangent(int32 Facet)
	{
		return (kFullHeightUU / kFullDepthUU) * (FacetU(Facet) + FacetU(Facet + 1));
	}

	/** The shallowest facet (the toe) and the steepest (the lip). */
	inline constexpr double kShallowestFacetTangent = FacetTangent(0);
	inline constexpr double kSteepestFacetTangent = FacetTangent(kFacetCount - 1);

	/**
	 * The height kDepthUU and the two tangents IMPLY, uu. kHeightUU has to be this, and the assert
	 * below is what says so — a height typed here that does not satisfy it would ship a face cut at
	 * angles other than the surf rails' and the file would still claim it matched them.
	 */
	inline constexpr double kDerivedHeightUU = kDepthUU * kCrestTangent * (1.0 + kProfileStartFrac) * 0.5;

	/** The toe's distance out from the wall face, uu — where the ramp meets the floor. */
	inline constexpr double kToeOutFromWallUU = kCrestOutFromWallUU + kDepthUU;

	/** Height of the BUILT deck at a distance `OutUU` out from the wall face, uu. */
	inline constexpr double HeightAtOut(double OutUU)
	{
		const double AlongFace = (kToeOutFromWallUU - OutUU) / kDepthUU;   // 0 at the toe, 1 at the crest
		const double U = kProfileStartFrac + (1.0 - kProfileStartFrac) * AlongFace;
		return kFullHeightUU * U * U - kUnbuiltRiseUU;
	}

	/** Height of the deck directly above the wall fillet's outer edge, uu. Must clear the terrace. */
	inline constexpr double kHeightOverFilletEdgeUU = HeightAtOut(kFilletOuterEdgeOutUU);

	/**
	 * The highest point on the face a BODY can actually touch, uu.
	 *
	 * Not kHeightUU: the wall's pawn-only standoff shell stops a capsule at out
	 * kReachableSurfaceOutUU, so anything above the deck height at that station is face nobody can
	 * ride. This is the number a ride table should be read against.
	 */
	inline constexpr double kReachableTopUU = HeightAtOut(kReachableSurfaceOutUU);

	/**
	 * Surfable vertical a body can actually reach, uu.
	 *
	 * It is the whole reachable face now, and that identity is the point of this rewrite: with no
	 * walkable segment there is no handoff height to subtract.
	 */
	inline constexpr double kReachableSurfableVerticalUU = kReachableTopUU;

	// ---------------------------------------------------------------------------------------------
	// THE ASSERTS
	// ---------------------------------------------------------------------------------------------

	static_assert(kProfileExponent == 2,
		"The owner's profile is a parabola (measured: h = u^2 to 2.3e-5). Changing the exponent ships "
		"a different curve from the one the owner supplied.");

	static_assert(kProfileStartFrac > 0.0 && kProfileStartFrac < 1.0,
		"The built face has to be a SEGMENT of the owner's curve. Outside (0,1) there is no segment.");

	static_assert(kProfileStartFrac * kCrestTangent > kToeTangent - 1e-6
		&& kProfileStartFrac * kCrestTangent < kToeTangent + 1e-6,
		"kProfileStartFrac MUST BE kToeTangent / kCrestTangent. The parabola's slope is linear in u, so "
		"that quotient is the only start fraction that cuts the face at the two angles this file claims "
		"it cuts it at. The literal has drifted from the quotient.");

	static_assert(kHeightUU > kDerivedHeightUU - 1.5 && kHeightUU < kDerivedHeightUU + 1.5,
		"kHeightUU IS NOT FREE. Fixing the toe and crest tangents fixes rise/run for the truncated "
		"segment, so H = D * kCrestTangent * (1 + kProfileStartFrac) / 2. A height that is not that is "
		"a face cut at angles other than the surf rails', while this file still says it matches them.");

	static_assert(kShallowestFacetTangent > kBandLoTangent,
		"THE RAMP MUST NEVER BE WALKABLE. Its shallowest facet — the toe — is shallower than the "
		"engine's walkable limit, so a player can simply walk up the bottom of it. That is the owner's "
		"item 5 ('they should not be walkable') failing, and it is silent without this assert. Raise "
		"kProfileStartFrac or kHeightUU.");

	static_assert(kSteepestFacetTangent < kBandHiTangent,
		"THE RAMP MUST NEVER BECOME A WALL. Its steepest facet is past the surf ceiling, so the top of "
		"the face refuses a ride instead of giving one. Lower kHeightUU or raise kDepthUU.");

	static_assert(kShallowestFacetTangent < kSteepestFacetTangent,
		"The face has to get steeper as it rises — that is what makes it a CURVE a rider has to re-aim "
		"the strafe on, which is the whole reason the owner's concavity was kept.");

	static_assert(kReachableSurfableVerticalUU > 600.0,
		"THE RIDE A BODY CAN ACTUALLY TAKE MUST BE WORTH TAKING. The free-standing surf rails this "
		"replaces gave 616 uu of rise, and the owner's verdict on them was 'the surf ramps work very "
		"well'. A wall run shorter than the thing it is replacing is a downgrade.");

	static_assert(kDepthUU < 1000.0,
		"THE LANE MUST SURVIVE. Two ramps this deep eat 2 x kDepthUU of a 9600 uu wide field; the "
		"collaborator's own original side ramps were 946 uu and that was the widest anyone has gone.");

	static_assert(kToeOutFromWallUU > kFilletOuterEdgeOutUU + 100.0,
		"THE TOE MUST LAND ON FLAT FLOOR. The wall fillet's lowest terrace leaves the floor at "
		"kFilletOuterEdgeOutUU; a toe inboard of that lands ON a 20 uu step. With the walkable run-up "
		"gone this is no longer merely a comfort: the ground-entry rule needs a pawn standing on the "
		"floor to be able to lean into the toe facet, and a kerb between the two is the whole entrance "
		"lost.");

	static_assert(kHeightOverFilletEdgeUU > kFilletOuterTerraceTopUU + 15.0,
		"THE DECK MUST CLEAR THE FILLET. Where the fillet's outer terrace starts, the ramp's deck has "
		"to be comfortably ABOVE it or the terrace's 20 uu riser stands proud through the ride "
		"surface. At D 760 / H 1096 / crest-out 40 the deck is 226.2 uu there against a 20 uu terrace.");

	static_assert(kCrestOutFromWallUU >= kWallStandoffDepthUU,
		"THE CREST MUST NOT PUSH PAST THE STANDOFF SHELL. Past it is face no capsule can reach, so "
		"every uu of burial is a uu of ride thrown away — and the shell is already what stops a player "
		"falling into the gap behind the crest.");

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
