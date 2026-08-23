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
 * AMMO'S RESOLVED NUMBERS AND ITS ALARMS (spec v16 §1).
 *
 * Same shape as TraceVulnerable in Gameplay/TraceHealthComponent.h and for the same two reasons: the
 * .ini can win over the header, so every reader must go through one clamped accessor rather than
 * touching UTraceSettings directly; and a rule worth stating is a rule worth COUNTING, so a harness
 * can measure it instead of asserting something about code somebody has read.
 *
 * THE COUNTERS SPLIT INTO ALARMS AND LIVENESS, deliberately:
 *
 *   GetCarrierRoundsConsumed()  *** ALARM. *** A round was taken out of a clip belonging to the Core
 *                               carrier. Spec v16 §1: "The Core carrier has no gun, so ammo must not
 *                               be consumed or shown while carrying." Zero for the life of a correct
 *                               process; only reachable with Trace.Ammo.CarrierGuard 0.
 *   GetRoundsConsumed()         LIVENESS. Rounds actually spent, ever, on this machine.
 *   GetReloadsCompleted()       LIVENESS. Reloads that actually finished.
 *   GetDryFireRefusals()        how often CanFire() said no because the clip was empty.
 *   GetReloadFireRefusals()     how often CanFire() said no because a reload was running.
 *
 * The liveness half is the other side of the evidence: a harness that only ever reads zeros cannot
 * tell "the carrier rule held" from "nothing ever fired".
 */
namespace TraceAmmo
{
	/**
	 * Rounds per clip for the PISTOL: UTraceSettings::ClipSize, clamped. Shipped 30.
	 *
	 * *** THE NO-ARGUMENT FORM IS THE PISTOL'S AND ALWAYS WAS. *** Spec v28 §9 added a second gun
	 * with its own 40-round clip, so every reader that has a WEAPON in hand should call the overload
	 * below (or, better, UTraceWeaponComponent::GetClipSize(), which asks the pawn's own selector).
	 * This form is kept, unchanged, because five call sites outside this slice pass no weapon and
	 * mean the pistol: X's Sting harness, Roxie's, the reload probe in TracePlayerController and the
	 * HUD's reload arc. Changing its meaning would have silently retuned all of them.
	 */
	TRACE_API int32 GetClipSize();

	/** Seconds a PISTOL reload takes: UTraceSettings::ReloadSeconds, clamped. Shipped 0.5. */
	TRACE_API float GetReloadSeconds();

	/**
	 * SPEC v28 §9. The same two numbers, for whichever weapon is being asked about.
	 *
	 * The knife has neither a clip nor a reload; it resolves to the pistol's values so that a caller
	 * which asks about a knife-holding pawn gets a sane denominator rather than a zero. Nothing gates
	 * on that — ShouldShowAmmo() is already false with a knife out.
	 */
	TRACE_API int32 GetClipSize(ETraceEquippedWeapon Weapon);
	TRACE_API float GetReloadSeconds(ETraceEquippedWeapon Weapon);

	/**
	 * SPEC v28 §9. Seconds between rounds for @p Weapon, BEFORE the per-character scale.
	 *
	 * *** THE SCALE IS DELIBERATELY NOT APPLIED HERE. *** UTraceAbilityComponent::GetFireIntervalScaleFor()
	 * needs the shooting ACTOR, which this namespace does not have, and folding it in would have
	 * meant two functions that both look like "the fire interval" with only one of them honouring
	 * Roxie. The two call sites that gate a shot (CanFire and ServerFire) both multiply by the scale
	 * on the very next line; see UTraceWeaponComponent::GetFireInterval(), which is the form with the
	 * pawn and is what everything else should use.
	 */
	TRACE_API float GetBaseFireInterval(ETraceEquippedWeapon Weapon);

	/**
	 * SPEC v28 §9. Positional damage for @p Weapon: the pistol's 100/40/25, the SMG's 33/18/12.
	 *
	 * THE NO-DISTANCE FORM IS POINT BLANK and is what the dumps and the harness's table checks use.
	 * Since spec v29 §2d the SMG's payout depends on range, so the SHOT must call the overload below;
	 * this form deliberately answers for range 0 rather than silently picking a table.
	 */
	TRACE_API float GetZoneDamage(ETraceEquippedWeapon Weapon, ETraceHitZone Zone);

	/**
	 * SPEC v29 §2d. The same number, priced at the range the bullet actually travelled.
	 *
	 * @param DistanceUU  muzzle-to-impact, in uu, as the SERVER resolved it.
	 *
	 * Identical to the form above for the pistol and the knife at every range — §2d is SMG-only. For
	 * the SMG it is the near table at or inside UTraceSettings::SmgFalloffStartUU and the far table
	 * past it, with SmgFalloffRampUU deciding whether the change is a cliff (0, which is what ships)
	 * or a linear ramp of that length.
	 */
	TRACE_API float GetZoneDamage(ETraceEquippedWeapon Weapon, ETraceHitZone Zone, double DistanceUU);

	/**
	 * SPEC v29 §2d. 0 at point blank, 1 past the falloff, and the blend in between when a ramp is
	 * configured. Exposed so the harness and the dumps can print the resolved CURVE rather than two
	 * numbers and a hope. Always 0 for the pistol and the knife, and always 0 when the falloff is off.
	 */
	TRACE_API float GetFalloffAlpha(ETraceEquippedWeapon Weapon, double DistanceUU);

	/**
	 * SPEC v29 §2b. True when a HELD trigger keeps feeding @p Weapon.
	 *
	 * False for the pistol as of §2b ("It must fire once per trigger press"), true for the SMG, and
	 * false for the knife, which has no trigger — the knife's repeat is CanSwing()'s own cadence and
	 * is deliberately untouched by fire mode.
	 *
	 * *** THIS IS THE BASE ANSWER AND NOT THE FINAL ONE. *** Roxie's MODDED forces full auto on
	 * whatever is in her hands (spec v18 §2, "the gun becomes full auto"); the pawn-aware form is
	 * UTraceWeaponComponent::IsFullAutoNow(), which is what the trigger loop actually asks.
	 */
	TRACE_API bool IsFullAuto(ETraceEquippedWeapon Weapon);

	/** True when the mechanic is armed (Trace.Ammo.Enabled). 0 is the RED arm: the clip never falls. */
	TRACE_API bool IsEnabled();

	/** True when a running reload refuses fire (Trace.Ammo.ReloadBlocksFire). 0 is the RED arm. */
	TRACE_API bool DoesReloadBlockFire();

	/** True when the carrier guard inside the consumption path is armed. 0 is the RED arm. */
	TRACE_API bool IsCarrierGuardArmed();

	/**
	 * True when an owning client predicts its own rounds (Trace.Ammo.Predict). 0 is the RED arm for
	 * spec v16 §1's "the local player must see their own count drop immediately": the client then
	 * waits for replication and its number lags its own muzzle flash by a round trip.
	 */
	TRACE_API bool IsPredictionEnabled();

	TRACE_API int32 GetCarrierRoundsConsumed();
	TRACE_API int32 GetRoundsConsumed();
	TRACE_API int32 GetReloadsCompleted();
	TRACE_API int32 GetDryFireRefusals();
	TRACE_API int32 GetReloadFireRefusals();
	TRACE_API void  ResetCounters();
}

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
	 * the knife is equipped, WITH AN EMPTY CLIP, MID-RELOAD (spec v16 §1), or while the local
	 * fire-rate gate is closed.
	 */
	bool CanFire() const;

	// =============================================================================================
	// AMMO  (spec v16 §1). Read the block comment on GetClipAmmo() before changing any of it.
	// =============================================================================================

	/**
	 * *** WHAT THIS MACHINE BELIEVES IS IN THE CLIP, AND THE ONE ACCESSOR EVERYTHING SHOULD USE. ***
	 *
	 * On the AUTHORITY it is ClipAmmo, which is the truth. On a remote owning client it is the
	 * PREDICTED count, which is the truth minus the shots that have not reached the server yet — and
	 * that is exactly what spec v16 §1 asks for: "the local player must see their own count drop
	 * immediately." A client that waited for the round trip would watch 30 stay on screen for a whole
	 * ping after the muzzle flashed.
	 *
	 * On a SIMULATED PROXY (somebody else's pawn on your machine) it is meaningless and reads as a
	 * full clip: ClipAmmo replicates to the owner only, because nothing in this game draws another
	 * player's ammo. Do not build a feature on another pawn's count without changing that condition
	 * first.
	 */
	int32 GetClipAmmo() const;

	/**
	 * Rounds a full clip of THE WEAPON THIS PAWN IS HOLDING holds. Pistol 30, SMG 40 (spec v28 §9).
	 *
	 * This is the accessor the HUD already calls for its denominator, so the "/40" beside an SMG
	 * needed no HUD change at all — which is the whole reason the weapon-aware answer was put on the
	 * component rather than only on the TraceAmmo overload.
	 */
	int32 GetClipSize() const { return TraceAmmo::GetClipSize(EquippedWeapon); }

	/**
	 * THE SIZE OF THE MAGAZINE THAT IS ACTUALLY IN THE GUN, which is not always the size of the
	 * magazine for the weapon currently SELECTED.
	 *
	 * *** THIS EXISTS BECAUSE THE SMG WAS PERMANENTLY LOSING TEN ROUNDS. *** A reload runs on a
	 * server-side deadline and completes wherever the player happens to be when it expires. Switch
	 * to the knife mid-reload and the refill asked GetClipSize(), which answers for the SELECTED
	 * weapon — so a 40-round SMG magazine was refilled to the pistol's 30 and the missing ten never
	 * came back.
	 *
	 * The live magazine belongs to LiveClipOwner by definition (that is the whole reason it exists,
	 * from spec v29 §5's stow state), so ask it rather than the selector.
	 */
	int32 GetLiveClipSize() const { return TraceAmmo::GetClipSize(LiveClipOwner); }

	/** Seconds a reload of the weapon in hand takes. Pistol 0.5, SMG 0.8 (spec v28 §9). */
	float GetReloadSeconds() const { return TraceAmmo::GetReloadSeconds(EquippedWeapon); }

	/**
	 * SECONDS BETWEEN ROUNDS FOR THIS PAWN, RIGHT NOW — the base interval of the weapon in hand,
	 * multiplied by this pawn's per-character scale.
	 *
	 * *** THIS IS THE STANDING RULE, IN ONE FUNCTION, AND IT IS WHY THE SMG INHERITED ROXIE AND
	 * SLIMEBALL FOR FREE. *** Spec v28 §9: "The existing per-character fire-rate modifiers must apply
	 * to the SMG the same way they apply to the pistol." Those modifiers are stored as RATE
	 * MULTIPLIERS and reach the gun as a SCALE ON THE INTERVAL through
	 * UTraceAbilityComponent::GetFireIntervalScaleFor() — a number below 1 for a faster gun. Because
	 * the scale multiplies whatever base it is given, pointing the base at SmgFireInterval was the
	 * entire change: Roxie's SMG is 0.1 / 1.65 = 0.0606 s = 990 RPM, and a stuck Slimeball's is
	 * 0.1 / 1.3 = 0.0769 s = 780 RPM, without either ability learning that a second gun exists.
	 *
	 * Both gates call this — the client's CanFire() and the server's ServerFire() — so a scaled
	 * client and an unscaled server can never disagree about what a legal cadence is. That exact
	 * disagreement was the spec v18 §2 bug: it read in game as the gun eating every second bullet.
	 */
	double GetFireInterval() const;

	/**
	 * SPEC v29 §2b. Does a HELD trigger keep feeding the weapon in this pawn's hands right now?
	 *
	 * TraceAmmo::IsFullAuto() is the weapon's own answer (pistol no, SMG yes) and this is that answer
	 * OR'd with the abilities that force full auto — which today is Roxie's MODDED, whose §2 clause
	 * "the gun becomes full auto" finally has something to switch now that a gun exists which is not.
	 *
	 * Asked once per frame by TickComponent, and by the harness. It does NOT gate the first round of
	 * a press: StartFire() always fires one, whatever the mode says. Semi-automatic is a rule about
	 * the REPEAT, and writing it that way is what keeps a semi-automatic gun's first shot identical to
	 * an automatic one's.
	 */
	bool IsFullAutoNow() const;

	/**
	 * SPEC v29 §2f. Local-clock instants of the last rounds this machine fired, oldest first.
	 *
	 * The measurement surface for the 537-RPM bug and nothing else: FireOnce appends one double per
	 * round while Trace.Weapons.RecordShots is on, so a harness can report a real distribution over
	 * 40+ rounds instead of the two-sample "interval" a before/after pair of timestamps would give.
	 * Empty (and never written) with the cvar off, which is the shipping state.
	 */
	const TArray<double>& GetRecordedShotTimes() const { return RecordedShotTimes; }

	/** SPEC v29 §2f. Drops every recorded stamp. The harness calls it between arms. */
	void ClearRecordedShotTimes() { RecordedShotTimes.Reset(); }

	/**
	 * SPEC v28 §10. Seconds of shooting lockout still owed by a swing; 0 when the gun is free.
	 *
	 * "Meleeing should lock the player out of shooting for the length of the animation." The length
	 * IS UTraceMeleeSettings::SwingAnimSeconds — not a copy of it — so retuning the animation moves
	 * the lockout with it and the two can never disagree about how long a swing looks like it lasts.
	 *
	 * TWO CLOCKS, MAXED, BECAUSE TWO MACHINES OWN TWO DIFFERENT FACTS. The swinging machine stamps
	 * SwingAnimStartLocalTime at the PRESS. The server only learns about the swing when ServerSwing
	 * arrives, which is at the RESOLVE — press + SwingWindupSeconds — so its own deadline is
	 * LastAcceptedSwingTime + (anim - windup). Both are derived from the same two knobs and land on
	 * the same instant in the absence of lag; taking the max means a client can never shorten its own
	 * lockout by lying about when it swung. On a listen host and on a bot both stamps are set in the
	 * same process and the max is simply the same number twice.
	 */
	float GetShootLockoutRemaining() const;

	/**
	 * True while the 0.5 s reload is running. Shared clock, so the client that predicted the reload
	 * and the server that validates against it compute the same answer — the same contract
	 * IsDeploying() has for the pullout, and for the same reason.
	 */
	bool IsReloading() const;

	/** Seconds of reload left; 0 when the gun is up. HUD and diagnostics. */
	float GetReloadRemaining() const;

	/**
	 * How many of the rounds in the clip were loaded by an ABILITY rather than by an ordinary reload.
	 * Zero for every ordinary clip.
	 *
	 * Spec v16 §1: "X's Sting ability reloads the clip with just the 5 bee bullets; after shooting
	 * those, his next reload is normal." The gun deliberately does NOT know what an ability round
	 * DOES — that is the ability layer's business — only that this clip contains some, so the HUD can
	 * say so and so the ability can ask whether the round that just left was one of them.
	 */
	int32 GetAbilityRoundsInClip() const;

	/**
	 * True when the LAST round this weapon consumed came out of the ability-loaded part of the clip.
	 *
	 * SERVER-SIDE AND READ IMMEDIATELY AFTER THE SHOT, which is the only window in which it means
	 * anything: UTraceAbilitySetX::NotifyBulletHit is called from ServerFire a few statements later,
	 * and it needs to know whether the bullet that just landed was a bee round. Asking "are there
	 * ability rounds left" instead would get the wrong answer on the fifth and last one.
	 */
	bool WasLastRoundAbilityRound() const { return bLastRoundWasAbilityRound; }

	/**
	 * True when this pawn's ammo should appear on screen at all.
	 *
	 * FALSE WHILE CARRYING THE CORE — spec v16 §1, verbatim: "The Core carrier has no gun, so ammo
	 * must not be consumed or shown while carrying." Also false with the knife out (it has no
	 * magazine) and while dead. This is a PRESENTATION question and it is answered here rather than
	 * in the HUD so that the "has no gun" rule has one definition instead of two.
	 */
	bool ShouldShowAmmo() const;

	/**
	 * MANUAL RELOAD — the R bind (ETraceInputAction::Reload). Returns true when a reload started.
	 *
	 * Refuses, and each refusal is a rule rather than an oversight:
	 *   - a FULL clip. [ASSUMPTION] (spec v16 §1): pressing R with 30 rounds does nothing at all. The
	 *     alternative — restarting the timer — would let a player lock their own gun for half a
	 *     second by leaning on the key, which no shooter does;
	 *   - a reload already running, for the same reason: R is not a way to extend it;
	 *   - dead, carrying the Core, mid-pullout, or with the knife out.
	 *
	 * Predicts locally on an owning client and asks the server; acts directly on the server. The
	 * client stamps the PRESS on the shared clock and ships it, so both machines anchor the same
	 * deadline — see ServerRequestReload for the measurement that made that necessary for the pullout.
	 */
	bool RequestReload();

	/**
	 * @param ClientPressServerTime shared-clock timestamp of the PRESS, from the pressing machine.
	 *
	 * THE STAMP IS WHY A CLIENT'S RELOAD IS 0.5 s AND NOT 0.5 s PLUS THEIR PING, and it is the same
	 * trick ServerRequestEquip plays for the 0.2 s pullout — where it was MEASURED turning 0.2 s into
	 * 0.294 s at 40 ms of latency before the parameter existed. The clamp is the security: whatever
	 * the client claims, the press is pinned into [ServerNow - MaxRewindTime, ServerNow], so a reload
	 * can neither be pre-booked nor back-dated past the point where it would already be finished.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestReload(float ClientPressServerTime);

	/**
	 * SERVER ONLY. Replaces the whole clip with @p RoundCount ability-loaded rounds.
	 *
	 * Spec v16 §1, X's Sting: "reloads the clip with just the 5 bee bullets". It REPLACES rather than
	 * adds — [ASSUMPTION], stated in the spec — so 20 ordinary rounds become 5 bee rounds and the
	 * count on the HUD goes DOWN. Any reload in progress is cancelled; the clip is up immediately,
	 * because the ability's own cast is the cost and charging a second 0.5 s on top would make Sting
	 * feel like a punishment.
	 *
	 * The gun does not know what an ability round does, and this is deliberately not called
	 * "LoadBeeRounds" — see GetAbilityRoundsInClip() and Abilities/Characters/TraceAbilityWeaponHooks.h
	 * for why the weapon stays character-agnostic.
	 */
	void LoadAbilityClip(int32 RoundCount);

	UFUNCTION()
	void OnRep_Ammo();

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

	/**
	 * "Is the knife the SELECTED weapon" — i.e. is no gun out, and can this pawn therefore not fire.
	 *
	 * *** [DUALWIELD] ALWAYS FALSE WHEN THE SPEC v28 §10 SWITCH IS ON, BY CONSTRUCTION RATHER THAN BY
	 * A TEST: *** no path could put ETraceEquippedWeapon::Knife in the selector under v28 §10. Every
	 * caller of this outside the melee slice means "cannot shoot right now" (X's Sting, Roxie's
	 * Modded, the ability reload hook, ShouldShowAmmo, the HUD), and under dual-wield a pawn can
	 * always shoot, so false was the right answer for all of them. Use TraceMelee::IsKnifeInHand()
	 * when the question is whether a blade is available to swing.
	 *
	 * *** SINCE SPEC v31 §1 THE SWITCH IS OFF AND THIS IS TRUE AGAIN — in the knife slot, key 3. ***
	 * The clause above is kept rather than deleted because the switch's true arm is intact and a
	 * reader who flips it back needs to know this predicate goes dead again when they do.
	 */
	bool IsKnifeEquipped() const { return EquippedWeapon == ETraceEquippedWeapon::Knife; }

	/** SPEC v28 §9. True while the SMG is the selected weapon. */
	bool IsSmgEquipped() const { return EquippedWeapon == ETraceEquippedWeapon::Smg; }

	/** True for any weapon that fires bullets. This is what almost every gate actually wants. */
	bool IsFirearmEquipped() const { return TraceIsFirearm(EquippedWeapon); }

	/** True inside the 0.2 s pullout, during which neither weapon works. Shared clock, so it is the
	 *  same answer on the client that predicted the swap and on the server that validates it. */
	bool IsDeploying() const;

	/** Seconds left of the pullout; 0 when the weapon is up. HUD and diagnostics. */
	float GetDeployRemaining() const;

	/** Seconds until the next swing is legal; 0 when it is legal now. HUD and bots. */
	float GetSwingCooldownRemaining() const;

	// DoSwapWeaponPressed() USED TO SIT HERE and is deleted (spec v15 §5). It was "the swap bind" —
	// the component-level toggle handler — and it had already lost its last caller before §5, because
	// TraceMelee::RequestSwapWeapon goes straight to RequestEquip. With the F bind gone there is no
	// key that could ever reach it. The toggle VERB still exists, once, at TraceMelee.h's
	// RequestSwapWeapon, which is where the console commands and the bots find it.

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
	 *   RequestEquip is UNGUARDED. A redundant request is a second intent, so it costs a pullout —
	 *                  swallowing it silently would make a double-tap feel like a dropped input.
	 *                  RequestEquip's own doc comment commits to that. It is no longer on a key
	 *                  (spec v15 §5 deleted the SwapWeapon toggle that used to be F), but it is still
	 *                  what the bots, the dev console, TraceMelee::RequestSwapWeapon and the v13 §2
	 *                  harness's red arm all call, so the behaviour is load-bearing.
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
	 * "MAY THIS PAWN'S WEAPON ACT WITHOUT BEING TOLD TO?" Default yes; false makes it a TARGET.
	 *
	 * DEMO 19 ITEM 2, verbatim: "Don't let the bots attack." The practice range's five targets are
	 * possessed by a bare ATracePracticeDummyController that deliberately steers nothing and presses
	 * nothing — but TickBotKnife() below does not consult the controller's intentions. It fires for
	 * ANY pawn whose controller is not an APlayerController, so the range's targets were drawing the
	 * knife and swinging it at whatever came within reach, which is exactly the "bots attacking" the
	 * user reported and precisely what a row of stationary targets must not do.
	 *
	 * WHY A PER-PAWN BIT RATHER THAN Trace.Knife.BotAuto 0. The cvar is global: switching it off in
	 * the range would also silence the bots of a range deliberately opened with `?bots=5`, and it
	 * would silence them in a real match on the same machine. This bit is a property of the PAWN, so
	 * "this body is a target" travels with the body and nothing else changes.
	 *
	 * WHY IT LIVES ON THE WEAPON AND NOT ON THE AI. The autonomy being switched off is this
	 * component's own (TickBotKnife is a melee-slice behaviour, not an ATraceBotController one), so
	 * the switch belongs at the thing it switches. The AI slice stays untouched.
	 *
	 * NOT REPLICATED and not saved: it only gates a server-side tick, and a target's pawn is rebuilt
	 * from scratch on every respawn — ATracePracticeDummyController::OnPossess re-applies it there,
	 * which is the only place it is ever set.
	 */
	void SetAutonomousAttacksAllowed(bool bAllowed) { bAutonomousAttacksAllowed = bAllowed; }
	bool AreAutonomousAttacksAllowed() const { return bAutonomousAttacksAllowed; }

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

	/**
	 * *** SPEC v29 §2f — THE 537 RPM FIX, IN ONE FUNCTION. ***
	 *
	 * Moves LastLocalFireTime on for the round that is being fired right now, and it is the ONLY
	 * writer of that field.
	 *
	 * WHAT WAS WRONG. It used to be `LastLocalFireTime = GetLocalTimeSeconds()` — the FRAME's time,
	 * not the time the round was DUE. The fire poll is a tick, so a round can only leave on a frame
	 * boundary; stamping the frame throws away however far past due it already was, and the next
	 * round starts its wait from there. The cadence is then dt * ceil(Interval / dt), not Interval,
	 * and the error is one frame per round rather than one frame per burst:
	 *
	 *     dt = 1/53.7 s : 0.018617 * ceil(0.1 / 0.018617) = 0.11170 s = 537 RPM against a 600 RPM knob
	 *     dt = 1/53.7 s : 0.018617 * ceil(0.315789 / 0.018617) = 0.31649 s = 189.6 RPM against 190
	 *
	 * i.e. exactly the measured 0.1117 s, and exactly why the pistol never looked broken. THE FASTER
	 * THE GUN, THE WORSE IT IS, so a 600 RPM weapon is what made a bug that has been in this file
	 * since v5 finally visible.
	 *
	 * WHAT IT DOES INSTEAD. It keeps the overshoot — the amount by which this frame is LATE for the
	 * round it is delivering — up to a cap, so the next round is due one exact interval after the
	 * round that was DUE rather than after the frame that happened to carry it. Over a burst the mean
	 * converges on the knob at any frame rate; the individual gaps still land on frames, because they
	 * have to.
	 *
	 * WHY THERE IS A CAP AT ALL, AND WHY IT IS THE ONE IT IS. Uncapped, a burst that resumed after a
	 * stall would bank credit and pay it out as a machine-gun catch-up, and — worse — could ask for a
	 * gap the SERVER rejects as rate-limited, which reads in game as the gun eating bullets. The cap
	 * is UTraceSettings::FireIntervalCarryFraction of the interval, clamped in the implementation to
	 * FireRateTolerance, which is the fraction the server itself forgives. The client can therefore
	 * never carry more than the server already tolerates: the two numbers are one rule.
	 *
	 * NOT A SECOND CLOCK. There is no accumulator to reset, no state to leak and nothing to
	 * re-initialise on a respawn, a swap or a reload: the carry lives entirely in the one field the
	 * gate already read, and a gap longer than one interval plus the cap collapses back to "stamp
	 * now" on its own.
	 */
	void AdvanceFireClock();

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

public:
	/**
	 * *** SPEC v29 §2e — HOW HARD THIS PAWN'S GUN KICKS, AS A MULTIPLE OF RecoilPitchPerShot. ***
	 *
	 * "Roxie's modded should add recoil now." Demo 22 (spec v25 §5) removed the aim punch globally
	 * and bRecoilEnabled is still FALSE; this is not a partial revert of that. It is a SUM:
	 *
	 *     scale = (bRecoilEnabled ? 1 : 0) + (MODDED up ? RoxieModdedRecoilScale : 0)
	 *
	 * so with recoil off the answer is 0 for every pawn in the game and ApplyRecoilKick still returns
	 * on its first test — except for a Roxie with MODDED up, who gets exactly one base kick's worth,
	 * and only while it is up. Set bRecoilEnabled back to true and everybody kicks while she kicks
	 * (1 + scale) times as hard, so the sentence "MODDED costs you recoil" stays true in both worlds.
	 * An OVERRIDE would have deleted her trade at the exact moment a designer turned recoil back on.
	 *
	 * *** RELATIVE, NOT ABSOLUTE (the standing rule). *** It is a MULTIPLE of the base per-shot kick,
	 * never a number of degrees, so retuning RecoilPitchPerShot moves Roxie in proportion with
	 * nothing to re-derive. Everything else about the model — growth, ceiling, recovery, burst reset,
	 * compensation — is shared and is deliberately not duplicated per character.
	 *
	 * Public so Trace.Weapons.V29 can assert the seam without firing a round, and so the HUD could
	 * show the trade if it ever wants to. Correct on any machine that has the pawn: the MODDED flag
	 * is replicated.
	 */
	double GetRecoilPitchScale() const;

private:

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

	/**
	 * SPEC v29 §2b. Has the trigger already spent its one round for this press?
	 *
	 * Set by FireOnce, cleared by StopFire — i.e. by the RELEASE and by nothing else. A semi-automatic
	 * weapon refuses the tick's repeat while this is true, so "one shot per trigger press" is a fact
	 * about the press rather than a second cooldown that could drift out of step with the fire rate.
	 *
	 * Deliberately NOT cleared on a weapon swap, a death or a Core pickup: all three go through a
	 * state the trigger cannot fire from anyway, and clearing it would hand a player holding the
	 * button a free extra round on the frame the gate reopened.
	 */
	bool bTriggerConsumedThisPress = false;

	/** Local-clock time of the last shot this machine predicted. */
	double LastLocalFireTime = -1000.0;

	/**
	 * SPEC v29 §2f. Local-clock instants of rounds fired, while Trace.Weapons.RecordShots is on.
	 *
	 * Off by default and never allocated in a shipping run. Capped at 4096 stamps (32 KB) so a
	 * harness that forgets to turn it off cannot grow it without bound over a match.
	 */
	TArray<double> RecordedShotTimes;

	/** Local-clock time of the last shot the server accepted. Authority only. */
	double LastAcceptedFireTime = -1000.0;

public:
	/**
	 * Fraction of FireInterval the server forgives. Honest clients time their shots against their
	 * own smoothed copy of the server clock and their packets arrive jittered and occasionally
	 * bunched, so a strict >= FireInterval test on arrival times punishes exactly the players we
	 * are trying to serve. Cheating past this buys ~20% more DPS, not an aimbot.
	 *
	 * PUBLIC AS OF SPEC v29 §2f, and for one reason: it is now a SECOND rule as well as a gate. The
	 * client's fire-clock carry (AdvanceFireClock) is clamped to it, because a client that carried
	 * more than the server forgives would ask for rounds the server rejects. Two places rely on one
	 * number, so the number has to be readable from both — including from Trace.Weapons.V29, which
	 * asserts that no measured gap ever falls under it.
	 */
	static constexpr double FireRateTolerance = 0.2;

private:

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

	// =============================================================================================
	// AMMO STATE  (spec v16 §1)
	//
	// FOUR REPLICATED FACTS AND TWO PREDICTION MIRRORS, and the split is the whole design:
	//
	//   ClipAmmo          the truth. Written ONLY on the authority, ONLY by ConsumeRound/RefillClip.
	//   ClipSerial        ++ on every REFILL. It is what lets a client tell "the server reloaded"
	//                     apart from "the server is simply behind my predicted shots".
	//   AbilityRoundsInClip  how many of ClipAmmo came from an ability (X's Sting).
	//   ReloadEndServerTime  the deadline, on the SHARED clock — a deadline rather than a countdown,
	//                     for the reason DeployEndServerTime's comment gives, and anchored at the
	//                     stamped press/shot so the client's prediction and the server's copy are the
	//                     same number rather than two numbers a ping apart.
	//
	//   PredictedClipAmmo / PredictedClipSerial   the OWNING CLIENT's copy, and nothing else's.
	//
	// WHY THE PREDICTION IS A SEPARATE FIELD RATHER THAN A LOCAL WRITE TO ClipAmmo. A local write is
	// what a replicated property update is guaranteed to undo: the shooter fires, predicts 29, and
	// the packet the server sent a moment BEFORE it heard about that shot arrives carrying 30. The
	// count would bounce 30 -> 29 -> 30 -> 29, which reads as the gun refunding a bullet. Keeping the
	// prediction beside the truth means OnRep_Ammo can reconcile the two with a rule (see there)
	// instead of being overwritten by whichever packet landed last.
	//
	// AND WHY THE SERVER DOES NOT PREDICT AT ALL. On the authority — a listen host's own pawn, and
	// every bot — FireOnce() and ServerFire_Implementation() both run in this same process, so a
	// decrement in each would spend two rounds per trigger pull. ConsumeRound is therefore called
	// from exactly one side of that pair on each machine: the authority spends in ServerFire, a
	// remote client predicts in FireOnce. GetClipAmmo() picks the right one to read.
	// =============================================================================================

	/** SERVER TRUTH. Rounds in the clip. See the block comment above before writing to it. */
	UPROPERTY(ReplicatedUsing = OnRep_Ammo)
	uint8 ClipAmmo = 0;

	/**
	 * Incremented by RefillClip() on every server-side refill (a finished reload, an ability clip, a
	 * respawn). Wraps at 255 and that is harmless: the reconcile only ever asks whether it CHANGED.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Ammo)
	uint8 ClipSerial = 0;

	/** How many of ClipAmmo are ability-loaded rounds (X's Sting). Always <= ClipAmmo. */
	UPROPERTY(Replicated)
	uint8 AbilityRoundsInClip = 0;

	/**
	 * Shared-clock instant the reload finishes. Negative means "not reloading".
	 *
	 * WRITTEN BY BOTH ENDS AND THAT IS SAFE, exactly as DeployEndServerTime is: both anchor at the
	 * same stamped instant, so the replicated update is a no-op rather than an extension, and a
	 * client can never predict a deadline EARLIER than the one the server will compute.
	 */
	UPROPERTY(Replicated)
	float ReloadEndServerTime = -1.f;

	// --- THE STOWED GUN'S CLIP  (spec v28 §9) -----------------------------------------------------
	//
	// *** EACH GUN REMEMBERS ITS OWN MAGAZINE, AND THE ALTERNATIVES ARE BOTH VISIBLY WRONG. ***
	//
	// One shared counter would print "40/30" the moment a full SMG was swapped for the pistol, and
	// clamping it down to 30 would eat ten rounds every time a player touched the other key — a gun
	// that silently confiscates ammunition is the kind of thing that gets reported as "the SMG is
	// bugged". Refilling on every draw would be worse still: the swap costs 0.2 s and a reload costs
	// 0.5-0.8 s, so tapping 1-2 would be a reload that is two to four times faster than reloading.
	//
	// ONE STOW SLOT IS ENOUGH BECAUSE THERE ARE EXACTLY TWO GUNS. ApplyEquip exchanges the live pair
	// with this pair on any gun-to-gun transition and leaves both alone otherwise — a knife swap in
	// the legacy build must NOT disturb the gun's magazine, which is the v16 §1 behaviour and is
	// preserved by that condition. Adding a third gun means turning this into a small array; the
	// exchange is a single named function (SwapStowedClip) so that is a local change.
	//
	// COND_OwnerOnly, like the four fields above it and for the identical reason: nothing in this
	// game draws another player's ammo, and this is two more bytes nobody can see.
	//
	// THE SERIAL DOES THE RECONCILING. ApplyEquip bumps ClipSerial on the authority, so an owning
	// client's OnRep_Ammo takes the "a NEW clip — throw the prediction away" branch it already had.
	// That is why the swap needed no new reconciliation rule.

	/**
	 * WHICH FIREARM THE LIVE MAGAZINE BELONGS TO.
	 *
	 * *** THIS EXISTS BECAUSE "SWAP WHEN THE PREVIOUS AND DESIRED ARE BOTH GUNS" IS WRONG THE MOMENT
	 * A THIRD SELECTOR VALUE IS REACHABLE. *** Spec v29 §5 made it reachable as the stow state:
	 * pistol -> stow -> SMG is two transitions and NEITHER of them is gun-to-gun, so no swap fired
	 * and the SMG came out holding the pistol's magazine. Ammo counts were being shuffled between
	 * weapons by a route the old condition could not see.
	 *
	 * The question that actually decides a swap is not "where did we come from" but "does the
	 * magazine in the gun belong to the gun being drawn". This answers that directly, so any route
	 * between two firearms — however many intermediate states are in the middle — swaps exactly once.
	 *
	 * *** SPEC v31 §1 REMOVED THE STOW STATE AND THIS FIELD IS STILL LOAD-BEARING. CHECKED, NOT
	 * ASSUMED. *** The stow state is gone as a NAME; the third selector value is not, because the
	 * knife is a weapon slot again and ETraceEquippedWeapon::Knife is the same enumerator it always
	 * was. Pistol -> KNIFE -> SMG is the identical two-transition route the v29 bug travelled, and
	 * reverting to the "both are guns" test would put that bug straight back — a player who tapped 3
	 * between 1 and 2 would draw the SMG with the pistol's rounds in it. Route-independence is what
	 * makes this correct in both eras and it is why the revert did not touch it.
	 */
	UPROPERTY(Replicated)
	ETraceEquippedWeapon LiveClipOwner = ETraceEquippedWeapon::Gun;

	/** SERVER TRUTH. Rounds in the OTHER gun's magazine. Meaningless while the knife is selected. */
	UPROPERTY(Replicated)
	uint8 StowedGunClipAmmo = 0;

	/** How many of StowedGunClipAmmo are ability-loaded (X's Sting, put away mid-clip). */
	UPROPERTY(Replicated)
	uint8 StowedGunAbilityRounds = 0;

	/** OWNING CLIENT ONLY. Prediction mirror of StowedGunClipAmmo. -1 until first seeded. */
	int32 PredictedStowedClipAmmo = -1;

	/**
	 * The selector THIS machine last applied locally, so OnRep_EquippedWeapon can tell a replicated
	 * CONFIRMATION of a predicted swap from a CORRECTION of one the server refused.
	 *
	 * Not replicated and deliberately not a UPROPERTY: it is a fact about this process's own last
	 * write, and replicating it would make it a copy of EquippedWeapon rather than a witness to it.
	 * Seeded to the same default EquippedWeapon has, so a pawn that never swaps never sees a
	 * spurious correction.
	 */
	ETraceEquippedWeapon LocallyAppliedWeapon = ETraceEquippedWeapon::Gun;

	/** OWNING CLIENT ONLY. -1 until the first OnRep seeds it; see GetClipAmmo(). */
	int32 PredictedClipAmmo = -1;

	/** The serial PredictedClipAmmo was last reconciled against. */
	uint8 PredictedClipSerial = 0;

	/**
	 * OWNING CLIENT ONLY. True between the client predicting a refill and the server's refill landing.
	 *
	 * It exists for one packet: while the client is ahead, an update carrying the OLD (nearly empty)
	 * clip under the OLD serial must not be allowed to pull the predicted count back down to 2 rounds
	 * for the ~RTT/2 before the real refill arrives. That flicker is at the END of the reload, which
	 * is precisely the moment the player is looking at the number to decide whether to re-engage.
	 */
	bool bPredictedRefillPending = false;

	/** Set by ConsumeRound for the round it just spent. See WasLastRoundAbilityRound(). */
	bool bLastRoundWasAbilityRound = false;

	/** Server-side lifetime bookkeeping, per pawn. Read by Trace.Ammo.BotWatch. */
	int32 TotalRoundsConsumed = 0;
	int32 TotalReloadsCompleted = 0;

	/**
	 * Spends one round on the machine that owns the decision (authority, or a predicting client).
	 *
	 * *** CARRIES ITS OWN CARRIER GUARD, AND THAT IS A BELT TO CanFire's BRACES. *** CanFire() and
	 * ServerFire() both refuse a carrier already (spec §4), so this can only fire if some future
	 * caller reaches the consumption path another way — which is exactly the failure mode
	 * UTraceHealthComponent::ApplyVulnerable's second carrier lock exists for. It counts and logs
	 * rather than silently coping, and Trace.Ammo.CarrierGuard 0 is its red arm.
	 */
	void ConsumeRound();

	/**
	 * AUTHORITY ONLY. Sets the clip to @p Rounds (of which @p AbilityRounds are ability-loaded),
	 * clears any running reload and bumps ClipSerial.
	 *
	 * The serial bump is the point: it is what tells an owning client "this is a new clip, throw your
	 * prediction away" rather than "I am behind you".
	 */
	void RefillClip(int32 Rounds, int32 AbilityRounds);

	/**
	 * Starts the 0.5 s reload with its deadline anchored at @p AnchorSharedTime.
	 *
	 * THE ANCHOR IS THE WHOLE REASON THIS TAKES A PARAMETER. For a manual reload it is the stamped
	 * key press; for the automatic one it is the SHOT that emptied the clip — the same clamped
	 * timestamp ServerFire already rewinds to. Both ends therefore compute one instant, and a client
	 * whose gun came back up at T does not then find the server refusing to fire until T + ping.
	 */
	void BeginReload(double AnchorSharedTime);

	/** Cancels a running reload without refilling. Death and picking up the Core. */
	void CancelReload();

	/**
	 * Runs the reload state machine once per tick: completion, the automatic reload of an empty clip,
	 * and the two cancellations (death, and picking up the Core).
	 *
	 * ON EVERY MACHINE THAT OWNS A DECISION — the authority for the truth, a predicting client for
	 * its own mirror. The automatic reload is what makes "30 bullets per clip, then the gun reloads"
	 * true for BOTS as well as humans without a line of AI code: a bot's pawn is authoritative and
	 * locally controlled in the same process, so it reloads through this identical path.
	 */
	void TickReload();

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

	/**
	 * SPEC v28 §9. Exchanges the live magazine with the stowed one. Called by ApplyEquip and by
	 * nothing else, on a GUN-TO-GUN transition only.
	 *
	 * Runs on the authority (the replicated pair) and on a predicting client (the predicted pair),
	 * which is the same "each machine owns one side of the decision" split ConsumeRound documents.
	 * A running reload is cancelled by the caller before this: a magazine that was half-loaded when
	 * it went in the pocket is not half-loaded when it comes out, it is exactly as empty as it was.
	 */
	void SwapStowedClip();

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

	/** See SetAutonomousAttacksAllowed. True for everything except a practice-range target. */
	bool bAutonomousAttacksAllowed = true;

	// --- Presentation: the knife you can see ------------------------------------------------------
	//
	// TWO rigs, because there are two audiences and they need opposite things:
	//
	//   KnifeViewRoot   first person, OnlyOwnerSee, hung under ATraceCharacter::ViewModelRoot so it
	//                   inherits the sway/bob/recoil transform the gun already gets for free. This
	//                   is the rig that physically swings, and it is the swinger's whole read.
	//   KnifeHandRoot   third person, OwnerNoSee, attached to the pawn's RIGHT HAND BONE so every
	//                   OTHER player can see that this person chose the knife — which is the tell
	//                   that they are now 22% faster (spec v12 §3) and cannot shoot back.
	//                   *** "hand_r" IS THE MANNEQUIN'S NAME FOR THAT BONE AND NOT EVERY BODY'S. ***
	//                   A pawn's body now depends on which character is playing it, and Rocco's rig
	//                   calls it "RightHand1". ATraceCharacter::ResolveBodyBoneName does the
	//                   translation; asking for "hand_r" on that rig gets nothing, which is not a
	//                   crash but a knife lying at the player's feet.
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

public:
	/**
	 * The pawn's body mesh has been REPLACED (a character was picked, switched, or arrived late), so
	 * anything of ours hanging off one of its bones is now attached by a name the new rig may not
	 * have. Tears the third-person knife down; EnsureKnifeVisualsBuilt() puts it back on the next
	 * tick against whatever the new body calls its right hand.
	 *
	 * ONLY THE THIRD-PERSON RIG. KnifeViewRoot hangs off the viewmodel, which is a first-person rig
	 * belonging to the camera and not to the body, and is unaffected by a body swap.
	 *
	 * Safe to call when nothing was built, and safe to call on a pawn with no knife.
	 */
	void NotifyBodyMeshChanged();

private:

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

	/**
	 * Dev-only: drive ConsumeRound() directly, for Trace.Ammo.CarrierTest.
	 *
	 * IT EXISTS TO REACH A GUARD THAT IS OTHERWISE UNREACHABLE, and that is the honest description.
	 * The shipped path cannot spend a carrier's round — CanFire() and ServerFire() both refuse a
	 * carrier before consumption ever comes up — so a harness driving the shipped path would watch
	 * the clip hold steady and learn nothing about ConsumeRound's own carrier guard, which is the
	 * second lock and the one that survives a future caller forgetting the first. This calls that
	 * lock directly so Trace.Ammo.CarrierGuard 0 can be shown breaking it and 1 can be shown holding.
	 *
	 * It is NOT how the §4 gate itself is tested; that is a separate check in the same command, made
	 * against CanFire() on a real carrier.
	 */
	void DebugConsumeRound() { ConsumeRound(); }

	/** Dev-only read-back of the raw server-side count, bypassing GetClipAmmo()'s owner/proxy rules. */
	int32 DebugGetAuthoritativeClipAmmo() const { return static_cast<int32>(ClipAmmo); }

	/** Dev-only: rounds this pawn has ever spent, and reloads it has ever finished. Server-side. */
	void DebugGetAmmoTotals(int32& OutRoundsConsumed, int32& OutReloadsCompleted) const
	{
		OutRoundsConsumed = TotalRoundsConsumed;
		OutReloadsCompleted = TotalReloadsCompleted;
	}
#endif
};
