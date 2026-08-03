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
	/** Default for a fresh title screen. The bots out-shoot a human at Normal; Easy is the way in. */
	inline constexpr ETraceBotDifficulty Default = ETraceBotDifficulty::Easy;

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
