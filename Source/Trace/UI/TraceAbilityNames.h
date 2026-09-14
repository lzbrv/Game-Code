// Trace — what an ABILITY is called, now that abilities are the thing you pick.
//
// THE REWORK MADE THIS NECESSARY. Before it, a player picked ROCCO and the three abilities came
// along unnamed — the select screen showed them as prose under a character's portrait and nothing
// ever had to refer to one on its own. A loadout screen has to: you equip a movement ability, a
// passive and an activated one, from three different people, and "ROCCO's movement" is not a name a
// menu can put in a column.
//
// NEUTRAL NAMES, NO CHARACTERS — the owner's decision, and it is the reason three activated
// abilities were renamed rather than only the twenty new ones being invented:
//
//     CHUD      -> BRACE      named after Chut
//     PICKLER   -> LOB        named after Oyster's pickle jars
//     SLIMEWALL -> SCREEN     named after Slimeball
//
// The other seven activated names were already neutral (RIPPLE, SPIKE, STING, MODDED, SNAP, QUAKE,
// ZIP) and are untouched — a rename with no reason behind it is churn in every player's vocabulary.
//
// *** THE KIT ID SURVIVES AS THE ABILITY'S SOURCE, WHICH IS WHY THIS IS A (kit, slot) PAIR. ***
// ETraceCharacterId did not become meaningless when the characters went away; it became the answer
// to "whose ability is this". That is what let the whole rework happen without an enum migration or
// a single asset being regenerated.
//
// EVERY NAME IS A TRACE_TEXT CALL SITE, so all thirty are editable in Config/TraceGameText.ini like
// every other word in the game. That is also why this is a switch of thirty literal cases rather
// than a table walked in a loop: TRACE_TEXT's key and default must be compile-time literals, and a
// table would have bought brevity by putting these strings outside the document entirely.

#pragma once

#include "CoreMinimal.h"

#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId, ETraceLoadoutSlot

namespace TraceAbilityNames
{
	/** The ability's display name — "HOP", "PERCH", "SCREEN". Empty for a slot nobody fills. */
	TRACE_API FString Get(ETraceCharacterId Id, ETraceLoadoutSlot Slot);

	/**
	 * The ability's one-line description, from the roster.
	 *
	 * Reads the SAME prose the character select has always shown, rather than a second copy: the
	 * roster's Movement/Passive/Activated strings are already document-backed and already the thing
	 * the owner edits when an ability is retuned.
	 */
	TRACE_API FString Describe(ETraceCharacterId Id, ETraceLoadoutSlot Slot);
}
