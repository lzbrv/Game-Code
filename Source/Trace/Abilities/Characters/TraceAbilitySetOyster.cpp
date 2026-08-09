// Trace — OYSTER. See the header for the four carrier-rule vectors and where each is enforced.

#include "Abilities/Characters/TraceAbilitySetOyster.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceAbilityInputRelay.h"
#include "Abilities/Characters/TraceOysterJar.h"
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace
{
	/**
	 * Seconds after a jar jump in which a second one is refused.
	 *
	 * NOT a cooldown on the ability — it is a latch between the two routes that can notice a jump
	 * (the framework's OnJumpPressed hook, and this file's own ground-state poll that stands in for
	 * it until the hook is wired). Both must be live, because either may be the only one running, and
	 * without the latch a single press could boost twice on the frame they overlap. Short enough that
	 * it cannot refuse a genuine second jump: nobody lands and re-jumps in a fifth of a second.
	 */
	constexpr float JarJumpLatchSeconds = 0.2f;
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void UTraceAbilitySetOyster::OnEquipped()
{
	LiveJars.Reset();
	bWasDashing = false;
	bDashJarSpawnedThisDash = false;
	bWasOnGround = false;
	LastJarJumpMatchTime = -100.f;

	if (HasAuthority())
	{
		if (UTraceAbilityComponent* Comp = GetAbilityComponent())
		{
			UTraceAbilityInputRelay::EnsureOn(Comp->GetOwningPlayerState());
		}
		PublishState();
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	UE_LOG(LogTraceGame, Log,
		TEXT("[Oyster] Equipped. jars %.1fs x%d, break %.0f uu | poison %.0f/%.2fs for %.1fs (-%.0f%% speed) in %.0f uu "
		     "| jar-jump %.0f uu/s within %.0f uu | Pickler %.0f dmg in %.0f uu, pull %.0f uu/s in %.0f uu, cooldown %.0fs "
		     "[ASSUMPTION: §6 unspecified]"),
		Settings.OysterJarLifetimeSeconds, Settings.OysterMaxJars, Settings.OysterJarBreakRadiusUU,
		Settings.OysterPoisonDamagePerTick, Settings.OysterPoisonTickIntervalSeconds,
		Settings.OysterPoisonDurationSeconds, Settings.OysterPoisonSlowFraction * 100.f, Settings.OysterPoisonRadiusUU,
		Settings.OysterJarJumpZVelocity, Settings.OysterJarJumpRadiusUU,
		Settings.OysterPicklerDamage, Settings.OysterPicklerDamageRadiusUU,
		Settings.OysterPicklerPullSpeed, Settings.OysterPicklerPullRadiusUU,
		GetActivatedCooldownSeconds());
}

void UTraceAbilitySetOyster::OnUnequipped()
{
	DestroyAllJars();
}

void UTraceAbilitySetOyster::OnPawnDied()
{
	// Cooldowns are untouched — §5. His jars are not: a jar is a thing he put in the world and a
	// corpse should not keep poisoning people for four more seconds.
	DestroyAllJars();
	bWasDashing = false;
	bDashJarSpawnedThisDash = false;
	bWasOnGround = false;
}

void UTraceAbilitySetOyster::OnHalfTime()
{
	DestroyAllJars();
}

bool UTraceAbilitySetOyster::ShouldDriveMovement() const
{
	return HasAuthority() || IsLocallyControlled();
}

// =================================================================================================
// Jar bookkeeping — "max 3; a fourth despawns the oldest"
// =================================================================================================

int32 UTraceAbilitySetOyster::GetLiveJarCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<ATraceOysterJar>& Entry : LiveJars)
	{
		if (Entry.IsValid())
		{
			++Count;
		}
	}
	return Count;
}

void UTraceAbilitySetOyster::PruneJars()
{
	LiveJars.RemoveAll([](const TWeakObjectPtr<ATraceOysterJar>& Entry) { return !Entry.IsValid(); });

	const int32 MaxJars = FMath::Max(1, UTraceSettings::Get().OysterMaxJars);
	while (LiveJars.Num() > MaxJars)
	{
		// FRONT, not back: the array is in spawn order, so the front is the oldest. "A fourth
		// despawns the oldest" — and it DESPAWNS rather than breaks, so no poison burst here. A jar
		// that vanishes because the player threw another one should not also poison the room.
		TWeakObjectPtr<ATraceOysterJar> Oldest = LiveJars[0];
		LiveJars.RemoveAt(0);
		if (ATraceOysterJar* JarActor = Oldest.Get())
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("[Oyster] Jar cap %d reached — despawning the oldest."), MaxJars);
			JarActor->Destroy();
		}
	}
}

void UTraceAbilitySetOyster::DestroyAllJars()
{
	if (HasAuthority())
	{
		for (const TWeakObjectPtr<ATraceOysterJar>& Entry : LiveJars)
		{
			if (ATraceOysterJar* JarActor = Entry.Get())
			{
				JarActor->Destroy();
			}
		}
	}
	LiveJars.Reset();

	if (HasAuthority())
	{
		PublishState();
	}
}

ATraceOysterJar* UTraceAbilitySetOyster::SpawnJar(const FVector& Location, const FVector& Velocity, bool bPickler)
{
	if (!HasAuthority())
	{
		return nullptr;
	}

	UWorld* WorldPtr = GetWorld();
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceAbilityComponent* Comp = GetAbilityComponent();
	if (WorldPtr == nullptr || MyPawn == nullptr || Comp == nullptr)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = MyPawn;
	SpawnParams.Instigator = MyPawn;

	ATraceOysterJar* JarActor = WorldPtr->SpawnActor<ATraceOysterJar>(
		ATraceOysterJar::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
	if (JarActor == nullptr)
	{
		return nullptr;
	}

	JarActor->Initialise(Comp, MyPawn->GetTeam(), bPickler, Velocity);

	LiveJars.Add(JarActor);
	PruneJars();
	PublishState();

	return JarActor;
}

ATraceOysterJar* UTraceAbilitySetOyster::FindOwnJarNear(const FVector& Location) const
{
	const float Radius = FMath::Max(1.f, UTraceSettings::Get().OysterJarJumpRadiusUU);

	ATraceOysterJar* Best = nullptr;
	float BestDistance = MAX_flt;

	for (const TWeakObjectPtr<ATraceOysterJar>& Entry : LiveJars)
	{
		ATraceOysterJar* JarActor = Entry.Get();
		if (JarActor == nullptr || !JarActor->IsGrounded())
		{
			continue;
		}
		const float Distance = FVector::Dist(JarActor->GetActorLocation(), Location);
		if (Distance <= Radius && Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = JarActor;
		}
	}
	return Best;
}

// =================================================================================================
// PASSIVE — a jar at the start of every dash
// =================================================================================================

bool UTraceAbilitySetOyster::OnDashStarted(const FVector& DashDirection)
{
	if (HasAuthority() && !bDashJarSpawnedThisDash)
	{
		bDashJarSpawnedThisDash = true;
		DebugDropDashJar();
	}
	return false;   // never cancels the dash
}

void UTraceAbilitySetOyster::OnDashEnded(bool bReachedFullDistance)
{
	bDashJarSpawnedThisDash = false;
}

ATraceOysterJar* UTraceAbilitySetOyster::DebugDropDashJar()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		return nullptr;
	}

	// At his FEET, not at his origin: a jar spawned at capsule centre floats at chest height and the
	// break radius then measures from the wrong place.
	float FootOffset = 0.f;
	if (const UCapsuleComponent* Capsule = MyPawn->GetCapsuleComponent())
	{
		FootOffset = Capsule->GetScaledCapsuleHalfHeight();
	}

	return SpawnJar(MyPawn->GetActorLocation() - FVector(0.f, 0.f, FootOffset),
		FVector::ZeroVector, /*bPickler*/ false);
}

// =================================================================================================
// MOVEMENT — the jar jump
// =================================================================================================

bool UTraceAbilitySetOyster::OnJumpPressed()
{
	ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || MoveComp == nullptr || !MoveComp->IsMovingOnGround())
	{
		return false;   // "while STOOD on one of his jars"
	}

	return DoJarJump(MyPawn->GetActorLocation());
}

bool UTraceAbilitySetOyster::DebugTryJarJump()
{
	ATraceCharacter* MyPawn = GetCharacter();
	return (MyPawn != nullptr) && DoJarJump(MyPawn->GetActorLocation());
}

bool UTraceAbilitySetOyster::DoJarJump(const FVector& FromLocation)
{
	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* MoveComp = GetMovement();
	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		return false;
	}

	const float Now = MatchTimeNow();
	if ((Now - LastJarJumpMatchTime) < JarJumpLatchSeconds)
	{
		return false;   // the other route already took this press
	}

	// The jar list is server-side only, so the owning client cannot find its own jar this way. It
	// still has to APPLY the boost or the jump would be a round trip late and then corrected; the
	// server's break is what makes it authoritative. See the report.
	if (HasAuthority())
	{
		ATraceOysterJar* JarActor = FindOwnJarNear(FromLocation);
		if (JarActor == nullptr)
		{
			return false;
		}

		LiveJars.RemoveAll([JarActor](const TWeakObjectPtr<ATraceOysterJar>& Entry) { return Entry.Get() == JarActor; });

		// "BREAKS it" — the same break an enemy's touch produces, poison burst and all. §6 gives the
		// jar one break, not two kinds. [ASSUMPTION] flagged in the report: the doc does not say
		// whether Oyster's own jar-jump poisons nearby enemies, and this says yes because "breaks it"
		// is the same verb.
		JarActor->ServerBreakNow(TEXT("Oyster's jar jump"));
		PublishState();
	}

	LastJarJumpMatchTime = Now;

	if (ShouldDriveMovement())
	{
		// "BOOSTS HIM UPWARD." An absolute Z, not an addition: the boost replaces the jump rather
		// than compounding with whatever vertical speed he happened to have.
		const float BoostZ = FMath::Max(0.f, UTraceSettings::Get().OysterJarJumpZVelocity);
		if (MoveComp->IsMovingOnGround())
		{
			MoveComp->SetMovementMode(MOVE_Falling);
		}
		MoveComp->Velocity.Z = FMath::Max(MoveComp->Velocity.Z, BoostZ);
	}

	if (HasAuthority())
	{
		UE_LOG(LogTraceGame, Log, TEXT("[Oyster] Jar jump: broke his own jar and launched at %.0f uu/s."),
			UTraceSettings::Get().OysterJarJumpZVelocity);
	}
	return true;
}

// =================================================================================================
// ACTIVATED — Pickler
// =================================================================================================

float UTraceAbilitySetOyster::GetActivatedCooldownSeconds() const
{
	// [ASSUMPTION] §6 leaves it unspecified; 20 s to match the others, as a knob.
	return FMath::Max(0.f, UTraceSettings::Get().OysterPicklerCooldownSeconds);
}

bool UTraceAbilitySetOyster::ActivateAbility()
{
	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || !MyPawn->IsAlive())
	{
		return false;
	}

	if (HasAuthority())
	{
		const UTraceSettings& Settings = UTraceSettings::Get();
		const float Speed = FMath::Max(1.f, Settings.OysterPicklerThrowSpeed);
		const float UpBias = FMath::Max(0.f, Settings.OysterPicklerThrowUpBias);

		// A LOB, not a shot: the up bias is a fraction of the throw speed, so retuning the speed
		// keeps the same arc shape rather than flattening it.
		const FVector Velocity = MyPawn->GetAimDirection() * Speed + FVector(0.f, 0.f, Speed * UpBias);

		if (SpawnJar(MyPawn->GetMuzzleLocation(), Velocity, /*bPickler*/ true) == nullptr)
		{
			return false;
		}

		UE_LOG(LogTraceGame, Log, TEXT("[Oyster] Pickler lobbed at %.0f uu/s (+%.0f%% up)."), Speed, UpBias * 100.f);
	}

	// True on both machines: the client charges its predicted cooldown, the server charges the real
	// one. There is no fizzle case — a lob always leaves his hand.
	return true;
}

ATraceOysterJar* UTraceAbilitySetOyster::DebugSpawnJarAt(const FVector& Location, bool bPickler)
{
	return SpawnJar(Location, FVector::ZeroVector, bPickler);
}

// =================================================================================================
// Tick — the two polls that stand in for hooks nobody calls yet
// =================================================================================================

void UTraceAbilitySetOyster::TickAbilities(float DeltaSeconds)
{
	if (!UTraceAbilityComponent::AreCharactersEnabled(this))
	{
		if (HasAuthority() && LiveJars.Num() > 0)
		{
			DestroyAllJars();
		}
		return;
	}

	ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (HasAuthority())
	{
		PruneJars();

		// --- THE DASH POLL ------------------------------------------------------------------------
		//
		// UTraceCharacterMovementComponent never calls NotifyDashStarted, so OnDashStarted above is
		// dead code today. IsDashing() is a pure function of the dash clock and is correct on the
		// server for every pawn, human or bot, so a rising edge on it is the same event the hook
		// would have delivered. The latch is shared, so wiring the hook later cannot double up.
		const bool bDashingNow = (MyPawn != nullptr) && MyPawn->IsDashing();
		if (bDashingNow && !bWasDashing && !bDashJarSpawnedThisDash)
		{
			bDashJarSpawnedThisDash = true;
			DebugDropDashJar();
		}
		else if (!bDashingNow && bWasDashing)
		{
			bDashJarSpawnedThisDash = false;
		}
		bWasDashing = bDashingNow;
	}

	// --- THE JUMP POLL --------------------------------------------------------------------------
	//
	// Runs on the server AND on the owning client, because the boost has to be applied on both or it
	// rubber-bands. ATracePlayerController never calls HandleJumpPressed, so OnJumpPressed above is
	// dead code today; leaving the ground with upward speed is the observable event that stands in
	// for it. The jar is found from the position he was standing at LAST tick — by the time he is
	// airborne he has already left it.
	if (MyPawn != nullptr && MoveComp != nullptr && ShouldDriveMovement())
	{
		const bool bOnGroundNow = MoveComp->IsMovingOnGround();
		if (bWasOnGround && !bOnGroundNow && MoveComp->Velocity.Z > 1.f)
		{
			DoJarJump(LastGroundedLocation);
		}
		if (bOnGroundNow)
		{
			LastGroundedLocation = MyPawn->GetActorLocation();
		}
		bWasOnGround = bOnGroundNow;
	}
}

void UTraceAbilitySetOyster::PublishState()
{
	if (!HasAuthority())
	{
		return;
	}

	FTraceAbilityNetState& NetState = MutableState();
	NetState.Stacks = static_cast<uint8>(FMath::Clamp(GetLiveJarCount(), 0, 255));
	NetState.Flags = (NetState.Stacks > 0) ? TraceAbilityFlags::AuxActive : 0;
	MarkStateDirty();
}
