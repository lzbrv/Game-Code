// Trace — CHUT, spec v14 §6.
//
//   PASSIVE   the knife deals 50 from the FRONT (the standard is 30). The 60° back zone stays at
//             100 — §6's [ASSUMPTION], and it is a knob rather than an omission so that "unchanged"
//             is a decision somebody made rather than a value nobody looked at.
//
//   MOVEMENT  BASH — hitting a player with the END of his standard dash knocks them along his
//             direction of travel. "NO EFFECT ON THE CORE CARRIER" — which is not a special case
//             here: it is ETraceAbilityEffect::Control asked of the §4 choke point, and the choke
//             point's Control answer for a carrier is exactly the sentence §6 wrote.
//             "Dashing through a trace still kills the carrier normally" — untouched, because the
//             bash never goes near the trail's trip test and never cancels a dash.
//
//   ACTIVATED CHUD — 30% less damage from body shots and melees for 10 s, REFRESHED (not extended,
//             not stacked) by a knife kill. 20 s cooldown. §6 [ASSUMPTION]: headshots and trace
//             deaths are unaffected, because it names body shots and melees.
//
// ===================================================================================================
// WHY THE BASH IS DRIVEN BY A POLL AS WELL AS BY THE HOOK
// ===================================================================================================
//
// UTraceAbilityComponent::NotifyDashHitCharacter is the framework's intended entry point, and this
// class implements OnDashHitCharacter for it.
//
// *** THE HOOK IS NOW LIVE. *** UTraceCharacterMovementComponent's per-frame dash contact sweep
// (TraceCharacterMovementComponent.cpp, ~:3997, "SPEC v14 §6 — THE PER-FRAME DASH CONTACT SWEEP")
// calls it: the block reads GetDashHitSweepRadiusFor(), which is 0 for everyone who is not Chut, and
// dispatches contacts to NotifyDashHitCharacter on the server only, never on a replayed move. This
// paragraph used to say the hook was "called by NOTHING", which stopped being true when that sweep
// landed and is corrected here rather than deleted, because the reasoning below still governs.
//
// The same single apply path (TryBash) is ALSO driven from TickAbilities on the server, which polls
// the pawn's own public dash state (IsDashing / GetDashDirection) and sweeps for victims. BOTH ROUTES
// STILL EXIST AND THAT IS DELIBERATE: the poll samples at 20 Hz and a dash covers 150 uu between
// samples against a 130 uu reach, so the mover's per-frame sweep is the one that cannot miss, while
// the poll keeps working if the movement slice is ever bypassed. Both are guarded by BashedThisDash,
// so the result is one bash per victim per dash, not two.
//
// Neither is a second RULE — both funnel into TryBash and TryBash asks the choke point exactly once.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetChut.generated.h"

/**
 * Chut's bits in FTraceAbilityNetState::Flags. Only one character is live per component, so these
 * cannot collide with anybody else's. Same shape as TraceLilyFlags, and for the same reason.
 */
namespace TraceChutFlags
{
	/**
	 * Chud is up. TraceAbilityFlags::EffectActive, so spec v19 §4.2's central death wipe stops it
	 * for free (UTraceAbilityComponent::ApplyDeathStateWipe clears exactly this bit, MovementActive
	 * and EffectEndMatchTime). Named here rather than spelled TraceAbilityFlags::EffectActive at the
	 * five call sites that read it, which is what this file used to do — the bit is now written down
	 * once, next to the bit below that it must never be confused with.
	 */
	inline constexpr uint8 Chud = TraceAbilityFlags::EffectActive;

	/**
	 * A DASH IS RUNNING, and its end is in FTraceAbilityNetState::AuxEndMatchTime.
	 *
	 * *** THIS BIT EXISTS BECAUSE THE FX PLAN'S ONE FACTUAL ERROR IS ABOUT THIS ABILITY. ***
	 * FX_AUDIO_PLAN §2.2 hangs the armed tell on "kit TickAbilities on every machine (dash state is
	 * replicated movement)". It is not. UTraceCharacterMovementComponent::IsDashing() is a pure
	 * function of DashTimeRemaining, DashTimeRemaining is saved-move state, and NOTHING replicates
	 * it — that component's own header says so twice, in the words "it is NOT valid on a SIMULATED
	 * proxy ... a simulated proxy reads false forever". So a tell driven from IsDashing() would be
	 * visible to Chut and to the server and to nobody else, which is precisely the audience an
	 * ARMED TELL is for. OnDashStarted is no better: it fires on the authority and the owning client
	 * and on no other machine.
	 *
	 * MovementActive rather than AuxActive, and the choice is load-bearing twice over:
	 *   - it MEANS that ("something of mine is running on my body right now" — TraceAbilityTypes.h),
	 *     where AuxActive means "a thing I put in the WORLD is still there";
	 *   - it is one of the two bits the central death wipe CLEARS, so a Chut who dies mid-dash stops
	 *     glowing on every machine without this file being trusted to remember. AuxActive is KEPT on
	 *     death by design, and a tell on that bit would have needed a hand-written clear that the
	 *     next reader of this file would have had to be told about.
	 *
	 * WHAT IS REPLICATED IS THE WHOLE DASH, NOT THE 63 ms WINDOW. At DashDuration 0.18 s the bash
	 * window is the last 35%, i.e. 63 ms — barely more than one 20 Hz tick, and a bit raised and
	 * cleared inside one replication interval coalesces to "nothing changed" and reaches no client
	 * at all. So the server publishes the dash (~0.18 s, 3-4 ticks, unmissable) with its END TIME,
	 * and every machine derives the window locally from the same two live knobs TryBash uses. One
	 * fact on the wire, one arithmetic, no second copy of the window to drift.
	 */
	inline constexpr uint8 Dashing = TraceAbilityFlags::MovementActive;
}

class ATracePlayerController;
class UMaterialInstanceDynamic;

UCLASS()
class TRACE_API UTraceAbilitySetChut : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Chut; }

	// --- lifecycle --------------------------------------------------------------------------------
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnUnequipped() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// =============================================================================================
	// THE FX ROUTER — FX_AUDIO_PLAN §2.2's armed tell. See TraceChutFlags::Dashing above for why the
	// dash has to reach other machines through the replicated scratch pad and not through the mover.
	// =============================================================================================
	//
	// The BASH ITSELF is not routed here and must not be: its whole presentation — wedge, speed
	// lines, contact ring and the ChutBash sound — rides ONE ATraceFxBurst spawned by the server in
	// TryBash. A replicated actor already is a multicast, so routing a second copy off the state
	// edge would show the owner two wedges and put the sound on top of itself (§8.7).
	virtual void OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New) override;
	virtual void SyncClientFx(const FTraceAbilityNetState& Current) override;

	// --- activated: Chud --------------------------------------------------------------------------
	virtual bool  ActivateAbility() override;
	virtual float GetActivatedCooldownSeconds() const override;

	// --- movement: the bash -----------------------------------------------------------------------
	virtual bool OnDashStarted(const FVector& DashDirection) override;
	virtual void OnDashEnded(bool bReachedFullDistance) override;
	virtual void OnDashHitCharacter(ATraceCharacter* Other, float DashProgress) override;
	virtual float GetDashHitSweepRadius() const override;

	// --- passive: the knife, and Chud's reduction --------------------------------------------------
	virtual float ModifyOutgoingDamage(float Damage, const FTraceAbilityDamageContext& Context) const override;
	virtual float ModifyIncomingDamage(float Damage, const FTraceAbilityDamageContext& Context) const override;
	virtual void  OnKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot) override;

	// --- queries, for the HUD and the verification harness -----------------------------------------

	/** True while Chud's window is open. Correct on every machine — it reads replicated state. */
	bool IsChudActive() const;

	/** Seconds left on Chud, 0 when it is down. */
	float GetChudSecondsRemaining() const;

	/**
	 * THE ONE APPLY PATH FOR THE BASH. Both the poll and OnDashHitCharacter come through here, and
	 * so does the harness — so what is verified is what ships.
	 *
	 * AUTHORITY ONLY. Applies, in order: the end-of-dash window, the reach, the §4 choke point
	 * (Control), and only then a launch.
	 *
	 * @param Victim         who is being hit.
	 * @param DashProgress   0..1 through the dash. §6 says the END of the dash, so an early value
	 *                       is refused rather than clamped.
	 * @param DashDirection  Chut's direction of travel; the knock goes this way, plus an up bias.
	 * @param OverrideGapUU  negative (the default) measures the gap from Chut's CURRENT location.
	 *                       The poll passes the distance to its swept segment instead, because at
	 *                       the component's 20 Hz tick a dash travels further between samples than
	 *                       the reach — see LastPolledLocation.
	 * @return true when the victim was actually launched.
	 */
	bool TryBash(ATraceCharacter* Victim, float DashProgress, const FVector& DashDirection,
	             float OverrideGapUU = -1.f);

	/** How many victims the current (or most recent) dash has bashed. */
	int32 GetBashedThisDashCount() const { return BashedThisDash.Num(); }

	/**
	 * *** THE ONE PLACE THE BASH WINDOW IS DEFINED. §6's "the END of his standard dash". ***
	 *
	 * Returns the dash progress at which the window opens: 1 - ChutBashEndFraction, read LIVE from
	 * UTraceSettings and clamped exactly once. TryBash gates on it, PollDashForBash pre-filters on
	 * it, and the armed tell lights on it — three readers, one arithmetic, so the thing a player is
	 * SHOWN and the thing that actually bashes them cannot drift apart. That drift is the bug class
	 * the bible calls "drawn != lethal" and it is not a hypothetical here: this file previously
	 * spelled `1.f - FMath::Clamp(...)` out twice, and a tell would have made it three.
	 */
	static float BashWindowStartFraction();

	/** True on ANY machine while the replicated dash says the bash window is open. Drives the tell. */
	bool IsBashWindowPresented() const;

#if !UE_BUILD_SHIPPING
	/** DEV ONLY, for Trace.Chut.BashFxTest: is this machine drawing the armed tell right now? */
	bool DebugIsAccentLifted() const { return bAccentLifted; }

	/** DEV ONLY: the AccentGlow the lift writes, and the bible Glow it converts to. */
	static void DebugAccentLiftValues(float& OutAccentGlow, float& OutBibleGlow);
#endif

private:
	/** Server-side dash-edge tracking for the poll. See the header for why the poll exists. */
	bool  bWasDashing = false;
	float DashStartWorldTime = 0.f;
	FVector PolledDashDirection = FVector::ZeroVector;

	/**
	 * Chut's location at the previous poll, so the reach is a SWEPT SEGMENT rather than a point.
	 *
	 * This is not a refinement, it is a correctness fix forced by where the poll lives:
	 * UTraceAbilityComponent ticks at TickInterval = 0.05 s, and a dash covers DashSpeed x 0.05 =
	 * 150 uu between two samples — more than the whole 130 uu bash reach. A point test at each
	 * sample would therefore step straight over a player standing between two samples, and the bash
	 * would "work" in a harness that places the victim and miss in a real dash. Testing the victim
	 * against the segment closes the gap without widening the tuned reach by a single uu.
	 */
	FVector LastPolledLocation = FVector::ZeroVector;
	bool bHasLastPolledLocation = false;

	/** One bash per victim per dash — the guard that lets the poll and the hook coexist. */
	TArray<TWeakObjectPtr<ATraceCharacter>> BashedThisDash;

	/** Server only. Sweeps for victims inside the bash reach and calls TryBash on each. */
	void PollDashForBash(float DeltaSeconds);

	/** Clears the per-dash bookkeeping. */
	void ResetDashTracking();

	bool HasBashed(const ATraceCharacter* Victim) const;

	/** SERVER ONLY. Publishes / clears TraceChutFlags::Dashing and the dash's end time. Edge-safe. */
	void PublishDashWindow(bool bDashing, float DashEndMatchTime);

	// =============================================================================================
	// FX_AUDIO_PLAN §2.2 — THE ARMED TELL, and why it is a material parameter and not a primitive
	// =============================================================================================
	//
	// "Chut's OWN BODY ACCENT STRIPES lift Glow 1.7 -> 3.0 (MID param via the §4.4-bible body
	// materials; accent-on-own-body is legal, trail tint is NOT — accents never touch the trail,
	// bible §2.3)."
	//
	// So there is no geometry here at all: the tell is a scalar written onto the body MIDs that
	// ATraceCharacter::ApplyColorToSkeletalMesh already built, and it costs ZERO of §1.4's four
	// attached primitives. That is the whole reason the plan chose it — a dash is 0.18 s and a
	// spawned primitive would be a component created and destroyed six times a fight.
	//
	// UNITS, AND THE ONE CONVERSION THAT MATTERS. "Glow" in the bible is the FINAL emissive
	// multiplier; the material parameter is AccentGlow and M_TraceBodyAccent multiplies it by 0.2125
	// (Scripts/generate_body_materials.py). So the shipped body value of 8 IS bible Glow 1.7 — the
	// integrator's own note, MASTER_PLAN conflict #1 — and §2.2's 3.0 is AccentGlow 3.0 / 0.2125.
	// The conversion lives in the .cpp beside its constant, never as a bare 14.1 anywhere.
	//
	// RESTORATION IS NOT A REMEMBERED NUMBER. The off-edge calls ATraceCharacter::ApplyTeamColors(),
	// which recomputes AccentGlow from the pawn's OWN state machine (8 normal / 30 carrier / 0 dead)
	// and repaints the whole body with it. Storing "the value before I lifted it" would be a copy of
	// somebody else's state that goes stale the moment he picks up the Core mid-dash — which is a
	// thing that happens.

	/** True while this machine is drawing the lift, so the restore fires exactly once. */
	bool bAccentLifted = false;

	/** The pawn whose MIDs were written. Weak: the set outlives pawns and must not restore a corpse. */
	TWeakObjectPtr<ATraceCharacter> AccentPawn;

	/** Writes the lifted AccentGlow onto every body MID, or restores via ApplyTeamColors(). */
	void SetAccentLift(bool bLifted);

	/** Every machine, every tick: opens and closes the tell off the replicated dash window. */
	void TickArmedTell();
};
