// Trace — title-screen / match contract implementation. See TraceMatchOptions.h.

#include "UI/TraceMatchOptions.h"

#include "Misc/ConfigCacheIni.h"   // GConfig — the characters toggle's and the difficulty's storage
#include "Trace.h"          // LogTraceGame
#include "TraceSettings.h"
#include "UI/Text/TraceGameText.h"   // TRACE_TEXT — the difficulty's words are Ranen's

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
	return DisplayName(Difficulty);
}

const FString& TraceDifficulty::DisplayName(ETraceBotDifficulty Difficulty)
{
	switch (Difficulty)
	{
	case ETraceBotDifficulty::Easy: return TRACE_TEXT("MENU.DIFFICULTY_EASY", "EASY");
	case ETraceBotDifficulty::Hard: return TRACE_TEXT("MENU.DIFFICULTY_HARD", "HARD");
	default:                        return TRACE_TEXT("MENU.DIFFICULTY_NORMAL", "NORMAL");
	}
}

FString TraceDifficulty::ToUrlValue(ETraceBotDifficulty Difficulty)
{
	// Fixed tokens, never the display name: FromUrlValue parses exactly these three, and the display
	// name is editable text.
	switch (Difficulty)
	{
	case ETraceBotDifficulty::Easy: return TEXT("easy");
	case ETraceBotDifficulty::Hard: return TEXT("hard");
	default:                        return TEXT("normal");
	}
}

FString TraceDifficulty::ToBlurb(ETraceBotDifficulty Difficulty)
{
	switch (Difficulty)
	{
	case ETraceBotDifficulty::Easy: return TRACE_TEXT("MENU.BLURB_DIFFICULTY_EASY", "BOTS REACT SLOWLY AND SHOOT LOOSELY.");
	case ETraceBotDifficulty::Hard: return TRACE_TEXT("MENU.BLURB_DIFFICULTY_HARD", "BOTS REACT FAST, AIM TIGHT AND PUSH THE CORE.");
	default:                        return TRACE_TEXT("MENU.BLURB_DIFFICULTY_NORMAL", "BOTS PLAY A FAIR FIGHT.");
	}
}

// Named, not anonymous: this module is a unity build (Scripts/check-jumbo-build-collisions.py).
namespace TraceDifficultyFile
{
	/** GameUserSettings.ini — per machine, outside source control. See TraceCharacters below. */
	static const TCHAR* const ConfigSection = TEXT("/Script/Trace.TraceDifficulty");
	static const TCHAR* const ConfigKey = TEXT("Difficulty");
}

ETraceBotDifficulty TraceDifficulty::GetSavedSetting()
{
	FString Saved;
	if (GConfig != nullptr
		&& GConfig->GetString(TraceDifficultyFile::ConfigSection, TraceDifficultyFile::ConfigKey, Saved, GGameUserSettingsIni)
		&& !Saved.IsEmpty())
	{
		return FromUrlValue(Saved);
	}
	return Default;
}

void TraceDifficulty::SetSavedSetting(ETraceBotDifficulty Difficulty)
{
	if (GConfig == nullptr)
	{
		return;
	}

	FString Saved;
	const bool bHadOne = GConfig->GetString(TraceDifficultyFile::ConfigSection, TraceDifficultyFile::ConfigKey,
		Saved, GGameUserSettingsIni);
	const FString Token = ToUrlValue(Difficulty);
	if (bHadOne && Saved == Token)
	{
		return;
	}

	GConfig->SetString(TraceDifficultyFile::ConfigSection, TraceDifficultyFile::ConfigKey, *Token, GGameUserSettingsIni);

	// Flushed now, for the reason TraceCharacters::SetEnabledSetting gives: this build is usually
	// closed with pkill, and a choice that only survives a clean exit does not survive.
	GConfig->Flush(/*bRead=*/false, GGameUserSettingsIni);
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
// Characters on / off (spec v14 §3)
// ---------------------------------------------------------------------------------------------

namespace
{
	/**
	 * GameUserSettings.ini, not DefaultGame.ini. See the header: this is a per-machine player choice
	 * and must never end up in a repo diff. GGameUserSettingsIni resolves to
	 *     <Project>/Saved/Config/<Platform>/GameUserSettings.ini
	 * which is writable, per-user and outside source control — the same file the video settings use.
	 */
	const TCHAR* CharactersConfigSection = TEXT("/Script/Trace.TraceCharacters");
	const TCHAR* CharactersConfigKey     = TEXT("bCharactersEnabled");
}

bool TraceCharacters::HasSavedSetting()
{
	bool bSaved = false;
	return (GConfig != nullptr)
		&& GConfig->GetBool(CharactersConfigSection, CharactersConfigKey, bSaved, GGameUserSettingsIni);
}

bool TraceCharacters::GetEnabledSetting()
{
	// Read live rather than cached. The row is drawn every frame the settings page is open, and a
	// cache here would be a second copy of a value that already exists twice — which is exactly how
	// the scoring-mode row nearly ended up showing something the match was not playing.
	//
	// The CDO is the fallback rather than a hardcoded `true`, so a project that ships characters off
	// in Config/DefaultGame.ini sees a row that says OFF on a machine that has never touched it.
	bool bEnabled = UTraceSettings::Get().bCharactersEnabled;

	if (GConfig != nullptr)
	{
		GConfig->GetBool(CharactersConfigSection, CharactersConfigKey, bEnabled, GGameUserSettingsIni);
	}

	return bEnabled;
}

void TraceCharacters::SetEnabledSetting(bool bEnabled)
{
	UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>();
	const bool bAlreadyThere = (GetEnabledSetting() == bEnabled)
		&& (MutableSettings == nullptr || MutableSettings->bCharactersEnabled == bEnabled);

	if (bAlreadyThere)
	{
		return;
	}

	// 1. THE CDO, so it takes effect. This is the value UTraceAbilityComponent::AreCharactersEnabled
	//    reads, and writing it here is what makes flipping the row on the title screen change the
	//    match that is about to start rather than only the one after it.
	if (MutableSettings != nullptr)
	{
		MutableSettings->bCharactersEnabled = bEnabled;
	}

	// 2. GameUserSettings.ini, so it survives a restart WITHOUT writing to the repo-tracked
	//    Config/DefaultGame.ini that UTraceSettings otherwise persists to.
	if (GConfig != nullptr)
	{
		GConfig->SetBool(CharactersConfigSection, CharactersConfigKey, bEnabled, GGameUserSettingsIni);

		// Flushed immediately, for the reason UTraceUserSettings::Save() gives: the way this build is
		// usually closed is pkill, and a setting that only survives a clean exit does not survive.
		GConfig->Flush(/*bRead=*/false, GGameUserSettingsIni);
	}

	UE_LOG(LogTraceGame, Log, TEXT("Characters %s for matches hosted from this machine."),
		bEnabled ? TEXT("ENABLED") : TEXT("DISABLED"));
}
