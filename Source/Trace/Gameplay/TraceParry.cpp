#include "Gameplay/TraceParry.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"                 // GEngine->GetWorldContexts()
#include "Engine/World.h"
#include "EngineUtils.h"                   // TActorIterator (character gather fallback)
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"           // FAutoConsoleVariableRef, FAutoConsoleCommand

#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Gameplay/TraceCore.h"
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
	// outcome proves nothing: the harness has to show BOTH branches, from the same code path, with
	// nothing else changed between them. So it alternates — odd runs parry, even runs do not — and
	// reports the carrier's fate for each.
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

		bool bParryThisRun = false;
		bool bParryWasActiveAtSweep = false;

		/** Attempts scratched for this run index (dash refused, no tripper). Bounded, then aborted. */
		int32 ScratchCount = 0;

		int32 ProtectedCount = 0;
		int32 KilledCount = 0;
		int32 AbortedCount = 0;
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
				TEXT("[PARRYTEST] DONE. %d runs: %d protected by a parry, %d killed with no parry, %d aborted."),
				State.TotalRuns, State.ProtectedCount, State.KilledCount, State.AbortedCount);
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
			State.bParryThisRun = ((State.RunIndex % 2) == 0);
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

			const FVector DashStart = Midpoint + Across * 260.0;
			State.DashEnd = Midpoint - Across * 240.0;
			State.Tripper = Tripper;
			State.TripperHome = Tripper->GetActorLocation();

			Tripper->SetActorLocation(DashStart, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
			Tripper->SetActorRotation((-Across).Rotation());
			if (UTraceCharacterMovementComponent* TripperMovement = Tripper->GetTraceMovement())
			{
				TripperMovement->StartDash();
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST %d/%d] carrier=%s parryRequested=%d granted=%d (%s) window=%.3fs cooldown=%.2fs | %s will dash across segment %d/%d at %s"),
				State.RunIndex + 1, State.TotalRuns, *GetNameSafe(Carrier),
				State.bParryThisRun ? 1 : 0, bParryGranted ? 1 : 0, LexToString(Refusal),
				TraceParry::GetWindowRemainingFor(Carrier), TraceParry::GetCooldownRemainingFor(Carrier),
				*GetNameSafe(Tripper), SegmentIndex, LastLethal, *Midpoint.ToCompactString());

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
			if (ATraceCharacter* Tripper = State.Tripper.Get())
			{
				Tripper->SetActorLocation(State.TripperHome, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
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
			// Carrier may legitimately be NULL here: an unparried dash kills them and the pawn is
			// gone by the time we score. That is the expected negative result, not a crash.
			const bool bCarrierAlive = (Carrier != nullptr) && Carrier->IsAlive();
			const bool bExpectedProtection = State.bParryWasActiveAtSweep;
			const bool bPass = (bCarrierAlive == bExpectedProtection);

			if (bCarrierAlive)
			{
				++State.ProtectedCount;
			}
			else
			{
				++State.KilledCount;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[PARRYTEST %d/%d] RESULT: parryActive=%d -> carrier %s. %s"),
				State.RunIndex + 1, State.TotalRuns, State.bParryWasActiveAtSweep ? 1 : 0,
				bCarrierAlive ? TEXT("SURVIVED (trace not broken)") : TEXT("DIED (trace broken)"),
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
			TEXT("[PARRYTEST] starting %d runs, alternating parried / unparried dashes through a live carrier's trace. "
			     "duration=%.2fs cooldown=%.2fs"),
			State.TotalRuns, TraceParry::GetDurationSeconds(), TraceParry::GetCooldownSeconds());

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
			TEXT("[DebugParry] duration=%.2fs cooldown=%.2fs glow=%.2f forceWindow=%d botAuto=%d"),
			TraceParry::GetDurationSeconds(), TraceParry::GetCooldownSeconds(), TraceParry::GetGlowScale(),
			TraceParry::IsWindowForced() ? 1 : 0, TraceParry::IsBotAutoParryEnabled() ? 1 : 0);

		for (const ATraceCharacter* TraceChar : Characters)
		{
			const UTraceTrailComponent* Trail = (TraceChar != nullptr) ? TraceChar->Trail : nullptr;
			if (Trail == nullptr || (Trail->TrailPoints.Items.Num() == 0 && !TraceChar->IsCarrier()))
			{
				continue;
			}

			// tint= is the colour actually pushed to the after-image material instances, so this line
			// is the objective answer to both "did it go red" and "did it come back".
			UE_LOG(LogTraceGame, Display,
				TEXT("[DebugParry]   %-24s carrier=%d points=%d lethal=%d | parry=%d visual=%d (%.3fs left, cd %.2fs) "
				     "passWindow=%d | traceInvulnerable=%d | tint=%s"),
				*GetNameSafe(TraceChar), TraceChar->IsCarrier() ? 1 : 0,
				Trail->TrailPoints.Items.Num(), Trail->ComputeLastLethalIndex() + 1,
				Trail->IsParryActive() ? 1 : 0, Trail->IsParryVisuallyActive() ? 1 : 0,
				Trail->GetParryWindowRemaining(), Trail->GetParryCooldownRemaining(),
				Trail->IsPassWindowInvulnerable() ? 1 : 0,
				Trail->IsTraceInvulnerable() ? 1 : 0,
				*Trail->GetAppliedTraceColor().ToString());
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
		TEXT("Trace.TestParry [Runs] - alternating parried/unparried dashes through a live carrier's trace."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const int32 Runs = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 6;
			StartParryTest(Runs);
		}));
}

#endif   // !UE_BUILD_SHIPPING
