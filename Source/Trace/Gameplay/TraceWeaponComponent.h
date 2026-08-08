#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "Gameplay/TraceHitZones.h"   // ETraceHitZone
#include "Gameplay/TraceMelee.h"      // ETraceEquippedWeapon, ETraceMeleeRefusal, FTraceMeleeHit
#include "UObject/ObjectPtr.h"
#include "TraceWeaponComponent.generated.h"

class ATraceCharacter;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * The hitscan handgun AND the knife (spec v10 §1) — one component, because they are one selector.
 *
 * ============================================================================================
 * THE KNIFE LIVES HERE, AND Gameplay/TraceMelee.h IS ITS DESIGN DOCUMENT.
 * ============================================================================================
 *
 * Read TraceMelee.h before changing anything below the FIRE section. It carries the full rationale
 * for the carrier immunity ([USER-CONFIRMED]: the knife can NEVER hurt the Core carrier, not even
 * mid-pass), for the back/front angle model, for why the swing is a swept arc rather than a point,
 * and for why the yaw ring exists at all. This header only says how the STATE is arranged.
 *
 * WHY BOTH WEAPONS ARE ONE COMPONENT. A separate melee component would mean two replicated objects
 * agreeing about which weapon is in your hands, and this codebase already carries a four-writer bug
 * of exactly that shape (ATracePlayerState::bIsCarrier). One component means:
 *
 *   * ONE selector on the wire — EquippedWeapon, plus one deadline for the pullout;
 *   * ONE gate. CanFire() and CanSwing() are two branches of the same question, so "you may not
 *     shoot while the knife is out" and "you may not swing while the gun is out" cannot disagree;
 *   * ONE input path. ATraceCharacter::DoFirePressed already routes mouse1 here, so StartFire()
 *     dispatches to a swing when the knife is equipped and nothing outside this file has to learn
 *     a new verb — which is also what gets the bots swinging with no change to their controller.
 *
 * THE SWAP IS A TOGGLE ON ONE BIND, and 0.2 s of it is dead air in which NEITHER weapon works. That
 * deadline is replicated (DeployEndServerTime, on the shared clock), so the server refuses a shot
 * or a swing inside a pullout using the same number the client gated on. The owning client predicts
 * the swap locally, exactly as it predicts its own tracer; the server's copy arrives ~RTT/2 later
 * and can only ever push the deadline LATER, never earlier, so prediction cannot buy time.
 *
 * ============================================================================================
 * The handgun.
 *
 * DAMAGE IS POSITIONAL (spec section 6): head 100 / body 40 / legs 25 against 100 health, so a
 * head shot kills outright, three body shots kill, four leg shots kill. There is no headshot
 * multiplier any more and there is NO SPREAD AT ALL - the spec removes movement inaccuracy, so the
 * shot goes exactly where the crosshair points, every time, moving or still. See
 * Gameplay/TraceHitZones.h for where the zones are and what the approximation costs.
 *
 * Network model (see docs/NETWORKING.md):
 *  1. The owning client gates on fire rate locally, rolls its own spread, draws its own tracer
 *     immediately, and only then tells the server. The shot *feels* instant regardless of ping.
 *  2. The client stamps the shot with the shared clock (AGameStateBase::GetServerWorldTimeSeconds)
 *     and ships that timestamp with the RPC.
 *  3. The server re-validates everything it can cheaply check, clamps the timestamp into
 *     [Now - MaxRewindTime, Now], and resolves the shot against rewound poses
 *     (UTraceLagCompensationComponent::ResolveHitscan) - the client's claim of a *hit* is never
 *     trusted, only its claim of *when and where it aimed*.
 *  4. Cosmetic effects go out over an unreliable multicast that deliberately skips the shooter,
 *     because the shooter already drew them in step 1.
 */
UCLASS(ClassGroup = (Trace), meta = (BlueprintSpawnableComponent))
class TRACE_API UTraceWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTraceWeaponComponent();

	/**
	 * Registers the ONE tick dependency this component needs: the owner's camera boom must update
	 * AFTER us, so a recoil kick and the camera that renders it land in the same frame.
	 *
	 * See the implementation for the measurement that made this necessary - without it the probe
	 * reads aimErr = 0.8 deg (exactly one kick) for one frame after every shot.
	 */
	virtual void BeginPlay() override;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** EquippedWeapon and DeployEndServerTime. Everything else here is local or RPC-only. */
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * ATTACK PRESSED. Local input path only.
	 *
	 * Dispatches on the equipped weapon: a shot with the gun, a swing with the knife. This is the
	 * whole reason the knife needed no new input plumbing and no bot-controller change — mouse1 is
	 * "attack", and ATraceCharacter::DoFirePressed already routes it here.
	 */
	void StartFire();

	/** Attack released. */
	void StopFire();

	/**
	 * False while carrying the Core, while dead, MID-DASH (spec §6), during a weapon pullout, while
	 * the knife is equipped, or while the local fire-rate gate is closed.
	 */
	bool CanFire() const;

	/**
	 * @param Origin               Muzzle position the client fired from.
	 * @param Direction            Unit aim direction. No spread is applied any more (spec section 6).
	 * @param ClientFireServerTime Shared-clock timestamp of the shot, from the client's GameState.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerFire(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientFireServerTime);

	/**
	 * Cosmetic railgun effect for everyone except the shooter, who already predicted it.
	 *
	 * @param bImpacted True when the beam actually stopped on something (a body or world geometry)
	 *                  rather than dying at maximum range, so the impact flash is only drawn where
	 *                  there is a surface to flash against.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastFireEffects(FVector_NetQuantize Origin, FVector_NetQuantize Impact, bool bImpacted);

	// =============================================================================================
	// THE KNIFE (spec v10 §1). Design rationale: Gameplay/TraceMelee.h. Read it first.
	// =============================================================================================

	/** Which weapon is in this pawn's hands. Replicated, so every machine agrees — including the
	 *  movement component, which multiplies the ground speed limit by 1.22 off the back of it (spec v12 s3). */
	ETraceEquippedWeapon GetEquippedWeapon() const { return EquippedWeapon; }

	bool IsKnifeEquipped() const { return EquippedWeapon == ETraceEquippedWeapon::Knife; }

	/** True inside the 0.2 s pullout, during which neither weapon works. Shared clock, so it is the
	 *  same answer on the client that predicted the swap and on the server that validates it. */
	bool IsDeploying() const;

	/** Seconds left of the pullout; 0 when the weapon is up. HUD and diagnostics. */
	float GetDeployRemaining() const;

	/** Seconds until the next swing is legal; 0 when it is legal now. HUD and bots. */
	float GetSwingCooldownRemaining() const;

	/**
	 * THE SWAP BIND. Toggles gun <-> knife. Local input path only; safe to call on any machine.
	 *
	 * Wire the rebindable key to this (through ATraceCharacter, see the pass report). Both
	 * directions are one bind and one 0.2 s pullout, because the user gave one pullout number and a
	 * knife that comes up faster than it goes away is a second mechanic nobody asked for.
	 */
	void DoSwapWeaponPressed();

	/**
	 * Explicit form of the swap — what the bots and the console commands want, since they are not
	 * toggling. Predicts locally on an owning client and asks the server; acts directly on the
	 * server. Returns true when a pullout started.
	 */
	bool RequestEquip(ETraceEquippedWeapon Desired, ETraceMeleeRefusal* OutRefusal = nullptr);

	/**
	 * DIRECT SELECT (spec v13 §2). Same as RequestEquip, except that asking for the weapon already in
	 * hand does NOTHING — it does not restart the 0.2 s pullout, and it returns false with
	 * ETraceMeleeRefusal::None.
	 *
	 * *** THIS IS A SECOND ENTRY POINT AND NOT A CHANGE TO RequestEquip, WHICH WOULD BE A BUG. ***
	 * The two verbs want opposite things from a redundant press and both are right:
	 *
	 *   SwapWeapon (F) TOGGLES. A second press is a second intent, so it costs a pullout — refusing
	 *                  it silently would make a double-tap feel like a dropped input. RequestEquip's
	 *                  own doc comment commits to that, and the bots and console commands rely on it.
	 *   EquipKnife (1) / EquipGun (2) SELECT. Spec §2, verbatim: "pressing 1 while already holding
	 *                  the knife does nothing (and must not re-trigger the 0.2 s pullout)." A player
	 *                  mashing 1 before a fight must not be re-drawing the blade on every press.
	 *
	 * IT IS CORRECT MID-PULLOUT, which is the case that looks like a hole. ApplyEquip sets
	 * EquippedWeapon at the PRESS and lets the deploy deadline run on, so GetEquippedWeapon() already
	 * names the DESTINATION weapon for the whole 0.2 s. Pressing 1 during the knife's own pullout is
	 * therefore ignored (correct — that IS the "must not re-trigger" case), while pressing 1 during
	 * the GUN's pullout is allowed and starts a fresh pullout to the knife (correct — the player
	 * changed their mind, and what is coming up is not what they now want).
	 *
	 * The gate lives HERE, on the component, rather than in the controller that binds the key, so
	 * every direct-select caller gets it — bots, console commands and a future UI all included. The
	 * legality questions (dead, carrying, no pawn) are untouched and still belong to RequestEquip;
	 * this only answers "did the player ask for anything at all".
	 */
	bool RequestEquipIfDifferent(ETraceEquippedWeapon Desired, ETraceMeleeRefusal* OutRefusal = nullptr);

	/**
	 * Starts one swing. The blade resolves SwingWindupSeconds later (see TickSwing), which is what
	 * makes it read as a swing rather than as a very short shotgun. Returns true when it started.
	 */
	bool StartSwing(ETraceMeleeRefusal* OutRefusal = nullptr);

	/** The knife's half of CanFire(): alive, not carrying, not dashing, deployed, off cooldown. */
	bool CanSwing(ETraceMeleeRefusal* OutRefusal = nullptr) const;

	/**
	 * @param Desired               the weapon to bring up. A request for the weapon already equipped
	 *                              still costs a pullout, deliberately: refusing it silently would
	 *                              make a double-tap of the bind feel like a dropped input.
	 * @param ClientPressServerTime  shared-clock timestamp of the PRESS, from the pressing machine.
	 *
	 * THE STAMP IS WHY A CLIENT'S PULLOUT IS 0.2 s AND NOT 0.2 s PLUS THEIR PING, and it is the same
	 * trick TraceParry::ServerResolvePress plays for the parry window. MEASURED, on a real client at
	 * 40 ms, before this parameter existed: the client predicted a deadline of press + 0.2 on the
	 * shared clock, the server anchored ITS deadline at arrival + 0.2, and the replicated value
	 * pushed the client's out to 0.294 s — a 47% error in one of only three numbers the user gave.
	 *
	 * Anchoring the deadline at the stamped press makes both machines compute the identical shared
	 * clock instant, so the replicated update is a no-op instead of an extension. THE CLAMP IS THE
	 * SECURITY, exactly as it is for a shot: whatever the client claims, the press is pinned into
	 * [ServerNow - MaxRewindTime, ServerNow] — never in the future, so a swap cannot be pre-booked,
	 * and never further back than a bullet may be rewound, so it cannot be used to skip the pullout.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestEquip(ETraceEquippedWeapon Desired, float ClientPressServerTime);

	/**
	 * One swing, resolved against rewound poses exactly as ServerFire resolves a shot.
	 *
	 * @param Origin                Blade origin the client swung from (GetMuzzleLocation()).
	 * @param Direction             Centre of the arc; the sweep is built around it identically on
	 *                              both machines, so there is no arc geometry on the wire.
	 * @param ClientSwingServerTime Shared-clock timestamp of the RESOLVE INSTANT — the moment the
	 *                              edge passed through, not the moment the key went down. That is
	 *                              the instant the client saw the blade cross the target, so it is
	 *                              the instant the server has to rewind to.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSwing(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientSwingServerTime);

	/**
	 * Cosmetic slash for everyone except the swinger, who already drew it. Same owner-skipping
	 * contract as MulticastFireEffects, and for the same reason.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastSwingEffects(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, bool bConnected);

	UFUNCTION()
	void OnRep_EquippedWeapon();

	/**
	 * SERVER. "Which way was @p Character's body facing at shared-clock time @p ServerTime?"
	 *
	 * THE ONE THING THE KNIFE COULD NOT BORROW FROM THE GUN. FTraceLagCompFrame records a capsule
	 * and no rotation at all, because a bullet does not care which way you are looking — and a
	 * back-stab cares about nothing else. At 40 ms each way a briskly turning player's yaw is 80 ms
	 * stale, which at 400 deg/s is 32 degrees: enough to flip a 3.3x damage verdict, in the
	 * direction the attacker cannot see. So this component keeps its own ring (one float per frame
	 * per pawn, trimmed by the same UTraceSettings::LagCompHistoryDuration window the pose history
	 * uses) and TraceMelee::ResolveSwing asks it.
	 *
	 * ACTOR yaw, not control yaw: the attacker is aiming at a MESH, and the mesh faces the actor.
	 *
	 * @return false when there is no history covering that instant (a fresh spawn, a client, lag
	 *         compensation switched off); @p OutYaw is then left exactly as the caller set it, so
	 *         the caller's live-pose fallback survives.
	 */
	static bool GetFacingYawAtTime(const ATraceCharacter* Character, float ServerTime, float& OutYaw);

private:
	ATraceCharacter* GetTraceCharacter() const;

	/** Shared clock, replicated by the GameState. Used for the rewind timestamp, never for gating. */
	double GetServerTimeSeconds() const;

	/** Local monotonic clock. Used for both fire-rate gates so a clock resync cannot stall firing. */
	double GetLocalTimeSeconds() const;

	/** Runs the whole predicted client-side shot and sends ServerFire. */
	void FireOnce();

	void PlayLocalTracer(const FVector& From, const FVector& To, bool bImpacted) const;

	// --- Upwards recoil (spec v5 section 6) ------------------------------------------------------
	//
	// PURELY VERTICAL, and purely local. The kick is added to the owning PLAYER controller's control
	// rotation after the shot's direction has already been sampled and sent, so:
	//   * the round that caused the kick still goes exactly where the crosshair was;
	//   * there is no recoil state on the wire - ServerFire carries a direction, so the authority
	//     resolves the same ray the shooter saw, and aimErr stays 0.0000 deg because the camera and
	//     the aim ray are both pure functions of the control rotation;
	//   * bots are excluded: ATraceBotController overwrites its control rotation every frame with an
	//     RInterpConstantTo slew, so a kick would be erased before it did anything.

	/** The local human's controller, or null for a bot, a proxy, or a pawn with no controller. */
	class APlayerController* GetRecoilController() const;

	/** One shot's worth of upward kick, with the growth term and the accumulation ceiling applied. */
	void ApplyRecoilKick();

	/** Runs the recovery, and the player-compensation cancellation, once per tick. */
	void TickRecoil(float DeltaTime);

	/**
	 * Adds DeltaPitchDegrees to the control rotation (positive = up), clamped into the camera
	 * manager's own pitch limits, and folds however much ACTUALLY landed into RecoilAppliedPitch.
	 * Reading the applied amount back is what keeps the accumulator honest when the view is already
	 * against the 89.9 degree stop.
	 */
	void AddRecoilPitch(class APlayerController* RecoilController, double DeltaPitchDegrees);

	/** Forgets the climb and the burst without touching the view. Death, respawn, teardown. */
	void ResetRecoil();

	/** Degrees of un-recovered climb currently sitting on top of the player's own aim. Never < 0. */
	double RecoilAppliedPitch = 0.0;

	/** How many shots into the current burst we are; drives the per-shot growth term. */
	int32 RecoilBurstShotIndex = 0;

	/** Local-clock time of the last shot that produced a kick. */
	double LastRecoilShotTime = -1000.0;

	/**
	 * Control pitch as it stood after our own last write, so the next tick can tell the player's
	 * mouse movement apart from our own. Only meaningful while bRecoilTrackingValid.
	 */
	double RecoilTrackedPitch = 0.0;
	bool bRecoilTrackingValid = false;

	/**
	 * Folds one server-resolved shot into the Trace.ShotStats distribution.
	 *
	 * Authority only, and only while the cvar is on. Reads LastPredicted* so it can also count the
	 * predicted-vs-authoritative agreement rate for shots whose prediction ran in this same process
	 * (a bot, single player, or the listen host's own pawn).
	 */
	void AccumulateShotStats(ETraceHitZone ServerZone, const class ATraceCharacter* Victim,
		const struct FTraceHitscanDiagnostics& Diagnostics);

	/**
	 * Last zone the LOCAL predicted trace produced, and the victim it produced it against.
	 *
	 * Only read by the Trace.DebugHitZones instrumentation, which compares them against what the
	 * server independently resolved. On a listen host both traces run in this same process, so the
	 * comparison is a direct answer to "does what the shooter saw match what the server scored" -
	 * the failure mode spec section 6 warns about. Never used for gameplay.
	 */
	ETraceHitZone LastPredictedZone = ETraceHitZone::None;
	TWeakObjectPtr<ATraceCharacter> LastPredictedVictim;
	double LastPredictedFireServerTime = -1000.0;

	/** Trigger state. Only meaningful on the machine that owns the input (client, or listen host). */
	bool bTriggerHeld = false;

	/** Local-clock time of the last shot this machine predicted. */
	double LastLocalFireTime = -1000.0;

	/** Local-clock time of the last shot the server accepted. Authority only. */
	double LastAcceptedFireTime = -1000.0;

	/**
	 * Fraction of FireInterval the server forgives. Honest clients time their shots against their
	 * own smoothed copy of the server clock and their packets arrive jittered and occasionally
	 * bunched, so a strict >= FireInterval test on arrival times punishes exactly the players we
	 * are trying to serve. Cheating past this buys ~20% more DPS, not an aimbot.
	 */
	static constexpr double FireRateTolerance = 0.2;

	/**
	 * How far the client-supplied muzzle may sit from the shooter's own rewound capsule centre
	 * before we distrust it. Generous enough to cover the capsule, the muzzle offset and a frame or
	 * two of movement; tight enough that a client cannot shoot from across the arena.
	 */
	static constexpr double MaxOriginErrorUU = 500.0;

	/** Absurdity check on the raw payload before any of it is used in maths. */
	static constexpr double MaxReasonableCoordinateUU = 1.0e7;

	// =============================================================================================
	// KNIFE STATE (spec v10 §1). Design rationale: Gameplay/TraceMelee.h.
	// =============================================================================================

	/**
	 * The selector. Replicated to EVERYONE, not just the owner, and that is load-bearing three ways:
	 * the movement component multiplies the ground speed limit by it on every machine (so the client
	 * predicts its own 976 uu/s instead of being corrected into it), other players see the knife in
	 * the hand, and the server gates ServerFire/ServerSwing on the same value the client gated on.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_EquippedWeapon)
	ETraceEquippedWeapon EquippedWeapon = ETraceEquippedWeapon::Gun;

	/**
	 * Shared-clock instant the current pullout finishes. Negative means "no pullout in progress".
	 *
	 * THE SHARED CLOCK, not a local one, and not a duration. A duration would need a start instant
	 * to be meaningful and would then have to be replicated alongside it; a deadline on
	 * AGameStateBase::GetServerWorldTimeSeconds is one float that means the same thing on every
	 * machine — the same choice UTraceTrailComponent makes for the parry window.
	 */
	UPROPERTY(Replicated)
	float DeployEndServerTime = -1.f;

	/** Local-clock instant of the last swing this machine STARTED. Drives the 0.5 s gate. */
	double LastLocalSwingTime = -1000.0;

	/** Local-clock instant of the last swing the server accepted. Authority only. */
	double LastAcceptedSwingTime = -1000.0;

	/** Local-clock instant the pending swing's blade should resolve; see TickSwing. */
	double SwingResolveAtLocalTime = -1000.0;
	bool bSwingPendingResolve = false;

	/** Local-clock instant the current swing animation started, or a large negative sentinel. */
	double SwingAnimStartLocalTime = -1000.0;

	/**
	 * Fraction of SwingCooldownSeconds the server forgives, for exactly the reason FireRateTolerance
	 * exists: honest clients time their swings against their own smoothed copy of the shared clock
	 * and their packets arrive jittered. Cheating past it buys ~20% more swings, not a teleport.
	 */
	static constexpr double SwingRateTolerance = 0.2;

	/** Runs the wind-up: resolves the blade and sends ServerSwing when the edge passes through. */
	void TickSwing(float DeltaTime);

	/**
	 * Pushes "is the knife the active weapon" into UTraceCharacterMovementComponent.
	 *
	 * THE DIVISION OF LABOUR (spec v10 §1): this slice owns the FACT, the movement slice owns the
	 * NUMBERS. UTraceCharacterMovementComponent::SetKnifeMovementProfileActive is the documented
	 * entry point for the melee slice and the multipliers behind it (KnifeMoveSpeedMultiplier and
	 * the two air-cap multipliers) live on UTraceSettings where the movement component reads them.
	 * There is no second copy of 1.22 anywhere in the melee code, deliberately.
	 *
	 * CALLED ON EVERY MACHINE, and that is required rather than tidy: the bit is round-tripped
	 * through FSavedMove_Trace, so a server correction replays each move under the profile that move
	 * actually ran with — but only if both ends were told. Their contract says it is idempotent and
	 * cheap enough to call every frame, so it is called from the tick as well as from the equip, and
	 * the tick call is what catches a carrier transition (which changes the answer without changing
	 * the selector) and a pawn whose movement component was not ready at equip time.
	 */
	void RefreshMovementProfile();

	/** Applies a swap locally (both the state and the deadline). Server and predicting client. */
	void ApplyEquip(ETraceEquippedWeapon Desired, double DeployEndSharedTime);

	// --- The victim-facing ring (see GetFacingYawAtTime) ------------------------------------------

	struct FTraceFacingSample
	{
		float ServerTime = 0.f;
		float Yaw = 0.f;
	};

	/**
	 * Oldest first, newest last, trimmed by age. Not a UPROPERTY on purpose: plain floats with no
	 * UObject references, so there is nothing for the GC to keep alive and nothing worth the
	 * reflection overhead — the same reasoning UTraceLagCompensationComponent::History gives.
	 */
	TArray<FTraceFacingSample> FacingHistory;

	/** Server only. Appends one sample and trims the window. Called once per tick. */
	void RecordFacingSample(float ServerTime);

	/** Hard ceiling so a hitch cannot grow the ring without bound. Matches the pose history's. */
	static constexpr int32 MaxFacingSamples = 512;

	// --- Bots (spec §1: "Bots must use it, or it will not be playtested") -------------------------

	/**
	 * The bot's knife rule, in one function: swap to the knife inside BotEngageRangeUU of the
	 * nearest living enemy, back to the gun outside BotDisengageRangeUU, and let the existing burst
	 * logic do the swinging — because StartFire() dispatches to a swing, a bot holding its trigger
	 * with the knife out is already swinging at the 0.5 s cadence with no change to its controller.
	 *
	 * TEMPORARY, AND HONESTLY SO. This belongs in ATraceBotController's state machine (a knife pass
	 * on HuntCarrier / ChaseLooseCore / Fight would be far better than a range band), but that file
	 * is another ownership slice this pass. It lives here so the weapon is actually exercised by a
	 * bot match TODAY rather than being shipped untested; see the pass report for the hand-off.
	 * Authority + bot-controlled only, and switchable with Trace.Knife.BotAuto.
	 */
	void TickBotKnife();

	/** Local-clock time of the last bot swap decision, so a bot cannot thrash the 0.2 s pullout. */
	double LastBotSwapDecisionTime = -1000.0;

	// --- Presentation: the knife you can see ------------------------------------------------------
	//
	// TWO rigs, because there are two audiences and they need opposite things:
	//
	//   KnifeViewRoot   first person, OnlyOwnerSee, hung under ATraceCharacter::ViewModelRoot so it
	//                   inherits the sway/bob/recoil transform the gun already gets for free. This
	//                   is the rig that physically swings, and it is the swinger's whole read.
	//   KnifeHandRoot   third person, OwnerNoSee, attached to the Mannequin's hand_r bone so every
	//                   OTHER player can see that this person chose the knife — which is the tell
	//                   that they are now 22% faster (spec v12 §3) and cannot shoot back.
	//
	// THERE IS NO THIRD-PERSON GUN RIG, and that is worth knowing before hunting spec v12 §7's
	// "knife and gun at the same time" on somebody else's pawn: ATraceCharacter builds a first-person
	// viewmodel and nothing else, so the only weapon another player can ever see in your hands is
	// KnifeHandRoot. That bug was first person only.
	//
	// Both are built lazily and on rendering machines only. Neither collides, neither casts a
	// shadow, and neither is ever read by the hit resolution: TraceMelee::ResolveSwing is pure
	// arithmetic on the aim ray, so the blade can lag, swing and overshoot without the arc it
	// actually cut moving by a millimetre.

	void EnsureKnifeVisualsBuilt();
	void UpdateKnifeVisuals(float DeltaTime);

	/**
	 * Hides the GUN half of ATraceCharacter's viewmodel while the knife is out — the weapon parts
	 * only, never the hands or forearms, or the player would be holding a knife with no hands.
	 *
	 * IDEMPOTENT AND RE-ASSERTED EVERY TICK, WHICH IS THE FIX FOR SPEC v12 §7 ("the knife and gun
	 * can be held at the same time"). This used to early-return when the requested state matched the
	 * last request, which made it an edge trigger on a set of components it does not exclusively own:
	 * ATraceCharacter::SetViewModelVisible(true) re-shows every part of its rig, gun included, when
	 * the carry blend returns to first person or a pawn respawns, and the latch then swallowed the
	 * correction. The full diagnosis is on the definition.
	 *
	 * The rule it now enforces, on every call, is one line: a gun part is visible exactly when the
	 * character wants its rig on screen AND no knife is out. It can therefore only ever hide a gun
	 * the character is showing, never resurrect one the character has hidden (a corpse's, say).
	 *
	 * Identified by NAME, and that is a documented compromise rather than an oversight: the parts
	 * table is private to ATraceCharacter (another ownership slice) and the only public handle on
	 * the rig is ViewModelRoot. The failure mode if that table is ever renamed is benign and
	 * visible — the gun stays on screen next to the knife, which is ugly and immediately obvious,
	 * rather than the hands vanishing, which is subtle and looks like a rendering bug.
	 */
	void SetGunViewModelHidden(bool bHidden);

	/**
	 * Refills CachedGunParts from ViewModelRoot's children when the rig has changed shape.
	 *
	 * Exists so the per-tick re-assert above costs nothing: walking the children every frame would
	 * allocate a TArray every frame for a list that changes twice in a pawn's life. Keyed on the
	 * child count because ATraceCharacter builds its viewmodel LAZILY — usually several frames after
	 * this component starts ticking — so a cache taken once, on the first tick, would be empty
	 * forever and the gun would never be hidden at all.
	 */
	void RefreshGunPartCache(const ATraceCharacter& Character);

	/** True for the hand/forearm parts of the viewmodel, which stay visible with either weapon. */
	static bool IsViewModelHandPart(const UStaticMeshComponent* Part);

	/** One primitive of either knife rig. Mirrors ATraceCharacter::AddViewModelPart's guarantees. */
	UStaticMeshComponent* AddKnifePart(USceneComponent* AttachTo, const TCHAR* DebugName,
		UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation, const FVector& Size,
		bool bNeon, bool bFirstPerson);

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> KnifeViewRoot;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> KnifeViewParts;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> KnifeHandRoot;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> KnifeHandParts;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> KnifeCubeMesh;

	/**
	 * Two latches, not one, and that is a real bug avoided rather than tidiness.
	 *
	 * On a client the controller arrives by replication some frames AFTER the pawn does, so a single
	 * "visuals built" latch set on the first tick would decide "this pawn is not locally controlled,
	 * therefore no first-person knife" and never revisit it — the local player would carry an
	 * invisible knife for the whole match. The two rigs have different preconditions (one needs the
	 * viewmodel and local control, the other needs the Mannequin's hand_r socket, which does not
	 * exist at all when the art import has not been run), so they latch independently.
	 */
	bool bKnifeViewBuilt = false;
	bool bKnifeHandBuilt = false;

	/** Set once when /Engine/BasicShapes could not be resolved, so the warning is logged once. */
	bool bKnifeMeshUnavailable = false;

	/** The last thing SetGunViewModelHidden was ASKED for. Diagnostics only — it gates nothing now. */
	bool bGunViewModelHidden = false;

	/** ViewModelRoot's weapon parts, minus the hands. Rebuilt when the rig's child count changes. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> CachedGunParts;

	/** Child count CachedGunParts was taken at. -1 so the first call always builds. */
	int32 CachedViewModelChildCount = -1;

	/** Last visibility pushed to each rig, so the render state is only dirtied on a change. */
	bool bKnifeViewVisible = false;
	bool bKnifeHandVisible = false;

#if !UE_BUILD_SHIPPING
public:
	/**
	 * Dev-only census of what is actually on screen, for Trace.Knife.DualWeaponTest.
	 *
	 * Counts VISIBLE components, not intended ones, because spec v12 §7 is a bug about the two
	 * rigs disagreeing — a test that asked either rig what it believed would have agreed with itself
	 * and reported no bug. Everything here is read from the components' own visibility flags.
	 *
	 * @param OutGunVisible    first-person GUN parts currently drawn.
	 * @param OutKnifeVisible  first-person KNIFE parts currently drawn.
	 * @param OutHandVisible   first-person hand/forearm parts, which belong to neither weapon.
	 * @param OutBodyKnife     third-person knife parts on the hand_r socket — what OTHER players see.
	 *                         There is no third-person gun mesh in this project, so this is the whole
	 *                         of what another player can see in your hands.
	 */
	void GetViewModelCensus(int32& OutGunVisible, int32& OutKnifeVisible, int32& OutHandVisible,
		int32& OutBodyKnife) const;
#endif
};
