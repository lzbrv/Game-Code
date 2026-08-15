// Trace — MORTIMER (spec v19 §3, Demo 18).
//
// ===================================================================================================
// THE DOC, VERBATIM
// ===================================================================================================
//
//   PASSIVE   "his dash is 75% shorter" and "he can charge the core up to 2x as long as anyone else,
//             on the same linear scale, so he throws it twice as far"
//
//   MOVEMENT  "he can mantle onto objects, 30% more generous than the old in-game mantle"
//
//   ACTIVATED "only while carrying the core AND standing on the ground or the top of an object, a
//             blast that knocks nearby enemies away"
//
// DEMO 20 amends two of those three lines:
//
//   ITEM 2   "Change mortimer's dash to be 40% of a normal one instead of 25%, but increase
//            mortimer's dash cooldown by 25%"
//   ITEM 3   "Mortimer's quake isn't working, Mortimer's mantle isn't working"
//
// DEMO 21 finally delivers the movement line and bends the passive:
//
//   ITEM 6   "Add a mantle for Mortimer, as the original instructions requested. It doesn't have to
//            be the old mantle - make a new one which acts the same, but for just him."
//   ITEM 7   "after the original 100% charge window has passed, add a .6x modifier to the linear
//            scaling of his throw charge"
//
// ===================================================================================================
// WHAT IS LIVE IN THIS FILE, AND WHAT IS A KNOB WAITING FOR SOMEBODY ELSE'S ONE-LINER
// ===================================================================================================
//
// THIS LIST IS THE ONE THING IN THIS HEADER THAT MUST NEVER GO STALE. Demo 18's version of it said
// the dash reach was "not live" long after it had been wired, and Demo 20 arrived with an integrator
// quoting that sentence as fact and re-reporting a working passive as broken. Correct it in the same
// edit as the code, every time.
//
//   LIVE   QUAKE, end to end — the posture gate, the victim search, the choke point and the launch.
//          Trace.Mortimer.BlastCarrierTest proves the choke point, red arm first, and
//          Trace.Mortimer.QuakeTest drives the SHIPPED E-key path and photographs the result.
//   LIVE   the dash REACH. UTraceCharacterMovementComponent::GetDashSpeed() multiplies by
//          TraceAbilityTraits::GetDashDistanceScale(). Demo 20 item 2 moves it 0.25 -> 0.40.
//   LIVE   the dash COOLDOWN. GetDashCooldown() multiplies by GetDashCooldownScale(). This list said
//          NOT LIVE for a whole demo after the call site landed in v23. Measured by
//          Trace.Mortimer.DashTest, which is the thing to believe.
//   LIVE   the Core THROW cap. ATraceCore::GetThrowChargeScaleForHold(), two call sites.
//   LIVE   DEMO 21 ITEM 7, the 0.6x past the original 100% point. Same one function.
//          Trace.Mortimer.ThrowTest throws four real Cores and measures the RANGE.
//   LIVE   DEMO 21 ITEM 6, THE MANTLE. OnJumpPressed() -> TryMantle(), gated on
//          TraceAbilityTraits::IsMantleAllowed(). Trace.Mortimer.MantleTest, red arm first.
//
// ===================================================================================================
// THE MANTLE: WHY IT WENT, WHAT CAME BACK, AND WHY THIS ONE IS NOT THE OLD ONE
// ===================================================================================================
//
// It was added in `dffea7c` (Demo 5) and DELETED in `d2319b2` (Demo 11). The deletion commit is not a
// tidy-up, it is a measurement: the "rubber banding on the edge of a raised section" the mantle was
// layered over turned out to be a genuine client prediction desync, and removing the mantle both
// forced that fix and proved it on a joined client at 40 ms —
//
//     shipped: 5/5 contacts, 0.00 corrections per contact, worst error 0.00 uu
//     legacy:  1.00 corrections per contact, worst error 88.11 uu, speed kept as low as 0.521
//
// The mantle itself was ALSO broken when it was written (0/8 successful mantles, because the ledge
// probes hit the probing pawn — one AddIgnoredActor took it to 7/8).
//
// SO THE RECOVERY IS GATED, NOT GLOBAL. TraceAbilityTraits::IsMantleAllowed() is false for every
// character but Mortimer and is the FIRST question TryMantle() asks, so for the other nine there is
// no probe, no launch, no pull-up, and therefore no new way for a client and a server to disagree
// about a ledge. "Do not bring the ledge bug back for everyone" is a property of the control flow.
//
// *** WHAT THIS ONE IS, IN FOUR SENTENCES. ***
//
//   1. IT IS ON THE JUMP KEY, NOT AUTOMATIC. UTraceCharacterAbilitySet::OnJumpPressed() is an
//      existing hook that ATracePlayerController runs LOCALLY and then re-runs on the SERVER through
//      UTraceAbilityComponent::ServerHandleJumpPressed. So both ends execute the same code from the
//      same press. The legacy mantle was attempted every frame from OnMovementUpdated, which is the
//      thing that had to be inside the saved-move pipeline to be safe.
//   2. IT IS TWO IMPULSES, NOT A PER-FRAME POSITION. ACharacter::LaunchCharacter, once to rise past
//      the lip and once — when his feet clear it — to carry him over. Nothing writes Velocity every
//      frame, nothing sets MOVE_Flying, nothing adds saved-move state, and the movement component is
//      not edited at all. It is exactly the shape Rocco's second jump and Oyster's jar jump already
//      have, and it inherits their honesty about prediction (see 4).
//   3. THE GEOMETRY IS MINED FROM dffea7c, because that part was right: three probe heights rather
//      than one, the ladder-to-the-sky guards (a degenerate hit, a non-vertical face, a destination
//      directly overhead), "you may not climb people", a walkable top, and a capsule-shaped clearance
//      test at the destination BEFORE anything moves.
//   3b. THE SLIDE JUMP STILL OUTRANKS IT; THE WALL JUMP DELIBERATELY NO LONGER DOES. The legacy rule
//      was "a wall jump outranks a mantle", and it was right for a mantle that fired BY ITSELF. This
//      one fires because the player pressed jump, and a player pressing jump while airborne against a
//      ledge he can climb wants to be on top of it. A tall wall has no walkable top inside the height
//      ceiling, so the probe finds nothing and the press falls through to the wall jump untouched.
//   4. THE PREDICTION CAVEAT IS STATED, NOT CLAIMED AWAY. PendingLaunchVelocity is not saved-move
//      state, so a correction that replays the press will not replay the launch — the identical,
//      documented limitation ATracePlayerController's own comment records for the two jump abilities
//      that shipped before this one. Running the same hook on both ends is what makes it survive in
//      practice. This is a smaller exposure than the legacy mantle's (two impulses versus a whole
//      flight's worth of positions) and it is not zero.
//
// The owner explicitly relaxed the requirement for Demo 21: "It doesn't have to be the old mantle —
// make a new one which acts the same, but for just him." This is that.
//
// ===================================================================================================
// QUAKE AND THE CARRIER CHOKE POINT — THE IRONY, STATED PRECISELY
// ===================================================================================================
//
// A knockback is a Control effect, so every victim goes through
// UTraceAbilityComponent::CanAffectTarget(Victim, Control) and there is NO carrier test in this file.
//
// The irony the spec flags: MORTIMER IS HIMSELF THE CARRIER WHEN HE CASTS IT. That is fine and it is
// the reason the choke point is asked about the TARGET and never about the instigator — a rule that
// asked "is the caster carrying?" would refuse this ability outright, and a rule that skipped the
// check "because he is the carrier anyway" would be the bug.
//
// AND THE TEST THAT MATTERS IS THE ONE THAT LOOKS VACUOUS. With one Core in play, an enemy carrier
// cannot exist while Mortimer is carrying, so in a real match the choke point can never fire for
// Quake. That is exactly why the harness calls ApplyBlastTo() DIRECTLY on a live carrier instead of
// waiting for a situation the game cannot produce: what is being proved is that the code path is
// routed, so that it stays routed on the day a second Core, a practice range, or a dropped-Core rule
// makes the situation reachable. A rule that is only correct because the situation never arises is
// not a rule, it is a coincidence.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetMortimer.generated.h"

class ATraceCharacter;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;

/**
 * Why Quake refused. Returned by CheckBlastPosture() so the log, the HUD toast and the harness can
 * all say the same sentence, and so "it did nothing" is never the whole story a player gets.
 */
UENUM()
enum class ETraceMortimerBlastRefusal : uint8
{
	/** It would fire. */
	Allowed = 0,
	/** No pawn, dead, or no movement component. */
	NoPawn,
	/** "only while carrying the core" — he is not. */
	NotCarryingCore,
	/** "standing on the ground or the top of an object" — he is in the air. */
	Airborne
};

TRACE_API const TCHAR* TraceMortimerBlastRefusalToString(ETraceMortimerBlastRefusal Reason);

/**
 * *** QUAKE'S SHOCKWAVE. DEMO 20 ITEM 3. THE WHOLE OF "the quake isn't working". ***
 *
 * An expanding ring of light at Mortimer's feet, drawn once per cast, that reaches
 * UTraceSettings::MortimerBlastRadiusUU — the ability's REAL radius, so what the player sees is the
 * area the rule actually used and not a decorative approximation of it — and then fades out.
 *
 * COSMETIC ONLY, AND THAT IS LOAD-BEARING. It has no collision on any channel, deals nothing, blocks
 * nothing, and no rule anywhere asks it a question. Deleting this class would change no gameplay
 * number; it exists because the ability had NO output a player could perceive. Before this, a Quake
 * cast with nobody inside 600 uu produced exactly one observable thing — the HUD's cooldown ring
 * greying out — and a Quake cast that was REFUSED produced nothing at all, because
 * UTraceAbilityComponent::TryActivate() drops the FText that CanActivate() fills in.
 *
 * SERVER-SPAWNED AND REPLICATED, like ATraceRippleActor and for the same two reasons: everybody in
 * the match must see the same blast, and the owning client deliberately predicts nothing about Quake
 * (see UTraceAbilitySetMortimer::ActivateAbility). bAlwaysRelevant because a 600 uu ring is exactly
 * the kind of thing net culling would drop for the enemy who is about to be launched by it.
 *
 * THE RING IS BUILT FROM /Engine/BasicShapes CYLINDERS, not from a particle system or a Niagara
 * asset. That is copied from ATraceRippleActor::AddRing deliberately: this project generates its
 * content with a Python script, an install that has not run it has no M_TraceNeon, and an effect that
 * silently draws nothing on half the team's machines would recreate the bug it is fixing. The neon
 * material is used when present and BasicShapeMaterial when it is not.
 */
UCLASS()
class TRACE_API ATraceMortimerQuakeWave : public AActor
{
	GENERATED_BODY()

public:
	ATraceMortimerQuakeWave();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * AUTHORITY ONLY. Call immediately after SpawnActor, before anything can tick.
	 *
	 * @param InRadiusUU   the radius the ring grows to. Pass the SAME number the blast used.
	 * @param InSeconds    how long the whole expand-and-fade takes.
	 */
	void InitialiseWave(float InRadiusUU, float InSeconds);

	/** The radius the ring is growing to, uu. For the harness. */
	float GetWaveRadiusUU() const { return WaveRadiusUU; }

	/** How far through the animation this machine is, 0..1. For the harness. */
	float GetWaveAlpha() const;

	/** How many bead instances are actually registered for drawing. ZERO MEANS INVISIBLE. */
	int32 GetDrawnBeadCount() const;

protected:
	/** REPLICATED. The blast radius this wave is drawing. */
	UPROPERTY(Replicated)
	float WaveRadiusUU = 600.f;

	/** REPLICATED. Total animation length, seconds. */
	UPROPERTY(Replicated)
	float WaveSeconds = 0.9f;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root = nullptr;

	/** The ring. One component, MortimerQuakeWave::BeadCount instances, re-transformed every frame. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> RingMesh = nullptr;

private:
	/**
	 * Places the beads once. Split from Tick because a client may tick before the replicated radius
	 * has landed, in which case there is nothing to build yet — same idempotent shape as
	 * ATraceRippleActor::BuildRingsIfNeeded.
	 */
	void BuildIfNeeded();

	/** Moves the beads out to @p Alpha of the radius and fades the material. */
	void UpdateRing(float Alpha);

	float Elapsed = 0.f;
	bool bBuilt = false;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> BeadMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> NeonMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FallbackMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RingMID = nullptr;
};

/**
 * *** DEMO 21 ITEM 6. Everything TryMantle() worked out about the ledge in front of Mortimer. ***
 *
 * A plain struct, not a USTRUCT: nothing replicates it and nothing reflects it. It exists so the
 * probe and the launch are two readable halves instead of one 300-line function (the legacy
 * TryBeginMantle was 323 lines and is the reason this file's spec entry says "smaller"), and so
 * Trace.Mortimer.MantleTest can ask "what did the probe SEE" separately from "did he get up".
 *
 * @c Why is filled in on every refusal and is the whole diagnostic surface: "the mantle did nothing"
 * is the single most useless bug report this feature can generate, and the legacy implementation
 * spent an entire pass being debugged from a log line that only said "degenerate".
 */
struct FTraceMortimerLedge
{
	/** True only when every test passed and Destination is a place he can stand. */
	bool bFound = false;

	/** Where his CAPSULE CENTRE ends up, i.e. the ledge top plus a half-height and a hair. */
	FVector Destination = FVector::ZeroVector;

	/** Horizontal, normalised, pointing into the ledge. The direction the probe ran. */
	FVector Forward = FVector::ZeroVector;

	/** Top of the ledge, in world Z. */
	float TopZ = 0.f;

	/** Top of the ledge above his FEET, uu. The number the height window is applied to. */
	float LedgeHeightUU = 0.f;

	/** The window that was applied, after generosity. Printed on refusal so the knob is visible. */
	float FloorUU = 0.f;
	float CeilingUU = 0.f;
	float ReachUU = 0.f;

	/** His own jump apex, uu — the base CeilingUU is derived from. See MortimerMantleApexReach. */
	float JumpApexUU = 0.f;

	/** Why it was refused. Empty when bFound. */
	FString Why;
};

UCLASS()
class TRACE_API UTraceAbilitySetMortimer : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Mortimer; }

	// =============================================================================================
	// PASSIVE — the two halves. Both are read through TraceAbilityTraits, never by casting.
	// =============================================================================================

	/**
	 * DEMO 20 ITEM 2: "40% of a normal one instead of 25%". 0.40 of everybody's dash REACH, from
	 * UTraceSettings::MortimerDashDistanceScale. (§3's original wording was "75% shorter" = 0.25.)
	 *
	 * It scales the dash's SPEED so that its DURATION — and therefore the trace it leaves, the parry
	 * window and the dash-hit sweep — is untouched. See the knob's comment for why that matters.
	 *
	 * LIVE: UTraceCharacterMovementComponent::GetDashSpeed() multiplies by this through the trait.
	 */
	float GetDashDistanceScale() const;

	/**
	 * DEMO 20 ITEM 2: "increase mortimer's dash cooldown by 25%". 1.25, from
	 * UTraceSettings::MortimerDashCooldownScale, applied to the shared UTraceSettings::DashCooldown.
	 *
	 * *** NOT LIVE. NOTHING CALLS THIS YET. *** The one line it needs is in
	 * UTraceCharacterMovementComponent::GetDashCooldown() and is written out verbatim in
	 * TraceAbilityTraits::GetDashCooldownScale's comment. Trace.Mortimer.Verify prints NOT LIVE for
	 * it and Trace.Mortimer.DashTest goes red on it; do not delete either warning without the fix.
	 */
	float GetDashCooldownScale() const;

	/**
	 * §3: "up to 2x as long ... on the same linear scale". 2.0, from
	 * UTraceSettings::MortimerThrowChargeHoldScale.
	 *
	 * It multiplies the CAP on t = HeldSeconds / CoreThrowChargeSeconds inside
	 * ATraceCore::GetThrowChargeScaleForHold and nothing else, so the shipped line
	 * Power = Floor + (1 - Floor) x t is extrapolated rather than replaced.
	 */
	float GetThrowChargeHoldScale() const;

	/**
	 * DEMO 21 ITEM 7: "after the original 100% charge window has passed, add a .6x modifier to the
	 * linear scaling of his throw charge". 0.6, from UTraceSettings::MortimerThrowChargePastFullScale.
	 *
	 * It multiplies ONLY the charge accumulated past the original 100% point, so the first 100% of his
	 * wind-up is byte-identical to everybody else's — the term this scales is zero there, for him and
	 * for them. LIVE: ATraceCore::GetThrowChargeScaleForHold().
	 *
	 * Trace.Mortimer.ThrowPastFull 0 is the red arm and forces this to 1.0, i.e. the pre-Demo-21
	 * straight line.
	 */
	float GetThrowChargePastFullScale() const;

	// =============================================================================================
	// MOVEMENT — THE MANTLE. DEMO 21 ITEM 6. See the header block for the shape and its caveat.
	// =============================================================================================

	/** True unless UTraceSettings::bMortimerCanMantle has been switched off. See the header. */
	bool AllowsMantle() const;

	/** §3: "30% more generous". 1.30, from UTraceSettings::MortimerMantleGenerosity. */
	float GetMantleGenerosityScale() const;

	/**
	 * *** THE MANTLE'S FIRST HALF, ON THE JUMP KEY. ***
	 *
	 * Runs on the owning client AND on the server (ATracePlayerController::OnJumpStarted, then
	 * UTraceAbilityComponent::ServerHandleJumpPressed). Returns TRUE only when a mantle actually
	 * started, because a true return CONSUMES the jump: a press that finds no ledge must fall through
	 * to the ordinary jump, or Mortimer would simply stop being able to jump near walls.
	 */
	virtual bool OnJumpPressed() override;

	/**
	 * *** THE MANTLE'S SECOND HALF. *** 20 Hz on every machine, but it does nothing at all unless a
	 * mantle is in flight on THIS machine — and only the owning client and the server ever start one,
	 * so a simulated proxy's copy of this set never enters the body.
	 *
	 * It waits for his feet to clear the lip and then applies the ONE forward impulse that carries him
	 * over it. Two impulses, not a per-frame position: see the header.
	 */
	virtual void TickAbilities(float DeltaSeconds) override;

	/** Cancels a mantle in flight. */
	virtual void OnPawnDied() override;
	virtual void OnUnequipped() override;

	/**
	 * THE PROBE, PURE AND PUBLIC. Reads the world, writes nothing, and answers "is there a ledge in
	 * front of him that he may climb". Public because Trace.Mortimer.MantleTest has to be able to ask
	 * what the probe SAW on a run where the pull-up did not happen — "it did nothing" and "it found no
	 * ledge because the window is 40 uu too short" are different bugs.
	 *
	 * @param bFromGround  apply the ground rule: on the ground a ledge at or below his own jump apex
	 *                     is refused, because a plain jump already reaches it and stealing the jump
	 *                     key for it would be a regression. Ignored while airborne.
	 */
	bool ProbeLedge(FTraceMortimerLedge& Out, bool bFromGround) const;

	/**
	 * Starts a mantle if one is available. THE WHOLE GATE, IN ORDER: the character gate, the red arm,
	 * the rate limit, "a wall jump outranks a mantle", then the probe, then the launch.
	 *
	 * Public so the harness can drive the rule without synthesising an input; the SHIPPED path into it
	 * is OnJumpPressed() and that is what Trace.Mortimer.MantleTest presses.
	 */
	bool TryMantle();

	/** True while a mantle's forward push is still pending. */
	bool IsMantling() const { return MantlePushDeadline > 0.f; }

	/** Mantles started since this set was equipped. Dev instrumentation, exactly like BlastCount. */
	int32 GetMantleCount() const { return MantleCount; }

	/** Forward pushes actually delivered. A start with no push is a mantle that stalled below the lip. */
	int32 GetMantlePushCount() const { return MantlePushCount; }

	// =============================================================================================
	// ACTIVATED — QUAKE
	// =============================================================================================

	/** §3's two conditions, as one question, with the reason. Pure; safe on any machine. */
	ETraceMortimerBlastRefusal CheckBlastPosture() const;

	/** The framework's pre-flight. Wraps CheckBlastPosture() and phrases it for the player. */
	virtual bool CanActivate(FText& OutReason) const override;

	/**
	 * Fire Quake. Returns true — and therefore charges the cooldown — only when the posture held.
	 *
	 * On the SERVER it finds and launches every victim. On the OWNING CLIENT it does nothing but
	 * agree that the press was legal: a knockback is somebody else's position and predicting it would
	 * show this player enemies flying who never moved.
	 */
	virtual bool ActivateAbility() override;

	/** §3 gives no number. [ASSUMPTION] UTraceSettings::MortimerBlastCooldownSeconds (20 s). */
	virtual float GetActivatedCooldownSeconds() const override;

	// =============================================================================================
	// The blast's two halves, public because the harness drives them directly. See the header.
	// =============================================================================================

	/**
	 * SERVER ONLY. Find every enemy inside the radius and launch them. Returns how many were moved.
	 *
	 * @param OutConsidered  how many living pawns were inside the radius at all, victims or not. The
	 *                       difference between this and the return value is what the choke point,
	 *                       friendly fire and line of sight refused, and a harness that cannot see it
	 *                       cannot tell "nobody was near" from "the rule fired".
	 */
	int32 RunBlast(int32& OutConsidered);

	/**
	 * SERVER ONLY. THE PER-VICTIM PATH, CHOKE POINT INCLUDED. One call, one victim, no radius test —
	 * so a harness can aim it at a Core carrier that a real match could never produce beside him.
	 *
	 * @return true only if the victim was actually launched.
	 */
	bool ApplyBlastTo(ATraceCharacter* Victim) const;

	/** Quakes this ability set has fired. Dev instrumentation; a harness reads it to prove a press landed. */
	int32 GetBlastCount() const { return BlastCount; }

	/**
	 * SERVER ONLY. DEMO 20 ITEM 3. Spawn the shockwave at @p Origin and hand back the actor.
	 *
	 * Public because Trace.Mortimer.QuakeTest has to be able to answer "did the cast produce a
	 * visible thing, and how many instances did it actually register for drawing" — the question
	 * ATraceElleGate's 60 unregistered ring segments are the standing example of. A harness that can
	 * only read a bool cannot tell a spawned-but-empty effect from a working one.
	 *
	 * @return the wave, or null on a client / dedicated server / missing world.
	 */
	ATraceMortimerQuakeWave* SpawnQuakeWave(const FVector& Origin);

	/** Shockwaves this set has spawned. Dev instrumentation, exactly like BlastCount. */
	int32 GetWaveCount() const { return WaveCount; }

private:
	/** Nothing but the launch: direction, falloff and LaunchCharacter. Assumes every rule has passed. */
	void LaunchVictim(ATraceCharacter* Victim, const FVector& FromLocation) const;

	/** bMortimerBlastNeedsLineOfSight's trace. True when the knob is off. */
	bool HasLineOfSightTo(const ATraceCharacter* Victim) const;

	/** Quakes fired by this set since it was equipped. Not replicated; server and local client each count their own. */
	int32 BlastCount = 0;

	/** Shockwaves spawned since it was equipped. Authority only — the client never spawns one. */
	int32 WaveCount = 0;

	// --- THE MANTLE'S ONLY STATE. Five plain members, none replicated. --------------------------
	//
	// NOT REPLICATED, AND THAT IS THE DESIGN. Both machines that matter compute this from the same
	// press against the same STATIC arena geometry, so there is nothing to agree about over the wire;
	// a simulated proxy never sets it because it never receives a jump press. Replicating it would be
	// inventing exactly the shared mutable ledge state the v12 §5 removal was about.

	/** Absolute MatchTimeNow() the pending forward push gives up at. 0 = no mantle in flight. */
	float MantlePushDeadline = 0.f;

	/** World Z his FEET must reach before the forward push fires — the lip, plus a hair. */
	float MantlePushAboveZ = 0.f;

	/** Horizontal, normalised. The direction he goes when the lip is cleared. */
	FVector MantlePushDirection = FVector::ZeroVector;

	/** uu/s of that push, derived from how far he still has to travel. */
	float MantlePushSpeed = 0.f;

	/** Absolute MatchTimeNow() before which no new mantle may start. MortimerMantleCooldownSeconds. */
	float MantleReadyTime = 0.f;

	/** Mantles STARTED and forward pushes DELIVERED. The difference is mantles that stalled. */
	int32 MantleCount = 0;
	int32 MantlePushCount = 0;

	/** Clears the five members above. One place, so an abandon and a completion cannot diverge. */
	void ClearMantle();
};
