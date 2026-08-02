// Trace — the carrier's trail. This is the signature mechanic.
//
// While a character carries the Core, this component lays a line of world-space points behind
// them. The points are delta-replicated (FFastArraySerializer) so appending one costs a single
// item on the wire rather than a full array resend.
//
// The rule the whole game hangs off (build contract §3, non-negotiable):
//
//     An ENEMY of the carrier who passes through the trail WHILE DASHING kills the CARRIER.
//     Walking or running through it does nothing at all. Teammates never trip it.
//     Dash is the only counterplay to an otherwise bullet-proof carrier.
//
// Everything below is arranged around that: the trip test is server-authoritative and swept
// (so a fast dash cannot tunnel through a thin trail between two ticks), the newest few points
// are exempt so a defender cannot simply stand on the emitter, and the kill goes through
// UTraceHealthComponent::Kill() rather than ApplyDamage() because the carrier is invulnerable.
//
// Clients only ever *read* TrailPoints: they rebuild a pooled set of cylinder meshes from it.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Math/Color.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

#include "TraceTypes.h"   // FTraceTrailPointArray

#include "TraceTrailComponent.generated.h"

class AController;
class ATraceCharacter;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Lays, replicates, evaluates and draws the Core carrier's trail.
 *
 * Attach one to every ATraceCharacter (it is dormant until SetEmitting(true)); ATraceCore drives
 * SetEmitting/ClearTrail as the Core changes hands.
 */
UCLASS(ClassGroup = (Trace), meta = (BlueprintSpawnableComponent))
class TRACE_API UTraceTrailComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UTraceTrailComponent();

	// ------------------------------------------------------------------------------------------
	// USceneComponent / UActorComponent
	// ------------------------------------------------------------------------------------------

	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ------------------------------------------------------------------------------------------
	// Replicated state
	// ------------------------------------------------------------------------------------------

	/**
	 * The trail itself, oldest point first, newest last. Server writes; clients read and draw.
	 * Delta-replicated — never assign to this wholesale on a client, and never mutate Items
	 * client-side (it would desync the fast array's ReplicationID bookkeeping).
	 */
	UPROPERTY(Replicated)
	FTraceTrailPointArray TrailPoints;

	// ------------------------------------------------------------------------------------------
	// Public API (build contract §7)
	// ------------------------------------------------------------------------------------------

	/**
	 * Server: start or stop laying trail. Starting wipes whatever was there so a new carrier
	 * never inherits the previous carrier's trail. Harmless no-op on clients.
	 */
	void SetEmitting(bool bEmit);

	/** Server: drop every point and tell clients to wipe their visuals immediately. */
	void ClearTrail();

	/** True while this component is laying trail. Replicated, so it is meaningful on clients too. */
	bool IsEmitting() const;

	/** Wipes client-side visuals the instant the trail dies, without waiting for the delta. */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastClearTrail();

	/**
	 * Called by FTraceTrailPoint's replication callbacks (defined in TraceSettings.cpp) whenever
	 * the replicated point set changed. Deliberately cheap: it only flags the visuals dirty,
	 * because the callbacks fire many times per packet and PreReplicatedRemove runs *before* the
	 * item leaves Items — so reading Items here would see stale data.
	 */
	void OnTrailPointsChanged();

private:
	// ------------------------------------------------------------------------------------------
	// Server state
	// ------------------------------------------------------------------------------------------

	/** Replicated so IsEmitting() is truthful on clients (one bit on the wire). */
	UPROPERTY(Replicated)
	bool bEmitting = false;

	/**
	 * Where every tracked character was at the end of the previous trip-test tick, used to build
	 * the swept segment. Server-only; reset whenever the trail restarts so a stale entry can
	 * never manufacture a kilometre-long sweep.
	 */
	TMap<TWeakObjectPtr<ATraceCharacter>, FVector> PreviousLocations;

	/** Scratch copy of the testable point locations, so the trip test never touches Items mid-loop. */
	TArray<FVector> TestPositions;

	// ------------------------------------------------------------------------------------------
	// Visuals (client + listen server; never created on a dedicated server)
	// ------------------------------------------------------------------------------------------

	/**
	 * Engine basic shapes, resolved by ConstructorHelpers so the cooker keeps a hard reference
	 * and they survive into a packaged build. Plain UPROPERTY (not Transient) for exactly that
	 * reason. Always null-checked — no .uasset may ever be a hard requirement (contract §2).
	 */
	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> TrailMaterial = nullptr;

	/** Pooled segment meshes. Grown on demand, hidden when unused, never recreated per frame. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> SegmentPool;

	/** Parallel to SegmentPool — one MID per pooled component, created once with it. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SegmentMaterials;

	/** Set by OnTrailPointsChanged() and by every server-side mutation. */
	bool bVisualsDirty = true;

	/** Cheap change detection, so the visuals still track even if the fast-array hooks go quiet. */
	int32 LastVisualPointCount = -1;
	FVector LastVisualHead = FVector::ZeroVector;
	FVector LastVisualTail = FVector::ZeroVector;

	/** World time until which visuals stay hidden after a MulticastClearTrail (0 = not suppressed). */
	float VisualSuppressUntilTime = 0.f;

	/** Last colour pushed to the MIDs, so we only touch them when the team actually resolves. */
	FLinearColor AppliedColor = FLinearColor::White;
	bool bColorApplied = false;

	/** Cylinder mesh metrics, read once from the asset so we never hardcode "the cube is 100uu". */
	FVector MeshHalfSize = FVector(50.0);
	FVector MeshPivotOffset = FVector::ZeroVector;
	bool bMeshMetricsCached = false;

	// ------------------------------------------------------------------------------------------
	// Internals
	// ------------------------------------------------------------------------------------------

	ATraceCharacter* GetOwnerCharacter() const;

	/** Shared clock: GameState time where available, local world time as a fallback. */
	float GetServerTimeSeconds() const;

	/** Server: expire, cap, and append points. */
	void ServerUpdateTrail();

	/** Server: swept enemy-dash trip test against the trail. */
	void ServerRunTripTest(float DeltaTime);

	/** Server: applies UTraceSettings::TrailLethality. Called only after the trip loops finish. */
	void ApplyTrailTrip(ATraceCharacter* Carrier, ATraceCharacter* Tripper);

	/** Server: every character that could possibly trip the trail this tick. */
	void GatherTrackedCharacters(TArray<ATraceCharacter*>& OutCharacters) const;

	/** Client/listen server: rebuild only when something actually changed. */
	void UpdateVisuals();
	void RebuildVisuals();
	void HideSegmentsFrom(int32 FirstIndex);
	UStaticMeshComponent* GetOrCreateSegment(int32 Index);
	void UpdateTeamColor();
	void CacheMeshMetrics();
	void DestroyVisualPool();
};
