#include "Abilities/Characters/TraceVerifyLock.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"

#include "Trace.h"

namespace TraceVerifyLock
{
	namespace
	{
		/** The fixture holding the subject: its command NAME, which is what Release() compares. */
		FString GHolder;
		/** The whole command line that claimed it, arguments included. For messages only. */
		FString GHolderLine;
		double GDeadlineRealTime = 0.0;

		/** One queued command's wait: who it is behind, and how many one-second retries so far. */
		struct FQueuedWait
		{
			FString Behind;
			int32 Retries = 0;
		};

		/** Keyed by the FULL command line. An entry exists only while that line is queued. */
		TMap<FString, FQueuedWait> GQueuedWaits;

		/** "Trace.Rocco.Verify" + {"1"} -> "Trace.Rocco.Verify 1", which is what the console split. */
		FString JoinCommandLine(const TCHAR* CommandName, const TArray<FString>& Args)
		{
			FString Line(CommandName);
			for (const FString& Arg : Args)
			{
				Line += TEXT(' ');
				Line += Arg;
			}
			return Line;
		}

		bool ClaimAs(const TCHAR* FixtureName, const FString& ClaimLine, double ExpectedSeconds)
		{
			const double Now = FPlatformTime::Seconds();

			if (!GHolder.IsEmpty() && Now < GDeadlineRealTime)
			{
				// Re-claiming by the same fixture is fine and just extends the deadline: a fixture that
				// runs several arms in sequence is one fixture, not three.
				if (GHolder != FixtureName)
				{
					return false;
				}
			}

			if (!GHolder.IsEmpty() && GHolder != FixtureName && Now >= GDeadlineRealTime)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[VerifyLock] %s held the subject past its deadline and never released it. %s is "
					     "taking it. If %s was still running, its result is not trustworthy."),
					*GHolderLine, *ClaimLine, *GHolderLine);
			}

			GHolder = FixtureName;
			GHolderLine = ClaimLine;
			GDeadlineRealTime = Now + FMath::Max(1.0, ExpectedSeconds);
			return true;
		}
	}

	bool TryClaim(const TCHAR* FixtureName, double ExpectedSeconds)
	{
		return ClaimAs(FixtureName, FString(FixtureName), ExpectedSeconds);
	}

	void Release(const TCHAR* FixtureName)
	{
		if (GHolder == FixtureName)
		{
			GHolder.Reset();
			GHolderLine.Reset();
			GDeadlineRealTime = 0.0;
		}
	}

	void ReleaseAny()
	{
		GHolder.Reset();
		GHolderLine.Reset();
		GDeadlineRealTime = 0.0;
	}

	FString CurrentHolder()
	{
		return (FPlatformTime::Seconds() < GDeadlineRealTime) ? GHolderLine : FString();
	}

	bool ClaimOrQueue(const TCHAR* CommandName, double ExpectedSeconds, const TArray<FString>& Args)
	{
		// THE WHOLE LINE, not the name: it is what the requeue re-runs and what the wait is keyed on.
		const FString Line = JoinCommandLine(CommandName, Args);

		// Every call here is a fresh invocation, so a live hold under this same NAME is another run of
		// this command — `Trace.Rocco.Verify 2` arriving while `Trace.Rocco.Verify 1` runs — and not
		// the fixture re-claiming itself. ClaimAs would let it straight in on top of the first.
		const bool bAnotherRunOfThisCommand =
			(GHolder == CommandName) && (FPlatformTime::Seconds() < GDeadlineRealTime);

		if (!bAnotherRunOfThisCommand && ClaimAs(CommandName, Line, ExpectedSeconds))
		{
			// Whatever this command waited through is over. Forgetting it HERE is what lets the same
			// command queue again later in the session with a clean slate — the old counter was never
			// cleared on a claim, so a second batch started its three minutes part-spent.
			GQueuedWaits.Remove(Line);
			return true;
		}

		// ---- queued -----------------------------------------------------------------------------
		//
		// A COUNT, NOT A CLOCK, and COUNTED PER HOLDER: each retry is a second, and the count starts
		// again whenever the subject changes hands. The old count ran across the whole wait, so in a
		// batch of eight the last three spent their three minutes queued behind OTHER queued fixtures
		// and gave up without measuring anything. Still bounded, so a holder that keeps re-claiming
		// cannot leave a command re-executing itself for the rest of the session — that would be a
		// far worse bug than the one this file fixes, and a silent one. (A holder that simply dies
		// cannot do it: its claim expires and the next retry takes it.)
		const FString HolderNow = CurrentHolder();
		FQueuedWait* ExistingWait = GQueuedWaits.Find(Line);
		const bool bNewHolder = (ExistingWait == nullptr) || (ExistingWait->Behind != HolderNow);
		FQueuedWait& MyWait = GQueuedWaits.FindOrAdd(Line);
		if (bNewHolder)
		{
			MyWait.Behind = HolderNow;
			MyWait.Retries = 0;

			// ONE LINE PER HOLDER, from here, rather than one per retry from every fixture.
			UE_LOG(LogTraceGame, Display,
				TEXT("[VerifyLock] %s is queued behind %s and will start when it finishes."),
				*Line, *HolderNow);
		}

		if (++MyWait.Retries > 180)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[VerifyLock] %s waited three minutes for %s and gave up. Nothing was measured."),
				*Line, *HolderNow);
			GQueuedWaits.Remove(Line);
			return false;
		}

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Line](float) -> bool
			{
				if (GEngine != nullptr)
				{
					// Re-enters the command WITH ITS ARGUMENTS, which hits this same guard and either
					// claims (and forgets the wait) or queues again.
					GEngine->Exec(nullptr, *Line);
				}
				return false;   // one shot
			}), 1.0f);

		return false;
	}

	FTickerDelegate ReleaseWhenFinished(const TCHAR* FixtureName, FTickerDelegate Body)
	{
		return FTickerDelegate::CreateLambda(
			[HeldBy = FString(FixtureName), Inner = MoveTemp(Body)](float DeltaTime) -> bool
			{
				// An unbound body has nothing to run, which is a finished run: release rather than
				// hold the subject for a fixture that will never tick.
				const bool bKeepTicking = Inner.IsBound() && Inner.Execute(DeltaTime);
				if (!bKeepTicking)
				{
					Release(*HeldBy);
				}
				return bKeepTicking;
			});
	}
}
