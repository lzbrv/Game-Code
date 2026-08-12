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

	/** Lateral spacing between the targets, in uu. */
	constexpr float DummySpacingY = 400.f;

	/** Where along the field the target row stands, as a fraction of the field length from centre. */
	constexpr float DummyRowFraction = 0.18f;

	/** How far back from centre, and how far out to the sides, the two toggle pads sit. */
	constexpr float TogglePadBackX = 900.f;
	constexpr float TogglePadOutY = 650.f;

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

	if (!bBuilt)
	{
		BuildRange();
	}

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

	// ---- the stationary targets ------------------------------------------------------------------
	//
	// A row across the field, on the far side of centre from the Blue endzone the solo player spawns
	// in, at a distance you would actually shoot from.
	const float RowX = FieldCentre.X + FieldSize.X * TracePracticeRangeLocal::DummyRowFraction;
	const float FirstY = FieldCentre.Y
		- TracePracticeRangeLocal::DummySpacingY * (TracePracticeRangeLocal::DummyCount - 1) * 0.5f;

	int32 SpawnedDummies = 0;
	for (int32 Index = 0; Index < TracePracticeRangeLocal::DummyCount; ++Index)
	{
		const FVector Where(RowX, FirstY + TracePracticeRangeLocal::DummySpacingY * Index, FieldCentre.Z);
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

	SpawnPad(ETracePracticePadRole::InfiniteAbilities,
		FVector(FieldCentre.X - TracePracticeRangeLocal::TogglePadBackX,
		        FieldCentre.Y - TracePracticeRangeLocal::TogglePadOutY,
		        FieldCentre.Z),
		TEXT("INFINITE ABILITIES: OFF"));

	SpawnPad(ETracePracticePadRole::CharacterSwap,
		FVector(FieldCentre.X - TracePracticeRangeLocal::TogglePadBackX,
		        FieldCentre.Y + TracePracticeRangeLocal::TogglePadOutY,
		        FieldCentre.Z),
		TEXT("CHANGE CHARACTER\nwalk on to reopen select"));

	bBuilt = true;
	bInfiniteAbilities = false;   // a range always opens with the cheat OFF
	bCoreOnRack = false;

	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] RANGE OPEN in the arena: %d/%d stationary targets, %d pads. "
		     "Core rack at %s. Infinite abilities OFF."),
		SpawnedDummies, TracePracticeRangeLocal::DummyCount, GetPadCount(), *RackPoint.ToCompactString());
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

	bBuilt = false;
	bInfiniteAbilities = false;
	bCoreOnRack = false;
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
	// and four seconds later the Core was in a player's hands 15864 uu away. The source is
	// ATraceGameMode::BeginHalf -> GrantCoreToTeam -> ATraceCore::KickoffTo(Blue) — the ordinary
	// start-of-half kickoff, which is entirely correct for a match and simply does not know that in
	// here the Core was put down on purpose.
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
			     "(mode A, or the settings toggle)."));
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
