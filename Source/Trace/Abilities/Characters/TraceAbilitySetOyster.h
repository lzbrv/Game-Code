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
//             *** SPEC v19 §4.3 MOVES THE DROP TO THE OTHER END OF THE DASH. *** Verbatim: "Oyster's
//             jar spawns at the END of his dash, not the beginning." Everything else about the jar is
//             unchanged — one per dash, same 4 s, same cap, same break. Only the place it lands moved,
//             from where he left to where he arrives.
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

	// --- PASSIVE: a jar at the END of every dash (spec v19 §4.3) --------------------------------------

	/**
	 * The framework's dash hooks. Both are wired now — UTraceCharacterMovementComponent::BeginDash
	 * calls NotifyDashStarted and the frame the dash clock runs out calls NotifyDashEnded — on the
	 * authority and on the owning client, and never on a replayed move.
	 *
	 * SPEC v19 §4.3 makes OnDashEnded the one that drops the jar; OnDashStarted now only ARMS it. The
	 * arm/drop pair is latched so the hook and the IsDashing() poll in TickAbilities (kept as the
	 * backstop for any dash that ends without the hook firing, e.g. the clock being cleared out from
	 * under it) cannot between them produce two jars for one dash.
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
	 * it. Reached from ATracePlayerController's jump binding through
	 * UTraceAbilityComponent::HandleJumpPressed; TickAbilities also detects a jump off the ground and
	 * applies the boost on the way up, and the two share one latch so a press cannot boost twice.
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

	/**
	 * HARNESS. The Pickler lob, without the cooldown or the input. Server only.
	 *
	 * Calls the same ThrowPickler() the E key reaches through ActivateAbility(), so a measurement taken
	 * here is a measurement of the shipping throw and not of a second implementation of it.
	 */
	ATraceOysterJar* DebugThrowPickler();

	/** HARNESS. Attempts the jar jump right now. Returns true if it fired. */
	bool DebugTryJarJump();

	/** HARNESS. Clears the field so one measurement cannot be polluted by the jars of the last. */
	void DebugDestroyAllJars() { DestroyAllJars(); }

private:
	/**
	 * "Max 3; a fourth despawns the OLDEST." Kept in spawn order, so the cap is a pop from the front
	 * and never a scan for a minimum. Weak, because a jar can be broken or expire out from under it.
	 */
	TArray<TWeakObjectPtr<ATraceOysterJar>> LiveJars;

	/** Edge state for the dash poll alone. Server only. */
	bool bWasDashing = false;

	/**
	 * TRUE between the opening edge of a dash and its closing edge, whichever of the hook or the poll
	 * saw each. It is what stops the two routes from treating one dash as two.
	 */
	bool bDashTracked = false;

	/**
	 * SPEC v19 §4.3. One jar per dash, dropped at the END, by whichever route notices the end first.
	 *
	 * TRUE from the opening edge until the jar has been dropped; the drop clears it, so the closing
	 * hook and the closing poll cannot both drop one for the same dash.
	 */
	bool bDashJarOwedForThisDash = false;

	/** Ground-state edge for the jar-jump poll. */
	bool bWasOnGround = false;

	/** Where he stood on the last tick, so a jump can ask "was I on a jar" AFTER leaving the ground. */
	FVector LastGroundedLocation = FVector::ZeroVector;

	/** Absolute match time of the last jar jump; a short latch so two routes cannot both boost. */
	float LastJarJumpMatchTime = -100.f;

	/** Server. Spawns a jar and enforces the max of 3. @p LaunchVelocity zero places it on the ground. */
	ATraceOysterJar* SpawnJar(const FVector& SpawnLocation, const FVector& LaunchVelocity, bool bPickler);

	/**
	 * Server. THE ONE PLACE A PICKLER JAR LEAVES HIS HAND. ActivateAbility() and DebugThrowPickler()
	 * both go through it, so the harness cannot measure a throw the game does not make.
	 */
	ATraceOysterJar* ThrowPickler();

	/** Server. The opening edge of a dash, from either route. Arms the debt exactly once. */
	void NoteDashBegan();

	/** Server. Settles the debt from bDashJarOwedForThisDash — the one place the dash jar is dropped. */
	void DropOwedDashJar();

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
