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

#include "Components/AudioComponent.h"      // the flight loop is faded out, not cut off
#include "Abilities/Characters/TraceAbilitySetRoxie.h"
#include "Audio/TraceAudio.h"                // §2.3's RoxieRocketLoop
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceFxBurst.h"           // §2.3's RocketBurst, at every one of the four endings
#include "Gameplay/TraceFxShapes.h"          // the flash and the trail are built through the library
#include "Gameplay/TraceTracer.h"            // the muzzle-flash timing this launch flash is modelled on
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

	// =============================================================================================
	// FX_AUDIO_PLAN §2.3 — THE FLASH AND THE TRAIL, EVERY SIZE AS A FRACTION OF ONE RADIUS
	//
	// *** READ TraceRoxieRocket::GetVisualRadiusUU()'s comment BEFORE ADDING A NUMBER HERE. *** Every
	// constant below is a MULTIPLE of the drawn body radius, never a literal in uu, because the owner
	// has a queued request to make the rocket model larger and this is what makes that a one-line
	// change instead of a hunt for four numbers that must all move together.
	//
	// The two exceptions are ceilings rather than sizes — the muzzle's 40 uu and the 8 uu emissive
	// width floor — and they belong to the bible rather than to the rocket, so they do NOT scale.
	// =============================================================================================

	/** §2.3's ember. One hue for the whole effect — body, flash and trail (bible §6.2 invariant 2). */
	const FLinearColor Ember(1.f, 0.45f, 0.12f, 1.f);

	/**
	 * The rocket body's emissive brightness, in the bible's Glow units.
	 *
	 * 4.0 is the value the Demo 17 code intended and never achieved (see BeginPlay), and it sits just
	 * under ART_BIBLE §6.2's 4.2 transient ceiling — asserted below rather than trusted.
	 */
	constexpr float BodyGlow = 4.f;
	static_assert(BodyGlow <= 4.2f,
		"ART_BIBLE 6.2: an FX transient's Glow never exceeds the smear-head precedent of 4.2.");

	/**
	 * The launch flash's radius AT FULL GROWTH, as a fraction of the body radius — subject to the
	 * bible §6.4 muzzle ceiling below. 0.9 x 45 uu = 40.5 uu, i.e. the ceiling is what actually
	 * decides it at the shipped size, which is the correct relationship: a flash on the scale of the
	 * projectile, capped by the rule that caps every muzzle in the game.
	 */
	constexpr float FlashPeakRadiusPerBodyRadius = 0.9f;

	/** ART_BIBLE §6.4, "Muzzle ... <= 40 uu". Not scaled by the rocket's size; it is a rule. */
	constexpr float MaxMuzzleRadiusUU = 40.f;

	/** The cone's length as a multiple of its own peak radius. Stubby: a flash, not a jet. */
	constexpr float FlashLengthPerRadius = 1.6f;

	/** §2.3: the trail is 220 uu long behind a 45 uu rocket. 220 / 45 = 4.889. */
	constexpr float TrailLengthPerBodyRadius = 220.f / 45.f;

	/** §2.3: "r 9 -> 2 uu" behind a 45 uu rocket. 9 / 45 and 2 / 45. */
	constexpr float TrailStartRadiusPerBodyRadius = 9.f / 45.f;
	constexpr float TrailEndRadiusPerBodyRadius = 2.f / 45.f;

	/** §2.3: "additive ember I 0.5, flicker I 0.4-0.6 @ 30 Hz". */
	constexpr float TrailIntensity = 0.5f;
	constexpr float TrailFlickerAmplitude = 0.1f;
	constexpr float TrailFlickerHz = 30.f;

	/**
	 * The launch flash's brightness. Additive, so this is literally "how much ember is added to what
	 * is behind it" and it is clamped at 1.0 by the material; 0.8 leaves headroom so the flash reads
	 * as bright without becoming a white disc at point-blank range.
	 */
	constexpr float FlashIntensity = 0.8f;
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

	float GetVisualRadiusUU()
	{
		// THE ONE SIZE. Both terms are live clamped reads, so a retune during PIE reaches the next
		// rocket — body, flash and trail together, because all three are expressed in this.
		return GetHitRadiusUU() * GetVisualScale();
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
	// thing coming at them.
	//
	// *** THIS BLOCK USED TO WRAP THE CONE'S OWN DEFAULT MATERIAL, AND THE ROCKET WAS THEREFORE
	// DARK. *** It read `Body->CreateDynamicMaterialInstance(0, Body->GetMaterial(0))` and then wrote
	// four parameter names — "EmissiveColor", "Glow", "EmissiveStrength", "EmissivePower" — on the
	// reasoning that "setting one a material does not have is a documented silent no-op, so the
	// rocket glows on M_TraceNeon and is merely ember on the BasicShapes fallback". The premise was
	// wrong: /Engine/BasicShapes/Cone SHIPS with BasicShapeMaterial, so the fallback was the ONLY
	// path this code ever took and all four writes were no-ops. Demo 17 asked for a rocket that is
	// easy to see and on a black arena floor a lit matte cone is the opposite; it is photographed as
	// a near-black blob with a bright trail behind it in this tranche's first capture run.
	//
	// Through UTraceFxShapes now, like the flash and the trail: the EMISSIVE parent is asked for by
	// name, the achieved blend is stored, and SetGlow writes brightness on the scalar (never folded
	// into the colour, which a material instance clamps to 1 and which renders as flat white).
	if (Body != nullptr)
	{
		BodyMID = UTraceFxShapes::MakeGlowMID(Body, 0, ETraceFxBlend::Emissive, BodyBlend);
		if (BodyMID != nullptr)
		{
			UTraceFxShapes::SetGlow(BodyMID, BodyBlend, TraceRoxieRocketFile::Ember,
				TraceRoxieRocketFile::BodyGlow);
		}
		if (BodyBlend == ETraceFxBlend::None)
		{
			// Nothing resolved at all. The rocket is INVISIBLE rather than default-grey, which is the
			// bible's ladder — and the damage is unaffected, because the hit test is arithmetic on a
			// path function and has never had anything to do with what is drawn.
			Body->SetVisibility(false, true);
		}
	}

	BuildFlightFx();

	// *** THE FIRST PLACEMENT IS SPLIT BY MACHINE, AND A MEASUREMENT IS WHY. ***
	//
	// On the AUTHORITY, BeginPlay runs INSIDE SpawnActor — before UTraceAbilitySetRoxie::SpawnRocket
	// gets the pointer back and calls InitialiseFlight. So at this instant LaunchMatchTime is still
	// 0, GetSecondsInFlight() returns the whole match clock, and anything driven off it believes the
	// rocket is minutes old. Trace.Roxie.RocketFxTest caught exactly that: the launch flash reached
	// its 0.28 s expiry and DESTROYED ITSELF during BeginPlay, so the shipped build had a flash that
	// no player could ever have seen and a log that said nothing was wrong.
	//
	// A CLIENT is the opposite case and needs this call: its four launch values arrive with the
	// initial bunch, before BeginPlay, and there is no InitialiseFlight on that machine at all.
	//
	// So: the client places from here, the server places at the end of InitialiseFlight, and neither
	// ever places from a launch time that has not been written yet.
	if (!HasAuthority())
	{
		UpdateVisual(GetCurrentPosition());
		UpdateFlightFx(GetCurrentPosition(), GetSecondsInFlight());
	}

	// --- FX_AUDIO_PLAN §2.3 / §1.6.4: the flight loop, LOCAL, on every machine ---------------------
	//
	// *** NO RPC, AND THAT IS THE WHOLE REASON IT IS HERE RATHER THAN AT THE LAUNCH SITE. *** This
	// actor is replicated, so its BeginPlay runs on every machine that can see the rocket — the
	// actor's own replication IS the multicast. RoxieRocketLoop is declared CLIENT-side in
	// TraceSoundEvents.cpp precisely so that a stray TraceAudio::Play() on it can never multicast a
	// second copy over the top of these (§8.7's double-audio rule).
	//
	// Attached to the ROOT, which UpdateVisual moves every frame along the same derived path the
	// hit test uses, so the sound travels with the thing that kills you rather than with a
	// stationary point behind it.
	FlightLoop = TraceAudio::StartLoopOn(GetRootComponent(), TraceSoundEvents::RoxieRocketLoop);
}

// =================================================================================================
// FX_AUDIO_PLAN §2.3 — THE LAUNCH FLASH AND THE TRAIL
//
// Both are built on EVERY MACHINE in BeginPlay and driven from Tick, and neither replicates a byte.
// They can be, because the position they hang off is DERIVED: GetPositionAtTime() is a pure function
// of four values replicated once plus the match clock, so a client's trail is behind the same rocket
// at the same instant as the server's. That is the same argument the flight itself is built on and
// it is why "the drawn path is the lethal path" survives the decoration being added to it.
// =================================================================================================

void ATraceRoxieRocket::BuildFlightFx()
{
	USceneComponent* const RocketRoot = GetRootComponent();
	if (RocketRoot == nullptr)
	{
		return;
	}

	UStaticMesh* const Cone = UTraceFxShapes::GetCone();
	UStaticMesh* const Cylinder = UTraceFxShapes::GetCylinder();

	// ---- the launch flash --------------------------------------------------------------------
	if (Cone != nullptr && LaunchFlash == nullptr)
	{
		LaunchFlash = NewObject<UStaticMeshComponent>(this, TEXT("RocketLaunchFlash"));
		if (LaunchFlash != nullptr)
		{
			LaunchFlash->SetupAttachment(RocketRoot);
			LaunchFlash->SetStaticMesh(Cone);
			UTraceFxShapes::ConfigureFxComponent(LaunchFlash);
			LaunchFlash->RegisterComponent();

			// ADDITIVE, and it must not degrade onto an opaque rung: a large opaque cone at the
			// muzzle writes depth and punches a hole in the arena behind it, and a faded-out opaque
			// cone is a dark matte disc rather than nothing at all. This is ATraceTracer's own rule
			// for its halo and muzzle pieces, applied here for the identical measured reason.
			LaunchFlashMID = UTraceFxShapes::MakeGlowMID(LaunchFlash, 0, ETraceFxBlend::Additive,
				LaunchFlashBlend);
			if (LaunchFlashBlend == ETraceFxBlend::None || LaunchFlashBlend == ETraceFxBlend::Emissive
				|| LaunchFlashBlend == ETraceFxBlend::Fallback)
			{
				// Nothing additive resolved. HIDE IT rather than show a grey 100 uu cone: bible §6.1's
				// degradation ladder ends at "no effect", never at "the wrong effect".
				LaunchFlash->SetVisibility(false, true);
			}
		}
	}

	// ---- the three trail segments ------------------------------------------------------------
	if (Cylinder != nullptr && TrailSegments[0] == nullptr)
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(TrailSegments); ++Index)
		{
			UStaticMeshComponent* Segment = NewObject<UStaticMeshComponent>(this,
				*FString::Printf(TEXT("RocketTrail%d"), Index));
			if (Segment == nullptr)
			{
				continue;
			}

			Segment->SetupAttachment(RocketRoot);
			Segment->SetStaticMesh(Cylinder);
			UTraceFxShapes::ConfigureFxComponent(Segment);
			Segment->RegisterComponent();

			ETraceFxBlend Achieved = ETraceFxBlend::None;
			TrailMIDs[Index] = UTraceFxShapes::MakeGlowMID(Segment, 0, ETraceFxBlend::Additive, Achieved);
			if (Index == 0)
			{
				TrailBlend = Achieved;
			}
			if (Achieved == ETraceFxBlend::None)
			{
				Segment->SetVisibility(false, true);
			}

			TrailSegments[Index] = Segment;
		}
	}

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Roxie] rocket FX built: body r %.1f uu, flash peak r %.1f uu (%s), trail %.0f uu long "
		     "r %.1f -> %.1f uu (%s)."),
		TraceRoxieRocket::GetVisualRadiusUU(),
		FMath::Min(TraceRoxieRocket::GetVisualRadiusUU() * TraceRoxieRocketFile::FlashPeakRadiusPerBodyRadius,
			TraceRoxieRocketFile::MaxMuzzleRadiusUU),
		UTraceFxShapes::BlendName(LaunchFlashBlend),
		TraceRoxieRocket::GetVisualRadiusUU() * TraceRoxieRocketFile::TrailLengthPerBodyRadius,
		FMath::Max(TraceRoxieRocket::GetVisualRadiusUU() * TraceRoxieRocketFile::TrailStartRadiusPerBodyRadius,
			ATraceFxBurst::MinEmissiveRadiusUU),
		FMath::Max(TraceRoxieRocket::GetVisualRadiusUU() * TraceRoxieRocketFile::TrailEndRadiusPerBodyRadius,
			ATraceFxBurst::MinEmissiveRadiusUU),
		UTraceFxShapes::BlendName(TrailBlend));
}

void ATraceRoxieRocket::UpdateFlightFx(const FVector& AtPosition, float SecondsInFlight)
{
	const float BodyRadius = TraceRoxieRocket::GetVisualRadiusUU();

	// The instantaneous direction of travel, sampled the same way UpdateVisual samples it (one extra
	// evaluation of the path function, no derivative) so the trail lies along the arc the rocket is
	// actually flying rather than along the launch line it left 200 uu ago.
	const FVector Ahead = TraceRoxieRocket::GetPositionAtTime(LaunchOrigin, LaunchDirection,
		SecondsInFlight + 0.02f, TraceRoxieRocket::GetSpeedUU(), TraceRoxieRocket::GetWobbleAmplitudeUU(),
		TraceRoxieRocket::GetWobbleFrequencyHz(), WobbleSeedTurns);
	FVector Travel = (Ahead - AtPosition).GetSafeNormal();
	if (Travel.IsNearlyZero())
	{
		Travel = FVector(LaunchDirection).GetSafeNormal(1.e-4f, FVector::ForwardVector);
	}

	// ---- the launch flash: 0.55x -> 3.2x over 0.28 s, then GONE -------------------------------
	if (LaunchFlash != nullptr)
	{
		// *** THE AGE IS THE ROCKET'S OWN, AND THERE IS DELIBERATELY NO SECOND GUARD ON IT. ***
		//
		// The first version of this fix added a "latch the age on the first update" belt, and it was
		// WRONG for the case it looked like it protected: a client that becomes relevant to a rocket
		// already one second into its flight would then be shown a fresh 0.28 s launch flash at a
		// muzzle the rocket left long ago. The plain comparison is what makes a late joiner destroy
		// the flash on its very first frame, which is the correct answer.
		//
		// The only way this can be handed a nonsense age is a caller that runs before the launch time
		// is written, and that is fixed where it happens — see the split in BeginPlay — rather than
		// papered over here with a clause that would read as protection while providing none.
		if (SecondsInFlight >= ATraceTracer::MuzzleFlashSeconds)
		{
			// DESTROYED, not hidden. See the header: the steady-state rocket is Body + three trail
			// segments = the bible's four primitives, and a permanently-hidden fifth would leave it
			// permanently at five for nothing.
			LaunchFlash->DestroyComponent();
			LaunchFlash = nullptr;
			LaunchFlashMID = nullptr;
		}
		else
		{
			// THE SAME THREE NUMBERS THE GUN'S OWN MUZZLE FLASH USES, referenced rather than copied:
			// §2.3 asks for "the tracer muzzle precedent" by name, and two files holding 0.28 / 0.55 /
			// 3.2 separately is two files that can be retuned apart without anybody noticing.
			const float Alpha = FMath::Clamp(SecondsInFlight / ATraceTracer::MuzzleFlashSeconds, 0.f, 1.f);
			const float Growth = FMath::Lerp(ATraceTracer::MuzzleFlashStartScale,
				ATraceTracer::MuzzleFlashEndScale, Alpha);

			// PEAK first, then the base that grows into it — so the CEILING is on the biggest the
			// cone ever gets rather than on the size it starts at.
			const float PeakRadius = FMath::Min(BodyRadius * TraceRoxieRocketFile::FlashPeakRadiusPerBodyRadius,
				TraceRoxieRocketFile::MaxMuzzleRadiusUU);
			const float Radius = (PeakRadius / ATraceTracer::MuzzleFlashEndScale) * Growth;
			const float Length = Radius * TraceRoxieRocketFile::FlashLengthPerRadius;

			// PINNED TO THE MUZZLE, in world space, every frame. A muzzle flash belongs to the tube:
			// carrying it along on the rocket's nose would read as an engine glow, which is a
			// different (and permanent) effect from the one §2.3 asks for.
			LaunchFlash->SetWorldLocationAndRotation(FVector(LaunchOrigin),
				(FVector(LaunchDirection).Rotation() + FRotator(90.f, 0.f, 0.f)).Quaternion());
			LaunchFlash->SetWorldScale3D(FVector(
				UTraceFxShapes::ShapeScaleForRadiusUU(Radius),
				UTraceFxShapes::ShapeScaleForRadiusUU(Radius),
				UTraceFxShapes::ShapeScaleForLengthUU(Length)));

			// FADES AS IT GROWS, which is the monotonic pairing every transient in this project uses
			// (sizes grow, brightness falls): the flash never brightens, so it can never be mistaken
			// for a state that is coming ON.
			UTraceFxShapes::SetGlow(LaunchFlashMID, LaunchFlashBlend, TraceRoxieRocketFile::Ember,
				TraceRoxieRocketFile::FlashIntensity, 1.f - Alpha);
		}
	}

	// ---- the trail: three tapered segments behind the nose ------------------------------------
	if (TrailSegments[0] != nullptr)
	{
		const float TrailLength = BodyRadius * TraceRoxieRocketFile::TrailLengthPerBodyRadius;

		// FLOORED AT THE 8 uu EMISSIVE WIDTH (bible §3.4). §2.3 asks for 2 uu at the tail; 2 uu is
		// 4 uu across and dissolves into dashes under TSR at arena range, which is the exact failure
		// ATraceFxBurst::MinEmissiveRadiusUU exists to stop. Same floor, same constant, one reason.
		const float StartRadius = FMath::Max(BodyRadius * TraceRoxieRocketFile::TrailStartRadiusPerBodyRadius,
			ATraceFxBurst::MinEmissiveRadiusUU);
		const float EndRadius = FMath::Max(BodyRadius * TraceRoxieRocketFile::TrailEndRadiusPerBodyRadius,
			ATraceFxBurst::MinEmissiveRadiusUU);

		// The trail starts where the rocket IS and runs backwards. Clamped at the muzzle so a rocket
		// 100 uu into its flight has a 100 uu trail rather than one that reaches back through the
		// wall behind Roxie's head.
		const float TravelledSoFar = FMath::Max(0.f, SecondsInFlight) * TraceRoxieRocket::GetSpeedUU();
		const FVector TrailEnd = AtPosition - Travel * FMath::Min(TrailLength, TravelledSoFar);

		UStaticMeshComponent* const Segments[3] = { TrailSegments[0], TrailSegments[1], TrailSegments[2] };
		UTraceFxShapes::TaperBetween(MakeArrayView(Segments, UE_ARRAY_COUNT(Segments)),
			AtPosition, TrailEnd, StartRadius, EndRadius);

		// §2.3's flicker: 0.4-0.6 at 30 Hz, on the MATCH clock so every machine flickers in step.
		// A TRANSIENT FLICKER, NOT A LETHAL-TELEGRAPH PULSE (§3.3): the lethal element is the BODY,
		// whose size is the hit radius and whose brightness never moves. This is exhaust.
		const float Flicker = TraceRoxieRocketFile::TrailIntensity
			+ TraceRoxieRocketFile::TrailFlickerAmplitude
			  * FMath::Sin(2.f * PI * TraceRoxieRocketFile::TrailFlickerHz * SecondsInFlight);

		for (int32 Index = 0; Index < UE_ARRAY_COUNT(TrailMIDs); ++Index)
		{
			UTraceFxShapes::SetGlow(TrailMIDs[Index], TrailBlend, TraceRoxieRocketFile::Ember, Flicker);
		}
	}
}

FString ATraceRoxieRocket::DebugDescribeFx() const
{
	// EVERY NUMBER IS READ BACK OFF A LIVE COMPONENT'S SCALE, through the inverse of the conversion
	// that wrote it. Nothing here consults TraceRoxieRocketFile.
	const float BodyRadius = (Body != nullptr)
		? (Body->GetComponentScale().X * UTraceFxShapes::BasicShapeExtentUU * 0.5f) : 0.f;

	const float FlashRadius = (LaunchFlash != nullptr)
		? UTraceFxShapes::RadiusUUFromShapeScale(LaunchFlash->GetComponentScale().X) : 0.f;

	int32 TrailPieces = 0;
	int32 TrailVisible = 0;
	float TrailNear = 0.f;
	float TrailFar = 0.f;
	float TrailLength = 0.f;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(TrailSegments); ++Index)
	{
		const UStaticMeshComponent* Segment = TrailSegments[Index];
		if (Segment == nullptr)
		{
			continue;
		}
		++TrailPieces;
		TrailVisible += Segment->IsVisible() ? 1 : 0;

		const FVector Scale = Segment->GetComponentScale();
		const float Radius = UTraceFxShapes::RadiusUUFromShapeScale(Scale.X);
		TrailLength += UTraceFxShapes::LengthUUFromShapeScale(Scale.Z);
		if (Index == 0) { TrailNear = Radius; }
		TrailFar = Radius;
	}

	// The primitive count a bible reviewer would count on screen: the body, the trail segments, and
	// the flash while it is still there.
	const int32 Primitives = ((Body != nullptr) ? 1 : 0) + TrailPieces + ((LaunchFlash != nullptr) ? 1 : 0);

	return FString::Printf(
		TEXT("t=%.2fs primitives=%d | body r=%.1fuu (%s) | flash %s r=%.1fuu (%s) | trail %d/%d visible "
		     "r=%.1f->%.1fuu len=%.0fuu (%s)"),
		GetSecondsInFlight(), Primitives, BodyRadius, UTraceFxShapes::BlendName(BodyBlend),
		(LaunchFlash != nullptr) ? TEXT("UP") : TEXT("gone"), FlashRadius,
		UTraceFxShapes::BlendName(LaunchFlashBlend),
		TrailVisible, TrailPieces, TrailNear, TrailFar, TrailLength,
		UTraceFxShapes::BlendName(TrailBlend));
}

void ATraceRoxieRocket::DetonateAndDestroy(const FVector& Location, const FVector& Normal)
{
	if (!HasAuthority())
	{
		return;
	}

	// RADIUS 0 = "the type's own default", and for RocketBurst that default is READ LIVE from
	// RoxieRocketHitRadiusUU — the identical clamped read ApplyRocketDamageTo's own hit test makes
	// (ATraceFxBurst::DefaultRadiusUUFor). Passing a number from here would be exactly the copy the
	// drawn-equals-lethal invariant forbids, so this deliberately passes nothing.
	//
	// It also carries the RoxieRocketBurst sound with it, locally on every machine, through the
	// burst's own PlayReplicatedLocal — which is why nothing here plays a sound.
	ATraceFxBurst::Burst(GetWorld(), ETraceFxBurstType::RocketBurst, Location,
		Normal.GetSafeNormal(1.e-4f, FVector::UpVector));

	Destroy();
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

	// THE SERVER'S FIRST PLACEMENT, on the same frame the launch facts become true. See BeginPlay for
	// why it cannot be done there: this is the first instant on the authority at which the flash's
	// age is a real number rather than the whole match clock.
	UpdateVisual(LaunchOrigin);
	UpdateFlightFx(LaunchOrigin, 0.f);
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

	// *** DEMO 17 item 3: "the model gets BIGGER so it is easy to see."
	// *** PATCH 28 item 1: "Make Roxie's rocket larger." RoxieRocketVisualScale 1.0 -> 1.6. ***
	//
	// The size is DERIVED FROM THE HIT RADIUS rather than being a second free number, and that is the
	// whole point of this function. The rocket kills anything whose capsule comes within
	// RoxieRocketHitRadiusUU of the path; drawing a 13 uu dart around a 45 uu touch radius meant the
	// thing a player was dodging was three and a half times wider than the thing they could see — which
	// is the "the lethal volume is not the drawn volume" defect this project already spent a pass
	// removing from the ribbon.
	//
	// *** THE BODY IS NOW WIDER THAN THE TOUCH RADIUS, DELIBERATELY, AND HERE IS WHY THAT IS STILL
	// *** DRAWN == LETHAL. *** The touch radius is not the volume a PLAYER is killed in: the body test
	// below (ApplyRocketDamageTo's segment-vs-segment) compares against HitRadius + the victim's own
	// CAPSULE RADIUS, which is 45 + 34 = 79 uu at the shipped numbers. A 72 uu drawn body is inside
	// that, so the ember shape a player watches still cannot claim a kill the rocket does not have.
	// 79/45 = 1.76 is the ceiling on RoxieRocketVisualScale and Trace.Roxie.RocketFlightTest asserts
	// it against the LIVE capsule, so a capsule or hit-radius retune moves the ceiling with it.
	//
	// The one place drawn and lethal genuinely differ is the WORLD sweep, which is a 45 uu sphere: a
	// 72 uu cone touches a wall about 27 uu before that sphere does. Harmless direction, stated on the
	// knob, not a second literal — both numbers still come from GetVisualRadiusUU / GetHitRadiusUU.
	//
	// RoxieRocketVisualScale is the tuning dial on top of that; 1.0 would be "draw the touch radius".
	// /Engine/BasicShapes/Cone is 100 uu across and 100 uu tall, so a scale of 1 is 100 uu.
	// ONE READ, and every other piece of the rocket's presentation is a fraction of the same number
	// (see TraceRoxieRocket::GetVisualRadiusUU). This used to multiply the two knobs inline; it now
	// asks the accessor, so there is exactly one place the drawn size is decided.
	const float Radius = TraceRoxieRocket::GetVisualRadiusUU();
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

	// EVERY MACHINE. The flash and the trail hang off the same derived position the hit test uses,
	// so they are correct on a lagged client without a byte of per-frame replication — and they are
	// updated BEFORE the authority gate for exactly that reason.
	UpdateFlightFx(Position, SecondsInFlight);

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
			// NO BURST HERE, deliberately — see DetonateAndDestroy's comment. This only fires in a
			// world whose match clock never became usable, i.e. for a rocket that never really flew;
			// a detonation would be advertising an event that did not happen.
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

		// §2.3's third ending. The normal is the reverse of travel, because there is no surface here
		// — a rocket that ran out of range detonates in mid-air facing back the way it came, which is
		// what puts the burst's ring across the flight path rather than flat on nothing.
		const FVector Backwards = (LastSweptPosition - Position).GetSafeNormal(1.e-4f, FVector::UpVector);
		DetonateAndDestroy(Position, Backwards);
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
		//
		// NO BURST. A fizzle is the ABSENCE of a detonation: this rocket can no longer deal damage to
		// anybody, and §2.3's burst is the presentation of damage having happened. Drawing one would
		// be the drawn-equals-lethal invariant broken in the one direction nobody checks for — an
		// effect claiming an event that did not occur.
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

	// The surface the burst lies on. Defaulted to "back down the flight line" so that the one case
	// with no real normal (bStartPenetrating — Roxie fired with her muzzle inside geometry) still
	// orients the ring at something rather than at a zero vector.
	FVector WallNormal = (FromPosition - ToPosition).GetSafeNormal(1.e-4f, FVector::UpVector);

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
		if (!WorldHit.bStartPenetrating && !WorldHit.ImpactNormal.IsNearlyZero())
		{
			WallNormal = WorldHit.ImpactNormal;
		}
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

			// §2.3's first ending. The burst goes at the CONTACT POINT on the path — not at the
			// victim's origin, which is between their feet — and its normal faces back down the
			// flight line, so the shell grows out of where the rocket actually touched them.
			const FVector Backwards = (FromPosition - ToPosition).GetSafeNormal(1.e-4f, FVector::UpVector);
			DetonateAndDestroy(Entry.ImpactPoint, Backwards);
			return;
		}
	}

	if (WallDistance < TNumericLimits<float>::Max())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Roxie] rocket struck geometry at (%s) after %.2fs."),
			*WallImpact.ToCompactString(), GetSecondsInFlight());

		// §2.3's second ending, and the one case where a real SURFACE NORMAL exists: RocketBurst lays
		// its ring flat on this and sprays its eight spokes off it, so the blast reads as coming out
		// of the wall rather than hanging in front of it.
		DetonateAndDestroy(WallImpact, WallNormal);
	}
}
