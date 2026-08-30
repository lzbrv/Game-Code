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
//     the owner's arms rig ...  ATraceCharacter::BuildOwnerArmsViewModel, behind
//                               ShouldUseOwnerArmsViewModel() — which is IsActive() and a cvar
//                               (demo 29 item 2; dev builds only, absent from Shipping entirely)
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

	/**
	 * *** SPEC v24 §5's RED ARM (Trace.Practice.PollOnlyInfinite 1). ***
	 *
	 * Restores the pre-v24 driver for the infinite-abilities toggle EXACTLY: the cheat is re-applied
	 * only by the range's 5 Hz furniture poll and never by the per-frame tick. That is the build the
	 * owner reported as "the toggle doesn't do anything", and it is what lets the BEFORE and AFTER
	 * numbers in Trace.Practice.InfiniteVerify come out of ONE binary minutes apart.
	 *
	 * Absent from shipping builds. Never set by any code path — only by the console.
	 */
	TRACE_API bool IsInfinitePollOnlyArmed();

	/**
	 * *** DEMO 29 ITEM 2 — THE OWNER'S FIRST-PERSON ARMS RIG, AND THE ONE QUESTION THAT DECIDES IT. ***
	 *
	 * Verbatim: "I need to test this first person arms rig i created. ... Implement this only in the
	 * practice range, for testing purposes." So the rig is a TEST FIXTURE, and this is the predicate
	 * that keeps it one. `SK_TraceArms` replaces the drawn pack hands (`SK_TraceHands`) in the
	 * first-person view when — and only when — this answers true.
	 *
	 * TWO TERMS, AND THEY DO DIFFERENT JOBS:
	 *
	 *   IsActive()  is the RANGE gate, unchanged and unfakeable in a shipped build: the practice game
	 *               mode can only arrive on the travel URL. This is the term that makes a normal match
	 *               structurally incapable of drawing the rig.
	 *   the cvar    is the A/B knob (`Trace.Practice.ArmsRig`, default 1 = the new rig). It exists so
	 *               the owner can put the shipped pack hands back with one console command and compare
	 *               them live, in one session, without a rebuild. It can only ever turn the fixture
	 *               OFF inside the range; on its own it turns nothing on anywhere.
	 *
	 * THE SEAM ASKS TWO QUESTIONS, IN TWO PLACES, AND THEY ARE NOT THE SAME QUESTION:
	 *   * WHETHER THE FIXTURE EXISTS — asked once, at rig build time, of IsActive() ALONE. The arms
	 *     component is constructed in the range whatever the knob says, so the knob can be flipped
	 *     BOTH ways in one session; a switch that can only be turned off is not an A/B. In every
	 *     world that is not the range there is no component, so the pawn's per-frame path degenerates
	 *     to one null pointer compare and a match cannot be reached from here at all.
	 *   * WHICH RIG IS DRAWN — asked per frame, of THIS function, but only on a pawn that already has
	 *     an arms component (i.e. only inside the range). That is what makes the knob live.
	 *
	 * ABSENT FROM SHIPPING ENTIRELY, along with the cvar and every line of the viewmodel seam that
	 * calls it: a test fixture for an unfinished art asset has no business in a shipped binary, and
	 * "compiled in but never true" is a weaker claim than "not compiled in".
	 */
	TRACE_API bool ShouldUseOwnerArmsViewModel(const UWorld* WorldPtr);
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
class TRACE_API UTracePracticeRangeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem / UWorldSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	//~ End USubsystem / UWorldSubsystem

	// ---------------------------------------------------------------------------------------------
	// *** SPEC v24 §5 — WHY THIS CLASS TICKS, AND WHY THE 5 Hz POLL WAS THE BUG ***
	// ---------------------------------------------------------------------------------------------
	//
	// The infinite-abilities toggle WAS read — PollRange consulted bInfiniteAbilities and called
	// ApplyInfiniteAbilities() — and it was read on the right side (the subsystem only exists
	// authoritatively, and UTraceAbilityComponent::DebugSetActivatedCooldown refuses without
	// authority). It was the THIRD case: read, applied, and then immediately overwritten by the
	// ordinary cooldown path.
	//
	// UTraceAbilityComponent::TryActivate() writes ActivatedCooldownEndMatchTime the instant the
	// ability fires, and refuses on it from that instant. The toggle's only enforcement was a sweep
	// on a 200 ms timer, so EVERY press was followed by up to 200 ms in which the ability was on a
	// full cooldown and TryActivate() said no — the cooldown genuinely started, the HUD ring
	// genuinely lit, and the toggle was worth at most five activations a second. "No cooldowns" was
	// never true for a single frame that anybody looked at.
	//
	// A tick, not a faster timer, and the ORDER is the whole reason it works:
	// UWorld::Tick runs TG_PrePhysics (where APlayerController delivers the E press and TryActivate
	// writes the deadline) at LevelTick.cpp:1750, and FTickableGameObject::TickObjects at :1821 —
	// AFTER it, in the SAME frame. So the deadline this class erases is the one written by the press
	// that just happened, and nothing downstream of the press (TG_PostUpdateWork, the HUD draw, the
	// next frame's input) ever observes a live cooldown. That is what "immediately reusable" means
	// here, and it is measured rather than asserted — see Trace.Practice.InfiniteVerify.
	//
	// COSTS NOTHING WHERE IT IS NOT WANTED. IsTickable() is bInfiniteAbilities, which is false in
	// every world that is not a range with the toggle on — SetInfiniteAbilities refuses outside the
	// gate, BuildRange opens with it off, and TearDownRange clears it.
	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject

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

	/**
	 * THE RANGE'S OWN SPAWN LINE (demo 19 item 1, "make it smaller"), or null before it is built.
	 *
	 * ATracePracticeGameMode::ChoosePlayerStart hands this back for a HUMAN, which is what stops a
	 * death in the range depositing the player 15600 uu away in the endzone the shipped pipeline
	 * quite correctly uses for a real match. The post's ROTATION matters as much as its location:
	 * AGameModeBase spawns the pawn facing it, so it is what makes you open the range already looking
	 * down the firing line.
	 */
	AActor* GetPlayerStartPost() const;

	// ---------------------------------------------------------------------------------------------
	// Introspection, for the console commands and the harnesses
	// ---------------------------------------------------------------------------------------------

	bool IsBuilt() const { return bBuilt; }
	int32 GetDummyCount() const;
	int32 GetPadCount() const;

	/**
	 * How many times the infinite-abilities cheat has been re-applied, and by which driver.
	 *
	 * Exists so Trace.Practice.InfiniteVerify can tell "the toggle is on and the tick is running" from
	 * "the toggle is on and only the 5 Hz poll is running" — which is the difference between the two
	 * arms, and the difference the reported bug is made of.
	 */
	int32 GetInfiniteApplyCount() const { return InfiniteApplyCount; }
	int32 GetInfiniteTickApplyCount() const { return InfiniteTickApplyCount; }

	/**
	 * The firing distance the CURRENT furniture was built with, in uu.
	 *
	 * A RESOLVED number rather than a constant since v24 §0 — see TargetStandbackFor() in the .cpp.
	 * Zero before BuildRange has run.
	 */
	float GetBuiltTargetStandbackUU() const { return BuiltTargetStandbackUU; }

	/** True while the Core is parked on the rack rather than in play. */
	bool IsCoreOnRack() const { return bCoreOnRack; }

	/** One line per fact, to the log. Behind Trace.Practice.Status. */
	void LogStatus() const;

	/**
	 * Seconds between polls of the range's FURNITURE — building it, gathering the player onto the
	 * spawn line, holding the Core on the rack, wiping the scoreboard.
	 *
	 * *** IT IS NO LONGER THE GRANULARITY OF THE INFINITE-ABILITIES REFRESH, AND THAT SENTENCE WAS
	 * THE BUG (spec v24 §5). *** That refresh is now driven by Tick(), every frame. See the block
	 * above the FTickableGameObject overrides. The poll still applies the cheat as well, so the two
	 * agree and so the red arm has something to fall back to.
	 */
	static constexpr float PollIntervalSeconds = 0.2f;

	/**
	 * How often the parked Core is re-parked.
	 *
	 * ATraceCore forces a kickoff after TraceCoreTuning::OutOfPlayRecoverySeconds (15 s) of a
	 * holderless Core during a live half — a promise that the Core never goes missing, and a
	 * completely correct one for a real match. Re-asserting the park well inside that window is how
	 * the range keeps its "leave it here" state without reaching into ATraceCore, which this pass
	 * does not own and must not fork.
	 *
	 * *** SPEC v24 §0: THIS IS AN ABSOLUTE THAT WANTS TO BE RELATIVE, AND IT COULD NOT BE FIXED HERE.
	 * ***
	 * 7 s is not a tuning value, it is "comfortably less than half of 15", i.e. a RATIO to
	 * TraceCoreTuning::OutOfPlayRecoverySeconds. Halve that base to 5 s tomorrow and this silently
	 * becomes a re-park that arrives two seconds after the Core has already been taken away, which is
	 * exactly the failure §0 exists to prevent. It cannot be expressed as the ratio it is because the
	 * base is a .cpp-local constant inside Gameplay/TraceCore.cpp, invisible from here and in a file
	 * this pass does not own. THE ONE-LINE FIX FOR WHOEVER OWNS ATraceCore: promote
	 * TraceCoreTuning::OutOfPlayRecoverySeconds into TraceCore.h, and this becomes
	 * `OutOfPlayRecoverySeconds * 0.47f` and tracks it for ever. Called out rather than quietly left.
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

	/**
	 * Places the HUMAN's spawn line post on the floor under @p DesiredPoint (demo 19 item 1).
	 *
	 * Spawned facing +X, which is where the target row is, because AGameModeBase spawns a pawn on the
	 * start spot's rotation as well as its location. Null on failure.
	 */
	ATracePracticePost* SpawnPlayerStartPost(const FVector& DesiredPoint);

	/**
	 * Moves a human who is already standing somewhere else onto the range's spawn line (demo 19
	 * item 1). ONE-SHOT PER PLAYER — see the body for why a repeat would be wrong.
	 *
	 * Needed because the shipped endzone spawn has already happened by the time the 5 Hz poll
	 * furnishes the range: ChoosePlayerStart fixes every LATER life, this fixes the first one.
	 */
	void GatherPlayersIntoRange();

	/**
	 * Planar distance from the local player to the nearest target, or -1 when there is no pawn.
	 * This is the number demo 19 item 1 is a claim about, so LogStatus prints it.
	 */
	double GetPlayerDistanceToTargets() const;

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

	/**
	 * The human's spawn line (demo 19 item 1). Also lives in Posts, which is what destroys it.
	 * Weak rather than owning for the same reason every other handle here is: the range's actors are
	 * ordinary world actors and the world outlives none of them.
	 */
	TWeakObjectPtr<ATracePracticePost> PlayerStartPost;

	/**
	 * Players GatherPlayersIntoRange has already moved once. Weak keys so a player who leaves does
	 * not keep a PlayerState alive; cleared with everything else by TearDownRange.
	 */
	TSet<TWeakObjectPtr<APlayerState>> GatheredPlayers;

	FTimerHandle PollHandle;

	/** True once BuildRange has run for the current activation. Cleared by TearDownRange. */
	bool bBuilt = false;

	/**
	 * Which footprint the furniture currently standing in the world was built with, so PollRange can
	 * notice Trace.Practice.OldSize flipping and rebuild. See CVarPracticeOldSize in the .cpp — it is
	 * demo 19 item 1's red arm, and it is what lets the before and after shots share one binary.
	 */
	bool bBuiltOldSize = false;

	/** The spec's toggle. Deliberately not config and not saved: a range opens with it OFF. */
	bool bInfiniteAbilities = false;

	/** Times ApplyInfiniteAbilities() has run since the range opened, and how many of those ticked. */
	int32 InfiniteApplyCount = 0;
	int32 InfiniteTickApplyCount = 0;

	/**
	 * The firing distance the standing furniture was built with (v24 §0). Resolved from the arena's
	 * own endzone depth rather than hard-coded — see TargetStandbackFor().
	 */
	float BuiltTargetStandbackUU = 0.f;

	/** True while the Core is deliberately parked on the rack. See KeepCoreParked. */
	bool bCoreOnRack = false;

	/** World time the park was last re-asserted. */
	float LastCoreParkWorldTime = -1000.f;
};
