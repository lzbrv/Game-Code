// Trace — the evidence for MACE and OYSTER (spec v14 §6), and for §4 as Oyster can break it.
//
// ===================================================================================================
// THREE COMMANDS. ONE OF THEM IS THE ONE THAT MATTERS.
// ===================================================================================================
//
//   Trace.Oyster.CarrierTest   *** SPEC §4, TWO ARMS, RED FIRST, ALL FOUR VECTORS. ***
//                              Oyster is the pass's biggest carrier-rule risk and he has FOUR ways to
//                              touch a player: poison damage, poison slow, Pickler's 30 area damage
//                              and Pickler's pull. This fires all four at a LIVE Core carrier with the
//                              rule REMOVED (every one must land, or the fixture never reached the
//                              carrier and its green means nothing), then again with the shipped rule
//                              (none may land) — and fires the identical four at a NON-CARRIER enemy
//                              in the same frames, which is the fixture proving itself.
//
//   Trace.Oyster.Verify        The passive and the movement ability, end to end: a real dash drops a
//                              jar; a fourth jar despawns the oldest; a real jump off his own jar
//                              breaks it and boosts him; poison deals 3 every 0.5 s.
//
//   Trace.Mace.Verify          The derived magnet radius; the suspend measured against an UNSUSPENDED
//                              fall of the same length (the falsification: a fixture that cannot see
//                              a fall cannot see a suspend); the spike embed; the pull at the momentum
//                              ceiling; cancel-on-input; and that she can still shoot while pulled.
//
// ===================================================================================================
// WHY THE CARRIER TEST IS JUDGED ON TALLIES AND NOT ON HEALTH
// ===================================================================================================
//
// The same reason the framework's own choke test gives, plus one more. A Core carrier ALSO has the
// ordinary damage shield, so "the carrier's health did not move" is true on a build with no carrier
// rule whatsoever — judging on health produces exactly the uninformative green this project has been
// bitten by. And three of Oyster's four vectors are not damage at all: a slow and a pull move no
// health by construction, so health could never have judged them.
//
// So every vector reports at the point the DECISION is taken — the moment the code commits to
// applying the effect — into TraceOyster::CarrierTally() / OtherTally(). A pull that launches a
// stationary bot nowhere still counts, because what is under test is whether the rule let it happen.
// The framework's own alarm (TraceAbility::GetCarrierAbilityDamageHitCount) is read alongside as an
// independent second opinion on the damage half.
//
// TIMING IS REAL TIME (FPlatformTime::Seconds). The character-select screen pauses the world; a
// harness waiting on world time waits forever. These commands also unpause explicitly and SAY SO,
// because a paused world would make every movement measurement in Trace.Mace.Verify a zero that looks
// like a pass.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Abilities/Characters/TraceAbilityInputRelay.h"
#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Abilities/Characters/TraceAbilitySetOyster.h"
#include "Abilities/Characters/TraceMaceSpike.h"
#include "Abilities/Characters/TraceOysterJar.h"
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceMaceOysterVerify
{
	// =============================================================================================
	// Shared fixture helpers
	// =============================================================================================

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

	/**
	 * The select screen pauses the world, and a paused world makes every movement number below a zero
	 * that reads like a pass. Unpause, and REPORT whether it had to.
	 */
	void UnpauseAndReport(UWorld* WorldPtr, const TCHAR* Tag)
	{
		if (WorldPtr == nullptr)
		{
			return;
		}
		if (!WorldPtr->IsPaused())
		{
			return;
		}
		if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
		{
			FirstPC->SetPause(false);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] The world was PAUSED (the character-select screen does that). Unpaused, or every movement "
				     "measurement below would have been a zero that looks like a pass."), Tag);
		}
	}

	/**
	 * The one player who can legally be given a character: a human (spec §3 keeps bots characterless,
	 * and ServerSetCharacter refuses them outright).
	 */
	UTraceAbilityComponent* FindHumanAbilityComponent(UWorld* WorldPtr)
	{
		const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (BaseState == nullptr)
		{
			return nullptr;
		}
		for (APlayerState* Entry : BaseState->PlayerArray)
		{
			if (Entry == nullptr || Entry->IsABot())
			{
				continue;
			}
			if (UTraceAbilityComponent* Comp = Entry->FindComponentByClass<UTraceAbilityComponent>())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/** Living characters on the other team from @p Comp, nearest first is not required — order is stable. */
	void GatherEnemiesOf(UWorld* WorldPtr, const UTraceAbilityComponent* Comp, TArray<ATraceCharacter*>& Out)
	{
		Out.Reset();
		if (WorldPtr == nullptr || Comp == nullptr)
		{
			return;
		}
		const ETraceTeam MyTeam = Comp->GetTeam();
		const ATraceCharacter* MyPawn = Comp->GetOwningCharacter();

		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == MyPawn || !Candidate->IsAlive())
			{
				continue;
			}
			const ETraceTeam TheirTeam = Candidate->GetTeam();
			if (TheirTeam == ETraceTeam::None || MyTeam == ETraceTeam::None || TheirTeam == MyTeam)
			{
				continue;
			}
			Out.Add(Candidate);
		}
	}

	FVector FeetOf(const ATraceCharacter* Who)
	{
		if (Who == nullptr)
		{
			return FVector::ZeroVector;
		}
		float HalfHeight = 0.f;
		if (const UCapsuleComponent* Capsule = Who->GetCapsuleComponent())
		{
			HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		}
		return Who->GetActorLocation() - FVector(0.f, 0.f, HalfHeight);
	}

	IConsoleVariable* AbilityCarrierImmuneCVar()
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Ability.CarrierImmune"));
	}

	void RestoreCarrierArm()
	{
		if (IConsoleVariable* Arm = AbilityCarrierImmuneCVar())
		{
			Arm->Set(1, ECVF_SetByConsole);
		}
	}

	FString DescribeTally(const TraceOyster::FEffectTally& Tally)
	{
		return FString::Printf(TEXT("poisonApplied=%d poisonTicks=%d slowFrames=%d picklerDmg=%d picklerPulls=%d"),
			Tally.PoisonApplications, Tally.PoisonTicks, Tally.SlowFrames,
			Tally.PicklerDamageHits, Tally.PicklerPulls);
	}

	// =============================================================================================
	// Trace.Oyster.CarrierTest — SPEC §4, two arms, red first, four vectors
	// =============================================================================================

	struct FOysterCarrierState
	{
		/** 0 = RED (Trace.Ability.CarrierImmune 0). 1 = GREEN (shipped). */
		int32 Arm = 0;

		/** -1 acquire, 0 poison burst, 1 wait for ticks, 2 pickler, 3 measure. */
		int32 Phase = -1;

		double PhaseStartReal = 0.0;
		double AcquireDeadline = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Attacker;
		TWeakObjectPtr<ATraceCharacter> Carrier;
		TWeakObjectPtr<ATraceCharacter> Control;

		int32 AlarmBefore = 0;

		TraceOyster::FEffectTally ArmCarrier;
		TraceOyster::FEffectTally ArmOther;
		int32 ArmAlarmDelta = 0;

		bool bRedRan = false;
		bool bRedReachedCarrier = false;
		bool bRedFixtureLanded = false;
		TraceOyster::FEffectTally RedCarrier;

		bool bGreenRan = false;
		bool bGreenClean = false;
		bool bGreenFixtureLanded = false;
	};

	/** Every one of the four vectors must have landed on SOMEBODY, or that vector was never tested. */
	bool AllFourVectorsLanded(const TraceOyster::FEffectTally& Tally)
	{
		return Tally.PoisonApplications > 0
			&& Tally.PoisonTicks > 0
			&& Tally.SlowFrames > 0
			&& Tally.PicklerDamageHits > 0
			&& Tally.PicklerPulls > 0;
	}

	void RunOysterCarrierTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERCARRIER] no authoritative game world — run this on the server."));
			return;
		}
		if (AbilityCarrierImmuneCVar() == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERCARRIER] Trace.Ability.CarrierImmune is not registered — cannot arm the test."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("OYSTERCARRIER"));

		TSharedPtr<FOysterCarrierState> State = MakeShared<FOysterCarrierState>();
		State->Arm = 0;                                  // RED FIRST. Always.
		State->PhaseStartReal = FPlatformTime::Seconds();
		State->AcquireDeadline = State->PhaseStartReal + 120.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[OYSTERCARRIER] ===== spec v14 §4 x §6 Oyster. FOUR VECTORS: poison damage, poison slow, Pickler "
			     "area damage, Pickler pull. Arm 0 = RED (rule REMOVED — all four MUST reach the carrier). Arm 1 = "
			     "GREEN (shipped — none may). Both arms fire the identical four at a NON-CARRIER enemy, which is the "
			     "fixture proving itself. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERCARRIER] ABORTED: the world went away mid-test."));
				RestoreCarrierArm();
				return false;
			}

			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase -1: stage --------------------------------------------------------------------
			if (State->Phase < 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Oyster)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Oyster);
				}

				UTraceAbilitySetOyster* OysterSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;

				TArray<ATraceCharacter*> Enemies;
				GatherEnemiesOf(TickWorld, Human, Enemies);

				if (OysterSet == nullptr || Enemies.Num() < 2 || CoreActor == nullptr)
				{
					if (NowReal > State->AcquireDeadline)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[OYSTERCARRIER] VERDICT: INVALID — could not stage inside the acquire window "
							     "(oysterSet=%d livingEnemies=%d core=%d). Needs a mode-B match with characters on and "
							     "at least two living enemies of the human player."),
							(OysterSet != nullptr) ? 1 : 0, Enemies.Num(), (CoreActor != nullptr) ? 1 : 0);
						RestoreCarrierArm();
						return false;
					}
					return true;
				}

				// Force the Core onto the FIRST enemy: the case under test needs a carrier who is an
				// enemy of Oyster, or the team rule would refuse everything and the red arm would not
				// reproduce for the wrong reason.
				CoreActor->TryPickup(Enemies[0]);
				if (CoreActor->Carrier != Enemies[0])
				{
					if (NowReal > State->AcquireDeadline)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[OYSTERCARRIER] VERDICT: INVALID — could not put the Core on an enemy of the "
							     "Oyster player, so there is no carrier to test against."));
						RestoreCarrierArm();
						return false;
					}
					return true;
				}

				State->Attacker = Human;
				State->Carrier = Enemies[0];
				State->Control = Enemies[1];
				State->Phase = 0;
				State->PhaseStartReal = NowReal;

				// THE ARM IS APPLIED HERE, not by the caller, so there is no way to run this command
				// that skips the red half.
				AbilityCarrierImmuneCVar()->Set(State->Arm, ECVF_SetByConsole);

				TraceOyster::ResetTallies();
				State->AlarmBefore = TraceAbility::GetCarrierAbilityDamageHitCount();

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERCARRIER] --- arm=%d (%s) | Oyster %s (team %d) | CARRIER %s (team %d, health %.0f) "
					     "| CONTROL %s (team %d, health %.0f)"),
					State->Arm, (State->Arm == 0) ? TEXT("RED, rule removed") : TEXT("GREEN, shipped rule"),
					*GetNameSafe(Human->GetOwningCharacter()), static_cast<int32>(Human->GetTeam()),
					*GetNameSafe(Enemies[0]), static_cast<int32>(Enemies[0]->GetTeam()),
					(Enemies[0]->Health != nullptr) ? Enemies[0]->Health->Health : -1.f,
					*GetNameSafe(Enemies[1]), static_cast<int32>(Enemies[1]->GetTeam()),
					(Enemies[1]->Health != nullptr) ? Enemies[1]->Health->Health : -1.f);
				return true;
			}

			UTraceAbilityComponent* Attacker = State->Attacker.Get();
			ATraceCharacter* Carrier = State->Carrier.Get();
			ATraceCharacter* Control = State->Control.Get();
			UTraceAbilitySetOyster* OysterSet = (Attacker != nullptr)
				? Attacker->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;

			if (Attacker == nullptr || Carrier == nullptr || Control == nullptr || OysterSet == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERCARRIER] ABORTED: a participant went away mid-test."));
				RestoreCarrierArm();
				return false;
			}

			// PRECONDITION, held every tick. Bots pass and score; a victim who stopped being the
			// carrier between staging and the strike would turn the case under test into an ordinary
			// damage test that passes for the wrong reason.
			if (CoreActor != nullptr && CoreActor->Carrier != Carrier)
			{
				CoreActor->TryPickup(Carrier);
			}

			// ---- Phase 0: the poison burst, at BOTH targets' feet ------------------------------------
			if (State->Phase == 0)
			{
				if ((NowReal - State->PhaseStartReal) < 0.20)
				{
					return true;
				}

				// The SHIPPING path: a real jar, broken the way an enemy's touch breaks it.
				if (ATraceOysterJar* CarrierJar = OysterSet->DebugSpawnJarAt(FeetOf(Carrier), /*bPickler*/ false))
				{
					CarrierJar->ServerBreakNow(TEXT("harness: poison burst at the carrier"));
				}
				if (ATraceOysterJar* ControlJar = OysterSet->DebugSpawnJarAt(FeetOf(Control), /*bPickler*/ false))
				{
					ControlJar->ServerBreakNow(TEXT("harness: poison burst at the control"));
				}

				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 1: let the poison tick and the slow accumulate --------------------------------
			if (State->Phase == 1)
			{
				// Long enough for at least two 0.5 s ticks and a hundred-odd slow frames.
				if ((NowReal - State->PhaseStartReal) < 1.40)
				{
					return true;
				}
				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 2: Pickler, landing on BOTH targets -------------------------------------------
			if (State->Phase == 2)
			{
				// A Pickler jar placed on the ground lands immediately, which fires exactly the
				// landing effect the lob would have — same function, same order.
				OysterSet->DebugSpawnJarAt(FeetOf(Carrier), /*bPickler*/ true);
				OysterSet->DebugSpawnJarAt(FeetOf(Control), /*bPickler*/ true);

				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: measure ---------------------------------------------------------------------
			if ((NowReal - State->PhaseStartReal) < 0.25)
			{
				return true;
			}

			State->ArmCarrier = TraceOyster::CarrierTally();
			State->ArmOther = TraceOyster::OtherTally();
			State->ArmAlarmDelta = TraceAbility::GetCarrierAbilityDamageHitCount() - State->AlarmBefore;

			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERCARRIER] arm=%d CARRIER  : %s | frameworkDamageAlarmDelta=%d"),
				State->Arm, *DescribeTally(State->ArmCarrier), State->ArmAlarmDelta);
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERCARRIER] arm=%d NONCARRIER: %s  <- the fixture proving itself"),
				State->Arm, *DescribeTally(State->ArmOther));

			// Clean up the jars this arm left lying around before the next one.
			for (TActorIterator<ATraceOysterJar> It(TickWorld); It; ++It)
			{
				if (*It != nullptr)
				{
					(*It)->Destroy();
				}
			}

			if (State->Arm == 0)
			{
				State->bRedRan = true;
				State->RedCarrier = State->ArmCarrier;
				State->bRedReachedCarrier = AllFourVectorsLanded(State->ArmCarrier);
				State->bRedFixtureLanded = AllFourVectorsLanded(State->ArmOther);

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERCARRIER] arm=0 (RED) %s — with the rule removed, all four vectors %s reach the carrier."),
					State->bRedReachedCarrier ? TEXT("REPRODUCED") : TEXT("*** DID NOT REPRODUCE ***"),
					State->bRedReachedCarrier ? TEXT("DID") : TEXT("DID NOT"));

				if (Carrier->Health != nullptr) { Carrier->Health->ResetHealth(); }
				if (Control->Health != nullptr) { Control->Health->ResetHealth(); }

				State->Arm = 1;
				State->Phase = -1;
				State->PhaseStartReal = NowReal;
				State->AcquireDeadline = NowReal + 120.0;
				return true;
			}

			State->bGreenRan = true;
			State->bGreenFixtureLanded = AllFourVectorsLanded(State->ArmOther);
			State->bGreenClean = (State->ArmCarrier.Total() == 0) && (State->ArmAlarmDelta == 0);

			RestoreCarrierArm();

			// ---- The verdict. THE FIXTURE IS JUDGED BEFORE THE RULE IS. --------------------------------
			const bool bFixtureValid = State->bRedRan
				&& State->bRedReachedCarrier
				&& State->bRedFixtureLanded
				&& State->bGreenFixtureLanded;

			if (!bFixtureValid)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[OYSTERCARRIER] VERDICT: INVALID — redReachedCarrier=%d redFixtureLanded=%d "
					     "greenFixtureLanded=%d. Either the red arm never got all four vectors onto the carrier (so a "
					     "green arm 1 says nothing about the rule) or a vector never landed on ANYBODY (so that vector "
					     "was never tested at all). Red carrier tally: %s"),
					State->bRedReachedCarrier ? 1 : 0, State->bRedFixtureLanded ? 1 : 0,
					State->bGreenFixtureLanded ? 1 : 0, *DescribeTally(State->RedCarrier));
			}
			else if (State->bGreenClean)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERCARRIER] VERDICT: PASS — arm 0 (rule removed) put ALL FOUR vectors onto the Core "
					     "carrier (%s); arm 1 (shipped rule) put ZERO of them there and tripped the framework's damage "
					     "alarm 0 times, while the IDENTICAL four still landed on a non-carrier enemy in the same "
					     "frames (%s). Spec v14 §4 holds for every one of Oyster's effects, and the harness is proven "
					     "able to fail."),
					*DescribeTally(State->RedCarrier), *DescribeTally(State->ArmOther));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[OYSTERCARRIER] VERDICT: *** FAIL *** — the SHIPPED rule let Oyster reach the Core carrier: "
					     "%s (framework damage alarm +%d). Spec v14 §4 is broken."),
					*DescribeTally(State->ArmCarrier), State->ArmAlarmDelta);
			}

			return false;
		}));
	}

	FAutoConsoleCommand CmdOysterCarrierTest(
		TEXT("Trace.Oyster.CarrierTest"),
		TEXT("Dev only, server only. SPEC v14 §4 x §6. Fires ALL FOUR of Oyster's effects (poison damage, poison "
		     "slow, Pickler area damage, Pickler pull) at a live Core carrier with the rule REMOVED (must all land) "
		     "and then with the shipped rule (none may). Reports INVALID unless the red arm reproduced."),
		FConsoleCommandDelegate::CreateStatic(&RunOysterCarrierTest));

	// =============================================================================================
	// Trace.Oyster.Verify — the passive and the movement ability, end to end
	// =============================================================================================

	struct FOysterVerifyState
	{
		int32 Phase = 0;
		double PhaseStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;
		TWeakObjectPtr<ATraceCharacter> Victim;

		int32 JarsBeforeDash = 0;
		int32 JarsAfterDash = 0;
		bool  bDashActuallyHappened = false;

		int32 JarsAfterFour = 0;
		bool  bOldestDespawned = false;

		float ZVelocityBeforeJarJump = 0.f;
		float ZVelocityAfterJarJump = 0.f;
		int32 JarsBeforeJarJump = 0;
		int32 JarsAfterJarJump = 0;

		float PoisonDamageAt2s = -1.f;

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; }
			else            { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERVERIFY]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	void RunOysterVerify()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERVERIFY] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("OYSTERVERIFY"));

		TSharedPtr<FOysterVerifyState> State = MakeShared<FOysterVerifyState>();
		State->PhaseStartReal = FPlatformTime::Seconds();

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[OYSTERVERIFY] ===== spec v14 §6 Oyster. Knobs AS THE RUNNING GAME HAS THEM: jars %.1fs x%d, "
			     "break %.0f uu | poison %.0f every %.2fs for %.1fs, -%.0f%% speed, burst %.0f uu | jar-jump %.0f uu/s "
			     "within %.0f uu | Pickler %.0f dmg / %.0f uu, pull %.0f uu/s / %.0f uu, cooldown %.0fs ====="),
			Settings.OysterJarLifetimeSeconds, Settings.OysterMaxJars, Settings.OysterJarBreakRadiusUU,
			Settings.OysterPoisonDamagePerTick, Settings.OysterPoisonTickIntervalSeconds,
			Settings.OysterPoisonDurationSeconds, Settings.OysterPoisonSlowFraction * 100.f,
			Settings.OysterPoisonRadiusUU, Settings.OysterJarJumpZVelocity, Settings.OysterJarJumpRadiusUU,
			Settings.OysterPicklerDamage, Settings.OysterPicklerDamageRadiusUU,
			Settings.OysterPicklerPullSpeed, Settings.OysterPicklerPullRadiusUU,
			Settings.OysterPicklerCooldownSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERVERIFY] ABORTED: the world went away."));
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: become Oyster ---------------------------------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Oyster)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Oyster);
				}
				UTraceAbilitySetOyster* OysterSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;

				if (OysterSet == nullptr || Human->GetOwningCharacter() == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[OYSTERVERIFY] VERDICT: INVALID — no human player could be made Oyster (set=%d pawn=%d). "
							     "Needs a mode-B match with characters enabled."),
							(OysterSet != nullptr) ? 1 : 0,
							(Human != nullptr && Human->GetOwningCharacter() != nullptr) ? 1 : 0);
						return false;
					}
					return true;
				}

				State->Subject = Human;
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			UTraceAbilityComponent* Comp = State->Subject.Get();
			UTraceAbilitySetOyster* OysterSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetOyster>() : nullptr;
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (OysterSet == nullptr || MyPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[OYSTERVERIFY] ABORTED: Oyster went away mid-test."));
				return false;
			}

			// ---- Phase 1: A REAL DASH must drop a jar ---------------------------------------------
			if (State->Phase == 1)
			{
				State->JarsBeforeDash = OysterSet->GetLiveJarCount();
				MyPawn->DoDash();                         // the shipping input entry point
				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}
			if (State->Phase == 2)
			{
				if (MyPawn->IsDashing())
				{
					State->bDashActuallyHappened = true;
				}
				if ((NowReal - State->PhaseStartReal) < 0.30)
				{
					return true;
				}
				State->JarsAfterDash = OysterSet->GetLiveJarCount();

				UE_LOG(LogTraceGame, Display, TEXT("[OYSTERVERIFY] --- PASSIVE: a jar at the start of every dash"));
				State->Check(State->bDashActuallyHappened,
					TEXT("the fixture actually produced a dash (IsDashing() was observed true) — without this the next line proves nothing"));
				State->Check(State->JarsAfterDash > State->JarsBeforeDash,
					FString::Printf(TEXT("the dash dropped a jar (%d -> %d live jars)"), State->JarsBeforeDash, State->JarsAfterDash));

				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: the cap ------------------------------------------------------------------
			if (State->Phase == 3)
			{
				const int32 MaxJars = FMath::Max(1, UTraceSettings::Get().OysterMaxJars);

				// Spawned FAR from everybody so nothing touches and breaks them mid-count.
				const FVector Far = MyPawn->GetActorLocation() + FVector(0.f, 0.f, 4000.f);
				TWeakObjectPtr<ATraceOysterJar> First;
				for (int32 Index = 0; Index < MaxJars + 1; ++Index)
				{
					ATraceOysterJar* JarActor = OysterSet->DebugSpawnJarAt(Far + FVector(Index * 300.f, 0.f, 0.f), false);
					if (Index == 0)
					{
						First = JarActor;
					}
				}

				State->JarsAfterFour = OysterSet->GetLiveJarCount();
				State->bOldestDespawned = !First.IsValid();

				UE_LOG(LogTraceGame, Display, TEXT("[OYSTERVERIFY] --- PASSIVE: \"max 3; a fourth despawns the oldest\""));
				State->Check(State->JarsAfterFour == MaxJars,
					FString::Printf(TEXT("%d jars thrown left exactly %d alive (got %d)"), MaxJars + 1, MaxJars, State->JarsAfterFour));
				State->Check(State->bOldestDespawned,
					TEXT("the one that went was the OLDEST (the first jar spawned is gone)"));

				State->Phase = 4;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 4: the jar jump -------------------------------------------------------------
			if (State->Phase == 4)
			{
				UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement();
				if (MoveComp == nullptr)
				{
					State->Check(false, TEXT("Oyster has a movement component"));
					State->Phase = 6;
					return true;
				}

				// Put him on the ground with a jar under him, then use the SHIPPING jump entry point
				// so the ground-state poll is what fires — that is the code path that runs in a match.
				if (!MoveComp->IsMovingOnGround())
				{
					if ((NowReal - State->PhaseStartReal) > 4.0)
					{
						State->Check(false, TEXT("Oyster reached the ground so a jar jump could be attempted"));
						State->Phase = 6;
						State->PhaseStartReal = NowReal;
					}
					return true;
				}

				OysterSet->DebugSpawnJarAt(FeetOf(MyPawn), /*bPickler*/ false);
				State->JarsBeforeJarJump = OysterSet->GetLiveJarCount();
				State->ZVelocityBeforeJarJump = MoveComp->Velocity.Z;
				MyPawn->Jump();

				State->Phase = 5;
				State->PhaseStartReal = NowReal;
				return true;
			}
			if (State->Phase == 5)
			{
				UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement();
				const float BoostZ = UTraceSettings::Get().OysterJarJumpZVelocity;

				if (MoveComp != nullptr)
				{
					State->ZVelocityAfterJarJump = FMath::Max(State->ZVelocityAfterJarJump, MoveComp->Velocity.Z);
				}
				if ((NowReal - State->PhaseStartReal) < 0.25)
				{
					return true;
				}
				State->JarsAfterJarJump = OysterSet->GetLiveJarCount();

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERVERIFY] --- MOVEMENT: \"jumping while stood on one of his jars breaks it and boosts him upward\""));
				State->Check(State->ZVelocityAfterJarJump >= BoostZ - 1.f,
					FString::Printf(TEXT("the jump reached the jar-jump launch speed (peak Z %.0f uu/s vs the %.0f uu/s knob; a plain jump is %.0f)"),
						State->ZVelocityAfterJarJump, BoostZ, (MoveComp != nullptr) ? MoveComp->JumpZVelocity : -1.f));
				State->Check(State->JarsAfterJarJump < State->JarsBeforeJarJump,
					FString::Printf(TEXT("the jar was BROKEN by the jump (%d -> %d live jars)"),
						State->JarsBeforeJarJump, State->JarsAfterJarJump));
				State->Check((MoveComp != nullptr) && BoostZ > MoveComp->JumpZVelocity,
					TEXT("the jar-jump launch is actually higher than a normal jump — otherwise the measurement above cannot tell them apart"));

				State->Phase = 6;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 6: poison arithmetic --------------------------------------------------------
			if (State->Phase == 6)
			{
				TArray<ATraceCharacter*> Enemies;
				GatherEnemiesOf(TickWorld, Comp, Enemies);
				if (Enemies.Num() == 0)
				{
					State->Check(false, TEXT("a living enemy exists to poison — without one the poison numbers are untested"));
					State->Phase = 8;
					return true;
				}

				State->Victim = Enemies[0];
				if (Enemies[0]->Health != nullptr)
				{
					Enemies[0]->Health->ResetHealth();
				}
				if (ATraceOysterJar* JarActor = OysterSet->DebugSpawnJarAt(FeetOf(Enemies[0]), false))
				{
					JarActor->ServerBreakNow(TEXT("harness: poison arithmetic"));
				}

				State->Phase = 7;
				State->PhaseStartReal = NowReal;
				return true;
			}
			if (State->Phase == 7)
			{
				// Sampled at 2.2 s: four 0.5 s ticks have landed and the 4 s window has not closed, so
				// the component still exists to be read.
				if ((NowReal - State->PhaseStartReal) < 2.20)
				{
					return true;
				}

				ATraceCharacter* Victim = State->Victim.Get();
				const UTraceOysterPoisonComponent* Poison = UTraceOysterPoisonComponent::Find(Victim);
				const float PerTick = UTraceSettings::Get().OysterPoisonDamagePerTick;
				const float Interval = UTraceSettings::Get().OysterPoisonTickIntervalSeconds;
				const float Expected = PerTick * FMath::FloorToFloat(2.20f / FMath::Max(0.01f, Interval));

				State->PoisonDamageAt2s = (Poison != nullptr) ? Poison->GetDamageDealtSoFar() : -1.f;

				UE_LOG(LogTraceGame, Display,
					TEXT("[OYSTERVERIFY] --- PASSIVE: \"3 damage every 0.5 s for 4 s, and -30%% speed for 4 s\""));
				State->Check(Poison != nullptr,
					TEXT("the poison component is still live on the victim 2.2 s in (the 4 s window has not closed early)"));
				State->Check(State->PoisonDamageAt2s >= Expected - PerTick - 0.01f && State->PoisonDamageAt2s <= Expected + PerTick + 0.01f,
					FString::Printf(TEXT("poison had dealt %.1f by 2.2 s; %.1f expected (%.0f x %d ticks), +/- one tick of scheduling"),
						State->PoisonDamageAt2s, Expected, PerTick, FMath::FloorToInt(2.20f / FMath::Max(0.01f, Interval))));
				// The victim is a live bot in a live match and may have picked the Core up mid-poison.
				// That is not a failure — it is the choke point doing its job — but it MUST be said out
				// loud, or a correct refusal would be reported as a broken slow.
				const bool bVictimIsCarrier = UTraceAbilityComponent::IsCarrier(Victim);
				if (bVictimIsCarrier)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[OYSTERVERIFY]   NOTE: the poison victim picked up the Core during the measurement, so the "
						     "slow is correctly OFF and the damage total above is short. Re-run for a clean number; "
						     "Trace.Oyster.CarrierTest is where that transition is deliberately tested."));
				}
				State->Check(Poison != nullptr && (Poison->IsSlowActive() || bVictimIsCarrier),
					TEXT("the -30% slow is in force on the victim (the choke point allows Control on a non-carrier)"));

				State->Phase = 8;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Verdict ----------------------------------------------------------------------------
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERVERIFY] ===== %d passed, %d failed. ====="),
				State->Passed, State->Failed);
			UE_LOG(LogTraceGame, Display, TEXT("[OYSTERVERIFY] VERDICT: %s"),
				(State->Failed == 0 && State->Passed > 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
			return false;
		}));
	}

	FAutoConsoleCommand CmdOysterVerify(
		TEXT("Trace.Oyster.Verify"),
		TEXT("Dev only, server only. SPEC v14 §6 Oyster: a real dash drops a jar, a fourth jar despawns the oldest, "
		     "a real jump off his own jar breaks it and boosts him, and poison deals 3 every 0.5 s."),
		FConsoleCommandDelegate::CreateStatic(&RunOysterVerify));

	// =============================================================================================
	// Trace.Mace.Verify — the suspend, the spike and the pull
	// =============================================================================================

	struct FMaceVerifyState
	{
		int32 Phase = 0;
		double PhaseStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;

		/** THE FALSIFICATION: an identical drop with NO suspend. If this does not fall, nothing below means anything. */
		float ControlDropStartZ = 0.f;
		float ControlDropFallen = -1.f;

		float SuspendStartZ = 0.f;
		float SuspendDrift = -1.f;
		float SuspendObservedSeconds = 0.f;
		bool  bSuspendEndedByItself = false;

		bool  bSpikeEmbedded = false;
		float PullPeakSpeed = 0.f;
		float PullStartDistance = 0.f;
		float PullEndDistance = -1.f;
		bool  bWeaponBlockedDuringPull = false;
		bool  bPullEndedAtArrival = false;
		bool  bSpikeGoneAfterPull = false;

		bool  bCancelPullFired = false;
		bool  bCancelledByInput = false;
		/** Seconds from the first movement input to the pull stopping. Discriminates input from a wall. */
		double CancelLatencySeconds = -1.0;

		FString AimSweepReport;

		/**
		 * THE WALL HUNT. Spec v14 §1 spent a whole pass on a wall-clip harness whose fixture could not
		 * reach a wall, and its green meant nothing. Mace's throw is "on hitting a WALL it embeds", so a
		 * run that never found a wall has not tested the throw at all — it has tested the pull from a
		 * hand-placed anchor and nothing else. These fields exist so the run says which it did.
		 */
		FVector HomeLocation = FVector::ZeroVector;
		int32   SearchStep = 0;
		bool    bWallFound = false;
		float   FoundWallDistance = -1.f;
		bool    bRealThrowFired = false;
		bool    bRealSpikeEmbedded = false;
		double  RealThrowStartReal = 0.0;
		float   RealThrowFlightSeconds = -1.f;
		bool    bSkyThrowFizzled = false;
		bool    bSkyThrowLeftNoSpike = false;

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; }
			else            { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[MACEVERIFY]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	/** Drops the pawn into a clean fall from @p Height above where it is. */
	void StageFall(ATraceCharacter* MyPawn, float Height)
	{
		if (MyPawn == nullptr)
		{
			return;
		}
		UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement();
		MyPawn->SetActorLocation(MyPawn->GetActorLocation() + FVector(0.f, 0.f, Height), /*bSweep*/ false);
		if (MoveComp != nullptr)
		{
			MoveComp->SetMovementMode(MOVE_Falling);
			MoveComp->Velocity = FVector::ZeroVector;
		}
	}

	void RunMaceVerify()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[MACEVERIFY] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("MACEVERIFY"));

		TSharedPtr<FMaceVerifyState> State = MakeShared<FMaceVerifyState>();
		State->PhaseStartReal = FPlatformTime::Seconds();

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[MACEVERIFY] ===== spec v14 §6 Mace. Knobs AS THE RUNNING GAME HAS THEM: magnet +%.0f%% of "
			     "CoreCatchRadius %.0f = %.0f uu (§6 says 585) | suspend %.2fs @ %.0f uu/s lateral | spike %.0f uu, "
			     "travel %.0f uu/s, embed %.1fs, arrive %.0f uu, cooldown %.0fs | pull = AirStrafeHardCapSpeed %.0f x "
			     "AirStrafeAsymptoteScale %.2f x %.2f ====="),
			Settings.MaceMagnetRadiusBonus * 100.f, Settings.CoreCatchRadius,
			Settings.CoreCatchRadius * (1.f + Settings.MaceMagnetRadiusBonus),
			Settings.MaceSuspendMaxSeconds, Settings.MaceSuspendLateralSpeedCap,
			Settings.MaceSpikeRangeUU, Settings.MaceSpikeTravelSpeed, Settings.MaceSpikeEmbedSeconds,
			Settings.MaceSpikeArriveRadiusUU, Settings.MaceSpikeCooldownSeconds,
			Settings.AirStrafeHardCapSpeed, Settings.AirStrafeAsymptoteScale, Settings.MaceSpikePullSpeedMultiplier);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[MACEVERIFY] ABORTED: the world went away."));
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: become Mace ---------------------------------------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Mace)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Mace);
				}
				UTraceAbilitySetMace* MaceSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetMace>() : nullptr;

				if (MaceSet == nullptr || Human->GetOwningCharacter() == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[MACEVERIFY] VERDICT: INVALID — no human player could be made Mace. Needs a mode-B "
							     "match with characters enabled."));
						return false;
					}
					return true;
				}

				State->Subject = Human;
				// Captured BEFORE anything teleports her. The suspend tests drop her from +1200 twice,
				// which puts her a clear kilometre above a 1640 uu arena — and a pawn outside the map
				// can never see a wall, which is exactly how the first run of this harness reported
				// "found no wall" and would have shipped an untested throw.
				State->HomeLocation = Human->GetOwningCharacter()->GetActorLocation();

				UE_LOG(LogTraceGame, Display, TEXT("[MACEVERIFY] --- PASSIVE: the derived magnet radius"));
				const float Derived = UTraceSettings::Get().CoreCatchRadius * MaceSet->GetMagnetRadiusMultiplier();
				State->Check(FMath::IsNearlyEqual(Derived, 585.f, 1.f),
					FString::Printf(TEXT("CoreCatchRadius %.0f x Mace's %.3f = %.1f uu, and §6 says 585 uu (DERIVED — 585 is written down nowhere)"),
						UTraceSettings::Get().CoreCatchRadius, MaceSet->GetMagnetRadiusMultiplier(), Derived));

				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			UTraceAbilityComponent* Comp = State->Subject.Get();
			UTraceAbilitySetMace* MaceSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetMace>() : nullptr;
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			UTraceCharacterMovementComponent* MoveComp = (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;
			if (MaceSet == nullptr || MyPawn == nullptr || MoveComp == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[MACEVERIFY] ABORTED: Mace went away mid-test."));
				return false;
			}

			// ---- Phase 1/2: THE CONTROL FALL. No suspend. It MUST fall, or nothing below is a test. --
			if (State->Phase == 1)
			{
				StageFall(MyPawn, 1200.f);
				State->ControlDropStartZ = MyPawn->GetActorLocation().Z;
				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}
			if (State->Phase == 2)
			{
				if ((NowReal - State->PhaseStartReal) < 1.00)
				{
					return true;
				}
				State->ControlDropFallen = State->ControlDropStartZ - MyPawn->GetActorLocation().Z;

				UE_LOG(LogTraceGame, Display,
					TEXT("[MACEVERIFY] --- MOVEMENT: \"hold V in the air to suspend for up to 1.25 s — gravity does not affect her\""));
				State->Check(State->ControlDropFallen > 200.f,
					FString::Printf(TEXT("THE FALSIFICATION: an identical 1.0 s with NO suspend fell %.0f uu, so the fixture can see gravity"),
						State->ControlDropFallen));

				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3/4: the suspend ----------------------------------------------------------------
			if (State->Phase == 3)
			{
				StageFall(MyPawn, 1200.f);
				State->SuspendStartZ = MyPawn->GetActorLocation().Z;
				MaceSet->OnSecondaryPressed();      // the shipping entry point the V relay calls
				State->Phase = 4;
				State->PhaseStartReal = NowReal;
				return true;
			}
			if (State->Phase == 4)
			{
				if (MaceSet->IsSuspending())
				{
					State->SuspendObservedSeconds = static_cast<float>(NowReal - State->PhaseStartReal);
					State->SuspendDrift = State->SuspendStartZ - MyPawn->GetActorLocation().Z;
				}
				else if (State->SuspendObservedSeconds > 0.1f)
				{
					State->bSuspendEndedByItself = true;
				}

				if ((NowReal - State->PhaseStartReal) < 1.60)
				{
					return true;
				}

				const float MaxSeconds = UTraceSettings::Get().MaceSuspendMaxSeconds;
				State->Check(State->SuspendDrift >= 0.f && State->SuspendDrift < State->ControlDropFallen * 0.15f,
					FString::Printf(TEXT("she held her height: %.1f uu of drift over the whole suspend, against %.0f uu of ordinary fall in 1.0 s"),
						State->SuspendDrift, State->ControlDropFallen));
				State->Check(State->bSuspendEndedByItself,
					FString::Printf(TEXT("the suspend ended ON ITS OWN after ~%.2fs (the knob says %.2fs) rather than running forever"),
						State->SuspendObservedSeconds, MaxSeconds));
				State->Check(FMath::IsNearlyEqual(State->SuspendObservedSeconds, MaxSeconds, 0.25f),
					FString::Printf(TEXT("the window it held for was the %.2fs the knob asks for (observed %.2fs)"),
						MaxSeconds, State->SuspendObservedSeconds));

				MaceSet->OnSecondaryReleased();
				State->Phase = 5;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 5: THE WALL HUNT, then a REAL throw ----------------------------------------------
			//
			// This is the half of Spike that DebugThrowSpikeAt cannot test: the aim sweep, the wall
			// test, the flight, and the embed. It has to find a real wall to do it, and the run says
			// out loud whether it did — a "found no wall" here means the throw was never exercised and
			// the pull result below is all this run proves.
			if (State->Phase == 5)
			{
				if (State->SearchStep == 0)
				{
					// Back inside the map first. See HomeLocation.
					MyPawn->SetActorLocation(State->HomeLocation, /*bSweep*/ false);
					MoveComp->Velocity = FVector::ZeroVector;
				}

				AController* Steer = MyPawn->GetController();
				if (Steer == nullptr)
				{
					State->Check(false, TEXT("Mace has a controller to aim with"));
					State->Phase = 6;
					State->PhaseStartReal = NowReal;
					return true;
				}

				// 24 yaws x 3 pitches. One candidate per tick so the aim has genuinely settled before
				// the sweep reads it, rather than 72 sweeps against one stale rotation.
				constexpr int32 YawSteps = 24;
				constexpr int32 PitchCount = 3;
				const float Pitches[PitchCount] = { 0.f, -12.f, 12.f };

				if (State->SearchStep < YawSteps * PitchCount)
				{
					const int32 PitchIndex = State->SearchStep / YawSteps;
					const int32 YawIndex = State->SearchStep % YawSteps;
					Steer->SetControlRotation(FRotator(Pitches[PitchIndex], YawIndex * (360.f / YawSteps), 0.f));
					++State->SearchStep;

					FVector Anchor, Normal;
					FString Why;
					if (MaceSet->ResolveSpikeAnchor(Anchor, Normal, Why))
					{
						State->bWallFound = true;
						State->FoundWallDistance = FVector::Dist(MyPawn->GetActorLocation(), Anchor);
						State->AimSweepReport = FString::Printf(
							TEXT("found a wall at %.0f uu on yaw %.0f pitch %.0f (|normal.Z| %.2f)"),
							State->FoundWallDistance, YawIndex * (360.f / YawSteps), Pitches[PitchIndex],
							FMath::Abs(Normal.Z));

						// THE REAL THROW, through the shipping ActivateAbility() — same sweep, same
						// wall test, same actor, same flight.
						State->bRealThrowFired = MaceSet->ActivateAbility();
						State->RealThrowStartReal = NowReal;
						State->Phase = 50;
						State->PhaseStartReal = NowReal;
					}
					return true;
				}

				State->AimSweepReport = TEXT("found NO wall on any of 72 aim directions");
				UE_LOG(LogTraceGame, Error,
					TEXT("[MACEVERIFY] --- ACTIVATED: %s. The throw path (aim sweep -> wall test -> flight -> embed) "
					     "was therefore NEVER EXERCISED by this run. Everything below tests the pull from a "
					     "hand-placed anchor only."), *State->AimSweepReport);
				State->Check(false, TEXT("the fixture CAN reach a wall to throw the spike at"));
				State->Phase = 6;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 50: the thrown spike must fly and embed, then the sky throw must fizzle --------
			if (State->Phase == 50)
			{
				const ATraceMaceSpike* Flying = MaceSet->GetSpike();
				if (Flying != nullptr && Flying->IsEmbedded() && !State->bRealSpikeEmbedded)
				{
					State->bRealSpikeEmbedded = true;
					State->RealThrowFlightSeconds = static_cast<float>(NowReal - State->RealThrowStartReal);
				}

				if ((NowReal - State->PhaseStartReal) < 1.00)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[MACEVERIFY] --- ACTIVATED: \"throws a roped spike in her aim direction... on hitting a WALL it embeds\""));
				UE_LOG(LogTraceGame, Display, TEXT("[MACEVERIFY]   the aim sweep %s"), *State->AimSweepReport);
				State->Check(State->bWallFound, TEXT("THE FIXTURE REACHED A WALL — without this the throw is untested"));
				State->Check(State->bRealThrowFired,
					TEXT("the shipping ActivateAbility() reported a throw (so it was NOT charged as a fizzle)"));
				State->Check(State->bRealSpikeEmbedded,
					FString::Printf(TEXT("the thrown spike flew to the wall and reported itself EMBEDDED after %.0f ms (%.0f uu at %.0f uu/s)"),
						State->RealThrowFlightSeconds * 1000.f, State->FoundWallDistance,
						UTraceSettings::Get().MaceSpikeTravelSpeed));

				// THE FIZZLE, which is the other half of the wall rule: aimed at the sky there is no
				// wall, so ActivateAbility must return FALSE and the framework must not charge 20 s.
				if (AController* Steer = MyPawn->GetController())
				{
					Steer->SetControlRotation(FRotator(-89.f, 0.f, 0.f));
				}
				const ATraceMaceSpike* SpikeBeforeSkyThrow = MaceSet->GetSpike();
				State->bSkyThrowFizzled = !MaceSet->ActivateAbility();
				// A fizzle must change NOTHING — it must not spawn a spike and, just as importantly, it
				// must not destroy the one already in the wall. "Is it null" would have been the wrong
				// question and would have passed on a build where a fizzle wiped a live spike.
				State->bSkyThrowLeftNoSpike = (MaceSet->GetSpike() == SpikeBeforeSkyThrow);
				State->Check(State->bSkyThrowFizzled,
					TEXT("aimed at the sky the throw FIZZLED (returned false), so the framework charges no cooldown for it"));
				State->Check(State->bSkyThrowLeftNoSpike,
					TEXT("and the fizzle spawned no spike and did not disturb the one already embedded"));

				State->Phase = 6;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 6/7: the pull, from an explicit anchor -------------------------------------------
			if (State->Phase == 6)
			{
				StageFall(MyPawn, 900.f);

				// An anchor 1000 uu out and level, so the pull is measurable and the arrival is
				// unambiguous. The aim sweep is exercised separately above; what is under test here is
				// the pull, and it must not depend on a bot happening to face a wall.
				const FVector Anchor = MyPawn->GetActorLocation() + MyPawn->GetActorForwardVector() * 1000.f;
				ATraceMaceSpike* NewSpike = MaceSet->DebugThrowSpikeAt(Anchor, /*TravelSpeedOverride*/ 0.f);
				State->bSpikeEmbedded = (NewSpike != nullptr) && NewSpike->IsEmbedded();
				State->PullStartDistance = FVector::Dist(MyPawn->GetActorLocation(), Anchor);

				MaceSet->RequestSpikePull();
				State->Phase = 7;
				State->PhaseStartReal = NowReal;
				return true;
			}
			if (State->Phase == 7)
			{
				if (MaceSet->IsPulling())
				{
					State->PullPeakSpeed = FMath::Max(State->PullPeakSpeed, static_cast<float>(MoveComp->Velocity.Size()));
					if (MyPawn->AreWeaponActionsBlocked())
					{
						State->bWeaponBlockedDuringPull = true;
					}
					State->PullEndDistance = (MaceSet->GetSpike() != nullptr)
						? FVector::Dist(MyPawn->GetActorLocation(), MaceSet->GetSpike()->GetAnchorLocation())
						: State->PullEndDistance;
				}
				else if (State->PullPeakSpeed > 0.f)
				{
					State->bPullEndedAtArrival = true;
					State->bSpikeGoneAfterPull = (MaceSet->GetSpike() == nullptr);
				}

				if ((NowReal - State->PhaseStartReal) < 2.00)
				{
					return true;
				}

				const float ExpectedSpeed = MaceSet->GetPullSpeed();
				const float ArriveRadius = UTraceSettings::Get().MaceSpikeArriveRadiusUU;

				UE_LOG(LogTraceGame, Display,
					TEXT("[MACEVERIFY] --- ACTIVATED: \"reactivating pulls her toward it AT THE MOMENTUM CEILING\""));
				State->Check(State->bSpikeEmbedded, TEXT("the spike reported itself embedded before the pull"));
				State->Check(State->PullPeakSpeed >= ExpectedSpeed * 0.95f,
					FString::Printf(TEXT("the pull ran at the momentum ceiling: peak %.0f uu/s against the derived %.0f uu/s (AirStrafeHardCapSpeed x AirStrafeAsymptoteScale x %.2f)"),
						State->PullPeakSpeed, ExpectedSpeed, UTraceSettings::Get().MaceSpikePullSpeedMultiplier));
				State->Check(State->bPullEndedAtArrival,
					FString::Printf(TEXT("the pull ended (started %.0f uu out, last measured %.0f uu, arrive radius %.0f uu)"),
						State->PullStartDistance, State->PullEndDistance, ArriveRadius));
				// "It ended" on its own is weak: the pull ALSO ends when it bounces off geometry, so a
				// wall 50 uu away would pass the line above. This is the one that says she travelled.
				State->Check(State->PullEndDistance >= 0.f && State->PullEndDistance < State->PullStartDistance * 0.35f,
					FString::Printf(TEXT("and she actually covered the ground: %.0f uu -> %.0f uu (>65%% of the way), not a bounce off something next to her"),
						State->PullStartDistance, State->PullEndDistance));
				State->Check(!State->bWeaponBlockedDuringPull,
					TEXT("\"she can shoot while pulled\": AreWeaponActionsBlocked() stayed FALSE for every frame of the pull"));
				State->Check(State->bSpikeGoneAfterPull,
					TEXT("the spike was removed when the pull ended"));

				State->Phase = 8;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 8/9: "any movement input cancels the pull and removes the spike" ------------------
			if (State->Phase == 8)
			{
				StageFall(MyPawn, 900.f);
				const FVector Anchor = MyPawn->GetActorLocation() + MyPawn->GetActorForwardVector() * 3000.f;
				MaceSet->DebugThrowSpikeAt(Anchor, 0.f);
				MaceSet->RequestSpikePull();
				State->bCancelPullFired = MaceSet->IsPulling();

				State->Phase = 9;
				State->PhaseStartReal = NowReal;
				return true;
			}
			if (State->Phase == 9)
			{
				// DoMove is the shipping input entry point; it feeds AddMovementInput, which becomes
				// Acceleration on the next movement update — which is exactly the signal the cancel
				// reads. Driven every tick so the cancel cannot be blamed on a single missed frame.
				MyPawn->DoMove(FVector2D(1.f, 0.f));

				if (State->bCancelPullFired && !MaceSet->IsPulling() && !State->bCancelledByInput)
				{
					State->bCancelledByInput = true;
					State->CancelLatencySeconds = NowReal - State->PhaseStartReal;
				}

				if ((NowReal - State->PhaseStartReal) < 0.60)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[MACEVERIFY] --- ACTIVATED: \"ANY movement input cancels the pull and removes the spike\""));
				State->Check(State->bCancelPullFired,
					TEXT("the second pull actually started (otherwise the cancel below is vacuous)"));
				State->Check(State->bCancelledByInput,
					FString::Printf(TEXT("movement input cancelled the pull, %.0f ms after the first DoMove"),
						State->CancelLatencySeconds * 1000.0));
				// THE DISCRIMINATOR. The pull also cancels when it BOUNCES OFF A WALL, and "it stopped"
				// alone cannot tell the two apart — which would let a wall three metres away pass this
				// as an input cancel. The anchor is 3000 uu out and the pull runs at ~1375 uu/s, so a
				// wall cannot be reached inside 200 ms; input cancels on the second tick.
				State->Check(State->bCancelledByInput && State->CancelLatencySeconds >= 0.0 && State->CancelLatencySeconds < 0.20,
					FString::Printf(TEXT("it cancelled within 200 ms (%.0f ms), which at %.0f uu/s toward an anchor 3000 uu away rules out a wall bounce"),
						State->CancelLatencySeconds * 1000.0, MaceSet->GetPullSpeed()));
				State->Check(MaceSet->GetSpike() == nullptr,
					TEXT("and the spike was removed with it"));

				State->Phase = 10;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Verdict -------------------------------------------------------------------------------
			UE_LOG(LogTraceGame, Display, TEXT("[MACEVERIFY] Aim sweep from the test position: %s"), *State->AimSweepReport);
			UE_LOG(LogTraceGame, Display, TEXT("[MACEVERIFY] ===== %d passed, %d failed. ====="), State->Passed, State->Failed);
			UE_LOG(LogTraceGame, Display, TEXT("[MACEVERIFY] VERDICT: %s"),
				(State->Failed == 0 && State->Passed > 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
			return false;
		}));
	}

	FAutoConsoleCommand CmdMaceVerify(
		TEXT("Trace.Mace.Verify"),
		TEXT("Dev only, server only. SPEC v14 §6 Mace: the derived magnet radius, the suspend measured against an "
		     "UNSUSPENDED fall of the same length, the spike embed, the pull at the momentum ceiling, shooting while "
		     "pulled, and cancel-on-input."),
		FConsoleCommandDelegate::CreateStatic(&RunMaceVerify));

	// =============================================================================================
	// Trace.MaceOyster.DumpState — cheap, synchronous, for eyeballing a live match
	// =============================================================================================

	void RunDumpState()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[MACEOYSTER] no authoritative game world."));
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[MACEOYSTER] input relay: activate=%s secondary=%s (realBindsInstalled=%d)"),
			*UTraceAbilityInputRelay::ResolveActivateKey().ToString(),
			*UTraceAbilityInputRelay::ResolveSecondaryKey().ToString(),
			UTraceAbilityInputRelay::AreRealBindsInstalled() ? 1 : 0);

		const AGameStateBase* BaseState = WorldPtr->GetGameState();
		if (BaseState == nullptr)
		{
			return;
		}

		for (APlayerState* Entry : BaseState->PlayerArray)
		{
			UTraceAbilityComponent* Comp = (Entry != nullptr) ? Entry->FindComponentByClass<UTraceAbilityComponent>() : nullptr;
			if (Comp == nullptr)
			{
				continue;
			}
			if (const UTraceAbilitySetMace* MaceSet = Comp->GetAbilitySetAs<UTraceAbilitySetMace>())
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[MACEOYSTER] %s = MACE | suspending=%d (%.2fs left) | pulling=%d | spike=%s | cooldown %.1fs"),
					*GetNameSafe(Entry), MaceSet->IsSuspending() ? 1 : 0, MaceSet->GetSuspendRemaining(),
					MaceSet->IsPulling() ? 1 : 0,
					(MaceSet->GetSpike() != nullptr)
						? (MaceSet->GetSpike()->IsEmbedded() ? TEXT("embedded") : TEXT("in flight")) : TEXT("none"),
					Comp->GetActivatedCooldownRemaining());
			}
			else if (const UTraceAbilitySetOyster* OysterSet = Comp->GetAbilitySetAs<UTraceAbilitySetOyster>())
			{
				UE_LOG(LogTraceGame, Display, TEXT("[MACEOYSTER] %s = OYSTER | live jars %d | cooldown %.1fs"),
					*GetNameSafe(Entry), OysterSet->GetLiveJarCount(), Comp->GetActivatedCooldownRemaining());
			}
		}

		UE_LOG(LogTraceGame, Display, TEXT("[MACEOYSTER] carrier tally (MUST BE ALL ZERO): %s"),
			*DescribeTally(TraceOyster::CarrierTally()));
		UE_LOG(LogTraceGame, Display, TEXT("[MACEOYSTER] non-carrier tally: %s"),
			*DescribeTally(TraceOyster::OtherTally()));
	}

	FAutoConsoleCommand CmdDumpState(
		TEXT("Trace.MaceOyster.DumpState"),
		TEXT("Dev only. Mace's and Oyster's live state, the resolved ability keys, and the per-vector carrier alarm."),
		FConsoleCommandDelegate::CreateStatic(&RunDumpState));
}   // namespace TraceMaceOysterVerify

#endif // !UE_BUILD_SHIPPING
