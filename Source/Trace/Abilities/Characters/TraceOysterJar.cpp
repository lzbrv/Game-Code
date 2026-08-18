// Trace — Oyster's jar. See the header: Pickler is a normal jar with a one-shot landing effect.

#include "Abilities/Characters/TraceOysterJar.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                 // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Core/TraceCharacter.h"
#include "HAL/IConsoleManager.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// SPEC v19 §4.4 — THE TEST ARM FOR "MORE CONSISTENT TO THROW"
//
// 0 (shipped): the release point is resolved clear of geometry and the arc is integrated in
//              distance-capped sub-steps with the exact constant-acceleration solution, so the same
//              throw lands in the same place whatever the frame rate is doing.
// 1:           restores the pre-v19 throw EXACTLY — released at the raw muzzle, one swept Euler step
//              per rendered frame — so Trace.Oyster.PicklerThrowTest can be shown FAILING in the same
//              process, on the same frames, on the same geometry. NEVER SHIP 1.
// =================================================================================================
static TAutoConsoleVariable<int32> CVarOysterLegacyThrow(
	TEXT("Trace.Oyster.LegacyThrow"), 0,
	TEXT("TEST ARM ONLY. 0 (shipped): Pickler is released clear of geometry and flown in "
	     "distance-capped sub-steps, so its landing point does not depend on the frame rate. "
	     "1: restores the pre-v19 muzzle release and one-Euler-step-per-frame flight so "
	     "Trace.Oyster.PicklerThrowTest can be shown failing. Never ship 1."),
	ECVF_Cheat);

// =================================================================================================
// SPEC v26 §6 — THE TEST ARM FOR BOTH HALVES OF OYSTER'S E
//
// 0 (shipped): the Pickler jar detonates when the pull finishes (§6b), and any poison landing on an
//              enemy resets the E cooldown (§6a).
// 1:           restores the pre-v26 behaviour of both — the jar lies there waiting for a touch or a
//              jump, and a poison refunds nothing — so Trace.Oyster.ETest can be shown failing in the
//              same process, in the same match, on the same fixtures. NEVER SHIP 1.
// =================================================================================================
static TAutoConsoleVariable<int32> CVarOysterLegacyE(
	TEXT("Trace.Oyster.LegacyE"), 0,
	TEXT("TEST ARM ONLY. 0 (shipped, spec v26 §6): the Pickler jar explodes when its pull finishes, "
	     "and poisoning an enemy resets Oyster's E cooldown. 1: restores the pre-v26 behaviour of "
	     "both, so Trace.Oyster.ETest can be shown failing. Never ship 1."),
	ECVF_Cheat);

namespace TraceOysterJar
{
	bool IsLegacyThrow()
	{
		return CVarOysterLegacyThrow.GetValueOnAnyThread() != 0;
	}

	bool IsLegacyE()
	{
		return CVarOysterLegacyE.GetValueOnAnyThread() != 0;
	}

	/**
	 * A hitch must not teleport a jar through the world — the same clamp, and the same reason, as
	 * ATraceCore's loose-ball integrator ("A hitch must not teleport the Core").
	 */
	constexpr float MaxFrameSeconds = 0.1f;

	/**
	 * Longest distance ONE swept step may cover.
	 *
	 * Two jobs. It keeps the straight chord each sweep tests close to the curve it stands in for (at
	 * the 1900 uu/s throw speed a 45 uu chord sags under a tenth of a uu), and it makes the number of
	 * contact tests along the arc a function of the ARC rather than of the frame rate — which is what
	 * stops a 20 fps machine and a 144 fps machine resolving the same landing at different points.
	 */
	constexpr float MaxStepDistanceUU = 45.f;

	/** Safety valve. A clamped 0.1 s frame at the fastest throw needs 6; 32 can never be reached. */
	constexpr int32 MaxSubStepsPerFrame = 32;

	/**
	 * SPEC v26 §6b. The shortest fuse a Pickler jar may be given, in seconds.
	 *
	 * One 60 Hz frame. Not a design number — a visibility floor. The jar has to be on the ground for
	 * at least one rendered frame or the "lands, then explodes" the section describes would read as
	 * "vanished in mid-air", and OysterPicklerPullSpeed being tuned to something enormous (or the
	 * pull being switched off entirely with speed 0, which makes the derived travel time zero) must
	 * not be able to produce that.
	 */
	constexpr float MinDetonateDelaySeconds = 1.f / 60.f;
}

ATraceOysterJar::ATraceOysterJar()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;
	SetReplicateMovement(true);      // the lob is short but it is the only thing clients see move
	bAlwaysRelevant = true;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetRootComponent(Collision);
	// ONE definition of the jar's size: the release-point probe has to use the same sphere the flight
	// is swept with, or it would clear a gap the jar does not actually fit through.
	Collision->InitSphereRadius(GetJarCollisionRadiusUU());
	// WORLD STATIC ONLY. Blocking geometry is what makes the lob land; ignoring everything else is
	// what stops the jar becoming a movement base, a bullet shield or a thing players can shove.
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetGenerateOverlapEvents(false);
	Collision->SetCanEverAffectNavigation(false);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCollisionProfileName(TEXT("NoCollision"));
	Mesh->SetCastShadow(false);
	Mesh->SetRelativeScale3D(FVector(0.35f, 0.35f, 0.35f));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		Mesh->SetStaticMesh(CylinderFinder.Object);
	}
}

void ATraceOysterJar::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceOysterJar, bIsPickler);
	DOREPLIFETIME(ATraceOysterJar, bGrounded);
}

void ATraceOysterJar::BeginPlay()
{
	Super::BeginPlay();
}

float ATraceOysterJar::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

void ATraceOysterJar::Initialise(UTraceAbilityComponent* InSourceComp, ETraceTeam InOwnerTeam,
                                 bool bInIsPickler, const FVector& InVelocity)
{
	SourceComponent = InSourceComp;
	OwnerTeam = InOwnerTeam;
	bIsPickler = bInIsPickler;
	FlightVelocity = InVelocity;

	// MEASUREMENT ONLY — nothing in the flight reads these. Trace.Oyster.PicklerThrowTest compares the
	// landing point of two throws that started from the identical pair, which is the whole of the
	// "the same throw must land in the same place" claim in spec v19 §4.4.
	LaunchLocation = GetActorLocation();
	LaunchVelocity = InVelocity;

	if (FlightVelocity.IsNearlyZero())
	{
		// The dash jar. It is dropped where he is, so it is on the ground from the first frame and
		// its 4 s starts now.
		Land();
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceOysterJar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority())
	{
		return;   // every rule is server-side; a client's jar is a mesh
	}

	if (!bGrounded)
	{
		TickFlight(DeltaSeconds);
		return;
	}

	TickGrounded();
}

void ATraceOysterJar::TickFlight(float DeltaSeconds)
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	// Ballistics against WORLD gravity, integrated here rather than by a projectile movement
	// component. The lob is under a second, it must not be affected by the pawn gravity scale the
	// movement component keeps re-pushing, and a swept SetActorLocation gives the landing point for
	// free — which is the one thing the jar actually needs from its flight.
	const float GravityZ = WorldPtr->GetGravityZ();

	if (TraceOysterJar::IsLegacyThrow())
	{
		// THE PRE-v19 ARC, KEPT VERBATIM AS THE RED ARM. One semi-implicit Euler step per rendered
		// frame: the drop is over-estimated by 0.5*g*dt*t, so the SAME throw fell short by tens of uu
		// more on a slow frame than on a fast one and the landing point moved with the frame rate.
		FlightVelocity.Z += GravityZ * DeltaSeconds;

		FHitResult LegacyHit;
		++FlightSweepCount;
		SetActorLocation(GetActorLocation() + FlightVelocity * DeltaSeconds, /*bSweep*/ true, &LegacyHit);

		if (LegacyHit.bBlockingHit)
		{
			Land();
		}
		return;
	}

	// SPEC v19 §4.4, "more consistent to throw".
	//
	// TWO changes, and the first is the one that matters. Each sub-step advances the position with the
	// EXACT constant-acceleration solution (p += v*dt + 0.5*g*dt^2) instead of with velocity alone, so
	// the total displacement over a flight is p0 + v0*T + 0.5*g*T^2 whatever sizes the steps came in —
	// i.e. the arc no longer depends on the frame rate at all, rather than merely depending on it less.
	// Second, no single swept step may cover more than MaxStepDistanceUU, so where along that arc the
	// contact is detected is also a property of the arc and not of the machine.
	float TimeLeft = FMath::Clamp(DeltaSeconds, 0.f, TraceOysterJar::MaxFrameSeconds);

	for (int32 SubStep = 0; SubStep < TraceOysterJar::MaxSubStepsPerFrame && TimeLeft > UE_SMALL_NUMBER; ++SubStep)
	{
		const float Speed = static_cast<float>(FlightVelocity.Size());
		const float StepSeconds = (Speed > 1.f)
			? FMath::Min(TimeLeft, TraceOysterJar::MaxStepDistanceUU / Speed)
			: TimeLeft;

		const FVector StepDelta = FlightVelocity * StepSeconds
			+ FVector(0.f, 0.f, 0.5f * GravityZ * StepSeconds * StepSeconds);

		FHitResult SweepHit;
		++FlightSweepCount;
		SetActorLocation(GetActorLocation() + StepDelta, /*bSweep*/ true, &SweepHit);

		FlightVelocity.Z += GravityZ * StepSeconds;
		TimeLeft -= StepSeconds;

		if (SweepHit.bBlockingHit)
		{
			Land();
			return;
		}
	}
}

void ATraceOysterJar::TickGrounded()
{
	// "An enemy TOUCHING a jar breaks it."
	if (ATraceCharacter* Toucher = FindToucher())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Oyster] Jar broken by %s."), *GetNameSafe(Toucher));
		ServerBreakNow(TEXT("touched by an enemy"));
		return;
	}

	// SPEC v26 §6b — THE PICKLER JAR'S FUSE. Second, deliberately: an enemy who walks into it during
	// the pull still breaks it early, which is the ordinary jar rule and is what "it is still a normal
	// jar for that window" means. This is the path that fires when nobody does.
	//
	// ServerBreakNow, NOT Destroy: "explode" is the poison burst plus the v16 §3 cloud, the same event
	// a touch or a jar-jump produces. There is one break in this class and this is another way in, not
	// another kind of ending.
	if (DetonateMatchTime > 0.f && MatchTimeNow() >= DetonateMatchTime)
	{
		ServerBreakNow(TEXT("Pickler's pull finished — spec v26 §6b"));
		return;
	}

	// "Jars last 4 s on the ground." Expiring untouched is NOT a break: nothing bursts, it is simply
	// gone. §6 ties the poison to being broken, not to the jar existing. A Pickler jar can no longer
	// reach this line — its fuse is clamped below the lifetime — and that is on purpose.
	if (ExpiryMatchTime > 0.f && MatchTimeNow() >= ExpiryMatchTime)
	{
		Destroy();
	}
}

// =================================================================================================
// Landing
// =================================================================================================

float ATraceOysterJar::GetPicklerDetonateDelaySeconds()
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	const float PullRadius = FMath::Max(0.f, Settings.OysterPicklerPullRadiusUU);
	const float PullSpeed  = FMath::Max(0.f, Settings.OysterPicklerPullSpeed);
	const float Scale      = FMath::Max(0.f, Settings.OysterPicklerDetonateDelayScale);

	// THE BASE: how long the pull itself takes. Someone standing at the very edge of the pull radius,
	// launched straight at the jar at the pull speed, closes that gap in exactly this. A pull with no
	// speed has no travel time, and then there is nothing to wait for and the floor below takes over.
	const float PullTravelSeconds = (PullSpeed > UE_SMALL_NUMBER) ? (PullRadius / PullSpeed) : 0.f;

	// The ceiling is the jar's own ground lifetime, so a big scale cannot produce a Pickler jar that
	// EXPIRES instead of exploding — expiring is silent (TickGrounded destroys it without a burst) and
	// would look exactly like the feature not working. Both ends move with the knobs they are cut from.
	const float LifetimeSeconds = FMath::Max(0.25f, Settings.OysterJarLifetimeSeconds);

	return FMath::Clamp(PullTravelSeconds * Scale,
		TraceOysterJar::MinDetonateDelaySeconds,
		FMath::Max(TraceOysterJar::MinDetonateDelaySeconds, LifetimeSeconds));
}

void ATraceOysterJar::Land()
{
	if (bGrounded)
	{
		return;
	}

	bGrounded = true;
	FlightVelocity = FVector::ZeroVector;
	LandedMatchTime = MatchTimeNow();
	ExpiryMatchTime = LandedMatchTime + FMath::Max(0.25f, UTraceSettings::Get().OysterJarLifetimeSeconds);

	if (bIsPickler && !bLandingEffectFired)
	{
		bLandingEffectFired = true;
		FireLandingEffect();

		// SPEC v26 §6b: "the E jar's should explode once the pull animation finishes, rather than
		// waiting for a jump to trigger them."
		//
		// Armed HERE, in the same branch as the impact, so the fuse and the pull cannot come apart:
		// the only jar that pulls is the only jar that detonates, and it is lit at the instant the
		// pull starts. Stored RELATIVE to the landing, exactly like ExpiryMatchTime on the line above
		// — the delay is a property of the pull, and the landing is when the pull happened.
		//
		// The red arm leaves DetonateMatchTime at 0, which is exactly the pre-v26 jar: no fuse, and
		// TickGrounded's detonation branch never fires.
		if (!TraceOysterJar::IsLegacyE())
		{
			DetonateMatchTime = LandedMatchTime + GetPicklerDetonateDelaySeconds();
		}
	}
}

void ATraceOysterJar::ServerForceLandNow()
{
	if (HasAuthority())
	{
		Land();
	}
}

FVector ATraceOysterJar::ResolveReleaseLocation(const UWorld* WorldPtr, const FVector& InsideThrower,
                                                const FVector& DesiredRelease, const AActor* IgnoreActor)
{
	if (WorldPtr == nullptr || TraceOysterJar::IsLegacyThrow())
	{
		return DesiredRelease;   // the red arm releases at the raw muzzle, exactly as before v19
	}

	const float Radius = GetJarCollisionRadiusUU();

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceOysterRelease), /*bTraceComplex=*/false, IgnoreActor);
	if (IgnoreActor != nullptr)
	{
		Params.AddIgnoredActor(IgnoreActor);
	}

	// BY OBJECT TYPE, NOT BY CHANNEL, and this is load-bearing.
	//
	// It was SweepSingleByChannel(ECC_WorldStatic) with a comment claiming pawns were not in it. They
	// are: a channel sweep asks every component how it RESPONDS to the WorldStatic channel, and the
	// pawn profile blocks it, so anybody standing next to Oyster clamped his release and moved his
	// landing point. Trace.Oyster.PicklerThrowTest caught it in the open, on a frame where a bot
	// wandered past — a throw that lands somewhere else because a team-mate walked by is exactly the
	// "inconsistent to throw" this section exists to remove, and the guard was causing it.
	//
	// An object-type query asks instead "is there a WorldStatic THING here", which is precisely what
	// ATraceOysterJar's own sphere blocks against and precisely what can swallow a jar. Pawns are
	// WorldDynamic and are now not consulted at all.
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);

	FHitResult SweepHit;
	const bool bBlocked = WorldPtr->SweepSingleByObjectType(
		SweepHit, InsideThrower, DesiredRelease, FQuat::Identity, ObjectParams,
		FCollisionShape::MakeSphere(Radius), Params);

	if (!bBlocked)
	{
		return DesiredRelease;
	}

	// Blocked: fall back along the segment to where the sphere last fitted. Hit.Location IS that
	// point for a normal hit; a hit that started penetrating has no usable one, and the eye is the
	// only place left that is known to be standing in clear space.
	if (SweepHit.bStartPenetrating)
	{
		return InsideThrower;
	}
	return SweepHit.Location;
}

void ATraceOysterJar::FireLandingEffect()
{
	UWorld* WorldPtr = GetWorld();
	UTraceAbilityComponent* SourceComp = SourceComponent.Get();
	if (WorldPtr == nullptr || SourceComp == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float DamageRadius = FMath::Max(1.f, Settings.OysterPicklerDamageRadiusUU);
	const float PullRadius   = FMath::Max(1.f, Settings.OysterPicklerPullRadiusUU);
	const float PullSpeed    = FMath::Max(0.f, Settings.OysterPicklerPullSpeed);
	const float ImpactDamage = FMath::Max(0.f, Settings.OysterPicklerDamage);
	const FVector Origin = GetActorLocation();

	int32 DamagedCount = 0;
	int32 PulledCount = 0;

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive())
		{
			continue;
		}

		const float Distance = FVector::Dist(Candidate->GetActorLocation(), Origin);

		// --- "deals 30 damage in an area" -----------------------------------------------------------
		if (Distance <= DamageRadius)
		{
			// THE CHOKE POINT. ApplyAbilityDamage asks CanAffectTargetDetailed(Damage) itself and
			// returns 0 for a carrier, for a team-mate and for the dead. There is no second path.
			const float Dealt = SourceComp->ApplyAbilityDamage(Candidate, ImpactDamage, TEXT("OysterPickler"));
			if (Dealt > 0.f)
			{
				++DamagedCount;
				TraceOyster::RecordEffect(Candidate, TEXT("Pickler impact damage"),
					&TraceOyster::FEffectTally::PicklerDamageHits);
			}
		}

		// --- "pulls enemies within a small radius toward it" ------------------------------------------
		if (Distance <= PullRadius && PullSpeed > 0.f)
		{
			// THE CHOKE POINT AGAIN, and with a DIFFERENT effect class. A pull is Control, so it is
			// governed by §4's [ASSUMPTION] (bCarrierImmuneToAbilityControl) rather than by the
			// unconditional damage rule — one flag reverses it, and it reverses here and nowhere else.
			if (!SourceComp->CanAffectTarget(Candidate, ETraceAbilityEffect::Control))
			{
				continue;
			}

			FVector ToJar = Origin - Candidate->GetActorLocation();
			if (ToJar.IsNearlyZero())
			{
				continue;
			}
			ToJar.Normalize();

			// LaunchCharacter, not a raw Velocity write: it is the engine's own "something threw this
			// pawn" entry point, it goes through the movement component's pending-launch path, and it
			// is what the rest of the project would use. See the report for the client-prediction
			// caveat that comes with it.
			Candidate->LaunchCharacter(ToJar * PullSpeed, /*bXYOverride*/ true, /*bZOverride*/ true);
			++PulledCount;
			TraceOyster::RecordEffect(Candidate, TEXT("Pickler pull"), &TraceOyster::FEffectTally::PicklerPulls);
		}
	}

	// The fuse is REPORTED with the pull it is cut from, so a log line is enough to tell whether a
	// retune moved them together: "pulled 2 in 380 uu (0.29s of travel)" beside "detonates in 0.29s".
	const float PullTravelSeconds = (PullSpeed > UE_SMALL_NUMBER) ? (PullRadius / PullSpeed) : 0.f;
	UE_LOG(LogTraceGame, Log,
		TEXT("[Oyster] Pickler landed at %s: %.0f damage to %d in %.0f uu, pulled %d in %.0f uu (%.2fs of pull at "
		     "%.0f uu/s). SPEC v26 §6b: it detonates in %.2fs rather than waiting to be broken."),
		*Origin.ToCompactString(), ImpactDamage, DamagedCount, DamageRadius, PulledCount, PullRadius,
		PullTravelSeconds, PullSpeed, GetPicklerDetonateDelaySeconds());
}

// =================================================================================================
// Breaking
// =================================================================================================

bool ATraceOysterJar::IsEnemyOfOwner(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr)
	{
		return false;
	}
	const ETraceTeam TheirTeam = Candidate->GetTeam();
	return OwnerTeam != ETraceTeam::None && TheirTeam != ETraceTeam::None && TheirTeam != OwnerTeam;
}

ATraceCharacter* ATraceOysterJar::FindToucher() const
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return nullptr;
	}

	const float BreakRadius = FMath::Max(1.f, UTraceSettings::Get().OysterJarBreakRadiusUU);
	const FVector Origin = GetActorLocation();

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive() || !IsEnemyOfOwner(Candidate))
		{
			continue;
		}
		if (FVector::Dist(Candidate->GetActorLocation(), Origin) <= BreakRadius)
		{
			return Candidate;
		}
	}
	return nullptr;
}

void ATraceOysterJar::ServerBreakNow(const TCHAR* Why)
{
	if (!HasAuthority())
	{
		return;
	}

	UE_LOG(LogTraceGame, Verbose, TEXT("[Oyster] Jar breaking (%s)."), Why);
	Burst();
	Destroy();
}

void ATraceOysterJar::Burst()
{
	UWorld* WorldPtr = GetWorld();
	UTraceAbilityComponent* SourceComp = SourceComponent.Get();
	if (WorldPtr == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Radius = FMath::Max(1.f, Settings.OysterPoisonRadiusUU);
	const FVector Origin = GetActorLocation();
	int32 PoisonedCount = 0;

	// SPEC v16 §3: "Add a small, semi transparent cloud when oyster's poison jars break. This cloud
	// should be the radius of the explosion."
	//
	// Spawned from the SAME `Radius` local the loop below tests every candidate against — not from a
	// second read of the knob, and certainly not from a number of its own. That is the whole of §3's
	// second sentence: if the two were ever allowed to diverge the cloud would be telling players the
	// poison is somewhere it is not, which is worse than drawing nothing.
	//
	// FIRST, and unconditionally: the cloud is the shape of the BURST, not a report on who it caught.
	// A jar that breaks in an empty corridor still poisoned that volume for the next four seconds and
	// still has to say so. It is cosmetic — nothing below reads it, and it touches nobody.
	const ATraceOysterPoisonCloud* Cloud = ATraceOysterPoisonCloud::ServerSpawnForBurst(
		WorldPtr, Origin, Radius, Settings.OysterPoisonDurationSeconds);

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive())
		{
			continue;
		}
		if (FVector::Dist(Candidate->GetActorLocation(), Origin) > Radius)
		{
			continue;
		}

		// THE CHOKE POINT, ASKED FOR BOTH HALVES OF WHAT POISON IS.
		//
		// Poison is 3 damage a tick (Damage) AND -30% speed (Control), so a target the choke point
		// refuses for BOTH is not poisoned at all — no component, no tick, nothing to leak. A target
		// it allows for either one gets the component, which then re-asks per tick and per frame and
		// applies only the half it is allowed. That is why a carrier ends up with no poison component
		// whatsoever rather than an inert one.
		const bool bMayDamage = (SourceComp != nullptr)
			? SourceComp->CanAffectTarget(Candidate, ETraceAbilityEffect::Damage)
			: UTraceAbilityComponent::CanAffect(nullptr, Candidate, ETraceAbilityEffect::Damage);
		const bool bMayControl = (SourceComp != nullptr)
			? SourceComp->CanAffectTarget(Candidate, ETraceAbilityEffect::Control)
			: UTraceAbilityComponent::CanAffect(nullptr, Candidate, ETraceAbilityEffect::Control);

		if (!bMayDamage && !bMayControl)
		{
			continue;
		}

		if (UTraceOysterPoisonComponent::ApplyTo(Candidate, SourceComp) != nullptr)
		{
			++PoisonedCount;
		}
	}

	// The cloud is reported from the POINTER, not from the fact that the call was made: "spawned a
	// cloud" and "asked for a cloud" are different claims and only one of them is worth logging.
	UE_LOG(LogTraceGame, Log, TEXT("[Oyster] Jar burst at %s: poisoned %d within %.0f uu; cloud %s."),
		*Origin.ToCompactString(), PoisonedCount, Radius,
		(Cloud != nullptr)
			? *FString::Printf(TEXT("spawned at %.0f uu (spec v16 §3)"), Cloud->GetCloudRadiusUU())
			: TEXT("NOT spawned"));
}
