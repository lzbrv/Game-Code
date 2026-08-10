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
//        1. THE GUN IS ALREADY FULL AUTO. UTraceWeaponComponent::TickComponent re-fires every frame
//           the trigger is held and CanFire() allows, so holding mouse1 already empties a clip at
//           FireInterval. "The gun becomes full auto" is therefore already true of the base gun and
//           bRoxieModdedFullAuto ON matches shipped behaviour. What actually makes a 0.40 s gun FEEL
//           semi-automatic is the 150 RPM cadence — so the whole felt content of that clause is the
//           x1.65, which is note 2.
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
	 * 1.0 normally; 1 / RoxieModdedFireRateMultiplier while MODDED is up — 0.606 at the shipped 1.65,
	 * turning a 0.40 s interval into 0.242 s, i.e. 150 RPM into 248.
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
	 * The base gun is ALREADY full auto (see the file header, note 1), so this is currently a
	 * statement of intent rather than a switch anything reads. It exists so the clause has a single
	 * named answer if the gun is ever made semi-automatic.
	 */
	bool IsFullAutoForced() const;

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
