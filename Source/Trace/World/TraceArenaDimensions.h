// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// =================================================================================================
// TraceArenaDimensions.h — THE ARENA'S SIZE, IN ONE PLACE THE WHOLE MODULE CAN READ.
//
// ATraceArenaBuilder::FieldLength / FieldWidth / WallThickness take their defaults from here, and so
// does every other number that is DERIVED from the size of the field but cannot ask a live builder:
//
//   * TraceSideRampProfile::kLengthUU — the concave side ramps are a mesh generated at the length of
//     the side wall. It stays a LITERAL in its own header (generate_side_ramp.py and
//     import_side_ramp.py regex-parse it) and a static_assert there pins it to kFieldLengthUU, so the
//     next resize fails the build instead of shipping ramps that stop short of the end walls.
//   * ATraceCharacter's net cull distance — set in the pawn's constructor, long before any arena
//     exists, and it has to cover the whole field or half the roster is never relevant on a client.
//   * UTraceSettings::GetHitscanRangeUU() — the gun's reach is FieldDiagonalUU() below plus the
//     HitscanRangeMarginUU knob, so a resize here carries the gun with it. It used to be a typed
//     number (HitscanRange, last 43600) that every resize had to remember to move by hand.
//
// 2026-10-04: 38400 x 9600 -> 42240 x 10560 (the owner: "Scale up the map to be 10% longer and 10%
// wider, leaving the dimensions of all the objects and goals the same"). The pocket behind each goal
// (EndzoneDepth, 2400) did not change, so the goal planes moved out with the walls and the goals are
// still 2400 uu from their back walls; goal to goal is now 37440.
//
// ATraceArenaBuilder::WarnIfHitscanRangeIsShort() checks the gun's reach and the pawn net cull
// against the field the builder ACTUALLY made, in the log of every match, so a level whose builder
// was given a different size than this header still shows up.
// =================================================================================================
namespace TraceArenaDimensions
{
	/** Wall to wall along X, uu. 37440 goal to goal + 2 x 2400 pockets. */
	inline constexpr float kFieldLengthUU = 42240.f;

	/** Sideline to sideline along Y, uu. */
	inline constexpr float kFieldWidthUU = 10560.f;

	/** Perimeter wall thickness, uu. The outer faces stand this far beyond the inner ones. */
	inline constexpr float kWallThicknessUU = 200.f;

	/**
	 * The field's inner diagonal, wall face to wall face, uu: sqrt(L^2 + W^2), i.e. 43540 uu on
	 * 42240 x 10560. The longest sight line a player has, and the base UTraceSettings::
	 * GetHitscanRangeUU() adds its margin to.
	 */
	inline float FieldDiagonalUU()
	{
		return FMath::Sqrt(FMath::Square(kFieldLengthUU) + FMath::Square(kFieldWidthUU));
	}

	/**
	 * Headroom on the pawn net cull over the field's OUTER diagonal (walls included). 2%: a pawn
	 * standing against one corner is still relevant to a viewer pressed into the opposite one.
	 */
	inline constexpr float kPawnNetCullMargin = 1.02f;

	/**
	 * How far away a pawn stays network-relevant, uu — DERIVED from the field, never typed.
	 *
	 * sqrt((L + 2T)^2 + (W + 2T)^2) x 1.02, i.e. 44907 uu on 42240 x 10560 with 200 uu walls. That
	 * clears the 43540 uu inner diagonal and the 43618 uu corner-floor-to-opposite-wall-top
	 * diagonal, so no two points a pawn can occupy are ever out of each other's relevancy. Squared
	 * it is 2.02e9, under 2^31, so it is an ordinary float with no precision game being played.
	 */
	inline float PawnNetCullDistanceUU()
	{
		return kPawnNetCullMargin * FMath::Sqrt(
			FMath::Square(kFieldLengthUU + 2.f * kWallThicknessUU)
			+ FMath::Square(kFieldWidthUU + 2.f * kWallThicknessUU));
	}
}
