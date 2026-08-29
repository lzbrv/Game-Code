#include "Gameplay/TraceMeleeArc.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/ConstructorHelpers.h"

#include "CollisionQueryParams.h"
#include "Engine/HitResult.h"
#include "Gameplay/TraceFxShapes.h"   // FX_AUDIO_PLAN §3 — the scuff quad's mesh, sizing and blend ladder
#include "Math/RotationMatrix.h"
#include "Trace.h"

// Named after the file, never anonymous: this module is a unity build and
// Scripts/check-jumbo-build-collisions.py gates on exactly that.
namespace TraceMeleeArcFile
{
	/**
	 * FX_AUDIO_PLAN §3 — THE KNIFE SCUFF, AND WHY IT IS NOT THE TRACER'S PLANE WITH A DIFFERENT HUE.
	 *
	 * "Melee wall hits: ATraceMeleeArc gains the same plane at the arc's terminal point when the
	 * swing resolved no victim but hit geometry within range — mint-less, neutral Ink white at 0.35
	 * opacity, 0.15 s (a knife scuff, not a laser)."
	 *
	 * Every one of those differences is doing work. NEUTRAL, because a blade has no energy colour and
	 * a team-tinted mark on a wall would read as somebody's ability rather than as a missed swing.
	 * DIMMER (0.35 against the tracer's 0.6) and SHORTER (0.15 s against 0.18 s), because a knife
	 * that missed is the least important event this effect system draws and it must not compete with
	 * the slash itself. "Ink" is the HUD's own near-white (0.95, 0.96, 1.00) — the project's one
	 * neutral, quoted here rather than re-picked so a repaint of the palette moves both.
	 */
	static const FLinearColor ScuffColor(0.95f, 0.96f, 1.00f, 1.00f);
	static constexpr float ScuffOpacity = 0.35f;
	static constexpr float ScuffSeconds = 0.15f;
	static constexpr float ScuffSizeUU = 26.0f;
	static constexpr float ScuffOffsetUU = 1.5f;

	/**
	 * The tag both scuff faces carry.
	 *
	 * *** IT IS HOW Tick FINDS THEM, AND THAT IS DELIBERATE RATHER THAN LAZY. *** The obvious
	 * implementation keeps two TObjectPtr members and a MID beside them — and Gameplay/TraceMeleeArc.h
	 * is not this pass's to edit (release tranche W4-SHOTS owns the .cpp). A component tag is the
	 * engine's own supported answer to "find the pieces I attached", the fan is nine components so
	 * the scan is nine comparisons on an actor that lives a fifth of a second, and the alternative —
	 * a file-static map keyed by actor — would be more state in a worse place. If a later pass owns
	 * the header, promoting these to members changes nothing else here.
	 */
	static const FName ScuffTag(TEXT("TraceMeleeScuff"));

	/**
	 * Places the scuff quad on the wall the swing's centre ray found, or does nothing.
	 *
	 * FREE FUNCTIONS RATHER THAN MEMBERS, and that is the header's fault rather than a preference:
	 * Gameplay/TraceMeleeArc.h is outside this pass's ownership line, so the scuff cannot declare
	 * itself on the class. Everything they need is passed in — the actor for the world and the
	 * origin, the root to attach to, and the age to fade against — and nothing is cached between
	 * them. If a later pass owns the header, these become two private members and nothing else moves.
	 */
	void BuildScuff(AActor* Owner, USceneComponent* Attach, const FVector& Forward, float RangeUU);

	/** Fades the scuff over ScuffSeconds and hides it at the end. Called from the arc's own Tick. */
	void UpdateScuff(USceneComponent* Attach, float Age);
}

ATraceMeleeArc::ATraceMeleeArc()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// Cosmetic-only: each machine spawns its own from the multicast, so this must never go on the
	// wire. Same contract as ATraceTracer.
	bReplicates = false;
	SetCanBeDamaged(false);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("ArcRoot"));
	SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);

	// Constructor-time finders (not runtime loads) so the cooker keeps these assets. MAP_PLAN §9:
	// M_TraceNeon is the COMMITTED parent under /Game/Trace/Materials/Parents — it used to be loaded
	// from the gitignored /Game/Generated tree, which is now deleted. A repository missing the parent
	// renders the slash on the lit fallback with no glow, which is dimmer but never missing.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Trace/Materials/Parents/M_TraceNeon.M_TraceNeon"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	if (CylinderFinder.Succeeded())
	{
		CylinderMesh = CylinderFinder.Object;
	}
	if (NeonFinder.Succeeded())
	{
		NeonMaterial = NeonFinder.Object;
	}
	if (BasicFinder.Succeeded())
	{
		FallbackMaterial = BasicFinder.Object;
	}

	// The segments are created as DEFAULT subobjects rather than at runtime, so the whole fan exists
	// on the CDO and spawning one costs a template copy instead of nine NewObject/RegisterComponent
	// round trips. A knife swing is rate-limited to one per 0.5 s per player, but ten bots swinging
	// is still ninety components a second and this is the cheap way to pay for them.
	Segments.Reserve(NumSegments);
	for (int32 Index = 0; Index < NumSegments; ++Index)
	{
		const FName SegmentName(*FString::Printf(TEXT("ArcSegment%d"), Index));
		UStaticMeshComponent* Segment = CreateDefaultSubobject<UStaticMeshComponent>(SegmentName);
		Segment->SetupAttachment(Root);
		Segment->SetMobility(EComponentMobility::Movable);

		// Visual only, and every one of these matters on something that spawns mid-fight: a
		// colliding slash would be an obstacle welded into a fight, and a shadow-casting one would
		// throw a moving shadow across the arena for a fifth of a second.
		Segment->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Segment->SetCollisionProfileName(TEXT("NoCollision"));
		Segment->SetGenerateOverlapEvents(false);
		Segment->SetCanEverAffectNavigation(false);
		Segment->SetCastShadow(false);
		Segment->bReceivesDecals = false;
		Segment->bUseAsOccluder = false;

		if (CylinderMesh != nullptr)
		{
			Segment->SetStaticMesh(CylinderMesh);
		}

		Segments.Add(Segment);
	}
}

ATraceMeleeArc* ATraceMeleeArc::Spawn(UWorld* World, const FVector& Origin, const FVector& Forward,
	const FVector& SwingAxis, float ArcDegrees, float RadiusUU, const FLinearColor& Color,
	bool bConnected)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	// A headless server has nobody to show this to.
	if (World->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}

	if (Origin.ContainsNaN() || Forward.ContainsNaN() || SwingAxis.ContainsNaN())
	{
		return nullptr;
	}
	if (Forward.IsNearlyZero() || SwingAxis.IsNearlyZero() || RadiusUU <= 1.f)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	ATraceMeleeArc* Arc = World->SpawnActor<ATraceMeleeArc>(
		ATraceMeleeArc::StaticClass(), FTransform(Origin), SpawnParams);
	if (Arc != nullptr)
	{
		Arc->Build(Forward, SwingAxis, ArcDegrees, RadiusUU, Color, bConnected);
	}
	return Arc;
}

void ATraceMeleeArc::Build(const FVector& Forward, const FVector& SwingAxis, float ArcDegrees,
	float RadiusUU, const FLinearColor& Color, bool bConnected)
{
	SlashColor = Color;

	// A connecting swing flares harder and hangs a little longer. That is the ONLY feedback a
	// bystander (or the victim) gets that a hundred damage just happened, so it is deliberately a
	// visible step rather than a subtle one.
	PeakGlow = bConnected ? 9.f : 5.f;
	LifeSeconds = bConnected ? 0.26f : 0.20f;
	InitialLifeSpan = LifeSeconds + 0.05f;

	const FVector Axis = SwingAxis.GetSafeNormal();
	const FVector Centre = Forward.GetSafeNormal();
	const float Radius = RadiusUU * BladeRadiusFraction;
	const float HalfArc = FMath::Clamp(ArcDegrees, 5.f, 270.f) * 0.5f;

	// Cylinders are laid along the CHORD between consecutive arc points rather than at the arc
	// points themselves, so consecutive segments meet end to end and the fan reads as one continuous
	// cut instead of as a dotted line.
	SegmentMIDs.Reset();
	SegmentMIDs.SetNum(Segments.Num());

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		UStaticMeshComponent* Segment = Segments[Index];
		if (Segment == nullptr)
		{
			continue;
		}

		const float AlphaA = (static_cast<float>(Index) / static_cast<float>(NumSegments)) * 2.f - 1.f;
		const float AlphaB = (static_cast<float>(Index + 1) / static_cast<float>(NumSegments)) * 2.f - 1.f;

		const FVector PointA = Centre.RotateAngleAxis(AlphaA * HalfArc, Axis) * Radius;
		const FVector PointB = Centre.RotateAngleAxis(AlphaB * HalfArc, Axis) * Radius;

		const FVector Chord = PointB - PointA;
		const float Length = static_cast<float>(Chord.Size());
		if (Length < 0.5f)
		{
			Segment->SetVisibility(false);
			continue;
		}

		// MakeFromZ because the basic cylinder's own axis is +Z; this turns that axis onto the
		// chord without hand-deriving a rotator. /Engine/BasicShapes primitives are 100 uu, hence
		// the divisions.
		Segment->SetRelativeLocationAndRotation(
			PointA + Chord * 0.5f,
			FRotationMatrix::MakeFromZ(Chord / Length).Rotator());
		Segment->SetRelativeScale3D(FVector(
			SegmentThicknessUU / 100.f, SegmentThicknessUU / 100.f, Length / 100.f));

		UMaterialInterface* Source = (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get();
		if (Source != nullptr)
		{
			SegmentMIDs[Index] = Segment->CreateDynamicMaterialInstance(0, Source);
		}

		// Dark on frame one. The sweep below is what lights them, in order — see the header.
		Segment->SetVisibility(true);
		PushSegment(Index, 0.f);
	}

	bHaveNeonParameters = (NeonMaterial != nullptr);

	if (Segments.Num() > 0 && SegmentMIDs.Num() > 0 && SegmentMIDs[0] == nullptr)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("ATraceMeleeArc: no material resolved; the slash will render untinted."));
	}

	// --- FX_AUDIO_PLAN §3: THE SCUFF, ON A MISS THAT FOUND A WALL -------------------------------
	//
	// *** ONLY WHEN THE SWING RESOLVED NO VICTIM. *** bConnected is the server's own verdict, carried
	// here for the flare two beats up, and it is the same "never on bodies" rule the tracer's plane
	// obeys — a blade that landed on somebody has a slash, a flare and a hundred damage to say so,
	// and a mark drawn on top of the person you just knifed is the deleted impact sphere's mistake in
	// a different shape (spec v4 §4).
	//
	// "WITHIN RANGE" IS THE RESOLVER'S RANGE, not a number of this file's own: the probe runs exactly
	// as far as the swing did, so a scuff can never appear where the blade could not have reached.
	// The trace is one ray down the arc's CENTRE rather than the fifteen-sample fan the resolver
	// sweeps — a scuff is a decoration and the centre is where the eye is, and fifteen extra rays per
	// missed swing to place a 26 uu mark 2 uu more accurately is not a trade worth making.
	if (!bConnected)
	{
		TraceMeleeArcFile::BuildScuff(this, Root, Centre, RadiusUU);
	}
}

void TraceMeleeArcFile::BuildScuff(AActor* Owner, USceneComponent* Attach, const FVector& Forward, float RangeUU)
{
	UWorld* World = (Owner != nullptr) ? Owner->GetWorld() : nullptr;
	UStaticMesh* Plane = UTraceFxShapes::GetPlane();
	if (World == nullptr || Attach == nullptr || Plane == nullptr || Forward.IsNearlyZero() || RangeUU <= 1.f)
	{
		return;
	}

	// ECC_Visibility for the reason TraceTracer's probe uses it: a character capsule's Pawn profile
	// IGNORES that channel, so this ray can only ever return world geometry. It therefore cannot
	// place a scuff on a pawn even if the swing's own resolver disagreed with it about who was where.
	const FVector Origin = Owner->GetActorLocation();
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceMeleeScuff), /*bTraceComplex=*/false);
	if (!World->LineTraceSingleByChannel(Hit, Origin, Origin + Forward * RangeUU, ECC_Visibility, Params))
	{
		return;
	}

	const FVector Normal = Hit.ImpactNormal.IsNearlyZero() ? -Forward : Hit.ImpactNormal.GetSafeNormal();
	const FQuat Facing = FRotationMatrix::MakeFromZ(Normal).ToQuat();
	const FVector Where = Hit.ImpactPoint + Normal * ScuffOffsetUU;

	// ADDITIVE OR NOTHING, exactly as the tracer's plane decides it: an opaque quad on a wall is a
	// hole punched in the architecture that stays full-size while its emissive fades. No additive
	// material, no scuff — which is what shipped before this pass and is not a regression.
	UMaterialInstanceDynamic* ScuffMID = nullptr;
	ETraceFxBlend Blend = ETraceFxBlend::None;

	// TWO FACES, ONE QUAD, ONE MID — see ATraceTracer::ImpactPlanes for the single-sided-plane
	// argument. Created at runtime rather than as default subobjects because every OTHER swing (the
	// ones that connect, and the ones that miss into open air) would carry them for nothing, and a
	// knife is rate-limited to one swing per half second per player.
	for (int32 Face = 0; Face < 2; ++Face)
	{
		UStaticMeshComponent* Piece = NewObject<UStaticMeshComponent>(Owner);
		if (Piece == nullptr)
		{
			return;
		}
		Piece->SetupAttachment(Attach);
		UTraceFxShapes::ConfigureFxComponent(Piece);
		Piece->SetStaticMesh(Plane);
		Piece->ComponentTags.Add(ScuffTag);
		Piece->RegisterComponent();

		if (Face == 0)
		{
			ScuffMID = UTraceFxShapes::MakeGlowMID(Piece, 0, ETraceFxBlend::Translucent, Blend);
			if (ScuffMID == nullptr || (Blend != ETraceFxBlend::Additive && Blend != ETraceFxBlend::Translucent))
			{
				Piece->DestroyComponent();
				return;
			}
		}
		else
		{
			Piece->SetMaterial(0, ScuffMID);
		}

		UTraceFxShapes::SizePlane(Piece, ScuffSizeUU, ScuffSizeUU);
		Piece->SetWorldLocationAndRotation(Where,
			(Face == 0) ? Facing : Facing * FQuat(FRotator(180.f, 0.f, 0.f)));
		Piece->SetVisibility(true);
	}

	// Full opacity on frame one; Tick fades it. Unlike the tracer's plane there is nothing to wait
	// for — a knife has no travelling bolt, the blade is already at the wall when the arc is drawn.
	UTraceFxShapes::SetGlow(ScuffMID, Blend, ScuffColor, 1.f, ScuffOpacity);
}

void TraceMeleeArcFile::UpdateScuff(USceneComponent* Attach, float Age)
{
	if (Attach == nullptr)
	{
		return;
	}

	// Age is the ACTOR's, and for the scuff that is exactly right: the mark is placed in Build, which
	// runs before the first tick, so the actor's age and the mark's age are the same number and there
	// is no second clock to keep. See ScuffTag for why the components are found rather than held.
	const float Alpha = FMath::Clamp(Age / ScuffSeconds, 0.f, 1.f);

	for (USceneComponent* Child : Attach->GetAttachChildren())
	{
		UStaticMeshComponent* Piece = Cast<UStaticMeshComponent>(Child);
		if (Piece == nullptr || !Piece->ComponentTags.Contains(ScuffTag))
		{
			continue;
		}

		if (Alpha >= 1.f)
		{
			Piece->SetVisibility(false);
			continue;
		}

		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Piece->GetMaterial(0)))
		{
			// The blend is re-read off the MID's own parent rather than remembered, because the only
			// two it can be here are the two MakeGlowMID accepted in BuildScuff, and SetGlow needs
			// the ACHIEVED one. Additive is what BuildScuff insists on; Translucent takes the same
			// branch inside SetGlow.
			UTraceFxShapes::SetGlow(MID, ETraceFxBlend::Additive, ScuffColor, 1.f,
				ScuffOpacity * (1.f - Alpha));
		}
	}
}

void ATraceMeleeArc::PushSegment(int32 Index, float Intensity)
{
	if (!SegmentMIDs.IsValidIndex(Index) || SegmentMIDs[Index] == nullptr)
	{
		return;
	}

	UMaterialInstanceDynamic* MID = SegmentMIDs[Index];

	// The blade's edge is hotter than its tint, exactly like the tracer's near-white core: pushing
	// the colour toward white as the intensity rises is what makes it read as BRIGHT rather than as
	// merely saturated once the tonemapper has had it.
	const FLinearColor Hot = FMath::Lerp(SlashColor, FLinearColor::White, 0.45f);

	MID->SetVectorParameterValue(TEXT("Color"), Hot);
	MID->SetScalarParameterValue(TEXT("Glow"), FMath::Max(0.f, Intensity));

	if (!bHaveNeonParameters)
	{
		// BasicShapeMaterial fallback: lit, "Color" only, no Glow scalar. Fold the intensity into
		// the colour so the sweep is still visible, just without the bloom.
		MID->SetVectorParameterValue(TEXT("Color"), Hot * FMath::Clamp(Intensity / FMath::Max(1.f, PeakGlow), 0.f, 1.f));
	}
}

void ATraceMeleeArc::BeginPlay()
{
	Super::BeginPlay();
}

void ATraceMeleeArc::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	Age += DeltaSeconds;
	const float Life = FMath::Max(0.01f, LifeSeconds);
	const float Alpha = FMath::Clamp(Age / Life, 0.f, 1.f);

	// FX_AUDIO_PLAN §3 — the wall scuff's own, shorter fade. BEFORE the destroy test, so the last
	// frame of the mark is drawn; its 0.15 s is comfortably inside the arc's own 0.20 s life, so the
	// mark is always gone before the actor is and there is nothing to leak.
	TraceMeleeArcFile::UpdateScuff(Root, Age);

	if (Alpha >= 1.f)
	{
		Destroy();
		return;
	}

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		// Each segment ignites at its own point in the sweep, so the slash draws itself across the
		// arc — that is the beat that carries the DIRECTION of the swing, which is the only thing a
		// victim can actually learn from it.
		const float IgniteAt = SweepFraction * (static_cast<float>(Index) / static_cast<float>(FMath::Max(1, NumSegments - 1)));
		if (Alpha < IgniteAt)
		{
			PushSegment(Index, 0.f);
			continue;
		}

		// Decay measured from ITS OWN ignition, so the leading edge is brightest and the tail is
		// already dying. A uniform fade reads as a light switching off; a trailing one reads as
		// motion.
		const float SinceIgnition = (Alpha - IgniteAt) / FMath::Max(0.05f, 1.f - IgniteAt);
		const float Falloff = FMath::Square(1.f - FMath::Clamp(SinceIgnition, 0.f, 1.f));
		PushSegment(Index, PeakGlow * Falloff);
	}
}
