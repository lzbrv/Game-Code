// Trace — the per-character extension point (spec v14 §5 / §6).
//
// ===================================================================================================
// FOR THE FIVE CHARACTER AGENTS. READ THIS FIRST.
// ===================================================================================================
//
// You add exactly ONE new file pair to Source/Trace/Abilities/ and you edit NOTHING ELSE:
//
//     Abilities/TraceAbilitySetRocco.h / .cpp        (and Chut / Mace / Oyster / X)
//
//     UCLASS()
//     class UTraceAbilitySetRocco : public UTraceCharacterAbilitySet
//     {
//         GENERATED_BODY()
//     public:
//         virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Rocco; }
//         // ... override the hooks you need, below.
//     };
//
// THERE IS NO REGISTRATION STEP AND NO SHARED TABLE TO EDIT. The framework finds your class by
// reflection: it walks every UClass derived from UTraceCharacterAbilitySet, asks each CDO for
// GetCharacterId(), and builds the map on first use. That is deliberate — a shared registry file is
// five simultaneous edits to one file and therefore five merge conflicts.
//
// EXACTLY ONE class may claim a given ETraceCharacterId. Two classes claiming Rocco is a hard error,
// logged at Error, and the first one found wins; that is a build mistake, not a runtime condition.
//
// ===================================================================================================
// THE THREE RULES YOU CANNOT BREAK
// ===================================================================================================
//
// 1. EVERY DAMAGE, SLOW, PULL OR KNOCKBACK GOES THROUGH CanAffect(). Not through your own team
//    check, not through a copy of the carrier test, not through "I know this one is safe".
//    UTraceAbilityComponent::CanAffectTarget is the single choke point spec §4 demands, and
//    UTraceCharacterAbilitySet::CanAffect() below is your one-line way to reach it. Spec §4 names
//    your abilities specifically as the fifteen new ways to break the game's founding invariant.
//
// 2. COOLDOWNS ARE ABSOLUTE MATCH-CLOCK TIMES, NEVER DURATIONS AND NEVER PER-LIFE TIMERS.
//    Spec §5 verbatim: "cooldowns should continue to countdown while a player is dead. They should
//    not automatically reset due to death or a goal (a player can spawn with an ability timer still
//    counting down). They should all reset at halftime." The framework already does all of this for
//    the activated (E) ability — you never touch the cooldown yourself. If YOUR character needs a
//    second timer, store an absolute time from MatchTimeNow() in FTraceAbilityNetState, and clear it
//    in OnHalfTime() and nowhere else.
//
// 3. AUTHORITY WRITES, CLIENTS PREDICT. Every hook below tells you which machines it runs on. Never
//    write FTraceAbilityNetState off the server; call MarkStateDirty() after you write it on the
//    server so a listen server's own HUD updates in the same frame a remote client's does.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceAbilityTypes.h"

#include "TraceCharacterAbilitySet.generated.h"

class ATraceCharacter;
class UTraceAbilityComponent;
class UTraceCharacterMovementComponent;

/**
 * One character's three abilities: movement, passive, activated.
 *
 * Lives as a plain UObject owned by the player's UTraceAbilityComponent (which lives on the
 * PlayerState, so it survives death). Instantiated on EVERY machine that has the component — server,
 * owning client and simulated proxies alike — so client-side prediction and cosmetic work have a
 * home. It holds NO replicated properties of its own; the replicated per-character state is
 * FTraceAbilityNetState on the component, reachable through State() / MutableState().
 */
UCLASS(Abstract)
class TRACE_API UTraceCharacterAbilitySet : public UObject
{
	GENERATED_BODY()

public:
	// =============================================================================================
	// IDENTITY — the one thing every subclass must override
	// =============================================================================================

	virtual ETraceCharacterId GetCharacterId() const
		PURE_VIRTUAL(UTraceCharacterAbilitySet::GetCharacterId, return ETraceCharacterId::None;);

	// =============================================================================================
	// CONTEXT — everything you need to reach the world, on every machine
	// =============================================================================================

	/** The component that owns this set. Never null after Initialize(); null in the CDO. */
	UTraceAbilityComponent* GetAbilityComponent() const { return AbilityComponent; }

	/** The player's CURRENT pawn, or null while they are dead / between pawns. Always re-ask. */
	ATraceCharacter* GetCharacter() const;

	/** The pawn's Trace movement component, or null when there is no pawn. */
	UTraceCharacterMovementComponent* GetMovement() const;

	/** True on the server (including a listen server). Everything that changes state must check it. */
	bool HasAuthority() const;

	/** True on the machine whose player this is — where prediction and cosmetics belong. */
	bool IsLocallyControlled() const;

	/**
	 * THE MATCH CLOCK. AGameStateBase::GetServerWorldTimeSeconds(), correct and smoothed on clients.
	 * Every timer you store must be an absolute value of this, never a countdown.
	 */
	float MatchTimeNow() const;

	/** The replicated scratch pad. Read on any machine. */
	const FTraceAbilityNetState& State() const;

	/** SERVER ONLY. Write here, then call MarkStateDirty(). Returns a dummy on clients. */
	FTraceAbilityNetState& MutableState();

	/** SERVER ONLY. Pushes the state to clients and runs the OnRep locally for the listen server. */
	void MarkStateDirty();

	// =============================================================================================
	// *** THE CHOKE POINT. RULE 1. ***
	// =============================================================================================

	/**
	 * "May I do this to that player?" — the ONE question every ability asks before it damages,
	 * slows, pulls or knocks back anybody. Forwards to UTraceAbilityComponent::CanAffectTarget,
	 * which is where spec §4's rule actually lives.
	 *
	 * @param Target  the victim.
	 * @param Effect  Damage for any health loss; Control for a slow / pull / knockback / debuff;
	 *                Beneficial for something the target wants (Rocco's Ripple).
	 *
	 * Returns false for a Core carrier on Damage — always, unconditionally, no knob — and for a Core
	 * carrier on Control unless a designer has reversed the [ASSUMPTION]. Also false for the dead,
	 * for yourself, and for teammates while friendly fire is off.
	 */
	bool CanAffect(const ATraceCharacter* Target, ETraceAbilityEffect Effect = ETraceAbilityEffect::Damage) const;

	/**
	 * SERVER ONLY. Damage a target THROUGH the choke point, with one call.
	 *
	 * Prefer this to reaching for UTraceHealthComponent yourself: it applies CanAffect(Damage), then
	 * the target's own incoming-damage passives (Chud) and the vulnerable multiplier (X), and only
	 * then touches health. Returns the damage actually dealt, 0 when the choke point refused.
	 */
	float DealDamage(ATraceCharacter* Target, float Amount, FName Cause, bool bMelee = false, bool bHeadshot = false) const;

	// =============================================================================================
	// LIFECYCLE — all authority-and-clients unless stated
	// =============================================================================================

	/** Called once, immediately after construction, before any other hook. */
	virtual void OnInitialized() {}

	/** This character became the player's character. State has already been Reset(). */
	virtual void OnEquipped() {}

	/** This character is being replaced or cleared. Tear down actors you spawned. */
	virtual void OnUnequipped() {}

	/** A new pawn has been possessed (spawn, respawn, half-time restart). Cooldowns are UNTOUCHED. */
	virtual void OnPawnSpawned() {}

	/** The player's pawn died. Cooldowns keep running — spec §5. Do not reset anything but cosmetics. */
	virtual void OnPawnDied() {}

	/**
	 * HALF TIME. Spec §5: "They should all reset at halftime."
	 *
	 * The framework has ALREADY cleared the activated cooldown and Reset() the net state by the time
	 * this runs. Override it only to destroy world actors you own (jars, spikes, ripples, bees).
	 */
	virtual void OnHalfTime() {}

	/**
	 * Per-frame, on every machine that has the component. Called from the component's tick, which
	 * runs on the PlayerState and therefore keeps ticking while the player is DEAD — that is
	 * deliberate and is how a cooldown counts down through a death.
	 *
	 * Guard anything that needs a pawn on GetCharacter() != nullptr.
	 */
	virtual void TickAbilities(float DeltaSeconds) {}

	// =============================================================================================
	// THE ACTIVATED ABILITY — bound to E by default (spec §5), rebindable
	// =============================================================================================

	/**
	 * Cheap, side-effect-free "could I press E right now?", asked on the client for the HUD and
	 * again on the server before ActivateAbility(). The framework has already checked the cooldown,
	 * that the player is alive, that characters are enabled and that the match is live — override
	 * only for a character-specific condition (Mace's spike wants a second meaning while embedded).
	 *
	 * @param OutReason  short, player-facing, for the HUD. Leave untouched when returning true.
	 */
	virtual bool CanActivate(FText& OutReason) const { return true; }

	/**
	 * DO THE THING. Called on the server, and ALSO on the owning client for prediction.
	 *
	 * Return true if the ability actually fired. Returning true is what starts the cooldown, so
	 * return false for a fizzle (Mace's spike hitting nothing) if you do not want to charge for it.
	 *
	 * Use HasAuthority() to split the authoritative half (spawning the jar, dealing the damage) from
	 * the predicted half (the local dash, the sound, the rings). Both halves run on a listen server.
	 */
	virtual bool ActivateAbility() { return false; }

	/**
	 * The cooldown this character's E ability costs, in seconds. Read from UTraceSettings at the
	 * point of use so it retunes live. Default is UTraceSettings::AbilityDefaultCooldownSeconds.
	 */
	virtual float GetActivatedCooldownSeconds() const;

	// =============================================================================================
	// THE MOVEMENT ABILITY — piggybacks on existing inputs (spec §5)
	// =============================================================================================

	/**
	 * Jump was pressed. Return TRUE to consume it — the normal jump will not run.
	 *
	 * Rocco's second jump and Oyster's jar-jump both live here. Called on the owning client and on
	 * the server. Nothing here is a cooldown; if your jump needs one, it is your own timer in the
	 * net state and it resets at half time like everything else.
	 */
	virtual bool OnJumpPressed() { return false; }
	virtual void OnJumpReleased() {}

	/**
	 * A dash is starting, in @p DashDirection (normalised, world space). Oyster drops a jar here —
	 * "at the start of every dash, including while carrying the Core". Chut arms his bash here.
	 * Return true to CANCEL the dash (nothing needs to today).
	 */
	virtual bool OnDashStarted(const FVector& DashDirection) { return false; }

	/** The dash ended. @p bReachedFullDistance is false when it was cut short by a wall. */
	virtual void OnDashEnded(bool bReachedFullDistance) {}

	/**
	 * SERVER ONLY. The dashing pawn overlapped @p Other during the dash. Chut's bash is here, and
	 * it MUST call CanAffect(Other, ETraceAbilityEffect::Control) — spec §6 says the bash has "no
	 * effect on the core carrier" and that is exactly the choke point's Control answer.
	 *
	 * @param DashProgress  0..1 through the dash. Chut's bash is "the END of his standard dash".
	 */
	virtual void OnDashHitCharacter(ATraceCharacter* Other, float DashProgress) {}

	/**
	 * INTEGRATION SEAM. How far, in uu, this character wants the per-frame dash sweep to look for
	 * victims. Return 0 (the default) and UTraceCharacterMovementComponent runs NO sweep at all for
	 * this pawn — which is every Mannequin, every bot and four of the five characters.
	 *
	 * THE MOVEMENT COMPONENT MUST NOT KNOW ABOUT CHUT. It knows only "somebody wants a radius", and
	 * the rule for what to do with a hit stays in the character's OnDashHitCharacter. That is the
	 * whole reason this is a float on the ability set rather than a knob read in Movement/.
	 */
	virtual float GetDashHitSweepRadius() const { return 0.f; }

	/**
	 * The SECONDARY ability key, V by default (spec §5: "Mace's suspend needs its own bind (V)").
	 * Press and release are both delivered because Mace's suspend is a HOLD: "Releasing V cancels
	 * immediately and gravity resumes."
	 *
	 * Return true from the press to say you used it (currently only used for logging).
	 */
	virtual bool OnSecondaryPressed() { return false; }
	virtual void OnSecondaryReleased() {}

	// =============================================================================================
	// THE PASSIVE — pure query hooks, called from the systems that own each number
	// =============================================================================================

	/**
	 * Multiplier on this player's max ground speed. 1.0 = unchanged.
	 * Rocco's stacking headshot boost and X's "+10% while any enemy is vulnerable" are both here.
	 * Called every movement tick on every machine, so keep it cheap and pure.
	 */
	virtual float GetMoveSpeedMultiplier() const { return 1.f; }

	/**
	 * Multiplier on this player's Core magnet (catch) radius. Mace is 1.30 — "+30% magnet radius…
	 * The base is now 450 uu, so Mace's is 585 uu. Derive it, do not hardcode."
	 */
	virtual float GetMagnetRadiusMultiplier() const { return 1.f; }

	/**
	 * Damage this player is ABOUT to take, before it lands. Return the modified amount.
	 * Chut's Chud is here (−30% from body shots and melees). Called on the server.
	 */
	virtual float ModifyIncomingDamage(float Damage, const FTraceAbilityDamageContext& Context) const { return Damage; }

	/**
	 * Damage this player is about to DEAL. Return the modified amount. Nothing in §6 needs it yet;
	 * it exists so the vulnerable multiplier has a symmetric partner and so a future "+x% damage"
	 * passive is not a new hook.
	 */
	virtual float ModifyOutgoingDamage(float Damage, const FTraceAbilityDamageContext& Context) const { return Damage; }

	/**
	 * Extra multiplier applied to ALL damage this player takes, from every source. X's vulnerable
	 * is here: 1.25 while marked. Separate from ModifyIncomingDamage so that a reduction and an
	 * amplification compose in a defined order (reduction first, then amplification).
	 */
	virtual float GetIncomingDamageMultiplier() const { return 1.f; }

	/**
	 * SERVER ONLY. This player killed somebody. Rocco's headshot stack and Chud's knife-kill refresh
	 * are both here.
	 */
	virtual void OnKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot) {}

	// =============================================================================================
	// FRAMEWORK PLUMBING — do not call from a character file
	// =============================================================================================

	/** Called once by the component immediately after NewObject. */
	void Initialize(UTraceAbilityComponent* InComponent);

	virtual UWorld* GetWorld() const override;

	/**
	 * The class that claims @p Id, or null. Built once by reflection over every subclass of this
	 * class; see the header comment for why there is no registration table.
	 */
	static UClass* FindClassFor(ETraceCharacterId Id);

	/** Logs the whole discovered roster. Behind Trace.Ability.DumpRoster. */
	static void LogDiscoveredRoster();

private:
	UPROPERTY(Transient)
	TObjectPtr<UTraceAbilityComponent> AbilityComponent = nullptr;
};
