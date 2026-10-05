// Trace — waiting out the kickoff instead of racing it. See TraceFixtureKickoff.h.

#include "Debug/TraceFixtureKickoff.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include "Core/TraceGameState.h"
#include "TraceTypes.h"

namespace TraceFixtureKickoff
{
	namespace RedArm
	{
		int32 GRaceKickoff = 0;

		FAutoConsoleVariableRef CVarRaceKickoff(
			TEXT("Trace.FixtureKickoff.Race"),
			GRaceKickoff,
			TEXT("RED ARM. 1 = IsHalfBoundaryDue() always answers no, so the fixtures that wait out the half-1 "
			     "kickoff race it again, exactly as they did before (Trace.Lily.FlightTest / KeyTest / TapTest, "
			     "Trace.Rocco.Verify, Trace.Move.AuditV16.Abilities). 0 (default) = they wait."),
			ECVF_Cheat);
	}

	bool IsHalfBoundaryDue(const UWorld* World, float SecondsNeeded, FString& OutWhy)
	{
		OutWhy.Reset();

		if (RedArm::GRaceKickoff != 0)
		{
			return false;   // the red arm: the pre-fix fixtures, which never asked
		}

		const ATraceGameState* const MatchState = (World != nullptr) ? World->GetGameState<ATraceGameState>() : nullptr;
		if (MatchState == nullptr)
		{
			return false;   // not a Trace match (a menu, a test map): there is no kickoff to race
		}

		switch (MatchState->TraceMatchState)
		{
		case ETraceMatchState::WaitingForPlayers:
			if (MatchState->MatchEndServerTime > 0.f)
			{
				OutWhy = FString::Printf(
					TEXT("the warm-up is counting down to the half-1 kickoff (%.1fs left at most), which puts every "
					     "pawn back on its spawn and clears every ability's state"),
					MatchState->GetMatchTimeRemaining());
				return true;
			}
			return false;   // nobody has scheduled a kickoff yet

		case ETraceMatchState::InProgress:
			if (MatchState->IsHalfTimeBreak())
			{
				OutWhy = TEXT("the half-time interval is running, and the next half's kickoff resets every pawn");
				return true;
			}
			if (MatchState->IsPendingPeriodEnd())
			{
				OutWhy = TEXT("the period is over and ends at the next dead ball");
				return true;
			}
			if (MatchState->MatchEndServerTime > 0.f && MatchState->GetMatchTimeRemaining() < SecondsNeeded)
			{
				OutWhy = FString::Printf(TEXT("only %.1fs of this half are left and the fixture needs %.1fs"),
					MatchState->GetMatchTimeRemaining(), SecondsNeeded);
				return true;
			}
			return false;

		default:
			return false;   // post-match: no boundary is coming
		}
	}
}

#endif // !UE_BUILD_SHIPPING
