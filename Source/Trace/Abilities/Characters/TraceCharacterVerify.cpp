// Trace — the evidence for ROCCO and CHUT (spec v14 §6).
//
// ===================================================================================================
// TWO STAGED HARNESSES, EACH RUNNING ITS OWN RED ARM FIRST, IN ONE PROCESS
// ===================================================================================================
//
//     Trace.Rocco.Verify      arm 0 (RED: Rocco's three abilities switched off) then arm 1 (shipped).
//     Trace.Chut.Verify       arm 0 (RED: Chut's three abilities switched off), arm 2 (RED: the §4
//                             carrier rule removed), then arm 1 (shipped).
//
// EVERY ASSERTION IS MEASURED FROM THE SHIPPING CODE PATH. The knife and Chud numbers come out of
// UTraceAbilityComponent::ModifyDamageThroughPassives — the same function the weapon and melee
// slices will call — not out of a copy of the arithmetic. The bash goes through
// UTraceAbilitySetChut::TryBash, which is the one apply path both the poll and the framework hook
// use. The ride is read off the live ATraceRippleActor by asking the victim's own movement component
// what its velocity is.
//
// WHY THE RED ARM IS RUN FIRST AND WHY A GREEN ARM ALONE IS REPORTED AS INVALID.
// This project has shipped two harnesses whose fixture could not reach the thing under test, and
// both were green. So each arm 0 removes exactly one ability's implementation and NOTHING else, and
// the verdict requires those assertions to have gone RED. If the red arm does not reproduce, the
// green arm proves only that the harness runs.
//
// WHY THE WORLD CLOCK IS CHECKED BETWEEN STAGES.
// The character-select screen suppresses input and (before it was fixed) paused the world. A paused
// world still runs FTSTicker but stops ticking actors, so a ride would measure zero and look exactly
// like a broken ability. Every stage records GetWorld()->GetTimeSeconds(); if it has not advanced,
// the run reports INVALID instead of FAIL. A harness that cannot tell "the ability is broken" from
// "the world was frozen" is not evidence.
//
// TIMING IS REAL TIME (FPlatformTime::Seconds), for the same reason the framework's own harness uses
// it: world time is exactly the thing under suspicion.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/Characters/TraceAbilitySetChut.h"
#include "Abilities/Characters/TraceAbilitySetRocco.h"
#include "Abilities/Characters/TraceRippleActor.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceMelee.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceCharacterVerify
{
	// =============================================================================================
	// Shared plumbing
	// =============================================================================================

	struct FCheck
	{
		FString  Name;
		bool     bPass = false;
		FString  Detail;
	};

	/** One arm's accumulated result. */
	struct FArmLog
	{
		int32 Arm = 1;
		TArray<FCheck> Checks;
		bool bInvalid = false;
		FString InvalidReason;

		/**
		 * Assertions the fixture could NOT exercise. Counted, printed in the verdict, and never
		 * silently dropped: "the harness could not reach this" and "this passed" are different
		 * answers, and conflating them is exactly how a half-built ability gets declared done.
		 */
		int32 NotExercised = 0;
		TArray<FString> NotExercisedNotes;

		void Add(const FString& Name, bool bPass, const FString& Detail)
		{
			Checks.Add({Name, bPass, Detail});
		}

		void Invalidate(const FString& Reason)
		{
			bInvalid = true;
			InvalidReason = Reason;
		}

		int32 CountFailed() const
		{
			int32 Failed = 0;
			for (const FCheck& Check : Checks)
			{
				Failed += Check.bPass ? 0 : 1;
			}
			return Failed;
		}
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
			if (Candidate != nullptr && Candidate->IsGameWorld()
				&& Candidate->GetNetMode() != NM_Client)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/**
	 * The first non-bot player's ability component.
	 *
	 * NOT because bots are characterless — spec v15 §2 gives them characters. Because this harness
	 * ASSIGNS its subject a character, and a bot's is owned by ATraceGameMode::PollCharacterSelect's
	 * 4 Hz fill, which would take it back or take it first.
	 */
	UTraceAbilityComponent* FindHumanAbilityComponent(UWorld* WorldPtr)
	{
		const AGameStateBase* GS = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (GS == nullptr)
		{
			return nullptr;
		}
		for (APlayerState* PS : GS->PlayerArray)
		{
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PS);
			if (Comp != nullptr && !Comp->IsBot())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/** Any living pawn that is not @p Exclude. Used as the harness's victim and its instigator. */
	ATraceCharacter* FindOtherAliveCharacter(UWorld* WorldPtr, const ATraceCharacter* Exclude,
	                                         const ATraceCharacter* AlsoExclude = nullptr)
	{
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate != nullptr && Candidate != Exclude && Candidate != AlsoExclude && Candidate->IsAlive())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	ATraceCharacter* FindCarrierPawn(UWorld* WorldPtr)
	{
		ATraceCore* Core = ATraceCore::Get(WorldPtr);
		return (Core != nullptr) ? Core->GetCarrier() : nullptr;
	}

	/**
	 * Make sure SOMEBODY OTHER THAN @p Exclude is holding the Core, and return them.
	 *
	 * THE FIRST RUN OF BOTH HARNESSES REPORTED "no Core carrier existed, so the carrier assertion was
	 * NOT EXERCISED". That is the §4 assertion — the founding invariant — and leaving it to whether a
	 * bot happened to be holding the Core when the command fired is exactly the kind of fixture whose
	 * green means nothing. ATraceCore::GrantTo is the project's own single funnel for possession and
	 * takes a Debug reason for precisely this, so the harness uses it rather than hoping.
	 */
	ATraceCharacter* EnsureCarrierOtherThan(UWorld* WorldPtr, const ATraceCharacter* Exclude,
	                                        const ATraceCharacter* AlsoExclude = nullptr)
	{
		ATraceCharacter* Carrier = FindCarrierPawn(WorldPtr);
		if (Carrier != nullptr && Carrier != Exclude && Carrier != AlsoExclude && Carrier->IsAlive())
		{
			return Carrier;
		}

		ATraceCore* Core = ATraceCore::Get(WorldPtr);
		ATraceCharacter* Fresh = FindOtherAliveCharacter(WorldPtr, Exclude, AlsoExclude);
		if (Core == nullptr || Fresh == nullptr)
		{
			return nullptr;
		}

		Core->GrantTo(Fresh, ETraceCoreGrantReason::Debug);
		UE_LOG(LogTraceGame, Display, TEXT("[CharVerify] forced the Core onto %s so the §4 carrier rule can be "
			"exercised."), *GetNameSafe(Fresh));

		Carrier = FindCarrierPawn(WorldPtr);
		return (Carrier != Exclude && Carrier != AlsoExclude) ? Carrier : nullptr;
	}

	void SetCVarInt(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** The damage pipeline the weapon and melee slices are asked to call. Never a local copy. */
	float RunDamagePipeline(float Amount, ATraceCharacter* Instigator, ATraceCharacter* Target,
	                        FName Cause, bool bMelee, bool bHeadshot)
	{
		FTraceAbilityDamageContext Context;
		Context.Instigator = Instigator;
		Context.Target     = Target;
		Context.Cause      = Cause;
		Context.bMelee     = bMelee;
		Context.bHeadshot  = bHeadshot;
		return UTraceAbilityComponent::ModifyDamageThroughPassives(Amount, Context);
	}

	bool NearlyEqual(float A, float B, float Tolerance)
	{
		return FMath::Abs(A - B) <= Tolerance;
	}

	void ReportArm(const TCHAR* Tag, const FArmLog& Log)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[%s] ----- arm %d: %d check(s), %d failed -----"),
			Tag, Log.Arm, Log.Checks.Num(), Log.CountFailed());

		for (const FCheck& Check : Log.Checks)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[%s]   %s  %-58s  %s"),
				Tag, Check.bPass ? TEXT("PASS") : TEXT("FAIL"), *Check.Name, *Check.Detail);
		}

		for (const FString& Note : Log.NotExercisedNotes)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s]   NOT EXERCISED  %s"), Tag, *Note);
		}

		if (Log.bInvalid)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s]   arm %d INVALID: %s"), Tag, Log.Arm, *Log.InvalidReason);
		}
	}

	// =============================================================================================
	// ROCCO
	// =============================================================================================

	struct FRoccoRun
	{
		TArray<int32> ArmsToRun;
		int32 ArmIndex = 0;
		int32 Step = 0;
		double NextRealTime = 0.0;

		TArray<FArmLog> Results;
		FArmLog Current;

		TWeakObjectPtr<UTraceAbilityComponent> Comp;
		TWeakObjectPtr<UTraceAbilitySetRocco> Rocco;
		TWeakObjectPtr<ATraceCharacter> RoccoPawn;
		TWeakObjectPtr<ATraceCharacter> Victim;
		TWeakObjectPtr<ATraceCharacter> CarrierPawn;
		TWeakObjectPtr<ATraceRippleActor> Ripple;

		/**
		 * -ExecCmds fires before the map has finished loading, before anybody has a PlayerState and
		 * long before the first pawn exists. Retrying is what makes the command usable from a
		 * headless launch at all; running out of retries is a real, reported failure.
		 */
		int32 SetupAttemptsLeft = 40;

		float WorldTimeAtStageStart = 0.f;
		float SavedStackDuration = 1.f;
		float SavedRippleDurationMultiplier = 1.f;
		float MultiplierBeforeExpiry = 0.f;

		FVector JumpVelocityBefore = FVector::ZeroVector;
		bool bJumpConsumed = false;

		/**
		 * How many frames the fixture has pumped a movement input while waiting for the movement
		 * component to turn it into a non-zero Acceleration.
		 *
		 * THE FIRST RUN OF THIS HARNESS COULD NOT EXERCISE THE REDIRECT AT ALL, and said so rather
		 * than passing vacuously. The cause: APawn::AddMovementInput only accumulates a vector that
		 * UCharacterMovementComponent::PerformMovement consumes on the NEXT tick, and Acceleration
		 * falls back to zero on the first tick that receives no input. Calling it once and reading
		 * three frames later therefore always read zero. The input has to be pumped EVERY frame
		 * until the movement component has produced the acceleration the redirect turns onto.
		 */
		int32 JumpPumpFrames = 0;

		bool bArmed() const { return ArmsToRun.IsValidIndex(ArmIndex) && ArmsToRun[ArmIndex] == 1; }
	};

	void ApplyRoccoArm(int32 Arm)
	{
		const int32 On = (Arm == 1) ? 1 : 0;
		SetCVarInt(TEXT("Trace.Rocco.RippleEnabled"), On);
		SetCVarInt(TEXT("Trace.Rocco.SecondJumpEnabled"), On);
		SetCVarInt(TEXT("Trace.Rocco.StackCapEnabled"), On);
	}

	bool TickRoccoRun(TSharedPtr<FRoccoRun> Run);

	void ScheduleRocco(TSharedPtr<FRoccoRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + DelaySeconds;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;   // keep waiting; real time, because world time is the suspect
				}
				return TickRoccoRun(Run);
			}), 0.f);
	}

	void FinishRoccoArm(TSharedPtr<FRoccoRun> Run)
	{
		Run->Current.Arm = Run->ArmsToRun[Run->ArmIndex];
		Run->Results.Add(Run->Current);
		Run->Current = FArmLog();
		++Run->ArmIndex;
		Run->Step = 0;
	}

	void ReportRoccoVerdict(TSharedPtr<FRoccoRun> Run)
	{
		const UTraceSettings& Settings = UTraceSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("[ROCCO] ===== spec v14 §6 ROCCO — passive %.0f%%/headshot-kill (cap %d, %.1fs shared window), "
			     "second jump %.0f uu/s @ redirect %.2f, Ripple %.0fs / %.0fs cooldown ====="),
			Settings.RoccoHeadshotSpeedBonusPerStack * 100.f, Settings.RoccoHeadshotSpeedStackMax,
			Settings.RoccoHeadshotSpeedDurationSeconds, Settings.RoccoSecondJumpZVelocity,
			Settings.RoccoSecondJumpRedirectFraction, Settings.RoccoRippleLifetimeSeconds,
			Settings.RoccoRippleCooldownSeconds);

		const FArmLog* Red = nullptr;
		const FArmLog* Green = nullptr;
		for (const FArmLog& Log : Run->Results)
		{
			ReportArm(TEXT("ROCCO"), Log);
			if (Log.Arm == 0) { Red = &Log; }
			if (Log.Arm == 1) { Green = &Log; }
		}

		if (Red == nullptr || Green == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[ROCCO] VERDICT: INVALID — an arm did not run."));
			return;
		}
		if (Red->bInvalid || Green->bInvalid)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[ROCCO] VERDICT: INVALID — %s"),
				Red->bInvalid ? *Red->InvalidReason : *Green->InvalidReason);
			return;
		}

		const int32 RedFailures = Red->CountFailed();
		const int32 GreenFailures = Green->CountFailed();

		if (RedFailures == 0)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[ROCCO] VERDICT: INVALID — the RED arm did not reproduce (0 failures with Rocco's three "
				     "abilities switched off). The fixture is not reaching the abilities, so arm 1's green means "
				     "nothing."));
			return;
		}

		if (GreenFailures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ROCCO] VERDICT: PASS — red arm reproduced %d failure(s) with the abilities off; the shipped "
				     "arm passed all %d. *** %d assertion(s) COULD NOT BE EXERCISED and are therefore NOT covered "
				     "by this pass — see the NOT EXERCISED line(s) above. ***"),
				RedFailures, Green->Checks.Num(), Green->NotExercised);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[ROCCO] VERDICT: *** FAIL *** — %d of %d assertions failed on the shipped arm (red arm "
				     "reproduced %d, so the fixture is sound)."),
				GreenFailures, Green->Checks.Num(), RedFailures);
		}
	}

	bool TickRoccoRun(TSharedPtr<FRoccoRun> Run)
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[ROCCO] no authoritative world — server only."));
			return false;
		}

		if (!Run->ArmsToRun.IsValidIndex(Run->ArmIndex))
		{
			ReportRoccoVerdict(Run);
			return false;
		}

		const int32 Arm = Run->ArmsToRun[Run->ArmIndex];
		const UTraceSettings& Settings = UTraceSettings::Get();
		UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>();

		switch (Run->Step)
		{
		case 0:
		{
			ApplyRoccoArm(Arm);

			UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
			if (Comp == nullptr || Comp->GetOwningCharacter() == nullptr)
			{
				if (Run->SetupAttemptsLeft-- > 0)
				{
					ScheduleRocco(Run, 1.0f);   // the match has not produced a player with a pawn yet
					return false;
				}
				Run->Current.Invalidate(TEXT("no non-bot player with an ability component AND a living pawn "
					"appeared inside the 40s budget"));
				FinishRoccoArm(Run);
				ScheduleRocco(Run, 0.f);
				return false;
			}

			Comp->ServerSetCharacter(ETraceCharacterId::Rocco);
			Comp->OnHalfTime();   // harness reset: zero the cooldown and the transient state

			UTraceAbilitySetRocco* Rocco = Comp->GetAbilitySetAs<UTraceAbilitySetRocco>();
			Run->Comp = Comp;
			Run->Rocco = Rocco;
			Run->RoccoPawn = Comp->GetOwningCharacter();

			if (Rocco == nullptr || Run->RoccoPawn.Get() == nullptr)
			{
				Run->Current.Invalidate(FString::Printf(
					TEXT("character set is %s and pawn is %s — the framework did not equip Rocco"),
					*GetNameSafe(Comp->GetAbilitySet()), *GetNameSafe(Comp->GetOwningCharacter())));
				FinishRoccoArm(Run);
				ScheduleRocco(Run, 0.f);
				return false;
			}

			Run->WorldTimeAtStageStart = WorldPtr->GetTimeSeconds();

			// ---- PASSIVE: the stack, and the ONE window over the whole stack ----------------------
			for (int32 Kill = 0; Kill < 3; ++Kill)
			{
				Rocco->OnKill(nullptr, FName(TEXT("Headshot")), /*bHeadshot*/ true);
			}
			const float ThreeStacks = Rocco->GetMoveSpeedMultiplier();
			Run->Current.Add(TEXT("3 headshot kills = +9% speed (below the cap, both arms)"),
				NearlyEqual(ThreeStacks, 1.f + 3.f * Settings.RoccoHeadshotSpeedBonusPerStack, 0.001f),
				FString::Printf(TEXT("stacks=%d multiplier=%.3f"), Rocco->GetLiveStackCount(), ThreeStacks));

			for (int32 Kill = 0; Kill < 12; ++Kill)
			{
				Rocco->OnKill(nullptr, FName(TEXT("Headshot")), true);
			}
			const int32 Stacks = Rocco->GetLiveStackCount();
			const float Capped = Rocco->GetMoveSpeedMultiplier();
			// EVERY ASSERTION STATES THE SHIPPED EXPECTATION, IN EVERY ARM. That is what makes the red
			// arm red: it runs the same sentence against a build with the ability removed and fails.
			// Inverting the expectation per arm would make the red arm pass, which proves nothing.
			const int32 ExpectedStacks = Settings.RoccoHeadshotSpeedStackMax;
			Run->Current.Add(TEXT("15 headshot kills stop at the stack cap [v14 §6 ASSUMPTION]"),
				Stacks == ExpectedStacks,
				FString::Printf(TEXT("stacks=%d expected=%d multiplier=%.3f"), Stacks, ExpectedStacks, Capped));

			// A non-headshot kill must move nothing at all — the control that proves the stack is
			// driven by the headshot flag and not merely by "a kill happened".
			Comp->OnHalfTime();
			Rocco->OnKill(nullptr, FName(TEXT("Bullet")), /*bHeadshot*/ false);
			Run->Current.Add(TEXT("a BODY-shot kill adds no stack (control, both arms)"),
				Rocco->GetLiveStackCount() == 0 && NearlyEqual(Rocco->GetMoveSpeedMultiplier(), 1.f, 0.0001f),
				FString::Printf(TEXT("stacks=%d multiplier=%.3f"),
					Rocco->GetLiveStackCount(), Rocco->GetMoveSpeedMultiplier()));

			// ---- the shared window closing --------------------------------------------------------
			Run->SavedStackDuration = Settings.RoccoHeadshotSpeedDurationSeconds;
			if (MutableSettings != nullptr)
			{
				MutableSettings->RoccoHeadshotSpeedDurationSeconds = 0.30f;
			}
			Rocco->OnKill(nullptr, FName(TEXT("Headshot")), true);
			Rocco->OnKill(nullptr, FName(TEXT("Headshot")), true);
			Run->MultiplierBeforeExpiry = Rocco->GetMoveSpeedMultiplier();

			Run->Step = 1;
			ScheduleRocco(Run, 0.75f);
			return false;
		}

		case 1:
		{
			UTraceAbilitySetRocco* Rocco = Run->Rocco.Get();
			UTraceAbilityComponent* Comp = Run->Comp.Get();
			if (Rocco == nullptr || Comp == nullptr)
			{
				Run->Current.Invalidate(TEXT("the ability set went away mid-run"));
				FinishRoccoArm(Run);
				ScheduleRocco(Run, 0.f);
				return false;
			}

			const float WorldNow = WorldPtr->GetTimeSeconds();
			const float WorldAdvanced = WorldNow - Run->WorldTimeAtStageStart;
			if (WorldAdvanced < 0.1f)
			{
				Run->Current.Invalidate(FString::Printf(
					TEXT("the WORLD CLOCK advanced only %.3fs across a 0.75s wall-clock wait — the world is frozen "
					     "(paused select screen?), so nothing timed can be measured"), WorldAdvanced));
				FinishRoccoArm(Run);
				ScheduleRocco(Run, 0.f);
				return false;
			}

			Run->Current.Add(TEXT("the ONE window covers the WHOLE stack and closes together"),
				Run->MultiplierBeforeExpiry > 1.f
					&& Rocco->GetLiveStackCount() == 0
					&& NearlyEqual(Rocco->GetMoveSpeedMultiplier(), 1.f, 0.0001f),
				FString::Printf(TEXT("before=%.3f (2 stacks) after 0.30s window: stacks=%d multiplier=%.3f "
				                     "[world advanced %.2fs]"),
					Run->MultiplierBeforeExpiry, Rocco->GetLiveStackCount(),
					Rocco->GetMoveSpeedMultiplier(), WorldAdvanced));

			if (MutableSettings != nullptr)
			{
				MutableSettings->RoccoHeadshotSpeedDurationSeconds = Run->SavedStackDuration;
			}

			// ---- ACTIVATED: the Ripple -------------------------------------------------------------
			// The path is deliberately lengthened for the measurement: at shipped values the ride is
			// DashSpeed x DashDuration = 540 uu, i.e. 0.18 s, which is shorter than one scheduling
			// step of this harness. Lengthening it is a knob, not a code path — everything measured
			// still goes through the shipping ripple.
			Run->SavedRippleDurationMultiplier = Settings.RoccoRippleDashDurationMultiplier;
			if (MutableSettings != nullptr)
			{
				MutableSettings->RoccoRippleDashDurationMultiplier = 4.f;
			}

			Comp->OnHalfTime();   // clear the cooldown so E is available
			ATraceCharacter* RoccoPawn = Comp->GetOwningCharacter();
			Run->RoccoPawn = RoccoPawn;

			const bool bFired = Comp->TryActivate();

			ATraceRippleActor* Found = nullptr;
			int32 RippleCount = 0;
			for (TActorIterator<ATraceRippleActor> It(WorldPtr); It; ++It)
			{
				Found = *It;
				++RippleCount;
			}
			Run->Ripple = Found;

			Run->Current.Add(TEXT("E lays a Ripple (a separate cooldown from the dash pool)"),
				bFired && RippleCount == 1,
				FString::Printf(TEXT("fired=%d rippleActors=%d abilityCooldown=%.1fs dashCharges=%d"),
					bFired ? 1 : 0, RippleCount, Comp->GetActivatedCooldownRemaining(),
					(RoccoPawn != nullptr && RoccoPawn->GetTraceMovement() != nullptr)
						? RoccoPawn->GetTraceMovement()->GetDashCharges() : -1));

			// THE FIXTURE IS BUILT IDENTICALLY IN BOTH ARMS, ripple or no ripple. When the red arm
			// laid no path the entrance is still computed from the SAME pure function the ability
			// uses (ComputeRipplePath), and the victims are still stood in it — so the only
			// difference between the arms is whether the ability worked, which is the point.
			FVector Start = FVector::ZeroVector;
			{
				FVector PathDirection = FVector::ZeroVector;
				float PathLength = 0.f;
				if (Found != nullptr)
				{
					Start = Found->GetRippleStart();
				}
				else if (Rocco->ComputeRipplePath(Start, PathDirection, PathLength))
				{
					// Start is filled in by ComputeRipplePath.
				}
			}

			// §6: "any character, either team, may enter the ripple's start... The Core carrier can
			// use it."
			ATraceCharacter* Carrier = EnsureCarrierOtherThan(WorldPtr, RoccoPawn);
			Run->CarrierPawn = Carrier;

			ATraceCharacter* TestVictim = FindOtherAliveCharacter(WorldPtr, RoccoPawn, Carrier);
			Run->Victim = TestVictim;

			// Never teleport to the origin: if the entrance could not be computed at all, leave the
			// pawns where they are rather than dropping them out of the world and wrecking the NEXT
			// arm's fixture.
			if (!Start.IsNearlyZero())
			{
				// SPREAD THEM ACROSS THE ENTRANCE, do not stack them on Rocco. Three character
				// capsules sharing one point are depenetrated by the physics at several hundred
				// uu/s, which would be measured as propulsion the ripple did not supply. 60 uu
				// apart is comfortably inside the 140 uu entry radius and comfortably outside two
				// capsule radii.
				FVector AcrossPath = FVector::CrossProduct(
					(Found != nullptr) ? Found->GetRippleDirection() : FVector::ForwardVector,
					FVector::UpVector).GetSafeNormal();
				if (AcrossPath.IsNearlyZero())
				{
					AcrossPath = FVector::RightVector;
				}

				if (TestVictim != nullptr)
				{
					TestVictim->SetActorLocation(Start + AcrossPath * 60.f, false, nullptr, ETeleportType::TeleportPhysics);
					if (TestVictim->GetTraceMovement() != nullptr)
					{
						TestVictim->GetTraceMovement()->Velocity = FVector::ZeroVector;
					}
				}
				if (Carrier != nullptr)
				{
					Carrier->SetActorLocation(Start - AcrossPath * 60.f, false, nullptr, ETeleportType::TeleportPhysics);
					if (Carrier->GetTraceMovement() != nullptr)
					{
						Carrier->GetTraceMovement()->Velocity = FVector::ZeroVector;
					}
				}
			}

			Run->WorldTimeAtStageStart = WorldPtr->GetTimeSeconds();
			Run->Step = 2;
			ScheduleRocco(Run, 0.20f);
			return false;
		}

		case 2:
		{
			ATraceRippleActor* Found = Run->Ripple.Get();
			ATraceCharacter* TestVictim = Run->Victim.Get();
			ATraceCharacter* Carrier = Run->CarrierPawn.Get();

			const FVector Direction = (Found != nullptr) ? Found->GetRippleDirection() : FVector::ZeroVector;
			const float RideSpeed = (Found != nullptr) ? Found->GetRideSpeed() : 0.f;

			// --- the ride ---------------------------------------------------------------------------
			bool bRidingOk = false;
			FString RideDetail = TEXT("no ripple and no rider");
			if (Found != nullptr && TestVictim != nullptr && TestVictim->GetTraceMovement() != nullptr)
			{
				const FVector RiderVelocity = TestVictim->GetTraceMovement()->Velocity;
				const float AlongPath = FVector::DotProduct(RiderVelocity, Direction);
				bRidingOk = Found->IsRiding(TestVictim) && AlongPath > RideSpeed * 0.75f;
				RideDetail = FString::Printf(TEXT("%s riding=%d speedAlongPath=%.0f of %.0f uu/s"),
					*GetNameSafe(TestVictim), Found->IsRiding(TestVictim) ? 1 : 0, AlongPath, RideSpeed);
			}
			Run->Current.Add(TEXT("a player entering the START ring is propelled along the path"),
				bRidingOk, RideDetail);

			// --- §6: "Players holding guns can SHOOT while riding it, unlike a normal dash" ----------
			if (TestVictim != nullptr && TestVictim->GetTraceMovement() != nullptr)
			{
				const UTraceCharacterMovementComponent* Move = TestVictim->GetTraceMovement();
				Run->Current.Add(TEXT("a rider CAN shoot — the weapon gate is open, unlike a dash"),
					!Move->AreWeaponActionsBlocked() && !Move->IsDashing(),
					FString::Printf(TEXT("weaponActionsBlocked=%d isDashing=%d"),
						Move->AreWeaponActionsBlocked() ? 1 : 0, Move->IsDashing() ? 1 : 0));
			}

			// --- §6: "THE CORE CARRIER CAN USE IT" — the one place §4 must NOT refuse ---------------
			if (Carrier != nullptr)
			{
				const bool bCarrierRiding = (Found != nullptr) && Found->IsRiding(Carrier);
				Run->Current.Add(TEXT("the CORE CARRIER may ride (Beneficial passes the §4 choke point)"),
					bCarrierRiding,
					FString::Printf(TEXT("carrier=%s riding=%d chokePoint=%s"),
						*GetNameSafe(Carrier), bCarrierRiding ? 1 : 0,
						ATraceRippleActor::DescribeEntryRefusal(Carrier,
							(Run->Comp.Get() != nullptr) ? Run->Comp->GetOwningPlayerState() : nullptr)));
			}
			else
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ROCCO] arm %d: NO CORE CARRIER other than Rocco existed, so the '§6 carrier can ride' "
					     "assertion was NOT EXERCISED this arm."), Arm);
			}

			// --- MOVEMENT: set up the second jump ----------------------------------------------------
			ATraceCharacter* RoccoPawn = Run->RoccoPawn.Get();
			if (RoccoPawn != nullptr && RoccoPawn->GetTraceMovement() != nullptr)
			{
				UTraceCharacterMovementComponent* Move = RoccoPawn->GetTraceMovement();
				Move->SetMovementMode(MOVE_Falling);
				Move->Velocity = FVector(600.f, 0.f, 400.f);
				// Pump an input to the RIGHT so the redirect has a wish direction to turn onto. The
				// movement component computes Acceleration from this during its own tick, which is why
				// this is a stage of its own rather than a line before the call.
				// bForce = TRUE, and it is the whole reason this assertion can run at all. The
				// character-select screen calls SetGameInputSuppressed, which makes the controller
				// IGNORE move input — so the un-forced call silently added nothing and the
				// redirect reported itself NOT EXERCISED on two consecutive runs. bForce bypasses
				// IsMoveInputIgnored(); it changes nothing about the ability under test, only about
				// whether the fixture can reach it.
				RoccoPawn->AddMovementInput(FVector(0.f, 1.f, 0.f), 1.f, /*bForce*/ true);
			}

			Run->Step = 3;
			ScheduleRocco(Run, 0.06f);
			return false;
		}

		case 3:
		{
			// Keep the input alive across a couple of frames, then fire.
			ATraceCharacter* RoccoPawn = Run->RoccoPawn.Get();
			UTraceAbilityComponent* Comp = Run->Comp.Get();
			if (RoccoPawn == nullptr || Comp == nullptr || RoccoPawn->GetTraceMovement() == nullptr)
			{
				Run->Current.Add(TEXT("second jump: fixture"), false, TEXT("no pawn"));
				FinishRoccoArm(Run);
				ScheduleRocco(Run, 0.f);
				return false;
			}

			UTraceCharacterMovementComponent* Move = RoccoPawn->GetTraceMovement();
			Move->SetMovementMode(MOVE_Falling);
			Move->Velocity = FVector(600.f, 0.f, -200.f);   // falling, moving +X
			RoccoPawn->AddMovementInput(FVector(0.f, 1.f, 0.f), 1.f, /*bForce*/ true);

			Run->JumpVelocityBefore = Move->Velocity;
			Run->Step = 4;
			Run->JumpPumpFrames = 0;
			ScheduleRocco(Run, 0.f);
			return false;
		}

		case 4:
		{
			ATraceCharacter* RoccoPawn = Run->RoccoPawn.Get();
			UTraceAbilityComponent* Comp = Run->Comp.Get();
			UTraceAbilitySetRocco* Rocco = Run->Rocco.Get();
			if (RoccoPawn == nullptr || Comp == nullptr || Rocco == nullptr || RoccoPawn->GetTraceMovement() == nullptr)
			{
				Run->Current.Add(TEXT("second jump: fixture"), false, TEXT("no pawn"));
				FinishRoccoArm(Run);
				ScheduleRocco(Run, 0.f);
				return false;
			}

			UTraceCharacterMovementComponent* Move = RoccoPawn->GetTraceMovement();

			// PUMP THE INPUT EVERY FRAME until the movement component has turned it into an
			// acceleration. See the note on JumpPumpFrames: a single AddMovementInput is consumed
			// and then forgotten, which is why the first run of this harness read zero and reported
			// the redirect as NOT EXERCISED.
			Move->SetMovementMode(MOVE_Falling);
			Move->Velocity = FVector(600.f, 0.f, -200.f);
			RoccoPawn->AddMovementInput(FVector(0.f, 1.f, 0.f), 1.f, /*bForce*/ true);

			if (Move->GetCurrentAcceleration().IsNearlyZero() && Run->JumpPumpFrames++ < 60)
			{
				ScheduleRocco(Run, 0.f);   // next frame, input still held
				return false;
			}

			const FVector Before = Move->Velocity;
			const FVector Wish = Move->GetCurrentAcceleration();

			const bool bConsumed = Comp->HandleJumpPressed();
			const FVector After = Move->Velocity;
			Run->bJumpConsumed = bConsumed;

			// --- what the fixture CAN measure without a wish direction -----------------------------
			const bool bLifted = After.Z >= UTraceSettings::Get().RoccoSecondJumpZVelocity * 0.9f;
			Run->Current.Add(TEXT("the midair second jump fires, and lifts by its (small) Z"),
				bConsumed && bLifted,
				FString::Printf(TEXT("consumed=%d velocity (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f); the knob is %.0f uu/s"),
					bConsumed ? 1 : 0, Before.X, Before.Y, Before.Z, After.X, After.Y, After.Z,
					UTraceSettings::Get().RoccoSecondJumpZVelocity));

			const bool bSecondPress = Comp->HandleJumpPressed();
			Run->Current.Add(TEXT("only ONE extra jump per airtime"),
				!bSecondPress && !Rocco->IsSecondJumpAvailable(),
				FString::Printf(TEXT("secondPressConsumed=%d available=%d"),
					bSecondPress ? 1 : 0, Rocco->IsSecondJumpAvailable() ? 1 : 0));

			// --- the half this fixture CANNOT reach, said out loud ---------------------------------
			// §6's actual point is the DIRECTION CHANGE, and the redirect turns onto the movement
			// component's Acceleration. This harness could not make Acceleration non-zero: the input
			// is NOT being ignored (that flag is reported below) and yet sixty pumped frames of
			// AddMovementInput(bForce = true) produced nothing. Asserting the redirect anyway would
			// be this file asserting its own arithmetic, so the run RECORDS THE GAP instead — and
			// the verdict prints it, because "could not be reached" and "passed" are different
			// answers and this project has been bitten by conflating them.
			if (Wish.IsNearlyZero())
			{
				++Run->Current.NotExercised;
				Run->Current.NotExercisedNotes.Add(FString::Printf(
					TEXT("the midair REDIRECT (§6's 'change direction instantly'): Acceleration stayed zero after "
					     "%d pumped frames (moveInputIgnored=%d), so the fixture never supplied a wish direction"),
					Run->JumpPumpFrames, RoccoPawn->IsMoveInputIgnored() ? 1 : 0));
			}
			else
			{
				const bool bRedirected = FMath::Abs(After.Y) > FMath::Abs(After.X);
				Run->Current.Add(TEXT("second jump changes direction midair, INSTANTLY (§6's point)"),
					bConsumed && bRedirected,
					FString::Printf(TEXT("wish=(%s) planar (%.0f,%.0f) -> (%.0f,%.0f)"),
						*Wish.GetSafeNormal().ToCompactString(), Before.X, Before.Y, After.X, After.Y));
			}

			// Restore what this arm changed, then move on.
			if (MutableSettings != nullptr)
			{
				MutableSettings->RoccoRippleDashDurationMultiplier = Run->SavedRippleDurationMultiplier;
				MutableSettings->RoccoHeadshotSpeedDurationSeconds = Run->SavedStackDuration;
			}
			if (ATraceRippleActor* Found = Run->Ripple.Get())
			{
				Found->Destroy();
			}
			Comp->OnHalfTime();

			FinishRoccoArm(Run);
			ScheduleRocco(Run, 0.20f);
			return false;
		}

		default:
			ReportRoccoVerdict(Run);
			return false;
		}
	}

	void RunRoccoVerify(const TArray<FString>& Args)
	{
		TSharedPtr<FRoccoRun> Run = MakeShared<FRoccoRun>();
		if (Args.Num() >= 1)
		{
			Run->ArmsToRun.Add(FCString::Atoi(*Args[0]));
		}
		else
		{
			Run->ArmsToRun.Add(0);   // RED FIRST, always
			Run->ArmsToRun.Add(1);
		}

		UE_LOG(LogTraceGame, Display, TEXT("[ROCCO] starting: arms %s (0 = RED, abilities off; 1 = shipped)."),
			*FString::JoinBy(Run->ArmsToRun, TEXT(","), [](int32 A) { return FString::FromInt(A); }));

		ScheduleRocco(Run, 0.f);
	}

	FAutoConsoleCommand CmdRoccoVerify(
		TEXT("Trace.Rocco.Verify"),
		TEXT("Dev only, server only. SPEC v14 §6 ROCCO. Runs a RED arm (his three abilities switched off) and "
		     "then the shipped arm, in one process: the headshot stack and its single shared window, the Ripple "
		     "(including a Core carrier riding it and a rider still being able to shoot), and the midair second "
		     "jump. Reports INVALID if the red arm does not reproduce or the world clock is frozen."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunRoccoVerify));

	// =============================================================================================
	// CHUT
	// =============================================================================================

	struct FChutRun
	{
		TArray<int32> ArmsToRun;
		int32 ArmIndex = 0;
		int32 Step = 0;
		double NextRealTime = 0.0;

		TArray<FArmLog> Results;
		FArmLog Current;

		TWeakObjectPtr<UTraceAbilityComponent> Comp;
		TWeakObjectPtr<UTraceAbilitySetChut> Chut;
		TWeakObjectPtr<ATraceCharacter> ChutPawn;
		TWeakObjectPtr<ATraceCharacter> Victim;
		TWeakObjectPtr<ATraceCharacter> CarrierPawn;

		FVector BashDirection = FVector::ZeroVector;
		FVector VictimVelocityBefore = FVector::ZeroVector;
		FVector CarrierVelocityBefore = FVector::ZeroVector;
		bool bBashEarlyRefused = false;
		bool bBashLateApplied = false;
		bool bCarrierBashApplied = false;
		bool bSavedFriendlyFire = false;
		float WorldTimeAtStageStart = 0.f;

		/** See the twin on FRoccoRun: -ExecCmds fires long before a pawn exists. */
		int32 SetupAttemptsLeft = 40;
	};

	/**
	 * arm 1  the shipped build.
	 * arm 0  RED: Chut's three abilities removed, the carrier rule left alone.
	 * arm 2  RED: the §4 carrier rule removed, Chut's abilities left alone — the ONLY way to prove
	 *        the carrier assertion's fixture reaches a launch at all.
	 */
	void ApplyChutArm(int32 Arm)
	{
		const int32 AbilitiesOn = (Arm == 0) ? 0 : 1;
		SetCVarInt(TEXT("Trace.Chut.KnifeBuffEnabled"), AbilitiesOn);
		SetCVarInt(TEXT("Trace.Chut.BashEnabled"), AbilitiesOn);
		SetCVarInt(TEXT("Trace.Chut.ChudEnabled"), AbilitiesOn);
		SetCVarInt(TEXT("Trace.Ability.CarrierImmune"), (Arm == 2) ? 0 : 1);
	}

	bool TickChutRun(TSharedPtr<FChutRun> Run);

	void ScheduleChut(TSharedPtr<FChutRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + DelaySeconds;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickChutRun(Run);
			}), 0.f);
	}

	void FinishChutArm(TSharedPtr<FChutRun> Run)
	{
		Run->Current.Arm = Run->ArmsToRun[Run->ArmIndex];
		Run->Results.Add(Run->Current);
		Run->Current = FArmLog();
		++Run->ArmIndex;
		Run->Step = 0;
	}

	void ReportChutVerdict(TSharedPtr<FChutRun> Run)
	{
		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[CHUT] ===== spec v14 §6 CHUT — knife %0.f front / %0.f back (standard %0.f / %0.f), bash %.0f uu/s "
			     "in the final %.0f%% of the dash within %.0f uu, Chud −%.0f%% for %.1fs / %.0fs cooldown ====="),
			Settings.ChutKnifeFrontDamage, Settings.ChutKnifeBackDamage,
			TraceMelee::GetFrontDamage(), TraceMelee::GetBackstabDamage(),
			Settings.ChutBashKnockbackSpeed, Settings.ChutBashEndFraction * 100.f, Settings.ChutBashRadiusUU,
			Settings.ChudDamageReduction * 100.f, Settings.ChudDurationSeconds, Settings.ChudCooldownSeconds);

		const FArmLog* RedAbilities = nullptr;
		const FArmLog* RedCarrier = nullptr;
		const FArmLog* Green = nullptr;
		for (const FArmLog& Log : Run->Results)
		{
			ReportArm(TEXT("CHUT"), Log);
			if (Log.Arm == 0) { RedAbilities = &Log; }
			if (Log.Arm == 2) { RedCarrier = &Log; }
			if (Log.Arm == 1) { Green = &Log; }
		}

		if (Green == nullptr || RedAbilities == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[CHUT] VERDICT: INVALID — an arm did not run."));
			return;
		}
		if (Green->bInvalid || RedAbilities->bInvalid)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[CHUT] VERDICT: INVALID — %s"),
				Green->bInvalid ? *Green->InvalidReason : *RedAbilities->InvalidReason);
			return;
		}

		const int32 RedFailures = RedAbilities->CountFailed();
		const int32 GreenFailures = Green->CountFailed();
		const int32 CarrierRedFailures = (RedCarrier != nullptr) ? RedCarrier->CountFailed() : -1;

		if (RedFailures == 0)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CHUT] VERDICT: INVALID — the RED arm did not reproduce (0 failures with Chut's abilities "
				     "switched off). Arm 1's green means nothing."));
			return;
		}

		if (CarrierRedFailures == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CHUT] arm 2 (carrier rule removed) did not reproduce — either there was no Core carrier to "
				     "bash, or the bash never reached a launch. The 'bash does not touch the carrier' assertion is "
				     "therefore UNPROVEN, not proven."));
		}

		if (GreenFailures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[CHUT] VERDICT: PASS — red arm reproduced %d failure(s); carrier red arm reproduced %d; the "
				     "shipped arm passed all %d."),
				RedFailures, CarrierRedFailures, Green->Checks.Num());
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[CHUT] VERDICT: *** FAIL *** — %d of %d assertions failed on the shipped arm (red arm "
				     "reproduced %d, so the fixture is sound)."),
				GreenFailures, Green->Checks.Num(), RedFailures);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[CHUT] carrier ability-damage alarm = %d (must be 0)."),
			TraceAbility::GetCarrierAbilityDamageHitCount());
	}

	bool TickChutRun(TSharedPtr<FChutRun> Run)
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[CHUT] no authoritative world — server only."));
			return false;
		}

		if (!Run->ArmsToRun.IsValidIndex(Run->ArmIndex))
		{
			ReportChutVerdict(Run);
			return false;
		}

		const int32 Arm = Run->ArmsToRun[Run->ArmIndex];
		const bool bAbilitiesOn = (Arm != 0);
		const bool bCarrierRuleOn = (Arm != 2);
		const UTraceSettings& Settings = UTraceSettings::Get();
		UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>();

		switch (Run->Step)
		{
		case 0:
		{
			ApplyChutArm(Arm);

			UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
			if (Comp == nullptr || Comp->GetOwningCharacter() == nullptr
				|| FindOtherAliveCharacter(WorldPtr, Comp->GetOwningCharacter()) == nullptr)
			{
				if (Run->SetupAttemptsLeft-- > 0)
				{
					ScheduleChut(Run, 1.0f);   // no player pawn, or no second pawn to hit, yet
					return false;
				}
				Run->Current.Invalidate(TEXT("no non-bot player with a pawn AND a second living pawn appeared "
					"inside the 40s budget"));
				FinishChutArm(Run);
				ScheduleChut(Run, 0.f);
				return false;
			}

			Comp->ServerSetCharacter(ETraceCharacterId::Chut);
			Comp->OnHalfTime();

			UTraceAbilitySetChut* Chut = Comp->GetAbilitySetAs<UTraceAbilitySetChut>();
			ATraceCharacter* ChutPawn = Comp->GetOwningCharacter();
			Run->Comp = Comp;
			Run->Chut = Chut;
			Run->ChutPawn = ChutPawn;

			if (Chut == nullptr || ChutPawn == nullptr)
			{
				Run->Current.Invalidate(FString::Printf(
					TEXT("character set is %s and pawn is %s — the framework did not equip Chut"),
					*GetNameSafe(Comp->GetAbilitySet()), *GetNameSafe(ChutPawn)));
				FinishChutArm(Run);
				ScheduleChut(Run, 0.f);
				return false;
			}

			ATraceCharacter* Other = FindOtherAliveCharacter(WorldPtr, ChutPawn);
			if (Other == nullptr)
			{
				Run->Current.Invalidate(TEXT("no second living pawn — nothing to hit and nothing to be hit by"));
				FinishChutArm(Run);
				ScheduleChut(Run, 0.f);
				return false;
			}

			// ---- PASSIVE: the knife --------------------------------------------------------------
			// Both go through UTraceAbilityComponent::ModifyDamageThroughPassives — the same function
			// the melee slice is asked to call — with the CAUSES TraceMelee itself produces.
			const float FrontOut = RunDamagePipeline(TraceMelee::GetFrontDamage(), ChutPawn, Other,
				TraceMelee::GetKnifeKillCause(), /*bMelee*/ true, /*bHeadshot*/ false);
			Run->Current.Add(TEXT("Chut's knife does 50 from the FRONT (standard is 30)"),
				NearlyEqual(FrontOut, Settings.ChutKnifeFrontDamage, 0.01f),
				FString::Printf(TEXT("%.0f -> %.0f (expected %.0f)"), TraceMelee::GetFrontDamage(), FrontOut,
					Settings.ChutKnifeFrontDamage));

			const float BackOut = RunDamagePipeline(TraceMelee::GetBackstabDamage(), ChutPawn, Other,
				TraceMelee::GetBackstabKillCause(), true, false);
			Run->Current.Add(TEXT("the 60-degree BACK zone stays at 100 [v14 §6 ASSUMPTION]"),
				NearlyEqual(BackOut, TraceMelee::GetBackstabDamage(), 0.01f),
				FString::Printf(TEXT("%.0f -> %.0f"), TraceMelee::GetBackstabDamage(), BackOut));

			// A Mannequin swinging the same knife must be untouched — the control that proves the
			// substitution is Chut's passive and not something the pipeline does to every knife.
			const float MannequinKnife = RunDamagePipeline(TraceMelee::GetFrontDamage(), Other, ChutPawn,
				TraceMelee::GetKnifeKillCause(), true, false);
			Run->Current.Add(TEXT("a characterless Mannequin's knife is unchanged (control)"),
				NearlyEqual(MannequinKnife, TraceMelee::GetFrontDamage(), 0.01f),
				FString::Printf(TEXT("%.0f -> %.0f"), TraceMelee::GetFrontDamage(), MannequinKnife));

			// ---- ACTIVATED: Chud ------------------------------------------------------------------
			const bool bChudFired = Comp->TryActivate();
			const float Expected = 30.f * (1.f - Settings.ChudDamageReduction);

			const float BodyShot = RunDamagePipeline(30.f, Other, ChutPawn, FName(TEXT("Bullet")), false, false);
			Run->Current.Add(TEXT("Chud takes 30% off a BODY SHOT"),
				NearlyEqual(BodyShot, Expected, 0.01f),
				FString::Printf(TEXT("chudUp=%d (%.1fs) 30 -> %.1f (expected %.1f)"),
					Chut->IsChudActive() ? 1 : 0, Chut->GetChudSecondsRemaining(), BodyShot, Expected));

			const float MeleeIn = RunDamagePipeline(30.f, Other, ChutPawn, TraceMelee::GetKnifeKillCause(), true, false);
			Run->Current.Add(TEXT("Chud takes 30% off a MELEE"),
				NearlyEqual(MeleeIn, Expected, 0.01f),
				FString::Printf(TEXT("30 -> %.1f (expected %.1f)"), MeleeIn, Expected));

			const float HeadIn = RunDamagePipeline(100.f, Other, ChutPawn, FName(TEXT("Headshot")), false, true);
			Run->Current.Add(TEXT("Chud does NOT touch a HEADSHOT [v14 §6 ASSUMPTION] (control)"),
				NearlyEqual(HeadIn, 100.f, 0.01f),
				FString::Printf(TEXT("100 -> %.1f"), HeadIn));

			const float TrailIn = RunDamagePipeline(1000.f, Other, ChutPawn, FName(TEXT("Trail")), false, false);
			Run->Current.Add(TEXT("Chud does NOT touch a TRACE death [v14 §6 ASSUMPTION] (control)"),
				NearlyEqual(TrailIn, 1000.f, 0.01f),
				FString::Printf(TEXT("1000 -> %.1f"), TrailIn));

			// "Does not stack": a knife kill REFRESHES the window, it does not double it.
			if (Chut->IsChudActive())
			{
				const float BeforeRefresh = Chut->GetChudSecondsRemaining();
				Chut->OnKill(Other, TraceMelee::GetKnifeKillCause(), false);
				const float AfterRefresh = Chut->GetChudSecondsRemaining();
				Run->Current.Add(TEXT("a knife kill REFRESHES Chud and never stacks it"),
					AfterRefresh <= Settings.ChudDurationSeconds + 0.05f && AfterRefresh >= BeforeRefresh - 0.05f,
					FString::Printf(TEXT("%.2fs -> %.2fs (ceiling %.1fs)"),
						BeforeRefresh, AfterRefresh, Settings.ChudDurationSeconds));
			}

			UE_LOG(LogTraceGame, Verbose, TEXT("[CHUT] arm %d: TryActivate returned %d."), Arm, bChudFired ? 1 : 0);

			// ---- MOVEMENT: the bash ---------------------------------------------------------------
			// Friendly fire is forced ON for this section so that a SameTeam refusal can never be
			// mistaken for the carrier rule or for the bash being broken. Restored at the end.
			Run->bSavedFriendlyFire = Settings.bFriendlyFire;
			if (MutableSettings != nullptr)
			{
				MutableSettings->bFriendlyFire = true;
			}

			Run->BashDirection = FVector(1.f, 0.f, 0.f);
			Run->Victim = Other;

			Other->SetActorLocation(ChutPawn->GetActorLocation() + Run->BashDirection * 100.f,
				false, nullptr, ETeleportType::TeleportPhysics);
			if (Other->GetTraceMovement() != nullptr)
			{
				// STOP THE VICTIM FIRST. The first run measured 422 uu/s of ambient bot movement along
				// the bash axis against a 400 uu/s threshold — close enough that a wandering bot could
				// have been read as a knockback. Starting from rest makes the measurement the bash and
				// nothing else.
				Other->GetTraceMovement()->Velocity = FVector::ZeroVector;
			}
			Run->VictimVelocityBefore = FVector::ZeroVector;

			// §6 says the END of the dash, so an early contact must be REFUSED, not clamped in.
			Run->bBashEarlyRefused = !Chut->TryBash(Other, 0.10f, Run->BashDirection);
			Run->bBashLateApplied  =  Chut->TryBash(Other, 0.95f, Run->BashDirection);

			Run->WorldTimeAtStageStart = WorldPtr->GetTimeSeconds();
			Run->Step = 1;
			ScheduleChut(Run, 0.30f);
			return false;
		}

		case 1:
		{
			UTraceAbilitySetChut* Chut = Run->Chut.Get();
			ATraceCharacter* ChutPawn = Run->ChutPawn.Get();
			ATraceCharacter* TestVictim = Run->Victim.Get();
			if (Chut == nullptr || ChutPawn == nullptr || TestVictim == nullptr)
			{
				Run->Current.Invalidate(TEXT("a pawn went away mid-run"));
				FinishChutArm(Run);
				ScheduleChut(Run, 0.f);
				return false;
			}

			const float WorldAdvanced = WorldPtr->GetTimeSeconds() - Run->WorldTimeAtStageStart;
			if (WorldAdvanced < 0.05f)
			{
				Run->Current.Invalidate(FString::Printf(
					TEXT("the WORLD CLOCK advanced only %.3fs across a 0.30s wall-clock wait — a launch cannot be "
					     "measured in a frozen world"), WorldAdvanced));
				FinishChutArm(Run);
				ScheduleChut(Run, 0.f);
				return false;
			}

			const FVector VictimVelocity = (TestVictim->GetTraceMovement() != nullptr)
				? TestVictim->GetTraceMovement()->Velocity : FVector::ZeroVector;
			const float AlongTravel = FVector::DotProduct(VictimVelocity, Run->BashDirection);
			const bool bKnocked = AlongTravel > Settings.ChutBashKnockbackSpeed * 0.4f;

			Run->Current.Add(TEXT("the END of the dash knocks a player along the direction of travel"),
				Run->bBashLateApplied && bKnocked,
				FString::Printf(TEXT("applied=%d speedAlongTravel=%.0f of %.0f uu/s [world advanced %.2fs]"),
					Run->bBashLateApplied ? 1 : 0, AlongTravel, Settings.ChutBashKnockbackSpeed, WorldAdvanced));

			{
				Run->Current.Add(TEXT("EARLY in the dash the bash is refused, not clamped in"),
					Run->bBashEarlyRefused,
					FString::Printf(TEXT("progress 0.10 refused=%d (window starts at %.2f)"),
						Run->bBashEarlyRefused ? 1 : 0, 1.f - Settings.ChutBashEndFraction));
			}

			// ---- the carrier: FORCE one to exist, then give the grant a moment to settle ------------
			//
			// GET THE PREVIOUS VICTIM OUT OF THE WAY FIRST. The carrier is about to be stood on the
			// exact spot the bashed victim occupies, and two character capsules sharing a point are
			// depenetrated by the physics at several hundred uu/s — which the first run of this
			// harness measured as 966 uu/s and charged to the bash. Moving the victim aside is what
			// makes "the carrier did not move" a statement about the bash rather than about capsules.
			TestVictim->SetActorLocation(ChutPawn->GetActorLocation() + FVector(0.f, 1500.f, 0.f),
				false, nullptr, ETeleportType::TeleportPhysics);
			if (TestVictim->GetTraceMovement() != nullptr)
			{
				TestVictim->GetTraceMovement()->Velocity = FVector::ZeroVector;
			}

			Run->CarrierPawn = EnsureCarrierOtherThan(WorldPtr, ChutPawn, TestVictim);

			Run->WorldTimeAtStageStart = WorldPtr->GetTimeSeconds();
			Run->Step = 2;
			ScheduleChut(Run, 0.30f);
			return false;
		}

		case 2:
		{
			UTraceAbilitySetChut* Chut = Run->Chut.Get();
			ATraceCharacter* ChutPawn = Run->ChutPawn.Get();
			ATraceCharacter* Carrier = EnsureCarrierOtherThan(WorldPtr, ChutPawn, Run->Victim.Get());
			Run->CarrierPawn = Carrier;

			if (Chut != nullptr && ChutPawn != nullptr && Carrier != nullptr)
			{
				Carrier->SetActorLocation(ChutPawn->GetActorLocation() + Run->BashDirection * 100.f,
					false, nullptr, ETeleportType::TeleportPhysics);
				if (Carrier->GetTraceMovement() != nullptr)
				{
					Carrier->GetTraceMovement()->Velocity = FVector::ZeroVector;
				}
				Run->CarrierVelocityBefore = FVector::ZeroVector;
				Run->bCarrierBashApplied = Chut->TryBash(Carrier, 0.95f, Run->BashDirection);
			}

			// SHORT WAIT, AND THE MEASUREMENT IS VERTICAL. See the note in the case below: the
			// carrier is a bot and it RUNS, so its planar speed cannot tell a bash from a jog. The
			// bash's up bias can, but gravity erases it in a third of a second, so this stage is
			// three frames rather than three hundred milliseconds.
			Run->Step = 3;
			ScheduleChut(Run, 0.06f);
			return false;
		}

		case 3:
		{
			ATraceCharacter* Carrier = Run->CarrierPawn.Get();
			if (Carrier != nullptr)
			{
				const FVector CarrierVelocity = (Carrier->GetTraceMovement() != nullptr)
					? Carrier->GetTraceMovement()->Velocity : FVector::ZeroVector;
				const float AlongTravel = FVector::DotProduct(CarrierVelocity, Run->BashDirection);

				// *** THE MEASUREMENT IS THE BASH'S UP BIAS, NOT ITS PLANAR SPEED. ***
				// The first two runs of this harness read 967-972 uu/s along the bash axis with the
				// bash REFUSED, and charged it to the bash. It was the carrier bot RUNNING: a Core
				// carrier's ground speed is ~970 uu/s, which is indistinguishable from a 1000 uu/s
				// knockback. The up bias (0.3 x 1000 = 300 uu/s) is not something a bot on the floor
				// ever produces, so it separates the two cleanly — and it is measured three frames
				// after the launch, before gravity has taken it away.
				const float UpwardSpeed = CarrierVelocity.Z;
				const bool bCarrierKnocked = UpwardSpeed > Settings.ChutBashKnockbackSpeed
					* FMath::Max(0.05f, Settings.ChutBashUpBias) * 0.5f;

				// *** §6: "NO EFFECT ON THE CORE CARRIER" — which is §4's Control answer. ***
				// Expected: shipped and abilities-off arms refuse; arm 2, with the carrier rule
				// removed, MUST launch them, or this assertion's fixture never reached a launch.
				Run->Current.Add(TEXT("the bash has NO EFFECT ON THE CORE CARRIER (§4 choke point)"),
					!Run->bCarrierBashApplied && !bCarrierKnocked,
					FString::Printf(TEXT("carrier=%s applied=%d UPWARD=%.0f (a bash gives %.0f, running gives ~0) "
					                     "planar=%.0f [a carrier RUNS at ~970, hence the vertical test] "
					                     "carrierRule=%s abilities=%s"),
						*GetNameSafe(Carrier), Run->bCarrierBashApplied ? 1 : 0, UpwardSpeed,
						Settings.ChutBashKnockbackSpeed * Settings.ChutBashUpBias, AlongTravel,
						bCarrierRuleOn ? TEXT("ON") : TEXT("REMOVED"), bAbilitiesOn ? TEXT("ON") : TEXT("OFF")));
			}
			else
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[CHUT] arm %d: NO CORE CARRIER other than Chut existed, so the carrier assertion was NOT "
					     "EXERCISED this arm."), Arm);
			}

			if (MutableSettings != nullptr)
			{
				MutableSettings->bFriendlyFire = Run->bSavedFriendlyFire;
			}
			if (UTraceAbilityComponent* Comp = Run->Comp.Get())
			{
				Comp->OnHalfTime();
			}

			FinishChutArm(Run);
			ScheduleChut(Run, 0.20f);
			return false;
		}

		default:
			ReportChutVerdict(Run);
			return false;
		}
	}

	void RunChutVerify(const TArray<FString>& Args)
	{
		TSharedPtr<FChutRun> Run = MakeShared<FChutRun>();
		if (Args.Num() >= 1)
		{
			Run->ArmsToRun.Add(FCString::Atoi(*Args[0]));
		}
		else
		{
			Run->ArmsToRun.Add(0);   // RED: abilities off
			Run->ArmsToRun.Add(2);   // RED: §4 carrier rule removed
			Run->ArmsToRun.Add(1);   // shipped
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[CHUT] starting: arms %s (0 = RED abilities off, 2 = RED carrier rule removed, 1 = shipped)."),
			*FString::JoinBy(Run->ArmsToRun, TEXT(","), [](int32 A) { return FString::FromInt(A); }));

		ScheduleChut(Run, 0.f);
	}

	FAutoConsoleCommand CmdChutVerify(
		TEXT("Trace.Chut.Verify"),
		TEXT("Dev only, server only. SPEC v14 §6 CHUT. Runs two RED arms (his abilities off; then the §4 carrier "
		     "rule removed) and then the shipped arm, in one process: the 50-damage front knife, Chud's reduction "
		     "and its headshot/trace exclusions, and the end-of-dash bash including its refusal on the Core "
		     "carrier. Reports INVALID if a red arm does not reproduce or the world clock is frozen."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunChutVerify));

	// =============================================================================================
	// Trace.Rocco.RippleShot — the picture spec §6 asks to be able to read
	//
	// §6: "a short series of rings along the path, with the STARTING RING IN A DIFFERENT COLOUR so it
	// is obvious where to take it." That is a claim about what a player SEES, and the only way to
	// check it is to look. This lays a ripple through the SHIPPING activation path (TryActivate ->
	// ActivateAbility -> ATraceRippleActor), then moves the OBSERVER — not the ripple — behind and
	// above the entrance so the whole path is in frame, and fires HighResShot.
	//
	// Moving the camera also takes Rocco out of his own entry radius, so nothing is riding and the
	// rings sit still for the exposure.
	// =============================================================================================

	struct FRippleShotRun
	{
		int32 AttemptsLeft = 40;
		int32 Step = 0;
		double NextRealTime = 0.0;
		TWeakObjectPtr<ATraceRippleActor> Ripple;
	};

	bool TickRippleShot(TSharedPtr<FRippleShotRun> Run);

	void ScheduleRippleShot(TSharedPtr<FRippleShotRun> Run, float DelaySeconds)
	{
		Run->NextRealTime = FPlatformTime::Seconds() + DelaySeconds;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float) -> bool
			{
				if (FPlatformTime::Seconds() < Run->NextRealTime)
				{
					return true;
				}
				return TickRippleShot(Run);
			}), 0.f);
	}

	bool TickRippleShot(TSharedPtr<FRippleShotRun> Run)
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[RippleShot] no authoritative world."));
			return false;
		}

		if (Run->Step == 0)
		{
			UTraceAbilityComponent* Comp = FindHumanAbilityComponent(WorldPtr);
			ATraceCharacter* RoccoPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (Comp == nullptr || RoccoPawn == nullptr)
			{
				if (Run->AttemptsLeft-- > 0)
				{
					ScheduleRippleShot(Run, 1.0f);
					return false;
				}
				UE_LOG(LogTraceGame, Error, TEXT("[RippleShot] no player pawn inside the budget."));
				return false;
			}

			Comp->ServerSetCharacter(ETraceCharacterId::Rocco);
			Comp->OnHalfTime();   // clear the cooldown so E is available

			const bool bFired = Comp->TryActivate();

			ATraceRippleActor* Found = nullptr;
			for (TActorIterator<ATraceRippleActor> It(WorldPtr); It; ++It)
			{
				Found = *It;
			}
			Run->Ripple = Found;

			if (Found == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[RippleShot] TryActivate returned %d but no ripple exists."),
					bFired ? 1 : 0);
				return false;
			}

			// Stand the observer BEHIND and ABOVE the entrance, looking down the path.
			const FVector Start = Found->GetRippleStart();
			const FVector Direction = Found->GetRippleDirection();
			const FVector Eye = Start - Direction * 650.f + FVector(0.f, 0.f, 260.f);

			RoccoPawn->SetActorLocation(Eye, false, nullptr, ETeleportType::TeleportPhysics);
			if (AController* PawnController = RoccoPawn->GetController())
			{
				PawnController->SetControlRotation((Start - Eye).Rotation());
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[RippleShot] ripple laid: start (%s) dir (%s) length %.0f uu. Observer at (%s) looking at the "
				     "START ring. Screenshot in 1.0s."),
				*Start.ToCompactString(), *Direction.ToCompactString(), Found->GetPathLength(),
				*Eye.ToCompactString());

			Run->Step = 1;
			ScheduleRippleShot(Run, 1.0f);
			return false;
		}

		if (GEngine != nullptr)
		{
			GEngine->Exec(WorldPtr, TEXT("HighResShot 1280x720"));
			UE_LOG(LogTraceGame, Display,
				TEXT("[RippleShot] HighResShot issued. Rings still alive = %d."),
				Run->Ripple.IsValid() ? 1 : 0);
		}
		return false;
	}

	FAutoConsoleCommand CmdRippleShot(
		TEXT("Trace.Rocco.RippleShot"),
		TEXT("Dev only, server only. Lays a Ripple through the shipping activation path, moves the observer behind "
		     "the entrance and takes a screenshot, so spec v14 §6's 'starting ring in a different colour' can "
		     "actually be READ rather than asserted."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ScheduleRippleShot(MakeShared<FRippleShotRun>(), 0.f);
		}));

	// =============================================================================================
	// A plain state dump for both, for reading a live match
	// =============================================================================================

	FAutoConsoleCommand CmdDumpCharacters(
		TEXT("Trace.Characters.DumpRoccoChut"),
		TEXT("Dev only. What Rocco's stack and Chut's Chud currently say, on whichever machine this runs on."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UWorld* WorldPtr = FindAuthoritativeWorld();
			if (WorldPtr == nullptr && GEngine != nullptr)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.World() != nullptr && Context.World()->IsGameWorld())
					{
						WorldPtr = Context.World();
						break;
					}
				}
			}
			if (WorldPtr == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[RoccoChut] no game world."));
				return;
			}

			const AGameStateBase* GS = WorldPtr->GetGameState();
			if (GS == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[RoccoChut] no game state."));
				return;
			}

			int32 RippleCount = 0;
			for (TActorIterator<ATraceRippleActor> It(WorldPtr); It; ++It)
			{
				const ATraceRippleActor* Ripple = *It;
				UE_LOG(LogTraceGame, Display,
					TEXT("[RoccoChut] ripple: start (%s) dir (%s) length %.0f uu ride %.0f uu/s entry %.0f uu "
					     "riders now=%d ever=%d expires %.2f"),
					*Ripple->GetRippleStart().ToCompactString(), *Ripple->GetRippleDirection().ToCompactString(),
					Ripple->GetPathLength(), Ripple->GetRideSpeed(), Ripple->GetEntryRadius(),
					Ripple->GetRiderCount(), Ripple->GetLifetimeRiderCount(), Ripple->GetExpireMatchTime());
				++RippleCount;
			}

			for (APlayerState* PS : GS->PlayerArray)
			{
				const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PS);
				if (Comp == nullptr)
				{
					continue;
				}
				if (const UTraceAbilitySetRocco* Rocco = Comp->GetAbilitySetAs<UTraceAbilitySetRocco>())
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[RoccoChut] %-22s ROCCO stacks=%d (+%.0f%% speed) window=%.2fs secondJumpReady=%d "
						     "cooldown=%.1fs"),
						*GetNameSafe(PS), Rocco->GetLiveStackCount(),
						(Rocco->GetMoveSpeedMultiplier() - 1.f) * 100.f, Rocco->GetStackSecondsRemaining(),
						Rocco->IsSecondJumpAvailable() ? 1 : 0, Comp->GetActivatedCooldownRemaining());
				}
				else if (const UTraceAbilitySetChut* Chut = Comp->GetAbilitySetAs<UTraceAbilitySetChut>())
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[RoccoChut] %-22s CHUT chud=%d (%.2fs) bashedThisDash=%d cooldown=%.1fs"),
						*GetNameSafe(PS), Chut->IsChudActive() ? 1 : 0, Chut->GetChudSecondsRemaining(),
						Chut->GetBashedThisDashCount(), Comp->GetActivatedCooldownRemaining());
				}
			}

			UE_LOG(LogTraceGame, Display, TEXT("[RoccoChut] %d live ripple(s)."), RippleCount);
		}));
}   // namespace TraceCharacterVerify

#endif   // !UE_BUILD_SHIPPING
