// Trace — the private constant tables and file-local knobs that ATraceCharacter's implementation
// shares with its sibling translation units. NOT a public interface: nothing outside the
// TraceCharacter .cpp family may include this.
//
// WHY THIS HEADER EXISTS AT ALL, given that the project's rule is "one class, one .cpp".
// TraceCharacter.cpp grew past ten thousand lines and was split (RESTRUCTURE tranche D) into the
// pawn itself plus TraceCharacterDebugCommands.cpp, which carries the fourteen `#if
// !UE_BUILD_SHIPPING` console commands. Those commands read the SAME measured constants the pawn is
// built from — the SMG's muzzle offset, the hands clips' emissive bands, the Mannequin's asset path
// — and a harness that measured against its own private copy of a number would be a harness that
// cannot fail. So the numbers moved here, verbatim, and both files read the one copy.
//
// EVERYTHING BELOW IS CONSTANT OR AN `extern` DECLARATION. The tables are `constexpr`/`const` at
// namespace scope, so each including TU gets its own read-only copy and there is nothing to link;
// the console variables are DECLARED here and DEFINED exactly once, in TraceCharacter.cpp, because
// a console variable registered twice would show up twice in the console.
//
// The comments on the constants are the project's measurements and the arguments behind them. They
// moved with the numbers, unedited. Do not summarise them.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"   // TAutoConsoleVariable — the extern blocks at the bottom

// Three constants below are DERIVED from other systems rather than copied, which is the project's
// rule for any number two features have to agree on. They are the reason this header needs module
// includes at all.
#include "Gameplay/TraceKnifeView.h"   // HandsActionPeakFraction = TraceKnifeView::StabPeakFraction
#include "Gameplay/TraceMelee.h"       // the dual-wield question, asked of the one owner of it
#include "TraceSettings.h"             // UTraceSettings, the settings-first tuning surface

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

	// --- WHERE A RIG POINT LANDS ON SCREEN, AS ONE EXPRESSION -------------------------------------
	//
	// *** THIS EXISTS BECAUSE THE ARITHMETIC ABOVE WAS DONE BY HAND, ONCE, FOR ONE POINT, AND THEN
	//     GENERALISED TO POINTS IT WAS NOT TRUE OF. *** Twice now. The wedge shipped because one
	//     forearm's elbow projection was quoted for both forearms (see HandsScale), and the pack
	//     rig then shipped with a lit cuff band whose centre is 21 px BELOW the bottom edge and a
	//     whole left forearm whose NEAREST end is 100 px below it — while a census line said both
	//     were "drawn", because they are: drawn is not the same as in frame.
	//
	// So the framing check is a function now, and both the comments below and Trace.Hands.Probe call
	// it rather than restating it. It is exactly the file header's own rule — "half-width of the
	// frame at distance d is d * tan(FirstPersonFieldOfView / 2), and half-height is that times
	// 9/16" — solved for the fraction instead of for the width, so the answer is a number that can
	// be compared against 1 without knowing the resolution.
	//
	// The FIRST-PERSON lens, not the scene's: FirstPersonPrimitives are drawn through
	// FirstPersonFieldOfView (80) and not CameraFOV (95), so projecting through the pawn camera —
	// which is what APlayerController::ProjectWorldLocationToScreen would do — answers 16% too near
	// the centre and would report an off-frame part as comfortably inside. FirstPersonScale does NOT
	// enter: it scales the rig radially about the view origin, which moves depth and leaves the
	// direction from the eye, and therefore the screen position, exactly where it was.
	constexpr float ViewModelFrameAspect = 9.f / 16.f;

	/**
	 * A camera-space point as a fraction of the half-frame: X right, Y up, both in [-1, 1] when the
	 * point is on screen. -1 is the bottom edge, +1 the top. Points behind the lens answer with a
	 * huge magnitude rather than wrapping, so a caller that only tests |v| <= 1 cannot be fooled.
	 */
	inline FVector2D ViewModelFrameFraction(const FVector& CameraPoint)
	{
		const double Depth = FMath::Max(CameraPoint.X, 0.01);
		const double HalfWidthPerUU = FMath::Tan(FMath::DegreesToRadians(FirstPersonViewModelFOV * 0.5f));
		return FVector2D(
			(CameraPoint.Y / Depth) / HalfWidthPerUU,
			(CameraPoint.Z / Depth) / (HalfWidthPerUU * ViewModelFrameAspect));
	}

	/** Engine basic shapes are 100 uu; every Size below is a world size and divides by this. */
	constexpr float ViewModelShapeUnit = 100.f;

	// --- THE CONSTANT LIGHT THE VIEWMODEL SUPPLIES ITSELF -----------------------------------------
	//
	// *** THIS ARENA DOES NOT LIGHT THE INSIDE OF A PLAYER'S FACE, SO ANYTHING HELD THERE HAS TO
	//     LIGHT ITSELF. *** The three directional lights total under 6 lux and none of them points
	//     at the rig; a surface at the arena's own albedo held 50 uu from the lens is therefore a
	//     black hole in the middle of the frame whatever its base colour is. The emissive term is
	//     the only lighting term that does not depend on an angle of incidence, which is the whole
	//     reason the procedural rig carries one — and the procedural rig is the one that photographs
	//     correctly.
	//
	// NAMED HERE RATHER THAN TYPED INTO ViewModelBodyMID, because the pack rig needs the SAME number
	// and did not have it. MI_Pack_shell and MI_Pack_carbon are exported with EmissiveColor (0,0,0)
	// — read them in Scripts/import_pack.py, MATERIALS — so every non-glowing surface of
	// SK_TraceHands renders as a silhouette: the palm, the fingers' bodies and the two forearm tubes
	// bound to the glove's own shell. What survives is `plating` (base 0.254, glossy) and
	// `circuit_cyan`, which is exactly the photographed complaint — a gun with some detached bright
	// chips beside it and no hand anywhere. BuildHandsEmissive writes a floor onto the gloves' unlit
	// slots for that reason.
	//
	// THE PACK'S TWO SURFACES ARE NOT BOTH ON THIS NUMBER, and the split is deliberate rather than
	// drift: the SLEEVE (HandsArmMID) is, because it is the procedural rig's own tube in a different
	// material and this is the value that tube photographs correctly at; the GLOVE is lifted above it
	// (TraceCharacterLayout::HandsGloveEmissiveStrength) because it has to read against a weapon that
	// is at this same near-black and the fallback fist never had one inside it.
	//
	// The two masters spell it differently and the product is what matters: M_TraceSurface takes
	// "Emissive" x "EmissiveStrength", M_TraceRailgun (which every MI_Pack_* instances) takes
	// "EmissiveColor" x "EmissiveIntensity", and the pack import folds strength into the colour and
	// leaves intensity at a clean 1.0. So the pack side is written as the PRODUCT of these two.
	const FLinearColor ViewModelBodyEmissiveColor(0.30f, 0.55f, 0.80f);
	constexpr float ViewModelBodyEmissiveStrength = 0.030f;

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

	// --- THE ONE POINT EVERY WEAPON IS PLACED AGAINST ---------------------------------------------
	//
	/**
	 * THE RIGHT HAND, IN RIG SPACE.
	 *
	 * It is VMHandR's position in the viewmodel parts table, and it is what HandsGripRig repeats for
	 * the pack rig — EnsureViewModelBuilt() asserts all three equal. It is named HERE, above the
	 * weapons rather than beside the hands, because every weapon origin below is
	 * (this) - (that weapon's scale) x (that weapon's own grip landmark): a weapon can only ever be
	 * placed where the hand already is, and resizing one can never slide it out of the fist.
	 */
	const FVector ViewModelRightHand(-0.8f, 0.f, -4.6f);

	/**
	 * *** THE WEAPON SIZE LAW, AND IT REPLACES TWO NUMBERS THAT WERE SIZED AGAINST EACH OTHER. ***
	 *
	 * WHAT WAS WRONG, AND IT IS IN THE FRAMES. RailgunScale 0.22 and SmgScale 0.30 were picked to
	 * MATCH THE TWO GUNS' DRAWN LENGTHS — 185.1 x 0.22 = 40.7 uu against 128.5 x 0.30 = 38.6 uu — on
	 * the argument that two guns the same size on screen read as siblings. Photographed, they read as
	 * ONE gun: Saved/Screenshots/v31size_BEFORE_41_key1_pistol_idle.png and
	 * v31size_BEFORE_44_key2_smg_idle.png — the two pre-fix frames, kept under a name the capture
	 * harness cannot overwrite — put the same silhouette in the same corner of the frame at the same
	 * length, and the SMG comes out 5% SHORTER than the pistol. In a shooter where which weapon you
	 * are holding has to be legible in a glance, that is a defect and not a style.
	 *
	 * WHAT THE PACK SAYS, and it is the only outside authority there is. PACK_README.md: "The weapons
	 * are authored larger than a human hand (pistol ~1.19 m, SMG ~1.25 m, Core 0.37 m against a
	 * 0.19 m hand). The first-person preview scales them to 0.34 m / 0.50 m / 0.30 m to read
	 * correctly." The pack therefore draws the SMG 47% LONGER than the pistol — the order every
	 * shooter draws them in, and the exact opposite of what shipped.
	 *
	 * SO THESE ARE TARGET DRAWN LENGTHS NOW, NOT SCALES, and the scale is arithmetic on top:
	 *
	 *     scale = (target drawn length in rig uu) / (the mesh's own measured length in mesh cm)
	 *
	 * with the mesh lengths read off Intermediate/Railgun/railgun_manifest.json and
	 * Intermediate/Smg/railgun_smg_manifest.json rather than off the README — the README's "pistol
	 * ~1.19 m" disagrees with its own export, whose Body spans x = -77.7 .. +107.4 cm = 185.1 cm, and
	 * the rule this file already keeps is that the measured number wins over the prose.
	 *
	 * THE PISTOL LANDS EXACTLY ON THE PACK'S NUMBER: 34.0 / 185.1 = 0.1837, drawn 0.340 m against the
	 * pack's 0.190 m hand, a hand-to-weapon ratio of 0.56 — the pack preview's 0.56 to two places,
	 * where the shipped rig was 0.47.
	 *
	 * THE SMG CANNOT, AND WHAT STOPS IT IS THE ONE MEASURED CONSTRAINT IN THIS FILE. The header's
	 * depth rule — nothing in the viewmodel may be DRAWN deeper than the 34 uu capsule radius,
	 * because past that it reaches surfaces the body itself cannot stand against — is a hard ceiling
	 * on grip-to-muzzle length, and the SMG is the rig that hits it, because the SMG is the one being
	 * GROWN — the pistol, which shrinks, walks away from the ceiling. The drawn depth of the SMG's
	 * muzzle tip (63.02 cm ahead of the mesh origin, 93.02 cm ahead of the grip) is
	 *
	 *     ( ViewModelRestLocation.X 56 + ViewModelRightHand.X -0.8
	 *       + scale x (muzzle tip 63.02 - grip -30.0) x cos(ViewModelRestRotation yaw 4 deg) )
	 *     x FirstPersonViewModelScale 0.40
	 *
	 * — the same walk that measured 33.2 uu at SmgScale 0.30 — and it passes 34.0 uu at scale 0.3211.
	 * SmgDrawnLengthUU 41.1 is scale 0.3198, i.e. that ceiling with half a millimetre of air under
	 * it: the tip lands at 33.95 uu drawn.
	 *
	 * WHAT THAT BUYS AND WHAT IT DOES NOT. The SMG is drawn 0.411 m, not the pack's 0.500 m, so it is
	 * 21% longer than the pistol rather than 47%. The ORDER is right, the two silhouettes are no
	 * longer interchangeable, and for the first time BOTH weapons are inside the depth rule — the
	 * shipped pistol measured 34.16 uu, i.e. it was over the rule the comment beside it cited. The
	 * missing 9 cm is owed to a measured constraint rather than to taste: reaching the full 0.50 m
	 * means pulling ViewModelRestLocation.X in from 56 to 49.6, which regrades the framing of the
	 * hands and of all three loadouts at once and is not this fix's to spend.
	 *
	 * RETUNING IS THE TWO DRAWN LENGTHS. Everything downstream — the origins, the hinge and foregrip
	 * offsets, the rail throw, the magazine drop, the muzzle markers and therefore ATraceTracer's
	 * beam origin — is expressed against them and follows without being touched.
	 */
	constexpr float RailgunBodyLengthCm = 185.1f;   // railgun_manifest.json, Body x span
	constexpr float SmgBodyLengthCm = 128.5f;       // railgun_smg_manifest.json, Body x span
	constexpr float RailgunDrawnLengthUU = 34.0f;   // the pack preview's 0.34 m, exactly
	constexpr float SmgDrawnLengthUU = 41.1f;       // the pack wants 50.0; the depth rule caps it here

	// --- The railgun ------------------------------------------------------------------------------
	//
	// The blocky procedural gun above is the FALLBACK now. When the imported railgun art resolves,
	// three static meshes stand in for the twelve gun cubes (the hands, knuckles, forearms and cuffs
	// are still built from the table — they hold the new weapon, they were never part of it).
	//
	// SCALE. The source model is authored 1:1 in metres and measures 185.1 cm nose to tail, which is
	// a real railgun and about six times the length of the cube gun it replaces. Everything is placed
	// against the mesh's own measured landmarks, so these numbers are the whole placement:
	//
	//   grip     mesh-local (-30.0, 0, -9.0) cm   ->  rig (-0.8, 0, -4.6), the right hand
	//   foregrip mesh-local (+30.0, 0, -4.5) cm   ->  rig, where the left hand is moved to
	//   muzzle   mesh-local (107.4, 0, +4.5) cm   ->  rig 24.5 uu out (it was 29.4 at the old 0.22)
	//
	// RailgunOrigin is therefore not a taste value: it is (right hand) - RailgunScale * (grip), and
	// it is now WRITTEN that way rather than pre-multiplied by hand, because the pre-multiplied
	// literal is exactly what goes stale the first time the size law above is retuned.
	constexpr float RailgunScale = RailgunDrawnLengthUU / RailgunBodyLengthCm;   // 0.1837

	/** Mesh-local landmarks, in the mesh's own centimetres. Read out of railgun_manifest.json. */
	const FVector RailgunGripLocal(-30.0f, 0.f, -9.0f);
	const FVector RailgunMuzzleLocal(107.4f, 0.f, 4.5f);

	const FVector RailgunOrigin = ViewModelRightHand - RailgunGripLocal * RailgunScale;

	/**
	 * The left hand moves off the cube gun's frame and onto the railgun's foregrip, dropped half the
	 * foregrip's depth so the fist closes around it rather than resting on top.
	 *
	 * IN THE MESH'S OWN CENTIMETRES, like every other landmark here, so the support hand follows the
	 * weapon when the weapon is resized. The foregrip is at mesh (+30.0, 0, -4.5); the -11.8 cm of Y
	 * is the fist standing off the barrel axis and the extra -8.7 cm of Z is that half-depth drop.
	 * At the old RailgunScale 0.22 this reproduces the hand-tuned rig points (12.4, -2.6, -5.5) and
	 * (14.5, -2.6, -4.5) it replaces, to the millimetre.
	 *
	 * IT IS LIVE ONLY ON THE FALLBACK RIG. With the pack hands up the left glove is posed by the
	 * animation and this is unused; with -TraceNoPackHands the cube left hand is placed here, and
	 * that is the frame the railgun's two-hand hold was photographed in.
	 */
	const FVector RailgunLeftHandLocal(30.0f, -11.8f, -13.2f);
	const FVector RailgunLeftKnuckleLocal(39.6f, -11.8f, -8.6f);
	const FVector RailgunLeftHand = RailgunOrigin + RailgunLeftHandLocal * RailgunScale;
	const FVector RailgunLeftKnuckle = RailgunOrigin + RailgunLeftKnuckleLocal * RailgunScale;

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
	//             far short of the muzzle (19.6 cube / 24.5 railgun) so it never crosses the barrel.
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
	// SCALE, AND IT IS THE LARGER OF THE TWO NOW. The two models are authored at 1:1 in metres and are
	// DIFFERENT LENGTHS — the railgun measures 185.1 cm nose to tail, the SMG 128.5 cm — so a single
	// scale would draw the SMG at three quarters the pistol's on-screen size. The v30 answer to that
	// was to pick the two scales so the two DRAWN lengths matched (40.7 uu against 38.5 uu), and a
	// photograph of the result is what retired it: two guns of the same length in the same corner of
	// the frame are not siblings, they are the same gun. The size law above replaces both numbers
	// with a target drawn length each, and the SMG's is 41.1 uu against the pistol's 34.0 — the
	// pack's own order (its preview draws the SMG 0.50 m against the pistol's 0.34 m), as far toward
	// the pack's own proportion as the depth rule allows.
	//
	// MEASURED, against the two checks the file header requires of every viewmodel constant, by
	// walking all 1993 vertices of the four imported meshes through the rig transform:
	//
	//   DEPTH    at the v30 scale 0.30 the deepest vertex sat at 82.9 uu -> 33.2 uu drawn at
	//            FirstPersonScale 0.40. SmgDrawnLengthUU 41.1 is chosen to keep exactly that bound at
	//            the new size: the muzzle tip lands at 33.9 uu drawn, inside the 34 uu capsule radius,
	//            so no part of it can reach a surface the body cannot already stand against. (The
	//            RAILGUN measured 34.1 uu at the old 0.22 — 0.1 uu OVER the rule it documents — and
	//            comes back to 32.1 uu at 0.1837, so both rigs keep the rule now.) Shallowest vertex
	//            44.4 uu -> 17.8 uu at 0.30, and it only recedes as the grip end shrinks toward the
	//            hand, so the 10 uu near plane is clear by more than it was.
	//   FRAMING  at 0.30 the highest point of the gun sat 31.5% of the frame's half-height BELOW the
	//            crosshair (the pistol's was 35.8%); the requirement is a quarter. Growing the gun
	//            about a grip that does not move raises that edge by the same 7% it grows, which is
	//            inside the margin — and the pistol, which shrinks, only clears the reticle by more.
	//   AND IT LANDS WHERE THE PISTOL LANDS. Both weapons hang off the same grip point at the same
	//            rest pose, so the hold is identical; what differs between them now is length, which
	//            is the entire point.
	//
	// PLACEMENT IS DERIVED, NOT CHOSEN. SmgOrigin = (right hand) - SmgScale * (grip landmark), the
	// identical construction RailgunOrigin uses, so the mesh's own grip lands in the hand that is
	// already there and the right hand does not move between the two weapons — nor when either
	// weapon is resized, which is the property that makes the size law above safe to retune:
	//
	//   grip     mesh-local (-30.0, 0, -8.5) cm  ->  rig (-0.8, 0, -4.6), the right hand
	//   muzzle   mesh-local ( 58.8, 0, +4.5) cm  ->  rig 27.6 uu out, vs the railgun's 24.5
	//
	// The grip and muzzle landmarks are the ones spec §2 names (+0.300 m and -0.588 m along the
	// source's -Z), read out of railgun_smg_manifest.json rather than out of the prose: the kit's own
	// README says the aperture is at -0.59 m, and the mesh says -0.588. 2 mm, but the measured one
	// wins, which is the rule §1 states.
	constexpr float SmgScale = SmgDrawnLengthUU / SmgBodyLengthCm;   // 0.3198
	const FVector SmgGripLocal(-30.0f, 0.f, -8.5f);
	const FVector SmgMuzzleLocal(58.8f, 0.f, 4.5f);
	const FVector SmgOrigin = ViewModelRightHand - SmgGripLocal * SmgScale;

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

	// --- THE PACK'S GLOVED HANDS  (spec v31 §6) --------------------------------------------------
	//
	// EVERY NUMBER BELOW WAS MEASURED OUT OF gloved_hands.glb, not chosen. The measuring script walked
	// all 72 mesh nodes of the export through their animated node transforms, converted to Unreal's
	// axes with the mapping the import run established (UE = (gl.x, gl.z, gl.y) x 100) and checked
	// itself against the imported asset's own bounding box: 22.90 x 40.09 x 25.23 uu, to the
	// centimetre. Anything below that looks like taste is arithmetic with its working shown.
	//
	// AXES. The pack authors its hands and its weapons with FORWARD along UE -Y and UP along +Z; this
	// project's viewmodel rig is +X out of the lens, +Y right, +Z up. Those differ by exactly one yaw
	// of +90 degrees (-Y -> +X, +X -> +Y, +Z -> +Z), which is HandsYaw. The same +90 is what the
	// import run reported for placing any pack weapon beside the shipped SM_Railgun_*.
	constexpr float HandsYaw = 90.f;

	/**
	 * *** SCALE, WHICH THE SPEC WARNED WOULD BITE, AND IT DID. ***
	 *
	 * The hands are authored LIFE SIZE — 0.19 m wrist to fingertip, 0.40 m including the forearm —
	 * and 1.0 is that size. The pack's own first-person preview keeps the hands at 1.0 and shrinks the
	 * WEAPONS to 0.34 m (pistol) / 0.50 m (SMG) / 0.30 m (Core) to make the proportion read.
	 *
	 * *** THE PARAGRAPH THAT USED TO STAND HERE DECLINED TO RESIZE THE WEAPONS, AND IT WAS THE
	 * WRONG HALF OF THE TRADE. *** It argued that the depth rule — nothing in the viewmodel may be
	 * drawn deeper than the 34 uu capsule radius once FirstPersonScale 0.40 is applied — capped the
	 * SMG below the pack's 0.50 m, and concluded from that cap that BOTH shipped weapon scales should
	 * stand and the hand be sized against them. Two things were wrong with it. The cap is real but it
	 * was overstated: 0.389 puts the SMG's deepest vertex at 36.6 uu drawn, not the ~43 uu claimed,
	 * because the arithmetic forgot that SmgOrigin slides back toward the hand as the gun grows. And
	 * a cap on ONE weapon is not a reason to leave the OTHER at a size that made the two guns the
	 * same length on screen. The weapons are sized now — see the weapon size law beside RailgunScale
	 * — and the hand keeps the pack's own 1.0.
	 *
	 * NOTHING DOWNSTREAM OF THE WEAPON SCALES HAD TO BE RETUNED BY HAND, which is the other half of
	 * why that paragraph was too cautious: the muzzle markers are children of the weapon bodies at
	 * their mesh-local landmarks, ATraceTracer reads the marker's world transform, and the origins,
	 * hinge offsets, rail throw and magazine drop are all expressed in mesh centimetres times the
	 * scale. Every one of them followed.
	 *
	 * WHAT 1.0 ACTUALLY PRODUCES, measured through the full rig transform at every one of the twenty
	 * clips:
	 *   * hand 19.0 uu drawn against the railgun's 34.0 uu and the SMG's 41.1 uu — ratios 0.56 and
	 *     0.46, against the pack preview's 0.56 and 0.38. The pistol is now exactly the pack's
	 *     proportion; the SMG is as close to its 0.38 as the depth rule permits, and it is at last on
	 *     the correct side of the pistol. (It was 0.47 and 0.49 — a pistol 20% too long and an SMG
	 *     23% too short, i.e. the two guns in the wrong order.)
	 *   * DEPTH, at rest: the shallowest ON-SCREEN point is 10.50 uu drawn (the right forearm in
	 *     Idle_Smg), clear of the 10 uu near plane; the deepest is 26.10 uu, well inside 34.
	 *
	 *     *** THE SENTENCE THAT USED TO FOLLOW THIS ONE WAS WRONG, AND IT IS WHY THE WEDGE SHIPPED. ***
	 *     It read: "the forearms' far ends do fall to 7.55 uu, and they are 264% of the frame's
	 *     half-height BELOW centre while they do — off screen". That is true of forearm_LEFT, whose
	 *     elbow corners project to py 1212-1645 on a 900 px frame. It is FALSE of forearm_RIGHT,
	 *     whose four elbow corners land at py 732, 799, 924 and 996 — two of them on screen — because
	 *     that arm points BACK AT THE LENS rather than downward. One arm's arithmetic was generalised
	 *     to both, and the frame nobody took would have said so in a second. The two forearms are now
	 *     not drawn at all (HandsHiddenBones, where the full measurement is); with them gone the
	 *     shallowest DRAWN point of the rig is 14.81 uu, so nothing is near-plane clipped either.
	 *   * The one exception, stated rather than hidden: Walljump_* plants the LEFT palm toward the
	 *     wall, which brings on-screen geometry to 8.03 uu drawn for about a fifth of that 0.85 s
	 *     clip. It is the authored motion — "left palm plants flat on the wall, shoves off" — and no
	 *     scale that keeps the hands legible avoids it.
	 *
	 * Retuning is this one number: everything else in this section is expressed against it.
	 */
	constexpr float HandsScale = 1.0f;

	/**
	 * THE GRIP, IN THE HANDS' OWN SPACE: the centre of the closed right fist, measured as the
	 * centroid of the palm, the four fingers' middle phalanges and the thumb's distal joint in
	 * Idle_Pistol at t=0.
	 *
	 * It is measured in the PISTOL idle and used for all four loadouts because the pack authors one
	 * fist and moves the WRIST: the fist centre relative to wrist_right is (+0.17, -6.08, -3.08) in
	 * the pistol idle, (+0.30, -6.12, -2.97) in the SMG's and (-0.21, -5.74, -3.58) in the knife's —
	 * a spread of 0.4 uu across the set. One constant is therefore correct for all of them, and a
	 * second would only be a second thing to keep in step.
	 */
	const FVector HandsFistLocal(4.47f, -8.13f, -4.68f);

	/**
	 * WHERE THAT FIST GOES IN RIG SPACE — and it is not a new number, it is ViewModelRightHand, the
	 * one point the whole viewmodel is built around. (-0.8, 0, -4.6) is where VMHandR sits in the
	 * procedural parts table, and it is what RailgunOrigin and SmgOrigin are DERIVED from ("origin =
	 * right hand - scale x grip landmark"), so both weapons put their grips exactly here whatever
	 * size they are drawn at.
	 *
	 * IT IS AN ALIAS RATHER THAN A COPY, deliberately: the two used to be the same three literals
	 * typed twice, and the failure mode of them drifting apart is a fist closed on empty air next to
	 * a floating gun. Placing the real fist on it is therefore the whole placement — the guns do not
	 * move when the hands are rebuilt, their muzzle markers do not move, and the hand closes around
	 * the grip that was already there. The checkf in EnsureViewModelBuilt() is what keeps this equal
	 * to the parts table if anyone edits either.
	 */
	const FVector HandsGripRig = ViewModelRightHand;

	/**
	 * *** THE ARMS THE PACK DOES NOT GIVE US, AIMED THE WAY A VIEWMODEL NEEDS. ***
	 *
	 * With forearm_right / forearm_left hidden (HandsHiddenBones) the pack rig is two gloves and
	 * nothing else, and two gloves with no arms float. The procedural rig already solved this and its
	 * solution was photographed working: two short tubes leaving the hands DOWNWARD and BACKWARD, off
	 * the bottom of the frame within a few centimetres. So the same two tubes are drawn under the pack
	 * hands — re-anchored onto the pack's own rig, and in the pack's own `shell` material so the arm
	 * and the glove read as one object rather than as a plastic tube pushed into a carbon glove. (The
	 * same material, at a lower emissive floor than the glove's: see HandsArmMID for why the sleeve
	 * and the hand had to stop sharing one instance.)
	 *
	 * *** THE PACK ARMS USED TO BORROW THE PROCEDURAL RIG'S DIRECTIONS AND THAT PUT THE SLEEVE IN
	 *     FRONT OF THE FIST. ***
	 *
	 * They were (-0.42, 0.36, -0.86) and (-0.40, -0.38, -0.86): 60 degrees below horizontal and, in
	 * the X term, LEANING BACK TOWARD THE LENS. On the cube rig that is harmless — the cube hand and
	 * the cube wrist are the same point, so a tube leaving it backwards leaves from the front of the
	 * hand and nothing of the hand is behind it. On the PACK rig they are not the same point, and the
	 * difference is the whole defect:
	 *
	 *     wrist_right   rig (-6.88, -0.17, -1.19)   camera depth 49.1   screen u +0.36  v -0.66
	 *     the fist      rig (-0.80,  0.00, -4.60)   camera depth 55.2   screen u +0.31  v -0.71
	 *
	 * The fist is 6 uu DEEPER than the wrist it hangs off and slightly LEFT of and BELOW it, and a
	 * 6.26 uu tube leaving that wrist downward subtends u +0.27..+0.42 at its near cap — which
	 * contains the fist — while standing 6 uu nearer the lens than the fist does. So the sleeve won
	 * the depth test over the hand and the hand was drawn INSIDE it: photographed at 5x in
	 * Saved/Screenshots/v33handsBEFORE_41_key1_pistol_idle.png, where the palm is simply not there and
	 * the glove's fingers stick out to the left of a black cylinder. That is the "no palm anywhere"
	 * verdict, and no amount of lighting the glove fixes it, because the palm is not dark — it is
	 * OCCLUDED. (Round 1 of this fix lit the glove and photographed exactly the same hole.)
	 *
	 * TWO CHANGES FOLLOW FROM IT AND THEY ARE BOTH RULES RATHER THAN VALUES.
	 *
	 *   1. THE RIGHT ARM ANCHORS ON THE FIST, NOT ON THE WRIST — HandsGripRig, the one point the whole
	 *      viewmodel is built around, which is also the DEEPEST point of the hand. Anchored there, the
	 *      tube's near cap is buried inside the closed fist (that is what a fist centroid IS) and
	 *      every point of the tube is at least as deep as the hand, so the glove draws over the
	 *      sleeve instead of the sleeve over the glove. It costs nothing to carry: the grip is rigid
	 *      with wrist_right, so the same HandsWristDelta still moves it.
	 *
	 *   2. NEITHER ARM MAY LEAN TOWARD THE LENS AGAIN. X is +0.10 on both now instead of -0.4: the
	 *      tube recedes very slightly as it falls, so its far end is its DEEPEST point rather than its
	 *      nearest. That is the same mistake the pack's own forearms made in the extreme
	 *      (HandsHiddenBones: camera axis (-0.99, -0.01, +0.17), straight at the eye) and it is worth
	 *      stating as a rule because it is the third time it has been paid for.
	 *
	 * WHAT THAT DOES TO THE THREE MEASURED BOUNDS, all recomputed through the same projection the
	 * numbers below were solved with:
	 *
	 *                       band v @ 5.7uu     far end v @ 19/18uu    far end DRAWN depth
	 *     right, was          -0.92               -1.10  (in frame!)      16.9 uu
	 *     right, now          -0.91               -1.36                   22.8 uu
	 *     left,  was          -1.47               -1.67                   13.5 uu
	 *     left,  now          -1.44               -1.93                   17.4 uu
	 *
	 * The band lands where HandsArmCuffAlongUU solved for it either way, both far ends leave the
	 * frame EARLIER than before, and the near-plane margin roughly doubles. That last column is not a
	 * bonus: the file records that Walljump_* used to bring on-screen geometry to 8.03 uu drawn, i.e.
	 * INSIDE the 10 uu near plane, and it did that because the arm was travelling toward the lens
	 * while the clip threw the hand at the wall. An arm that only ever recedes cannot do it.
	 *
	 * LENGTH is 19 / 18 uu, unchanged. From the new right anchor the tube crosses the bottom edge at
	 * t = 8.3 uu and runs to 19, so more than half of it is out of frame — which is what "the arms run
	 * off the bottom of the frame" is supposed to mean.
	 *
	 * DIAMETER is one number for both arms and it is the pack's, not a taste value: forearm_right is
	 * authored 7.0 x 5.6 uu in cross-section, and sqrt(7.0 x 5.6) = 6.26 is the round tube of equal
	 * area. Kept at 6.26 through the re-aim even though the tube reads THINNER now (107 px of a 1600
	 * px frame against 145 before), because the change is honest perspective — the sleeve is 7 uu
	 * further from the lens — and not a second, hidden taste value laid on top of a measured one.
	 *
	 * THE LIT BAND STAYS, AND IT IS THE GLOVE'S OWN CIRCUIT. The procedural rig puts a neon ring on
	 * each arm and the frame it produces is the composition this work is measured against — without
	 * it a shell-material tube is a black silhouette, because `shell` is authored near-black
	 * (0.041, 0.056, 0.078) and has no detail to catch a light. It is bound to the glove's own
	 * circuit_cyan dynamic instance rather than to the arena's neon, so it breathes on the idle
	 * curve and flares on an action with the rest of the glove.
	 *
	 * *** AND THE SENTENCE THAT USED TO SIT HERE PUT THAT BAND OFF THE BOTTOM OF THE SCREEN. ***
	 * It read: "SK_TraceHands' own cuff_right / cuff_light_right sit AT the wrist and reach about
	 * 6 uu past it, so the band goes 9 uu down the arm, where it reads as a band on a sleeve." The
	 * clearance is real. 9 uu is not where it puts you. See HandsArmCuffAlongUU: the band's centre
	 * lands 5% of the half-frame BELOW the bottom edge, so what shipped was a black tube with no
	 * band on it at all — "a gun floating with some chips beside it", photographed in
	 * Saved/Screenshots/prefix_41_key1_pistol_idle.png and prefix_44_key2_smg_idle.png.
	 */
	constexpr float HandsArmLengthRightUU = 19.f;
	constexpr float HandsArmLengthLeftUU = 18.f;
	constexpr float HandsArmDiameterUU = 6.26f;

	/**
	 * The two re-aimed pack directions, normalised in place by the builder. X is POSITIVE on both and
	 * that sign is the rule, not the magnitude — see the two numbered changes above. Kept as named
	 * constants rather than as literals in the Forearms table because the table's other row is the
	 * cube rig's, which must keep the directions it was photographed with.
	 */
	const FVector HandsArmDirectionRight(0.10f, 0.30f, -0.949f);
	const FVector HandsArmDirectionLeft(0.10f, -0.30f, -0.949f);

	/**
	 * *** HOW FAR DOWN EACH ARM THE LIT BAND SITS — SOLVED AGAINST THE FRAME, NOT CHOSEN. ***
	 *
	 * The band is the whole readability of the arm (the paragraph above says why), so the only
	 * requirement that matters is that it be IN the frame, and the fallback rig is the worked
	 * example because its band is the one that photographs correctly.
	 *
	 * RUN ViewModelFrameFraction ALONG EACH TUBE — the vertical fraction is the number, -1 being the
	 * bottom edge:
	 *
	 *            anchor         band            frame edge      band height
	 *   cube R   v -0.66 @ 2    v -0.90 @ 5     v -1 @ ~7.4     36 px of 900   <- ships, reads
	 *   pack R   v -0.66 @ 0    v -1.05 @ 9     v -1 @ ~8.0     38 px of 900   <- v31, invisible
	 *   pack R   v -0.66 @ 0    v -0.90 @ 5.7   v -1 @ ~8.0     37 px of 900   <- v32, a puck
	 *   pack R   v -0.71 @ 0    v -0.99 @ 8.0   v -1 @ ~8.3     36 px of 900   <- this constant
	 *
	 * So the pack band was placed at the fallback band's OWN frame fraction, -0.90, solved on the
	 * pack's anchor instead of copied as a distance: 5.7 uu down a 19 uu tube whose wrist starts
	 * 6 uu nearer the lens and 3 uu higher than the cube hand's.
	 *
	 * *** AND THEN RE-AIMING THE ARM TOOK THE TUBE OUT FROM UNDER IT AND TURNED IT INTO A LAMP. ***
	 *
	 * -0.90 was solved for a tube that LEANED TOWARD THE LENS and crossed the frame in front of the
	 * hand: a black cylinder needing one lit ring to be legible at all, seen edge-on, with tube above
	 * the ring and tube below it. Anchoring the right arm on the fist and giving both arms a positive
	 * X (HandsArmLengthRightUU) changed all three of those facts at once. The tube now leaves from
	 * INSIDE the glove and recedes, so (a) the length of it above the band is hidden behind the hand,
	 * and (b) it is seen nearly face-on, so the band presents its full disc rather than its edge.
	 * Photographed at 5.7 the result is a bright ellipse sitting directly under the fist with no
	 * visible sleeve joining them — a hand standing on a glowing puck, and the second-brightest object
	 * in the frame after the arena's own strip lights.
	 *
	 *            band centre v      what is above it            what the band reads as
	 *   at 5.7      -0.89           nothing — the glove          a pedestal under the hand
	 *   at 8.0      -0.99           ~60 px of visible sleeve     a cuff at the frame edge
	 *
	 * 8.0 puts the band's CENTRE on the bottom edge instead of a band-height clear of it, so its top
	 * half lights the last visible inch of sleeve and its bottom half is off screen. That is the
	 * requirement now — the arm is legible because the hand it grows out of is legible, and the band's
	 * job has shrunk from "be the whole arm" to "be where the arm leaves the frame".
	 *
	 * THE STAND-OFF CAME DOWN WITH IT, 2.0 -> 1.2. The 2.0 was bought to clear a 12 uu glove, because
	 * the band used to sit only 5.7 uu from a wrist that is buried in one. Eight uu down a receding
	 * tube there is no glove within reach, and 2.0 uu proud on a 6.26 uu tube is a ring almost a third
	 * again as wide as the arm it is on — which is the other half of why it read as a disc rather than
	 * as a cuff. 1.2 is still comfortably clear of the sleeve and no longer out-measures it.
	 *
	 * *** THE LEFT ARM CANNOT BE FIXED BY MOVING ITS BAND AND THAT IS NOT A BUG IN THIS CONSTANT. ***
	 * The pack's left WRIST rests at rig (-14.42, -13.64, -8.64), which is v = -1.22 — the arm's
	 * NEAREST point is already a fifth of the half-frame below the bottom edge, and every point
	 * further along it is lower. No band position and no direction puts a left forearm on screen from
	 * there, because THE LEFT HAND IS NOT ON SCREEN EITHER: the pack authors pistol and SMG as
	 * one-handed holds ("every weapon is a right-handed one-hand hold" — unreal-hands_README.md) and
	 * the free hand hangs below the frame. The tube is still built and still rides
	 * HandsOffWristDelta, so it arrives with the hand for the Core cradle and the wall jump, which
	 * are the clips that raise it. Trace.Hands.Probe prints this fraction per part so that "the
	 * census says VMForearmL is drawn" can never again be read as "there is a left forearm in frame".
	 */
	constexpr float HandsArmCuffAlongUU = 8.0f;
	constexpr float CubeArmCuffAlongUU = 5.f;
	constexpr float HandsArmCuffProudUU = 1.2f;
	constexpr float CubeArmCuffProudUU = 0.8f;

	/**
	 * *** HOW BRIGHT THE GLOVE'S OWN BODY IS LIT, AND WHY IT IS NOT ViewModelBodyEmissiveStrength. ***
	 *
	 * The unlit floor exists because MI_Pack_shell and MI_Pack_carbon export with EmissiveColor
	 * (0,0,0) and this arena puts under 6 lux on the inside of a player's face, so the palm, the back
	 * of the hand and the fingers' bodies render as a silhouette (the full argument is on
	 * ViewModelBodyEmissiveColor). That floor shipped at ViewModelBodyEmissiveStrength — the
	 * procedural rig's own number — on the reasoning that "one value, one place, two rigs" was the
	 * conservative choice.
	 *
	 * *** IT IS THE WRONG NUMBER FOR A HAND THAT IS HOLDING SOMETHING, AND THE KNIFE FRAME IS THE
	 *     PROOF. *** With no gun in it (Saved/Screenshots/v31integ_47_key3_knife_idle.png) the pack
	 * glove at that floor reads perfectly well: the back of the hand is a solid lit panel and the
	 * fingers hang off it. Put a weapon in the same hand
	 * (v31integ_41_key1_pistol_idle.png / _44_key2_smg_idle.png) and the hand disappears — because
	 * MI_Railgun_shell is NOT given the floor (deliberately: see PackShellSlot) and is therefore at
	 * almost exactly the same near-black as the glove. Grip and fist become one undifferentiated dark
	 * mass with the glove's `plating` chips and `circuit_cyan` runs floating on it, which is what an
	 * adversarial verifier photographed as "5-6 detached white/cyan plates alongside the grip with no
	 * palm anywhere".
	 *
	 * So the requirement is not "bright enough to see against a black arena" — the old number met
	 * that. It is "bright enough to READ AGAINST THE WEAPON IT IS HOLDING", and the weapon is the
	 * thing the old number was equal to. The gloves are lifted clear of it and the guns are not, which
	 * is the whole point: the fist is now the lightest solid mass in the frame and the gun is the
	 * silhouette it is drawn on, exactly as the fallback rig's fist-over-gun reads.
	 *
	 * THE SLEEVE STAYS DOWN AT THE OLD NUMBER (HandsArmMID). The two tubes are the largest objects in
	 * the viewmodel and they are drawn nearer the lens than anything else in it, so lifting them with
	 * the glove would simply move the frame's brightest mass from the hand to the arm. The step
	 * between them lands at the wrist, under the glove's own cuff plate and the lit band, which is
	 * where a sleeve-to-glove change of material belongs.
	 *
	 * *** HOW FAR TO LIFT IT WAS BRACKETED AND NOT GUESSED, AND THE FIRST GUESS WAS 80% TOO HIGH. ***
	 * Photographed at 0.30x, 0.60x and 1.00x of a first guess of 0.115 through this section's own live
	 * knob (three walks, no rebuild — that is what the knob is FOR), and measured as the median
	 * luminance of the 90x90 px block the fist occupies at Idle_Pistol:
	 *
	 *     the gun / the shipped floor   33      the state the "no palm" frames were taken in
	 *     0.30x  (0.035)                34      indistinguishable from it; not worth the change
	 *     0.60x  (0.069)                57      a hand that is plainly lighter than the gun in it
	 *     1.00x  (0.115)                84      pale plastic; the glove out-values the weapon
	 *
	 * 0.065 is the 0.60x column. The target is not "bright": the procedural fallback's fist medians
	 * SIXTEEN and reads perfectly, because a big flat cube reads by silhouette. This glove cannot —
	 * it is thirty small facets with bright chips on them — so it needs enough separation from the
	 * weapon to read as one object, and no more. Twice the gun is that; four times is a lamp.
	 *
	 * LIVE, because this is a taste value in a lighting rig and the shipped idiom for those in this
	 * module is a CVar (Trace.Core.FxGeometry, Trace.Fx.BeamScale): Trace.Hands.GloveFloor multiplies
	 * it, so re-tuning is -dpcvars and not a rebuild. See CVarTraceHandsGloveFloor.
	 */
	constexpr float HandsGloveEmissiveStrength = 0.065f;

	// --- Clip timing.  AUTHORED LENGTHS, MEASURED OFF THE GLB, NOT COPIED FROM THE PROSE ----------
	//
	// The pack's README rounds; the files do not, and where they disagree the file wins (which is the
	// rule the shipped SMG comment already sets). Draw is 0.5167 s and not 0.52; Shoot is 0.1667 s and
	// not 0.16. Each hand clip still matches its weapon clip EXACTLY, which is what "frame for frame"
	// actually means.
	//
	// TWO OF THEM ARE NOT COSMETIC ROUNDING AND BOTH ARE HANDLED HERE.

	/**
	 * *** A_Hands_Inspect_Knife IS 5.600 s AND THE KNIFE'S OWN Inspect IS 3.200 s. ***
	 *
	 * The hands README's pairing table calls them "frame-for-frame" and they are not — the ratio is
	 * exactly 1.75. The knife doc carries the authoritative four catch beats and they are laid out
	 * inside 3.20 s, so the KNIFE is the clock and the hand clip is played at 1.75x to land on it.
	 * Written as the two lengths rather than as "1.75" so a re-export of either file moves the rate
	 * with it. The real fix is the artist re-exporting the hand clip at 3.20 s.
	 */
	constexpr float HandsInspectAuthoredSeconds = 5.600f;
	constexpr float KnifeInspectAuthoredSeconds = 3.200f;

	/**
	 * A_Hands_Throw_Core is 1.050 s where the doc says 0.55 s, and it is NOT a mistake to truncate:
	 * the Core's own Pickup is 0.550 s and its Throw is 0.500 s, and 0.55 + 0.50 = 1.05, so the hand
	 * clip is wind-up plus follow-through in one take. It is played whole.
	 */
	constexpr float HandsThrowAuthoredSeconds = 1.050f;

	/**
	 * Stab_Knife, 0.300 s, played at RATE 1.0 — deliberately NOT stretched onto the 0.32 s gameplay
	 * swing lockout (TraceMelee::GetSwingAnimSeconds).
	 *
	 * Stretching would make the hand agree with the LOCKOUT and disagree with the BLADE: §5 plays
	 * A_Knife_Stab at its own authored 0.300 s, and the two clips are authored to match thrust for
	 * thrust. The spec is explicit — do not re-time clips that are already paired. What is left over
	 * is 20 ms of shooting lockout after the hand has finished, against a visibly desynchronised fist
	 * and blade if it went the other way.
	 *
	 * RELOAD IS THE OPPOSITE CASE AND *IS* STRETCHED, and the two are consistent rather than
	 * contradictory: there the weapon's own magazine is stretched onto the same gameplay clock (see
	 * UpdateSmgAnimation), so following the pairing means stretching. The rule is "match the weapon
	 * clip", not "always play at 1.0".
	 */
	constexpr float HandsStabAuthoredSeconds = 0.300f;

	/** Reload's position is not rate-driven at all: it is read straight off the weapon's replicated
	  * deadline every frame, so a cancelled or ability-shortened reload cannot desynchronise it. This
	  * is only the length that phase is mapped onto. */
	constexpr float HandsReloadAuthoredSeconds = 0.800f;

	/**
	 * Draw is played at its AUTHORED rate and allowed to overrun the pullout, which is the one place
	 * a clip is deliberately not stretched.
	 *
	 * The pullout is 0.2 s shipped and §1 makes the knife's 35% shorter still (~0.13 s); the authored
	 * wrist flip that snaps a balisong open is 0.5167 s. Compressing it 4x would not read as a flip,
	 * it would read as a glitch. The README's own rule makes the overrun safe: "actions start and end
	 * on their loadout pose", so the tail lands back on Idle_Knife wherever it is interrupted — the
	 * same argument the pack makes for Shoot_Smg overrunning one shot at 600 RPM.
	 */
	constexpr float HandsDrawAuthoredSeconds = 0.5167f;

	/** Jump 0.70 s, wall jump 0.85 s, both as authored and both one-shots. */
	constexpr float HandsJumpAuthoredSeconds = 0.700f;
	constexpr float HandsWalljumpAuthoredSeconds = 0.850f;

	// --- The gloves' emissive  (spec v32 §5) ------------------------------------------------------
	//
	// unreal-fx_README, "Gloved hands", in full:
	//
	//     "Idle 0.95-1.15x, rising to 2.7x cyan / 2.1x amber at the peak of any action. Drive it from
	//      the same curve as the weapon so hands and weapon pulse together."
	//
	// and unreal-hands_README's own table, which is the finer-grained version of the same sentence
	// and is where amber's idle band comes from — the FX doc quotes only cyan's:
	//
	//     | State      | circuit_cyan   | core_amber   |
	//     | Idle       | 0.95-1.15x     | 0.9-1.1x     |
	//     | Mid-action | up to 2.7x     | up to 2.1x   |
	//
	// 1.0 IS REST ON EVERY MI_Pack_* INSTANCE. The import folded the KHR emissive strengths (cyan
	// 1.5, amber 1.4) into EmissiveColor precisely so this scalar could mean "multiplier on rest", so
	// these numbers go into the parameter literally — the same convention TraceKnifeView's constants
	// block states for the blade, and the reason the gloves and the knife can share a material.
	constexpr float HandsCyanIdleLow  = 0.95f;
	constexpr float HandsCyanIdleHigh = 1.15f;
	constexpr float HandsCyanPeak     = 2.70f;

	constexpr float HandsAmberIdleLow  = 0.90f;
	constexpr float HandsAmberIdleHigh = 1.10f;
	constexpr float HandsAmberPeak     = 2.10f;

	/** Midpoints — what a rig writes at BUILD time, before any clock exists to breathe against. */
	constexpr float HandsCyanIdleMid  = 0.5f * (HandsCyanIdleLow + HandsCyanIdleHigh);
	constexpr float HandsAmberIdleMid = 0.5f * (HandsAmberIdleLow + HandsAmberIdleHigh);

	/**
	 * The idle breath, in RADIANS PER SECOND, and it is a function of the world clock rather than an
	 * accumulator — ATraceCore::UpdateCoreArtEmissive argues this at length and the argument is the
	 * same here: an accumulator drifts, double-advances across a hitch, and desynchronises between
	 * machines, while sin(t*w) is the same value everywhere at the same instant and cannot drift
	 * because it remembers nothing.
	 *
	 * 2.0 s per cycle, matching the Core's "slow ~2 s cycle" — the two are the same body's idle and
	 * beating at visibly different rates in one frame would read as a bug rather than as a choice.
	 */
	constexpr float HandsIdleBreathRadPerSecond = 2.f * PI / 2.0f;

	/**
	 * WHERE A HAND ACTION PEAKS, as a FRACTION of its clip, for the actions that have no weapon curve
	 * to borrow (see GetHandsActionPulse).
	 *
	 * *** IT IS NOT A NUMBER AT ALL ANY MORE: IT IS THE BLADE'S OWN CONSTANT. *** This used to read
	 * `= 0.35f` with a comment saying it was a knowing duplicate of the knife's StabPeakFraction,
	 * which lived in TraceKnifeView.cpp's file namespace and could not be reached. The v32
	 * integration pass published it as TraceKnifeView::StabPeakFraction, so the glove and the blade
	 * now peak on the same frame of a stab BECAUSE THERE IS ONE NUMBER, not because two files were
	 * given equal values — and a re-tune of the blade moves the glove with it, which is Demo 21's
	 * standing rule about derived values applied to a presentation seam.
	 *
	 * The hand clip and A_Knife_Stab are both 0.300 s and both played at rate 1.0, which is what
	 * makes one fraction serve both.
	 *
	 * It generalises honestly to the rest: every one of these clips is an impulse and a recovery
	 * (a thrust snapping back, a magazine dropping, a shove off a wall), so the front third is where
	 * the energy is in all of them.
	 */
	constexpr float HandsActionPeakFraction = TraceKnifeView::StabPeakFraction;

	/**
	 * The hump itself: 0 at both ends of the clip, 1 at HandsActionPeakFraction. Triangle for
	 * triangle UTraceKnifeViewSubsystem::StabFlare, off the same peak fraction, which is what lets a
	 * verifier grade the glove against the blade with one formula.
	 *
	 * IT IS A FUNCTION OF AN ALPHA HANDED TO IT, and never of a member. That is deliberate: its two
	 * callers both live inside UpdateHandsAnimation, at the two places a hand pose is actually
	 * written to the component, so the value published for a frame is by construction the value of
	 * the pose that frame DREW. When this shape was a member read from a second function later in the
	 * tick it was a frame late to be told the clip had moved — see HandsClipPulseNorm.
	 */
	inline float HandsActionFlare(float Alpha01)
	{
		const float Alpha = FMath::Clamp(Alpha01, 0.f, 1.f);
		const float Shaped = (Alpha <= HandsActionPeakFraction)
			? (Alpha / FMath::Max(KINDA_SMALL_NUMBER, HandsActionPeakFraction))
			: (1.f - (Alpha - HandsActionPeakFraction)
				/ FMath::Max(KINDA_SMALL_NUMBER, 1.f - HandsActionPeakFraction));
		return FMath::Clamp(Shaped, 0.f, 1.f);
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

	// --- THE PACK'S GLOVED HANDS  (spec v31 §6) ---------------------------------------------------
	//
	// COMMITTED ART, like the railgun and unlike the Mannequin: Art/Pack/models/gloved_hands.glb ->
	// /Game/Trace/Art/Pack/Hands, imported by Scripts/import-pack.sh. Present on a fresh clone once
	// `git lfs pull` has run, and OPTIONAL until then — a miss builds the procedural cube hands.
	//
	// FIVE SKELETONS EXIST IN THE PACK AND THEY DO NOT INTERCHANGE. A_Hands_* play ONLY on
	// SK_TraceHands_Skeleton; the knife's, the core's and the two weapons' clips are on their own
	// skeletons and cannot be cross-assigned. This file owns exactly the hand set.
	const TCHAR* const HandsMeshPath = TEXT("/Game/Trace/Art/Pack/Hands/SK_TraceHands.SK_TraceHands");

	/** The bone the hands README names as the weapon mount. Verified present on the imported skeleton
	  * (the GLB has no SOCKET_ leaf nodes, so Interchange created no sockets — a bone name resolves
	  * through the same attachment API). */
	const FName HandsWeaponBone(TEXT("wrist_right"));

	/** The off hand, reported through GetViewModelOffHand() so the knife rig lands on a real hand. */
	const FName HandsOffHandBone(TEXT("wrist_left"));

	/**
	 * *** THE TWO BONES THIS RIG DOES NOT DRAW, AND THE FRAME THAT SAYS WHY. ***
	 *
	 * The pack's export carries its own forearms: two 8-vertex boxes, 7.0 x 5.6 x 21.5 uu each,
	 * material `shell`, hanging off hand_right / hand_left as CHILDLESS LEAVES. They were meant to be
	 * the arm, and in a preview rendered from the side they are. In THIS rig they are not, because
	 * this rig's camera looks down the arm.
	 *
	 * MEASURED, at Idle_Pistol, through the full transform chain (HandsYaw 90 -> HandsLocation ->
	 * ViewModelRestRotation/Location -> FirstPersonViewModelFOV 80):
	 *   * forearm_right's long axis in CAMERA space is (-0.99, -0.01, +0.17). It points BACK AT THE
	 *     EYE with essentially no downward component, so instead of leaving the bottom of the frame
	 *     it flies at the lens and perspective flares it from 157 px wide at the wrist (48.5 uu real
	 *     depth) to 273 px at the elbow (27.3 uu). That trapezoid — 59,860 px, 4.16% of a 1600x900
	 *     frame, 81% of every pixel the whole hand rig draws — is the flat salmon WEDGE that the
	 *     shipped pack-hands frames have across them. It is not a skinning fault, not a material
	 *     fault and not a wrong slot: `shell` is bound correctly and its base colour is near-black.
	 *     It is a big untextured face catching the arena's warm rim light.
	 *   * forearm_left's elbow corners sit at 7.80-9.05 uu DRAWN depth, i.e. INSIDE the 10 uu near
	 *     plane, so the renderer cuts them open. That is the hard straight-edged salmon roof at the
	 *     bottom of the same frames.
	 *
	 * Compare the procedural fallback's forearms, which are 60 degrees below horizontal on
	 * (-0.42, 0.36, -0.86) and run off the bottom of a 900 px frame within 17 uu of the hand. That is
	 * what a viewmodel arm is supposed to do, and it is why the fallback frame reads as hands holding
	 * a gun while the pack frame reads as a wedge with a gun behind it.
	 *
	 * SO THE FOREARMS ARE HIDDEN AND THE CUFFS ARE KEPT. cuff_right / cuff_light_right are SIBLINGS
	 * under hand_right, not children, so hiding these two leaves the lit wrist band that gives the
	 * hand a clean cut-off — the same silhouette the fallback's neon cuff rings draw. Hiding is done
	 * per BONE and not per SECTION on purpose: Scripts/import_pack.py sets keep_sections_separate =
	 * False, so slot 0 `shell` is 22 nodes carrying BOTH forearms AND both palms AND ten finger
	 * segments, and hiding it would take the hands with it.
	 *
	 * Neither bone has children and neither is wrist_right, so nothing that places a weapon depends
	 * on them: UpdateWeaponsFollowHands composes every gun part from wrist_right alone.
	 */
	const FName HandsHiddenBones[] = { FName(TEXT("forearm_right")), FName(TEXT("forearm_left")) };

	/**
	 * THE TWENTY CLIPS, IN THE ORDER ResolveHandsClip() INDEXES THEM. The order is a contract between
	 * this table, the enum below it and the constructor's finder array; the static_assert in
	 * BuildPackHandsViewModel() is what stops the three drifting apart.
	 */
	const TCHAR* const HandsAnimPaths[] =
	{
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Idle_Knife.A_Hands_Idle_Knife"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Idle_Pistol.A_Hands_Idle_Pistol"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Idle_Smg.A_Hands_Idle_Smg"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Idle_Core.A_Hands_Idle_Core"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Draw_Knife.A_Hands_Draw_Knife"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Stab_Knife.A_Hands_Stab_Knife"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Inspect_Knife.A_Hands_Inspect_Knife"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Shoot_Pistol.A_Hands_Shoot_Pistol"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Reload_Pistol.A_Hands_Reload_Pistol"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Shoot_Smg.A_Hands_Shoot_Smg"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Reload_Smg.A_Hands_Reload_Smg"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Throw_Core.A_Hands_Throw_Core"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Jump_Knife.A_Hands_Jump_Knife"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Jump_Pistol.A_Hands_Jump_Pistol"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Jump_Smg.A_Hands_Jump_Smg"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Jump_Core.A_Hands_Jump_Core"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Walljump_Knife.A_Hands_Walljump_Knife"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Walljump_Pistol.A_Hands_Walljump_Pistol"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Walljump_Smg.A_Hands_Walljump_Smg"),
		TEXT("/Game/Trace/Art/Pack/Hands/Anims/A_Hands_Walljump_Core.A_Hands_Walljump_Core")
	};

	/** Index into HandsAnimPaths. Must stay in step with the table above and with the constructor. */
	enum EHandsClipIndex : int32
	{
		HandsClip_IdleKnife = 0,
		HandsClip_IdlePistol,
		HandsClip_IdleSmg,
		HandsClip_IdleCore,
		HandsClip_DrawKnife,
		HandsClip_StabKnife,
		HandsClip_InspectKnife,
		HandsClip_ShootPistol,
		HandsClip_ReloadPistol,
		HandsClip_ShootSmg,
		HandsClip_ReloadSmg,
		HandsClip_ThrowCore,
		HandsClip_JumpKnife,
		HandsClip_JumpPistol,
		HandsClip_JumpSmg,
		HandsClip_JumpCore,
		HandsClip_WalljumpKnife,
		HandsClip_WalljumpPistol,
		HandsClip_WalljumpSmg,
		HandsClip_WalljumpCore,
		HandsClip_Count
	};

	static_assert(UE_ARRAY_COUNT(HandsAnimPaths) == HandsClip_Count,
		"The hand clip path table and the index enum have drifted apart.");

	/** Short names for the log and for Trace.Hands.Probe, in the same order. */
	const TCHAR* const HandsClipNames[] =
	{
		TEXT("Idle_Knife"), TEXT("Idle_Pistol"), TEXT("Idle_Smg"), TEXT("Idle_Core"),
		TEXT("Draw_Knife"), TEXT("Stab_Knife"), TEXT("Inspect_Knife"),
		TEXT("Shoot_Pistol"), TEXT("Reload_Pistol"), TEXT("Shoot_Smg"), TEXT("Reload_Smg"),
		TEXT("Throw_Core"),
		TEXT("Jump_Knife"), TEXT("Jump_Pistol"), TEXT("Jump_Smg"), TEXT("Jump_Core"),
		TEXT("Walljump_Knife"), TEXT("Walljump_Pistol"), TEXT("Walljump_Smg"), TEXT("Walljump_Core")
	};

	static_assert(UE_ARRAY_COUNT(HandsClipNames) == HandsClip_Count,
		"The hand clip name table and the index enum have drifted apart.");

	/**
	 * WHAT EACH CLIP IS SUPPOSED TO BE, so a re-export that changes a length is CAUGHT rather than
	 * discovered later as "the reload looks wrong now".
	 *
	 * These are the lengths measured out of gloved_hands.glb, not the ones the pack's README quotes —
	 * the README rounds (0.52 for 0.5167, 0.16 for 0.1667) and where the two disagree the file wins.
	 * BuildPackHandsViewModel checks every imported sequence against this table and says so on a
	 * mismatch; that is what makes the constants below load-bearing instead of decorative. Two entries
	 * are known to disagree with the pack's PROSE and both are deliberate — see
	 * HandsInspectAuthoredSeconds (5.600 against a documented 3.20) and HandsThrowAuthoredSeconds
	 * (1.050 against a documented 0.55).
	 */
	const float HandsClipAuthoredSeconds[] =
	{
		2.4f, 2.4f, 2.4f, 2.4f,                                     // the four idles, all looping
		TraceCharacterLayout::HandsDrawAuthoredSeconds,
		TraceCharacterLayout::HandsStabAuthoredSeconds,
		TraceCharacterLayout::HandsInspectAuthoredSeconds,
		0.1667f, TraceCharacterLayout::HandsReloadAuthoredSeconds,  // pistol: shoot, reload
		0.1667f, TraceCharacterLayout::HandsReloadAuthoredSeconds,  // smg: shoot, reload
		TraceCharacterLayout::HandsThrowAuthoredSeconds,
		TraceCharacterLayout::HandsJumpAuthoredSeconds,
		TraceCharacterLayout::HandsJumpAuthoredSeconds,
		TraceCharacterLayout::HandsJumpAuthoredSeconds,
		TraceCharacterLayout::HandsJumpAuthoredSeconds,
		TraceCharacterLayout::HandsWalljumpAuthoredSeconds,
		TraceCharacterLayout::HandsWalljumpAuthoredSeconds,
		TraceCharacterLayout::HandsWalljumpAuthoredSeconds,
		TraceCharacterLayout::HandsWalljumpAuthoredSeconds
	};

	static_assert(UE_ARRAY_COUNT(HandsClipAuthoredSeconds) == HandsClip_Count,
		"The hand clip length table and the index enum have drifted apart.");

	/**
	 * The two roots of the pack rig. unreal-hands_hands_stats.json, "rig": { "roots": ["hand_right",
	 * "hand_left"] } — each side hangs off its own root, so one hidden root takes that whole hand
	 * with it (palm, five digits, cuff, cuff light) and touches nothing on the other side.
	 */
	const FName HandsOffHandRootBone(TEXT("hand_left"));

	/**
	 * *** WHETHER THE PACK'S LEFT HAND IS DOING ANYTHING THE PLAYER CAN SEE, PER CLIP. ***
	 *
	 * The pack authors every weapon as a RIGHT-HANDED ONE-HAND HOLD and says so in its own data:
	 * unreal-hands_hands_stats.json's `loadouts` gives left = "open, free" for knife, pistol and smg,
	 * and left = "core cradle" for the Core alone. What "open, free" means on screen is that the left
	 * wrist hangs at rig (-14.42, -13.64, -8.64), which is v = -1.14 through the first-person lens:
	 * a fifth of the half-frame BELOW the bottom edge.
	 *
	 * THAT IS NOT THE SAME AS "OFF SCREEN", AND THAT IS THE WHOLE DEFECT. The wrist is off; the palm
	 * is 19 uu long and hangs off the wrist toward the lens, so its top edge and its `plating`
	 * knuckle caps land between v = -0.95 and -0.78 (the handL_top row in Trace.Hands.Probe's framing
	 * block measures it per frame, and names the bone) — on screen, in the bottom-left corner, as a bright
	 * salmon-white shard with no hand attached to it, in EVERY one-handed frame we have photographed.
	 * It is the second-most distracting thing in the viewmodel after the forearm wedge, and it is the
	 * same class of error: a part that is drawn but not composed.
	 *
	 * *** THE RELOAD IS NOT AN EXEMPTION, AND THE FIRST VERSION OF THIS TABLE SAID IT WAS. ***
	 *
	 * It shipped with Reload_Pistol and Reload_Smg marked SHOWN, on the reading that those two clips
	 * "bring the support hand up to the gun". They do raise it. They do not raise it into the frame,
	 * and the numbers quoted here as the proof — a left wrist reaching v = -1.00 (pistol) and -0.96
	 * (smg) — were themselves the disproof: -1 IS the bottom edge, so the best frame of the whole
	 * clip has the wrist between four hundredths of a half-frame above the edge and dead on it.
	 * Swept at eight playhead positions on each clip (Trace.Hands.Hold + Trace.Hands.Probe, live
	 * match so the sway and the bob are in the numbers; Saved/Logs/shard-sweep.log), wristL reads
	 *
	 *   Reload_Pistol  a=.00 -1.15  .15 -1.04  .30 -0.97  .45 -1.22  .60 -1.18  .75 -1.03  .90 -1.00  .99 -1.14
	 *   Reload_Smg     a=.00 -1.12  .15 -1.02  .30 -0.96  .45 -1.20  .60 -1.16  .75 -1.01  .90 -0.98  .99 -1.11
	 *
	 * — a peak of -0.96 at a = 0.30, eighteen pixels of a 900 px frame, and the ARM below that wrist
	 * never gets nearer than v = -1.13: VMForearmL and VMCuffL print OFF-FRAME at all sixteen
	 * samples. What DOES get in is the palm, and the handL_top row says by how much: on the beat this
	 * walk photographs, wristL = -1.00 / handL_top[index_seg_left] = -0.81 on the pistol and
	 * wristL = -0.96 / handL_top[index_left] = -0.78 on the SMG. A fifth of the half-frame of glove,
	 * with its wrist on the edge and its forearm off — the shard of the paragraph above, exempted by
	 * name. It is in v34knifeFINAL_43_pistol_reload.png and _46_smg_reload.png pixel-for-pixel as it
	 * is in the pre-fix v33handsBEFORE frames, while the idles beside them came out clean;
	 * v35shardFIX_43 and _46 are the same two beats with the table corrected.
	 *
	 * BOTH RELOADS ARE THEREFORE HIDDEN TOO, and that cannot uncap a bare forearm the way hiding the
	 * glove does elsewhere, because on these two clips the tube is off the bottom of the frame at
	 * every sample above — checked in the frame as well as in the log: the bottom strip of
	 * v35shardFIX_43 and _46 is empty arena, not a floating tube. What survives as a real exemption is the clip that puts the WHOLE hand in
	 * frame rather than its top edge: the four Core clips cradle it in front of the lens, and
	 * Walljump_* plants the palm flat on the wall at v = -0.76. So the rule is per CLIP, not per rig
	 * and not per loadout, and it asks "is this hand COMPOSED into the shot", not "does this hand
	 * move":
	 *
	 *   SHOWN: the four Core clips — idle, throw, jump and walljump, both hands on the Core — and all
	 *          four Walljumps (the palm plants on the wall).
	 *   HIDDEN: everything else. The three one-handed idles, both shoots, BOTH RELOADS, the knife's
	 *          draw/stab/inspect and the three non-Core jumps — every clip where the pack's own data
	 *          says the hand is "open, free" and every clip where raising it does not reach the frame.
	 *
	 * IT FLIPS ONLY ON A CLIP CHANGE, never on a per-frame framing test. A visibility decision taken
	 * against "is any of it inside the frame this tick" pops the moment a breath cycle crosses the
	 * threshold — and the reload sweep above, which crosses v = -1 four times in 0.8 s, is exactly
	 * the clip that would strobe. A clip change is already a re-pose of the whole hand and is the
	 * only moment where an appearance cannot read as a flicker.
	 */
	const bool HandsClipShowsOffHand[] =
	{
		false, false, false, true,     // Idle: knife, pistol, smg, CORE (both hands cradle it)
		false, false, false,           // Draw_Knife, Stab_Knife, Inspect_Knife — right hand only
		false, false,                  // pistol: Shoot, RELOAD (the reload's peak is v = -0.97: the edge)
		false, false,                  // smg:    Shoot, RELOAD (peak v = -0.96, likewise the edge)
		true,                          // Throw_Core — the cradle opens
		false, false, false, true,     // Jump: knife, pistol, smg, CORE
		true, true, true, true         // Walljump: all four plant the left palm on the wall
	};

	static_assert(UE_ARRAY_COUNT(HandsClipShowsOffHand) == HandsClip_Count,
		"The off-hand visibility table and the index enum have drifted apart.");

	/** Material slot names baked into the meshes, used to find the two glowing slots to animate.
	  * The SMG's are the same two FNames — the import writes the raw glTF material names undecorated
	  * on both weapons, which is why one pair of constants serves both. */
	const FName RailgunCyanSlot(TEXT("circuit_cyan"));
	const FName RailgunAmberSlot(TEXT("core_amber"));

	/**
	 * THE GLOVES' UNLIT SLOTS, WHICH ARE THE ONES THE PLAYER CANNOT SEE.
	 *
	 * SK_TraceHands carries five: shell, carbon, circuit_cyan, plating, core_amber. Two of those
	 * glow and are driven above; `plating` is exported at base 0.254 and glossy, which is bright
	 * enough to read on its own. These two are not — shell is 0.041 and carbon is 0.0086, both with
	 * EmissiveColor (0, 0, 0) — and between them they are the palm, the back of the hand and the
	 * fingers' bodies, i.e. the entire shape of the fist. See ViewModelBodyEmissiveColor for the
	 * lighting argument and BuildHandsEmissive for the write.
	 *
	 * NAMED SEPARATELY FROM THE RAILGUN'S PAIR ON PURPOSE. The same FNames appear on the weapons'
	 * meshes, and the weapons are NOT given this floor: they are lit well enough by their own
	 * plating and their own circuit runs, and a gun is a silhouette against a bright floor while a
	 * hand is a shape that has to be read against the gun.
	 */
	const FName PackShellSlot(TEXT("shell"));
	const FName PackCarbonSlot(TEXT("carbon"));

#if !UE_BUILD_SHIPPING
	// --- THE OWNER'S OWN FIRST-PERSON ARMS RIG  (demo 29 item 2) ---------------------------------
	//
	// A TEST FIXTURE, PRACTICE RANGE ONLY, AND THAT IS THE WHOLE SCOPE. Demo 29 asked to *test* this
	// rig, not to replace SK_TraceHands, which is shipped, photographed and correct. Everything below
	// is behind TracePracticeRange::ShouldUseOwnerArmsViewModel() and compiles out of Shipping.
	//
	// WHAT THE ASSET IS. `Art/Characters/Hands/HandModel2.fbx` (Blender Rigify, 1,251 model nodes,
	// 336 verts, 47 weighted clusters) rebuilt by Scripts/import_hands.py into a clean 51-bone
	// SkeletalMesh; the full derivation is in Art/Characters/Hands/SOURCE_NOTES.md. It is a BODY rig,
	// not a view model, and three measured facts follow from that and drive the placement code:
	//
	//   * it is TWICE the size the first-person camera is tuned for — hand_right -> index_right_2 is
	//     17.83 uu against the pack hands' 9.14 (1.95x), and the knuckle spread is only 1.30x, so it
	//     is a differently proportioned hand rather than a scaled copy of the pack's;
	//   * it is T-POSED, hands 142.83 uu apart against the pack's 18.46;
	//   * it sits at Z 136.7..161.1 uu — a standing character's shoulder height — not at the
	//     component origin where SK_TraceHands sits (Z -17.07..8.15).
	//
	// So the fixture owns a component transform as well as a mesh, and `arms_root` at the origin
	// exists to give that a handle. There are TWO such transforms and which one is used depends on
	// whether a POSE is on the rig, because a solved pose carries its own placement (below):
	//
	//   * NO POSE — BuildOwnerArmsViewModel() DERIVES a placement from the two skeletons rather than
	//     typing it, landing this rig's `hand_right` on the pack rig's wrist, so a re-export retunes
	//     itself. It is what the bind pose is drawn at, and it is the degrade path;
	//   * A POSE — ArmsPoseScale / ArmsPoseYawDegrees / ArmsPoseTranslationRig, which is the
	//     placement the poses were SOLVED against and is part of the pose deliverable.
	//
	// The constants below are the bone names the derivation reads, one fallback for the case where a
	// bone has been renamed, the four poses, and that second placement.
	//
	// NAMES, NOT Rigify's. UE 5.8 headless CANNOT rename a bone after import (measured, wave 3), so
	// the rebuild emits the names the shipped hands already use — `hand_right`, `index_right_2` — and
	// that is why the two rigs can be measured against each other by one pair of FNames below.
	const TCHAR* const ArmsMeshPath = TEXT("/Game/Trace/Characters/Hands/SK_TraceArms.SK_TraceArms");

	/**
	 * The right hand's root, on BOTH rigs, and the point the fixture is anchored by.
	 *
	 * NOT `wrist_right`: the owner's rig has no such bone. That asymmetry is the reason the arms are
	 * an ADDITIONAL component rather than a mesh swapped into HandsPart — every weapon in this
	 * viewmodel is composed off `wrist_right`, and a rig that does not carry it cannot be asked to
	 * hold them. See BuildOwnerArmsViewModel for the full argument.
	 */
	const FName ArmsHandBone(TEXT("hand_right"));

	/**
	 * The right forearm. On the OWNER's rig this is the real forearm and `hand_right`'s PARENT; on
	 * the pack rig the same name is a decoration leaf parented UNDER the hand. Same anatomy, opposite
	 * parentage — so this name may only ever be read on the arms skeleton, and it is read for exactly
	 * one thing: the hand -> elbow axis the whole rig is rotated by.
	 */
	const FName ArmsForearmBone(TEXT("forearm_right"));

	/**
	 * The right index fingertip, present on BOTH rigs (TraceKnifeView.cpp builds the knife's hold
	 * basis out of it, so the pack rig's copy is load-bearing elsewhere). `hand -> fingertip` is the
	 * one length that means the same thing on two differently proportioned hands, which is what makes
	 * it the scale yardstick.
	 */
	const FName ArmsFingerTipBone(TEXT("index_right_2"));

	/**
	 * The scale to fall back to when either skeleton stops carrying the two bones above.
	 *
	 * MEASURED, not chosen: 9.14 / 17.83 = 0.5126 is the pack hand's length over the owner's, i.e.
	 * the number the runtime derivation produces today. It exists so that a renamed bone degrades to
	 * a rig at roughly the right size with a warning, rather than to a pair of arms at twice scale
	 * filling the frame — which is the failure this codebase keeps meeting as "the fix silently
	 * stopped applying".
	 */
	constexpr float ArmsFallbackScale = 0.5126f;

	// --- THE FOUR SOLVED HOLD POSES, AND THE PLACEMENT THEY WERE SOLVED AGAINST -------------------
	//
	// Scripts/pose_hands.py's deliverable. Each is a ONE-FRAME AnimSequence on SK_TraceArms_Skeleton
	// (0.033 s, two identical keys) holding all 51 bones: a two-bone IK put each hand on the weapon's
	// grip and every finger was closed by binary search until its own measured flesh sat on the
	// weapon's triangles. They are POSES, not clips — nothing in them moves, and UpdateOwnerArmsPose
	// pins the playhead at 0 rather than letting a 33 ms clip free-run.
	//
	// *** INDEXED BY ATraceCharacter::EHandsLoadout, WHICH IS CHECKED BY static_assert AT THE ONE
	// PLACE THAT INDEXES THEM *** (UpdateOwnerArmsPose, which is the only file where both this table
	// and that enum are visible). So the hand shape the pack rig is already playing picks the arms'
	// pose by construction, and the two rigs cannot end up holding different weapons.
	//
	// THE CORE'S ROW IS THE FIST, and it is a deliberate substitute rather than a missing entry: the
	// Core is the two-hand cradle, it is carried in THIRD person, and the viewmodel is hidden for the
	// whole of it (see UpdateHandsAnimation). No cradle pose was solved because there is no weapon
	// geometry to solve one against; a closed fist is the honest neutral if the rig is ever seen in
	// that loadout, and it is what the ring-finger fix was photographed on.
	const TCHAR* const ArmsPosePaths[] =
	{
		TEXT("/Game/Trace/Characters/Hands/Poses/A_TraceArms_Knife.A_TraceArms_Knife"),
		TEXT("/Game/Trace/Characters/Hands/Poses/A_TraceArms_Pistol.A_TraceArms_Pistol"),
		TEXT("/Game/Trace/Characters/Hands/Poses/A_TraceArms_Smg.A_TraceArms_Smg"),
		TEXT("/Game/Trace/Characters/Hands/Poses/A_TraceArms_Fist.A_TraceArms_Fist")
	};

	/** Index into ArmsPosePaths. Must stay in step with ATraceCharacter::EHandsLoadout. */
	enum EArmsPoseIndex : int32
	{
		ArmsPose_Knife = 0,
		ArmsPose_Pistol = 1,
		ArmsPose_Smg = 2,
		ArmsPose_Fist = 3,
		ArmsPose_Count
	};
	static_assert(UE_ARRAY_COUNT(ArmsPosePaths) == ArmsPose_Count,
		"The arms pose paths and the arms pose index enum have drifted apart.");

	/** For the log line and Trace.Hands.Probe, in the same order. */
	const TCHAR* const ArmsPoseNames[] =
	{
		TEXT("A_TraceArms_Knife"),
		TEXT("A_TraceArms_Pistol"),
		TEXT("A_TraceArms_Smg"),
		TEXT("A_TraceArms_Fist")
	};
	static_assert(UE_ARRAY_COUNT(ArmsPoseNames) == ArmsPose_Count,
		"The arms pose names and the arms pose index enum have drifted apart.");

	/**
	 * *** THE POSE AND THIS TRANSFORM ARE ONE DELIVERABLE. YOU MAY NOT MIX POSE A WITH PLACEMENT B. ***
	 *
	 * Every pose above was solved with the component sitting HERE, in rig space: the shoulders were
	 * pinned at rig (-11.0, +/-8.86, -16.0) by this transform and the arms were then IK'd from there
	 * onto the weapons. Draw the same pose under a different component transform and the solved hands
	 * land wherever that transform happens to put them — which is exactly what a first attempt at
	 * this did, and it is why the numbers are HERE, beside the assets they belong to, instead of
	 * being re-derived by geometry that knows nothing about how the poses were solved.
	 *
	 * NOT TYPED, AND NOT INDEPENDENT OF THE ASSETS: pose_hands.py writes these three numbers into
	 * `placement` in Intermediate/Hands/TraceArmsPoses_manifest.json on every run, and they are
	 * copied from it. Re-solve the poses and copy them again if that block has changed.
	 *
	 *   scale 0.5126192 — the SAME ratio BuildOwnerArmsViewModel measures at runtime (pack
	 *     `hand_right -> index_right_2` over this rig's), quoted here to the solver's precision;
	 *   yaw -90 deg — the rig lays its arms along component +/-X and rig space wants them along +/-Y;
	 *   translation rig (-9.7007, 0, -94.6355) — drops a standing body rig's shoulder height onto the
	 *     viewmodel's, with the shoulders CHEATED FORWARD on purpose (an anatomically placed shoulder
	 *     is ~66 uu from the grip and this arm is 57.9 x 0.5126 = 29.7 uu long, so it cannot reach;
	 *     every first-person view model resolves that the same way).
	 */
	constexpr float ArmsPoseScale = 0.5126192f;
	constexpr float ArmsPoseYawDegrees = -90.f;
	const FVector ArmsPoseTranslationRig(-9.7007f, 0.f, -94.6355f);
#endif // !UE_BUILD_SHIPPING

	/** Printed at most once per process, so a missing import is loud but not spam. */
	const TCHAR* const MissingImportHint =
		TEXT("Character art is not imported. Run Scripts/import-mannequin.sh to copy Epic's Mannequin ")
		TEXT("out of your own UE 5.8 install into Content/Characters/Mannequins. ")
		TEXT("Falling back to plain team-coloured shapes.");

	/** The one command that fixes it. Quoted verbatim on screen and in the log, so nobody has to guess. */
	const TCHAR* const ImportCommand = TEXT("./Scripts/import-mannequin.sh");
}

// =================================================================================================
// Console variables owned by TraceCharacter.cpp, declared here for its sibling TUs
// =================================================================================================
//
// DECLARATIONS ONLY. Each of these is constructed exactly once, in TraceCharacter.cpp, next to the
// code and the essay it belongs to — registering a console variable twice would put two rows with
// the same name in the console. The debug-command TU reads them so that what a harness PRINTS and
// what the live rule OBEYS are provably the same object, which is the whole point of the red arms
// built on top of them (Trace.Bounds.Enabled 0, Trace.Characters.BodyAnimIgnore 1, ...).

namespace TraceCharacterBody
{
	/** Trace.Characters.BodyAnimIgnore — see the essay on the definition in TraceCharacter.cpp. */
	extern TAutoConsoleVariable<int32> CVarBodyAnimIgnore;

	/** Trace.Characters.BodyMeshEventsOnly — ditto. */
	extern TAutoConsoleVariable<int32> CVarBodyMeshEventsOnly;

	/** Trace.Characters.BodyMeshKeepOverrides — ditto. */
	extern TAutoConsoleVariable<int32> CVarBodyMeshKeepOverrides;
}

namespace TraceCharacterBounds
{
	// SPEC v19 §4.1. The five knobs of the out-of-bounds rule. Trace.Bounds.Verify prints all five
	// before it runs, so a reader of the log can tell a genuine PASS from a pass bought by somebody
	// having widened the margins; and Trace.Bounds.Enabled is the arm the harness must go RED on.
	// The full argument for each default is on the definition in TraceCharacter.cpp.
	extern TAutoConsoleVariable<int32> CVarBoundsEnabled;
	extern TAutoConsoleVariable<float> CVarBoundsMarginXY;
	extern TAutoConsoleVariable<float> CVarBoundsMarginBelow;
	extern TAutoConsoleVariable<float> CVarBoundsCeilingMargin;
	extern TAutoConsoleVariable<float> CVarBoundsGraceSeconds;
}
