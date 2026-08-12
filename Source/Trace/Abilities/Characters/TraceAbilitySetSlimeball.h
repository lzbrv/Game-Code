// Trace — SLIMEBALL (spec v18 §2).
//
// ===================================================================================================
// THE DOC, VERBATIM
// ===================================================================================================
//
//   MOVEMENT  "Wall stick (Hold V): sticks to walls while held."
//
//             DEMO 17 gave it an EXIT: "Slimeball should be able to cancel the wall stick with jump,
//             and perform a wall jump off the wall." So the stick is a position to take rather than a
//             commitment to make — see OnJumpPressed() below, and note the two things that fix has to
//             not break: he must not be able to re-grab the same face on the next 20 Hz sweep while V
//             is still held (that is wall-jump stickiness, which spec v10 §5 removed for everybody),
//             and stick-jump-stick-jump must not be an infinite ladder (spec v8 §7's consecutive
//             cap, which this launch does not go through and therefore has to honour itself).
//
//   PASSIVE   while stuck, "+30% fire rate and 30% damage reduction on body shots and front knife
//             stabs."   [ASSUMPTION] headshots and BACK stabs are unreduced — the doc names body
//             shots and front stabs, exactly as Chut's Chud names body shots and melees.
//
//   ACTIVATED "Slimewall (25 s cooldown): throws up a wall in his aim direction, one player height
//             tall and wide, and about the length of a standard in-game box — (for now, make this
//             changeable). The wall can be shot through but obstructs vision, and moving through it
//             slows enemies by 35%. Lasts 4 s."
//
// V IS SHARED THREE WAYS NOW — Mace's suspend (hold), Roxie's rocket (press) and this stick (hold).
// That is fine because it is per-character: all three arrive through OnSecondaryPressed /
// OnSecondaryReleased and each character interprets them. There is no key test anywhere in this file
// and there must never be one.
//
// ===================================================================================================
// SLIMEBALL AND THE CARRIER CHOKE POINT
// ===================================================================================================
//
// Exactly ONE thing he does touches another player: the Slimewall's 35% slow. It is Control, it goes
// through UTraceAbilityComponent::CanAffect, and it does so in ATraceSlimewall — not here — because
// the wall outlives the pawn that threw it. There is deliberately no carrier test in this file.
//
// The wall stick and the two stuck passives write only his OWN pawn and his OWN damage numbers, so
// they need no choke point at all, for the same reason Mace's file has none.
//
// ===================================================================================================
// HOW THE STICK MOVES HIM, AND WHY IT IS DONE THIS WAY
// ===================================================================================================
//
// It writes UCharacterMovementComponent::Velocity every tick from TickAbilities(), on the SERVER and
// on the OWNING CLIENT, from state both machines compute the same way. That is Mace's suspend
// verbatim, and the two closed alternatives are the same two:
//
//   * GravityScale = 0 does not survive one frame — UTraceCharacterMovementComponent::
//     RefreshEngineTunablesFromSettings() re-pushes it from UTraceSettings on every simulated move.
//   * a proper saved-move flag is the right long-term answer and lives in Movement/, which this slice
//     does not own.
//
// So the stick zeroes Velocity each tick (or sets a downward slide, if SlimeballWallStickSlideSpeed
// is ever tuned above 0). The cost is the same one Mace's file documents: this is not saved-move
// state, so a corrected/replayed move re-derives velocity without the ability and the correction path
// absorbs the difference. It is a named limitation, not a claim.
//
// ===================================================================================================
// THE FIRE RATE IS WIRED UP, AS OF THE v18 §2 INTEGRATION PASS
// ===================================================================================================
//
// GetFireIntervalMultiplier() below used to be correct, red-armed and CALLED BY NOBODY except
// Trace.Slimeball.Verify: UTraceWeaponComponent read UTraceSettings::FireInterval directly at two
// call sites, so a stuck Slimeball took 30% less damage and fired at exactly the normal rate.
//
// The seam that closed it is UTraceCharacterAbilitySet::GetFireIntervalScale(), fronted by
// UTraceAbilityComponent::GetFireIntervalScaleFor() so the gun learns no character's name — the same
// aggregator Roxie's Modded reads, added ONCE rather than as two casts at each of the two sites.
// GetFireIntervalScale() below is this character's one-line override of it, and
// TraceSlimeball::GetFireIntervalScaleFor() survives unchanged as the free-function form the harness
// already drives. BOTH weapon call sites scale: the local gate in CanFire() and the SERVER's rate
// validation in ServerFire_Implementation. Scaling only the first would let the client fire faster
// than the server accepts, which reads as the gun eating bullets rather than as a buff.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetSlimeball.generated.h"

class ATraceSlimewall;

/**
 * Slimeball's bits in FTraceAbilityNetState::Flags. Only one character is live per component, so
 * these cannot collide with Mace's or Chut's.
 */
namespace TraceSlimeballFlags
{
	/** Stuck to a wall: holding V with a wall in reach. TraceAbilityFlags::MovementActive. */
	inline constexpr uint8 Stuck = 1 << 1;
	/** A Slimewall of his is standing. TraceAbilityFlags::AuxActive. */
	inline constexpr uint8 WallUp = 1 << 2;
}

/**
 * THE SEAM THE WEAPON NEEDS.
 *
 * Character-agnostic on purpose: UTraceWeaponComponent must not learn the name of one character (see
 * Abilities/Characters/TraceAbilityWeaponHooks.h for the same argument about X's Sting). Whoever
 * wires this should ideally add ONE aggregator that multiplies Slimeball's answer by Roxie's rather
 * than two casts at each call site.
 */
namespace TraceSlimeball
{
	/**
	 * Multiplier the weapon should apply to UTraceSettings::FireInterval for @p Shooter.
	 *
	 * *** IT DIVIDES, BECAUSE FireInterval IS A PERIOD AND THE KNOB IS A RATE. *** +30% fire rate is
	 * 1/1.30 = 0.769 of the interval, i.e. 0.400 s becomes 0.308 s. Multiplying by 1.30 instead would
	 * make him fire SLOWER while the card claims he is faster — which reads in a playtest as "the
	 * passive does nothing", the hardest kind of bug to report.
	 *
	 * 1.0 for everybody who is not a stuck Slimeball, including every Mannequin and every bot.
	 */
	TRACE_API float GetFireIntervalScaleFor(const AActor* Shooter);
}

UCLASS()
class TRACE_API UTraceAbilitySetSlimeball : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Slimeball; }

	// --- lifecycle ---------------------------------------------------------------------------------
	virtual void OnUnequipped() override;
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// --- MOVEMENT: hold V to stick to a wall ---------------------------------------------------------

	/** V pressed. Starts the stick if a wall is in reach and he is off the floor. True if it started. */
	virtual bool OnSecondaryPressed() override;

	/** V released. Lets go immediately — gravity resumes on the same tick. */
	virtual void OnSecondaryReleased() override;

	/**
	 * DEMO 17 item 2, verbatim: "Slimeball should be able to cancel the wall stick with jump, and
	 * perform a wall jump off the wall."
	 *
	 * Returns TRUE — consuming the press — only while he is actually stuck. Consuming matters: a
	 * mid-air press that reached ACharacter::Jump() with no wall-jump window open is refused by
	 * UTraceCharacterMovementComponent::DoJump anyway, so returning false would spend the stick and
	 * launch nothing.
	 *
	 * *** IT DOES NOT CALL THE MOVEMENT COMPONENT'S TryWallJump(), AND CANNOT. *** That function is
	 * protected, is only reachable from inside DoJump, and needs a contact window that a stuck pawn
	 * does not have — the stick zeroes Velocity, so nothing collides with the wall and HandleImpact
	 * never opens one. So the launch is written here, the way Rocco's second jump and Oyster's jar jump
	 * are written in their own files, and every NUMBER in it is read from the shipped wall-jump knobs
	 * (WallJumpOutwardImpulse, WallJumpVerticalMultiplier, WallJumpSpeedRetention) so a retune of the
	 * wall jump carries him with it. A stuck pawn's planar speed is zero by construction, which is the
	 * case TryWallJump itself describes as "the outward impulse is the whole launch".
	 *
	 * Off the wall it returns false and the ordinary jump runs, untouched.
	 */
	virtual bool OnJumpPressed() override;

	// --- PASSIVE: while stuck -----------------------------------------------------------------------

	/**
	 * §2: "30% damage reduction on body shots and front knife stabs", WHILE STUCK ONLY.
	 *
	 * Chud's shape exactly (UTraceAbilitySetChut::ModifyIncomingDamage): the exclusions are by CAUSE
	 * as well as by flag, because the flag is filled in by whichever slice is calling and the cause is
	 * not. Server side.
	 */
	virtual float ModifyIncomingDamage(float Damage, const FTraceAbilityDamageContext& Context) const override;

	/** See TraceSlimeball::GetFireIntervalScaleFor. 1/(1+bonus) while stuck, 1.0 otherwise. */
	float GetFireIntervalMultiplier() const;

	/**
	 * The ability base class's fire-rate seam, which the gun reads through
	 * UTraceAbilityComponent::GetFireIntervalScaleFor(). One line, forwarding to the function above,
	 * so that this character's answer has exactly one definition and the harness and the gun cannot
	 * disagree about it.
	 */
	virtual float GetFireIntervalScale() const override { return GetFireIntervalMultiplier(); }

	/**
	 * Is he stuck to a wall right now?
	 *
	 * Answers from the LOCAL flag on the machines that actually drive the stick (the server and the
	 * owning client) and from the replicated flag everywhere else, which is what makes a proxy's HUD
	 * and a remote client's cosmetics agree with the server. Same split, and the same reason, as
	 * UTraceAbilitySetMace::HasSpikeEmbedded.
	 */
	bool IsStuck() const;

	/** Seconds of stick left when SlimeballWallStickMaxSeconds caps it; 0 means "as long as V is held". */
	float GetStickSecondsRemaining() const;

	// --- ACTIVATED: Slimewall -----------------------------------------------------------------------

	virtual float GetActivatedCooldownSeconds() const override;
	virtual bool  CanActivate(FText& OutReason) const override;

	/** Throws the wall up. Returns FALSE (a free fizzle, no cooldown) when there is nowhere to put it. */
	virtual bool ActivateAbility() override;

	/** His standing wall, or null. Server only holds the pointer; clients see the replicated actor. */
	ATraceSlimewall* GetActiveWall() const;

	// --- harness seams ------------------------------------------------------------------------------

	/**
	 * The wall sweep on its own: "is there something to stick to from here, and if not, why not?"
	 * @param OutPoint     the contact point on the wall.
	 * @param OutNormal    the wall's surface normal.
	 * @param OutDistance  gap from the capsule centre to the contact, in uu.
	 */
	bool ProbeForWall(FVector& OutPoint, FVector& OutNormal, float& OutDistance, FString& OutWhy) const;

	/** The placement sweep on its own: "where would a Slimewall go from here?" */
	bool ResolveSlimewallPlacement(FVector& OutCenter, FVector& OutPlanarAim, FVector& OutHalfExtents,
	                               FString& OutWhy) const;

	/**
	 * HARNESS ONLY. Forces the stuck state without a wall, so the two stuck PASSIVES can be measured
	 * without steering a pawn at geometry. Does not move him and does not touch the movement
	 * component — the stick's own motion is proved by Trace.Slimeball.StickTest against real walls.
	 */
	void DebugSetStuck(bool bForceStuck);

private:
	/** True while V is down, whether or not a wall was found. Mirrors Mace's bSuspendHeld. */
	bool bSecondaryHeld = false;

	/** True while actually attached. The passives read this, not bSecondaryHeld. */
	bool bStuck = false;

	/** Absolute match time the stick is force-released. 0 = no cap (the shipped knob). */
	float StickEndMatchTime = 0.f;

	/** Absolute match time the next stick is allowed. 0 = ready (the shipped knob). */
	float StickReadyMatchTime = 0.f;

	/**
	 * DEMO 17 item 2. Wall jumps taken OFF A STICK since he last touched the ground.
	 *
	 * *** THIS IS THE ANTI-LADDER CAP AND IT IS NOT OPTIONAL. *** The movement component keeps its own
	 * WallJumpsSinceGround and refuses past WallJumpMaxConsecutive (2) — spec v8 §7 asks for that by
	 * name, because "two close walls become an infinite ladder" otherwise. This launch does not go
	 * through that counter and therefore cannot be stopped by it, so stick-jump-stick-jump would have
	 * been a free lift to the skybox for exactly one character. Counted here, capped against the SAME
	 * shipped knob, and cleared by the ground exactly as the movement component clears its own.
	 */
	int32 StickJumpsSinceGround = 0;

	/** The surface he is on. Used to re-probe along the same axis each tick rather than sweeping again. */
	FVector StickWallNormal = FVector::ZeroVector;

	/** Server only. A wall he threw; weak because it destroys itself after 4 s. */
	TWeakObjectPtr<ATraceSlimewall> ActiveWall;

	void StartStick(const FVector& WallNormal);
	void StopStick(const TCHAR* Why);
	void ApplyStick(float DeltaSeconds);

	/** Server only. Mirrors the local state into the replicated scratch pad for the HUD and proxies. */
	void PublishState();

	/** True on the machines that actually simulate this pawn's movement: the server and its owner. */
	bool ShouldDriveMovement() const;

	/** Destroys his standing wall, if any. Half time and unequip only — death leaves it its 4 s. */
	void ClearWall();
};
