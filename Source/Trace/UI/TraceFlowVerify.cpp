// Trace — Trace.Flow.Verify: the match flow from the first menu to the whistle, held to account.
//
// One frame-driven run through a real match, each claim one the code before P09 fails:
//
//   A. THE PRE-MATCH HOLD. While the human is still in team select or on the loadout page the match
//      clock does not start: the phase stays WaitingForPlayers and the warm-up countdown reads the
//      latest moment their own screens could still be open, plus the warm-up. It used to be a flat 5 s
//      warm-up, so the first half ran for 30-45 s under the menus while bots played. Locking in drops
//      the countdown to the warm-up; the whistle then comes on time, every bot is equipped at kickoff
//      (bots wait for their team's humans), warm-up kills and deaths are wiped with the scores, and
//      every bot's name is upper case (it was "BOT Blue 3").
//   C. THE REFUSAL TOASTS go through the text document: an E pressed while cooling reads
//      HUD.TOAST_COOLDOWN, and SNAP's second gate pressed on the spot reads HUD.TOAST_TOO_CLOSE ("TOO
//      CLOSE"), where it used to say "SNAP: move 340 uu further from the first gate".
//   B. A RESPAWN UNDER AN OPEN MENU keeps the menu's mouse. The team screen is opened (the H path), the
//      pawn behind it is killed, and after the respawn the cursor must still be shown and the viewport
//      must not have captured the mouse. OnPossess used to put the game-only mode back and freeze the
//      pointer for the rest of the page. Closing the menu must still take the mouse back.
//   E. HALF TIME SHUTS THE TEAM SCREEN. With the team screen open (H), the half-time whistle closes it
//      as it opens the loadout window, so the loadout page is the only page drawn and it takes the
//      input. The team screen used to stay up under the loadout page: each gated the other's input,
//      so no key, pad or click worked until the team screen's own 15 s timeout ran out. The second
//      half's whistle then shuts the window and hands the input back.
//   D. FULL TIME SHUTS THE MENUS. With the team screen open, the whistle closes it (and any loadout
//      window), the server refuses a team change after the whistle, and the select poll opens nothing
//      over the results. The team page used to stay up over the results and still switch teams.
//
// Headless recipe — run it EARLY, while the human is still in team select (it drives the rest itself):
//   Arena -bots=4 -TraceExecAt=6 -TraceExec="Trace.Flow.Verify"

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Trace.h"                        // LogTraceGame
#include "TraceSettings.h"
#include "UI/Text/TraceGameText.h"
#include "UI/TraceHUD.h"

// Named after the file, not anonymous: Scripts/check-jumbo-build-collisions.py.
namespace TraceFlowVerify
{
	enum class EStep : uint8
	{
		Start,
		WaitForWhistle,
		ToastCooldown,
		ToastTooClose,
		OpenMenuForRespawn,
		WaitForRespawn,
		CloseMenuAfterRespawn,
		OpenMenuForHalfTime,
		HalfTimeDrawn,
		SecondHalf,
		OpenMenuForWhistle,
		AfterWhistle,
		Done,
	};

	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		EStep Step = EStep::Start;
		double StepStart = 0.0;
		double Deadline = 0.0;
		int32 Passes = 0;
		int32 Failures = 0;
		int32 Inconclusive = 0;

		/** Server time the harness locked in, and the whistle it was then promised. */
		double LockedAtServer = -1.0;
		double PromisedWhistle = -1.0;

		/** Real time the harness first saw the match live; the bot fill gets a poll after it. */
		double LiveSeenAt = -1.0;

		/** The pawn that was killed under the menu, so "a respawn happened" means a new body. */
		TWeakObjectPtr<APawn> KilledPawn;
		bool bKilled = false;
		bool bBotNamesChecked = false;
		bool bSnapPrimed = false;
	};

	static void Report(FRun& Run, bool bPass, const FString& Claim, const FString& Detail)
	{
		(bPass ? Run.Passes : Run.Failures) += 1;
		UE_LOG(LogTraceGame, Display, TEXT("[FlowVerify]   %-4s %s  %s"), bPass ? TEXT("ok") : TEXT("FAIL"), *Claim, *Detail);
	}

	static void Skip(FRun& Run, const FString& Claim, const FString& Why)
	{
		Run.Inconclusive += 1;
		UE_LOG(LogTraceGame, Warning, TEXT("[FlowVerify]   ---- %s  INCONCLUSIVE: %s"), *Claim, *Why);
	}

	static void GoTo(FRun& Run, EStep Step)
	{
		Run.Step = Step;
		Run.StepStart = FPlatformTime::Seconds();
	}

	static void Finish(FRun& Run)
	{
		Run.Step = EStep::Done;
		if (Run.Failures == 0 && Run.Passes > 0)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[FlowVerify] VERDICT: ===== PASS (%d checks%s) ====="), Run.Passes,
				Run.Inconclusive > 0 ? TEXT(", some INCONCLUSIVE - see above") : TEXT(""));
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[FlowVerify] VERDICT: ===== *** FAIL *** %d of %d check(s) ====="),
				Run.Failures, Run.Failures + Run.Passes);
		}
	}

	/** True while this human is still in the pre-match menus, by the game mode's own reading of it. */
	static bool IsChoosing(const ATracePlayerController* HumanPC, const ATracePlayerState* HumanState)
	{
		if (HumanPC == nullptr || HumanState == nullptr)
		{
			return false;
		}
		return HumanPC->IsTeamSelectOpen()
			|| (UTraceAbilityComponent::AreCharactersEnabled(HumanPC) && !HumanState->HasCharacter() && !HumanState->IsCharacterLocked());
	}

	/** Whether the text document has been asked for @p Key this session (i.e. a call site ran). */
	static bool IsKeyRegistered(const TCHAR* Key)
	{
		TArray<TraceGameText::FEntry> Entries;
		TraceGameText::GetAllEntries(Entries);
		for (const TraceGameText::FEntry& Entry : Entries)
		{
			if (Entry.Key == Key)
			{
				return true;
			}
		}
		return false;
	}

	/** Words a HUD line may not carry: lower case (the kit is upper case) or engine units. */
	static bool IsHudWording(const FString& Words)
	{
		return !Words.IsEmpty() && Words.Equals(Words.ToUpper(), ESearchCase::CaseSensitive)
			&& !Words.Contains(TEXT(" UU"), ESearchCase::CaseSensitive);
	}

	static FString DescribeInput(const ATracePlayerController* HumanPC, const UWorld* WorldPtr)
	{
		const UGameViewportClient* const Viewport = (WorldPtr != nullptr) ? WorldPtr->GetGameViewport() : nullptr;
		return FString::Printf(TEXT("suppressed %d, cursor shown %d, capture mode %d, lock mode %d"),
			(HumanPC != nullptr && HumanPC->IsGameInputSuppressed()) ? 1 : 0,
			(HumanPC != nullptr && HumanPC->bShowMouseCursor) ? 1 : 0,
			(Viewport != nullptr) ? static_cast<int32>(Viewport->GetMouseCaptureMode()) : -1,
			(Viewport != nullptr) ? static_cast<int32>(Viewport->GetMouseLockMode()) : -1);
	}

	/** The menu's input mode: cursor shown, and the viewport captures only while a button is held. */
	static bool HasMenuMouse(const ATracePlayerController* HumanPC, const UWorld* WorldPtr)
	{
		const UGameViewportClient* const Viewport = (WorldPtr != nullptr) ? WorldPtr->GetGameViewport() : nullptr;
		if (HumanPC == nullptr || Viewport == nullptr)
		{
			return false;
		}
		const EMouseCaptureMode Capture = Viewport->GetMouseCaptureMode();
		return HumanPC->bShowMouseCursor
			&& Capture != EMouseCaptureMode::CapturePermanently
			&& Capture != EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown;
	}

	/** Gameplay's input mode: cursor hidden, the viewport captures on the first click. */
	static bool HasGameMouse(const ATracePlayerController* HumanPC, const UWorld* WorldPtr)
	{
		const UGameViewportClient* const Viewport = (WorldPtr != nullptr) ? WorldPtr->GetGameViewport() : nullptr;
		if (HumanPC == nullptr || Viewport == nullptr)
		{
			return false;
		}
		const EMouseCaptureMode Capture = Viewport->GetMouseCaptureMode();
		return !HumanPC->bShowMouseCursor
			&& (Capture == EMouseCaptureMode::CapturePermanently
				|| Capture == EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown);
	}

	/** One ticker pass. Returns false when the run is over. */
	static bool Tick(FRun& Run)
	{
		UWorld* const WorldPtr = Run.World.Get();
		ATracePlayerController* const HumanPC = (WorldPtr != nullptr) ? Cast<ATracePlayerController>(WorldPtr->GetFirstPlayerController()) : nullptr;
		ATracePlayerState* const HumanState = (HumanPC != nullptr) ? HumanPC->GetPlayerState<ATracePlayerState>() : nullptr;
		ATraceGameMode* const Rules = (WorldPtr != nullptr) ? WorldPtr->GetAuthGameMode<ATraceGameMode>() : nullptr;
		ATraceGameState* const MatchGS = (WorldPtr != nullptr) ? WorldPtr->GetGameState<ATraceGameState>() : nullptr;
		UTraceAbilityComponent* const Abilities = (HumanState != nullptr) ? UTraceAbilityComponent::Get(HumanState) : nullptr;
		ATraceHUD* const Hud = (HumanPC != nullptr) ? Cast<ATraceHUD>(HumanPC->GetHUD()) : nullptr;
		if (WorldPtr == nullptr || HumanPC == nullptr || HumanState == nullptr || Rules == nullptr || MatchGS == nullptr
			|| Abilities == nullptr || Hud == nullptr)
		{
			Report(Run, false, TEXT("a standalone or host match with a local human, a HUD and an ability component"), TEXT("missing"));
			Finish(Run);
			return false;
		}
		if (FPlatformTime::Seconds() > Run.Deadline)
		{
			Report(Run, false, TEXT("the run finished inside its time limit"),
				FString::Printf(TEXT("stuck at step %d"), static_cast<int32>(Run.Step)));
			Finish(Run);
			return false;
		}

		const double NowServer = MatchGS->GetServerWorldTimeSeconds();
		const double SinceStep = FPlatformTime::Seconds() - Run.StepStart;
		const double WarmupSeconds = FMath::Max(0.0, static_cast<double>(UTraceSettings::Get().WarmupDuration));
		const ETraceTeam OtherTeam = (HumanState->Team == ETraceTeam::Blue) ? ETraceTeam::Orange : ETraceTeam::Blue;

		switch (Run.Step)
		{
		// ---- A. THE PRE-MATCH HOLD ----------------------------------------------------------------
		case EStep::Start:
		{
			if (!IsChoosing(HumanPC, HumanState))
			{
				Skip(Run, TEXT("A: the pre-match hold"), TEXT("the human was not in team select or the loadout page; run this early"));
				GoTo(Run, EStep::WaitForWhistle);
				return true;
			}

			const double Countdown = static_cast<double>(MatchGS->MatchEndServerTime) - NowServer;
			const double TeamLeft = HumanPC->IsTeamSelectOpen() ? static_cast<double>(HumanPC->GetTeamSelectTimeRemaining()) : 0.0;
			Report(Run, MatchGS->TraceMatchState == ETraceMatchState::WaitingForPlayers && Rules->IsPreMatchHeldForMenus()
					&& MatchGS->MatchEndServerTime > 0.f && Countdown > TeamLeft + WarmupSeconds - 0.5,
				TEXT("*** A1 the match clock waits while the human is in team select ***"),
				FString::Printf(TEXT("phase %d, held %d, countdown %.1fs, team screen %.1fs left, warm-up %.0fs"),
					static_cast<int32>(MatchGS->TraceMatchState), Rules->IsPreMatchHeldForMenus() ? 1 : 0,
					Countdown, TeamLeft, WarmupSeconds));

			// Team select closes the way every close ends (pick, timeout, H): the loadout opens in the same call.
			if (HumanPC->IsTeamSelectOpen())
			{
				HumanPC->ServerSetTeamSelectOpen(/*bOpen=*/false, 0.f);
			}
			Rules->DebugPollCharacterSelect();
			if (HumanState->IsCharacterSelectOpen())
			{
				const double LoadoutLeft = static_cast<double>(HumanState->CharacterSelectDeadlineServerTime) - NowServer;
				const double CountdownNow = static_cast<double>(MatchGS->MatchEndServerTime) - NowServer;
				Report(Run, MatchGS->TraceMatchState == ETraceMatchState::WaitingForPlayers
						&& CountdownNow > LoadoutLeft + WarmupSeconds - 0.5,
					TEXT("*** A2 ...and while they are on the loadout page ***"),
					FString::Printf(TEXT("phase %d, countdown %.1fs, loadout auto-pick in %.1fs"),
						static_cast<int32>(MatchGS->TraceMatchState), CountdownNow, LoadoutLeft));
			}
			else
			{
				Skip(Run, TEXT("A2: the hold on the loadout page"), TEXT("no loadout window opened (characters off?)"));
			}

			// Warm-up deaths and kills, planted, so the whistle has something to wipe.
			HumanState->Kills = 2;
			HumanState->Deaths = 3;

			// LOCK IN through the page's own server path. SNAP on E, for claim C.
			FTraceLoadout Wanted;
			Wanted.Movement  = ETraceAbilityId::StickyGloves;
			Wanted.Passive   = ETraceAbilityId::Magnet;
			Wanted.Activated = ETraceAbilityId::Snap;
			Abilities->ServerRequestSetLoadout(Wanted);
			Rules->DebugPollCharacterSelect();

			const double CountdownLocked = static_cast<double>(MatchGS->MatchEndServerTime) - NowServer;
			Report(Run, !IsChoosing(HumanPC, HumanState) && !Rules->IsPreMatchHeldForMenus()
					&& FMath::Abs(CountdownLocked - WarmupSeconds) < 0.6,
				TEXT("*** A3 locking in drops the countdown to the warm-up ***"),
				FString::Printf(TEXT("choosing %d, held %d, countdown %.1fs (warm-up %.0fs), loadout %s"),
					IsChoosing(HumanPC, HumanState) ? 1 : 0, Rules->IsPreMatchHeldForMenus() ? 1 : 0,
					CountdownLocked, WarmupSeconds, *TraceLoadoutToString(Abilities->GetLoadout())));

			Run.LockedAtServer = NowServer;
			Run.PromisedWhistle = static_cast<double>(MatchGS->MatchEndServerTime);
			GoTo(Run, EStep::WaitForWhistle);
			return true;
		}

		case EStep::WaitForWhistle:
		{
			if (MatchGS->TraceMatchState != ETraceMatchState::InProgress)
			{
				if (SinceStep > WarmupSeconds + 60.0)
				{
					Report(Run, false, TEXT("the match went live"), TEXT("still not InProgress"));
					Finish(Run);
					return false;
				}
				return true;
			}
			if (Run.LiveSeenAt < 0.0 && Run.LockedAtServer >= 0.0)
			{
				Report(Run, NowServer - Run.PromisedWhistle < 1.5,
					TEXT("A4 the whistle came when the countdown said"),
					FString::Printf(TEXT("live %.1fs after lock-in, promised %.1fs"),
						NowServer - Run.LockedAtServer, Run.PromisedWhistle - Run.LockedAtServer));
				Report(Run, HumanState->Kills == 0 && HumanState->Deaths == 0,
					TEXT("*** A5 warm-up kills and deaths are wiped at the whistle ***"),
					FString::Printf(TEXT("kills %d, deaths %d (planted 2 / 3 before it)"), HumanState->Kills, HumanState->Deaths));
				Report(Run, !HumanState->IsCharacterSelectOpen() && HumanState->IsCharacterLocked(),
					TEXT("A6 the loadout page stayed shut through the whistle"),
					FString::Printf(TEXT("window %d, locked %d"), HumanState->IsCharacterSelectOpen() ? 1 : 0,
						HumanState->IsCharacterLocked() ? 1 : 0));
			}
			if (Run.LiveSeenAt < 0.0)
			{
				Run.LiveSeenAt = FPlatformTime::Seconds();
			}
			const double SinceLive = FPlatformTime::Seconds() - Run.LiveSeenAt;

			// Every bot's name in upper case, and (characters on) every bot equipped by the first poll.
			TArray<FString> BadNames;
			int32 Bots = 0;
			int32 BotsEquipped = 0;
			for (const APlayerState* const EachState : MatchGS->PlayerArray)
			{
				const ATracePlayerState* const Bot = Cast<ATracePlayerState>(EachState);
				if (Bot == nullptr || !Bot->IsABot())
				{
					continue;
				}
				++Bots;
				BotsEquipped += Bot->HasAnyAbility() ? 1 : 0;
				if (!IsHudWording(Bot->GetPlayerName()))
				{
					BadNames.Add(Bot->GetPlayerName());
				}
			}
			if (Bots == 0)
			{
				Skip(Run, TEXT("A7: bot names and kickoff loadouts"), TEXT("no bots in this match (-bots=4)"));
			}
			else
			{
				if (!Run.bBotNamesChecked)
				{
					Run.bBotNamesChecked = true;
					Report(Run, BadNames.Num() == 0, TEXT("*** A7 every bot's name is upper case ***"),
						BadNames.Num() == 0 ? FString::Printf(TEXT("%d bots"), Bots) : FString::Join(BadNames, TEXT(", ")));
				}
				if (Run.LockedAtServer >= 0.0 && UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
				{
					if (SinceLive < 0.6)
					{
						return true;   // the fill runs on the next select poll after the whistle
					}
					Report(Run, BotsEquipped == Bots, TEXT("A8 every bot is equipped at kickoff"),
						FString::Printf(TEXT("%d of %d bots hold abilities %.1fs after the whistle"), BotsEquipped, Bots, SinceLive));
				}
			}
			GoTo(Run, EStep::ToastCooldown);
			return true;
		}

		// ---- C. THE REFUSAL TOASTS ----------------------------------------------------------------
		case EStep::ToastCooldown:
		{
			if (Abilities->GetLoadout().Activated == ETraceAbilityId::None)
			{
				Skip(Run, TEXT("C: refusal toasts"), TEXT("no activated ability equipped"));
				GoTo(Run, EStep::OpenMenuForRespawn);
				return true;
			}
			Abilities->DebugSetActivatedCooldown(12.f);
			Abilities->TryActivate();
			const FString Words = Hud->GetLastAbilityToastText().ToString();
			Report(Run, IsKeyRegistered(TEXT("HUD.TOAST_COOLDOWN")) && IsHudWording(Words) && Words.EndsWith(TEXT("S")),
				TEXT("*** C1 the cooling toast is the text document's HUD.TOAST_COOLDOWN ***"),
				FString::Printf(TEXT("\"%s\""), *Words));

			// SNAP: first press opens the window; the second, on the spot, is refused as too close.
			Abilities->DebugSetActivatedCooldown(0.f);
			Run.bSnapPrimed = (Abilities->GetLoadout().Activated == ETraceAbilityId::Snap) && Abilities->TryActivate();
			GoTo(Run, EStep::ToastTooClose);
			return true;
		}

		case EStep::ToastTooClose:
		{
			if (SinceStep < 0.2)
			{
				return true;
			}
			if (!Run.bSnapPrimed)
			{
				Skip(Run, TEXT("C2: SNAP's too-close toast"), TEXT("SNAP's first gate did not open"));
			}
			else
			{
				Abilities->TryActivate();
				const FString Words = Hud->GetLastAbilityToastText().ToString();
				Report(Run, Words == TRACE_TEXT("HUD.TOAST_TOO_CLOSE", "TOO CLOSE") && IsHudWording(Words),
					TEXT("*** C2 SNAP on the spot says TOO CLOSE, not engine units ***"), FString::Printf(TEXT("\"%s\""), *Words));
			}
			GoTo(Run, EStep::OpenMenuForRespawn);
			return true;
		}

		// ---- B. A RESPAWN UNDER AN OPEN MENU ------------------------------------------------------
		case EStep::OpenMenuForRespawn:
		{
			if (SinceStep < 0.1)
			{
				Rules->OpenTeamSelectFor(HumanPC);   // the H key's server path
				return true;
			}
			if (!HumanPC->IsGameInputSuppressed())
			{
				if (SinceStep > 3.0)
				{
					Report(Run, false, TEXT("B: the team screen took the input"), DescribeInput(HumanPC, WorldPtr));
					GoTo(Run, EStep::OpenMenuForHalfTime);
				}
				return true;
			}
			ATraceCharacter* const Body = Cast<ATraceCharacter>(HumanPC->GetPawn());
			if (Body == nullptr || !Body->IsAlive() || Body->Health == nullptr)
			{
				Skip(Run, TEXT("B: respawn under the menu"), TEXT("no live local pawn to kill"));
				GoTo(Run, EStep::CloseMenuAfterRespawn);
				return true;
			}
			Report(Run, HasMenuMouse(HumanPC, WorldPtr), TEXT("B0 the open team screen has the mouse"), DescribeInput(HumanPC, WorldPtr));
			Run.KilledPawn = Body;
			Body->Health->Kill(nullptr, FName(TEXT("FlowVerify")));
			Run.bKilled = true;
			GoTo(Run, EStep::WaitForRespawn);
			return true;
		}

		case EStep::WaitForRespawn:
		{
			const ATraceCharacter* const Body = Cast<ATraceCharacter>(HumanPC->GetPawn());
			const bool bRespawned = Body != nullptr && Body != Run.KilledPawn.Get() && Body->IsAlive();
			if (!bRespawned)
			{
				if (SinceStep > 8.0)
				{
					Report(Run, false, TEXT("B: the pawn respawned under the menu"), TEXT("no new body in 8 s"));
					GoTo(Run, EStep::CloseMenuAfterRespawn);
				}
				return true;
			}
			if (SinceStep < 0.5)
			{
				return true;   // let both possession paths (OnPossess, AcknowledgePossession) run
			}
			Report(Run, HumanPC->IsTeamSelectOpen() && HumanPC->IsGameInputSuppressed() && HasMenuMouse(HumanPC, WorldPtr),
				TEXT("*** B1 after a respawn under the open team screen, the menu keeps the mouse ***"),
				DescribeInput(HumanPC, WorldPtr));
			GoTo(Run, EStep::CloseMenuAfterRespawn);
			return true;
		}

		case EStep::CloseMenuAfterRespawn:
		{
			if (SinceStep < 0.05)
			{
				HumanPC->ServerSetTeamSelectOpen(/*bOpen=*/false, 0.f);
				return true;
			}
			if (HumanPC->IsGameInputSuppressed() && SinceStep < 2.0)
			{
				return true;
			}
			Report(Run, !HumanPC->IsGameInputSuppressed() && HasGameMouse(HumanPC, WorldPtr),
				TEXT("B2 closing the menu hands the mouse back to the game"), DescribeInput(HumanPC, WorldPtr));
			GoTo(Run, EStep::OpenMenuForHalfTime);
			return true;
		}

		// ---- E. HALF TIME SHUTS THE TEAM SCREEN ---------------------------------------------------
		case EStep::OpenMenuForHalfTime:
		{
			if (MatchGS->IsHalfTimeBreak() || MatchGS->CurrentHalf >= MatchGS->NumHalves)
			{
				Skip(Run, TEXT("E: half time over an open team screen"),
					FString::Printf(TEXT("no half-time whistle left to blow (half %d of %d, break %d)"),
						MatchGS->CurrentHalf, MatchGS->NumHalves, MatchGS->IsHalfTimeBreak() ? 1 : 0));
				GoTo(Run, EStep::OpenMenuForWhistle);
				return true;
			}
			if (SinceStep < 0.3)
			{
				return true;
			}
			if (!HumanPC->IsTeamSelectOpen())
			{
				Rules->OpenTeamSelectFor(HumanPC);   // the H key's server path, which live play accepts
				return true;
			}
			// The bug needs the team page actually UP on this client, not just the replicated flag.
			if ((!HumanPC->IsGameInputSuppressed() || !Hud->GetHudKitRecord().bTeamSelectOpen) && SinceStep < 3.0)
			{
				return true;
			}
			const bool bTeamPageUp = Hud->GetHudKitRecord().bTeamSelectOpen;

			Rules->DebugEndPeriodNow();
			const bool bCharacters = UTraceAbilityComponent::AreCharactersEnabled(HumanPC);
			const double LoadoutBy = static_cast<double>(HumanState->CharacterSelectDeadlineServerTime);
			Report(Run, bTeamPageUp && MatchGS->IsHalfTimeBreak() && !HumanPC->IsTeamSelectOpen()
					&& (!bCharacters || (HumanState->IsCharacterSelectOpen()
						&& FMath::Abs(LoadoutBy - static_cast<double>(MatchGS->MatchEndServerTime)) < 0.6)),
				TEXT("*** E1 the half-time whistle closes the open team screen and opens the loadout window ***"),
				FString::Printf(TEXT("team page drawn before %d, break %d, team screen %d, loadout window %d (characters %d), "
					"window closes %.1fs / break ends %.1fs"),
					bTeamPageUp ? 1 : 0, MatchGS->IsHalfTimeBreak() ? 1 : 0, HumanPC->IsTeamSelectOpen() ? 1 : 0,
					HumanState->IsCharacterSelectOpen() ? 1 : 0, bCharacters ? 1 : 0,
					LoadoutBy - NowServer, static_cast<double>(MatchGS->MatchEndServerTime) - NowServer));
			GoTo(Run, EStep::HalfTimeDrawn);
			return true;
		}

		case EStep::HalfTimeDrawn:
		{
			// A second and a half of HUD frames: long enough for the team page's own close (it follows the
			// replicated flag) to have run, and for -TraceAutoShotRepeat to catch the page in a capture.
			if (SinceStep < 1.5)
			{
				return true;
			}
			const ATraceHUD::FHudKitRecord& Rec = Hud->GetHudKitRecord();
			if (!UTraceAbilityComponent::AreCharactersEnabled(HumanPC))
			{
				Skip(Run, TEXT("E2: the loadout page takes the input at half time"), TEXT("characters are off; no loadout page"));
			}
			else
			{
				// The loadout page's input gate is !pause && !team page (TraceHUD's LoadoutSelect.Tick call);
				// these are the two flags that frame drew with.
				Report(Run, Rec.bLoadoutOpen && !Rec.bTeamSelectOpen && !Rec.bPauseOpen
						&& HumanPC->IsGameInputSuppressed() && HasMenuMouse(HumanPC, WorldPtr),
					TEXT("*** E2 at half time the loadout page is the only page up, and it has the input ***"),
					FString::Printf(TEXT("loadout page drawn %d, team page drawn %d, pause %d; %s"),
						Rec.bLoadoutOpen ? 1 : 0, Rec.bTeamSelectOpen ? 1 : 0, Rec.bPauseOpen ? 1 : 0,
						*DescribeInput(HumanPC, WorldPtr)));
			}
			GoTo(Run, EStep::SecondHalf);
			return true;
		}

		case EStep::SecondHalf:
		{
			if (SinceStep < 0.05)
			{
				if (MatchGS->IsHalfTimeBreak())
				{
					Rules->DebugEndHalfTimeBreak();
				}
				return true;
			}
			if (HumanPC->IsGameInputSuppressed() && SinceStep < 3.0)
			{
				return true;
			}
			Report(Run, !MatchGS->IsHalfTimeBreak() && MatchGS->CurrentHalf == 2 && !HumanPC->IsTeamSelectOpen()
					&& !HumanState->IsCharacterSelectOpen() && !HumanPC->IsGameInputSuppressed(),
				TEXT("E3 the second half's whistle shuts the window and hands the input back"),
				FString::Printf(TEXT("half %d, break %d, team screen %d, loadout window %d; %s"), MatchGS->CurrentHalf,
					MatchGS->IsHalfTimeBreak() ? 1 : 0, HumanPC->IsTeamSelectOpen() ? 1 : 0,
					HumanState->IsCharacterSelectOpen() ? 1 : 0, *DescribeInput(HumanPC, WorldPtr)));
			GoTo(Run, EStep::OpenMenuForWhistle);
			return true;
		}

		// ---- D. FULL TIME SHUTS THE MENUS ---------------------------------------------------------
		case EStep::OpenMenuForWhistle:
		{
			if (SinceStep < 0.3)
			{
				return true;
			}
			if (!HumanPC->IsTeamSelectOpen())
			{
				Rules->OpenTeamSelectFor(HumanPC);
				return true;
			}
			if (!HumanPC->IsGameInputSuppressed() && SinceStep < 3.0)
			{
				return true;
			}

			const ETraceTeam TeamBefore = HumanState->Team;
			Rules->DebugFinishMatch(ETraceTeam::Blue, ETraceMatchEndReason::Clock);
			Report(Run, MatchGS->TraceMatchState == ETraceMatchState::PostMatch && !HumanPC->IsTeamSelectOpen()
					&& !HumanState->IsCharacterSelectOpen(),
				TEXT("*** D1 the whistle closes the open team screen ***"),
				FString::Printf(TEXT("phase %d, team screen %d, loadout window %d"), static_cast<int32>(MatchGS->TraceMatchState),
					HumanPC->IsTeamSelectOpen() ? 1 : 0, HumanState->IsCharacterSelectOpen() ? 1 : 0));

			const ETraceTeamChangeResult Change = Rules->RequestTeamChange(HumanState, OtherTeam);
			Report(Run, Change == ETraceTeamChangeResult::NotAllowed && HumanState->Team == TeamBefore,
				TEXT("*** D2 the server refuses a team change after the whistle ***"),
				FString::Printf(TEXT("result %d, team %s -> %s"), static_cast<int32>(Change),
					*TraceTeamName(TeamBefore).ToString(), *TraceTeamName(HumanState->Team).ToString()));

			// An unsettled human after the whistle: the poll must not open a page over the results. The
			// team screen is shut first (the whistle should already have done it), because the poll never
			// opens a loadout page behind an open team screen and this claim must stand on its own.
			if (HumanPC->IsTeamSelectOpen())
			{
				HumanPC->ServerSetTeamSelectOpen(/*bOpen=*/false, 0.f);
			}
			const bool bWasLocked = HumanState->IsCharacterLocked();
			const bool bWasChosen = HumanState->WasCharacterChosen();
			HumanState->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);
			Rules->DebugPollCharacterSelect();
			const bool bOpened = HumanState->IsCharacterSelectOpen();
			if (bOpened)
			{
				HumanState->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
			}
			HumanState->ServerMarkCharacterResolved(bWasLocked, bWasChosen);
			Report(Run, !bOpened, TEXT("*** D3 the select poll opens nothing over the results ***"),
				FString::Printf(TEXT("window opened %d"), bOpened ? 1 : 0));
			GoTo(Run, EStep::AfterWhistle);
			return true;
		}

		case EStep::AfterWhistle:
		{
			if (HumanPC->IsGameInputSuppressed() && SinceStep < 2.0)
			{
				return true;
			}
			Report(Run, !HumanPC->IsGameInputSuppressed(), TEXT("D4 the results screen is not left behind a dead menu's input"),
				DescribeInput(HumanPC, WorldPtr));
			Finish(Run);
			return false;
		}

		default:
			Finish(Run);
			return false;
		}
	}

	static void Start(const TArray<FString>& /*Args*/, UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr || WorldPtr->GetNetMode() == NM_Client || WorldPtr->GetAuthGameMode<ATraceGameMode>() == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[FlowVerify] Trace.Flow.Verify runs on a host or standalone match (Arena)."));
			return;
		}

		TSharedRef<FRun> Run = MakeShared<FRun>();
		Run->World = WorldPtr;
		Run->Deadline = FPlatformTime::Seconds() + 150.0;
		GoTo(*Run, EStep::Start);

		UE_LOG(LogTraceGame, Display, TEXT("[FlowVerify] ===== the match flow: pre-match hold, toasts, respawn under a menu, half time, full time ====="));
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float /*Delta*/) -> bool
		{
			return Tick(*Run);
		}), 0.f);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdFlowVerify(
		TEXT("Trace.Flow.Verify"),
		TEXT("The match flow end to end (Arena, run early while team select is up, -bots=4): the match clock waits ")
		TEXT("for the pre-match menus and starts a warm-up after lock-in; warm-up K/D wiped; bot names upper case; ")
		TEXT("refusal toasts from the text document; a respawn under an open menu keeps the menu's mouse; half ")
		TEXT("time over an open team screen closes it and leaves the loadout page in charge; full ")
		TEXT("time closes the menus and refuses a team change. Ends the match."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Start));
}

#endif // !UE_BUILD_SHIPPING
