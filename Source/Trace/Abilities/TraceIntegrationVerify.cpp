// Trace — THE INTEGRATION HARNESS. spec v14 §§4/5/6, the seams between the slices.
//
//   Trace.Integration.Verify
//
// ===================================================================================================
// WHY THIS FILE EXISTS
// ===================================================================================================
//
// Every character in this pass was built and proven against the framework's own hooks, and every one
// of those proofs called the hook DIRECTLY. That is the right way to test an ability, and it is
// worth exactly nothing as evidence that anything in the game ever calls it. Three separate reports
// from this pass close with the same sentence in different words: "correct, measured through the
// exact function it is meant to call, and the last mile is one multiply someone else owns."
//
// This harness is the last mile, and only the last mile. It asserts nothing about whether Rocco's
// arithmetic is right — Trace.Rocco.Verify does that. It asserts that the SHIPPING GAME CODE reaches
// the ability layer at all:
//
//   ATraceGameMode::NotifyCharacterDied            -> NotifyKill / NotifyPawnDied
//   ATraceGameMode::RestartPlayerFresh             -> NotifyPawnSpawned
//   UTraceCharacterMovementComponent::BeginDash    -> NotifyDashStarted
//     ... and its dash-exit branch                 -> NotifyDashEnded
//     ... and its per-frame dash contact sweep     -> NotifyDashHitCharacter      (Chut's bash)
//     ... and GetMaxSpeed()                        -> GetMoveSpeedMultiplierFor   (Rocco, X)
//   ATraceCore::ServerApplyCatchZone               -> GetMagnetRadiusMultiplierFor (Mace)
//   ATracePlayerController::OnAbilityStarted       -> the E key, through the REAL input pipeline
//
// ===================================================================================================
// THE RED ARM, AND WHY IT IS THE WHOLE POINT
// ===================================================================================================
//
// Trace.Ability.Integration 0 removes every one of those calls at once — it reproduces, exactly, the
// build this project actually had before the integration pass. So this harness runs the ENTIRE
// scenario twice: arm 0 with the wiring removed, then arm 1 with it shipped, in one process on one
// binary.
//
// A check is only reported as passing when the RED arm failed it and the GREEN arm passed it. A
// check that is green in both arms is not measuring the wiring — it is measuring something else that
// happens to be true — and it is reported as INCONCLUSIVE, never as a pass. That is the failure mode
// that has bitten this project repeatedly (a fixture that could not reach a wall; a speed test that
// read its own cache; a bee test that invalidated itself), so the verdict is built to name it rather
// than to average it away.
//
// A check the scenario could not stage at all — no loose Core to magnet, no second enemy to bash —
// is reported NOT EXERCISED and is excluded from the verdict, loudly. It is not a pass.
//
// TIMING IS REAL TIME (FPlatformTime::Seconds), for the reason Trace.Ability.CooldownPersistenceTest
// gives: the character-select screen pauses the world, and a harness waiting on world time waits
// forever.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityWorldSubsystem.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceIntegrationVerify
{
	// -------------------------------------------------------------------------------------------
	// One measured seam.
	// -------------------------------------------------------------------------------------------
	struct FSeam
	{
		const TCHAR* Name = TEXT("");
		/** What the green arm saw. Meaning is per seam; > 0 is "the call site fired". */
		int32 Green = 0;
		/** What the red arm saw. Must be 0, or the seam is not what moved the number. */
		int32 Red = 0;
		/** False when the scenario could not be staged at all in either arm. */
		bool  bExercised = false;
		/**
		 * True when the RED arm actually ran this scenario. A seam the red arm never staged has a
		 * red count of 0 for a reason that is NOT the wiring, so its green must be reported with
		 * that caveat rather than as a clean red-then-green result.
		 */
		bool  bRedStaged = false;
		/** Free text printed beside the numbers — the actual measurement, not a restatement. */
		FString Detail;
	};

	enum class EStep : uint8
	{
		Setup,
		AwaitSelectScreen,
		BaselineSpeed,
		Kill,
		AfterKill,
		AwaitRespawn,
		Dash,
		AfterDash,
		ChutSetup,
		ChutDash,
		AfterChutDash,
		Magnet,
		AfterMagnet,
		AbilityKey,
		AfterAbilityKey,
		Finish,
		Done,
	};

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

	void SetCVarInt(const TCHAR* CVarName, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** The local human's ability component — the one the E key can actually reach. */
	UTraceAbilityComponent* FindHumanComponent(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}
		TArray<UTraceAbilityComponent*> Components;
		UTraceAbilityWorldSubsystem::GatherAllComponents(WorldPtr, Components);
		for (UTraceAbilityComponent* Comp : Components)
		{
			if (Comp != nullptr && !Comp->IsBot())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/** A living enemy of @p Of, excluding @p Exclude. */
	ATraceCharacter* FindEnemy(UWorld* WorldPtr, const ATraceCharacter* Of, const ATraceCharacter* Exclude)
	{
		if (WorldPtr == nullptr || Of == nullptr)
		{
			return nullptr;
		}
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Of || Candidate == Exclude || !Candidate->IsAlive())
			{
				continue;
			}
			if (Candidate->GetTeam() == ETraceTeam::None || Candidate->GetTeam() == Of->GetTeam())
			{
				continue;
			}
			// Never the Core carrier: killing or bashing one is the §4 rule's business, not this
			// harness's, and a refusal there would look exactly like a missing call site.
			if (UTraceAbilityComponent::IsCarrier(Candidate))
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	// -------------------------------------------------------------------------------------------
	// The run.
	// -------------------------------------------------------------------------------------------
	struct FRun : public TSharedFromThis<FRun>
	{
		int32  Arm = 0;                 // 0 = RED (wiring removed), 1 = GREEN (shipped)
		EStep  Step = EStep::Setup;
		double StepDeadline = 0.0;
		double StepStarted = 0.0;

		TWeakObjectPtr<UWorld> WorldPtr;
		TWeakObjectPtr<UTraceAbilityComponent> Tester;
		TWeakObjectPtr<ATraceCharacter> TesterPawn;
		TWeakObjectPtr<ATraceCharacter> Victim;
		TWeakObjectPtr<ATraceCharacter> BashTarget;

		float BaselineMaxSpeed = 0.f;
		float BoostedMaxSpeed = 0.f;
		bool  bSelectPickIssued = false;
		/** What the movement component will actually sweep with. 0 means "the tester is not Chut". */
		float ChutSweepRadius = 0.f;
		/** True once IsDashing() has been SEEN true — a DoDash() call is not a dash. */
		bool  bChutDashObserved = false;
		/** One-shot: the character swap that makes the E press produce a real cooldown. */
		bool  bSwitchedForKeyPress = false;

		TraceAbilityIntegration::FCounters Mark;

		/** Seam results, indexed by ESeam. Shared across the two arms. */
		TArray<FSeam>* Seams = nullptr;

		/**
		 * Re-poll the CURRENT step in @p Seconds. Deliberately does NOT touch StepStarted.
		 *
		 * THE FIRST VERSION OF THIS HARNESS RESET StepStarted HERE, and every "give up after N
		 * seconds" test in the file was therefore measured from the last poll rather than from the
		 * step's entry — so every one of them was an infinite loop. It hung on its first run, in
		 * silence, with the verdict never printed. Two clocks, and only Goto() moves the second one.
		 */
		void Wait(float Seconds)
		{
			StepDeadline = FPlatformTime::Seconds() + static_cast<double>(Seconds);
		}

		/** Enter @p NewStep after @p Seconds. This is the only thing that restarts the step clock. */
		void Goto(EStep NewStep, float Seconds)
		{
			Step = NewStep;
			StepStarted = FPlatformTime::Seconds();
			StepDeadline = StepStarted + static_cast<double>(Seconds);
			UE_LOG(LogTraceGame, Verbose, TEXT("[Integration] arm %d -> step %d"), Arm, static_cast<int32>(NewStep));
		}

		/** Seconds inside the current step. Never reset by a poll. */
		double InStep() const { return FPlatformTime::Seconds() - StepStarted; }

		bool Elapsed() const { return FPlatformTime::Seconds() >= StepDeadline; }

		void MarkCounters() { Mark = TraceAbilityIntegration::Counters(); }
	};

	enum ESeam : int32
	{
		Seam_Kill = 0,
		Seam_PawnDied,
		Seam_PawnSpawned,
		Seam_SpeedMultiply,
		Seam_DashStart,
		Seam_DashEnd,
		Seam_DashContact,
		Seam_Magnet,
		Seam_AbilityKey,
		Seam_Count,
	};

	TArray<FSeam>& Seams()
	{
		static TArray<FSeam> Instance;
		if (Instance.Num() != Seam_Count)
		{
			Instance.SetNum(Seam_Count);
			Instance[Seam_Kill].Name         = TEXT("GameMode -> NotifyKill                 (Rocco's stack, Chud's refresh)");
			Instance[Seam_PawnDied].Name     = TEXT("GameMode -> NotifyPawnDied             (jar/spike teardown)");
			Instance[Seam_PawnSpawned].Name  = TEXT("GameMode -> NotifyPawnSpawned          (respawn, cooldown UNTOUCHED)");
			Instance[Seam_SpeedMultiply].Name= TEXT("Movement::GetMaxSpeed -> speed passive (Rocco +3%/stack, X +10%)");
			Instance[Seam_DashStart].Name    = TEXT("Movement::BeginDash -> NotifyDashStarted   (Oyster's jar)");
			Instance[Seam_DashEnd].Name      = TEXT("Movement dash exit -> NotifyDashEnded      (latch reset)");
			Instance[Seam_DashContact].Name  = TEXT("Movement sweep -> NotifyDashHitCharacter   (Chut's bash)");
			Instance[Seam_Magnet].Name       = TEXT("TraceCore::ServerApplyCatchZone -> magnet  (Mace +30%)");
			Instance[Seam_AbilityKey].Name   = TEXT("THE E KEY -> OnAbilityStarted -> TryActivate");
		}
		return Instance;
	}

	void Record(int32 Which, int32 Arm, int32 Value, const FString& Detail)
	{
		FSeam& Seam = Seams()[Which];
		if (Arm == 0)
		{
			Seam.Red = Value;
			Seam.bRedStaged = true;
		}
		else
		{
			Seam.Green = Value;
			Seam.Detail = Detail;
		}
		Seam.bExercised = true;
	}

	/** Marks a seam as un-stageable. Never a pass, and excluded from the verdict. */
	void RecordNotExercised(int32 Which, const FString& Why)
	{
		FSeam& Seam = Seams()[Which];
		Seam.bExercised = false;
		Seam.Detail = Why;
	}

	void StartArm(TSharedRef<FRun> Run, int32 Arm);

	void PrintVerdict()
	{
		int32 Passed = 0;
		int32 Failed = 0;
		int32 Inconclusive = 0;
		int32 NotExercised = 0;

		UE_LOG(LogTraceGame, Display, TEXT("[Integration] ================ SEAMS ================"));
		for (const FSeam& Seam : Seams())
		{
			if (!Seam.bExercised)
			{
				++NotExercised;
				UE_LOG(LogTraceGame, Warning, TEXT("[Integration] NOT EXERCISED  %s  (%s)"),
					Seam.Name, *Seam.Detail);
				continue;
			}

			const bool bGreenFired = Seam.Green > 0;
			const bool bRedQuiet   = Seam.Red == 0;

			if (bGreenFired && bRedQuiet)
			{
				++Passed;
				UE_LOG(LogTraceGame, Display, TEXT("[Integration] PASS   %s | green=%d red=%d%s | %s"),
					Seam.Name, Seam.Green, Seam.Red,
					Seam.bRedStaged ? TEXT("")
					                : TEXT("  [CAVEAT: the RED arm never staged this one, so its 0 is not "
					                       "itself evidence]"),
					*Seam.Detail);
			}
			else if (bGreenFired && !bRedQuiet)
			{
				++Inconclusive;
				UE_LOG(LogTraceGame, Error,
					TEXT("[Integration] INCONCLUSIVE %s | green=%d red=%d — the RED arm fired too, so this "
					     "number is NOT measuring the wiring. Do not read it as a pass."),
					Seam.Name, Seam.Green, Seam.Red);
			}
			else
			{
				++Failed;
				UE_LOG(LogTraceGame, Error, TEXT("[Integration] FAIL   %s | green=%d red=%d | %s"),
					Seam.Name, Seam.Green, Seam.Red, *Seam.Detail);
			}
		}

		const bool bPass = (Failed == 0) && (Inconclusive == 0) && (Passed > 0);
		UE_LOG(LogTraceGame, Display,
			TEXT("[Integration] VERDICT: %s — %d passed, %d failed, %d inconclusive, %d NOT EXERCISED."),
			bPass ? ((NotExercised == 0) ? TEXT("PASS") : TEXT("PASS (with un-staged seams, listed above)"))
			      : TEXT("FAIL"),
			Passed, Failed, Inconclusive, NotExercised);
	}

	bool Tick(TSharedRef<FRun> Run);

	void StartArm(TSharedRef<FRun> Run, int32 Arm)
	{
		Run->Arm = Arm;
		Run->Goto(EStep::Setup, 0.f);

		SetCVarInt(TEXT("Trace.Ability.Integration"), Arm);
		UE_LOG(LogTraceGame, Display,
			TEXT("[Integration] ===== ARM %d (%s) ===== Trace.Ability.Integration %d"),
			Arm, (Arm == 0) ? TEXT("RED — the pre-integration build") : TEXT("GREEN — shipped"), Arm);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float /*Delta*/) -> bool { return Tick(Run); }), 0.f);
	}

	bool Tick(TSharedRef<FRun> Run)
	{
		if (!Run->Elapsed())
		{
			return true;
		}

		UWorld* WorldPtr = Run->WorldPtr.Get();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[Integration] the world went away mid-run. INVALID."));
			return false;
		}

		const TraceAbilityIntegration::FCounters& Now = TraceAbilityIntegration::Counters();

		switch (Run->Step)
		{
		case EStep::Setup:
		{
			UTraceAbilityComponent* Comp = FindHumanComponent(WorldPtr);
			if (Comp == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[Integration] no non-bot ability component. INVALID."));
				return false;
			}
			Run->Tester = Comp;

			// The select screen suppresses input, and the E-key seam is measured through the real
			// pipeline — so it must be dismissed rather than worked around. One console pick drives
			// the SCREEN's own request path; if no screen is up this is a harmless no-op.
			if (!Run->bSelectPickIssued)
			{
				Run->bSelectPickIssued = true;
				GEngine->Exec(WorldPtr, TEXT("Trace.Characters.Select 1"));
			}
			Run->Goto(EStep::AwaitSelectScreen, 0.5f);
			return true;
		}

		case EStep::AwaitSelectScreen:
		{
			const ATracePlayerController* PC = WorldPtr->GetFirstPlayerController<ATracePlayerController>();
			const bool bSuppressed = (PC != nullptr) && PC->IsGameInputSuppressed();
			if (bSuppressed && Run->InStep() < 12.0)
			{
				Run->Wait(0.25f);
				return true;
			}
			if (bSuppressed)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Integration] input is still suppressed after 12s — the E-key seam will not be "
					     "reachable and will report NOT EXERCISED."));
			}

			// Rocco for the kill / speed seams. Set directly rather than through the screen: the
			// screen's own path is Trace.Characters.Verify's job, not this harness's.
			GEngine->Exec(WorldPtr, TEXT("Trace.Ability.SetCharacter Rocco"));
			Run->Goto(EStep::BaselineSpeed, 0.4f);
			return true;
		}

		case EStep::BaselineSpeed:
		{
			UTraceAbilityComponent* Comp = Run->Tester.Get();
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (MyPawn == nullptr || !MyPawn->IsAlive())
			{
				if (Run->InStep() < 10.0)
				{
					Run->Wait(0.25f);
					return true;
				}
				UE_LOG(LogTraceGame, Error, TEXT("[Integration] the tester never had a living pawn. INVALID."));
				return false;
			}
			Run->TesterPawn = MyPawn;

			const UTraceCharacterMovementComponent* Move = MyPawn->GetTraceMovement();
			Run->BaselineMaxSpeed = (Move != nullptr) ? Move->GetMaxSpeed() : 0.f;

			Run->Victim = FindEnemy(WorldPtr, MyPawn, nullptr);
			if (!Run->Victim.IsValid())
			{
				UE_LOG(LogTraceGame, Error, TEXT("[Integration] no living enemy to kill. INVALID."));
				return false;
			}

			Run->MarkCounters();
			Run->Goto(EStep::Kill, 0.f);
			return true;
		}

		case EStep::Kill:
		{
			ATraceCharacter* VictimPawn = Run->Victim.Get();
			UTraceAbilityComponent* Comp = Run->Tester.Get();
			ATraceCharacter* MyPawn = Run->TesterPawn.Get();
			if (VictimPawn == nullptr || Comp == nullptr || MyPawn == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[Integration] lost a participant before the kill. INVALID."));
				return false;
			}

			UTraceHealthComponent* VictimHealth = VictimPawn->FindComponentByClass<UTraceHealthComponent>();
			if (VictimHealth == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[Integration] the victim has no health component. INVALID."));
				return false;
			}

			// THE SHIPPING KILL PATH, not a direct NotifyKill: Kill() -> OnDeath ->
			// ATraceGameMode::NotifyCharacterDied is the funnel the wiring lives in. Cause
			// "Headshot" because Rocco's stack is headshot-kills-only and a body shot would leave
			// the stack at zero for a reason that has nothing to do with the wiring.
			VictimHealth->Kill(MyPawn->GetController(), FName(TEXT("Headshot")));

			Run->Goto(EStep::AfterKill, 0.6f);
			return true;
		}

		case EStep::AfterKill:
		{
			const int32 KillDelta = Now.Kills - Run->Mark.Kills;
			const int32 DiedDelta = Now.PawnDied - Run->Mark.PawnDied;

			UTraceAbilityComponent* Comp = Run->Tester.Get();
			ATraceCharacter* MyPawn = Run->TesterPawn.Get();
			const UTraceCharacterMovementComponent* Move = (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;
			Run->BoostedMaxSpeed = (Move != nullptr) ? Move->GetMaxSpeed() : 0.f;

			const float SpeedDelta = Run->BoostedMaxSpeed - Run->BaselineMaxSpeed;
			const int32 SpeedFired = (SpeedDelta > 1.f) ? 1 : 0;

			Record(Seam_Kill, Run->Arm, KillDelta,
				FString::Printf(TEXT("one headshot kill through ATraceGameMode::NotifyCharacterDied")));
			Record(Seam_PawnDied, Run->Arm, DiedDelta, TEXT("the victim's own set was told"));
			Record(Seam_SpeedMultiply, Run->Arm, SpeedFired,
				FString::Printf(TEXT("GetMaxSpeed %.1f -> %.1f uu/s (%+.1f) with %d Rocco stack(s)"),
					Run->BaselineMaxSpeed, Run->BoostedMaxSpeed, SpeedDelta,
					(Comp != nullptr && Comp->GetAbilitySet() != nullptr)
						? FMath::RoundToInt((Comp->GetAbilitySet()->GetMoveSpeedMultiplier() - 1.f)
							/ FMath::Max(0.0001f, UTraceSettings::Get().RoccoHeadshotSpeedBonusPerStack))
						: 0));

			Run->Goto(EStep::AwaitRespawn, 0.5f);
			return true;
		}

		case EStep::AwaitRespawn:
		{
			const int32 SpawnDelta = Now.PawnSpawned - Run->Mark.PawnSpawned;
			// The respawn is on a world timer. Give it a generous real-time window, then record
			// whatever happened — a zero here in the GREEN arm is a genuine finding, not a timeout.
			if (SpawnDelta <= 0 && Run->InStep() < 8.0)
			{
				Run->Wait(0.3f);
				return true;
			}
			Record(Seam_PawnSpawned, Run->Arm, SpawnDelta, TEXT("RestartPlayerFresh told the ability layer"));

			Run->MarkCounters();
			Run->Goto(EStep::Dash, 0.f);
			return true;
		}

		case EStep::Dash:
		{
			ATraceCharacter* MyPawn = Run->TesterPawn.Get();
			if (MyPawn == nullptr || !MyPawn->IsAlive())
			{
				// The tester can be replaced by a respawn of their own; re-acquire rather than fail.
				UTraceAbilityComponent* Comp = Run->Tester.Get();
				MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
				Run->TesterPawn = MyPawn;
			}
			if (MyPawn == nullptr || !MyPawn->IsAlive())
			{
				UE_LOG(LogTraceGame, Error, TEXT("[Integration] no living tester to dash. INVALID."));
				return false;
			}

			MyPawn->DoDash();                     // the shipping input entry point
			Run->Goto(EStep::AfterDash, FMath::Max(0.5f, UTraceSettings::Get().DashDuration + 0.4f));
			return true;
		}

		case EStep::AfterDash:
		{
			Record(Seam_DashStart, Run->Arm, Now.DashStarted - Run->Mark.DashStarted,
				TEXT("one DoDash() through the real movement component"));
			Record(Seam_DashEnd, Run->Arm, Now.DashEnded - Run->Mark.DashEnded,
				TEXT("the same dash's exit branch"));

			// Chut for the contact sweep: he is the only character that asks for a sweep radius, and
			// asking a Mannequin for one is exactly the no-op this seam must not be measured on.
			GEngine->Exec(WorldPtr, TEXT("Trace.Ability.SetCharacter Chut"));
			Run->Goto(EStep::ChutSetup, 0.4f);
			return true;
		}

		case EStep::ChutSetup:
		{
			ATraceCharacter* MyPawn = Run->TesterPawn.Get();
			if (MyPawn == nullptr || !MyPawn->IsAlive())
			{
				RecordNotExercised(Seam_DashContact, TEXT("no living tester to dash with"));
				Run->Goto(EStep::Magnet, 0.f);
				return true;
			}

			// THE SWEEP RADIUS IS THE PRECONDITION, AND IT IS CHECKED RATHER THAN ASSUMED. A zero
			// here means the tester is not Chut — SetCharacter was refused, the roster did not
			// resolve, whatever — and the sweep is then CORRECTLY skipped by the movement
			// component. Reporting that as a FAIL would blame the wiring for a staging failure,
			// which is the exact mistake this file exists to avoid.
			Run->ChutSweepRadius = UTraceAbilityComponent::GetDashHitSweepRadiusFor(MyPawn);
			if (Run->ChutSweepRadius <= 0.f)
			{
				RecordNotExercised(Seam_DashContact,
					TEXT("the tester's sweep radius was 0 — Chut never equipped, so there was no sweep to fire"));
				Run->Goto(EStep::Magnet, 0.f);
				return true;
			}

			ATraceCharacter* Target = FindEnemy(WorldPtr, MyPawn, nullptr);
			if (Target == nullptr)
			{
				RecordNotExercised(Seam_DashContact, TEXT("no living non-carrier enemy to put in reach"));
				Run->Goto(EStep::Magnet, 0.f);
				return true;
			}

			Run->BashTarget = Target;
			Run->MarkCounters();
			Run->bChutDashObserved = false;
			Run->Goto(EStep::ChutDash, 0.f);
			return true;
		}

		case EStep::ChutDash:
		{
			ATraceCharacter* MyPawn = Run->TesterPawn.Get();
			ATraceCharacter* Target = Run->BashTarget.Get();
			UTraceCharacterMovementComponent* Move = (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;
			if (MyPawn == nullptr || Target == nullptr || Move == nullptr || !MyPawn->IsAlive())
			{
				RecordNotExercised(Seam_DashContact, TEXT("the tester or the target went away before the bash dash"));
				Run->Goto(EStep::Magnet, 0.f);
				return true;
			}

			// RE-PLACED EVERY ATTEMPT, not once. A bot walks; a placement made three seconds ago is
			// not a placement. Just inside the radius rather than on top of the tester, because two
			// capsules sharing a point are flung apart at ~950 uu/s by depenetration — the fixture
			// bug the Rocco pass documented, and it would look exactly like "the sweep missed".
			const FVector Ahead = MyPawn->GetActorLocation()
				+ MyPawn->GetActorForwardVector() * (Run->ChutSweepRadius * 0.75f);
			Target->SetActorLocation(Ahead, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);

			if (Move->IsDashing())
			{
				// The dash is live and the target is in reach. Nothing else to do; the sweep runs
				// inside the movement component's own frame.
				Run->bChutDashObserved = true;
			}
			else if (!Run->bChutDashObserved)
			{
				// DoDash() can be REFUSED — the tester already dashed twice in this scenario and the
				// charge pool takes a second to refill. A refused dash is not a missing call site,
				// so this retries until a dash is genuinely observed, and gives up as NOT EXERCISED
				// rather than as a failure.
				MyPawn->DoDash();
			}

			if (Run->bChutDashObserved && !Move->IsDashing())
			{
				Run->Goto(EStep::AfterChutDash, 0.f);
				return true;
			}

			if (Run->InStep() > 8.0)
			{
				RecordNotExercised(Seam_DashContact,
					TEXT("DoDash() was refused for 8s (no charges) — no dash ever ran, so no sweep could fire"));
				Run->Goto(EStep::Magnet, 0.f);
				return true;
			}

			Run->Wait(0.05f);
			return true;
		}

		case EStep::AfterChutDash:
		{
			Record(Seam_DashContact, Run->Arm, Now.DashHits - Run->Mark.DashHits,
				FString::Printf(TEXT("one OBSERVED dash with a non-carrier enemy held inside %.0f uu"),
					Run->ChutSweepRadius));

			GEngine->Exec(WorldPtr, TEXT("Trace.Ability.SetCharacter Mace"));
			Run->Goto(EStep::Magnet, 0.4f);
			return true;
		}

		case EStep::Magnet:
		{
			// The magnet only runs while the Core is LOOSE — held, it is nobody's catch candidate.
			// If it is held for the whole window the seam is NOT EXERCISED, and says so, rather than
			// reporting a zero that means "the Core was in somebody's hands".
			TActorIterator<ATraceCore> CoreIt(WorldPtr);
			ATraceCore* Core = CoreIt ? *CoreIt : nullptr;

			// RE-ACQUIRED, not remembered. The tester has been killed and respawned twice by now in
			// a live match, so the pawn cached at the start of the run is usually a corpse — and
			// "no living tester" was the whole of this seam's first failure message, which said
			// nothing about the wiring at all.
			ATraceCharacter* MyPawn = Run->TesterPawn.Get();
			if (MyPawn == nullptr || !MyPawn->IsAlive())
			{
				UTraceAbilityComponent* Comp = Run->Tester.Get();
				MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
				Run->TesterPawn = MyPawn;
			}

			if (Core == nullptr)
			{
				RecordNotExercised(Seam_Magnet, TEXT("there is no ATraceCore in this world"));
				Run->Goto(EStep::AbilityKey, 0.f);
				return true;
			}
			if (MyPawn == nullptr || !MyPawn->IsAlive())
			{
				if (Run->InStep() < 8.0)
				{
					Run->Wait(0.3f);   // waiting on a respawn is not the same as having no wiring
					return true;
				}
				RecordNotExercised(Seam_Magnet, TEXT("the tester never had a living pawn inside 8s"));
				Run->Goto(EStep::AbilityKey, 0.f);
				return true;
			}

			if (Core->IsHeld())
			{
				if (Run->InStep() < 6.0)
				{
					Run->Wait(0.25f);
					return true;
				}
				RecordNotExercised(Seam_Magnet,
					TEXT("the Core stayed HELD for the whole window — ServerApplyCatchZone never ran"));
				Run->Goto(EStep::AbilityKey, 0.f);
				return true;
			}

			// Standing on top of a loose Core is the one placement that makes the catch zone
			// certainly consider this pawn. Mace's widening is per candidate, so being the nearest
			// candidate is exactly what the seam needs.
			MyPawn->SetActorLocation(Core->GetActorLocation() + FVector(0.f, 0.f, 90.f),
				/*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);

			Run->MarkCounters();
			Run->Goto(EStep::AfterMagnet, 0.6f);
			return true;
		}

		case EStep::AfterMagnet:
		{
			const int32 MagnetDelta = Now.MagnetWidenedFrames - Run->Mark.MagnetWidenedFrames;
			if (MagnetDelta <= 0 && Run->Arm == 1)
			{
				// Green arm and nothing: either the call site is dead or the Core was caught within
				// the frame. Distinguish the two rather than guessing, by asking the passive itself.
				UTraceAbilityComponent* Comp = Run->Tester.Get();
				const float Mult = (Comp != nullptr)
					? UTraceAbilityComponent::GetMagnetRadiusMultiplierFor(Comp->GetOwningCharacter()) : 1.f;
				if (FMath::IsNearlyEqual(Mult, 1.f))
				{
					RecordNotExercised(Seam_Magnet,
						FString::Printf(TEXT("the tester's magnet multiplier was %.2f — Mace never equipped, so "
						                     "there was nothing for the call site to widen"), Mult));
					Run->Goto(EStep::AbilityKey, 0.f);
					return true;
				}
			}
			Record(Seam_Magnet, Run->Arm, MagnetDelta,
				FString::Printf(TEXT("frames on which ServerApplyCatchZone widened Mace's radius (%.0f x %.2f)"),
					UTraceSettings::Get().CoreCatchRadius, 1.f + UTraceSettings::Get().MaceMagnetRadiusBonus));

			Run->Goto(EStep::AbilityKey, 0.f);
			return true;
		}

		case EStep::AbilityKey:
		{
			// BACK TO CHUT BEFORE THE PRESS. Mace's Spike needs a wall in the aim direction and
			// otherwise fizzles for free, so pressing E as Mace proves the key reached the framework
			// and then leaves a 0.0s cooldown that reads exactly like the ability never fired.
			// Chud has no aim precondition, so the cooldown printed beside this seam is real.
			if (!Run->bSwitchedForKeyPress)
			{
				Run->bSwitchedForKeyPress = true;
				GEngine->Exec(WorldPtr, TEXT("Trace.Ability.SetCharacter Chut"));
				Run->Wait(0.4f);
				return true;
			}

			const ATracePlayerController* PC = WorldPtr->GetFirstPlayerController<ATracePlayerController>();
			if (PC == nullptr || PC->IsGameInputSuppressed())
			{
				RecordNotExercised(Seam_AbilityKey,
					TEXT("no local controller, or input was suppressed — the real key could not be pressed"));
				Run->Goto(EStep::Finish, 0.f);
				return true;
			}

			Run->MarkCounters();
			// THE REAL KEY, THROUGH THE REAL PIPELINE. Not HandleActivatePressed(), not the relay's
			// router — Trace.SimInput injects the press at the input subsystem, so a bind that does
			// not exist, is not mapped, or is not routed produces a zero here.
			GEngine->Exec(WorldPtr, TEXT("Trace.SimInput E 0.10"));
			Run->Goto(EStep::AfterAbilityKey, 0.8f);
			return true;
		}

		case EStep::AfterAbilityKey:
		{
			UTraceAbilityComponent* Comp = Run->Tester.Get();
			const float Cooldown = (Comp != nullptr) ? Comp->GetActivatedCooldownRemaining() : 0.f;
			const int32 PressDelta = Now.ActivatePressed - Run->Mark.ActivatePressed;

			Record(Seam_AbilityKey, Run->Arm, PressDelta,
				FString::Printf(TEXT("one Trace.SimInput E; cooldown after the press = %.1fs"), Cooldown));

			Run->Goto(EStep::Finish, 0.f);
			return true;
		}

		case EStep::Finish:
		{
			if (Run->Arm == 0)
			{
				// The red arm is done. Put the world back the way it was and run it for real.
				TSharedRef<FRun> Next = MakeShared<FRun>();
				Next->WorldPtr = Run->WorldPtr;
				StartArm(Next, 1);
				return false;
			}

			SetCVarInt(TEXT("Trace.Ability.Integration"), 1);   // never leave the arm disarmed
			PrintVerdict();
			Run->Step = EStep::Done;
			return false;
		}

		default:
			return false;
		}
	}

	void RunCommand()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Integration] server only, and there is no authoritative game world yet."));
			return;
		}

		Seams().Reset();
		Seams();                                   // re-seed the names
		TraceAbilityIntegration::ResetCounters();

		UE_LOG(LogTraceGame, Display,
			TEXT("[Integration] ===== Trace.Integration.Verify — spec v14, the seams between the slices ====="));
		UE_LOG(LogTraceGame, Display,
			TEXT("[Integration] Two arms on one binary. A seam PASSES only when the RED arm did NOT fire it "
			     "and the GREEN arm did. Green in both arms is reported INCONCLUSIVE, never as a pass."));

		TSharedRef<FRun> Run = MakeShared<FRun>();
		Run->WorldPtr = WorldPtr;
		StartArm(Run, 0);
	}

	FAutoConsoleCommand CmdIntegrationVerify(
		TEXT("Trace.Integration.Verify"),
		TEXT("SPEC v14. Proves the SHIPPING game code reaches the ability layer: the kill/death/respawn "
		     "hooks, the dash hooks, the per-frame dash contact sweep, the speed passive inside "
		     "GetMaxSpeed(), Mace's magnet inside the catch zone, and the E key through the real input "
		     "pipeline. Runs the Trace.Ability.Integration 0 RED arm first; a seam green in both arms is "
		     "reported INCONCLUSIVE."),
		FConsoleCommandDelegate::CreateStatic(&RunCommand));
}

#endif   // !UE_BUILD_SHIPPING
