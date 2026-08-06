#include "Gameplay/TraceParry.h"

#include <type_traits>                     // v7 §6: the two cross-slice accessor detectors

#include "Containers/Ticker.h"
#include "Engine/Engine.h"                 // GEngine->GetWorldContexts()
#include "Engine/World.h"
#include "EngineUtils.h"                   // TActorIterator (character gather fallback)
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"           // FAutoConsoleVariableRef, FAutoConsoleCommand

#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"   // v8 §3: the shared GetServerWorldTimeSeconds clock
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"     // v8 §3: GetPingInMilliseconds() -> the hold budget

#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TracePlayerController.h"                // ClientNotifyParryKill (v6 3, carrier side)
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"             // v6 3 — the punish kills the dasher
#include "Gameplay/TraceTrailComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"   // StartDash() (verification harness only)
#include "Trace.h"
#include "TraceSettings.h"                           // the parry tunables live here

// =================================================================================================
// Tunables
//
// THE SETTINGS ARE THE SOURCE OF TRUTH. Duration, cooldown, tint and glow are UTraceSettings
// properties — categorised, clamped, tooltipped, and live-editable during PIE, which is what the
// user explicitly asked for ("implement these as tunable variables so I can playtest and adjust
// numbers"). This file no longer owns a single default.
//
// The three tuning cvars survive as OVERRIDES, not as the values. Each defaults to a negative
// sentinel meaning "defer to the setting"; set one and it wins until you set it back to -1. That
// keeps `Trace.Parry.Duration 0.3` working for a quick console experiment during a headless run —
// where there is no Project Settings panel to open — without letting the console silently become a
// second, competing definition of the mechanic's defaults. The clamps are applied on read either
// way, so neither a hand-edited ini nor a silly console value can break the mechanic.
// =================================================================================================

namespace
{
	/** Negative = "use UTraceSettings". See the block comment above. */
	float GParryDurationOverride = -1.f;
	float GParryCooldownOverride = -1.f;
	float GParryGlowScaleOverride = -1.f;

	/** Debug only. See the header. */
	int32 GParryForceWindow = 0;
	int32 GParryBotAuto = 0;

	// ---------------------------------------------------------------------------------------------
	// v6 3 — the punish, and the carrier-side feedback record.
	// ---------------------------------------------------------------------------------------------

	/**
	 * 1 = a parry that intercepts a lethal dash kills the dasher (spec v6 3, the shipping rule).
	 * 0 = the pre-v6 behaviour, parry protects and nothing else.
	 *
	 * A live switch and not a constant because it is the single biggest change to how the parry
	 * feels, and the user's standing instruction is that anything they might want to feel out in a
	 * playtest is a knob rather than a number an agent chose.
	 *
	 * INTEGRATED: the shipping value now lives in UTraceSettings::bParryKillsDasher next to
	 * ParryDuration, so it is on the settings panel and in DefaultGame.ini like every other tunable.
	 * This is the OVERRIDE, and it follows the same sentinel convention as GParryDurationOverride
	 * above: -1 (default) = "use UTraceSettings", 0/1 = force. Cheat-flagged so it cannot be flipped
	 * in a real match.
	 */
	int32 GParryKillsDasherOverride = -1;

	/** The one answer, resolved the way every other parry tunable in this file is resolved. */
	bool ParryKillsDasherEnabled()
	{
		if (GParryKillsDasherOverride >= 0)
		{
			return GParryKillsDasherOverride != 0;
		}
		return UTraceSettings::Get().bParryKillsDasher;
	}

	/** Seconds a parry kill stays on the carrier's HUD. See GetParryKillFeedbackSeconds(). */
	float GParryKillFeedbackSeconds = 2.5f;

	FAutoConsoleVariableRef CVarParryKillsDasher(
		TEXT("Trace.Parry.KillsDasher"),
		GParryKillsDasherOverride,
		TEXT("OVERRIDE for whether a parry that intercepts a LETHAL dash kills the dasher (spec v6 3). "
		     "Negative (default) = use UTraceSettings::bParryKillsDasher. 1 = force on, "
		     "0 = force off (pre-v6, the parry only protects the trace)."),
		ECVF_Cheat);

	FAutoConsoleVariableRef CVarParryKillFeedbackSeconds(
		TEXT("Trace.Parry.KillFeedbackSeconds"),
		GParryKillFeedbackSeconds,
		TEXT("How long a parry kill stays worth drawing on the parrying carrier's HUD."),
		ECVF_Default);

	/** One parry kill, kept only long enough for the HUD to draw it. */
	struct FParryKillRecord
	{
		TWeakObjectPtr<const AActor> Parrier;
		FString VictimName;
		double WorldTime = 0.0;
	};

	/**
	 * The recent parry kills, newest last. Bounded by the ten pawns in a match and swept on every
	 * write, so it cannot grow: a record older than the feedback window is dead weight by definition.
	 */
	TArray<FParryKillRecord> GParryKills;

	/** Every parry kill this process has awarded. Verification only — never read by a game rule. */
	int32 GParryKillCount = 0;

	// ---------------------------------------------------------------------------------------------
	// SPEC v7 §6 — the refunds. Counters and the two bridges to state this file does not own.
	// ---------------------------------------------------------------------------------------------

	int32 GParryCooldownRefundCount = 0;
	int32 GDashRefundCount = 0;
	int32 GDashRefundUnwiredCount = 0;

	/**
	 * 1 = a parry KILL refunds the carrier's parry cooldown and one dash charge (spec v7 §6).
	 * 0 = the v6 behaviour, where a parry kill is its own reward and nothing is given back.
	 *
	 * A switch for the same reason Trace.Parry.KillsDasher is one: this changes how the parry FEELS
	 * more than it changes what it does, and the user's standing instruction is that anything they
	 * might want to feel out in a playtest is a knob rather than a number an agent chose. Cheat-
	 * flagged so it cannot be flipped in a real match.
	 */
	int32 GParryKillRefunds = 1;

	FAutoConsoleVariableRef CVarParryKillRefunds(
		TEXT("Trace.Parry.KillRefunds"),
		GParryKillRefunds,
		TEXT("SPEC v7 §6. 1 (default): a parry KILL resets the carrier's parry cooldown to zero and hands "
		     "back one dash charge. 0: the pre-v7 behaviour, no refunds."),
		ECVF_Cheat);

	// =============================================================================================
	// THE TWO WRITES SPEC v7 §6 MAKES OUTSIDE THIS SLICE — NOW PLAIN CALLS
	//
	// Spec v7 §6 has to write two facts this file does not own:
	//
	//   THE PARRY COOLDOWN   lives on UTraceTrailComponent (ParryCooldownEndServerTime), deliberately
	//                        — see this file's header, the window and the cooldown belong with the
	//                        thing that replicates and draws the trace.
	//   THE DASH POOL        lives on UTraceCharacterMovementComponent (DashCharges), deliberately —
	//                        it is saved-move state and has to round-trip through prediction.
	//
	// Both were reached through SFINAE detectors while the accessors were being written in parallel,
	// and the parry half additionally had a by-name reflection fallback so the mechanic could be
	// tested before the accessor existed. INTEGRATED: UTraceTrailComponent::ServerResetParryCooldown()
	// and UTraceCharacterMovementComponent::RefundDashCharge() both exist now, so the detectors and
	// the reflection bridge are DELETED and these are ordinary calls that fail to compile if either
	// accessor is ever removed — which is the whole reason they were only ever meant to be temporary.
	//
	// Each accessor also owns the part of the job only it can do: the trail forces a net update so the
	// HUD pip moves immediately, and the movement component mirrors the refunded charge onto the
	// owning client, DashCharges being predicted state that is never replicated as a property.
	// =============================================================================================

	/** PlayerState name if there is one, else the actor name. Bots have both; a fresh pawn may not. */
	FString ParryDisplayName(const AActor* Actor)
	{
		if (const APawn* AsPawn = Cast<const APawn>(Actor))
		{
			if (const APlayerState* State = AsPawn->GetPlayerState())
			{
				const FString PlayerName = State->GetPlayerName();
				if (!PlayerName.IsEmpty())
				{
					return PlayerName;
				}
			}
		}
		return GetNameSafe(Actor);
	}

	FAutoConsoleVariableRef CVarParryDuration(
		TEXT("Trace.Parry.Duration"),
		GParryDurationOverride,
		TEXT("OVERRIDE for seconds of TRACE invulnerability granted by a parry (spec v3 3). "
		     "Negative (default) = use UTraceSettings::ParryDuration."),
		ECVF_Default);

	FAutoConsoleVariableRef CVarParryCooldown(
		TEXT("Trace.Parry.Cooldown"),
		GParryCooldownOverride,
		TEXT("OVERRIDE for seconds before the carrier may parry again. "
		     "Negative (default) = use UTraceSettings::ParryCooldown."),
		ECVF_Default);

	FAutoConsoleVariableRef CVarParryGlowScale(
		TEXT("Trace.Parry.GlowScale"),
		GParryGlowScaleOverride,
		TEXT("OVERRIDE for the emissive multiplier on the red trace while parrying. "
		     "Negative (default) = use UTraceSettings::ParryGlowScale."),
		ECVF_Default);

	// =============================================================================================
	// SPEC v8 §3 — the lag-compensated press. State, switches and the ledger.
	//
	// All of it is SERVER-SIDE and all of it is bounded. Nothing here replicates: the window that
	// replicates is still UTraceTrailComponent::ParryEndServerTime, still the single fact every
	// machine reads back. This is the server's working out, not a second copy of the answer.
	// =============================================================================================

	/**
	 * One window this process granted, in shared-clock time.
	 *
	 * Why a ledger at all: the trip test asks "was a parry open when the sweep resolved", and the
	 * replicated end time can only answer "is one open now". Those are the same question only when
	 * nothing was in flight — i.e. only for the host, which is precisely the player who has no
	 * complaint. Arrival is kept because a save made by a press that arrived AFTER the trip is the
	 * one number that proves the compensation did something a pre-v8 build could not.
	 */
	struct FParryWindowRecord
	{
		TWeakObjectPtr<const AActor> Parrier;
		float StartServerTime = 0.f;
		float EndServerTime = 0.f;
		float ArrivalServerTime = 0.f;
	};

	/** Newest last. Swept on every write; ten pawns and ~2s of history, so it cannot grow. */
	TArray<FParryWindowRecord> GParryWindows;

	/** How long a granted window stays in the ledger after it closes. Covers the longest hold. */
	constexpr float ParryLedgerKeepSeconds = 2.f;

	int32 GPressCount = 0;
	double GPressRewindMsTotal = 0.0;
	float GPressRewindMsMax = 0.f;

	/**
	 * Presses whose stamp was so far behind the server that only the ceiling saved them.
	 *
	 * MEASURED, not hypothetical, and it is the NORMAL case rather than a fault: a freshly joined
	 * client's shared clock was seen 1.4-4.7s behind while it converged, and a SETTLED one still reads
	 * ~138ms behind at a 105ms ping (6 presses of 6 pinned) because stock UE replicates
	 * ServerWorldTimeSeconds with no half-round-trip correction. Expect this counter to equal the
	 * press count on a real connection; see ParryRewindSlackSeconds for what that costs and bounds.
	 */
	int32 GPressClockPinnedCount = 0;

	/**
	 * Slack added to the measured one-way lag before a press is called impossible.
	 *
	 * WHY THIS IS SMALL, AND WHY IT USED TO BE 0.05. MEASURED ON THE TWO-PROCESS RIG AT 40ms/direction
	 * (ping 105ms), six presses out of six: the stamp was PINNED against the ceiling every single time,
	 * i.e. the client's GetServerWorldTimeSeconds() read ~138ms behind the server's when the packet
	 * landed, and the true upstream flight time was ~53ms. That is not a broken client. Stock UE's
	 * AGameStateBase replicates ServerWorldTimeSeconds with NO half-round-trip correction, so an
	 * honest client's shared clock sits one DOWNSTREAM lag behind by construction, and its stamp
	 * therefore lands a full ROUND TRIP back rather than one upstream leg back.
	 *
	 * The consequence is directional and it is the reason this number matters. Rewinding by more than
	 * the upstream leg does not "help the client": the window is [anchor, anchor + 0.2], so every
	 * millisecond of over-rewind is a millisecond taken off the END of a window the carrier is still
	 * alive inside of, and given to an interval they could not yet have been reacting to. At the old
	 * 0.05 the ceiling was ping/2 + 50ms = 103ms against a true leg of 53ms: HALF the slack budget of
	 * a 200ms window was being spent moving the window into the past.
	 *
	 * So the ceiling is now the server's own one-way estimate plus a jitter allowance only. When a
	 * client's clock IS tracking, the stamp is below the ceiling and nothing here binds; when it is
	 * not, the fallback is exactly "rewind by the leg the server measured", which is the right answer
	 * and the one no client can inflate. The header's arithmetic asks for one L back, and this is the
	 * line that keeps it to one L.
	 */
	constexpr float ParryRewindSlackSeconds = 0.01f;
	int32 GLateParrySaveCount = 0;
	int32 GHeldTripCount = 0;
	int32 GHeldTripExpiredCount = 0;

	// =============================================================================================
	// THE DEAD-MAN SWITCH ON HELD TRIPS. MEASURED, and it is the reason this exists.
	//
	// A held trip is only half owned here. This file decides HOW LONG to hold and WHAT the hold
	// resolves to; the trip test owns the pending trip itself and has to keep re-asking each frame
	// until one of those answers comes back. UTraceTrailComponent today does not: it re-asks only
	// while the DASHER IS STILL INTERSECTING THE TRACE, and clears the pending trip the first frame
	// they are not (the `else { PendingTripDasher = nullptr; }` branch of ServerRunTripTest). A dash
	// crosses a ribbon in one or two frames and a hold is three or four, so the hold does not expire
	// into a kill — IT VANISHES, and with it the kill.
	//
	// MEASURED with Trace.Parry.ForceTripHold 0.06 + Trace.TestParry 9: case B (dash, NO parry) left
	// the carrier ALIVE 3 times out of 3, against 4 out of 4 DEAD with no hold. In a real match the
	// hold is on for every REMOTE carrier, so that reads as: the joined player cannot be killed by a
	// dash through their trace. That is a worse client-only bug than the one this item set out to fix.
	//
	// So the hold disarms itself the first time it sees a trip abandoned, and takes the anchoring with
	// it (AreHeldTripsWired gates both, deliberately — an anchored window with no hold is strictly
	// worse than the pre-v8 one, see ServerResolvePress). The build reverts to exactly the Demo 7
	// behaviour, loudly, in one log line, instead of quietly making carriers immortal. When the trip
	// test keeps its pending trip alive for the hold, nothing is ever abandoned and this never fires.
	// =============================================================================================

	/** One lethal trip this process answered KeepHolding to, and when it was last asked about. */
	struct FPendingHeldTripRecord
	{
		TWeakObjectPtr<const AActor> Carrier;
		float TripServerTime = 0.f;
		double LastAskedWallTime = 0.0;
	};

	TArray<FPendingHeldTripRecord> GPendingHeldTrips;

	/**
	 * Seconds without a re-ask after which a held trip is certainly abandoned.
	 *
	 * The trip test re-asks every frame while it holds, so a correctly held trip is never more than
	 * one frame stale, and the longest legal hold is Trace.Parry.MaxTripHold (0.12s). 0.1s is three to
	 * six frames of slack; a hitch that trips it costs one disarm in the SAFE direction.
	 */
	constexpr double AbandonedHoldSeconds = 0.1;

	int32 GAbandonedHoldCount = 0;

	/** Set once, by the dead-man switch. Only Trace.Parry.HeldTrips 1 can override it. */
	int32 GHeldTripsDisarmed = 0;

	/**
	 * Set by TraceParry::NotifyHeldTripsWired() from the trip test. -1 (default) = "ask the build".
	 *
	 * See the header on NotifyHeldTripsWired for why the window's anchor depends on this: anchoring
	 * the start to the press while the server still cannot answer a trip that predates the packet
	 * would hand the client a window that ENDS one upstream lag earlier than it does today, in
	 * exchange for coverage nothing can use. The two halves land together or the first one waits.
	 */
	int32 GHeldTripsWired = 0;

	/** Trace.Parry.HeldTrips — -1 defer to the wiring, 0 force off, 1 force on. */
	int32 GHeldTripsOverride = -1;

	/**
	 * Ceiling on how long a lethal trip may be held for a remote carrier's press to arrive, seconds.
	 *
	 * The hold is the DASHER's cost: for this long their kill is not yet a kill. 0.12 covers a 240ms
	 * round trip, which is past anything the user's group plays on, and it is additionally clamped
	 * by MaxRewindTime so the parry can never be rewound further than a bullet.
	 */
	float GParryMaxTripHold = 0.12f;

	FAutoConsoleVariableRef CVarParryHeldTrips(
		TEXT("Trace.Parry.HeldTrips"),
		GHeldTripsOverride,
		TEXT("SPEC v8 §3. A lethal trip against a REMOTE carrier waits out that carrier's own upstream "
		     "lag before it kills them, so a parry pressed before the dash landed still counts. "
		     "-1 (default) = on wherever the trip test has been wired for it, 0 = force off, 1 = force on."),
		ECVF_Cheat);

	FAutoConsoleVariableRef CVarParryMaxTripHold(
		TEXT("Trace.Parry.MaxTripHold"),
		GParryMaxTripHold,
		TEXT("SPEC v8 §3. Ceiling in seconds on how long a lethal trip is held for a remote carrier's "
		     "parry to arrive. Also clamped by UTraceSettings::MaxRewindTime."),
		ECVF_Default);

	/**
	 * DEBUG. Seconds of trip hold to apply to EVERY carrier, including the host's own pawn and bots.
	 *
	 * WHY IT EXISTS. The hold is the half of v8 §3 that only a REMOTE carrier ever exercises: for a
	 * host or a bot GetTripHoldSeconds() is 0 by design, so every deterministic harness this project
	 * has — Trace.TestParry included — runs straight past it and proves nothing about it. That is the
	 * exact shape of bug this whole pass exists to stop shipping: a path that only clients take, only
	 * ever verified on the machine that does not take it.
	 *
	 * With this set, Trace.TestParry's case B (dash, NO parry) becomes a direct test of whether a held
	 * trip still kills the carrier when it finally expires. If the carrier stops dying, the trip is
	 * being dropped rather than held, and the log says so with a number instead of an argument.
	 */
	float GParryForceTripHold = 0.f;

	FAutoConsoleVariableRef CVarParryForceTripHold(
		TEXT("Trace.Parry.ForceTripHold"),
		GParryForceTripHold,
		TEXT("DEBUG. Seconds of lethal-trip hold to force on EVERY carrier, host and bots included, so "
		     "the remote-only hold path can be exercised by a single-machine harness. 0 (default) = the "
		     "real rule, which holds only for a remote carrier and only for their own upstream lag."),
		ECVF_Cheat);

	FAutoConsoleVariableRef CVarParryForceWindow(
		TEXT("Trace.Parry.ForceWindow"),
		GParryForceWindow,
		TEXT("DEBUG. 1 = every carrier's trace behaves as if permanently parried. Verification only."),
		ECVF_Cheat);

	FAutoConsoleVariableRef CVarParryBotAuto(
		TEXT("Trace.Parry.BotAuto"),
		GParryBotAuto,
		TEXT("DEBUG. 1 = AI carriers parry the instant their cooldown is ready, so a bot match produces "
		     "both parried and unparried dashes through the real code path."),
		ECVF_Cheat);
}

const TCHAR* LexToString(ETraceParryRefusal Refusal)
{
	switch (Refusal)
	{
	case ETraceParryRefusal::None:        return TEXT("granted");
	case ETraceParryRefusal::NoPawn:      return TEXT("no pawn / no trail component");
	case ETraceParryRefusal::Dead:        return TEXT("dead");
	case ETraceParryRefusal::NotCarrying: return TEXT("not carrying the Core");
	case ETraceParryRefusal::OnCooldown:  return TEXT("on cooldown");
	default:                              return TEXT("?");
	}
}


// =================================================================================================
// TraceParry
// =================================================================================================

namespace TraceParry
{
	float GetDurationSeconds()
	{
		const float Value = (GParryDurationOverride >= 0.f)
			? GParryDurationOverride : UTraceSettings::Get().ParryDuration;
		return FMath::Clamp(Value, 0.f, 2.f);
	}

	float GetCooldownSeconds()
	{
		const float Value = (GParryCooldownOverride >= 0.f)
			? GParryCooldownOverride : UTraceSettings::Get().ParryCooldown;
		return FMath::Clamp(Value, 0.f, 30.f);
	}

	FLinearColor GetTintColor()
	{
		// Deliberately NOT clamped or normalised. See the header: G and B are ~0 because the trace is
		// unlit emissive above glow 1 and any channel with weight clips the whole segment to white —
		// the measured "shapeless white slab" defect. A designer who dials G up in Project Settings
		// gets to see that happen, which is more useful than a silent correction.
		return UTraceSettings::Get().ParryTintColor;
	}

	float GetGlowScale()
	{
		const float Value = (GParryGlowScaleOverride >= 0.f)
			? GParryGlowScaleOverride : UTraceSettings::Get().ParryGlowScale;
		return FMath::Clamp(Value, 0.1f, 12.f);
	}

	bool IsWindowForced()
	{
		return GParryForceWindow != 0;
	}

	bool IsBotAutoParryEnabled()
	{
		return GParryBotAuto != 0;
	}

	/** The trail component of @p Actor, or null. Everything below funnels through this. */
	static UTraceTrailComponent* FindTrail(const AActor* Actor)
	{
		const ATraceCharacter* TraceChar = Cast<const ATraceCharacter>(Actor);
		if (TraceChar == nullptr)
		{
			return nullptr;
		}
		return TraceChar->Trail;
	}

	bool RequestParry(ATraceCharacter* Parrier, ETraceParryRefusal* OutRefusal)
	{
		ETraceParryRefusal Refusal = ETraceParryRefusal::None;

		if (Parrier == nullptr || Parrier->Trail == nullptr)
		{
			Refusal = ETraceParryRefusal::NoPawn;
		}
		else
		{
			// The trail component owns the window and the cooldown, so it owns the decision too —
			// there is exactly one implementation of the rules and the server runs it.
			Parrier->Trail->RequestParry(Refusal);
		}

		if (OutRefusal != nullptr)
		{
			*OutRefusal = Refusal;
		}
		return Refusal == ETraceParryRefusal::None;
	}

	bool IsParryActiveFor(const AActor* Actor)
	{
		const UTraceTrailComponent* Trail = FindTrail(Actor);
		return Trail != nullptr && Trail->IsParryActive();
	}

	// ---------------------------------------------------------------------------------------------
	// SPEC v8 §3 — THE CLIENT'S PRESS. See the header for the arithmetic this implements.
	// ---------------------------------------------------------------------------------------------

	float GetPressStampSeconds(const AActor* Parrier)
	{
		if (Parrier == nullptr)
		{
			return 0.f;
		}

		const UWorld* World = Parrier->GetWorld();
		if (World == nullptr)
		{
			return 0.f;
		}

		// The SAME clock UTraceTrailComponent::GetServerTimeSeconds() and UTraceWeaponComponent's
		// shot stamps read, deliberately: a press stamped on one clock and judged on another is the
		// bug this item exists to remove, not a rounding difference.
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return static_cast<float>(GameState->GetServerWorldTimeSeconds());
		}
		return static_cast<float>(World->GetTimeSeconds());
	}

	float GetMaxPressRewindSeconds()
	{
		const UTraceSettings& Settings = UTraceSettings::Get();
		if (!Settings.bEnableLagCompensation)
		{
			// Lag compensation off is a deliberate tuning state for the whole build. A parry that
			// rewound while bullets did not would be the one mechanic ignoring the switch.
			return 0.f;
		}
		return FMath::Clamp(Settings.MaxRewindTime, 0.f, 1.f);
	}

	bool AreHeldTripsWired()
	{
		if (GHeldTripsOverride >= 0)
		{
			return GHeldTripsOverride != 0;   // an explicit console answer outranks everything
		}
		// GHeldTripsDisarmed is the dead-man switch: a trip test that abandons held trips does not get
		// to hold them. See the block comment on FPendingHeldTripRecord.
		return GHeldTripsWired != 0 && GHeldTripsDisarmed == 0;
	}

	void NotifyHeldTripsWired()
	{
		GHeldTripsWired = 1;
	}

	/** Drops records whose window closed more than ParryLedgerKeepSeconds ago, and dead owners. */
	static void SweepParryLedger(float Now)
	{
		GParryWindows.RemoveAll([Now](const FParryWindowRecord& Record)
		{
			return !Record.Parrier.IsValid() || (Now - Record.EndServerTime) > ParryLedgerKeepSeconds;
		});
	}

	bool ServerResolvePress(
		ATraceCharacter* Parrier,
		float ClientPressServerTime,
		float ServerNow,
		float ExistingCooldownEndServerTime,
		float& OutPressServerTime,
		float& OutWindowEndServerTime,
		float& OutCooldownEndServerTime,
		ETraceParryRefusal& OutRefusal)
	{
		OutRefusal = ETraceParryRefusal::None;
		OutPressServerTime = ServerNow;
		OutWindowEndServerTime = ServerNow + GetDurationSeconds();
		OutCooldownEndServerTime = ServerNow + GetCooldownSeconds();

		if (Parrier == nullptr || !Parrier->HasAuthority())
		{
			OutRefusal = ETraceParryRefusal::NoPawn;
			return false;
		}

		// --- 1. anchor the press ------------------------------------------------------------------
		//
		// THE CLAMP IS THE SECURITY, and it is deliberately the same clamp ServerFire applies to a
		// shot: never forward of now (nobody pre-books a window), never further back than
		// MaxRewindTime. A stamp of 0 — bot, host, or a client on an older build — lands on ServerNow
		// and every line below then behaves exactly as the pre-v8 code did.
		//
		// AND A SECOND CLAMP, WHICH IS NOT PARANOIA BUT A MEASUREMENT. On the two-process rig a
		// freshly joined client's GetServerWorldTimeSeconds() was measured running 1.4 to 4.7 SECONDS
		// behind the server's and converging only slowly (see the pass report). A stamp that stale
		// would clamp to ServerNow - MaxRewindTime and anchor the whole window in the past, i.e. a
		// parry that protects nothing — strictly worse than the bug this item exists to fix. So the
		// rewind is additionally bounded by what the SERVER's own ping measurement says is possible
		// for that connection: a press cannot have been in flight longer than the flight time.
		// Nothing the client asserts widens this, which is also why it is the better exploit bound.
		const float MaxRewind = GetMaxPressRewindSeconds();
		float RewindCeiling = MaxRewind;
		if (const APlayerState* ParrierState = Parrier->GetPlayerState())
		{
			// One way + a frame of slack for honest jitter and for the 0.1s clock update interval.
			const float OneWaySeconds = FMath::Max(0.f, ParrierState->GetPingInMilliseconds()) * 0.5f * 0.001f;
			RewindCeiling = FMath::Min(RewindCeiling, OneWaySeconds + ParryRewindSlackSeconds);
		}

		float PressServerTime = ServerNow;
		if (RewindCeiling > 0.f && ClientPressServerTime > 0.f && FMath::IsFinite(ClientPressServerTime))
		{
			PressServerTime = FMath::Clamp(ClientPressServerTime, ServerNow - RewindCeiling, ServerNow);
		}
		const float RewindSeconds = FMath::Max(0.f, ServerNow - PressServerTime);

		// A stamp pinned hard against the ceiling is the signature of a clock that is not tracking
		// the server, and it is worth saying out loud once rather than silently rewinding by the cap
		// every time. Counted, so a session can be asked how often it happened.
		const bool bStampPinned = (ClientPressServerTime > 0.f)
			&& (ClientPressServerTime < ServerNow - RewindCeiling - 0.005f);
		if (bStampPinned)
		{
			++GPressClockPinnedCount;
		}

		// --- 2. the cooldown, asked AT THE PRESS --------------------------------------------------
		//
		// The whole point, in one line: a carrier whose cooldown expired 5ms before they pressed must
		// not be refused because their packet spent 40ms in the air. Same rule, same instant as the
		// window it gates.
		if (ExistingCooldownEndServerTime > 0.f && PressServerTime < ExistingCooldownEndServerTime)
		{
			OutRefusal = ETraceParryRefusal::OnCooldown;
			return false;
		}

		// --- 3. the two deadlines -----------------------------------------------------------------
		//
		// WINDOW END. Anchored to the press only when held trips are wired; until then it keeps its
		// full length from arrival. See NotifyHeldTripsWired() in the header — an anchored start
		// without a hold is a shorter live window in exchange for coverage the server cannot use.
		//
		// COOLDOWN. Always from the press. It is measured from the parry's START by definition
		// (UTraceSettings::ParryCooldown says so), it is the carrier's own clock, and charging a
		// client for their own latency twice — once at the window, once at the next one — is the
		// same unfairness one layer down.
		const float Duration = GetDurationSeconds();
		OutPressServerTime = PressServerTime;
		OutWindowEndServerTime = (AreHeldTripsWired() ? PressServerTime : ServerNow) + Duration;
		OutCooldownEndServerTime = PressServerTime + GetCooldownSeconds();

		// --- 4. file it, so the trip test can be asked about an INSTANT rather than about "now" ----
		SweepParryLedger(ServerNow);
		FParryWindowRecord& Record = GParryWindows.AddDefaulted_GetRef();
		Record.Parrier = Parrier;
		Record.StartServerTime = PressServerTime;
		Record.EndServerTime = OutWindowEndServerTime;
		Record.ArrivalServerTime = ServerNow;

		++GPressCount;
		GPressRewindMsTotal += static_cast<double>(RewindSeconds) * 1000.0;
		GPressRewindMsMax = FMath::Max(GPressRewindMsMax, RewindSeconds * 1000.f);

		// One line per press — a parry is a rare, deliberate act, so this cannot spam — and it is the
		// evidence for the whole item: how much of the window the network ate, before and after.
		UE_LOG(LogTraceGame, Display,
			TEXT("[PARRYNET] %s pressed at t=%.3f, packet arrived t=%.3f: rewound %.1fms "
			     "(ceiling %.0fms of a %.0fms cap%s). window [%.3f..%.3f] (%.0fms), cooldown until %.3f. heldTrips=%d"),
			*GetNameSafe(Parrier), PressServerTime, ServerNow, RewindSeconds * 1000.f,
			RewindCeiling * 1000.f, MaxRewind * 1000.f,
			bStampPinned ? TEXT(", STAMP PINNED - this client's clock is not tracking the server") : TEXT(""),
			OutPressServerTime, OutWindowEndServerTime, (OutWindowEndServerTime - OutPressServerTime) * 1000.f,
			OutCooldownEndServerTime, AreHeldTripsWired() ? 1 : 0);

		return true;
	}

	bool ServerWasParryOpenAt(const AActor* Parrier, float ServerTime)
	{
		if (Parrier == nullptr)
		{
			return false;
		}

		// The debug force outranks everything, exactly as it does in IsParryActive().
		if (IsWindowForced())
		{
			return IsParryActiveFor(Parrier);
		}

		bool bHaveRecord = false;
		for (const FParryWindowRecord& Record : GParryWindows)
		{
			if (Record.Parrier.Get() != Parrier)
			{
				continue;
			}
			bHaveRecord = true;
			if (ServerTime >= Record.StartServerTime && ServerTime < Record.EndServerTime)
			{
				return true;
			}
		}

		// NO STAMPED PRESS ON FILE -> THE PRE-v8 ANSWER, UNCHANGED. Bots, the host's own pawn and any
		// caller that never went through ServerResolvePress keep reading the replicated window and
		// nothing about their game moves. A pawn that DOES have records is judged by them alone,
		// which is what stops "the packet landed late, so read it as open now" creeping back in.
		return bHaveRecord ? false : IsParryActiveFor(Parrier);
	}

	float GetTripHoldSeconds(const ATraceCharacter* Carrier)
	{
		if (Carrier == nullptr || !Carrier->HasAuthority() || !AreHeldTripsWired())
		{
			return 0.f;
		}

		// Verification only, and it outranks the "no connection means no wait" rule below on purpose:
		// the point of the switch is to make a HOST carrier take the remote carrier's code path. Still
		// bounded by the same two ceilings, so it cannot be used to make a carrier unkillable.
		if (GParryForceTripHold > 0.f)
		{
			const float ForcedCeiling = FMath::Min(FMath::Max(0.f, GParryMaxTripHold), GetMaxPressRewindSeconds());
			return FMath::Clamp(GParryForceTripHold, 0.f, ForcedCeiling);
		}

		// A pawn with no owning connection is the host's own or a bot's: their press is already here,
		// so there is nothing in flight to wait for and their game is byte-for-byte the old one.
		if (Carrier->GetNetConnection() == nullptr)
		{
			return 0.f;
		}

		const APlayerState* State = Carrier->GetPlayerState();
		if (State == nullptr)
		{
			return 0.f;
		}

		// Half the round trip = the upstream leg, which is exactly the interval a press sent before
		// this trip can still be in the air. ExactPing is the server's own measurement; nothing the
		// client asserts is involved, so this cannot be inflated by a client that would like longer.
		const float OneWaySeconds = FMath::Max(0.f, State->GetPingInMilliseconds()) * 0.5f * 0.001f;
		const float Ceiling = FMath::Min(FMath::Max(0.f, GParryMaxTripHold), GetMaxPressRewindSeconds());
		return FMath::Clamp(OneWaySeconds, 0.f, Ceiling);
	}

	/**
	 * Notices held trips the caller stopped asking about, and disarms the whole mechanism if it finds
	 * one. Called at the top of every resolve, which is the only moment this file is ever running.
	 */
	static void SweepPendingHeldTrips(const AActor* CurrentCarrier, float CurrentTripServerTime)
	{
		const double Now = FPlatformTime::Seconds();

		for (int32 Index = GPendingHeldTrips.Num() - 1; Index >= 0; --Index)
		{
			const FPendingHeldTripRecord& Record = GPendingHeldTrips[Index];

			// The one being asked about right now is by definition not abandoned.
			if (Record.Carrier.Get() == CurrentCarrier && Record.TripServerTime == CurrentTripServerTime)
			{
				continue;
			}

			const bool bOwnerGone = !Record.Carrier.IsValid();
			if (!bOwnerGone && (Now - Record.LastAskedWallTime) <= AbandonedHoldSeconds)
			{
				continue;
			}

			// A carrier who died or left took their trip with them; that is not an abandonment.
			if (!bOwnerGone)
			{
				++GAbandonedHoldCount;
				if (GHeldTripsDisarmed == 0)
				{
					GHeldTripsDisarmed = 1;
					UE_LOG(LogTraceGame, Warning,
						TEXT("[PARRYNET] HELD TRIPS DISARMED. A lethal trip against %s was held and then never "
						     "asked about again (%.0fms stale), so it resolved to NOTHING - the carrier lived "
						     "and no kill was applied. The trip test must keep re-asking a pending trip until "
						     "it answers KillNow or Parried, INDEPENDENTLY of whether the dasher is still "
						     "touching the trace (UTraceTrailComponent::ServerRunTripTest clears "
						     "PendingTripDasher the first frame they are not). Falling back to the pre-v8 "
						     "window, which is correct if less generous, for the rest of this session. "
						     "Force it back on with Trace.Parry.HeldTrips 1."),
						*GetNameSafe(Record.Carrier.Get()), (Now - Record.LastAskedWallTime) * 1000.0);
				}
			}

			GPendingHeldTrips.RemoveAtSwap(Index);
		}
	}

	/** Files or refreshes the record for a trip this file has just answered KeepHolding to. */
	static void RememberPendingHeldTrip(const AActor* Carrier, float TripServerTime)
	{
		const double Now = FPlatformTime::Seconds();
		for (FPendingHeldTripRecord& Record : GPendingHeldTrips)
		{
			if (Record.Carrier.Get() == Carrier && Record.TripServerTime == TripServerTime)
			{
				Record.LastAskedWallTime = Now;
				return;
			}
		}

		FPendingHeldTripRecord& Added = GPendingHeldTrips.AddDefaulted_GetRef();
		Added.Carrier = Carrier;
		Added.TripServerTime = TripServerTime;
		Added.LastAskedWallTime = Now;
	}

	/** Drops the record for a trip that has now resolved, so the sweep cannot call it abandoned. */
	static void ForgetPendingHeldTrip(const AActor* Carrier, float TripServerTime)
	{
		GPendingHeldTrips.RemoveAllSwap([Carrier, TripServerTime](const FPendingHeldTripRecord& Record)
		{
			return Record.Carrier.Get() == Carrier && Record.TripServerTime == TripServerTime;
		});
	}

	EHeldTrip ServerResolveHeldTrip(const ATraceCharacter* Carrier, float TripServerTime, float HeldSoFarSeconds)
	{
		if (Carrier == nullptr)
		{
			return EHeldTrip::KillNow;
		}

		SweepPendingHeldTrips(Carrier, TripServerTime);

		if (HeldSoFarSeconds <= 0.f)
		{
			++GHeldTripCount;
		}

		// The lag-compensated question, asked about the instant the sweep resolved.
		if (ServerWasParryOpenAt(Carrier, TripServerTime))
		{
			if (HeldSoFarSeconds > 0.f)
			{
				// The press arrived AFTER the trip it answers. This counter is the item's proof: a
				// pre-v8 build had already killed this carrier before their packet landed.
				++GLateParrySaveCount;
				UE_LOG(LogTraceGame, Display,
					TEXT("[PARRYNET] %s: a trip at t=%.3f was answered by a press that arrived %.0fms later "
					     "- carrier lives, dasher is punished. (v8 §3; a pre-v8 build killed them here.)"),
					*GetNameSafe(Carrier), TripServerTime, HeldSoFarSeconds * 1000.f);
			}
			ForgetPendingHeldTrip(Carrier, TripServerTime);
			return EHeldTrip::Parried;
		}

		if (HeldSoFarSeconds >= GetTripHoldSeconds(Carrier))
		{
			++GHeldTripExpiredCount;
			ForgetPendingHeldTrip(Carrier, TripServerTime);
			return EHeldTrip::KillNow;
		}

		// STILL HOLDING, so this trip is now this file's to worry about: file it (or refresh it) and
		// the sweep above will notice if the caller never comes back for the answer.
		RememberPendingHeldTrip(Carrier, TripServerTime);
		return EHeldTrip::KeepHolding;
	}

	void ServerCancelHeldTrip(const ATraceCharacter* Carrier, float TripServerTime)
	{
		// Deliberately NOT counted as abandoned: the caller is telling us the kill became moot, which
		// is a different fact from the caller forgetting to come back. See the header.
		ForgetPendingHeldTrip(Carrier, TripServerTime);
	}

	int32 GetPressCount()            { return GPressCount; }
	int32 GetPressClockPinnedCount() { return GPressClockPinnedCount; }
	float GetMaxPressRewindMs()      { return GPressRewindMsMax; }
	int32 GetLateParrySaveCount()    { return GLateParrySaveCount; }
	int32 GetHeldTripCount()         { return GHeldTripCount; }
	int32 GetHeldTripExpiredCount()  { return GHeldTripExpiredCount; }
	int32 GetAbandonedHeldTripCount(){ return GAbandonedHoldCount; }

	float GetMeanPressRewindMs()
	{
		return (GPressCount > 0) ? static_cast<float>(GPressRewindMsTotal / GPressCount) : 0.f;
	}

	// ---------------------------------------------------------------------------------------------
	// v6 3 — THE PUNISH. See the long comment on the declaration; this is deliberately one
	// straight-line function with no branches other than the rules themselves.
	// ---------------------------------------------------------------------------------------------

	FName GetParryKillCause()
	{
		// Constructed once. Matches the existing cause vocabulary ("Bullet" / "Trail" / "Fell"), which
		// ATraceHUD::DrawDeathPanel prints verbatim as "by <killer>  (<cause>)".
		static const FName ParryKillCause(TEXT("Parried"));
		return ParryKillCause;
	}

	float GetParryKillFeedbackSeconds()
	{
		return FMath::Clamp(GParryKillFeedbackSeconds, 0.f, 10.f);
	}

	int32 GetParryKillCount()
	{
		return GParryKillCount;
	}

	void RecordParryKill(const AActor* Carrier, const AActor* Victim)
	{
		if (Carrier == nullptr)
		{
			return;
		}

		const UWorld* World = Carrier->GetWorld();
		const double Now = (World != nullptr) ? World->GetTimeSeconds() : 0.0;

		// Sweep first: anything stale or whose parrier has been destroyed is gone. This is what keeps
		// the array at "kills in the last couple of seconds" rather than "kills this match".
		const double Window = static_cast<double>(GetParryKillFeedbackSeconds());
		GParryKills.RemoveAll([Now, Window](const FParryKillRecord& Record)
		{
			return !Record.Parrier.IsValid() || (Now - Record.WorldTime) > Window;
		});

		// One record per parrier: the newest kill is the one worth drawing, and two banners fighting
		// over the same slot is worse feedback than one.
		GParryKills.RemoveAll([Carrier](const FParryKillRecord& Record)
		{
			return Record.Parrier.Get() == Carrier;
		});

		FParryKillRecord& Record = GParryKills.AddDefaulted_GetRef();
		Record.Parrier = Carrier;
		Record.VictimName = ParryDisplayName(Victim);
		Record.WorldTime = Now;
	}

	bool GetParryKillFeedback(const AActor* Parrier, float& OutSecondsAgo, FString& OutVictimName)
	{
		OutSecondsAgo = 0.f;
		OutVictimName.Reset();

		if (Parrier == nullptr)
		{
			return false;
		}

		const UWorld* World = Parrier->GetWorld();
		const double Now = (World != nullptr) ? World->GetTimeSeconds() : 0.0;

		for (const FParryKillRecord& Record : GParryKills)
		{
			if (Record.Parrier.Get() != Parrier)
			{
				continue;
			}

			const double Age = Now - Record.WorldTime;
			if (Age < 0.0 || Age > static_cast<double>(GetParryKillFeedbackSeconds()))
			{
				return false;
			}

			OutSecondsAgo = static_cast<float>(Age);
			OutVictimName = Record.VictimName;
			return true;
		}

		return false;
	}

	int32 GetParryCooldownRefundCount() { return GParryCooldownRefundCount; }
	int32 GetDashRefundCount()          { return GDashRefundCount; }
	int32 GetDashRefundUnwiredCount()   { return GDashRefundUnwiredCount; }

	bool IsDashRefundWired()
	{
		// Was a SFINAE probe for UTraceCharacterMovementComponent::RefundDashCharge(). The accessor is
		// integrated, so the call below is a hard compile-time dependency and this cannot be false.
		// Kept because the verifier prints it, and a reader deserves to see the answer is now trivial.
		return true;
	}

	bool ServerRefundOnParryKill(ATraceCharacter* Carrier)
	{
		if (GParryKillRefunds == 0)
		{
			return false;
		}

		// SERVER-AUTHORITATIVE (spec v7 §6). Same test, asked of the same pawn, as the punish that
		// leads here — a client reaching this decides nothing.
		if (Carrier == nullptr || !Carrier->HasAuthority())
		{
			return false;
		}

		bool bRefundedAnything = false;

		// --- 1. "reset that players parry cooldown to zero" ----------------------------------------
		//
		// Unconditional, not "if it is on cooldown": it always IS on cooldown here. The kill can only
		// have happened inside a live parry window, and the cooldown is charged when the window opens.
		float CooldownBefore = 0.f;
		if (UTraceTrailComponent* Trail = Carrier->Trail)
		{
			CooldownBefore = Trail->GetParryCooldownRemaining();

			// The deadline is replicated, and ServerResetParryCooldown() forces the net update itself,
			// so the carrier's own HUD pip and every other machine's idea of it move together on this
			// frame rather than at the next scheduled update — a refund the player cannot see is a
			// refund they will not use.
			Trail->ServerResetParryCooldown();

			++GParryCooldownRefundCount;
			bRefundedAnything = true;
		}

		// --- 2. "and one of their dash cooldowns to zero" ------------------------------------------
		//
		// See the header for why "one of them, the one actually on cooldown" is exactly "hand back one
		// charge" in the pool model the movement component uses.
		int32 ChargesBefore = 0;
		int32 ChargesAfter = 0;
		int32 MaxCharges = 0;
		bool bDashOwed = false;

		if (UTraceCharacterMovementComponent* Movement = Carrier->GetTraceMovement())
		{
			ChargesBefore = Movement->GetDashCharges();
			ChargesAfter = ChargesBefore;
			MaxCharges = FMath::Max(1, Movement->GetMaxDashCharges());

			// "If both are already available, nothing to do" — the spec's own third case.
			bDashOwed = (ChargesBefore < MaxCharges);

			if (bDashOwed)
			{
				// RefundDashCharge() also mirrors the charge onto the owning client, which is the only
				// way a remote carrier's HUD meter can move: DashCharges is predicted saved-move state
				// and is never replicated as a property.
				Movement->RefundDashCharge();

				ChargesAfter = Movement->GetDashCharges();
				if (ChargesAfter > ChargesBefore)
				{
					++GDashRefundCount;
					bRefundedAnything = true;
				}
				else
				{
					// OWED AND NOT PAID. Cannot happen now that the accessor is integrated — the pool
					// was measured short one line above — so this is left as a loud tripwire rather
					// than deleted. The only thing worse than a missing refund is a run that reports
					// one it never made.
					++GDashRefundUnwiredCount;
					UE_LOG(LogTraceGame, Warning,
						TEXT("[PARRYREFUND] %s was owed a dash charge (%d/%d in hand) and "
						     "UTraceCharacterMovementComponent::RefundDashCharge() did not pay it. The "
						     "pool was measured short, so this is a real bug. Parry cooldown was reset."),
						*GetNameSafe(Carrier), ChargesBefore, MaxCharges);
				}
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[PARRYREFUND] %s (spec v7 §6): parry cooldown %.2fs -> %.2fs | dash charges %d -> %d of %d "
			     "(%s) | totals: parry resets %d, dash refunds %d, dash refunds owed but unwired %d"),
			*GetNameSafe(Carrier),
			CooldownBefore, (Carrier->Trail != nullptr) ? Carrier->Trail->GetParryCooldownRemaining() : 0.f,
			ChargesBefore, ChargesAfter, MaxCharges,
			!bDashOwed ? TEXT("both dashes were already available - nothing owed")
				: (ChargesAfter > ChargesBefore ? TEXT("refunded") : TEXT("OWED, NOT PAID")),
			GParryCooldownRefundCount, GDashRefundCount, GDashRefundUnwiredCount);

		return bRefundedAnything;
	}

	bool ServerPunishParriedDash(ATraceCharacter* Carrier, ATraceCharacter* Dasher, float TripServerTime)
	{
		// --- the rule is off ----------------------------------------------------------------------
		if (!ParryKillsDasherEnabled())
		{
			return false;
		}

		// --- the two pawns ------------------------------------------------------------------------
		if (Carrier == nullptr || Dasher == nullptr || Carrier == Dasher)
		{
			return false;
		}

		// --- SERVER-AUTHORITATIVE (spec v6 3). A client reaching here decides nothing. --------------
		//
		// Asked of the CARRIER because the carrier owns the trace and the window; the dasher may be a
		// remote pawn whose authority answer is about a different connection.
		if (!Carrier->HasAuthority())
		{
			return false;
		}

		if (!Carrier->IsAlive() || !Dasher->IsAlive())
		{
			return false;
		}

		// --- the window, from the REPLICATED fact, never a local prediction ------------------------
		//
		// Re-checked rather than trusted from the caller. The caller has it right today, but "the
		// dasher dies" and "the carrier lives" must be two consequences of ONE evaluation of one
		// fact — the failure mode where they disagree is a dead dasher AND a dead carrier, which
		// would read as the mechanic being broken in both directions at once.
		//
		// v8 §3: for a HELD trip the two are tens of milliseconds apart, so the question is asked
		// about the INSTANT THE SWEEP RESOLVED. A live trip passes 0 and gets the v6 answer, from
		// the same replicated window, unchanged.
		if (!((TripServerTime > 0.f) ? ServerWasParryOpenAt(Carrier, TripServerTime) : IsParryActiveFor(Carrier)))
		{
			return false;
		}

		// --- "only a dash that WOULD have killed the carrier is punished" (v6 3 [ASSUMPTION]) -------
		//
		// The geometry half of that sentence is guaranteed by the call site (see the header). This is
		// the other half: under the KillsToucher tuning rule a dash does not kill the carrier at all,
		// so there is no lethal dash to punish and the parry goes back to being purely protective.
		const ETrailLethality Lethality = UTraceSettings::Get().TrailLethality;
		if (Lethality != ETrailLethality::KillsCarrier && Lethality != ETrailLethality::KillsBoth)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYKILL] %s parried %s's dash, but TrailLethality=%d means the dash was never "
				     "lethal to the carrier - nothing to punish (v6 3 [ASSUMPTION])."),
				*GetNameSafe(Carrier), *GetNameSafe(Dasher), static_cast<int32>(Lethality));
			return false;
		}

		UTraceHealthComponent* DasherHealth = Dasher->Health;
		if (DasherHealth == nullptr)
		{
			return false;
		}

		// Resolved BEFORE the kill: UTraceHealthComponent::Kill() broadcasts synchronously and the
		// death path can unpossess, after which GetController() is null and the carrier silently
		// loses the credit. This is the same ordering ApplyTrailTrip() uses, for the same reason.
		AController* CarrierController = Carrier->GetController();

		// ATTRIBUTION. Killer = the carrier's controller, so ATraceGameMode::NotifyCharacterDied
		// credits the carrier with the kill on the scoreboard and sends the dasher
		// ClientNotifyKilledBy(<carrier>, "Parried") — which is the sentence the dasher reads on
		// their own death panel. Cause is its own tag and NOT "Trail": the two are opposite outcomes
		// of the same collision and must never be confused in a log or a feed.
		//
		// Kill() and not ApplyDamage() because the dasher may be shielded for reasons of their own
		// (they could be a carrier too, in a two-Core future) and because a parry is not chip damage:
		// spec v6 3 says "that enemy dies instead".
		// Display, not Log: this is the one line that explains a death to two different players, and
		// a mechanic has twice been declared dead in this project because its evidence sat at a
		// verbosity nobody was capturing. It fires once per parry kill, so it cannot spam.
		UE_LOG(LogTraceGame, Display,
			TEXT("[PARRYKILL] %s PARRIED %s's dash and killed them (v6 3). window=%.3fs left, cause=%s, "
			     "killer=%s. The dash WOULD have killed the carrier; it killed the dasher instead."),
			*GetNameSafe(Carrier), *GetNameSafe(Dasher),
			GetWindowRemainingFor(Carrier), *GetParryKillCause().ToString(),
			*GetNameSafe(CarrierController));

		++GParryKillCount;
		RecordParryKill(Carrier, Dasher);

		// v6 §3 FEEDBACK, CARRIER SIDE. The RECORD above already drives the banner on a listen
		// server, which is where the host — the only human in a bot playtest — actually plays. This
		// RPC is what carries it to a REMOTE client's screen, which has no access to that record.
		// Sent before the kill for the same reason CarrierController was resolved before it: the
		// death path below can unpossess, and an RPC through a null controller goes nowhere.
		if (ATracePlayerController* CarrierPC = Cast<ATracePlayerController>(CarrierController))
		{
			CarrierPC->ClientNotifyParryKill(ParryDisplayName(Dasher));
		}

		DasherHealth->Kill(CarrierController, GetParryKillCause());

		// SPEC v7 §6, AFTER THE KILL AND NOT BEFORE IT. The refund is the reward for a KILL, so it is
		// applied on the far side of the one line that makes the kill true. Ordering matters for a
		// second reason too: Kill() broadcasts synchronously and the death chain can move the Core,
		// and the refund is a statement about the pawn that parried rather than about whoever is
		// holding the Core a microsecond later - it is deliberately not re-derived from the Core here.
		ServerRefundOnParryKill(Carrier);
		return true;
	}

	float GetWindowRemainingFor(const AActor* Actor)
	{
		const UTraceTrailComponent* Trail = FindTrail(Actor);
		return Trail != nullptr ? Trail->GetParryWindowRemaining() : 0.f;
	}

	float GetCooldownRemainingFor(const AActor* Actor)
	{
		const UTraceTrailComponent* Trail = FindTrail(Actor);
		return Trail != nullptr ? Trail->GetParryCooldownRemaining() : 0.f;
	}

	float GetCooldownTotal()
	{
		return GetCooldownSeconds();
	}
}


// =================================================================================================
// Debug and verification
//
// Three console commands, all #if !UE_BUILD_SHIPPING. Drive them unattended with
// -ExecCmds="Trace.TestParry 8" — they self-schedule on the core ticker until a match exists, so it
// does not matter that -ExecCmds fires long before there is a possessed pawn.
//
//   Trace.Parry                 — parry with the local pawn, through the real entry point.
//   Trace.DebugParry            — dump every trace's parry / pass-window / tint state.
//   Trace.TestParry [Runs]      — the deterministic proof, alternating parried and unparried dashes
//                                 through a live carrier's trace. See the comment on the command.
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace
{
	/** Whichever world is actually playing. Mirrors the helper in TraceCharacter.cpp's debug block. */
	UWorld* FindParryDebugWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/** The local player's pawn, or null. */
	ATraceCharacter* FindParryDebugLocalCharacter(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (PC != nullptr && PC->IsLocalController())
			{
				if (ATraceCharacter* TraceChar = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					return TraceChar;
				}
			}
		}
		return nullptr;
	}

	/** Every character the match knows about, GameMode list with an actor-iterator fallback. */
	void GatherParryDebugCharacters(UWorld* World, TArray<ATraceCharacter*>& OutCharacters)
	{
		OutCharacters.Reset();
		if (World == nullptr)
		{
			return;
		}

		if (const ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			for (const TWeakObjectPtr<ATraceCharacter>& Weak : GameMode->GetTrackedCharacters())
			{
				if (ATraceCharacter* TraceChar = Weak.Get())
				{
					OutCharacters.Add(TraceChar);
				}
			}
		}

		if (OutCharacters.Num() == 0)
		{
			for (TActorIterator<ATraceCharacter> It(World); It; ++It)
			{
				if (ATraceCharacter* TraceChar = *It)
				{
					OutCharacters.Add(TraceChar);
				}
			}
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.TestParry — THE DETERMINISTIC PROOF.
	//
	// The claim under test is a conditional ("parrying as they dash protects the trace"), so a single
	// outcome proves nothing: the harness has to show every branch, from the same code path, with
	// nothing else changed between them. Spec v6 §3 added a third branch and a second victim, so the
	// harness now CYCLES THROUGH THREE CASES and reports the fate of BOTH pawns in each:
	//
	//   case A  parry + dash    -> the dasher dies, the carrier lives      (the v6 §3 punish)
	//   case B  dash, no parry  -> the carrier dies, the dasher lives      (the unchanged old rule)
	//   case C  parry, no dash  -> nobody dies                             (v6 §3 [ASSUMPTION])
	//
	// Case C is not padding. The [ASSUMPTION] the punish is built on is *"a parry thrown at nothing
	// kills nobody"*, and a harness that only ever parries into an incoming dash cannot tell a
	// correctly-gated punish apart from one that kills the nearest enemy on every parry press. So C
	// presses the parry, moves nobody, and asserts that both pawns are alive AND that the global
	// parry-kill counter did not move.
	//
	// The dash is two teleports on consecutive frames rather than a bot running at the trace, for the
	// same reason ATraceGameMode's -TraceTripTest does it that way: what the trip test actually
	// evaluates is the swept segment between two of its ticks, so reproducing that segment directly
	// is both exact and repeatable. StartDash() is called on the server pawn so the "only a DASH
	// trips the trace" rule is genuinely satisfied and not bypassed.
	//
	// Note the deliberate difference from -TraceTripTest: that harness forces a TURNOVER first and
	// then dashes through the ex-carrier's residual trace. This one leaves the carrier CARRYING,
	// because the parry is carrier-only — a residual trace has nobody left who is allowed to parry it.
	// ---------------------------------------------------------------------------------------------

	/** Which of the three cases above a run is exercising. RunIndex % 3, so a session cycles. */
	enum class EParryTestCase : uint8
	{
		ParriedDash = 0,     // A
		UnparriedDash = 1,   // B
		ParryNoDash = 2,     // C
	};

	/**
	 * Deliberately NOT another LexToString overload. This one would live in the anonymous namespace
	 * and would therefore HIDE the global LexToString(ETraceParryRefusal) for every call in this
	 * block — which today still compiles only because argument-dependent lookup rescues it. A
	 * distinct name costs nothing and removes the trap.
	 */
	const TCHAR* ParryTestCaseName(EParryTestCase Case)
	{
		switch (Case)
		{
		case EParryTestCase::ParriedDash:   return TEXT("A parry+dash");
		case EParryTestCase::UnparriedDash: return TEXT("B dash,no parry");
		case EParryTestCase::ParryNoDash:   return TEXT("C parry,no dash");
		default:                            return TEXT("?");
		}
	}

	/** One in-flight Trace.TestParry session. Captured by value into the ticker, mutated in place. */
	struct FParryTestState
	{
		int32 TotalRuns = 6;
		int32 RunIndex = 0;

		/** Frames since this run's phase 0. */
		int32 Phase = 0;

		/**
		 * Frames spent waiting on a carrier who EXISTS but is not yet usable (trace still short, or a
		 * previous parry still cooling down), so a hopeless session gives up instead of leaking.
		 */
		int32 WaitFrames = 0;

		/**
		 * Frames spent with no living carrier at all — the match not being live yet (travel, warmup,
		 * half-time break, Core loose between carriers). Budgeted separately and far more generously
		 * than WaitFrames; see the note in phase 0.
		 */
		int32 IdleFrames = 0;

		TWeakObjectPtr<ATraceCharacter> Carrier;
		TWeakObjectPtr<ATraceCharacter> Tripper;
		FVector DashEnd = FVector::ZeroVector;

		/**
		 * Where the tripper stood before the harness picked it up.
		 *
		 * It is put straight back there the frame AFTER the sweep, and that is load-bearing rather
		 * than tidiness — see phase 3.
		 */
		FVector TripperHome = FVector::ZeroVector;

		EParryTestCase Case = EParryTestCase::ParriedDash;
		bool bParryThisRun = false;
		bool bDashThisRun = true;
		bool bParryWasActiveAtSweep = false;

		/** GetParryKillCount() sampled at the start of the run, so case C can assert "+0". */
		int32 ParryKillsAtRunStart = 0;

		// --- SPEC v7 §6, the refunds. Sampled either side of the sweep. ------------------------
		//
		// The whole claim is a CHANGE across one event, so both ends have to be measured on the
		// carrier the harness actually used. Sampling only the "after" state would pass trivially
		// against a carrier who happened to have a full pool and a ready parry to begin with, which
		// is the usual state of a bot that has not been made to spend anything.
		float ParryCooldownBeforeSweep = 0.f;
		float ParryCooldownAfterKill = 0.f;
		int32 DashChargesBeforeSweep = 0;
		int32 DashChargesAfterKill = 0;
		int32 DashMaxChargesAtSweep = 0;
		bool bRefundSampled = false;

		/** Attempts scratched for this run index (dash refused, no tripper). Bounded, then aborted. */
		int32 ScratchCount = 0;

		// --- tallies, reported at the end -----------------------------------------------------
		int32 CaseARuns = 0;   // parry + dash
		int32 CaseBRuns = 0;   // dash, no parry
		int32 CaseCRuns = 0;   // parry, no dash

		/** Outcomes, counted across all cases. These three are mutually exclusive per run. */
		int32 DasherDiedCount = 0;
		int32 CarrierDiedCount = 0;
		int32 NobodyDiedCount = 0;

		/** Both dying at once would mean the punish and the protection disagreed. Must stay 0. */
		int32 BothDiedCount = 0;

		int32 PassCount = 0;
		int32 FailCount = 0;
		int32 AbortedCount = 0;

		// --- SPEC v7 §6 tallies, reported at the end -------------------------------------------
		int32 RefundRunsMeasured = 0;
		int32 ParryCooldownResetsConfirmed = 0;
		int32 ParryCooldownResetsMissed = 0;
		int32 DashRefundsConfirmed = 0;
		int32 DashRefundsOwedButUnwired = 0;
		int32 DashRefundsNotOwed = 0;
		int32 RefundPassCount = 0;
		int32 RefundFailCount = 0;
	};

	/** Advances one Trace.TestParry session by one frame. Returns false when the session is done. */
	bool TickParryTest(FParryTestState& State)
	{
		UWorld* World = FindParryDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		if (State.RunIndex >= State.TotalRuns)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST] DONE. %d runs (A parry+dash=%d, B dash-no-parry=%d, C parry-no-dash=%d, aborted=%d)"),
				State.TotalRuns, State.CaseARuns, State.CaseBRuns, State.CaseCRuns, State.AbortedCount);
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST] OUTCOMES: dasher died=%d, carrier died=%d, nobody died=%d, BOTH died=%d (must be 0)."),
				State.DasherDiedCount, State.CarrierDiedCount, State.NobodyDiedCount, State.BothDiedCount);
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST] VERDICT: %d PASS, %d FAIL. Total parry kills awarded this process: %d."),
				State.PassCount, State.FailCount, TraceParry::GetParryKillCount());
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST] v7 §6 REFUND VERDICT: %d PASS, %d FAIL (scored apart from the v6 §3 verdict ")
				TEXT("above, which is about who died)."),
				State.RefundPassCount, State.RefundFailCount);
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST] v7 §6 REFUNDS: %d kills measured | parry cooldown reset %d, NOT reset %d | ")
				TEXT("dash refunded %d, owed-but-unwired %d, nothing owed %d | process totals: parry resets %d, ")
				TEXT("dash refunds %d, unwired %d (accessor wired=%d)"),
				State.RefundRunsMeasured,
				State.ParryCooldownResetsConfirmed, State.ParryCooldownResetsMissed,
				State.DashRefundsConfirmed, State.DashRefundsOwedButUnwired, State.DashRefundsNotOwed,
				TraceParry::GetParryCooldownRefundCount(), TraceParry::GetDashRefundCount(),
				TraceParry::GetDashRefundUnwiredCount(), TraceParry::IsDashRefundWired() ? 1 : 0);
			return false;
		}

		// ---- phase 0: wait for a carrier with enough established trace to aim at -----------------
		if (State.Phase == 0)
		{
			ATraceCore* TheCore = ATraceCore::Get(World);
			ATraceCharacter* Carrier = (TheCore != nullptr) ? TheCore->GetCarrier() : nullptr;

			const int32 LethalPoints = (Carrier != nullptr && Carrier->Trail != nullptr)
				? Carrier->Trail->ComputeLastLethalIndex() + 1
				: 0;

			// A run may not begin until the PREVIOUS run's parry has fully lapsed — window closed AND
			// cooldown expired. Without this the runs come ~75ms apart, so an "unparried" run inherits
			// the previous run's still-open window and the harness never actually tests the
			// negative branch (measured: run 2 reported parryActive=1 with 0.016s left). Waiting for
			// the cooldown is also what makes the odd/even alternation real rather than aspirational.
			const bool bParryStateClear = (Carrier == nullptr)
				|| (!TraceParry::IsParryActiveFor(Carrier) && TraceParry::GetCooldownRemainingFor(Carrier) <= 0.f);

			if (Carrier == nullptr || !Carrier->IsAlive() || LethalPoints < 8 || !bParryStateClear)
			{
				// TWO SEPARATE BUDGETS, because the two waits have completely different natural lengths
				// and merging them is what made this harness unusable from -ExecCmds.
				//
				// -ExecCmds fires at map load, long before the match is live: there is no Core carrier
				// during travel, warmup or the half-time break, and on a bot match it can easily be a
				// minute before a bot picks the Core up and holds it long enough to lay 8 points. The
				// old single 1800-frame (~30s) budget was therefore spent almost entirely on "the match
				// has not started yet" and the session aborted before it could test anything —
				// measured: gave up at t+27s having seen 1 lethal point, while the same run had a
				// carrier with 25-30 lethal points ~70s later.
				//
				// So: NO CARRIER AT ALL is not progress-less waiting, it is the match not being ready,
				// and it gets a long budget. Waiting on a carrier who exists but has a short trace (or
				// a parry still cooling down) is the real bounded wait and keeps the tight one.
				if (Carrier == nullptr || !Carrier->IsAlive())
				{
					if (++State.IdleFrames > 36000)   // ~10 min at 60Hz: "no carrier ever appeared"
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[PARRYTEST] gave up: no living Core carrier ever appeared."));
						return false;
					}
					return true;
				}

				if (++State.WaitFrames > 1800)   // ~30s at 60Hz, with a carrier present the whole time
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[PARRYTEST] gave up waiting for a carrier with 8+ lethal trace points (had %d)."),
						LethalPoints);
					return false;
				}
				return true;
			}

			State.IdleFrames = 0;

			State.WaitFrames = 0;
			State.Carrier = Carrier;

			// THE THREE-CASE CYCLE. Derived from RunIndex so a session of 6 runs is two full cycles
			// and every case is exercised the same number of times.
			State.Case = static_cast<EParryTestCase>(State.RunIndex % 3);
			State.bParryThisRun = (State.Case != EParryTestCase::UnparriedDash);
			State.bDashThisRun = (State.Case != EParryTestCase::ParryNoDash);
			State.ParryKillsAtRunStart = TraceParry::GetParryKillCount();
			State.Phase = 1;
			return true;
		}

		ATraceCharacter* Carrier = State.Carrier.Get();

		// PHASES 1-2 ONLY. Setting the scene needs a live carrier, and losing them there (shot by
		// somebody, a goal, half time) is a scratched run rather than a result.
		//
		// Phase 3+ is the SCORING window and must NOT bail out here: a dead carrier is the EXPECTED
		// outcome of an unparried dash, and treating it as "aborted" would silently delete every
		// negative sample — leaving a run that reports only the parried cases and looks like a pass.
		if (State.Phase <= 2 && (Carrier == nullptr || !Carrier->IsAlive() || Carrier->Trail == nullptr))
		{
			UE_LOG(LogTraceGame, Display, TEXT("[PARRYTEST %d] aborted: carrier gone before the sweep resolved."),
				State.RunIndex + 1);
			++State.AbortedCount;
			++State.RunIndex;
			State.Phase = 0;
			State.Carrier = nullptr;
			State.Tripper = nullptr;
			return true;
		}

		// ---- phase 1: request the parry (or not), line the tripper up, start the dash ------------
		if (State.Phase == 1)
		{
			const int32 LastLethal = Carrier->Trail->ComputeLastLethalIndex();
			if (LastLethal < 2)
			{
				State.Phase = 0;
				return true;
			}

			// Halfway down the lethal set: comfortably behind the exempt head stub, comfortably
			// inside its lifetime.
			const int32 SegmentIndex = FMath::Clamp(LastLethal / 2, 0, LastLethal - 1);
			const FVector SegmentStart = Carrier->Trail->TrailPoints.Items[SegmentIndex].Location;
			const FVector SegmentEnd = Carrier->Trail->TrailPoints.Items[SegmentIndex + 1].Location;

			FVector Along = SegmentEnd - SegmentStart;
			Along.Z = 0.0;
			if (!Along.Normalize())
			{
				State.Phase = 0;
				return true;
			}
			const FVector Across = FVector::CrossProduct(Along, FVector::UpVector).GetSafeNormal();
			const FVector Midpoint = (SegmentStart + SegmentEnd) * 0.5;

			TArray<ATraceCharacter*> Candidates;
			GatherParryDebugCharacters(World, Candidates);

			TArray<ATraceCharacter*> Enemies;
			for (ATraceCharacter* Candidate : Candidates)
			{
				if (Candidate != nullptr && Candidate != Carrier && Candidate->IsAlive()
					&& Candidate->GetTeam() != ETraceTeam::None
					&& Candidate->GetTeam() != Carrier->GetTeam())
				{
					Enemies.Add(Candidate);
				}
			}
			if (Enemies.Num() == 0)
			{
				State.Phase = 0;
				return true;
			}

			// ROTATE THE TRIPPER ON EVERY RETRY. StartDash() is refused while that pawn's own dash is
			// on cooldown, and always taking the first enemy in the roster meant the same bot was
			// asked again and again — measured: every UNPARRIED run aborted after nine attempts,
			// because the preceding parried run had just spent that one bot's dash. Deleting exactly
			// the negative half of the sample is the worst possible failure for this harness, so the
			// retry walks the roster instead of retrying one pawn.
			ATraceCharacter* Tripper = Enemies[State.ScratchCount % Enemies.Num()];

			// THE PARRY, through the real entry point, on the frame before the sweep resolves. A
			// 0.2s window is ~12 frames, so it is still open when the trip test runs next frame —
			// which is precisely the "parrying as they dash" timing the spec describes.
			ETraceParryRefusal Refusal = ETraceParryRefusal::None;
			bool bParryGranted = false;
			if (State.bParryThisRun)
			{
				bParryGranted = TraceParry::RequestParry(Carrier, &Refusal);
			}

			// SPEC v7 §6: MAKE THERE BE SOMETHING TO REFUND.
			//
			// The refund is "hand back the dash that is on cooldown", and a carrier who has not spent
			// one has nothing owed to them — the spec's own third case ("if both are available,
			// nothing to do"). A harness that never spent one would therefore assert nothing at all
			// and would pass against a completely dead refund path. So the carrier spends a charge
			// here, through the real StartDash() the player's key drives, and the assertion below is
			// about a pool that is genuinely short.
			if (State.Case == EParryTestCase::ParriedDash)
			{
				if (UTraceCharacterMovementComponent* CarrierMovement = Carrier->GetTraceMovement())
				{
					CarrierMovement->StartDash();
				}
			}

			const FVector DashStart = Midpoint + Across * 260.0;
			State.DashEnd = Midpoint - Across * 240.0;
			State.Tripper = Tripper;
			State.TripperHome = Tripper->GetActorLocation();

			// CASE C moves nobody and starts no dash. The enemy is recorded anyway — as the WITNESS,
			// not the dasher — because "nobody dies" has to be asserted about a specific, named,
			// still-standing enemy for the result to mean anything.
			if (State.bDashThisRun)
			{
				Tripper->SetActorLocation(DashStart, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
				Tripper->SetActorRotation((-Across).Rotation());
				if (UTraceCharacterMovementComponent* TripperMovement = Tripper->GetTraceMovement())
				{
					TripperMovement->StartDash();
				}
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST %d/%d] case=%s carrier=%s parryRequested=%d granted=%d (%s) window=%.3fs cooldown=%.2fs "
				     "| %s %s segment %d/%d at %s"),
				State.RunIndex + 1, State.TotalRuns, ParryTestCaseName(State.Case), *GetNameSafe(Carrier),
				State.bParryThisRun ? 1 : 0, bParryGranted ? 1 : 0, LexToString(Refusal),
				TraceParry::GetWindowRemainingFor(Carrier), TraceParry::GetCooldownRemainingFor(Carrier),
				*GetNameSafe(Tripper),
				State.bDashThisRun ? TEXT("will dash across") : TEXT("stays home, witness to"),
				SegmentIndex, LastLethal, *Midpoint.ToCompactString());

			// A case-C run whose parry was REFUSED tests nothing at all — the assertion "a parry that
			// hits nothing kills nobody" needs a parry. Scratch and retry at the same run index.
			if (State.Case == EParryTestCase::ParryNoDash && !bParryGranted)
			{
				if (++State.ScratchCount > 20)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[PARRYTEST %d] aborted: parry never granted for the no-dash case (%s)."),
						State.RunIndex + 1, LexToString(Refusal));
					++State.AbortedCount;
					++State.RunIndex;
					State.ScratchCount = 0;
				}
				State.Phase = 0;
				State.Carrier = nullptr;
				State.Tripper = nullptr;
				return true;
			}

			State.Phase = 2;
			return true;
		}

		// ---- phase 2: the second half of the sweep. This is the frame the trip test resolves. ----
		if (State.Phase == 2)
		{
			ATraceCharacter* Tripper = State.Tripper.Get();
			if (Tripper == nullptr)
			{
				State.Phase = 0;
				return true;
			}

			// CASE C: no sweep to resolve. Record that the window really was open with nothing dashing
			// into it — which is the exact situation the [ASSUMPTION] is about — and fall through to
			// the same settle-and-score tail as the other two cases.
			if (!State.bDashThisRun)
			{
				State.bParryWasActiveAtSweep = TraceParry::IsParryActiveFor(Carrier);

				UE_LOG(LogTraceGame, Display,
					TEXT("[PARRYTEST %d/%d] no dash this run: parryActive=%d (%.3fs left) traceInvuln=%d, %s untouched at %s"),
					State.RunIndex + 1, State.TotalRuns,
					State.bParryWasActiveAtSweep ? 1 : 0, TraceParry::GetWindowRemainingFor(Carrier),
					Carrier->Trail->IsTraceInvulnerable() ? 1 : 0,
					*GetNameSafe(Tripper), *Tripper->GetActorLocation().ToCompactString());

				State.Phase = 3;
				return true;
			}

			// THE DASH MUST ACTUALLY HAVE STARTED, or this run tests nothing.
			//
			// StartDash() is a request, and the movement component refuses it on cooldown. Measured:
			// two runs in ten had dashing=0 at this point, no trip test fired at all, and the carrier
			// "survived" — which the scoring then read as the parry having worked. A negative result
			// produced by an absent dash is worse than no result, so the run is SCRATCHED and retried
			// at the same run index, preserving the parried/unparried alternation.
			if (!Tripper->IsDashing())
			{
				if (++State.ScratchCount > 20)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[PARRYTEST %d] aborted: %s would not dash after %d attempts (cooldown?)."),
						State.RunIndex + 1, *GetNameSafe(Tripper), State.ScratchCount);
					++State.AbortedCount;
					++State.RunIndex;
					State.ScratchCount = 0;
				}
				else
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[PARRYTEST %d] scratched: %s did not enter a dash, so no sweep would be tested. Retrying."),
						State.RunIndex + 1, *GetNameSafe(Tripper));
				}

				Tripper->SetActorLocation(State.TripperHome, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
				State.Phase = 0;
				State.Carrier = nullptr;
				State.Tripper = nullptr;
				return true;
			}

			State.bParryWasActiveAtSweep = TraceParry::IsParryActiveFor(Carrier);

			// SPEC v7 §6 BASELINE, taken on the frame the sweep resolves and BEFORE it does: the core
			// ticker runs ahead of the world tick that will run the trip test, so this is the last
			// reading of the carrier's cooldowns that predates the kill.
			if (UTraceCharacterMovementComponent* CarrierMovement = Carrier->GetTraceMovement())
			{
				State.DashChargesBeforeSweep = CarrierMovement->GetDashCharges();
				State.DashMaxChargesAtSweep = FMath::Max(1, CarrierMovement->GetMaxDashCharges());
				State.bRefundSampled = true;
			}
			State.ParryCooldownBeforeSweep = TraceParry::GetCooldownRemainingFor(Carrier);

			Tripper->SetActorLocation(State.DashEnd, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);

			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST %d/%d] sweep resolves now: dashing=1 parryActive=%d (%.3fs left) traceInvuln=%d"),
				State.RunIndex + 1, State.TotalRuns,
				State.bParryWasActiveAtSweep ? 1 : 0, TraceParry::GetWindowRemainingFor(Carrier),
				Carrier->Trail->IsTraceInvulnerable() ? 1 : 0);

			State.Phase = 3;
			return true;
		}

		// ---- phase 3: get the tripper OFF the trace, so the run measures ONE sweep ----------------
		//
		// This is not cleanup; without it the harness measures the wrong thing. The tripper is a bot
		// that was hunting the trace: left where the sweep put it, it stays inside the lethal volume
		// and inside its own dash, and the trip test fires again on EVERY following frame. Measured
		// against a 0.1s parry: four consecutive frames of "NO KILL - PARRIED", and then the window
	// lapsed (v8's 0.2s only makes that run of frames longer, not different)
		// lapsed and the fifth frame killed the carrier — a correct outcome that the scoring below
		// would have read as the parry having failed.
		//
		// The parry buys 0.2 seconds, not immunity. Isolating the single sweep is what lets this
		// harness answer the question the spec actually asks ("parrying AS they dash protects the
		// trace") instead of the different question "does a parry survive a camper".
		if (State.Phase == 3)
		{
			// SPEC v7 §6, THE OTHER END OF THE MEASUREMENT. One frame after the sweep, so the trip
			// test, the punish, the kill and the refund have all run. Read from the carrier's own
			// components, which is where the HUD reads them, rather than from the refund's counters:
			// a counter proves the code ran, and this proves the state the player sees changed.
			if (Carrier != nullptr && State.bRefundSampled)
			{
				State.ParryCooldownAfterKill = TraceParry::GetCooldownRemainingFor(Carrier);
				if (const UTraceCharacterMovementComponent* CarrierMovement = Carrier->GetTraceMovement())
				{
					State.DashChargesAfterKill = CarrierMovement->GetDashCharges();
				}
			}

			// Nothing to undo in case C, and a parry-killed dasher is a corpse the death path is
			// already hiding — moving either one would only add noise.
			if (State.bDashThisRun)
			{
				if (ATraceCharacter* Tripper = State.Tripper.Get())
				{
					if (Tripper->IsAlive())
					{
						Tripper->SetActorLocation(State.TripperHome, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
					}
				}
			}
			State.Phase = 4;
			return true;
		}

		// ---- phase 4+: give the kill a couple of frames to land, then score the run --------------
		if (State.Phase < 7)
		{
			++State.Phase;
			return true;
		}

		{
			// EITHER pawn may legitimately be NULL here: an unparried dash kills the carrier and a
			// parried dash now kills the dasher, and the loser's pawn can be gone by the time we
			// score. Both are expected results, not crashes.
			ATraceCharacter* Dasher = State.Tripper.Get();
			const bool bCarrierAlive = (Carrier != nullptr) && Carrier->IsAlive();
			const bool bDasherAlive = (Dasher != nullptr) && Dasher->IsAlive();
			const int32 KillsAwarded = TraceParry::GetParryKillCount() - State.ParryKillsAtRunStart;

			// THE EXPECTATION IS DERIVED FROM WHAT WAS OBSERVED, NOT FROM WHAT WAS REQUESTED.
			//
			// bParryWasActiveAtSweep is the REPLICATED window read on the very frame the sweep
			// resolved — the same fact ServerRunTripTest read. Scoring against the *request* instead
			// would turn "the parry lapsed a frame early" into a spurious FAIL and, worse, would let a
			// genuinely broken window pass whenever the request happened to be refused.
			bool bExpectCarrierAlive = true;
			bool bExpectDasherAlive = true;
			if (State.bDashThisRun)
			{
				// A dash landed. Exactly one of the two dies, and which one is the whole of v6 §3.
				bExpectCarrierAlive = State.bParryWasActiveAtSweep;
				bExpectDasherAlive = !State.bParryWasActiveAtSweep;
			}

			const int32 ExpectedKills = (State.bDashThisRun && State.bParryWasActiveAtSweep) ? 1 : 0;

			// --- SPEC v7 §6: the refunds, scored only on a run that actually produced a KILL --------
			//
			// Gated on ExpectedKills rather than on the case, for the same reason the fates above are
			// gated on the OBSERVED window: a case-A run whose parry lapsed a frame early produces no
			// kill, and asserting a refund there would fail the refund for the timing's mistake.
			bool bRefundOk = true;
			if (ExpectedKills == 1 && KillsAwarded == 1 && State.bRefundSampled && bCarrierAlive)
			{
				++State.RefundRunsMeasured;

				// "reset that players parry cooldown to zero" - and it is a RESET, so the number that
				// matters is the one after, not the delta. A carrier whose cooldown happened to be
				// zero already is still a pass; a carrier still counting down is not.
				const bool bCooldownOk = (State.ParryCooldownAfterKill <= 0.001f);
				(bCooldownOk ? State.ParryCooldownResetsConfirmed : State.ParryCooldownResetsMissed)++;

				// "one of their dash cooldowns to zero. If the first dash is already off cooldown, the
				// second one should be set to zero instead" = one charge back, unless the pool is full.
				const bool bDashOwed = (State.DashChargesBeforeSweep < State.DashMaxChargesAtSweep);
				bool bDashOk = true;
				if (!bDashOwed)
				{
					++State.DashRefundsNotOwed;
				}
				else if (State.DashChargesAfterKill > State.DashChargesBeforeSweep)
				{
					++State.DashRefundsConfirmed;
				}
				else if (!TraceParry::IsDashRefundWired())
				{
					// The accessor this build needs is not there. Recorded as its own outcome and NOT
					// as a pass: the mechanic is genuinely missing, and a harness that shrugged at that
					// would be the reason nobody noticed.
					++State.DashRefundsOwedButUnwired;
					bDashOk = false;
				}
				else
				{
					bDashOk = false;
				}

				bRefundOk = bCooldownOk && bDashOk;
				(bRefundOk ? State.RefundPassCount : State.RefundFailCount)++;

				UE_LOG(LogTraceGame, Display,
					TEXT("[PARRYTEST %d/%d] v7 §6 REFUND: parry cooldown %.2fs -> %.2fs (%s) | dash charges "
					     "%d -> %d of %d (%s) | accessor wired=%d"),
					State.RunIndex + 1, State.TotalRuns,
					State.ParryCooldownBeforeSweep, State.ParryCooldownAfterKill,
					bCooldownOk ? TEXT("RESET") : TEXT("*** NOT RESET ***"),
					State.DashChargesBeforeSweep, State.DashChargesAfterKill, State.DashMaxChargesAtSweep,
					!bDashOwed ? TEXT("nothing owed - the pool was full")
						: (bDashOk ? TEXT("REFUNDED") : TEXT("*** OWED, NOT REFUNDED ***")),
					TraceParry::IsDashRefundWired() ? 1 : 0);
			}

			// TWO VERDICTS, KEPT APART ON PURPOSE. bPass is the v6 §3 claim - who died - and it must
			// stay comparable with the 12/12 this harness has reported since that spec landed. Folding
			// a missing v7 §6 refund into it would turn a working punish into a FAIL line and invite
			// exactly the wrong diagnosis. The refund gets its own verdict, counted below and printed
			// on its own line, so a gap in it is loud without being mistaken for a regression.
			const bool bPass = (bCarrierAlive == bExpectCarrierAlive)
				&& (bDasherAlive == bExpectDasherAlive)
				&& (KillsAwarded == ExpectedKills);

			switch (State.Case)
			{
			case EParryTestCase::ParriedDash:   ++State.CaseARuns; break;
			case EParryTestCase::UnparriedDash: ++State.CaseBRuns; break;
			case EParryTestCase::ParryNoDash:   ++State.CaseCRuns; break;
			}

			if (!bCarrierAlive && !bDasherAlive)
			{
				// The punish and the protection disagreed about one window. Called out separately
				// because it is the one failure that would look "half correct" in the other tallies.
				++State.BothDiedCount;
			}
			else if (!bDasherAlive)
			{
				++State.DasherDiedCount;
			}
			else if (!bCarrierAlive)
			{
				++State.CarrierDiedCount;
			}
			else
			{
				++State.NobodyDiedCount;
			}

			(bPass ? State.PassCount : State.FailCount)++;

			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST %d/%d] RESULT case=%s: parryActive=%d dashed=%d -> carrier %s (%s), dasher %s (%s), "
				     "parryKills+%d (expected +%d). %s"),
				State.RunIndex + 1, State.TotalRuns, ParryTestCaseName(State.Case),
				State.bParryWasActiveAtSweep ? 1 : 0, State.bDashThisRun ? 1 : 0,
				*GetNameSafe(Carrier), bCarrierAlive ? TEXT("ALIVE") : TEXT("DIED"),
				*GetNameSafe(Dasher), bDasherAlive ? TEXT("ALIVE") : TEXT("DIED"),
				KillsAwarded, ExpectedKills,
				bPass ? TEXT("PASS") : TEXT("*** FAIL ***"));

			++State.RunIndex;
			State.ScratchCount = 0;
			State.Phase = 0;
			State.Carrier = nullptr;
			State.Tripper = nullptr;
		}

		return true;
	}

	/** Starts a session. */
	void StartParryTest(int32 Runs)
	{
		FParryTestState State;
		State.TotalRuns = FMath::Clamp(Runs, 1, 100);

		UE_LOG(LogTraceGame, Display,
			TEXT("[PARRYTEST] starting %d runs, cycling A(parry+dash) / B(dash,no parry) / C(parry,no dash) "
			     "through a live carrier's trace. duration=%.2fs cooldown=%.2fs killsDasher=%d"),
			State.TotalRuns, TraceParry::GetDurationSeconds(), TraceParry::GetCooldownSeconds(),
			ParryKillsDasherEnabled() ? 1 : 0);

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float /*DeltaTime*/) mutable -> bool
			{
				return TickParryTest(State);
			}));
	}

	FAutoConsoleCommand CmdParry(
		TEXT("Trace.Parry"),
		TEXT("Trace.Parry - parry with the local pawn, through the real entry point."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ATraceCharacter* TraceChar = FindParryDebugLocalCharacter(FindParryDebugWorld());
			ETraceParryRefusal Refusal = ETraceParryRefusal::None;
			const bool bGranted = TraceParry::RequestParry(TraceChar, &Refusal);

			UE_LOG(LogTraceGame, Display, TEXT("[Parry] %s -> %s (%s). window=%.3fs cooldown=%.2fs"),
				*GetNameSafe(TraceChar), bGranted ? TEXT("GRANTED") : TEXT("REFUSED"), LexToString(Refusal),
				TraceParry::GetWindowRemainingFor(TraceChar), TraceParry::GetCooldownRemainingFor(TraceChar));
		}));

	/** One dump of every trace's invulnerability state and the colour actually on its materials. */
	void DumpParryState()
	{
		UWorld* World = FindParryDebugWorld();
		TArray<ATraceCharacter*> Characters;
		GatherParryDebugCharacters(World, Characters);

		UE_LOG(LogTraceGame, Display,
			TEXT("[DebugParry] duration=%.2fs cooldown=%.2fs glow=%.2f forceWindow=%d botAuto=%d | v6 3: "
			     "killsDasher=%d cause='%s' parryKillsAwarded=%d | v7 6: refundsEnabled=%d "
			     "parryCooldownResets=%d dashRefunds=%d dashRefundsOwedButUnwired=%d accessorWired=%d"),
			TraceParry::GetDurationSeconds(), TraceParry::GetCooldownSeconds(), TraceParry::GetGlowScale(),
			TraceParry::IsWindowForced() ? 1 : 0, TraceParry::IsBotAutoParryEnabled() ? 1 : 0,
			ParryKillsDasherEnabled() ? 1 : 0, *TraceParry::GetParryKillCause().ToString(), TraceParry::GetParryKillCount(),
			GParryKillRefunds, TraceParry::GetParryCooldownRefundCount(), TraceParry::GetDashRefundCount(),
			TraceParry::GetDashRefundUnwiredCount(), TraceParry::IsDashRefundWired() ? 1 : 0);

		// v8 §3. Meaningful on the SERVER: presses are resolved on the machine that owns the window,
		// so a client's copy of these counters is legitimately zero.
		UE_LOG(LogTraceGame, Display,
			TEXT("[DebugParry] v8 §3: stampedPresses=%d rewind mean %.1fms / worst %.1fms (maxRewind=%.0fms, "
			     "stamps pinned by a lagging client clock=%d) | heldTrips wired=%d opened=%d expired-to-kill=%d "
			     "ABANDONED=%d (must be 0) | LATE SAVES (press arrived after the trip)=%d"),
			TraceParry::GetPressCount(), TraceParry::GetMeanPressRewindMs(), TraceParry::GetMaxPressRewindMs(),
			TraceParry::GetMaxPressRewindSeconds() * 1000.f, TraceParry::GetPressClockPinnedCount(),
			TraceParry::AreHeldTripsWired() ? 1 : 0,
			TraceParry::GetHeldTripCount(), TraceParry::GetHeldTripExpiredCount(),
			TraceParry::GetAbandonedHeldTripCount(),
			TraceParry::GetLateParrySaveCount());

		for (const ATraceCharacter* TraceChar : Characters)
		{
			const UTraceTrailComponent* Trail = (TraceChar != nullptr) ? TraceChar->Trail : nullptr;
			if (Trail == nullptr || (Trail->TrailPoints.Items.Num() == 0 && !TraceChar->IsCarrier()))
			{
				continue;
			}

			// v6 §3 feedback record: what this player's HUD banner would be showing right now. It is
			// here because "the dasher must understand why they died" has a mirror obligation on the
			// other side, and a claim about on-screen feedback needs something objective behind it.
			float KillAge = 0.f;
			FString KillVictim;
			const FString ParryKillText = TraceParry::GetParryKillFeedback(TraceChar, KillAge, KillVictim)
				? FString::Printf(TEXT(" | PARRY KILL: %s, %.2fs ago"), *KillVictim, KillAge)
				: FString();

			// tint= is the colour actually pushed to the after-image material instances, so this line
			// is the objective answer to both "did it go red" and "did it come back".
			UE_LOG(LogTraceGame, Display,
				TEXT("[DebugParry]   %-24s carrier=%d points=%d lethal=%d | parry=%d visual=%d (%.3fs left, cd %.2fs) "
				     "passWindow=%d | traceInvulnerable=%d | tint=%s%s"),
				*GetNameSafe(TraceChar), TraceChar->IsCarrier() ? 1 : 0,
				Trail->TrailPoints.Items.Num(), Trail->ComputeLastLethalIndex() + 1,
				Trail->IsParryActive() ? 1 : 0, Trail->IsParryVisuallyActive() ? 1 : 0,
				Trail->GetParryWindowRemaining(), Trail->GetParryCooldownRemaining(),
				Trail->IsPassWindowInvulnerable() ? 1 : 0,
				Trail->IsTraceInvulnerable() ? 1 : 0,
				*Trail->GetAppliedTraceColor().ToString(), *ParryKillText);
		}
	}

	FAutoConsoleCommand CmdDebugParry(
		TEXT("Trace.DebugParry"),
		TEXT("Trace.DebugParry [IntervalSeconds] [Samples] - dump parry window, cooldown, pass-window state and "
		     "the tint actually applied to the trace materials, for every character that has a trace. "
		     "With arguments it repeats, which is how you watch the red go on AND come back off."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Interval = (Args.Num() > 0) ? FMath::Max(0.05f, FCString::Atof(*Args[0])) : 0.f;
			const int32 Samples = (Args.Num() > 1) ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 500) : 1;

			if (Interval <= 0.f || Samples <= 1)
			{
				DumpParryState();
				return;
			}

			int32 Remaining = Samples;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Remaining](float /*DeltaTime*/) mutable -> bool
				{
					DumpParryState();
					return --Remaining > 0;
				}), Interval);
		}));

	// ---------------------------------------------------------------------------------------------
	// SPEC v8 §3 — THE TWO COMMANDS THAT MAKE THE CLIENT'S NUMBER MEASURABLE
	//
	// The claim under test is "the window opens late for a joining player, and by how much". That
	// cannot be answered on the host — on the host the answer is 0 by construction, which is exactly
	// how this survived three demos. So it has to be measured ON A CLIENT, and a client cannot
	// measure it without (a) being the carrier and (b) a stopwatch that reads the same clock the
	// window is written in. One command each.
	//
	//   SERVER PROCESS:  Trace.Parry.GiveCoreToRemote [HoldSeconds]
	//   CLIENT PROCESS:  Trace.Parry.NetProbe [Samples] [IntervalSeconds]
	// ---------------------------------------------------------------------------------------------

	/** Server side of the rig: keep the Core in a REMOTE player's hands so they can legally parry. */
	FAutoConsoleCommand CmdParryGiveCoreToRemote(
		TEXT("Trace.Parry.GiveCoreToRemote"),
		TEXT("Trace.Parry.GiveCoreToRemote [HoldSeconds] [DelaySeconds] - SERVER ONLY. Grants the Core to "
		     "the first remote-controlled pawn and re-grants it if they lose it, so a joined client can be "
		     "measured as the carrier. DelaySeconds holds off the first grant, which is how the probe's "
		     "first press is kept away from the join transient. Verification rig only."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float HoldSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 1.f, 600.f) : 120.f;
			const float DelaySeconds = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.f, 300.f) : 0.f;

			// A COUNTED COUNTDOWN, NOT A WALL-CLOCK GATE, AND IT SAYS SO ON TICK ONE.
			//
			// MEASURED: the previous form accumulated the ticker's DeltaTime into an Elapsed double and
			// gated everything behind `Elapsed < DelaySeconds`. On the rig it produced NO output at all
			// across three runs — no grant, no heartbeat, no "needs the server" — while Trace.DebugParry,
			// registered from the same -ExecCmds string in the same frame, ticked perfectly beside it.
			// Whatever the cause (the delegate is dropped, or the delta it is handed is not what the
			// gate assumes), the failure mode is the one this whole pass is about: a harness that is
			// silent is read as a harness that worked, and two rig runs were already lost that way.
			//
			// So: the shape that is PROVEN to tick in this build — a countdown that returns
			// `--TicksLeft > 0`, exactly like Trace.DebugParry — plus a line at arm time and a line on
			// the first working tick. If it ever goes quiet again, the log says how far it got.
			constexpr float RigTickSeconds = 0.5f;
			int32 TicksLeft = FMath::CeilToInt((HoldSeconds + DelaySeconds) / RigTickSeconds) + 1;
			const int32 SkipTicks = FMath::CeilToInt(DelaySeconds / RigTickSeconds);
			int32 TickIndex = 0;
			int32 Grants = 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYRIG] GiveCoreToRemote armed: %d ticks of %.2fs, first grant after %d ticks (%.0fs delay, %.0fs hold)."),
				TicksLeft, RigTickSeconds, SkipTicks, DelaySeconds, HoldSeconds);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([TicksLeft, SkipTicks, TickIndex, Grants](float /*DeltaTime*/) mutable -> bool
			{
				++TickIndex;
				if (TicksLeft-- <= 0)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[PARRYRIG] GiveCoreToRemote finished after %d ticks, %d grants."), TickIndex, Grants);
					return false;
				}
				if (TickIndex <= SkipTicks)
				{
					if (TickIndex == 1)
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[PARRYRIG] ticking; holding off the first grant for %d more ticks."), SkipTicks);
					}
					return true;
				}

				UWorld* World = FindParryDebugWorld();
				if (World == nullptr || World->GetNetMode() == NM_Client)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[PARRYRIG] GiveCoreToRemote needs the server."));
					return false;
				}

				ATraceCore* TheCore = ATraceCore::Get(World);

				// A remote pawn is one with an owning net connection: on a listen server that is
				// every joined client and nobody else, which is the whole selection rule.
				//
				// REMOTE MEANS "NOT THE LISTEN SERVER'S OWN PLAYER", and GetNetConnection() alone does
				// NOT say that: on a listen server the host's own pawn is owned by a PlayerController
				// whose GetNetConnection() walks up to the world's net driver and answers non-null just
				// like a joined client's does. Selecting on it therefore matched the HOST first, found
				// it was already the carrier or granted the Core straight back to the machine that has
				// no complaint, and the command sat there silently doing nothing useful — which is
				// exactly what it did on two rig runs before this was measured. The reliable test is
				// the controller's REMOTE ROLE: a joined client's PlayerController is ROLE_AutonomousProxy
				// on its own machine and, on the server, has a non-null UNetConnection of its own that
				// is NOT the server's. IsLocalController() is the one-line form of that and it is what
				// the rest of this debug block already uses.
				TArray<ATraceCharacter*> Characters;
				GatherParryDebugCharacters(World, Characters);

				int32 Alive = 0;
				int32 Remote = 0;
				for (ATraceCharacter* TraceChar : Characters)
				{
					if (TraceChar == nullptr || !TraceChar->IsAlive())
					{
						continue;
					}
					++Alive;

					const AController* Ctrl = TraceChar->GetController();
					const APlayerController* AsPC = Cast<const APlayerController>(Ctrl);
					if (AsPC == nullptr || AsPC->IsLocalController())
					{
						continue;   // a bot, or the listen server's own player
					}
					++Remote;

					if (TheCore == nullptr || TheCore->GetCarrier() == TraceChar)
					{
						continue;   // no Core yet, or already theirs
					}

					TheCore->GrantTo(TraceChar, ETraceCoreGrantReason::Debug);
					++Grants;
					UE_LOG(LogTraceGame, Display,
						TEXT("[PARRYRIG] Core granted to REMOTE pawn %s (grant #%d, ping %.0fms)."),
						*GetNameSafe(TraceChar), Grants,
						(TraceChar->GetPlayerState() != nullptr) ? TraceChar->GetPlayerState()->GetPingInMilliseconds() : -1.f);
					return true;
				}

				// SILENCE IS THE FAILURE MODE THIS COSTS ONE LINE TO CLOSE. Two rig runs produced no
				// PARRYRIG output at all and were read as "the grant happened"; they had in fact found
				// no remote pawn. Every 8 ticks (~4s), say what was actually seen — counted off the same
				// tick index the countdown uses, so there is no second notion of time to get wrong.
				if ((TickIndex % 8) == 0)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[PARRYRIG] waiting: core=%s, %d characters, %d alive, %d remote-controlled, %d grants so far."),
						(TheCore != nullptr) ? TEXT("yes") : TEXT("NO - match not live"),
						Characters.Num(), Alive, Remote, Grants);
				}
				return true;
			}), RigTickSeconds);
		}));

	/**
	 * Trace.Parry.NetProbe — THE NUMBER THIS ITEM IS ABOUT, measured from the machine that has it.
	 *
	 * Per sample: press through the real entry point, stamp the press on the shared clock, then wait
	 * for the AUTHORITATIVE window (the replicated one, the only thing the trip test respects) and
	 * work out where the server put it.
	 *
	 *   lostMs   = serverWindowOpen - press, where serverWindowOpen = (end - duration). This is the
	 *              part of the parry the network ate before it existed: 0 on the host by definition,
	 *              one upstream lag on a client. It is derived rather than reported by the server on
	 *              purpose — the client can compute it alone, so the measurement needs no cooperation
	 *              from the machine under test.
	 *   seenMs   = wall-clock press -> this machine can SEE its own authoritative window. That is a
	 *              full round trip and it is why the red tint is predicted locally; it is reported so
	 *              the two costs are never confused with each other.
	 */
	struct FParryNetProbeState
	{
		int32 TotalSamples = 5;
		int32 SampleIndex = 0;

		/**
		 * Samples space themselves out with no interval argument: the parry's own 1.5s cooldown is
		 * the gate in phase 0, and a probe that pressed faster than the mechanic allows would be
		 * measuring refusals.
		 */

		/** 0 = waiting for a legal press, 1 = pressed, waiting for the authoritative window. */
		int32 Phase = 0;
		double PhaseSeconds = 0.0;
		double IdleSeconds = 0.0;

		float PressStamp = 0.f;
		double PressWallTime = 0.0;
		bool bPredictedTint = false;

		int32 Measured = 0;
		int32 TimedOut = 0;
		int32 Refused = 0;
		double LostMsTotal = 0.0;
		float LostMsMax = 0.f;
		double SeenMsTotal = 0.0;

		/**
		 * THE OFFSET-FREE NUMBER, and the one to trust if the two disagree.
		 *
		 * LostMs above is derived from the shared clock, so a client whose GetServerWorldTimeSeconds()
		 * is not tracking the server reports its clock error rather than its network cost — which is
		 * exactly what the first rig run measured. This one is pure local wall clock: from the press
		 * to the moment the AUTHORITATIVE window is visible here, and then to the moment it closes.
		 * Neither end touches the shared clock, so neither can be poisoned by it.
		 *
		 * UsableMs is the whole complaint in one number: how much of their own 200ms parry a joined
		 * player can still see running once they know it exists. The host's is 200 by construction.
		 */
		double UsableMsTotal = 0.0;
		float UsableMsMin = 1e9f;
		int32 UsableSamples = 0;

		/** Set while a measured window is still open, so its close can be timed. */
		bool bTimingClose = false;
		double WindowSeenWallTime = 0.0;

		/**
		 * Next IdleSeconds at which the probe says why it has not pressed yet.
		 *
		 * Not decoration. A probe that prints "starting 8 samples" and then nothing looks identical to
		 * a probe that is working, and two rig runs were read that way before this was added. The
		 * carrier gate is the one that fails in practice (the Core is with a bot), and it is invisible
		 * from the client's log without this line.
		 */
		double NextReportSeconds = 5.0;
	};

	bool TickParryNetProbe(FParryNetProbeState& State, float DeltaTime)
	{
		UWorld* World = FindParryDebugWorld();
		ATraceCharacter* TraceChar = FindParryDebugLocalCharacter(World);

		if (State.SampleIndex >= State.TotalSamples)
		{
			const int32 Denominator = FMath::Max(1, State.Measured);
			const int32 UsableDenominator = FMath::Max(1, State.UsableSamples);
			const float WindowMs = TraceParry::GetDurationSeconds() * 1000.f;

			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYPROBE] DONE on %s. %d measured, %d refused, %d timed out (a timeout means the "
				     "authoritative window never became visible here AT ALL). heldTrips=%d"),
				(World != nullptr && World->GetNetMode() == NM_Client) ? TEXT("a CLIENT") : TEXT("the SERVER"),
				State.Measured, State.Refused, State.TimedOut, TraceParry::AreHeldTripsWired() ? 1 : 0);

			// WALL CLOCK, and therefore the numbers to quote. Neither end reads the shared clock.
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYPROBE] WALL CLOCK: press -> the window is visible here: mean %.1fms | "
				     "USABLE WINDOW LEFT once it is visible: mean %.1fms, worst %.1fms, of %.0fms (%.0f%%). "
				     "A host's usable window is the full %.0fms by construction."),
				State.SeenMsTotal / Denominator,
				State.UsableMsTotal / UsableDenominator,
				(State.UsableSamples > 0) ? State.UsableMsMin : 0.f,
				WindowMs,
				100.0 * (State.UsableMsTotal / UsableDenominator) / FMath::Max(1.f, WindowMs),
				WindowMs);

			// SHARED CLOCK. Kept, but second, and labelled: it is only the network cost when the
			// client's GetServerWorldTimeSeconds() is actually tracking the server's.
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYPROBE] SHARED CLOCK: press -> server opened the window: mean %.1fms, worst %.1fms. "
				     "Anything far above one one-way lag here is CLOCK SKEW on this client, not parry latency."),
				State.LostMsTotal / Denominator, State.LostMsMax);
			return false;
		}

		// ---- phase 2: time the CLOSE of a window we already timed the open of ---------------------
		//
		// Wall clock at both ends, so the client's shared-clock error cannot touch this number.
		if (State.bTimingClose)
		{
			if (!TraceParry::IsParryActiveFor(TraceChar)
				|| (FPlatformTime::Seconds() - State.WindowSeenWallTime) > 1.0)
			{
				const float UsableMs = static_cast<float>(
					(FPlatformTime::Seconds() - State.WindowSeenWallTime) * 1000.0);
				State.bTimingClose = false;
				++State.UsableSamples;
				State.UsableMsTotal += UsableMs;
				State.UsableMsMin = FMath::Min(State.UsableMsMin, UsableMs);

				UE_LOG(LogTraceGame, Display,
					TEXT("[PARRYPROBE] ... and it was still open here for %.1fms of its %.0fms "
					     "(wall clock, immune to the shared-clock offset)."),
					UsableMs, TraceParry::GetDurationSeconds() * 1000.f);
			}
			return true;
		}

		// ---- phase 0: wait until this machine's own pawn may legally parry ------------------------
		if (State.Phase == 0)
		{
			State.IdleSeconds += DeltaTime;
			if (State.IdleSeconds > 180.0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[PARRYPROBE] gave up: the local pawn never became a carrier with the parry ready. "
					     "Run Trace.Parry.GiveCoreToRemote on the SERVER process."));
				return false;
			}

			if (TraceChar == nullptr || !TraceChar->IsAlive() || !TraceChar->IsCarrier()
				|| TraceParry::GetCooldownRemainingFor(TraceChar) > 0.f
				|| TraceParry::IsParryActiveFor(TraceChar))
			{
				if (State.IdleSeconds >= State.NextReportSeconds)
				{
					State.NextReportSeconds = State.IdleSeconds + 5.0;
					UE_LOG(LogTraceGame, Display,
						TEXT("[PARRYPROBE] waiting to press after %.0fs: pawn=%s alive=%d carrier=%d "
						     "cooldownLeft=%.2fs windowOpen=%d. (The carrier gate needs "
						     "Trace.Parry.GiveCoreToRemote running on the SERVER.)"),
						State.IdleSeconds, *GetNameSafe(TraceChar),
						(TraceChar != nullptr && TraceChar->IsAlive()) ? 1 : 0,
						(TraceChar != nullptr && TraceChar->IsCarrier()) ? 1 : 0,
						TraceParry::GetCooldownRemainingFor(TraceChar),
						TraceParry::IsParryActiveFor(TraceChar) ? 1 : 0);
				}
				return true;
			}

			// THE PRESS. Through TraceParry::RequestParry, the same call the Q bind makes - a probe
			// that reached past the entry point would be measuring a path nobody plays.
			State.PressStamp = TraceParry::GetPressStampSeconds(TraceChar);
			State.PressWallTime = FPlatformTime::Seconds();

			ETraceParryRefusal Refusal = ETraceParryRefusal::None;
			const bool bGranted = TraceParry::RequestParry(TraceChar, &Refusal);
			State.bPredictedTint = (TraceChar->Trail != nullptr) && TraceChar->Trail->IsParryVisuallyActive();

			// THE ONE LINE THAT MAKES THE ITEM'S NUMBER CROSS-PROCESS. Every other measurement here is
			// either local wall clock (blind to what the server did with the press) or the shared clock
			// (poisoned by the client's own offset — see the header). Both UE processes on this rig run
			// on the same machine, so the OS timestamp UE_LOG stamps on this line and the one it stamps
			// on the server's [PARRYNET] arrival line are the SAME clock to the millisecond. Pair them
			// and press -> window-open falls out with no clock either side can distort:
			//
			//     delay = (arrival_wall - rewindMs) - press_wall      [v8, anchored]
			//     delay =  arrival_wall             - press_wall      [pre-v8, anchored at arrival]
			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYPRESS %d/%d] pressed now (granted=%d, predictedTint=%d, sharedClockStamp=%.3f). "
				     "Pair this line's timestamp with the server's next [PARRYNET] line."),
				State.SampleIndex + 1, State.TotalSamples, bGranted ? 1 : 0,
				State.bPredictedTint ? 1 : 0, State.PressStamp);

			if (!bGranted)
			{
				++State.Refused;
				++State.SampleIndex;
				UE_LOG(LogTraceGame, Warning, TEXT("[PARRYPROBE %d/%d] refused: %s"),
					State.SampleIndex, State.TotalSamples, LexToString(Refusal));
				return true;
			}

			State.Phase = 1;
			State.PhaseSeconds = 0.0;
			return true;
		}

		// ---- phase 1: wait for the AUTHORITATIVE window to reach this machine ---------------------
		State.PhaseSeconds += DeltaTime;

		const bool bAuthoritative = TraceParry::IsParryActiveFor(TraceChar);
		if (!bAuthoritative)
		{
			if (State.PhaseSeconds > 1.5)
			{
				++State.TimedOut;
				++State.SampleIndex;
				State.Phase = 0;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[PARRYPROBE %d/%d] the authoritative window never arrived within 1.5s "
					     "(server refused the press, or the window had already lapsed when it landed)."),
					State.SampleIndex, State.TotalSamples);
			}
			return true;
		}

		// The window END is what replicates; the server's OPEN instant is end - duration, in the same
		// shared clock the press was stamped in. No server cooperation, no second clock.
		const float Now = TraceParry::GetPressStampSeconds(TraceChar);
		const float WindowEnd = Now + TraceParry::GetWindowRemainingFor(TraceChar);
		const float ServerOpen = WindowEnd - TraceParry::GetDurationSeconds();
		const float LostMs = (ServerOpen - State.PressStamp) * 1000.f;
		const float SeenMs = static_cast<float>((FPlatformTime::Seconds() - State.PressWallTime) * 1000.0);

		++State.Measured;
		State.LostMsTotal += LostMs;
		State.LostMsMax = FMath::Max(State.LostMsMax, LostMs);
		State.SeenMsTotal += SeenMs;
		++State.SampleIndex;
		State.Phase = 0;
		State.PhaseSeconds = 0.0;
		State.bTimingClose = true;
		State.WindowSeenWallTime = FPlatformTime::Seconds();

		const APlayerState* PawnState = (TraceChar != nullptr) ? TraceChar->GetPlayerState() : nullptr;
		UE_LOG(LogTraceGame, Display,
			TEXT("[PARRYPROBE %d/%d] press t=%.3f -> server opened the window at t=%.3f: %.1fms of a %.0fms "
			     "parry gone before it existed. predictedTintInstantly=%d, visible here after %.1fms, ping=%.0fms."),
			State.SampleIndex, State.TotalSamples, State.PressStamp, ServerOpen, LostMs,
			TraceParry::GetDurationSeconds() * 1000.f, State.bPredictedTint ? 1 : 0, SeenMs,
			(PawnState != nullptr) ? PawnState->GetPingInMilliseconds() : -1.f);

		return true;
	}

	FAutoConsoleCommand CmdParryNetProbe(
		TEXT("Trace.Parry.NetProbe"),
		TEXT("Trace.Parry.NetProbe [Samples] - SPEC v8 §3. Measures, on THIS machine, how "
		     "much of the parry window is gone before the server opens it. Meaningful on a CLIENT; on the "
		     "host it reads ~0 by definition, which is the point. Needs the local pawn to be the carrier."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FParryNetProbeState State;
			State.TotalSamples = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 50) : 5;

			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYPROBE] starting %d samples. duration=%.0fms cooldown=%.2fs maxRewind=%.0fms heldTrips=%d"),
				State.TotalSamples, TraceParry::GetDurationSeconds() * 1000.f, TraceParry::GetCooldownSeconds(),
				TraceParry::GetMaxPressRewindSeconds() * 1000.f, TraceParry::AreHeldTripsWired() ? 1 : 0);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickParryNetProbe(State, DeltaTime);
				}));
		}));

	FAutoConsoleCommand CmdTestParry(
		TEXT("Trace.TestParry"),
		TEXT("Trace.TestParry [Runs] - cycles three cases through a live carrier's trace: A parry+dash "
		     "(dasher dies, v6 3), B dash with no parry (carrier dies), C parry with nothing dashing "
		     "(nobody dies). Runs defaults to 6 = two full cycles."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const int32 Runs = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 6;
			StartParryTest(Runs);
		}));
}

#endif   // !UE_BUILD_SHIPPING
