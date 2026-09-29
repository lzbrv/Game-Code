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
//
// *** BUT THE DEADLINE IS THE BACKSTOP, NOT THE RELEASE. *** For a while only Rocco, Chut and the
// dash cloak released; the other six held the subject until their 60 s ran out. One at a time that
// looked like "slow but correct". In a batch of eight it was neither: every fixture sat out its
// predecessor's full minute, the queue gave up after three, and Oyster, Mace and Lily were never
// measured at all ("waited three minutes for Trace.Elle.Verify and gave up"). So EVERY fixture now
// releases on EVERY way it can end — a synchronous one with ON_SCOPE_EXIT straight after the claim,
// a ticker-driven one by handing its ticker to ReleaseWhenFinished below — and the deadline is left
// for the case nobody wrote code for.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

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

	/** Who holds it, as the command line that claimed it ("Trace.Rocco.Verify 1"). Empty when free. */
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
	 * THE GIVE-UP IS PER HOLDER, NOT PER QUEUE. A command gives up only after three minutes behind
	 * ONE holder; each time the subject changes hands the wait starts again. A fixed three minutes
	 * for the whole wait meant the eighth fixture in a batch had to fit behind seven others in that
	 * window, so the queue's depth decided which characters got tested. Behind one holder, three
	 * minutes cannot run out in practice: every claim expires first (60 s by default), and an expired
	 * claim is taken. It is there for a holder that keeps re-claiming and never finishes.
	 *
	 * THE REQUEUE CARRIES THE ARGUMENTS. It used to re-run the bare command name, so a queued
	 * `Trace.Rocco.Verify 1` came back as `Trace.Rocco.Verify` and ran every arm. The whole line is
	 * rebuilt from @p Args and re-run, and the wait is keyed on that line, so `Trace.Rocco.Verify 1`
	 * and `Trace.Rocco.Verify 2` queue as two runs rather than one.
	 *
	 * A SECOND RUN OF THE SAME COMMAND QUEUES TOO. The lock's identity is the command name, and
	 * TryClaim lets a holder re-claim itself; that let `Trace.Rocco.Verify 2` start on top of a running
	 * `Trace.Rocco.Verify 1`. Every call here is a fresh invocation, so a hold by the same name is
	 * another run and this one waits for it.
	 *
	 * THE WAIT IS ANNOUNCED HERE, once per holder: one Display line when it starts queuing and one each
	 * time the subject changes hands. The fixtures used to log their own "QUEUED behind" Warning on
	 * every one-second retry, which put about a hundred lines into a batch of eight.
	 *
	 * @param CommandName the console command to re-run, which is also the lock's identity.
	 * @param ExpectedSeconds the whole run, INCLUDING the fixture's own staging wait. A fixture that
	 *        waits up to 120 s for a pawn must say so, or a queued one takes the subject mid-staging.
	 * @param Args the command's own arguments, exactly as the console passed them.
	 * @return true if the subject is yours now. false means "queued, or given up" — the caller must
	 *         return either way, and the queue will call the command again if it is coming back.
	 */
	TRACE_API bool ClaimOrQueue(const TCHAR* CommandName, double ExpectedSeconds = 60.0,
		const TArray<FString>& Args = TArray<FString>());

	/**
	 * Wraps a ticker-driven fixture's ticker so the claim is released on the tick that ENDS the run,
	 * whichever way it ends — verdict, INVALID, "the world went away", "a participant went away".
	 *
	 * WHY A WRAPPER AND NOT A Release() AT THE VERDICT. Those fixtures stop by returning false from
	 * one lambda, and each has four or five places that do it. A Release beside each one is a list
	 * the next early-out will be missing from. The ticker stopping IS the run ending, so the release
	 * lives there, once. It runs after the body's last tick, so anything the body restores on its
	 * way out (a red arm's cvar, a knob) is back before the next fixture can claim.
	 *
	 *     FTSTicker::GetCoreTicker().AddTicker(TraceVerifyLock::ReleaseWhenFinished(
	 *         TEXT("Trace.Elle.Verify"), FTickerDelegate::CreateLambda([State](float) -> bool { ... })));
	 *
	 * For a single ticker that runs to the end only. A fixture that re-schedules itself as a chain of
	 * one-shot tickers (Rocco, Chut) would release after its first step; those release at the verdict.
	 */
	TRACE_API FTickerDelegate ReleaseWhenFinished(const TCHAR* FixtureName, FTickerDelegate Body);
}
