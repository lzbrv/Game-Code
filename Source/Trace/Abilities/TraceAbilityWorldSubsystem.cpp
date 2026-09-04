// Trace — attachment, half time, and the mode-A freeze. See the header for why all three live here.

#include "Abilities/TraceAbilityWorldSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameModeBase.h"   // FGameModeEvents lives with the game mode, not in its own header

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceGameState.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace
{
	/**
	 * Attachment and the mode-A sweep at 5 Hz.
	 *
	 * Both are "has anything appeared / has the mode changed" polls over at most ten PlayerStates.
	 * At 60 Hz that is 600 FindComponentByClass calls a second for a question whose answer changes
	 * a handful of times a match. Half time is polled on the SAME cadence and that is fine: the
	 * break lasts seconds and a reset landing 200 ms into it is indistinguishable from one landing
	 * on the frame.
	 */
	constexpr float PollIntervalSeconds = 0.2f;
}

bool UTraceAbilityWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Game and PIE only. Not Editor, not EditorPreview, not Inactive — a subsystem that attaches
	// replicated components inside an editor preview world is a crash waiting for a blueprint
	// thumbnail to render.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UTraceAbilityWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (const ATraceGameState* TraceGS = InWorld.GetGameState<ATraceGameState>())
	{
		bWasHalfTimeBreak = TraceGS->IsHalfTimeBreak();
		LastSeenHalf = TraceGS->GetCurrentHalf();
	}

	// ATTACH ON JOIN, NOT ONLY ON THE 5 Hz SWEEP.
	//
	// The sweep alone leaves a window of up to one poll interval in which a player exists and has no
	// ability component, and the character-select screen (spec §3) opens during PostLogin — inside
	// that window. A select screen that asks "which characters are free" before anybody has a
	// component gets "all of them" and the answer is a lie. The sweep stays as the backstop for
	// PlayerStates that appear by other routes (bots being filled in, seamless travel).
	PostLoginHandle = FGameModeEvents::GameModePostLoginEvent.AddUObject(
		this, &UTraceAbilityWorldSubsystem::HandlePostLogin);

	EnsureComponentsAttached();
}

void UTraceAbilityWorldSubsystem::Deinitialize()
{
	if (PostLoginHandle.IsValid())
	{
		FGameModeEvents::GameModePostLoginEvent.Remove(PostLoginHandle);
		PostLoginHandle.Reset();
	}

	Super::Deinitialize();
}

void UTraceAbilityWorldSubsystem::HandlePostLogin(AGameModeBase* GameModeBase, APlayerController* NewPlayer)
{
	// The event is global to the process, so a second PIE world's login must not be handled here.
	if (GameModeBase == nullptr || GameModeBase->GetWorld() != GetWorld())
	{
		return;
	}
	EnsureComponentsAttached();
}

TStatId UTraceAbilityWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTraceAbilityWorldSubsystem, STATGROUP_Tickables);
}

UTraceAbilityWorldSubsystem* UTraceAbilityWorldSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* WorldPtr = (WorldContextObject != nullptr) ? WorldContextObject->GetWorld() : nullptr;
	return (WorldPtr != nullptr) ? WorldPtr->GetSubsystem<UTraceAbilityWorldSubsystem>() : nullptr;
}

void UTraceAbilityWorldSubsystem::GatherAllComponents(const UObject* WorldContextObject,
                                                      TArray<UTraceAbilityComponent*>& Out)
{
	Out.Reset();

	const UWorld* WorldPtr = (WorldContextObject != nullptr) ? WorldContextObject->GetWorld() : nullptr;
	const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	if (BaseState == nullptr)
	{
		return;
	}

	for (APlayerState* Entry : BaseState->PlayerArray)
	{
		if (Entry == nullptr)
		{
			continue;
		}
		if (UTraceAbilityComponent* Comp = Entry->FindComponentByClass<UTraceAbilityComponent>())
		{
			Out.Add(Comp);
		}
	}
}

void UTraceAbilityWorldSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	PollAccumulator += DeltaTime;
	if (PollAccumulator < PollIntervalSeconds)
	{
		return;
	}
	PollAccumulator = 0.f;

	const UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr || WorldPtr->GetGameState() == nullptr)
	{
		return;
	}

	// Half time is polled on EVERY machine's copy of this subsystem, but OnHalfTime() itself is
	// authority-gated, so a client's edge detector runs and then does nothing. That is on purpose:
	// the edge state stays in step on both sides, so a listen server that later becomes a client
	// (it cannot, but the code should not depend on that) does not double-fire.
	PollHalfTime();

	const bool bAuthority = WorldPtr->GetNetMode() != NM_Client;
	if (!bAuthority)
	{
		return;
	}

	EnsureComponentsAttached();
	EnforceModeAFreeze();
}

void UTraceAbilityWorldSubsystem::EnsureComponentsAttached()
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	if (BaseState == nullptr || WorldPtr->GetNetMode() == NM_Client)
	{
		return;
	}

	for (APlayerState* Entry : BaseState->PlayerArray)
	{
		if (Entry == nullptr || !Entry->HasAuthority())
		{
			continue;
		}

		if (Entry->FindComponentByClass<UTraceAbilityComponent>() != nullptr)
		{
			continue;   // already has one — including the day it becomes a CreateDefaultSubobject
		}

		UTraceAbilityComponent* Comp = NewObject<UTraceAbilityComponent>(Entry, UTraceAbilityComponent::StaticClass(),
			TEXT("TraceAbilityComponent"));

		// ORDER MATTERS. SetIsReplicated BEFORE RegisterComponent: the engine samples the flag when
		// the component registers in order to add it to the owner's replicated-component list, and a
		// component that registers unreplicated and is flipped afterwards can miss the actor channel
		// it was supposed to be created on.
		Comp->SetIsReplicated(true);
		Comp->RegisterComponent();

		UE_LOG(LogTraceGame, Log, TEXT("[Ability] Attached ability component to %s (bot=%d)."),
			*GetNameSafe(Entry), Entry->IsABot() ? 1 : 0);
	}
}

void UTraceAbilityWorldSubsystem::PollHalfTime()
{
	const UWorld* WorldPtr = GetWorld();
	const ATraceGameState* TraceGS = (WorldPtr != nullptr) ? WorldPtr->GetGameState<ATraceGameState>() : nullptr;
	if (TraceGS == nullptr)
	{
		return;
	}

	const bool  bBreakNow = TraceGS->IsHalfTimeBreak();
	const int32 HalfNow   = TraceGS->GetCurrentHalf();

	// TWO EDGES, EITHER ONE SUFFICIENT.
	//
	// The break beginning is the natural signal, and it is what a player sees. But
	// ATraceGameMode's deferred half time (spec v9 §11) can move sides and advance the half in the
	// same frame it opens the break, and a 5 Hz poll that happened to sample either side of a very
	// short break would miss the flag entirely. Watching the half INDEX as well means the reset
	// cannot be skipped: the index is monotonic and latched, so even a break that came and went
	// between two polls still leaves evidence.
	const bool bBreakStarted = bBreakNow && !bWasHalfTimeBreak;
	const bool bHalfAdvanced = HalfNow != LastSeenHalf;

	bWasHalfTimeBreak = bBreakNow;
	LastSeenHalf = HalfNow;

	if (bBreakStarted || bHalfAdvanced)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Ability] Half-time edge (breakStarted=%d halfAdvanced=%d, now half %d) — resetting every cooldown (spec §5)."),
			bBreakStarted ? 1 : 0, bHalfAdvanced ? 1 : 0, HalfNow);
		NotifyHalfTime();
	}
}

void UTraceAbilityWorldSubsystem::NotifyHalfTime()
{
	const UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr || WorldPtr->GetNetMode() == NM_Client)
	{
		return;
	}

	TArray<UTraceAbilityComponent*> Components;
	GatherAllComponents(this, Components);

	for (UTraceAbilityComponent* Comp : Components)
	{
		Comp->OnHalfTime();
	}

	++HalfTimeResetCount;

	UE_LOG(LogTraceGame, Display, TEXT("[Ability] Half-time reset #%d applied to %d players."),
		HalfTimeResetCount, Components.Num());
}

void UTraceAbilityWorldSubsystem::EnforceModeAFreeze()
{
	// SPEC §2. "Do not implement abilities or characters into what was game mode a (endzones)" —
	// with the [ASSUMPTION] that switching characters off forces every player to the default characterless
	// Mannequin. This is that force, and it is a SWEEP rather than a one-shot on purpose: there is
	// no ordering guarantee between the mode being published and a late joiner's component arriving,
	// and "characters off but somebody has Rocco" must not be reachable by any interleaving.
	//
	// It is also the enforcement for the §3 disable toggle, because AreCharactersEnabled() answers
	// for both and a designer flipping the toggle mid-match should see it take effect.
	if (UTraceAbilityComponent::AreCharactersEnabled(this))
	{
		return;
	}

	TArray<UTraceAbilityComponent*> Components;
	GatherAllComponents(this, Components);

	for (UTraceAbilityComponent* Comp : Components)
	{
		if (Comp->GetCharacterId() != ETraceCharacterId::None)
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[Ability] Characters are OFF (the disable toggle) — forcing %s back to the default Mannequin."),
				*GetNameSafe(Comp->GetOwner()));
			Comp->ServerSetCharacter(ETraceCharacterId::None);
		}
	}
}
