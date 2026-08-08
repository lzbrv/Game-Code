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
// THE TRACE EXPIRES BY LENGTH, NOT BY TIME (spec v7 §1, and this replaced the timer rather than
// layering over it). A point leaves the TAIL only when the total path length exceeds
// GetTraceMaxLengthUU() — i.e. only when NEW trace is spawned at the head. A carrier who stands
// still therefore keeps their entire trace indefinitely, which is the whole point: the trace is the
// only way to kill a carrier, so a timer made "stand still and become unkillable" a real exploit.
// There is deliberately no surviving time-based expiry anywhere in this file; reintroducing one
// reintroduces the exploit. The TURNOVER GRACE (0.75s, spec v10 §3) is a separate mechanism and
// still applies.
//
// THE TRACE BELONGS TO POSSESSION, AND ENDS WITH IT (spec v9 §§3-4). Points may exist only while
// this component is emitting for a live owner who is holding the Core. The instant possession leaves
// — pass, mode-B throw, death, disconnect, goal, half time, kickoff, match end — the trace is wiped,
// visually and lethally, in the same call stack as the possession change. See SetEmitting(), and the
// invariant TickComponent asserts every server frame.
//
// THAT IS NOT A LIFETIME AND MUST NEVER BE TURNED INTO ONE. It is triggered by an event, not by a
// clock: a carrier standing still with the Core keeps every point they have laid, which is what
// killed the stand-still exploit in v7 §1 and has to stay killed.
//
// Three rules sit on top of that and are implemented here:
//
//   GRACE (§2)          After the Core changes TEAM, the trace does not begin to form for
//                       0.75 seconds (1.0 -> 0.4 in spec v3 §1, -> 0.5, -> 0.75 in v10 §3, which
//                       raised it after the mechanism was verified working). ATraceCore calls
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
//   PARRY (v3 §3)       A carrier-only, 0.2s window of trace invulnerability on a 1.5s cooldown,
//                       during which the ENTIRE trace turns red. Unlike the pass window it does NOT
//                       drop the shield. The window lives here (see ParryEndServerTime); the
//                       tunables, the entry point and the debug commands live in Gameplay/TraceParry.h,
//                       whose file header explains the split and — importantly — exactly how the two
//                       invulnerability sources compose without clobbering each other. Read it
//                       before changing either.
//
// VISUALLY (spec v6 §2) the trace is ONE CURVED RIBBON: "a rectangle which curves to follow the
// player ... one fluid shape ... a Tron path but from the model's back, curving through the air."
//
//   THE RIBBON   A single continuous swept rectangle, built along a Catmull-Rom smoothing of the
//                lethal point set, at EXACTLY the lethal cross-section (2 x TrailRadius wide,
//                TrailHeight tall) and centred on the trail points — which are the carrier's actor
//                location, i.e. the middle of their model. See RebuildRibbon().
//
// THIS REPLACED, AND DID NOT LAYER OVER, THE SPEC v4 §2 CHARACTER-SHAPED GHOSTS. Twenty
// UPoseableMeshComponents per carrier, each a full skinned Mannequin, were the prime suspect for the
// Demo 6 "1/6 the fps" report, and the user judged the look worse besides. The old two-layer renderer
// (posed mannequins + a two-band cylinder smear) is still compiled in behind Trace.Trail.Renderer 0
// for ONE reason and one only: it is the BEFORE arm of the A/B measurement, so the cost of this
// change can be quoted as a number on an identical scene rather than asserted. It is never the
// default and nothing but a console command can select it. Trace.Trail.PerfAB runs the comparison.
//
// See RebuildVisuals() and the block comment above it for the numbers and the justification.
//
// Clients only ever *read* TrailPoints: they rebuild a pooled set of meshes from it.
//
// THE THIRD, OWNER-ONLY LAYER (spec v5 §2): THE PREDICTED HEAD. See UpdatePredictedHead().
// The replicated point set always ends BEHIND the carrier — by the head-grace stub on every machine,
// and by a further round trip of travel on a remote client — so the carrier's own trace visibly
// stopped short of their feet. The gap is closed by drawing one extra stub from the last drawn point
// to where the carrier actually is, on the carrier's own machine, visible to the carrier ALONE. It is
// never lethal and it is never shown to anybody who could be killed by it; the reasoning that keeps
// the visible == lethal invariant intact is written out in full above UpdatePredictedHead().

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
class UPoseableMeshComponent;
class USkeletalMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * One character-shaped after-image that has been dropped on the path (spec v4 §2).
 *
 * Deliberately NOT one-per-trail-point. A trail point every 60uu for 2 seconds is ~27 points, and a
 * posed skeletal mesh per point does not survive ten players. A record is created only when the
 * newest LETHAL point is GhostSpacing uu clear of the last one, which is what makes the ghost count
 * a function of distance travelled rather than of point density.
 *
 * BirthServerTime is copied from the trail point the ghost was placed at, and is the only thing that
 * retires it: a ghost dies exactly when the piece of trace it stands on does, so the after-images and
 * the lethal polyline can never end in different places.
 */
struct FTraceGhostRecord
{
	/** Shared-clock birth time of the trail point this ghost was placed at. */
	float BirthServerTime = 0.f;

	/** The trail point itself, kept so the GhostSpacing test measures along the path, not the mesh. */
	FVector PathLocation = FVector::ZeroVector;

	/** World transform for the posed mesh: the trail point, plus the character's mesh offset. */
	FTransform MeshTransform = FTransform::Identity;

	/** True once a pose was successfully copied into the pooled component holding this record. */
	bool bPosed = false;
};

/**
 * SPEC v8 §2. One frame of where the LOCALLY CONTROLLED carrier actually was, on their own machine.
 *
 * The predicted head (spec v5 §2) used to be a straight line from the newest replicated point to the
 * pawn, and a straight line is only honest while it is short. On a JOINED CLIENT it is not short: the
 * pawn is client-predicted a round trip AHEAD of the server, and the newest point the server laid is
 * still a round trip BEHIND on the wire, so at dash speed the two ends are 400-600uu and one or two
 * corners apart. That is why the stub's length cap kept firing on a client and never on the host —
 * and a cap that fires deletes the whole stub, which is the "my trace detaches when I dash, but only
 * when I have joined a server" report.
 *
 * The carrier's own machine does not have to guess: it KNOWS where the pawn has been every frame.
 * These samples are that record. Nothing is replicated, nothing is lethal, and nothing is drawn for
 * any other viewer — see UpdatePredictedHead().
 */
struct FTraceLocalPathSample
{
	/** Where the pawn's capsule centre was — the same anchor ServerUpdateTrail() lays points at. */
	FVector Location = FVector::ZeroVector;

	/**
	 * Shared-clock stamp. Used for AGEING SAMPLES OUT and for nothing else.
	 *
	 * It is deliberately NOT how the stub decides which samples are still ahead of the replicated
	 * trace. That decision is geometric (see UpdatePredictedHead), because a client's shared clock is
	 * an estimate of the server's and a stamp comparison across the two would put the correctness of
	 * the picture at the mercy of a few milliseconds of clock skew. A DIFFERENCE of two stamps taken
	 * on this machine has no such problem, which is exactly what ageing out needs.
	 */
	float ServerTime = 0.f;
};

/**
 * The smear's cross-section and brightness, resolved once per rebuild and handed to every element.
 *
 * It exists so the REPLICATED smear and the OWNER-ONLY predicted head stub are built by one function
 * from one set of numbers. They draw the same shape at the same width for the same reason (the drawn
 * width is the statement of where the kill volume's edge is), and two copies of that arithmetic would
 * be two chances for the predicted stub to advertise a boundary the real trace does not have.
 */
struct FTraceSmearStyle
{
	/**
	 * Full lethal width — 2 x UTraceTrailComponent::GetTraceTrailRadius(). Never a flattering
	 * fraction of it. v7 §3 halved the radius (45 -> 22.5), so this is 45 rather than 90.
	 */
	double Width = 45.0;

	/** Full lethal height — GetTraceTrailHeight(). v7 §3: the middle third, 190 -> ~63. */
	double Height = 63.0;

	/** Body band placement within the lethal height. Spans the whole column when ghosts are off. */
	double BodyCentreFrac = 0.19;
	double BodyHeightFrac = 0.38;

	/** Multiplies both parts' glow: the ghosts-on dimming, so the mannequins stay the brighter thing. */
	float LayerScale = 1.f;
};

/**
 * SPEC v10 §2. The half-extents the trip test uses for one pawn, measured from its ACTOR LOCATION —
 * which is the capsule centre, and is the same reference frame every trail point is laid in.
 *
 * Filled by UTraceTrailComponent::MeasureModelReach; see the long comment above that declaration for
 * why the rendered mesh's bounds and not a constant, and for the bounded over-reach. Reported rather
 * than merely used, because "report the margin chosen and the worst-case over-reach in uu" is a
 * deliverable, and a number nobody can read is not one.
 */
struct FTraceModelReach
{
	/** The capsule the old test used, and the floor under the new one. 34 / 88 on the Mannequin. */
	double CapsuleRadius = 34.0;
	double CapsuleHalfHeight = 88.0;

	/** What the rendered mesh actually spans, before clamping. Equals the capsule when unknown. */
	double RawMeshRadius = 34.0;
	double RawMeshHalfHeight = 88.0;

	/** What the trip test uses. Always >= the capsule and <= capsule + the configured maximum. */
	double EffectiveRadius = 34.0;
	double EffectiveHalfHeight = 88.0;

	/** True when a skeletal mesh with usable bounds was found; false means a clamp supplied the answer. */
	bool bMeshMeasured = false;

	/** EffectiveRadius - CapsuleRadius. The number the spec asks to have reported. */
	double HorizontalMargin() const { return EffectiveRadius - CapsuleRadius; }
	double VerticalMargin() const { return EffectiveHalfHeight - CapsuleHalfHeight; }
};

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
	 * Server: start or stop laying the trace. Harmless no-op on clients.
	 *
	 * STARTING wipes whatever was there, so a new holder never inherits the previous holder's trace.
	 *
	 * STOPPING WIPES IT TOO (spec v9 §§3-4), and that is a rule rather than a tidy-up. Every way
	 * possession can end — a completed pass, a mode-B throw, the carrier being killed, the carrier
	 * disconnecting, a goal, half time, a kickoff, the end of the match — funnels through
	 * ATraceCore::ReleaseHolder(), which ends in exactly one call to this function. So this is the
	 * single place that can honour "the trace disappears the instant a pass is made", and it does it
	 * in the same call stack as the possession change: no fade, no grace, no one-frame window in
	 * which the trip test could still see the points.
	 *
	 * It used to stop WITHOUT clearing, on the reasoning that a residual trace was harmless and would
	 * fade out on its own. Neither was true after v7 §1 deleted time-based expiry: nothing retires a
	 * point except new trace arriving at the head, and the trip test gates on the POINTS rather than
	 * on emission — so the abandoned trace was immortal and it went on killing the player who laid
	 * it. See the implementation for the full account.
	 *
	 * Deliberately idempotent and re-assertable: ATraceCore re-calls this every tick
	 * (EnforceHolderTrailState), because several foreign systems (score reset, death handling) switch
	 * every trail in the match off wholesale and there is no pickup event left to switch the holder's
	 * back on. A REDUNDANT STOP STILL CLEARS — see the early-out — so a component that somehow ends
	 * up holding points with emission already off is swept by the next assertion rather than never.
	 */
	void SetEmitting(bool bEmit);

	/**
	 * Server: spec §2 — suppress POINT LAYING for @p Seconds from now.
	 *
	 * Must be called BEFORE SetEmitting(true), because that is what starts the emission and lays
	 * the first point. Stored as an absolute deadline rather than a countdown, so re-asserting
	 * emission (see SetEmitting) can neither extend nor lose the window.
	 *
	 * THE GRACE IS 0.75s (v10 §3; it was 1.0 -> 0.4 in v3 §1 and 0.5 before v10). The value itself
	 * lives in UTraceSettings::CoreTurnoverGraceSeconds and in Config/DefaultGame.ini, and the ini
	 * wins — do not read the number off this comment, read it off a running game (the [TRACEGRACE]
	 * log line prints what was actually granted). The request is capped here rather than
	 * at the single call site (ATraceCore's TraceCoreTuning::TransferGraceSeconds, still 1.0) for
	 * exactly the reason GetTraceLifetimeSeconds() caps the lifetime: that constant lives in a file
	 * this slice does not own, and a stale value there must not be able to reinstate the old rule.
	 * A caller asking for LESS still wins. Tune with Trace.Trail.TurnoverGrace.
	 *
	 * IT DELAYS FORMATION, NOT LETHALITY. Nothing in the grace path touches the trip test.
	 */
	void SetEmitGrace(float Seconds);

	/** The turnover grace actually in force, in seconds. v10 §3: 0.75. */
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

	/**
	 * Owning client -> server. Reliable: a dropped parry is a death, which is not a droppable event.
	 *
	 * v8 §3: @p ClientPressServerTime is the client's own shared-clock reading AT THE PRESS. The
	 * server clamps it exactly as a shot's timestamp is clamped and judges the parry against it, so
	 * a joined player is not charged for their packet's flight time. 0 = "no stamp", which resolves
	 * to arrival and reproduces the pre-v8 behaviour byte for byte.
	 */
	UFUNCTION(Server, Reliable)
	void ServerRequestParry(float ClientPressServerTime);

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

	/**
	 * SPEC v7 §6. SERVER. Clears the parry cooldown outright, so the carrier may parry again NOW.
	 *
	 * Verbatim: "when a player carrying the trace successfully kills an enemy with a parry, reset
	 * that players parry cooldown to zero". Called from exactly one place, TraceParry's
	 * ServerRefundOnParryKill, and only after the kill has actually landed.
	 *
	 * ForceNetUpdate because ParryCooldownEndServerTime is what the owning client's HUD pip reads:
	 * a refund the player cannot see until the next scheduled update is a refund they will not use.
	 * Silently ignored off the authority — the cooldown is server truth, and always has been.
	 */
	void ServerResetParryCooldown();

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

	/**
	 * NO LONGER THE EXPIRY RULE (v7 §1). Points are not retired by age any more — see
	 * GetTraceMaxLengthUU(). This survives as the AGE-FADE reference for the legacy measurement
	 * renderer (Trace.Trail.Renderer 0) and as the number Trace.DumpSettings prints, and it is still
	 * the derivation input for the default max length. Anything that asks it "is this point still
	 * alive?" is asking the wrong question and will be wrong for a stationary carrier.
	 */
	static float GetTraceLifetimeSeconds();

	// ------------------------------------------------------------------------------------------
	// THE THREE NUMBERS THE MECHANIC NOW HANGS ON (spec v7 §§1-3).
	//
	// Static and public because the trip test, the drawing, the bots' intercept planning and the
	// third-person camera all have to agree about the same volume. Every one of them is resolved
	// through these functions and nothing in this file reads UTraceSettings::TrailRadius /
	// TrailHeight directly any more — one number, one place, so "the lethal volume matches the drawn
	// volume" is structural rather than maintained by hand.
	// ------------------------------------------------------------------------------------------

	/**
	 * v7 §§1-2: the maximum LENGTH of the trace in uu, measured along the path. Points leave the
	 * tail only when a new one at the head pushes the total past this — never because time passed.
	 *
	 * Default 1200: TrailLifetime (2.0s) x WalkSpeed (800uu/s) = 1600uu, minus the 25% the spec
	 * asks for. Derived from those two live-editable settings rather than hardcoded so it still
	 * responds to the settings panel; Trace.Trail.MaxLength overrides it outright.
	 */
	static float GetTraceMaxLengthUU();

	/** v7 §3: half the lethal (and drawn) WIDTH, in uu. 45 -> 22.5, narrower than the player model. */
	static float GetTraceTrailRadius();

	/** v7 §3: the lethal (and drawn) HEIGHT, in uu, centred on the carrier's mid-model. 190 -> ~63. */
	static float GetTraceTrailHeight();

	// ------------------------------------------------------------------------------------------
	// SPEC v10 §2 — THE WHOLE MODEL, NOT THE CAPSULE.
	//
	// THE REPORT, verbatim: "Ensure that if ANY part of a players model touches the trace while
	// dashing, it counts as a connection (either for a parry or for a kill). This feels wonky right
	// now."
	//
	// THE MECHANISM. ServerRunTripTest sweeps a CAPSULE against the trace polyline, and the capsule
	// is 34uu of radius on a Mannequin whose arms, elbows and trailing leg reach further than that
	// in every animation a dash is ever seen in. The trace itself is only 22.5uu to a side since
	// Demo 7 shrank it. So the band between "the capsule missed" and "the model visibly clipped it"
	// is a real, several-inch-wide ring in which the player sees a hit and the game scores nothing.
	// That band IS the wonkiness, and it is the same band for both outcomes because there is one
	// test: bHitLethal decides the KILL when the carrier is vulnerable and the PARRY punish when
	// they are not.
	//
	// THE FIX, AND WHY THE BOUNDS AND NOT A CONSTANT. Three options were on the table (mesh bounds,
	// a flat configurable margin, a set of body-part volumes). This uses the SKELETAL MESH'S OWN
	// WORLD BOUNDS, clamped, because:
	//
	//   * They are, definitionally, the extent of the thing being RENDERED. The felt rule the spec
	//     states is "if it looks like it touched, it counts" — and what it looks like is exactly what
	//     the bounds enclose. A flat margin cannot be that; it is a guess that is too small in a
	//     spread pose and too large in a tucked one.
	//   * They FOLLOW THE POSE. A pawn standing with its arms down reports very nearly the capsule
	//     and gets very nearly the old behaviour; a pawn mid-dash with an arm and a leg extended
	//     reports more, and gets more, on that frame only. That is the correct shape for "do not
	//     overcorrect into phantom connections at visible distance" — the widening only exists while
	//     there is visible model out there to justify it.
	//   * They cost nothing. USceneComponent::Bounds is already computed and cached every frame for
	//     culling; this reads it. Per-bone body-part volumes would be more precise and would need a
	//     hardcoded bone list that silently stops matching the moment the mesh is re-imported — and
	//     this project imports its Mannequin automatically.
	//
	// AND WHY IT IS CLAMPED AT BOTH ENDS. Bounds are an AABB and can be inflated by an attached
	// actor, a stray socket or a physics-asset body that reaches further than the silhouette. The
	// measured reach is therefore admitted only within [capsule + MinMargin, capsule + MaxMargin],
	// so the WORST-CASE OVER-REACH OF THIS CHANGE IS EXACTLY MaxMargin uu AND IS BOUNDED BY
	// CONSTRUCTION rather than by trusting the asset. Both bounds are CVars (Trace.Trail.Model*).
	// ------------------------------------------------------------------------------------------

	/**
	 * Measure one pawn for the trip test. Static and public: the trip test uses it, the
	 * Trace.Trail.ModelReach readout prints it, and the bots' intercept planner should eventually
	 * ask it too so that what a bot aims at is what the test scores (see the report — that lives in
	 * AI/, which this pass does not own).
	 *
	 * Never returns anything smaller than the capsule. A pawn with no mesh gets the configured
	 * floor, so the fix does not silently evaporate on a pawn whose animation is not ticking.
	 */
	static FTraceModelReach MeasureModelReach(const ATraceCharacter* Candidate);

	/**
	 * Session counters for the widening, so its blast radius is a measurement and not a claim.
	 *
	 * OutTotal        every lethal trip the trip test has scored.
	 * OutModelOnly    those that the OLD capsule-only test would have MISSED — i.e. the kills and
	 *                 parry punishes this change created. The spec's "watch the knock-on" number:
	 *                 bots on Hard already take 82% of their kills off the trace, so the share of
	 *                 trips that exist only because of the widening is exactly what has to be
	 *                 reported before anybody retunes anything.
	 */
	static void GetModelTripStats(int32& OutTotal, int32& OutModelOnly);
	static void ResetModelTripStats();

#if !UE_BUILD_SHIPPING
	/**
	 * HARNESS ONLY (Trace.Trail.ModelHitTest). Force the sweep segment the NEXT trip test will
	 * evaluate for one candidate, by writing the "where were you last tick" entry the test builds it
	 * from.
	 *
	 * Why this exists rather than "teleport the pawn twice and hope": a dashing pawn is moving at
	 * DashSpeed, so between the frame the harness places it and the frame the test runs, the
	 * movement component has already moved it tens of uu — swamping a test whose whole subject is a
	 * band a few uu wide. Seeding the previous location makes the swept segment EXACTLY the one
	 * being asserted about, and everything downstream of it (the eligibility rules, the sweep, the
	 * thresholds, the kill, the parry punish) is the shipping path, untouched.
	 */
	void DebugSeedPreviousLocation(ATraceCharacter* Candidate, const FVector& Location);
#endif

	/**
	 * Shared-clock instant the turnover grace expires, or 0 when none is running. Exposed so
	 * Trace.Trail.GraceTest can report what the grace ACTUALLY was rather than what it was asked
	 * for — spec v10 §3 opens with "the grace period on turnovers doesn't seem to be working", and
	 * the only way to answer that is to read the deadline the emitter is really gated on.
	 */
	float GetEmitGraceEndServerTime() const { return EmitGraceEndServerTime; }

	// ------------------------------------------------------------------------------------------
	// THE PREDICTED HEAD (spec v5 §2). Owner-only, cosmetic, never lethal. Read-only accessors so
	// Trace.Trail.TestHeadGap can measure the thing it claims to have fixed instead of asserting it.
	// ------------------------------------------------------------------------------------------

	/** True when THIS machine is drawing the owner-only predicted head stub for this trace. */
	bool IsPredictingLocalHead() const;

	/**
	 * uu of trace the predicted stub is currently covering: from the newest DRAWN (== lethal) point
	 * to the carrier's own feet. This is exactly the gap the user reported, so it is exactly what the
	 * verification harness prints. 0 when nothing is being predicted.
	 */
	float GetPredictedHeadLength() const;

	/**
	 * uu between the newest LETHAL point and the carrier's current position — the gap that would be
	 * on screen with no prediction. Independent of whether the prediction is running, so a run can
	 * print the before and the after on one line.
	 */
	float MeasureHeadGap() const;

	// ------------------------------------------------------------------------------------------
	// SPEC v8 §2 — WHY THE STUB WAS NOT DRAWN. The v5 §2 prediction can DECLINE (see
	// UpdatePredictedHead), and a decline is invisible in every number above: the stub simply is not
	// there, exactly as if the feature had never shipped. On a client that decline was the bug, so it
	// is counted rather than inferred.
	// ------------------------------------------------------------------------------------------

	/** Times this machine has abandoned the stub because the gap was too long to draw honestly. */
	int32 GetPredictedHeadDropCount() const { return PredictedHeadDrops; }

	/**
	 * uu the stub would have had to span on the last frame it was considered — drawn or dropped.
	 *
	 * This is the number the length cap is compared against, so it is the one that says whether a
	 * client is living near the cliff edge. It keeps its value on a frame the stub was declined,
	 * which GetPredictedHeadLength() (0 when nothing is drawn) deliberately cannot.
	 */
	float GetPredictedHeadSpan() const { return PredictedHeadSpan; }

	/** Recorded local path samples the last stub was built through. 0 = it was a straight chord. */
	int32 GetPredictedHeadSamplesUsed() const { return PredictedHeadSamplesUsed; }

	/**
	 * THE NUMBER THE USER ACTUALLY REPORTED: how far it is from the carrier to the nearest piece of
	 * their own trace THAT THEIR OWN CAMERA IS ALLOWED TO SEE, in uu, surface to centre.
	 *
	 * MeasureHeadGap() above only knows about the point set, so it cannot see the far bigger half of
	 * this bug — the owner-only hide, which drew nothing for the newest 850uu without removing a
	 * single point. This walks the drawn geometry instead and skips anything flagged bOwnerNoSee, so
	 * it measures the hole in the picture rather than the hole in the data.
	 *
	 * @param bIncludePredictedHead  false answers "what would it be with the prediction switched
	 *                               off", so one line can carry both halves of the fix.
	 */
	float MeasureOwnerVisibleGap(bool bIncludePredictedHead) const;

	// ------------------------------------------------------------------------------------------
	// MEASUREMENT (spec v6 §1). Read-only counters so Trace.Trail.PerfAB can report what the trace
	// actually put in the scene alongside the frame times, rather than inferring it.
	// ------------------------------------------------------------------------------------------

	/** Pooled mesh components this trace currently has VISIBLE, split by kind. */
	void CountDrawnPieces(int32& OutStaticVisible, int32& OutSkinnedVisible) const;

	/** Pooled mesh components this trace has ALLOCATED, visible or not, split by kind. */
	void CountPooledPieces(int32& OutStaticTotal, int32& OutSkinnedTotal) const;

	// ------------------------------------------------------------------------------------------
	// THE CLIENT TETHER BUG (spec v7 §7). Read-only counters so Trace.Trail.TetherCheck can report
	// the reordering as it happens instead of asserting that it was fixed. See
	// RestoreReplicatedPointOrder().
	// ------------------------------------------------------------------------------------------

	/** How many times this machine has had to put a replicated point set back into path order. */
	int32 GetPointOrderRepairCount() const { return PointOrderRepairs; }

	/**
	 * Adjacent pairs currently out of PATH order in TrailPoints.Items. 0 = the identity order is sane.
	 *
	 * v8 §2: keyed on ReplicationID, the integer FFastArraySerializer hands out in append order and
	 * keeps in sync across the wire — not on the replicated float BirthServerTime v7 §7 used. A float
	 * key made both this counter and the repair blind to ties, so a scrambled array with two identical
	 * stamps scored a clean zero here while still being drawn wrong. See RestoreReplicatedPointOrder().
	 */
	int32 CountPointOrderViolations() const;

	/**
	 * uu from the carrier to the OLDEST point in the array — i.e. to the far end of the drawn
	 * ribbon. THE NUMBER THE BUG REPORT IS ABOUT: with the tether bug this collapses towards zero on
	 * a client (the newest point, under the carrier's feet, gets swapped into slot 0), while a
	 * healthy trace reads roughly the trace's full length. Negative when there is no trace.
	 */
	float MeasureTailDistanceToCarrier() const;

	/** Total length of the replicated path, in uu — what v7 §1's max length is enforced against. */
	float MeasureTraceLength() const;

	/**
	 * THE SHARPEST SINGLE DETECTOR OF A CORRUPTED PATH, and the reason it exists is that ORD is not.
	 *
	 * The longest distance between two ADJACENT points in TrailPoints.Items. The server lays points at
	 * TrailPointSpacing and restarts the trace on any discontinuity, so a healthy path's worst adjacent
	 * gap is a small multiple of the spacing whatever the carrier is doing. A path whose order has been
	 * scrambled contains one adjacent pair that jumps most of the trace's length — so this reads ~1200
	 * instead of ~80 the instant the array is wrong, on the frame it is wrong.
	 *
	 * ORD (CountPointOrderViolations) can only see a violation of CHRONOLOGICAL order. This sees the
	 * geometry the ribbon is actually built from, which is the thing the player is looking at.
	 */
	float MeasureMaxAdjacentSegment() const;

	/**
	 * Surface distances, in uu, from the carrier to the NEAREST and FARTHEST visible piece of the
	 * REPLICATED ribbon (the owner-only stub is excluded — it is not drawn for anybody else).
	 *
	 * The far end of the drawn ribbon being pulled onto the character is the literal wording of the
	 * bug report, so it is measured off the geometry rather than inferred from the point array:
	 * OutFarthest collapsing towards zero while the trace is long IS the reported symptom.
	 * Both are -1 when nothing is drawn.
	 */
	void MeasureDrawnSpan(float& OutNearest, float& OutFarthest, int32& OutVisiblePieces) const;

	/**
	 * SPEC v7 §3 VERIFICATION, BOTH DIRECTIONS, off the geometry that is actually on screen.
	 *
	 * @param OutMaxHalfWidth   widest half-width any VISIBLE piece is drawn at. Must not exceed
	 *                          GetTraceTrailRadius(), or there is ribbon that does not kill.
	 * @param OutMaxHalfHeight  tallest half-height any visible piece is drawn at. Compared against
	 *                          GetTraceTrailHeight()/2 plus whatever vertical span the carrier's
	 *                          jump added to the segment.
	 * @param OutWorstUncovered worst distance, in uu, from a LETHAL trail point to the nearest
	 *                          visible piece's surface. Non-zero means there is a stretch that kills
	 *                          and is not drawn — the other direction of the same invariant.
	 * @param OutVisiblePieces  how many pooled pieces are currently visible.
	 */
	void MeasureDrawnVolume(double& OutMaxHalfWidth, double& OutMaxHalfHeight,
		double& OutWorstUncovered, int32& OutVisiblePieces) const;

#if !UE_BUILD_SHIPPING
	/**
	 * SPEC v12 §6 VERIFICATION. How far the trace is currently INSIDE the level, measured on both
	 * halves of the invariant separately so they can be compared rather than assumed equal.
	 *
	 * DRAWN is sampled over the oriented box of every visible ribbon piece — the actual on-screen
	 * geometry, joint overlaps and minimum element lengths included. LETHAL is sampled over the swept
	 * volume the trip test evaluates (TrailRadius x TrailHeight along the lethal polyline). Both are
	 * probed against ECC_Visibility blocking geometry with pawns filtered out, so what is being
	 * measured is "is this inside a wall", not "is this near a player".
	 *
	 * The two depths are the diagnosis as well as the verdict:
	 *   LETHAL ~= DRAWN and both > 0  -> the PATH itself cuts through the structure (a chord across a
	 *                                    corner), so the fix has to move the path.
	 *   DRAWN > LETHAL                -> the drawing is reaching outside the volume that kills, which
	 *                                    is a lethal!=drawn violation in its own right.
	 */
	struct FTraceClipSample
	{
		/** Deepest uu of DRAWN ribbon inside a piece of level geometry, where, and which piece. */
		double DrawnDepth = 0.0;
		FVector DrawnWorst = FVector::ZeroVector;
		FVector DrawnWorstPushOut = FVector::ZeroVector;
		FString DrawnWorstPiece;

		/** Deepest uu of the LETHAL swept volume inside a piece of level geometry. */
		double LethalDepth = 0.0;
		FVector LethalWorst = FVector::ZeroVector;
		FString LethalWorstPiece;

		/**
		 * CLOSEST the drawn ribbon came to a surface WITHOUT entering it, in uu.
		 *
		 * The number that stops a clean result being a vacuous one. A run that reports no clipping
		 * because the carrier never went near a structure looks identical, in every other field, to a
		 * run that hugged every wall in the arena and stayed out of all of them — and only one of
		 * those is evidence. Large = the fixture never did the thing the report describes.
		 */
		double DrawnNearestSurface = TNumericLimits<double>::Max();

		/** Same measurement taken on the CARRIER's own actor location — how close the BODY got. */
		double CarrierNearestSurface = TNumericLimits<double>::Max();
		FString CarrierNearestPiece;

		int32 DrawnSamplesInside = 0;
		int32 DrawnSamplesTotal = 0;
		int32 LethalSamplesInside = 0;
		int32 LethalSamplesTotal = 0;
		int32 VisiblePieces = 0;
		int32 LethalPoints = 0;
	};

	/**
	 * ONE RENDERED BOX OF THE LEVEL, flattened out of whatever component draws it.
	 *
	 * Flattened rather than held as components for two reasons, both of which cost this pass a run.
	 * First, most of the arena is pooled into UInstancedStaticMeshComponents, so a component's own
	 * transform is the POOL's and describes nothing a player can see — the instances have to be
	 * expanded or the measurement silently tests a few dozen boxes stacked at the origin. Second, a
	 * flat array with precomputed world bounds is what makes a per-frame lattice over the whole trace
	 * affordable: the trace is ~1200uu long and the arena is thousands of boxes, so almost all of the
	 * work is rejecting the ones that are nowhere near it.
	 */
	struct FTraceClipBox
	{
		FTransform Transform;
		FVector LocalCentre = FVector::ZeroVector;
		FVector LocalExtent = FVector::ZeroVector;
		FVector Scale = FVector::OneVector;
		FBox WorldBounds = FBox(ForceInit);
		FString Name;
	};

	/**
	 * @param Geometry  the RENDERED level boxes to test against, gathered once by the caller.
	 *
	 * IT HAS TO BE THE RENDERED MESHES, and the first arm of this pass measured the wrong thing by
	 * assuming otherwise. The arena's visible structure is built from NoCollision meshes with
	 * separate, smaller UBoxComponents behind them for physics; on top of that the interior carries
	 * PAWN-ONLY standoff shells that hold a body 26-40uu further out again. So a query against the
	 * physics scene reports acres of clearance while the ribbon is visibly inside a corner rib —
	 * which is exactly the report. What the player sees is the rendered mesh, so that is what a
	 * measurement of "the trace clips into walls" has to intersect.
	 */
	void MeasureWorldClipping(const TArray<FTraceClipBox>& Geometry, FTraceClipSample& Out) const;

	/** Points appended purely because the direct chord was obstructed. Session totals, all carriers. */
	static void GetWallFitStats(int32& OutRoutedAppends, int32& OutInsertedPoints, int32& OutUnroutable);
	static void ResetWallFitStats();
#endif

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

	/**
	 * Server: the actual rules. Returns true and opens the window, or false with a reason.
	 *
	 * v8 §3: @p ClientPressServerTime is the client's stamped press, clamped and anchored by
	 * TraceParry::ServerResolvePress. 0 (the host path and the bot auto-parry path) resolves to
	 * "now", so those callers do not change by one frame.
	 */
	bool ServerTryBeginParry(ETraceParryRefusal& OutRefusal, float ClientPressServerTime = 0.f);

	// --- v8 §3: a lethal trip waiting out a remote carrier's upstream lag ------------------------
	//
	// See Gameplay/TraceParry.h. GetTripHoldSeconds() is 0 for the host and for bots, so on a single
	// machine these three fields are written, read once and cleared with no behaviour change at all.

	/** The dasher whose lethal trip is currently being held. Null when nothing is pending. */
	TWeakObjectPtr<ATraceCharacter> PendingTripDasher;

	/** Shared-clock instant the held trip's sweep resolved. This, not "now", is what a parry must cover. */
	float PendingTripServerTime = 0.f;

	/** How long the trail has already held it. Compared against GetTripHoldSeconds(). */
	float PendingTripHeldSeconds = 0.f;

	/**
	 * SERVER. Advance the one pending held trip by DeltaTime and act on the verdict.
	 *
	 * THE CONTRACT THIS EXISTS TO HONOUR (TraceParry.h, GetAbandonedHeldTripCount): once
	 * ServerResolveHeldTrip has answered KeepHolding, the caller must keep asking EVERY FRAME until it
	 * answers KillNow or Parried — *whether or not the dasher is still touching the trace*. A dash
	 * crosses a ribbon in one or two frames and a hold lasts three or four, so the old
	 * "else { PendingTripDasher = nullptr; }" dropped essentially every remote hold on the floor: the
	 * kill never landed, the dead-man switch tripped, and held trips disarmed themselves for the
	 * session. Measured: case B left the carrier ALIVE 3/3 instead of DEAD 4/4.
	 *
	 * @param Holder        the carrier the trip would kill.
	 * @param DeltaTime     the frame's delta, accumulated into PendingTripHeldSeconds.
	 * @param OutPunished   set to the dasher this call punished for a parry, if any. The caller must
	 *                      not punish that same dasher again through the live-parry path in the same
	 *                      frame — ServerPunishParriedDash carries the Demo 7 refunds (parry cooldown
	 *                      to zero, one dash charge back), so a double call hands out two.
	 * @return true if the trip is still pending (the caller must not start a different one).
	 */
	bool ServerAdvancePendingTrip(ATraceCharacter* Holder, float DeltaTime, ATraceCharacter** OutPunished = nullptr);

	/** SERVER. Drop the pending hold cleanly — cancels the parry-side record so it is not "abandoned". */
	void ServerCancelPendingTrip();

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
	// SPEC v12 §6 — keeping the trace out of the walls
	// ------------------------------------------------------------------------------------------

	/**
	 * THE CARRIER'S ACTUAL ROUTE SINCE THE LAST POINT WAS LAID. Server-only, one entry per server
	 * tick, cleared every time a point is appended (and by ClearTrail / SetEmitting).
	 *
	 * This is the raw material for the whole fix, so it is worth being explicit about why it has to
	 * exist. Points are laid every TrailPointSpacing (60uu) of travel, and the trace — both the
	 * ribbon and the trip test — is the STRAIGHT CHORD between consecutive points. Round a pillar
	 * corner the carrier's capsule follows an arc of radius ~34uu (its own radius) about the corner
	 * vertex, and 60uu of arc at that radius is more than the whole quarter turn: the two points
	 * straddle the corner and the chord between them passes ~10uu from the vertex, i.e. straight
	 * through the pillar. That is the reported bug, and note that it is not a DRAWING bug — the
	 * lethal volume takes the identical shortcut.
	 *
	 * The carrier's real path cannot have that problem: their capsule is 34uu of radius and the trace
	 * is 22.5uu, so wherever a body legally stood there is room for the trace. So the repair is to
	 * SUBDIVIDE rather than to invent — insert the positions the carrier really occupied until the
	 * polyline stops cutting the corner. Nothing is moved off the route the player ran.
	 */
	TArray<FVector> PendingPathSamples;

	/**
	 * Is the trace's own volume — a TrailRadius-wide, TrailHeight-tall upright column — clear of
	 * blocking level geometry everywhere along the segment From->To?
	 *
	 * ECC_Visibility, with pawns filtered out, because "solid enough to hide a bullet" is the same
	 * set of surfaces as "solid enough to look wrong with a ribbon inside it", and because the arena's
	 * pawn-standoff shells are deliberately invisible and must not push the trace around.
	 */
	bool IsTrailVolumeClear(const FVector& From, const FVector& To) const;

	/**
	 * Nudge one candidate point out of any geometry it is already inside, horizontally, by at most
	 * GetWallFitMaxPush() uu — and only if the nudge actually reduces the penetration AND the moved
	 * point is still in line of sight of where it started (so a thin wall can never be tunnelled
	 * through, which would put the trace, and therefore the kill volume, on the far side of a wall
	 * the player never crossed).
	 */
	FVector FitPointToWorld(const FVector& Candidate) const;

	/**
	 * Append Target to TrailPoints, inserting as few of PendingPathSamples as it takes for no
	 * segment of the polyline to pass through the level. Returns the number of points appended.
	 *
	 * THIS IS THE ONE PLACE THE FIX LIVES, AND THAT IS THE POINT. The ribbon is built from
	 * TrailPoints and the trip test is built from TrailPoints; neither is touched by this change. So
	 * "the lethal volume matches the drawn volume" is preserved the same way it was already
	 * guaranteed — by there being one polyline — rather than by two pieces of geometry code being
	 * kept in agreement by hand.
	 */
	int32 AppendTrailPointsFitted(const FVector& Target, float Now);

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

	/**
	 * THE RIBBON'S SOURCE MESH (spec v6 §2): a box, because the request is literally "a rectangle
	 * which curves to follow the player". Falls back to the cylinder if /Engine/BasicShapes/Cube is
	 * somehow absent — a rounded ribbon is still a ribbon, and no .uasset is ever a hard requirement.
	 */
	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> TrailMaterial = nullptr;

	/**
	 * True when TrailMaterial resolved to /Game/Generated/Materials/M_TraceNeon (unlit, Color * Glow)
	 * rather than to the BasicShapeMaterial fallback. Only the neon material has a Glow parameter,
	 * and only it can be pushed past the bloom threshold.
	 */
	bool bTrailMaterialIsNeon = false;

	/**
	 * THE DRAWN TRACE. One pool, shared by both renderers, because they are mutually exclusive and a
	 * second set of arrays would be a second set of things to forget to hide.
	 *
	 *   RIBBON (spec v6 §2, the default): ONE element per resampled ribbon segment.
	 *   LEGACY (spec v4 §2, measurement only): TWO per lethal segment, interleaved [i*2+0] body,
	 *          [i*2+1] head.
	 *
	 * PartsPerElement() is the multiplier, and every loop over this array goes through it. Switching
	 * renderer at runtime tears the pool down (see ActiveRendererArm) rather than reinterpreting it.
	 *
	 * The legacy comment, still true of the legacy arm:
	 *
	 * THE SMEAR. Pooled static meshes, TWO per lethal SEGMENT, interleaved: [i*2+0] body, [i*2+1] head.
	 *
	 * One element per SEGMENT, not per point, and that is the whole reason this layer is trustworthy:
	 * an element spans exactly from its segment's start to its segment's end (plus one TrailRadius of
	 * overlap at interior joints, which covers the wedge on the outside of a corner), so the drawn
	 * smear is the lethal polyline and not an approximation of it.
	 *
	 * Sized from the same UTraceSettings TrailRadius/TrailHeight the server's trip test uses, so what
	 * you dash at is what kills you — a smear narrower than its own lethal volume would be a trap.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> SmearMeshes;

	/** Parallel to SmearMeshes — one MID per pooled component, created once with it. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SmearMaterials;

	/**
	 * Parallel to SmearMeshes. The brightness each piece WOULD have with the camera far away, i.e.
	 * before ApplyProximityGlowFade() attenuates it. Split out because the rebuild is guarded by a
	 * dirty check while the proximity fade has to run every frame, so the two cannot share a pass.
	 */
	TArray<float> SmearBaseGlow;

	/** Parallel again: the proximity scale last actually written, so the pass can skip no-op writes. */
	TArray<float> SmearAppliedGlowScale;

	// ------------------------------------------------------------------------------------------
	// THE PREDICTED HEAD (spec v5 §2) — a SECOND, TINY SMEAR POOL, owner-only.
	//
	// Deliberately not slots at the end of the main pool. The main pool is rebuilt only when the
	// replicated point set changes; the predicted stub has to be re-placed EVERY FRAME because it
	// tracks a moving pawn, and putting it in the same array would mean either a full 96-element
	// rebuild every frame or index bookkeeping that the "grow one whole element at a time" invariant
	// in EnsureSmearElement was written to make impossible. Four elements is the whole cost.
	// ------------------------------------------------------------------------------------------

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> PredictedSmearMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> PredictedSmearMaterials;

	TArray<float> PredictedSmearBaseGlow;
	TArray<float> PredictedSmearAppliedGlowScale;

	/** uu the predicted stub covered on the last frame it ran. 0 when it is not drawing. */
	float PredictedHeadLength = 0.f;

	/**
	 * SPEC v8 §2. The locally controlled carrier's own recent path, newest last. Never replicated.
	 *
	 * Recorded on the machine that is PREDICTING the pawn, which is the only machine that knows where
	 * it has been between two replicated trail points. Trimmed every frame to the samples newer than
	 * the newest replicated point, so it holds one round trip of travel and nothing else — it is a
	 * window onto the present, not a history that could ever redraw the past.
	 */
	TArray<FTraceLocalPathSample> LocalPathHistory;

	/** Counters behind GetPredictedHeadDropCount / GetPredictedHeadSpan / GetPredictedHeadSamplesUsed. */
	int32 PredictedHeadDrops = 0;
	float PredictedHeadSpan = 0.f;
	int32 PredictedHeadSamplesUsed = 0;

	/** One line per episode of the stub being declined, re-armed when it draws again. */
	bool bPredictedHeadDropReported = false;

	// ------------------------------------------------------------------------------------------
	// THE GHOSTS (spec v4 §2). Pooled posed Mannequins.
	// ------------------------------------------------------------------------------------------

	/**
	 * Pooled UPoseableMeshComponents, one per FTraceGhostRecord, index-aligned with GhostRecords.
	 *
	 * UPoseableMeshComponent and not USkeletalMeshComponent, deliberately: a skeletal mesh component
	 * would carry an AnimInstance, evaluate a graph and tick a pose EVERY FRAME for something that is
	 * frozen by definition. A poseable mesh has no anim graph at all — CopyPoseFromSkeletalComponent
	 * writes the bones once, RefreshBoneTransforms is called once, and after that the component is a
	 * static skinned draw with its tick disabled.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UPoseableMeshComponent>> PoseGhosts;

	/** Parallel to PoseGhosts. One MID per ghost, bound to EVERY material slot of the Mannequin. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> PoseGhostMaterials;

	/** Parallel to PoseGhosts — same split as SmearBaseGlow / SmearAppliedGlowScale, same reason. */
	TArray<float> PoseGhostBaseGlow;
	TArray<float> PoseGhostAppliedGlowScale;

	/**
	 * The after-images currently standing on the path, oldest first. Purely local: the trace is
	 * replicated as points and every machine derives its own ghosts from them.
	 */
	TArray<FTraceGhostRecord> GhostRecords;

	/**
	 * The Mannequin the ghosts are wearing, cached so a change of character art rebuilds the pool.
	 * Weak: nothing here should keep a skinned asset alive.
	 */
	TWeakObjectPtr<UObject> GhostSkinnedAsset;

	/**
	 * Whether M_TraceNeon is usable on a skinned mesh at all.
	 *
	 * Resolved ONCE, lazily, because it is a hard gate: a material without MATUSAGE_SkeletalMesh is
	 * silently swapped for the engine's default grey checkerboard on a skeletal draw, which would put
	 * a herd of untinted grey mannequins on the pitch instead of a trace. If the answer is no, the
	 * ghosts are switched off and the smear carries the whole trace on its own at full brightness —
	 * i.e. exactly the pre-v4 look, which is worse but is not broken.
	 */
	enum class EGhostMaterialState : uint8 { Unknown, Usable, Unusable };
	EGhostMaterialState GhostMaterialState = EGhostMaterialState::Unknown;

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

	/** Source-mesh metrics, read once from the asset so we never hardcode "the cylinder is 100uu". */
	FVector CylinderHalfSize = FVector(50.0);
	FVector CylinderPivotOffset = FVector::ZeroVector;
	FVector CubeHalfSize = FVector(50.0);
	FVector CubePivotOffset = FVector::ZeroVector;
	bool bMeshMetricsCached = false;

	// ------------------------------------------------------------------------------------------
	// THE RIBBON (spec v6 §2)
	// ------------------------------------------------------------------------------------------

	/**
	 * Which renderer arm the pooled components were built for (see Trace.Trail.Renderer).
	 *
	 * The two arms disagree about how many meshes an element is and what mesh each one wears, so a
	 * switch destroys the pool instead of reinterpreting it. -1 = nothing built yet.
	 */
	int32 ActiveRendererArm = -1;

	/**
	 * Scratch for one ribbon rebuild: the Catmull-Rom centreline resampled at a near-uniform arc
	 * length, and the shared-clock birth time interpolated to each sample (which is what makes the
	 * age fade a smooth gradient along the ribbon instead of a step per element).
	 *
	 * Members rather than locals purely so a rebuild allocates nothing after the first one.
	 */
	TArray<FVector> RibbonSamples;
	TArray<float> RibbonSampleBirth;

	/** The unsmoothed input to the above: the lethal polyline, or the predicted stub's polyline. */
	TArray<FVector> RibbonSourcePoints;
	TArray<float> RibbonSourceBirths;

	// ------------------------------------------------------------------------------------------
	// Internals
	// ------------------------------------------------------------------------------------------

	ATraceCharacter* GetOwnerCharacter() const;

	/** Shared clock: GameState time where available, local world time as a fallback. */
	float GetServerTimeSeconds() const;

	/** Server: append points, then trim the tail back to the max LENGTH (v7 §1). */
	void ServerUpdateTrail();

	/**
	 * SPEC v7 §7, THE CLIENT TETHER BUG — and the fix is here rather than in the drawing because the
	 * damage is done to the DATA, not to the geometry.
	 *
	 * FFastArraySerializer deletes with RemoveAtSwap (FastArraySerializer.h, "Items.RemoveAtSwap
	 * (DeleteIndex, EAllowShrinking::No)"). Every machine that only RECEIVES the array therefore has
	 * its order scrambled by every removal: dropping the oldest point swaps the NEWEST point — the
	 * one under the carrier's feet — into slot 0, which is the tail of everything drawn off this
	 * array. So the far end of the ribbon snaps to the character, exactly as reported, and both ends
	 * of the trace end up tied to them. The server never sees it because authority mutates Items
	 * itself with an order-preserving RemoveAt.
	 *
	 * SPEC v8 §2 — THE KEY CHANGED, AND THAT IS THE FIX. v7 sorted on the replicated FLOAT
	 * BirthServerTime with a strict `<` and a stable sort, so two points sharing a stamp kept whatever
	 * order RemoveAtSwap left them in — and CountPointOrderViolations(), using the same comparison,
	 * scored that array as perfect. It now sorts on ReplicationID, which IS the server's append order
	 * (FFastArraySerializer::MarkItemDirty issues `++IDCounter`), is an integer, and is the one field
	 * the engine guarantees is "replicated and in sync between client and server" while the indices
	 * are not. A second, independent GEOMETRIC detector (an adjacent gap the server could never have
	 * laid) also triggers the repair and is re-checked afterwards, so a path this cannot fix is
	 * reported rather than silently drawn.
	 *
	 * Restores path order in place, on non-authority only, and empties the fast array's ItemMap so the
	 * engine rebuilds the ReplicationID -> index map against the new layout (the same thing the engine
	 * itself does immediately after its RemoveAtSwap loop). Returns true when it had to move anything.
	 *
	 * It is the ARRAY that is fixed, not a private copy of it, because every other reader —
	 * ComputeLastLethalIndex, the bots, TraceParry's verifier, the GameMode — indexes TrailPoints
	 * .Items directly and would otherwise each need their own copy of this knowledge.
	 */
	bool RestoreReplicatedPointOrder();

	/** Counter behind GetPointOrderRepairCount(). */
	int32 PointOrderRepairs = 0;

	/**
	 * Throttle for the "this path is not a path" warning in RestoreReplicatedPointOrder().
	 *
	 * That check runs every tick, so an unthrottled warning would be a per-frame log flood on the one
	 * machine that is already in trouble. One line per episode, re-armed the moment the path is sane
	 * again, so a recurring fault still produces one line per occurrence rather than one ever.
	 */
	bool bPathBreakReported = false;

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

	/** The cross-section every smear element is built from. One resolution per rebuild. */
	FTraceSmearStyle MakeSmearStyle() const;

	/**
	 * Places one two-part smear element (body + head band) across [SegStart, SegEnd] into @p Pieces.
	 *
	 * Shared by the replicated smear and by the owner-only predicted head, so the predicted stub is
	 * the same shape, at the same width, from the same numbers as the trace it continues.
	 *
	 * @param GlowScale         age fade x invulnerability step, applied on top of the style's LayerScale.
	 * @param bOnlyOwnerSees    true ONLY for the predicted head: hides the piece from every viewer
	 *                          except the carrier it belongs to. See UpdatePredictedHead().
	 */
	void PlaceSmearSegment(
		TArray<TObjectPtr<UStaticMeshComponent>>& Pieces,
		TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
		TArray<float>& BaseGlowOut,
		TArray<float>& AppliedScaleOut,
		int32 ElementIndex,
		const FVector& SegStart,
		const FVector& SegEnd,
		double BackOverlap,
		double ForwardOverlap,
		const FTraceSmearStyle& Style,
		float GlowScale,
		bool bOnlyOwnerSees);

	/** Layer 1: the continuous body-shaped extrusion along every lethal segment. */
	void RebuildSmear(int32 LethalPointCount, float InvulnerableScale);

	// ------------------------------------------------------------------------------------------
	// THE RIBBON (spec v6 §2). Read the block comment above RebuildRibbon().
	// ------------------------------------------------------------------------------------------

	/** True when the ribbon is the active renderer (Trace.Trail.Renderer 1, the default). */
	static bool IsRibbonRenderer();

	/** Pooled meshes per drawn element: 1 for the ribbon, 2 for the legacy smear. */
	static int32 PartsPerElement();

	/**
	 * Fills RibbonSamples / RibbonSampleBirth with a Catmull-Rom smoothing of @p Points, resampled at
	 * a near-uniform arc length. @p Births may be empty, in which case the birth array is zeroed.
	 *
	 * The step coarsens automatically rather than truncating when the path is long: a dashing carrier
	 * must get a slightly chunkier ribbon, never a ribbon that stops short of trace that can kill.
	 */
	void BuildRibbonSamples(const TArray<FVector>& Points, const TArray<float>& Births, double Step, int32 MaxElements);

	/** Spec v6 §2: the whole lethal set as ONE swept, curved rectangle. */
	void RebuildRibbon(int32 LethalPointCount, float InvulnerableScale);

	/**
	 * Places the elements of RibbonSamples into @p Pieces. Shared by the replicated ribbon and by the
	 * owner-only predicted head, so the stub is the same shape at the same width from the same
	 * numbers as the ribbon it continues.
	 */
	void PlaceRibbon(
		TArray<TObjectPtr<UStaticMeshComponent>>& Pieces,
		TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
		TArray<float>& BaseGlowOut,
		TArray<float>& AppliedScaleOut,
		int32 MaxElements,
		float InvulnerableScale,
		bool bTailFade,
		bool bOnlyOwnerSees,
		bool bOverlapAtStart);

	/** Grows @p Pieces to cover element @p ElementIndex with the ribbon's source mesh. */
	bool EnsureRibbonElement(
		TArray<TObjectPtr<UStaticMeshComponent>>& Pieces,
		TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
		TArray<float>& BaseGlowOut,
		TArray<float>& AppliedScaleOut,
		int32 ElementIndex,
		int32 MaxElements,
		bool bOnlyOwnerSees);

	/**
	 * Layer 2: retire the after-images whose stretch of trace has expired, drop a new one when the
	 * path has moved on far enough, and push colour/brightness at all of them.
	 */
	void RebuildPoseGhosts(int32 LethalPointCount, float InvulnerableScale);

	/**
	 * Spec v5 §2. Every frame, on the carrier's OWN machine only: continue the drawn trace from the
	 * newest drawn point to where the carrier actually is now, so their trace reaches their feet.
	 * Read the block comment above the implementation before touching it — it is the argument that
	 * this cannot make anything visible-but-not-lethal for a player who could be killed by it.
	 */
	void UpdatePredictedHead();

	/** Hides the predicted stub and zeroes its measurement. Cheap and safe to call every frame. */
	void HidePredictedHead();

	/**
	 * SPEC v8 §2. Appends one FTraceLocalPathSample when this machine is the one predicting the
	 * carrier, and drops everything the replicated point set has already caught up with.
	 *
	 * Called every frame from TickComponent, BEFORE UpdatePredictedHead and OUTSIDE its early-outs:
	 * the frames that matter most are the ones where the stub is currently declining to draw, and a
	 * recorder that only ran when the stub ran would have no path to offer on exactly those frames.
	 */
	void RecordLocalPathSample();

	/** Grows the predicted-head pool to cover element @p ElementIndex. False once its cap is hit. */
	bool EnsurePredictedElement(int32 ElementIndex);

	/** True when a posed Mannequin ghost can actually be drawn (art present AND material usable). */
	bool AreCharacterGhostsEnabled() const;

	/** The live skeletal mesh the ghosts copy their pose from, or null if this build has no art. */
	USkeletalMeshComponent* GetGhostSourceMesh() const;

	void HideSmearFrom(int32 FirstElementIndex);
	void ReleasePoseGhostsFrom(int32 FirstGhostIndex);
	void ClearGhostRecords();

	/**
	 * Local, cosmetic anti-whiteout guard: dims an after-image's emissive as the LOCAL camera gets
	 * close to it, so an eye inside the trace does not take an unattenuated unlit emissive slab at
	 * full frame width. Never changes the lethal volume. Runs every frame — see the implementation.
	 *
	 * It ALSO owns the owner-only near cull (the last resort behind the fade), for the same reason it
	 * owns the fade: both are functions of where the local camera is this frame, and the camera moves
	 * continuously while the geometry does not.
	 */
	void ApplyProximityGlowFade();

	/** Grows the smear pool to cover element @p ElementIndex. False once the pool cap is hit. */
	bool EnsureSmearElement(int32 ElementIndex);

	/** Grows the ghost pool to cover ghost @p GhostIndex. False once the pool cap is hit. */
	bool EnsurePoseGhost(int32 GhostIndex);

	/** Shared setup for one pooled smear piece. */
	UStaticMeshComponent* CreatePooledMesh(UStaticMesh* SourceMesh, UMaterialInstanceDynamic*& OutMaterial);

	void UpdateTeamColor();
	void CacheMeshMetrics();
	void DestroyVisualPool();
};
