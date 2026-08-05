// Trace — THE PARRY (spec v3 §3).
//
// Verbatim from the design notes:
//
//     "Create a parry mechanic for the core carrier. Parrying gives your trace invulnerability for
//      .1seconds. It also makes the entire trace turn red for the duration of the parry. If an enemy
//      would break your trace with a dash, parrying as they dash protects the trace."
//
// WHERE THE PIECES LIVE, AND WHY
//
// This header is the *policy and entry point*: the tunables, the one function input calls, and the
// read-only queries the HUD and the bots want. The *state* — when the window opened, when the
// cooldown ends — lives on UTraceTrailComponent, deliberately:
//
//   * it is TRACE invulnerability, and the trail component is the thing that owns, replicates and
//     draws the trace, so putting the window anywhere else would mean two objects agreeing about
//     one fact (this codebase already has a four-writer bug of exactly that shape logged against
//     ATracePlayerState::bIsCarrier);
//   * the trail component already exists on every ATraceCharacter and already replicates, so the
//     window costs one float on the wire and needs no new dynamically-attached replicated
//     subobject;
//   * the red tint has to be applied by whatever owns the material instances, which is the trail
//     component.
//
// THERE ARE NOW **TWO** SOURCES OF TRACE INVULNERABILITY. READ THIS BEFORE TOUCHING EITHER.
//
//   1. THE PASS WINDOW (spec §4, older). From the instant the holder inputs a pass until it
//      completes or cancels, the trace cannot be broken AND the holder's shield is down. That is a
//      single replicated bool on ATraceCore, read back through ATraceCore::IsTraceInvulnerableFor()
//      so the two consequences can never disagree. It lasts ~0.5s and it is a COMMITMENT: you gave
//      up your shield for it.
//
//   2. THE PARRY (spec v3 §3, this file). 0.1s, on a 1.5s cooldown, and it does NOT touch the
//      shield — you are still bulletproof while it is up. It is a reaction check, not a commitment.
//
// They compose by OR and neither may clobber the other:
//   UTraceTrailComponent::IsTraceInvulnerable() == IsPassWindowInvulnerable() || IsParryActive()
//
// Concretely, the two mistakes a future reader is about to make:
//   * DO NOT fold the parry into ATraceCore::IsTraceInvulnerableFor(). That function is also what
//     drops the carrier's shield (UTraceHealthComponent::IsInvulnerable consults it), so a parry
//     routed through it would make the carrier SHOOTABLE for 0.1s every 1.5s. The spec grants trace
//     invulnerability, not a damage window.
//   * DO NOT make the parry write the pass window's bool, or "parry, then pass" would end the parry
//     early and "pass, then parry" would leave the shield down after the pass resolved.
//
// AUTHORITY. The server alone decides whether a dash landed inside a window: the trip test reads
// IsParryActive(), which on every machine is a pure function of the REPLICATED window end time, and
// which deliberately ignores the local prediction. The client predicts only the red tint (see
// IsParryVisuallyActive), because 0.1s is shorter than a round trip and a tell that arrives after
// the window has closed is not a tell.
//
// SPEC v6 §3 — A PERFECT PARRY NOW KILLS THE DASHER.
//
//     "Perfectly timed parries should kill the enemy dashing. E.g. if an enemy's dash would kill the
//      carrier, but the carrier stops it with a parry, that enemy dies instead. Retain all other
//      rules regarding dashing a trace and parrying."
//
// The parry stopped being purely defensive. It is now a punish, and the one function that implements
// it is ServerPunishParriedDash() below. Read its comment before changing anything here: the whole
// mechanic is one conditional and every clause of that conditional is a rule the user stated.
//
// Nothing else about the parry moved. 0.1s window, 1.5s cooldown, carrier-only, whole trace flashes
// red, and an UNPARRIED dash still kills the carrier exactly as before — that branch is untouched.

#pragma once

#include "CoreMinimal.h"
#include "Math/Color.h"
#include "UObject/NameTypes.h"

class AActor;
class ATraceCharacter;

/** Why a parry request was turned down. Reported to the log and available to the HUD. */
enum class ETraceParryRefusal : uint8
{
	/** Not refused — a window was opened. */
	None,

	/** No pawn, or the pawn has no trail component. */
	NoPawn,

	/** Dead players do not parry. */
	Dead,

	/** Spec §3: "a parry mechanic for the core carrier". A non-carrier pressing parry does nothing. */
	NotCarrying,

	/** The cooldown has not elapsed. */
	OnCooldown,
};

/** Human-readable form of @p Refusal, for logs and the HUD. */
const TCHAR* LexToString(ETraceParryRefusal Refusal);

/**
 * The parry, as seen by everything that is not the trace itself.
 *
 * Every function here is safe to call on any machine and on any actor, including null.
 */
namespace TraceParry
{
	// ---------------------------------------------------------------------------------------------
	// TUNABLES
	//
	// THESE LIVE IN UTraceSettings, Category = "Parry": ParryDuration, ParryCooldown, ParryTintColor
	// and ParryGlowScale. Categorised, clamped, tooltipped, live-editable during PIE — which is the
	// user's standing instruction:
	//
	//     "implement these as tunable variables so I can playtest and adjust numbers rather than you
	//      guessing at feel"
	//
	// The accessors below are the ONLY readers. Nothing else in the codebase may read the settings
	// properties directly, because these apply the clamps and the tint's documented constraint.
	//
	// Three console OVERRIDES exist alongside them, each defaulting to a negative sentinel meaning
	// "defer to the setting". They are for console experiments during a headless run, where there is
	// no Project Settings panel to open — not a second definition of the defaults.
	//
	//     Trace.Parry.Duration     -1   override for the seconds of trace invulnerability
	//     Trace.Parry.Cooldown     -1   override for the seconds before the next parry
	//     Trace.Parry.GlowScale    -1   override for the emissive multiplier on the red trace
	//     Trace.Parry.ForceWindow   0   debug: every carrier is permanently parrying
	//     Trace.Parry.BotAuto       0   debug: AI carriers parry whenever the cooldown is ready
	//
	// And two added by v6 §3, which are NOT overrides — there is no setting behind them yet, because
	// UTraceSettings is another ownership slice this pass. Promoting them is specified in the report:
	//
	//     Trace.Parry.KillsDasher          1     the v6 §3 rule itself; 0 restores the pre-v6 parry
	//     Trace.Parry.KillFeedbackSeconds  2.5   how long the carrier's parry-kill banner lasts
	// ---------------------------------------------------------------------------------------------

	/** Spec §3: 0.1 seconds of trace invulnerability. */
	float GetDurationSeconds();

	/** [ASSUMPTION] spec §3: not specified by the user. 1.5s makes 0.1s a reaction check. */
	float GetCooldownSeconds();

	/**
	 * The colour the ENTIRE trace turns for the duration (spec §3).
	 *
	 * Green and blue are near zero on purpose and must stay that way. The trace is drawn on an unlit
	 * emissive material at glow values well above 1, so every channel with any weight in it clips to
	 * white at the tonemapper — the exact failure a previous pass measured when the whole trace ran
	 * at glow 3.4 and became "a shapeless white slab". A tint of (1, 0.03, 0.06) stays unambiguously
	 * RED no matter how hard the glow is pushed, which is the whole point: red is the mechanic's
	 * readability, for the carrier and for the enemy who is mid-dash.
	 */
	FLinearColor GetTintColor();

	/** Emissive multiplier while parrying. Distinct from (and above) the pass window's 1.9. */
	float GetGlowScale();

	// ---------------------------------------------------------------------------------------------
	// Entry points
	// ---------------------------------------------------------------------------------------------

	/**
	 * THE ONE ENTRY POINT. Wire the parry bind, the bots and any debug command to this.
	 *
	 * Safe from any machine: on the server it opens the window directly, on an owning client it
	 * predicts the tint locally and asks the server. Returns true if a window was opened or
	 * predicted, false (with @p OutRefusal set) if the rules said no.
	 *
	 * @param Parrier     the parrying pawn. Null is a refusal, not a crash.
	 * @param OutRefusal  optional; why it was refused.
	 */
	bool RequestParry(ATraceCharacter* Parrier, ETraceParryRefusal* OutRefusal = nullptr);

	// ---------------------------------------------------------------------------------------------
	// SPEC v6 §3 — THE PUNISH
	// ---------------------------------------------------------------------------------------------

	/**
	 * A parried dash kills the DASHER. Server only. This is the entire v6 §3 mechanic.
	 *
	 * CALL SITE. Exactly one: UTraceTrailComponent::ServerRunTripTest, at the point where it has
	 * already established that @p Dasher's swept capsule intersected @p Carrier's LETHAL trace this
	 * tick and that a parry window was open, i.e. the branch that today only logs
	 * "NO KILL - PARRIED". Do not call it from anywhere else and do not re-derive the geometry here:
	 * the trip test is the single implementation of "did a dash hit the trace", and a second one
	 * would be exactly the two-objects-agreeing-about-one-fact bug this file's header warns about.
	 *
	 * WHY THE GEOMETRY GATE MATTERS. Spec v6 §3 [ASSUMPTION]: *"only a dash that WOULD have killed
	 * the carrier is punished. A parry thrown at nothing kills nobody."* Because the only caller is
	 * the branch inside `if (bHitLethal)`, a parry pressed at empty air can never reach this
	 * function — there is no "did anyone dash?" test to get wrong, the call simply does not happen.
	 * That is the whole reason the punish lives at the call site instead of on a timer.
	 *
	 * THE REMAINING CLAUSES, all re-checked here so the rule survives a future caller:
	 *
	 *   * authority — the server decides. A client calling this does nothing.
	 *   * IsParryActiveFor(Carrier) — the REPLICATED window, never the local prediction, so the
	 *     punish and the protection are decided by the same fact on the same machine.
	 *   * both pawns alive.
	 *   * UTraceSettings::TrailLethality must actually kill the carrier (KillsCarrier / KillsBoth).
	 *     Under the KillsToucher tuning rule the dash would NOT have killed the carrier, so by the
	 *     [ASSUMPTION] above there is nothing to punish.
	 *
	 * DELIBERATELY *NOT* A CLAUSE: whether the §4 pass window happened to be open too. If both were
	 * up the dasher still dies. The trace they dashed into was RED — IsParryActive() wins the tint
	 * (UTraceTrailComponent::GetAppliedTraceColor) — and "red trace kills me" has to be true every
	 * single time or it is not a tell. Consistency of the signal beats the pedantic reading that the
	 * pass window would have saved the carrier anyway.
	 *
	 * FEEDBACK. The dasher is killed with GetParryKillCause() as the death cause and the CARRIER'S
	 * controller as the killer, so the existing victim-side path (ATraceGameMode::NotifyCharacterDied
	 * -> ClientNotifyKilledBy -> ATraceHUD::DrawDeathPanel) puts "by <carrier> (Parried)" on the
	 * dasher's own death panel, and the carrier is credited with the kill on the scoreboard. The
	 * carrier-side banner is fed by RecordParryKill/GetParryKillFeedback below.
	 *
	 * @return true if the dasher was killed.
	 */
	bool ServerPunishParriedDash(ATraceCharacter* Carrier, ATraceCharacter* Dasher);

	// ---------------------------------------------------------------------------------------------
	// SPEC v7 §6 — A PARRY KILL REFUNDS THE CARRIER'S COOLDOWNS
	//
	// Verbatim: *"When a player carrying the trace successfully kills an enemy with a parry, reset
	// that players parry cooldown to zero and one of their dash cooldowns to zero. If the first dash
	// is already off cooldown, the second one should be set to zero instead."*
	//
	// A KILL, NOT A PARRY. This is called from exactly one place — inside ServerPunishParriedDash,
	// after the dasher is confirmed dead — and that is the whole of the gating. A parry that merely
	// protects the trace refunds nothing, and a parry thrown at empty air cannot reach here at all,
	// for the same structural reason the punish itself cannot: the only caller is inside the branch
	// that has already established a lethal dash hit a live parry window.
	//
	// "ONE OF THEIR DASH COOLDOWNS", AND WHICH ONE. The carrier's two dashes are a CHARGE POOL with a
	// single refill clock (see UTraceCharacterMovementComponent's "why charges and not a second
	// timer"), not two independent timers, so "the first one, unless it is already off cooldown, in
	// which case the second" has exactly one meaning in that model: HAND BACK ONE CHARGE. Spent one
	// of two -> the spent one comes back. Spent both -> one comes back and the other keeps refilling
	// on the clock it was already on. Spent neither -> nothing to refund, which the spec also says.
	// ---------------------------------------------------------------------------------------------

	/**
	 * Server. Applies spec v7 §6's refunds to @p Carrier: parry cooldown to zero, one dash charge back.
	 *
	 * Server-authoritative and idempotent-safe: a second call with nothing left to refund is a no-op.
	 * The parry cooldown is a REPLICATED deadline, so zeroing it reaches the carrier's HUD by the
	 * ordinary replication path; the dash pool is saved-move state and needs the owning client told
	 * directly, which is what the accessor described in this pass's report does.
	 *
	 * @return true if anything was actually refunded.
	 */
	bool ServerRefundOnParryKill(ATraceCharacter* Carrier);

	/** Parry cooldowns zeroed by a parry kill this process. Verification counter; never a game rule. */
	int32 GetParryCooldownRefundCount();

	/** Dash charges handed back by a parry kill this process. Verification counter. */
	int32 GetDashRefundCount();

	/**
	 * Parry kills where a dash charge was owed but could NOT be handed back, because the movement
	 * component in this build exposes no way to do it.
	 *
	 * Exists so the gap is a NUMBER in the run's own log rather than a claim in a report. See
	 * IsDashRefundWired().
	 */
	int32 GetDashRefundUnwiredCount();

	/**
	 * True when this build actually has a way to hand a dash charge back.
	 *
	 * The dash pool lives on UTraceCharacterMovementComponent, which is another ownership slice this
	 * pass; the one-function accessor it needs is specified in this pass's report. This answers
	 * whether that accessor has landed, so the verification harness reports "refunded" or "the
	 * accessor is missing" instead of silently reporting a refund nobody performed.
	 */
	bool IsDashRefundWired();

	/**
	 * The death cause reported for a dasher killed by a parry: "Parried".
	 *
	 * Its own tag, not "Trail". The two deaths are opposite outcomes of the same collision and the
	 * kill feed, the death panel and every log line have to be able to tell them apart — "Trail" on
	 * the dasher's screen would read as the game having killed the wrong player.
	 */
	FName GetParryKillCause();

	// ---------------------------------------------------------------------------------------------
	// Queries
	// ---------------------------------------------------------------------------------------------

	/**
	 * True while @p Actor's trace is protected SPECIFICALLY BY A PARRY.
	 *
	 * This is the authoritative answer — the replicated window only — so the server's verdict and
	 * every client's readback are the same verdict. It deliberately says nothing about the pass
	 * window; ask ATraceCore::IsTraceInvulnerableFor() for that, or
	 * UTraceTrailComponent::IsTraceInvulnerable() for "protected by anything at all".
	 */
	bool IsParryActiveFor(const AActor* Actor);

	/** Seconds of the parry window left, 0 when it is closed. */
	float GetWindowRemainingFor(const AActor* Actor);

	/** Seconds until @p Actor may parry again; 0 means ready. For the HUD pip. */
	float GetCooldownRemainingFor(const AActor* Actor);

	/** Total cooldown, so the HUD can draw a fraction without knowing the tunable. */
	float GetCooldownTotal();

	// ---------------------------------------------------------------------------------------------
	// Parry-kill feedback (v6 §3: "the dasher needs to understand why they died")
	//
	// TWO SIDES, TWO CHANNELS, because the two players need different sentences:
	//
	//   THE DASHER already has a complete, replicated answer with no new plumbing at all — they are
	//   killed with cause "Parried" and the carrier as the killer, so ATraceHUD's death panel reads
	//   "by <carrier>  (Parried)". That path is server -> victim and is reliable.
	//
	//   THE CARRIER gets the record below. It is written on the server the instant the punish lands
	//   and is what a HUD banner reads ("PARRIED — <victim>"). On a listen server — the host, which
	//   is how this game is played — the carrier's HUD is on the writing machine and reads it
	//   directly. A REMOTE carrier needs the one-line client RPC described in this pass's report;
	//   ATracePlayerController is not in this ownership slice, so it is specified there rather than
	//   written here. The mechanic itself does not depend on it.
	// ---------------------------------------------------------------------------------------------

	/** Server-side. Files a parry kill by @p Carrier against @p Victim for the feedback readers. */
	void RecordParryKill(const AActor* Carrier, const AActor* Victim);

	/**
	 * Most recent parry kill by @p Parrier, if it is recent enough to still be worth drawing.
	 *
	 * @param OutSecondsAgo   age of the event, 0 on the frame it happened.
	 * @param OutVictimName   display name of the dasher who died.
	 * @return false if @p Parrier has no parry kill inside GetParryKillFeedbackSeconds().
	 */
	bool GetParryKillFeedback(const AActor* Parrier, float& OutSecondsAgo, FString& OutVictimName);

	/** How long a parry kill stays worth showing. */
	float GetParryKillFeedbackSeconds();

	/** Total parry kills this process has awarded. Verification counter; never a game rule. */
	int32 GetParryKillCount();

	// ---------------------------------------------------------------------------------------------
	// Debug switches, read by UTraceTrailComponent. Never true in normal play.
	// ---------------------------------------------------------------------------------------------

	/** Trace.Parry.ForceWindow — every carrier's trace behaves as permanently parried. */
	bool IsWindowForced();

	/** Trace.Parry.BotAuto — AI carriers parry the instant their cooldown is ready. */
	bool IsBotAutoParryEnabled();
}
