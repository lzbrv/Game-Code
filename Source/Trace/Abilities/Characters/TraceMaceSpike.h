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
#include "Engine/NetSerialization.h"   // FVector_NetQuantize100 / FVector_NetQuantizeNormal (C4)
#include "GameFramework/Actor.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "Gameplay/TraceFxShapes.h"    // ETraceFxBlend — every dressed piece stores what it ACHIEVED

#include "TraceMaceSpike.generated.h"

class UMaterialInstanceDynamic;
class UStaticMesh;
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

	/**
	 * HOW MANY OF THE SPIKE'S THREE DRESSED PIECES THIS MACHINE ACTUALLY DRAWS — the cone, the rope
	 * core and the rope sleeve — where "draws" means all three of: the component has a static mesh,
	 * it resolved a material (achieved blend != None) and it is visible right now.
	 *
	 * *** IT IS THE SAME COUNTER ATraceElleGate::GetDrawnBeadCount() IS, FOR THE SAME REASON. ***
	 * "The spike exists" and "the spike is visible" are different facts, and the Elle gate is this
	 * project's own proof that a harness which only asks the first one passes on a build the player
	 * is staring at an empty wall on: an InstancedStaticMeshComponent with no mesh accepts every
	 * instance and reports them all while the renderer is never told it exists. Mace has exactly that
	 * failure mode available to her — F1 was already "the rope is invisible on every client" — so the
	 * counter requires the pair of facts a piece has to satisfy to reach a screen, and
	 * Trace.Mace.SpikeVisible 0 is the red arm that drives it to 0 with every rule still working.
	 */
	int32 GetDrawnPieceCount() const;

	/** The blend each piece achieved, for logs and probes. None means "hidden rather than grey". */
	ETraceFxBlend GetSpikeBlend() const { return SpikeBlend; }
	ETraceFxBlend GetRopeCoreBlend() const { return RopeCoreBlend; }
	ETraceFxBlend GetRopeSleeveBlend() const { return RopeSleeveBlend; }

#if !UE_BUILD_SHIPPING
	/**
	 * Dev-only. One line describing what THIS machine believes about the spike — position, the
	 * replicated launch facts, which pawn the rope resolved to, and whether the rope is actually
	 * visible with a real length. It is the seam Trace.Mace.RopeProbe reads on a client, where the
	 * only alternative evidence is a screenshot of a cylinder.
	 */
	FString DebugDescribe() const;
#endif

protected:
	virtual void BeginPlay() override;

	/** REPLICATED. The wall point. Set once, at throw time, and never moved. */
	UPROPERTY(Replicated)
	FVector AnchorLocation = FVector::ZeroVector;

	/** REPLICATED. False while in flight. Nothing may pull on a spike that has not landed. */
	UPROPERTY(Replicated)
	bool bEmbedded = false;

	/**
	 * REPLICATED LAUNCH FACTS — the three numbers a client needs to DERIVE the flight (C4).
	 *
	 * The spike does not replicate movement (see the constructor), so before this a client's copy sat
	 * frozen at the muzzle until bEmbedded arrived: the throw was invisible and the rope pointed at
	 * the thrower's own chest. Replicating the flight itself would spend bandwidth on 0.4 s of
	 * interpolation; replicating the three facts that DEFINE it costs one bunch, once.
	 *
	 * This is the same shape ATraceRoxieRocket uses and the module convention §3.5 states ("no
	 * movement replication where position is derivable"). Position at time T is
	 * LaunchLocation + LaunchDirection * speed * (T - LaunchServerTime), clamped so it can never
	 * pass AnchorLocation — the anchor, not the arithmetic, is the authority on where it stops.
	 *
	 * LaunchServerTime is AGameStateBase::GetServerWorldTimeSeconds(), the one clock both machines
	 * agree on (the same one the health regen countdown reads); a client's own world time would be
	 * out by however long that client has been connected.
	 */
	UPROPERTY(Replicated)
	FVector_NetQuantize100 LaunchLocation = FVector::ZeroVector;

	UPROPERTY(Replicated)
	FVector_NetQuantizeNormal LaunchDirection = FVector::ZeroVector;

	UPROPERTY(Replicated)
	float LaunchServerTime = 0.f;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Mace")
	TObjectPtr<UStaticMeshComponent> Mesh = nullptr;

	/**
	 * The rope's CORE. Stretched between Mace and the anchor every frame on every machine; cosmetic.
	 *
	 * FX §2.4 gives it r 4 uu — i.e. 8 uu ACROSS, which is exactly bible §3.4's AA floor for a world
	 * emissive that has to read at 3,000 uu. It shipped at r 2 uu (scale 0.04), half the floor, which
	 * is a rope TSR dissolves into dashes at range.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Mace")
	TObjectPtr<UStaticMeshComponent> Rope = nullptr;

	/**
	 * The rope's SLEEVE — a fatter, additive halo around the core (FX §2.4: r 7 uu, violet I 0.35).
	 *
	 * Additive geometry writes no depth, so the sleeve cannot hide the core inside it; that is the
	 * tracer's own core/halo construction (ATraceTracer's sheath) and the reason it is used here. It
	 * is what makes a 8 uu-wide line read as a rope rather than as a wire at across-the-arena range.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Mace")
	TObjectPtr<UStaticMeshComponent> RopeSleeve = nullptr;

private:
	TWeakObjectPtr<UTraceAbilitySetMace> OwnerSet;

	float TravelSpeed = 0.f;

	/** Seconds of world time after which the actor destroys itself no matter what. See the header. */
	float BackstopLifetimeSeconds = 30.f;
	float SpawnWorldTime = 0.f;

	void UpdateRope();

	/**
	 * FX §2.4 — the DRESSING, built once per machine and idempotent (BeginPlay and Tick both call it).
	 *
	 * It is a RUNTIME build rather than constructor work for one reason: Trace.Mace.SpikeVisible has
	 * to be able to withhold the mesh assignment, and a CVar cannot be read in a CDO constructor that
	 * runs at module load. That is exactly where ATraceElleGate puts its own mesh assignment and why.
	 *
	 * A dedicated server builds nothing — shaders are not cooked for a server target — and the FACT
	 * still replicates from there; only the paint is skipped.
	 */
	void BuildDressingIfNeeded();

	/** True once BuildDressingIfNeeded has run on this machine (or deliberately skipped, on a server). */
	bool bDressingBuilt = false;

	/**
	 * AUTHORITY ONLY. FX §2.4's embed beat: one ATraceFxBurst(SpikeEmbed) at the anchor, which is the
	 * multicast — the burst actor's own replication puts the sparks and the MaceSpikeEmbed sound on
	 * every machine, frame-synced, with no RPC of this actor's own.
	 *
	 * Latched by bEmbedBurstFired so the two routes into "embedded" (the flight arriving in Tick, and
	 * InitialiseFlight's zero-travel-speed harness path) cannot fire two.
	 */
	void FireEmbedBurstIfNeeded();

	/** Server-side latch so the embed burst is spawned exactly once per spike. */
	bool bEmbedBurstFired = false;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SpikeMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RopeCoreMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RopeSleeveMID = nullptr;

	/** What each piece ACTUALLY resolved to. None ⇒ the piece is hidden, never drawn grey (§8.4). */
	ETraceFxBlend SpikeBlend = ETraceFxBlend::None;
	ETraceFxBlend RopeCoreBlend = ETraceFxBlend::None;
	ETraceFxBlend RopeSleeveBlend = ETraceFxBlend::None;

	/**
	 * CLIENTS ONLY. Puts the actor where the replicated launch facts say it is this frame (C4).
	 * A no-op once bEmbedded has arrived — the snap to AnchorLocation is the authority from then on.
	 */
	void UpdateDerivedFlight();

	/** The one clock both machines agree on; falls back to local world time before the game state exists. */
	float ServerTimeNow() const;
};
