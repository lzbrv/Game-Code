// Trace — Oyster's poison. See the header for why the choke point is re-asked every tick.

#include "Abilities/Characters/TraceOysterPoison.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// The alarms
// =================================================================================================

namespace TraceOyster
{
	namespace
	{
		FEffectTally GCarrierTally;
		FEffectTally GOtherTally;
	}

	FEffectTally& CarrierTally() { return GCarrierTally; }
	FEffectTally& OtherTally()   { return GOtherTally; }

	void ResetTallies()
	{
		GCarrierTally.Reset();
		GOtherTally.Reset();
	}

	void RecordEffect(const ATraceCharacter* Target, const TCHAR* VectorName, int32 FEffectTally::* Field)
	{
		if (Field == nullptr)
		{
			return;
		}

		if (UTraceAbilityComponent::IsCarrier(Target))
		{
			++(GCarrierTally.*Field);

			// LOUD, and naming the vector. Reaching here means one of Oyster's four ways to touch a
			// player got past spec §4's rule; the whole point of counting per-vector is that the log
			// says WHICH one.
			UE_LOG(LogTraceGame, Error,
				TEXT("[Oyster] *** '%s' RESOLVED ONTO THE CORE CARRIER %s. Spec v14 §4: no ability may damage or "
				     "control a carrier. Check Trace.Ability.CarrierImmune and CanAffectTargetDetailed. ***"),
				VectorName, *GetNameSafe(Target));
		}
		else
		{
			++(GOtherTally.*Field);
		}
	}
}

// =================================================================================================
// The component
// =================================================================================================

UTraceOysterPoisonComponent::UTraceOysterPoisonComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// AFTER physics: the clamp is a correction to the velocity the movement step just produced, and
	// clamping before it would be overwritten by the same step it is trying to limit.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	// SetIsReplicatedByDefault, NOT SetIsReplicated — same reason as UTraceAbilityInputRelay's
	// constructor: SetIsReplicated during CDO construction trips a handled ensure at engine init on
	// every run. The runtime call in EnsureOn below is the legitimate case and is unchanged.
	SetIsReplicatedByDefault(true);
}

void UTraceOysterPoisonComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UTraceOysterPoisonComponent, EndMatchTime);
	DOREPLIFETIME(UTraceOysterPoisonComponent, bSlowActive);
}

ATraceCharacter* UTraceOysterPoisonComponent::GetVictim() const
{
	return Cast<ATraceCharacter>(GetOwner());
}

float UTraceOysterPoisonComponent::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

UTraceOysterPoisonComponent* UTraceOysterPoisonComponent::Find(const ATraceCharacter* Target)
{
	return (Target != nullptr) ? Target->FindComponentByClass<UTraceOysterPoisonComponent>() : nullptr;
}

UTraceOysterPoisonComponent* UTraceOysterPoisonComponent::ApplyTo(ATraceCharacter* Target,
                                                                  UTraceAbilityComponent* SourceComp)
{
	if (Target == nullptr || !Target->HasAuthority() || !Target->IsAlive())
	{
		return nullptr;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Now = (Target->GetWorld() != nullptr && Target->GetWorld()->GetGameState() != nullptr)
		? static_cast<float>(Target->GetWorld()->GetGameState()->GetServerWorldTimeSeconds())
		: 0.f;

	UTraceOysterPoisonComponent* Poison = Target->FindComponentByClass<UTraceOysterPoisonComponent>();
	const bool bFresh = (Poison == nullptr);

	if (bFresh)
	{
		Poison = NewObject<UTraceOysterPoisonComponent>(Target, UTraceOysterPoisonComponent::StaticClass(),
			TEXT("TraceOysterPoison"));

		// SetIsReplicated BEFORE RegisterComponent, for the reason the ability framework's own
		// attachment documents: the flag is sampled at registration.
		Poison->SetIsReplicated(true);
		Poison->RegisterComponent();
	}

	// REFRESH, NOT STACK. §6 gives one duration and one damage number and says nothing about two
	// jars; a second application resetting the clock is the same rule X's vulnerable states
	// explicitly ("a new application resets the timer") and is the conservative reading.
	Poison->EndMatchTime = Now + FMath::Max(0.25f, Settings.OysterPoisonDurationSeconds);
	Poison->SourceComponent = SourceComp;

	if (bFresh)
	{
		// The first tick lands one interval in, so 4 s at 0.5 s is 8 ticks of 3 = 24 damage, which is
		// what "3 damage every 0.5 s for 4 s" reads as.
		Poison->NextTickMatchTime = Now + FMath::Max(0.05f, Settings.OysterPoisonTickIntervalSeconds);
		Poison->DamageDealtSoFar = 0.f;
	}

	TraceOyster::RecordEffect(Target, TEXT("poison applied"), &TraceOyster::FEffectTally::PoisonApplications);

	return Poison;
}

void UTraceOysterPoisonComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (GetOwner() != nullptr && GetOwner()->HasAuthority())
	{
		TickAuthority();
	}

	ApplySlowClamp();
}

void UTraceOysterPoisonComponent::TickAuthority()
{
	ATraceCharacter* Victim = GetVictim();
	const float Now = MatchTimeNow();

	if (Victim == nullptr || !Victim->IsAlive() || Now >= EndMatchTime)
	{
		bSlowActive = false;
		DestroyComponent();
		return;
	}

	UTraceAbilityComponent* SourceComp = SourceComponent.Get();
	AActor* SourceActor = (SourceComp != nullptr) ? SourceComp->GetOwner() : nullptr;

	// *** THE CHOKE POINT, RE-ASKED EVERY TICK. See the header. ***
	//
	// The static form is used rather than the instance form so that a poison whose Oyster has died,
	// left or changed character still has the carrier rule applied to it — the static form applies
	// MayAbilityAffectCarrier even with a null instigator, which is exactly the "orphaned effect"
	// case it was written for.
	const bool bControlAllowed = UTraceAbilityComponent::CanAffect(SourceActor, Victim, ETraceAbilityEffect::Control);
	const bool bDamageAllowed  = UTraceAbilityComponent::CanAffect(SourceActor, Victim, ETraceAbilityEffect::Damage);

	bSlowActive = bControlAllowed;
	if (bSlowActive)
	{
		TraceOyster::RecordEffect(Victim, TEXT("poison slow"), &TraceOyster::FEffectTally::SlowFrames);
	}

	const float Interval = FMath::Max(0.05f, UTraceSettings::Get().OysterPoisonTickIntervalSeconds);
	while (Now >= NextTickMatchTime)
	{
		NextTickMatchTime += Interval;

		if (!bDamageAllowed || SourceComp == nullptr)
		{
			continue;   // the tick is SKIPPED, not deferred: a carrier does not bank up poison damage
		}

		// Through ApplyAbilityDamage, which is the framework's one damage path and asks the choke
		// point again itself. Two independent refusals for the same rule is the intent, not waste.
		const float Dealt = SourceComp->ApplyAbilityDamage(
			Victim, FMath::Max(0.f, UTraceSettings::Get().OysterPoisonDamagePerTick), TEXT("OysterPoison"));

		if (Dealt > 0.f)
		{
			DamageDealtSoFar += Dealt;
			TraceOyster::RecordEffect(Victim, TEXT("poison tick"), &TraceOyster::FEffectTally::PoisonTicks);
		}
	}
}

float UTraceOysterPoisonComponent::GetSpeedMultiplier() const
{
	if (!bSlowActive)
	{
		return 1.f;
	}

	// Clamped at 0.95 for the same reason the clamp below was: a knob of 1.0 would be a total stop,
	// which is not a slow — it is a stun, and §6 does not ask for one.
	const float Fraction = FMath::Clamp(UTraceSettings::Get().OysterPoisonSlowFraction, 0.f, 0.95f);
	return 1.f - Fraction;
}

void UTraceOysterPoisonComponent::ApplySlowClamp()
{
	if (!bSlowActive)
	{
		return;
	}

	ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr || !Victim->IsAlive())
	{
		return;
	}

	// Only the machines that actually simulate this pawn. A simulated proxy's velocity is replicated;
	// clamping it there would fight the interpolation and would slow somebody else's view of a player
	// the server never slowed.
	if (!Victim->HasAuthority() && !Victim->IsLocallyControlled())
	{
		return;
	}

	UTraceCharacterMovementComponent* MoveComp = Victim->GetTraceMovement();
	if (MoveComp == nullptr)
	{
		return;
	}

	// [ASSUMPTION], and a load-bearing one. §6 says "-30% speed for 4 s" and nothing more.
	//
	// NOT DURING A DASH: a dash is velocity on rails for its window and GetMaxSpeed() returns
	// DashSpeed while it runs, so clamping there would make the poison a dash nerf of a completely
	// different magnitude than 30%.
	//
	// GROUND ONLY: the air ceilings in this project are the momentum model (soft cap, hard cap,
	// falloff), and clamping planar air speed to 70% of the WALK speed would not be a slow, it would
	// delete air momentum entirely — a far bigger effect than the doc describes. So the poison is a
	// ground-speed debuff. Both halves are flagged in the report as reversible tuning decisions.
	if (MoveComp->IsFalling() || Victim->IsDashing())
	{
		return;
	}

	// THE FRACTION IS NOT RE-APPLIED HERE. As of the v14 integration,
	// UTraceCharacterMovementComponent::GetMaxSpeed() already multiplies by GetSpeedMultiplier()
	// (through TraceAbilityDebuff::GetMoveSpeedMultiplier), so GetMaxSpeed() IS the slowed ceiling.
	// Multiplying by (1 - fraction) a second time here would compound 0.70 into 0.49 and quietly
	// turn a -30% slow into a -51% one. This clamp's job is now only to stop the acceleration model
	// overshooting a ceiling it already knows about.
	const float Limit = FMath::Max(1.f, MoveComp->GetMaxSpeed());

	FVector Vel = MoveComp->Velocity;
	const FVector Planar(Vel.X, Vel.Y, 0.f);
	const float PlanarSpeed = Planar.Size();
	if (PlanarSpeed > Limit && PlanarSpeed > KINDA_SMALL_NUMBER)
	{
		const FVector Capped = Planar * (Limit / PlanarSpeed);
		Vel.X = Capped.X;
		Vel.Y = Capped.Y;
		MoveComp->Velocity = Vel;
	}
}
