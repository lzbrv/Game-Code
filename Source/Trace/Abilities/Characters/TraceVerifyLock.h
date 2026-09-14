// Trace — one character fixture at a time.
//
// WHY THIS EXISTS. The per-character verifies (Trace.Rocco.Verify, Trace.Chut.Verify, and the six
// siblings) each do the same thing to set themselves up: find the first player state that has an
// ability component, give it their character, and drive it through a scripted sequence across many
// frames. They are ASYNCHRONOUS — scheduled on tickers — so running two from one -TraceExec list
// does not run them in order, it runs them ON TOP OF EACH OTHER, both steering the same pawn.
//
// The symptom is not a crash. It is a fixture reporting that an ability does not work:
//
//     [Ability] TracePlayerState_0 loadout Rocco/Rocco/Rocco     17:26:32.870
//     [Ability] TracePlayerState_0 loadout Slimeball/Slimeball   17:26:32.885   <-- 15ms later
//     [ROCCO]   FAIL  the midair second jump fires ...  consumed=0
//
// Rocco's second jump "failed" because by the time it was pressed the pawn was Slimeball. Run alone,
// the identical build passes all eleven assertions. That cost several rounds of stashing and
// rebuilding to establish, twice, which is exactly the tax a silent-corruption bug charges.
//
// SO: A FIXTURE CLAIMS THE SUBJECT BEFORE IT STARTS, and a second one refuses rather than
// interleaving. Refusing is the whole point — a fixture that cannot run says INCONCLUSIVE, which is
// a true answer, where the old behaviour produced a FALSE FAILURE about shipped gameplay.
//
// THE CLAIM EXPIRES. Release is best effort: these fixtures have many early-out paths and a leaked
// lock would disable every character verify for the rest of the session, which is a worse failure
// than the one being fixed. So a claim carries a deadline in REAL time and a later fixture may take
// the subject once it passes. Real time, not world time, because a fixture that froze the world
// clock is precisely one that will never release.

#pragma once

#include "CoreMinimal.h"

namespace TraceVerifyLock
{
	/**
	 * Claims the shared test subject for @p FixtureName.
	 *
	 * @param ExpectedSeconds how long this fixture may hold the subject before another may take it.
	 *                        Generous is correct: the cost of over-estimating is one refused fixture,
	 *                        the cost of under-estimating is the corruption this file exists to stop.
	 * @return false if another fixture holds it. The caller must log and return.
	 */
	TRACE_API bool TryClaim(const TCHAR* FixtureName, double ExpectedSeconds = 60.0);

	/** Releases the claim if @p FixtureName holds it. Safe to call when it does not. */
	TRACE_API void Release(const TCHAR* FixtureName);

	/**
	 * Releases whoever holds it.
	 *
	 * For the fixtures whose verdict is printed by a shared helper that does not know which command
	 * it is serving. Safe BECAUSE of the lock: only one fixture can be running, so "whoever holds it"
	 * and "me" are the same claim. Do not use it anywhere that invariant is not already guaranteed.
	 */
	TRACE_API void ReleaseAny();

	/** Who holds it, for the refusal message. Empty when free. */
	TRACE_API FString CurrentHolder();

	/**
	 * Claims the subject, or QUEUES this fixture to run again once the holder is done.
	 *
	 * *** THIS IS THE ONE THE FIXTURES CALL. *** Refusing outright was the honest minimum, but it
	 * makes a batch useless: `-TraceExec="Trace.Chut.Verify|Trace.Mace.Verify|Trace.Rocco.Verify"`
	 * fires all three within a millisecond, so two of them would report INCONCLUSIVE and the run
	 * would cover one character. Queuing turns that same command line into what the person typing it
	 * obviously meant — the three fixtures, one after another.
	 *
	 * The requeue re-executes the console command itself rather than holding a callback, so no
	 * fixture has to be restructured to be queueable and none of them can hold a stale pointer to a
	 * pawn across the wait.
	 *
	 * @param CommandName the console command to re-run, which is also the lock's identity.
	 * @return true if the subject is yours now. false means "queued, or given up" — the caller must
	 *         return either way, and the queue will call the command again if it is coming back.
	 */
	TRACE_API bool ClaimOrQueue(const TCHAR* CommandName, double ExpectedSeconds = 60.0);
}
