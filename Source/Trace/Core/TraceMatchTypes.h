// Trace — how a match ended (spec v4 §6).
//
// One enum and one string helper. It is its own header rather than a member of TraceTypes.h for the
// same reason ETraceScoringMode lives in TraceSettings.h: TraceTypes.h is included by nearly every
// file in the module and edited by nearly every ownership slice, and the match-result type belongs
// to the rules slice alone.
//
// Deliberately dependency-free. Nothing here may pull in a gameplay class or the settings header.
//
// WHERE ETraceScoringMode LIVES, because three files got this wrong at once and it is worth writing
// down. The enum itself is DECLARED IN TraceSettings.h, immediately above the
// UTraceSettings::ScoringMode property that selects it, and its enumerators are
// EndzoneStatusCore (mode A) and ThrownCoreAndGoals (mode B) — that spelling is what
// Config/DefaultGame.ini serialises, so it is not something a consumer may rename locally.
//
// THIS HEADER IS THE ONE INCLUDE ANY CONSUMER NEEDS. It pulls TraceSettings.h in so that including
// "Core/TraceMatchTypes.h" gives you the complete type, the mode helpers and the match-end reason
// together — a header that half-declares a type is how three files ended up disagreeing about the
// enumerator names in the first place. A file that genuinely only wants to branch on the mode and
// cannot afford the settings header may still forward declare it itself (`enum class
// ETraceScoringMode : uint8;`, legal because the underlying type is fixed), as TraceCore.h does.
//
// What is DEFINED here is everything needed to TALK about that enum — the letter, the name, the
// blurb, the carry verb, the "?mode=" token. One spelling of "GOALS" for the title screen, the HUD
// and the logs, instead of three that drift.
//
// SPEC v25 §8 — "goals is just goals, not game mode B" — is a one-line edit in TraceMatchTypes.cpp
// precisely BECAUSE of that centralisation. The scoreboard footer, the results subtitle and the
// title screen all read TraceScoringModeLabel(); none of them spells the mode itself.

#pragma once

#include "CoreMinimal.h"
#include "Containers/UnrealString.h"
#include "UObject/ObjectMacros.h"

#include "TraceSettings.h"           // ETraceScoringMode — see the note above

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

// ---------------------------------------------------------------------------------------------
// Talking about ETraceScoringMode (spec v4 §7)
//
// The one spelling of every string the A/B toggle needs, shared by the title screen, the in-match
// HUD, the travel URL and the logs. Defined in TraceMatchTypes.cpp because the definitions need the
// real enum and this header deliberately only has the forward declaration.
// ---------------------------------------------------------------------------------------------

/** How many modes there are. Drives the title screen's left/right stepping. */
inline constexpr int32 TraceScoringModeCount = 2;

/** URL option carried from the title screen into the arena: "?mode=b". */
inline const TCHAR* TraceScoringModeUrlOption = TEXT("mode");

/** True in mode B — narrow goals, and a Core that is thrown and intercepted. */
TRACE_API bool TraceIsGoalMode(ETraceScoringMode Mode);

/**
 * "A" / "B" — how the design owner refers to the two modes IN THE NOTES.
 *
 * SPEC v25 §8 TOOK THIS OFF THE SCREEN. It is no longer part of any player-visible string; it is
 * kept because the letters are still what the design notes, the run scripts and the Trace.ModeB.*
 * console variables are named after, and a future log line has every right to print one.
 */
TRACE_API const TCHAR* TraceScoringModeLetter(ETraceScoringMode Mode);

/** "ENDZONES" / "GOALS" — what the mode does, for anyone who has not read the notes. */
TRACE_API const TCHAR* TraceScoringModeName(ETraceScoringMode Mode);

/**
 * "ENDZONES" / "GOALS". The full label the menu, the HUD and the logs show.
 *
 * SPEC v25 §8: this used to read "MODE A - ENDZONES" / "MODE B - GOALS", and the prefix was
 * manufactured in exactly one place (TraceScoringModeLabel's body). "Goals is just goals."
 */
TRACE_API FString TraceScoringModeLabel(ETraceScoringMode Mode);

/** One line of plain English, so the toggle means something before it is committed to. */
TRACE_API const TCHAR* TraceScoringModeBlurb(ETraceScoringMode Mode);

/** What LMB does while carrying, in this mode. The HUD prompt, and the reason the HUD must branch. */
TRACE_API const TCHAR* TraceScoringModeCarryVerb(ETraceScoringMode Mode);

/** Lowercase token for the travel URL: "a" / "b". */
TRACE_API FString TraceScoringModeToUrlValue(ETraceScoringMode Mode);

/**
 * Parses "a"/"b", "0"/"1" or the long spellings. Anything unrecognised falls back to MODE A, never
 * to B: a typo must not silently start the experimental ruleset.
 */
TRACE_API ETraceScoringMode TraceScoringModeFromUrlValue(const FString& Value);

/** Clamped index arithmetic, so the menu's left/right keys cannot run off either end. */
TRACE_API ETraceScoringMode TraceScoringModeStep(ETraceScoringMode Mode, int32 Delta);
