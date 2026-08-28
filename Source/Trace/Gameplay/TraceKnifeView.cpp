// Copyright Trace. All Rights Reserved.

#include "Gameplay/TraceKnifeView.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Camera/CameraComponent.h"          // the streak billboards at the lens (spec v32 §4)
#include "Camera/CameraTypes.h"              // FMinimalViewInfo::TransformWorldToFirstPerson
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"  // the streak plane
#include "Containers/Ticker.h"               // FTSTicker — Trace.Knife.PackDemo spans frames
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"       // the streak probe reports the viewport it measured in
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                     // TActorIterator
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"             // MakeFromXZ — the streak's billboard basis
#include "Math/UnrealMathUtility.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ReferenceSkeleton.h"               // the tip's bone is MEASURED off the ref pose
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"

#include "Core/TraceCharacter.h"
#include "Gameplay/TraceFxShapes.h"
#include "Gameplay/TraceMelee.h"
#include "Trace.h"

// Named after the file and NEVER anonymous: this module is a unity build and
// Scripts/check-jumbo-build-collisions.py gates on exactly that. Four Windows-only breaks in this
// project's history were this mistake.
namespace TraceKnifeViewFile
{
	// =============================================================================================
	// THE ASSETS. Paths are the ones the import pass created and reported; nothing here invents one.
	// =============================================================================================
	//
	// The import pass's own note is worth repeating because it is the thing most likely to trip
	// somebody up: THERE ARE FIVE SKELETONS, NOT ONE. A_Knife_* play ONLY on SK_TraceKnife_Skeleton
	// and cannot be cross-assigned to the hands. The hand/knife pair is therefore TWO
	// SkeletalMeshComponents each running its own clip, kept in step by STARTING THEM ON THE SAME
	// FRAME — not one animation on one skeleton. This file owns the knife half; §6 owns the hands.
	static const TCHAR* MeshPath   = TEXT("/Game/Trace/Art/Pack/Knife/SK_TraceKnife.SK_TraceKnife");
	static const TCHAR* IdlePath   = TEXT("/Game/Trace/Art/Pack/Knife/Anims/A_Knife_Idle_Open.A_Knife_Idle_Open");
	static const TCHAR* DrawPath   = TEXT("/Game/Trace/Art/Pack/Knife/Anims/A_Knife_Draw.A_Knife_Draw");
	static const TCHAR* StabPath   = TEXT("/Game/Trace/Art/Pack/Knife/Anims/A_Knife_Stab.A_Knife_Stab");
	static const TCHAR* InspectPath= TEXT("/Game/Trace/Art/Pack/Knife/Anims/A_Knife_Inspect.A_Knife_Inspect");

	/**
	 * /Game/Trace/Art/Pack/Materials/MI_Pack_<slot>.MI_Pack_<slot>, assembled per slot.
	 *
	 * Built by concatenation and NOT by FString::Printf: Printf's format argument has to be a literal
	 * (it is a template with a static_assert on the argument being an array of TCHAR), so a `const
	 * TCHAR* Format` variable does not compile. Two halves and a `+` is the idiom that does.
	 */
	static const TCHAR* MaterialPathPrefix = TEXT("/Game/Trace/Art/Pack/Materials/MI_Pack_");
	static const TCHAR* MaterialObjectInfix = TEXT(".MI_Pack_");

	/** The scalar every MI_Pack_* and every shipped MI_Railgun_* exposes. One name, one curve. */
	static const FName EmissiveIntensityParam(TEXT("EmissiveIntensity"));

	// =============================================================================================
	// THE POSE. Every number here is either MEASURED off the imported asset or is a style choice
	// stored RELATIVE to a measured base — the standing project rule.
	// =============================================================================================

	/**
	 * *** THE AXIS CORRECTION, AND IT IS A MEASUREMENT, NOT A TASTE. ***
	 *
	 * Interchange maps glTF -> UE as (gl.x, gl.z, gl.y) x 100, which the import pass confirmed
	 * against all five bounding boxes. Under that map the pack's blade points along UE **-Y**:
	 * measured mesh-local landmarks are tip (0, -12.6, 0), pivot (0,0,0), pommel (0, +12.2, 0).
	 *
	 * The rest of this project aims along **+X** — Scripts/railgun_glb_to_obj.py, which built the
	 * SHIPPED static meshes, uses (-gl.z, gl.x, gl.y) instead, and ATraceCharacter's whole viewmodel
	 * rig is authored with +X out of the lens. The two conventions differ by exactly one yaw of 90
	 * degrees, so that is what this is: yaw +90 sends -Y to +X and the blade points down the aim ray.
	 *
	 * IT IS SEPARATE FROM HeldCant() BELOW ON PURPOSE. This 90 is a fact about the export and must not
	 * be retuned; the cant is a look and is meant to be. Folding them into one FRotator would make
	 * the fact editable by accident, which is how a measured number quietly becomes a guess.
	 */
	static constexpr float PackAimYawCorrectionDeg = 90.f;

	/**
	 * WHERE THE PIVOT HANGS, in ATraceCharacter's viewmodel rig space (+X out of the lens, +Y right,
	 * +Z up), and the DEPTH ARITHMETIC that has to keep holding.
	 *
	 * The pack knife's own extents, measured: 12.6 uu of blade forward of the pivot, 12.2 uu of
	 * handle behind it, 24.8 uu open overall. Putting the pivot at +6.0 puts the TIP at 18.6 uu and
	 * the POMMEL at -6.2 uu.
	 *
	 * The cube rig this replaces reached 23 uu at rest and ~39 uu fully extended, and the gun's
	 * muzzle — the part that actually had to clear the capsule — sits at 76 uu. So the pack blade is
	 * SHALLOWER than the rig it replaces even before the stab clip's thrust, and it can no more
	 * intersect the world than the cubes could. That is the check TraceKnifeLayout's header asks any
	 * replacement to redo, redone.
	 *
	 * The lateral +1.2 and the -4.0 drop put it in the same lower-right corner the gun and the cube
	 * knife both hang in, so the crosshair clearance the HUD is laid out against does not move.
	 */
	static const FVector RestLocation(6.0f, 1.2f, -4.0f);

	/**
	 * THE HELD CANT, and the one thing here that had to be reasoned about rather than measured.
	 *
	 * A first-person primitive is rendered with its DEPTH COMPRESSED — EFirstPersonPrimitiveType::
	 * FirstPerson halves it — so a blade lying along +X has its X extent squashed while its Z extent
	 * does not, and an angle authored in rig space reads STEEPER on screen than the number says.
	 * TraceKnifeLayout measured this the expensive way for the cube rig: 22 degrees of authored pitch
	 * photographed at roughly 60, i.e. about a third of the authored value survives to the screen.
	 *
	 * These are authored at a third of what they should look like, on that measurement:
	 *   pitch +7   reads as ~20 on screen — the tip leads, slightly up, the way a held blade does.
	 *   yaw   -10  the point crosses inboard toward the crosshair. Yaw is NOT compressed (it is a
	 *              rotation in the screen plane at this pitch), so this one is close to literal.
	 *   roll  +4   turns the lit edge — the cyan channel, the only part that glows — toward the
	 *              camera instead of hiding it under the blade.
	 *
	 * Deliberately the SAME three numbers TraceKnifeLayout::OffHandRotation settled on, because the
	 * measurement behind them is about the RENDERER, not about which mesh is being held.
	 */
	static TAutoConsoleVariable<float> CVarHoldPitch(
		TEXT("Trace.Knife.HoldPitch"),
		7.f,
		TEXT("Spec v33. The NO-HANDS fallback blade's PITCH in rig space, degrees, tip up. Authored ")
		TEXT("at a third of what it should look like because a first-person primitive is drawn with ")
		TEXT("its depth compressed. Reaches only the ViewModelRoot path: with pack hands up the hold ")
		TEXT("comes from Trace.Knife.HoldSwing/HoldFlat instead."),
		ECVF_Cheat);

	static TAutoConsoleVariable<float> CVarHoldYaw(
		TEXT("Trace.Knife.HoldYaw"),
		-10.f,
		TEXT("Spec v33. The NO-HANDS fallback blade's YAW in rig space, degrees, negative = the ")
		TEXT("point crosses inboard toward the crosshair. Not depth-compressed, so close to literal."),
		ECVF_Cheat);

	static TAutoConsoleVariable<float> CVarHoldRoll(
		TEXT("Trace.Knife.HoldRoll"),
		4.f,
		TEXT("Spec v33. The NO-HANDS fallback blade's ROLL in rig space, degrees. Turns the lit edge ")
		TEXT("— the cyan channel, the only part of the blade that glows — toward the camera. On the ")
		TEXT("wrist the equivalent knob is Trace.Knife.HoldFlat."),
		ECVF_Cheat);

	/**
	 * *** THE HELD ATTITUDE IN RIG SPACE — THE NO-HANDS FALLBACK'S POSE, AND ONLY ITS POSE. ***
	 *
	 * It was briefly the shipped pose for BOTH attachment paths, and that was the over-correction
	 * this file's ComputeWristHold header describes: a rig-space direction that points out of the
	 * lens and a hand whose fingers point out of the lens are the same direction, so carrying this
	 * cant onto the wrist bone laid the balisong along the fingers all over again. The wrist path now
	 * builds its attitude out of the hand's own measured anatomy and never reads this.
	 *
	 * IT IS STILL EXACTLY RIGHT WHERE IT IS STILL USED. With no pack hands — a fresh clone with no
	 * `git lfs pull`, -TraceNoCharacterArt, or Trace.Knife.PackAttach 0 — the blade hangs off
	 * ViewModelRoot and there is no hand to take a hold from. A rig-space cant is then not a second
	 * opinion about the pose; it is the only opinion available.
	 *
	 * The three numbers are unchanged and their reasoning is unchanged:
	 *   pitch +7   reads as ~20 on screen — the tip leads, slightly up, the way a held blade does.
	 *              A first-person primitive is rendered with its DEPTH COMPRESSED
	 *              (EFirstPersonPrimitiveType::FirstPerson halves it), so an angle authored in rig
	 *              space reads STEEPER than the number says; TraceKnifeLayout measured 22 degrees of
	 *              authored pitch photographing at roughly 60.
	 *   yaw   -10  the point crosses inboard toward the crosshair. Yaw is NOT compressed (it is a
	 *              rotation in the screen plane at this pitch), so this one is close to literal.
	 *   roll  +4   turns the lit edge toward the camera instead of hiding it under the blade.
	 *
	 * They are the SAME three numbers TraceKnifeLayout::OffHandRotation settled on, because the
	 * measurement behind them is about the RENDERER and not about which mesh is being held. They are
	 * CVars now for one reason: a pose that can only be judged in a photograph has to be brackettable
	 * without a rebuild, which is the idiom Trace.Fx.BeamScale and Trace.Hands.GloveFloor already set
	 * in this module.
	 */
	static FRotator HeldCant()
	{
		return FRotator(CVarHoldPitch.GetValueOnGameThread(), CVarHoldYaw.GetValueOnGameThread(),
			CVarHoldRoll.GetValueOnGameThread());
	}

	/**
	 * *** THE OPEN HANDLE, MEASURED, AND THE FRACTION OF IT THE FIST CLOSES ON. ***
	 *
	 * 12.2 uu is the import pass's own landmark for the handle's LENGTH: pivot (0,0,0) to pommel,
	 * 12.2 uu apart. It cannot be re-measured from the mesh's bounds at runtime and that is not a
	 * slip — the reference pose is the CLOSED balisong, where the handles are folded forward over
	 * the blade and the whole asset lies on one side of the pivot (measured: the long axis runs
	 * -15.00 to +1.20 uu). Bounds can only ever describe the pose the mesh was exported in.
	 *
	 * *** THE DIRECTION IN THAT LANDMARK IS NOT USABLE AND v34 STOPPED USING IT. *** The import note
	 * writes the pommel at (0, +12.2, 0), i.e. on the far side of the pivot from the blade. That is
	 * not where this asset's handle is on any frame that gets drawn: Trace.Knife.HoldProbe, tracking
	 * the point 6.1 uu down `handle_safe` through the live pose, measures it at mesh
	 * (-0.57, -5.89, 0.05) during Idle_Open — the SAME side as the blade, because the open balisong
	 * folds its handles back along the spine. Placing a constant (0, +6.1, 0) at the fist therefore
	 * put the hand on a point 12 uu from any part of the knife, which is the "knife beside the fist,
	 * not in it" defect. ComputeWristHold now authors the grip point at (0, -6.1, 0) in the SHUT
	 * pose, where the mesh can actually be measured, and lets the handle's own bone carry it.
	 *
	 * THE FRACTION IS THE STYLE NUMBER AND THE LENGTH IS THE BASE IT MODIFIES, which is this
	 * project's standing rule for exactly this shape of pair. 0.5 = the fist closes on the MIDDLE of
	 * the handle, so half the handle is forward of the grip and half is behind it, and the pivot —
	 * the guard, the widest part of a balisong — ends up clear of the fingertips rather than inside
	 * them. It is what the hold is graded on and it is bracketable live.
	 */
	static constexpr float OpenHandleLengthUU = 12.2f;

	static TAutoConsoleVariable<float> CVarHoldGrip(
		TEXT("Trace.Knife.HoldGrip"),
		0.5f,
		TEXT("Spec v33. Where down the 12.2 uu open handle the fist closes, as a fraction: 0 = on the ")
		TEXT("pivot itself (which is what shipped, and it buried the whole handle in the hand), 0.5 = ")
		TEXT("the middle of the handle, 1 = on the pommel. Multiplied by the MEASURED handle length, ")
		TEXT("never typed as an absolute."),
		ECVF_Cheat);

	/**
	 * *** SCALE IS 1.0 AND THAT IS A DECISION, WITH THE BASE IT MODIFIES WRITTEN NEXT TO IT. ***
	 *
	 * Standing rule: a value that modifies a base is stored relative to the base. The BASE is the
	 * measured imported size — 2.00 x 16.20 x 2.80 uu closed, 24.8 uu open — and this is the factor
	 * on it, not a new absolute.
	 *
	 * 1.0 because the knife is the one pack asset already authored life-size for a first-person view:
	 * 24.8 uu open against the cube rig's ~27 uu of grip-to-tip is within 10%, so it drops into the
	 * same corner of the frame at its authored size. The pack's own preview scales the PISTOL to 0.28
	 * and the SMG to 0.39 for exactly this reason and leaves the knife and the hands at 1.0.
	 */
	static constexpr float PackScale = 1.0f;

	// =============================================================================================
	// THE EMISSIVE. Numbers are unreal-knife_knife_stats.json's, unchanged.
	// =============================================================================================
	//
	//   closed idle   cyan 0.8-1.1   amber 0.7-1.0     (only ever seen inside Draw)
	//   open idle     cyan 1.5-1.9   amber 1.6-2.1
	//   flip peak     cyan 3.6       amber 2.8         ("cyan spikes to 3.6x on each catch beat")
	//   stab peak     cyan 4.4       amber 3.0
	//
	// 1.0 is REST on every MI_Pack_* instance — the import pass folded the KHR emissive strengths
	// (cyan 1.5, amber 1.4) into EmissiveColor precisely so that the scalar could mean "multiplier on
	// rest" here. So these numbers go into the parameter literally.
	static constexpr float CyanClosedIdleLow  = 0.8f;
	static constexpr float CyanClosedIdleHigh = 1.1f;
	static constexpr float CyanOpenIdleLow    = 1.5f;
	static constexpr float CyanOpenIdleHigh   = 1.9f;
	static constexpr float CyanFlipPeak       = 3.6f;
	static constexpr float CyanStabPeak       = 4.4f;

	static constexpr float AmberClosedIdleLow  = 0.7f;
	static constexpr float AmberClosedIdleHigh = 1.0f;
	static constexpr float AmberOpenIdleLow    = 1.6f;
	static constexpr float AmberOpenIdleHigh   = 2.1f;
	static constexpr float AmberFlipPeak       = 2.8f;
	static constexpr float AmberStabPeak       = 3.0f;

	/**
	 * *** THE FOUR CATCH BEATS OF Inspect, FROM unreal-knife_knife_stats.json, IN SECONDS. ***
	 *
	 * The clip has six beats; four of them are FLIPS and it is the flips that carry the flare — the
	 * doc: "cyan spikes to 3.6x on each of the four catch beats". `hold` and `settle` are not flips
	 * and get the open idle.
	 *
	 *   openFlip     0.06 - 0.51
	 *   aerialClose  1.09 - 1.47     (the ONE moment the balisong folds shut, and it rises 50 mm)
	 *   reopen       1.66 - 2.05
	 *   doubleTwirl  2.24 - 2.88
	 *
	 * THE PEAK IS AT THE END OF THE BEAT, NOT THE MIDDLE, and that is the doc's own sentence: "the
	 * edge flares as the handles swing and settles as they catch". The catch IS the end. So the
	 * intensity ramps up across the beat and falls away after it, which puts the brightest frame on
	 * the frame the handles meet.
	 */
	struct FCatchBeat
	{
		float Start;
		float End;
	};

	static const FCatchBeat InspectCatchBeats[] =
	{
		{ 0.06f, 0.51f },
		{ 1.09f, 1.47f },
		{ 1.66f, 2.05f },
		{ 2.24f, 2.88f },
	};

	/** How long the flare takes to fall back to idle after a catch. Shorter than the shortest gap. */
	static constexpr float CatchFallSeconds = 0.12f;

	/**
	 * WHERE THE STAB PEAKS, as a FRACTION of the clip rather than as a time, so it tracks the
	 * sequence if the clip is ever re-exported at a different length. The stats file describes Stab
	 * as "130 mm thrust with a -0.22 rad pitch, snapping back": the thrust is the front of the clip
	 * and the snap-back is the rest, so the flare peaks a third of the way in and decays out.
	 *
	 * *** THE VALUE NOW LIVES IN THE HEADER, NOT HERE (v32 integration). *** TraceCharacter.cpp's
	 * gloved-hands driver has to peak on the same frame as this blade, and it was carrying its own
	 * copy of 0.35 because this one was file-local and unreachable. This alias keeps every reader in
	 * this file spelled the way it always was while there is exactly one definition of the number.
	 */
	static constexpr float StabPeakFraction = TraceKnifeView::StabPeakFraction;

	/** Where inside Draw the balisong actually snaps open. Fraction, same reasoning as above. */
	static constexpr float DrawSnapFraction = 0.72f;

	/**
	 * *** THE STAB'S FLARE SHAPE. ONE TRIANGLE, TWO CONSUMERS, NO SECOND COPY. ***
	 *
	 * SPEC v32 §4: "Drive its opacity off StabPeakFraction — THE SAME NUMBER the emissive peak
	 * already uses — so the flash and the streak cannot disagree." A comment saying "these use the
	 * same constant" is not that guarantee: two call sites can read one constant and still shape it
	 * differently, and then the blade's brightest frame and the streak's brightest frame are two
	 * frames apart for a reason nobody can find. So there is exactly ONE function, it is the one the
	 * emissive curve was already computing inline, and both readers call it.
	 *
	 * 0 at both ends of the clip, 1 at StabPeakFraction. Ramps up over the thrust and falls away
	 * across the snap-back, which is what "fading with the stab" means.
	 *
	 * @param Alpha01  where in A_Knife_Stab we are, as a fraction of the CLIP'S OWN LENGTH — so a
	 *                 re-export at a different length moves the peak with it.
	 */
	static float StabFlare(float Alpha01)
	{
		const float Alpha = FMath::Clamp(Alpha01, 0.f, 1.f);
		const float T = (Alpha <= StabPeakFraction)
			? (Alpha / FMath::Max(KINDA_SMALL_NUMBER, StabPeakFraction))
			: (1.f - (Alpha - StabPeakFraction) / FMath::Max(KINDA_SMALL_NUMBER, 1.f - StabPeakFraction));
		return FMath::Clamp(T, 0.f, 1.f);
	}

	// =============================================================================================
	// [SPEC v32 §4] THE STAB STREAK. *** THE FX DOC IS IN METRES; UNREAL IS IN CENTIMETRES. ***
	// =============================================================================================
	//
	// unreal-fx_README, Butterfly knife: "add a short streak plane (0.26 x 0.10 m) at the blade tip
	// for the thrust, ~0.9 opacity at peak."
	//
	//   0.26 m -> 26 uu     0.10 m -> 10 uu
	//
	// The conversion from those to a /Engine/BasicShapes/Plane scale is NOT done here: it is
	// UTraceFxShapes::SizePlane, which is the one place in this module allowed to divide by 100. A
	// plane is 100 x 100 uu in its own XY, so 26 uu is scale 0.26 — the LENGTH conversion, not the
	// radius one, and getting those two mixed up is the factor-of-two this module's single constant
	// exists to make unwritable.

	static constexpr float StreakWidthUU  = 26.f;
	static constexpr float StreakHeightUU = 10.f;

	/** "~0.9 opacity at peak". The peak is StabFlare == 1; everything below it is this, scaled. */
	static constexpr float StreakPeakOpacity = 0.9f;

	/**
	 * How much of the 26 uu streak LEADS the point, the rest trailing back down the blade.
	 *
	 * The doc says "at the blade tip" and says nothing about which way it hangs, so this is a choice
	 * and it is written down as one: a motion streak is the smear a fast object LEAVES BEHIND, so
	 * centring the plane on the tip — which would float 13 uu of it out past the point, in front of
	 * a knife that has not reached there yet — reads as a glowing spike rather than as speed. 4 uu
	 * ahead and 22 uu behind keeps a little glow on the point itself without inventing blade.
	 */
	static constexpr float StreakLeadUU = 4.f;

	/**
	 * #25E6FF — circuit_cyan, the FX doc's own hex, which is the channel the knife's edge and fuller
	 * are lit in. Written as LINEAR floats rather than as FLinearColor(FColor(0x25,0xE6,0xFF))
	 * because that constructor reads a global sRGB lookup table, and a file-scope static that depends
	 * on another translation unit's initialisation is a static-initialisation-order question nobody
	 * should have to think about for a colour. Converted once, by hand, with the standard sRGB
	 * transfer function: 37/255 -> 0.0185, 230/255 -> 0.7913, 255/255 -> 1.0.
	 */
	static const FLinearColor StreakCyanLinear(0.0185f, 0.7913f, 1.0f, 1.0f);

	/**
	 * *** THE RED ARM FOR §4, AND IT IS OBSERVABLE IN BOTH DIRECTIONS. ***
	 *
	 * 1 (default) — the streak plane is built and driven.
	 * 0           — it is never made visible. Trace.Knife.PackDemo's streak clause must FAIL in this
	 *               arm; a harness whose two arms agree is not measuring its rule, and this project
	 *               has shipped three that did.
	 *
	 * Read every frame rather than latched, so one session can show both arms — the same argument
	 * Trace.Knife.PackArt's own comment makes at length about a switch that cannot undo itself.
	 */
	static TAutoConsoleVariable<int32> CVarStabStreak(
		TEXT("Trace.Knife.StabStreak"),
		1,
		TEXT("Spec v32 s4. 1 = draw the 26x10 uu stab streak plane at the blade tip during ")
		TEXT("A_Knife_Stab, at up to 0.9 opacity, driven off the SAME StabPeakFraction the 4.4x cyan ")
		TEXT("flash uses. 0 = never show it, which is the red arm for Trace.Knife.PackDemo and for ")
		TEXT("Trace.Knife.StreakProbe."),
		ECVF_Cheat);

	// =============================================================================================
	// SWITCHES
	// =============================================================================================

	/**
	 * THE A/B SWITCH FOR THE WHOLE FEATURE, and the RED ARM for Trace.Knife.PackStatus.
	 *
	 * 1 (default) — the pack blade is built, the cube blade is hidden.
	 * 0           — nothing is built and nothing is hidden, so the v27 cube knife is exactly what is
	 *               on screen. This is the switch to flip if the pack art plays badly, and it is also
	 *               how the fallback path gets tested on a machine where the art DID resolve, which
	 *               is the same service -TraceNoCharacterArt does for the Mannequin.
	 */
	static TAutoConsoleVariable<int32> CVarPackKnife(
		TEXT("Trace.Knife.PackArt"),
		1,
		TEXT("Spec v31 s5. 1 = draw the pack's SK_TraceKnife with its four authored clips and hide ")
		TEXT("the procedural cube blade. 0 = the v27 cube blade, which is also what a checkout with ")
		TEXT("no LFS pull gets. Trace.Knife.PackStatus reports which one is live."),
		ECVF_Cheat);

	/**
	 * *** WHERE THE BLADE HANGS: THE HAND'S WRIST, OR THE RIG ROOT. ***
	 *
	 * 1 (default) — if a skeletal mesh with a `wrist_right` bone has appeared on this pawn (spec v31
	 *               §6's gloved hands), the blade parents to that BONE, which is what the pack's own
	 *               notes ask for in as many words: "Attach the weapon to a socket on wrist_right —
	 *               every weapon is a right-handed one-hand hold." The knife then inherits whatever
	 *               the hand clip is doing, which is the whole reason Stab_Knife and Stab are
	 *               authored frame-for-frame.
	 * 0           — always hang off ViewModelRoot at this file's own measured rest pose, ignoring the
	 *               hands entirely. This is the escape hatch if §6's rig is not where it should be:
	 *               the blade is then independent of it and can be judged on its own.
	 *
	 * EITHER WAY IT DEGRADES RATHER THAN FAILS. With no hands rig present at all — a checkout that
	 * has not imported the pack's hands, or -TraceNoCharacterArt — there is no wrist_right to find
	 * and the rig-root pose is used. That is a fallback, not an error, and the log says which of the
	 * two happened.
	 */
	static TAutoConsoleVariable<int32> CVarPackKnifeAttach(
		TEXT("Trace.Knife.PackAttach"),
		1,
		TEXT("Spec v31 s5/s6. 1 = parent the pack blade to the gloved hands' wrist_right bone when ")
		TEXT("that rig exists (the pack's stated attachment). 0 = always hang it off ViewModelRoot at ")
		TEXT("this file's own rest pose. Trace.Knife.PackStatus reports which was used."),
		ECVF_Cheat);

	/** The bone the pack's hands doc names for a right-handed one-hand hold. */
	static const FName WristRightBone(TEXT("wrist_right"));

	// =============================================================================================
	// *** THE THREE BONES THE HOLD IS BUILT OUT OF, AND WHY IT IS THESE THREE. ***  (spec v33)
	// =============================================================================================
	//
	// A hand does not hold a knife along an axis somebody typed. It holds it in the CHANNEL the
	// curled fingers make with the palm, and that channel has a measurable direction on this rig:
	//
	//   index_right_0 -> pinky_right_0   the knuckle line. Trace.Knife.HoldProbe measured the two at
	//                                    (-2.70, -6.60, 0.00) and (+2.70, -6.60, 0.00) in wrist
	//                                    space, i.e. exactly bone +X, 5.40 uu apart. That is the
	//                                    tube a fist closes around and the axis a handle must lie on.
	//   wrist_right -> knuckle_bar_right the length of the hand: (0.00, -6.20, 0.00), i.e. bone -Y.
	//                                    Perpendicular to the knuckle line, and the second axis of
	//                                    the palm's plane.
	//
	// The third axis is their cross product and is NOT typed either: on this rig index x forward
	// comes out palm-ward, which is checked at runtime against a fingertip (a fingertip is on the
	// palm side of its own knuckle by construction) and warned about rather than assumed.
	//
	// THEY ARE READ LIVE AND POSED AND THEN CAPTURED ONCE, for the same reason the wrist's base
	// attitude is: Inspect_Knife's four catch beats RELEASE THE FINGERS, so a basis re-measured per
	// frame would swim during the flourish and drag the knife through the hand it is supposed to be
	// rigid with.
	static const FName IndexKnuckleRightBone(TEXT("index_right_0"));
	static const FName PinkyKnuckleRightBone(TEXT("pinky_right_0"));
	static const FName KnuckleBarRightBone(TEXT("knuckle_bar_right"));
	static const FName IndexTipRightBone(TEXT("index_right_2"));

	// =============================================================================================
	// *** THE HANDLE THE FIST ACTUALLY CLOSES ON — AND WHY IT HAS TO BE A BONE. ***  (spec v34)
	// =============================================================================================
	//
	// A balisong is held by ONE HANDLE, the safe one, and everything else — the blade, the pivot, the
	// bite handle — rotates around the pin at its end. The pack's 24 bones name that handle: the
	// `_safe` chain (handle_pivot_safe -> handle_safe -> handle_cap_safe/boss_safe) against the
	// `_bite` chain that carries the edge.
	//
	// THE BLADE COMPONENT PLAYS ITS OWN CLIPS, so where that handle IS is a per-frame question and
	// not a constant. EnsureBladeBuilt's own comment says it: "an unanimated blade is a SHUT blade,
	// and the knife is carried open" — the reference pose is the folded knife and Idle_Open is what
	// swings the handles out of it. Reading the handle's LIVE bone transform is therefore the only
	// way to put the fist on it; a mesh-space constant can only ever be right in one pose, and the
	// pose it was right in (open idle) is not the pose Inspect_Knife's catch beats are in.
	static const FName HandleSafeBone(TEXT("handle_safe"));
	static const FName HandlePivotSafeBone(TEXT("handle_pivot_safe"));

	/**
	 * A bone's REFERENCE-pose transform in component space, walked up the parent chain.
	 *
	 * FReferenceSkeleton stores each bone relative to its parent and there is no cached component
	 * array on the asset that is safe to reach for here, so the chain is walked. It runs once per
	 * rig, at capture time, and never again.
	 */
	static FTransform RefBoneComponentTransform(const FReferenceSkeleton& RefSkeleton, int32 BoneIndex)
	{
		FTransform Accumulated = FTransform::Identity;
		const TArray<FTransform>& BonePose = RefSkeleton.GetRefBonePose();
		while (BonePose.IsValidIndex(BoneIndex))
		{
			Accumulated = Accumulated * BonePose[BoneIndex];
			BoneIndex = RefSkeleton.GetParentIndex(BoneIndex);
		}
		return Accumulated;
	}

	/**
	 * *** THE SWING: WHERE THE HANDLE LIES BETWEEN THE GRIP CHANNEL AND THE FINGERS. ***
	 *
	 * ZERO IS THE HAMMER GRIP AND IT IS THE ANATOMICAL ANSWER: the handle lies straight down the
	 * channel, out of the thumb side of the fist, so the fingers close ACROSS it and the pivot — the
	 * guard, the widest part of a balisong — stands clear of the index knuckle. NINETY is the pose
	 * that shipped and that the user photographed: the handle laid along the FINGERS, at ninety
	 * degrees to the tube they close around, which is why the plates read as standing on the back of
	 * the glove instead of inside it.
	 *
	 * It is one angle rather than three because the sweep between those two extremes is a rotation in
	 * the PALM'S OWN PLANE — the plane the two measured axes span — and every value on it is a grip a
	 * hand can actually make. A knife held at 40 degrees is the sabre grip, handle diagonal from the
	 * heel of the palm to the web of the thumb. Anything off that plane is not a grip at all, so
	 * there is deliberately no knob for it.
	 *
	 * BRACKETED IN PHOTOGRAPHS, NOT REASONED ABOUT: the trade is that the smaller the angle the more
	 * honestly the fist closes on the handle, and the further LEFT the blade points, because this
	 * rig's grip channel runs across the screen (bone +X measures as rig (-0.12, +0.99, -0.02), i.e.
	 * very nearly straight right) while the fingers point out of the lens.
	 */
	static TAutoConsoleVariable<float> CVarHoldSwing(
		TEXT("Trace.Knife.HoldSwing"),
		0.f,
		TEXT("Spec v33. Degrees, 0..90, measured in the palm's own plane. 0 = the handle lies down ")
		TEXT("the grip channel the curled fingers make (the hammer grip, and the anatomical answer); ")
		TEXT("90 = the handle lies along the fingers, which is the pose the user photographed as ")
		TEXT("'clipping through the hand'. 40 is the sabre grip. Live."),
		ECVF_Cheat);

	/**
	 * THE ROLL ABOUT THE BLADE'S OWN LONG AXIS, MEASURED FROM FLAT-TO-THE-LENS.
	 *
	 * Zero does NOT mean "no rotation": the reference the roll is measured from is the attitude whose
	 * flat faces the camera, derived from the lens direction expressed in the wrist's base pose. That
	 * is what makes this knob independent of Trace.Knife.HoldSwing — the blade presents its profile
	 * at every swing angle rather than turning edge-on to the eye as the handle comes round.
	 *
	 * 180 flips which way the cutting edge points, and is the knob to reach for if the edge reads as
	 * being on top; the pack ships no socket and no convention for which face of the blade is sharp
	 * (§7f records that as the re-export it wants), so the sign is a photograph, not a fact.
	 */
	static TAutoConsoleVariable<float> CVarHoldFlat(
		TEXT("Trace.Knife.HoldFlat"),
		0.f,
		TEXT("Spec v33. Degrees of roll about the blade's own long axis, measured from the attitude ")
		TEXT("that presents the blade's flat to the lens. 180 flips the cutting edge. Live."),
		ECVF_Cheat);

	/**
	 * HOW FAR INTO THE PALM THE HANDLE SITS, in uu along the measured palm normal.
	 *
	 * The fist centroid ATraceCharacter publishes is the centre of the CLOSED HAND, which is not
	 * quite the centre of the channel: Trace.Knife.HoldProbe measures the four fingers' curl centres
	 * at bone Z -3.42 / -2.34 / -2.00 / -1.63 while the centroid sits at -1.87, so the centroid is
	 * about half a uu shy of the tube on the knuckle side. Positive here moves the handle palm-ward,
	 * i.e. DEEPER into the curl, which is also the direction that makes the fingers draw in front of
	 * it instead of behind it.
	 */
	static TAutoConsoleVariable<float> CVarHoldPalm(
		TEXT("Trace.Knife.HoldPalm"),
		0.6f,
		TEXT("Spec v33. uu along the measured palm normal, positive = deeper into the finger curl. ")
		TEXT("Offsets the handle off the published fist centroid, which is the centre of the closed ")
		TEXT("hand rather than the centre of the grip tube. Live."),
		ECVF_Cheat);

	/**
	 * *** NO LONGER THE SHIPPED POSE. THIS IS THE FALLBACK FOR WHEN THE HANDS RIG WILL NOT ANSWER,
	 *     AND IT IS KEPT ONLY BECAUSE IT IS REACHABLE.  (spec v32 §8) ***
	 *
	 * It used to be the pose, under the comment "the hand is 19 cm from wrist to fingertip, so ~7 uu
	 * down the fingers is the middle of the grip". The MAGNITUDE was fine — the measured wrist-to-fist
	 * distance on this skeleton is 6.81 uu — but the DIRECTION was reasoned about instead of measured,
	 * and bone-local +X is not the length of the hand here. The pack's own hands README gives the game
	 * away in one line: the finger CURL axis is "local X (negative = toward palm)". Seven uu along a
	 * curl axis puts the pivot out through the SIDE of the wrist, and that is what shipped:
	 * Saved/Screenshots/v31integ_47_key3_knife_idle.png is a balisong lying beside the forearm with
	 * lit floor showing between it and the fingers, against v31fallback_47_key3_knife_idle.png — the
	 * same beat, same walk, cube rig — where the blade comes out of the closed fist.
	 *
	 * EnsureBladeBuilt now asks ATraceCharacter::GetViewModelGripWristLocal() instead, which derives
	 * the same delta from the fist centroid the guns are already placed against, so there is exactly
	 * one opinion in the build about where a hand is. THE STANDING RULE HELD ALL ALONG — "a delta onto
	 * the bone, not a second set of absolute coordinates" — it was the delta itself that was a guess.
	 */
	static const FVector WristOffset(7.0f, 0.f, 0.f);

	/** Per-clip-change logging. Off by default: a clip change is a per-press event, not per frame. */
	static TAutoConsoleVariable<int32> CVarPackKnifeLog(
		TEXT("Trace.Knife.PackLog"),
		0,
		TEXT("Spec v31 s5. 1 = print one line every time the blade changes clip, naming the clip and ")
		TEXT("the state that chose it. This is how a 0.30 s stab is verified without a camera."),
		ECVF_Cheat);

	/** Ping-pong 0..1..0 over [0,1], for the breathing idles. Cheap, and phase-continuous. */
	static float Breathe(float Alpha01)
	{
		return 0.5f - 0.5f * FMath::Cos(2.f * PI * FMath::Frac(FMath::Max(0.f, Alpha01)));
	}

	/** The pack art is refused wholesale under -TraceNoCharacterArt, exactly like the Mannequin. */
	static bool ArtDisabledOnCommandLine()
	{
		static const bool bDisabled = FParse::Param(FCommandLine::Get(), TEXT("TraceNoCharacterArt"));
		return bDisabled;
	}

	/**
	 * The reference pose in COMPONENT space, one transform per bone.
	 *
	 * FReferenceSkeleton hands out the ref pose in PARENT space, which is not the space the mesh's
	 * bounds are in — and the bounds are where the tip comes from. Composing down the hierarchy is
	 * what puts the two in the same space so "which bone is nearest the tip" is a question about
	 * centimetres rather than about two different coordinate systems. Parents always precede their
	 * children in a UE reference skeleton, so one forward pass is enough; the `Parent < Index` guard
	 * is belt-and-braces against a malformed import rather than an expected case.
	 */
	static bool BuildRefPoseComponentSpace(const USkeletalMesh* Source, TArray<FTransform>& OutComponentSpace)
	{
		OutComponentSpace.Reset();
		if (Source == nullptr)
		{
			return false;
		}

		const FReferenceSkeleton& RefSkeleton = Source->GetRefSkeleton();
		const TArray<FTransform>& LocalPose = RefSkeleton.GetRefBonePose();
		const int32 BoneCount = LocalPose.Num();
		if (BoneCount <= 0)
		{
			return false;
		}

		OutComponentSpace.SetNum(BoneCount);
		for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
			OutComponentSpace[BoneIndex] = (ParentIndex >= 0 && ParentIndex < BoneIndex)
				? (LocalPose[BoneIndex] * OutComponentSpace[ParentIndex])
				: LocalPose[BoneIndex];
		}
		return true;
	}

	/**
	 * *** PROBES ONLY. Where a point on the viewmodel is actually DRAWN. ***
	 *
	 * The blade and its streak are tagged EFirstPersonPrimitiveType::FirstPerson, so the renderer
	 * does NOT draw them at their own world transform — it re-projects them through the camera's
	 * FirstPersonFieldOfView. A verifier that projects the raw world position to the screen is
	 * therefore measuring a pixel the player never sees, which would make "is the streak on screen"
	 * a question about the wrong point by roughly 30%. This is the same morph
	 * ATraceCharacter::GetViewModelMuzzleViewPoint and ATraceTracer both apply, and for the same
	 * reason; it is duplicated here rather than shared because both of those live in files this pass
	 * does not own, and a probe is the one place a third copy costs nothing.
	 *
	 * Returns @p RawWorld unchanged when there is nothing to morph WITH, exactly as they do.
	 */
	static FVector MorphToFirstPerson(const ATraceCharacter* Pawn, const FVector& RawWorld)
	{
		if (Pawn == nullptr)
		{
			return RawWorld;
		}
		UCameraComponent* PawnCamera = Pawn->FindComponentByClass<UCameraComponent>();
		if (PawnCamera == nullptr)
		{
			return RawWorld;
		}

		FMinimalViewInfo POV;
		PawnCamera->GetCameraView(0.f, POV);
		if (!POV.bUseFirstPersonParameters)
		{
			return RawWorld;
		}

		const FVector Morphed = POV.TransformWorldToFirstPerson(RawWorld, /*bIgnoreFirstPersonScale=*/true);
		return Morphed.ContainsNaN() ? RawWorld : Morphed;
	}
}

const TCHAR* LexTraceKnifeClip(ETraceKnifeClip Clip)
{
	switch (Clip)
	{
	case ETraceKnifeClip::Idle_Open: return TEXT("Idle_Open");
	case ETraceKnifeClip::Draw:      return TEXT("Draw");
	case ETraceKnifeClip::Stab:      return TEXT("Stab");
	case ETraceKnifeClip::Inspect:   return TEXT("Inspect");
	case ETraceKnifeClip::None:      break;
	}
	return TEXT("none");
}

// =================================================================================================
// namespace TraceKnifeView — the two lines the rest of the project calls
// =================================================================================================

bool TraceKnifeView::RequestInspect(ATraceCharacter* Pawn)
{
	UTraceKnifeViewSubsystem* Driver = UTraceKnifeViewSubsystem::Get(Pawn);
	return (Driver != nullptr) && Driver->RequestInspect(Pawn);
}

bool TraceKnifeView::IsInspecting(const ATraceCharacter* Pawn)
{
	const UTraceKnifeViewSubsystem* Driver = UTraceKnifeViewSubsystem::Get(Pawn);
	return (Driver != nullptr) && Driver->IsInspecting(Pawn);
}

int32 TraceKnifeView::VisibleBladeParts(const ATraceCharacter* Pawn)
{
	const UTraceKnifeViewSubsystem* Driver = UTraceKnifeViewSubsystem::Get(Pawn);
	return (Driver != nullptr) ? Driver->VisibleBladeParts(Pawn) : 0;
}

// =================================================================================================
// UTraceKnifeViewSubsystem
// =================================================================================================

bool UTraceKnifeViewSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Game, PIE and dedicated-server worlds only — the same test every other Trace world subsystem
	// makes. A dedicated server creates it and then never ticks (IsTickable refuses without a local
	// player), which costs one allocation and keeps the shape identical on every machine.
	const UWorld* OuterWorld = Cast<UWorld>(Outer);
	return OuterWorld != nullptr && OuterWorld->IsGameWorld();
}

void UTraceKnifeViewSubsystem::Deinitialize()
{
	// The components are owned by their pawns and die with them; clearing the arrays is what releases
	// this subsystem's references so a travel does not keep a dead world's blades alive.
	Rigs.Reset();
	OwnedBlades.Reset();
	OwnedStreaks.Reset();
	OwnedMids.Reset();
	Super::Deinitialize();
}

TStatId UTraceKnifeViewSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTraceKnifeViewSubsystem, STATGROUP_Tickables);
}

bool UTraceKnifeViewSubsystem::IsTickable() const
{
	// A RENDERING MACHINE, ASKED EVERY FRAME rather than cached. A client that becomes a host, a
	// listen server that travels, a pawn possessed several seconds after the world came up — all of
	// them are the same subsystem meeting a new precondition, and none of them can be relied on to
	// tell anybody about it.
	const UWorld* World = GetWorld();
	if (World == nullptr || !World->IsGameWorld() || World->GetNetMode() == NM_DedicatedServer)
	{
		return false;
	}

	// *** DELIBERATELY NOT GATED ON Trace.Knife.PackArt. *** The obvious version of this line returns
	// the CVar and it is wrong: flipping the switch to 0 mid-session would stop the tick, and a
	// stopped tick cannot UNDO anything — the pack blade would stay on screen and the cube blade
	// would stay hidden, so the "off" position of the A/B switch would look exactly like the "on"
	// position. A switch that cannot be observed to do anything is not a red arm. The off state is
	// handled inside Tick instead, where the restore can actually happen; the cost of ticking while
	// off is one CVar read and an early return.
	return true;
}

UTraceKnifeViewSubsystem* UTraceKnifeViewSubsystem::Get(const UObject* WorldContext)
{
	if (WorldContext == nullptr || GEngine == nullptr)
	{
		return nullptr;
	}
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	return World != nullptr ? World->GetSubsystem<UTraceKnifeViewSubsystem>() : nullptr;
}

void UTraceKnifeViewSubsystem::ResolveAssets()
{
	if (bAssetsResolved)
	{
		return;
	}
	bAssetsResolved = true;

	if (TraceKnifeViewFile::ArtDisabledOnCommandLine())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeView] -TraceNoCharacterArt: the pack blade is being refused ON PURPOSE. The "
			     "procedural cube knife is what you are looking at, which is the point of the switch."));
		return;
	}

	// LOAD_NoWarn | LOAD_Quiet because "the art has not been pulled" is the ORDINARY state of a fresh
	// clone, not an error. The engine's own missing-package warning reads as a fault; the one line
	// below says what actually happened and what to do about it.
	PackMesh    = LoadObject<USkeletalMesh>(nullptr, TraceKnifeViewFile::MeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	ClipIdle    = LoadObject<UAnimSequence>(nullptr, TraceKnifeViewFile::IdlePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	ClipDraw    = LoadObject<UAnimSequence>(nullptr, TraceKnifeViewFile::DrawPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	ClipStab    = LoadObject<UAnimSequence>(nullptr, TraceKnifeViewFile::StabPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	ClipInspect = LoadObject<UAnimSequence>(nullptr, TraceKnifeViewFile::InspectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);

	// *** THE IDLE IS PART OF THE MINIMUM SET AND THAT IS NOT AN ARBITRARY CHOICE. *** The mesh's
	// reference pose is the CLOSED balisong, so a blade with no clip playing draws folded — and the
	// pack is explicit that the knife is carried open. A build with the mesh but no Idle_Open would
	// put a permanently-shut knife on screen, which is worse than the cube it replaced. Refusing the
	// whole rig unless the mesh AND the idle both resolve is what makes "never leave it unanimated"
	// a property of the code rather than a rule somebody has to remember.
	if (PackMesh == nullptr || ClipIdle == nullptr)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeView] the pack blade did not resolve (mesh=%s idle=%s) — falling back to the "
			     "procedural cube knife, which is a supported configuration. Run "
			     "Scripts/import-pack.sh, or `git lfs pull`, to author it."),
			PackMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			ClipIdle != nullptr ? TEXT("ok") : TEXT("MISSING"));
		PackMesh = nullptr;
		return;
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[KnifeView] pack blade up: %s + clips idle %.4fs / draw %s / stab %s / inspect %s."),
		*GetNameSafe(PackMesh), ClipIdle->GetPlayLength(),
		ClipDraw != nullptr ? *FString::Printf(TEXT("%.4fs"), ClipDraw->GetPlayLength()) : TEXT("MISSING"),
		ClipStab != nullptr ? *FString::Printf(TEXT("%.4fs"), ClipStab->GetPlayLength()) : TEXT("MISSING"),
		ClipInspect != nullptr ? *FString::Printf(TEXT("%.4fs"), ClipInspect->GetPlayLength()) : TEXT("MISSING"));
}

UAnimSequence* UTraceKnifeViewSubsystem::SequenceFor(ETraceKnifeClip Clip) const
{
	switch (Clip)
	{
	case ETraceKnifeClip::Idle_Open: return ClipIdle;
	case ETraceKnifeClip::Draw:      return ClipDraw;
	case ETraceKnifeClip::Stab:      return ClipStab;
	case ETraceKnifeClip::Inspect:   return ClipInspect;
	case ETraceKnifeClip::None:      break;
	}
	return nullptr;
}

UTraceKnifeViewSubsystem::FKnifeRig* UTraceKnifeViewSubsystem::RecordFor(ATraceCharacter* Pawn)
{
	if (Pawn == nullptr)
	{
		return nullptr;
	}
	for (FKnifeRig& Rig : Rigs)
	{
		if (Rig.Pawn.Get() == Pawn)
		{
			return &Rig;
		}
	}
	FKnifeRig& Fresh = Rigs.AddDefaulted_GetRef();
	Fresh.Pawn = Pawn;
	return &Fresh;
}

const UTraceKnifeViewSubsystem::FKnifeRig* UTraceKnifeViewSubsystem::FindRecord(const ATraceCharacter* Pawn) const
{
	for (const FKnifeRig& Rig : Rigs)
	{
		if (Rig.Pawn.Get() == Pawn)
		{
			return &Rig;
		}
	}
	return nullptr;
}

void UTraceKnifeViewSubsystem::ForgetDeadRecords()
{
	Rigs.RemoveAll([](const FKnifeRig& Rig) { return !Rig.Pawn.IsValid(); });

	OwnedBlades.RemoveAll([](const TObjectPtr<USkeletalMeshComponent>& Blade) { return !IsValid(Blade); });
	OwnedStreaks.RemoveAll([](const TObjectPtr<UStaticMeshComponent>& Streak) { return !IsValid(Streak); });
	OwnedMids.RemoveAll([](const TObjectPtr<UMaterialInstanceDynamic>& Mid) { return !IsValid(Mid); });
}

void UTraceKnifeViewSubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// THE RED ARM, AND IT UNDOES ITSELF. Flipping Trace.Knife.PackArt to 0 mid-session hides every
	// pack blade and gives the six-cube rig its bHiddenInGame back, so the switch is observable in
	// both directions from one session. Flipping it back to 1 restores the pack blade on the next
	// tick, because everything below is re-asserted rather than latched.
	if (TraceKnifeViewFile::CVarPackKnife.GetValueOnGameThread() == 0)
	{
		for (FKnifeRig& Rig : Rigs)
		{
			if (IsValid(Rig.Blade))
			{
				Rig.Blade->SetVisibility(false);
			}
			if (IsValid(Rig.StabStreak))
			{
				// The streak belongs to the pack blade; putting the cube knife back has to take it
				// with it, or the off position of the A/B switch leaves a plane hanging in the air.
				Rig.StabStreak->SetVisibility(false);
			}
			if (USceneComponent* Cube = Rig.HiddenCubeRoot.Get())
			{
				Cube->SetHiddenInGame(/*bNewHidden=*/false, /*bPropagateToChildren=*/true);
				Rig.HiddenCubeRoot = nullptr;
			}
		}
		return;
	}

	ResolveAssets();
	if (PackMesh == nullptr)
	{
		// The fallback path, and it is DELIBERATELY inert rather than merely silent: nothing was
		// built, so nothing is hidden, so the cube knife is untouched and a fresh clone is playable.
		return;
	}

	ForgetDeadRecords();

	for (TActorIterator<ATraceCharacter> It(World); It; ++It)
	{
		ATraceCharacter* Pawn = *It;

		// =========================================================================================
		// *** THE PAWN A HUMAN IS LOOKING OUT OF. NOT "LOCALLY CONTROLLED". MEASURED BUG. ***
		// =========================================================================================
		//
		// The first version of this test was `Pawn->IsLocallyControlled()` and the first run caught
		// it: on the practice range with five bots, the log read "pack blade built" SIX times.
		// APawn::IsLocallyControlled() is TRUE FOR EVERY AI-CONTROLLED PAWN ON THE SERVER — an
		// AIController is a local controller — so a listen-server host was building, animating and
		// pose-ticking a first-person knife for every bot in the match. Nobody could ever see one
		// (they are OnlyOwnerSee), so it was pure cost, and AlwaysTickPoseAndRefreshBones below means
		// it was cost that could not even be culled.
		//
		// This is the same trap TraceAudio::IsLocalPlayerActor documents at length for exactly the
		// same reason ("without this test a host would hear every bot's jump"), and the answer is the
		// same: test for a PLAYER controller specifically.
		//
		// RE-ASKED EVERY FRAME, never latched, because on a client the controller arrives by
		// replication SOME FRAMES AFTER the pawn does — a latch taken on the first tick would decide
		// "not mine" forever and the local player would carry an invisible knife for the whole match.
		// UTraceWeaponComponent's two-latch note is the same bug seen from the other side.
		if (!IsValid(Pawn) || Pawn->ViewModelRoot == nullptr)
		{
			continue;
		}
		const APlayerController* AsPlayer = Cast<APlayerController>(Pawn->GetController());
		if (AsPlayer == nullptr || !AsPlayer->IsLocalController())
		{
			continue;
		}

		if (FKnifeRig* Rig = RecordFor(Pawn))
		{
			TickRig(*Pawn, *Rig, DeltaSeconds);
		}
	}
}

// =================================================================================================
// [SPEC v33] *** THE HOLD. WHY THE BLADE WAS THREADED THROUGH THE HAND, AND WHAT REPLACES IT. ***
// =================================================================================================
//
// WHAT SHIPPED. On the wrist path the blade took FQuat::Identity for its rotation and the fist
// CENTROID for its location, under the argument that "the hand and the knife came out of the SAME
// pack through the SAME importer, so wrist_right is already oriented in the space the blade is
// authored in". That argument is about two ARTISTS' conventions agreeing; it says nothing about
// where a hand points, and Trace.Knife.HoldProbe measured what it actually produced:
//
//     the blade's long axis  = mesh -Y  = (identity) = bone -Y
//     bone -Y, on this rig   = wrist -> knuckles, i.e. STRAIGHT DOWN THE HAND
//     the fist's grip tube   = bone  X  = index (-2.70) to pinky (+2.70) across the knuckles
//
// So the balisong was laid along the hand at ninety degrees to the tube the fingers close around,
// with its PIVOT — the widest part of the knife — at the centre of the fist. Its 12.2 uu of open
// handle therefore ran back through the palm, through the wrist and 4.5 uu out the far side into
// the forearm, and its 12.6 uu of blade ran out through the finger geometry. That is the
// photographed defect exactly: "the gloved fingers are BURIED IN a large dark angular slab".
//
// THE FIRST REPAIR SLID THE KNIFE AND LEFT ITS DIRECTION ALONE, AND THAT WAS HALF AN ANSWER. It
// authored the attitude in RIG space (HeldCant x the +90 axis correction, the pose the ViewModelRoot
// fallback uses) and carried it into the bone, then slid the fist 0.5 x 12.2 uu down the handle. The
// slide was right and stays. The attitude was not: measured back on the live Idle_Knife pose, the
// blade came out along bone (-0.32, -0.91, +0.26), which is 83 degrees off the grip channel — still
// very nearly STRAIGHT DOWN THE FINGERS, because a rig-space direction that points out of the lens
// and a hand whose fingers point out of the lens are the same direction. The photograph agreed: the
// two handle plates stood proud ACROSS THE BACK OF THE GLOVE, drawn in front of the knuckle ring.
//
// *** WHAT REPLACES IT: THE HOLD IS BUILT IN THE HAND'S OWN ANATOMY AND NOT IN RIG SPACE AT ALL. ***
//
//   1. THE BASIS IS THREE MEASURED BONES, CAPTURED ONCE. index_right_0 -> pinky_right_0 is the
//      knuckle line, which is the tube a fist closes around; wrist_right -> knuckle_bar_right is the
//      length of the hand; their cross product is the palm normal, and its palm-ward sense is
//      CHECKED against a fingertip rather than assumed. See FKnifeRig::GripBasisBone.
//
//   2. THE HANDLE LIES IN THE PALM'S PLANE, AT Trace.Knife.HoldSwing. Zero is the hammer grip — the
//      handle straight down the channel and out of the thumb side — and 90 is the pose that shipped,
//      the handle along the fingers. Every value between is a grip a hand can make; nothing off that
//      plane is, which is why there is one angle here and not three.
//
//   3. THE ROLL IS MEASURED FROM FLAT-TO-THE-LENS. The one thing the hold needs from outside the
//      hand is where the eye is, so the blade presents its profile instead of its edge. The lens
//      direction is rig -X carried into the bone's frame by the captured base attitude — so it is
//      constant, and the blade turns WITH the hand instead of tracking the camera.
//
//   4. THE FIST CLOSES ON THE HANDLE AND NOT ON THE PIVOT, AND ON THE HANDLE THE CLIP IS DRAWING.
//      The rule is unchanged and it is still why the guard ends up clear of the fingertips: the
//      point Trace.Knife.HoldGrip x 12.2 uu down the SAFE HANDLE from the pivot is placed at the
//      fist, plus Trace.Knife.HoldPalm uu deeper into the curl. What changed in v34 is that the
//      point is carried by the safe handle's own bone instead of being a mesh-space constant, so it
//      stays on the handle while Draw folds the knife and Inspect swings it. See the GRIP section
//      of ComputeWristHold and FKnifeRig::GripHandleRefInv.
//
// A HAND ANIMATION NOW DECIDES WHERE THE KNIFE POINTS, WHICH IS THE POINT. Nothing here aims the
// blade at the crosshair; Idle_Knife, Draw_Knife's wrist flip and Inspect_Knife's four catch beats
// carry it, exactly as a hand carries a held thing. The old formulation could only be correct in the
// one pose its rig-space cant was authored against.
//
// FOUR CVar READS AND ONE BASIS ROTATION, EVERY FRAME, DELIBERATELY. It could be latched, but then
// the four Trace.Knife.Hold* knobs would need a rebuild to bracket and a hands rig that finished
// building after the blade did would never be picked up. The captured half — the expensive half, the
// one that must not be re-measured — is still captured exactly once.
bool UTraceKnifeViewSubsystem::ComputeWristHold(const ATraceCharacter& Pawn, FKnifeRig& Rig,
	FVector& OutBoneLocation, FQuat& OutBoneRotation) const
{
	// THE FIST FIRST, BECAUSE IT IS ALSO THE GATE. GetViewModelGripWristLocal returns false whenever
	// the pack rig is not up, and everything below would then be measuring a bone that does not
	// exist. Same contract, same reason, as the accessor's own header note.
	FVector FistBoneLocal = FVector::ZeroVector;
	if (!Pawn.GetViewModelGripWristLocal(FistBoneLocal))
	{
		return false;
	}

	USkeletalMeshComponent* Hands = Pawn.GetViewModelHandsMesh();
	if (Hands == nullptr)
	{
		return false;
	}

	if (!Rig.bBaseWristCaptured)
	{
		// --- THE BASE POSE, DERIVED FROM TWO LIVE READS IN ONE FRAME ------------------------------
		//
		// ATraceCharacter::GetViewModelWeaponDelta() IS `base^-1 x live`, so `live x delta^-1` is the
		// base — whatever pose the rig is in on the frame this runs. That is the point of doing it
		// this way round instead of sampling the wrist and assuming it is at rest: the previous pass
		// in this area shipped a rest transform captured from the wrong pose (the mesh REFERENCE pose
		// is Idle_Knife frame 0 while the weapons were placed for Idle_Pistol t=0) and paid a
		// constant 6.5 degree error on every part for it. There is no pose to be wrong about here.
		//
		// `live` is read the way ComputeHandsWristDelta reads it — socket in COMPONENT space carried
		// out through the mesh component's own relative transform — because that is where HandsScale,
		// HandsYaw and HandsLocation live, and re-deriving those three is the duplicate-constant
		// failure this codebase logs by name.
		const FTransform WristLiveRig =
			Hands->GetSocketTransform(TraceKnifeViewFile::WristRightBone, RTS_Component)
			* Hands->GetRelativeTransform();
		const FTransform WristBaseRig = WristLiveRig * Pawn.GetViewModelWeaponDelta().Inverse();

		Rig.BaseWristRotRigInv = WristBaseRig.GetRotation().Inverse();

		// --- THE GRIP BASIS, MEASURED OFF LIVE POSED BONES ----------------------------------------
		//
		// Read in the WRIST's frame, because that is the frame the blade's relative transform is
		// expressed in and therefore the only one in which the answer is directly usable. Everything
		// is a difference of two bone positions, so HandsScale and HandsYaw cancel and there is no
		// second opinion about them to get wrong.
		const FTransform WristWorld =
			Hands->GetSocketTransform(TraceKnifeViewFile::WristRightBone, RTS_World);
		auto InWrist = [&](const FName Bone)
		{
			return WristWorld.InverseTransformPosition(
				Hands->GetSocketTransform(Bone, RTS_World).GetLocation());
		};

		const FVector IndexKnuckle = InWrist(TraceKnifeViewFile::IndexKnuckleRightBone);
		const FVector PinkyKnuckle = InWrist(TraceKnifeViewFile::PinkyKnuckleRightBone);
		const FVector KnuckleBar = InWrist(TraceKnifeViewFile::KnuckleBarRightBone);
		const FVector IndexTip = InWrist(TraceKnifeViewFile::IndexTipRightBone);

		// A DEGENERATE HAND IS A REFUSAL, NOT A GUESS. If any of the four bones is missing,
		// GetSocketTransform returns the component's own transform and all four collapse onto one
		// point; building a basis out of that would produce an arbitrary attitude that looks like a
		// bug in the pose rather than a missing bone. Leaving bBaseWristCaptured false means the
		// caller keeps the fist-centroid fallback and tries again next frame, which is also what
		// happens while the rig is still building.
		const FVector Across = PinkyKnuckle - IndexKnuckle;   // the knuckle line, index -> pinky
		const FVector Along = KnuckleBar;                     // the wrist is the origin of this frame
		if (Across.SizeSquared() < 1.f || Along.SizeSquared() < 1.f)
		{
			return false;
		}

		// MakeFromXY takes the first axis exactly and orthogonalises the second against it, which is
		// what turns two measured directions that are not quite perpendicular into a basis.
		const FQuat Basis = FRotationMatrix::MakeFromXY(Across, Along).ToQuat();

		// THE PALM-WARD SENSE IS CHECKED, NOT ASSUMED. A fingertip is on the palm side of its own
		// knuckle by construction, so the sign of that displacement along the basis' Z is the test.
		// It is a warning and not a correction because flipping Z alone would make the basis
		// left-handed, and the honest repair to a re-exported rig is to swap the two knuckle bones.
		const FVector PalmDir = Basis.GetAxisZ();
		if (FVector::DotProduct(IndexTip - IndexKnuckle, PalmDir) <= 0.f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[PackKnife] the measured grip basis' Z is NOT palm-ward on this rig — the "
				     "index fingertip is on its other side. Trace.Knife.HoldPalm will push the "
				     "handle OUT of the fingers rather than into them; the fix is to swap "
				     "index_right_0/pinky_right_0, not to negate Z."));
		}

		Rig.GripBasisBone = Basis;
		Rig.bBaseWristCaptured = true;
	}

	// --- THE SAFE HANDLE, CAPTURED ONCE ---------------------------------------------------------
	//
	// See FKnifeRig::GripHandleRefInv. Separate from the block above because it is a fact about the
	// BLADE asset and not about the hand, and because a hands rig that is still building must not
	// cost the blade its handle. Both are one-shot and both are cheap to have missed a frame of.
	if (!Rig.bGripHandleCaptured && IsValid(Rig.Blade))
	{
		if (const USkeletalMesh* BladeMesh = Rig.Blade->GetSkeletalMeshAsset())
		{
			const FReferenceSkeleton& BladeRef = BladeMesh->GetRefSkeleton();
			int32 HandleIndex = BladeRef.FindBoneIndex(TraceKnifeViewFile::HandleSafeBone);
			FName HandleName = TraceKnifeViewFile::HandleSafeBone;
			if (HandleIndex == INDEX_NONE)
			{
				HandleIndex = BladeRef.FindBoneIndex(TraceKnifeViewFile::HandlePivotSafeBone);
				HandleName = TraceKnifeViewFile::HandlePivotSafeBone;
			}

			if (HandleIndex != INDEX_NONE)
			{
				Rig.GripHandleRefInv =
					TraceKnifeViewFile::RefBoneComponentTransform(BladeRef, HandleIndex).Inverse();
				Rig.GripHandleBone = HandleName;
			}
			else
			{
				// NOT A GUESS AND NOT A CRASH. A blade asset without a `_safe` chain is a different
				// knife; the constant-offset fallback below is the pose that shipped before this and
				// it is still a hold, just one that cannot follow a flip.
				UE_LOG(LogTraceGame, Warning,
					TEXT("[PackKnife] no 'handle_safe' or 'handle_pivot_safe' bone on '%s' — the fist "
					     "cannot track the handle through a flip and falls back to the constant "
					     "%.1f uu offset down the blade axis."),
					*GetNameSafe(BladeMesh), TraceKnifeViewFile::OpenHandleLengthUU);
			}

			Rig.bGripHandleCaptured = true;
		}
	}

	// --- THE ATTITUDE -----------------------------------------------------------------------------
	//
	// The handle sweeps in the palm's plane between the grip channel and the fingers. -X is the
	// THUMB side of the channel (X is measured index -> pinky), which is the side a right fist lets a
	// blade out of; +Y is the length of the hand.
	const float SwingRad = FMath::DegreesToRadians(
		FMath::Clamp(TraceKnifeViewFile::CVarHoldSwing.GetValueOnGameThread(), 0.f, 90.f));
	const FVector BladeAxisBone =
		(FMath::Cos(SwingRad) * -Rig.GripBasisBone.GetAxisX()
			+ FMath::Sin(SwingRad) * Rig.GripBasisBone.GetAxisY()).GetSafeNormal();

	// WHERE THE EYE IS, IN THE BONE'S FRAME. The lens looks down rig -X; the captured base attitude
	// carries that into the wrist's frame. Removing its component along the blade leaves the
	// direction the blade's FLAT should face, which is what stops the knife presenting its edge to
	// the camera as the swing brings the handle round.
	const FVector LensBone = Rig.BaseWristRotRigInv.RotateVector(FVector(-1.f, 0.f, 0.f));
	FVector FlatNormal =
		(LensBone - FVector::DotProduct(LensBone, BladeAxisBone) * BladeAxisBone).GetSafeNormal();
	if (FlatNormal.IsNearlyZero())
	{
		// The blade points straight at the lens: every roll presents the same silhouette, so any
		// perpendicular will do and the palm normal is the one that carries meaning.
		FlatNormal = Rig.GripBasisBone.GetAxisZ();
	}

	// The blade is the mesh's -Y, so the mesh's +Y is the direction the HANDLE runs; MakeFromYX takes
	// that exactly and puts the mesh's +X — the thin axis, i.e. the normal of the blade's flat — as
	// near FlatNormal as it can. The roll is then a rotation about the blade's own long axis, applied
	// in the bone's frame, so it is a left multiply and not a right one.
	const FQuat FlatToLens = FRotationMatrix::MakeFromYX(-BladeAxisBone, FlatNormal).ToQuat();
	const FQuat Roll(BladeAxisBone,
		FMath::DegreesToRadians(TraceKnifeViewFile::CVarHoldFlat.GetValueOnGameThread()));
	OutBoneRotation = Roll * FlatToLens;

	// --- THE GRIP ---------------------------------------------------------------------------------
	//
	// *** THE FIST IS PUT ON THE HANDLE THE CLIP IS ACTUALLY DRAWING, NOT ON A MESH-SPACE CONSTANT.
	//
	// The rule has not changed — the fist closes `Slide` uu down the handle from the pivot — but
	// WHERE that point is has to be asked of the pose, because the blade component runs its own
	// clips and the reference pose is the SHUT knife. The previous formulation placed the mesh-space
	// point (0, +Slide, 0) at the fist, on the strength of an import note putting the open pommel at
	// (0, +12.2, 0). MEASURED, this asset's handle is nowhere near there on any frame it draws: the
	// point 6.1 uu down `handle_safe` sits at mesh (-0.57, -5.89, 0.05) during Idle_Open, on the
	// blade's own side. The fist was being held 12 uu off the knife, which is the photographed
	// "knife beside the fist" and the band of background between the blade and the glove on
	// inspect-late — the pivot ended up 3.5 uu OUTSIDE the index knuckle with nothing between the
	// fingers at all.
	//
	// GripHandleRefInv turns the constant into a point ON THE SAFE HANDLE and lets the handle's own
	// bone carry it. Shut, the handles fold forward over the blade (mesh -Y) and the whole asset is
	// measurable, so the grip point is authored there — (0, -Slide, 0) — and the bone's live
	// skinning delta puts it wherever the running clip has swung the handle to. Through Draw's fold
	// and Inspect_Knife's catch beats the fist therefore stays ON the handle and the blade turns
	// about it, which is what a balisong does and what "held naturally" means for one.
	//
	// The ATTITUDE deliberately stays the component's. Compensating the handle bone's rotation as
	// well would freeze the flourish inside the fingers — the knife would stop flipping — and the
	// defect being fixed here is a POSITION: the knife beside the fist rather than in it.
	const float Slide = TraceKnifeViewFile::OpenHandleLengthUU
		* FMath::Clamp(TraceKnifeViewFile::CVarHoldGrip.GetValueOnGameThread(), 0.f, 1.f);

	// Mesh-space, this frame. The scale is the component's own (PackScale, 1.0) and is applied
	// rather than assumed, because a mesh-space offset has to be scaled the way the mesh is.
	FVector GripMeshLocal = FVector(0.f, Slide, 0.f);
	if (!Rig.GripHandleBone.IsNone() && IsValid(Rig.Blade)
		&& Rig.Blade->DoesSocketExist(Rig.GripHandleBone))
	{
		const FTransform HandleLive = Rig.Blade->GetSocketTransform(Rig.GripHandleBone, RTS_Component);
		GripMeshLocal =
			(Rig.GripHandleRefInv * HandleLive).TransformPosition(FVector(0.f, -Slide, 0.f));
	}

	const FVector BladeScale = IsValid(Rig.Blade) ? Rig.Blade->GetRelativeScale3D() : FVector::OneVector;
	const FVector GripInBone = OutBoneRotation.RotateVector(GripMeshLocal * BladeScale);

	OutBoneLocation = FistBoneLocal - GripInBone
		+ TraceKnifeViewFile::CVarHoldPalm.GetValueOnGameThread() * Rig.GripBasisBone.GetAxisZ();
	return true;
}

void UTraceKnifeViewSubsystem::ApplyWristHold(const ATraceCharacter& Pawn, FKnifeRig& Rig)
{
	if (!IsValid(Rig.Blade) || !Rig.bOnWrist)
	{
		return;
	}

	FVector BoneLocation = FVector::ZeroVector;
	FQuat BoneRotation = FQuat::Identity;
	if (ComputeWristHold(Pawn, Rig, BoneLocation, BoneRotation))
	{
		Rig.Blade->SetRelativeLocationAndRotation(BoneLocation, BoneRotation);
	}
}

void UTraceKnifeViewSubsystem::EnsureBladeBuilt(ATraceCharacter& Pawn, FKnifeRig& Rig)
{
	if (IsValid(Rig.Blade) || Rig.bArtUnavailable || PackMesh == nullptr || Pawn.ViewModelRoot == nullptr)
	{
		return;
	}

	USkeletalMeshComponent* Blade = NewObject<USkeletalMeshComponent>(
		&Pawn, MakeUniqueObjectName(&Pawn, USkeletalMeshComponent::StaticClass(), FName(TEXT("PackKnife"))));
	if (Blade == nullptr)
	{
		Rig.bArtUnavailable = true;
		return;
	}

	// --- WHERE IT HANGS ---------------------------------------------------------------------------
	//
	// THE HAND'S WRIST IF THERE IS ONE, otherwise the rig root. Found by asking every skeletal mesh
	// on this pawn whether its skeleton has a `wrist_right` bone, which is the pack's own bone name
	// and is a stronger test than a component name: the hands rig can be renamed, re-parented or
	// rebuilt by §6 and this still finds it, and it cannot false-positive on the blade itself (the
	// knife's 24 bones are handle_pivot_safe/bite/latch_pivot, none of them a wrist).
	USceneComponent* AttachParent = Pawn.ViewModelRoot;
	FName AttachSocket = NAME_None;

	if (TraceKnifeViewFile::CVarPackKnifeAttach.GetValueOnGameThread() != 0)
	{
		TArray<USkeletalMeshComponent*> Skeletals;
		Pawn.GetComponents<USkeletalMeshComponent>(Skeletals);
		for (USkeletalMeshComponent* Candidate : Skeletals)
		{
			if (Candidate == nullptr || Candidate == Blade)
			{
				continue;
			}
			if (Candidate->DoesSocketExist(TraceKnifeViewFile::WristRightBone))
			{
				AttachParent = Candidate;
				AttachSocket = TraceKnifeViewFile::WristRightBone;
				break;
			}
		}
	}

	Blade->SetMobility(EComponentMobility::Movable);
	Blade->SetupAttachment(AttachParent, AttachSocket);
	Blade->SetSkeletalMeshAsset(PackMesh);
	Rig.bOnWrist = !AttachSocket.IsNone();

	// =============================================================================================
	// THE POSE. TWO ATTACHMENT PATHS, AND THEY NO LONGER SHARE AN AUTHORED ATTITUDE.
	// =============================================================================================
	//
	// ON ViewModelRoot: correction FIRST, then the cant — quaternion multiplication, because FRotator
	// addition is not rotation composition and the two answers differ once either term is large.
	// 90 degrees is large. The +90 is needed because ViewModelRoot is THIS PROJECT'S rig space (+X out
	// of the lens), authored against Scripts/railgun_glb_to_obj.py's axis map, and the pack arrives
	// through Interchange's, which is a yaw of 90 away. This is the whole pose on that path: with no
	// hands there is no hold to derive.
	//
	// *** ON THE WRIST: NOTHING IS DECIDED HERE. ComputeWristHold OWNS IT. ***
	// It has to build its attitude out of the hand's own measured bones, and on the frame the blade
	// is built those bones may not have been posed yet — so the identity written below is a
	// PLACEHOLDER and not a pose. It stands for less than one frame: TickRig calls ApplyWristHold
	// immediately after EnsureBladeBuilt returns, on this same tick, before anything is drawn. It is
	// written at all only because a component must be given SOME relative transform before it is
	// registered.
	//
	// Two dead ends this line has already been down, so that neither is walked again: FQuat::Identity
	// as the SHIPPED pose (the hand and the knife share an importer, but what the shared convention
	// makes bone -Y is THE LENGTH OF THE HAND, so identity laid the balisong down the fingers), and
	// then HeldCant carried onto the bone (a rig-space aim out of the lens, on a hand whose fingers
	// point out of the lens, is the same direction again). See ComputeWristHold.
	const FQuat Corrected = Rig.bOnWrist
		? FQuat::Identity
		: FQuat(TraceKnifeViewFile::HeldCant())
			* FQuat(FRotator(0.f, TraceKnifeViewFile::PackAimYawCorrectionDeg, 0.f));

	// =============================================================================================
	// *** THE OFFSET IS ASKED FOR, NOT GUESSED, AND THAT IS THE v32 §8 FIX. ***
	// =============================================================================================
	//
	// WristOffset is (7, 0, 0) with the comment "the hand is 19 cm from wrist to fingertip, so ~7 uu
	// down the fingers is the middle of the grip". The 7 is very nearly right — the measured
	// wrist-to-fist distance on this skeleton is 6.81 uu — but the DIRECTION was reasoned about
	// rather than measured, and bone-local +X is not the length of the hand on this rig: it is the
	// KNUCKLE LINE, index (-2.70) to pinky (+2.70), which Trace.Knife.HoldProbe prints.
	//
	// ATraceCharacter OWNS THE ANSWER AND PUBLISHES IT. It already knows the closed fist's centroid in
	// rig space (HandsGripRig — the point both guns' grips are derived from) and the wrist's rig
	// transform in the same base pose, so the offset in the BONE's own frame is one inverse-transform
	// and no new measurement. Asking for it is also the standing rule here: this file must not carry a
	// second opinion about where a hand is.
	//
	// THE (7, 0, 0) LITERAL SURVIVES AS THE FALLBACK AND IS STILL REACHABLE — with a hands rig that
	// resolved a wrist_right but has not finished building, ComputeWristHold returns false and this is
	// what is used. It is a worse pose, not a broken one, and the log line below says which of the two
	// a given session got, so a frame can never be graded against the wrong one. Trace.Knife.PackStatus
	// prints it too, and Trace.Knife.HoldProbe measures whichever one is live.
	FVector WristPivot = TraceKnifeViewFile::WristOffset;
	const bool bMeasuredGrip = Rig.bOnWrist && Pawn.GetViewModelGripWristLocal(WristPivot);

	Blade->SetRelativeLocationAndRotation(
		Rig.bOnWrist ? WristPivot : TraceKnifeViewFile::RestLocation, Corrected);

	Blade->SetRelativeScale3D(FVector(TraceKnifeViewFile::PackScale));

	// CONTRACT §7: the capsule is the ONLY collider on this actor. Hitscan resolution, the trail trip
	// test and the lag-compensation history all reason purely about the capsule, and a colliding
	// blade would break all three — besides being a permanent obstacle welded to a player's face.
	Blade->SetCollisionProfileName(TEXT("NoCollision"));
	Blade->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Blade->SetGenerateOverlapEvents(false);
	Blade->SetCanEverAffectNavigation(false);
	Blade->bReceivesDecals = false;

	// NOBODY ELSE MAY EVER SEE THIS, and no shadow of any kind, so there is no path by which a
	// floating knife appears in another player's frame. Set BEFORE RegisterComponent so the scene
	// proxy is created with these rather than being rebuilt a frame later.
	Blade->SetOnlyOwnerSee(true);
	Blade->SetCastShadow(false);
	Blade->bCastHiddenShadow = false;
	Blade->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;

	// SINGLE-NODE ANIMATION, NO ANIMBLUEPRINT. Four clips and one at a time is exactly what single
	// node mode is for; an AnimBP would be an asset to author, an asset to load and a second place
	// for the state machine to live. Set before RegisterComponent so the first frame drawn is already
	// posed by Idle_Open rather than by the CLOSED reference pose.
	Blade->SetAnimationMode(EAnimationMode::AnimationSingleNode);

	// ALWAYS TICK THE POSE. A viewmodel is a metre from the camera but its BOUNDS are tiny, and a
	// skeletal mesh that fails its own visibility/relevance test stops updating its pose — which for
	// this asset means it reverts to reading as frozen mid-flip. OnlyTickPoseWhenRendered is the
	// default and is exactly wrong here.
	Blade->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	Blade->RegisterComponent();

	// --- Materials ------------------------------------------------------------------------------
	//
	// STAMPED BY SLOT NAME, and the slot names are the glTF material names the import pass kept:
	// shell, circuit_cyan, plating, core_amber, carbon. So MI_Pack_<slotname> is a lookup and not a
	// guess, and a slot the pack adds tomorrow either finds its instance or is left alone.
	//
	// glTF material import was switched OFF in the import pipeline, so the mesh arrives with no
	// materials of its own — this loop is what makes the blade something other than grey plastic, and
	// it is the single reason unreal-fx_README calls itself the important file.
	const TArray<FSkeletalMaterial>& Slots = PackMesh->GetMaterials();
	Rig.SlotNames.Reset(Slots.Num());
	Rig.SlotMids.Reset(Slots.Num());

	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
	{
		const FName SlotName = Slots[SlotIndex].MaterialSlotName;
		Rig.SlotNames.Add(SlotName);

		const FString Leaf = SlotName.ToString();
		const FString Path = FString(TraceKnifeViewFile::MaterialPathPrefix) + Leaf
			+ TraceKnifeViewFile::MaterialObjectInfix + Leaf;
		UMaterialInterface* Instance =
			LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);

		if (Instance != nullptr)
		{
			Blade->SetMaterial(SlotIndex, Instance);
		}

		// A DYNAMIC INSTANCE PER SLOT PER PAWN. The MI_Pack_* assets are shared, so writing
		// EmissiveIntensity on them directly would make one player's stab flare on everybody's
		// screen. CreateDynamicMaterialInstance returns null for a slot with no material at all,
		// which is a legal state and is stored as a null rather than skipped — SlotMids stays
		// index-aligned with SlotNames, which is what lets the emissive loop trust the pairing.
		UMaterialInstanceDynamic* Mid = Blade->CreateDynamicMaterialInstance(SlotIndex);
		Rig.SlotMids.Add(Mid);
		if (Mid != nullptr)
		{
			OwnedMids.Add(Mid);
		}
	}

	Rig.Blade = Blade;
	OwnedBlades.Add(Blade);

	// *** PLAY SOMETHING IMMEDIATELY. *** See ETraceKnifeClip's comment: an unanimated blade is a
	// SHUT blade, and the knife is carried open. This is the one PlayAnimation that is not driven by
	// a state edge, because there is no previous state to have an edge against.
	Blade->PlayAnimation(ClipIdle, /*bLooping=*/true);
	Rig.Playing = ETraceKnifeClip::Idle_Open;

	// The pivot's offset is printed with the word MEASURED or GUESSED in front of it, because those
	// are the only two things it can be and a frame taken against the wrong one proves nothing.
	UE_LOG(LogTraceGame, Log,
		TEXT("[KnifeView] %s: pack blade built, %d material slot(s), Idle_Open looping, attached to %s. ")
		TEXT("Pivot offset %s = (%.2f, %.2f, %.2f) uu, |%.2f| uu."),
		*GetNameSafe(&Pawn), Rig.SlotNames.Num(),
		Rig.bOnWrist
			? *FString::Printf(TEXT("%s's wrist_right bone (the pack's stated hold)"), *GetNameSafe(AttachParent))
			: TEXT("ViewModelRoot at this file's own rest pose (no gloved-hands rig found)"),
		bMeasuredGrip
			? TEXT("MEASURED off the hands rig's own fist (v32 §8)")
			: (Rig.bOnWrist ? TEXT("GUESSED — WristOffset fallback, the hands rig did not answer")
			                : TEXT("RestLocation, rig space")),
		Rig.bOnWrist ? WristPivot.X : TraceKnifeViewFile::RestLocation.X,
		Rig.bOnWrist ? WristPivot.Y : TraceKnifeViewFile::RestLocation.Y,
		Rig.bOnWrist ? WristPivot.Z : TraceKnifeViewFile::RestLocation.Z,
		(Rig.bOnWrist ? WristPivot : TraceKnifeViewFile::RestLocation).Size());
}

// =================================================================================================
// [SPEC v32 §4] THE STAB STREAK
// =================================================================================================

bool UTraceKnifeViewSubsystem::ResolveBladeTipOnce(FKnifeRig& Rig)
{
	if (Rig.bTipResolved)
	{
		return !Rig.TipOffsetMeshLocal.IsNearlyZero();
	}
	if (PackMesh == nullptr)
	{
		return false;
	}
	Rig.bTipResolved = true;

	// =============================================================================================
	// *** SK_TraceKnife SHIPS NO NAMED SOCKETS. THE TIP IS A MEASUREMENT, NOT A LOOKUP. ***
	// =============================================================================================
	//
	// SPEC v32 §7f records it as a disclosed pack limitation: there is no SOCKET_tip, no SOCKET_nose
	// and no SOCKET_muzzle on ANY of the five pack skeletons, and fixing that properly needs a
	// re-export from the artist. So the tip is derived the way every other landmark in this project
	// is derived — ATraceCharacter's muzzle marker is placed at the railgun's own recorded muzzle
	// vertex, ATraceTracer's per-gun table is mesh-local centimetres — and then LOGGED, because a
	// derived landmark that nobody printed is a landmark nobody can check.
	//
	// THE LONG AXIS IS WHICHEVER BOUNDS EXTENT IS LARGEST. Measured on the imported asset the answer
	// is Y: Interchange maps glTF -> UE as (gl.x, gl.z, gl.y) x 100, under which the blade points
	// along UE -Y, with tip (0, -12.6, 0), pivot (0, 0, 0) and pommel (0, +12.2, 0). Asking the
	// bounds rather than hard-coding "Y" is what survives a re-export under a different axis map —
	// and the log line says which axis it picked, so a silent change is a visible one.
	//
	// WHICH END OF THAT AXIS IS THE POINT, AND WHY THE ANSWER IS NOT A COIN FLIP. The mesh's own
	// origin IS the pivot — the pack's landmark table names the pivot as the hand attach point — so
	// the tip is the end FARTHER from the origin. On the reference pose that is not a close call:
	// the ref pose is the CLOSED balisong (the handles are folded forward over the blade), which
	// measures 16.2 uu on this axis and spans roughly -12.6 to +3.6, so the blade end is more than
	// three times as far out as the other. Even re-exported OPEN it still wins, 12.6 against 12.2.
	// Both ends go into the log line so the margin can be judged rather than assumed.
	const FBoxSphereBounds MeshBounds = PackMesh->GetBounds();
	const FVector Extent = MeshBounds.BoxExtent;

	int32 LongAxis = 0;
	if (Extent.Y > Extent[LongAxis]) { LongAxis = 1; }
	if (Extent.Z > Extent[LongAxis]) { LongAxis = 2; }

	const double LowEnd  = MeshBounds.Origin[LongAxis] - Extent[LongAxis];
	const double HighEnd = MeshBounds.Origin[LongAxis] + Extent[LongAxis];
	const bool bTipIsLowEnd = FMath::Abs(LowEnd) >= FMath::Abs(HighEnd);

	// THE OTHER TWO COMPONENTS ARE THE PIVOT'S, i.e. ZERO — deliberately NOT the bounds origin. The
	// bounds box is a box around the WHOLE knife, handles included, so its centre is pulled sideways
	// by however thick the handles are; the blade itself runs dead down the pivot's own axis, which
	// is exactly what the pack's landmark table records (tip (0, -12.6, 0), not (0.4, -12.6, -0.9)).
	// Only the long axis is a bounds question. The cross axes are an attachment-point question and
	// the attachment point is the origin.
	FVector TipLocal = FVector::ZeroVector;
	TipLocal[LongAxis] = bTipIsLowEnd ? LowEnd : HighEnd;
	Rig.TipOffsetMeshLocal = TipLocal;

	// --- WHICH BONE CARRIES IT ---------------------------------------------------------------------
	//
	// *** A COMPONENT-SPACE OFFSET WOULD NOT THRUST. *** A_Knife_Stab is "a 130 mm thrust with a
	// -0.22 rad pitch, snapping back". A landmark pinned to the COMPONENT would sit still through the
	// one 0.30 s it is visible while the blade drove forward past it, so the tip has to ride a bone.
	//
	// =============================================================================================
	// *** IT RIDES THE ROOT, AND "THE BONE NEAREST THE TIP" IS A MEASURED BUG, NOT A NEAR MISS. ***
	// =============================================================================================
	//
	// The obvious no-socket workaround is to hang the point off whichever bone's rest position is
	// closest to it. THIS FILE SHIPPED THAT FOR ONE RUN AND Trace.Knife.StreakProbe FAILED IT ON THE
	// FIRST SWING, which is the whole reason that probe measures a position rather than a flag:
	//
	//     [PackKnife] streak f31: tip=V(X=937.96 ...) pivot=V(X=949.69 ...) tip-to-pivot=12.08 uu
	//
	// — a plausible-looking 12 uu, and 12 uu BEHIND the hand. The nearest bone to the point of a
	// balisong is a HANDLE PIVOT (`handle_pivot_safe`, 13.6 uu away, which is not "near" anything),
	// and a handle pivot carries a 180-degree bind rotation that every clip but Draw undoes: express
	// a blade landmark in that bone's space and the moment the knife opens, the 13.6 uu offset flips
	// end for end and the streak is drawn off the POMMEL.
	//
	// The right carrier is the one the BLADE is rigid to, and on a butterfly knife that is by
	// definition the root: the handles are the only parts that move relative to it — that is what
	// makes it a balisong. Riding the root gives the correct answer in both clips that matter: a
	// thrust translates the root and the tip goes with it; a flip rotates the handles and the blade,
	// correctly, does not move at all.
	TArray<FTransform> RefPoseComponentSpace;
	const bool bHaveSkeleton = TraceKnifeViewFile::BuildRefPoseComponentSpace(PackMesh, RefPoseComponentSpace)
		&& RefPoseComponentSpace.Num() > 0;

	if (bHaveSkeleton)
	{
		const FReferenceSkeleton& RefSkeleton = PackMesh->GetRefSkeleton();
		const FTransform& RootRest = RefPoseComponentSpace[0];
		Rig.TipBone = RefSkeleton.GetBoneName(0);
		Rig.TipOffsetInBone = RootRest.InverseTransformPosition(TipLocal);
		Rig.TipBoneResidualUU = static_cast<float>(FVector::Dist(RootRest.GetLocation(), TipLocal));
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[KnifeView] STAB STREAK TIP RESOLVED (spec v32 s4). %s ships NO named socket, so the "
		     "point is derived from the mesh's own bounds: origin=%s extent=%s uu -> long axis %s "
		     "(half-extent %.2f uu), ends at %.2f and %.2f uu, and the tip is the end farther from the "
		     "pivot (the mesh origin, which is the pack's hand attach point). "
		     "*** RESOLVED TIP OFFSET = (%.2f, %.2f, %.2f) uu, %.2f uu from the pivot. *** "
		     "It is carried by the ROOT bone '%s' (of %d), which is what the blade is rigid to on a "
		     "balisong; offset in that bone's space = (%.2f, %.2f, %.2f) uu, %.2f uu from its rest "
		     "position. NOTE, because this number is derived and not authored: the reference pose is "
		     "the CLOSED knife, so this long-axis extreme is the FOLDED HANDLE end — about 2.4 uu "
		     "past the blade point the import pass recorded at 12.6 uu. The streak's lead absorbs "
		     "that; a re-export with a real SOCKET_tip would remove the guess entirely."),
		*GetNameSafe(PackMesh),
		*MeshBounds.Origin.ToCompactString(), *Extent.ToCompactString(),
		(LongAxis == 0) ? TEXT("X") : ((LongAxis == 1) ? TEXT("Y") : TEXT("Z")),
		Extent[LongAxis], LowEnd, HighEnd,
		TipLocal.X, TipLocal.Y, TipLocal.Z, TipLocal.Size(),
		Rig.TipBone.IsNone() ? TEXT("<none: no skeleton, using the component-space offset>") : *Rig.TipBone.ToString(),
		RefPoseComponentSpace.Num(),
		Rig.TipOffsetInBone.X, Rig.TipOffsetInBone.Y, Rig.TipOffsetInBone.Z, Rig.TipBoneResidualUU);

	return !TipLocal.IsNearlyZero();
}

void UTraceKnifeViewSubsystem::EnsureStreakBuilt(ATraceCharacter& Pawn, FKnifeRig& Rig)
{
	if (IsValid(Rig.StabStreak) || Rig.bStreakUnavailable || !IsValid(Rig.Blade))
	{
		return;
	}

	if (!ResolveBladeTipOnce(Rig))
	{
		// A mesh with no measurable long axis is not a thing that happens, but if it ever does the
		// answer is no streak rather than a plane at the origin — which would sit inside the fist.
		Rig.bStreakUnavailable = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeView] %s: no stab streak — the blade tip could not be derived from the mesh's "
			     "bounds. The blade itself is unaffected."), *GetNameSafe(&Pawn));
		return;
	}

	UStaticMesh* PlaneMesh = UTraceFxShapes::GetPlane();
	if (PlaneMesh == nullptr)
	{
		// SILENT DEGRADATION, LATCHED. /Engine/BasicShapes/Plane missing is not a state this project
		// expects, but §4's rule for the fallback path is the rule for every path: degrade without
		// warning every frame. One Display line, once, and then nothing.
		Rig.bStreakUnavailable = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeView] %s: no stab streak — /Engine/BasicShapes/Plane did not resolve."),
			*GetNameSafe(&Pawn));
		return;
	}

	UStaticMeshComponent* Streak = NewObject<UStaticMeshComponent>(
		&Pawn, MakeUniqueObjectName(&Pawn, UStaticMeshComponent::StaticClass(), FName(TEXT("KnifeStabStreak"))));
	if (Streak == nullptr)
	{
		Rig.bStreakUnavailable = true;
		return;
	}

	// --- WHERE IT HANGS, AND WHY IT IS NOT ON THE BONE --------------------------------------------
	//
	// Parented to the BLADE COMPONENT, not to TipBone, even though TipBone is what it follows. The
	// streak has to BILLBOARD — its own world rotation is recomputed every frame so the plane faces
	// the lens instead of presenting its zero-thickness edge — and a component that both inherits a
	// bone's transform and has its world transform overwritten every frame has two writers for one
	// number. Hanging it off the blade and driving its world transform from the SAMPLED bone
	// transform leaves exactly one writer, and makes the number the probe prints the number that was
	// actually written.
	Streak->SetMobility(EComponentMobility::Movable);
	Streak->SetupAttachment(Rig.Blade);
	Streak->SetStaticMesh(PlaneMesh);

	// 26 x 10 uu, converted by the library's one constant. Never by a literal /100 here.
	UTraceFxShapes::SizePlane(Streak, TraceKnifeViewFile::StreakWidthUU, TraceKnifeViewFile::StreakHeightUU);

	// The shared "this is decoration" pass: no collision, no overlaps, no shadow, no occluder.
	// CONTRACT §7 again — the capsule is the only collider on this actor, and a colliding streak
	// would break hitscan for the 0.3 s it existed.
	UTraceFxShapes::ConfigureFxComponent(Streak);

	// SAME VIEWMODEL RULES AS THE BLADE IT LIVES ON. Set before RegisterComponent so the scene proxy
	// is created with them rather than rebuilt a frame later — and a 0.3 s effect does not have a
	// frame to spare.
	Streak->SetOnlyOwnerSee(true);
	Streak->bCastHiddenShadow = false;
	Streak->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;

	// STARTS HIDDEN. The blade is carried for whole minutes between stabs and the streak is visible
	// for 0.3 s of them; the default has to be off or the first frame after the rig is built draws a
	// bright plane for no stab at all.
	Streak->SetVisibility(false);

	Streak->RegisterComponent();

	// ADDITIVE, VIA THE LIBRARY'S OWN LADDER. The FX doc wants "~0.9 opacity"; this project has no
	// translucent parent material, so Translucent resolves to Additive with the opacity riding as a
	// weight on the colour — which is TraceFxShapes' documented and deliberate stand-in, not a fudge:
	// an additive plane writes no depth, so it cannot occlude the blade it is streaking off, and on a
	// self-luminous energy weapon "can only brighten" is the right sign. THE ACHIEVED BLEND IS
	// STORED, because SetGlow takes two completely different routes for the two blends and handing it
	// the requested one instead of the achieved one is a silent no-op.
	Rig.StreakMid = UTraceFxShapes::MakeGlowMID(Streak, 0, ETraceFxBlend::Translucent, Rig.StreakBlend);
	if (Rig.StreakMid == nullptr || Rig.StreakBlend == ETraceFxBlend::None)
	{
		// Nothing resolved: HIDE IT. An untextured 100 uu default plane welded to the player's face
		// is far worse than no effect, which is the argument ETraceFxBlend::None makes in as many
		// words. One line, once.
		Rig.bStreakUnavailable = true;
		Streak->DestroyComponent();
		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeView] %s: no stab streak — no FX material resolved for it. The blade is "
			     "unaffected."), *GetNameSafe(&Pawn));
		return;
	}

	Rig.StabStreak = Streak;
	OwnedStreaks.Add(Streak);
	OwnedMids.Add(Rig.StreakMid);

	UE_LOG(LogTraceGame, Log,
		TEXT("[KnifeView] %s: stab streak built — %.0f x %.0f uu plane, peak opacity %.2f, blend %s, "
		     "riding %s."),
		*GetNameSafe(&Pawn), TraceKnifeViewFile::StreakWidthUU, TraceKnifeViewFile::StreakHeightUU,
		TraceKnifeViewFile::StreakPeakOpacity, UTraceFxShapes::BlendName(Rig.StreakBlend),
		Rig.TipBone.IsNone() ? TEXT("the blade's component-space tip") : *Rig.TipBone.ToString());
}

bool UTraceKnifeViewSubsystem::GetBladeTipWorld(const FKnifeRig& Rig, FVector& OutTipWorld) const
{
	if (!IsValid(Rig.Blade) || !Rig.bTipResolved)
	{
		return false;
	}

	// THE BONE FIRST, because that is the one that moves with the thrust. DoesSocketExist answers for
	// bones as well as sockets on a skeletal mesh, which is the whole reason a bone name can stand in
	// for the socket the pack does not ship.
	if (!Rig.TipBone.IsNone() && Rig.Blade->DoesSocketExist(Rig.TipBone))
	{
		OutTipWorld = Rig.Blade->GetSocketTransform(Rig.TipBone, RTS_World).TransformPosition(Rig.TipOffsetInBone);
		return true;
	}

	// The degradation: the component's own transform. The streak then rides the wrist but not the
	// thrust, which is worse and is still better than nothing — and it is silent.
	OutTipWorld = Rig.Blade->GetComponentTransform().TransformPosition(Rig.TipOffsetMeshLocal);
	return true;
}

void UTraceKnifeViewSubsystem::UpdateStabStreak(FKnifeRig& Rig, ETraceKnifeClip SampledClip,
	float SampledClipSeconds, bool bBladeVisible)
{
	if (!IsValid(Rig.StabStreak))
	{
		return;
	}

	// =============================================================================================
	// *** THE OPACITY IS StabFlareAt. THE FLASH'S OWN NUMBER, NOT A SECOND CURVE THAT AGREES. ***
	// =============================================================================================
	// Off the SAMPLED clip and the SAMPLED playhead — the pair read at the top of TickRig, before
	// anything could have restarted the animation. A_Knife_Stab is 0.300 s; re-reading the playhead
	// here, after ChooseClip, would read the new clip's zero on every transition frame and the
	// streak would flash its first frame every time the blade changed clip.
	const float Flare = (SampledClip == ETraceKnifeClip::Stab) ? StabFlareAt(SampledClipSeconds) : 0.f;
	const float Opacity = TraceKnifeViewFile::StreakPeakOpacity * Flare;
	Rig.StreakOpacityLast = Opacity;

	const bool bWantVisible = bBladeVisible
		&& Opacity > KINDA_SMALL_NUMBER
		&& TraceKnifeViewFile::CVarStabStreak.GetValueOnGameThread() != 0;

	// RE-ASSERTED, NOT LATCHED, for the same reason the blade's own visibility is: SetVisibility is a
	// no-op when nothing changed, and an edge trigger would leave a streak burning on a corpse.
	Rig.StabStreak->SetVisibility(bWantVisible);
	if (!bWantVisible)
	{
		return;
	}

	FVector TipWorld = FVector::ZeroVector;
	if (!GetBladeTipWorld(Rig, TipWorld))
	{
		Rig.StabStreak->SetVisibility(false);
		return;
	}

	// The blade's own axis, taken LIVE from pivot to tip, so it follows the stab's -0.22 rad pitch
	// instead of being a constant that stops matching the moment the clip rotates anything.
	const FVector Pivot = Rig.Blade->GetComponentLocation();
	FVector BladeAxis = (TipWorld - Pivot).GetSafeNormal();
	if (BladeAxis.IsNearlyZero())
	{
		BladeAxis = Rig.Blade->GetForwardVector();
	}

	// --- THE BILLBOARD, AND WHY A FIXED ROTATION WAS NOT GOOD ENOUGH -------------------------------
	//
	// A plane has no thickness. Authored at a fixed angle to the blade it is edge-on from somewhere,
	// and "somewhere" for a first-person viewmodel is wherever the artist's cant and the hand clip's
	// wrist roll happen to put it on the frame that matters — i.e. a streak that is invisible in a
	// screenshot for a reason no log line would ever explain. So the plane's normal is aimed at the
	// lens every frame it is drawn: local X (its 26 uu width) along the blade, local Z (its normal)
	// as near the camera as that allows. This costs one MakeFromXZ on the 0.3 s the streak exists.
	FVector ToLens = FVector::UpVector;
	if (const ATraceCharacter* Viewer = Rig.Pawn.Get())
	{
		if (const UCameraComponent* ViewerCamera = Viewer->FindComponentByClass<UCameraComponent>())
		{
			ToLens = (ViewerCamera->GetComponentLocation() - TipWorld).GetSafeNormal();
		}
	}
	// Degenerate when the blade points straight at the lens, which a thrust very nearly does. Any
	// vector off the axis serves; the blade's own right vector is the one that keeps the plane's
	// width lying along the screen rather than spinning frame to frame.
	if (ToLens.IsNearlyZero() || FMath::Abs(FVector::DotProduct(ToLens, BladeAxis)) > 0.99)
	{
		ToLens = Rig.Blade->GetRightVector();
	}

	// SEE StreakLeadUU: mostly trailing the point, a little ahead of it.
	const FVector Centre = TipWorld
		- BladeAxis * (TraceKnifeViewFile::StreakWidthUU * 0.5f - TraceKnifeViewFile::StreakLeadUU);

	Rig.StabStreak->SetWorldLocationAndRotation(Centre, FRotationMatrix::MakeFromXZ(BladeAxis, ToLens).ToQuat());
	Rig.StreakTipWorldLast = TipWorld;

	// Intensity 1.0 and the fade carried entirely by the opacity: on the additive path SetGlow
	// multiplies the two together anyway (for additive geometry they ARE the same physical quantity,
	// which is how much of this colour is added to what is behind it), so splitting the fade across
	// both would be the same number applied twice.
	UTraceFxShapes::SetGlow(Rig.StreakMid, Rig.StreakBlend, TraceKnifeViewFile::StreakCyanLinear,
		/*Intensity=*/1.0f, Opacity);
}

void UTraceKnifeViewSubsystem::SuppressCubeKnife(ATraceCharacter& Pawn, FKnifeRig& Rig)
{
	if (Rig.HiddenCubeRoot.IsValid() || Pawn.ViewModelRoot == nullptr)
	{
		return;
	}

	// FOUND BY NAME, AND THAT IS A DOCUMENTED COMPROMISE RATHER THAN AN OVERSIGHT — the same one
	// UTraceWeaponComponent::SetGunViewModelHidden makes about the gun's parts, for the same reason:
	// KnifeViewRoot is a private member of another agent's component and the only public handle on
	// the rig is ViewModelRoot. MakeUniqueObjectName may append a number, so the test is a PREFIX.
	//
	// THE FAILURE MODE IF THE NAME EVER MOVES IS BENIGN AND LOUD: two knives on screen at once, which
	// is instantly obvious, rather than no knife, which reads as a rendering bug.
	TArray<USceneComponent*> Children;
	Pawn.ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);

	for (USceneComponent* Child : Children)
	{
		if (Child == nullptr || Child == Rig.Blade)
		{
			continue;
		}
		if (!Child->GetName().StartsWith(TEXT("KnifeViewRoot")))
		{
			continue;
		}

		// *** bHiddenInGame, NOT SetVisibility, AND THE DISTINCTION IS THE WHOLE DESIGN. ***
		// A primitive draws only when IsVisible() AND !bHiddenInGame. UTraceWeaponComponent owns the
		// visibility flag on those cubes and re-asserts it on its own schedule; this file owns the
		// hidden flag and nothing else in the project writes it on a knife component. Two writers,
		// two flags, no fight — which is the alternative to editing a file another agent holds.
		Child->SetHiddenInGame(/*bNewHidden=*/true, /*bPropagateToChildren=*/true);
		Rig.HiddenCubeRoot = Child;

		UE_LOG(LogTraceGame, Log,
			TEXT("[KnifeView] %s: the procedural cube blade (%s) is hidden; the pack blade has it."),
			*GetNameSafe(&Pawn), *Child->GetName());
		return;
	}
}

bool UTraceKnifeViewSubsystem::HasFinished(const UAnimSequence* Sequence, float PlayheadSeconds)
{
	if (Sequence == nullptr)
	{
		return true;
	}

	// A NON-LOOPING SINGLE-NODE CLIP SATURATES AT ITS LENGTH — that saturation IS "holding the last
	// frame", and it is what A_Knife_Draw is supposed to do. The tolerance is one 60 Hz frame, so a
	// clip that stops a hair short of its own length is still called finished rather than sticking.
	return PlayheadSeconds >= Sequence->GetPlayLength() - (1.f / 60.f);
}

ETraceKnifeClip UTraceKnifeViewSubsystem::ChooseClip(const ATraceCharacter& Pawn, FKnifeRig& Rig,
	double NowSeconds, float PlayheadSeconds) const
{
	// =============================================================================================
	// THE STATE MACHINE. FOUR LINES OF PRIORITY, AND EVERY INPUT IS AUTHORED GAMEPLAY STATE.
	// =============================================================================================
	//
	// Nothing here reads a clock this file started. `Deploy` and `Cooldown` are the weapon
	// component's own remaining-time accessors, `bKnife` is the selector, and the only timestamp is
	// the flourish's, which is compared against the SEQUENCE'S OWN LENGTH rather than against a
	// duration typed in twice.
	const bool bKnife = TraceMelee::IsKnifeEquipped(&Pawn);
	const float Deploy = TraceMelee::GetDeployRemaining(&Pawn);
	const float Cooldown = TraceMelee::GetSwingCooldownRemaining(&Pawn);

	// --- the two edges, taken BEFORE anything is chosen -------------------------------------------
	//
	// FIRST SIGHT never draws. A pawn is met with whatever weapon it already has, and a Draw on the
	// first tick would flip the balisong open at spawn for no reason a player could connect to a
	// press. Seeding and returning is the same rule UTraceAudioWatchSubsystem::TickGunshots applies
	// to the clip count for the same reason.
	const bool bFirstLook = !Rig.bSeen;
	const bool bDrawEdge = !bFirstLook && bKnife && !Rig.bKnifeLast;

	// A SWING JUST STARTED IF THE COOLDOWN ROSE. It only ever counts DOWN otherwise, so a rise is
	// unambiguous and — this is the point — it CANNOT BE MISSED THE WAY A BOOLEAN CAN. A_Knife_Stab
	// is 0.300 s and the swing cooldown is longer than the animation, so polling "is a swing in
	// flight" would work; polling "did one start" is what survives two swings back to back inside
	// one animation length, which a boolean would render as one continuous stab.
	const bool bStabEdge = !bFirstLook && (Cooldown > Rig.SwingCooldownLast + KINDA_SMALL_NUMBER);

	Rig.bSeen = true;
	Rig.bKnifeLast = bKnife;
	Rig.SwingCooldownLast = Cooldown;

	if (!bKnife || !Pawn.IsAlive())
	{
		// NOT `None`. The blade is hidden this frame, but the pose it is holding is what it will be
		// showing the instant it comes back — and the reference pose is SHUT. Idle_Open is the only
		// safe thing to be holding while invisible.
		Rig.InspectStartedWorldSeconds = -1.0e9;
		return ETraceKnifeClip::Idle_Open;
	}

	// 1. THE STAB. Highest priority, unconditionally, and that is what makes the flourish
	//    interruptible by a real action rather than merely "usually interruptible".
	if (bStabEdge && ClipStab != nullptr)
	{
		Rig.InspectStartedWorldSeconds = -1.0e9;
		return ETraceKnifeClip::Stab;
	}
	if (Rig.Playing == ETraceKnifeClip::Stab && !HasFinished(ClipStab, PlayheadSeconds))
	{
		// *** LET THE CLIP FINISH, AND END IT ON THE CLIP'S OWN LENGTH. ***
		//
		// MEASURED BUG, FIXED HERE: this used to read `Rig.Playing == Stab` with no end condition at
		// all, which is not "let it finish" — it is a state with no exit. The blade would hold the
		// stab's last frame until some OTHER edge (a weapon change, a death) knocked it out, and the
		// idle would never come back. Bounding it on the sequence is also what keeps it independent
		// of the swing cooldown, which is a different number that happens to be longer today.
		return ETraceKnifeClip::Stab;
	}

	// 2. THE DRAW. "The ONLY clip that starts shut; play it on weapon switch and hold the last
	//    frame." The switch IS the deploy: while GetDeployRemaining is non-zero the weapon component
	//    is running the pullout, and that pullout is v31 §1's 35%-shorter one — so this reads the
	//    live number and never a copy of it.
	if (bDrawEdge && ClipDraw != nullptr)
	{
		Rig.InspectStartedWorldSeconds = -1.0e9;
		return ETraceKnifeClip::Draw;
	}
	if (Rig.Playing == ETraceKnifeClip::Draw && !HasFinished(ClipDraw, PlayheadSeconds))
	{
		// =========================================================================================
		// *** THE PULLOUT AND THE CLIP ARE DIFFERENT LENGTHS, AND THE CLIP WINS. MEASURED BUG. ***
		// =========================================================================================
		//
		// This used to read `... && Deploy > 0.f`, and the first run of Trace.Knife.PackDemo caught
		// it in one line: "Draw -> Idle_Open at t=8.157", 0.134 s after Draw started. A_Knife_Draw is
		// 0.5167 s. The knife pullout after v31 §1 is 0.2 x 0.65 = 0.13 s, so gating on the pullout
		// truncated the authored balisong flip to a QUARTER of itself and the blade snapped open
		// with no flip at all — the exact "do not re-time them by eye" failure the spec warns about,
		// arrived at by gating on the wrong quantity rather than by typing a wrong number.
		//
		// The gameplay pullout and the presentation are simply different durations and always were.
		// The pullout says when you may swing; the clip says how the blade looks getting there. So
		// the clip runs to its own end and then HOLDS its last frame, which is what §5 asks for in
		// as many words: "play it on weapon switch and hold the last frame".
		//
		// `Deploy` is still read above for the Draw EDGE and by RequestInspect. It just no longer
		// decides when an animation is over.
		return ETraceKnifeClip::Draw;
	}

	// 3. THE FLOURISH. Lowest priority of the three actions, which is what "cosmetic" means here.
	if (Rig.InspectStartedWorldSeconds > -1.0e8 && ClipInspect != nullptr)
	{
		if (Rig.Playing == ETraceKnifeClip::Inspect)
		{
			// RUNNING: ended by the SEQUENCE'S OWN PLAYHEAD, not by a stopwatch started when the key
			// was pressed. The two agree at 60 fps and stop agreeing the moment the world is paused,
			// time-dilated or hitching — and the flourish is 3.2 s, which is long enough for all
			// three to happen inside one press.
			if (!HasFinished(ClipInspect, PlayheadSeconds))
			{
				return ETraceKnifeClip::Inspect;
			}
			Rig.InspectStartedWorldSeconds = -1.0e9;
		}
		else
		{
			// REQUESTED BUT NOT YET STARTED — the single frame between RequestInspect writing the
			// timestamp and the tick below calling PlayAnimation. The world clock is the right tool
			// for exactly this one frame, and the half-second ceiling is a stale-request guard: if
			// something outranked the flourish on the frame it was asked for, the request expires
			// instead of firing later from a queue nobody can see.
			if ((NowSeconds - Rig.InspectStartedWorldSeconds) <= 0.5)
			{
				return ETraceKnifeClip::Inspect;
			}
			Rig.InspectStartedWorldSeconds = -1.0e9;
		}
	}

	// 4. CARRIED OPEN. The resting state, and the only looping clip.
	return ETraceKnifeClip::Idle_Open;
}

float UTraceKnifeViewSubsystem::StabFlareAt(float ClipSeconds) const
{
	// A FRACTION OF THE CLIP'S OWN MEASURED LENGTH, not of a duration typed in here. 0.3f is only
	// the value used when the sequence is absent, which is the fallback path — where nothing is
	// drawn anyway and the arithmetic just has to stay finite.
	const float Length = (ClipStab != nullptr) ? ClipStab->GetPlayLength() : 0.3f;
	const float Alpha = (Length > KINDA_SMALL_NUMBER) ? (ClipSeconds / Length) : 0.f;
	return TraceKnifeViewFile::StabFlare(Alpha);
}

void UTraceKnifeViewSubsystem::ComputeEmissive(ETraceKnifeClip Clip, float ClipSeconds,
	float& OutCyan, float& OutAmber) const
{
	using namespace TraceKnifeViewFile;

	// The default everywhere is the OPEN idle, breathing. The knife is carried open.
	const float IdleAlpha = (ClipIdle != nullptr && ClipIdle->GetPlayLength() > KINDA_SMALL_NUMBER)
		? (ClipSeconds / ClipIdle->GetPlayLength())
		: 0.f;
	const float IdleBreath = Breathe(IdleAlpha);

	OutCyan = FMath::Lerp(CyanOpenIdleLow, CyanOpenIdleHigh, IdleBreath);
	OutAmber = FMath::Lerp(AmberOpenIdleLow, AmberOpenIdleHigh, IdleBreath);

	switch (Clip)
	{
	case ETraceKnifeClip::Idle_Open:
	case ETraceKnifeClip::None:
		return;

	case ETraceKnifeClip::Draw:
	{
		// *** THE ONLY PLACE THE CLOSED IDLE IS EVER SEEN. *** Draw starts shut and ends open, so
		// the emissive starts on the closed band and arrives on the open one, with the flip peak on
		// the frame the balisong snaps. The stats file gives both bands; using the closed one for the
		// front of this clip is what makes the difference between them mean anything at all.
		const float Length = (ClipDraw != nullptr) ? ClipDraw->GetPlayLength() : 0.5167f;
		const float Alpha = (Length > KINDA_SMALL_NUMBER) ? FMath::Clamp(ClipSeconds / Length, 0.f, 1.f) : 0.f;
		const float ClosedCyan = FMath::Lerp(CyanClosedIdleLow, CyanClosedIdleHigh, IdleBreath);
		const float ClosedAmber = FMath::Lerp(AmberClosedIdleLow, AmberClosedIdleHigh, IdleBreath);
		const float OpenCyan = OutCyan;
		const float OpenAmber = OutAmber;

		if (Alpha <= DrawSnapFraction)
		{
			// Closed, rising into the snap.
			const float T = (DrawSnapFraction > KINDA_SMALL_NUMBER) ? (Alpha / DrawSnapFraction) : 1.f;
			OutCyan = FMath::Lerp(ClosedCyan, CyanFlipPeak, T);
			OutAmber = FMath::Lerp(ClosedAmber, AmberFlipPeak, T);
		}
		else
		{
			// Caught: falling from the peak onto the OPEN band, which is where the clip ends.
			const float T = (Alpha - DrawSnapFraction) / FMath::Max(KINDA_SMALL_NUMBER, 1.f - DrawSnapFraction);
			OutCyan = FMath::Lerp(CyanFlipPeak, OpenCyan, FMath::Clamp(T, 0.f, 1.f));
			OutAmber = FMath::Lerp(AmberFlipPeak, OpenAmber, FMath::Clamp(T, 0.f, 1.f));
		}
		return;
	}

	case ETraceKnifeClip::Stab:
	{
		// Cyan 4.4x, amber 3.0x — the brightest the blade ever gets, on the shortest clip it has.
		//
		// [SPEC v32 §4] THE TRIANGLE USED TO BE WRITTEN OUT HERE. It now lives in StabFlareAt, and
		// this branch is one of its two callers; the streak plane is the other. That is not tidying:
		// §4 requires the streak's opacity and this flash to be driven off the same number, and one
		// function is the only version of "the same number" that a later edit cannot quietly break.
		const float Shaped = StabFlareAt(ClipSeconds);
		OutCyan = FMath::Lerp(OutCyan, CyanStabPeak, Shaped);
		OutAmber = FMath::Lerp(OutAmber, AmberStabPeak, Shaped);
		return;
	}

	case ETraceKnifeClip::Inspect:
	{
		// *** THE FOUR CATCH BEATS. "cyan spikes to 3.6x on each of the four catch beats." ***
		//
		// Driven off ClipSeconds — the ANIMATION'S OWN PLAYHEAD, sampled at the top of the tick —
		// and not off a timer this file runs. That is the difference between a flare that lands on
		// the frame the handles meet and one that drifts a frame per hitch until it is lighting the
		// wrong beat. Nothing about this survives being rewritten as an accumulator.
		float Best = 0.f;
		for (const FCatchBeat& Beat : InspectCatchBeats)
		{
			float Weight = 0.f;
			if (ClipSeconds >= Beat.Start && ClipSeconds <= Beat.End)
			{
				// Rising across the flip: brightest ON the catch, which is the beat's END.
				Weight = (ClipSeconds - Beat.Start) / FMath::Max(KINDA_SMALL_NUMBER, Beat.End - Beat.Start);
			}
			else if (ClipSeconds > Beat.End && ClipSeconds <= Beat.End + CatchFallSeconds)
			{
				// Settling after it.
				Weight = 1.f - (ClipSeconds - Beat.End) / CatchFallSeconds;
			}
			Best = FMath::Max(Best, FMath::Clamp(Weight, 0.f, 1.f));
		}
		OutCyan = FMath::Lerp(OutCyan, CyanFlipPeak, Best);
		OutAmber = FMath::Lerp(OutAmber, AmberFlipPeak, Best);
		return;
	}
	}
}

void UTraceKnifeViewSubsystem::TickRig(ATraceCharacter& Pawn, FKnifeRig& Rig, float /*DeltaSeconds*/)
{
	EnsureBladeBuilt(Pawn, Rig);
	if (!IsValid(Rig.Blade))
	{
		return;
	}

	SuppressCubeKnife(Pawn, Rig);

	// [SPEC v33] THE HELD POSE, RE-ASSERTED. Constant in the bone's frame — the blade is rigid with
	// the wrist and turns with the clip, which is what "held" means — so this only actually changes
	// anything when one of the three Trace.Knife.Hold* knobs moves or when a hands rig that was not
	// ready at build time finally answers. Both of those are worth one quaternion multiply a frame.
	ApplyWristHold(Pawn, Rig);

	// [SPEC v32 §4] After the blade, because the streak's tip is derived from the blade's mesh and
	// hangs off the blade's component. Once-latched inside; a no-op on every frame but the first.
	EnsureStreakBuilt(Pawn, Rig);

	UWorld* World = Pawn.GetWorld();
	const double NowSeconds = (World != nullptr) ? World->GetTimeSeconds() : 0.0;

	// =============================================================================================
	// *** SAMPLE BEFORE YOU ADVANCE. THIS LINE, AND ITS POSITION, ARE THE POINT. ***
	// =============================================================================================
	// The playhead is read ONCE, here, into a local — before ChooseClip can decide to start a
	// different clip and before PlayAnimation can reset it to zero. Everything downstream reads the
	// local. Reading it again after the state machine would read the NEW clip's zero every time a
	// clip changed, and the emissive curve would flash its first frame on every transition. The
	// spec's warning names this exact shape twice ("the SMG's shot frame never being sampled because
	// the phase advanced before it was read"), and A_Knife_Stab is 0.300 s — 18 frames.
	const UAnimSingleNodeInstance* Node = Rig.Blade->GetSingleNodeInstance();
	const float SampledClipSeconds = (Node != nullptr) ? Node->GetCurrentTime() : 0.f;
	const ETraceKnifeClip SampledClip = Rig.Playing;

	const ETraceKnifeClip Desired = ChooseClip(Pawn, Rig, NowSeconds, SampledClipSeconds);

	// --- visibility -------------------------------------------------------------------------------
	//
	// RE-ASSERTED EVERY TICK, NOT LATCHED, for the reason UTraceWeaponComponent::SetGunViewModelHidden
	// documents at length: ATraceCharacter is the other writer of the rig's visibility and re-shows
	// everything on its own schedule (a respawn, the third-person carry blend returning). An edge
	// trigger would miss that and leave a blade on a corpse. SetVisibility is a no-op when nothing
	// changed, so this costs a comparison.
	const bool bWantVisible = TraceMelee::IsKnifeEquipped(&Pawn) && Pawn.IsAlive() && Pawn.IsViewModelVisible();
	Rig.Blade->SetVisibility(bWantVisible);

	// --- the clip edge ----------------------------------------------------------------------------
	//
	// *** PlayAnimation ONLY ON A CHANGE. *** UAnimSingleNodeInstance restarts from t=0 on every
	// call, so a per-frame call is a clip that plays its first frame sixty times a second and never
	// advances. This one `if` is the difference between an animation and a freeze-frame.
	if (Desired != Rig.Playing)
	{
		if (UAnimSequence* Sequence = SequenceFor(Desired))
		{
			// LOOPING FOR THE IDLE, ONE-SHOT FOR EVERYTHING ELSE — and a one-shot HOLDS ITS LAST
			// FRAME, which is exactly what "play Draw on weapon switch and hold the last frame" asks
			// for. No timer stops it and nothing has to remember to.
			const bool bLoop = (Desired == ETraceKnifeClip::Idle_Open);
			Rig.Blade->PlayAnimation(Sequence, bLoop);

			if (TraceKnifeViewFile::CVarPackKnifeLog.GetValueOnGameThread() != 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[KnifeView] %s: %s -> %s (%.4fs, %s) at t=%.3f"),
					*GetNameSafe(&Pawn), LexTraceKnifeClip(Rig.Playing), LexTraceKnifeClip(Desired),
					Sequence->GetPlayLength(), bLoop ? TEXT("loop") : TEXT("one-shot"), NowSeconds);
			}

			Rig.Playing = Desired;
		}
	}

	// --- the emissive -----------------------------------------------------------------------------
	//
	// Driven from the SAMPLED playhead and the SAMPLED clip, i.e. from where the blade was when this
	// frame began — which is the pose the renderer is about to draw. Using `Desired` here on a
	// transition frame would light the new clip's first frame with the old clip's playhead.
	float Cyan = 0.f;
	float Amber = 0.f;
	ComputeEmissive(SampledClip, SampledClipSeconds, Cyan, Amber);

	for (int32 SlotIndex = 0; SlotIndex < Rig.SlotMids.Num(); ++SlotIndex)
	{
		UMaterialInstanceDynamic* Mid = Rig.SlotMids[SlotIndex];
		if (Mid == nullptr)
		{
			continue;
		}

		// WHICH BAND A SLOT IS ON IS DECIDED BY ITS NAME, which is the glTF material name and is the
		// same string the MI asset is named after. `core_amber` gets the amber band, everything else
		// gets cyan — including `shell`, `plating` and `carbon`, whose materials have no emissive
		// contribution at all, so writing the parameter on them is free and keeps this loop from
		// needing a second table that could disagree with the first.
		const bool bAmber = Rig.SlotNames.IsValidIndex(SlotIndex)
			&& Rig.SlotNames[SlotIndex].ToString().Contains(TEXT("amber"));
		Mid->SetScalarParameterValue(TraceKnifeViewFile::EmissiveIntensityParam, bAmber ? Amber : Cyan);
	}

	// --- the stab streak (spec v32 §4) ------------------------------------------------------------
	//
	// Fed the SAME two locals the emissive was, for the same reason, so the plane's opacity and the
	// blade's 4.4x cyan are two readings of one number at one instant. bWantVisible is passed rather
	// than re-derived: a streak drawn on a frame the blade is not is a floating plane.
	UpdateStabStreak(Rig, SampledClip, SampledClipSeconds, bWantVisible);
}

bool UTraceKnifeViewSubsystem::RequestInspect(ATraceCharacter* Pawn)
{
	if (!IsValid(Pawn) || TraceKnifeViewFile::CVarPackKnife.GetValueOnGameThread() == 0)
	{
		return false;
	}

	// =============================================================================================
	// EVERY REFUSAL, IN ONE PLACE, AND NONE OF THEM TOUCHES GAMEPLAY.
	// =============================================================================================
	//
	// A cosmetic key must not be able to change anything, so this reads state and writes exactly one
	// presentation timestamp. It does not call into the weapon component, it does not take a
	// cooldown, it sends no RPC, and nothing in the project treats "inspecting" as a condition.
	if (!Pawn->IsAlive())
	{
		return false;
	}

	// THE CARRIER IS REFUSED, and the reason is the pack's own loadout table rather than a gameplay
	// rule: the Core is a TWO-HAND CRADLE. A knife flourish with both hands on the objective is not a
	// pose that exists, and §6's Idle_Core is the clip the hands will be running.
	if (Pawn->IsCarrier())
	{
		return false;
	}

	if (!TraceMelee::IsKnifeEquipped(Pawn))
	{
		return false;
	}

	// MID-PULLOUT AND MID-SWING ARE BOTH REFUSED, which is the same rule stated from the other side:
	// a real action always beats the flourish, so the flourish never starts on top of one.
	if (TraceMelee::GetDeployRemaining(Pawn) > 0.f || TraceMelee::GetSwingCooldownRemaining(Pawn) > 0.f)
	{
		return false;
	}

	FKnifeRig* Rig = RecordFor(Pawn);
	if (Rig == nullptr || ClipInspect == nullptr)
	{
		return false;
	}

	// ALREADY FLOURISHING: refused rather than restarted. Re-pressing F halfway through and watching
	// the blade snap back to frame zero reads as a bug, and "the second press does nothing" is the
	// behaviour every inspect key in the genre has.
	UWorld* World = Pawn->GetWorld();
	const double NowSeconds = (World != nullptr) ? World->GetTimeSeconds() : 0.0;
	if (Rig->InspectStartedWorldSeconds > -1.0e8
		&& (NowSeconds - Rig->InspectStartedWorldSeconds) <= static_cast<double>(ClipInspect->GetPlayLength()))
	{
		return false;
	}

	Rig->InspectStartedWorldSeconds = NowSeconds;

	UE_LOG(LogTraceGame, Verbose, TEXT("[KnifeView] %s: inspect requested (%.3fs flourish)."),
		*GetNameSafe(Pawn), ClipInspect->GetPlayLength());
	return true;
}

bool UTraceKnifeViewSubsystem::IsInspecting(const ATraceCharacter* Pawn) const
{
	const FKnifeRig* Rig = FindRecord(Pawn);
	if (Rig == nullptr || ClipInspect == nullptr || Pawn == nullptr)
	{
		return false;
	}
	const UWorld* World = Pawn->GetWorld();
	const double NowSeconds = (World != nullptr) ? World->GetTimeSeconds() : 0.0;
	return Rig->InspectStartedWorldSeconds > -1.0e8
		&& (NowSeconds - Rig->InspectStartedWorldSeconds) <= static_cast<double>(ClipInspect->GetPlayLength());
}

int32 UTraceKnifeViewSubsystem::VisibleBladeParts(const ATraceCharacter* Pawn) const
{
	// IsVisible(), not bHiddenInGame and not "is the knife equipped". The census's whole value is
	// that it asks the COMPONENT what it is drawing rather than asking either rig what it believes,
	// so this asks the same question the same way: a blade that some other system hid still counts
	// as not drawn, and a blade drawn for a reason this file does not know about still counts as
	// drawn. TickRig writes SetVisibility from (knife equipped && alive && viewmodel visible), so in
	// the ordinary case this returns 1 exactly while the player can see a blade.
	const FKnifeRig* Rig = FindRecord(Pawn);
	return (Rig != nullptr && IsValid(Rig->Blade) && Rig->Blade->IsVisible()) ? 1 : 0;
}

void UTraceKnifeViewSubsystem::SampleForHarness(const ATraceCharacter* Pawn, ETraceKnifeClip& OutClip,
	float& OutSeconds, float& OutCyan, float& OutAmber) const
{
	OutClip = ETraceKnifeClip::None;
	OutSeconds = 0.f;
	OutCyan = 0.f;
	OutAmber = 0.f;

	const FKnifeRig* Rig = FindRecord(Pawn);
	if (Rig == nullptr || !IsValid(Rig->Blade))
	{
		return;
	}

	const UAnimSingleNodeInstance* Node = Rig->Blade->GetSingleNodeInstance();
	OutClip = Rig->Playing;
	OutSeconds = (Node != nullptr) ? Node->GetCurrentTime() : 0.f;
	ComputeEmissive(OutClip, OutSeconds, OutCyan, OutAmber);
}

void UTraceKnifeViewSubsystem::SampleStreakForHarness(const ATraceCharacter* Pawn, bool& bOutBuilt,
	bool& bOutVisible, float& OutOpacity, FVector& OutTipWorld, FVector& OutPivotWorld,
	FName& OutTipBone, FVector& OutBladeAimWorld) const
{
	bOutBuilt = false;
	bOutVisible = false;
	OutOpacity = 0.f;
	OutTipWorld = FVector::ZeroVector;
	OutPivotWorld = FVector::ZeroVector;
	OutTipBone = NAME_None;

	// A ZERO AXIS IS THE HONEST "NO ANSWER". Every caller signs a distance with this, and a unit
	// vector invented for a pawn with no blade would make a meaningless projection look like a
	// measurement. Dotting with zero gives 0.00 uu, which the streak probe's threshold rejects.
	OutBladeAimWorld = FVector::ZeroVector;

	const FKnifeRig* Rig = FindRecord(Pawn);
	if (Rig == nullptr || !IsValid(Rig->Blade))
	{
		return;
	}

	OutPivotWorld = Rig->Blade->GetComponentLocation();
	OutTipBone = Rig->TipBone;

	// THE BLADE'S OWN AIM, TAKEN OFF THE COMPONENT AND NOT OFF THE STREAK'S PARENT BONE. The pack
	// aims its blade down mesh -Y (the import pass's landmarks: tip (0, -12.6, 0), pivot at the
	// origin, pommel (0, +12.2, 0)), and the component's Y axis is that axis carried into the world
	// by whatever is holding the knife. Reading it from the streak's own parent would make the
	// projection self-referential and unable to catch a flipped bind — which is the one defect it
	// exists to catch.
	OutBladeAimWorld = -Rig->Blade->GetComponentTransform().GetUnitAxis(EAxis::Y);
	bOutBuilt = IsValid(Rig->StabStreak);
	if (!bOutBuilt)
	{
		return;
	}

	// THE COMPONENT IS ASKED, NOT THE RECORD. IsVisible() reads both of the two independent flags a
	// primitive draws on, so a streak something else hid still reports as not drawn — the same
	// argument VisibleBladeParts makes about the census, applied to the thing §4 has to prove is on
	// screen. The POSITION, by contrast, is the one this file last WROTE rather than the component's
	// current location, because the two differ by a frame of the parent's motion and the number a
	// verifier wants is the one the driver decided on.
	bOutVisible = Rig->StabStreak->IsVisible();
	OutOpacity = Rig->StreakOpacityLast;
	OutTipWorld = Rig->StreakTipWorldLast;
}

void UTraceKnifeViewSubsystem::DescribeTo(TArray<FString>& OutLines) const
{
	OutLines.Add(FString::Printf(
		TEXT("[PackKnife] Trace.Knife.PackArt=%d   -TraceNoCharacterArt=%d   assetsResolved=%d"),
		TraceKnifeViewFile::CVarPackKnife.GetValueOnGameThread(),
		TraceKnifeViewFile::ArtDisabledOnCommandLine() ? 1 : 0, bAssetsResolved ? 1 : 0));

	OutLines.Add(FString::Printf(
		TEXT("[PackKnife] mesh=%s  idle=%s  draw=%s  stab=%s  inspect=%s"),
		*GetNameSafe(PackMesh), *GetNameSafe(ClipIdle), *GetNameSafe(ClipDraw),
		*GetNameSafe(ClipStab), *GetNameSafe(ClipInspect)));

	if (PackMesh == nullptr)
	{
		OutLines.Add(TEXT("[PackKnife] THE FALLBACK IS LIVE: nothing was built and nothing was hidden, "
		                  "so the six-cube procedural blade is what is on screen. That is the supported "
		                  "state for a checkout with no `git lfs pull`."));
		return;
	}

	for (const FKnifeRig& Rig : Rigs)
	{
		const ATraceCharacter* Pawn = Rig.Pawn.Get();
		OutLines.Add(FString::Printf(
			TEXT("[PackKnife] %-20s blade=%s visible=%d playing=%-9s slots=%d cubeHidden=%d "
			     "inspecting=%d"),
			*GetNameSafe(Pawn), IsValid(Rig.Blade) ? TEXT("built") : TEXT("none"),
			(IsValid(Rig.Blade) && Rig.Blade->IsVisible()) ? 1 : 0,
			LexTraceKnifeClip(Rig.Playing), Rig.SlotNames.Num(),
			Rig.HiddenCubeRoot.IsValid() ? 1 : 0, IsInspecting(Pawn) ? 1 : 0));
		OutLines.Add(FString::Printf(
			TEXT("[PackKnife]     hangs from %s"),
			Rig.bOnWrist ? TEXT("the gloved hands' wrist_right BONE (Trace.Knife.PackAttach 1)")
			             : TEXT("ViewModelRoot at this file's rest pose (no hands rig, or PackAttach 0)")));

		// [SPEC v32 §4] THE STREAK, INCLUDING ITS MEASURED SIZE. The 26 x 10 uu is read BACK off the
		// live component's scale through the library's own inverse, rather than reprinted from the
		// constant that set it: a verifier that re-derives the number it is checking is only checking
		// its own arithmetic, and the size on screen is the thing the FX doc actually specifies.
		if (IsValid(Rig.StabStreak))
		{
			const FVector StreakScale = Rig.StabStreak->GetRelativeScale3D();
			OutLines.Add(FString::Printf(
				TEXT("[PackKnife]     streak: built, visible=%d opacity=%.3f blend=%s size=%.1f x %.1f uu "
				     "tip='%s' offset=(%.2f, %.2f, %.2f) uu  Trace.Knife.StabStreak=%d"),
				Rig.StabStreak->IsVisible() ? 1 : 0, Rig.StreakOpacityLast,
				UTraceFxShapes::BlendName(Rig.StreakBlend),
				UTraceFxShapes::LengthUUFromShapeScale(static_cast<float>(StreakScale.X)),
				UTraceFxShapes::LengthUUFromShapeScale(static_cast<float>(StreakScale.Y)),
				Rig.TipBone.IsNone() ? TEXT("<component space>") : *Rig.TipBone.ToString(),
				Rig.TipOffsetMeshLocal.X, Rig.TipOffsetMeshLocal.Y, Rig.TipOffsetMeshLocal.Z,
				TraceKnifeViewFile::CVarStabStreak.GetValueOnGameThread()));
		}
		else
		{
			OutLines.Add(FString::Printf(
				TEXT("[PackKnife]     streak: NOT BUILT (%s)"),
				Rig.bStreakUnavailable ? TEXT("degraded — see the one-line reason above")
				                       : TEXT("not built yet")));
		}

		if (IsValid(Rig.Blade))
		{
			if (const UAnimSingleNodeInstance* Node = Rig.Blade->GetSingleNodeInstance())
			{
				float Cyan = 0.f;
				float Amber = 0.f;
				ComputeEmissive(Rig.Playing, Node->GetCurrentTime(), Cyan, Amber);
				OutLines.Add(FString::Printf(
					TEXT("[PackKnife]     playhead %.4f / %.4f s   EmissiveIntensity cyan %.2fx  amber %.2fx"),
					Node->GetCurrentTime(),
					(SequenceFor(Rig.Playing) != nullptr) ? SequenceFor(Rig.Playing)->GetPlayLength() : 0.f,
					Cyan, Amber));
			}
		}
	}

	if (Rigs.Num() == 0)
	{
		OutLines.Add(TEXT("[PackKnife] no locally-controlled pawn has been seen yet — this is a "
		                  "first-person viewmodel and it is only ever built for the player looking "
		                  "out of it."));
	}
}

// =================================================================================================
// [SPEC v33] Trace.Knife.HoldProbe — THE HOLD, IN THE ONLY FRAME THAT CAN DESCRIBE IT.
// =================================================================================================
//
// The user's verdict is "the knife clips through the hand and is not held naturally at all", and
// before this function there was no measurement in the project that could either confirm or repair
// it: PackStatus says WHICH rig is on screen, StreakProbe says where the POINT is. Neither says
// where the HANDLE is relative to the fingers, and that is the entire question.
//
// Everything below is printed in wrist_right's OWN frame, because that is the frame the blade is
// attached in and therefore the only frame in which its relative location is directly editable. The
// hand's bones are read LIVE and POSED — GetSocketTransform on the hands component — so the numbers
// describe the clip that is actually playing rather than the reference pose the last two defects in
// this area both came from.
void UTraceKnifeViewSubsystem::DescribeHoldTo(TArray<FString>& OutLines) const
{
	const FKnifeRig* Found = nullptr;
	for (const FKnifeRig& Candidate : Rigs)
	{
		if (IsValid(Candidate.Blade))
		{
			Found = &Candidate;
			break;
		}
	}

	if (Found == nullptr)
	{
		OutLines.Add(TEXT("[KnifeHold] no built pack blade on this machine — nothing to measure. "
		                  "(Equip the knife first; the rig is built on the frame it is needed.)"));
		return;
	}

	const FKnifeRig& Rig = *Found;
	USkeletalMeshComponent* Hands = Cast<USkeletalMeshComponent>(Rig.Blade->GetAttachParent());
	const FName Socket = Rig.Blade->GetAttachSocketName();

	OutLines.Add(FString::Printf(
		TEXT("[KnifeHold] blade '%s' attached to '%s' socket '%s'; relative loc=(%.2f, %.2f, %.2f) "
		     "rot=(P %.1f, Y %.1f, R %.1f) scale %.3f."),
		*GetNameSafe(Rig.Blade), *GetNameSafe(Hands), *Socket.ToString(),
		Rig.Blade->GetRelativeLocation().X, Rig.Blade->GetRelativeLocation().Y,
		Rig.Blade->GetRelativeLocation().Z,
		Rig.Blade->GetRelativeRotation().Pitch, Rig.Blade->GetRelativeRotation().Yaw,
		Rig.Blade->GetRelativeRotation().Roll,
		Rig.Blade->GetRelativeScale3D().X));

	if (Hands == nullptr || Socket.IsNone())
	{
		OutLines.Add(TEXT("[KnifeHold] the blade is NOT on a hand bone — there is no hold to measure. "
		                  "This is the ViewModelRoot fallback pose (Trace.Knife.PackAttach 0, or no "
		                  "pack hands)."));
		return;
	}

	// THE FRAME. Live and posed, not the reference pose: the two placement defects this file has
	// already shipped were both a reference-pose number used against a live one.
	const FTransform WristWorld = Hands->GetSocketTransform(Socket, RTS_World);

	// --- THE FRAME ITSELF, IN RIG SPACE -----------------------------------------------------------
	//
	// *** WITHOUT THIS THE REST OF THE PROBE IS UNREADABLE. *** Every number below is in the wrist
	// bone's own axes, and nothing in this project is authored in those axes — ViewModelRoot's are
	// +X out of the lens, +Y right, +Z up, and that is the frame every placement decision is made in.
	// So the wrist's three axis directions are printed in RIG space, which is what turns "the fingers
	// run along bone -Y" into "the fingers point forward and down".
	if (const ATraceCharacter* BasisPawn = Rig.Pawn.Get())
	{
		if (BasisPawn->ViewModelRoot != nullptr)
		{
			const FTransform RigWorld = BasisPawn->ViewModelRoot->GetComponentTransform();
			const FTransform WristInRig = WristWorld.GetRelativeTransform(RigWorld);
			const FVector Ax = WristInRig.GetUnitAxis(EAxis::X);
			const FVector Ay = WristInRig.GetUnitAxis(EAxis::Y);
			const FVector Az = WristInRig.GetUnitAxis(EAxis::Z);
			OutLines.Add(FString::Printf(
				TEXT("[KnifeHold] wrist frame in RIG space (+X out of the lens, +Y right, +Z up): "
				     "origin (%.2f, %.2f, %.2f); bone +X -> (%.3f, %.3f, %.3f); bone +Y -> (%.3f, %.3f, "
				     "%.3f); bone +Z -> (%.3f, %.3f, %.3f)."),
				WristInRig.GetLocation().X, WristInRig.GetLocation().Y, WristInRig.GetLocation().Z,
				Ax.X, Ax.Y, Ax.Z, Ay.X, Ay.Y, Ay.Z, Az.X, Az.Y, Az.Z));
		}
	}

	// --- THE HAND ---------------------------------------------------------------------------------
	//
	// Every bone within 30 uu of the wrist, which on this skeleton is the right hand and nothing
	// else, sorted by how far down the hand it sits. The knuckle line and the thumb come out of this
	// list as positions rather than as an assumption about which axis "along the fingers" is.
	struct FNearBone
	{
		FName Name;
		FVector WristLocal;
	};
	TArray<FNearBone> Near;

	if (const USkeletalMesh* HandsMesh = Hands->GetSkeletalMeshAsset())
	{
		const FReferenceSkeleton& Ref = HandsMesh->GetRefSkeleton();
		for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
		{
			const FName BoneName = Ref.GetBoneName(BoneIndex);
			const FVector Local = WristWorld.InverseTransformPosition(
				Hands->GetSocketTransform(BoneName, RTS_World).GetLocation());
			if (Local.Size() <= 30.f)
			{
				Near.Add({ BoneName, Local });
			}
		}
	}

	Near.Sort([](const FNearBone& A, const FNearBone& B) { return A.WristLocal.Size() < B.WristLocal.Size(); });

	OutLines.Add(FString::Printf(
		TEXT("[KnifeHold] %d bone(s) of the gloved hand within 30 uu of '%s', LIVE and POSED, in that "
		     "bone's own frame:"), Near.Num(), *Socket.ToString()));
	for (const FNearBone& Bone : Near)
	{
		OutLines.Add(FString::Printf(TEXT("[KnifeHold]     %-22s (%7.2f, %7.2f, %7.2f)  |%.2f| uu"),
			*Bone.Name.ToString(), Bone.WristLocal.X, Bone.WristLocal.Y, Bone.WristLocal.Z,
			Bone.WristLocal.Size()));
	}

	// --- THE FIST, AS THE HANDS RIG ITSELF REPORTS IT ---------------------------------------------
	FVector GripWristLocal = FVector::ZeroVector;
	const ATraceCharacter* Pawn = Rig.Pawn.Get();
	const bool bHaveGrip = (Pawn != nullptr) && Pawn->GetViewModelGripWristLocal(GripWristLocal);
	if (bHaveGrip)
	{
		OutLines.Add(FString::Printf(
			TEXT("[KnifeHold] fist centroid (ATraceCharacter::GetViewModelGripWristLocal, the number "
			     "the placement is built on) = (%.2f, %.2f, %.2f) uu, |%.2f| from the wrist."),
			GripWristLocal.X, GripWristLocal.Y, GripWristLocal.Z, GripWristLocal.Size()));
	}

	// --- THE GRIP BASIS AND THE FOUR KNOBS THAT SIT ON IT -----------------------------------------
	//
	// [SPEC v33] Without this line the rest of the probe cannot say WHY the blade is where it is: the
	// landmarks below are an outcome, and these are the inputs that produced them. The basis is the
	// captured one — the same object the placement used this frame — so a swimming basis would show
	// up here as an axis that disagrees with the knuckle bones printed above.
	if (Rig.bBaseWristCaptured)
	{
		const FVector Across = Rig.GripBasisBone.GetAxisX();
		const FVector Along = Rig.GripBasisBone.GetAxisY();
		const FVector Palm = Rig.GripBasisBone.GetAxisZ();
		const float SwingDeg =
			FMath::Clamp(TraceKnifeViewFile::CVarHoldSwing.GetValueOnGameThread(), 0.f, 90.f);
		const float SwingRad = FMath::DegreesToRadians(SwingDeg);
		const FVector BladeAxis =
			(FMath::Cos(SwingRad) * -Across + FMath::Sin(SwingRad) * Along).GetSafeNormal();

		OutLines.Add(FString::Printf(
			TEXT("[KnifeHold] grip basis in wrist space (captured once, off live posed bones): "
			     "across/knuckle-line (%.3f, %.3f, %.3f); along/hand (%.3f, %.3f, %.3f); "
			     "palm-ward (%.3f, %.3f, %.3f)."),
			Across.X, Across.Y, Across.Z, Along.X, Along.Y, Along.Z, Palm.X, Palm.Y, Palm.Z));
		OutLines.Add(FString::Printf(
			TEXT("[KnifeHold] HoldSwing %.1f deg (0 = down the grip channel, 90 = along the fingers) "
			     "-> blade axis (%.3f, %.3f, %.3f); HoldGrip %.2f; HoldPalm %.2f uu; HoldFlat %.1f deg."),
			SwingDeg, BladeAxis.X, BladeAxis.Y, BladeAxis.Z,
			TraceKnifeViewFile::CVarHoldGrip.GetValueOnGameThread(),
			TraceKnifeViewFile::CVarHoldPalm.GetValueOnGameThread(),
			TraceKnifeViewFile::CVarHoldFlat.GetValueOnGameThread()));
	}
	else
	{
		OutLines.Add(TEXT("[KnifeHold] the grip basis has NOT been captured — the blade is on the "
		                  "fist-centroid fallback with no measured attitude. Either the hands rig is "
		                  "still building, or one of index_right_0 / pinky_right_0 / "
		                  "knuckle_bar_right is missing from this skeleton."));
	}

	// --- THE BLADE'S OWN LANDMARKS, IN THE SAME FRAME ---------------------------------------------
	//
	// Pommel, pivot and tip, each transformed out of the blade's LIVE component transform and back
	// into the wrist's. "The handle is inside the fingers" and "the pommel is out through the back of
	// the wrist" are then two subtractions rather than two opinions.
	if (PackMesh != nullptr)
	{
		const FBoxSphereBounds MeshBounds = PackMesh->GetBounds();
		const FVector Extent = MeshBounds.BoxExtent;
		int32 LongAxis = 0;
		if (Extent.Y > Extent[LongAxis]) { LongAxis = 1; }
		if (Extent.Z > Extent[LongAxis]) { LongAxis = 2; }

		FVector LowLocal = FVector::ZeroVector;
		FVector HighLocal = FVector::ZeroVector;
		LowLocal[LongAxis] = MeshBounds.Origin[LongAxis] - Extent[LongAxis];
		HighLocal[LongAxis] = MeshBounds.Origin[LongAxis] + Extent[LongAxis];

		const FTransform BladeWorld = Rig.Blade->GetComponentTransform();
		auto ToWrist = [&](const FVector& MeshLocal)
		{
			return WristWorld.InverseTransformPosition(BladeWorld.TransformPosition(MeshLocal));
		};

		const FVector PivotW = ToWrist(FVector::ZeroVector);
		const FVector LowW = ToWrist(LowLocal);
		const FVector HighW = ToWrist(HighLocal);

		OutLines.Add(FString::Printf(
			TEXT("[KnifeHold] blade landmarks in wrist space, FROM THE REFERENCE (CLOSED) BOUNDS — the "
			     "handles are folded over the blade there, so this is the shut knife's envelope and "
			     "NOT where the open handle is: pivot (%.2f, %.2f, %.2f), long-axis ends "
			     "(%.2f, %.2f, %.2f) and (%.2f, %.2f, %.2f)  [mesh-local %.2f and %.2f on %s]."),
			PivotW.X, PivotW.Y, PivotW.Z, LowW.X, LowW.Y, LowW.Z, HighW.X, HighW.Y, HighW.Z,
			LowLocal[LongAxis], HighLocal[LongAxis],
			(LongAxis == 0) ? TEXT("X") : ((LongAxis == 1) ? TEXT("Y") : TEXT("Z"))));

		// --- AND THE BLADE AS IT IS ACTUALLY POSED ------------------------------------------------
		//
		// The three landmarks above come off the mesh's REFERENCE bounds, which on a balisong is the
		// CLOSED knife — the handles folded forward over the blade. The clip on screen is Idle_Open.
		// So the live bones are printed too, in the blade's own mesh space, and THAT is what says
		// where the handle halves are while the knife is being held.
		if (const USkeletalMesh* BladeMesh = Rig.Blade->GetSkeletalMeshAsset())
		{
			const FTransform BladeWorldNow = Rig.Blade->GetComponentTransform();
			const FReferenceSkeleton& BladeRef = BladeMesh->GetRefSkeleton();
			OutLines.Add(FString::Printf(
				TEXT("[KnifeHold] the blade's %d bones AS POSED, in the blade's own mesh space:"),
				BladeRef.GetNum()));
			for (int32 BoneIndex = 0; BoneIndex < BladeRef.GetNum(); ++BoneIndex)
			{
				const FName BoneName = BladeRef.GetBoneName(BoneIndex);
				const FVector MeshLocal = BladeWorldNow.InverseTransformPosition(
					Rig.Blade->GetSocketTransform(BoneName, RTS_World).GetLocation());
				OutLines.Add(FString::Printf(TEXT("[KnifeHold]     %-24s (%7.2f, %7.2f, %7.2f)"),
					*BoneName.ToString(), MeshLocal.X, MeshLocal.Y, MeshLocal.Z));
			}
		}

		if (bHaveGrip)
		{
			OutLines.Add(FString::Printf(
				TEXT("[KnifeHold] fist-to-pivot %.2f uu, fist-to-low-end %.2f uu, fist-to-high-end "
				     "%.2f uu — again, the two ends are the CLOSED envelope's. A NATURAL HOLD PUTS "
				     "THE FIST ON THE HANDLE, i.e. the fist near the MIDDLE of the handle half and "
				     "the pivot OUTSIDE the fingers."),
				FVector::Dist(GripWristLocal, PivotW), FVector::Dist(GripWristLocal, LowW),
				FVector::Dist(GripWristLocal, HighW)));

			// --- THE RULE THE HOLD IS ACTUALLY GRADED ON, MEASURED ON THIS FRAME'S POSE -----------
			//
			// [SPEC v34] Everything above describes an ENVELOPE. This is the one number that says
			// whether the fist is holding anything: the grip point the placement put in the hand is
			// re-derived here off the safe handle's LIVE bone and compared with the fist. It must be
			// HoldPalm uu away and nothing more — a metre of blade can be anywhere and the knife is
			// still held; this going non-zero is the air gap, and it is what the inspect-late frame
			// photographed before the handle was tracked.
			const float Slide = TraceKnifeViewFile::OpenHandleLengthUU
				* FMath::Clamp(TraceKnifeViewFile::CVarHoldGrip.GetValueOnGameThread(), 0.f, 1.f);

			if (!Rig.GripHandleBone.IsNone() && Rig.Blade->DoesSocketExist(Rig.GripHandleBone))
			{
				const FTransform HandleLive =
					Rig.Blade->GetSocketTransform(Rig.GripHandleBone, RTS_Component);
				const FVector GripMeshLocal =
					(Rig.GripHandleRefInv * HandleLive).TransformPosition(FVector(0.f, -Slide, 0.f));
				const FVector GripW = ToWrist(GripMeshLocal);

				// The knuckle line, re-read here so the verdict carries its own yardstick: "outboard
				// of the index knuckle" is only a sentence until the two x values sit next to it.
				const float IndexKnuckleX = WristWorld.InverseTransformPosition(
					Hands->GetSocketTransform(TraceKnifeViewFile::IndexKnuckleRightBone, RTS_World)
						.GetLocation()).X;
				const float PinkyKnuckleX = WristWorld.InverseTransformPosition(
					Hands->GetSocketTransform(TraceKnifeViewFile::PinkyKnuckleRightBone, RTS_World)
						.GetLocation()).X;

				OutLines.Add(FString::Printf(
					TEXT("[KnifeHold] GRIP POINT, tracked on '%s' this frame: mesh-local (%.2f, %.2f, "
					     "%.2f) -> wrist (%.2f, %.2f, %.2f); fist-to-grip %.2f uu (HoldPalm is %.2f, "
					     "so THAT is the number this should equal). Knuckle line spans x %.2f to "
					     "%.2f."),
					*Rig.GripHandleBone.ToString(), GripMeshLocal.X, GripMeshLocal.Y, GripMeshLocal.Z,
					GripW.X, GripW.Y, GripW.Z, FVector::Dist(GripWristLocal, GripW),
					TraceKnifeViewFile::CVarHoldPalm.GetValueOnGameThread(),
					IndexKnuckleX, PinkyKnuckleX));
			}
			else
			{
				OutLines.Add(FString::Printf(
					TEXT("[KnifeHold] GRIP POINT is NOT tracked — no '%s' bone on this blade, so the "
					     "fist is on the old constant %.2f uu offset down the blade axis and cannot "
					     "follow a flip."),
					*TraceKnifeViewFile::HandleSafeBone.ToString(), Slide));
			}
		}
	}
}

#if !UE_BUILD_SHIPPING

namespace TraceKnifeViewFile
{
	/** The game world a console command should act on. */
	static UWorld* PlayableWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld()
				&& (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	static void PackStatus()
	{
		UTraceKnifeViewSubsystem* Driver = UTraceKnifeViewSubsystem::Get(PlayableWorld());
		if (Driver == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[PackKnife] no knife view subsystem on this machine (a dedicated server has "
				     "no viewmodel, which is correct)."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Knife.PackStatus (spec v31 s5) ================"));

		TArray<FString> Lines;
		Driver->DescribeTo(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line);
		}
	}

	static void PackInspect()
	{
		UWorld* World = PlayableWorld();
		const APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		if (Pawn == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[PackKnife] no local player pawn to inspect with."));
			return;
		}

		const bool bStarted = TraceKnifeView::RequestInspect(Pawn);
		UE_LOG(LogTraceGame, Display,
			TEXT("[PackKnife] inspect request -> %s. (Refused is normal: dead, carrying, no knife, "
			     "mid-pullout, mid-swing, or already flourishing.)"),
			bStarted ? TEXT("STARTED") : TEXT("refused"));
	}

	// ---------------------------------------------------------------------------------------------
	// *** Trace.Knife.PackDemo — THE WHOLE OF §5, DRIVEN AND SAMPLED, WITHOUT A KEYBOARD. ***
	// ---------------------------------------------------------------------------------------------
	//
	// A photograph cannot show that a 0.300 s clip advanced, and a log line saying "PlayAnimation was
	// called" is not evidence that it did. This drives the four clips in the order a player would
	// meet them and SAMPLES THE PLAYHEAD every frame, so the report is a series of measured positions
	// rather than a claim:
	//
	//     equip the knife  -> Draw   (0.5167 s, and it must HOLD its last frame, not rewind)
	//     the pullout ends -> Idle_Open loops
	//     the flourish     -> Inspect (3.200 s) and the four catch beats' cyan peaks
	//     a swing          -> Stab   (0.300 s) interrupting the flourish, which is §5's
	//                                "a real action must interrupt it", measured
	//
	// IT WAITS ON REAL STATE, NOT ON A SCHEDULE. The step from Draw to the flourish is gated on
	// TraceMelee::GetDeployRemaining reaching zero, so the demo cannot pass by being lucky with
	// timings on a fast machine and fail on a slow one.
	struct FPackDemo
	{
		/**
		 * NOT CACHED. The pawn is re-resolved from the local controller every tick, because the one
		 * thing that reliably happens between "the command was typed" and "the demo runs" is a
		 * RESPAWN: locking in a character at the select screen destroys the placeholder pawn and
		 * possesses a new one. A weak pointer taken at t=0 goes null there and the demo reports "the
		 * pawn went away" for a run in which nothing was wrong.
		 */
		int32 Phase = 0;
		double PhaseStartedReal = 0.0;
		/** Seconds to let a respawn land before phase 0 does anything. See the command below. */
		double SettleSeconds = 0.0;
		float MaxCyanSeen = 0.f;
		float StabSecondsSeen = 0.f;
		float InspectSecondsSeen = 0.f;
		int32 CatchPeaks = 0;
		bool bStabInterruptedInspect = false;

		/** [SPEC v32 §4] The streak, observed on the same swing. Reported, not graded — see below. */
		bool bStreakSeen = false;
		float MaxStreakOpacity = 0.f;
		float StreakForwardUU = 0.f;
	};

	static TSharedPtr<FPackDemo> GPackDemo;

	static bool PackDemoTick(float /*Delta*/)
	{
		TSharedPtr<FPackDemo> Demo = GPackDemo;
		if (!Demo.IsValid())
		{
			return false;
		}

		UWorld* World = PlayableWorld();
		const APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		UTraceKnifeViewSubsystem* Driver = UTraceKnifeViewSubsystem::Get(World);

		const double Elapsed = FPlatformTime::Seconds() - Demo->PhaseStartedReal;

		if (Pawn == nullptr || Driver == nullptr)
		{
			// A GAP IS NOT A FAILURE. Between locking in a character and the new pawn being
			// possessed there are frames with no pawn at all; waiting through them is what lets this
			// demo be fired from the same -TraceExec batch as Trace.Characters.Select.
			if (Elapsed < 12.0)
			{
				return true;
			}
			UE_LOG(LogTraceGame, Warning,
				TEXT("[PackKnife] demo: no local player pawn for 12s; giving up."));
			GPackDemo.Reset();
			return false;
		}

		// SAMPLED FIRST, ALWAYS. Same rule as the driver's own tick and for the same reason: whatever
		// this function does below may change the clip, and the number that matters is where the
		// blade WAS this frame.
		TArray<FString> Lines;
		Driver->DescribeTo(Lines);

		switch (Demo->Phase)
		{
		case 0:
			// A SETTLE WINDOW, so this can be fired from the same -TraceExec batch as
			// Trace.Characters.Select without racing the respawn that lands a moment later.
			if (Elapsed < Demo->SettleSeconds)
			{
				return true;
			}
			UE_LOG(LogTraceGame, Display,
				TEXT("================ Trace.Knife.PackDemo (spec v31 s5) ================"));
			UE_LOG(LogTraceGame, Display,
				TEXT("[PackKnife] demo: %s, equipping the knife (key 3's verb)."), *GetNameSafe(Pawn));
			TraceMelee::RequestEquipIfDifferent(Pawn, ETraceEquippedWeapon::Knife);
			Demo->Phase = 1;
			Demo->PhaseStartedReal = FPlatformTime::Seconds();
			return true;

		case 1:
			// WAIT ON THE PULLOUT ITSELF. v31 §1 made the knife's pullout 35% shorter, so the number
			// is 0.13 s today and could be anything tomorrow; asking the live state is what stops
			// this harness from encoding a stale constant.
			//
			// *** AND WAIT ON THE KNIFE ACTUALLY BEING IN HAND, WHICH IS NOT THE SAME QUESTION.
			// MEASURED FLAKE, FIXED AT INTEGRATION (v32). *** The deploy clock reads ZERO in two
			// completely different situations: the pullout has finished, and the equip request has
			// not been applied yet. On a fast frame this phase saw the second one — the log line
			// read "pullout complete after 0.002s", RequestInspect landed while the pawn still had
			// a gun out and was refused, and the harness sat through its own 12 s safety net and
			// printed FAIL over a flourish that had never been asked for. Caught on the FIRST run
			// of this integration pass; the immediate re-run passed, which is exactly what makes it
			// worth fixing rather than re-rolling. IsWeaponEquipped is the two-sided question, so
			// the phase now leaves only when the knife is both in hand AND finished being drawn.
			if ((!TraceMelee::IsWeaponEquipped(Pawn, ETraceEquippedWeapon::Knife)
					|| TraceMelee::GetDeployRemaining(Pawn) > 0.f)
				&& Elapsed < 3.0)
			{
				return true;
			}
			// *** AND THE REQUEST'S OWN ANSWER IS READ, WHICH IT WAS NOT. SECOND HALF OF THE SAME
			// MEASURED FLAKE. *** RequestInspect returns a bool and lists seven refusals in one
			// block (dead, carrier, no knife, mid-pullout, mid-swing, no rig, already flourishing).
			// This harness threw that bool away, so a refused flourish was indistinguishable from a
			// flourish that played and was not sampled — and the verdict blamed the driver either
			// way. Two of the three runs in this integration pass failed here.
			//
			// So: retry every tick until the driver accepts, inside the same 3 s budget the wait
			// above uses, and if it never does, say so at Error level and let the verdict fail for
			// a reason a reader can act on. Retrying is safe by that block's own rule — the
			// "already flourishing" guard makes a second request a no-op rather than a restart.
			if (!TraceKnifeView::RequestInspect(Pawn))
			{
				if (Elapsed < 3.0)
				{
					return true; // not yet — the driver refused, try again next frame
				}
				UE_LOG(LogTraceGame, Error,
					TEXT("[PackKnife] demo: THE FLOURISH WAS REFUSED for %.1fs and never started. "
					     "UTraceKnifeViewSubsystem::RequestInspect says no when the pawn is dead, is the "
					     "CARRIER, has no knife in hand (in hand: %d), is mid-pullout (%.3fs left) or "
					     "mid-swing (%.3fs left). The verdict below is about THAT, not about the clips."),
					Elapsed,
					TraceMelee::IsWeaponEquipped(Pawn, ETraceEquippedWeapon::Knife) ? 1 : 0,
					TraceMelee::GetDeployRemaining(Pawn), TraceMelee::GetSwingCooldownRemaining(Pawn));
			}
			else
			{
				for (const FString& Line : Lines) { UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line); }
				UE_LOG(LogTraceGame, Display,
					TEXT("[PackKnife] demo: pullout complete after %.3fs (knife in hand: %d). Flourish ACCEPTED."),
					Elapsed, TraceMelee::IsWeaponEquipped(Pawn, ETraceEquippedWeapon::Knife) ? 1 : 0);
			}
			Demo->Phase = 2;
			Demo->PhaseStartedReal = FPlatformTime::Seconds();
			return true;

		case 2:
		{
			// Sample the flourish. The playhead and the emissive both come from the DRIVER — the same
			// two functions the renderer is being fed by — rather than being re-derived here. A
			// harness that re-derives the value it is checking cannot fail, which is the argument
			// Trace.Audio.Loudness makes about UTraceAudioSubsystem::VolumeFor in as many words.
			float Cyan = 0.f;
			float Amber = 0.f;
			float Seconds = 0.f;
			ETraceKnifeClip Clip = ETraceKnifeClip::None;
			Driver->SampleForHarness(Pawn, Clip, Seconds, Cyan, Amber);

			if (Clip == ETraceKnifeClip::Inspect)
			{
				Demo->InspectSecondsSeen = FMath::Max(Demo->InspectSecondsSeen, Seconds);
				if (Cyan > Demo->MaxCyanSeen)
				{
					Demo->MaxCyanSeen = Cyan;
				}
				if (Cyan >= CyanFlipPeak - 0.05f)
				{
					++Demo->CatchPeaks;
				}
			}

			// HALF WAY THROUGH THE FLOURISH, SWING. §5: "it must be interruptible by a real action."
			//
			// *** GATED ON THE CLIP'S OWN PLAYHEAD, NOT ON THE WALL CLOCK. MEASURED FLAKE, FIXED. ***
			// This used to fire at 1.6 s of REAL time counted from the end of the pullout, which is
			// not the same thing at all: A_Knife_Draw (0.5167 s) outranks the flourish and has to
			// finish first, and a screenshot hitch eats more. One run interrupted at 0.483 s of clip
			// and reported FAIL for a system that was working. The playhead is the quantity the
			// verdict is about, so it is the quantity the trigger reads — the same rule the driver
			// itself follows, applied to the harness that grades it.
			//
			// 1.6 s of CLIP is inside beat 3 (reopen, 1.66-2.05 is next) and past two catch peaks,
			// so the interruption lands in the middle of the flourish rather than near an edge where
			// it would prove less. The wall-clock term is a 12 s safety net, not the trigger.
			if ((Clip == ETraceKnifeClip::Inspect && Seconds >= 1.6f) || Elapsed >= 12.0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[PackKnife] demo: INTERRUPTING the flourish with a real swing at clip t=%.3f "
					     "of 3.200 (real time %.2fs)."), Seconds, Elapsed);
				TraceMelee::RequestSwing(Pawn);
				Demo->Phase = 3;
				Demo->PhaseStartedReal = FPlatformTime::Seconds();
			}
			return true;
		}

		case 3:
		{
			float Cyan = 0.f;
			float Amber = 0.f;
			float Seconds = 0.f;
			ETraceKnifeClip Clip = ETraceKnifeClip::None;
			Driver->SampleForHarness(Pawn, Clip, Seconds, Cyan, Amber);

			if (Clip == ETraceKnifeClip::Stab)
			{
				Demo->bStabInterruptedInspect = true;
				Demo->StabSecondsSeen = FMath::Max(Demo->StabSecondsSeen, Seconds);
			}

			// [SPEC v32 §4] The streak, sampled on the same frames — the swing this demo already
			// makes is a real one, so the plane is up for 0.3 s of it and there is no reason to make
			// a second swing to see it. REPORTED, NOT GRADED: this demo's verdict is §5's rule (the
			// authored clips advance and a real action beats the cosmetic one), and hanging a §4
			// clause off it would make one PASS/FAIL answer two different questions. §4's own rule is
			// red-armed in Trace.Knife.StreakProbe.
			{
				bool bStreakBuilt = false;
				bool bStreakVisible = false;
				float StreakOpacity = 0.f;
				FVector StreakTip = FVector::ZeroVector;
				FVector StreakPivot = FVector::ZeroVector;
				FName StreakBone = NAME_None;
				FVector StreakBladeAim = FVector::ZeroVector;
				Driver->SampleStreakForHarness(Pawn, bStreakBuilt, bStreakVisible, StreakOpacity,
					StreakTip, StreakPivot, StreakBone, StreakBladeAim);
				if (bStreakVisible && StreakOpacity > Demo->MaxStreakOpacity)
				{
					Demo->MaxStreakOpacity = StreakOpacity;
					// SIGNED, down THE BLADE'S OWN AXIS — see FStreakProbe::PeakForwardUU for why a
					// plain distance cannot tell the point of the knife from its pommel, and for why
					// the player's aim ray is the wrong axis to sign it against now that the hold
					// puts the knife across the fist rather than down the aim.
					Demo->StreakForwardUU = static_cast<float>(
						FVector::DotProduct(StreakTip - StreakPivot, StreakBladeAim));
					Demo->bStreakSeen = true;
				}
			}

			if (Elapsed < 1.2)
			{
				return true;
			}

			for (const FString& Line : Lines) { UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line); }

			UE_LOG(LogTraceGame, Display,
				TEXT("[PackKnife] demo: stab streak (spec v32 s4) %s during that swing — peak opacity "
				     "%.3f, drawn %+.2f uu down the aim ray from the blade's pivot. Graded by "
				     "Trace.Knife.StreakProbe, not here."),
				Demo->bStreakSeen ? TEXT("WAS DRAWN") : TEXT("was NOT drawn"),
				Demo->MaxStreakOpacity, Demo->StreakForwardUU);

			// THE FOUR THINGS §5 ASKS FOR, EACH AS ITS OWN CONDITION so a failure names itself:
			// the flourish ran past the middle of its own clip, its cyan reached the doc's 3.6x on at
			// least one sampled catch frame, a real action took the blade, and the stab clip then
			// advanced through most of its 0.300 s rather than being restarted every frame.
			const bool bPass = Demo->bStabInterruptedInspect
				&& Demo->InspectSecondsSeen >= 1.5f
				&& Demo->StabSecondsSeen > 0.10f
				&& Demo->CatchPeaks > 0;

			// TWO CALLS AND NOT A TERNARY VERBOSITY: UE_LOG pastes its second argument onto
			// `ELogVerbosity::`, so an expression there does not compile.
#define TRACE_PACKKNIFE_VERDICT_TEXT \
	TEXT("TRACE PACKKNIFE VERDICT: %s - the flourish advanced to %.3fs of 3.200 and peaked at %.2fx " \
	     "cyan on %d sampled catch frame(s); a real swing then took the blade and A_Knife_Stab " \
	     "advanced to %.3fs of 0.300.")
#define TRACE_PACKKNIFE_VERDICT_ARGS \
	(bPass ? TEXT("PASS") : TEXT("FAIL")), Demo->InspectSecondsSeen, Demo->MaxCyanSeen, \
	Demo->CatchPeaks, Demo->StabSecondsSeen

			if (bPass)
			{
				UE_LOG(LogTraceGame, Display, TRACE_PACKKNIFE_VERDICT_TEXT, TRACE_PACKKNIFE_VERDICT_ARGS);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TRACE_PACKKNIFE_VERDICT_TEXT, TRACE_PACKKNIFE_VERDICT_ARGS);
			}

#undef TRACE_PACKKNIFE_VERDICT_ARGS
#undef TRACE_PACKKNIFE_VERDICT_TEXT

			GPackDemo.Reset();
			return false;
		}

		default:
			GPackDemo.Reset();
			return false;
		}
	}

	static void PackDemo(const TArray<FString>& Args)
	{

		TSharedPtr<FPackDemo> Demo = MakeShared<FPackDemo>();
		Demo->PhaseStartedReal = FPlatformTime::Seconds();
		// A settle window, so `Trace.Characters.Select 1|Trace.Knife.PackDemo` in one -TraceExec
		// batch works: the select respawns the pawn a moment later and the demo must not start on
		// the placeholder that is about to be destroyed.
		Demo->SettleSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atod(*Args[0]), 0.0, 20.0) : 2.5;
		GPackDemo = Demo;

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&PackDemoTick), 0.f);
	}

	// ---------------------------------------------------------------------------------------------
	// *** Trace.Knife.StreakProbe — §4's ONE PIECE OF GEOMETRY, MEASURED WHILE IT IS ON SCREEN. ***
	// ---------------------------------------------------------------------------------------------
	//
	// The streak exists for 0.300 s and peaks 0.105 s in. Nothing about that can be checked by typing
	// a command and reading a status line: by the time a human has pressed return the plane has been
	// gone for seconds. So this SWINGS, and then samples every frame the streak is up, printing four
	// things per frame that together answer the only question worth asking — is the plane where the
	// point of the knife is, or is it at the origin / buried in the fist:
	//
	//   * the clip playhead and the opacity, which must rise to 0.9 and fall back;
	//   * the tip in world space and the blade's PIVOT in world space, and the distance between
	//     them — the pivot is the hand attach point, so a distance of ~0 IS "inside the hand" and a
	//     distance of a blade's length is the point;
	//   * where that lands ON SCREEN, through the first-person morph the renderer actually applies,
	//     so "on screen" is a pixel and not an assertion.
	//
	// RED ARM: Trace.Knife.StabStreak 0. The probe must then report NEVER VISIBLE and FAIL.
	struct FStreakProbe
	{
		int32 Frames = 0;
		double StartedReal = 0.0;
		/** Seconds to let a respawn land before anything is driven. See the command below. */
		double SettleSeconds = 0.0;
		bool bSwung = false;
		/** Elapsed seconds at which the swing was actually requested; the sample window runs off it. */
		double SwungAtReal = 0.0;
		bool bEverVisible = false;
		float PeakOpacity = 0.f;
		float PeakTipToPivotUU = 0.f;
		/**
		 * (tip - pivot) projected onto the AIM RAY, at the peak frame.
		 *
		 * *** THIS, AND NOT THE PLAIN DISTANCE, IS THE CHECK. *** A plain distance cannot tell the
		 * point of a knife from its pommel: both are about twelve centimetres from the pivot, in
		 * opposite directions, and the first version of this probe passed a streak drawn off the BUTT
		 * of the handle for exactly that reason. The blade is aimed down the ray the player is looking
		 * along (see PackAimYawCorrectionDeg), so a SIGNED projection separates the two: positive and
		 * large is the point, near zero is inside the fist, negative is the pommel.
		 */
		float PeakForwardUU = 0.f;
		FVector2D PeakScreen = FVector2D::ZeroVector;
		bool bPeakOnScreen = false;
		FIntPoint ViewportSize = FIntPoint::ZeroValue;
	};

	static TSharedPtr<FStreakProbe> GStreakProbe;

	static bool StreakProbeTick(float /*Delta*/)
	{
		TSharedPtr<FStreakProbe> Probe = GStreakProbe;
		if (!Probe.IsValid())
		{
			return false;
		}

		UWorld* World = PlayableWorld();
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		UTraceKnifeViewSubsystem* Driver = UTraceKnifeViewSubsystem::Get(World);
		const double Elapsed = FPlatformTime::Seconds() - Probe->StartedReal;

		if (Pawn == nullptr || Driver == nullptr)
		{
			if (Elapsed < 12.0)
			{
				return true;
			}
			UE_LOG(LogTraceGame, Warning, TEXT("[PackKnife] streak probe: no local pawn for 12s; giving up."));
			GStreakProbe.Reset();
			return false;
		}

		// EQUIP FIRST, THEN WAIT ON THE REAL PULLOUT. Swinging mid-deploy is refused by the weapon
		// component, and a probe that fired into a refusal would print "never visible" for a system
		// that works — the false red this project's red-arm rule is meant to protect.
		if (!Probe->bSwung)
		{
			if (Elapsed < Probe->SettleSeconds)
			{
				return true;
			}
			TraceMelee::RequestEquipIfDifferent(Pawn, ETraceEquippedWeapon::Knife);
			if (!TraceMelee::IsKnifeEquipped(Pawn) || TraceMelee::GetDeployRemaining(Pawn) > 0.f)
			{
				return (Elapsed < Probe->SettleSeconds + 6.0);
			}
			UE_LOG(LogTraceGame, Display,
				TEXT("================ Trace.Knife.StreakProbe (spec v32 s4) ================"));
			TraceMelee::RequestSwing(Pawn);
			Probe->bSwung = true;
			Probe->SwungAtReal = Elapsed;
			return true;
		}

		bool bBuilt = false;
		bool bVisible = false;
		float Opacity = 0.f;
		FVector TipWorld = FVector::ZeroVector;
		FVector PivotWorld = FVector::ZeroVector;
		FName TipBone = NAME_None;
		FVector BladeAimWorld = FVector::ZeroVector;
		Driver->SampleStreakForHarness(Pawn, bBuilt, bVisible, Opacity, TipWorld, PivotWorld, TipBone,
			BladeAimWorld);

		float ClipSeconds = 0.f;
		float Cyan = 0.f;
		float Amber = 0.f;
		ETraceKnifeClip Clip = ETraceKnifeClip::None;
		Driver->SampleForHarness(Pawn, Clip, ClipSeconds, Cyan, Amber);

		if (bVisible)
		{
			Probe->bEverVisible = true;
			const float TipToPivot = static_cast<float>(FVector::Dist(TipWorld, PivotWorld));
			const float ForwardUU = static_cast<float>(
				FVector::DotProduct(TipWorld - PivotWorld, BladeAimWorld));

			// THROUGH THE MORPH, NOT THROUGH THE RAW TRANSFORM. See MorphToFirstPerson: the streak is
			// a first-person primitive and the renderer re-projects it, so the raw world point
			// projects to a pixel nothing was ever drawn at.
			FVector2D Screen = FVector2D::ZeroVector;
			const bool bOnScreen = (PC != nullptr)
				&& PC->ProjectWorldLocationToScreen(MorphToFirstPerson(Pawn, TipWorld), Screen, /*bPlayerViewportRelative=*/true);

			if (Opacity > Probe->PeakOpacity)
			{
				Probe->PeakOpacity = Opacity;
				Probe->PeakTipToPivotUU = TipToPivot;
				Probe->PeakForwardUU = ForwardUU;
				Probe->PeakScreen = Screen;
				Probe->bPeakOnScreen = bOnScreen;
			}

			if (const ULocalPlayer* LP = PC->GetLocalPlayer())
			{
				if (LP->ViewportClient != nullptr)
				{
					FVector2D Size = FVector2D::ZeroVector;
					LP->ViewportClient->GetViewportSize(Size);
					Probe->ViewportSize = FIntPoint(FMath::RoundToInt(Size.X), FMath::RoundToInt(Size.Y));
				}
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[PackKnife] streak f%02d: clip=%s t=%.4f  opacity=%.3f cyan=%.2fx  tip=%s  "
				     "pivot=%s  tip-to-pivot=%.2f uu  DOWN-THE-BLADE=%+.2f uu  screen=(%.0f, %.0f)%s"),
				Probe->Frames, LexTraceKnifeClip(Clip), ClipSeconds, Opacity, Cyan,
				*TipWorld.ToCompactString(), *PivotWorld.ToCompactString(), TipToPivot, ForwardUU,
				Screen.X, Screen.Y, bOnScreen ? TEXT("") : TEXT(" [no projection]"));
			++Probe->Frames;
		}

		// The swing's own real-time window: A_Knife_Stab is 0.300 s, so 1.2 s past the swing is
		// comfortably past the fade with room for a hitch, and the swing itself may have been waited
		// on for several seconds before that.
		if (Elapsed < Probe->SwungAtReal + 1.2)
		{
			return true;
		}

		// THE RULE, AS THREE SEPARATE CONDITIONS so a failure names itself rather than printing one
		// undifferentiated FAIL. The middle one is the whole point of the probe: a plane at the
		// component origin would sit ON the pivot and measure ~0 uu away from it.
		const bool bOpacityOk = Probe->PeakOpacity >= 0.85f * TraceKnifeViewFile::StreakPeakOpacity;

		// 8 uu DOWN THE BLADE'S OWN AXIS. The knife's own blade is 12.6 uu long, so anything at or
		// past two-thirds of it is unambiguously out on the blade rather than in the fist (~0) or
		// back on the pommel (negative). It is a threshold on the signed projection precisely so that
		// the failure this probe has already caught once cannot pass it again.
		//
		// *** IT USED TO BE SIGNED AGAINST THE PLAYER'S AIM DIRECTION, AND THAT WAS A COINCIDENCE
		//     AND NOT A RULE. *** (spec v33) It agreed with the blade's axis only while the blade
		// pointed down the aim ray, and the moment the hold laid the knife across the fist the way a
		// hand actually holds one, an unmoved and correct streak measured +2.15 uu and the harness
		// went red for the pose rather than for the streak. The rule the sentence above states is
		// about where the plane sits ON THE KNIFE, so it is the knife's axis that signs it. The red
		// arm is untouched by the change: a plane hung off a bone with a 180-degree bind rotation —
		// the defect this probe caught on its first run — projects NEGATIVE on the blade's axis in
		// exactly the way it did on the aim ray.
		const bool bAtTipNotInHand = Probe->PeakForwardUU >= 8.f;
		const bool bPass = Probe->bEverVisible && bOpacityOk && bAtTipNotInHand;

		// TWO CALLS AND NOT A TERNARY VERBOSITY: UE_LOG pastes its second argument onto
		// `ELogVerbosity::`, so an expression there does not compile.
#define TRACE_STREAK_VERDICT_TEXT \
	TEXT("TRACE KNIFESTREAK VERDICT: %s - the streak was drawn on %d frame(s) of one real swing, " \
	     "peaking at %.3f opacity (doc asks %.2f) with the plane %.2f uu from the blade's pivot and " \
	     "%+.2f uu of that DOWN THE BLADE'S OWN AXIS (positive = out on the blade, ~0 = inside " \
	     "the fist, negative = on the pommel), at screen (%.0f, %.0f) of %dx%d%s. built=%d " \
	     "visible-at-least-once=%d Trace.Knife.StabStreak=%d")
#define TRACE_STREAK_VERDICT_ARGS \
	(bPass ? TEXT("PASS") : TEXT("FAIL")), Probe->Frames, Probe->PeakOpacity, \
	TraceKnifeViewFile::StreakPeakOpacity, Probe->PeakTipToPivotUU, Probe->PeakForwardUU, \
	Probe->PeakScreen.X, Probe->PeakScreen.Y, Probe->ViewportSize.X, Probe->ViewportSize.Y, \
	(Probe->bPeakOnScreen ? TEXT("") : TEXT(" [projection unavailable]")), \
	(bBuilt ? 1 : 0), (Probe->bEverVisible ? 1 : 0), \
	TraceKnifeViewFile::CVarStabStreak.GetValueOnGameThread()

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TRACE_STREAK_VERDICT_TEXT, TRACE_STREAK_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_STREAK_VERDICT_TEXT, TRACE_STREAK_VERDICT_ARGS);
		}

#undef TRACE_STREAK_VERDICT_ARGS
#undef TRACE_STREAK_VERDICT_TEXT

		TArray<FString> Lines;
		Driver->DescribeTo(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line);
		}

		GStreakProbe.Reset();
		return false;
	}

	static void StreakProbe(const TArray<FString>& Args)
	{
		TSharedPtr<FStreakProbe> Probe = MakeShared<FStreakProbe>();
		Probe->StartedReal = FPlatformTime::Seconds();
		// A settle window, so this can share a -TraceExec batch with Trace.Characters.Select: the
		// select respawns the pawn a moment later and the probe must not drive the placeholder that
		// is about to be destroyed. Same argument, same default, as Trace.Knife.PackDemo's.
		Probe->SettleSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atod(*Args[0]), 0.0, 20.0) : 2.5;
		GStreakProbe = Probe;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&StreakProbeTick), 0.f);
	}

	FAutoConsoleCommand CmdStreakProbe(
		TEXT("Trace.Knife.StreakProbe"),
		TEXT("Spec v32 s4. Equips the knife, waits on the real pullout, SWINGS, and then samples the ")
		TEXT("26x10 uu stab streak every frame it is drawn: playhead, opacity, the tip and the pivot ")
		TEXT("in world space, the distance between them and the on-screen pixel (through the ")
		TEXT("first-person morph). Prints a PASS/FAIL. RED ARM: Trace.Knife.StabStreak 0, which must ")
		TEXT("FAIL. Optional argument: seconds to settle first."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&StreakProbe));

	FAutoConsoleCommand CmdPackDemo(
		TEXT("Trace.Knife.PackDemo"),
		TEXT("Spec v31 s5. Equips the knife, waits on the REAL pullout, plays the 3.20s inspect ")
		TEXT("flourish, samples its playhead and its cyan catch peaks, then interrupts it with a real ")
		TEXT("swing and samples the 0.30s stab. Prints a PASS/FAIL. This is the evidence that the ")
		TEXT("authored clips advance and that a real action beats the cosmetic one. Optional argument: ")
		TEXT("seconds to settle before starting (default 2.5), so it can share a -TraceExec batch with ")
		TEXT("Trace.Characters.Select."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&PackDemo));

	// ---------------------------------------------------------------------------------------------
	// *** Trace.Knife.HoldProbe — spec v33. The hold, measured, in wrist_right's own frame. ***
	// ---------------------------------------------------------------------------------------------
	//
	// It EQUIPS THE KNIFE AND WAITS ON THE REAL PULLOUT before it measures, for the same reason
	// StreakProbe does: the rig is built the frame it is first needed, so a command that printed
	// immediately would print "no built pack blade" on a pawn that is about to have one. Optional
	// argument: seconds to settle first, so it can share a -TraceExec batch with a character select.
	struct FHoldProbe
	{
		double StartedReal = 0.0;
		double SettleSeconds = 0.0;
	};

	static TSharedPtr<FHoldProbe> GHoldProbe;

	static bool HoldProbeTick(float /*Delta*/)
	{
		TSharedPtr<FHoldProbe> Probe = GHoldProbe;
		if (!Probe.IsValid())
		{
			return false;
		}

		UWorld* World = PlayableWorld();
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		UTraceKnifeViewSubsystem* Driver = UTraceKnifeViewSubsystem::Get(World);
		const double Elapsed = FPlatformTime::Seconds() - Probe->StartedReal;

		if (Pawn == nullptr || Driver == nullptr)
		{
			if (Elapsed < 12.0)
			{
				return true;
			}
			UE_LOG(LogTraceGame, Warning, TEXT("[KnifeHold] no local pawn for 12s; giving up."));
			GHoldProbe.Reset();
			return false;
		}

		if (Elapsed < Probe->SettleSeconds)
		{
			return true;
		}

		TraceMelee::RequestEquipIfDifferent(Pawn, ETraceEquippedWeapon::Knife);
		if ((!TraceMelee::IsKnifeEquipped(Pawn) || TraceMelee::GetDeployRemaining(Pawn) > 0.f)
			&& Elapsed < Probe->SettleSeconds + 6.0)
		{
			return true;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Knife.HoldProbe (spec v33) ================"));
		TArray<FString> Lines;
		Driver->DescribeHoldTo(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line);
		}

		GHoldProbe.Reset();
		return false;
	}

	static void HoldProbe(const TArray<FString>& Args)
	{
		TSharedPtr<FHoldProbe> Probe = MakeShared<FHoldProbe>();
		Probe->StartedReal = FPlatformTime::Seconds();
		Probe->SettleSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atod(*Args[0]), 0.0, 20.0) : 2.5;
		GHoldProbe = Probe;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&HoldProbeTick), 0.f);
	}

	FAutoConsoleCommand CmdHoldProbe(
		TEXT("Trace.Knife.HoldProbe"),
		TEXT("Spec v33. Equips the knife, waits on the real pullout, then prints the HOLD measured in ")
		TEXT("wrist_right's own frame: every live posed bone of the gloved hand near the wrist, the ")
		TEXT("fist centroid the placement is built on, and the blade's pommel/pivot/tip in the same ")
		TEXT("frame. This is what says whether the handle is in the fingers or the knife is through ")
		TEXT("the hand. Optional argument: seconds to settle first."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HoldProbe));

	FAutoConsoleCommand CmdPackStatus(
		TEXT("Trace.Knife.PackStatus"),
		TEXT("Spec v31 s5. Prints which knife rig is on screen (the pack's SK_TraceKnife or the v27 ")
		TEXT("cube blade), which clip is playing, the live playhead and the EmissiveIntensity the ")
		TEXT("blade is being driven at. RED ARM: Trace.Knife.PackArt 0, which must report the ")
		TEXT("fallback."),
		FConsoleCommandDelegate::CreateStatic(&PackStatus));

	FAutoConsoleCommand CmdPackInspect(
		TEXT("Trace.Knife.Inspect"),
		TEXT("Spec v31 s5. Fires the F-key flourish on the local pawn without a keyboard, so a ")
		TEXT("headless run can photograph it. Same entry point the bind uses; same refusals."),
		FConsoleCommandDelegate::CreateStatic(&PackInspect));
}

#endif // !UE_BUILD_SHIPPING
