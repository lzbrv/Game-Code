// Trace — LILY (spec v19 §3, Demo 18).
//
// ===================================================================================================
// THE DOC, VERBATIM
// ===================================================================================================
//
//   MOVEMENT  "an extra dash" — 2 normally, 3 while carrying the Core — and "she has only 60 health"
//
//   PASSIVE   "+30% wall-jump momentum bonus"  (hers alone; the global wall-jump numbers must not move)
//
//   ACTIVATED "Zip (30 s): for 5 s she can fly. Jump goes up at walking speed, slide/crouch goes down,
//             all other movement mechanics apply as usual. With the core the duration is halved."
//             And, separately: "If she activates it and then picks up the core, the remaining duration
//             is halved."
//
// ===================================================================================================
// THE SUBTLE CLAUSE, AND THE MISREADING IT IS GUARDING AGAINST
// ===================================================================================================
//
// The two Core sentences are the SAME 0.5 applied at two different moments, and the second one is the
// one that is easy to get wrong:
//
//     cast while carrying          5 s  ->  2.5 s     (halve the DURATION)
//     Core arrives 4 s into a Zip  1 s  ->  0.5 s     (halve WHAT IS LEFT)
//
// The wrong implementation — "on pickup, clamp the remaining time to the carrier duration" — gives
// 2.5 s in the second case, i.e. it LENGTHENS a Zip that was nearly over, and rewards picking the
// Core up late. Trace.Lily.ZipVerify stages exactly that case (1 s left, then a pickup) and asserts
// 0.5, because 2.5 is the answer the natural mistake produces and an assertion that cannot tell them
// apart is not an assertion.
//
// IT APPLIES ONCE PER PICKUP, not once per Zip. Drop the Core and take it again and what is left
// halves again. That is the literal reading, and it is the one that cannot be farmed by juggling.
//
// ===================================================================================================
// WHAT IS LIVE IN THIS FILE, AND WHAT IS WAITING ON SOMEBODY ELSE'S ONE-LINER
// ===================================================================================================
//
// *** LIVE, END TO END: ZIP. *** The flight, both halvings, the jump/crouch controls and the death
// stop are all in this file and run today. Trace.Lily.ZipVerify proves the halvings red-arm first.
//
// *** THE OTHER THREE ARE LIVE TOO, as of the pass that added the call sites. *** They are still
// implemented here and still exposed through TraceAbilityTraits (Abilities/TraceAbilityTypes.h); what
// changed is that the three slices that own the numbers now ask:
//
//     the charge pool   Movement/TraceCharacterMovementComponent.cpp  GetMaxDashCharges()
//     max health        Gameplay/TraceHealthComponent.cpp             GetMaxHealth()
//     the wall jump     Movement/TraceCharacterMovementComponent.cpp  the retention term in TryWallJump
//
// (An older version of this block said none of them were wired. It was true when it was written and
// it stopped being true without being edited, which is the failure mode every "not yet live" note in
// this project has. Trace.Lily.Verify prints what is actually read rather than what is intended.)
//
// ===================================================================================================
// DEMO 19, AND THE TWO THINGS IT CHANGED
// ===================================================================================================
//
// ITEM 4, verbatim: "Lily's E isn't working right. Pressing space once makes her continuously move up
// even after I let go, and pressing control doesn't make her come down. Half the speed she moves
// vertically at."
//
// The first two sentences are ONE bug with one cause — the framework's jump-RELEASE hook has no
// caller anywhere, so the climb latch could never be cleared and the crouch test could never be
// reached. See SampleClimbIntent() below; that is where the whole story is written down. The third
// sentence is the two speed scales, both now 0.5.
//
// ITEM 8: the extra dash charge is now hers only while she is NOT carrying the Core. See
// GetExtraDashCharges().
//
// ===================================================================================================
// HOW ZIP MOVES HER, AND THE LIMIT OF DOING IT FROM AN ABILITY SET
// ===================================================================================================
//
// It writes UCharacterMovementComponent::Velocity.Z from TickAbilities(), on the server and on the
// owning client, from state both machines compute the same way — the pattern Mace's suspend
// established and for the identical reasons (GravityScale is re-pushed from UTraceSettings on every
// simulated move, and FSavedMove_Trace lives in a slice this file does not own).
//
// *** TickAbilities RUNS AT 20 Hz AND PHYSICS RUNS AT 60+. *** So the vertical command is applied in
// ~50 ms lumps into a simulation that integrates gravity continuously, and a naive "set Z to 0 to
// hover" sags at about g x dt / 2 = 24.5 uu/s, i.e. roughly 120 uu over a full 5 s Zip. That is not
// acceptable for something the doc calls flying, so ApplyZip adds the closed-form correction for
// exactly one tick of gravity (see the comment there). It makes the MEAN vertical velocity the
// commanded one; it does not make the flight frame-accurate, and the honest fix is a movement mode in
// the prediction path. That is named in the report as a limitation, not hidden as a detail.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

#include "Abilities/TraceCharacterAbilitySet.h"
#include "Gameplay/TraceFxShapes.h"                       // ETraceFxBlend — the ACHIEVED blend is stored

#include "TraceAbilitySetLily.generated.h"

class ATraceCharacter;
class UAudioComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/** Lily's bits in FTraceAbilityNetState::Flags. Only one character is live per component, so these cannot collide. */
namespace TraceLilyFlags
{
	/**
	 * Zip is up. DELIBERATELY TraceAbilityFlags::EffectActive.
	 *
	 * Spec v19 §4.2 — "death wipes active abilities" — is enforced centrally by
	 * UTraceAbilityComponent::ApplyDeathStateWipe, which clears exactly EffectActive, MovementActive
	 * and EffectEndMatchTime. Choosing that bit and that field is what makes Zip stop on death for
	 * free, on every machine, without this file being trusted to remember.
	 */
	inline constexpr uint8 Zipping = 1 << 0;   // == TraceAbilityFlags::EffectActive
}

UCLASS()
class TRACE_API UTraceAbilitySetLily : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Lily; }

	// --- lifecycle -------------------------------------------------------------------------------
	virtual void OnEquipped() override;
	virtual void OnUnequipped() override;
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// =============================================================================================
	// THE FX ROUTER — FX_AUDIO_PLAN §2.1. Everything anybody but Lily can see or hear is here.
	// =============================================================================================
	//
	// WHY NONE OF IT IS IN ActivateAbility(). Zip is predicted: ActivateAbility runs on the server
	// AND on the owning client, so building the aura there would build it twice on a listen host and
	// leave every OTHER machine with nothing — which is the F10 blocker §1.2 exists to close. The
	// replicated Zipping flag is the one fact all three machine roles share, so the flag's EDGE is
	// the single producer of every visual and of the loop sound.
	//
	// THE OWNER LOSES HALF A ROUND TRIP on his own cast flash by that choice, and it is the right
	// trade here rather than a compromise: Zip's flash is a GROUND RING at her feet and she is in
	// first person, so the person who would notice the delay is the one person who cannot see the
	// effect at all. (Contrast §6's gunshot, which is predicted precisely because the shooter feels
	// its delay.)
	virtual void OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New) override;
	virtual void SyncClientFx(const FTraceAbilityNetState& Current) override;

	// =============================================================================================
	// MOVEMENT + PASSIVE — all three read through TraceAbilityTraits, never by casting
	// =============================================================================================

	/**
	 * §3: "an extra dash", as amended by DEMO 19 ITEM 8: *** ONLY WHILE SHE IS NOT CARRYING THE CORE. ***
	 *
	 * "Change Lily so she only has an extra dash when she is not carrying the core." Read against the
	 * shipped pool (BaseDashCharges 1, CarrierExtraDashCharges 1) this is:
	 *
	 *     free-running  1 + 1 (hers)     = 2      unchanged
	 *     carrying      1 + 1 (carrier's) = 2      was 3
	 *
	 * So the change is worth stating in one sentence: SHE NO LONGER STACKS HER CHARGE ON TOP OF THE
	 * CARRIER'S. She keeps two dashes at all times, and picking the Core up stops being a straight
	 * upgrade for her the way it is for everyone else.
	 *
	 * Written as "+1 unless carrying" rather than as the literals 2 and 2, for the same reason the
	 * addend was written that way in the first place: a retune of BaseDashCharges must still move her
	 * with everybody else.
	 */
	int32 GetExtraDashCharges() const;

	/** §3: "only 60 health". An ABSOLUTE, from UTraceSettings::LilyMaxHealth. 0 would mean "no opinion". */
	float GetMaxHealthOverride() const;

	/**
	 * §3: "+30% wall-jump momentum bonus". 1.30, from UTraceSettings::LilyWallJumpMomentumBonus.
	 *
	 * It multiplies the RETENTION term — the part of the launch that is the speed she arrived with —
	 * and never the flat outward impulse or the vertical multiplier. See the knob's comment.
	 */
	float GetWallJumpMomentumScale() const;

	// =============================================================================================
	// ACTIVATED — ZIP
	// =============================================================================================

	virtual bool CanActivate(FText& OutReason) const override;
	virtual bool ActivateAbility() override;

	/** §3: "Zip (30 s)". UTraceSettings::LilyZipCooldownSeconds. */
	virtual float GetActivatedCooldownSeconds() const override;

	/**
	 * Jump while flying is CLIMB, not a jump. Consumed (returns true) so the ordinary jump — and the
	 * wall jump, and the slide-jump grace — never runs mid-Zip; §3's "jump goes up at walking speed"
	 * is a different verb on the same key.
	 */
	virtual bool OnJumpPressed() override;
	virtual void OnJumpReleased() override;

	// --- readouts, for the HUD, the bots and the harness ------------------------------------------

	/** True while she is flying. Correct on every machine — it reads the replicated flag. */
	bool IsZipping() const;

	/** Seconds of flight left, 0 when not flying. */
	float GetZipRemaining() const;

	/** What a Zip started RIGHT NOW would last: the full duration, halved if she is carrying. */
	float GetZipDurationForNow() const;

#if !UE_BUILD_SHIPPING
	/**
	 * DEV ONLY. Forces the remaining flight time, so a harness can stage "4 s in with 1 s left"
	 * without spending four seconds of real time on it.
	 *
	 * Exists for ONE caller: Trace.Lily.ZipVerify. The mid-Zip pickup clause is a claim about what
	 * happens to a PARTIALLY SPENT Zip, and the natural wrong implementation ("clamp to 2.5 s") is
	 * indistinguishable from the right one until the remaining time is below 2.5 s. So the harness has
	 * to be able to get there, and waiting is not a test, it is a delay.
	 */
	void DebugSetZipRemaining(float Seconds);

	/** DEV ONLY, for Trace.Lily.ZipFxTest: how many of the four §2.1 pieces exist right now. */
	int32 DebugFxPrimitiveCount() const;

	/**
	 * DEV ONLY: one line naming every piece, its ACHIEVED blend, and its radius/height/height-on-body
	 * MEASURED BACK OFF THE LIVE COMPONENT. A verifier that re-derives the radius it expects from the
	 * same constants the code used is checking its own arithmetic; this reads the thing on screen.
	 */
	FString DebugFxReport() const;
#endif

private:
	/** Begins the flight. Duration is halved here if she is ALREADY carrying (§3's first clause). */
	void StartZip();

	/** Ends it, for any reason. Never touches the E cooldown — the framework owns that. */
	void StopZip(const TCHAR* Why);

	/** One tick of flight: the guards, then the single Velocity.Z write. */
	void ApplyZip(float DeltaSeconds);

	/** §3's SECOND clause: the Core has just arrived mid-flight, so halve WHAT IS LEFT. */
	void HalveRemainingForCorePickup();

	/**
	 * DEMO 19 ITEM 4 — THE FIX, AND THE ONE SENTENCE WORTH READING IN THIS FILE.
	 *
	 * The user: "Pressing space once makes her continuously move up even after I let go, and pressing
	 * control doesn't make her come down."
	 *
	 * ONE CAUSE, BOTH HALVES. UTraceAbilityComponent::HandleJumpReleased() has NO CALLER anywhere in
	 * the project — ATracePlayerController::OnJumpCompleted() sends the release to
	 * ACharacter::StopJumping() and stops there, while OnJumpStarted() does forward the press. So
	 * OnJumpReleased() below was never once invoked in a real match, bJumpHeld latched true at the
	 * first press and stayed true for the rest of the flight, and ApplyZip's descend test
	 * (`!bJumpHeld && IsCrouchHeld()`) could therefore never be reached. Crouch was not "wired but
	 * ineffective": it was correct code standing behind a condition that could not go false.
	 *
	 * (For the record, since it is the fix everyone reaches for first and this codebase already
	 * documents it as a trap in Mace's and Slimeball's headers: GravityScale had nothing to do with
	 * it, and setting it to 0 would have left the climb-forever bug exactly where it was.)
	 *
	 * So the climb intent stops being a latch and becomes a LEVEL, sampled here from the key itself on
	 * whichever machine has a keyboard, and published into
	 * UTraceCharacterMovementComponent::SetJumpHeld() — which rides FLAG_Custom_1 to the server, the
	 * same saved-move road IsCrouchHeld() already travels. That is what makes the release arrive on
	 * the authority as well as on the owning client without a release RPC that does not exist.
	 *
	 * THE POLL IS 20 Hz, so a release lands within 50 ms — about 20 uu of overshoot at the halved
	 * climb speed. The press does NOT wait for it: OnJumpPressed() sets the level immediately, so the
	 * key still feels instant.
	 */
	void SampleClimbIntent();

	/** "Is she climbing right now?" — the level above, or the old latch under the red arm. */
	bool IsClimbHeld() const;

	/** Server: push bZipping / ZipEndMatchTime into the replicated scratch pad. */
	void PublishState();

	/** Server or owning client. A simulated proxy's velocity is replicated and must not be written. */
	bool ShouldDriveMovement() const;

	/** True while she is flying, from THIS machine's own state rather than the replicated mirror. */
	bool bZipping = false;

	/** Absolute match-clock time the flight ends. Never a countdown — see the cooldown contract. */
	float ZipEndMatchTime = 0.f;

	/**
	 * THE OLD LATCH. Kept for exactly one purpose: it is the RED ARM of Trace.Lily.KeyTest.
	 *
	 * The comment that used to sit here said "press and release both arrive; a lost release must not
	 * fly her forever" — and every word of it was wrong, which is why it is quoted rather than
	 * deleted. The release never arrived (see SampleClimbIntent), so this latched at the first press
	 * and flew her for the whole flight. Trace.Lily.ZipHoldRelease 0 puts it back in charge so the
	 * user's complaint can be reproduced on the same harness that shows it fixed.
	 */
	bool bJumpHeld = false;

	/** Last carrier reading, so a PICKUP is an edge and not a level. Halving on a level would halve every tick. */
	bool bWasCarrier = false;

	/** Mid-Zip pickups this flight has seen. Instrumentation the harness reads; not replicated. */
	int32 ZipCorePickups = 0;

	// =============================================================================================
	// FX_AUDIO_PLAN §2.1 — THE FLIGHT PRESENTATION. Four primitives, and four is the whole budget.
	// =============================================================================================
	//
	// §1.4's ceiling is FOUR attached primitives per pawn, additive only, intensity <= 0.5, inside
	// 96 uu of the capsule axis. Lily spends all four, and the peak is momentary:
	//
	//     AuraRingA   loop, r 46 uu       |  present for the whole flight
	//     AuraRingB   loop, r 46 uu       |  (the two halves of the anti-gravity wash)
	//     ClimbJet    cone, base r 22 uu  |  present for the whole flight, VISIBLE only climbing
	//     CastRing    r 40 -> 150 uu      |  0.4 s at the cast, then destroyed
	//
	// The cast ring is the only one that can push the count to four, it does so for 0.4 s of a 5 s
	// flight, and it is destroyed rather than hidden so a Lily who is not casting is at three.
	// 150 uu is outside the 96 uu capsule footprint — deliberately, and it is not a §1.4 breach: the
	// 96 uu rule governs LOOPING state FX ("attached while the flag is up"), and the flash is a
	// transient beat, the same class as an ATraceFxBurst ring and half the size of Elle's.
	//
	// WHY THEY ARE COMPONENTS ON THE PAWN AND NOT AN ACTOR. Every one of these has to follow her,
	// and a follower actor is a second transform to drive at 20 Hz against a pawn that moves at 60+.
	// The pawn owning them also gives the death rule for free: a destroyed pawn destroys its
	// components, so a corpse cannot inherit an aura even if this file forgets — and it does not
	// forget, see OnPawnDied.

	/**
	 * The two drifting flight rings, and the cast flash. Null when she is not flying.
	 *
	 * *** RINGS OF BEADS, NOT CYLINDERS, AND THE FIRST CAPTURE IS WHY. ***
	 * §2.1 asks for a "cylinder SHELL" and /Engine/BasicShapes/Cylinder is SOLID, so the first pass
	 * drew all three as filled discs — frames-W4-KITS-A/TraceAutoShot_lily_aura_20260825_030115.png
	 * is a Lily standing on two white dinner plates, one of them cutting through her hips. The engine
	 * ships no torus and this project cannot author meshes, so a ring is made the way W3-FXBURST
	 * makes its rings: ONE UInstancedStaticMeshComponent carrying beads around a circle. That is one
	 * primitive against §1.4's budget no matter how many beads it holds.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> AuraRingA = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> AuraRingB = nullptr;

	/** The climb jet: an inverted cone under the capsule, shown only while she is actually rising.
	  * A cone is a SOLID shape in §2.1's own terms ("inverted cone under the capsule"), so this one
	  * stays a plain mesh — the bead treatment above is specifically about the word "shell". */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> ClimbJet = nullptr;

	/** The cast flash. Built on the rising edge, destroyed 0.4 s later. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> CastRing = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> AuraMIDA = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> AuraMIDB = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ClimbJetMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CastRingMID = nullptr;

	/**
	 * The §1.6.4 loop. ONE PER MACHINE, started by the router off the replicated edge, which is what
	 * makes the hum "the enemy-audible one the audit asks for" without a per-loop RPC.
	 *
	 * The caller owns it (see TraceAudio.h): a component that is never FadeOut'd leaks until its
	 * actor dies, so every exit from flight goes through DetachZipFx.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> ZipLoopSound = nullptr;

	/**
	 * The pawn the four components above are attached to.
	 *
	 * WEAK, AND CHECKED EVERY TICK. The ability set lives on the PlayerState and outlives the pawn
	 * (that is the whole cooldown contract), so "my components" and "my pawn" are two lifetimes and
	 * a respawn or a character swap can move one without the other. A stale pointer here would mean
	 * an aura drifting on a pawn this player no longer owns.
	 */
	TWeakObjectPtr<ATraceCharacter> FxPawn;

	/** The ACHIEVED blends (never the requested ones — SetGlow takes two different routes). */
	ETraceFxBlend AuraBlendA = ETraceFxBlend::None;
	ETraceFxBlend AuraBlendB = ETraceFxBlend::None;
	ETraceFxBlend ClimbJetBlend = ETraceFxBlend::None;
	ETraceFxBlend CastRingBlend = ETraceFxBlend::None;

	/** 0..1 around the wash. Advanced by real time, so the two rings never drift apart. */
	float AuraPhase = 0.f;

	/** Match time the cast flash began, or 0 when none is running. */
	float CastRingStartTime = 0.f;

	/**
	 * Match time the END DISSOLVE began, or 0 while she is flying.
	 *
	 * §2.1: "aura rings fade I->0 over 0.3 s (never pop-out, bible §6.4)". So the falling edge does
	 * not destroy anything — it starts a fade, and the tick destroys the pieces when it finishes.
	 * DEATH IS THE ONE EXCEPTION and takes the immediate path: a corpse must not be wearing a
	 * fading aura for a third of a second (§1.2 obligation 2).
	 */
	float DissolveStartTime = 0.f;

	/** True while the presented state says she is flying. The router's own copy of the flag. */
	bool bFxFlying = false;

	// --- the FX call surface ----------------------------------------------------------------------

	/** True when the REPLICATED flag says Zip is up. The FX read this, never bZipping. */
	bool IsZipPresented() const;

	/** Idempotent. Builds the three loop pieces and starts the loop sound. Safe to call twice. */
	void AttachZipFx();

	/** @param bImmediate  true for death/swap (destroy now), false for the §2.1 0.3 s dissolve. */
	void DetachZipFx(bool bImmediate);

	/** The 0.4 s ground ring. Built fresh each cast; a second cast cannot stack two. */
	void SpawnCastFlash();

	/** One frame of the wash, the jet's visibility test, the dissolve and the flash. Every machine. */
	void TickZipFx(float DeltaSeconds);

	/** Re-parents everything when the pawn changed under us. Returns the pawn to build on, or null. */
	ATraceCharacter* ResolveFxPawn();

	/** Creates one configured, registered, additive FX primitive on @p Pawn, or null. */
	UStaticMeshComponent* MakeFxPiece(ATraceCharacter* Pawn, const TCHAR* NameHint, UStaticMesh* Mesh,
		TObjectPtr<UMaterialInstanceDynamic>& OutMID, ETraceFxBlend& OutBlend);

	/** The same, as an instanced bead ring: one component, BeadsPerRing instances, no transforms yet. */
	UInstancedStaticMeshComponent* MakeFxRing(ATraceCharacter* Pawn, const TCHAR* NameHint,
		TObjectPtr<UMaterialInstanceDynamic>& OutMID, ETraceFxBlend& OutBlend);

	/**
	 * Lays @p Ring's beads on a circle of @p RadiusUU at height @p LocalZ on the pawn.
	 *
	 * The bead RADIUS is 13% of the ring's, floored at the bible's 8 uu-across minimum — the same
	 * proportion W3-FXBURST measured for its rings, and it is not arbitrary: 24 beads around a circle
	 * are spaced 2*PI*R/24 = 0.262*R apart and a 13% bead is 0.26*R across, so the beads just touch
	 * at every radius. That is what makes one expansion look like a ring growing rather than like a
	 * necklace coming apart.
	 */
	void LayOutRing(UInstancedStaticMeshComponent* Ring, float RadiusUU, float LocalZ,
		float FadeAlpha = 1.f) const;
};
