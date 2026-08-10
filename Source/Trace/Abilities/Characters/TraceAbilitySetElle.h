// Trace — ELLE, spec v18 §2.
//
// ===================================================================================================
// WHAT §2 ASKS FOR, CLAUSE BY CLAUSE, AND WHERE EACH ONE LIVES
// ===================================================================================================
//
//   PASSIVE 1 — CLOAK. "Right after passing or throwing the trace, Elle automatically cloaks
//             themselves for 3 seconds."
//
//             *** [ASSUMPTION], FLAGGED: "the trace" IS READ AS THE CORE. *** A trail is not passed
//             and not thrown; the Core is the only thing in this game that is either. Elsewhere in
//             the same document "trace" and "core" are used interchangeably, and every other reading
//             produces an ability with no trigger.
//
//             The trigger is therefore the falling edge of "Elle holds the Core", qualified: she must
//             still be ALIVE, and the Core must have gone either to a living TEAM-MATE (a completed
//             pass) or LOOSE (a mode-B throw). Dying with it, being killed for it and half time all
//             fail that test, which is the difference between "she gave it up" and "it was taken".
//
//             [ASSUMPTION] VISUAL ONLY: no hitbox change, no aim-assist change, no lag-compensation
//             change. Anything that made her harder to HIT rather than harder to SEE would be a
//             defensive ability §2 did not ask for, and it would interact with the server rewind in
//             ways nobody has designed. [ASSUMPTION] it drops early if she takes the Core back
//             (bElleCloakEndsOnCorePickup).
//
//             *** IT REPLICATES, AND THAT IS A REQUIREMENT RATHER THAN A DETAIL. *** The fact lives
//             in FTraceAbilityNetState, is written only by the server, and is applied cosmetically on
//             every machine from the replicated value. A cloak only the cloaked player can see is
//             worthless; a cloak the server does not know about is a cheat.
//
//             Knobs: ElleCloakDurationSeconds, ElleCloakOpacity, bElleCloakEndsOnCorePickup.
//
//   PASSIVE 2 — "+40% on well-timed slide-jump momentum boosts."
//             ElleSlideJumpGainBonus scales the GAIN, not the whole multiplier: everyone's well-timed
//             slide-jump is x1.446875, i.e. 1 + 0.446875 of gain, and +40% of the gain is
//             0.446875 x 1.40 = 0.625, so Elle's is x1.625. Scaling the whole multiplier would give
//             x2.025 — an ability more than twice the size of the one asked for. That reading is the
//             project's own (bSlideJumpBonusScalesGainOnly under Movement|Slide) rather than one
//             invented here.
//
//             GetSlideJumpWindowSpeedBonusForElle() below DERIVES it from the shipped global number
//             at the point of use, so a retune of the base carries Elle with it and 1.625 is never
//             hardcoded anywhere.
//
//             *** WIRED IN AS OF THE v18 §2 INTEGRATION PASS. ***
//             UTraceCharacterMovementComponent::GetSlideJumpWindowSpeedBonus() used to read
//             UTraceSettings and ask no ability anything, so this passive reached no actual
//             slide-jump. The missing piece was the per-pawn seam that ground speed
//             (GetMoveSpeedMultiplierFor) and the dash sweep (GetDashHitSweepRadiusFor) already had;
//             it now exists as UTraceCharacterAbilitySet::ModifySlideJumpWindowSpeedBonus() with
//             UTraceAbilityComponent::GetSlideJumpWindowSpeedBonusFor() in front of it, and the
//             movement component's last line passes its own answer through it.
//
//             It was deliberately NOT faked from Elle's own 20 Hz tick — topping a launch velocity up
//             after the fact would have been a second, wrong copy of the slide-jump rule.
//
//   ACTIVATED — SNAP, 35 s. "Places a portal gate where she stands. She may reactivate within 4 s to
//             place a second gate, and then players teleport between them. If no second gate inside
//             4 s the first expires. With both placed, both expire after 8 s."
//
//             The gates are ATraceElleGate actors — see that header for the founding-invariant
//             argument and for the two [ASSUMPTION] knobs (bElleSnapUsableByBothTeams,
//             bElleSnapCarrierMayUseGate). What lives HERE is Elle's cast: the two-press shape, the
//             4 s window, the pairing, and the cooldown.
//
// ===================================================================================================
// THE COOLDOWN, AND THE ONE PLACE THIS ABILITY DOES NOT FIT THE FRAMEWORK CLEANLY
// ===================================================================================================
//
// UTraceAbilityComponent::TryActivate() refuses while the activated cooldown is running, and it
// checks that BEFORE the character's CanActivate(). SNAP is a TWO-PRESS cast, so if the first press
// started the 35 s cooldown the second press could never reach Elle at all and the ability would have
// no second gate. This is exactly the problem Mace's spike reactivation hit (see
// TraceAbilityInputRelay.h), and Mace solved it with a branch in the relay's router — a file this
// pass does not own.
//
// So SNAP solves it from inside the character, with the framework's own tools:
//
//   press 1   places gate A, and GetActivatedCooldownSeconds() returns 0, so E is immediately live
//             again and the second press reaches ActivateAbility() normally. SnapReadyMatchTime
//             (AuxEndMatchTime, replicated) is set to now + 4 s: the WINDOW.
//   press 2   inside the window places gate B, pairs them, and GetActivatedCooldownSeconds() returns
//             the REMAINDER of 35 s measured from press 1 — so the ring counts 35 s down from the
//             start of the cast, not from the end of it.
//   no press  the window lapses, gate A expires, and AuxEndMatchTime is rewritten to press 1 + 35 s.
//             CanActivate() refuses until then, so a fluffed cast still costs the full cooldown and
//             Elle cannot farm free decoy gates every 4 s.
//
// *** THE KNOWN INACCURACY, AND ITS FIX. *** In that last branch the HUD's cooldown ring used to read
// READY while CanActivate() was refusing, because the ring is drawn from the FRAMEWORK's cooldown and
// the framework's cooldown really is zero. The player saw "ready" and E did nothing for up to 31 s.
// The v18 §2 integration pass closed it: UTraceCharacterAbilitySet::GetCharacterOwnedCooldownRemaining()
// is a seam for exactly this, GetCharacterOwnedCooldownRemaining() below returns the gap to
// SnapReadyMatchTime, and UTraceAbilityComponent::GetActivatedCooldownRemaining() takes the MAX of it
// and its own two deadlines. A character can therefore only ever make the ring more conservative,
// never make a genuinely-cooling ability look ready. It reads 0 while the 4 s window is open, because
// there the ability really is available and pressing E is the only thing the player should do.
//
// ===================================================================================================
// THE REPLICATED SCRATCH PAD — what Elle stores in which field
// ===================================================================================================
//
//   Flags & EffectActive   the CLOAK is up.
//   EffectEndMatchTime     when the cloak ends. Absolute match clock, like every timer in this
//                          framework; see the cooldown contract on UTraceAbilityComponent.
//   Flags & AuxActive      SNAP is mid-cast: gate A is down and the second-gate window is open.
//   Flags & Charged        a complete PAIR is live. Diagnostics and the HUD; the gates are the truth.
//   AuxEndMatchTime        DUAL PURPOSE, and deliberately so — there are only two float fields and
//                          Elle needs three timers. While AuxActive it is the END OF THE 4 s WINDOW;
//                          otherwise it is the time SNAP is next usable. One field, two meanings, and
//                          the flag says which — which is what lets a CLIENT answer CanActivate()
//                          correctly instead of mispredicting every press.
//   AuxLocation            gate A's mouth. Diagnostics only.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetElle.generated.h"

class ATraceCharacter;
class ATraceElleGate;

UCLASS()
class TRACE_API UTraceAbilitySetElle : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Elle; }

	// --- lifecycle ---------------------------------------------------------------------------------

	virtual void OnUnequipped() override;
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// --- SNAP --------------------------------------------------------------------------------------

	virtual bool CanActivate(FText& OutReason) const override;
	virtual bool ActivateAbility() override;

	/** SNAP's 35 s, or the remainder of it on the press that completes the pair. See the header. */
	virtual float GetActivatedCooldownSeconds() const override;

	// --- queries: the HUD, the bots and the verification harness -----------------------------------

	/** True while the cloak is up on THIS machine, read from the replicated state. */
	bool IsCloaked() const;

	/** Absolute match time the cloak ends. 0 when she is not cloaked. */
	float GetCloakEndMatchTime() const;

	/**
	 * True while THIS machine has the cosmetic half of the cloak pushed onto the pawn's body meshes.
	 *
	 * Separate from IsCloaked() on purpose, and the separation is the whole point of exposing it: the
	 * cloak is a replicated FACT plus a local PAINT, and "the fact replicated but nothing repainted"
	 * is a distinct and entirely plausible failure — it is what a dedicated server does deliberately,
	 * and what a missed race with ATraceCharacter::ApplyTeamColors() would look like accidentally.
	 * Always false on a dedicated server, which draws nothing.
	 */
	bool IsCloakVisualApplied() const { return bCloakVisualApplied; }

	/** True while gate A is down and the second-gate window is still open. */
	bool IsAwaitingSecondGate() const;

	/**
	 * Absolute match time SNAP is next usable, from Elle's OWN bookkeeping.
	 *
	 * NOT the same number as UTraceAbilityComponent::GetActivatedCooldownRemaining() in one branch —
	 * see the header block above. This is the one the ability actually enforces.
	 */
	float GetSnapReadyMatchTime() const;

	/** The live gates, or null. Server-side truth; a client sees the replicated actors instead. */
	ATraceElleGate* GetFirstGate() const;
	ATraceElleGate* GetSecondGate() const;

	/**
	 * PASSIVE 2. Elle's well-timed slide-jump PLANAR multiplier, DERIVED from the shipped global one.
	 *
	 * @param GlobalWellTimedBonus  whatever UTraceCharacterMovementComponent::
	 *                              GetSlideJumpWindowSpeedBonus() answers for everybody else (1.446875
	 *                              today). Passed in rather than read here so that this function has
	 *                              exactly one definition of "the base" and it is the shipped one.
	 *
	 * Returns 1 + (GlobalWellTimedBonus - 1) x (1 + ElleSlideJumpGainBonus): the GAIN is scaled, the
	 * multiplier is not. At today's numbers, 1 + 0.446875 x 1.40 = 1.625.
	 *
	 * Static, and null-safe, because the seam that will eventually call it lives in the movement
	 * component and must be able to ask without knowing whether an ability set exists at all — the
	 * same shape as UTraceAbilityComponent::GetDashHitSweepRadiusFor.
	 */
	static float GetSlideJumpWindowSpeedBonusForElle(const AActor* Actor, float GlobalWellTimedBonus);

	/**
	 * Seconds SNAP itself will refuse for, which the HUD's cooldown ring folds in through
	 * UTraceAbilityComponent::GetActivatedCooldownRemaining().
	 *
	 * Zero while the 4 s second-gate window is open (the ability genuinely IS available then), and
	 * otherwise the gap to GetSnapReadyMatchTime(). It exists because press 1 charges no framework
	 * cooldown, so after a fluffed cast the framework reads READY while CanActivate() refuses.
	 */
	virtual float GetCharacterOwnedCooldownRemaining() const override;

	/**
	 * The ability base class's slide-jump seam, which is what the movement component actually calls
	 * (through UTraceAbilityComponent::GetSlideJumpWindowSpeedBonusFor, so Movement/ never learns
	 * Elle's name). Forwards to the static above so the arithmetic has one definition.
	 */
	virtual float ModifySlideJumpWindowSpeedBonus(float InWellTimedBonus) const override;

#if !UE_BUILD_SHIPPING
	/** DEV ONLY. Puts the cloak up for @p Seconds without a pass, so the harness can stage it. */
	void DebugStartCloak(float Seconds);

	/**
	 * DEV ONLY, AUTHORITY ONLY. Places a COMPLETE, PAIRED pair at two chosen points, through the
	 * shipping placement and pairing path.
	 *
	 * Exists for one caller: Trace.Elle.CarrierTest. The carrier rule can only be exercised by putting
	 * a carrier inside a gate mouth, and a mouth appears "where she stands" — so without this the
	 * harness would have to walk Elle to the victim, which is not a thing a headless test can do
	 * reliably. It calls PlaceGate() and ATraceElleGate::PairWith(), i.e. the real functions, so what
	 * is tested is the shipped gate and not a fixture's imitation of one.
	 *
	 * @return false when either spawn was refused.
	 */
	bool DebugPlaceGatePair(const FVector& MouthA, const FVector& MouthB);
#endif

private:
	/** SERVER. Starts (or refreshes) the cloak and replicates it. */
	void StartCloak(const TCHAR* Why);

	/** SERVER. Takes the cloak down early. */
	void EndCloak(const TCHAR* Why);

	/** SERVER. The falling edge of "Elle holds the Core", qualified into pass / throw / lost. */
	void TickCloakTrigger();

	/** EVERY MACHINE. Pushes the cosmetic half of the cloak onto (or off) the pawn's body meshes. */
	void ApplyCloakVisual(bool bCloakOn);

	/** SERVER. Destroys both gates and clears the pair bookkeeping. */
	void DestroyGates();

	/** SERVER. Places one gate at @p Mouth. Returns null when the world refuses to spawn it. */
	ATraceElleGate* PlaceGate(const FVector& Mouth, float ExpireMatchTime, bool bSecondOfPair);

	/** The two mouths. Weak, because a gate destroys itself at its own deadline. */
	TWeakObjectPtr<ATraceElleGate> FirstGate;
	TWeakObjectPtr<ATraceElleGate> SecondGate;

	/**
	 * SERVER ONLY. Whether Elle held the Core on the previous ability tick.
	 *
	 * Not replicated and not saved: it is a one-tick edge detector, and the thing it produces (the
	 * cloak) is what replicates. The tick runs at 20 Hz, so the cloak can start up to 50 ms after the
	 * pass completes — stated rather than hidden, because "right after" is the requirement and 50 ms
	 * is what "right after" costs without a delegate on ATraceCore that this pass may not add.
	 */
	bool bHeldCoreLastTick = false;

	/**
	 * OWNING CLIENT ONLY. Absolute match time this machine's PREDICTED second-gate window ends.
	 *
	 * Zero on the server, where FTraceAbilityNetState is the truth. It exists because the replicated
	 * AuxActive flag is one round trip behind the press that set it, and a remote client that thought
	 * the cast was already over would grey its own cooldown ring — putting the second gate out of
	 * reach of everybody who is not the listen-server host.
	 */
	float LocalFirstGatePredictionEnd = 0.f;

	/** EVERY MACHINE. Whether the cosmetic half is currently pushed onto the pawn. */
	bool bCloakVisualApplied = false;

	/** The pawn the visual was pushed onto, so a respawn cannot leave a corpse's materials dimmed. */
	TWeakObjectPtr<ATraceCharacter> CloakVisualPawn;
};
