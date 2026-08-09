// Trace — OYSTER (spec v14 §6).
//
// ===================================================================================================
// THE DOC, VERBATIM
// ===================================================================================================
//
//   PASSIVE   "leaves a poison jar at the START OF EVERY DASH, including while carrying the Core. An
//             enemy touching a jar breaks it, poisoning nearby enemies: 3 damage every 0.5 s for 4 s,
//             and -30% speed for 4 s. Jars last 4 s on the ground. Max 3; a fourth despawns the
//             oldest."
//
//   MOVEMENT  "jumping while stood on one of his jars breaks it and boosts him upward."
//
//   ACTIVATED "Pickler: lobs a jar that, ON LANDING, deals 30 damage in an area and pulls enemies
//             within a small radius toward it."  Plus the doc's own clarification: "The jar does not
//             explode upon landing, it is the same as his other jars, which stay behind for a short
//             period until broken."   [ASSUMPTION] 20 s cooldown; §6 leaves it unspecified. FLAGGED.
//
// ===================================================================================================
// *** OYSTER IS THE PASS'S BIGGEST CARRIER-RULE RISK. THIS IS THE LIST. ***
// ===================================================================================================
//
// Spec §4 names him twice by name. He has FOUR ways to touch another player, and every one of them
// is refused on a Core carrier:
//
//   1. poison damage    3 per 0.5 s   Damage   ATraceOysterJar::Burst -> UTraceOysterPoisonComponent
//                                              -> ApplyAbilityDamage. Refused, no knob.
//   2. poison slow      -30%          Control  re-decided EVERY FRAME in the poison component, so a
//                                              player who picks up the Core mid-poison stops being
//                                              slowed within a frame.
//   3. Pickler damage   30 in an area Damage   ATraceOysterJar::FireLandingEffect -> ApplyAbilityDamage.
//   4. Pickler pull     toward the jar Control ATraceOysterJar::FireLandingEffect -> CanAffectTarget.
//
// There is no fifth. Nothing in this file writes another player's health or velocity: this class
// spawns jars and moves Oyster, and the jars do the touching. That is the point of the split — one
// place to audit, and TraceOyster::CarrierTally() counts all four vectors independently so a green
// run cannot mean "three of the four were never fired".
//
// Trace.Oyster.CarrierTest runs both arms, red first, over all four.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetOyster.generated.h"

class ATraceOysterJar;

UCLASS()
class TRACE_API UTraceAbilitySetOyster : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Oyster; }

	// --- lifecycle -----------------------------------------------------------------------------------
	virtual void OnEquipped() override;
	virtual void OnUnequipped() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// --- PASSIVE: a jar at the start of every dash ----------------------------------------------------

	/**
	 * The framework's dash hook. NOTHING CALLS IT YET (a cross-file need in
	 * Movement/TraceCharacterMovementComponent.cpp, filed by both foundation reports), so
	 * TickAbilities also watches ATraceCharacter::IsDashing() for a rising edge and drops the jar
	 * itself. Both routes go through the same latch, so wiring the hook cannot produce two jars.
	 *
	 * "including WHILE CARRYING THE CORE" — there is deliberately no carrier test on Oyster's own
	 * side of this. The choke point governs what a jar does to other players, never what Oyster is
	 * allowed to do while holding the Core.
	 */
	virtual bool OnDashStarted(const FVector& DashDirection) override;
	virtual void OnDashEnded(bool bReachedFullDistance) override;

	// --- MOVEMENT: the jar jump ------------------------------------------------------------------------

	/**
	 * "jumping while stood on one of his jars breaks it and boosts him upward."
	 *
	 * Returns TRUE to consume the jump, so the boost REPLACES the normal jump rather than adding to
	 * it. Also unreachable today (the same cross-file need), so TickAbilities detects a jump off the
	 * ground and applies the boost on the way up; the two share one latch.
	 */
	virtual bool OnJumpPressed() override;

	// --- ACTIVATED: Pickler ----------------------------------------------------------------------------

	virtual float GetActivatedCooldownSeconds() const override;
	virtual bool  ActivateAbility() override;

	// --- state, for the HUD and for Trace.Oyster.Verify --------------------------------------------------

	/** Live jars this Oyster owns, oldest first. Server only — jars are not tracked on clients. */
	int32 GetLiveJarCount() const;

	/** HARNESS. Places a jar on the ground at @p Location, skipping the dash / the lob. Server only. */
	ATraceOysterJar* DebugSpawnJarAt(const FVector& Location, bool bPickler);

	/** HARNESS. The dash jar, without a dash. Server only. */
	ATraceOysterJar* DebugDropDashJar();

	/** HARNESS. Attempts the jar jump right now. Returns true if it fired. */
	bool DebugTryJarJump();

private:
	/**
	 * "Max 3; a fourth despawns the OLDEST." Kept in spawn order, so the cap is a pop from the front
	 * and never a scan for a minimum. Weak, because a jar can be broken or expire out from under it.
	 */
	TArray<TWeakObjectPtr<ATraceOysterJar>> LiveJars;

	/** Rising-edge state for the dash poll. Server only. */
	bool bWasDashing = false;

	/** One jar per dash, whichever route noticed the dash first. */
	bool bDashJarSpawnedThisDash = false;

	/** Ground-state edge for the jar-jump poll. */
	bool bWasOnGround = false;

	/** Where he stood on the last tick, so a jump can ask "was I on a jar" AFTER leaving the ground. */
	FVector LastGroundedLocation = FVector::ZeroVector;

	/** Absolute match time of the last jar jump; a short latch so two routes cannot both boost. */
	float LastJarJumpMatchTime = -100.f;

	/** Server. Spawns a jar and enforces the max of 3. @p Velocity zero places it on the ground. */
	ATraceOysterJar* SpawnJar(const FVector& Location, const FVector& Velocity, bool bPickler);

	/** Server. Drops LiveJars entries that have gone and pops the oldest while over the cap. */
	void PruneJars();

	/** Server. Destroys every live jar. Half time, death, character change. */
	void DestroyAllJars();

	/** The jar of HIS OWN nearest to @p Location within the jar-jump radius, or null. */
	ATraceOysterJar* FindOwnJarNear(const FVector& Location) const;

	/** Applies the upward boost. Server and owning client, from the same latch. */
	bool DoJarJump(const FVector& FromLocation);

	/** True on the machines that actually simulate this pawn. */
	bool ShouldDriveMovement() const;

	/** Server. Mirrors the jar count into the replicated scratch pad for the HUD. */
	void PublishState();
};
