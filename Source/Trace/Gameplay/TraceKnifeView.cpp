// Copyright Trace. All Rights Reserved.

#include "Gameplay/TraceKnifeView.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Containers/Ticker.h"               // FTSTicker — Trace.Knife.PackDemo spans frames
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                     // TActorIterator
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"

#include "Core/TraceCharacter.h"
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
	 * IT IS SEPARATE FROM RestCant BELOW ON PURPOSE. This 90 is a fact about the export and must not
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
	static const FRotator RestCant(7.f, -10.f, 4.f);

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
	 */
	static constexpr float StabPeakFraction = 0.35f;

	/** Where inside Draw the balisong actually snaps open. Fraction, same reasoning as above. */
	static constexpr float DrawSnapFraction = 0.72f;

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
		ECVF_Default);

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
		ECVF_Default);

	/** The bone the pack's hands doc names for a right-handed one-hand hold. */
	static const FName WristRightBone(TEXT("wrist_right"));

	/**
	 * WHERE THE PIVOT SITS INSIDE THE FIST, when the blade is parented to wrist_right.
	 *
	 * A DELTA ONTO THE BONE, not a second set of absolute coordinates — the standing rule, and the
	 * same argument TraceKnifeLayout::OffHandOffset makes: retune the hand rig and the blade follows
	 * the fist instead of being left behind in mid-air. The pack's own landmark table puts the knife's
	 * hand attach point at the PIVOT (its origin), so the delta only has to say where the pins sit
	 * relative to the wrist joint — a few centimetres out along the fingers and a fraction forward.
	 * The hand is 19 cm from wrist to fingertip, so ~7 uu down the fingers is the middle of the grip.
	 */
	static const FVector WristOffset(7.0f, 0.f, 0.f);

	/** Per-clip-change logging. Off by default: a clip change is a per-press event, not per frame. */
	static TAutoConsoleVariable<int32> CVarPackKnifeLog(
		TEXT("Trace.Knife.PackLog"),
		0,
		TEXT("Spec v31 s5. 1 = print one line every time the blade changes clip, naming the clip and ")
		TEXT("the state that chose it. This is how a 0.30 s stab is verified without a camera."),
		ECVF_Default);

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
	// THE POSE, AND THE YAW CORRECTION IS APPLIED IN EXACTLY ONE OF THE TWO CASES.
	// =============================================================================================
	//
	// ON ViewModelRoot: correction FIRST, then the cant — quaternion multiplication, because FRotator
	// addition is not rotation composition and the two answers differ once either term is large.
	// 90 degrees is large. The +90 is needed here because ViewModelRoot is THIS PROJECT'S rig space
	// (+X out of the lens), authored against Scripts/railgun_glb_to_obj.py's axis map, and the pack
	// arrives through Interchange's, which is a yaw of 90 away.
	//
	// *** ON THE WRIST: NO ROTATION AT ALL, AND THAT IS THE WHOLE POINT OF ATTACHING TO A BONE. ***
	// The hand and the knife came out of the SAME pack through the SAME importer with the SAME axis
	// convention, so `wrist_right` is already oriented in the space the blade's own mesh is authored
	// in. The +90 exists to reconcile the pack with the PROJECT; it has no business reconciling the
	// pack with itself, and applying it here would rotate the blade a quarter turn out of the fist
	// that is holding it. The cant is dropped for the same family of reason: the hand clip is already
	// holding the blade at the angle the artist authored, and a screen-space cant on top would fight
	// it. MEASURED: with the +90 still applied the blade came out of the fist sideways, pointing down
	// and across the screen instead of along the fingers.
	const FQuat Corrected = Rig.bOnWrist
		? FQuat::Identity
		: FQuat(TraceKnifeViewFile::RestCant)
			* FQuat(FRotator(0.f, TraceKnifeViewFile::PackAimYawCorrectionDeg, 0.f));

	Blade->SetRelativeLocationAndRotation(
		Rig.bOnWrist ? TraceKnifeViewFile::WristOffset : TraceKnifeViewFile::RestLocation, Corrected);
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

	UE_LOG(LogTraceGame, Log,
		TEXT("[KnifeView] %s: pack blade built, %d material slot(s), Idle_Open looping, attached to %s."),
		*GetNameSafe(&Pawn), Rig.SlotNames.Num(),
		Rig.bOnWrist
			? *FString::Printf(TEXT("%s's wrist_right bone (the pack's stated hold)"), *GetNameSafe(AttachParent))
			: TEXT("ViewModelRoot at this file's own rest pose (no gloved-hands rig found)"));
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
		const float Length = (ClipStab != nullptr) ? ClipStab->GetPlayLength() : 0.3f;
		const float Alpha = (Length > KINDA_SMALL_NUMBER) ? FMath::Clamp(ClipSeconds / Length, 0.f, 1.f) : 0.f;
		const float T = (Alpha <= StabPeakFraction)
			? (Alpha / FMath::Max(KINDA_SMALL_NUMBER, StabPeakFraction))
			: (1.f - (Alpha - StabPeakFraction) / FMath::Max(KINDA_SMALL_NUMBER, 1.f - StabPeakFraction));
		const float Shaped = FMath::Clamp(T, 0.f, 1.f);
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
			if (TraceMelee::GetDeployRemaining(Pawn) > 0.f && Elapsed < 3.0)
			{
				return true;
			}
			for (const FString& Line : Lines) { UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line); }
			UE_LOG(LogTraceGame, Display,
				TEXT("[PackKnife] demo: pullout complete after %.3fs. Requesting the flourish."), Elapsed);
			TraceKnifeView::RequestInspect(Pawn);
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

			if (Elapsed < 1.2)
			{
				return true;
			}

			for (const FString& Line : Lines) { UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line); }

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

	FAutoConsoleCommand CmdPackDemo(
		TEXT("Trace.Knife.PackDemo"),
		TEXT("Spec v31 s5. Equips the knife, waits on the REAL pullout, plays the 3.20s inspect ")
		TEXT("flourish, samples its playhead and its cyan catch peaks, then interrupts it with a real ")
		TEXT("swing and samples the 0.30s stab. Prints a PASS/FAIL. This is the evidence that the ")
		TEXT("authored clips advance and that a real action beats the cosmetic one. Optional argument: ")
		TEXT("seconds to settle before starting (default 2.5), so it can share a -TraceExec batch with ")
		TEXT("Trace.Characters.Select."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&PackDemo));

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
