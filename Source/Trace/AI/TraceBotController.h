// Trace — the singleplayer bot brain.
//
// WHY THIS EXISTS
// A 5v5 prototype is untestable by one person. ATraceBotController fills the other nine slots so
// a solo player gets a real match: 1 human + 4 bots vs 5 bots. Nine of the ten players on the field
// are bots, which makes this class the primary *testbed* for every rule in the game: a mechanic no
// bot ever performs is a mechanic that has never been played.
//
// WHY IT IS A HAND-WRITTEN STATE MACHINE AND NOT A BEHAVIOUR TREE
// Build contract §2: nothing in this project may depend on a .uasset we author. A Behaviour Tree
// and a Blackboard are both assets, and a runtime-generated navmesh is fragile (it has to be built
// after ATraceArenaBuilder runs, which is inside PreInitializeComponents). Direct steering with
// AddMovementInput plus a wall-repulsion field from ATraceArenaBuilder::GetFieldBounds() is not a
// compromise here, it is the correct amount of machinery.
//
// --- MECHANICS SPEC v2: WHAT CHANGED IN THIS CLASS -------------------------------------------
//
// 1. THE CORE IS A STATUS, NOT AN OBJECT. There is nothing loose on the ground and nothing to run
//    over, so ETraceBotState::ChaseCore is gone. The carrier is discovered by scanning the live
//    roster for ATraceCharacter::IsCarrier() rather than by asking ATraceCore who it is holding —
//    which also means this class no longer depends on the Core actor at all, and keeps working if
//    the Core degenerates to a purely cosmetic attachment. The three ways to take it are: break the
//    carrier's trace with a dash, kill the carrier while their shield is down, or be passed to.
//    Every one of those is now a bot behaviour.
//
// 2. HOVER PASSING IS A HELD, CANCELLABLE ACTION AND IT IS DANGEROUS. Starting a pass drops the
//    carrier's shield. So ConsiderPass() is no longer "throw at the best receiver": it is a small
//    state machine (ETraceBotPassPhase) that lines the aim up on a teammate, decides whether the
//    moment is SAFE ENOUGH to be shootable for half a second, holds the input while staying on
//    them, and bails out if the situation turns. See BehaviourPass().
//
// 3. DEFENDERS PUNISH PASSERS. ETraceBotState::PunishPasser puts a defender on the enemy carrier
//    with the trigger ready, betting on the shield dropping. That bet is the other half of the
//    risk/reward loop in (2) — without someone waiting for it, passing would be free.
//
// 4. THE MOVEMENT KIT IS USED ON PURPOSE. Dash charges (two while carrying, and the second is held
//    in reserve), slide, crouch fast-fall and the unstick jump each have a specific job, listed on
//    UpdateMovementTech(). None of them fire randomly.
//
// 5. SIDES SWITCH AT HALF TIME. GetAttackGoalLocation() no longer derives the attacking end from
//    "Blue attacks +X". It asks the ATraceEndzone actors which one this bot's team SCORES IN, and
//    re-asks once a second, so a mid-match side switch is picked up automatically however the game
//    mode chooses to implement it. A bot running at the wrong endzone in the second half is the
//    single most likely bug in this pass, so the resolution is also logged at Display every time it
//    flips, and a bot that cannot resolve an endzone says so instead of guessing.
//
// THE SIGNATURE MECHANIC
// A shielded carrier is killed by an ENEMY DASHING THROUGH THEIR TRACE. ETraceBotState::HuntCarrier
// does not chase the carrier — it computes the PERPENDICULAR crossing of the trace and dashes
// across it. If that state ever stops working the bots become harmless and the whole game reads as
// broken. It was, measurably, barely working once (1.3% of deaths) before the crossing maths below
// replaced "steer at the nearest point"; the last measured baseline was 37.5% of kills.
//
// NOTHING IN HERE IS TUNED IN CODE
// Every number a designer would want to move lives in UTraceSettings — the per-difficulty
// FTraceBotProfile for skill, and a set of field-relative fractions for positioning. Distances
// that describe the PITCH are fractions of ATraceArenaBuilder::GetFieldBounds(), because the arena
// is not a fixed size. Distances that describe the CHARACTER (dash reach, trace crossing band) stay
// absolute.
//
// AUTHORITY
// Every bot decision runs on the server only. AAIController never exists on a client, but the
// HasAuthority() guards are kept on the entry points anyway so a listen-server mistake elsewhere
// fails quietly instead of desyncing.

#pragma once

#include "AIController.h"
#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Math/Box.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "AI/TraceBotAbilityBrain.h"    // spec v15 §3: the ability half of the brain, held by value
#include "TraceTypes.h"                 // ETraceTeam

#include "TraceBotController.generated.h"

class ATraceCharacter;
class ATraceEndzone;
class ATraceGameMode;
struct FTraceBotProfile;

/**
 * What a bot has decided to do this instant. Evaluated top-down in DecideState(), so the order of
 * the enumerators is also the priority order.
 */
UENUM()
enum class ETraceBotState : uint8
{
	/** No pawn, dead, or no idea — hold still. */
	Idle = 0,

	/** I have the Core. Run it into the enemy endzone; dash out of trouble; look for the pass. */
	CarryToGoal,

	/**
	 * An ENEMY has the Core and I am one of the assigned interceptors. Cross their trace and dash
	 * through it. Under spec v2 this is the only *unilateral* way to take the Core off a carrier —
	 * every other route needs them to expose themselves first.
	 */
	HuntCarrier,

	/**
	 * An ENEMY has the Core and I am one of the assigned punishers. Hold a bead on them from
	 * shooting range. Bullets do nothing while their shield is up, but the instant they begin a pass
	 * the shield drops and the shot lands — and the killer takes the Core. This is what makes their
	 * pass a real decision instead of a free reset.
	 */
	PunishPasser,

	/** A TEAMMATE has the Core. Screen for them, or break deep and stay visible as a receiver. */
	EscortCarrier,

	/**
	 * Nobody is carrying (the carrier died and the grant has not landed yet, or the match has not
	 * started). Push to a staging position up the field so the team is not caught flat when it does.
	 */
	Regroup,

	/** Nothing objective-shaped is available to me. Fight the nearest enemy I can see. */
	Fight,

	// --- MODE B ONLY (spec v4 §7). Unreachable while the match is playing mode A. -----------------

	/**
	 * The Core is LOOSE — thrown, in flight, or lying on the ground — and I am one of the players
	 * going for it. "The first player to contact the core should pick it up", enemy or teammate, so
	 * this is the state in which mode B's central act, the interception, actually happens.
	 *
	 * Deliberately ranked above the carrier states in this enum: while the Core is loose there IS no
	 * carrier, so nothing below can fire anyway, and a loose Core is the most urgent thing on the
	 * field for both sides at once.
	 */
	ChaseLooseCore,

	/**
	 * MODE B. Somebody else is going to reach the Core first, or an enemy already has it and I am not
	 * one of the players pressuring them. Sit between the threat and my own goal.
	 *
	 * This role does not exist in mode A and would be wrong there: a mode-A endzone spans the full
	 * width of the field, so there is no mouth to stand in front of and "defending" it means
	 * intercepting the carrier, which is what HuntCarrier already does. A mode-B goal is a third of
	 * the width, which makes standing in front of it a real and coverable job.
	 */
	DefendGoal
};

/**
 * Where a carrying bot is in the hover-pass sequence.
 *
 * The pass is a HELD input with a 0.5 s dwell, not a button press, and starting it drops the
 * carrier's shield — so it has to be modelled as a commitment with an abort path, not as a call.
 */
UENUM()
enum class ETraceBotPassPhase : uint8
{
	/** Not passing. Free to start one when a receiver and a safe moment coincide. */
	None = 0,

	/**
	 * A receiver is chosen and the aim is slewing onto them. The input is NOT down yet, so the
	 * shield is still up and this phase costs nothing. Abandoned if the line-up takes too long.
	 */
	Lining,

	/** Input down, shield down, holding the crosshair on the receiver until the dwell completes. */
	Holding,

	/**
	 * MODE B ONLY, spec v13 §6. Input down, WINDING A THROW UP.
	 *
	 * A separate phase from Holding rather than a reuse of it, and the reason is that they are
	 * opposite trades. Holding is mode A's hover pass: the shield is DOWN and every frame of the dwell
	 * is a frame of risk, so it is abortable and is aborted often. Charging is mode B's throw: the
	 * shield is untouched, nothing is at risk, and the only thing the hold buys is momentum — so it is
	 * a COMMITMENT and is never aborted, because releasing the button IS the throw. Aborting a charge
	 * by releasing would launch the Core, which is the opposite of what any caller asking to abort
	 * wants. IsPassing() deliberately still means Holding alone: it is asked "is this bot's shield
	 * down", and during a charge it is not.
	 */
	Charging,

	/** Just finished or aborted; the rules impose a cooldown before another attempt. */
	Cooldown
};

/**
 * Drives one ATraceCharacter with no behaviour tree, no blackboard and no navmesh.
 *
 * Bots possess exactly the same pawn class as humans, so every rule — the shield, the trace, dash
 * charges, positional damage, no friendly fire, respawn timing — applies to them unchanged. Nothing
 * in this class writes gameplay state directly; it only calls the same input entry points a human's
 * key press would, through the small adapter in TraceBotController.cpp.
 */
UCLASS()
class TRACE_API ATraceBotController : public AAIController
{
	GENERATED_BODY()

public:
	/**
	 * The FObjectInitializer overload is required by AAIController's own constructor signature.
	 * Note what it does NOT do: it does not remove the default UPathFollowingComponent. AAIController
	 * binds a delegate to that component unguarded, so suppressing the subobject crashes on spawn.
	 * It is inert without a navmesh, which is exactly the state we leave it in.
	 */
	ATraceBotController(const FObjectInitializer& OI);

	//~ Begin AController / AAIController interface
	virtual void BeginPlay() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void Tick(float DeltaSeconds) override;

	/**
	 * Deliberately empty. AAIController::Tick calls this every frame and the base implementation
	 * would (a) force Pitch to zero whenever the focus actor is not a pawn and (b) drive
	 * APawn::FaceRotation. This class owns the control rotation outright — it IS the bot's aim, via
	 * ATraceCharacter::GetAimDirection — so the base behaviour has to be switched off, not merely
	 * overwritten afterwards: overwriting leaves the pitch reset to fight our interpolation every
	 * frame, and a bot that cannot hold a pitch offset never satisfies its own fire-cone test.
	 */
	virtual void UpdateControlRotation(float DeltaTime, bool bUpdatePawn = true) override {}
	//~ End interface

	/** Server. Sets the PlayerState name shown on the scoreboard, e.g. "BOT Blue 3". */
	void SetBotDisplayName(const FString& InName);

	/** For the GameMode's roster diagnostics. */
	ETraceBotState GetBotState() const { return State; }

	/** Human-readable state name, for logs only. */
	static const TCHAR* StateToString(ETraceBotState InState);
	static const TCHAR* PassPhaseToString(ETraceBotPassPhase InPhase);

	/**
	 * Only static geometry blocks this: the Pawn collision profile ignores ECC_Visibility.
	 *
	 * PUBLIC for FTraceBotAbilityBrain (spec v15 §3). The ability brain has to answer "can I see the
	 * player I am about to Sting" and there must be exactly ONE answer to that in the bot layer — a
	 * second copy in the brain would be free to drift, and "the bots shoot through a wall" and "the
	 * bots throw jars through a wall" would then be two separate bugs with two separate fixes.
	 */
	bool HasLineOfSight(const AActor* Target) const;

	/**
	 * SPEC v28 §6. How far a LOCKED-OUT bot keeps clear of a turned-over Core, in uu.
	 *
	 * PUBLIC because Trace.Bots.LockoutTest judges the before-and-after against it, and a harness
	 * that re-typed the number would be measuring itself rather than the game. Relative to
	 * UTraceSettings::CorePickupRadius — see the definition.
	 */
	static float GetLockoutKeepOutRadius();

protected:
	// ------------------------------------------------------------------------------------------
	// Per-tick pipeline
	// ------------------------------------------------------------------------------------------

	/** Re-reads the world into the cached fields below. Cheap; runs every tick. */
	void GatherWorldState();

	/** Picks State from the gathered world state. Runs on the decision cadence, not every tick. */
	void DecideState();

	/** Produces DesiredMoveDirection (and may raise a dash) for the current State. */
	void UpdateMovementIntent(float DeltaSeconds);

	/** Slide, crouch fast-fall and the unstick jump. Runs every tick, after the behaviours. */
	void UpdateMovementTech(float DeltaSeconds);

	/**
	 * SPEC v15 §3. Asks FTraceBotAbilityBrain what this bot wants to do with E / V / jump / dash, and
	 * turns the answer into the same latches the rest of this class uses.
	 *
	 * ORDERING, and it is not arbitrary. This runs AFTER the behaviours (which choose ShootTarget and
	 * DesiredMoveDirection — both are inputs to an ability decision) and BEFORE UpdateCombat (which
	 * writes the control rotation, and therefore has to know when an ability owns the crosshair). The
	 * actual key PRESS is deferred to ApplySteering, because Rocco's Ripple composes its direction
	 * from the pawn's acceleration exactly as the dash does: pressed before AddMovementInput it would
	 * fire along last frame's steering.
	 */
	void UpdateAbilities(float DeltaSeconds);

	/**
	 * SPEC v15 §3. Presses the keys the brain asked for, through the same entry points a human's key
	 * press reaches. Called from the tail of ApplySteering — see UpdateAbilities for why.
	 */
	void ApplyAbilityInputs();

	/**
	 * SPEC v15 §3, and it is a BACKSTOP — §2 owns bot character selection.
	 *
	 * ATraceGameMode::PollCharacterSelect fills every bot at 4 Hz with a random free character, once
	 * every human on its team has settled, and it wins essentially every race with this 0.5 Hz poll.
	 * This runs only for a bot still holding ETraceCharacterId::None, and asks for a RANDOM free one
	 * so that whichever path gets there first obeys the same sentence of the spec.
	 *
	 * It respects the ordering the same way §2 does: it defers while any team-mate's select screen is
	 * open, and ServerSetCharacter enforces the rest. Behind Trace.Bot.AutoCharacter, whose 0 setting
	 * is the red arm for "is this behaviour really coming from the character".
	 */
	void UpdateAutoCharacter();

	/** Aim, reaction delay, aim error, burst discipline and the trigger. Suppressed while carrying. */
	void UpdateCombat(float DeltaSeconds);

	/**
	 * RESTRUCTURE tranche D5 — THE KNIFE BAND. Spec v10 §1: "Bots must use it, or it will not be
	 * playtested." Swap to the knife inside TraceMelee::GetBotEngageRangeUU() of the nearest living
	 * non-carrier enemy, back to the gun outside GetBotDisengageRangeUU(), and swing whenever one is
	 * in reach. Under the v28 §10 dual-wield switch the swap half collapses and only the swing
	 * remains — the blade is already in the off hand, so there is nothing to trade.
	 *
	 * THIS IS UTraceWeaponComponent::TickBotKnife(), MOVED. Its own declaration had said for two
	 * passes that it "belongs in ATraceBotController's state machine ... but that file is another
	 * ownership slice this pass". The rule is unchanged; what changed is that "is this a bot?" is
	 * now answered by the type of `this` instead of by rejecting APlayerControllers, and that the
	 * practice range's targets — possessed by ATracePracticeDummyController, which is not this
	 * class — can no longer reach it at all. The per-pawn autonomy bit stays on the weapon
	 * component (it is a property of the BODY and travels with it) and is still consulted.
	 *
	 * Called from Tick immediately BEFORE UpdateCombat, so its decision is in the hands before
	 * UpdateWeaponMinimum below defers to it.
	 */
	void UpdateKnifeBand(ATraceCharacter* BotCharacter, float Now);

	/**
	 * RESTRUCTURE tranche E (E3) — THE SMG MINIMUM. Spec v28 §9 shipped a second firearm and the
	 * bots never once drew it, which fails the standing bar ("a mechanic no bot ever performs is a
	 * mechanic that has never been played"). The rule is the SMG's own falloff cliff read as a
	 * band: inside UTraceSettings::SmgFalloffStartUU the SMG pays its full 33/18/12 and is the
	 * right tool; past the cliff plus a hysteresis margin the pistol is. Mirrors UpdateKnifeBand's
	 * engage/disengage shape, and DEFERS to it: whenever the knife rule is holding the blade out,
	 * this asks for nothing at all.
	 *
	 * Called from UpdateCombat once the live target is resolved, so a bot whose crosshair belongs
	 * to a pass, a pull or an aimed ability never swaps weapons mid-commitment.
	 */
	void UpdateWeaponMinimum(ATraceCharacter* BotCharacter, const ATraceCharacter* CurrentTarget, float Now);

	/**
	 * RESTRUCTURE tranche E (E4) — THE PARRY MINIMUM. Carrier-only, Normal/Hard only (Easy stays
	 * parry-less — difficulty legibility): when an enemy is DASHING AT this carrier from inside
	 * detection range, press parry once, through TraceParry::RequestParry — the same entry point a
	 * human's bind reaches — after the profile's reaction delay. A refused ask (cooldown) is
	 * dropped, not retried same-tick.
	 */
	void UpdateParryMinimum(float Now);

	/** Applies wall repulsion + stuck evasion to DesiredMoveDirection and feeds AddMovementInput. */
	void ApplySteering(float DeltaSeconds);

	// ------------------------------------------------------------------------------------------
	// Behaviours
	// ------------------------------------------------------------------------------------------

	void BehaviourCarryToGoal(float DeltaSeconds);
	void BehaviourHuntCarrier(float DeltaSeconds);
	void BehaviourPunishPasser(float DeltaSeconds);
	void BehaviourEscortCarrier(float DeltaSeconds);
	void BehaviourRegroup(float DeltaSeconds);
	void BehaviourFight(float DeltaSeconds);

	/** MODE B. Run at the loose Core, led by its own velocity so the bot meets it rather than trails it. */
	void BehaviourChaseLooseCore(float DeltaSeconds);

	/** MODE B. Hold a station between the current threat and the goal this bot's team defends. */
	void BehaviourDefendGoal(float DeltaSeconds);

	// ------------------------------------------------------------------------------------------
	// SPEC v28 §6 — THE TURNOVER LOCKOUT. "The bots just stand on top of a locked out core."
	//
	// During the 5 s window a turnover opens (ATraceCore::IsTurnoverActive) the Core is loose but
	// only ONE side may have it: the team that dropped it is locked out of the pickup entirely, and
	// the other side may either walk onto it or PULL it from range. Before this pass the bots knew
	// none of that — bCoreLoose was the whole of their model — so a locked-out bot ran the ordinary
	// chase, arrived, found ServerTryLoosePickup refusing it, and stood on the ball for five
	// seconds because "run at LooseCorePoint" is satisfied the instant you are on top of it.
	//
	// Two behaviours, and they are opposite sides of the same fact:
	//   * LOCKED OUT — do not contest something you cannot have. Back off out of the keep-out
	//     radius, defend the mouth (or fight), and be goal-side when the window expires.
	//   * MAY PULL   — go for the pull. Aim at the Core and hold the pull input, which is the input
	//     a human's right mouse produces, so the bot races through the same CanPullNow /
	//     ServerTickTurnover rules a player does.
	// ------------------------------------------------------------------------------------------

	/**
	 * Server. Holds or releases this bot's Core-pull input, mirroring bPullInputHeld so the edge is
	 * only ever sent once — the same shape as ApplyPassInput / ApplySecondaryInput.
	 *
	 * Routed through ATraceCore::RequestPullInput, i.e. the entry point §7's right mouse reaches on
	 * a human. Nothing here writes pull state directly: the hold, its 0.3 s clock, the aim cone, the
	 * line of sight and the race between two pullers all stay the server's.
	 */
	void ApplyPullInput(bool bHeld);

	/**
	 * SPEC v28 §6. THE LOCKED-OUT TEAM'S RETREAT, applied after the behaviours have steered.
	 *
	 * One place rather than a branch in each behaviour: whichever state a locked-out bot ends up in
	 * (DefendGoal, Fight, Regroup, EscortCarrier...), the rule is the same sentence — do not be
	 * stood on a ball you are not allowed to touch — and a rule spread across five behaviours is a
	 * rule that will be missing from the sixth.
	 *
	 * Inside the keep-out radius it REPLACES the steering with a push directly away from the Core;
	 * outside it, it does nothing at all.
	 */
	void ApplyLockoutKeepOut();

	/**
	 * MODE B. True when this bot is one of the players whose job is the goal mouth right now.
	 *
	 * Ranked by distance to OUR OWN goal among living teammates, capped at
	 * TraceBotConstants::GoalDefenders. Always false in mode A, where there is no mouth to guard.
	 */
	bool ShouldHoldGoalDefence() const;

	// ------------------------------------------------------------------------------------------
	// MODE B: throwing  (spec v4 §7)
	//
	// The mode-B counterpart of UpdatePass, and deliberately a SEPARATE state machine rather than a
	// branch inside it. A hover pass is a half-second commitment with a shield cost and an abort
	// path; a throw is instantaneous, costs nothing, and cannot be aborted once released. Folding
	// the two together would mean every line of the pass machine growing a mode test, and the risk
	// beat that machine exists to implement does not apply to a throw at all.
	// ------------------------------------------------------------------------------------------

	/**
	 * Carrier only, mode B. Decides whether to throw, at what, lines the aim up and releases.
	 *
	 * Two targets are considered, in this order: the GOAL when the bot is close enough and has a
	 * clear line to it (that is how mode B scores at range), and an OPEN TEAMMATE further up the
	 * field otherwise. A carrier under pressure with neither available throws the Core forward
	 * anyway rather than dying with it — a loose Core its own team can contest is worth more than a
	 * turnover.
	 */
	void UpdateThrow(float DeltaSeconds);

	/**
	 * Ballistic aim point for a throw at @p WorldTarget: the target raised by the amount the Core
	 * will drop over its flight, so the bot lobs rather than firing flat into the floor.
	 *
	 * Uses the Core's own throw speed and gravity scale, so a designer retuning either does not
	 * silently make every bot throw short.
	 */
	FVector ComputeThrowAimPoint(const FVector& WorldTarget) const;

	/**
	 * Solves the launch DIRECTION that puts a thrown Core through @p WorldTarget, or fails if the
	 * target is out of ballistic range.
	 *
	 * Replaces a first-order "raise the aim point by the drop" approximation that was fine for a fast
	 * flat Core and is not fine for a heavy one: with the v5 weight model the Core leaves 25% slower
	 * under twice the gravity, so the drop over a long throw is larger than the throw is long and the
	 * approximation has no fixed point at all. This is the real thing — the Core's own launch
	 * (direction x speed, plus a world-up bias term) integrated under the Core's own gravity, solved
	 * for pitch by bisection because the up-bias term makes the closed form a quartic.
	 *
	 * Returns the LOW arc when both exist: a flat throw spends less time in the air, which is less
	 * time for a defender to walk into it.
	 *
	 * SPEC v13 §6: solved at @p ChargeScale, because the throw is now only as fast as the charge that
	 * releases it. A solver that assumed full power and then released at 40% would aim short of
	 * everything by the difference, every time.
	 *
	 * @param OutLaunchDirection unit vector to aim along, valid only on true.
	 * @param OutFlightSeconds   time of flight, for the caller's lane sweep. Optional.
	 */
	bool SolveThrowLaunch(const FVector& From, const FVector& WorldTarget,
		FVector& OutLaunchDirection, float* OutFlightSeconds = nullptr, float ChargeScale = 1.f) const;

	/**
	 * MODE B, SPEC v13 §6. How hard should this throw be released?
	 *
	 * "Have them charge appropriately for distance." A bot that always charges fully is not wrong -
	 * every throw would simply be the pre-v13 throw - but it wastes the mechanic and makes every short
	 * pass a rocket. A bot that always taps throws at 15% and mode B looks broken. So the charge is
	 * derived from the range the throw actually has to cover:
	 *
	 *   a throw's flat range goes as the SQUARE of its launch speed, so the charge needed to cover a
	 *   distance D out of a full-power reach R is sqrt(D/R) - plus a margin, because arriving exactly
	 *   at the limit of the arc means arriving on the floor, and since spec v6 §4.2 a throw that lands
	 *   is the enemy's.
	 *
	 * Never returns less than a floor: a bot has no reason to fumble the Core at its own feet, and the
	 * floor is what stops a very short pass from becoming one.
	 */
	float ChooseThrowCharge(const FVector& From, const FVector& WorldTarget) const;

	/**
	 * MODE B. Furthest horizontal distance a throw from @p From can reach a point @p HeightDelta
	 * above it, in uu.
	 *
	 * THE NUMBER THE OLD SHOT TEST GOT WRONG. It used HalfFieldLength() * 0.55 — 9240 uu on this
	 * pitch — as "close enough to shoot", which was never a fact about the Core: the light Core
	 * carried ~5000 uu on a flat throw and the heavy one carries ~3400. A bot inside the old range
	 * but outside the real one throws the Core into the floor in front of a defender. Answered from
	 * the Core's published flight model so it retunes with it.
	 */
	float MaxThrowRange(const FVector& From, float HeightDelta, float ChargeScale = 1.f) const;

	/**
	 * MODE B. Is the arc from @p From to @p WorldTarget clear of arena geometry?
	 *
	 * "Teach them to shoot at the goal when they have a lane" is a claim about the whole trajectory,
	 * not about a straight line: a lobbed Core clears cover a ray would call blocking, and drops into
	 * a crossbar a ray would call clear. So the parabola is sampled and the segments between samples
	 * are traced, against ECC_Visibility for the same reason HasLineOfSight uses it — arena geometry
	 * blocks, pawns do not (a defender standing in the lane is a contest, not a reason not to shoot).
	 */
	bool HasThrowLane(const FVector& From, const FVector& WorldTarget, const FVector& LaunchDirection,
		float FlightSeconds, float ChargeScale = 1.f) const;

	/**
	 * MODE B. Should this carrier stop throwing and run the Core into the goal itself?
	 *
	 * Sets/clears bCommitCarryIn. See that member for why it is a latch.
	 */
	void UpdateCarryInCommit();

	/** Ends a throw attempt and starts the same cooldown a pass attempt would. */
	void AbortThrow(const TCHAR* Reason);

	// ------------------------------------------------------------------------------------------
	// Hover passing  (spec §4)
	// ------------------------------------------------------------------------------------------

	/**
	 * Carrier only. Runs the whole pass state machine: choose a receiver, decide whether it is safe
	 * enough to drop the shield for half a second, line the aim up, hold, and abort if it turns.
	 *
	 * Called every tick while carrying (not on the decision cadence) — a 0.5 s dwell with a 2 s
	 * cooldown cannot be steered from a 0.3 s decision tick without losing whole attempts.
	 */
	void UpdatePass(float DeltaSeconds);

	/** Ends the current attempt: releases the input, restores the shield, starts the cooldown. */
	void AbortPass(const TCHAR* Reason);

	/**
	 * The teammate this carrier should throw to, or null.
	 *
	 * Requires line of sight (the rule needs the crosshair genuinely on them), a minimum and maximum
	 * range, and either a forward advantage or — under pressure — merely being alive and open.
	 */
	ATraceCharacter* ChooseReceiver(float& OutAdvantage) const;

	/**
	 * How exposed this bot is right now, as the number of enemies that could shoot it this instant
	 * (alive, within the punish radius, with line of sight).
	 *
	 * This is the input to the risk half of the risk/reward loop: it is the count of guns that get a
	 * free half-second at an unshielded carrier if a pass is started now.
	 */
	int32 CountEnemiesCoveringMe() const;

	/** True while this bot is holding the pass input, i.e. shield down and vulnerable. */
	bool IsPassing() const { return PassPhase == ETraceBotPassPhase::Holding; }

	// ------------------------------------------------------------------------------------------
	// Queries
	// ------------------------------------------------------------------------------------------

	ATraceCharacter* GetBotCharacter() const;
	ATraceGameMode* GetTraceGameMode() const;

	/**
	 * Deep inside the endzone this bot is attacking, on this bot's own lane across the width.
	 *
	 * Resolved from the live ATraceEndzone actors (see ResolveAttackEndzone) so it stays correct
	 * across the half-time side switch. Falls back to the bounds-derived "Blue attacks +X" rule only
	 * if no endzone can be found at all, and complains when it does.
	 */
	FVector GetAttackGoalLocation() const;

	/**
	 * Re-asks the world which endzone this bot's team scores in.
	 *
	 * Cheap (two actors) and run once a second rather than per tick. Logs at Display whenever the
	 * answer changes, which is the evidence that the second half is being played the right way
	 * round.
	 */
	void ResolveAttackEndzone();

	/**
	 * Half the arena's X extent, in uu, from ATraceArenaBuilder::GetFieldBounds().
	 *
	 * Every positioning distance in this class is a fraction of this (or of HalfFieldWidth), which
	 * is what lets the same tuning work on an 8000-long pitch and a 24000-long one.
	 */
	float HalfFieldLength() const;
	float HalfFieldWidth() const;

	/**
	 * Nearest living enemy inside the profile's engagement range with line of sight, or null.
	 *
	 * Skips a SHIELDED carrier, because bullets do nothing to them — but deliberately does NOT skip
	 * a carrier whose shield is down (mid-pass). Taking that shot is how the Core changes hands.
	 */
	ATraceCharacter* FindBestShootTarget() const;

	/**
	 * Where on @p Target this bot aims, in world space: head, body or leg.
	 *
	 * Positional damage is 100 / 40 / 25, so the head is an instant kill and the choice of zone is
	 * most of a bot's lethality. FTraceBotProfile::HeadshotAimFraction is the dial, and it is ZERO on
	 * Easy on purpose — Easy is kept beatable by not turning nine bots into headshot machines, not by
	 * making them miss more.
	 */
	FVector GetAimPointOn(const ATraceCharacter* Target) const;

	/**
	 * The point on the enemy carrier's trace this bot should cross, plus the trace's local
	 * direction there.
	 *
	 * Two filters that between them are most of the reason the signature kill used to never land:
	 *   * the newest TrailHeadGracePoints entries are skipped, because the server's trip test
	 *     exempts them — dashing at those burns a charge for nothing;
	 *   * points with less than BotTrailMinPointLifeRemaining seconds left are skipped, because a
	 *     bot that commits to a point which expires before it arrives spends the entire carry
	 *     running at ghosts.
	 *
	 * OutTangent is what makes the crossing work: the caller aims PAST the trace along the
	 * perpendicular rather than AT a point on it, so the dash sweeps through instead of pulling up
	 * alongside.
	 *
	 * --- SPEC v9 §1: WHY THIS WAS REWRITTEN ---------------------------------------------------
	 *
	 * Both of the filters described above were ABSOLUTE distances calibrated against a trace whose
	 * length was TrailLifetime x WalkSpeed. Spec v7 §2 then capped a trace at TrailMaxLengthUU =
	 * 1200 uu. The head filter (BotTrailMinDistanceFromCarrier, 900 uu) and the tail filter
	 * (BotTrailMinPointLifeRemaining x WalkSpeed, 320 uu) sum to 1220 uu, so on a 1200 uu trace
	 * they overlap and NO point can satisfy both — for any carrier, at any moment, on every
	 * difficulty. This function returned false unconditionally, and BehaviourHuntCarrier's fallback
	 * (steer at a lead point off the carrier's velocity) is a pursuit, which is what the report
	 * describes: bots mirroring the carrier's turns and standing next to them.
	 *
	 * It now:
	 *   * takes the lethal set from UTraceTrailComponent::ComputeLastLethalIndex(), the same
	 *     function the server's trip test and the renderer use, instead of re-deriving it from
	 *     TrailHeadGracePoints and getting a different answer;
	 *   * measures both margins as a FRACTION OF THE TRACE THAT EXISTS (capped by the settings
	 *     values), so they can never again meet in the middle;
	 *   * measures "behind the carrier" along the PATH rather than as a straight line, which is
	 *     what makes a hairpin huntable — trace 900 uu back along a U-turn is 200 uu away in space,
	 *     it is fully lethal, and the old test threw it away;
	 *   * scores candidates by time-to-reach WEIGHTED BY CROSSING ANGLE rather than by raw
	 *     distance, because the nearest point is frequently the one you can only approach along;
	 *   * refuses points the length cap will delete before the bot can get there, which is the
	 *     "running at ghosts" failure expressed in the units a length-based trace has.
	 */
	bool FindTrailInterceptPoint(FVector& OutPoint, FVector& OutTangent) const;

	/**
	 * The pre-v9 version of the above, reachable only with `Trace.Bot.InterceptFix 0`.
	 *
	 * Kept verbatim so the before/after measurement is a property of one build rather than of two,
	 * and so the reproduction can be shown failing on demand. Nothing in the game calls it with the
	 * cvar at its default.
	 */
	bool FindTrailInterceptPointLegacy(FVector& OutPoint, FVector& OutTangent) const;

	/**
	 * Where a hunter goes when the enemy carrier has no trace worth crossing yet.
	 *
	 * NOT the carrier. Pathing at the carrier's current position is a pursuit: it mirrors their
	 * turns and converges on standing next to them, and it never crosses the trace because the
	 * trace is BEHIND them. This answers with a point on the line the trace is about to occupy —
	 * a fixed gap behind the carrier, offset to whichever side the bot is already on by roughly a
	 * dash's reach — so the bot arrives beside the line it will have to cross, facing across it.
	 *
	 * @param OutStation where to run, valid only on true.
	 */
	bool FindCutBehindStation(FVector& OutStation) const;

	/**
	 * This bot's rank by distance to the enemy carrier among living teammates, or INDEX_NONE if it
	 * is outside the assignment radius entirely.
	 *
	 * Computed independently by every bot from the same public world state, so there is no shared
	 * assignment table to keep in sync, and a bot dying re-ranks everyone else on their next
	 * decision tick for free. Ranks 0..InterceptorCount-1 hunt the trace; the next PunisherCount
	 * ranks hold a bead on the carrier; everyone else fights.
	 */
	int32 GetCarrierPressureRank() const;

	// ------------------------------------------------------------------------------------------
	// Dash charges  (spec §5)
	// ------------------------------------------------------------------------------------------

	/**
	 * Charges available right now. The carrier has two, everyone else has one, and both refill on
	 * the same 4 s cooldown.
	 *
	 * Prefers the movement component's own count when it exposes one; otherwise runs the shadow
	 * model in TickDashCharges(), which is exact because this controller is the only thing that ever
	 * asks this pawn to dash.
	 */
	int32 GetDashCharges() const;

	/** Advances the shadow charge model. No-op when the movement component reports its own count. */
	void TickDashCharges(float DeltaSeconds);

	/**
	 * Raises the dash intent if a charge can be spared.
	 *
	 * @param bReserveLast keep one charge back for emergencies. A carrier that spends both charges
	 *                     pushing forward has nothing left when a defender lines up its trace, which
	 *                     is exactly when the second charge exists to be spent.
	 */
	bool RequestDash(bool bReserveLast);

private:
	// ------------------------------------------------------------------------------------------
	// Cached world state, refreshed by GatherWorldState() every tick
	// ------------------------------------------------------------------------------------------

	ETraceTeam MyTeam = ETraceTeam::None;

	/** Whoever currently holds the Core status, found by scanning the roster for IsCarrier(). */
	TWeakObjectPtr<ATraceCharacter> Carrier;

	TWeakObjectPtr<ATraceCharacter> NearestEnemy;
	float NearestEnemyDistSq = 0.f;

	/** Living characters this tick, split by side. Raw pointers: valid only inside the tick. */
	TArray<ATraceCharacter*> LiveTeammates;
	TArray<ATraceCharacter*> LiveEnemies;

	bool bIAmCarrier = false;
	bool bTeammateIsCarrier = false;
	bool bEnemyIsCarrier = false;

	/** Inside faces of the four walls, floor to wall top. Resolved from the arena builder. */
	FBox FieldBounds = FBox(ForceInit);
	bool bBoundsValid = false;

	// ------------------------------------------------------------------------------------------
	// MODE B world state. All false / zero for the whole match in mode A.
	// ------------------------------------------------------------------------------------------

	/** True when the match is playing spec v4 §7 mode B: goals, and a Core that is thrown. */
	bool bModeB = false;

	/** True while the Core is out in the world with no holder — thrown, flying, or lying still. */
	bool bCoreLoose = false;

	/** Where to run to intercept the loose Core: its position, led by its own velocity. */
	FVector LooseCorePoint = FVector::ZeroVector;

	/** 2D distance from this bot to LooseCorePoint, for the chase ranking. */
	float LooseCoreDistSq = 0.f;

	// --- SPEC v28 §6: the turnover lockout, as this bot sees it ----------------------------------
	//
	// Gathered once per tick in GatherWorldState from the replicated turnover state, so every
	// decision below asks the same three questions the Core's own CanPullNow / ServerTryLoosePickup
	// ask, rather than each behaviour re-deriving "am I allowed to have this".

	/** True while a turnover's lockout window is open. Always false in mode A. */
	bool bTurnoverActive = false;

	/** True when MY team dropped it: I may not pick it up and I may not pull. Back off. */
	bool bLockedOutOfCore = false;

	/** True when the OTHER team dropped it: I may pull it, and I may walk onto it. */
	bool bMayPullCore = false;

	/** Seconds left on the window. 0 when no turnover is running. Logged, never used as a threshold. */
	float TurnoverSecondsLeft = 0.f;

	/**
	 * The Core's ACTUAL resting position this tick — not LooseCorePoint, which is led by velocity.
	 *
	 * The pull's aim cone is 4 degrees wide and is measured against where the Core IS, so aiming a
	 * pull at the lead point would miss the cone at exactly the moment the ball is moving. A turned
	 * over Core is at rest (RegisterTurnover zeroes its velocity), so the two agree today — this is
	 * a guard against the day they stop agreeing, not a correction to anything visible now.
	 */
	FVector TurnoverCoreLocation = FVector::ZeroVector;

	/**
	 * Last tick's bLockedOutOfCore, so the WINDOW'S EDGES can force an immediate re-decision.
	 *
	 * DecideState runs on the profile's decision cadence (~0.3 s), and both edges of a 5 s window are
	 * exactly the moments where a stale decision looks worst: at the opening edge a bot would keep
	 * running at a ball it may not touch for another third of a second, and at the closing edge it
	 * would stand off a ball that is live again. The keep-out push already covers the first case
	 * physically; this covers both of them in the plan.
	 */
	bool bWasLockedOutOfCore = false;

	/** Mirror of the pull button, so the press/release edges are only sent once. */
	bool bPullInputHeld = false;

	/** Aim override while going for a pull: the crosshair has one job and it is not shooting. */
	bool bPullOwnsAim = false;
	FVector PullAimPoint = FVector::ZeroVector;

	/** Centre of the goal this team DEFENDS, in mode B. Only meaningful while bDefendGoalValid. */
	FVector DefendGoalCentre = FVector::ZeroVector;
	bool bDefendGoalValid = false;

	// ------------------------------------------------------------------------------------------
	// Attacking endzone  (survives the half-time side switch)
	// ------------------------------------------------------------------------------------------

	/** Centre of the endzone this team scores in. Only meaningful while bAttackGoalValid. */
	FVector AttackGoalCentre = FVector::ZeroVector;
	bool bAttackGoalValid = false;

	/** Sign of the attacking end along X, +1 or -1. Flipping this is what half time looks like. */
	float AttackSideSign = 0.f;

	/** Last sign we logged, so the flip is reported exactly once per bot per half. */
	float LoggedAttackSideSign = 0.f;

	float NextEndzoneResolveTime = 0.f;

	/** One-shot complaint when no ATraceEndzone answers for this team, so it cannot pass silently. */
	bool bWarnedNoEndzone = false;

	// ------------------------------------------------------------------------------------------
	// Intent produced this tick
	// ------------------------------------------------------------------------------------------

	ETraceBotState State = ETraceBotState::Idle;

	/** Unit, planar, world space. Zero means "stand still". */
	FVector DesiredMoveDirection = FVector::ZeroVector;

	/** Where the gun should point. Only meaningful while bWantsToAim. */
	FVector DesiredAimPoint = FVector::ZeroVector;
	bool bWantsToAim = false;

	/**
	 * Aim override used while lining up and holding a pass.
	 *
	 * Passing needs the crosshair genuinely ON a teammate, so for those frames the pass owns the
	 * control rotation outright and the combat aim is suppressed. It is a separate flag from
	 * bWantsToAim because the two must never both be honoured.
	 */
	bool bPassOwnsAim = false;
	FVector PassAimPoint = FVector::ZeroVector;

	/** Mirror of the weapon's trigger, so we only call the press/release entry points on an edge. */
	bool bTriggerHeld = false;

	/**
	 * Raised by a behaviour, consumed at the very end of ApplySteering().
	 *
	 * The dash direction is locked by UTraceCharacterMovementComponent::BeginDash from the pawn's
	 * Acceleration, so the dash entry point has to be called AFTER this tick's AddMovementInput and
	 * BEFORE the movement component consumes it. Raising a flag is the only way to express that
	 * ordering from inside a behaviour function that runs before steering.
	 */
	bool bWantsDashThisTick = false;

	/** Same ordering argument as the dash: a jump is a launch and wants this tick's heading. */
	bool bWantsJumpThisTick = false;

	// ------------------------------------------------------------------------------------------
	// Abilities  (spec v15 §3)
	// ------------------------------------------------------------------------------------------

	/** The per-character decision layer. By value: no replicated state, dies with the controller. */
	FTraceBotAbilityBrain AbilityBrain;

	/** This tick's orders, produced by UpdateAbilities and consumed by UpdateCombat/ApplySteering. */
	FTraceBotAbilityOrders AbilityOrders;

	/**
	 * Mirror of the V (secondary) input, so the release is delivered exactly once.
	 *
	 * The same argument as bPassInputHeld, and for the same reason it is dangerous to get wrong: on
	 * the pawn side a held V is Mace suspended with gravity switched off, and a release that never
	 * arrives leaves her floating for the rest of the match.
	 */
	bool bSecondaryInputHeld = false;

	/** Earliest world time UpdateAutoCharacter may ask again. Also the post-spawn grace period. */
	float NextAutoCharacterTime = 0.f;

	// ------------------------------------------------------------------------------------------
	// Timers and per-bot personality
	// ------------------------------------------------------------------------------------------

	float TimeUntilNextDecision = 0.f;

	/** Weak, because the target dying is the common case and must not keep it alive. */
	TWeakObjectPtr<ATraceCharacter> ShootTarget;

	/** The target the reaction clock below is currently running for. Switching targets restarts it. */
	TWeakObjectPtr<ATraceCharacter> AcquiredTarget;

	/**
	 * Whether AcquiredTarget was non-null last tick.
	 *
	 * Needed separately from the weak pointer: when a target is killed its pawn is destroyed and
	 * the weak pointer silently becomes null, which is indistinguishable from "never had one". That
	 * distinction is exactly what the reacquire delay is triggered by.
	 */
	bool bHadAcquiredTarget = false;

	/** World time this bot first had AcquiredTarget in its sights. Reaction delay measured from it. */
	float TargetAcquiredTime = -1000.f;

	/** Reaction delay rolled for the CURRENT acquisition, including this bot's personal jitter. */
	float CurrentReactionDelay = 0.f;

	/**
	 * Which zone this bot is aiming at on the CURRENT target: 1 head, 0 body, -1 leg.
	 *
	 * Rolled once per acquisition rather than per tick, so a bot commits to a headshot attempt for
	 * the length of an engagement instead of flickering between zones and hitting neither.
	 */
	int32 AimZone = 0;

	/** World time until which this bot cannot acquire anyone. Set when a target dies or is lost. */
	float BlindUntilTime = 0.f;

	/** Slowly-varying aim offset in degrees (yaw, pitch). Refreshed every AimErrorRefreshSeconds. */
	FVector2D AimError = FVector2D::ZeroVector;
	float AimErrorNextRefreshTime = 0.f;

	/** Burst discipline: when the current burst ends, and when the bot may start the next one. */
	float BurstEndTime = 0.f;
	float BurstRestUntilTime = 0.f;

	// --- RESTRUCTURE tranche E (E3): the SMG minimum -------------------------------------------

	/**
	 * Earliest world time the SMG band may be re-evaluated. Four decisions a second, for the knife
	 * band's own reason: the pullout is 0.2 s, and a bot re-deciding every frame at a range
	 * boundary would live permanently inside a swap and never hold either weapon.
	 */
	float NextWeaponBandTime = 0.f;

	/**
	 * RESTRUCTURE tranche D5. Local-clock time of the last knife-band swap decision, so a bot cannot
	 * thrash the 0.2 s pullout. Moved here with UpdateKnifeBand from UTraceWeaponComponent, where it
	 * was per-PAWN state on a per-pawn component; on the controller it is per-BOT state, which is
	 * the same thing for every pawn this rule has ever run on (one controller, one pawn) and is
	 * where a decision cadence belongs.
	 */
	float LastBotSwapDecisionTime = -1000.f;

	// --- RESTRUCTURE tranche E (E4): the parry minimum -----------------------------------------

	/**
	 * World time a latched parry intent fires, or 0 while none is latched. The delay is rolled from
	 * the profile's reaction time exactly as the trigger's is — a frame-one parry is both robotic
	 * and unfair. The press is DELIVERED when the delay expires (unless the carry itself ended):
	 * a dash lasts 0.18 s against a reaction delay of most of a second, so a press conditioned on
	 * the dasher still being mid-dash could never land at all — the flinch-parry is aimed at the
	 * volley, not at the dive that provoked it. See UpdateParryMinimum.
	 */
	float ParryPressTime = 0.f;

	/** Earliest world time a new parry intent may be latched (hesitation after a refusal/whiff). */
	float NextParryEvalTime = 0.f;

	/**
	 * Whether this bot intends to spend its dash on a trace intercept right now.
	 *
	 * NOT rolled per tick: FTraceBotProfile::TrailDashCommitChance would not be a probability at
	 * all, because anything above about 0.2 fires within two frames of entering range and every
	 * value from 0.3 to 1.0 behaves identically.
	 *
	 * NOT rolled once per charge either, which is what it used to be and is the bug spec v9 §1
	 * describes. "One roll per charge" is an absorbing state: declining means never dashing, never
	 * dashing means the charge is never spent, and a charge that is never spent never arrives to
	 * trigger another roll — so one unlucky roll disarms the bot for the rest of its life, and it
	 * spends that life pacing the dash pocket beside the carrier. Measured on Easy: trace dashes
	 * per 10s ran 1,3,3,0,0,0,0,1,1 while the bots' time inside dash range stayed flat.
	 *
	 * Rolled on a charge ARRIVING, and again every TrailCommitRetrySeconds while a hunter is
	 * holding a charge it has declined to spend. The knob therefore sets how long a difficulty
	 * hesitates rather than whether it ever commits.
	 */
	bool bCommitDashWithThisCharge = false;

	/** Earliest world time the commit roll above may be taken again after declining. */
	float NextTrailCommitRollTime = 0.f;

	/**
	 * Dash charges seen last tick, so a charge ARRIVING can be told from a charge merely existing.
	 *
	 * SPEC v9 §1. This replaces a `bool bDashReadyLastTick`, and the type change is the fix. Watching
	 * a boolean means the only observable event is "went from none to some", and an interceptor whose
	 * dash request the pawn refused (CanDash is false in the air and mid-dash) never produces one:
	 * the count does not move, so the commit flag it cleared on request is never re-rolled and the
	 * bot never dashes at a trace again. Measured at 4963 of 8869 hunt ticks refused for "no-commit"
	 * on a profile that commits 95% of its charges. Watching the count catches every refill,
	 * including the carrier's second charge, and a refused dash simply stays armed.
	 */
	int32 LastSeenDashCharges = 0;

	// --- Dash charge shadow model (see GetDashCharges) ---------------------------------------

	/** Charges spent and not yet refilled. Clamped to the current maximum. */
	int32 SpentDashCharges = 0;

	/** World time the oldest outstanding charge comes back. Zero when nothing is outstanding. */
	float NextDashRefillTime = 0.f;

	// --- Movement tech ------------------------------------------------------------------------

	/** True while the crouch input is held (slide on the ground, fast-fall in the air). */
	bool bCrouchHeld = false;

	/** World time the current slide ends, and the earliest the next one may start. */
	float SlideEndTime = 0.f;
	float SlideReadyTime = 0.f;

	/** Strafe handedness while duelling; flips on a timer so bots do not orbit predictably. */
	float StrafeSign = 1.f;
	float StrafeFlipTime = 0.f;

	// --- Passing ------------------------------------------------------------------------------

	ETraceBotPassPhase PassPhase = ETraceBotPassPhase::None;

	/** The teammate the current attempt is aimed at. Weak: they can die mid-hold, which aborts. */
	TWeakObjectPtr<ATraceCharacter> PassReceiver;

	/** World time the current phase began, and when the cooldown expires. */
	float PassPhaseStartTime = 0.f;
	float PassCooldownUntilTime = 0.f;

	/** Next tick this bot is allowed to LOOK for a pass. Throttles the receiver search only. */
	float NextPassEvalTime = 0.f;

	/** Set while the pass input is down, so it is always released exactly once. */
	bool bPassInputHeld = false;

	// --- MODE B throwing ------------------------------------------------------------------------
	//
	// Deliberately reuses PassPhase, PassCooldownUntilTime and NextPassEvalTime. Only one of the two
	// machines can ever run — the scoring mode is latched for the whole match — so a second copy of
	// the phase bookkeeping would be state that is always half dead, and the two would drift.

	/** MODE B. True when the current attempt is a shot at the goal rather than a throw to a teammate. */
	bool bThrowAtGoal = false;

	/** MODE B. What the current attempt is aimed at, BEFORE the ballistic elevation is added. */
	FVector ThrowTargetPoint = FVector::ZeroVector;

	// --- MODE B, SPEC v13 §6: THE CHARGE ----------------------------------------------------------
	//
	// "Bots must use it — a bot that always instant-clicks will throw at 15% power and mode B will
	// look broken. Have them charge appropriately for distance."
	//
	// It is not enough to hold the button and release: the whole throw solver has to know how hard the
	// throw is going to be. SolveThrowLaunch, MaxThrowRange and HasThrowLane all take the charge now,
	// because a bot that solves the pitch for a full-power throw and then releases at 40% aims at a
	// point 60% of the way short of everything it shoots at — which would be a far more visible
	// regression than instant-clicking.

	/**
	 * MODE B. The fraction of full throw momentum the current attempt is going to be released at.
	 *
	 * Chosen from the DISTANCE (see ChooseThrowCharge) and then used by every part of the attempt: the
	 * ballistic solve that produces the aim point, the lane sweep, and the length of the hold. One
	 * number, decided once per frame of the line-up, so the aim and the launch cannot disagree.
	 */
	float PlannedThrowCharge = 1.f;

	/** MODE B. World time the held throw button is released, i.e. when the Core actually leaves. */
	float ThrowChargeReleaseTime = 0.f;

	/**
	 * MODE B, spec v5 §4. True while this carrier has committed to running the Core INTO the goal
	 * rather than throwing it.
	 *
	 * A latch, not a per-frame test, and that is the point. Scoring by carrying it in never fired
	 * once in ~7 minutes of measured play even though the rule works, because a carrier close to the
	 * goal kept re-deciding: one evaluation tick it was in range for a shot, the next an escort came
	 * open and it threw the Core away 1500 uu from an open mouth. Inside the commit radius the throw
	 * machine is switched off entirely and the bot drives at the mouth until it scores, dies, or
	 * leaves the radius.
	 */
	bool bCommitCarryIn = false;

	/** World time the carry-in commit began, so a bot that is being held out of the mouth gives up. */
	float CarryInCommitTime = 0.f;

	/**
	 * Throttle for the [BotThrowLane] diagnostic. Mutable because HasThrowLane is a const query.
	 *
	 * A carrier pressed against cover re-evaluates the shot several times a second and is refused
	 * every time, which turned a one-line diagnosis into six lines a second per bot the first time it
	 * fired. The information is in the FIRST one.
	 */
	mutable float NextThrowLaneLogTime = 0.f;

	// --- Steering -----------------------------------------------------------------------------

	/** Seconds spent wanting to move but not moving. Drives the evade kick and the unstick jump. */
	float StuckSeconds = 0.f;

	/** While world time < EvadeUntilTime, steering is overridden by EvadeDirection. */
	float EvadeUntilTime = 0.f;
	FVector EvadeDirection = FVector::ZeroVector;

	/**
	 * Per-bot jitter in [0,1), seeded once at possession. Multiplies reaction time and aim error so
	 * five bots on a team do not act as one organism, and picks the escort's screen/deep-run role.
	 */
	float PersonalitySkillBias = 0.5f;

	/** Lateral offset used to keep escorting/attacking bots from stacking on one another. -1..1. */
	float FormationBias = 0.f;

	/** Log noise control: only report a state change once. */
	ETraceBotState LastLoggedState = ETraceBotState::Idle;
};
