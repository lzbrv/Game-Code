// Trace — the evidence for ELLE (spec v18 §2), and for the founding invariant as her gates can break
// it.
//
// ===================================================================================================
// TWO COMMANDS. ONE OF THEM IS THE ONE THAT MATTERS.
// ===================================================================================================
//
//   Trace.Elle.CarrierTest   *** THE FOUNDING INVARIANT, TWO ARMS, RED FIRST. ***
//                            §2: "A Core carrier must not be teleported by an enemy gate." This puts a
//                            live enemy CARRIER and a live enemy NON-CARRIER inside the same mouth of
//                            the same pair, in the same frames, twice: once with the carrier rule
//                            REMOVED (Trace.Ability.CarrierImmune 0 — the carrier MUST be moved, or
//                            the fixture never reached the teleport and its green would mean nothing)
//                            and once with the shipped rule (the carrier must NOT be moved, while the
//                            non-carrier still is).
//
//   Trace.Elle.Verify        The rest, end to end: a real pass raises the cloak and a KILL-loss does
//                            not; the cloak replicates as a fact and comes down again; SNAP's
//                            two-press shape, its 4 s window, its 8 s pair and its 35 s cooldown; a
//                            team-mate stepping into a mouth arrives at the other one; and the
//                            slide-jump derivation, which is Elle-only and is NOT wired in (the
//                            command says so, loudly, rather than reporting a green for a passive
//                            nothing calls).
//
// ===================================================================================================
// WHY THE CARRIER TEST IS JUDGED ON A TALLY AND NOT ON WHERE THE PAWN ENDED UP
// ===================================================================================================
//
// The same reason Oyster's is (see the block at the top of TraceMaceOysterVerify.cpp). "The carrier
// did not move" is true on a build with NO carrier rule at all whenever the fixture failed to put
// them in the mouth, or the gates expired early, or the pair never formed — and this project has
// twice shipped a harness whose green meant "the test did not run".
//
// So ATraceElleGate counts every DECISION at the point it is taken, split by whether the subject was
// holding the Core at that instant (TraceElle::CarrierTally / OtherTally). The red arm must make
// CarrierTally().TeleportsCommitted climb; the shipped rule must hold it at zero AND show the refusal
// filed under RefusedByChokePoint — because "nobody was ever in the mouth" and "the choke point said
// no" are different facts and only one of them is the invariant working.
//
// TIMING IS REAL TIME (FPlatformTime::Seconds). The character-select screen pauses the world, and a
// harness waiting on world time waits forever. Both commands unpause explicitly and say so.

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

#include "Abilities/Characters/TraceAbilitySetElle.h"
#include "Abilities/Characters/TraceElleGate.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceElleVerify
{
	// =============================================================================================
	// Shared fixture helpers — deliberately the same shapes TraceMaceOysterVerify.cpp uses, so that
	// two harnesses staging the same match do not disagree about what "the human" or "an enemy" is.
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

	/** The select screen pauses the world; a paused world makes every measurement below a false zero. */
	void UnpauseAndReport(UWorld* WorldPtr, const TCHAR* Tag)
	{
		if (WorldPtr == nullptr || !WorldPtr->IsPaused())
		{
			return;
		}
		if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
		{
			FirstPC->SetPause(false);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] The world was PAUSED (the character-select screen does that). Unpaused, or every "
				     "measurement below would have been a zero that looks like a pass."), Tag);
		}
	}

	/** The human's ability component. A bot's character belongs to the game mode's fill; do not race it. */
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

	void GatherLiving(UWorld* WorldPtr, const UTraceAbilityComponent* Comp, bool bWantEnemies,
	                  TArray<ATraceCharacter*>& Out)
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
			if (TheirTeam == ETraceTeam::None || MyTeam == ETraceTeam::None)
			{
				continue;
			}
			const bool bEnemy = (TheirTeam != MyTeam);
			if (bEnemy == bWantEnemies)
			{
				Out.Add(Candidate);
			}
		}
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

	FString DescribeTally(const TraceElle::FGateTally& Tally)
	{
		return FString::Printf(
			TEXT("teleports=%d chokePoint=%d teamKnob=%d carrierKnob=%d lockout=%d"),
			Tally.TeleportsCommitted, Tally.RefusedByChokePoint, Tally.RefusedByTeamKnob,
			Tally.RefusedByCarrierKnob, Tally.RefusedByLockout);
	}

	// =============================================================================================
	// Trace.Elle.CarrierTest — the founding invariant. TWO ARMS, RED FIRST.
	// =============================================================================================

	struct FElleCarrierState
	{
		/** 0 = RED (Trace.Ability.CarrierImmune 0). 1 = GREEN (shipped). */
		int32 Arm = 0;

		/** -1 stage, 0 lay the pair, 1 hold both subjects in the mouth and measure, 2 judge. */
		int32 Phase = -1;

		double PhaseStartReal = 0.0;
		double AcquireDeadline = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Elle;
		TWeakObjectPtr<ATraceCharacter> Carrier;
		TWeakObjectPtr<ATraceCharacter> Control;

		FVector MouthA = FVector::ZeroVector;
		FVector MouthB = FVector::ZeroVector;

		bool bRedRan = false;
		bool bRedMovedCarrier = false;
		bool bRedMovedControl = false;

		bool bGreenRan = false;
		bool bGreenHeldCarrier = false;
		bool bGreenRefusedByChokePoint = false;
		bool bGreenMovedControl = false;
	};

	void FinishCarrierTest(const TSharedPtr<FElleCarrierState>& State)
	{
		RestoreCarrierArm();

		const bool bFixtureReached = State->bRedRan && State->bRedMovedCarrier;
		const bool bInvariantHolds = State->bGreenRan && State->bGreenHeldCarrier
			&& State->bGreenRefusedByChokePoint && State->bGreenMovedControl;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLECARRIER] RED arm  (rule REMOVED): carrier moved=%d  control moved=%d"),
			State->bRedMovedCarrier ? 1 : 0, State->bRedMovedControl ? 1 : 0);
		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLECARRIER] GREEN arm (shipped rule): carrier moved=%d  refused by the choke point=%d  "
			     "control moved=%d"),
			State->bGreenHeldCarrier ? 0 : 1, State->bGreenRefusedByChokePoint ? 1 : 0,
			State->bGreenMovedControl ? 1 : 0);

		if (!bFixtureReached)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[ELLECARRIER] VERDICT: INVALID — the RED arm did NOT move the carrier, so the fixture "
				     "never reached the teleport at all and the green arm proves nothing. Check that the pair "
				     "formed, that the subjects were inside a mouth, and that "
				     "bElleSnapUsableByBothTeams is on."));
			return;
		}

		if (bInvariantHolds)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ELLECARRIER] VERDICT: PASS — an ENEMY gate moved the Core carrier with the rule removed "
				     "and REFUSED to move them with it in place, while moving a non-carrier enemy in the same "
				     "frames. Spec v18 §2's 'a Core carrier must not be teleported by an enemy gate' holds, and "
				     "it holds because UTraceAbilityComponent::CanAffectTarget said so."));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[ELLECARRIER] VERDICT: *** FAIL *** — THE FOUNDING INVARIANT IS BROKEN. An ability moved a "
				     "Core carrier (or the control case stopped working, which makes the result unreadable)."));
		}
	}

	void RunElleCarrierTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ELLECARRIER] no authoritative game world — run this on the server."));
			return;
		}
		if (AbilityCarrierImmuneCVar() == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ELLECARRIER] Trace.Ability.CarrierImmune is not registered — cannot arm the test."));
			return;
		}
		if (!UTraceSettings::Get().bElleSnapUsableByBothTeams)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ELLECARRIER] INVALID — bElleSnapUsableByBothTeams is OFF, so an enemy is refused before the "
				     "choke point is ever asked and there is no 'enemy gate moves a carrier' case to test. Turn it "
				     "on (Project Settings > Game > Trace > Abilities|Elle) and run again."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("ELLECARRIER"));

		TSharedPtr<FElleCarrierState> State = MakeShared<FElleCarrierState>();
		State->Arm = 0;                                  // RED FIRST. Always.
		State->PhaseStartReal = FPlatformTime::Seconds();
		State->AcquireDeadline = State->PhaseStartReal + 120.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLECARRIER] ===== spec v18 §2 x the founding invariant. An ENEMY of Elle holds the Core and "
			     "stands in a gate mouth. Arm 0 = RED (Trace.Ability.CarrierImmune 0 — the carrier MUST be moved). "
			     "Arm 1 = GREEN (shipped — they must NOT be, while a non-carrier enemy in the same mouth still "
			     "is). ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ELLECARRIER] ABORTED: the world went away mid-test."));
				RestoreCarrierArm();
				return false;
			}

			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase -1: stage ---------------------------------------------------------------
			if (State->Phase < 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Elle)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Elle);
				}

				UTraceAbilitySetElle* ElleSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetElle>() : nullptr;

				TArray<ATraceCharacter*> Enemies;
				GatherLiving(TickWorld, Human, /*bWantEnemies=*/true, Enemies);

				if (ElleSet == nullptr || Enemies.Num() < 2 || CoreActor == nullptr)
				{
					if (NowReal > State->AcquireDeadline)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[ELLECARRIER] VERDICT: INVALID — could not stage inside the acquire window "
							     "(elleSet=%d livingEnemies=%d core=%d). Needs a match with characters ON and at "
							     "least two living enemies of the human player."),
							(ElleSet != nullptr) ? 1 : 0, Enemies.Num(), (CoreActor != nullptr) ? 1 : 0);
						RestoreCarrierArm();
						return false;
					}
					return true;
				}

				// The carrier must be an ENEMY of Elle, or the team rule would refuse everything and the
				// red arm would fail to reproduce for entirely the wrong reason.
				CoreActor->TryPickup(Enemies[0]);
				if (CoreActor->Carrier != Enemies[0])
				{
					if (NowReal > State->AcquireDeadline)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[ELLECARRIER] VERDICT: INVALID — could not put the Core on an enemy of Elle, so "
							     "there is no carrier to test against."));
						RestoreCarrierArm();
						return false;
					}
					return true;
				}

				State->Elle = Human;
				State->Carrier = Enemies[0];
				State->Control = Enemies[1];

				// THE TWO MOUTHS ARE THE TWO SUBJECTS' OWN STANDING POSITIONS, for one reason: they are
				// the only two points in the arena this harness KNOWS a pawn fits in. A synthetic offset
				// can land inside a wall, and TeleportTo would refuse the arrival — which reads exactly
				// like the carrier rule working, in both arms.
				State->MouthA = Enemies[0]->GetActorLocation();
				State->MouthB = Enemies[1]->GetActorLocation();

				State->Phase = 0;
				State->PhaseStartReal = NowReal;

				// THE ARM IS APPLIED HERE, not by the caller, so there is no way to run this command that
				// skips the red half.
				AbilityCarrierImmuneCVar()->Set(State->Arm, ECVF_SetByConsole);
				TraceElle::ResetTallies();

				UE_LOG(LogTraceGame, Display,
					TEXT("[ELLECARRIER] --- arm=%d (%s) | Elle %s (team %d) | CARRIER %s (team %d) | CONTROL %s "
					     "(team %d) | mouths (%s) <-> (%s)"),
					State->Arm, (State->Arm == 0) ? TEXT("RED, rule removed") : TEXT("GREEN, shipped rule"),
					*GetNameSafe(Human->GetOwningCharacter()), static_cast<int32>(Human->GetTeam()),
					*GetNameSafe(Enemies[0]), static_cast<int32>(Enemies[0]->GetTeam()),
					*GetNameSafe(Enemies[1]), static_cast<int32>(Enemies[1]->GetTeam()),
					*State->MouthA.ToCompactString(), *State->MouthB.ToCompactString());
				return true;
			}

			UTraceAbilityComponent* Elle = State->Elle.Get();
			ATraceCharacter* Carrier = State->Carrier.Get();
			ATraceCharacter* Control = State->Control.Get();
			UTraceAbilitySetElle* ElleSet = (Elle != nullptr)
				? Elle->GetAbilitySetAs<UTraceAbilitySetElle>() : nullptr;

			if (Elle == nullptr || Carrier == nullptr || Control == nullptr || ElleSet == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ELLECARRIER] ABORTED: a participant went away mid-test."));
				RestoreCarrierArm();
				return false;
			}

			// PRECONDITION, HELD EVERY TICK. Bots pass and score; a victim who stopped being the carrier
			// between staging and the measurement would turn the case under test into an ordinary
			// non-carrier teleport that passes for the wrong reason.
			if (CoreActor != nullptr && CoreActor->Carrier != Carrier)
			{
				CoreActor->TryPickup(Carrier);
			}

			// ---- Phase 0: lay the pair ----------------------------------------------------------
			if (State->Phase == 0)
			{
				if (!ElleSet->DebugPlaceGatePair(State->MouthA, State->MouthB))
				{
					UE_LOG(LogTraceGame, Error, TEXT("[ELLECARRIER] VERDICT: INVALID — the gate pair refused to spawn."));
					RestoreCarrierArm();
					return false;
				}
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 1: hold both subjects in mouth A and let the gate decide ------------------
			if (State->Phase == 1)
			{
				// PairWith seeds the re-entry lockout for everybody already standing in a mouth — which
				// is BOTH subjects, by construction, since the mouths are their own feet. Waiting it out
				// is what makes this measure the carrier rule rather than the lockout.
				const double LockoutWait = static_cast<double>(UTraceSettings::Get().ElleSnapTeleportLockoutSeconds) + 0.35;

				if ((NowReal - State->PhaseStartReal) < LockoutWait)
				{
					return true;
				}

				// Both subjects are pushed back into mouth A every tick. That deliberately re-arms the
				// test after each successful teleport, so a single lockout cannot make an arm read as
				// "nothing happened".
				Carrier->TeleportTo(State->MouthA, Carrier->GetActorRotation());
				Control->TeleportTo(State->MouthA, Control->GetActorRotation());

				// Two seconds past the lockout is several lockout cycles at the shipped 1 s, so the
				// control has had more than one chance to be taken.
				if ((NowReal - State->PhaseStartReal) < (LockoutWait + 2.0))
				{
					return true;
				}

				State->Phase = 2;
				return true;
			}

			// ---- Phase 2: judge this arm, then run the other ------------------------------------
			const TraceElle::FGateTally CarrierTally = TraceElle::CarrierTally();
			const TraceElle::FGateTally OtherTally = TraceElle::OtherTally();

			UE_LOG(LogTraceGame, Display,
				TEXT("[ELLECARRIER] arm=%d tallies | CARRIER: %s | OTHERS: %s"),
				State->Arm, *DescribeTally(CarrierTally), *DescribeTally(OtherTally));

			if (State->Arm == 0)
			{
				State->bRedRan = true;
				State->bRedMovedCarrier = (CarrierTally.TeleportsCommitted > 0);
				State->bRedMovedControl = (OtherTally.TeleportsCommitted > 0);

				// Straight into the green arm, from the same staging, so the two arms differ in exactly
				// one bit.
				State->Arm = 1;
				State->Phase = -1;
				State->PhaseStartReal = NowReal;
				State->AcquireDeadline = NowReal + 120.0;
				return true;
			}

			State->bGreenRan = true;
			State->bGreenHeldCarrier = (CarrierTally.TeleportsCommitted == 0);
			State->bGreenRefusedByChokePoint = (CarrierTally.RefusedByChokePoint > 0);
			State->bGreenMovedControl = (OtherTally.TeleportsCommitted > 0);

			FinishCarrierTest(State);
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Elle.Verify — the cloak, SNAP's shape, the teleport and the slide-jump derivation
	// =============================================================================================

	struct FElleVerifyState
	{
		int32 Phase = 0;
		double PhaseStartReal = 0.0;
		double AcquireDeadline = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Elle;
		TWeakObjectPtr<ATraceCharacter> Mate;

		// SNAP
		bool bFirstPressFired = false;
		bool bGateOneExists = false;
		bool bAwaitingAfterFirst = false;
		float CooldownAfterFirst = -1.f;

		bool bSecondPressFired = false;
		bool bPairFormed = false;
		float CooldownAfterPair = -1.f;

		FVector MateStart = FVector::ZeroVector;
		FVector MouthA = FVector::ZeroVector;
		FVector MouthB = FVector::ZeroVector;
		bool bMateTeleported = false;

		/**
		 * How far apart the staging actually got the two mouths, in uu. Reported so a degenerate
		 * fixture names itself instead of failing the product: two mouths inside one gate radius make
		 * "arrived at B rather than A" undecidable, and that is a broken test, not a broken portal.
		 */
		float MouthSeparation = -1.f;

		// CLOAK
		double CoreStagedAt = 0.0;
		bool bCloakedAfterPass = false;
		float CloakSecondsSeen = -1.f;
		bool bThrowLaunched = false;
		bool bCloakedAfterThrow = false;
		bool bCloakedAfterEnemyGrant = false;

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; }
			else            { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[ELLEVERIFY]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}

		/**
		 * The fixture, not the product, could not be staged. Counts as NEITHER a pass nor a failure,
		 * and says why in the same breath — a "PASS" here would be the uninformative green this
		 * project has shipped twice, and a "FAIL" would be a product defect that is not one.
		 */
		void Invalidate(const FString& Reason)
		{
			++Invalid;
			UE_LOG(LogTraceGame, Warning, TEXT("[ELLEVERIFY]   INVALID  %s"), *Reason);
		}

		int32 Invalid = 0;
	};

	void RunElleVerify()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ELLEVERIFY] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr, TEXT("ELLEVERIFY"));

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLEVERIFY] ===== spec v18 §2 Elle. Knobs AS THE RUNNING GAME HAS THEM: cloak %.1fs at opacity "
			     "%.2f (dropsOnPickup=%d) | slide-jump gain bonus +%.0f%% | SNAP window %.1fs, pair %.1fs, radius "
			     "%.0f uu, lockout %.2fs, cooldown %.0fs, bothTeams=%d, carrierMayUse=%d ====="),
			Settings.ElleCloakDurationSeconds, Settings.ElleCloakOpacity,
			Settings.bElleCloakEndsOnCorePickup ? 1 : 0, Settings.ElleSlideJumpGainBonus * 100.f,
			Settings.ElleSnapSecondGateWindowSeconds, Settings.ElleSnapPairLifetimeSeconds,
			Settings.ElleSnapGateRadiusUU, Settings.ElleSnapTeleportLockoutSeconds,
			Settings.ElleSnapCooldownSeconds, Settings.bElleSnapUsableByBothTeams ? 1 : 0,
			Settings.bElleSnapCarrierMayUseGate ? 1 : 0);

		TSharedPtr<FElleVerifyState> State = MakeShared<FElleVerifyState>();
		State->PhaseStartReal = FPlatformTime::Seconds();
		State->AcquireDeadline = State->PhaseStartReal + 120.0;

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ELLEVERIFY] ABORTED: the world went away."));
				return false;
			}

			const double NowReal = FPlatformTime::Seconds();
			const UTraceSettings& Knobs = UTraceSettings::Get();
			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);

			// ---- Phase 0: stage -----------------------------------------------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Elle)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Elle);
				}

				UTraceAbilitySetElle* ElleSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetElle>() : nullptr;
				ATraceCharacter* EllePawn = (Human != nullptr) ? Human->GetOwningCharacter() : nullptr;

				TArray<ATraceCharacter*> Mates;
				GatherLiving(TickWorld, Human, /*bWantEnemies=*/false, Mates);

				if (ElleSet == nullptr || EllePawn == nullptr || Mates.Num() < 1)
				{
					if (NowReal > State->AcquireDeadline)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[ELLEVERIFY] VERDICT: INVALID — could not stage (elleSet=%d pawn=%d livingMates=%d). "
							     "Run this EARLY in a match, with characters ON and at least one living team-mate."),
							(ElleSet != nullptr) ? 1 : 0, (EllePawn != nullptr) ? 1 : 0, Mates.Num());
						return false;
					}
					return true;
				}

				State->Elle = Human;
				State->Mate = Mates[0];
				State->MateStart = Mates[0]->GetActorLocation();
				State->Phase = 1;
				State->PhaseStartReal = NowReal;

				// SECTION E, run first because it needs nothing staged and is a pure derivation.
				//
				// *** THE GLOBAL IS READ OFF A NON-ELLE PAWN, AND THAT CHANGED WHEN THE SEAM LANDED. ***
				// GetSlideJumpWindowSpeedBonus() used to be character-blind, so asking ELLE's own
				// movement component for "the global" was harmless. It is now per-pawn (v18 §2
				// integration), so asking hers returns 1.625625 — her own answer — and feeding that
				// back into her own formula scored 1.875875 against a 1.625625 "global", i.e. the
				// passive applied twice and the NOT-WIRED probe below reported a wired seam as dead.
				// A team-mate's component is the un-scaled number by construction: every character
				// except Elle returns its argument unchanged, which is the very property this section
				// then asserts.
				const UTraceCharacterMovementComponent* Move = EllePawn->GetTraceMovement();
				const UTraceCharacterMovementComponent* MateMove = Mates[0]->GetTraceMovement();
				const float Global = (MateMove != nullptr) ? MateMove->GetSlideJumpWindowSpeedBonusForAudit() : 1.446875f;
				const float Hers = UTraceAbilitySetElle::GetSlideJumpWindowSpeedBonusForElle(EllePawn, Global);
				const float Theirs = UTraceAbilitySetElle::GetSlideJumpWindowSpeedBonusForElle(Mates[0], Global);
				const float Expected = 1.f + (Global - 1.f) * (1.f + Knobs.ElleSlideJumpGainBonus);

				UE_LOG(LogTraceGame, Display,
					TEXT("[ELLEVERIFY] --- PASSIVE 2, the well-timed slide-jump gain ---"));
				State->Check(FMath::IsNearlyEqual(Hers, Expected, 0.0005f),
					FString::Printf(TEXT("Elle's well-timed slide-jump multiplier is %.6f — the GAIN of the shipped "
					                     "%.6f scaled by +%.0f%%, not the multiplier (which would be %.6f)"),
						Hers, Global, Knobs.ElleSlideJumpGainBonus * 100.f,
						Global * (1.f + Knobs.ElleSlideJumpGainBonus)));
				State->Check(FMath::IsNearlyEqual(Theirs, Global, 0.0005f),
					FString::Printf(TEXT("a NON-Elle pawn is untouched at %.6f — spec v18 §4's 'Elle changes only "
					                     "her own'"), Theirs));

				// *** AND THE HALF THAT USED TO BE MISSING, NOW ASSERTED RATHER THAN WARNED ABOUT. ***
				//
				// This was a Warning reading "NOT WIRED IN": the derivation above was correct and
				// reached no actual slide-jump, because GetSlideJumpWindowSpeedBonus() read UTraceSettings
				// and asked no ability anything. The v18 §2 integration pass added the per-pawn seam
				// (UTraceCharacterAbilitySet::ModifySlideJumpWindowSpeedBonus, fronted by
				// UTraceAbilityComponent::GetSlideJumpWindowSpeedBonusFor), so the movement component now
				// answers HER number for HER pawn. That is a check, not a warning — and it is the one
				// assertion in this section that fails if somebody unhooks the seam again.
				const float LiveForElle = (Move != nullptr) ? Move->GetSlideJumpWindowSpeedBonusForAudit() : -1.f;
				const float LiveForMate = (MateMove != nullptr) ? MateMove->GetSlideJumpWindowSpeedBonusForAudit() : -1.f;
				State->Check(FMath::IsNearlyEqual(LiveForElle, Hers, 0.0005f),
					FString::Printf(TEXT("*** WIRED IN: the MOVEMENT COMPONENT answers %.6f for Elle's own pawn ***"
					                     " — the derivation above reaches a real slide-jump, not just a harness"),
						LiveForElle));
				State->Check(FMath::IsNearlyEqual(LiveForMate, Global, 0.0005f),
					FString::Printf(TEXT("...and %.6f for a team-mate's, so the seam moved HER number and nobody "
					                     "else's — spec v18 §4's 'slide-jump 1.446875 (Elle changes only her own)'"),
						LiveForMate));
				return true;
			}

			UTraceAbilityComponent* Elle = State->Elle.Get();
			ATraceCharacter* Mate = State->Mate.Get();
			UTraceAbilitySetElle* ElleSet = (Elle != nullptr)
				? Elle->GetAbilitySetAs<UTraceAbilitySetElle>() : nullptr;
			ATraceCharacter* EllePawn = (Elle != nullptr) ? Elle->GetOwningCharacter() : nullptr;

			if (Elle == nullptr || Mate == nullptr || ElleSet == nullptr || EllePawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ELLEVERIFY] ABORTED: a participant went away mid-test."));
				return false;
			}

			// ---- Phase 1: SNAP, press 1 ---------------------------------------------------------
			if (State->Phase == 1)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[ELLEVERIFY] --- ACTIVATED: SNAP ---"));

				State->bFirstPressFired = Elle->TryActivate();
				State->bGateOneExists = (ElleSet->GetFirstGate() != nullptr);
				State->bAwaitingAfterFirst = ElleSet->IsAwaitingSecondGate();
				State->CooldownAfterFirst = Elle->GetActivatedCooldownRemaining();
				State->MouthA = (ElleSet->GetFirstGate() != nullptr)
					? ElleSet->GetFirstGate()->GetMouthLocation() : FVector::ZeroVector;

				State->Check(State->bFirstPressFired && State->bGateOneExists,
					TEXT("press 1 places a gate 'where she stands'"));
				State->Check(State->bAwaitingAfterFirst,
					FString::Printf(TEXT("the %.1fs second-gate window is open after press 1"),
						Knobs.ElleSnapSecondGateWindowSeconds));
				State->Check(State->CooldownAfterFirst <= 0.01f,
					FString::Printf(TEXT("press 1 charges NO cooldown (%.2fs), which is the only way press 2 can "
					                     "reach the ability at all"), State->CooldownAfterFirst));
				State->Check(ElleSet->GetFirstGate() != nullptr && !ElleSet->GetFirstGate()->IsPaired(),
					TEXT("a lone gate is UNPAIRED and therefore teleports nobody"));

				State->Phase = 2;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 2: SNAP, press 2 (inside the window) --------------------------------------
			if (State->Phase == 2)
			{
				if ((NowReal - State->PhaseStartReal) < 0.30)
				{
					return true;   // a beat, so "within 4 s" is a real interval rather than the same frame
				}

				// Elle is moved before press 2 so the two mouths are demonstrably different places, and
				// so the teleport below has somewhere to arrive that is not where it started.
				//
				// *** AND THE SEPARATION IS NOW ENFORCED RATHER THAN HOPED FOR. *** Moving her to the
				// team-mate's start is only a real move when the team-mate started somewhere else, and
				// on a headless listen host the local player and Mates[0] can spawn on ADJACENT PADS:
				// a measured run put the two mouths 69 uu apart, i.e. both inside the 130 uu gate
				// radius, so "did the mate arrive at B rather than stay at A?" had no answer and the
				// section failed on a build where the teleport demonstrably fired. That is a fixture
				// defect reported as a product defect, which is the worst kind of red.
				//
				// So: take the mate's start if it is far enough, and otherwise walk a ring of candidate
				// points around mouth A at six gate radii until TeleportTo accepts one. TeleportTo is
				// what refuses a point inside geometry, so a candidate that lands in a wall simply is
				// not taken and the next bearing is tried.
				{
					const float Radius = FMath::Max(1.f, Knobs.ElleSnapGateRadiusUU);
					const float MinSeparation = Radius * 3.f;

					bool bPlaced = EllePawn->TeleportTo(State->MateStart, EllePawn->GetActorRotation())
						&& FVector::Dist2D(EllePawn->GetActorLocation(), State->MouthA) >= MinSeparation;

					for (int32 Bearing = 0; !bPlaced && Bearing < 8; ++Bearing)
					{
						const float Angle = FMath::DegreesToRadians(45.f * static_cast<float>(Bearing));
						const FVector Candidate = State->MouthA
							+ FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.f) * (Radius * 6.f);

						bPlaced = EllePawn->TeleportTo(Candidate, EllePawn->GetActorRotation())
							&& FVector::Dist2D(EllePawn->GetActorLocation(), State->MouthA) >= MinSeparation;
					}

					State->MouthSeparation = FVector::Dist2D(EllePawn->GetActorLocation(), State->MouthA);
				}

				State->bSecondPressFired = Elle->TryActivate();
				State->bPairFormed = (ElleSet->GetFirstGate() != nullptr) && (ElleSet->GetSecondGate() != nullptr)
					&& ElleSet->GetFirstGate()->IsPaired() && ElleSet->GetSecondGate()->IsPaired();
				State->CooldownAfterPair = Elle->GetActivatedCooldownRemaining();
				State->MouthB = (ElleSet->GetSecondGate() != nullptr)
					? ElleSet->GetSecondGate()->GetMouthLocation() : FVector::ZeroVector;

				State->Check(State->bSecondPressFired && State->bPairFormed,
					TEXT("press 2 inside the window places the second gate and PAIRS them"));
				// ALSO QUALIFIED ON THE PAIR HAVING FORMED. Trace.Elle.SnapEnabled 0 charges the full
				// cooldown from press 1 and places nothing, so the cooldown NUMBER on its own reads
				// correct on an arm where the ability did nothing whatsoever. The claim is "a COMPLETED
				// cast costs 35 s counted from press 1", and half of that claim is that it completed.
				State->Check(State->bPairFormed
					&& State->CooldownAfterPair > (Knobs.ElleSnapCooldownSeconds - 3.f)
					&& State->CooldownAfterPair <= Knobs.ElleSnapCooldownSeconds + 0.01f,
					FString::Printf(TEXT("the pair charges the %.0fs cooldown, measured from press 1 (%.2fs left)"),
						Knobs.ElleSnapCooldownSeconds, State->CooldownAfterPair));

				const float PairLifetime = (ElleSet->GetFirstGate() != nullptr)
					? ElleSet->GetFirstGate()->GetExpireMatchTime() - static_cast<float>(TickWorld->GetGameState()->GetServerWorldTimeSeconds())
					: -1.f;
				State->Check(FMath::IsNearlyEqual(PairLifetime, Knobs.ElleSnapPairLifetimeSeconds, 0.35f),
					FString::Printf(TEXT("BOTH gates expire %.1fs after the PAIR completes (%.2fs left on the first, "
					                     "which was placed earlier)"), Knobs.ElleSnapPairLifetimeSeconds, PairLifetime));

				State->Phase = 3;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 3: a team-mate steps into mouth A and arrives at mouth B -------------------
			if (State->Phase == 3)
			{
				// PairWith locked out everybody standing in a mouth at the instant of pairing — Elle
				// included. Wait it out, then push the team-mate in.
				const double LockoutWait = static_cast<double>(Knobs.ElleSnapTeleportLockoutSeconds) + 0.35;
				if ((NowReal - State->PhaseStartReal) < LockoutWait)
				{
					return true;
				}

				if (!State->bMateTeleported)
				{
					Mate->TeleportTo(State->MouthA, Mate->GetActorRotation());
					State->bMateTeleported = true;
					return true;   // let the gate tick once
				}

				const float ToB = static_cast<float>(FVector::Dist(Mate->GetActorLocation(), State->MouthB));
				const float ToA = static_cast<float>(FVector::Dist(Mate->GetActorLocation(), State->MouthA));
				const float Separation = static_cast<float>(FVector::Dist(State->MouthA, State->MouthB));

				// A DEGENERATE FIXTURE IS REPORTED AS ONE, not as a failed portal. If the staging above
				// could not get the mouths further apart than a gate radius, a player standing at B is
				// also standing at A and no distance test can tell the two apart — so there is nothing
				// to measure and saying so is the honest answer. Trace.Elle.SnapTeleportEnabled 0 still
				// goes RED whenever the fixture is legitimate, which is every run where the staging
				// found anywhere to stand.
				if (Separation < Knobs.ElleSnapGateRadiusUU)
				{
					State->Invalidate(FString::Printf(
						TEXT("the two mouths ended up %.0f uu apart, inside the %.0f uu gate radius — a player at B "
						     "is also at A, so 'arrived at B' has no answer. The staging tried the team-mate's "
						     "start and then eight bearings around mouth A and could not find anywhere to stand"),
						Separation, Knobs.ElleSnapGateRadiusUU));
				}
				else
				{
					State->Check(ToB < ToA && ToB < (Knobs.ElleSnapGateRadiusUU + 60.f),
						FString::Printf(TEXT("a team-mate placed in mouth A arrives at mouth B (%.0f uu from B, %.0f uu "
						                     "from A, mouths %.0f uu apart). Trace.Elle.SnapTeleportEnabled 0 makes "
						                     "this FAIL and nothing else"),
							ToB, ToA, Separation));
				}

				State->Phase = 4;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 4: let the pair expire, and assert that it DOES ---------------------------
			//
			// THIS WAIT IS NOT PADDING, IT IS THE FIRST RUN'S BUG. With the pair still live, Elle was
			// standing in her own mouth and was flung across the arena every second — which cancelled
			// the staged pass on range and line of sight, and reported the cloak as broken when the
			// cloak was fine. Waiting the pair out both fixes that and buys §2's second lifetime
			// clause an assertion of its own.
			if (State->Phase == 4)
			{
				if (ElleSet->GetFirstGate() != nullptr || ElleSet->GetSecondGate() != nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > (Knobs.ElleSnapPairLifetimeSeconds + 4.0))
					{
						State->Check(false,
							FString::Printf(TEXT("the pair is gone %.1fs after it completed"),
								Knobs.ElleSnapPairLifetimeSeconds));
						State->Phase = 5;
						State->PhaseStartReal = NowReal;
					}
					return true;
				}

				// QUALIFIED ON THE PAIR HAVING EXISTED, and that is not pedantry — it is what the red
				// arms measured. Under Trace.Elle.SnapEnabled 0 no gate is ever placed, so "there are no
				// gates now" is trivially true and this assertion reported PASS in an arm where the whole
				// ability was switched off. An expiry test that also passes when nothing was ever created
				// is the uninformative green this project has shipped twice; requiring bPairFormed is what
				// makes it measure expiry rather than absence.
				State->Check(State->bPairFormed,
					FString::Printf(TEXT("both gates are GONE %.1fs after the pair completed — §2's 'with both "
					                     "placed, both expire after 8 s' (a pair was actually formed first: %d)"),
						Knobs.ElleSnapPairLifetimeSeconds, State->bPairFormed ? 1 : 0));

				UE_LOG(LogTraceGame, Display, TEXT("[ELLEVERIFY] --- PASSIVE 1: the cloak ---"));
				State->Phase = 5;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 5: stage the Core on Elle and HOLD it ------------------------------------
			//
			// *** THE HOLD IS THE FIRST RUN'S SECOND BUG, AND IT IS A REAL PROPERTY OF THE TRIGGER. ***
			// The cloak trigger is an edge detector on the ability component's 20 Hz tick, so Elle has
			// to have been SEEN holding the Core before she can be seen stopping. The first version of
			// this harness gave it to her and threw it 16 ms later — inside a single ability tick — so
			// the rising edge never happened and the cloak correctly did not fire. Holding it for 0.6 s
			// is what a player does by definition (you cannot pass in less than the 0.5 s hold, and you
			// cannot throw without charging), and it is stated in the ability's header as the 50 ms
			// granularity that buys not having to add a delegate to ATraceCore.
			if (State->Phase == 5)
			{
				if (CoreActor == nullptr)
				{
					State->Check(false, TEXT("there is a Core (INVALID without one)"));
					State->Phase = 8;
					State->PhaseStartReal = NowReal;
					return true;
				}

				if (!ATraceCore::IsCoreHolder(EllePawn))
				{
					CoreActor->TryPickup(EllePawn);
					State->CoreStagedAt = 0.0;
					if ((NowReal - State->PhaseStartReal) > 8.0)
					{
						State->Check(false, TEXT("the Core could be staged on Elle"));
						State->Phase = 8;
						State->PhaseStartReal = NowReal;
					}
					return true;
				}

				if (State->CoreStagedAt <= 0.0)
				{
					State->CoreStagedAt = NowReal;
					return true;
				}
				if ((NowReal - State->CoreStagedAt) < 0.6)
				{
					return true;
				}

				// THE COMPLETION OF A PASS, through the exact call the pass state machine makes:
				// ATraceCore::ServerTickPass ends a matured hover-pass with
				// GrantTo(Receiver, ETraceCoreGrantReason::Pass). Driving the 0.5 s hover itself is not
				// available to a headless test — DebugForcePassWindow deliberately HOLDS the window
				// open rather than letting it mature (its own comment says so), and a hand-driven hover
				// is cancelled by the receiver moving. This is the same function, the same reason code
				// and the same resulting state; what it skips is the input, not the mechanic.
				CoreActor->GrantTo(Mate, ETraceCoreGrantReason::Pass);

				State->Phase = 6;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 6: the cloak is up, is 3 s long, and drops when she retakes the Core -------
			if (State->Phase == 6)
			{
				if (ElleSet->IsCloaked() && !State->bCloakedAfterPass)
				{
					State->bCloakedAfterPass = true;
					const float Now = static_cast<float>(TickWorld->GetGameState()->GetServerWorldTimeSeconds());
					State->CloakSecondsSeen = ElleSet->GetCloakEndMatchTime() - Now;
				}

				// The ability tick is 20 Hz, so the cloak can be up to 50 ms behind the pass. Half a
				// second is ten of those.
				if ((NowReal - State->PhaseStartReal) < 0.5)
				{
					return true;
				}

				State->Check(State->bCloakedAfterPass,
					TEXT("PASSING the Core to a team-mate raises the cloak. Trace.Elle.CloakEnabled 0 makes this "
					     "FAIL and nothing else"));
				State->Check(State->CloakSecondsSeen > (Knobs.ElleCloakDurationSeconds - 0.6f)
					&& State->CloakSecondsSeen <= Knobs.ElleCloakDurationSeconds + 0.01f,
					FString::Printf(TEXT("the cloak is %.1fs long (%.2fs remained when it was first seen)"),
						Knobs.ElleCloakDurationSeconds, State->CloakSecondsSeen));
				State->Check(State->bCloakedAfterPass && ElleSet->GetCloakEndMatchTime() > 0.f,
					TEXT("the cloak is a REPLICATED fact on FTraceAbilityNetState — one field, written only by "
					     "the server, read identically on every machine"));

				// AND THE OTHER HALF OF "it replicates": the fact having arrived is not the same as
				// anything having been repainted. A listen-server host draws, so the paint must be on.
				const bool bDraws = (TickWorld->GetNetMode() != NM_DedicatedServer);
				State->Check(!bDraws || !State->bCloakedAfterPass || ElleSet->IsCloakVisualApplied(),
					TEXT("the COSMETIC half is pushed onto the pawn's body meshes on a machine that draws — the "
					     "fact replicating and the pawn actually being repainted are two different failures"));

				// §2 [ASSUMPTION]: taking the Core back drops the cloak early.
				if (Knobs.bElleCloakEndsOnCorePickup && ElleSet->IsCloaked())
				{
					CoreActor->TryPickup(EllePawn);
					State->Phase = 65;
					State->PhaseStartReal = NowReal;
					return true;
				}

				State->Phase = 7;
				State->PhaseStartReal = NowReal;
				State->CoreStagedAt = 0.0;
				return true;
			}

			if (State->Phase == 65)
			{
				// One ability tick (50 ms) plus slack for the pickup to land.
				if ((NowReal - State->PhaseStartReal) < 0.4)
				{
					return true;
				}

				State->Check(ATraceCore::IsCoreHolder(EllePawn) && !ElleSet->IsCloaked(),
					TEXT("taking the Core BACK drops the cloak early — §2's [ASSUMPTION], and one tick box "
					     "(bElleCloakEndsOnCorePickup) away from the other reading"));

				State->Phase = 7;
				State->PhaseStartReal = NowReal;
				State->CoreStagedAt = 0.0;
				return true;
			}

			// ---- Phase 7: THROWING it raises the cloak too — §2's other trigger -------------------
			if (State->Phase == 7)
			{
				if (CoreActor == nullptr || !ATraceCore::IsModeB(TickWorld))
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[ELLEVERIFY]   SKIPPED the THROW half — this match is mode A, where the Core is never "
						     "thrown. The pass above is the mode-A trigger and it is the same code path."));
					State->Phase = 8;
					State->PhaseStartReal = NowReal;
					return true;
				}

				if (!ATraceCore::IsCoreHolder(EllePawn))
				{
					CoreActor->TryPickup(EllePawn);
					State->CoreStagedAt = 0.0;
					if ((NowReal - State->PhaseStartReal) > 8.0)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[ELLEVERIFY]   SKIPPED the THROW half — the Core could not be staged on Elle."));
						State->Phase = 8;
						State->PhaseStartReal = NowReal;
					}
					return true;
				}

				if (State->CoreStagedAt <= 0.0)
				{
					State->CoreStagedAt = NowReal;
					State->bCloakedAfterThrow = false;
					return true;
				}
				if ((NowReal - State->CoreStagedAt) < 0.6)
				{
					return true;
				}

				// Elle must not still be wearing the cloak from the pass, or "the throw raised it" is
				// unfalsifiable.
				if (ElleSet->IsCloaked())
				{
					return true;
				}

				State->bThrowLaunched = CoreActor->ThrowFromHolder(EllePawn);
				State->Phase = 75;
				State->PhaseStartReal = NowReal;
				return true;
			}

			if (State->Phase == 75)
			{
				if (ElleSet->IsCloaked())
				{
					State->bCloakedAfterThrow = true;
				}
				if ((NowReal - State->PhaseStartReal) < 0.5)
				{
					return true;
				}

				if (State->bThrowLaunched)
				{
					State->Check(State->bCloakedAfterThrow,
						TEXT("THROWING the Core raises the cloak too — §2's 'passing OR throwing'"));
				}
				else
				{
					// HONEST, NOT GREEN. ThrowFromHolder refuses on its own throw cooldown, on a locked
					// Core state and on a Core that is already loose — none of which is a statement
					// about Elle. The pass above already proved the trigger on the same code path.
					UE_LOG(LogTraceGame, Warning,
						TEXT("[ELLEVERIFY]   SKIPPED the THROW half — ATraceCore::ThrowFromHolder refused (its own "
						     "throw cooldown, a locked Core state or an already-loose Core). NOT counted as a "
						     "failure: the PASS above tested the same trigger on the same code path."));
				}

				CoreActor->TryPickup(EllePawn);
				State->Phase = 8;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 8: LOSING the Core to an enemy must NOT cloak ------------------------------
			if (State->Phase == 8)
			{
				TArray<ATraceCharacter*> Enemies;
				GatherLiving(TickWorld, Elle, /*bWantEnemies=*/true, Enemies);

				if (CoreActor != nullptr && Enemies.Num() > 0 && ATraceCore::IsCoreHolder(EllePawn))
				{
					// Any cloak from the earlier trigger has to have expired first, or what is measured
					// is that one and not this loss.
					if (ElleSet->IsCloaked())
					{
						return true;
					}
					CoreActor->GrantTo(Enemies[0], ETraceCoreGrantReason::Kill);
					State->Phase = 9;
					State->PhaseStartReal = NowReal;
					return true;
				}

				if (!ATraceCore::IsCoreHolder(EllePawn) && CoreActor != nullptr)
				{
					CoreActor->TryPickup(EllePawn);
				}

				if ((NowReal - State->PhaseStartReal) > 10.0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[ELLEVERIFY]   SKIPPED the 'losing it to an enemy must not cloak' case — could not "
						     "stage it (enemies=%d, Elle holds it=%d)."),
						Enemies.Num(), ATraceCore::IsCoreHolder(EllePawn) ? 1 : 0);
					State->Phase = 10;
				}
				return true;
			}

			if (State->Phase == 9)
			{
				if (ElleSet->IsCloaked())
				{
					State->bCloakedAfterEnemyGrant = true;
				}
				if ((NowReal - State->PhaseStartReal) < 1.0)
				{
					return true;
				}

				State->Check(!State->bCloakedAfterEnemyGrant,
					TEXT("LOSING the Core to an enemy does NOT cloak — 'right after passing or throwing' is not "
					     "'right after ceasing to hold', and killing Elle for the Core must not reward her"));

				State->Phase = 10;
				return true;
			}

			// ---- Phase 10: verdict ---------------------------------------------------------------
			//
			// INVALID is printed in the headline count rather than folded into either column. A run
			// that could not stage a case must not read like a run that tested it.
			UE_LOG(LogTraceGame, Display,
				TEXT("[ELLEVERIFY] ===== %d passed, %d failed, %d could not be staged (INVALID) ====="),
				State->Passed, State->Failed, State->Invalid);
			if (State->Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[ELLEVERIFY] VERDICT: PASS"));
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[ELLEVERIFY] VERDICT: *** FAIL *** (%d)"), State->Failed);
			}
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Elle.RemoteCloak — the half of "it replicates" that a listen server structurally cannot
	// prove about itself.
	//
	// Trace.Elle.Verify asserts the cloak is a replicated FACT and that the paint went on. Run on a
	// listen server, both of those are read on the machine that WROTE them, so they would still pass
	// on a build where the cloak never left the server — which is precisely the failure §2 calls
	// worthless ("a cloak only the cloaked player sees").
	//
	// So this runs on a SECOND PROCESS, a real client, and watches SOMEBODY ELSE'S Elle: the fact must
	// arrive over the wire, and the client must have repainted its own copy of her body off it. It
	// asserts nothing about the server and takes no part in staging — the host's Trace.Elle.Verify
	// raises the cloak by completing a real pass, and this only reports what reached here.
	// =============================================================================================

	struct FElleRemoteState
	{
		double StartReal = 0.0;
		double WatchSeconds = 40.0;

		bool bSawRemoteElle = false;
		bool bSawFactArrive = false;
		bool bSawPaintApplied = false;
		FString ElleName;
	};

	void RunElleRemoteCloak()
	{
		UWorld* WorldPtr = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* Candidate = Context.World();
				if (Candidate != nullptr && Candidate->IsGameWorld())
				{
					WorldPtr = Candidate;
					break;
				}
			}
		}
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ELLEREMOTE] no game world."));
			return;
		}

		if (WorldPtr->GetNetMode() != NM_Client)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ELLEREMOTE] INVALID — this is not a client (netMode=%d). The whole point is to read the "
				     "cloak on a machine that did NOT write it; run it in a second process joined to a host."),
				static_cast<int32>(WorldPtr->GetNetMode()));
			return;
		}

		TSharedPtr<FElleRemoteState> State = MakeShared<FElleRemoteState>();
		State->StartReal = FPlatformTime::Seconds();

		UE_LOG(LogTraceGame, Display,
			TEXT("[ELLEREMOTE] ===== watching a REMOTE Elle for %.0fs. The cloak must arrive as replicated state "
			     "AND this machine must have repainted her from it. ====="), State->WatchSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}

			const AGameStateBase* BaseState = TickWorld->GetGameState();
			const APlayerController* LocalPC = TickWorld->GetFirstPlayerController();
			const APlayerState* LocalState = (LocalPC != nullptr) ? LocalPC->PlayerState : nullptr;

			if (BaseState != nullptr)
			{
				for (APlayerState* Entry : BaseState->PlayerArray)
				{
					// SOMEBODY ELSE'S Elle. Reading our own would put us back on the machine that wrote
					// it, which is the thing this command exists not to do.
					if (Entry == nullptr || Entry == LocalState)
					{
						continue;
					}
					UTraceAbilityComponent* Comp = Entry->FindComponentByClass<UTraceAbilityComponent>();
					UTraceAbilitySetElle* ElleSet = (Comp != nullptr)
						? Comp->GetAbilitySetAs<UTraceAbilitySetElle>() : nullptr;
					if (ElleSet == nullptr)
					{
						continue;
					}

					State->bSawRemoteElle = true;
					State->ElleName = GetNameSafe(Entry);

					if (ElleSet->IsCloaked())
					{
						State->bSawFactArrive = true;
						if (ElleSet->IsCloakVisualApplied())
						{
							State->bSawPaintApplied = true;
						}
					}
				}
			}

			if ((FPlatformTime::Seconds() - State->StartReal) < State->WatchSeconds)
			{
				return true;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[ELLEREMOTE] remote Elle seen=%d (%s) | cloak FACT arrived=%d | this client REPAINTED her=%d"),
				State->bSawRemoteElle ? 1 : 0, *State->ElleName,
				State->bSawFactArrive ? 1 : 0, State->bSawPaintApplied ? 1 : 0);

			if (!State->bSawRemoteElle)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[ELLEREMOTE] VERDICT: INVALID — no OTHER player was running Elle on this client, so "
					     "nothing was watched. The host must be Elle and must raise a cloak during the window."));
			}
			else if (State->bSawFactArrive && State->bSawPaintApplied)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[ELLEREMOTE] VERDICT: PASS — the cloak crossed the wire and this client, which never "
					     "wrote it, repainted her from the replicated fact. §2's 'it must replicate' holds off "
					     "the server as well as on it."));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[ELLEREMOTE] VERDICT: *** FAIL *** — the cloak did not reach this client as both a fact "
					     "and a repaint. A cloak only the cloaked player can see is worthless."));
			}
			return false;
		}));
	}

	// =============================================================================================
	// Registration
	// =============================================================================================

	FAutoConsoleCommand CmdElleCarrierTest(
		TEXT("Trace.Elle.CarrierTest"),
		TEXT("SPEC v18 §2 x THE FOUNDING INVARIANT. Two arms, RED first: an enemy Core carrier standing "
		     "in one of Elle's gates MUST be teleported with Trace.Ability.CarrierImmune 0 and MUST NOT "
		     "be with the shipped rule, while a non-carrier enemy in the same mouth is teleported in "
		     "both. Restores the arm itself."),
		FConsoleCommandDelegate::CreateStatic(&RunElleCarrierTest));

	FAutoConsoleCommand CmdElleVerify(
		TEXT("Trace.Elle.Verify"),
		TEXT("SPEC v18 §2 Elle, end to end: the cloak from a real pass (and NOT from a loss), SNAP's "
		     "two-press shape, window, pair lifetime and cooldown, a team-mate stepping through, and the "
		     "Elle-only slide-jump derivation. Red arms: Trace.Elle.CloakEnabled 0, Trace.Elle.SnapEnabled "
		     "0, Trace.Elle.SnapTeleportEnabled 0."),
		FConsoleCommandDelegate::CreateStatic(&RunElleVerify));

	FAutoConsoleCommand CmdElleRemoteCloak(
		TEXT("Trace.Elle.RemoteCloak"),
		TEXT("SPEC v18 §2, the replication half. Run on a CLIENT while a host runs Trace.Elle.Verify: "
		     "watches somebody ELSE'S Elle and reports whether the cloak arrived as replicated state and "
		     "whether this machine repainted her from it. Refuses to run anywhere but a client."),
		FConsoleCommandDelegate::CreateStatic(&RunElleRemoteCloak));
}

#endif   // !UE_BUILD_SHIPPING
