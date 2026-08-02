#include "Gameplay/TraceTrailComponent.h"

#include "Net/UnrealNetwork.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                       // TActorIterator (fallback character gather)
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"       // GetServerWorldTimeSeconds()
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"                // TNumericLimits (trip-test broad phase)
#include "Math/UnrealMathUtility.h"            // FMath::SegmentDistToSegmentSafe
#include "UObject/ConstructorHelpers.h"

#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"   // GetLastDashActiveWorldTime()
#include "Trace.h"
#include "TraceSettings.h"

namespace
{
	/** Upper bound on pooled visual components, so a pathological MaxTrailPoints cannot explode. */
	constexpr int32 MaxPooledSegments = 512;

	/**
	 * How long client visuals stay hidden after MulticastClearTrail. The reliable multicast can
	 * arrive a frame before the property delta that actually empties Items; without this the
	 * trail of a just-killed carrier flickers back for a few frames.
	 */
	constexpr float TrailClearSuppressSeconds = 0.35f;

	/**
	 * Sweeps longer than this are treated as teleports (respawn, post-score reposition) rather
	 * than movement, and are not tested — otherwise the segment from a player's pre-respawn
	 * position to their spawn point would scythe through the whole arena.
	 */
	constexpr double MinTeleportSweepDistance = 600.0;

	/**
	 * The same idea applied to the carrier: a gap this large between two consecutive trail points
	 * cannot have been walked, so the trail restarts rather than joining them into one lethal
	 * segment spanning the arena. ATraceGameMode clears the trail explicitly on every teleport it
	 * knows about (death, respawn, post-score reset); this is the backstop for the rest.
	 */
	constexpr double MaxTrailSegmentLength = 1000.0;

	/** Divide-by-zero epsilon. Written as a literal on purpose: the KINDA_SMALL_NUMBER family of
	 *  macros was renamed during the 5.x line and we must compile on 5.4 through 5.8. */
	constexpr double GeometryEpsilon = 1.0e-8;

	/**
	 * How long after the server last saw a pawn inside its dash window that pawn still counts as
	 * dashing for the trip test. Comfortably longer than one server frame at 60Hz and shorter than
	 * the dash cooldown, so it can never turn "dashed a moment ago" into free permanent immunity to
	 * the rule that walking does nothing.
	 */
	constexpr double RecentDashGraceSeconds = 0.15;

	/** Where along [A,B] the point P projects, clamped to [0,1]. Zero-length segments give 0. */
	double SegmentAlpha(const FVector& A, const FVector& B, const FVector& P)
	{
		const FVector Segment = B - A;
		const double LengthSquared = Segment.SizeSquared();
		if (LengthSquared <= GeometryEpsilon)
		{
			return 0.0;
		}
		return FMath::Clamp(FVector::DotProduct(P - A, Segment) / LengthSquared, 0.0, 1.0);
	}
}


UTraceTrailComponent::UTraceTrailComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Post-physics so the swept trip test reads this frame's *final* capsule positions rather
	// than positions from halfway through the movement update.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	SetIsReplicatedByDefault(true);

	// The component itself is a bare USceneComponent: a logical anchor with no primitive and
	// therefore no collision of its own. Everything it draws is a pooled child mesh.

	// Contract §2: engine basic shapes only, resolved with a constructor-time FObjectFinder so
	// the cooker follows the CDO reference and the asset survives into a packaged build. A bare
	// runtime LoadObject would return nullptr there.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		CylinderMesh = CylinderFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		TrailMaterial = MaterialFinder.Object;
	}
}

void UTraceTrailComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None on both: the carrier's own client needs to see the trail it is laying just as
	// much as everyone else does.
	DOREPLIFETIME(UTraceTrailComponent, TrailPoints);
	DOREPLIFETIME(UTraceTrailComponent, bEmitting);
}

void UTraceTrailComponent::OnRegister()
{
	Super::OnRegister();

	// The fast array's back pointer must be live before the first delta lands on a client, which
	// can happen before BeginPlay. It cannot be set from the constructor: FObjectInitializer
	// copies UPROPERTY values from the archetype *after* the C++ constructor runs, so a `this`
	// captured there would be overwritten with the CDO's pointer.
	TrailPoints.OwnerComponent = this;
}

void UTraceTrailComponent::BeginPlay()
{
	Super::BeginPlay();

	TrailPoints.OwnerComponent = this;
	bVisualsDirty = true;
}

void UTraceTrailComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyVisualPool();
	PreviousLocations.Reset();

	Super::EndPlay(EndPlayReason);
}

void UTraceTrailComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return;
	}

	if (Owner->HasAuthority())
	{
		// Contract §3: the trail dies with its carrier, instantly. ATraceCore drives this in the
		// normal flow; this is the safety net so a trail can never outlive the body that owns it
		// and go on killing a corpse.
		if (bEmitting)
		{
			const ATraceCharacter* Carrier = GetOwnerCharacter();
			if (Carrier == nullptr || !Carrier->IsAlive())
			{
				SetEmitting(false);
				ClearTrail();
			}
		}

		ServerUpdateTrail();
		ServerRunTripTest(DeltaTime);
	}

	// Listen servers draw the trail too; only a headless server skips it.
	if (GetNetMode() != NM_DedicatedServer)
	{
		UpdateVisuals();
	}
}


// =================================================================================================
// Public API
// =================================================================================================

void UTraceTrailComponent::SetEmitting(bool bEmit)
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	if (bEmitting == bEmit)
	{
		return;
	}

	bEmitting = bEmit;

	// Remembered sweep positions are only valid within one continuous emission window.
	PreviousLocations.Reset();

	if (bEmit)
	{
		// A new carrier never inherits the previous trail.
		ClearTrail();

		// Lay the first point at the pickup itself rather than one spacing later.
		ServerUpdateTrail();
	}

	// Note: stopping does NOT clear. A trail left behind by a pass is harmless (the trip test
	// requires bEmitting) and fading out over TrailLifetime reads much better than popping.
	// Death is the case that must clear instantly, and it does so explicitly.

	UE_LOG(LogTraceGame, Verbose, TEXT("Trail: %s emitting for %s"),
		bEmit ? TEXT("started") : TEXT("stopped"), *GetNameSafe(GetOwner()));
}

bool UTraceTrailComponent::IsEmitting() const
{
	return bEmitting;
}

void UTraceTrailComponent::ClearTrail()
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	if (TrailPoints.Items.Num() > 0)
	{
		TrailPoints.Items.Reset();

		// Removal from a fast array is signalled with MarkArrayDirty, not MarkItemDirty.
		TrailPoints.MarkArrayDirty();

		// The delta alone would get there, but "instantly" is a game rule here — the reliable
		// multicast lets every client drop the visuals on the same frame the carrier dies.
		//
		// Guarded on there having actually been something to clear: ClearTrail() is called on every
		// death (twice - HandleDeath and NotifyCharacterDied), on Logout, on SetEmitting(true), and
		// for EVERY player on EVERY goal from ResetPlayersToSpawns. Unguarded that is ~10 reliable
		// multicasts per score, nine of them for components that have never held a point, on the
		// exact frame the server is also teleporting ten pawns. Reliable RPCs cannot be dropped.
		MulticastClearTrail();
	}

	PreviousLocations.Reset();
	bVisualsDirty = true;
}

void UTraceTrailComponent::MulticastClearTrail_Implementation()
{
	HideSegmentsFrom(0);

	LastVisualPointCount = -1;
	LastVisualHead = FVector::ZeroVector;
	LastVisualTail = FVector::ZeroVector;
	bVisualsDirty = true;

	// Hold the visuals down briefly: this reliable RPC can beat the property delta that empties
	// Items, and re-showing a dead carrier's trail for a few frames looks like a bug. Any
	// subsequent replication callback (or an empty Items) lifts the hold early.
	if (const UWorld* World = GetWorld())
	{
		VisualSuppressUntilTime = static_cast<float>(World->GetTimeSeconds()) + TrailClearSuppressSeconds;
	}
}

void UTraceTrailComponent::OnTrailPointsChanged()
{
	// Called once per changed item from FTraceTrailPoint's replication callbacks, which means it
	// can fire many times per packet and, for removals, *before* Items has actually shrunk.
	// So: flag only, and rebuild from the settled array on the next tick.
	bVisualsDirty = true;

	// A delta arrived, so Items is authoritative again — no reason to keep suppressing.
	VisualSuppressUntilTime = 0.f;
}


// =================================================================================================
// Server: laying the trail
// =================================================================================================

void UTraceTrailComponent::ServerUpdateTrail()
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Now = GetServerTimeSeconds();
	bool bChanged = false;

	// 1. Expire. Items are strictly ordered oldest-first, so the first survivor ends the scan.
	const float Lifetime = FMath::Max(0.1f, Settings.TrailLifetime);
	int32 ExpiredCount = 0;
	while (ExpiredCount < TrailPoints.Items.Num()
		&& (Now - TrailPoints.Items[ExpiredCount].BirthServerTime) > Lifetime)
	{
		++ExpiredCount;
	}
	if (ExpiredCount > 0)
	{
		TrailPoints.Items.RemoveAt(0, ExpiredCount);
		TrailPoints.MarkArrayDirty();
		bChanged = true;
	}

	// 2. Hard cap, oldest dropped first.
	const int32 MaxPoints = FMath::Max(2, Settings.MaxTrailPoints);
	if (TrailPoints.Items.Num() > MaxPoints)
	{
		TrailPoints.Items.RemoveAt(0, TrailPoints.Items.Num() - MaxPoints);
		TrailPoints.MarkArrayDirty();
		bChanged = true;
	}

	// 3. Append, distance-gated so a stationary carrier does not spam identical points.
	if (bEmitting)
	{
		// Anchor on the capsule centre (the owner's actor location), NOT on this component's own
		// world location. The trip test measures every candidate by its actor location, so both
		// halves of the geometry have to live in the same reference frame; if the pawn ever
		// attaches this component with an offset, GetComponentLocation() would quietly slide the
		// trail away from the volume the test evaluates. Falls back for a non-character owner.
		const ATraceCharacter* Carrier = GetOwnerCharacter();
		const FVector Location = Carrier != nullptr ? Carrier->GetActorLocation() : GetComponentLocation();

		const double Spacing = FMath::Max(1.0, static_cast<double>(Settings.TrailPointSpacing));

		const bool bHasHead = TrailPoints.Items.Num() > 0;
		const double DistanceFromHead = bHasHead
			? FVector::Dist(Location, TrailPoints.Items.Last().Location)
			: 0.0;

		// Teleport, not movement — restart rather than laying a segment across the map.
		if (bHasHead && DistanceFromHead > MaxTrailSegmentLength)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("Trail: discontinuity of %.0fuu on %s, restarting trail"),
				DistanceFromHead, *GetNameSafe(GetOwner()));

			ClearTrail();
			bChanged = true;
		}

		if (TrailPoints.Items.Num() == 0 || DistanceFromHead >= Spacing)
		{
			FTraceTrailPoint& NewPoint = TrailPoints.Items.AddDefaulted_GetRef();
			NewPoint.Location = Location;
			NewPoint.BirthServerTime = Now;

			// Adding or changing an item is signalled per item; only removals need MarkArrayDirty.
			TrailPoints.MarkItemDirty(NewPoint);
			bChanged = true;
		}
	}

	if (bChanged)
	{
		bVisualsDirty = true;

		// Authority cannot race itself: TrailPoints is settled by the time we get here. A listen
		// server runs MulticastClearTrail on itself too, so without this the host would hold its
		// own visuals down for the suppression window every time a carrier picks the Core up
		// (SetEmitting -> ClearTrail -> multicast -> immediately lay the first point again).
		VisualSuppressUntilTime = 0.f;
	}
}


// =================================================================================================
// Server: the trip test — the whole game lives here
// =================================================================================================

void UTraceTrailComponent::ServerRunTripTest(float DeltaTime)
{
	ATraceCharacter* Carrier = GetOwnerCharacter();
	if (Carrier == nullptr || !bEmitting || !Carrier->IsAlive())
	{
		// Not laying trail: nothing is lethal, and any remembered positions are now stale.
		PreviousLocations.Reset();
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// The newest TrailHeadGracePoints points are exempt so a defender cannot simply stand on the
	// emitter and dash on the spot — they have to actually cross the laid trail.
	const int32 GraceCount = FMath::Max(0, Settings.TrailHeadGracePoints);
	const int32 LastTestableIndex = TrailPoints.Items.Num() - 1 - GraceCount;

	// Snapshot the testable positions. Nothing below may touch TrailPoints.Items: applying a kill
	// re-enters this component (death -> SetCarrying(false) -> SetEmitting/ClearTrail) and would
	// invalidate any live iteration.
	TestPositions.Reset();
	for (int32 PointIndex = 0; PointIndex <= LastTestableIndex; ++PointIndex)
	{
		TestPositions.Add(TrailPoints.Items[PointIndex].Location);
	}

	// Broad phase. The narrow phase below is O(candidates x segments) - with MaxTrailPoints=256 and
	// ten players that is ~2500 segment-to-segment tests every server frame, for a test that is
	// almost always a miss. One XY AABB over the whole trail turns the common case into four
	// comparisons per candidate. It is only ever used to reject, so it cannot change the outcome.
	// Written as four scalars rather than an FBox2D so nothing depends on that type's float/double
	// spelling on any given 5.x engine.
	double TrailMinX = TNumericLimits<double>::Max();
	double TrailMinY = TNumericLimits<double>::Max();
	double TrailMaxX = -TNumericLimits<double>::Max();
	double TrailMaxY = -TNumericLimits<double>::Max();
	for (const FVector& Position : TestPositions)
	{
		TrailMinX = FMath::Min(TrailMinX, Position.X);
		TrailMinY = FMath::Min(TrailMinY, Position.Y);
		TrailMaxX = FMath::Max(TrailMaxX, Position.X);
		TrailMaxY = FMath::Max(TrailMaxY, Position.Y);
	}

	TArray<ATraceCharacter*> Candidates;
	GatherTrackedCharacters(Candidates);

	const double MaxSweepDistance = FMath::Max(
		MinTeleportSweepDistance,
		static_cast<double>(Settings.DashSpeed) * static_cast<double>(DeltaTime) * 2.0);

	const double TrailRadius = FMath::Max(0.0, static_cast<double>(Settings.TrailRadius));
	const double TrailHalfHeight = FMath::Max(0.0, static_cast<double>(Settings.TrailHeight)) * 0.5;

	ATraceCharacter* Tripper = nullptr;

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (Candidate == nullptr)
		{
			continue;
		}

		const FVector CurrentLocation = Candidate->GetActorLocation();

		// Refresh the remembered position for EVERY tracked character, before any filtering and
		// even after a trip has been found. If we only tracked characters that pass the filters,
		// then a player's "previous" position would date from whenever they last happened to be
		// dashing, and the sweep for the first tick of their next dash would be a huge stale
		// segment that the teleport guard throws away — i.e. dashes would randomly not register.
		FVector PreviousLocation = CurrentLocation;
		if (FVector* Stored = PreviousLocations.Find(Candidate))
		{
			PreviousLocation = *Stored;
			*Stored = CurrentLocation;
		}
		else
		{
			PreviousLocations.Add(Candidate, CurrentLocation);
		}

		if (Tripper != nullptr)
		{
			continue;   // Already resolved this tick; keep looping only to refresh positions.
		}

		// --- eligibility, in the exact order the game rules state it -------------------------

		// The carrier can never trip their own trail (grace points are not enough on their own
		// when bOnlyEnemiesTripTrail is turned off for tuning).
		if (Candidate == Carrier)
		{
			continue;
		}

		// (a) alive
		if (!Candidate->IsAlive())
		{
			continue;
		}

		// (b) an enemy of the carrier. Unknown teams never count as enemies.
		if (Settings.bOnlyEnemiesTripTrail)
		{
			const ETraceTeam CarrierTeam = Carrier->GetTeam();
			const ETraceTeam CandidateTeam = Candidate->GetTeam();
			const bool bIsEnemy = CarrierTeam != ETraceTeam::None
				&& CandidateTeam != ETraceTeam::None
				&& CandidateTeam != CarrierTeam;
			if (!bIsEnemy)
			{
				continue;
			}
		}

		// (c) dashing. This is the rule: walking or running through a trail does nothing at all,
		// and the dash is the only counterplay to an invulnerable carrier.
		//
		// Sampled with a short trailing window rather than as an instant. This test ticks once per
		// server frame, but the server advances a remote client's dash clock inside MoveAutonomous
		// and can consume several client moves in one frame - so the tail of a dash (or, after a
		// hitch, all 0.18s of it) can be simulated *between* two ticks here. The displacement is
		// still credited to this frame's sweep, but DashTimeRemaining has already hit zero, and the
		// player watches themselves dash through the trail with nothing happening. The movement
		// component latches the last instant it was authoritatively dashing; accept that too.
		if (Settings.bRequireDashToTripTrail && !Candidate->IsDashing())
		{
			bool bRecentlyDashed = false;
			if (const UWorld* World = GetWorld())
			{
				if (const UTraceCharacterMovementComponent* CandidateMovement = Candidate->GetTraceMovement())
				{
					bRecentlyDashed = (World->GetTimeSeconds() - CandidateMovement->GetLastDashActiveWorldTime())
						<= RecentDashGraceSeconds;
				}
			}

			if (!bRecentlyDashed)
			{
				continue;
			}
		}

		// --- swept geometry -------------------------------------------------------------------

		if (LastTestableIndex < 1)
		{
			continue;   // Fewer than two testable points means there is no segment yet.
		}

		const double SweepDistance = FVector::Dist(PreviousLocation, CurrentLocation);
		if (SweepDistance > MaxSweepDistance)
		{
			continue;   // Teleport, not movement.
		}

		double CapsuleRadius = 34.0;
		double CapsuleHalfHeight = 88.0;
		if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
		{
			CapsuleRadius = Capsule->GetScaledCapsuleRadius();
			CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		}

		// The trail is a vertical wall of radius TrailRadius and height TrailHeight swept along
		// the carrier's path, and the tripper is a capsule swept along its path this tick. Test
		// those two sweeps as: horizontal segment-to-segment distance (which catches tunnelling
		// at dash speed, unlike a point test), plus a separate vertical overlap check so that
		// clearing the wall in the air is not a hit.
		const double HorizontalThreshold = TrailRadius + CapsuleRadius;
		const double HorizontalThresholdSquared = HorizontalThreshold * HorizontalThreshold;
		const double VerticalThreshold = TrailHalfHeight + CapsuleHalfHeight;

		const FVector SweepStart(PreviousLocation.X, PreviousLocation.Y, 0.0);
		const FVector SweepEnd(CurrentLocation.X, CurrentLocation.Y, 0.0);

		// Broad phase: if this candidate's swept XY box, inflated by the same horizontal threshold
		// the narrow phase uses, does not touch the trail's XY box, no segment can be within range.
		{
			const double SweepMinX = FMath::Min(PreviousLocation.X, CurrentLocation.X) - HorizontalThreshold;
			const double SweepMaxX = FMath::Max(PreviousLocation.X, CurrentLocation.X) + HorizontalThreshold;
			const double SweepMinY = FMath::Min(PreviousLocation.Y, CurrentLocation.Y) - HorizontalThreshold;
			const double SweepMaxY = FMath::Max(PreviousLocation.Y, CurrentLocation.Y) + HorizontalThreshold;

			if (SweepMaxX < TrailMinX || SweepMinX > TrailMaxX || SweepMaxY < TrailMinY || SweepMinY > TrailMaxY)
			{
				continue;
			}
		}

		for (int32 SegmentIndex = 0; SegmentIndex + 1 <= LastTestableIndex; ++SegmentIndex)
		{
			const FVector& TrailStart = TestPositions[SegmentIndex];
			const FVector& TrailEnd = TestPositions[SegmentIndex + 1];

			const FVector FlatTrailStart(TrailStart.X, TrailStart.Y, 0.0);
			const FVector FlatTrailEnd(TrailEnd.X, TrailEnd.Y, 0.0);

			// Returns void — the closest point on each segment, not the distance.
			FVector ClosestOnSweep = FVector::ZeroVector;
			FVector ClosestOnTrail = FVector::ZeroVector;
			FMath::SegmentDistToSegmentSafe(SweepStart, SweepEnd, FlatTrailStart, FlatTrailEnd, ClosestOnSweep, ClosestOnTrail);

			if (FVector::DistSquared(ClosestOnSweep, ClosestOnTrail) > HorizontalThresholdSquared)
			{
				continue;
			}

			// Recover where along each segment the closest approach happened so we can compare
			// heights there (the flattened test threw the Z away).
			const double SweepAlpha = SegmentAlpha(SweepStart, SweepEnd, ClosestOnSweep);
			const double TrailAlpha = SegmentAlpha(FlatTrailStart, FlatTrailEnd, ClosestOnTrail);
			const double ToucherZ = FMath::Lerp(PreviousLocation.Z, CurrentLocation.Z, SweepAlpha);
			const double TrailZ = FMath::Lerp(TrailStart.Z, TrailEnd.Z, TrailAlpha);

			if (FMath::Abs(ToucherZ - TrailZ) > VerticalThreshold)
			{
				continue;
			}

			Tripper = Candidate;
			break;
		}
	}

	// Applied outside every loop above: this kills, which re-enters the component and mutates
	// TrailPoints.Items and PreviousLocations.
	if (Tripper != nullptr)
	{
		ApplyTrailTrip(Carrier, Tripper);
	}
}

void UTraceTrailComponent::ApplyTrailTrip(ATraceCharacter* Carrier, ATraceCharacter* Tripper)
{
	if (Carrier == nullptr || Tripper == nullptr)
	{
		return;
	}

	// Resolve both controllers first: the first Kill() may unpossess the pawn we would otherwise
	// ask for a controller afterwards.
	AController* TripperController = Tripper->GetController();
	AController* CarrierController = Carrier->GetController();

	/** Cause tag reported to the GameMode / kill feed for a trail death (build contract §7). */
	static const FName TrailDeathCause(TEXT("Trail"));

	const ETrailLethality Lethality = UTraceSettings::Get().TrailLethality;

	UE_LOG(LogTraceGame, Log, TEXT("Trail tripped: %s dashed through %s's trail (lethality %d)"),
		*GetNameSafe(Tripper), *GetNameSafe(Carrier), static_cast<int32>(Lethality));

	if (Lethality == ETrailLethality::KillsCarrier || Lethality == ETrailLethality::KillsBoth)
	{
		// Kill(), never ApplyDamage(): the carrier is invulnerable to damage by design, and the
		// trail is the one thing that gets through. This is the whole point of the mechanic.
		if (UTraceHealthComponent* CarrierHealth = Carrier->Health)
		{
			CarrierHealth->Kill(TripperController, TrailDeathCause);
		}
	}

	if (Lethality == ETrailLethality::KillsToucher || Lethality == ETrailLethality::KillsBoth)
	{
		if (UTraceHealthComponent* TripperHealth = Tripper->Health)
		{
			TripperHealth->Kill(CarrierController, TrailDeathCause);
		}
	}
}

void UTraceTrailComponent::GatherTrackedCharacters(TArray<ATraceCharacter*>& OutCharacters) const
{
	OutCharacters.Reset();

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (const ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
	{
		for (const TWeakObjectPtr<ATraceCharacter>& WeakCharacter : GameMode->GetTrackedCharacters())
		{
			if (ATraceCharacter* Character = WeakCharacter.Get())
			{
				OutCharacters.Add(Character);
			}
		}

		if (OutCharacters.Num() > 0)
		{
			return;
		}
	}

	// Fallback: the GameMode is the fast path, but the signature mechanic must not silently stop
	// working if registration is incomplete (e.g. a character spawned outside the normal flow).
	for (TActorIterator<ATraceCharacter> It(World); It; ++It)
	{
		if (ATraceCharacter* Character = *It)
		{
			OutCharacters.Add(Character);
		}
	}
}

float UTraceTrailComponent::GetServerTimeSeconds() const
{
	if (const UWorld* World = GetWorld())
	{
		// GetServerWorldTimeSeconds() is already replicated and smoothed; do not hand-roll a
		// clock. It returns double on 5.3+, so narrow explicitly.
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return static_cast<float>(GameState->GetServerWorldTimeSeconds());
		}
		return static_cast<float>(World->GetTimeSeconds());
	}
	return 0.f;
}

ATraceCharacter* UTraceTrailComponent::GetOwnerCharacter() const
{
	return Cast<ATraceCharacter>(GetOwner());
}


// =================================================================================================
// Visuals (client + listen server)
// =================================================================================================

void UTraceTrailComponent::UpdateVisuals()
{
	if (VisualSuppressUntilTime > 0.f)
	{
		const UWorld* World = GetWorld();
		const float Now = World != nullptr ? static_cast<float>(World->GetTimeSeconds()) : 0.f;
		if (World == nullptr || TrailPoints.Items.Num() == 0 || Now >= VisualSuppressUntilTime)
		{
			VisualSuppressUntilTime = 0.f;
		}
		else
		{
			return;
		}
	}

	// Team colour resolves late on clients (PlayerState replication), so re-check every tick.
	// UpdateTeamColor() early-outs unless the colour actually changed.
	UpdateTeamColor();

	// Change detection. bVisualsDirty is the primary signal (set by the replication callbacks and
	// by every server-side mutation); the head/tail/count comparison is a cheap backstop so the
	// visuals keep tracking even if a fast-array callback is ever missed.
	const int32 PointCount = TrailPoints.Items.Num();
	const FVector Head = PointCount > 0 ? FVector(TrailPoints.Items.Last().Location) : FVector::ZeroVector;
	const FVector Tail = PointCount > 0 ? FVector(TrailPoints.Items[0].Location) : FVector::ZeroVector;

	if (!bVisualsDirty
		&& PointCount == LastVisualPointCount
		&& Head.Equals(LastVisualHead, 0.01)
		&& Tail.Equals(LastVisualTail, 0.01))
	{
		return;
	}

	bVisualsDirty = false;
	LastVisualPointCount = PointCount;
	LastVisualHead = Head;
	LastVisualTail = Tail;

	RebuildVisuals();
}

void UTraceTrailComponent::RebuildVisuals()
{
	const int32 SegmentCount = FMath::Max(0, TrailPoints.Items.Num() - 1);
	if (SegmentCount == 0 || CylinderMesh == nullptr)
	{
		HideSegmentsFrom(0);
		return;
	}

	CacheMeshMetrics();

	const UTraceSettings& Settings = UTraceSettings::Get();
	const double Width = FMath::Max(1.0, 2.0 * static_cast<double>(Settings.TrailRadius));
	const double Height = FMath::Max(1.0, static_cast<double>(Settings.TrailHeight));

	int32 PlacedCount = 0;
	for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
	{
		UStaticMeshComponent* Segment = GetOrCreateSegment(SegmentIndex);
		if (Segment == nullptr)
		{
			break;   // Pool cap hit, or no mesh — draw what we have.
		}

		const FVector Start = TrailPoints.Items[SegmentIndex].Location;
		const FVector End = TrailPoints.Items[SegmentIndex + 1].Location;

		FVector Along = End - Start;
		Along.Z = 0.0;
		const double AlongLength = Along.Size();

		// Yaw-only: Along is horizontal, so the cylinder's local Z stays world-up and the mesh
		// remains a vertical wall segment.
		const FRotator SegmentRotation = AlongLength > 1.0 ? (Along / AlongLength).Rotation() : FRotator::ZeroRotator;

		// Stretched along the path and padded by one radius at each end so consecutive segments
		// overlap into one continuous wall instead of a dotted line of pillars. The resulting
		// elliptical prism is a close match for the swept capsule the trip test actually uses.
		const FVector DesiredSize(AlongLength + Width, Width, Height);
		const FVector Scale(
			DesiredSize.X / (2.0 * MeshHalfSize.X),
			DesiredSize.Y / (2.0 * MeshHalfSize.Y),
			DesiredSize.Z / (2.0 * MeshHalfSize.Z));

		// Corrects for a source mesh whose pivot is not at its bounds centre, so we never have to
		// assume anything about the engine cylinder's authoring.
		const FVector PivotCorrection = SegmentRotation.RotateVector(MeshPivotOffset * Scale);
		const FVector Midpoint = (Start + End) * 0.5;

		Segment->SetWorldLocationAndRotation(Midpoint - PivotCorrection, SegmentRotation);
		Segment->SetWorldScale3D(Scale);
		Segment->SetVisibility(true);
		++PlacedCount;
	}

	HideSegmentsFrom(PlacedCount);
}

UStaticMeshComponent* UTraceTrailComponent::GetOrCreateSegment(int32 Index)
{
	if (SegmentPool.IsValidIndex(Index))
	{
		return SegmentPool[Index];
	}

	// The pool only ever grows one slot at a time, in order.
	if (Index != SegmentPool.Num() || Index >= MaxPooledSegments)
	{
		return nullptr;
	}

	AActor* Owner = GetOwner();
	if (Owner == nullptr || CylinderMesh == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Segment = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
	if (Segment == nullptr)
	{
		return nullptr;
	}

	// Mobility must be set before registration.
	Segment->SetMobility(EComponentMobility::Movable);
	Segment->SetStaticMesh(CylinderMesh);
	Segment->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Segment->SetCollisionProfileName(TEXT("NoCollision"));
	Segment->SetGenerateOverlapEvents(false);
	Segment->SetCanEverAffectNavigation(false);
	Segment->SetCastShadow(false);
	Segment->bReceivesDecals = false;
	Segment->SetIsReplicated(false);   // Purely local cosmetics, rebuilt from TrailPoints.
	Segment->SetVisibility(false);

	Segment->RegisterComponent();
	Segment->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);

	// Critical: the trail is laid in WORLD space and must not follow the carrier around. Absolute
	// transforms keep the components in the actor's hierarchy (so they are cleaned up with it)
	// while making them ignore the parent transform entirely.
	Segment->SetAbsolute(true, true, true);

	UMaterialInstanceDynamic* SegmentMaterial = nullptr;
	if (TrailMaterial != nullptr)
	{
		SegmentMaterial = Segment->CreateDynamicMaterialInstance(0, TrailMaterial);
	}

	SegmentPool.Add(Segment);
	SegmentMaterials.Add(SegmentMaterial);

	// Newly created components need the colour that the pool already agreed on.
	if (SegmentMaterial != nullptr && bColorApplied)
	{
		SegmentMaterial->SetVectorParameterValue(TEXT("Color"), AppliedColor);
		SegmentMaterial->SetVectorParameterValue(TEXT("BaseColor"), AppliedColor);
	}

	return Segment;
}

void UTraceTrailComponent::HideSegmentsFrom(int32 FirstIndex)
{
	for (int32 Index = FMath::Max(0, FirstIndex); Index < SegmentPool.Num(); ++Index)
	{
		if (UStaticMeshComponent* Segment = SegmentPool[Index])
		{
			Segment->SetVisibility(false);
		}
	}
}

void UTraceTrailComponent::UpdateTeamColor()
{
	FLinearColor Desired = TraceTeamColor(ETraceTeam::None);
	if (const ATraceCharacter* Character = GetOwnerCharacter())
	{
		Desired = TraceTeamColor(Character->GetTeam());
	}
	Desired.A = 1.f;

	if (bColorApplied && Desired.Equals(AppliedColor, 0.001f))
	{
		return;
	}

	AppliedColor = Desired;
	bColorApplied = true;

	for (UMaterialInstanceDynamic* SegmentMaterial : SegmentMaterials)
	{
		if (SegmentMaterial != nullptr)
		{
			// "Color" is BasicShapeMaterial's real vector parameter; "BaseColor" is a defensive
			// second guess and is a silent no-op if the parameter does not exist.
			SegmentMaterial->SetVectorParameterValue(TEXT("Color"), AppliedColor);
			SegmentMaterial->SetVectorParameterValue(TEXT("BaseColor"), AppliedColor);
		}
	}
}

void UTraceTrailComponent::CacheMeshMetrics()
{
	if (bMeshMetricsCached)
	{
		return;
	}

	if (CylinderMesh != nullptr)
	{
		const FBoxSphereBounds Bounds = CylinderMesh->GetBounds();
		MeshHalfSize = Bounds.BoxExtent;
		MeshPivotOffset = Bounds.Origin;
	}

	// Never divide by zero, whatever the asset turns out to be.
	MeshHalfSize.X = FMath::Max(MeshHalfSize.X, 1.0);
	MeshHalfSize.Y = FMath::Max(MeshHalfSize.Y, 1.0);
	MeshHalfSize.Z = FMath::Max(MeshHalfSize.Z, 1.0);

	bMeshMetricsCached = true;
}

void UTraceTrailComponent::DestroyVisualPool()
{
	for (UStaticMeshComponent* Segment : SegmentPool)
	{
		if (Segment != nullptr)
		{
			Segment->DestroyComponent();
		}
	}

	SegmentPool.Reset();
	SegmentMaterials.Reset();

	LastVisualPointCount = -1;
	bColorApplied = false;
}
