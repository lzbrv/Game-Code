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
//    in reserve), slide, crouch fast-fall and boost each have a specific job, listed on
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
	Fight
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

	/** Slide, crouch fast-fall, boost and jump. Runs every tick, after the behaviours. */
	void UpdateMovementTech(float DeltaSeconds);

	/** Aim, reaction delay, aim error, burst discipline and the trigger. Suppressed while carrying. */
	void UpdateCombat(float DeltaSeconds);

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

	/** Only static geometry blocks this: the Pawn collision profile ignores ECC_Visibility. */
	bool HasLineOfSight(const AActor* Target) const;

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
	 */
	bool FindTrailInterceptPoint(FVector& OutPoint, FVector& OutTangent) const;

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

	/** Same ordering argument as the dash: a boost is a launch and wants this tick's heading. */
	bool bWantsBoostThisTick = false;
	bool bWantsJumpThisTick = false;

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

	/**
	 * Whether this bot intends to spend its CURRENT dash charge on a trace intercept.
	 *
	 * Rolled once per charge, on the frame the dash comes off cooldown — not per tick. Rolled per
	 * tick, FTraceBotProfile::TrailDashCommitChance was not a probability at all: anything above
	 * about 0.2 fired within two frames of entering range, so every value from 0.3 to 1.0 behaved
	 * identically and the knob could not be used to tune how hard the defence presses.
	 */
	bool bCommitDashWithThisCharge = false;
	bool bDashReadyLastTick = false;

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

	/** World time the boost comes off cooldown. Bot-side mirror of the 12 s rule. */
	float BoostReadyTime = 0.f;

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

	// --- Steering -----------------------------------------------------------------------------

	/** Seconds spent wanting to move but not moving. Drives the evade kick and the boost hop. */
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
