// Trace — what an ABILITY is called, and what it does.
//
// KEYED BY ABILITY, NOT BY (kit, slot). It used to take a character and a slot, because that pair
// identified an ability — one kit had one ability per slot. Demo 35 ended that: Chut has two
// passives, Lily two movement abilities, so the pair no longer names anything on its own. The
// ability id is the key now, which is also what makes a rename or a category move a one-line edit
// in the ability table rather than a hunt.
//
// NAMES COME FROM THE ABILITY TABLE. Demo 35 named most of them — JET BOOTS, STICKY GLOVES,
// X-MECHS — and deliberately left three blank: Mace's magnet, Mortimer's dash/throw passive (marked
// TBD in the note), and Oyster's new dash cloak. An empty name is a supported state and the loadout
// card falls back to the description, which is what every unnamed ability did before any of them
// had names.
//
// DESCRIPTIONS STILL COME FROM THE ROSTER, so retuning or rewording an ability is one edit and every
// screen follows. The roster is keyed by CHARACTER, which is still correct: it is where the kit's
// prose lives, and the ability table says which kit and which slot to read.

#pragma once

#include "CoreMinimal.h"

#include "Abilities/TraceAbilityTypes.h"

namespace TraceAbilityNames
{
	/** The ability's display name, or empty for one the owner has not named. */
	TRACE_API FString Get(ETraceAbilityId Id);

	/** The ability's one-line description, from the roster. */
	TRACE_API FString Describe(ETraceAbilityId Id);

	/**
	 * The shortest honest label — the name where there is one, otherwise the opening words of what it
	 * does, trimmed on a word boundary. Exists so the loadout tabs and the menu's saved-loadout rows
	 * cannot drift apart about how an unnamed ability is written.
	 */
	TRACE_API FString ShortLabel(ETraceAbilityId Id, int32 MaxChars = 34);
}
