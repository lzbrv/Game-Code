#include "UI/TraceAbilityNames.h"

#include "Core/TraceCharacterRoster.h"
#include "UI/Text/TraceGameText.h"

namespace TraceAbilityNames
{
FString Get(ETraceCharacterId Id, ETraceLoadoutSlot Slot)
{
	switch (Slot)
	{
	case ETraceLoadoutSlot::Movement:
		switch (Id)
		{
		case ETraceCharacterId::Rocco: return TRACE_TEXT("ABILITY.MOVEMENT.ROCCO", "HOP");
		case ETraceCharacterId::Chut: return TRACE_TEXT("ABILITY.MOVEMENT.CHUT", "BARGE");
		case ETraceCharacterId::Mace: return TRACE_TEXT("ABILITY.MOVEMENT.MACE", "HANG");
		case ETraceCharacterId::Oyster: return TRACE_TEXT("ABILITY.MOVEMENT.OYSTER", "POP");
		case ETraceCharacterId::X: return TRACE_TEXT("ABILITY.MOVEMENT.X", "SCENT");
		case ETraceCharacterId::Roxie: return TRACE_TEXT("ABILITY.MOVEMENT.ROXIE", "KICK");
		case ETraceCharacterId::Elle: return TRACE_TEXT("ABILITY.MOVEMENT.ELLE", "COAST");
		case ETraceCharacterId::Slimeball: return TRACE_TEXT("ABILITY.MOVEMENT.SLIMEBALL", "CLING");
		case ETraceCharacterId::Mortimer: return TRACE_TEXT("ABILITY.MOVEMENT.MORTIMER", "HAUL");
		case ETraceCharacterId::Lily: return TRACE_TEXT("ABILITY.MOVEMENT.LILY", "SPARE");
		default: return FString();
		}
	case ETraceLoadoutSlot::Passive:
		switch (Id)
		{
		case ETraceCharacterId::Rocco: return TRACE_TEXT("ABILITY.PASSIVE.ROCCO", "RUSH");
		case ETraceCharacterId::Chut: return TRACE_TEXT("ABILITY.PASSIVE.CHUT", "EDGE");
		case ETraceCharacterId::Mace: return TRACE_TEXT("ABILITY.PASSIVE.MACE", "REACH");
		case ETraceCharacterId::Oyster: return TRACE_TEXT("ABILITY.PASSIVE.OYSTER", "TRAIL");
		case ETraceCharacterId::X: return TRACE_TEXT("ABILITY.PASSIVE.X", "SWARM");
		case ETraceCharacterId::Roxie: return TRACE_TEXT("ABILITY.PASSIVE.ROXIE", "SPRING");
		case ETraceCharacterId::Elle: return TRACE_TEXT("ABILITY.PASSIVE.ELLE", "FADE");
		case ETraceCharacterId::Slimeball: return TRACE_TEXT("ABILITY.PASSIVE.SLIMEBALL", "PERCH");
		case ETraceCharacterId::Mortimer: return TRACE_TEXT("ABILITY.PASSIVE.MORTIMER", "WINDUP");
		case ETraceCharacterId::Lily: return TRACE_TEXT("ABILITY.PASSIVE.LILY", "LIGHT");
		default: return FString();
		}
	case ETraceLoadoutSlot::Activated:
		switch (Id)
		{
		case ETraceCharacterId::Rocco: return TRACE_TEXT("ABILITY.ACTIVATED.ROCCO", "RIPPLE");
		case ETraceCharacterId::Chut: return TRACE_TEXT("ABILITY.ACTIVATED.CHUT", "BRACE");
		case ETraceCharacterId::Mace: return TRACE_TEXT("ABILITY.ACTIVATED.MACE", "SPIKE");
		case ETraceCharacterId::Oyster: return TRACE_TEXT("ABILITY.ACTIVATED.OYSTER", "LOB");
		case ETraceCharacterId::X: return TRACE_TEXT("ABILITY.ACTIVATED.X", "STING");
		case ETraceCharacterId::Roxie: return TRACE_TEXT("ABILITY.ACTIVATED.ROXIE", "MODDED");
		case ETraceCharacterId::Elle: return TRACE_TEXT("ABILITY.ACTIVATED.ELLE", "SNAP");
		case ETraceCharacterId::Slimeball: return TRACE_TEXT("ABILITY.ACTIVATED.SLIMEBALL", "SCREEN");
		case ETraceCharacterId::Mortimer: return TRACE_TEXT("ABILITY.ACTIVATED.MORTIMER", "QUAKE");
		case ETraceCharacterId::Lily: return TRACE_TEXT("ABILITY.ACTIVATED.LILY", "ZIP");
		default: return FString();
		}
	default:
		return FString();
	}
}

FString Describe(ETraceCharacterId Id, ETraceLoadoutSlot Slot)
{
	// STRAIGHT FROM THE ROSTER, not a second copy. These are the exact strings the character select
	// has always drawn under a portrait, so retuning an ability moves both screens at once and the
	// owner edits one line rather than hunting for the other.
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
}   // namespace TraceAbilityNames
