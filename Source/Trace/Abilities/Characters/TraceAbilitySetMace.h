// Trace — MACE (spec v14 §6).
//
// ===================================================================================================
// THE DOC, VERBATIM
// ===================================================================================================
//
//   PASSIVE   "+30% magnet radius for the Core arcing to her. The base is now 450 uu (reduced 10% in
//             Demo 12), so Mace's is 585 uu. Derive it, do not hardcode."
//
//   MOVEMENT  "hold V in the air to suspend for up to 1.25 s — gravity does not affect her, and she
//             may move laterally capped at 550 uu/s. Releasing V cancels immediately and gravity
//             resumes."
//
//   ACTIVATED "Spike: throws a roped spike in her aim direction for a medium distance. On hitting a
//             WALL it embeds for 2 s. Reactivating pulls her toward it at the momentum ceiling (the
//             air-strafe hard cap). Any movement input cancels the pull and removes the spike. She
//             obeys normal physics while pulled — bouncing off a wall cancels it. She can shoot while
//             pulled, and be shot."   [ASSUMPTION] 20 s cooldown; §6 leaves it unspecified. FLAGGED.
//
// ===================================================================================================
// MACE AND THE CARRIER CHOKE POINT
// ===================================================================================================
//
// Mace has no ability that damages, slows, pulls or knocks back another player. Nothing she does
// takes a target at all: the suspend and the pull both write HER OWN velocity, and the spike anchors
// to world geometry. So there is no CanAffect() call in this file, and that is not an oversight — it
// is the reason Oyster's file is full of them and this one is not. If a future tuning pass ever lets
// the spike catch a player, or lets the pull drag somebody along, THAT is the moment this file gains
// a CanAffect(Target, Control) and not a moment later.
//
// ===================================================================================================
// HOW SUSPEND AND PULL MOVE HER, AND WHY IT IS DONE THIS WAY
// ===================================================================================================
//
// Both write UCharacterMovementComponent::Velocity every frame from TickAbilities(), on the SERVER
// and on the OWNING CLIENT, from state both machines compute the same way.
//
// The obvious implementations are both closed:
//   * GravityScale = 0 for the suspend does not survive one frame. UTraceCharacterMovementComponent::
//     RefreshEngineTunablesFromSettings() re-pushes GravityScale (and MaxWalkSpeed, and AirControl)
//     from UTraceSettings on EVERY simulated move, precisely so those values stay live under
//     Trace.LiveEdit. Anything this file wrote there would be overwritten before the next physics
//     step, and would additionally be wrong on a replayed move.
//   * A proper saved-move flag is the right long-term answer and is NOT available: FSavedMove_Trace
//     and the whole prediction path live in Movement/TraceCharacterMovementComponent.h, which this
//     slice does not own.
//
// So the suspend zeroes Velocity.Z and clamps the planar speed each frame, and the pull writes the
// whole velocity vector each frame. Because both ends run the identical rule off the identical
// replicated inputs, they agree to within one frame of gravity — MEASURED, and the number is in the
// report. It is not free: neither is saved-move state, so a corrected/replayed move re-derives
// velocity without the ability and the correction path absorbs the difference. That is a real, named
// limitation of this pass and the fix is a cross-file one.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetMace.generated.h"

class ATraceMaceSpike;
class UAudioComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
struct FHitResult;

/** Mace's bits in FTraceAbilityNetState::Flags. Only one character is live per component, so these cannot collide. */
namespace TraceMaceFlags
{
	/** Suspending: holding V in the air. TraceAbilityFlags::EffectActive. */
	inline constexpr uint8 Suspending = 1 << 0;
	/** Being pulled toward the spike. TraceAbilityFlags::MovementActive. */
	inline constexpr uint8 Pulling = 1 << 1;
	/** A spike is embedded and may be pulled on. TraceAbilityFlags::AuxActive. */
	inline constexpr uint8 SpikeEmbedded = 1 << 2;
	/** A spike is in flight and may NOT be pulled on yet. TraceAbilityFlags::Charged. */
	inline constexpr uint8 SpikeInFlight = 1 << 3;
}

UCLASS()
class TRACE_API UTraceAbilitySetMace : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Mace; }

	// --- lifecycle ---------------------------------------------------------------------------------
	virtual void OnEquipped() override;
	virtual void OnUnequipped() override;
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// --- the client FX router (FX_AUDIO_PLAN §1.2 / §2.4) ------------------------------------------
	//
	// Mace's two WHILE-ACTIVE tells — the suspend halo and the pull slip-stream — are third-person
	// presentation, so they live here and NOT in OnSecondaryPressed/RequestSpikePull. Both of those
	// run on one machine; these two hooks run on every machine off the same replicated flags the HUD
	// already reads, which is the only way an opponent watching Mace hover sees anything at all.
	virtual void OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New) override;
	virtual void SyncClientFx(const FTraceAbilityNetState& Current) override;

	// --- PASSIVE: the magnet ------------------------------------------------------------------------

	/**
	 * §6: "+30% magnet radius ... Derive it, do not hardcode." 1 + UTraceSettings::MaceMagnetRadiusBonus,
	 * read live, so 450 x 1.30 = 585 falls out of CoreCatchRadius and follows it if it is retuned.
	 *
	 * IT IS WIRED UP. ATraceCore::ServerApplyCatchZone multiplies its PER-CANDIDATE catch radius by
	 * UTraceAbilityComponent::GetMagnetRadiusMultiplierFor(Candidate) — per candidate and not once
	 * globally, because the magnet is a contest (spec v13 §5) and a single widened radius would have
	 * let Mace's bonus pull the Core toward her opponent as well. This comment used to say "nothing
	 * calls this yet", which was true when the character was written and is not true now; the call
	 * arrived with the integration seam (see TraceAbilityIntegration in TraceAbilityComponent.h) and
	 * is gated on Trace.Ability.Integration so the whole set of hooks can be shown failing at once.
	 * Trace.Mace.Verify still proves the multiplier in isolation; Trace.Integration.Verify and the
	 * MagnetWidenedFrames counter are what prove it is reached in a live match.
	 */
	virtual float GetMagnetRadiusMultiplier() const override;

	// --- MOVEMENT: hold V to suspend ----------------------------------------------------------------

	/** V pressed. Starts the suspend if she is airborne and off cooldown. Returns true if it started. */
	virtual bool OnSecondaryPressed() override;

	/** V released. "Releasing V cancels immediately and gravity resumes." */
	virtual void OnSecondaryReleased() override;

	// --- ACTIVATED: Spike ---------------------------------------------------------------------------

	virtual float GetActivatedCooldownSeconds() const override;
	virtual bool  CanActivate(FText& OutReason) const override;

	/** Throws the spike. Returns FALSE (a free fizzle, no cooldown) when the aim finds no wall. */
	virtual bool ActivateAbility() override;

	/**
	 * The REACTIVATION. Starts the pull toward an embedded spike.
	 *
	 * NOT reachable through TryActivate() and that is deliberate — see UTraceAbilityInputRelay's
	 * header. TryActivate() refuses while the cooldown the throw just started is running, and
	 * weakening that check for one character would put a second opinion into the one cooldown
	 * contract the whole framework rests on. The relay routes the press here instead.
	 *
	 * Safe to call on any machine. If the spike is still IN FLIGHT the press is REMEMBERED and the
	 * pull begins on the frame it lands — see bPullQueued.
	 */
	void RequestSpikePull();

	/**
	 * True when a press of E should be read as the reactivation rather than as a fresh throw.
	 *
	 * TRUE WHILE THE SPIKE IS STILL FLYING, TOO, and that is the point rather than an oversight. The
	 * alternative is handing the press back to the framework, where TryActivate() refuses it against
	 * the cooldown this very spike started — so a player who throws and immediately double-taps loses
	 * the input entirely. Routed here it is remembered instead.
	 */
	bool IsSpikeReadyToPull() const;

	/**
	 * Is there a spike, and has it landed? Reads the ACTOR on the server and the replicated state
	 * everywhere else, because only the server ever holds the pointer — see the block comment above
	 * these in the .cpp, which is the bug that made the reactivation unusable on a client.
	 */
	bool HasSpikeEmbedded() const;
	bool HasSpikeInFlight() const;

	/** Where the spike is (or is heading). Actor on the server, replicated AuxLocation elsewhere. */
	FVector GetSpikeAnchorLocation() const;

	/** Called by the spike actor the frame its flight ends. Server only. */
	void NotifySpikeEmbedded(ATraceMaceSpike* WhichSpike);

	// --- state, for the HUD and for Trace.Mace.Verify -------------------------------------------------

	bool IsSuspending() const { return bSuspending; }
	bool IsPulling() const { return bPulling; }
	ATraceMaceSpike* GetSpike() const { return Spike.Get(); }

	/** The momentum ceiling the pull runs at, derived per §6 from the air-strafe hard cap. */
	float GetPullSpeed() const;

	/** Seconds of suspend left, 0 when not suspending. For the HUD and the harness. */
	float GetSuspendRemaining() const;

	/**
	 * DEMO 17 item 6. Seconds until V will be accepted again, 0 when it is ready.
	 *
	 * *** FOR THE HARNESS AND THE LOG. NOT FOR THE HUD, AND THAT IS THE REQUIREMENT. *** Demo 17 asks
	 * for a cooldown the player is not shown ("do not show it on the HUD"), so this exists to make the
	 * rule TESTABLE, not to make it visible. Nothing in UI/ calls it; if something ever does, that is
	 * the change Demo 17 explicitly refused. The E ring is unaffected either way — this character does
	 * not override GetCharacterOwnedCooldownRemaining(), so the framework's ring never sees it.
	 *
	 * Counted from the END of the last suspend (release, expiry, landing, a pull, or death), because
	 * that is where UTraceAbilitySetMace::StopSuspend stamps it.
	 */
	float GetSuspendCooldownRemaining() const;

	/**
	 * HARNESS ONLY. Throws a spike at an explicit anchor, bypassing the aim sweep, so the spike and
	 * pull can be exercised without steering a pawn at a wall. Server only.
	 */
	ATraceMaceSpike* DebugThrowSpikeAt(const FVector& Anchor, float TravelSpeedOverride);

	/** HARNESS ONLY. The aim sweep on its own: "would a throw from here find a wall, and where?" */
	bool ResolveSpikeAnchor(FVector& OutAnchor, FVector& OutNormal, FString& OutWhy) const;

	/** The wall test, on one hit: "is this a surface a spike may embed in, and if not, why not?" */
	bool IsSpikeableSurface(const FHitResult& Hit, FString& OutWhy) const;

#if !UE_BUILD_SHIPPING
	/**
	 * DEV ONLY. One line describing the kit FX THIS MACHINE has attached to the pawn right now:
	 * which pieces exist, their measured radii read back off the live components, and whether the
	 * MacePullLoop audio component is actually playing.
	 *
	 * Read back rather than re-derived, for the reason every probe in this module gives: a fixture
	 * that recomputes the number the recipe asked for is checking its own arithmetic, not the thing
	 * on screen. It is also the only way the router half is falsifiable at all — an attached loop has
	 * no log line of its own once it is up.
	 */
	FString DebugDescribeKitFx() const;

	/** DEV ONLY. How many of this kit's loop primitives are attached on this machine (0..3). */
	int32 DebugAttachedFxCount() const;

	/**
	 * DEV ONLY. The suspend halo's local Z, uu, or 0 when it is not attached.
	 *
	 * The bob is the halo's whole content — §2.4 makes it MOTION precisely so that suspend is not a
	 * brightness pulse — and motion cannot be photographed. Sampling this over a second and reporting
	 * the SPREAD is the measurement; two readings could both land on the same phase of a 1.2 Hz sine
	 * and report a moving thing as still.
	 */
	double GetSuspendHaloLocalZ() const;

	/**
	 * DEV ONLY. The HUE a live dressing piece is actually wearing, read back off its own MID.
	 *
	 * *** THE ONE THING THE §2.4 PARADE COULD NOT SEE, AND THAT IS WHY A SECOND STALE VIOLET SURVIVED
	 * A WHOLE WAVE HERE. *** Trace.Mace.FxTest photographed the halo and both slip-streams, measured
	 * their radii, their bob and their audio, and asserted nine things about them — and never once
	 * said what COLOUR they came out. A file-local `MaceViolet` literal that had stopped matching the
	 * roster therefore passed every check the harness had. A frame is not enough either: a violet
	 * 13 degrees off the right violet looks like violet.
	 *
	 * @param Piece  "halo", "streamA" or "streamB".
	 * @param OutHue the accent as the MID holds it. These pieces are ADDITIVE, so intensity rides in
	 *               the colour (UTraceFxShapes::SetGlow) — the intensity is divided back out here so
	 *               the caller can compare against TraceCharacterRoster's row directly, which is the
	 *               comparison that matters. Returns false when the piece is not attached.
	 */
	bool DebugPieceHue(const TCHAR* Piece, FLinearColor& OutHue) const;
#endif

private:
	// --- suspend ------------------------------------------------------------------------------------
	bool  bSuspendHeld = false;
	bool  bSuspending = false;
	float SuspendEndMatchTime = 0.f;
	float SuspendReadyMatchTime = 0.f;

	void StartSuspend();
	void StopSuspend(const TCHAR* Why);
	void ApplySuspend(float DeltaSeconds);

	// --- spike / pull -------------------------------------------------------------------------------
	TWeakObjectPtr<ATraceMaceSpike> Spike;

	/** Absolute match time the 2 s embed window closes. Meaningless while the spike is in flight. */
	float SpikeEmbedEndMatchTime = 0.f;

	bool    bPulling = false;
	FVector PullAnchor = FVector::ZeroVector;
	FVector PullLastLocation = FVector::ZeroVector;

	/**
	 * The pull's first tick does not test for movement input.
	 *
	 * §6 says "any movement input cancels", and taken literally a player still holding W from the
	 * approach cancels the pull on the frame she starts it — the ability would be unusable in
	 * motion, which cannot be what "reactivating pulls her toward it" means. One frame of grace is
	 * the smallest thing that makes the rule mean "let go and then press" rather than "you must
	 * already be still". It is a frame, not a window: the second tick tests, and holding a key
	 * cancels then.
	 */
	bool bPullSkipInputTestThisTick = false;

	/**
	 * A reactivation pressed while the spike was still travelling, held until it lands.
	 *
	 * Nothing may pull on a spike that has not landed, and the framework cannot take the press either
	 * (TryActivate refuses it against the cooldown the throw just started), so before v15 §6 that
	 * press was simply gone. A player who throws to reach height and immediately double-taps does
	 * this every time; at 20000 uu/s the window is a third of a second, which is exactly the length
	 * of time a dropped input is blamed on the ability being "inconsistent".
	 *
	 * A REMEMBERED PRESS, NOT A TIMED WINDOW. Cleared by the pull starting, by the spike going away,
	 * and by ClearSpike (so a second throw cannot inherit the first throw's press), which means it
	 * can never outlive the throw that armed it and can never queue a pull she did not ask for.
	 */
	bool bPullQueued = false;

	void StartPull();
	void StopPull(const TCHAR* Why, bool bRemoveSpike);
	void ApplyPull(float DeltaSeconds);

	void ClearSpike();

	/** Server only. Mirrors the local state into the replicated scratch pad for the HUD and proxies. */
	void PublishState();

	/** True on the machines that actually simulate this pawn's movement: the server and its owner. */
	bool ShouldDriveMovement() const;

	// =============================================================================================
	// FX §2.4 — THE ATTACHED KIT FX. Every machine, driven from the replicated flags.
	// =============================================================================================
	//
	// Two effects, at most three primitive components, and they are mutually exclusive in practice:
	// StartPull() stops a suspend, so the halo is down before the slip-stream goes up. Even both at
	// once is 3, inside §1.4's budget of 4 — and every piece goes through
	// TraceFxLoopBudget::AttachLoopPrimitive, which is the thing that enforces that rather than this
	// comment.
	//
	// THE PAWN IS RESOLVED THROUGH THE PLAYER STATE, never cached across a respawn: an attached
	// component whose pawn has been replaced is a component nobody will ever clean up (§1.2 rule 1).

	/** Reads @p Which and attaches or detaches each effect to match it. Idempotent — the sync path. */
	void ApplyKitFx(const FTraceAbilityNetState& Which);

	/** The suspend halo: one additive disc under the feet, bobbing. Attach/detach, idempotent. */
	void SetSuspendFxAttached(bool bAttached);

	/** The pull slip-stream: two additive cylinders trailing the pawn, plus MacePullLoop. Idempotent. */
	void SetPullFxAttached(bool bAttached);

	/** Everything down, in one call: death, unequip, half time, characters disabled, pawn gone. */
	void DetachAllKitFx();

	/** Per-tick motion — the halo's bob and the slip-stream's aim. Runs on EVERY machine. */
	void TickKitFx(float DeltaSeconds);

	/** The pawn the FX belong on: the player state's CURRENT pawn, or null. Never a cached one. */
	ATraceCharacter* ResolveFxPawn() const;

	/** Which pawn the attached pieces are actually on, so a respawn under us is detectable. */
	TWeakObjectPtr<ATraceCharacter> FxPawn;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> SuspendHalo = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SuspendHaloMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> PullStreamA = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PullStreamAMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> PullStreamB = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PullStreamBMID = nullptr;

	/**
	 * MacePullLoop, started by the router on every machine and OWNED here.
	 *
	 * TraceAudio::StartLoopOn hands back a component with bAutoDestroy off, so a loop that is never
	 * faded leaks until the pawn dies — the caller keeping the pointer IS the contract (§1.6.4).
	 */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> PullLoopAudio = nullptr;

	/** Seconds the halo has been up, for the bob. Absolute time would jump when the halo re-attaches. */
	float SuspendFxSeconds = 0.f;
};
