// Trace — THE PRACTICE RANGE (spec v19 §2).
//
// Verbatim: "Include a practice range, with stationary enemy bots that take damage. Make sure they
// respawn in place a short time after dying. This doesn't need to be a full map, just a space to
// test abilities, movement, and gunplay." Plus: "Include an option to pick up and drop the core,
// within the testing range. This means that there is a spot where you can leave the core without it
// turning over. Again, this is ONLY within the testing range." Plus: "Include a toggle within the
// range for infinite abilities (no cooldowns) and allow players to switch characters without
// leaving the range."
//
// WHICH SPACE: the ARENA, unchanged. No new map, no new geometry, no edit to ATraceArenaBuilder.
// The range is a GAME MODE (ATracePracticeGameMode) that opens the same /Game/Maps/Arena_Baked
// everybody plays on and furnishes it. Three reasons, in order of how much they cost to get wrong:
//   1. spec v19 §4.1 kills anything outside ATraceArenaBuilder::GetFieldBounds(). A range carved out
//      beside the arena would be a room that executes you for standing in it, and the phase-1 report
//      on §4.1 says so explicitly.
//   2. movement is most of what you would come here to practise, and the walls, ramps and block tops
//      you want to wall-jump and mantle on are all already here.
//   3. a second .umap is an asset this module would have to author and keep in step with the baked
//      arena's ~1291 wall boxes forever.
//
// ===================================================================================================
// *** THE RANGE MUST NOT LEAK, AND THIS IS HOW THE CODE MAKES THAT STRUCTURAL ***
// ===================================================================================================
//
// There is exactly ONE predicate — TracePracticeRange::IsActive() — and every range-only behaviour
// is downstream of it:
//
//     infinite abilities .....  UTracePracticeRangeSubsystem::PollRange, behind IsActive()
//     character switching ....  UTracePracticeRangeSubsystem::PollRange, behind IsActive()
//     the no-turnover spot ...  the CoreRack pad, only ever spawned behind IsActive()
//     the dummies ............  only ever spawned behind IsActive()
//
// IsActive() answers "is the authoritative game mode an ATracePracticeGameMode?". In a real match it
// is not, and it cannot be made so by a setting, a cvar or an .ini: the mode arrives on the travel
// URL. So the cheats are not "switched off" in a real match, they have no driver at all.
//
// THE MACHINERY LIVES IN A WORLD SUBSYSTEM RATHER THAN ON THE GAME MODE, AND THAT IS THE RED ARM.
// A subsystem exists in every world, including a real match's, and does nothing there because
// IsActive() says no. That is what makes Trace.Practice.LeakArm a genuine red arm: setting it makes
// IsActive() say YES inside a real match, the subsystem wakes up, the cheats really do apply, and
// Trace.Practice.LeakTest — the same command, unchanged — reports FAIL on all four. Had the cheats
// lived on the game mode, the arm could not have turned them on and a leak test that "passed"
// because the code was not loaded would have been exactly the manufactured evidence this project
// has already been burned by twice.

#pragma once

#include "CoreMinimal.h"

#include "Engine/EngineTypes.h"                 // FTimerHandle
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "TracePracticeRange.generated.h"

class AActor;
class AController;
class APlayerState;
class ATraceCharacter;
class ATraceCore;
class ATracePracticeDummyController;
class ATracePracticePad;
class ATracePracticePost;
class UWorld;

enum class ETracePracticePadRole : uint8;

/**
 * THE GATE. Everything range-only is downstream of IsActive(), and nothing else may answer the
 * question — a second opinion about "am I in the range?" is how a cheat escapes.
 */
namespace TracePracticeRange
{
	/**
	 * True only when the authoritative game mode for @p WorldPtr is an ATracePracticeGameMode.
	 *
	 * SERVER-SIDED BY CONSTRUCTION. AGameModeBase exists only on the server, so a remote client
	 * always gets false — which is the safe direction and matches spec v19 §2's stated assumption
	 * that the range is single-player / local. A listen-server host IS the server, so the host of a
	 * range session gets true and everything works for them.
	 */
	TRACE_API bool IsActive(const UWorld* WorldPtr);

#if !UE_BUILD_SHIPPING
	/**
	 * THE RED ARM (Trace.Practice.LeakArm 1). Forces IsActive() true everywhere, so the range's
	 * cheats really do apply inside a real match and Trace.Practice.LeakTest can be shown to FAIL.
	 *
	 * Absent from shipping builds entirely. Never set by any code path — only by the console.
	 */
	TRACE_API bool IsLeakArmed();
#endif
}

/**
 * Builds, drives and tears down the practice range.
 *
 * Exists in every game world; INERT in every world that is not a range (one game-mode cast every
 * 200 ms, and nothing else). See the header block for why the machinery is here rather than on the
 * game mode.
 */
UCLASS()
class TRACE_API UTracePracticeRangeSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem / UWorldSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	//~ End USubsystem / UWorldSubsystem

	/** The subsystem for @p WorldPtr, or null. Null-safe on the world. */
	static UTracePracticeRangeSubsystem* Get(const UWorld* WorldPtr);

	// ---------------------------------------------------------------------------------------------
	// THE THREE RANGE-ONLY AFFORDANCES
	// ---------------------------------------------------------------------------------------------

	/**
	 * The infinite-abilities toggle (spec v19 §2). OFF whenever the range is built, never latched
	 * across sessions.
	 *
	 * @return the state it ended in. Refuses and returns false outside the range.
	 */
	bool SetInfiniteAbilities(bool bEnabled);
	bool IsInfiniteAbilitiesOn() const { return bInfiniteAbilities; }

	/**
	 * Reopens the character select screen for @p Player without them leaving the range.
	 *
	 * Does it by clearing their character and unlocking them, then letting ATraceGameMode::
	 * PollCharacterSelect open the screen on its own next pass — the shipped screen, the shipped
	 * uniqueness rule, the shipped grant. There is deliberately no second selection path here: the
	 * one thing a practice range must not do is teach you a character flow the match does not have.
	 *
	 * @return false outside the range, or when the player already has a screen open.
	 */
	bool ReopenCharacterSelect(APlayerState* Player);

	/** A player stepped on a pad. Called by ATracePracticePad; refuses outside the range. */
	void HandlePadTouched(ATracePracticePad* Pad, ATraceCharacter* Toucher);

	/**
	 * The spot @p ForController's dummy stands on, or null when it is not a range dummy.
	 * ATracePracticeGameMode::ChoosePlayerStart is the only caller — see ATracePracticePost.
	 */
	AActor* FindRespawnPostFor(const AController* ForController) const;

	// ---------------------------------------------------------------------------------------------
	// Introspection, for the console commands and the harnesses
	// ---------------------------------------------------------------------------------------------

	bool IsBuilt() const { return bBuilt; }
	int32 GetDummyCount() const;
	int32 GetPadCount() const;

	/** True while the Core is parked on the rack rather than in play. */
	bool IsCoreOnRack() const { return bCoreOnRack; }

	/** One line per fact, to the log. Behind Trace.Practice.Status. */
	void LogStatus() const;

	/** Seconds between polls. Also the granularity of the infinite-abilities refresh. */
	static constexpr float PollIntervalSeconds = 0.2f;

	/**
	 * How often the parked Core is re-parked.
	 *
	 * ATraceCore forces a kickoff after TraceCoreTuning::OutOfPlayRecoverySeconds (15 s) of a
	 * holderless Core during a live half — a promise that the Core never goes missing, and a
	 * completely correct one for a real match. Re-asserting the park well inside that window is how
	 * the range keeps its "leave it here" state without reaching into ATraceCore, which this pass
	 * does not own and must not fork.
	 */
	static constexpr float CoreReparkSeconds = 7.f;

private:
	void PollRange();

	/** Spawns the dummies, their posts and the three pads. Idempotent. */
	void BuildRange();

	/** Destroys everything BuildRange spawned and clears the toggle. Idempotent. */
	void TearDownRange();

	/** Places one dummy on the floor at @p DesiredXY, with a post under it. Null on failure. */
	ATracePracticeDummyController* SpawnDummy(const FVector& DesiredXY);

	/** Places one pad on the floor under @p DesiredPoint. Null on failure. */
	ATracePracticePad* SpawnPad(ETracePracticePadRole InRole, const FVector& DesiredPoint, const FString& InLabel);

	/** Drops @p Point onto whatever is beneath it. False when there is nothing to stand on. */
	bool TraceToFloor(const FVector& Point, FVector& OutFloorPoint) const;

	/** Zeroes every human's activated cooldown and refills their dash pool. */
	void ApplyInfiniteAbilities();

	/**
	 * Holds the racked Core there.
	 *
	 * Two jobs, and the second one was a measured failure rather than a precaution: it re-asserts the
	 * park before ATraceCore's 15 s out-of-play recovery can claim the Core, AND it takes the Core
	 * back off anyone a kickoff hands it to. Stepping on the pad is the only legitimate way off the
	 * rack, and that path clears bCoreOnRack first — see the body.
	 */
	void KeepCoreParked();

	/** The range keeps no score, so no capture run can end the session on the mercy rule. */
	void SuppressScore();

	/** The Core rack pad was stepped on: deposit if carrying, collect if it is racked. */
	void HandleCoreRack(ATraceCharacter* Toucher);

	/** Repaints the infinite-abilities pad to match bInfiniteAbilities. */
	void RefreshInfiniteAbilitiesPad();

	ATraceCore* GetCore() const;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<ATracePracticeDummyController>> Dummies;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<ATracePracticePost>> Posts;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<ATracePracticePad>> Pads;

	FTimerHandle PollHandle;

	/** True once BuildRange has run for the current activation. Cleared by TearDownRange. */
	bool bBuilt = false;

	/** The spec's toggle. Deliberately not config and not saved: a range opens with it OFF. */
	bool bInfiniteAbilities = false;

	/** True while the Core is deliberately parked on the rack. See KeepCoreParked. */
	bool bCoreOnRack = false;

	/** World time the park was last re-asserted. */
	float LastCoreParkWorldTime = -1000.f;
};
