// Trace — the contract between the title screen and the match.
//
// The menu and the game mode live in different maps and never share an object, so everything they
// have to agree on is written down exactly once, here:
//
//   * the two map paths they travel between;
//   * the difficulty the player picked, and the "?difficulty=" URL option that carries it across
//     the travel;
//   * how long the post-match results screen stays up, which the HUD renders as a countdown and
//     the game mode uses as the timer that actually returns to the menu.
//
// Deliberately plain C++ — no UCLASS, no UENUM, nothing reflected. This header is included by the
// game mode, so keeping UHT out of it keeps the include cheap.

#pragma once

#include "CoreMinimal.h"

#include "Core/TraceMatchTypes.h"   // ETraceScoringMode (forward declaration) + its string helpers

/** The only two levels in the project. */
namespace TraceMaps
{
	inline const TCHAR* MainMenu = TEXT("/Game/Maps/MainMenu");
	inline const TCHAR* Arena    = TEXT("/Game/Maps/Arena");
}

/**
 * How hard the bots play.
 *
 * These are applied as MULTIPLIERS over whatever Config/DefaultGame.ini says, never as absolute
 * numbers: Normal is defined to be "exactly the shipped tuning". That means whoever is tuning
 * UTraceSettings can keep tuning it without this file silently overriding their work, and the
 * three difficulties stay meaningfully spaced apart from wherever the baseline ends up.
 */
enum class ETraceBotDifficulty : uint8
{
	Easy   = 0,
	Normal = 1,
	Hard   = 2
};

namespace TraceDifficulty
{
	/**
	 * Default for a fresh title screen.
	 *
	 * Spec v9 §9, verbatim: "Set default startup bot difficulty to normal". This constant is the one
	 * that actually decides it — NOT UTraceSettings::BotDifficulty and not Config/DefaultGame.ini.
	 * The title screen seeds its DIFFICULTY row from here and then writes "?difficulty=<value>" onto
	 * the travel URL, and ATraceGameMode::InitGame applies the URL option OVER the ini. So a build
	 * with BotDifficulty=Normal in the ini and Easy here still plays every menu-started match on
	 * Easy, which is what shipped before this change. Keep all three in step; this one wins.
	 */
	inline constexpr ETraceBotDifficulty Default = ETraceBotDifficulty::Normal;

	inline constexpr int32 Count = 3;

	/** URL option carried across the travel from the menu into the arena: "?difficulty=easy". */
	inline const TCHAR* UrlOption = TEXT("difficulty");

	/** "EASY" / "NORMAL" / "HARD", for the menu and the in-game HUD. */
	TRACE_API FString ToDisplayName(ETraceBotDifficulty Difficulty);

	/** Lowercase token used in the travel URL. */
	TRACE_API FString ToUrlValue(ETraceBotDifficulty Difficulty);

	/** Parses "easy"/"normal"/"hard" or "0"/"1"/"2". Falls back to Default on anything else. */
	TRACE_API ETraceBotDifficulty FromUrlValue(const FString& Value);

	/** Clamped index arithmetic, so the menu's left/right keys cannot run off either end. */
	TRACE_API ETraceBotDifficulty Step(ETraceBotDifficulty Difficulty, int32 Delta);

	/**
	 * Rewrites the bot tuning on the UTraceSettings CDO for this session.
	 *
	 * Idempotent and order-independent: the first call snapshots the config-loaded baseline, and
	 * every call thereafter multiplies that snapshot rather than the current (already multiplied)
	 * values. Without the snapshot, travelling menu -> match -> menu -> match would compound Easy
	 * on top of Easy until the bots stood still.
	 */
	TRACE_API void ApplyToSettings(ETraceBotDifficulty Difficulty);
}

/**
 * The A/B scoring-mode toggle's half of the menu/match contract (spec v4 §7).
 *
 * Deliberately thin, and deliberately shaped exactly like TraceDifficulty above: the two things the
 * title screen chooses travel the same way, so they should be selected, applied and carried the
 * same way. Everything about NAMING the modes lives in Core/TraceMatchTypes.h, which this header
 * includes; the URL option is TraceScoringModeUrlOption ("mode") from the same place.
 */
namespace TraceScoring
{
	/**
	 * Writes @p Mode onto the UTraceSettings CDO, which is what ATraceGameMode publishes from and
	 * what the Project Settings page displays.
	 *
	 * Called by the title screen when the row changes AND again on PLAY, for the same reason
	 * TraceDifficulty::ApplyToSettings is: the travel URL is the authority for the destination map,
	 * but a standalone launch where the URL is ever dropped must still honour what the player picked.
	 *
	 * It is also what keeps the settings page honest while sitting on the title screen — the panel
	 * shows the mode the next match will actually play, rather than the last one written to ini.
	 */
	TRACE_API void ApplyToSettings(ETraceScoringMode Mode);

	/** The mode currently on the UTraceSettings CDO. The title screen's starting value. */
	TRACE_API ETraceScoringMode GetCurrentSetting();
}

namespace TraceMatchFlow
{
	/**
	 * Seconds the post-match results screen stays up before the game mode returns to the title.
	 *
	 * Shared so the HUD's "RETURNING TO MENU IN n" countdown and the timer that actually travels
	 * can never disagree — a countdown that hits zero and does nothing is exactly the kind of thing
	 * that makes a build feel broken.
	 */
	inline constexpr float PostMatchDuration = 14.f;
}
