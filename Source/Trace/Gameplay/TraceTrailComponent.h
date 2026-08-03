// Trace — the holder's TRACE. This is the signature mechanic.
//
// (The class is still called UTraceTrailComponent, and its replicated array is still TrailPoints,
// because the GameMode, the bots and the game state all name it that. The design doc calls the
// thing it draws "the trace"; the two words mean the same object in this codebase.)
//
// While a character holds the Core, this component lays a line of world-space points behind them.
// The points are delta-replicated (FFastArraySerializer) so appending one costs a single item on
// the wire rather than a full array resend.
//
// The rule the whole game hangs off (mechanics spec §3, non-negotiable):
//
//     An ENEMY of the holder who passes through the trace WHILE DASHING kills the HOLDER, and
//     takes the Core. Walking or running through it does nothing at all. Teammates never trip it.
//     Dashing through the trace is the only counterplay to an otherwise shielded holder.
//
// Two rules from spec v2 sit on top of that and are implemented here:
//
//   GRACE (§2)          After the Core changes TEAM, the trace does not begin to form for 1
//                       second. ATraceCore calls SetEmitGrace() immediately before it starts the
//                       new holder emitting; points laid inside the window are simply not laid.
//
//   INVULNERABILITY (§4) From the instant the holder INPUTS a pass until it completes or cancels,
//                       the trace cannot be broken. That fact is not stored here — it is read back
//                       out of ATraceCore::IsTraceInvulnerableFor(), which is the same replicated
//                       bool that drops the holder's shield, so the two can never disagree.
//
// VISUALLY (§3) the trace is "a blur created where your character model has passed through": a
// chain of character-shaped after-images, not a wall and not a tube. See RebuildVisuals().
//
// Clients only ever *read* TrailPoints: they rebuild a pooled set of meshes from it.

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
 * Lays, replicates, evaluates and draws the Core holder's trace.
 *
 * Attach one to every ATraceCharacter (it is dormant until SetEmitting(true)); ATraceCore drives
 * SetEmitting / SetEmitGrace / ClearTrail as the Core changes hands.
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
	 * The trace itself, oldest point first, newest last. Server writes; clients read and draw.
	 * Delta-replicated — never assign to this wholesale on a client, and never mutate Items
	 * client-side (it would desync the fast array's ReplicationID bookkeeping).
	 */
	UPROPERTY(Replicated)
	FTraceTrailPointArray TrailPoints;

	// ------------------------------------------------------------------------------------------
	// Public API
	// ------------------------------------------------------------------------------------------

	/**
	 * Server: start or stop laying the trace. Starting wipes whatever was there so a new holder
	 * never inherits the previous holder's trace. Harmless no-op on clients.
	 *
	 * Deliberately idempotent and re-assertable: ATraceCore re-calls this every tick, because
	 * several foreign systems (score reset, death handling) switch every trail in the match off
	 * wholesale and there is no pickup event left to switch the holder's back on.
	 */
	void SetEmitting(bool bEmit);

	/**
	 * Server: spec §2 — suppress point laying for @p Seconds from now.
	 *
	 * Must be called BEFORE SetEmitting(true), because that is what starts the emission and lays
	 * the first point. Stored as an absolute deadline rather than a countdown, so re-asserting
	 * emission (see SetEmitting) can neither extend nor lose the window.
	 */
	void SetEmitGrace(float Seconds);

	/** Server: drop every point and tell clients to wipe their visuals immediately. */
	void ClearTrail();

	/** True while this component is laying the trace. Replicated, so it is meaningful on clients. */
	bool IsEmitting() const;

	/** True while the trace is inside the holder's pass window and cannot be broken (§4). */
	bool IsTraceInvulnerable() const;

	/** Called by ATraceCore when the pass window opens or closes, so the visuals react at once. */
	void NotifyInvulnerabilityChanged();

	/** Wipes client-side visuals the instant the trace dies, without waiting for the delta. */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastClearTrail();

	/**
	 * Called by FTraceTrailPoint's replication callbacks (defined in TraceSettings.cpp) whenever
	 * the replicated point set changed. Deliberately cheap: it only flags the visuals dirty,
	 * because the callbacks fire many times per packet and PreReplicatedRemove runs *before* the
	 * item leaves Items — so reading Items here would see stale data.
	 */
	void OnTrailPointsChanged();

	/** Spec §3: 4 seconds. Exposed so the bots' intercept planning can agree with the truth. */
	static float GetTraceLifetimeSeconds();

private:
	// ------------------------------------------------------------------------------------------
	// Server state
	// ------------------------------------------------------------------------------------------

	/** Replicated so IsEmitting() is truthful on clients (one bit on the wire). */
	UPROPERTY(Replicated)
	bool bEmitting = false;

	/**
	 * Shared-clock deadline before which no point may be laid (spec §2's 1s transfer grace).
	 * Server-only: a client that is not told about the grace simply has nothing to draw, which is
	 * exactly what the grace looks like.
	 */
	float EmitGraceEndServerTime = 0.f;

	/**
	 * Where every tracked character was at the end of the previous trip-test tick, used to build
	 * the swept segment. Server-only; reset whenever the trace restarts so a stale entry can
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
	 * reason. Always null-checked — no .uasset may ever be a hard requirement.
	 */
	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UStaticMesh> SphereMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> TrailMaterial = nullptr;

	/**
	 * True when TrailMaterial resolved to /Game/Generated/Materials/M_TraceNeon (unlit, Color * Glow)
	 * rather than to the BasicShapeMaterial fallback. Only the neon material has a Glow parameter,
	 * and only it can be pushed past the bloom threshold.
	 */
	bool bTrailMaterialIsNeon = false;

	/**
	 * Pooled after-image meshes, THREE per trail point, interleaved: [i*3+0] legs, [i*3+1] torso,
	 * [i*3+2] head. See ETraceGhostPart and RebuildVisuals().
	 *
	 * The silhouette is sized from the same UTraceSettings TrailRadius/TrailHeight the server's trip
	 * test uses, so what you dash at is what kills you — an after-image narrower than its own lethal
	 * volume would be a trap, not a warning.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> GhostMeshes;

	/** Parallel to GhostMeshes — one MID per pooled component, created once with it. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> GhostMaterials;

	/**
	 * Parallel to GhostMeshes. The brightness each piece WOULD have with the camera far away, i.e.
	 * before ApplyProximityGlowFade() attenuates it. Split out because the rebuild is guarded by a
	 * dirty check while the proximity fade has to run every frame, so the two cannot share a pass.
	 */
	TArray<float> GhostBaseGlow;

	/** Parallel again: the proximity scale last actually written, so the pass can skip no-op writes. */
	TArray<float> GhostAppliedGlowScale;

	/** Set by OnTrailPointsChanged() and by every server-side mutation. */
	bool bVisualsDirty = true;

	/** Cheap change detection, so the visuals still track even if the fast-array hooks go quiet. */
	int32 LastVisualPointCount = -1;
	FVector LastVisualHead = FVector::ZeroVector;
	FVector LastVisualTail = FVector::ZeroVector;

	/** Last invulnerability state the visuals were built for; a change forces a rebuild. */
	bool bLastVisualInvulnerable = false;

	/** World time until which visuals stay hidden after a MulticastClearTrail (0 = not suppressed). */
	float VisualSuppressUntilTime = 0.f;

	/** Last colour pushed to the MIDs, so we only touch them when the team actually resolves. */
	FLinearColor AppliedColor = FLinearColor::White;
	bool bColorApplied = false;

	/** Source-mesh metrics, read once from each asset so we never hardcode "the cylinder is 100uu". */
	FVector CylinderHalfSize = FVector(50.0);
	FVector CylinderPivotOffset = FVector::ZeroVector;
	FVector SphereHalfSize = FVector(50.0);
	FVector SpherePivotOffset = FVector::ZeroVector;
	bool bMeshMetricsCached = false;

	// ------------------------------------------------------------------------------------------
	// Internals
	// ------------------------------------------------------------------------------------------

	ATraceCharacter* GetOwnerCharacter() const;

	/** Shared clock: GameState time where available, local world time as a fallback. */
	float GetServerTimeSeconds() const;

	/** Server: expire, cap, and append points. */
	void ServerUpdateTrail();

	/** Server: swept enemy-dash trip test against the trace. */
	void ServerRunTripTest(float DeltaTime);

	/** Server: applies UTraceSettings::TrailLethality. Called only after the trip loops finish. */
	void ApplyTrailTrip(ATraceCharacter* Holder, ATraceCharacter* Tripper);

	/** Server: every character that could possibly trip the trace this tick. */
	void GatherTrackedCharacters(TArray<ATraceCharacter*>& OutCharacters) const;

	/** Client/listen server: rebuild only when something actually changed. */
	void UpdateVisuals();
	void RebuildVisuals();
	void HideGhostsFrom(int32 FirstGhostIndex);

	/**
	 * Local, cosmetic anti-whiteout guard: dims an after-image's emissive as the LOCAL camera gets
	 * close to it, so an eye inside the trace does not take an unattenuated unlit emissive slab at
	 * full frame width. Never changes the lethal volume. Runs every frame — see the implementation.
	 */
	void ApplyProximityGlowFade();

	/** Grows the pool to cover ghost @p GhostIndex. False once the pool cap is hit. */
	bool EnsureGhost(int32 GhostIndex);

	/** Shared setup for one pooled after-image piece. */
	UStaticMeshComponent* CreatePooledMesh(UStaticMesh* SourceMesh, UMaterialInstanceDynamic*& OutMaterial);

	void UpdateTeamColor();
	void CacheMeshMetrics();
	void DestroyVisualPool();
};
