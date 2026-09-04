// Trace — the ability framework's world-level plumbing.
//
// Three jobs, and it exists so that none of them has to be a change to a file this slice does not
// own (the contract for this pass is strict file ownership; see the report's cross-file section):
//
//   1. ATTACHMENT. Puts a UTraceAbilityComponent on every player's PlayerState as it appears, on
//      the authority, with replication turned on so clients receive it. When the component is later
//      promoted to a CreateDefaultSubobject on ATracePlayerState — which is where it belongs
//      permanently — this step becomes a no-op automatically, because it only ever adds a component
//      that is not already there.
//
//   2. HALF TIME. Spec §5: "They should all reset at halftime." Watches ATraceGameState for the
//      half-time break beginning (and for the half index advancing, which is the same event seen
//      from the other side) and calls UTraceAbilityComponent::OnHalfTime() on every player exactly
//      once per break. ATraceGameMode may call NotifyHalfTime() directly instead; the edge detector
//      is idempotent, so having both is safe and one of them being removed later is also safe.
//
//   3. THE DISABLE TOGGLE. Spec §3's "include a toggle in game settings to turn off all
//      characters". When it is off this subsystem forces every player back to
//      ETraceCharacterId::None — the default characterless Mannequin — and keeps doing so, so there
//      is no ordering between "the mode replicated" and "somebody picked a character".
//
// Server-authoritative throughout. On a client the subsystem exists but does nothing except read.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceAbilityTypes.h"

#include "TraceAbilityWorldSubsystem.generated.h"

class AGameModeBase;
class APlayerController;
class APlayerState;
class UTraceAbilityComponent;

UCLASS()
class TRACE_API UTraceAbilityWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// UWorldSubsystem
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return true; }

	/**
	 * AUTHORITY ONLY. Reset every player's cooldowns and transient ability state — spec §5's "They
	 * should all reset at halftime."
	 *
	 * Idempotent within one break: calling it twice for the same half-time does the work once.
	 * Exposed so ATraceGameMode can call it at the exact frame it blows the whistle rather than
	 * relying on this subsystem's edge detector; both paths are supported on purpose.
	 */
	void NotifyHalfTime();

	/** The subsystem for @p WorldContextObject, or null. */
	static UTraceAbilityWorldSubsystem* Get(const UObject* WorldContextObject);

	/** Every ability component in the world, in PlayerArray order. Cheap enough for 10 players. */
	static void GatherAllComponents(const UObject* WorldContextObject, TArray<UTraceAbilityComponent*>& Out);

	/** Diagnostics: how many half-time resets this world has performed. */
	int32 GetHalfTimeResetCount() const { return HalfTimeResetCount; }

private:
	/** Attaches immediately on join, so the select screen never asks before anybody has a component. */
	void HandlePostLogin(AGameModeBase* GameModeBase, APlayerController* NewPlayer);

	/** Adds the component to any PlayerState that lacks one. Authority only. */
	void EnsureComponentsAttached();

	/** Rising edge of ATraceGameState::bHalfTimeBreak, and any change of CurrentHalf. */
	void PollHalfTime();

	/** Spec §3: characters switched off forces everybody to the characterless Mannequin. */
	void EnforceModeAFreeze();

	FDelegateHandle PostLoginHandle;

	bool  bWasHalfTimeBreak = false;
	int32 LastSeenHalf = 1;
	int32 HalfTimeResetCount = 0;

	/** Attachment and the mode-A sweep are cheap but not free; they do not need 60 Hz. */
	float PollAccumulator = 0.f;
};
