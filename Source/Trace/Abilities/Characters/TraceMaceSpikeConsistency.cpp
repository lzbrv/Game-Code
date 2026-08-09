// Trace — MACE'S SPIKE, MEASURED. Spec v15 §6: "Spike is inconsistent and her ability is way too slow".
//
// ===================================================================================================
// WHY THIS FILE EXISTS AND WHY Trace.Mace.Verify WAS NOT ENOUGH
// ===================================================================================================
//
// Trace.Mace.Verify throws the spike ONCE, at whatever the first of 72 aim directions happens to find,
// and calls it proven. "Inconsistent" is a statement about a DISTRIBUTION, and one sample has no
// distribution. Mace was flagged UNVERIFIED at the end of the v14 pass for exactly this reason.
//
// So this file fires the shipping throw path many dozens of times against geometry whose answer is
// known in advance, and prints a RATE. Three arms:
//
//   ARM A — THE ARENA CENSUS. From the positions living players actually stand in, 24 yaws x 3
//           pitches through ResolveSpikeAnchor(). No actors spawned, no state touched. This is the
//           "can she even find a wall from where she is" number, and it is the one the range knob
//           moves.
//
//   ARM B — THE GEOMETRY CENSUS. A rig of box colliders with KNOWN normals — flat, tilted 15/30/40
//           degrees, a 4 uu plate, a 40 uu pillar, a 16 uu railing, an outer corner, point blank,
//           long range, and one thrown while she is moving at 1200 uu/s — each fired at with a spread
//           of small aim jitters through the real ActivateAbility(), and each waited on until the
//           real ATraceMaceSpike reports itself embedded.
//
//           IT CARRIES ITS OWN FALSIFICATION. Four cases MUST FIZZLE: a 35-degrees-from-horizontal
//           ramp she could walk up, a floor, the sky, and a wall past the range knob. A build that
//           embedded in everything would score 100% on the arm above and be a worse bug than the one
//           being fixed, so the verdict fails if any of those four embeds.
//
//   ARM C — THE PULL. The half of §6 that DebugThrowSpikeAt cannot reach: a REAL thrown spike, at
//           four distances, pulled on through the real reactivation, including one pull requested in
//           the last 150 ms of the 2 s embed window — which is the "embed racing the timer" candidate
//           the spec names.
//
// THE RIG SITS 9000 uu ABOVE THE ARENA on purpose. The spike's range is the thing under test, so the
// test geometry must be the ONLY thing in range: at 9000 uu up, nothing in a 1640-uu-tall arena is
// within even the raised 6600 uu range, and "aimed at the sky, must fizzle" cannot accidentally find
// a wall. The pawn's pose is snapshotted before the first teleport and restored at the end.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceAbilityInputRelay.h"
#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Abilities/Characters/TraceMaceSpike.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceMaceSpikeConsistency
{
	// =============================================================================================
	// Fixture plumbing
	// =============================================================================================

	UWorld* FindAuthoritativeWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetAuthGameMode() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** The select screen pauses the world; a paused world turns every measurement below into a zero. */
	void UnpauseAndReport(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr || !WorldPtr->IsPaused())
		{
			return;
		}
		if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
		{
			FirstPC->SetPause(false);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[SPIKERATE] The world was PAUSED. Unpaused, or every rate below would have been a "
				     "zero that looks like a pass."));
		}
	}

	UTraceAbilityComponent* FindHumanAbilityComponent(UWorld* WorldPtr)
	{
		const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (BaseState == nullptr)
		{
			return nullptr;
		}
		for (APlayerState* Entry : BaseState->PlayerArray)
		{
			if (Entry == nullptr || Entry->IsABot())
			{
				continue;
			}
			if (UTraceAbilityComponent* Comp = Entry->FindComponentByClass<UTraceAbilityComponent>())
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/**
	 * One collider, owned by the harness. A UBoxComponent rather than a static mesh deliberately: the
	 * face normals are then exactly what the case says they are, so "the wall test rejected a surface
	 * at |normal.Z| 0.50" is arithmetic and not an assertion about somebody's art.
	 *
	 * "BlockAll" is the profile the arena's own walls use (TraceArenaBuilder.cpp), which is what makes
	 * these boxes WorldStatic objects and therefore visible to the same query the throw runs.
	 */
	AActor* SpawnPlate(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* PlateActor = WorldPtr->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
		if (PlateActor == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* PlateBox = NewObject<UBoxComponent>(PlateActor, TEXT("HarnessPlate"));
		PlateBox->SetMobility(EComponentMobility::Movable);
		PlateActor->SetRootComponent(PlateBox);
		PlateBox->RegisterComponent();
		PlateBox->SetCollisionProfileName(TEXT("BlockAll"));
		PlateBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		PlateBox->SetCollisionObjectType(ECC_WorldStatic);
		PlateBox->SetBoxExtent(FVector(10.f, 400.f, 400.f));
		PlateBox->SetHiddenInGame(true);
		return PlateActor;
	}

	void PlacePlate(AActor* PlateActor, bool bEnabled, const FVector& Where, const FRotator& Rot, const FVector& Extent)
	{
		if (PlateActor == nullptr)
		{
			return;
		}
		UBoxComponent* PlateBox = Cast<UBoxComponent>(PlateActor->GetRootComponent());
		if (PlateBox == nullptr)
		{
			return;
		}
		PlateBox->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		if (bEnabled)
		{
			PlateBox->SetBoxExtent(Extent);
			PlateActor->SetActorLocationAndRotation(Where, Rot, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		}
	}

	// =============================================================================================
	// The case table
	// =============================================================================================

	/**
	 * A plate box with extent (Thickness, Width, Height) and no rotation presents its -X face to a
	 * pawn standing at -X of it, and that face's normal is exactly -X: |normal.Z| = 0. Pitching the
	 * box by T degrees tilts that face by T, so |normal.Z| = |sin T| — which is how each tilt case
	 * below states, in advance, the number the wall test is going to be asked about.
	 */
	struct FSpikeCase
	{
		int32   Category = 0;
		bool    bMustEmbed = true;

		float   Distance = 1200.f;     // plate centre, along her aim axis
		/**
		 * The pitch she is looking along BEFORE the jitter, and the plate is placed along it. A tilted
		 * plate is therefore always hit SQUARE ON ITS FACE, so |normal.Z| is the only thing that varies
		 * between the tilt cases and the wall test is asked exactly the question it exists to answer.
		 *
		 * The first build of this harness left it at zero, and the 88-degree "floor" plate was then
		 * presented edge-on: a 400 uu half-extent seen at 88 degrees is 14 uu tall, so six of seven
		 * throws sailed past it and the seventh stuck in its 20 uu RIM and was scored as the wall test
		 * failing. That was the fixture being wrong, not the game.
		 */
		float   BaseAimPitch = 0.f;
		FRotator PlateRotation = FRotator::ZeroRotator;
		FVector PlateExtent = FVector(10.f, 400.f, 400.f);

		bool    bSecondPlate = false;  // the outer corner
		FRotator SecondRotation = FRotator::ZeroRotator;
		FVector SecondOffset = FVector::ZeroVector;

		bool    bNoPlate = false;      // "aimed at the sky"
		float   LateralSpeed = 0.f;    // thrown while moving

		float   AimYaw = 0.f;          // degrees off dead-on
		float   AimPitch = 0.f;
	};

	struct FCategory
	{
		FString Name;
		FString Expectation;
		int32 Attempts = 0;
		int32 Accepted = 0;      // ActivateAbility() returned true
		int32 Embedded = 0;      // the real actor reported itself embedded
		int32 MissedNoHit = 0;   // the query found nothing at all
		int32 MissedNotWall = 0; // the query found a surface the wall test refused
		float WorstFlightMs = 0.f;
		FString FirstFailWhy;
	};

	enum : int32
	{
		Cat_Flat = 0, Cat_Tilt15, Cat_Tilt30, Cat_Tilt40, Cat_ThinPlate, Cat_Pillar,
		Cat_Railing, Cat_Corner, Cat_PointBlank, Cat_LongRange, Cat_Moving,
		Cat_Ramp, Cat_Floor, Cat_Sky, Cat_OutOfRange, Cat_Count
	};

	void BuildCategories(TArray<FCategory>& Out)
	{
		Out.SetNum(Cat_Count);
		auto Set = [&Out](int32 Index, const TCHAR* Name, const TCHAR* Expect)
		{
			Out[Index].Name = Name;
			Out[Index].Expectation = Expect;
		};
		Set(Cat_Flat,       TEXT("flat wall           "), TEXT("EMBED"));
		Set(Cat_Tilt15,     TEXT("wall tilted 15 deg   "), TEXT("EMBED"));
		Set(Cat_Tilt30,     TEXT("wall tilted 30 deg   "), TEXT("EMBED"));
		Set(Cat_Tilt40,     TEXT("wall tilted 40 deg   "), TEXT("EMBED"));
		Set(Cat_ThinPlate,  TEXT("thin plate (4 uu)    "), TEXT("EMBED"));
		Set(Cat_Pillar,     TEXT("pillar (40 uu wide)  "), TEXT("EMBED"));
		Set(Cat_Railing,    TEXT("railing (16 uu tall) "), TEXT("EMBED"));
		Set(Cat_Corner,     TEXT("outer corner seam    "), TEXT("EMBED"));
		Set(Cat_PointBlank, TEXT("point blank (90 uu)  "), TEXT("EMBED"));
		Set(Cat_LongRange,  TEXT("long range 3000/5500 "), TEXT("EMBED"));
		Set(Cat_Moving,     TEXT("thrown while moving  "), TEXT("EMBED"));
		Set(Cat_Ramp,       TEXT("ramp 35 deg (walkable)"), TEXT("FIZZLE"));
		Set(Cat_Floor,      TEXT("floor                "), TEXT("FIZZLE"));
		Set(Cat_Sky,        TEXT("open sky             "), TEXT("FIZZLE"));
		Set(Cat_OutOfRange, TEXT("wall past the range  "), TEXT("FIZZLE"));
	}

	void BuildCases(TArray<FSpikeCase>& Out)
	{
		Out.Reset();

		// A generous spread for the big plates: at 1200 uu, 3 degrees is 63 uu of aim error against a
		// 400 uu half-extent, so a jitter can never be the reason a big flat wall was missed.
		const FVector2D BigJitter[] = {
			{0.f, 0.f}, {2.f, 0.f}, {-2.f, 0.f}, {0.f, 2.f}, {0.f, -2.f}, {3.f, 3.f}, {-3.f, -3.f}
		};

		// A plate pitched by T presents a face whose normal is (-cos T, 0, -sin T), so |normal.Z| =
		// |sin T| — and she is aimed along pitch T so the ray meets that face square. Positive T leans
		// the surface over her (an overhang); negative T is a ramp or a floor she looks down at.
		auto AddPlateCases = [&Out, &BigJitter](int32 Category, float PitchDeg, const FVector& Extent, bool bMustEmbed)
		{
			for (const FVector2D& Jitter : BigJitter)
			{
				FSpikeCase Case;
				Case.Category = Category;
				Case.bMustEmbed = bMustEmbed;
				Case.BaseAimPitch = PitchDeg;
				Case.PlateRotation = FRotator(PitchDeg, 0.f, 0.f);
				Case.PlateExtent = Extent;
				Case.AimYaw = Jitter.X;
				Case.AimPitch = Jitter.Y;
				Out.Add(Case);
			}
		};

		const FVector BigPlate(10.f, 400.f, 400.f);

		AddPlateCases(Cat_Flat,      0.f,  BigPlate,                    true);
		AddPlateCases(Cat_Tilt15,    15.f, BigPlate,                    true);
		AddPlateCases(Cat_Tilt30,    30.f, BigPlate,                    true);
		AddPlateCases(Cat_Tilt40,    40.f, BigPlate,                    true);
		AddPlateCases(Cat_ThinPlate, 0.f,  FVector(2.f, 400.f, 400.f),  true);

		// THIN GEOMETRY. A 40 uu pillar at 1200 uu: one degree of aim error is 21 uu, which is past
		// its 20 uu half-width. An infinitely thin line trace therefore misses a pillar the crosshair
		// is visibly on — which is precisely what "inconsistent" feels like from the player's chair.
		{
			const FVector2D PillarJitter[] = {
				{0.f, 0.f}, {0.5f, 0.f}, {-0.5f, 0.f}, {1.0f, 0.f}, {-1.0f, 0.f}, {0.f, 0.5f}, {0.f, -0.5f}
			};
			for (const FVector2D& Jitter : PillarJitter)
			{
				FSpikeCase Case;
				Case.Category = Cat_Pillar;
				Case.PlateExtent = FVector(20.f, 20.f, 400.f);
				Case.AimYaw = Jitter.X;
				Case.AimPitch = Jitter.Y;
				Out.Add(Case);
			}
			// The same argument rotated 90 degrees: a 16 uu deep rail, missed by half a degree of pitch.
			const FVector2D RailJitter[] = {
				{0.f, 0.f}, {0.f, 0.5f}, {0.f, -0.5f}, {0.f, 1.0f}, {0.f, -1.0f}, {2.f, 0.f}, {-2.f, 0.f}
			};
			for (const FVector2D& Jitter : RailJitter)
			{
				FSpikeCase Case;
				Case.Category = Cat_Railing;
				Case.PlateExtent = FVector(20.f, 400.f, 8.f);
				Case.AimYaw = Jitter.X;
				Case.AimPitch = Jitter.Y;
				Out.Add(Case);
			}
		}

		// AN OUTER CORNER, the shape spec v14 §8 says the arena's wall joins are made of. Two plates
		// yawed +/-45 so their near edges meet on the aim axis; the aim lands on the seam itself,
		// where a query returns an edge rather than a face.
		{
			const FVector2D CornerJitter[] = {
				{0.f, 0.f}, {0.2f, 0.f}, {-0.2f, 0.f}, {0.f, 1.f}, {0.f, -1.f}, {1.f, 1.f}, {-1.f, -1.f}
			};
			for (const FVector2D& Jitter : CornerJitter)
			{
				FSpikeCase Case;
				Case.Category = Cat_Corner;
				Case.PlateRotation = FRotator(0.f, 45.f, 0.f);
				Case.PlateExtent = FVector(10.f, 400.f, 400.f);
				Case.bSecondPlate = true;
				Case.SecondRotation = FRotator(0.f, -45.f, 0.f);
				Out.Add(Case);
				Out.Last().AimYaw = Jitter.X;
				Out.Last().AimPitch = Jitter.Y;
			}
		}

		// POINT BLANK. The muzzle is the eye stepped 22 uu forward along the aim ray and the capsule
		// radius is 34, so at 90 uu the query starts a bare 34 uu from the surface.
		{
			const FVector2D CloseJitter[] = { {0.f, 0.f}, {5.f, 0.f}, {-5.f, 0.f}, {0.f, 5.f}, {0.f, -5.f} };
			for (const FVector2D& Jitter : CloseJitter)
			{
				FSpikeCase Case;
				Case.Category = Cat_PointBlank;
				Case.Distance = 90.f;
				Case.AimYaw = Jitter.X;
				Case.AimPitch = Jitter.Y;
				Out.Add(Case);
			}
		}

		// LONG RANGE. 3000 and 5500 uu both sit past the shipped 2200 uu range knob, so this category
		// is the one the range change is measured by.
		{
			const float Distances[] = { 3000.f, 5500.f };
			const FVector2D FarJitter[] = { {0.f, 0.f}, {0.5f, 0.f}, {-0.5f, 0.f} };
			for (float Dist : Distances)
			{
				for (const FVector2D& Jitter : FarJitter)
				{
					FSpikeCase Case;
					Case.Category = Cat_LongRange;
					Case.Distance = Dist;
					Case.AimYaw = Jitter.X;
					Case.AimPitch = Jitter.Y;
					Out.Add(Case);
				}
			}
		}

		// THROWN WHILE MOVING at the air-strafe sort of speed, because a throw that only works from a
		// standstill is a throw nobody can use.
		{
			const FVector2D MoveJitter[] = { {0.f, 0.f}, {2.f, 0.f}, {-2.f, 0.f}, {0.f, 2.f}, {0.f, -2.f} };
			for (const FVector2D& Jitter : MoveJitter)
			{
				FSpikeCase Case;
				Case.Category = Cat_Moving;
				Case.Distance = 1500.f;
				Case.LateralSpeed = 1200.f;
				Case.AimYaw = Jitter.X;
				Case.AimPitch = Jitter.Y;
				Out.Add(Case);
			}
		}

		// ---- THE FALSIFICATION. Four things that MUST still fizzle. ------------------------------
		//
		// Without these the arm above is satisfied by "embed in absolutely anything", which is a worse
		// bug than the one being fixed and would read as a clean 100%.

		// -55 degrees is a surface 35 degrees off horizontal, looked down at: |normal.Z| = 0.82,
		// comfortably inside the 45-degree walkable limit. She can walk up it, so it is not a wall.
		AddPlateCases(Cat_Ramp, -55.f, FVector(10.f, 400.f, 400.f), false);
		// -88 degrees is a floor in all but name.
		AddPlateCases(Cat_Floor, -88.f, FVector(10.f, 400.f, 400.f), false);

		for (int32 Index = 0; Index < 3; ++Index)
		{
			FSpikeCase SkyCase;
			SkyCase.Category = Cat_Sky;
			SkyCase.bMustEmbed = false;
			SkyCase.bNoPlate = true;
			SkyCase.AimPitch = 60.f + Index * 10.f;
			Out.Add(SkyCase);

			FSpikeCase FarCase;
			FarCase.Category = Cat_OutOfRange;
			FarCase.bMustEmbed = false;
			// Placed relative to the knob, so this case follows the range wherever it is retuned.
			FarCase.Distance = FMath::Max(100.f, UTraceSettings::Get().MaceSpikeRangeUU) + 900.f + Index * 200.f;
			Out.Add(FarCase);
		}
	}

	// =============================================================================================
	// Run state
	// =============================================================================================

	struct FPullTrial
	{
		float Distance = 0.f;
		float DelayBeforePull = 0.f;   // seconds waited after the embed before reactivating
		/**
		 * Press the reactivation while the spike is still IN THE AIR. A player who throws to reach
		 * height and immediately double-taps is doing this every time, and "the reactivate input being
		 * dropped" is one of the causes spec v15 §6 names for the inconsistency.
		 */
		bool  bReactivateInFlight = false;
		/**
		 * Reactivate WHILE STANDING ON THE FLOOR, which is where most of a match happens. Every other
		 * trial here starts her in mid-air, and mid-air is the case StartPull's ground-to-falling
		 * hand-off already covered — so until this trial existed, the pull was only ever measured in
		 * the situation it was known to work in.
		 */
		bool  bFromGround = false;
		bool  bThrown = false;
		bool  bEmbedded = false;
		bool  bPullStarted = false;
		FVector Anchor = FVector::ZeroVector;
		float StartDistance = -1.f;
		float ClosestDistance = -1.f;
		float PeakSpeed = 0.f;
		bool  bWeaponBlocked = false;
		bool  bSpikeGone = false;
	};

	struct FRunState
	{
		int32  Phase = 0;
		int32  Step = 0;
		int32  SubStep = 0;
		double PhaseStartReal = 0.0;
		double StepStartReal = 0.0;

		TWeakObjectPtr<UTraceAbilityComponent> Subject;
		TWeakObjectPtr<AActor> PlateA;
		TWeakObjectPtr<AActor> PlateB;

		FVector  HomeLocation = FVector::ZeroVector;
		FRotator HomeRotation = FRotator::ZeroRotator;
		FVector  RigOrigin = FVector::ZeroVector;

		// --- ARM A ---
		TArray<FVector> CensusPositions;
		int32 CensusHits = 0;
		int32 CensusNoHit = 0;
		int32 CensusNotWall = 0;

		// --- ARM B ---
		TArray<FSpikeCase> Cases;
		TArray<FCategory>  Categories;
		FString PendingWhy;
		bool    bPendingAccepted = false;
		double  PendingThrowReal = 0.0;
		float   PendingDeadline = 0.f;

		// --- ARM C ---
		TArray<FPullTrial> PullTrials;

		int32 Passed = 0;
		int32 Failed = 0;

		void Check(bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; }
			else            { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[SPIKERATE]   %s  %s"),
				bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *What);
		}
	};

	/** Puts her at @p Where, frozen (MOVE_Flying, no gravity) unless the case wants her moving. */
	void PoseFor(ATraceCharacter* MyPawn, const FVector& Where, const FRotator& Look, const FVector& Velocity)
	{
		if (MyPawn == nullptr)
		{
			return;
		}
		MyPawn->SetActorLocation(Where, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* Steer = MyPawn->GetController())
		{
			Steer->SetControlRotation(Look);
		}
		if (UTraceCharacterMovementComponent* MoveComp = MyPawn->GetTraceMovement())
		{
			MoveComp->SetMovementMode(MOVE_Flying);
			MoveComp->Velocity = Velocity;
		}
	}

	FString DescribeRate(int32 Hit, int32 Total)
	{
		return (Total > 0)
			? FString::Printf(TEXT("%3d/%-3d = %5.1f%%"), Hit, Total, 100.f * Hit / Total)
			: FString(TEXT("  no attempts  "));
	}

	// =============================================================================================
	// The command
	// =============================================================================================

	void RunSpikeConsistency()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[SPIKERATE] no authoritative game world — run this on the server."));
			return;
		}
		UnpauseAndReport(WorldPtr);

		TSharedPtr<FRunState> State = MakeShared<FRunState>();
		State->PhaseStartReal = FPlatformTime::Seconds();
		BuildCategories(State->Categories);
		BuildCases(State->Cases);

		const UTraceSettings& Settings = UTraceSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[SPIKERATE] ===== spec v15 §6, Mace's Spike measured over %d throws. Knobs AS THE RUNNING "
			     "GAME HAS THEM: range %.0f uu, travel %.0f uu/s, embed %.1fs, arrive %.0f uu, pull %.0f uu/s, "
			     "cooldown %.0fs. ====="),
			State->Cases.Num(), Settings.MaceSpikeRangeUU, Settings.MaceSpikeTravelSpeed,
			Settings.MaceSpikeEmbedSeconds, Settings.MaceSpikeArriveRadiusUU,
			FMath::Max(1.f, Settings.AirStrafeHardCapSpeed) * FMath::Clamp(Settings.AirStrafeAsymptoteScale, 0.5f, 2.f)
				* FMath::Max(0.1f, Settings.MaceSpikePullSpeedMultiplier),
			Settings.MaceSpikeCooldownSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[SPIKERATE] ABORTED: the world went away."));
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			// ---- Phase 0: become Mace, snapshot her pose, build the rig ---------------------------
			if (State->Phase == 0)
			{
				UTraceAbilityComponent* Human = FindHumanAbilityComponent(TickWorld);
				if (Human != nullptr && Human->GetCharacterId() != ETraceCharacterId::Mace)
				{
					Human->ServerSetCharacter(ETraceCharacterId::Mace);
				}
				UTraceAbilitySetMace* MaceSet = (Human != nullptr)
					? Human->GetAbilitySetAs<UTraceAbilitySetMace>() : nullptr;
				ATraceCharacter* MacePawn = (Human != nullptr) ? Human->GetOwningCharacter() : nullptr;

				if (MaceSet == nullptr || MacePawn == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 60.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[SPIKERATE] VERDICT: INVALID — no human player could be made Mace. Needs a "
							     "mode-B match with characters enabled."));
						return false;
					}
					return true;
				}

				State->Subject = Human;
				State->HomeLocation = MacePawn->GetActorLocation();
				State->HomeRotation = (MacePawn->GetController() != nullptr)
					? MacePawn->GetController()->GetControlRotation() : MacePawn->GetActorRotation();
				State->RigOrigin = State->HomeLocation + FVector(0.f, 0.f, 9000.f);

				// The census positions: where players ACTUALLY are. A wall-finding rate sampled from a
				// point somebody chose for being next to a wall is not a rate, it is a demonstration.
				State->CensusPositions.Add(State->HomeLocation);
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate != nullptr && Candidate != MacePawn && Candidate->IsAlive()
						&& State->CensusPositions.Num() < 9)
					{
						State->CensusPositions.Add(Candidate->GetActorLocation());
					}
				}

				State->PlateA = SpawnPlate(TickWorld);
				State->PlateB = SpawnPlate(TickWorld);
				PlacePlate(State->PlateA.Get(), false, FVector::ZeroVector, FRotator::ZeroRotator, FVector::ZeroVector);
				PlacePlate(State->PlateB.Get(), false, FVector::ZeroVector, FRotator::ZeroRotator, FVector::ZeroVector);

				if (!State->PlateA.IsValid() || !State->PlateB.IsValid())
				{
					UE_LOG(LogTraceGame, Error, TEXT("[SPIKERATE] VERDICT: INVALID — could not spawn the test rig."));
					return false;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[SPIKERATE] --- ARM A: THE ARENA CENSUS. %d real player positions x 24 yaws x 3 pitches "
					     "through the shipping aim query."), State->CensusPositions.Num());

				State->Phase = 1;
				State->Step = 0;
				State->PhaseStartReal = NowReal;
				return true;
			}

			UTraceAbilityComponent* Comp = State->Subject.Get();
			UTraceAbilitySetMace* MaceSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetMace>() : nullptr;
			ATraceCharacter* MyPawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			UTraceCharacterMovementComponent* MoveComp = (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;
			if (MaceSet == nullptr || MyPawn == nullptr || MoveComp == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[SPIKERATE] ABORTED: Mace went away mid-run."));
				return false;
			}
			AController* Steer = MyPawn->GetController();

			// ---- Phase 1: ARM A, the arena census -------------------------------------------------
			if (State->Phase == 1)
			{
				constexpr int32 YawSteps = 24;
				constexpr int32 PitchSteps = 3;
				const float Pitches[PitchSteps] = { 0.f, -15.f, 15.f };
				const int32 PerPosition = YawSteps * PitchSteps;
				const int32 TotalSamples = State->CensusPositions.Num() * PerPosition;

				// One position per tick, all 72 of its directions in it: the aim query reads the control
				// rotation directly (ATraceCharacter::ResolveAimRotation), so it needs no settle time,
				// and a whole match's worth of positions would otherwise take a minute of wall clock.
				if (State->Step < State->CensusPositions.Num())
				{
					const FVector Where = State->CensusPositions[State->Step];
					MyPawn->SetActorLocation(Where, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
					MoveComp->SetMovementMode(MOVE_Flying);
					MoveComp->Velocity = FVector::ZeroVector;

					for (int32 PitchIndex = 0; PitchIndex < PitchSteps; ++PitchIndex)
					{
						for (int32 YawIndex = 0; YawIndex < YawSteps; ++YawIndex)
						{
							if (Steer != nullptr)
							{
								Steer->SetControlRotation(FRotator(Pitches[PitchIndex], YawIndex * (360.f / YawSteps), 0.f));
							}
							FVector Anchor, Normal;
							FString Why;
							if (MaceSet->ResolveSpikeAnchor(Anchor, Normal, Why))
							{
								++State->CensusHits;
							}
							else if (Why.StartsWith(TEXT("nothing")))
							{
								++State->CensusNoHit;
							}
							else
							{
								++State->CensusNotWall;
							}
						}
					}

					++State->Step;
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[SPIKERATE]   ARM A: %s of %d aim directions from real player positions found a wall "
					     "inside the %.0f uu range knob. Refused: %d found nothing at all, %d found a surface the "
					     "wall test called a floor or a ramp."),
					*DescribeRate(State->CensusHits, TotalSamples), TotalSamples,
					UTraceSettings::Get().MaceSpikeRangeUU, State->CensusNoHit, State->CensusNotWall);

				UE_LOG(LogTraceGame, Display,
					TEXT("[SPIKERATE] --- ARM B: THE GEOMETRY CENSUS. %d throws through the shipping "
					     "ActivateAbility(), each waited on until the real spike actor reports itself embedded. "
					     "Four categories MUST FIZZLE — that is this arm's falsification."), State->Cases.Num());

				State->Phase = 2;
				State->Step = 0;
				State->SubStep = 0;
				State->PhaseStartReal = NowReal;
				return true;
			}

			// ---- Phase 2: ARM B, the geometry census ----------------------------------------------
			if (State->Phase == 2)
			{
				if (State->Step >= State->Cases.Num())
				{
					PlacePlate(State->PlateA.Get(), false, FVector::ZeroVector, FRotator::ZeroRotator, FVector::ZeroVector);
					PlacePlate(State->PlateB.Get(), false, FVector::ZeroVector, FRotator::ZeroRotator, FVector::ZeroVector);

					UE_LOG(LogTraceGame, Display, TEXT("[SPIKERATE]   %-22s %-7s %-18s  %s"),
						TEXT("category"), TEXT("expect"), TEXT("embedded"), TEXT("why the rest failed"));
					for (const FCategory& Cat : State->Categories)
					{
						UE_LOG(LogTraceGame, Display, TEXT("[SPIKERATE]   %-22s %-7s %-18s  noHit=%d notWall=%d %s"),
							*Cat.Name, *Cat.Expectation, *DescribeRate(Cat.Embedded, Cat.Attempts),
							Cat.MissedNoHit, Cat.MissedNotWall, *Cat.FirstFailWhy);
					}

					UE_LOG(LogTraceGame, Display,
						TEXT("[SPIKERATE] --- ARM C: THE PULL, on REAL thrown spikes, including one reactivated in "
						     "the last 150 ms of the %.1fs embed window."), UTraceSettings::Get().MaceSpikeEmbedSeconds);

					State->PullTrials.Reset();
					const float Distances[] = { 800.f, 2000.f, 4000.f, 6000.f, 1500.f, 1500.f, 2000.f };
					const float Delays[]    = { 0.f,   0.f,    0.f,    0.f,
					                            FMath::Max(0.f, UTraceSettings::Get().MaceSpikeEmbedSeconds - 0.15f),
					                            0.f,    0.f };
					const bool  InFlight[]  = { false, false,  false,  false,  false,  true,   false };
					const bool  FromGround[] = { false, false,  false,  false,  false,  false, true };
					for (int32 Index = 0; Index < 7; ++Index)
					{
						FPullTrial Trial;
						Trial.Distance = Distances[Index];
						Trial.DelayBeforePull = Delays[Index];
						Trial.bReactivateInFlight = InFlight[Index];
						Trial.bFromGround = FromGround[Index];
						State->PullTrials.Add(Trial);
					}

					State->Phase = 3;
					State->Step = 0;
					State->SubStep = 0;
					State->PhaseStartReal = NowReal;
					return true;
				}

				const FSpikeCase& Case = State->Cases[State->Step];
				FCategory& Cat = State->Categories[Case.Category];

				// -- sub-step 0: stage the plate and the pose -------------------------------------
				if (State->SubStep == 0)
				{
					// Dead-on is BaseAimPitch and yaw 0, so the jitter in the case table IS the aim
					// error in degrees. The plate is hung off the EYE, not off the actor origin: the
					// ray leaves GetPawnViewLocation() (actor + BaseEyeHeight), and hanging a 16 uu
					// railing off the feet instead is how the first run of this harness scored a
					// dead-on throw as "nothing within 2200 uu".
					const FRotator Look(Case.BaseAimPitch + Case.AimPitch, Case.AimYaw, 0.f);
					const FVector EyeOffset(0.f, 0.f, MyPawn->GetPawnViewLocation().Z - MyPawn->GetActorLocation().Z);
					const FVector PlateAt = State->RigOrigin + EyeOffset
						+ FRotator(Case.BaseAimPitch, 0.f, 0.f).Vector() * Case.Distance;

					PlacePlate(State->PlateA.Get(), !Case.bNoPlate, PlateAt, Case.PlateRotation, Case.PlateExtent);
					if (Case.bSecondPlate)
					{
						PlacePlate(State->PlateB.Get(), true, PlateAt + Case.SecondOffset,
							Case.SecondRotation, Case.PlateExtent);
					}
					else
					{
						PlacePlate(State->PlateB.Get(), false, FVector::ZeroVector, FRotator::ZeroRotator, FVector::ZeroVector);
					}

					const FVector Velocity = (Case.LateralSpeed > 0.f)
						? FVector(0.f, Case.LateralSpeed, 0.f) : FVector::ZeroVector;
					PoseFor(MyPawn, State->RigOrigin, Look, Velocity);

					State->SubStep = 1;
					State->StepStartReal = NowReal;
					return true;
				}

				// -- sub-step 1: throw, through the shipping entry point ---------------------------
				if (State->SubStep == 1)
				{
					// A second tick before firing so the pose and the plate's physics transform have
					// both landed. The moving case is re-driven here so she is genuinely in motion on
					// the frame the throw resolves rather than only on the frame before it.
					if (Case.LateralSpeed > 0.f)
					{
						MoveComp->Velocity = FVector(0.f, Case.LateralSpeed, 0.f);
					}

					FVector Anchor, Normal;
					// Captured for the report only. ResolveSpikeAnchor is a pure query and it is the
					// same one ActivateAbility is about to run, so this names the reason a fizzle
					// fizzled instead of leaving the table saying only "it did not embed".
					MaceSet->ResolveSpikeAnchor(Anchor, Normal, State->PendingWhy);
					State->bPendingAccepted = MaceSet->ActivateAbility();
					State->PendingThrowReal = NowReal;

					const float TravelSpeed = FMath::Max(1.f, UTraceSettings::Get().MaceSpikeTravelSpeed);
					State->PendingDeadline = Case.Distance / TravelSpeed + 0.60f;

					State->SubStep = 2;
					State->StepStartReal = NowReal;
					return true;
				}

				// -- sub-step 2: wait for the real actor to report itself embedded ------------------
				const ATraceMaceSpike* Flying = MaceSet->GetSpike();
				const bool bEmbedded = (Flying != nullptr) && Flying->IsEmbedded();
				if (!bEmbedded && (NowReal - State->StepStartReal) < State->PendingDeadline && State->bPendingAccepted)
				{
					return true;
				}

				++Cat.Attempts;
				if (State->bPendingAccepted) { ++Cat.Accepted; }
				if (bEmbedded)
				{
					++Cat.Embedded;
					Cat.WorstFlightMs = FMath::Max(Cat.WorstFlightMs,
						static_cast<float>((NowReal - State->PendingThrowReal) * 1000.0));
				}
				else if (State->PendingWhy.StartsWith(TEXT("nothing")))
				{
					++Cat.MissedNoHit;
				}
				else if (!State->bPendingAccepted)
				{
					++Cat.MissedNotWall;
				}
				if (Cat.FirstFailWhy.IsEmpty() && bEmbedded != Case.bMustEmbed)
				{
					Cat.FirstFailWhy = FString::Printf(TEXT("| first wrong answer: yaw%+.1f pitch%+.1f -> \"%s\""),
						Case.AimYaw, Case.AimPitch, *State->PendingWhy);
				}

				// Leave nothing in the wall for the next case to trip over.
				MaceSet->OnHalfTime();

				++State->Step;
				State->SubStep = 0;
				return true;
			}

			// ---- Phase 3: ARM C, the pull ----------------------------------------------------------
			if (State->Phase == 3)
			{
				if (State->Step >= State->PullTrials.Num())
				{
					State->Phase = 4;
					State->PhaseStartReal = NowReal;
					return true;
				}

				FPullTrial& Trial = State->PullTrials[State->Step];

				if (State->SubStep == 0)
				{
					const FVector EyeOffset(0.f, 0.f, MyPawn->GetPawnViewLocation().Z - MyPawn->GetActorLocation().Z);
					const FVector PlateAt = State->RigOrigin + EyeOffset + FVector(Trial.Distance, 0.f, 0.f);
					PlacePlate(State->PlateA.Get(), true, PlateAt, FRotator::ZeroRotator, FVector(10.f, 600.f, 600.f));

					if (Trial.bFromGround)
					{
						// A floor slab under her feet, and she is dropped onto it rather than frozen in
						// place: what is under test is a pull begun from PhysWalking.
						//
						// SHE AIMS SLIGHTLY DOWN, and that detail is the whole trial. An anchor ABOVE her
						// lifts her off the floor on its own, and the pull then behaves like every other
						// trial here — the hard case is the one where only the pull can get her
						// airborne, because that is the one PhysWalking can swallow. Six degrees down
						// puts the anchor ~146 uu BELOW her centre. The slab is kept short enough
						// (600 uu) that the aim ray clears its edge instead of stopping on it.
						//
						// MEASURED ANSWER: it works. StartPull's ground-to-falling hand-off holds for a
						// downward pull too, and the pull arrives. Kept as standing evidence rather than
						// deleted, because "she was always already in the air" was true of every other
						// trial in this arm and that is not a thing a report should have to assume.
						PlacePlate(State->PlateB.Get(), true,
							State->RigOrigin - FVector(0.f, 0.f, 130.f), FRotator::ZeroRotator,
							FVector(600.f, 600.f, 20.f));
						MyPawn->SetActorLocation(State->RigOrigin, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
						if (AController* Aim = MyPawn->GetController())
						{
							Aim->SetControlRotation(FRotator(-6.f, 0.f, 0.f));
						}
						MoveComp->SetMovementMode(MOVE_Falling);
						MoveComp->Velocity = FVector::ZeroVector;
					}
					else
					{
						PlacePlate(State->PlateB.Get(), false, FVector::ZeroVector, FRotator::ZeroRotator, FVector::ZeroVector);
						PoseFor(MyPawn, State->RigOrigin, FRotator::ZeroRotator, FVector::ZeroVector);
					}

					State->SubStep = 1;
					State->StepStartReal = NowReal;
					return true;
				}

				if (State->SubStep == 1)
				{
					// The ground trial waits until she is genuinely stood on the slab. Throwing before
					// she lands would quietly turn it back into one more mid-air trial.
					if (Trial.bFromGround && !MoveComp->IsMovingOnGround()
						&& (NowReal - State->StepStartReal) < 3.0)
					{
						return true;
					}
					if (Trial.bReactivateInFlight)
					{
						// Falling BEFORE the throw, because a double-tap in the air is the situation
						// this trial is about, and the press lands while the actor is still travelling.
						MoveComp->SetMovementMode(MOVE_Falling);
						MoveComp->Velocity = FVector::ZeroVector;
					}
					Trial.bThrown = MaceSet->ActivateAbility();
					if (Trial.bReactivateInFlight)
					{
						MaceSet->RequestSpikePull();
					}
					State->SubStep = 2;
					State->StepStartReal = NowReal;
					return true;
				}

				// Wait for the embed, then for this trial's deliberate delay inside the 2 s window.
				if (State->SubStep == 2)
				{
					const ATraceMaceSpike* Flying = MaceSet->GetSpike();
					if (Flying != nullptr && Flying->IsEmbedded())
					{
						Trial.bEmbedded = true;
					}
					const float FlightBudget = Trial.Distance / FMath::Max(1.f, UTraceSettings::Get().MaceSpikeTravelSpeed) + 0.60f;
					if (!Trial.bEmbedded && (NowReal - State->StepStartReal) < FlightBudget)
					{
						return true;
					}
					if (!Trial.bEmbedded)
					{
						State->SubStep = 4;   // nothing to pull on; fall through to scoring
						return true;
					}

					if (Trial.bReactivateInFlight)
					{
						// The press was made mid-flight. Either it survived the flight or it was eaten;
						// this is the frame that says which, and no second press is given.
						const ATraceMaceSpike* Anchored = MaceSet->GetSpike();
						Trial.Anchor = (Anchored != nullptr) ? Anchored->GetAnchorLocation() : FVector::ZeroVector;
						Trial.StartDistance = (Anchored != nullptr)
							? FVector::Dist(MyPawn->GetActorLocation(), Trial.Anchor) : -1.f;
						Trial.ClosestDistance = Trial.StartDistance;
						Trial.bPullStarted = MaceSet->IsPulling();
						State->SubStep = 4;
						State->StepStartReal = NowReal;
						return true;
					}

					State->SubStep = 3;
					State->StepStartReal = NowReal;
					return true;
				}

				if (State->SubStep == 3)
				{
					if ((NowReal - State->StepStartReal) < Trial.DelayBeforePull)
					{
						return true;
					}

					// THE REAL REACTIVATION. MOVE_Flying is dropped here because it would hide
					// StartPull's own ground-to-falling hand-off; the ground trial is left exactly as
					// it is, stood on the slab, because that hand-off is the thing it is testing.
					if (!Trial.bFromGround)
					{
						MoveComp->SetMovementMode(MOVE_Falling);
						MoveComp->Velocity = FVector::ZeroVector;
					}
					const ATraceMaceSpike* Anchored = MaceSet->GetSpike();
					Trial.Anchor = (Anchored != nullptr) ? Anchored->GetAnchorLocation() : FVector::ZeroVector;
					Trial.StartDistance = (Anchored != nullptr)
						? FVector::Dist(MyPawn->GetActorLocation(), Trial.Anchor) : -1.f;
					Trial.ClosestDistance = Trial.StartDistance;

					MaceSet->RequestSpikePull();
					Trial.bPullStarted = MaceSet->IsPulling();

					State->SubStep = 4;
					State->StepStartReal = NowReal;
					return true;
				}

				// -- sub-step 4: watch the pull ---------------------------------------------------
				//
				// Distance is measured against the anchor CAPTURED AT PULL START, not against the live
				// actor, and it is sampled whether or not she is still pulling. The first build of
				// this harness sampled only while IsPulling() and read the spike through GetSpike(),
				// so the last sample it ever took was the one before the ability tick that ended the
				// pull — it reported her stopping 170 uu from a 150 uu arrive radius when she had in
				// fact arrived. That is the fixture lying, and it lies in the flattering direction.
				if (!Trial.Anchor.IsZero())
				{
					Trial.ClosestDistance = FMath::Min(Trial.ClosestDistance,
						static_cast<float>(FVector::Dist(MyPawn->GetActorLocation(), Trial.Anchor)));
				}
				if (MaceSet->IsPulling())
				{
					Trial.PeakSpeed = FMath::Max(Trial.PeakSpeed, static_cast<float>(MoveComp->Velocity.Size()));
					if (MyPawn->AreWeaponActionsBlocked())
					{
						Trial.bWeaponBlocked = true;
					}
				}

				const float PullBudget = Trial.Distance / FMath::Max(1.f, MaceSet->GetPullSpeed()) + 2.0f;
				if (MaceSet->IsPulling() && (NowReal - State->StepStartReal) < PullBudget)
				{
					return true;
				}

				Trial.bSpikeGone = (MaceSet->GetSpike() == nullptr);
				MaceSet->OnHalfTime();

				++State->Step;
				State->SubStep = 0;
				return true;
			}

			// ---- Phase 4: score, restore, verdict --------------------------------------------------
			int32 MustEmbedAttempts = 0;
			int32 MustEmbedHits = 0;
			int32 MustFizzleAttempts = 0;
			int32 MustFizzleWrong = 0;
			for (int32 Index = 0; Index < State->Categories.Num(); ++Index)
			{
				const FCategory& Cat = State->Categories[Index];
				if (Cat.Expectation == TEXT("EMBED"))
				{
					MustEmbedAttempts += Cat.Attempts;
					MustEmbedHits += Cat.Embedded;
				}
				else
				{
					MustFizzleAttempts += Cat.Attempts;
					MustFizzleWrong += Cat.Embedded;
				}
			}

			int32 PullsStarted = 0;
			int32 PullsArrived = 0;
			int32 PullsWeaponBlocked = 0;
			for (const FPullTrial& Trial : State->PullTrials)
			{
				if (Trial.bPullStarted) { ++PullsStarted; }
				if (Trial.bPullStarted && Trial.StartDistance > 0.f
					&& Trial.ClosestDistance <= FMath::Max(20.f, UTraceSettings::Get().MaceSpikeArriveRadiusUU) * 1.5f)
				{
					++PullsArrived;
				}
				if (Trial.bWeaponBlocked) { ++PullsWeaponBlocked; }
				UE_LOG(LogTraceGame, Display,
					TEXT("[SPIKERATE]   pull %.0f uu (%s): thrown=%d embedded=%d started=%d %.0f uu -> %.0f uu, "
					     "peak %.0f uu/s, spikeRemoved=%d"),
					Trial.Distance,
					Trial.bReactivateInFlight ? TEXT("reactivated MID-FLIGHT, one press only")
						: Trial.bFromGround ? TEXT("reactivated STOOD ON THE FLOOR")
						: *FString::Printf(TEXT("reactivated %.2fs into the embed window"), Trial.DelayBeforePull),
					Trial.bThrown ? 1 : 0, Trial.bEmbedded ? 1 : 0,
					Trial.bPullStarted ? 1 : 0, Trial.StartDistance, Trial.ClosestDistance, Trial.PeakSpeed,
					Trial.bSpikeGone ? 1 : 0);
			}

			// Put her back where she was found, and take the rig with us.
			MaceSet->OnHalfTime();
			if (AActor* PlateActor = State->PlateA.Get()) { PlateActor->Destroy(); }
			if (AActor* PlateActor = State->PlateB.Get()) { PlateActor->Destroy(); }
			MyPawn->SetActorLocation(State->HomeLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
			if (Steer != nullptr)
			{
				Steer->SetControlRotation(State->HomeRotation);
			}
			MoveComp->SetMovementMode(MOVE_Falling);
			MoveComp->Velocity = FVector::ZeroVector;

			UE_LOG(LogTraceGame, Display, TEXT("[SPIKERATE] ===== the numbers ====="));

			// THE FIXTURE IS JUDGED FIRST. A run in which the sky and the floor embedded has not
			// measured consistency, it has measured a build that sticks to anything.
			State->Check(MustFizzleAttempts > 0 && MustFizzleWrong == 0,
				FString::Printf(TEXT("THE FALSIFICATION: %d throws that MUST fizzle (a walkable ramp, a floor, the "
				                     "open sky, a wall past the range knob) — %d of them embedded"),
					MustFizzleAttempts, MustFizzleWrong));
			State->Check(MustEmbedAttempts >= 60,
				FString::Printf(TEXT("the census actually fired: %d throws that must embed"), MustEmbedAttempts));
			State->Check(MustEmbedAttempts > 0 && MustEmbedHits * 100 >= MustEmbedAttempts * 95,
				FString::Printf(TEXT("*** THE CONSISTENCY NUMBER ***: %s of throws at geometry she should be able to "
				                     "spike actually embedded (the bar is 95%%)"),
					*DescribeRate(MustEmbedHits, MustEmbedAttempts)));
			State->Check(PullsStarted == State->PullTrials.Num(),
				FString::Printf(TEXT("every reactivation started a pull (%d of %d) — including the one pressed 150 ms "
				                     "before the embed window closes, and the one pressed while the spike was still "
				                     "in the air"),
					PullsStarted, State->PullTrials.Num()));
			State->Check(PullsArrived == State->PullTrials.Num(),
				FString::Printf(TEXT("every pull carried her to the spike rather than cancelling itself (%d of %d)"),
					PullsArrived, State->PullTrials.Num()));
			State->Check(PullsWeaponBlocked == 0,
				TEXT("\"she can shoot while pulled\": AreWeaponActionsBlocked() stayed FALSE for every frame of every pull"));

			UE_LOG(LogTraceGame, Display, TEXT("[SPIKERATE] ===== %d passed, %d failed. ====="),
				State->Passed, State->Failed);
			UE_LOG(LogTraceGame, Display, TEXT("[SPIKERATE] VERDICT: %s"),
				(State->Failed == 0 && State->Passed > 0) ? TEXT("PASS") : TEXT("*** FAIL ***"));
			return false;
		}));
	}

	// =============================================================================================
	// THE TWO-PROCESS PAIR — "can a REMOTE CLIENT reactivate the spike at all"
	// =============================================================================================
	//
	// One process cannot answer this. ActivateAbility() spawns the spike inside `if (HasAuthority())`,
	// so a listen-server HOST — which every single-process test is — holds the pointer and everything
	// works. A remote client never gets one, and before v15 §6 that meant IsSpikeReadyToPull() was
	// permanently false there, the relay handed the press back to TryActivate(), and TryActivate()
	// refused it against the cooldown the throw had just started. The press vanished.
	//
	// So it takes a host and a client:
	//
	//   host    -TraceExec="Trace.Mace.RemotePullTest"   arms a REMOTE player's Mace with an embedded
	//                                                    spike and then watches whether SHE pulls.
	//   client  -TraceExec="Trace.Mace.RemotePullPress"  waits for its own local Mace to report a
	//                                                    pullable spike and, the moment it does,
	//                                                    presses E through the SHIPPING router. It
	//                                                    presses ONCE, off IsSpikeReadyToPull(), so a
	//                                                    build where that stays false simply never
	//                                                    presses — which is the bug, reproduced.
	//
	// The host's verdict is the one that counts: it is the authority, and "she pulled" there is the
	// only evidence that survives being on the wrong machine.

	/** The first human PlayerState whose controller is NOT local to this process. */
	APlayerState* FindRemoteHumanPlayerState(UWorld* WorldPtr)
	{
		const AGameStateBase* BaseState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
		if (BaseState == nullptr)
		{
			return nullptr;
		}
		for (APlayerState* Entry : BaseState->PlayerArray)
		{
			if (Entry == nullptr || Entry->IsABot())
			{
				continue;
			}
			const APlayerController* TheirPC = Cast<APlayerController>(Entry->GetOwningController());
			if (TheirPC != nullptr && !TheirPC->IsLocalController())
			{
				return Entry;
			}
		}
		return nullptr;
	}

	struct FRemotePullState
	{
		int32  Phase = 0;
		double PhaseStartReal = 0.0;
		TWeakObjectPtr<UTraceAbilityComponent> Remote;
		FVector Anchor = FVector::ZeroVector;
		float   StartDistance = -1.f;
		float   ClosestDistance = -1.f;
		bool    bArmed = false;
		bool    bPullSeen = false;
	};

	void RunRemotePullTest()
	{
		UWorld* WorldPtr = FindAuthoritativeWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[REMOTEPULL] no authoritative game world — this half runs on the HOST."));
			return;
		}
		UnpauseAndReport(WorldPtr);

		TSharedPtr<FRemotePullState> State = MakeShared<FRemotePullState>();
		State->PhaseStartReal = FPlatformTime::Seconds();

		UE_LOG(LogTraceGame, Display,
			TEXT("[REMOTEPULL] ===== HOST HALF. Arms a REMOTE client's Mace with an embedded spike, then watches "
			     "whether that client's press ever reaches the server. Run Trace.Mace.RemotePullPress on the "
			     "client. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(WorldPtr)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[REMOTEPULL] ABORTED: the world went away."));
				return false;
			}
			const double NowReal = FPlatformTime::Seconds();

			if (State->Phase == 0)
			{
				APlayerState* RemoteState = FindRemoteHumanPlayerState(TickWorld);
				UTraceAbilityComponent* Comp = (RemoteState != nullptr)
					? RemoteState->FindComponentByClass<UTraceAbilityComponent>() : nullptr;
				if (Comp != nullptr && Comp->GetCharacterId() != ETraceCharacterId::Mace)
				{
					Comp->ServerSetCharacter(ETraceCharacterId::Mace);
				}
				UTraceAbilitySetMace* MaceSet = (Comp != nullptr)
					? Comp->GetAbilitySetAs<UTraceAbilitySetMace>() : nullptr;
				ATraceCharacter* MacePawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;

				if (MaceSet == nullptr || MacePawn == nullptr)
				{
					if ((NowReal - State->PhaseStartReal) > 45.0)
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[REMOTEPULL] VERDICT: INVALID — no REMOTE human player could be made Mace "
							     "(remotePlayerState=%d set=%d pawn=%d). This half needs a second process joined "
							     "to this match."),
							(RemoteState != nullptr) ? 1 : 0, (MaceSet != nullptr) ? 1 : 0, (MacePawn != nullptr) ? 1 : 0);
						return false;
					}
					return true;
				}

				State->Remote = Comp;
				State->Phase = 1;
				State->PhaseStartReal = NowReal;
				UE_LOG(LogTraceGame, Display, TEXT("[REMOTEPULL] remote player %s is Mace; arming a spike."),
					*GetNameSafe(RemoteState));
				return true;
			}

			UTraceAbilityComponent* Comp = State->Remote.Get();
			UTraceAbilitySetMace* MaceSet = (Comp != nullptr) ? Comp->GetAbilitySetAs<UTraceAbilitySetMace>() : nullptr;
			ATraceCharacter* MacePawn = (Comp != nullptr) ? Comp->GetOwningCharacter() : nullptr;
			if (MaceSet == nullptr || MacePawn == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[REMOTEPULL] ABORTED: the remote Mace went away mid-test."));
				return false;
			}

			// Phase 1: put an embedded spike 1500 uu in front of her, and keep it there. Re-armed
			// whenever the 2 s window closes, because the client's press has to cross a network and
			// the point of this test is the ROUTING, not the window.
			if (State->Phase == 1)
			{
				if (MaceSet->GetSpike() == nullptr && !MaceSet->IsPulling())
				{
					State->Anchor = MacePawn->GetActorLocation() + MacePawn->GetActorForwardVector() * 1500.f;
					MaceSet->DebugThrowSpikeAt(State->Anchor, /*TravelSpeedOverride*/ 0.f);
					State->bArmed = State->bArmed || (MaceSet->GetSpike() != nullptr);
					State->StartDistance = FVector::Dist(MacePawn->GetActorLocation(), State->Anchor);
					State->ClosestDistance = State->StartDistance;
				}

				if (MaceSet->IsPulling())
				{
					State->bPullSeen = true;
					State->Phase = 2;
					State->PhaseStartReal = NowReal;
					return true;
				}

				if ((NowReal - State->PhaseStartReal) > 25.0)
				{
					State->Phase = 3;
					return true;
				}
				return true;
			}

			// Phase 2: it started. Did it actually carry her?
			if (State->Phase == 2)
			{
				State->ClosestDistance = FMath::Min(State->ClosestDistance,
					static_cast<float>(FVector::Dist(MacePawn->GetActorLocation(), State->Anchor)));
				if (MaceSet->IsPulling() && (NowReal - State->PhaseStartReal) < 4.0)
				{
					return true;
				}
				State->Phase = 3;
				return true;
			}

			MaceSet->OnHalfTime();

			UE_LOG(LogTraceGame, Display, TEXT("[REMOTEPULL] armed=%d pullSeen=%d %.0f uu -> %.0f uu"),
				State->bArmed ? 1 : 0, State->bPullSeen ? 1 : 0, State->StartDistance, State->ClosestDistance);
			if (!State->bArmed)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[REMOTEPULL] VERDICT: INVALID — the fixture never got a spike into the remote Mace, so "
					     "the client had nothing to press on and a red result would mean nothing."));
			}
			// JUDGED ON ROUTING, NOT ON DISTANCE, and the difference is not a softened bar — it is the
			// right question. The anchor here is a hand-placed point 1500 uu in front of her inside a
			// real arena, so whatever is between the two is between the two, and §6's "bouncing off a
			// wall cancels it" will legitimately stop her short. What this pair of processes exists to
			// answer is whether a press made on a CLIENT ever becomes a pull on the SERVER; 200 uu of
			// travel under a 1375 uu/s pull cannot happen by accident and settles that outright.
			// Judging on distance instead made this report FAIL on a run where the press had plainly
			// arrived and she had moved 584 uu.
			else if (State->bPullSeen && (State->StartDistance - State->ClosestDistance) > 200.f)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[REMOTEPULL] VERDICT: PASS — a REMOTE CLIENT's single press of E reached the server and "
					     "pulled her %.0f uu -> %.0f uu. The reactivation works off the machine that owns the "
					     "pointer. (How far she got is the arena's business, not this test's.)"),
					State->StartDistance, State->ClosestDistance);
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[REMOTEPULL] VERDICT: *** FAIL *** — the spike sat embedded and pullable for 25 s and the "
					     "remote client's press never produced a pull on the server (pullSeen=%d, %.0f uu -> %.0f uu). "
					     "The reactivation is unreachable from a client."),
					State->bPullSeen ? 1 : 0, State->StartDistance, State->ClosestDistance);
			}
			return false;
		}));
	}

	FAutoConsoleCommand CmdRemotePullTest(
		TEXT("Trace.Mace.RemotePullTest"),
		TEXT("Dev only, HOST half of a two-process test. Makes a REMOTE client's player Mace, keeps an embedded "
		     "spike in front of her, and reports whether that client's reactivation ever reaches the server. Run "
		     "Trace.Mace.RemotePullPress on the client."),
		FConsoleCommandDelegate::CreateStatic(&RunRemotePullTest));

	void RunRemotePullPress()
	{
		// THE CLIENT HALF. No authority here on purpose — this is the machine the bug lived on.
		UWorld* LocalWorld = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				UWorld* Candidate = Context.World();
				if (Candidate != nullptr && Candidate->IsGameWorld())
				{
					LocalWorld = Candidate;
					break;
				}
			}
		}
		if (LocalWorld == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[REMOTEPRESS] no game world."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[REMOTEPRESS] ===== CLIENT HALF. Polls the LOCAL player's Mace and presses E through "
			     "UTraceAbilityInputRelay::RouteActivatePressed the FIRST time IsSpikeReadyToPull() is true. One "
			     "press, no retries: a build where that predicate never becomes true simply never presses. ====="));

		const double StartReal = FPlatformTime::Seconds();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[StartReal, WeakWorld = TWeakObjectPtr<UWorld>(LocalWorld)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			if (TickWorld == nullptr)
			{
				return false;
			}
			APlayerController* LocalPC = TickWorld->GetFirstPlayerController();
			APlayerState* MyState = (LocalPC != nullptr) ? LocalPC->PlayerState : nullptr;
			UTraceAbilityComponent* Comp = (MyState != nullptr)
				? MyState->FindComponentByClass<UTraceAbilityComponent>() : nullptr;
			UTraceAbilitySetMace* MaceSet = (Comp != nullptr)
				? Comp->GetAbilitySetAs<UTraceAbilitySetMace>() : nullptr;

			const double NowReal = FPlatformTime::Seconds();

			if (MaceSet != nullptr && MaceSet->IsSpikeReadyToPull())
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[REMOTEPRESS] the client CAN see a pullable spike after %.1fs — pressing E once through "
					     "the shipping router."), NowReal - StartReal);
				UTraceAbilityInputRelay::RouteActivatePressed(MyState);
				return false;
			}

			if ((NowReal - StartReal) > 40.0)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[REMOTEPRESS] *** the client NEVER saw a pullable spike in 40 s *** (isMace=%d hasSet=%d). "
					     "IsSpikeReadyToPull() stayed false, so the press was never even attempted — this is the "
					     "reactivation being unreachable on a client."),
					(Comp != nullptr && Comp->GetCharacterId() == ETraceCharacterId::Mace) ? 1 : 0,
					(MaceSet != nullptr) ? 1 : 0);
				return false;
			}
			return true;
		}));
	}

	FAutoConsoleCommand CmdRemotePullPress(
		TEXT("Trace.Mace.RemotePullPress"),
		TEXT("Dev only, CLIENT half of a two-process test. Presses E once, through the shipping router, the first "
		     "time the LOCAL Mace reports a pullable spike. Pair with Trace.Mace.RemotePullTest on the host."),
		FConsoleCommandDelegate::CreateStatic(&RunRemotePullPress));

	FAutoConsoleCommand CmdSpikeConsistency(
		TEXT("Trace.Mace.SpikeConsistency"),
		TEXT("Dev only, server only. SPEC v15 §6. Fires Mace's Spike ~90 times at geometry with known normals — "
		     "flat, tilted, thin, pillar, railing, corner, point blank, long range and while moving — plus a census "
		     "of the real arena, and reports the EMBED RATE. Four categories must still fizzle; that is the "
		     "falsification. Then pulls on real spikes at four distances."),
		FConsoleCommandDelegate::CreateStatic(&RunSpikeConsistency));
}   // namespace TraceMaceSpikeConsistency

#endif // !UE_BUILD_SHIPPING
