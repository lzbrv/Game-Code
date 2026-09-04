// Copyright (c) Trace. All Rights Reserved.

#include "Core/TraceGameState.h"

#include "Net/UnrealNetwork.h"

#include "Engine/World.h"                // AActor::GetWorld() (ApplyTeamSidesInWorld)
#include "GameFramework/PlayerState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Trace.h"
#include "World/TraceArenaBuilder.h"   // repaint the endzones when the sides switch

ATraceGameState::ATraceGameState()
{
	PrimaryActorTick.bCanEverTick = false;

	// AGameStateBase already sets both; stated explicitly per contract §8 so nobody has to go
	// reading engine source to be sure the match state actually reaches every client.
	bReplicates = true;
	bAlwaysRelevant = true;
}

void ATraceGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// All COND_None: every one of these is on the HUD of every player, including spectators.
	DOREPLIFETIME(ATraceGameState, BlueScore);
	DOREPLIFETIME(ATraceGameState, OrangeScore);
	DOREPLIFETIME(ATraceGameState, TraceMatchState);
	DOREPLIFETIME(ATraceGameState, MatchEndServerTime);
	DOREPLIFETIME(ATraceGameState, Core);

	// The half/side block is small, changes a handful of times per match and decides what every
	// client draws and which way every bot runs, so it is unconditional and pushed eagerly.
	DOREPLIFETIME(ATraceGameState, CurrentHalf);
	DOREPLIFETIME(ATraceGameState, NumHalves);
	DOREPLIFETIME(ATraceGameState, bHalfTimeBreak);

	// Spec v9 §11. Both are on the HUD of every player the moment the clock hits 0:00 and play
	// carries on, so they travel with the rest of the half block and under the same conditions.
	DOREPLIFETIME(ATraceGameState, bPendingPeriodEnd);
	DOREPLIFETIME(ATraceGameState, PendingPeriodEndCapServerTime);

	DOREPLIFETIME(ATraceGameState, TeamOnNegativeSide);
	DOREPLIFETIME(ATraceGameState, LastWipeBonusTeam);
	DOREPLIFETIME(ATraceGameState, LastWipeBonusServerTime);

	// The scoring mode decides what the world is made of and what the mouse button does, so it has
	// to reach a late joiner before their first frame — unconditional, like the rest of this block.

	DOREPLIFETIME(ATraceGameState, MatchEndReason);
	DOREPLIFETIME(ATraceGameState, MatchWinner);
}

int32 ATraceGameState::GetScore(ETraceTeam Team) const
{
	switch (Team)
	{
	case ETraceTeam::Blue:
		return BlueScore;
	case ETraceTeam::Orange:
		return OrangeScore;
	default:
		return 0;
	}
}

void ATraceGameState::AddScore(ETraceTeam Team, int32 Amount)
{
	if (!HasAuthority())
	{
		return;
	}

	switch (Team)
	{
	case ETraceTeam::Blue:
		BlueScore = FMath::Max(0, BlueScore + Amount);
		break;
	case ETraceTeam::Orange:
		OrangeScore = FMath::Max(0, OrangeScore + Amount);
		break;
	default:
		// Nobody scores for ETraceTeam::None.
		return;
	}

	// OnRep callbacks never fire on the authority, so drive it by hand — a listen server's own HUD
	// and logs must see exactly what a remote client sees.
	OnRep_Scores();

	// A goal is a rare, high-value event: do not wait for the GameState's normal net update slot.
	ForceNetUpdate();
}

float ATraceGameState::GetMatchTimeRemaining() const
{
	// GetServerWorldTimeSeconds() returns double on 5.3+ (it was float before), so keep the maths in
	// double and narrow only at the end — that stays correct whichever signature the engine has.
	const double Remaining = static_cast<double>(MatchEndServerTime) - GetServerWorldTimeSeconds();
	return FMath::Max(0.f, static_cast<float>(Remaining));
}

int32 ATraceGameState::CountTeamMembers(ETraceTeam Team) const
{
	int32 Count = 0;
	for (const APlayerState* PlayerState : PlayerArray)
	{
		// Cast<To>(const From*) resolves to the const overload and yields const To*, so the template
		// argument must stay non-const.
		const ATracePlayerState* TracePlayerState = Cast<ATracePlayerState>(PlayerState);
		if (TracePlayerState == nullptr || TracePlayerState->IsOnlyASpectator())
		{
			continue;
		}

		if (TracePlayerState->Team == Team)
		{
			++Count;
		}
	}

	return Count;
}

// ---------------------------------------------------------------------------------------------
// Halves and sides
// ---------------------------------------------------------------------------------------------

float ATraceGameState::GetDefendedEndSign(ETraceTeam Team) const
{
	if (Team == ETraceTeam::None)
	{
		return 0.f;
	}

	// TeamOnNegativeSide is the whole side assignment. Everything else is this one comparison, which
	// is exactly why it is worth a function: a `Team == Blue ? -1 : +1` written anywhere else in the
	// codebase is a bug that only shows up in the second half, when it is hardest to notice.
	const ETraceTeam NegativeSideTeam = (TeamOnNegativeSide == ETraceTeam::None) ? ETraceTeam::Blue : TeamOnNegativeSide;
	return (Team == NegativeSideTeam) ? -1.f : 1.f;
}

float ATraceGameState::GetAttackEndSign(ETraceTeam Team) const
{
	return -GetDefendedEndSign(Team);
}

ETraceTeam ATraceGameState::GetTeamDefendingEnd(float XSign) const
{
	const ETraceTeam NegativeSideTeam = (TeamOnNegativeSide == ETraceTeam::None) ? ETraceTeam::Blue : TeamOnNegativeSide;
	return (XSign < 0.f) ? NegativeSideTeam : TraceOpposingTeam(NegativeSideTeam);
}

ETraceTeam ATraceGameState::GetTeamAttackingEnd(float XSign) const
{
	return TraceOpposingTeam(GetTeamDefendingEnd(XSign));
}

bool ATraceGameState::AreSidesSwapped() const
{
	return TeamOnNegativeSide != ETraceTeam::Blue;
}

bool ATraceGameState::IsHalfTimeBreak() const
{
	return bHalfTimeBreak;
}

int32 ATraceGameState::GetCurrentHalf() const
{
	return CurrentHalf;
}

FString ATraceGameState::GetHalfLabel() const
{
	if (bHalfTimeBreak)
	{
		return TEXT("HALF TIME");
	}

	// Ordinals only go as far as the number of halves anyone will ever configure; beyond that fall
	// back to a plain count rather than inventing English.
	switch (CurrentHalf)
	{
	case 1:  return (NumHalves <= 1) ? FString(TEXT("MATCH")) : FString(TEXT("1ST HALF"));
	case 2:  return TEXT("2ND HALF");
	case 3:  return TEXT("3RD HALF");
	default: return FString::Printf(TEXT("PERIOD %d"), CurrentHalf);
	}
}

// ---------------------------------------------------------------------------------------------
// Deferred half time / full time (spec v9 §11)
// ---------------------------------------------------------------------------------------------

bool ATraceGameState::IsPendingPeriodEnd() const
{
	// The break itself and the results screen both clear the flag, but ask anyway: a pending whistle
	// is by definition a thing that happens DURING play, and reporting one during the interval would
	// have the HUD promise a break that is already on screen.
	return bPendingPeriodEnd && !bHalfTimeBreak && TraceMatchState == ETraceMatchState::InProgress;
}

FString ATraceGameState::GetPendingPeriodEndLabel() const
{
	if (!IsPendingPeriodEnd())
	{
		return FString();
	}

	// "HALF" or "MATCH" depending on whether there is another period to come — the same test
	// ATraceGameMode::EndPeriodNow() makes, so the HUD can never promise a half time that is
	// actually full time.
	return (CurrentHalf < NumHalves)
		? FString(TEXT("HALF ENDS AT NEXT DEAD BALL"))
		: FString(TEXT("MATCH ENDS AT NEXT DEAD BALL"));
}

float ATraceGameState::GetPendingPeriodEndTimeRemaining() const
{
	if (!IsPendingPeriodEnd() || PendingPeriodEndCapServerTime <= 0.f)
	{
		return 0.f;
	}

	// Double maths for the same reason GetMatchTimeRemaining does it: GetServerWorldTimeSeconds()
	// returns double on 5.3+.
	const double Remaining = static_cast<double>(PendingPeriodEndCapServerTime) - GetServerWorldTimeSeconds();
	return FMath::Max(0.f, static_cast<float>(Remaining));
}

void ATraceGameState::SetPendingPeriodEnd(bool bInPending, float InCapServerTime)
{
	if (!HasAuthority())
	{
		return;
	}

	bPendingPeriodEnd = bInPending;
	PendingPeriodEndCapServerTime = bInPending ? FMath::Max(0.f, InCapServerTime) : 0.f;

	// OnRep never fires on the authority. The listen host's own HUD has to show "NEXT DEAD BALL"
	// exactly when a remote client's does.
	OnRep_HalfChanged();

	// Once or twice a period, and it changes what every player is looking at. Never wait for the
	// GameState's normal net update slot.
	ForceNetUpdate();
}

void ATraceGameState::SetTeamSides(ETraceTeam InTeamOnNegativeSide)
{
	if (!HasAuthority() || InTeamOnNegativeSide == ETraceTeam::None)
	{
		return;
	}

	TeamOnNegativeSide = InTeamOnNegativeSide;
	OnRep_SidesChanged();
	ForceNetUpdate();
}

void ATraceGameState::SetHalfState(int32 InCurrentHalf, int32 InNumHalves, bool bInHalfTimeBreak)
{
	if (!HasAuthority())
	{
		return;
	}

	CurrentHalf = FMath::Max(1, InCurrentHalf);
	NumHalves = FMath::Max(1, InNumHalves);
	bHalfTimeBreak = bInHalfTimeBreak;

	OnRep_HalfChanged();
	ForceNetUpdate();
}

void ATraceGameState::NotifyWipeBonus(ETraceTeam BonusTeam)
{
	if (!HasAuthority() || BonusTeam == ETraceTeam::None)
	{
		return;
	}

	LastWipeBonusTeam = BonusTeam;
	LastWipeBonusServerTime = static_cast<float>(GetServerWorldTimeSeconds());

	OnRep_WipeBonus();
	ForceNetUpdate();
}

// ---------------------------------------------------------------------------------------------
// Result (spec v4 §6)
//
// Spec v4 §7's scoring mode used to be published beside it, replicated as a byte with an OnRep that
// broadcast to whatever had to rebuild its world. There is one ruleset now, so there is nothing to
// publish, nothing to replicate and nothing to rebuild.
// ---------------------------------------------------------------------------------------------

ETraceMatchEndReason ATraceGameState::GetMatchEndReason() const
{
	return MatchEndReason;
}

ETraceTeam ATraceGameState::GetMatchWinner() const
{
	return MatchWinner;
}

bool ATraceGameState::DidMatchEndByMercy() const
{
	return MatchEndReason == ETraceMatchEndReason::Mercy;
}

void ATraceGameState::SetMatchResult(ETraceTeam WinningTeam, ETraceMatchEndReason Reason)
{
	if (!HasAuthority())
	{
		return;
	}

	MatchWinner = WinningTeam;
	MatchEndReason = Reason;
	ForceNetUpdate();
}

// ---------------------------------------------------------------------------------------------
// Replication callbacks
// ---------------------------------------------------------------------------------------------

void ATraceGameState::OnRep_Scores()
{
	// Nothing to push: the HUD polls GetScore() every frame. The log is the cheapest way to watch
	// score replication actually land on a client during a two-instance net test.
	UE_LOG(LogTraceGame, Verbose, TEXT("Scores replicated: Blue %d - Orange %d"), BlueScore, OrangeScore);
}

void ATraceGameState::OnRep_HalfChanged()
{
	// Log at Display, not Verbose: half time is a once-a-match event and the single most useful line
	// in the log when something about the second half looks wrong. (See the project's hard-won
	// lesson about mechanics being declared dead because their log line was suppressed.)
	UE_LOG(LogTraceGame, Display, TEXT("[Half] %s  (half %d of %d)%s"), *GetHalfLabel(), CurrentHalf, NumHalves,
		bPendingPeriodEnd ? TEXT("  [PENDING: ends at the next dead ball]") : TEXT(""));
}

void ATraceGameState::OnRep_SidesChanged()
{
	UE_LOG(LogTraceGame, Display, TEXT("[Sides] %s now defends the -X end (%s defends +X)."),
		*TraceTeamName(TeamOnNegativeSide).ToString(),
		*TraceTeamName(TraceOpposingTeam(TeamOnNegativeSide)).ToString());

	// Repaint the arena on THIS machine. The builder is not a replicated actor — every client
	// constructs its own copy of the arena from the same parameters — so the server calling
	// ApplyTeamSides on its own builder does nothing for anybody else. This OnRep is the only place
	// a client learns the sides changed, so it is the only place a client can act on it.
	//
	// Runs on the listen host too, harmlessly: ApplyTeamSides is idempotent and early-outs when the
	// paint already matches.
	ATraceArenaBuilder::ApplyTeamSidesInWorld(GetWorld(), TeamOnNegativeSide);
}

void ATraceGameState::OnRep_WipeBonus()
{
	UE_LOG(LogTraceGame, Display, TEXT("[Wipe] %s wiped the enemy team."), *TraceTeamName(LastWipeBonusTeam).ToString());
}
