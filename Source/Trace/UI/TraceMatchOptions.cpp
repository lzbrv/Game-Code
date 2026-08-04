// Trace — title-screen / match contract implementation. See TraceMatchOptions.h.

#include "UI/TraceMatchOptions.h"

#include "Trace.h"          // LogTraceGame
#include "TraceSettings.h"

namespace
{
	/**
	 * The UI's difficulty enum mapped onto the one the bots actually read.
	 *
	 * These are two separate enums by accident of parallel development, not by design: the title
	 * screen grew ETraceBotDifficulty at the same time the bot AI grew EBotDifficulty. They are kept
	 * distinct only so the UI layer does not have to include TraceSettings.h in its header; this
	 * function is the single point of contact between them.
	 */
	EBotDifficulty ToBotDifficulty(ETraceBotDifficulty Difficulty)
	{
		switch (Difficulty)
		{
		case ETraceBotDifficulty::Easy: return EBotDifficulty::Easy;
		case ETraceBotDifficulty::Hard: return EBotDifficulty::Hard;
		default:                        return EBotDifficulty::Normal;
		}
	}
}

FString TraceDifficulty::ToDisplayName(ETraceBotDifficulty Difficulty)
{
	switch (Difficulty)
	{
	case ETraceBotDifficulty::Easy: return TEXT("EASY");
	case ETraceBotDifficulty::Hard: return TEXT("HARD");
	default:                        return TEXT("NORMAL");
	}
}

FString TraceDifficulty::ToUrlValue(ETraceBotDifficulty Difficulty)
{
	return ToDisplayName(Difficulty).ToLower();
}

ETraceBotDifficulty TraceDifficulty::FromUrlValue(const FString& Value)
{
	const FString Trimmed = Value.TrimStartAndEnd().ToLower();

	if (Trimmed == TEXT("easy")   || Trimmed == TEXT("0")) { return ETraceBotDifficulty::Easy; }
	if (Trimmed == TEXT("normal") || Trimmed == TEXT("1")) { return ETraceBotDifficulty::Normal; }
	if (Trimmed == TEXT("hard")   || Trimmed == TEXT("2")) { return ETraceBotDifficulty::Hard; }

	return TraceDifficulty::Default;
}

ETraceBotDifficulty TraceDifficulty::Step(ETraceBotDifficulty Difficulty, int32 Delta)
{
	const int32 Index = FMath::Clamp(static_cast<int32>(Difficulty) + Delta, 0, TraceDifficulty::Count - 1);
	return static_cast<ETraceBotDifficulty>(Index);
}

void TraceDifficulty::ApplyToSettings(ETraceBotDifficulty Difficulty)
{
	// This used to multiply UTraceSettings::BotReactionTime / BotAimErrorDegrees / BotAggression /
	// BotDecisionInterval / BotSightRange by a per-difficulty curve. Those five properties are now
	// dead: ATraceBotController reads UTraceSettings::GetBotProfile() exclusively, so the old code
	// scaled numbers nothing consumed and then logged them as if they were in force. The log line
	// was actively misleading — it reported an aim error the bots did not have.
	//
	// The real knob set is the FTraceBotProfile chosen by difficulty, so all this has to do is latch
	// which profile is in force. UTraceSettings logs the result.
	UTraceSettings::SetBotDifficulty(ToBotDifficulty(Difficulty));
}

// ---------------------------------------------------------------------------------------------
// Scoring mode (spec v4 §7)
// ---------------------------------------------------------------------------------------------

void TraceScoring::ApplyToSettings(ETraceScoringMode Mode)
{
	// GetMutableDefault, not a static latch. The difficulty uses a latch because the thing being
	// selected is WHICH of three profiles is in force, which is not a single property. The mode IS a
	// single property, and it is the one the details panel edits — so writing it here means the
	// title screen, the travel URL, the Project Settings page and ATraceGameMode all read and write
	// the same storage, and there is no second copy to fall out of step.
	UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>();
	if (MutableSettings == nullptr || MutableSettings->ScoringMode == Mode)
	{
		return;
	}

	MutableSettings->ScoringMode = Mode;

	UE_LOG(LogTraceGame, Log, TEXT("Scoring mode set to %s."), *TraceScoringModeLabel(Mode));
}

ETraceScoringMode TraceScoring::GetCurrentSetting()
{
	return UTraceSettings::Get().ScoringMode;
}
