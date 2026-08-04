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
// Three rules sit on top of that and are implemented here:
//
//   GRACE (§2)          After the Core changes TEAM, the trace does not begin to form for
//                       0.4 seconds (spec v3 §1 shortened it from 1.0). ATraceCore calls
//                       SetEmitGrace() immediately before it starts the new holder emitting;
//                       points laid inside the window are simply not laid.
//
//                       IT DELAYS FORMATION AND NOTHING ELSE. A segment that has already been laid
//                       is lethal, grace or no grace. Making the grace suppress the TRIP TEST
//                       instead of the point laying is a bug this project has already shipped once
//                       and fixed once — see ServerRunTripTest.
//
//   PASS WINDOW (§4)    From the instant the holder INPUTS a pass until it completes or cancels,
//                       the trace cannot be broken. That fact is not stored here — it is read back
//                       out of ATraceCore::IsTraceInvulnerableFor(), which is the same replicated
//                       bool that drops the holder's shield, so the two can never disagree.
//
//   PARRY (v3 §3)       A carrier-only, 0.1s window of trace invulnerability on a 1.5s cooldown,
//                       during which the ENTIRE trace turns red. Unlike the pass window it does NOT
//                       drop the shield. The window lives here (see ParryEndServerTime); the
//                       tunables, the entry point and the debug commands live in Gameplay/TraceParry.h,
//                       whose file header explains the split and — importantly — exactly how the two
//                       invulnerability sources compose without clobbering each other. Read it
//                       before changing either.
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

#include "TraceTypes.h"          // FTraceTrailPointArray
#include "Gameplay/TraceParry.h" // ETraceParryRefusal (plain header, no reflection)

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
	 * Server: spec §2 — suppress POINT LAYING for @p Seconds from now.
	 *
	 * Must be called BEFORE SetEmitting(true), because that is what starts the emission and lays
	 * the first point. Stored as an absolute deadline rather than a countdown, so re-asserting
	 * emission (see SetEmitting) can neither extend nor lose the window.
	 *
	 * v3 §1 SHORTENED THE TURNOVER GRACE FROM 1.0s TO 0.4s. The request is capped here rather than
	 * at the single call site (ATraceCore's TraceCoreTuning::TransferGraceSeconds, still 1.0) for
	 * exactly the reason GetTraceLifetimeSeconds() caps the lifetime: that constant lives in a file
	 * this slice does not own, and a stale value there must not be able to reinstate the old rule.
	 * A caller asking for LESS still wins. Tune with Trace.Trail.TurnoverGrace.
	 *
	 * IT DELAYS FORMATION, NOT LETHALITY. Nothing in the grace path touches the trip test.
	 */
	void SetEmitGrace(float Seconds);

	/** The turnover grace actually in force, in seconds. v3 §1: 0.4. */
	static float GetTurnoverGraceSeconds();

	/** Server: drop every point and tell clients to wipe their visuals immediately. */
	void ClearTrail();

	/** True while this component is laying the trace. Replicated, so it is meaningful on clients. */
	bool IsEmitting() const;

	// ------------------------------------------------------------------------------------------
	// TRACE INVULNERABILITY — TWO INDEPENDENT SOURCES. See Gameplay/TraceParry.h's file header.
	//
	// IsTraceInvulnerable() is the OR of the two and is what the trip test and the visuals ask.
	// The two component questions stay separately answerable on purpose: the HUD, the logs and any
	// future reader need to be able to say WHICH one is protecting a trace, and a single merged
	// bool would make "the parry ate my pass window" impossible to diagnose.
	// ------------------------------------------------------------------------------------------

	/** True while the trace cannot be broken FOR ANY REASON: pass window, parry, or a debug force. */
	bool IsTraceInvulnerable() const;

	/**
	 * Source 1 (§4). True while the holder's PASS window is open.
	 *
	 * Read straight back out of ATraceCore, which is also what drops the holder's shield — the two
	 * are one fact read twice. The parry must never write it; see TraceParry.h.
	 */
	bool IsPassWindowInvulnerable() const;

	// ------------------------------------------------------------------------------------------
	// Source 2 (v3 §3): THE PARRY.
	// ------------------------------------------------------------------------------------------

	/**
	 * Ask for a parry. Call TraceParry::RequestParry() instead of this from gameplay code — that is
	 * the documented entry point and it copes with a null pawn.
	 *
	 * Server: applies the rules and opens the window. Owning client: predicts the RED TINT locally
	 * (nothing else) and sends ServerRequestParry. Anyone else: refused.
	 */
	void RequestParry(ETraceParryRefusal& OutRefusal);

	/** Owning client -> server. Reliable: a dropped parry is a death, which is not a droppable event. */
	UFUNCTION(Server, Reliable)
	void ServerRequestParry();

	/**
	 * AUTHORITATIVE. True while a parry window is open, per the REPLICATED window end time only.
	 *
	 * The trip test reads this and nothing else, which is what makes the server the sole judge of
	 * whether a dash landed inside the window. It deliberately ignores the local prediction — a
	 * client that predicts a parry the server refused still dies, and correctly so.
	 */
	bool IsParryActive() const;

	/**
	 * COSMETIC ONLY: authoritative window OR this machine's local prediction.
	 *
	 * Used exclusively by the visuals. 0.1s is shorter than a round trip, so a tint that waited for
	 * the server would arrive after the window it is meant to advertise had already closed.
	 */
	bool IsParryVisuallyActive() const;

	/** Seconds of parry window remaining (authoritative), 0 when closed. */
	float GetParryWindowRemaining() const;

	/** Seconds until this holder may parry again; 0 means ready. For the HUD pip. */
	float GetParryCooldownRemaining() const;

	/** The colour last pushed to the after-image materials. Debug readout for Trace.DebugParry. */
	FLinearColor GetAppliedTraceColor() const { return AppliedColor; }

	/**
	 * THE VISIBLE == LETHAL INVARIANT, in one function.
	 *
	 * Returns the highest index in TrailPoints that takes part in the lethal set. Everything from
	 * 0 to this index inclusive both KILLS (ServerRunTripTest) and is DRAWN (RebuildVisuals);
	 * everything newer than it does neither. -1 means the trace is too young to be either.
	 *
	 * It is deliberately a pure function of the replicated state (TrailPoints, bEmitting) plus
	 * UTraceSettings, so the server's answer and every client's answer are the same answer. A
	 * segment that cannot kill must never be on screen looking like it can - that mismatch is
	 * precisely the "I dashed through the trace and nothing happened" bug.
	 */
	int32 ComputeLastLethalIndex() const;

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
	 * v3 §3: shared-clock instant the current PARRY window closes. 0 = never parried.
	 *
	 * Replicated to EVERYONE, not just the owner, and that is the mechanic rather than a detail: the
	 * enemy who is mid-dash is the person who most needs the red trace, and they can only be shown it
	 * if their machine knows the window is open. One float, written once per parry.
	 *
	 * Stored as an absolute deadline rather than a countdown so it cannot be extended or lost by a
	 * re-assertion, and so the server's answer and every client's answer are the same arithmetic on
	 * the same replicated clock (AGameStateBase::GetServerWorldTimeSeconds).
	 */
	UPROPERTY(ReplicatedUsing = OnRep_ParryEndServerTime)
	float ParryEndServerTime = 0.f;

	/** Shared-clock instant this holder may parry again. Replicated so the HUD pip is truthful. */
	UPROPERTY(Replicated)
	float ParryCooldownEndServerTime = 0.f;

	/**
	 * CLIENT-ONLY, COSMETIC-ONLY: when this machine's locally predicted red tint expires.
	 *
	 * Never consulted by the trip test, never replicated, and cleared the moment the authoritative
	 * window replicates in. If the server refuses the parry this simply lapses and the trace goes
	 * back to team colour — the player sees a 0.1s flash and dies anyway, which is the honest
	 * outcome of predicting something the server said no to.
	 */
	float LocalParryPredictEndTime = 0.f;

	UFUNCTION()
	void OnRep_ParryEndServerTime();

	/** Server: the actual rules. Returns true and opens the window, or false with a reason. */
	bool ServerTryBeginParry(ETraceParryRefusal& OutRefusal);

	/**
	 * Server, DEBUG ONLY: Trace.Parry.BotAuto — AI carriers parry the instant their cooldown allows.
	 *
	 * It exists so an unattended bot match produces a large, mixed sample of parried and unparried
	 * dashes through the *real* code path, which is the only way to measure the mechanic without a
	 * human sitting there reacting. Compiled out of shipping and off by default.
	 */
	void ServerTickBotAutoParry();

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

	/**
	 * Scratch copy of the newest, NON-lethal stub of the trace (the emitter's own footprint). Not
	 * drawn and not lethal; kept only so the trip test can report a dash that crossed it, which is
	 * the difference between "the fix works" and "I hope the fix works".
	 */
	TArray<FVector> ExemptPositions;

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

	/**
	 * Last PARRY visual state the visuals were built for.
	 *
	 * Tracked separately from bLastVisualInvulnerable, and it has to be: a parry raised during an
	 * open pass window leaves IsTraceInvulnerable() true on both sides of the transition, so that
	 * flag alone would not notice — and the trace would stay pass-window-cyan instead of going red.
	 * This is the change detector for the RED, which is the whole readability of the mechanic.
	 */
	bool bLastVisualParry = false;

	/**
	 * Last emission state the visuals were built for. It is part of the change detection because
	 * ComputeLastLethalIndex() depends on it: the moment a holder stops emitting, the stub under
	 * their feet becomes lethal and must therefore become visible on the same frame.
	 */
	bool bLastVisualEmitting = false;

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

	/**
	 * Server: does the capsule swept from @p PreviousLocation to @p CurrentLocation this tick touch
	 * the polyline @p Positions? Horizontal segment-to-segment distance plus a vertical overlap
	 * test, exactly as the trip test has always done it — factored out so the LETHAL set and the
	 * exempt head stub can be asked the same question, which is what makes the instrumentation
	 * ("a dash crossed the trace but did not kill, and here is why") possible at all.
	 *
	 * A single-point polyline is tested as a degenerate segment, so a two-point trace is lethal.
	 */
	bool SweepIntersectsTrace(const TArray<FVector>& Positions, const FVector& PreviousLocation,
		const FVector& CurrentLocation, double HorizontalThreshold, double VerticalThreshold) const;

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
