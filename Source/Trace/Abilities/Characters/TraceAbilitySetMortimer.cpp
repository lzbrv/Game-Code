// Trace — MORTIMER. See the header for the spec v19 §3 reading, for the mantle's history, and for
// exactly which half of this character is live and which half is a knob waiting on somebody else's
// one-line call site.

#include "Abilities/Characters/TraceAbilitySetMortimer.h"

#include "CollisionQueryParams.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"                            // FTSTicker — Trace.Mortimer.QuakeTest
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"                            // DOREPLIFETIME
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

#define LOCTEXT_NAMESPACE "TraceMortimer"

// =================================================================================================
// THE RED ARMS. One per rule, each removing that rule and nothing else, so the verification command
// below can be made to FAIL on an otherwise identical build. Same shape and the same reasoning as
// TraceAbilitySetChut.cpp's three.
// =================================================================================================

/**
 * *** THE ONE THAT MATTERS. *** 0 removes the CanAffectTarget(Victim, Control) call from Quake's
 * per-victim path and NOTHING ELSE — the search, the radius, the falloff and the launch are
 * byte-identical. So a Core carrier standing beside Mortimer IS thrown, which is the founding
 * invariant broken, which is precisely what Trace.Mortimer.BlastCarrierTest's first arm has to
 * reproduce before its second arm's "0 uu/s" means anything at all.
 *
 * ECVF_Cheat, and never ship 0.
 */
static TAutoConsoleVariable<int32> CVarMortimerBlastChoke(
	TEXT("Trace.Mortimer.BlastChoke"),
	1,
	TEXT("Dev/red arm. 1 (default) = Quake asks the spec v14 §4 carrier choke point about every victim, so it "
	     "can never displace a Core carrier. 0 = the choke point is not consulted, so it CAN — the arm that "
	     "makes Trace.Mortimer.BlastCarrierTest's green half mean something. NEVER SHIP 0."),
	ECVF_Cheat);

/**
 * 0 removes §3's posture gate — "only while carrying the core AND standing on the ground or the top
 * of an object" — so E fires from anywhere. Exists so the posture assertions can go red, and so the
 * blast itself can be exercised in a fixture that has no Core.
 */
static TAutoConsoleVariable<int32> CVarMortimerBlastPosture(
	TEXT("Trace.Mortimer.BlastPosture"),
	1,
	TEXT("Dev/red arm. 1 (default) = Quake is refused unless Mortimer is carrying the Core AND grounded "
	     "(spec v19 §3). 0 = it fires from anywhere, so every posture assertion must go red."),
	ECVF_Cheat);

/**
 * 0 removes DEMO 20 ITEM 3's shockwave and nothing else — the posture gate, the search, the choke
 * point and the launch are byte-identical, so a Quake still throws exactly as many people exactly as
 * far. It is the RED ARM for "the quake isn't working": with this at 0 the ability is precisely what
 * the owner was pressing, and the screenshot must show nothing at all.
 *
 * A cosmetic with a red arm looks like over-engineering until you remember what the report has to
 * prove. "Here is a picture of a blue ring" is not evidence that the ring is the ability firing; two
 * pictures from the same fixture, one with the ring and one without, are.
 */
static TAutoConsoleVariable<int32> CVarMortimerQuakeWave(
	TEXT("Trace.Mortimer.QuakeWave"),
	1,
	TEXT("Dev/red arm. 1 (default) = a Quake spawns its shockwave (Demo 20 item 3). 0 = it does not, "
	     "which is exactly the invisible ability the owner reported. The knockback is identical either "
	     "way — this switch touches nothing but the cosmetic."),
	ECVF_Cheat);

const TCHAR* TraceMortimerBlastRefusalToString(ETraceMortimerBlastRefusal Reason)
{
	switch (Reason)
	{
	case ETraceMortimerBlastRefusal::Allowed:         return TEXT("Allowed");
	case ETraceMortimerBlastRefusal::NoPawn:          return TEXT("NoPawn");
	case ETraceMortimerBlastRefusal::NotCarryingCore: return TEXT("NotCarryingCore");
	case ETraceMortimerBlastRefusal::Airborne:        return TEXT("Airborne");
	default:                                          return TEXT("<invalid>");
	}
}

// =================================================================================================
// DEMO 20 ITEM 3 — QUAKE'S SHOCKWAVE
//
// THE DIAGNOSIS FIRST, BECAUSE IT DECIDES THE FIX. The owner reported "Mortimer's quake isn't
// working". It was working. Trace.Mortimer.QuakeTest drives the shipped E-key path
// (UTraceAbilityComponent::TryActivate) and measures that the press is accepted, the victims are
// launched and the 20 s cooldown starts. What did NOT exist was any output a human could perceive:
//
//     no particle, no Niagara, no sound, no camera shake, no multicast cosmetic, no debug draw —
//     `grep -inE 'niagara|particle|sound|shake|multicast|drawdebug' TraceAbilitySetMortimer.cpp`
//     returned nothing at all before this pass.
//
// So the ability had exactly two observable effects, and BOTH of them require somebody else to be
// standing nearby: enemies fly, and the HUD cooldown ring greys out. A Quake pressed in an empty
// room — which is what a person testing their own ability presses — produced NOTHING. And a Quake
// pressed WITHOUT the Core, or in the air, was refused silently: CheckBlastPosture() fills in a
// perfectly good sentence ("QUAKE NEEDS THE CORE") that TryActivate() then throws away.
//
// THOSE ARE TWO DIFFERENT BUGS AND THIS CLASS IS THE FIX FOR THE FIRST ONE. The second — surfacing
// the refusal to the player — belongs to the HUD, which this pass does not own, and is named in the
// report rather than half-built here.
// =================================================================================================

namespace TraceMortimerQuakeWaveFile
{
	/** Beads in the ring. 48 reads as a continuous circle out at 600 uu without being a mesh farm. */
	constexpr int32 BeadCount = 48;

	/** Where the ring starts, uu. Not zero: a ring of zero radius is 48 cylinders inside each other. */
	constexpr float StartRadiusUU = 40.f;

	/** Lifted off the floor so it does not z-fight the arena's ground plane. */
	constexpr float GroundLiftUU = 12.f;

	/** Emissive strength at the instant of the cast. Above 1 to clear the bloom threshold. */
	constexpr float PeakGlow = 6.f;

	/**
	 * The ring is at full brightness for the first fraction of its life and fades over the rest.
	 * A ring that starts fading immediately reads as a mistake rather than as a shockwave.
	 */
	constexpr float HoldFraction = 0.35f;
}

ATraceMortimerQuakeWave::ATraceMortimerQuakeWave()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	bReplicates = true;
	SetReplicateMovement(false);   // it never moves; the RING grows, the ACTOR does not
	bAlwaysRelevant = true;        // the enemy being launched must not have it culled away
	SetNetUpdateFrequency(10.f);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	RingMesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("RingMesh"));
	RingMesh->SetupAttachment(Root);
	// NO COLLISION ON ANY CHANNEL. This is the line that makes "cosmetic only" true rather than
	// intended: a shockwave that blocked a bullet or a line-of-sight trace would change the ability's
	// own HasLineOfSightTo() answer for every Quake after the first.
	RingMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RingMesh->SetCollisionProfileName(TEXT("NoCollision"));
	RingMesh->SetGenerateOverlapEvents(false);
	RingMesh->SetCanEverAffectNavigation(false);
	RingMesh->SetCastShadow(false);
	RingMesh->bReceivesDecals = false;

	// Both lookups are static, so the cost is paid once per process — the same shape ATraceCore and
	// ATraceRippleActor use. /Engine/BasicShapes ships with every install; M_TraceNeon is generated
	// content (Scripts/generate_content.py) and may not be there.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		BeadMesh = CylinderFinder.Object;
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

void ATraceMortimerQuakeWave::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceMortimerQuakeWave, WaveRadiusUU);
	DOREPLIFETIME(ATraceMortimerQuakeWave, WaveSeconds);
}

void ATraceMortimerQuakeWave::InitialiseWave(float InRadiusUU, float InSeconds)
{
	WaveRadiusUU = FMath::Max(TraceMortimerQuakeWaveFile::StartRadiusUU + 1.f, InRadiusUU);
	WaveSeconds  = FMath::Clamp(InSeconds, 0.1f, 5.f);

	// AUTHORITY-SIDE LIFETIME, so the destruction replicates like any other. A client that computed
	// its own deadline would delete the actor on a clock the server does not share.
	SetLifeSpan(WaveSeconds + 0.25f);
}

void ATraceMortimerQuakeWave::BeginPlay()
{
	Super::BeginPlay();

	// On the server the radius is already set by InitialiseWave. On a client the replicated values may
	// not have landed, so this is idempotent and Tick calls it again — the same arrangement as
	// ATraceRippleActor::BuildRingsIfNeeded, and for the same reason.
	BuildIfNeeded();
	UpdateRing(0.f);
}

float ATraceMortimerQuakeWave::GetWaveAlpha() const
{
	return FMath::Clamp(Elapsed / FMath::Max(0.01f, WaveSeconds), 0.f, 1.f);
}

int32 ATraceMortimerQuakeWave::GetDrawnBeadCount() const
{
	// *** GetInstanceCount(), NOT a bool and NOT the number we MEANT to add. ***
	// ATraceElleGate::BuildRingsIfNeeded is the standing example on this project of an effect that
	// built 60 ring segments and registered none of them: every internal counter said 60 and the
	// screen was empty. This asks the component what it will actually draw.
	return (RingMesh != nullptr) ? RingMesh->GetInstanceCount() : 0;
}

void ATraceMortimerQuakeWave::BuildIfNeeded()
{
	if (bBuilt || BeadMesh == nullptr || WaveRadiusUU <= 0.f)
	{
		return;
	}

	// Shaders are not cooked for server targets, so a dedicated server builds nothing. It still ticks
	// and still destroys itself on schedule, so the clients' copies are unaffected.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bBuilt = true;
		return;
	}

	bBuilt = true;

	RingMesh->SetStaticMesh(BeadMesh);

	UMaterialInterface* Parent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get();
	if (Parent != nullptr)
	{
		RingMID = UMaterialInstanceDynamic::Create(Parent, this);
		if (RingMID != nullptr)
		{
			// "Color" is what M_TraceNeon and BasicShapeMaterial both call it; "Glow" is neon-only and
			// is a harmless no-op on the fallback, which is why the fallback also gets a matte
			// roughness — it will not bloom, and that is the cost of not having generated the content.
			const FLinearColor Tint = UTraceSettings::Get().MortimerQuakeWaveColor;
			RingMID->SetVectorParameterValue(TEXT("Color"), Tint);
			RingMID->SetVectorParameterValue(TEXT("BaseColor"), Tint);
			RingMID->SetScalarParameterValue(TEXT("Glow"), TraceMortimerQuakeWaveFile::PeakGlow);
			RingMID->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
			RingMesh->SetMaterial(0, RingMID);
		}
	}

	// The beads are added ONCE at the start radius and moved every frame afterwards. Adding and
	// removing 48 instances per frame would be the obvious way to write this and would rebuild the
	// component's render state 60 times a second for no reason.
	for (int32 Bead = 0; Bead < TraceMortimerQuakeWaveFile::BeadCount; ++Bead)
	{
		RingMesh->AddInstance(FTransform::Identity);
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Mortimer] Quake shockwave built: %d beads, radius %.0f uu over %.2fs, material %s."),
		RingMesh->GetInstanceCount(), WaveRadiusUU, WaveSeconds,
		(NeonMaterial != nullptr) ? TEXT("M_TraceNeon") : TEXT("BasicShapeMaterial (generated content missing)"));
}

void ATraceMortimerQuakeWave::UpdateRing(float Alpha)
{
	if (RingMesh == nullptr || RingMesh->GetInstanceCount() <= 0)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Thickness = FMath::Clamp(Settings.MortimerQuakeWaveThicknessUU, 2.f, 80.f);

	// EASE OUT. A shockwave leaves fast and settles; a linear ring reads as a growing circle, which
	// is a UI element rather than a blast. 1 - (1 - a)^2 is the cheapest curve with that shape.
	const float Eased = 1.f - FMath::Square(1.f - Alpha);
	const float Radius = FMath::Lerp(TraceMortimerQuakeWaveFile::StartRadiusUU, WaveRadiusUU, Eased);

	// Each bead is a cylinder lying along the ring's tangent, overlapped 1.2x so the seams close.
	const float ArcLength = (2.f * PI * Radius / static_cast<float>(TraceMortimerQuakeWaveFile::BeadCount)) * 1.2f;
	const FVector BeadScale(Thickness / 100.f, Thickness / 100.f, ArcLength / 100.f);

	for (int32 Bead = 0; Bead < RingMesh->GetInstanceCount(); ++Bead)
	{
		const float Angle = (2.f * PI * static_cast<float>(Bead)) / static_cast<float>(TraceMortimerQuakeWaveFile::BeadCount);
		const float SinA = FMath::Sin(Angle);
		const float CosA = FMath::Cos(Angle);

		const FVector Offset(CosA * Radius, SinA * Radius, 0.f);
		const FVector Tangent(-SinA, CosA, 0.f);

		const FTransform BeadTransform(FRotationMatrix::MakeFromZ(Tangent).Rotator(), Offset, BeadScale);

		// The render state is marked dirty ONCE, on the last bead, rather than 48 times.
		RingMesh->UpdateInstanceTransform(Bead, BeadTransform, /*bWorldSpace*/ false,
			/*bMarkRenderStateDirty*/ Bead == RingMesh->GetInstanceCount() - 1, /*bTeleport*/ true);
	}

	if (RingMID != nullptr)
	{
		// FADE IN RGB, NOT IN ALPHA. Both candidate materials are OPAQUE, so an alpha of zero is a
		// fully visible ring — the same trap AHUD::DrawLine has and the same rule the house style
		// states for it. The emissive strength and the colour are what actually dim.
		const float Fade = 1.f - FMath::Clamp(
			(Alpha - TraceMortimerQuakeWaveFile::HoldFraction) / (1.f - TraceMortimerQuakeWaveFile::HoldFraction),
			0.f, 1.f);
		RingMID->SetScalarParameterValue(TEXT("Glow"), TraceMortimerQuakeWaveFile::PeakGlow * Fade);
		RingMID->SetVectorParameterValue(TEXT("Color"), Settings.MortimerQuakeWaveColor * Fade);
		RingMID->SetVectorParameterValue(TEXT("BaseColor"), Settings.MortimerQuakeWaveColor * Fade);
	}
}

void ATraceMortimerQuakeWave::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	BuildIfNeeded();

	Elapsed += DeltaSeconds;
	UpdateRing(GetWaveAlpha());
}

// =================================================================================================
// PASSIVE — the two halves. Both are pure reads of UTraceSettings, so they retune live.
// =================================================================================================

float UTraceAbilitySetMortimer::GetDashDistanceScale() const
{
	// Clamped rather than trusted: a zero here would be a dash that does not move him at all, which
	// is not "75% shorter", it is a broken movement kit with no error message.
	return FMath::Clamp(UTraceSettings::Get().MortimerDashDistanceScale, 0.05f, 4.f);
}

float UTraceAbilitySetMortimer::GetDashCooldownScale() const
{
	// Floored at 0.25 rather than at 1: the knob is a general per-character scale and a future
	// character could legitimately want a SHORTER cooldown, so this clamps the arithmetic instead of
	// enforcing a design opinion. Demo 20 asks for 1.25.
	return FMath::Clamp(UTraceSettings::Get().MortimerDashCooldownScale, 0.25f, 4.f);
}

float UTraceAbilitySetMortimer::GetThrowChargeHoldScale() const
{
	// Floored at 1: a value below 1 would make Mortimer's throw WEAKER than everybody's, which is the
	// opposite of the passive and would look like the sign of the knob had been flipped.
	return FMath::Clamp(UTraceSettings::Get().MortimerThrowChargeHoldScale, 1.f, 8.f);
}

bool UTraceAbilitySetMortimer::AllowsMantle() const
{
	return UTraceSettings::Get().bMortimerCanMantle;
}

float UTraceAbilitySetMortimer::GetMantleGenerosityScale() const
{
	return FMath::Clamp(UTraceSettings::Get().MortimerMantleGenerosity, 1.f, 4.f);
}

// =================================================================================================
// ACTIVATED — QUAKE
// =================================================================================================

float UTraceAbilitySetMortimer::GetActivatedCooldownSeconds() const
{
	return FMath::Max(0.f, UTraceSettings::Get().MortimerBlastCooldownSeconds);
}

ETraceMortimerBlastRefusal UTraceAbilitySetMortimer::CheckBlastPosture() const
{
	const ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* MoveComp = GetMovement();

	if (MyPawn == nullptr || !MyPawn->IsAlive() || MoveComp == nullptr)
	{
		return ETraceMortimerBlastRefusal::NoPawn;
	}

	if (CVarMortimerBlastPosture.GetValueOnAnyThread() == 0)
	{
		return ETraceMortimerBlastRefusal::Allowed;   // RED ARM: the gate is removed, nothing else is.
	}

	// "only while carrying the core". THE SAME PREDICATE THE CHOKE POINT USES, not a second one:
	// UTraceAbilityComponent::IsCarrier ORs the pawn's replicated mirror with ATraceCore's own
	// holder pointer, which is what makes this answer identical on the server and on the client that
	// predicted the press.
	if (!UTraceAbilityComponent::IsCarrier(MyPawn))
	{
		return ETraceMortimerBlastRefusal::NotCarryingCore;
	}

	// "AND standing on the ground or the top of an object".
	//
	// IsGroundedForAbilities(), not IsMovingOnGround(): it is the ABILITY LAYER's definition of
	// grounded and it carries LedgeGroundGraceSeconds of hysteresis, which exists precisely because a
	// one-frame contact blip on a ledge LIP reads as airborne on one machine and grounded on the
	// other. Since "the top of an object" is exactly where a player stands when they are on a lip,
	// using the raw engine answer here would refuse the ability in the one place §3 names.
	//
	// There is deliberately no separate test for "the top of an object" — the floor of the arena and
	// the roof of a crate are the same walkable surface to the movement component, and inventing a
	// second test would be inventing a rule the doc does not have.
	if (!MoveComp->IsGroundedForAbilities())
	{
		return ETraceMortimerBlastRefusal::Airborne;
	}

	return ETraceMortimerBlastRefusal::Allowed;
}

bool UTraceAbilitySetMortimer::CanActivate(FText& OutReason) const
{
	switch (CheckBlastPosture())
	{
	case ETraceMortimerBlastRefusal::Allowed:
		return true;

	case ETraceMortimerBlastRefusal::NotCarryingCore:
		// Phrased as an instruction rather than as a state, because it is one a player can act on —
		// see the UI note in the report: today this sentence exists only in the server log.
		OutReason = LOCTEXT("MortimerNoCore", "QUAKE NEEDS THE CORE");
		return false;

	case ETraceMortimerBlastRefusal::Airborne:
		OutReason = LOCTEXT("MortimerAirborne", "QUAKE NEEDS SOLID GROUND");
		return false;

	case ETraceMortimerBlastRefusal::NoPawn:
	default:
		OutReason = LOCTEXT("MortimerNoPawn", "NOT READY");
		return false;
	}
}

bool UTraceAbilitySetMortimer::ActivateAbility()
{
	if (CheckBlastPosture() != ETraceMortimerBlastRefusal::Allowed)
	{
		// FALSE, so the framework charges no cooldown. §3 makes Quake conditional, and a conditional
		// ability that eats its 20 s on a refused press is a trap rather than a condition.
		return false;
	}

	if (!HasAuthority())
	{
		// THE OWNING CLIENT PREDICTS NOTHING BUT THE PRESS. Every effect Quake has is somebody else's
		// POSITION, which is replicated from the server; a client that launched them locally would be
		// showing itself enemies flying who, a round trip later, never moved. Returning true is what
		// starts the local cooldown ring, which is the only thing there is to predict.
		++BlastCount;
		return true;
	}

	int32 Considered = 0;
	const int32 Knocked = RunBlast(Considered);
	++BlastCount;

	// *** DEMO 20 ITEM 3. THE CAST IS NOW VISIBLE WHETHER OR NOT IT HITS ANYBODY. ***
	//
	// Deliberately AFTER RunBlast and deliberately NOT conditional on Knocked. "It did nothing" was
	// the report, and the single most common way to press this ability is with nobody inside 600 uu —
	// so an effect that only appeared when somebody was launched would leave the exact case that
	// produced the complaint looking exactly as broken as before.
	//
	// Also deliberately at his FEET rather than at the capsule's centre: the blast is a ground slam
	// and a ring floating at chest height reads as a bubble.
	ATraceCharacter* CasterPawn = GetCharacter();
	const ATraceMortimerQuakeWave* Wave = nullptr;
	if (CasterPawn != nullptr)
	{
		const FVector Feet = CasterPawn->GetActorLocation()
			- FVector(0.f, 0.f, CasterPawn->GetSimpleCollisionHalfHeight());
		Wave = SpawnQuakeWave(Feet);
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Mortimer] QUAKE #%d fired: %d of %d pawn(s) inside %.0f uu were knocked away. "
		     "(The rest were team-mates, out of sight, or refused by the spec §4 choke point.) "
		     "Shockwave: %s."),
		BlastCount, Knocked, Considered, FMath::Max(1.f, UTraceSettings::Get().MortimerBlastRadiusUU),
		(Wave != nullptr)
			? *FString::Printf(TEXT("spawned, %d beads, radius %.0f uu"), Wave->GetDrawnBeadCount(), Wave->GetWaveRadiusUU())
			: TEXT("NOT spawned (Trace.Mortimer.QuakeWave 0, no world, or a dedicated server)"));

	// TRUE EVEN WHEN NOBODY WAS HIT, and that is a decision. §3 makes the ability conditional on
	// MORTIMER'S OWN POSTURE and not on there being a victim, so a Quake in an empty room is a Quake
	// he chose to spend — the same call Rocco's Ripple makes and the opposite of Mace's spike, which
	// fizzles free because it can miss the WORLD.
	return true;
}

int32 UTraceAbilitySetMortimer::RunBlast(int32& OutConsidered)
{
	OutConsidered = 0;

	if (!HasAuthority())
	{
		return 0;
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UWorld* BlastWorld = (MyPawn != nullptr) ? MyPawn->GetWorld() : nullptr;
	if (MyPawn == nullptr || BlastWorld == nullptr)
	{
		return 0;
	}

	const float Radius = FMath::Max(1.f, UTraceSettings::Get().MortimerBlastRadiusUU);
	const float RadiusSquared = Radius * Radius;
	const FVector Origin = MyPawn->GetActorLocation();

	// A LIST FIRST, THEN THE LAUNCHES. LaunchCharacter writes Velocity, which cannot move an actor out
	// from under a TActorIterator — but it CAN run a listener that destroys one, and iterating a level
	// while it mutates is the kind of thing that works for a year and then does not.
	TArray<ATraceCharacter*, TInlineAllocator<16>> Candidates;
	for (TActorIterator<ATraceCharacter> It(BlastWorld); It; ++It)
	{
		ATraceCharacter* Other = *It;
		if (Other == nullptr || Other == MyPawn || !IsValid(Other) || !Other->IsAlive())
		{
			continue;
		}
		if (FVector::DistSquared(Other->GetActorLocation(), Origin) > RadiusSquared)
		{
			continue;
		}
		Candidates.Add(Other);
	}

	OutConsidered = Candidates.Num();

	int32 Knocked = 0;
	for (ATraceCharacter* Victim : Candidates)
	{
		if (ApplyBlastTo(Victim))
		{
			++Knocked;
		}
	}

	return Knocked;
}

bool UTraceAbilitySetMortimer::ApplyBlastTo(ATraceCharacter* Victim) const
{
	if (!HasAuthority())
	{
		return false;   // A knockback is server truth. See ActivateAbility's prediction note.
	}

	ATraceCharacter* MyPawn = GetCharacter();
	if (MyPawn == nullptr || Victim == nullptr || Victim == MyPawn || !IsValid(Victim) || !Victim->IsAlive())
	{
		return false;
	}

	// =============================================================================================
	// *** THE CHOKE POINT. SPEC v14 §4, AND THE WHOLE REASON THIS FUNCTION IS SEPARATE. ***
	//
	// Control, because a knockback is movement the target did not ask for. This ONE call is what
	// makes "it cannot displace a Core carrier" true; there is no carrier test anywhere in this file
	// and there must not be one, because a second copy of the rule is a second thing that can rot.
	//
	// It also answers the team question, the dead question and the characters-disabled question, so
	// there is no friendly-fire test in this file either.
	//
	// READ THE HEADER'S LAST PARAGRAPH BEFORE DELETING THIS AS DEAD CODE. With one Core in play an
	// enemy carrier cannot exist while Mortimer is carrying, so in a real match this can never fire —
	// which is exactly why it is here and why the harness reaches this function directly.
	// =============================================================================================
	if (CVarMortimerBlastChoke.GetValueOnAnyThread() != 0
		&& !CanAffect(Victim, ETraceAbilityEffect::Control))
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Mortimer] Quake on %s refused by the choke point (carrier=%d)."),
			*GetNameSafe(Victim), UTraceAbilityComponent::IsCarrier(Victim) ? 1 : 0);
		return false;
	}

	if (!HasLineOfSightTo(Victim))
	{
		return false;
	}

	LaunchVictim(Victim, MyPawn->GetActorLocation());
	return true;
}

ATraceMortimerQuakeWave* UTraceAbilitySetMortimer::SpawnQuakeWave(const FVector& Origin)
{
	if (!HasAuthority())
	{
		// The cosmetic is server-spawned and replicated for the same reason the knockback is
		// server-only: see ActivateAbility's prediction note. A client that spawned its own would show
		// the caster a ring nobody else could see, which is a worse lie than showing nothing.
		return nullptr;
	}

	if (CVarMortimerQuakeWave.GetValueOnAnyThread() == 0)
	{
		return nullptr;   // RED ARM: no cosmetic, identical knockback. See the CVar.
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UWorld* WaveWorld = (MyPawn != nullptr) ? MyPawn->GetWorld() : nullptr;
	if (WaveWorld == nullptr)
	{
		return nullptr;
	}

	const FVector Lifted = Origin + FVector(0.f, 0.f, TraceMortimerQuakeWaveFile::GroundLiftUU);
	const FTransform SpawnAt(FRotator::ZeroRotator, Lifted);

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Instigator = MyPawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	// DEFERRED, unlike ATraceRippleActor's plain SpawnActor, and the difference is worth a sentence:
	// SpawnActor runs BeginPlay before the caller can set anything, so the ripple has to build its
	// rings from Tick and cannot report how many it built. Deferring lets InitialiseWave() land the
	// radius FIRST, so BeginPlay builds the beads and this function can return an actor whose
	// GetDrawnBeadCount() is already the truth — which is what the harness and the log above assert on.
	ATraceMortimerQuakeWave* Wave = WaveWorld->SpawnActorDeferred<ATraceMortimerQuakeWave>(
		ATraceMortimerQuakeWave::StaticClass(), SpawnAt, /*Owner*/ nullptr, MyPawn,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Wave == nullptr)
	{
		return nullptr;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	Wave->InitialiseWave(FMath::Max(1.f, Settings.MortimerBlastRadiusUU),
	                     FMath::Max(0.1f, Settings.MortimerQuakeWaveSeconds));
	Wave->FinishSpawning(SpawnAt);

	++WaveCount;
	return Wave;
}

void UTraceAbilitySetMortimer::LaunchVictim(ATraceCharacter* Victim, const FVector& FromLocation) const
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	// AWAY FROM HIM, PLANAR, PLUS A POP. Planar because the victim's own height above or below
	// Mortimer must not tilt the shove into the floor or the sky; the deliberate vertical component
	// is MortimerBlastUpBias and only that.
	FVector Away = Victim->GetActorLocation() - FromLocation;
	Away.Z = 0.f;
	if (!Away.Normalize())
	{
		// Standing in exactly the same column — a corner case, not an impossible one. Throwing them
		// along Mortimer's facing is the only direction that means anything here; never a random one.
		const ATraceCharacter* MyPawn = GetCharacter();
		Away = (MyPawn != nullptr) ? MyPawn->GetActorForwardVector() : FVector::ForwardVector;
		Away.Z = 0.f;
		if (!Away.Normalize())
		{
			Away = FVector::ForwardVector;
		}
	}

	float Scale = 1.f;
	if (Settings.bMortimerBlastFallsOff)
	{
		const float Radius = FMath::Max(1.f, Settings.MortimerBlastRadiusUU);
		const float Distance = FMath::Clamp(
			static_cast<float>(FVector::Dist(Victim->GetActorLocation(), FromLocation)), 0.f, Radius);
		const float RimScale = FMath::Clamp(Settings.MortimerBlastMinFalloffScale, 0.f, 1.f);
		Scale = FMath::Lerp(1.f, RimScale, Distance / Radius);
	}

	const float Speed = FMath::Max(0.f, Settings.MortimerBlastKnockbackSpeed) * Scale;
	const FVector Impulse = Away * Speed + FVector::UpVector * (Speed * FMath::Max(0.f, Settings.MortimerBlastUpBias));

	// bXYOverride / bZOverride both true: Quake REPLACES the victim's velocity rather than adding to
	// it, so a player sprinting at Mortimer is thrown exactly as far as one standing still. Identical
	// to Chut's bash, and for the identical reason — a knockback whose strength depended on the
	// victim's own speed would be strongest against the people best placed to punish it.
	Victim->LaunchCharacter(Impulse, true, true);
}

bool UTraceAbilitySetMortimer::HasLineOfSightTo(const ATraceCharacter* Victim) const
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	if (!Settings.bMortimerBlastNeedsLineOfSight)
	{
		return true;
	}

	const ATraceCharacter* MyPawn = GetCharacter();
	UWorld* SightWorld = (MyPawn != nullptr) ? MyPawn->GetWorld() : nullptr;
	if (MyPawn == nullptr || Victim == nullptr || SightWorld == nullptr)
	{
		return false;
	}

	FCollisionQueryParams SightParams(SCENE_QUERY_STAT(TraceMortimerQuakeSight), /*bTraceComplex*/ false);
	// Both pawns are ignored, and both for the same reason the mantle's probes learned the hard way:
	// a trace that starts inside a collider reports that collider at distance zero, so without these
	// the blast would decide it could not see anybody, ever.
	SightParams.AddIgnoredActor(MyPawn);
	SightParams.AddIgnoredActor(Victim);

	FHitResult SightHit;
	const bool bBlocked = SightWorld->LineTraceSingleByChannel(
		SightHit, MyPawn->GetActorLocation(), Victim->GetActorLocation(), ECC_Visibility, SightParams);

	if (bBlocked)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Mortimer] Quake on %s blocked by %s."),
			*GetNameSafe(Victim), *GetNameSafe(SightHit.GetActor()));
	}

	return !bBlocked;
}

// =================================================================================================
// Trace.Mortimer.BlastCarrierTest — SPEC v14 §4, TWO ARMS, RED FIRST
//
// THE QUESTION: does Quake's per-victim path actually route through the carrier choke point?
//
// THE REASON IT IS NOT A LIVE-MATCH TEST: there is one Core, so while Mortimer holds it no enemy can
// be a carrier, and a harness that waited for that situation would wait forever and then report
// green for having measured nothing. So this drives ApplyBlastTo() directly — the same function the
// real ability calls once per victim — at a pawn the harness has deliberately handed the Core to.
//
// THE FIXTURE PROVES ITSELF. Every arm also blasts a CONTROL target who is a living enemy and NOT a
// carrier. If the control does not fly in both arms then the harness has measured nothing about the
// carrier rule and everything about a broken fixture, and it says INVALID rather than PASS. This is
// the failure mode Trace.Ability.CarrierChokeTest was caught in (1 run in 3 reported a vacuous
// green), and the guard is copied from its fix.
//
// SYNCHRONOUS, no ticker: ATraceCore::TryPickup is an authority-side debug grant that lands the same
// frame, and LaunchCharacter writes Velocity the same frame, so every reading below is taken from
// the same call stack that caused it.
// =================================================================================================

// NAMED after the file rather than anonymous — UBT builds this module as a unity/jumbo blob, so two
// files that each open `namespace { }` become one namespace holding both, and "FindEnemy" is exactly
// the kind of name another character file would also want. See Scripts/check-jumbo-build-collisions.py.
namespace TraceMortimerVerifyFile
{
	/** How much velocity change counts as "was displaced", in uu/s. Well below any real launch. */
	constexpr float DisplacedThreshold = 50.f;

	/**
	 * Makes the first HUMAN player Mortimer and returns their set.
	 *
	 * A human, and it must be: handing a BOT a character of the harness's choosing fights
	 * ATraceGameMode::PollCharacterSelect's 4 Hz fill for that player state and measures whichever
	 * won. Same reasoning and the same wording as Slimeball's MakePlayerIntoSlimeball.
	 */
	UTraceAbilitySetMortimer* MakePlayerIntoMortimer(UWorld* WorldPtr, FString& OutWhy)
	{
		if (WorldPtr == nullptr)
		{
			OutWhy = TEXT("no world");
			return nullptr;
		}

		if (!UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
		{
			OutWhy = TEXT("characters are DISABLED in this match (mode A, or the §3 toggle is off) — "
			              "run this in mode B with characters on");
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = WorldPtr->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = Cast<APlayerController>(It->Get());
			if (PC == nullptr || PC->GetPawn() == nullptr)
			{
				continue;
			}
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(PC->GetPawn());
			if (Comp == nullptr || Comp->IsBot())
			{
				continue;
			}

			if (Comp->GetCharacterId() != ETraceCharacterId::Mortimer)
			{
				Comp->ServerSetCharacter(ETraceCharacterId::Mortimer);
			}

			if (UTraceAbilitySetMortimer* Found = Comp->GetAbilitySetAs<UTraceAbilitySetMortimer>())
			{
				return Found;
			}

			OutWhy = FString::Printf(
				TEXT("ServerSetCharacter(Mortimer) did not produce a UTraceAbilitySetMortimer (id is now %s) — "
				     "a team-mate may already hold him, or the reflection roster did not find the class"),
				TraceCharacterIdToString(Comp->GetCharacterId()));
			return nullptr;
		}

		OutWhy = TEXT("no human player controller with a pawn");
		return nullptr;
	}

	/** A living enemy of @p Mine, optionally skipping one already spoken for. */
	ATraceCharacter* FindLivingEnemy(UWorld* WorldPtr, const ATraceCharacter* Mine, const ATraceCharacter* Skip)
	{
		if (WorldPtr == nullptr || Mine == nullptr)
		{
			return nullptr;
		}
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Mine || Candidate == Skip || !Candidate->IsAlive())
			{
				continue;
			}
			if (Candidate->GetTeam() == Mine->GetTeam() || Candidate->GetTeam() == ETraceTeam::None)
			{
				continue;
			}
			return Candidate;
		}
		return nullptr;
	}

	/**
	 * Stand @p Target beside Mortimer, blast them, and answer how hard they were thrown, in uu/s.
	 *
	 * *** IT READS PendingLaunchVelocity, NOT Velocity, AND THAT IS THE WHOLE FIX. *** The first
	 * version of this harness read GetVelocity() before and after and measured 0.0 uu/s on EVERY arm,
	 * including the red one — a harness that could not go red. The cause is that
	 * ACharacter::LaunchCharacter does not write Velocity: UCharacterMovementComponent::Launch parks
	 * the impulse in PendingLaunchVelocity and HandlePendingLaunch spends it during the NEXT movement
	 * tick, so a same-frame reading of Velocity can only ever be zero. PendingLaunchVelocity is the
	 * value the ability actually produced, on the frame it produced it, and it is the honest same-call-
	 * stack observable. (Chut's bash has the identical shape; it looks instant in a match only because
	 * the world goes on to tick.)
	 *
	 * THE TARGET IS MOVED NEXT TO HIM FIRST, and that is not a convenience either: Quake requires line
	 * of sight, so blasting a bot standing across a 3.5:1 arena would be refused by geometry and the
	 * harness would report INVALID for a reason that has nothing to do with the rule under test.
	 * Standing them 150 uu away is what a real Quake looks like.
	 *
	 * @param OutApplied  what ApplyBlastTo itself returned, so "refused" and "launched with 0" are
	 *                    distinguishable in the log rather than both reading as a zero.
	 */
	float MeasureBlastOn(const UTraceAbilitySetMortimer* Mortimer, ATraceCharacter* Target,
	                     const FVector& Offset, bool& OutApplied)
	{
		OutApplied = false;

		const ATraceCharacter* MyPawn = (Mortimer != nullptr) ? Mortimer->GetCharacter() : nullptr;
		if (MyPawn == nullptr || Target == nullptr)
		{
			return -1.f;
		}

		Target->SetActorLocation(MyPawn->GetActorLocation() + Offset, /*bSweep*/ false, nullptr,
			ETeleportType::TeleportPhysics);

		UCharacterMovementComponent* TargetMove = Target->GetCharacterMovement();
		if (TargetMove == nullptr)
		{
			return -1.f;
		}

		TargetMove->PendingLaunchVelocity = FVector::ZeroVector;
		OutApplied = const_cast<UTraceAbilitySetMortimer*>(Mortimer)->ApplyBlastTo(Target);
		return static_cast<float>(TargetMove->PendingLaunchVelocity.Size());
	}

	void RunBlastCarrierTest()
	{
		const TCHAR* const Tag = TEXT("MORTIMERCHOKE");

		UWorld* TestWorld = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->IsGameWorld()
					&& Context.World()->GetAuthGameMode() != nullptr)
				{
					TestWorld = Context.World();
					break;
				}
			}
		}

		if (TestWorld == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. Run this on the server/host, in a live match."),
				Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetMortimer* Mortimer = MakePlayerIntoMortimer(TestWorld, Why);
		if (Mortimer == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — %s."), Tag, *Why);
			return;
		}

		ATraceCharacter* MyPawn = Mortimer->GetCharacter();
		ATraceCore* CoreActor = ATraceCore::Get(TestWorld);
		if (MyPawn == nullptr || CoreActor == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — Mortimer has no pawn, or there is no Core in this world."), Tag);
			return;
		}

		ATraceCharacter* Victim = FindLivingEnemy(TestWorld, MyPawn, nullptr);
		ATraceCharacter* Control = FindLivingEnemy(TestWorld, MyPawn, Victim);
		if (Victim == nullptr || Control == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — needs TWO living enemies (one to make a carrier, one control). "
				     "Found victim=%s control=%s. Run this early, in a populated match."),
				Tag, *GetNameSafe(Victim), *GetNameSafe(Control));
			return;
		}

		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.BlastChoke"));
		if (Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — Trace.Mortimer.BlastChoke is not registered."), Tag);
			return;
		}
		const int32 ArmBefore = Arm->GetInt();

		float RedCarrier = -1.f;
		float RedControl = -1.f;
		float GreenCarrier = -1.f;
		float GreenControl = -1.f;
		bool bRedVictimWasCarrier = false;
		bool bGreenVictimWasCarrier = false;

		for (int32 ArmIndex = 0; ArmIndex < 2; ++ArmIndex)
		{
			const bool bGreen = (ArmIndex == 1);
			Arm->Set(bGreen ? 1 : 0, ECVF_SetByConsole);

			// Hand the Core to the victim. TryPickup is the authority-side debug grant and it takes
			// the Core off whoever had it, so this is also what guarantees MORTIMER is not holding it
			// — which is the situation a real match cannot produce and the whole point of the test.
			CoreActor->TryPickup(Victim);

			const bool bIsCarrierNow = UTraceAbilityComponent::IsCarrier(Victim);

			// Opposite sides of him, so neither can be standing in the other's line.
			bool bCarrierApplied = false;
			bool bControlApplied = false;
			const float CarrierMoved = MeasureBlastOn(Mortimer, Victim,  FVector(150.f, 0.f, 0.f), bCarrierApplied);
			const float ControlMoved = MeasureBlastOn(Mortimer, Control, FVector(-150.f, 0.f, 0.f), bControlApplied);

			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%s  victim=%s carrier=%d launched=%d at %.1f uu/s | control=%s launched=%d at %.1f uu/s"),
				Tag, bGreen ? TEXT("GREEN (shipped)") : TEXT("RED (choke removed)"),
				*GetNameSafe(Victim), bIsCarrierNow ? 1 : 0, bCarrierApplied ? 1 : 0, CarrierMoved,
				*GetNameSafe(Control), bControlApplied ? 1 : 0, ControlMoved);

			if (bGreen)
			{
				GreenCarrier = CarrierMoved;
				GreenControl = ControlMoved;
				bGreenVictimWasCarrier = bIsCarrierNow;
			}
			else
			{
				RedCarrier = CarrierMoved;
				RedControl = ControlMoved;
				bRedVictimWasCarrier = bIsCarrierNow;
			}
		}

		Arm->Set(ArmBefore, ECVF_SetByConsole);

		// ---- the verdict --------------------------------------------------------------------------
		//
		// THE FIXTURE HAS TO PROVE ITSELF FIRST. Three things must be true before "the carrier did not
		// move" is allowed to mean anything: the victim really was a carrier in BOTH arms, the control
		// really did fly in BOTH arms (so the blast works at all and the difference is the rule, not
		// the fixture), and the RED arm really did reach the carrier (so the rule is what stops it).
		const bool bControlWorked = RedControl > DisplacedThreshold && GreenControl > DisplacedThreshold;
		const bool bRedReproduced = RedCarrier > DisplacedThreshold;
		const bool bCarrierHeld   = bRedVictimWasCarrier && bGreenVictimWasCarrier;

		if (!bCarrierHeld || !bControlWorked || !bRedReproduced)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — victimWasCarrierBothArms=%d controlFlewBothArms=%d "
				     "redArmReachedTheCarrier=%d (red carrier %.1f, red control %.1f, green control %.1f uu/s). "
				     "A harness that cannot go red has proved nothing; this reports INVALID rather than PASS."),
				Tag, bCarrierHeld ? 1 : 0, bControlWorked ? 1 : 0, bRedReproduced ? 1 : 0,
				RedCarrier, RedControl, GreenControl);
			return;
		}

		if (GreenCarrier > DisplacedThreshold)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] VERDICT: *** FAIL *** — the shipped build DISPLACED a Core carrier by %.1f uu/s. "
				     "Quake is not routed through UTraceAbilityComponent::CanAffectTarget(Control). This is the "
				     "founding invariant."),
				Tag, GreenCarrier);
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] VERDICT: PASS — with the choke point REMOVED the carrier flew %.1f uu/s; with it in "
			     "place the identical call moved them %.1f uu/s. The control target flew %.1f then %.1f uu/s, "
			     "so the blast itself worked in both arms and the difference is the rule."),
			Tag, RedCarrier, GreenCarrier, RedControl, GreenControl);
	}

	FAutoConsoleCommand CmdBlastCarrierTest(
		TEXT("Trace.Mortimer.BlastCarrierTest"),
		TEXT("SPEC v19 §3 / v14 §4. Two arms, RED FIRST: hands the Core to an enemy and fires Quake's per-victim "
		     "path at them with the carrier choke point removed (must throw them) and then in place (must not). "
		     "A second, non-carrying enemy is blasted in both arms so the fixture proves itself."),
		FConsoleCommandDelegate::CreateStatic(&RunBlastCarrierTest));

	/**
	 * Trace.Mortimer.Verify — the parts of §3 that are answerable without a fixture.
	 *
	 * Deliberately SEPARATE from the choke test: this one is arithmetic and posture and can run in an
	 * empty match, while the choke test needs two live enemies and a Core. Running them together
	 * would mean the cheap check reported INVALID whenever the expensive one could not be staged.
	 */
	void RunMortimerVerify()
	{
		const TCHAR* const Tag = TEXT("MORTIMER");
		const UTraceSettings& Settings = UTraceSettings::Get();

		int32 Passed = 0;
		int32 Failed = 0;
		auto Check = [&](bool bCondition, const FString& What)
		{
			if (bCondition) { ++Passed; }
			else { ++Failed; UE_LOG(LogTraceGame, Error, TEXT("[%s] FAIL: %s"), Tag, *What); }
		};

		// --- the passive, as numbers -------------------------------------------------------------
		//
		// DEMO 20 ITEM 2 MOVED THIS TARGET. It was 0.25 ("his dash is 75% shorter", spec v19 §3) and
		// the owner has revised it to "40% of a normal one instead of 25%". If a future demo moves it
		// again, this literal and the two places the number is written (TraceSettings.h and
		// Config/DefaultGame.ini, which beats it) move together or this check is the one that says so.
		Check(FMath::IsNearlyEqual(Settings.MortimerDashDistanceScale, 0.40f, 0.001f),
			FString::Printf(TEXT("Demo 20 item 2 says the dash is 40%% of a normal one, i.e. a scale of 0.40; "
			                     "the knob is %.3f. Remember Config/DefaultGame.ini beats TraceSettings.h — "
			                     "check BOTH before believing the header."),
				Settings.MortimerDashDistanceScale));

		Check(FMath::IsNearlyEqual(Settings.MortimerDashCooldownScale, 1.25f, 0.001f),
			FString::Printf(TEXT("Demo 20 item 2 says his dash cooldown is 25%% longer, i.e. a scale of 1.25; "
			                     "the knob is %.3f"),
				Settings.MortimerDashCooldownScale));

		Check(FMath::IsNearlyEqual(Settings.MortimerThrowChargeHoldScale, 2.f, 0.001f),
			FString::Printf(TEXT("§3 says he may charge for 2x as long; the knob is %.3f"),
				Settings.MortimerThrowChargeHoldScale));

		Check(FMath::IsNearlyEqual(Settings.MortimerMantleGenerosity, 1.3f, 0.001f),
			FString::Printf(TEXT("§3 says the mantle is 30%% more generous, i.e. 1.30; the knob is %.3f"),
				Settings.MortimerMantleGenerosity));

		// --- and the derived numbers a designer actually reads ------------------------------------
		const float DashReach = FMath::Max(0.f, Settings.DashSpeed) * FMath::Max(0.f, Settings.DashDuration);
		const float HisReach  = DashReach * Settings.MortimerDashDistanceScale;
		const float FullHold  = FMath::Max(0.01f, Settings.CoreThrowChargeSeconds);
		const float HisHold   = FullHold * Settings.MortimerThrowChargeHoldScale;
		const float Floor     = FMath::Clamp(Settings.CoreThrowChargeFloorFraction, 0.f, 1.f);
		const float HisPower  = Floor + (1.f - Floor) * Settings.MortimerThrowChargeHoldScale;

		const float HisCooldown = FMath::Max(0.f, Settings.DashCooldown) * Settings.MortimerDashCooldownScale;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] DASH   everybody %.0f uu (%.0f uu/s x %.2fs) -> Mortimer %.0f uu. "
			     "COOLDOWN everybody %.2fs -> Mortimer %.2fs (x%.2f) ON PAPER — see the NOT LIVE line below."),
			Tag, DashReach, Settings.DashSpeed, Settings.DashDuration, HisReach,
			Settings.DashCooldown, HisCooldown, Settings.MortimerDashCooldownScale);
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] THROW  everybody may hold %.2fs for x1.00 launch speed -> Mortimer may hold %.2fs for "
			     "x%.2f, ON THE SAME LINE (Power = %.2f + %.2f x t). NOTE: x%.2f SPEED is not x2 DISTANCE — "
			     "flat-ground range goes as speed squared, so this is nearer 3.4x the range. Flagged."),
			Tag, FullHold, HisHold, HisPower, Floor, 1.f - Floor, HisPower);
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] QUAKE  radius %.0f uu, %.0f uu/s at the centre falling to %.0f%% at the rim, up bias %.2f, "
			     "line of sight %s, cooldown %.0fs."),
			Tag, Settings.MortimerBlastRadiusUU, Settings.MortimerBlastKnockbackSpeed,
			Settings.bMortimerBlastFallsOff ? Settings.MortimerBlastMinFalloffScale * 100.f : 100.f,
			Settings.MortimerBlastUpBias, Settings.bMortimerBlastNeedsLineOfSight ? TEXT("REQUIRED") : TEXT("not required"),
			Settings.MortimerBlastCooldownSeconds);

		// --- THE HONEST PART, AND IT WAS DISHONEST UNTIL DEMO 20. ----------------------------------
		//
		// *** THIS BLOCK USED TO SHIP A FALSE WARNING. *** It said "NOT YET LIVE: the dash scale, the
		// mantle and the Core-throw cap ... are read by NOTHING today". Two thirds of that sentence
		// had been untrue since the pass that WROTE it: UTraceCharacterMovementComponent::GetDashSpeed
		// already multiplied by TraceAbilityTraits::GetDashDistanceScale, and ATraceCore::
		// GetThrowChargeScaleForHold already multiplied by GetThrowChargeHoldScale, at two call sites.
		// An integrator read this line, quoted it as fact, and re-reported a working passive as broken.
		//
		// So the rule this block now follows: EVERY CLAIM HERE IS A CALL SITE THAT EITHER EXISTS OR
		// DOES NOT, NAMED BY FILE AND FUNCTION, so the next reader can check it with one grep instead
		// of believing a sentence. If you wire one of the two remaining knobs, delete its line here in
		// the same edit — a stale "NOT LIVE" costs exactly as much as a stale "LIVE".
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] LIVE: the dash REACH (Movement/TraceCharacterMovementComponent.cpp, GetDashSpeed "
			     "multiplies by TraceAbilityTraits::GetDashDistanceScale) and the Core-throw CAP "
			     "(Gameplay/TraceCore.cpp, GetThrowChargeScaleForHold, two call sites). QUAKE IS LIVE and "
			     "as of Demo 20 it also draws a shockwave, so a cast with nobody in range is visible."),
			Tag);

		UE_LOG(LogTraceGame, Warning,
			TEXT("[%s] NOT LIVE (1/2): the dash COOLDOWN scale. UTraceCharacterMovementComponent::"
			     "GetDashCooldown() still returns UTraceSettings::DashCooldown flat and never consults "
			     "TraceAbilityTraits::GetDashCooldownScale(), so Mortimer's dash recharges in %.2fs like "
			     "everybody's and NOT in the %.2fs printed above. The whole fix is one multiplication in "
			     "that one function; it is written out verbatim in MortimerDashCooldownScale's comment in "
			     "TraceSettings.h. Trace.Mortimer.DashTest MEASURES this rather than trusting the knob."),
			Tag, Settings.DashCooldown, HisCooldown);

		UE_LOG(LogTraceGame, Warning,
			TEXT("[%s] NOT LIVE (2/2): the MANTLE, and the word is MISSING rather than unwired. There is no "
			     "CanAttemptMantle() to gate: IsMantling(), CanAttemptMantle(), TryBeginMantle(), "
			     "ApplyMantleVelocity() and EndMantle() were DELETED from the movement component in d2319b2 "
			     "with six pieces of saved-move state, eight tuning knobs and one CVar. bMortimerCanMantle "
			     "and MortimerMantleGenerosity are the gate on a feature with no body, and "
			     "TraceAbilityTraits::IsMantleAllowed() has zero callers. Restoring it is ~500 lines inside "
			     "Source/Trace/Movement/ (see `git show dffea7c`), not a one-liner — budget for it and do "
			     "not let this line be read as 'nearly done'."),
			Tag);

		if (Failed == 0)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[%s] VERDICT: PASS — %d checks, 0 failed."), Tag, Passed);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d passed, %d FAILED."), Tag, Passed, Failed);
		}
	}

	FAutoConsoleCommand CmdMortimerVerify(
		TEXT("Trace.Mortimer.Verify"),
		TEXT("SPEC v19 §3 + DEMO 20 item 2. Checks Mortimer's knobs against the doc's numbers, prints the derived "
		     "dash reach, cooldown and throw power, and names — by file and function — which call sites exist and "
		     "which do not. It does NOT measure the game; Trace.Mortimer.DashTest does that."),
		FConsoleCommandDelegate::CreateStatic(&RunMortimerVerify));

	// =============================================================================================
	// Trace.Mortimer.DashTest — DEMO 20 ITEM 2, MEASURED THROUGH THE SHIPPED MOVEMENT COMPONENT
	//
	// THE QUESTION Trace.Mortimer.Verify CANNOT ANSWER: a knob holding 0.40 proves nothing about
	// what the game does with it. This asks UTraceCharacterMovementComponent's OWN accessors — the
	// exact two functions BeginDash() and the recharge clock call — what Mortimer's dash speed and
	// dash cooldown are, on a live pawn.
	//
	// TWO ARMS PER NUMBER, AND THE ARMS ARE THE KNOB ITSELF. Each half sets the character knob to a
	// probe value, re-reads the shipped accessor, and restores it. If the accessor's answer MOVES,
	// it reads the knob. If it does not, nothing reads the knob and the harness says so.
	//
	// *** THIS HARNESS IS EXPECTED TO GO HALF-RED TODAY, AND THAT IS THE POINT. *** The reach arms
	// separate (594 uu vs 238 uu) and the cooldown arms do not (3.50 s vs 3.50 s), which is exactly
	// the difference between a wired knob and a dead one — measured on one instrument, in one run, so
	// "the harness cannot fail" is not available as an explanation for either result.
	//
	// It is also why the number this reports is a SPEED and not a scripted dash's travelled distance.
	// The last pass measured 139.3 uu for Mortimer against 813.5 uu for Rocco and read it as "still
	// 25%" — but 139.3/813.5 is 0.171, not 0.25, because a dash hands its exit speed back
	// (DashExitSpeedMultiplier 1.25) and the pawn keeps coasting after the dash window closes. The
	// coast is proportional to the dash speed, so the ratio of TOTAL travel is not the ratio of dash
	// reach. GetDashSpeed() x GetDashDuration() is the quantity the knob actually names.
	// =============================================================================================

	/**
	 * One arm's worth of measurement. Everything here is read through the movement component's
	 * PUBLIC surface, because GetDashSpeed(), GetDashDuration() and GetDashCooldown() are all
	 * protected — a previous scaffolding (Trace.V6.DashMeasure) hit the same wall and worked around
	 * it by reading the TRAIT instead, which measures the knob rather than the game and is exactly
	 * the mistake this command exists to avoid making twice.
	 *
	 * So the two observables are:
	 *
	 *   PEAK PLANAR SPEED while IsDashing()      == GetDashSpeed(), as applied.
	 *   PEAK GetDashCooldownRemaining()          == GetDashRechargeWindow()
	 *                                            == GetDashDuration() + GetDashCooldown().
	 *
	 * The second one is the whole reason this command can see item 2b at all. Nothing public returns
	 * the dash cooldown on its own; the recharge clock does, with the dash duration added to it, and
	 * that constant addend cancels out of the comparison between the two arms.
	 */
	struct FMortimerDashArm
	{
		float PeakSpeed = 0.f;
		float PeakRechargeWindow = 0.f;
		float Travelled = 0.f;
		float DashSeconds = 0.f;
	};

	struct FMortimerDashRun
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;
		TWeakObjectPtr<UTraceCharacterMovementComponent> Move;

		/** 0 = arm the dash, 1 = wait for it to start, 2 = sample it, 3 = report. */
		int32 Phase = 0;
		/** 0 = IDENTITY (both scales forced to 1.00), 1 = SHIPPED (the knobs as configured). */
		int32 ArmIndex = 0;

		FMortimerDashArm Arms[2];

		FVector DashStart = FVector::ZeroVector;
		double PhaseDeadline = 0.0;
		double DashBeganAt = 0.0;

		float SavedReachScale = 0.f;
		float SavedCooldownScale = 0.f;
		bool bInvalid = false;
		FString InvalidWhy;
	};

	/** Puts the two character knobs back exactly as they were found. Called on every exit path. */
	void RestoreMortimerDashKnobs(const FMortimerDashRun* Run)
	{
		if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
		{
			Mutable->MortimerDashDistanceScale = Run->SavedReachScale;
			Mutable->MortimerDashCooldownScale = Run->SavedCooldownScale;
		}
	}

	void ReportMortimerDashRun(const FMortimerDashRun* Run)
	{
		const TCHAR* const Tag = TEXT("MORTIMERDASH");
		const UTraceSettings& Settings = UTraceSettings::Get();

		const FMortimerDashArm& Identity = Run->Arms[0];
		const FMortimerDashArm& Shipped  = Run->Arms[1];

		const float BaseSpeed    = FMath::Max(1.f, Settings.DashSpeed);
		const float BaseDuration = FMath::Max(0.01f, Settings.DashDuration);
		const float BaseCooldown = FMath::Max(0.f, Settings.DashCooldown);
		const float WantReach    = Settings.MortimerDashDistanceScale;
		const float WantCooldown = Settings.MortimerDashCooldownScale;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] arm=IDENTITY (both scales 1.00)  peak dash speed %.1f uu/s  reach %.1f uu over %.3fs  "
			     "recharge window %.3fs"),
			Tag, Identity.PeakSpeed, Identity.Travelled, Identity.DashSeconds, Identity.PeakRechargeWindow);
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] arm=SHIPPED  (reach %.2f, cooldown %.2f)  peak dash speed %.1f uu/s  reach %.1f uu over "
			     "%.3fs  recharge window %.3fs"),
			Tag, WantReach, WantCooldown, Shipped.PeakSpeed, Shipped.Travelled, Shipped.DashSeconds,
			Shipped.PeakRechargeWindow);
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] baseline: DashSpeed %.0f uu/s, DashDuration %.3fs, DashCooldown %.3fs -> everybody's "
			     "recharge window is %.3fs and everybody's reach is %.0f uu."),
			Tag, BaseSpeed, BaseDuration, BaseCooldown, BaseDuration + BaseCooldown, BaseSpeed * BaseDuration);

		// ---- ITEM 2a: THE REACH -----------------------------------------------------------------
		//
		// The instrument is PEAK SPEED, not travelled distance, and that correction matters: the last
		// pass measured 139.3 uu for Mortimer against 813.5 uu for Rocco and called it "still 25%", but
		// 139.3/813.5 is 0.171. A dash hands its exit speed back (DashExitSpeedMultiplier 1.25) and the
		// pawn coasts on afterwards, so total travel is not dash reach and its ratio is not the scale.
		// Travel is printed above as corroboration and is not what the verdict rests on — it can also be
		// cut short by a wall, which is a property of where the pawn was standing and not of the knob.
		const bool bReachRouted  = Identity.PeakSpeed > 1.f
			&& !FMath::IsNearlyEqual(Identity.PeakSpeed, Shipped.PeakSpeed, 20.f);
		const float MeasuredScale = (Identity.PeakSpeed > 1.f) ? (Shipped.PeakSpeed / Identity.PeakSpeed) : -1.f;
		const bool bReachCorrect = FMath::IsNearlyEqual(MeasuredScale, WantReach, 0.02f);

		if (bReachRouted && bReachCorrect)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] ITEM 2a REACH: *** PASS *** — forcing the knob to 1.00 gave %.1f uu/s and the shipped "
				     "%.2f gave %.1f uu/s, a measured ratio of %.3f. His dash reach is %.0f uu against "
				     "everybody's %.0f uu."),
				Tag, Identity.PeakSpeed, WantReach, Shipped.PeakSpeed, MeasuredScale,
				Shipped.PeakSpeed * BaseDuration, Identity.PeakSpeed * BaseDuration);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] ITEM 2a REACH: *** FAIL *** — routed=%d correct=%d. Wanted a ratio of %.2f, measured "
				     "%.3f (%.1f uu/s shipped against %.1f uu/s at identity). If routed=0 the shipped "
				     "GetDashSpeed() is not reading TraceAbilityTraits::GetDashDistanceScale()."),
				Tag, bReachRouted ? 1 : 0, bReachCorrect ? 1 : 0, WantReach, MeasuredScale,
				Shipped.PeakSpeed, Identity.PeakSpeed);
		}

		// ---- ITEM 2b: THE COOLDOWN --------------------------------------------------------------
		//
		// GetDashRechargeWindow() is duration + cooldown, so the duration cancels: the DIFFERENCE
		// between the two arms' windows is exactly the difference between the two cooldowns.
		const bool bCooldownRouted = Identity.PeakRechargeWindow > 0.01f
			&& !FMath::IsNearlyEqual(Identity.PeakRechargeWindow, Shipped.PeakRechargeWindow, 0.05f);
		const float WantedWindow = BaseDuration + BaseCooldown * WantCooldown;
		const bool bCooldownCorrect = FMath::IsNearlyEqual(Shipped.PeakRechargeWindow, WantedWindow, 0.12f);

		if (bCooldownRouted && bCooldownCorrect)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] ITEM 2b COOLDOWN: *** PASS *** — the recharge window moved with the knob "
				     "(%.3fs at identity -> %.3fs shipped), which is a dash cooldown of %.3fs against "
				     "everybody's %.3fs, i.e. x%.2f."),
				Tag, Identity.PeakRechargeWindow, Shipped.PeakRechargeWindow,
				Shipped.PeakRechargeWindow - BaseDuration, BaseCooldown,
				(Shipped.PeakRechargeWindow - BaseDuration) / FMath::Max(0.01f, BaseCooldown));
		}
		else if (!bCooldownRouted)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] ITEM 2b COOLDOWN: *** FAIL — NOT ROUTED *** — the knob went from 1.00 to %.2f and the "
				     "shipped recharge window did not move off %.3fs (wanted %.3fs). NOTHING READS "
				     "TraceAbilityTraits::GetDashCooldownScale(). The entire fix is one multiplication:\n"
				     "        return FMath::Max(0.f, UTraceSettings::Get().DashCooldown\n"
				     "            * TraceAbilityTraits::GetDashCooldownScale(CharacterOwner));\n"
				     "    in Source/Trace/Movement/TraceCharacterMovementComponent.cpp, GetDashCooldown().\n"
				     "    THE REACH ARMS ABOVE SEPARATED ON THIS SAME INSTRUMENT IN THIS SAME RUN, so this is a "
				     "missing call site and not a harness that cannot go red."),
				Tag, WantCooldown, Shipped.PeakRechargeWindow, WantedWindow);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] ITEM 2b COOLDOWN: *** FAIL *** — routed, but the window landed at %.3fs and %.3fs was "
				     "wanted (duration %.3fs + cooldown %.3fs x %.2f)."),
				Tag, Shipped.PeakRechargeWindow, WantedWindow, BaseDuration, BaseCooldown, WantCooldown);
		}
	}

	void RunMortimerDashTest()
	{
		const TCHAR* const Tag = TEXT("MORTIMERDASH");

		UWorld* TestWorld = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->IsGameWorld()
					&& Context.World()->GetAuthGameMode() != nullptr)
				{
					TestWorld = Context.World();
					break;
				}
			}
		}
		if (TestWorld == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. Run this on the server/host, in a "
				     "live match."), Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetMortimer* Mortimer = MakePlayerIntoMortimer(TestWorld, Why);
		ATraceCharacter* MyPawn = (Mortimer != nullptr) ? Mortimer->GetCharacter() : nullptr;
		UTraceCharacterMovementComponent* Move = (MyPawn != nullptr)
			? Cast<UTraceCharacterMovementComponent>(MyPawn->GetCharacterMovement()) : nullptr;
		UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>();
		if (Mortimer == nullptr || MyPawn == nullptr || Move == nullptr || Mutable == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — %s."), Tag,
				(Mortimer == nullptr) ? *Why
				                      : TEXT("Mortimer has no pawn, no UTraceCharacterMovementComponent, or "
				                             "there is no UTraceSettings CDO"));
			return;
		}

		FMortimerDashRun* Run = new FMortimerDashRun();
		Run->Pawn = MyPawn;
		Run->Move = Move;
		Run->SavedReachScale = Mutable->MortimerDashDistanceScale;
		Run->SavedCooldownScale = Mutable->MortimerDashCooldownScale;
		Run->PhaseDeadline = FPlatformTime::Seconds() + 8.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] begin: two real dashes on %s, ~5s apart. Arm 1 forces both character scales to 1.00; "
			     "arm 2 uses the shipped %.2f / %.2f. Everything is read through the movement component's "
			     "PUBLIC surface (IsDashing, Velocity, GetDashCooldownRemaining) because its dash accessors "
			     "are protected."),
			Tag, *GetNameSafe(MyPawn), Run->SavedReachScale, Run->SavedCooldownScale);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run, Tag](float /*Delta*/) -> bool
			{
				UTraceCharacterMovementComponent* Component = Run->Move.Get();
				ATraceCharacter* Pawn = Run->Pawn.Get();
				UTraceSettings* Knobs = GetMutableDefault<UTraceSettings>();
				const double Now = FPlatformTime::Seconds();

				if (Component == nullptr || Pawn == nullptr || Knobs == nullptr)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[%s] VERDICT: INVALID — the pawn went away mid-run (respawn, or the match ended)."),
						Tag);
					RestoreMortimerDashKnobs(Run);
					delete Run;
					return false;
				}

				if (Now > Run->PhaseDeadline)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[%s] VERDICT: INVALID — arm %d stalled in phase %d. The pawn never dashed: it is "
						     "probably out of charges, dead, or the match is paused on the select screen."),
						Tag, Run->ArmIndex, Run->Phase);
					RestoreMortimerDashKnobs(Run);
					delete Run;
					return false;
				}

				switch (Run->Phase)
				{
				case 0:
				{
					// ARM THE KNOBS, THEN DASH. The identity arm forces BOTH scales to 1.00 so the two
					// arms differ in nothing else; the shipped arm restores whatever is configured.
					if (Run->ArmIndex == 0)
					{
						Knobs->MortimerDashDistanceScale = 1.f;
						Knobs->MortimerDashCooldownScale = 1.f;
					}
					else
					{
						Knobs->MortimerDashDistanceScale = Run->SavedReachScale;
						Knobs->MortimerDashCooldownScale = Run->SavedCooldownScale;
					}

					// Top the pool up so arm 2 does not have to wait out arm 1's real cooldown, and so
					// "no charges" can never be mistaken for "the knob did nothing".
					Component->RefundDashCharge();

					Run->DashStart = Pawn->GetActorLocation();
					Component->StartDash();
					Run->PhaseDeadline = Now + 3.0;
					Run->Phase = 1;
					return true;
				}

				case 1:
				{
					// StartDash() only raises bWantsToDash — the launch happens on the next simulated
					// move — so this waits for the window to actually open rather than assuming it.
					if (!Component->IsDashing())
					{
						return true;
					}
					Run->DashStart = Pawn->GetActorLocation();
					Run->DashBeganAt = Now;
					Run->PhaseDeadline = Now + 3.0;
					Run->Phase = 2;
					return true;
				}

				case 2:
				{
					FMortimerDashArm& Arm = Run->Arms[Run->ArmIndex];

					if (Component->IsDashing())
					{
						const FVector Planar(Component->Velocity.X, Component->Velocity.Y, 0.f);
						Arm.PeakSpeed = FMath::Max(Arm.PeakSpeed, static_cast<float>(Planar.Size()));
						Arm.PeakRechargeWindow = FMath::Max(Arm.PeakRechargeWindow,
							Component->GetDashCooldownRemaining());
						return true;
					}

					Arm.Travelled = static_cast<float>(
						FVector::Dist2D(Pawn->GetActorLocation(), Run->DashStart));
					Arm.DashSeconds = static_cast<float>(Now - Run->DashBeganAt);

					UE_LOG(LogTraceGame, Display,
						TEXT("[%s] arm %d done: peak %.1f uu/s, travelled %.1f uu in %.3fs, recharge window "
						     "%.3fs."),
						Tag, Run->ArmIndex, Arm.PeakSpeed, Arm.Travelled, Arm.DashSeconds,
						Arm.PeakRechargeWindow);

					++Run->ArmIndex;
					Run->Phase = (Run->ArmIndex >= 2) ? 3 : 0;
					Run->PhaseDeadline = Now + 8.0;
					return true;
				}

				default:
					break;
				}

				RestoreMortimerDashKnobs(Run);
				ReportMortimerDashRun(Run);
				delete Run;
				return false;
			}), 0.f);
	}

	FAutoConsoleCommand CmdMortimerDashTest(
		TEXT("Trace.Mortimer.DashTest"),
		TEXT("DEMO 20 item 2. Measures Mortimer's dash REACH and dash COOLDOWN through the shipped "
		     "UTraceCharacterMovementComponent accessors on a live pawn, two arms each (knob at 1.00, then "
		     "shipped), and asserts the other characters did not move. Reports PASS/FAIL per half."),
		FConsoleCommandDelegate::CreateStatic(&RunMortimerDashTest));

	// =============================================================================================
	// Trace.Mortimer.QuakeTest — DEMO 20 ITEM 3a, AND THE ANSWER TO "DOES IT DO NOTHING, OR SOMETHING
	// INVISIBLE?"
	//
	// THREE ARMS, IN THE ORDER A PLAYER WOULD HIT THEM, ALL THROUGH THE SHIPPED KEY PATH
	// (UTraceAbilityComponent::TryActivate — literally what the E key calls):
	//
	//   ARM 1  NO CORE.        The press the owner most likely made. MUST be refused, MUST fire
	//                          nothing, and — this is the finding — MUST produce no player-facing
	//                          output at all, because TryActivate() discards CanActivate()'s reason.
	//   ARM 2  CORE, WAVE OFF. Trace.Mortimer.QuakeWave 0. The ability as it shipped before this pass:
	//                          the press is accepted and enemies are launched, and NOTHING IS DRAWN.
	//                          This is the red arm — it reproduces "the quake isn't working".
	//   ARM 3  CORE, WAVE ON.  The same press, same code, cosmetic restored. The wave must spawn and
	//                          must register beads for drawing.
	//
	// The distinction the report needs is between arm 1 and arm 2, and they are different bugs: arm 1
	// is an ability that did nothing and did not say why; arm 2 is an ability that did its whole job
	// with no way to perceive it. Only arm 2 is fixed here — arm 1's refusal text belongs to the HUD.
	//
	// AFTER THE VERDICT IT KEEPS CASTING, on a ticker, so -TraceAutoShotRepeat has something to
	// photograph. A ~0.9 s effect fired once is a coin flip against a screenshot timer, and "we could
	// not get a picture of it" is how the last two of these went unverified.
	// =============================================================================================

	struct FMortimerQuakeShow
	{
		TWeakObjectPtr<UTraceAbilitySetMortimer> Mortimer;
		TWeakObjectPtr<ATraceCore> CoreActor;
		int32 CastsLeft = 0;
		double NextCastAt = 0.0;
		float SavedAbilityCooldown = 20.f;
		/** What Trace.Mortimer.QuakeWave is put back to when the photo op ends. */
		int32 RestoreWaveArm = 1;
	};

	/** One press through the shipped path, with the posture staged. Returns what TryActivate said. */
	bool PressQuake(UTraceAbilitySetMortimer* Mortimer, ATraceCore* CoreActor, bool bGiveCore)
	{
		ATraceCharacter* MyPawn = (Mortimer != nullptr) ? Mortimer->GetCharacter() : nullptr;
		UTraceAbilityComponent* Comp = (MyPawn != nullptr) ? UTraceAbilityComponent::Get(MyPawn) : nullptr;
		if (Comp == nullptr)
		{
			return false;
		}

		if (bGiveCore && CoreActor != nullptr && !UTraceAbilityComponent::IsCarrier(MyPawn))
		{
			CoreActor->TryPickup(MyPawn);
		}

		return Comp->TryActivate();
	}

	void RunMortimerQuakeTest(const TArray<FString>& Args)
	{
		const TCHAR* const Tag = TEXT("MORTIMERQUAKE");

		int32 ExtraCasts = 6;
		if (Args.Num() >= 1)
		{
			ExtraCasts = FMath::Clamp(FCString::Atoi(*Args[0]), 0, 60);
		}

		// THE SECOND ARGUMENT IS THE PHOTO OP'S ARM, and it is the reason a red screenshot exists at
		// all. The three verdict arms above always run in the same order and always end green, so
		// without this the only frames a camera could ever catch would be green ones — and one picture
		// of a blue ring is not evidence that the ring is the ability. `QuakeTest 14 0` fires the same
		// fourteen casts with the cosmetic removed, on the same fixture, for the paired image.
		int32 PhotoWaveArm = 1;
		if (Args.Num() >= 2)
		{
			PhotoWaveArm = (FCString::Atoi(*Args[1]) != 0) ? 1 : 0;
		}

		UWorld* TestWorld = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->IsGameWorld()
					&& Context.World()->GetAuthGameMode() != nullptr)
				{
					TestWorld = Context.World();
					break;
				}
			}
		}
		if (TestWorld == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. Quake's effects are server truth; "
				     "run this on the host."), Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetMortimer* Mortimer = MakePlayerIntoMortimer(TestWorld, Why);
		ATraceCharacter* MyPawn = (Mortimer != nullptr) ? Mortimer->GetCharacter() : nullptr;
		if (Mortimer == nullptr || MyPawn == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — %s."), Tag,
				(Mortimer == nullptr) ? *Why : TEXT("Mortimer has no pawn"));
			return;
		}

		ATraceCore* CoreActor = ATraceCore::Get(TestWorld);
		if (CoreActor == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — there is no Core in this world, and §3 makes Quake conditional "
				     "on carrying it. Run this in a mode B match."), Tag);
			return;
		}

		UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>();
		IConsoleVariable* const WaveArm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.QuakeWave"));
		if (Mutable == nullptr || WaveArm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — no settings CDO, or Trace.Mortimer.QuakeWave is not registered."), Tag);
			return;
		}

		// The 20 s activated cooldown is the framework's, not the ability's, and three presses in one
		// call stack would otherwise be one press and two refusals-for-the-wrong-reason. Zeroed for the
		// test and restored at the end; every press still goes through the whole of TryActivate().
		const float SavedAbilityCooldown = Mutable->MortimerBlastCooldownSeconds;
		const int32 SavedWaveArm = WaveArm->GetInt();
		Mutable->MortimerBlastCooldownSeconds = 0.f;

		const int32 Blasts0 = Mortimer->GetBlastCount();
		const int32 Waves0  = Mortimer->GetWaveCount();

		// ---- ARM 1: the owner's press. No Core. -------------------------------------------------
		//
		// The Core is handed to somebody else rather than merely "not picked up": a harness that
		// assumed he was empty-handed would silently become arm 2 the moment the fixture changed.
		ATraceCharacter* Holder = FindLivingEnemy(TestWorld, MyPawn, nullptr);
		if (Holder != nullptr)
		{
			CoreActor->TryPickup(Holder);
		}
		const bool bWasCarryingBeforeArm1 = UTraceAbilityComponent::IsCarrier(MyPawn);

		FText Arm1Reason;
		const bool bArm1CanActivate = Mortimer->CanActivate(Arm1Reason);
		const bool bArm1Fired = PressQuake(Mortimer, CoreActor, /*bGiveCore*/ false);
		const int32 Arm1Blasts = Mortimer->GetBlastCount() - Blasts0;
		const int32 Arm1Waves  = Mortimer->GetWaveCount()  - Waves0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] ARM 1  NO CORE — carrying=%d  CanActivate=%d (\"%s\")  TryActivate=%d  quakes=+%d  waves=+%d"),
			Tag, bWasCarryingBeforeArm1 ? 1 : 0, bArm1CanActivate ? 1 : 0, *Arm1Reason.ToString(),
			bArm1Fired ? 1 : 0, Arm1Blasts, Arm1Waves);

		// ---- ARM 2: with the Core, cosmetic REMOVED. The bug, reproduced. -----------------------
		WaveArm->Set(0, ECVF_SetByConsole);
		const int32 Blasts1 = Mortimer->GetBlastCount();
		const int32 Waves1  = Mortimer->GetWaveCount();
		const bool bArm2Fired = PressQuake(Mortimer, CoreActor, /*bGiveCore*/ true);
		const int32 Arm2Blasts = Mortimer->GetBlastCount() - Blasts1;
		const int32 Arm2Waves  = Mortimer->GetWaveCount()  - Waves1;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] ARM 2  CORE + Trace.Mortimer.QuakeWave 0 (the shipped-before-Demo-20 ability) — "
			     "carrying=%d  TryActivate=%d  quakes=+%d  waves=+%d"),
			Tag, UTraceAbilityComponent::IsCarrier(MyPawn) ? 1 : 0, bArm2Fired ? 1 : 0, Arm2Blasts, Arm2Waves);

		// ---- ARM 3: identical press, cosmetic restored. -----------------------------------------
		WaveArm->Set(1, ECVF_SetByConsole);
		const int32 Blasts2 = Mortimer->GetBlastCount();
		const int32 Waves2  = Mortimer->GetWaveCount();
		const bool bArm3Fired = PressQuake(Mortimer, CoreActor, /*bGiveCore*/ true);
		const int32 Arm3Blasts = Mortimer->GetBlastCount() - Blasts2;
		const int32 Arm3Waves  = Mortimer->GetWaveCount()  - Waves2;

		// How many beads the wave that just spawned actually registered. ZERO WOULD BE THE ELLE BUG.
		int32 Arm3Beads = 0;
		float Arm3Radius = 0.f;
		for (TActorIterator<ATraceMortimerQuakeWave> It(TestWorld); It; ++It)
		{
			if (ATraceMortimerQuakeWave* Wave = *It)
			{
				Arm3Beads = FMath::Max(Arm3Beads, Wave->GetDrawnBeadCount());
				Arm3Radius = FMath::Max(Arm3Radius, Wave->GetWaveRadiusUU());
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] ARM 3  CORE + Trace.Mortimer.QuakeWave 1 (shipped now) — TryActivate=%d  quakes=+%d  "
			     "waves=+%d  beads registered for drawing=%d  radius=%.0f uu"),
			Tag, bArm3Fired ? 1 : 0, Arm3Blasts, Arm3Waves, Arm3Beads, Arm3Radius);

		// ---- the verdict ------------------------------------------------------------------------
		//
		// The fixture proves itself first: arm 2 has to FIRE, or arms 2 and 3 differ because the press
		// was refused rather than because the cosmetic moved, and the whole run has measured a posture
		// gate instead of a bug.
		const bool bFixtureValid = bArm2Fired && Arm2Blasts == 1 && bArm3Fired && Arm3Blasts == 1;
		const bool bArm1Silent   = !bArm1Fired && Arm1Blasts == 0 && Arm1Waves == 0;
		const bool bRedReproduced= Arm2Waves == 0;
		const bool bGreenVisible = Arm3Waves == 1 && Arm3Beads > 0;

		if (!bFixtureValid)
		{
			// *** SAY WHICH REFUSAL, NOT WHICH REFUSAL IS LIKELY. *** The first version of this branch
			// guessed ("he is probably airborne") and guessed WRONG on its second run: the real cause
			// was that the character-select screen had handed the player ELLE, her Snap had been cast,
			// and ServerSetCharacter(Mortimer) deliberately does not clear the framework cooldown — so
			// TryActivate() refused at the cooldown gate with 26.19 s still on it, several gates before
			// any posture test. A harness that speculates about its own INVALID sends the next reader
			// to the wrong file, which is a cheaper version of the same failure this whole pass exists
			// to correct. So: ask every gate and print the answers.
			UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(MyPawn);
			FText NowReason;
			const bool bCanNow = Mortimer->CanActivate(NowReason);
			const float CooldownLeft = (Comp != nullptr) ? Comp->GetActivatedCooldownRemaining() : -1.f;

			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the shipped press did not fire in arms 2 and 3 (fired %d/%d, "
				     "quakes +%d/+%d). Diagnosis: activated cooldown %.2fs remaining, posture=%s, "
				     "CanActivate=%d (\"%s\"), carrying=%d, characters enabled=%d. A non-zero cooldown here "
				     "usually means the select screen gave this player somebody ELSE first and that "
				     "character's ability was spent — run Trace.Practice.InfiniteAbilities before this "
				     "command in the practice range. Nothing here measured the cosmetic; this reports "
				     "INVALID rather than PASS."),
				Tag, bArm2Fired ? 1 : 0, bArm3Fired ? 1 : 0, Arm2Blasts, Arm3Blasts,
				CooldownLeft, TraceMortimerBlastRefusalToString(Mortimer->CheckBlastPosture()),
				bCanNow ? 1 : 0, *NowReason.ToString(),
				UTraceAbilityComponent::IsCarrier(MyPawn) ? 1 : 0,
				UTraceAbilityComponent::AreCharactersEnabled(TestWorld) ? 1 : 0);
		}
		else if (bArm1Silent && bRedReproduced && bGreenVisible)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] VERDICT: PASS — AND THE DIAGNOSIS IS THAT QUAKE WAS NEVER DEAD. Arm 2 shows the "
				     "ability doing its whole job (press accepted, quake #%d fired) and drawing NOTHING: that is "
				     "\"does something invisible\", not \"does nothing\". Arm 3 is the identical press with %d "
				     "beads of shockwave out to %.0f uu. Arm 1 is the OTHER half of the report and is NOT fixed "
				     "here: a press with no Core is refused with a perfectly good sentence (\"%s\") that "
				     "UTraceAbilityComponent::TryActivate() throws away, so the player still gets nothing. That "
				     "one belongs to the HUD."),
				Tag, Arm2Blasts, Arm3Beads, Arm3Radius, *Arm1Reason.ToString());
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] VERDICT: *** FAIL *** — arm1Silent=%d redArmDrewNothing=%d greenArmDrewSomething=%d "
				     "(arm2 waves=+%d, arm3 waves=+%d, beads=%d)."),
				Tag, bArm1Silent ? 1 : 0, bRedReproduced ? 1 : 0, bGreenVisible ? 1 : 0,
				Arm2Waves, Arm3Waves, Arm3Beads);
		}

		// ---- and now keep casting, so a screenshot can land on one ------------------------------
		if (ExtraCasts <= 0)
		{
			Mutable->MortimerBlastCooldownSeconds = SavedAbilityCooldown;
			WaveArm->Set(SavedWaveArm, ECVF_SetByConsole);
			return;
		}

		FMortimerQuakeShow* Show = new FMortimerQuakeShow();
		Show->Mortimer = Mortimer;
		Show->CoreActor = CoreActor;
		Show->CastsLeft = ExtraCasts;
		Show->NextCastAt = FPlatformTime::Seconds() + 1.1;
		Show->SavedAbilityCooldown = SavedAbilityCooldown;
		Show->RestoreWaveArm = SavedWaveArm;

		WaveArm->Set(PhotoWaveArm, ECVF_SetByConsole);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] PHOTO OP: %d more casts, ~1.1s apart (~%.0fs), with the shockwave %s. It lasts %.2fs, so "
			     "run with -TraceAutoShotRepeat at or under 0.5 and several frames will land inside one."),
			Tag, ExtraCasts, ExtraCasts * 1.1f,
			(PhotoWaveArm != 0) ? TEXT("ON (green arm)") : TEXT("*** OFF (RED ARM — the frames must show nothing) ***"),
			UTraceSettings::Get().MortimerQuakeWaveSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Show, Tag](float /*Delta*/) -> bool
			{
				UTraceAbilitySetMortimer* Set = Show->Mortimer.Get();
				if (Set == nullptr || Show->CastsLeft <= 0)
				{
					// Put the framework cooldown back exactly as it was found, whether we finished or
					// the pawn died under us. A harness that leaves a 0 s cooldown behind has changed
					// the game it was measuring.
					if (UTraceSettings* Restore = GetMutableDefault<UTraceSettings>())
					{
						Restore->MortimerBlastCooldownSeconds = Show->SavedAbilityCooldown;
					}
					if (IConsoleVariable* RestoreArm =
						IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.QuakeWave")))
					{
						RestoreArm->Set(Show->RestoreWaveArm, ECVF_SetByConsole);
					}
					UE_LOG(LogTraceGame, Display,
						TEXT("[%s] PHOTO OP over; MortimerBlastCooldownSeconds restored to %.1fs and "
						     "Trace.Mortimer.QuakeWave to %d."),
						Tag, Show->SavedAbilityCooldown, Show->RestoreWaveArm);
					delete Show;
					return false;
				}

				if (FPlatformTime::Seconds() < Show->NextCastAt)
				{
					return true;
				}

				const bool bFired = PressQuake(Set, Show->CoreActor.Get(), /*bGiveCore*/ true);
				--Show->CastsLeft;
				Show->NextCastAt = FPlatformTime::Seconds() + 1.1;

				UE_LOG(LogTraceGame, Display, TEXT("[%s] photo cast: fired=%d, %d left."),
					Tag, bFired ? 1 : 0, Show->CastsLeft);
				return true;
			}), 0.f);
	}

	FAutoConsoleCommand CmdMortimerQuakeTest(
		TEXT("Trace.Mortimer.QuakeTest"),
		TEXT("DEMO 20 item 3a. Presses E through the shipped UTraceAbilityComponent::TryActivate() three times — "
		     "with no Core, with the Core and the shockwave REMOVED (the bug, reproduced), and with the Core and "
		     "the shockwave in place — and says which of \"does nothing\" and \"does something invisible\" was "
		     "true. Then casts N more times (default 6, ~1.1s apart) so a screenshot can land on one."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunMortimerQuakeTest));
}

#undef LOCTEXT_NAMESPACE
