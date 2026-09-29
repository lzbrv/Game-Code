// =================================================================================================
// Trace — TraceFixtureCore.h
//
// PUTTING THE CORE BACK WHERE A TEST FOUND IT.
//
// Several fixtures move the Core to stage their case — Trace.Ammo.CarrierTest needs a carrier and a
// non-carrier, Trace.Knife.CarrierImmunityTest pins it on a victim and forces passes, Trace.Lily's
// ZipVerify and DashTest pick it up and put it down. Each then "put it back" its own way, and two of
// the ways were wrong: the CarrierTest handed it to the pawn IT had staged as the carrier, which was
// the local player whenever the Core had been loose, and ZipVerify did not put it back at all. The
// next harness in the process then found the local player carrying, and a carrier cannot fire, swing
// a knife or reload — Trace.Knife.DualWeaponTest said NOT PROVEN, TestKnife aborted, Railgun.Verify
// failed and Trace.Ammo.BindTest waited for a usable pawn forever. Every one of them passes on its own.
//
// So there is one rule, written once:
//
//   * Capture who held it before the test touched anything.
//   * If that pawn is still alive, it gets the Core back.
//   * Otherwise (nobody held it, or the holder has since died) nobody should: a Core the fixture left
//     on somebody is put on the half-start spot, held by nobody — the same contested placement a half
//     starts with, through ATraceCore::KickoffContested. A loose Core in flight cannot be put back in
//     flight; "held by nobody" is the part the next test depends on.
//
// KEEPING THE LOCAL PLAYER'S HANDS FREE.
//
// Putting the Core back was not enough on its own. In a batch, the fixtures above restored it
// correctly, and then a bot on the local player's own team threw it to him while he stood idle
// between steps. He never lets go, so every later harness that drives his weapon found a carrier
// again — the same four failures, with nothing wrong in any fixture. Waiting for him to drop it
// cannot work either: nobody is at his controls.
//
// So a harness that needs his hands free takes the Core off him ITSELF (BeginClearOfCore) and keeps
// it off him for as long as it runs: ATraceCore::GatherCharacters, the roster that the pickup poll,
// the catch magnet, pass targeting, the pending kickoff grant and the turnover landing all choose
// from, leaves him out while he is kept clear. (A kill still hands it to the killer, as it must; none
// of the harnesses that keep him clear kills anybody.) EndClearOfCore stops that and, if Begin took
// the Core off him, gives it back by the rule above. If he was not carrying, End touches nothing: the
// Core is wherever play has taken it, and that is not the harness's to undo.
//
// Trace.FixtureCore.KeepClearTest proves the switch: a Core parked at his feet is his with nobody
// kept clear, and is not his while he is.
//
// Dev-only, authority-only. Every fixture that calls this is itself compiled out of Shipping.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

class ATraceCharacter;
class UWorld;

namespace TraceFixtureCore
{
	/** Who held the Core when a fixture started. */
	struct FCoreHolding
	{
		/** There was a Core to capture at all. */
		bool bCaptured = false;

		/** Somebody was carrying it. */
		bool bWasHeld = false;

		TWeakObjectPtr<ATraceCharacter> Holder;

		/** For the log line; the pawn may be gone by the time it is printed. */
		FString HolderName;
	};

	/** Reads the Core's holder in @p World. Changes nothing. */
	FCoreHolding CaptureCoreHolding(const UWorld* World);

	/**
	 * Puts possession back as @p Captured describes (see the file header for the rule).
	 *
	 * @return one sentence saying what it did, for the fixture's own log line.
	 */
	FString RestoreCoreHolding(UWorld* World, const FCoreHolding& Captured);

	// ---------------------------------------------------------------------------------------------
	// Keeping a pawn clear of the Core (see the file header).
	// ---------------------------------------------------------------------------------------------

	/** One harness's hold on a pawn's hands. Filled by BeginClearOfCore, closed by EndClearOfCore. */
	struct FClearOfCore
	{
		/** Who held the Core when Begin ran. Only used if Begin took it off the pawn. */
		FCoreHolding Before;

		TWeakObjectPtr<UWorld> ClearWorld;
		TWeakObjectPtr<ATraceCharacter> ClearPawn;

		/** Begin has run, whatever it managed. A harness asks this so it begins once. */
		bool bBegun = false;

		/** This hold is keeping the pawn out of the Core's roster (authority, a pawn and a Core). */
		bool bKeeping = false;

		/**
		 * The pawn WAS carrying and Begin took the Core off them. Their gun comes back over the view
		 * blend, so a harness gives it a moment before it asks anything of the weapon.
		 */
		bool bTookItOff = false;

		/** End has run. A second End does nothing. */
		bool bEnded = false;
	};

	/**
	 * Server. Frees @p Pawn's hands for a harness: captures who held the Core, takes it off @p Pawn if
	 * they have it (onto the half-start spot, held by nobody), and keeps it off them until
	 * EndClearOfCore. Safe on a client or without a Core: it does nothing and says so.
	 *
	 * @return one sentence saying what it did, for the harness's own log line.
	 */
	FString BeginClearOfCore(UWorld* World, ATraceCharacter* Pawn, FClearOfCore& OutClear);

	/**
	 * Stops keeping the pawn clear and, if Begin took the Core off them, puts possession back by the
	 * RestoreCoreHolding rule. Call it on every exit; it does nothing the second time.
	 *
	 * @return one sentence saying what it did.
	 */
	FString EndClearOfCore(FClearOfCore& InOutClear);

	/**
	 * The switch under Begin/End, for a test of the switch itself: while held, @p Pawn is left out of
	 * ATraceCore::GatherCharacters. Holds nest (a count); a hold that is never released lapses after
	 * KeepClearSafetySeconds, so a harness that dies mid-run cannot leave the player unable to touch
	 * the Core for the rest of the session.
	 */
	void KeepClearOfCore(ATraceCharacter* Pawn);
	void StopKeepingClearOfCore(const ATraceCharacter* Pawn);

	/** True while a harness is keeping @p Character clear of the Core. ATraceCore::GatherCharacters asks. */
	bool IsKeptClearOfCore(const ATraceCharacter* Character);

	/** How long an unreleased hold lasts. Longer than any harness that takes one runs. */
	constexpr double KeepClearSafetySeconds = 180.0;
}

#endif // !UE_BUILD_SHIPPING
