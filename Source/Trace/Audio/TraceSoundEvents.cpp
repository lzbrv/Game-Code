// Copyright Trace. All Rights Reserved.

#include "Audio/TraceSoundEvents.h"

namespace TraceSoundEvents
{
	// The names. These are the WAV stems in Art/Sounds/ EXACTLY — Scripts/import_sounds.py keys the
	// bank by the file stem, so a rename on disk without a rename here shows up as one missing sound
	// and one "logged once" line rather than as silence nobody can explain.
	const FName CoreTurnover(TEXT("CoreTurnover"));
	const FName Dash(TEXT("Dash"));
	const FName Parry(TEXT("Parry"));
	const FName Bodyshot(TEXT("Bodyshot"));
	const FName Headshot(TEXT("Headshot"));
	const FName CorePickup(TEXT("CorePickup"));
	const FName Jump(TEXT("Jump"));
	const FName WallJump(TEXT("WallJump"));
	const FName ButtonPress(TEXT("ButtonPress"));

	TConstArrayView<FTraceSoundEvent> All()
	{
		// Function-local static rather than a file-scope array: FName's pool is initialised lazily,
		// and a file-scope table would construct nine FNames during static init in an order the
		// linker chooses. This constructs them on first use, which is after everything is up.
		static const FTraceSoundEvent Table[] =
		{
			{ CoreTurnover, ETraceSoundSide::World,  TEXT("a turnover is registered") },
			{ Dash,         ETraceSoundSide::World,  TEXT("a dash starts") },
			{ Parry,        ETraceSoundSide::World,  TEXT("a parry fires") },
			{ Bodyshot,     ETraceSoundSide::Client, TEXT("your shot hits a body") },
			{ Headshot,     ETraceSoundSide::Client, TEXT("your shot hits a head") },
			{ CorePickup,   ETraceSoundSide::Client, TEXT("you pick up the Core") },
			{ Jump,         ETraceSoundSide::Client, TEXT("you jump") },
			{ WallJump,     ETraceSoundSide::Client, TEXT("you wall-jump") },
			{ ButtonPress,  ETraceSoundSide::Client, TEXT("a menu row is activated") },
		};
		return TConstArrayView<FTraceSoundEvent>(Table, UE_ARRAY_COUNT(Table));
	}

	ETraceSoundSide SideOf(FName Event)
	{
		for (const FTraceSoundEvent& Row : All())
		{
			if (Row.Name == Event)
			{
				return Row.Side;
			}
		}

		// THE SAFE DEFAULT, and it is the quiet one. An undeclared event reaching one machine is a
		// missing sound; an undeclared event reaching every machine is a design decision made by a
		// typo. See the header.
		return ETraceSoundSide::Client;
	}

	bool IsKnown(FName Event)
	{
		for (const FTraceSoundEvent& Row : All())
		{
			if (Row.Name == Event)
			{
				return true;
			}
		}
		return false;
	}

	const TCHAR* SideName(ETraceSoundSide Side)
	{
		return (Side == ETraceSoundSide::World) ? TEXT("game-side") : TEXT("client-side");
	}
}
