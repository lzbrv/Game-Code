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

	// RELEASE FX/AUDIO PLAN §5.1 — the synthesized palette. Stems live in Art/Sounds/Combat/,
	// Abilities/, UI/ and Music/; the folder is filing, not naming, exactly as with the footsteps.
	const FName MeleeSwing(TEXT("MeleeSwing"));
	const FName MeleeHit(TEXT("MeleeHit"));
	const FName MeleeBackstab(TEXT("MeleeBackstab"));
	const FName Reload(TEXT("Reload"));
	const FName DryFire(TEXT("DryFire"));
	const FName WeaponSwitch(TEXT("WeaponSwitch"));
	const FName DamageTaken(TEXT("DamageTaken"));
	const FName DeathBurst(TEXT("DeathBurst"));
	const FName Respawn(TEXT("Respawn"));
	const FName ShieldBlock(TEXT("ShieldBlock"));
	const FName ChutBash(TEXT("ChutBash"));
	const FName MaceSpikeThrow(TEXT("MaceSpikeThrow"));
	const FName MaceSpikeEmbed(TEXT("MaceSpikeEmbed"));
	const FName MacePullLoop(TEXT("MacePullLoop"));
	const FName OysterPickler(TEXT("OysterPickler"));
	const FName OysterJarBreak(TEXT("OysterJarBreak"));
	const FName XSting(TEXT("XSting"));
	const FName XStingLoad(TEXT("XStingLoad"));
	const FName RoxieRocketBurst(TEXT("RoxieRocketBurst"));
	const FName RoxieRocketLaunch(TEXT("RoxieRocketLaunch"));
	const FName RoxieRocketLoop(TEXT("RoxieRocketLoop"));
	const FName RoxieModded(TEXT("RoxieModded"));
	const FName ElleTeleport(TEXT("ElleTeleport"));
	const FName ElleSnap(TEXT("ElleSnap"));
	const FName ElleCloak(TEXT("ElleCloak"));
	const FName ElleDecloak(TEXT("ElleDecloak"));
	const FName SlimeballWall(TEXT("SlimeballWall"));
	const FName SlimeballStick(TEXT("SlimeballStick"));
	const FName MortimerQuake(TEXT("MortimerQuake"));
	const FName MortimerMantle(TEXT("MortimerMantle"));
	const FName LilyZip(TEXT("LilyZip"));
	const FName LilyZipLoop(TEXT("LilyZipLoop"));
	const FName RoccoRideLoop(TEXT("RoccoRideLoop"));
	const FName RoccoJump(TEXT("RoccoJump"));
	const FName UIHover(TEXT("UIHover"));
	const FName UIBack(TEXT("UIBack"));
	const FName UIDeny(TEXT("UIDeny"));
	const FName CountdownTick(TEXT("CountdownTick"));
	const FName CountdownGo(TEXT("CountdownGo"));
	const FName StingerVictory(TEXT("StingerVictory"));
	const FName StingerDefeat(TEXT("StingerDefeat"));
	const FName MusicTitle(TEXT("MusicTitle"));
	const FName AmbienceMatch(TEXT("AmbienceMatch"));

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
		// table should see nine untouched rows followed by v29's nineteen and then the release
		// palette's forty-three, not a reshuffle they have to audit line by line.
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

			// ---- RELEASE FX/AUDIO PLAN §5.1: core combat. -----------------------------------------
			// Client rows marked "(burst)" or "replicated-local" DO reach every machine — through
			// code that already runs everywhere — and are Client here precisely so a stray Play()
			// cannot double-multicast them. See the header comment.
			{ MeleeSwing,       ETraceSoundSide::World,  TEXT("a melee swing (predicted local + PlayAtExcluding)") },
			{ MeleeHit,         ETraceSoundSide::World,  TEXT("a melee hit lands, front verdict") },
			{ MeleeBackstab,    ETraceSoundSide::World,  TEXT("a melee hit lands, backstab verdict") },
			{ Reload,           ETraceSoundSide::World,  TEXT("a reload begins (predicted local + PlayAtExcluding)") },
			{ DryFire,          ETraceSoundSide::Client, TEXT("you pulled the trigger on an empty clip") },
			{ WeaponSwitch,     ETraceSoundSide::Client, TEXT("a weapon switch (replicated-local: OnRep_EquippedWeapon)") },
			{ DamageTaken,      ETraceSoundSide::Client, TEXT("YOUR health dropped (victim's machine only)") },
			{ DeathBurst,       ETraceSoundSide::World,  TEXT("a death, at the body") },
			{ Respawn,          ETraceSoundSide::Client, TEXT("you respawned (AcknowledgePossession)") },
			{ ShieldBlock,      ETraceSoundSide::Client, TEXT("your shot was shield-blocked (ClientNotifyHit)") },

			// ---- RELEASE FX/AUDIO PLAN §5.1: per-kit abilities. -----------------------------------
			{ ChutBash,         ETraceSoundSide::Client, TEXT("Chut's Bash connects (burst)") },
			{ MaceSpikeThrow,   ETraceSoundSide::World,  TEXT("Mace's spike cast is accepted") },
			{ MaceSpikeEmbed,   ETraceSoundSide::Client, TEXT("Mace's spike embeds (burst)") },
			{ MacePullLoop,     ETraceSoundSide::Client, TEXT("Mace reeling in (router loop, per machine)") },
			{ OysterPickler,    ETraceSoundSide::World,  TEXT("Oyster lobs the Pickler") },
			{ OysterJarBreak,   ETraceSoundSide::Client, TEXT("a jar breaks into a cloud (replicated-local: cloud BeginPlay)") },
			{ XSting,           ETraceSoundSide::Client, TEXT("X's sting spark (burst)") },
			{ XStingLoad,       ETraceSoundSide::World,  TEXT("X loads the sting clip") },
			{ RoxieRocketBurst, ETraceSoundSide::Client, TEXT("Roxie's rocket detonates (burst, Big attenuation)") },
			{ RoxieRocketLaunch, ETraceSoundSide::World, TEXT("Roxie's rocket leaves the tube") },
			{ RoxieRocketLoop,  ETraceSoundSide::Client, TEXT("Roxie's rocket in flight (replicated-local: rocket BeginPlay)") },
			{ RoxieModded,      ETraceSoundSide::World,  TEXT("Roxie goes MODDED") },
			{ ElleTeleport,     ETraceSoundSide::Client, TEXT("Elle teleports (burst, both mouths)") },
			{ ElleSnap,         ETraceSoundSide::Client, TEXT("an Elle gate snaps open (replicated-local: gate BeginPlay)") },
			{ ElleCloak,        ETraceSoundSide::World,  TEXT("Elle cloaks") },
			{ ElleDecloak,      ETraceSoundSide::World,  TEXT("Elle decloaks") },
			{ SlimeballWall,    ETraceSoundSide::World,  TEXT("a Slimewall goes up") },
			{ SlimeballStick,   ETraceSoundSide::World,  TEXT("somebody sticks to slime") },
			{ MortimerQuake,    ETraceSoundSide::World,  TEXT("Mortimer's Quake blast (Big attenuation)") },
			{ MortimerMantle,   ETraceSoundSide::Client, TEXT("Mortimer's mantle impulse") },
			{ LilyZip,          ETraceSoundSide::World,  TEXT("Lily's Zip fires") },
			{ LilyZipLoop,      ETraceSoundSide::Client, TEXT("Lily in flight (router loop, per machine)") },
			{ RoccoRideLoop,    ETraceSoundSide::Client, TEXT("riding Rocco's Ripple (router loop, per machine)") },
			{ RoccoJump,        ETraceSoundSide::World,  TEXT("Rocco's second jump") },

			// ---- RELEASE FX/AUDIO PLAN §5.1: UI + countdown. All Client, all 2D. ------------------
			{ UIHover,          ETraceSoundSide::Client, TEXT("a menu row gains focus") },
			{ UIBack,           ETraceSoundSide::Client, TEXT("back / cancel") },
			{ UIDeny,           ETraceSoundSide::Client, TEXT("a refusal (s7.1 toast, unbindable key)") },
			{ CountdownTick,    ETraceSoundSide::Client, TEXT("kickoff countdown, last 3 s") },
			{ CountdownGo,      ETraceSoundSide::Client, TEXT("kickoff release") },

			// ---- RELEASE FX/AUDIO PLAN §5.1: music. Family Music = the MusicVolumeScale hook. -----
			{ StingerVictory,   ETraceSoundSide::Client, TEXT("match end, your team won"),
			  ETraceSoundFamily::Music },
			{ StingerDefeat,    ETraceSoundSide::Client, TEXT("match end, your team lost"),
			  ETraceSoundFamily::Music },
			{ MusicTitle,       ETraceSoundSide::Client, TEXT("title-screen loop (UTraceMusicSubsystem)"),
			  ETraceSoundFamily::Music },
			{ AmbienceMatch,    ETraceSoundSide::Client, TEXT("in-match ambience loop (UTraceMusicSubsystem)"),
			  ETraceSoundFamily::Music },
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
		case ETraceSoundFamily::Music:    return TEXT("music");
		default:                          return TEXT("-");
		}
	}
}
