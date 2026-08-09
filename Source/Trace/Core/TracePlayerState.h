// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "UObject/ObjectMacros.h"

#include "Core/TraceCharacterRoster.h"  // TraceCharacterRoster::NoneId and the id space
#include "TraceTypes.h"                 // ETraceTeam

#include "TracePlayerState.generated.h"

/**
 * Why a character request was refused, so the client can say something true rather than "no".
 *
 * Spec v14 §3 [ASSUMPTION], verbatim: "first request wins, the loser is told and re-picks". Being
 * TOLD is the load-bearing half — a button that silently does nothing is indistinguishable from a
 * dropped packet, and the player's next move would be to press it again forever.
 */
UENUM()
enum class ETraceCharacterPickResult : uint8
{
	/** Locked in. The selection is now this player's for the rest of the match. */
	Granted = 0,
	/** A TEAM-MATE got there first. Enemies mirroring your pick is legal and never lands here. */
	TakenByTeammate,
	/** Not one of the five. A malformed or hostile RPC. */
	InvalidId,
	/** This player has already locked one in. Re-picking is not offered. */
	AlreadyLocked,
	/** Characters are switched off for this match (mode A, or the settings toggle). */
	Disabled,
	/** The select screen is not open for this player — nothing was being asked. */
	NotSelecting
};

/**
 * Per-player replicated state: team, K/D, whether this player currently holds the Core, and — new in
 * spec v14 — the state of this player's CHARACTER SELECT SESSION.
 *
 * Written on the server only (ATraceGameMode assigns the team and drives the selection; ATraceCore
 * maintains bIsCarrier), read everywhere. This is the one object that survives the pawn, which is why
 * the team lives here and not on ATraceCharacter — a dead player still belongs to a team, still shows
 * on the scoreboard, and still spawns on their own half.
 *
 * ---------------------------------------------------------------------------------------------
 * WHAT THIS CLASS DOES *NOT* STORE, AND WHERE THAT LIVES INSTEAD
 * ---------------------------------------------------------------------------------------------
 * THE CHARACTER ITSELF AND ITS COOLDOWN ARE NOT HERE. They live on UTraceAbilityComponent, which the
 * ability framework attaches to THIS ACTOR — the component is on the PlayerState, not on the pawn,
 * precisely so that spec v14 §5's cooldown rule ("continue to countdown while a player is dead...
 * a player can spawn with an ability timer still counting down") is true by construction rather than
 * by anybody remembering to preserve it.
 *
 * That makes a second copy here worse than useless: two replicated CharacterIds that can disagree is
 * exactly the drift this codebase has been bitten by before. So GetSelectedCharacter() and
 * GetActivatedCooldownRemaining() below are FORWARDERS, not storage. They exist because the select
 * screen and the HUD are in the UI slice and should not each have to know how to resolve a component;
 * they are one FindComponentByClass deep and answer the same value the framework answers.
 *
 * WHAT IS GENUINELY OURS is the SELECT SESSION: is this player being asked to pick, how long have
 * they got, did they choose or were they assigned, and what did the server say to their last request.
 * None of that is ability state — it is menu state with a server-authoritative lifetime — and the
 * ability framework has no notion of it.
 */
UCLASS()
class TRACE_API ATracePlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	ATracePlayerState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Team this player fights for. None until the GameMode balances them in during PostLogin. */
	UPROPERTY(ReplicatedUsing = OnRep_Team)
	ETraceTeam Team = ETraceTeam::None;

	UPROPERTY(Replicated)
	int32 Kills = 0;

	UPROPERTY(Replicated)
	int32 Deaths = 0;

	/**
	 * Mirror of ATraceCharacter::bIsCarrier, kept here so the scoreboard and HUD can show who has
	 * the Core without needing that player's pawn to be relevant. ATraceCore owns both writes.
	 */
	UPROPERTY(Replicated)
	bool bIsCarrier = false;

	/**
	 * Absolute server time (AGameStateBase::GetServerWorldTimeSeconds) at which this player's
	 * pending respawn fires. Zero means "not waiting to respawn".
	 *
	 * Exists because the respawn delay is no longer a constant a client can assume: it lives on the
	 * game mode (which only exists on the server), and the death panel used to count down from
	 * UTraceSettings::RespawnDelay — a number nothing enforces. Replicating the deadline instead of
	 * the duration also makes a late-arriving client immediately correct, exactly like
	 * MatchEndServerTime.
	 */
	UPROPERTY(Replicated)
	float RespawnEndServerTime = 0.f;

	/** Seconds until RespawnEndServerTime, clamped at zero. Zero when no respawn is pending. */
	float GetRespawnTimeRemaining() const;

	/** Server only. Assigns the team and runs the OnRep locally so a listen server updates too. */
	void SetTeam(ETraceTeam NewTeam);

	UFUNCTION()
	void OnRep_Team();

	// ------------------------------------------------------------------------------------------
	// Character selection (spec v14 §3)
	// ------------------------------------------------------------------------------------------

	/**
	 * True once the server has accepted a pick. THE LOCK-IN.
	 *
	 * Separate from "SelectedCharacter != 0" on purpose. The two differ for exactly one player: the
	 * one whose select screen timed out and who was auto-assigned. Both end up with a character, but
	 * only one of them chose it, and the select screen says so.
	 */
	UPROPERTY(Replicated)
	bool bCharacterLocked = false;

	/**
	 * True while this player must pick. The select screen draws if and only if this is true, so it is
	 * also the answer to "does mode A show a select screen?" — the game mode never sets it there.
	 */
	UPROPERTY(Replicated)
	bool bCharacterSelectOpen = false;

	/**
	 * Absolute server time at which an unanswered selection is auto-assigned. Zero = no deadline.
	 *
	 * Spec v14 §3 [ASSUMPTION]: "a timeout that auto-assigns a free character, so one idle player
	 * cannot stall the match". Replicated as a DEADLINE rather than a remaining duration for the same
	 * reason RespawnEndServerTime is: a client that joins late, or whose select screen is re-opened,
	 * is immediately correct instead of restarting somebody else's countdown.
	 */
	UPROPERTY(Replicated)
	float CharacterSelectDeadlineServerTime = 0.f;

	/**
	 * True when the player pressed the button, false when the timeout picked for them.
	 *
	 * Replicated rather than derived because only the server knows which happened, and the select
	 * screen's closing line is materially different: "LOCKED IN" is a decision, "ASSIGNED" is a
	 * notification the player should be able to see and complain about.
	 */
	UPROPERTY(Replicated)
	bool bCharacterWasChosen = false;

	/**
	 * The character this player holds, as ETraceCharacterId's underlying value. 0 = the Mannequin.
	 *
	 * FORWARDS to UTraceAbilityComponent::GetCharacterId() — there is no copy here. Returns 0 when
	 * the component has not been attached yet (early BeginPlay, a client before the component has
	 * replicated), which is the correct and safe answer: "no character".
	 *
	 * The uint8 rather than ETraceCharacterId is not a second numbering, it is the SAME number: the
	 * UI slice draws five cards from a plain-C++ table and must not take the reflected ability
	 * headers to do it. The static_asserts at the top of TraceCharacterRoster.cpp assert the two agree.
	 */
	uint8 GetSelectedCharacter() const;

	bool  HasCharacter() const { return TraceCharacterRoster::IsValidId(GetSelectedCharacter()); }
	bool  IsCharacterSelectOpen() const { return bCharacterSelectOpen; }
	bool  WasCharacterChosen() const { return bCharacterWasChosen; }

	// ---- Last verdict, for the select screen's message line -----------------------------------
	//
	// CLIENT-LOCAL AND NOT REPLICATED, on purpose. This is the answer to one RPC this player sent;
	// it is not state, it has no meaning to anybody else, and replicating it would put a refusal in
	// front of a player who never pressed anything. ClientCharacterPickResult writes it, the select
	// screen reads it and lets it age out.

	ETraceCharacterPickResult LastPickResult = ETraceCharacterPickResult::Granted;
	uint8 LastPickResultCharacter = TraceCharacterRoster::NoneId;

	/** UWorld::GetTimeSeconds on the client when the verdict landed. Far in the past = none yet. */
	float LastPickResultLocalTime = -1000.f;

	/** Seconds until the auto-assign fires, clamped at zero. Zero when no deadline is running. */
	float GetCharacterSelectTimeRemaining() const;

	/**
	 * Seconds left on this player's activated (E) ability, or 0 when it is ready.
	 *
	 * FORWARDS to UTraceAbilityComponent::GetActivatedCooldownRemaining(). Correct while DEAD, which
	 * is the whole reason the HUD asks a player state rather than a pawn — see spec v14 §5 and the
	 * cooldown contract at the top of Abilities/TraceAbilityComponent.h.
	 */
	float GetActivatedCooldownRemaining() const;

	/**
	 * THE CLIENT'S REQUEST. Server RPC, and it is legal on this class because AController::
	 * InitPlayerState makes the owning controller this actor's Owner — so the owning connection, and
	 * only the owning connection, may call it.
	 *
	 * It does no arbitration itself. It forwards to ATraceGameMode::RequestCharacter, which is the
	 * single authority on per-team uniqueness, and reports the verdict back through
	 * ClientCharacterPickResult. Two team-mates pressing the same key on the same frame arrive as two
	 * ordinary server function calls executed one after the other on one thread: the first is
	 * Granted, the second is TakenByTeammate. That is what "first request wins" means here, and it
	 * needs no tie-break code because there is no tie to break.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestCharacter(uint8 RequestedCharacter);

	/** The verdict, back to the requesting client only. Granted is sent too — silence is not an answer. */
	UFUNCTION(Client, Reliable)
	void ClientCharacterPickResult(uint8 RequestedCharacter, ETraceCharacterPickResult Result);

	/**
	 * Server only. Records the OUTCOME of a selection on this player's session.
	 *
	 * Does NOT set the character — UTraceAbilityComponent::ServerSetCharacter does that, and is the
	 * only thing allowed to. This closes the session around it: the lock, and whether the player
	 * chose or was assigned.
	 *
	 * @param bWasChosen false when the game mode auto-assigned on the timeout, which the select
	 *                   screen prints differently ("ASSIGNED" rather than "LOCKED IN").
	 */
	void ServerMarkCharacterResolved(bool bLocked, bool bWasChosen);

	/** Server only. Opens or closes the select screen and sets/clears the auto-assign deadline. */
	void ServerSetCharacterSelectOpen(bool bOpen, float DeadlineServerTime);

	bool IsActivatedAbilityReady() const { return GetActivatedCooldownRemaining() <= 0.f; }

	/**
	 * Carries Team/Kills/Deaths across seamless travel. Called on the OLD player state with the NEW
	 * one as the argument, so the copy goes outwards into @p PlayerState.
	 */
	virtual void CopyProperties(APlayerState* PlayerState) override;
};
