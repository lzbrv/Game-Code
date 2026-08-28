// Trace — ROXIE, spec v18 §2.
//
// ===================================================================================================
// WHAT §2 SAYS, VERBATIM, AND WHERE EACH CLAUSE IS IMPLEMENTED
// ===================================================================================================
//
//   PASSIVE   "jumps 15% higher"
//
//        ApplyJumpProfile(). It scales the pawn's UCharacterMovementComponent::JumpZVelocity by
//        sqrt(1 + RoxieJumpHeightBonus) — 1.0724, NOT 1.15.
//
//        *** THE SQUARE ROOT IS THE WHOLE POINT AND THIS PROJECT HAS SHIPPED THE MISTAKE ONCE. ***
//        Apex height under gravity is v^2 / 2g, so height goes as the SQUARE of launch speed: a naive
//        x1.15 on the velocity buys +32.25% height, more than double what was asked for. Spec v16 §0
//        asked for "+25% distance" on Chut's bash, got +65.8% from the naive scale, and had to be
//        solved back out. The knob is named for what the designer asked for; the code does the maths.
//        Trace.Roxie.JumpTest measures the HEIGHT ratio, not the velocity ratio, for that reason.
//
//        IT IS WRITTEN ON THE PAWN, NOT READ AT A HOOK, and that is forced rather than chosen: the
//        framework has no jump hook that scales a launch (OnJumpPressed can only CONSUME the press,
//        which would delete the wall jump and the slide-jump for Roxie alone), and Movement/ belongs
//        to another slice this pass. See the comment on ApplyJumpProfile for why that is safe — and
//        for the two side effects it honestly has.
//
//   MOVEMENT  the rocket, on V (press). "Launches her backwards, fast and far", "100 damage on
//             impact, anywhere on the body — no headshot/body distinction", "wobbles in flight",
//             35 s cooldown.
//
//        the press           OnSecondaryPressed(). Its own 35 s cooldown, absolute match-clock time,
//                            separate from MODDED's 25 s on E — exactly as Rocco's Ripple is separate
//                            from the dash.
//        the flight, wobble  ATraceRoxieRocket + namespace TraceRoxieRocket (TraceRoxieRocket.h).
//        "launches her BACKWARDS"  ApplySelfLaunch(), which writes ROXIE's velocity and nothing else's.
//        THE 100             ApplyRocketDamageTo(), the ONE damage call site in this feature.
//
//        *** THE 100 IS THE MOST DANGEROUS NUMBER ADDED TO THIS GAME. *** It is flat, it ignores hit
//        zones, and the founding invariant says NO ABILITY MAY DAMAGE A CORE CARRIER. It is dealt with
//        UTraceCharacterAbilitySet::DealDamage, which routes through
//        UTraceAbilityComponent::CanAffectTarget — never by reaching for a health component, and never
//        behind a carrier test written in this file. There is exactly one such call in the whole
//        feature (ApplyRocketDamageTo); the rocket actor has none. Trace.Roxie.RocketCarrierTest
//        red-arms it: with Trace.Ability.CarrierImmune 0 the rocket MUST damage a carrier, and with
//        the shipped rule it must deal exactly zero to that carrier while KILLING a non-carrier enemy
//        in the same fixture on the same call.
//
//   ACTIVATED MODDED, 25 s. "Loads a modded clip: the gun becomes full auto and fire rate x1.65.
//             Lasts one clip OR 5 seconds, whichever comes first."
//
//        the 25 s            GetActivatedCooldownSeconds().
//        the 5 s             FTraceAbilityNetState::EffectEndMatchTime, absolute match-clock time.
//        "one clip"          the v16 ammo system, READ rather than copied: TickModded watches
//                            UTraceWeaponComponent::GetClipAmmo() and IsReloading() and ends MODDED
//                            when the clip she started with runs dry or is replaced. That is §2's own
//                            [ASSUMPTION] — "'one clip' means the clip loaded when Modded starts;
//                            reloading ends the effect" — and bRoxieModdedEndsOnReload is its switch.
//        the x1.65           GetFireIntervalScale(), an override of the ability base class's seam of
//                            the same name. WIRED as of the v18 §2 integration pass — see note 2.
//
//        *** TWO HONEST NOTES ON MODDED, BOTH OF WHICH BELONG IN THE PLAYTEST BRIEF. ***
//
//        1. *** THE FULL-AUTO CLAUSE IS A MECHANIC AS OF SPEC v29 §2b. THIS NOTE USED TO SAY THE
//           OPPOSITE AND IS KEPT AS THE RECORD OF WHY. *** It read: "THE GUN IS ALREADY FULL AUTO —
//           UTraceWeaponComponent::TickComponent re-fires every frame the trigger is held and
//           CanFire() allows, so 'the gun becomes full auto' is already true of the base gun and
//           bRoxieModdedFullAuto ON matches shipped behaviour."
//
//           That was true for eleven specs and stopped being true this pass. §2b: "The pistol is NOT
//           full auto. It must fire once per trigger press. The SMG stays full auto." So there is now
//           a gun the clause can act on, IsFullAutoForced() is read for the first time (by
//           UTraceWeaponComponent::IsFullAutoNow, through TraceRoxie::IsFullAutoForcedFor), and a
//           Roxie with MODDED up can HOLD the trigger on a PISTOL where nobody else can. The felt
//           content of MODDED is therefore now the x1.65 AND the held trigger — and, since §2e, the
//           recoil she pays for both. Note 3.
//        2. THE x1.65 IS NOW WIRED, AND THIS NOTE RECORDS HOW. It used to say the fire gate read
//           UTraceSettings::FireInterval directly in two places inside
//           Gameplay/TraceWeaponComponent.cpp (CanFire() and ServerFire's rate validation) with no
//           per-pawn seam to hook, so MODDED ran its timers, its cooldown and its end conditions
//           correctly and changed NOTHING THE PLAYER COULD FEEL. The v18 §2 integration pass added
//           that seam: UTraceCharacterAbilitySet::GetFireIntervalScale() with
//           UTraceAbilityComponent::GetFireIntervalScaleFor() in front of it, and both call sites now
//           multiply by it. BOTH had to move — scaling only the local gate would have let the client
//           fire faster than the server's validation accepts, which reads as the gun eating bullets.
//           Trace.Roxie.ModdedTest is the acceptance test and it went from 9/1 to 10/0 on that line.
//        3. *** MODDED NOW COSTS HER RECOIL. SPEC v29 §2e: "Roxie's modded should add recoil now." ***
//           GetAddedRecoilScale() is the seam, TraceRoxie::GetAddedRecoilScaleFor() is the weapon's
//           door to it, and UTraceWeaponComponent::GetRecoilPitchScale() is where it lands. Demo 22
//           (spec v25 §5) removed the aim punch for everybody and bRecoilEnabled is STILL FALSE — this
//           ADDS to that switch rather than overriding it, so the shipped build has recoil for exactly
//           one pawn in one state: a Roxie with MODDED up. It is stored as a MULTIPLE of
//           UTraceSettings::RecoilPitchPerShot and never as degrees, for the same reason §0 forced the
//           fire rate to be a ratio. Trace.Weapons.V29 measures the actual view climb over a live
//           burst, with RoxieModdedRecoilScale=0 as the red arm.
//
// ===================================================================================================
// WHAT THIS FILE DELIBERATELY DOES NOT DO
// ===================================================================================================
//
// It does NOT replace the clip with ability rounds the way X's Sting does, even though "loads a modded
// clip" reads like an invitation to. UTraceWeaponComponent::RequestReload REFUSES while any ability
// round is in the clip (a deliberate v16 §1 rule that stops a reflex R from throwing X's five bees
// away). For X that locks out a manual reload for five rounds; for Roxie it would lock one out for up
// to twenty-nine, every time MODDED's 5 s expired with most of the clip left — an invisible, unexplained
// handicap arriving after the buff ended. So MODDED reads the ammo system instead of writing to it,
// which is the same machinery and none of that side effect.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtr.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetRoxie.generated.h"

class ATraceCharacter;
class ATraceRoxieRocket;

/**
 * *** THE TWO SEAMS THE WEAPON NEEDS FROM MODDED (spec v29 §2e and §2b). ***
 *
 * Free functions, taking the shooting ACTOR, exactly as TraceSlimeball::GetFireIntervalScaleFor()
 * does and for the same reason: UTraceWeaponComponent asks one question and does no casting.
 *
 * *** SAID PLAINLY: THIS IS THE SECOND-BEST SHAPE, AND THE BEST ONE IS ONE FILE AWAY. *** The right
 * home for both is the character-agnostic aggregator the ability layer already has —
 * UTraceAbilityComponent::GetFireIntervalScaleFor() is the model, a static on the component
 * dispatching to a virtual on UTraceCharacterAbilitySet, so the gun learns no character's name.
 * Abilities/TraceAbilityComponent.{h,cpp} and Abilities/TraceCharacterAbilitySet.h belong to another
 * slice this pass, so the seam lives here instead and the weapon includes this header. MIGRATING IT
 * IS TWO LINES: add `virtual float GetAddedRecoilScale() const { return 0.f; }` and
 * `virtual bool IsFullAutoForced() const { return false; }` to UTraceCharacterAbilitySet, add the
 * two `...For(Actor)` statics beside GetFireIntervalScaleFor, and repoint the two call sites in
 * UTraceWeaponComponent (GetRecoilPitchScale and IsFullAutoNow). Nothing else reads these.
 */
namespace TraceRoxie
{
	/**
	 * SPEC v29 §2e: "Roxie's modded should add recoil now."
	 *
	 * Added recoil for @p Shooter AS A MULTIPLE OF UTraceSettings::RecoilPitchPerShot —
	 * RoxieModdedRecoilScale while MODDED is up, and 0.0 for everybody else, every bot, every
	 * Mannequin and for Roxie herself the instant MODDED ends.
	 *
	 * *** A MULTIPLE AND NOT A NUMBER OF DEGREES (standing rule). *** The value modifies the base
	 * per-shot kick, so it is stored and returned relative to it; the weapon multiplies. Retune the
	 * base and Roxie moves with it. See UTraceWeaponComponent::GetRecoilPitchScale for the sum it
	 * lands in and for why it ADDS to the global bRecoilEnabled term rather than overriding it.
	 */
	TRACE_API float GetAddedRecoilScaleFor(const AActor* Shooter);

	/**
	 * SPEC v18 §2's "the gun becomes full auto", which spec v29 §2b finally gives something to do.
	 *
	 * True while MODDED is up (and bRoxieModdedFullAuto is set), false for everybody else. The weapon
	 * ORs it with the weapon's own mode, so MODDED can ADD full auto to the now-semi-automatic pistol
	 * and can never take it away from the SMG.
	 */
	TRACE_API bool IsFullAutoForcedFor(const AActor* Shooter);
}

/**
 * Roxie's bits in FTraceAbilityNetState::Flags. Only one character is live on a component at a time,
 * so these cannot collide with Mace's or anybody else's — see the note on TraceAbilityFlags.
 */
namespace TraceRoxieFlags
{
	/** MODDED is up. TraceAbilityFlags::EffectActive. */
	inline constexpr uint8 ModdedActive = 1 << 0;

	/** A rocket is in the air. TraceAbilityFlags::AuxActive. */
	inline constexpr uint8 RocketInFlight = 1 << 2;
}

/**
 * Roxie's three abilities.
 *
 * FTraceAbilityNetState, as Roxie interprets it:
 *   Flags & ModdedActive    MODDED is running.
 *   Flags & RocketInFlight  a rocket is in the air (for the HUD and for proxies; the actor itself is
 *                           replicated, so nothing gameplay-critical depends on this bit).
 *   EffectEndMatchTime      absolute match time MODDED ends — the "5 seconds" half of §2.
 *   AuxEndMatchTime         absolute match time the ROCKET comes off its 35 s cooldown. Replicated so
 *                           a client can grey its own V without a round trip, and so the cooldown
 *                           survives death exactly as the framework's E cooldown does.
 *   Stacks, AuxLocation, AuxDirection   unused.
 */
UCLASS()
class TRACE_API UTraceAbilitySetRoxie : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Roxie; }

	// ---- lifecycle ---------------------------------------------------------------------------------
	virtual void OnEquipped() override;
	virtual void OnUnequipped() override;
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// ---- FX_AUDIO_PLAN §1.2's client FX router, for MODDED's third-person tell -----------------------
	virtual void OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New) override;
	virtual void SyncClientFx(const FTraceAbilityNetState& Current) override;

	/**
	 * *** THE V ROW'S PRODUCER — FX_AUDIO_PLAN §7.2, and it closes finding F2. ***
	 *
	 * Roxie is one of exactly two characters for whom the base class's default `return false` is the
	 * wrong answer, and she is the only one for whom it is a BUG rather than a design decision (Mace
	 * returns false on purpose — Demo 17 hides her suspend cooldown). AuxEndMatchTime has been
	 * replicated since spec v18 *expressly* "so a client can grey its own V", and until this override
	 * existed nothing read it: a replicated field with no consumer, which is the whole of F2.
	 *
	 * It answers from the SAME two published accessors the rest of the feature uses —
	 * GetRocketCooldownRemaining() and TraceRoxieRocket::GetCooldownSeconds() — and never from a
	 * second copy of either number. That matters twice over:
	 *
	 *   * the REMAINING is GetRocketCooldownRemaining(), which is the MAX of the replicated deadline
	 *     and the predicted local mirror, so the row greys on the press rather than a round trip
	 *     later and can never be more permissive than the server's truth;
	 *   * the DURATION is the live clamped knob read (TraceRoxieRocket::GetCooldownSeconds()), not
	 *     UTraceSettings::RoxieRocketCooldownSeconds dereferenced here and not the DataAsset's
	 *     snapshot of it — the F6 dual-source trap. Retune the cooldown mid-PIE and the meter's
	 *     denominator moves with the ability in the same frame.
	 *
	 * It returns TRUE while the rocket is on cooldown AND while it is ready: 0 remaining is a drawn
	 * state, not a reason to draw nothing. The row is how a player learns the key exists.
	 */
	virtual bool GetSecondaryCooldownDisplay(float& OutRemaining, float& OutDuration,
	                                         FString& OutLabel) const override;

	// =============================================================================================
	// PASSIVE — "jumps 15% higher"
	// =============================================================================================

	/**
	 * The VELOCITY scale that buys a +RoxieJumpHeightBonus APEX: sqrt(1 + bonus) = 1.0724 at 0.15.
	 * Read the header before touching this. It is a square root and that is not a rounding choice.
	 */
	float GetJumpVelocityScale() const;

	/** The authored JumpZVelocity for this pawn's movement class, before Roxie touches it. */
	float GetBaseJumpZVelocity() const;

	/** What the pawn's JumpZVelocity actually is right now. For the HUD, the harness and logs. */
	float GetAppliedJumpZVelocity() const;

	// =============================================================================================
	// MOVEMENT — the rocket, on V
	// =============================================================================================

	/**
	 * V pressed. Fires the rocket if it is off cooldown and she has a pawn. Returns true if it fired.
	 *
	 * Runs on the OWNING CLIENT (which predicts the self-launch, so being thrown backwards has no
	 * round trip in it) and on the SERVER (which is the only machine that spawns the projectile).
	 *
	 * [ASSUMPTION], FLAGGED: it does NOT refuse while she is carrying the Core. §2 is silent, this
	 * codebase's precedent is that carrying does not switch an ability off (Oyster drops damaging jars
	 * "including while carrying the Core"), and the rocket is mostly a movement ability. The comment on
	 * the guard in the implementation says how to reverse it in one clause.
	 */
	virtual bool OnSecondaryPressed() override;

	/** True when V is available: off cooldown, alive, and holding a pawn. Correct on clients. */
	bool IsRocketReady() const;

	/** Seconds until V is ready, 0 when it is. Correct on clients — the deadline is replicated. */
	float GetRocketCooldownRemaining() const;

	/** The rocket currently in the air, or null. Server-side only; clients see the replicated actor. */
	ATraceRoxieRocket* GetLiveRocket() const;

	/**
	 * *** SERVER ONLY. THE ONE PLACE THIS FEATURE DEALS DAMAGE. ***
	 *
	 * Called by ATraceRoxieRocket on a body hit, and by Trace.Roxie.RocketCarrierTest — deliberately
	 * the same function, so the harness measures the shipped path rather than a copy of it.
	 *
	 * Goes through UTraceCharacterAbilitySet::DealDamage, i.e. through
	 * UTraceAbilityComponent::CanAffectTarget, which is where spec §4's founding invariant lives. It
	 * therefore returns 0 for a Core carrier — always, with no knob and no exception — and for a
	 * team-mate while friendly fire is off, and for the dead.
	 *
	 * bHeadshot is FALSE and bMelee is FALSE, always: §2 says the rocket deals its damage "anywhere on
	 * the body — no headshot/body distinction", so this feature never looks up a hit zone.
	 *
	 * @return the damage actually dealt. 0 means the choke point refused, and the rocket flies ON
	 *         through that target rather than detonating — see the comment in
	 *         ATraceRoxieRocket::TickFlightAuthority for why a carrier must not become a rocket shield.
	 */
	float ApplyRocketDamageTo(ATraceCharacter* Victim);

	// =============================================================================================
	// ACTIVATED — MODDED
	// =============================================================================================

	virtual bool  CanActivate(FText& OutReason) const override;
	virtual bool  ActivateAbility() override;
	virtual float GetActivatedCooldownSeconds() const override;

	/** True while MODDED is up. Correct on every machine — the flag is replicated. */
	bool IsModdedActive() const;

	/** Seconds of MODDED left, 0 when it is not running. */
	float GetModdedRemainingSeconds() const;

	/**
	 * *** THE SEAM, READ SIDE. MULTIPLY UTraceSettings::FireInterval BY THIS. ***
	 *
	 * 1.0 normally; 1 / RoxieModdedFireRateMultiplier while MODDED is up — 0.606 at the shipped 1.65.
	 *
	 * *** STATED AS A RATIO, NEVER AS AN INTERVAL OR AN RPM (spec v24 §0). *** This used to read
	 * "turning a 0.40 s interval into 0.242 s, i.e. 150 RPM into 248". Spec v24 §4 moved the base gun
	 * to 0.3158 s (190 RPM) and every one of those four numbers went stale in the same instant, while
	 * the CODE was already correct — it multiplies the base, so Roxie moved to ~313 RPM on her own.
	 * The rule is the ratio: MODDED is always exactly RoxieModdedFireRateMultiplier times the base
	 * fire rate, whatever the base becomes. Run Trace.FireRate.Measure for the live figure.
	 *
	 * IT RETURNS AN INTERVAL SCALE, NOT A RATE MULTIPLIER, AND THE INVERSION IS THE WHOLE REASON IT IS
	 * NAMED THIS WAY. §2 asks for "fire rate x1.65" and FireInterval is a PERIOD, so a call site that
	 * multiplied the interval by 1.65 would make Roxie fire SLOWER while the HUD claimed she was
	 * faster — which reads in a playtest as "the ability does nothing", not as a bug, and is exactly
	 * the inversion the plumbing pass warned all three character agents about.
	 *
	 * *** WIRED, spec v18 §2 integration pass. *** It is now an override of
	 * UTraceCharacterAbilitySet::GetFireIntervalScale(), and the two sites that read it are
	 * UTraceWeaponComponent::CanFire() and UTraceWeaponComponent::ServerFire_Implementation's rate
	 * validation — both through UTraceAbilityComponent::GetFireIntervalScaleFor(), so the gun still
	 * does not know Roxie's name. The file header's note 2 described the gap that closed.
	 */
	virtual float GetFireIntervalScale() const override;

	/**
	 * §2: "the gun becomes FULL AUTO". True while MODDED is up and bRoxieModdedFullAuto is set.
	 *
	 * *** WIRED AT LAST, SPEC v29 §2b. *** This used to say "a statement of intent rather than a
	 * switch anything reads… it exists so the clause has a single named answer if the gun is ever
	 * made semi-automatic". The gun has now been made semi-automatic: §2b says "The pistol is NOT
	 * full auto. It must fire once per trigger press." So MODDED's full-auto clause is a mechanic
	 * from this pass on — a Roxie with MODDED up can HOLD the trigger on a pistol, which nobody else
	 * can — and it is read by UTraceWeaponComponent::IsFullAutoNow() through
	 * TraceRoxie::IsFullAutoForcedFor(), the seam at the top of this header.
	 *
	 * The SMG is unaffected: it is full auto for everybody, and the weapon ORs the two answers, so an
	 * ability can only ever ADD full auto and never remove it.
	 */
	bool IsFullAutoForced() const;

	/**
	 * *** SPEC v29 §2e — "Roxie's modded should add recoil now". HER TRADE FOR THE FIRE RATE. ***
	 *
	 * Added per-shot kick AS A MULTIPLE OF UTraceSettings::RecoilPitchPerShot: RoxieModdedRecoilScale
	 * while MODDED is up, 0 otherwise.
	 *
	 * *** A MULTIPLE, NEVER DEGREES (standing rule) *** — the same shape as GetFireIntervalScale()
	 * above and for the same reason spec v24 §0 forced that one to be a ratio: an absolute stored
	 * here would go stale the first time the base recoil moved, silently, with the card and the HUD
	 * still claiming the old trade. The weapon multiplies; nothing here knows what a degree is.
	 *
	 * Demo 22 removed recoil globally and that removal STANDS — bRecoilEnabled is still false. This
	 * ADDS to it rather than overriding it, so recoil is off for everybody and on for her while
	 * MODDED runs; see UTraceWeaponComponent::GetRecoilPitchScale() for the sum.
	 */
	float GetAddedRecoilScale() const;

	// =============================================================================================
	// HARNESS ENTRY POINTS — dev only in effect, but plain functions so the shipping path is identical
	// =============================================================================================

	/**
	 * SERVER ONLY. Fires a rocket bypassing the cooldown, with an explicit wobble seed so a fixture
	 * can reproduce one exact arc. Returns the rocket, or null.
	 *
	 * @param bAlsoSelfLaunch false leaves Roxie standing still, which is what a hit-test fixture wants
	 *                        — the self-launch would otherwise move the shooter out of the scenario.
	 */
	ATraceRoxieRocket* DebugFireRocket(float WobbleSeedTurns, bool bAlsoSelfLaunch);

	/** SERVER ONLY. Clears the rocket's 35 s cooldown, so a fixture does not have to wait it out. */
	void DebugClearRocketCooldown();

private:
	// ---- the passive -------------------------------------------------------------------------------

	/**
	 * Pushes (or restores) JumpZVelocity on the current pawn. IDEMPOTENT, called every tick.
	 *
	 * *** WHY A WRITE AND NOT A HOOK, AND WHY IT IS PREDICTION-SAFE. ***
	 *
	 * There is no "scale this jump" hook on UTraceCharacterAbilitySet: OnJumpPressed can only CONSUME
	 * the press, and a Roxie who consumed it would lose the wall jump, the slide-jump and the coyote
	 * window, all of which live inside UTraceCharacterMovementComponent::DoJump. Adding such a hook is
	 * a Movement/ change and Movement/ is another slice this pass.
	 *
	 * The write is safe for the same reason RefreshEngineTunablesFromSettings' GravityScale write is,
	 * and the argument is worth spelling out because it is the only thing standing between this and a
	 * rubber-band: the value is a PURE FUNCTION of config and of the replicated CharacterId, both ends
	 * compute it identically, and it is re-asserted every tick rather than toggled on an edge — so a
	 * corrected or replayed move re-derives the same number. Nothing else in the project writes
	 * JumpZVelocity (the movement component sets it once in its constructor and never again), so there
	 * is no second writer to fight.
	 *
	 * *** TWO SIDE EFFECTS, STATED RATHER THAN HIDDEN. *** JumpZVelocity is the unit two other launches
	 * are expressed in, so Roxie also gets:
	 *   * a wall jump 15% higher (Velocity.Z = JumpZVelocity x WallJumpVerticalMultiplier), which reads
	 *     as consistent with "jumps 15% higher" and is almost certainly wanted;
	 *   * a dash-exit vertical CAP 7.2% higher (GetDashExitVerticalSpeedLimit). That one is incidental
	 *     and is a ceiling rather than a boost, so it can only matter to a dash that was already
	 *     against the stop.
	 * Neither is reachable from this slice without editing Movement/. Both are named in the report.
	 */
	void ApplyJumpProfile();

	/** Puts JumpZVelocity back to the movement class's authored default. Unequip, death, red arm. */
	void RestoreJumpProfile();

	// ---- the rocket --------------------------------------------------------------------------------

	/** Spawns the projectile. Server only. Returns null when there is no pawn or no world. */
	ATraceRoxieRocket* SpawnRocket(float WobbleSeedTurns);

	/**
	 * "Launches her BACKWARDS, fast and far" — writes Roxie's own velocity, opposite her aim, with the
	 * up bias added so "far" has air time to happen in. No target, so no choke point: this is the same
	 * shape as Mace's suspend and pull, which also move only their own owner.
	 */
	void ApplySelfLaunch();

	/** True on the machines that actually simulate this pawn's movement: the server and its owner. */
	bool ShouldDriveMovement() const;

	// ---- MODDED ------------------------------------------------------------------------------------

	/** SERVER ONLY. Runs MODDED's two end conditions once per ability tick. */
	void TickModded();

	/** SERVER ONLY. Ends MODDED and says why, once, at Log. */
	void EndModded(const TCHAR* Why);

	// ---- MODDED's tell (FX_AUDIO_PLAN §2.3, the "MODDED gun tell" row) ------------------------------
	//
	// TWO HALVES, ON DIFFERENT MACHINES, AND NEITHER IS THE OTHER'S FALLBACK:
	//
	//   third person   her body ACCENT STRIPES lift from bible Glow 1.7 to 2.6, so everybody in the
	//                  arena can see that the Roxie shooting at them is the fast one. Driven from the
	//                  §1.2 router, which runs on EVERY machine off the replicated ModdedActive bit —
	//                  it is the only hook that does, and a tell only the owner could see would be a
	//                  tell for the one player who already knows.
	//   first person   the VIEWMODEL gun's own MIDs get an ember emissive lift, owner only, because
	//                  the viewmodel exists on exactly one machine and nobody else can see it anyway.
	//
	// Both are applied by the same pair of functions and both are torn down by the same clear, so
	// there is no state either half can be left in that the other is not.

	/** Lifts the accent stripes (every machine) and the viewmodel (owner only). IDEMPOTENT. */
	void ApplyModdedTell();

	/** Puts both halves back. Safe to call when the tell was never up. */
	void ClearModdedTell();

	/**
	 * True while ApplyModdedTell has written something that ClearModdedTell has not yet taken back.
	 *
	 * Purely a "do I have anything to undo" latch — the restore goes through ApplyTeamColors(), which
	 * recomputes the correct accent for the pawn's CURRENT state, so this never has to remember a
	 * brightness. See ClearModdedTell for why remembering one would be wrong.
	 */
	bool bModdedTellUp = false;

	/** The live rocket, server side. Weak: the actor may expire or be destroyed under us. */
	TWeakObjectPtr<ATraceRoxieRocket> LiveRocket;

	/**
	 * The rocket's cooldown as the PRESSING machine believes it, on the shared match clock.
	 *
	 * A PREDICTED MIRROR OF FTraceAbilityNetState::AuxEndMatchTime, and it exists for one case: an
	 * owning client presses V, the server's AuxEndMatchTime takes ~RTT/2 to arrive, and a second press
	 * inside that window would otherwise predict a second launch the server never granted. Both
	 * machines run OnSecondaryPressed and both write this from MatchTimeNow(), so they compute the same
	 * instant; the gate takes the LATER of the two, which means the mirror can only ever be stricter
	 * than the replicated truth and never more permissive.
	 */
	float PredictedRocketReadyMatchTime = 0.f;

	/**
	 * SERVER ONLY. The clip count MODDED last saw, so the next tick can tell "she fired a round" from
	 * "the clip was replaced". A rising count is a refill, i.e. a new clip, i.e. the end of "one clip".
	 * -1 means MODDED is not running.
	 */
	int32 ModdedClipTracked = -1;
};
