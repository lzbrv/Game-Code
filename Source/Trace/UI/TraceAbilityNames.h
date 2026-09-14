// Trace — what an ABILITY is called.
//
// ONLY THE ACTIVATED ONE HAS A NAME. The roster has always carried ActivatedName — RIPPLE, CHUD,
// SPIKE, PICKLER, STING, MODDED, SNAP, SLIMEWALL, QUAKE, ZIP — and movement and passive abilities
// were drawn as prose under a heading, with no name at all. The loadout screen does the same.
//
// An earlier pass invented thirty names and renamed three of the ten real ones on top of that; it
// is reverted, and this file is now a thin read of the roster rather than a second table of names.
//
// *** THE KIT ID SURVIVES AS THE ABILITY'S SOURCE, WHICH IS WHY THIS IS A (kit, slot) PAIR. ***
// ETraceCharacterId did not become meaningless when the characters went away; it became the answer
// to "whose ability is this" — internally. NOTHING SHOWS IT TO A PLAYER: the abilities are
// freestanding and no screen prints a character name. That is what let the whole rework happen
// without an enum migration or a single asset being regenerated.

#pragma once

#include "CoreMinimal.h"

#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId, ETraceLoadoutSlot

namespace TraceAbilityNames
{
	/**
	 * The ability's display name — "RIPPLE", "CHUD", "SNAP".
	 *
	 * EMPTY FOR MOVEMENT AND PASSIVE, because those abilities have no names and never did. A caller
	 * that wants something to show for them wants Describe().
	 */
	TRACE_API FString Get(ETraceCharacterId Id, ETraceLoadoutSlot Slot);

	/**
	 * The ability's one-line description, from the roster.
	 *
	 * Reads the SAME prose the character select has always shown, rather than a second copy: the
	 * roster's Movement/Passive/Activated strings are already document-backed and already the thing
	 * the owner edits when an ability is retuned.
	 */
	TRACE_API FString Describe(ETraceCharacterId Id, ETraceLoadoutSlot Slot);

	/**
	 * The shortest honest label for an ability — for a tab, a summary row, a one-line list.
	 *
	 * The activated ability's NAME where it has one; otherwise the opening words of what it does,
	 * trimmed on a word boundary. Exists so the loadout screen's tabs and the menu's saved-loadout
	 * rows cannot drift apart about how a nameless ability is written, which they did the moment one
	 * of them started printing an empty string for every movement pick.
	 */
	TRACE_API FString ShortLabel(ETraceCharacterId Id, ETraceLoadoutSlot Slot, int32 MaxChars = 34);
}
