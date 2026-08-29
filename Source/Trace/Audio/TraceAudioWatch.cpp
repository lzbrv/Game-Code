// Copyright Trace. All Rights Reserved.

#include "Audio/TraceAudioWatch.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Containers/Ticker.h"          // FTSTicker — Trace.Audio.ShotAudit paces itself on real frames
#include "Engine/World.h"
#include "EngineUtils.h"                 // TActorIterator
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"   // FX_AUDIO_PLAN s6 - the client half walks the local controllers
#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"

#include "Audio/TraceAudio.h"
#include "Audio/TraceSoundBank.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Trace.h"

// Named after the file, never anonymous: this module is a unity build and
// Scripts/check-jumbo-build-collisions.py gates on exactly that.
namespace TraceAudioWatchFile
{
	/**
	 * THE A/B SWITCH FOR THE GUNSHOT OBSERVER.
	 *
	 * 1 (default) — the clip watch announces gunshots, which is how §1c/§1d/§1e ship this pass.
	 * 0           — silent. This is the red arm for Trace.Audio.GunLadder: a run at 0 must report
	 *               zero shots, which is what makes a run at 1 evidence rather than an assertion.
	 *
	 * *** IT IS NO LONGER "THE OFF SWITCH FOR WHOEVER ADDS THE DIRECT CALL SITE". *** That call site
	 * now exists (FX_AUDIO_PLAN §6, UTraceWeaponComponent::FireOnce) and the two do NOT double each
	 * other, because they are heard by disjoint sets of machines: the direct one plays only on the
	 * shooter's own machine, and this one's multicast EXCLUDES that machine. The switch that turns
	 * the pair off together is Trace.Audio.PredictedShot below; this one still silences the observer,
	 * and with it everybody except the shooter.
	 */
	static TAutoConsoleVariable<int32> CVarShotWatch(
		TEXT("Trace.Audio.ShotWatch"),
		1,
		TEXT("Spec v29 s1e. 1 = the audio system announces a gunshot when a round leaves the ")
		TEXT("authoritative clip. 0 = silent, which is Trace.Audio.GunLadder's RED ARM and is also ")
		TEXT("the switch to flip if a direct call site is ever added inside the weapon component."),
		ECVF_Cheat);

	/** The same switch for footsteps, and the red arm for Trace.Audio.Footsteps. */
	static TAutoConsoleVariable<int32> CVarFootstepWatch(
		TEXT("Trace.Audio.FootstepWatch"),
		1,
		TEXT("Spec v29 s1b. 1 = a pawn walking a stride's worth of ground plays a random footstep. ")
		TEXT("0 = silent, which is Trace.Audio.Footsteps' RED ARM."),
		ECVF_Cheat);

	/** How many entries the debug logs keep. Long enough for any burst a harness drives. */
	static constexpr int32 MaxLoggedShots = 512;
	static constexpr int32 MaxLoggedFootsteps = 512;

	/**
	 * THE IMMEDIATE-REPEAT GUARD, and Trace.Audio.Footsteps' RED ARM.
	 *
	 * 1 (default) — a step is never the same clip as the step before it (§1b: "randomize it so that
	 *               step sounds vary" — a repeat is the one outcome a listener notices).
	 * 0           — pure uniform random over all eleven, which repeats about 9% of the time. The
	 *               harness must then report repeats and FAIL, which is what makes the green run at 1
	 *               evidence that the guard is doing something rather than that repeats are rare.
	 */
	static TAutoConsoleVariable<int32> CVarFootstepRepeatGuard(
		TEXT("Trace.Audio.FootstepRepeatGuard"),
		1,
		TEXT("Spec v29 s1b. 1 = a footstep clip is never played twice in a row. 0 = pure random, ")
		TEXT("which is Trace.Audio.Footsteps' RED ARM (about 9% of draws repeat)."),
		ECVF_Cheat);

	/**
	 * Verbose per-shot logging. Default 0 since the release pass (AU4): a retail log must not carry
	 * one line per gunshot, so harness runs flip this on explicitly. The in-memory ShotLog ledger
	 * that Trace.Audio.GunLadder actually grades fills regardless of this switch — only the printed
	 * line is gated.
	 */
	static TAutoConsoleVariable<int32> CVarShotLog(
		TEXT("Trace.Audio.ShotLog"),
		0,
		TEXT("Spec v29 s1c. 1 = print one line per gunshot naming the CLIP that was chosen and the ")
		TEXT("gap since the previous shot. This is the evidence the pistol ladder is asked for."),
		ECVF_Default);

	/**
	 * *** FX_AUDIO_PLAN §6.4 — THE A/B FOR THE PREDICTED LOCAL SHOT, AND IT GATES BOTH HALVES. ***
	 *
	 * 1 (default) — the shooter's own machine plays its own gunshot, melee swing and reload the
	 *               instant it asks for them (UTraceWeaponComponent, TraceAudio::PlayPredictedLocal),
	 *               and the authority's multicast EXCLUDES that pawn's machine so nobody hears the
	 *               same round twice.
	 * 0           — the pre-§6 behaviour, exactly: no predicted copy anywhere, and the multicast goes
	 *               to everyone including the shooter, who therefore hears their own shot half a round
	 *               trip late again. This is the A/B arm code-support.md AU1 asks for.
	 *
	 * *** THE TWO HALVES MUST MOVE TOGETHER, AND THAT IS WHY THE SWITCH LIVES IN ONE PLACE. *** If the
	 * exclusion stayed armed while the prediction was off, the shooter would be the ONE person in the
	 * match who heard nothing at all — a far worse failure than the latency this replaces, and one
	 * that would look like "the gun stopped making a noise". Both readers come through
	 * TraceAudioWatch::IsPredictedShotEnabled() so there is a single definition of the state.
	 *
	 * NOT ECVF_Cheat, AND IT IS NOW THE ONLY ONE IN THIS FILE THAT IS NOT — W9-SHIPGUARD flagged
	 * Trace.Audio.ShotWatch, .FootstepWatch, .FootstepRepeatGuard and .PredictedFootstep, so "the same
	 * family as Trace.Audio.ShotWatch above it" is no longer the reason and would be a false one. THE
	 * DISCRIMINATOR IS EACH SWITCH'S OWN HELP TEXT, WHICH IS ALSO HOW W8-BATTERY FOUND THEM: those
	 * four each describe their 0 as a RED ARM, i.e. a state the game is not meant to be played in.
	 * This one does not, and must not — its 0 is a SUPPORTED fallback (the pure-observer behaviour,
	 * measured at exactly one play per shot), which is why the help text below describes it as late
	 * rather than as broken, and a supported mode has no business being cheat-gated. It still changes
	 * no gameplay fact whatsoever, and a listener must be able to flip it from an ini in any build.
	 *
	 * *** IT NO LONGER CARRIES FOOTSTEPS, AND W6 §F-1 IS WHY. *** See CVarPredictedFootstep below:
	 * the observer fallback this switch selects works for gunshots and does not work for footsteps,
	 * measured, so the two families are no longer welded together by one read.
	 */
	static TAutoConsoleVariable<int32> CVarPredictedShot(
		TEXT("Trace.Audio.PredictedShot"),
		1,
		TEXT("FX_AUDIO_PLAN s6. GUNSHOTS, MELEE SWINGS AND RELOADS ONLY - footsteps have their own ")
		TEXT("switch, Trace.Audio.PredictedFootstep, because the fallback this one selects loses ")
		TEXT("most of a remote player's own steps (W6 F-1). 1 = the shooter's own machine plays its ")
		TEXT("gunshot / melee swing / reload with no round trip, and the authority's multicast ")
		TEXT("excludes that machine. 0 = the pure-observer behaviour: no predicted copy and no ")
		TEXT("exclusion, so a remote shooter hears its own shot half a round trip late - which is ")
		TEXT("late, but is exactly ONE play, measured. Set BOTH switches to 0 for the whole ")
		TEXT("pre-s6 behaviour."),
		ECVF_Default);

	/**
	 * *** W6 §F-1 — THE FOOTSTEP HALF OF THE PREDICTION, SPLIT OFF, BECAUSE THE FALLBACK IS NOT
	 * SYMMETRIC AND THE OLD HELP TEXT SAID IT WAS. ***
	 *
	 * THE MEASUREMENT THAT FORCED THIS. W5-NETQA ran a listen host and a real client on a private
	 * port with `-LogCmds=LogTraceGame Verbose` on BOTH machines and matched the two footstep ledgers
	 * position by position, for the client's OWN pawn, over the same scripted walk:
	 *
	 *     Trace.Audio.PredictedShot 1   authority announced 16, multicast EXCLUDING that client;
	 *                                   the client played 17 of its own, one per announcement,
	 *                                   with `multicasts sent 0`.
	 *     Trace.Audio.PredictedShot 0   authority announced 16 and multicast them to EVERYBODY,
	 *                                   correctly and with no exclusion; THREE arrived. The other
	 *                                   thirteen were never heard on the machine that took the steps.
	 *
	 * 81% of a player's own gait, gone, in the arm whose help text read as a plain on/off. Gunshots
	 * in the same runs fell back correctly — one announcement, one play — so the fallback is not
	 * broken, it is RATE-LIMITED: the relay's multicast is NetMulticast+Unreliable by deliberate
	 * design (ATraceAudioRelay: "a late sound is worse than a lost one") and a walking pawn asks it
	 * for a sound five times a second where a gun asks twice in two seconds. Whatever the exact
	 * mechanism inside the driver, the shape of the result is not in doubt and it is not this file's
	 * to fix — the relay is not this tranche's to edit, and making footsteps reliable would trade
	 * the game's most frequent sound against every other channel under load.
	 *
	 * *** SO THE ARM IS SPLIT INSTEAD OF THE FALLBACK BEING PRETENDED INTO WORKING. *** Gunshots keep
	 * a fallback that demonstrably works. Footsteps keep the predicted-local path, which is not an
	 * optimisation for them but the only thing that makes a remote player's own steps audible at all
	 * — and they keep an honest red arm of their own, so the 3-of-16 stays reproducible.
	 *
	 * *** §6.4'S RULE IS NOT WEAKENED, IT IS APPLIED PER FAMILY. *** The rule is that the predicted
	 * copy and the multicast exclusion must be gated on ONE read, or the owner is the one machine
	 * that hears nothing. Both footstep halves — the local play in AnnounceFootstep and the
	 * PlayAtExcluding two lines below it — now read THIS variable, and both gunshot halves still read
	 * the one above. What is forbidden is a family reading two different switches, and no family
	 * does.
	 */
	static TAutoConsoleVariable<int32> CVarPredictedFootstep(
		TEXT("Trace.Audio.PredictedFootstep"),
		1,
		TEXT("W6 F-1. 1 (default) = a machine plays its OWN player's footsteps from its own stride ")
		TEXT("accumulator, on the frame they happen, and the authority's multicast excludes it. ")
		TEXT("0 = the observer alone, which on a remote client LOSES MOST OF THAT PLAYER'S OWN ")
		TEXT("STEPS (13 of 16 in the two-process ledger that produced this switch) because the ")
		TEXT("relay's multicast is unreliable and a gait asks it for a sound five times a second. ")
		TEXT("0 is a RED ARM, not a supported fallback. Other players' steps are unaffected by it ")
		TEXT("either way."),
		ECVF_Cheat);
}

bool TraceAudioWatch::IsPredictedShotEnabled()
{
	return TraceAudioWatchFile::CVarPredictedShot.GetValueOnAnyThread() != 0;
}

bool TraceAudioWatch::IsPredictedFootstepEnabled()
{
	return TraceAudioWatchFile::CVarPredictedFootstep.GetValueOnAnyThread() != 0;
}

bool TraceAudioWatch::IsShotLogEnabled()
{
	return TraceAudioWatchFile::CVarShotLog.GetValueOnAnyThread() != 0;
}

// =================================================================================================
// FTracePistolLadder — spec v29 §1c
// =================================================================================================

FName FTracePistolLadder::NextShot(double NowSeconds, float ResetSeconds)
{
	// *** THE WHOLE RULE, AND IT IS FOUR LINES. ***
	//
	// The gap is measured against the LAST SHOT (LastShotSeconds is written below and nowhere else),
	// so "0.3 s with no firing" means exactly that: not 0.3 s since a key came up, not 0.3 s since a
	// burst was declared over. A player holding the trigger through a reload gets the reset, because
	// no round left the gun during it — which is what the sentence in the spec says.
	const double Gap = NowSeconds - LastShotSeconds;
	const double Threshold = static_cast<double>(FMath::Max(0.f, ResetSeconds));

	// >= and not >, so a reset time of exactly 0 means "every shot is shot 1" rather than "the
	// ladder never resets". A knob at its own minimum should do the obvious thing.
	if (Gap >= Threshold)
	{
		ShotNumber = 0;
	}

	++ShotNumber;
	LastShotSeconds = NowSeconds;

	// The clamp at four lives in TraceSoundEvents::PistolShotEvent, which is also what the harness
	// calls — so the rule and the check cannot disagree about what shot 40 sounds like.
	return TraceSoundEvents::PistolShotEvent(ShotNumber);
}

void FTracePistolLadder::Reset()
{
	ShotNumber = 0;
	LastShotSeconds = -1.0e9;
}

// =================================================================================================
// FTraceFootstepPicker — spec v29 §1b
// =================================================================================================

FName FTraceFootstepPicker::Next()
{
	const int32 Count = TraceSoundEvents::FootstepCount();
	if (Count <= 0)
	{
		return NAME_None;
	}
	if (Count == 1)
	{
		// One clip cannot avoid repeating itself, and pretending otherwise by looping forever is a
		// hang. Stated rather than assumed away.
		LastIndex = 0;
		return TraceSoundEvents::FootstepAt(0);
	}

	// THE RED ARM: pure random over all eleven, repeats and all. See CVarFootstepRepeatGuard.
	if (TraceAudioWatchFile::CVarFootstepRepeatGuard.GetValueOnGameThread() == 0)
	{
		LastIndex = FMath::RandHelper(Count);
		return TraceSoundEvents::FootstepAt(LastIndex);
	}

	// A UNIFORM DRAW OVER THE OTHER TEN, done by construction rather than by rejection sampling: pick
	// in [0, Count-2] and skip over the previous index. Rejection ("draw until it differs") has the
	// same distribution but an unbounded worst case, and a loop that can in principle spin forever
	// does not belong on a game thread.
	int32 Index = FMath::RandHelper(Count - 1);
	if (LastIndex != INDEX_NONE && Index >= LastIndex)
	{
		++Index;
	}

	LastIndex = Index;
	return TraceSoundEvents::FootstepAt(Index);
}

// =================================================================================================
// UTraceAudioWatchSubsystem
// =================================================================================================

bool UTraceAudioWatchSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Game, PIE and dedicated-server worlds only — the same test UTraceAudioSubsystem makes, and for
	// the same reason: an editor preview world has no pawns to watch and no listener to hear them.
	const UWorld* OuterWorld = Cast<UWorld>(Outer);
	return OuterWorld != nullptr && OuterWorld->IsGameWorld();
}

void UTraceAudioWatchSubsystem::Deinitialize()
{
	Watched.Reset();
	ShotLog.Reset();
	FootstepLog.Reset();
	FootstepWalkers.Reset();
	Super::Deinitialize();
}

TStatId UTraceAudioWatchSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTraceAudioWatchSubsystem, STATGROUP_Tickables);
}

bool UTraceAudioWatchSubsystem::IsTickable() const
{
	// Asked every frame rather than cached: a listen server that travels is the same subsystem in a
	// new world, and a client that becomes a host mid-session must start ticking without anybody
	// remembering to tell it to.
	const UWorld* World = GetWorld();
	if (World == nullptr || !World->IsGameWorld())
	{
		return false;
	}

	// *** A CLIENT NOW TICKS, AND FOR EXACTLY ONE REASON (FX_AUDIO_PLAN §6). *** Its own player's
	// footsteps, predicted locally. It runs no clip watch, looks at no other pawn and sends no RPC,
	// so what it pays is one stride accumulator per locally-controlled player pawn — and only while
	// the footstep half is armed AND the footstep prediction is armed. With
	// Trace.Audio.PredictedFootstep 0 a client goes back to paying nothing at all, which is what
	// makes that arm a real A/B rather than a partial one.
	//
	// W6 §F-1 MOVED THIS READ off Trace.Audio.PredictedShot. It has to move with the two announce
	// sites or a client would stop accumulating while AnnounceFootstep still excluded it from the
	// multicast — the silent-walker version of §6.4's silent shooter.
	if (World->GetNetMode() == NM_Client)
	{
		return TraceAudioWatchFile::CVarFootstepWatch.GetValueOnGameThread() != 0
			&& TraceAudioWatch::IsPredictedFootstepEnabled();
	}

	// Nothing to do while both halves are off. This is what makes the red arms free rather than
	// merely quiet.
	return TraceAudioWatchFile::CVarShotWatch.GetValueOnGameThread() != 0
		|| TraceAudioWatchFile::CVarFootstepWatch.GetValueOnGameThread() != 0;
}

UTraceAudioWatchSubsystem* UTraceAudioWatchSubsystem::Get(const UObject* WorldContext)
{
	if (WorldContext == nullptr || GEngine == nullptr)
	{
		return nullptr;
	}
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	return World != nullptr ? World->GetSubsystem<UTraceAudioWatchSubsystem>() : nullptr;
}

UTraceAudioWatchSubsystem::FWatchedPawn& UTraceAudioWatchSubsystem::RecordFor(ATraceCharacter* Pawn)
{
	for (FWatchedPawn& Record : Watched)
	{
		if (Record.Pawn.Get() == Pawn)
		{
			return Record;
		}
	}

	FWatchedPawn& Fresh = Watched.AddDefaulted_GetRef();
	Fresh.Pawn = Pawn;
	return Fresh;
}

void UTraceAudioWatchSubsystem::ForgetDeadRecords()
{
	Watched.RemoveAll([](const FWatchedPawn& Record) { return !Record.Pawn.IsValid(); });
}

void UTraceAudioWatchSubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// THE CLIENT BRANCH IS A DIFFERENT JOB ON A DIFFERENT POPULATION, so it returns rather than
	// falling through: a client must never walk every pawn in the world and must never watch a clip.
	if (World->GetNetMode() == NM_Client)
	{
		if (TraceAudioWatchFile::CVarFootstepWatch.GetValueOnGameThread() != 0
			&& TraceAudioWatch::IsPredictedFootstepEnabled())
		{
			TickLocalPlayerFootsteps(World, DeltaSeconds);
		}
		return;
	}

	const bool bShots = TraceAudioWatchFile::CVarShotWatch.GetValueOnGameThread() != 0;
	const bool bSteps = TraceAudioWatchFile::CVarFootstepWatch.GetValueOnGameThread() != 0;

	// THE WORLD CLOCK, not the wall clock. The gun's own fire gate uses the world clock
	// (UTraceWeaponComponent::CanFire), so the ladder's 0.3 s and the gun's 0.1 s are measured against
	// the same thing and time dilation cannot make them disagree — which is a mistake this project
	// has made before and Trace.FireRate.Measure prints a warning about to this day.
	const double NowSeconds = World->GetTimeSeconds();

	ForgetDeadRecords();

	for (TActorIterator<ATraceCharacter> It(World); It; ++It)
	{
		ATraceCharacter* Pawn = *It;
		if (!IsValid(Pawn))
		{
			continue;
		}

		FWatchedPawn& Record = RecordFor(Pawn);

		if (bShots)
		{
			TickGunshots(Pawn, Record, NowSeconds);
		}
		if (bSteps)
		{
			TickFootsteps(Pawn, Record, DeltaSeconds);
		}
	}
}

void UTraceAudioWatchSubsystem::TickGunshots(ATraceCharacter* Pawn, FWatchedPawn& Record, double NowSeconds)
{
	const UTraceWeaponComponent* Weapon = Pawn->Weapon;
	if (Weapon == nullptr)
	{
		Record.LastClipAmmo = INDEX_NONE;
		return;
	}

	const int32 Clip = Weapon->GetClipAmmo();
	const ETraceEquippedWeapon Now = Weapon->GetEquippedWeapon();

	// ---- the two edges polling cannot see, handled before anything is announced ------------------
	//
	// FIRST SIGHT of a pawn: there is no "before", so there is no drop. Seeding and returning is what
	// stops a pawn that spawns with a part-used clip from being heard to fire the difference.
	const bool bFirstLook = (Record.LastClipAmmo == INDEX_NONE);

	// A SWAP EXCHANGES MAGAZINES (spec v28 §9: each gun keeps its own clip), so the count can fall by
	// any amount with no shot behind it. Re-seed; do not announce.
	const bool bWeaponChanged = (Now != Record.LastWeapon);

	const int32 Dropped = bFirstLook ? 0 : (Record.LastClipAmmo - Clip);

	Record.LastClipAmmo = Clip;
	Record.LastWeapon = Now;

	if (bFirstLook || bWeaponChanged || Dropped <= 0)
	{
		return;
	}

	// The knife spends no rounds, so a drop while the blade is out is not a shot by definition. It
	// should be unreachable; refusing it is a second lock rather than a comment.
	if (!TraceIsFirearm(Now))
	{
		return;
	}

	const bool bSmg = (Now == ETraceEquippedWeapon::Smg);

	for (int32 Round = 0; Round < Dropped; ++Round)
	{
		FTraceShotAudioRecord Entry;
		Entry.Shooter = GetNameSafe(Pawn);
		Entry.Time = NowSeconds;
		Entry.Weapon = Now;

		if (bSmg)
		{
			// §1d: "smg shoot 1 should play on every bullet" — no ladder, no state, no reset.
			Entry.Event = TraceSoundEvents::SmgShoot1;
			Entry.ShotNumber = 1;
			Entry.SinceLast = -1.0;

			// The ladder is a PISTOL ladder and the SMG must not advance it. Deliberately NOT reset
			// either: a player who fires three pistol rounds, swaps to the SMG for a burst and swaps
			// back within 0.3 s is a case the spec does not describe, and the reading that does the
			// least surprising thing is "the pistol's own silence is what resets the pistol". The
			// swap's pullout time is longer than 0.3 s anyway, so the ladder resets on the clock in
			// practice; this is about which rule decides it.
		}
		else
		{
			// THE DERIVED reset, never the raw knob. See UTraceAudioSettings::
			// GetPistolLadderResetSeconds: the owner's literal 0.3 s is shorter than the pistol's own
			// fire interval, so the effective window has a floor expressed as a multiple of that
			// interval. Asking for the derived value here is what stops this call site from being a
			// second, quietly-wrong copy of the rule.
			// A pawn that has never fired carries a sentinel timestamp, so the raw difference is a
			// billion seconds. Reported as "no previous shot" (-1) rather than as a number that
			// reads like a measurement, because a log line that looks like data gets quoted as data.
			const double RawGap = Record.Ladder.SecondsSince(NowSeconds);
			Entry.SinceLast = (RawGap > 1.0e6) ? -1.0 : RawGap;
			Entry.Event = Record.Ladder.NextShot(
				NowSeconds, UTraceAudioSettings::Get().GetPistolLadderResetSeconds());
			Entry.ShotNumber = Record.Ladder.GetShotNumber();
		}

		// GAME-SIDE, AT THE MUZZLE (§1e). It multicasts from the authority, so every machine hears it
		// once, attenuated by distance from the barrel — which is the "hear where a shot came from"
		// the spec asks for. GetMuzzleLocation() is the shot's own origin, not the pawn's feet, so
		// the sound and the tracer come from the same point.
		//
		// *** EXCEPT ON THE SHOOTER'S OWN MACHINE (FX_AUDIO_PLAN §6.2). *** That machine already
		// played its own copy at the instant the trigger broke, so PlayAtExcluding hands the relay
		// the shooter's pawn and every receiver that locally PLAYER-controls it skips playback —
		// the listen host's own local play included. Bots exclude nobody, because no machine
		// player-controls a bot, so a bot's shot degrades to exactly the PlayAt it was before.
		//
		// ONE READ, HERE, FOR BOTH HALVES (§6.4): with the prediction off there is no predicted copy
		// to avoid doubling, so excluding anybody would leave the shooter in silence.
		if (TraceAudioWatch::IsPredictedShotEnabled())
		{
			TraceAudio::PlayAtExcluding(Pawn, Entry.Event, Pawn->GetMuzzleLocation(), Pawn);
		}
		else
		{
			TraceAudio::PlayAt(Pawn, Entry.Event, Pawn->GetMuzzleLocation());
		}

		if (TraceAudioWatchFile::CVarShotLog.GetValueOnGameThread() != 0)
		{
			const FString Gap = (Entry.SinceLast >= 0.0)
				? FString::Printf(TEXT("%.3fs since the previous shot"), Entry.SinceLast)
				: FString(TEXT("no previous shot"));
			UE_LOG(LogTraceGame, Display,
				TEXT("[ShotAudio] %-18s %-6s shot #%d -> %-12s   (%s, reset at %.3fs)"),
				*Entry.Shooter, LexToString(Now), Entry.ShotNumber, *Entry.Event.ToString(),
				*Gap, UTraceAudioSettings::Get().GetPistolLadderResetSeconds());
		}

		if (ShotLog.Num() < TraceAudioWatchFile::MaxLoggedShots)
		{
			ShotLog.Add(MoveTemp(Entry));
		}
	}
}

void UTraceAudioWatchSubsystem::TickFootsteps(ATraceCharacter* Pawn, FWatchedPawn& Record, float DeltaSeconds)
{
	const UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
	if (Move == nullptr || !Pawn->IsAlive())
	{
		Record.StrideAccumulatorUU = 0.0;
		return;
	}

	// ONLY ON THE GROUND. A dash through the air, a wall jump and a fall all move a pawn a long way
	// and none of them is a footstep; the movement component already knows which is which.
	if (!Move->IsMovingOnGround())
	{
		return;
	}

	const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();

	// SPEED x TIME, not the change in position. A teleport — the practice range's target reset, the
	// kickoff's ResetPlayersToSpawns, a harness fixture placing a bot on the aim ray — moves a pawn
	// thousands of units in one frame with no walking behind it, and a positional accumulator would
	// pay out a burst of footsteps for it. Integrating the velocity cannot make that mistake.
	const double Speed = Move->Velocity.Size2D();
	if (Speed < static_cast<double>(FMath::Max(0.f, Settings.FootstepMinSpeedUU)))
	{
		return;
	}

	Record.StrideAccumulatorUU += Speed * static_cast<double>(FMath::Max(0.f, DeltaSeconds));

	const double Stride = static_cast<double>(FMath::Max(1.f, Settings.FootstepStrideUU));
	if (Record.StrideAccumulatorUU < Stride)
	{
		return;
	}

	// SUBTRACT rather than zero. A frame that overshoots the stride by 30 uu has already walked those
	// 30 uu and they belong to the next step; zeroing would make every step very slightly late and,
	// at a low frame rate, visibly slower than the pawn's actual gait.
	//
	// The FMod guard is for the pathological frame — a hitch, or a debug speed cheat — that covers
	// several strides at once. One step, and the remainder carried, rather than a machine-gun burst.
	Record.StrideAccumulatorUU = FMath::Fmod(Record.StrideAccumulatorUU, Stride);

	AnnounceFootstep(Pawn);
}

void UTraceAudioWatchSubsystem::TickLocalPlayerFootsteps(UWorld* World, float DeltaSeconds)
{
	if (World == nullptr)
	{
		return;
	}

	// THE PLAYER CONTROLLERS, NOT A TActorIterator OVER EVERY PAWN. A client's job here is one pawn
	// (two in a split-screen build that does not exist yet), and walking the whole arena's actor list
	// every frame to find it would cost more than the feature saves. It also makes the "my own pawn
	// only" rule structural rather than a test that could be forgotten.
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		if (PC == nullptr || !PC->IsLocalController())
		{
			continue;
		}

		ATraceCharacter* Pawn = Cast<ATraceCharacter>(PC->GetPawn());
		if (!IsValid(Pawn))
		{
			continue;
		}

		// The SAME accumulator the authority runs, on the same rule, fed by this machine's own copy
		// of the pawn's velocity. The two will not agree step for step under jitter — and they do not
		// have to, because each listener only ever hears ONE of them: this machine hears its own and
		// the multicast excludes it, exactly the ladder-agreement argument in FX_AUDIO_PLAN §6.3.
		TickFootsteps(Pawn, RecordFor(Pawn), DeltaSeconds);
	}

	// Records for pawns that have gone (a respawn hands the controller a new pawn every time) are
	// dropped on the same tick, so a long session cannot grow the array one entry per death.
	ForgetDeadRecords();
}

void UTraceAudioWatchSubsystem::AnnounceFootstep(ATraceCharacter* Pawn)
{
	if (!IsValid(Pawn))
	{
		return;
	}

	FWatchedPawn& Record = RecordFor(Pawn);
	const FName Clip = Record.Steps.Next();
	if (Clip.IsNone())
	{
		return;
	}

	// AT THE FEET, not at the actor origin, which is the capsule's CENTRE and about a metre up. A
	// footstep that comes from a walker's chest is audibly wrong the moment anybody is above or below
	// you, which in this arena is most of the time.
	FVector Where = Pawn->GetActorLocation();
	if (const UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent())
	{
		Where.Z -= Capsule->GetScaledCapsuleHalfHeight();
	}

	// GAME-SIDE (§1b): "they are a WORLD sound: other players hear yours" — heard by everyone in
	// earshot and by nobody out of it. WHICH ROUTE gets it there is the §6 pattern, and the three
	// cases below add up to exactly one play per machine per step. See AnnounceFootstep's header
	// comment for the table and the file header for why footsteps needed this at all.
	//
	// W6 §F-1: THE FOOTSTEP SWITCH, NOT THE GUNSHOT ONE. Both halves below read this one value, so
	// §6.4's "the predicted copy and the exclusion move together" still holds exactly as written —
	// it just holds per family now, because the two families do not have the same fallback. See
	// TraceAudioWatchFile::CVarPredictedFootstep for the 3-of-16 that split them.
	const bool bPredicted = TraceAudioWatch::IsPredictedFootstepEnabled();
	const bool bMinePlayer = bPredicted && TraceAudio::IsLocalPlayerActor(Pawn);
	const UWorld* StepWorld = Pawn->GetWorld();
	const bool bAuthority = (StepWorld != nullptr) && (StepWorld->GetNetMode() != NM_Client);

	if (bMinePlayer)
	{
		// MY OWN GAIT, ON THE FRAME IT HAPPENED. No RPC, no round trip, and it cannot be dropped by
		// an unreliable multicast under load — which is the failure this closes. Same PlayWorldNow,
		// same gain, same attenuation curve as everybody else's copy, so it is the same sound.
		TraceAudio::PlayPredictedLocal(Pawn, Clip, Where);
	}

	if (bAuthority)
	{
		if (bPredicted)
		{
			// Everybody but the owning machine — which is playing its own copy, either the predicted
			// one two lines up (a listen host's own pawn) or its own accumulator's (a remote client).
			// A BOT EXCLUDES NOBODY and needs no special case: the exclusion is evaluated per
			// receiver as "is this pawn a locally-controlled PLAYER here", which is false for a bot
			// on every machine in the match, so this degrades to the PlayAt below by itself.
			TraceAudio::PlayAtExcluding(Pawn, Clip, Where, Pawn);
		}
		else
		{
			// Trace.Audio.PredictedFootstep is 0. This is the pre-§6 line, intact, and it is the RED
			// ARM — not a fallback. It is correct code doing exactly what it says: it announces every
			// step and multicasts every one of them to everybody, excluding nobody. What W6 §F-1
			// measured is that on a remote client only about a fifth of them arrive, because the
			// relay's multicast is unreliable and a gait is five sounds a second. Everyone ELSE's
			// steps come down this same line in both arms and are fine; it is only the walker's own
			// copy that has a better route available, and in this arm it is not taking it.
			TraceAudio::PlayAt(Pawn, Clip, Where);
		}
	}

	// APPENDED AS A PAIR, ALWAYS. FootstepWalkers is the log's owner column and the two arrays are
	// indexed together by CountFootstepsForSince; adding to one without the other would shift every
	// later entry onto the wrong pawn, which is a worse instrument than no instrument.
	if (FootstepLog.Num() < TraceAudioWatchFile::MaxLoggedFootsteps)
	{
		FootstepLog.Add(Clip);
		FootstepWalkers.Add(Pawn);
	}

	UE_LOG(LogTraceGame, Verbose, TEXT("[Footstep] %s -> %s at %s"),
		*GetNameSafe(Pawn), *Clip.ToString(), *Where.ToCompactString());
}

int32 UTraceAudioWatchSubsystem::CountFootstepsForSince(const ATraceCharacter* Pawn, const int32 FromIndex) const
{
	if (Pawn == nullptr)
	{
		return 0;
	}

	// THE TWO ARRAYS ARE THE SAME LENGTH BY CONSTRUCTION (AnnounceFootstep appends both or neither),
	// and this still asks for the minimum rather than trusting it. A trimmed log or a future edit that
	// touched one of them would otherwise read past the end of the other, and a diagnostic that can
	// crash the game it is diagnosing is worse than the mismeasurement it was written to fix.
	const int32 End = FMath::Min(FootstepLog.Num(), FootstepWalkers.Num());
	const int32 Start = FMath::Clamp(FromIndex, 0, End);

	int32 Count = 0;
	for (int32 Index = Start; Index < End; ++Index)
	{
		if (FootstepWalkers[Index].Get() == Pawn)
		{
			++Count;
		}
	}
	return Count;
}

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Audio.ShotAudit — FX_AUDIO_PLAN §8.7's double-audio audit, MEASURED, on the machine §8.7
// names: "The listen host is the machine to test — it takes every authority path locally."
//
// THE CLAIM UNDER TEST, IN ONE SENTENCE: after §6, one trigger pull must produce exactly ONE gunshot
// on every machine — the shooter's predicted copy on its own machine, the excluded multicast
// everywhere else — and flipping Trace.Audio.PredictedShot to 0 must still produce exactly one, from
// the observer alone. The two failures it is written to catch are the two that matter and they look
// nothing alike in a log: TWO plays is the double audio a naive exclusion-less predicted copy gives,
// and ZERO is the silent shooter an exclusion left armed over a disabled prediction gives.
//
// *** IT COUNTS PLAYS THAT REACHED THE ENGINE, NOT CALLS THAT WERE MADE. ***
// UTraceAudioSubsystem::GetPlaysByEvent() is bumped inside PlayLocalNow/PlayWorldNow, i.e. after the
// side gate, the settings gate, the device test and the resolve — so an entry there means a sound was
// handed to the audio engine on THIS machine. Counting call sites instead would pass a build where
// the relay's exclusion silently dropped everything.
//
// The footstep half is the same audit for the §6 treatment given to footsteps in this file, driven
// through AnnounceFootstep so the measurement is about the ROUTING and not about walking a pawn.
// =================================================================================================
namespace TraceAudioWatchAudit
{
	FTSTicker::FDelegateHandle GTicker;
	int32 GStep = 0;
	float GWait = 0.f;
	int32 GShotsPerArm = 0;
	int32 GShotsFired = 0;
	int32 GArmedPlays = -1;      // plays counted with PredictedShot 1
	int32 GObserverPlays = -1;   // plays counted with PredictedShot 0
	int32 GFootstepPlays = -1;
	int32 GFootstepsDriven = 0;
	int32 GMulticastsAtArm = 0;
	int32 GMulticastsExcluding = 0;
	int32 GForeignShots = 0;

	/** Declared here, defined below: the contamination check the verdict refuses to print without. */
	int32 CountForeignShots(UWorld* World, const ATraceCharacter* Mine);

	UWorld* AuditWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() != nullptr && Context.World()->IsGameWorld())
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	ATraceCharacter* LocalPawn(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			if (PC != nullptr && PC->IsLocalController())
			{
				if (ATraceCharacter* Pawn = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					return Pawn;
				}
			}
		}
		return nullptr;
	}

	/** Every gunshot clip's plays on this machine, summed: PistolShoot1..4 plus SmgShoot1. */
	int32 SumShotPlays(const UTraceAudioSubsystem* Audio)
	{
		if (Audio == nullptr)
		{
			return 0;
		}
		int32 Total = 0;
		const TMap<FName, int32>& Plays = Audio->GetPlaysByEvent();
		for (int32 Shot = 1; Shot <= 4; ++Shot)
		{
			if (const int32* Found = Plays.Find(TraceSoundEvents::PistolShotEvent(Shot)))
			{
				Total += *Found;
			}
		}
		if (const int32* Found = Plays.Find(TraceSoundEvents::SmgShoot1))
		{
			Total += *Found;
		}
		return Total;
	}

	/** Every footstep clip's plays on this machine, summed. */
	int32 SumFootstepPlays(const UTraceAudioSubsystem* Audio)
	{
		if (Audio == nullptr)
		{
			return 0;
		}
		int32 Total = 0;
		const TMap<FName, int32>& Plays = Audio->GetPlaysByEvent();
		for (int32 Index = 0; Index < TraceSoundEvents::FootstepCount(); ++Index)
		{
			if (const int32* Found = Plays.Find(TraceSoundEvents::FootstepAt(Index)))
			{
				Total += *Found;
			}
		}
		return Total;
	}

	void SetPredictedShot(int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Audio.PredictedShot")))
		{
			// The string overload: the one IConsoleVariable declares virtually on every engine version
			// this project has been built against.
			Var->Set(Value != 0 ? TEXT("1") : TEXT("0"), ECVF_SetByConsole);
		}
	}

	void Finish()
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
		GTicker.Reset();
		SetPredictedShot(1);

		// CONTAMINATION FIRST, because every number below is meaningless without it. See
		// CountForeignShots: this is the observer's own ledger, not a headcount of who is armed.
		if (GForeignShots > 0)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[ShotAudit] VERDICT: REFUSED — %d round(s) were fired by OTHER pawns while this "
				     "audit was counting, and their gunshots land in the same per-event counters. Run it "
				     "on the practice range (?game=/Script/Trace.TracePracticeGameMode), where the "
				     "dummies never shoot."),
				GForeignShots);
			return;
		}

		const int32 Want = GShotsPerArm;
		const bool bArmedOk = (GArmedPlays == Want);
		const bool bObserverOk = (GObserverPlays == Want);
		const bool bStepsOk = (GFootstepPlays == GFootstepsDriven);

		UE_LOG(LogTraceGame, Display,
			TEXT("[ShotAudit] PredictedShot 1: %d shot(s) fired -> %d gunshot play(s) on this machine "
			     "(%d authority multicast(s) went out during that pass, every gunshot one of them excluding "
			     "this pawn)."),
			Want, GArmedPlays, GMulticastsExcluding);
		UE_LOG(LogTraceGame, Display,
			TEXT("[ShotAudit] PredictedShot 0: %d shot(s) fired -> %d gunshot play(s) on this machine "
			     "(the pure-observer fallback)."),
			Want, GObserverPlays);
		UE_LOG(LogTraceGame, Display,
			TEXT("[ShotAudit] Footsteps: %d announced -> %d play(s) on this machine."),
			GFootstepsDriven, GFootstepPlays);

		if (bArmedOk && bObserverOk && bStepsOk)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ShotAudit] VERDICT: PASS — one trigger pull is exactly one gunshot on this "
				     "machine with the prediction on AND off, and one footstep is exactly one play. "
				     "No double audio, no silent shooter."));
			return;
		}

		// NAMED FAILURES, because "2" and "0" mean opposite bugs and a verdict that only said FAIL
		// would send the next reader to the wrong half of the system.
		UE_LOG(LogTraceGame, Error,
			TEXT("[ShotAudit] VERDICT: FAIL — %s%s%s"),
			bArmedOk ? TEXT("") : (GArmedPlays > Want
				? TEXT("DOUBLE AUDIO with the prediction armed (the multicast is not excluding the shooter). ")
				: TEXT("SILENT SHOOTER with the prediction armed (the predicted copy did not play). ")),
			bObserverOk ? TEXT("") : (GObserverPlays > Want
				? TEXT("DOUBLE AUDIO at PredictedShot 0 (a predicted copy played anyway). ")
				: TEXT("SILENT SHOOTER at PredictedShot 0 (the exclusion did not disarm with the prediction). ")),
			bStepsOk ? TEXT("") : TEXT("Footstep count wrong (the predicted/excluded pair does not sum to one). "));
	}

	bool Tick(float Delta)
	{
		UWorld* World = AuditWorld();
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		ATraceCharacter* Pawn = LocalPawn(World);
		UTraceWeaponComponent* Weapon = (Pawn != nullptr) ? Pawn->Weapon : nullptr;

		if (World == nullptr || Audio == nullptr || Pawn == nullptr || Weapon == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[ShotAudit] VERDICT: FAIL — no local pawn with a weapon. Run this in a match, "
				     "after the character select has resolved."));
			FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
			GTicker.Reset();
			SetPredictedShot(1);
			return false;
		}

		GWait -= Delta;
		if (GWait > 0.f)
		{
			return true;
		}

		switch (GStep)
		{
		case 0:
		{
			// ARM. The play map is per-machine; the observer's ledger is reset with it so the verdict
			// can say afterwards whether anybody ELSE fired into these counts (CountForeignShots).
			if (UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World))
			{
				Watch->ResetShotLog();
			}
			SetPredictedShot(1);
			Audio->ResetPlaysByEvent();
			GMulticastsAtArm = Audio->GetCounters().MulticastsSent;
			GShotsFired = 0;
			GWait = 0.2f;
			++GStep;
			return true;
		}

		case 1:
			// One press per visit. StartFire is the trigger's own verb, so this exercises the real
			// path — CanFire, FireOnce, the predicted copy, ServerFire, the observer — and not a
			// private shortcut that could pass while the trigger was broken.
			Weapon->StartFire();
			Weapon->StopFire();
			++GShotsFired;
			GWait = 0.45f;   // comfortably past the pistol's 0.3158 s interval
			if (GShotsFired >= GShotsPerArm)
			{
				++GStep;
			}
			return true;

		case 2:
			GArmedPlays = SumShotPlays(Audio);
			GMulticastsExcluding = Audio->GetCounters().MulticastsSent - GMulticastsAtArm;

			// THE FALLBACK ARM. §6.4: with the prediction off the exclusion must disarm with it.
			SetPredictedShot(0);
			Audio->ResetPlaysByEvent();
			GShotsFired = 0;
			GWait = 0.4f;
			++GStep;
			return true;

		case 3:
			Weapon->StartFire();
			Weapon->StopFire();
			++GShotsFired;
			GWait = 0.45f;
			if (GShotsFired >= GShotsPerArm)
			{
				++GStep;
			}
			return true;

		case 4:
		{
			GObserverPlays = SumShotPlays(Audio);

			// FOOTSTEPS, THROUGH THE ANNOUNCE. Driving the announce rather than walking is the same
			// choice Trace.Audio.Footsteps makes and for the same reason: the stride accumulator is
			// not what is under test here, the ROUTING is.
			SetPredictedShot(1);
			Audio->ResetPlaysByEvent();
			GFootstepsDriven = 0;
			if (UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World))
			{
				for (int32 Index = 0; Index < 10; ++Index)
				{
					Watch->AnnounceFootstep(Pawn);
					++GFootstepsDriven;
				}
			}
			GWait = 0.3f;
			++GStep;
			return true;
		}

		default:
			GFootstepPlays = SumFootstepPlays(Audio);
			GForeignShots = CountForeignShots(World, Pawn);
			Finish();
			return false;
		}
	}

	/**
	 * How many rounds OTHER pawns actually fired during the audit, i.e. how contaminated the count is.
	 *
	 * *** THE MEASUREMENT IS PER-MACHINE AND PER-EVENT, NOT PER-PAWN, AND THAT IS A REAL LIMIT. ***
	 * UTraceAudioSubsystem::GetPlaysByEvent() answers "how many times did PistolShoot2 reach the
	 * engine here", which is exactly the right question for "did MY shot play twice" and exactly the
	 * wrong one in a room full of bots also firing pistols. The first run of this audit was taken in
	 * an eight-bot match and reported 13 plays for 4 shots — all of it real, none of it mine.
	 *
	 * *** AND THE OBVIOUS GUARD — "refuse if any other armed pawn is alive" — WAS ALSO WRONG. *** The
	 * practice range, which is the environment this audit is FOR, stands five armed dummies in front
	 * of you; they simply never pull a trigger, so their weapons contaminate nothing and a pre-flight
	 * headcount refused the one place the audit works.
	 *
	 * What is exact, in every environment, is the observer's own ledger. UTraceAudioWatchSubsystem
	 * records every round it announced with the NAME of the pawn that fired it, so "did anybody else
	 * shoot while I was counting" is a question with a real answer rather than a proxy for one. The
	 * audit resets that ledger when it arms and reads it at the verdict: nobody else in it means the
	 * per-event counts are entirely this pawn's, whatever else is standing in the arena.
	 *
	 * IT IS THEREFORE AN AUTHORITY-SIDE INSTRUMENT, and that is the right side: §8.7 names the listen
	 * host as the machine to test because it takes every authority path locally. The ledger only
	 * fills where the observer ticks.
	 */
	int32 CountForeignShots(UWorld* World, const ATraceCharacter* Mine)
	{
		const UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World);
		if (Watch == nullptr)
		{
			return 0;
		}
		const FString MyName = GetNameSafe(Mine);
		int32 Count = 0;
		for (const FTraceShotAudioRecord& Record : Watch->GetShotLog())
		{
			if (Record.Shooter != MyName)
			{
				++Count;
			}
		}
		return Count;
	}

	void Arm(int32 Shots)
	{
		if (GTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
			GTicker.Reset();
		}
		GStep = 0;
		GWait = 0.f;
		GShotsPerArm = FMath::Clamp(Shots, 1, 20);
		GShotsFired = 0;
		GArmedPlays = -1;
		GObserverPlays = -1;
		GFootstepPlays = -1;
		GFootstepsDriven = 0;
		GForeignShots = 0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ShotAudit] arming: %d shot(s) with Trace.Audio.PredictedShot 1, then %d with it 0, "
			     "then 10 footsteps. Counting plays that reached the audio engine ON THIS MACHINE."),
			GShotsPerArm, GShotsPerArm);

		GTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.f);
	}
} // namespace TraceAudioWatchAudit

// -------------------------------------------------------------------------------------------------
// Trace.Audio.EventPlays — WHICH §5.1 ROWS ACTUALLY SOUNDED IN THIS SESSION
//
// FX_AUDIO_PLAN §5.1 is a table of trigger sites, and the only honest way to show a row is wired is
// to play the match it describes and then ask what reached the audio engine. Trace.Audio.Report
// answers "does this event RESOLVE to a sound" (the bank is wired) and Trace.Audio.Probe answers
// "can the mixer play one" — neither can go red for a CALL SITE that was never added, which is
// exactly the failure a wiring pass has to rule out.
//
// This dumps UTraceAudioSubsystem::GetPlaysByEvent(): the per-event tally bumped inside
// PlayLocalNow / PlayWorldNow, i.e. after the side gate, the settings gate, the device test and the
// resolve. A row with a count is a row whose trigger fired and whose sound was handed to the engine
// ON THIS MACHINE. A row that is absent is a row that did not.
//
// It is a DUMP and not a verdict on purpose: which rows are reachable depends entirely on what the
// match did (nobody backstabbed, nobody ran dry), so a pass/fail here would be a fact about the
// match rather than about the wiring. The verdict belongs to the person reading it against the run
// they staged.
// -------------------------------------------------------------------------------------------------
static FAutoConsoleCommand GTraceAudioEventPlaysCmd(
	TEXT("Trace.Audio.EventPlays"),
	TEXT("Print every sound event that actually reached the audio engine on this machine this "
	     "session, with its play count and its declared side. The FX_AUDIO_PLAN s5.1 coverage read: "
	     "a row that is missing is a trigger site that never fired."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		UWorld* World = TraceAudioWatchAudit::AuditWorld();
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		if (Audio == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[EventPlays] no audio subsystem in this world."));
			return;
		}

		TArray<TPair<FName, int32>> Rows;
		for (const TPair<FName, int32>& Pair : Audio->GetPlaysByEvent())
		{
			Rows.Add(Pair);
		}
		Rows.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B)
		{
			return A.Key.LexicalLess(B.Key);
		});

		const FTraceAudioCounters& Tally = Audio->GetCounters();
		UE_LOG(LogTraceGame, Display,
			TEXT("[EventPlays] %d distinct event(s) reached the engine here. local=%d world=%d "
			     "multicastsSent=%d refusedNotAuthority=%d refusedNotLocalPlayer=%d missingSound=%d "
			     "noDevice=%d refusedUnwired=%d"),
			Rows.Num(), Tally.LocalPlays, Tally.WorldPlays, Tally.MulticastsSent,
			Tally.RefusedNotAuthority, Tally.RefusedNotLocalPlayer, Tally.MissingSound,
			Tally.NoAudioDevice, Tally.RefusedUnwired);
		for (const TPair<FName, int32>& Row : Rows)
		{
			const ETraceSoundSide Side = TraceSoundEvents::SideOf(Row.Key);
			UE_LOG(LogTraceGame, Display, TEXT("[EventPlays]   %-22s %5d play(s)   side=%s"),
				*Row.Key.ToString(), Row.Value,
				(Side == ETraceSoundSide::World) ? TEXT("World") : TEXT("Client"));
		}

		// DEMO 29 items 9 and 11. An event that is ABSENT from the list above means one of two very
		// different things - "its trigger never fired" or "it is switched off" - and this ledger's
		// whole job is to tell those apart. So the unwired rows are printed here, by name, whether
		// or not their trigger fired, rather than leaving the reader to notice a gap.
		const TConstArrayView<FName> UnwiredRows = TraceSoundEvents::Unwired();
		if (UnwiredRows.Num() > 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[EventPlays] %d event(s) are UNWIRED and could not have sounded here "
				     "(Trace.Audio.UnwiredEvents 0 restores them):"),
				UnwiredRows.Num());
			for (const FName& Name : UnwiredRows)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[EventPlays]   %-22s UNWIRED   %s"),
					*Name.ToString(), TraceSoundEvents::UnwiredReason(Name));
			}
		}
	}));

static FAutoConsoleCommand GTraceAudioShotAuditCmd(
	TEXT("Trace.Audio.ShotAudit"),
	TEXT("Trace.Audio.ShotAudit [shots] — FX_AUDIO_PLAN s8.7's double-audio audit for the predicted "
	     "gunshot (s6). Fires `shots` (default 4) with Trace.Audio.PredictedShot 1, then the same "
	     "number with it 0, then announces ten footsteps, and counts the plays that actually reached "
	     "the audio engine on this machine. Every arm must come out at exactly one play per event: "
	     "two is double audio, zero is a silent shooter. RUN IT ON THE PRACTICE RANGE "
	     "(?game=/Script/Trace.TracePracticeGameMode): the counters are per-event and per-machine, so "
	     "any other armed pawn's shots land in them too — the audit refuses rather than lying."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		TraceAudioWatchAudit::Arm((Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 4);
	}));
#endif // !UE_BUILD_SHIPPING
