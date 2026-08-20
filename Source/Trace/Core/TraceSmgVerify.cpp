// =================================================================================================
// Trace — TraceSmgVerify.cpp
//
// The SMG's half of the weapon diagnostics. Trace.Railgun.Probe reports the pistol rig; spec v30 §5
// asks for "an equivalent", and this is it, plus the two commands that make the SMG's own motion and
// its beam photographable at all.
//
//   Trace.Smg.Probe [delay]             what rig is on screen, every part, every material slot, the
//                                       live EmissiveIntensity, the ammo-driven amber, and where the
//                                       tracer would leave the barrel
//   Trace.Smg.Hold <a> [reload] [secs]  pin the fire pose (0 = the shot frame, 1 = rest) and
//                                       optionally the reload pose, so a screenshot can catch them
//   Trace.Smg.Tracer                    equip the SMG, hold the trigger, photograph the beam, and
//                                       end in a PASS/FAIL that the beam left the SMG's aperture
//
// *** WHY A PROBE AND NOT A LOG LINE. *** "The mesh resolved" and "the right thing is on screen" are
// different claims and only one of them is cheap to make. A mesh can resolve and still be scaled
// wrong, hidden behind a hand, wearing the wrong material, or wearing a material whose parameter
// nobody is writing — every one of which logs nothing at all. This prints the numbers a screenshot
// cannot (which asset, which slot, what scale, what the emissive parameter actually reads back as)
// and the screenshot supplies what the numbers cannot (that it is not behind the player's head).
//
// *** THE EXPECTATIONS COME FROM THE SPEC, NOT FROM THE VIEWMODEL. *** Every "want" printed here is
// typed out of spec v30 §4 — 1.8x/4.8x cyan, 1.4x full / 0.35x empty amber, 58.8 cm aperture — and
// none of it is read back out of ATraceCharacter. That is deliberate and it is the only arrangement
// under which this file can fail: a check that derives its expectation from the implementation it is
// checking passes by construction. The cost is that if the spec's numbers ever change, this file has
// to change with them, which is the correct cost.
//
// Dev-only; the whole file compiles out of Shipping.
// =================================================================================================
#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Core/TraceCharacter.h"
#include "Gameplay/TraceMelee.h"               // ETraceEquippedWeapon, RequestEquipIfDifferent
#include "Gameplay/TraceTracer.h"              // FTraceTracerBeamStart — the beam's own resolver
#include "Gameplay/TraceWeaponComponent.h"     // the live clip, which drives the amber
#include "Trace.h"

#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                       // TActorIterator — counting live tracers
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"                      // FScreenshotRequest

// Named after the file, exactly as TraceRailgunVerify's namespace is and for the same reason: an
// anonymous namespace is NOT file-local in UBT's unity/jumbo build, where several .cpp files become
// one translation unit and two same-named statics collide (MSVC C2084). This project has been broken
// that way once already and Scripts/check-jumbo-build-collisions.py now gates on it.
namespace TraceSmgVerifyLocal
{
	// ---------------------------------------------------------------------------------------------
	// WHAT THE SPEC ASKS FOR. Retyped here on purpose; see the file header.
	// ---------------------------------------------------------------------------------------------

	/** §4: circuit_cyan sits at 1.8x at rest and spikes to 4.8x on the shot frame. */
	constexpr float WantCyanRest = 1.8f;
	constexpr float WantCyanPeak = 4.8f;

	/** §4: core_amber is driven by REMAINING AMMO — 1.4x at a full magazine, 0.35x at empty. */
	constexpr float WantAmberFull = 1.4f;
	constexpr float WantAmberEmpty = 0.35f;

	/**
	 * How far a live EmissiveIntensity may sit from the value the spec's own rule computes.
	 *
	 * 0.05 is the same tolerance Trace.Railgun.Verify uses, and it is chosen against the size of the
	 * thing it must be able to see: the amber's full sweep is 1.05 (1.4 down to 0.35), so 0.05 is one
	 * round out of twenty on a 40-round clip. A parameter nobody is writing reads 1.000, which is
	 * 0.40 away from a full magazine's 1.40 and 0.65 away from an empty one's — both far outside.
	 */
	constexpr float EmissiveTolerance = 0.05f;

	/** §4's slot names. The import writes the raw glTF material names undecorated on both weapons. */
	const FName CyanSlot(TEXT("circuit_cyan"));
	const FName AmberSlot(TEXT("core_amber"));

	/** The mesh whose transform carries the SMG's aperture, and the pistol landmark that is NOT it. */
	const FName SmgBodyMeshName(TEXT("SM_RailgunSmg_Body"));
	const FName PistolBodyMeshName(TEXT("SM_Railgun_Body"));

	/**
	 * How close the beam's start must be to the SMG's own aperture to count as leaving the barrel.
	 *
	 * 0.5 uu is half a centimetre on a gun whose muzzle is measured ~17.7 uu ahead of its own body
	 * origin. It is not a fudge factor: when the beam and the aperture are the same point the distance
	 * is float noise — the shipped rig measures 0.0000 uu — and the smallest interesting way to get
	 * this wrong, using the pistol's 107.4 cm landmark on the SMG's body, measures 14.58 uu out on
	 * that same rig. Nothing lands between, which is what the CONTROL line prints so a reader can
	 * check that claim instead of taking it.
	 */
	constexpr double BeamAtMuzzleUU = 0.5;

	/** Below this the "wrong landmark" control cannot tell the two apart, and the run says so. */
	constexpr double DiscriminationUU = 2.0;

	ATraceCharacter* FindLocalCharacter(UWorld* World)
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
				if (ATraceCharacter* Character = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					return Character;
				}
			}
		}
		return nullptr;
	}

	/** One screenshot, at a path that says which run and which process produced it. */
	FString Shot(const TCHAR* Tag)
	{
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"),
			FString::Printf(TEXT("Smg_%s_pid%d.png"), Tag, FPlatformProcess::GetCurrentProcessId()));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Warning, TEXT("[Smg] screenshot -> %s"), *Path);
		return Path;
	}

	/**
	 * The live value of a scalar parameter on whatever material is actually on a slot.
	 *
	 * READ OFF THE COMPONENT, NOT OFF A CACHED MID POINTER, which is the whole point of doing it here
	 * as well as through ATraceCharacter::DebugGetSmgEmissive(): the pawn can hold a perfectly good
	 * MID that it never assigned to a slot, or assigned to the wrong one, and a read through the
	 * pointer would report the animation working while the mesh on screen wore the untouched asset.
	 * This asks the renderer's own question — what material is on slot N, and what does it say.
	 */
	bool ReadSlotScalar(const UStaticMeshComponent* Part, FName SlotName, const TCHAR* Parameter,
		float& OutValue, bool& bOutIsDynamic)
	{
		OutValue = -1.f;
		bOutIsDynamic = false;
		if (Part == nullptr)
		{
			return false;
		}
		const int32 Slot = Part->GetMaterialIndex(SlotName);
		if (Slot == INDEX_NONE)
		{
			return false;
		}
		UMaterialInterface* Material = Part->GetMaterial(Slot);
		if (Material == nullptr)
		{
			return false;
		}
		bOutIsDynamic = (Cast<UMaterialInstanceDynamic>(Material) != nullptr);
		return Material->GetScalarParameterValue(Parameter, OutValue);
	}

	/** Every slot of one part, with the material that is on it and whether it is animatable. */
	void ReportPart(const TCHAR* Label, const UStaticMeshComponent* Part)
	{
		if (Part == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg]   %s: NULL"), Label);
			return;
		}

		const UStaticMesh* Mesh = Part->GetStaticMesh();
		const FVector Loc = Part->GetRelativeLocation();
		const FRotator Rot = Part->GetRelativeRotation();
		const FVector Scale = Part->GetRelativeScale3D();

		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg]   %s: mesh=%s loc=(%.2f, %.2f, %.2f) rot=(P%.2f Y%.2f R%.2f) scale=%.3f visible=%d worldPos=%s"),
			Label,
			Mesh != nullptr ? *Mesh->GetName() : TEXT("NONE"),
			Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Rot.Yaw, Rot.Roll, Scale.X,
			Part->IsVisible() ? 1 : 0,
			*Part->GetComponentLocation().ToCompactString());

		const int32 NumSlots = Part->GetNumMaterials();
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			const UMaterialInterface* Material = Part->GetMaterial(Slot);
			const FName SlotName = (Mesh != nullptr && Mesh->GetStaticMaterials().IsValidIndex(Slot))
				? Mesh->GetStaticMaterials()[Slot].MaterialSlotName : NAME_None;

			float Live = -1.f;
			const bool bDynamic = (Cast<UMaterialInstanceDynamic>(Material) != nullptr);
			const bool bHasParam = (Material != nullptr)
				&& Material->GetScalarParameterValue(TEXT("EmissiveIntensity"), Live);

			UE_LOG(LogTraceGame, Warning, TEXT("[Smg]       slot %d '%s' -> %s%s%s"),
				Slot, *SlotName.ToString(),
				Material != nullptr ? *Material->GetName() : TEXT("NONE"),
				bDynamic ? TEXT("  [MID]") : TEXT(""),
				bHasParam ? *FString::Printf(TEXT("  EmissiveIntensity=%.3f"), Live) : TEXT(""));
		}
	}

	/** What §4's ammo rule says the amber should read at this clip count. */
	float ExpectedAmber(int32 ClipAmmo, int32 ClipSize)
	{
		const float Fraction = (ClipSize > 0)
			? FMath::Clamp(static_cast<float>(ClipAmmo) / static_cast<float>(ClipSize), 0.f, 1.f) : 0.f;
		return FMath::Lerp(WantAmberEmpty, WantAmberFull, Fraction);
	}

	/**
	 * Where a world point is DRAWN relative to the crosshair, in degrees right and down.
	 *
	 * The one form of this number a screenshot can be checked against: "the beam starts 19 degrees
	 * right and 12 degrees below the centre of the frame" is a claim about the picture, whereas a
	 * world coordinate is a claim about a coordinate system nobody can see. Trace.DebugViewProbe
	 * prints the muzzle the same way, deliberately.
	 */
	void ScreenAngles(const ATraceCharacter* Character, const FVector& World, double& OutRight, double& OutDown)
	{
		OutRight = 0.0;
		OutDown = 0.0;
		const UCameraComponent* Cam = (Character != nullptr)
			? Character->FindComponentByClass<UCameraComponent>() : nullptr;
		if (Cam == nullptr)
		{
			return;
		}
		const FVector Local = Cam->GetComponentTransform().InverseTransformPosition(World);
		OutRight = FMath::RadiansToDegrees(FMath::Atan2(Local.Y, FMath::Max(Local.X, 1.0)));
		OutDown = FMath::RadiansToDegrees(FMath::Atan2(-Local.Z, FMath::Max(Local.X, 1.0)));
	}

	/**
	 * THE MUZZLE BLOCK — spec v30 §5, printed rather than believed.
	 *
	 * Four blocks of numbers and no verdict — the caller assembles that, because half of these lines
	 * mean nothing when the SMG is not the gun out and a verdict without that context misleads:
	 *   marker raw / drawn      where ATraceCharacter says its ACTIVE gun's marker is, before and
	 *                           after the first-person re-projection. If those two are ever equal the
	 *                           morph is not being applied and the beam is back beside the barrel.
	 *   gun mesh + landmark     which weapon the tracer found DRAWN, and that mesh's own aperture.
	 *   markerToGun             the distance between the two answers. This is the number that says
	 *                           whether the pawn's marker is on the gun being drawn: ~0 means the
	 *                           character parented it correctly and ATraceTracer changed nothing.
	 *   wrong-landmark control  what the start would be if the PISTOL's 107.4 cm landmark had been
	 *                           re-used on the SMG's body — the realistic way to get this wrong, and
	 *                           the thing the check has to be able to tell apart from the right one.
	 *
	 * @return true when the beam start could be resolved at all.
	 */
	bool ReportMuzzle(ATraceCharacter* Character, UWorld* World, const UStaticMeshComponent* SmgBody,
		FTraceTracerBeamStart& OutInfo)
	{
		if (!ATraceTracer::DescribeLocalBeamStart(World, OutInfo))
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] muzzle: NO BEAM START — no viewmodel is being drawn (third person, dead, or no rig). ")
				TEXT("A tracer fired now starts at the camera-derived shot origin, as it does on every remote machine."));
			return false;
		}

		double StartRight = 0.0, StartDown = 0.0;
		ScreenAngles(Character, OutInfo.Start, StartRight, StartDown);

		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] muzzle: gun=%s landmark=(%.1f, %.1f, %.1f)cm rawWorld=%s"),
			OutInfo.bHasGun ? *OutInfo.GunMesh.ToString() : TEXT("none (fallback rig — marker only)"),
			OutInfo.GunMuzzleLocalCm.X, OutInfo.GunMuzzleLocalCm.Y, OutInfo.GunMuzzleLocalCm.Z,
			*OutInfo.GunMuzzleRaw.ToCompactString());
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] muzzle: markerRaw=%s markerDrawn=%s morphApplied=%d"),
			*OutInfo.MarkerRaw.ToCompactString(), *OutInfo.MarkerDrawn.ToCompactString(),
			(FVector::Dist(OutInfo.MarkerRaw, OutInfo.MarkerDrawn) > 0.01) ? 1 : 0);
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] muzzle: BEAM STARTS AT %s (%.1f deg right, %.1f deg below the crosshair), source=%s, markerToGun=%.4f uu"),
			*OutInfo.Start.ToCompactString(), StartRight, StartDown,
			OutInfo.bFromGunMesh ? TEXT("THE GUN'S OWN MESH (the pawn's marker disagreed and was overridden)")
			                     : TEXT("the pawn's muzzle marker"),
			OutInfo.MarkerToGunUU);

		// The control, and it is only MEANINGFUL when the SMG is the gun the beam was placed on. With
		// the pistol out, "the pistol's landmark on the SMG's body" is a distance between two things
		// neither of which the beam used — a number that looks like evidence and is not. Printing it
		// there would be the exact failure mode the file header warns about, one step removed.
		if (SmgBody != nullptr && OutInfo.bHasGun && OutInfo.GunMesh == SmgBodyMeshName)
		{
			FVector PistolLandmark = FVector::ZeroVector;
			if (ATraceTracer::GetGunMuzzleLandmark(PistolBodyMeshName, PistolLandmark))
			{
				const FVector WrongRaw = SmgBody->GetComponentTransform().TransformPosition(PistolLandmark);
				const double WrongUU = FVector::Dist(WrongRaw, OutInfo.GunMuzzleRaw);
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] muzzle: CONTROL — the pistol's (%.1f, %.1f, %.1f)cm landmark on this same body would ")
					TEXT("sit %.2f uu away%s"),
					PistolLandmark.X, PistolLandmark.Y, PistolLandmark.Z, WrongUU,
					(WrongUU >= DiscriminationUU) ? TEXT(", so this check can tell the two apart.")
					                              : TEXT(" — TOO CLOSE TO DISCRIMINATE, treat the muzzle verdict as unproven."));
			}
		}
		return true;
	}

	// =============================================================================================
	// Trace.Smg.Probe
	//
	// A FUNCTION FIRST AND A COMMAND SECOND, because Trace.Smg.Tracer needs to print exactly this
	// report at the one moment it is worth printing — the SMG equipped, the trigger down, a beam on
	// screen — and an unattended run gets one -TraceExec list with no way to space two commands
	// apart. Two copies of the report would be two things to keep in step; there is one.
	// =============================================================================================
	void RunProbe(UWorld* World)
	{
		ATraceCharacter* Character = FindLocalCharacter(World);
		if (Character == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] No local ATraceCharacter yet — run this once a match has started."));
			return;
		}

		UTraceWeaponComponent* Weapon = Character->FindComponentByClass<UTraceWeaponComponent>();
		const ETraceEquippedWeapon Selected = (Weapon != nullptr)
			? Weapon->GetEquippedWeapon() : ETraceEquippedWeapon::Gun;
		const ATraceCharacter::EShownGun Shown = Character->GetShownGun();
		const TCHAR* ShownName =
			(Shown == ATraceCharacter::EShownGun::Smg) ? TEXT("SMG") :
			(Shown == ATraceCharacter::EShownGun::Pistol) ? TEXT("PISTOL") : TEXT("NONE (stowed)");

		UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ===================================="));
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] selected=%s shown=%s smgArt=%s pistolArt=%s parts=%d vmVisible=%d"),
			LexToString(Selected), ShownName,
			Character->UsesSmgViewModel() ? TEXT("BUILT") : TEXT("NOT BUILT"),
			Character->UsesRailgunViewModel() ? TEXT("BUILT") : TEXT("NOT BUILT"),
			Character->GetViewModelPartCount(),
			Character->IsViewModelVisible() ? 1 : 0);

		// The SELECTOR and the RIG can legitimately disagree: spec v30 §2 requires the `3` slot to
		// fall back to the pistol rig when the SMG art is missing. Saying so here is what keeps a
		// fallback from being mistaken for a broken swap.
		if (Selected == ETraceEquippedWeapon::Smg && Shown != ATraceCharacter::EShownGun::Smg)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] The SMG is SELECTED but is NOT the rig on screen. That is the documented fallback ")
				TEXT("when Content/Trace/Weapons/Meshes/SM_RailgunSmg_* did not resolve — run ")
				TEXT("./Scripts/import-railgun.sh --rig smg, or `git lfs pull` on a fresh clone."));
		}

		UStaticMeshComponent* Body = nullptr;
		UStaticMeshComponent* WallLeft = nullptr;
		UStaticMeshComponent* WallRight = nullptr;
		UStaticMeshComponent* Mag = nullptr;
		Character->DebugGetSmgParts(Body, WallLeft, WallRight, Mag);

		ReportPart(TEXT("body     "), Body);
		ReportPart(TEXT("wallLeft "), WallLeft);
		ReportPart(TEXT("wallRight"), WallRight);
		ReportPart(TEXT("mag      "), Mag);

		// --- the glow, read back off the components ------------------------------------------
		//
		// PER PART, because on this weapon the two glowing materials are on DIFFERENT MESHES:
		// circuit_cyan is on the body and both walls, core_amber exists ONLY on the magazine. A
		// reader that assumed the pistol's "both slots live on the body" arrangement would find
		// no amber slot at all and report a working gun.
		float BodyCyan = -1.f, WallLCyan = -1.f, WallRCyan = -1.f, MagAmber = -1.f;
		bool bBodyDyn = false, bWallLDyn = false, bWallRDyn = false, bMagDyn = false;
		const bool bBodyOk = ReadSlotScalar(Body, CyanSlot, TEXT("EmissiveIntensity"), BodyCyan, bBodyDyn);
		const bool bWallLOk = ReadSlotScalar(WallLeft, CyanSlot, TEXT("EmissiveIntensity"), WallLCyan, bWallLDyn);
		const bool bWallROk = ReadSlotScalar(WallRight, CyanSlot, TEXT("EmissiveIntensity"), WallRCyan, bWallRDyn);
		const bool bMagOk = ReadSlotScalar(Mag, AmberSlot, TEXT("EmissiveIntensity"), MagAmber, bMagDyn);

		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] live cyan  : body=%.3f%s wallL=%.3f%s wallR=%.3f%s   (want %.2f at rest, %.2f on the shot frame)"),
			BodyCyan, bBodyOk ? (bBodyDyn ? TEXT("") : TEXT(" [STATIC]")) : TEXT(" [NO SLOT]"),
			WallLCyan, bWallLOk ? (bWallLDyn ? TEXT("") : TEXT(" [STATIC]")) : TEXT(" [NO SLOT]"),
			WallRCyan, bWallROk ? (bWallRDyn ? TEXT("") : TEXT(" [STATIC]")) : TEXT(" [NO SLOT]"),
			WantCyanRest, WantCyanPeak);

		float PawnCyan = -1.f, PawnAmber = -1.f;
		const bool bPawnRead = Character->DebugGetSmgEmissive(PawnCyan, PawnAmber);
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] pawn MIDs  : cyan=%.3f amber=%.3f (readback %s)"),
			PawnCyan, PawnAmber,
			bPawnRead ? TEXT("OK") : TEXT("FAILED — the pawn holds no MID, or the parameter is not on the material"));

		// --- the ammo-driven amber (§4) ------------------------------------------------------
		const int32 Clip = (Weapon != nullptr) ? Weapon->GetClipAmmo() : -1;
		const int32 ClipSize = TraceAmmo::GetClipSize(ETraceEquippedWeapon::Smg);
		const float Want = ExpectedAmber(Clip, ClipSize);
		const bool bAmberDriven = bMagOk && bMagDyn
			&& FMath::IsNearlyEqual(MagAmber, Want, EmissiveTolerance);

		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] live amber : %.3f%s  ammo=%d/%d  ->  spec §4 wants %.3f (%.2f full .. %.2f empty)  %s"),
			MagAmber, bMagOk ? (bMagDyn ? TEXT("") : TEXT(" [STATIC — nothing is driving it]")) : TEXT(" [NO core_amber SLOT ON THE MAGAZINE]"),
			Clip, ClipSize, Want, WantAmberFull, WantAmberEmpty,
			bAmberDriven ? TEXT("PASS") : TEXT("**FAIL**"));

		if (Selected != ETraceEquippedWeapon::Smg)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] NOTE: the SMG is not the equipped weapon, so the clip above is the pistol's and the ")
				TEXT("amber check is not meaningful — it is reported n/a below rather than failed. Press 3 ")
				TEXT("(or run Trace.Smg.Tracer) and probe again."));
		}

		// --- the beam (§5) -------------------------------------------------------------------
		FTraceTracerBeamStart Beam;
		ReportMuzzle(Character, World, Body, Beam);

		// --- verdict --------------------------------------------------------------------------
		//
		// *** PASS / FAIL / N-A, AND THE THIRD ONE MATTERS AS MUCH AS THE OTHER TWO. *** Every check
		// below is a claim about the SMG, and with the pistol out most of them have no subject: the
		// SMG's magazine is not draining because it is not the gun being fired, and the beam is not on
		// the SMG's barrel because the SMG is not on screen. Printing FAIL there would be a report
		// that cries wolf about somebody else's slice on every pistol frame — and this project's own
		// house rule is that a check which cries wolf gets ignored, at which point it is worth nothing
		// when it finally has something to say.
		//
		// The one case that stays a FAIL is the SMG SELECTED but not SHOWN: that is the missing-art
		// fallback, it is a real finding, and it has its own explanation printed above.
		const bool bSmgSelected = (Selected == ETraceEquippedWeapon::Smg);
		const bool bSmgOnScreen = Character->UsesSmgViewModel() && (Shown == ATraceCharacter::EShownGun::Smg);
		const bool bApplicable = bSmgSelected || bSmgOnScreen;

		const bool bCyanDriven = bBodyOk && bBodyDyn
			&& BodyCyan >= WantCyanRest - EmissiveTolerance
			&& BodyCyan <= WantCyanPeak + EmissiveTolerance;
		const bool bBeamOnGun = Beam.bValid && Beam.bHasGun && (Beam.GunMesh == SmgBodyMeshName);

		const auto Mark = [bApplicable](bool bOk) -> const TCHAR*
		{
			return !bApplicable ? TEXT("n/a ") : (bOk ? TEXT("PASS") : TEXT("FAIL"));
		};

		UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ---- VERDICT ----"));
		if (!bApplicable)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] The SMG is neither selected nor on screen, so every line below is n/a — they are "
				     "claims about a gun that is not out. The rig, its materials and its transforms are still "
				     "printed above, which is the part that is true whether or not it is being held."));
		}
		UE_LOG(LogTraceGame, Warning, TEXT("[Smg] rig   %s: the SMG's own four meshes are the gun on screen"),
			Mark(bSmgOnScreen));
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] cyan  %s: %.3f is inside the authored %.2f..%.2f band (1.000 would mean nobody is writing it)"),
			Mark(bCyanDriven), BodyCyan, WantCyanRest, WantCyanPeak);
		UE_LOG(LogTraceGame, Warning, TEXT("[Smg] amber %s: %.3f against the %.3f this clip calls for"),
			Mark(bAmberDriven), MagAmber, Want);
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] beam  %s: the near end is placed on %s"),
			Mark(bBeamOnGun),
			Beam.bHasGun ? *Beam.GunMesh.ToString() : TEXT("nothing — no gun mesh is drawn"));

		// WITH THE PISTOL OUT THIS IS THE ONE LINE WORTH READING, and it is a real result rather than
		// an n/a: it says the beam's near end landed exactly on the PISTOL's own marker, i.e. that the
		// gun-on-screen resolver added by spec v30 §5 changes nothing at all on the shipped weapon.
		if (!bApplicable && Beam.bValid)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] pistol control: beam on %s, markerToGun=%.4f uu — %s"),
				Beam.bHasGun ? *Beam.GunMesh.ToString() : TEXT("no gun mesh"), Beam.MarkerToGunUU,
				(Beam.bHasGun && !Beam.bFromGunMesh && Beam.MarkerToGunUU >= 0.0
					&& Beam.MarkerToGunUU <= 0.01)
					? TEXT("PASS, the v30 resolver is a no-op on the pistol")
					: TEXT("**LOOK AT THIS**, the resolver moved the pistol's beam"));
		}
		UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ===================================="));
	}

	/** Its own handle, so a delayed probe and a running Trace.Smg.Tracer cannot cancel each other. */
	FTimerHandle GProbeHandle;

	FAutoConsoleCommandWithWorldAndArgs CmdProbe(
		TEXT("Trace.Smg.Probe"),
		TEXT("Trace.Smg.Probe [delaySeconds] — spec v30 §2/§4/§5. Reports the SMG rig actually on screen: "
		     "which gun is drawn, every part's mesh, transform and material slots, the LIVE "
		     "EmissiveIntensity on each glowing slot, the ammo-driven amber against the value the spec's own "
		     "rule computes for the current clip, and where a tracer fired now would leave the barrel. The "
		     "optional delay is for unattended runs: -TraceExec fires its whole list at one instant, and at "
		     "that instant the select screen is usually still up and there is no pawn to report on."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
		{
			const float Delay = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 0.f;
			if (Delay <= 0.f || World == nullptr)
			{
				RunProbe(World);
				return;
			}
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] probing in %.1fs."), Delay);
			World->GetTimerManager().SetTimer(GProbeHandle,
				FTimerDelegate::CreateStatic(&RunProbe, World), Delay, /*bLoop=*/false);
		}));

	// =============================================================================================
	// Trace.Smg.Hold — the fire cycle is 0.100 s, so no unattended screenshot can catch it otherwise
	// =============================================================================================
	FAutoConsoleCommandWithWorldAndArgs CmdHold(
		TEXT("Trace.Smg.Hold"),
		TEXT("Trace.Smg.Hold <alpha> [reloadAlpha] [seconds] — pins the SMG's pose so a screenshot can catch "
		     "it. alpha 0 = the shot frame (walls thrown apart, cyan at its peak), 1 = rest, -1 releases. "
		     "reloadAlpha 0..1 pins the reload instead (0.33 is the cell fully dropped); negative leaves the "
		     "magazine to the real weapon."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
		{
			ATraceCharacter* Character = FindLocalCharacter(World);
			if (Character == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Smg] No local ATraceCharacter yet."));
				return;
			}

			const float Alpha = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 0.f;
			const float ReloadAlpha = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : -1.f;
			const float Seconds = (Args.Num() > 2) ? FCString::Atof(*Args[2]) : 5.f;
			Character->DebugHoldSmgPhase(Alpha, ReloadAlpha, Seconds);

			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] holding fire alpha=%.2f reload alpha=%.2f for %.1fs. Probe on the NEXT frame, not this "
				     "one: the pose is applied in Tick, so a read taken now reports the value from before the hold."),
				Alpha, ReloadAlpha, Seconds);
		}));

	// =============================================================================================
	// Trace.Smg.Tracer — spec v30 §5's photograph, and the measurement behind it
	//
	// WHAT IT MEASURES, AND WHY IT IS NOT THE SAME AS WHAT Trace.Smg.Probe PRINTS. The probe asks the
	// resolver where a beam WOULD start. This fires real rounds through the shipped trigger and then
	// reads the location off the ATraceTracer actors that actually spawned — the thing on screen. The
	// two can disagree (a caller that ignores the resolver, a proximity gate that rejects the shot),
	// and the disagreement is exactly the bug a probe alone cannot see.
	//
	// The comparison is made IN THE SAME FRAME AS THE SHOT. UTraceWeaponComponent::StartFire() calls
	// FireOnce() synchronously, so the tracer exists by the time DoFirePressed() returns, and the rig
	// has not moved between the sample and the shot. A frame later the recoil kick has moved the
	// muzzle several uu and any tolerance tight enough to be worth having would fail.
	// =============================================================================================
	/** At 0.35 s a step, ten waits is 3.5 s — five times the pullout, and still bounded. */
	constexpr int32 MaxDeployWaits = 10;

	/**
	 * How many steps to wait for a pawn before giving up. 40 x 0.35 s = 14 s.
	 *
	 * NEEDED BECAUSE OF WHERE AN UNATTENDED RUN STARTS. -TraceExec fires its whole list at one
	 * instant, and at that instant the select screen is usually still up — no pawn has been
	 * possessed, so a run that demanded one immediately would report "no local pawn" and stop, which
	 * looks exactly like a broken build. Waiting lets the list be written as
	 * "Trace.Characters.Select 1|Trace.Smg.Tracer" and still work.
	 */
	constexpr int32 MaxPawnWaits = 40;

	struct FTracerRun
	{
		int32 Step = 0;
		int32 DeployWaits = 0;
		int32 PawnWaits = 0;
		bool bFired = false;
		bool bBeamAtMuzzle = false;
		bool bDiscriminated = false;
		double BeamToMuzzleUU = -1.0;
		double WrongLandmarkUU = -1.0;
		FVector BeamStart = FVector::ZeroVector;
		FVector ActualStart = FVector::ZeroVector;
		FName Gun = NAME_None;
		FTimerHandle Handle;
	};

	FTracerRun GRun;

	/** The most recently spawned tracer in the world, or null. */
	ATraceTracer* NewestTracer(UWorld* World)
	{
		ATraceTracer* Newest = nullptr;
		float Youngest = TNumericLimits<float>::Max();
		for (TActorIterator<ATraceTracer> It(World); It; ++It)
		{
			ATraceTracer* Candidate = *It;
			if (Candidate == nullptr || !IsValid(Candidate))
			{
				continue;
			}
			const float Age = Candidate->GetGameTimeSinceCreation();
			if (Age < Youngest)
			{
				Youngest = Age;
				Newest = Candidate;
			}
		}
		return Newest;
	}

	void TracerStep(UWorld* World)
	{
		ATraceCharacter* Character = FindLocalCharacter(World);
		if (Character == nullptr)
		{
			if (GRun.PawnWaits >= MaxPawnWaits)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[Smg] VERDICT: FAIL — no local pawn to fire from after %.1fs. If the select screen is ")
					TEXT("still up, run Trace.Characters.Select 1 first."),
					MaxPawnWaits * 0.35f);
				return;
			}
			if (GRun.PawnWaits == 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] no local pawn yet (select screen, or travel in progress) — waiting."));
			}
			++GRun.PawnWaits;
			World->GetTimerManager().SetTimer(GRun.Handle,
				FTimerDelegate::CreateStatic(&TracerStep, World), 0.35f, /*bLoop=*/false);
			return;
		}
		UTraceWeaponComponent* Weapon = Character->FindComponentByClass<UTraceWeaponComponent>();

		switch (GRun.Step)
		{
		case 0:
		{
			// The `3` key's own verb, not a private setter: if the swap is refused for a reason the
			// player would also hit (carrying the Core, dead), this run must be refused too.
			ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
			const bool bAsked = TraceMelee::RequestEquipIfDifferent(Character, ETraceEquippedWeapon::Smg, &Refusal);
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] equipping the SMG: %s (%s)"),
				bAsked ? TEXT("asked") : TEXT("no change / REFUSED"), LexToString(Refusal));
			break;
		}

		case 1:
			// Deploy is a replicated deadline; nothing may fire until it has passed. BOUNDED, because
			// a wait with no ceiling turns a refused swap into a run that never reports anything —
			// and silence is the one outcome a verification command must never produce.
			if (Weapon != nullptr && Weapon->IsDeploying() && GRun.DeployWaits < MaxDeployWaits)
			{
				++GRun.DeployWaits;
				UE_LOG(LogTraceGame, Warning, TEXT("[Smg] still deploying (%.2fs left) — waiting (%d/%d)."),
					Weapon->GetDeployRemaining(), GRun.DeployWaits, MaxDeployWaits);
				--GRun.Step;   // repeat this step
			}
			break;

		case 2:
		{
			// THE MEASUREMENT. Sample, fire, read — no frame boundary anywhere in between.
			FTraceTracerBeamStart Before;
			const bool bResolved = ATraceTracer::DescribeLocalBeamStart(World, Before);

			UStaticMeshComponent* Body = nullptr;
			UStaticMeshComponent* WallLeft = nullptr;
			UStaticMeshComponent* WallRight = nullptr;
			UStaticMeshComponent* Mag = nullptr;
			Character->DebugGetSmgParts(Body, WallLeft, WallRight, Mag);

			int32 TracersBefore = 0;
			for (TActorIterator<ATraceTracer> It(World); It; ++It) { ++TracersBefore; }

			Character->DoFirePressed();       // full auto: the trigger stays down until the last step

			int32 TracersAfter = 0;
			for (TActorIterator<ATraceTracer> It(World); It; ++It) { ++TracersAfter; }

			GRun.bFired = (TracersAfter > TracersBefore);
			GRun.BeamStart = Before.Start;
			GRun.Gun = Before.GunMesh;

			if (GRun.bFired && bResolved)
			{
				if (const ATraceTracer* Tracer = NewestTracer(World))
				{
					GRun.ActualStart = Tracer->GetActorLocation();
					GRun.BeamToMuzzleUU = FVector::Dist(GRun.ActualStart, Before.Start);
					GRun.bBeamAtMuzzle = (GRun.BeamToMuzzleUU <= BeamAtMuzzleUU);
				}

				// The control: the same rig, the pistol's landmark. If this is not far away, the
				// measurement above cannot tell a right answer from the obvious wrong one.
				FVector PistolLandmark = FVector::ZeroVector;
				if (Body != nullptr && ATraceTracer::GetGunMuzzleLandmark(PistolBodyMeshName, PistolLandmark))
				{
					const FVector WrongRaw = Body->GetComponentTransform().TransformPosition(PistolLandmark);
					GRun.WrongLandmarkUU = FVector::Dist(WrongRaw, Before.GunMuzzleRaw);
					GRun.bDiscriminated = (GRun.WrongLandmarkUU >= DiscriminationUU);
				}
			}

			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] fired: tracers %d -> %d, resolver said %s, the tracer actually started at %s"),
				TracersBefore, TracersAfter, *Before.Start.ToCompactString(), *GRun.ActualStart.ToCompactString());
			break;
		}

		case 3:
			// Mid-burst, with a live beam on screen and the walls thrown apart. The full report is
			// printed HERE rather than before the run: this is the only moment at which the SMG is
			// equipped, the clip is draining and a beam exists, so it is the only moment at which
			// every number in it means something.
			RunProbe(World);
			Shot(TEXT("tracer"));
			break;

		case 4:
			Shot(TEXT("tracer2"));
			break;

		default:
		{
			Character->DoFireReleased();

			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ---- TRACER VERDICT ----"));
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] fired %s: a round left the gun and a tracer spawned"),
				GRun.bFired ? TEXT("PASS") : TEXT("FAIL"));
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] start %s: the beam began %.4f uu from %s's own aperture (limit %.2f uu)"),
				GRun.bBeamAtMuzzle ? TEXT("PASS") : TEXT("FAIL"), GRun.BeamToMuzzleUU,
				GRun.Gun.IsNone() ? TEXT("no gun") : *GRun.Gun.ToString(), BeamAtMuzzleUU);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] control %s: the pistol's landmark on this rig would have been %.2f uu away, so the ")
				TEXT("check above can tell the right barrel from the wrong one"),
				GRun.bDiscriminated ? TEXT("PASS") : TEXT("WEAK"), GRun.WrongLandmarkUU);
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] VERDICT: %s"),
				(GRun.bFired && GRun.bBeamAtMuzzle && GRun.bDiscriminated && GRun.Gun == SmgBodyMeshName)
					? TEXT("PASS") : TEXT("FAIL"));
			return;
		}
		}

		++GRun.Step;
		World->GetTimerManager().SetTimer(GRun.Handle,
			FTimerDelegate::CreateStatic(&TracerStep, World), 0.35f, /*bLoop=*/false);
	}

	FAutoConsoleCommandWithWorld CmdTracer(
		TEXT("Trace.Smg.Tracer"),
		TEXT("Spec v30 §5. Equips the SMG through the shipped `3` verb, holds the trigger, photographs the "
		     "beam mid-burst, and ends in a PASS/FAIL on whether the tracer actually started at the SMG's own "
		     "aperture — with the pistol's landmark on the same rig as the control it has to beat."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World)
		{
			if (World == nullptr)
			{
				return;
			}
			GRun = FTracerRun();
			TracerStep(World);
		}));
}

#endif // !UE_BUILD_SHIPPING
