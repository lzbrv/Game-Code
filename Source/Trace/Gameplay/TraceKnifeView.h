// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — THE PACK BUTTERFLY KNIFE, ON SCREEN   (spec v31 §5)
// ===================================================================================================
//
//     "Implement the new knife model with its stab animation and inspect animation; bind the F key
//      to inspect"
//
// *** THE ONE SENTENCE THAT MATTERS: THIS PACK HAS ANIMATIONS AND NONE OF THEM ARE WRITTEN HERE. ***
// The last three kits shipped mesh-only and every motion in this project was arithmetic as a result —
// UTraceWeaponComponent::UpdateKnifeVisuals' three-keyframe stab, TraceKnifeLayout's cocked pose,
// ATraceCharacter::UpdateCrouchPresentation's hand-posed slide. This file writes NO motion. It plays
// four authored UAnimSequences and decides, from real gameplay state, WHICH one is playing.
//
//     A_Knife_Idle_Open   2.400 s, LOOPS.  The knife is CARRIED OPEN; this is the resting state.
//     A_Knife_Draw        0.517 s, one-shot, HOLD THE LAST FRAME. The only clip that starts SHUT.
//     A_Knife_Stab        0.300 s, one-shot. Pairs frame-for-frame with the hands' Stab_Knife.
//     A_Knife_Inspect     3.200 s, one-shot. A flourish, on F. Cosmetic; interruptible.
//
// (Lengths are the MEASURED asset lengths, not the docs'. The pack README says Draw is 0.52 s; the
// imported sequence is 0.5167 s, which is 31 frames at the 60 Hz bake it was pinned to. The measured
// file wins, and the hand clip it pairs with is the same 0.5167 s, so the pair still matches exactly.)
//
// ---------------------------------------------------------------------------------------------------
// *** WHY THIS IS A NEW FILE AND NOT A BLOCK INSIDE TraceMelee.cpp. SAY IT PLAINLY. ***
// ---------------------------------------------------------------------------------------------------
// The pass brief named Source/Trace/Gameplay/TraceMelee.{h,cpp} as where the knife's VISUALS live.
// They do not live there and never have: TraceMelee is a settings object plus a namespace of pure
// gameplay predicates, and every line of knife presentation in this project is in
// UTraceWeaponComponent::EnsureKnifeVisualsBuilt / UpdateKnifeVisuals — which is the §1 revert
// slice's file, being actively edited during this pass. Adding 700 lines to either of those two
// files while another agent held them open is how a lost update happens. So the new driver is
// self-contained, in files nobody else is touching, and it reaches the outside world through exactly
// two public surfaces:
//
//     TraceKnifeView::RequestInspect(Pawn)   — called by ATracePlayerController's F handler
//     UTraceKnifeViewSubsystem               — a tickable world subsystem that finds its own pawns
//
// Nothing in TraceMelee, TraceWeaponComponent or TraceCharacter was edited to make this work.
//
// ---------------------------------------------------------------------------------------------------
// HOW THE PROCEDURAL CUBE KNIFE GETS OUT OF THE WAY WITHOUT TOUCHING ITS OWNER'S FILE
// ---------------------------------------------------------------------------------------------------
// UTraceWeaponComponent builds a six-cube blade under a KnifeViewRoot hung off
// ATraceCharacter::ViewModelRoot, and re-asserts its visibility with SetVisibility(). Visibility and
// HIDDEN-IN-GAME are two INDEPENDENT flags on a scene component — a primitive draws only when
// IsVisible() AND !bHiddenInGame — so this file writes the flag that file does not:
// SetHiddenInGame(true) on the cube rig, and nothing anywhere fights over one boolean. Grep confirms
// UTraceWeaponComponent never calls SetHiddenInGame on any knife component.
//
// *** THE FALLBACK SURVIVES, WHICH IS A REQUIREMENT AND NOT A COURTESY. *** If SK_TraceKnife does not
// resolve — a fresh clone with no `git lfs pull`, or -TraceNoCharacterArt — this file builds nothing,
// hides nothing, and the cube knife is exactly what it was. That is checked by
// Trace.Knife.PackStatus, which names which of the two rigs is on screen and why.
//
// ---------------------------------------------------------------------------------------------------
// *** BEWARE PER-FRAME READERS OF FAST QUANTITIES — THE SPEC'S WARNING APPLIES HARDEST HERE. ***
// ---------------------------------------------------------------------------------------------------
// A_Knife_Stab is 0.300 s: EIGHTEEN FRAMES at 60 fps. Two bugs this month were per-frame readers of
// exactly this shape. Three rules are enforced in the implementation and each one is a bug that would
// otherwise be shipped:
//
//   1. PlayAnimation IS CALLED ON A STATE EDGE, NEVER PER FRAME. UAnimSingleNodeInstance restarts
//      from t=0 on every PlayAnimation call, so a per-frame call is a clip that plays frame 0 sixty
//      times a second and never advances. The state machine below computes a DESIRED CLIP and only
//      touches the component when the desired clip differs from the one already playing.
//   2. SAMPLE BEFORE YOU ADVANCE. The playhead is read ONCE at the top of the tick, into a local, and
//      everything downstream (the emissive curve, the "has the flourish finished" test) uses that
//      local. Reading it again after a PlayAnimation would read the new clip's zero.
//   3. NOTHING IS DRIVEN BY A FREE-RUNNING ACCUMULATOR. "Is a stab in flight" is an edge on
//      TraceMelee::GetSwingCooldownRemaining, which is authored gameplay state; "is the flourish
//      over" is the animation's own playhead against the sequence's own length. Neither can drift
//      from what is on screen, and a hitch cannot desync the emissive flare from the flip it lights.
//
// ---------------------------------------------------------------------------------------------------
// SPEC v32 §4 — THE STAB STREAK, AND THE ONE THING IT ADDED TO ALL OF THE ABOVE
// ---------------------------------------------------------------------------------------------------
// The FX doc's last line about this knife is a piece of GEOMETRY, not emissive: "add a short streak
// plane (0.26 x 0.10 m) at the blade tip for the thrust, ~0.9 opacity at peak." Built out of
// UTraceFxShapes' engine primitives, hung off the blade, and visible only across A_Knife_Stab.
//
// TWO THINGS ABOUT IT ARE WORTH KNOWING BEFORE READING THE CODE:
//
//   * ITS OPACITY IS THE FLASH'S OWN NUMBER. StabFlareAt() is one triangular curve about
//     StabPeakFraction, and it has exactly two callers: the cyan 4.4x emissive peak and the streak.
//     §4 asks for them to be unable to disagree, and one function is the only version of that which
//     survives a later edit.
//
//   * *** THE TIP IS DERIVED AND WAS WRONG ONCE. *** SK_TraceKnife ships NO named socket — §7f
//     records that as a pack limitation needing a re-export — so the point comes from the mesh's own
//     bounds along its long axis, and it is LOGGED on resolution rather than trusted. The first
//     version hung it off "the bone nearest the tip", which on a balisong is a HANDLE PIVOT carrying
//     a 180-degree bind rotation, and Trace.Knife.StreakProbe caught the streak being drawn off the
//     POMMEL on the first real swing. It rides the ROOT now, which is what the blade is rigid to.
//     That probe is why the failure was a log line instead of a screenshot nobody took.
// ===================================================================================================

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

// [SPEC v32 §4] ETraceFxBlend is stored per rig, by value, so it has to be a complete type here. It
// is the blend MakeGlowMID ACTUALLY achieved, which is not necessarily the one that was asked for —
// see TraceFxShapes.h, which is emphatic that SetGlow must be handed the achieved value.
#include "Gameplay/TraceFxShapes.h"

#include "TraceKnifeView.generated.h"

class ATraceCharacter;
class UAnimSequence;
class UMaterialInstanceDynamic;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMeshComponent;

/**
 * Which authored clip the blade should be playing. Order is NOT priority — see ChooseClip.
 *
 * `None` means "the rig is not built or not on screen". It is never a state the blade rests in while
 * visible: *** THE KNIFE'S REFERENCE POSE IS SHUT. *** Measured on the imported asset,
 * handle_pivot_safe and handle_pivot_bite each carry a 180-degree bind rotation and the mesh measures
 * 16.2 uu, which is the CLOSED length; every clip except Draw begins and ends at identity, which is
 * OPEN. So a SkeletalMeshComponent with nothing playing draws a FOLDED balisong, and the pack README
 * is explicit that the knife is carried open. Something must always be playing.
 */
UENUM()
enum class ETraceKnifeClip : uint8
{
	None = 0,
	Idle_Open,
	Draw,
	Stab,
	Inspect
};

/** LexToString for the clip enum, for Trace.Knife.PackStatus and the debug log. */
TRACE_API const TCHAR* LexTraceKnifeClip(ETraceKnifeClip Clip);

namespace TraceKnifeView
{
	/**
	 * WHERE A_Knife_Stab PEAKS, as a FRACTION of the clip rather than as a time, so it tracks the
	 * sequence if the clip is ever re-exported at a different length.
	 *
	 * *** PUBLISHED HERE BECAUSE A SECOND FILE HAS TO PEAK ON THE SAME FRAME. *** The blade's cyan
	 * 4.4x flare, this file's §4 stab streak and — since §5 — the GLOVES' 2.7x/2.1x emissive all
	 * shape their curve about this fraction. The gloves live in TraceCharacter.cpp, which could not
	 * reach the copy in TraceKnifeView.cpp's file-local namespace and therefore carried its own
	 * `HandsActionPeakFraction = 0.35f` with a comment admitting the duplication. Both the §4 and §5
	 * agents flagged that in their reports as the fix worth making, and this is it: one number, one
	 * definition, so a re-tune of the blade cannot silently leave the glove peaking two frames late.
	 *
	 * The reasoning behind 0.35 belongs with the clip and stays in TraceKnifeView.cpp beside
	 * StabFlare(): the stats file describes Stab as "130 mm thrust ... snapping back", so the thrust
	 * is the front third and the snap-back is the rest.
	 */
	inline constexpr float StabPeakFraction = 0.35f;

	/**
	 * *** SPEC v31 §5 — THE F KEY. "Inspect is COSMETIC." ***
	 *
	 * Start the 3.20 s flourish on @p Pawn, if it is legal right now. Returns true if it started.
	 *
	 * COSMETIC MEANS IT CONFERS NOTHING AND BLOCKS NOTHING, and that is enforced by what this
	 * function does NOT do rather than by a comment: it writes one timestamp on a presentation-only
	 * record inside a world subsystem, it never touches the weapon component, the movement profile,
	 * the melee cooldown or any replicated state, and it is never called on a machine that is not
	 * rendering the pawn. Nothing anywhere reads "is inspecting" as a gameplay condition — grep for
	 * the record type and there is exactly one reader, the clip chooser.
	 *
	 * *** AND A REAL ACTION INTERRUPTS IT, which is the other half of the sentence. *** The chooser
	 * ranks Stab and Draw ABOVE Inspect, unconditionally, so a swing or a weapon switch takes the
	 * blade back on the same frame it is requested. The flourish is not asked for permission.
	 *
	 * REFUSED (returns false, quietly) when: the pawn is dead, is the Core carrier (both hands are
	 * on the objective — see the pack's own loadout table, the Core is a two-hand cradle), has no
	 * knife equipped, is mid-pullout, is mid-swing, or is already inspecting. None of those is an
	 * error; a cosmetic key that argues with you is worse than one that does nothing.
	 */
	TRACE_API bool RequestInspect(ATraceCharacter* Pawn);

	/** True while @p Pawn's flourish is running. PRESENTATION ONLY — no gameplay may read this. */
	TRACE_API bool IsInspecting(const ATraceCharacter* Pawn);

	/**
	 * How many FIRST-PERSON blade parts @p Pawn is currently DRAWING out of this file's pack rig:
	 * 1 when SK_TraceKnife is built and visible, 0 otherwise (including the whole fallback path).
	 *
	 * *** THIS EXISTS SO ONE CENSUS CAN STILL SEE THE KNIFE. *** Spec v12 §7's rule — a gun part and
	 * a knife part are never drawn together — is guarded by
	 * UTraceWeaponComponent::GetViewModelCensus, which counts the v27 cube blade in KnifeViewParts
	 * and the gun's static meshes under ViewModelRoot. The pack blade is NEITHER: it is a
	 * USkeletalMeshComponent hanging off the hands mesh's `wrist_right` BONE, so it appears in no
	 * list the census walks. The moment §5 hid the cube blade and put this one on screen instead,
	 * that census started reporting `1P knife=0` for a pawn holding a visible knife — a reading that
	 * is not merely incomplete but false, and Trace.Knife.DualWeaponTest's overlap arithmetic is
	 * min(gun, knife), so a permanent zero makes the whole rule untestable.
	 *
	 * Returning a COUNT rather than a bool because that is what the census adds it to, and because a
	 * future rig with a sheath or a second blade fits without changing the caller.
	 */
	TRACE_API int32 VisibleBladeParts(const ATraceCharacter* Pawn);
}

/**
 * The per-world driver. A TICKABLE WORLD SUBSYSTEM and not a component, for the reason
 * UTraceAudioWatchSubsystem gives for the same choice: it owns nothing that replicates, it must not
 * appear in a level, it has to die with the world, and — the deciding reason here — it needs to add
 * components to a pawn owned by two other agents' files without either of them declaring a member for
 * it.
 *
 * IT TICKS ON RENDERING MACHINES, NOT ON THE AUTHORITY, which is the opposite of the audio watcher
 * and is correct for a viewmodel: a dedicated server has no viewmodel and a client has to draw its
 * own blade without waiting for a round trip. Everything here is presentation; nothing is replicated;
 * nothing is authoritative.
 */
UCLASS()
class TRACE_API UTraceKnifeViewSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	//~ End USubsystem

	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaSeconds) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;
	virtual bool IsTickableInEditor() const override { return false; }
	//~ End FTickableGameObject

	/** The driver for @p WorldContext's world, or null. Null is legal everywhere. */
	static UTraceKnifeViewSubsystem* Get(const UObject* WorldContext);

	/** See TraceKnifeView::RequestInspect. */
	bool RequestInspect(ATraceCharacter* Pawn);

	/** See TraceKnifeView::IsInspecting. */
	bool IsInspecting(const ATraceCharacter* Pawn) const;

	/** Trace.Knife.PackStatus's whole output. Dev-only reader; nothing in the game calls it. */
	void DescribeTo(TArray<FString>& OutLines) const;

	/**
	 * [SPEC v33] Trace.Knife.HoldProbe's whole output: THE HOLD, MEASURED IN wrist_right's OWN FRAME.
	 *
	 * *** THIS EXISTS BECAUSE "THE KNIFE CLIPS THROUGH THE HAND" IS A GEOMETRY CLAIM AND NOTHING IN
	 * THIS FILE COULD MEASURE IT. *** PackStatus reports which rig is on screen and StreakProbe
	 * reports where the POINT is; neither can say where the HANDLE is relative to the fingers, which
	 * is the only question a "held naturally" verdict turns on. This prints, all in the one frame
	 * the blade is actually attached in:
	 *
	 *   * every live posed bone of the gloved hand within reach of the wrist, in wrist-local uu, so
	 *     the knuckle line and the thumb are positions rather than assumptions;
	 *   * the knuckle line's own direction, which IS the axis a fist's grip runs along;
	 *   * the blade's landmarks — pommel, pivot, tip — in the SAME frame, so "the handle is inside
	 *     the fingers" and "the pommel is out through the back of the wrist" are numbers.
	 *
	 * Dev-only reader; nothing in the game calls it.
	 */
	void DescribeHoldTo(TArray<FString>& OutLines) const;

	/**
	 * What @p Pawn's blade is doing RIGHT NOW: the clip, the playhead in seconds, and the two
	 * EmissiveIntensity values being written to the cyan and amber slots.
	 *
	 * Exists for Trace.Knife.PackDemo and for nothing else. It reads the SAME playhead and calls the
	 * SAME ComputeEmissive the renderer is being driven by, rather than letting the harness re-derive
	 * either — a check that re-derives the value it is checking cannot fail, which is the argument
	 * Trace.Audio.Loudness makes about asking UTraceAudioSubsystem::VolumeFor instead of
	 * recomputing master x scale.
	 */
	void SampleForHarness(const ATraceCharacter* Pawn, ETraceKnifeClip& OutClip, float& OutSeconds,
		float& OutCyan, float& OutAmber) const;

	/**
	 * [SPEC v32 §4] The same service for the STREAK: is it built, is it being drawn this frame, at
	 * what opacity, and WHERE — the derived tip and the blade's own pivot, both in world space.
	 *
	 * The two positions are the point. "The plane is at the tip" is not a claim a log line can make
	 * on its own; the DISTANCE between the tip it drew at and the pivot inside the fist is a
	 * measurement, and it has to come out at the blade's own length rather than at zero.
	 *
	 * *** OutBladeAimWorld IS WHAT MAKES THAT DISTANCE SIGNED, AND IT IS THE BLADE'S OWN AXIS. ***
	 * (spec v33) The harness used to sign it against the PLAYER'S AIM DIRECTION, which was only ever
	 * a stand-in: it worked while the blade happened to point down the aim ray, and it went red the
	 * moment the hold put the knife across the fist the way a hand actually holds one. The rule the
	 * probe states — "out on the blade, not in the fist and not on the pommel" — is about the KNIFE,
	 * so this is the knife's own long axis (mesh -Y, out of the blade) in world space, and the
	 * defect it must still catch is the one it caught before: a streak hung off a bone carrying a
	 * 180-degree bind rotation projects NEGATIVE on this axis, on the pommel, exactly as it did on
	 * the aim ray.
	 */
	void SampleStreakForHarness(const ATraceCharacter* Pawn, bool& bOutBuilt, bool& bOutVisible,
		float& OutOpacity, FVector& OutTipWorld, FVector& OutPivotWorld, FName& OutTipBone,
		FVector& OutBladeAimWorld) const;

	/** 1 when this pawn's pack blade is built and VISIBLE, 0 otherwise. See TraceKnifeView::VisibleBladeParts. */
	int32 VisibleBladeParts(const ATraceCharacter* Pawn) const;

private:
	/** Everything one pawn's blade needs between frames. Presentation only, every field. */
	struct FKnifeRig
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;

		UPROPERTY()
		TObjectPtr<USkeletalMeshComponent> Blade = nullptr;

		/** One MID per material slot, in slot order. Null entries are slots with no material. */
		TArray<TObjectPtr<UMaterialInstanceDynamic>> SlotMids;

		/** Slot names as the mesh reports them: shell, circuit_cyan, plating, core_amber, carbon. */
		TArray<FName> SlotNames;

		/** What is playing right now. The PlayAnimation edge is (Desired != Playing). */
		ETraceKnifeClip Playing = ETraceKnifeClip::None;

		/**
		 * The knife-equipped flag as of LAST tick. INDEX_NONE-equivalent: bSeen guards the first look.
		 * The Draw edge is (equipped now && !equipped last), which is real state and not a timer.
		 */
		bool bKnifeLast = false;
		bool bSeen = false;

		/**
		 * TraceMelee::GetSwingCooldownRemaining as of last tick. It only ever counts DOWN, so a RISE
		 * is a swing that has just started — an edge on authored gameplay state, which is what the
		 * spec's "drive from real state" means. A 0.30 s clip cannot be caught by polling a boolean.
		 */
		float SwingCooldownLast = 0.f;

		/** World seconds the flourish was requested, or a large negative for "not inspecting". */
		double InspectStartedWorldSeconds = -1.0e9;

		/** The cube rig this pawn is hiding, so the hide is asserted once and undone on teardown. */
		TWeakObjectPtr<USceneComponent> HiddenCubeRoot;

		/** Set once when the pack art did not resolve for this pawn, so the log line is printed once. */
		bool bArtUnavailable = false;

		/**
		 * True when the blade found spec v31 §6's gloved hands and parented to their `wrist_right`
		 * bone; false when it is hanging off ViewModelRoot at this file's own rest pose. Reported by
		 * Trace.Knife.PackStatus, because "which of the two placements am I looking at" is the first
		 * question anybody asks of a screenshot where the blade is in the wrong place.
		 */
		bool bOnWrist = false;

		/**
		 * [SPEC v33] *** `wrist_right`'s BASE-POSE ATTITUDE IN RIG SPACE, INVERTED, CAPTURED ONCE. ***
		 *
		 * The hold is built in the BONE's frame, out of the hand's own anatomy, so this is no longer
		 * how the attitude is authored — it is how the LENS is found. The one thing the hold needs
		 * from outside the hand is which way the camera is, so that the blade can be rolled to present
		 * its flat rather than its edge; the lens looks down rig -X, and this quaternion is what
		 * carries that direction into the bone's frame.
		 *
		 * IT MUST BE THE BASE POSE AND NOT THIS FRAME'S. Taken live it would re-roll the blade every
		 * frame to keep facing the eye, i.e. the knife would refuse to turn with the hand holding it,
		 * and Draw's wrist flip and Inspect's four catch beats would spin it inside the fingers.
		 *
		 * DERIVED, NEVER TYPED: base = live x delta^-1, where the delta is
		 * ATraceCharacter::GetViewModelWeaponDelta() — which is defined as base^-1 x live. Both halves
		 * are read in the SAME frame, so this comes out at the base pose whatever pose the rig happens
		 * to be seeded into on the frame the blade is built. That is the whole reason it is done this
		 * way round rather than by sampling a wrist and hoping it is at rest: the previous pass paid
		 * for a rest transform captured from the wrong pose.
		 */
		FQuat BaseWristRotRigInv = FQuat::Identity;

		/**
		 * [SPEC v33] *** THE HAND'S OWN GRIP BASIS, IN THE WRIST BONE'S FRAME, CAPTURED ONCE. ***
		 *
		 * Its axes are the three directions a grip is described in, and every one of them is MEASURED
		 * off live posed bones rather than typed (see TraceKnifeViewFile::IndexKnuckleRightBone and
		 * its neighbours for which bones and why):
		 *
		 *     X  index_right_0 -> pinky_right_0   the knuckle line: the tube a fist closes around.
		 *     Y  wrist_right   -> knuckle_bar_right, orthogonalised: the length of the hand.
		 *     Z  X x Y, which on this rig comes out PALM-WARD and is checked against a fingertip.
		 *
		 * CAPTURED ONCE, on the same frame as BaseWristRotRigInv and for a stronger version of the
		 * same reason: Inspect_Knife's catch beats open the fingers, so a basis re-measured per frame
		 * would swim through the flourish and slide the blade out of the hand it is rigid with.
		 */
		FQuat GripBasisBone = FQuat::Identity;

		bool bBaseWristCaptured = false;

		/**
		 * [SPEC v34] *** THE SAFE HANDLE'S REFERENCE-POSE TRANSFORM, INVERTED, CAPTURED ONCE. ***
		 *
		 * This is what lets the fist hold a handle that MOVES. The blade component plays its own
		 * clips (Idle_Open, Draw, Inspect, Stab) and the reference pose is the SHUT balisong, so the
		 * two handles are not where the mesh's rest bounds say they are on any frame that is actually
		 * drawn — they are wherever the running clip has swung them. Placing the component by its
		 * ORIGIN (the pivot) therefore pins the BLADE in the hand and lets the handles swing away
		 * from it, which is the photographed "knife beside the fist" and the air gap on inspect-late.
		 *
		 * `live x this` is the safe handle's own skinning delta, so `(live x this) x P` carries a
		 * point P authored in the SHUT pose to wherever the clip has put it this frame. The grip
		 * point is P = (0, -HoldGrip x 12.2, 0): shut, the handles fold FORWARD OVER THE BLADE (mesh
		 * -Y — the whole asset measures -15.00 to +1.20 on that axis), so a point that far down -Y
		 * from the pivot is on the handle in the one pose the mesh can be measured in. Open, the same
		 * point comes back out at roughly (0, +HoldGrip x 12.2, 0), which is where the old constant
		 * used to be typed.
		 *
		 * NAME_None / Identity means the capture has not happened or the asset has no such bone; the
		 * hold then falls back to the old constant offset, which is a worse pose and not a broken one.
		 */
		FTransform GripHandleRefInv = FTransform::Identity;

		/** Which bone GripHandleRefInv was captured from — `handle_safe`, or its pivot as a fallback. */
		FName GripHandleBone = NAME_None;

		bool bGripHandleCaptured = false;

		// =========================================================================================
		// [SPEC v32 §4] THE STAB STREAK — everything it needs between frames.
		// =========================================================================================

		/** The 26 x 10 uu plane. Null until EnsureStreakBuilt succeeds; null forever if it cannot. */
		UPROPERTY()
		TObjectPtr<UStaticMeshComponent> StabStreak = nullptr;

		UPROPERTY()
		TObjectPtr<UMaterialInstanceDynamic> StreakMid = nullptr;

		/**
		 * The blend UTraceFxShapes::MakeGlowMID ACTUALLY achieved, which is what SetGlow has to be
		 * given. Asking for Translucent and then writing the Translucent parameters would be a silent
		 * no-op: this project has no translucent parent material and the request lands on Additive.
		 */
		ETraceFxBlend StreakBlend = ETraceFxBlend::None;

		/** Set once, either way, so neither the derivation nor its log line ever runs twice. */
		bool bTipResolved = false;

		/** Set when the plane could not be built at all. Latches the ONE log line; no per-frame warning. */
		bool bStreakUnavailable = false;

		/**
		 * The bone the tip rides. SK_TraceKnife SHIPS NO NAMED SOCKETS (spec v32 §7f records it as a
		 * pack limitation needing a re-export), so this is resolved by measurement: the bone whose
		 * reference-pose position is nearest the bounds-derived tip. NAME_None means the derivation
		 * found no skeleton and the component-space offset below is used instead.
		 */
		FName TipBone = NAME_None;

		/** The tip in the MESH's own space, derived from its bounds. The number the log line prints. */
		FVector TipOffsetMeshLocal = FVector::ZeroVector;

		/** The same point expressed in TipBone's space, so it survives the bone being animated. */
		FVector TipOffsetInBone = FVector::ZeroVector;

		/** How far TipBone's rest position sits from the derived tip. Reported; it is the fit quality. */
		float TipBoneResidualUU = 0.f;

		/** Last opacity written to the streak, and where the tip was. HARNESS ONLY — nothing reads these. */
		float StreakOpacityLast = 0.f;
		FVector StreakTipWorldLast = FVector::ZeroVector;
	};

	/** The record for @p Pawn, creating it on first sight. Null only if @p Pawn is null. */
	FKnifeRig* RecordFor(ATraceCharacter* Pawn);
	const FKnifeRig* FindRecord(const ATraceCharacter* Pawn) const;

	/** Drops records whose pawn has gone, releasing the components with them. */
	void ForgetDeadRecords();

	/** Builds the blade for @p Pawn if it is not built and the art resolves. */
	void EnsureBladeBuilt(ATraceCharacter& Pawn, FKnifeRig& Rig);

	/**
	 * [SPEC v33] THE HOLD: where the blade sits in `wrist_right`'s own frame so that a fist closes on
	 * the HANDLE and the blade leaves the hand at the authored rig-space attitude.
	 *
	 * Returns false — leaving both outputs untouched — whenever the pack hands cannot answer, which
	 * is the same contract GetViewModelGripWristLocal() has and the same reason: the caller's own
	 * fallback pose is then the correct answer and must not be overwritten.
	 */
	bool ComputeWristHold(const ATraceCharacter& Pawn, FKnifeRig& Rig, FVector& OutBoneLocation,
		FQuat& OutBoneRotation) const;

	/** Writes the hold onto the blade. Cheap enough to run every frame, which is what makes the
	  * three Trace.Knife.Hold* CVars live and what lets a late-arriving hands rig be picked up. */
	void ApplyWristHold(const ATraceCharacter& Pawn, FKnifeRig& Rig);

	/** Hides the six-cube procedural blade, once, via bHiddenInGame. See the file header. */
	void SuppressCubeKnife(ATraceCharacter& Pawn, FKnifeRig& Rig);

	/** One pawn's blade, one frame. */
	void TickRig(ATraceCharacter& Pawn, FKnifeRig& Rig, float DeltaSeconds);

	/**
	 * The state machine: what SHOULD be playing, from real gameplay state.
	 *
	 * @param PlayheadSeconds  where the CURRENTLY PLAYING clip is, sampled by the caller BEFORE
	 *                         anything could have restarted it. It is passed in rather than read
	 *                         here for the reason the tick's own comment gives at length: read it a
	 *                         second time, after a PlayAnimation, and it is the new clip's zero.
	 *                         A one-shot's end is "the playhead reached the sequence's length", and
	 *                         that is the ONLY thing that ends Draw or Stab — not a pullout timer,
	 *                         not a cooldown, and not a duration typed in twice.
	 */
	ETraceKnifeClip ChooseClip(const ATraceCharacter& Pawn, FKnifeRig& Rig, double NowSeconds,
		float PlayheadSeconds) const;

	/** True when a one-shot has reached (or is holding) its last frame. Tolerant of a frame's slop. */
	static bool HasFinished(const UAnimSequence* Sequence, float PlayheadSeconds);

	/** EmissiveIntensity for the cyan and amber slots at @p ClipSeconds into @p Clip. */
	void ComputeEmissive(ETraceKnifeClip Clip, float ClipSeconds, float& OutCyan, float& OutAmber) const;

	/**
	 * *** THE ONE NUMBER THE STAB'S FLASH AND THE STAB'S STREAK BOTH READ. 0 at the ends, 1 at the
	 * peak. ***
	 *
	 * SPEC v32 §4 asks for the streak's opacity to come off "THE SAME NUMBER the emissive peak
	 * already uses — so the flash and the streak cannot disagree", and the only way to make that a
	 * property of the code rather than a promise in a comment is for there to be exactly one
	 * function. ComputeEmissive's Stab branch calls this; UpdateStabStreak calls this; there is no
	 * second copy of the triangle to drift.
	 *
	 * @param ClipSeconds  the playhead SAMPLED at the top of the tick, never re-read afterwards.
	 *                     A_Knife_Stab is 0.300 s — eighteen frames — and this file's header names
	 *                     re-reading it after a PlayAnimation as a bug it has already shipped twice.
	 */
	float StabFlareAt(float ClipSeconds) const;

	/** Builds the streak plane once. Silent and once-latched when anything it needs is missing. */
	void EnsureStreakBuilt(ATraceCharacter& Pawn, FKnifeRig& Rig);

	/**
	 * Derives the blade tip from SK_TraceKnife's OWN BOUNDS along its long axis and logs the answer
	 * exactly once, so it can be checked rather than trusted. Returns false if there is no mesh.
	 */
	bool ResolveBladeTipOnce(FKnifeRig& Rig);

	/** The live world position of the derived tip, following the animated bone. False if unresolvable. */
	bool GetBladeTipWorld(const FKnifeRig& Rig, FVector& OutTipWorld) const;

	/** One rig's streak, one frame. Reads the SAMPLED clip and playhead, never the desired one. */
	void UpdateStabStreak(FKnifeRig& Rig, ETraceKnifeClip SampledClip, float SampledClipSeconds,
		bool bBladeVisible);

	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> OwnedBlades;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> OwnedMids;

	/**
	 * [SPEC v32 §4] The streak planes, held here for the same reason the blades are: FKnifeRig is a
	 * plain struct and NOT a USTRUCT, so the TObjectPtrs inside it are invisible to the garbage
	 * collector. These arrays are the only thing keeping any of it alive.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> OwnedStreaks;

	TArray<FKnifeRig> Rigs;

	/** Resolved once per world, cached including the nulls — a missing asset must not cost a lookup. */
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMesh> PackMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> ClipIdle = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> ClipDraw = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> ClipStab = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> ClipInspect = nullptr;

	bool bAssetsResolved = false;

	/** Resolves the mesh and the four clips. Idempotent; logs its verdict exactly once per world. */
	void ResolveAssets();

	/** The sequence for a clip id, or null. */
	UAnimSequence* SequenceFor(ETraceKnifeClip Clip) const;
};
