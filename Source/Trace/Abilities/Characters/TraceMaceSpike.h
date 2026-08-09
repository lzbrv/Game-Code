// Trace — Mace's roped spike (spec v14 §6).
//
// Verbatim: "throws a roped spike in her aim direction for a medium distance. On hitting a WALL it
// embeds for 2 s. Reactivating pulls her toward it at the momentum ceiling (the air-strafe hard cap).
// Any movement input cancels the pull and removes the spike. She obeys normal physics while pulled —
// bouncing off a wall cancels it. She can shoot while pulled, and be shot."
//
// THIS ACTOR IS THE ANCHOR AND THE VISUAL, AND NOTHING ELSE. Every rule above lives in
// UTraceAbilitySetMace, because every one of them is about Mace's pawn rather than about the spike:
// the pull writes her velocity, the cancel reads her input, the bounce reads her displacement. An
// actor that also owned the pull would be a second place her movement is decided.
//
// It does own two things, both of which are about the spike itself:
//   * the FLIGHT. The anchor is resolved by a sweep at throw time, so the outcome (wall / no wall) is
//     decided in one frame on the server and cannot change mid-flight; the actor then covers the
//     distance at MaceSpikeTravelSpeed so the throw is visible. Nothing may pull until it lands.
//   * a BACKSTOP expiry, so a spike whose owner disconnected mid-flight cannot outlive the match.
//
// COLLISION: none at all. The spike is decoration around a point. Giving it a collider would let it
// block a bullet, break an Oyster jar, or become a movement base — three bugs for no gain.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceMaceSpike.generated.h"

class UStaticMeshComponent;
class UTraceAbilitySetMace;

UCLASS()
class TRACE_API ATraceMaceSpike : public AActor
{
	GENERATED_BODY()

public:
	ATraceMaceSpike();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * SERVER ONLY. @p InAnchor is where the sweep said it will stick; the actor flies there from its
	 * spawn transform and only then reports itself embedded.
	 *
	 * @param InOwnerSet    Mace, so the actor can tell her it landed. Weakly held.
	 * @param InAnchor      the resolved wall point.
	 * @param InTravelSpeed uu/s. Zero or less means "already there" (used by the harness).
	 */
	void InitialiseFlight(UTraceAbilitySetMace* InOwnerSet, const FVector& InAnchor, float InTravelSpeed,
	                      float InBackstopLifetimeSeconds);

	/** Where the spike is stuck (or heading). Replicated so a client's rope draws in the right place. */
	UFUNCTION(BlueprintPure, Category = "Trace|Mace")
	FVector GetAnchorLocation() const { return AnchorLocation; }

	/** True once the flight has finished and the spike is in the wall. Replicated. */
	UFUNCTION(BlueprintPure, Category = "Trace|Mace")
	bool IsEmbedded() const { return bEmbedded; }

protected:
	virtual void BeginPlay() override;

	/** REPLICATED. The wall point. Set once, at throw time, and never moved. */
	UPROPERTY(Replicated)
	FVector AnchorLocation = FVector::ZeroVector;

	/** REPLICATED. False while in flight. Nothing may pull on a spike that has not landed. */
	UPROPERTY(Replicated)
	bool bEmbedded = false;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Mace")
	TObjectPtr<UStaticMeshComponent> Mesh = nullptr;

	/** The rope. Stretched between Mace and the anchor every frame on every machine; cosmetic only. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Mace")
	TObjectPtr<UStaticMeshComponent> Rope = nullptr;

private:
	TWeakObjectPtr<UTraceAbilitySetMace> OwnerSet;

	float TravelSpeed = 0.f;

	/** Seconds of world time after which the actor destroys itself no matter what. See the header. */
	float BackstopLifetimeSeconds = 30.f;
	float SpawnWorldTime = 0.f;

	void UpdateRope();
};
