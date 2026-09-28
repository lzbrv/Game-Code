#include "Abilities/Characters/TraceVerifyLock.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"

#include "Trace.h"

namespace TraceVerifyLock
{
	namespace
	{
		FString GHolder;
		double GDeadlineRealTime = 0.0;

		/** One queued command's wait: who it is behind, and how many one-second retries so far. */
		struct FQueuedWait
		{
			FString Behind;
			int32 Retries = 0;
		};

		/** Keyed by command name. An entry exists only while that command is queued. */
		TMap<FString, FQueuedWait> GQueuedWaits;
	}

	bool TryClaim(const TCHAR* FixtureName, double ExpectedSeconds)
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
				*GHolder, FixtureName, *GHolder);
		}

		GHolder = FixtureName;
		GDeadlineRealTime = Now + FMath::Max(1.0, ExpectedSeconds);
		return true;
	}

	void Release(const TCHAR* FixtureName)
	{
		if (GHolder == FixtureName)
		{
			GHolder.Reset();
			GDeadlineRealTime = 0.0;
		}
	}

	void ReleaseAny()
	{
		GHolder.Reset();
		GDeadlineRealTime = 0.0;
	}

	FString CurrentHolder()
	{
		return (FPlatformTime::Seconds() < GDeadlineRealTime) ? GHolder : FString();
	}

	bool ClaimOrQueue(const TCHAR* CommandName, double ExpectedSeconds)
	{
		const FString QueueKey(CommandName);

		if (TryClaim(CommandName, ExpectedSeconds))
		{
			// Whatever this command waited through is over. Forgetting it HERE is what lets the same
			// command queue again later in the session with a clean slate — the old counter was never
			// cleared on a claim, so a second batch started its three minutes part-spent.
			GQueuedWaits.Remove(QueueKey);
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
		const bool bFirstWait = !GQueuedWaits.Contains(QueueKey);
		FQueuedWait& MyWait = GQueuedWaits.FindOrAdd(QueueKey);
		if (MyWait.Behind != HolderNow)
		{
			MyWait.Behind = HolderNow;
			MyWait.Retries = 0;
		}

		if (++MyWait.Retries > 180)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[VerifyLock] %s waited three minutes for %s and gave up. Nothing was measured."),
				CommandName, *HolderNow);
			GQueuedWaits.Remove(QueueKey);
			return false;
		}

		if (bFirstWait)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[VerifyLock] %s is queued behind %s and will start when it finishes."),
				CommandName, *HolderNow);
		}

		const FString Command(CommandName);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Command](float) -> bool
			{
				if (GEngine != nullptr)
				{
					// Re-enters the command, which hits this same guard and either claims (and forgets
					// the wait) or queues again.
					GEngine->Exec(nullptr, *Command);
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
