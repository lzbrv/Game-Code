// Trace — per-character health, damage and death.

#include "Gameplay/TraceHealthComponent.h"

#include "Net/UnrealNetwork.h"

#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "Math/UnrealMathUtility.h"
#include "Trace.h"
#include "TraceSettings.h"

UTraceHealthComponent::UTraceHealthComponent()
{
	// Nothing to tick: health only changes in response to events.
	PrimaryComponentTick.bCanEverTick = false;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	SetIsReplicatedByDefault(true);

	bDeathBroadcast = 0;
}

void UTraceHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// Settings are read at runtime rather than cached in the constructor: UTraceSettings is a
	// config CDO and designers change MaxHealth live.
	if (HasAuthority())
	{
		Health = GetMaxHealth();
		bDeathBroadcast = 0;
	}
}

void UTraceHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UTraceHealthComponent, Health);
}

bool UTraceHealthComponent::HasAuthority() const
{
	const AActor* Owner = GetOwner();
	return Owner != nullptr && Owner->HasAuthority();
}

float UTraceHealthComponent::GetMaxHealth() const
{
	return FMath::Max(1.f, UTraceSettings::Get().MaxHealth);
}

bool UTraceHealthComponent::IsAlive() const
{
	return Health > 0.f;
}

bool UTraceHealthComponent::IsInvulnerable() const
{
	// Invulnerability is a property of *holding the Core*, not of this component, so it is derived
	// rather than stored — there is no way for the two to fall out of sync.
	if (const ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner()))
	{
		// Spec §4, THE RISK BEAT: the shield drops for the whole pass window. Both halves of the
		// beat (this, and the trace hardening) read the SAME replicated bool on ATraceCore, so they
		// cannot drift apart, and a cancel restores both in one statement.
		//
		// BOTH TERMS NOW ASK THE CORE. This used to be `OwningCharacter->IsCarrier() && ...`, mixing
		// the pawn's replicated presentation mirror with the Core's authoritative pointer inside one
		// expression — two independently replicated facts deciding one damage rule. IsCoreHolder()
		// reads Core->Carrier, the same object the second term reads, so a frame in which the mirror
		// and the Core disagree can no longer make a player briefly invulnerable or briefly shootable.
		return ATraceCore::IsCoreHolder(OwningCharacter)
			&& !ATraceCore::IsShieldSuppressedFor(OwningCharacter);
	}
	return false;
}

float UTraceHealthComponent::GetHealthPercent() const
{
	return FMath::Clamp(Health / GetMaxHealth(), 0.f, 1.f);
}

void UTraceHealthComponent::ApplyDamage(float Amount, AController* Instigator, FName Cause)
{
	if (!HasAuthority() || !IsAlive())
	{
		return;
	}

	// Network-sourced numbers reach this via UTraceWeaponComponent; validate instead of check().
	if (!FMath::IsFinite(Amount) || Amount <= 0.f)
	{
		return;
	}

	if (IsInvulnerable())
	{
		// The carrier is bullet-proof. Dropping the hit here (rather than at the weapon) means
		// every future damage source inherits the rule for free.
		UE_LOG(LogTraceGame, Verbose, TEXT("[%s] Damage %.1f ignored: carrier is invulnerable"),
			*GetNameSafe(GetOwner()), Amount);
		return;
	}

	Health = FMath::Clamp(Health - Amount, 0.f, GetMaxHealth());

	// Replication callbacks never fire on the authority; call it directly so a listen server's
	// local HUD/effects behave exactly like a remote client's.
	OnRep_Health();

	if (Health <= 0.f)
	{
		BroadcastDeath(Instigator, Cause);
	}
}

void UTraceHealthComponent::Kill(AController* Instigator, FName Cause)
{
	if (!HasAuthority() || !IsAlive())
	{
		return;
	}

	// Deliberately no IsInvulnerable() check: the trail is the counterplay to the carrier, and
	// this is the door it comes through.
	Health = 0.f;
	OnRep_Health();

	BroadcastDeath(Instigator, Cause);
}

void UTraceHealthComponent::ResetHealth()
{
	if (!HasAuthority())
	{
		// Clients get the reset through replication; a local write would only be overwritten.
		return;
	}

	Health = GetMaxHealth();
	bDeathBroadcast = 0;

	OnRep_Health();
}

void UTraceHealthComponent::BroadcastDeath(AController* Instigator, FName Cause)
{
	if (bDeathBroadcast)
	{
		return;
	}
	bDeathBroadcast = 1;

	UE_LOG(LogTraceGame, Log, TEXT("[%s] died (cause '%s', killer '%s')"),
		*GetNameSafe(GetOwner()), *Cause.ToString(), *GetNameSafe(Instigator));

	OnDeath.Broadcast(GetOwner(), Instigator, Cause);
}

void UTraceHealthComponent::OnRep_Health()
{
	// The HUD polls GetHealthPercent() every frame, so there is nothing to push there. What this
	// hook *is* responsible for is the alive/dead presentation of the pawn.
	//
	// Driving it from replicated health rather than from a death multicast is deliberate: this fires
	// on the server (called by hand after every write), on every connected client, and on a client
	// that joins after the death — because a replicated value that differs from the class default
	// runs its OnRep on initial receive. A multicast would miss that last case and leave a late
	// joiner colliding with a corpse the server has already made intangible.
	if (ATraceCharacter* OwningCharacter = Cast<ATraceCharacter>(GetOwner()))
	{
		OwningCharacter->SetDeadPresentation(!IsAlive());
	}
}
