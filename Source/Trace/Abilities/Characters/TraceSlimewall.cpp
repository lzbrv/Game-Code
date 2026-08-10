// Trace — SLIMEBALL's Slimewall. See the header for the spec v18 §2 reading, for why the slab has no
// collision at all, and for why the choke point is re-asked every tick.

#include "Abilities/Characters/TraceSlimewall.h"

#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                      // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// THE RED ARMS. Named after the file so the unity build cannot collide them (see
// Scripts/check-jumbo-build-collisions.py).
// =================================================================================================

/**
 * *** THE SHOT-THROUGH ARM. This one is INVERTED and that is deliberate. ***
 *
 * Every other arm on this project ships at 1 and is switched OFF to reproduce a failure. This one
 * ships at 0 and is switched ON, because the failure it reproduces is a wall that BLOCKS bullets —
 * i.e. the bug §2 forbids — and there is no way to express "remove the absence of collision".
 *
 * With it at 1 the slab gets BlockAll, which truncates UTraceLagCompensationComponent::ResolveHitscan's
 * ECC_Visibility world trace short of anybody standing behind it. That is exactly the silent failure
 * the header describes, and Trace.Slimeball.Verify uses it to prove its shot-through check can fail.
 */
static TAutoConsoleVariable<int32> CVarSlimewallBlocksBullets(
	TEXT("Trace.Slimeball.WallBlocksBullets"),
	0,
	TEXT("TEST ARM ONLY, and INVERTED. 0 (shipped): the Slimewall has no collision and cannot stop a "
	     "bullet, a body or a trace — spec v18 §2's 'can be shot through'. 1: the slab is given "
	     "BlockAll, which reproduces the bug where the wall silently eats hitscan. Never ship 1."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarSlimewallSlow(
	TEXT("Trace.Slimeball.Slow"),
	1,
	TEXT("Dev/red arm. 1 (default) = walking through a Slimewall slows enemies (spec v18 §2: 35%). "
	     "0 = the wall still goes up, still replicates and still blocks sight, but slows nobody, so "
	     "every slow assertion must go red."),
	ECVF_Cheat);

namespace TraceSlimewallTuning
{
	/** /Engine/BasicShapes primitives are 100 uu across, so a scale of 1 is 100 uu on that axis. */
	constexpr float BasicShapeSizeUU = 100.f;

	/**
	 * Emissive strength for M_TraceNeon. Well below the arena's 3.5 on purpose: the slab is a large
	 * flat surface right in front of a player's face, and a bloom-clearing wall of it is a flashbang.
	 */
	constexpr float SlabGlow = 0.9f;

	/** Slime. Not team-coloured — both teams read the same object, and it is a hazard, not a marker. */
	const FLinearColor SlimeColor(0.32f, 0.78f, 0.18f, 1.f);
}

// =================================================================================================
// The alarm
// =================================================================================================

namespace TraceSlimewall
{
	FSlowTally& CarrierTally()
	{
		// Function-local statics: one set per process, alive before any world exists, never torn down
		// between PIE sessions. Same reasoning as TraceAbilityIntegration::Counters().
		static FSlowTally Instance;
		return Instance;
	}

	FSlowTally& OtherTally()
	{
		static FSlowTally Instance;
		return Instance;
	}

	void ResetTallies()
	{
		CarrierTally().Reset();
		OtherTally().Reset();
	}

	void RecordEffect(const ATraceCharacter* Target, const TCHAR* VectorName, int32 FSlowTally::* Field)
	{
		const bool bCarrier = UTraceAbilityComponent::IsCarrier(Target);
		FSlowTally& Tally = bCarrier ? CarrierTally() : OtherTally();
		++(Tally.*Field);

		if (bCarrier)
		{
			// LOUD, and at Error. Spec §4's invariant has been broken if this ever prints on a shipped
			// build; a Verbose line would be indistinguishable from the noise a match already makes.
			UE_LOG(LogTraceGame, Error,
				TEXT("[Slimewall] *** SPEC v18 §4 VIOLATION: '%s' landed on the CORE CARRIER %s. ***"),
				VectorName, *GetNameSafe(Target));
		}
	}
}

// =================================================================================================
// UTraceSlimewallSlowComponent — the debuff on the victim
// =================================================================================================

UTraceSlimewallSlowComponent::UTraceSlimewallSlowComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	SetIsReplicatedByDefault(true);
}

void UTraceSlimewallSlowComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UTraceSlimewallSlowComponent, EndMatchTime);
	DOREPLIFETIME(UTraceSlimewallSlowComponent, bSlowActive);
}

ATraceCharacter* UTraceSlimewallSlowComponent::GetVictim() const
{
	return Cast<ATraceCharacter>(GetOwner());
}

float UTraceSlimewallSlowComponent::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

UTraceSlimewallSlowComponent* UTraceSlimewallSlowComponent::Find(const ATraceCharacter* Target)
{
	return (Target != nullptr) ? Target->FindComponentByClass<UTraceSlimewallSlowComponent>() : nullptr;
}

UTraceSlimewallSlowComponent* UTraceSlimewallSlowComponent::ApplyTo(ATraceCharacter* Target, APlayerState* InSource,
                                                                    float LingerSeconds)
{
	if (Target == nullptr || !Target->HasAuthority() || !Target->IsAlive())
	{
		return nullptr;
	}

	UTraceSlimewallSlowComponent* Existing = Find(Target);
	if (Existing == nullptr)
	{
		Existing = NewObject<UTraceSlimewallSlowComponent>(Target);
		if (Existing == nullptr)
		{
			return nullptr;
		}
		Existing->RegisterComponent();
	}

	Existing->SourcePlayerState = InSource;

	// ASSIGNMENT, NOT ADDITION. Standing in the slab refreshes the linger rather than stacking it —
	// otherwise a player who walked the long way through a 1100 uu wall would come out the far side
	// slowed for the rest of the round.
	Existing->EndMatchTime = Existing->MatchTimeNow() + FMath::Max(0.f, LingerSeconds);

	// Decided immediately rather than on the next tick: the wall has already asked the choke point to
	// get here, and leaving bSlowActive false for one tick would make the very first frame of the slow
	// a frame in which nothing happened — the difference between "it slows you" and "it slows you,
	// eventually".
	Existing->ServerRefreshNow();

	// ServerRefreshNow destroys the component on an expired linger. That cannot happen on the line
	// above (EndMatchTime was just pushed into the future), but returning a pointer without checking
	// would make this function's contract depend on that never changing.
	return IsValid(Existing) ? Existing : nullptr;
}

float UTraceSlimewallSlowComponent::GetSpeedMultiplier() const
{
	if (!bSlowActive)
	{
		return 1.f;
	}

	// Clamped at 0.95 for the same reason Oyster's is: a knob of 1.0 would be a total stop, which is
	// not a slow — it is a stun, and §2 does not ask for one.
	const float Fraction = FMath::Clamp(UTraceSettings::Get().SlimewallSlowFraction, 0.f, 0.95f);
	return 1.f - Fraction;
}

void UTraceSlimewallSlowComponent::ServerRefreshNow()
{
	ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr || !Victim->HasAuthority())
	{
		return;
	}

	// STRICTLY GREATER, AND THE ONE CHARACTER MATTERS — AT A LINGER OF 0.
	//
	// SlimewallSlowLingerSeconds is clamped at 0, and 0 is a reasonable thing for a designer to ask
	// for: "slow them while they are standing in it, and not a moment longer". With `>=` that reading
	// was silently inverted. ApplyTo pushes EndMatchTime to exactly now and then calls this function
	// on the next statement; `now >= now` called the slow expired before it had ever been in force,
	// the component destroyed itself, ApplyTo returned null, and the wall slowed NOBODY — not even
	// the player standing inside it — with no warning and no log line. That reads in a playtest as
	// "the Slimewall does nothing", which is the hardest kind of bug to report.
	//
	// With `>` the slow is in force for the frame it is applied, and the wall re-applies it every
	// tick a body is inside, so a linger of 0 means exactly what it says. Nothing moves at the
	// shipped 0.75 s: the two forms differ only on the single frame where now == EndMatchTime.
	const bool bExpired = (MatchTimeNow() > EndMatchTime);

	// *** THE CHOKE POINT, RE-ASKED. See the header: this is the "picked up the Core mid-slow" case,
	// and it is the whole reason this is a ticking component rather than a one-shot velocity write. ***
	const bool bAllowed =
		!bExpired
		&& Victim->IsAlive()
		&& CVarSlimewallSlow.GetValueOnAnyThread() != 0
		&& UTraceAbilityComponent::CanAffect(SourcePlayerState.Get(), Victim, ETraceAbilityEffect::Control);

	if (bAllowed != bSlowActive)
	{
		bSlowActive = bAllowed;
	}

	if (bSlowActive)
	{
		TraceSlimewall::RecordEffect(Victim, TEXT("35% slow (in force this tick)"),
			&TraceSlimewall::FSlowTally::SlowFrames);
	}

	if (bExpired)
	{
		// The slow is a property of a body and it is over. Destroying the component rather than
		// leaving an inert one behind keeps TraceAbilityDebuff's FindComponentByClass honest.
		DestroyComponent();
	}
}

void UTraceSlimewallSlowComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                 FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr)
	{
		return;
	}

	if (Victim->HasAuthority())
	{
		ServerRefreshNow();
		if (!IsValid(this))
		{
			return;   // ServerRefreshNow destroyed us on expiry
		}
	}

	ApplySlowClamp();
}

void UTraceSlimewallSlowComponent::ApplySlowClamp()
{
	if (!bSlowActive)
	{
		return;
	}

	ATraceCharacter* Victim = GetVictim();
	if (Victim == nullptr || !Victim->IsAlive())
	{
		return;
	}

	// Only the machines that actually simulate this pawn. A simulated proxy's velocity is replicated;
	// clamping it there would fight the interpolation and would slow somebody else's view of a player
	// the server never slowed.
	if (!Victim->HasAuthority() && !Victim->IsLocallyControlled())
	{
		return;
	}

	UTraceCharacterMovementComponent* MoveComp = Victim->GetTraceMovement();
	if (MoveComp == nullptr)
	{
		return;
	}

	// [ASSUMPTION], and the same two halves Oyster's poison flags, for the same reasons:
	//
	// NOT DURING A DASH: a dash is velocity on rails and GetMaxSpeed() returns DashSpeed while it
	// runs, so clamping there would make the wall a dash nerf of a completely different magnitude
	// than 35%. The LINGER is what stops that being a hole — a player who dashes through the slab
	// comes out the far side already slimed and is slowed for SlimewallSlowLingerSeconds after.
	//
	// GROUND ONLY: the air ceilings here are the momentum model (soft cap, hard cap, falloff) and are
	// not expressed through GetMaxSpeed(); clamping planar air speed to 65% of the WALK speed would
	// not be a slow, it would delete air momentum entirely.
	if (MoveComp->IsFalling() || Victim->IsDashing())
	{
		return;
	}

	// *** THE FRACTION IS APPLIED HERE, AND THAT IS A TEMPORARY STATE OF AFFAIRS. ***
	//
	// The framework's proper path for an external debuff is TraceAbilityDebuff::GetMoveSpeedMultiplier
	// (Abilities/TraceAbilityTypes.cpp), which UTraceCharacterMovementComponent::GetMaxSpeed() already
	// multiplies by — so ACCELERATION would target the slowed ceiling instead of only being clipped
	// after the fact. That aggregator knows about Oyster's poison and nothing else, and this slice does
	// not own that file; adding the line is a cross-file need, filed in the report.
	//
	// WHEN THAT LINE LANDS, DELETE THE `* GetSpeedMultiplier()` BELOW AND NOTHING ELSE. GetMaxSpeed()
	// will already be the slowed ceiling and multiplying a second time compounds 0.65 into 0.42 — a
	// -58% slow wearing a -35% label. Oyster's poison lived through exactly this transition and its
	// ApplySlowClamp carries the comment from the far side of it.
	const float Limit = FMath::Max(1.f, MoveComp->GetMaxSpeed() * GetSpeedMultiplier());

	FVector Vel = MoveComp->Velocity;
	const FVector Planar(Vel.X, Vel.Y, 0.f);
	const float PlanarSpeed = Planar.Size();
	if (PlanarSpeed > Limit && PlanarSpeed > KINDA_SMALL_NUMBER)
	{
		const FVector Capped = Planar * (Limit / PlanarSpeed);
		Vel.X = Capped.X;
		Vel.Y = Capped.Y;
		MoveComp->Velocity = Vel;
	}
}

// =================================================================================================
// ATraceSlimewall — construction
// =================================================================================================

ATraceSlimewall::ATraceSlimewall()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	bReplicates = true;
	SetReplicateMovement(false);      // it never moves; replicating a static transform is pure cost
	bAlwaysRelevant = true;           // a 4 s sight blocker must not be culled off a client
	SetNetUpdateFrequency(10.f);

	WallRoot = CreateDefaultSubobject<USceneComponent>(TEXT("WallRoot"));
	SetRootComponent(WallRoot);

	Slab = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Slab"));
	Slab->SetupAttachment(WallRoot);

	// *** SPEC v18 §2: "can be shot through". NOTHING may collide with this. ***
	// Not "no blocking on the weapon channel" — none at all. See the file header for the two traces
	// this protects and for why a blocking slab would fail silently rather than loudly.
	Slab->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Slab->SetCollisionProfileName(TEXT("NoCollision"));
	Slab->SetGenerateOverlapEvents(false);
	Slab->SetCanEverAffectNavigation(false);
	Slab->SetCastShadow(false);        // a 1100 uu slab casting shadows would darken the whole approach
	Slab->bReceivesDecals = false;

	// /Engine/BasicShapes ships with every install; M_TraceNeon is generated content and may not.
	// Both lookups are static so the cost is paid once per process, exactly as ATraceRippleActor does.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		SlabMesh = CubeFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		NeonMaterial = NeonFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BasicFinder.Succeeded())
	{
		FallbackMaterial = BasicFinder.Object;
	}
}

void ATraceSlimewall::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceSlimewall, WallCenter);
	DOREPLIFETIME(ATraceSlimewall, WallAim);
	DOREPLIFETIME(ATraceSlimewall, HalfExtents);
	DOREPLIFETIME(ATraceSlimewall, ExpireMatchTime);
	DOREPLIFETIME(ATraceSlimewall, SourcePlayerState);
	DOREPLIFETIME(ATraceSlimewall, SourceTeam);
}

ATraceSlimewall* ATraceSlimewall::ServerSpawn(UWorld* WorldPtr, APlayerState* InSource, ETraceTeam InSourceTeam,
                                              const FVector& InCenter, const FVector& InPlanarAim,
                                              const FVector& InHalfExtents, float InExpireMatchTime)
{
	if (WorldPtr == nullptr)
	{
		return nullptr;
	}

	const FVector Aim = FVector(InPlanarAim.X, InPlanarAim.Y, 0.f).GetSafeNormal();
	if (Aim.IsNearlyZero() || InHalfExtents.GetMin() <= 0.f)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Slimewall] refused a degenerate wall: aim=%s halfExtents=%s"),
			*InPlanarAim.ToCompactString(), *InHalfExtents.ToCompactString());
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ATraceSlimewall* Wall = WorldPtr->SpawnActor<ATraceSlimewall>(
		ATraceSlimewall::StaticClass(), InCenter, Aim.Rotation(), SpawnParams);
	if (Wall == nullptr)
	{
		return nullptr;
	}

	Wall->SourcePlayerState = InSource;
	Wall->SourceTeam        = InSourceTeam;
	Wall->WallCenter        = InCenter;
	Wall->WallAim           = Aim;
	Wall->HalfExtents       = InHalfExtents;
	Wall->ExpireMatchTime   = InExpireMatchTime;

	Wall->BuildSlabIfNeeded();

	UE_LOG(LogTraceGame, Log,
		TEXT("[Slimewall] up at %s facing %s — %.0f wide (through) x %.0f long (across) x %.0f tall, "
		     "expires at match time %.2f."),
		*InCenter.ToCompactString(), *Aim.ToCompactString(),
		InHalfExtents.X * 2.f, InHalfExtents.Y * 2.f, InHalfExtents.Z * 2.f, InExpireMatchTime);

	return Wall;
}

void ATraceSlimewall::BeginPlay()
{
	Super::BeginPlay();

	// On the server everything is already set by ServerSpawn. On a client the replicated properties
	// may not have landed yet, so the slab is built from Tick instead — BuildSlabIfNeeded is
	// idempotent and cheap until the extents arrive. Same shape as ATraceRippleActor's rings.
	BuildSlabIfNeeded();
}

float ATraceSlimewall::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceSlimewall::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	BuildSlabIfNeeded();

	if (!HasAuthority())
	{
		return;
	}

	// "Lasts 4 s." One deadline, one destroy, and the visuals leave with the actor rather than being
	// faded out by a second timer that could disagree. Tested BEFORE the shape guard below so a wall
	// that somehow reached the world malformed still expires.
	if (ExpireMatchTime > 0.f && MatchTimeNow() >= ExpireMatchTime)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Slimewall] expired after slowing %d player(s)."),
			LifetimeSlowed.Num());
		Destroy();
		return;
	}

	ServerUpdateSlows();
}

bool ATraceSlimewall::IsInsideWall(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr)
	{
		return false;
	}

	const FVector Extents = FVector(HalfExtents);
	if (Extents.GetMin() <= 0.f)
	{
		return false;   // on a client, until the replicated extents arrive
	}

	const FVector Forward = FVector(WallAim).GetSafeNormal();
	if (Forward.IsNearlyZero())
	{
		return false;
	}

	// Up x Forward is the slab's local +Y (the LENGTH axis). Forward is planar by construction, so
	// this is always well conditioned — no FindBestAxisVectors needed.
	const FVector Across = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();

	// The capsule, not the actor origin: a pawn is 34 uu of radius and 88 uu of half height, and a
	// point test would let somebody skim the face of the slab untouched while visibly inside it.
	float CapsuleRadius = 34.f;
	float CapsuleHalfHeight = 88.f;
	if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
	{
		CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}

	const FVector Delta = Candidate->GetActorLocation() - FVector(WallCenter);

	return FMath::Abs(FVector::DotProduct(Delta, Forward)) <= Extents.X + CapsuleRadius
		&& FMath::Abs(FVector::DotProduct(Delta, Across))  <= Extents.Y + CapsuleRadius
		&& FMath::Abs(Delta.Z)                             <= Extents.Z + CapsuleHalfHeight;
}

void ATraceSlimewall::ServerUpdateSlows()
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr || !HasAuthority())
	{
		return;
	}

	if (CVarSlimewallSlow.GetValueOnAnyThread() == 0)
	{
		return;   // RED ARM: the wall still stands and still blocks sight; it just slows nobody.
	}

	const float Linger = FMath::Max(0.f, UTraceSettings::Get().SlimewallSlowLingerSeconds);
	const bool bSlowsOwnTeam = UTraceSettings::Get().bSlimewallSlowsOwnTeam;

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive())
		{
			continue;
		}

		if (!IsInsideWall(Candidate))
		{
			continue;
		}

		// §2 [ASSUMPTION]: "it does not slow Slimeball or his own team." Written HERE rather than left
		// to the choke point's friendly-fire clause on purpose — that clause is off when friendly fire
		// is ON, and a designer turning friendly fire on must not accidentally make Slimeball's own
		// wall a trap for his team. bSlimewallSlowsOwnTeam reverses it in one knob.
		if (!bSlowsOwnTeam && SourceTeam != ETraceTeam::None && Candidate->GetTeam() == SourceTeam)
		{
			continue;
		}

		// *** THE CHOKE POINT. SPEC §4. ***
		// Control, because a slow is movement the target did not ask for. This single call is what
		// makes "a Core carrier is not slowed by a slime wall" true — there is no carrier test in this
		// file, and there must not be one, because a second copy of the rule is a second thing that
		// can rot. The STATIC form, because the wall outlives its owner's pawn.
		if (!UTraceAbilityComponent::CanAffect(SourcePlayerState, Candidate, ETraceAbilityEffect::Control))
		{
			continue;
		}

		if (UTraceSlimewallSlowComponent::ApplyTo(Candidate, SourcePlayerState, Linger) != nullptr)
		{
			TraceSlimewall::RecordEffect(Candidate, TEXT("slow applied"),
				&TraceSlimewall::FSlowTally::Applications);

			bool bAlready = false;
			for (const TWeakObjectPtr<ATraceCharacter>& Past : LifetimeSlowed)
			{
				if (Past.Get() == Candidate)
				{
					bAlready = true;
					break;
				}
			}
			if (!bAlready)
			{
				LifetimeSlowed.Add(Candidate);
				UE_LOG(LogTraceGame, Verbose,
					TEXT("[Slimewall] %s walked into the slime — %.0f%% slower, lingering %.2fs."),
					*GetNameSafe(Candidate),
					FMath::Clamp(UTraceSettings::Get().SlimewallSlowFraction, 0.f, 0.95f) * 100.f, Linger);
			}
		}
	}
}

// =================================================================================================
// The slab — "obstructs vision", and nothing else
// =================================================================================================

void ATraceSlimewall::BuildSlabIfNeeded()
{
	const FVector Extents = FVector(HalfExtents);
	if (Slab == nullptr || SlabMesh == nullptr || Extents.GetMin() <= 0.f)
	{
		return;
	}

	// The transform is re-asserted every call rather than only on the first, because on a client the
	// spawn transform and the replicated centre can arrive in either order and the second one must
	// win. It is three float compares in the common case.
	const FVector Forward = FVector(WallAim).GetSafeNormal();
	if (!Forward.IsNearlyZero())
	{
		SetActorLocationAndRotation(FVector(WallCenter), Forward.Rotation());
	}

	// A 100 uu cube scaled to (WIDTH, LENGTH, HEIGHT) in the actor's own frame: local +X is the aim
	// direction (the thickness you walk through), +Y is the span across it, +Z is up.
	Slab->SetRelativeScale3D(FVector(
		(Extents.X * 2.f) / TraceSlimewallTuning::BasicShapeSizeUU,
		(Extents.Y * 2.f) / TraceSlimewallTuning::BasicShapeSizeUU,
		(Extents.Z * 2.f) / TraceSlimewallTuning::BasicShapeSizeUU));

	// THE MESH IS SET ON EVERY NET MODE, INCLUDING A DEDICATED SERVER, and only the MATERIAL is
	// skipped there. Geometry is a gameplay fact and shaders are not: ATraceRippleActor can skip its
	// rings wholesale because they are pure decoration, whereas skipping this mesh would mean the
	// "has no collision" invariant was trivially true on a dedicated server for the wrong reason —
	// there would be nothing to have collision — and Trace.Slimeball.Verify's RED arm could not
	// reproduce there. An invariant that is only testable on one net mode is not an invariant.
	if (!bSlabBuilt)
	{
		bSlabBuilt = true;
		Slab->SetStaticMesh(SlabMesh);
	}

	// THE RED ARM, applied AFTER the mesh (a body instance exists to carry it) and re-applied on every
	// call so toggling the CVar mid-match takes effect on a wall that is already standing. On the
	// shipped arm the else-branch is the single line that makes "can be shot through" true.
	if (CVarSlimewallBlocksBullets.GetValueOnAnyThread() != 0)
	{
		if (Slab->GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics)
		{
			Slab->SetCollisionProfileName(TEXT("BlockAll"));
			Slab->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		}
	}
	else if (Slab->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
	{
		Slab->SetCollisionProfileName(TEXT("NoCollision"));
		Slab->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (bSlabDressed)
	{
		return;
	}

	// Shaders are not cooked for server targets, so a dedicated server never builds the material.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bSlabDressed = true;
		return;
	}

	bSlabDressed = true;

	UMaterialInterface* Parent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get();
	if (Parent == nullptr)
	{
		// No material at all: draw it anyway with the engine default. A grey slab still obstructs
		// vision, which is the gameplay half, and a wall you cannot see is far worse than an ugly one.
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Slimewall] neither M_TraceNeon nor BasicShapeMaterial resolved — the slab draws with the "
			     "engine default. It still blocks sight and still does not block bullets."));
		return;
	}

	SlabMID = UMaterialInstanceDynamic::Create(Parent, this);
	if (SlabMID == nullptr)
	{
		return;
	}

	// *** SlimewallOpacity IS A BRIGHTNESS KNOB, NOT A TRANSPARENCY ONE, AND THAT IS A LIMITATION. ***
	//
	// §2 asks for a wall that "obstructs vision", and an OPAQUE slab is the strongest possible reading
	// of that — but this project ships no translucent material: Scripts/generate_content.py builds
	// M_TraceNeon and M_TraceSurface as BLEND_Opaque, and blend mode is compiled into the parent, so a
	// dynamic instance cannot change it. (The one translucent thing nearby, EmissiveMeshMaterial, is
	// BLEND_Additive and writes no depth, i.e. it occludes NOTHING — the exact opposite of what this
	// ability needs, and the reason Oyster's poison cloud uses it.)
	//
	// So the knob dims the slab rather than making it see-through, and 1.0 is what §2 actually
	// describes. A translucent parent material is a content-side need and is filed in the report.
	const float Opacity = FMath::Clamp(UTraceSettings::Get().SlimewallOpacity, 0.f, 1.f);
	const FLinearColor SlabColor = TraceSlimewallTuning::SlimeColor * Opacity;

	// "Color" is what both M_TraceNeon and BasicShapeMaterial call it; "Glow" is neon only and is a
	// no-op on the fallback. Same three-parameter push ATraceRippleActor makes, for the same reason.
	SlabMID->SetVectorParameterValue(TEXT("Color"), SlabColor);
	SlabMID->SetVectorParameterValue(TEXT("BaseColor"), SlabColor);
	SlabMID->SetScalarParameterValue(TEXT("Glow"), TraceSlimewallTuning::SlabGlow * Opacity);
	SlabMID->SetScalarParameterValue(TEXT("Roughness"), 0.85f);
	Slab->SetMaterial(0, SlabMID);
}

bool ATraceSlimewall::HasAnyCollisionEnabled() const
{
	TArray<UPrimitiveComponent*> Primitives;
	GetComponents<UPrimitiveComponent>(Primitives);

	for (const UPrimitiveComponent* Primitive : Primitives)
	{
		if (Primitive != nullptr && Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
		{
			return true;
		}
	}
	return false;
}

FString ATraceSlimewall::DescribeSlabMaterial() const
{
	if (Slab == nullptr)
	{
		return TEXT("<no slab>");
	}
	if (const UMaterialInterface* Applied = Slab->GetMaterial(0))
	{
		return GetNameSafe(Applied);
	}
	return TEXT("<none>");
}
