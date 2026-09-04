// Trace — how a match ended (spec v4 §6).
//
// One enum and one string helper. It is its own header rather than a member of TraceTypes.h because
// TraceTypes.h is included by nearly every file in the module and edited by nearly every ownership
// slice, and the match-result type belongs to the rules slice alone.
//
// Deliberately dependency-free. Nothing here may pull in a gameplay class or the settings header.
//
// WHAT USED TO LIVE HERE, because the emptiness is the point. Spec v4 §7's A/B SCORING TOGGLE —
// ETraceScoringMode, declared in TraceSettings.h next to the UTraceSettings::ScoringMode property
// that selected it — needed one spelling of the letter, the name, the blurb, the carry verb and the
// "?mode=" token, shared by the title screen, the in-match HUD, the travel URL and the logs. That
// centralisation is what made spec v25 §8 ("goals is just goals, not game mode B") a one-line edit.
//
// The toggle is now GONE, ruleset and all: the endzone game was removed, goals is simply the game,
// and with one ruleset there is nothing to name, no letter to print, no URL token to parse and no
// stepper to clamp. TraceMatchTypes.cpp, which held every one of those definitions, was deleted
// rather than left as a set of functions that can only return one answer.
//
// If a second ruleset is ever wanted again, put its enum back beside its selecting property and put
// its words back here: one spelling in one file is what stopped three files disagreeing about the
// enumerator names the first time.

#pragma once

#include "CoreMinimal.h"
#include "Containers/UnrealString.h"
#include "UObject/ObjectMacros.h"

#include "TraceMatchTypes.generated.h"

/**
 * WHY THE MATCH STOPPED. Replicated on ATraceGameState so the results screen can say which.
 *
 * Spec v4 §6 asks for this in as many words: the mercy rule ends matches early, and "the post-match
 * screen should say the match ended by mercy rather than by clock". A 9-1 mercy win and a 9-1
 * full-time win produce the identical scoreboard and are not the same result — the difference only
 * exists if it is recorded, and it matters most to whoever reads a result they did not watch.
 */
UENUM(BlueprintType)
enum class ETraceMatchEndReason : uint8
{
	/** The match has not reached PostMatch. The value every live match carries. */
	NotEnded = 0,

	/**
	 * FULL TIME: the final half's clock expired. The highest score wins; equal scores are a genuine
	 * draw, because with the score cap removed there is no longer anything to break a tie against.
	 */
	Clock = 1,

	/**
	 * THE MERCY RULE: one team's lead reached UTraceSettings::MercyRuleLead and play stopped there
	 * and then, in whichever half it happened. See ATraceGameMode::CheckMercyRule().
	 */
	Mercy = 2
};

/** "FULL TIME" / "MERCY RULE" — the headline the results screen leads with. */
TRACE_API inline const TCHAR* TraceMatchEndReasonHeadline(ETraceMatchEndReason Reason)
{
	switch (Reason)
	{
	case ETraceMatchEndReason::Mercy: return TEXT("MERCY RULE");
	case ETraceMatchEndReason::Clock: return TEXT("FULL TIME");
	default:                          return TEXT("MATCH OVER");
	}
}
