// Trace — Oyster's jar. See the header: Pickler is a normal jar with a one-shot landing effect.

#include "Abilities/Characters/TraceOysterJar.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                 // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId::Oyster — the id, not the colour
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Audio/TraceAudio.h"
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterRoster.h"     // THE accent. See JarColor() below.
#include "Gameplay/TraceFxBurst.h"
#include "Gameplay/TraceFxShapes.h"
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

#if !UE_BUILD_SHIPPING
/**
 * DEV ONLY. Overrides the jar body's Glow so the value above can be MEASURED off frames rather than
 * argued about. 0 = use the shipped constant. Latched per jar at build, exactly as ATraceFxBurst
 * latches its hue headroom — read every frame instead, four jars staged at four values would all
 * re-write themselves to whatever the CVar said last and produce four identical rungs.
 */
static TAutoConsoleVariable<float> CVarOysterJarGlow(
	TEXT("Trace.Oyster.JarGlow"), 0.f,
	TEXT("DEV ONLY. >0 overrides the Glow a NEWLY BUILT Oyster jar is tinted at (the collar follows by "
	     "the fixed lip-to-face ratio). 0 = the shipped, measured value. Used by Trace.Oyster.JarLadder."),
	ECVF_Cheat);
#endif

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

	// =============================================================================================
	// FX_AUDIO_PLAN §2.6 — THE JAR'S LOOK. Four numbers, and every one of them is argued.
	// =============================================================================================

	/**
	 * Oyster's accent — READ FROM THE ROSTER, NOT COPIED, AND THAT IS A BUG FIX AND NOT A REFACTOR.
	 *
	 * THE JAR WEARS THE ACCENT AND THE CLOUD WEARS THE SEMANTIC HUE, and they are different colours on
	 * purpose. §6.2's hue priority is "semantic beats team beats accent": an intact jar is Oyster's
	 * object and says so in HIS colour; the moment it breaks, what is standing there is POISON and it
	 * is poison green (ATraceOysterPoisonCloud::CloudColor). One hue per effect, two effects.
	 *
	 * *** THIS LINE READ `const FLinearColor JarColor(0.30f, 0.85f, 0.95f, 1.f)` AND CALLED ITSELF
	 * "Oyster's accent, cyan". IT HAD NOT BEEN HIS ACCENT FOR A WHOLE WAVE. *** When the ten accents
	 * were re-spaced away from the two team hues, Oyster moved from cyan #95EDF9 (sRGB hue 187.1) to
	 * deep sea green #6FE5A2 (hue 145.8) — 41.3 degrees — and this copy did not move with him. His
	 * body wore the green and the object with his name on it lay on the floor in last palette's cyan.
	 * The sentence that claimed otherwise is the reason nobody noticed: a constant that "is" a fact
	 * goes stale silently, because nothing is watching it. So the answer is not a better literal, it
	 * is no literal. Same rule and same shape as the drawn-vs-lethal rule — never a second copy of a
	 * number something else owns.
	 *
	 * TraceCharacterRoster is the owner: it is the table the ten UTraceCharacterDefinition assets are
	 * generated from and the table the body materials are stamped from, so "equals the roster row" is
	 * literally "equals Oyster's body". Falls back to white on a roster that did not resolve, which is
	 * loud rather than subtly wrong. Called twice per jar build, never per frame.
	 */
	FLinearColor JarColor()
	{
		if (const TraceCharacterRoster::FTraceCharacterEntry* Row =
			TraceCharacterRoster::Find(static_cast<uint8>(ETraceCharacterId::Oyster)))
		{
			return FLinearColor(Row->Accent.R, Row->Accent.G, Row->Accent.B, 1.f);
		}
		return FLinearColor::White;
	}

	/**
	 * The hue headroom, mirrored from TraceFxBurstFile::EmissiveHueHeadroom.
	 *
	 * *** A SECOND COPY OF A NUMBER, AND IT IS DELIBERATE BECAUSE C++ LEAVES NO ALTERNATIVE HERE. ***
	 * The constant lives in a file-local namespace inside TraceFxBurst.cpp and there is no accessor on
	 * TraceFxBurst.h, so the jar cannot read it; adding one would put a new symbol on a header half the
	 * module includes for the sake of one float. What guards it instead is MECHANISED, not a promise:
	 * release-impl scratch tool w7-looseends/verify_sync.py asserts this constant equals
	 * TraceFxBurstFile::EmissiveHueHeadroom, and goes red the moment the two drift. If the accessor
	 * ever appears, delete this and call it.
	 */
	constexpr float FxBurstHueHeadroom = 0.70f;

	/**
	 * T0/T1 border — the jar is an object you range-find against on the floor, not wayfinding.
	 *
	 * CONSTANT PER JAR. It never shimmers and it never pulses, and that is the ART_BIBLE §6.3 ruling
	 * quoted in the header on HasCollarBuilt(): a jar is a lethal volume and §3.3 forbids brightness
	 * animation on every one of those. Nothing in this file writes Glow twice.
	 *
	 * *** IT IS DERIVED FROM THE ACCENT, NOT A LITERAL, AND THAT IS THE FIX FOR A REAL DEFECT. ***
	 * This was `constexpr float JarGlowShipped = 0.74f`, and the comment below already stated the rule
	 * the number came from: ATraceFxBurst caps an emissive piece at 0.70 on its BRIGHTEST CHANNEL, so
	 * the right glow is 0.70 / brightest. It then wrote down the ANSWER for the palette of the day
	 * (0.70 / 0.95 = 0.737, shipped as 0.74) instead of the arithmetic — the copied-literal bug this
	 * project keeps being bitten by. When Oyster's accent moved from cyan (brightest 0.95) to deep sea
	 * green (brightest 0.78), the literal stopped meaning what its own comment said it meant:
	 *
	 *     accent x glow, the product the tonemapper actually sees
	 *       shipped, old accent :  0.95 x 0.74   = 0.703      <- what the ladder below was shot at
	 *       stale literal, new accent : 0.78 x 0.74 = 0.577   <- 82% of it; by the ladder, val ~0.904
	 *       DERIVED, new accent : 0.78 x 0.897    = 0.700     <- val ~0.932, the shipped read restored
	 *
	 * The arena floor beside a jar measured val 0.916, so at the stale literal the jar would have been
	 * DIMMER THAN THE FLOOR IT LIES ON — which is the exact rung (0.50) the ladder rejected. Deriving
	 * it moves the glow 0.74 -> 0.897 and puts the product back on the cap, to three decimals.
	 *
	 * THE LADDER, all four rungs in ONE frame on the blue end of the arena — the brightest surface in
	 * the game, i.e. the worst case for any emissive. HSV, sampled off the jar bodies. *** SHOT ON THE
	 * PRE-RE-SPACE PALETTE (cyan #95EDF9), AND STILL THE GOVERNING MEASUREMENT, *** for exactly the
	 * reason TraceFxBurst.cpp gives about its own ladder: what it measures is not a colour, it is the
	 * rate at which SATURATION is spent as the brightest channel is pushed, and that rate is a property
	 * of the tonemapper. Read the left column as the product 0.95 x Glow, not as a glow:
	 *
	 *     Glow 1.40  (product 1.330)  hue 182.9  sat 0.100  val 0.969   white with a cast; no identity
	 *     Glow 1.00  (product 0.950)  hue 183.2  sat 0.133  val 0.955
	 *     Glow 0.74  (product 0.703)  hue 184.1  sat 0.178  val 0.933   <- the product that ships
	 *     Glow 0.50  (product 0.475)  hue 183.6  sat 0.259  val 0.881   most colour, dimmer than floor
	 *
	 * The HUE is right at every rung — it was never the hue that was wrong. The trade is the one
	 * ATraceFxBurst measured for its own pieces: every step up buys a little brightness and costs a lot
	 * of COLOUR. Holding the PRODUCT at the cap keeps a readable accent while the jar is still brighter
	 * than everything around it, whichever accent Oyster is wearing — including a dark one, where a
	 * fixed glow would have gone invisible. Re-run Trace.Oyster.JarLadder if the HEADROOM is retuned;
	 * do not re-run it because an accent moved.
	 *
	 * FX_AUDIO_PLAN §2.6 says 1.4. It is not 1.4 and never was: at 1.4 the brightest channels are
	 * pushed past the point where M_TraceNeon's emissive clips in every channel and the jar photographs
	 * as a WHITE cylinder with a faint cast — the first parade frame sampled at saturation 0.094, i.e.
	 * the shape perfect and the identity gone. A jar has no MASS carrying the hue the way the slime
	 * wall's slab does, so if the glow clips there is nothing left that says "Oyster".
	 */
	float JarGlowShipped()
	{
		const FLinearColor Accent = JarColor();
		const float Brightest = FMath::Max3(Accent.R, Accent.G, Accent.B);
		return (Brightest > UE_KINDA_SMALL_NUMBER) ? (FxBurstHueHeadroom / Brightest) : 1.f;
	}

	/** The collar outranks the body by the arena's own lip-to-face ratio (ART_BIBLE §3.2), preserved
	  * as a RATIO so a retune of the body's glow carries the collar with it instead of inverting them. */
	constexpr float CollarGlowRatio = 2.2f / 1.4f;

	/** The bible's ceiling for an FX emissive: 4.2, the smear-head precedent (§3.2). */
	constexpr float MaxJarGlow = 4.2f;

	/** The Glow a jar built RIGHT NOW gets. The dev override exists only outside Shipping. */
	float BodyGlowNow()
	{
#if !UE_BUILD_SHIPPING
		const float Override = CVarOysterJarGlow.GetValueOnAnyThread();
		if (Override > 0.f)
		{
			return FMath::Min(Override, MaxJarGlow);
		}
#endif
		return JarGlowShipped();
	}

	/** §2.6: "thin cylinder r x1.25 of jar" — the flange's radius, as a multiple of the body's. */
	constexpr float CollarRadiusScale = 1.25f;

	/**
	 * §2.6 asks for h 6 uu. It is drawn at 8.
	 *
	 * ART_BIBLE §3.4: a world-space emissive under 8 uu ACROSS dissolves into dashes under TSR at any
	 * useful range, and the collar's height is the dimension that carries its read from a player's eye
	 * (which is above a jar lying on the floor, so the flange is seen edge-on). 6 uu would have been a
	 * flicker. Same call, same floor, and the same reason as ATraceFxBurst's MinEmissiveRadiusUU.
	 */
	constexpr float CollarHeightUU = 8.f;
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

	// FX_AUDIO_PLAN §2.6, the Pickler collar. A default subobject on EVERY jar, hidden until
	// BuildLookIfNeeded decides this one is a Pickler — the same "one actor with a flag" shape the
	// header argues for, extended to the geometry. Attached to Collision (the root) and NOT to Mesh,
	// so the body's 0.35 scale does not multiply into the collar's; its size is computed in uu.
	Collar = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Collar"));
	Collar->SetupAttachment(Collision);
	Collar->SetVisibility(false);
	if (CylinderFinder.Succeeded())
	{
		Collar->SetStaticMesh(CylinderFinder.Object);
	}
	// Decoration: no collision, no shadow, no overlaps, no navigation. The shared pass, so a future
	// edit here cannot quietly give a cosmetic flange a collision profile.
	UTraceFxShapes::ConfigureFxComponent(Collar);
	Collar->SetCanEverAffectNavigation(false);
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

	// On the server bIsPickler is already set by Initialise. On a client it rides the same bunch as
	// the spawn, so it is normally here too — and when it is not, Tick's call covers it. Both entry
	// points are idempotent.
	BuildLookIfNeeded();
}

// =================================================================================================
// FX_AUDIO_PLAN §2.6 — the look
// =================================================================================================

float ATraceOysterJar::MeasureJarRadiusUU() const
{
	// MEASURED OFF THE LIVE COMPONENT, not off the 0.35 written in the constructor. The two are the
	// same today, and that is exactly why this reads the component: the collar is specified as a
	// MULTIPLE of the jar's radius, so a retune of the body scale that left a hard-coded radius here
	// would silently detach the flange from the thing it is a flange on.
	if (Mesh == nullptr)
	{
		return 0.f;
	}
	return 0.5f * UTraceFxShapes::BasicShapeExtentUU * static_cast<float>(Mesh->GetRelativeScale3D().X);
}

bool ATraceOysterJar::HasCollarBuilt() const
{
	return Collar != nullptr && Collar->IsVisible() && CollarBlend != ETraceFxBlend::None;
}

FString ATraceOysterJar::DescribeLook() const
{
	return FString::Printf(TEXT("jar=%s glow %.2f collar=%s"),
		UTraceFxShapes::BlendName(JarBlend), BuiltGlow,
		(Collar != nullptr && Collar->IsVisible()) ? UTraceFxShapes::BlendName(CollarBlend) : TEXT("<none: dash jar>"));
}

void ATraceOysterJar::BuildLookIfNeeded()
{
	// Re-entered whenever the KIND changes, which on a client is the frame bIsPickler lands if it
	// arrived after the actor did. Everything below is a write of the same values, so a second pass
	// costs one comparison and produces the identical jar.
	if (bLookBuilt && bLookIsPickler == bIsPickler)
	{
		return;
	}

	// A dedicated server cooks no shaders, so there is no material to make and nothing to see. The
	// GEOMETRY is left alone — the collar is decoration and, unlike ATraceSlimewall's slab, no
	// invariant is asked about it.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bLookBuilt = true;
		bLookIsPickler = bIsPickler;
		return;
	}

	const bool bFirstBuild = !bLookBuilt;
	bLookBuilt = true;
	bLookIsPickler = bIsPickler;

	// ---- the body -------------------------------------------------------------------------------
	//
	// Made once. The tint never changes and the jar never animates, so re-tinting on a kind change
	// would be a second write of the same three parameters.
	if (bFirstBuild && Mesh != nullptr)
	{
		JarMID = UTraceFxShapes::MakeGlowMID(Mesh, 0, ETraceFxBlend::Emissive, JarBlend);
		if (JarMID != nullptr)
		{
			// LATCHED: read once, here, and never again for this jar. See CVarOysterJarGlow.
			BuiltGlow = TraceOysterJar::BodyGlowNow();
			UTraceFxShapes::SetGlow(JarMID, JarBlend, TraceOysterJar::JarColor(), BuiltGlow);
		}
		else
		{
			// *** THE BODY IS DRAWN ANYWAY. *** ART_BIBLE §6.1's "None => hide the component" is a rule
			// about DECORATION, and a jar is not decoration: it is a volume that poisons whoever walks
			// into it, and an invisible one is a trap rather than an ugly one. This is the identical
			// call ATraceSlimewall::BuildSlabIfNeeded makes about its slab, for the identical reason,
			// and the collar below — which IS decoration — takes the opposite branch.
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Oyster] Jar: no FX material resolved, so the body draws with the engine default. It is "
				     "still lethal, still breakable and still on the same 4 s clock; only the accent is missing."));
		}
	}

	// ---- the collar -----------------------------------------------------------------------------
	if (Collar == nullptr)
	{
		return;
	}

	if (!bIsPickler)
	{
		Collar->SetVisibility(false);   // a dash jar is a plain cylinder — ART_BIBLE §6.3, by shape
		return;
	}

	const float JarRadiusUU = MeasureJarRadiusUU();
	if (JarRadiusUU <= KINDA_SMALL_NUMBER)
	{
		return;   // no body mesh: there is nothing for a flange to be a flange on
	}

	const float CollarRadiusUU = JarRadiusUU * TraceOysterJar::CollarRadiusScale;

	// Sat DOWN onto the rim rather than centred on it: the flange occupies the top CollarHeightUU of
	// the jar's own silhouette, so the Pickler jar is the same height as a dash jar and only its
	// PROFILE differs. A collar floating above the rim would have changed the height as well, and then
	// two things would be saying "Pickler" instead of one.
	const float JarHalfHeightUU = 0.5f * UTraceFxShapes::BasicShapeExtentUU
		* static_cast<float>(Mesh->GetRelativeScale3D().Z);

	Collar->SetRelativeLocation(FVector(0.f, 0.f, JarHalfHeightUU - 0.5f * TraceOysterJar::CollarHeightUU));
	Collar->SetRelativeScale3D(FVector(
		UTraceFxShapes::ShapeScaleForRadiusUU(CollarRadiusUU),
		UTraceFxShapes::ShapeScaleForRadiusUU(CollarRadiusUU),
		UTraceFxShapes::ShapeScaleForLengthUU(TraceOysterJar::CollarHeightUU)));

	if (CollarMID == nullptr)
	{
		CollarMID = UTraceFxShapes::MakeGlowMID(Collar, 0, ETraceFxBlend::Emissive, CollarBlend);
	}

	if (CollarMID != nullptr)
	{
		UTraceFxShapes::SetGlow(CollarMID, CollarBlend, TraceOysterJar::JarColor(),
			FMath::Min(BuiltGlow * TraceOysterJar::CollarGlowRatio, TraceOysterJar::MaxJarGlow));
	}

	// HIDDEN, NOT GREY. The collar is pure decoration, so it takes ART_BIBLE §6.1's ordinary ruling:
	// an untextured 100 uu default-grey ring around a jar is worse than no ring. The cost of losing it
	// is that the two jar kinds stop being distinguishable, which is a degradation and not a lie.
	Collar->SetVisibility(CollarMID != nullptr && CollarBlend != ETraceFxBlend::None);

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Oyster] Pickler jar dressed: body r %.1f uu glow %.2f, collar r %.1f uu (x%.2f) h %.0f uu "
		     "glow %.2f, %s."),
		JarRadiusUU, BuiltGlow, CollarRadiusUU, TraceOysterJar::CollarRadiusScale,
		TraceOysterJar::CollarHeightUU,
		FMath::Min(BuiltGlow * TraceOysterJar::CollarGlowRatio, TraceOysterJar::MaxJarGlow),
		*DescribeLook());
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

	// The look depends on bIsPickler, which the line above is the first place to know. On the server
	// this is what actually raises the collar; on a client Tick does it.
	BuildLookIfNeeded();

	if (FlightVelocity.IsNearlyZero())
	{
		// The dash jar. It is dropped where he is, so it is on the ground from the first frame and
		// its 4 s starts now.
		Land();
		return;
	}

	// FX_AUDIO_PLAN §2.6 audio row: `OysterPickler` World (server, lob cast).
	//
	// HERE AND NOT IN THE KIT, and the reason is that this is the moment the lob EXISTS: Initialise is
	// the one call that turns a spawned actor into a thrown one, it is server-only, and it runs after
	// ResolveReleaseLocation has decided where the jar was actually let go of — so the sound is at the
	// release point rather than at the muzzle the release was clamped back from.
	//
	// TraceAudio::Play, i.e. the ordinary table-driven path: `OysterPickler` is declared World-side in
	// Audio/TraceSoundEvents.cpp, so this multicasts once and every machine (the host included) plays
	// one copy. It is NOT PlayReplicatedLocal — the jar replicates, but Initialise does not run on a
	// client, so there is no already-on-every-machine call site here to ride.
	TraceAudio::Play(this, TraceSoundEvents::OysterPickler);
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceOysterJar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// BEFORE the authority guard, deliberately. A client's jar IS the mesh, and this is what builds
	// it: bIsPickler can land a frame or two after the actor on a client, and the collar has to grow
	// the moment it does. Idempotent and one comparison once it has.
	BuildLookIfNeeded();

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

			// FX_AUDIO_PLAN §2.6, the pull link. ONE PER VICTIM, fired from the line that actually
			// launched them, so the FX cannot exist for a pull the choke point refused — every `continue`
			// above is a player who is not yanked and does not get a ring.
			//
			// A RING FACING THE JAR RATHER THAN §2.6's LITERAL "cylinder victim->jar", and that is a
			// deliberate substitution the burst tranche's own handoff offers (W3-FXBURST report §7.5:
			// "GenericRing is a ring; if you want a link cylinder, draw it in the jar actor"). The
			// cylinder option requires the jar to know its victims on every machine, i.e. a replicated
			// victim list on a 0.29 s actor; the ring is the SAME replicated fact (one burst actor per
			// victim, spawned by the server, drawn and heard identically everywhere) with no new wire
			// format. Its normal is the pull axis, so it reads as a hoop the victim is being drawn
			// through — which is the direction information the cylinder was carrying.
			//
			// POISONED GREEN, NOT OYSTER CYAN. §6.2's hue priority: the pull is the front half of a
			// poison event, and GenericRing is the one burst type that takes a tint precisely because
			// two kits share it.
			ATraceFxBurst::Burst(WorldPtr, ETraceFxBurstType::GenericRing,
				Candidate->GetActorLocation(), ToJar, /*RadiusUU*/ 0.f,
				&ATraceOysterPoisonCloud::GetPoisonedHue());
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
