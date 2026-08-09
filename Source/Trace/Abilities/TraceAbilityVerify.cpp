// Trace — the ability framework's evidence.
//
// Four console commands, and two of them are real staged scenarios rather than assertions about
// code somebody has already read:
//
//   Trace.Ability.CarrierChokeTest        SPEC §4. TWO ARMS, RED FIRST. Fires ability damage at a
//                                         LIVE Core carrier with the rule REMOVED (which must
//                                         damage them, or the fixture never reached the carrier and
//                                         its green means nothing), then again with the shipped
//                                         rule (which must not).
//   Trace.Ability.CooldownPersistenceTest SPEC §5. Puts a cooldown on a player, KILLS them, waits
//                                         for the respawn, and checks the cooldown survived the
//                                         pawn being destroyed and replaced. Then calls half time
//                                         and checks it is cleared. Reports INVALID if the pawn was
//                                         not actually replaced — a test that never respawned
//                                         anybody proves nothing about surviving a respawn.
//   Trace.Ability.DumpState               Who is what, and what their cooldown says.
//   Trace.Ability.SetCharacter            Drive the framework before the select screen exists.
//
// ===================================================================================================
// WHY THE CHOKE TEST FIRES TWICE PER ARM, AND WHY IT DOES NOT JUDGE ON HEALTH ALONE
// ===================================================================================================
//
// A Core carrier ALSO has the ordinary damage shield (UTraceHealthComponent::IsInvulnerable), so
// "the carrier's health did not move" is true on a build with no carrier rule at all. Judging on
// health alone would therefore produce exactly the uninformative green this project has been bitten
// by twice. Worse, the usual way to drop that shield — forcing the hover-pass window open, which is
// what the knife's harness does — DOES NOT WORK IN MODE B: mode B has no hover-pass (LMB throws), so
// the forced window is cancelled on the very next tick. Mode B is now the default, so that route is
// gone.
//
// So each arm fires TWO strikes with the same call:
//
//   THE CARRIER STRIKE   the case under test. Judged on ApplyAbilityDamage's RETURN VALUE, which is
//                        the choke point's own output and cannot be confounded by the shield, by
//                        team rules or by anything else. Health and the alarm counter are printed
//                        beside it, and are judged too whenever the shield happened to be down.
//
//   THE CONTROL STRIKE   the identical call against a living NON-CARRIER enemy. This is the fixture
//                        proving itself: if the control strike moves health, then the pipeline
//                        reaches health, the attacker is a valid instigator, the teams are right and
//                        the damage number is real — so a zero on the carrier strike means the
//                        carrier rule and nothing else. If the control strike moves NOTHING, the
//                        whole run is INVALID and says so, because then a zero proves nothing.
//
// The verdict requires the RED arm to reproduce (carrier strike > 0 with the rule removed) AND the
// control strike to have worked. A green arm on top of a red arm that never reproduced is reported
// as INVALID, never as a pass.
//
// TIMING IS REAL TIME (FPlatformTime::Seconds), NOT WORLD TIME, and that is not a style choice: the
// character-select screen pauses the world while it is up, world time stops advancing, and a harness
// waiting on world time waits forever. The first run of this harness did exactly that.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                  // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Abilities/TraceAbilityWorldSubsystem.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceAbilityVerify
{
	/** The one authoritative game world, or null. Every command here is server only. */
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

	/** A living enemy of @p Victim who has an ability component to attack from. */
	UTraceAbilityComponent* FindAttackerFor(UWorld* WorldPtr, const ATraceCharacter* Victim)
	{
		if (WorldPtr == nullptr || Victim == nullptr)
		{
			return nullptr;
		}
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Victim || !Candidate->IsAlive())
			{
				continue;
			}
			if (Candidate->GetTeam() == Victim->GetTeam() || Candidate->GetTeam() == ETraceTeam::None)
			{
				continue;
			}
			if (UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Candidate))
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/**
	 * THE CONTROL TARGET: a living enemy of @p AttackerComp who is NOT the carrier.
	 *
	 * The whole fixture-validity argument rests on this pawn. The identical ApplyAbilityDamage call
	 * that returns 0 on the carrier must return a real number and move real health on this one, or
	 * the harness has proved nothing about the carrier rule and everything about the harness.
	 */
	ATraceCharacter* FindControlTarget(UWorld* WorldPtr, const UTraceAbilityComponent* AttackerComp)
	{
		if (WorldPtr == nullptr || AttackerComp == nullptr)
		{
			return nullptr;
		}
		const ETraceTeam AttackerTeam = AttackerComp->GetTeam();
		const ATraceCharacter* AttackerPawn = AttackerComp->GetOwningCharacter();

		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == AttackerPawn || !Candidate->IsAlive())
			{
				continue;
			}
			if (UTraceAbilityComponent::IsCarrier(Candidate))
			{
				continue;   // the carrier is the case under test, not the control
			}
			if (Candidate->GetTeam() == AttackerTeam || Candidate->GetTeam() == ETraceTeam::None)
			{
				continue;
			}
			// Full health, so the damage has somewhere to go and a regen tick cannot mask it.
			if (Candidate->Health != nullptr)
			{
				Candidate->Health->ResetHealth();
				return Candidate;
			}
		}
		return nullptr;
	}

	// =============================================================================================
	// Trace.Ability.CarrierChokeTest — SPEC §4, two arms, red first
	// =============================================================================================

	struct FChokeTestState
	{
		/** 0 = RED (Trace.Ability.CarrierImmune 0, the rule removed). 1 = GREEN (shipped). */
		int32 Arm = 0;

		/** -1 acquire, 0 settle, 1 strike-and-measure. */
		int32 Phase = -1;

		/** REAL time, not world time. The select screen pauses the world; see the file header. */
		double StartRealTime = 0.0;
		double AcquireDeadline = 0.0;

		TWeakObjectPtr<ATraceCharacter> Victim;
		TWeakObjectPtr<ATraceCharacter> ControlTarget;
		TWeakObjectPtr<UTraceAbilityComponent> Attacker;

		// ---- the carrier strike: the case under test ----
		float CarrierHealthBefore = -1.f;
		float CarrierDamageReturned = -1.f;
		bool  bShieldDownAtStrike = false;
		int32 AlarmBefore = 0;

		// ---- the control strike: the fixture proving itself ----
		float ControlHealthBefore = -1.f;
		float ControlDamageReturned = -1.f;

		/** Per-arm outcomes, filled as each arm completes. */
		bool bRedArmRan = false;
		bool bRedArmReachedCarrier = false;
		bool bRedArmControlWorked = false;
		float RedArmCarrierDamage = 0.f;
		bool bGreenArmRan = false;
		bool bGreenArmClean = false;
		bool bGreenArmControlWorked = false;
	};

	/** The damage the staged strikes ask for. Unambiguous, and well short of lethal. */
	constexpr float ChokeTestDamage = 25.f;

	IConsoleVariable* AbilityCarrierImmuneCVar()
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Ability.CarrierImmune"));
	}

	void RestoreArm()
	{
		if (IConsoleVariable* Arm = AbilityCarrierImmuneCVar())
		{
			Arm->Set(1, ECVF_SetByConsole);
		}
	}

	void RunCarrierChokeTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ABILITYCHOKE] no authoritative game world — run this on the server."));
			return;
		}
		if (AbilityCarrierImmuneCVar() == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ABILITYCHOKE] Trace.Ability.CarrierImmune is not registered — cannot arm the test."));
			return;
		}

		TSharedPtr<FChokeTestState> State = MakeShared<FChokeTestState>();
		State->Arm = 0;                                            // RED FIRST. Always.
		State->StartRealTime = FPlatformTime::Seconds();
		State->AcquireDeadline = State->StartRealTime + 120.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ABILITYCHOKE] ===== spec v14 §4: 'NO abilities damage carriers'. Arm 0 = RED (rule REMOVED, "
			     "MUST reach the carrier). Arm 1 = GREEN (shipped rule, must NOT). Each arm also fires a CONTROL "
			     "strike at a non-carrier enemy, which must land — that is the fixture proving itself. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ABILITYCHOKE] ABORTED: the world went away mid-test."));
				RestoreArm();
				return false;
			}

			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase -1: acquire a live carrier, an enemy attacker and a control target -------
			if (State->Phase < 0)
			{
				ATraceCharacter* NewVictim = (CoreActor != nullptr) ? CoreActor->Carrier : nullptr;

				// No holder at all (a loose Core in mode B, or the very start of a half): grant it to
				// somebody so the scenario can exist. TryPickup is a no-op when there IS a holder.
				if (NewVictim == nullptr && CoreActor != nullptr)
				{
					for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
					{
						if (*It != nullptr && (*It)->IsAlive())
						{
							CoreActor->TryPickup(*It);
							break;
						}
					}
					NewVictim = CoreActor->Carrier;
				}

				UTraceAbilityComponent* NewAttacker = (NewVictim != nullptr && NewVictim->IsAlive())
					? FindAttackerFor(TickWorld, NewVictim) : nullptr;
				ATraceCharacter* NewControl = FindControlTarget(TickWorld, NewAttacker);

				if (NewVictim == nullptr || NewAttacker == nullptr || NewControl == nullptr)
				{
					if (NowReal > State->AcquireDeadline)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[ABILITYCHOKE] arm=%d SKIPPED: could not stage the scenario inside the acquire "
							     "window (carrier=%s attacker=%s control=%s)."),
							State->Arm, *GetNameSafe(NewVictim),
							(NewAttacker != nullptr) ? TEXT("yes") : TEXT("none"), *GetNameSafe(NewControl));
						RestoreArm();
						return false;
					}
					return true;
				}

				State->Victim = NewVictim;
				State->Attacker = NewAttacker;
				State->ControlTarget = NewControl;
				State->StartRealTime = NowReal;
				State->Phase = 0;

				// THE ARM IS APPLIED HERE, not by the caller, so the command carries its own
				// falsification and there is no way to run it that skips the red half.
				AbilityCarrierImmuneCVar()->Set(State->Arm, ECVF_SetByConsole);

				UE_LOG(LogTraceGame, Display,
					TEXT("[ABILITYCHOKE] --- arm=%d (%s) | attacker %s (team %d) | CARRIER %s (team %d, health %.0f) "
					     "| CONTROL %s (team %d, health %.0f)"),
					State->Arm, (State->Arm == 0) ? TEXT("RED, rule removed") : TEXT("GREEN, shipped rule"),
					*GetNameSafe(NewAttacker->GetOwningCharacter()), static_cast<int32>(NewAttacker->GetTeam()),
					*GetNameSafe(NewVictim), static_cast<int32>(NewVictim->GetTeam()),
					(NewVictim->Health != nullptr) ? NewVictim->Health->Health : -1.f,
					*GetNameSafe(NewControl), static_cast<int32>(NewControl->GetTeam()),
					(NewControl->Health != nullptr) ? NewControl->Health->Health : -1.f);
				return true;
			}

			ATraceCharacter* Victim = State->Victim.Get();
			ATraceCharacter* Control = State->ControlTarget.Get();
			UTraceAbilityComponent* Attacker = State->Attacker.Get();
			if (Victim == nullptr || Control == nullptr || Attacker == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ABILITYCHOKE] ABORTED: a participant went away mid-test."));
				RestoreArm();
				return false;
			}

			// PRECONDITION, held every tick: the Core must stay on the victim. Bots pass and score,
			// and a victim who stopped being the carrier between staging and strike would turn the
			// case under test into an ordinary damage test that passes for the wrong reason.
			if (CoreActor != nullptr && CoreActor->Carrier != Victim)
			{
				CoreActor->TryPickup(Victim);
			}

			// ---- Phase 0: one settle tick, then both strikes ------------------------------------
			if (State->Phase == 0)
			{
				if ((NowReal - State->StartRealTime) < 0.20)
				{
					return true;
				}

				// The shield state is RECORDED, not required. In mode B it will be up, because mode B
				// has no hover-pass to suppress it — which is exactly why the verdict below does not
				// rest on the carrier's health. See the file header.
				State->bShieldDownAtStrike = ATraceCore::IsShieldSuppressedFor(Victim);
				State->AlarmBefore = TraceAbility::GetCarrierAbilityDamageHitCount();

				State->CarrierHealthBefore = (Victim->Health != nullptr) ? Victim->Health->Health : -1.f;
				State->ControlHealthBefore = (Control->Health != nullptr) ? Control->Health->Health : -1.f;

				// *** THE TWO STRIKES. Identical call, one carrier and one non-carrier. ***
				State->CarrierDamageReturned = Attacker->ApplyAbilityDamage(
					Victim, ChokeTestDamage, TEXT("AbilityChokeTest"), /*bMelee*/ false, /*bHeadshot*/ false);

				State->ControlDamageReturned = Attacker->ApplyAbilityDamage(
					Control, ChokeTestDamage, TEXT("AbilityChokeControl"), /*bMelee*/ false, /*bHeadshot*/ false);

				State->Phase = 1;
				State->StartRealTime = NowReal;
				return true;
			}

			// ---- Phase 1: measure ------------------------------------------------------------------
			if ((NowReal - State->StartRealTime) < 0.10)
			{
				return true;
			}

			const float CarrierHealthAfter = (Victim->Health != nullptr) ? Victim->Health->Health : -1.f;
			const float ControlHealthAfter = (Control->Health != nullptr) ? Control->Health->Health : -1.f;
			const float CarrierDelta = State->CarrierHealthBefore - CarrierHealthAfter;
			const float ControlDelta = State->ControlHealthBefore - ControlHealthAfter;
			const int32 AlarmDelta = TraceAbility::GetCarrierAbilityDamageHitCount() - State->AlarmBefore;
			const bool  bStillCarrier = (CoreActor != nullptr) && CoreActor->Carrier == Victim;

			const bool bControlWorked = (State->ControlDamageReturned > 0.f) && (ControlDelta > 0.01f);

			UE_LOG(LogTraceGame, Display,
				TEXT("[ABILITYCHOKE] arm=%d CARRIER strike: choke returned %.1f | health %.1f -> %.1f (delta %.1f) | "
				     "shieldDownAtStrike=%d stillCarrier=%d alarmDelta=%d"),
				State->Arm, State->CarrierDamageReturned, State->CarrierHealthBefore, CarrierHealthAfter,
				CarrierDelta, State->bShieldDownAtStrike ? 1 : 0, bStillCarrier ? 1 : 0, AlarmDelta);

			UE_LOG(LogTraceGame, Display,
				TEXT("[ABILITYCHOKE] arm=%d CONTROL strike (non-carrier enemy, IDENTICAL call): choke returned %.1f | "
				     "health %.1f -> %.1f (delta %.1f) -> fixture %s"),
				State->Arm, State->ControlDamageReturned, State->ControlHealthBefore, ControlHealthAfter,
				ControlDelta, bControlWorked ? TEXT("REACHES HEALTH") : TEXT("*** DEAD - measures nothing ***"));

			if (State->Arm == 0)
			{
				// RED. The rule is removed, so the choke point MUST have let the carrier damage through.
				State->bRedArmRan = true;
				State->bRedArmReachedCarrier = (State->CarrierDamageReturned > 0.f);
				State->bRedArmControlWorked = bControlWorked;
				State->RedArmCarrierDamage = State->CarrierDamageReturned;

				UE_LOG(LogTraceGame, Display,
					TEXT("[ABILITYCHOKE] arm=0 (RED) %s — with the rule removed the choke point %s the carrier strike."),
					State->bRedArmReachedCarrier ? TEXT("REPRODUCED") : TEXT("*** DID NOT REPRODUCE ***"),
					State->bRedArmReachedCarrier ? TEXT("passed") : TEXT("still refused"));

				// Heal both back so arm 1 starts from a comparable state and the red arm's damage
				// cannot be mistaken for the green arm's.
				if (Victim->Health != nullptr)  { Victim->Health->ResetHealth(); }
				if (Control->Health != nullptr) { Control->Health->ResetHealth(); }

				State->Arm = 1;
				State->Phase = -1;
				State->StartRealTime = NowReal;
				State->AcquireDeadline = NowReal + 120.0;
				return true;
			}

			// GREEN. The shipped rule. Nothing may have got through, by any measure.
			State->bGreenArmRan = true;
			State->bGreenArmControlWorked = bControlWorked;
			State->bGreenArmClean = (State->CarrierDamageReturned <= 0.f)
				&& (CarrierDelta <= 0.01f)
				&& (AlarmDelta == 0);

			RestoreArm();

			// ---- The verdict ------------------------------------------------------------------------
			//
			// THE FIXTURE IS JUDGED BEFORE THE RULE IS. Two independent ways for this run to be worth
			// nothing, and both are checked out loud:
			//   * the RED arm never reached the carrier — then a green arm 1 is not evidence;
			//   * the CONTROL strike never moved health — then the call under test reaches nothing at
			//     all and every zero in the run is the fixture's, not the rule's.
			const bool bFixtureValid = State->bRedArmRan
				&& State->bRedArmReachedCarrier
				&& State->bRedArmControlWorked
				&& State->bGreenArmControlWorked;

			if (!bFixtureValid)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[ABILITYCHOKE] VERDICT: INVALID — redArmReachedCarrier=%d redArmControlLanded=%d "
					     "greenArmControlLanded=%d. The fixture did not demonstrate that it can reach a carrier with "
					     "the rule removed AND move health on a normal target, so arm 1 passing says nothing about "
					     "the rule. Fix the fixture before trusting the green."),
					State->bRedArmReachedCarrier ? 1 : 0, State->bRedArmControlWorked ? 1 : 0,
					State->bGreenArmControlWorked ? 1 : 0);
			}
			else if (State->bGreenArmClean)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[ABILITYCHOKE] VERDICT: PASS — arm 0 (rule removed) got %.1f damage through to the Core "
					     "carrier; arm 1 (shipped rule) returned 0, moved 0 health and tripped the alarm 0 times, "
					     "while the IDENTICAL call still did %.1f damage to a non-carrier enemy in the same frame. "
					     "Spec v14 §4 holds at the choke point, the rule is carrier-specific, and the harness is "
					     "proven able to fail."),
					State->RedArmCarrierDamage, ControlDelta);
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[ABILITYCHOKE] VERDICT: *** FAIL *** — the SHIPPED rule let ability damage reach the Core "
					     "carrier (returned %.1f, health delta %.1f, alarm +%d). Spec v14 §4 is broken. See "
					     "UTraceAbilityComponent::CanAffectTargetDetailed."),
					State->CarrierDamageReturned, CarrierDelta, AlarmDelta);
			}

			return false;
		}));
	}

	FAutoConsoleCommand CmdCarrierChokeTest(
		TEXT("Trace.Ability.CarrierChokeTest"),
		TEXT("Dev only, server only. SPEC v14 §4. Runs BOTH arms, RED first: fires ability damage at a live "
		     "Core carrier with the rule removed (must damage), then with the shipped rule (must not). Arm 0 "
		     "must reproduce or the verdict is INVALID."),
		FConsoleCommandDelegate::CreateStatic(&RunCarrierChokeTest));

	// =============================================================================================
	// Trace.Ability.CooldownPersistenceTest — SPEC §5
	// =============================================================================================

	struct FCooldownTestState
	{
		int32 Phase = 0;
		/** REAL time. The select screen pauses the world; a world-time wait would never expire. */
		double StartRealTime = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;
		TWeakObjectPtr<APawn> PawnBefore;

		float SetDeadline = 0.f;
		float RemainingBeforeDeath = 0.f;
		float RemainingAfterRespawn = 0.f;
		float RemainingAfterHalfTime = -1.f;
		bool  bPawnActuallyReplaced = false;
	};

	/** Long enough that it cannot expire during the respawn delay and confound the measurement. */
	constexpr float CooldownTestSeconds = 45.f;

	void RunCooldownPersistenceTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ABILITYCD] no authoritative game world — run this on the server."));
			return;
		}

		// Any player with a component and a living pawn. Bots do fine: the cooldown storage is not a
		// character feature, and this test is about the STORAGE surviving a pawn being destroyed.
		UTraceAbilityComponent* Subject = nullptr;
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate != nullptr && Candidate->IsAlive() && !Candidate->IsCarrier())
			{
				if (UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Candidate))
				{
					Subject = Comp;
					break;
				}
			}
		}

		if (Subject == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ABILITYCD] SKIPPED: no living non-carrier pawn with an ability component."));
			return;
		}

		TSharedPtr<FCooldownTestState> State = MakeShared<FCooldownTestState>();
		State->Subject = Subject;
		State->PawnBefore = Subject->GetOwningCharacter();
		State->StartRealTime = FPlatformTime::Seconds();

		UE_LOG(LogTraceGame, Display,
			TEXT("[ABILITYCD] ===== spec v14 §5: cooldowns run on the MATCH CLOCK. Subject %s, pawn %s. ====="),
			*GetNameSafe(Subject->GetOwningPlayerState()), *GetNameSafe(State->PawnBefore.Get()));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			UTraceAbilityComponent* Comp = State->Subject.Get();
			if (TickWorld == nullptr || Comp == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[ABILITYCD] ABORTED: subject or world went away."));
				return false;
			}

			const double Elapsed = FPlatformTime::Seconds() - State->StartRealTime;

			// ---- Phase 0: put a cooldown on, then kill the pawn ---------------------------------
			if (State->Phase == 0)
			{
				Comp->DebugSetActivatedCooldown(CooldownTestSeconds);
				State->RemainingBeforeDeath = Comp->GetActivatedCooldownRemaining();

				ATraceCharacter* Victim = Comp->GetOwningCharacter();
				if (Victim == nullptr || Victim->Health == nullptr)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[ABILITYCD] SKIPPED: subject lost its pawn before the kill."));
					return false;
				}

				UE_LOG(LogTraceGame, Display, TEXT("[ABILITYCD] cooldown set to %.1fs, killing pawn %s."),
					State->RemainingBeforeDeath, *GetNameSafe(Victim));

				Victim->Health->Kill(nullptr, TEXT("AbilityCooldownTest"));

				State->Phase = 1;
				State->StartRealTime = FPlatformTime::Seconds();
				return true;
			}

			// ---- Phase 1: wait for a NEW pawn ----------------------------------------------------
			if (State->Phase == 1)
			{
				APawn* PawnNow = Comp->GetOwningCharacter();
				const bool bNewPawn = (PawnNow != nullptr) && (PawnNow != State->PawnBefore.Get());

				if (!bNewPawn)
				{
					// 90s of REAL time, not 20 of world time. The respawn delay is a world-time timer
					// and the world is PAUSED while the character-select screen is up, so a tight
					// budget here reports INVALID on a build where everything works.
					if (Elapsed > 90.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[ABILITYCD] VERDICT: INVALID — no replacement pawn arrived within 90s of real time, "
							     "so the respawn this test is about never happened. Nothing was measured."));
						return false;
					}
					return true;
				}

				State->bPawnActuallyReplaced = true;
				State->RemainingAfterRespawn = Comp->GetActivatedCooldownRemaining();

				UE_LOG(LogTraceGame, Display,
					TEXT("[ABILITYCD] respawned: pawn %s -> %s | cooldown %.1fs -> %.1fs"),
					*GetNameSafe(State->PawnBefore.Get()), *GetNameSafe(PawnNow),
					State->RemainingBeforeDeath, State->RemainingAfterRespawn);

				// ---- and now half time, which is the ONE thing that may clear it -----------------
				if (UTraceAbilityWorldSubsystem* Subsystem = UTraceAbilityWorldSubsystem::Get(TickWorld))
				{
					Subsystem->NotifyHalfTime();
				}
				State->RemainingAfterHalfTime = Comp->GetActivatedCooldownRemaining();

				// The verdict. Both halves must hold, and the fixture check comes first.
				const bool bSurvivedDeath = State->RemainingAfterRespawn > 1.f;
				const bool bClearedAtHalfTime = State->RemainingAfterHalfTime <= 0.01f;

				if (!State->bPawnActuallyReplaced)
				{
					UE_LOG(LogTraceGame, Error, TEXT("[ABILITYCD] VERDICT: INVALID — the pawn was never replaced."));
				}
				else if (bSurvivedDeath && bClearedAtHalfTime)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[ABILITYCD] VERDICT: PASS — the pawn was DESTROYED and REPLACED (%s -> %s) and the "
						     "cooldown kept counting through it (%.1fs -> %.1fs, spec §5 'a player can spawn with an "
						     "ability timer still counting down'); half time then cleared it to %.1fs."),
						*GetNameSafe(State->PawnBefore.Get()), *GetNameSafe(PawnNow),
						State->RemainingBeforeDeath, State->RemainingAfterRespawn, State->RemainingAfterHalfTime);
				}
				else
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[ABILITYCD] VERDICT: *** FAIL *** — survivedDeath=%d (%.1fs after respawn, expected >1) "
						     "clearedAtHalfTime=%d (%.1fs, expected 0)."),
						bSurvivedDeath ? 1 : 0, State->RemainingAfterRespawn,
						bClearedAtHalfTime ? 1 : 0, State->RemainingAfterHalfTime);
				}

				return false;
			}

			return false;
		}));
	}

	FAutoConsoleCommand CmdCooldownPersistenceTest(
		TEXT("Trace.Ability.CooldownPersistenceTest"),
		TEXT("Dev only, server only. SPEC v14 §5. Puts a cooldown on a player, kills them, waits for the "
		     "replacement pawn and checks the cooldown survived; then calls half time and checks it cleared."),
		FConsoleCommandDelegate::CreateStatic(&RunCooldownPersistenceTest));

	// =============================================================================================
	// Trace.Ability.DumpState / Trace.Ability.SetCharacter
	// =============================================================================================

	/**
	 * Dumps once, and — on a CLIENT that has not received anything yet — keeps retrying.
	 *
	 * THIS RETRY IS THE WHOLE POINT ON A CLIENT. The ability component is created on the server at
	 * runtime and reaches clients as a replicated dynamic subobject, which lands some time AFTER the
	 * join completes. -ExecCmds fires before the client has even been welcomed, so a single-shot dump
	 * on a client always prints "0 components" and would look exactly like replication being broken.
	 * Retrying until something arrives (or the budget runs out) is what makes the two outcomes
	 * distinguishable, and the budget expiring is a real, reportable failure.
	 */
	void DumpAbilityStateWithRetry(int32 AttemptsLeft);

	void DumpAbilityState()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			// A client can still dump what it has — that is the interesting case for replication.
			if (GEngine != nullptr)
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
		}
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[AbilityState] no game world."));
			return;
		}

		const ATraceGameState* TraceGS = WorldPtr->GetGameState<ATraceGameState>();
		const bool bEnabled = UTraceAbilityComponent::AreCharactersEnabled(WorldPtr);

		UE_LOG(LogTraceGame, Display,
			TEXT("[AbilityState] ===== netmode=%d | scoringMode=%s | charactersEnabled=%d (setting=%d) | "
			     "matchClock=%.2f | carrierControlImmune=%d ====="),
			static_cast<int32>(WorldPtr->GetNetMode()),
			(TraceGS != nullptr && TraceGS->IsGoalMode()) ? TEXT("B-ThrownCoreAndGoals") : TEXT("A-EndzoneStatusCore"),
			bEnabled ? 1 : 0, UTraceSettings::Get().bCharactersEnabled ? 1 : 0,
			(TraceGS != nullptr) ? TraceGS->GetServerWorldTimeSeconds() : 0.0,
			UTraceSettings::Get().bCarrierImmuneToAbilityControl ? 1 : 0);

		TArray<UTraceAbilityComponent*> Components;
		UTraceAbilityWorldSubsystem::GatherAllComponents(WorldPtr, Components);

		if (Components.Num() == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[AbilityState]   NO ability components found. On a client this means the component has not "
				     "replicated; on the server it means the world subsystem has not attached them."));
		}

		for (const UTraceAbilityComponent* Comp : Components)
		{
			const FTraceAbilityNetState& NetState = Comp->GetNetState();
			UE_LOG(LogTraceGame, Display,
				TEXT("[AbilityState]   %-24s bot=%d team=%d character=%-7s set=%-28s cooldown=%6.2fs "
				     "carrier=%d | state stacks=%d flags=0x%02x effectEnd=%.2f auxEnd=%.2f"),
				*GetNameSafe(Comp->GetOwner()), Comp->IsBot() ? 1 : 0, static_cast<int32>(Comp->GetTeam()),
				TraceCharacterIdToString(Comp->GetCharacterId()),
				(Comp->GetAbilitySet() != nullptr) ? *Comp->GetAbilitySet()->GetClass()->GetName() : TEXT("<none>"),
				Comp->GetActivatedCooldownRemaining(),
				UTraceAbilityComponent::IsCarrier(Comp->GetOwningCharacter()) ? 1 : 0,
				NetState.Stacks, NetState.Flags, NetState.EffectEndMatchTime, NetState.AuxEndMatchTime);
		}

		UE_LOG(LogTraceGame, Display, TEXT("[AbilityState] %d component(s). Alarm (ability damage onto a carrier) = %d, must be 0."),
			Components.Num(), TraceAbility::GetCarrierAbilityDamageHitCount());
	}

	void DumpAbilityStateWithRetry(int32 AttemptsLeft)
	{
		TArray<UTraceAbilityComponent*> Components;
		UWorld* WorldPtr = nullptr;
		if (GEngine != nullptr)
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
		if (WorldPtr != nullptr)
		{
			UTraceAbilityWorldSubsystem::GatherAllComponents(WorldPtr, Components);
		}

		if (Components.Num() > 0 || AttemptsLeft <= 0)
		{
			if (Components.Num() == 0)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[AbilityState] *** NOTHING ARRIVED. No ability component reached this machine inside the "
					     "retry budget. On a client that means the replicated dynamic subobject is not being "
					     "created — see UTraceAbilityWorldSubsystem::EnsureComponentsAttached. ***"));
			}
			DumpAbilityState();
			return;
		}

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[AttemptsLeft](float) -> bool
			{
				DumpAbilityStateWithRetry(AttemptsLeft - 1);
				return false;
			}), 1.0f);
	}

	FAutoConsoleCommand CmdDumpAbilityState(
		TEXT("Trace.Ability.DumpState"),
		TEXT("Dev only. Every player's character, cooldown and replicated ability state, plus the carrier alarm. "
		     "Retries for up to 30s when nothing has arrived yet, so it is usable from -ExecCmds on a client that "
		     "has not finished joining."),
		FConsoleCommandDelegate::CreateStatic([]() { DumpAbilityStateWithRetry(30); }));

	FAutoConsoleCommand CmdSetCharacter(
		TEXT("Trace.Ability.SetCharacter"),
		TEXT("Dev only, server only. Trace.Ability.SetCharacter <Rocco|Chut|Mace|Oyster|X|None> [PlayerNameSubstring] "
		     "— drives ServerSetCharacter directly, so the framework is testable before the select screen exists. "
		     "With no name it targets the first non-bot player."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[AbilityState] usage: Trace.Ability.SetCharacter <name> [playerSubstring]"));
				return;
			}

			UWorld* WorldPtr = FindAuthoritativeWorld();
			if (WorldPtr == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[AbilityState] server only."));
				return;
			}

			const ETraceCharacterId Wanted = TraceCharacterIdFromString(Args[0]);
			const FString NameFilter = (Args.Num() > 1) ? Args[1] : FString();

			TArray<UTraceAbilityComponent*> Components;
			UTraceAbilityWorldSubsystem::GatherAllComponents(WorldPtr, Components);

			for (UTraceAbilityComponent* Comp : Components)
			{
				const APlayerState* MyState = Comp->GetOwningPlayerState();
				if (MyState == nullptr)
				{
					continue;
				}
				if (!NameFilter.IsEmpty() && !MyState->GetPlayerName().Contains(NameFilter))
				{
					continue;
				}
				if (NameFilter.IsEmpty() && Comp->IsBot())
				{
					continue;
				}

				Comp->ServerSetCharacter(Wanted);
				UE_LOG(LogTraceGame, Display, TEXT("[AbilityState] %s asked for %s, now %s."),
					*MyState->GetPlayerName(), TraceCharacterIdToString(Wanted),
					TraceCharacterIdToString(Comp->GetCharacterId()));

				if (NameFilter.IsEmpty())
				{
					return;   // first non-bot only
				}
			}
		}));
}   // namespace TraceAbilityVerify

#endif // !UE_BUILD_SHIPPING
