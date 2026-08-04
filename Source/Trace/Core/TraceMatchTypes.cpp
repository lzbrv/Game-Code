// Trace — the words for the A/B scoring-mode toggle. See Core/TraceMatchTypes.h.
//
// This is the only translation unit that turns an ETraceScoringMode into text. Everything that
// displays the mode — the title screen row, the in-match HUD chip, the carry prompt, the log lines
// on both sides of a travel — comes through here, so the two modes are called the same thing
// everywhere and renaming one is a single edit.

#include "Core/TraceMatchTypes.h"

#include "TraceSettings.h"   // ETraceScoringMode itself

bool TraceIsGoalMode(ETraceScoringMode Mode)
{
	return Mode == ETraceScoringMode::ThrownCoreAndGoals;
}

const TCHAR* TraceScoringModeLetter(ETraceScoringMode Mode)
{
	return TraceIsGoalMode(Mode) ? TEXT("B") : TEXT("A");
}

const TCHAR* TraceScoringModeName(ETraceScoringMode Mode)
{
	return TraceIsGoalMode(Mode) ? TEXT("GOALS") : TEXT("ENDZONES");
}

FString TraceScoringModeLabel(ETraceScoringMode Mode)
{
	return FString::Printf(TEXT("MODE %s - %s"), TraceScoringModeLetter(Mode), TraceScoringModeName(Mode));
}

const TCHAR* TraceScoringModeBlurb(ETraceScoringMode Mode)
{
	return TraceIsGoalMode(Mode)
		? TEXT("NARROW GOALS.  THE CORE IS A REAL OBJECT - THROW IT, INTERCEPT IT.")
		: TEXT("FULL-WIDTH ENDZONES.  THE CORE IS A STATUS - CARRY IT, HOLD LMB TO PASS.");
}

const TCHAR* TraceScoringModeCarryVerb(ETraceScoringMode Mode)
{
	// The single most consequential difference the player has to be told about. Spec v4 §7: "The
	// carrier should be able to throw the core forward by left clicking" — the same button that
	// starts a 0.5 s hover-hold pass in mode A. A HUD that shows the wrong one is teaching the wrong
	// control, which is worse than showing nothing.
	return TraceIsGoalMode(Mode) ? TEXT("THROW") : TEXT("PASS");
}

FString TraceScoringModeToUrlValue(ETraceScoringMode Mode)
{
	return TraceIsGoalMode(Mode) ? FString(TEXT("b")) : FString(TEXT("a"));
}

ETraceScoringMode TraceScoringModeFromUrlValue(const FString& Value)
{
	const FString Trimmed = Value.TrimStartAndEnd().ToLower();

	if (Trimmed == TEXT("b") || Trimmed == TEXT("1") || Trimmed == TEXT("goals") || Trimmed == TEXT("goal"))
	{
		return ETraceScoringMode::ThrownCoreAndGoals;
	}

	// Everything else, including an outright typo, is mode A. Deliberately not symmetric with the
	// test above: mode A is the shipped game, and a mistyped URL that quietly starts the
	// experimental ruleset is a playtest whose notes describe the wrong build.
	return ETraceScoringMode::EndzoneStatusCore;
}

ETraceScoringMode TraceScoringModeStep(ETraceScoringMode Mode, int32 Delta)
{
	const int32 Index = FMath::Clamp(static_cast<int32>(Mode) + Delta, 0, TraceScoringModeCount - 1);
	return static_cast<ETraceScoringMode>(Index);
}
