#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "Gameplay/TraceHitZones.h"   // ETraceHitZone
#include "TraceWeaponComponent.generated.h"

class ATraceCharacter;

/**
 * The hitscan handgun carried by everyone who is not holding the Core.
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

	/** Trigger pressed. Local input path only; fires one shot right away, then repeats while held. */
	void StartFire();

	/** Trigger released. */
	void StopFire();

	/** False while carrying the Core, while dead, or while the local fire-rate gate is closed. */
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
};
