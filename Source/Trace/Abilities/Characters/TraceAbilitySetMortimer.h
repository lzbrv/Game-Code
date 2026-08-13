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
//   LIVE   the Core THROW cap. ATraceCore::GetThrowChargeScaleForHold(), two call sites.
//
//   NOT    the dash COOLDOWN (Demo 20 item 2's second half). GetDashCooldownScale() below returns
//   LIVE   1.25 and NOTHING CALLS IT: UTraceCharacterMovementComponent::GetDashCooldown() still
//          reads the shared UTraceSettings::DashCooldown flat. The whole fix is one multiplication
//          in that function; it is written out verbatim in the knob's comment in TraceSettings.h and
//          in TraceAbilityTraits::GetDashCooldownScale's. Source/Trace/Movement/ was owned by
//          another agent in the pass that added this, which is the only reason it is not done.
//   NOT    the MANTLE. Not "unwired" — ABSENT. IsMantling(), CanAttemptMantle(), TryBeginMantle(),
//   LIVE   ApplyMantleVelocity() and EndMantle() were deleted from the movement component in
//          d2319b2 along with six pieces of saved-move state, eight tuning knobs and one CVar.
//          AllowsMantle() and GetMantleGenerosityScale() below are the GATE for a feature that has
//          no body. See the next block.
//
// ===================================================================================================
// THE MANTLE, AND WHY IT CANNOT COME BACK FOR EVERYBODY
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
// probes hit the probing pawn — one AddIgnoredActor took it to 7/8) and the fixed version is what
// `git show dffea7c` restores.
//
// SO THE RECOVERY IS GATED, NOT GLOBAL. TraceAbilityTraits::IsMantleAllowed() is false for every
// character but Mortimer and is meant to be the FIRST question CanAttemptMantle() asks, so for the
// other nine there is no probe, no MOVE_Flying, no pull-up, and therefore no new way for a client and
// a server to disagree about a ledge. "Do not bring the ledge bug back for everyone" becomes a
// property of the control flow rather than a promise.
//
// *** DEMO 20 STATUS: STILL ABSENT, AND DELIBERATELY NOT FAKED. ***
//
// There is no CanAttemptMantle() to put that first question in. Restoring the mantle means restoring
// five functions, six pieces of FSavedMove_Trace state, the CanCombineWith() bar and the Clear /
// SetMoveFor / PrepMoveFor round trip — ALL of it inside
// Source/Trace/Movement/TraceCharacterMovementComponent.{h,cpp}, which this pass does not own.
//
// The tempting shortcut is to drive the pull-up from HERE — this class can reach the movement
// component, set MOVE_Flying and write Velocity every tick without touching Movement/ at all. THAT
// SHORTCUT IS THE BUG THE DELETION FIXED. A pull-up outside the saved-move pipeline is a position the
// client computes and the server never replays, which is a correction on every frame of every mantle:
// exactly the 1.00 corrections per contact and 88.11 uu of error measured above, re-introduced by the
// feature that was supposed to hide them. Running it server-only instead just moves the corrections
// onto the joined client. So it is NOT implemented, rather than implemented wrongly and reported as
// done, and Trace.Mortimer.Verify says MISSING for it rather than NOT WIRED — the two words mean
// different amounts of work and the last report used the wrong one.
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

	// =============================================================================================
	// MOVEMENT — the mantle, recovered from dffea7c and gated to him alone
	// =============================================================================================

	/** True unless UTraceSettings::bMortimerCanMantle has been switched off. See the header. */
	bool AllowsMantle() const;

	/** §3: "30% more generous". 1.30, from UTraceSettings::MortimerMantleGenerosity. */
	float GetMantleGenerosityScale() const;

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
};
