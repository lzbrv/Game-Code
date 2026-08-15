// Trace — MORTIMER. See the header for the spec v19 §3 reading, for the mantle's history, and for
// exactly which half of this character is live and which half is a knob waiting on somebody else's
// one-line call site.

#include "Abilities/Characters/TraceAbilitySetMortimer.h"

#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"                   // the mantle probe's own dimensions
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"                // Trace.Mortimer.MantleTest's test ledge
#include "Engine/StaticMeshActor.h"                        // Trace.Mortimer.MantleTest's test ledge
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

/**
 * *** DEMO 21 ITEM 6'S RED ARM. *** 0 removes the mantle and NOTHING else — the probe never runs, so
 * OnJumpPressed() declines and Mortimer's jump key is the ordinary jump key again, exactly the build
 * the owner has asked three times for a mantle on.
 *
 * It is a CVar as well as bMortimerCanMantle so that Trace.Mortimer.MantleTest can flip it between
 * two arms of one run without writing to UTraceSettings (which is config-backed and would be a real
 * settings change if a run died halfway). The knob and the arm mean the same thing; the arm is the
 * one a harness should touch.
 */
static TAutoConsoleVariable<int32> CVarMortimerMantle(
	TEXT("Trace.Mortimer.Mantle"),
	1,
	TEXT("Dev/red arm. 1 (default) = Demo 21 item 6's mantle is live for Mortimer alone. 0 = it is not, "
	     "which is the build the owner reported a missing mantle in. Nothing else changes."),
	ECVF_Cheat);

/**
 * *** DEMO 21 ITEM 7'S RED ARM. *** 0 forces UTraceAbilitySetMortimer::GetThrowChargePastFullScale()
 * to 1.0, i.e. the straight line the game had before Demo 21, and changes NOTHING else — the hold cap
 * still stretches by MortimerThrowChargeHoldScale, the floor is the same, the shared curve is the
 * same. So Trace.Mortimer.ThrowTest can throw the same four Cores on both sides of the item and the
 * only difference in the world is the rule under test.
 */
static TAutoConsoleVariable<int32> CVarMortimerThrowPastFull(
	TEXT("Trace.Mortimer.ThrowPastFull"),
	1,
	TEXT("Dev/red arm. 1 (default) = Demo 21 item 7: charge past the ORIGINAL 100% point counts at "
	     "MortimerThrowChargePastFullScale (0.6). 0 = it counts in full, which is the pre-Demo-21 "
	     "straight line. Only Mortimer has any charge past 100% to scale."),
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

float UTraceAbilitySetMortimer::GetThrowChargePastFullScale() const
{
	// DEMO 21 ITEM 7. The red arm returns the IDENTITY (1.0) rather than 0, because "no modifier" is
	// what the pre-Demo-21 game did — a 0 here would mean "extra charge is worth nothing", which is a
	// third behaviour that never shipped and would make the arm prove the wrong thing.
	if (CVarMortimerThrowPastFull.GetValueOnAnyThread() == 0)
	{
		return 1.f;
	}

	// Clamped to [0,1] rather than trusted: above 1 this would make his extra charge worth MORE than
	// the base line's, which is the opposite of the item and would read as the sign being flipped.
	return FMath::Clamp(UTraceSettings::Get().MortimerThrowChargePastFullScale, 0.f, 1.f);
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
// DEMO 21 ITEM 6 — THE MANTLE
//
// Read the header block first: it says what this is (two impulses on the jump key), what it is not
// (a per-frame MOVE_Flying pull-up inside the movement component) and what the prediction caveat is.
// This block is only about the arithmetic.
//
// THE THREE MARGINS BELOW ARE FILE CONSTANTS AND NOT KNOBS, ON PURPOSE. Every one of them is a
// property of the CAPSULE or of the probe's own geometry rather than a design dial: a designer
// retuning the mantle wants the reach, the ceiling and the generosity, and four more sliders that do
// nothing legible is how this file ended up with three knobs nobody called. They are all expressed
// against the capsule radius so they follow it (spec v24 §0) — none of them is a raw uu count.
// =================================================================================================

namespace TraceMortimerMantleFile
{
	/** How far ABOVE the lip his feet are aimed, in capsule radii. The margin the forward push spends. */
	constexpr float ClearanceRadii = 1.0f;

	/** How far past the lip the landing point sits, in capsule radii. Legacy used the same half-radius. */
	constexpr float LandingInsetRadii = 0.5f;

	/** Seconds the forward push is budgeted to cross the gap in. Sets its speed; see BeginMantle. */
	constexpr float PushSeconds = 0.40f;

	/** The three heights the face is probed at, as fractions of the capsule HALF-height. */
	constexpr float ProbeFractions[] = { 0.02f, 0.5f, 1.0f };
}

void UTraceAbilitySetMortimer::ClearMantle()
{
	MantlePushDeadline = 0.f;
	MantlePushAboveZ = 0.f;
	MantlePushDirection = FVector::ZeroVector;
	MantlePushSpeed = 0.f;
}

void UTraceAbilitySetMortimer::OnPawnDied()
{
	ClearMantle();
}

void UTraceAbilitySetMortimer::OnUnequipped()
{
	ClearMantle();
}

bool UTraceAbilitySetMortimer::ProbeLedge(FTraceMortimerLedge& Out, bool bFromGround) const
{
	Out = FTraceMortimerLedge();

	const ATraceCharacter* MyPawn = GetCharacter();
	const UTraceCharacterMovementComponent* Move = GetMovement();
	const UCapsuleComponent* Capsule = (MyPawn != nullptr) ? MyPawn->GetCapsuleComponent() : nullptr;
	UWorld* const World = (MyPawn != nullptr) ? MyPawn->GetWorld() : nullptr;

	if (MyPawn == nullptr || Move == nullptr || Capsule == nullptr || World == nullptr)
	{
		Out.Why = TEXT("no pawn, no movement component or no world");
		return false;
	}

	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const FVector Here = MyPawn->GetActorLocation();
	const float FeetZ = static_cast<float>(Here.Z) - HalfHeight;

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Generosity = FMath::Clamp(TraceAbilityTraits::GetMantleGenerosityScale(MyPawn), 1.f, 4.f);

	// --- THE WINDOW, DERIVED (spec v24 §0) -------------------------------------------------------
	//
	// NOT ONE uu LITERAL IN HERE. The ceiling is a multiple of HIS OWN JUMP APEX and the floor is his
	// own step height, both read off the live movement component, so a retune of JumpZVelocity, of
	// MovementGravityScale or of MaxStepHeight moves the mantle with them. The legacy mantle stored
	// 230 uu and 55 uu flat and would have silently stopped meaning "the ledges he cannot jump" the
	// first time anybody touched the jump.
	const float GravityDown = FMath::Max(1.f, -Move->GetGravityZ());
	const float JumpApex = (Move->JumpZVelocity * Move->JumpZVelocity) / (2.f * GravityDown);

	Out.JumpApexUU = JumpApex;
	Out.CeilingUU  = JumpApex * FMath::Max(0.1f, Settings.MortimerMantleApexReach) * Generosity;
	Out.FloorUU    = FMath::Max(1.f, Move->MaxStepHeight) / Generosity;
	Out.ReachUU    = Radius * FMath::Max(1.f, Settings.MortimerMantleReachRadii) * Generosity;

	if (Out.CeilingUU <= Out.FloorUU)
	{
		Out.Why = FString::Printf(TEXT("the window is inverted (floor %.0f >= ceiling %.0f) — check "
			"MortimerMantleApexReach and MortimerMantleGenerosity"), Out.FloorUU, Out.CeilingUU);
		return false;
	}

	// --- WHICH WAY IS "AT THE LEDGE" -------------------------------------------------------------
	//
	// The movement INPUT when he is giving one, his facing when he is not. That is the whole of "a
	// ledge he is holding against": the probe only reaches 124 uu, so a face has to be right there in
	// the direction he is driving, and a player who is walking away from a wall is probing away from
	// it and gets an ordinary jump. No separate speed gate — the legacy mantle needed one because it
	// fired by itself, and this one fires because he pressed a key.
	FVector Forward = Move->GetCurrentAcceleration().GetSafeNormal2D();
	if (Forward.IsNearlyZero())
	{
		Forward = MyPawn->GetActorForwardVector().GetSafeNormal2D();
	}
	if (Forward.IsNearlyZero())
	{
		Out.Why = TEXT("no forward direction (no input and a degenerate facing)");
		return false;
	}
	Out.Forward = Forward;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceMortimerMantle), /*bTraceComplex*/ false, MyPawn);
	// The probing pawn is ignored via the constructor above — the ONE line that took the legacy
	// mantle from 0/8 successful attempts to 7/8, because these are raw line traces starting on the
	// capsule's own axis and MoveComponent's self-exclusion does not apply to them.
	Params.bFindInitialOverlaps = false;
	const FCollisionResponseParams Response = FCollisionResponseParams::DefaultResponseParam;
	const ECollisionChannel Channel = Capsule->GetCollisionObjectType();

	// --- 1. THE FACE ------------------------------------------------------------------------------
	//
	// THREE HEIGHTS, AND AN UNUSABLE HIT DOES NOT END THE SEARCH. Mined verbatim from dffea7c, where
	// keeping the FIRST probe that hit anything and breaking produced 6825 refusals reading
	// "penetrating=1 dist=0.00" against 39 real mantles: the low probe sits inside the floor slab the
	// pawn is pressed against on almost every frame of a real approach, and masked the two above it.
	FHitResult WallHit;
	bool bFoundWall = false;
	FString WallWhy = FString::Printf(TEXT("no face within %.0f uu ahead along %s"),
		Out.ReachUU, *Forward.ToCompactString());

	for (const float Fraction : TraceMortimerMantleFile::ProbeFractions)
	{
		const FVector ProbeStart(Here.X, Here.Y, FeetZ + HalfHeight * Fraction);
		FHitResult ProbeHit;
		if (!World->LineTraceSingleByChannel(ProbeHit, ProbeStart, ProbeStart + Forward * Out.ReachUU,
			Channel, Params, Response))
		{
			continue;
		}

		// THE LADDER-TO-THE-SKY GUARD, and it is measured rather than precautionary: without it a
		// headless match produced 289 mantles in 25 s and carried a bot team from Z=313 to Z=4097.
		// A penetrating hit reports ImpactPoint = the trace's own start and ImpactNormal =
		// -TraceDirection, which is horizontal and therefore passes for a wall, so the pawn mantles
		// onto itself, ends up embedded slightly higher, and does it again forever.
		if (ProbeHit.bStartPenetrating || ProbeHit.Distance < 1.f)
		{
			WallWhy = FString::Printf(
				TEXT("face at +%.0f uu is degenerate (penetrating=%d dist=%.2f actor=%s)"),
				HalfHeight * Fraction, ProbeHit.bStartPenetrating ? 1 : 0, ProbeHit.Distance,
				*GetNameSafe(ProbeHit.GetActor()));
			continue;
		}

		// Near-vertical. 0.3 is about 17 degrees of lean either way; anything flatter is a ramp he
		// should be running up rather than climbing.
		if (FMath::Abs(ProbeHit.ImpactNormal.Z) > 0.3f)
		{
			WallWhy = FString::Printf(TEXT("face at +%.0f uu is not vertical (normal.Z=%.2f, actor %s)"),
				HalfHeight * Fraction, ProbeHit.ImpactNormal.Z, *GetNameSafe(ProbeHit.GetActor()));
			continue;
		}

		// YOU MAY NOT CLIMB PEOPLE, and the reason is prediction rather than taste: this whole feature
		// is safe to run on two machines because it is derived from STATIC arena geometry, which is
		// byte-identical on both. Another pawn's position is replicated, so a mantle keyed off one
		// would put the client and the server on different ledges. (It is also a stack-of-pawns ladder
		// onto any roof in the arena, which the legacy mantle was measured doing.)
		if (Cast<APawn>(ProbeHit.GetActor()) != nullptr)
		{
			WallWhy = FString::Printf(TEXT("face at +%.0f uu belongs to a pawn (%s) — people are not ledges"),
				HalfHeight * Fraction, *GetNameSafe(ProbeHit.GetActor()));
			continue;
		}

		WallHit = ProbeHit;
		bFoundWall = true;
		break;
	}

	if (!bFoundWall)
	{
		Out.Why = WallWhy;
		return false;
	}

	// --- 2. THE TOP -------------------------------------------------------------------------------
	//
	// Dropped from above, just BEYOND the face, so it lands on the ledge's top surface and not on the
	// face itself. The overshoot is half a capsule radius: far enough in to clear the lip's bevel,
	// near enough to the edge that a narrow ledge still registers.
	const FVector TopProbeXY = WallHit.ImpactPoint
		+ Forward * (Radius * TraceMortimerMantleFile::LandingInsetRadii);
	const FVector TopStart(TopProbeXY.X, TopProbeXY.Y, FeetZ + Out.CeilingUU + HalfHeight);
	const FVector TopEnd(TopProbeXY.X, TopProbeXY.Y, FeetZ - 1.f);

	FHitResult TopHit;
	if (!World->LineTraceSingleByChannel(TopHit, TopStart, TopEnd, Channel, Params, Response))
	{
		Out.Why = FString::Printf(TEXT("no top above the face (probed z %.0f down to %.0f) — it is taller "
			"than the %.0f uu ceiling, or it is a pillar with nothing on it"),
			TopStart.Z, TopEnd.Z, Out.CeilingUU);
		return false;
	}
	if (TopHit.bStartPenetrating)
	{
		Out.Why = TEXT("the top probe started inside geometry, so its 'surface' does not exist");
		return false;
	}
	if (!Move->IsWalkable(TopHit))
	{
		Out.Why = FString::Printf(TEXT("the top is not walkable (normal.Z=%.2f) — an underside or a slope "
			"he would slide straight back off"), TopHit.ImpactNormal.Z);
		return false;
	}
	if (Cast<APawn>(TopHit.GetActor()) != nullptr)
	{
		Out.Why = FString::Printf(TEXT("the top belongs to a pawn (%s)"), *GetNameSafe(TopHit.GetActor()));
		return false;
	}

	Out.TopZ = static_cast<float>(TopHit.ImpactPoint.Z);
	Out.LedgeHeightUU = Out.TopZ - FeetZ;

	if (Out.LedgeHeightUU < Out.FloorUU || Out.LedgeHeightUU > Out.CeilingUU)
	{
		Out.Why = FString::Printf(TEXT("ledge height %.0f uu is outside the window [%.0f, %.0f] "
			"(apex %.0f x %.2f x generosity %.2f)"),
			Out.LedgeHeightUU, Out.FloorUU, Out.CeilingUU, JumpApex,
			Settings.MortimerMantleApexReach, Generosity);
		return false;
	}

	// *** THE GROUND RULE. *** Standing on the floor, a ledge he can already JUMP onto is not a
	// mantle — and consuming the jump key for it would take his ordinary jump away everywhere near a
	// crate, which reads as the character being broken rather than as an ability firing. So on the
	// ground the live window is (apex, ceiling]; in the air it is [floor, ceiling], which is the
	// "came up short at the lip" save the mantle was originally asked for.
	if (bFromGround && Out.LedgeHeightUU <= JumpApex)
	{
		Out.Why = FString::Printf(TEXT("standing still at a %.0f uu ledge and his own jump apex is %.0f uu "
			"— a plain jump gets him there, so the jump key stays the jump key"),
			Out.LedgeHeightUU, JumpApex);
		return false;
	}

	// --- 3. ROOM TO STAND, PROVED BEFORE ANYTHING MOVES -------------------------------------------
	//
	// A mantle that starts and then finds the destination occupied has to either stop dead in mid-air
	// or push the pawn into geometry, and both of those are corrections waiting to happen — which is
	// the entire class of bug this feature's history is about.
	Out.Destination = FVector(TopHit.ImpactPoint.X, TopHit.ImpactPoint.Y, Out.TopZ + HalfHeight + 2.f);

	// AND IT HAS TO BE SOMEWHERE ELSE. A "ledge" directly overhead is him mantling his own column —
	// the last guard against the climbing loop, and the one that still holds if some future geometry
	// finds a way past the two penetration tests.
	if (FVector::DistSquared2D(Out.Destination, Here) < FMath::Square(Radius * 0.5f))
	{
		Out.Why = FString::Printf(TEXT("the destination is directly overhead (%.0f uu ahead)"),
			static_cast<float>(FVector::Dist2D(Out.Destination, Here)));
		return false;
	}

	// Shrunk by 1 uu so a destination flush against a wall is not rejected by its own floor.
	const FCollisionShape StandShape = FCollisionShape::MakeCapsule(Radius - 1.f, HalfHeight - 1.f);
	if (World->OverlapBlockingTestByChannel(Out.Destination, FQuat::Identity, Channel, StandShape,
		Params, Response))
	{
		Out.Why = FString::Printf(TEXT("no room to stand at %s"), *Out.Destination.ToCompactString());
		return false;
	}

	Out.bFound = true;
	return true;
}

bool UTraceAbilitySetMortimer::TryMantle()
{
	// --- THE GATE, IN ORDER. Every early return costs at most two int compares before any trace. ---

	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* Move = GetMovement();
	const UCapsuleComponent* Capsule = (MyPawn != nullptr) ? MyPawn->GetCapsuleComponent() : nullptr;
	if (MyPawn == nullptr || Move == nullptr || Capsule == nullptr || !MyPawn->IsAlive())
	{
		return false;
	}

	// *** FIRST QUESTION, AND IT IS THE ONE THE WHOLE FEATURE'S SAFETY RESTS ON. *** False for every
	// character but Mortimer, so nine tenths of the roster never reach a single line trace below.
	if (!TraceAbilityTraits::IsMantleAllowed(MyPawn))
	{
		return false;
	}

	if (CVarMortimerMantle.GetValueOnAnyThread() == 0)
	{
		return false;   // RED ARM: the mantle is removed and nothing else is.
	}

	const float Now = MatchTimeNow();

	// Already climbing, or still inside the rate limit. Declines rather than consuming the key, so a
	// second press during a mantle is still a normal jump press for whatever else wants it.
	if (IsMantling() || Now < MantleReadyTime)
	{
		return false;
	}

	// A SLIDE JUMP OUTRANKS A MANTLE. It is a deliberate, timed, high-value momentum play that the
	// player has already spent a slide setting up, it lives inside the movement component's saved-move
	// pipeline (so it is the better-predicted move), and there is no way to give it back once the
	// window has closed. The mantle can wait a third of a second; that cannot.
	//
	// *** THE WALL JUMP DELIBERATELY DOES NOT. *** The legacy mantle's rule was the other way round —
	// "a wall jump outranks a mantle", with a 0.30 s lockout — and it was right FOR THAT MANTLE,
	// because that one fired by itself and would otherwise steal wall jumps the player never asked to
	// give up. This one only fires when the player presses jump, and a player pressing jump while
	// airborne against a ledge he can climb wants to be on top of it. The inversion is safe precisely
	// because the probe is narrow: a tall wall has no walkable top inside the height ceiling, the probe
	// finds nothing, and the press falls through to the wall jump exactly as it does today.
	if (Move->IsSlideJumpAvailable())
	{
		return false;
	}

	const bool bFromGround = Move->IsGroundedForAbilities();

	FTraceMortimerLedge Ledge;
	if (!ProbeLedge(Ledge, bFromGround))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Mortimer] mantle refused: %s"), *Ledge.Why);
		return false;
	}

	// --- THE LAUNCH. TWO IMPULSES; THIS IS THE FIRST. ---------------------------------------------
	//
	// Aimed so his FEET arrive one capsule radius above the lip. Solved rather than tuned: under
	// constant gravity the launch speed that just reaches a height h is sqrt(2 g h), so this is the
	// smallest rise that clears the ledge and it moves automatically with gravity and with the ledge.
	// A flat "mantle jump velocity" knob would be too weak for a tall ledge and a trampoline off a
	// short one.
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const float FeetZ = static_cast<float>(MyPawn->GetActorLocation().Z) - HalfHeight;
	const float GravityDown = FMath::Max(1.f, -Move->GetGravityZ());

	const float Clearance = Radius * TraceMortimerMantleFile::ClearanceRadii;
	const float NeededRise = FMath::Max(1.f, (Ledge.TopZ + Clearance) - FeetZ);
	const float RiseSpeed = FMath::Min(FMath::Sqrt(2.f * GravityDown * NeededRise),
		Move->JumpZVelocity * 3.f);

	// bXYOverride AND bZOverride. A mantle is a COMMIT: zeroing the planar velocity stops a Mortimer
	// who was running along the wall from sliding off the side of the ledge he just asked to climb,
	// and assigning Z (rather than taking a max with it) makes the rise the same whether he pressed
	// while falling fast, while rising, or from a standstill. Determinism here is what lets the
	// server's copy of this press land in the same place as the client's.
	MyPawn->LaunchCharacter(FVector(0.f, 0.f, RiseSpeed), /*bXYOverride*/ true, /*bZOverride*/ true);

	// --- AND THE SECOND ONE IS ARMED, NOT FIRED ---------------------------------------------------
	//
	// It waits for his feet to clear the lip because firing it NOW would push him straight into the
	// face he is climbing, where the engine's SlideAlongSurface would eat the whole forward component
	// and he would rise, stall and drop back down exactly as if nothing had happened. That is the one
	// non-obvious thing about this implementation and it is why there are two impulses instead of one.
	const float Distance2D = static_cast<float>(FVector::Dist2D(Ledge.Destination, MyPawn->GetActorLocation()));

	MantlePushDirection = Ledge.Forward;
	MantlePushAboveZ    = Ledge.TopZ + 4.f;
	MantlePushSpeed     = FMath::Clamp(Distance2D / TraceMortimerMantleFile::PushSeconds,
		0.25f * Move->GetMaxSpeed(), 1.5f * Move->GetMaxSpeed());

	// The give-up clock: twice the ballistic rise time, floored at half a second. If he never gets
	// there — a lift moved, a Quake threw him, the ledge was destroyed — the mantle is abandoned and
	// he simply falls, rather than being shoved sideways a second later by a stale impulse.
	const float RiseSeconds = RiseSpeed / GravityDown;
	MantlePushDeadline = Now + FMath::Max(0.5f, RiseSeconds * 2.f);
	MantleReadyTime    = Now + FMath::Max(0.f, UTraceSettings::Get().MortimerMantleCooldownSeconds);
	++MantleCount;

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Mortimer] MANTLE #%d from %s: ledge %.0f uu (window %.0f..%.0f, apex %.0f, reach %.0f), "
		     "rise %.0f uu/s, push %.0f uu/s over %.0f uu when feet pass z=%.0f."),
		MantleCount, bFromGround ? TEXT("the ground") : TEXT("the air"), Ledge.LedgeHeightUU,
		Ledge.FloorUU, Ledge.CeilingUU, Ledge.JumpApexUU, Ledge.ReachUU, RiseSpeed, MantlePushSpeed,
		Distance2D, MantlePushAboveZ);

	return true;
}

bool UTraceAbilitySetMortimer::OnJumpPressed()
{
	// TRUE CONSUMES THE JUMP (ATracePlayerController::OnJumpStarted), so this returns true only when a
	// mantle actually started. A press that finds no ledge has to fall through to ACharacter::Jump or
	// Mortimer would stop being able to jump anywhere near a wall — which is a much worse bug than a
	// missing mantle, and is the failure mode every other character on this hook guards against too.
	return TryMantle();
}

void UTraceAbilitySetMortimer::TickAbilities(float /*DeltaSeconds*/)
{
	if (!IsMantling())
	{
		return;   // Every machine, every tick, for every Mortimer who is not mid-mantle. One compare.
	}

	ATraceCharacter* MyPawn = GetCharacter();
	UTraceCharacterMovementComponent* Move = GetMovement();
	const UCapsuleComponent* Capsule = (MyPawn != nullptr) ? MyPawn->GetCapsuleComponent() : nullptr;
	const float Now = MatchTimeNow();

	if (MyPawn == nullptr || Move == nullptr || Capsule == nullptr || !MyPawn->IsAlive())
	{
		ClearMantle();
		return;
	}

	const float FeetZ = static_cast<float>(MyPawn->GetActorLocation().Z)
		- Capsule->GetScaledCapsuleHalfHeight();

	if (Now > MantlePushDeadline)
	{
		// He never got there. Say so at Verbose rather than silently: "the mantle sometimes does
		// nothing" is exactly the report this branch explains, and the deadline is the only thing in
		// the feature that can produce it.
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Mortimer] mantle abandoned: feet reached z=%.0f, needed z=%.0f before the deadline."),
			FeetZ, MantlePushAboveZ);
		ClearMantle();
		return;
	}

	// Landed again without ever clearing the lip — he was blocked, or the ledge moved. Same abandon.
	if (Move->IsMovingOnGround() && FeetZ < MantlePushAboveZ)
	{
		ClearMantle();
		return;
	}

	if (FeetZ < MantlePushAboveZ)
	{
		return;   // Still climbing.
	}

	// *** THE SECOND AND LAST IMPULSE. *** bXYOverride so the planar velocity IS the push (he has none
	// of his own: the first impulse zeroed it), and NOT bZOverride so whatever the climb has left —
	// usually a few tens of uu/s — is kept and he arcs over the lip instead of stopping dead above it.
	MyPawn->LaunchCharacter(FVector(MantlePushDirection.X * MantlePushSpeed,
		MantlePushDirection.Y * MantlePushSpeed, 0.f), /*bXYOverride*/ true, /*bZOverride*/ false);

	++MantlePushCount;
	ClearMantle();
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

		Check(FMath::IsNearlyEqual(Settings.MortimerThrowChargePastFullScale, 0.6f, 0.001f),
			FString::Printf(TEXT("Demo 21 item 7 asks for a .6x modifier past the original 100%% charge; "
			                     "the knob is %.3f. Config/DefaultGame.ini beats TraceSettings.h — check "
			                     "BOTH."), Settings.MortimerThrowChargePastFullScale));

		// *** THE GATE, ASSERTED RATHER THAN ASSUMED. *** A null actor stands in for "any pawn with no
		// ability component" — a Mannequin, a bot mid-spawn, a client pawn whose PlayerState has not
		// arrived. If this ever returns true, every character in the game has just been given a mantle.
		Check(!TraceAbilityTraits::IsMantleAllowed(nullptr),
			TEXT("TraceAbilityTraits::IsMantleAllowed(nullptr) must be FALSE — it is the gate that keeps "
			     "the mantle off the other nine characters and off every Mannequin"));
		Check(FMath::IsNearlyEqual(TraceAbilityTraits::GetThrowChargePastFullScale(nullptr), 1.f, 0.001f),
			TEXT("TraceAbilityTraits::GetThrowChargePastFullScale(nullptr) must be 1.0 — Demo 21 item 7 "
			     "must be arithmetically absent for everybody but Mortimer"));

		// --- and the derived numbers a designer actually reads ------------------------------------
		const float DashReach = FMath::Max(0.f, Settings.DashSpeed) * FMath::Max(0.f, Settings.DashDuration);
		const float HisReach  = DashReach * Settings.MortimerDashDistanceScale;
		const float FullHold  = FMath::Max(0.01f, Settings.CoreThrowChargeSeconds);
		const float HisHold   = FullHold * Settings.MortimerThrowChargeHoldScale;
		const float Floor     = FMath::Clamp(Settings.CoreThrowChargeFloorFraction, 0.f, 1.f);

		const float HisCooldown = FMath::Max(0.f, Settings.DashCooldown) * Settings.MortimerDashCooldownScale;

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] DASH   everybody %.0f uu (%.0f uu/s x %.2fs) -> Mortimer %.0f uu. "
			     "COOLDOWN everybody %.2fs -> Mortimer %.2fs (x%.2f). Both LIVE; Trace.Mortimer.DashTest "
			     "measures them on a live pawn rather than printing the knobs back."),
			Tag, DashReach, Settings.DashSpeed, Settings.DashDuration, HisReach,
			Settings.DashCooldown, HisCooldown, Settings.MortimerDashCooldownScale);
		// DEMO 21 ITEM 7. Both numbers, from the SHIPPED function rather than from a second copy of the
		// arithmetic — the whole point of ATraceCore::GetThrowChargeScaleForHold being one expression.
		const float LegacyPower = Floor + (1.f - Floor) * Settings.MortimerThrowChargeHoldScale;
		const float BentPower   = Floor + (1.f - Floor)
			* (1.f + FMath::Clamp(Settings.MortimerThrowChargePastFullScale, 0.f, 1.f)
				* (Settings.MortimerThrowChargeHoldScale - 1.f));

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] THROW  everybody may hold %.2fs for x1.00 launch speed -> Mortimer may hold %.2fs. "
			     "ON THE SAME LINE (Power = %.2f + %.2f x t) that used to be x%.2f; DEMO 21 ITEM 7 bends it "
			     "past the original 100%% point at x%.2f, so his full extended charge is now x%.3f."),
			Tag, FullHold, HisHold, Floor, 1.f - Floor, LegacyPower,
			Settings.MortimerThrowChargePastFullScale, BentPower);
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] THROW  SPEED IS NOT DISTANCE. Flat range goes as speed SQUARED (the loose Core is "
			     "integrated under gravity with no drag), so his ceiling moves from about %.2fx a "
			     "full-charge throw's range to about %.2fx it. Trace.Mortimer.ThrowTest throws four real "
			     "Cores and MEASURES this; %.2f/%.2f here is arithmetic, not a measurement."),
			Tag, LegacyPower * LegacyPower, BentPower * BentPower, LegacyPower, BentPower);
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
			TEXT("[%s] LIVE: the dash REACH and the dash COOLDOWN (Movement/TraceCharacterMovementComponent"
			     ".cpp, GetDashSpeed / GetDashCooldown multiply by TraceAbilityTraits::GetDash*Scale), and "
			     "the Core-throw CAP plus Demo 21 item 7's 0.6x bend (Gameplay/TraceCore.cpp, "
			     "GetThrowChargeScaleForHold). QUAKE IS LIVE and draws a shockwave. Measured by "
			     "Trace.Mortimer.DashTest and Trace.Mortimer.ThrowTest — believe those, not this line."),
			Tag);

		// *** THIS BLOCK SHIPPED A FALSE 'NOT LIVE' FOR THE DASH COOLDOWN FOR A WHOLE DEMO after the
		// call site landed, which is the same failure the paragraph above it warns about, made twice.
		// It is deleted rather than reworded. The mantle's 'NOT LIVE' is deleted too, because Demo 21
		// item 6 gives it a body — in THIS file, not in Movement/.
		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] MANTLE (Demo 21 item 6) LIVE: UTraceAbilitySetMortimer::OnJumpPressed() -> "
			     "TryMantle(), first question TraceAbilityTraits::IsMantleAllowed(). Window is DERIVED, "
			     "not typed: ceiling = jump apex x %.2f x generosity %.2f, floor = MaxStepHeight / "
			     "generosity, reach = capsule radius x %.2f x generosity. Red arm Trace.Mortimer.Mantle 0; "
			     "measured by Trace.Mortimer.MantleTest, which builds its own ledge."),
			Tag, Settings.MortimerMantleApexReach, Settings.MortimerMantleGenerosity,
			Settings.MortimerMantleReachRadii);

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

	// =============================================================================================
	// Trace.Mortimer.MantleTest — DEMO 21 ITEM 6, MEASURED THROUGH THE SHIPPED JUMP KEY
	//
	// THREE ARMS ON ONE FIXTURE, RED FIRST.
	//
	//   0  RED    Trace.Mortimer.Mantle 0. The identical press at the identical ledge. He must NOT get
	//             up, and the press must NOT be consumed — this is the build the owner has reported a
	//             missing mantle in three times, reproduced on the instrument that is about to say it
	//             is fixed.
	//   1  GREEN  the shipped build. He must get up ONTO the ledge and the press must be consumed.
	//   2  GUARD  the same green build against a ledge BELOW his own jump apex. The press must NOT be
	//             consumed, because a plain jump already reaches it. This arm is the reason the ground
	//             rule exists: without it Mortimer would lose his ordinary jump everywhere near a
	//             crate, which is a far worse regression than a missing mantle and is exactly the kind
	//             of thing a two-arm harness would have shipped without noticing.
	//
	// IT BUILDS ITS OWN LEDGE. A fixture that depends on finding a suitable block in whatever arena
	// happens to be loaded is a fixture that reports INVALID for map reasons; a 200 uu cube spawned in
	// front of him, sized so its top is at a chosen height above his feet, is the same geometry every
	// run. It is destroyed on every exit path.
	//
	// AND IT PRESSES THE REAL KEY. UTraceAbilityComponent::HandleJumpPressed() is the function
	// ATracePlayerController::OnJumpStarted calls; nothing here reaches into TryMantle() behind it, so
	// a mantle that works only when called directly cannot pass.
	// =============================================================================================

	struct FMortimerMantleArm
	{
		bool  bPressConsumed = false;
		bool  bEndedOnLedge = false;
		float StartFeetZ = 0.f;
		float PeakFeetZ = 0.f;
		float EndFeetZ = 0.f;
		float LedgeTopZ = 0.f;
		float LedgeHeight = 0.f;
		bool  bProbeFound = false;
		FString ProbeWhy;
	};

	struct FMortimerMantleRun
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;
		TWeakObjectPtr<UTraceAbilitySetMortimer> Set;
		TWeakObjectPtr<AStaticMeshActor> Block;

		/** 0 = place and settle, 1 = press, 2 = watch, 3 = report. */
		int32 Phase = 0;
		int32 ArmIndex = 0;
		double PhaseDeadline = 0.0;
		double WatchUntil = 0.0;

		FVector Anchor = FVector::ZeroVector;
		FRotator AnchorRotation = FRotator::ZeroRotator;
		float JumpApexUU = 0.f;

		FMortimerMantleArm Arms[3];
		int32 SavedMantleArm = 1;
	};

	/** The three ledge heights, as multiples of his own jump apex. Derived, so a jump retune follows. */
	constexpr float MantleTestTallApexes = 1.28f;   // above the apex: only a mantle reaches it
	constexpr float MantleTestLowApexes  = 0.55f;   // well below it: a plain jump reaches it

	void EndMortimerMantleRun(FMortimerMantleRun* Run)
	{
		if (IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.Mantle")))
		{
			Arm->Set(Run->SavedMantleArm, ECVF_SetByConsole);
		}
		if (AStaticMeshActor* Block = Run->Block.Get())
		{
			Block->Destroy();
		}
		delete Run;
	}

	/** Moves and resizes the test ledge so its TOP sits @p HeightAboveFeet above the anchor's feet. */
	void PlaceMortimerTestLedge(FMortimerMantleRun* Run, float HeightAboveFeet, float FeetZ, float Radius)
	{
		AStaticMeshActor* Block = Run->Block.Get();
		UStaticMeshComponent* Mesh = (Block != nullptr) ? Block->GetStaticMeshComponent() : nullptr;
		if (Mesh == nullptr)
		{
			return;
		}

		// Tall enough to reach 200 uu BELOW his feet, so the low face probe has something to hit even
		// when he is stood on a floor slab — the legacy mantle's 6825 degenerate refusals were exactly
		// a low probe with nothing usable in front of it.
		const float TopZ = FeetZ + HeightAboveFeet;
		const float BlockHeight = HeightAboveFeet + 200.f;
		const FVector Forward = Run->AnchorRotation.Vector().GetSafeNormal2D();

		Mesh->SetWorldScale3D(FVector(2.f, 2.f, BlockHeight / 100.f));
		Block->SetActorLocation(
			FVector(Run->Anchor.X, Run->Anchor.Y, 0.f) + Forward * (Radius + 100.f + 25.f)
				+ FVector(0.f, 0.f, TopZ - BlockHeight * 0.5f),
			/*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
	}

	void ReportMortimerMantleRun(const FMortimerMantleRun* Run)
	{
		const TCHAR* const Tag = TEXT("MORTIMERMANTLE");

		const FMortimerMantleArm& Red   = Run->Arms[0];
		const FMortimerMantleArm& Green = Run->Arms[1];
		const FMortimerMantleArm& Guard = Run->Arms[2];

		for (int32 Index = 0; Index < 3; ++Index)
		{
			static const TCHAR* const Names[3] = { TEXT("RED   (Trace.Mortimer.Mantle 0)"),
			                                       TEXT("GREEN (shipped)"),
			                                       TEXT("GUARD (ledge below his jump apex)") };
			const FMortimerMantleArm& Arm = Run->Arms[Index];
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%s  ledge %.0f uu (top z=%.0f)  press consumed=%d  feet %.0f -> peak %.0f -> "
				     "end %.0f  onLedge=%d | probe found=%d %s"),
				Tag, Names[Index], Arm.LedgeHeight, Arm.LedgeTopZ, Arm.bPressConsumed ? 1 : 0,
				Arm.StartFeetZ, Arm.PeakFeetZ, Arm.EndFeetZ, Arm.bEndedOnLedge ? 1 : 0,
				Arm.bProbeFound ? 1 : 0, *Arm.ProbeWhy);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] his own jump apex is %.0f uu, so the tall ledge (%.0f uu) is one a plain jump "
			     "CANNOT reach and the low ledge (%.0f uu) is one it can."),
			Tag, Run->JumpApexUU, Red.LedgeHeight, Guard.LedgeHeight);

		// THE FIXTURE PROVES ITSELF FIRST. If the red arm ALSO got him onto the ledge, then something
		// other than the mantle is lifting him and every number here is meaningless.
		if (Red.bEndedOnLedge || Red.bPressConsumed)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the RED arm (mantle removed) still consumed the press (%d) or "
				     "still ended on the ledge (%d). Something other than Demo 21 item 6 is lifting him, so "
				     "the green arm proves nothing."),
				Tag, Red.bPressConsumed ? 1 : 0, Red.bEndedOnLedge ? 1 : 0);
			return;
		}

		if (!Green.bEndedOnLedge || !Green.bPressConsumed)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] VERDICT: *** FAIL *** — the shipped build did not mantle a %.0f uu ledge: press "
				     "consumed=%d, ended on the ledge=%d (feet peaked at %.0f, the lip is at %.0f). The probe "
				     "said: found=%d %s"),
				Tag, Green.LedgeHeight, Green.bPressConsumed ? 1 : 0, Green.bEndedOnLedge ? 1 : 0,
				Green.PeakFeetZ, Green.LedgeTopZ, Green.bProbeFound ? 1 : 0, *Green.ProbeWhy);
			return;
		}

		if (Guard.bPressConsumed)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] VERDICT: *** FAIL *** — the mantle STOLE THE ORDINARY JUMP at a %.0f uu ledge, "
				     "which is below his own %.0f uu apex. The ground rule in ProbeLedge() is not firing, and "
				     "Mortimer has just lost his jump near every crate in the arena."),
				Tag, Guard.LedgeHeight, Run->JumpApexUU);
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] VERDICT: PASS — with the mantle REMOVED the identical press was declined and his feet "
			     "finished %.0f uu SHORT of a lip at z=%.0f; with it in place the press was consumed, his feet "
			     "peaked at z=%.0f and he ended STOOD ON that lip at z=%.0f. A %.0f uu ledge (under his %.0f uu "
			     "apex) still leaves the jump key alone."),
			Tag, Red.LedgeTopZ - Red.PeakFeetZ, Red.LedgeTopZ, Green.PeakFeetZ, Green.EndFeetZ,
			Guard.LedgeHeight, Run->JumpApexUU);
	}

	void RunMortimerMantleTest()
	{
		const TCHAR* const Tag = TEXT("MORTIMERMANTLE");

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
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. Run this on the server/host."), Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetMortimer* Mortimer = MakePlayerIntoMortimer(TestWorld, Why);
		ATraceCharacter* MyPawn = (Mortimer != nullptr) ? Mortimer->GetCharacter() : nullptr;
		UTraceCharacterMovementComponent* Move = (MyPawn != nullptr)
			? Cast<UTraceCharacterMovementComponent>(MyPawn->GetCharacterMovement()) : nullptr;
		UCapsuleComponent* Capsule = (MyPawn != nullptr) ? MyPawn->GetCapsuleComponent() : nullptr;
		IConsoleVariable* const Arm = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.Mantle"));
		if (Mortimer == nullptr || MyPawn == nullptr || Move == nullptr || Capsule == nullptr || Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — %s."), Tag,
				(Mortimer == nullptr) ? *Why
				                      : TEXT("Mortimer has no pawn / movement component / capsule, or "
				                             "Trace.Mortimer.Mantle is not registered"));
			return;
		}

		UStaticMesh* const Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (Cube == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — /Engine/BasicShapes/Cube is not available, so no test ledge can "
				     "be built."), Tag);
			return;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Block = TestWorld->SpawnActor<AStaticMeshActor>(
			MyPawn->GetActorLocation() + FVector(0.f, 0.f, 5000.f), FRotator::ZeroRotator, SpawnParams);
		if (Block == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — could not spawn the test ledge."), Tag);
			return;
		}
		// Movable or SetActorLocation is refused: this block is repositioned between arms.
		Block->SetMobility(EComponentMobility::Movable);
		if (UStaticMeshComponent* Mesh = Block->GetStaticMeshComponent())
		{
			Mesh->SetStaticMesh(Cube);
			Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Mesh->SetCollisionProfileName(TEXT("BlockAll"));
		}

		FMortimerMantleRun* Run = new FMortimerMantleRun();
		Run->Pawn = MyPawn;
		Run->Set = Mortimer;
		Run->Block = Block;
		Run->Anchor = MyPawn->GetActorLocation();
		Run->AnchorRotation = FRotator(0.f, MyPawn->GetActorRotation().Yaw, 0.f);
		Run->SavedMantleArm = Arm->GetInt();
		Run->PhaseDeadline = FPlatformTime::Seconds() + 6.0;

		const float GravityDown = FMath::Max(1.f, -Move->GetGravityZ());
		Run->JumpApexUU = (Move->JumpZVelocity * Move->JumpZVelocity) / (2.f * GravityDown);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] begin: three arms on one built ledge, RED first. %s, jump apex %.0f uu, capsule "
			     "r=%.0f h=%.0f. The ledge is %.0f uu tall for arms 0 and 1 and %.0f uu for arm 2."),
			Tag, *GetNameSafe(MyPawn), Run->JumpApexUU, Capsule->GetScaledCapsuleRadius(),
			Capsule->GetScaledCapsuleHalfHeight(), Run->JumpApexUU * MantleTestTallApexes,
			Run->JumpApexUU * MantleTestLowApexes);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run, Tag](float /*Delta*/) -> bool
			{
				ATraceCharacter* Pawn = Run->Pawn.Get();
				UTraceAbilitySetMortimer* Set = Run->Set.Get();
				UTraceCharacterMovementComponent* Component = (Pawn != nullptr)
					? Cast<UTraceCharacterMovementComponent>(Pawn->GetCharacterMovement()) : nullptr;
				UCapsuleComponent* PawnCapsule = (Pawn != nullptr) ? Pawn->GetCapsuleComponent() : nullptr;
				IConsoleVariable* const MantleArm =
					IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.Mantle"));
				const double Now = FPlatformTime::Seconds();

				if (Pawn == nullptr || Set == nullptr || Component == nullptr || PawnCapsule == nullptr
					|| MantleArm == nullptr || Run->Block.Get() == nullptr)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[%s] VERDICT: INVALID — the pawn or the test ledge went away mid-run."), Tag);
					EndMortimerMantleRun(Run);
					return false;
				}

				if (Now > Run->PhaseDeadline)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[%s] VERDICT: INVALID — arm %d stalled in phase %d."), Tag, Run->ArmIndex, Run->Phase);
					EndMortimerMantleRun(Run);
					return false;
				}

				const float HalfHeight = PawnCapsule->GetScaledCapsuleHalfHeight();
				const float Radius = PawnCapsule->GetScaledCapsuleRadius();
				const float FeetZ = static_cast<float>(Pawn->GetActorLocation().Z) - HalfHeight;
				FMortimerMantleArm& CurrentArm = Run->Arms[Run->ArmIndex];

				switch (Run->Phase)
				{
				case 0:
				{
					// RED FIRST. Arm 0 removes the mantle; arms 1 and 2 restore it.
					MantleArm->Set(Run->ArmIndex == 0 ? 0 : 1, ECVF_SetByConsole);

					// Back to the anchor every arm, with no velocity, so the three presses start from
					// the identical state and arm 1's success cannot set arm 2 up on top of a block.
					Pawn->SetActorLocation(Run->Anchor, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
					Pawn->SetActorRotation(Run->AnchorRotation);
					Component->StopMovementImmediately();
					Component->Velocity = FVector::ZeroVector;

					const float Height = Run->JumpApexUU
						* ((Run->ArmIndex == 2) ? MantleTestLowApexes : MantleTestTallApexes);
					const float AnchorFeetZ = static_cast<float>(Run->Anchor.Z) - HalfHeight;
					PlaceMortimerTestLedge(Run, Height, AnchorFeetZ, Radius);

					CurrentArm.LedgeHeight = Height;
					CurrentArm.LedgeTopZ = AnchorFeetZ + Height;
					CurrentArm.StartFeetZ = AnchorFeetZ;
					CurrentArm.PeakFeetZ = AnchorFeetZ;

					Run->PhaseDeadline = Now + 3.0;
					Run->WatchUntil = Now + 0.35;   // let the teleport settle before pressing
					Run->Phase = 1;
					return true;
				}

				case 1:
				{
					if (Now < Run->WatchUntil)
					{
						return true;
					}

					// WHAT THE PROBE SAW, recorded BEFORE the press, so a refusal has a sentence attached
					// to it rather than being an absence. This is a pure read; it moves nothing.
					FTraceMortimerLedge Ledge;
					CurrentArm.bProbeFound = Set->ProbeLedge(Ledge, Component->IsGroundedForAbilities());
					CurrentArm.ProbeWhy = CurrentArm.bProbeFound
						? FString::Printf(TEXT("(ledge %.0f uu, window %.0f..%.0f, reach %.0f)"),
							Ledge.LedgeHeightUU, Ledge.FloorUU, Ledge.CeilingUU, Ledge.ReachUU)
						: Ledge.Why;

					// *** THE REAL KEY, THROUGH THE REAL PIPELINE. *** This is the function
					// ATracePlayerController::OnJumpStarted calls, so a mantle that only works when
					// TryMantle() is called directly cannot pass this harness.
					UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Pawn);
					CurrentArm.bPressConsumed = (Abilities != nullptr) && Abilities->HandleJumpPressed();

					Run->WatchUntil = Now + 1.8;
					Run->PhaseDeadline = Now + 4.0;
					Run->Phase = 2;
					return true;
				}

				case 2:
				{
					CurrentArm.PeakFeetZ = FMath::Max(CurrentArm.PeakFeetZ, FeetZ);
					if (Now < Run->WatchUntil)
					{
						return true;
					}

					CurrentArm.EndFeetZ = FeetZ;
					// ON the lip, not merely level with it: he must also be standing, or a Mortimer
					// still arcing through the right height would count as a successful mantle.
					CurrentArm.bEndedOnLedge = Component->IsMovingOnGround()
						&& FeetZ > (CurrentArm.LedgeTopZ - 8.f);

					++Run->ArmIndex;
					Run->Phase = (Run->ArmIndex >= 3) ? 3 : 0;
					Run->PhaseDeadline = Now + 6.0;
					return true;
				}

				default:
					break;
				}

				ReportMortimerMantleRun(Run);

				// Put him back where he was found, so a test does not leave him on a block that is about
				// to be destroyed underneath him.
				Pawn->SetActorLocation(Run->Anchor, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
				EndMortimerMantleRun(Run);
				return false;
			}), 0.f);
	}

	FAutoConsoleCommand CmdMortimerMantleTest(
		TEXT("Trace.Mortimer.MantleTest"),
		TEXT("DEMO 21 item 6. Builds a ledge in front of Mortimer and presses the SHIPPED jump key three times: "
		     "with the mantle removed (must decline and must not get up), with it in place (must consume the "
		     "press and end STOOD on the lip), and at a ledge below his own jump apex (must leave the ordinary "
		     "jump alone). Destroys its ledge on every exit path."),
		FConsoleCommandDelegate::CreateStatic(&RunMortimerMantleTest));

	// =============================================================================================
	// Trace.Mortimer.ThrowTest — DEMO 21 ITEM 7, MEASURED AS DISTANCE AND NOT AS A MULTIPLIER
	//
	// FOUR REAL THROWS: {100% charge, full extended charge} x {item 7 removed, item 7 in place}.
	//
	// *** WHY DISTANCE HAS TO BE MEASURED SEPARATELY FROM SPEED. *** The charge scales the IMPULSE, so
	// it scales launch SPEED — and a flat-ground throw's range goes as the SQUARE of speed. Reporting
	// "x1.85 -> x1.51" as though it answered "how much further does he throw it" is the same class of
	// error this project already shipped twice (Roxie's "+15% jump HEIGHT" applied to velocity, and
	// Chut's "+25% distance" applied to speed for +65.8% distance). So both are printed, and the one
	// the item is about is the range.
	//
	// *** THE RANGE IS SOLVED FROM THE MEASURED LAUNCH, NOT STEPPED, AND HERE IS WHY THAT IS HONEST. ***
	// The loose Core is integrated in ATraceCore::ServerTickLooseCore as LooseVelocity += gravity x dt
	// and nothing else — no drag, no air resistance, no terminal velocity. So the flat-ground range of
	// a launch is exactly 2 x Vz x Vxy / |g|, and using it re-derives NOTHING about the rule under
	// test: the launch velocity is read off the Core AFTER the shipped ThrowFromHolder() has thrown it,
	// and |g| is ATraceCore::GetThrowGravityZ(), the same accessor the integrator uses.
	//
	// The alternative — flying each Core until it comes down — cannot be done in an arena: at x1.85 a
	// full extended throw's flat range is tens of thousands of uu and every arm would report the
	// distance to the same wall. The distance actually travelled IS still measured and printed, as
	// corroboration that a real Core really left his hand in each arm.
	// =============================================================================================

	struct FMortimerThrowArm
	{
		float HoldSeconds = 0.f;
		bool  bPastFullLive = false;
		float ChargeScale = -1.f;
		float LaunchSpeed = -1.f;
		float LaunchVxy = 0.f;
		float LaunchVz = 0.f;
		float FlatRangeUU = -1.f;
		float TravelledUU = 0.f;
		bool  bThrown = false;
	};

	struct FMortimerThrowRun
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;
		TWeakObjectPtr<ATraceCore> CoreActor;

		int32 Phase = 0;
		int32 ArmIndex = 0;
		double PhaseDeadline = 0.0;
		double WatchUntil = 0.0;

		FVector Anchor = FVector::ZeroVector;
		FVector LaunchPoint = FVector::ZeroVector;
		float GravityDown = 539.f;

		FMortimerThrowArm Arms[4];
		int32 SavedThrowArm = 1;
	};

	void EndMortimerThrowRun(FMortimerThrowRun* Run)
	{
		if (IConsoleVariable* const Arm =
			IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.ThrowPastFull")))
		{
			Arm->Set(Run->SavedThrowArm, ECVF_SetByConsole);
		}
		delete Run;
	}

	void ReportMortimerThrowRun(const FMortimerThrowRun* Run)
	{
		const TCHAR* const Tag = TEXT("MORTIMERTHROW");

		static const TCHAR* const Names[4] = {
			TEXT("RED   100%  (item 7 removed)"),
			TEXT("RED   FULL  (item 7 removed)"),
			TEXT("GREEN 100%  (shipped)"),
			TEXT("GREEN FULL  (shipped)") };

		for (int32 Index = 0; Index < 4; ++Index)
		{
			const FMortimerThrowArm& Arm = Run->Arms[Index];
			UE_LOG(LogTraceGame, Display,
				TEXT("[%s] arm=%s  held %.3fs -> charge x%.3f  launch %.0f uu/s (xy %.0f, z %+.0f)  "
				     "FLAT RANGE %.0f uu  | travelled %.0f uu before it met the arena"),
				Tag, Names[Index], Arm.HoldSeconds, Arm.ChargeScale, Arm.LaunchSpeed, Arm.LaunchVxy,
				Arm.LaunchVz, Arm.FlatRangeUU, Arm.TravelledUU);
		}

		const FMortimerThrowArm& RedHundred   = Run->Arms[0];
		const FMortimerThrowArm& RedFull      = Run->Arms[1];
		const FMortimerThrowArm& GreenHundred = Run->Arms[2];
		const FMortimerThrowArm& GreenFull    = Run->Arms[3];

		if (!RedHundred.bThrown || !RedFull.bThrown || !GreenHundred.bThrown || !GreenFull.bThrown)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — not every arm produced a throw (%d %d %d %d). The Core was "
				     "probably caught, or the throw cooldown swallowed a press."),
				Tag, RedHundred.bThrown ? 1 : 0, RedFull.bThrown ? 1 : 0, GreenHundred.bThrown ? 1 : 0,
				GreenFull.bThrown ? 1 : 0);
			return;
		}

		// 1. "Up to 100% he scales like everyone else." The two 100% arms differ only in the rule under
		//    test, so if the rule leaked into the first half of the curve they would separate.
		const bool bHundredUnchanged = FMath::IsNearlyEqual(RedHundred.FlatRangeUU, GreenHundred.FlatRangeUU,
			FMath::Max(1.f, RedHundred.FlatRangeUU * 0.01f));

		// 2. THE HARNESS MUST BE ABLE TO GO RED. If the two FULL arms do not separate, item 7 is not
		//    routed and every other number here is a coincidence.
		const bool bRedSeparated = RedFull.FlatRangeUU > GreenFull.FlatRangeUU * 1.05f;

		if (!bHundredUnchanged)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[%s] VERDICT: *** FAIL *** — the 100%% charge MOVED (%.0f uu red, %.0f uu green). Demo 21 "
				     "item 7 is only allowed to touch charge PAST the original 100%% point, and it has reached "
				     "into the half of the curve every other character shares."),
				Tag, RedHundred.FlatRangeUU, GreenHundred.FlatRangeUU);
			return;
		}

		if (!bRedSeparated)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[%s] VERDICT: INVALID — the two FULL-charge arms did not separate (%.0f uu vs %.0f uu). "
				     "Either item 7 is not routed through ATraceCore::GetThrowChargeScaleForHold, or "
				     "Trace.Mortimer.ThrowPastFull is not reaching it. A harness that cannot go red has proved "
				     "nothing."),
				Tag, RedFull.FlatRangeUU, GreenFull.FlatRangeUU);
			return;
		}

		const float RangeRatioBefore = RedFull.FlatRangeUU / FMath::Max(1.f, RedHundred.FlatRangeUU);
		const float RangeRatioAfter  = GreenFull.FlatRangeUU / FMath::Max(1.f, GreenHundred.FlatRangeUU);

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] VERDICT: PASS — 100%% charge is UNCHANGED (%.0f uu before, %.0f uu after). His FULL "
			     "extended charge goes %.0f uu -> %.0f uu, i.e. from %.2fx a full-charge throw's range to "
			     "%.2fx it. Launch speed went x%.3f -> x%.3f; the range moved by the SQUARE of that, which is "
			     "the whole reason this command measures distance."),
			Tag, RedHundred.FlatRangeUU, GreenHundred.FlatRangeUU, RedFull.FlatRangeUU, GreenFull.FlatRangeUU,
			RangeRatioBefore, RangeRatioAfter, RedFull.ChargeScale, GreenFull.ChargeScale);
	}

	void RunMortimerThrowTest()
	{
		const TCHAR* const Tag = TEXT("MORTIMERTHROW");

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
				TEXT("[%s] VERDICT: INVALID — no authoritative game world. The throw is resolved on the "
				     "server; run this on the host."), Tag);
			return;
		}

		FString Why;
		UTraceAbilitySetMortimer* Mortimer = MakePlayerIntoMortimer(TestWorld, Why);
		ATraceCharacter* MyPawn = (Mortimer != nullptr) ? Mortimer->GetCharacter() : nullptr;
		ATraceCore* CoreActor = ATraceCore::Get(TestWorld);
		IConsoleVariable* const Arm =
			IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.ThrowPastFull"));
		if (Mortimer == nullptr || MyPawn == nullptr || CoreActor == nullptr || Arm == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[%s] VERDICT: INVALID — %s."), Tag,
				(Mortimer == nullptr) ? *Why
				                      : TEXT("Mortimer has no pawn, there is no Core in this world (run in "
				                             "mode B), or Trace.Mortimer.ThrowPastFull is not registered"));
			return;
		}

		const float FullHold = FMath::Max(0.01f, ATraceCore::GetThrowChargeSeconds());
		const float ExtendedHold = FullHold * FMath::Max(1.f, Mortimer->GetThrowChargeHoldScale());

		FMortimerThrowRun* Run = new FMortimerThrowRun();
		Run->Pawn = MyPawn;
		Run->CoreActor = CoreActor;
		Run->Anchor = MyPawn->GetActorLocation();
		Run->SavedThrowArm = Arm->GetInt();
		Run->GravityDown = FMath::Max(1.f, -ATraceCore::GetThrowGravityZ(TestWorld));
		Run->PhaseDeadline = FPlatformTime::Seconds() + 10.0;

		// RED FIRST, and within each arm the 100% throw before the extended one.
		Run->Arms[0] = { FullHold,     false };
		Run->Arms[1] = { ExtendedHold, false };
		Run->Arms[2] = { FullHold,     true  };
		Run->Arms[3] = { ExtendedHold, true  };

		UE_LOG(LogTraceGame, Display,
			TEXT("[%s] begin: four real throws by %s. A full charge is %.3fs, his extended ceiling is %.3fs. "
			     "Loose-Core gravity is %.0f uu/s^2 and the integrator has NO drag, so the flat range of a "
			     "launch is 2 x Vz x Vxy / g."),
			Tag, *GetNameSafe(MyPawn), FullHold, ExtendedHold, Run->GravityDown);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run, Tag](float /*Delta*/) -> bool
			{
				ATraceCharacter* Pawn = Run->Pawn.Get();
				ATraceCore* Core = Run->CoreActor.Get();
				IConsoleVariable* const ThrowArm =
					IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Mortimer.ThrowPastFull"));
				const double Now = FPlatformTime::Seconds();

				if (Pawn == nullptr || Core == nullptr || ThrowArm == nullptr)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[%s] VERDICT: INVALID — the pawn or the Core went away mid-run."), Tag);
					EndMortimerThrowRun(Run);
					return false;
				}
				if (Now > Run->PhaseDeadline)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[%s] VERDICT: INVALID — arm %d stalled in phase %d."), Tag, Run->ArmIndex, Run->Phase);
					EndMortimerThrowRun(Run);
					return false;
				}

				FMortimerThrowArm& CurrentArm = Run->Arms[Run->ArmIndex];

				switch (Run->Phase)
				{
				case 0:
				{
					ThrowArm->Set(CurrentArm.bPastFullLive ? 1 : 0, ECVF_SetByConsole);

					// STOOD STILL AND AIMED LEVEL, so spec v8 §4's inherited velocity is zero in every
					// arm and the only thing that differs between them is the charge. A moving thrower
					// would add his own motion to the launch and the four numbers would stop being
					// comparable.
					Pawn->SetActorLocation(Run->Anchor, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
					if (UCharacterMovementComponent* Move = Pawn->GetCharacterMovement())
					{
						Move->StopMovementImmediately();
						Move->Velocity = FVector::ZeroVector;
					}
					if (AController* Controller = Pawn->GetController())
					{
						Controller->SetControlRotation(FRotator(0.f, Pawn->GetActorRotation().Yaw, 0.f));
					}
					Core->TryPickup(Pawn);

					Run->WatchUntil = Now + 0.25;
					Run->PhaseDeadline = Now + 3.0;
					Run->Phase = 1;
					return true;
				}

				case 1:
				{
					if (Now < Run->WatchUntil)
					{
						return true;
					}

					// THE SHIPPED THROW, on the server, with the server's own hold. Nothing here
					// reimplements the launch: ThrowFromHolder calls GetThrowChargeScaleForHold with the
					// THROWER, which is the one call site item 7 lives at.
					if (!Core->ThrowFromHolder(Pawn, CurrentArm.HoldSeconds))
					{
						// Usually the per-throw cooldown (CoreThrowCooldownSeconds, 0.35 s after any
						// grant). Wait it out rather than failing the arm — but say WHY every second, or
						// a stall here reads as "the throw is broken" when it is a fixture problem.
						if (Now > Run->WatchUntil)
						{
							Run->WatchUntil = Now + 1.0;
							UE_LOG(LogTraceGame, Display,
								TEXT("[%s] arm %d: ThrowFromHolder refused. modeB=%d loose=%d carrier=%s "
								     "(wanted %s)."),
								Tag, Run->ArmIndex, Core->IsModeB() ? 1 : 0, Core->IsLoose() ? 1 : 0,
								*GetNameSafe(Core->GetCarrier()), *GetNameSafe(Pawn));
							Core->TryPickup(Pawn);
						}
						return true;
					}

					const FVector LaunchVelocity = Core->GetLooseVelocity();
					Run->LaunchPoint = Core->GetLooseLocation();

					CurrentArm.bThrown = true;
					CurrentArm.ChargeScale = ATraceCore::LastThrow.ChargeScale;
					CurrentArm.LaunchSpeed = static_cast<float>(LaunchVelocity.Size());
					CurrentArm.LaunchVxy = static_cast<float>(LaunchVelocity.Size2D());
					CurrentArm.LaunchVz = static_cast<float>(LaunchVelocity.Z);

					// Flat-ground range: the time to rise and fall back to the launch height is
					// 2 Vz / g, and the horizontal speed is constant because there is no drag.
					CurrentArm.FlatRangeUU = (CurrentArm.LaunchVz > 0.f)
						? (2.f * CurrentArm.LaunchVz * CurrentArm.LaunchVxy / Run->GravityDown)
						: 0.f;

					Run->WatchUntil = Now + 1.5;
					Run->PhaseDeadline = Now + 4.0;
					Run->Phase = 2;
					return true;
				}

				case 2:
				{
					// Corroboration only: proof that a real Core really left his hand and flew. It is
					// cut short by arena geometry, which is why the verdict rests on the flat range.
					if (Core->IsLoose())
					{
						CurrentArm.TravelledUU = FMath::Max(CurrentArm.TravelledUU,
							static_cast<float>(FVector::Dist2D(Core->GetLooseLocation(), Run->LaunchPoint)));
					}
					if (Now < Run->WatchUntil)
					{
						return true;
					}

					++Run->ArmIndex;
					Run->Phase = (Run->ArmIndex >= 4) ? 3 : 0;
					Run->PhaseDeadline = Now + 10.0;
					return true;
				}

				default:
					break;
				}

				ReportMortimerThrowRun(Run);
				EndMortimerThrowRun(Run);
				return false;
			}), 0.f);
	}

	FAutoConsoleCommand CmdMortimerThrowTest(
		TEXT("Trace.Mortimer.ThrowTest"),
		TEXT("DEMO 21 item 7. Throws four real Cores through the shipped ATraceCore::ThrowFromHolder — 100% and "
		     "full extended charge, with the 0.6x modifier removed and then in place — and reports the thrown "
		     "DISTANCE, not the speed multiplier. Asserts the 100% charge did not move and that the two "
		     "full-charge arms separate."),
		FConsoleCommandDelegate::CreateStatic(&RunMortimerThrowTest));

	FAutoConsoleCommand CmdMortimerQuakeTest(
		TEXT("Trace.Mortimer.QuakeTest"),
		TEXT("DEMO 20 item 3a. Presses E through the shipped UTraceAbilityComponent::TryActivate() three times — "
		     "with no Core, with the Core and the shockwave REMOVED (the bug, reproduced), and with the Core and "
		     "the shockwave in place — and says which of \"does nothing\" and \"does something invisible\" was "
		     "true. Then casts N more times (default 6, ~1.1s apart) so a screenshot can land on one."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunMortimerQuakeTest));
}

#undef LOCTEXT_NAMESPACE
