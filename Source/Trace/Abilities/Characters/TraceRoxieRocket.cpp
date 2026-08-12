// Trace — Roxie's rocket. Read TraceRoxieRocket.h first; it carries the design and the two rules
// this file is not allowed to break (no damage call of its own, no collision of its own).

#include "Abilities/Characters/TraceRoxieRocket.h"

#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                    // TActorIterator
#include "GameFramework/GameStateBase.h"    // the match clock the path is drawn on
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/Characters/TraceAbilitySetRoxie.h"
#include "Core/TraceCharacter.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE RED ARM FOR "DELIBERATELY INACCURATE AND HARD TO AIM"
//
// A rocket with no wobble is a straight, easily-aimed, 100-damage projectile — a materially different
// and much stronger ability than the one spec v18 §2 describes. Trace.Roxie.RocketFlightTest A/Bs
// against this, so "it wobbles" is a measured lateral deviation in uu rather than an adjective, and so
// the harness has a build it can be shown FAILING on.
//
// 1 (shipped): the rocket wobbles.
// 0:           it flies dead straight. NEVER SHIP 0.
// =================================================================================================
static TAutoConsoleVariable<int32> CVarRoxieRocketWobble(
	TEXT("Trace.Roxie.RocketWobble"), 1,
	TEXT("TEST ARM ONLY. 1 (shipped): Roxie's rocket wobbles in flight — spec v18 §2. 0: it flies "
	     "straight, so Trace.Roxie.RocketFlightTest can be shown FAILING. Never ship 0."),
	ECVF_Cheat);

/**
 * File-private constants. Named after the file rather than left anonymous, because an unnamed
 * namespace collides with every other unnamed namespace concatenated into the same unity translation
 * unit — a Windows-only failure macOS structurally cannot see.
 */
namespace TraceRoxieRocketFile
{
	/**
	 * How long the drawn rocket is, as a multiple of its radius. 3 is a rocket rather than a ball or a
	 * needle; it is a look, not a rule, which is why it is a file constant and not a twelfth knob.
	 */
	constexpr float VisualLengthPerRadius = 3.f;
}

namespace TraceRoxieRocket
{
	float GetDamage()
	{
		// §2's flat 100. Clamped like every other knob read on this project: the .ini layers over the
		// header default, so nothing may dereference the property and hope.
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketDamage, 0.f, 500.f);
	}

	float GetSpeedUU()
	{
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketSpeed, 200.f, 20000.f);
	}

	float GetLifetimeSeconds()
	{
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketLifetimeSeconds, 0.1f, 30.f);
	}

	float GetHitRadiusUU()
	{
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketHitRadiusUU, 1.f, 300.f);
	}

	float GetVisualScale()
	{
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketVisualScale, 0.1f, 6.f);
	}

	float GetWobbleAmplitudeUU()
	{
		// THE RED ARM LIVES HERE rather than at the call sites, so that every one of the three path
		// callers (the server sweep, the visual, the harness) is disarmed by one cvar and cannot
		// disagree about whether this rocket wobbles.
		if (CVarRoxieRocketWobble.GetValueOnAnyThread() == 0)
		{
			return 0.f;
		}
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketWobbleAmplitudeUU, 0.f, 1000.f);
	}

	float GetWobbleFrequencyHz()
	{
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketWobbleFrequencyHz, 0.f, 20.f);
	}

	float GetSelfLaunchImpulse()
	{
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketSelfLaunchImpulse, 0.f, 8000.f);
	}

	float GetSelfLaunchUpBias()
	{
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketSelfLaunchUpBias, 0.f, 1.5f);
	}

	float GetCooldownSeconds()
	{
		return FMath::Clamp(UTraceSettings::Get().RoxieRocketCooldownSeconds, 0.f, 180.f);
	}

	FName GetKillCause()
	{
		// Constructed once, like TraceMelee's causes. The string is the contract between Roxie's
		// DealDamage() and the kill feed's glyph mapping; see the header for why it is not a literal
		// in either of them.
		static const FName Cause(TEXT("RoxieRocket"));
		return Cause;
	}

	void BuildWobbleBasis(const FVector& Direction, FVector& OutRightAxis, FVector& OutUpAxis)
	{
		const FVector Forward = Direction.GetSafeNormal();
		if (Forward.IsNearlyZero())
		{
			OutRightAxis = FVector::RightVector;
			OutUpAxis = FVector::UpVector;
			return;
		}

		// World up first, so a level shot wobbles left/right and up/down the way a player expects.
		// Straight up or straight down degenerates that cross product, and the fallback picks a second
		// reference axis rather than returning a zero vector — a zero basis would silently delete the
		// wobble for exactly the shot a rocket-jumping Roxie takes most often.
		OutRightAxis = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();
		if (OutRightAxis.IsNearlyZero())
		{
			OutRightAxis = FVector::CrossProduct(FVector::ForwardVector, Forward).GetSafeNormal();
		}
		OutUpAxis = FVector::CrossProduct(Forward, OutRightAxis).GetSafeNormal();
	}

	FVector GetWobbleOffsetAtTime(const FVector& Direction, float SecondsSinceLaunch,
	                              float WobbleAmplitudeUU, float WobbleFrequencyHz, float WobbleSeedTurns)
	{
		if (WobbleAmplitudeUU <= KINDA_SMALL_NUMBER || SecondsSinceLaunch <= 0.f)
		{
			// Zero at t = 0 by construction as well as by this early-out: the rocket has to leave the
			// muzzle ON the crosshair, or the gun reads as misaligned rather than the rocket as wild.
			return FVector::ZeroVector;
		}

		FVector RightAxis = FVector::ZeroVector;
		FVector UpAxis = FVector::ZeroVector;
		BuildWobbleBasis(Direction, RightAxis, UpAxis);

		// The SEED rolls the plane the wobble swings in. Without it every rocket would trace the
		// identical sine and a player would learn to lead it, which would leave "hard to aim" true only
		// for the first week. It is rolled per shot on the server and replicated, so the arc is exactly
		// reproducible on every machine and unpredictable to the shooter.
		const float SeedRadians = WobbleSeedTurns * 2.f * PI;
		const FVector AxisA = RightAxis * FMath::Cos(SeedRadians) + UpAxis * FMath::Sin(SeedRadians);
		const FVector AxisB = RightAxis * -FMath::Sin(SeedRadians) + UpAxis * FMath::Cos(SeedRadians);

		// TWO INCOMMENSURATE FREQUENCIES, NOT ONE. A single sine crosses the aim line once per period,
		// so a player only has to learn the period to fire "on the beat" and hit dead straight. 0.61x
		// makes the path a Lissajous wander with no repeat inside any lifetime this knob can hold.
		const float Phase = 2.f * PI * WobbleFrequencyHz * SecondsSinceLaunch;

		return AxisA * (WobbleAmplitudeUU * FMath::Sin(Phase))
		     + AxisB * (WobbleAmplitudeUU * 0.55f * FMath::Sin(Phase * 0.61f));
	}

	FVector GetPositionAtTime(const FVector& Origin, const FVector& Direction, float SecondsSinceLaunch,
	                          float SpeedUU, float WobbleAmplitudeUU, float WobbleFrequencyHz,
	                          float WobbleSeedTurns)
	{
		const float ClampedTime = FMath::Max(0.f, SecondsSinceLaunch);
		const FVector Forward = Direction.GetSafeNormal();

		return Origin
		     + Forward * (SpeedUU * ClampedTime)
		     + GetWobbleOffsetAtTime(Direction, ClampedTime, WobbleAmplitudeUU, WobbleFrequencyHz, WobbleSeedTurns);
	}
}

// =================================================================================================
// ATraceRoxieRocket
// =================================================================================================

ATraceRoxieRocket::ATraceRoxieRocket()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;

	// NO MOVEMENT REPLICATION. Every machine derives the position from four values replicated once and
	// the match clock (see the header), so per-frame transform updates would be bandwidth spent to
	// arrive at the number the receiver already has — and would arrive INTERPOLATED, which is exactly
	// how a drawn path stops being the lethal path.
	SetReplicateMovement(false);

	// A rocket crosses the arena. Relevance culling on a 3 s projectile would make it pop into
	// existence halfway down its own flight on any client that was not already looking at Roxie.
	bAlwaysRelevant = true;

	USceneComponent* RocketRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(RocketRoot);
	RocketRoot->SetMobility(EComponentMobility::Movable);

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(RocketRoot);

	// NO COLLISION, on purpose and stated three times in this feature. See the header.
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetCollisionProfileName(TEXT("NoCollision"));
	Body->SetGenerateOverlapEvents(false);
	Body->SetCanEverAffectNavigation(false);
	Body->SetCastShadow(false);
	Body->bReceivesDecals = false;

	// The SIZE is applied in BeginPlay, not here: it is derived from the rocket's own hit radius, which
	// is a live settings knob, and a CDO built before the .ini layers over the header would bake the
	// wrong one in for the whole process. See ApplyVisualSize().

	// ConstructorHelpers rather than a runtime LoadObject, for the reason ATraceCore's constructor
	// gives: a constructor-time reference is what makes an engine asset cook into a packaged build,
	// where a runtime load of the same path resolves to null.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (ConeFinder.Succeeded())
	{
		Body->SetStaticMesh(ConeFinder.Object);
	}
}

void ATraceRoxieRocket::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceRoxieRocket, LaunchOrigin);
	DOREPLIFETIME(ATraceRoxieRocket, LaunchDirection);
	DOREPLIFETIME(ATraceRoxieRocket, LaunchMatchTime);
	DOREPLIFETIME(ATraceRoxieRocket, WobbleSeedTurns);
}

void ATraceRoxieRocket::BeginPlay()
{
	Super::BeginPlay();

	if (const UWorld* RocketWorld = GetWorld())
	{
		SpawnWorldTime = RocketWorld->GetTimeSeconds();
	}

	ApplyVisualSize();

	// Ember, matching the select card's stripe, so a player who has seen the card recognises the
	// thing coming at them. Built here rather than in the constructor because a dynamic material
	// instance is per-instance state; the base material is resolved by path and may legitimately be
	// missing in a fixture, in which case the rocket is simply grey.
	if (Body != nullptr)
	{
		UMaterialInterface* BaseMaterial = Body->GetMaterial(0);
		if (BaseMaterial != nullptr)
		{
			if (UMaterialInstanceDynamic* RocketMID = Body->CreateDynamicMaterialInstance(0, BaseMaterial))
			{
				const FLinearColor Ember(1.f, 0.45f, 0.12f, 1.f);
				RocketMID->SetVectorParameterValue(TEXT("Color"), Ember);
				RocketMID->SetVectorParameterValue(TEXT("BaseColor"), Ember);

				// DEMO 17 item 3 asks for a model that is EASY TO SEE, and on this arena half of that is
				// brightness rather than size: the field is black and everything a player reads at range
				// is emissive. These four are the names the project's material families use for the same
				// two ideas, and setting one a material does not have is a documented silent no-op (see
				// ApplyColorToSkeletalMesh) — so the rocket glows on M_TraceNeon and is merely ember on
				// the BasicShapes fallback, with no branch here.
				RocketMID->SetVectorParameterValue(TEXT("EmissiveColor"), Ember * 4.f);
				RocketMID->SetScalarParameterValue(TEXT("Glow"), 4.f);
				RocketMID->SetScalarParameterValue(TEXT("EmissiveStrength"), 4.f);
				RocketMID->SetScalarParameterValue(TEXT("EmissivePower"), 4.f);
			}
		}
	}

	UpdateVisual(GetCurrentPosition());
}

void ATraceRoxieRocket::InitialiseFlight(UTraceAbilitySetRoxie* InOwnerSet, const FVector& InOrigin,
                                         const FVector& InDirection, float InLaunchMatchTime,
                                         float InWobbleSeedTurns)
{
	OwnerSet = InOwnerSet;
	LaunchOrigin = InOrigin;
	LaunchDirection = InDirection.GetSafeNormal();
	LaunchMatchTime = InLaunchMatchTime;

	// Frac rather than Clamp: the seed is an ANGLE in turns, so 1.4 and 0.4 are the same wobble plane
	// and clamping would pile every out-of-range roll onto one arc.
	WobbleSeedTurns = FMath::Frac(FMath::Abs(InWobbleSeedTurns));

	if (InOwnerSet != nullptr)
	{
		ShooterPawn = InOwnerSet->GetCharacter();
	}

	LastSweptPosition = LaunchOrigin;
	bHasSwept = true;

	SetActorLocation(LaunchOrigin);
}

float ATraceRoxieRocket::GetSecondsInFlight() const
{
	// THE MATCH CLOCK, not the local world clock — it is the one clock the server and every client
	// agree on, which is what makes the drawn path and the lethal path the same path.
	float Now = LaunchMatchTime;
	if (const UWorld* RocketWorld = GetWorld())
	{
		if (const AGameStateBase* StateBase = RocketWorld->GetGameState())
		{
			Now = static_cast<float>(StateBase->GetServerWorldTimeSeconds());
		}
		else
		{
			Now = RocketWorld->GetTimeSeconds();
		}
	}
	return Now - LaunchMatchTime;
}

FVector ATraceRoxieRocket::GetCurrentPosition() const
{
	return TraceRoxieRocket::GetPositionAtTime(LaunchOrigin, LaunchDirection, GetSecondsInFlight(),
		TraceRoxieRocket::GetSpeedUU(), TraceRoxieRocket::GetWobbleAmplitudeUU(),
		TraceRoxieRocket::GetWobbleFrequencyHz(), WobbleSeedTurns);
}

void ATraceRoxieRocket::ApplyVisualSize()
{
	if (Body == nullptr)
	{
		return;
	}

	// *** DEMO 17 item 3: "the model gets BIGGER so it is easy to see." ***
	//
	// The size is DERIVED FROM THE HIT RADIUS rather than being a second free number, and that is the
	// whole point of this function. The rocket kills anything whose capsule comes within
	// RoxieRocketHitRadiusUU of the path; drawing a 13 uu dart around a 45 uu lethal radius meant the
	// thing a player was dodging was three and a half times wider than the thing they could see — which
	// is the "the lethal volume is not the drawn volume" defect this project already spent a pass
	// removing from the ribbon. Now the body is exactly as wide as its own touch radius, so a near miss
	// LOOKS like a near miss.
	//
	// RoxieRocketVisualScale is the tuning dial on top of that, and it defaults to 1.0 = "draw the
	// lethal size". /Engine/BasicShapes/Cone is 100 uu across and 100 uu tall, so a scale of 1 is 100 uu.
	const float Radius = TraceRoxieRocket::GetHitRadiusUU() * TraceRoxieRocket::GetVisualScale();
	const float Length = Radius * TraceRoxieRocketFile::VisualLengthPerRadius;

	Body->SetRelativeScale3D(FVector((Radius * 2.f) / 100.f, (Radius * 2.f) / 100.f, Length / 100.f));
}

void ATraceRoxieRocket::UpdateVisual(const FVector& AtPosition)
{
	SetActorLocation(AtPosition);

	if (Body == nullptr)
	{
		return;
	}

	// Point it along the INSTANTANEOUS direction of travel rather than along the launch direction, so
	// a wobbling rocket visibly banks through its own arc. Sampled a hair ahead on the same path
	// function, which costs one extra evaluation and needs no derivative.
	const float Ahead = GetSecondsInFlight() + 0.02f;
	const FVector NextPosition = TraceRoxieRocket::GetPositionAtTime(LaunchOrigin, LaunchDirection, Ahead,
		TraceRoxieRocket::GetSpeedUU(), TraceRoxieRocket::GetWobbleAmplitudeUU(),
		TraceRoxieRocket::GetWobbleFrequencyHz(), WobbleSeedTurns);

	const FVector Travel = NextPosition - AtPosition;
	if (!Travel.IsNearlyZero())
	{
		// The engine cone is Z-aligned about its centre; the +90 pitch is the same mapping
		// ATraceMaceSpike's rope uses to lay a Z-up primitive along an arbitrary vector.
		Body->SetWorldRotation(Travel.Rotation() + FRotator(90.f, 0.f, 0.f));
	}
}

void ATraceRoxieRocket::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const float SecondsInFlight = GetSecondsInFlight();
	const float Lifetime = TraceRoxieRocket::GetLifetimeSeconds();

	// Clamped for the VISUAL so a client whose destroy packet is a frame late does not draw the rocket
	// sailing past its own expiry; the server's expiry below is the real one.
	const FVector Position = TraceRoxieRocket::GetPositionAtTime(LaunchOrigin, LaunchDirection,
		FMath::Min(SecondsInFlight, Lifetime), TraceRoxieRocket::GetSpeedUU(),
		TraceRoxieRocket::GetWobbleAmplitudeUU(), TraceRoxieRocket::GetWobbleFrequencyHz(), WobbleSeedTurns);

	UpdateVisual(Position);

	if (!HasAuthority())
	{
		return;
	}

	// THE BACKSTOP, on local world time, and it is not a duplicate of the lifetime below. It catches a
	// rocket whose match clock never became usable (a GameState that had not replicated when it
	// spawned, a fixture with no game state at all) — in which case GetSecondsInFlight() can sit near
	// zero forever and the real expiry would never fire. Same belt ATraceMaceSpike carries.
	if (const UWorld* RocketWorld = GetWorld())
	{
		if ((RocketWorld->GetTimeSeconds() - SpawnWorldTime) > (Lifetime + 10.f))
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("[Roxie] rocket hit its backstop lifetime and destroyed itself."));
			Destroy();
			return;
		}
	}

	if (bHasSwept)
	{
		TickFlightAuthority(LastSweptPosition, Position);
		if (!IsValid(this))
		{
			return;   // TickFlightAuthority detonated us
		}
	}
	LastSweptPosition = Position;
	bHasSwept = true;

	if (SecondsInFlight >= Lifetime)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Roxie] rocket expired after %.2fs without hitting anything (range %.0f uu)."),
			SecondsInFlight, TraceRoxieRocket::GetSpeedUU() * Lifetime);
		Destroy();
	}
}

void ATraceRoxieRocket::TickFlightAuthority(const FVector& FromPosition, const FVector& ToPosition)
{
	UWorld* RocketWorld = GetWorld();
	if (RocketWorld == nullptr || FromPosition.Equals(ToPosition, 0.01f))
	{
		return;
	}

	UTraceAbilitySetRoxie* Roxie = OwnerSet.Get();
	if (Roxie == nullptr)
	{
		// Roxie changed character, left, or her ability set was torn down mid-flight. A rocket with no
		// owner has no instigator, and an ability effect with no instigator is precisely the "orphaned
		// world effect" spec §4 warns about becoming a path that damages a carrier. Fizzle instead.
		UE_LOG(LogTraceGame, Verbose, TEXT("[Roxie] rocket lost its owner mid-flight and fizzled."));
		Destroy();
		return;
	}

	const float HitRadius = TraceRoxieRocket::GetHitRadiusUU();
	ATraceCharacter* ShooterActor = ShooterPawn.Get();

	// ---- 1. THE WORLD, first, to bound the segment -------------------------------------------------
	//
	// ECC_Visibility and a sphere the size of the rocket. Character capsules use the Pawn profile,
	// which IGNORES ECC_Visibility (see Net/TraceLagCompensationComponent.cpp, which relies on the same
	// fact), so this finds geometry only and cannot be body-blocked — bodies are resolved analytically
	// below, against live poses.
	float WallDistance = TNumericLimits<float>::Max();
	FVector WallImpact = ToPosition;

	FCollisionQueryParams SweepParams(SCENE_QUERY_STAT(RoxieRocketFlight), /*bTraceComplex*/ false);
	if (ShooterActor != nullptr)
	{
		SweepParams.AddIgnoredActor(ShooterActor);
	}
	SweepParams.AddIgnoredActor(this);

	FHitResult WorldHit;
	if (RocketWorld->SweepSingleByChannel(WorldHit, FromPosition, ToPosition, FQuat::Identity,
			ECC_Visibility, FCollisionShape::MakeSphere(HitRadius), SweepParams))
	{
		// bStartPenetrating means the sweep began inside geometry, which happens when Roxie fires with
		// her muzzle in a wall. Treat it as an immediate impact at the start rather than as a miss:
		// the alternative is a rocket that silently passes through the wall it was born in.
		WallDistance = WorldHit.bStartPenetrating ? 0.f : WorldHit.Distance;
		WallImpact = WorldHit.bStartPenetrating ? FromPosition : WorldHit.ImpactPoint;
	}

	// ---- 2. BODIES, analytically, nearest first ----------------------------------------------------
	//
	// The victim's capsule as a segment, the rocket's path as a segment, and the touch test is the
	// distance between them against (rocket radius + capsule radius) — the same narrow phase X's bees
	// use, which is what keeps "what counts as a hit" one idea in this codebase rather than two.
	struct FRocketBodyHit
	{
		ATraceCharacter* Victim = nullptr;
		float DistanceAlong = 0.f;
		FVector ImpactPoint = FVector::ZeroVector;
	};
	TArray<FRocketBodyHit, TInlineAllocator<16>> BodyHits;

	for (TActorIterator<ATraceCharacter> It(RocketWorld); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || Candidate == ShooterActor || !Candidate->IsAlive())
		{
			continue;
		}

		float CapsuleRadius = 34.f;
		float CapsuleHalfHeight = 88.f;
		if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
		{
			CapsuleRadius = Capsule->GetScaledCapsuleRadius();
			CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		}

		const FVector BodyCentre = Candidate->GetActorLocation();
		const float HalfSegment = FMath::Max(0.f, CapsuleHalfHeight - CapsuleRadius);
		const FVector BodyTop = BodyCentre + FVector(0.f, 0.f, HalfSegment);
		const FVector BodyBottom = BodyCentre - FVector(0.f, 0.f, HalfSegment);

		FVector ClosestOnPath = FVector::ZeroVector;
		FVector ClosestOnBody = FVector::ZeroVector;
		FMath::SegmentDistToSegmentSafe(FromPosition, ToPosition, BodyBottom, BodyTop,
			ClosestOnPath, ClosestOnBody);

		const float Touch = HitRadius + CapsuleRadius;
		if (FVector::DistSquared(ClosestOnPath, ClosestOnBody) > (Touch * Touch))
		{
			continue;
		}

		FRocketBodyHit Entry;
		Entry.Victim = Candidate;
		Entry.DistanceAlong = static_cast<float>((ClosestOnPath - FromPosition).Size());
		Entry.ImpactPoint = ClosestOnPath;
		BodyHits.Add(Entry);
	}

	BodyHits.Sort([](const FRocketBodyHit& A, const FRocketBodyHit& B)
	{
		return A.DistanceAlong < B.DistanceAlong;
	});

	// ---- 3. RESOLVE, nearest first, and PASS THROUGH anybody the choke point refuses ---------------
	//
	// *** THE PASS-THROUGH IS A DELIBERATE DESIGN CALL AND IT IS WORTH READING TWICE. ***
	//
	// A Core carrier takes no ability damage — that is the founding invariant and it is not negotiable.
	// The question this loop answers is the SECOND one: should a carrier the rocket cannot hurt still
	// STOP the rocket? No. Detonating for zero would hand the carrier a brand-new defensive ability the
	// doc never granted — body-blocking rockets aimed at the team-mate behind them — and would do the
	// same for team-mates, which is the classic friendly-fire-off frustration (your own rocket eaten by
	// an ally who took no damage from it). So a refused target is flown THROUGH and the next candidate
	// is asked.
	//
	// It costs nothing in safety: the refusal is still the choke point's, made by
	// UTraceAbilitySetRoxie::ApplyRocketDamageTo -> UTraceCharacterAbilitySet::DealDamage ->
	// UTraceAbilityComponent::CanAffectTarget, exactly once per candidate, and the zero it returns is
	// what this loop reads.
	for (const FRocketBodyHit& Entry : BodyHits)
	{
		if (Entry.DistanceAlong > WallDistance)
		{
			break;   // the wall is in front of this body; the rocket never reaches them
		}

		const float Dealt = Roxie->ApplyRocketDamageTo(Entry.Victim);
		if (Dealt > 0.f)
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[Roxie] rocket hit %s for %.0f (flat, no hit zone) after %.2fs of flight."),
				*GetNameSafe(Entry.Victim), Dealt, GetSecondsInFlight());
			Destroy();
			return;
		}
	}

	if (WallDistance < TNumericLimits<float>::Max())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Roxie] rocket struck geometry at (%s) after %.2fs."),
			*WallImpact.ToCompactString(), GetSecondsInFlight());
		Destroy();
	}
}
