// Trace — Mace's roped spike. The anchor and the visual; the rules live in UTraceAbilitySetMace.

#include "Abilities/Characters/TraceMaceSpike.h"

#include "Camera/CameraActor.h"          // Trace.Mace.SpikeFxWatch's side-on observer
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"             // FTSTicker — Trace.Mace.RopeProbe polls per frame
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                   // TActorIterator — same
#include "GameFramework/GameStateBase.h"   // GetServerWorldTimeSeconds — the clock the flight is derived against
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"                  // FScreenshotRequest — the probe aims its own frames
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"

#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId::Mace — the id, not the colour
#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterRoster.h"     // THE accent. See MaceAccent() below.
#include "Gameplay/TraceFxBurst.h"         // the SpikeEmbed one-shot — server-spawned, seen everywhere
#include "Gameplay/TraceFxShapes.h"        // the shared primitive + MID discipline (FX §2.4 dressing)
#include "Trace.h"
#include "TraceSettings.h"                 // MaceSpikeTravelSpeed — derived, never copied onto the wire

// =================================================================================================
// FX_AUDIO_PLAN §2.4 — MACE'S DRESSING. The numbers, and the one measurement that changed one of them.
// =================================================================================================

namespace TraceMaceSpikeFile
{
	/**
	 * Mace's accent — ONE hue for the whole kit: cone, rope, sleeve, embed burst.
	 *
	 * *** READ FROM THE ROSTER, NOT COPIED, AND THAT IS A BUG FIX AND NOT A REFACTOR. *** This was
	 * `const FLinearColor MaceViolet(0.65f, 0.55f, 1.00f, 1.f)` with a comment quoting ART_BIBLE
	 * §2.3's #D3C4FF. When the ten accents were re-spaced away from the two team hues, Mace moved to
	 * #DFC4FE and this copy did not, so his rope and his spike flew in the old violet while his body
	 * wore the new one. Same failure, same shape and the same answer as the drawn-vs-lethal rule:
	 * read the live number from the one place that owns it.
	 *
	 * TraceCharacterRoster is that place — it is what the ten UTraceCharacterDefinition assets are
	 * generated from and what the body materials are stamped from. Falls back to white on a roster
	 * that did not resolve at all, which is loud rather than subtly wrong.
	 */
	FLinearColor MaceAccent()
	{
		if (const TraceCharacterRoster::FTraceCharacterEntry* Row =
			TraceCharacterRoster::Find(static_cast<uint8>(ETraceCharacterId::Mace)))
		{
			return FLinearColor(Row->Accent.R, Row->Accent.G, Row->Accent.B, 1.f);
		}
		return FLinearColor::White;
	}

	/** FX §2.4: "cone gets violet M_TraceNeon MID Glow 3.0". Bible §3.2 T2 — a wayfinding read. */
	constexpr float SpikeConeGlow = 3.0f;

	/** FX §2.4: rope core "emissive violet Glow 2.6" — bible §3.2 T1, the trail-ribbon band exactly. */
	constexpr float RopeCoreGlow = 2.6f;

	/** FX §2.4 radii. 4 uu radius is 8 uu ACROSS: bible §3.4's AA floor for a 3,000 uu read. */
	constexpr float RopeCoreRadiusUU = 4.f;
	constexpr float RopeSleeveRadiusUU = 7.f;

	/** FX §2.4: the sleeve's additive weight. Additive intensity rides in the colour and caps at 1.0. */
	constexpr float RopeSleeveIntensity = 0.35f;

	/** Below this the rope is not drawn at all: a 10 uu rope is a dot at the muzzle, not a line. */
	constexpr float MinRopeLengthUU = 10.f;

	// =============================================================================================
	// *** THE HUE HEADROOM, AND WHY §2.4'S GLOW NUMBERS ARE CEILINGS RATHER THAN SETTINGS. ***
	//
	// M_TraceNeon's emissive is Colour x Glow and Mace's violet has a channel at 1.0, so Glow 2.6
	// asks the renderer for 2.6 in blue, 1.7 in red and 1.4 in green. Every one of those clips, the
	// tonemapper hands back WHITE, and the "one hue per effect" rule (bible §6.2) has been broken by
	// the very line that was supposed to deliver the hue. That is not a hypothesis: this project has
	// now measured it twice, on two different effects, and written both measurements down —
	//
	//   ATraceElleGate (TraceElleGate.cpp:100-117): purple at Glow 3.5 photographs as a white core
	//     with a PINK halo (hue clusters at 301 deg); at Glow 1.0 it photographs as purple (280 deg).
	//     Product of brightest channel x Glow at the shipped value: 0.90 x 1.0 = 0.90.
	//   ATraceFxBurst (W3-FXBURST §5, the four-rung hue ladder): every step up in headroom buys ~1.5%
	//     brightness and costs ~20% of the SATURATION. It latched 0.70 as the best hue-per-brightness.
	//
	// So the tier from §3.2 is kept as the ASK and this is the CEILING it is clamped to: whichever
	// channel of the hue is brightest may not be pushed past the headroom. For Mace's violet that
	// resolves 2.6 down to well under 1 — which is the tier the arena's own purple already sits in,
	// and the reason it is the only purple in this game that photographs purple.
	//
	// The default is between the two measurements rather than at either, because Mace's rope is seen
	// against dark walls (where the gate's 0.90 holds) AND across a lit floor (where the burst's 0.70
	// was measured) — and it was then confirmed against real frames rather than reasoned into place;
	// Trace.Mace.VioletHeadroom is the dev knob those frames were shot with.
	// =============================================================================================

	constexpr float EmissiveHueHeadroom = 0.80f;
}

#if !UE_BUILD_SHIPPING

/**
 * DEV ONLY. Overrides TraceMaceSpikeFile::EmissiveHueHeadroom so a ladder can be MEASURED off frames
 * rather than argued about. It is read ONCE, when a spike builds its dressing, and never again — the
 * FxBurst hue ladder's own bug was re-reading it every tick, which made four rungs re-write themselves
 * to whatever the CVar said last and produced four identical frames presented as a ladder.
 */
static TAutoConsoleVariable<float> CVarMaceVioletHeadroom(
	TEXT("Trace.Mace.VioletHeadroom"),
	-1.f,
	TEXT("Dev/measurement. < 0 (default) = use the shipped headroom. Otherwise the ceiling that Mace's "
	     "brightest violet channel may be pushed to before every channel clips and the rope renders "
	     "white. Latched when a spike builds, so a ladder shot at four values gives four looks."),
	ECVF_Cheat);

/**
 * THE RED ARM FOR THE VISIBILITY FACT — FX §8.8 asks for it by name, and it is the Elle gate's arm.
 *
 * 1 (shipped): the cone, the rope core and the rope sleeve are given their meshes, so there is
 *              something on screen.
 * 0:           every rule still runs — the spike flies, embeds, replicates, is pulled on, the rope is
 *              stretched to the right two points every frame — and NO MESH IS EVER ASSIGNED, so the
 *              renderer is never told any of it exists. That is precisely the shape of the shipped
 *              "Elle's portal is invisible" defect and of F1's "the rope is invisible on clients":
 *              GetDrawnPieceCount() must fall to 0 under it while nothing else changes.
 */
static TAutoConsoleVariable<int32> CVarMaceSpikeVisible(
	TEXT("Trace.Mace.SpikeVisible"),
	1,
	TEXT("Dev/red arm (FX §8.8). 1 (default) = the spike cone and both rope pieces are given their "
	     "meshes and are on screen. 0 = they are built, stretched and coloured exactly as now but "
	     "never given a mesh, so ATraceMaceSpike::GetDrawnPieceCount() reads 0 with every rule intact. "
	     "Never ship 0."),
	ECVF_Cheat);

#endif // !UE_BUILD_SHIPPING

namespace TraceMaceSpikeFile
{
	/** The headroom this process is using. Dev-overridable; a constant in Shipping. */
	FORCEINLINE float HueHeadroom()
	{
#if !UE_BUILD_SHIPPING
		const float Override = CVarMaceVioletHeadroom.GetValueOnAnyThread();
		if (Override > 0.f)
		{
			return FMath::Clamp(Override, 0.2f, 4.2f);
		}
#endif
		return EmissiveHueHeadroom;
	}

	/** True when the meshes may be assigned. See the red arm above. */
	FORCEINLINE bool MayAssignMeshes()
	{
#if !UE_BUILD_SHIPPING
		return CVarMaceSpikeVisible.GetValueOnGameThread() != 0;
#else
		return true;
#endif
	}

	/**
	 * Pushes hue + intensity into a dressed piece, clamping Glow to bible §3.2's 4.2 transient ceiling
	 * AND to the hue headroom on the way. Additive is separately capped at 1.0 by the material itself
	 * (it has no Glow scalar), so both clamps only bite on the Emissive/Fallback path.
	 */
	void SetPieceGlow(UMaterialInstanceDynamic* MID, ETraceFxBlend Blend, const FLinearColor& Hue,
	                  float Intensity, float Headroom)
	{
		if (MID == nullptr || Blend == ETraceFxBlend::None)
		{
			return;
		}

		float Ceiling = 1.f;
		if (Blend == ETraceFxBlend::Emissive || Blend == ETraceFxBlend::Fallback)
		{
			const float BrightestChannel = FMath::Max3(Hue.R, Hue.G, Hue.B);
			Ceiling = (BrightestChannel > UE_KINDA_SMALL_NUMBER)
				? FMath::Min(ATraceFxBurst::MaxTransientGlow, Headroom / BrightestChannel)
				: ATraceFxBurst::MaxTransientGlow;
		}

		UTraceFxShapes::SetGlow(MID, Blend, Hue, FMath::Clamp(Intensity, 0.f, Ceiling));
	}
}

ATraceMaceSpike::ATraceMaceSpike()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;
	// NO MOVEMENT REPLICATION, AND NOW THE FLIGHT IS STILL VISIBLE (C4). The three launch facts
	// below (LaunchLocation / LaunchDirection / LaunchServerTime) plus the shared travel-speed
	// setting let every machine DERIVE where the spike is this frame, and AnchorLocation ends it —
	// so a client sees the throw without 0.4 s of position updates on the wire. This used to say the
	// anchor alone was enough "once it lands", which was true and was also the bug: until it landed,
	// a client's copy sat frozen at the muzzle with the rope pointing at Mace's own chest.
	SetReplicateMovement(false);
	bAlwaysRelevant = true;

	USceneComponent* SpikeRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SpikeRoot);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(SpikeRoot);
	// NO COLLISION. See the header: a collider here would block bullets and break jars.
	UTraceFxShapes::ConfigureFxComponent(Mesh);
	Mesh->SetRelativeScale3D(FVector(0.28f, 0.28f, 0.28f));
	Mesh->SetVisibility(false);   // until the dressing resolves. Never a grey untextured cone.

	Rope = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Rope"));
	Rope->SetupAttachment(SpikeRoot);
	UTraceFxShapes::ConfigureFxComponent(Rope);
	Rope->SetVisibility(false);

	RopeSleeve = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RopeSleeve"));
	RopeSleeve->SetupAttachment(SpikeRoot);
	UTraceFxShapes::ConfigureFxComponent(RopeSleeve);
	RopeSleeve->SetVisibility(false);

	// *** NO MESH IS ASSIGNED HERE ANY MORE, AND THAT IS THE POINT (FX §8.8). ***
	//
	// This used to run two ConstructorHelpers::FObjectFinders and call SetStaticMesh right here, with
	// a comment saying a runtime LoadObject of an engine basic shape resolves to null once cooked
	// unless something has already referenced it. That reason is still true and it is now UTraceFxShapes'
	// job: its CDO constructor holds /Engine/BasicShapes/{Cone,Cylinder} as hard UPROPERTYs, which is
	// the cook reference, and GetCone()/GetCylinder() hand them back at runtime for free.
	//
	// Moving the assignment to BuildDressingIfNeeded() buys the thing a constructor cannot have: a
	// CVar. Trace.Mace.SpikeVisible 0 withholds the meshes with every rule still running, which is the
	// only way to prove that GetDrawnPieceCount() can go red — the exact hole that let "Elle's portal
	// is invisible" survive a dedicated is-it-drawn harness for two demos.
}

void ATraceMaceSpike::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceMaceSpike, AnchorLocation);
	DOREPLIFETIME(ATraceMaceSpike, bEmbedded);

	// C4. Written once in InitialiseFlight, in the same frame as the spawn, so they ride the actor's
	// initial bunch and a client can derive the flight from the first frame it knows the spike at all.
	DOREPLIFETIME(ATraceMaceSpike, LaunchLocation);
	DOREPLIFETIME(ATraceMaceSpike, LaunchDirection);
	DOREPLIFETIME(ATraceMaceSpike, LaunchServerTime);
}

void ATraceMaceSpike::BeginPlay()
{
	Super::BeginPlay();

	if (const UWorld* WorldPtr = GetWorld())
	{
		SpawnWorldTime = WorldPtr->GetTimeSeconds();
	}

	// The dressing is built here AND from Tick, because which of the two arrives first depends on the
	// machine: on the server BeginPlay runs inside the SpawnActor call with the anchor already set, on
	// a client it runs before a single replicated property has landed. It is idempotent either way.
	BuildDressingIfNeeded();
}

void ATraceMaceSpike::BuildDressingIfNeeded()
{
	if (bDressingBuilt)
	{
		return;
	}

	// A dedicated server draws nothing and has no cooked shaders to draw it with. The FACT still
	// replicates from here; only the paint is skipped. (ATraceElleGate::BuildRingsIfNeeded, same test,
	// same reason.)
	if (GetNetMode() == NM_DedicatedServer)
	{
		bDressingBuilt = true;
		return;
	}

	UStaticMesh* Cone = UTraceFxShapes::GetCone();
	UStaticMesh* Cylinder = UTraceFxShapes::GetCylinder();
	if (Cone == nullptr || Cylinder == nullptr || Mesh == nullptr || Rope == nullptr || RopeSleeve == nullptr)
	{
		return;   // try again next tick; the primitives are engine content and should never be absent
	}

	bDressingBuilt = true;

	// LATCHED, read nowhere else. See the CVar's own comment for the ladder bug that made this a rule.
	const float Headroom = TraceMaceSpikeFile::HueHeadroom();

	// THE RED ARM (Trace.Mace.SpikeVisible 0) LIVES ON EXACTLY THIS LINE. Everything below it still
	// runs: the MIDs are made, the colours are pushed, the rope is stretched every frame to the right
	// two points. Without a static mesh, UStaticMeshComponent::CreateSceneProxy returns nullptr and
	// the renderer is never told any of it exists.
	if (TraceMaceSpikeFile::MayAssignMeshes())
	{
		Mesh->SetStaticMesh(Cone);
		Rope->SetStaticMesh(Cylinder);
		RopeSleeve->SetStaticMesh(Cylinder);
	}

	// THE SPIKE ITSELF: emissive, because it is a small solid (28 uu across) and the tracer's measured
	// rule is that BIG VOLUMES ARE ADDITIVE, THIN AND SMALL PIECES ARE EMISSIVE. An additive cone this
	// size over a lit wall is a smudge; an emissive one is an object stuck in the wall.
	SpikeMID = UTraceFxShapes::MakeGlowMID(Mesh, 0, ETraceFxBlend::Emissive, SpikeBlend);
	TraceMaceSpikeFile::SetPieceGlow(SpikeMID, SpikeBlend, TraceMaceSpikeFile::MaceAccent(),
		TraceMaceSpikeFile::SpikeConeGlow, Headroom);
	Mesh->SetVisibility(SpikeBlend != ETraceFxBlend::None);

	// THE ROPE, core + sleeve, straight off ATraceTracer's beam: an emissive core inside an additive
	// halo. The sleeve is additive-ONLY — it must never fall back to an opaque material, because a fat
	// opaque tube would write depth and hide the core it exists to surround.
	RopeCoreMID = UTraceFxShapes::MakeGlowMID(Rope, 0, ETraceFxBlend::Emissive, RopeCoreBlend);
	TraceMaceSpikeFile::SetPieceGlow(RopeCoreMID, RopeCoreBlend, TraceMaceSpikeFile::MaceAccent(),
		TraceMaceSpikeFile::RopeCoreGlow, Headroom);

	RopeSleeveMID = UTraceFxShapes::MakeGlowMID(RopeSleeve, 0, ETraceFxBlend::Additive, RopeSleeveBlend);
	if (RopeSleeveBlend == ETraceFxBlend::Emissive || RopeSleeveBlend == ETraceFxBlend::Fallback)
	{
		// The additive parent did not resolve and this piece degraded to an opaque one. Drop it rather
		// than draw it: see above — an opaque sleeve is strictly worse than no sleeve.
		RopeSleeveBlend = ETraceFxBlend::None;
		RopeSleeveMID = nullptr;
	}
	TraceMaceSpikeFile::SetPieceGlow(RopeSleeveMID, RopeSleeveBlend, TraceMaceSpikeFile::MaceAccent(),
		TraceMaceSpikeFile::RopeSleeveIntensity, Headroom);

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[Mace] spike dressing built: cone %s glow %.2f | rope core %s r %.0fuu | sleeve %s r %.0fuu "
		     "| headroom %.2f | meshes assigned=%d, pieces DRAWN=%d."),
		UTraceFxShapes::BlendName(SpikeBlend), TraceMaceSpikeFile::SpikeConeGlow,
		UTraceFxShapes::BlendName(RopeCoreBlend), TraceMaceSpikeFile::RopeCoreRadiusUU,
		UTraceFxShapes::BlendName(RopeSleeveBlend), TraceMaceSpikeFile::RopeSleeveRadiusUU,
		Headroom, TraceMaceSpikeFile::MayAssignMeshes() ? 1 : 0, GetDrawnPieceCount());
}

int32 ATraceMaceSpike::GetDrawnPieceCount() const
{
	// BOTH FACTS ARE REQUIRED OF EVERY PIECE, and the pair is what makes the counter able to go red:
	// a mesh (or the renderer never gets a scene proxy) AND a resolved material (or the piece is
	// deliberately hidden rather than drawn as an untextured default). Visibility is asked last
	// because the rope legitimately hides itself when it is shorter than MinRopeLengthUU.
	int32 Drawn = 0;

	auto CountPiece = [&Drawn](const UStaticMeshComponent* Piece, ETraceFxBlend Blend)
	{
		if (Piece != nullptr && Piece->GetStaticMesh() != nullptr
			&& Blend != ETraceFxBlend::None && Piece->IsVisible())
		{
			++Drawn;
		}
	};

	CountPiece(Mesh, SpikeBlend);
	CountPiece(Rope, RopeCoreBlend);
	CountPiece(RopeSleeve, RopeSleeveBlend);
	return Drawn;
}

void ATraceMaceSpike::InitialiseFlight(UTraceAbilitySetMace* InOwnerSet, const FVector& InAnchor,
                                       float InTravelSpeed, float InBackstopLifetimeSeconds)
{
	OwnerSet = InOwnerSet;
	AnchorLocation = InAnchor;
	TravelSpeed = InTravelSpeed;
	BackstopLifetimeSeconds = FMath::Max(1.f, InBackstopLifetimeSeconds);

	// C4 — THE THREE LAUNCH FACTS, stamped here and never touched again. This runs in the same frame
	// as SpawnActor and before the actor's first net update, so they are in the initial bunch: a
	// client that has just been told the spike exists already knows where the throw began, which way
	// it went and when. Direction is taken from the resolved anchor rather than from the pawn's aim,
	// because the anchor is what the flight actually ends at.
	LaunchLocation = GetActorLocation();
	LaunchDirection = (AnchorLocation - LaunchLocation).GetSafeNormal();
	LaunchServerTime = ServerTimeNow();

	if (TravelSpeed <= 0.f)
	{
		SetActorLocation(AnchorLocation);
		bEmbedded = true;
	}
}

float ATraceMaceSpike::ServerTimeNow() const
{
	if (const UWorld* WorldPtr = GetWorld())
	{
		if (const AGameStateBase* GameStateBase = WorldPtr->GetGameState())
		{
			return static_cast<float>(GameStateBase->GetServerWorldTimeSeconds());
		}
		return WorldPtr->GetTimeSeconds();
	}
	return 0.f;
}

void ATraceMaceSpike::UpdateDerivedFlight()
{
	if (bEmbedded)
	{
		// Landed. AnchorLocation is the authority from here on and the snap in Tick owns the actor.
		return;
	}

	if (LaunchServerTime <= 0.f || LaunchDirection.IsNearlyZero())
	{
		// The launch facts have not arrived yet (or this is a spike from before C4). Leaving the
		// actor where the spawn transform put it is exactly the old behaviour, which is the right
		// thing to degrade to.
		return;
	}

	// DERIVED, NOT COPIED (Demo-21). The speed is the same settings knob the server's flight reads,
	// so there is one number and not two; the clamp against the anchor distance means that if a
	// harness ever throws at an overridden speed, the client's spike simply arrives early and waits
	// at the anchor instead of flying through the wall.
	const float Speed = FMath::Max(1.f, UTraceSettings::Get().MaceSpikeTravelSpeed);
	const float Elapsed = FMath::Max(0.f, ServerTimeNow() - LaunchServerTime);
	const float MaxDistance = FVector::Dist(FVector(LaunchLocation), FVector(AnchorLocation));
	const float Travelled = FMath::Min(Speed * Elapsed, MaxDistance);

	SetActorLocation(FVector(LaunchLocation) + FVector(LaunchDirection) * Travelled);
}

void ATraceMaceSpike::FireEmbedBurstIfNeeded()
{
	if (bEmbedBurstFired || !bEmbedded || !HasAuthority())
	{
		return;
	}
	bEmbedBurstFired = true;

	// THE SURFACE NORMAL IS DERIVED, NOT REPLICATED. SpikeEmbed's Direction is documented as the hit
	// normal, and the spike flew INTO the wall, so the wall faces back along the flight. Deriving it
	// costs nothing and keeps the anchor the only thing on the wire that describes where it stuck.
	// (The harness path throws with a zero direction; UpVector is the honest degradation there.)
	const FVector Flight = FVector(LaunchDirection);
	const FVector Normal = Flight.IsNearlyZero() ? FVector::UpVector : (-Flight).GetSafeNormal();

	ATraceFxBurst::Burst(GetWorld(), ETraceFxBurstType::SpikeEmbed, FVector(AnchorLocation), Normal);
}

void ATraceMaceSpike::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	BuildDressingIfNeeded();

	if (!HasAuthority())
	{
		// A client's copy learns bEmbedded and AnchorLocation by replication, and derives everything
		// in between from the launch facts. Position FIRST, rope second: the rope is stretched to
		// wherever the spike is this frame, so doing it the other way round would draw one frame of
		// rope to the previous position on every frame of the flight.
		UpdateDerivedFlight();

		// Snap to the anchor the moment it is embedded so the visual never sits at a stale point.
		if (bEmbedded && !GetActorLocation().Equals(AnchorLocation, 1.f))
		{
			SetActorLocation(AnchorLocation);
		}

		UpdateRope();
		return;
	}

	UpdateRope();
	FireEmbedBurstIfNeeded();   // covers the zero-travel-speed harness path, which never reaches the arrival branch

	const UWorld* WorldPtr = GetWorld();
	if (WorldPtr != nullptr && (WorldPtr->GetTimeSeconds() - SpawnWorldTime) > BackstopLifetimeSeconds)
	{
		// The backstop, not the ability's timer. Mace clears her own spike on every legitimate route;
		// this only catches a spike whose owner vanished mid-flight.
		UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Spike hit its backstop lifetime and destroyed itself."));
		Destroy();
		return;
	}

	if (bEmbedded)
	{
		return;
	}

	const FVector Here = GetActorLocation();
	const FVector ToAnchor = AnchorLocation - Here;
	const float Remaining = ToAnchor.Size();
	const float Step = TravelSpeed * DeltaSeconds;

	if (Remaining <= Step || Remaining <= KINDA_SMALL_NUMBER)
	{
		SetActorLocation(AnchorLocation);
		bEmbedded = true;

		// The beat lands on the frame it arrives, not the frame after: the burst is what a player
		// reads as "it stuck", and one tick of lag between the spike stopping and the sparks is the
		// difference between an impact and two events.
		FireEmbedBurstIfNeeded();

		if (UTraceAbilitySetMace* Mace = OwnerSet.Get())
		{
			Mace->NotifySpikeEmbedded(this);
		}
		return;
	}

	SetActorLocation(Here + (ToAnchor / Remaining) * Step);
}

void ATraceMaceSpike::UpdateRope()
{
	// Cosmetic on every machine, and deliberately driven off whoever the ability component says the
	// pawn is rather than off a replicated pointer of our own: the rope is allowed to be a frame late.
	//
	// C4 — AND ON A CLIENT THERE IS NO ABILITY SET TO ASK. OwnerSet is written by InitialiseFlight,
	// which only ever runs on the server, and it is not a UPROPERTY, so on every other machine it was
	// null and this function returned "no pawn" — the rope was invisible for the whole life of the
	// spike, which is the bug F1 reported. The fallback is the actor's own Owner: both spawn sites
	// pass SpawnParams.Owner = Mace's pawn, and AActor::Owner replicates.
	const UTraceAbilitySetMace* Mace = OwnerSet.Get();
	const ATraceCharacter* MacePawn = (Mace != nullptr) ? Mace->GetCharacter() : nullptr;
	if (MacePawn == nullptr)
	{
		MacePawn = Cast<ATraceCharacter>(GetOwner());
	}
	if (Rope == nullptr)
	{
		return;
	}

	// The sleeve follows the core exactly — same two points, a fatter radius — so both are hidden and
	// shown together. A sleeve left visible around a hidden core is a violet tube with nothing in it.
	auto ShowRope = [this](bool bShow)
	{
		Rope->SetVisibility(bShow && RopeCoreBlend != ETraceFxBlend::None);
		if (RopeSleeve != nullptr)
		{
			RopeSleeve->SetVisibility(bShow && RopeSleeveBlend != ETraceFxBlend::None);
		}
	};

	if (MacePawn == nullptr)
	{
		ShowRope(false);
		return;
	}

	const FVector From = MacePawn->GetActorLocation();
	const FVector To = GetActorLocation();
	if (FVector::Dist(From, To) < TraceMaceSpikeFile::MinRopeLengthUU)
	{
		ShowRope(false);
		return;
	}

	// UTraceFxShapes::StretchBetween owns the arithmetic now (the engine cylinder is 100 uu tall about
	// its own centre and Z-aligned, so the piece goes at the midpoint with the length as its Z scale)
	// — hand-rolling it here was one more place the 100 uu assumption could go stale.
	ShowRope(true);
	UTraceFxShapes::StretchBetween(Rope, From, To, TraceMaceSpikeFile::RopeCoreRadiusUU);
	UTraceFxShapes::StretchBetween(RopeSleeve, From, To, TraceMaceSpikeFile::RopeSleeveRadiusUU);
}

#if !UE_BUILD_SHIPPING

FString ATraceMaceSpike::DebugDescribe() const
{
	const UTraceAbilitySetMace* Mace = OwnerSet.Get();
	const ATraceCharacter* MacePawn = (Mace != nullptr) ? Mace->GetCharacter() : nullptr;
	const TCHAR* PawnRoute = TEXT("ownerSet");
	if (MacePawn == nullptr)
	{
		MacePawn = Cast<ATraceCharacter>(GetOwner());
		PawnRoute = (MacePawn != nullptr) ? TEXT("Owner-chain") : TEXT("none");
	}

	const bool bRopeVisible = (Rope != nullptr) && Rope->IsVisible();
	const float RopeLength = (Rope != nullptr)
		? UTraceFxShapes::LengthUUFromShapeScale(Rope->GetComponentScale().Z) : 0.f;
	const float RopeRadius = (Rope != nullptr)
		? UTraceFxShapes::RadiusUUFromShapeScale(Rope->GetComponentScale().X) : 0.f;

	// The DRESSING half is on the same line as the replication half on purpose. F1 was "the rope is
	// invisible on a client" and it had two independent ways to be true — the pawn never resolved, or
	// the pieces never got a mesh — so a probe that reports one of them can pass on the other's bug.
	return FString::Printf(
		TEXT("at %s | anchor %s | embedded=%d | launch %s dir %s t=%.2f (serverNow %.2f) | pawn=%s via %s | ")
		TEXT("rope visible=%d len=%.0fuu r=%.1fuu | dressing cone=%s core=%s sleeve=%s, pieces DRAWN=%d/3"),
		*GetActorLocation().ToCompactString(), *FVector(AnchorLocation).ToCompactString(), bEmbedded ? 1 : 0,
		*FVector(LaunchLocation).ToCompactString(), *FVector(LaunchDirection).ToCompactString(),
		LaunchServerTime, ServerTimeNow(), *GetNameSafe(MacePawn), PawnRoute,
		bRopeVisible ? 1 : 0, RopeLength, RopeRadius,
		UTraceFxShapes::BlendName(SpikeBlend), UTraceFxShapes::BlendName(RopeCoreBlend),
		UTraceFxShapes::BlendName(RopeSleeveBlend), GetDrawnPieceCount());
}

/**
 * Trace.Mace.RopeProbe [seconds] — RUNS ANYWHERE, AND IS MEANT FOR THE CLIENT (C4).
 *
 * Polls every frame for @p seconds (default 20) and reports the FIRST FRAME on which this machine
 * knows about a spike at all, with everything it believes about it. That first line is the whole
 * verification: before C4 a client's first frame showed the spike frozen at the muzzle with the rope
 * invisible, because the flight was not derivable and OwnerSet is server-only. It then prints the
 * final state and a verdict.
 *
 * A screenshot can only show the 2 s embed window; this can show frame one.
 */
namespace TraceMaceRopeProbe
{
	/**
	 * @param bWatch  also point THIS machine's camera at the spike, by making the resolved Mace pawn
	 *                the local view target. A purely local view change — no pawn is moved, so nothing
	 *                the server owns is touched and no correction can fight it — which is what makes
	 *                it safe to do on a client whose own pawn is somewhere else entirely.
	 */
	/**
	 * @param bSideCamera  true = watch the rope from BESIDE it, with a spawned camera actor, instead of
	 *                     from the thrower's own eyes.
	 *
	 *                     C4's watch mode makes the THROWER the view target, which is the right answer
	 *                     for its question (does this client know about the spike at all?) and the wrong
	 *                     one for a dressing frame: a first-person camera is inside the pawn's head,
	 *                     pointing where she is aiming, which for a spike throw is a wall 1,248 uu away
	 *                     filling the screen. Measured — join3's client frames are three photographs of
	 *                     a black slab, taken while the log line beside them reported the rope at
	 *                     r 4.0 uu with all three pieces drawn.
	 */
	static void Run(UWorld* World, float Seconds, bool bWatch, const TCHAR* ShotPrefix = TEXT("W3RestrC_Rope"),
	                bool bSideCamera = false)
	{
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[RopeProbe] no world."));
			return;
		}

		const double EndTime = FPlatformTime::Seconds() + FMath::Clamp(Seconds, 1.f, 120.f);
		TWeakObjectPtr<UWorld> WeakWorld(World);

		UE_LOG(LogTraceGame, Display,
			TEXT("[RopeProbe] watching for %.0fs on netmode=%d (0 standalone, 2 listen, 3 client), watch=%d."),
			Seconds, static_cast<int32>(World->GetNetMode()), bWatch ? 1 : 0);

		TSharedRef<bool> bSeen = MakeShared<bool>(false);
		TSharedRef<bool> bViewed = MakeShared<bool>(false);
		TSharedRef<FString> Last = MakeShared<FString>();
		TSharedRef<int32> ShotsTaken = MakeShared<int32>(0);
		TSharedRef<double> NextShotTime = MakeShared<double>(0.0);

		const FString Prefix(ShotPrefix);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakWorld, EndTime, bSeen, bViewed, Last, bWatch, ShotsTaken, NextShotTime, Prefix, bSideCamera](float) -> bool
			{
				UWorld* PollWorld = WeakWorld.Get();
				if (PollWorld == nullptr)
				{
					return false;
				}

				for (TActorIterator<ATraceMaceSpike> It(PollWorld); It; ++It)
				{
					ATraceMaceSpike* Spike = *It;
					if (!IsValid(Spike))
					{
						continue;
					}

					*Last = Spike->DebugDescribe();
					if (!*bSeen)
					{
						*bSeen = true;
						UE_LOG(LogTraceGame, Display,
							TEXT("[RopeProbe] *** FIRST FRAME WITH A SPIKE *** %s"), **Last);
					}

					if (bWatch)
					{
						APlayerController* PC = PollWorld->GetFirstPlayerController();
						AActor* Look = Cast<AActor>(Spike->GetOwner());
						if (PC != nullptr && Look != nullptr && !*bViewed)
						{
							if (bSideCamera)
							{
								// A camera BESIDE the rope, at its midpoint. ACameraActor and not a bare
								// AActor: AActor has no root component, so a spawn transform has nothing
								// to write to and the "observer" reports (0,0,0) for ever.
								const FVector From = Look->GetActorLocation();
								const FVector To = Spike->GetActorLocation();
								const FVector Mid = 0.5f * (From + To);
								const FVector Along = (To - From).GetSafeNormal();
								const FVector Side = FVector::CrossProduct(Along, FVector::UpVector).GetSafeNormal();
								const float Back = FMath::Clamp(FVector::Dist(From, To) * 0.55f, 260.f, 900.f);
								const FVector At = Mid + Side * Back + FVector(0.f, 0.f, 140.f);

								FActorSpawnParameters Params;
								Params.ObjectFlags |= RF_Transient;
								if (ACameraActor* Cam = PollWorld->SpawnActor<ACameraActor>(
									ACameraActor::StaticClass(), At, (Mid - At).Rotation(), Params))
								{
									PC->SetViewTargetWithBlend(Cam, 0.f);
								}
							}
							else
							{
								PC->SetViewTargetWithBlend(Look, 0.f);
							}
							*bViewed = true;
							// A couple of frames for the view target to take, then the frames. The
							// AutoShot harness cannot be aimed at an event; this can, which is the
							// difference between a screenshot of the rope and a screenshot of the
							// moment after it was removed.
							*NextShotTime = FPlatformTime::Seconds() + 0.35;
							UE_LOG(LogTraceGame, Display,
								TEXT("[RopeProbe] view target -> %s (local only; the pawn is not moved). ")
								TEXT("Spike is %.0f uu from it."),
								*GetNameSafe(Look),
								FVector::Dist(Look->GetActorLocation(), Spike->GetActorLocation()));
						}

						// FIXED FILENAMES, so this run takes the capture lock (see the release
						// operational law). Three frames while a spike is genuinely alive.
						if (*bViewed && *ShotsTaken < 3 && FPlatformTime::Seconds() >= *NextShotTime)
						{
							const FString Path = FPaths::ConvertRelativePathToFull(
								FPaths::ProjectSavedDir() / TEXT("Screenshots")
								/ FString::Printf(TEXT("%s_%02d.png"), *Prefix, *ShotsTaken + 1));
							FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
							++(*ShotsTaken);
							*NextShotTime = FPlatformTime::Seconds() + 0.6;
							UE_LOG(LogTraceGame, Display,
								TEXT("[RopeProbe] Screenshot requested: %s  (with %s)"), *Path, **Last);
						}
					}
					break;
				}

				if (FPlatformTime::Seconds() < EndTime)
				{
					return true;
				}

				if (*bSeen)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[RopeProbe] last seen: %s"), **Last);
					UE_LOG(LogTraceGame, Display,
						TEXT("[RopeProbe] VERDICT: a spike was visible to this machine (see the first-frame line "
						     "for whether the rope was drawn on it)."));
				}
				else
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[RopeProbe] VERDICT: no spike ever existed on this machine in the window."));
				}
				return false;
			}), 0.f);
	}
}

static FAutoConsoleCommandWithWorldAndArgs CmdMaceRopeProbe(
	TEXT("Trace.Mace.RopeProbe"),
	TEXT("Dev only, runs anywhere (meant for a CLIENT). Trace.Mace.RopeProbe [seconds]: watches for a "
	     "Mace spike and reports the first frame it exists on this machine — position, launch facts, "
	     "resolved pawn and whether the rope is drawn."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World)
	{
		const float Seconds = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 20.f;
		TraceMaceRopeProbe::Run(World, Seconds, /*bWatch=*/false);
	}));

/**
 * The same probe, 40 s, and it also points this machine's camera at the thrower.
 *
 * A SEPARATE COMMAND NAME RATHER THAN AN ARGUMENT, deliberately: -TraceExec has to be one unquoted
 * command-line token on this project (a quoted value with a space has, on this codebase, been eaten
 * by the URL parser and produced a "passing" run whose commands never executed), so a headless client
 * can only be handed verbs with no arguments.
 */
static FAutoConsoleCommandWithWorld CmdMaceRopeProbeWatch(
	TEXT("Trace.Mace.RopeProbeWatch"),
	TEXT("Dev only, runs anywhere (meant for a CLIENT). Trace.Mace.RopeProbe for 40s, and makes the "
	     "spike's owner this machine's view target so a screenshot frames the rope."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		TraceMaceRopeProbe::Run(World, 40.f, /*bWatch=*/true);
	}));

/**
 * The same watch, writing DRESSING-named frames instead of C4's rope-named ones.
 *
 * A separate verb rather than an argument for the -TraceExec one-token reason above, and a separate
 * FILENAME because the two runs are evidence for two different claims: C4 proved the rope reaches a
 * client at all, and this proves what it LOOKS like when it gets there. Overwriting C4's frames with
 * a later tranche's would quietly destroy the earlier proof.
 */
static FAutoConsoleCommandWithWorld CmdMaceSpikeFxWatch(
	TEXT("Trace.Mace.SpikeFxWatch"),
	TEXT("Dev only, runs anywhere (meant for a CLIENT). Trace.Mace.RopeProbeWatch with the dressing "
	     "line in every report, writing Saved/Screenshots/W4KitsB_Spike_NN.png."),
	FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
	{
		TraceMaceRopeProbe::Run(World, 40.f, /*bWatch=*/true, TEXT("W4KitsB_Spike"), /*bSideCamera=*/true);
	}));

#endif // !UE_BUILD_SHIPPING
