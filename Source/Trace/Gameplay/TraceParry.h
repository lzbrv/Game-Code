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

#pragma once

#include "CoreMinimal.h"
#include "Math/Color.h"

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
	// Debug switches, read by UTraceTrailComponent. Never true in normal play.
	// ---------------------------------------------------------------------------------------------

	/** Trace.Parry.ForceWindow — every carrier's trace behaves as permanently parried. */
	bool IsWindowForced();

	/** Trace.Parry.BotAuto — AI carriers parry the instant their cooldown is ready. */
	bool IsBotAutoParryEnabled();
}
