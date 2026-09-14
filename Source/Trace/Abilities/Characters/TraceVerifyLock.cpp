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
		if (TryClaim(CommandName, ExpectedSeconds))
		{
			return true;
		}

		// ---- queued -----------------------------------------------------------------------------
		//
		// A COUNT, NOT A CLOCK, and the count is generous: each retry is a second, and the longest of
		// these fixtures runs well under two minutes. Bounded so a holder that dies mid-run cannot
		// leave a command re-executing itself for the rest of the session — that would be a far worse
		// bug than the one this file fixes, and a silent one.
		static TMap<FString, int32> Attempts;
		const FString Key(CommandName);
		int32& Count = Attempts.FindOrAdd(Key);

		if (++Count > 180)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[VerifyLock] %s waited three minutes for %s and gave up. Nothing was measured."),
				CommandName, *CurrentHolder());
			Attempts.Remove(Key);
			return false;
		}

		if (Count == 1)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[VerifyLock] %s is queued behind %s and will start when it finishes."),
				CommandName, *CurrentHolder());
		}

		const FString Command(CommandName);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Command, Key](float) -> bool
			{
				if (GEngine != nullptr)
				{
					// Re-enters the command, which hits this same guard and either claims or queues
					// again. On the claim, the counter is cleared by the success path below.
					GEngine->Exec(nullptr, *Command);
				}
				return false;   // one shot
			}), 1.0f);

		return false;
	}
}
