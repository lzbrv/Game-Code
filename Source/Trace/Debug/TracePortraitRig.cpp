// =================================================================================================
// Trace — TracePortraitRig.cpp.  See TracePortraitRig.h.
// =================================================================================================

#include "Debug/TracePortraitRig.h"

#if !UE_BUILD_SHIPPING

#include "Core/TraceCharacterRoster.h"
#include "Trace.h"

#include "Animation/SkeletalMeshActor.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/GameViewportClient.h"
#include "Engine/PointLight.h"
#include "Engine/Scene.h"                     // EAutoExposureMethod, ELightUnits
#include "Engine/SkeletalMesh.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"                 // FScreenshotRequest

// The namespace is named after the file rather than left anonymous, for the reason every other
// debug TU in this module states: UBT compiles this module as a unity/jumbo build, two files that
// each say `namespace { }` become one namespace with two definitions of everything they share a
// name with, and this project has already lost a build that way.
// Scripts/check-jumbo-build-collisions.py gates it.
namespace TracePortraitRigLocal
{
	// =============================================================================================
	// THE LIVE FRAMING KNOBS — THE ONE REASON THIS FILE HAS CVARS AT ALL
	// =============================================================================================
	//
	// *** THE ACCEPTANCE GATE FOR THIS RIG IS A MEASUREMENT ON A FRAME, NOT A NUMBER IN THIS FILE.
	// *** ART_BIBLE §7.5 asks for a chest-up bust whose HEAD IS 38% OF FRAME HEIGHT, with identical
	// margins across all ten. Nobody can compute that from the camera transform: it depends on the
	// generated bodies' actual proportions, which are the output of another stage entirely. So it is
	// found by shooting a frame, measuring it, moving the camera and shooting again — and the
	// numbers below are STARTING VALUES (PIPELINE_DESIGN.md §8.2's own word), not settled ones.
	//
	// A rebuild per iteration of that loop is ten minutes of link time to move a camera 20 uu — and in
	// a wave where seven other tranches share this tree, it is ten minutes that may not even end in a
	// green link. Every value below is therefore a knob: a `-dpcvars=` on the next launch, or a line
	// typed into the console followed by `Trace.Portrait.Rig 1`. That is the same trade every other
	// look-tuning value in this module is on (Trace.Hands.GloveFloor, Trace.Core.FxGeometry,
	// Trace.Fx.BeamScale): a value judged by LOOKING at a frame lives on a knob. The knobs are the
	// camera (CamDist / CamZ / LookZ / Fov), the exposure, and the three light levels + bloom, because
	// framing and lighting were both wrong at the starting values and both are judged the same way.
	//
	// ONCE THE 38% IS MEASURED, THE RIG IS FROZEN — the ten frames must come from one set-up, so the
	// tuned numbers are written back into the defaults here and the knobs go back to being defaults.

	// *** FROZEN 2026-08-25 AGAINST A MEASUREMENT, NOT AN OPINION. ***
	// 226 / 162.5 put the frame at world Z [116.23, 208.77] -- 92.54 uu tall, measured by the
	// projection probe below and cross-checked against the pixels of all ten frames. Rocco is
	// frame 1 and his head (the geometry bound to the `head` bone: helmet 152 -> crest tip
	// 187.31, 35.31 uu) is 38.16% of that. ART_BIBLE §7.5 asks for 38%. Do not move these two
	// numbers without re-running Scripts/compose_portraits.py --log, which re-measures the gate
	// and FAILS if it is missed.
	//
	// The top edge at 208.77 is not incidental either: it is the lowest edge that keeps EVERY
	// crown ornament in frame (Lily's fin tips reach 207.67, X's top bead 206.00). Lowering it
	// buys headroom on the six characters that have no crown and costs the three most legible
	// silhouettes in the roster their tips.
	TAutoConsoleVariable<float> CVarPortraitCamDist(
		TEXT("Trace.Portrait.CamDist"), 226.f,
		TEXT("Portrait rig: camera distance in uu along -X from the subject. Bigger = wider shot. ")
		TEXT("FROZEN at the value that measures 38.16% head height on frame 1 (ART_BIBLE §7.5)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarPortraitCamZ(
		TEXT("Trace.Portrait.CamZ"), 162.5f,
		TEXT("Portrait rig: camera height in uu above the subject's soles. Also the look-at height, ")
		TEXT("so the camera stays level, unless Trace.Portrait.LookZ overrides it. FROZEN: this ")
		TEXT("puts the frame's top edge at Z 208.77, just above the tallest crown in the roster."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarPortraitLookZ(
		TEXT("Trace.Portrait.LookZ"), -1.f,
		TEXT("Portrait rig: look-at height in uu. Negative (default) means 'the same as ")
		TEXT("Trace.Portrait.CamZ', i.e. a level camera. Set it to tilt without moving the lens."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarPortraitFov(
		TEXT("Trace.Portrait.Fov"), 40.f,
		TEXT("Portrait rig: horizontal field of view in degrees. ART_BIBLE §7.5 asks for ~40; it is ")
		TEXT("live because head-height and margins are solved by distance AND lens together."),
		ECVF_Default);

	/**
	 * *** EXPOSURE IS PINNED, AND THAT IS A CORRECTNESS REQUIREMENT RATHER THAN A LOOK CHOICE. ***
	 *
	 * Auto-exposure adapts to what is in frame. Ten subjects with ten different accent rim colours,
	 * different silhouette areas and different amounts of bright trim would each pull the eye
	 * adaptation somewhere slightly different, and the set would come out at ten exposures — which is
	 * precisely the "provably ONE set" requirement failing in the one way a per-frame check could not
	 * see, because each frame on its own would look fine.
	 *
	 * Pinned by setting the auto-exposure min and max brightness to the same value, which is the
	 * engine's own way of saying "do not adapt" while leaving the tone mapper alone. 0 disables the
	 * pin entirely and hands the frames back to auto-exposure, which is the A/B if a composite ever
	 * comes out clipped and somebody needs to see whether the pin is the cause.
	 */
	TAutoConsoleVariable<float> CVarPortraitExposure(
		TEXT("Trace.Portrait.Exposure"), 6.f,
		TEXT("Portrait rig: pinned auto-exposure brightness in EV100 (min == max). Bigger = DARKER ")
		TEXT("(one whole stop per unit). 0 leaves auto-exposure alone, which un-pins the set and is ")
		TEXT("only useful as a diagnostic A/B."),
		ECVF_Default);

	/**
	 * *** THE THREE LIGHTS ARE KNOBS FOR THE SAME REASON THE CAMERA IS: THEIR RATIO IS JUDGED BY
	 *     LOOKING. ***
	 *
	 * PIPELINE §8.2 gives key 8 / rim 16 / fill 1500 and calls the fill "≈0.3× the key's read". In
	 * candelas at this rig's distances that is not what those numbers do: the fill sits 1.7 m from
	 * the chest and the key 2.6 m, so 1500 cd of fill delivers ~500 lux against the key's ~1.2 lux —
	 * the "faint cyan bounce" was out-lighting the key by better than two orders of magnitude, and
	 * W3-CHARWIRE's smoke frames show exactly that: ten cyan-washed bodies with no cool-white key
	 * read anywhere on them. The intent in §8.2's prose (key dominant and cool, rim 2× key in the
	 * accent, fill a faint bounce) is the spec; the three literals were not measured against it.
	 *
	 * So the ratio is re-derived here by eye against the bible's description, and lives on knobs so
	 * the next person can re-judge it without a ten-minute link. The UNITS are pinned to candelas
	 * explicitly (`SetIntensityUnits`) rather than inherited from the project's light-unit default,
	 * because a rig whose exposure depends on a project setting is not "provably one set" the day
	 * someone flips that setting.
	 */
	TAutoConsoleVariable<float> CVarPortraitKey(
		TEXT("Trace.Portrait.KeyIntensity"), 180.f,
		TEXT("Portrait rig: key spot light intensity in CANDELAS (cool white, upper-left)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarPortraitRim(
		TEXT("Trace.Portrait.RimIntensity"), 450.f,
		TEXT("Portrait rig: accent rim spot intensity in CANDELAS. ART_BIBLE §7.5 wants 2x key — ")
		TEXT("the rim is where the character accent lives."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarPortraitFill(
		TEXT("Trace.Portrait.FillIntensity"), 18.f,
		TEXT("Portrait rig: cyan bounce point light intensity in CANDELAS. It sits closer than the ")
		TEXT("key, so its number is smaller than the key's for the same read."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarPortraitBloom(
		TEXT("Trace.Portrait.Bloom"), 1.9f,
		TEXT("Portrait rig: bloom intensity. 1.9 is the arena's own value; the team-glow slabs are ")
		TEXT("emissive 1.7 and bloom is what decides whether they read as glow or as clipped white."),
		ECVF_Default);

	// =============================================================================================
	// THE RIG'S FIXED GEOMETRY — everything that is NOT a framing knob
	// =============================================================================================

	/**
	 * *** THE RIG LIVES 50 000 uu UNDER THE MENU MAP, AND THAT IS THE WHOLE ISOLATION STRATEGY. ***
	 * The menu map is a UI map: a black world with a Slate/Canvas menu drawn over it. Fifty thousand
	 * units down there is provably nothing within 40 000 uu of the subject, so the backdrop plane is
	 * the only thing the camera can see besides the body — no arena geometry, no stray lights, no
	 * skybox. That is cheaper and far more certain than trying to hide a map.
	 */
	const FVector RigOrigin(0.f, 0.f, -50000.f);

	/**
	 * Subject yaw. The mesh is authored facing +Y, so a 120 degree actor yaw puts its world facing at
	 * 210 degrees — 30 degrees screen-left of the camera's -X line, which is ART_BIBLE §7.5's
	 * "yawed 30 degrees screen-left, the character's RIGHT side toward camera".
	 */
	constexpr float SubjectYawDeg = 120.f;

	/** Key light: the arena Key's cool identity (TraceArenaBuilder.cpp:4090-4144), 45 degrees upper-left. */
	const FVector KeyOffset(-180.f, -140.f, 260.f);
	const FLinearColor KeyColor(0.92f, 0.96f, 1.00f);

	/** Rim: back-right, in the character's ACCENT, at 2x key — "the rim is where the accent lives". */
	const FVector RimOffset(140.f, 160.f, 200.f);

	/** Fill: the faint cyan bounce from below. NeonNeutral. */
	const FVector FillOffset(-120.f, 60.f, 40.f);
	const FLinearColor FillColor(0.18f, 0.78f, 1.00f);

	// The three intensities that used to live here are Trace.Portrait.{Key,Rim,Fill}Intensity above.
	// Only the POSITIONS and COLOURS are fixed geometry; the levels are a look judgement.

	/** Both spots aim here — chest height, so the falloff lands on the bust and not on the floor. */
	const FVector LightAim(0.f, 0.f, 150.f);

	/**
	 * Backdrop: a 6 m plane 220 uu behind the subject, vertical, facing -X (the camera). Painted
	 * PlateFill — the menu's own plate colour, linear (0.0114, 0.0222, 0.0822) = sRGB #1D2951,
	 * TraceMenuArtStyle.h:209 — so the portrait ground is the SAME blue the card it will sit on is,
	 * and the composite step's radial falloff darkens it toward #0A0E1A at the corners.
	 */
	const FVector BackdropOffset(220.f, 0.f, 150.f);
	const FLinearColor BackdropColor(0.0114f, 0.0222f, 0.0822f, 1.f);
	constexpr float BackdropRoughness = 0.6f;
	constexpr float BackdropScale = 6.f;

	/** The arena's own bloom (TraceArenaBuilder.cpp:4214-4328), so a portrait glows like the game does. */
	constexpr float BloomThreshold = -0.35f;
	constexpr float BloomSizeScale = 3.f;

	/** Per-character cadence: dress, settle, shoot, next. */
	constexpr float StepSeconds = 1.6f;
	constexpr float SettleSeconds = 1.2f;

	const TCHAR* const PlaneMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");
	const TCHAR* const SurfaceMaterialPath = TEXT("/Game/Trace/Materials/Parents/M_TraceSurface.M_TraceSurface");

	// =============================================================================================
	// STATE
	// =============================================================================================

	/**
	 * The rig, and the CaptureAll sequence's place in it.
	 *
	 * WEAK POINTERS THROUGHOUT, keyed on the world: the actors are transient and die with the level,
	 * and a travel between two CaptureAll runs must not leave this struct holding ten dangling
	 * pointers that look built. Everything below re-spawns anything it finds missing, so a torn-down
	 * rig is indistinguishable from one that was never built.
	 */
	struct FPortraitRigState
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ASkeletalMeshActor> Subject;
		TWeakObjectPtr<ACameraActor> Camera;
		TWeakObjectPtr<ASpotLight> Key;
		TWeakObjectPtr<ASpotLight> Rim;
		TWeakObjectPtr<APointLight> Fill;
		TWeakObjectPtr<AStaticMeshActor> Backdrop;
		TWeakObjectPtr<UMaterialInstanceDynamic> BackdropMID;

		// CaptureAll bookkeeping.
		FTimerHandle StepTimer;
		FTimerHandle ShotTimer;
		FString OutDir;
		uint8 NextId = TraceCharacterRoster::FirstId;
		bool bCapturing = false;

		/** Ids whose frame was actually requested, in order, so the DONE sweep knows what to look for. */
		TArray<uint8> Requested;

		/** The HUD's own bShowHUD, put back when the run ends. See BeginCaptureAll. */
		TWeakObjectPtr<AHUD> HiddenHUD;
		bool bRestoreShowHUD = false;
	};

	FPortraitRigState& Rig()
	{
		static FPortraitRigState State;
		return State;
	}

	APlayerController* LocalController(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (PC != nullptr && PC->IsLocalController())
			{
				return PC;
			}
		}
		return nullptr;
	}

	FString ResolveOutDir(const FString& Requested)
	{
		const FString Dir = Requested.IsEmpty()
			? (FPaths::ProjectSavedDir() / TEXT("Portraits"))
			: Requested;
		return FPaths::ConvertRelativePathToFull(Dir);
	}

	/**
	 * The roster's own name, title-cased. ONE definition of "what is this character called in a
	 * filename" so that the capture, Scripts/compose_portraits.py and T_Portrait_<Name> cannot
	 * disagree — the roster prints ROCCO for a card, the asset set is written Rocco.
	 */
	FString FileNameFor(uint8 CharacterId)
	{
		const FString Name = TraceCharacterRoster::NameFor(CharacterId);
		return Name.Left(1).ToUpper() + Name.Mid(1).ToLower();
	}

	FString RawFileFor(const FString& OutDir, uint8 CharacterId)
	{
		return OutDir / FString::Printf(TEXT("raw_%s.png"), *FileNameFor(CharacterId));
	}

	// =============================================================================================
	// BUILDING AND DRESSING
	// =============================================================================================

	/** Spawns @p T transient at RigOrigin + @p Offset if @p Existing is dead; returns the live actor. */
	template <typename T>
	T* EnsureActor(UWorld* World, TWeakObjectPtr<T>& Existing, const FVector& Offset, const FRotator& Rotation)
	{
		T* Actor = Existing.Get();
		if (Actor == nullptr)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Actor = World->SpawnActor<T>(T::StaticClass(), RigOrigin + Offset, Rotation, Params);
			Existing = Actor;
		}
		else
		{
			Actor->SetActorLocationAndRotation(RigOrigin + Offset, Rotation);
		}
		return Actor;
	}

	void ConfigureSpot(ASpotLight* Light, const FVector& Offset, const FLinearColor& Color, float Intensity)
	{
		if (Light == nullptr)
		{
			return;
		}
		Light->SetActorLocationAndRotation(RigOrigin + Offset, ((LightAim - Offset).Rotation()));
		if (USpotLightComponent* Comp = Cast<USpotLightComponent>(Light->GetLightComponent()))
		{
			// Movable for the reason every light this project makes is: static lighting is disabled
			// outright (r.AllowStaticLighting=False) and nothing here has baked data.
			Comp->SetMobility(EComponentMobility::Movable);
			// Units before intensity: SetIntensityUnits re-interprets the number that is already
			// there, so setting them the other way round changes the light twice and lands somewhere
			// nobody asked for.
			Comp->SetIntensityUnits(ELightUnits::Candelas);
			Comp->SetIntensity(Intensity);
			Comp->SetLightColor(Color);
			Comp->SetAttenuationRadius(3000.f);
			Comp->SetInnerConeAngle(28.f);
			Comp->SetOuterConeAngle(50.f);
			Comp->SetCastShadows(true);
			Comp->SetVolumetricScatteringIntensity(0.f);
		}
	}

	/**
	 * Builds every part of the rig that does NOT change per character, and re-reads the framing
	 * knobs. Called from Dress() on every character, so moving a CVar and re-running
	 * Trace.Portrait.Rig re-frames without a rebuild — which is the whole point of the knobs.
	 */
	bool EnsureRig(UWorld* World)
	{
		FPortraitRigState& State = Rig();

		// A DIFFERENT WORLD MEANS A DIFFERENT RIG. Travel destroys the actors; holding the old weak
		// pointers would be harmless but the OutDir/sequence state would not be, so it is all reset
		// together rather than piecemeal.
		if (State.World.Get() != World)
		{
			State = FPortraitRigState();
			State.World = World;
		}

		// --- subject -----------------------------------------------------------------------------
		ASkeletalMeshActor* Subject = EnsureActor<ASkeletalMeshActor>(
			World, State.Subject, FVector::ZeroVector, FRotator(0.f, SubjectYawDeg, 0.f));
		if (Subject == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Portrait] could not spawn the subject actor."));
			return false;
		}
		if (USkeletalMeshComponent* SubjectMesh = Subject->GetSkeletalMeshComponent())
		{
			SubjectMesh->SetMobility(EComponentMobility::Movable);
			// NO ANIM INSTANCE ON PURPOSE: the reference pose is the A-pose, which reads correctly
			// chest-up, and a portrait must not depend on the retarget stage having run. It also means
			// the ten frames are ten identical POSES, which "identical margins" quietly requires.
			SubjectMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			SubjectMesh->SetCastShadow(true);
		}

		// --- camera ------------------------------------------------------------------------------
		const float CamDist = FMath::Max(1.f, CVarPortraitCamDist.GetValueOnGameThread());
		const float CamZ = CVarPortraitCamZ.GetValueOnGameThread();
		const float LookZRaw = CVarPortraitLookZ.GetValueOnGameThread();
		const float LookZ = (LookZRaw < 0.f) ? CamZ : LookZRaw;
		const FVector CamOffset(-CamDist, 0.f, CamZ);
		const FRotator CamRotation = (FVector(0.f, 0.f, LookZ) - CamOffset).Rotation();

		ACameraActor* Camera = EnsureActor<ACameraActor>(World, State.Camera, CamOffset, CamRotation);
		if (Camera == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Portrait] could not spawn the rig camera."));
			return false;
		}
		if (UCameraComponent* CamComp = Camera->GetCameraComponent())
		{
			CamComp->SetFieldOfView(FMath::Clamp(CVarPortraitFov.GetValueOnGameThread(), 5.f, 170.f));
			CamComp->SetConstraintAspectRatio(false);

			// *** THE POST LIVES ON THE CAMERA, NOT IN AN APostProcessVolume, AND THE DEVIATION IS
			//     DELIBERATE. *** A bounded post-process volume is an ABrush, and a brush's shape is
			//     BSP that only the editor can author — a game build cannot spawn one with a box in
			//     it. An unbound volume would work but would apply to every view in the process, which
			//     is a global side effect for a rig that is meant to be invisible to everything else.
			//     Camera post is exactly equivalent for a single-camera capture and is scoped to the
			//     one view that takes the picture.
			FPostProcessSettings& Post = CamComp->PostProcessSettings;
			Post.bOverride_BloomIntensity = true;
			Post.BloomIntensity = FMath::Max(0.f, CVarPortraitBloom.GetValueOnGameThread());
			Post.bOverride_BloomThreshold = true;
			Post.BloomThreshold = BloomThreshold;
			Post.bOverride_BloomSizeScale = true;
			Post.BloomSizeScale = BloomSizeScale;
			Post.bOverride_VignetteIntensity = true;
			Post.VignetteIntensity = 0.f;
			Post.bOverride_SceneFringeIntensity = true;
			Post.SceneFringeIntensity = 0.f;

			// *** THE PIN ONLY PINS IF THE METHOD IS ALSO OVERRIDDEN. ***
			// Config/DefaultEngine.ini sets r.DefaultFeature.AutoExposure=False, which makes the
			// engine's DEFAULT exposure method Manual — and Manual ignores Min/MaxBrightness entirely
			// and reads the physical camera instead. Setting the two brightnesses without also
			// claiming the method leaves the whole pin at the mercy of a project setting and of
			// whatever post-process volume the map happens to carry. Claiming AEM_Basic here makes
			// this rig's exposure a property of THIS rig, which is what "provably one set" means.
			const float Exposure = CVarPortraitExposure.GetValueOnGameThread();
			const bool bPin = (Exposure > 0.f);
			Post.bOverride_AutoExposureMethod = bPin;
			Post.bOverride_AutoExposureMinBrightness = bPin;
			Post.bOverride_AutoExposureMaxBrightness = bPin;
			Post.bOverride_AutoExposureBias = bPin;
			if (bPin)
			{
				Post.AutoExposureMethod = EAutoExposureMethod::AEM_Basic;
				Post.AutoExposureMinBrightness = Exposure;
				Post.AutoExposureMaxBrightness = Exposure;
				Post.AutoExposureBias = 0.f;
			}

			CamComp->PostProcessBlendWeight = 1.f;
		}

		// --- lights ------------------------------------------------------------------------------
		ConfigureSpot(EnsureActor<ASpotLight>(World, State.Key, KeyOffset, FRotator::ZeroRotator),
			KeyOffset, KeyColor, CVarPortraitKey.GetValueOnGameThread());
		// The rim's COLOUR is per character and is written by Dress(); its transform is not.
		ConfigureSpot(EnsureActor<ASpotLight>(World, State.Rim, RimOffset, FRotator::ZeroRotator),
			RimOffset, FLinearColor::White, CVarPortraitRim.GetValueOnGameThread());

		if (APointLight* Fill = EnsureActor<APointLight>(World, State.Fill, FillOffset, FRotator::ZeroRotator))
		{
			if (UPointLightComponent* Comp = Cast<UPointLightComponent>(Fill->GetLightComponent()))
			{
				Comp->SetMobility(EComponentMobility::Movable);
				Comp->SetIntensityUnits(ELightUnits::Candelas);
				Comp->SetIntensity(CVarPortraitFill.GetValueOnGameThread());
				Comp->SetLightColor(FillColor);
				Comp->SetAttenuationRadius(1200.f);
				Comp->SetCastShadows(false);
				Comp->SetVolumetricScatteringIntensity(0.f);
			}
		}

		// --- backdrop ----------------------------------------------------------------------------
		//
		// FRotator(90, 0, 0) is what turns the engine plane — authored in XY with its normal on +Z —
		// to face -X, i.e. at the camera: a pitch of +90 carries local up onto world -X. Worked out
		// rather than guessed, because a backdrop facing the wrong way is a black frame with no error
		// in the log, which is the most expensive kind of wrong here.
		AStaticMeshActor* Backdrop = EnsureActor<AStaticMeshActor>(
			World, State.Backdrop, BackdropOffset, FRotator(90.f, 0.f, 0.f));
		if (Backdrop != nullptr)
		{
			if (UStaticMeshComponent* Comp = Backdrop->GetStaticMeshComponent())
			{
				Comp->SetMobility(EComponentMobility::Movable);
				if (Comp->GetStaticMesh() == nullptr)
				{
					if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, PlaneMeshPath))
					{
						Comp->SetStaticMesh(Plane);
					}
					else
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[Portrait] %s did not load; the portraits will have no backdrop."), PlaneMeshPath);
					}
				}
				Comp->SetRelativeScale3D(FVector(BackdropScale, BackdropScale, BackdropScale));
				Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Comp->SetCastShadow(false);

				if (State.BackdropMID.Get() == nullptr)
				{
					if (UMaterialInterface* Surface = LoadObject<UMaterialInterface>(nullptr, SurfaceMaterialPath))
					{
						State.BackdropMID = Comp->CreateDynamicMaterialInstance(0, Surface);
					}
					else
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[Portrait] %s did not load; the backdrop keeps whatever material it had."),
							SurfaceMaterialPath);
					}
				}
				if (UMaterialInstanceDynamic* MID = State.BackdropMID.Get())
				{
					// Both names for the same idea, the convention ApplyColorToSkeletalMesh sets:
					// setting a parameter a material does not have is a free no-op, so covering
					// M_TraceSurface's name and the BasicShape name costs nothing and cannot be wrong.
					MID->SetVectorParameterValue(TEXT("BaseColor"), BackdropColor);
					MID->SetVectorParameterValue(TEXT("Color"), BackdropColor);
					MID->SetScalarParameterValue(TEXT("Roughness"), BackdropRoughness);
					MID->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.f);
				}
			}
		}

		return true;
	}

	// =============================================================================================
	// THE FRAMING PROBE — what makes the 38% gate a MEASUREMENT
	// =============================================================================================

	/**
	 * *** ART_BIBLE §7.5'S GATE IS "HEAD = 38% OF FRAME HEIGHT", AND NOBODY CAN READ THAT OFF A
	 *     CAMERA TRANSFORM. ***
	 *
	 * Not because the projection is hard, but because two of its terms are not knowable from this
	 * file: the viewport's real pixel size at capture time (-ResX/-ResY, window chrome, DPI), and
	 * which way the engine resolves a horizontal FOV onto a square frame. W3-CHARWIRE's smoke frames
	 * measure ~11 px/uu where a naive "half-height = dist × tan(fov/2)" predicts ~5.9 — a factor of
	 * the camera aspect ratio that a hand calculation gets wrong in exactly the direction that
	 * silently mis-frames a whole set.
	 *
	 * So the ENGINE is asked instead. Two points on the subject's own vertical axis are projected
	 * through the live view; the pixels-per-uu between them, and the world Z the top and bottom edges
	 * of the frame land on, are printed next to every capture. Scripts/compose_portraits.py reads
	 * that line and reports the head fraction against the per-character head geometry, so the gate is
	 * arithmetic on two measured numbers rather than a judgement about a screenshot's blurry edges.
	 *
	 * The two probe points sit on the line X=Y=0, which is where the head is (the 120° subject yaw
	 * moves the head bone by 0.6 uu — beneath the width of the crest it sits under). The camera is
	 * level whenever LookZ == CamZ, and then Z→screenY is affine and two samples describe it exactly;
	 * a tilted camera makes it projective, and the printed pxPerUu is then only true near the
	 * probe span, which the line says out loud.
	 */
	void LogProjectionProbe(UWorld* World, uint8 CharacterId)
	{
		APlayerController* PC = LocalController(World);
		if (PC == nullptr)
		{
			return;
		}

		constexpr float ProbeLowZ = 100.f;
		constexpr float ProbeHighZ = 200.f;
		FVector2D LowPx = FVector2D::ZeroVector;
		FVector2D HighPx = FVector2D::ZeroVector;
		const bool bProjected =
			PC->ProjectWorldLocationToScreen(RigOrigin + FVector(0.f, 0.f, ProbeLowZ), LowPx, false) &&
			PC->ProjectWorldLocationToScreen(RigOrigin + FVector(0.f, 0.f, ProbeHighZ), HighPx, false);
		if (!bProjected || FMath::IsNearlyEqual(LowPx.Y, HighPx.Y))
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Portrait] proj %s: the view would not project — framing is UNMEASURED for this frame."),
				*FileNameFor(CharacterId));
			return;
		}

		FVector2D Viewport = FVector2D::ZeroVector;
		if (GEngine != nullptr && GEngine->GameViewport != nullptr)
		{
			GEngine->GameViewport->GetViewportSize(Viewport);
		}

		// Screen Y grows downward, so a HIGHER world Z is a SMALLER Y: the subtraction is this way
		// round on purpose and the sign is the whole meaning of the number.
		const float PxPerUu = (LowPx.Y - HighPx.Y) / (ProbeHighZ - ProbeLowZ);
		const float TopEdgeZ = ProbeLowZ + (LowPx.Y - 0.f) / PxPerUu;
		const float BottomEdgeZ = ProbeLowZ + (LowPx.Y - Viewport.Y) / PxPerUu;

		UE_LOG(LogTraceGame, Display,
			TEXT("[Portrait] proj %s: viewport=%.0fx%.0f pxPerUu=%.4f yZ%.0f=%.1f yZ%.0f=%.1f ")
			TEXT("frameZ=[%.2f,%.2f] frameH=%.2fuu"),
			*FileNameFor(CharacterId), Viewport.X, Viewport.Y, PxPerUu,
			ProbeLowZ, LowPx.Y, ProbeHighZ, HighPx.Y,
			BottomEdgeZ, TopEdgeZ, TopEdgeZ - BottomEdgeZ);
	}

	// =============================================================================================
	// THE CAPTURE SEQUENCE
	// =============================================================================================

	void RequestShot(uint8 CharacterId)
	{
		FPortraitRigState& State = Rig();
		const FString Path = RawFileFor(State.OutDir, CharacterId);

		// Measured from the SAME view the screenshot is about to come from, one line before the
		// request, so the framing line and the frame cannot describe two different cameras.
		LogProjectionProbe(State.World.Get(), CharacterId);

		// FFileHelper does not reliably create the tree, and the render thread will not either — the
		// same lesson TraceAutoShot::TakeCapture records.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Path));

		// *** bShowUI IS FALSE HERE AND TRUE IN TraceAutoShot, AND BOTH ARE RIGHT. *** AutoShot
		// photographs the GAME, whose HUD is half Slate and must be in the picture. A portrait is a
		// picture of a body on a plate and any UI in it is contamination. bShowUI=false excludes the
		// whole Slate layer; the Canvas HUD, which draws into the scene and would survive it, is
		// switched off separately for the run — see BeginCaptureAll.
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);

		UE_LOG(LogTraceGame, Display, TEXT("[Portrait] requested %s"), *Path);
	}

	void FinishCaptureAll()
	{
		FPortraitRigState& State = Rig();
		State.bCapturing = false;

		if (AHUD* HUD = State.HiddenHUD.Get())
		{
			HUD->bShowHUD = State.bRestoreShowHUD;
		}
		State.HiddenHUD = nullptr;

		// *** THE DONE LINE COUNTS FILES, NOT REQUESTS. *** A screenshot is fulfilled on the render
		// thread at the end of a later frame, so "we asked for ten" and "ten exist" are different
		// claims — and a wrapper greps this line and believes it. Two waves of this project have been
		// bitten by a harness whose log said more than its disk did.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		int32 OnDisk = 0;
		for (const uint8 Id : State.Requested)
		{
			const FString Path = RawFileFor(State.OutDir, Id);
			if (PlatformFile.FileExists(*Path))
			{
				++OnDisk;
				UE_LOG(LogTraceGame, Display, TEXT("[Portrait] wrote raw_%s.png (%lld bytes): %s"),
					*FileNameFor(Id), PlatformFile.FileSize(*Path), *Path);
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Portrait] MISSING raw_%s.png - nothing at %s"),
					*FileNameFor(Id), *Path);
			}
		}

		UE_LOG(LogTraceGame, Display, TEXT("[Portrait] DONE %d/%d  (dir: %s)"),
			OnDisk, TraceCharacterRoster::Count, *State.OutDir);
	}

	void StepCaptureAll()
	{
		FPortraitRigState& State = Rig();
		UWorld* World = State.World.Get();
		if (World == nullptr || !State.bCapturing)
		{
			return;
		}

		if (State.NextId > TraceCharacterRoster::LastId)
		{
			World->GetTimerManager().ClearTimer(State.StepTimer);

			// *** DRAIN BEFORE COUNTING. *** The last shot was requested 1.2 s into this step and a
			// screenshot is fulfilled on the render thread at the END of a later frame, so finishing
			// here would count the tenth file 0.4 s after asking for it and could report DONE 9/10 on
			// a perfectly good run. Two seconds is the same margin TraceAutoShot's own confirm timer
			// uses, and it is generous even on a cold Metal pipeline.
			constexpr float DrainSeconds = 2.f;
			if (AActor* Anchor = State.Camera.Get())
			{
				World->GetTimerManager().SetTimer(State.ShotTimer,
					FTimerDelegate::CreateWeakLambda(Anchor, []() { FinishCaptureAll(); }),
					DrainSeconds, false);
			}
			else
			{
				FinishCaptureAll();
			}
			return;
		}

		const uint8 Id = State.NextId++;
		if (!TracePortraitRig::Dress(World, Id))
		{
			// SKIPPED, NOT ABORTED. A body that is not on this machine is a stage-3 problem, and the
			// other nine portraits are still worth having — the DONE count is what says the set is
			// incomplete, loudly, in one place.
			return;
		}

		State.Requested.Add(Id);

		// The settle: TSR accumulates over several frames and streaming may still be resolving the
		// mesh's LOD, so the shutter opens 1.2 s after the subject changes rather than on the frame
		// it changes. WeakLambda on the camera actor: if the rig is torn down mid-run the shot is
		// simply never taken instead of firing into a half-destroyed world.
		if (AActor* Anchor = State.Camera.Get())
		{
			World->GetTimerManager().SetTimer(State.ShotTimer,
				FTimerDelegate::CreateWeakLambda(Anchor, [Id]() { RequestShot(Id); }),
				SettleSeconds, false);
		}
	}

	void BeginCaptureAll(UWorld* World, const FString& OutDir)
	{
		FPortraitRigState& State = Rig();
		if (State.bCapturing)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Portrait] a CaptureAll run is already in flight; ignoring this one. Two runs would "
				     "write the same ten filenames."));
			return;
		}

		if (!EnsureRig(World))
		{
			return;
		}

		// EnsureRig may have reset the struct for a new world; State is a reference to the one static,
		// so it is still the right object and simply has fresh contents.
		State.OutDir = ResolveOutDir(OutDir);
		State.NextId = TraceCharacterRoster::FirstId;
		State.Requested.Reset();
		State.bCapturing = true;

		// *** THE CANVAS HUD IS SWITCHED OFF FOR THE RUN, AND bShowUI=false IS NOT ENOUGH ON ITS
		//     OWN. *** bShowUI excludes the SLATE layer; anything AHUD::DrawHUD paints goes into the
		//     scene's back buffer and is captured regardless. The menu map's HUD is exactly that, so
		//     without this the title menu would be printed across every portrait. Put back in
		//     FinishCaptureAll, because a debug command that leaves the game with no HUD is a debug
		//     command that breaks what it was measuring.
		if (APlayerController* PC = LocalController(World))
		{
			if (AHUD* HUD = PC->GetHUD())
			{
				State.HiddenHUD = HUD;
				State.bRestoreShowHUD = HUD->bShowHUD;
				HUD->bShowHUD = false;
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Portrait] CaptureAll: %d character(s), %.1fs each (%.1fs settle), into %s"),
			TraceCharacterRoster::Count, StepSeconds, SettleSeconds, *State.OutDir);

		// One repeating step timer, fired immediately so character 1 does not cost a free 1.6 s.
		World->GetTimerManager().SetTimer(State.StepTimer,
			FTimerDelegate::CreateStatic(&StepCaptureAll), StepSeconds, /*bLoop=*/true, /*FirstDelay=*/0.f);
	}

	// =============================================================================================
	// COMMANDS
	// =============================================================================================

	FAutoConsoleCommandWithWorldAndArgs CmdPortraitRig(
		TEXT("Trace.Portrait.Rig"),
		TEXT("Trace, dev only. Trace.Portrait.Rig <CharacterId 1-10> - build the portrait rig in this "
		     "world and dress it for one character, then look through its camera. For hand-iterating "
		     "framing with Trace.Portrait.CamDist / CamZ / LookZ / Fov."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Portrait] usage: Trace.Portrait.Rig <CharacterId %d-%d>"),
					static_cast<int32>(TraceCharacterRoster::FirstId),
					static_cast<int32>(TraceCharacterRoster::LastId));
				return;
			}
			const int32 Id = FCString::Atoi(*Args[0]);
			if (!TraceCharacterRoster::IsValidId(static_cast<uint8>(Id)))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Portrait] '%s' is not a character id (%d-%d)."),
					*Args[0], static_cast<int32>(TraceCharacterRoster::FirstId),
					static_cast<int32>(TraceCharacterRoster::LastId));
				return;
			}
			TracePortraitRig::Dress(World, static_cast<uint8>(Id));
		}));

	FAutoConsoleCommandWithWorldAndArgs CmdPortraitCaptureAll(
		TEXT("Trace.Portrait.CaptureAll"),
		TEXT("Trace, dev only. Trace.Portrait.CaptureAll [OutDir] - sequence all ten characters "
		     "through the portrait rig, one PNG each into OutDir (default Saved/Portraits). Prints "
		     "'[Portrait] DONE n/10' when every file has been checked on disk."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
		{
			BeginCaptureAll(World, Args.Num() > 0 ? Args[0] : FString());
		}));
}

// =================================================================================================
// Public entry points
// =================================================================================================

bool TracePortraitRig::Dress(UWorld* World, uint8 CharacterId)
{
	using namespace TracePortraitRigLocal;

	if (World == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Portrait] no world."));
		return false;
	}

	const TraceCharacterRoster::FTraceCharacterEntry* Row = TraceCharacterRoster::Find(CharacterId);
	if (Row == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Portrait] no roster row for id %d."), static_cast<int32>(CharacterId));
		return false;
	}

	if (!EnsureRig(World))
	{
		return false;
	}

	FPortraitRigState& State = Rig();
	ASkeletalMeshActor* Subject = State.Subject.Get();
	if (Subject == nullptr)
	{
		return false;
	}

	USkeletalMeshComponent* SubjectMesh = Subject->GetSkeletalMeshComponent();
	if (SubjectMesh == nullptr)
	{
		return false;
	}

	// SOFT-FAIL ON A MISSING BODY, LOUDLY AND BY NAME. A clone that has not run
	// Scripts/import-characters.sh has ten rows pointing at ten assets that are not there, and that
	// is a stage-3 report, not a crash — but the subject is HIDDEN rather than left wearing the
	// previous character, because a portrait of the wrong body under the right name is the one
	// failure this whole stage exists to catch.
	const FString BodyPath = (Row->BodyMeshPath != nullptr) ? Row->BodyMeshPath : TEXT("");
	USkeletalMesh* Body = BodyPath.IsEmpty()
		? nullptr
		: LoadObject<USkeletalMesh>(nullptr, *BodyPath);
	if (Body == nullptr)
	{
		SubjectMesh->SetSkeletalMeshAsset(nullptr);
		Subject->SetActorHiddenInGame(true);
		UE_LOG(LogTraceGame, Warning, TEXT("[Portrait] SKIP %s - no body at '%s'."),
			*TraceCharacterRoster::NameFor(CharacterId), *BodyPath);
		return false;
	}

	SubjectMesh->SetSkeletalMeshAsset(Body);
	Subject->SetActorHiddenInGame(false);

	// THE RIM CARRIES THE ACCENT AND NOTHING ELSE DOES. The body's own team emissives render at
	// their material defaults here — neutral cyan, because nothing in this world calls
	// ApplyTeamColors — which is exactly ART_BIBLE §7.5's "portraits are team-neutral". The one
	// per-character colour in the frame is this light.
	if (ASpotLight* RimLight = State.Rim.Get())
	{
		if (USpotLightComponent* Comp = Cast<USpotLightComponent>(RimLight->GetLightComponent()))
		{
			Comp->SetLightColor(Row->Accent);
		}
	}

	if (APlayerController* PC = LocalController(World))
	{
		if (ACameraActor* Camera = State.Camera.Get())
		{
			if (PC->GetViewTarget() != Camera)
			{
				PC->SetViewTargetWithBlend(Camera, 0.f);
			}
		}
	}
	else
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Portrait] no local player controller, so nothing is looking through the rig camera. "
			     "The actors are built; the frame will be of whatever the game was already showing."));
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[Portrait] dressed %s: body=%s accent=(%.2f, %.2f, %.2f) cam=(-%.0f, 0, %.0f) fov=%.0f"),
		*TraceCharacterRoster::NameFor(CharacterId), *GetNameSafe(Body),
		Row->Accent.R, Row->Accent.G, Row->Accent.B,
		CVarPortraitCamDist.GetValueOnGameThread(), CVarPortraitCamZ.GetValueOnGameThread(),
		CVarPortraitFov.GetValueOnGameThread());

	return true;
}

void TracePortraitRig::CaptureAll(UWorld* World, const FString& OutDir)
{
	TracePortraitRigLocal::BeginCaptureAll(World, OutDir);
}

#endif // !UE_BUILD_SHIPPING
