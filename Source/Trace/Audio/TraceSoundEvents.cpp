// Copyright Trace. All Rights Reserved.

#include "Audio/TraceSoundEvents.h"

namespace TraceSoundEvents
{
	// The names. These are the WAV stems in Art/Sounds/ EXACTLY — Scripts/import_sounds.py keys the
	// bank by the file stem, so a rename on disk without a rename here shows up as one missing sound
	// and one "logged once" line rather than as silence nobody can explain.
	//
	// THE FOOTSTEPS' STEMS ARE Step1..Step11 EVEN THOUGH THEY LIVE IN Art/Sounds/Footsteps/. The
	// importer globs recursively and keys by the stem alone; the folder is filing, not naming.
	const FName CoreTurnover(TEXT("CoreTurnover"));
	const FName Dash(TEXT("Dash"));
	const FName Parry(TEXT("Parry"));
	const FName Bodyshot(TEXT("Bodyshot"));
	const FName Headshot(TEXT("Headshot"));
	const FName CorePickup(TEXT("CorePickup"));
	const FName Jump(TEXT("Jump"));
	const FName WallJump(TEXT("WallJump"));
	const FName ButtonPress(TEXT("ButtonPress"));

	// SPEC v29 §1.
	const FName Kill(TEXT("Kill"));
	const FName Goal(TEXT("Goal"));
	const FName RoccoRipple(TEXT("RoccoRipple"));
	const FName PistolShoot1(TEXT("PistolShoot1"));
	const FName PistolShoot2(TEXT("PistolShoot2"));
	const FName PistolShoot3(TEXT("PistolShoot3"));
	const FName PistolShoot4(TEXT("PistolShoot4"));
	const FName SmgShoot1(TEXT("SmgShoot1"));

	// Function-local so these are built after FName's pool is up, for the same reason All() is a
	// function-local static. Eleven names, in clip order, and the ONE place the count lives.
	static TConstArrayView<FName> Footsteps()
	{
		static const FName Table[] =
		{
			FName(TEXT("Step1")),  FName(TEXT("Step2")),  FName(TEXT("Step3")),  FName(TEXT("Step4")),
			FName(TEXT("Step5")),  FName(TEXT("Step6")),  FName(TEXT("Step7")),  FName(TEXT("Step8")),
			FName(TEXT("Step9")),  FName(TEXT("Step10")), FName(TEXT("Step11")),
		};
		return TConstArrayView<FName>(Table, UE_ARRAY_COUNT(Table));
	}

	int32 FootstepCount()
	{
		return Footsteps().Num();
	}

	FName FootstepAt(int32 Index)
	{
		const TConstArrayView<FName> Table = Footsteps();
		return Table.IsValidIndex(Index) ? Table[Index] : NAME_None;
	}

	FName PistolShotEvent(int32 ShotNumber)
	{
		// *** THE CLAMP IS THE SPEC. *** "shot 4 and every shot after -> PistolShoot4". Written as a
		// clamp rather than as a chain of ifs so that the fourth, the fortieth and the four-hundredth
		// shot are provably the same answer, and so that a harness can assert the rule by calling this
		// rather than by re-deriving it.
		switch (FMath::Clamp(ShotNumber, 1, 4))
		{
		case 1:  return PistolShoot1;
		case 2:  return PistolShoot2;
		case 3:  return PistolShoot3;
		default: return PistolShoot4;
		}
	}

	TConstArrayView<FTraceSoundEvent> All()
	{
		// Function-local static rather than a file-scope array: FName's pool is initialised lazily,
		// and a file-scope table would construct the FNames during static init in an order the linker
		// chooses. This constructs them on first use, which is after everything is up.
		//
		// APPEND, NEVER INSERT. The v26 nine are first and in the v26 order, deliberately: spec v29
		// §1a is "keep the existing client/global split", and a reader diffing this against the old
		// table should see nine untouched rows followed by nineteen new ones, not a reshuffle they
		// have to audit line by line.
		static const FTraceSoundEvent Table[] =
		{
			// ---- spec v26 §9. NOT ONE OF THESE MOVED IN v29 §1a. --------------------------------
			{ CoreTurnover, ETraceSoundSide::World,  TEXT("a turnover is registered") },
			{ Dash,         ETraceSoundSide::World,  TEXT("a dash starts") },
			{ Parry,        ETraceSoundSide::World,  TEXT("a parry fires") },
			{ Bodyshot,     ETraceSoundSide::Client, TEXT("your shot hits a body") },
			{ Headshot,     ETraceSoundSide::Client, TEXT("your shot hits a head") },
			{ CorePickup,   ETraceSoundSide::Client, TEXT("you pick up the Core") },
			{ Jump,         ETraceSoundSide::Client, TEXT("you jump") },
			{ WallJump,     ETraceSoundSide::Client, TEXT("you wall-jump") },
			{ ButtonPress,  ETraceSoundSide::Client, TEXT("a menu row is activated") },

			// ---- spec v29 §1e: gunshots are GLOBAL, at the muzzle, both weapons. -----------------
			{ PistolShoot1, ETraceSoundSide::World,  TEXT("pistol, shot 1 of a burst"),
			  ETraceSoundFamily::Gunshot },
			{ PistolShoot2, ETraceSoundSide::World,  TEXT("pistol, shot 2 of a burst"),
			  ETraceSoundFamily::Gunshot },
			{ PistolShoot3, ETraceSoundSide::World,  TEXT("pistol, shot 3 of a burst"),
			  ETraceSoundFamily::Gunshot },
			{ PistolShoot4, ETraceSoundSide::World,  TEXT("pistol, shot 4 and every shot after"),
			  ETraceSoundFamily::Gunshot },
			{ SmgShoot1,    ETraceSoundSide::World,  TEXT("SMG, every round, no ladder"),
			  ETraceSoundFamily::Gunshot },

			// ---- spec v29 §1b: footsteps. World, randomised, and much quieter. -------------------
			{ FName(TEXT("Step1")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step2")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step3")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step4")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step5")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step6")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step7")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step8")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step9")),  ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step10")), ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },
			{ FName(TEXT("Step11")), ETraceSoundSide::World, TEXT("a footstep"), ETraceSoundFamily::Footstep },

			// ---- spec v29 §1f: inferred triggers. See TraceSoundEvents.h for the reasoning. ------
			{ Goal,         ETraceSoundSide::World,  TEXT("a goal is scored (ATraceGameMode::NotifyScored)") },
			{ Kill,         ETraceSoundSide::Client, TEXT("you registered a kill (ClientNotifyHit, bKilled)") },
			{ RoccoRipple,  ETraceSoundSide::World,  TEXT("Rocco lays a Ripple (UTraceAbilitySetRocco::ActivateAbility)") },
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

	ETraceSoundFamily FamilyOf(FName Event)
	{
		for (const FTraceSoundEvent& Row : All())
		{
			if (Row.Name == Event)
			{
				return Row.Family;
			}
		}
		return ETraceSoundFamily::Default;
	}

	const TCHAR* FamilyName(ETraceSoundFamily Family)
	{
		switch (Family)
		{
		case ETraceSoundFamily::Footstep: return TEXT("footstep");
		case ETraceSoundFamily::Gunshot:  return TEXT("gunshot");
		default:                          return TEXT("-");
		}
	}
}
