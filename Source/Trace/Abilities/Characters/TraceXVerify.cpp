// Trace — X's self-tests (spec v14 §6, guarded by spec v14 §4).
//
// ===================================================================================================
// WHAT EACH COMMAND FALSIFIES, AND WHY IT IS ARRANGED THE WAY IT IS
// ===================================================================================================
//
//   Trace.X.Dump             what the running process actually resolved. The .ini wins over the
//                            header, so this is the only honest way to read X's numbers.
//
//   Trace.X.VulnerableTest   THE AMPLIFIER. Two arms on one binary: arm 0 marks the target and turns
//                            the multiplier off (Trace.X.Vulnerable 0) and must measure 40; arm 1 is
//                            the shipped build and must measure 50. Same target, same call, same
//                            frame budget. Also: non-stacking, the timer reset, and expiry.
//
//   Trace.X.CarrierTest      *** SPEC §4. *** Three independent locks stop X's mark from ever
//                            reaching a Core carrier, and this red-arms all three at once
//                            (Trace.Ability.CarrierImmune 0, Trace.X.VulnerableCarrierImmune 0,
//                            Trace.X.VulnerableApplyOrder 0). Arm 0 must REPRODUCE — the carrier gets
//                            marked and the amplifier runs on their damage — or arm 1's clean run is
//                            uninformative and the verdict says INVALID rather than PASS.
//
//   Trace.X.StingTest        the activated ability: five bullets, five bees, the swarm resumes, the
//                            sixth bullet is free, and a bullet into a carrier does not spend one.
//
//   Trace.X.SpeedTest        "+10% while ANY ENEMY is vulnerable" — including the half that is easy
//                            to get wrong: a marked TEAMMATE must not switch it on.
//
//   Trace.X.BeeTest          the passive's contact test, with its own red arm: an enemy placed INSIDE
//                            the swarm must be marked, and an enemy placed at three times the orbit
//                            radius must NOT be. Without the second half, a sweep that marked
//                            everybody in the level would report green.
//
// ===================================================================================================
// WHY EVERY DELTA IS READ IN THE SAME STATEMENT SEQUENCE AS THE DAMAGE
// ===================================================================================================
//
// A headless run fills both teams with bots, and a bot shooting the target between "read health" and
// "read health again" would be indistinguishable from a broken multiplier. UTraceHealthComponent::
// ApplyDamage is synchronous, so every measurement below reads the health, applies the damage and
// reads it again with nothing in between — no tick, no await. That makes the numbers measurements
// rather than anecdotes, and it is the same reasoning the regen fixture's header gives.
//
// Times are FPlatformTime::Seconds() (real time) for pacing, because the character-select screen has
// paused the world in this project before; the two places that need the MATCH clock (the mark's
// remaining time, expiry) read it explicitly and say so.

#include "CoreMinimal.h"

#include "Abilities/Characters/TraceAbilitySetX.h"
#include "Abilities/Characters/TraceAbilityWeaponHooks.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceCharacterAbilitySet.h"

#include "Containers/Ticker.h"
#include "Core/TraceCharacter.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"

#if !UE_BUILD_SHIPPING

namespace TraceXVerify
{
	/** The damage every measurement below asks for. Unambiguous, well short of lethal, and 40 x 1.25
	 *  is 50 — two numbers nobody can confuse with each other or with a regen tick. */
	constexpr float XTestDamage = 40.f;

	const FName XTestCause(TEXT("XVerify"));

	UWorld* FindAuthoritativeWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetAuthGameMode() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	IConsoleVariable* FindCVar(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	void SetCVar(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = FindCVar(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** Puts every arm back to its shipped value. Called on every exit path, including the aborts. */
	void RestoreAllArms()
	{
		SetCVar(TEXT("Trace.X.Vulnerable"), 1);
		SetCVar(TEXT("Trace.X.VulnerableApplyOrder"), 1);
		SetCVar(TEXT("Trace.X.VulnerableCarrierImmune"), 1);
		SetCVar(TEXT("Trace.Ability.CarrierImmune"), 1);
	}

	// =============================================================================================
	// A tiny checklist, so every command reports the same way and a FAIL is impossible to skim past
	// =============================================================================================

	struct FXChecklist
	{
		const TCHAR* Tag = TEXT("X");
		int32 Passed = 0;
		int32 Failed = 0;
		bool  bInvalid = false;
		FString InvalidReason;

		void Check(bool bCondition, const FString& Name, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[%s] %s  %s  |  %s"),
				Tag, bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *Name, *Detail);
		}

		void Invalidate(const FString& Reason)
		{
			bInvalid = true;
			InvalidReason = Reason;
		}

		void Report()
		{
			if (bInvalid)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: INVALID — %s (%d passed, %d failed)"),
					Tag, *InvalidReason, Passed, Failed);
			}
			else if (Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[%s] VERDICT: PASS — %d checks, 0 failed."), Tag, Passed);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d passed, %d FAILED."),
					Tag, Passed, Failed);
			}
		}
	};

	// =============================================================================================
	// Staging: who is X, who is the target
	// =============================================================================================

	/**
	 * Makes the first HUMAN player X and returns their set.
	 *
	 * A human, because spec §3 says bots stay characterless and ServerSetCharacter refuses them —
	 * so in a headless run with bots the host's own pawn is the only candidate, and a harness that
	 * silently settled for a bot would be testing nothing.
	 */
	UTraceAbilitySetX* MakePlayerIntoX(UWorld* WorldPtr, FString& OutWhy)
	{
		if (WorldPtr == nullptr)
		{
			OutWhy = TEXT("no world");
			return nullptr;
		}

		if (!UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
		{
			OutWhy = TEXT("characters are DISABLED in this match (mode A, or the §3 toggle is off) — "
			              "run this in mode B with characters on");
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = Cast<APlayerController>(It->Get());
			if (PC == nullptr || PC->GetPawn() == nullptr)
			{
				continue;
			}
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PC->GetPawn());
			if (Comp == nullptr || Comp->IsBot())
			{
				continue;
			}

			if (Comp->GetCharacterId() != ETraceCharacterId::X)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::X);
			}

			if (UTraceAbilitySetX* XSet = Comp->GetAbilitySetAs<UTraceAbilitySetX>())
			{
				return XSet;
			}

			OutWhy = FString::Printf(TEXT("ServerSetCharacter(X) did not produce a UTraceAbilitySetX (id is now %s) — "
			                              "the reflection roster may not have found the class"),
				TraceCharacterIdToString(Comp->GetCharacterId()));
			return nullptr;
		}

		OutWhy = TEXT("no human player controller with a pawn");
		return nullptr;
	}

	/** A living enemy of @p XSet who is NOT the carrier, healed to full so damage has somewhere to go. */
	ATraceCharacter* FindEnemyTarget(UWorld* WorldPtr, const UTraceAbilitySetX* XSet)
	{
		if (WorldPtr == nullptr || XSet == nullptr)
		{
			return nullptr;
		}
		const ATraceCharacter* XPawn = XSet->GetCharacter();
		ETraceTeam XTeam = ETraceTeam::None;
		if (const UTraceAbilityComponent* Comp = XSet->GetAbilityComponent())
		{
			XTeam = Comp->GetTeam();
		}

		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == XPawn || !Candidate->IsAlive() || Candidate->Health == nullptr)
			{
				continue;
			}
			if (UTraceAbilityComponent::IsCarrier(Candidate))
			{
				continue;
			}
			if (Candidate->GetTeam() == XTeam || Candidate->GetTeam() == ETraceTeam::None)
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	/** A living TEAMMATE of X. Used by the speed test's "any ENEMY, not any player" half. */
	ATraceCharacter* FindTeammate(UWorld* WorldPtr, const UTraceAbilitySetX* XSet)
	{
		if (WorldPtr == nullptr || XSet == nullptr)
		{
			return nullptr;
		}
		const ATraceCharacter* XPawn = XSet->GetCharacter();
		ETraceTeam XTeam = ETraceTeam::None;
		if (const UTraceAbilityComponent* Comp = XSet->GetAbilityComponent())
		{
			XTeam = Comp->GetTeam();
		}
		if (XTeam == ETraceTeam::None)
		{
			return nullptr;
		}

		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == XPawn || !Candidate->IsAlive() || Candidate->Health == nullptr)
			{
				continue;
			}
			if (UTraceAbilityComponent::IsCarrier(Candidate))
			{
				continue;
			}
			if (Candidate->GetTeam() == XTeam)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/**
	 * Health before, one damage call, health after — in one statement sequence with no tick between,
	 * so a bot's bullet cannot land inside the measurement. See the file header.
	 */
	float MeasureDamage(UTraceHealthComponent* TargetHealth, float Amount)
	{
		if (TargetHealth == nullptr)
		{
			return -1.f;
		}
		const float Before = TargetHealth->Health;
		TargetHealth->ApplyDamage(Amount, nullptr, XTestCause);
		return Before - TargetHealth->Health;
	}

	// =============================================================================================
	// Trace.X.Dump
	// =============================================================================================

	void RunDump()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		const UTraceSettings& Settings = UTraceSettings::Get();

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE X (spec v14 §6) =========="));
		UE_LOG(LogTraceGame, Display, TEXT("PASSIVE  bees        : %d, orbit %.0fuu at %.0f deg/s, hit radius %.0fuu"),
			TraceXBees::GetBeeCount(), TraceXBees::GetOrbitRadiusUU(),
			TraceXBees::GetOrbitSpeedDegreesPerSecond(), TraceXBees::GetBeeHitRadiusUU());
		UE_LOG(LogTraceGame, Display, TEXT("PASSIVE  vulnerable  : %.2fs, x%.3f damage from ALL sources (§6 says 2s, +25%%)"),
			TraceVulnerable::GetDurationSeconds(), TraceVulnerable::GetDamageMultiplier());
		UE_LOG(LogTraceGame, Display, TEXT("MOVEMENT speed bonus : +%.1f%% while ANY enemy is vulnerable (§6 says +10%%)"),
			TraceXBees::GetSpeedBonusFraction() * 100.f);
		UE_LOG(LogTraceGame, Display, TEXT("STING    bullets     : %d, cooldown %.1fs (§6 says 5 and 25 — the only 25)"),
			TraceXBees::GetStingBulletCount(), FMath::Clamp(Settings.XStingCooldownSeconds, 0.f, 180.f));
		UE_LOG(LogTraceGame, Display, TEXT("ARMS     shipped=1   : Trace.X.Vulnerable=%d ApplyOrder=%d CarrierImmune=%d"),
			TraceVulnerable::IsEnabled() ? 1 : 0, TraceVulnerable::IsApplyOrderShipped() ? 1 : 0,
			TraceVulnerable::IsCarrierImmune() ? 1 : 0);
		UE_LOG(LogTraceGame, Display, TEXT("ALARMS   must be 0   : carrierMarked=%d carrierAmplified=%d | liveness marks=%d amplified=%d"),
			TraceVulnerable::GetCarrierMarkedCount(), TraceVulnerable::GetCarrierAmplifiedCount(),
			TraceVulnerable::GetMarkAppliedCount(), TraceVulnerable::GetAmplifiedHitCount());

		if (WorldPtr != nullptr)
		{
			UE_LOG(LogTraceGame, Display, TEXT("WORLD    charactersEnabled=%d paused=%d netmode=%d"),
				UTraceAbilityComponent::AreCharactersEnabled(WorldPtr) ? 1 : 0,
				WorldPtr->IsPaused() ? 1 : 0, static_cast<int32>(WorldPtr->GetNetMode()));

			int32 MarkedNow = 0;
			for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
			{
				if (*It != nullptr && It->Health != nullptr && It->Health->IsVulnerable())
				{
					++MarkedNow;
					UE_LOG(LogTraceGame, Display, TEXT("LIVE     marked      : %s, %.2fs left, x%.3f"),
						*GetNameSafe(*It), It->Health->GetVulnerableRemaining(),
						It->Health->GetVulnerableDamageMultiplier());
				}
			}
			if (MarkedNow == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("LIVE     marked      : nobody"));
			}

			for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
			{
				if (const UTraceAbilitySetX* XSet = Cast<UTraceAbilitySetX>(UTraceAbilityComponent::GetAbilitySetFor(*It)))
				{
					UE_LOG(LogTraceGame, Display, TEXT("LIVE     X is        : %s | loadedBees=%d anyEnemyVulnerable=%d speedX=%.3f"),
						*GetNameSafe(*It), XSet->GetLoadedBees(), XSet->IsAnyEnemyVulnerable() ? 1 : 0,
						XSet->GetMoveSpeedMultiplier());
				}
			}
		}
		UE_LOG(LogTraceGame, Display, TEXT("=========================================="));
	}

	// =============================================================================================
	// Trace.X.VulnerableTest — the amplifier, red arm first
	// =============================================================================================

	struct FVulnTestState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		TWeakObjectPtr<ATraceCharacter> Target;
		FXChecklist List;
		float MarkedRemainingAtStamp = 0.f;
	};

	void RunVulnerableTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XVULN] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FVulnTestState> State = MakeShared<FVulnTestState>();
		State->List.Tag = TEXT("XVULN");
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[XVULN] ===== spec v14 §6: 'VULNERABLE for 2s, taking +25%% damage FROM ALL SOURCES. Does not "
			     "stack; a new application RESETS the timer.' arm 0 = RED (Trace.X.Vulnerable 0, the mark still "
			     "lands but multiplies nothing) must measure %.0f; arm 1 = shipped must measure %.0f. ====="),
			XTestDamage, XTestDamage * TraceVulnerable::GetDamageMultiplier());

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreAllArms();
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			FString Why;
			UTraceAbilitySetX* XSet = MakePlayerIntoX(TickWorld, Why);

			// ---- step 0: stage -------------------------------------------------------------
			if (State->Step == 0)
			{
				ATraceCharacter* Target = (XSet != nullptr) ? FindEnemyTarget(TickWorld, XSet) : nullptr;
				if (XSet == nullptr || Target == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(FString::Printf(
							TEXT("could not stage: X=%s target=%s (%s)"),
							(XSet != nullptr) ? TEXT("ok") : TEXT("none"), *GetNameSafe(Target), *Why));
						State->List.Report();
						RestoreAllArms();
						return false;
					}
					return true;
				}

				State->Target = Target;
				UE_LOG(LogTraceGame, Display, TEXT("[XVULN] staged: X is %s (team %d), target %s (team %d)."),
					*GetNameSafe(XSet->GetCharacter()),
					static_cast<int32>(XSet->GetAbilityComponent()->GetTeam()),
					*GetNameSafe(Target), static_cast<int32>(Target->GetTeam()));

				State->Step = 1;
				return true;
			}

			ATraceCharacter* Target = State->Target.Get();
			if (XSet == nullptr || Target == nullptr || Target->Health == nullptr || !Target->IsAlive())
			{
				State->List.Invalidate(TEXT("a participant went away or died mid-test"));
				State->List.Report();
				RestoreAllArms();
				return false;
			}
			UTraceHealthComponent* TargetHealth = Target->Health;

			// ---- step 1: THE RED ARM -------------------------------------------------------
			if (State->Step == 1)
			{
				SetCVar(TEXT("Trace.X.Vulnerable"), 0);

				TargetHealth->ResetHealth();                 // clears the mark too, so mark AFTER this
				const bool bMarked = XSet->MarkVulnerable(Target);
				const bool bIsVulnerable = TargetHealth->IsVulnerable();
				const float Multiplier = TargetHealth->GetVulnerableDamageMultiplier();
				const float Delta = MeasureDamage(TargetHealth, XTestDamage);

				State->List.Check(bMarked && bIsVulnerable,
					TEXT("RED ARM: the mark still lands with the amplifier disarmed"),
					FString::Printf(TEXT("marked=%d isVulnerable=%d — this is what makes the arm a fair comparison"),
						bMarked ? 1 : 0, bIsVulnerable ? 1 : 0));

				State->List.Check(FMath::IsNearlyEqual(Delta, XTestDamage, 0.01f),
					TEXT("RED ARM: a marked target takes UNAMPLIFIED damage"),
					FString::Printf(TEXT("asked %.1f, took %.2f (multiplier reported x%.3f) — expected %.1f"),
						XTestDamage, Delta, Multiplier, XTestDamage));

				SetCVar(TEXT("Trace.X.Vulnerable"), 1);
				State->Step = 2;
				return true;
			}

			// ---- step 2: THE GREEN ARM, the identical call ---------------------------------
			if (State->Step == 2)
			{
				const float Expected = XTestDamage * TraceVulnerable::GetDamageMultiplier();

				TargetHealth->ResetHealth();
				const bool bMarked = XSet->MarkVulnerable(Target);
				const float Multiplier = TargetHealth->GetVulnerableDamageMultiplier();
				const float Delta = MeasureDamage(TargetHealth, XTestDamage);

				State->List.Check(bMarked, TEXT("GREEN: the mark lands on a living enemy"),
					FString::Printf(TEXT("marked=%d, %.2fs remaining"), bMarked ? 1 : 0,
						TargetHealth->GetVulnerableRemaining()));

				State->List.Check(FMath::IsNearlyEqual(Delta, Expected, 0.01f),
					TEXT("GREEN: +25% from a bullet-shaped source, at the health component"),
					FString::Printf(TEXT("asked %.1f, took %.2f (x%.3f) — expected %.2f"),
						XTestDamage, Delta, Multiplier, Expected));

				// ---- UNMARKED CONTROL: the fixture proving the number is the MARK's doing ----
				TargetHealth->ResetHealth();                 // ResetHealth clears the mark
				const float UnmarkedDelta = MeasureDamage(TargetHealth, XTestDamage);
				State->List.Check(FMath::IsNearlyEqual(UnmarkedDelta, XTestDamage, 0.01f),
					TEXT("GREEN: an UNMARKED target takes exactly the asked damage"),
					FString::Printf(TEXT("asked %.1f, took %.2f — this is what proves the %.2f above was the mark"),
						XTestDamage, UnmarkedDelta, Expected));

				// ---- NON-STACKING: mark twice, then measure ----
				TargetHealth->ResetHealth();
				XSet->MarkVulnerable(Target);
				XSet->MarkVulnerable(Target);
				XSet->MarkVulnerable(Target);
				const float TripleMarkedDelta = MeasureDamage(TargetHealth, XTestDamage);
				State->List.Check(FMath::IsNearlyEqual(TripleMarkedDelta, Expected, 0.01f),
					TEXT("NON-STACKING: three marks amplify exactly once"),
					FString::Printf(TEXT("took %.2f — expected %.2f, NOT %.2f (which is what stacking would give)"),
						TripleMarkedDelta, Expected, XTestDamage * FMath::Pow(TraceVulnerable::GetDamageMultiplier(), 3.f)));

				// Stamp a mark and let it decay; step 3 re-applies it and step 4 watches it expire.
				TargetHealth->ResetHealth();
				XSet->MarkVulnerable(Target);
				State->MarkedRemainingAtStamp = TargetHealth->GetVulnerableRemaining();
				State->Step = 3;
				State->NextStepRealTime = NowReal + 1.0;     // let ~1s of the 2s burn off
				return true;
			}

			// ---- step 3: THE TIMER RESET ---------------------------------------------------
			if (State->Step == 3)
			{
				const float BeforeReset = TargetHealth->GetVulnerableRemaining();
				XSet->MarkVulnerable(Target);
				const float AfterReset = TargetHealth->GetVulnerableRemaining();
				const float Duration = TraceVulnerable::GetDurationSeconds();

				State->List.Check(BeforeReset > 0.f && BeforeReset < Duration * 0.85f,
					TEXT("the mark decays on the match clock"),
					FString::Printf(TEXT("%.2fs left of %.2fs after ~1s of real time"), BeforeReset, Duration));

				State->List.Check(FMath::IsNearlyEqual(AfterReset, Duration, 0.15f),
					TEXT("'a new application RESETS the timer'"),
					FString::Printf(TEXT("%.2fs -> %.2fs, and NOT %.2fs (which is what extending would give)"),
						BeforeReset, AfterReset, BeforeReset + Duration));

				State->Step = 4;
				State->NextStepRealTime = NowReal + static_cast<double>(Duration) + 0.5;
				return true;
			}

			// ---- step 4: EXPIRY ------------------------------------------------------------
			const bool bStillMarked = TargetHealth->IsVulnerable();
			TargetHealth->ResetHealth();
			// ResetHealth clears the mark, so re-read expiry from the flag captured before it.
			const float ExpiredDelta = MeasureDamage(TargetHealth, XTestDamage);

			State->List.Check(!bStillMarked,
				TEXT("the mark expires on its own"),
				FString::Printf(TEXT("isVulnerable=%d after the duration elapsed"), bStillMarked ? 1 : 0));

			State->List.Check(FMath::IsNearlyEqual(ExpiredDelta, XTestDamage, 0.01f),
				TEXT("an expired target is back to unamplified damage"),
				FString::Printf(TEXT("took %.2f, expected %.1f"), ExpiredDelta, XTestDamage));

			State->List.Report();
			RestoreAllArms();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.X.CarrierTest — SPEC §4, three locks, red first
	// =============================================================================================

	struct FCarrierTestState
	{
		int32 Arm = 0;          // 0 = RED (all three locks removed), 1 = GREEN (shipped)
		int32 Step = 0;
		double Deadline = 0.0;
		TWeakObjectPtr<ATraceCharacter> CarrierPawn;
		TWeakObjectPtr<ATraceCharacter> ControlPawn;
		FXChecklist List;

		bool bRedMarkedCarrier = false;
		bool bRedAmplifiedCarrier = false;
		bool bGreenChokeRefused = false;
		bool bGreenDirectRefused = false;
		bool bGreenNoAmplify = false;
		bool bControlWorked = false;
	};

	void RunCarrierTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XCARRIER] no authoritative game world — run this on the server."));
			return;
		}

		TSharedPtr<FCarrierTestState> State = MakeShared<FCarrierTestState>();
		State->List.Tag = TEXT("XCARRIER");
		State->Deadline = FPlatformTime::Seconds() + 120.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[XCARRIER] ===== spec v14 §4: 'NO abilities damage carriers', and §6's X 'must not become a damage "
			     "path'. THREE locks: (1) the choke point refuses Control on a carrier, (2) ApplyVulnerable refuses a "
			     "carrier on its own, (3) the multiplier is 1 for a carrier AND is evaluated after ApplyDamage's "
			     "carrier early-out. arm 0 removes all three and MUST reproduce; arm 1 is shipped and must not. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreAllArms();
				return false;
			}

			FString Why;
			UTraceAbilitySetX* XSet = MakePlayerIntoX(TickWorld, Why);
			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);

			// ---- staging: a carrier who is not X, plus a non-carrier control -------------------
			if (State->Step == 0)
			{
				ATraceCharacter* XPawn = (XSet != nullptr) ? XSet->GetCharacter() : nullptr;
				ATraceCharacter* CarrierPawn = (CoreActor != nullptr) ? CoreActor->Carrier : nullptr;

				// THE CARRIER MUST BE AN ENEMY OF X, AND THAT IS NOT A DETAIL.
				//
				// The first version accepted whoever happened to be holding the Core. In a live
				// match that is often one of X's OWN TEAM — and then the red arm, which removes the
				// three carrier locks, still marks nobody, because the FRIENDLY-FIRE rule refuses
				// it for a completely different reason. The harness duly reported "the RED arm did
				// not reproduce" and invalidated a run in which nothing was actually wrong. A
				// fixture that cannot go red is not a fixture, and this one could not go red on any
				// frame where a Blue player was carrying.
				//
				// GrantTo, not TryPickup: TryPickup is a no-op while there is a holder, which is
				// exactly the case that has to be corrected.
				const bool bCarrierUnusable =
					(CarrierPawn == nullptr) || (CarrierPawn == XPawn)
					|| (XPawn != nullptr && CarrierPawn->GetTeam() == XPawn->GetTeam());

				if (bCarrierUnusable && CoreActor != nullptr && XSet != nullptr)
				{
					if (ATraceCharacter* Volunteer = FindEnemyTarget(TickWorld, XSet))
					{
						CoreActor->GrantTo(Volunteer, ETraceCoreGrantReason::Debug);
					}
					CarrierPawn = CoreActor->Carrier;
				}

				ATraceCharacter* ControlPawn = (XSet != nullptr) ? FindEnemyTarget(TickWorld, XSet) : nullptr;

				// The same-team carrier is REJECTED here too, not merely corrected above: if the
				// grant did not take (the Core was mid-flight, the volunteer died in the same
				// frame), staging must wait rather than proceed with a carrier whose immunity would
				// be produced by the friendly-fire rule.
				const bool bCarrierIsAlly =
					(CarrierPawn != nullptr && XPawn != nullptr && CarrierPawn->GetTeam() == XPawn->GetTeam());

				if (XSet == nullptr || CarrierPawn == nullptr || CarrierPawn == XPawn || bCarrierIsAlly
					|| ControlPawn == nullptr || ControlPawn == CarrierPawn)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(FString::Printf(
							TEXT("could not stage: X=%s carrier=%s control=%s (%s)"),
							(XSet != nullptr) ? TEXT("ok") : TEXT("none"),
							*GetNameSafe(CarrierPawn), *GetNameSafe(ControlPawn), *Why));
						State->List.Report();
						RestoreAllArms();
						return false;
					}
					return true;
				}

				State->CarrierPawn = CarrierPawn;
				State->ControlPawn = ControlPawn;
				State->Step = 1;

				UE_LOG(LogTraceGame, Display,
					TEXT("[XCARRIER] staged: X %s (team %d) | CARRIER %s (team %d) | CONTROL %s (team %d)"),
					*GetNameSafe(XPawn), static_cast<int32>(XSet->GetAbilityComponent()->GetTeam()),
					*GetNameSafe(CarrierPawn), static_cast<int32>(CarrierPawn->GetTeam()),
					*GetNameSafe(ControlPawn), static_cast<int32>(ControlPawn->GetTeam()));
				return true;
			}

			ATraceCharacter* CarrierPawn = State->CarrierPawn.Get();
			ATraceCharacter* ControlPawn = State->ControlPawn.Get();
			if (XSet == nullptr || CarrierPawn == nullptr || ControlPawn == nullptr
				|| CarrierPawn->Health == nullptr || ControlPawn->Health == nullptr)
			{
				State->List.Invalidate(TEXT("a participant went away mid-test"));
				State->List.Report();
				RestoreAllArms();
				return false;
			}

			// PRECONDITION, HELD EVERY TICK. Bots pass and score; a victim who stopped being the
			// carrier between staging and the strike would turn the case under test into an ordinary
			// damage test that passes for the wrong reason.
			if (CoreActor != nullptr && CoreActor->Carrier != CarrierPawn)
			{
				CoreActor->TryPickup(CarrierPawn);
			}
			if (CoreActor == nullptr || CoreActor->Carrier != CarrierPawn)
			{
				if (NowReal > State->Deadline)
				{
					State->List.Invalidate(TEXT("could not keep the Core on the victim"));
					State->List.Report();
					RestoreAllArms();
					return false;
				}
				return true;
			}

			// ---- arm 0: RED. All three locks removed. -----------------------------------------
			if (State->Arm == 0)
			{
				SetCVar(TEXT("Trace.Ability.CarrierImmune"), 0);        // lock 1: the choke point
				SetCVar(TEXT("Trace.X.VulnerableCarrierImmune"), 0);    // locks 2 and 3's carrier test
				SetCVar(TEXT("Trace.X.VulnerableApplyOrder"), 0);       // lock 3's ordering

				const int32 MarkedBefore = TraceVulnerable::GetCarrierMarkedCount();
				const int32 AmplifiedBefore = TraceVulnerable::GetCarrierAmplifiedCount();

				const bool bMarked = XSet->MarkVulnerable(CarrierPawn);
				const bool bIsVulnerable = CarrierPawn->Health->IsVulnerable();
				const float CarrierHealthBefore = CarrierPawn->Health->Health;
				CarrierPawn->Health->ApplyDamage(XTestDamage, nullptr, XTestCause);
				const float CarrierDelta = CarrierHealthBefore - CarrierPawn->Health->Health;

				State->bRedMarkedCarrier = bMarked && bIsVulnerable
					&& (TraceVulnerable::GetCarrierMarkedCount() > MarkedBefore);
				State->bRedAmplifiedCarrier = TraceVulnerable::GetCarrierAmplifiedCount() > AmplifiedBefore;

				UE_LOG(LogTraceGame, Display,
					TEXT("[XCARRIER] arm=0 (RED, all three locks removed): carrier MARKED=%d isVulnerable=%d "
					     "carrierMarkedAlarm +%d | amplifierRanOnCarrier=%d carrierAmplifiedAlarm +%d | "
					     "carrier health delta %.2f (0 is expected — the ordinary damage SHIELD is a separate rule "
					     "and is untouched by this test)"),
					bMarked ? 1 : 0, bIsVulnerable ? 1 : 0,
					TraceVulnerable::GetCarrierMarkedCount() - MarkedBefore,
					State->bRedAmplifiedCarrier ? 1 : 0,
					TraceVulnerable::GetCarrierAmplifiedCount() - AmplifiedBefore, CarrierDelta);

				UE_LOG(LogTraceGame, Display, TEXT("[XCARRIER] arm=0 (RED) %s"),
					(State->bRedMarkedCarrier && State->bRedAmplifiedCarrier)
						? TEXT("REPRODUCED — with the rules removed the mark reached the carrier AND the multiplier "
						       "was evaluated on their damage.")
						: TEXT("*** DID NOT REPRODUCE — the arms did not disarm anything, so arm 1 proves nothing. ***"));

				CarrierPawn->Health->ResetHealth();     // also clears the mark the red arm applied
				State->Arm = 1;
				return true;
			}

			// ---- arm 1: GREEN. The shipped build. ---------------------------------------------
			RestoreAllArms();

			const int32 MarkedBefore = TraceVulnerable::GetCarrierMarkedCount();
			const int32 AmplifiedBefore = TraceVulnerable::GetCarrierAmplifiedCount();

			CarrierPawn->Health->ResetHealth();

			// LOCK 1: the ability's own path, through UTraceCharacterAbilitySet::CanAffect(Control).
			const bool bChokeMarked = XSet->MarkVulnerable(CarrierPawn);
			State->bGreenChokeRefused = !bChokeMarked && !CarrierPawn->Health->IsVulnerable();

			// LOCK 2: the health component ON ITS OWN, with the choke point bypassed entirely. This is
			// the "a future caller forgets the choke point" case, and it is the reason lock 2 exists.
			const bool bDirectMarked = CarrierPawn->Health->ApplyVulnerable(TraceVulnerable::GetDurationSeconds(), nullptr);
			State->bGreenDirectRefused = !bDirectMarked && !CarrierPawn->Health->IsVulnerable();

			// LOCK 3: damage on the carrier, with the multiplier where it ships. The health delta is
			// zero because of the ORDINARY carrier shield, which is a different rule — so the thing
			// actually measured here is the ALARM: the multiplier must not have been evaluated at all.
			const float CarrierHealthBefore = CarrierPawn->Health->Health;
			CarrierPawn->Health->ApplyDamage(XTestDamage, nullptr, XTestCause);
			const float CarrierDelta = CarrierHealthBefore - CarrierPawn->Health->Health;
			State->bGreenNoAmplify = (TraceVulnerable::GetCarrierAmplifiedCount() == AmplifiedBefore)
				&& (TraceVulnerable::GetCarrierMarkedCount() == MarkedBefore);

			// ---- THE CONTROL. The identical calls on a non-carrier enemy MUST work, or this
			//      harness has proved something about itself and nothing about the carrier rule. ----
			ControlPawn->Health->ResetHealth();
			const bool bControlMarked = XSet->MarkVulnerable(ControlPawn);
			const float Expected = XTestDamage * TraceVulnerable::GetDamageMultiplier();
			const float ControlDelta = MeasureDamage(ControlPawn->Health, XTestDamage);
			State->bControlWorked = bControlMarked && FMath::IsNearlyEqual(ControlDelta, Expected, 0.01f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[XCARRIER] arm=1 (GREEN, shipped): chokeRefused=%d directRefused=%d amplifierNeverRan=%d | "
				     "carrier health delta %.2f"),
				State->bGreenChokeRefused ? 1 : 0, State->bGreenDirectRefused ? 1 : 0,
				State->bGreenNoAmplify ? 1 : 0, CarrierDelta);
			UE_LOG(LogTraceGame, Display,
				TEXT("[XCARRIER] arm=1 CONTROL (non-carrier enemy, IDENTICAL calls): marked=%d damage %.2f "
				     "(expected %.2f) -> fixture %s"),
				bControlMarked ? 1 : 0, ControlDelta, Expected,
				State->bControlWorked ? TEXT("REACHES HEALTH") : TEXT("*** DEAD — measures nothing ***"));

			// ---- verdict ----------------------------------------------------------------------
			if (!State->bControlWorked)
			{
				State->List.Invalidate(TEXT("the CONTROL strike on a non-carrier enemy did not land — the fixture "
				                            "cannot reach health, so nothing it says about the carrier means anything"));
			}
			else if (!State->bRedMarkedCarrier || !State->bRedAmplifiedCarrier)
			{
				State->List.Invalidate(FString::Printf(
					TEXT("the RED arm did not reproduce (markedCarrier=%d amplifiedCarrier=%d) — with nothing "
					     "falsified, the green arm's clean run is uninformative"),
					State->bRedMarkedCarrier ? 1 : 0, State->bRedAmplifiedCarrier ? 1 : 0));
			}

			State->List.Check(State->bRedMarkedCarrier,
				TEXT("RED: with Trace.Ability.CarrierImmune 0 the mark REACHES the carrier"),
				TEXT("this is the failure the shipped build must prevent"));
			State->List.Check(State->bRedAmplifiedCarrier,
				TEXT("RED: with Trace.X.VulnerableApplyOrder 0 the multiplier RUNS on a carrier's damage"),
				TEXT("the ordering claim, made observable — the carrier check is a return, so health cannot show it"));
			State->List.Check(State->bGreenChokeRefused,
				TEXT("LOCK 1 (GREEN): the choke point refuses Control on a carrier"),
				TEXT("UTraceCharacterAbilitySet::CanAffect(Target, Control)"));
			State->List.Check(State->bGreenDirectRefused,
				TEXT("LOCK 2 (GREEN): ApplyVulnerable refuses a carrier with the choke point BYPASSED"),
				TEXT("a future caller that forgets the choke point still cannot mark one"));
			State->List.Check(State->bGreenNoAmplify,
				TEXT("LOCK 3 (GREEN): the multiplier is never evaluated on a carrier's damage"),
				FString::Printf(TEXT("carrierAmplified alarm +%d and carrierMarked alarm +%d across this arm (both "
				                     "must be +0; the running totals are %d and %d and include the red arm's)"),
					TraceVulnerable::GetCarrierAmplifiedCount() - AmplifiedBefore,
					TraceVulnerable::GetCarrierMarkedCount() - MarkedBefore,
					TraceVulnerable::GetCarrierAmplifiedCount(), TraceVulnerable::GetCarrierMarkedCount()));
			State->List.Check(State->bControlWorked,
				TEXT("CONTROL: the identical calls DO work on a non-carrier enemy"),
				FString::Printf(TEXT("%.2f damage, expected %.2f"), ControlDelta, Expected));

			State->List.Report();
			RestoreAllArms();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.X.StingTest
	// =============================================================================================

	void RunStingTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XSTING] no authoritative game world."));
			return;
		}

		TSharedPtr<FXChecklist> List = MakeShared<FXChecklist>();
		List->Tag = TEXT("XSTING");
		TSharedPtr<double> DeadlinePtr = MakeShared<double>(FPlatformTime::Seconds() + 60.0);

		UE_LOG(LogTraceGame, Display,
			TEXT("[XSTING] ===== spec v14 §6: 'loads the 5 bees into his gun and they stop orbiting. His NEXT FIVE "
			     "BULLETS apply vulnerable on hit, at NORMAL damage. When all five are fired the bees resume "
			     "orbiting. 25s cooldown.' ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[List, DeadlinePtr, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}

			FString Why;
			UTraceAbilitySetX* XSet = MakePlayerIntoX(TickWorld, Why);
			ATraceCharacter* Target = (XSet != nullptr) ? FindEnemyTarget(TickWorld, XSet) : nullptr;
			ATraceCharacter* XPawn = (XSet != nullptr) ? XSet->GetCharacter() : nullptr;

			if (XSet == nullptr || Target == nullptr || XPawn == nullptr)
			{
				if (FPlatformTime::Seconds() > *DeadlinePtr)
				{
					List->Invalidate(FString::Printf(TEXT("could not stage (%s)"), *Why));
					List->Report();
					return false;
				}
				return true;
			}

			UTraceAbilityComponent* Comp = XSet->GetAbilityComponent();

			// ---- the cooldown is X's own 25, not the framework's default 20 ----
			List->Check(FMath::IsNearlyEqual(XSet->GetActivatedCooldownSeconds(),
					FMath::Clamp(UTraceSettings::Get().XStingCooldownSeconds, 0.f, 180.f), 0.01f)
				&& XSet->GetActivatedCooldownSeconds() > UTraceSettings::Get().AbilityDefaultCooldownSeconds,
				TEXT("Sting's cooldown is X's own, and is longer than the default"),
				FString::Printf(TEXT("%.1fs vs the framework default %.1fs (§6 says 25 — 'the only 25')"),
					XSet->GetActivatedCooldownSeconds(), UTraceSettings::Get().AbilityDefaultCooldownSeconds));

			// ---- load ----
			const int32 Expected = TraceXBees::GetStingBulletCount();
			const bool bFired = XSet->ActivateAbility();
			List->Check(bFired && XSet->GetLoadedBees() == Expected && XSet->IsStingLoaded(),
				TEXT("ActivateAbility loads the bees into the gun"),
				FString::Printf(TEXT("fired=%d loaded=%d, expected %d"), bFired ? 1 : 0, XSet->GetLoadedBees(), Expected));

			// ---- "they stop orbiting": the body cannot sting while loaded ----
			Target->Health->ResetHealth();
			const FVector Saved = Target->GetActorLocation();
			Target->SetActorLocation(XPawn->GetActorLocation() + FVector(20.f, 0.f, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
			XSet->TickAbilities(0.016f);
			const bool bStungWhileLoaded = Target->Health->IsVulnerable();
			Target->SetActorLocation(Saved, false, nullptr, ETeleportType::TeleportPhysics);
			List->Check(!bStungWhileLoaded,
				TEXT("'they stop orbiting': a loaded X cannot also sting with his body"),
				FString::Printf(TEXT("target standing inside the orbit radius, vulnerable=%d (must be 0)"),
					bStungWhileLoaded ? 1 : 0));

			// ---- a carrier does not spend a bee ----
			int32 BeesBeforeCarrier = XSet->GetLoadedBees();
			if (ATraceCore* CoreActor = ATraceCore::Get(TickWorld))
			{
				if (ATraceCharacter* CarrierPawn = CoreActor->Carrier)
				{
					if (CarrierPawn != XPawn)
					{
						TraceAbilityWeaponHooks::OnBulletHit(XPawn, CarrierPawn, false);
						List->Check(XSet->GetLoadedBees() == BeesBeforeCarrier
							&& (CarrierPawn->Health == nullptr || !CarrierPawn->Health->IsVulnerable()),
							TEXT("a Sting bullet into the CORE CARRIER marks nothing and spends no bee"),
							FString::Printf(TEXT("%d -> %d bees, carrier vulnerable=%d"),
								BeesBeforeCarrier, XSet->GetLoadedBees(),
								(CarrierPawn->Health != nullptr && CarrierPawn->Health->IsVulnerable()) ? 1 : 0));
					}
				}
			}

			// ---- five bullets ----
			const int32 StartBees = XSet->GetLoadedBees();
			int32 MarksLanded = 0;
			for (int32 Shot = 0; Shot < StartBees; ++Shot)
			{
				Target->Health->ClearVulnerable();
				TraceAbilityWeaponHooks::OnBulletHit(XPawn, Target, false);
				if (Target->Health->IsVulnerable())
				{
					++MarksLanded;
				}
			}

			List->Check(MarksLanded == StartBees,
				TEXT("every loaded bullet applies the mark"),
				FString::Printf(TEXT("%d of %d bullets marked the target"), MarksLanded, StartBees));

			List->Check(XSet->GetLoadedBees() == 0 && !XSet->IsStingLoaded(),
				TEXT("'when all five are fired the bees resume orbiting'"),
				FString::Printf(TEXT("loaded=%d, stingLoaded=%d"), XSet->GetLoadedBees(), XSet->IsStingLoaded() ? 1 : 0));

			// ---- the sixth bullet is an ordinary bullet ----
			Target->Health->ClearVulnerable();
			TraceAbilityWeaponHooks::OnBulletHit(XPawn, Target, false);
			List->Check(!Target->Health->IsVulnerable() && XSet->GetLoadedBees() == 0,
				TEXT("the SIXTH bullet marks nothing"),
				FString::Printf(TEXT("vulnerable=%d loaded=%d"),
					Target->Health->IsVulnerable() ? 1 : 0, XSet->GetLoadedBees()));

			// ---- 'at NORMAL damage': a Sting bullet is not amplified by the mark it delivers ----
			// The gun applies damage BEFORE the hook, so the delivering bullet sees an unmarked
			// target. Reproduce that ordering exactly.
			Target->Health->ResetHealth();
			XSet->ActivateAbility();
			const float DeliveringDelta = MeasureDamage(Target->Health, XTestDamage);
			TraceAbilityWeaponHooks::OnBulletHit(XPawn, Target, false);
			const float FollowUpDelta = MeasureDamage(Target->Health, XTestDamage);

			List->Check(FMath::IsNearlyEqual(DeliveringDelta, XTestDamage, 0.01f),
				TEXT("'at NORMAL damage': the bullet that DELIVERS the mark is not amplified by it"),
				FString::Printf(TEXT("delivering bullet took %.2f, expected %.1f"), DeliveringDelta, XTestDamage));
			List->Check(FMath::IsNearlyEqual(FollowUpDelta, XTestDamage * TraceVulnerable::GetDamageMultiplier(), 0.01f),
				TEXT("...and the NEXT bullet is"),
				FString::Printf(TEXT("follow-up took %.2f, expected %.2f"),
					FollowUpDelta, XTestDamage * TraceVulnerable::GetDamageMultiplier()));

			if (Comp != nullptr)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[XSTING] cooldown after activation: %.1fs remaining."),
					Comp->GetActivatedCooldownRemaining());
			}

			Target->Health->ClearVulnerable();
			Target->Health->ResetHealth();
			List->Report();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.X.SpeedTest — "+10% while ANY ENEMY is vulnerable"
	// =============================================================================================

	struct FSpeedTestState
	{
		int32 Step = 0;
		double Deadline = 0.0;
		FXChecklist List;
		TWeakObjectPtr<ATraceCharacter> Enemy;
		TWeakObjectPtr<ATraceCharacter> Mate;
	};

	void RunSpeedTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XSPEED] no authoritative game world."));
			return;
		}

		TSharedPtr<FSpeedTestState> State = MakeShared<FSpeedTestState>();
		State->List.Tag = TEXT("XSPEED");
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[XSPEED] ===== spec v14 §6 movement: '+10%% speed while ANY enemy is vulnerable.' Measured through "
			     "UTraceAbilityComponent::GetMoveSpeedMultiplierFor — the function the movement component is meant to "
			     "call. IT DOES NOT CALL IT YET; see the report. ====="));

		// *** ONE MEASUREMENT PER FRAME, AND THAT IS NOT COSMETIC. ***
		//
		// UTraceAbilitySetX::GetMoveSpeedMultiplier caches its answer for the frame (the hook's
		// contract asks for "cheap and pure" and it is called from the movement tick). The FIRST
		// version of this test took all four readings inside one ticker callback and reported
		// "x1.0000, expected x1.1000" for the marked-enemy case — not a broken passive, a harness
		// reading its own cached baseline three more times. Worse, the marked-TEAMMATE check went
		// GREEN for exactly the same wrong reason. Stepping one frame at a time is what makes each
		// reading a fresh evaluation, and it is the reason that check can now be believed.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}

			FString Why;
			UTraceAbilitySetX* XSet = MakePlayerIntoX(TickWorld, Why);
			ATraceCharacter* XPawn = (XSet != nullptr) ? XSet->GetCharacter() : nullptr;

			if (XSet == nullptr || XPawn == nullptr)
			{
				if (FPlatformTime::Seconds() > State->Deadline)
				{
					State->List.Invalidate(FString::Printf(TEXT("could not stage (%s)"), *Why));
					State->List.Report();
					return false;
				}
				return true;
			}

			const float Bonus = TraceXBees::GetSpeedBonusFraction();

			// ---- frame 0: stage and clear every mark in the level ----
			if (State->Step == 0)
			{
				ATraceCharacter* Enemy = FindEnemyTarget(TickWorld, XSet);
				if (Enemy == nullptr)
				{
					if (FPlatformTime::Seconds() > State->Deadline)
					{
						State->List.Invalidate(TEXT("no living non-carrier enemy"));
						State->List.Report();
						return false;
					}
					return true;
				}
				State->Enemy = Enemy;
				State->Mate = FindTeammate(TickWorld, XSet);

				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					if (*It != nullptr && It->Health != nullptr) { It->Health->ClearVulnerable(); }
				}
				State->Step = 1;
				return true;
			}

			ATraceCharacter* Enemy = State->Enemy.Get();
			ATraceCharacter* Mate = State->Mate.Get();
			if (Enemy == nullptr || Enemy->Health == nullptr)
			{
				State->List.Invalidate(TEXT("the enemy went away mid-test"));
				State->List.Report();
				return false;
			}

			// ---- frame 1: the baseline ----
			if (State->Step == 1)
			{
				const float Baseline = UTraceAbilityComponent::GetMoveSpeedMultiplierFor(XPawn);
				State->List.Check(FMath::IsNearlyEqual(Baseline, 1.f, 0.001f),
					TEXT("no marks -> no boost"),
					FString::Printf(TEXT("x%.4f, expected 1.0"), Baseline));

				// Mark the teammate now; frame 2 reads it.
				if (Mate != nullptr && Mate->Health != nullptr)
				{
					Mate->Health->ApplyVulnerable(TraceVulnerable::GetDurationSeconds(), nullptr);
				}
				State->Step = 2;
				return true;
			}

			// ---- frame 2: a marked TEAMMATE must NOT switch it on ----
			if (State->Step == 2)
			{
				if (Mate != nullptr && Mate->Health != nullptr && Mate->Health->IsVulnerable())
				{
					const float WithMate = UTraceAbilityComponent::GetMoveSpeedMultiplierFor(XPawn);
					State->List.Check(FMath::IsNearlyEqual(WithMate, 1.f, 0.001f),
						TEXT("'ANY ENEMY' — a marked TEAMMATE does not switch it on"),
						FString::Printf(TEXT("teammate %s IS marked (%.2fs), X is x%.4f, expected 1.0"),
							*GetNameSafe(Mate), Mate->Health->GetVulnerableRemaining(), WithMate));
					Mate->Health->ClearVulnerable();
				}
				else
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[XSPEED] SKIPPED the marked-teammate half: no living non-carrier teammate could be marked."));
				}

				Enemy->Health->ApplyVulnerable(TraceVulnerable::GetDurationSeconds(), nullptr);
				State->Step = 3;
				return true;
			}

			// ---- frame 3: a marked ENEMY switches it on ----
			if (State->Step == 3)
			{
				const float Boosted = UTraceAbilityComponent::GetMoveSpeedMultiplierFor(XPawn);
				State->List.Check(FMath::IsNearlyEqual(Boosted, 1.f + Bonus, 0.001f),
					TEXT("a marked ENEMY switches the boost on"),
					FString::Printf(TEXT("enemy %s marked (%.2fs), X is x%.4f, expected x%.4f (+%.0f%%)"),
						*GetNameSafe(Enemy), Enemy->Health->GetVulnerableRemaining(), Boosted,
						1.f + Bonus, Bonus * 100.f));

				Enemy->Health->ClearVulnerable();
				State->Step = 4;
				return true;
			}

			// ---- frame 4: and off again ----
			const float AfterClear = UTraceAbilityComponent::GetMoveSpeedMultiplierFor(XPawn);
			State->List.Check(FMath::IsNearlyEqual(AfterClear, 1.f, 0.001f),
				TEXT("the boost ends with the mark"),
				FString::Printf(TEXT("x%.4f, expected 1.0"), AfterClear));

			State->List.Report();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.X.BeeTest — the passive's contact test, with its own red arm (distance)
	// =============================================================================================

	struct FBeeTestState
	{
		int32 Step = 0;
		int32 TicksInPhase = 0;
		double Deadline = 0.0;
		FVector SavedLocation = FVector::ZeroVector;
		TWeakObjectPtr<ATraceCharacter> Enemy;
		FXChecklist List;
		bool bMarkedFar = false;
		bool bMarkedNear = false;
	};

	void RunBeeTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XBEE] no authoritative game world."));
			return;
		}

		TSharedPtr<FBeeTestState> State = MakeShared<FBeeTestState>();
		State->List.Tag = TEXT("XBEE");
		State->Deadline = FPlatformTime::Seconds() + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[XBEE] ===== spec v14 §6 passive [ASSUMPTION]: 'the orbiting bees hit on contact — X's body is the "
			     "delivery mechanism.' TWO placements on one binary: an enemy at THREE TIMES the orbit radius must "
			     "NOT be marked (the red half — a sweep that marked the whole level would otherwise report green), "
			     "and an enemy standing INSIDE the orbit must be. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				return false;
			}

			FString Why;
			UTraceAbilitySetX* XSet = MakePlayerIntoX(TickWorld, Why);
			ATraceCharacter* XPawn = (XSet != nullptr) ? XSet->GetCharacter() : nullptr;

			if (State->Step == 0)
			{
				ATraceCharacter* Enemy = (XSet != nullptr) ? FindEnemyTarget(TickWorld, XSet) : nullptr;
				if (XSet == nullptr || XPawn == nullptr || Enemy == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(FString::Printf(TEXT("could not stage (%s)"), *Why));
						State->List.Report();
						return false;
					}
					return true;
				}
				State->Enemy = Enemy;
				State->SavedLocation = Enemy->GetActorLocation();
				Enemy->Health->ClearVulnerable();
				State->Step = 1;
				State->TicksInPhase = 0;

				UE_LOG(LogTraceGame, Display, TEXT("[XBEE] staged: X %s, enemy %s, orbit %.0fuu, bee hit radius %.0fuu."),
					*GetNameSafe(XPawn), *GetNameSafe(Enemy),
					TraceXBees::GetOrbitRadiusUU(), TraceXBees::GetBeeHitRadiusUU());
				return true;
			}

			ATraceCharacter* Enemy = State->Enemy.Get();

			// RE-ACQUIRE, DO NOT ABORT.
			//
			// The first version of this test invalidated the moment a participant was unavailable for
			// one tick, and it duly reported "INVALID — a participant went away mid-test" seven ticks
			// in on a run that had been green minutes earlier. In a live match with bots the staged
			// enemy can be shot dead, can pick up the Core, or can be mid-respawn on any given frame,
			// and the local pawn is briefly null across a respawn. None of that is a fact about the
			// bees. Put the enemy back where it was, find another, restart the phase, and only give up
			// at the deadline — a harness that reports INVALID for the world doing normal things is a
			// harness whose green means nothing either.
			if (XSet == nullptr || XPawn == nullptr || Enemy == nullptr || Enemy->Health == nullptr || !Enemy->IsAlive())
			{
				if (NowReal > State->Deadline)
				{
					State->List.Invalidate(FString::Printf(
						TEXT("could not hold a scenario together for long enough (X=%s pawn=%s enemy=%s)"),
						(XSet != nullptr) ? TEXT("ok") : TEXT("none"), *GetNameSafe(XPawn), *GetNameSafe(Enemy)));
					State->List.Report();
					return false;
				}

				if (Enemy != nullptr && !State->SavedLocation.IsZero())
				{
					Enemy->SetActorLocation(State->SavedLocation, false, nullptr, ETeleportType::TeleportPhysics);
				}
				State->Enemy = nullptr;
				State->Step = 0;          // re-stage from scratch, including a fresh red half
				State->TicksInPhase = 0;
				State->bMarkedFar = false;
				State->bMarkedNear = false;
				return true;
			}

			const float OrbitRadius = TraceXBees::GetOrbitRadiusUU();

			// ---- phase 1: THE RED HALF. Far away, for a full orbit's worth of ticks. ----
			if (State->Step == 1)
			{
				Enemy->SetActorLocation(XPawn->GetActorLocation() + FVector(OrbitRadius * 3.f, 0.f, 0.f),
					false, nullptr, ETeleportType::TeleportPhysics);
				XSet->TickAbilities(0.016f);
				if (Enemy->Health->IsVulnerable())
				{
					State->bMarkedFar = true;
				}

				if (++State->TicksInPhase < 90)
				{
					return true;
				}

				State->List.Check(!State->bMarkedFar,
					TEXT("RED HALF: an enemy at 3x the orbit radius is NOT stung"),
					FString::Printf(TEXT("held at %.0fuu for 90 ticks, marked=%d (must be 0)"),
						OrbitRadius * 3.f, State->bMarkedFar ? 1 : 0));

				State->Step = 2;
				State->TicksInPhase = 0;
				return true;
			}

			// ---- phase 2: a body placed where a BEE IS.
			//
			// The enemy is put at bee 0's current position every tick rather than at a fixed point on
			// the orbit circle, and that is not a shortcut — it is what makes the check independent of
			// whether the match clock is advancing. Five bees 72° apart on a 120 uu ring are 141 uu
			// from each other, so a body parked at a fixed angle while the clock is FROZEN (this
			// project's character-select screen has paused the world before) would sit in a permanent
			// gap and this test would report a red that was really a stopped clock. Following bee 0
			// asks the question that actually matters — "does a body at a bee's location get stung" —
			// and asks it the same way on a running clock and a stopped one.
			{
				float MatchNow = 0.f;
				if (const AGameStateBase* StateBase = TickWorld->GetGameState())
				{
					MatchNow = static_cast<float>(StateBase->GetServerWorldTimeSeconds());
				}
				const FVector BeeZero = TraceXBees::GetBeeLocation(
					TraceXBees::GetSwarmCentre(XPawn), MatchNow, 0, TraceXBees::GetBeeCount(),
					OrbitRadius, TraceXBees::GetOrbitSpeedDegreesPerSecond());

				Enemy->SetActorLocation(BeeZero, false, nullptr, ETeleportType::TeleportPhysics);
				XSet->TickAbilities(0.016f);
				if (Enemy->Health->IsVulnerable())
				{
					State->bMarkedNear = true;
				}

				if (!State->bMarkedNear && ++State->TicksInPhase < 30)
				{
					return true;
				}

				State->List.Check(State->bMarkedNear,
					TEXT("a body standing where a BEE is gets stung by the passive"),
					FString::Printf(TEXT("placed at bee 0 (%.0fuu ring), marked=%d after %d ticks, matchClock=%.2f"),
						OrbitRadius, State->bMarkedNear ? 1 : 0, State->TicksInPhase, MatchNow));
			}

			Enemy->SetActorLocation(State->SavedLocation, false, nullptr, ETeleportType::TeleportPhysics);
			Enemy->Health->ClearVulnerable();
			State->List.Report();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.X.Showcase — stage the two VISUALS so a screenshot can be read
	//
	// Nobody can verify a renderer by reading a log (the argument TraceAutoShot.h makes, and it
	// applies to gameplay cosmetics too). This puts a marked enemy in front of X at a readable
	// distance and marks them for long enough to photograph, so the orbiting bees and the vulnerable
	// ring are things somebody can look at rather than things a log line claims exist.
	// =============================================================================================

	void RunShowcase()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[XSHOW] no authoritative game world."));
			return;
		}

		// A TICKER, NOT A ONE-SHOT, and the first version of this WAS a one-shot.
		//
		// It placed the enemy once, three seconds before the capture, and the screenshot came back
		// with an empty corridor: a bot that is put somewhere simply runs off again, because it is a
		// bot with a bot's orders. The stage has to be HELD, not set. Fifteen seconds covers any
		// -TraceAutoShot delay.
		const double EndRealTime = FPlatformTime::Seconds() + 15.0;
		TSharedPtr<bool> Announced = MakeShared<bool>(false);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[EndRealTime, Announced, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr || FPlatformTime::Seconds() > EndRealTime)
			{
				return false;
			}

			FString Why;
			UTraceAbilitySetX* XSet = MakePlayerIntoX(TickWorld, Why);
			ATraceCharacter* XPawn = (XSet != nullptr) ? XSet->GetCharacter() : nullptr;
			if (XSet == nullptr || XPawn == nullptr)
			{
				return true;
			}

			ATraceCharacter* Enemy = FindEnemyTarget(TickWorld, XSet);
			if (Enemy == nullptr || Enemy->Health == nullptr)
			{
				return true;
			}

			// Straight down the barrel: the head ring is then centred in frame rather than wherever
			// the bot's pathing happened to leave it.
			const FVector Forward = XPawn->GetActorForwardVector();
			const FVector Placement = XPawn->GetActorLocation() + Forward * 380.f;
			Enemy->SetActorLocation(Placement, false, nullptr, ETeleportType::TeleportPhysics);
			Enemy->SetActorRotation((XPawn->GetActorLocation() - Placement).Rotation());

			// Re-marked every tick with a long duration, purely so the capture window is not a race.
			// Gameplay is 2 s; this is a stage light, and Trace.X.Dump prints the real duration.
			Enemy->Health->ApplyVulnerable(20.f, nullptr);

			if (!*Announced)
			{
				*Announced = true;
				UE_LOG(LogTraceGame, Display,
					TEXT("[XSHOW] holding the stage for 15s: X %s with %d bees orbiting at %.0fuu; enemy %s pinned "
					     "%.0fuu dead ahead and MARKED (%.1fs shown, gameplay duration is %.1fs). Loaded bees: %d."),
					*GetNameSafe(XPawn), TraceXBees::GetBeeCount(), TraceXBees::GetOrbitRadiusUU(),
					*GetNameSafe(Enemy), FVector::Dist(XPawn->GetActorLocation(), Placement),
					Enemy->Health->GetVulnerableRemaining(), TraceVulnerable::GetDurationSeconds(),
					XSet->GetLoadedBees());
			}
			return true;
		}));
	}

	// =============================================================================================
	// Registration. NONE of these names is also a CVar — that collision is fatal at module load.
	// The CVars are Trace.X.Vulnerable / .VulnerableApplyOrder / .VulnerableCarrierImmune (nouns);
	// every command here ends in Test or is Dump (verbs).
	// =============================================================================================

	FAutoConsoleCommand CmdXDump(
		TEXT("Trace.X.Dump"),
		TEXT("Spec v14 §6: what this process resolved for X — bees, the vulnerable mark, Sting, the arms and the "
		     "§4 alarms. The .ini wins over the header, so read this and never the header."),
		FConsoleCommandDelegate::CreateStatic(&RunDump));

	FAutoConsoleCommand CmdXVulnerableTest(
		TEXT("Trace.X.VulnerableTest"),
		TEXT("Spec v14 §6: '+25% damage from ALL sources', non-stacking, timer reset, expiry. Two arms on one "
		     "binary, red first (Trace.X.Vulnerable 0 marks but does not amplify)."),
		FConsoleCommandDelegate::CreateStatic(&RunVulnerableTest));

	FAutoConsoleCommand CmdXCarrierTest(
		TEXT("Trace.X.CarrierTest"),
		TEXT("Spec v14 §4: X's mark must never become a path to damaging a Core carrier. Three locks, red-armed "
		     "together, with a non-carrier control strike that proves the fixture reaches health."),
		FConsoleCommandDelegate::CreateStatic(&RunCarrierTest));

	FAutoConsoleCommand CmdXStingTest(
		TEXT("Trace.X.StingTest"),
		TEXT("Spec v14 §6: Sting loads five bees, five bullets mark, the sixth does not, a carrier spends none, "
		     "and the delivering bullet is at NORMAL damage."),
		FConsoleCommandDelegate::CreateStatic(&RunStingTest));

	FAutoConsoleCommand CmdXSpeedTest(
		TEXT("Trace.X.SpeedTest"),
		TEXT("Spec v14 §6: '+10% speed while ANY enemy is vulnerable' — including that a marked TEAMMATE does not "
		     "switch it on."),
		FConsoleCommandDelegate::CreateStatic(&RunSpeedTest));

	FAutoConsoleCommand CmdXShowcase(
		TEXT("Trace.X.Showcase"),
		TEXT("Dev only, SERVER. Stages X's two visuals for a screenshot: the orbiting bees, and a marked enemy "
		     "placed in front of him wearing the vulnerable ring."),
		FConsoleCommandDelegate::CreateStatic(&RunShowcase));

	FAutoConsoleCommand CmdXBeeTest(
		TEXT("Trace.X.BeeTest"),
		TEXT("Spec v14 §6: the orbiting bees sting on contact. Red half first — an enemy at 3x the orbit radius "
		     "must NOT be marked."),
		FConsoleCommandDelegate::CreateStatic(&RunBeeTest));
}   // namespace TraceXVerify

#endif // !UE_BUILD_SHIPPING
