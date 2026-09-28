#include "UI/TraceAbilityNames.h"

#include "Core/TraceCharacterRoster.h"
#include "TraceSettings.h"              // the dash cloak's tuned duration
#include "UI/Text/TraceGameText.h"

namespace TraceAbilityNames
{
FString Get(ETraceAbilityId Id)
{
	const FTraceAbilityDef* Def = TraceAbilityTable::Find(Id);
	return (Def != nullptr) ? FString(Def->Name) : FString();
}

FString Describe(ETraceAbilityId Id)
{
	// STRAIGHT FROM THE ROSTER, not a second copy: the same prose the character screen always drew,
	// so retuning or rewording an ability moves one line and every screen follows.
	//
	// The ability table says which kit's row to read and which of its three fields — which is the
	// whole reason a category move (Demo 35 sent BASH from movement to passive) needs no change here.
	const FTraceAbilityDef* Def = TraceAbilityTable::Find(Id);
	if (Def == nullptr)
	{
		return FString();
	}

	const TraceCharacterRoster::FTraceCharacterEntry* Entry =
		TraceCharacterRoster::Find(static_cast<uint8>(Def->Kit));
	if (Entry == nullptr)
	{
		return FString();
	}

	// WHICH FIELD OF THE ROSTER ROW, and it is the ability's ORIGINAL field rather than its current
	// slot. Demo 35 moved two abilities between categories and their prose did not move with them:
	// Chut's bash is still written on his MOVEMENT line and Lily's wall jumps on her PASSIVE line.
	// Reading by the ability's new slot would have printed the wrong sentence for both.
	switch (Id)
	{
	case ETraceAbilityId::Bash:       return FString(Entry->Movement);   // now a passive
	case ETraceAbilityId::Acrobatics: return FString(Entry->Passive);    // now a movement ability
	case ETraceAbilityId::DashCloak:
		// OYSTER HAS TWO PASSIVES NOW and the roster row has one passive line, PICKLE JAR's — so the
		// unnamed dash cloak's card, which is nothing BUT its description, printed PICKLE JAR's rules a
		// second time. Demo 35's own line instead, with the duration read from the tuning so the card
		// cannot drift from the ability (the Demo 21 rule).
		return TRACE_TEXTF("LOADOUT.DESC_DASH_CLOAK", "JUMPING DIRECTLY FOLLOWING A DASH CLOAKS YOU FOR {0}S.",
			{ FString::Printf(TEXT("%.3g"), UTraceSettings::Get().OysterDashCloakDurationSeconds) });
	default:
		break;
	}

	switch (Def->Slot)
	{
	case ETraceLoadoutSlot::Movement:  return FString(Entry->Movement);
	case ETraceLoadoutSlot::Passive:   return FString(Entry->Passive);
	case ETraceLoadoutSlot::Activated: return FString(Entry->Activated);
	default:                           return FString();
	}
}

FString ShortLabel(ETraceAbilityId Id, int32 MaxChars)
{
	if (Id == ETraceAbilityId::None)
	{
		return FString();
	}

	const FString Name = Get(Id);
	if (!Name.IsEmpty())
	{
		return Name;
	}

	FString Body = Describe(Id);
	if (Body.Len() <= MaxChars)
	{
		return Body;
	}

	// Cut on a word boundary and end with two dots. NOT an ellipsis: the licensed sheets have no
	// U+2026, the same reason the scoreboard gives for its own truncation.
	int32 Space = INDEX_NONE;
	Body.Left(MaxChars).FindLastChar(TEXT(' '), Space);
	return Body.Left(Space > 8 ? Space : MaxChars).TrimEnd() + TEXT("..");
}
}   // namespace TraceAbilityNames
