// Trace — the player pawn. See TraceCharacter.h for the shape of the thing and why.

#include "Core/TraceCharacter.h"

#include "Net/UnrealNetwork.h"

#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"                // FMinimalViewInfo (GetViewModelMuzzleViewPoint)
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
#include "Abilities/TraceAbilityComponent.h"    // Trace.Bounds.Verify: "the cooldown kept ticking"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceRailgunFireCurve.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceMelee.h"                // spec v28 §10: TraceMelee::IsDualWieldEnabled()
#include "Gameplay/TraceParry.h"                // the parry entry point and its queries (spec §3)
#include "Gameplay/TraceTrailComponent.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Net/TraceLagCompensationComponent.h"
#include "Settings/TraceGameUserSettings.h"    // ApplySavedFieldOfView(): the VIDEO page's FOV row
#include "Trace.h"
#include "TraceSettings.h"
#include "World/TraceArenaBuilder.h"          // SetBase(): the arena is not a moving platform

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

	/**
	 * THE SHIPPED THIRD-PERSON CARRY FRAMING, held explicitly since spec v7 §3 — 172 uu above the
	 * carrier's actor centre.
	 *
	 * GetThirdPersonPivotZ() derives its height from the trail so the camera clears the wall, and for
	 * the whole life of that rule the trail was 190 uu tall, which put the pivot at
	 * 190*0.5 + clamp(190*0.12,14,42)*0.5 + 66 = 172.4. That is the framing the carry blend was tuned
	 * with, playtested with and signed off on.
	 *
	 * v7 §3 then cut the trail to 63 uu "in order to make visibility around the trace better". The
	 * derivation alone would have answered 38.5 + 66 = 104.5 and silently dropped the carry camera
	 * ~68 uu — a large, unrequested change to a DO-NOT-REGRESS feature, arriving as a side effect of
	 * a visibility change that never mentioned the camera. The clearance rule is still right and
	 * still live (raise TrailHeight past ~212 and it takes over again); this is the floor it may not
	 * fall below, replacing the generic ThirdPersonPivotZ 60 in that role while carrying.
	 *
	 * If the lower camera turns out to be wanted now that the trail no longer fills the frame, this
	 * one constant is the whole change — drop it back to ThirdPersonPivotZ and the derivation wins.
	 */
	constexpr float CarryPivotZ = 172.4f;

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

	/**
	 * Where the FALLBACK cube gun's shot leaves, in rig space — the centre of the VMMuzzle ring.
	 *
	 * Named rather than typed twice: the parts table below places VMMuzzle here, and the muzzle marker
	 * ATraceTracer reads is parked here too. Two literals would be two things to remember to move
	 * together, and the failure mode of forgetting is a beam that leaves from beside the barrel — which
	 * is precisely the defect spec v26 §4 is about.
	 */
	const FVector CubeGunMuzzle(19.6f, 0.f, 2.4f);

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

	// --- The railgun ------------------------------------------------------------------------------
	//
	// The blocky procedural gun above is the FALLBACK now. When the imported railgun art resolves,
	// three static meshes stand in for the twelve gun cubes (the hands, knuckles, forearms and cuffs
	// are still built from the table — they hold the new weapon, they were never part of it).
	//
	// SCALE. The source model is authored 1:1 in metres and measures 185 cm nose to tail, which is a
	// real railgun and about six times the length of the cube gun it replaces. Everything is placed
	// against the mesh's own measured landmarks, so these three numbers are the whole placement:
	//
	//   grip     mesh-local (-30.0, 0, -9.0) uu   ->  rig (-0.8, 0, -4.6), the right hand
	//   foregrip mesh-local (+30.0, 0, -4.5) uu   ->  rig, where the left hand is moved to
	//   muzzle   mesh-local (107.4, 0, +4.5) uu   ->  rig  29.4 uu out, vs the cube gun's 19.6
	//
	// RailgunOrigin is therefore not a taste value: it is (right hand) - RailgunScale * (grip).
	constexpr float RailgunScale = 0.22f;
	const FVector RailgunOrigin(5.8f, 0.f, -2.6f);

	/** Mesh-local landmarks, in the mesh's own centimetres. Read out of railgun_manifest.json. */
	const FVector RailgunMuzzleLocal(107.4f, 0.f, 4.5f);

	/**
	 * The left hand moves off the cube gun's frame and onto the railgun's foregrip, dropped half the
	 * foregrip's depth so the fist closes around it rather than resting on top.
	 */
	const FVector RailgunLeftHand(12.4f, -2.6f, -5.5f);
	const FVector RailgunLeftKnuckle(14.5f, -2.6f, -4.5f);

	// ---------------------------------------------------------------------------------------------
	// [DUALWIELD] THE OFF HAND  (spec v28 §10: "Gun in one hand, knife in the other")
	// ---------------------------------------------------------------------------------------------
	//
	// The owner's sentence is literal, so the left hand has to LEAVE the weapon. On both rigs it is
	// currently a support hand — the cube gun's frame, or the railgun's foregrip — and leaving it
	// there while a blade grew out of it would read as a knife taped to the gun rather than as a
	// second weapon.
	//
	// WHERE IT GOES, AND WHY THESE THREE NUMBERS. Down and out to the left, forward of the frame:
	//   x  +6.0   ahead of the right hand (-0.8) so the blade is not hidden behind the gun body, and
	//             far short of the muzzle (19.6 cube / 29.4 railgun) so it never crosses the barrel.
	//   y  -9.6   nearly three times the -3.4 support pose. The rig itself hangs at y = +14.5 (the
	//             lower RIGHT of frame), so this pulls the off hand back toward the screen centre —
	//             which is where a low guard actually sits — without reaching the crosshair.
	//   z  -9.5   5.5 uu below the support pose. A low, relaxed guard: the blade angles up and inward
	//             from it, which is the pose the stab animation already thrusts out of.
	//
	// The SAME pose for the cube gun and the railgun, deliberately. The support hand differed between
	// them because it had to touch each weapon's geometry; an off hand touches neither, so one pose is
	// correct for both and a second constant would only be a second thing to keep in step.
	//
	// FRAMING AND DEPTH, the two rules the file header requires every viewmodel constant to keep:
	// x = 6.0 is a third of the cube gun's muzzle depth and a fifth of the railgun's, and the knife
	// rig hung off it reaches ~23 uu at rest and ~39 uu fully extended (see TraceKnifeLayout) — still
	// well inside the 76 uu the muzzle already clears the capsule at. Nothing here can touch world
	// geometry or the near plane.
	const FVector DualWieldLeftHand(6.0f, -9.6f, -9.5f);
	const FVector DualWieldLeftKnuckle(8.1f, -9.6f, -8.5f);

	// Fire animation. The artist's clip is 1.90 s: 1.05 s of charge, a 0.10 s discharge, then 0.75 s
	// of decay. Trace's gun has NO windup — it is a 150 RPM hitscan — so the charge segment is not
	// played; the shot IS the discharge frame, and what plays is the tail from there to the end,
	// time-warped to finish inside one fire interval so the rails are always shut before the next
	// round leaves. See Source/Trace/Gameplay/TraceRailgunFireCurve.h for the authored table.
	constexpr float RailgunFireMinSeconds = 0.12f;
	constexpr float RailgunFireIntervalFraction = 0.9f;

	/** Rail walls throw ±75 mm apart and cant outward; both are mesh centimetres, so both scale. */
	constexpr float RailgunRailThrowUU = 7.5f;
	constexpr float RailgunRailCantDegrees = 4.f;

	/** The receiver's own recoil INSIDE the hands — 45 mm back, 0.10 rad down, as authored. This is
	  * on top of ViewModelKick, which recoils the whole rig (hands included) and is not replaced. */
	constexpr float RailgunRecoilBackUU = 4.5f;
	constexpr float RailgunRecoilPitchDegrees = 5.73f;

	// --- THE SMG  (spec v30 §2/§3/§4) --------------------------------------------------------------
	//
	// Demo 24 shipped the SMG with NO viewmodel of its own: pressing `3` changed the damage, the fire
	// rate and the magazine, and left the same pistol on screen. A verifier flagged exactly that. The
	// rig below is what closes it, and everything about its placement is arithmetic for the same
	// reason the pistol's is — a gun placed by eye has to be re-placed by eye every time anything
	// around it moves.
	//
	// SCALE, AND WHY 0.30 RATHER THAN THE PISTOL'S 0.22. The two models are authored at 1:1 in metres
	// and are DIFFERENT LENGTHS — the railgun measures 185.1 cm nose to tail, the SMG 128.5 cm. Using
	// one scale for both would draw the SMG at three quarters the pistol's on-screen size, which reads
	// as a toy rather than as the other half of the same armoury. Matching the DRAWN LENGTH is what
	// makes them siblings: 185.1 x 0.22 = 40.7 uu of camera depth against 128.5 x 0.30 = 38.5 uu.
	//
	// MEASURED, against the two checks the file header requires of every viewmodel constant, by
	// walking all 1993 vertices of the four imported meshes through the rig transform:
	//
	//   DEPTH    deepest vertex at 82.9 uu -> 33.2 uu drawn at FirstPersonScale 0.40, inside the 34 uu
	//            capsule radius, so no part of it can reach a surface the body cannot already stand
	//            against. (The shipped RAILGUN measures 34.1 uu on the same walk, i.e. it is 0.1 uu
	//            over the rule it documents. The SMG is the first rig in this project to keep it.)
	//            Shallowest vertex 44.4 uu -> 17.8 uu, clear of the 10 uu near plane.
	//   FRAMING  the highest point of the gun sits 31.5% of the frame's half-height BELOW the
	//            crosshair (the pistol's is 35.8%); the requirement is a quarter. The nearest the
	//            silhouette comes to the centre line is 13.2% of the half-width right of it, so no
	//            part of the weapon ever touches the reticle.
	//   AND IT LANDS WHERE THE PISTOL LANDS. The muzzle projects to 19% right of centre and 40% below
	//            it; the railgun's is 18% / 41%. Two different guns, the same hold.
	//
	// PLACEMENT IS DERIVED, NOT CHOSEN. SmgOrigin = (right hand) - SmgScale * (grip landmark), the
	// identical construction RailgunOrigin uses, so the mesh's own grip lands in the hand that is
	// already there and the right hand does not move between the two weapons:
	//
	//   grip     mesh-local (-30.0, 0, -8.5) uu  ->  rig (-0.8, 0, -4.6), the right hand
	//   muzzle   mesh-local ( 58.8, 0, +4.5) uu  ->  rig 25.8 uu out, vs the railgun's 29.4
	//
	// The grip and muzzle landmarks are the ones spec §2 names (+0.300 m and -0.588 m along the
	// source's -Z), read out of railgun_smg_manifest.json rather than out of the prose: the kit's own
	// README says the aperture is at -0.59 m, and the mesh says -0.588. 2 mm, but the measured one
	// wins, which is the rule §1 states.
	constexpr float SmgScale = 0.30f;
	const FVector SmgGripLocal(-30.0f, 0.f, -8.5f);
	const FVector SmgMuzzleLocal(58.8f, 0.f, 4.5f);
	const FVector SmgOrigin(8.2f, 0.f, -2.05f);

	// THE SUPPORT HAND NEEDS NO SMG CONSTANT OF ITS OWN, AND THAT IS A MEASUREMENT RATHER THAN
	// LAZINESS. It only exists at all in the legacy build — under the v28 dual-wield switch the left
	// hand holds the knife and touches neither gun — and RailgunLeftHand's rig point (12.4, -2.6,
	// -5.5) maps back into SMG mesh space at (14.0, -8.7, -11.5) cm. The SMG body's underside runs
	// dead flat at z = -8.46 cm across the whole span x = 10..30 cm, so that pose puts the fist 3.0 cm
	// (0.9 uu) under a real handguard and 8.7 cm off the barrel axis, on a body that is 13.2 cm
	// half-wide there. That is the same "closed around the guard, not resting on top of it"
	// relationship RailgunLeftHand has with the railgun's own foregrip. One pose, correct for both
	// guns; a second constant would only be a second thing to keep in step, which is exactly the
	// argument the DualWield block below makes for its own single pose.

	// --- Fire: 0.100 s, LOOPING  (spec v30 §3) -----------------------------------------------------
	//
	// One shot at 600 RPM, and the kit is explicit that the first and last keys match so it cycles
	// without a hitch. Every curve below therefore returns EXACTLY ZERO at phase 1.0 — a cycle that
	// ended anywhere else would step on the next shot at 0.1 s intervals and read as a stutter.
	//
	// NOTE THE UNITS. The kit quotes millimetres of MESH motion ("+/-42 mm", "20 mm back"), which
	// after the x100 import are mesh centimetres, which is what these constants are. They are
	// multiplied by SmgScale wherever they are applied, exactly as RailgunRailThrowUU is, so
	// retuning the rig's size cannot silently change how far the walls travel relative to the gun.
	/**
	 * The clip as authored. Used ONLY as the fallback length when there is no weapon component to ask
	 * — NotifyWeaponFired takes the real cadence off UTraceWeaponComponent::GetFireInterval(), so a
	 * character whose ability makes this gun fire at 990 RPM gets a cycle that finishes in time.
	 */
	constexpr float SmgFireClipSeconds = 0.100f;

	/**
	 * The cycle is the WHOLE interval, unlike the railgun's 0.9. The railgun shortens its tail so the
	 * rails are provably shut before the next round; this clip was authored to loop, its first and
	 * last keys match, and every curve above lands on exactly zero — so ending the cycle early would
	 * introduce the very discontinuity the fraction exists to avoid.
	 */
	constexpr float SmgFireIntervalFraction = 1.0f;

	/** Floor, so a pathological ini or an ability stack cannot ask for a zero-length cycle. */
	constexpr float SmgFireMinSeconds = 0.03f;
	constexpr float SmgWallThrowUU = 4.2f;            // ±42 mm apart on the shot frame
	constexpr float SmgRecoilBackUU = 2.0f;           // 20 mm back
	constexpr float SmgRecoilPitchDegrees = 2.58f;    // -0.045 rad, converted once and written down

	/**
	 * The elastic settle, as an envelope and a ringing frequency.
	 *
	 * "Snap apart on the shot frame, then elastic-settle" is a decaying oscillation, and the two
	 * numbers that describe one are how fast it dies and how many times it crosses zero on the way.
	 * Damping 3.2 leaves the second overshoot at ~4% of the first; ringing 1.25 puts one full swing
	 * and a quarter inside the cycle, so the walls visibly come back THROUGH the closed position
	 * rather than easing onto it. The (1 - phase) factor is what forces the exact zero at the end.
	 */
	constexpr float SmgSettleDamping = 3.2f;
	constexpr float SmgSettleRinging = 1.25f;

	// --- Reload  (spec v30 §3) ---------------------------------------------------------------------
	//
	// *** THE CONFLICT, RESOLVED AND STATED: THE MOTION IS TIME-STRETCHED TO THE GAMEPLAY RELOAD. ***
	//
	// The kit authored a 0.800 s reload. Demo 25 set the SMG's gameplay reload to 1.300 s
	// (SmgReloadSeconds, Config/DefaultGame.ini), which is newer and explicit, so 1.3 s wins and the
	// gameplay number is NOT touched here. That leaves the two choices §3 names, and this is the one
	// taken and why:
	//
	//   TIME-STRETCH (chosen). The three beats keep their PROPORTIONS — 32.5% out, 50% up, 17.5%
	//   seating — and the whole motion is played across whatever the weapon's own reload actually is.
	//   The magazine is therefore moving for exactly as long as the player is waiting, and the seat
	//   lands on the frame the trigger comes back.
	//
	//   PLAY IT AUTHORED AND HOLD (rejected). The cell would seat at 0.80 s and the gun would then
	//   sit visibly finished for half a second while the trigger was still dead. That is a viewmodel
	//   disagreeing with the weapon, which is the one thing §3 says must not happen — and it is worse
	//   than a slow reload, because the player would read the seat as "ready" and pull.
	//
	// The fractions are the authored beats divided by the authored length, so if the art is ever
	// re-timed these follow it: 0.26/0.80 and 0.66/0.80.
	constexpr float SmgReloadAuthoredSeconds = 0.800f;
	constexpr float SmgReloadOutFraction = 0.26f / 0.80f;    // cell drops away
	constexpr float SmgReloadUpFraction = 0.66f / 0.80f;     // new cell rides up
	constexpr float SmgMagDropUU = 30.f;                     // 300 mm, in mesh centimetres

	/** How far the cell is still short when the ride-up ends, i.e. the size of the seating bump. */
	constexpr float SmgMagSeatBumpUU = 2.4f;

	// --- Emissive  (spec v30 §4) -------------------------------------------------------------------
	//
	// Multipliers on the material's own authored emissive, which is what the EmissiveIntensity scalar
	// on M_TraceRailgun means and what the pistol's fire curve already feeds it. Rest is NOT 1.0 here:
	// the SMG idles hot at 1.8x, so these have to be written at build time as well as per frame, or
	// a gun that is never fired sits at the material's 1.0 default.
	constexpr float SmgCyanRest = 1.8f;
	constexpr float SmgCyanPeak = 4.8f;

	/**
	 * The ammo readout. 1.4x at a full 40, 0.35x at empty, linear between — driven by the CLIP, not
	 * by the fire clip, so the cell drains as you shoot and refills when the magazine seats.
	 */
	constexpr float SmgAmberFull = 1.4f;
	constexpr float SmgAmberEmpty = 0.35f;

	// --- The three motion curves, written down as functions --------------------------------------
	//
	// EVERY ONE OF THEM IS EXACTLY 1.0 AT PHASE 0 AND EXACTLY 0.0 AT PHASE 1, and that is not
	// tidiness — it is what "first and last keys match, so it cycles without a hitch" means when the
	// clip is code. A curve that ended at 0.03 would leave the walls 0.13 mm open at 600 RPM forever.

	/**
	 * The elastic settle, for the rail walls. 1.0 on the shot frame, ringing through zero on the way
	 * back to a hard 0.0 at the end of the cycle.
	 *
	 * Three factors, each doing one job: (1 - T) forces the exact zero at the end, the exponential is
	 * the decay envelope, and the cosine is the ring. Multiplying rather than adding them is what
	 * keeps the overshoots shrinking instead of the tail wagging.
	 */
	inline float SmgElasticSettle(float Phase)
	{
		const float T = FMath::Clamp(Phase, 0.f, 1.f);
		return (1.f - T) * FMath::Exp(-SmgSettleDamping * T)
			* FMath::Cos(2.f * PI * SmgSettleRinging * T);
	}

	/**
	 * The flash fall, for circuit_cyan and for the receiver's recoil. 1.0 on the shot frame, 0.0 at
	 * the end, and MONOTONE — a glow that rang would strobe, and a receiver that rang would buzz.
	 * Quadratic rather than linear: most of the drop happens in the first third of the cycle, which
	 * is what a discharge looks like.
	 */
	inline float SmgFlashFall(float Phase)
	{
		const float T = FMath::Clamp(Phase, 0.f, 1.f);
		return (1.f - T) * (1.f - T);
	}

	/**
	 * The magazine's height, in MESH centimetres below seated, for a reload phase in 0..1. Three
	 * beats, as authored, at the authored proportions:
	 *
	 *   0.000 .. 0.325   the spent cell falls away, eased IN — a magazine released under gravity
	 *                    accelerates, it does not slide out at a constant rate.
	 *   0.325 .. 0.825   the new cell rides up, eased OUT, stopping SmgMagSeatBumpUU short.
	 *   0.825 .. 1.000   IT SEATS WITH A BUMP: that last 2.4 cm closes, overshoots 1.2 cm into the
	 *                    well and rebounds to exactly zero. The overshoot is the bump; without it
	 *                    the third beat is just a slower second beat and the kit calls for three.
	 */
	inline float SmgMagDrop(float Phase)
	{
		const float T = FMath::Clamp(Phase, 0.f, 1.f);

		if (T <= SmgReloadOutFraction)
		{
			const float A = T / SmgReloadOutFraction;
			return SmgMagDropUU * A * A;
		}
		if (T <= SmgReloadUpFraction)
		{
			const float A = (T - SmgReloadOutFraction) / (SmgReloadUpFraction - SmgReloadOutFraction);
			const float Eased = 1.f - (1.f - A) * (1.f - A);
			return FMath::Lerp(SmgMagDropUU, SmgMagSeatBumpUU, Eased);
		}

		const float A = (T - SmgReloadUpFraction)
			/ FMath::Max(1.f - SmgReloadUpFraction, KINDA_SMALL_NUMBER);
		return SmgMagSeatBumpUU * (1.f - A) * FMath::Cos(2.f * PI * A);
	}
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
	 * The two Tron materials, shared with ATraceArenaBuilder.
	 *
	 * COMMITTED FIRST, LEGACY SECOND — and getting this wrong is invisible on a machine that has run
	 * the generator. Spec v17 §3 promoted these to /Game/Trace/Materials/Parents, which IS in the
	 * repository; /Game/Generated is the pre-v17 generator output and is GITIGNORED. The arena
	 * builder was migrated to prefer the committed pair, and this file was missed because it belongs
	 * to no agent's slice — so on a fresh clone the ARENA rendered correctly while every CHARACTER
	 * silently degraded to BasicShapeMaterial. That asymmetry is the giveaway if it ever regresses.
	 *
	 * A miss on BOTH is still tolerated: MakeViewModelMaterials() falls back to BasicShapeMaterial,
	 * because a flat-shaded gun beats no gun.
	 */
	const TCHAR* const SurfaceMaterialPath = TEXT("/Game/Trace/Materials/Parents/M_TraceSurface.M_TraceSurface");
	const TCHAR* const NeonMaterialPath = TEXT("/Game/Trace/Materials/Parents/M_TraceNeon.M_TraceNeon");

	/** Pre-v17 generator output. Gitignored, so present only on a machine that ran the generator. */
	const TCHAR* const LegacySurfaceMaterialPath = TEXT("/Game/Generated/Materials/M_TraceSurface.M_TraceSurface");
	const TCHAR* const LegacyNeonMaterialPath = TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon");

	/**
	 * The railgun. COMMITTED, unlike the Mannequin — this is our own art, not Epic's, so it lives in
	 * the repository (through LFS) and needs no import step on a fresh clone.
	 *
	 * Authored in Art/Railgun/railgun.glb and turned into these assets by Scripts/import-railgun.sh.
	 * Three meshes rather than one because the fire animation throws the two rail walls apart; each
	 * wall is baked around its own hinge so rotating the component swings the muzzle end outward.
	 *
	 * All optional. If any of the three misses, EnsureViewModelBuilt() falls back to the procedural
	 * cube gun and says so — the same contract every other asset in this file honours. Launch with
	 * -TraceNoRailgun to take that path deliberately.
	 */
	const TCHAR* const RailgunBodyMeshPath = TEXT("/Game/Trace/Weapons/Meshes/SM_Railgun_Body.SM_Railgun_Body");
	const TCHAR* const RailgunRailLeftMeshPath = TEXT("/Game/Trace/Weapons/Meshes/SM_Railgun_RailL.SM_Railgun_RailL");
	const TCHAR* const RailgunRailRightMeshPath = TEXT("/Game/Trace/Weapons/Meshes/SM_Railgun_RailR.SM_Railgun_RailR");

	/**
	 * The SMG. Same contract, same folder, same import script (`Scripts/import-railgun.sh --rig smg`),
	 * authored in Art/Smg/railgun_smg.glb.
	 *
	 * FOUR meshes rather than three because this export carries authored pivot nodes — the railgun's
	 * did not, and its walls had to be inferred by name. The split here is exact:
	 * wall_pivot_left/right own the two rail walls, mag_pivot owns the 40-round cell, and everything
	 * else is the body. All three pivots sit at (0,0,0) relative to the weapon root, so unlike the
	 * railgun there are no hinge offsets to carry: every part's rest position is SmgOrigin itself.
	 *
	 * All four optional, exactly like the railgun's three: a miss on any one leaves the SMG slot
	 * showing the pistol rig and says why. -TraceNoSmg forces that path deliberately, and
	 * -TraceNoRailgun (which already forced it for the pistol) suppresses both.
	 */
	const TCHAR* const SmgBodyMeshPath = TEXT("/Game/Trace/Weapons/Meshes/SM_RailgunSmg_Body.SM_RailgunSmg_Body");
	const TCHAR* const SmgWallLeftMeshPath = TEXT("/Game/Trace/Weapons/Meshes/SM_RailgunSmg_WallLeft.SM_RailgunSmg_WallLeft");
	const TCHAR* const SmgWallRightMeshPath = TEXT("/Game/Trace/Weapons/Meshes/SM_RailgunSmg_WallRight.SM_RailgunSmg_WallRight");
	const TCHAR* const SmgMagMeshPath = TEXT("/Game/Trace/Weapons/Meshes/SM_RailgunSmg_Mag.SM_RailgunSmg_Mag");

	/** Material slot names baked into the meshes, used to find the two glowing slots to animate.
	  * The SMG's are the same two FNames — the import writes the raw glTF material names undecorated
	  * on both weapons, which is why one pair of constants serves both. */
	const FName RailgunCyanSlot(TEXT("circuit_cyan"));
	const FName RailgunAmberSlot(TEXT("core_amber"));

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

	/**
	 * Push the player's saved FIELD OF VIEW onto this pawn's camera.
	 *
	 * WHY THIS LIVES HERE AND NOT ONLY IN THE SETTINGS CLASS. UTraceGameUserSettings also re-applies
	 * the FOV from a 1 Hz ticker, which is what made the row work before this pawn knew about it —
	 * but a ticker can be up to a second late, and the second after a respawn is exactly when a
	 * player is re-orienting. Called from BeginPlay and again on the frame the pawn becomes locally
	 * controlled (on a client the controller arrives by replication AFTER the pawn, so BeginPlay
	 * alone is not enough), the value is correct on the first rendered frame of every life.
	 *
	 * Only ever writes a locally controlled human's camera: no other pawn's UCameraComponent is
	 * rendered through, and a bot's FOV is not the local player's business.
	 *
	 * Does nothing when the settings object is unavailable (dedicated server, early startup) — the
	 * constructor's shipped 95 stands, which is the correct fallback. It deliberately leaves
	 * FirstPersonFieldOfView alone: that is the view model's own projection, and widening the world
	 * must not stretch the gun.
	 */
	void ApplySavedFieldOfView(const ATraceCharacter& TraceChar, UCameraComponent* InCamera)
	{
		if (InCamera == nullptr || !IsLocalPlayerPawn(TraceChar))
		{
			return;
		}

		if (const UTraceGameUserSettings* const Video = UTraceGameUserSettings::Get())
		{
			InCamera->SetFieldOfView(Video->GetFieldOfView());
		}
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

	// MEASURED (spec v5 section 0 acceptance run): a joining client saw only 5 of the 10 characters.
	// APawn's default NetCullDistanceSquared is 225000000 = 15000 uu, and this field is 33600 x 9600
	// - a corner-to-corner distance of ~34900 uu - so on a full-length field half the roster is
	// never relevant and simply is not in the client's world. Trace's whole premise is reading an
	// enemy's trail from across the arena, so there is no distance at which a player stops mattering.
	//
	// 40000 uu covers the diagonal with headroom. Squared it is 1.6e9, which is exact in a float
	// (it is 1.6e9 < 2^31 and the value has few significant digits), so no precision game is being
	// played here. Ten pawns on one field is a trivial relevancy set; this is not a bandwidth risk.
	//
	// Written through the SETTER, not the field. Direct access to NetCullDistanceSquared is
	// UE_DEPRECATED(5.5) and clang here reports it; Unreal builds this module warnings-as-errors on
	// MSVC, so the field form is a Windows build break waiting to happen (and the deprecation note
	// says it stops compiling outright next release).
	SetNetCullDistanceSquared(40000.f * 40000.f);

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

	// The SHIPPED default only. The player's own value (VIDEO page -> FIELD OF VIEW, persisted in
	// GameUserSettings.ini) is pushed on top of this by ApplySavedFieldOfView() from BeginPlay and
	// again the frame this pawn becomes locally controlled — see the note there. It is deliberately
	// NOT read here: this runs during CDO construction, before the engine has created the
	// UTraceGameUserSettings, so a lookup here would either be null or force the settings object
	// into existence early. TraceCharacterLayout::CameraFOV and
	// UTraceGameUserSettings::DefaultFieldOfView are the same 95, so a player who never touches the
	// row never sees a change.
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

	// The Tron materials, resolved the same way ATraceArenaBuilder resolves them: constructor-time
	// finders so the reference lands on the CDO and the cooker follows it, COMMITTED pair first and
	// the gitignored legacy pair only as a fallback, and a tolerated miss on both that
	// MakeViewModelMaterials() turns into a BasicShapeMaterial fallback.
	//
	// Separate static finders per path rather than a loop: ConstructorHelpers::FObjectFinder must be
	// static and is only legal during construction, so each candidate needs its own.
	{
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> SurfaceFinder(TraceCharacterAssets::SurfaceMaterialPath);
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TraceCharacterAssets::NeonMaterialPath);
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> LegacySurfaceFinder(TraceCharacterAssets::LegacySurfaceMaterialPath);
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> LegacyNeonFinder(TraceCharacterAssets::LegacyNeonMaterialPath);

		if (SurfaceFinder.Succeeded())
		{
			SurfaceMaterial = SurfaceFinder.Object;
		}
		else if (LegacySurfaceFinder.Succeeded())
		{
			SurfaceMaterial = LegacySurfaceFinder.Object;
		}

		if (NeonFinder.Succeeded())
		{
			NeonMaterial = NeonFinder.Object;
		}
		else if (LegacyNeonFinder.Succeeded())
		{
			NeonMaterial = LegacyNeonFinder.Object;
		}
	}

	// The railgun's three meshes. Constructor-time finders for the same reason as everything above:
	// the reference lands on the CDO, so the cooker packages the art without anyone maintaining a
	// list of "additional assets to cook". A miss is not an error here — EnsureViewModelBuilt()
	// checks all three and builds the cube gun instead if any is absent.
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> RailgunBodyFinder(TraceCharacterAssets::RailgunBodyMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> RailgunRailLeftFinder(TraceCharacterAssets::RailgunRailLeftMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> RailgunRailRightFinder(TraceCharacterAssets::RailgunRailRightMeshPath);

		if (RailgunBodyFinder.Succeeded())
		{
			RailgunBodyMesh = RailgunBodyFinder.Object;
		}
		if (RailgunRailLeftFinder.Succeeded())
		{
			RailgunRailLeftMesh = RailgunRailLeftFinder.Object;
		}
		if (RailgunRailRightFinder.Succeeded())
		{
			RailgunRailRightMesh = RailgunRailRightFinder.Object;
		}
	}

	// The SMG's four meshes, on the identical contract: a CDO reference so the cooker packages them,
	// and a miss on any one is a fallback rather than an error. See BuildSmgViewModel().
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SmgBodyFinder(TraceCharacterAssets::SmgBodyMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SmgWallLeftFinder(TraceCharacterAssets::SmgWallLeftMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SmgWallRightFinder(TraceCharacterAssets::SmgWallRightMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SmgMagFinder(TraceCharacterAssets::SmgMagMeshPath);

		if (SmgBodyFinder.Succeeded())
		{
			SmgBodyMesh = SmgBodyFinder.Object;
		}
		if (SmgWallLeftFinder.Succeeded())
		{
			SmgWallLeftMesh = SmgWallLeftFinder.Object;
		}
		if (SmgWallRightFinder.Succeeded())
		{
			SmgWallRightMesh = SmgWallRightFinder.Object;
		}
		if (SmgMagFinder.Succeeded())
		{
			SmgMagMesh = SmgMagFinder.Object;
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

	// The player's FOV, on the first frame of the life rather than up to a second into it.
	ApplySavedFieldOfView(*this, Camera);

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

	// SPEC v19 §4.1, first, and server-only. It is here rather than in the game mode because the
	// question is about ONE pawn's transform and this is the function that already runs once per pawn
	// per frame; a game-mode sweep would be a second iteration over the same actors to ask the same
	// thing. Two branches on a client, where it returns immediately.
	ServerCheckArenaBounds();

	// The rest of this function is presentation. See UpdateViewBlend().
	//
	// The rotation model is re-asserted here rather than only on possession because on a client the
	// controller arrives by replication, after the pawn: there is no PossessedBy() on that machine.
	// Both calls early-out on "nothing changed", so the steady-state cost is two branches.
	const bool bLocalPlayer = IsLocalPlayerPawn(*this);
	const bool bJustBecameLocal = bLocalPlayer && !bWasLocallyControlled;
	bWasLocallyControlled = bLocalPlayer;

	// The client path: BeginPlay ran before this pawn had a controller, so that call declined.
	if (bJustBecameLocal)
	{
		ApplySavedFieldOfView(*this, Camera);
	}

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

// =================================================================================================
// SPEC v19 §4.1 — OUT OF BOUNDS: DIE AND RESPAWN
//
// Verbatim: "If a player ever goes out of bounds of the arena, they should die and respawn."
//
// WHAT WAS ALREADY THERE, AND WHY IT IS NOT ENOUGH. FellOutOfWorld() below already turns the engine's
// KillZ into a real death, so falling THROUGH the floor was covered. Nothing covered leaving the
// arena sideways — through a seam, over a wall, out past a goal alcove — and the two characters
// landing this pass make that dramatically more reachable: Lily gets five seconds of flight and
// Mortimer gets a mantle. The spec's own warning is the design constraint here.
//
// *** THIS IS DELIBERATELY A HORIZONTAL TEST, PLUS A FLOOR. THERE IS NO CEILING BY DEFAULT. ***
// "Out of bounds" has to mean GENUINELY OUTSIDE THE ARENA and not "higher than usual", because the
// arena has no roof and a flying Lily above the wall line is still over the pitch, still inside the
// playing area seen from above, and still coming down. A ceiling that killed her would be a new bug
// wearing this feature's clothes. Trace.Bounds.CeilingMargin exists for a designer who disagrees and
// is 0 (off) as shipped.
//
// The margins are generous on purpose. This rule's failure modes are wildly asymmetric: a boundary
// that is slightly too tight kills players who are standing somewhere legal, which is unplayable; a
// boundary that is slightly too loose lets somebody stand in a void for another half second before
// dying, which nobody will ever notice.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarBoundsEnabled(
	TEXT("Trace.Bounds.Enabled"), 1,
	TEXT("SPEC v19 §4.1. 1 (shipped): a player genuinely outside the arena dies and respawns, credited "
	     "to nobody. 0: removes the rule, which is the A/B arm Trace.Bounds.Verify must go RED on."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarBoundsMarginXY(
	TEXT("Trace.Bounds.MarginXY"), 1200.f,
	TEXT("SPEC v19 §4.1. How far OUTSIDE the arena footprint, in uu, a player may be before it counts "
	     "as out of bounds. Generous by design: goal alcoves and the standoff shells all sit at or just "
	     "past the wall line, and killing somebody standing in a legal alcove is far worse than letting "
	     "somebody hang in a void for another half second."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarBoundsMarginBelow(
	TEXT("Trace.Bounds.MarginBelow"), 800.f,
	TEXT("SPEC v19 §4.1. How far BELOW the arena floor, in uu, before a player is out of bounds. This "
	     "usually fires before the engine's KillZ does, which is the point: it produces a death that "
	     "reads as 'you left the arena' rather than one that reads as the world ending."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarBoundsCeilingMargin(
	TEXT("Trace.Bounds.CeilingMargin"), 0.f,
	TEXT("SPEC v19 §4.1. 0 (shipped, and the default on purpose): THERE IS NO CEILING. Lily flies and "
	     "Mortimer mantles, so 'above the wall line' is a legal place to be and must not be a death. "
	     "Set it to a positive number of uu above the wall tops to add one anyway."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarBoundsGraceSeconds(
	TEXT("Trace.Bounds.GraceSeconds"), 0.35f,
	TEXT("SPEC v19 §4.1. How long a player must be continuously outside before the rule kills them. "
	     "Not a hair trigger: a depenetration push, an unratified teleport and the frame between a "
	     "spawn transform and its first movement update all look like one frame out of bounds."),
	ECVF_Default);

/** Counts real out-of-bounds deaths. Read by Trace.Bounds.Verify so the harness is evidence. */
static int32 GTraceOutOfBoundsDeaths = 0;

/** The FName the death panel and the kill credit see. "the arena" is what the panel prints for it. */
static const FName GTraceOutOfBoundsCause(TEXT("OutOfBounds"));

bool ATraceCharacter::IsLocationOutOfArenaBounds(const UWorld* World, const FVector& Location, FString& OutReason)
{
	OutReason.Reset();

	if (World == nullptr || CVarBoundsEnabled.GetValueOnAnyThread() == 0)
	{
		return false;
	}

	const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
	if (Arena == nullptr)
	{
		// No arena means no bounds to be outside of — a menu map, the practice range if it is carved
		// somewhere else, a test fixture. Refusing to guess is the only safe answer: a rule that kills
		// on a map it does not understand is a rule that deletes somebody else's feature.
		return false;
	}

	const FBox Field = Arena->GetFieldBounds();
	if (Field.IsValid == 0)
	{
		return false;
	}

	const double MarginXY = FMath::Max(0.f, CVarBoundsMarginXY.GetValueOnAnyThread());
	const double MarginBelow = FMath::Max(0.f, CVarBoundsMarginBelow.GetValueOnAnyThread());
	const double CeilingMargin = CVarBoundsCeilingMargin.GetValueOnAnyThread();

	if (Location.X < Field.Min.X - MarginXY || Location.X > Field.Max.X + MarginXY)
	{
		OutReason = FString::Printf(TEXT("%.0f uu past the end wall"),
			(Location.X < Field.Min.X) ? (Field.Min.X - Location.X) : (Location.X - Field.Max.X));
		return true;
	}

	if (Location.Y < Field.Min.Y - MarginXY || Location.Y > Field.Max.Y + MarginXY)
	{
		OutReason = FString::Printf(TEXT("%.0f uu past the sideline wall"),
			(Location.Y < Field.Min.Y) ? (Field.Min.Y - Location.Y) : (Location.Y - Field.Max.Y));
		return true;
	}

	if (Location.Z < Field.Min.Z - MarginBelow)
	{
		OutReason = FString::Printf(TEXT("%.0f uu below the floor"), Field.Min.Z - Location.Z);
		return true;
	}

	// See the block above: OFF by default, and that is a decision rather than an oversight.
	if (CeilingMargin > 0.f && Location.Z > Field.Max.Z + CeilingMargin)
	{
		OutReason = FString::Printf(TEXT("%.0f uu above the wall tops"), Location.Z - Field.Max.Z);
		return true;
	}

	return false;
}

void ATraceCharacter::ServerCheckArenaBounds()
{
	UWorld* const MyWorld = GetWorld();
	if (!HasAuthority() || MyWorld == nullptr || !IsAlive() || Health == nullptr)
	{
		OutOfBoundsSinceServerTime = -1.f;
		return;
	}

	FString Reason;
	if (!IsLocationOutOfArenaBounds(MyWorld, GetActorLocation(), Reason))
	{
		OutOfBoundsSinceServerTime = -1.f;
		return;
	}

	const float NowSeconds = static_cast<float>(MyWorld->GetTimeSeconds());
	if (OutOfBoundsSinceServerTime < 0.f)
	{
		OutOfBoundsSinceServerTime = NowSeconds;
		return;   // The grace starts here. See the field's comment for what it is protecting against.
	}

	if ((NowSeconds - OutOfBoundsSinceServerTime) < FMath::Max(0.f, CVarBoundsGraceSeconds.GetValueOnAnyThread()))
	{
		return;
	}

	OutOfBoundsSinceServerTime = -1.f;
	++GTraceOutOfBoundsDeaths;

	UE_LOG(LogTraceGame, Display,
		TEXT("[Bounds] spec v19 §4.1: %s went OUT OF BOUNDS at %s (%s) - killing them. Credited to "
		     "nobody; their ability cooldowns keep running through it."),
		*GetName(), *GetActorLocation().ToCompactString(), *Reason);

	// Kill(), not ApplyDamage(), for the same reason FellOutOfWorld() uses it: a Core carrier is
	// invulnerable to damage, and a carrier who walks out of the world would otherwise take the Core
	// with them permanently. This is a world death, so it goes through the door the trail uses.
	//
	// *** THE KILLER IS DELIBERATELY nullptr. *** That is what makes it uncreditable:
	// ATraceGameMode::NotifyCharacterDied reads a null killer as bSelfKill, which skips the Kills
	// column entirely and makes the victim's death panel say "by the arena". Passing GetController()
	// would reach the same place today, but only because a self-kill happens to be excluded too —
	// null says the thing we actually mean.
	Health->Kill(nullptr, GTraceOutOfBoundsCause);
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

bool ATraceCharacter::AreWeaponActionsBlocked() const
{
	// Spec v10 §6. See the header for why this is not simply IsDashing() at the call site.
	const UTraceCharacterMovementComponent* Movement = GetTraceMovement();
	return Movement != nullptr && Movement->AreWeaponActionsBlocked();
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
	// Resolved through the trail component rather than off UTraceSettings::TrailHeight directly, so
	// that the height the camera clears is the height the trail is ACTUALLY built at — the console
	// override Trace.Trail.Height moves both together, and the two can never disagree.
	//
	// UTraceTrailComponent builds its wall centred on the carrier's actor location and rides a cap
	// strip of clamp(Height * 0.12, 14, 42) on top of it, so the highest lit surface sits half the
	// height plus half the cap above the actor centre — and TargetOffset.Z is measured from that
	// same actor centre.
	const float TrailHeight = FMath::Max(0.f, UTraceTrailComponent::GetTraceTrailHeight());
	const float TrailTopAboveCentre = TrailHeight * 0.5f + FMath::Clamp(TrailHeight * 0.12f, 14.f, 42.f) * 0.5f;

	// Max, not a plain sum, and the floor is CarryPivotZ rather than the generic ThirdPersonPivotZ.
	// Spec v7 §3 cut the trail to a third of its height, and without this floor that visibility change
	// would have dragged the approved carry framing down 68uu with it. See CarryPivotZ for the whole
	// argument and for the one-line way to reverse this decision.
	return FMath::Max(
		TraceCharacterLayout::CarryPivotZ,
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
		/** Part of the WEAPON (skipped when the railgun was built) rather than part of the HANDS. */
		bool bWeapon;
	};

	// The railgun replaces the twelve gun parts if its art resolved. The hands and arms below are
	// built either way — they are what holds whichever weapon won.
	const bool bRailgun = BuildRailgunViewModel();

	// SPEC v30 §2 — and the SMG is built BESIDE the pistol, not instead of it. Both rigs exist from
	// this moment on and UpdateWeaponSelection() decides which one is drawn; see the declaration for
	// why a swap must not be allowed to construct geometry. A false here is not an error: the `3`
	// slot then shows whichever pistol rig this pawn got, and says so once.
	BuildSmgViewModel();

	const FViewModelPart Parts[] =
	{
		// The gun. A slide over a frame over a raked grip: three masses, which is what makes a
		// blocky shape read as a handgun rather than as a brick.
		{ TEXT("VMSlide"),      false, FVector(9.0f, 0.f, 2.4f),    FRotator::ZeroRotator,        FVector(21.0f, 4.6f, 5.2f),  false, true  },
		{ TEXT("VMFrame"),      false, FVector(7.0f, 0.f, -1.6f),   FRotator::ZeroRotator,        FVector(16.5f, 4.2f, 4.4f),  false, true  },
		{ TEXT("VMGrip"),       false, FVector(-1.6f, 0.f, -8.2f),  FRotator(14.f, 0.f, 0.f),     FVector(5.6f, 4.0f, 13.5f),  false, true  },
		{ TEXT("VMGuard"),      false, FVector(3.2f, 0.f, -4.6f),   FRotator::ZeroRotator,        FVector(5.6f, 3.0f, 1.4f),   false, true  },

		// Light channels. The muzzle ring is a cylinder turned to point down the barrel (pitch 90
		// swings the shape's own +Z axis onto +X), and it is the piece that makes the gun read as a
		// weapon at a glance: a lit circle where the shot comes out.
		{ TEXT("VMMuzzle"),     true,  TraceCharacterLayout::CubeGunMuzzle, FRotator(90.f, 0.f, 0.f), FVector(5.8f, 5.8f, 2.2f), true,  true  },
		{ TEXT("VMSlideNeon"),  false, FVector(8.4f, 0.f, 5.3f),    FRotator::ZeroRotator,        FVector(16.5f, 1.8f, 1.5f),  true,  true  },
		{ TEXT("VMSight"),      false, FVector(17.6f, 0.f, 5.6f),   FRotator::ZeroRotator,        FVector(1.4f, 1.4f, 2.0f),   true,  true  },
		{ TEXT("VMSideNeonL"),  false, FVector(7.6f, -2.4f, 0.4f),  FRotator::ZeroRotator,        FVector(12.5f, 0.9f, 1.6f),  true,  true  },
		{ TEXT("VMSideNeonR"),  false, FVector(7.6f, 2.4f, 0.4f),   FRotator::ZeroRotator,        FVector(12.5f, 0.9f, 1.6f),  true,  true  },
		{ TEXT("VMGripNeon"),   false, FVector(-3.9f, 0.f, -8.0f),  FRotator(14.f, 0.f, 0.f),     FVector(1.2f, 3.0f, 9.5f),   true,  true  },
		{ TEXT("VMRearSight"),  false, FVector(1.4f, 0.f, 5.6f),    FRotator::ZeroRotator,        FVector(1.6f, 3.6f, 2.0f),   true,  true  },
		{ TEXT("VMRailNeon"),   false, FVector(6.5f, 0.f, -3.9f),   FRotator::ZeroRotator,        FVector(13.0f, 1.6f, 1.0f),  true,  true  },

		// Hands. Blocks, not fingers: at this scale and this framing a gloved fist is a shape, and
		// trying to model knuckles on a 6 uu cube only produces noise. What DOES read is a lit bar
		// across each one — a knuckle line, in the same language as everything else in this world.
		{ TEXT("VMHandR"),      false, FVector(-0.8f, 0.f, -4.6f),  FRotator(14.f, 0.f, 0.f),     FVector(5.6f, 5.4f, 7.0f),   false, false },
		{ TEXT("VMKnuckleR"),   false, FVector(1.4f, 0.f, -3.2f),   FRotator(14.f, 0.f, 0.f),     FVector(1.2f, 5.0f, 4.6f),   true,  false },
		{ TEXT("VMHandL"),      false, FVector(2.8f, -3.4f, -4.0f), FRotator::ZeroRotator,        FVector(5.0f, 4.8f, 5.6f),   false, false },
		{ TEXT("VMKnuckleL"),   false, FVector(4.9f, -3.4f, -3.0f), FRotator::ZeroRotator,        FVector(1.1f, 4.4f, 3.8f),   true,  false }
	};

	// The right hand does not move: RailgunOrigin was DERIVED from it, so the railgun's grip lands
	// in it by construction. The left hand does — it comes off the cube gun's frame and forward onto
	// the railgun's foregrip, which is 9.6 uu further out.
	constexpr int32 LeftHandIndex = 14;
	constexpr int32 LeftKnuckleIndex = 15;
	checkf(FCString::Strcmp(Parts[LeftHandIndex].Name, TEXT("VMHandL")) == 0
		&& FCString::Strcmp(Parts[LeftKnuckleIndex].Name, TEXT("VMKnuckleL")) == 0,
		TEXT("The viewmodel part table was reordered; the left-hand indices below no longer point "
			 "at the left hand, so the railgun would be held by nothing."));

	// [DUALWIELD] SPEC v28 §10 — the off hand comes off the weapon entirely and takes the knife.
	//
	// ONE `if`, ABOVE THE EXISTING TERNARIES RATHER THAN INSIDE THEM, so the railgun/cube choice below
	// is exactly the code that shipped in v27 and a revert has nothing to unpick. The anchor is
	// remembered on the actor (ViewModelOffHandLocation) because UTraceWeaponComponent needs to hang
	// KnifeViewRoot at the same point and must not carry a second copy of these numbers — that is the
	// duplicate-constant failure this codebase logs by name.
	//
	// READ AT BUILD TIME, WHICH IS ONCE PER PAWN. Flipping Trace.Knife.DualWield mid-session changes
	// every rule immediately but re-poses the hand on the next respawn; the .ini and the launch flag,
	// which are how the switch is actually meant to be thrown, are both set before any pawn exists.
	// Stated so nobody spends time on a "the hand did not move" that is not a bug.
	const bool bDualWieldPose = TraceMelee::IsDualWieldEnabled();

	const FVector LeftHand = bDualWieldPose
		? TraceCharacterLayout::DualWieldLeftHand
		: (bRailgun ? TraceCharacterLayout::RailgunLeftHand : Parts[LeftHandIndex].Location);
	const FVector LeftKnuckle = bDualWieldPose
		? TraceCharacterLayout::DualWieldLeftKnuckle
		: (bRailgun ? TraceCharacterLayout::RailgunLeftKnuckle : Parts[LeftKnuckleIndex].Location);

	ViewModelOffHandLocation = LeftHand;
	bViewModelOffHandFree = bDualWieldPose;

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Parts); ++Index)
	{
		const FViewModelPart& Part = Parts[Index];
		if (bRailgun && Part.bWeapon)
		{
			continue;
		}

		FVector Location = Part.Location;
		if (Index == LeftHandIndex)
		{
			Location = LeftHand;
		}
		else if (Index == LeftKnuckleIndex)
		{
			Location = LeftKnuckle;
		}

		UStaticMeshComponent* Built = AddViewModelPart(Part.bCylinder ? CylinderMesh : CubeMesh, Part.Name,
			Location, Part.Rotation, Part.Size,
			Part.bNeon ? ViewModelNeonMID : ViewModelBodyMID);

		// [SPEC v30 §2] The cube gun's own twelve pieces ARE the pistol rig when the railgun art is
		// missing, so the selector has to be able to hide them for the `1` and `3` states just as it
		// hides the railgun's. Only reached when !bRailgun — the `continue` above skipped them
		// otherwise — so this can never double-add.
		if (Part.bWeapon && Built != nullptr)
		{
			PistolWeaponParts.Add(Built);
		}
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
		{ TEXT("VMForearmL"), TEXT("VMCuffL"), LeftHand,                    FVector(-0.40f, -0.38f, -0.86f), 16.f, 6.7f }
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

	// --- The muzzle marker (spec v26 §4) ---------------------------------------------------------
	//
	// "The bullet tracer animation needs to come from the gun barrel, not above or behind it."
	//
	// The reason it did not is that ATraceTracer had no way to ASK where the barrel was: it carried
	// three hand-tuned camera-space constants (a standoff plus a right/down screen offset) that had
	// been eyeballed against the small cube gun and were never revisited when the 185 cm railgun
	// replaced it. This is the fix at its root — the barrel now says where it is.
	//
	// Parented to the GUN, at the gun's own muzzle landmark, in the gun's own units:
	//   * railgun    -> a child of RailgunBodyPart at RailgunMuzzleLocal, the (107.4, 0, 4.5) cm point
	//                   recorded in railgun_manifest.json. The parent's 0.22 scale is applied by the
	//                   scene graph, so this stays the mesh's real muzzle vertex whatever the rig
	//                   scale becomes.
	//   * cube gun   -> a child of ViewModelRoot at CubeGunMuzzle, the same constant the VMMuzzle ring
	//                   above is placed with.
	//
	// It is deliberately NOT a constant in ATraceTracer, and deliberately NOT the rig root: every
	// motion the gun has — recoil, sway, bob, the slide dip — is a transform on one of its ancestors,
	// and a marker under them inherits all of it with no code that has to remember to.
	if (ViewModelMuzzle == nullptr)
	{
		USceneComponent* MuzzleParent = (RailgunBodyPart != nullptr)
			? static_cast<USceneComponent*>(RailgunBodyPart) : static_cast<USceneComponent*>(ViewModelRoot);
		const FVector MuzzleLocal = (RailgunBodyPart != nullptr)
			? TraceCharacterLayout::RailgunMuzzleLocal : TraceCharacterLayout::CubeGunMuzzle;

		ViewModelMuzzle = NewObject<USceneComponent>(this, TEXT("ViewModelMuzzle"));
		if (ViewModelMuzzle != nullptr)
		{
			ViewModelMuzzle->SetMobility(EComponentMobility::Movable);
			ViewModelMuzzle->SetupAttachment(MuzzleParent);
			ViewModelMuzzle->SetRelativeLocation(MuzzleLocal);
			ViewModelMuzzle->RegisterComponent();
		}
	}

	// [SPEC v30 §5] The SMG's own marker, on the SMG's own body, at the SMG's own aperture. Same
	// arrangement, second gun — GetActiveMuzzleMarker() picks between them by what is DRAWN, so the
	// beam leaves whichever barrel the player is actually looking at with no change in ATraceTracer.
	if (ViewModelSmgMuzzle == nullptr && SmgBodyPart != nullptr)
	{
		ViewModelSmgMuzzle = NewObject<USceneComponent>(this, TEXT("ViewModelSmgMuzzle"));
		if (ViewModelSmgMuzzle != nullptr)
		{
			ViewModelSmgMuzzle->SetMobility(EComponentMobility::Movable);
			ViewModelSmgMuzzle->SetupAttachment(SmgBodyPart);
			ViewModelSmgMuzzle->SetRelativeLocation(TraceCharacterLayout::SmgMuzzleLocal);
			ViewModelSmgMuzzle->RegisterComponent();
		}
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

	// [SPEC v30 §2] Two guns are now on the rig and only one of them may be drawn. Settle that here,
	// before the first frame, so a pawn that spawns holding the SMG never shows a pistol — not even
	// for the one tick it would take Tick() to get around to it.
	UpdateWeaponSelection();

	ApplyTeamColors();

	UE_LOG(LogTraceGame, Verbose,
		TEXT("%s built a first-person viewmodel (%d parts, pistol muzzle on %s, SMG rig %s)."),
		*GetName(), ViewModelParts.Num(),
		(RailgunBodyPart != nullptr) ? TEXT("the railgun body") : TEXT("the fallback rig"),
		(SmgBodyPart != nullptr) ? TEXT("built") : TEXT("ABSENT - the `3` slot falls back"));
}

bool ATraceCharacter::BuildRailgunViewModel()
{
	if (RailgunBodyMesh == nullptr || RailgunRailLeftMesh == nullptr || RailgunRailRightMesh == nullptr)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("Railgun art did not resolve (body=%s railL=%s railR=%s); building the fallback gun. ")
			TEXT("Run Scripts/import-railgun.sh, or `git lfs pull` if this is a fresh clone."),
			RailgunBodyMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			RailgunRailLeftMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			RailgunRailRightMesh != nullptr ? TEXT("ok") : TEXT("MISSING"));
		return false;
	}

	// The same escape hatch -TraceNoCharacterArt gives the Mannequin: a way to SEE the fallback
	// without deleting anything, so "does the fallback still work" is a launch flag and not a
	// destructive experiment.
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoRailgun")))
	{
		UE_LOG(LogTraceGame, Log, TEXT("-TraceNoRailgun: building the procedural gun on purpose."));
		return false;
	}

	const float S = TraceCharacterLayout::RailgunScale;
	const FVector Size(TraceCharacterLayout::ViewModelShapeUnit * S);   // AddViewModelPart divides by the unit

	// The body carries its own materials from the asset, so no MID is passed: AddViewModelPart only
	// overrides slot 0, and this mesh has five slots that are already correct.
	RailgunBodyPart = AddViewModelPart(RailgunBodyMesh, TEXT("VMRailgunBody"),
		TraceCharacterLayout::RailgunOrigin, FRotator::ZeroRotator, Size, nullptr);

	// Each wall's mesh is baked around its hinge, so its rest position is the hinge offset scaled
	// into rig space and its rest rotation is zero. Those offsets came out of the source model and
	// are mirrored, hence one constant and a sign.
	const FVector HingeOffset(-5.0f, 7.8f, 4.5f);
	RailgunRailLeftPart = AddViewModelPart(RailgunRailLeftMesh, TEXT("VMRailgunRailL"),
		TraceCharacterLayout::RailgunOrigin + FVector(HingeOffset.X, -HingeOffset.Y, HingeOffset.Z) * S,
		FRotator::ZeroRotator, Size, nullptr);
	RailgunRailRightPart = AddViewModelPart(RailgunRailRightMesh, TEXT("VMRailgunRailR"),
		TraceCharacterLayout::RailgunOrigin + HingeOffset * S,
		FRotator::ZeroRotator, Size, nullptr);

	if (RailgunBodyPart == nullptr || RailgunRailLeftPart == nullptr || RailgunRailRightPart == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("Railgun parts failed to attach; falling back."));
		RailgunBodyPart = nullptr;
		RailgunRailLeftPart = nullptr;
		RailgunRailRightPart = nullptr;
		return false;
	}

	// Pull the two glowing slots out as dynamic instances. Found BY SLOT NAME, not by index: the
	// import assigns five slots and their order is an artefact of the OBJ writer, not a contract.
	const int32 CyanSlot = RailgunBodyPart->GetMaterialIndex(TraceCharacterAssets::RailgunCyanSlot);
	const int32 AmberSlot = RailgunBodyPart->GetMaterialIndex(TraceCharacterAssets::RailgunAmberSlot);
	if (CyanSlot != INDEX_NONE)
	{
		RailgunCyanMID = RailgunBodyPart->CreateDynamicMaterialInstance(CyanSlot);
	}
	if (AmberSlot != INDEX_NONE)
	{
		RailgunAmberMID = RailgunBodyPart->CreateDynamicMaterialInstance(AmberSlot);
	}
	if (RailgunCyanMID == nullptr || RailgunAmberMID == nullptr)
	{
		// Not fatal: the gun renders, it just will not flare. Loud, because a silently dead effect
		// is the exact failure this project keeps having to hunt down after the fact.
		UE_LOG(LogTraceGame, Warning,
			TEXT("Railgun built, but its glow will not animate: slot '%s'=%d, '%s'=%d."),
			*TraceCharacterAssets::RailgunCyanSlot.ToString(), CyanSlot,
			*TraceCharacterAssets::RailgunAmberSlot.ToString(), AmberSlot);
	}

	// [SPEC v30 §2] The three railgun parts are the PISTOL rig, and the selector has to be able to
	// take them off screen for the `1` (stowed) and `3` (SMG) states.
	PistolWeaponParts.Add(RailgunBodyPart);
	PistolWeaponParts.Add(RailgunRailLeftPart);
	PistolWeaponParts.Add(RailgunRailRightPart);

	UE_LOG(LogTraceGame, Log, TEXT("%s built the railgun viewmodel (muzzle at rig x=%.1f)."),
		*GetName(),
		TraceCharacterLayout::RailgunOrigin.X + TraceCharacterLayout::RailgunMuzzleLocal.X * S);
	return true;
}

// =================================================================================================
// THE SMG RIG  —  spec v30 §2, §3, §4
// =================================================================================================
//
// "Demo 24 added the SMG with no viewmodel of its own — a verifier flagged that nothing on screen
// tells the player which gun they are holding."
//
// THE SHAPE OF THE FIX. Three weapon states exist (1 stows, 2 pistol, 3 SMG) and the knife is in the
// off hand in all three, so the rig has to answer three questions, not two, and the answer has to be
// visible at a glance rather than in an ammo counter:
//
//     1  ->  neither gun. Both weapon rigs off screen, hands and knife only.
//     2  ->  the pistol rig  (the railgun, or the procedural cube gun if the art is missing)
//     3  ->  THE SMG RIG     (or, if THAT art is missing, the pistol rig and a line in the log)
//
// WHY THE MOTION IS CODE AND NOT AN ANIMATION. The GLB is the mesh-only export: `animations: []`.
// The kit's README promises `Fire` and `Reload` clips and warns, in the same paragraph, that the
// stage's bottom-right toolbar exports without them — which is the export we have. Waiting for the
// clips is not an option and is not necessary: §3 states the whole motion in numbers, and the
// pistol's Fire was reproduced from numbers the same way in spec v20. What is reproduced here is
// driven off the WEAPON'S REAL STATE — the fire cycle by NotifyWeaponFired, the reload by the
// component's own replicated deadline — so the picture cannot disagree with the gun.

bool ATraceCharacter::BuildSmgViewModel()
{
	if (SmgBodyMesh == nullptr || SmgWallLeftMesh == nullptr
		|| SmgWallRightMesh == nullptr || SmgMagMesh == nullptr)
	{
		// *** THE FALLBACK MUST SURVIVE. *** A fresh clone that has not run `git lfs pull` has the
		// .uasset files as LFS pointer stubs, so this is the NORMAL first-run state and not an
		// error — it must leave a playable game and a log line that says the one command that fixes
		// it. Warning rather than Error for the same reason the railgun's is: this is a missing
		// optional asset, and the pawn behind it is fully functional.
		UE_LOG(LogTraceGame, Warning,
			TEXT("SMG art did not resolve (body=%s wallL=%s wallR=%s mag=%s); the `3` weapon slot will ")
			TEXT("show the pistol rig instead. Run `Scripts/import-railgun.sh --rig smg`, or ")
			TEXT("`git lfs pull` if this is a fresh clone."),
			SmgBodyMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			SmgWallLeftMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			SmgWallRightMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			SmgMagMesh != nullptr ? TEXT("ok") : TEXT("MISSING"));
		return false;
	}

	// The same escape hatch the railgun has, and -TraceNoRailgun suppresses BOTH: "show me the
	// fallback" is one intention, and having to remember two switches to express it is how a
	// half-forced state gets photographed and reported as a bug.
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoSmg"))
		|| FParse::Param(FCommandLine::Get(), TEXT("TraceNoRailgun")))
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("-TraceNoSmg/-TraceNoRailgun: the `3` slot will show the pistol rig on purpose."));
		return false;
	}

	const float S = TraceCharacterLayout::SmgScale;
	const FVector Size(TraceCharacterLayout::ViewModelShapeUnit * S);   // AddViewModelPart divides by the unit

	// ALL FOUR PARTS SIT AT THE SAME ORIGIN, and that is a property of the export rather than a
	// simplification. The railgun's walls had to be baked around inferred hinges and placed at a
	// mirrored offset; this GLB carries authored pivot nodes (wall_pivot_left/right, mag_pivot) and
	// every one of them is at (0,0,0) relative to the weapon root, so each group's rest transform is
	// SmgOrigin exactly and its motion is a pure delta on top. railgun_smg_manifest.json records
	// `attach_to_body_cm: [0,0,0]` for all three, which is where that claim is checked.
	//
	// No MID is passed on any of them: these meshes carry their own imported material instances on
	// four, two, two and three slots respectively, and AddViewModelPart's override only reaches
	// slot 0.
	SmgBodyPart = AddViewModelPart(SmgBodyMesh, TEXT("VMSmgBody"),
		TraceCharacterLayout::SmgOrigin, FRotator::ZeroRotator, Size, nullptr);
	SmgWallLeftPart = AddViewModelPart(SmgWallLeftMesh, TEXT("VMSmgWallL"),
		TraceCharacterLayout::SmgOrigin, FRotator::ZeroRotator, Size, nullptr);
	SmgWallRightPart = AddViewModelPart(SmgWallRightMesh, TEXT("VMSmgWallR"),
		TraceCharacterLayout::SmgOrigin, FRotator::ZeroRotator, Size, nullptr);
	SmgMagPart = AddViewModelPart(SmgMagMesh, TEXT("VMSmgMag"),
		TraceCharacterLayout::SmgOrigin, FRotator::ZeroRotator, Size, nullptr);

	if (SmgBodyPart == nullptr || SmgWallLeftPart == nullptr
		|| SmgWallRightPart == nullptr || SmgMagPart == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("SMG parts failed to attach; the `3` slot falls back."));
		SmgBodyPart = nullptr;
		SmgWallLeftPart = nullptr;
		SmgWallRightPart = nullptr;
		SmgMagPart = nullptr;
		return false;
	}

	SmgWeaponParts.Add(SmgBodyPart);
	SmgWeaponParts.Add(SmgWallLeftPart);
	SmgWeaponParts.Add(SmgWallRightPart);
	SmgWeaponParts.Add(SmgMagPart);

	// --- The two glowing slots, and the one trap in this whole section ---------------------------
	//
	// *** THE SMG'S GLOW IS SPLIT ACROSS MESHES; THE PISTOL'S IS NOT. *** On the railgun both glowing
	// materials are on the single body mesh, so "two MIDs off the body part" was the whole pattern.
	// Here:
	//
	//     circuit_cyan  ->  Body (slot 2), WallLeft (slot 1), WallRight (slot 1).  NOT on the Mag.
	//     core_amber    ->  Mag (slot 1).                                          NOT on the Body.
	//
	// Copying the pistol's pattern verbatim therefore asks the BODY for core_amber, gets INDEX_NONE,
	// and produces an ammo readout that silently never lights — the failure mode this project keeps
	// having to find after the fact. Found BY SLOT NAME on each component that actually has it, for
	// the same reason the pistol's are: slot ORDER is an artefact of the OBJ writer, not a contract.
	const auto AddGlowMID = [this](UStaticMeshComponent* Part, const FName& Slot) -> UMaterialInstanceDynamic*
	{
		if (Part == nullptr)
		{
			return nullptr;
		}
		const int32 Index = Part->GetMaterialIndex(Slot);
		return (Index != INDEX_NONE) ? Part->CreateDynamicMaterialInstance(Index) : nullptr;
	};

	SmgCyanMIDs.Reset();
	for (UStaticMeshComponent* Part : { SmgBodyPart.Get(), SmgWallLeftPart.Get(), SmgWallRightPart.Get() })
	{
		if (UMaterialInstanceDynamic* MID = AddGlowMID(Part, TraceCharacterAssets::RailgunCyanSlot))
		{
			SmgCyanMIDs.Add(MID);
		}
	}
	SmgAmberMID = AddGlowMID(SmgMagPart, TraceCharacterAssets::RailgunAmberSlot);

	if (SmgCyanMIDs.Num() != 3 || SmgAmberMID == nullptr)
	{
		// Not fatal — the gun renders, it just will not flare or report ammo. Loud, and it names the
		// count rather than just "failed", because 2-of-3 cyan (a wall that stays dark through every
		// shot) is a real and much less obvious failure than 0-of-3.
		UE_LOG(LogTraceGame, Warning,
			TEXT("SMG built, but its glow is incomplete: circuit_cyan MIDs %d/3 (body+both walls), ")
			TEXT("core_amber on the magazine %s."),
			SmgCyanMIDs.Num(), SmgAmberMID != nullptr ? TEXT("ok") : TEXT("MISSING"));
	}

	// THE REST POSE HAS TO BE WRITTEN NOW, not on the first shot. circuit_cyan idles at 1.8x and the
	// imported material instance's own EmissiveIntensity default is 1.0, so a gun that is drawn but
	// never fired would sit visibly dull — and UpdateSmgAnimation only runs while the rig is on
	// screen, so there is no later moment that is guaranteed to happen first.
	for (UMaterialInstanceDynamic* MID : SmgCyanMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), TraceCharacterLayout::SmgCyanRest);
		}
	}
	if (SmgAmberMID != nullptr)
	{
		SmgAmberMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), TraceCharacterLayout::SmgAmberFull);
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("%s built the SMG viewmodel (4 parts at scale %.2f, muzzle at rig x=%.1f, cyan MIDs %d, amber %s)."),
		*GetName(), S,
		TraceCharacterLayout::SmgOrigin.X + TraceCharacterLayout::SmgMuzzleLocal.X * S,
		SmgCyanMIDs.Num(), SmgAmberMID != nullptr ? TEXT("ok") : TEXT("MISSING"));
	return true;
}

void ATraceCharacter::UpdateRailgunFire(float DeltaSeconds)
{
	if (RailgunBodyPart == nullptr)
	{
		return;
	}

	// Trace.Railgun.Hold pins the pose so a screenshot can catch it. Checked before the early-out
	// below, because a held pose has no shot behind it.
	bool bHeld = false;
	if (RailgunDebugHoldAlpha >= 0.f)
	{
		const UWorld* World = GetWorld();
		if (World != nullptr && World->GetTimeSeconds() < RailgunDebugHoldUntil)
		{
			bHeld = true;
		}
		else
		{
			RailgunDebugHoldAlpha = -1.f;
			RailgunFireElapsed = -1.f;
		}
	}

	if (!bHeld && RailgunFireElapsed < 0.f)
	{
		return;
	}

	if (!bHeld)
	{
		RailgunFireElapsed += DeltaSeconds;
	}

	// Map elapsed real time onto the authored clip, starting at the discharge frame. The charge
	// segment is deliberately skipped: this weapon has no windup to charge through.
	const float Span = TraceRailgunFireCurve::ClipSeconds - TraceRailgunFireCurve::DischargeSeconds;
	const float Alpha = bHeld
		? FMath::Clamp(RailgunDebugHoldAlpha, 0.f, 1.f)
		: ((RailgunFireDuration > KINDA_SMALL_NUMBER)
			? FMath::Clamp(RailgunFireElapsed / RailgunFireDuration, 0.f, 1.f) : 1.f);
	const float ClipTime = TraceRailgunFireCurve::DischargeSeconds + Alpha * Span;

	float Cyan = 1.f;
	float Amber = 1.f;
	TraceRailgunFireCurve::Sample(ClipTime, Cyan, Amber);

	if (RailgunCyanMID != nullptr)
	{
		RailgunCyanMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Cyan);
	}
	if (RailgunAmberMID != nullptr)
	{
		RailgunAmberMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Amber);
	}

	// The MECHANICS ride the same authored curve as the glow, normalised to 0..1, so the rails are
	// widest at the brightest frame and shut as the flash dies. Two effects, one curve, no chance of
	// them drifting apart the way two hand-tuned timelines would.
	const float Mechanical = FMath::Clamp(
		(Cyan - 1.f) / FMath::Max(TraceRailgunFireCurve::PeakCyan - 1.f, KINDA_SMALL_NUMBER), 0.f, 1.f);

	const float S = TraceCharacterLayout::RailgunScale;
	const FVector Recoil(-TraceCharacterLayout::RailgunRecoilBackUU * S * Mechanical, 0.f, 0.f);
	const FRotator RecoilPitch(-TraceCharacterLayout::RailgunRecoilPitchDegrees * Mechanical, 0.f, 0.f);

	RailgunBodyPart->SetRelativeLocationAndRotation(
		TraceCharacterLayout::RailgunOrigin + Recoil, RecoilPitch);

	const float Throw = TraceCharacterLayout::RailgunRailThrowUU * S * Mechanical;
	const float Cant = TraceCharacterLayout::RailgunRailCantDegrees * Mechanical;
	const FVector HingeOffset(-5.0f, 7.8f, 4.5f);

	if (RailgunRailLeftPart != nullptr)
	{
		RailgunRailLeftPart->SetRelativeLocationAndRotation(
			TraceCharacterLayout::RailgunOrigin
				+ FVector(HingeOffset.X, -HingeOffset.Y, HingeOffset.Z) * S
				+ Recoil + FVector(0.f, -Throw, 0.f),
			RecoilPitch + FRotator(0.f, -Cant, 0.f));
	}
	if (RailgunRailRightPart != nullptr)
	{
		RailgunRailRightPart->SetRelativeLocationAndRotation(
			TraceCharacterLayout::RailgunOrigin + HingeOffset * S
				+ Recoil + FVector(0.f, Throw, 0.f),
			RecoilPitch + FRotator(0.f, Cant, 0.f));
	}

	// Finished: park the state so the next shot restarts cleanly, and so a rig that is never fired
	// again is not doing this arithmetic forever.
	if (Alpha >= 1.f && !bHeld)
	{
		RailgunFireElapsed = -1.f;
	}
}

// -------------------------------------------------------------------------------------------------
// SPEC v30 §2 — WHICH GUN IS ON SCREEN
// -------------------------------------------------------------------------------------------------
//
// *** THE TWO VISIBILITY LAYERS, AND WHY THIS USES THE SECOND ONE. ***
//
// A first-person weapon part is drawn only when BOTH `bVisible` and `!bHiddenInGame` say so, and the
// two flags have separate setters that never touch each other. That is exactly one flag more than
// this project had owners for, and the extra one is what makes a three-state selector safe:
//
//   bVisible        "is the rig on screen at all". TWO writers already: ATraceCharacter::
//                   SetViewModelVisible (the carry blend, the corpse, respawns) and
//                   UTraceWeaponComponent::SetGunViewModelHidden (the knife). The latter deliberately
//                   RE-ASSERTS ITSELF EVERY TICK — that re-assert is the spec v12 §7 fix — and it
//                   sets every non-hand part it can find under ViewModelRoot.
//   bHiddenInGame   "which of the two guns is selected". Written HERE AND NOWHERE ELSE.
//
// Had the selector been written in bVisible it would have been in a fight it could not win: the
// knife's per-tick re-assert shows every gun part whenever the knife is not out, so a pistol hidden
// for the SMG would come back sixty times a second, and which one you saw would depend on component
// tick order. Splitting the layers means the two rules COMPOSE instead of racing — the knife decides
// whether any gun is drawn, this decides which one, and neither has to know the other exists.
//
// It also keeps UTraceWeaponComponent::GetViewModelCensus honest for free: it counts with
// IsVisible(), which already folds in bHiddenInGame, so the SMG-hidden pistol is correctly NOT
// counted as a gun on screen and Trace.Knife.DualWeaponTest keeps measuring what it measures.

void ATraceCharacter::UpdateWeaponSelection()
{
	if (!bViewModelBuilt || ViewModelRoot == nullptr)
	{
		return;
	}

	// THE REPLICATED SELECTOR IS THE SOURCE OF TRUTH, not a local guess and not an input event. It is
	// the same value the damage table, the fire rate and the ammo counter read, so the gun on screen
	// and the gun being simulated cannot disagree — which is the entire complaint spec §2 opens with.
	//
	// *** IT ANSWERS "WHICH GUN", AND DELIBERATELY NOT "ANY GUN AT ALL". *** The `1` state — guns
	// stowed — is UTraceWeaponComponent's rule, enforced by SetGunViewModelHidden and re-asserted
	// every tick, and it works on this rig unchanged because both gun rigs are ordinary children of
	// ViewModelRoot. So the Knife case below moves NOTHING: it records that nothing is drawn and
	// leaves the holstered firearm's flags exactly where they were.
	//
	// THAT RESTRAINT IS NOT TIDINESS, IT IS A MEASURED FIX. The first version of this function also
	// hid both rigs on Knife — harmless-looking, and the same rule enforced twice. Running
	// Trace.Knife.DualWeaponTest with -TraceLegacyKnife then reported
	//     RESULT: *** NOT PROVEN *** — the RED arm did not reproduce the bug
	// because that harness's red arm restores the v12 §7 latch defect in SetGunViewModelHidden and
	// counts the gun parts left on screen beside the knife — and this function was quietly hiding
	// them for it. A second owner for one rule had taken an existing verifier's red arm away. With
	// the Knife case inert the red arm reproduces again, and the `1` state is still exactly as
	// correct as it was before this pass, through the path that is actually tested.
	bool bStowed = false;
	if (Weapon != nullptr)
	{
		switch (Weapon->GetEquippedWeapon())
		{
		case ETraceEquippedWeapon::Knife:
			// SPEC v29 §5 gave this value back its old meaning: guns stowed. The holstered firearm
			// does not change when you put it away, so SelectedFirearm is left alone and comes back
			// unchanged on the next `2` or `3`.
			bStowed = true;
			break;

		case ETraceEquippedWeapon::Smg:
			// THE FALLBACK. No SMG art (a fresh clone without `git lfs pull`, or -TraceNoSmg) means
			// the SMG slot shows the pistol rig rather than an empty pair of hands. A player who can
			// see a gun and shoot it has a playable game; a player holding nothing has a bug report.
			SelectedFirearm = (SmgBodyPart != nullptr) ? EShownGun::Smg : EShownGun::Pistol;
			if (SmgBodyPart == nullptr && !bSmgFallbackLogged)
			{
				bSmgFallbackLogged = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("%s selected the SMG but has no SMG rig; showing the pistol rig instead. ")
					TEXT("The weapon's damage, fire rate, clip and reload are unaffected — this is a ")
					TEXT("MISSING-ART fallback, not a gameplay change. See the build log above for which ")
					TEXT("mesh was absent."), *GetName());
			}
			break;

		default:
			SelectedFirearm = EShownGun::Pistol;
			break;
		}
	}

	// What is actually on screen, which is what GetShownGun() promises to report: nothing while the
	// guns are stowed, otherwise the firearm this rig is holding.
	ShownGun = bStowed ? EShownGun::None : SelectedFirearm;

	const bool bShowPistol = (SelectedFirearm == EShownGun::Pistol);

	for (const TObjectPtr<UStaticMeshComponent>& Part : PistolWeaponParts)
	{
		if (Part != nullptr)
		{
			Part->SetHiddenInGame(!bShowPistol);
		}
	}
	for (const TObjectPtr<UStaticMeshComponent>& Part : SmgWeaponParts)
	{
		if (Part != nullptr)
		{
			Part->SetHiddenInGame(bShowPistol);
		}
	}
}

USceneComponent* ATraceCharacter::GetActiveMuzzleMarker() const
{
	// [SPEC v30 §5] The beam must leave whichever gun is actually on screen. Asked of what is DRAWN
	// rather than of the selector, so the missing-art fallback — SMG selected, pistol rig up — puts
	// the beam on the barrel the player can see rather than on one that is hidden.
	if (SelectedFirearm == EShownGun::Smg && ViewModelSmgMuzzle != nullptr)
	{
		return ViewModelSmgMuzzle;
	}
	return ViewModelMuzzle;
}

// -------------------------------------------------------------------------------------------------
// SPEC v30 §3 and §4 — the SMG's motion and glow
// -------------------------------------------------------------------------------------------------

void ATraceCharacter::UpdateSmgAnimation(float DeltaSeconds)
{
	if (SmgBodyPart == nullptr)
	{
		return;
	}

	// Trace.Smg.Hold pins a pose so a screenshot can catch it, and it is checked FIRST for the same
	// reason the railgun's is: a held pose has no shot and no reload behind it. The whole fire cycle
	// is 0.100 s — three frames at 30 fps — so without this there is no way to photograph the shot
	// frame at all, and "the walls move" would be a claim rather than a picture.
	bool bHeld = false;
	if (SmgDebugHoldAlpha >= 0.f || SmgDebugHoldReloadAlpha >= 0.f)
	{
		const UWorld* World = GetWorld();
		if (World != nullptr && World->GetTimeSeconds() < SmgDebugHoldUntil)
		{
			bHeld = true;
		}
		else
		{
			SmgDebugHoldAlpha = -1.f;
			SmgDebugHoldReloadAlpha = -1.f;
			SmgFireElapsed = -1.f;
		}
	}

	// --- Where in the 0.100 s fire cycle are we? -------------------------------------------------
	//
	// DRIVEN BY REAL SHOTS. SmgFireElapsed is armed by NotifyWeaponFired, which UTraceWeaponComponent
	// calls at the moment a round is committed. Nothing here free-runs: with the trigger up the phase
	// sits at 1.0, every curve returns zero, and the gun is at rest by construction rather than by a
	// timer happening to be in the right place.
	// *** SAMPLE FIRST, THEN ADVANCE — AND THE ORDER IS THE WHOLE POINT. ***
	//
	// NotifyWeaponFired arms this at 0, which IS the shot frame: full +/-42 mm wall snap, full 4.8x
	// cyan, full recoil. Advancing before sampling meant phase 0 was never once evaluated — the
	// first sample after a shot landed a whole frame in, and on a 0.100 s cycle at 50 fps that is
	// 20% of the way down the decay. Measured before this change: peak cyan 4.058 instead of 4.8,
	// recoil at 55-75% of its authored amplitude, and the wall snap essentially never rendered.
	//
	// It is the same shape as the fire-rate bug in spec v29 §2f: a per-frame reader sampling a
	// quantity that changes faster than the frame does, and losing the part that falls between.
	float FirePhase = 1.f;   // 1.0 == settled
	if (bHeld && SmgDebugHoldAlpha >= 0.f)
	{
		FirePhase = FMath::Clamp(SmgDebugHoldAlpha, 0.f, 1.f);
	}
	else if (SmgFireElapsed >= 0.f)
	{
		FirePhase = (SmgFireDuration > KINDA_SMALL_NUMBER)
			? FMath::Clamp(SmgFireElapsed / SmgFireDuration, 0.f, 1.f) : 1.f;
	}

	// Advanced AFTER the sample above, so the frame that follows a shot draws the shot frame.
	if (!bHeld && SmgFireElapsed >= 0.f)
	{
		SmgFireElapsed += DeltaSeconds;
	}

	// --- Where in the reload are we? -------------------------------------------------------------
	//
	// READ OFF THE WEAPON'S OWN REPLICATED DEADLINE, every frame, rather than started by an event and
	// counted locally. That is what makes the picture unable to lie: a reload that is cancelled, that
	// arrives late over the network, or that runs at a length nobody told this file about still puts
	// the magazine exactly where the gun's remaining time says it should be.
	//
	// AND IT IS WHERE THE 0.8 s / 1.3 s CONFLICT IS RESOLVED. GetReloadSeconds() is the GAMEPLAY
	// number (1.3 s for the SMG, from Config/DefaultGame.ini), so dividing by it time-stretches the
	// authored 0.8 s motion onto it. See the constants block for why stretching beats holding.
	float ReloadPhase = -1.f;   // negative == not reloading, magazine seated
	if (bHeld && SmgDebugHoldReloadAlpha >= 0.f)
	{
		ReloadPhase = FMath::Clamp(SmgDebugHoldReloadAlpha, 0.f, 1.f);
	}
	else if (Weapon != nullptr && Weapon->IsReloading())
	{
		const float Total = FMath::Max(Weapon->GetReloadSeconds(), KINDA_SMALL_NUMBER);
		ReloadPhase = FMath::Clamp(1.f - (Weapon->GetReloadRemaining() / Total), 0.f, 1.f);
	}

	// --- §4: the emissive ------------------------------------------------------------------------
	//
	// circuit_cyan idles at 1.8x and spikes to 4.8x on the shot frame. Written to ALL THREE MIDs
	// together — body and both walls — because the channel light runs down the rails, and lighting
	// only the body would leave the two brightest strips on the weapon dead through every shot.
	const float Cyan = TraceCharacterLayout::SmgCyanRest
		+ (TraceCharacterLayout::SmgCyanPeak - TraceCharacterLayout::SmgCyanRest)
			* TraceCharacterLayout::SmgFlashFall(FirePhase);

	for (const TObjectPtr<UMaterialInstanceDynamic>& MID : SmgCyanMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Cyan);
		}
	}

	// core_amber is THE AMMO READOUT — 1.4x at a full 40, 0.35x at empty, interpolating. Driven by
	// the clip and not by the fire clip, which is the point: the cell visibly drains as the magazine
	// empties, so a player can read how much they have left off the gun in their hands instead of off
	// a number in the corner. It refills on the frame the reload lands, which is the same frame the
	// magazine seats — one event, two things on screen agreeing about it.
	//
	// The clip is asked for even when the weapon is not the SMG, because a swap can happen at any
	// time and a stale amber value would be a lit cell on a gun that is empty.
	if (SmgAmberMID != nullptr)
	{
		float Fill = 1.f;
		if (Weapon != nullptr)
		{
			const int32 ClipSize = FMath::Max(1, Weapon->GetClipSize());
			Fill = FMath::Clamp(static_cast<float>(Weapon->GetClipAmmo()) / static_cast<float>(ClipSize), 0.f, 1.f);
		}
		SmgAmberMID->SetScalarParameterValue(TEXT("EmissiveIntensity"),
			FMath::Lerp(TraceCharacterLayout::SmgAmberEmpty, TraceCharacterLayout::SmgAmberFull, Fill));
	}

	// --- §3: the motion ---------------------------------------------------------------------------
	//
	// UNITS, ONCE, HERE. Every constant below is in the MESH's centimetres (the kit quotes millimetres
	// and the import is x100), so every one of them is multiplied by SmgScale to reach rig space. A
	// value that modifies a base must be relative to that base: retuning the rig's size must not
	// silently change how far the walls travel across the gun.
	const float S = TraceCharacterLayout::SmgScale;

	// The whole weapon recoils — the root node `railgun_smg` in the kit's table, which is body, walls
	// and magazine together. Applied to all four components as one rigid transform about SmgOrigin,
	// which is exact because all four meshes are baked around that same origin.
	//
	// The pitch sign follows the kit (-0.045 rad) and the railgun's own convention. This is the
	// receiver rocking INSIDE the hands; the muzzle rise a player actually reads comes from
	// ViewModelKick, which pitches the entire rig — hands included — and is not replaced here.
	const float Recoil = TraceCharacterLayout::SmgFlashFall(FirePhase);
	const FVector RecoilOffset(-TraceCharacterLayout::SmgRecoilBackUU * S * Recoil, 0.f, 0.f);
	const FRotator RecoilPitch(-TraceCharacterLayout::SmgRecoilPitchDegrees * Recoil, 0.f, 0.f);

	// The walls snap apart on the shot frame and elastic-settle. LEFT IS -Y: the manifest puts
	// SM_RailgunSmg_WallLeft entirely at y = -12.7 .. -4.7 cm, so the sign is read off the geometry
	// rather than assumed from a node name.
	const float Throw = TraceCharacterLayout::SmgWallThrowUU * S
		* TraceCharacterLayout::SmgElasticSettle(FirePhase);

	// The magazine. Straight down in the weapon's own frame; the recoil pitch above then carries it
	// with the rest of the gun, so a reload during a burst does not tear the cell off the well.
	const float MagDrop = (ReloadPhase >= 0.f) ? TraceCharacterLayout::SmgMagDrop(ReloadPhase) : 0.f;

	SmgBodyPart->SetRelativeLocationAndRotation(
		TraceCharacterLayout::SmgOrigin + RecoilOffset, RecoilPitch);

	if (SmgWallLeftPart != nullptr)
	{
		SmgWallLeftPart->SetRelativeLocationAndRotation(
			TraceCharacterLayout::SmgOrigin + RecoilOffset + FVector(0.f, -Throw, 0.f), RecoilPitch);
	}
	if (SmgWallRightPart != nullptr)
	{
		SmgWallRightPart->SetRelativeLocationAndRotation(
			TraceCharacterLayout::SmgOrigin + RecoilOffset + FVector(0.f, Throw, 0.f), RecoilPitch);
	}
	if (SmgMagPart != nullptr)
	{
		SmgMagPart->SetRelativeLocationAndRotation(
			TraceCharacterLayout::SmgOrigin + RecoilOffset + FVector(0.f, 0.f, -MagDrop * S), RecoilPitch);
	}

	// Finished: park the state so the next round restarts the cycle cleanly. At 600 RPM the next
	// shot lands on the frame after this, which is exactly what "set it to loop" means.
	if (FirePhase >= 1.f && !bHeld)
	{
		SmgFireElapsed = -1.f;
	}
}

void ATraceCharacter::DebugHoldRailgunPhase(float Alpha, float HoldSeconds)
{
	const UWorld* World = GetWorld();
	RailgunDebugHoldAlpha = Alpha;
	RailgunDebugHoldUntil = (World != nullptr && Alpha >= 0.f)
		? World->GetTimeSeconds() + FMath::Max(0.f, HoldSeconds) : -1.0;
}

void ATraceCharacter::DebugGetRailgunParts(UStaticMeshComponent*& OutBody,
	UStaticMeshComponent*& OutRailLeft, UStaticMeshComponent*& OutRailRight) const
{
	OutBody = RailgunBodyPart;
	OutRailLeft = RailgunRailLeftPart;
	OutRailRight = RailgunRailRightPart;
}

bool ATraceCharacter::DebugGetRailgunEmissive(float& OutCyan, float& OutAmber) const
{
	OutCyan = -1.f;
	OutAmber = -1.f;
	if (RailgunCyanMID == nullptr || RailgunAmberMID == nullptr)
	{
		return false;
	}
	const bool bCyan = RailgunCyanMID->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutCyan);
	const bool bAmber = RailgunAmberMID->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutAmber);
	return bCyan && bAmber;
}

bool ATraceCharacter::UsesRailgunViewModel() const
{
	return RailgunBodyPart != nullptr;
}

// --- The SMG's twins of the four accessors above  (spec v30 §2) ----------------------------------

bool ATraceCharacter::UsesSmgViewModel() const
{
	return SmgBodyPart != nullptr;
}

ATraceCharacter::EShownGun ATraceCharacter::GetShownGun() const
{
	return ShownGun;
}

void ATraceCharacter::DebugHoldSmgPhase(float Alpha, float ReloadAlpha, float HoldSeconds)
{
	const UWorld* World = GetWorld();
	SmgDebugHoldAlpha = Alpha;
	SmgDebugHoldReloadAlpha = ReloadAlpha;
	SmgDebugHoldUntil = (World != nullptr && (Alpha >= 0.f || ReloadAlpha >= 0.f))
		? World->GetTimeSeconds() + FMath::Max(0.f, HoldSeconds) : -1.0;
}

void ATraceCharacter::DebugGetSmgParts(UStaticMeshComponent*& OutBody, UStaticMeshComponent*& OutWallLeft,
	UStaticMeshComponent*& OutWallRight, UStaticMeshComponent*& OutMag) const
{
	OutBody = SmgBodyPart;
	OutWallLeft = SmgWallLeftPart;
	OutWallRight = SmgWallRightPart;
	OutMag = SmgMagPart;
}

bool ATraceCharacter::DebugGetSmgEmissive(float& OutCyan, float& OutAmber) const
{
	OutCyan = -1.f;
	OutAmber = -1.f;
	if (SmgCyanMIDs.Num() == 0 || SmgCyanMIDs[0] == nullptr || SmgAmberMID == nullptr)
	{
		return false;
	}
	const bool bCyan = SmgCyanMIDs[0]->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutCyan);
	const bool bAmber = SmgAmberMID->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutAmber);
	return bCyan && bAmber;
}

bool ATraceCharacter::DebugGetViewModelMuzzleRaw(FVector& OutWorldLocation) const
{
	// [SPEC v30 §5] The gun that is DRAWN, not the pistol's marker unconditionally.
	const USceneComponent* Marker = GetActiveMuzzleMarker();
	if (Marker == nullptr)
	{
		return false;
	}
	OutWorldLocation = Marker->GetComponentLocation();
	return true;
}

bool ATraceCharacter::GetViewModelMuzzleViewPoint(FVector& OutWorldLocation) const
{
	// Nothing drawn, nothing to answer. bViewModelVisible rather than bViewModelBuilt: a carrier in
	// third person and a corpse both still HAVE a rig, they just are not looking at it, and a beam
	// started at a hidden gun would come out of thin air beside the camera.
	// [SPEC v30 §5] GetActiveMuzzleMarker(), not ViewModelMuzzle: the beam has to leave whichever of
	// the two guns is actually being drawn. Everything below is unchanged — the marker is a child of
	// its own gun's body either way, so the same argument about inheriting recoil, sway and bob holds
	// for both of them.
	const USceneComponent* Marker = GetActiveMuzzleMarker();
	if (Marker == nullptr || Camera == nullptr || !bViewModelVisible)
	{
		return false;
	}

	// The camera component, not the player camera manager. The marker hangs off this component, so its
	// position RELATIVE to this transform is exact by construction; asking the manager instead would
	// mix two transforms that agree today (arm length 0, no lag, no modifiers) but need not tomorrow.
	//
	// GetCameraView() is not const and is not purely a getter: it WRITES the component's world rotation
	// when bUsePawnControlRotation is set. It is safe from a const query here only because this camera
	// hangs off a spring arm that has already applied the control rotation, so bUsePawnControlRotation
	// is false (see the constructor). Turn that flag on and this call starts moving the camera from
	// inside a tracer spawn — read the constructor before changing it.
	FMinimalViewInfo POV;
	Camera->GetCameraView(0.f, POV);

	// Not an optimisation — a correctness gate. TransformWorldToFirstPerson() uses FirstPersonFOV and
	// FirstPersonScale unconditionally, and GetCameraView fills them with neutral values (scene FOV,
	// scale 1) when the feature is off; that is a no-op, but saying so explicitly means a future
	// "turn the first-person rendering off" cannot silently start shifting the beam.
	if (!POV.bUseFirstPersonParameters)
	{
		OutWorldLocation = Marker->GetComponentLocation();
		return true;
	}

	// bIgnoreFirstPersonScale=TRUE, and the engine's own comment on the parameter is the argument:
	// "useful for cases where a full size projectile is spawned in front of the first person weapon.
	// By ignoring the first person scale for the spawn location, the spawned full-size projectile will
	// be spawned a bit further away from the camera, but its on-screen size will look correct."
	//
	// The tracer is exactly that — a world-space beam, drawn at world depth, spawned in front of a
	// first-person weapon. The two flags put the start at the SAME PIXEL either way (the morph scales
	// depth and offset together, so the projected point does not move); what changes is how far from
	// the eye the near end of a 20 uu-wide sheath sits. Taking the full squash would park it ~34 uu
	// out, where that sheath subtends a third of the frame and the muzzle flash becomes the near-field
	// whiteout the class comment in TraceTracer.h was written about. Ignoring the scale leaves it at
	// the gun's true ~85 uu, which is also within a couple of uu of the standoff this replaces — so
	// the beam's THICKNESS at the muzzle is unchanged and only its POSITION moves onto the barrel.
	OutWorldLocation = POV.TransformWorldToFirstPerson(
		Marker->GetComponentLocation(), /*bIgnoreFirstPersonScale=*/true);
	return !OutWorldLocation.ContainsNaN();
}

UMaterialInstanceDynamic* ATraceCharacter::GetViewModelBodyMID() const
{
	return ViewModelBodyMID;
}

UMaterialInstanceDynamic* ATraceCharacter::GetViewModelNeonMID() const
{
	return ViewModelNeonMID;
}

bool ATraceCharacter::GetViewModelOffHand(FVector& OutLocation) const
{
	// [DUALWIELD] Both facts come out of EnsureViewModelBuilt, which is the only writer. Reporting
	// the location even when the hand is NOT free is deliberate: a caller that wants to know where
	// the support hand is (a future two-handed prop, a debug draw) gets a real answer, and the bool
	// is the only thing that says whether the hand is available to hold something.
	OutLocation = ViewModelOffHandLocation;
	return bViewModelBuilt && bViewModelOffHandFree;
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

	// [SPEC v30 §2] WHICH GUN, decided before anything else and OUTSIDE the bAnimate gate below.
	// A rig that is hidden — a Core carrier in third person, a corpse — can still have its selector
	// changed, and settling that here means the correct gun is already on the rig at the moment
	// SetViewModelVisible brings it back, rather than a frame of the wrong one. It is also cheap:
	// SetHiddenInGame is a no-op when the flag already matches.
	UpdateWeaponSelection();

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

	// The railgun's own animation runs on top of the rig transform above: the rig carries the whole
	// weapon-and-hands assembly, this moves parts of the weapon relative to it.
	UpdateRailgunFire(DeltaSeconds);

	// [SPEC v30 §3/§4] And the SMG's, on the same terms. BOTH are ticked whichever gun is on screen,
	// deliberately: the hidden one costs four early-outs or four transforms that nothing renders, and
	// the alternative — animating only the selected rig — leaves the other one holding whatever pose
	// it had when it was put away, so a swap back mid-burst would show a gun frozen with its walls
	// open. Cheap insurance against a class of bug that only appears under a swap.
	UpdateSmgAnimation(DeltaSeconds);
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

	// Restart the railgun's discharge, time-warped so it always finishes before the next round can
	// leave: at 150 RPM that is 0.36 s for the clip's 0.85 s tail, and it shortens further for the
	// characters that fire faster. Rails caught half-open by the next shot would read as a stutter.
	if (RailgunBodyPart != nullptr)
	{
		const float FireInterval = FMath::Max(0.01f, UTraceSettings::Get().FireInterval)
			* UTraceAbilityComponent::GetFireIntervalScaleFor(this);
		RailgunFireDuration = FMath::Clamp(
			FireInterval * TraceCharacterLayout::RailgunFireIntervalFraction,
			TraceCharacterLayout::RailgunFireMinSeconds,
			TraceRailgunFireCurve::ClipSeconds - TraceRailgunFireCurve::DischargeSeconds);
		RailgunFireElapsed = 0.f;
	}

	// [SPEC v30 §3] The SMG's cycle, restarted by the same real shot. THIS IS THE ONLY THING THAT
	// STARTS IT — there is no free-running timer anywhere in UpdateSmgAnimation — so the walls
	// cannot snap on a frame no round left on.
	//
	// The cycle is the kit's 0.100 s clip, but its LENGTH is taken from the weapon in hand rather
	// than from that constant, because 0.100 s is only the cadence of a stock SMG: Roxie's Modded
	// runs the same gun at 990 RPM and Slimeball's stuck passive at 780. Using the authored length
	// there would leave the walls still ringing when the next round left. GetFireInterval() is the
	// component's own answer, ability scaling already folded in, so the animation inherits every
	// per-character fire-rate modifier for free — the same seam spec v28 §9 required of the SMG's
	// gameplay numbers.
	if (SmgBodyPart != nullptr)
	{
		float Cycle = TraceCharacterLayout::SmgFireClipSeconds;
		if (Weapon != nullptr)
		{
			Cycle = static_cast<float>(Weapon->GetFireInterval());
		}
		SmgFireDuration = FMath::Max(
			Cycle * TraceCharacterLayout::SmgFireIntervalFraction,
			TraceCharacterLayout::SmgFireMinSeconds);
		SmgFireElapsed = 0.f;
	}
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

// -----------------------------------------------------------------------------------------------
// Movement base — the arena is not a moving platform. Full reasoning on the declarations.
// -----------------------------------------------------------------------------------------------

bool ATraceCharacter::ShouldIgnoreAsMovementBase(const UPrimitiveComponent* BaseComponent)
{
	if (BaseComponent == nullptr)
	{
		return false;
	}

	// Cheap out first: anything that is not Movable is already a static base as far as the engine is
	// concerned (MovementBaseUtility::IsDynamicBase is exactly a Movable test), so it never produces
	// a base-relative correction and there is nothing to suppress.
	if (BaseComponent->Mobility != EComponentMobility::Movable)
	{
		return false;
	}

	// The arena and only the arena. Characters and the Core are real dynamic bases.
	return BaseComponent->GetOwner() != nullptr
		&& BaseComponent->GetOwner()->IsA<ATraceArenaBuilder>();
}

void ATraceCharacter::SetBase(FMovementBaseInterfaceData* MovementBaseInterfaceData, const FName BoneName, bool bNotifyActor)
{
	const UPrimitiveComponent* AsPrimitive = (MovementBaseInterfaceData != nullptr)
		? Cast<UPrimitiveComponent>(MovementBaseInterfaceData->GetMovementBaseObject())
		: nullptr;

	if (ShouldIgnoreAsMovementBase(AsPrimitive))
	{
		// Not "skip the call" — the pawn may currently be based on something and has to be told it
		// no longer is, or it keeps a stale base forever.
		FMovementBaseInterfaceData* NoBase = nullptr;
		Super::SetBase(NoBase, NAME_None, bNotifyActor);
		return;
	}

	Super::SetBase(MovementBaseInterfaceData, BoneName, bNotifyActor);
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

					// SPEC v26 §4 — the beam's start, as an ANGLE OFF THE CROSSHAIR, because that is
					// the thing the report was about and the thing a screenshot can be checked against:
					// "muzDeg=(19.2r, 11.6d)" says the tracer leaves 19.2 degrees right and 11.6 degrees
					// below the centre of the frame, which is where the barrel is drawn.
					//
					// RAW and DRAWN are both printed and they are DELIBERATELY DIFFERENT. Raw is where
					// the muzzle component sits; drawn is where the first-person re-projection puts it
					// on screen. If those two ever print the same numbers, the morph is not being
					// applied and the beam is back to leaving from beside the barrel — a green-looking
					// probe that cannot see the thing it is checking, which this project has been
					// bitten by before.
					auto ProbeMuzzleAngles = [TraceChar](const FVector& World, double& OutRight, double& OutDown)
					{
						const FVector Local = TraceChar->Camera->GetComponentTransform().InverseTransformPosition(World);
						OutRight = FMath::RadiansToDegrees(FMath::Atan2(Local.Y, FMath::Max(Local.X, 1.0)));
						OutDown = FMath::RadiansToDegrees(FMath::Atan2(-Local.Z, FMath::Max(Local.X, 1.0)));
					};

					FVector MuzzleRaw = FVector::ZeroVector;
					FVector MuzzleDrawn = FVector::ZeroVector;
					const bool bHasRaw = TraceChar->DebugGetViewModelMuzzleRaw(MuzzleRaw);
					const bool bHasDrawn = TraceChar->GetViewModelMuzzleViewPoint(MuzzleDrawn);

					double RawRight = 0.0, RawDown = 0.0, DrawnRight = 0.0, DrawnDown = 0.0;
					if (bHasRaw)   { ProbeMuzzleAngles(MuzzleRaw, RawRight, RawDown); }
					if (bHasDrawn) { ProbeMuzzleAngles(MuzzleDrawn, DrawnRight, DrawnDown); }

					// crouch / eye / viewmodel are logged alongside because they are the three things
					// that can silently break the aim guarantee or the new viewmodel and that a
					// screenshot cannot distinguish: a crouch that never engaged looks exactly like
					// one that did if the eye height is not printed, and a viewmodel hidden by a
					// visibility bug looks exactly like one that was never built.
					UE_LOG(LogTraceGame, Display,
						TEXT("[ViewProbe] mode=%s carrier=%d coreHolder=%s holderIsMe=%d passActive=%d passHeld=%d predicted=%d ")
						TEXT("blend=%.2f arm=%.1f eyeErr=%.2fuu aimErr=%.4fdeg ")
						TEXT("bodyHiddenFromOwner=%d ctrlYaw=%d orientToMove=%d ")
						TEXT("crouched=%d sliding=%d halfHeight=%.1f baseEye=%.1f vmParts=%d vmVisible=%d ")
						TEXT("muzRawDeg=(%.1fr,%.1fd) muzDrawnDeg=(%.1fr,%.1fd) muzDepth=%.1fuu"),
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
						TraceChar->IsViewModelVisible() ? 1 : 0,
						RawRight, RawDown, DrawnRight, DrawnDown,
						bHasDrawn ? FVector::Dist(CameraLoc, MuzzleDrawn) : -1.0);

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

	// =============================================================================================
	// Trace.ViewModel.Guns / Trace.ViewModel.Equip  —  SPEC v30 §2
	// =============================================================================================
	//
	// WHAT THEY ARE FOR. The complaint spec §2 opens with is that nothing on screen said which gun
	// was in hand. The fix is visual, so a screenshot is the primary evidence — but a screenshot
	// cannot tell you WHY the pistol is on screen when you pressed 3 (missing art? a refused swap? a
	// mesh that failed to attach?), and those three have different fixes. Guns prints the difference,
	// and Equip is what lets a headless run reach all three states to photograph them at all.
	//
	// *** DELIBERATELY NOT NAMED Trace.Smg.* ***. Core/TraceSmgVerify.cpp registers Trace.Smg.Probe
	// and Trace.Smg.Hold — the SMG's equivalents of Trace.Railgun.Probe/Hold that spec §5 asks for,
	// sitting where TraceRailgunVerify.cpp's live, which is the right place for them. Both of those
	// drive the accessors THIS file owns (GetShownGun, DebugGetSmgParts, DebugGetSmgEmissive,
	// DebugHoldSmgPhase), so the pawn keeps the state and the verify file keeps the commands. Two
	// registrations of one console name is a warning at best and a silently lost command at worst,
	// so these two take names that cannot collide and answer a narrower question: the RIG, not the
	// weapon.

	/** One SMG part: mesh, transform, and every material slot with what is actually on it. */
	void ReportViewModelSmgPart(const TCHAR* Label, UStaticMeshComponent* Part)
	{
		if (Part == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] %s : NOT BUILT"), Label);
			return;
		}

		const UStaticMesh* Mesh = Part->GetStaticMesh();
		const FVector Loc = Part->GetRelativeLocation();
		const FRotator Rot = Part->GetRelativeRotation();

		// bVisible AND bHiddenInGame, separately, because the whole selector rests on them being two
		// independent layers (see UpdateWeaponSelection). "drawn=0" with "visible=1" is the selector
		// having hidden this gun on purpose; "visible=0" is the rig being off screen entirely.
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Smg] %s mesh=%-24s rel=(%.2f, %.2f, %.2f) pitch=%.2f scale=%.3f visible=%d hiddenInGame=%d drawn=%d"),
			Label, Mesh != nullptr ? *Mesh->GetName() : TEXT("NONE"),
			Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Part->GetRelativeScale3D().X,
			Part->GetVisibleFlag() ? 1 : 0, Part->bHiddenInGame ? 1 : 0, Part->IsVisible() ? 1 : 0);

		const int32 NumSlots = Part->GetNumMaterials();
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			const UMaterialInterface* Material = Part->GetMaterial(Slot);
			const FName SlotName = (Mesh != nullptr && Mesh->GetStaticMaterials().IsValidIndex(Slot))
				? Mesh->GetStaticMaterials()[Slot].MaterialSlotName : NAME_None;
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg]       slot %d '%s' -> %s"),
				Slot, *SlotName.ToString(),
				Material != nullptr ? *Material->GetName() : TEXT("NONE"));
		}
	}

	const TCHAR* ViewModelShownGunName(ATraceCharacter::EShownGun Gun)
	{
		switch (Gun)
		{
		case ATraceCharacter::EShownGun::None:   return TEXT("NEITHER (guns stowed)");
		case ATraceCharacter::EShownGun::Pistol: return TEXT("PISTOL");
		case ATraceCharacter::EShownGun::Smg:    return TEXT("SMG");
		default:                                 return TEXT("?");
		}
	}

	FAutoConsoleCommand CmdViewModelGuns(
		TEXT("Trace.ViewModel.Guns"),
		TEXT("Spec v30 §2. Reports which of the THREE weapon states is on screen and whether it agrees "
		     "with the replicated selector: whether the SMG rig was built or fell back, every SMG "
		     "part's mesh/transform/visibility layers/slots, the live EmissiveIntensity on both glowing "
		     "materials, and where the active muzzle marker is. Takes an optional DelaySeconds so a "
		     "single deferred-exec batch can sample it AFTER an equip or a held pose has landed."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			// DEFERRABLE FOR THE SAME REASON THE EQUIP IS, and it matters more here: the pose is
			// applied in Tick, so a probe run in the same frame as the command that changed it reports
			// the state from BEFORE the change. A delay of a frame or more is the difference between
			// measuring the feature and measuring the frame before it.
			const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 0.f;
			if (Delay > 0.f)
			{
				double Waited = 0.0;
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
					[Waited, Delay](float DeltaTime) mutable -> bool
				{
					Waited += DeltaTime;
					if (Waited < Delay)
					{
						return true;
					}
					if (GEngine != nullptr)
					{
						GEngine->Exec(FindDebugGameWorld(), TEXT("Trace.ViewModel.Guns"));
					}
					return false;
				}), 0.f);
				return;
			}

			ATraceCharacter* Character = FindDebugLocalCharacter(FindDebugGameWorld());
			if (Character == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] No local ATraceCharacter yet - run this once a match has started."));
				return;
			}

			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ===================================================="));
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] ON SCREEN: %s   |   smgRig=%s pistolRig=%s   rigVisible=%d parts=%d"),
				ViewModelShownGunName(Character->GetShownGun()),
				Character->UsesSmgViewModel() ? TEXT("IMPORTED ART") : TEXT("NOT BUILT"),
				Character->UsesRailgunViewModel() ? TEXT("RAILGUN") : TEXT("FALLBACK CUBES"),
				Character->IsViewModelVisible() ? 1 : 0,
				Character->GetViewModelPartCount());

			// The selector beside the picture. These two disagreeing is the ONE state worth shouting
			// about: the gun being simulated is not the gun being drawn, which is spec §2's complaint
			// restated. It is legal exactly once — the missing-art fallback — and the line says so.
			if (const UTraceWeaponComponent* W = Character->FindComponentByClass<UTraceWeaponComponent>())
			{
				const bool bSmgSelected = W->IsSmgEquipped();
				const bool bSmgDrawn = (Character->GetShownGun() == ATraceCharacter::EShownGun::Smg);
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] selector=%s clip=%d/%d reloading=%d (%.2fs left of %.2fs)  %s"),
					LexToString(W->GetEquippedWeapon()), W->GetClipAmmo(), W->GetClipSize(),
					W->IsReloading() ? 1 : 0, W->GetReloadRemaining(), W->GetReloadSeconds(),
					(bSmgSelected == bSmgDrawn)
						? TEXT("-- selector and picture agree")
						: (Character->UsesSmgViewModel()
							? TEXT("*** MISMATCH: the gun drawn is not the gun selected ***")
							: TEXT("-- SMG selected with no SMG art: the documented fallback")));
			}

			if (!Character->UsesSmgViewModel())
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] The SMG art did not resolve, or -TraceNoSmg/-TraceNoRailgun was passed. "
					     "Content/Trace/Weapons/Meshes must contain SM_RailgunSmg_Body/WallLeft/WallRight/Mag."));
				UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ===================================================="));
				return;
			}

			UStaticMeshComponent* Body = nullptr;
			UStaticMeshComponent* WallL = nullptr;
			UStaticMeshComponent* WallR = nullptr;
			UStaticMeshComponent* Mag = nullptr;
			Character->DebugGetSmgParts(Body, WallL, WallR, Mag);

			ReportViewModelSmgPart(TEXT("body  "), Body);
			ReportViewModelSmgPart(TEXT("wallL "), WallL);
			ReportViewModelSmgPart(TEXT("wallR "), WallR);
			ReportViewModelSmgPart(TEXT("mag   "), Mag);

			// The wall SPREAD rather than each wall's Y, because the spread is the thing §3 specifies
			// (±4.2 uu apart) and it is invariant under any recoil offset applied to both.
			if (WallL != nullptr && WallR != nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] wall spread = %.3f uu (rig), i.e. %.2f mesh-cm apart; magazine drop = %.3f uu"),
					WallR->GetRelativeLocation().Y - WallL->GetRelativeLocation().Y,
					(WallR->GetRelativeLocation().Y - WallL->GetRelativeLocation().Y) / TraceCharacterLayout::SmgScale,
					(Mag != nullptr && Body != nullptr) ? (Body->GetRelativeLocation().Z - Mag->GetRelativeLocation().Z) : 0.f);
			}

			float LiveCyan = -1.f;
			float LiveAmber = -1.f;
			const bool bReadBack = Character->DebugGetSmgEmissive(LiveCyan, LiveAmber);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Smg] live EmissiveIntensity: cyan=%.3f (rest %.2f, peak %.2f) amber=%.3f (full %.2f, empty %.2f) -- readback %s"),
				LiveCyan, TraceCharacterLayout::SmgCyanRest, TraceCharacterLayout::SmgCyanPeak,
				LiveAmber, TraceCharacterLayout::SmgAmberFull, TraceCharacterLayout::SmgAmberEmpty,
				bReadBack ? TEXT("OK") : TEXT("FAILED - the parameter is not on the material"));

			FVector MuzzleRaw = FVector::ZeroVector;
			if (Character->DebugGetViewModelMuzzleRaw(MuzzleRaw))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Smg] active muzzle marker (world) = (%.1f, %.1f, %.1f); mesh-local landmark (%.1f, %.1f, %.1f) cm"),
					MuzzleRaw.X, MuzzleRaw.Y, MuzzleRaw.Z,
					TraceCharacterLayout::SmgMuzzleLocal.X, TraceCharacterLayout::SmgMuzzleLocal.Y,
					TraceCharacterLayout::SmgMuzzleLocal.Z);
			}
			UE_LOG(LogTraceGame, Warning, TEXT("[Smg] ===================================================="));
		}));

	/**
	 * Trace.ViewModel.Equip <1|2|3> [DelaySeconds] [TimeoutSeconds]
	 *
	 * IT SELF-SCHEDULES, and that is what makes photographing all three states possible at all. The
	 * deferred-exec harness fires ONE batch of commands at ONE time (see TraceAutoShot::
	 * ArmDeferredExec, "one timer, not one per command"), and at the moment that batch runs the match
	 * map has usually not produced a possessed pawn yet — the character-select screen is still open.
	 * A command that needed a pawn to already exist could therefore only ever capture whichever state
	 * happened to be selected, which is precisely one third of the evidence spec §2 asks for.
	 *
	 * So this waits for a living local pawn, then equips, and each invocation carries its OWN delay:
	 *
	 *     -TraceExec="Trace.ViewModel.Equip 2 0|Trace.ViewModel.Equip 3 4|Trace.ViewModel.Equip 1 8"
	 *     -TraceAutoShot=42 -TraceAutoShotRepeat=4
	 *
	 * ...photographs the pistol, the SMG and the stowed state in a single run.
	 */
	FAutoConsoleCommand CmdViewModelEquip(
		TEXT("Trace.ViewModel.Equip"),
		TEXT("Trace.ViewModel.Equip <1|2|3> [DelaySeconds] [TimeoutSeconds]. Dev only. Puts the local "
		     "player in one of the three weapon states through the SHIPPED equip path, waiting for a "
		     "pawn to exist first. 1 stows, 2 pistol, 3 SMG."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const int32 Slot = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 3;
			const float Delay = (Args.Num() > 1) ? FMath::Max(0.f, FCString::Atof(*Args[1])) : 0.f;
			const float Timeout = (Args.Num() > 2) ? FMath::Max(1.f, FCString::Atof(*Args[2])) : 90.f;

			ETraceEquippedWeapon Desired = ETraceEquippedWeapon::Smg;
			if (Slot == 1) { Desired = ETraceEquippedWeapon::Knife; }
			else if (Slot == 2) { Desired = ETraceEquippedWeapon::Gun; }

			double ElapsedSeconds = 0.0;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[ElapsedSeconds, Slot, Desired, Delay, Timeout](float DeltaTime) mutable -> bool
				{
					ElapsedSeconds += DeltaTime;
					if (ElapsedSeconds < Delay)
					{
						return true;
					}

					ATraceCharacter* Character = FindDebugLocalCharacter(FindDebugGameWorld());
					if (Character == nullptr || !Character->IsAlive())
					{
						if (ElapsedSeconds > Delay + Timeout)
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[ViewModel.Equip] Gave up after %.1fs: no living local pawn."), Timeout);
							return false;
						}
						return true;
					}

					// THE SHIPPED VERB, not a write to the selector. A harness that set EquippedWeapon
					// directly would photograph a state no key can actually reach, which is how a
					// screenshot ends up proving something the game does not do.
					ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
					const bool bOk = TraceMelee::RequestEquip(Character, Desired, &Refusal);

					// A refusal is usually the 0.35s pullout from the PREVIOUS command in the same
					// batch still running, which is transient — so retry rather than report a failure
					// that would have succeeded a frame later.
					if (!bOk && Refusal == ETraceMeleeRefusal::Deploying && ElapsedSeconds < Delay + Timeout)
					{
						return true;
					}

					// Reports the rig drawn BEFORE this frame, and says so: UpdateWeaponSelection runs in
					// Tick, so the new selector has not reached the rig yet. Printing it as "now" was
					// off by one frame and read as the equip having failed. Ask Trace.ViewModel.Guns
					// (which takes a delay for exactly this reason) for the settled answer.
					UE_LOG(LogTraceGame, Display,
						TEXT("[ViewModel.Equip] slot %d (%s) -> %s (refusal=%d); rig drawn as of the previous frame: %s."),
						Slot, LexToString(Desired), bOk ? TEXT("accepted") : TEXT("REFUSED"),
						static_cast<int32>(Refusal), ViewModelShownGunName(Character->GetShownGun()));
					return false;
				}),
				0.f);
		}));
}

#endif // !UE_BUILD_SHIPPING

// =================================================================================================
// SPEC v19 §4.1 — THE REPRODUCTION
//
// "If a player ever goes out of bounds of the arena, they should die and respawn."
//
// The rule is three lines of arithmetic, which is exactly the kind of rule that gets shipped broken:
// the arithmetic is trivially right and the CONSEQUENCES are the whole feature. So this asserts the
// consequences, not the arithmetic — a death, credited to nobody, with the cooldowns still running,
// and a pawn back inside the arena afterwards.
//
// WHY IT CAN GO RED, WHICH IS THE PART THAT MATTERS. Trace.Bounds.Enabled 0 removes the rule and
// nothing else, and the harness then reports FAIL with "still alive N seconds after being put
// outside". A harness whose only possible outcome is PASS proves nothing, and this project has
// already shipped one of those.
//
// It also asserts the two clauses that are easy to get wrong in the other direction and that a
// player would notice immediately:
//   * NOBODY IS CREDITED. Every other player's kill count must be unchanged. A world death that pays
//     out to whoever last shot at you is worse than no rule at all.
//   * THE COOLDOWN KEPT TICKING. Spec v19 §4.2's rule, restated by the user in the same breath, and
//     an out-of-bounds death is still a death, so it has to hold here too. Measured across the death
//     rather than argued from where the timer lives.
// =================================================================================================

#if !UE_BUILD_SHIPPING

namespace TraceCharacterBoundsVerify
{
	/** How long after the shove to wait for the rule to fire, in seconds. Grace + generous slack. */
	constexpr float DeathWindowSeconds = 3.0f;

	/** Cooldown parked on the victim before the shove, so "it kept ticking" is measurable at all. */
	constexpr float ParkedCooldownSeconds = 30.f;

	UWorld* PlayingWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr && Context.World()->GetAuthGameMode() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/** Every other player's kills, summed. The number that must not move. */
	int32 SumOtherKills(const UWorld* World, const APlayerState* Excluding)
	{
		int32 Total = 0;
		const AGameStateBase* GS = (World != nullptr) ? World->GetGameState() : nullptr;
		if (GS == nullptr)
		{
			return 0;
		}

		for (APlayerState* PS : GS->PlayerArray)
		{
			const ATracePlayerState* TracePS = Cast<ATracePlayerState>(PS);
			if (TracePS != nullptr && TracePS != Excluding)
			{
				Total += TracePS->Kills;
			}
		}
		return Total;
	}

	struct FRun
	{
		TWeakObjectPtr<ATraceCharacter> Victim;
		TWeakObjectPtr<ATracePlayerState> VictimState;
		TWeakObjectPtr<AController> VictimController;
		int32  DeathsAtStart = 0;
		int32  OtherKillsAtStart = 0;
		float  CooldownAtStart = 0.f;
		float  Elapsed = 0.f;
		bool   bDeathSeen = false;
		float  CooldownAtDeath = 0.f;
		int32  Failures = 0;
		int32  Stage = 0;   // 0 = waiting for the death, 1 = waiting for the respawn
	};

	void Report(FRun& Run, bool bPass, const FString& Detail)
	{
		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[BoundsVerify] PASS: %s"), *Detail);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[BoundsVerify] FAIL: %s"), *Detail);
		}
	}

	FAutoConsoleCommand CmdBoundsVerify(
		TEXT("Trace.Bounds.Verify"),
		TEXT("SPEC v19 §4.1. Parks a cooldown on a living player, teleports them outside the arena, and "
		     "asserts that they DIE, that nobody is credited with the kill, that the cooldown kept "
		     "ticking through the death, and that they respawn inside. Run it again with "
		     "Trace.Bounds.Enabled 0, which is the arm that must go RED."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UWorld* const World = PlayingWorld();
			if (World == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[BoundsVerify] INVALID: no authoritative world. NOT a pass - it could not run."));
				return;
			}

			const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
			const FBox Field = (Arena != nullptr) ? Arena->GetFieldBounds() : FBox(ForceInit);
			if (Field.IsValid == 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[BoundsVerify] INVALID: no arena, so there are no bounds to leave. NOT a pass."));
				return;
			}

			// A living pawn that is NOT holding the Core. The carrier would drag the possession rules
			// into a test about a boundary, and those are proven separately.
			ATraceCharacter* Victim = nullptr;
			for (TActorIterator<ATraceCharacter> It(World); It; ++It)
			{
				ATraceCharacter* const Candidate = *It;
				if (IsValid(Candidate) && Candidate->IsAlive() && !Candidate->IsCarrier()
					&& Candidate->GetController() != nullptr)
				{
					Victim = Candidate;
					break;
				}
			}

			if (Victim == nullptr)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[BoundsVerify] INVALID: nobody alive to push out of the world. NOT a pass."));
				return;
			}

			TSharedRef<FRun> Run = MakeShared<FRun>();
			Run->Victim = Victim;
			Run->VictimState = Victim->GetPlayerState<ATracePlayerState>();
			Run->VictimController = Victim->GetController();
			Run->DeathsAtStart = Run->VictimState.IsValid() ? Run->VictimState->Deaths : 0;
			Run->OtherKillsAtStart = SumOtherKills(World, Run->VictimState.Get());

			// Park a cooldown so "it kept ticking" is a measurement rather than an assertion about
			// where a float lives. DebugSetActivatedCooldown works with no character assigned, which is
			// deliberate here: this harness must be runnable in the first seconds of a match, before
			// the bots have claimed characters.
			if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Run->VictimState.Get()))
			{
				Abilities->DebugSetActivatedCooldown(ParkedCooldownSeconds);
				Run->CooldownAtStart = Abilities->GetActivatedCooldownRemaining();
			}

			// Well outside, along +X, and far enough that no margin setting could call it inside.
			const FVector Outside(
				Field.Max.X + FMath::Max(4000.f, CVarBoundsMarginXY.GetValueOnAnyThread() * 2.f),
				Field.GetCenter().Y,
				Field.Min.Z + 400.0);

			UE_LOG(LogTraceGame, Display,
				TEXT("[BoundsVerify] ===== pushing %s from %s to %s (the arena runs X %.0f..%.0f, Y %.0f..%.0f, ")
				TEXT("Z %.0f..%.0f; margins XY %.0f, below %.0f, ceiling %.0f = %s; grace %.2fs) | rule = %s ====="),
				*Victim->GetName(), *Victim->GetActorLocation().ToCompactString(), *Outside.ToCompactString(),
				Field.Min.X, Field.Max.X, Field.Min.Y, Field.Max.Y, Field.Min.Z, Field.Max.Z,
				CVarBoundsMarginXY.GetValueOnAnyThread(), CVarBoundsMarginBelow.GetValueOnAnyThread(),
				CVarBoundsCeilingMargin.GetValueOnAnyThread(),
				(CVarBoundsCeilingMargin.GetValueOnAnyThread() > 0.f) ? TEXT("ON") : TEXT("OFF (Lily flies)"),
				CVarBoundsGraceSeconds.GetValueOnAnyThread(),
				(CVarBoundsEnabled.GetValueOnAnyThread() != 0)
					? TEXT("v19 §4.1 ON") : TEXT("OFF - THE RED ARM, ARMED"));

			Victim->SetActorLocation(Outside, false, nullptr, ETeleportType::TeleportPhysics);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Run](float DeltaTime) -> bool
				{
					Run->Elapsed += DeltaTime;

					UWorld* const TickWorld = PlayingWorld();
					if (TickWorld == nullptr || !Run->VictimState.IsValid())
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[BoundsVerify] INVALID: the world or the player went away mid-run. NOT a pass."));
						return false;
					}

					UTraceAbilityComponent* const Abilities =
						UTraceAbilityComponent::Get(Run->VictimState.Get());
					const float CooldownNow =
						(Abilities != nullptr) ? Abilities->GetActivatedCooldownRemaining() : 0.f;

					// --- Stage 0: did the rule kill them? -------------------------------------------
					if (Run->Stage == 0)
					{
						const bool bDiedByCount = (Run->VictimState->Deaths > Run->DeathsAtStart);
						const bool bDiedByPawn = Run->Victim.IsValid() && !Run->Victim->IsAlive();

						if (bDiedByCount || bDiedByPawn)
						{
							Run->bDeathSeen = true;
							Run->CooldownAtDeath = CooldownNow;
							Run->Stage = 1;

							const int32 OtherKillsNow = SumOtherKills(TickWorld, Run->VictimState.Get());
							if (OtherKillsNow != Run->OtherKillsAtStart)
							{
								++Run->Failures;
								Report(*Run, false, FString::Printf(
									TEXT("somebody was CREDITED with the out-of-bounds death (other players' "
									     "kills %d -> %d). It must be creditable to nobody."),
									Run->OtherKillsAtStart, OtherKillsNow));
							}

							// The cooldown must be RUNNING, and must be LOWER than it was: a reset would
							// snap it back to 30, and a stopped clock would hold it exactly.
							if (Abilities == nullptr)
							{
								++Run->Failures;
								Report(*Run, false, TEXT("the victim has no ability component, so the cooldown "
								                         "clause could not be measured. NOT a pass."));
							}
							else if (CooldownNow <= 0.f)
							{
								++Run->Failures;
								Report(*Run, false, FString::Printf(
									TEXT("the cooldown was CLEARED by the death (%.2fs -> %.2fs). Spec v19 §4.2: "
									     "cooldowns keep ticking down through a death."),
									Run->CooldownAtStart, CooldownNow));
							}
							else if (CooldownNow >= Run->CooldownAtStart)
							{
								++Run->Failures;
								Report(*Run, false, FString::Printf(
									TEXT("the cooldown did not TICK across the death (%.2fs -> %.2fs). It must "
									     "keep counting down, not freeze or restart."),
									Run->CooldownAtStart, CooldownNow));
							}

							return true;   // now wait for the respawn
						}

						if (Run->Elapsed >= DeathWindowSeconds)
						{
							Report(*Run, false, FString::Printf(
								TEXT("%s was still ALIVE %.1fs after being teleported outside the arena. Spec "
								     "v19 §4.1 says out of bounds is a death. Rule = %s."),
								*GetNameSafe(Run->Victim.Get()), Run->Elapsed,
								(CVarBoundsEnabled.GetValueOnAnyThread() != 0)
									? TEXT("ON - THIS IS A REAL FAILURE")
									: TEXT("OFF - this is the RED ARM reproducing correctly")));
							return false;
						}

						return true;
					}

					// --- Stage 1: did they come back, inside? ---------------------------------------
					const APawn* const FreshPawn = Run->VictimController.IsValid()
						? Run->VictimController->GetPawn() : nullptr;
					const ATraceCharacter* const FreshCharacter = Cast<ATraceCharacter>(FreshPawn);

					if (FreshCharacter != nullptr && FreshCharacter->IsAlive())
					{
						FString Unused;
						const bool bInside = !ATraceCharacter::IsLocationOutOfArenaBounds(
							TickWorld, FreshCharacter->GetActorLocation(), Unused);

						if (!bInside)
						{
							++Run->Failures;
							Report(*Run, false, FString::Printf(
								TEXT("they respawned at %s, which is STILL out of bounds - so the rule would "
								     "kill them again in a loop."),
								*FreshCharacter->GetActorLocation().ToCompactString()));
						}

						Report(*Run, Run->Failures == 0, FString::Printf(
							TEXT("%s went out of bounds, DIED (deaths %d -> %d, cause is not creditable to any "
							     "enemy - their kills stayed at %d), their E cooldown kept ticking straight "
							     "through it (%.2fs at the push -> %.2fs at the death -> %.2fs now, never "
							     "reset), and they respawned INSIDE at %s. %d sub-check(s) failed."),
							*GetNameSafe(FreshCharacter), Run->DeathsAtStart, Run->VictimState->Deaths,
							Run->OtherKillsAtStart, Run->CooldownAtStart, Run->CooldownAtDeath, CooldownNow,
							*FreshCharacter->GetActorLocation().ToCompactString(), Run->Failures));
						return false;
					}

					if (Run->Elapsed >= DeathWindowSeconds + 15.f)
					{
						Report(*Run, false, FString::Printf(
							TEXT("%s died out of bounds but never respawned within %.0fs. 'Die AND respawn' is "
							     "one requirement, not two."),
							*GetNameSafe(Run->Victim.Get()), Run->Elapsed));
						return false;
					}

					return true;
				}),
				0.f);
		}));
}

#endif // !UE_BUILD_SHIPPING
