#include "Gameplay/TraceTrailComponent.h"

#include "Net/UnrealNetwork.h"

#include "Camera/PlayerCameraManager.h"        // local camera location (proximity glow fade)
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"                     // GEngine->GetFirstLocalPlayerController()
#include "Engine/EngineBaseTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                       // TActorIterator (fallback character gather)
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"       // GetServerWorldTimeSeconds()
#include "GameFramework/PlayerController.h"    // IsLocalPlayerController() (own-trace near hide)
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"                // TNumericLimits (trip-test broad phase)
#include "Math/UnrealMathUtility.h"            // FMath::SegmentDistToSegmentSafe
#include "UObject/ConstructorHelpers.h"

#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Gameplay/TraceCore.h"                // IsTraceInvulnerableFor (spec §4)
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"   // GetLastDashActiveWorldTime()
#include "Trace.h"
#include "TraceSettings.h"

namespace
{
	/**
	 * Spec §3: the trace lasts FOUR seconds (it was six).
	 *
	 * Taken as a CEILING over UTraceSettings::TrailLifetime rather than replacing it, because that
	 * property still ships at 6.0 and TraceSettings.h belongs to another ownership slice this pass.
	 * A designer lowering the setting still wins; a stale 6 cannot silently reinstate the old rule.
	 * Once the setting is updated to 4 this min() becomes an identity and can be deleted.
	 */
	constexpr float SpecTraceLifetimeSeconds = 4.0f;

	/** Upper bound on pooled after-images. At 4s and 60uu spacing a run produces roughly 52. */
	constexpr int32 MaxPooledGhosts = 96;

	/** Meshes per after-image: legs, torso, head. */
	constexpr int32 PartsPerGhost = 3;
	constexpr int32 PartLegs = 0;
	constexpr int32 PartTorso = 1;
	constexpr int32 PartHead = 2;

	/**
	 * How much of the freshest trace is hidden from the holder's own camera, measured along the path.
	 *
	 * Must stay comfortably longer than ATraceCharacter's third-person arm (450 uu), because the whole
	 * point is that the camera and its near field sit in the gap. 850 leaves 400 uu of clearance in
	 * front of the lens. Nobody else's view is affected — see the SetOwnerNoSee block in
	 * RebuildVisuals() for why this is presentation-only and cannot touch the lethal volume.
	 */
	constexpr double OwnerNearHideDistance = 850.0;

	/**
	 * Camera-proximity emissive fade — the anti-whiteout guard. See ApplyProximityGlowFade().
	 *
	 * Far: beyond this the trace is exactly as bright as it was measured to need. 320uu is a little
	 * over three capsule widths, so nothing at a normal fighting distance is affected at all.
	 * Near: at and below this the eye is effectively inside the volume.
	 * MinScale: NOT zero. The trace must stay visible from inside it — it is lethal from inside it.
	 */
	constexpr double ProximityFadeFarDistance = 320.0;
	constexpr double ProximityFadeNearDistance = 30.0;
	constexpr float ProximityFadeMinScale = 0.10f;

	/**
	 * How long client visuals stay hidden after MulticastClearTrail. The reliable multicast can
	 * arrive a frame before the property delta that actually empties Items; without this the
	 * trace of a just-killed holder flickers back for a few frames.
	 */
	constexpr float TrailClearSuppressSeconds = 0.35f;

	/**
	 * Sweeps longer than this are treated as teleports (respawn, post-score reposition) rather
	 * than movement, and are not tested — otherwise the segment from a player's pre-respawn
	 * position to their spawn point would scythe through the whole arena.
	 */
	constexpr double MinTeleportSweepDistance = 600.0;

	/**
	 * The same idea applied to the holder: a gap this large between two consecutive trace points
	 * cannot have been walked, so the trace restarts rather than joining them into one lethal
	 * segment spanning the arena.
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

	// ---------------------------------------------------------------------------------------------
	// After-image silhouette (spec §3: "a blur created where your character model has passed
	// through"). Every number is a FRACTION of the lethal volume, never an absolute — the shape has
	// to be re-derived if TrailRadius/TrailHeight are retuned, or the player is being shown a
	// boundary that is not the boundary.
	//
	// Measured from the bottom of the volume upward, the three parts tile it: legs occupy the lowest
	// 45%, torso the next 34%, head the top 23%. Widths taper the way a body does, with the TORSO at
	// the full lethal width so the widest part of the smear is the part you actually judge.
	// ---------------------------------------------------------------------------------------------

	constexpr double GhostLegCentreFrac = 0.225;
	constexpr double GhostLegHeightFrac = 0.450;
	constexpr double GhostLegWidthFrac = 0.80;

	constexpr double GhostTorsoCentreFrac = 0.615;
	constexpr double GhostTorsoHeightFrac = 0.340;
	constexpr double GhostTorsoWidthFrac = 1.00;

	constexpr double GhostHeadCentreFrac = 0.885;
	constexpr double GhostHeadHeightFrac = 0.230;
	constexpr double GhostHeadWidthFrac = 0.50;

	/**
	 * Glow per part, on M_TraceNeon.
	 *
	 * MEASURED, and the reason the split exists at all: a previous pass ran the whole trace at 3.4
	 * and every channel clipped, so it rendered as a shapeless white slab — extremely visible and
	 * useless, because you could no longer tell WHOSE it was, and a trace whose team you cannot read
	 * is a trace you cannot decide whether to dash through.
	 *
	 * So the body stays under 2 and keeps its team colour through the tonemapper, and the HEAD —
	 * a small element, exactly the case where clipping to white is the desired look — carries the
	 * brightness. That is not decoration either: the head sits at ~160uu above the floor, which is
	 * first-person eye height, so a chain of hot heads is the one feature of the smear that survives
	 * being seen edge-on from a player's own eyeline at any range. Do not dim it.
	 */
	constexpr float GhostLegGlow = 1.25f;
	constexpr float GhostTorsoGlow = 1.70f;
	constexpr float GhostHeadGlow = 4.20f;

	/** Oldest after-images dim to this fraction of full glow. Never to zero: they are still lethal. */
	constexpr float GhostOldestGlowScale = 0.55f;

	/** Spec §4: while the pass window is open the trace hardens. This is what that looks like. */
	constexpr float GhostInvulnerableGlowScale = 1.90f;

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

	// Engine basic shapes only, resolved with a constructor-time FObjectFinder so the cooker
	// follows the CDO reference and the asset survives into a packaged build. A bare runtime
	// LoadObject would return nullptr there.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		CylinderMesh = CylinderFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		SphereMesh = SphereFinder.Object;
	}

	// The trace is drawn on the arena's own unlit neon material, NOT on BasicShapeMaterial.
	//
	// This mattered more than anything else about the visuals. BasicShapeMaterial is LIT, and this
	// world is a black room with three deliberately weak directional lights in it - so a tinted lit
	// mesh standing on the floor came out as a dark grey-blue smudge that you could genuinely walk
	// past without noticing. M_TraceNeon emits Color * Glow with no lighting term at all, which is
	// what makes the after-image the same kind of object as every glowing edge in the arena, and
	// pushes it past the post-process bloom threshold so it reads as light rather than as geometry.
	//
	// The trace is the ONLY counterplay to a shielded holder (§3), so a player who cannot see it
	// cannot play the game.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		TrailMaterial = NeonFinder.Object;
		bTrailMaterialIsNeon = true;
	}

	// Fallback exactly as the arena builder does it: /Game/Generated is gitignored and produced by
	// Scripts/generate_content.py, so a developer who has not run that script must still get a
	// visible - if flat and lit - trace rather than an invisible one. No .uasset is ever a hard
	// requirement.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (TrailMaterial == nullptr && BasicFinder.Succeeded())
	{
		TrailMaterial = BasicFinder.Object;
		bTrailMaterialIsNeon = false;
	}
}

void UTraceTrailComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None on both: the holder's own client needs to see the trace it is laying just as
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
		// §3: the trace dies with its holder, instantly. ATraceCore drives this in the normal flow;
		// this is the safety net so a trace can never outlive the body that owns it and go on
		// killing a corpse.
		//
		// Checked whether or not this component is still emitting. A trace kills the player who
		// LAID it, so once that player is dead it can never kill anyone again — and by the
		// visible == lethal invariant a set of points that can never kill must not be on screen.
		// (Before, this was inside an `if (bEmitting)`, so a holder who passed the Core away and
		// then died left their trace hanging in the air, visible and completely inert.)
		{
			const ATraceCharacter* Holder = GetOwnerCharacter();
			if ((Holder == nullptr || !Holder->IsAlive()) && (bEmitting || TrailPoints.Items.Num() > 0))
			{
				SetEmitting(false);
				ClearTrail();
			}
		}

		ServerUpdateTrail();
		ServerRunTripTest(DeltaTime);
	}

	// Listen servers draw the trace too; only a headless server skips it.
	if (GetNetMode() != NM_DedicatedServer)
	{
		UpdateVisuals();

		// EVERY frame, not just on a rebuild: this depends on where the local camera is, and the
		// camera moves continuously while the geometry does not. See the function's comment.
		ApplyProximityGlowFade();
	}
}


// =================================================================================================
// Public API
// =================================================================================================

float UTraceTrailComponent::GetTraceLifetimeSeconds()
{
	return FMath::Max(0.1f, FMath::Min(UTraceSettings::Get().TrailLifetime, SpecTraceLifetimeSeconds));
}

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
		// A new holder never inherits the previous trace.
		ClearTrail();

		// Lay the first point at the transfer itself rather than one spacing later — unless the
		// §2 grace is running, in which case ServerUpdateTrail lays nothing and the trace simply
		// starts a second later, from wherever the holder has got to by then.
		ServerUpdateTrail();
	}
	else
	{
		// Not emitting means not in a grace window either. Leaving a stale deadline behind would
		// eat the first second of the NEXT emission window this component ever opens.
		EmitGraceEndServerTime = 0.f;
	}

	// Note: stopping does NOT clear. A trace left behind by a completed pass is harmless (the trip
	// test requires bEmitting) and fading out over its lifetime reads much better than popping.
	// Death is the case that must clear instantly, and it does so explicitly.

	UE_LOG(LogTraceGame, Verbose, TEXT("Trace: %s emitting for %s"),
		bEmit ? TEXT("started") : TEXT("stopped"), *GetNameSafe(GetOwner()));
}

void UTraceTrailComponent::SetEmitGrace(float Seconds)
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	EmitGraceEndServerTime = (Seconds > 0.f) ? (GetServerTimeSeconds() + Seconds) : 0.f;

	if (Seconds > 0.f)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("Trace: %.2fs grace before %s begins leaving a trace"),
			Seconds, *GetNameSafe(GetOwner()));
	}
}

bool UTraceTrailComponent::IsEmitting() const
{
	return bEmitting;
}

bool UTraceTrailComponent::IsTraceInvulnerable() const
{
	// Read straight out of the Core rather than mirrored here. §4 says the trace hardening and the
	// holder's shield loss happen "simultaneously"; the only way to guarantee that is for both to be
	// the same replicated bool, read twice.
	return ATraceCore::IsTraceInvulnerableFor(GetOwner());
}

void UTraceTrailComponent::NotifyInvulnerabilityChanged()
{
	bVisualsDirty = true;
}

int32 UTraceTrailComponent::ComputeLastLethalIndex() const
{
	const int32 PointCount = TrailPoints.Items.Num();
	if (PointCount == 0)
	{
		return -1;
	}

	// Nobody is standing on the head of a trace that has stopped growing, so there is nothing to
	// exempt: a residual trace left behind by a pass or a Core steal is lethal end to end. This is
	// half of the reported bug — see ServerRunTripTest for the other half.
	if (!bEmitting)
	{
		return PointCount - 1;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const int32 MaxExempt = FMath::Max(0, Settings.TrailHeadGracePoints);
	if (MaxExempt == 0)
	{
		return PointCount - 1;   // Exemption switched off entirely.
	}

	// WHY THE HEAD EXEMPTION IS A DISTANCE, NOT A COUNT.
	//
	// It exists for exactly one reason: the newest point is under the holder's own feet, so without
	// it a defender could stand on the emitter and dash on the spot instead of crossing anything.
	// That is a claim about ONE BODY WIDTH of trace, and it is written here as one body width —
	// TrailRadius, the lethal volume's own radius, measured back along the chain.
	//
	// Read as a point COUNT (the old code took the newest TrailHeadGracePoints=3 wholesale) it was
	// 3 x TrailPointSpacing = 180uu of trace that was drawn but could not kill, permanently trailing
	// every carrier, and 5 points' worth of travel after every turnover during which the trace was
	// visible and completely harmless. TrailHeadGracePoints still caps the exemption, so 0 removes
	// it and a larger value cannot make the invisible-but-drawn window come back: the distance
	// binds first at any sane spacing.
	const double GraceDistance = FMath::Max(0.0, static_cast<double>(Settings.TrailRadius));

	// The head point itself always counts as exempt while emitting: it is the holder's own position
	// this frame, not a place they have been.
	int32 ExemptCount = 1;
	double DistanceFromHead = 0.0;
	for (int32 Index = PointCount - 1; Index > 0 && ExemptCount < MaxExempt; --Index)
	{
		DistanceFromHead += FVector::Dist(TrailPoints.Items[Index - 1].Location, TrailPoints.Items[Index].Location);
		if (DistanceFromHead > GraceDistance)
		{
			break;
		}
		++ExemptCount;
	}

	return PointCount - 1 - ExemptCount;
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
		// multicast lets every client drop the visuals on the same frame the holder dies.
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
	HideGhostsFrom(0);

	LastVisualPointCount = -1;
	LastVisualHead = FVector::ZeroVector;
	LastVisualTail = FVector::ZeroVector;
	bVisualsDirty = true;

	// Hold the visuals down briefly: this reliable RPC can beat the property delta that empties
	// Items, and re-showing a dead holder's trace for a few frames looks like a bug. Any
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
// Server: laying the trace
// =================================================================================================

void UTraceTrailComponent::ServerUpdateTrail()
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Now = GetServerTimeSeconds();
	bool bChanged = false;

	// 1. Expire. Items are strictly ordered oldest-first, so the first survivor ends the scan.
	const float Lifetime = GetTraceLifetimeSeconds();
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

	// 3. Append, distance-gated so a stationary holder does not spam identical points.
	//
	//    §2's transfer grace lives right here: for one second after the Core changes team, the new
	//    holder is running around laying nothing. Everything else about the emission window is
	//    already true — bEmitting is set, the trip test is live — there simply are no points yet,
	//    which is exactly what "the trace has not begun to form" means.
	const bool bGraceActive = (EmitGraceEndServerTime > 0.f) && (Now < EmitGraceEndServerTime);

	if (bEmitting && !bGraceActive)
	{
		// Anchor on the capsule centre (the owner's actor location), NOT on this component's own
		// world location. The trip test measures every candidate by its actor location, so both
		// halves of the geometry have to live in the same reference frame; if the pawn ever
		// attaches this component with an offset, GetComponentLocation() would quietly slide the
		// trace away from the volume the test evaluates. Falls back for a non-character owner.
		const ATraceCharacter* Holder = GetOwnerCharacter();
		const FVector Location = Holder != nullptr ? Holder->GetActorLocation() : GetComponentLocation();

		const double Spacing = FMath::Max(1.0, static_cast<double>(Settings.TrailPointSpacing));

		const bool bHasHead = TrailPoints.Items.Num() > 0;
		const double DistanceFromHead = bHasHead
			? FVector::Dist(Location, TrailPoints.Items.Last().Location)
			: 0.0;

		// Teleport, not movement — restart rather than laying a segment across the map.
		if (bHasHead && DistanceFromHead > MaxTrailSegmentLength)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("Trace: discontinuity of %.0fuu on %s, restarting"),
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
		// own visuals down for the suppression window every time the Core changes hands.
		VisualSuppressUntilTime = 0.f;
	}
}


// =================================================================================================
// Server: the trip test — the whole game lives here
// =================================================================================================

void UTraceTrailComponent::ServerRunTripTest(float DeltaTime)
{
	ATraceCharacter* Holder = GetOwnerCharacter();

	// -------------------------------------------------------------------------------------------
	// THE INVARIANT: ONCE A TRACE SEGMENT EXISTS AND IS VISIBLE, IT IS LETHAL.
	//
	// The gate is the POINTS, not bEmitting. This is the bug the user reported: a holder who lost
	// the Core (a completed pass, or being killed and the Core changing hands) stops EMITTING, but
	// the trace they already laid stays on screen for its full lifetime — and the old test bailed
	// out on !bEmitting, so every one of those visible segments was completely inert. Dashing
	// through a trace right after a turnover did nothing, which is exactly what was described.
	//
	// A trace kills the player who laid it, so the only hard requirement is that that player is
	// still alive; if they are not, TickComponent has already wiped the points above.
	// -------------------------------------------------------------------------------------------
	if (Holder == nullptr || !Holder->IsAlive() || TrailPoints.Items.Num() == 0)
	{
		PreviousLocations.Reset();
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// -------------------------------------------------------------------------------------------
	// SPEC §4, THE RISK BEAT. From the instant the holder inputs a pass until it completes or
	// cancels, the trace CANNOT BE BROKEN. This is the whole reason the passer is willing to give
	// up their shield: for those 0.5s the dash counterplay is off the table and the only way to
	// stop the pass is to shoot them. It is the ONE intended exception to the invariant above, and
	// it is signposted: the after-images brighten by GhostInvulnerableGlowScale while it holds.
	//
	// It suppresses the KILL only — the loop below still runs, so PreviousLocations keeps tracking
	// every candidate. The old code returned early and reset them, which meant the first tick after
	// the window closed had no valid previous position for anyone; that is a sweep the teleport
	// guard throws away, i.e. the first dash after every single pass was silently swallowed.
	// -------------------------------------------------------------------------------------------
	const bool bInvulnerable = IsTraceInvulnerable();

	// Everything up to and including this index is BOTH lethal and drawn; everything after it is
	// neither. One function, one answer, used by the trip test and by RebuildVisuals.
	const int32 LastTestableIndex = ComputeLastLethalIndex();

	// Snapshot the testable positions. Nothing below may touch TrailPoints.Items: applying a kill
	// re-enters this component (death -> SetCarrying(false) -> SetEmitting/ClearTrail) and would
	// invalidate any live iteration.
	TestPositions.Reset();
	for (int32 PointIndex = 0; PointIndex <= LastTestableIndex; ++PointIndex)
	{
		TestPositions.Add(TrailPoints.Items[PointIndex].Location);
	}

	// The newest stub — the holder's own footprint — which is neither lethal nor drawn. Snapshotted
	// only so a dash that crosses it can SAY SO in the log instead of looking like a lost kill.
	ExemptPositions.Reset();
	for (int32 PointIndex = FMath::Max(0, LastTestableIndex); PointIndex < TrailPoints.Items.Num(); ++PointIndex)
	{
		ExemptPositions.Add(TrailPoints.Items[PointIndex].Location);
	}

	// Broad phase. The narrow phase below is O(candidates x segments) - with MaxTrailPoints=256 and
	// ten players that is ~2500 segment-to-segment tests every server frame, for a test that is
	// almost always a miss. One XY AABB over the whole trace turns the common case into four
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

		// The holder can never trip their own trace (grace points are not enough on their own
		// when bOnlyEnemiesTripTrail is turned off for tuning).
		if (Candidate == Holder)
		{
			continue;
		}

		// (a) alive
		if (!Candidate->IsAlive())
		{
			continue;
		}

		// (b) an enemy of the holder. Unknown teams never count as enemies; teammates never trip it.
		if (Settings.bOnlyEnemiesTripTrail)
		{
			const ETraceTeam HolderTeam = Holder->GetTeam();
			const ETraceTeam CandidateTeam = Candidate->GetTeam();
			const bool bIsEnemy = HolderTeam != ETraceTeam::None
				&& CandidateTeam != ETraceTeam::None
				&& CandidateTeam != HolderTeam;
			if (!bIsEnemy)
			{
				continue;
			}
		}

		// (c) dashing. This is the rule: walking or running through a trace does nothing at all,
		// and the dash is the only counterplay to a shielded holder.
		//
		// Sampled with a short trailing window rather than as an instant. This test ticks once per
		// server frame, but the server advances a remote client's dash clock inside MoveAutonomous
		// and can consume several client moves in one frame - so the tail of a dash (or, after a
		// hitch, all 0.18s of it) can be simulated *between* two ticks here. The displacement is
		// still credited to this frame's sweep, but DashTimeRemaining has already hit zero, and the
		// player watches themselves dash through the trace with nothing happening. The movement
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

		// The trace is a vertical volume of radius TrailRadius and height TrailHeight swept along
		// the holder's path, and the tripper is a capsule swept along its path this tick. Test
		// those two sweeps as: horizontal segment-to-segment distance (which catches tunnelling
		// at dash speed, unlike a point test), plus a separate vertical overlap check so that
		// clearing the trace in the air is not a hit.
		const double HorizontalThreshold = TrailRadius + CapsuleRadius;
		const double VerticalThreshold = TrailHalfHeight + CapsuleHalfHeight;

		// Broad phase: if this candidate's swept XY box, inflated by the same horizontal threshold
		// the narrow phase uses, does not touch the trace's XY box, no segment can be within range.
		// Only ever used to REJECT the lethal test — the exempt stub below is two points and is
		// checked unconditionally, so the instrumentation can never be broad-phased away.
		bool bNearTrace = TestPositions.Num() > 0;
		if (bNearTrace)
		{
			const double SweepMinX = FMath::Min(PreviousLocation.X, CurrentLocation.X) - HorizontalThreshold;
			const double SweepMaxX = FMath::Max(PreviousLocation.X, CurrentLocation.X) + HorizontalThreshold;
			const double SweepMinY = FMath::Min(PreviousLocation.Y, CurrentLocation.Y) - HorizontalThreshold;
			const double SweepMaxY = FMath::Max(PreviousLocation.Y, CurrentLocation.Y) + HorizontalThreshold;

			bNearTrace = !(SweepMaxX < TrailMinX || SweepMinX > TrailMaxX || SweepMaxY < TrailMinY || SweepMinY > TrailMaxY);
		}

		const bool bHitLethal = bNearTrace
			&& SweepIntersectsTrace(TestPositions, PreviousLocation, CurrentLocation, HorizontalThreshold, VerticalThreshold);

		if (bHitLethal)
		{
			if (!bInvulnerable)
			{
				Tripper = Candidate;
				continue;   // Resolved; keep looping only to refresh the remaining positions.
			}

			// The one intended exception. Logged, because "I dashed through it and nothing
			// happened" must always have an answer in the log.
			UE_LOG(LogTraceGame, Log,
				TEXT("[TRACEDASH] %s dashed through %s's trace: NO KILL (pass window invulnerable, spec 4). points=%d lethal=%d emitting=%d"),
				*GetNameSafe(Candidate), *GetNameSafe(Holder),
				TrailPoints.Items.Num(), TestPositions.Num(), bEmitting ? 1 : 0);
			continue;
		}

		// Did they cross the stub that is neither drawn nor lethal? If this ever fires the visible
		// state and the lethal state have drifted apart, which is the whole bug class this pass
		// exists to close — so it is reported at Log, not at Verbose.
		if (ExemptPositions.Num() > 1
			&& SweepIntersectsTrace(ExemptPositions, PreviousLocation, CurrentLocation, HorizontalThreshold, VerticalThreshold))
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[TRACEDASH] %s dashed through the NON-DRAWN head stub of %s's trace: NO KILL (emitter footprint, %.0fuu). points=%d lethal=%d"),
				*GetNameSafe(Candidate), *GetNameSafe(Holder),
				static_cast<double>(Settings.TrailRadius), TrailPoints.Items.Num(), TestPositions.Num());
		}
	}

	// Applied outside every loop above: this kills, which re-enters the component and mutates
	// TrailPoints.Items and PreviousLocations.
	if (Tripper != nullptr)
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("[TRACEDASH] %s dashed through %s's trace: KILL. points=%d lethal=%d emitting=%d (residual trace: %s)"),
			*GetNameSafe(Tripper), *GetNameSafe(Holder),
			TrailPoints.Items.Num(), TestPositions.Num(), bEmitting ? 1 : 0,
			bEmitting ? TEXT("no") : TEXT("yes - laid before a turnover"));

		ApplyTrailTrip(Holder, Tripper);
	}
}

bool UTraceTrailComponent::SweepIntersectsTrace(const TArray<FVector>& Positions, const FVector& PreviousLocation,
	const FVector& CurrentLocation, double HorizontalThreshold, double VerticalThreshold) const
{
	if (Positions.Num() == 0)
	{
		return false;
	}

	const double HorizontalThresholdSquared = HorizontalThreshold * HorizontalThreshold;

	const FVector SweepStart(PreviousLocation.X, PreviousLocation.Y, 0.0);
	const FVector SweepEnd(CurrentLocation.X, CurrentLocation.Y, 0.0);

	// A single point is tested as a zero-length segment rather than skipped. The old code needed
	// two testable points before ANYTHING was lethal, which — stacked on the head exemption — meant
	// a freshly formed trace was drawn but harmless for its first few points. SegmentDistToSegment
	// handles a degenerate segment correctly, so there is no reason for that hole to exist.
	const int32 LastSegment = FMath::Max(0, Positions.Num() - 2);

	for (int32 SegmentIndex = 0; SegmentIndex <= LastSegment; ++SegmentIndex)
	{
		const FVector& TrailStart = Positions[SegmentIndex];
		const FVector& TrailEnd = Positions[FMath::Min(SegmentIndex + 1, Positions.Num() - 1)];

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

		return true;
	}

	return false;
}

void UTraceTrailComponent::ApplyTrailTrip(ATraceCharacter* Holder, ATraceCharacter* Tripper)
{
	if (Holder == nullptr || Tripper == nullptr)
	{
		return;
	}

	// Resolve both controllers first: the first Kill() may unpossess the pawn we would otherwise
	// ask for a controller afterwards.
	AController* TripperController = Tripper->GetController();
	AController* HolderController = Holder->GetController();

	/** Cause tag reported to the GameMode / kill feed for a trace death. */
	static const FName TrailDeathCause(TEXT("Trail"));

	const ETrailLethality Lethality = UTraceSettings::Get().TrailLethality;

	UE_LOG(LogTraceGame, Log, TEXT("Trace broken: %s dashed through %s's trace (lethality %d)"),
		*GetNameSafe(Tripper), *GetNameSafe(Holder), static_cast<int32>(Lethality));

	if (Lethality == ETrailLethality::KillsCarrier || Lethality == ETrailLethality::KillsBoth)
	{
		// Kill(), never ApplyDamage(): the holder is shielded against damage by design, and the
		// trace is the one thing that gets through. This is the whole point of the mechanic.
		//
		// TripperController is what carries the §2 transfer: ATraceCore listens to this health
		// component's OnDeath and hands the Core to whoever is credited here. "The core transfers
		// to the enemy who breaks your trace" is implemented by this argument.
		if (UTraceHealthComponent* HolderHealth = Holder->Health)
		{
			HolderHealth->Kill(TripperController, TrailDeathCause);
		}
	}

	if (Lethality == ETrailLethality::KillsToucher || Lethality == ETrailLethality::KillsBoth)
	{
		if (UTraceHealthComponent* TripperHealth = Tripper->Health)
		{
			TripperHealth->Kill(HolderController, TrailDeathCause);
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
			if (ATraceCharacter* TraceChar = WeakCharacter.Get())
			{
				OutCharacters.Add(TraceChar);
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
		if (ATraceCharacter* TraceChar = *It)
		{
			OutCharacters.Add(TraceChar);
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
//
// SPEC §3: "a blur created where your character model has passed through".
//
// The previous implementation extruded one continuous wall between consecutive points, with a hot
// strip along its top edge. It was readable, but it read as a fence — a solid barrier the carrier
// had built — which is the wrong mental model for a thing you are supposed to run at and dash
// through, and it is not what the design doc asks for.
//
// What is drawn now is a chain of CHARACTER-SHAPED after-images: at every trail point, a coarse
// three-part silhouette (legs / torso / head) oriented along the direction of travel, fading with
// age. Consecutive points are 60uu apart and each silhouette is deeper than that along its own axis,
// so they overlap into a continuous smear rather than a dotted line of statues — which is what makes
// it read as motion blur instead of as a crowd.
//
// Two properties were preserved deliberately, because both were measured and both are load-bearing:
//
//   1. The silhouette spans EXACTLY the lethal volume (TrailRadius wide, TrailHeight tall, centred
//      on the point). What you dash at is what kills you.
//   2. The HEAD is the hottest part, and it sits at first-person eye height. A previous pass
//      established that a trace has to be readable from a player's own eyeline at range; a bright
//      element at that height is the feature that survives the projection when everything else
//      collapses edge-on.
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
	const bool bInvulnerable = IsTraceInvulnerable();

	if (!bVisualsDirty
		&& PointCount == LastVisualPointCount
		&& bInvulnerable == bLastVisualInvulnerable
		&& bEmitting == bLastVisualEmitting
		&& Head.Equals(LastVisualHead, 0.01)
		&& Tail.Equals(LastVisualTail, 0.01))
	{
		return;
	}

	bVisualsDirty = false;
	LastVisualPointCount = PointCount;
	LastVisualHead = Head;
	LastVisualTail = Tail;
	bLastVisualInvulnerable = bInvulnerable;

	// bEmitting is in the comparison above because ComputeLastLethalIndex() reads it: the frame a
	// holder stops emitting, the stub under their feet becomes lethal and must become visible with
	// it. Nothing else about the point set changes on that frame, so without this the rebuild would
	// wait for the next expiry and the trace would be lethal-but-invisible in between.
	bLastVisualEmitting = bEmitting;

	RebuildVisuals();
}

void UTraceTrailComponent::RebuildVisuals()
{
	// THE OTHER HALF OF THE INVARIANT. What is drawn is exactly the lethal set — not the whole
	// point array — so a player can never be shown a segment that would not have killed them.
	// ComputeLastLethalIndex() is the same function the server's trip test runs off, and it reads
	// only replicated state, so this client's answer is the server's answer.
	const int32 PointCount = ComputeLastLethalIndex() + 1;
	if (PointCount <= 0 || CylinderMesh == nullptr)
	{
		HideGhostsFrom(0);
		return;
	}

	CacheMeshMetrics();

	const UTraceSettings& Settings = UTraceSettings::Get();

	// The silhouette is derived from the lethal volume, never chosen. If TrailRadius/TrailHeight are
	// retuned the after-image moves with them.
	const double Width = FMath::Max(1.0, 2.0 * static_cast<double>(Settings.TrailRadius));
	const double Height = FMath::Max(1.0, static_cast<double>(Settings.TrailHeight));
	const double HalfHeight = Height * 0.5;
	const double Spacing = FMath::Max(1.0, static_cast<double>(Settings.TrailPointSpacing));

	// Depth along the direction of travel. Kept above the point spacing so consecutive after-images
	// interpenetrate and the chain reads as one smear rather than as separate figures.
	const double Depth = FMath::Max(Width * 0.62, Spacing * 1.35);

	const float Lifetime = GetTraceLifetimeSeconds();
	const float Now = GetServerTimeSeconds();
	const float InvulnerableScale = IsTraceInvulnerable() ? GhostInvulnerableGlowScale : 1.f;

	// --- Hide the newest stretch of the trace from the holder's OWN eyes -------------------------
	//
	// Holding the Core is what puts the camera into third person, and third person parks it
	// ThirdPersonArmLength straight back down the path the holder just walked — which is exactly
	// where this component is placing unlit emissive geometry. Raising the camera above the trace
	// (see ATraceCharacter::GetThirdPersonPivotZ) stops it being INSIDE it, but the freshest
	// after-images are still hot surfaces a few tens of uu below the lens: they blew out the bottom
	// third of the frame and, worse, drowned the player's own character in glare.
	//
	// SetOwnerNoSee hides a primitive from ONE viewer — the one whose view target owns it — so this
	// costs every other player nothing. They still see the whole trace, including the part its own
	// holder cannot, and the LETHAL VOLUME IS UNTOUCHED: trip resolution runs off TrailPoints, never
	// off what happens to be rendered. A holder cannot trip their own trace anyway, so nothing is
	// being hidden that its owner could act on.
	int32 FirstOwnerHiddenPoint = PointCount;   // == PointCount means "hide nothing"
	if (const ATraceCharacter* OwnerCharacter = GetOwnerCharacter())
	{
		const APlayerController* OwnerPC = Cast<APlayerController>(OwnerCharacter->GetController());
		if (OwnerPC != nullptr && OwnerPC->IsLocalPlayerController())
		{
			double DistanceFromHead = 0.0;
			FirstOwnerHiddenPoint = 0;
			for (int32 Index = PointCount - 1; Index >= 0; --Index)
			{
				if (DistanceFromHead >= OwnerNearHideDistance)
				{
					FirstOwnerHiddenPoint = Index + 1;
					break;
				}
				if (Index > 0)
				{
					DistanceFromHead += FVector::Dist(TrailPoints.Items[Index - 1].Location, TrailPoints.Items[Index].Location);
				}
			}
		}
	}

	// If there are more points than the pool can draw, drop the OLDEST. UTraceSettings::MaxTrailPoints
	// is 256 and the pool caps at 96, so this is reachable if the point spacing is ever lowered — and
	// truncating from the wrong end would hide the freshest stretch of the trace, which is the part
	// an approaching enemy is judging and the part nearest the holder. The tail simply fades early.
	const int32 FirstDrawnPoint = FMath::Max(0, PointCount - MaxPooledGhosts);

	int32 PlacedGhosts = 0;

	for (int32 PointIndex = FirstDrawnPoint; PointIndex < PointCount; ++PointIndex)
	{
		if (!EnsureGhost(PlacedGhosts))
		{
			break;   // Pool cap hit — draw what we have.
		}

		const FVector Centre = TrailPoints.Items[PointIndex].Location;

		// Facing: the direction the holder was travelling when this after-image was left. Taken from
		// the segment that ENDS here where one exists, so the first point borrows the second's.
		FVector Along = FVector::ZeroVector;
		if (PointIndex > 0)
		{
			Along = Centre - FVector(TrailPoints.Items[PointIndex - 1].Location);
		}
		else if (PointCount > 1)
		{
			Along = FVector(TrailPoints.Items[1].Location) - Centre;
		}
		Along.Z = 0.0;
		const FRotator Facing = (Along.SizeSquared() > 1.0) ? Along.GetSafeNormal().Rotation() : FRotator::ZeroRotator;

		// Age fade. Never to zero: an old after-image is exactly as lethal as a new one, so it has
		// to stay clearly visible — this is a "cooling" cue, not a disappearance.
		const float Age = FMath::Max(0.f, Now - TrailPoints.Items[PointIndex].BirthServerTime);
		const float Remaining = FMath::Clamp(1.f - (Age / FMath::Max(0.01f, Lifetime)), 0.f, 1.f);
		const float FadeScale = FMath::Lerp(GhostOldestGlowScale, 1.f, Remaining);

		const bool bHideFromOwner = (PointIndex >= FirstOwnerHiddenPoint);

		for (int32 Part = 0; Part < PartsPerGhost; ++Part)
		{
			UStaticMeshComponent* Piece = GhostMeshes[PlacedGhosts * PartsPerGhost + Part];
			if (Piece == nullptr)
			{
				continue;
			}

			double CentreFrac = GhostTorsoCentreFrac;
			double HeightFrac = GhostTorsoHeightFrac;
			double WidthFrac = GhostTorsoWidthFrac;
			float BaseGlow = GhostTorsoGlow;

			if (Part == PartLegs)
			{
				CentreFrac = GhostLegCentreFrac;
				HeightFrac = GhostLegHeightFrac;
				WidthFrac = GhostLegWidthFrac;
				BaseGlow = GhostLegGlow;
			}
			else if (Part == PartHead)
			{
				CentreFrac = GhostHeadCentreFrac;
				HeightFrac = GhostHeadHeightFrac;
				WidthFrac = GhostHeadWidthFrac;
				BaseGlow = GhostHeadGlow;
			}

			const bool bIsHead = (Part == PartHead);
			const FVector& MeshHalfSize = bIsHead ? SphereHalfSize : CylinderHalfSize;
			const FVector& MeshPivot = bIsHead ? SpherePivotOffset : CylinderPivotOffset;

			const FVector DesiredSize(
				Depth * WidthFrac,
				Width * WidthFrac,
				Height * HeightFrac);

			const FVector Scale(
				DesiredSize.X / (2.0 * MeshHalfSize.X),
				DesiredSize.Y / (2.0 * MeshHalfSize.Y),
				DesiredSize.Z / (2.0 * MeshHalfSize.Z));

			// Measured from the BOTTOM of the lethal volume, so the three parts tile it exactly.
			const FVector PartCentre = Centre + FVector(0.0, 0.0, -HalfHeight + Height * CentreFrac);

			// Corrects for a source mesh whose pivot is not at its bounds centre, so we never have
			// to assume anything about the engine primitives' authoring.
			const FVector PivotCorrection = Facing.RotateVector(MeshPivot * Scale);

			Piece->SetWorldLocationAndRotation(PartCentre - PivotCorrection, Facing);
			Piece->SetWorldScale3D(Scale);
			Piece->SetVisibility(true);

			// Guarded: SetOwnerNoSee dirties the render state, and this runs as the trace grows.
			if (Piece->bOwnerNoSee != bHideFromOwner)
			{
				Piece->SetOwnerNoSee(bHideFromOwner);
			}

			// The intended brightness of this piece, BEFORE the camera-proximity fade. Recorded rather
			// than pushed directly, because ApplyProximityGlowFade() runs every frame and needs to
			// know what full brightness means for this piece without re-deriving the whole rebuild.
			const int32 SlotIndex = PlacedGhosts * PartsPerGhost + Part;
			if (GhostBaseGlow.Num() <= SlotIndex)
			{
				GhostBaseGlow.SetNumZeroed(SlotIndex + 1);
				GhostAppliedGlowScale.SetNumZeroed(SlotIndex + 1);
			}
			GhostBaseGlow[SlotIndex] = BaseGlow * FadeScale * InvulnerableScale;

			if (bTrailMaterialIsNeon)
			{
				if (UMaterialInstanceDynamic* Material = GhostMaterials[SlotIndex])
				{
					// Push full brightness and let the proximity pass pull it down. Resetting the
					// remembered scale forces that pass to re-evaluate this piece, which it must:
					// the piece has just been moved somewhere else entirely.
					Material->SetScalarParameterValue(TEXT("Glow"), GhostBaseGlow[SlotIndex]);
					GhostAppliedGlowScale[SlotIndex] = 1.f;
				}
			}
		}

		++PlacedGhosts;
	}

	HideGhostsFrom(PlacedGhosts);
}

void UTraceTrailComponent::ApplyProximityGlowFade()
{
	// ------------------------------------------------------------------------------------------
	// WHY THIS EXISTS
	//
	// This project has a measured, named defect: point-blank whiteout from unlit emissive surfaces.
	// The arena's version of it was fixed by STANDOFF — geometry gained a pawn-blocking shell so an
	// eye can never get close enough (see ATraceArenaBuilder::AddPawnStandoff). That fix is not
	// available here and must not be: walking through a trace has to stay free, because only a DASH
	// may trip it (spec §3). So a player can and will stand with their eye INSIDE an after-image.
	//
	// M_TraceNeon is unlit: it emits Color * Glow with no distance term whatsoever, so a head piece
	// at Glow 4.2 arrives at the lens at full intensity and fills the frame. A full-field walk
	// measured two frames out of 108 blown past 40% (52.7% and 43.0%), and BOTH were a player
	// standing inside somebody else's trace — after the arena fix, the only remaining source.
	//
	// So bound the SOLID ANGLE instead of the standoff: attenuate a piece's emissive by how close
	// the local camera is to it. Far away nothing changes at all, which is the whole point — the
	// trace's readability across the arena is a measured win and is untouched.
	//
	// STRICTLY LOCAL AND STRICTLY COSMETIC. It reads this machine's camera, writes only this
	// machine's material instances, and the lethal volume is TrailPoints — which this function does
	// not touch and the trip test never renders. Two players standing in the same trace see their
	// own fade and die to exactly the same geometry.
	// ------------------------------------------------------------------------------------------
	if (!bTrailMaterialIsNeon || GhostMeshes.Num() == 0)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// The local viewpoint. On a listen host with nine bots there is exactly one, and a dedicated
	// server never reaches here.
	const APlayerController* LocalPC = GEngine != nullptr ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	if (LocalPC == nullptr || LocalPC->PlayerCameraManager == nullptr)
	{
		return;
	}
	const FVector CameraLocation = LocalPC->PlayerCameraManager->GetCameraLocation();

	const int32 SlotCount = FMath::Min(GhostMeshes.Num(), GhostMaterials.Num());
	for (int32 Slot = 0; Slot < SlotCount; ++Slot)
	{
		UStaticMeshComponent* Piece = GhostMeshes[Slot];
		UMaterialInstanceDynamic* Material = GhostMaterials[Slot];
		if (Piece == nullptr || Material == nullptr || !Piece->IsVisible())
		{
			continue;
		}
		if (!GhostBaseGlow.IsValidIndex(Slot))
		{
			continue;
		}

		// Distance to the piece's SURFACE, not its centre: these are wide, flat slabs, and a centre
		// distance would report a torso as far away at the exact moment its face is against the lens.
		// The bounds are already computed for culling, so this costs nothing extra.
		const FBoxSphereBounds& LocalBounds = Piece->Bounds;
		const double SurfaceDistance = FMath::Max(0.0,
			FVector::Dist(CameraLocation, LocalBounds.Origin) - LocalBounds.SphereRadius);

		float Scale = 1.f;
		if (SurfaceDistance < ProximityFadeFarDistance)
		{
			// Smooth, so a piece does not pop as the player walks past it. The floor is not zero:
			// an after-image you are standing inside is exactly as lethal as one across the field,
			// and a player must still be able to see that they are in it.
			const float T = FMath::Clamp(
				static_cast<float>((SurfaceDistance - ProximityFadeNearDistance)
					/ FMath::Max(1.0, ProximityFadeFarDistance - ProximityFadeNearDistance)),
				0.f, 1.f);
			Scale = FMath::Lerp(ProximityFadeMinScale, 1.f, FMath::InterpEaseIn(0.f, 1.f, T, 2.f));
		}

		// Only touch the material when the change is visible. Without this every pooled piece would
		// dirty its render state every frame for the entire life of the trace.
		if (!GhostAppliedGlowScale.IsValidIndex(Slot))
		{
			GhostAppliedGlowScale.SetNumZeroed(Slot + 1);
		}
		if (FMath::IsNearlyEqual(GhostAppliedGlowScale[Slot], Scale, 0.02f))
		{
			continue;
		}

		GhostAppliedGlowScale[Slot] = Scale;
		Material->SetScalarParameterValue(TEXT("Glow"), GhostBaseGlow[Slot] * Scale);
	}
}

bool UTraceTrailComponent::EnsureGhost(int32 GhostIndex)
{
	if (GhostIndex < 0 || GhostIndex >= MaxPooledGhosts)
	{
		return false;
	}

	const int32 RequiredNum = (GhostIndex + 1) * PartsPerGhost;
	if (GhostMeshes.Num() >= RequiredNum)
	{
		return true;
	}

	// The pool only ever grows one whole after-image at a time, in order, so the interleaving
	// (legs / torso / head) can never slip.
	if (GhostMeshes.Num() != GhostIndex * PartsPerGhost)
	{
		return false;
	}

	for (int32 Part = 0; Part < PartsPerGhost; ++Part)
	{
		UStaticMesh* SourceMesh = (Part == PartHead) ? SphereMesh.Get() : CylinderMesh.Get();
		if (SourceMesh == nullptr)
		{
			SourceMesh = CylinderMesh.Get();   // A missing sphere just means a blockier head.
		}

		UMaterialInstanceDynamic* Material = nullptr;
		UStaticMeshComponent* Piece = CreatePooledMesh(SourceMesh, Material);

		// A null entry is skipped harmlessly in RebuildVisuals, whereas a SHORT array would
		// silently pair one after-image's head with the next one's legs.
		GhostMeshes.Add(Piece);
		GhostMaterials.Add(Material);
	}

	return true;
}

UStaticMeshComponent* UTraceTrailComponent::CreatePooledMesh(UStaticMesh* SourceMesh, UMaterialInstanceDynamic*& OutMaterial)
{
	OutMaterial = nullptr;

	AActor* Owner = GetOwner();
	if (Owner == nullptr || SourceMesh == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* NewMesh = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
	if (NewMesh == nullptr)
	{
		return nullptr;
	}

	// Mobility must be set before registration.
	NewMesh->SetMobility(EComponentMobility::Movable);
	NewMesh->SetStaticMesh(SourceMesh);
	NewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	NewMesh->SetCollisionProfileName(TEXT("NoCollision"));
	NewMesh->SetGenerateOverlapEvents(false);
	NewMesh->SetCanEverAffectNavigation(false);
	NewMesh->SetCastShadow(false);
	NewMesh->bReceivesDecals = false;
	NewMesh->SetIsReplicated(false);   // Purely local cosmetics, rebuilt from TrailPoints.
	NewMesh->SetVisibility(false);

	NewMesh->RegisterComponent();
	NewMesh->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);

	// Critical: the trace is laid in WORLD space and must not follow the holder around. Absolute
	// transforms keep the components in the actor's hierarchy (so they are cleaned up with it)
	// while making them ignore the parent transform entirely.
	NewMesh->SetAbsolute(true, true, true);

	if (TrailMaterial != nullptr)
	{
		OutMaterial = NewMesh->CreateDynamicMaterialInstance(0, TrailMaterial);
	}

	if (OutMaterial != nullptr)
	{
		if (!bTrailMaterialIsNeon)
		{
			// BasicShapeMaterial fallback: lit, no Glow, so the best available approximation is a
			// bright matte albedo. It will not bloom and it will not read as light — that is the
			// cost of not having run Scripts/generate_content.py.
			OutMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
		}

		// Newly created components need the colour that the pool already agreed on.
		if (bColorApplied)
		{
			OutMaterial->SetVectorParameterValue(TEXT("Color"), AppliedColor);
			OutMaterial->SetVectorParameterValue(TEXT("BaseColor"), AppliedColor);
		}
	}

	return NewMesh;
}

void UTraceTrailComponent::HideGhostsFrom(int32 FirstGhostIndex)
{
	for (int32 Index = FMath::Max(0, FirstGhostIndex) * PartsPerGhost; Index < GhostMeshes.Num(); ++Index)
	{
		if (UStaticMeshComponent* Piece = GhostMeshes[Index])
		{
			Piece->SetVisibility(false);
		}
	}
}

void UTraceTrailComponent::UpdateTeamColor()
{
	FLinearColor Desired = TraceTeamColor(ETraceTeam::None);
	if (const ATraceCharacter* TraceChar = GetOwnerCharacter())
	{
		Desired = TraceTeamColor(TraceChar->GetTeam());
	}
	Desired.A = 1.f;

	if (bColorApplied && Desired.Equals(AppliedColor, 0.001f))
	{
		return;
	}

	AppliedColor = Desired;
	bColorApplied = true;

	// "Color" is the real vector parameter on both M_TraceNeon and BasicShapeMaterial; "BaseColor" is
	// a defensive second guess and is a silent no-op if the parameter does not exist.
	//
	// The head keeps the team colour rather than going white. A white element would read as "generic
	// hazard"; the whole point is that a glance tells you WHOSE trace it is, and therefore whether
	// dashing through it kills their holder or does nothing at all.
	for (UMaterialInstanceDynamic* Material : GhostMaterials)
	{
		if (Material != nullptr)
		{
			Material->SetVectorParameterValue(TEXT("Color"), AppliedColor);
			Material->SetVectorParameterValue(TEXT("BaseColor"), AppliedColor);
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
		const FBoxSphereBounds LocalBounds = CylinderMesh->GetBounds();
		CylinderHalfSize = LocalBounds.BoxExtent;
		CylinderPivotOffset = LocalBounds.Origin;
	}

	if (SphereMesh != nullptr)
	{
		const FBoxSphereBounds LocalBounds = SphereMesh->GetBounds();
		SphereHalfSize = LocalBounds.BoxExtent;
		SpherePivotOffset = LocalBounds.Origin;
	}

	// Never divide by zero, whatever the assets turn out to be.
	CylinderHalfSize.X = FMath::Max(CylinderHalfSize.X, 1.0);
	CylinderHalfSize.Y = FMath::Max(CylinderHalfSize.Y, 1.0);
	CylinderHalfSize.Z = FMath::Max(CylinderHalfSize.Z, 1.0);
	SphereHalfSize.X = FMath::Max(SphereHalfSize.X, 1.0);
	SphereHalfSize.Y = FMath::Max(SphereHalfSize.Y, 1.0);
	SphereHalfSize.Z = FMath::Max(SphereHalfSize.Z, 1.0);

	bMeshMetricsCached = true;
}

void UTraceTrailComponent::DestroyVisualPool()
{
	for (UStaticMeshComponent* Piece : GhostMeshes)
	{
		if (Piece != nullptr)
		{
			Piece->DestroyComponent();
		}
	}

	GhostMeshes.Reset();
	GhostMaterials.Reset();

	LastVisualPointCount = -1;
	bColorApplied = false;
}
