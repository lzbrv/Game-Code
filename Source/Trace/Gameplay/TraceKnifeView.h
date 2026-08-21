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
// ===================================================================================================

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

#include "TraceKnifeView.generated.h"

class ATraceCharacter;
class UAnimSequence;
class UMaterialInstanceDynamic;
class USkeletalMesh;
class USkeletalMeshComponent;

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
	};

	/** The record for @p Pawn, creating it on first sight. Null only if @p Pawn is null. */
	FKnifeRig* RecordFor(ATraceCharacter* Pawn);
	const FKnifeRig* FindRecord(const ATraceCharacter* Pawn) const;

	/** Drops records whose pawn has gone, releasing the components with them. */
	void ForgetDeadRecords();

	/** Builds the blade for @p Pawn if it is not built and the art resolves. */
	void EnsureBladeBuilt(ATraceCharacter& Pawn, FKnifeRig& Rig);

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

	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> OwnedBlades;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> OwnedMids;

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
