// Trace — X (spec v14 §6), the fifth character.
//
// ===================================================================================================
// WHAT SPEC §6 SAYS, VERBATIM, AND WHERE EACH CLAUSE IS IMPLEMENTED
// ===================================================================================================
//
//   "PASSIVE: orbited by FIVE mechanical bees. An enemy hit by a bee becomes VULNERABLE for 2s,
//    taking +25% damage FROM ALL SOURCES. Does not stack; a new application RESETS the timer.
//    [ASSUMPTION] the orbiting bees hit on contact — X's body is the delivery mechanism."
//
//        the swarm             ATraceBeeSwarm, below. Cosmetic, local to each machine.
//        the contact test      UTraceAbilitySetX::SweepBeeContacts, server only, in TickAbilities.
//        the mark              UTraceHealthComponent::ApplyVulnerable.
//        +N% from ALL sources  UTraceHealthComponent::ApplyDamage — ONE multiplication, in the one
//                              function every damage source in the project already funnels through.
//                              *** SPEC v24 §9 RAISED THE FIRST STACK FROM +25% TO +35%. *** Nothing
//                              in this file moved for it either: the number is
//                              UTraceSettings::XVulnerableDamageBonus and the multiplier is derived
//                              as 1 + that, so the mark means "+35% of whatever the hit was worth"
//                              without a second copy of the arithmetic anywhere.
//                              *** Read the block comment at the top of TraceHealthComponent.h. ***
//        "does not stack"      *** SUPERSEDED BY SPEC v16 §4. *** It stacks now: +35% for the first
//                              hit and +5% for each after, all of them expiring together on the ONE
//                              timer. Nothing in THIS file changed for it — the mark is still one
//                              ApplyVulnerable call and the count lives on the victim's health
//                              component, which is what made the change a two-line one there and a
//                              no-op here. The "a new application RESETS the timer" half still holds.
//
//   *** SPEC v16 §1 RESHAPED STING: it is a CLIP now, not a tag on the next five shots. ***
//   "X's Sting ability reloads the clip with just the 5 bee bullets; after shooting those, his next
//    reload is normal."
//        ActivateAbility()      calls UTraceWeaponComponent::LoadAbilityClip(5), which REPLACES the
//                               clip. The count of bee rounds left is the CLIP's from then on, and
//                               FTraceAbilityNetState::Stacks is a downward-only mirror of it
//                               (SyncStingToClip) kept for the swarm and the HUD.
//        the consequence        a bee round that MISSES is spent. Under v14 §6 only a landed mark
//                               cost a bee; a clip cannot work that way.
//
//   "MOVEMENT: +10% speed while ANY enemy is vulnerable."
//
//        GetMoveSpeedMultiplier(). ANY enemy, not the one he marked — so a teammate's kill pressure
//        on a marked target does not switch X's boost off, and a second X is not required for it to
//        be worth marking two people. LIVE: UTraceCharacterMovementComponent::GetMaxSpeed()
//        (TraceCharacterMovementComponent.cpp, ~:4190) multiplies the ground speed by
//        UTraceAbilityComponent::GetMoveSpeedMultiplierFor. Ground only, and deliberately — the air
//        ceilings are the momentum model, not GetMaxSpeed. (This note used to say the movement
//        component "does not call it yet".)
//
//   "ACTIVATED — STING: loads the 5 bees into his gun and they stop orbiting. His NEXT FIVE BULLETS
//    apply vulnerable on hit, at NORMAL damage. When all five are fired the bees resume orbiting.
//    25s cooldown."
//
//        ActivateAbility()      loads FTraceAbilityNetState::Stacks with XStingBulletCount.
//        the bullets            TraceAbilityWeaponHooks::OnBulletHit -> NotifyBulletHit().
//        "at NORMAL damage"     the mark is applied AFTER the bullet's damage has landed, so the
//                               delivering bullet is never itself amplified.
//        "they stop orbiting"   Stacks > 0 hides the swarm AND disables the contact sweep, so a
//                               loaded X cannot also be stinging with his body.
//        25 s                   UTraceSettings::XStingCooldownSeconds — the only 25 in §6.
//
// ===================================================================================================
// SPEC §4 — HOW THE CARRIER IS PROTECTED FROM ALL OF THIS, IN THREE INDEPENDENT PLACES
// ===================================================================================================
//
// X is named in §4 as one of the fifteen new ways to break the founding invariant: "X's vulnerable
// (moot if nothing can damage them, but must not become a damage path)". The mark is a CONTROL
// effect, never a damage effect, and it is stopped three times over:
//
//   1. THE CHOKE POINT. Every mark this file applies goes through
//      UTraceCharacterAbilitySet::CanAffect(Target, ETraceAbilityEffect::Control), which is
//      UTraceAbilityComponent::CanAffectTargetDetailed — the single §4 function. It refuses a
//      carrier. There is no second path in this file: SweepBeeContacts and NotifyBulletHit both go
//      through MarkVulnerable() and MarkVulnerable() is the only caller of ApplyVulnerable().
//   2. THE MARK ITSELF. UTraceHealthComponent::ApplyVulnerable refuses a Core carrier on its own,
//      with no reference to the choke point, so a future caller that forgets rule 1 still fails.
//   3. THE MULTIPLIER. UTraceHealthComponent::GetVulnerableDamageMultiplier returns 1 for a Core
//      holder, and is evaluated strictly AFTER ApplyDamage's carrier early-out — so even a carrier
//      who was somehow marked contributes no amplified damage, and their zero is produced by the
//      carrier rule rather than by an amplifier multiplying a zero.
//
// Trace.X.CarrierTest red-arms all three.
//
// ===================================================================================================
// WHAT THIS FILE DELIBERATELY DOES NOT DO
// ===================================================================================================
//
// It does NOT override UTraceCharacterAbilitySet::GetIncomingDamageMultiplier(). That hook looks like
// the natural home for "vulnerable's +N% incoming" and it is the wrong one, and THE DECIDING REASON
// IS THE DOUBLE AMPLIFICATION: the ability damage path
// (UTraceAbilityComponent::ModifyDamageThroughPassives) runs that hook and THEN calls
// UTraceHealthComponent::ApplyDamage, so implementing it in both places would amplify ability damage
// TWICE — the square of the mark's multiplier instead of the multiplier (at spec v24 §9's +35% that
// is 1.8225x where 1.35x is meant; it was 1.5625x against 1.25x before §9, and the shape of the bug
// is the same whatever the knob says). Leaving the hook at its 1.0 default is what makes the health
// component the single site, and it is load-bearing.
//
// The secondary reason still stands but no longer reads the way it used to. The hook is consulted
// only when the TARGET has an ability set, and this comment used to justify that with spec v14 §3's
// "Bots remain characterless" — a rule spec v15 §2 reversed, so "a bee that stung a bot would mark
// nothing" is simply false now. What is still true is that a stung player may hold no character at
// all (mode A, the characters toggle, or a full team roster that could not serve them), and a mark
// that only worked on characters would be a mark that quietly stops working in mode A.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "Abilities/TraceAbilityTypes.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Gameplay/TraceFxShapes.h"       // ETraceFxBlend — stored per piece, so it must be complete

#include "TraceAbilitySetX.generated.h"

class ATraceCharacter;
class UInstancedStaticMeshComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;

/**
 * The bee orbit, as pure arithmetic.
 *
 * ONE function, called by both the hit test (server) and the visual (every machine), because a swarm
 * that is drawn somewhere other than where it stings is the exact class of defect spec §1 spent this
 * whole pass fixing on the ribbon: a lethal volume that is not the drawn volume. Everything it needs
 * is an argument, so the two callers cannot drift.
 *
 * @param Centre     X's chest, in world space.
 * @param TimeSeconds  the MATCH clock (GetServerWorldTimeSeconds), which every machine agrees on.
 */
namespace TraceXBees
{
	TRACE_API FVector GetBeeLocation(const FVector& Centre, float TimeSeconds, int32 Index, int32 Count,
	                                 float RadiusUU, float SpeedDegreesPerSecond);

	/** Resolved from UTraceSettings, clamped. The .ini wins; read these, never the header defaults. */
	TRACE_API int32 GetBeeCount();
	TRACE_API float GetOrbitRadiusUU();
	TRACE_API float GetOrbitSpeedDegreesPerSecond();
	TRACE_API float GetBeeHitRadiusUU();
	TRACE_API int32 GetStingBulletCount();
	TRACE_API float GetSpeedBonusFraction();

	/** Where the swarm is centred on a pawn — chest height, not the actor origin at the feet. */
	TRACE_API FVector GetSwarmCentre(const ATraceCharacter* Character);
}

/**
 * X's three abilities.
 *
 * FTraceAbilityNetState, as X interprets it:
 *   Stacks                bees currently loaded in the gun. 0 = orbiting (the resting state).
 *   Flags & AuxActive     Sting is active. Redundant with Stacks > 0 and kept because it is what the
 *                         HUD and the swarm read, and because a future "loaded but empty" state
 *                         would need them to differ.
 *   EffectEndMatchTime    unused. X's only timer is the per-VICTIM mark, which lives on the victim's
 *                         health component precisely because the victim may hold no character at all
 *                         (mode A, the characters toggle, an unservable roster) and so may have no
 *                         ability set to store it on. See the double-amplification note at the top.
 */
UCLASS()
class TRACE_API UTraceAbilitySetX : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::X; }

	// ---- lifecycle -----------------------------------------------------------------------------
	virtual void OnEquipped() override;
	virtual void OnUnequipped() override;
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// ---- FX_AUDIO_PLAN §1.2's client FX router ---------------------------------------------------
	//
	// The ONE thing X needs from the router is the STING-LOAD CONVERGE: §2.7 replaces "the swarm
	// hides instantly" with "the bees fly to the gun over 0.3 s and then hide", and 0.3 s of
	// animation has to start on the EDGE. The 20 Hz poll in TickAbilities is the belt (it starts the
	// same converge if the edge was missed, and it is idempotent), but an animation started from a
	// poll is an animation that begins up to 50 ms late on a state change the player caused.
	//
	// SyncClientFx SNAPS instead of animating, and that is the whole difference between the two
	// hooks: a machine seeing an already-loaded X for the first time must not play the flight of
	// bees that happened before it was watching.
	virtual void OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New) override;
	virtual void SyncClientFx(const FTraceAbilityNetState& Current) override;

	// ---- the activated ability: STING ----------------------------------------------------------
	virtual bool  CanActivate(FText& OutReason) const override;
	virtual bool  ActivateAbility() override;
	virtual float GetActivatedCooldownSeconds() const override;

	// ---- the movement passive ------------------------------------------------------------------
	virtual float GetMoveSpeedMultiplier() const override;

	// NOTE: GetIncomingDamageMultiplier() is deliberately NOT overridden. See the file header.

	// ---- queries, for the HUD, the harness and the swarm ----------------------------------------

	/**
	 * Bee rounds in the gun right now. 0 when the bees are orbiting. Correct on clients.
	 *
	 * SPEC v16 §1 MADE THIS A MIRROR. The authoritative count is now the CLIP's
	 * UTraceWeaponComponent::GetAbilityRoundsInClip(), because rounds leave the gun whether or not
	 * they hit anybody; FTraceAbilityNetState::Stacks follows it (SyncStingToClip) and is what
	 * clients and the swarm read, since the weapon's ammo is replicated to its owner only.
	 */
	int32 GetLoadedBees() const;

	/** True while Sting is up: the bees are in the gun and are not orbiting or stinging. */
	bool IsStingLoaded() const;

	/** True when at least one living enemy of X is currently marked. Drives the +10%. */
	bool IsAnyEnemyVulnerable() const;

	/**
	 * SERVER ONLY. A bullet from X landed on @p Victim.
	 *
	 * Marks the victim when the round that just left the gun was one of the bee rounds. It no longer
	 * decides how many bees are LEFT — spec v16 §1 moved that to the clip, and a round is spent when
	 * it is fired rather than when it connects. The v14 §6 [ASSUMPTION] that a wasted Sting bullet
	 * cost X nothing is therefore superseded: a bee round that misses, or that finds a Core carrier,
	 * is gone. See the implementation for the exact two-term test that decides "was this a bee round"
	 * and for why the old per-hit decrement is still there.
	 */
	void NotifyBulletHit(ATraceCharacter* Victim, bool bHeadshot);

	/**
	 * SERVER ONLY. THE ONLY PLACE THIS FILE MARKS ANYBODY.
	 *
	 * Asks the §4 choke point with ETraceAbilityEffect::Control, then asks the victim's health
	 * component. Returns true only if the mark landed.
	 */
	bool MarkVulnerable(ATraceCharacter* Target) const;

private:
	/**
	 * SERVER ONLY. Pulls FTraceAbilityNetState::Stacks down to the gun's ability-round count.
	 *
	 * ONE-WAY, ALWAYS DOWNWARD, AND THAT IS THE WHOLE CONTRACT (spec v16 §1). The clip is the source
	 * of truth for how many bee rounds are left; Stacks is the replicated display of it, because the
	 * clip itself is replicated to its owner only and the swarm has to be drawn on every machine.
	 * Only ActivateAbility ever raises Stacks. A second writer that could raise it would be the
	 * four-writer bug this codebase already carries once.
	 */
	void SyncStingToClip();

	/** SERVER ONLY. The passive: every bee, against every living enemy, once per tick. */
	void SweepBeeContacts();

	/** Spawns / destroys the cosmetic swarm to match the current state. Never authoritative. */
	void UpdateSwarmActor();

	/** The local, non-replicated swarm. One per machine that draws anything. */
	TWeakObjectPtr<class ATraceBeeSwarm> Swarm;

	/**
	 * GetMoveSpeedMultiplier() is called from the movement tick on every machine, so it caches its
	 * answer for the frame. Mutable because the hook is const and must stay const — it is a pure
	 * query as far as every caller is concerned.
	 */
	mutable uint64 CachedSpeedFrame = 0;
	mutable float  CachedSpeedMultiplier = 1.f;
};

/**
 * X's five mechanical bees — COSMETIC ONLY, LOCAL TO EACH MACHINE, NEVER REPLICATED.
 *
 * Every machine spawns its own from replicated facts (X's CharacterId, his pawn's transform and
 * FTraceAbilityNetState::Stacks) and places the bees with TraceXBees::GetBeeLocation on the MATCH
 * clock, which every machine agrees on. So the bees are in the same place on the server and on a
 * lagged client without a single byte of per-bee replication, and — the part that matters — the bees
 * a player can SEE are the bees that sting, because the visual and the server's contact test call the
 * same function with the same arguments.
 *
 * No collision anywhere on it. It orbits at head height through the space bullets travel and must
 * never intercept one.
 */
UCLASS(NotPlaceable, Transient)
class TRACE_API ATraceBeeSwarm : public AActor
{
	GENERATED_BODY()

public:
	ATraceBeeSwarm();

	virtual void Tick(float DeltaSeconds) override;

	/** The pawn being orbited. Gone -> the swarm retires itself. */
	TWeakObjectPtr<ATraceCharacter> Host;

	/**
	 * "They stop orbiting" — and, since FX_AUDIO_PLAN §2.7, they FLY THERE first.
	 *
	 * @param bLoaded   true when Sting has the bees in the gun.
	 * @param bAnimate  true to play §2.7's 0.3 s converge into the muzzle before hiding; false to
	 *                  snap straight to the answer. FALSE is for FIRST SIGHT (SyncClientFx): a
	 *                  machine that has just started watching an already-loaded X must not be shown
	 *                  a flight of bees that happened before it was looking.
	 *
	 * IDEMPOTENT IN BOTH DIRECTIONS. It is called from the §1.2 router edge AND from the 20 Hz poll
	 * in UTraceAbilitySetX::TickAbilities, so it has to be safe to call with the same answer forty
	 * times a second — a converge already in flight is left alone rather than restarted, which is
	 * what would otherwise freeze the bees at their starting radius for as long as the poll ran.
	 */
	void SetSwarmLoaded(bool bLoaded, bool bAnimate);

	/** True while the bees are flying into the gun. For the harness and for the poll's idempotence. */
	bool IsConverging() const { return Phase == EPhase::Converging; }

	/** True once they have arrived and the swarm is drawn nowhere. */
	bool IsLoadedAndHidden() const { return Phase == EPhase::Loaded; }

	/**
	 * ONE LINE OF MEASUREMENTS TAKEN OFF THE LIVE COMPONENTS, for Trace.X.BeeFxTest.
	 *
	 * *** READ BACK, NEVER RE-DERIVED. *** Every number in it comes out of the components that are on
	 * screen — the achieved blends, the instance counts, the halo's scale relative to the core's, the
	 * trail radius and segment length recovered through UTraceFxShapes' inverse scale helpers. A
	 * verifier that recomputed the numbers from TraceXBeeFx would only be checking its own
	 * arithmetic; this checks the thing a player is looking at.
	 */
	FString DebugDescribe() const;

	/** Bee @p BeeIndex's world position as the CORE instance actually holds it. False if absent. */
	bool DebugGetBeeWorldLocation(int32 BeeIndex, FVector& OutLocation) const;

private:
	/**
	 * WHERE THE SWARM IS IN §2.7'S THREE-STATE LIFE. It used to be one bool.
	 *
	 *   Orbiting     the resting state: five bees on the orbit, stinging on contact.
	 *   Converging   Sting was activated; the bees are lerping into the muzzle over ConvergeSeconds.
	 *   Loaded       they are in the gun and drawn nowhere.
	 */
	enum class EPhase : uint8
	{
		Orbiting,
		Converging,
		Loaded
	};

	/**
	 * Builds the pieces once, and re-counts instances when XBeeCount is retuned. FIVE pieces with
	 * TraceXBeeFx::bBeePolish on; ONE — the bee cores — with it off, which is the shipped state since
	 * Demo 29 item 5.
	 */
	void EnsureBeeInstances(int32 DesiredCount);

	/** Places every instance of every piece that EXISTS, for one frame. @p Alpha 0..1 is the converge. */
	void PlaceBees(const FVector& Centre, float MatchTimeSeconds, int32 BeeCount, float ConvergeAlpha);

	/** Shows or hides every piece that exists, at once. */
	void SetPiecesVisible(bool bVisible);

	// =============================================================================================
	// THE FIVE PIECES — FX_AUDIO_PLAN §2.7's "bee polish", AND WHY THERE ARE FIVE OF THEM
	// =============================================================================================
	//
	// §2.7 asks, PER BEE, for a halo sleeve at x1.8 scale and a trailing cylinder whose intensity
	// fades 0.4 -> 0 along its length. Written literally that is five bees x (core + halo + trail) =
	// fifteen components on one actor, and a fading trail needs more than one anyway because a
	// material instance has ONE intensity.
	//
	// So every repeated element is ONE UInstancedStaticMeshComponent carrying N instances — the
	// project's own bead-ring precedent (ATraceElleGate's 60, ATraceFxBurst's whole scatter slot) —
	// and the trail's fade is three stacked segment components at three intensities rather than a
	// gradient no material here can express.
	//
	// THE NET COMPONENT COUNT WAS UNCHANGED: it replaced five per-bee UStaticMeshComponents with
	// five instanced pieces, and bought the halo and the trail for nothing.
	//
	// *** DEMO 29 ITEM 5 TURNED THE HALO AND THE TRAIL OFF AGAIN. *** TraceXBeeFx::bBeePolish is
	// false, so only the CORES are built and the swarm is one component drawing N spheres — fewer
	// than the five per-bee components the old model used, and the same picture. Everything below
	// still describes what is built when the gate is on.
	//
	// The swarm is NOT counted against TraceFxLoopBudget's four-per-pawn ceiling, and that is not an
	// oversight: §1.4's budget counts primitives ATTACHED TO A PAWN, and this is a separate,
	// unattached, local-only actor whose bees orbit 120 uu out — outside the capsule footprint the
	// budget is defined in terms of. It predates the budget and is X's passive rather than a
	// while-active loop FX.

	/** The bees themselves: N spheres 16 uu across. This piece is drawn whatever the polish gate says. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Cores;

	/**
	 * §2.7's "additive halo sleeve x1.8 scale BeeRounds amber I 0.3". N instances, one per bee.
	 *
	 * *** NULL WHILE TraceXBeeFx::bBeePolish IS FALSE — DEMO 29 ITEM 5. *** The owner asked for the
	 * old bee model back, and the halo is one of the two things the overhaul added on top of it.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Halos;

	/**
	 * §2.7's "one trailing cylinder (l 60 uu, r 3 uu) along the orbit tangent, I fading 0.4 -> 0".
	 *
	 * Three components because the fade is real: each holds N instances covering one third of the
	 * trail's length, at its own intensity. One component could hold all 3N instances but would have
	 * one MID and therefore one brightness, which is a streak rather than a trail.
	 *
	 * *** ALL THREE ARE NULL WHILE TraceXBeeFx::bBeePolish IS FALSE — DEMO 29 ITEM 5. *** The trail is
	 * the other thing the overhaul added on top of the old bee model.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> TrailParts[3];

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CoreMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> HaloMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> TrailMIDs[3];

	/** The blends the pieces ACHIEVED. SetGlow must be given these, never the preferred values. */
	ETraceFxBlend CoreBlend = ETraceFxBlend::None;
	ETraceFxBlend HaloBlend = ETraceFxBlend::None;
	ETraceFxBlend TrailBlend = ETraceFxBlend::None;

	/**
	 * A UPROPERTY, and found in the CONSTRUCTOR, for two separate reasons: a constructor-time
	 * FObjectFinder is what makes an engine asset cook into a packaged build (a runtime LoadObject
	 * resolves to null once cooked — the policy TraceCore states), and holding it in a tracked
	 * property is what stops the garbage collector from taking it out from under a function-local
	 * static.
	 *
	 * The POLISH materials do not come from here: the halo's and the trail's blends are resolved
	 * through UTraceFxShapes, whose own CDO holds the cook references and which degrades down a
	 * defined ladder instead of silently producing an opaque piece where an additive one was wanted.
	 * The OLD bee material does come from here again — see LegacyBeeMaterial.
	 */
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMesh> BeeMesh;

	/** The cylinder the trail segments are made of. Same constructor-time policy as BeeMesh. */
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMesh> TrailMesh;

	/**
	 * THE OLD BEE MATERIAL, BACK FOR DEMO 29 ITEM 5 — M_TraceNeon with an engine BasicShapeMaterial
	 * fallback, exactly the pair the swarm carried before the overhaul, and found in the constructor
	 * for exactly the reason BeeMesh is. Only read while TraceXBeeFx::bBeePolish is false; with the
	 * gate on, the cores go back through UTraceFxShapes like the other pieces.
	 */
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> LegacyBeeMaterial;

	EPhase Phase = EPhase::Orbiting;

	/**
	 * LOCAL world time the converge started. Local and not the match clock, deliberately: this is a
	 * 0.3 s presentation flourish on a purely cosmetic actor, it starts when THIS machine learns the
	 * bees were loaded, and nothing anywhere compares one machine's converge against another's. The
	 * ORBIT stays on the match clock, because the orbit is where the bees sting from.
	 */
	float ConvergeStartWorldTime = 0.f;
};
