// Trace — THE PRACTICE RANGE (spec v19 §2). See TracePracticeRange.h for the design and for the
// leak argument.

#include "Modes/TracePracticeRange.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"                   // -TracePracticeOldSize, item 1's red arm
#include "Misc/Parse.h"
#include "Components/CapsuleComponent.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Modes/TracePracticeActors.h"
#include "Modes/TracePracticeGameMode.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"                              // LogTraceGame
#include "TraceTypes.h"                         // ETraceTeam
#include "World/TraceArenaBuilder.h"

// NAMED, not anonymous — Scripts/check-jumbo-build-collisions.py gates the build on it.
namespace TracePracticeRangeLocal
{
	/** How many stationary targets the range furnishes. One per player on a team, so a full row. */
	constexpr int32 DummyCount = 5;

	// =============================================================================================
	// *** THE RANGE FOOTPRINT — DEMO 19 ITEM 1, VERBATIM: "Make it smaller." ***
	// =============================================================================================
	//
	// WHAT WAS BIG. The range is the arena, and the arena is 33600 x 9600 uu (336 m long). Nothing
	// placed the player inside the range, so they took the shipped endzone spawn — mid-endzone, about
	// 15600 uu from the centre circle — while the target row sat at 0.18 of the field length on the
	// FAR side of centre, i.e. +6048. That is a 21600 uu walk, over 200 metres, before the first
	// shot, repeated after every death. The three pads then sat 900 uu behind centre, another 7000 uu
	// back from the targets, so using the CHANGE CHARACTER pad and then shooting was a round trip of
	// nearly 15000 uu.
	//
	// WHAT SMALL MEANS HERE, and why these numbers. The range keeps the arena (see the header: spec
	// v19 §4.1 kills anything outside GetFieldBounds(), and the walls and ramps you would come here
	// to practise movement on are already in it). What shrinks is THE FURNISHED FOOTPRINT — the part
	// you are put in and expected to use:
	//
	//   2200 uu from the spawn line to the targets. Chosen as a firing distance, not as a small
	//     number: the gun reaches 36000 uu, but 2200 is far enough that aim matters and close enough
	//     that a knife rush is about four seconds. It is also inside the 2400 uu the arena's own
	//     endzone is deep, so the whole range is smaller than one end of the field.
	//   260 uu between targets, so the five-wide row spans 1040 uu and the outermost target is 13
	//     degrees off the crosshair — one glance takes in the whole row.
	//   450 uu from the spawn line to the two toggle pads, +/-620 uu out: a second's walk, in view
	//     from the spawn, and clear of the firing line to the targets.
	//
	// THE CORE RACK IS DELIBERATELY BEHIND YOU. It has to sit on ATraceCore::GetHomeLocation() (the
	// field centre) — that is what makes "leave the Core here" a real state of the shipped Core
	// rather than a rule this file re-implements — so the spawn line is placed 900 uu FORWARD of
	// centre and everything else forward of that. Walking from the spawn to the targets therefore
	// never crosses the rack, which matters now that the range opens with the Core parked on it:
	// crossing it would hand you the Core, and a carrier can neither shoot nor knife.
	//
	// Total furnished footprint, rack to far targets: 3100 x 1240 uu, against ~21600 uu before.

	// =============================================================================================
	// *** SPEC v24 §0 — THE TWO NUMBERS BELOW USED TO BE ABSOLUTES AND ARE NOW DERIVED ***
	// =============================================================================================
	//
	// The standing rule is about a value that MODIFIES a base having to move when the base moves. The
	// range's footprint is exactly that shape: both of these numbers were written as absolutes whose
	// only justification, in the comment block above, is a RATIO to something else —
	//
	//   "2200 ... is also inside the 2400 uu the arena's own endzone is deep, so the whole range is
	//    smaller than one end of the field"
	//   "260 uu between targets ... the outermost target is 13 degrees off the crosshair"
	//
	// — and neither claim survived the base moving. ATraceArenaBuilder::EndzoneDepth is an editable
	// UPROPERTY: drop it to 1200 and the "smaller than one end of the field" sentence quietly becomes
	// false while the code keeps saying 2200. Move the standback and the "13 degrees" sentence
	// quietly becomes false while the code keeps saying 260. Both are now computed FROM their base,
	// so both sentences stay true by construction, and on the shipped arena both resolve to the same
	// geometry as before (2200.1 uu and 260.0 uu) — which is what a §0 fix should look like.

	/**
	 * The firing distance, as a fraction of the arena's own endzone depth.
	 *
	 * 0.9167 x 2400 = 2200.1 uu on the shipped arena, i.e. the number this used to be. Under 1.0 is
	 * the whole claim: the range is shallower than one end of the field, whatever that end is.
	 */
	constexpr float TargetStandbackFractionOfEndzone = 0.9167f;

	/** Floor and ceiling, so a pathological EndzoneDepth cannot put the targets in your face or a
	 *  kilometre away. Wide enough never to bind on a sane arena. */
	constexpr float MinTargetStandbackUU = 600.f;
	constexpr float MaxTargetStandbackUU = 6000.f;

	/**
	 * Fallback firing distance, for the one case the arena cannot be asked (it is null, or its bounds
	 * are invalid). Same number the shipped arena resolves to.
	 */
	constexpr float FallbackTargetStandbackUU = 2200.f;

	/**
	 * How far FORWARD of the field centre (and so of the Core rack) the spawn line sits, in uu.
	 *
	 * Left absolute deliberately: it is a clearance from the rack, not a claim about the arena's size,
	 * and the rack is a point rather than a dimension.
	 */
	constexpr float PlayerSpawnForwardUU = 900.f;

	/**
	 * Half the angular width of the target row, in degrees, measured from the spawn line.
	 *
	 * THIS is the number the design comment is actually about — "one glance takes in the whole row".
	 * The lateral spacing is derived from it and from the firing distance, so the row keeps the same
	 * apparent width when the standback moves. atan(520 / 2200) = 13.30 degrees, which is the row the
	 * range shipped with.
	 */
	constexpr float DummyRowHalfAngleDegrees = 13.30f;

	/** The lateral spacing that puts the OUTERMOST target DummyRowHalfAngleDegrees off the crosshair. */
	float DummySpacingForStandback(float StandbackUU)
	{
		if (DummyCount < 2)
		{
			return 0.f;
		}

		const float OutermostOffsetY =
			StandbackUU * FMath::Tan(FMath::DegreesToRadians(DummyRowHalfAngleDegrees));
		return (OutermostOffsetY * 2.f) / static_cast<float>(DummyCount - 1);
	}

	/**
	 * The firing distance for @p Arena, derived from ITS endzone depth rather than from a constant.
	 *
	 * ClampedEndzoneDepth() rather than the raw EndzoneDepth on purpose: the arena's own header says
	 * to call it instead of re-clamping, and it is the depth the endzone trigger is actually built
	 * with, which is the thing the "smaller than one end of the field" claim is about.
	 */
	float TargetStandbackFor(const ATraceArenaBuilder* Arena)
	{
		if (Arena == nullptr)
		{
			return FallbackTargetStandbackUU;
		}

		const float FromEndzone = Arena->ClampedEndzoneDepth() * TargetStandbackFractionOfEndzone;
		return FMath::Clamp(FromEndzone, MinTargetStandbackUU, MaxTargetStandbackUU);
	}

	/** How far ahead of the spawn line, and how far out to the sides, the two toggle pads sit. */
	constexpr float TogglePadForwardUU = 450.f;
	constexpr float TogglePadOutY = 620.f;

	// ---- THE RED ARM FOR ITEM 1 -----------------------------------------------------------------
	//
	// Restores the footprint above EXACTLY as it was, and nothing else: same target row fraction, same
	// spacing, same pads behind centre, and no spawn line — so the player takes the shipped endzone
	// spawn and has to walk. It deliberately does NOT restore the Core-on-kickoff behaviour, because
	// that is item 3's bug and an arm that moves two things at once measures neither.
	//
	// It exists so the BEFORE and AFTER screenshots come from one binary, taken minutes apart, rather
	// than from two builds that differ in whatever else landed in between.
	TAutoConsoleVariable<int32> CVarPracticeOldSize(
		TEXT("Trace.Practice.OldSize"),
		0,
		TEXT("DEV ONLY. RED ARM for demo 19 item 1. 1 rebuilds the practice range with the OLD, big "
		     "footprint: target row at 0.18 of the field, pads 900uu behind centre, and no range spawn "
		     "line, so the player starts in the endzone ~21600uu from the targets."),
		ECVF_Cheat);

	/**
	 * The arm, as one question.
	 *
	 * ALSO READABLE AS A BARE LAUNCH SWITCH (`-TracePracticeOldSize`), and that is not belt-and-braces:
	 * the BEFORE screenshot needs the arm live before the player's FIRST spawn is chosen, which happens
	 * before any console command this project can deliver (-TraceExec waits for a pawn, which is the
	 * very thing being placed). A switch is parsed from the command line at engine init, so it is the
	 * only form that is certainly early enough. Latched: the command line cannot change mid-session.
	 */
	bool IsOldSizeArmed()
	{
		static const bool bFromCommandLine =
			FParse::Param(FCommandLine::Get(), TEXT("TracePracticeOldSize"));
		return bFromCommandLine || CVarPracticeOldSize.GetValueOnAnyThread() != 0;
	}

	/** The old target row's distance from centre, as a fraction of field length. Arm only. */
	constexpr float OldDummyRowFraction = 0.18f;

	/** The old lateral spacing between targets, in uu. Arm only. */
	constexpr float OldDummySpacingY = 400.f;

	/** The old pad placement: behind centre, and further out. Arm only. */
	constexpr float OldTogglePadBackX = 900.f;
	constexpr float OldTogglePadOutY = 650.f;

	/** Fallback pawn half height when the pawn class cannot be interrogated. */
	constexpr float FallbackCapsuleHalfHeight = 90.f;

	/**
	 * How far the racked Core may sit from its park point before the rack puts it back, in uu.
	 *
	 * Comfortably larger than any settle or depenetration the Core does on its own, and vastly
	 * smaller than the distance a kickoff moves it (measured at 15864 uu). See KeepCoreParked.
	 */
	constexpr double RackDriftToleranceUU = 250.0;

#if !UE_BUILD_SHIPPING
	/**
	 * *** THE RED ARM. *** 1 makes TracePracticeRange::IsActive() answer YES in EVERY world,
	 * including a real match's — so the range's cheats genuinely apply there and
	 * Trace.Practice.LeakTest, unchanged, reports FAIL. A leak test that cannot be made to fail is
	 * not evidence, and this project has already lost two passes to exactly that.
	 */
	TAutoConsoleVariable<int32> CVarPracticeLeakArm(
		TEXT("Trace.Practice.LeakArm"),
		0,
		TEXT("DEV ONLY. 1 forces the practice range's gate open in EVERY world, so the range's "
		     "cheats leak into a real match on purpose and Trace.Practice.LeakTest can be shown to "
		     "FAIL. 0 (default) is the shipped gate: the range is active only under "
		     "ATracePracticeGameMode."),
		ECVF_Cheat);

	/**
	 * *** SPEC v24 §5's RED ARM. *** 1 restores the pre-v24 driver for the infinite-abilities toggle
	 * exactly: only the 5 Hz furniture poll re-applies the cheat, and the per-frame tick does not.
	 * That is the shipped-before build the owner reported — the cooldown really does start on every
	 * press and TryActivate() really does refuse — so Trace.Practice.InfiniteVerify, unchanged,
	 * reports FAIL with the arm on and PASS with it off, from one binary.
	 */
	TAutoConsoleVariable<int32> CVarPracticePollOnlyInfinite(
		TEXT("Trace.Practice.PollOnlyInfinite"),
		0,
		TEXT("DEV ONLY. RED ARM for spec v24 §5. 1 drives the range's infinite-abilities toggle from "
		     "the 5 Hz furniture poll ONLY, which is the build in which the toggle 'does nothing': "
		     "every press starts a real cooldown and the ability is refused until the next poll. "
		     "0 (default) also refreshes it every frame, after input, so it is never refused."),
		ECVF_Cheat);

	/**
	 * *** DEMO 29 ITEM 2's A/B KNOB. *** See ShouldUseOwnerArmsViewModel() in the header for the
	 * whole argument; the short version is that this can only ever turn the fixture OFF, and only
	 * inside the range. It defaults to 1 because the owner asked for the rig to be there when they
	 * open the range — "I need to test this" — not for a switch they then have to find.
	 *
	 * NOT A RED ARM, unlike the two above, and the difference is worth stating: LeakArm and
	 * PollOnlyInfinite exist to make a green harness go red. This one changes which of two meshes is
	 * drawn in one game mode and asserts nothing.
	 */
	TAutoConsoleVariable<int32> CVarPracticeArmsRig(
		TEXT("Trace.Practice.ArmsRig"),
		1,
		TEXT("DEV ONLY. Demo 29 item 2. 1 (default) draws the owner's imported first-person arms rig "
		     "(SK_TraceArms) in the PRACTICE RANGE in place of the shipped pack hands "
		     "(SK_TraceHands); 0 puts the pack hands back, so the two can be A/B'd live in one "
		     "session. Has no effect in any other game mode: the range gate is "
		     "TracePracticeRange::IsActive(), which no cvar can open in a real match."),
		ECVF_Cheat);
#endif
}

namespace TracePracticeRange
{
	bool IsActive(const UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr)
		{
			return false;
		}

#if !UE_BUILD_SHIPPING
		if (IsLeakArmed())
		{
			return true;
		}
#endif

		// THE WHOLE GATE, and it is one line on purpose. AGameModeBase exists only on the server, so
		// this is false on a remote client by construction, and the mode itself can only arrive on
		// the travel URL — there is no setting, cvar or .ini that can turn a real match into a range.
		return WorldPtr->GetAuthGameMode<ATracePracticeGameMode>() != nullptr;
	}

#if !UE_BUILD_SHIPPING
	bool IsLeakArmed()
	{
		return TracePracticeRangeLocal::CVarPracticeLeakArm.GetValueOnAnyThread() != 0;
	}

	bool IsInfinitePollOnlyArmed()
	{
		return TracePracticeRangeLocal::CVarPracticePollOnlyInfinite.GetValueOnAnyThread() != 0;
	}

	bool ShouldUseOwnerArmsViewModel(const UWorld* WorldPtr)
	{
		// THE RANGE TERM FIRST, AND NOT ONLY FOR SPEED. Written this way round the cvar is never
		// consulted at all in a real match, so there is no reading of this line on which a value
		// typed into a match's console reaches the viewmodel.
		return IsActive(WorldPtr)
			&& TracePracticeRangeLocal::CVarPracticeArmsRig.GetValueOnAnyThread() != 0;
	}
#endif
}

// =================================================================================================
// Lifecycle
// =================================================================================================

bool UTracePracticeRangeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Game worlds only. There is nothing for this to do in an editor preview, a thumbnail render or
	// the transient package's world.
	const UWorld* const OuterWorld = Cast<UWorld>(Outer);
	return (OuterWorld != nullptr) && OuterWorld->IsGameWorld();
}

void UTracePracticeRangeSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// A LOOPING POLL RATHER THAN A HOOK, for the same reason ATraceGameMode polls the select screen:
	// the three facts this needs (the game mode exists, the arena has been built, the Core has been
	// spawned) become true on three different schedules, and a poll is idempotent and
	// self-correcting in BOTH directions — it tears the range down as readily as it builds it, which
	// is what makes Trace.Practice.LeakArm 0 put a leaked range away again.
	InWorld.GetTimerManager().SetTimer(
		PollHandle, this, &UTracePracticeRangeSubsystem::PollRange, PollIntervalSeconds, /*bLoop=*/true);
}

void UTracePracticeRangeSubsystem::Deinitialize()
{
	if (UWorld* const WorldPtr = GetWorld())
	{
		WorldPtr->GetTimerManager().ClearTimer(PollHandle);
	}

	TearDownRange();
	Super::Deinitialize();
}

UTracePracticeRangeSubsystem* UTracePracticeRangeSubsystem::Get(const UWorld* WorldPtr)
{
	return (WorldPtr != nullptr) ? WorldPtr->GetSubsystem<UTracePracticeRangeSubsystem>() : nullptr;
}

// =================================================================================================
// *** THE INFINITE-ABILITIES TOGGLE'S REAL DRIVER — SPEC v24 §5 ***
//
// See the block above the FTickableGameObject overrides in the header for the diagnosis and for why
// a tick beats a faster timer. In one line: the press writes the cooldown in TG_PrePhysics and this
// erases it later in the SAME frame, so no reader downstream of the press ever sees one.
// =================================================================================================

bool UTracePracticeRangeSubsystem::IsTickable() const
{
	// The single cheapest question this class can be asked, and the answer is false in every world
	// that is not a range with the toggle deliberately on. SetInfiniteAbilities refuses outside the
	// gate, BuildRange opens with it off, and TearDownRange clears it — so a real match never ticks
	// this at all, and the leak argument in the header is untouched by the class gaining a tick.
	return bInfiniteAbilities;
}

TStatId UTracePracticeRangeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTracePracticeRangeSubsystem, STATGROUP_Tickables);
}

void UTracePracticeRangeSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bInfiniteAbilities)
	{
		return;
	}

#if !UE_BUILD_SHIPPING
	if (TracePracticeRange::IsInfinitePollOnlyArmed())
	{
		// THE RED ARM. The toggle is on, the poll still sweeps at 5 Hz, and this frame does nothing —
		// which is precisely the build the owner reported. Trace.Practice.InfiniteVerify then fails on
		// the same assertions it passes with the arm off.
		return;
	}
#endif

	// GATED AGAIN HERE, not only by IsTickable(). bInfiniteAbilities cannot be true outside the range
	// today, and a rule whose only enforcement is "that bool cannot be set" is one future setter away
	// from being no rule at all. Same argument as HandlePadTouched's second gate.
	if (!TracePracticeRange::IsActive(GetWorld()))
	{
		return;
	}

	ApplyInfiniteAbilities();
	++InfiniteTickApplyCount;
}

// =================================================================================================
// The poll
// =================================================================================================

void UTracePracticeRangeSubsystem::PollRange()
{
	UWorld* const WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	// *** THE GATE. Everything below this line is range-only. ***
	if (!TracePracticeRange::IsActive(WorldPtr))
	{
		if (bBuilt)
		{
			// Reachable in exactly one way: Trace.Practice.LeakArm was on and has been turned off.
			// The range puts itself away rather than leaving furniture in somebody's match.
			UE_LOG(LogTraceGame, Display,
				TEXT("[Practice] the range's gate closed - tearing the range down."));
			TearDownRange();
		}
		return;
	}

	// DEMO 19 ITEM 1's RED ARM. Trace.Practice.OldSize restores the footprint the user called too big,
	// and rebuilding on the flip is what lets the BEFORE and AFTER screenshots come out of ONE binary
	// instead of one build each. BuildRange and TearDownRange are both idempotent, which is the whole
	// reason this is three lines.
	const bool bWantOldSize = TracePracticeRangeLocal::IsOldSizeArmed();
	if (bBuilt && bWantOldSize != bBuiltOldSize)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Practice] Trace.Practice.OldSize flipped to %d - rebuilding the range's furniture."),
			bWantOldSize ? 1 : 0);
		TearDownRange();
	}

	if (!bBuilt)
	{
		BuildRange();
	}

	// DEMO 19 ITEM 1. In the poll rather than in BuildRange because the two facts arrive on different
	// schedules: the range is furnished at about 2 s and the solo player's pawn does not exist until
	// they have left the character select screen. It is a one-shot per player — see the body.
	GatherPlayersIntoRange();

	KeepCoreParked();
	SuppressScore();

	if (bInfiniteAbilities)
	{
		ApplyInfiniteAbilities();
	}
}

// =================================================================================================
// Building and tearing down
// =================================================================================================

void UTracePracticeRangeSubsystem::BuildRange()
{
	UWorld* const WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	// THE RANGE IS THE ARENA. No new map and no new geometry — see the header for why (spec v19 §4.1
	// kills anything outside these bounds, so a room beside the arena would be a room that executes
	// you for standing in it).
	const ATraceArenaBuilder* const Arena = ATraceArenaBuilder::Get(WorldPtr);
	if (Arena == nullptr)
	{
		// Not an error yet: the poll runs from OnWorldBeginPlay and the game mode builds the arena in
		// PreInitializeComponents, but a map with no builder at all would never get one. Silent, and
		// tried again in 200 ms.
		return;
	}

	const FBox FieldBox = Arena->GetFieldBounds();
	if (FieldBox.IsValid == 0)
	{
		return;
	}

	const FVector FieldCentre = FieldBox.GetCenter();
	const FVector FieldSize = FieldBox.GetSize();

	// DEMO 19 ITEM 1's RED ARM — see CVarPracticeOldSize. Latched into bBuiltOldSize at the bottom so
	// PollRange can notice a flip and rebuild.
	const bool bOldSize = TracePracticeRangeLocal::IsOldSizeArmed();

	// ---- the spawn line ---------------------------------------------------------------------------
	//
	// DEMO 19 ITEM 1. Everything the range furnishes is now measured from HERE rather than from the
	// two ends of a 336 m field, and the player is put on it (SpawnPlayerStartPost + the game mode's
	// ChoosePlayerStart, so a death brings them back to the range and not to the endzone).
	const float SpawnLineX = FieldCentre.X + TracePracticeRangeLocal::PlayerSpawnForwardUU;

	// ---- the stationary targets ------------------------------------------------------------------
	//
	// A row across the range, StandbackUU in front of the spawn line, facing back down it.
	//
	// SPEC v24 §0: the standback and the spacing are DERIVED (from the arena's own endzone depth, and
	// from the row's stated 13.3-degree half angle) rather than typed in. See the block at the top of
	// TracePracticeRangeLocal. The OLD-SIZE arm keeps its literals — an arm's whole job is to be the
	// number that shipped before, and deriving it would make it track the thing it is measuring.
	const float StandbackUU = TracePracticeRangeLocal::TargetStandbackFor(Arena);

	const float RowX = bOldSize
		? (FieldCentre.X + FieldSize.X * TracePracticeRangeLocal::OldDummyRowFraction)
		: (SpawnLineX + StandbackUU);
	const float SpacingY = bOldSize
		? TracePracticeRangeLocal::OldDummySpacingY
		: TracePracticeRangeLocal::DummySpacingForStandback(StandbackUU);
	const float FirstY = FieldCentre.Y - SpacingY * (TracePracticeRangeLocal::DummyCount - 1) * 0.5f;

	int32 SpawnedDummies = 0;
	for (int32 Index = 0; Index < TracePracticeRangeLocal::DummyCount; ++Index)
	{
		const FVector Where(RowX, FirstY + SpacingY * Index, FieldCentre.Z);
		if (SpawnDummy(Where) != nullptr)
		{
			++SpawnedDummies;
		}
	}

	// ---- the three pads --------------------------------------------------------------------------
	//
	// THE CORE RACK GOES WHERE THE CORE ALREADY PARKS. ATraceCore::GetHomeLocation() is the point the
	// Core is teleported to when it is taken out of play, so putting the rack there means "leave the
	// Core here" is a real state of the shipped Core rather than a rule this file re-implements.
	FVector RackPoint = Arena->GetCoreSpawnLocation();
	if (const ATraceCore* TheCore = GetCore())
	{
		RackPoint = TheCore->GetHomeLocation();
	}

	SpawnPad(ETracePracticePadRole::CoreRack, RackPoint, TEXT("CORE RACK\nwalk on to drop / collect"));

	const float PadX = bOldSize
		? (FieldCentre.X - TracePracticeRangeLocal::OldTogglePadBackX)
		: (SpawnLineX + TracePracticeRangeLocal::TogglePadForwardUU);
	const float PadOutY = bOldSize
		? TracePracticeRangeLocal::OldTogglePadOutY
		: TracePracticeRangeLocal::TogglePadOutY;

	SpawnPad(ETracePracticePadRole::InfiniteAbilities,
		FVector(PadX, FieldCentre.Y - PadOutY, FieldCentre.Z),
		TEXT("INFINITE ABILITIES: OFF"));

	SpawnPad(ETracePracticePadRole::CharacterSwap,
		FVector(PadX, FieldCentre.Y + PadOutY, FieldCentre.Z),
		TEXT("CHANGE CHARACTER\nwalk on to reopen select"));

	// ---- where the player stands ------------------------------------------------------------------
	//
	// Armed, no spawn line is built at all — which is precisely the old behaviour: GetPlayerStartPost
	// returns null, ChoosePlayerStart falls through to the shipped endzone pipeline, and
	// GatherPlayersIntoRange has nothing to gather anyone onto.
	if (!bOldSize)
	{
		SpawnPlayerStartPost(FVector(SpawnLineX, FieldCentre.Y, FieldCentre.Z));
	}

	bBuilt = true;
	bBuiltOldSize = bOldSize;     // so PollRange can notice the arm flipping and rebuild
	bInfiniteAbilities = false;   // a range always opens with the cheat OFF
	InfiniteApplyCount = 0;
	InfiniteTickApplyCount = 0;

	// The RESOLVED standback, so LogStatus and the harnesses report the distance that was actually
	// built rather than the constant somebody hopes it still equals (v24 §0).
	BuiltTargetStandbackUU = bOldSize ? (RowX - FieldCentre.X) : StandbackUU;

	// ---- *** THE CORE STARTS ON THE RACK — DEMO 19 ITEM 3 *** -------------------------------------
	//
	// MEASURED, not anticipated. Saved/Logs/v22range-knife-red.log: the range's ordinary start-of-half
	// kickoff handed the Core to Blue — and in a solo practice session the player IS Blue, so they
	// were handed the Core about nine seconds in and never put it down.
	//
	// (THE ROUTE IN THAT MEASUREMENT NO LONGER EXISTS, AND THE PROBLEM DOES. It was
	// ATraceGameMode::BeginHalf -> GrantCoreToTeam -> KickoffTo(Blue); DEMO 29 §3 replaced it with
	// BeginHalf -> KickoffCoreForHalf -> ATraceCore::KickoffContested, which puts the Core LOOSE on
	// the centre octagon and grants it to nobody. A range player who walks over it still picks it up,
	// and KeepCoreParked() below still takes it back — that poll watches the Core's STATE, not the
	// call that changed it, which is why it needed no edit for either behaviour.) A CARRIER CANNOT DRAW THE KNIFE AND CANNOT SWING IT
	// (UTraceWeaponComponent::RequestEquip and CanSwing both refuse with ETraceMeleeRefusal::Carrying,
	// silently), and cannot fire the gun either. That is the whole of "knife backstabs don't work" as
	// seen from the practice range: the knife never comes out at all.
	//
	// The fix is the range's own existing machinery pointed at the opening state: bCoreOnRack is the
	// flag KeepCoreParked() already watches, and its kickoff-catching arm — written for exactly this
	// event, with the measurement in its comment — takes the Core back off whoever a kickoff hands it
	// to within one 200 ms poll. So the range opens with the Core PARKED, the player opens with empty
	// hands, and the CORE RACK pad is how you deliberately pick it up. No rule is switched off: the
	// Core is out of play, which is a state ATraceCore already has.
	bCoreOnRack = true;
	LastCoreParkWorldTime = WorldPtr->GetTimeSeconds();
	if (ATraceCore* TheCore = GetCore())
	{
		TheCore->KickoffTo(ETraceTeam::None);
	}

	// The two numbers demo 19 item 1 is a claim about, printed rather than described: where the target
	// row ended up, and how far the player has to travel to reach it from where they start.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] RANGE OPEN in the arena (%s): %d/%d stationary targets at X=%.0f spaced %.0fuu, "
		     "%d pads. Spawn line %s. Core rack at %s (the Core starts PARKED, so your hands are free). "
		     "Infinite abilities OFF."),
		bOldSize ? TEXT("*** OLD, BIG FOOTPRINT — Trace.Practice.OldSize is ON ***") : TEXT("small footprint"),
		SpawnedDummies, TracePracticeRangeLocal::DummyCount, RowX, SpacingY, GetPadCount(),
		bOldSize
			? TEXT("NONE - the player takes the shipped endzone spawn")
			: *FString::Printf(TEXT("X=%.0f, %.1fuu behind the targets (%.2f x the arena's own %.0fuu "
			                        "endzone depth - v24 §0, derived rather than typed)"),
				SpawnLineX, StandbackUU,
				TracePracticeRangeLocal::TargetStandbackFractionOfEndzone, Arena->ClampedEndzoneDepth()),
		*RackPoint.ToCompactString());
}

ATracePracticePost* UTracePracticeRangeSubsystem::SpawnPlayerStartPost(const FVector& DesiredPoint)
{
	UWorld* const WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return nullptr;
	}

	FVector FloorPoint = FVector::ZeroVector;
	if (!TraceToFloor(DesiredPoint, FloorPoint))
	{
		return nullptr;
	}

	// Facing the targets. ChoosePlayerStart hands this actor back, and AGameModeBase spawns the pawn
	// on the start spot's ROTATION — so this one number is what makes a player open the range already
	// looking down the firing line instead of at a wall 300 metres from anything.
	FActorSpawnParameters PostParams;
	PostParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ATracePracticePost* const PostActor = WorldPtr->SpawnActor<ATracePracticePost>(
		ATracePracticePost::StaticClass(), FloorPoint + FVector(0.f, 0.f, 3.f),
		FRotator::ZeroRotator, PostParams);
	if (PostActor == nullptr)
	{
		return nullptr;
	}

	PlayerStartPost = PostActor;
	Posts.Add(PostActor);   // so TearDownRange destroys it with everything else
	return PostActor;
}

void UTracePracticeRangeSubsystem::GatherPlayersIntoRange()
{
	UWorld* const WorldPtr = GetWorld();
	ATracePracticePost* const StartPost = PlayerStartPost.Get();
	const AGameStateBase* const BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	if (StartPost == nullptr || BaseState == nullptr)
	{
		return;
	}

	// ONCE PER PLAYER, EVER. ChoosePlayerStart covers every respawn; this covers only the FIRST life,
	// which the shipped endzone spawn had already handed out before the subsystem's 5 Hz poll got to
	// build anything. Repeating it would be a range that yanks you back every time you walked off to
	// wall-jump on the arena's ramps — which is a thing you came here to do (see the header).
	for (APlayerState* const EachState : BaseState->PlayerArray)
	{
		if (EachState == nullptr || EachState->IsABot())
		{
			continue;
		}

		ATraceCharacter* const PlayerPawn = Cast<ATraceCharacter>(EachState->GetPawn());
		if (PlayerPawn == nullptr)
		{
			continue;   // still in character select, or mid-respawn; try again next poll
		}

		if (GatheredPlayers.Contains(EachState))
		{
			continue;
		}
		GatheredPlayers.Add(EachState);

		const FVector Where = StartPost->GetActorLocation() + FVector(0.f, 0.f, 120.f);
		const double MovedFrom = FVector::Dist(PlayerPawn->GetActorLocation(), Where);
		PlayerPawn->TeleportTo(Where, StartPost->GetActorRotation(), /*bIsATest=*/false, /*bNoCheck=*/true);
		if (AController* PlayerCtrl = PlayerPawn->GetController())
		{
			PlayerCtrl->SetControlRotation(StartPost->GetActorRotation());
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Practice] '%s' was %0.f uu away when the range opened; moved onto the range's spawn "
			     "line. Every later respawn comes back here through ChoosePlayerStart."),
			*EachState->GetPlayerName(), MovedFrom);
	}
}

void UTracePracticeRangeSubsystem::TearDownRange()
{
	for (const TWeakObjectPtr<ATracePracticeDummyController>& EachDummy : Dummies)
	{
		if (ATracePracticeDummyController* DummyCtrl = EachDummy.Get())
		{
			if (APawn* DummyPawn = DummyCtrl->GetPawn())
			{
				DummyCtrl->UnPossess();
				DummyPawn->Destroy();
			}
			DummyCtrl->Destroy();
		}
	}
	Dummies.Reset();

	for (const TWeakObjectPtr<ATracePracticePost>& EachPost : Posts)
	{
		if (ATracePracticePost* PostActor = EachPost.Get())
		{
			PostActor->Destroy();
		}
	}
	Posts.Reset();

	for (const TWeakObjectPtr<ATracePracticePad>& EachPad : Pads)
	{
		if (ATracePracticePad* PadActor = EachPad.Get())
		{
			PadActor->Destroy();
		}
	}
	Pads.Reset();

	PlayerStartPost.Reset();
	GatheredPlayers.Reset();

	bBuilt = false;
	bInfiniteAbilities = false;
	bCoreOnRack = false;
	InfiniteApplyCount = 0;
	InfiniteTickApplyCount = 0;
	BuiltTargetStandbackUU = 0.f;
}

bool UTracePracticeRangeSubsystem::TraceToFloor(const FVector& Point, FVector& OutFloorPoint) const
{
	const UWorld* const WorldPtr = GetWorld();
	const ATraceArenaBuilder* const Arena = ATraceArenaBuilder::Get(WorldPtr);
	if (WorldPtr == nullptr || Arena == nullptr)
	{
		return false;
	}

	const FBox FieldBox = Arena->GetFieldBounds();
	if (FieldBox.IsValid == 0)
	{
		return false;
	}

	const FVector From(Point.X, Point.Y, FieldBox.Max.Z);
	const FVector To(Point.X, Point.Y, FieldBox.Min.Z - 200.0);

	FHitResult FloorHit;
	FCollisionQueryParams FloorParams(SCENE_QUERY_STAT(TracePracticeFloor), /*bTraceComplex=*/false);
	if (WorldPtr->LineTraceSingleByChannel(FloorHit, From, To, ECC_WorldStatic, FloorParams))
	{
		OutFloorPoint = FloorHit.ImpactPoint;
		return true;
	}

	// No geometry under the point — the arena floor plane is still a truthful answer, and it is what
	// the Core's own surface rule measures against.
	OutFloorPoint = FVector(Point.X, Point.Y, FieldBox.Min.Z);
	return true;
}

ATracePracticeDummyController* UTracePracticeRangeSubsystem::SpawnDummy(const FVector& DesiredXY)
{
	UWorld* const WorldPtr = GetWorld();
	AGameModeBase* const BaseMode = (WorldPtr != nullptr) ? WorldPtr->GetAuthGameMode() : nullptr;
	if (WorldPtr == nullptr || BaseMode == nullptr)
	{
		return nullptr;
	}

	FVector FloorPoint = FVector::ZeroVector;
	if (!TraceToFloor(DesiredXY, FloorPoint))
	{
		return nullptr;
	}

	// Read the capsule off the pawn class rather than assuming 88: ATraceCharacter sets it in its
	// constructor, so the class default object already knows, and a future resize follows.
	float HalfHeight = TracePracticeRangeLocal::FallbackCapsuleHalfHeight;
	if (const ACharacter* PawnDefault = (BaseMode->DefaultPawnClass != nullptr)
		? GetDefault<ACharacter>(BaseMode->DefaultPawnClass) : nullptr)
	{
		if (const UCapsuleComponent* DefaultCapsule = PawnDefault->GetCapsuleComponent())
		{
			HalfHeight = DefaultCapsule->GetScaledCapsuleHalfHeight();
		}
	}

	// Facing the end the solo player spawns in, so the row looks back at you.
	const FRotator DummyFacing(0.f, 180.f, 0.f);
	const FVector StandPoint(FloorPoint.X, FloorPoint.Y, FloorPoint.Z + HalfHeight + 2.f);

	FActorSpawnParameters PostParams;
	PostParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ATracePracticePost* const PostActor = WorldPtr->SpawnActor<ATracePracticePost>(
		ATracePracticePost::StaticClass(), FVector(FloorPoint.X, FloorPoint.Y, FloorPoint.Z + 3.f),
		DummyFacing, PostParams);
	if (PostActor == nullptr)
	{
		return nullptr;
	}

	FActorSpawnParameters ControllerParams;
	ControllerParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ATracePracticeDummyController* const DummyCtrl = WorldPtr->SpawnActor<ATracePracticeDummyController>(
		ATracePracticeDummyController::StaticClass(), StandPoint, DummyFacing, ControllerParams);
	if (DummyCtrl == nullptr)
	{
		PostActor->Destroy();
		return nullptr;
	}

	DummyCtrl->SetHomePost(PostActor);

	// THE SHIPPED SPAWN PATH, not a private one. AGameModeBase::RestartPlayerAtTransform builds the
	// mode's own DefaultPawnClass, possesses it and runs every initialisation a human's pawn gets —
	// which is what makes a dummy an ordinary ATraceCharacter that the shot resolver, the ability
	// choke point and the death pipeline already know how to treat.
	BaseMode->RestartPlayerAtTransform(DummyCtrl, FTransform(DummyFacing, StandPoint));

	if (DummyCtrl->GetPawn() == nullptr)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Practice] a target at %s could not be given a pawn; skipping it."),
			*StandPoint.ToCompactString());
		DummyCtrl->Destroy();
		PostActor->Destroy();
		return nullptr;
	}

	// Registered only once BOTH halves exist, so a partial failure leaves no orphan in either array
	// for TearDownRange to have to reason about.
	Posts.Add(PostActor);
	Dummies.Add(DummyCtrl);
	return DummyCtrl;
}

ATracePracticePad* UTracePracticeRangeSubsystem::SpawnPad(ETracePracticePadRole InRole,
                                                          const FVector& DesiredPoint,
                                                          const FString& InLabel)
{
	UWorld* const WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return nullptr;
	}

	FVector FloorPoint = FVector::ZeroVector;
	if (!TraceToFloor(DesiredPoint, FloorPoint))
	{
		return nullptr;
	}

	FActorSpawnParameters PadParams;
	PadParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ATracePracticePad* const PadActor = WorldPtr->SpawnActor<ATracePracticePad>(
		ATracePracticePad::StaticClass(), FloorPoint + FVector(0.f, 0.f, 10.f),
		FRotator::ZeroRotator, PadParams);
	if (PadActor == nullptr)
	{
		return nullptr;
	}

	PadActor->ConfigurePad(InRole, InLabel);
	Pads.Add(PadActor);
	return PadActor;
}

// =================================================================================================
// THE THREE RANGE-ONLY AFFORDANCES
// =================================================================================================

void UTracePracticeRangeSubsystem::HandlePadTouched(ATracePracticePad* Pad, ATraceCharacter* Toucher)
{
	// GATED AGAIN HERE, not only at spawn time. The pads cannot exist outside the range, but a rule
	// whose only enforcement is "the actor should not be there" is one stray SpawnActor away from
	// being no rule at all.
	if (Pad == nullptr || Toucher == nullptr || !TracePracticeRange::IsActive(GetWorld()))
	{
		return;
	}

	switch (Pad->GetPadRole())
	{
	case ETracePracticePadRole::CoreRack:
		HandleCoreRack(Toucher);
		break;

	case ETracePracticePadRole::InfiniteAbilities:
		SetInfiniteAbilities(!bInfiniteAbilities);
		break;

	case ETracePracticePadRole::CharacterSwap:
		ReopenCharacterSelect(Toucher->GetPlayerState());
		break;

	default:
		break;
	}
}

void UTracePracticeRangeSubsystem::HandleCoreRack(ATraceCharacter* Toucher)
{
	ATraceCore* const TheCore = GetCore();
	if (TheCore == nullptr)
	{
		return;
	}

	if (TheCore->GetCarrier() == Toucher)
	{
		// ---- DROP IT, AND IT STAYS -------------------------------------------------------------
		//
		// KickoffTo(ETraceTeam::None) is the shipped "take the Core out of play" call — the same one
		// ATraceGameMode::ReleaseCore makes for the half-time interval. It releases the holder,
		// clears any loose state and parks the Core at GetHomeLocation(), which is where this pad is.
		//
		// *** THIS IS WHY THERE IS NO TURNOVER, AND IT IS NOT A SUSPENDED RULE. *** The surface
		// turnover in ATraceCore::ServerSurfaceTurnover refuses outright unless the Core is LOOSE
		// (`if (!HasAuthority() || !bLoose || bCoreStateLocked) return false;`). A parked Core is not
		// loose, so the rule never fires — the range did not switch it off, weaken it, or fork it,
		// and there is consequently nothing about it that could be switched on somewhere else.
		TheCore->KickoffTo(ETraceTeam::None);
		bCoreOnRack = true;
		LastCoreParkWorldTime = (GetWorld() != nullptr) ? GetWorld()->GetTimeSeconds() : 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("[Practice] %s racked the Core. It will stay there until somebody collects it."),
			*GetNameSafe(Toucher));
		return;
	}

	if (TheCore->IsHeld())
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Practice] the rack was stepped on, but %s is carrying the Core."),
			*GetNameSafe(TheCore->GetCarrier()));
		return;
	}

	// ---- COLLECT IT ----------------------------------------------------------------------------
	//
	// TryPickup is ATraceCore's unconditional debug grant. It is the right call precisely BECAUSE it
	// is unconditional: the Core is parked out of play, so none of the shipped acquisition routes
	// (the loose-pickup poll, the catch zone, a turnover) can see it at all, and re-deriving one
	// here would be a second way to take the Core that a real match would then have to be proved
	// safe from.
	TheCore->TryPickup(Toucher);
	bCoreOnRack = false;

	UE_LOG(LogTraceGame, Display, TEXT("[Practice] %s collected the Core from the rack."),
		*GetNameSafe(Toucher));
}

void UTracePracticeRangeSubsystem::KeepCoreParked()
{
	if (!bCoreOnRack)
	{
		return;
	}

	UWorld* const WorldPtr = GetWorld();
	ATraceCore* const TheCore = GetCore();
	if (WorldPtr == nullptr || TheCore == nullptr)
	{
		return;
	}

	const float NowWorld = WorldPtr->GetTimeSeconds();

	// ---- THE RACK HOLDS AGAINST A KICKOFF -------------------------------------------------------
	//
	// MEASURED RED, not anticipated. Trace.Practice.Verify failed here with the Core racked and then
	// gone: Saved/Logs/practice-verify-range.log shows "[Practice] ... racked the Core" at t=16.5 s,
	// then "Core: kickoff queued for Blue in 1.0s" / "Kickoff: the Core goes to Blue." at t=17.7 s,
	// and four seconds later the Core was in a player's hands 15864 uu away. The source was the
	// ordinary start-of-half kickoff, which is entirely correct for a match and simply does not know
	// that in here the Core was put down on purpose.
	//
	// SINCE DEMO 29 §3 THAT KICKOFF PLACES RATHER THAN GRANTS — BeginHalf -> KickoffCoreForHalf ->
	// ATraceCore::KickoffContested leaves the Core LOOSE on the centre octagon with no holder — so
	// the two log lines quoted above are no longer what a half start prints. Both arms below still
	// catch it: the held arm covers a range player who walks onto the loose Core, and the drift arm
	// covers the placement itself, which moves the Core off the rack whether or not anybody takes it.
	// The rack therefore holds against the new restart for the same reason it held against the old
	// one — it watches where the Core IS.
	//
	// This version therefore RE-PARKS instead of surrendering. It is not a fork of the kickoff rule:
	// KickoffTo(None) is the same shipped call HandleCoreRack makes, and parking clears
	// PendingGrantTeam as a side effect — so one kickoff produces one take-back and one log line,
	// not a fight repeated at 5 Hz.
	//
	// The one thing that legitimately takes the Core off the rack is stepping on the pad, and that
	// path clears bCoreOnRack before this poll can run again — so reaching here with the Core held
	// always means something else took it.
	if (const ATraceCharacter* Snatcher = TheCore->GetCarrier())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Practice] a kickoff handed the racked Core to %s; the rack is taking it back. "
			     "Walk onto the CORE RACK to collect it."),
			*GetNameSafe(Snatcher));

		TheCore->KickoffTo(ETraceTeam::None);
		LastCoreParkWorldTime = NowWorld;
		return;
	}

	// The same event seen a fifth of a second earlier: KickoffTo(Team) teleports the Core to the
	// receiving end of the field and only hands it over a second later. Catching the move as well as
	// the hand-over means the Core is never left LOOSE somewhere else — which is the one state in
	// which ATraceCore::ServerSurfaceTurnover could see it at all.
	const double DriftFromHome = FVector::Dist(TheCore->GetActorLocation(), TheCore->GetHomeLocation());

	// ATraceCore forces a kickoff after 15 s of a holderless Core during a live half — its promise
	// that the Core never goes missing, and completely correct for a real match. Re-asserting the
	// park well inside that window is how the range holds "leave it here" WITHOUT reaching into
	// ATraceCore, which this pass does not own and must not fork.
	if (DriftFromHome > TracePracticeRangeLocal::RackDriftToleranceUU
		|| NowWorld - LastCoreParkWorldTime >= CoreReparkSeconds)
	{
		TheCore->KickoffTo(ETraceTeam::None);
		LastCoreParkWorldTime = NowWorld;
	}
}

bool UTracePracticeRangeSubsystem::SetInfiniteAbilities(bool bEnabled)
{
	if (!TracePracticeRange::IsActive(GetWorld()))
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Practice] infinite abilities refused: this is not the practice range."));
		return false;
	}

	bInfiniteAbilities = bEnabled;
	RefreshInfiniteAbilitiesPad();

	UE_LOG(LogTraceGame, Display, TEXT("[Practice] infinite abilities %s."),
		bInfiniteAbilities ? TEXT("ON - the E cooldown and the dash pool refill continuously")
		                   : TEXT("OFF - cooldowns run normally again"));

	if (bInfiniteAbilities)
	{
		ApplyInfiniteAbilities();
	}

	return bInfiniteAbilities;
}

void UTracePracticeRangeSubsystem::ApplyInfiniteAbilities()
{
	const UWorld* const WorldPtr = GetWorld();
	const AGameStateBase* const BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	if (BaseState == nullptr)
	{
		return;
	}

	++InfiniteApplyCount;

	for (APlayerState* const EachState : BaseState->PlayerArray)
	{
		if (EachState == nullptr || EachState->IsABot())
		{
			// The targets are bots and hold no character; refilling nothing costs nothing, but
			// skipping them keeps the intent readable.
			continue;
		}

#if !UE_BUILD_SHIPPING
		if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(EachState))
		{
			// WHAT THIS DOES AND DOES NOT REACH, stated plainly because the gap is real:
			//
			// IT CLEARS the framework's activated (E) cooldown — the ring on the HUD, and the timer
			// UTraceAbilityComponent::TryActivate actually refuses on. That is the whole cooldown for
			// Chut, Rocco, X, Slimeball and Oyster.
			//
			// SINCE v24 §5 THIS RUNS EVERY FRAME (see Tick), and that is the fix rather than a tuning
			// change: on the 200 ms timer alone, the deadline TryActivate() writes on the press
			// survived for up to a fifth of a second and the ability was genuinely refused for it.
			//
			// IT DOES NOT CLEAR a character's OWN second timer (FTraceAbilityNetState::AuxEndMatchTime
			// — Elle's Snap window and 35 s pair, Roxie's rocket, Mace's hidden V). That field is
			// DUAL PURPOSE and its meaning is per-character: for Elle mid-cast it is the end of the
			// 4 s window, not a ready time, so zeroing it from out here would cancel a cast rather
			// than refresh it. Clearing those needs a hook on UTraceCharacterAbilitySet that this
			// pass does not own — see the report.
			//
			// DEV BUILDS ONLY. DebugSetActivatedCooldown is compiled out of Shipping, so in a
			// Shipping build this half of the toggle silently does nothing. Also in the report.
			Abilities->DebugSetActivatedCooldown(0.f);
		}
#endif

		// The dash pool is the other thing a player feels as "a cooldown". RefundDashCharge is the
		// shipped grant (it is what a successful parry pays out), it mirrors itself onto the owning
		// client so the HUD meter moves, and it returns false on a full pool — so this loop
		// terminates on its own and costs one call in the common case.
		if (const ATraceCharacter* PlayerPawn = Cast<ATraceCharacter>(EachState->GetPawn()))
		{
			if (UTraceCharacterMovementComponent* PlayerMove = PlayerPawn->GetTraceMovement())
			{
				const int32 MaxRefunds = FMath::Max(1, PlayerMove->GetMaxDashCharges());
				for (int32 Refund = 0; Refund < MaxRefunds; ++Refund)
				{
					if (!PlayerMove->RefundDashCharge())
					{
						break;
					}
				}
			}
		}
	}
}

bool UTracePracticeRangeSubsystem::ReopenCharacterSelect(APlayerState* Player)
{
	const UWorld* const WorldPtr = GetWorld();
	if (!TracePracticeRange::IsActive(WorldPtr))
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Practice] character switch refused: this is not the practice range."));
		return false;
	}

	ATracePlayerState* const TraceState = Cast<ATracePlayerState>(Player);
	if (TraceState == nullptr || TraceState->IsABot())
	{
		return false;
	}

	if (!UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Practice] character switch refused: characters are switched off for this session "
			     "(the settings toggle)."));
		return false;
	}

	if (TraceState->IsCharacterSelectOpen())
	{
		return false;   // already choosing; a second nudge would only reset their deadline
	}

	// THE SHIPPED SELECT SCREEN, REOPENED — not a second selection path.
	//
	// ATraceGameMode::PollCharacterSelect (4 Hz) opens a screen for any human on a team who has no
	// character. So the switch is: hand the character back, clear the lock, and let the shipped poll
	// notice. The player then picks on the real screen, ATracePlayerState::ServerRequestCharacter
	// runs the real per-team uniqueness rule, and ATraceGameMode::RequestCharacter re-locks them.
	//
	// Setting ETraceCharacterId::None is documented on ServerSetCharacter as always allowed and
	// never refused, which is what makes this an infallible first step rather than a request that
	// might bounce and leave the player locked to a character they have already lost.
	if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(TraceState))
	{
		Abilities->ServerSetCharacter(ETraceCharacterId::None);
	}
	TraceState->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);

	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] '%s' handed their character back; the select screen reopens on the next poll."),
		*TraceState->GetPlayerName());
	return true;
}

void UTracePracticeRangeSubsystem::SuppressScore()
{
	UWorld* const WorldPtr = GetWorld();
	ATraceGameState* const TraceState = (WorldPtr != nullptr) ? WorldPtr->GetGameState<ATraceGameState>() : nullptr;
	if (TraceState == nullptr)
	{
		return;
	}

	if (TraceState->BlueScore == 0 && TraceState->OrangeScore == 0)
	{
		return;
	}

	// THE RANGE KEEPS NO SCORE, and that is a safety rule rather than a design preference. A session
	// that banks points walks towards UTraceSettings::MercyRuleLead, and reaching it ends the match
	// and travels to the main menu — i.e. practising well would eject you from the range. The wipe
	// bonus is already off (ATracePracticeGameMode sets WipeBonusPoints = 0); this covers the other
	// way in, which is simply carrying the Core through the hoop eight times.
	TraceState->BlueScore = 0;
	TraceState->OrangeScore = 0;
	TraceState->ForceNetUpdate();
}

// =================================================================================================
// Queries
// =================================================================================================

AActor* UTracePracticeRangeSubsystem::FindRespawnPostFor(const AController* ForController) const
{
	const ATracePracticeDummyController* const DummyCtrl = Cast<ATracePracticeDummyController>(ForController);
	if (DummyCtrl == nullptr)
	{
		return nullptr;
	}

	return DummyCtrl->GetHomePost();
}

AActor* UTracePracticeRangeSubsystem::GetPlayerStartPost() const
{
	// DEMO 19 ITEM 1. Null until BuildRange has run, which is the honest answer during the couple of
	// seconds before the range is furnished — ATracePracticeGameMode::ChoosePlayerStart falls through
	// to the shipped endzone pipeline on null, and GatherPlayersIntoRange collects the player once
	// the post does exist.
	return PlayerStartPost.Get();
}

int32 UTracePracticeRangeSubsystem::GetDummyCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<ATracePracticeDummyController>& EachDummy : Dummies)
	{
		if (EachDummy.IsValid())
		{
			++Count;
		}
	}
	return Count;
}

int32 UTracePracticeRangeSubsystem::GetPadCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<ATracePracticePad>& EachPad : Pads)
	{
		if (EachPad.IsValid())
		{
			++Count;
		}
	}
	return Count;
}

ATraceCore* UTracePracticeRangeSubsystem::GetCore() const
{
	return ATraceCore::Get(GetWorld());
}

void UTracePracticeRangeSubsystem::RefreshInfiniteAbilitiesPad()
{
	for (const TWeakObjectPtr<ATracePracticePad>& EachPad : Pads)
	{
		ATracePracticePad* const PadActor = EachPad.Get();
		if (PadActor != nullptr && PadActor->GetPadRole() == ETracePracticePadRole::InfiniteAbilities)
		{
			PadActor->SetPadLabel(bInfiniteAbilities
				? TEXT("INFINITE ABILITIES: ON") : TEXT("INFINITE ABILITIES: OFF"));
			PadActor->SetPadLit(bInfiniteAbilities);
		}
	}
}

double UTracePracticeRangeSubsystem::GetPlayerDistanceToTargets() const
{
	const UWorld* const WorldPtr = GetWorld();
	const APlayerController* const FirstPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
	const APawn* const PlayerPawn = (FirstPC != nullptr) ? FirstPC->GetPawn() : nullptr;
	if (PlayerPawn == nullptr)
	{
		return -1.0;
	}

	double Nearest = -1.0;
	for (const TWeakObjectPtr<ATracePracticeDummyController>& EachDummy : Dummies)
	{
		const ATracePracticeDummyController* const DummyCtrl = EachDummy.Get();
		const APawn* const DummyPawn = (DummyCtrl != nullptr) ? DummyCtrl->GetPawn() : nullptr;
		if (DummyPawn == nullptr)
		{
			continue;
		}
		const double Distance = FVector::Dist2D(PlayerPawn->GetActorLocation(), DummyPawn->GetActorLocation());
		if (Nearest < 0.0 || Distance < Nearest)
		{
			Nearest = Distance;
		}
	}
	return Nearest;
}

void UTracePracticeRangeSubsystem::LogStatus() const
{
	const UWorld* const WorldPtr = GetWorld();
	const bool bActive = TracePracticeRange::IsActive(WorldPtr);

	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] gate=%s built=%s targets=%d pads=%d infiniteAbilities=%s coreOnRack=%s"),
		bActive ? TEXT("OPEN") : TEXT("CLOSED"),
		bBuilt ? TEXT("yes") : TEXT("no"),
		GetDummyCount(), GetPadCount(),
		bInfiniteAbilities ? TEXT("ON") : TEXT("OFF"),
		bCoreOnRack ? TEXT("yes") : TEXT("no"));

	// WHICH DRIVER IS REFRESHING THE CHEAT (spec v24 §5). A toggle reported as "does nothing" is a
	// question about the driver, so the driver is a printed fact rather than something to read the
	// source for: applies=<total>, of which <ticked> came from the per-frame tick. Poll-only means
	// every press really does start a cooldown for up to one poll — which was the bug.
	// The red-arm note is resolved BEFORE the log call, not inside its argument list. A #if between
	// a function-like macro's parentheses is undefined behaviour: clang accepts it silently, MSVC
	// rejects it (C5101, then C2760/C3553/C2059), so it compiles here and breaks every Windows
	// developer. Scripts/check-preprocessor-in-macro-args.py now fails the build on this pattern.
#if !UE_BUILD_SHIPPING
	const TCHAR* const RedArmNote = TracePracticeRange::IsInfinitePollOnlyArmed()
		? TEXT("   *** Trace.Practice.PollOnlyInfinite IS ON — this is the RED ARM ***")
		: TEXT("");
#else
	const TCHAR* const RedArmNote = TEXT("");
#endif

	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] infinite-abilities driver: applies=%d ticked=%d polled=%d%s"),
		InfiniteApplyCount, InfiniteTickApplyCount, InfiniteApplyCount - InfiniteTickApplyCount,
		RedArmNote);

	// DEMO 19 ITEM 1's measurement, printed rather than described. "Make it smaller" is a claim about
	// how far the player has to walk, so this is the number that has to move.
	const double ToTargets = GetPlayerDistanceToTargets();
	const APlayerController* const FirstPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
	const APawn* const PlayerPawn = (FirstPC != nullptr) ? FirstPC->GetPawn() : nullptr;
	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] the player is at %s, %.0f uu from the nearest target (the whole furnished "
		     "footprint is %.0f uu deep)."),
		(PlayerPawn != nullptr) ? *PlayerPawn->GetActorLocation().ToCompactString() : TEXT("<no pawn>"),
		ToTargets,
		TracePracticeRangeLocal::PlayerSpawnForwardUU + BuiltTargetStandbackUU);

#if !UE_BUILD_SHIPPING
	if (TracePracticeRange::IsLeakArmed())
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Practice] *** Trace.Practice.LeakArm IS ON. *** The range's gate is forced open in "
			     "every world, including a real match's. This is the red arm; set it back to 0."));
	}
#endif
}

// =================================================================================================
// Console surface
//
// The pads are the PLAYER's interface — walking onto a disc needs no key bind, which matters because
// this pass may not touch ATracePlayerController. These commands are for headless runs and for a
// player who would rather type; each one refuses outside the range through the same gate the pads
// use, so neither surface is a second way in.
// =================================================================================================

namespace TracePracticeRangeLocal
{
	UWorld* FindPracticeWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		UWorld* Fallback = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* const Candidate = Context.World();
			if (Candidate == nullptr || !Candidate->IsGameWorld())
			{
				continue;
			}

			// Prefer the world that actually has a range in it, so a PIE session with more than one
			// world answers the question the typist meant.
			if (TracePracticeRange::IsActive(Candidate))
			{
				return Candidate;
			}
			if (Fallback == nullptr)
			{
				Fallback = Candidate;
			}
		}
		return Fallback;
	}

	void CmdInfiniteAbilities(const TArray<FString>& Args)
	{
		UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(FindPracticeWorld());
		if (Range == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Practice] no world."));
			return;
		}

		const bool bWanted = (Args.Num() == 0) ? !Range->IsInfiniteAbilitiesOn() : (FCString::Atoi(*Args[0]) != 0);
		Range->SetInfiniteAbilities(bWanted);
	}

	void CmdSwitchCharacter()
	{
		UWorld* const WorldPtr = FindPracticeWorld();
		UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(WorldPtr);
		if (Range == nullptr || WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Practice] no world."));
			return;
		}

		const APlayerController* const LocalPC = WorldPtr->GetFirstPlayerController();
		APlayerState* const LocalState = (LocalPC != nullptr) ? LocalPC->PlayerState : nullptr;
		if (LocalState == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Practice] no local player state."));
			return;
		}

		Range->ReopenCharacterSelect(LocalState);
	}

	void CmdStatus()
	{
		if (const UTracePracticeRangeSubsystem* Range = UTracePracticeRangeSubsystem::Get(FindPracticeWorld()))
		{
			Range->LogStatus();
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Practice] no world."));
		}
	}

	FAutoConsoleCommand CmdPracticeInfinite(
		TEXT("Trace.Practice.InfiniteAbilities"),
		TEXT("SPEC v19 §2. The range's infinite-abilities toggle. No argument flips it; 0/1 sets it. "
		     "Refuses outside the practice range."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CmdInfiniteAbilities));

	FAutoConsoleCommand CmdPracticeSwitch(
		TEXT("Trace.Practice.SwitchCharacter"),
		TEXT("SPEC v19 §2. Reopens the character select screen for the local player without leaving "
		     "the range. Refuses outside the practice range."),
		FConsoleCommandDelegate::CreateStatic(&CmdSwitchCharacter));

	FAutoConsoleCommand CmdPracticeStatus(
		TEXT("Trace.Practice.Status"),
		TEXT("Prints the range's gate, its furniture and its toggles."),
		FConsoleCommandDelegate::CreateStatic(&CmdStatus));
}
