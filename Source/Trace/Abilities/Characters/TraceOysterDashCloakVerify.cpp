// Trace — the evidence for Oyster's DASH CLOAK (Demo 35: "JUMPING DIRECTLY FOLLOWING A DASH CLOAKS
// YOU FOR 1S"), and for the two ways it used to miss the jump.
//
// ===================================================================================================
// THE TWO BUGS THIS PROVES GONE
// ===================================================================================================
//
//   (a) THE PASSIVE NEVER HEARD A JUMP A MOVEMENT KIT SPENT. The cloak hung off OnJumpPressed, which
//       UTraceAbilityComponent::HandleJumpPressed offers Activated -> Movement -> Passive and STOPS at
//       the first kit that consumes it. With JET BOOTS on movement, the air jump is consumed by
//       Rocco's kit before the passive is asked, so jumping out of an air dash never cloaked.
//
//   (b) THE SERVER NEVER HEARD A REMOTE CLIENT'S ORDINARY JUMP. ATracePlayerController::OnJumpStarted
//       sends ServerHandleJumpPressed only when a kit CONSUMED the press; an ordinary jump reaches
//       the server as the saved move's jump flag and nothing else. The cloak was server-only, so it
//       worked for the listen host and for nobody who had connected to one.
//
// The fix is UTraceCharacterAbilitySet::OnJumpPerformed: a non-consuming "a jump happened" delivered
// on the server to every equipped kit, from ACharacter::OnJumped (every engine jump, whichever
// machine pressed it), the buffered wall jump, and every press a kit consumed.
//
// ===================================================================================================
// TWO COMMANDS
// ===================================================================================================
//
//   Trace.Oyster.DashCloakVerify     Standalone or listen host, after the team screen is closed:
//       -bots=0 -TraceExecAt=10 -TraceExec="Trace.Teams.Close" -TraceExec2At=16 -TraceExec2="Trace.Oyster.DashCloakVerify"
//
//     NO DASH        a jump with no dash in front of it                         -> no cloak (control)
//     GROUND         BLINK on movement, a dash, then the jump key on the ground  -> cloak
//     JET BOOTS      JET BOOTS on movement, a dash, then the jump key on the ground -> cloak; then
//       GROUND       JET BOOTS' own second jump INSIDE the window                -> the deadline does
//                                                                                   not move (no re-arm)
//     JET BOOTS AIR  a dash that ends in the air, then the jump key: JET BOOTS
//                    consumes it and that second jump is the jump                -> cloak        [bug a]
//     REMOTE-SHAPED  a dash, then ACharacter::Jump() and NOTHING else — no
//                    HandleJumpPressed, no RPC. That is all the server receives
//                    from a remote client's ordinary press (FLAG_JumpPressed
//                    replayed through MoveAutonomous -> CheckJumpInput)          -> cloak        [bug b]
//     LATE           a dash, then a jump after the window has closed            -> no cloak (control)
//
//     And on GROUND, JET BOOTS GROUND and REMOTE-SHAPED, two faults inside Oyster's own kit that
//     switched a cloak off as soon as it went up: the dash must leave NO poison jar (DASH CLOAK is
//     not PICKLE JAR — the old IsSlot(Passive) guard could not tell them apart, and the jar at his
//     feet was broken by the very jump that cloaked him), and the cloak must still be up half its
//     duration later (the jar-count publish used to overwrite the cloak's bit).
//
//   Trace.Oyster.DashCloakClientProbe   On a REAL CLIENT connected to a listen server whose side ran
//       Trace.Oyster.DashCloakStageRemote (which gives every remote player JET BOOTS / DASH CLOAK /
//       RIPPLE and closes their select screen). The client dashes and presses its own jump key; the
//       server decides; the client reads the cloak off the replicated state:
//
//     JET BOOTS GROUND, JET BOOTS AIR, LATE — the same claims as above, judged on the client — plus
//     that the cloak REACHES the client within 0.25 s of the press. The state rides the PlayerState,
//     which replicates once a second unless pushed, and a 1 s cloak cannot wait for that.
//
//   Server:  "/Game/Maps/Arena?listen" -port=<p> -bots=0 -TraceExecAt=5 -TraceExec="Trace.Oyster.DashCloakStageRemote"
//   Client:  127.0.0.1:<p> -TraceExecAt=12 -TraceExec="Trace.Teams.Close" -TraceExec2At=22 -TraceExec2="Trace.Oyster.DashCloakClientProbe"
//
// THE RED RUNS. Every claim marked [bug a] / [bug b], and the no-jar and still-up claims, were run
// against the code before the fix (the production files reverted, this file and
// ATracePlayerController::DebugPressJump kept) and failed there, on the authority and on the real
// client. The no-re-arm claim cannot fail on that code — the second jump never reached the cloak —
// so it was shown red separately, with the one-dash-one-cloak latch deleted (the deadline moved
// 22.745 -> 22.858). The client's 0.25 s bound was measured at 0.364 s and 0.430 s before the push.
// The commit message records those runs. A harness never seen to fail is not evidence.
//
// TIMING IS REAL TIME (FPlatformTime::Seconds), like every character fixture: a paused world would
// stall a harness waiting on world time forever. The authoritative command unpauses and says so.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Abilities/TraceCharacterAbilitySet.h"
#include "Abilities/Characters/TraceAbilitySetOyster.h"
#include "Abilities/Characters/TraceAbilitySetRocco.h"
#include "Abilities/Characters/TraceVerifyLock.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceOysterDashCloakVerify
{
	const TCHAR* const VerifyCommandName = TEXT("Trace.Oyster.DashCloakVerify");

	enum class EArm : uint8
	{
		NoDash,
		Ground,
		JetBootsGround,
		JetBootsAir,
		RemoteShaped,
		Late,
	};

	const TCHAR* ArmLabel(EArm Arm)
	{
		switch (Arm)
		{
		case EArm::NoDash:         return TEXT("NO DASH");
		case EArm::Ground:         return TEXT("GROUND");
		case EArm::JetBootsGround: return TEXT("JET BOOTS GROUND");
		case EArm::JetBootsAir:    return TEXT("JET BOOTS AIR");
		case EArm::RemoteShaped:   return TEXT("REMOTE-SHAPED");
		case EArm::Late:           return TEXT("LATE");
		default:                   return TEXT("?");
		}
	}

	FTraceLoadout MakeLoadout(ETraceAbilityId MovementId)
	{
		FTraceLoadout Out;
		Out.Set(ETraceLoadoutSlot::Movement, MovementId);
		Out.Set(ETraceLoadoutSlot::Passive, ETraceAbilityId::DashCloak);
		Out.Set(ETraceLoadoutSlot::Activated, ETraceAbilityId::Ripple);
		return Out;
	}

	/** What each arm equips. A plain movement kit that never takes the jump, or the one that does. */
	FTraceLoadout LoadoutFor(EArm Arm)
	{
		const bool bJetBoots = (Arm == EArm::JetBootsGround || Arm == EArm::JetBootsAir);
		return MakeLoadout(bJetBoots ? ETraceAbilityId::JetBoots : ETraceAbilityId::Blink);
	}

	/** The one loadout the client probe runs under, staged by the server. */
	FTraceLoadout RemoteLoadout()
	{
		return MakeLoadout(ETraceAbilityId::JetBoots);
	}

	enum class EStage : uint8
	{
		Setup,
		WaitReady,
		AirPrep,
		Dashing,
		BeforePress,
		Press,
		Observe,
		Judge,
	};

	struct FRun
	{
		bool bClient = false;
		TArray<EArm> Arms;
		int32 ArmIndex = 0;
		EStage Stage = EStage::Setup;
		double StageStartReal = 0.0;

		TWeakObjectPtr<ATracePlayerController> PC;
		FTraceLoadout OriginalLoadout;

		// ---- one arm's observations, reset in Setup ------------------------------------------
		bool bArmAborted = false;
		bool bAirLaunched = false;
		bool bSawDashing = false;
		double DashEndReal = 0.0;
		double PressReal = 0.0;
		bool bPressReached = false;
		bool bGroundedAtPress = false;
		bool bAirborneAtPress = false;
		bool bSawLift = false;
		bool bSawCloak = false;
		double CloakLatency = -1.0;
		double CloakSeenReal = 0.0;
		bool bHeldSampled = false;
		bool bCloakHeld = false;
		int32 JarsAtPress = -1;
		int32 ConsumedBefore = 0;
		bool bJetBootsReadyBefore = false;
		bool bJetBootsTookPress = false;
		float CloakEndFirst = 0.f;
		bool bSecondPressDone = false;
		double SecondPressAfterDash = -1.0;
		double SecondPressReal = 0.0;
		double SecondPressMatchGap = -1.0;
		int32 SecondConsumedBefore = 0;
		bool bSecondJumpConsumed = false;
		bool bSecondSampled = false;
		float CloakEndAfterSecond = 0.f;

		int32 Checks = 0;
		int32 Failures = 0;
		TArray<FString> FailedClaims;
	};

	const TCHAR* Tag(const FRun& Run)
	{
		return Run.bClient ? TEXT("[DashCloakClient]") : TEXT("[DashCloak]");
	}

	void Check(FRun& Run, bool bOk, const FString& Claim)
	{
		++Run.Checks;
		if (bOk)
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s   ok    %s"), Tag(Run), *Claim);
			return;
		}
		++Run.Failures;
		Run.FailedClaims.Add(Claim);
		UE_LOG(LogTraceGame, Error, TEXT("%s   FAIL  %s"), Tag(Run), *Claim);
	}

	ATracePlayerController* FindLocalPC(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			ATracePlayerController* Candidate = Cast<ATracePlayerController>(It->Get());
			if (Candidate != nullptr && Candidate->IsLocalController())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	UWorld* FindGameWorld(bool bWantClient)
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate == nullptr || !Candidate->IsGameWorld())
			{
				continue;
			}
			const bool bIsClient = (Candidate->GetNetMode() == NM_Client);
			const bool bIsLocalAuthority = (Candidate->GetNetMode() == NM_Standalone
				|| Candidate->GetNetMode() == NM_ListenServer);
			if ((bWantClient && bIsClient) || (!bWantClient && bIsLocalAuthority))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	UTraceAbilityComponent* AbilitiesOf(const FRun& Run)
	{
		const ATracePlayerController* Controller = Run.PC.Get();
		return (Controller != nullptr) ? UTraceAbilityComponent::Get(Controller->PlayerState) : nullptr;
	}

	ATraceCharacter* PawnOf(const FRun& Run)
	{
		const ATracePlayerController* Controller = Run.PC.Get();
		return (Controller != nullptr) ? Cast<ATraceCharacter>(Controller->GetPawn()) : nullptr;
	}

	UTraceAbilitySetOyster* CloakKitOf(const UTraceAbilityComponent* Abilities)
	{
		return (Abilities != nullptr)
			? Cast<UTraceAbilitySetOyster>(Abilities->GetAbilitySetForSlot(ETraceLoadoutSlot::Passive))
			: nullptr;
	}

	const UTraceAbilitySetRocco* JetBootsKitOf(const UTraceAbilityComponent* Abilities)
	{
		return (Abilities != nullptr)
			? Cast<UTraceAbilitySetRocco>(Abilities->GetAbilitySetForSlot(ETraceLoadoutSlot::Movement))
			: nullptr;
	}

	void EnterStage(FRun& Run, EStage NewStage)
	{
		Run.Stage = NewStage;
		Run.StageStartReal = FPlatformTime::Seconds();
	}

	void ResetArm(FRun& Run)
	{
		Run.bArmAborted = false;
		Run.bAirLaunched = false;
		Run.bSawDashing = false;
		Run.DashEndReal = 0.0;
		Run.PressReal = 0.0;
		Run.bPressReached = false;
		Run.bGroundedAtPress = false;
		Run.bAirborneAtPress = false;
		Run.bSawLift = false;
		Run.bSawCloak = false;
		Run.CloakLatency = -1.0;
		Run.CloakSeenReal = 0.0;
		Run.bHeldSampled = false;
		Run.bCloakHeld = false;
		Run.JarsAtPress = -1;
		Run.ConsumedBefore = 0;
		Run.bJetBootsReadyBefore = false;
		Run.bJetBootsTookPress = false;
		Run.CloakEndFirst = 0.f;
		Run.bSecondPressDone = false;
		Run.SecondPressAfterDash = -1.0;
		Run.SecondPressReal = 0.0;
		Run.SecondPressMatchGap = -1.0;
		Run.SecondConsumedBefore = 0;
		Run.bSecondJumpConsumed = false;
		Run.bSecondSampled = false;
		Run.CloakEndAfterSecond = 0.f;
	}

	/** Seconds the jump has to show up as a cloak. The client waits out a net update of the PlayerState. */
	double ObserveSeconds(const FRun& Run)
	{
		return Run.bClient ? 1.6 : 0.75;
	}

	/**
	 * How long after the cloak first shows it must STILL be up. Half its duration: long enough that a
	 * cloak switched off again by something else in Oyster's kit a few frames later is caught, short
	 * enough to be nowhere near the real deadline.
	 */
	double HeldAfterSeconds()
	{
		return 0.5 * FMath::Max(0.f, UTraceSettings::Get().OysterDashCloakDurationSeconds);
	}

	/** Arms whose cloak must also be seen to LAST, and whose loadout carries no PICKLE JAR. Authority only. */
	bool IsHoldArm(const FRun& Run, EArm Arm)
	{
		return !Run.bClient && (Arm == EArm::Ground || Arm == EArm::JetBootsGround || Arm == EArm::RemoteShaped);
	}

	void AbortArm(FRun& Run, const FString& Why)
	{
		Check(Run, false, FString::Printf(TEXT("%s: (fixture) %s"), ArmLabel(Run.Arms[Run.ArmIndex]), *Why));
		Run.bArmAborted = true;
		EnterStage(Run, EStage::Judge);
	}

	/** Every claim for the arm just observed. Starred lines are the behaviour; the rest prove the fixture. */
	void JudgeArm(FRun& Run)
	{
		const EArm Arm = Run.Arms[Run.ArmIndex];
		const TCHAR* Label = ArmLabel(Arm);
		const double Window = FMath::Max(0.f, UTraceSettings::Get().OysterDashCloakWindowSeconds);

		if (Run.bArmAborted)
		{
			return;
		}

		const bool bDashed = (Arm != EArm::NoDash);
		if (bDashed)
		{
			Check(Run, Run.bSawDashing, FString::Printf(TEXT("%s: (fixture) a real dash ran (IsDashing observed)"), Label));
		}

		switch (Arm)
		{
		case EArm::NoDash:
			Check(Run, Run.bPressReached && Run.bGroundedAtPress,
				FString::Printf(TEXT("%s: (fixture) the jump key reached a grounded pawn"), Label));
			Check(Run, Run.bSawLift, FString::Printf(TEXT("%s: (fixture) the jump happened (the pawn rose)"), Label));
			Check(Run, !Run.bSawCloak,
				FString::Printf(TEXT("*** %s: a jump with no dash in front of it does NOT cloak ***"), Label));
			break;

		case EArm::Ground:
			Check(Run, Run.bPressReached && Run.bGroundedAtPress,
				FString::Printf(TEXT("%s: (fixture) the jump key reached a grounded pawn"), Label));
			Check(Run, Run.bSawLift, FString::Printf(TEXT("%s: (fixture) the jump happened (the pawn rose)"), Label));
			Check(Run, Run.bSawCloak,
				FString::Printf(TEXT("*** %s: a plain ground jump straight after a dash cloaks ***"), Label));
			break;

		case EArm::JetBootsGround:
			Check(Run, Run.bPressReached && Run.bGroundedAtPress,
				FString::Printf(TEXT("%s: (fixture) the jump key reached a grounded pawn"), Label));
			Check(Run, Run.bSawCloak,
				FString::Printf(TEXT("*** %s: with JET BOOTS equipped, a ground jump straight after a dash cloaks%s ***"),
					Label, Run.bClient ? TEXT(" (a REMOTE client's jump, judged on the client)") : TEXT("")));
			if (!Run.bClient)
			{
				Check(Run, Run.bSecondPressDone && Run.SecondPressAfterDash >= 0.0 && Run.SecondPressAfterDash < Window,
					FString::Printf(TEXT("%s: (fixture) the second press came %.3f s after the dash, inside the %.2f s window"),
						Label, Run.SecondPressAfterDash, Window));
				Check(Run, Run.bSecondJumpConsumed,
					FString::Printf(TEXT("%s: (fixture) JET BOOTS' second jump really fired on that press"), Label));
				Check(Run, Run.SecondPressMatchGap >= 0.05,
					FString::Printf(TEXT("%s: (fixture) the second jump came %.3f s of MATCH time after the cloak started, "
					                     "so a re-arm would have moved the deadline by that much"),
						Label, Run.SecondPressMatchGap));
				Check(Run, Run.bSecondSampled && Run.CloakEndFirst > 0.f
					&& FMath::IsNearlyEqual(Run.CloakEndFirst, Run.CloakEndAfterSecond, 0.001f),
					FString::Printf(TEXT("*** %s: a second jump inside the window does NOT re-arm the cloak "
					                     "(deadline %.3f -> %.3f) ***"),
						Label, Run.CloakEndFirst, Run.CloakEndAfterSecond));
			}
			break;

		case EArm::JetBootsAir:
			Check(Run, Run.bAirborneAtPress,
				FString::Printf(TEXT("%s: (fixture) the dash ended in the air and the pawn was still airborne at the press"), Label));
			Check(Run, Run.bPressReached && Run.bJetBootsTookPress,
				FString::Printf(TEXT("%s: (fixture) JET BOOTS consumed the press (its second jump fired)"), Label));
			Check(Run, Run.bSawCloak,
				FString::Printf(TEXT("*** %s: JET BOOTS' second jump straight after a dash cloaks — the passive hears a "
				                     "jump a movement kit spent%s ***  [bug a]"),
					Label, Run.bClient ? TEXT(", from a REMOTE client") : TEXT("")));
			break;

		case EArm::RemoteShaped:
			Check(Run, Run.bGroundedAtPress,
				FString::Printf(TEXT("%s: (fixture) the pawn was grounded for the bare ACharacter::Jump"), Label));
			Check(Run, Run.bSawLift, FString::Printf(TEXT("%s: (fixture) the jump happened (the pawn rose)"), Label));
			Check(Run, Run.bSawCloak,
				FString::Printf(TEXT("*** %s: a jump the server sees ONLY as movement (no HandleJumpPressed, no RPC — "
				                     "a remote client's ordinary jump) cloaks ***  [bug b]"), Label));
			break;

		case EArm::Late:
			Check(Run, Run.bPressReached && Run.bGroundedAtPress,
				FString::Printf(TEXT("%s: (fixture) the jump key reached a grounded pawn"), Label));
			Check(Run, Run.bSawLift, FString::Printf(TEXT("%s: (fixture) the jump happened (the pawn rose)"), Label));
			Check(Run, !Run.bSawCloak,
				FString::Printf(TEXT("*** %s: a jump %.2f s after the dash, past the %.2f s window, does NOT cloak ***"),
					Label, Run.PressReal - Run.DashEndReal, Window));
			break;

		default:
			break;
		}

		if (IsHoldArm(Run, Arm))
		{
			// Two faults that hid the cloak even when it fired: a RILLA CANS jar dropped by a dash the player
			// never picked the jar trail for, and the jar-count publish that cleared the cloak's bit.
			Check(Run, Run.JarsAtPress == 0,
				FString::Printf(TEXT("*** %s: VISISPURS without RILLA CANS — the dash left no poison jar (%d live) ***"),
					Label, Run.JarsAtPress));
			Check(Run, Run.bSawCloak && Run.bHeldSampled && Run.bCloakHeld,
				FString::Printf(TEXT("*** %s: the cloak is STILL up %.2f s after it appeared ***"), Label, HeldAfterSeconds()));
		}

		// ON A CLIENT, PROMPTLY. The state rides the PlayerState, which replicates once a second unless
		// pushed; a one-second cloak that shows up most of a second late is a cloak the other players
		// mostly never see. Measured on loopback, so the bound is the server's net tick, not a ping.
		const bool bExpectsCloak = (Arm != EArm::NoDash && Arm != EArm::Late);
		if (Run.bClient && bExpectsCloak && Run.bSawCloak)
		{
			Check(Run, Run.CloakLatency >= 0.0 && Run.CloakLatency <= 0.25,
				FString::Printf(TEXT("*** %s: the cloak reached this client %.3f s after the press (within 0.25 s; "
				                     "the PlayerState's own once-a-second update is not fast enough for a 1 s cloak) ***"),
					Label, Run.CloakLatency));
		}

		if (Run.bSawCloak)
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s   %s: the cloak was visible %.3f s after the press."),
				Tag(Run), Label, Run.CloakLatency);
		}
	}

	void Finish(FRun& Run)
	{
		UTraceAbilityComponent* Abilities = AbilitiesOf(Run);
		if (!Run.bClient && Abilities != nullptr)
		{
			Abilities->ApplyLoadout(Run.OriginalLoadout);
		}

		UE_LOG(LogTraceGame, Display, TEXT("%s ===== %d checks, %d failed. ====="), Tag(Run), Run.Checks, Run.Failures);
		for (const FString& Claim : Run.FailedClaims)
		{
			UE_LOG(LogTraceGame, Error, TEXT("%s   failed: %s"), Tag(Run), *Claim);
		}
		const bool bPass = (Run.Failures == 0 && Run.Checks > 0);
		UE_LOG(LogTraceGame, Display, TEXT("%s VERDICT: %s"), Tag(Run),
			bPass ? TEXT("PASS") : TEXT("*** FAIL ***"));

		if (!Run.bClient)
		{
			TraceVerifyLock::Release(VerifyCommandName);
		}
	}

	/** One tick of the run. Returns false when the run is over. */
	bool Step(FRun& Run)
	{
		ATracePlayerController* Controller = Run.PC.Get();
		ATraceCharacter* Pawn = PawnOf(Run);
		UTraceAbilityComponent* Abilities = AbilitiesOf(Run);
		UTraceCharacterMovementComponent* Move = (Pawn != nullptr) ? Pawn->GetTraceMovement() : nullptr;
		UTraceAbilitySetOyster* CloakKit = CloakKitOf(Abilities);

		if (Controller == nullptr || Abilities == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("%s VERDICT: *** FAIL *** — the local player went away mid-run."), Tag(Run));
			if (!Run.bClient)
			{
				TraceVerifyLock::Release(VerifyCommandName);
			}
			return false;
		}

		const double NowReal = FPlatformTime::Seconds();
		const double InStage = NowReal - Run.StageStartReal;
		const EArm Arm = Run.Arms[Run.ArmIndex];
		const double Window = FMath::Max(0.f, UTraceSettings::Get().OysterDashCloakWindowSeconds);

		switch (Run.Stage)
		{
		case EStage::Setup:
		{
			ResetArm(Run);
			if (Run.bClient)
			{
				if (!(Abilities->GetLoadout() == RemoteLoadout()))
				{
					AbortArm(Run, FString::Printf(TEXT("this client is not on %s (it has %s) — run "
						"Trace.Oyster.DashCloakStageRemote on the server"),
						*TraceLoadoutToString(RemoteLoadout()), *TraceLoadoutToString(Abilities->GetLoadout())));
					return true;
				}
			}
			else
			{
				Abilities->ApplyLoadout(LoadoutFor(Arm));
			}
			UE_LOG(LogTraceGame, Display, TEXT("%s --- %s on %s"), Tag(Run), ArmLabel(Arm),
				*TraceLoadoutToString(Abilities->GetLoadout()));
			EnterStage(Run, EStage::WaitReady);
			return true;
		}

		case EStage::WaitReady:
		{
			// A settled pawn: alive, standing, not mid-dash, a dash charge to spend, and no cloak still up
			// from the arm before — so a cloak seen in THIS arm can only have come from THIS arm's jump.
			const bool bReady = Pawn != nullptr && Pawn->IsAlive() && Move != nullptr && CloakKit != nullptr
				&& Move->IsMovingOnGround() && !Move->IsDashing() && Move->CanDash()
				&& !CloakKit->IsDashCloaked()
				&& !Controller->IsGameInputSuppressed()
				&& InStage >= 0.5;
			if (!bReady)
			{
				if (InStage > 12.0)
				{
					AbortArm(Run, FString::Printf(TEXT("the pawn never settled (alive=%d grounded=%d dashing=%d "
						"canDash=%d cloaked=%d kit=%d inputSuppressed=%d)"),
						(Pawn != nullptr && Pawn->IsAlive()) ? 1 : 0,
						(Move != nullptr && Move->IsMovingOnGround()) ? 1 : 0,
						(Move != nullptr && Move->IsDashing()) ? 1 : 0,
						(Move != nullptr && Move->CanDash()) ? 1 : 0,
						(CloakKit != nullptr && CloakKit->IsDashCloaked()) ? 1 : 0,
						(CloakKit != nullptr) ? 1 : 0,
						Controller->IsGameInputSuppressed() ? 1 : 0));
				}
				return true;
			}

			// Level aim, so the dash is flat and cannot drive the pawn into the floor or up a wall.
			Controller->SetControlRotation(FRotator(0.f, Controller->GetControlRotation().Yaw, 0.f));

			if (Arm == EArm::NoDash)
			{
				EnterStage(Run, EStage::Press);
			}
			else if (Arm == EArm::JetBootsAir)
			{
				EnterStage(Run, EStage::AirPrep);
			}
			else
			{
				Pawn->DoDash();   // the shipping input entry point
				EnterStage(Run, EStage::Dashing);
			}
			return true;
		}

		case EStage::AirPrep:
		{
			// Get off the ground WITHOUT the dash cloak's window being open, then dash in the air so the
			// dash ends there. The authority uses a launch (not a jump, so it reports nothing); a client
			// cannot launch itself without a correction, so it jumps — more than 3.5 s after its last
			// dash, which is far outside the window.
			if (Pawn == nullptr || Move == nullptr)
			{
				AbortArm(Run, TEXT("no pawn for the air arm"));
				return true;
			}
			if (!Run.bAirLaunched)
			{
				if (Run.bClient)
				{
					Controller->DebugPressJump();
				}
				else
				{
					Pawn->LaunchCharacter(FVector(0.f, 0.f, 1100.f), /*bXYOverride*/ false, /*bZOverride*/ true);
				}
				Run.bAirLaunched = true;
				return true;
			}
			// Near the top of the rise, so the fall after the dash is as long as it can be.
			const float RiseLimit = Run.bClient ? 200.f : 650.f;
			if (Move->IsFalling() && Move->Velocity.Z < RiseLimit)
			{
				Pawn->DoDash();
				EnterStage(Run, EStage::Dashing);
				return true;
			}
			if (InStage > 2.0)
			{
				AbortArm(Run, FString::Printf(TEXT("the pawn never got airborne for the air dash (falling=%d vz=%.0f)"),
					Move->IsFalling() ? 1 : 0, Move->Velocity.Z));
			}
			return true;
		}

		case EStage::Dashing:
		{
			const bool bDashingNow = (Move != nullptr) && Move->IsDashing();
			Run.bSawDashing = Run.bSawDashing || bDashingNow;
			if (Run.bSawDashing && !bDashingNow)
			{
				Run.DashEndReal = NowReal;
				EnterStage(Run, EStage::BeforePress);
				return true;
			}
			if (InStage > 1.5)
			{
				AbortArm(Run, FString::Printf(TEXT("the dash %s"), Run.bSawDashing ? TEXT("never ended") : TEXT("never started")));
			}
			return true;
		}

		case EStage::BeforePress:
		{
			// LATE waits the window out, plus a margin wider than a slow headless frame.
			//
			// A CLIENT waits a human beat (0.08 s, well inside the window) before any press. JET BOOTS'
			// press reaches the server as a reliable RPC on the PlayerState's channel while the dash's
			// end rides the pawn's unreliable move stream, and a press fired on the very next frame can
			// overtake the move that ended the dash — so the server would see a jump DURING the dash.
			// That race is one frame wide and no player can hit it; the harness must not either.
			const double PressDelay = (Arm == EArm::Late) ? Window + 0.25 : (Run.bClient ? 0.08 : 0.0);
			if ((NowReal - Run.DashEndReal) < PressDelay)
			{
				return true;
			}
			EnterStage(Run, EStage::Press);
			return true;
		}

		case EStage::Press:
		{
			if (Pawn == nullptr || Move == nullptr)
			{
				AbortArm(Run, TEXT("no pawn at the press"));
				return true;
			}
			Run.bGroundedAtPress = Move->IsMovingOnGround();
			Run.bAirborneAtPress = Move->IsFalling();
			Run.JarsAtPress = (!Run.bClient && CloakKit != nullptr) ? CloakKit->GetLiveJarCount() : -1;
			Run.ConsumedBefore = TraceAbilityIntegration::Counters().JumpConsumed;
			const UTraceAbilitySetRocco* JetBoots = JetBootsKitOf(Abilities);
			Run.bJetBootsReadyBefore = (JetBoots != nullptr) && JetBoots->IsSecondJumpAvailable();

			if (Arm == EArm::RemoteShaped)
			{
				// THE SERVER'S WHOLE VIEW OF A REMOTE CLIENT'S ORDINARY JUMP: the saved move's jump flag
				// sets bPressedJump and MoveAutonomous runs CheckJumpInput. ACharacter::Jump sets exactly
				// that bit and the next move runs exactly that function. Nothing else is called — not the
				// controller, not HandleJumpPressed, not ServerHandleJumpPressed.
				Pawn->Jump();
				Run.bPressReached = true;
			}
			else
			{
				Run.bPressReached = Controller->DebugPressJump();
			}
			Run.PressReal = NowReal;

			if (Arm == EArm::JetBootsAir)
			{
				const UTraceAbilitySetRocco* JetBootsAfter = JetBootsKitOf(Abilities);
				Run.bJetBootsTookPress = Run.bJetBootsReadyBefore
					&& TraceAbilityIntegration::Counters().JumpConsumed > Run.ConsumedBefore
					&& JetBootsAfter != nullptr && !JetBootsAfter->IsSecondJumpAvailable();
			}
			EnterStage(Run, EStage::Observe);
			return true;
		}

		case EStage::Observe:
		{
			if (Move != nullptr && Move->IsFalling() && Move->Velocity.Z > 50.f)
			{
				Run.bSawLift = true;
			}
			if (CloakKit != nullptr && CloakKit->IsDashCloaked() && !Run.bSawCloak)
			{
				Run.bSawCloak = true;
				Run.CloakSeenReal = NowReal;
				Run.CloakLatency = NowReal - Run.PressReal;
				Run.CloakEndFirst = CloakKit->GetDashCloakEndMatchTime();
			}
			if (Run.bSawCloak && !Run.bHeldSampled && (NowReal - Run.CloakSeenReal) >= HeldAfterSeconds())
			{
				Run.bCloakHeld = (CloakKit != nullptr) && CloakKit->IsDashCloaked();
				Run.bHeldSampled = true;
			}

			// JET BOOTS GROUND, authority: once the cloak is up and the pawn is in the air, spend JET
			// BOOTS' second jump while the window is still open. That jump DOES reach the cloak now (a
			// consumed press is reported), so only the one-dash-one-cloak latch stops a re-arm.
			//
			// A TENTH OF A SECOND AFTER THE CLOAK APPEARED, NOT ON THE NEXT HARNESS TICK. The first
			// version pressed at once, which is the same WORLD time the cloak started at (no world tick
			// had run in between), so a re-arm would have written the identical deadline — and the check
			// stayed green with the latch deleted. The gap is measured below and asserted as a fixture
			// line, so that failure mode cannot come back quietly.
			if (Arm == EArm::JetBootsGround && !Run.bClient && Run.bSawCloak && !Run.bSecondPressDone
				&& Move != nullptr && Move->IsFalling()
				&& ((NowReal - Run.CloakSeenReal) >= 0.1 || (NowReal - Run.DashEndReal) >= Window - 0.1))
			{
				const UWorld* PressWorld = Pawn->GetWorld();
				const AGameStateBase* PressGameState = (PressWorld != nullptr) ? PressWorld->GetGameState() : nullptr;
				const double CloakStartMatch = static_cast<double>(Run.CloakEndFirst)
					- FMath::Max(0.f, UTraceSettings::Get().OysterDashCloakDurationSeconds);
				Run.SecondPressMatchGap = (PressGameState != nullptr)
					? PressGameState->GetServerWorldTimeSeconds() - CloakStartMatch : -1.0;
				Run.SecondPressAfterDash = NowReal - Run.DashEndReal;
				Run.SecondConsumedBefore = TraceAbilityIntegration::Counters().JumpConsumed;
				Controller->DebugPressJump();
				const UTraceAbilitySetRocco* JetBoots = JetBootsKitOf(Abilities);
				Run.bSecondJumpConsumed = TraceAbilityIntegration::Counters().JumpConsumed > Run.SecondConsumedBefore
					&& JetBoots != nullptr && !JetBoots->IsSecondJumpAvailable();
				Run.bSecondPressDone = true;
				Run.SecondPressReal = NowReal;
			}
			if (Run.bSecondPressDone && !Run.bSecondSampled && (NowReal - Run.SecondPressReal) >= 0.2)
			{
				Run.CloakEndAfterSecond = (CloakKit != nullptr) ? CloakKit->GetDashCloakEndMatchTime() : -1.f;
				Run.bSecondSampled = true;
			}

			const bool bHeldPending = IsHoldArm(Run, Arm) && Run.bSawCloak && !Run.bHeldSampled;
			if (InStage >= ObserveSeconds(Run) && (!Run.bSecondPressDone || Run.bSecondSampled) && !bHeldPending)
			{
				EnterStage(Run, EStage::Judge);
			}
			return true;
		}

		case EStage::Judge:
		{
			JudgeArm(Run);
			if (++Run.ArmIndex >= Run.Arms.Num())
			{
				Finish(Run);
				return false;
			}
			EnterStage(Run, EStage::Setup);
			return true;
		}

		default:
			return false;
		}
	}

	void StartRun(const TSharedPtr<FRun>& Run)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float) -> bool
		{
			return Step(*Run);
		}));
	}

	// =============================================================================================
	// Trace.Oyster.DashCloakVerify — the authoritative arms
	// =============================================================================================

	void RunVerify()
	{
		if (!TraceVerifyLock::ClaimOrQueue(VerifyCommandName, 90.0))
		{
			return;
		}

		UWorld* WorldPtr = FindGameWorld(/*bWantClient=*/false);
		ATracePlayerController* Controller = FindLocalPC(WorldPtr);
		ATracePlayerState* LocalState = (Controller != nullptr) ? Controller->GetPlayerState<ATracePlayerState>() : nullptr;
		UTraceAbilityComponent* Abilities = (LocalState != nullptr) ? UTraceAbilityComponent::Get(LocalState) : nullptr;
		const ATraceCharacter* Pawn = (Controller != nullptr) ? Cast<ATraceCharacter>(Controller->GetPawn()) : nullptr;
		if (Controller == nullptr || LocalState == nullptr || Abilities == nullptr || Pawn == nullptr || !Pawn->IsAlive())
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[DashCloak] VERDICT: *** FAIL *** — needs a standalone or listen-server match with a living "
				     "local pawn (run on /Game/Maps/Arena after Trace.Teams.Close)."));
			TraceVerifyLock::Release(VerifyCommandName);
			return;
		}

		if (WorldPtr->IsPaused())
		{
			Controller->SetPause(false);
			UE_LOG(LogTraceGame, Warning, TEXT("[DashCloak] The world was PAUSED; unpaused so the pawn can move."));
		}

		// Out of the select screen the way LOCK IN leaves it, so the page does not suppress the jump key
		// and the 4 Hz poll does not reopen it. Character id None: a loadout-only player, as shipped.
		if (Abilities->GetCharacterId() != ETraceCharacterId::None)
		{
			Abilities->ServerSetCharacter(ETraceCharacterId::None);
		}
		LocalState->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/true);
		LocalState->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[DashCloak] ===== Oyster's dash cloak: window %.2f s after the dash, cloak %.2f s. pawn=%s ====="),
			Settings.OysterDashCloakWindowSeconds, Settings.OysterDashCloakDurationSeconds, *GetNameSafe(Pawn));

		TSharedPtr<FRun> Run = MakeShared<FRun>();
		Run->bClient = false;
		Run->PC = Controller;
		Run->OriginalLoadout = Abilities->GetLoadout();
		Run->Arms = { EArm::NoDash, EArm::Ground, EArm::JetBootsGround, EArm::JetBootsAir, EArm::RemoteShaped, EArm::Late };
		EnterStage(*Run, EStage::Setup);
		StartRun(Run);
	}

	FAutoConsoleCommand CmdVerify(
		VerifyCommandName,
		TEXT("Dev only, standalone or listen host. Oyster's VISISPURS (the dash cloak) hears every jump that follows a dash: a plain ")
		TEXT("ground jump, a ground jump with JET BOOTS equipped, JET BOOTS' own second jump out of an air dash, and a ")
		TEXT("jump the server only sees as movement (a remote client's). Plus: no re-arm inside the window, no cloak ")
		TEXT("without a dash, no cloak after the window."),
		FConsoleCommandDelegate::CreateStatic(&RunVerify));

	// =============================================================================================
	// Trace.Oyster.DashCloakStageRemote — the server's half of the two-process run
	// =============================================================================================

	void StageRemote()
	{
		// Polls, because the client may still be connecting when the server's -TraceExec fires. Stages
		// each remote player once; a player who is already on the loadout is left alone.
		const double StartReal = FPlatformTime::Seconds();
		TSharedPtr<TSet<FString>> Staged = MakeShared<TSet<FString>>();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([StartReal, Staged](float) -> bool
		{
			UWorld* WorldPtr = FindGameWorld(/*bWantClient=*/false);
			if (WorldPtr == nullptr || WorldPtr->GetNetMode() != NM_ListenServer)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[DashCloakStage] needs a listen server (\"/Game/Maps/Arena?listen\")."));
				return false;
			}
			for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
			{
				ATracePlayerController* Remote = Cast<ATracePlayerController>(It->Get());
				ATracePlayerState* RemoteState = (Remote != nullptr) ? Remote->GetPlayerState<ATracePlayerState>() : nullptr;
				UTraceAbilityComponent* RemoteAbilities = (RemoteState != nullptr) ? UTraceAbilityComponent::Get(RemoteState) : nullptr;
				if (Remote == nullptr || Remote->IsLocalController() || RemoteAbilities == nullptr
					|| Staged->Contains(RemoteState->GetPlayerName()))
				{
					continue;
				}
				if (RemoteAbilities->GetCharacterId() != ETraceCharacterId::None)
				{
					RemoteAbilities->ServerSetCharacter(ETraceCharacterId::None);
				}
				RemoteState->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/true);
				RemoteState->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
				RemoteAbilities->ApplyLoadout(RemoteLoadout());
				Staged->Add(RemoteState->GetPlayerName());
				UE_LOG(LogTraceGame, Display, TEXT("[DashCloakStage] staged remote player '%s' on %s."),
					*RemoteState->GetPlayerName(), *TraceLoadoutToString(RemoteAbilities->GetLoadout()));
			}
			if (FPlatformTime::Seconds() - StartReal > 120.0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[DashCloakStage] done watching; staged %d remote player(s)."), Staged->Num());
				return false;
			}
			return true;
		}));
	}

	FAutoConsoleCommand CmdStageRemote(
		TEXT("Trace.Oyster.DashCloakStageRemote"),
		TEXT("Dev only, listen server. For two minutes, gives every REMOTE player JET BOOTS / VISISPURS / RIPPLE and ")
		TEXT("closes their select screen, for Trace.Oyster.DashCloakClientProbe to run on the client."),
		FConsoleCommandDelegate::CreateStatic(&StageRemote));

	// =============================================================================================
	// Trace.Oyster.DashCloakClientProbe — the same claims from a REAL remote client
	// =============================================================================================

	void RunClientProbe()
	{
		UWorld* WorldPtr = FindGameWorld(/*bWantClient=*/true);
		ATracePlayerController* Controller = FindLocalPC(WorldPtr);
		if (Controller == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[DashCloakClient] VERDICT: *** FAIL *** — run this on a CLIENT connected to a listen server."));
			return;
		}

		// Waits for the server's staging to arrive (the loadout replicates, the select page closes) and
		// for a living pawn, then hands over to the same arms the authority runs.
		const double StartReal = FPlatformTime::Seconds();
		TWeakObjectPtr<ATracePlayerController> WeakPC = Controller;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([StartReal, WeakPC](float) -> bool
		{
			ATracePlayerController* Local = WeakPC.Get();
			const UTraceAbilityComponent* LocalAbilities = (Local != nullptr) ? UTraceAbilityComponent::Get(Local->PlayerState) : nullptr;
			const ATraceCharacter* LocalPawn = (Local != nullptr) ? Cast<ATraceCharacter>(Local->GetPawn()) : nullptr;
			const bool bStaged = LocalAbilities != nullptr && LocalAbilities->GetLoadout() == RemoteLoadout()
				&& LocalPawn != nullptr && LocalPawn->IsAlive() && !Local->IsGameInputSuppressed()
				&& CloakKitOf(LocalAbilities) != nullptr;
			if (!bStaged)
			{
				if (FPlatformTime::Seconds() - StartReal > 90.0)
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[DashCloakClient] VERDICT: *** FAIL *** — never staged (loadout %s, pawn alive %d, input "
						     "suppressed %d). Run Trace.Oyster.DashCloakStageRemote on the server."),
						(LocalAbilities != nullptr) ? *TraceLoadoutToString(LocalAbilities->GetLoadout()) : TEXT("-"),
						(LocalPawn != nullptr && LocalPawn->IsAlive()) ? 1 : 0,
						(Local != nullptr && Local->IsGameInputSuppressed()) ? 1 : 0);
					return false;
				}
				return true;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[DashCloakClient] ===== a REMOTE client's jumps after a dash, judged on the client. pawn=%s role=%d ====="),
				*GetNameSafe(LocalPawn), static_cast<int32>(LocalPawn->GetLocalRole()));

			TSharedPtr<FRun> Run = MakeShared<FRun>();
			Run->bClient = true;
			Run->PC = Local;
			Run->OriginalLoadout = LocalAbilities->GetLoadout();
			Run->Arms = { EArm::JetBootsGround, EArm::JetBootsAir, EArm::Late };
			EnterStage(*Run, EStage::Setup);
			StartRun(Run);
			return false;
		}));
	}

	FAutoConsoleCommand CmdClientProbe(
		TEXT("Trace.Oyster.DashCloakClientProbe"),
		TEXT("Dev only, on a CLIENT. Waits for Trace.Oyster.DashCloakStageRemote's loadout, then dashes and jumps ")
		TEXT("through this client's own jump key and reads the cloak off the replicated state: a ground jump with JET ")
		TEXT("BOOTS equipped, JET BOOTS' second jump out of an air dash, and a late jump that must not cloak."),
		FConsoleCommandDelegate::CreateStatic(&RunClientProbe));
}

#endif // !UE_BUILD_SHIPPING
