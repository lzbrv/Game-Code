// Trace — the player pawn. See TraceCharacter.h for the shape of the thing and why.

#include "Core/TraceCharacter.h"

#include "Net/UnrealNetwork.h"

#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"                 // FTSTicker (debug console command below)
#include "Engine/Engine.h"                     // GEngine (debug console command below)
#include "EngineUtils.h"                       // TActorIterator (Trace.DebugAnimProbe)
#include "HAL/IConsoleManager.h"               // FAutoConsoleCommand (debug console command below)
#include "Components/PrimitiveComponent.h"      // EFirstPersonPrimitiveType (the viewmodel)
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"    // ApplyRotationMode: human vs bot
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"                  // DoesPackageExist (the character-art first-run check)
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"


#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceParry.h"                // the parry entry point and its queries (spec §3)
#include "Gameplay/TraceTrailComponent.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Net/TraceLagCompensationComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceCharacterLayout
{
	// The capsule is the collider and therefore the single source of truth for the character's
	// size — hitscan, the trail trip test and lag compensation all reason about it. Every visual
	// dimension below is derived from these two numbers so the meshes can never drift away from
	// what the game actually tests against.
	constexpr float CapsuleRadius = 34.f;
	constexpr float CapsuleHalfHeight = 88.f;

	/** Engine basic shapes are 100 uu cubes/cylinders/spheres, so scale = desired size / 100. */
	constexpr float BasicShapeSize = 100.f;

	constexpr float BodyHeight = 136.f;
	constexpr float HeadDiameter = 62.f;

	/** Head centre, chosen so the top of the sphere lands level with the top of the capsule. */
	constexpr float HeadCentreZ = CapsuleHalfHeight - (HeadDiameter * 0.5f);

	/**
	 * Skeletal mesh placement on the capsule. SKM_Manny_Simple is authored feet-at-origin, facing
	 * +Y: its bounds run Z 0..180 about an origin at Z 90 (measured, not assumed — see
	 * Scripts/import-mannequin.sh). So dropping it by the capsule half-height stands it exactly on
	 * the bottom of the capsule, and yaw -90 turns its +Y forward onto the actor's +X, which is the
	 * direction the movement component orients to and the direction GetAimDirection() shoots along.
	 * Get either of these wrong and the character floats, sinks, or strafes everywhere.
	 */
	constexpr float MeshOffsetZ = -CapsuleHalfHeight;
	constexpr float MeshYaw = -90.f;

	/**
	 * Pushed into M_Mannequin's "EmissivePower" scalar.
	 *
	 * MEASURED, NOT ASSUMED, AND THE ANSWER IS DISAPPOINTING: rendering the same frame with this at
	 * 0 and at 8 produces a visually identical character. M_Mannequin does expose the parameter, but
	 * MI_Manny_01_New / MI_Manny_02_New — the two instances SKM_Manny_Simple actually ships with —
	 * mask the emissive contribution to nothing, so on the stock materials this scalar is a no-op.
	 *
	 * It is still set, for two reasons: setting a parameter is free, and it means the moment anyone
	 * swaps in a material whose emissive is live (an authored Tron suit, say) the glow arrives with
	 * no code change. What ACTUALLY carries team identity today is the "Paint Tint" vector below,
	 * which is verified to recolour the whole suit — cyan and orange players are unmistakable at
	 * across-the-arena distance in a captured frame. Do not "rely" on these numbers for readability.
	 */
	constexpr float EmissiveNormal = 8.f;
	constexpr float EmissiveCarrier = 30.f;
	constexpr float EmissiveDead = 0.f;

	// --- View rig (see the file header) ---------------------------------------------------------

	/**
	 * Eye height above the capsule centre, and the single number the whole first-person view is
	 * pinned to. It is pushed into APawn::BaseEyeHeight in the constructor, so GetPawnViewLocation()
	 * — which the aim ray, the bots and the harness all use — and the first-person camera are the
	 * same point BY CONSTRUCTION rather than by two constants that agree today.
	 *
	 * 64 above centre is 152 above the feet on an 88-half-height capsule, which lands in Manny's
	 * head (the mesh is 180 tall). It is also APawn's own default, so nothing is being fought.
	 */
	constexpr float EyeHeight = 64.f;

	/** Collapsed onto the eye. Not "small": exactly 0, or the camera stops being the gun. */
	constexpr float FirstPersonArmLength = 0.f;

	constexpr float ThirdPersonArmLength = 450.f;

	/**
	 * Where the third-person arm pivots, as a WORLD-space offset from the capsule centre
	 * (USpringArmComponent::TargetOffset, not SocketOffset — SocketOffset is rotated by the arm, so
	 * using it would swing the camera height around as the player pitches, and in first person that
	 * would drag the eye off GetPawnViewLocation() and break the aim guarantee above).
	 *
	 * Centred, not over-the-shoulder: a lateral offset makes the muzzle ray and the crosshair
	 * disagree at close range, and this prototype has one crosshair dead centre.
	 */
	constexpr float ThirdPersonPivotZ = 60.f;

	/**
	 * How far the third-person camera must clear the TOP of the carrier's own light trail.
	 *
	 * This is not a taste number, it is a correctness one. Third person happens EXACTLY when you are
	 * carrying the Core, and carrying the Core is exactly when you are laying a trail — a 190 uu tall
	 * UNLIT EMISSIVE wall, with no collision, extruded along the precise path you just walked. The
	 * camera hangs ThirdPersonArmLength straight back down that same path. So at the old pivot of 60
	 * (camera 150 above the feet, trail top 196) the camera spent every moving carry INSIDE its own
	 * trail, and because the trail material has no lighting term the result was a flat pale slab
	 * filling the whole frame. Measured over one match: every third-person frame taken while the
	 * carrier was moving was >50% blown out (one was 96%), while the third-person frames taken while
	 * the carrier stood still — no trail being laid — were correctly dark.
	 *
	 * Raising the pivot fixes it structurally rather than by tuning a glow value down: the camera
	 * flies above the wall and looks along it, which is the light-cycle shot this game wants anyway.
	 * The clearance is measured from the top of the trail's cap strip, not from the trail centre.
	 */
	constexpr float TrailCameraClearance = 66.f;

	/** Long enough to read as a deliberate pull-back, short enough not to fight for control. */
	constexpr float ViewBlendSeconds = 0.35f;

	/**
	 * Below this blend alpha the local player's own body is hidden from their own camera. Slightly
	 * above 0 on purpose: the last few centimetres of the pull-in put the camera inside the head,
	 * and a frame of the inside of your own skull is the one artefact this transition can produce.
	 */
	constexpr float OwnBodyHideAlpha = 0.2f;

	constexpr float CameraFOV = 95.f;

	/**
	 * Muzzle: on the aim ray, one short step in front of the eye.
	 *
	 * It used to be chest height and 45 uu forward, which put the shot origin 24 uu below and
	 * outside the crosshair ray and left AimConvergenceDistance below to paper over the parallax.
	 * In first person that is not acceptable — the camera IS the gun — so the muzzle now sits on the
	 * ray itself and the convergence correction resolves to the view direction exactly.
	 *
	 * 22 uu is deliberately INSIDE the 34 uu capsule radius: the origin can never end up on the far
	 * side of a wall the player is pressed against, which the old 45 could.
	 */
	constexpr float MuzzleForward = 22.f;

	/**
	 * Distance at which the muzzle ray is made to meet the camera ray. With the muzzle on the ray
	 * this correction is a no-op (it returns the view direction to float precision); it is kept
	 * because it is what makes the muzzle offset above a free parameter rather than a load-bearing
	 * one — move the muzzle off-axis and shots still converge on the crosshair.
	 */
	constexpr float AimConvergenceDistance = 6000.f;

	/** Team-colour polling: PlayerState and Team arrive in an order nothing can rely on. */
	constexpr float TeamColorPollInterval = 0.25f;
	constexpr int32 MaxTeamColorAttempts = 24;   // ~6 s, then give up and stay grey

	// --- Crouch / slide ---------------------------------------------------------------------------

	/**
	 * Where the eye sits above the FEET while crouched, in uu.
	 *
	 * Expressed against the feet rather than as a BaseEyeHeight because BaseEyeHeight is measured
	 * from the capsule CENTRE, and the crouched capsule height belongs to
	 * UTraceCharacterMovementComponent, not to this class. OnStartCrouch() reads the resized capsule
	 * and subtracts, so this number stays correct whatever the movement component decides a crouch
	 * is. 92 against a standing 152 is a 60 uu drop: unmistakable in first person, and low enough to
	 * genuinely change what a slide can see over.
	 */
	constexpr float CrouchedEyeAboveFeet = 92.f;

	/**
	 * BaseEyeHeight while SLIDING, i.e. 30 above the capsule centre and 118 above the feet, against
	 * 64 / 152 standing.
	 *
	 * Not derived from the capsule, because in this game a slide does not shrink the capsule (see
	 * UpdateCrouchPresentation): the movement component keeps it fixed so hitscan, the rewind
	 * history and the trail trip test cannot disagree with it. Only the view drops. It stays well
	 * inside the 88 uu half height so the eye can never leave the collider, and a 34 uu dip is
	 * plainly readable at a glance without putting the camera in the floor.
	 */
	constexpr float SlideEyeHeight = 30.f;

	/** Fast enough that the dip lands with the slide, slow enough that it is a move, not a cut. */
	constexpr float SlideEyeInterpSpeed = 11.f;

	/** Keeps the crouched eye inside the capsule, so a slide can never poke the camera up through it. */
	constexpr float CrouchedEyeCapsuleMargin = 6.f;

	// CrouchLeanPitch (20 degrees) WAS HERE. It is now UTraceSettings::SlidePoseLeanDegrees, promoted
	// along with the rest of the slide pose (drop, roll, anim rate, blend speed) because spec v4 §1
	// asks for a slide pose the user can actually look at and retune, and a pose whose only dial is a
	// recompile is not one. See UpdateCrouchPresentation().

	/**
	 * How fast the first-person viewmodel's own crouch dip follows the slide.
	 *
	 * Deliberately still a constant, and deliberately NOT UTraceSettings::SlidePoseBlendSpeed: this is
	 * the gun in front of the camera, not the third-person body, and they are tuned by different
	 * people looking at different things. Fast enough not to feel laggy.
	 */
	constexpr float CrouchLeanInterpSpeed = 9.f;

	/** Ground speed at which the skid streak reaches full length. */
	constexpr float SkidFullSpeed = 900.f;

	/** Streak size at full strength: long, narrow, and hugging the deck. */
	constexpr float SkidLength = 260.f;
	constexpr float SkidWidth = 46.f;
	constexpr float SkidThickness = 5.f;
	constexpr float SkidGlow = 3.4f;

	// --- First-person viewmodel (see the file header) ---------------------------------------------
	//
	// EVERYTHING BELOW IS ARITHMETIC, NOT TASTE, and the arithmetic is written down because it is
	// what let this land without a launch-look-tweak loop.
	//
	// The rig is authored in CAMERA SPACE: +X out through the lens, +Y right, +Z up, origin at the
	// eye. The renderer then draws it through FirstPersonFieldOfView and squashes its depth by
	// FirstPersonScale, so two independent checks have to pass:
	//
	//   FRAMING. Half-width of the frame at distance d is d * tan(FirstPersonFieldOfView / 2), and
	//   half-height is that times 9/16. The muzzle sits at (75.5, 12.9, -12.3), i.e. 20% right of
	//   centre and 35% below it; the highest point of the gun - the top of the slide's light channel
	//   - is 27% below centre. THE CROSSHAIR IS AT 0,0, so the gun clears it by more than a quarter
	//   of the frame at every point along its length. That was the requirement.
	//
	//   DEPTH. The deepest part of the rig is the muzzle at 76 uu. At FirstPersonScale 0.40 the
	//   renderer draws it at 30 uu, and the pawn capsule radius is 34, so nothing in the viewmodel
	//   can reach a surface the body cannot already stand against - it can never intersect the
	//   world. The shallowest part is the far end of a forearm at ~43 uu, drawn at ~17 uu, which
	//   clears the 10 uu near plane. Those two bounds are why the forearms are 16-17 uu long rather
	//   than anatomically correct: they run off the bottom of the frame, which is exactly where a
	//   real viewmodel's arms go.
	//
	// MEASURED AND RETUNED. The first pass put the rig at 40 uu through a 70 degree first-person
	// lens, and a captured frame showed the reason to write the arithmetic down and then go and look
	// anyway: the gun and hands filled 27% of the frame's width and 43% of its height, which reads
	// as a prop shoved against the lens rather than as a weapon being carried. Apparent size is
	// (size / depth) * FOVCorrection, and FOVCorrection is tan(SceneFOV/2) / tan(FirstPersonFOV/2) -
	// so BOTH dials shrink it. Moving out 40 -> 56 and opening the first-person lens 70 -> 80 (a
	// correction of 1.30 rather than 1.56) multiplies the on-screen size by 0.60 between them, and
	// the scale came down 0.5 -> 0.40 to keep the now-more-distant muzzle inside the capsule radius.
	// Same gun, same aim, three fifths the frame.
	constexpr float FirstPersonViewModelFOV = 80.f;
	constexpr float FirstPersonViewModelScale = 0.40f;

	/** Rest pose of the rig in camera space. */
	const FVector ViewModelRestLocation(56.f, 14.5f, -14.f);

	/** Slight muzzle-up, slight inward yaw, slight cant. A gun held dead square looks like a prop. */
	const FRotator ViewModelRestRotation(2.f, -4.f, 5.f);

	/** Engine basic shapes are 100 uu; every Size below is a world size and divides by this. */
	constexpr float ViewModelShapeUnit = 100.f;

	// Sway / bob / recoil. All small: this is texture, not animation, and anything big enough to
	// notice as movement is big enough to read as the crosshair drifting.
	constexpr float SwayPerDegree = 0.55f;      // rig degrees per degree of view change this frame
	constexpr float SwayMaxDegrees = 4.f;
	constexpr float SwayRecoverSpeed = 9.f;
	constexpr float BobInterpSpeed = 6.f;
	constexpr float BobBaseRate = 7.2f;         // radians/s at a standstill-to-walk blend
	constexpr float BobVerticalUU = 0.6f;
	constexpr float BobLateralUU = 0.8f;
	constexpr float KickRecoverSpeed = 11.f;
	constexpr float KickBackUU = 2.4f;
	constexpr float KickPitchDegrees = 5.f;
	constexpr float CrouchDipUU = 2.2f;

	/** One kick per shot however many code paths report the same shot. */
	constexpr double FireKickRefractorySeconds = 0.02;
}

namespace TraceCharacterAssets
{
	/**
	 * Imported by Scripts/import-mannequin.sh out of the developer's own engine install; NOT in the
	 * repository. These paths are not a preference — the .uasset files carry their own absolute
	 * /Game references internally (SKM_Manny_Simple names /Game/Characters/Mannequins/Meshes/
	 * SK_Mannequin as its skeleton), so the import script reads the expected root back out of the
	 * mesh and refuses to copy anywhere else. If you move these, move them in both places.
	 */
	const TCHAR* const MannequinMesh = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");

	/**
	 * ABP_Unarmed is the ONLY anim blueprint Epic ships in that folder. The Rifle set
	 * (MM_Rifle_Fire, MF_Rifle_Idle_ADS, the Jog/Walk directional sets) is raw AnimSequences with no
	 * blueprint and no weapon mesh to hold, so choosing it would mean hand-authoring a state machine
	 * in C++ and would still leave every character miming a rifle they visibly are not holding.
	 * ABP_Unarmed reads its own Speed/Direction/IsFalling from the pawn's velocity and drives
	 * BS_Idle_Walk_Run, so it animates correctly for a C++ ACharacter with no glue at all.
	 */
	const TCHAR* const UnarmedAnimClass = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed.ABP_Unarmed_C");

	/** M_Mannequin's parameters, measured from the asset rather than guessed. */
	const FName PaintTintParam(TEXT("Paint Tint"));
	const FName EmissivePowerParam(TEXT("EmissivePower"));

	/**
	 * The two generated Tron materials, shared with ATraceArenaBuilder. NOT in the repository:
	 * Scripts/generate_content.py writes them into the gitignored Content/Generated, exactly like
	 * the arena's copies, and exactly like the arena this degrades to BasicShapeMaterial rather than
	 * failing - a flat-shaded gun beats no gun.
	 */
	const TCHAR* const SurfaceMaterialPath = TEXT("/Game/Generated/Materials/M_TraceSurface.M_TraceSurface");
	const TCHAR* const NeonMaterialPath = TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon");

	/** Printed at most once per process, so a missing import is loud but not spam. */
	const TCHAR* const MissingImportHint =
		TEXT("Character art is not imported. Run Scripts/import-mannequin.sh to copy Epic's Mannequin ")
		TEXT("out of your own UE 5.8 install into Content/Characters/Mannequins. ")
		TEXT("Falling back to plain team-coloured shapes.");

	/** The one command that fixes it. Quoted verbatim on screen and in the log, so nobody has to guess. */
	const TCHAR* const ImportCommand = TEXT("./Scripts/import-mannequin.sh");
}

// =================================================================================================
// Character art availability — the "why am I looking at capsules" answer
// =================================================================================================
//
// A bug report arrived asking for the human character models that were already implemented. The
// models were fine; the REPORTER'S MACHINE had never run Scripts/import-mannequin.sh, and the only
// evidence of that was a single Warning line, emitted after the pawn had already spawned, in a log
// that nobody reads while playtesting. The models are not the defect. The SILENT DEGRADE is.
//
// Three things therefore happen now, and all three are needed:
//   1. the check runs BEFORE any pawn exists (VerifyCharacterArtInstalled, from ATraceHUD::BeginPlay),
//      by asking the package store rather than by loading anything;
//   2. it logs a boxed banner at Error verbosity — Warning has been suppressed in this project's
//      logs before, and this is the one message that must survive that;
//   3. the status is queryable, so ATraceHUD can keep a warning on screen for the whole session.
namespace
{
	/** Process-wide, because the answer is a property of the INSTALL, not of any one pawn. */
	ETraceCharacterArtStatus GCharacterArtStatus = ETraceCharacterArtStatus::Unknown;

	/** So the banner is printed once per distinct status rather than once per pawn (ten per match). */
	ETraceCharacterArtStatus GLoggedCharacterArtStatus = ETraceCharacterArtStatus::Unknown;

	/** "/Game/.../SKM_Manny_Simple.SKM_Manny_Simple" -> "/Game/.../SKM_Manny_Simple". */
	FString PackageNameOf(const TCHAR* ObjectPath)
	{
		return FPackageName::ObjectPathToPackageName(FString(ObjectPath));
	}

	/**
	 * Records the status and, on a CHANGE, says so at a verbosity that cannot be missed.
	 *
	 * Error rather than Warning for the two broken states. That is not shouting for its own sake:
	 * this project has twice concluded a working mechanic was dead because its log line was filtered,
	 * and the whole purpose of this message is to be the thing that is still visible when everything
	 * else has been turned down.
	 */
	void ReportCharacterArtStatus(ETraceCharacterArtStatus NewStatus)
	{
		GCharacterArtStatus = NewStatus;
		if (GLoggedCharacterArtStatus == NewStatus)
		{
			return;
		}
		GLoggedCharacterArtStatus = NewStatus;

		switch (NewStatus)
		{
		case ETraceCharacterArtStatus::Ok:
			UE_LOG(LogTraceGame, Display,
				TEXT("[CharacterArt] OK — %s and %s both resolved. Characters are Epic's Mannequin with ")
				TEXT("head, limbs and the ABP_Unarmed run cycle."),
				TraceCharacterAssets::MannequinMesh, TraceCharacterAssets::UnarmedAnimClass);
			break;

		case ETraceCharacterArtStatus::MeshMissing:
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			UE_LOG(LogTraceGame, Error, TEXT("  CHARACTER ART IS NOT INSTALLED ON THIS MACHINE."));
			UE_LOG(LogTraceGame, Error, TEXT("  Every player will be drawn as a coloured capsule with a ball"));
			UE_LOG(LogTraceGame, Error, TEXT("  for a head, with NO running animation. This is a missing"));
			UE_LOG(LogTraceGame, Error, TEXT("  import, NOT the intended look of the game."));
			UE_LOG(LogTraceGame, Error, TEXT(""));
			UE_LOG(LogTraceGame, Error, TEXT("  FIX IT WITH ONE COMMAND, from the project root:"));
			UE_LOG(LogTraceGame, Error, TEXT("      %s"), TraceCharacterAssets::ImportCommand);
			UE_LOG(LogTraceGame, Error, TEXT(""));
			UE_LOG(LogTraceGame, Error, TEXT("  It copies Epic's Mannequin out of YOUR OWN UE 5.8 install into"));
			UE_LOG(LogTraceGame, Error, TEXT("  Content/Characters/Mannequins. The art is gitignored on"));
			UE_LOG(LogTraceGame, Error, TEXT("  purpose (126 MB of binaries), so a fresh clone always needs"));
			UE_LOG(LogTraceGame, Error, TEXT("  this once. Missing package: %s"), *PackageNameOf(TraceCharacterAssets::MannequinMesh));
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			break;

		case ETraceCharacterArtStatus::AnimMissing:
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			UE_LOG(LogTraceGame, Error, TEXT("  CHARACTER ART IS ONLY HALF INSTALLED."));
			UE_LOG(LogTraceGame, Error, TEXT("  The Mannequin mesh loaded but %s did not, so characters will"), TraceCharacterAssets::UnarmedAnimClass);
			UE_LOG(LogTraceGame, Error, TEXT("  stand in a fixed pose and never run."));
			UE_LOG(LogTraceGame, Error, TEXT("  FIX:  %s --force"), TraceCharacterAssets::ImportCommand);
			UE_LOG(LogTraceGame, Error, TEXT("=================================================================="));
			break;

		case ETraceCharacterArtStatus::DisabledByCommandLine:
			UE_LOG(LogTraceGame, Display,
				TEXT("[CharacterArt] -TraceNoCharacterArt: the fallback primitives are being shown ON PURPOSE. ")
				TEXT("Relaunch without the switch for the Mannequin."));
			break;

		case ETraceCharacterArtStatus::NotRequired:
			UE_LOG(LogTraceGame, Display, TEXT("[CharacterArt] Dedicated server: no character art is loaded or needed."));
			break;

		default:
			break;
		}
	}
}

void ATraceCharacter::VerifyCharacterArtInstalled()
{
	// Idempotent: the first caller decides, everybody after it is free. SetupCharacterVisuals() may
	// have got here first with a definitive answer (it actually tried to LOAD the assets, which beats
	// a package-store lookup), and must not be second-guessed by a cheaper test.
	if (GCharacterArtStatus != ETraceCharacterArtStatus::Unknown)
	{
		return;
	}

	if (IsRunningDedicatedServer())
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::NotRequired);
		return;
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoCharacterArt")))
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::DisabledByCommandLine);
		return;
	}

	// DoesPackageExist, not LoadObject: this runs during BeginPlay on the HUD, before the match has
	// drawn a frame, and the whole point is to answer the question without paying 126 MB for it.
	// Works in a cooked build too — the package store knows what was cooked in.
	const bool bMeshPresent = FPackageName::DoesPackageExist(PackageNameOf(TraceCharacterAssets::MannequinMesh));
	const bool bAnimPresent = FPackageName::DoesPackageExist(PackageNameOf(TraceCharacterAssets::UnarmedAnimClass));

	if (!bMeshPresent)
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::MeshMissing);
	}
	else if (!bAnimPresent)
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::AnimMissing);
	}
	else
	{
		// Deliberately NOT reported as Ok here. Both packages exist, but "exists" is not "loads and
		// has a skeleton": SetupCharacterVisuals() is the only thing that knows that, and it will set
		// the real status the moment the first pawn is dressed. Leaving it Unknown keeps the on-screen
		// warning off (Unknown is not a warned state) without claiming a success nobody has verified.
		UE_LOG(LogTraceGame, Display,
			TEXT("[CharacterArt] Pre-flight OK: both Mannequin packages are present on disk. ")
			TEXT("Confirming on the first spawned character."));
	}
}

ETraceCharacterArtStatus ATraceCharacter::GetCharacterArtStatus()
{
	if (GCharacterArtStatus == ETraceCharacterArtStatus::Unknown)
	{
		VerifyCharacterArtInstalled();
	}
	return GCharacterArtStatus;
}

bool ATraceCharacter::GetCharacterArtWarning(FString& OutHeadline, FString& OutDetail)
{
	switch (GetCharacterArtStatus())
	{
	case ETraceCharacterArtStatus::MeshMissing:
		OutHeadline = TEXT("CHARACTER ART NOT INSTALLED");
		OutDetail = FString::Printf(TEXT("Players are placeholder shapes. Run  %s  from the project root, then relaunch."),
			TraceCharacterAssets::ImportCommand);
		return true;

	case ETraceCharacterArtStatus::AnimMissing:
		OutHeadline = TEXT("CHARACTER ANIMATIONS NOT INSTALLED");
		OutDetail = FString::Printf(TEXT("Players are posed but never move. Run  %s --force, then relaunch."),
			TraceCharacterAssets::ImportCommand);
		return true;

	case ETraceCharacterArtStatus::DisabledByCommandLine:
		// WARNED TOO, and that is the whole reason -TraceNoCharacterArt exists. The switch is there so
		// the fallback branch can be exercised on a machine where the import HAS been run; if it
		// exercised everything about the fallback EXCEPT the warning, the warning would be the one
		// part of this fix that no automated run could ever photograph. Different wording, so nobody
		// mistakes a deliberate test for a broken install.
		OutHeadline = TEXT("CHARACTER ART DISABLED (-TraceNoCharacterArt)");
		OutDetail = TEXT("This is the simulated missing-import state. Relaunch without the switch for the Mannequin.");
		return true;

	default:
		// Ok, Unknown and NotRequired draw nothing.
		OutHeadline.Reset();
		OutDetail.Reset();
		return false;
	}
}

namespace
{
	/**
	 * Where this character is looking, as a rotation.
	 *
	 * The control rotation is the authority: it exists on the owning client (locally set) and on the
	 * server (replicated inside every ServerMove), which are the only two machines that resolve a
	 * shot. A simulated proxy on a bystander's client has no controller at all, so it falls back to
	 * the replicated actor rotation — nothing gameplay-critical ever asks a simulated proxy where it
	 * is aiming.
	 */
	FRotator ResolveAimRotation(const ATraceCharacter& TraceChar)
	{
		if (const AController* OwningController = TraceChar.GetController())
		{
			return OwningController->GetControlRotation();
		}

		FRotator Rotation = TraceChar.GetActorRotation();
		Rotation.Pitch = 0.f;
		Rotation.Roll = 0.f;
		return Rotation;
	}

	/** Clamps a tint back into a sane range and forces opaque alpha (colours get scaled about). */
	/**
	 * "Is a human sitting behind this pawn's eyes on THIS machine."
	 *
	 * Not APawn::IsLocallyControlled(), which is true for every bot on the server as well — an
	 * AIController is by definition a local controller. Hiding a bot's body from "its owner" is
	 * harmless right up until something makes that bot a view target, and paying for a camera blend
	 * on nine pawns that have no camera is pure waste.
	 */
	bool IsLocalPlayerPawn(const ATraceCharacter& TraceChar)
	{
		const APlayerController* PC = Cast<APlayerController>(TraceChar.GetController());
		return PC != nullptr && PC->IsLocalController();
	}

	FLinearColor SanitizeTint(const FLinearColor& InColor)
	{
		return FLinearColor(
			FMath::Clamp(InColor.R, 0.f, 1.f),
			FMath::Clamp(InColor.G, 0.f, 1.f),
			FMath::Clamp(InColor.B, 0.f, 1.f),
			1.f);
	}

	/** Shared setup for the fallback shapes: drawable, and incapable of colliding. */
	void ConfigureVisualMesh(UStaticMeshComponent* InMesh)
	{
		if (InMesh == nullptr)
		{
			return;
		}

		// Contract §7: the capsule is the ONLY collider. A colliding mesh here would let a bullet
		// stop on "the shoulder" while the lag-compensated capsule test says it missed.
		InMesh->SetCollisionProfileName(TEXT("NoCollision"));
		InMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		InMesh->SetGenerateOverlapEvents(false);
		InMesh->SetCanEverAffectNavigation(false);
		InMesh->bReceivesDecals = false;

		// Same reason as the skeletal mesh: hidden from its owner in first person, still casting.
		InMesh->bCastHiddenShadow = true;
	}
}

// =================================================================================================
// Construction
// =================================================================================================

ATraceCharacter::ATraceCharacter(const FObjectInitializer& OI)
	// This is the whole reason the FObjectInitializer constructor exists: ACharacter creates its
	// movement component as a named default subobject, and overriding the class here is the only
	// way to get UTraceCharacterMovementComponent (and with it the predicted dash) in its place.
	: Super(OI.SetDefaultSubobjectClass<UTraceCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	// The actor tick exists for exactly one thing: the first/third person camera blend, which needs
	// a per-frame delta and nothing else. AActor defaults it off and ACharacter does not override
	// ::Tick at all in 5.8, so this is a new tick function rather than a re-enabled one; everything
	// else here still ticks itself (the movement component, lag compensation, the trail).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;
	SetReplicateMovement(true);

	// Set here so the class default is right for a pawn that is spawned and never possessed, and
	// re-derived from that default every time ACharacter::UnCrouch calls RecalculateBaseEyeHeight().
	// The first-person camera is placed at exactly this height — see EyeHeight.
	BaseEyeHeight = TraceCharacterLayout::EyeHeight;

	// A starting value only. Crouching now exists (it is how sliding works — spec section 5), and the
	// real crouched eye height depends on the crouched capsule height, which belongs to
	// UTraceCharacterMovementComponent. OnStartCrouch() computes it from the resized capsule; see the
	// note on the override.
	CrouchedEyeHeight = TraceCharacterLayout::EyeHeight;

	// Facing is mode-dependent and is configured by ApplyRotationMode() once a controller exists —
	// first person turns the body with the aim, carrying turns it with the movement. These class
	// defaults are the carrying/bot case (capsule follows its movement, camera and shot direction
	// follow the control rotation), which is also the only correct answer for an unpossessed pawn.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->InitCapsuleSize(TraceCharacterLayout::CapsuleRadius, TraceCharacterLayout::CapsuleHalfHeight);
		Capsule->SetCollisionProfileName(TEXT("Pawn"));

		// The spring arm below probes on ECC_Camera, and the engine's Pawn profile overrides only
		// Visibility to Ignore - Camera stays Block. In a 5v5 that means any teammate standing behind
		// you yanks your camera into your own head. Pawn-vs-Pawn *movement* blocking is on the
		// WorldDynamic/Pawn channels and is unaffected by this.
		Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

		// Both sides of an overlap pair must generate events. The endzone trigger and the Core's
		// pickup sphere are the other half; without this the game has no way to score or to pick
		// the Core up.
		Capsule->SetGenerateOverlapEvents(true);
	}

	// --- The character you actually see ----------------------------------------------------------
	//
	// ACharacter already owns a USkeletalMeshComponent; this places it and makes it harmless. The
	// asset and the anim blueprint are attached later, in SetupCharacterVisuals(), because they are
	// imported per developer and may not exist (see the header).
	//
	// The relative transform is set HERE and nowhere else: ACharacter::PostInitializeComponents()
	// snapshots it into BaseTranslationOffset/BaseRotationOffset, which root motion and crouch then
	// work against. Moving the mesh after that point desynchronises those.
	if (USkeletalMeshComponent* SkeletalMesh = GetMesh())
	{
		SkeletalMesh->SetRelativeLocation(FVector(0.f, 0.f, TraceCharacterLayout::MeshOffsetZ));
		SkeletalMesh->SetRelativeRotation(FRotator(0.f, TraceCharacterLayout::MeshYaw, 0.f));

		// Contract §7 again: the capsule is the only collider. The mannequin ships with a physics
		// asset (PA_Mannequin) and would otherwise start blocking bullets per-bone, which is exactly
		// the "hit the shoulder, miss the capsule" desync the rest of this file is built to avoid.
		SkeletalMesh->SetCollisionProfileName(TEXT("NoCollision"));
		SkeletalMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SkeletalMesh->SetGenerateOverlapEvents(false);
		SkeletalMesh->SetCanEverAffectNavigation(false);
		SkeletalMesh->bReceivesDecals = false;

		// In first person this mesh is hidden from its own owner (SetOwnerNoSee). Without this the
		// player would also lose their own shadow, and on a black floor with hard neon key lights the
		// shadow is most of what tells you where your feet are. Costs one extra shadow-only draw for
		// the one pawn that is hidden, and nothing at all for the other nine.
		SkeletalMesh->bCastHiddenShadow = true;

		// Ten characters in a 5v5, most of them off-screen most of the time. Skipping the pose for
		// anything not rendered is the single biggest animation saving available and costs nothing
		// visible — the pose is rebuilt the moment the character comes back on screen.
		SkeletalMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;

		// Hidden until SetupCharacterVisuals() confirms there is a mesh to show, so a missing import
		// can never leave a T-posing null-mesh component drawing nothing over the fallback shapes.
		SkeletalMesh->SetVisibility(false);
	}

	// Soft references, resolved once per pawn in PostInitializeComponents(). Assigning them here
	// (rather than in a config or a Blueprint) keeps them on the CDO, which is what makes the cooker
	// follow them into a packaged build.
	CharacterMeshAsset = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TraceCharacterAssets::MannequinMesh));
	CharacterAnimClass = TSoftClassPtr<UAnimInstance>(FSoftClassPath(TraceCharacterAssets::UnarmedAnimClass));

	// --- Camera rig ------------------------------------------------------------------------------

	// ONE arm, ONE camera, for both view modes — UpdateViewBlend() lerps the arm between them. Two
	// camera actors and a SetViewTarget would put a player camera manager blend on top of ours and
	// would cut, not travel; the travel is the whole point.
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);

	// First person is the default state of the game, so the rig is built collapsed onto the eye and
	// only opens out when the Core is picked up.
	SpringArm->TargetArmLength = TraceCharacterLayout::FirstPersonArmLength;
	SpringArm->TargetOffset = FVector(0.f, 0.f, TraceCharacterLayout::EyeHeight);
	// SocketOffset stays zero in both modes — see ThirdPersonPivotZ for why the height lives in
	// TargetOffset instead.
	SpringArm->SocketOffset = FVector::ZeroVector;

	SpringArm->bUsePawnControlRotation = true;
	SpringArm->bDoCollisionTest = true;
	// Camera lag would smear the crosshair away from the aim direction the weapon actually uses.
	SpringArm->bEnableCameraLag = false;
	SpringArm->bEnableCameraRotationLag = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;   // the arm already applied it
	Camera->SetFieldOfView(TraceCharacterLayout::CameraFOV);

	// First-person rendering parameters. These affect ONLY primitives tagged
	// EFirstPersonPrimitiveType::FirstPerson — i.e. the viewmodel and nothing else in the world — so
	// they are harmless while the camera is in third person, where the viewmodel is hidden anyway.
	//
	// FirstPersonFieldOfView 70 against a scene FOV of 95: the renderer cancels the scene projection
	// and re-applies this one, so the gun is drawn through a normal lens instead of the wide one the
	// arena needs, which is what stops a viewmodel at arm's length looking stretched and enormous.
	// FirstPersonScale 0.5 then squashes the rig's DEPTH range toward the camera by half while
	// preserving the solid angle it covers, so it looks identical and can no longer reach into the
	// scene. See the depth arithmetic in the file header.
	Camera->bEnableFirstPersonFieldOfView = true;
	Camera->bEnableFirstPersonScale = true;
	Camera->FirstPersonFieldOfView = TraceCharacterLayout::FirstPersonViewModelFOV;
	Camera->FirstPersonScale = TraceCharacterLayout::FirstPersonViewModelScale;

	// The viewmodel rig hangs off the CAMERA, not off the capsule or the mesh: it has to inherit the
	// aim exactly, and the camera is the one thing in the hierarchy that already has it. Nothing in
	// the shot path reads this component, so animating it cannot move a bullet.
	ViewModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ViewModelRoot"));
	ViewModelRoot->SetupAttachment(Camera);
	ViewModelRoot->SetRelativeLocationAndRotation(
		TraceCharacterLayout::ViewModelRestLocation, TraceCharacterLayout::ViewModelRestRotation);

	// --- Fallback shapes -------------------------------------------------------------------------
	//
	// A clone that has not run Scripts/import-mannequin.sh has no character art at all. Rather than
	// render nothing — an invisible player is far worse than an ugly one — the pawn keeps the old
	// primitive body/head, built from /Engine/BasicShapes which ship with every install, hidden
	// unless the mannequin fails to load.

	FallbackBodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FallbackBodyMesh"));
	FallbackBodyMesh->SetupAttachment(RootComponent);
	ConfigureVisualMesh(FallbackBodyMesh);
	FallbackBodyMesh->SetVisibility(false);

	FallbackHeadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FallbackHeadMesh"));
	FallbackHeadMesh->SetupAttachment(RootComponent);
	ConfigureVisualMesh(FallbackHeadMesh);
	FallbackHeadMesh->SetVisibility(false);

	// Constructor-time FObjectFinders (not runtime LoadObject) on purpose: the reference lands on
	// the CDO, which is what makes the cooker pull these engine assets into a packaged build. A
	// bare runtime load of the same path would resolve in the editor and return null in a package.
	// These are /Engine/ assets, so unlike the mannequin they are always present and a hard
	// reference is safe.
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
		static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

		if (CubeFinder.Succeeded())
		{
			CubeMesh = CubeFinder.Object;
		}
		if (CylinderFinder.Succeeded())
		{
			CylinderMesh = CylinderFinder.Object;
			FallbackBodyMesh->SetStaticMesh(CylinderFinder.Object);
		}
		if (SphereFinder.Succeeded())
		{
			FallbackHeadMesh->SetStaticMesh(SphereFinder.Object);
		}
		if (MaterialFinder.Succeeded())
		{
			BasicShapeMaterial = MaterialFinder.Object;
		}
	}

	// The generated Tron materials, resolved the same way ATraceArenaBuilder resolves them: a
	// constructor-time finder so the reference lands on the CDO and the cooker follows it, and a
	// tolerated miss (the folder is gitignored and produced by Scripts/generate_content.py) that
	// MakeViewModelMaterials() turns into a BasicShapeMaterial fallback.
	{
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> SurfaceFinder(TraceCharacterAssets::SurfaceMaterialPath);
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TraceCharacterAssets::NeonMaterialPath);

		if (SurfaceFinder.Succeeded())
		{
			SurfaceMaterial = SurfaceFinder.Object;
		}
		if (NeonFinder.Succeeded())
		{
			NeonMaterial = NeonFinder.Object;
		}
	}

	// Body: a cylinder standing on the bottom of the capsule, as wide as the capsule is.
	{
		const float BodyScaleXY = (TraceCharacterLayout::CapsuleRadius * 2.f) / TraceCharacterLayout::BasicShapeSize;
		const float BodyScaleZ = TraceCharacterLayout::BodyHeight / TraceCharacterLayout::BasicShapeSize;
		FallbackBodyMesh->SetRelativeScale3D(FVector(BodyScaleXY, BodyScaleXY, BodyScaleZ));
		FallbackBodyMesh->SetRelativeLocation(FVector(
			0.f, 0.f, (TraceCharacterLayout::BodyHeight * 0.5f) - TraceCharacterLayout::CapsuleHalfHeight));
	}

	// Head: a sphere capping the body, overlapping it slightly so the silhouette reads as one piece.
	{
		const float HeadScale = TraceCharacterLayout::HeadDiameter / TraceCharacterLayout::BasicShapeSize;
		FallbackHeadMesh->SetRelativeScale3D(FVector(HeadScale));
		FallbackHeadMesh->SetRelativeLocation(FVector(0.f, 0.f, TraceCharacterLayout::HeadCentreZ));
	}

	// --- Slide skid streak -------------------------------------------------------------------------
	//
	// Flat on the deck under the feet, unlit, team-coloured, and hidden until the pawn is actually
	// sliding. See UpdateCrouchPresentation for why this exists at all.
	SlideSkidMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SlideSkidMesh"));
	SlideSkidMesh->SetupAttachment(RootComponent);
	// After the FObjectFinder block above, which is where CubeMesh is resolved. A null mesh here
	// would leave the streak invisible AND leave CreateDynamicMaterialInstance with no slot to wrap,
	// so ApplyTeamColors would silently never build SlideSkidMID either.
	if (CubeMesh != nullptr)
	{
		SlideSkidMesh->SetStaticMesh(CubeMesh);
	}
	ConfigureVisualMesh(SlideSkidMesh);
	SlideSkidMesh->SetVisibility(false);
	// The one visual on this pawn that must NOT be hidden from its owner in first person and must
	// NOT cast a shadow: it is a light on the floor, not part of the body.
	SlideSkidMesh->bCastHiddenShadow = false;
	SlideSkidMesh->SetCastShadow(false);
	SlideSkidMesh->SetRelativeLocation(FVector(0.f, 0.f, -TraceCharacterLayout::CapsuleHalfHeight + 3.f));

	// --- Movement capabilities ---------------------------------------------------------------------
	//
	// Crouch has to be ALLOWED on the nav agent or UCharacterMovementComponent::Crouch() silently
	// refuses, and crouch is how spec section 5's slide is implemented. Set here rather than in the
	// movement component because it is a property of the pawn, and setting it twice is free.
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->GetNavAgentPropertiesRef().bCanCrouch = true;
	}

	// --- Gameplay components ---------------------------------------------------------------------

	Health = CreateDefaultSubobject<UTraceHealthComponent>(TEXT("Health"));
	Weapon = CreateDefaultSubobject<UTraceWeaponComponent>(TEXT("Weapon"));
	LagComp = CreateDefaultSubobject<UTraceLagCompensationComponent>(TEXT("LagComp"));

	// The trail is a scene component: its points are laid in world space from the owner's position,
	// so it needs a transform in the hierarchy.
	Trail = CreateDefaultSubobject<UTraceTrailComponent>(TEXT("Trail"));
	Trail->SetupAttachment(RootComponent);
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void ATraceCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// After Super, deliberately: ACharacter::PostInitializeComponents() is what snapshots the mesh's
	// relative transform and initialises the anim instance machinery. Dressing the mesh before that
	// would have the base class re-derive its offsets from a component we were halfway through
	// changing.
	SetupCharacterVisuals();
}

void ATraceCharacter::SetupCharacterVisuals()
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp == nullptr)
	{
		return;
	}

	// A dedicated server renders nothing and never builds a pose. Loading 126 MB of character art
	// there would be pure cost — and the capsule, which is what the server actually simulates and
	// tests against, is unaffected either way.
	if (IsRunningDedicatedServer())
	{
		ReportCharacterArtStatus(ETraceCharacterArtStatus::NotRequired);
		MeshComp->SetVisibility(false);
		return;
	}

	// -TraceNoCharacterArt exists so the not-imported path can actually be TESTED, on a machine where
	// the import has been run, without deleting anyone's Content folder out from under a parallel
	// session. The fallback is the branch every fresh clone takes; leaving it unexercised until a
	// new hire hits it is how "optional asset" handling quietly rots.
	const bool bForceNoArt = FParse::Param(FCommandLine::Get(), TEXT("TraceNoCharacterArt"));

	// LoadSynchronous on a soft pointer: the first pawn pays for the load, every pawn after it gets
	// the already-resident object back. Ten characters spawn at match start, so this happens once.
	USkeletalMesh* LoadedMesh = (bForceNoArt || CharacterMeshAsset.IsNull())
		? nullptr
		: CharacterMeshAsset.LoadSynchronous();

	if (LoadedMesh == nullptr)
	{
		// The whole point of the soft reference. No crash, no invisible player: show the primitives
		// and say exactly what to run. Banner-logged once per process rather than once per pawn,
		// because ten identical screenfuls of this would bury everything else in the log — and
		// mirrored onto the HUD by ATraceHUD::DrawArtWarning(), because a log line demonstrably was
		// not enough (this is the bug that got reported as "the character models were not replaced").
		ReportCharacterArtStatus(bForceNoArt
			? ETraceCharacterArtStatus::DisabledByCommandLine
			: ETraceCharacterArtStatus::MeshMissing);

		static bool bWarnedMissingMesh = false;
		if (!bWarnedMissingMesh)
		{
			bWarnedMissingMesh = true;
			UE_LOG(LogTraceGame, Warning, TEXT("%s (looked for %s)"),
				TraceCharacterAssets::MissingImportHint, TraceCharacterAssets::MannequinMesh);
		}

		bUsingSkeletalMesh = false;
		MeshComp->SetVisibility(false);
		if (FallbackBodyMesh != nullptr) { FallbackBodyMesh->SetVisibility(true); }
		if (FallbackHeadMesh != nullptr) { FallbackHeadMesh->SetVisibility(true); }
		ApplyTeamColors();
		return;
	}

	MeshComp->SetSkeletalMeshAsset(LoadedMesh);
	MeshComp->SetVisibility(true);

	bUsingSkeletalMesh = true;
	if (FallbackBodyMesh != nullptr) { FallbackBodyMesh->SetVisibility(false); }
	if (FallbackHeadMesh != nullptr) { FallbackHeadMesh->SetVisibility(false); }

	// The anim blueprint is loaded separately and is separately optional: a character with a mesh
	// but no anim instance is a T-pose, which is ugly but still playable and still correctly
	// team-coloured. That is a better failure than refusing to draw the player.
	UClass* LoadedAnimClass = (bForceNoArt || CharacterAnimClass.IsNull())
		? nullptr
		: CharacterAnimClass.LoadSynchronous();
	if (LoadedAnimClass != nullptr)
	{
		MeshComp->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		MeshComp->SetAnimInstanceClass(LoadedAnimClass);

		// The ONLY place in the codebase that can honestly say the art is fine: both objects loaded,
		// the mesh is attached and the anim blueprint is instanced. Everything else is a prediction.
		ReportCharacterArtStatus(ETraceCharacterArtStatus::Ok);
	}
	else
	{
		ReportCharacterArtStatus(bForceNoArt
			? ETraceCharacterArtStatus::DisabledByCommandLine
			: ETraceCharacterArtStatus::AnimMissing);

		static bool bWarnedMissingAnim = false;
		if (!bWarnedMissingAnim)
		{
			bWarnedMissingAnim = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("Character mesh loaded but its anim blueprint did not (%s). Characters will be ")
				TEXT("posed but not animated; re-run Scripts/import-mannequin.sh --force."),
				TraceCharacterAssets::UnarmedAnimClass);
		}
	}

	// Slot count is only knowable once the mesh is attached, so the MIDs are built from here.
	ApplyTeamColors();
}

void ATraceCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (Health != nullptr)
	{
		// AddUnique so a re-entrant BeginPlay (seamless travel, level streaming) cannot double-bind
		// and turn one death into two.
		Health->OnDeath.AddUniqueDynamic(this, &ATraceCharacter::OnHealthDeath);
	}

	UWorld* World = GetWorld();

	if (HasAuthority() && World != nullptr)
	{
		// The GameMode's roster is what the lag-compensated hitscan resolver and the trail trip test
		// iterate. A character that fails to register is invisible to both: unshootable and unable
		// to trip a trail.
		if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			GameMode->RegisterCharacter(this);
		}
	}

	ApplyTeamColors();

	// Snapped, never blended: a pawn spawns without the Core, so the first frame must already be
	// first person. Blending into it would fly every camera in the match forward from 450 uu back.
	ApplyRotationMode();
	UpdateViewBlend(0.f, /*bSnap=*/true);

	// Team colours are cosmetic, so the poll costs a dedicated server nothing (it never runs there).
	if (World != nullptr && GetNetMode() != NM_DedicatedServer && GetTeam() == ETraceTeam::None)
	{
		World->GetTimerManager().SetTimer(
			TeamColorTimerHandle, this, &ATraceCharacter::PollTeamColors,
			TraceCharacterLayout::TeamColorPollInterval, /*bLoop=*/true);
	}
}

void ATraceCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(TeamColorTimerHandle);

		if (HasAuthority())
		{
			// Unregister before Super: the roster holds weak pointers, but leaving a dead entry in
			// it makes every iteration pay for it until the next compaction.
			if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
			{
				GameMode->UnregisterCharacter(this);
			}
		}
	}

	if (Health != nullptr)
	{
		Health->OnDeath.RemoveDynamic(this, &ATraceCharacter::OnHealthDeath);
	}

	Super::EndPlay(EndPlayReason);
}

void ATraceCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The only reason this pawn ticks. See UpdateViewBlend().
	//
	// The rotation model is re-asserted here rather than only on possession because on a client the
	// controller arrives by replication, after the pawn: there is no PossessedBy() on that machine.
	// Both calls early-out on "nothing changed", so the steady-state cost is two branches.
	const bool bLocalPlayer = IsLocalPlayerPawn(*this);
	const bool bJustBecameLocal = bLocalPlayer && !bWasLocallyControlled;
	bWasLocallyControlled = bLocalPlayer;

	ApplyRotationMode();

	// BEFORE UpdateViewBlend, and the order is load-bearing: this is what moves BaseEyeHeight for a
	// slide, and UpdateViewBlend pins the spring arm to BaseEyeHeight. Running it after would leave
	// the camera one frame behind the point the shot is built from for the whole descent, i.e. a
	// non-zero eyeErr exactly while the player is sliding. Runs for every pawn on every machine.
	UpdateCrouchPresentation(DeltaSeconds);

	UpdateViewBlend(DeltaSeconds, /*bSnap=*/bJustBecameLocal);

	// Local player only — UpdateViewBlend has already hidden the rig on everyone else, and this
	// early-outs on a hidden one.
	if (bLocalPlayer)
	{
		UpdateViewModel(DeltaSeconds);
	}
}

void ATraceCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Server side, this is the first moment GetPlayerState() can answer — and therefore the first
	// moment the team is knowable.
	ApplyTeamColors();

	// ...and the first moment the pawn knows whether it is a bot or a human, which is what decides
	// the rotation model. Snapped for the same reason as BeginPlay: a fresh pawn has no Core.
	ApplyRotationMode();
	UpdateViewBlend(0.f, /*bSnap=*/true);
}

void ATraceCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	// Client side, the PlayerState pointer can arrive before or after the pawn, and its Team can
	// arrive after that again. Every path that could learn the team ends up here.
	ApplyTeamColors();
}

void ATraceCharacter::FellOutOfWorld(const UDamageType& DmgType)
{
	if (HasAuthority() && IsAlive() && Health != nullptr)
	{
		// Kill(), not ApplyDamage(): the carrier is invulnerable to damage, and a carrier who falls
		// out of the world would otherwise take the Core with them, permanently.
		Health->Kill(GetController(), FName(TEXT("Fell")));

		// Deliberately not calling Super here. AActor::FellOutOfWorld destroys the actor outright,
		// and the GameMode still needs a live pawn to read the death location from and to credit the
		// death against. The corpse is frozen by SetDeadPresentation(); when the engine tests the
		// kill Z again on a later frame this character is no longer alive, so the call falls through
		// to Super and the body is cleaned up then.
		return;
	}

	Super::FellOutOfWorld(DmgType);
}

void ATraceCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None: every client needs this, not just the owner — it drives the carrier tint, the HUD
	// banner and the trail's "who is the carrier" question.
	DOREPLIFETIME(ATraceCharacter, bIsCarrier);

	// COND_SkipOwner: the owning client predicts its own slide from the saved move and already has a
	// better answer than any packet could carry. Everyone ELSE has no way to know at all — see the
	// property's comment; without this a sliding player's hit zones desync from what a shooter sees.
	DOREPLIFETIME_CONDITION(ATraceCharacter, bReplicatedSliding, COND_SkipOwner);
}

// =================================================================================================
// Queries
// =================================================================================================

ETraceTeam ATraceCharacter::GetTeam() const
{
	// Team lives on the PlayerState because it outlives the pawn (contract §7). Null until the
	// PlayerState has replicated, which is why ApplyTeamColors() is re-entrant and polled.
	if (const ATracePlayerState* State = GetPlayerState<ATracePlayerState>())
	{
		return State->Team;
	}
	return ETraceTeam::None;
}

bool ATraceCharacter::IsCarrier() const
{
	return bIsCarrier;
}

bool ATraceCharacter::IsAlive() const
{
	return Health != nullptr && Health->IsAlive();
}

bool ATraceCharacter::IsDashing() const
{
	const UTraceCharacterMovementComponent* Movement = GetTraceMovement();
	return Movement != nullptr && Movement->IsDashing();
}

UTraceCharacterMovementComponent* ATraceCharacter::GetTraceMovement() const
{
	// Not cached: Cast<> on a known-typed subobject is a pointer compare against the class chain,
	// and caching a component pointer in a UPROPERTY set from the constructor is a subtler thing
	// than it looks when the CDO is involved.
	return Cast<UTraceCharacterMovementComponent>(GetCharacterMovement());
}

#if !UE_BUILD_SHIPPING
/**
 * Dev only. Forces the third-person camera regardless of the Core, so the procedural slide pose can
 * actually be LOOKED AT.
 *
 * It exists because the pose is the one part of spec v4 §1 that cannot be verified from a log line,
 * and the pawn a headless run drives is in first person — where its own body is deliberately hidden.
 * Bots are the only other sliding bodies and they are specks at arena scale. Cheat-flagged, compiled
 * out of shipping, and it changes nothing but which camera the local player looks through.
 */
int32 GTraceForceThirdPerson = 0;
static FAutoConsoleVariableRef CVarTraceForceThirdPerson(
	TEXT("Trace.ForceThirdPerson"),
	GTraceForceThirdPerson,
	TEXT("Dev only. Non-zero forces the third-person camera on the local player, so the slide pose and "
	     "the carry blend can be inspected without holding the Core."),
	ECVF_Cheat);
#endif

bool ATraceCharacter::WantsFirstPersonView() const
{
#if !UE_BUILD_SHIPPING
	if (GTraceForceThirdPerson != 0)
	{
		return false;
	}
#endif

	// The entire rule. Carrying the Core is the one state that needs the space behind you visible,
	// because the trail you are laying there is the only thing that can kill you.
	return !bIsCarrier;
}

float ATraceCharacter::GetViewBlendAlpha() const
{
	return ViewBlendAlpha;
}

int32 ATraceCharacter::GetViewModelPartCount() const
{
	return ViewModelParts.Num();
}

bool ATraceCharacter::IsViewModelVisible() const
{
	return bViewModelVisible;
}

FVector ATraceCharacter::GetMuzzleLocation() const
{
	// ON THE AIM RAY. The eye, stepped forward along the full aim rotation — pitch included, unlike
	// the old chest muzzle which used yaw only and therefore sat off the ray whenever the player
	// looked anywhere but the horizon.
	//
	// GetPawnViewLocation() (actor + BaseEyeHeight) rather than the camera component's world
	// location, deliberately and for the same reason as in GetAimDirection(): it is pure arithmetic
	// on the actor transform, so the server computes the same origin as the client. The camera's
	// real position depends on the spring arm's collision probe and on where the view blend happens
	// to be this frame, neither of which the server knows or should know.
	//
	// Every shot in the game is fired from first person (a carrier cannot fire), so this is the
	// first-person muzzle and there is no second case to keep in step.
	const FVector ViewForward = ResolveAimRotation(*this).Vector();

	return GetPawnViewLocation() + ViewForward * TraceCharacterLayout::MuzzleForward;
}

FVector ATraceCharacter::GetAimDirection() const
{
	const FRotator AimRotation = ResolveAimRotation(*this);
	const FVector ViewForward = AimRotation.Vector();

	// Aim at a point far along the *view* ray and shoot from the muzzle toward it, so the shot
	// converges on whatever the crosshair covers wherever the muzzle happens to be.
	//
	// Since the muzzle was moved onto the ray itself this is arithmetically the identity — the
	// focus point, the muzzle and the eye are collinear, so Converged == ViewForward to float
	// precision. That is the point: the crosshair is exact in first person, and it stays exact if
	// anyone later moves the muzzle to a weapon socket off the axis.
	//
	// GetPawnViewLocation() is used rather than the camera component's world location on purpose: it
	// is pure arithmetic on the actor transform, so the server computes the same answer as the
	// client. The camera's real position depends on the spring arm's collision probe, which can and
	// does differ between machines.
	const FVector FocusPoint = GetPawnViewLocation() + ViewForward * TraceCharacterLayout::AimConvergenceDistance;
	const FVector Converged = (FocusPoint - GetMuzzleLocation()).GetSafeNormal();

	return Converged.IsNearlyZero() ? ViewForward : Converged;
}

// =================================================================================================
// Server-authoritative state
// =================================================================================================

void ATraceCharacter::SetCarrying(bool bNewCarrying)
{
	if (!HasAuthority() || bIsCarrier == bNewCarrying)
	{
		return;
	}

	bIsCarrier = bNewCarrying;

	// THIS IS THE SOLE WRITER of ATracePlayerState::bIsCarrier for a pawn that still exists.
	//
	// It used to be one of FOUR: ATraceCore::GrantTo, ATraceCore::ReleaseHolder and
	// ATraceGameMode::NotifyCharacterDied all wrote the mirror too, and every one of them also called
	// SetCarrying — so three of the four writes were pure duplication, and duplicated state with
	// scattered writers is what the whole-project audit named as the most likely source of the next
	// bug. Those three are gone. If you find yourself adding a fourth, the answer is to call
	// SetCarrying() instead.
	//
	// The ONE case this cannot cover is a holder whose pawn has already been destroyed: the
	// PlayerState outlives the pawn, so there is no pawn left to call. ATraceCore caches the holder's
	// PlayerState at grant time and clears it in ReleaseHolder() for exactly that case — see the
	// comment there. Do not re-add a write here to "fix" it; a destroyed pawn cannot reach this line.
	if (ATracePlayerState* State = GetPlayerState<ATracePlayerState>())
	{
		State->bIsCarrier = bNewCarrying;
		// The scoreboard reads this on every client; without a nudge it waits for the PlayerState's
		// ordinary replication interval, which is long enough to be seen as a wrong scoreboard.
		State->ForceNetUpdate();
	}

	if (Trail != nullptr)
	{
		Trail->SetEmitting(bNewCarrying);
	}

	if (bNewCarrying && Weapon != nullptr)
	{
		// The carrier cannot shoot (contract §3). Dropping the trigger here means a player who was
		// mid-burst when they caught the Core does not resume firing the instant they pass it on.
		Weapon->StopFire();
	}

	// Replication callbacks never fire on the authority; run it by hand so a listen server tints
	// itself exactly like a remote client does.
	OnRep_IsCarrier();

	// Carrier state gates invulnerability and the HUD banner — worth a packet immediately.
	ForceNetUpdate();

	// Display, not Verbose, and one line per possession change is cheap at ten players. bIsCarrier is
	// the ONLY input to the view mode (WantsFirstPersonView), so "the camera is stuck in third
	// person" is always a question about this exact line: either it never ran, or it ran on the
	// server and the owning client never heard about it. Without it, both look identical.
	UE_LOG(LogTraceGame, Display, TEXT("[Carry] %s bIsCarrier -> %d (authority, netmode=%d)"),
		*GetName(), bNewCarrying ? 1 : 0, static_cast<int32>(GetNetMode()));
}

void ATraceCharacter::HandleDeath(AController* Killer, FName Cause)
{
	if (!HasAuthority() || bDeathHandled)
	{
		return;
	}
	bDeathHandled = true;

	if (Weapon != nullptr)
	{
		Weapon->StopFire();
	}

	// The trail is the carrier's threat and it must not outlive them by even a frame (contract §3:
	// "on carrier death the trail is cleared instantly"). Non-carriers have an empty trail, so this
	// is unconditional and cheap. The GameMode repeats it; both calls are idempotent.
	if (Trail != nullptr)
	{
		Trail->SetEmitting(false);
		Trail->ClearTrail();
	}

	// Normally already applied — Health::Kill/ApplyDamage call OnRep_Health on the server, which
	// routes here. Repeating it covers a direct HandleDeath() call and costs nothing.
	SetDeadPresentation(true);

	UE_LOG(LogTraceGame, Verbose, TEXT("%s died (%s)"), *GetName(), *Cause.ToString());

	// Last, and while still possessed: the GameMode drops the Core at this location, credits the
	// kill from the still-attached PlayerState and schedules the respawn.
	if (UWorld* World = GetWorld())
	{
		if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			GameMode->NotifyCharacterDied(this, Killer, Cause);
		}
	}
}

void ATraceCharacter::OnHealthDeath(AActor* Victim, AController* Killer, FName Cause)
{
	// Victim is always this actor; the delegate carries it for listeners that bind to many pawns.
	HandleDeath(Killer, Cause);
}

void ATraceCharacter::SetDeadPresentation(bool bDead)
{
	if (bDeadPresentation == bDead)
	{
		return;
	}
	bDeadPresentation = bDead;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		if (bDead)
		{
			Movement->StopMovementImmediately();
			Movement->DisableMovement();
		}
		else
		{
			Movement->SetMovementMode(MOVE_Walking);
		}
	}

	if (!bDead)
	{
		// Coming back to life re-arms the death latch. The GameMode normally destroys a corpse and
		// spawns a fresh pawn, but nothing in the contract forbids reviving one in place — and a
		// revived pawn that could never die again would be a very quiet bug.
		bDeathHandled = false;
	}

	// Corpses stop blocking: bullets, dashes and the endzone trigger all pass straight through one.
	// This runs on every machine (see the header) so no client is left colliding with something the
	// server does not, which would otherwise show up as rubber-banding around a body.
	// The visual meshes are NoCollision in their own right and stay that way through this toggle.
	SetActorEnableCollision(!bDead);

	// And the body itself goes, on the same frame, on every machine. There is no dying animation to
	// wait for and no ragdoll to settle: the pawn is already frozen and already non-colliding, so a
	// lingering mesh is a prop that reads as a live player from any distance at which you cannot see
	// that it is dimmed. Deliberately AFTER the collision toggle, so nothing can ever be invisible
	// and still solid.
	SetCorpseHidden(bDead);

	ApplyTeamColors();
}

void ATraceCharacter::SetCorpseHidden(bool bInHidden)
{
	// Note the parameter name. A local or parameter called bHidden would shadow AActor's own member
	// and fail the Windows build on C4458 — this file has already paid that toll once.

	// One flag, every primitive this actor owns: the mannequin, both fallback shapes, the skid
	// streak, the pooled trail meshes and every viewmodel part. See the header for why this is done
	// at the actor level rather than component by component.
	SetActorHiddenInGame(bInHidden);

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		// bCastHiddenShadow is set in the constructor so that a first-person player, whose own mesh is
		// hidden from their own camera, still casts a shadow they can find their feet by. On a corpse
		// that same flag is a bug with a very obvious symptom: the body disappears and a body-shaped
		// shadow stays standing on the floor. Cleared while dead, restored on revive.
		MeshComp->SetCastHiddenShadow(!bInHidden);

		// A hidden mesh under OnlyTickPoseWhenRendered stops evaluating anyway; saying so explicitly
		// keeps a dead pawn from being the one skeleton still animating because it happened to be the
		// local player's (SetOwnBodyHiddenFromOwner puts that one on AlwaysTickPoseAndRefreshBones).
		if (bInHidden)
		{
			MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		}
	}

	if (bInHidden)
	{
		// The viewmodel hangs off the CAMERA, and the camera is still the local player's view target
		// while the respawn timer runs — so its own visibility bool has to be brought in line, or
		// UpdateViewBlend would go on believing the gun is shown and skip the restore.
		SetViewModelVisible(false);
	}

	// Display, not Verbose. This project has twice declared a mechanic dead because the only line
	// proving it ran was suppressed at the default verbosity of an automated run — and "did the body
	// actually go away on every machine" is precisely the kind of claim a screenshot cannot settle
	// for the nine pawns that were off camera. One line per death and one per respawn.
	UE_LOG(LogTraceGame, Display, TEXT("[Corpse] %s %s (hiddenInGame=%d, alive=%d, authority=%d)"),
		*GetName(),
		bInHidden ? TEXT("hidden on death") : TEXT("shown on respawn"),
		IsHidden() ? 1 : 0,
		IsAlive() ? 1 : 0,
		HasAuthority() ? 1 : 0);

	// NOT touched here: the Core. It is a separate actor that merely attaches to its holder, it is
	// still in play, and hiding it with the body would blank the one object the whole match is about.
	// ATraceCore's own holder-sanity pass releases it from a dead carrier (see its Tick), which is
	// what makes the orb leave the corpse.
	//
	// NOT touched either: lag compensation. UTraceLagCompensationComponent records the CAPSULE pose,
	// which is a transform and is unaffected by visibility; it also skips targets that are not alive.
	// Hit registration for everyone still breathing is untouched by any of this.
}

// =================================================================================================
// Presentation
// =================================================================================================

void ATraceCharacter::ApplyTeamColors()
{
	// A dedicated server cooks no material shaders and renders nothing, so there is neither a need
	// nor a guarantee that the basic-shape material resolves there.
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	const FLinearColor TeamColor = TraceTeamColor(GetTeam());

	// Carrier reads as "lit up". Pull hard toward white so the Core holder is unmistakable at arena
	// distance, but keep enough hue to tell which team is holding it.
	// Blended per component on purpose. FMath::Lerp<T> expands to `A + Alpha * (B - A)`, i.e. it
	// needs `float * FLinearColor`; whether that free operator exists has moved around across the
	// 5.x line, and UI/TraceHUD.cpp hand-rolls its own colour lerp for exactly this reason. Doing it
	// component-wise costs nothing and cannot be wrong on any engine version.
	FLinearColor BodyColor = TeamColor;
	if (bIsCarrier)
	{
		constexpr float CarrierWhiteBlend = 0.72f;
		BodyColor = FLinearColor(
			FMath::Lerp(TeamColor.R, 1.f, CarrierWhiteBlend),
			FMath::Lerp(TeamColor.G, 1.f, CarrierWhiteBlend),
			FMath::Lerp(TeamColor.B, 1.f, CarrierWhiteBlend),
			1.f);
	}
	FLinearColor HeadColor = bIsCarrier ? FLinearColor::White : (TeamColor * 1.4f);

	// See EmissiveNormal: a no-op on the stock Mannequin materials, kept so a material with a live
	// emissive would light up for free. The carrier is made distinct by the near-white tint above,
	// which is the part that has been confirmed to work.
	float EmissivePower = bIsCarrier
		? TraceCharacterLayout::EmissiveCarrier
		: TraceCharacterLayout::EmissiveNormal;

	if (bDeadPresentation)
	{
		// Dim rather than hide: seeing where someone died is useful information, and it makes the
		// respawn delay legible without any UI. The glow goes out entirely, which reads instantly as
		// "that one is not a threat".
		BodyColor *= 0.2f;
		HeadColor *= 0.2f;
		EmissivePower = TraceCharacterLayout::EmissiveDead;
	}

	if (bUsingSkeletalMesh)
	{
		ApplyColorToSkeletalMesh(SanitizeTint(BodyColor), EmissivePower);
	}
	else
	{
		ApplyColorToMesh(FallbackBodyMesh, FallbackBodyMID, SanitizeTint(BodyColor));
		ApplyColorToMesh(FallbackHeadMesh, FallbackHeadMID, SanitizeTint(HeadColor));
	}

	// The gun's light channels wear the team colour too. It is the only part of your own kit you can
	// see in first person, so it is the only thing that answers "which side am I on" without opening
	// the scoreboard — and it means a spectator switching between players sees the change instantly.
	// Team::None replicates as grey-cyan, which is also the correct read: the team is not known yet.
	if (ViewModelNeonMID != nullptr)
	{
		const FLinearColor NeonColor = (GetTeam() == ETraceTeam::None)
			? FLinearColor(0.55f, 0.88f, 1.f)
			: TeamColor;
		ViewModelNeonMID->SetVectorParameterValue(TEXT("Color"), NeonColor);
		ViewModelNeonMID->SetScalarParameterValue(TEXT("Glow"), 2.4f);
		// Fallback path (BasicShapeMaterial, no Glow input): the best available approximation of an
		// unlit strip is a bright matte albedo. Same trade ATraceArenaBuilder::MakeNeonMID makes.
		ViewModelNeonMID->SetVectorParameterValue(TEXT("Color"), NeonColor);
		ViewModelNeonMID->SetVectorParameterValue(TEXT("BaseColor"), NeonColor);
	}

	// The skid streak is deliberately BRIGHTER than the suit and not whitened for the carrier: it is
	// a movement tell, and it has to be readable across the arena on a near-black floor.
	if (SlideSkidMesh != nullptr)
	{
		if (SlideSkidMID == nullptr)
		{
			UMaterialInterface* Parent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : BasicShapeMaterial.Get();
			if (Parent != nullptr)
			{
				SlideSkidMID = SlideSkidMesh->CreateDynamicMaterialInstance(0, Parent);
			}
		}
		if (SlideSkidMID != nullptr)
		{
			SlideSkidMID->SetVectorParameterValue(TEXT("Color"), TeamColor);
			SlideSkidMID->SetVectorParameterValue(TEXT("BaseColor"), TeamColor);
			SlideSkidMID->SetScalarParameterValue(TEXT("Glow"), TraceCharacterLayout::SkidGlow);
		}
	}
}

void ATraceCharacter::ApplyColorToSkeletalMesh(const FLinearColor& InColor, float InEmissivePower)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp == nullptr || MeshComp->GetSkeletalMeshAsset() == nullptr)
	{
		return;
	}

	const int32 NumSlots = MeshComp->GetNumMaterials();

	// Built once and then reused, for the same reason as the static-mesh MIDs below: this runs on
	// every team change, carrier change, death and poll tick, and a fresh MID per call would leave a
	// trail of garbage. CreateDynamicMaterialInstance(Index) with no parent wraps whatever material
	// is already in the slot, so Manny keeps his own textures and normal maps — this tints the
	// existing material, it does not replace it with flat colour.
	if (CharacterMIDs.Num() != NumSlots)
	{
		CharacterMIDs.Reset(NumSlots);
		for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
		{
			CharacterMIDs.Add(MeshComp->CreateDynamicMaterialInstance(SlotIndex));
		}
	}

	for (UMaterialInstanceDynamic* MID : CharacterMIDs)
	{
		if (MID == nullptr)
		{
			continue;
		}

		// "Paint Tint" and "EmissivePower" are M_Mannequin's own parameters, read out of the asset
		// rather than guessed at. Setting a parameter a material does not have is a silent no-op, so
		// the extra generic names below cost nothing and keep this working if the mesh is ever
		// swapped for one built on a different material.
		MID->SetVectorParameterValue(TraceCharacterAssets::PaintTintParam, InColor);
		MID->SetScalarParameterValue(TraceCharacterAssets::EmissivePowerParam, InEmissivePower);

		MID->SetVectorParameterValue(TEXT("Color"), InColor);
		MID->SetVectorParameterValue(TEXT("BaseColor"), InColor);
		MID->SetVectorParameterValue(TEXT("Tint"), InColor);
		MID->SetVectorParameterValue(TEXT("EmissiveColor"), InColor);
	}
}

void ATraceCharacter::ApplyColorToMesh(UStaticMeshComponent* InMesh, TObjectPtr<UMaterialInstanceDynamic>& InOutMID, const FLinearColor& InColor)
{
	if (InMesh == nullptr || BasicShapeMaterial == nullptr)
	{
		return;
	}

	// Created once and then reused. This function runs on every team change, every carrier change,
	// every death and on a poll timer — allocating a fresh material instance each time would leave a
	// trail of them for the GC.
	if (InOutMID == nullptr)
	{
		InOutMID = InMesh->CreateDynamicMaterialInstance(0, BasicShapeMaterial);
	}

	if (InOutMID == nullptr)
	{
		return;
	}

	// BasicShapeMaterial's parameter is "Color"; "BaseColor" is set as well because setting a
	// parameter that does not exist is a silent no-op, and this way the code survives being pointed
	// at a differently-named material later.
	InOutMID->SetVectorParameterValue(TEXT("Color"), InColor);
	InOutMID->SetVectorParameterValue(TEXT("BaseColor"), InColor);
}

void ATraceCharacter::PollTeamColors()
{
	++TeamColorAttempts;
	ApplyTeamColors();

	// Stop as soon as the team is known, or give up. The OnRep hooks are the primary path; this
	// exists because the pawn, its PlayerState and that PlayerState's Team replicate independently
	// and no single callback is guaranteed to fire last.
	if (GetTeam() != ETraceTeam::None || TeamColorAttempts >= TraceCharacterLayout::MaxTeamColorAttempts)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(TeamColorTimerHandle);
		}
	}
}

void ATraceCharacter::OnRep_IsCarrier()
{
	ApplyTeamColors();

	// The remote half of the [Carry] line in SetCarrying(). On a client this is the only proof the
	// new value actually arrived — and a replicated bool re-set to the value it already holds fires
	// no OnRep at all, so a missing line here next to a present one there IS the bug report.
	if (!HasAuthority())
	{
		UE_LOG(LogTraceGame, Display, TEXT("[Carry] %s bIsCarrier -> %d (replicated to client)"),
			*GetName(), bIsCarrier ? 1 : 0);
	}

	// The camera is NOT moved from here. UpdateViewBlend() reads the carrier state every frame and
	// walks toward it, which means the transition is identical whether the state arrived by
	// replication, by SetCarrying() on a listen host, or by the Core being passed away — and there
	// is no path that can leave the camera stranded in the wrong mode because a callback did not
	// fire on some machine.
}

// =================================================================================================
// View mode — first person, third person while carrying. See the file header.
// =================================================================================================

float ATraceCharacter::GetThirdPersonPivotZ() const
{
	// Read from the settings rather than from a second copy of "190" so that retuning TrailHeight
	// moves the camera with it. UTraceTrailComponent builds its wall as TrailHeight tall, centred on
	// the carrier's actor location, and rides a cap strip of clamp(Height * 0.12, 14, 42) on top of
	// it — so the highest lit surface sits half the height plus half the cap above the actor centre,
	// and TargetOffset.Z is measured from that same actor centre.
	const float TrailHeight = FMath::Max(0.f, UTraceSettings::Get().TrailHeight);
	const float TrailTopAboveCentre = TrailHeight * 0.5f + FMath::Clamp(TrailHeight * 0.12f, 14.f, 42.f) * 0.5f;

	// Max, not a plain sum: if the trail is ever made short enough to duck under, the camera goes
	// back to the framing that was chosen on its own merits instead of drifting down with it.
	return FMath::Max(
		TraceCharacterLayout::ThirdPersonPivotZ,
		TrailTopAboveCentre + TraceCharacterLayout::TrailCameraClearance);
}

void ATraceCharacter::UpdateViewBlend(float DeltaSeconds, bool bSnap)
{
	// Pure presentation: a dedicated server has no camera, renders nothing, and must not spend a
	// frame of anyone's time on this.
	if (SpringArm == nullptr || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// This pawn's camera is only ever looked through by the machine that controls it. On every other
	// machine the arm is inert scenery — but the owner-visibility flag is NOT inert, so if this pawn
	// ever was ours (a listen host respawning, a controller changing hands) the body has to be put
	// back before we stop maintaining it.
	if (!IsLocalPlayerPawn(*this))
	{
		SetOwnBodyHiddenFromOwner(false);
		SetViewModelVisible(false);
		return;
	}

	// The first frame this pawn turns out to be the one a human is inside is the first frame it is
	// worth building a gun for. See EnsureViewModelBuilt() for why this is lazy.
	EnsureViewModelBuilt();

	const float Target = WantsFirstPersonView() ? 0.f : 1.f;

	if (bSnap || TraceCharacterLayout::ViewBlendSeconds <= 0.f)
	{
		ViewBlendAlpha = Target;
	}
	else
	{
		// Constant rate, so the pull-back always takes exactly ViewBlendSeconds however far through a
		// previous blend it was interrupted — a player who grabs the Core, passes it and catches it
		// again inside 0.7 s gets a continuous camera, not a stutter.
		ViewBlendAlpha = FMath::FInterpConstantTo(
			ViewBlendAlpha, Target, DeltaSeconds, 1.f / TraceCharacterLayout::ViewBlendSeconds);
	}

	// Smoothstep. The easing is the difference between "the camera was yanked" and "the camera
	// pulled back": it leaves and arrives at zero velocity, so neither end of the move reads as a cut.
	const float Eased = ViewBlendAlpha * ViewBlendAlpha * (3.f - 2.f * ViewBlendAlpha);

	// TargetArmLength 0 puts the camera exactly on the arm origin, and the arm origin is
	// TargetOffset above the capsule centre — which at Eased 0 is precisely GetPawnViewLocation(),
	// the point the aim ray is built from. That equality is the first-person aim guarantee; do not
	// introduce a lateral SocketOffset here without re-reading GetMuzzleLocation().
	SpringArm->TargetArmLength = FMath::Lerp(
		TraceCharacterLayout::FirstPersonArmLength, TraceCharacterLayout::ThirdPersonArmLength, Eased);

	// BaseEyeHeight, not the EyeHeight constant, and this is load-bearing now that sliding exists.
	// GetPawnViewLocation() is actor + BaseEyeHeight and is what GetAimDirection() and
	// GetMuzzleLocation() build the shot from; ACharacter drops BaseEyeHeight to CrouchedEyeHeight
	// the moment a slide starts. Pinning the arm to the standing constant instead would leave the
	// camera 60 uu above the point the bullet leaves from for the whole slide — the crosshair would
	// lie exactly when the player is moving fastest. Reading the live value keeps eyeErr at 0.00 uu
	// in first person standing AND crouched, which is what Trace.DebugViewProbe measures.
	SpringArm->TargetOffset = FVector(0.f, 0.f, FMath::Lerp(
		BaseEyeHeight, GetThirdPersonPivotZ(), Eased));

	// Our own body follows the camera out of the way. Other players' pawns are untouched by this —
	// SetOwnerNoSee hides a mesh from ONE viewer, the one whose view target owns it.
	const bool bFirstPersonNow = (Eased < TraceCharacterLayout::OwnBodyHideAlpha);
	SetOwnBodyHiddenFromOwner(bFirstPersonNow);

	// The gun follows the same switch as the body, so the two can never disagree: the moment the
	// camera pulls back to show the carrier, the viewmodel goes away with it. A corpse holds no gun
	// either — the third condition is what stops a dead player staring at a floating pistol while
	// the respawn timer runs.
	SetViewModelVisible(bFirstPersonNow && !bDeadPresentation && IsAlive());
}

// =================================================================================================
// First-person viewmodel — see the file header for the framing and depth arithmetic.
// =================================================================================================

UStaticMeshComponent* ATraceCharacter::AddViewModelPart(UStaticMesh* InMesh, const TCHAR* DebugName,
	const FVector& Location, const FRotator& Rotation, const FVector& Size, UMaterialInstanceDynamic* MID)
{
	if (InMesh == nullptr || ViewModelRoot == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), FName(DebugName)));
	if (Part == nullptr)
	{
		return nullptr;
	}

	Part->SetMobility(EComponentMobility::Movable);
	Part->SetupAttachment(ViewModelRoot);
	Part->SetStaticMesh(InMesh);
	Part->SetRelativeLocationAndRotation(Location, Rotation);
	Part->SetRelativeScale3D(Size / TraceCharacterLayout::ViewModelShapeUnit);

	// Contract section 7 again, and it matters more here than anywhere: the capsule is the ONLY
	// collider on this actor. A viewmodel is 40 uu from the eye — a colliding one would be a
	// permanent obstacle welded to the player's face.
	Part->SetCollisionProfileName(TEXT("NoCollision"));
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCanEverAffectNavigation(false);
	Part->bReceivesDecals = false;

	// NOBODY ELSE MAY EVER SEE THIS. bOnlyOwnerSee restricts it to the machine whose view target
	// owns it, and no shadow of any kind is cast, so there is no path by which a floating gun
	// appears in anyone else's frame — not even as a silhouette on the floor.
	Part->SetOnlyOwnerSee(true);
	Part->SetCastShadow(false);
	Part->bCastHiddenShadow = false;

	// The whole point of the rig. Set before RegisterComponent so the scene proxy is created with it
	// rather than having to be rebuilt; see the depth arithmetic in the file header.
	Part->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;

	if (MID != nullptr)
	{
		Part->SetMaterial(0, MID);
	}

	Part->RegisterComponent();
	ViewModelParts.Add(Part);
	return Part;
}

void ATraceCharacter::EnsureViewModelBuilt()
{
	if (bViewModelBuilt || ViewModelRoot == nullptr || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	bViewModelBuilt = true;

	if (CubeMesh == nullptr || CylinderMesh == nullptr)
	{
		// /Engine/BasicShapes ships with every install, so this is close to impossible — but an
		// invisible gun is a far better failure than a crash, and it is the same contract every
		// other optional asset in this file honours.
		UE_LOG(LogTraceGame, Warning,
			TEXT("First-person viewmodel skipped: /Engine/BasicShapes did not resolve."));
		return;
	}

	// --- Materials -------------------------------------------------------------------------------
	//
	// The gun is made of the same two materials as the arena, which is what makes it look like it
	// belongs in the world rather than like a prop dropped into it.
	//
	// The body albedo (0.06) is four times the arena's structure albedo, and deliberately: the three
	// directional lights in this arena total under 6 lux and none of them is aimed at the inside of a
	// player's face, so a gun at the arena's own 0.015 would be a black hole in the middle of the
	// frame. The emissive term does the rest — it is the only lighting term that does not depend on
	// an angle of incidence, which is exactly the argument the arena's own cover blocks make.
	{
		UMaterialInterface* BodyParent = (SurfaceMaterial != nullptr) ? SurfaceMaterial.Get() : BasicShapeMaterial.Get();
		if (BodyParent != nullptr)
		{
			ViewModelBodyMID = UMaterialInstanceDynamic::Create(BodyParent, this);
			if (ViewModelBodyMID != nullptr)
			{
				// MEASURED. The first pass ran albedo 0.055 at metallic 0.35, and against a black
				// arena that produced two flat mid-blue slabs where the hands should be: bright
				// enough to be the largest object in the frame, dark enough to have no detail in it,
				// and glossy enough to pick the floor's cyan up over the whole surface. Dropping the
				// albedo and most of the metallic hands the shape reading back to the light channels,
				// which is the same argument the arena's own cover blocks make - the silhouette is
				// drawn in neon, and the body is what the neon is drawn ON.
				const FLinearColor BodyColor(0.028f, 0.032f, 0.042f);
				ViewModelBodyMID->SetVectorParameterValue(TEXT("BaseColor"), BodyColor);
				ViewModelBodyMID->SetVectorParameterValue(TEXT("Color"), BodyColor);
				ViewModelBodyMID->SetScalarParameterValue(TEXT("Roughness"), 0.52f);
				ViewModelBodyMID->SetScalarParameterValue(TEXT("Metallic"), 0.12f);
				ViewModelBodyMID->SetVectorParameterValue(TEXT("Emissive"), FLinearColor(0.30f, 0.55f, 0.80f));
				ViewModelBodyMID->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.030f);
			}
		}

		UMaterialInterface* NeonParent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : BasicShapeMaterial.Get();
		if (NeonParent != nullptr)
		{
			ViewModelNeonMID = UMaterialInstanceDynamic::Create(NeonParent, this);
		}
	}

	// --- Geometry --------------------------------------------------------------------------------
	//
	// Rig space: +X out of the lens, +Y right, +Z up, origin at the top-rear of the grip. Every
	// number here was placed against the framing arithmetic in the file header, so the gun sits in
	// the lower right with its highest point a quarter of a frame below the crosshair.
	struct FViewModelPart
	{
		const TCHAR* Name;
		bool bCylinder;
		FVector Location;
		FRotator Rotation;
		FVector Size;
		bool bNeon;
	};

	const FViewModelPart Parts[] =
	{
		// The gun. A slide over a frame over a raked grip: three masses, which is what makes a
		// blocky shape read as a handgun rather than as a brick.
		{ TEXT("VMSlide"),      false, FVector(9.0f, 0.f, 2.4f),    FRotator::ZeroRotator,        FVector(21.0f, 4.6f, 5.2f),  false },
		{ TEXT("VMFrame"),      false, FVector(7.0f, 0.f, -1.6f),   FRotator::ZeroRotator,        FVector(16.5f, 4.2f, 4.4f),  false },
		{ TEXT("VMGrip"),       false, FVector(-1.6f, 0.f, -8.2f),  FRotator(14.f, 0.f, 0.f),     FVector(5.6f, 4.0f, 13.5f),  false },
		{ TEXT("VMGuard"),      false, FVector(3.2f, 0.f, -4.6f),   FRotator::ZeroRotator,        FVector(5.6f, 3.0f, 1.4f),   false },

		// Light channels. The muzzle ring is a cylinder turned to point down the barrel (pitch 90
		// swings the shape's own +Z axis onto +X), and it is the piece that makes the gun read as a
		// weapon at a glance: a lit circle where the shot comes out.
		{ TEXT("VMMuzzle"),     true,  FVector(19.6f, 0.f, 2.4f),   FRotator(90.f, 0.f, 0.f),     FVector(5.8f, 5.8f, 2.2f),   true  },
		{ TEXT("VMSlideNeon"),  false, FVector(8.4f, 0.f, 5.3f),    FRotator::ZeroRotator,        FVector(16.5f, 1.8f, 1.5f),  true  },
		{ TEXT("VMSight"),      false, FVector(17.6f, 0.f, 5.6f),   FRotator::ZeroRotator,        FVector(1.4f, 1.4f, 2.0f),   true  },
		{ TEXT("VMSideNeonL"),  false, FVector(7.6f, -2.4f, 0.4f),  FRotator::ZeroRotator,        FVector(12.5f, 0.9f, 1.6f),  true  },
		{ TEXT("VMSideNeonR"),  false, FVector(7.6f, 2.4f, 0.4f),   FRotator::ZeroRotator,        FVector(12.5f, 0.9f, 1.6f),  true  },
		{ TEXT("VMGripNeon"),   false, FVector(-3.9f, 0.f, -8.0f),  FRotator(14.f, 0.f, 0.f),     FVector(1.2f, 3.0f, 9.5f),   true  },
		{ TEXT("VMRearSight"),  false, FVector(1.4f, 0.f, 5.6f),    FRotator::ZeroRotator,        FVector(1.6f, 3.6f, 2.0f),   true  },
		{ TEXT("VMRailNeon"),   false, FVector(6.5f, 0.f, -3.9f),   FRotator::ZeroRotator,        FVector(13.0f, 1.6f, 1.0f),  true  },

		// Hands. Blocks, not fingers: at this scale and this framing a gloved fist is a shape, and
		// trying to model knuckles on a 6 uu cube only produces noise. What DOES read is a lit bar
		// across each one — a knuckle line, in the same language as everything else in this world.
		{ TEXT("VMHandR"),      false, FVector(-0.8f, 0.f, -4.6f),  FRotator(14.f, 0.f, 0.f),     FVector(5.6f, 5.4f, 7.0f),   false },
		{ TEXT("VMKnuckleR"),   false, FVector(1.4f, 0.f, -3.2f),   FRotator(14.f, 0.f, 0.f),     FVector(1.2f, 5.0f, 4.6f),   true  },
		{ TEXT("VMHandL"),      false, FVector(2.8f, -3.4f, -4.0f), FRotator::ZeroRotator,        FVector(5.0f, 4.8f, 5.6f),   false },
		{ TEXT("VMKnuckleL"),   false, FVector(4.9f, -3.4f, -3.0f), FRotator::ZeroRotator,        FVector(1.1f, 4.4f, 3.8f),   true  }
	};

	for (const FViewModelPart& Part : Parts)
	{
		AddViewModelPart(Part.bCylinder ? CylinderMesh : CubeMesh, Part.Name,
			Part.Location, Part.Rotation, Part.Size,
			Part.bNeon ? ViewModelNeonMID : ViewModelBodyMID);
	}

	// --- Forearms --------------------------------------------------------------------------------
	//
	// Two cylinders running from the hands down and back out of frame, each with a neon cuff. They
	// are short (17-18 uu) for the depth reason in the file header: the far end must stay in front of
	// the near plane once FirstPersonScale has halved its depth. That is not a compromise — a real
	// viewmodel's arms leave the bottom of the frame within a few centimetres of the hands too.
	struct FForearmSpec
	{
		const TCHAR* Name;
		const TCHAR* CuffName;
		FVector Hand;
		FVector Direction;   // normalised in place; points away from the gun, down and outward
		float Length;
		float Diameter;
	};

	const FForearmSpec Forearms[] =
	{
		{ TEXT("VMForearmR"), TEXT("VMCuffR"), FVector(-0.8f, 0.f, -4.6f),  FVector(-0.42f, 0.36f, -0.86f),  17.f, 7.0f },
		{ TEXT("VMForearmL"), TEXT("VMCuffL"), FVector(2.8f, -3.4f, -4.0f), FVector(-0.40f, -0.38f, -0.86f), 16.f, 6.7f }
	};

	for (const FForearmSpec& Arm : Forearms)
	{
		const FVector Dir = Arm.Direction.GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			continue;
		}

		// MakeFromZ because the basic cylinder's axis is its own +Z; this turns that axis onto the
		// arm direction without having to hand-derive a rotator.
		const FRotator ArmRotation = FRotationMatrix::MakeFromZ(Dir).Rotator();
		const FVector ArmCentre = Arm.Hand + Dir * (2.f + Arm.Length * 0.5f);

		AddViewModelPart(CylinderMesh, Arm.Name, ArmCentre, ArmRotation,
			FVector(Arm.Diameter, Arm.Diameter, Arm.Length), ViewModelBodyMID);

		AddViewModelPart(CylinderMesh, Arm.CuffName, Arm.Hand + Dir * 5.f, ArmRotation,
			FVector(Arm.Diameter + 0.8f, Arm.Diameter + 0.8f, 1.8f), ViewModelNeonMID);
	}

	// Hidden until UpdateViewBlend says first person; ApplyTeamColors paints the light channels.
	for (UStaticMeshComponent* Part : ViewModelParts)
	{
		if (Part != nullptr)
		{
			Part->SetVisibility(false);
		}
	}
	bViewModelVisible = false;

	ApplyTeamColors();

	UE_LOG(LogTraceGame, Verbose, TEXT("%s built a first-person viewmodel (%d parts)."),
		*GetName(), ViewModelParts.Num());
}

void ATraceCharacter::SetViewModelVisible(bool bVisible)
{
	if (bViewModelVisible == bVisible)
	{
		return;
	}
	bViewModelVisible = bVisible;

	for (UStaticMeshComponent* Part : ViewModelParts)
	{
		if (Part != nullptr)
		{
			Part->SetVisibility(bVisible);
		}
	}
}

void ATraceCharacter::UpdateViewModel(float DeltaSeconds)
{
	if (ViewModelRoot == nullptr)
	{
		return;
	}

	// A hidden rig still gets its state DECAYED rather than frozen, so a player who takes the Core
	// mid-burst and hands it back does not come out of third person with a stale recoil kick and a
	// bob phase from four seconds ago. It just does not get a transform written.
	const bool bAnimate = bViewModelVisible && DeltaSeconds > 0.f;

	// --- Sway ------------------------------------------------------------------------------------
	//
	// The rig lags a fast turn by a fraction of a degree and springs back. This is the single
	// cheapest thing that makes a viewmodel feel like an object being carried rather than a decal
	// stuck to the lens — and it is free of consequence, because nothing in the shot path reads the
	// rig transform. GetAimDirection() is still built from the control rotation alone.
	const FRotator ControlRotation = GetControlRotation();
	if (bViewModelHasLastRotation && DeltaSeconds > 0.f)
	{
		const FRotator Delta = (ControlRotation - ViewModelLastControlRotation).GetNormalized();

		ViewModelSwayYaw = FMath::Clamp(
			ViewModelSwayYaw - static_cast<float>(Delta.Yaw) * TraceCharacterLayout::SwayPerDegree,
			-TraceCharacterLayout::SwayMaxDegrees, TraceCharacterLayout::SwayMaxDegrees);
		ViewModelSwayPitch = FMath::Clamp(
			ViewModelSwayPitch - static_cast<float>(Delta.Pitch) * TraceCharacterLayout::SwayPerDegree,
			-TraceCharacterLayout::SwayMaxDegrees, TraceCharacterLayout::SwayMaxDegrees);
	}
	ViewModelLastControlRotation = ControlRotation;
	bViewModelHasLastRotation = true;

	ViewModelSwayYaw = FMath::FInterpTo(ViewModelSwayYaw, 0.f, DeltaSeconds, TraceCharacterLayout::SwayRecoverSpeed);
	ViewModelSwayPitch = FMath::FInterpTo(ViewModelSwayPitch, 0.f, DeltaSeconds, TraceCharacterLayout::SwayRecoverSpeed);

	// --- Walk bob --------------------------------------------------------------------------------

	const UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bGrounded = (Movement != nullptr) && Movement->IsMovingOnGround();
	const float PlanarSpeed = GetVelocity().Size2D();
	const float MaxSpeed = (Movement != nullptr && Movement->MaxWalkSpeed > 1.f) ? Movement->MaxWalkSpeed : 720.f;
	const float BobTarget = bGrounded ? FMath::Clamp(PlanarSpeed / MaxSpeed, 0.f, 1.f) : 0.f;

	ViewModelBobStrength = FMath::FInterpTo(ViewModelBobStrength, BobTarget, DeltaSeconds, TraceCharacterLayout::BobInterpSpeed);
	ViewModelBobPhase = FMath::Fmod(
		ViewModelBobPhase + DeltaSeconds * TraceCharacterLayout::BobBaseRate * (0.6f + ViewModelBobStrength),
		2.f * PI);

	// --- Recoil and crouch dip -------------------------------------------------------------------

	ViewModelKick = FMath::FInterpTo(ViewModelKick, 0.f, DeltaSeconds, TraceCharacterLayout::KickRecoverSpeed);

	// The gun drops a little further than the eye does during a slide — the hands come down into the
	// lap. CrouchLeanAlpha is the already-eased slide state UpdateCrouchPresentation computed this
	// frame, so the two cannot disagree about when a slide started.
	ViewModelCrouchDip = FMath::FInterpTo(ViewModelCrouchDip, CrouchLeanAlpha, DeltaSeconds,
		TraceCharacterLayout::CrouchLeanInterpSpeed);

	if (!bAnimate)
	{
		return;
	}

	// Vertical bob runs at twice the lateral rate — one dip per footfall, one sway per stride, which
	// is what a gait actually does.
	FVector Offset(
		-ViewModelKick * TraceCharacterLayout::KickBackUU,
		FMath::Sin(ViewModelBobPhase) * TraceCharacterLayout::BobLateralUU * ViewModelBobStrength,
		FMath::Sin(ViewModelBobPhase * 2.f) * TraceCharacterLayout::BobVerticalUU * ViewModelBobStrength
			- ViewModelCrouchDip * TraceCharacterLayout::CrouchDipUU);

	const FRotator Rotation = TraceCharacterLayout::ViewModelRestRotation
		+ FRotator(ViewModelSwayPitch + ViewModelKick * TraceCharacterLayout::KickPitchDegrees,
			ViewModelSwayYaw, 0.f);

	ViewModelRoot->SetRelativeLocationAndRotation(TraceCharacterLayout::ViewModelRestLocation + Offset, Rotation);
}

void ATraceCharacter::NotifyWeaponFired()
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	if ((Now - LastFireKickTime) < TraceCharacterLayout::FireKickRefractorySeconds)
	{
		return;
	}
	LastFireKickTime = Now;

	ViewModelKick = 1.f;
}

// =================================================================================================
// Crouch / slide
// =================================================================================================

void ATraceCharacter::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	// BEFORE Super — see the note on the declaration. ACharacter::OnStartCrouch is what calls
	// RecalculateBaseEyeHeight(), and that is what copies CrouchedEyeHeight into BaseEyeHeight.
	if (const UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		const float NewHalfHeight = Capsule->GetScaledCapsuleHalfHeight();

		// Clamped INSIDE the capsule (minus a margin) rather than left free: BaseEyeHeight is where
		// the first-person camera and the aim ray both live, and an eye above the top of a crouched
		// capsule would let a slide see over cover the body is genuinely behind.
		const float MaxEye = FMath::Max(4.f, NewHalfHeight - TraceCharacterLayout::CrouchedEyeCapsuleMargin);
		CrouchedEyeHeight = FMath::Clamp(TraceCharacterLayout::CrouchedEyeAboveFeet - NewHalfHeight, 4.f, MaxEye);
	}

	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
}

void ATraceCharacter::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
}

void ATraceCharacter::UpdateCrouchPresentation(float DeltaSeconds)
{
	// WHAT "CROUCHED" MEANS IN THIS GAME, and why this does not read bIsCrouched.
	//
	// UTraceCharacterMovementComponent overrides CanCrouchInCurrentState() to ALWAYS RETURN FALSE,
	// on purpose and with a good reason: the capsule is this project's single source of truth for
	// hitscan, for the pose history the server rewinds and for the trail trip test, and a slide that
	// silently halved a pawn's hit height would change all three on the server only, in the middle
	// of the one mechanic the game is about. The crouch key therefore never resizes anything and
	// ACharacter::bIsCrouched is never set — it is consumed as an INPUT and turned into a slide.
	//
	// So the state to present is IsSliding(), not bIsCrouched. Both are checked anyway, so this
	// keeps working if the capsule ever does start shrinking.
	const UTraceCharacterMovementComponent* TraceMovement = GetTraceMovement();
	const bool bLocallySliding = (TraceMovement != nullptr && TraceMovement->IsSliding());

	// The authority is the one machine that knows the truth for every pawn — its own, its bots' and
	// every remote client's, all of which it simulates from the same saved moves. Publishing it here
	// (rather than from the movement component) keeps the write next to the single reader.
	if (HasAuthority())
	{
		bReplicatedSliding = bLocallySliding;
	}

	// bReplicatedSliding is what carries the slide to a THIRD machine: a simulated proxy runs no
	// saved moves, so without it a bystander's copy of a sliding player stands bolt upright and, far
	// worse, lays its hit zones out at standing height (see the property's comment). ORed, never
	// substituted, so the owner's own prediction always wins on the frame it starts.
	const bool bSliding = bLocallySliding || bReplicatedSliding || bIsCrouched;

	// --- The eye ---------------------------------------------------------------------------------
	//
	// The capsule stays put; the EYE does not have to, and it is the half of the crouch the player
	// actually experiences. Driving BaseEyeHeight (rather than dropping the camera on its own) is
	// what keeps the aim honest: GetPawnViewLocation() is actor + BaseEyeHeight, UpdateViewBlend
	// pins the spring arm to the same value, so the camera, the muzzle and the bullet all descend
	// together and Trace.DebugViewProbe still reads eyeErr 0.00 mid-slide. Dropping only the camera
	// would have put the crosshair 34 uu above the shot for the whole slide.
	//
	// This half runs on EVERY machine including a dedicated server, because the server evaluates
	// GetPawnViewLocation() when it resolves a shot; the visual half below does not.
	const float StandingEye = bIsCrouched ? CrouchedEyeHeight : TraceCharacterLayout::EyeHeight;
	const float TargetEye = bSliding
		? FMath::Min(StandingEye, TraceCharacterLayout::SlideEyeHeight)
		: StandingEye;
	BaseEyeHeight = FMath::FInterpTo(BaseEyeHeight, TargetEye, DeltaSeconds,
		TraceCharacterLayout::SlideEyeInterpSpeed);

	if (GetNetMode() == NM_DedicatedServer)
	{
		return;   // nothing is drawn there
	}

	// --- THE SLIDE POSE (spec v4 §1) — PROCEDURAL, BECAUSE THERE IS NO STOCK SLIDE ANIMATION ------
	//
	// The Demo 4 notes ask: "If unreal has a default slide animation for the mannequins, please add
	// that in." IT DOES NOT. The Mannequin set Scripts/import-mannequin.sh brings in is Death, Jump,
	// Pistol, Rifle and Unarmed locomotion — BS_Idle_Walk_Run and MM_Idle — and there is no slide and
	// no crouch anywhere in Templates/TemplateResources or Engine/Content. Nothing below is a shipped
	// Epic animation; it is an approximation assembled from the skeleton the project already has, and
	// a real slide would still have to be authored or bought.
	//
	// Four things together sell it, and the fourth matters most:
	//   RECLINE   the body tips BACK over its own feet (SlidePoseLeanDegrees), feet leading, which is
	//             the Apex/Titanfall silhouette.
	//   DROP      the whole mesh sinks toward the deck (SlidePoseDropUU). The capsule deliberately
	//             does NOT shrink — it is the single source of truth for hitscan, for the pose history
	//             the server rewinds and for the trail trip test — so this is the only thing that
	//             makes a sliding player look low from the outside.
	//   ROLL      a few degrees of lead shoulder (SlidePoseRollDegrees) so the pose is a body
	//             committed to a direction rather than a mannequin tipped back on a hinge.
	//   ANIM RATE the locomotion blend space is slowed almost to a stop (SlidePoseAnimRateScale).
	//             Without this the legs sprint at full cadence underneath the reclined torso, which is
	//             the one thing that unambiguously reads as a bug rather than as a move; near zero
	//             they freeze mid-stride, which is close enough to "extended into a slide" to sell.
	//
	// All of it is driven off the same eased CrouchLeanAlpha, which is derived from the slide state —
	// client-predicted and server-simulated from the same saved moves — so a bystander's copy reaches
	// this pose on its own, with no RPC and no presentation flag, and it cannot disagree with the
	// movement that caused it. And all of it is visual: nothing here feeds the simulation or moves a
	// hit zone.
	const UTraceSettings& PoseSettings = UTraceSettings::Get();

	const float LeanTarget = bSliding ? 1.f : 0.f;
	const float PreviousLean = CrouchLeanAlpha;
	CrouchLeanAlpha = FMath::FInterpTo(CrouchLeanAlpha, LeanTarget, DeltaSeconds,
		FMath::Max(1.f, PoseSettings.SlidePoseBlendSpeed));

	// Composed as Pose * Yaw, not as a rotator sum: the mesh already carries MeshYaw (-90) to point
	// its +Y forward down the actor's +X, and adding a pitch to a rotator that is already yawed 90
	// degrees rolls the character sideways instead of tipping it back. Quaternion order in Unreal is
	// "apply the right one first", so this yaws the mesh into place and THEN pitches and rolls it
	// about the ACTOR's own Y and X axes, which are the axes a body leans and lists about.
	//
	// The pivot is the mesh origin, which sits at the bottom of the capsule (MeshOffsetZ), i.e. at the
	// feet — so the body tips over its feet rather than swinging about its waist, and the head travels
	// while the feet stay planted on the skid streak.
	if (FMath::Abs(CrouchLeanAlpha - PreviousLean) > 0.001f)
	{
		if (USkeletalMeshComponent* MeshComp = GetMesh())
		{
			const float LeanDegrees = FMath::Clamp(PoseSettings.SlidePoseLeanDegrees, 0.f, 70.f);
			const float RollDegrees = FMath::Clamp(PoseSettings.SlidePoseRollDegrees, -40.f, 40.f);
			const float DropUU      = FMath::Clamp(PoseSettings.SlidePoseDropUU, 0.f, 80.f);

			const FQuat YawQuat(FRotator(0.f, TraceCharacterLayout::MeshYaw, 0.f));
			const FQuat PoseQuat(FRotator(
				CrouchLeanAlpha * LeanDegrees,   // pitch: positive tips the body BACK, feet leading
				0.f,
				CrouchLeanAlpha * RollDegrees)); // roll: the lead shoulder
			MeshComp->SetRelativeRotation(PoseQuat * YawQuat);

			// Straight down from the standing offset. Kept modest and clamped because the feet sit at
			// the mesh origin: drop far enough and they go through the deck. The recline is already
			// bringing the head down by roughly (1 - cos(lean)) of the body height, so this only has to
			// finish the job.
			MeshComp->SetRelativeLocation(FVector(
				0.f, 0.f, TraceCharacterLayout::MeshOffsetZ - CrouchLeanAlpha * DropUU));

			// THE ONE THAT STOPS IT LOOKING LIKE A BUG. Lerped rather than switched so the legs wind
			// down into the slide and spin back up out of it, instead of snapping.
			MeshComp->GlobalAnimRateScale = FMath::Lerp(
				1.f, FMath::Clamp(PoseSettings.SlidePoseAnimRateScale, 0.f, 1.f), CrouchLeanAlpha);
		}
	}

	// --- The skid streak -------------------------------------------------------------------------
	//
	// Only while crouched, on the ground AND actually moving: a stationary crouch is a crouch, and a
	// streak under a player who is standing still would be nonsense. Scaled by speed so the streak
	// grows out of nothing as a slide starts and dies as it runs out, which is the whole read.
	if (SlideSkidMesh == nullptr)
	{
		return;
	}

	const UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bGrounded = (Movement != nullptr) && Movement->IsMovingOnGround();
	const float PlanarSpeed = GetVelocity().Size2D();
	const float SkidTarget = (bSliding && bGrounded && IsAlive())
		? FMath::Clamp(PlanarSpeed / TraceCharacterLayout::SkidFullSpeed, 0.f, 1.f)
		: 0.f;

	SkidGlowAlpha = FMath::FInterpTo(SkidGlowAlpha, SkidTarget, DeltaSeconds, 8.f);

	const bool bShowSkid = SkidGlowAlpha > 0.05f;
	if (SlideSkidMesh->IsVisible() != bShowSkid)
	{
		SlideSkidMesh->SetVisibility(bShowSkid);
	}
	if (!bShowSkid)
	{
		return;
	}

	// Sits under the crouched capsule, not the standing one: the capsule really did shrink, so the
	// streak has to come up with it or it would be drawn inside the floor.
	const float FeetZ = (GetCapsuleComponent() != nullptr)
		? -GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.f
		: -TraceCharacterLayout::CapsuleHalfHeight + 3.f;

	// Trailing BEHIND the pawn, which is where a skid mark belongs. Local +X is the actor's forward
	// and the actor faces its movement while sliding.
	SlideSkidMesh->SetRelativeLocation(
		FVector(-TraceCharacterLayout::SkidLength * 0.5f * SkidGlowAlpha, 0.f, FeetZ));
	SlideSkidMesh->SetRelativeScale3D(FVector(
		TraceCharacterLayout::SkidLength * SkidGlowAlpha / TraceCharacterLayout::ViewModelShapeUnit,
		TraceCharacterLayout::SkidWidth / TraceCharacterLayout::ViewModelShapeUnit,
		TraceCharacterLayout::SkidThickness / TraceCharacterLayout::ViewModelShapeUnit));
}

void ATraceCharacter::ApplyRotationMode()
{
	// Only a human in first person turns their body with their aim.
	//
	// A bot is never looked out of, and ABP_Unarmed drives a SPEED-only blend space: a pawn whose
	// body faces its aim while it strafes plays a forward run sideways. Nobody sees that on their own
	// character (it is hidden in first person, and a carrier faces its movement), but everyone sees
	// it on all nine other characters — so bots stay on orient-to-movement in both modes. This is a
	// deliberate asymmetry, not an oversight.
	const bool bHumanControlled = (Cast<APlayerController>(GetController()) != nullptr);
	const bool bFirstPersonRotation = bHumanControlled && WantsFirstPersonView();

	if (bRotationModeApplied && bRotationModeIsFirstPerson == bFirstPersonRotation)
	{
		return;
	}
	bRotationModeApplied = true;
	bRotationModeIsFirstPerson = bFirstPersonRotation;

	// These two are exclusive by construction: leaving both on makes the movement component and the
	// controller fight over the same yaw every frame.
	bUseControllerRotationYaw = bFirstPersonRotation;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->bOrientRotationToMovement = !bFirstPersonRotation;
	}
}

void ATraceCharacter::SetOwnBodyHiddenFromOwner(bool bInHidden)
{
	// Guarded because SetOwnerNoSee marks the render state dirty; the blend calls this every frame.
	if (bOwnBodyHiddenFromOwner == bInHidden)
	{
		return;
	}
	bOwnBodyHiddenFromOwner = bInHidden;

	// All three visual components, because which of them is showing depends on whether the mannequin
	// import has been run — a fresh clone in first person must not be staring at the inside of a
	// fallback cylinder.
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetOwnerNoSee(bInHidden);

		// Every other pawn keeps OnlyTickPoseWhenRendered, which is the big animation saving in a
		// 5v5. This one pawn cannot: in first person it is deliberately not drawn for the only
		// viewer there is, so "when rendered" would mean "never" — its shadow (bCastHiddenShadow,
		// set in the constructor) would freeze mid-stride, and the pose would still be stale on the
		// frame the Core is picked up and the body swings back into view. One always-ticked
		// skeleton out of ten is a price worth paying for both of those.
		MeshComp->VisibilityBasedAnimTickOption = bInHidden
			? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
			: EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	}
	if (FallbackBodyMesh != nullptr)
	{
		FallbackBodyMesh->SetOwnerNoSee(bInHidden);
	}
	if (FallbackHeadMesh != nullptr)
	{
		FallbackHeadMesh->SetOwnerNoSee(bInHidden);
	}
}

// =================================================================================================
// Input entry points
// =================================================================================================

void ATraceCharacter::DoMove(const FVector2D& Value)
{
	if (Controller == nullptr || Value.IsNearlyZero())
	{
		return;
	}

	// Movement is relative to where the camera looks, never to where the capsule faces: the capsule
	// is busy turning to follow the movement (bOrientRotationToMovement), so feeding its own
	// rotation back in as the input basis would make it spiral.
	const FRotationMatrix YawBasis(FRotator(0.f, GetControlRotation().Yaw, 0.f));

	AddMovementInput(YawBasis.GetUnitAxis(EAxis::X), Value.Y);   // W/S
	AddMovementInput(YawBasis.GetUnitAxis(EAxis::Y), Value.X);   // A/D
}

void ATraceCharacter::DoLook(const FVector2D& Value)
{
	// Both signs are already correct on arrival: the Look mapping's Y scalar carries BOTH the
	// vertical sensitivity and the invert-Y sign (Settings/TraceUserSettings). Nothing else in the
	// chain flips a sign — with bEnableLegacyInputScales=False, AddControllerPitchInput's multiplier
	// is +1 and raw MouseY is already positive when the mouse moves up. Do not add a negate here.
	AddControllerYawInput(Value.X);
	AddControllerPitchInput(Value.Y);
}

void ATraceCharacter::DoFirePressed()
{
	// SPEC §4. Carrying the Core overloads mouse1: it passes instead of shooting, and the gun stays
	// silent. Returning here (rather than also calling StartFire) is what guarantees the "cannot
	// shoot while carrying" rule holds even if the weapon's own gate is ever relaxed.
	if (bIsCarrier)
	{
		DoPassPressed();
		return;
	}

	// The weapon owns every remaining rule about whether the shot is legal (dead, fire rate). The
	// pawn just forwards the trigger.
	if (Weapon != nullptr)
	{
		Weapon->StartFire();
	}
}

void ATraceCharacter::DoFireReleased()
{
	// UNCONDITIONAL, both halves. The comment here used to say "the release ALWAYS propagates" and
	// then gate the pass half on bIsCarrier, which is the one state guaranteed to be wrong by the
	// time it is read: a completed pass clears bIsCarrier on this pawn BEFORE the player's finger
	// leaves the button, so the gate swallowed exactly the release it existed to deliver. See
	// DoPassReleased(), whose own comment has always said this.
	//
	// Safe to send unconditionally now that ATraceCore::RequestPassInput takes the requester and
	// decides for itself whose button it is - a non-holder's mouse1 release can no longer cancel
	// the holder's pass, which is what made the gate look necessary in the first place.
	DoPassReleased();

	if (Weapon != nullptr)
	{
		Weapon->StopFire();
	}
}

void ATraceCharacter::DoPassPressed()
{
	// No local bIsCarrier gate. ATraceCore owns "may this pawn arm the pass", it checks the Core's
	// own idea of who is holding rather than a replicated mirror of it, and it re-checks possession,
	// range, line of sight and the aim cone on the server every tick of the hold. A second copy of
	// the rule here could only ever disagree with the first.
	if (ATraceCore* TheCore = ATraceCore::Get(GetWorld()))
	{
		TheCore->RequestPassInput(true, this);
	}
}

void ATraceCharacter::DoPassReleased()
{
	// NOT gated on bIsCarrier: a completed pass clears bIsCarrier on this pawn before the player's
	// finger leaves the button, and the Core still needs to hear the release so bPassInputHeld does
	// not stay latched into the next possession. Passing `this` is what makes that safe — the Core
	// matches the release against whoever armed the latch, so this pawn can still deliver its own
	// release after losing the Core, while a non-holder's release cannot touch anybody else's pass.
	if (ATraceCore* TheCore = ATraceCore::Get(GetWorld()))
	{
		TheCore->RequestPassInput(false, this);
	}
}

float ATraceCharacter::GetPassProgress() const
{
	// Negative means "no pass in progress" — the HUD's contract, see the header. Only the holder
	// ever reports progress, so a spectator or a teammate draws nothing.
	if (!bIsCarrier)
	{
		return -1.f;
	}

	const ATraceCore* TheCore = ATraceCore::Get(GetWorld());
	if (TheCore == nullptr || TheCore->GetCarrier() != this || !TheCore->IsPassActive())
	{
		return -1.f;
	}

	return FMath::Clamp(TheCore->GetPassProgress(), 0.f, 1.f);
}

float ATraceCharacter::GetHitZonePostureScale() const
{
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (Capsule == nullptr)
	{
		return 1.f;
	}

	// Both heights are measured from the FEET, which is where the zone model lays its bands out
	// from. Standing is the constant rather than the live capsule's own default so that a pawn whose
	// capsule is ever resized still reports 1.0 when it is upright.
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const float StandingAboveFeet = HalfHeight + TraceCharacterLayout::EyeHeight;
	if (StandingAboveFeet <= KINDA_SMALL_NUMBER)
	{
		return 1.f;
	}

	const float CurrentAboveFeet = HalfHeight + BaseEyeHeight;

	// Floored well above zero: a degenerate posture would collapse the head sphere onto the feet and
	// turn a leg shot into a one-shot kill. 0.5 is far below anything the slide can produce (0.776).
	return FMath::Clamp(CurrentAboveFeet / StandingAboveFeet, 0.5f, 1.f);
}

// DELETED THIS PASS: ServerPass / PerformPass / DoPass.
//
// All three were legacy and all three had ZERO callers. The pass has been a held hover evaluated
// entirely on the server from the holder's own aim (ATraceCore::ServerTickPass) for two passes now;
// the direction a client sends is precisely the thing the server must not trust, so PerformPass's
// whole payload was unusable. DoPass() was a one-line alias for DoPassPressed().
//
// ServerPass was kept "in case an old client build calls it". There are no shipped clients, and a
// declared reliable server RPC with no callers is a surface a modified client can burn a reliable
// slot on every frame — rate-limited, so not a hole, but cost for nothing. The rate limiter
// (LastPassRequestTime / MinPassRequestInterval) went with it.
//
// The live path is DoPassPressed / DoPassReleased -> ATraceCore::RequestPassInput, which has its own
// server RPC (ATraceCore::ServerSetPassInput) that re-enters the same validation. One door.

void ATraceCharacter::DoDash()
{
	if (!IsAlive())
	{
		return;
	}

	if (UTraceCharacterMovementComponent* Movement = GetTraceMovement())
	{
		// No RPC. StartDash() raises a flag that the next saved move packs into FLAG_Custom_0, so the
		// dash is simulated locally this frame and reproduced identically by the server from the
		// ordinary ServerMove — that is what makes it predicted rather than merely responsive.
		Movement->StartDash();
	}
}

// BOOST IS GONE (spec §1: "remove boost from the game entirely"). ATraceCharacter::DoBoost() used to
// live here and forwarded to UTraceCharacterMovementComponent::StartBoost(). It was deleted with the
// rest of the feature; see the report for the two callers outside this file that must go with it
// (ATracePlayerController::OnBoostStarted and the IA_Boost binding).

// =================================================================================================
// Parry (spec §3) — input routing only
// =================================================================================================
//
// THE MECHANIC IS NOT HERE AND MUST NOT MOVE HERE. Gameplay/TraceParry.h is the policy and entry
// point (duration, cooldown, refusal reasons, the red tint) and UTraceTrailComponent owns the
// window itself, because trace invulnerability and trace colour are already its two jobs and the
// pass window's invulnerability already lives beside it — the two compose there by OR instead of
// fighting over a flag on the pawn. This function is the pawn-side doorway and nothing more.
//
// TraceParry::RequestParry() is documented as "THE ONE ENTRY POINT. Wire the parry bind, the bots
// and any debug command to this", and it is safe from any machine: it refuses non-carriers, refuses
// a cooldown, predicts the red tint on the owning client and sends the server RPC itself. So there
// is deliberately NO ServerParry RPC on this class — a second path to the same window is how the
// prediction and the authoritative window end up disagreeing.

void ATraceCharacter::DoParryPressed()
{
	// Every rule (carrier-only, cooldown, death) belongs to TraceParry::RequestParry, which reports
	// them through ETraceParryRefusal. Duplicating any of them here would give the mechanic two
	// policies to keep in step; the ONE thing checked locally is that there is a pawn to parry with,
	// and RequestParry treats even a null pawn as a refusal rather than a crash.
	ETraceParryRefusal Refusal = ETraceParryRefusal::None;
	TraceParry::RequestParry(this, &Refusal);

	// Verbose and refusal-only: a non-carrier holding the key is the normal case, not an error, and
	// with ten pawns in a match a Display line per press would be per-frame noise. Trace.DebugParry
	// (Gameplay/TraceParry.cpp) is the loud diagnostic when one is actually wanted.
	UE_LOG(LogTraceGame, Verbose, TEXT("[%s] parry input: %s"), *GetName(), LexToString(Refusal));
}

void ATraceCharacter::DoParryReleased()
{
	// Intentionally empty — see the header. Parry is a tap; the window length is TraceParry's and
	// holding the key must not extend it.
}

bool ATraceCharacter::IsParryActive() const
{
	return TraceParry::IsParryActiveFor(this);
}

bool ATraceCharacter::GetParryHudState(float& OutRemaining, float& OutTotal, bool& bOutActive) const
{
	OutRemaining = FMath::Max(0.f, TraceParry::GetCooldownRemainingFor(this));
	OutTotal = FMath::Max(KINDA_SMALL_NUMBER, TraceParry::GetCooldownTotal());
	bOutActive = TraceParry::IsParryActiveFor(this);
	return true;
}

// =================================================================================================
// Debug console command
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace
{
	/** Shared by both debug commands below: whichever world is actually playing. */
	UWorld* FindDebugGameWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/** The local player's pawn, or null. */
	ATraceCharacter* FindDebugLocalCharacter(UWorld* World)
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
				if (ATraceCharacter* TraceChar = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					return TraceChar;
				}
			}
		}
		return nullptr;
	}

	/**
	 * Trace.DebugViewProbe [IntervalSeconds] [Samples]
	 *
	 * Prints, at Display so it survives an automated run's default verbosity, the two numbers that
	 * decide whether this feature works and that a screenshot cannot show:
	 *
	 *   eyeErr — distance from the CAMERA to GetPawnViewLocation(), the point the shot is built
	 *            from. In first person this must be 0: the camera IS the gun. In third person it is
	 *            the arm length, which is the whole point of third person and is fine, because a
	 *            carrier cannot fire.
	 *   aimErr — angle between the camera's forward vector and GetAimDirection(). This must be ~0
	 *            in BOTH modes, or the crosshair is lying about where the bullet goes.
	 *
	 * A screenshot proves the view changed; this proves the view did not take the aim with it.
	 */
	FAutoConsoleCommand CmdDebugViewProbe(
		TEXT("Trace.DebugViewProbe"),
		TEXT("Trace.DebugViewProbe [IntervalSeconds] [Samples] — log camera vs aim agreement in whichever view mode is active."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Interval = (Args.Num() > 0) ? FMath::Max(0.05f, FCString::Atof(*Args[0])) : 1.f;
			const int32 Samples = (Args.Num() > 1) ? FMath::Max(1, FCString::Atoi(*Args[1])) : 30;

			int32 Emitted = 0;
			double SinceLast = 0.0;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Emitted, SinceLast, Interval, Samples](float DeltaTime) mutable -> bool
				{
					SinceLast += DeltaTime;
					if (SinceLast < Interval)
					{
						return true;
					}
					SinceLast = 0.0;

					ATraceCharacter* TraceChar = FindDebugLocalCharacter(FindDebugGameWorld());
					if (TraceChar == nullptr || TraceChar->Camera == nullptr)
					{
						return (++Emitted < Samples);
					}

					const FVector CameraLoc = TraceChar->Camera->GetComponentLocation();
					const FVector CameraFwd = TraceChar->Camera->GetForwardVector();
					const FVector EyeLoc = TraceChar->GetPawnViewLocation();
					const FVector AimDir = TraceChar->GetAimDirection();

					const double EyeError = FVector::Dist(CameraLoc, EyeLoc);
					const double AimErrorDegrees = FMath::RadiansToDegrees(
						FMath::Acos(FMath::Clamp(FVector::DotProduct(CameraFwd, AimDir), -1.0, 1.0)));

					// The Core's own answer, printed next to the pawn's mirror of it. THE TWO MUST
					// AGREE. carrier=1 with coreHolder pointing at somebody else is precisely the
					// "stuck in third person" bug: the camera is a pure function of the mirror, so a
					// mirror that outlives the possession strands the view behind the player forever.
					// passHeld is here for the same reason — a latched mouse1 with nobody's finger on
					// it can start a pass on its own, and nothing else in the game shows it.
					const ATraceCore* ProbeCore = ATraceCore::Get(TraceChar->GetWorld());
					const ATraceCharacter* ProbeHolder = (ProbeCore != nullptr) ? ProbeCore->GetCarrier() : nullptr;

					// crouch / eye / viewmodel are logged alongside because they are the three things
					// that can silently break the aim guarantee or the new viewmodel and that a
					// screenshot cannot distinguish: a crouch that never engaged looks exactly like
					// one that did if the eye height is not printed, and a viewmodel hidden by a
					// visibility bug looks exactly like one that was never built.
					UE_LOG(LogTraceGame, Display,
						TEXT("[ViewProbe] mode=%s carrier=%d coreHolder=%s holderIsMe=%d passActive=%d passHeld=%d predicted=%d ")
						TEXT("blend=%.2f arm=%.1f eyeErr=%.2fuu aimErr=%.4fdeg ")
						TEXT("bodyHiddenFromOwner=%d ctrlYaw=%d orientToMove=%d ")
						TEXT("crouched=%d sliding=%d halfHeight=%.1f baseEye=%.1f vmParts=%d vmVisible=%d"),
						TraceChar->GetViewBlendAlpha() < 0.5f ? TEXT("FIRST") : TEXT("THIRD"),
						TraceChar->IsCarrier() ? 1 : 0,
						*GetNameSafe(ProbeHolder),
						(ProbeHolder == TraceChar) ? 1 : 0,
						(ProbeCore != nullptr && ProbeCore->IsPassActive()) ? 1 : 0,
						(ProbeCore != nullptr && ProbeCore->IsPassInputHeld()) ? 1 : 0,
						(ProbeCore != nullptr && ProbeCore->IsPassLocallyPredicted()) ? 1 : 0,
						TraceChar->GetViewBlendAlpha(),
						TraceChar->SpringArm != nullptr ? TraceChar->SpringArm->TargetArmLength : -1.f,
						EyeError,
						AimErrorDegrees,
						(TraceChar->GetMesh() != nullptr && TraceChar->GetMesh()->bOwnerNoSee) ? 1 : 0,
						TraceChar->bUseControllerRotationYaw ? 1 : 0,
						(TraceChar->GetCharacterMovement() != nullptr && TraceChar->GetCharacterMovement()->bOrientRotationToMovement) ? 1 : 0,
						TraceChar->bIsCrouched ? 1 : 0,
						(TraceChar->GetTraceMovement() != nullptr && TraceChar->GetTraceMovement()->IsSliding()) ? 1 : 0,
						TraceChar->GetCapsuleComponent() != nullptr ? TraceChar->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : -1.f,
						TraceChar->BaseEyeHeight,
						TraceChar->GetViewModelPartCount(),
						TraceChar->IsViewModelVisible() ? 1 : 0);

					return (++Emitted < Samples);
				}),
				0.f);
		}));

	/**
	 * Trace.DebugAnimProbe [Seconds] [SampleInterval]
	 *
	 * "ARE THE CHARACTERS ACTUALLY ANIMATING?" — answered with a measurement instead of an opinion.
	 *
	 * A screenshot cannot tell a T-pose from a run cycle caught at its neutral frame, and it
	 * certainly cannot tell an idle loop from a run loop. Both of those failures have a specific,
	 * plausible cause in this project: the art is imported per developer (see the file header), so
	 * a machine can have SKM_Manny_Simple present and ABP_Unarmed absent, which renders a perfectly
	 * good-looking, perfectly motionless human.
	 *
	 * So this samples the actual POSE. It watches foot_l and hand_r in COMPONENT space — component,
	 * not world, so walking the actor across the pitch contributes nothing and only the animation
	 * can move them — and reports the travel of each. The pass/fail is unambiguous:
	 *
	 *     range ~0 uu  -> no anim instance, or a single static frame (T-pose / bind pose)
	 *     range >10 uu -> limbs are being posed; a Manny run cycle swings a foot ~60-90 uu
	 *
	 * It prefers whichever character is actually MOVING, because an idle pose legitimately barely
	 * moves the feet and would read as a failure. Display verbosity throughout: this is a diagnostic
	 * whose whole job is to be readable in a log somebody is already looking at.
	 */
	FAutoConsoleCommand CmdDebugAnimProbe(
		TEXT("Trace.DebugAnimProbe"),
		TEXT("Trace.DebugAnimProbe [Seconds] [SampleInterval] — measure whether the character anim blueprint is really posing limbs."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Seconds = (Args.Num() > 0) ? FMath::Max(0.5f, FCString::Atof(*Args[0])) : 6.f;
			const float Interval = (Args.Num() > 1) ? FMath::Max(0.02f, FCString::Atof(*Args[1])) : 0.1f;

			// Armed-line at Display, for the same reason every other diagnostic in this file has one:
			// a probe that prints nothing is indistinguishable from a probe that never ran, and this
			// one is deliberately silent until it has an answer.
			UE_LOG(LogTraceGame, Display, TEXT("[AnimProbe] armed: sampling for %.1fs every %.2fs."), Seconds, Interval);

			// Captured by value into the ticker; the lambda outlives this scope by design.
			double Elapsed = 0.0;
			double FirstSampleTime = -1.0;
			double LastSampleTime = -1.0e9;
			FBox FootBounds(ForceInit);
			FBox HandBounds(ForceInit);
			int32 Samples = 0;
			float FastestSpeed = 0.f;
			FString SubjectName;
			FString AnimClassName(TEXT("<none>"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[Elapsed, FirstSampleTime, LastSampleTime, FootBounds, HandBounds, Samples, FastestSpeed, SubjectName, AnimClassName, Seconds, Interval](float DeltaTime) mutable -> bool
				{
					Elapsed += DeltaTime;

					// EVERY-FRAME TICKER, sampled on our own clock. Registering with a non-zero ticker
					// period was measured NOT to fire in this project's -ExecCmds path at all (the probe
					// armed and then never ran once), while every other debug command here registers at
					// 0 and works. So: tick every frame, and decide here whether this frame is a sample.
					const bool bSampleThisFrame = (Elapsed - LastSampleTime) >= Interval;
					if (!bSampleThisFrame)
					{
						return true;
					}
					LastSampleTime = Elapsed;

					UWorld* World = FindDebugGameWorld();
					if (World == nullptr)
					{
						return (Elapsed < Seconds);
					}

					// Whoever is moving fastest right now. An idle Manny's feet are planted, so probing
					// a standing player would measure "no animation" on a perfectly healthy build.
					ATraceCharacter* Subject = nullptr;
					float BestSpeed = -1.f;
					for (TActorIterator<ATraceCharacter> It(World); It; ++It)
					{
						ATraceCharacter* Candidate = *It;
						if (Candidate == nullptr || !Candidate->IsAlive() || Candidate->GetMesh() == nullptr)
						{
							continue;
						}
						const float Speed = Candidate->GetVelocity().Size2D();
						if (Speed > BestSpeed)
						{
							BestSpeed = Speed;
							Subject = Candidate;
						}
					}

					if (Subject != nullptr)
					{
						USkeletalMeshComponent* MeshComp = Subject->GetMesh();
						FastestSpeed = FMath::Max(FastestSpeed, BestSpeed);
						SubjectName = Subject->GetName();

						const UAnimInstance* AnimInstance = MeshComp->GetAnimInstance();
						AnimClassName = (AnimInstance != nullptr) ? AnimInstance->GetClass()->GetName() : FString(TEXT("<none>"));

						// Component space: the actor's own travel across the pitch cannot contribute, so
						// anything this measures came from the animation and nothing else.
						if (MeshComp->GetSkinnedAsset() != nullptr)
						{
							FootBounds += MeshComp->GetSocketTransform(TEXT("foot_l"), RTS_Component).GetLocation();
							HandBounds += MeshComp->GetSocketTransform(TEXT("hand_r"), RTS_Component).GetLocation();
							if (Samples == 0)
							{
								FirstSampleTime = Elapsed;
							}
							++Samples;
						}
					}

					// THE SAMPLING WINDOW STARTS AT THE FIRST CHARACTER, NOT AT THE COMMAND.
					//
					// Measured the hard way: -ExecCmds fires at engine init, the menu map and the level
					// travel then take 15-30s on a loaded machine, and a 14s window had closed before a
					// single pawn existed — the probe honestly reported "NOT ANIMATING" about an empty
					// world. A diagnostic that can produce a false failure is worse than none, so the
					// clock does not start until there is somebody to watch.
					if (Samples == 0)
					{
						if (Elapsed < (Seconds + 30.0))
						{
							return true;
						}

						// Absolute stop, and it SAYS SO. A probe that quietly gives up is how a run
						// gets read as "the feature is fine, nothing was reported".
						UE_LOG(LogTraceGame, Display,
							TEXT("[AnimProbe] gave up after %.0fs: no living character with a skeletal mesh ever ")
							TEXT("appeared. On a machine without the art import that is EXPECTED — the pawns are ")
							TEXT("fallback primitives; see the [CharacterArt] line above."), Elapsed);
						return false;
					}

					if (Elapsed < (FirstSampleTime + Seconds))
					{
						return true;
					}

					const FVector FootTravel = (Samples > 0) ? FootBounds.GetSize() : FVector::ZeroVector;
					const FVector HandTravel = (Samples > 0) ? HandBounds.GetSize() : FVector::ZeroVector;
					const float FootRange = static_cast<float>(FootTravel.GetMax());
					const float HandRange = static_cast<float>(HandTravel.GetMax());

					UE_LOG(LogTraceGame, Display,
						TEXT("[AnimProbe] subject=%s animClass=%s samples=%d peakSpeed=%.0fuu/s ")
						TEXT("foot_l travel=%.1fuu hand_r travel=%.1fuu -> %s"),
						SubjectName.IsEmpty() ? TEXT("<none>") : *SubjectName, *AnimClassName, Samples, FastestSpeed,
						FootRange, HandRange,
						(FootRange > 10.f || HandRange > 10.f)
							? TEXT("ANIMATING (limbs are being posed)")
							: TEXT("NOT ANIMATING (static pose — check the anim blueprint / the art import)"));

					return false;
				}),
				0.f);
		}));

	/**
	 * Trace.DebugCrouch [HoldSeconds] [DelaySeconds]
	 *
	 * Pulses the crouch/slide input on the local player, so the slide presentation can be
	 * photographed by an automated -TraceAutoShot run.
	 *
	 * It exists for the same reason Trace.DebugTakeCore does: the thing being verified is a VISUAL
	 * state, a screenshot harness has no hands, and the crouch bind belongs to the input layer,
	 * which is a different file and a different pass. It drives the two real entry points —
	 * ACharacter::Crouch() and UTraceCharacterMovementComponent::SetWantsToSlide() — so the slide
	 * begins through the same predicted path a key press uses, and the lean, the eye dip and the
	 * skid streak are driven by exactly the state the real thing produces. Only the key is skipped.
	 *
	 * IT PULSES RATHER THAN HOLDING, and that is not a detail. A ground slide needs a fresh PRESS
	 * (see UTraceCharacterMovementComponent's SlideBufferRemaining note), so holding the key gives
	 * you ONE slide of SlideDuration and then a walk. A harness taking a frame every second or two
	 * would then photograph a slide that had already ended and conclude the feature was dead —
	 * which is exactly the failure mode this project has already hit twice with suppressed logging.
	 * Pressing and releasing on a cycle guarantees a slide window overlaps a capture.
	 */
	FAutoConsoleCommand CmdDebugCrouch(
		TEXT("Trace.DebugCrouch"),
		TEXT("Trace.DebugCrouch [HoldSeconds] [DelaySeconds] — pulse crouch/slide on the local player so the slide pose can be captured."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Hold = (Args.Num() > 0) ? FMath::Max(0.1f, FCString::Atof(*Args[0])) : 4.f;
			const float Delay = (Args.Num() > 1) ? FMath::Max(0.f, FCString::Atof(*Args[1])) : 0.f;

			// Long enough to cover a whole slide window, short enough to fit several presses inside
			// a capture window.
			constexpr double PressSeconds = 1.1;
			constexpr double ReleaseSeconds = 0.5;

			double Elapsed = 0.0;
			bool bLogged = false;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Elapsed, bLogged, Hold, Delay](float DeltaTime) mutable -> bool
				{
					Elapsed += DeltaTime;
					if (Elapsed < Delay)
					{
						return true;
					}

					ATraceCharacter* TraceChar = FindDebugLocalCharacter(FindDebugGameWorld());
					if (TraceChar == nullptr)
					{
						// Keep waiting for a pawn until well past the hold window, then give up.
						return Elapsed < (Delay + Hold + 30.0);
					}

					UTraceCharacterMovementComponent* Movement = TraceChar->GetTraceMovement();

					// BOTH entry points: the movement component ORs its own intent flag with the
					// engine's bWantsToCrouch, and which of the two a real bind ends up using is the
					// input layer's business, not this command's.
					auto SetHeld = [TraceChar, Movement](bool bDown)
					{
						if (bDown) { TraceChar->Crouch(); } else { TraceChar->UnCrouch(); }
						if (Movement != nullptr) { Movement->SetWantsToSlide(bDown); }
					};

					if (Elapsed >= (Delay + Hold))
					{
						SetHeld(false);
						UE_LOG(LogTraceGame, Display,
							TEXT("[DebugCrouch] %s finished (sliding=%d baseEye=%.1f)."),
							*TraceChar->GetName(),
							(Movement != nullptr && Movement->IsSliding()) ? 1 : 0,
							TraceChar->BaseEyeHeight);
						return false;
					}

					if (!bLogged)
					{
						bLogged = true;
						UE_LOG(LogTraceGame, Display,
							TEXT("[DebugCrouch] %s pulsing crouch/slide for %.1fs (capsule halfHeight=%.1f baseEye=%.1f)."),
							*TraceChar->GetName(),
							Hold,
							TraceChar->GetCapsuleComponent() != nullptr ? TraceChar->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : -1.f,
							TraceChar->BaseEyeHeight);
					}

					// Square wave on the key, re-asserted every tick so a prediction correction that
					// clears the intent cannot silently end the test.
					const double Phase = FMath::Fmod(Elapsed - Delay, PressSeconds + ReleaseSeconds);
					SetHeld(Phase < PressSeconds);
					return true;
				}),
				0.f);
		}));

	/**
	 * Trace.DebugTakeCore [DelaySeconds] [TimeoutSeconds]
	 *
	 * Hands the Core to the local player, so the third-person carry view can be captured by an
	 * automated -TraceAutoShot run. Without this the only way to see it is for a human to walk to the
	 * centre of a 24000 uu arena and beat nine bots to the pickup, which is not a thing a screenshot
	 * harness can do — and "I could not photograph it" is how an unverified feature ships broken.
	 *
	 * It is not a cheat that bypasses the rules: it calls ATraceCore::TryPickup(), the same entry
	 * point the pickup sphere calls, so the Core really attaches, the trail really starts, the
	 * PlayerState really updates and bIsCarrier really replicates. Only the walking is skipped.
	 *
	 * HoldSeconds then passes the Core straight back out through ATraceCharacter::DoPass() — again
	 * the real path, the one RMB uses — so a single automated run covers the whole round trip:
	 * first person, pull back to third on pickup, push back in to first on release. Testing only
	 * the way in would leave the return blend unmeasured, and that is half the feature.
	 *
	 * It self-schedules: -ExecCmds fires long before the match map has a possessed pawn, so this
	 * retries on the core ticker until a living local pawn and a loose Core both exist, then stops.
	 */
	FAutoConsoleCommand CmdDebugTakeCore(
		TEXT("Trace.DebugTakeCore"),
		TEXT("Trace.DebugTakeCore [DelaySeconds] [HoldSeconds] [TimeoutSeconds] — give the local player the Core, then pass it away again."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 0.f;
			const float Hold = (Args.Num() > 1) ? FMath::Max(0.f, FCString::Atof(*Args[1])) : 0.f;
			const float Timeout = (Args.Num() > 2) ? FMath::Max(1.f, FCString::Atof(*Args[2])) : 60.f;

			// Captured by value into the ticker; the lambda outlives this scope by design.
			double ElapsedSeconds = 0.0;
			double CarriedSinceSeconds = -1.0;
			double PassPressedSeconds = -1.0;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[ElapsedSeconds, CarriedSinceSeconds, PassPressedSeconds, Delay, Hold, Timeout](float DeltaTime) mutable -> bool
				{
					ElapsedSeconds += DeltaTime;
					if (ElapsedSeconds < Delay)
					{
						return true;   // keep waiting
					}

					// ---- second leg: hand it back, so the return blend is exercised too ----------
					//
					// INTEGRATION FIX (spec §4). This used to be a single Carrier->DoPass() call, and
					// under the old model that was complete: DoPass() threw the Core along the aim and
					// it detached on the spot. The pass is now a HELD hover — DoPass() is only
					// DoPassPressed(), and the transfer needs the button to stay down for 0.5s with a
					// teammate under the crosshair. Pressing once and immediately dropping the ticker
					// left mouse1 LATCHED DOWN on the Core with nothing left alive to release it, so
					// the pawn sat with its shield suppressed and its trace invulnerable indefinitely
					// while this command logged that it had passed the Core away. It had not.
					//
					// So: press, then keep ticking until the Core actually leaves, and always send the
					// matching release. Whether it completes or times out, both halves of the risk beat
					// are restored, which is the state the camera blend is supposed to be measured in.
					if (CarriedSinceSeconds >= 0.0)
					{
						if ((ElapsedSeconds - CarriedSinceSeconds) < Hold)
						{
							return true;
						}

						ATraceCharacter* Carrier = FindDebugLocalCharacter(FindDebugGameWorld());
						if (Carrier == nullptr)
						{
							return false;
						}

						if (PassPressedSeconds < 0.0)
						{
							// Whether we still HAVE the Core decides how to read everything below. The
							// local pawn is re-resolved every tick, so by now it may be a different
							// actor entirely: getting trail-dashed while carrying kills you, hands the
							// Core to the dasher and respawns you as a fresh pawn. Without this the
							// "no longer carrying" that follows would be reported as a successful
							// transfer, which is how a harness talks itself into a false pass.
							if (!Carrier->IsCarrier())
							{
								UE_LOG(LogTraceGame, Display,
									TEXT("[DebugTakeCore] %s no longer has the Core after %.1fs (lost it while carrying); nothing to pass."),
									*Carrier->GetName(), Hold);
								return false;
							}

							// The same entry point mouse1 uses, so the server evaluates our real aim.
							Carrier->DoPassPressed();
							PassPressedSeconds = ElapsedSeconds;
							UE_LOG(LogTraceGame, Display,
								TEXT("[DebugTakeCore] %s began a pass after %.1fs carrying (shield down, trace invulnerable)."),
								*Carrier->GetName(), Hold);
							return true;
						}

						// Give the hover 20s to find a teammate — far more than the 0.5s the rule needs,
						// and the surplus is not slack. A pass that acquires and then CANCELS (a
						// receiver stepping behind cover, the crosshair drifting for a frame) spends
						// PassCooldownSeconds before the still-held button may acquire again, so a
						// window of a few seconds could only ever observe the first attempt. Watching
						// a held button recover from a cancel is the whole behaviour under test.
						const bool bStillCarrying = Carrier->IsCarrier();
						if (bStillCarrying && (ElapsedSeconds - PassPressedSeconds) < 20.0)
						{
							return true;
						}

						Carrier->DoPassReleased();

						UE_LOG(LogTraceGame, Display,
							TEXT("[DebugTakeCore] %s released the pass after %.1fs (carrier=%d, %s, view blend -> first person)."),
							*Carrier->GetName(), ElapsedSeconds - PassPressedSeconds, bStillCarrying ? 1 : 0,
							bStillCarrying ? TEXT("no receiver found - cancelled, shield restored") : TEXT("transfer completed"));
						return false;
					}

					if (ElapsedSeconds > (Delay + Timeout))
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[DebugTakeCore] Gave up after %.1fs: no living local pawn and loose Core."), Timeout);
						return false;
					}

					UWorld* World = FindDebugGameWorld();
					if (World == nullptr)
					{
						return true;
					}

					// Authority only. On a client the Core is server-owned and TryPickup would be a
					// no-op; say so rather than spinning silently for a minute.
					if (World->GetNetMode() == NM_Client)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[DebugTakeCore] Client build: the server owns the Core."));
						return false;
					}

					ATraceCore* TheCore = nullptr;
					if (const ATraceGameState* State = World->GetGameState<ATraceGameState>())
					{
						TheCore = State->Core;
					}
					if (TheCore == nullptr)
					{
						return true;
					}

					ATraceCharacter* TraceChar = FindDebugLocalCharacter(World);
					if (TraceChar == nullptr || !TraceChar->IsAlive())
					{
						return true;
					}

					if (TraceChar->IsCarrier())
					{
						UE_LOG(LogTraceGame, Display, TEXT("[DebugTakeCore] %s is already the carrier."), *TraceChar->GetName());
						CarriedSinceSeconds = ElapsedSeconds;
						return (Hold > 0.f);
					}

					TheCore->TryPickup(TraceChar);

					// TryPickup can legitimately refuse (someone else is carrying it, pickup lockout).
					// Report what actually happened rather than what was asked for, and keep retrying
					// if it did not take.
					if (!TraceChar->IsCarrier())
					{
						return true;
					}

					UE_LOG(LogTraceGame, Display,
						TEXT("[DebugTakeCore] %s is now carrying the Core (view blend -> third person)."),
						*TraceChar->GetName());

					CarriedSinceSeconds = ElapsedSeconds;
					return (Hold > 0.f);
				}),
				0.f);
		}));

	/**
	 * Trace.DebugAimAtTeammate [DelaySeconds] [DurationSeconds]
	 *
	 * Points the local player's view at their nearest living teammate and keeps it there.
	 *
	 * Same reason as Trace.DebugTakeCore, one step further along. The pass reticle only reaches its
	 * interesting state — brackets closed, team coloured, receiver named, hold ring filling — when a
	 * legal receiver is actually under the crosshair, and a headless harness cannot aim at a bot that
	 * is wandering a 24000 uu arena. Without this, the acquired state is the one state of the HUD
	 * that can never be photographed, which is exactly the sort of gap this project has shipped bugs
	 * through before.
	 *
	 * It moves the CONTROL ROTATION only, which is the same thing a mouse moves: the aim, the camera
	 * and the reticle all follow from it through the normal path, and ATraceCore evaluates the pass
	 * against the same rotation it would have got from a human. No pass rule is bypassed — if the
	 * teammate is behind a wall the pass is still refused and the reticle still shows no lock, which
	 * is the correct answer.
	 */
	FAutoConsoleCommand CmdDebugAimAtTeammate(
		TEXT("Trace.DebugAimAtTeammate"),
		TEXT("Trace.DebugAimAtTeammate [DelaySeconds] [DurationSeconds] — hold the local player's aim on their nearest teammate."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 0.f;
			const float Duration = (Args.Num() > 1) ? FMath::Max(0.5f, FCString::Atof(*Args[1])) : 20.f;

			double ElapsedSeconds = 0.0;
			// Weak, and re-logged whenever the choice changes: which teammate is reachable moves
			// around constantly in a live match, and "the harness stopped finding anyone" is the
			// single most useful thing this command can tell whoever is reading the log afterwards.
			TWeakObjectPtr<ATraceCharacter> LastAimedAt;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[ElapsedSeconds, LastAimedAt, Delay, Duration](float DeltaTime) mutable -> bool
				{
					ElapsedSeconds += DeltaTime;
					if (ElapsedSeconds < Delay)
					{
						return true;
					}
					if (ElapsedSeconds > (Delay + Duration))
					{
						return false;
					}

					UWorld* World = FindDebugGameWorld();
					ATraceCharacter* Me = FindDebugLocalCharacter(World);
					const ATraceGameState* State = (World != nullptr) ? World->GetGameState<ATraceGameState>() : nullptr;
					if (Me == nullptr || State == nullptr || !Me->IsAlive())
					{
						return true;
					}

					// Nearest living ally, found through the PlayerArray rather than an actor iterator:
					// every pawn in this match belongs to a player state, so this keeps the search to
					// the ten actors that could possibly be receivers.
					//
					// LINE OF SIGHT IS PART OF THE SEARCH, and it has to be. The first version simply
					// took the nearest ally, and the nearest ally in a real match is very often on a
					// platform behind a railing — so the harness pointed the crosshair at a teammate
					// the pass rules correctly refused, and photographed a reticle that was correctly
					// showing no lock. The Core's own LOS test (ATraceCore::IsLegalPassTarget) is
					// against world geometry only, so this mirrors it exactly.
					const FVector MyView = Me->GetPawnViewLocation();

					FCollisionObjectQueryParams ObjectParams;
					ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
					ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);

					ATraceCharacter* Nearest = nullptr;
					double NearestDistance = TNumericLimits<double>::Max();
					for (const APlayerState* PlayerState : State->PlayerArray)
					{
						ATraceCharacter* Candidate = (PlayerState != nullptr)
							? Cast<ATraceCharacter>(PlayerState->GetPawn())
							: nullptr;
						if (Candidate == nullptr || Candidate == Me || !Candidate->IsAlive())
						{
							continue;
						}
						// Same test ATraceCore::AreAllies makes, spelled out: that helper is file-local
						// to TraceCore.cpp and a second copy of the team rules is not worth a header.
						if (Me->GetTeam() == ETraceTeam::None || Candidate->GetTeam() != Me->GetTeam())
						{
							continue;
						}

						const double Distance = FVector::Dist(Me->GetActorLocation(), Candidate->GetActorLocation());
						if (Distance >= NearestDistance)
						{
							continue;
						}

						const FVector CandidateChest = Candidate->GetActorLocation() + FVector(0.0, 0.0, 40.0);
						FCollisionQueryParams QueryParams(FName(TEXT("TraceDebugAimLOS")), /*bTraceComplex=*/false);
						QueryParams.AddIgnoredActor(Me);
						QueryParams.AddIgnoredActor(Candidate);
						if (World->LineTraceTestByObjectType(MyView, CandidateChest, ObjectParams, QueryParams))
						{
							continue;   // something solid in the way: not a legal receiver, do not aim at it
						}

						NearestDistance = Distance;
						Nearest = Candidate;
					}

					AController* MyController = Me->GetController();
					if (Nearest != LastAimedAt.Get())
					{
						LastAimedAt = Nearest;
						UE_LOG(LogTraceGame, Display,
							TEXT("[DebugAimAtTeammate] %s is now looking at %s (%.0f uu, clear line of sight)."),
							*Me->GetName(),
							(Nearest != nullptr) ? *Nearest->GetName() : TEXT("<nobody reachable>"),
							(Nearest != nullptr) ? NearestDistance : 0.0);
					}

					if (Nearest == nullptr || MyController == nullptr)
					{
						return true;
					}

					// Chest height, which is the point ATraceCore aims a pass at.
					const FVector Chest = Nearest->GetActorLocation() + FVector(0.0, 0.0, 40.0);
					MyController->SetControlRotation((Chest - MyView).Rotation());
					return true;
				}),
				0.f);
		}));

	/**
	 * Trace.DebugPassTargets [IntervalSeconds] [Samples]
	 *
	 * For the local player, asks ATraceCore::IsLegalPassTarget about EVERY other character and prints
	 * which test each one failed.
	 *
	 * This exists because of a bug whose only symptom was a camera. The view mode is
	 * `!bIsCarrier` and nothing else, so a carrier who cannot get rid of the Core is a player stuck in
	 * third person — and "the camera is stuck" and "the pass never acquires anybody" produce exactly
	 * the same log line ("no receiver found") and exactly the same screenshot. The reasons come out of
	 * the real rule through an out-param rather than from a copy of it here, so this cannot drift away
	 * from what the game actually enforces.
	 *
	 * When the answer is "no line of sight" it re-runs the same ECC_Visibility trace and NAMES THE
	 * BLOCKER, because "something is in the way" across a 24000 uu arena is not a diagnosis.
	 */
	FAutoConsoleCommand CmdDebugPassTargets(
		TEXT("Trace.DebugPassTargets"),
		TEXT("Trace.DebugPassTargets [IntervalSeconds] [Samples] — log why each teammate is or is not a legal pass receiver."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Interval = (Args.Num() > 0) ? FMath::Max(0.1f, FCString::Atof(*Args[0])) : 1.f;
			const int32 Samples = (Args.Num() > 1) ? FMath::Max(1, FCString::Atoi(*Args[1])) : 30;

			int32 Emitted = 0;
			double SinceLast = 0.0;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Emitted, SinceLast, Interval, Samples](float DeltaTime) mutable -> bool
				{
					SinceLast += DeltaTime;
					if (SinceLast < Interval)
					{
						return true;
					}
					SinceLast = 0.0;

					UWorld* World = FindDebugGameWorld();
					ATraceCharacter* Me = FindDebugLocalCharacter(World);
					ATraceCore* TheCore = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
					if (Me == nullptr || TheCore == nullptr)
					{
						return (++Emitted < Samples);
					}

					TArray<ATraceCharacter*> Candidates;
					TheCore->GatherCharacters(Candidates);

					const FVector MyView = Me->GetPawnViewLocation();
					const FVector MyAim = Me->GetAimDirection();

					UE_LOG(LogTraceGame, Display,
						TEXT("[PassTargets] %s carrier=%d holder=%s candidates=%d best=%s"),
						*Me->GetName(), Me->IsCarrier() ? 1 : 0, *GetNameSafe(TheCore->GetCarrier()),
						Candidates.Num(), *GetNameSafe(TheCore->FindPassTargetFor(Me)));

					for (const ATraceCharacter* Candidate : Candidates)
					{
						if (Candidate == nullptr || Candidate == Me)
						{
							continue;
						}

						const TCHAR* Reason = TEXT("?");
						const bool bLegal = TheCore->IsLegalPassTarget(Me, Candidate, /*bRequireAim=*/true, &Reason);

						const FVector Chest = Candidate->GetActorLocation() + FVector(0.0, 0.0, 20.0);
						FVector ToTarget = Chest - MyView;
						const double Distance = ToTarget.Size();
						const double AngleDegrees = (Distance > 1.0)
							? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
								FVector::DotProduct(ToTarget / Distance, MyAim), -1.0, 1.0)))
							: 0.0;

						// Only the interesting ones: an enemy on the far side of the map failing "not
						// an ally" is noise, and ten players times thirty samples of it is a log nobody
						// reads.
						const bool bAlly = (Candidate->GetTeam() != ETraceTeam::None)
							&& (Candidate->GetTeam() == Me->GetTeam());
						if (!bAlly)
						{
							continue;
						}

						FString Blocker;
						if (FCString::Strcmp(Reason, TEXT("no line of sight")) == 0)
						{
							FCollisionQueryParams QueryParams(FName(TEXT("TraceDebugPassLOS")), /*bTraceComplex=*/false);
							QueryParams.AddIgnoredActor(TheCore);
							QueryParams.AddIgnoredActor(Me);
							QueryParams.AddIgnoredActor(Candidate);

							FHitResult Hit;
							if (World->LineTraceSingleByChannel(Hit, MyView, Chest, ECC_Visibility, QueryParams))
							{
								Blocker = FString::Printf(TEXT(" blockedBy=%s/%s profile=%s at %.0fuu"),
									*GetNameSafe(Hit.GetActor()),
									*GetNameSafe(Hit.GetComponent()),
									Hit.GetComponent() != nullptr
										? *Hit.GetComponent()->GetCollisionProfileName().ToString()
										: TEXT("?"),
									Hit.Distance);
							}
						}

						UE_LOG(LogTraceGame, Display,
							TEXT("[PassTargets]   ally %-18s legal=%d reason=%-24s dist=%.0fuu angle=%.1fdeg alive=%d%s"),
							*Candidate->GetName(), bLegal ? 1 : 0, Reason, Distance, AngleDegrees,
							Candidate->IsAlive() ? 1 : 0, *Blocker);
					}

					return (++Emitted < Samples);
				}),
				0.f);
		}));
}

#endif // !UE_BUILD_SHIPPING
