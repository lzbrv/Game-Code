#include "Gameplay/TraceParry.h"

#include <type_traits>                     // v7 §6: the two cross-slice accessor detectors

#include "Containers/Ticker.h"
#include "Engine/Engine.h"                 // GEngine->GetWorldContexts()
#include "Engine/World.h"
#include "EngineUtils.h"                   // TActorIterator (character gather fallback)
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"           // FAutoConsoleVariableRef, FAutoConsoleCommand

#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"

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

	bool ServerPunishParriedDash(ATraceCharacter* Carrier, ATraceCharacter* Dasher)
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
		if (!IsParryActiveFor(Carrier))
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
			// the previous run's still-open 0.1s window and the harness never actually tests the
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
			// 0.1s window is ~6 frames, so it is still open when the trip test runs next frame —
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
		// lapsed and the fifth frame killed the carrier — a correct outcome that the scoring below
		// would have read as the parry having failed.
		//
		// The parry buys 0.1 seconds, not immunity. Isolating the single sweep is what lets this
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
