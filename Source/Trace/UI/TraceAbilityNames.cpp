#include "UI/TraceAbilityNames.h"

#include "Core/TraceCharacterRoster.h"

namespace TraceAbilityNames
{
FString Get(ETraceCharacterId Id, ETraceLoadoutSlot Slot)
{
	// *** ONLY THE ACTIVATED ABILITY HAS A NAME, AND THAT IS NOT AN OMISSION. ***
	//
	// It is how the game has always been: the roster carries ActivatedName — RIPPLE, CHUD, SPIKE,
	// PICKLER, STING, MODDED, SNAP, SLIMEWALL, QUAKE, ZIP — and the movement and passive abilities
	// were drawn as prose under a heading on the character select, with no name at all.
	//
	// AN EARLIER PASS INVENTED THIRTY NAMES and renamed three of the ten real ones on top (CHUD ->
	// BRACE, PICKLER -> LOB, SLIMEWALL -> SCREEN). That was wrong twice over: it renamed abilities
	// players already knew by name, and it made up names for eighteen others that had never had one.
	// Reverted. The loadout screen shows movement and passive abilities the way the character screen
	// always did — by what they DO.
	//
	// Straight from the roster, so retuning or rewording an ability still moves exactly one line and
	// every screen follows.
	if (Slot != ETraceLoadoutSlot::Activated)
	{
		return FString();
	}

	const TraceCharacterRoster::FTraceCharacterEntry* Entry =
		TraceCharacterRoster::Find(static_cast<uint8>(Id));
	return (Entry != nullptr) ? FString(Entry->ActivatedName) : FString();
}

FString Describe(ETraceCharacterId Id, ETraceLoadoutSlot Slot)
{
	const TraceCharacterRoster::FTraceCharacterEntry* Entry =
		TraceCharacterRoster::Find(static_cast<uint8>(Id));
	if (Entry == nullptr)
	{
		return FString();   // None, or an id nothing implements yet. An empty slot has nothing to say.
	}

	switch (Slot)
	{
	case ETraceLoadoutSlot::Movement:  return FString(Entry->Movement);
	case ETraceLoadoutSlot::Passive:   return FString(Entry->Passive);
	case ETraceLoadoutSlot::Activated: return FString(Entry->Activated);
	default:                           return FString();
	}
}

FString ShortLabel(ETraceCharacterId Id, ETraceLoadoutSlot Slot, int32 MaxChars)
{
	if (Id == ETraceCharacterId::None)
	{
		return FString();
	}

	const FString Name = Get(Id, Slot);
	if (!Name.IsEmpty())
	{
		return Name;
	}

	FString Body = Describe(Id, Slot);
	if (Body.Len() <= MaxChars)
	{
		return Body;
	}

	// Cut on a word boundary and end with two dots. NOT an ellipsis: the licensed sheets have no
	// U+2026, which is the same reason the scoreboard gives for its own truncation.
	int32 Space = INDEX_NONE;
	Body.Left(MaxChars).FindLastChar(TEXT(' '), Space);
	return Body.Left(Space > 8 ? Space : MaxChars).TrimEnd() + TEXT("..");
}
}   // namespace TraceAbilityNames
