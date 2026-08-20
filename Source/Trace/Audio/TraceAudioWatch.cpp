// Copyright Trace. All Rights Reserved.

#include "Audio/TraceAudioWatch.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                 // TActorIterator
#include "GameFramework/CharacterMovementComponent.h"
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
	 * 0           — silent. This is BOTH the red arm for Trace.Audio.GunLadder (a run at 0 must
	 *               report zero shots, which is what makes a run at 1 evidence rather than an
	 *               assertion) AND the off switch for whoever adds the one-line call in
	 *               UTraceWeaponComponent::ServerFire later — see the header. Two sources announcing
	 *               the same round would double every shot, so exactly one of them is live.
	 */
	static TAutoConsoleVariable<int32> CVarShotWatch(
		TEXT("Trace.Audio.ShotWatch"),
		1,
		TEXT("Spec v29 s1e. 1 = the audio system announces a gunshot when a round leaves the ")
		TEXT("authoritative clip. 0 = silent, which is Trace.Audio.GunLadder's RED ARM and is also ")
		TEXT("the switch to flip if a direct call site is ever added inside the weapon component."),
		ECVF_Default);

	/** The same switch for footsteps, and the red arm for Trace.Audio.Footsteps. */
	static TAutoConsoleVariable<int32> CVarFootstepWatch(
		TEXT("Trace.Audio.FootstepWatch"),
		1,
		TEXT("Spec v29 s1b. 1 = a pawn walking a stride's worth of ground plays a random footstep. ")
		TEXT("0 = silent, which is Trace.Audio.Footsteps' RED ARM."),
		ECVF_Default);

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
		ECVF_Default);

	/** Verbose per-shot logging, which is what proves the ladder in a headless run. */
	static TAutoConsoleVariable<int32> CVarShotLog(
		TEXT("Trace.Audio.ShotLog"),
		1,
		TEXT("Spec v29 s1c. 1 = print one line per gunshot naming the CLIP that was chosen and the ")
		TEXT("gap since the previous shot. This is the evidence the pistol ladder is asked for."),
		ECVF_Default);
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
	// AUTHORITY ONLY, and asked every frame rather than cached: a listen server that travels is the
	// same subsystem in a new world, and a client that becomes a host mid-session must start ticking
	// without anybody remembering to tell it to.
	const UWorld* World = GetWorld();
	if (World == nullptr || !World->IsGameWorld() || World->GetNetMode() == NM_Client)
	{
		return false;
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
	if (World == nullptr || World->GetNetMode() == NM_Client)
	{
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

		// GAME-SIDE, AT THE MUZZLE (§1e). PlayAt multicasts from the authority, so every machine
		// hears it once, attenuated by distance from the barrel — which is the "hear where a shot
		// came from" the spec asks for. GetMuzzleLocation() is the shot's own origin, not the pawn's
		// feet, so the sound and the tracer come from the same point.
		TraceAudio::PlayAt(Pawn, Entry.Event, Pawn->GetMuzzleLocation());

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

	// GAME-SIDE (§1b): "they are a WORLD sound: other players hear yours". PlayAt multicasts from the
	// authority, so a pawn's steps are heard by everyone in earshot and by nobody out of it.
	TraceAudio::PlayAt(Pawn, Clip, Where);

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
