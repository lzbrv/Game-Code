// Trace — ROCCO. See the header for the spec v14 §6 reading.

#include "Abilities/Characters/TraceAbilitySetRocco.h"

#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/Characters/TraceRippleActor.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"                  // spec v26 §9 - the second jump IS a jump, and was silent
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE RED ARMS.
//
// Each of these removes one of Rocco's three abilities and NOTHING else. They exist so
// Trace.Rocco.Verify can be made to FAIL on a build that is otherwise identical — a harness whose
// red arm cannot be made to reproduce is a harness whose green means nothing, which is exactly how
// the wall-clip bug survived two passes in this project.
//
// Cheat-flagged, default ON (the shipped behaviour), and never read anywhere but the one function
// each of them arms.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarRoccoRippleEnabled(
	TEXT("Trace.Rocco.RippleEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = pressing E spawns the Ripple. 0 = it charges the cooldown and spawns "
	     "nothing, so every ride assertion in Trace.Rocco.Verify must go red."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarRoccoSecondJumpEnabled(
	TEXT("Trace.Rocco.SecondJumpEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = the midair second jump redirects. 0 = the hook declines the jump, so the "
	     "redirect assertion must go red."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarRoccoStackCapEnabled(
	TEXT("Trace.Rocco.StackCapEnabled"),
	1,
	TEXT("Dev/red arm. 1 (default) = the headshot stack is capped at RoccoHeadshotSpeedStackMax (spec §6's "
	     "[ASSUMPTION]). 0 = uncapped, so the cap assertion must go red."),
	ECVF_Cheat);

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetRocco::OnUnequipped()
{
	DestroyActiveRipple();
}

void UTraceAbilitySetRocco::OnPawnSpawned()
{
	// A fresh pawn is standing on the floor with its extra jump in hand. The cooldown is NOT touched
	// — spec §5 is explicit that a player can spawn with an ability timer still counting down, and
	// the framework owns that timer anyway.
	bSecondJumpUsed = false;
}

void UTraceAbilitySetRocco::OnPawnDied()
{
	bSecondJumpUsed = false;

	// [ASSUMPTION] THE RIPPLE OUTLIVES ROCCO. §6 says it "lasts 4 s"; it says nothing about its
	// author. A path that vanished the instant its owner was shot would make the ability strictly
	// worse for the team it was laid for, and the choke point already answers entry without needing
	// Rocco to be alive (the instigator is his PlayerState, which survives).
}

void UTraceAbilitySetRocco::OnHalfTime()
{
	// The framework has already cleared the cooldown and Reset() the net state. All that is left is
	// the world actor, which is exactly what this hook is documented to be for.
	DestroyActiveRipple();
	bSecondJumpUsed = false;
}

void UTraceAbilitySetRocco::DestroyActiveRipple()
{
	if (ATraceRippleActor* Ripple = ActiveRipple.Get())
	{
		Ripple->Destroy();
	}
	ActiveRipple = nullptr;
}

void UTraceAbilitySetRocco::TickAbilities(float DeltaSeconds)
{
	// --- every machine: the extra jump comes back when the feet touch the floor --------------------
	// Landing is the one event both ends see without a message, which is why the flag is local and
	// cleared here rather than replicated.
	if (const UTraceCharacterMovementComponent* Move = GetMovement())
	{
		if (Move->IsMovingOnGround())
		{
			bSecondJumpUsed = false;
		}
	}

	if (!HasAuthority())
	{
		return;
	}

	const float Now = MatchTimeNow();
	const FTraceAbilityNetState& Current = State();

	// --- the ONE shared stack window closing ------------------------------------------------------
	// Edge-triggered: the whole stack drops together, which is the difference between §6's "each kill
	// extends the timer on the entire boost" and a per-stack decay nobody asked for.
	if ((Current.Flags & TraceAbilityFlags::EffectActive) != 0 && Now >= Current.EffectEndMatchTime)
	{
		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::EffectActive);
		Writable.Stacks = 0;
		Writable.EffectEndMatchTime = 0.f;
		MarkStateDirty();
	}

	// --- the ripple's bookkeeping bit -------------------------------------------------------------
	if ((Current.Flags & TraceAbilityFlags::AuxActive) != 0
		&& (!ActiveRipple.IsValid() || Now >= Current.AuxEndMatchTime))
	{
		ActiveRipple = nullptr;
		FTraceAbilityNetState& Writable = MutableState();
		Writable.Flags &= static_cast<uint8>(~TraceAbilityFlags::AuxActive);
		Writable.AuxEndMatchTime = 0.f;
		MarkStateDirty();
	}
}

// =================================================================================================
// ACTIVATED — the Ripple
// =================================================================================================

float UTraceAbilitySetRocco::GetActivatedCooldownSeconds() const
{
	// §6: "20 s cooldown", and it is a DIFFERENT NUMBER IN A DIFFERENT PLACE from the movement
	// component's dash charge pool — which is the whole of "a separate cooldown from the standard
	// dash". Firing this spends no dash charge and never touches DashTimeRemaining.
	return UTraceSettings::Get().RoccoRippleCooldownSeconds;
}

bool UTraceAbilitySetRocco::ComputeRipplePath(FVector& OutStart, FVector& OutDirection, float& OutLength) const
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* Move = GetMovement();
	if (MyPawn == nullptr || Move == nullptr)
	{
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// "DASH IN ANY DIRECTION" — composed by the SAME pure function the standard dash uses, so the
	// Ripple's direction rules are the dash's direction rules by construction rather than by a copy
	// that would drift. W/S carry the aim pitch, which is what makes straight up and diagonals work.
	OutDirection = Move->ComputeDashDirection(Move->GetCurrentAcceleration(), MyPawn->GetControlRotation());
	OutStart = MyPawn->GetActorLocation();
	OutLength = (Settings.DashSpeed * Settings.RoccoRippleDashSpeedMultiplier)
	          * (Settings.DashDuration * Settings.RoccoRippleDashDurationMultiplier);

	return !OutDirection.IsNearlyZero() && OutLength > 0.f;
}

bool UTraceAbilitySetRocco::ActivateAbility()
{
	FVector Start = FVector::ZeroVector;
	FVector Direction = FVector::ZeroVector;
	float Length = 0.f;
	if (!ComputeRipplePath(Start, Direction, Length))
	{
		return false;   // a fizzle: no pawn, no movement component. Do not charge for it.
	}

	if (!HasAuthority())
	{
		// PREDICTED HALF. There is deliberately nothing here yet. The ride is driven by the
		// REPLICATED ripple actor, which each client already simulates for its own pawn, so the only
		// thing a local spawn would buy is the ~1 RTT before the actor arrives — at the cost of two
		// ripples briefly existing on one machine and both writing Velocity. That trade is called
		// out in the report as a known, measurable gap rather than papered over.
		return true;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	if (CVarRoccoRippleEnabled.GetValueOnAnyThread() == 0)
	{
		// RED ARM: charge the cooldown, lay no path. Everything else about the two arms is identical.
		return true;
	}

	UWorld* WorldPtr = GetWorld();
	ATraceCharacter* MyPawn = GetCharacter();
	if (WorldPtr == nullptr || MyPawn == nullptr)
	{
		return false;
	}

	// ONE RIPPLE PER ROCCO. Firing again replaces the old path rather than stacking two, which keeps
	// "lasts 4 s" a statement about a thing rather than about a set of things.
	DestroyActiveRipple();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Instigator = MyPawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	ATraceRippleActor* Ripple = WorldPtr->SpawnActor<ATraceRippleActor>(
		ATraceRippleActor::StaticClass(), Start, FRotator::ZeroRotator, SpawnParams);
	if (Ripple == nullptr)
	{
		return false;
	}

	const float RideSpeed = Settings.DashSpeed * Settings.RoccoRippleRideSpeedMultiplier;
	const float Expiry = MatchTimeNow() + Settings.RoccoRippleLifetimeSeconds;

	APlayerState* SourceState = (GetAbilityComponent() != nullptr)
		? GetAbilityComponent()->GetOwningPlayerState()
		: nullptr;

	Ripple->InitialiseRipple(SourceState, Start, Direction, Length, RideSpeed,
		Settings.RoccoRippleEntryRadiusUU, Expiry);

	ActiveRipple = Ripple;

	// SPEC v29 §1f — RoccoRipple. The third WAV with no stated trigger, and the name settles it:
	// "Rocco's Ripple ability firing". This is the moment it fires — authority only (the early return
	// above means a client never reaches here), once per cast, after the path actually exists so a
	// fizzle is silent.
	//
	// GAME-SIDE, and that is the judgement §1f asks for. A Ripple is a thing OTHER players ride and
	// other players have to react to; it belongs in the same class as Dash and Parry, which spec v26
	// §9 already made game-side. At the START RING rather than at Rocco, because the start ring is
	// where the path begins and where a team-mate has to go to get on it.
	TraceAudio::PlayAt(MyPawn, TraceSoundEvents::RoccoRipple, Start);

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Flags |= TraceAbilityFlags::AuxActive;
	Writable.AuxEndMatchTime = Expiry;
	Writable.AuxLocation = Start;
	Writable.AuxDirection = Direction;
	MarkStateDirty();

	UE_LOG(LogTraceGame, Log,
		TEXT("[Rocco] RIPPLE laid by %s: start (%s) dir (%s) length %.0f uu, ride %.0f uu/s, entry radius %.0f uu, "
		     "expires at match time %.2f (%.1fs). Cooldown %.0fs (separate from the dash pool)."),
		*GetNameSafe(SourceState), *Start.ToCompactString(), *Direction.ToCompactString(), Length, RideSpeed,
		Settings.RoccoRippleEntryRadiusUU, Expiry, Settings.RoccoRippleLifetimeSeconds,
		Settings.RoccoRippleCooldownSeconds);

	return true;
}

// =================================================================================================
// MOVEMENT — "a very small second jump, which allows Rocco to change direction midair, instantly"
// =================================================================================================

bool UTraceAbilitySetRocco::OnJumpPressed()
{
	if (CVarRoccoSecondJumpEnabled.GetValueOnAnyThread() == 0)
	{
		return false;   // RED ARM: decline, and the normal jump runs as if Rocco were a Mannequin.
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* Move = GetMovement();
	if (MyPawn == nullptr || Move == nullptr || !MyPawn->IsAlive())
	{
		return false;
	}

	// ON THE GROUND THE NORMAL JUMP OWNS THE KEY. This ability is a SECOND jump; consuming the first
	// one would replace a 600 uu/s launch with a 260 uu/s one and read as the jump being broken.
	if (Move->IsMovingOnGround())
	{
		return false;
	}

	if (bSecondJumpUsed)
	{
		return false;
	}
	bSecondJumpUsed = true;

	const UTraceSettings& Settings = UTraceSettings::Get();

	const FVector VelocityBefore = Move->Velocity;
	FVector NewVelocity = VelocityBefore;

	// --- THE POINT OF THE ABILITY: the instant direction change -----------------------------------
	const FVector Wish = Move->GetCurrentAcceleration().GetSafeNormal2D();
	if (!Wish.IsNearlyZero())
	{
		// Speed is PRESERVED, not granted: the redirect turns the existing planar momentum, it does
		// not add to it. [ASSUMPTION] a floor of the ground speed limit, so that a Rocco who is
		// falling straight down with no planar speed still gets a usable change of direction —
		// without it the ability would silently do nothing in the one situation it reads as being
		// for ("change direction midair").
		const float PlanarSpeed = FMath::Max(NewVelocity.Size2D(), Move->GetMaxSpeed());
		const FVector CurrentPlanar(NewVelocity.X, NewVelocity.Y, 0.f);
		const FVector TargetPlanar = Wish * PlanarSpeed;

		const float Fraction = FMath::Clamp(Settings.RoccoSecondJumpRedirectFraction, 0.f, 1.f);
		const FVector BlendedPlanar = FMath::Lerp(CurrentPlanar, TargetPlanar, Fraction);

		NewVelocity.X = BlendedPlanar.X;
		NewVelocity.Y = BlendedPlanar.Y;
	}

	// --- "a VERY SMALL second jump" — the height is the smaller half of the ability ----------------
	// Max, not assignment: a Rocco still rising from his first jump must not be SLOWED by pressing
	// jump again. The floor is what makes it a jump; the ceiling is whatever he already had.
	NewVelocity.Z = FMath::Max(NewVelocity.Z, Settings.RoccoSecondJumpZVelocity);

	Move->Velocity = NewVelocity;

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Rocco] second jump: redirect fraction %.2f, planar (%.0f,%.0f) -> (%.0f,%.0f), Z %.0f -> %.0f."),
		Settings.RoccoSecondJumpRedirectFraction, VelocityBefore.X, VelocityBefore.Y,
		NewVelocity.X, NewVelocity.Y, VelocityBefore.Z, NewVelocity.Z);

	// SPEC v26 §9 — Jump, client-side, and it has to be HERE because of the line below.
	//
	// FOUND BY THE INTEGRATION HARNESS, not by reading. Returning true consumes the press, so
	// ACharacter::Jump is never called and UTraceCharacterMovementComponent::DoJump — where the Jump
	// sound lives for everybody else — never runs. Rocco's second jump was therefore the one jump in
	// the game that made no sound at all: the player presses the jump key, leaves the ground, and
	// hears nothing. (The same press next to a wall also becomes a second jump rather than a wall
	// jump, which is why the harness picks MACE for the wall-jump step.)
	//
	// Exactly one play, by construction: DoJump is unreachable on this path, so this cannot double
	// with the movement layer's call. Same client-side gate as every other Jump — only the machine
	// whose player pressed the key hears it.
	TraceAudio::Play(MyPawn, TraceSoundEvents::Jump);

	// TRUE CONSUMES THE JUMP. The normal jump must not also run — that would be a double launch.
	return true;
}

// =================================================================================================
// PASSIVE — "3% speed boost from headshot kills for 1 second, stacking, each kill extends the timer
//            on the entire boost"
// =================================================================================================

void UTraceAbilitySetRocco::OnKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot)
{
	if (!HasAuthority() || !bHeadshot)
	{
		// HEADSHOT KILLS ONLY. A body-shot kill, a knife kill and a trace kill all leave the stack
		// exactly where it was — including its timer, which is not refreshed either.
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// Spec §6 [ASSUMPTION], and the spec asks for it: "cap the stack (say 10 = +30%) — unbounded is
	// a bug waiting to happen; make the cap a knob."
	const int32 Cap = (CVarRoccoStackCapEnabled.GetValueOnAnyThread() != 0)
		? FMath::Max(1, Settings.RoccoHeadshotSpeedStackMax)
		: 255;                                   // RED ARM: uncapped, bounded only by the uint8

	FTraceAbilityNetState& Writable = MutableState();
	Writable.Stacks = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Writable.Stacks) + 1, 0, FMath::Min(Cap, 255)));

	// *** ONE TIMER OVER THE WHOLE STACK. *** "each kill extends the timer on the entire boost" —
	// so this is an assignment, not a per-stack timer and not an addition. Absolute match-clock
	// time, like every other timer in the framework.
	Writable.EffectEndMatchTime = MatchTimeNow() + FMath::Max(0.f, Settings.RoccoHeadshotSpeedDurationSeconds);
	Writable.Flags |= TraceAbilityFlags::EffectActive;
	MarkStateDirty();

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Rocco] headshot kill on %s (%s): stack %d (cap %d) = +%.0f%% speed, whole-stack window refreshed "
		     "to %.1fs."),
		*GetNameSafe(Victim), *Cause.ToString(), Writable.Stacks, Cap,
		Writable.Stacks * Settings.RoccoHeadshotSpeedBonusPerStack * 100.f,
		Settings.RoccoHeadshotSpeedDurationSeconds);
}

int32 UTraceAbilitySetRocco::GetLiveStackCount() const
{
	const FTraceAbilityNetState& Current = State();
	if ((Current.Flags & TraceAbilityFlags::EffectActive) == 0)
	{
		return 0;
	}
	return (MatchTimeNow() < Current.EffectEndMatchTime) ? static_cast<int32>(Current.Stacks) : 0;
}

float UTraceAbilitySetRocco::GetStackSecondsRemaining() const
{
	const FTraceAbilityNetState& Current = State();
	if ((Current.Flags & TraceAbilityFlags::EffectActive) == 0)
	{
		return 0.f;
	}
	return FMath::Max(0.f, Current.EffectEndMatchTime - MatchTimeNow());
}

float UTraceAbilitySetRocco::GetMoveSpeedMultiplier() const
{
	// Read from the REPLICATED state, so a client's own movement prediction and the server compute
	// the same multiplier from the same numbers. Cheap and pure — this is called every movement tick.
	const int32 Stacks = GetLiveStackCount();
	if (Stacks <= 0)
	{
		return 1.f;
	}
	return 1.f + (static_cast<float>(Stacks) * UTraceSettings::Get().RoccoHeadshotSpeedBonusPerStack);
}
