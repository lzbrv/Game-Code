// =================================================================================================
// Trace — TraceFixtureKickoff.h
//
// WAITING OUT THE KICKOFF INSTEAD OF RACING IT.
//
// A half boundary (ATraceGameMode::BeginHalf) puts every pawn back on its spawn pad, zeroes its
// velocity, clears every ability's cooldown AND transient state (a Ripple, a Zip, a suspend) and
// re-stages the Core. A harness that is half-way through measuring one of those things when the
// whistle goes reports a FAILURE of the ability, with nothing wrong in the game. That is what four
// harnesses were doing, every time, for the same reason:
//
//   * a headless run starts in the warm-up, and the fixture's own first move - giving the player a
//     character - is what releases the warm-up hold ("[Warmup] Everyone is ready; the match starts in
//     5.0s"), so the half-1 kickoff lands a few seconds into the fixture it was started by;
//   * Trace.Lily.FlightTest read "-522 uu of drift while holding nothing": the kickoff put her back
//     on the floor mid-hover. Trace.Lily.KeyTest read "-1264 uu while holding jump" and "+0 uu while
//     holding crouch": same reset, a second later. Trace.Rocco.Verify's two ripple-ride checks went
//     red in the eight-character batch when the reset destroyed the Ripple between "lay it" and "ride
//     it", and Trace.Move.AuditV16.Abilities could be caught the same way.
//
// So a fixture that needs N uninterrupted seconds asks IsHalfBoundaryDue() before it starts measuring
// and waits while the answer is yes. It does not try to prevent the kickoff (that would be the harness
// changing the rules it is measuring), and it does not guess a delay: it waits for the match state
// that says the boundary has happened.
//
// RED ARM: Trace.FixtureKickoff.Race 1 makes IsHalfBoundaryDue() always answer no, i.e. the fixtures race
// the kickoff again exactly as they did, so the failure can be reproduced in the binary that fixes it.
//
// Dev-only, authority-only. Every caller is itself compiled out of Shipping.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

class UWorld;

namespace TraceFixtureKickoff
{
	/**
	 * True when a half boundary would land inside the next @p SecondsNeeded of play, so a fixture that
	 * needs that long without its pawn being reset must wait first:
	 *
	 *   - the warm-up is counting down to the half-1 kickoff (any warm-up with a deadline: the fixture's
	 *     own character pick can shorten it to five seconds, so "far enough away" cannot be trusted);
	 *   - the half-time interval is running, or the period is waiting for its next dead ball;
	 *   - a half is being played and has less than @p SecondsNeeded left on its clock.
	 *
	 * False when nothing is scheduled (no ATraceGameState, a warm-up with no deadline, post-match).
	 *
	 * @param OutWhy one sentence for the fixture's log line; empty when the answer is false.
	 */
	bool IsHalfBoundaryDue(const UWorld* World, float SecondsNeeded, FString& OutWhy);

	/**
	 * How long a fixture waits for a boundary to pass before it gives up and says INVALID. A warm-up
	 * held for an open menu ends within 50 s at the latest; this covers that and a half-time interval.
	 */
	inline constexpr double MaxWaitSeconds = 90.0;
}

#endif // !UE_BUILD_SHIPPING
