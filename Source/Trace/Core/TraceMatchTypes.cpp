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
	// *** SPEC v25 §8 — "Change the name in the UI so goals is just goals not game mode B." ***
	//
	// This ONE line is the whole item. Every player-visible spelling of the mode in the game comes
	// through here: the scoreboard footer ("1ST HALF   -   MODE B - GOALS" until now), the results
	// screen's subtitle, and the title screen's own confirmation lines. The "MODE B - " prefix was
	// manufactured here and nowhere else, so deleting it here deletes it everywhere at once — which
	// is exactly why the label was centralised in the first place (see this file's header).
	//
	// BOTH MODES LOSE THE PREFIX, not just B. The note is about GOALS, but this function is the
	// shared one and a stepper that reads "MODE A - ENDZONES" on the left and "GOALS" on the right
	// would look like a bug in the menu rather than a rename. "ENDZONES" and "GOALS" are each
	// already unambiguous — the letters only ever existed as the design owner's shorthand.
	//
	// WHAT DELIBERATELY DID NOT CHANGE, per the note's own carve-out ("internal enum names and
	// console commands may keep their identifiers"):
	//   * ETraceScoringMode::ThrownCoreAndGoals / ::EndzoneStatusCore — serialised into
	//     Config/DefaultGame.ini, so renaming them would silently reset every host's mode;
	//   * every Trace.ModeB.* console variable and its help text;
	//   * the "?mode=a" / "?mode=b" URL token, which is typed by hand into run scripts;
	//   * TraceScoringModeLetter(), kept because the letter is still how the notes and the logs
	//     refer to the two rulesets. It simply no longer reaches the player's eyes.
	return FString(TraceScoringModeName(Mode));
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
