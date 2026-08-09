// Trace — MACE. See the header for the doc's wording and for why velocity is written by hand.

#include "Abilities/Characters/TraceAbilitySetMace.h"

#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceAbilityInputRelay.h"
#include "Abilities/Characters/TraceMaceSpike.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetMace::OnEquipped()
{
	bSuspendHeld = false;
	bSuspending = false;
	SuspendEndMatchTime = 0.f;
	SuspendReadyMatchTime = 0.f;
	bPulling = false;
	bPullQueued = false;
	SpikeEmbedEndMatchTime = 0.f;
	ClearSpike();

	if (HasAuthority())
	{
		// The V key has nowhere to come from until ETraceInputAction gains the action; the relay is
		// the interim path and it is attached the moment somebody actually becomes Mace.
		if (UTraceAbilityComponent* Comp = GetAbilityComponent())
		{
			UTraceAbilityInputRelay::EnsureOn(Comp->GetOwningPlayerState());
		}
		PublishState();
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Mace] Equipped. magnet x%.2f | suspend %.2fs @ %.0f uu/s | spike %.0f uu @ %.0f uu/s, wall test "
		     "|n.Z|<=%.2f, aim forgiveness %.0f uu, embed %.1fs, pull %.0f uu/s | cooldown %.0fs [ASSUMPTION: §6 unspecified]"),
		GetMagnetRadiusMultiplier(),
		UTraceSettings::Get().MaceSuspendMaxSeconds, UTraceSettings::Get().MaceSuspendLateralSpeedCap,
		UTraceSettings::Get().MaceSpikeRangeUU, UTraceSettings::Get().MaceSpikeTravelSpeed,
		UTraceSettings::Get().MaceSpikeMaxSurfaceNormalZ, UTraceSettings::Get().MaceSpikeTraceRadiusUU,
		UTraceSettings::Get().MaceSpikeEmbedSeconds,
		GetPullSpeed(), GetActivatedCooldownSeconds());
}

void UTraceAbilitySetMace::OnUnequipped()
{
	StopSuspend(TEXT("unequipped"));
	StopPull(TEXT("unequipped"), /*bRemoveSpike*/ true);
	ClearSpike();
}

void UTraceAbilitySetMace::OnPawnDied()
{
	// Cooldowns are NOT touched — spec §5 is explicit that death does not reset them. Only the world
	// state a dead pawn cannot own goes away.
	StopSuspend(TEXT("died"));
	StopPull(TEXT("died"), /*bRemoveSpike*/ true);
	ClearSpike();
}

void UTraceAbilitySetMace::OnHalfTime()
{
	// The framework has already zeroed the cooldown and Reset() the net state. All that is left is
	// the actor, which the framework knows nothing about.
	StopSuspend(TEXT("half time"));
	StopPull(TEXT("half time"), /*bRemoveSpike*/ true);
	ClearSpike();
}

bool UTraceAbilitySetMace::ShouldDriveMovement() const
{
	// A simulated proxy's velocity is replicated; writing it there would fight the interpolation and
	// would show up as somebody else's Mace stuttering.
	return HasAuthority() || IsLocallyControlled();
}

// =================================================================================================
// PASSIVE — the magnet
// =================================================================================================

float UTraceAbilitySetMace::GetMagnetRadiusMultiplier() const
{
	// DERIVED, per §6's own instruction. The 450 lives in UTraceSettings::CoreCatchRadius and the
	// 0.30 lives in MaceMagnetRadiusBonus; 585 is written down nowhere.
	return 1.f + FMath::Max(0.f, UTraceSettings::Get().MaceMagnetRadiusBonus);
}

// =================================================================================================
// MOVEMENT — hold V to suspend
// =================================================================================================

bool UTraceAbilitySetMace::OnSecondaryPressed()
{
	bSuspendHeld = true;

	const ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		return false;
	}

	// "hold V IN THE AIR to suspend". On the ground it is simply not the ability.
	if (!MoveComp->IsFalling())
	{
		return false;
	}

	if (bSuspending || bPulling)
	{
		return false;
	}

	if (MatchTimeNow() < SuspendReadyMatchTime)
	{
		return false;
	}

	StartSuspend();
	return true;
}

void UTraceAbilitySetMace::OnSecondaryReleased()
{
	bSuspendHeld = false;

	// "Releasing V cancels IMMEDIATELY and gravity resumes." Not at the end of the frame, not on the
	// next physics step: the flag is cleared here and ApplySuspend simply stops writing Velocity.Z.
	if (bSuspending)
	{
		StopSuspend(TEXT("V released"));
	}
}

void UTraceAbilitySetMace::StartSuspend()
{
	bSuspending = true;
	SuspendEndMatchTime = MatchTimeNow() + FMath::Max(0.05f, UTraceSettings::Get().MaceSuspendMaxSeconds);

	if (HasAuthority())
	{
		PublishState();
	}
}

void UTraceAbilitySetMace::StopSuspend(const TCHAR* Why)
{
	if (!bSuspending)
	{
		return;
	}

	bSuspending = false;
	SuspendEndMatchTime = 0.f;
	SuspendReadyMatchTime = MatchTimeNow() + FMath::Max(0.f, UTraceSettings::Get().MaceSuspendCooldownSeconds);

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Suspend ended (%s)."), Why);
	}
}

void UTraceAbilitySetMace::ApplySuspend(float DeltaSeconds)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		StopSuspend(TEXT("no pawn"));
		return;
	}
	if (bPulling)
	{
		StopSuspend(TEXT("pull started"));
		return;
	}
	if (!MoveComp->IsFalling())
	{
		StopSuspend(TEXT("landed"));
		return;
	}
	if (MatchTimeNow() >= SuspendEndMatchTime)
	{
		StopSuspend(TEXT("1.25 s window elapsed"));
		return;
	}
	if (!bSuspendHeld)
	{
		StopSuspend(TEXT("no longer held"));
		return;
	}

	// "gravity does not affect her" — the whole of it. Velocity.Z is re-zeroed every frame rather
	// than once, because PhysFalling integrates gravity on every sub-step in between.
	FVector Vel = MoveComp->Velocity;
	Vel.Z = 0.f;

	// "she may move laterally capped at 550 uu/s". A CLAMP, not a scale: below the cap her ordinary
	// air strafe is untouched, which is what "may move laterally" reads as.
	const float Cap = FMath::Max(0.f, UTraceSettings::Get().MaceSuspendLateralSpeedCap);
	const FVector Planar(Vel.X, Vel.Y, 0.f);
	const float PlanarSpeed = Planar.Size();
	if (PlanarSpeed > Cap && PlanarSpeed > KINDA_SMALL_NUMBER)
	{
		const FVector Capped = Planar * (Cap / PlanarSpeed);
		Vel.X = Capped.X;
		Vel.Y = Capped.Y;
	}

	MoveComp->Velocity = Vel;
}

float UTraceAbilitySetMace::GetSuspendRemaining() const
{
	return bSuspending ? FMath::Max(0.f, SuspendEndMatchTime - MatchTimeNow()) : 0.f;
}

// =================================================================================================
// ACTIVATED — Spike
// =================================================================================================

float UTraceAbilitySetMace::GetActivatedCooldownSeconds() const
{
	// [ASSUMPTION] §6 leaves Mace's cooldown unspecified and the spec suggests 20 s "to match the
	// others". Shipped as a knob so a playtest can move it without a rebuild.
	return FMath::Max(0.f, UTraceSettings::Get().MaceSpikeCooldownSeconds);
}

bool UTraceAbilitySetMace::CanActivate(FText& OutReason) const
{
	if (bPulling)
	{
		OutReason = NSLOCTEXT("Trace", "MaceAlreadyPulling", "PULLING");
		return false;
	}
	return true;
}

bool UTraceAbilitySetMace::IsSpikeableSurface(const FHitResult& Hit, FString& OutWhy) const
{
	// "On hitting a WALL it embeds."
	//
	// THIS USED TO READ WallJumpMaxNormalZ, on the reasoning that the project should have exactly one
	// definition of a wall. Trace.Mace.SpikeConsistency measured that reasoning wrong: the wall jump's
	// 0.40 is a surface 66 degrees off horizontal, so every surface between the 45-degree walkable
	// limit and 66 degrees was refused — geometry she can neither stand on, walk up, NOR spike. Plates
	// at |normal.Z| 0.50 and 0.64 scored 0/14.
	//
	// MaceSpikeMaxSurfaceNormalZ defaults to the walkable floor limit instead, so the rule the spike
	// applies is "if she cannot walk on it, she can stick a spike in it". The wall jump keeps its own
	// number, which is a movement-feel decision that has been tuned against and has no reason to move
	// because the spike moved.
	const float MaxNormalZ = FMath::Clamp(UTraceSettings::Get().MaceSpikeMaxSurfaceNormalZ, 0.f, 1.f);
	if (FMath::Abs(Hit.ImpactNormal.Z) > MaxNormalZ)
	{
		OutWhy = FString::Printf(TEXT("hit a surface with |normal.Z| %.2f > %.2f — a floor or a ramp, not a wall"),
			FMath::Abs(Hit.ImpactNormal.Z), MaxNormalZ);
		return false;
	}
	return true;
}

bool UTraceAbilitySetMace::ResolveSpikeAnchor(FVector& OutAnchor, FVector& OutNormal, FString& OutWhy) const
{
	OutAnchor = FVector::ZeroVector;
	OutNormal = FVector::ZeroVector;

	const ATraceCharacter* MyPawn = GetCharacter();
	UWorld* WorldPtr = GetWorld();
	if (MyPawn == nullptr || WorldPtr == nullptr)
	{
		OutWhy = TEXT("no pawn");
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Range = FMath::Max(100.f, Settings.MaceSpikeRangeUU);
	const FVector Start = MyPawn->GetMuzzleLocation();
	const FVector End = Start + MyPawn->GetAimDirection() * Range;

	// BY OBJECT TYPE, not by channel. A channel trace on ECC_WorldStatic is also blocked by every
	// pawn capsule (the "Pawn" profile blocks the WorldStatic channel), so a team-mate standing in
	// front of Mace would have eaten the throw and reported "no wall". Asking for WorldStatic
	// OBJECTS asks the question the doc asks: is there a wall there.
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MaceSpikeThrow), /*bTraceComplex*/ false);
	QueryParams.AddIgnoredActor(MyPawn);

	// ---- STAGE 1: THE EXACT AIM RAY. Tried first, and it wins whenever it lands. ------------------
	//
	// It has to stay first, and it has to stay a line: the crosshair is exact in first person
	// (ATraceCharacter::GetAimDirection is arithmetically the view forward), so the wall the player
	// is looking AT must beat any wall the forgiveness below would settle for.
	FString WhyExactRayFailed;
	FHitResult Hit;
	if (WorldPtr->LineTraceSingleByObjectType(Hit, Start, End, ObjectParams, QueryParams))
	{
		if (IsSpikeableSurface(Hit, WhyExactRayFailed))
		{
			OutAnchor = Hit.ImpactPoint;
			OutNormal = Hit.ImpactNormal;
			OutWhy = TEXT("wall");
			return true;
		}
	}
	else
	{
		WhyExactRayFailed = FString::Printf(TEXT("nothing within %.0f uu"), Range);
	}

	// ---- STAGE 2: THE FORGIVENESS SWEEP. Only reached when the exact ray found nothing to stick to.
	//
	// An infinitely thin ray misses a 40 uu pillar at 1200 uu on one degree of aim error and a 16 uu
	// railing on half a degree — while the crosshair is still visibly on them. MEASURED at 5/7 and
	// 3/7 by Trace.Mace.SpikeConsistency, and the player has no way to tell that silence apart from
	// a bug. A sphere the width of the spike head is the smallest thing that fixes it.
	//
	// bStartPenetrating is refused rather than used: a sweep that begins inside geometry reports the
	// depenetration direction, not a surface, and anchoring to that would put the spike inside a wall
	// at a normal nobody asked about. Stage 1 is the path for anything that close.
	const float SweepRadius = FMath::Clamp(Settings.MaceSpikeTraceRadiusUU, 0.f, 60.f);
	if (SweepRadius > KINDA_SMALL_NUMBER)
	{
		FHitResult SweepHit;
		if (WorldPtr->SweepSingleByObjectType(SweepHit, Start, End, FQuat::Identity, ObjectParams,
				FCollisionShape::MakeSphere(SweepRadius), QueryParams)
			&& !SweepHit.bStartPenetrating)
		{
			FString WhySweepFailed;
			if (IsSpikeableSurface(SweepHit, WhySweepFailed))
			{
				OutAnchor = SweepHit.ImpactPoint;
				OutNormal = SweepHit.ImpactNormal;
				OutWhy = TEXT("wall (aim forgiveness sweep)");
				return true;
			}
		}
	}

	// The exact ray's complaint is the one reported: it is the one that describes what the player was
	// actually pointing at, and the harness classifies fizzles by reading it.
	OutWhy = WhyExactRayFailed;
	return false;
}

bool UTraceAbilitySetMace::ActivateAbility()
{
	const ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr)
	{
		return false;
	}

	FVector Anchor = FVector::ZeroVector;
	FVector Normal = FVector::ZeroVector;
	FString Why;

	// THE SWEEP RUNS ON BOTH MACHINES. The client needs the same answer the server will reach or its
	// predicted cooldown is wrong for a whole round trip; arena geometry is present on both, so the
	// sweep is the cheapest way to agree. Only the server spawns anything.
	if (!ResolveSpikeAnchor(Anchor, Normal, Why))
	{
		// A FREE FIZZLE. The framework charges the cooldown only on a true return, and the header of
		// UTraceCharacterAbilitySet names "Mace's spike hitting nothing" as the example. Throwing at
		// the sky must not cost 20 s.
		UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Spike fizzled: %s. No cooldown charged."), *Why);
		return false;
	}

	if (HasAuthority())
	{
		// A second throw replaces the first rather than orphaning it. Reachable after a half-time
		// cooldown reset while a spike is still in the wall.
		ClearSpike();

		UWorld* WorldPtr = GetWorld();
		if (WorldPtr == nullptr)
		{
			return false;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Owner = const_cast<ATraceCharacter*>(MyPawn);

		ATraceMaceSpike* NewSpike = WorldPtr->SpawnActor<ATraceMaceSpike>(
			ATraceMaceSpike::StaticClass(), MyPawn->GetMuzzleLocation(), Normal.Rotation(), SpawnParams);

		if (NewSpike == nullptr)
		{
			return false;
		}

		const UTraceSettings& Settings = UTraceSettings::Get();
		// The backstop is generous on purpose: it is not the ability's timer, it is the "the owner
		// vanished" catch. The embed window is enforced in TickAbilities.
		NewSpike->InitialiseFlight(this, Anchor, FMath::Max(1.f, Settings.MaceSpikeTravelSpeed),
			/*BackstopLifetimeSeconds*/ Settings.MaceSpikeEmbedSeconds + 10.f);

		Spike = NewSpike;
		SpikeEmbedEndMatchTime = 0.f;    // starts when the flight ends, not when it is thrown
		PublishState();

		UE_LOG(LogTraceGame, Log, TEXT("[Mace] Spike thrown at %s (%.0f uu away); embeds for %.1fs on arrival."),
			*Anchor.ToCompactString(), FVector::Dist(MyPawn->GetActorLocation(), Anchor),
			UTraceSettings::Get().MaceSpikeEmbedSeconds);
	}

	return true;
}

ATraceMaceSpike* UTraceAbilitySetMace::DebugThrowSpikeAt(const FVector& Anchor, float TravelSpeedOverride)
{
	if (!HasAuthority())
	{
		return nullptr;
	}
	UWorld* WorldPtr = GetWorld();
	ATraceCharacter* MyPawn = GetCharacter();
	if (WorldPtr == nullptr || MyPawn == nullptr)
	{
		return nullptr;
	}

	ClearSpike();

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = MyPawn;

	ATraceMaceSpike* NewSpike = WorldPtr->SpawnActor<ATraceMaceSpike>(
		ATraceMaceSpike::StaticClass(), MyPawn->GetMuzzleLocation(), FRotator::ZeroRotator, SpawnParams);
	if (NewSpike == nullptr)
	{
		return nullptr;
	}

	NewSpike->InitialiseFlight(this, Anchor, TravelSpeedOverride,
		UTraceSettings::Get().MaceSpikeEmbedSeconds + 10.f);

	Spike = NewSpike;
	SpikeEmbedEndMatchTime = 0.f;

	if (NewSpike->IsEmbedded())
	{
		NotifySpikeEmbedded(NewSpike);
	}
	else
	{
		PublishState();
	}
	return NewSpike;
}

void UTraceAbilitySetMace::NotifySpikeEmbedded(ATraceMaceSpike* WhichSpike)
{
	if (!HasAuthority() || WhichSpike == nullptr || WhichSpike != Spike.Get())
	{
		return;
	}

	// "On hitting a wall it embeds for 2 s." The window starts HERE, on arrival, not at the throw —
	// otherwise a long throw would spend most of its 2 s in the air.
	SpikeEmbedEndMatchTime = MatchTimeNow() + FMath::Max(0.1f, UTraceSettings::Get().MaceSpikeEmbedSeconds);
	PublishState();

	UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Spike embedded; pullable for %.1fs."),
		UTraceSettings::Get().MaceSpikeEmbedSeconds);

	// A press made while it was still travelling fires HERE, on the frame it lands. See bPullQueued.
	if (bPullQueued)
	{
		bPullQueued = false;
		StartPull();
	}
}

// =================================================================================================
// WHOSE SPIKE IS IT — the two accessors below exist because only the SERVER has the pointer
// =================================================================================================
//
// ActivateAbility() spawns the actor inside `if (HasAuthority())`, so `Spike` is set on the server
// and NEVER on a remote client — the client's copy of the actor exists (it replicates) but nothing
// hands the ability set a pointer to it. Everything that asked `Spike.Get()` was therefore answering
// "no spike" on every machine that is not the server, which is why the reactivation could not be
// pressed on a client at all: IsSpikeReadyToPull() was false, the relay fell through to a fresh
// TryActivate(), and the cooldown the throw had just started refused it.
//
// The replicated scratch pad already carries both facts for the HUD — the flags and AuxLocation —
// so these read the pointer where it exists and the replicated state everywhere else.

bool UTraceAbilitySetMace::HasSpikeEmbedded() const
{
	if (const ATraceMaceSpike* MySpike = Spike.Get())
	{
		return MySpike->IsEmbedded();
	}
	return !HasAuthority() && (State().Flags & TraceMaceFlags::SpikeEmbedded) != 0;
}

bool UTraceAbilitySetMace::HasSpikeInFlight() const
{
	if (const ATraceMaceSpike* MySpike = Spike.Get())
	{
		return !MySpike->IsEmbedded();
	}
	return !HasAuthority() && (State().Flags & TraceMaceFlags::SpikeInFlight) != 0;
}

FVector UTraceAbilitySetMace::GetSpikeAnchorLocation() const
{
	if (const ATraceMaceSpike* MySpike = Spike.Get())
	{
		return MySpike->GetAnchorLocation();
	}
	return State().AuxLocation;
}

bool UTraceAbilitySetMace::IsSpikeReadyToPull() const
{
	if (bPulling)
	{
		return false;
	}
	// IN FLIGHT COUNTS. See RequestSpikePull: the press cannot pull yet, but it must not be handed
	// back to the framework as a fresh throw either, because TryActivate() will refuse it against the
	// cooldown this same spike started and the input is then simply gone.
	if (!HasSpikeEmbedded() && !HasSpikeInFlight())
	{
		return false;
	}

	const ATraceCharacter* MyPawn = GetCharacter();
	return MyPawn != nullptr && MyPawn->IsAlive();
}

float UTraceAbilitySetMace::GetPullSpeed() const
{
	// §6: "pulls her toward it AT THE MOMENTUM CEILING (the air-strafe hard cap)". DERIVED — the
	// ceiling is AirStrafeHardCapSpeed x AirStrafeAsymptoteScale, which is exactly how
	// UTraceCharacterMovementComponent::GetAirStrafeHardCapSpeed() composes it, and those two knobs
	// stay the only place the number lives.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Ceiling = FMath::Max(1.f, Settings.AirStrafeHardCapSpeed)
		* FMath::Clamp(Settings.AirStrafeAsymptoteScale, 0.5f, 2.f);
	return Ceiling * FMath::Max(0.1f, Settings.MaceSpikePullSpeedMultiplier);
}

void UTraceAbilitySetMace::RequestSpikePull()
{
	if (!IsSpikeReadyToPull())
	{
		return;
	}

	// THE PRESS IS REMEMBERED RATHER THAN DROPPED. A player who throws to reach height and
	// immediately double-taps is pressing during the flight every single time, and the flight is a
	// third of a second at the v15 travel speed. Before this the press went nowhere at all: nothing
	// may pull on a spike that has not landed, so RequestSpikePull returned and TryActivate had
	// already been ruled out by the cooldown.
	//
	// It is a remembered press, NOT a timed window: it is cleared by the pull starting, by the spike
	// going away, and by a second throw (ClearSpike), so it cannot outlive the throw that armed it.
	if (!HasSpikeEmbedded())
	{
		bPullQueued = true;
		return;
	}

	StartPull();
}

void UTraceAbilitySetMace::StartPull()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (!HasSpikeEmbedded() || MyPawn == nullptr)
	{
		return;
	}

	StopSuspend(TEXT("pull started"));

	bPulling = true;
	bPullQueued = false;
	PullAnchor = GetSpikeAnchorLocation();
	PullLastLocation = MyPawn->GetActorLocation();
	bPullSkipInputTestThisTick = true;

	if (UTraceCharacterMovementComponent* MoveComp = GetMovement())
	{
		// The pull must be able to lift her off the floor. Without this a ground-level pull is
		// integrated by PhysWalking, which discards Z and drags her along the floor instead.
		if (MoveComp->IsMovingOnGround())
		{
			MoveComp->SetMovementMode(MOVE_Falling);
		}
	}

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Log, TEXT("[Mace] Pull started toward %s at %.0f uu/s (the momentum ceiling)."),
			*PullAnchor.ToCompactString(), GetPullSpeed());
	}
}

void UTraceAbilitySetMace::StopPull(const TCHAR* Why, bool bRemoveSpike)
{
	if (!bPulling)
	{
		if (bRemoveSpike)
		{
			ClearSpike();
		}
		return;
	}

	bPulling = false;
	bPullSkipInputTestThisTick = false;
	bPullQueued = false;

	if (bRemoveSpike)
	{
		ClearSpike();
	}

	if (HasAuthority())
	{
		PublishState();
		UE_LOG(LogTraceGame, Log, TEXT("[Mace] Pull ended (%s)%s."), Why,
			bRemoveSpike ? TEXT(", spike removed") : TEXT(""));
	}
}

void UTraceAbilitySetMace::ApplyPull(float DeltaSeconds)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();

	// HasSpikeEmbedded() rather than Spike.Get(): on the owning client the pointer is always null (see
	// the accessors above), so asking for it here cancelled the predicted pull on its very first tick
	// and the client saw a pull that never moved her.
	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr || !HasSpikeEmbedded())
	{
		StopPull(TEXT("pawn or spike gone"), /*bRemoveSpike*/ true);
		return;
	}

	// THE FIRST TICK OF A PULL TESTS NEITHER OF THE TWO CANCEL CONDITIONS, and both exclusions are
	// load-bearing rather than tidy:
	//
	//   the INPUT test, because §6's "any movement input cancels" taken literally would cancel on the
	//   very frame a player still holding W presses the key (see the header on bPullSkipInputTestThisTick);
	//
	//   the BLOCKED test, because PullLastLocation was seeded at StartPull() in this same frame, so
	//   the pawn has by definition not moved yet and every pull would cancel itself as "bounced off
	//   geometry" one frame after it began. MEASURED: this is exactly what the first build did, and
	//   Trace.Mace.Verify caught it as a pull that reached 0 uu/s.
	const bool bFirstPullTick = bPullSkipInputTestThisTick;
	bPullSkipInputTestThisTick = false;

	// GetCurrentAcceleration() is the wish vector after the controller has applied it, and it is
	// valid on the server for a remote client too (ServerMove carries it), so both ends cancel on the
	// same press.
	if (!bFirstPullTick && !MoveComp->GetCurrentAcceleration().IsNearlyZero(1.f))
	{
		StopPull(TEXT("movement input"), /*bRemoveSpike*/ true);
		return;
	}

	const FVector Here = MyPawn->GetActorLocation();
	const FVector ToAnchor = PullAnchor - Here;
	const float Distance = ToAnchor.Size();

	const float ArriveRadius = FMath::Max(20.f, UTraceSettings::Get().MaceSpikeArriveRadiusUU);
	if (Distance <= ArriveRadius)
	{
		StopPull(TEXT("arrived"), /*bRemoveSpike*/ true);
		return;
	}

	// "She obeys normal physics while pulled — BOUNCING OFF A WALL CANCELS IT."
	//
	// Measured rather than predicted: the pull writes a known velocity, so the displacement it should
	// have produced last frame is known too. If the pawn covered far less than that, something solid
	// was in the way — which is precisely "bounced off a wall", and it also covers being wedged on a
	// ledge or a corner. Done AFTER the first tick (PullLastLocation is seeded at StartPull).
	const float ExpectedStep = GetPullSpeed() * DeltaSeconds;
	if (!bFirstPullTick && ExpectedStep > KINDA_SMALL_NUMBER && !PullLastLocation.IsZero())
	{
		const float ActualStep = FVector::Dist(Here, PullLastLocation);
		if (ActualStep < ExpectedStep * 0.35f)
		{
			StopPull(TEXT("blocked — bounced off geometry"), /*bRemoveSpike*/ true);
			return;
		}
	}
	PullLastLocation = Here;

	// The pull IS her velocity for the frame. Nothing here touches IsDashing(), so
	// AreWeaponActionsBlocked() stays false and §6's "she can shoot while pulled, and be shot" is
	// true for free — the weapon gate is a pure function of the dash clock.
	MoveComp->Velocity = (ToAnchor / Distance) * GetPullSpeed();
}

void UTraceAbilitySetMace::ClearSpike()
{
	if (ATraceMaceSpike* MySpike = Spike.Get())
	{
		if (HasAuthority())
		{
			MySpike->Destroy();
		}
	}
	Spike.Reset();
	SpikeEmbedEndMatchTime = 0.f;
	// A remembered reactivation belongs to ONE throw. ActivateAbility clears the old spike before
	// spawning the new one, so this is also what stops a press aimed at a spike she has replaced.
	bPullQueued = false;

	if (HasAuthority())
	{
		PublishState();
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void UTraceAbilitySetMace::TickAbilities(float DeltaSeconds)
{
	// The whole character is mode-B only. AreCharactersEnabled() is false in mode A and false with
	// the §3 toggle off, and the world subsystem will already be forcing her back to the Mannequin;
	// this stops her writing velocity in the frames before that lands.
	if (!UTraceAbilityComponent::AreCharactersEnabled(this))
	{
		if (bSuspending || bPulling)
		{
			StopSuspend(TEXT("characters disabled"));
			StopPull(TEXT("characters disabled"), /*bRemoveSpike*/ true);
		}
		return;
	}

	if (HasAuthority())
	{
		// The 2 s embed window. Not enforced while she is mid-pull: the window is how long she has to
		// USE the spike, and cutting a pull that has already started at the two-second mark would be
		// a different rule from the one §6 states.
		const ATraceMaceSpike* MySpike = Spike.Get();
		if (MySpike != nullptr && MySpike->IsEmbedded() && !bPulling
			&& SpikeEmbedEndMatchTime > 0.f && MatchTimeNow() >= SpikeEmbedEndMatchTime)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Spike embed window expired unused."));
			ClearSpike();
		}
		else if (MySpike == nullptr && (State().Flags & (TraceMaceFlags::SpikeEmbedded | TraceMaceFlags::SpikeInFlight)) != 0)
		{
			PublishState();   // the actor went away by some route that did not publish
		}
		else if (MySpike != nullptr && MySpike->IsEmbedded()
			&& (State().Flags & TraceMaceFlags::SpikeEmbedded) == 0)
		{
			PublishState();   // the flight ended on a frame NotifySpikeEmbedded could not reach us
		}
	}

	if (!ShouldDriveMovement())
	{
		return;
	}

	// THE REMEMBERED REACTIVATION. NotifySpikeEmbedded fires this on the server the instant the spike
	// lands; this is the path for the owning client, where "it landed" arrives as a replicated flag
	// rather than as a call. Also the place a remembered press is forgotten when the spike it was
	// meant for stopped existing — a queued press must never survive its own throw.
	if (bPullQueued && !bPulling)
	{
		if (HasSpikeEmbedded())
		{
			bPullQueued = false;
			StartPull();
		}
		else if (!HasSpikeInFlight())
		{
			bPullQueued = false;
		}
	}

	if (bPulling)
	{
		ApplyPull(DeltaSeconds);
		return;   // the pull owns velocity outright; a suspend on the same frame would fight it
	}

	if (bSuspending)
	{
		ApplySuspend(DeltaSeconds);
	}
}

// =================================================================================================
// Replicated mirror — HUD, proxies, and the harness reading a client
// =================================================================================================

void UTraceAbilitySetMace::PublishState()
{
	if (!HasAuthority())
	{
		return;
	}

	const ATraceMaceSpike* MySpike = Spike.Get();

	FTraceAbilityNetState& NetState = MutableState();
	NetState.Flags = 0;
	if (bSuspending)                                 { NetState.Flags |= TraceMaceFlags::Suspending; }
	if (bPulling)                                    { NetState.Flags |= TraceMaceFlags::Pulling; }
	if (MySpike != nullptr && MySpike->IsEmbedded()) { NetState.Flags |= TraceMaceFlags::SpikeEmbedded; }
	if (MySpike != nullptr && !MySpike->IsEmbedded()){ NetState.Flags |= TraceMaceFlags::SpikeInFlight; }

	NetState.EffectEndMatchTime = SuspendEndMatchTime;
	NetState.AuxEndMatchTime = SpikeEmbedEndMatchTime;
	NetState.AuxLocation = (MySpike != nullptr) ? MySpike->GetAnchorLocation() : FVector::ZeroVector;

	MarkStateDirty();
}
