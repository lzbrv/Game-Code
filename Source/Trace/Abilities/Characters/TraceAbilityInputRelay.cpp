// Trace — the interim input path for the two ability keys. See the header for why it exists.

#include "Abilities/Characters/TraceAbilityInputRelay.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Settings/TraceUserSettings.h"
#include "Trace.h"

UTraceAbilityInputRelay::UTraceAbilityInputRelay()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// A key poll is one IsInputKeyDown per key per frame on ONE machine's ONE local player. There is
	// nothing to spread over a slower cadence: a 5 Hz poll would eat presses.
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	SetIsReplicated(true);
}

// =================================================================================================
// Attachment
// =================================================================================================

UTraceAbilityInputRelay* UTraceAbilityInputRelay::Find(const APlayerState* ForPlayerState)
{
	return (ForPlayerState != nullptr) ? ForPlayerState->FindComponentByClass<UTraceAbilityInputRelay>() : nullptr;
}

UTraceAbilityInputRelay* UTraceAbilityInputRelay::EnsureOn(APlayerState* ForPlayerState)
{
	if (ForPlayerState == nullptr || !ForPlayerState->HasAuthority())
	{
		return nullptr;
	}

	if (UTraceAbilityInputRelay* Existing = ForPlayerState->FindComponentByClass<UTraceAbilityInputRelay>())
	{
		return Existing;
	}

	UTraceAbilityInputRelay* Relay = NewObject<UTraceAbilityInputRelay>(
		ForPlayerState, UTraceAbilityInputRelay::StaticClass(), TEXT("TraceAbilityInputRelay"));

	// ORDER MATTERS, exactly as it does for UTraceAbilityComponent: the engine samples bReplicates
	// when the component registers, so a component that registers unreplicated and is flipped
	// afterwards can miss the actor channel it was supposed to be created on — and then the client's
	// Server RPC has nowhere to land.
	Relay->SetIsReplicated(true);
	Relay->RegisterComponent();

	UE_LOG(LogTraceGame, Log, TEXT("[AbilityInput] Attached the interim key relay to %s (activate=%s secondary=%s, realBinds=%d)."),
		*GetNameSafe(ForPlayerState),
		*ResolveActivateKey().ToString(), *ResolveSecondaryKey().ToString(),
		AreRealBindsInstalled() ? 1 : 0);

	return Relay;
}

// =================================================================================================
// Key resolution — the player's bind first, the doc's default second
// =================================================================================================

FKey UTraceAbilityInputRelay::FindBindingByConfigId(const TCHAR* ConfigId)
{
	if (ConfigId == nullptr)
	{
		return FKey();
	}

	const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();
	for (const FTraceInputActionInfo& Entry : Table)
	{
		if (Entry.ConfigId != nullptr && FCString::Stricmp(Entry.ConfigId, ConfigId) == 0)
		{
			return UTraceUserSettings::Get().GetKey(Entry.Action);
		}
	}
	return FKey();
}

FKey UTraceAbilityInputRelay::ResolveActivateKey()
{
	const FKey Bound = FindBindingByConfigId(TEXT("Ability"));
	return Bound.IsValid() ? Bound : EKeys::E;
}

FKey UTraceAbilityInputRelay::ResolveSecondaryKey()
{
	const FKey Bound = FindBindingByConfigId(TEXT("AbilitySecondary"));
	return Bound.IsValid() ? Bound : EKeys::V;
}

bool UTraceAbilityInputRelay::AreRealBindsInstalled()
{
	return FindBindingByConfigId(TEXT("Ability")).IsValid()
		&& FindBindingByConfigId(TEXT("AbilitySecondary")).IsValid();
}

// =================================================================================================
// The poll
// =================================================================================================

void UTraceAbilityInputRelay::TickComponent(float DeltaTime, ELevelTick TickType,
                                            FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const APlayerState* MyState = Cast<APlayerState>(GetOwner());
	if (MyState == nullptr)
	{
		return;
	}

	// THE POLL IS OFF THE MOMENT THE REAL BINDS EXIST. ATracePlayerController now binds
	// ETraceInputAction::Ability / AbilitySecondary and routes them through the same two routers
	// below, so polling as well would deliver every press twice. The activated ability would absorb
	// the duplicate in its cooldown, but Mace's V is a HOLD and a second press/release pair is a
	// visible stutter — and "absorbed by another rule" is not a reason to send a wrong event.
	//
	// Kept rather than deleted so a build that loses those two rows (a bad merge on the input table)
	// degrades to a playable ability instead of an unreachable one, and says so in the log once.
	if (AreRealBindsInstalled())
	{
		if (!bLoggedPollDisabled)
		{
			bLoggedPollDisabled = true;
			UE_LOG(LogTraceGame, Log,
				TEXT("[AbilityInput] Real binds are installed (%s / %s) — the interim key poll is OFF; "
				     "ATracePlayerController owns the presses."),
				*ResolveActivateKey().ToString(), *ResolveSecondaryKey().ToString());
		}
		return;
	}

	// ONLY the machine the player is sitting at polls keys. On a dedicated server this is nobody; on
	// a listen server it is the host, whose presses execute the Server RPCs locally.
	APlayerController* LocalPC = Cast<APlayerController>(MyState->GetOwner());
	if (LocalPC == nullptr)
	{
		LocalPC = Cast<APlayerController>(MyState->GetOwningController());
	}
	if (LocalPC == nullptr || !LocalPC->IsLocalController())
	{
		return;
	}

	// A key poll must not fire while the player is typing into the character-select screen or the
	// options overlay. Both suppress game input through the same flag the controller already owns.
	if (LocalPC->bShowMouseCursor)
	{
		bPolledActivateDown = false;
		bPolledSecondaryDown = false;
		return;
	}

	const bool bActivateDown = LocalPC->IsInputKeyDown(ResolveActivateKey());
	if (bActivateDown != bPolledActivateDown)
	{
		bPolledActivateDown = bActivateDown;
		HandleActivateEdge(bActivateDown);
	}

	const bool bSecondaryDown = LocalPC->IsInputKeyDown(ResolveSecondaryKey());
	if (bSecondaryDown != bPolledSecondaryDown)
	{
		bPolledSecondaryDown = bSecondaryDown;
		HandleSecondaryEdge(bSecondaryDown);
	}
}

void UTraceAbilityInputRelay::HandleActivateEdge(bool bDown)
{
	if (!bDown)
	{
		return;   // the activated ability is a press, not a hold
	}
	RouteActivatePressed(Cast<APlayerState>(GetOwner()));
}

void UTraceAbilityInputRelay::HandleSecondaryEdge(bool bDown)
{
	RouteSecondaryEdge(Cast<APlayerState>(GetOwner()), bDown);
}

// =================================================================================================
// THE ROUTERS — the one definition of what a press means, shared by the key bind and the poll
// =================================================================================================

void UTraceAbilityInputRelay::RouteActivatePressed(APlayerState* ForPlayerState)
{
	UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(ForPlayerState);
	if (Comp == nullptr)
	{
		return;
	}

	// MACE'S REACTIVATION comes first. See the header: TryActivate() refuses while the cooldown the
	// throw just started is running, so the second press of Spike cannot reach her through it.
	if (UTraceAbilitySetMace* Mace = Comp->GetAbilitySetAs<UTraceAbilitySetMace>())
	{
		if (Mace->IsSpikeReadyToPull())
		{
			Mace->RequestSpikePull();      // predicted locally (and authoritative on a listen server)

			// The authoritative half, only when this machine is NOT the authority — on a host the
			// call above already was it, and a Server RPC executes locally.
			const bool bAuthority = (ForPlayerState != nullptr) && ForPlayerState->HasAuthority();
			if (!bAuthority)
			{
				if (UTraceAbilityInputRelay* Relay = Find(ForPlayerState))
				{
					Relay->ServerRequestSpikePull();
				}
				else
				{
					// The relay is attached on the SERVER when Mace equips and replicates from
					// there, so this is the one-RTT window right after the swap. Saying so beats
					// swallowing the press silently.
					UE_LOG(LogTraceGame, Warning,
						TEXT("[AbilityInput] Spike reactivation had no relay on %s yet — press dropped."),
						*GetNameSafe(ForPlayerState));
				}
			}
			return;
		}
	}

	Comp->HandleActivatePressed();   // predicts locally and sends ServerTryActivate itself
}

void UTraceAbilityInputRelay::RouteSecondaryEdge(APlayerState* ForPlayerState, bool bDown)
{
	UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(ForPlayerState);
	if (Comp == nullptr)
	{
		return;
	}

	// Predict locally so the suspend has no round trip in it — "hold V to stop falling" with 80 ms of
	// lag before the fall stops is a different ability.
	if (bDown)
	{
		Comp->HandleSecondaryPressed();
	}
	else
	{
		Comp->HandleSecondaryReleased();
	}

	// The RPC lives on the COMPONENT, not on this relay, so V works for a character that never
	// attaches one. Skipped on the authority for the reason above.
	const bool bAuthority = (ForPlayerState != nullptr) && ForPlayerState->HasAuthority();
	if (!bAuthority)
	{
		Comp->ServerSetSecondaryHeld(bDown);
	}
}

// =================================================================================================
// Client -> server
// =================================================================================================

void UTraceAbilityInputRelay::ServerSetSecondaryHeld_Implementation(bool bHeld)
{
	UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(GetOwner());
	if (Comp == nullptr)
	{
		return;
	}

	if (bHeld)
	{
		Comp->HandleSecondaryPressed();
	}
	else
	{
		Comp->HandleSecondaryReleased();
	}
}

void UTraceAbilityInputRelay::ServerRequestSpikePull_Implementation()
{
	UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(GetOwner());
	if (Comp == nullptr)
	{
		return;
	}

	if (UTraceAbilitySetMace* Mace = Comp->GetAbilitySetAs<UTraceAbilitySetMace>())
	{
		Mace->RequestSpikePull();
	}
}
