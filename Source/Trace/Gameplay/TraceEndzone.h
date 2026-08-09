// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"      // FHitResult (overlap delegate signature)
#include "GameFramework/Actor.h"
#include "Math/Box.h"                // FBox
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"              // ETraceTeam, TraceOpposingTeam

#include "TraceEndzone.generated.h"

class ATraceCharacter;
class UBoxComponent;
class UPrimitiveComponent;

/**
 * A scoring volume at one end of the field. ONE class, TWO shapes - see the mode note below.
 *
 * SHAPE IN MODE A (ETraceScoringMode::EndzoneStatusCore, the shipped game). Goal line to end wall along X,
 * floor to wall top in Z, and the ENTIRE WIDTH of the field along Y - sideline to sideline, like a
 * real football endzone. ATraceArenaBuilder sizes it from EndzoneHalfWidth() and paints the floor
 * patch, the goal line and the endzone gate from the same number, so the rectangle you can see is
 * the rectangle that scores. If you ever narrow one of them, narrow all of them: a carrier who
 * crosses the line by a sideline and does not score reads as a broken trigger, not as a design
 * decision.
 *
 * SHAPE IN MODE B (ETraceScoringMode::ThrownCoreAndGoals, spec v4 §7, resized by spec v5 §4). A GOAL: the same
 * depth along X, but only ATraceArenaBuilder::GoalHalfWidth() either side of the centre line (1000 uu
 * by default, i.e. a 2000 uu mouth in a 9600 uu field) and only ClampedGoalHeight() tall (440 uu), so
 * it reads as a goal rather than as a wall. Both numbers shrank in v5 - "for game mode b ONLY ...
 * decrease the size of the goal (reduce height and width)" - by the same factor, so the mouth kept
 * its proportions. Same rule as mode A: the builder draws the posts, the crossbar and the mouth
 * patch from the same two functions that size this box.
 *
 * WHY ONE CLASS AND NOT TWO. Everything below - the team check, the carrier check, the debounce, the
 * geometric poll - is identical for both shapes, because "the carrier got inside the volume" is the
 * same question in both modes. Only the box extent and the trim differ, and both arrive through
 * ConfigureZone. bGoalVolume exists so that a caller can TELL the two apart (and so the log says
 * which it is); it is never used to decide which game is being played. That question has exactly one
 * legal answer, ATraceGameState::GetScoringMode() - see the note at the top of TraceMatchTypes.h.
 *
 * BOTH SETS EXIST AT ONCE. The builder spawns the endzone pair AND the goal pair and then activates
 * one of them (SetZoneActive), so the A/B toggle is a flag flip rather than a rebuild and no restart
 * is needed. An inactive zone has no collision and does not tick: it cannot score, and it cannot be
 * found by an overlap query either.
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
	 * Sets the owning team, the box half-extent and the shape in one call. Used by
	 * ATraceArenaBuilder, which spawns these deferred so OwningTeam is already correct by the time
	 * BeginPlay runs.
	 *
	 * @param bInGoalVolume true for the mode-B goal, false for the mode-A endzone. Shape only - see
	 *                      the class comment on why this must never be read as "which mode is live".
	 */
	void ConfigureZone(ETraceTeam InOwningTeam, const FVector& BoxHalfExtent, bool bInGoalVolume = false);

	// =============================================================================================
	// SPEC v6 §4.3 — THE CIRCULAR GOAL. MODE B ONLY.
	//
	// "Raise the goals 1.5x player height from the ground, place them into the back walls, and make
	// them circular."
	//
	// The volume stays a BOX component, because a box is what UBoxComponent can be and because the
	// box is still the right broad phase — it is the ring's bounding slab: half a ring diameter in Y
	// and Z, and the slab's depth in X. What the ring adds is one extra test inside it, applied by
	// IsInsideZone() and by ATraceCore's swept goal test alike, so the shape that scores and the
	// shape the arena draws are still the same shape.
	//
	// WHY THE RING TEST LIVES HERE AND NOT IN ATraceCore. The class comment above makes the promise
	// that "the volume that scores is the volume you can see", and the way that promise is kept is
	// that nobody reconstructs the shape: the arena sizes it, this actor stores it, and every reader
	// asks. A radius kept privately by the Core would be a second copy of the goal, which is exactly
	// how the drawn goal and the scoring goal drifted apart the last time somebody tried it.
	// =============================================================================================

	/**
	 * Makes this goal CIRCULAR: the mouth is the disc of radius @p Radius about the trigger's own
	 * origin, in the trigger's local Y/Z plane, and the box's X extent becomes the slab depth.
	 *
	 * @p Radius <= 0 restores the plain box, which is what mode A's endzones stay as forever.
	 * Call after ConfigureZone (it does not resize the box - the caller owns that, because the box
	 * has to be the ring's bounding slab and only the caller knows the depth it wants).
	 */
	void ConfigureRing(float Radius);

	/** True when this volume scores on a DISC rather than on the whole box. Mode B goals only. */
	bool IsRingGoal() const { return bRingGoal && RingRadius > 0.f; }

	/** Radius of the ring mouth, uu. 0 when this is not a ring. */
	float GetRingRadius() const { return RingRadius; }

	/** World-space centre of the ring mouth: the trigger's own origin. */
	FVector GetRingCentre() const;

	/** World-space unit normal of the ring plane (the trigger's local +X, i.e. down the field). */
	FVector GetRingAxis() const;

	/** True when a carrier on @p Team scores by entering this zone. See the class comment. */
	bool ScoresHere(ETraceTeam Team) const;

	//~ Mode A / mode B surface (spec v4 §7) - the contract the scoring slice reads.

	/** True if this is the narrow mode-B GOAL rather than the full-width mode-A endzone. */
	bool IsGoalVolume() const { return bGoalVolume; }

	/**
	 * Arms or disarms the volume. An inactive zone drops its collision, stops ticking and refuses to
	 * score, so the pair belonging to the other mode is inert rather than merely quiet.
	 *
	 * Idempotent, and safe to call at any time on the server. On a client it only bookkeeps the flag
	 * (the collision is already off there - see BeginPlay).
	 */
	void SetZoneActive(bool bInActive);

	/** True while this volume can score. See SetZoneActive. */
	bool IsZoneActive() const { return bZoneActive; }

	/**
	 * Point-in-box against the trigger volume, in the trigger's own space.
	 *
	 * PUBLIC ON PURPOSE. Mode B scores a THROWN Core as well as a carried one, and the code that owns
	 * the throw must be able to ask "is the Core inside the goal" without rebuilding the box from
	 * width and depth constants and getting it subtly wrong - which is precisely how the visible goal
	 * and the scoring goal drift apart. This is the same test the tick poll uses, so the answer
	 * cannot disagree with the answer that awards points.
	 *
	 * Deliberately geometric rather than overlap-based, so it never depends on the other actor's
	 * collision settings or on overlap events being enabled on both primitives - the two classic ways
	 * a trigger silently stops firing. Returns false on an INACTIVE zone: the volume belonging to the
	 * mode that is not being played contains nothing.
	 */
	bool IsInsideZone(const FVector& WorldLocation) const;

	/** World-space box of this volume, for debug draw, bot targeting and throw aiming. */
	FBox GetZoneBounds() const;

	//~ End mode surface

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

	// =============================================================================================
	// WHY THE SHAPE FIELDS BELOW ARE UPROPERTYs (spec v15 §1)
	//
	// They used to be plain C++ members, which was correct while the ONLY way an endzone came into
	// existence was ATraceArenaBuilder spawning one and calling ConfigureZone/ConfigureRing in the
	// same breath. Spec v15 §1 bakes the arena into a real .umap, so an endzone can now be PLACED -
	// saved to disk and loaded again with nobody to reconfigure it. A plain member does not survive
	// that round trip, and the failure mode is the ugly kind: the actor loads with bZoneActive true
	// and RingRadius 0, i.e. a mode-B goal silently degraded to its own bounding slab - "an invisible
	// one that scores off the corners", exactly what the ring comment below warns about.
	//
	// Serialised, so a placed endzone keeps its shape. bRingGoal is serialised WITH RingRadius even
	// though only the radius was asked for: IsRingGoal() reads both, and a ring whose radius survived
	// but whose flag did not is a goal that looks right in the details panel and scores as a box.
	// EditAnywhere rather than VisibleAnywhere because §1's whole point is a level a human can edit -
	// but note that typing a new RingRadius does NOT resize the trigger box (ConfigureRing's comment
	// says why the caller owns the extent), so hand-editing the radius wants the box adjusted too.
	// =============================================================================================

	/** Shape flag: the narrow mode-B goal rather than the full-width mode-A endzone. */
	UPROPERTY(EditAnywhere, Category = "Trace")
	bool bGoalVolume = false;

	/**
	 * Spec v6 §4.3. Circular mouth, mode B only.
	 *
	 * Two fields rather than "radius > 0 means circular" alone, so that a zero radius arriving from a
	 * degenerate settings value reads as a BROKEN ring in the log rather than silently reverting the
	 * goal to a full box - which would be a goal the size of its own bounding slab, i.e. an invisible
	 * one that scores off the corners.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace")
	bool bRingGoal = false;

	UPROPERTY(EditAnywhere, Category = "Trace")
	float RingRadius = 0.f;

	/**
	 * Whether this volume can score right now. Both pairs are built; one pair is armed.
	 *
	 * Defaults TRUE so that a zone nobody ever calls SetZoneActive on behaves exactly as it did
	 * before mode B existed - a level-placed endzone, or any future caller that spawns one directly,
	 * must not be silently dead.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace")
	bool bZoneActive = true;
};
