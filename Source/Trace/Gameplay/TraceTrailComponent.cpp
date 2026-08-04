#include "Gameplay/TraceTrailComponent.h"

#include "Net/UnrealNetwork.h"

#include "Camera/PlayerCameraManager.h"        // local camera location (proximity glow fade)
#include "Components/CapsuleComponent.h"
#include "Components/PoseableMeshComponent.h"  // spec v4 §2 — the character-shaped after-images
#include "Components/SkeletalMeshComponent.h"  // the live Mannequin the ghosts copy their pose from
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"                     // GEngine->GetFirstLocalPlayerController()
#include "Engine/EngineBaseTypes.h"
#include "Engine/SkinnedAsset.h"               // ghost pool identity (which Mannequin is skinned in)
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                       // TActorIterator (fallback character gather)
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"       // GetServerWorldTimeSeconds()
#include "GameFramework/Pawn.h"                 // Trace.Trail.DebugLookBack
#include "GameFramework/PlayerController.h"    // IsLocalPlayerController() (own-trace near hide)
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"                // TNumericLimits (trip-test broad phase)
#include "Math/UnrealMathUtility.h"            // FMath::SegmentDistToSegmentSafe
#include "UObject/ConstructorHelpers.h"

#include "Containers/Ticker.h"                 // FTSTicker (Trace.Trail.DebugLookBack)
#include "HAL/IConsoleManager.h"               // FAutoConsoleVariableRef (trace timing knobs)

#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Gameplay/TraceCore.h"                // IsTraceInvulnerableFor (spec §4)
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceParry.h"               // v3 §3 — the second invulnerability source
#include "Movement/TraceCharacterMovementComponent.h"   // GetLastDashActiveWorldTime()
#include "Trace.h"
#include "TraceSettings.h"

namespace
{
	/**
	 * SPEC v3 §1: the trace lasts TWO seconds, and the turnover grace is 0.4s.
	 *
	 * BOTH VALUES NOW LIVE IN UTraceSettings (TrailLifetime, CoreTurnoverGraceSeconds). These used to
	 * be ceilings applied over the settings, because TraceSettings.h belonged to another ownership
	 * slice mid-pass and a stale 2.8 could otherwise have reinstated the old rule. The settings have
	 * landed at 2.0 / 0.4, so the min() was an identity and is gone: there is exactly one number for
	 * each, it is in Project Settings, and it is live-editable in PIE.
	 *
	 * What survives is an OVERRIDE apiece, negative by default, for console experiments during a
	 * headless run where there is no settings panel to open. Same pattern, and same reasoning, as the
	 * parry cvars in Gameplay/TraceParry.cpp.
	 *
	 * KNOCK-ON, still true and still load-bearing: UTraceSettings::BotTrailMinPointLifeRemaining is
	 * "the oldest ~20% of the trace, written as an absolute" and is calibrated against the lifetime.
	 * At 2.8 it was 0.56; at 2.0 it is 0.40. Move one without the other and the interceptor bots
	 * discard 28% of the trace instead of 20% — the exact mistake, in the exact direction, that cut
	 * trail kills from 37.5% to 25.9% the last time the lifetime moved on its own.
	 */
	float GTraceLifetimeOverride = -1.f;
	float GTurnoverGraceOverride = -1.f;

	FAutoConsoleVariableRef CVarTraceLifetimeCeiling(
		TEXT("Trace.Trail.LifetimeCeiling"),
		GTraceLifetimeOverride,
		TEXT("OVERRIDE for seconds a trace point survives (spec v3 1: 2.00). "
		     "Negative (default) = use UTraceSettings::TrailLifetime."),
		ECVF_Default);

	FAutoConsoleVariableRef CVarTurnoverGrace(
		TEXT("Trace.Trail.TurnoverGrace"),
		GTurnoverGraceOverride,
		TEXT("OVERRIDE for seconds after the Core changes team before the new holder's trace BEGINS "
		     "FORMING (spec v3 1: 0.40). Negative (default) = use UTraceSettings::CoreTurnoverGraceSeconds. "
		     "Delays formation only; already-laid segments stay lethal."),
		ECVF_Default);

	/** Upper bound on pooled SMEAR elements (one per lethal segment). 2.0s at 60uu spacing is ~27. */
	constexpr int32 MaxPooledSmearElements = 96;

	/** Meshes per smear element, interleaved: [0] body (legs+torso), [1] the hot head band. */
	constexpr int32 PartsPerSmear = 2;
	constexpr int32 PartHead = 1;

	/**
	 * Upper bound on pooled POSED MANNEQUIN GHOSTS. This is the number that decides whether spec v4 §2
	 * is affordable, so it is worth being explicit about the arithmetic:
	 *
	 *   trace length  = TrailLifetime x speed, capped by MaxTrailPoints x TrailPointSpacing
	 *                 = 2.0s x 800uu/s = 1600uu at a walk, ~3200uu through a sustained dash
	 *   ghosts        = length / GhostSpacing = 1600/220 = 8 at a walk, ~15 dashing
	 *
	 * 20 covers the dashing case with headroom. Beyond it the OLDEST ghosts are released — never the
	 * newest, which are the ones an approaching enemy is judging — and the smear still covers the tail,
	 * so the cap degrades the look and never the continuity.
	 *
	 * ONLY THE CORE HOLDER EMITS. In the worst realistic case (a turnover leaving a residual trace
	 * while the new holder lays a fresh one) two traces coexist, so the ceiling on posed mannequins in
	 * the world is ~40, not 20-per-player. Ten simultaneous traces is not a state this game has.
	 */
	int32 GMaxPoseGhosts = 20;

	FAutoConsoleVariableRef CVarMaxPoseGhosts(
		TEXT("Trace.Trail.GhostMaxCount"),
		GMaxPoseGhosts,
		TEXT("Cap on posed-Mannequin after-images per trace (spec v4 2). 0 disables them and leaves "
		     "the trace as the continuous smear alone. Cosmetic - never changes the lethal volume."),
		ECVF_Default);

	/**
	 * uu along the path between consecutive posed after-images.
	 *
	 * The tension it resolves: a ghost per trail point (60uu) is the most beautiful version and costs
	 * ~27 skinned draws per trace; a ghost every 400uu is nearly free and reads as a row of statues
	 * with holes between them. 220uu is a little over one body-depth of gap, so consecutive
	 * mannequins nearly touch and the eye joins them up — and the smear covers the gap regardless.
	 */
	float GGhostSpacing = 220.f;

	FAutoConsoleVariableRef CVarGhostSpacing(
		TEXT("Trace.Trail.GhostSpacing"),
		GGhostSpacing,
		TEXT("uu between posed-Mannequin after-images along the trace (spec v4 2). Lower = denser and "
		     "prettier and more skinned draws. Cosmetic only."),
		ECVF_Default);

	/**
	 * Forced LOD for the ghosts. 0 = automatic (screen-size driven), 1 = LOD0, 2 = LOD1, ...
	 *
	 * Left on automatic because the ghosts are exactly the case the LOD system is good at: they are
	 * static, they are frequently far away, and they have no animation whose popping could give the
	 * transition away. Exposed because forcing LOD2 is the single biggest lever available if a
	 * ten-player capture ever measures these as expensive.
	 */
	int32 GGhostForcedLOD = 0;

	FAutoConsoleVariableRef CVarGhostForcedLOD(
		TEXT("Trace.Trail.GhostForcedLOD"),
		GGhostForcedLOD,
		TEXT("Forced LOD on posed-Mannequin after-images. 0 = automatic (default), 1 = LOD0, 2 = LOD1. "
		     "Perf lever only."),
		ECVF_Default);

	/**
	 * How much of the freshest trace is hidden from the holder's own camera, measured along the path.
	 *
	 * Must stay comfortably longer than ATraceCharacter's third-person arm (450 uu), because the whole
	 * point is that the camera and its near field sit in the gap. 850 leaves 400 uu of clearance in
	 * front of the lens. Nobody else's view is affected — see the SetOwnerNoSee block in
	 * RebuildVisuals() for why this is presentation-only and cannot touch the lethal volume.
	 *
	 * IT IS ALSO THE CEILING ON HOW MUCH OF THEIR OWN RED TRACE A PARRYING CARRIER CAN SEE (v3 §3).
	 * At a 2.0s lifetime and ~600uu/s that leaves the carrier roughly the oldest third of their own
	 * trace to read the parry off, which is enough on a curving path and thin on a straight one — so
	 * the carrier's PRIMARY parry tell has to be a HUD element, not this. Lowering it back toward the
	 * 450uu camera arm re-enters the near field that produced the measured point-blank whiteout, so
	 * it is exposed as a live knob to be playtested rather than quietly retuned here.
	 */
	float GOwnerNearHideDistance = 850.f;

	FAutoConsoleVariableRef CVarOwnerNearHideDistance(
		TEXT("Trace.Trail.OwnerNearHideDistance"),
		GOwnerNearHideDistance,
		TEXT("uu of the freshest trace hidden from its OWN holder's camera (anti-whiteout). Presentation "
		     "only - never changes the lethal volume. Lower to let a parrying carrier see more of their "
		     "own red trace; below ~600 it re-enters the third-person near field."),
		ECVF_Default);

	/**
	 * Camera-proximity emissive fade — the anti-whiteout guard. See ApplyProximityGlowFade().
	 *
	 * Far: beyond this the trace is exactly as bright as it was measured to need. 320uu is a little
	 * over three capsule widths, so nothing at a normal fighting distance is affected at all.
	 * Near: at and below this the eye is effectively inside the volume.
	 * MinScale: NOT zero. The trace must stay visible from inside it — it is lethal from inside it.
	 */
	constexpr double ProximityFadeFarDistance = 320.0;
	constexpr double ProximityFadeNearDistance = 30.0;
	constexpr float ProximityFadeMinScale = 0.10f;

	/**
	 * How long client visuals stay hidden after MulticastClearTrail. The reliable multicast can
	 * arrive a frame before the property delta that actually empties Items; without this the
	 * trace of a just-killed holder flickers back for a few frames.
	 */
	constexpr float TrailClearSuppressSeconds = 0.35f;

	/**
	 * Sweeps longer than this are treated as teleports (respawn, post-score reposition) rather
	 * than movement, and are not tested — otherwise the segment from a player's pre-respawn
	 * position to their spawn point would scythe through the whole arena.
	 */
	constexpr double MinTeleportSweepDistance = 600.0;

	/**
	 * The same idea applied to the holder: a gap this large between two consecutive trace points
	 * cannot have been walked, so the trace restarts rather than joining them into one lethal
	 * segment spanning the arena.
	 */
	constexpr double MaxTrailSegmentLength = 1000.0;

	/** Divide-by-zero epsilon. Written as a literal on purpose: the KINDA_SMALL_NUMBER family of
	 *  macros was renamed during the 5.x line and we must compile on 5.4 through 5.8. */
	constexpr double GeometryEpsilon = 1.0e-8;

	/**
	 * How long after the server last saw a pawn inside its dash window that pawn still counts as
	 * dashing for the trip test. Comfortably longer than one server frame at 60Hz and shorter than
	 * the dash cooldown, so it can never turn "dashed a moment ago" into free permanent immunity to
	 * the rule that walking does nothing.
	 */
	constexpr double RecentDashGraceSeconds = 0.15;

	// ---------------------------------------------------------------------------------------------
	// THE SMEAR's cross-section. Every number is a FRACTION of the lethal volume, never an absolute —
	// the shape has to be re-derived if TrailRadius/TrailHeight are retuned, or the player is being
	// shown a boundary that is not the boundary.
	//
	// WHY THE MIDDLE OF THE VOLUME IS LEFT EMPTY, which is the single most important decision in this
	// file and was arrived at by looking at a screenshot of the first attempt:
	//
	//   M_TraceNeon IS OPAQUE AND WRITES DEPTH. The first version of this pass drew the smear as one
	//   band spanning the full lethal height at the full lethal width — geometrically perfect, and it
	//   completely swallowed the mannequins. Every ghost was rendered, correctly posed, in the right
	//   place, and every one of them was INSIDE an opaque cylinder. The capture came back as a flat
	//   featureless slab of cyan: exactly the "old cylinder models" look the user asked us to leave.
	//
	// So the smear now draws the two bands the eye needs and vacates the band the mannequins live in:
	//
	//   FLOOR BAND   bottom 38% (0-72uu of 190), at EXACTLY the lethal width. This is the continuous
	//                one. It is the drawn statement of where the kill volume's edge is, it runs
	//                unbroken along every lethal segment, and it is the surface a DASHING player
	//                actually meets — the trip test is a horizontal sweep, so the decision a player is
	//                making when they judge a gap is a decision about this band.
	//   EYE LINE     a thin hot ribbon at ~170uu, 30% of the width. First-person eye height is 152uu;
	//                a previous pass measured that a bright continuous element at that height is the
	//                one feature of a trace that survives being seen edge-on across the arena, and the
	//                mannequins cannot replace it because they are discrete and, at 6000uu, two pixels.
	//   THE GAP      72-162uu is left open, and that is where a Mannequin's torso, arms and shoulders
	//                are. The after-images show through it.
	//
	// The honest cost is written up in the block comment above RebuildVisuals(): the smear no longer
	// paints the full HEIGHT of the kill volume. It paints the full WIDTH, continuously, at the height
	// a dasher crosses. The vertical extent was never what the trip test turns on anyway — its
	// vertical threshold is TrailHeight/2 + the tripper's own capsule half-height (95 + 88 = 183uu),
	// which any grounded or jumping player passes trivially.
	// ---------------------------------------------------------------------------------------------

	constexpr double SmearBodyCentreFrac = 0.190;
	constexpr double SmearBodyHeightFrac = 0.380;
	// EXACTLY the lethal width, not a flattering 0.9 of it. A boundary drawn narrower than it really
	// is turns the trace into a trap rather than a warning. (The trip test is wider still — it
	// inflates by the tripper's own capsule radius — so the error that remains favours the player.)
	constexpr double SmearBodyWidthFrac = 1.00;

	constexpr double SmearHeadCentreFrac = 0.900;
	constexpr double SmearHeadHeightFrac = 0.100;
	constexpr double SmearHeadWidthFrac = 0.30;

	/**
	 * Glow per part, on M_TraceNeon.
	 *
	 * MEASURED, and the reason the split exists at all: a previous pass ran the whole trace at 3.4
	 * and every channel clipped, so it rendered as a shapeless white slab — extremely visible and
	 * useless, because you could no longer tell WHOSE it was, and a trace whose team you cannot read
	 * is a trace you cannot decide whether to dash through.
	 *
	 * So the body stays under 2 and keeps its team colour through the tonemapper, and the HEAD —
	 * a small element, exactly the case where clipping to white is the desired look — carries the
	 * brightness. That is not decoration either: the head sits at ~160uu above the floor, which is
	 * first-person eye height, so a chain of hot heads is the one feature of the smear that survives
	 * being seen edge-on from a player's own eyeline at any range. Do not dim it.
	 */
	constexpr float SmearBodyGlow = 1.50f;
	constexpr float SmearHeadGlow = 4.20f;

	/**
	 * What the smear's brightness is multiplied by ONCE THE POSED GHOSTS ARE DRAWING.
	 *
	 * This is the whole art direction of v4 §2 in one number. At 1.0 the smear is as bright as the
	 * mannequins and the trace goes back to reading as a solid extruded fence with figures buried in
	 * it. Near 0 the mannequins float in a row of statues and the gaps between them look passable,
	 * which is the one thing the trip mechanic cannot afford. It wants to sit where the smear is
	 * plainly, continuously THERE and the mannequins are plainly the brighter thing.
	 *
	 * When the ghosts are unavailable this is not applied at all and the smear runs at full strength,
	 * i.e. the pre-v4 look, so a build with no character art still has a perfectly readable trace.
	 */
	float GSmearGlowScale = 0.50f;

	FAutoConsoleVariableRef CVarSmearGlowScale(
		TEXT("Trace.Trail.SmearGlowScale"),
		GSmearGlowScale,
		TEXT("Brightness of the continuous smear relative to the posed-Mannequin after-images (spec v4 2). "
		     "1 = the old solid-fence look; the smear must stay clearly visible or the gaps between "
		     "ghosts read as passable. Cosmetic only - the kill volume is unchanged either way."),
		ECVF_Default);

	/** Brightness of a posed after-image on M_TraceNeon. Above the smear, below the clipping point. */
	float GGhostGlow = 2.60f;

	FAutoConsoleVariableRef CVarGhostGlow(
		TEXT("Trace.Trail.GhostGlow"),
		GGhostGlow,
		TEXT("Emissive strength of the posed-Mannequin after-images (spec v4 2). Above ~3.5 the team "
		     "colour clips to white and you can no longer tell whose trace it is."),
		ECVF_Default);

	/** Oldest after-images dim to this fraction of full glow. Never to zero: they are still lethal. */
	constexpr float GhostOldestGlowScale = 0.55f;

	/** Spec §4: while the PASS window is open the trace hardens. This is what that looks like. */
	constexpr float GhostInvulnerableGlowScale = 1.90f;

	// The PARRY's own tell (v3 §3) is not here: it is the whole trace going RED, and both the colour
	// and its glow multiplier are TraceParry tunables so the two halves of one visual state cannot
	// drift apart. See TraceParry::GetTintColor() / GetGlowScale().

	/** Where along [A,B] the point P projects, clamped to [0,1]. Zero-length segments give 0. */
	double SegmentAlpha(const FVector& A, const FVector& B, const FVector& P)
	{
		const FVector Segment = B - A;
		const double LengthSquared = Segment.SizeSquared();
		if (LengthSquared <= GeometryEpsilon)
		{
			return 0.0;
		}
		return FMath::Clamp(FVector::DotProduct(P - A, Segment) / LengthSquared, 0.0, 1.0);
	}

	// =============================================================================================
	// GHOST KNOBS: UTraceSettings is the authority, the CVars are overrides
	//
	// Every value above shipped as a console variable only, which meant the five after-image dials
	// were invisible to the settings panel the user actually tunes from — a dead knob on the page
	// and a live one on the console is worse than either alone, because the page lies.
	//
	// So each dial is resolved at the point of use: the UTraceSettings property wins, UNLESS the
	// CVar has been explicitly set at runtime or from an ini (SetBy above Constructor), in which
	// case a measurement run's pin wins. Same pattern as ATraceCore's mode-B tuning and as
	// GTraceLifetimeOverride, and it is read fresh every time so PIE edits land immediately.
	//
	// Property names are matched here at COMPILE time, not by reflection, so a rename that misses
	// one half fails the build instead of silently falling back to the CVar default.
	// =============================================================================================

	/** True once something other than the C++ default has written this CVar. */
	bool IsCVarOverridden(const TCHAR* Name)
	{
		const IConsoleVariable* Console = IConsoleManager::Get().FindConsoleVariable(Name);
		if (Console == nullptr)
		{
			return false;
		}
		const uint32 SetBy = static_cast<uint32>(Console->GetFlags()) & static_cast<uint32>(ECVF_SetByMask);
		return SetBy > static_cast<uint32>(ECVF_SetByConstructor);
	}

	float ResolvedGhostSpacing()
	{
		const float Value = IsCVarOverridden(TEXT("Trace.Trail.GhostSpacing"))
			? GGhostSpacing
			: UTraceSettings::Get().TraceGhostSpacingUU;
		return FMath::Max(20.f, Value);
	}

	int32 ResolvedMaxGhosts()
	{
		const int32 Value = IsCVarOverridden(TEXT("Trace.Trail.GhostMaxCount"))
			? GMaxPoseGhosts
			: UTraceSettings::Get().MaxTraceGhosts;
		return FMath::Clamp(Value, 0, 64);
	}

	float ResolvedGhostGlow()
	{
		const float Value = IsCVarOverridden(TEXT("Trace.Trail.GhostGlow"))
			? GGhostGlow
			: UTraceSettings::Get().TraceGhostGlow;
		return FMath::Max(0.f, Value);
	}

	float ResolvedSmearGlowScale()
	{
		const float Value = IsCVarOverridden(TEXT("Trace.Trail.SmearGlowScale"))
			? GSmearGlowScale
			: UTraceSettings::Get().TraceSmearGlowScale;
		return FMath::Clamp(Value, 0.02f, 4.f);
	}

	int32 ResolvedGhostForcedLOD()
	{
		const int32 Value = IsCVarOverridden(TEXT("Trace.Trail.GhostForcedLOD"))
			? GGhostForcedLOD
			: UTraceSettings::Get().TraceGhostForcedLOD;
		return FMath::Clamp(Value, 0, 4);
	}
}


UTraceTrailComponent::UTraceTrailComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Post-physics so the swept trip test reads this frame's *final* capsule positions rather
	// than positions from halfway through the movement update.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	SetIsReplicatedByDefault(true);

	// The component itself is a bare USceneComponent: a logical anchor with no primitive and
	// therefore no collision of its own. Everything it draws is a pooled child mesh.

	// Engine basic shapes only, resolved with a constructor-time FObjectFinder so the cooker
	// follows the CDO reference and the asset survives into a packaged build. A bare runtime
	// LoadObject would return nullptr there.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		CylinderMesh = CylinderFinder.Object;
	}

	// (The sphere that used to be the after-image's head is gone with the per-point silhouette. The
	// smear's head band runs ALONG a segment, so it wants a tube, not a ball: a stretched sphere's
	// rounded ends would leave a visible pinch at every joint in exactly the element whose job is to
	// be an unbroken line at eye height.)

	// The trace is drawn on the arena's own unlit neon material, NOT on BasicShapeMaterial.
	//
	// This mattered more than anything else about the visuals. BasicShapeMaterial is LIT, and this
	// world is a black room with three deliberately weak directional lights in it - so a tinted lit
	// mesh standing on the floor came out as a dark grey-blue smudge that you could genuinely walk
	// past without noticing. M_TraceNeon emits Color * Glow with no lighting term at all, which is
	// what makes the after-image the same kind of object as every glowing edge in the arena, and
	// pushes it past the post-process bloom threshold so it reads as light rather than as geometry.
	//
	// The trace is the ONLY counterplay to a shielded holder (§3), so a player who cannot see it
	// cannot play the game.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		TrailMaterial = NeonFinder.Object;
		bTrailMaterialIsNeon = true;
	}

	// Fallback exactly as the arena builder does it: /Game/Generated is gitignored and produced by
	// Scripts/generate_content.py, so a developer who has not run that script must still get a
	// visible - if flat and lit - trace rather than an invisible one. No .uasset is ever a hard
	// requirement.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (TrailMaterial == nullptr && BasicFinder.Succeeded())
	{
		TrailMaterial = BasicFinder.Object;
		bTrailMaterialIsNeon = false;
	}
}

void UTraceTrailComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None on both: the holder's own client needs to see the trace it is laying just as
	// much as everyone else does.
	DOREPLIFETIME(UTraceTrailComponent, TrailPoints);
	DOREPLIFETIME(UTraceTrailComponent, bEmitting);

	// COND_None on both, deliberately. The parry's entire job is to be SEEN — by the carrier and by
	// the enemy already committed to a dash — so every machine has to know the window is open, not
	// just the owner. Two floats, written once per parry.
	DOREPLIFETIME(UTraceTrailComponent, ParryEndServerTime);
	DOREPLIFETIME(UTraceTrailComponent, ParryCooldownEndServerTime);
}

void UTraceTrailComponent::OnRegister()
{
	Super::OnRegister();

	// The fast array's back pointer must be live before the first delta lands on a client, which
	// can happen before BeginPlay. It cannot be set from the constructor: FObjectInitializer
	// copies UPROPERTY values from the archetype *after* the C++ constructor runs, so a `this`
	// captured there would be overwritten with the CDO's pointer.
	TrailPoints.OwnerComponent = this;
}

void UTraceTrailComponent::BeginPlay()
{
	Super::BeginPlay();

	TrailPoints.OwnerComponent = this;
	bVisualsDirty = true;
}

void UTraceTrailComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyVisualPool();
	PreviousLocations.Reset();

	Super::EndPlay(EndPlayReason);
}

void UTraceTrailComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return;
	}

	if (Owner->HasAuthority())
	{
		// §3: the trace dies with its holder, instantly. ATraceCore drives this in the normal flow;
		// this is the safety net so a trace can never outlive the body that owns it and go on
		// killing a corpse.
		//
		// Checked whether or not this component is still emitting. A trace kills the player who
		// LAID it, so once that player is dead it can never kill anyone again — and by the
		// visible == lethal invariant a set of points that can never kill must not be on screen.
		// (Before, this was inside an `if (bEmitting)`, so a holder who passed the Core away and
		// then died left their trace hanging in the air, visible and completely inert.)
		{
			const ATraceCharacter* Holder = GetOwnerCharacter();
			if ((Holder == nullptr || !Holder->IsAlive()) && (bEmitting || TrailPoints.Items.Num() > 0))
			{
				SetEmitting(false);
				ClearTrail();
			}
		}

		ServerUpdateTrail();
		ServerRunTripTest(DeltaTime);
		ServerTickBotAutoParry();
	}

	// Listen servers draw the trace too; only a headless server skips it.
	if (GetNetMode() != NM_DedicatedServer)
	{
		UpdateVisuals();

		// EVERY frame, not just on a rebuild: this depends on where the local camera is, and the
		// camera moves continuously while the geometry does not. See the function's comment.
		ApplyProximityGlowFade();
	}
}


// =================================================================================================
// Public API
// =================================================================================================

float UTraceTrailComponent::GetTraceLifetimeSeconds()
{
	const float Value = (GTraceLifetimeOverride >= 0.f)
		? GTraceLifetimeOverride : UTraceSettings::Get().TrailLifetime;
	return FMath::Max(0.1f, Value);
}

float UTraceTrailComponent::GetTurnoverGraceSeconds()
{
	const float Value = (GTurnoverGraceOverride >= 0.f)
		? GTurnoverGraceOverride : UTraceSettings::Get().CoreTurnoverGraceSeconds;
	return FMath::Clamp(Value, 0.f, 5.f);
}

void UTraceTrailComponent::SetEmitting(bool bEmit)
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	if (bEmitting == bEmit)
	{
		return;
	}

	bEmitting = bEmit;

	// Remembered sweep positions are only valid within one continuous emission window.
	PreviousLocations.Reset();

	if (bEmit)
	{
		// A new holder never inherits the previous trace.
		ClearTrail();

		// Lay the first point at the transfer itself rather than one spacing later — unless the
		// §2 grace is running, in which case ServerUpdateTrail lays nothing and the trace simply
		// starts a second later, from wherever the holder has got to by then.
		ServerUpdateTrail();
	}
	else
	{
		// Not emitting means not in a grace window either. Leaving a stale deadline behind would
		// eat the first second of the NEXT emission window this component ever opens.
		EmitGraceEndServerTime = 0.f;
	}

	// Note: stopping does NOT clear. A trace left behind by a completed pass is harmless (the trip
	// test requires bEmitting) and fading out over its lifetime reads much better than popping.
	// Death is the case that must clear instantly, and it does so explicitly.

	UE_LOG(LogTraceGame, Verbose, TEXT("Trace: %s emitting for %s"),
		bEmit ? TEXT("started") : TEXT("stopped"), *GetNameSafe(GetOwner()));
}

void UTraceTrailComponent::SetEmitGrace(float Seconds)
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	// v3 §1: 1.0s -> 0.4s. Capped rather than replaced, so a caller asking for less still wins and a
	// stale 1.0 in TraceCoreTuning cannot reinstate the old rule. See the header for the full reason.
	const float Granted = (Seconds > 0.f) ? FMath::Min(Seconds, GetTurnoverGraceSeconds()) : 0.f;

	EmitGraceEndServerTime = (Granted > 0.f) ? (GetServerTimeSeconds() + Granted) : 0.f;

	if (Seconds > 0.f)
	{
		// Log level, not Verbose: this is one line per turnover (not per tick), and the last two
		// times a mechanic here was declared dead it was because its only evidence sat at Verbose.
		UE_LOG(LogTraceGame, Log,
			TEXT("[TRACEGRACE] %s: %.2fs requested, %.2fs granted before the trace BEGINS FORMING. "
			     "Already-laid segments stay lethal throughout."),
			*GetNameSafe(GetOwner()), Seconds, Granted);
	}
}

bool UTraceTrailComponent::IsEmitting() const
{
	return bEmitting;
}

// =================================================================================================
// TRACE INVULNERABILITY — TWO SOURCES, COMPOSED HERE AND NOWHERE ELSE
//
// If you read one comment in this file, read this one and TraceParry.h's file header.
//
//   Source 1, THE PASS WINDOW (§4). ~0.5s. Owned by ATraceCore. It is the SAME replicated bool that
//   drops the holder's shield, so the risk beat cannot half-apply.
//
//   Source 2, THE PARRY (v3 §3). 0.1s on a 1.5s cooldown. Owned by this component. It does NOT
//   touch the shield: the carrier stays bulletproof throughout.
//
// They are ORed, and each stays separately readable. The failure mode this structure exists to
// prevent is a future reader assuming there is only one source and "simplifying" — e.g. routing the
// parry through ATraceCore::IsTraceInvulnerableFor(), which would silently make the carrier
// shootable for 0.1s every 1.5s, or letting one source's end time clear the other's.
//
// Neither source can end the other: they are different pieces of state on different objects, and
// nothing below writes across the boundary. A parry raised during a pass window simply reddens a
// trace that was already unbreakable, and expires without shortening the pass.
// =================================================================================================

bool UTraceTrailComponent::IsPassWindowInvulnerable() const
{
	// Read straight out of the Core rather than mirrored here. §4 says the trace hardening and the
	// holder's shield loss happen "simultaneously"; the only way to guarantee that is for both to be
	// the same replicated bool, read twice.
	return ATraceCore::IsTraceInvulnerableFor(GetOwner());
}

bool UTraceTrailComponent::IsTraceInvulnerable() const
{
	return IsPassWindowInvulnerable() || IsParryActive();
}

void UTraceTrailComponent::NotifyInvulnerabilityChanged()
{
	bVisualsDirty = true;
}


// =================================================================================================
// The parry (spec v3 §3)
// =================================================================================================

bool UTraceTrailComponent::IsParryActive() const
{
	// DEBUG ONLY, and it is checked first so a verification run cannot be defeated by a cooldown.
	// Off in every normal session; see TraceParry.h.
	if (TraceParry::IsWindowForced())
	{
		const ATraceCharacter* Holder = GetOwnerCharacter();
		return Holder != nullptr && Holder->IsCarrier();
	}

	// AUTHORITATIVE, and only the authoritative value: the replicated deadline on the replicated
	// clock. The local prediction is deliberately absent — see IsParryVisuallyActive().
	return ParryEndServerTime > 0.f && GetServerTimeSeconds() < ParryEndServerTime;
}

bool UTraceTrailComponent::IsParryVisuallyActive() const
{
	if (IsParryActive())
	{
		return true;
	}

	// Cosmetic prediction. Never reached on the server, because the server sets the authoritative
	// value directly and never sets this.
	return LocalParryPredictEndTime > 0.f && GetServerTimeSeconds() < LocalParryPredictEndTime;
}

float UTraceTrailComponent::GetParryWindowRemaining() const
{
	if (ParryEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, ParryEndServerTime - GetServerTimeSeconds());
}

float UTraceTrailComponent::GetParryCooldownRemaining() const
{
	if (ParryCooldownEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, ParryCooldownEndServerTime - GetServerTimeSeconds());
}

void UTraceTrailComponent::RequestParry(ETraceParryRefusal& OutRefusal)
{
	OutRefusal = ETraceParryRefusal::None;

	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr)
	{
		OutRefusal = ETraceParryRefusal::NoPawn;
		return;
	}

	if (OwnerActor->HasAuthority())
	{
		ServerTryBeginParry(OutRefusal);
		return;
	}

	// ---- owning client -----------------------------------------------------------------------
	//
	// Predict the TINT and nothing else. The window that decides whether anybody lives is the
	// server's, and the trip test never reads the prediction.
	const ATraceCharacter* Holder = GetOwnerCharacter();
	if (Holder == nullptr || !Holder->IsAlive())
	{
		OutRefusal = ETraceParryRefusal::Dead;
		return;
	}
	if (!Holder->IsCarrier())
	{
		OutRefusal = ETraceParryRefusal::NotCarrying;
		return;
	}

	// Respect the replicated cooldown locally too, so mashing the key does not strobe the trace red
	// on requests the server is certain to refuse.
	if (GetParryCooldownRemaining() > 0.f)
	{
		OutRefusal = ETraceParryRefusal::OnCooldown;
		return;
	}

	LocalParryPredictEndTime = GetServerTimeSeconds() + TraceParry::GetDurationSeconds();
	bVisualsDirty = true;

	ServerRequestParry();
}

void UTraceTrailComponent::ServerRequestParry_Implementation()
{
	ETraceParryRefusal Refusal = ETraceParryRefusal::None;
	ServerTryBeginParry(Refusal);
}

bool UTraceTrailComponent::ServerTryBeginParry(ETraceParryRefusal& OutRefusal)
{
	OutRefusal = ETraceParryRefusal::None;

	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority())
	{
		OutRefusal = ETraceParryRefusal::NoPawn;
		return false;
	}

	const ATraceCharacter* Holder = GetOwnerCharacter();
	if (Holder == nullptr)
	{
		OutRefusal = ETraceParryRefusal::NoPawn;
		return false;
	}
	if (!Holder->IsAlive())
	{
		OutRefusal = ETraceParryRefusal::Dead;
		return false;
	}

	// v3 §3, first line: "a parry mechanic for THE CORE CARRIER". A non-carrier pressing parry does
	// nothing at all — no window, and no cooldown burnt either, so the key is dead rather than
	// punishing while you are not holding the Core.
	if (!Holder->IsCarrier())
	{
		OutRefusal = ETraceParryRefusal::NotCarrying;
		return false;
	}

	const float Now = GetServerTimeSeconds();
	if (ParryCooldownEndServerTime > 0.f && Now < ParryCooldownEndServerTime)
	{
		OutRefusal = ETraceParryRefusal::OnCooldown;
		return false;
	}

	ParryEndServerTime = Now + TraceParry::GetDurationSeconds();
	ParryCooldownEndServerTime = Now + TraceParry::GetCooldownSeconds();

	// A listen server is also a viewer: it will not get OnRep, so dirty the visuals here.
	bVisualsDirty = true;

	UE_LOG(LogTraceGame, Log, TEXT("[PARRY] %s parried: trace invulnerable for %.2fs, next parry in %.2fs."),
		*GetNameSafe(GetOwner()), TraceParry::GetDurationSeconds(), TraceParry::GetCooldownSeconds());

	return true;
}

void UTraceTrailComponent::OnRep_ParryEndServerTime()
{
	// The authority has spoken; the prediction has nothing left to cover.
	LocalParryPredictEndTime = 0.f;
	bVisualsDirty = true;
}

void UTraceTrailComponent::ServerTickBotAutoParry()
{
	if (!TraceParry::IsBotAutoParryEnabled())
	{
		return;
	}

	const ATraceCharacter* Holder = GetOwnerCharacter();
	if (Holder == nullptr || !Holder->IsAlive() || !Holder->IsCarrier())
	{
		return;
	}

	// Only AI. A human's parry stays a human's decision even with this on.
	if (Cast<APlayerController>(Holder->GetController()) != nullptr)
	{
		return;
	}

	if (GetParryCooldownRemaining() > 0.f || IsParryActive())
	{
		return;
	}

	ETraceParryRefusal Refusal = ETraceParryRefusal::None;
	ServerTryBeginParry(Refusal);
}

int32 UTraceTrailComponent::ComputeLastLethalIndex() const
{
	const int32 PointCount = TrailPoints.Items.Num();
	if (PointCount == 0)
	{
		return -1;
	}

	// Nobody is standing on the head of a trace that has stopped growing, so there is nothing to
	// exempt: a residual trace left behind by a pass or a Core steal is lethal end to end. This is
	// half of the reported bug — see ServerRunTripTest for the other half.
	if (!bEmitting)
	{
		return PointCount - 1;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const int32 MaxExempt = FMath::Max(0, Settings.TrailHeadGracePoints);
	if (MaxExempt == 0)
	{
		return PointCount - 1;   // Exemption switched off entirely.
	}

	// WHY THE HEAD EXEMPTION IS A DISTANCE, NOT A COUNT.
	//
	// It exists for exactly one reason: the newest point is under the holder's own feet, so without
	// it a defender could stand on the emitter and dash on the spot instead of crossing anything.
	// That is a claim about ONE BODY WIDTH of trace, and it is written here as one body width —
	// TrailRadius, the lethal volume's own radius, measured back along the chain.
	//
	// Read as a point COUNT (the old code took the newest TrailHeadGracePoints=3 wholesale) it was
	// 3 x TrailPointSpacing = 180uu of trace that was drawn but could not kill, permanently trailing
	// every carrier, and 5 points' worth of travel after every turnover during which the trace was
	// visible and completely harmless. TrailHeadGracePoints still caps the exemption, so 0 removes
	// it and a larger value cannot make the invisible-but-drawn window come back: the distance
	// binds first at any sane spacing.
	const double GraceDistance = FMath::Max(0.0, static_cast<double>(Settings.TrailRadius));

	// The head point itself always counts as exempt while emitting: it is the holder's own position
	// this frame, not a place they have been.
	int32 ExemptCount = 1;
	double DistanceFromHead = 0.0;
	for (int32 Index = PointCount - 1; Index > 0 && ExemptCount < MaxExempt; --Index)
	{
		DistanceFromHead += FVector::Dist(TrailPoints.Items[Index - 1].Location, TrailPoints.Items[Index].Location);
		if (DistanceFromHead > GraceDistance)
		{
			break;
		}
		++ExemptCount;
	}

	return PointCount - 1 - ExemptCount;
}

void UTraceTrailComponent::ClearTrail()
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	if (TrailPoints.Items.Num() > 0)
	{
		TrailPoints.Items.Reset();

		// Removal from a fast array is signalled with MarkArrayDirty, not MarkItemDirty.
		TrailPoints.MarkArrayDirty();

		// The delta alone would get there, but "instantly" is a game rule here — the reliable
		// multicast lets every client drop the visuals on the same frame the holder dies.
		//
		// Guarded on there having actually been something to clear: ClearTrail() is called on every
		// death (twice - HandleDeath and NotifyCharacterDied), on Logout, on SetEmitting(true), and
		// for EVERY player on EVERY goal from ResetPlayersToSpawns. Unguarded that is ~10 reliable
		// multicasts per score, nine of them for components that have never held a point, on the
		// exact frame the server is also teleporting ten pawns. Reliable RPCs cannot be dropped.
		MulticastClearTrail();
	}

	PreviousLocations.Reset();
	bVisualsDirty = true;
}

void UTraceTrailComponent::MulticastClearTrail_Implementation()
{
	HideSmearFrom(0);
	ClearGhostRecords();

	LastVisualPointCount = -1;
	LastVisualHead = FVector::ZeroVector;
	LastVisualTail = FVector::ZeroVector;
	bVisualsDirty = true;

	// Hold the visuals down briefly: this reliable RPC can beat the property delta that empties
	// Items, and re-showing a dead holder's trace for a few frames looks like a bug. Any
	// subsequent replication callback (or an empty Items) lifts the hold early.
	if (const UWorld* World = GetWorld())
	{
		VisualSuppressUntilTime = static_cast<float>(World->GetTimeSeconds()) + TrailClearSuppressSeconds;
	}
}

void UTraceTrailComponent::OnTrailPointsChanged()
{
	// Called once per changed item from FTraceTrailPoint's replication callbacks, which means it
	// can fire many times per packet and, for removals, *before* Items has actually shrunk.
	// So: flag only, and rebuild from the settled array on the next tick.
	bVisualsDirty = true;

	// A delta arrived, so Items is authoritative again — no reason to keep suppressing.
	VisualSuppressUntilTime = 0.f;
}


// =================================================================================================
// Server: laying the trace
// =================================================================================================

void UTraceTrailComponent::ServerUpdateTrail()
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Now = GetServerTimeSeconds();
	bool bChanged = false;

	// 1. Expire. Items are strictly ordered oldest-first, so the first survivor ends the scan.
	const float Lifetime = GetTraceLifetimeSeconds();
	int32 ExpiredCount = 0;
	while (ExpiredCount < TrailPoints.Items.Num()
		&& (Now - TrailPoints.Items[ExpiredCount].BirthServerTime) > Lifetime)
	{
		++ExpiredCount;
	}
	if (ExpiredCount > 0)
	{
		TrailPoints.Items.RemoveAt(0, ExpiredCount);
		TrailPoints.MarkArrayDirty();
		bChanged = true;
	}

	// 2. Hard cap, oldest dropped first.
	const int32 MaxPoints = FMath::Max(2, Settings.MaxTrailPoints);
	if (TrailPoints.Items.Num() > MaxPoints)
	{
		TrailPoints.Items.RemoveAt(0, TrailPoints.Items.Num() - MaxPoints);
		TrailPoints.MarkArrayDirty();
		bChanged = true;
	}

	// 3. Append, distance-gated so a stationary holder does not spam identical points.
	//
	//    THE TRANSFER GRACE LIVES RIGHT HERE, AND ONLY HERE (§2; 0.4s since v3 §1). For its duration
	//    the new holder runs around laying nothing. Everything else about the emission window is
	//    already true — bEmitting is set, THE TRIP TEST IS LIVE — there simply are no points yet,
	//    which is exactly what "the trace has not begun to form" means.
	//
	//    THIS IS THE ONLY CORRECT PLACE FOR IT. Shortening the grace must not, and here cannot, make
	//    a visible trace non-lethal: the invariant is "once a segment is visible it is lethal, except
	//    during an explicit parry or pass window", and the grace is upstream of any segment existing
	//    at all. A previous pass shipped the grace as an early-out in the TRIP TEST instead; that is
	//    the reported "I dashed through it and nothing happened" bug, and it must not come back.
	const bool bGraceActive = (EmitGraceEndServerTime > 0.f) && (Now < EmitGraceEndServerTime);

	if (bEmitting && !bGraceActive)
	{
		// Anchor on the capsule centre (the owner's actor location), NOT on this component's own
		// world location. The trip test measures every candidate by its actor location, so both
		// halves of the geometry have to live in the same reference frame; if the pawn ever
		// attaches this component with an offset, GetComponentLocation() would quietly slide the
		// trace away from the volume the test evaluates. Falls back for a non-character owner.
		const ATraceCharacter* Holder = GetOwnerCharacter();
		const FVector Location = Holder != nullptr ? Holder->GetActorLocation() : GetComponentLocation();

		const double Spacing = FMath::Max(1.0, static_cast<double>(Settings.TrailPointSpacing));

		const bool bHasHead = TrailPoints.Items.Num() > 0;
		const double DistanceFromHead = bHasHead
			? FVector::Dist(Location, TrailPoints.Items.Last().Location)
			: 0.0;

		// Teleport, not movement — restart rather than laying a segment across the map.
		if (bHasHead && DistanceFromHead > MaxTrailSegmentLength)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("Trace: discontinuity of %.0fuu on %s, restarting"),
				DistanceFromHead, *GetNameSafe(GetOwner()));

			ClearTrail();
			bChanged = true;
		}

		if (TrailPoints.Items.Num() == 0 || DistanceFromHead >= Spacing)
		{
			FTraceTrailPoint& NewPoint = TrailPoints.Items.AddDefaulted_GetRef();
			NewPoint.Location = Location;
			NewPoint.BirthServerTime = Now;

			// Adding or changing an item is signalled per item; only removals need MarkArrayDirty.
			TrailPoints.MarkItemDirty(NewPoint);
			bChanged = true;
		}
	}

	if (bChanged)
	{
		bVisualsDirty = true;

		// Authority cannot race itself: TrailPoints is settled by the time we get here. A listen
		// server runs MulticastClearTrail on itself too, so without this the host would hold its
		// own visuals down for the suppression window every time the Core changes hands.
		VisualSuppressUntilTime = 0.f;
	}
}


// =================================================================================================
// Server: the trip test — the whole game lives here
// =================================================================================================

void UTraceTrailComponent::ServerRunTripTest(float DeltaTime)
{
	ATraceCharacter* Holder = GetOwnerCharacter();

	// -------------------------------------------------------------------------------------------
	// THE INVARIANT: ONCE A TRACE SEGMENT EXISTS AND IS VISIBLE, IT IS LETHAL.
	//
	// The gate is the POINTS, not bEmitting. This is the bug the user reported: a holder who lost
	// the Core (a completed pass, or being killed and the Core changing hands) stops EMITTING, but
	// the trace they already laid stays on screen for its full lifetime — and the old test bailed
	// out on !bEmitting, so every one of those visible segments was completely inert. Dashing
	// through a trace right after a turnover did nothing, which is exactly what was described.
	//
	// A trace kills the player who laid it, so the only hard requirement is that that player is
	// still alive; if they are not, TickComponent has already wiped the points above.
	// -------------------------------------------------------------------------------------------
	if (Holder == nullptr || !Holder->IsAlive() || TrailPoints.Items.Num() == 0)
	{
		PreviousLocations.Reset();
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// -------------------------------------------------------------------------------------------
	// THE TWO INTENDED EXCEPTIONS TO THE INVARIANT ABOVE. Both suppress the KILL and nothing else.
	//
	// §4, THE PASS WINDOW / RISK BEAT. From the instant the holder inputs a pass until it completes
	// or cancels, the trace CANNOT BE BROKEN. This is the whole reason the passer is willing to give
	// up their shield: for those 0.5s the dash counterplay is off the table and the only way to stop
	// the pass is to shoot them. Signposted by GhostInvulnerableGlowScale.
	//
	// v3 §3, THE PARRY. 0.1s, carrier-only, on a cooldown, shield untouched. Signposted by the
	// entire trace turning RED. Read from IsParryActive(), which is the REPLICATED window and never
	// the client's local prediction — this line is where "server-authoritative on whether a dash
	// landed inside the window" actually happens.
	//
	// The two are read separately so the log can name which one saved the carrier; a merged bool
	// would turn every one of these lines into a guess. They suppress the KILL only — the loop below
	// still runs, so PreviousLocations keeps tracking every candidate. The old code returned early
	// and reset them, which meant the first tick after the window closed had no valid previous
	// position for anyone; that is a sweep the teleport guard throws away, i.e. the first dash after
	// every single pass was silently swallowed.
	// -------------------------------------------------------------------------------------------
	const bool bPassWindow = IsPassWindowInvulnerable();
	const bool bParry = IsParryActive();
	const bool bInvulnerable = bPassWindow || bParry;

	// Everything up to and including this index is BOTH lethal and drawn; everything after it is
	// neither. One function, one answer, used by the trip test and by RebuildVisuals.
	const int32 LastTestableIndex = ComputeLastLethalIndex();

	// Snapshot the testable positions. Nothing below may touch TrailPoints.Items: applying a kill
	// re-enters this component (death -> SetCarrying(false) -> SetEmitting/ClearTrail) and would
	// invalidate any live iteration.
	TestPositions.Reset();
	for (int32 PointIndex = 0; PointIndex <= LastTestableIndex; ++PointIndex)
	{
		TestPositions.Add(TrailPoints.Items[PointIndex].Location);
	}

	// The newest stub — the holder's own footprint — which is neither lethal nor drawn. Snapshotted
	// only so a dash that crosses it can SAY SO in the log instead of looking like a lost kill.
	ExemptPositions.Reset();
	for (int32 PointIndex = FMath::Max(0, LastTestableIndex); PointIndex < TrailPoints.Items.Num(); ++PointIndex)
	{
		ExemptPositions.Add(TrailPoints.Items[PointIndex].Location);
	}

	// Broad phase. The narrow phase below is O(candidates x segments) - with MaxTrailPoints=256 and
	// ten players that is ~2500 segment-to-segment tests every server frame, for a test that is
	// almost always a miss. One XY AABB over the whole trace turns the common case into four
	// comparisons per candidate. It is only ever used to reject, so it cannot change the outcome.
	// Written as four scalars rather than an FBox2D so nothing depends on that type's float/double
	// spelling on any given 5.x engine.
	double TrailMinX = TNumericLimits<double>::Max();
	double TrailMinY = TNumericLimits<double>::Max();
	double TrailMaxX = -TNumericLimits<double>::Max();
	double TrailMaxY = -TNumericLimits<double>::Max();
	for (const FVector& Position : TestPositions)
	{
		TrailMinX = FMath::Min(TrailMinX, Position.X);
		TrailMinY = FMath::Min(TrailMinY, Position.Y);
		TrailMaxX = FMath::Max(TrailMaxX, Position.X);
		TrailMaxY = FMath::Max(TrailMaxY, Position.Y);
	}

	TArray<ATraceCharacter*> Candidates;
	GatherTrackedCharacters(Candidates);

	const double MaxSweepDistance = FMath::Max(
		MinTeleportSweepDistance,
		static_cast<double>(Settings.DashSpeed) * static_cast<double>(DeltaTime) * 2.0);

	const double TrailRadius = FMath::Max(0.0, static_cast<double>(Settings.TrailRadius));
	const double TrailHalfHeight = FMath::Max(0.0, static_cast<double>(Settings.TrailHeight)) * 0.5;

	ATraceCharacter* Tripper = nullptr;

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (Candidate == nullptr)
		{
			continue;
		}

		const FVector CurrentLocation = Candidate->GetActorLocation();

		// Refresh the remembered position for EVERY tracked character, before any filtering and
		// even after a trip has been found. If we only tracked characters that pass the filters,
		// then a player's "previous" position would date from whenever they last happened to be
		// dashing, and the sweep for the first tick of their next dash would be a huge stale
		// segment that the teleport guard throws away — i.e. dashes would randomly not register.
		FVector PreviousLocation = CurrentLocation;
		if (FVector* Stored = PreviousLocations.Find(Candidate))
		{
			PreviousLocation = *Stored;
			*Stored = CurrentLocation;
		}
		else
		{
			PreviousLocations.Add(Candidate, CurrentLocation);
		}

		if (Tripper != nullptr)
		{
			continue;   // Already resolved this tick; keep looping only to refresh positions.
		}

		// --- eligibility, in the exact order the game rules state it -------------------------

		// The holder can never trip their own trace (grace points are not enough on their own
		// when bOnlyEnemiesTripTrail is turned off for tuning).
		if (Candidate == Holder)
		{
			continue;
		}

		// (a) alive
		if (!Candidate->IsAlive())
		{
			continue;
		}

		// (b) an enemy of the holder. Unknown teams never count as enemies; teammates never trip it.
		if (Settings.bOnlyEnemiesTripTrail)
		{
			const ETraceTeam HolderTeam = Holder->GetTeam();
			const ETraceTeam CandidateTeam = Candidate->GetTeam();
			const bool bIsEnemy = HolderTeam != ETraceTeam::None
				&& CandidateTeam != ETraceTeam::None
				&& CandidateTeam != HolderTeam;
			if (!bIsEnemy)
			{
				continue;
			}
		}

		// (c) dashing. This is the rule: walking or running through a trace does nothing at all,
		// and the dash is the only counterplay to a shielded holder.
		//
		// Sampled with a short trailing window rather than as an instant. This test ticks once per
		// server frame, but the server advances a remote client's dash clock inside MoveAutonomous
		// and can consume several client moves in one frame - so the tail of a dash (or, after a
		// hitch, all 0.18s of it) can be simulated *between* two ticks here. The displacement is
		// still credited to this frame's sweep, but DashTimeRemaining has already hit zero, and the
		// player watches themselves dash through the trace with nothing happening. The movement
		// component latches the last instant it was authoritatively dashing; accept that too.
		if (Settings.bRequireDashToTripTrail && !Candidate->IsDashing())
		{
			bool bRecentlyDashed = false;
			if (const UWorld* World = GetWorld())
			{
				if (const UTraceCharacterMovementComponent* CandidateMovement = Candidate->GetTraceMovement())
				{
					bRecentlyDashed = (World->GetTimeSeconds() - CandidateMovement->GetLastDashActiveWorldTime())
						<= RecentDashGraceSeconds;
				}
			}

			if (!bRecentlyDashed)
			{
				continue;
			}
		}

		// --- swept geometry -------------------------------------------------------------------

		const double SweepDistance = FVector::Dist(PreviousLocation, CurrentLocation);
		if (SweepDistance > MaxSweepDistance)
		{
			continue;   // Teleport, not movement.
		}

		double CapsuleRadius = 34.0;
		double CapsuleHalfHeight = 88.0;
		if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
		{
			CapsuleRadius = Capsule->GetScaledCapsuleRadius();
			CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		}

		// The trace is a vertical volume of radius TrailRadius and height TrailHeight swept along
		// the holder's path, and the tripper is a capsule swept along its path this tick. Test
		// those two sweeps as: horizontal segment-to-segment distance (which catches tunnelling
		// at dash speed, unlike a point test), plus a separate vertical overlap check so that
		// clearing the trace in the air is not a hit.
		const double HorizontalThreshold = TrailRadius + CapsuleRadius;
		const double VerticalThreshold = TrailHalfHeight + CapsuleHalfHeight;

		// Broad phase: if this candidate's swept XY box, inflated by the same horizontal threshold
		// the narrow phase uses, does not touch the trace's XY box, no segment can be within range.
		// Only ever used to REJECT the lethal test — the exempt stub below is two points and is
		// checked unconditionally, so the instrumentation can never be broad-phased away.
		bool bNearTrace = TestPositions.Num() > 0;
		if (bNearTrace)
		{
			const double SweepMinX = FMath::Min(PreviousLocation.X, CurrentLocation.X) - HorizontalThreshold;
			const double SweepMaxX = FMath::Max(PreviousLocation.X, CurrentLocation.X) + HorizontalThreshold;
			const double SweepMinY = FMath::Min(PreviousLocation.Y, CurrentLocation.Y) - HorizontalThreshold;
			const double SweepMaxY = FMath::Max(PreviousLocation.Y, CurrentLocation.Y) + HorizontalThreshold;

			bNearTrace = !(SweepMaxX < TrailMinX || SweepMinX > TrailMaxX || SweepMaxY < TrailMinY || SweepMinY > TrailMaxY);
		}

		const bool bHitLethal = bNearTrace
			&& SweepIntersectsTrace(TestPositions, PreviousLocation, CurrentLocation, HorizontalThreshold, VerticalThreshold);

		if (bHitLethal)
		{
			if (!bInvulnerable)
			{
				Tripper = Candidate;
				continue;   // Resolved; keep looping only to refresh the remaining positions.
			}

			// An intended exception. ALWAYS logged, and always naming WHICH source saved the
			// carrier, because "I dashed through it and nothing happened" must have an answer in
			// the log — and with two sources, "it was invulnerable" is not an answer.
			const TCHAR* NoKillReason =
				(bParry && bPassWindow) ? TEXT("PARRIED (v3 3), and also inside a pass window (spec 4)")
				: bParry                ? TEXT("PARRIED - trace protected, carrier lives (v3 3)")
				:                         TEXT("pass window invulnerable (spec 4)");

			UE_LOG(LogTraceGame, Log,
				TEXT("[TRACEDASH] %s dashed through %s's trace: NO KILL - %s. parry=%d (%.3fs left) passWindow=%d points=%d lethal=%d emitting=%d"),
				*GetNameSafe(Candidate), *GetNameSafe(Holder), NoKillReason,
				bParry ? 1 : 0, GetParryWindowRemaining(), bPassWindow ? 1 : 0,
				TrailPoints.Items.Num(), TestPositions.Num(), bEmitting ? 1 : 0);
			continue;
		}

		// Did they cross the stub that is neither drawn nor lethal? If this ever fires the visible
		// state and the lethal state have drifted apart, which is the whole bug class this pass
		// exists to close — so it is reported at Log, not at Verbose.
		if (ExemptPositions.Num() > 1
			&& SweepIntersectsTrace(ExemptPositions, PreviousLocation, CurrentLocation, HorizontalThreshold, VerticalThreshold))
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[TRACEDASH] %s dashed through the NON-DRAWN head stub of %s's trace: NO KILL (emitter footprint, %.0fuu). points=%d lethal=%d"),
				*GetNameSafe(Candidate), *GetNameSafe(Holder),
				static_cast<double>(Settings.TrailRadius), TrailPoints.Items.Num(), TestPositions.Num());
		}
	}

	// Applied outside every loop above: this kills, which re-enters the component and mutates
	// TrailPoints.Items and PreviousLocations.
	if (Tripper != nullptr)
	{
		// parry=0 passWindow=0 is printed on the KILL line too, and it is not noise: the claim under
		// test is a conditional, so the log has to carry the negative case as explicitly as the
		// positive one. "Every dash through a trace, and whether a parry was active" is one grep.
		UE_LOG(LogTraceGame, Log,
			TEXT("[TRACEDASH] %s dashed through %s's trace: KILL. parry=0 passWindow=0 points=%d lethal=%d emitting=%d (residual trace: %s)"),
			*GetNameSafe(Tripper), *GetNameSafe(Holder),
			TrailPoints.Items.Num(), TestPositions.Num(), bEmitting ? 1 : 0,
			bEmitting ? TEXT("no") : TEXT("yes - laid before a turnover"));

		ApplyTrailTrip(Holder, Tripper);
	}
}

bool UTraceTrailComponent::SweepIntersectsTrace(const TArray<FVector>& Positions, const FVector& PreviousLocation,
	const FVector& CurrentLocation, double HorizontalThreshold, double VerticalThreshold) const
{
	if (Positions.Num() == 0)
	{
		return false;
	}

	const double HorizontalThresholdSquared = HorizontalThreshold * HorizontalThreshold;

	const FVector SweepStart(PreviousLocation.X, PreviousLocation.Y, 0.0);
	const FVector SweepEnd(CurrentLocation.X, CurrentLocation.Y, 0.0);

	// A single point is tested as a zero-length segment rather than skipped. The old code needed
	// two testable points before ANYTHING was lethal, which — stacked on the head exemption — meant
	// a freshly formed trace was drawn but harmless for its first few points. SegmentDistToSegment
	// handles a degenerate segment correctly, so there is no reason for that hole to exist.
	const int32 LastSegment = FMath::Max(0, Positions.Num() - 2);

	for (int32 SegmentIndex = 0; SegmentIndex <= LastSegment; ++SegmentIndex)
	{
		const FVector& TrailStart = Positions[SegmentIndex];
		const FVector& TrailEnd = Positions[FMath::Min(SegmentIndex + 1, Positions.Num() - 1)];

		const FVector FlatTrailStart(TrailStart.X, TrailStart.Y, 0.0);
		const FVector FlatTrailEnd(TrailEnd.X, TrailEnd.Y, 0.0);

		// Returns void — the closest point on each segment, not the distance.
		FVector ClosestOnSweep = FVector::ZeroVector;
		FVector ClosestOnTrail = FVector::ZeroVector;
		FMath::SegmentDistToSegmentSafe(SweepStart, SweepEnd, FlatTrailStart, FlatTrailEnd, ClosestOnSweep, ClosestOnTrail);

		if (FVector::DistSquared(ClosestOnSweep, ClosestOnTrail) > HorizontalThresholdSquared)
		{
			continue;
		}

		// Recover where along each segment the closest approach happened so we can compare
		// heights there (the flattened test threw the Z away).
		const double SweepAlpha = SegmentAlpha(SweepStart, SweepEnd, ClosestOnSweep);
		const double TrailAlpha = SegmentAlpha(FlatTrailStart, FlatTrailEnd, ClosestOnTrail);
		const double ToucherZ = FMath::Lerp(PreviousLocation.Z, CurrentLocation.Z, SweepAlpha);
		const double TrailZ = FMath::Lerp(TrailStart.Z, TrailEnd.Z, TrailAlpha);

		if (FMath::Abs(ToucherZ - TrailZ) > VerticalThreshold)
		{
			continue;
		}

		return true;
	}

	return false;
}

void UTraceTrailComponent::ApplyTrailTrip(ATraceCharacter* Holder, ATraceCharacter* Tripper)
{
	if (Holder == nullptr || Tripper == nullptr)
	{
		return;
	}

	// Resolve both controllers first: the first Kill() may unpossess the pawn we would otherwise
	// ask for a controller afterwards.
	AController* TripperController = Tripper->GetController();
	AController* HolderController = Holder->GetController();

	/** Cause tag reported to the GameMode / kill feed for a trace death. */
	static const FName TrailDeathCause(TEXT("Trail"));

	const ETrailLethality Lethality = UTraceSettings::Get().TrailLethality;

	UE_LOG(LogTraceGame, Log, TEXT("Trace broken: %s dashed through %s's trace (lethality %d)"),
		*GetNameSafe(Tripper), *GetNameSafe(Holder), static_cast<int32>(Lethality));

	if (Lethality == ETrailLethality::KillsCarrier || Lethality == ETrailLethality::KillsBoth)
	{
		// Kill(), never ApplyDamage(): the holder is shielded against damage by design, and the
		// trace is the one thing that gets through. This is the whole point of the mechanic.
		//
		// TripperController is what carries the §2 transfer: ATraceCore listens to this health
		// component's OnDeath and hands the Core to whoever is credited here. "The core transfers
		// to the enemy who breaks your trace" is implemented by this argument.
		if (UTraceHealthComponent* HolderHealth = Holder->Health)
		{
			HolderHealth->Kill(TripperController, TrailDeathCause);
		}
	}

	if (Lethality == ETrailLethality::KillsToucher || Lethality == ETrailLethality::KillsBoth)
	{
		if (UTraceHealthComponent* TripperHealth = Tripper->Health)
		{
			TripperHealth->Kill(HolderController, TrailDeathCause);
		}
	}
}

void UTraceTrailComponent::GatherTrackedCharacters(TArray<ATraceCharacter*>& OutCharacters) const
{
	OutCharacters.Reset();

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (const ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
	{
		for (const TWeakObjectPtr<ATraceCharacter>& WeakCharacter : GameMode->GetTrackedCharacters())
		{
			if (ATraceCharacter* TraceChar = WeakCharacter.Get())
			{
				OutCharacters.Add(TraceChar);
			}
		}

		if (OutCharacters.Num() > 0)
		{
			return;
		}
	}

	// Fallback: the GameMode is the fast path, but the signature mechanic must not silently stop
	// working if registration is incomplete (e.g. a character spawned outside the normal flow).
	for (TActorIterator<ATraceCharacter> It(World); It; ++It)
	{
		if (ATraceCharacter* TraceChar = *It)
		{
			OutCharacters.Add(TraceChar);
		}
	}
}

float UTraceTrailComponent::GetServerTimeSeconds() const
{
	if (const UWorld* World = GetWorld())
	{
		// GetServerWorldTimeSeconds() is already replicated and smoothed; do not hand-roll a
		// clock. It returns double on 5.3+, so narrow explicitly.
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return static_cast<float>(GameState->GetServerWorldTimeSeconds());
		}
		return static_cast<float>(World->GetTimeSeconds());
	}
	return 0.f;
}

ATraceCharacter* UTraceTrailComponent::GetOwnerCharacter() const
{
	return Cast<ATraceCharacter>(GetOwner());
}


// =================================================================================================
// Visuals (client + listen server)
//
// SPEC §3: "a blur created where your character model has passed through".
// SPEC v4 §2: "change the look of the trace to match the new mannequin models, rather than the old
// cylinder models."
//
// TWO LAYERS, BOTH DERIVED FROM THE SAME LETHAL POINT SET.
//
// LAYER 1 — THE GHOSTS. Every GhostSpacing uu along the path, a UPoseableMeshComponent wearing the
// holder's own Mannequin is frozen at the pose the holder was actually in and left standing there,
// rendered on the unlit neon material in the team colour. This is the literal reading of the design
// language and it is what the player actually looks at: it is unmistakably a person, so "somebody
// ran through here" is legible at a glance instead of inferred from a shape.
//
// LAYER 2 — THE SMEAR. Along every lethal SEGMENT, a two-part body-shaped extrusion (body + a hot
// head band) at roughly half the ghosts' brightness. Two jobs, in this order of importance:
//
//   (a) IT KEEPS THE DRAWING CONTINUOUS WHERE THE GHOSTS ARE DISCRETE. This is not decoration. The
//       kill volume is a continuous swept polyline; the ghosts are ~220uu apart. If the gaps between
//       them were empty a player would look at one, reasonably conclude they could run it, and die —
//       and the trip test is the mechanic the whole game turns on. The smear is drawn from the same
//       ComputeLastLethalIndex() polyline the server kills with, at the same TrailRadius/TrailHeight,
//       one element per segment with a TrailRadius overlap at interior joints so the outside of a
//       corner is covered. There is no gap anywhere along the lethal set. See RebuildSmear().
//   (b) It is what makes the thing a BLUR rather than a row of statues.
//
// WHY POSEABLE, AND WHY NOT ONE PER POINT. A trail point every 60uu for 2s is ~27 points; a skeletal
// mesh component per point would be 27 anim-graph evaluations per trace per frame for a set of poses
// that are frozen by definition. UPoseableMeshComponent has no anim instance at all: the bones are
// written once by CopyPoseFromSkeletalComponent, RefreshBoneTransforms is called once by hand, tick
// is then disabled, and the component is a plain static skinned draw for the rest of its life.
// Sampling every 220uu instead of every 60uu takes the count from 27 to ~8 at a walk. Measured
// component counts are in EnsurePoseGhost / MaxPooledSmearElements.
//
// Two properties were preserved deliberately, because both were measured and both are load-bearing:
//
//   1. The smear spans EXACTLY the lethal volume (TrailRadius wide, TrailHeight tall, along the
//      lethal polyline). What you dash at is what kills you.
//   2. The HEAD band is the hottest part of the smear, and it sits at first-person eye height. A
//      previous pass established that a trace has to be readable from a player's own eyeline at
//      range; a bright element at that height is the feature that survives the projection when
//      everything else collapses edge-on. The mannequins do not replace it — a mannequin seen
//      edge-on at 6000uu is two pixels — so it stays.
//
// WHERE VISUAL AND COLLISION DO NOT AGREE EXACTLY, stated plainly rather than buried:
//   * THE SMEAR NO LONGER SPANS THE FULL HEIGHT OF THE KILL VOLUME, WHILE GHOSTS ARE ON. It draws
//     the bottom 38% at the exact lethal width, plus a thin ribbon at eye height, and leaves
//     72-162uu open so the opaque mannequins are not buried inside it (see the cross-section
//     constants for the screenshot that forced this). Horizontally and along the path it is still
//     exactly the lethal set, which is what a dash is judged against; vertically, 72-162uu is
//     covered by the after-images alone and between two of them there is a body-shaped hole in the
//     middle of the column. A player cannot use it — the trip test's vertical threshold is 95 plus
//     their own 88uu capsule half-height, so anything short of leaving the ground entirely is still
//     inside it — but it IS a place where the drawing is thinner than the rule.
//
//     WITH GHOSTS OFF THE HOLE CLOSES. RebuildSmear() spans the body band over the full lethal
//     height whenever AreCharacterGhostsEnabled() is false, because the hole is only ever safe when
//     there are mannequins standing in it. That covers both reachable cases — missing character art
//     or a material without MATUSAGE_SkeletalMesh, and MaxTraceGhosts set to 0, which the settings
//     panel advertises as "0 = smear only". Dimming alone would have left that setting drawing a
//     trace with a body-shaped gap through the middle of it.
//   * The GHOSTS are a person-shaped mesh standing inside a cylinder-shaped kill volume, so the
//     lethal volume is strictly WIDER than a mannequin's arms and legs. That is the safe direction
//     (you die slightly before you touch the silhouette, never after), and the floor band draws the
//     true width continuously underneath, so the boundary is never invisible.
//   * A ghost's POSE is snapshotted when it is placed, which is up to one TrailRadius of travel
//     (~0.06s at 800uu/s) after the point it stands on was laid. Its position is the point's; only
//     the limb positions are that fraction of a second late.
//   * On a segment where the holder changed height (a jump), the smear covers the union of both
//     ends' vertical bands rather than the exact lerp, so it over-draws by at most half the height
//     change at the segment ends. Over-drawing a lethal boundary is the safe direction.
// =================================================================================================

void UTraceTrailComponent::UpdateVisuals()
{
	if (VisualSuppressUntilTime > 0.f)
	{
		const UWorld* World = GetWorld();
		const float Now = World != nullptr ? static_cast<float>(World->GetTimeSeconds()) : 0.f;
		if (World == nullptr || TrailPoints.Items.Num() == 0 || Now >= VisualSuppressUntilTime)
		{
			VisualSuppressUntilTime = 0.f;
		}
		else
		{
			return;
		}
	}

	// Team colour resolves late on clients (PlayerState replication), so re-check every tick.
	// UpdateTeamColor() early-outs unless the colour actually changed.
	UpdateTeamColor();

	// Change detection. bVisualsDirty is the primary signal (set by the replication callbacks and
	// by every server-side mutation); the head/tail/count comparison is a cheap backstop so the
	// visuals keep tracking even if a fast-array callback is ever missed.
	const int32 PointCount = TrailPoints.Items.Num();
	const FVector Head = PointCount > 0 ? FVector(TrailPoints.Items.Last().Location) : FVector::ZeroVector;
	const FVector Tail = PointCount > 0 ? FVector(TrailPoints.Items[0].Location) : FVector::ZeroVector;
	const bool bInvulnerable = IsTraceInvulnerable();

	// The PARRY is its own change detector — see bLastVisualParry's declaration. Without it, a parry
	// raised during an open pass window would leave every other term unchanged and the trace would
	// never go red.
	const bool bParryVisual = IsParryVisuallyActive();

	if (!bVisualsDirty
		&& PointCount == LastVisualPointCount
		&& bInvulnerable == bLastVisualInvulnerable
		&& bParryVisual == bLastVisualParry
		&& bEmitting == bLastVisualEmitting
		&& Head.Equals(LastVisualHead, 0.01)
		&& Tail.Equals(LastVisualTail, 0.01))
	{
		return;
	}

	bVisualsDirty = false;
	LastVisualPointCount = PointCount;
	LastVisualHead = Head;
	LastVisualTail = Tail;
	bLastVisualInvulnerable = bInvulnerable;
	bLastVisualParry = bParryVisual;

	// bEmitting is in the comparison above because ComputeLastLethalIndex() reads it: the frame a
	// holder stops emitting, the stub under their feet becomes lethal and must become visible with
	// it. Nothing else about the point set changes on that frame, so without this the rebuild would
	// wait for the next expiry and the trace would be lethal-but-invisible in between.
	bLastVisualEmitting = bEmitting;

	RebuildVisuals();
}

void UTraceTrailComponent::RebuildVisuals()
{
	// THE OTHER HALF OF THE INVARIANT. What is drawn is exactly the lethal set — not the whole
	// point array — so a player can never be shown a segment that would not have killed them.
	// ComputeLastLethalIndex() is the same function the server's trip test runs off, and it reads
	// only replicated state, so this client's answer is the server's answer.
	const int32 LethalPointCount = ComputeLastLethalIndex() + 1;
	if (LethalPointCount <= 0)
	{
		HideSmearFrom(0);
		ClearGhostRecords();
		return;
	}

	CacheMeshMetrics();

	// v3 §3: the parry's brightness step, ABOVE the pass window's, and checked first so that a parry
	// raised during a pass window still reads as a parry. The colour half of the same state is
	// applied in UpdateTeamColor(); the two are driven by the same predicate so they cannot
	// disagree — a red trace at pass-window brightness (or a cyan one at parry brightness) would be
	// a state the player has no name for.
	//
	// It is passed to BOTH layers. The parry reddens and brightens the ENTIRE trace (v3 §3), and
	// "entire" now includes the mannequins — a red smear under white-hot ghosts would be a third
	// state nobody has a name for.
	float InvulnerableScale = 1.f;
	if (IsParryVisuallyActive())
	{
		InvulnerableScale = TraceParry::GetGlowScale();
	}
	else if (IsPassWindowInvulnerable())
	{
		InvulnerableScale = GhostInvulnerableGlowScale;
	}

	// --- Hide the newest stretch of the trace from the holder's OWN eyes -------------------------
	//
	// Holding the Core is what puts the camera into third person, and third person parks it
	// ThirdPersonArmLength straight back down the path the holder just walked — which is exactly
	// where this component is placing unlit emissive geometry. Raising the camera above the trace
	// (see ATraceCharacter::GetThirdPersonPivotZ) stops it being INSIDE it, but the freshest
	// after-images are still hot surfaces a few tens of uu below the lens: they blew out the bottom
	// third of the frame and, worse, drowned the player's own character in glare.
	//
	// SetOwnerNoSee hides a primitive from ONE viewer — the one whose view target owns it — so this
	// costs every other player nothing. They still see the whole trace, including the part its own
	// holder cannot, and the LETHAL VOLUME IS UNTOUCHED: trip resolution runs off TrailPoints, never
	// off what happens to be rendered. A holder cannot trip their own trace anyway, so nothing is
	// being hidden that its owner could act on.
	//
	// It applies to the mannequin ghosts for the same reason and by the same rule: a posed copy of
	// YOURSELF, hot, 100uu from your own third-person lens is the single worst version of the
	// whiteout this exists to prevent.
	int32 FirstOwnerHiddenPoint = LethalPointCount;   // == LethalPointCount means "hide nothing"
	if (const ATraceCharacter* OwnerCharacter = GetOwnerCharacter())
	{
		const APlayerController* OwnerPC = Cast<APlayerController>(OwnerCharacter->GetController());
		if (OwnerPC != nullptr && OwnerPC->IsLocalPlayerController())
		{
			double DistanceFromHead = 0.0;
			FirstOwnerHiddenPoint = 0;
			for (int32 Index = LethalPointCount - 1; Index >= 0; --Index)
			{
				if (DistanceFromHead >= static_cast<double>(FMath::Max(0.f, GOwnerNearHideDistance)))
				{
					FirstOwnerHiddenPoint = Index + 1;
					break;
				}
				if (Index > 0)
				{
					DistanceFromHead += FVector::Dist(TrailPoints.Items[Index - 1].Location, TrailPoints.Items[Index].Location);
				}
			}
		}
	}

	RebuildSmear(LethalPointCount, FirstOwnerHiddenPoint, InvulnerableScale);
	RebuildPoseGhosts(LethalPointCount, FirstOwnerHiddenPoint, InvulnerableScale);
}

// -------------------------------------------------------------------------------------------------
// LAYER 2 (drawn first, read second): THE CONTINUOUS SMEAR.
//
// ONE ELEMENT PER LETHAL SEGMENT, spanning that segment exactly. This is the part of the visual that
// is allowed to make a promise about where the kill volume is, so it is built the same way the trip
// test evaluates it: along the polyline TrailPoints[0..LastLethal], TrailRadius to either side,
// TrailHeight tall. Interior joints get one TrailRadius of overlap at each end, which fills the wedge
// on the outside of a corner; the two OUTER ends get none, so the smear never extends past the first
// or last lethal point into trace that would not kill.
// -------------------------------------------------------------------------------------------------

void UTraceTrailComponent::RebuildSmear(int32 LethalPointCount, int32 FirstOwnerHiddenPoint, float InvulnerableScale)
{
	if (CylinderMesh == nullptr || LethalPointCount <= 0)
	{
		HideSmearFrom(0);
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// Derived from the lethal volume, never chosen. If TrailRadius/TrailHeight are retuned the smear
	// moves with them, because it is the drawn statement of where they are.
	const double Width = FMath::Max(1.0, 2.0 * static_cast<double>(Settings.TrailRadius));
	const double Height = FMath::Max(1.0, static_cast<double>(Settings.TrailHeight));
	const double JointOverlap = FMath::Max(1.0, static_cast<double>(Settings.TrailRadius));

	// THE SMEAR STEPS OUT OF THE MANNEQUINS' WAY ONLY WHEN THERE ARE MANNEQUINS — and that has to
	// govern the GEOMETRY, not just the brightness.
	//
	// The body band normally covers the bottom 38% of the lethal height and the head ribbon sits at
	// eye level, leaving 72-162uu open so the opaque mannequins are not buried inside an opaque
	// slab. That hole is only safe because the after-images fill it. With ghosts off there is
	// nothing to fill it, and dimming alone would leave a trace with a body-shaped gap through the
	// middle of the column — on the one mechanic the whole game turns on.
	//
	// Ghosts are off in two reachable cases, neither of them exotic: no character art / no
	// skeletal-capable material (a packaged build that missed MATUSAGE_SkeletalMesh), and
	// UTraceSettings::MaxTraceGhosts set to 0, which the settings panel offers as "0 = smear only".
	// "Smear only" has to mean a solid trace, so in that case the body band spans the FULL lethal
	// height at full strength: the pre-v4 look, and perfectly playable.
	const bool bGhostsOn = AreCharacterGhostsEnabled();
	const float LayerScale = bGhostsOn ? ResolvedSmearGlowScale() : 1.f;

	const double BodyCentreFrac = bGhostsOn ? SmearBodyCentreFrac : 0.5;
	const double BodyHeightFrac = bGhostsOn ? SmearBodyHeightFrac : 1.0;

	const float Lifetime = GetTraceLifetimeSeconds();
	const float Now = GetServerTimeSeconds();

	// A one-point trace is a real, lethal, degenerate segment (SweepIntersectsTrace tests it as one),
	// so it gets one stub element rather than nothing at all.
	const int32 SegmentCount = FMath::Max(1, LethalPointCount - 1);

	// If there are more segments than the pool can draw, drop the OLDEST. Truncating from the wrong
	// end would hide the freshest stretch — the part an approaching enemy is judging.
	const int32 FirstSegment = FMath::Max(0, SegmentCount - MaxPooledSmearElements);

	int32 Placed = 0;

	for (int32 SegmentIndex = FirstSegment; SegmentIndex < SegmentCount; ++SegmentIndex)
	{
		if (!EnsureSmearElement(Placed))
		{
			break;   // Pool cap hit — draw what we have.
		}

		const FVector SegStart = TrailPoints.Items[SegmentIndex].Location;
		const FVector SegEnd = (LethalPointCount > 1)
			? FVector(TrailPoints.Items[SegmentIndex + 1].Location)
			: SegStart;

		FVector Along = SegEnd - SegStart;
		Along.Z = 0.0;
		const double PlanarLength = Along.Size();
		const FVector Direction = (PlanarLength > 1.0) ? (Along / PlanarLength) : FVector::ZeroVector;
		const FRotator Facing = (PlanarLength > 1.0) ? Direction.Rotation() : FRotator::ZeroRotator;

		// Overlap at INTERIOR joints only. The two ends of the whole trace stay flush with the first
		// and last lethal point, so nothing is drawn beyond the polyline the server kills along.
		const double BackOverlap = (SegmentIndex > FirstSegment) ? JointOverlap : 0.0;
		const double ForwardOverlap = (SegmentIndex + 1 < LethalPointCount - 1) ? JointOverlap : 0.0;

		// The floor is one full body width, so a one-point (degenerate but lethal) trace draws a
		// blob you can see rather than a sliver you cannot.
		const double ElementLength = FMath::Max(Width, PlanarLength + BackOverlap + ForwardOverlap);
		const FVector ElementCentre = (SegStart + SegEnd) * 0.5
			+ Direction * ((ForwardOverlap - BackOverlap) * 0.5);

		// A segment the holder JUMPED along is lethal over a band that slides from one end's height to
		// the other's. Covering the union of the two bands over-draws by at most half the height change
		// at the segment's ends, which is the safe direction: the boundary is never drawn smaller than
		// it is.
		const double SpanHeight = Height + FMath::Abs(SegEnd.Z - SegStart.Z);

		// Age from the NEWER endpoint: a segment is as young as its leading edge.
		const float Age = FMath::Max(0.f, Now - TrailPoints.Items[FMath::Min(SegmentIndex + 1, LethalPointCount - 1)].BirthServerTime);
		const float Remaining = FMath::Clamp(1.f - (Age / FMath::Max(0.01f, Lifetime)), 0.f, 1.f);
		const float FadeScale = FMath::Lerp(GhostOldestGlowScale, 1.f, Remaining);

		const bool bHideFromOwner = ((SegmentIndex + 1) >= FirstOwnerHiddenPoint);

		for (int32 Part = 0; Part < PartsPerSmear; ++Part)
		{
			const int32 SlotIndex = Placed * PartsPerSmear + Part;
			UStaticMeshComponent* Piece = SmearMeshes[SlotIndex];
			if (Piece == nullptr)
			{
				continue;
			}

			const bool bIsHead = (Part == PartHead);
			const double CentreFrac = bIsHead ? SmearHeadCentreFrac : BodyCentreFrac;
			const double HeightFrac = bIsHead ? SmearHeadHeightFrac : BodyHeightFrac;
			const double WidthFrac = bIsHead ? SmearHeadWidthFrac : SmearBodyWidthFrac;
			const float BaseGlow = (bIsHead ? SmearHeadGlow : SmearBodyGlow) * LayerScale;

			const FVector DesiredSize(
				ElementLength,
				Width * WidthFrac,
				SpanHeight * HeightFrac);

			const FVector Scale(
				DesiredSize.X / (2.0 * CylinderHalfSize.X),
				DesiredSize.Y / (2.0 * CylinderHalfSize.Y),
				DesiredSize.Z / (2.0 * CylinderHalfSize.Z));

			// Measured from the BOTTOM of the lethal band, so the two parts tile it exactly.
			const FVector PartCentre = ElementCentre
				+ FVector(0.0, 0.0, -SpanHeight * 0.5 + SpanHeight * CentreFrac);

			// Corrects for a source mesh whose pivot is not at its bounds centre, so we never have
			// to assume anything about the engine primitives' authoring.
			const FVector PivotCorrection = Facing.RotateVector(CylinderPivotOffset * Scale);

			Piece->SetWorldLocationAndRotation(PartCentre - PivotCorrection, Facing);
			Piece->SetWorldScale3D(Scale);
			Piece->SetVisibility(true);

			// Guarded: SetOwnerNoSee dirties the render state, and this runs as the trace grows.
			if (Piece->bOwnerNoSee != bHideFromOwner)
			{
				Piece->SetOwnerNoSee(bHideFromOwner);
			}

			// The intended brightness of this piece, BEFORE the camera-proximity fade. Recorded rather
			// than pushed directly, because ApplyProximityGlowFade() runs every frame and needs to
			// know what full brightness means for this piece without re-deriving the whole rebuild.
			if (SmearBaseGlow.Num() <= SlotIndex)
			{
				SmearBaseGlow.SetNumZeroed(SlotIndex + 1);
				SmearAppliedGlowScale.SetNumZeroed(SlotIndex + 1);
			}
			SmearBaseGlow[SlotIndex] = BaseGlow * FadeScale * InvulnerableScale;

			if (bTrailMaterialIsNeon)
			{
				if (UMaterialInstanceDynamic* Material = SmearMaterials[SlotIndex])
				{
					// Push full brightness and let the proximity pass pull it down. Resetting the
					// remembered scale forces that pass to re-evaluate this piece, which it must:
					// the piece has just been moved somewhere else entirely.
					Material->SetScalarParameterValue(TEXT("Glow"), SmearBaseGlow[SlotIndex]);
					SmearAppliedGlowScale[SlotIndex] = 1.f;
				}
			}
		}

		++Placed;
	}

	HideSmearFrom(Placed);
}

// -------------------------------------------------------------------------------------------------
// LAYER 1: THE POSED MANNEQUIN AFTER-IMAGES (spec v4 §2).
//
// The pool is a DEQUE aligned index-for-index with GhostRecords: PoseGhosts[i] holds the pose of
// GhostRecords[i], and anything past GhostRecords.Num() is a free, hidden component waiting to be
// re-posed. Retiring the oldest ghost therefore rotates its component to the BACK of the pool rather
// than shifting every record onto a different component — which matters, because the pose lives in
// the component and a shift would silently re-pair every surviving record with somebody else's pose.
//
// A ghost is created only when the head of the LETHAL polyline has moved GhostSpacing uu clear of the
// last one, and retired the instant its BirthServerTime falls off the back of TrailPoints. Both
// endpoints of the ghost chain are therefore pinned to the point array, so the after-images can never
// extend past the trace, outlive it, or survive a turnover.
// -------------------------------------------------------------------------------------------------

void UTraceTrailComponent::RebuildPoseGhosts(int32 LethalPointCount, int32 FirstOwnerHiddenPoint, float InvulnerableScale)
{
	if (!AreCharacterGhostsEnabled() || LethalPointCount <= 0)
	{
		ClearGhostRecords();
		return;
	}

	// --- 0. The pool is skinned to ONE asset -----------------------------------------------------
	//
	// ATraceCharacter resolves its Mannequin lazily and falls back to a primitive stand-in when the
	// import is missing, so "which mesh am I" can change after this component has already built
	// ghosts. A pool skinned to the previous asset would keep drawing the wrong body, so it is torn
	// down and rebuilt rather than reused.
	if (USkeletalMeshComponent* CurrentSource = GetGhostSourceMesh())
	{
		if (PoseGhosts.Num() > 0 && GhostSkinnedAsset.Get() != CurrentSource->GetSkinnedAsset())
		{
			for (UPoseableMeshComponent* Stale : PoseGhosts)
			{
				if (Stale != nullptr)
				{
					Stale->DestroyComponent();
				}
			}
			PoseGhosts.Reset();
			PoseGhostMaterials.Reset();
			PoseGhostBaseGlow.Reset();
			PoseGhostAppliedGlowScale.Reset();
			GhostRecords.Reset();
		}
	}

	const float Now = GetServerTimeSeconds();
	const float Lifetime = GetTraceLifetimeSeconds();

	// --- 1. Retire ------------------------------------------------------------------------------
	//
	// A ghost dies exactly when the stretch of trace it stands on does. Comparing birth times against
	// the OLDEST SURVIVING POINT (rather than running an independent lifetime here) means the ghost
	// chain and the lethal polyline are trimmed by one decision made in one place — the server's
	// expiry loop in ServerUpdateTrail — so they cannot end up ending in different places.
	const float OldestPointBirth = TrailPoints.Items[0].BirthServerTime;

	int32 RetireCount = 0;
	while (RetireCount < GhostRecords.Num()
		&& GhostRecords[RetireCount].BirthServerTime < OldestPointBirth - 0.001f)
	{
		++RetireCount;
	}

	// --- 2. Make room for a new one -------------------------------------------------------------
	const int32 MaxGhosts = ResolvedMaxGhosts();

	const int32 LastLethalIndex = LethalPointCount - 1;
	const FVector HeadLocation = TrailPoints.Items[LastLethalIndex].Location;

	const double Spacing = static_cast<double>(ResolvedGhostSpacing());
	const bool bWantNew = (GhostRecords.Num() - RetireCount) <= 0
		|| FVector::Dist(GhostRecords.Last().PathLocation, HeadLocation) >= Spacing;

	if (bWantNew && (GhostRecords.Num() - RetireCount) >= MaxGhosts)
	{
		// At the cap the OLDEST goes, never the newest: the fresh end of the trace is the end an
		// approaching enemy is judging, and the smear still covers the tail either way.
		RetireCount = FMath::Min(GhostRecords.Num(), RetireCount + 1);
	}

	if (RetireCount > 0)
	{
		// Rotate the retired components to the back of the pool, keeping record <-> component pairing
		// intact for every survivor. Bounded by MaxGhosts (<= 64) so the shifting is trivial.
		for (int32 Rotation = 0; Rotation < RetireCount; ++Rotation)
		{
			if (PoseGhosts.Num() > 0)
			{
				UPoseableMeshComponent* Recycled = PoseGhosts[0];
				if (Recycled != nullptr)
				{
					Recycled->SetVisibility(false);
				}
				PoseGhosts.RemoveAt(0);
				PoseGhosts.Add(Recycled);
			}
			if (PoseGhostMaterials.Num() > 0)
			{
				UMaterialInstanceDynamic* RecycledMaterial = PoseGhostMaterials[0];
				PoseGhostMaterials.RemoveAt(0);
				PoseGhostMaterials.Add(RecycledMaterial);
			}
			if (PoseGhostBaseGlow.Num() > 0)
			{
				PoseGhostBaseGlow.RemoveAt(0);
				PoseGhostBaseGlow.Add(0.f);
			}
			if (PoseGhostAppliedGlowScale.Num() > 0)
			{
				PoseGhostAppliedGlowScale.RemoveAt(0);
				PoseGhostAppliedGlowScale.Add(1.f);
			}
		}

		GhostRecords.RemoveAt(0, RetireCount);
	}

	// --- 3. Drop a new after-image ---------------------------------------------------------------
	if (bWantNew && GhostRecords.Num() < MaxGhosts)
	{
		USkeletalMeshComponent* SourceMesh = GetGhostSourceMesh();
		const ATraceCharacter* OwnerCharacter = GetOwnerCharacter();
		const int32 NewIndex = GhostRecords.Num();

		if (SourceMesh != nullptr && OwnerCharacter != nullptr && EnsurePoseGhost(NewIndex))
		{
			FTraceGhostRecord Record;
			Record.BirthServerTime = TrailPoints.Items[LastLethalIndex].BirthServerTime;
			Record.PathLocation = HeadLocation;

			// The ghost stands on the TRAIL POINT, wearing the mesh's offset from its actor. The
			// point is an actor location and so is GetActorLocation(), so the difference is exactly
			// the mesh's placement on the capsule (feet on the capsule bottom, yaw -90) with no
			// assumption about either constant baked in here.
			const FTransform SourceTransform = SourceMesh->GetComponentTransform();
			const FVector MeshOffset = SourceTransform.GetLocation() - OwnerCharacter->GetActorLocation();

			Record.MeshTransform = FTransform(
				SourceTransform.GetRotation(),
				HeadLocation + MeshOffset,
				SourceTransform.GetScale3D());

			if (UPoseableMeshComponent* Ghost = PoseGhosts[NewIndex])
			{
				Ghost->SetWorldTransform(Record.MeshTransform);

				// The whole cost of a ghost, paid once: copy the bones, evaluate them once by hand,
				// and never touch it again. RefreshBoneTransforms is normally driven from the
				// component's own tick; calling it here is what lets that tick stay switched off.
				Ghost->CopyPoseFromSkeletalComponent(SourceMesh);
				Ghost->RefreshBoneTransforms(nullptr);

				Record.bPosed = true;
			}

			GhostRecords.Add(Record);
		}
	}

	// --- 4. Colour, brightness, visibility --------------------------------------------------------
	for (int32 GhostIndex = 0; GhostIndex < GhostRecords.Num(); ++GhostIndex)
	{
		if (!PoseGhosts.IsValidIndex(GhostIndex))
		{
			break;
		}

		UPoseableMeshComponent* Ghost = PoseGhosts[GhostIndex];
		if (Ghost == nullptr)
		{
			continue;
		}

		const FTraceGhostRecord& Record = GhostRecords[GhostIndex];

		// Age fade. Never to zero: an old after-image is exactly as lethal as a new one, so it has
		// to stay clearly visible — this is a "cooling" cue, not a disappearance.
		const float Age = FMath::Max(0.f, Now - Record.BirthServerTime);
		const float Remaining = FMath::Clamp(1.f - (Age / FMath::Max(0.01f, Lifetime)), 0.f, 1.f);
		const float FadeScale = FMath::Lerp(GhostOldestGlowScale, 1.f, Remaining);

		// Same rule as the smear, expressed against the same point array: a ghost is hidden from its
		// own holder when the point it stands on is inside the near-hide window.
		const bool bHideFromOwner = FirstOwnerHiddenPoint < LethalPointCount
			&& Record.BirthServerTime >= TrailPoints.Items[FirstOwnerHiddenPoint].BirthServerTime;

		Ghost->SetVisibility(true);
		if (Ghost->bOwnerNoSee != bHideFromOwner)
		{
			Ghost->SetOwnerNoSee(bHideFromOwner);
		}

		if (PoseGhostBaseGlow.Num() <= GhostIndex)
		{
			PoseGhostBaseGlow.SetNumZeroed(GhostIndex + 1);
			PoseGhostAppliedGlowScale.SetNumZeroed(GhostIndex + 1);
		}
		PoseGhostBaseGlow[GhostIndex] = ResolvedGhostGlow() * FadeScale * InvulnerableScale;

		if (bTrailMaterialIsNeon && PoseGhostMaterials.IsValidIndex(GhostIndex))
		{
			if (UMaterialInstanceDynamic* Material = PoseGhostMaterials[GhostIndex])
			{
				Material->SetScalarParameterValue(TEXT("Glow"), PoseGhostBaseGlow[GhostIndex]);
				PoseGhostAppliedGlowScale[GhostIndex] = 1.f;
			}
		}
	}

	ReleasePoseGhostsFrom(GhostRecords.Num());
}

USkeletalMeshComponent* UTraceTrailComponent::GetGhostSourceMesh() const
{
	const ATraceCharacter* OwnerCharacter = GetOwnerCharacter();
	if (OwnerCharacter == nullptr)
	{
		return nullptr;
	}

	USkeletalMeshComponent* SourceMesh = OwnerCharacter->GetMesh();
	if (SourceMesh == nullptr || SourceMesh->GetSkinnedAsset() == nullptr)
	{
		// No Mannequin on this machine (Scripts/import-mannequin.sh has not run, or the character
		// fell back to its primitive stand-in). The smear covers for it at full brightness.
		return nullptr;
	}

	return SourceMesh;
}

bool UTraceTrailComponent::AreCharacterGhostsEnabled() const
{
	if (ResolvedMaxGhosts() <= 0)
	{
		return false;
	}

	if (GetGhostSourceMesh() == nullptr)
	{
		return false;
	}

	// ------------------------------------------------------------------------------------------
	// THE MATERIAL GATE, and why it is a hard one.
	//
	// A material without MATUSAGE_SkeletalMesh is not "a bit wrong" on a skinned draw: the renderer
	// silently substitutes the engine's default grey checkerboard, because the skeletal vertex
	// factory permutation of its shader was never compiled. That would put a herd of untinted grey
	// mannequins on the pitch in place of a trace — strictly worse than having no ghosts at all, and
	// worse still because it would look like a bug in the trace rather than a missing flag.
	//
	// The editor normally repairs this on first use, but ONLY when it is not running as a game
	// (UMaterial::SetMaterialUsage checks FApp::IsGame), and every run of this project is -game. So
	// the flag has to be saved into M_TraceNeon by Scripts/generate_content.py, and this asks rather
	// than assumes. Logged once, at Log, because a silently-disabled feature is exactly how the last
	// two "that mechanic is dead" false alarms happened here.
	// ------------------------------------------------------------------------------------------
	if (GhostMaterialState == EGhostMaterialState::Unknown)
	{
		UTraceTrailComponent* Mutable = const_cast<UTraceTrailComponent*>(this);

		const bool bUsable = TrailMaterial != nullptr
			&& TrailMaterial->CheckMaterialUsage_Concurrent(MATUSAGE_SkeletalMesh);

		Mutable->GhostMaterialState = bUsable ? EGhostMaterialState::Usable : EGhostMaterialState::Unusable;

		UE_LOG(LogTraceGame, Log,
			TEXT("[TRACEGHOST] Character after-images %s. material=%s skeletalUsage=%d neon=%d "
			     "(spec v4 2: the trace is drawn as posed Mannequins; without skeletal usage on the "
			     "trace material they are switched off and the continuous smear carries the trace alone)."),
			bUsable ? TEXT("ENABLED") : TEXT("DISABLED"),
			*GetNameSafe(TrailMaterial), bUsable ? 1 : 0, bTrailMaterialIsNeon ? 1 : 0);
	}

	return GhostMaterialState == EGhostMaterialState::Usable;
}

void UTraceTrailComponent::ApplyProximityGlowFade()
{
	// ------------------------------------------------------------------------------------------
	// WHY THIS EXISTS
	//
	// This project has a measured, named defect: point-blank whiteout from unlit emissive surfaces.
	// The arena's version of it was fixed by STANDOFF — geometry gained a pawn-blocking shell so an
	// eye can never get close enough (see ATraceArenaBuilder::AddPawnStandoff). That fix is not
	// available here and must not be: walking through a trace has to stay free, because only a DASH
	// may trip it (spec §3). So a player can and will stand with their eye INSIDE an after-image.
	//
	// M_TraceNeon is unlit: it emits Color * Glow with no distance term whatsoever, so a head piece
	// at Glow 4.2 arrives at the lens at full intensity and fills the frame. A full-field walk
	// measured two frames out of 108 blown past 40% (52.7% and 43.0%), and BOTH were a player
	// standing inside somebody else's trace — after the arena fix, the only remaining source.
	//
	// So bound the SOLID ANGLE instead of the standoff: attenuate a piece's emissive by how close
	// the local camera is to it. Far away nothing changes at all, which is the whole point — the
	// trace's readability across the arena is a measured win and is untouched.
	//
	// STRICTLY LOCAL AND STRICTLY COSMETIC. It reads this machine's camera, writes only this
	// machine's material instances, and the lethal volume is TrailPoints — which this function does
	// not touch and the trip test never renders. Two players standing in the same trace see their
	// own fade and die to exactly the same geometry.
	// ------------------------------------------------------------------------------------------
	if (!bTrailMaterialIsNeon || (SmearMeshes.Num() == 0 && PoseGhosts.Num() == 0))
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// The local viewpoint. On a listen host with nine bots there is exactly one, and a dedicated
	// server never reaches here.
	const APlayerController* LocalPC = GEngine != nullptr ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	if (LocalPC == nullptr || LocalPC->PlayerCameraManager == nullptr)
	{
		return;
	}
	const FVector CameraLocation = LocalPC->PlayerCameraManager->GetCameraLocation();

	// Both pools take the same treatment: a posed mannequin is exactly as unlit, exactly as emissive
	// and exactly as standable-inside as a smear slab, so exempting it would reinstate the whiteout
	// this function exists to prevent — from the one piece of geometry now closest to the lens.
	auto FadePool = [&CameraLocation](
		const auto& Pieces,
		const TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
		const TArray<float>& BaseGlow,
		TArray<float>& AppliedScale)
	{
		const int32 SlotCount = FMath::Min(Pieces.Num(), Materials.Num());
		for (int32 Slot = 0; Slot < SlotCount; ++Slot)
		{
			UMeshComponent* Piece = Pieces[Slot];
			UMaterialInstanceDynamic* Material = Materials[Slot];
			if (Piece == nullptr || Material == nullptr || !Piece->IsVisible())
			{
				continue;
			}
			if (!BaseGlow.IsValidIndex(Slot))
			{
				continue;
			}

			// Distance to the piece's SURFACE, not its centre: these are wide, flat slabs, and a
			// centre distance would report a torso as far away at the exact moment its face is
			// against the lens. The bounds are already computed for culling, so this is free.
			const FBoxSphereBounds& LocalBounds = Piece->Bounds;
			const double SurfaceDistance = FMath::Max(0.0,
				FVector::Dist(CameraLocation, LocalBounds.Origin) - LocalBounds.SphereRadius);

			float Scale = 1.f;
			if (SurfaceDistance < ProximityFadeFarDistance)
			{
				// Smooth, so a piece does not pop as the player walks past it. The floor is not zero:
				// an after-image you are standing inside is exactly as lethal as one across the
				// field, and a player must still be able to see that they are in it.
				const float T = FMath::Clamp(
					static_cast<float>((SurfaceDistance - ProximityFadeNearDistance)
						/ FMath::Max(1.0, ProximityFadeFarDistance - ProximityFadeNearDistance)),
					0.f, 1.f);
				Scale = FMath::Lerp(ProximityFadeMinScale, 1.f, FMath::InterpEaseIn(0.f, 1.f, T, 2.f));
			}

			// Only touch the material when the change is visible. Without this every pooled piece
			// would dirty its render state every frame for the entire life of the trace.
			if (!AppliedScale.IsValidIndex(Slot))
			{
				AppliedScale.SetNumZeroed(Slot + 1);
			}
			if (FMath::IsNearlyEqual(AppliedScale[Slot], Scale, 0.02f))
			{
				continue;
			}

			AppliedScale[Slot] = Scale;
			Material->SetScalarParameterValue(TEXT("Glow"), BaseGlow[Slot] * Scale);
		}
	};

	// Generic over the array's element type: TArray<TObjectPtr<UStaticMeshComponent>> and
	// TArray<TObjectPtr<UPoseableMeshComponent>> are unrelated types, and copying either into a
	// widened scratch array every frame to share one signature would be a real per-frame cost paid
	// for a compile-time problem.
	FadePool(SmearMeshes, SmearMaterials, SmearBaseGlow, SmearAppliedGlowScale);
	FadePool(PoseGhosts, PoseGhostMaterials, PoseGhostBaseGlow, PoseGhostAppliedGlowScale);
}

bool UTraceTrailComponent::EnsureSmearElement(int32 ElementIndex)
{
	if (ElementIndex < 0 || ElementIndex >= MaxPooledSmearElements)
	{
		return false;
	}

	const int32 RequiredNum = (ElementIndex + 1) * PartsPerSmear;
	if (SmearMeshes.Num() >= RequiredNum)
	{
		return true;
	}

	// The pool only ever grows one whole element at a time, in order, so the interleaving
	// (body / head) can never slip.
	if (SmearMeshes.Num() != ElementIndex * PartsPerSmear)
	{
		return false;
	}

	for (int32 Part = 0; Part < PartsPerSmear; ++Part)
	{
		UMaterialInstanceDynamic* Material = nullptr;
		UStaticMeshComponent* Piece = CreatePooledMesh(CylinderMesh.Get(), Material);

		// A null entry is skipped harmlessly in RebuildSmear, whereas a SHORT array would silently
		// pair one element's head band with the next one's body.
		SmearMeshes.Add(Piece);
		SmearMaterials.Add(Material);
	}

	return true;
}

bool UTraceTrailComponent::EnsurePoseGhost(int32 GhostIndex)
{
	const int32 MaxGhosts = ResolvedMaxGhosts();
	if (GhostIndex < 0 || GhostIndex >= MaxGhosts)
	{
		return false;
	}

	// Free components live past GhostRecords.Num(), so the pool is usually already big enough and
	// this is a no-op — creating a UPoseableMeshComponent is the expensive part of a ghost and it is
	// paid at most MaxGhosts times per trace, ever.
	if (PoseGhosts.Num() > GhostIndex)
	{
		return true;
	}
	if (PoseGhosts.Num() != GhostIndex)
	{
		return false;
	}

	AActor* OwnerActor = GetOwner();
	USkeletalMeshComponent* SourceMesh = GetGhostSourceMesh();
	if (OwnerActor == nullptr || SourceMesh == nullptr)
	{
		return false;
	}

	UPoseableMeshComponent* Ghost = NewObject<UPoseableMeshComponent>(OwnerActor, NAME_None, RF_Transient);
	if (Ghost == nullptr)
	{
		return false;
	}

	// Mobility must be set before registration.
	Ghost->SetMobility(EComponentMobility::Movable);
	Ghost->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ghost->SetCollisionProfileName(TEXT("NoCollision"));
	Ghost->SetGenerateOverlapEvents(false);
	Ghost->SetCanEverAffectNavigation(false);
	Ghost->SetCastShadow(false);
	Ghost->bReceivesDecals = false;
	Ghost->SetIsReplicated(false);   // Purely local cosmetics, rebuilt from TrailPoints.
	Ghost->SetVisibility(false);

	// THE PERFORMANCE CLAIM, in two lines. A frozen pose has nothing to advance, so the component
	// never ticks; RebuildPoseGhosts calls RefreshBoneTransforms() by hand exactly once, when the
	// pose is written. Everything after that is a static skinned draw.
	Ghost->PrimaryComponentTick.bCanEverTick = false;
	Ghost->SetComponentTickEnabled(false);

	Ghost->RegisterComponent();
	Ghost->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);

	// Critical: the trace is laid in WORLD space and must not follow the holder around. Absolute
	// transforms keep the components in the actor's hierarchy (so they are cleaned up with it) while
	// making them ignore the parent transform entirely.
	Ghost->SetAbsolute(true, true, true);

	// Allocates the bone arrays and RequiredBones, which CopyPoseFromSkeletalComponent needs.
	Ghost->SetSkinnedAssetAndUpdate(SourceMesh->GetSkinnedAsset(), /*bReinitPose=*/true);
	GhostSkinnedAsset = SourceMesh->GetSkinnedAsset();

	const int32 ForcedLOD = ResolvedGhostForcedLOD();
	if (ForcedLOD > 0)
	{
		Ghost->SetForcedLOD(ForcedLOD);
	}

	// ONE MID ON EVERY SLOT. The Mannequin ships two materials (body and a logo decal sheet); an
	// after-image is not a character, it is a light, so every slot gets the same unlit neon instance
	// and the ghost renders as a flat team-coloured silhouette rather than as a second player.
	UMaterialInstanceDynamic* Material = nullptr;
	if (TrailMaterial != nullptr)
	{
		Material = UMaterialInstanceDynamic::Create(TrailMaterial, this);
		if (Material != nullptr)
		{
			const int32 SlotCount = Ghost->GetNumMaterials();
			for (int32 Slot = 0; Slot < SlotCount; ++Slot)
			{
				Ghost->SetMaterial(Slot, Material);
			}

			if (!bTrailMaterialIsNeon)
			{
				Material->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
			}
			if (bColorApplied)
			{
				Material->SetVectorParameterValue(TEXT("Color"), AppliedColor);
				Material->SetVectorParameterValue(TEXT("BaseColor"), AppliedColor);
			}
		}
	}

	PoseGhosts.Add(Ghost);
	PoseGhostMaterials.Add(Material);
	PoseGhostBaseGlow.Add(0.f);
	PoseGhostAppliedGlowScale.Add(1.f);

	UE_LOG(LogTraceGame, Verbose, TEXT("[TRACEGHOST] %s grew the after-image pool to %d posed mannequins."),
		*GetNameSafe(OwnerActor), PoseGhosts.Num());

	return true;
}

UStaticMeshComponent* UTraceTrailComponent::CreatePooledMesh(UStaticMesh* SourceMesh, UMaterialInstanceDynamic*& OutMaterial)
{
	OutMaterial = nullptr;

	AActor* Owner = GetOwner();
	if (Owner == nullptr || SourceMesh == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* NewMesh = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
	if (NewMesh == nullptr)
	{
		return nullptr;
	}

	// Mobility must be set before registration.
	NewMesh->SetMobility(EComponentMobility::Movable);
	NewMesh->SetStaticMesh(SourceMesh);
	NewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	NewMesh->SetCollisionProfileName(TEXT("NoCollision"));
	NewMesh->SetGenerateOverlapEvents(false);
	NewMesh->SetCanEverAffectNavigation(false);
	NewMesh->SetCastShadow(false);
	NewMesh->bReceivesDecals = false;
	NewMesh->SetIsReplicated(false);   // Purely local cosmetics, rebuilt from TrailPoints.
	NewMesh->SetVisibility(false);

	NewMesh->RegisterComponent();
	NewMesh->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);

	// Critical: the trace is laid in WORLD space and must not follow the holder around. Absolute
	// transforms keep the components in the actor's hierarchy (so they are cleaned up with it)
	// while making them ignore the parent transform entirely.
	NewMesh->SetAbsolute(true, true, true);

	if (TrailMaterial != nullptr)
	{
		OutMaterial = NewMesh->CreateDynamicMaterialInstance(0, TrailMaterial);
	}

	if (OutMaterial != nullptr)
	{
		if (!bTrailMaterialIsNeon)
		{
			// BasicShapeMaterial fallback: lit, no Glow, so the best available approximation is a
			// bright matte albedo. It will not bloom and it will not read as light — that is the
			// cost of not having run Scripts/generate_content.py.
			OutMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
		}

		// Newly created components need the colour that the pool already agreed on.
		if (bColorApplied)
		{
			OutMaterial->SetVectorParameterValue(TEXT("Color"), AppliedColor);
			OutMaterial->SetVectorParameterValue(TEXT("BaseColor"), AppliedColor);
		}
	}

	return NewMesh;
}

void UTraceTrailComponent::HideSmearFrom(int32 FirstElementIndex)
{
	for (int32 Index = FMath::Max(0, FirstElementIndex) * PartsPerSmear; Index < SmearMeshes.Num(); ++Index)
	{
		if (UStaticMeshComponent* Piece = SmearMeshes[Index])
		{
			Piece->SetVisibility(false);
		}
	}
}

void UTraceTrailComponent::ReleasePoseGhostsFrom(int32 FirstGhostIndex)
{
	for (int32 Index = FMath::Max(0, FirstGhostIndex); Index < PoseGhosts.Num(); ++Index)
	{
		if (UPoseableMeshComponent* Ghost = PoseGhosts[Index])
		{
			Ghost->SetVisibility(false);
		}
	}
}

void UTraceTrailComponent::ClearGhostRecords()
{
	// The components are NOT destroyed: they keep their skinned asset and their material instance and
	// go back to being free slots at the back of the pool. A turnover happens several times a minute
	// and rebuilding twenty poseable mesh components each time would be the one genuinely expensive
	// thing in this file.
	GhostRecords.Reset();
	ReleasePoseGhostsFrom(0);
}

void UTraceTrailComponent::UpdateTeamColor()
{
	FLinearColor Desired = TraceTeamColor(ETraceTeam::None);
	if (const ATraceCharacter* TraceChar = GetOwnerCharacter())
	{
		Desired = TraceTeamColor(TraceChar->GetTeam());
	}

	// -------------------------------------------------------------------------------------------
	// v3 §3: "It also makes the ENTIRE trace turn red for the duration of the parry."
	//
	// Every after-image, not the ones near the dasher — the tell has to be legible from anywhere on
	// the field, to the carrier (who is looking forward, away from their own trace) and to the enemy
	// (who is already mid-dash and has one frame to abort). One colour on every MID does that.
	//
	// AND IT REVERTS BY CONSTRUCTION, which is the point of doing it here rather than on an event.
	// This function is POLLED from UpdateVisuals() every single tick and simply asks "what colour
	// should the trace be right now"; when the window lapses the answer goes back to the team colour
	// on the very next tick with nothing having to remember to undo anything. A stuck red trace is
	// the obvious failure mode of this mechanic, and an event-driven tint is how you get one — a
	// dropped RPC, a holder who dies mid-parry, a Core that changes hands inside the window, and the
	// "turn it off" edge never arrives. There is no such edge here.
	//
	// The early-out below still means the MIDs are touched only on an actual change, so polling
	// costs one FLinearColor comparison per tick.
	// -------------------------------------------------------------------------------------------
	if (IsParryVisuallyActive())
	{
		Desired = TraceParry::GetTintColor();
	}

	Desired.A = 1.f;

	if (bColorApplied && Desired.Equals(AppliedColor, 0.001f))
	{
		return;
	}

	AppliedColor = Desired;
	bColorApplied = true;

	// "Color" is the real vector parameter on both M_TraceNeon and BasicShapeMaterial; "BaseColor" is
	// a defensive second guess and is a silent no-op if the parameter does not exist.
	//
	// The head keeps the team colour rather than going white. A white element would read as "generic
	// hazard"; the whole point is that a glance tells you WHOSE trace it is, and therefore whether
	// dashing through it kills their holder or does nothing at all.
	//
	// BOTH POOLS. "The ENTIRE trace turns red" (v3 §3) has to include the mannequins, or the parry
	// produces a red smear full of team-coloured ghosts and the tell stops being a single readable
	// state. The ghosts share this one loop precisely so they cannot be forgotten.
	for (UMaterialInstanceDynamic* Material : SmearMaterials)
	{
		if (Material != nullptr)
		{
			Material->SetVectorParameterValue(TEXT("Color"), AppliedColor);
			Material->SetVectorParameterValue(TEXT("BaseColor"), AppliedColor);
		}
	}

	for (UMaterialInstanceDynamic* Material : PoseGhostMaterials)
	{
		if (Material != nullptr)
		{
			Material->SetVectorParameterValue(TEXT("Color"), AppliedColor);
			Material->SetVectorParameterValue(TEXT("BaseColor"), AppliedColor);
		}
	}
}

void UTraceTrailComponent::CacheMeshMetrics()
{
	if (bMeshMetricsCached)
	{
		return;
	}

	if (CylinderMesh != nullptr)
	{
		const FBoxSphereBounds LocalBounds = CylinderMesh->GetBounds();
		CylinderHalfSize = LocalBounds.BoxExtent;
		CylinderPivotOffset = LocalBounds.Origin;
	}

	// Never divide by zero, whatever the assets turn out to be.
	CylinderHalfSize.X = FMath::Max(CylinderHalfSize.X, 1.0);
	CylinderHalfSize.Y = FMath::Max(CylinderHalfSize.Y, 1.0);
	CylinderHalfSize.Z = FMath::Max(CylinderHalfSize.Z, 1.0);

	bMeshMetricsCached = true;
}

#if !UE_BUILD_SHIPPING
namespace
{
	/**
	 * Trace.Trail.DebugLookBack [Seconds] [OnSeconds] [OffSeconds]
	 *
	 * Periodically spins the local player 180 degrees so an automated capture can photograph THEIR OWN
	 * TRACE from in front of it.
	 *
	 * It exists for exactly the reason Trace.DebugTakeCore does, and the reason is worth writing down
	 * because it cost this pass real time: the trace forms BEHIND the holder, and holding the Core is
	 * what puts the camera 450uu behind them looking FORWARD. So the one player guaranteed to have a
	 * trace is the one player who structurally cannot see it, and an unattended screenshot harness has
	 * no hands with which to turn round. Every capture of a carrier comes back as a picture of the
	 * arena in front of them. Pair this with Trace.DebugTakeCore and Trace.Trail.OwnerNearHideDistance
	 * and a headless run can photograph the after-images head-on.
	 *
	 * IT PULSES rather than holding, for the same reason Trace.DebugCrouch does: the control rotation
	 * is also the movement basis, so a permanent 180 makes the player walk backwards down their own
	 * trace and stop laying new one. On/off means the trace keeps being drawn AND gets photographed.
	 *
	 * Purely a camera aid. It writes the control rotation and nothing else — no gameplay state, no
	 * trail state, and it is compiled out of Shipping.
	 */
	FAutoConsoleCommand CmdTrailDebugLookBack(
		TEXT("Trace.Trail.DebugLookBack"),
		TEXT("Trace.Trail.DebugLookBack [Seconds] [OnSeconds] [OffSeconds] — pulse the local player's view "
		     "180 degrees so an automated capture can photograph their own trace. Camera only."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Seconds = (Args.Num() > 0) ? FMath::Max(1.f, FCString::Atof(*Args[0])) : 120.f;
			const float OnSeconds = (Args.Num() > 1) ? FMath::Max(0.2f, FCString::Atof(*Args[1])) : 2.5f;
			const float OffSeconds = (Args.Num() > 2) ? FMath::Max(0.2f, FCString::Atof(*Args[2])) : 4.0f;

			double Elapsed = 0.0;
			double PhaseStart = 0.0;
			bool bLookingBack = false;
			bool bLogged = false;

			// Registered at period 0 and self-timed: a non-zero ticker period was measured NOT to fire
			// at all through this project's -ExecCmds path. See TraceCharacter.cpp's anim probe.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[Elapsed, PhaseStart, bLookingBack, bLogged, Seconds, OnSeconds, OffSeconds](float DeltaTime) mutable -> bool
			{
				Elapsed += DeltaTime;

				UWorld* World = nullptr;
				if (GEngine != nullptr)
				{
					for (const FWorldContext& Context : GEngine->GetWorldContexts())
					{
						if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
							&& Context.World() != nullptr)
						{
							World = Context.World();
							break;
						}
					}
				}

				APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
				APawn* ViewPawn = (PC != nullptr) ? PC->GetPawn() : nullptr;
				if (ViewPawn == nullptr)
				{
					return (Elapsed < Seconds);   // Wait for a pawn; the map may still be loading.
				}

				if (!bLogged)
				{
					bLogged = true;
					UE_LOG(LogTraceGame, Log,
						TEXT("[TRACELOOKBACK] Armed for %.0fs: %.1fs looking back, %.1fs looking forward."),
						Seconds, OnSeconds, OffSeconds);
				}

				const double PhaseLength = bLookingBack ? OnSeconds : OffSeconds;
				if ((Elapsed - PhaseStart) >= PhaseLength)
				{
					PhaseStart = Elapsed;
					bLookingBack = !bLookingBack;

					if (bLookingBack)
					{
						FRotator Look = ViewPawn->GetActorRotation();
						Look.Yaw += 180.f;
						Look.Pitch = -8.f;   // Slightly down: the trace stands on the floor.
						Look.Roll = 0.f;
						PC->SetControlRotation(Look);

						UE_LOG(LogTraceGame, Log, TEXT("[TRACELOOKBACK] Looking back down the trace (t=%.1fs)."),
							Elapsed);
					}
				}
				else if (bLookingBack)
				{
					// Hold the heading against the walk harness's steering, which would otherwise turn
					// the player round again inside the very window we are trying to photograph.
					FRotator Look = PC->GetControlRotation();
					Look.Pitch = -8.f;
					Look.Roll = 0.f;
					PC->SetControlRotation(Look);
				}

				return (Elapsed < Seconds);
			}), 0.f);
		}));
}
#endif // !UE_BUILD_SHIPPING

void UTraceTrailComponent::DestroyVisualPool()
{
	for (UStaticMeshComponent* Piece : SmearMeshes)
	{
		if (Piece != nullptr)
		{
			Piece->DestroyComponent();
		}
	}

	for (UPoseableMeshComponent* Ghost : PoseGhosts)
	{
		if (Ghost != nullptr)
		{
			Ghost->DestroyComponent();
		}
	}

	SmearMeshes.Reset();
	SmearMaterials.Reset();
	SmearBaseGlow.Reset();
	SmearAppliedGlowScale.Reset();

	PoseGhosts.Reset();
	PoseGhostMaterials.Reset();
	PoseGhostBaseGlow.Reset();
	PoseGhostAppliedGlowScale.Reset();
	GhostRecords.Reset();

	LastVisualPointCount = -1;
	bColorApplied = false;
}
