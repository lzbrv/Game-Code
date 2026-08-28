// Trace — the per-character extension point (spec v14 §5 / §6).
//
// ===================================================================================================
// ADDING A CHARACTER. READ THIS FIRST.
// ===================================================================================================
//
// This front matter was written for the original five-agent pass, when five agents were each given
// one character and told not to touch each other's files. The ROSTER IS TEN NOW (v14's five, +Roxie
// /Elle/Slimeball at v18 §2, +Mortimer/Lily at v19 §3) and the instructions below have held for
// every one of them unchanged — which is the point of the reflection bridge. Read "the five agents"
// as "whoever adds the eleventh".
//
// You add exactly ONE new file pair to Source/Trace/Abilities/Characters/ and you edit NOTHING ELSE:
//
//     Abilities/Characters/TraceAbilitySetRocco.h / .cpp     (and the other nine)
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
	 * *** 20 Hz, NOT PER FRAME. *** On every machine that has the component. Called from the
	 * component's tick, which runs on the PlayerState and therefore keeps ticking while the player is
	 * DEAD — that is deliberate and is how a cooldown counts down through a death.
	 *
	 * THE RATE IS LOAD-BEARING and this comment used to say "per-frame", which is wrong and will make
	 * you mis-reason about anything continuous. UTraceAbilityComponent's constructor sets
	 * PrimaryComponentTick.TickInterval = 0.05f, so DeltaSeconds arrives in ~50 ms lumps while
	 * physics integrates every frame. Mace's pull and V-suspend write Velocity from here, i.e. they
	 * write it 20 times a second into a simulation that steps 60+ times a second. That was measured
	 * rather than assumed (spec v15 §6: 37.9 uu of suspend drift over 1.25 s against 529 uu of free
	 * fall, pull peak within 6 uu/s of the cap) and found to be adequate — but it is a granularity
	 * limit, and a future ability that needs frame-accurate integration needs an opt-in on the
	 * component rather than a character quietly poking PrimaryComponentTick behind its back.
	 *
	 * Guard anything that needs a pawn on GetCharacter() != nullptr.
	 */
	virtual void TickAbilities(float DeltaSeconds) {}

	// =============================================================================================
	// THE CLIENT FX ROUTER — FX_AUDIO_PLAN §1.2. Two hooks, one rule.
	// =============================================================================================
	//
	// THE RULE, and everything below follows from it: THE ROUTER OWNS EVERYTHING A NON-OWNER CAN SEE
	// OR HEAR. First-person predicted feedback (the local dash, the owner's own cast flash) stays in
	// the input path — TryActivate() and the kit's local half — because it must not wait for a round
	// trip. Third-person presentation — attached loop FX, world beats, loop SOUNDS — belongs here,
	// because here is the only place that runs on every machine with the same information.
	//
	// The two must not double-fire. If an element is in both halves, the owner sees it twice; if it is
	// in neither, everybody but the owner sees nothing, which is the F10 blocker this plan exists to
	// close.
	//
	// FOUR OBLIGATIONS ON EVERY IMPLEMENTATION (they are cheap, and each one is a shipped bug if it is
	// skipped):
	//   1. Resolve the pawn through GetOwningPlayerState()->GetPawn(). A NULL pawn means DETACH
	//      EVERYTHING — a component parented to nothing is a component nobody will ever clean up.
	//   2. Re-attach in OnPawnSpawned(), detach in OnPawnDied(). Attached FX must never survive onto a
	//      corpse; the state that drove them is wiped on death by ApplyDeathStateWipe() anyway.
	//   3. Build every component through UTraceFxShapes::ConfigureFxComponent + MakeGlowMID, and honour
	//      the achieved blend: ETraceFxBlend::None hides the component rather than showing it grey.
	//   4. Budget (bible §6.4): at most FOUR attached primitives per pawn, additive intensity <= 0.5,
	//      inside 96 uu of the capsule axis. Motion is allowed; a brightness pulse on a lethal
	//      telegraph is not.
	//
	// Elle's cloak is the one interaction worth naming: emissive FX attached by a router hook must
	// either register with the same ApplyTeamColors() refresh that restores body emissives
	// (TraceAbilitySetElle.cpp:60-181) or be plainly hidden while IsCloakVisualApplied().

	/**
	 * EDGE-TRIGGERED, on every machine: the replicated scratch pad CHANGED from @p Old to @p New.
	 *
	 * Called from UTraceAbilityComponent::RouteNetStateEdges — on a client from OnRep_AbilityState, on
	 * the authority from the component's own 20 Hz tick, so a listen host sees remote players' edges
	 * at most 50 ms late and its own the moment MarkStateDirty() runs.
	 *
	 * Compare the fields you care about (Old.Flags vs New.Flags for a rising bit, the two
	 * EffectEndMatchTimes for a re-arm) and do only presentation here: no damage, no movement, no
	 * state writes. The state is already what it is; this is the notification, not the decision.
	 */
	virtual void OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New) {}

	/**
	 * FIRST SIGHT, on every machine: this machine now has a valid state and has NOT been told about
	 * the edges that produced it. Attach the loop FX for whatever is ALREADY on.
	 *
	 * When it runs: a client joining a match in progress, a character swap, a pawn respawn — anything
	 * that builds the ability set fresh while the state is non-empty. It is the difference between
	 * "Lily's flight aura is on every machine that watched her cast" and "on every machine, including
	 * the one that connected halfway through the flight".
	 *
	 * MUST BE IDEMPOTENT. It can run twice for the same live state (a rebuild that lands on the same
	 * character), and it must not stack a second set of components when it does.
	 */
	virtual void SyncClientFx(const FTraceAbilityNetState& Current) {}

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
	 * INTEGRATION SEAM (spec v18 §2). Multiplier the gun applies to UTraceSettings::FireInterval for
	 * this player. 1.0 = the ordinary rate, which is every character except two.
	 *
	 * *** IT SCALES A PERIOD, SO A FASTER GUN RETURNS A SMALLER NUMBER. *** Roxie's Modded is "fire
	 * rate ×1.65", i.e. 1/1.65 = 0.606 of the interval; Slimeball's stuck passive is "+30% fire rate",
	 * i.e. 1/1.30 = 0.769. A character that returned 1.65 here would fire SLOWER while its card and
	 * its HUD both claimed faster — which reads in a playtest as "the ability does nothing" rather
	 * than as a bug, and is the inversion every note on both characters warns about.
	 *
	 * THE GUN MUST NOT KNOW EITHER CHARACTER'S NAME, which is the whole reason this is a virtual here
	 * rather than two casts in Gameplay/TraceWeaponComponent.cpp — the same argument
	 * GetDashHitSweepRadius() makes for the movement component and Chut.
	 */
	virtual float GetFireIntervalScale() const { return 1.f; }

	/**
	 * INTEGRATION SEAM (spec v18 §2). This character's WELL-TIMED slide-jump planar multiplier,
	 * derived from the one the movement component computes for everybody.
	 *
	 * @param InWellTimedBonus  whatever UTraceCharacterMovementComponent::GetSlideJumpWindowSpeedBonus()
	 *                          answers globally (1.375 today — it was 1.446875 when this was written,
	 *                          and spec v26 §3a and v28 §5 have both moved it since).
	 *
	 * The global number is passed IN rather than read here so there is exactly one definition of "the
	 * base" and it is the shipped one — a character that read UTraceSettings itself would quietly stop
	 * tracking a retune of the base. THAT DESIGN IS WHY THE STALE NUMBER ABOVE COST NOTHING: the base
	 * moved twice and every character tracked it, because none of them holds a copy. Elle is the only
	 * override (+30% of the GAIN as of Patch 28 §3, not the multiplier); everybody else returns their
	 * argument, which is what makes spec v18 §4's "slide-jump (Elle changes only her own)" true by
	 * construction — the parenthesis is the invariant, the number in the spec's sentence is only the
	 * value it happened to have.
	 */
	virtual float ModifySlideJumpWindowSpeedBonus(float InWellTimedBonus) const { return InWellTimedBonus; }

	/**
	 * Seconds this CHARACTER will refuse its own activated ability for, over and above the framework's
	 * cooldown. 0 (the default) means "the framework's timer is the whole truth", which is every
	 * character but one.
	 *
	 * WHY IT EXISTS: the HUD's cooldown ring reads the framework timer, and for a character whose
	 * CanActivate() enforces a longer wait of its own the ring says READY while the ability refuses.
	 * That is worse than a wrong number — a player presses a lit button, nothing happens, and there is
	 * nothing on screen that could explain it. Elle's Snap is the case: her FIRST press deliberately
	 * charges no framework cooldown (or the second press could never reach her), so a cast she never
	 * completes leaves the framework at zero and Elle refusing for up to 31 s.
	 *
	 * Read through UTraceAbilityComponent::GetActivatedCooldownRemaining(), which takes the max of
	 * this and its own two deadlines — so a character can only ever make the ring MORE conservative,
	 * never make a genuinely-cooling ability look ready.
	 */
	virtual float GetCharacterOwnedCooldownRemaining() const { return 0.f; }

	/**
	 * THE SECONDARY (V) ABILITY'S COOLDOWN, FOR THE HUD (release FX/AUDIO plan §7.2, closes F2).
	 *
	 * Return false — the default, and the answer for eight of the ten characters — and the HUD draws
	 * NO V row at all. Return true and it draws a second, half-height row under the E row: the
	 * player's own [V] binding, the character's accent, and @p OutLabel with @p OutRemaining beside
	 * it, greyed while cooling and flashing once on the rising edge of ready.
	 *
	 * *** WHY THIS IS A VIRTUAL AND NOT A CAST IN TraceHUD.cpp. *** The HUD already knows five
	 * character classes by name for the status chips and every one of those is a place a new
	 * character has to remember to be added. A V cooldown is not character-specific INFORMATION — it
	 * is "this character's secondary is on a timer" — so the question belongs on the base class,
	 * exactly as GetDashHitSweepRadius() moved Chut's radius out of the movement component.
	 *
	 * *** THE TWO ANSWERS THAT ARE NOT "false" ARE BOTH DECISIONS. ***
	 *   - ROXIE returns true. Her FTraceAbilityNetState::AuxEndMatchTime is replicated *expressly*
	 *     "so a client can grey its own V" (TraceAbilitySetRoxie.h) and until this row existed it
	 *     was read by nothing — a replicated field with no consumer, which is the F2 finding.
	 *   - MACE returns FALSE ON PURPOSE. Demo 17 hides her suspend cooldown (TraceAbilitySetMace.h);
	 *     lighting it up here would be reversing a design decision by accident.
	 *
	 * @param OutRemaining  seconds until V is usable. 0 means READY, which is a drawn state, not a
	 *                      reason to return false — the row is how the player learns the key exists.
	 * @param OutDuration   the full cooldown, for the meter's denominator. Read it from the LIVE
	 *                      UTraceSettings knob, never from the DataAsset copy (the F6 dual-source
	 *                      trap: the DA is a snapshot and drifts the moment somebody retunes).
	 * @param OutLabel      the ability's own name, upper case ("ROCKET"). Short: it shares a row.
	 */
	virtual bool GetSecondaryCooldownDisplay(float& OutRemaining, float& OutDuration, FString& OutLabel) const
	{
		return false;
	}

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
