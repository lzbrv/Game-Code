// Trace — the five characters, as DATA.
//
// Spec v14 §3 asks for a select screen that shows "each one's movement, passive and activated
// ability so a player can choose meaningfully". That means the prose is a shipped asset, not
// decoration, and it must live in exactly one place: the select screen draws it, the game mode logs
// from it, and the ability framework can read it rather than writing a second table of names.
//
// DELIBERATELY PLAIN C++ — no UCLASS, no UENUM, nothing reflected. Same shape and same reasoning as
// UI/TraceMatchOptions.h: this header is included by the game mode, the player state and the HUD, so
// keeping UnrealHeaderTool out of it keeps the include free.
//
// *** THE ID SPACE IS ETraceCharacterId's (spec v14 §5). ***
//     None = 0, Rocco = 1, Chut = 2, Mace = 3, Oyster = 4, X = 5
// Every id in this project — the replicated byte on ATracePlayerState, the argument to
// UTraceAbilityComponent::ServerSetCharacter, the number a console command takes — is that value.
// This file stores it as a uint8 rather than as the enum ONLY so that the character-select slice
// does not have to include the ability framework's header to draw a menu; the two are the same
// number and the static_asserts at the top of TraceCharacterRoster.cpp are where that agreement is asserted
// once the enum exists. Do not introduce a second numbering.

#pragma once

#include "CoreMinimal.h"
#include "Math/Color.h"

namespace TraceCharacterRoster
{
	/**
	 * ETraceCharacterId::None — the default characterless Mannequin.
	 *
	 * Who ends up here: mode A (frozen by spec v14 §2), every player when the "disable characters"
	 * setting is off, and anybody a full team roster cannot serve. NOT bots — this used to say "bots
	 * are always this", which spec v15 §2 reverses: bots pick like everybody else, they simply pick
	 * after the humans on their team and are assigned rather than asked.
	 */
	inline constexpr uint8 NoneId = 0;

	/** ETraceCharacterId::Rocco. */
	inline constexpr uint8 FirstId = 1;

	/** ETraceCharacterId::X. */
	inline constexpr uint8 LastId = 5;

	/** Selectable characters. NOT the size of the enum — None is not selectable. */
	inline constexpr int32 Count = 5;

	/**
	 * One character's shipped description.
	 *
	 * The three ability strings are the spec's own words, compressed to fit a card. They are ASCII
	 * only and upper case on purpose: every string in this project's UI is drawn with the engine's
	 * built-in BITMAP fonts, whose glyph pages do not cover an em dash or a curly quote — those come
	 * out as a box or as nothing at all, and that has already bitten the title screen once.
	 */
	struct FTraceCharacterEntry
	{
		/** ETraceCharacterId as a uint8. */
		uint8 Id = NoneId;

		const TCHAR* Name = TEXT("");

		/** Spec §6 "Movement:". */
		const TCHAR* Movement = TEXT("");

		/** Spec §6 "Passive:". */
		const TCHAR* Passive = TEXT("");

		/** The activated ability's own name — Ripple, Chud, Spike, Pickler, Sting. */
		const TCHAR* ActivatedName = TEXT("");

		/** Spec §6 "Activated:". This is the one bound to E. */
		const TCHAR* Activated = TEXT("");

		/** Cooldown in seconds, for the card. The ability framework owns the number that is ENFORCED. */
		float ActivatedCooldown = 20.f;

		/**
		 * Card accent. Deliberately NOT a team colour: the select screen is the one place in this
		 * game where the two teams look identical (enemies may mirror your pick), so the cards have
		 * to be told apart by character rather than by side.
		 */
		FLinearColor Accent = FLinearColor::White;
	};

	/** The table, in the doc's order: Rocco, Chut, Mace, Oyster, X. Always Count entries. */
	TRACE_API const TArray<FTraceCharacterEntry>& All();

	/** Null for NoneId and for anything out of range. */
	TRACE_API const FTraceCharacterEntry* Find(uint8 Id);

	/** True for 1..5. NoneId is deliberately NOT valid — "no character" is not a character. */
	TRACE_API bool IsValidId(uint8 Id);

	/** "ROCCO" / ... / "MANNEQUIN" for NoneId, so a log line never prints a bare number. */
	TRACE_API FString NameFor(uint8 Id);
}
