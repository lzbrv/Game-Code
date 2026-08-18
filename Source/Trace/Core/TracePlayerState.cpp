// Copyright (c) Trace. All Rights Reserved.

#include "Core/TracePlayerState.h"

#include "Net/UnrealNetwork.h"

#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Abilities/TraceAbilityComponent.h"   // the one home of CharacterId and the E cooldown
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Trace.h"

ATracePlayerState::ATracePlayerState()
{
	PrimaryActorTick.bCanEverTick = false;

	// APlayerState is an AInfo and already replicates; stated explicitly per contract §8.
	bReplicates = true;
}

void ATracePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None throughout: the scoreboard shows every player's team, K/D and carrier flag, so all
	// of this has to reach every client, not just the owner.
	DOREPLIFETIME(ATracePlayerState, Team);
	DOREPLIFETIME(ATracePlayerState, Kills);
	DOREPLIFETIME(ATracePlayerState, Deaths);
	DOREPLIFETIME(ATracePlayerState, bIsCarrier);
	DOREPLIFETIME(ATracePlayerState, RespawnEndServerTime);

	// The CHARACTER is deliberately absent from this list. It lives on UTraceAbilityComponent (which
	// hangs off this actor) and is replicated there, to everyone — the select screen needs every
	// team-mate's pick to grey a card out. A second copy here could disagree with it.

	// The select SESSION is owner-only: it is a private screen, and ten players' worth of deadlines
	// on every connection is traffic nothing reads. bCharacterLocked is owner-only too — it is a
	// statement about this player's own menu, not about the roster (the roster is the component's
	// CharacterId, which everybody already has).
	DOREPLIFETIME_CONDITION(ATracePlayerState, bCharacterLocked, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(ATracePlayerState, bCharacterWasChosen, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(ATracePlayerState, bCharacterSelectOpen, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(ATracePlayerState, CharacterSelectDeadlineServerTime, COND_OwnerOnly);
}

float ATracePlayerState::GetRespawnTimeRemaining() const
{
	if (RespawnEndServerTime <= 0.f)
	{
		return 0.f;
	}

	const AGameStateBase* BaseGameState = (GetWorld() != nullptr) ? GetWorld()->GetGameState() : nullptr;
	if (BaseGameState == nullptr)
	{
		return 0.f;
	}

	// Kept in double until the end: GetServerWorldTimeSeconds() is double on 5.3+.
	const double Remaining = static_cast<double>(RespawnEndServerTime) - BaseGameState->GetServerWorldTimeSeconds();
	return FMath::Max(0.f, static_cast<float>(Remaining));
}

void ATracePlayerState::SetTeam(ETraceTeam NewTeam)
{
	if (!HasAuthority())
	{
		return;
	}

	Team = NewTeam;

	// Replication callbacks never fire on the authority, and a listen server's local pawn still
	// needs its team colours; run the OnRep by hand so both sides take the identical path.
	OnRep_Team();

	// PlayerStates replicate on a slow net update rate by default. Team is a one-shot, highly
	// visible change (it decides colours, spawn side and friendly fire), so push it immediately.
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Verbose, TEXT("PlayerState '%s' assigned to team %d"), *GetPlayerName(), static_cast<int32>(Team));
}

void ATracePlayerState::OnRep_Team()
{
	// Best-effort only: the PlayerState usually replicates before the pawn exists. ATraceCharacter
	// re-applies its colours from BeginPlay and OnRep_PlayerState as well, so whichever of the two
	// arrives last is the one that actually paints the character.
	APawn* OwnedPawn = GetPawn();
	if (OwnedPawn == nullptr)
	{
		// Covers the window where PawnPrivate has not been set yet but the owning controller is
		// already possessing (server and owning connection).
		if (const AController* OwningController = Cast<AController>(GetOwner()))
		{
			OwnedPawn = OwningController->GetPawn();
		}
	}

	if (ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(OwnedPawn))
	{
		TraceCharacter->ApplyTeamColors();
	}
}

// ---------------------------------------------------------------------------------------------
// Character selection (spec v14 §3)
// ---------------------------------------------------------------------------------------------

float ATracePlayerState::GetCharacterSelectTimeRemaining() const
{
	if (CharacterSelectDeadlineServerTime <= 0.f)
	{
		return 0.f;
	}

	const AGameStateBase* BaseGameState = (GetWorld() != nullptr) ? GetWorld()->GetGameState() : nullptr;
	if (BaseGameState == nullptr)
	{
		return 0.f;
	}

	const double Remaining = static_cast<double>(CharacterSelectDeadlineServerTime) - BaseGameState->GetServerWorldTimeSeconds();
	return FMath::Max(0.f, static_cast<float>(Remaining));
}

bool ATracePlayerState::ServerRequestCharacter_Validate(uint8 RequestedCharacter)
{
	// Cheap shape check only. Everything about WHETHER this player may have this character is a game
	// rule and belongs on the game mode; rejecting rules here would close the connection over an
	// ordinary race between two team-mates, which is a thing that legitimately happens.
	return RequestedCharacter <= TraceCharacterRoster::LastId;
}

void ATracePlayerState::ServerRequestCharacter_Implementation(uint8 RequestedCharacter)
{
	UWorld* const ThisWorld = GetWorld();
	ATraceGameMode* const Rules = (ThisWorld != nullptr) ? ThisWorld->GetAuthGameMode<ATraceGameMode>() : nullptr;

	if (Rules == nullptr)
	{
		// No authoritative game mode: this is not the server, or the mode is not ours. Say so rather
		// than dropping the request, so the client's screen does not sit there waiting forever.
		ClientCharacterPickResult(RequestedCharacter, ETraceCharacterPickResult::Disabled);
		return;
	}

	const ETraceCharacterPickResult Result = Rules->RequestCharacter(this, RequestedCharacter);

	// Sent on Granted as well as on every refusal. A client that only ever hears about failure has no
	// way to tell "accepted" from "the packet went missing", and would keep re-sending.
	ClientCharacterPickResult(RequestedCharacter, Result);
}

void ATracePlayerState::ClientCharacterPickResult_Implementation(uint8 RequestedCharacter, ETraceCharacterPickResult Result)
{
	// Nothing here changes state — SelectedCharacter arrives by replication, which is the only copy.
	// This is purely so the select screen can print a reason. It reads the values off the player
	// state, so they are stored rather than routed through a delegate the HUD would have to bind.
	LastPickResult = Result;
	LastPickResultCharacter = RequestedCharacter;
	LastPickResultLocalTime = (GetWorld() != nullptr) ? GetWorld()->GetTimeSeconds() : 0.f;

	UE_LOG(LogTraceGame, Log, TEXT("[CharSelect] Pick of %s answered: %d."),
		*TraceCharacterRoster::NameFor(RequestedCharacter), static_cast<int32>(Result));
}

uint8 ATracePlayerState::GetSelectedCharacter() const
{
	// One FindComponentByClass, no cache. A cache here would be a second copy of the very thing this
	// class deliberately does not store, and it would go stale on exactly the frame that matters —
	// the one where a team-mate's pick replicates in and the select screen has to grey a card out.
	if (const UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(this))
	{
		return static_cast<uint8>(Abilities->GetCharacterId());
	}

	return TraceCharacterRoster::NoneId;
}

bool ATracePlayerState::DoBotsYieldToHumans()
{
	return UTraceAbilityComponent::DoBotsYieldToHumans();
}

float ATracePlayerState::GetActivatedCooldownRemaining() const
{
	if (const UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(this))
	{
		return Abilities->GetActivatedCooldownRemaining();
	}

	return 0.f;
}

void ATracePlayerState::ServerMarkCharacterResolved(bool bLocked, bool bWasChosen)
{
	if (!HasAuthority())
	{
		return;
	}

	bCharacterLocked = bLocked;
	bCharacterWasChosen = bWasChosen;
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Log, TEXT("[CharSelect] '%s' (%s) is now %s%s."),
		*GetPlayerName(), *TraceTeamName(Team).ToString(),
		*TraceCharacterRoster::NameFor(GetSelectedCharacter()),
		bWasChosen ? TEXT("") : TEXT(" (auto-assigned)"));
}

void ATracePlayerState::ServerSetCharacterSelectOpen(bool bOpen, float DeadlineServerTime)
{
	if (!HasAuthority())
	{
		return;
	}

	if (bCharacterSelectOpen == bOpen
		&& FMath::IsNearlyEqual(CharacterSelectDeadlineServerTime, bOpen ? DeadlineServerTime : 0.f))
	{
		return;
	}

	bCharacterSelectOpen = bOpen;
	CharacterSelectDeadlineServerTime = bOpen ? DeadlineServerTime : 0.f;
	ForceNetUpdate();
}

void ATracePlayerState::CopyProperties(APlayerState* PlayerState)
{
	Super::CopyProperties(PlayerState);

	// Runs on the OLD state during seamless travel, with the NEW state as the argument.
	if (ATracePlayerState* TracePlayerState = Cast<ATracePlayerState>(PlayerState))
	{
		// Direct assignment rather than SetTeam(): the destination has no pawn and is not yet
		// replicating, so the OnRep/ForceNetUpdate work SetTeam does would be wasted.
		TracePlayerState->Team = Team;
		TracePlayerState->Kills = Kills;
		TracePlayerState->Deaths = Deaths;

		// bIsCarrier is deliberately NOT carried: the Core does not survive travel, and a stale
		// "carrier" flag would make the HUD claim someone is holding an object that no longer exists.
		//
		// RespawnEndServerTime is not carried either, and for a sharper reason: it is an absolute
		// timestamp on the OLD world's clock. Copying it across travel would hand the new match a
		// deadline in its own distant past or future and pin a death panel open.
		//
		// THE SELECT SESSION IS NOT CARRIED, and that is a decision rather than an omission. A travel
		// is a new match: it may be mode A (no characters at all), it may have the toggle off, and the
		// balance on the other side may put this player next to a team-mate holding the same pick —
		// which per-team uniqueness would have no chance to prevent, because CopyProperties runs
		// before anybody is on a team. Everyone re-picks, and ATraceGameMode::PollCharacterSelect
		// re-opens the screen on its own once teams exist.
		//
		// The CHARACTER is not this class's to carry in any case; it belongs to the ability component,
		// which is attached fresh in the destination world.
	}
}
