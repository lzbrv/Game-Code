// Trace — Trace.Net.FailureVerify: what a network failure puts on the player's screen, held to account.
//
// Four claims, each of which the code before this pass fails:
//
//   1. THE WORDS. Every engine failure code's line (TraceNet::DescribeNetworkFailure /
//      DescribeTravelFailure) is one line, ASCII (the kit's typeface has no em dash), and short
//      enough to sit on one banner. "BUILD MISMATCH. YOURS IS NET ... — COMPARE IT WITH THE HOST'S
//      (TITLE SCREEN, BOTTOM RIGHT)." was neither.
//   2. NO ENGINE TEXT ON SCREEN. A client-side connection timeout is raised through the engine's own
//      delegate (UEngine::BroadcastNetworkFailure) carrying the engine's real kind of error string. The
//      banner the match HUD then draws must carry the player's line and nothing of that string — it
//      used to draw "CONNECTIONTIMEOUT: UNETCONNECTION::TICK: CONNECTION TIMED OUT. CLOSING ..." under
//      the headline.
//   3. A GUEST DROPPING IS NOT THE HOST'S FAILURE. The same timeout raised against this machine's own
//      SERVER driver (a guest's wifi) records no failure and draws no banner. It used to draw "A PLAYER
//      LEFT THE MATCH" in red for 12 s. Needs a listen server (?listen); INCONCLUSIVE otherwise.
//   4. A GUEST WHO LEAVES IS NAMED. A remote player controller is spawned and destroyed — the engine's
//      own path to ATraceGameMode::Logout, the one a clean leave and a timeout both take — and the kill
//      feed must then draw "<NAME> LEFT". A leave used to be announced nowhere.
//
// Headless recipe (live play, so the feed is not under the team / loadout screens):
//   <Arena_Baked>?listen -TraceExecAt=10 -TraceExec="Trace.Teams.Close|Trace.Loadout.Press wait=3,lock"
//   -TraceExec2At=20 -TraceExec2="Trace.Net.FailureVerify"
//
// `joined` is a separate run, on a GUEST: a JOIN that arrived is no longer pending, so a HOST LEFT in the
// match is not taken for a failed join. Host: /Game/Maps/Arena?listen -port=<P>. Guest: /Game/Maps/MainMenu
//   -TraceExecOn=Menu -TraceExecAt=4 -TraceExec="Trace.Menu.JoinOnce 127.0.0.1:<P>"
//   -TraceExec2On=Match -TraceExec2At=8 -TraceExec2="Trace.Net.FailureVerify joined"

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Parse.h"

#include "Trace.h"                        // LogTraceGame
#include "UI/Text/TraceGameText.h"
#include "UI/TraceHUD.h"
#include "UI/TraceKillFeed.h"
#include "UI/TraceNetworking.h"

// Named after the file, not anonymous: Scripts/check-jumbo-build-collisions.py.
namespace TraceNetFailureVerify
{
	/** The engine's own shape of error string for a timeout (UNetConnection::Tick), verbatim in kind. */
	static const TCHAR* const EngineTimeoutText =
		TEXT("UNetConnection::Tick: Connection TIMED OUT. Closing connection.. Elapsed: 20.02, Real: 20.01, ")
		TEXT("Good: 20.01, DriverTime: 51.20, Threshold: 20.00, [UNetConnection] RemoteAddr: 203.0.113.9:7777, ")
		TEXT("Name: IpConnection_2147482380, Driver: Name:GameNetDriver Def:GameNetDriver IpNetDriver_2147482381");

	/** Words that only the engine's string contains; none may reach a screen. */
	static bool CarriesEngineText(const FString& Line)
	{
		return Line.Contains(TEXT("UNETCONNECTION"), ESearchCase::IgnoreCase)
			|| Line.Contains(TEXT("CONNECTIONTIMEOUT"), ESearchCase::IgnoreCase)
			|| Line.Contains(TEXT("DRIVERTIME"), ESearchCase::IgnoreCase)
			|| Line.Contains(TEXT("IPCONNECTION"), ESearchCase::IgnoreCase);
	}

	/** The longest line a banner can carry on one line, in characters. */
	static constexpr int32 MaxLineChars = 40;

	/** The name the stand-in guest leaves under. */
	static const TCHAR* const GuestName = TEXT("HARNESS GUEST");

	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		int32 Step = 0;
		double StepStart = 0.0;
		double Deadline = 0.0;
		int32 Passes = 0;
		int32 Failures = 0;
		int32 Inconclusive = 0;
		FString ExpectedHeadline;
		bool bListen = false;

		/** hold=<s>: keep the client-timeout banner up this long before moving on, for a screenshot. */
		double HoldSeconds = 0.0;
	};

	static void Report(FRun& Run, bool bPass, const FString& Claim, const FString& Detail)
	{
		(bPass ? Run.Passes : Run.Failures) += 1;
		UE_LOG(LogTraceGame, Display, TEXT("[NetFailVerify]   %-4s %s  %s"), bPass ? TEXT("ok") : TEXT("FAIL"), *Claim, *Detail);
	}

	static void GoTo(FRun& Run, int32 Step)
	{
		Run.Step = Step;
		Run.StepStart = FPlatformTime::Seconds();
	}

	static ATraceHUD* LocalHud(UWorld* WorldPtr)
	{
		APlayerController* const LocalPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		return (LocalPC != nullptr) ? Cast<ATraceHUD>(LocalPC->GetHUD()) : nullptr;
	}

	static bool IsOneAsciiLine(const FString& Line)
	{
		for (const TCHAR Char : Line)
		{
			if (Char > 127 || Char == TEXT('\n') || Char == TEXT('\r'))
			{
				return false;
			}
		}
		return true;
	}

	/** Claim 1, over every code the engine can raise. */
	static void CheckWords(FRun& Run)
	{
		TArray<FString> Bad;
		int32 Checked = 0;
		const auto Judge = [&Bad, &Checked](const TCHAR* Code, const FString& Line)
		{
			++Checked;
			if (!IsOneAsciiLine(Line) || Line.Len() > MaxLineChars)
			{
				Bad.Add(FString::Printf(TEXT("%s -> \"%s\" (%d chars)"), Code, *Line, Line.Len()));
			}
		};

		for (int32 Code = 0; Code <= static_cast<int32>(ENetworkFailure::NetChecksumMismatch); ++Code)
		{
			const ENetworkFailure::Type Type = static_cast<ENetworkFailure::Type>(Code);
			Judge(ENetworkFailure::ToString(Type), TraceNet::DescribeNetworkFailure(Type));
		}
		for (int32 Code = 0; Code <= static_cast<int32>(ETravelFailure::ClientTravelFailure); ++Code)
		{
			const ETravelFailure::Type Type = static_cast<ETravelFailure::Type>(Code);
			Judge(ETravelFailure::ToString(Type), TraceNet::DescribeTravelFailure(Type));
		}

		Report(Run, Bad.Num() == 0,
			FString::Printf(TEXT("every failure code's line is one ASCII line of at most %d characters"), MaxLineChars),
			Bad.Num() == 0 ? FString::Printf(TEXT("%d codes"), Checked) : FString::Join(Bad, TEXT("; ")));
	}

	static void Finish(FRun& Run)
	{
		TraceNet::ClearFailure();
		if (Run.Failures == 0 && Run.Passes > 0)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[NetFailVerify] VERDICT: ===== PASS (%d checks%s) ====="), Run.Passes,
				Run.Inconclusive > 0 ? TEXT(", some INCONCLUSIVE - see above") : TEXT(""));
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[NetFailVerify] VERDICT: ===== *** FAIL *** %d of %d check(s) ====="),
				Run.Failures, Run.Failures + Run.Passes);
		}
	}

	/** One ticker pass. Returns false when the run is over. */
	static bool Tick(FRun& Run)
	{
		UWorld* const WorldPtr = Run.World.Get();
		ATraceHUD* const Hud = LocalHud(WorldPtr);
		if (WorldPtr == nullptr || Hud == nullptr || GEngine == nullptr)
		{
			Report(Run, false, TEXT("a match world with a local HUD"), TEXT("missing"));
			Finish(Run);
			return false;
		}
		if (FPlatformTime::Seconds() > Run.Deadline)
		{
			Report(Run, false, TEXT("the run finished inside its time limit"), FString::Printf(TEXT("stuck at step %d"), Run.Step));
			Finish(Run);
			return false;
		}

		const ATraceHUD::FHudKitRecord& Record = Hud->GetHudKitRecord();
		const double SinceStep = FPlatformTime::Seconds() - Run.StepStart;
		switch (Run.Step)
		{
		case 0:   // wait for live play: the banner and the feed do not draw under an overlay
		{
			if (Record.bOverlayUp)
			{
				if (SinceStep > 20.0)
				{
					Report(Run, false, TEXT("a live frame with no overlay up"), TEXT("an overlay stayed up for 20 s"));
					Finish(Run);
					return false;
				}
				return true;
			}

			CheckWords(Run);

			// ---- 2. a CLIENT's timeout, through the engine's own delegate ----------------------------
			// No driver: the shape a failed or dropped client connection reaches the handlers in once its
			// driver is gone, and one the engine's own handler ignores (it acts only on a named driver).
			TraceNet::ClearFailure();
			Run.ExpectedHeadline = TraceNet::DescribeNetworkFailure(ENetworkFailure::ConnectionTimeout);
			GEngine->BroadcastNetworkFailure(WorldPtr, nullptr, ENetworkFailure::ConnectionTimeout, EngineTimeoutText);

			FString Headline;
			double Age = 0.0;
			const bool bRecorded = TraceNet::GetLastFailure(Headline, Age);
			Report(Run, bRecorded && Headline == Run.ExpectedHeadline && !CarriesEngineText(Headline),
				TEXT("a client's timeout is recorded as the player's line, not the engine's"),
				FString::Printf(TEXT("recorded %d, \"%s\""), bRecorded ? 1 : 0, *Headline));
			GoTo(Run, 1);
			return true;
		}

		case 1:   // ...and the banner the HUD drew for it
		{
			if (SinceStep < FMath::Max(0.3, Run.HoldSeconds))
			{
				return true;
			}
			bool bEngineTextDrawn = false;
			for (const FString& Line : Record.NetFailureLines)
			{
				bEngineTextDrawn |= CarriesEngineText(Line);
			}
			Report(Run, Record.bNetFailurePanel && !bEngineTextDrawn
					&& Record.NetFailureLines.Num() == 1 && Record.NetFailureLines[0] == Run.ExpectedHeadline,
				TEXT("the in-match banner draws that one line and none of the engine's text"),
				FString::Printf(TEXT("panel %d, lines [%s]"), Record.bNetFailurePanel ? 1 : 0,
					*FString::Join(Record.NetFailureLines, TEXT(" | "))));

			// ---- 3. a GUEST's timeout, raised against this machine's own server driver -----------------
			TraceNet::ClearFailure();
			UNetDriver* const ServerDriver = WorldPtr->GetNetDriver();
			Run.bListen = (ServerDriver != nullptr) && (ServerDriver->ServerConnection == nullptr);
			if (Run.bListen)
			{
				// ConnectionTimeout on a server driver: the engine's own handler does not travel for it
				// ("Hosts don't travel when clients disconnect"), so this is safe to raise on a live host.
				GEngine->BroadcastNetworkFailure(WorldPtr, ServerDriver, ENetworkFailure::ConnectionTimeout, EngineTimeoutText);
				FString Headline;
				double Age = 0.0;
				const bool bRecorded = TraceNet::GetLastFailure(Headline, Age);
				Report(Run, !bRecorded, TEXT("a guest timing out records no failure on the host"),
					bRecorded ? FString::Printf(TEXT("recorded \"%s\""), *Headline) : FString(TEXT("nothing recorded")));
			}
			else
			{
				++Run.Inconclusive;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[NetFailVerify]   INCONCLUSIVE: not a listen server (no server driver), so a guest's timeout cannot be raised. Run the map with ?listen."));
			}
			GoTo(Run, 2);
			return true;
		}

		case 2:
		{
			if (SinceStep < 0.3)
			{
				return true;
			}
			if (Run.bListen)
			{
				Report(Run, !Record.bNetFailurePanel, TEXT("...and the host's screen shows no banner for it"),
					FString::Printf(TEXT("panel %d, lines [%s]"), Record.bNetFailurePanel ? 1 : 0,
						*FString::Join(Record.NetFailureLines, TEXT(" | "))));
			}

			// ---- 4. a guest leaves --------------------------------------------------------------------
			// A player controller with no local player is a remote one to IsLocalController, on a listen
			// server and in standalone alike. Its PlayerState comes from the game mode (PostInitialize-
			// Components), and Destroy() is AController::Destroyed -> GameMode->Logout: the engine's own
			// path for a guest who leaves or times out (UNetConnection::CleanUp -> OnNetCleanup -> Destroy).
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			APlayerController* const Guest = WorldPtr->SpawnActor<APlayerController>(APlayerController::StaticClass(),
				FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
			if (Guest == nullptr || Guest->PlayerState == nullptr || Guest->IsLocalController())
			{
				Report(Run, false, TEXT("a stand-in remote guest can be spawned"),
					FString::Printf(TEXT("pc %d, state %d, local %d"), Guest != nullptr ? 1 : 0,
						(Guest != nullptr && Guest->PlayerState != nullptr) ? 1 : 0,
						(Guest != nullptr && Guest->IsLocalController()) ? 1 : 0));
				if (Guest != nullptr)
				{
					Guest->Destroy();
				}
				Finish(Run);
				return false;
			}
			Guest->PlayerState->SetPlayerName(GuestName);
			Guest->Destroy();

			const ATraceKillFeedRelay* const Feed = ATraceKillFeedRelay::Find(WorldPtr);
			const FTraceKillFeedEntry* const Newest = (Feed != nullptr && Feed->GetEntries().Num() > 0) ? &Feed->GetEntries()[0] : nullptr;
			Report(Run, Newest != nullptr && Newest->Icon == ETraceKillIcon::Left && Newest->VictimName == GuestName && !Newest->bHasKiller,
				TEXT("a guest's Logout puts a LEFT row in the replicated kill feed"),
				(Newest != nullptr)
					? FString::Printf(TEXT("newest row: \"%s\", icon %d, killer %d"), *Newest->VictimName,
						static_cast<int32>(Newest->Icon), Newest->bHasKiller ? 1 : 0)
					: FString(TEXT("the feed is empty")));
			GoTo(Run, 3);
			return true;
		}

		case 3:
		{
			if (SinceStep < 0.4)
			{
				return true;
			}
			const FString Expected = FString(GuestName) + TEXT(" ") + TRACE_TEXT("HUD.FEED_LEFT", "LEFT");
			Report(Run, Record.KillFeedTexts.Contains(Expected),
				TEXT("...and every screen's kill feed draws it as \"<NAME> LEFT\""),
				FString::Printf(TEXT("expected \"%s\", feed [%s]"), *Expected, *FString::Join(Record.KillFeedTexts, TEXT(" | "))));
			Finish(Run);
			return false;
		}

		default:
			Finish(Run);
			return false;
		}
	}

	/**
	 * `joined`, on a GUEST that came in through the title's JOIN prompt (Trace.Menu.JoinOnce): the join is
	 * over once the guest is in the match. Two claims, each failed by the code before NoteJoinArrived:
	 * the attempt is no longer pending, and a HOST LEFT raised now — inside the 150 s connect window,
	 * which is where a short match or an early RETURN TO TITLE puts it — is not taken for a failed join
	 * by the title screen's own test (ConsumeFailedJoin, what the next title's BeginPlay asks).
	 */
	static void RunJoined(UWorld* WorldPtr)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[NetFailVerify] ===== a JOIN that arrived is over ====="));
		if (WorldPtr == nullptr || WorldPtr->GetNetMode() != NM_Client)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[NetFailVerify] VERDICT: ===== *** FAIL *** INCONCLUSIVE: 'joined' runs on a guest in a match ====="));
			return;
		}
		if (TraceNet::GetJoinAttemptCount() == 0)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[NetFailVerify] VERDICT: ===== *** FAIL *** INCONCLUSIVE: this guest did not come in through the JOIN prompt ")
				TEXT("(start it on the title with Trace.Menu.JoinOnce <host>) ====="));
			return;
		}

		FRun Run;
		Run.World = WorldPtr;
		const bool bPending = TraceNet::IsJoinAttemptPending();
		Report(Run, !bPending, TEXT("the JOIN that brought this guest in is no longer pending"),
			FString::Printf(TEXT("pending %d, joins dialled this process %d"), bPending ? 1 : 0, TraceNet::GetJoinAttemptCount()));

		// The failure the finding names: the host leaves. ReportHostLeft is the call ClientHostLeft makes.
		TraceNet::ReportHostLeft();
		FString Headline;
		const bool bTakenForJoin = TraceNet::ConsumeFailedJoin(Headline);
		TraceNet::ClearFailure();
		Report(Run, !bTakenForJoin,
			TEXT("*** a HOST LEFT in the match is not taken for a failed JOIN (the title shows the banner, not the prompt) ***"),
			bTakenForJoin ? FString::Printf(TEXT("the title would reopen JOIN with \"%s\" under the field"), *Headline)
			              : FString(TEXT("not a failed join")));
		Finish(Run);
	}

	static void Start(const TArray<FString>& Args, UWorld* WorldPtr)
	{
		if (Args.Contains(TEXT("joined")))
		{
			RunJoined(WorldPtr);
			return;
		}

		// raise: only raise the client timeout through the engine's delegate, and stop. For photographing
		// the banners on any screen, the title's included (which this harness cannot otherwise run on).
		if (Args.Contains(TEXT("raise")))
		{
			if (GEngine != nullptr && WorldPtr != nullptr)
			{
				TraceNet::ClearFailure();
				GEngine->BroadcastNetworkFailure(WorldPtr, nullptr, ENetworkFailure::ConnectionTimeout, EngineTimeoutText);
				UE_LOG(LogTraceGame, Display, TEXT("[NetFailVerify] raised a client ConnectionTimeout carrying the engine's error text."));
			}
			return;
		}

		if (WorldPtr == nullptr || WorldPtr->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[NetFailVerify] Trace.Net.FailureVerify runs on a host or standalone match."));
			return;
		}

		TSharedRef<FRun> Run = MakeShared<FRun>();
		Run->World = WorldPtr;
		for (const FString& Arg : Args)
		{
			double Hold = 0.0;
			if (FParse::Value(*Arg, TEXT("hold="), Hold))
			{
				Run->HoldSeconds = FMath::Clamp(Hold, 0.0, 10.0);
			}
		}
		Run->Deadline = FPlatformTime::Seconds() + 45.0 + Run->HoldSeconds;
		GoTo(*Run, 0);

		UE_LOG(LogTraceGame, Display, TEXT("[NetFailVerify] ===== what a network failure puts on screen ====="));
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float /*Delta*/) -> bool
		{
			return Tick(*Run);
		}), 0.f);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdNetFailureVerify(
		TEXT("Trace.Net.FailureVerify"),
		TEXT("What a network failure puts on screen: every failure code's line is one short ASCII line; a ")
		TEXT("client timeout (raised through the engine's own delegate) draws that line and none of the ")
		TEXT("engine's text; a guest's timeout on a listen host records nothing and draws nothing; and a ")
		TEXT("guest's Logout draws \"<NAME> LEFT\" in the kill feed. Run in live play; ?listen for claim 3. ")
		TEXT("hold=<s> keeps the banner up for a screenshot; 'raise' only raises the client timeout (any screen). ")
		TEXT("'joined', on a guest that came in through the JOIN prompt: the join is no longer pending, and a HOST ")
		TEXT("LEFT in the match is not taken for a failed join."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Start));
}

#endif // !UE_BUILD_SHIPPING
