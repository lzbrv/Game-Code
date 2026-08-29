// Trace — Rocco's Ripple. See the header for the clause-by-clause reading of spec v14 §6.

#include "Abilities/Characters/TraceRippleActor.h"

#include "Components/AudioComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Audio/TraceAudio.h"                             // §2.9 — the RoccoRideLoop attach
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceFxShapes.h"                       // the shared primitives + the blend ladder
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceRippleTuning
{
	/** Beads per ring. Twenty reads as a ring at 90 uu radius without being a mesh budget. */
	constexpr int32 BeadsPerRing = 20;

	/** Bead cross-section, uu. /Engine/BasicShapes primitives are 100 uu across. */
	constexpr float BeadThicknessUU = 14.f;

	/** Rings, including the start ring. Capped so a long path does not become a tunnel. */
	constexpr int32 MaxRings = 12;

	/**
	 * Emissive strength for M_TraceNeon. UNCHANGED by the FX pass, deliberately: ART_BIBLE §3.2's
	 * Glow ladder names "ripple rings 3.5" by name in its T2 wayfinding band, so this number is
	 * canon rather than a leftover. The FX pass moved the two HUES and left the brightness alone.
	 */
	constexpr float RingGlow = 3.5f;

	// --- FX_AUDIO_PLAN §2.9: the start-ring pulse -------------------------------------------------

	/** §2.9 / bible §3.3: 0.8 Hz. Faster than anything in the world, so it cannot be read as scenery. */
	constexpr float StartPulseRateHz = 0.8f;

	/** §2.9 / bible §3.3: ±15% of Glow. The one permitted GAMEPLAY pulse in the project. */
	constexpr float StartPulseAmp = 0.15f;

	// --- FX_AUDIO_PLAN §2.9: the expiry dissolve --------------------------------------------------

	/** §2.9: "all rings fade Glow -> 0 over 0.3 s before destroy". Bible §6.4 — never a pop-out. */
	constexpr float DissolveSeconds = 0.3f;

	// --- FX_AUDIO_PLAN §2.9: the ride FX ----------------------------------------------------------

	/** §2.9: "3 speed-line cylinders". */
	constexpr int32 SpeedLinesPerRider = 3;

	/** §2.9: "l 120 uu". */
	constexpr float SpeedLineLengthUU = 120.f;

	/** §2.9: "r 4 uu" — 8 uu across, exactly bible §3.4's floor for world-readable geometry. */
	constexpr float SpeedLineRadiusUU = 4.f;

	/**
	 * *** §2.9 ASKS FOR "additive amber I 0.4"; THE LINES ARE EMISSIVE INSTEAD, FOR A MEASURED
	 * REASON THAT IS NOT ABOUT TASTE. ***
	 *
	 * The engine's additive material carries no InstancedStaticMeshes usage flag, so an additive
	 * piece drawn through a UInstancedStaticMeshComponent is silently replaced by the ENGINE DEFAULT
	 * MATERIAL in game — grey, lit, checker-textured. The renderer says so in one line, and this
	 * project's own capture runs photographed it happening to Mortimer's quake dust:
	 *
	 *     LogMaterial: Warning: Material /Engine/EngineMaterials/EmissiveMeshMaterial missing usage
	 *     flag InstancedStaticMeshes! Default Material will be used in game.
	 *
	 * MakeGlowMID cannot see that: it reports the blend it RESOLVED, and the substitution happens
	 * later. So "additive" and "instanced" are mutually exclusive here until somebody edits an engine
	 * asset, and every instanced FX element in this project is already Emissive for exactly this
	 * reason (W3-FXBURST moved its sparks, spokes, speed lines, blobs and ring beads there).
	 *
	 * 1.2 is bible §3.2's T0 ambient-detail band — well under the rings' own 3.5, so the wake stays
	 * subordinate to the path it is drawn along, which is what "I 0.4" was asking for.
	 */
	constexpr float SpeedLineGlow = 1.2f;

	/**
	 * How many riders this machine will DRAW at once. Four is two more than a ripple has ever had:
	 * the pool is fixed so instances are transformed rather than added and removed, and a fifth
	 * rider simply gets the sound and no speed lines rather than a reallocation mid-frame.
	 */
	constexpr int32 MaxPresentedRiders = 4;

	/** How far off the path's axis a pawn may be and still LOOK like it is riding, uu. */
	constexpr float RideCorridorUU = 220.f;

	/** A rider's velocity must point this closely along the path. cos(25 deg). */
	constexpr float RideAlignmentDot = 0.906f;

	/** ...and carry at least this fraction of the ride speed. Below it, he is merely walking that way. */
	constexpr float RideSpeedFraction = 0.6f;
}

// =================================================================================================
// Construction
// =================================================================================================

ATraceRippleActor::ATraceRippleActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	bReplicates = true;
	SetReplicateMovement(false);      // it never moves; replicating a static transform is pure cost
	bAlwaysRelevant = true;           // a 4 s path across the arena must not be culled off a client
	SetNetUpdateFrequency(10.f);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	auto MakeRingMesh = [this](const TCHAR* Name) -> UInstancedStaticMeshComponent*
	{
		UInstancedStaticMeshComponent* Mesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		Mesh->SetupAttachment(Root);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCollisionProfileName(TEXT("NoCollision"));
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(false);
		Mesh->bReceivesDecals = false;
		return Mesh;
	};

	StartRingMesh = MakeRingMesh(TEXT("StartRingMesh"));
	TrailRingMesh = MakeRingMesh(TEXT("TrailRingMesh"));
	RideFxMesh    = MakeRingMesh(TEXT("RideFxMesh"));

	// THE MESH AND THE MATERIALS ARE NOT FOUND HERE ANY MORE. UTraceFxShapes holds both on its own
	// CDO — one ConstructorHelpers pass for the whole project — and hands them out through static
	// accessors that are safe to call outside a constructor, so BuildRingsIfNeeded asks for them at
	// the moment it needs them. What that buys over the two finders this replaced is the degradation
	// LADDER: MakeGlowMID reports the blend it ACTUALLY achieved (Emissive, then Fallback, then None)
	// and on None the component is HIDDEN, where the old code would have drawn an untextured 100 uu
	// grey cylinder ring across the arena. MAP_PLAN §9's committed-parent path lives in that one
	// place now instead of in every actor that wants to glow.
}

void ATraceRippleActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceRippleActor, RippleStart);
	DOREPLIFETIME(ATraceRippleActor, RippleDirection);
	DOREPLIFETIME(ATraceRippleActor, PathLength);
	DOREPLIFETIME(ATraceRippleActor, RideSpeed);
	DOREPLIFETIME(ATraceRippleActor, EntryRadius);
	DOREPLIFETIME(ATraceRippleActor, ExpireMatchTime);
	DOREPLIFETIME(ATraceRippleActor, SourcePlayerState);
}

void ATraceRippleActor::InitialiseRipple(APlayerState* InSource, const FVector& InStart, const FVector& InDirection,
                                         float InPathLength, float InRideSpeed, float InEntryRadius,
                                         float InExpireMatchTime)
{
	SourcePlayerState = InSource;
	RippleStart       = InStart;
	RippleDirection   = InDirection.GetSafeNormal();
	PathLength        = FMath::Max(0.f, InPathLength);
	RideSpeed         = FMath::Max(0.f, InRideSpeed);
	EntryRadius       = FMath::Max(0.f, InEntryRadius);
	ExpireMatchTime   = InExpireMatchTime;

	SetActorLocation(InStart);
	SetActorRotation(FRotator::ZeroRotator);
}

void ATraceRippleActor::BeginPlay()
{
	Super::BeginPlay();

	// On the server everything is already set by InitialiseRipple. On a client the replicated
	// properties may not have landed yet, so the rings are built from Tick instead — see
	// BuildRingsIfNeeded, which is idempotent and cheap until the path arrives.
	BuildRingsIfNeeded();
}

void ATraceRippleActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// THE LOOPS ARE THE ONE THING HERE THAT DOES NOT CLEAN ITSELF UP. Every visual is a component of
	// this actor and dies with it; a UAudioComponent started by TraceAudio::StartLoopOn is attached
	// to the RIDER, has bAutoDestroy off (the caller owns it, §1.6.4), and would keep playing on a
	// pawn that has long since left a ripple that no longer exists.
	StopAllRideLoops();

	Super::EndPlay(EndPlayReason);
}

float ATraceRippleActor::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceRippleActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	BuildRingsIfNeeded();
	UpdateRides(DeltaSeconds);

	// COSMETIC HALF, ON EVERY MACHINE THAT RENDERS. Deliberately AFTER UpdateRides so that on a
	// machine which simulates a rider, the speed lines are drawn at the position that rider was just
	// moved to rather than one frame behind it. UpdateRides may have called Destroy() at the
	// deadline, in which case IsActorBeingDestroyed() is the honest test — the actor is still alive
	// enough to tick out this frame and there is nothing left to present.
	if (!IsActorBeingDestroyed() && GetNetMode() != NM_DedicatedServer)
	{
		UpdateRideFx();
	}
}

bool ATraceRippleActor::ShouldSimulate(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr)
	{
		return false;
	}

	// The server owns every pawn's truth. A client owns exactly one pawn's prediction. A simulated
	// proxy is nobody's to push — its motion arrives by replication like everything else.
	return HasAuthority() || Candidate->IsLocallyControlled();
}

bool ATraceRippleActor::IsRiding(const ATraceCharacter* Candidate) const
{
	for (const FRippleRider& Rider : Riders)
	{
		if (Rider.Pawn.Get() == Candidate)
		{
			return true;
		}
	}
	return false;
}

void ATraceRippleActor::UpdateRides(float DeltaSeconds)
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	const float Now = MatchTimeNow();

	// "Lasts 4 s, then all effects and visuals vanish." One deadline, one destroy, and the visuals
	// leave with the actor rather than being faded out by a second timer that could disagree.
	//
	// TESTED BEFORE THE PATH-LENGTH GUARD BELOW, DELIBERATELY: a ripple that somehow reached the
	// world with a zero-length path would otherwise take the early return every tick and never be
	// destroyed. The expiry is the one thing that must not depend on the ripple being well formed.
	if (HasAuthority() && ExpireMatchTime > 0.f && Now >= ExpireMatchTime)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Ripple] expired at match time %.2f after %d rider(s)."),
			Now, LifetimeRiders.Num());
		Destroy();
		return;
	}

	if (PathLength <= 0.f)
	{
		return;   // on a client, until the replicated path arrives
	}

	const FVector Direction = FVector(RippleDirection);

	// ---- advance the pawns already riding --------------------------------------------------------
	for (int32 Index = Riders.Num() - 1; Index >= 0; --Index)
	{
		ATraceCharacter* RiderPawn = Riders[Index].Pawn.Get();
		UTraceCharacterMovementComponent* Move = (RiderPawn != nullptr) ? RiderPawn->GetTraceMovement() : nullptr;

		const bool bStillValid = (RiderPawn != nullptr) && RiderPawn->IsAlive() && (Move != nullptr)
			&& ShouldSimulate(RiderPawn);

		Riders[Index].DistanceTravelled += RideSpeed * DeltaSeconds;

		const bool bFinished = !bStillValid
			|| (Riders[Index].DistanceTravelled >= PathLength)
			|| (Now >= ExpireMatchTime);

		if (bFinished)
		{
			// The ride hands back a fast player rather than a stopped one — the same choice the dash
			// makes on exit (ApplyDashExitSpeed). Nothing is zeroed here.
			Riders.RemoveAt(Index);
			continue;
		}

		// Ground movement would discard the vertical component of an upward path on the very first
		// frame, so a rider is put into the falling mode the ride actually needs. Mode first, then
		// velocity: a mode change can rewrite Velocity, and the write must be the last word.
		if (Move->IsMovingOnGround())
		{
			Move->SetMovementMode(MOVE_Falling);
		}
		Move->Velocity = Direction * RideSpeed;
	}

	// ---- pick up anybody standing in the entrance ------------------------------------------------
	const FVector Start = FVector(RippleStart);
	const float EntryRadiusSq = EntryRadius * EntryRadius;

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive() || !ShouldSimulate(Candidate))
		{
			continue;
		}

		// ONE RIDE PER PLAYER PER RIPPLE. [ASSUMPTION] — §6 does not say. Without it a path whose
		// end lands back inside its own entry radius (a short vertical ripple on a ledge) would loop
		// a player forever, and "lasts 4 s" would become "is stuck for 4 s".
		bool bAlreadyRode = false;
		for (const TWeakObjectPtr<ATraceCharacter>& Past : LifetimeRiders)
		{
			if (Past.Get() == Candidate)
			{
				bAlreadyRode = true;
				break;
			}
		}
		if (bAlreadyRode)
		{
			continue;
		}

		if (FVector::DistSquared(Candidate->GetActorLocation(), Start) > EntryRadiusSq)
		{
			continue;
		}

		// *** THE CHOKE POINT. Beneficial — which is the effect class spec §4 defines precisely so
		// that §6's "any character, either team... the Core carrier can use it" is expressible
		// WITHOUT an exception to the carrier rule. There is no team test here on purpose. ***
		if (!UTraceAbilityComponent::CanAffect(SourcePlayerState, Candidate, ETraceAbilityEffect::Beneficial))
		{
			continue;
		}

		UTraceCharacterMovementComponent* Move = Candidate->GetTraceMovement();
		if (Move == nullptr)
		{
			continue;
		}

		FRippleRider NewRider;
		NewRider.Pawn = Candidate;
		NewRider.DistanceTravelled = 0.f;
		Riders.Add(NewRider);
		LifetimeRiders.Add(Candidate);

		if (Move->IsMovingOnGround())
		{
			Move->SetMovementMode(MOVE_Falling);
		}
		Move->Velocity = Direction * RideSpeed;

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Ripple] %s entered (authority=%d, carrier=%d) — propelled at %.0f uu/s along (%s)."),
			*GetNameSafe(Candidate), HasAuthority() ? 1 : 0,
			UTraceAbilityComponent::IsCarrier(Candidate) ? 1 : 0, RideSpeed, *Direction.ToCompactString());
	}
}

const TCHAR* ATraceRippleActor::DescribeEntryRefusal(const ATraceCharacter* Candidate, const APlayerState* Source)
{
	if (const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Source))
	{
		return TraceAbilityBlockReasonToString(
			Comp->CanAffectTargetDetailed(Candidate, ETraceAbilityEffect::Beneficial));
	}
	return TEXT("<no ability component on the source>");
}

// =================================================================================================
// The rings — §6's "short series of rings along the path, with the starting ring in a different
// colour so it is obvious where to take it"
// =================================================================================================

void ATraceRippleActor::BuildRingsIfNeeded()
{
	if (bRingsBuilt || PathLength <= 0.f)
	{
		return;
	}

	// Shaders are not cooked for server targets, so a dedicated server builds nothing at all.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bRingsBuilt = true;
		return;
	}

	UStaticMesh* const Cylinder = UTraceFxShapes::GetCylinder();
	if (Cylinder == nullptr)
	{
		return;   // nothing to build from yet; Tick asks again, exactly as it did for the path
	}

	bRingsBuilt = true;

	const UTraceSettings& Settings = UTraceSettings::Get();

	auto SetupRing = [this, Cylinder](UInstancedStaticMeshComponent* Mesh,
	                                  TObjectPtr<UMaterialInstanceDynamic>& OutMID,
	                                  ETraceFxBlend& OutBlend,
	                                  const FLinearColor& Color)
	{
		if (Mesh == nullptr)
		{
			return;
		}
		Mesh->SetStaticMesh(Cylinder);

		// EMISSIVE, because a ring bead is THIN. ATraceTracer's rule and this file inherits it: big
		// volumes are additive so they cannot hide what is behind them, thin pieces are emissive so
		// they can push their hue past a lit background — which is the whole job of a wayfinding
		// ring at 3.5 on the bible's ladder.
		OutMID = UTraceFxShapes::MakeGlowMID(Mesh, 0, ETraceFxBlend::Emissive, OutBlend);

		// NO GREY, EVER. On None there is no material at all, and an untextured engine cylinder ring
		// laid across the arena would be far worse than no ripple visuals.
		Mesh->SetVisibility(OutBlend != ETraceFxBlend::None);
		SetRingGlow(OutMID, OutBlend, Color, TraceRippleTuning::RingGlow);
	};

	SetupRing(StartRingMesh, StartRingMID, StartRingBlend, Settings.RoccoRippleStartRingColor);
	SetupRing(TrailRingMesh, TrailRingMID, TrailRingBlend, Settings.RoccoRippleTrailRingColor);

	// =============================================================================================
	// *** FX_AUDIO_PLAN §2.9's START-RING PULSE. The one gameplay pulse the bible permits. ***
	//
	// PREFERRED PATH: M_TraceNeon's own PulseAmp/PulseRate scalars, added by the material pass
	// (Scripts/generate_content.py — Emissive *= 1 + PulseAmp * sin(2*PI * Time * PulseRate)). They
	// are uniform expressions folded on the CPU, so the pulse costs nothing per frame and, more
	// importantly, it is FRAME-RATE AND TICK INDEPENDENT: every machine's start ring breathes in the
	// same phase because they all read the same Time.
	//
	// FALLBACK: §2.9 says in as many words "if the material pass hasn't landed yet, tick the Glow
	// scalar ±15% @ 0.8 Hz from the actor", and UpdateRides does that when this test fails. The test
	// is a READ of the parameter rather than a version check — GetScalarParameterValue answers false
	// on a parent that does not declare it, which is the only honest way to ask "does this material
	// have a pulse" and works on the Fallback rung too (BasicShapeMaterial has neither).
	// =============================================================================================
	if (StartRingMID != nullptr)
	{
		float Existing = 0.f;
		bPulseInMaterial = StartRingMID->GetScalarParameterValue(
			FMaterialParameterInfo(TEXT("PulseRate")), Existing);

		if (bPulseInMaterial)
		{
			StartRingMID->SetScalarParameterValue(TEXT("PulseRate"), TraceRippleTuning::StartPulseRateHz);
			StartRingMID->SetScalarParameterValue(TEXT("PulseAmp"), TraceRippleTuning::StartPulseAmp);
		}
	}

	// --- §2.9's ride FX pool ----------------------------------------------------------------------
	//
	// EMISSIVE, not additive — see SpeedLineGlow for the renderer warning that settled it. Kept dim
	// instead: a wake that out-shouted the path it is drawn along would be the wrong way round.
	if (RideFxMesh != nullptr)
	{
		RideFxMesh->SetStaticMesh(Cylinder);
		RideFxMID = UTraceFxShapes::MakeGlowMID(RideFxMesh, 0, ETraceFxBlend::Emissive, RideFxBlend);

		const bool bUsable = (RideFxBlend != ETraceFxBlend::None);
		RideFxMesh->SetVisibility(bUsable);

		if (bUsable)
		{
			// THE RIDE FX WEAR ROCCO'S AMBER WHOEVER IS RIDING. §2.9 is explicit and bible §6.2 is the
			// reason: the effect belongs to the ABILITY, and the ability is Rocco's. A ride that wore
			// the rider's own accent would make the ripple look like ten different abilities.
			SetRingGlow(RideFxMID, RideFxBlend, Settings.RoccoRippleStartRingColor,
				TraceRippleTuning::SpeedLineGlow);

			// Added once at zero scale and transformed every frame afterwards; an unused slot is a
			// zero-scale instance rather than an add/remove that would rebuild the render state.
			const int32 Slots = TraceRippleTuning::MaxPresentedRiders * TraceRippleTuning::SpeedLinesPerRider;
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				RideFxMesh->AddInstance(FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector::ZeroVector));
			}
		}
	}

	const FVector Start = FVector(RippleStart);
	const FVector Direction = FVector(RippleDirection).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		return;
	}

	const float Spacing = FMath::Max(20.f, Settings.RoccoRippleRingSpacingUU);
	const int32 TrailRings = FMath::Clamp(FMath::FloorToInt(PathLength / Spacing), 1, TraceRippleTuning::MaxRings - 1);

	// THE START RING IS FIRST AND IS ITS OWN COMPONENT. §6 asks for it in a different colour "so it
	// is obvious where to take it" — it marks the ENTRANCE, which is the only place entry is
	// possible, so making it a separate draw rather than a tinted instance is the point.
	AddRing(StartRingMesh, Start, Direction);

	for (int32 Index = 1; Index <= TrailRings; ++Index)
	{
		const float Distance = FMath::Min(PathLength, Spacing * static_cast<float>(Index));
		AddRing(TrailRingMesh, Start + Direction * Distance, Direction);
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Ripple] rings built: 1 START ring (colour %s) + %d trail rings (colour %s), spacing %.0f uu, "
		     "radius %.0f uu, path %.0f uu. Blends %s. Start pulse %.1f Hz +-%.0f%% via %s. "
		     "Ride FX slots %d."),
		*Settings.RoccoRippleStartRingColor.ToString(), TrailRings,
		*Settings.RoccoRippleTrailRingColor.ToString(), Spacing, Settings.RoccoRippleRingRadiusUU, PathLength,
		*DescribeBlends(), TraceRippleTuning::StartPulseRateHz, TraceRippleTuning::StartPulseAmp * 100.f,
		bPulseInMaterial ? TEXT("M_TraceNeon PulseAmp/PulseRate") : TEXT("a per-frame Glow write (§2.9 fallback)"),
		(RideFxMesh != nullptr) ? RideFxMesh->GetInstanceCount() : 0);
}

// =================================================================================================
// FX_AUDIO_PLAN §2.9 — the dissolve, the pulse fallback, the ride FX and the ride loop
// =================================================================================================

void ATraceRippleActor::SetRingGlow(UMaterialInstanceDynamic* MID, ETraceFxBlend Blend,
	const FLinearColor& Color, float Intensity) const
{
	if (MID == nullptr || Blend == ETraceFxBlend::None)
	{
		return;
	}

	// UTraceFxShapes::SetGlow knows which of the two routes each blend takes — the scalar on
	// M_TraceNeon, the colour weight on the additive material — which is exactly why the fade is one
	// number here rather than three parameter writes that could disagree. FADE IN BRIGHTNESS, NEVER
	// IN ALPHA: the emissive parents are OPAQUE, so an alpha of zero is a fully visible ring.
	UTraceFxShapes::SetGlow(MID, Blend, Color, FMath::Max(0.f, Intensity));
}

float ATraceRippleActor::GetDissolveAlpha() const
{
	// COSMETIC ONLY. The ride, the entry test and the destroy all still run off ExpireMatchTime
	// untouched; this is the last 0.3 s of the SAME deadline expressed as a brightness. A ripple in
	// its dissolve is still a ripple you can step into and be carried by, which is the honest read:
	// the fade says "this is going", not "this is gone".
	if (ExpireMatchTime <= 0.f)
	{
		return 1.f;
	}

	const float Remaining = ExpireMatchTime - MatchTimeNow();
	return FMath::Clamp(Remaining / TraceRippleTuning::DissolveSeconds, 0.f, 1.f);
}

FString ATraceRippleActor::DescribeBlends() const
{
	return FString::Printf(TEXT("start=%s trail=%s ride=%s"),
		UTraceFxShapes::BlendName(StartRingBlend),
		UTraceFxShapes::BlendName(TrailRingBlend),
		UTraceFxShapes::BlendName(RideFxBlend));
}

int32 ATraceRippleActor::GetDrawnBeadCount() const
{
	// GetInstanceCount(), NOT the number we MEANT to add. ATraceElleGate::BuildRingsIfNeeded is this
	// project's standing example of an effect that built 60 ring segments and registered none of
	// them: every internal counter said 60 and the screen was empty.
	return ((StartRingMesh != nullptr) ? StartRingMesh->GetInstanceCount() : 0)
	     + ((TrailRingMesh != nullptr) ? TrailRingMesh->GetInstanceCount() : 0);
}

int32 ATraceRippleActor::GetPresentedRiderCount() const
{
	return PresentedRiders.Num();
}

bool ATraceRippleActor::LooksLikeRiding(const ATraceCharacter* Candidate) const
{
	// PURE, AND IT ASKS ONLY ABOUT MOTION THIS MACHINE ALREADY HAS. On the server and on the rider's
	// own client, UpdateRideFx has the authoritative Riders list and never needs this; on every OTHER
	// machine the pawn is a simulated proxy whose velocity arrives by ordinary movement replication,
	// and three questions about that velocity are enough to tell "being carried along this path"
	// from "running in roughly the same direction":
	//
	//   1. is he ON the path (inside a corridor around the axis, between its two ends)?
	//   2. is he moving ALONG it (within 25 degrees)?
	//   3. is he moving FAST — at least 60% of the ride speed?
	//
	// A player cannot satisfy all three by accident for long: the ride speed is a dash speed, and
	// sprinting is well under 60% of it. A false positive costs three amber bars for a frame.
	const UTraceCharacterMovementComponent* Move =
		(Candidate != nullptr) ? Candidate->GetTraceMovement() : nullptr;
	if (Move == nullptr || RideSpeed <= 0.f || PathLength <= 0.f)
	{
		return false;
	}

	const FVector Direction = FVector(RippleDirection).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		return false;
	}

	const FVector Start = FVector(RippleStart);
	const FVector Offset = Candidate->GetActorLocation() - Start;

	const float Along = static_cast<float>(FVector::DotProduct(Offset, Direction));
	if (Along < -TraceRippleTuning::RideCorridorUU || Along > PathLength + TraceRippleTuning::RideCorridorUU)
	{
		return false;
	}

	const float OffAxis = static_cast<float>((Offset - Direction * Along).Size());
	if (OffAxis > TraceRippleTuning::RideCorridorUU)
	{
		return false;
	}

	const FVector Velocity = Move->Velocity;
	const float Speed = static_cast<float>(Velocity.Size());
	if (Speed < RideSpeed * TraceRippleTuning::RideSpeedFraction)
	{
		return false;
	}

	return FVector::DotProduct(Velocity / Speed, Direction) >= TraceRippleTuning::RideAlignmentDot;
}

void ATraceRippleActor::StopAllRideLoops()
{
	for (FPresentedRider& Rider : PresentedRiders)
	{
		if (UAudioComponent* Loop = Rider.Loop.Get())
		{
			// FadeOut and not Stop: §1.6.4 hands the caller a component with bAutoDestroy OFF, and the
			// fade destroys it at the end. A hard stop on a wind loop is a click.
			Loop->FadeOut(0.25f, 0.f);
		}
	}
	PresentedRiders.Reset();
}

void ATraceRippleActor::UpdateRideFx()
{
	const float Dissolve = GetDissolveAlpha();

	// --- the two ring brightnesses ----------------------------------------------------------------
	//
	// The dissolve multiplies both rings. The PULSE multiplies only the start ring, and only when the
	// material could not carry it — when it can, M_TraceNeon is already doing this arithmetic on the
	// GPU with a clock every machine agrees about.
	float StartGlow = TraceRippleTuning::RingGlow * Dissolve;
	if (!bPulseInMaterial)
	{
		const float Now = MatchTimeNow();
		const float Phase = 2.f * PI * TraceRippleTuning::StartPulseRateHz * Now;
		StartGlow *= 1.f + TraceRippleTuning::StartPulseAmp * FMath::Sin(Phase);
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	SetRingGlow(StartRingMID, StartRingBlend, Settings.RoccoRippleStartRingColor, StartGlow);
	SetRingGlow(TrailRingMID, TrailRingBlend, Settings.RoccoRippleTrailRingColor,
		TraceRippleTuning::RingGlow * Dissolve);

	// *** AND THE BEADS SHRINK WITH IT. THIS IS NOT POLISH, IT IS THE DIFFERENCE BETWEEN A DISSOLVE
	// AND A BLACK RING. *** M_TraceNeon is UNLIT and OPAQUE — Emissive = Colour x Glow — so at Glow 0
	// the beads are not gone, they are BLACK, and a matte black ring lying across a lit blue floor is
	// MORE visible than the glowing one it replaced. Bible §6.4 asks for "never a pop-out"; dimming
	// alone would have delivered a pop-IN. Taking the bead cross-section to zero over the same curve
	// makes the last frame draw nothing.
	//
	// Gated on Dissolve < 1 so the ordinary case — the whole of a ripple's life bar the last 0.3 s —
	// costs one float compare instead of 240 instance transforms a frame.
	if (Dissolve < 1.f)
	{
		auto ShrinkBeads = [Dissolve](UInstancedStaticMeshComponent* Mesh)
		{
			if (Mesh == nullptr)
			{
				return;
			}
			const float Thickness = TraceRippleTuning::BeadThicknessUU * Dissolve / 100.f;
			const int32 Count = Mesh->GetInstanceCount();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FTransform BeadTransform;
				if (!Mesh->GetInstanceTransform(Index, BeadTransform, /*bWorldSpace*/ false))
				{
					continue;
				}
				// Only the CROSS-SECTION moves. The bead's length is its share of the ring's
				// circumference and shrinking that would open gaps between the beads instead of
				// thinning the ring.
				FVector Scale = BeadTransform.GetScale3D();
				Scale.X = Thickness;
				Scale.Y = Thickness;
				BeadTransform.SetScale3D(Scale);
				Mesh->UpdateInstanceTransform(Index, BeadTransform, /*bWorldSpace*/ false,
					/*bMarkRenderStateDirty*/ Index == Count - 1, /*bTeleport*/ true);
			}
		};

		ShrinkBeads(StartRingMesh);
		ShrinkBeads(TrailRingMesh);
	}

	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	// --- who is visibly riding, on THIS machine ---------------------------------------------------
	//
	// *** ONE KNOWN INTERACTION, STATED RATHER THAN QUIETLY LEFT. *** A CLOAKED Elle who rides a
	// ripple gets a wake like anybody else, so the wake reveals roughly where she is. That is a
	// deliberate call, not an oversight:
	//
	//   - the FX are ADDITIVE and live on this actor, not on her body, so they are outside the
	//     ApplyTeamColors() refresh path §1.2 obligation 4 is about — nothing here dims or restores
	//     her emissives, and nothing here can leave a component behind on her pawn;
	//   - the thing she is riding is a lane of amber rings that this ability drew across the arena
	//     and that everyone can already see. The wake sits ON that lane. A player who wanted to be
	//     unseen had the option of not stepping into a lit path;
	//   - and the alternative — asking every candidate's ability set whether it is a cloaked Elle —
	//     would couple this file to another kit's header for a cosmetic edge case.
	//
	// If the owner decides the leak matters, the fix is one test here against Elle's
	// IsCloakVisualApplied(); it is named in the W4-KITS-D report so the decision is somebody's
	// rather than nobody's.
	TArray<ATraceCharacter*, TInlineAllocator<8>> Riding;
	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !IsValid(Candidate) || !Candidate->IsAlive())
		{
			continue;
		}

		// IsRiding() is the exact answer on a machine that simulates this pawn; LooksLikeRiding() is
		// the derived one everywhere else. Asking both means the FX start on the same frame the ride
		// does for the rider himself, and within a movement update for everybody watching.
		if (IsRiding(Candidate) || LooksLikeRiding(Candidate))
		{
			Riding.Add(Candidate);
		}
	}

	// --- the ride loop: one start per rider per machine, one fade per exit -------------------------
	for (int32 Index = PresentedRiders.Num() - 1; Index >= 0; --Index)
	{
		ATraceCharacter* Pawn = PresentedRiders[Index].Pawn.Get();
		if (Pawn != nullptr && Riding.Contains(Pawn))
		{
			continue;
		}

		if (UAudioComponent* Loop = PresentedRiders[Index].Loop.Get())
		{
			Loop->FadeOut(0.25f, 0.f);
		}
		PresentedRiders.RemoveAt(Index);
	}

	for (ATraceCharacter* Pawn : Riding)
	{
		const bool bKnown = PresentedRiders.ContainsByPredicate(
			[Pawn](const FPresentedRider& Held) { return Held.Pawn.Get() == Pawn; });
		if (bKnown)
		{
			continue;
		}

		FPresentedRider NewRider;
		NewRider.Pawn = Pawn;

		// LOCAL, NO RPC, ON EVERY MACHINE — which is the whole point of deriving the ride rather than
		// replicating it. The event is declared Client-side in the sound table precisely so that a
		// stray TraceAudio::Play() on it could never multicast a second copy over the top of these.
		NewRider.Loop = TraceAudio::StartLoopOn(Pawn->GetRootComponent(), TraceSoundEvents::RoccoRideLoop);

		PresentedRiders.Add(NewRider);
	}

	PeakPresentedRiders = FMath::Max(PeakPresentedRiders, PresentedRiders.Num());

	// --- the speed lines --------------------------------------------------------------------------
	if (RideFxMesh == nullptr || RideFxMesh->GetInstanceCount() <= 0)
	{
		return;
	}

	const FVector Direction = FVector(RippleDirection).GetSafeNormal();
	const float LineScaleZ = UTraceFxShapes::ShapeScaleForLengthUU(TraceRippleTuning::SpeedLineLengthUU);

	// Same rule as the rings: the wake thins by SHRINKING, because an unlit opaque bar dimmed to zero
	// is a black bar. An unused slot is already a zero-scale instance, so this is the same mechanism
	// the pool uses to hide a slot, applied by degrees.
	const float LineScaleXY = UTraceFxShapes::ShapeScaleForRadiusUU(
		TraceRippleTuning::SpeedLineRadiusUU * Dissolve);
	const FRotator LineRotation = FRotationMatrix::MakeFromZ(Direction).Rotator();

	// The three bars sit behind the rider and beside each other: one on his axis and one out to each
	// side, so the set reads as a wake rather than as one thick tube. FindBestAxisVectors is the
	// engine's numerically safe perpendicular pick, which matters because a ripple can be vertical.
	FVector AxisU = FVector::ZeroVector;
	FVector AxisV = FVector::ZeroVector;
	Direction.FindBestAxisVectors(AxisU, AxisV);

	const int32 Slots = RideFxMesh->GetInstanceCount();
	const int32 Drawn = FMath::Min(Riding.Num(), TraceRippleTuning::MaxPresentedRiders);

	for (int32 Slot = 0; Slot < Slots; ++Slot)
	{
		const int32 RiderIndex = Slot / TraceRippleTuning::SpeedLinesPerRider;
		const int32 LineIndex = Slot % TraceRippleTuning::SpeedLinesPerRider;

		FTransform LineTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector::ZeroVector);

		if (RiderIndex < Drawn && Riding[RiderIndex] != nullptr)
		{
			// Behind the rider by half a bar plus a gap, so the near end of the wake starts at his
			// back rather than inside his chest.
			const FVector Lateral = (LineIndex == 0) ? FVector::ZeroVector
				: ((LineIndex == 1) ? AxisU * 34.f : AxisU * -34.f);
			const FVector Centre = Riding[RiderIndex]->GetActorLocation()
				+ Lateral
				- Direction * (TraceRippleTuning::SpeedLineLengthUU * 0.5f + 30.f);

			LineTransform = FTransform(LineRotation, Centre, FVector(LineScaleXY, LineScaleXY, LineScaleZ));
		}

		RideFxMesh->UpdateInstanceTransform(Slot, LineTransform, /*bWorldSpace*/ true,
			/*bMarkRenderStateDirty*/ Slot == Slots - 1, /*bTeleport*/ true);
	}

	// The wake dissolves with the rings: a ripple that is fading out should not still be throwing
	// full-brightness speed lines off a rider in its last frames.
	SetRingGlow(RideFxMID, RideFxBlend, Settings.RoccoRippleStartRingColor,
		TraceRippleTuning::SpeedLineGlow * Dissolve);
}

void ATraceRippleActor::AddRing(UInstancedStaticMeshComponent* Mesh, const FVector& Center, const FVector& Direction) const
{
	if (Mesh == nullptr)
	{
		return;
	}

	const float RingRadius = FMath::Max(10.f, UTraceSettings::Get().RoccoRippleRingRadiusUU);

	// A basis for the plane the ring lies in. FindBestAxisVectors is the engine's own numerically
	// safe pick, which matters because the path may be exactly vertical.
	FVector AxisU = FVector::ZeroVector;
	FVector AxisV = FVector::ZeroVector;
	Direction.FindBestAxisVectors(AxisU, AxisV);

	// Each bead is a cylinder lying along the ring's tangent, so a ring of them reads as a torus
	// rather than a necklace. 1.2 overlaps them slightly and hides the seams.
	const float ArcLength = (2.f * PI * RingRadius / static_cast<float>(TraceRippleTuning::BeadsPerRing)) * 1.2f;
	const FVector BeadScale(TraceRippleTuning::BeadThicknessUU / 100.f,
	                        TraceRippleTuning::BeadThicknessUU / 100.f,
	                        ArcLength / 100.f);

	const FVector CenterRelative = Center - GetActorLocation();

	for (int32 Bead = 0; Bead < TraceRippleTuning::BeadsPerRing; ++Bead)
	{
		const float Angle = (2.f * PI * static_cast<float>(Bead)) / static_cast<float>(TraceRippleTuning::BeadsPerRing);
		const float SinA = FMath::Sin(Angle);
		const float CosA = FMath::Cos(Angle);

		const FVector Offset  = (AxisU * CosA + AxisV * SinA) * RingRadius;
		const FVector Tangent = (AxisU * -SinA + AxisV * CosA);

		const FTransform BeadTransform(FRotationMatrix::MakeFromZ(Tangent).Rotator(),
		                               CenterRelative + Offset, BeadScale);
		Mesh->AddInstance(BeadTransform);
	}
}
