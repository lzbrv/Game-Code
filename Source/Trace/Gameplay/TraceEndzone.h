// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"      // FHitResult (overlap delegate signature)
#include "GameFramework/Actor.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"              // ETraceTeam, TraceOpposingTeam

#include "TraceEndzone.generated.h"

class ATraceCharacter;
class UBoxComponent;
class UPrimitiveComponent;

/**
 * A scoring volume at one end of the field.
 *
 * SCORING DIRECTION - read this before touching anything below.
 * OwningTeam is the team that *defends* this zone. You score in your OPPONENT's zone, so a zone
 * with OwningTeam == Blue is scored in by an ORANGE carrier, and vice versa. The whole rule is
 * one line in ScoresHere(): Team == TraceOpposingTeam(OwningTeam). Getting this backwards
 * produces a game that looks like it works (players score) but is exactly inverted, which is
 * hard to spot in a playtest - hence the explicit helper and this comment.
 *
 * The volume is invisible: ATraceArenaBuilder draws the team-tinted floor patch that marks it,
 * and spawns these triggers on the server only. Everything here is server-authoritative; on any
 * non-authoritative net mode the trigger disables its own collision and tick, so a level-placed
 * endzone can never make a client believe it scored.
 */
UCLASS()
class TRACE_API ATraceEndzone : public AActor
{
	GENERATED_BODY()

public:
	ATraceEndzone();

	//~ Contract surface (spec section 7)

	/** The team that DEFENDS this zone. Their opponent scores by carrying the Core in here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace")
	ETraceTeam OwningTeam = ETraceTeam::None;

	//~ End contract surface

	/** Overlap volume. Root component; overlaps pawns only, blocks nothing. */
	UPROPERTY(VisibleAnywhere, Category = "Trace")
	TObjectPtr<UBoxComponent> Trigger;

	//~ AActor
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	//~ End AActor

	/**
	 * Sets the owning team and the box half-extent in one call. Used by ATraceArenaBuilder, which
	 * spawns these deferred so OwningTeam is already correct by the time BeginPlay runs.
	 */
	void ConfigureZone(ETraceTeam InOwningTeam, const FVector& BoxHalfExtent);

	/** True when a carrier on @p Team scores by entering this zone. See the class comment. */
	bool ScoresHere(ETraceTeam Team) const;

protected:
	UFUNCTION()
	void OnTriggerBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	/**
	 * Server only. Validates that Character really is the Core carrier, is alive, and is on the
	 * team that scores here, then notifies the GameMode.
	 */
	void TryScore(ATraceCharacter* Character);

	/** Authoritative "is this the carrier": asks the GameMode's Core, not just the replicated flag. */
	bool IsCoreCarrier(const ATraceCharacter* Character) const;

	/**
	 * Pure point-in-box test against the trigger volume, in the trigger's own space.
	 *
	 * Used by the tick poll instead of GetOverlappingActors so that scoring never depends on the
	 * pawn's collision settings or on overlap events being enabled on both primitives - the two
	 * classic ways a trigger silently stops firing.
	 */
	bool IsInsideZone(const FVector& WorldLocation) const;

	/**
	 * Seconds of deafness after a score. NotifyScored resets the Core and teleports every pawn, but
	 * that happens over the following frames - without this, the still-overlapping carrier would be
	 * counted again by the very next poll.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace")
	float ScoreCooldown = 1.f;

private:
	/**
	 * World time of the last accepted score, for the ScoreCooldown debounce. Seeded far in the
	 * past (world time starts at 0) so the first carry through is never swallowed. A literal is
	 * used rather than BIG_NUMBER/MAX_FLT because those macros were renamed to UE_-prefixed forms
	 * during the 5.x line and the old spellings warn on newer engines.
	 */
	float LastScoreTime = -10000.f;
};
