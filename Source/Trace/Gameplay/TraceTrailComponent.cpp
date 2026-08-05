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

// Spec v6 §1: Trace.Trail.PerfAB reads the SAME numbers `stat unit` displays, out of the same
// FStatUnitData the overlay is drawn from, so a headless run can quote game vs render vs GPU without
// anybody having to read them off a screenshot.
//
// Via the viewport client and NOT via RenderCore's GGameThreadTime / RHIGetGPUFrameCycles(), which
// would be the direct route: those live in the RenderCore and RHI modules, which are include-path
// dependencies of Engine but not LINK dependencies of this one, so using them fails at the very end
// with "Undefined symbols for architecture arm64" — exactly the trap Trace.Build.cs already warns
// about for Sockets and ApplicationCore. FStatUnitData is Engine, and Engine is already linked.
//
// The cost of that choice, stated so nobody is surprised by a table of zeroes: these fields are only
// filled while `stat unit` is actually enabled. PerfAB turns it on itself and says so if it is off.
#include "Engine/GameViewportClient.h"         // GetStatUnitData()
#include "UnrealClient.h"                      // FStatUnitData
#include "UObject/UObjectIterator.h"           // TObjectIterator (scene primitive census)

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

	// =============================================================================================
	// SPEC v7 §§1-3 — THE TRACE IS A LENGTH, AND IT IS THINNER AND SHALLOWER
	//
	// §1 THE EXPLOIT THIS CLOSES, in the user's words: "When a player stands still, their trace is
	//    gone and they can't be killed." The trace is the ONLY counterplay to a shielded carrier, so
	//    a 2-second timer meant a carrier who simply stopped moving was, after two seconds,
	//    unkillable. The timer is GONE — not layered under a length cap, gone — and points now leave
	//    the tail for exactly one reason: a new point at the head pushed the path past its maximum
	//    LENGTH. Stand still and nothing is spawned, so nothing is retired, so the whole trace stays.
	//
	// §2 THE VALUE. TrailLifetime (2.0s) x WalkSpeed (800uu/s) = 1600uu was the old walking trace;
	//    the spec asks for that minus 25%, i.e. 1200uu.
	//
	//    DERIVED from those two settings rather than written as a bare 1200, and that is deliberate:
	//    TraceSettings.h and Config/DefaultGame.ini are not this slice's files, so a new property
	//    would have to be requested from the integrator, and until it landed the number would be a
	//    console variable that the settings panel does not show — "a knob on the page that lies",
	//    which this project has already been bitten by. Binding to two knobs the panel ALREADY shows
	//    and already live-edits keeps it tunable in PIE today. Trace.Trail.MaxLength overrides it
	//    outright for a headless run, and the report asks for a first-class TrailMaxLengthUU.
	//
	// §3 THE CROSS-SECTION. "Get rid of the top and bottom third of the trace ... also reduce the
	//    width, it doesn't need to be the full width of the player model." Height 190 -> 63 (the
	//    middle third, centred on the trail point, which IS mid-model), radius 45 -> 22.5.
	//
	//    THE LETHAL VOLUME AND THE DRAWN VOLUME COME OUT OF THE SAME TWO FUNCTIONS. Both the trip
	//    test and every renderer arm read GetTraceTrailRadius()/GetTraceTrailHeight() and nothing in
	//    this file reads UTraceSettings::TrailRadius/TrailHeight any more, so the standing invariant
	//    ("once a trace segment is visible it is lethal", and its converse) cannot be broken by
	//    editing one of them. Shrinking the drawing without shrinking the trip volume would be the
	//    "I dashed past it and died anyway" bug; the reverse is trace you cannot see that kills you.
	//
	//    THESE TWO ARE REPLACEMENTS, NOT SCALES, so they cannot double-apply if and when the
	//    integrator moves 22.5/63 into TraceSettings + DefaultGame.ini. At that point set both
	//    defaults below to -1 and the settings take over unchanged. That handover is in the report.
	// =============================================================================================

	float GTrailMaxLength = -1.f;

	FAutoConsoleVariableRef CVarTrailMaxLength(
		TEXT("Trace.Trail.MaxLength"),
		GTrailMaxLength,
		TEXT("OVERRIDE for the maximum LENGTH of a trace in uu (spec v7 1-2: 1200). Points leave the "
		     "tail only when new trace at the head pushes the path past this - never because time "
		     "passed, so a stationary carrier keeps their whole trace. Negative (default) = use "
		     "UTraceSettings::TrailMaxLengthUU, or derive it from TrailLifetime x WalkSpeed x 0.75 "
		     "if that is left at or below zero."),
		ECVF_Default);

	// INTEGRATED: -1 now, because 22.5 and 63 live in UTraceSettings + DefaultGame.ini where the
	// settings panel can reach them. These stay as CONSOLE overrides only. See GetTraceTrailRadius().
	float GTrailRadiusOverride = -1.f;

	FAutoConsoleVariableRef CVarTrailRadius(
		TEXT("Trace.Trail.Radius"),
		GTrailRadiusOverride,
		TEXT("HALF the trace's width in uu, LETHAL AND DRAWN TOGETHER (spec v7 3: 22.5, down from the "
		     "player model's 45). Negative = fall back to UTraceSettings::TrailRadius. Changing this "
		     "moves the kill volume and the ribbon by the same amount, on purpose."),
		ECVF_Default);

	float GTrailHeightOverride = -1.f;

	FAutoConsoleVariableRef CVarTrailHeight(
		TEXT("Trace.Trail.Height"),
		GTrailHeightOverride,
		TEXT("The trace's height in uu, LETHAL AND DRAWN TOGETHER, centred on the carrier's mid-model "
		     "(spec v7 3: 63, the middle third of the old 190). Negative = fall back to "
		     "UTraceSettings::TrailHeight."),
		ECVF_Default);

	/**
	 * v7 §7: whether a receiving machine repairs the order of the replicated point array.
	 *
	 * On by default — off is not a quality setting, it is the BEFORE arm. Setting it to 0 reproduces
	 * the reported tether on demand, which is how the mechanism was confirmed rather than assumed.
	 */
	int32 GClientOrderFix = 1;

	FAutoConsoleVariableRef CVarClientOrderFix(
		TEXT("Trace.Trail.ClientOrderFix"),
		GClientOrderFix,
		TEXT("1 (default) = a receiving client puts the delta-replicated trail points back into path "
		     "order after the fast array's RemoveAtSwap scrambles them (spec v7 7). 0 reproduces the "
		     "bug: the far end of the trace snaps to the carrier. Clients only - authority never "
		     "reorders."),
		ECVF_Default);

	/** Upper bound on pooled SMEAR elements (one per lethal segment). 2.0s at 60uu spacing is ~27. */
	constexpr int32 MaxPooledSmearElements = 96;

	/** Meshes per smear element, interleaved: [0] body (legs+torso), [1] the hot head band. */
	constexpr int32 PartsPerSmear = 2;
	constexpr int32 PartHead = 1;

	// =============================================================================================
	// SPEC v6 §1 + §2 — WHICH RENDERER, AND THE A/B LEVER THAT MEASURES THE DIFFERENCE
	//
	// 1 = THE RIBBON. One continuous swept rectangle along a Catmull-Rom smoothing of the lethal
	//     point set. This is the shipping look and the default (§2).
	// 0 = THE LEGACY spec v4 §2 renderer: up to MaxTraceGhosts posed Mannequins
	//     (UPoseableMeshComponent, one CopyPoseFromSkeletalComponent each) plus a two-part cylinder
	//     smear. It exists ONLY as the BEFORE arm of Trace.Trail.PerfAB. The user's report was
	//     "1/6 the fps", which is a number, so the answer has to be a number measured on the same
	//     scene in the same process — not "the new one feels lighter".
	// 2 = NOTHING DRAWN. The third arm, and the one that makes the measurement honest: it bounds the
	//     ENTIRE cost of this component's visuals. If arm 2 does not recover the frame rate then the
	//     trace was never the problem, and saying so plainly is worth more than a fix.
	//
	// The lethal volume is TrailPoints and is identical on all three arms. Arm 2 breaks the
	// visible == lethal invariant by drawing nothing at all, which is why it is a measurement arm and
	// not a quality setting.
	// =============================================================================================
	int32 GTrailRenderer = 1;

	FAutoConsoleVariableRef CVarTrailRenderer(
		TEXT("Trace.Trail.Renderer"),
		GTrailRenderer,
		TEXT("1 = the curved ribbon (spec v6 2, default). 0 = the legacy posed-Mannequin + smear "
		     "renderer, kept ONLY as the before arm of Trace.Trail.PerfAB. 2 = draw nothing, which "
		     "bounds the whole cost of the trace's visuals. Cosmetic - the kill volume is TrailPoints "
		     "on every arm."),
		ECVF_Default);

	/**
	 * uu of arc length per ribbon element (spec v6 §2).
	 *
	 * This is the ONLY perf dial the ribbon has, and it is a straight trade of components for
	 * smoothness. At the default TrailPointSpacing of 60 and a 2.0s lifetime, a walking carrier's
	 * trace is ~1600uu, so 60 gives ~27 elements — HALF the component count of the legacy smear
	 * (two meshes per segment) and with none of the twenty skinned Mannequins beside it.
	 *
	 * Note the ribbon is resampled along a Catmull-Rom curve THROUGH the points, so a step equal to
	 * the point spacing is not the same thing as drawing the chords: the samples land on the smoothed
	 * curve, and the elements between them are oriented by it in full 3D (yaw AND pitch), which is
	 * what "curving through the air" means for a trace laid over a jump.
	 */
	float GRibbonStep = 60.f;

	FAutoConsoleVariableRef CVarRibbonStep(
		TEXT("Trace.Trail.RibbonStep"),
		GRibbonStep,
		TEXT("uu of arc length per element of the curved ribbon (spec v6 2). Lower = smoother and more "
		     "draw calls. Purely cosmetic - the kill volume is unchanged."),
		ECVF_Default);

	/**
	 * Upper bound on pooled RIBBON elements. Past it the STEP COARSENS instead of the ribbon being
	 * truncated, which is the whole difference between "a long trace looks chunkier" and "a long
	 * trace has a lethal stretch nobody can see". The invariant does not get a budget.
	 *
	 * 96 at the 60uu default is 5760uu of trace, which is longer than MaxTrailPoints x
	 * TrailPointSpacing at any dash speed this game has.
	 */
	constexpr int32 MaxRibbonElements = 96;

	/** Elements in the owner-only predicted-head ribbon. The stub is at most GPredictedHeadMaxLength. */
	constexpr int32 MaxPredictedRibbonElements = 12;

	/**
	 * ALTERNATING CROSS-SECTION INSET, and it is not a cosmetic nicety — it is the fix for the
	 * "noticeable sections" half of the user's report.
	 *
	 * Consecutive elements overlap by half a body width so the outside of a corner is never open (the
	 * same joint overlap the smear has always used). On a STRAIGHT stretch that puts two identically
	 * sized boxes' top and side faces exactly coplanar over the whole overlap — which is textbook
	 * Z-FIGHTING, and Z-fighting on an unlit emissive surface reads precisely as a flickering band at
	 * every single joint, i.e. as sections.
	 *
	 * Insetting every other element's cross-section by 0.6% (0.54uu of the 90uu width, 1.1uu of the
	 * 190uu height) makes the overlapping faces non-coplanar, so the depth test resolves them
	 * consistently and the ribbon reads as one surface. It is far below the width of a pixel at any
	 * range the trace is judged from, and it only ever makes the drawn thing SMALLER than the lethal
	 * volume by half a uu on alternate elements — inside the 34uu of tripper-capsule inflation the
	 * trip test already adds, so it cannot manufacture a graze.
	 */
	constexpr double RibbonAlternateInset = 0.994;

	/**
	 * Fraction of the LETHAL width the ribbon is drawn at. 1.0, and it should stay 1.0.
	 *
	 * It exists because "a rectangle which curves" and "a Tron path" pull in opposite directions on
	 * exactly one number: a Tron light wall is a thin sheet, and the volume that kills here is 90uu
	 * wide — a body width. Drawn at 1.0 the ribbon is a slab, which is honest; drawn thinner it looks
	 * more like a light wall and starts lying.
	 *
	 * SO THE DEFAULT IS THE HONEST ONE AND THE DIAL IS EXPOSED WITH THE COST WRITTEN ON IT. At scale
	 * S the drawn half-width is 45*S, while a dash is caught at 45 + the tripper's own capsule radius
	 * (~34) = ~79uu from the centreline. So S = 0.5 puts up to 56uu of lethal ground on each side of
	 * the ribbon that a player can graze without ever touching the thing they were shown. That is the
	 * "I dashed past it and died anyway" bug, bought back one uu at a time.
	 */
	float GRibbonWidthScale = 1.f;

	FAutoConsoleVariableRef CVarRibbonWidthScale(
		TEXT("Trace.Trail.RibbonWidthScale"),
		GRibbonWidthScale,
		TEXT("Fraction of the LETHAL width the ribbon is drawn at (spec v6 2). 1 (default) draws the "
		     "kill volume exactly. Below 1 makes it look more like a thin Tron light wall AND makes it "
		     "narrower than the thing that kills - see the comment for how much lethal ground that "
		     "hides. Never changes the kill volume itself, only what the player is shown of it."),
		ECVF_Default);

	/**
	 * Spec v6 §1: game-thread milliseconds every UTraceTrailComponent in the world spent on VISUALS
	 * this frame — the rebuild, the predicted head and the per-frame proximity pass.
	 *
	 * Accumulated unconditionally (one FPlatformTime::Seconds pair per component per frame, which is
	 * a few nanoseconds) and drained by Trace.Trail.PerfAB. It exists because the frame-time column
	 * alone cannot tell "the trace is expensive on the game thread" from "the trace is expensive on
	 * the GPU" from "the trace was never the problem", and those are three different conclusions with
	 * three different owners.
	 */
	double GTrailVisualMillisecondsThisFrame = 0.0;

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
	 * THE OWNER-ONLY NEAR CULL — and the single most important number in the v5 §2 bug report.
	 *
	 * WHAT IT USED TO BE, AND WHY THAT WAS THE REPORTED BUG. This was
	 * Trace.Trail.OwnerNearHideDistance, 850uu, measured ALONG THE PATH back from the carrier's feet:
	 * the freshest 850uu of a carrier's own trace was hidden from that carrier by SetOwnerNoSee. It is
	 * an owner-only effect, which is exactly the asymmetry the user reported — a bot's trace belongs
	 * to no local viewer, so nothing of it is ever hidden, while the human carrying the Core lost the
	 * newest ~1.4 seconds of their own trace. That is "my trace has a gap between me and the end of
	 * it, the most recent section is missing", verbatim, and it happens in a solo match with no
	 * networking involved at all.
	 *
	 * WHY IT WAS WRONG-SHAPED, not just too big. The thing it defends against is POINT-BLANK WHITEOUT:
	 * an unlit emissive slab arriving at the lens at full intensity. That is a function of how close a
	 * piece is to the CAMERA, not of how far back along the path it lies. Path distance is only a
	 * proxy for it, and a bad one: the third-person camera sits 450uu back down the path, so hiding by
	 * path distance necessarily hides the trace on BOTH sides of the lens — including the stretch at
	 * the carrier's feet, 450uu in FRONT of the camera, which cannot whiteout anything and is the
	 * exact stretch the player wanted to see.
	 *
	 * SO IT IS NOW A RADIUS AROUND THE LENS. A piece is hidden from its own carrier when its SURFACE
	 * comes within this many uu of the local camera. Measured from the current layout (camera 172uu
	 * above the carrier's actor centre and 450uu behind, TrailHeight 190):
	 *
	 *   piece under the lens : head band ~20uu from the camera surface, body band ~136uu   -> HIDDEN
	 *   piece at the feet    : head band ~384uu, body band ~411uu                          -> DRAWN
	 *
	 * 200 sits in the middle of a 250uu-wide separation, so the two cases cannot swap places under
	 * any reasonable retune, and the hysteresis below removes any chance of a piece flickering at the
	 * boundary. It also stays well inside ProximityFadeFarDistance (320), which means everything this
	 * culls had ALREADY been faded to ~0.1-0.3 of its brightness by ApplyProximityGlowFade: the cull
	 * removes geometry that was, by construction, nearly invisible anyway.
	 *
	 * When the carrier LOOKS BACK down their own trace — the moment they actually read it, and the
	 * moment this bug was reported from — the arm swings the camera to the far side of the pawn, so
	 * nothing on the trace is near the lens and the whole thing is drawn, feet included.
	 *
	 * Presentation only, on one machine, for one viewer. The lethal volume is TrailPoints and no line
	 * of the cull touches it.
	 */
	float GOwnerHideCameraRadius = 200.f;

	FAutoConsoleVariableRef CVarOwnerHideCameraRadius(
		TEXT("Trace.Trail.OwnerHideCameraRadius"),
		GOwnerHideCameraRadius,
		TEXT("uu from the LOCAL CAMERA within which a carrier's own trace is hidden from that carrier "
		     "(anti-whiteout). 0 disables the cull and leaves the proximity glow fade alone. Presentation "
		     "only - never changes the lethal volume, and never affects any other player's view. "
		     "Replaces Trace.Trail.OwnerNearHideDistance, which hid by distance along the path and was "
		     "the cause of the reported gap between the carrier and the end of their own trace."),
		ECVF_Default);

	/** Re-show hysteresis on the cull above, so a piece at the boundary cannot flicker. */
	constexpr double OwnerHideCameraHysteresis = 40.0;

	/**
	 * A SECOND, MUCH SMALLER owner-only hide, for the POSED MANNEQUINS ONLY, measured from the
	 * carrier's own body rather than from the camera.
	 *
	 * The trace forms exactly where the carrier has just been, and the third-person camera looks
	 * along that same line — so once the trace is allowed to reach the carrier's feet (which is the
	 * whole point of v5 §2), the newest after-images stand BETWEEN the lens and the carrier's own
	 * character, 100uu closer to the camera than the body they are copies of. An opaque, person-shaped,
	 * emissive duplicate of yourself drawn slightly larger than and directly on top of your own
	 * character is not a trace, it is a rendering bug with an explanation.
	 *
	 * The smear is deliberately NOT subject to this: it is the layer that carries continuity, it is
	 * built at the exact lethal width, and it is a knee-high band plus a thin eye-line ribbon rather
	 * than a body. It runs all the way to the carrier's feet, so the trace the player sees still
	 * reaches them — which is the reported bug — while the mannequins start one body-length back.
	 *
	 * 300 is a little over one ghost spacing (220), so the carrier loses at most the newest one or
	 * two after-images and every other player still sees all of them. 0 disables it entirely.
	 */
	float GOwnerGhostHideDistance = 300.f;

	FAutoConsoleVariableRef CVarOwnerGhostHideDistance(
		TEXT("Trace.Trail.OwnerGhostHideDistance"),
		GOwnerGhostHideDistance,
		TEXT("uu from the carrier's own body within which the posed-Mannequin after-images are hidden "
		     "from that carrier alone (spec v5 2). The continuous smear is unaffected and still reaches "
		     "their feet. 0 draws the ghosts right up to the body. Presentation only - never changes the "
		     "lethal volume, and never affects another player's view."),
		ECVF_Default);

	/**
	 * SPEC v5 §2: how far the OWNER-ONLY predicted head stub may reach ahead of the newest drawn point.
	 *
	 * It has to cover two things at once:
	 *   - the head-grace stub, which exists on every machine including a listen host: the newest
	 *     TrailRadius (45uu) of trace, plus the ungated head point itself, is deliberately neither
	 *     lethal nor drawn, so even the authority's own view of its own trace stops ~one body width
	 *     short of the carrier's feet;
	 *   - on a remote client, one further round trip of travel — the points the server has already
	 *     laid but this machine has not received. At 800uu/s and 120ms that is ~100uu; at dash speed
	 *     it is more.
	 *
	 * 400 covers both with room to spare. Beyond it the stub is NOT clamped, it is ABANDONED: a
	 * distance that large means a teleport, a respawn or a desync severe enough that a straight line
	 * from the last known point to the pawn would be an invention rather than an interpolation, and
	 * the honest thing to draw is the trace the machine actually knows about. Same reasoning, and the
	 * same order of magnitude, as MaxTrailSegmentLength below.
	 */
	float GPredictedHeadMaxLength = 400.f;

	FAutoConsoleVariableRef CVarPredictedHeadMaxLength(
		TEXT("Trace.Trail.PredictMaxLength"),
		GPredictedHeadMaxLength,
		TEXT("uu the owner-only predicted head stub may span (spec v5 2). 0 disables the prediction and "
		     "restores the gap between a carrier and the end of their own trace. Beyond this length the "
		     "stub is dropped entirely rather than clamped - see the comment."),
		ECVF_Default);

	/** Elements in the predicted-head pool: up to TrailHeadGracePoints real segments plus the stub. */
	constexpr int32 MaxPredictedSmearElements = 6;

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

	/**
	 * Uniform Catmull-Rom through P1 and P2, with P0/P3 as the neighbouring control points.
	 *
	 * Catmull-Rom rather than a Bezier fit because it INTERPOLATES its control points: every trail
	 * point the server laid is still exactly on the drawn centreline, so the ribbon cannot drift off
	 * the lethal polyline at the samples. Between them the curve can bulge outside the chord by up to
	 * ~1/8 of the chord length (~7uu at the 60uu default), which is bounded in the report and is an
	 * order of magnitude inside the 45uu lethal radius.
	 */
	FVector CatmullRom(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, double T)
	{
		const double T2 = T * T;
		const double T3 = T2 * T;
		return 0.5 * ((2.0 * P1)
			+ (-P0 + P2) * T
			+ (2.0 * P0 - 5.0 * P1 + 4.0 * P2 - P3) * T2
			+ (-P0 + 3.0 * P1 - 3.0 * P2 + P3) * T3);
	}

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

	/**
	 * THE RIBBON'S BRIGHTNESS (spec v6 §2).
	 *
	 * Deliberately bound to UTraceSettings::TraceGhostGlow rather than to a new property. That knob
	 * was "the emissive strength of the brightest layer of the trace", the settings panel already
	 * shows it, it is already live-editable in PIE, and the ribbon IS the brightest (and only) layer
	 * now — so the number keeps its meaning even though the geometry under it changed. Adding a
	 * property would have meant editing TraceSettings.h, which this slice does not own, and a knob
	 * that silently does nothing is the failure mode this project has already been bitten by.
	 * Trace.Trail.GhostGlow still overrides it for a measurement run. The rename is in the report.
	 *
	 * 2.6 was measured as "above the smear, below the point where the team colour clips to white and
	 * you can no longer tell whose trace it is". The ribbon spans first-person eye height along its
	 * whole length, which is what the legacy renderer needed a separate 4.2-glow head band to
	 * achieve, so the single value now does both jobs.
	 */
	float ResolvedRibbonGlow()
	{
		const float Value = IsCVarOverridden(TEXT("Trace.Trail.GhostGlow"))
			? GGhostGlow
			: UTraceSettings::Get().TraceGhostGlow;
		return FMath::Max(0.f, Value);
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

	// Spec v6 §2: "a rectangle which curves to follow the player". The ribbon's cross-section is a
	// rectangle, so its source primitive is a box — 12 triangles, the plain static-mesh vertex
	// factory, and no material usage flag beyond the one the smear has always needed.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		CubeMesh = CubeFinder.Object;
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

	// SPEC v7 §7. BEFORE anything reads the point set on this machine — the visuals, the predicted
	// head, and on a listen client's screen every judgement a player makes about where the trace is.
	// A no-op on authority and on any client whose array happens to have arrived in order.
	RestoreReplicatedPointOrder();

	// Listen servers draw the trace too; only a headless server skips it.
	if (GetNetMode() != NM_DedicatedServer)
	{
		// Spec v6 §1. One timer pair around every line of visual work this component does, so
		// Trace.Trail.PerfAB can quote the trace's own game-thread cost rather than infer it.
		const double VisualStartSeconds = FPlatformTime::Seconds();

		UpdateVisuals();

		// EVERY frame, and BEFORE the fade below, for two reasons that are really one: it tracks a
		// moving pawn rather than a settled point array, and the piece it places has to be handed to
		// the proximity pass on the same frame it appears (spec v5 §2).
		UpdatePredictedHead();

		// EVERY frame, not just on a rebuild: this depends on where the local camera is, and the
		// camera moves continuously while the geometry does not. See the function's comment.
		ApplyProximityGlowFade();

		GTrailVisualMillisecondsThisFrame += (FPlatformTime::Seconds() - VisualStartSeconds) * 1000.0;
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

float UTraceTrailComponent::GetTraceMaxLengthUU()
{
	if (GTrailMaxLength > 0.f)
	{
		return GTrailMaxLength;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// INTEGRATED: the first-class knob asked for in the v7 report now exists, so the settings panel
	// and DefaultGame.ini own this number (1200) and the derivation below is the fallback for a
	// config that leaves it at or below zero.
	if (Settings.TrailMaxLengthUU > 0.f)
	{
		return FMath::Max(FMath::Max(1.f, Settings.TrailPointSpacing) * 2.f, Settings.TrailMaxLengthUU);
	}

	// Spec v7 §2's arithmetic, written as arithmetic so it stays true when the settings move:
	// the old walking trace was TrailLifetime x WalkSpeed long, and the request is that minus 25%.
	const float Derived = FMath::Max(0.1f, Settings.TrailLifetime)
		* FMath::Max(1.f, Settings.WalkSpeed)
		* 0.75f;

	// Floored at one point spacing: below that the trace could not hold two points, and a one-point
	// trace is a degenerate blob rather than a path.
	return FMath::Max(FMath::Max(1.f, Settings.TrailPointSpacing) * 2.f, Derived);
}

float UTraceTrailComponent::GetTraceTrailRadius()
{
	const float Value = (GTrailRadiusOverride >= 0.f)
		? GTrailRadiusOverride : UTraceSettings::Get().TrailRadius;
	return FMath::Max(1.f, Value);
}

float UTraceTrailComponent::GetTraceTrailHeight()
{
	const float Value = (GTrailHeightOverride >= 0.f)
		? GTrailHeightOverride : UTraceSettings::Get().TrailHeight;
	return FMath::Max(1.f, Value);
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

void UTraceTrailComponent::ServerResetParryCooldown()
{
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}

	// Zero, not "now minus something": GetParryCooldownRemaining() special-cases <= 0 as "never set",
	// which is exactly the state a refund wants to restore.
	ParryCooldownEndServerTime = 0.f;

	// Plain DOREPLIFETIME with no push-model dirty flag, so the write itself is enough to replicate —
	// this only stops the owning client's HUD pip waiting for the next scheduled net update to learn
	// that its parry is back. Spec v7 §6 asks for the refund to show immediately.
	if (AActor* OwnerActor = GetOwner())
	{
		OwnerActor->ForceNetUpdate();
	}
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
	//
	// v7 §3 halved the radius (45 -> 22.5), so the exempt stub halves with it — which is right: the
	// exemption is "one body width of trace under the emitter's own feet", and the trace is now half
	// a body wide. It is read through GetTraceTrailRadius() so it can never disagree with the volume.
	const double GraceDistance = FMath::Max(0.0, static_cast<double>(GetTraceTrailRadius()));

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

	// The predicted stub is the newest thing on screen, so it is the one piece a dying holder would
	// most obviously leave hanging in the air. It goes on the same frame as everything else.
	HidePredictedHead();

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

// =================================================================================================
// SPEC v7 §7 — THE CLIENT TETHER BUG
//
// THE REPORT, verbatim, from a joining client at 40 ping: "The far end of the trace seems to be
// pulled back towards their character model, so that both ends of the trace are tied to the
// character instead of just the most recent part of the trace."
//
// THE SPEC'S LEAD WAS WRONG, AND CHECKING IT FIRST IS WHY THIS IS THE RIGHT FIX. The suspicion was
// that the ribbon pieces attach with KeepRelativeTransform and never get SetAbsolute(true,true,true),
// so they ride the character. They do get it: EnsureRibbonElement builds every piece through
// CreatePooledMesh, and CreatePooledMesh calls SetAbsolute on the line after the attach — the same
// call the ghost path makes. Both renderer arms share it. Nothing drawn by this component inherits
// the carrier's transform, and no amount of stale frames could make it.
//
// THE ACTUAL MECHANISM IS IN THE ENGINE'S DELTA SERIALISER, AND IT IS THE DATA THAT MOVES, NOT THE
// MESHES. FFastArraySerializer applies removals like this
// (Engine/Source/Runtime/Net/Core/Classes/Net/Serialization/FastArraySerializer.h):
//
//     Items.RemoveAtSwap(DeleteIndex, EAllowShrinking::No);
//
// RemoveAtSwap, not RemoveAt. A receiving machine therefore has NO ordering guarantee at all, and
// this component's entire model — "Items are strictly ordered oldest-first" — is an authority-only
// truth. The server retires points from the FRONT, so index 0 is what gets deleted, and RemoveAtSwap
// fills the hole with the LAST element: the newest point, the one under the carrier's own feet.
// After one removal the client's array reads [newest, second-oldest, third-oldest, ...], so the
// polyline everything is drawn from now starts AT THE CARRIER, runs out to the far end of the trace,
// and comes back. Both ends tied to the character. That is the report, exactly, and it is not a
// rendering artefact — the client's idea of where the trace IS has been corrupted.
//
// WHY THE HOST NEVER SEES IT: authority mutates Items directly with RemoveAt(0, N), which preserves
// order. It has nothing to do with the host rebuilding more often.
//
// WHY IT GOT WORSE WITH PING rather than appearing at all: it needs a removal to have happened, and
// it is repaired-looking on any frame where the swapped-in point is close to where the tail was. At
// 40ms the client sits on the corrupted array across several frames of the carrier's movement, so
// the segment from slot 0 to the carrier stretches and the tether is unmistakable.
//
// THE FIX IS TO REPAIR THE ARRAY, not to draw around it. Every other reader in the project indexes
// TrailPoints.Items directly and assumes path order — ComputeLastLethalIndex, the bots' intercept
// planning, TraceParry's verifier, the GameMode's trail-kill probe. A private sorted copy inside the
// renderer would have fixed the picture and left all of them holding a scrambled path.
// =================================================================================================

bool UTraceTrailComponent::RestoreReplicatedPointOrder()
{
	if (GClientOrderFix == 0)
	{
		return false;   // The BEFORE arm. See Trace.Trail.ClientOrderFix.
	}

	const AActor* Owner = GetOwner();
	if (Owner == nullptr || Owner->HasAuthority())
	{
		// Authority OWNS the order and is the only writer. Sorting here would be a no-op at best and,
		// if a point set were ever legitimately non-monotonic, would silently rewrite the truth.
		return false;
	}

	const int32 PointCount = TrailPoints.Items.Num();
	if (PointCount < 2)
	{
		return false;
	}

	// The common case is a single O(n) scan that finds nothing: n is ~21 points at the v7 §2 length.
	bool bOutOfOrder = false;
	for (int32 Index = 1; Index < PointCount; ++Index)
	{
		if (TrailPoints.Items[Index].BirthServerTime < TrailPoints.Items[Index - 1].BirthServerTime)
		{
			bOutOfOrder = true;
			break;
		}
	}

	if (!bOutOfOrder)
	{
		return false;
	}

	// BirthServerTime is the authority's own shared-clock stamp, replicated with the point and never
	// touched on a client, so it is the one field that still knows the true path order after the swap.
	// Stable, so points stamped in the same tick (which the append gate makes impossible, but a
	// pathological spacing could not rule out) keep whatever relative order they arrived in.
	TrailPoints.Items.StableSort([](const FTraceTrailPoint& A, const FTraceTrailPoint& B)
	{
		return A.BirthServerTime < B.BirthServerTime;
	});

	// ItemMap maps ReplicationID -> index into Items, and we have just moved every index. Emptying it
	// is what forces the engine's ConditionalRebuildItemMap to rebuild it against the new layout on
	// the next delta (its guard is ItemMap.Num() != Items.Num()); without this, the next
	// PostReplicatedChange would write the update for one point into a different point. This is
	// exactly what the engine itself does on the line after its own RemoveAtSwap loop.
	TrailPoints.ItemMap.Empty();

	++PointOrderRepairs;
	bVisualsDirty = true;

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[TRACEORDER] %s: repaired a scrambled replicated trace (%d points, repair #%d). "
		     "FFastArraySerializer removes with RemoveAtSwap - see RestoreReplicatedPointOrder()."),
		*GetNameSafe(GetOwner()), PointCount, PointOrderRepairs);

	return true;
}

int32 UTraceTrailComponent::CountPointOrderViolations() const
{
	int32 Violations = 0;
	for (int32 Index = 1; Index < TrailPoints.Items.Num(); ++Index)
	{
		if (TrailPoints.Items[Index].BirthServerTime < TrailPoints.Items[Index - 1].BirthServerTime)
		{
			++Violations;
		}
	}
	return Violations;
}

float UTraceTrailComponent::MeasureTailDistanceToCarrier() const
{
	const ATraceCharacter* Holder = GetOwnerCharacter();
	if (Holder == nullptr || TrailPoints.Items.Num() == 0)
	{
		return -1.f;
	}

	return static_cast<float>(FVector::Dist(
		FVector(TrailPoints.Items[0].Location), Holder->GetActorLocation()));
}

float UTraceTrailComponent::MeasureTraceLength() const
{
	double Length = 0.0;
	for (int32 Index = 1; Index < TrailPoints.Items.Num(); ++Index)
	{
		Length += FVector::Dist(TrailPoints.Items[Index - 1].Location, TrailPoints.Items[Index].Location);
	}
	return static_cast<float>(Length);
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

	// 1. Append, distance-gated so a stationary holder does not spam identical points.
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

	// 2. TRIM THE TAIL TO THE MAXIMUM LENGTH (spec v7 §1-2). THIS IS THE ONLY THING THAT RETIRES A
	//    POINT, and it runs AFTER the append for the reason the whole change exists: a point can
	//    only be pushed off the back by new trace arriving at the front. There is no clock in this
	//    block, and there must never be one again — the user's exploit was "stand still, the timer
	//    empties your trace, and you cannot be killed", and the trace is the only counterplay to a
	//    shielded carrier. A stationary holder appends nothing, so nothing here fires, so they keep
	//    every point they have laid for as long as they hold the Core.
	//
	//    Whole points are dropped rather than sliding the tail point along its first segment. The
	//    length therefore lands in (Max - TrailPointSpacing, Max] instead of exactly on Max, which
	//    is the same 60uu granularity the tail has always receded at — and sliding the point would
	//    mean re-replicating item 0 on every server tick a carrier is moving, for a smoothness the
	//    old timer never had either.
	const double MaxLength = static_cast<double>(GetTraceMaxLengthUU());
	if (TrailPoints.Items.Num() > 2)
	{
		double PathLength = 0.0;
		for (int32 Index = 1; Index < TrailPoints.Items.Num(); ++Index)
		{
			PathLength += FVector::Dist(TrailPoints.Items[Index - 1].Location, TrailPoints.Items[Index].Location);
		}

		int32 DropCount = 0;
		while ((TrailPoints.Items.Num() - DropCount) > 2 && PathLength > MaxLength)
		{
			PathLength -= FVector::Dist(
				TrailPoints.Items[DropCount].Location,
				TrailPoints.Items[DropCount + 1].Location);
			++DropCount;
		}

		if (DropCount > 0)
		{
			TrailPoints.Items.RemoveAt(0, DropCount);
			TrailPoints.MarkArrayDirty();
			bChanged = true;
		}
	}

	// 3. Hard cap on POINTS, oldest dropped first. Not an expiry rule — a memory and bandwidth
	//    ceiling that the length cap already keeps two orders of magnitude clear of (1200uu at 60uu
	//    spacing is ~21 points against MaxTrailPoints 256). It survives as the backstop for a
	//    pathological spacing setting.
	const int32 MaxPoints = FMath::Max(2, Settings.MaxTrailPoints);
	if (TrailPoints.Items.Num() > MaxPoints)
	{
		TrailPoints.Items.RemoveAt(0, TrailPoints.Items.Num() - MaxPoints);
		TrailPoints.MarkArrayDirty();
		bChanged = true;
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

	// SPEC v7 §3. THE SAME TWO FUNCTIONS THE RIBBON IS DRAWN FROM — this is the line that makes
	// "the lethal volume matches the drawn volume" structural. 22.5uu to either side, 63uu tall,
	// centred on the trail point (which is the carrier's actor location, i.e. mid-model).
	const double TrailRadius = FMath::Max(0.0, static_cast<double>(GetTraceTrailRadius()));
	const double TrailHalfHeight = FMath::Max(0.0, static_cast<double>(GetTraceTrailHeight())) * 0.5;

	ATraceCharacter* Tripper = nullptr;

	// SPEC v6 §3. The enemy whose LETHAL dash was stopped by a parry this tick — they die instead of
	// the carrier. Resolved after the loop, exactly like Tripper and for the same reason: killing
	// re-enters this component. Owned by Gameplay/TraceParry.cpp; this is its only call site.
	//
	// Tripper and ParriedDasher can never both be set on one tick, and that is structural rather
	// than lucky: bInvulnerable is computed once above the loop, Tripper is only assigned in the
	// !bInvulnerable arm and ParriedDasher only in the bInvulnerable arm. So "the dasher dies" and
	// "the carrier dies" are two exclusive outcomes of one evaluation of one fact — they cannot
	// double-kill, and neither can cancel the other.
	ATraceCharacter* ParriedDasher = nullptr;

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

			// v6 §3. A PARRY specifically — not the pass window — turns this into a punish. Note the
			// deliberate non-clause: if the pass window happens to be open too, the dasher still
			// dies. The trace they hit was red (parry wins the tint), and "red trace kills me" has
			// to hold every time or it is not a tell.
			if (bParry && ParriedDasher == nullptr)
			{
				ParriedDasher = Candidate;
			}
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
				static_cast<double>(GetTraceTrailRadius()), TrailPoints.Items.Num(), TestPositions.Num());
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

	// v6 §3, deferred for the same re-entrancy reason as ApplyTrailTrip above: this kills, and the
	// death path re-enters this component.
	if (ParriedDasher != nullptr)
	{
		TraceParry::ServerPunishParriedDash(Holder, ParriedDasher);
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

	// A renderer switch has to force a rebuild, because none of the terms below would notice it: the
	// point set, the invulnerability and the emission state are all identical either side of the
	// lever. Without this, a stationary carrier would keep the old renderer's geometry on screen and
	// Trace.Trail.PerfAB would measure the previous arm.
	if (ActiveRendererArm != GTrailRenderer)
	{
		bVisualsDirty = true;
	}

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

bool UTraceTrailComponent::IsRibbonRenderer()
{
	return GTrailRenderer != 0;
}

int32 UTraceTrailComponent::PartsPerElement()
{
	return IsRibbonRenderer() ? 1 : PartsPerSmear;
}

void UTraceTrailComponent::RebuildVisuals()
{
	// THE RENDERER ARM IS PART OF THE POOL'S IDENTITY. Arm 0 puts two cylinders in every element and
	// keeps a herd of posed Mannequins beside them; arm 1 puts one box in every element and no
	// skinned meshes at all. Reinterpreting one pool as the other would pair a ribbon element with a
	// head band's material and leave orphaned mannequins standing in the arena, so a switch tears the
	// pool down. It happens only when a console command moves the lever.
	if (ActiveRendererArm != GTrailRenderer)
	{
		DestroyVisualPool();
		ActiveRendererArm = GTrailRenderer;
	}

	// Arm 2: the measurement arm that draws nothing at all. See CVarTrailRenderer.
	if (GTrailRenderer == 2)
	{
		HideSmearFrom(0);
		ClearGhostRecords();
		HidePredictedHead();
		return;
	}

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

	// --- Hiding the trace from the holder's OWN eyes is NOT decided here any more -----------------
	//
	// Holding the Core is what puts the camera into third person, and third person parks it
	// ThirdPersonArmLength straight back down the path the holder just walked — which is exactly
	// where this component is placing unlit emissive geometry. Raising the camera above the trace
	// (see ATraceCharacter::GetThirdPersonPivotZ) stops it being INSIDE it, but the after-images
	// immediately under the lens are still hot surfaces a few tens of uu away: they blew out the
	// bottom third of the frame and, worse, drowned the player's own character in glare.
	//
	// That guard used to live right here as a PATH-DISTANCE window — the newest 850uu of the trace,
	// measured back along the polyline, hidden from its own carrier. That is the v5 §2 bug: the
	// camera is 450uu back down that same polyline, so a path-distance window necessarily eats the
	// trace at the carrier's FEET, which is 450uu in front of the lens and cannot blow anything out.
	// It is owner-only, which is precisely why bots never showed it.
	//
	// The guard is now a RADIUS AROUND THE LENS, applied per piece, every frame, in
	// ApplyProximityGlowFade() — where the camera position is already known and already used. See
	// GOwnerHideCameraRadius for the measured numbers. Nothing about it is visible to any other
	// player, and the LETHAL VOLUME IS UNTOUCHED either way: trip resolution runs off TrailPoints,
	// never off what happens to be rendered. A holder cannot trip their own trace anyway, so nothing
	// is being hidden or shown here that its owner could act on.
	if (IsRibbonRenderer())
	{
		// Spec v6 §2. ONE layer, one shape, no ghosts. ClearGhostRecords() is a no-op unless the
		// lever was just moved off the legacy arm, and costs one Num() test otherwise.
		ClearGhostRecords();
		RebuildRibbon(LethalPointCount, InvulnerableScale);
	}
	else
	{
		RebuildSmear(LethalPointCount, InvulnerableScale);
		RebuildPoseGhosts(LethalPointCount, InvulnerableScale);
	}
}

FTraceSmearStyle UTraceTrailComponent::MakeSmearStyle() const
{

	FTraceSmearStyle Style;

	// Derived from the lethal volume, never chosen. If TrailRadius/TrailHeight are retuned the smear
	// moves with them, because it is the drawn statement of where they are.
	Style.Width = FMath::Max(1.0, 2.0 * static_cast<double>(GetTraceTrailRadius()));
	Style.Height = FMath::Max(1.0, static_cast<double>(GetTraceTrailHeight()));

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

	Style.LayerScale = bGhostsOn ? ResolvedSmearGlowScale() : 1.f;
	Style.BodyCentreFrac = bGhostsOn ? SmearBodyCentreFrac : 0.5;
	Style.BodyHeightFrac = bGhostsOn ? SmearBodyHeightFrac : 1.0;

	return Style;
}

void UTraceTrailComponent::PlaceSmearSegment(
	TArray<TObjectPtr<UStaticMeshComponent>>& Pieces,
	TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
	TArray<float>& BaseGlowOut,
	TArray<float>& AppliedScaleOut,
	int32 ElementIndex,
	const FVector& SegStart,
	const FVector& SegEnd,
	double BackOverlap,
	double ForwardOverlap,
	const FTraceSmearStyle& Style,
	float GlowScale,
	bool bOnlyOwnerSees)
{
	FVector Along = SegEnd - SegStart;
	Along.Z = 0.0;
	const double PlanarLength = Along.Size();
	const FVector Direction = (PlanarLength > 1.0) ? (Along / PlanarLength) : FVector::ZeroVector;
	const FRotator Facing = (PlanarLength > 1.0) ? Direction.Rotation() : FRotator::ZeroRotator;

	// The floor is one full body width, so a one-point (degenerate but lethal) trace draws a
	// blob you can see rather than a sliver you cannot.
	const double ElementLength = FMath::Max(Style.Width, PlanarLength + BackOverlap + ForwardOverlap);
	const FVector ElementCentre = (SegStart + SegEnd) * 0.5
		+ Direction * ((ForwardOverlap - BackOverlap) * 0.5);

	// A segment the holder JUMPED along is lethal over a band that slides from one end's height to
	// the other's. Covering the union of the two bands over-draws by at most half the height change
	// at the segment's ends, which is the safe direction: the boundary is never drawn smaller than
	// it is.
	const double SpanHeight = Style.Height + FMath::Abs(SegEnd.Z - SegStart.Z);

	for (int32 Part = 0; Part < PartsPerSmear; ++Part)
	{
		const int32 SlotIndex = ElementIndex * PartsPerSmear + Part;
		if (!Pieces.IsValidIndex(SlotIndex))
		{
			continue;
		}

		UStaticMeshComponent* Piece = Pieces[SlotIndex];
		if (Piece == nullptr)
		{
			continue;
		}

		const bool bIsHead = (Part == PartHead);
		const double CentreFrac = bIsHead ? SmearHeadCentreFrac : Style.BodyCentreFrac;
		const double HeightFrac = bIsHead ? SmearHeadHeightFrac : Style.BodyHeightFrac;
		const double WidthFrac = bIsHead ? SmearHeadWidthFrac : SmearBodyWidthFrac;
		const float BaseGlow = (bIsHead ? SmearHeadGlow : SmearBodyGlow) * Style.LayerScale;

		const FVector DesiredSize(
			ElementLength,
			Style.Width * WidthFrac,
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

		// THE ONE LINE THAT KEEPS THE PREDICTED HEAD OFF EVERY OTHER PLAYER'S SCREEN. Guarded because
		// SetOnlyOwnerSee dirties the render state and this runs as the trace grows.
		if (Piece->bOnlyOwnerSee != bOnlyOwnerSees)
		{
			Piece->SetOnlyOwnerSee(bOnlyOwnerSees);
		}

		// The intended brightness of this piece, BEFORE the camera-proximity fade. Recorded rather
		// than pushed directly, because ApplyProximityGlowFade() runs every frame and needs to
		// know what full brightness means for this piece without re-deriving the whole rebuild.
		if (BaseGlowOut.Num() <= SlotIndex)
		{
			BaseGlowOut.SetNumZeroed(SlotIndex + 1);
		}
		if (AppliedScaleOut.Num() <= SlotIndex)
		{
			AppliedScaleOut.SetNumZeroed(SlotIndex + 1);
		}
		BaseGlowOut[SlotIndex] = BaseGlow * GlowScale;

		if (bTrailMaterialIsNeon && Materials.IsValidIndex(SlotIndex))
		{
			if (UMaterialInstanceDynamic* Material = Materials[SlotIndex])
			{
				// Push full brightness and let the proximity pass pull it down. Resetting the
				// remembered scale forces that pass to re-evaluate this piece, which it must:
				// the piece has just been moved somewhere else entirely.
				Material->SetScalarParameterValue(TEXT("Glow"), BaseGlowOut[SlotIndex]);
				AppliedScaleOut[SlotIndex] = 1.f;
			}
		}
	}
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

void UTraceTrailComponent::RebuildSmear(int32 LethalPointCount, float InvulnerableScale)
{
	if (CylinderMesh == nullptr || LethalPointCount <= 0)
	{
		HideSmearFrom(0);
		return;
	}

	const FTraceSmearStyle Style = MakeSmearStyle();
	const double JointOverlap = FMath::Max(1.0, static_cast<double>(GetTraceTrailRadius()));

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

		// Overlap at INTERIOR joints only. The two ends of the whole trace stay flush with the first
		// and last lethal point, so nothing is drawn beyond the polyline the server kills along.
		//
		// THE FORWARD END IS THE ONE THAT MATTERS FOR v5 §2 AND IT IS STILL FLUSH. The predicted head
		// stub continues the trace from exactly this point, in a separate pool, seen only by the
		// carrier — it does not extend this element, and it does not move where the lethal set ends.
		const double BackOverlap = (SegmentIndex > FirstSegment) ? JointOverlap : 0.0;
		const double ForwardOverlap = (SegmentIndex + 1 < LethalPointCount - 1) ? JointOverlap : 0.0;

		// Age from the NEWER endpoint: a segment is as young as its leading edge.
		const float Age = FMath::Max(0.f, Now - TrailPoints.Items[FMath::Min(SegmentIndex + 1, LethalPointCount - 1)].BirthServerTime);
		const float Remaining = FMath::Clamp(1.f - (Age / FMath::Max(0.01f, Lifetime)), 0.f, 1.f);
		const float FadeScale = FMath::Lerp(GhostOldestGlowScale, 1.f, Remaining);

		PlaceSmearSegment(SmearMeshes, SmearMaterials, SmearBaseGlow, SmearAppliedGlowScale,
			Placed, SegStart, SegEnd, BackOverlap, ForwardOverlap, Style,
			FadeScale * InvulnerableScale, /*bOnlyOwnerSees=*/false);

		++Placed;
	}

	HideSmearFrom(Placed);
}

// =================================================================================================
// SPEC v6 §2 — THE CURVED RIBBON. THIS IS THE TRACE.
//
// Verbatim: "Change the trace from a player shaped trace to a rectangle which curves to follow the
// player. Rather than having noticeable sections, make it one fluid shape. It should emanate from
// the middle of the player's model, and follow all the exact same rules as the old trace, but look
// cleaner. Think a Tron path but from the model's back, curving through the air."
//
// Four requirements, and what each one is in the code:
//
//   A RECTANGLE, NOT A PERSON.   The element mesh is a box at EXACTLY the lethal cross-section:
//                                2 x TrailRadius wide (90uu) and TrailHeight tall (190uu), both read
//                                from UTraceSettings so a retune moves the drawing with the rule.
//                                Nothing about the drawn silhouette is a flattering fraction of the
//                                volume that kills. The posed Mannequins are gone entirely.
//
//   IT CURVES TO FOLLOW.         The centreline is a Catmull-Rom curve THROUGH the trail points,
//                                resampled at a near-uniform arc length, and every element is
//                                oriented by the curve in full 3D — yaw AND pitch. The legacy smear
//                                flattened every segment (Along.Z = 0), so a trace laid over a jump
//                                was a staircase of level slabs; this one banks through the arc,
//                                which is the "curving through the air" half of the request.
//
//   ONE FLUID SHAPE.             Three separate things were producing "noticeable sections", and all
//                                three are addressed rather than one of them:
//                                  1. the twenty discrete Mannequins — deleted;
//                                  2. the two-band cross-section with an empty 72-162uu gap between
//                                     the floor band and the eye ribbon — replaced by one solid
//                                     rectangle spanning the whole lethal height;
//                                  3. Z-FIGHTING between the coplanar faces of overlapping elements,
//                                     which strobes as a bright band at every joint — see
//                                     RibbonAlternateInset.
//                                Elements still overlap by half a body width at interior joints, so
//                                the outside of a corner is never open; on a flat unlit emissive
//                                material an overlap is not visible as anything at all.
//
//   FROM THE MIDDLE OF THE MODEL. The trail points ARE the carrier's actor location, i.e. the centre
//                                of the capsule, i.e. the middle of the model — and the ribbon's
//                                cross-section is centred on the point rather than hung below it.
//                                The legacy smear put its solid band in the bottom 38% of the column
//                                (to leave room for the mannequins), so it read as emanating from
//                                the feet. The owner-only predicted head (spec v5 §2) still carries
//                                the ribbon forward to the carrier so there is no gap at the source.
//
// GAMEPLAY IS UNTOUCHED (spec v6 §3). The ribbon is built from ComputeLastLethalIndex(), the same
// function the server's trip test runs off, so the drawn set is still exactly the lethal set: the
// trip test, lethality, lifetime, turnover grace and the parry all read TrailPoints and have no
// notion that the renderer changed. WHERE THE SILHOUETTE AND THE TRIP VOLUME DIFFER, stated plainly:
//
//   * THE CENTRELINE IS SMOOTHED, so between two trail points it can bow outside the chord by up to
//     ~1/8 of the chord (~7uu at the 60uu default spacing), and the elements chord that curve back
//     by a sagitta of at most a few uu. Net: the drawn centreline is within ~10uu of the lethal
//     polyline, against a lethal radius of 45uu — and the trip test inflates that by the tripper's
//     own capsule radius (~34uu) before it decides. The ribbon is inside the volume that kills
//     everywhere; it never advertises lethal ground that is not.
//   * ALTERNATE ELEMENTS ARE INSET BY 0.6% of the cross-section (0.54uu of 90) to break Z-fighting.
//     Same direction, three orders of magnitude smaller than the margin above.
//   * A PITCHED ELEMENT tilts its cross-section with the curve, so its vertical band is the union of
//     its two ends' bands (Height + |dZ|) rather than the exact lerp — an over-draw of at most half
//     the height change, which is the safe direction and is what the smear always did.
//   * VERTICALLY THE RIBBON NOW COVERS THE FULL LETHAL HEIGHT, where the legacy renderer left
//     72-162uu open for the mannequins to stand in. This is strictly MORE honest than before.
// =================================================================================================

void UTraceTrailComponent::BuildRibbonSamples(const TArray<FVector>& Points, const TArray<float>& Births,
	double Step, int32 MaxElements)
{
	RibbonSamples.Reset();
	RibbonSampleBirth.Reset();

	const int32 PointCount = Points.Num();
	if (PointCount == 0)
	{
		return;
	}

	const bool bHaveBirths = (Births.Num() == PointCount);

	if (PointCount == 1)
	{
		// A one-point trace is a real, lethal, degenerate segment (SweepIntersectsTrace tests it as
		// one), so it gets one stub element rather than nothing at all.
		RibbonSamples.Add(Points[0]);
		RibbonSamples.Add(Points[0]);
		RibbonSampleBirth.Add(bHaveBirths ? Births[0] : 0.f);
		RibbonSampleBirth.Add(bHaveBirths ? Births[0] : 0.f);
		return;
	}

	// Chord lengths on the ORIGINAL polyline. Used as the arc-length parameter: the Catmull-Rom curve
	// is a little longer than its control polygon, so the resample is slightly non-uniform. That is
	// immaterial to a ribbon — it changes element lengths by a percent or two, not their coverage.
	double TotalLength = 0.0;
	TArray<double, TInlineAllocator<64>> Cumulative;
	Cumulative.Reserve(PointCount);
	Cumulative.Add(0.0);
	for (int32 Index = 1; Index < PointCount; ++Index)
	{
		TotalLength += FVector::Dist(Points[Index - 1], Points[Index]);
		Cumulative.Add(TotalLength);
	}

	if (TotalLength <= GeometryEpsilon)
	{
		RibbonSamples.Add(Points[0]);
		RibbonSamples.Add(Points.Last());
		RibbonSampleBirth.Add(bHaveBirths ? Births[0] : 0.f);
		RibbonSampleBirth.Add(bHaveBirths ? Births.Last() : 0.f);
		return;
	}

	// COARSEN, NEVER TRUNCATE. A trace longer than the pool can draw at the requested step gets
	// longer elements, not a missing tail: every uu of the lethal set stays covered. (The legacy
	// smear dropped the oldest segments at its cap, which was a hole in the drawing of a thing that
	// still kills.)
	const double SafeStep = FMath::Max(1.0, Step);
	int32 ElementCount = FMath::CeilToInt(TotalLength / SafeStep);
	ElementCount = FMath::Clamp(ElementCount, 1, FMath::Max(1, MaxElements));

	RibbonSamples.Reserve(ElementCount + 1);
	RibbonSampleBirth.Reserve(ElementCount + 1);

	int32 Segment = 0;
	for (int32 SampleIndex = 0; SampleIndex <= ElementCount; ++SampleIndex)
	{
		const double Distance = (TotalLength * SampleIndex) / static_cast<double>(ElementCount);

		while (Segment + 2 < PointCount && Cumulative[Segment + 1] < Distance)
		{
			++Segment;
		}

		const double SegmentLength = Cumulative[Segment + 1] - Cumulative[Segment];
		const double T = (SegmentLength > GeometryEpsilon)
			? FMath::Clamp((Distance - Cumulative[Segment]) / SegmentLength, 0.0, 1.0)
			: 0.0;

		// End control points are duplicated rather than extrapolated: an extrapolated phantom point
		// would let the curve overshoot PAST the first or last lethal point, i.e. draw trace beyond
		// where the server kills. Duplication makes the curve stop exactly on the endpoint.
		const FVector& P0 = Points[FMath::Max(0, Segment - 1)];
		const FVector& P1 = Points[Segment];
		const FVector& P2 = Points[FMath::Min(PointCount - 1, Segment + 1)];
		const FVector& P3 = Points[FMath::Min(PointCount - 1, Segment + 2)];

		RibbonSamples.Add(CatmullRom(P0, P1, P2, P3, T));

		if (bHaveBirths)
		{
			const float BirthA = Births[Segment];
			const float BirthB = Births[FMath::Min(PointCount - 1, Segment + 1)];
			RibbonSampleBirth.Add(FMath::Lerp(BirthA, BirthB, static_cast<float>(T)));
		}
		else
		{
			RibbonSampleBirth.Add(0.f);
		}
	}
}

void UTraceTrailComponent::RebuildRibbon(int32 LethalPointCount, float InvulnerableScale)
{
	if (LethalPointCount <= 0 || (CubeMesh == nullptr && CylinderMesh == nullptr))
	{
		HideSmearFrom(0);
		return;
	}

	// Snapshot the lethal polyline and its birth times into the reusable scratch arrays, rather than
	// reading TrailPoints inside the resample loop: nothing here can then be surprised by a
	// replication callback landing mid-rebuild, and a steady-state rebuild allocates nothing.
	RibbonSourcePoints.Reset();
	RibbonSourceBirths.Reset();
	for (int32 Index = 0; Index < LethalPointCount; ++Index)
	{
		RibbonSourcePoints.Add(FVector(TrailPoints.Items[Index].Location));
		RibbonSourceBirths.Add(TrailPoints.Items[Index].BirthServerTime);
	}

	BuildRibbonSamples(RibbonSourcePoints, RibbonSourceBirths,
		static_cast<double>(FMath::Max(5.f, GRibbonStep)), MaxRibbonElements);

	CacheMeshMetrics();

	PlaceRibbon(SmearMeshes, SmearMaterials, SmearBaseGlow, SmearAppliedGlowScale,
		MaxRibbonElements, InvulnerableScale, /*bTailFade=*/true, /*bOnlyOwnerSees=*/false,
		/*bOverlapAtStart=*/false);
}

void UTraceTrailComponent::PlaceRibbon(
	TArray<TObjectPtr<UStaticMeshComponent>>& Pieces,
	TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
	TArray<float>& BaseGlowOut,
	TArray<float>& AppliedScaleOut,
	int32 MaxElements,
	float InvulnerableScale,
	bool bTailFade,
	bool bOnlyOwnerSees,
	bool bOverlapAtStart)
{
	const int32 ElementCount = FMath::Max(0, RibbonSamples.Num() - 1);
	if (ElementCount <= 0)
	{
		for (int32 Index = 0; Index < Pieces.Num(); ++Index)
		{
			if (UStaticMeshComponent* Piece = Pieces[Index])
			{
				Piece->SetVisibility(false);
			}
		}
		return;
	}


	// EXACTLY the lethal cross-section, both axes, unless somebody has explicitly dialled the width
	// down with Trace.Trail.RibbonWidthScale (default 1.0 — see the comment there for what a
	// narrower ribbon costs the player). A boundary drawn narrower than it really is turns the trace
	// into a trap rather than a warning.
	const double LethalWidth = FMath::Max(1.0, 2.0 * static_cast<double>(GetTraceTrailRadius()));
	const double Width = LethalWidth * FMath::Clamp(static_cast<double>(GRibbonWidthScale), 0.05, 2.0);
	const double Height = FMath::Max(1.0, static_cast<double>(GetTraceTrailHeight()));

	// Half a body width of overlap at interior joints — enough to close the wedge on the outside of a
	// corner up to a 90-degree turn between two consecutive elements.
	const double JointOverlap = FMath::Max(1.0, static_cast<double>(GetTraceTrailRadius()));

	const float RibbonGlow = ResolvedRibbonGlow();

	// THE FADE IS ALONG THE LENGTH NOW, NOT THROUGH TIME (spec v7 §1).
	//
	// It used to ramp on each sample's AGE against the lifetime, which was honest while age was what
	// retired a point. It no longer is: a carrier who stands still keeps their trace forever, so an
	// age ramp would quietly dim a perfectly lethal trace to its floor and tell the player it was
	// about to expire when nothing of the kind was happening. Distance back from the head is now the
	// thing that predicts what dies next, so distance back from the head is what the gradient shows.
	//
	// Computed as a cumulative arc length over the samples — the same samples the elements are placed
	// between — so it stays a smooth ramp along the ribbon rather than a step per element.
	double RibbonTotalLength = 0.0;
	TArray<double, TInlineAllocator<128>> SampleDistance;
	if (bTailFade)
	{
		SampleDistance.Reserve(RibbonSamples.Num());
		SampleDistance.Add(0.0);
		for (int32 Index = 1; Index < RibbonSamples.Num(); ++Index)
		{
			RibbonTotalLength += FVector::Dist(RibbonSamples[Index - 1], RibbonSamples[Index]);
			SampleDistance.Add(RibbonTotalLength);
		}
	}

	// The trace is trimmed to this, so distance-from-head divided by it is "how close this stretch is
	// to being the next thing pushed off the tail" — which is exactly what the gradient means.
	const double FadeLength = FMath::Max(1.0, static_cast<double>(GetTraceMaxLengthUU()));

	const FVector HalfSize = (CubeMesh != nullptr) ? CubeHalfSize : CylinderHalfSize;
	const FVector PivotOffset = (CubeMesh != nullptr) ? CubePivotOffset : CylinderPivotOffset;

	int32 Placed = 0;
	for (int32 ElementIndex = 0; ElementIndex < ElementCount; ++ElementIndex)
	{
		if (!EnsureRibbonElement(Pieces, Materials, BaseGlowOut, AppliedScaleOut,
			Placed, MaxElements, bOnlyOwnerSees))
		{
			break;
		}

		UStaticMeshComponent* Piece = Pieces.IsValidIndex(Placed) ? Pieces[Placed].Get() : nullptr;
		if (Piece == nullptr)
		{
			++Placed;
			continue;
		}

		const FVector SegStart = RibbonSamples[ElementIndex];
		const FVector SegEnd = RibbonSamples[ElementIndex + 1];

		FVector Along = SegEnd - SegStart;
		const double Length = Along.Size();
		// FULL 3D, pitch included — this is what makes the ribbon curve through the air rather than
		// step through it. Roll is left at zero so the ribbon always stands upright: the lethal volume
		// is a vertical column, and a banked cross-section would advertise a boundary that is not
		// there. Rotation() yields yaw+pitch with roll 0 by construction.
		const FVector Direction = (Length > GeometryEpsilon) ? (Along / Length) : FVector::ForwardVector;
		const FRotator Facing = Direction.Rotation();

		// Interior joints overlap; the two OUTER ends stay flush with the first and last lethal point,
		// so the ribbon never extends past the polyline the server kills along.
		//
		// bOverlapAtStart is the one exception, and it is the seam the user is looking straight at: the
		// owner-only predicted stub begins exactly where the drawn lethal set ends, so ITS first
		// element overlaps BACKWARD into the last real one. Without that, the one joint a carrier sees
		// from a metre away is the only butt joint in the whole trace.
		const double BackOverlap = (ElementIndex > 0 || bOverlapAtStart) ? JointOverlap : 0.0;
		const double ForwardOverlap = (ElementIndex + 1 < ElementCount) ? JointOverlap : 0.0;

		// Never shorter than one body width, so a degenerate (but lethal) one-point trace draws a
		// block you can see rather than a sliver you cannot.
		const double ElementLength = FMath::Max(LethalWidth, Length + BackOverlap + ForwardOverlap);
		const FVector ElementCentre = (SegStart + SegEnd) * 0.5
			+ Direction * ((ForwardOverlap - BackOverlap) * 0.5);

		// Union of the two ends' vertical bands, exactly as the smear did it: over-drawing a lethal
		// boundary is the safe direction, under-drawing it is a trap.
		const double SpanHeight = Height + FMath::Abs(SegEnd.Z - SegStart.Z);

		// See RibbonAlternateInset: this is the anti-Z-fight, not a taper.
		const double Inset = ((ElementIndex & 1) != 0) ? RibbonAlternateInset : 1.0;

		const FVector Scale(
			ElementLength / (2.0 * HalfSize.X),
			(Width * Inset) / (2.0 * HalfSize.Y),
			(SpanHeight * Inset) / (2.0 * HalfSize.Z));

		// Corrects for a source mesh whose pivot is not at its bounds centre, so we never assume
		// anything about the engine primitives' authoring.
		const FVector PivotCorrection = Facing.RotateVector(PivotOffset * Scale);

		Piece->SetWorldLocationAndRotation(ElementCentre - PivotCorrection, Facing);
		Piece->SetWorldScale3D(Scale);
		if (!Piece->IsVisible())
		{
			Piece->SetVisibility(true);
		}

		// THE ONE LINE THAT KEEPS THE PREDICTED HEAD OFF EVERY OTHER PLAYER'S SCREEN. Guarded because
		// SetOnlyOwnerSee dirties the render state and this runs as the trace grows.
		if (Piece->bOnlyOwnerSee != bOnlyOwnerSees)
		{
			Piece->SetOnlyOwnerSee(bOnlyOwnerSees);
		}

		// The tail fade is a GRADIENT ALONG THE RIBBON, not a step per element: it is evaluated at
		// each element's own arc length, so consecutive elements differ by a percent or two and the
		// eye reads a cooling ramp instead of a row of tiles. It never falls to zero — a stretch of
		// trace near the tail is exactly as lethal as one at the head, and the moment it stops being
		// lethal it stops being drawn.
		float FadeScale = 1.f;
		if (bTailFade && SampleDistance.IsValidIndex(ElementIndex + 1))
		{
			const double DistanceFromHead = FMath::Max(0.0, RibbonTotalLength - SampleDistance[ElementIndex + 1]);
			const float Remaining = static_cast<float>(FMath::Clamp(1.0 - (DistanceFromHead / FadeLength), 0.0, 1.0));
			FadeScale = FMath::Lerp(GhostOldestGlowScale, 1.f, Remaining);
		}

		if (BaseGlowOut.Num() <= Placed)
		{
			BaseGlowOut.SetNumZeroed(Placed + 1);
		}
		if (AppliedScaleOut.Num() <= Placed)
		{
			AppliedScaleOut.SetNumZeroed(Placed + 1);
		}
		BaseGlowOut[Placed] = RibbonGlow * FadeScale * InvulnerableScale;

		if (bTrailMaterialIsNeon && Materials.IsValidIndex(Placed))
		{
			if (UMaterialInstanceDynamic* Material = Materials[Placed])
			{
				// Push full brightness and let the proximity pass pull it down. Resetting the
				// remembered scale forces that pass to re-evaluate this piece, which it must: the
				// piece has just been moved somewhere else entirely.
				Material->SetScalarParameterValue(TEXT("Glow"), BaseGlowOut[Placed]);
				AppliedScaleOut[Placed] = 1.f;
			}
		}

		++Placed;
	}

	for (int32 Index = Placed; Index < Pieces.Num(); ++Index)
	{
		if (UStaticMeshComponent* Piece = Pieces[Index])
		{
			if (Piece->IsVisible())
			{
				Piece->SetVisibility(false);
			}
		}
	}
}

bool UTraceTrailComponent::EnsureRibbonElement(
	TArray<TObjectPtr<UStaticMeshComponent>>& Pieces,
	TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
	TArray<float>& BaseGlowOut,
	TArray<float>& AppliedScaleOut,
	int32 ElementIndex,
	int32 MaxElements,
	bool bOnlyOwnerSees)
{
	if (ElementIndex < 0 || ElementIndex >= MaxElements)
	{
		return false;
	}

	if (Pieces.Num() > ElementIndex)
	{
		return true;
	}

	// One element at a time, in order, so an index can never mean two different things.
	if (Pieces.Num() != ElementIndex)
	{
		return false;
	}

	UMaterialInstanceDynamic* Material = nullptr;
	UStaticMeshComponent* Piece = CreatePooledMesh(
		(CubeMesh != nullptr) ? CubeMesh.Get() : CylinderMesh.Get(), Material);

	// Set on creation as well as on every placement: a predicted-head piece must NEVER be visible to
	// anyone but the carrier, so it starts that way rather than becoming that way.
	if (Piece != nullptr && bOnlyOwnerSees)
	{
		Piece->SetOnlyOwnerSee(true);
	}

	Pieces.Add(Piece);
	Materials.Add(Material);
	BaseGlowOut.Add(0.f);
	AppliedScaleOut.Add(1.f);

	return true;
}

// -------------------------------------------------------------------------------------------------
// SPEC v5 §2 — THE PREDICTED HEAD, and why it cannot break "visible implies lethal".
//
// THE BUG. "When I as a player pick up the core, my trace has a gap between me and the end of it (the
// most recent section is missing). This bug is not present for bots carrying the trace."
//
// THERE ARE TWO CAUSES, and they stack. Both are owner-only, which is the whole reason bots never
// showed the symptom — a bot's trace is drawn from the same replicated array by the same code, but no
// local viewer owns it.
//
//   1. THE OWNER NEAR HIDE, and this is the big one, worth ~850uu. It was a path-distance window and
//      it is now a radius around the lens: see GOwnerHideCameraRadius, which carries the numbers.
//      It is present in a solo match with no networking at all, which is how this got reported at
//      all given that PLAY has never opened a listen server (spec v5 §0).
//
//   2. THE DRAWN SET ENDS AT ComputeLastLethalIndex(), which deliberately stops one TrailRadius short
//      of the carrier's feet (the head-grace stub: the newest ~45uu, plus the ungated head point,
//      is neither lethal nor drawn so a defender cannot stand on the emitter and dash on the spot).
//      On a REMOTE CLIENT there is a third term on top of that: the points the server has already
//      laid but this machine has not received yet — one round trip of trace, exactly as the spec's
//      diagnosis says.
//
// THE FIX. On the machine where the carrier is locally controlled, continue the drawn polyline from
// the newest drawn point, through any real-but-not-yet-lethal points, to where the pawn actually is
// this frame. Stateless: it is recomputed from scratch every frame, so "reconciling when the
// authoritative point lands" is not a code path at all — the moment a new point arrives it becomes
// part of the lethal set, the base of the stub advances, and the stub shortens by the same amount.
// There is no accumulated prediction that could ever be wrong about the past.
//
// HOW THE VISIBLE == LETHAL INVARIANT SURVIVES. The invariant exists to protect ONE decision: an
// enemy looking at a piece of trace and deciding whether to dash through it. The stub is unreachable
// by that decision, twice over:
//
//   (a) IT IS ONLY BUILT ON THE CARRIER'S OWN MACHINE. These are locally created, non-replicated
//       transient components. No other machine has them, so no other machine can draw them.
//   (b) IT IS bOnlyOwnerSee. Even on a listen host, where the carrier's components and the enemy's
//       viewer share a process, the renderer shows the stub only to the viewer whose view target
//       owns it — the carrier.
//
// So the set of viewers who can see the stub is exactly "whoever is currently looking THROUGH the
// carrier" — the carrier, plus any future spectator bound to them as a view target. The carrier is
// the one player who can NEVER be killed by their own trace: ServerRunTripTest skips
// Candidate == Holder unconditionally, before any of the eligibility rules; a spectator has no pawn
// to dash with at all. Not one pixel changes on the screen of anybody who could dash through it. The
// inverse hazard the spec warns about — "an enemy dashes through something that looks lethal and
// survives" — requires an enemy who can SEE it, and there is none.
//
// WHAT THE CARRIER IS BEING TOLD, stated honestly rather than glossed: their own trace now reaches
// their feet, and the newest ~45-105uu of what they see (plus, on a client, one round trip more) is
// not yet lethal to anyone. That over-states their own coverage by about one body width, in the one
// place — under their own feet — where an enemy trying to use it would be standing on top of them.
// The alternative was under-stating it by 850uu, which is the bug being fixed. Nothing else in the
// game reads this geometry: the bots plan against TrailPoints, and the trip test evaluates
// TrailPoints, and neither has any notion that the stub exists.
//
// It is also DROPPED ENTIRELY rather than clamped when it would have to span more than
// GPredictedHeadMaxLength, because past that distance a straight line from the last known point to
// the pawn is an invention (a teleport, a respawn, a severe desync) rather than an interpolation.
// -------------------------------------------------------------------------------------------------

bool UTraceTrailComponent::IsPredictingLocalHead() const
{
	return PredictedHeadLength > 0.f;
}

float UTraceTrailComponent::GetPredictedHeadLength() const
{
	return PredictedHeadLength;
}

float UTraceTrailComponent::MeasureHeadGap() const
{
	const ATraceCharacter* Holder = GetOwnerCharacter();
	const int32 LastLethal = ComputeLastLethalIndex();
	if (Holder == nullptr || LastLethal < 0)
	{
		return 0.f;
	}

	// Measured along the chain of real points and then out to the pawn, not as a straight line from
	// the last lethal point: on a curving path those differ, and the gap the player sees is the one
	// that follows the path.
	double Gap = 0.0;
	FVector Cursor = FVector(TrailPoints.Items[LastLethal].Location);
	for (int32 Index = LastLethal + 1; Index < TrailPoints.Items.Num(); ++Index)
	{
		const FVector Next = FVector(TrailPoints.Items[Index].Location);
		Gap += FVector::Dist(Cursor, Next);
		Cursor = Next;
	}
	Gap += FVector::Dist(Cursor, Holder->GetActorLocation());

	return static_cast<float>(Gap);
}

float UTraceTrailComponent::MeasureOwnerVisibleGap(bool bIncludePredictedHead) const
{
	const ATraceCharacter* Holder = GetOwnerCharacter();
	if (Holder == nullptr)
	{
		return -1.f;
	}

	const FVector PawnLocation = Holder->GetActorLocation();
	double Nearest = TNumericLimits<double>::Max();

	// Surface distance, so a piece the carrier is standing in reports 0 rather than half its length.
	auto Consider = [&PawnLocation, &Nearest](const auto& Pieces)
	{
		for (int32 Slot = 0; Slot < Pieces.Num(); ++Slot)
		{
			const UMeshComponent* Piece = Pieces[Slot];
			if (Piece == nullptr || !Piece->IsVisible() || Piece->bOwnerNoSee)
			{
				continue;   // Not drawn, or drawn for everyone EXCEPT the person we are measuring for.
			}

			const FBoxSphereBounds& LocalBounds = Piece->Bounds;
			Nearest = FMath::Min(Nearest,
				FMath::Max(0.0, FVector::Dist(PawnLocation, LocalBounds.Origin) - LocalBounds.SphereRadius));
		}
	};

	Consider(SmearMeshes);
	Consider(PoseGhosts);
	if (bIncludePredictedHead)
	{
		Consider(PredictedSmearMeshes);
	}

	return (Nearest == TNumericLimits<double>::Max()) ? -1.f : static_cast<float>(Nearest);
}

void UTraceTrailComponent::UpdatePredictedHead()
{
	// ---- every reason not to draw it, cheapest first ------------------------------------------
	//
	// Arm 2 of Trace.Trail.Renderer draws NOTHING, and that has to include the owner-only stub: the
	// arm exists to bound the total cost of this component's visuals, and six elements it forgot to
	// hide would be six elements silently subtracted from the answer.
	if (CylinderMesh == nullptr || GPredictedHeadMaxLength <= 0.f || GTrailRenderer == 2)
	{
		HidePredictedHead();
		return;
	}

	// Suppressed visuals mean a holder just died: the trace is about to vanish and the stub must not
	// outlive it by even the suppression window.
	if (VisualSuppressUntilTime > 0.f)
	{
		HidePredictedHead();
		return;
	}

	// ONLY THE LOCALLY CONTROLLED CARRIER. Not "the owner": a listen host owns every bot's component
	// too, and predicting a head for a bot would put geometry on the host's screen that no client has.
	ATraceCharacter* Holder = GetOwnerCharacter();
	if (Holder == nullptr || !Holder->IsAlive() || !bEmitting)
	{
		HidePredictedHead();
		return;
	}

	const APlayerController* HolderPC = Cast<APlayerController>(Holder->GetController());
	if (HolderPC == nullptr || !HolderPC->IsLocalPlayerController())
	{
		HidePredictedHead();
		return;
	}

	// THE STUB ONLY EVER CONTINUES SOMETHING THAT IS ALREADY LETHAL AND ALREADY DRAWN. If there is no
	// lethal point yet — an empty trace, or the whole of it still inside the turnover grace (§2) or
	// the head exemption — there is nothing to continue and nothing is drawn. That is what keeps the
	// grace window looking like a grace window instead of a trace the server does not have.
	const int32 LastLethal = ComputeLastLethalIndex();
	if (LastLethal < 0)
	{
		HidePredictedHead();
		return;
	}

	// ---- build the polyline: last drawn point -> real ungated points -> the pawn ----------------
	TArray<FVector, TInlineAllocator<8>> Path;
	Path.Add(FVector(TrailPoints.Items[LastLethal].Location));
	for (int32 Index = LastLethal + 1; Index < TrailPoints.Items.Num(); ++Index)
	{
		Path.Add(FVector(TrailPoints.Items[Index].Location));
	}

	const FVector PawnLocation = Holder->GetActorLocation();
	if (FVector::Dist(Path.Last(), PawnLocation) > 1.0)
	{
		Path.Add(PawnLocation);
	}

	double TotalLength = 0.0;
	for (int32 Index = 1; Index < Path.Num(); ++Index)
	{
		TotalLength += FVector::Dist(Path[Index - 1], Path[Index]);
	}

	if (TotalLength <= 1.0 || TotalLength > static_cast<double>(GPredictedHeadMaxLength))
	{
		// Nothing to cover, or so much that a straight line would be a fabrication. See the header
		// comment: abandoned, never clamped.
		HidePredictedHead();
		return;
	}

	CacheMeshMetrics();

	const double JointOverlap = FMath::Max(1.0, static_cast<double>(GetTraceTrailRadius()));

	// The stub is the newest trace there is, so it takes the newest trace's brightness: no age fade,
	// and the same parry / pass-window step the rest of the trace is wearing this frame. A stub that
	// stayed team-cyan while the trace behind it went red would be a state nobody has a name for.
	float InvulnerableScale = 1.f;
	if (IsParryVisuallyActive())
	{
		InvulnerableScale = TraceParry::GetGlowScale();
	}
	else if (IsPassWindowInvulnerable())
	{
		InvulnerableScale = GhostInvulnerableGlowScale;
	}

	// ---- the ribbon arm (spec v6 §2): the stub is the same shape as the trace it continues --------
	//
	// It has to be built from the SAME resample-and-place code, not from a lookalike: the stub's whole
	// job is to be indistinguishable from the ribbon it joins onto, and two implementations of "a
	// curved rectangle at the lethal cross-section" would be two chances for the join to show. The
	// FIRST sample is the last drawn point, so the stub's back end overlaps into the last real element
	// exactly as an interior joint does.
	if (IsRibbonRenderer())
	{
		RibbonSourcePoints.Reset();
		RibbonSourceBirths.Reset();
		for (const FVector& PathPoint : Path)
		{
			RibbonSourcePoints.Add(PathPoint);
		}

		BuildRibbonSamples(RibbonSourcePoints, RibbonSourceBirths,
			static_cast<double>(FMath::Max(5.f, GRibbonStep)), MaxPredictedRibbonElements);

		// No age fade: the stub is the newest trace there is, so it wears the newest trace's
		// brightness and cannot read as older than the element behind it.
		PlaceRibbon(PredictedSmearMeshes, PredictedSmearMaterials,
			PredictedSmearBaseGlow, PredictedSmearAppliedGlowScale,
			MaxPredictedRibbonElements, InvulnerableScale, /*bTailFade=*/false, /*bOnlyOwnerSees=*/true,
			/*bOverlapAtStart=*/true);

		PredictedHeadLength = (PredictedSmearMeshes.Num() > 0) ? static_cast<float>(TotalLength) : 0.f;
		return;
	}

	// ---- the legacy arm (spec v4 §2), reached only from Trace.Trail.Renderer 0 ---------------------
	const FTraceSmearStyle Style = MakeSmearStyle();

	int32 Placed = 0;
	for (int32 SegmentIndex = 0; SegmentIndex + 1 < Path.Num(); ++SegmentIndex)
	{
		if (!EnsurePredictedElement(Placed))
		{
			break;
		}

		// The BACK end overlaps into the last lethal element, which is what makes the join seamless —
		// this is the joint the user is looking at. The FORWARD end of the last segment gets no
		// overlap, so the drawn thing stops at the pawn and never reaches past it.
		const double BackOverlap = JointOverlap;
		const double ForwardOverlap = (SegmentIndex + 2 < Path.Num()) ? JointOverlap : 0.0;

		PlaceSmearSegment(PredictedSmearMeshes, PredictedSmearMaterials,
			PredictedSmearBaseGlow, PredictedSmearAppliedGlowScale,
			Placed, Path[SegmentIndex], Path[SegmentIndex + 1], BackOverlap, ForwardOverlap,
			Style, InvulnerableScale, /*bOnlyOwnerSees=*/true);

		++Placed;
	}

	for (int32 Index = Placed * PartsPerSmear; Index < PredictedSmearMeshes.Num(); ++Index)
	{
		if (UStaticMeshComponent* Piece = PredictedSmearMeshes[Index])
		{
			Piece->SetVisibility(false);
		}
	}

	PredictedHeadLength = (Placed > 0) ? static_cast<float>(TotalLength) : 0.f;
}

void UTraceTrailComponent::HidePredictedHead()
{
	PredictedHeadLength = 0.f;

	for (UStaticMeshComponent* Piece : PredictedSmearMeshes)
	{
		if (Piece != nullptr && Piece->IsVisible())
		{
			Piece->SetVisibility(false);
		}
	}
}

bool UTraceTrailComponent::EnsurePredictedElement(int32 ElementIndex)
{
	if (ElementIndex < 0 || ElementIndex >= MaxPredictedSmearElements)
	{
		return false;
	}

	const int32 RequiredNum = (ElementIndex + 1) * PartsPerSmear;
	if (PredictedSmearMeshes.Num() >= RequiredNum)
	{
		return true;
	}

	// Same one-whole-element-at-a-time rule as EnsureSmearElement, for the same reason: a short array
	// would pair one element's head band with the next one's body.
	if (PredictedSmearMeshes.Num() != ElementIndex * PartsPerSmear)
	{
		return false;
	}

	for (int32 Part = 0; Part < PartsPerSmear; ++Part)
	{
		UMaterialInstanceDynamic* Material = nullptr;
		UStaticMeshComponent* Piece = CreatePooledMesh(CylinderMesh.Get(), Material);

		// Set on creation as well as on every placement. A piece of this pool must NEVER be visible
		// to anyone but the carrier, so it starts that way rather than becoming that way.
		if (Piece != nullptr)
		{
			Piece->SetOnlyOwnerSee(true);
		}

		PredictedSmearMeshes.Add(Piece);
		PredictedSmearMaterials.Add(Material);
		PredictedSmearBaseGlow.Add(0.f);
		PredictedSmearAppliedGlowScale.Add(1.f);
	}

	return true;
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

void UTraceTrailComponent::RebuildPoseGhosts(int32 LethalPointCount, float InvulnerableScale)
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

		// Whether this after-image is hidden from its own holder is NOT decided here any more: it is a
		// question about where the local camera is this frame, and it is answered per frame, per
		// piece, in ApplyProximityGlowFade(). See the note in RebuildVisuals().
		Ghost->SetVisibility(true);

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
	//
	// IT ALSO OWNS THE OWNER-ONLY NEAR CULL (spec v5 §2), the last resort behind the fade, for a piece
	// close enough that even ProximityFadeMinScale would be too bright in the corner of the carrier's
	// own third-person frame. It lives here and not in the rebuild because it is a question about the
	// CAMERA, which moves every frame while the geometry does not — and answering it in the rebuild,
	// as a distance along the path, is what produced the reported gap. See GOwnerHideCameraRadius.
	//
	// The cull is why the neon check below is per-write rather than an early-out: a build that fell
	// back to BasicShapeMaterial has no Glow parameter to fade, but it still has a carrier with a
	// camera and still needs the cull.
	// ------------------------------------------------------------------------------------------
	if (SmearMeshes.Num() == 0 && PoseGhosts.Num() == 0 && PredictedSmearMeshes.Num() == 0)
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

	// IS THE LOCAL VIEWER THE CARRIER OF *THIS* TRACE? bOwnerNoSee is resolved by the renderer against
	// the view target's owner chain, so this is the same question the flag itself asks — and it has to
	// be asked, because writing the flag from OUR camera's distance when the owner is somebody else's
	// pawn (or a bot) would hide a trace from a player on another machine for a reason that has
	// nothing to do with their view.
	const bool bLocalViewerOwnsThisTrace = (LocalPC->GetViewTarget() == GetOwner());
	const double OwnerHideRadius = FMath::Max(0.0, static_cast<double>(GOwnerHideCameraRadius));
	const bool bNeon = bTrailMaterialIsNeon;

	// For the ghost-only body hide. Zero-length when there is no owner pawn, which switches it off.
	const AActor* OwnerActor = GetOwner();
	const FVector OwnerLocation = (OwnerActor != nullptr) ? OwnerActor->GetActorLocation() : FVector::ZeroVector;

	// Both pools take the same treatment for the FADE: a posed mannequin is exactly as unlit, exactly
	// as emissive and exactly as standable-inside as a smear slab, so exempting it would reinstate the
	// whiteout this function exists to prevent — from the one piece of geometry now closest to the lens.
	//
	// They differ in ONE thing: @p BodyHideRadius, which is non-zero only for the ghosts. See
	// GOwnerGhostHideDistance.
	auto FadePool = [&CameraLocation, &OwnerLocation, bLocalViewerOwnsThisTrace, OwnerHideRadius, bNeon](
		const auto& Pieces,
		const TArray<TObjectPtr<UMaterialInstanceDynamic>>& Materials,
		const TArray<float>& BaseGlow,
		TArray<float>& AppliedScale,
		double BodyHideRadius)
	{
		for (int32 Slot = 0; Slot < Pieces.Num(); ++Slot)
		{
			UMeshComponent* Piece = Pieces[Slot];
			if (Piece == nullptr || !Piece->IsVisible())
			{
				continue;
			}

			// Distance to the piece's SURFACE, not its centre: these are wide, flat slabs, and a
			// centre distance would report a torso as far away at the exact moment its face is
			// against the lens. The bounds are already computed for culling, so this is free.
			const FBoxSphereBounds& LocalBounds = Piece->Bounds;
			const double SurfaceDistance = FMath::Max(0.0,
				FVector::Dist(CameraLocation, LocalBounds.Origin) - LocalBounds.SphereRadius);

			// --- the owner-only near cull (spec v5 §2) --------------------------------------------
			//
			// Hysteresis in one direction only: a piece already hidden stays hidden until it is
			// clear of the radius by a margin, so a piece hovering on the boundary cannot flicker
			// on and off as the camera breathes. SetOwnerNoSee dirties the render state, so the
			// write is guarded on an actual change either way.
			{
				bool bHideFromOwner = false;
				if (bLocalViewerOwnsThisTrace && OwnerHideRadius > 0.0)
				{
					const double Threshold = Piece->bOwnerNoSee
						? (OwnerHideRadius + OwnerHideCameraHysteresis)
						: OwnerHideRadius;
					bHideFromOwner = (SurfaceDistance < Threshold);
				}

				// The ghost-only body hide: measured from the carrier's own pawn, not from the lens,
				// because what it prevents is a person-shaped after-image being drawn on top of the
				// person it is a copy of. Same hysteresis, same one-viewer scope.
				if (bLocalViewerOwnsThisTrace && BodyHideRadius > 0.0)
				{
					const double BodyThreshold = Piece->bOwnerNoSee
						? (BodyHideRadius + OwnerHideCameraHysteresis)
						: BodyHideRadius;
					bHideFromOwner = bHideFromOwner
						|| (FVector::Dist(OwnerLocation, LocalBounds.Origin) < BodyThreshold);
				}

				if (Piece->bOwnerNoSee != bHideFromOwner)
				{
					Piece->SetOwnerNoSee(bHideFromOwner);
				}
			}

			UMaterialInstanceDynamic* Material = Materials.IsValidIndex(Slot) ? Materials[Slot].Get() : nullptr;
			if (!bNeon || Material == nullptr || !BaseGlow.IsValidIndex(Slot))
			{
				continue;   // Nothing to fade — the cull above has already run, which is the point.
			}

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
	FadePool(SmearMeshes, SmearMaterials, SmearBaseGlow, SmearAppliedGlowScale, /*BodyHideRadius=*/0.0);

	// The ghosts, and ONLY the ghosts, also step away from the carrier's own body.
	FadePool(PoseGhosts, PoseGhostMaterials, PoseGhostBaseGlow, PoseGhostAppliedGlowScale,
		static_cast<double>(FMath::Max(0.f, GOwnerGhostHideDistance)));

	// The predicted head takes exactly the same fade and the same camera cull as everything else. It
	// is the piece closest to the carrier, so if any part of the trace is ever going to be too bright
	// in their own frame it is this one — exempting it would be exempting the worst case. It is NOT
	// subject to the body hide: reaching the carrier's feet is its entire job.
	FadePool(PredictedSmearMeshes, PredictedSmearMaterials, PredictedSmearBaseGlow,
		PredictedSmearAppliedGlowScale, /*BodyHideRadius=*/0.0);
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
	// PartsPerElement(), not PartsPerSmear: the ribbon is one mesh per element and the legacy smear is
	// two, and this is called from both.
	for (int32 Index = FMath::Max(0, FirstElementIndex) * PartsPerElement(); Index < SmearMeshes.Num(); ++Index)
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

	// AND THE OWNER-ONLY PREDICTED HEAD. It was missing from this loop, which is a real bug and not a
	// tidy-up: the stub took its colour once, at creation, from bColorApplied — so a carrier who
	// parried saw their entire trace go red EXCEPT the 45-400uu of it nearest their own feet, which
	// stayed team-cyan. The carrier is the person the parry tell exists for.
	for (UMaterialInstanceDynamic* Material : PredictedSmearMaterials)
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

	if (CubeMesh != nullptr)
	{
		const FBoxSphereBounds LocalBounds = CubeMesh->GetBounds();
		CubeHalfSize = LocalBounds.BoxExtent;
		CubePivotOffset = LocalBounds.Origin;
	}
	else
	{
		// No cube: the ribbon is drawn on the cylinder instead, so it must measure the cylinder.
		CubeHalfSize = CylinderHalfSize;
		CubePivotOffset = CylinderPivotOffset;
	}

	// Never divide by zero, whatever the assets turn out to be.
	CylinderHalfSize.X = FMath::Max(CylinderHalfSize.X, 1.0);
	CylinderHalfSize.Y = FMath::Max(CylinderHalfSize.Y, 1.0);
	CylinderHalfSize.Z = FMath::Max(CylinderHalfSize.Z, 1.0);
	CubeHalfSize.X = FMath::Max(CubeHalfSize.X, 1.0);
	CubeHalfSize.Y = FMath::Max(CubeHalfSize.Y, 1.0);
	CubeHalfSize.Z = FMath::Max(CubeHalfSize.Z, 1.0);

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
	 * arena in front of them. Pair this with Trace.DebugTakeCore (and, if the near cull is in the way,
	 * Trace.Trail.OwnerHideCameraRadius 0) and a headless run can photograph the after-images head-on
	 * — including the predicted head stub, which only this viewer can see.
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

	// =============================================================================================
	// SPEC v5 §2 VERIFICATION. Two commands, because the fix makes two claims and they are different
	// kinds of claim:
	//
	//   Trace.Trail.DebugHeadGap  — MEASURES the gap the user reported, in uu, on the live carrier.
	//                               "The trace reaches their feet" is a number, so it is printed as
	//                               one rather than argued from a screenshot.
	//   Trace.Trail.TestHeadGap   — DASHES an enemy through the NEWEST DRAWN segment and reports
	//                               whether the carrier died. This is the half that matters for the
	//                               inverse hazard: the newest thing an enemy can see must still
	//                               kill, and the prediction must not have moved where that is.
	// =============================================================================================

	UWorld* FindTrailDebugWorld()
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

	void GatherTrailDebugCharacters(UWorld* World, TArray<ATraceCharacter*>& OutCharacters)
	{
		OutCharacters.Reset();
		if (World == nullptr)
		{
			return;
		}
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			if (ATraceCharacter* TraceChar = *It)
			{
				OutCharacters.Add(TraceChar);
			}
		}
	}

	/** True when this pawn is the one a human is playing on THIS machine — the case in the report. */
	bool IsLocallyControlledHuman(const ATraceCharacter* TraceChar)
	{
		if (TraceChar == nullptr)
		{
			return false;
		}
		const APlayerController* PC = Cast<APlayerController>(TraceChar->GetController());
		return PC != nullptr && PC->IsLocalPlayerController();
	}

	/** One line per trace that exists: the reported gap, and what the prediction is covering of it. */
	void DumpHeadGap()
	{
		UWorld* World = FindTrailDebugWorld();
		TArray<ATraceCharacter*> Characters;
		GatherTrailDebugCharacters(World, Characters);

		UE_LOG(LogTraceGame, Display,
			TEXT("[HEADGAP] ownerHideCameraRadius=%.0f ownerGhostHideDistance=%.0f predictMaxLength=%.0f (spec v5 2). "
			     "gapToFeet = uu from the newest LETHAL point to the carrier's feet, along the path. "
			     "predicted = uu of that the owner-only stub draws. "
			     "SEEN = uu to the nearest piece of their own trace the carrier's camera may actually see "
			     "(-1 = nothing drawn); SEEN-nopredict = the same with the prediction ignored. "
			     "SEEN is the number the bug report is about."),
			GOwnerHideCameraRadius, GOwnerGhostHideDistance, GPredictedHeadMaxLength);

		for (const ATraceCharacter* TraceChar : Characters)
		{
			const UTraceTrailComponent* Trail = (TraceChar != nullptr) ? TraceChar->Trail : nullptr;
			if (Trail == nullptr || Trail->TrailPoints.Items.Num() == 0)
			{
				continue;
			}

			const float Gap = Trail->MeasureHeadGap();
			const float Predicted = Trail->GetPredictedHeadLength();

			UE_LOG(LogTraceGame, Display,
				TEXT("[HEADGAP]   %-26s localHuman=%d carrier=%d points=%3d lethal=%3d | "
				     "gapToFeet=%6.1fuu predicted=%6.1fuu | SEEN=%7.1fuu SEEN-nopredict=%7.1fuu"),
				*GetNameSafe(TraceChar), IsLocallyControlledHuman(TraceChar) ? 1 : 0,
				TraceChar->IsCarrier() ? 1 : 0,
				Trail->TrailPoints.Items.Num(), Trail->ComputeLastLethalIndex() + 1,
				Gap, Predicted,
				Trail->MeasureOwnerVisibleGap(/*bIncludePredictedHead=*/true),
				Trail->MeasureOwnerVisibleGap(/*bIncludePredictedHead=*/false));
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.TetherCheck — SPEC v7 §1 AND §7 IN ONE LINE PER TRACE, AND IT HAS TO BE RUN ON THE
	// CLIENT.
	//
	// TAIL is the number the §7 bug report is about: the distance from the carrier to the OLDEST
	// point in the array, i.e. to the far end of everything drawn off it. A healthy trace reads
	// roughly its own length (~1200uu). With the tether, the fast array's RemoveAtSwap has put the
	// NEWEST point — the one at the carrier's feet — into slot 0, so TAIL collapses towards zero
	// while LEN stays long: the far end has been pulled back onto the character, which is exactly
	// what was described. ORD counts adjacent pairs still out of chronological order, and FIX counts
	// how many times this machine has had to put the array back together.
	//
	// Run it with Trace.Trail.ClientOrderFix 0 to see the failure, and with 1 to see it gone. LEN
	// doubles as the §1/§2 readout: it must sit at or just under the max length and must NOT fall
	// when a carrier stops moving.
	// ---------------------------------------------------------------------------------------------
	void DumpTetherCheck()
	{
		UWorld* World = FindTrailDebugWorld();
		TArray<ATraceCharacter*> Characters;
		GatherTrailDebugCharacters(World, Characters);

		const TCHAR* NetRole = TEXT("unknown");
		if (World != nullptr)
		{
			switch (World->GetNetMode())
			{
			case NM_Standalone:    NetRole = TEXT("standalone"); break;
			case NM_ListenServer:  NetRole = TEXT("HOST");       break;
			case NM_Client:        NetRole = TEXT("CLIENT");     break;
			case NM_DedicatedServer: NetRole = TEXT("dedicated"); break;
			default: break;
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[TETHER] netmode=%s orderFix=%d maxLen=%.0fuu radius=%.1fuu height=%.1fuu (spec v7 1-3,7). "
			     "LEN = replicated path length. TAIL = uu from the carrier to the OLDEST point, i.e. to the "
			     "far end of the drawn ribbon - it collapses towards 0 when the tether bug is present. "
			     "ORD = adjacent pairs out of chronological order. FIX = order repairs this machine has made."),
			NetRole, GClientOrderFix, UTraceTrailComponent::GetTraceMaxLengthUU(),
			UTraceTrailComponent::GetTraceTrailRadius(), UTraceTrailComponent::GetTraceTrailHeight());

		for (const ATraceCharacter* TraceChar : Characters)
		{
			const UTraceTrailComponent* Trail = (TraceChar != nullptr) ? TraceChar->Trail : nullptr;
			if (Trail == nullptr || Trail->TrailPoints.Items.Num() == 0)
			{
				continue;
			}

			double MaxHalfWidth = 0.0;
			double MaxHalfHeight = 0.0;
			double WorstUncovered = 0.0;
			int32 VisiblePieces = 0;
			Trail->MeasureDrawnVolume(MaxHalfWidth, MaxHalfHeight, WorstUncovered, VisiblePieces);

			UE_LOG(LogTraceGame, Display,
				TEXT("[TETHER]   %-26s localHuman=%d carrier=%d points=%3d lethal=%3d | LEN=%7.1fuu "
				     "TAIL=%7.1fuu ORD=%d FIX=%d | drawn: pieces=%2d halfWidth=%5.1f halfHeight=%5.1f "
				     "worstUncoveredLethal=%5.2fuu"),
				*GetNameSafe(TraceChar), IsLocallyControlledHuman(TraceChar) ? 1 : 0,
				TraceChar->IsCarrier() ? 1 : 0,
				Trail->TrailPoints.Items.Num(), Trail->ComputeLastLethalIndex() + 1,
				Trail->MeasureTraceLength(), Trail->MeasureTailDistanceToCarrier(),
				Trail->CountPointOrderViolations(), Trail->GetPointOrderRepairCount(),
				VisiblePieces, MaxHalfWidth, MaxHalfHeight, WorstUncovered);
		}
	}

	FAutoConsoleCommand CmdTetherCheck(
		TEXT("Trace.Trail.TetherCheck"),
		TEXT("Trace.Trail.TetherCheck [IntervalSeconds] [Samples] - per trace: path length, distance from "
		     "the carrier to the FAR end of it, replicated-order violations, and the drawn cross-section "
		     "against the lethal one (spec v7 1-3, 7). Run it ON THE CLIENT."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Interval = (Args.Num() > 0) ? FMath::Max(0.05f, FCString::Atof(*Args[0])) : 0.f;
			const int32 Samples = (Args.Num() > 1) ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 500) : 1;

			if (Interval <= 0.f || Samples <= 1)
			{
				DumpTetherCheck();
				return;
			}

			int32 Remaining = Samples;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Remaining](float /*DeltaTime*/) mutable -> bool
				{
					DumpTetherCheck();
					return --Remaining > 0;
				}), Interval);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.Geometry — spec v7 §3's invariant, stated as a PASS or a FAIL rather than as a
	// screenshot. Both directions, because only one of them is the obvious one:
	//
	//   DRAWN <= LETHAL   no visible piece is wider or taller than the volume that kills. Failing
	//                     this is visible ribbon that cannot kill.
	//   LETHAL <= DRAWN   every lethal point is inside something on screen. Failing this is an
	//                     invisible kill volume, which is the worse of the two.
	//
	// It also prints where each number came from, because v7 §3's values live in this file's CVars
	// until the integrator moves them into UTraceSettings, and a shadowed settings knob that silently
	// does nothing is a failure mode this project has already paid for once.
	// ---------------------------------------------------------------------------------------------
	FAutoConsoleCommand CmdTrailGeometry(
		TEXT("Trace.Trail.Geometry"),
		TEXT("Trace.Trail.Geometry - print the resolved lethal cross-section and max trace length, where "
		     "each number came from, and whether the DRAWN volume matches the LETHAL one in both "
		     "directions (spec v7 1-3)."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			const UTraceSettings& Settings = UTraceSettings::Get();

			const float Radius = UTraceTrailComponent::GetTraceTrailRadius();
			const float Height = UTraceTrailComponent::GetTraceTrailHeight();
			const float MaxLength = UTraceTrailComponent::GetTraceMaxLengthUU();

			UE_LOG(LogTraceGame, Display,
				TEXT("[TRAILGEOM] LETHAL == DRAWN cross-section: radius=%.2fuu (%s; UTraceSettings::TrailRadius=%.1f) "
				     "height=%.2fuu (%s; UTraceSettings::TrailHeight=%.1f) | maxLength=%.0fuu (%s; "
				     "TrailLifetime=%.2f x WalkSpeed=%.0f x 0.75 = %.0f) | spacing=%.0fuu -> ~%d points"),
				Radius, (GTrailRadiusOverride >= 0.f) ? TEXT("Trace.Trail.Radius") : TEXT("settings"),
				Settings.TrailRadius,
				Height, (GTrailHeightOverride >= 0.f) ? TEXT("Trace.Trail.Height") : TEXT("settings"),
				Settings.TrailHeight,
				MaxLength, (GTrailMaxLength > 0.f) ? TEXT("Trace.Trail.MaxLength")
					: ((Settings.TrailMaxLengthUU > 0.f) ? TEXT("settings::TrailMaxLengthUU") : TEXT("derived")),
				Settings.TrailLifetime, Settings.WalkSpeed,
				Settings.TrailLifetime * Settings.WalkSpeed * 0.75f,
				Settings.TrailPointSpacing,
				FMath::CeilToInt(MaxLength / FMath::Max(1.f, Settings.TrailPointSpacing)));

			UWorld* World = FindTrailDebugWorld();
			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);

			int32 Checked = 0;
			int32 Failed = 0;

			for (const ATraceCharacter* TraceChar : Characters)
			{
				const UTraceTrailComponent* Trail = (TraceChar != nullptr) ? TraceChar->Trail : nullptr;
				if (Trail == nullptr || Trail->ComputeLastLethalIndex() < 0)
				{
					continue;
				}

				double MaxHalfWidth = 0.0;
				double MaxHalfHeight = 0.0;
				double WorstUncovered = 0.0;
				int32 VisiblePieces = 0;
				Trail->MeasureDrawnVolume(MaxHalfWidth, MaxHalfHeight, WorstUncovered, VisiblePieces);

				if (VisiblePieces == 0)
				{
					continue;   // Nothing on screen to compare against (dedicated server, or renderer 2).
				}

				++Checked;

				// One uu of slack: the alternating anti-Z-fight inset makes every other element 0.6%
				// smaller, and a segment the carrier JUMPED along is deliberately drawn over the union
				// of its two ends' vertical bands, so the height may legitimately exceed the flat
				// cross-section by the segment's own rise. Width has no such allowance.
				const double WidthLimit = static_cast<double>(Radius) + 1.0;
				const bool bWidthOk = MaxHalfWidth <= WidthLimit;
				const bool bCoverOk = WorstUncovered <= 1.0;

				if (!bWidthOk || !bCoverOk)
				{
					++Failed;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[TRAILGEOM]   %-26s lethalPoints=%3d visiblePieces=%2d | drawn halfWidth=%5.2f "
					     "(lethal %5.2f) %s | drawn halfHeight=%5.2f (lethal flat %5.2f, +rise on jumps) | "
					     "worstUncoveredLethalPoint=%5.2fuu %s"),
					*GetNameSafe(TraceChar), Trail->ComputeLastLethalIndex() + 1, VisiblePieces,
					MaxHalfWidth, static_cast<double>(Radius), bWidthOk ? TEXT("OK") : TEXT("** WIDER THAN LETHAL **"),
					MaxHalfHeight, static_cast<double>(Height) * 0.5,
					WorstUncovered, bCoverOk ? TEXT("OK") : TEXT("** LETHAL BUT NOT DRAWN **"));
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[TRAILGEOM] %s - %d trace(s) checked, %d violation(s)."),
				(Checked > 0 && Failed == 0) ? TEXT("PASS") : (Checked == 0 ? TEXT("NO DATA") : TEXT("FAIL")),
				Checked, Failed);
		}));

	FAutoConsoleCommand CmdDebugHeadGap(
		TEXT("Trace.Trail.DebugHeadGap"),
		TEXT("Trace.Trail.DebugHeadGap [IntervalSeconds] [Samples] - measure the gap between a carrier and "
		     "the end of their own drawn trace, and how much of it the owner-only predicted head is covering "
		     "(spec v5 2). With arguments it repeats."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float Interval = (Args.Num() > 0) ? FMath::Max(0.05f, FCString::Atof(*Args[0])) : 0.f;
			const int32 Samples = (Args.Num() > 1) ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 500) : 1;

			if (Interval <= 0.f || Samples <= 1)
			{
				DumpHeadGap();
				return;
			}

			int32 Remaining = Samples;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Remaining](float /*DeltaTime*/) mutable -> bool
				{
					DumpHeadGap();
					return --Remaining > 0;
				}), Interval);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.TestHeadGap — DOES THE NEWEST VISIBLE SEGMENT STILL KILL?
	//
	// Same two-teleport dash as Trace.TestParry (see the long comment there for why a synthesised
	// sweep is the right shape of test), aimed at a deliberately different segment: the LAST one in
	// the lethal set, i.e. the newest thing that is drawn on an enemy's screen. That is the segment
	// the prediction sits immediately in front of, so it is the one where a mistake in this pass
	// would show up as "I dashed through the freshest part of the trace and lived".
	//
	// It also prints the gap measurement for the carrier at the moment of the dash, so one run
	// answers both halves of the request: the trace reaches the carrier's feet, AND the newest
	// segment an enemy can see is still lethal.
	// ---------------------------------------------------------------------------------------------

	struct FHeadGapTestState
	{
		int32 TotalRuns = 4;
		int32 RunIndex = 0;
		int32 Phase = 0;

		int32 WaitFrames = 0;
		int32 IdleFrames = 0;
		int32 ScratchCount = 0;

		TWeakObjectPtr<ATraceCharacter> Carrier;
		TWeakObjectPtr<ATraceCharacter> Tripper;
		FVector DashEnd = FVector::ZeroVector;
		FVector TripperHome = FVector::ZeroVector;

		/** Snapshotted before the sweep, because an unparried dash deletes the carrier we measured. */
		float GapAtSweep = 0.f;
		float PredictedAtSweep = 0.f;
		int32 LethalAtSweep = 0;
		bool bCarrierWasLocalHuman = false;

		/**
		 * DID THE CARRIER SPEND ANY PART OF THIS RUN INSIDE A DESIGNED INVULNERABILITY WINDOW?
		 *
		 * THIS FIELD EXISTS BECAUSE THE HARNESS WAS CALLING CORRECT BEHAVIOUR A FAILURE. Scoring was
		 * a bare `Carrier->IsAlive()`, which cannot tell "the newest visible segment is not lethal"
		 * (a real bug, and the whole point of this test) apart from "the trip test deliberately
		 * declined to kill" — and there are exactly two ways it declines, both of them shipped rules:
		 *
		 *   * spec v4 §4, THE PASS WINDOW. From the instant the holder inputs a pass until it
		 *     completes or cancels, the trace CANNOT be broken. That is the risk beat the passer is
		 *     paid for, and a bot carrier hovering a pass is a completely ordinary thing to catch.
		 *   * spec v3 §3, THE PARRY. 0.1s, carrier-only. Trace.TestParry already models this one;
		 *     this harness did not.
		 *
		 * Measured on the shipping build: 51 PASS / 2 FAIL over 53 runs, and BOTH "failures" have the
		 * engine's own verdict logged one line above them — "[TRACEDASH] ... NO KILL - pass window
		 * invulnerable (spec 4) ... passWindow=1", followed 21 ms later by "Core: pass completed".
		 * The trace was fine; the scoreboard was wrong. An unattributed intermittent failure on the
		 * project's headline lethality invariant is exactly the kind of noise that gets a real
		 * regression waved through later, so the run is now EXEMPTED and says why.
		 *
		 * Sampled across phases 1-3 rather than once: the window can open or close between the frame
		 * the dash is issued and the frame the sweep resolves, and either overlap is enough to make
		 * the outcome unusable as evidence about geometry.
		 */
		bool bInvulnerableAtSweep = false;

		int32 KilledCount = 0;
		int32 SurvivedCount = 0;
		int32 AbortedCount = 0;
		int32 ExemptCount = 0;
	};

	/** True while the trip test is under standing orders NOT to kill this carrier (v4 §4 / v3 §3). */
	bool IsCarrierInvulnerableByDesign(const ATraceCharacter* Carrier)
	{
		const UTraceTrailComponent* Trail = (Carrier != nullptr) ? Carrier->Trail : nullptr;
		return (Trail != nullptr)
			&& (Trail->IsPassWindowInvulnerable() || Trail->IsParryActive());
	}

	bool TickHeadGapTest(FHeadGapTestState& State)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		if (State.RunIndex >= State.TotalRuns)
		{
			// The verdict is over SCORED runs only. Exempt and aborted runs are reported separately
			// and excluded from the denominator, because neither one measured the thing.
			const int32 Scored = State.KilledCount + State.SurvivedCount;

			UE_LOG(LogTraceGame, Display,
				TEXT("[HEADGAPTEST] DONE. %d of %d SCORED dashes through the NEWEST DRAWN segment killed the "
				     "carrier, %d did not. (%d exempt: carrier inside the pass window or a parry, spec v4 4 / "
				     "v3 3; %d aborted; %d runs requested.) The newest visible segment is lethal iff "
				     "killed == scored. %s"),
				State.KilledCount, Scored, State.SurvivedCount,
				State.ExemptCount, State.AbortedCount, State.TotalRuns,
				(Scored > 0 && State.KilledCount == Scored)
					? TEXT("VERDICT: PASS.")
					: (Scored == 0
						? TEXT("VERDICT: NO DATA - every run was exempt or aborted.")
						: TEXT("VERDICT: *** FAIL ***.")));
			return false;
		}

		// ---- phase 0: find a living carrier with enough trace to aim at -------------------------
		if (State.Phase == 0)
		{
			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);

			ATraceCharacter* Carrier = nullptr;
			int32 LethalPoints = 0;
			for (ATraceCharacter* TraceChar : Characters)
			{
				if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive()
					&& TraceChar->Trail != nullptr)
				{
					// PREFER THE LOCAL HUMAN. The bug is about the local player's trace, so if the
					// local player is carrying, that is the trace to test — but a bot carrier is
					// still a valid subject for the lethality half and keeps an unattended run
					// producing samples.
					const int32 Lethal = TraceChar->Trail->ComputeLastLethalIndex() + 1;
					if (Lethal > LethalPoints || (Carrier != nullptr && IsLocallyControlledHuman(TraceChar)
						&& !IsLocallyControlledHuman(Carrier)))
					{
						Carrier = TraceChar;
						LethalPoints = Lethal;
					}
				}
			}

			if (Carrier == nullptr || LethalPoints < 4)
			{
				if (Carrier == nullptr)
				{
					if (++State.IdleFrames > 36000)
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[HEADGAPTEST] gave up: no living Core carrier ever appeared."));
						return false;
					}
					return true;
				}

				if (++State.WaitFrames > 1800)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[HEADGAPTEST] gave up waiting for a carrier with 4+ lethal trace points (had %d)."),
						LethalPoints);
					return false;
				}
				return true;
			}

			State.IdleFrames = 0;
			State.WaitFrames = 0;
			State.Carrier = Carrier;
			State.Phase = 1;
			return true;
		}

		ATraceCharacter* Carrier = State.Carrier.Get();

		if (State.Phase <= 2 && (Carrier == nullptr || !Carrier->IsAlive() || Carrier->Trail == nullptr))
		{
			UE_LOG(LogTraceGame, Display, TEXT("[HEADGAPTEST %d] aborted: carrier gone before the sweep resolved."),
				State.RunIndex + 1);
			++State.AbortedCount;
			++State.RunIndex;
			State.Phase = 0;
			State.Carrier = nullptr;
			State.Tripper = nullptr;
			return true;
		}

		// ---- phase 1: line the tripper up across the NEWEST lethal segment ----------------------
		if (State.Phase == 1)
		{
			const int32 LastLethal = Carrier->Trail->ComputeLastLethalIndex();
			if (LastLethal < 1)
			{
				State.Phase = 0;
				return true;
			}

			// THE NEWEST DRAWN SEGMENT, not a comfortable one in the middle. This is the segment the
			// predicted stub joins onto, and the one an enemy chasing a carrier actually meets.
			const int32 SegmentIndex = LastLethal - 1;
			const FVector SegmentStart = Carrier->Trail->TrailPoints.Items[SegmentIndex].Location;
			const FVector SegmentEnd = Carrier->Trail->TrailPoints.Items[SegmentIndex + 1].Location;

			FVector Along = SegmentEnd - SegmentStart;
			Along.Z = 0.0;
			if (!Along.Normalize())
			{
				State.Phase = 0;
				return true;
			}
			const FVector Across = FVector::CrossProduct(Along, FVector::UpVector).GetSafeNormal();
			const FVector Midpoint = (SegmentStart + SegmentEnd) * 0.5;

			TArray<ATraceCharacter*> Candidates;
			GatherTrailDebugCharacters(World, Candidates);

			TArray<ATraceCharacter*> Enemies;
			for (ATraceCharacter* Candidate : Candidates)
			{
				if (Candidate != nullptr && Candidate != Carrier && Candidate->IsAlive()
					&& Candidate->GetTeam() != ETraceTeam::None
					&& Candidate->GetTeam() != Carrier->GetTeam())
				{
					Enemies.Add(Candidate);
				}
			}
			if (Enemies.Num() == 0)
			{
				State.Phase = 0;
				return true;
			}

			// Rotate through the roster on retries — one pawn's dash cooldown must not delete a run.
			ATraceCharacter* Tripper = Enemies[State.ScratchCount % Enemies.Num()];

			State.GapAtSweep = Carrier->Trail->MeasureHeadGap();
			State.PredictedAtSweep = Carrier->Trail->GetPredictedHeadLength();
			State.LethalAtSweep = LastLethal + 1;
			State.bCarrierWasLocalHuman = IsLocallyControlledHuman(Carrier);
			State.bInvulnerableAtSweep = IsCarrierInvulnerableByDesign(Carrier);

			const FVector DashStart = Midpoint + Across * 260.0;
			State.DashEnd = Midpoint - Across * 240.0;
			State.Tripper = Tripper;
			State.TripperHome = Tripper->GetActorLocation();

			Tripper->SetActorLocation(DashStart, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
			Tripper->SetActorRotation((-Across).Rotation());
			if (UTraceCharacterMovementComponent* TripperMovement = Tripper->GetTraceMovement())
			{
				TripperMovement->StartDash();
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[HEADGAPTEST %d/%d] carrier=%s localHuman=%d lethal=%d | gapToFeet=%.1fuu predicted=%.1fuu "
				     "residual=%.1fuu | %s will dash across the NEWEST segment %d/%d at %s"),
				State.RunIndex + 1, State.TotalRuns, *GetNameSafe(Carrier),
				State.bCarrierWasLocalHuman ? 1 : 0, State.LethalAtSweep,
				State.GapAtSweep, State.PredictedAtSweep, FMath::Max(0.f, State.GapAtSweep - State.PredictedAtSweep),
				*GetNameSafe(Tripper), SegmentIndex, LastLethal, *Midpoint.ToCompactString());

			State.Phase = 2;
			return true;
		}

		// ---- phase 2: the frame the trip test resolves ------------------------------------------
		if (State.Phase == 2)
		{
			// Re-sampled here, not only when the dash was issued: this is the frame the sweep is
			// actually evaluated, so this is the reading that decides whether the outcome is evidence.
			State.bInvulnerableAtSweep |= IsCarrierInvulnerableByDesign(State.Carrier.Get());

			ATraceCharacter* Tripper = State.Tripper.Get();
			if (Tripper == nullptr)
			{
				State.Phase = 0;
				return true;
			}

			// A run where the dash was refused measures nothing at all, and a negative produced by an
			// absent dash is worse than no result. Scratch and retry on another pawn.
			if (!Tripper->IsDashing())
			{
				if (++State.ScratchCount > 20)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[HEADGAPTEST %d] aborted: %s would not dash after %d attempts (cooldown?)."),
						State.RunIndex + 1, *GetNameSafe(Tripper), State.ScratchCount);
					++State.AbortedCount;
					++State.RunIndex;
					State.ScratchCount = 0;
				}

				Tripper->SetActorLocation(State.TripperHome, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
				State.Phase = 0;
				State.Carrier = nullptr;
				State.Tripper = nullptr;
				return true;
			}

			Tripper->SetActorLocation(State.DashEnd, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
			State.Phase = 3;
			return true;
		}

		// ---- phase 3: off the trace, so the run measures ONE sweep ------------------------------
		if (State.Phase == 3)
		{
			State.bInvulnerableAtSweep |= IsCarrierInvulnerableByDesign(State.Carrier.Get());

			if (ATraceCharacter* Tripper = State.Tripper.Get())
			{
				Tripper->SetActorLocation(State.TripperHome, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
			}
			State.Phase = 4;
			return true;
		}

		// ---- phase 4+: let the kill land, then score --------------------------------------------
		if (State.Phase < 7)
		{
			++State.Phase;
			return true;
		}

		{
			const bool bCarrierAlive = (Carrier != nullptr) && Carrier->IsAlive();

			// A survival inside the pass window or the parry is the trip test obeying spec v4 §4 /
			// v3 §3, not the geometry failing — see FHeadGapTestState::bInvulnerableAtSweep. It is
			// EXEMPTED rather than passed: the run produced no evidence either way about whether the
			// newest visible segment is lethal, and counting it as a PASS would let a genuinely dead
			// segment hide behind a carrier who happened to be passing.
			//
			// A survival is only ever exempted. A KILL is scored normally however invulnerable the
			// carrier was supposed to be — a kill that lands inside a protection window is a real
			// defect and this harness must not become the place it goes unnoticed.
			const bool bExempt = bCarrierAlive && State.bInvulnerableAtSweep;

			if (bExempt)
			{
				++State.ExemptCount;
			}
			else if (bCarrierAlive)
			{
				++State.SurvivedCount;
			}
			else
			{
				++State.KilledCount;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[HEADGAPTEST %d/%d] RESULT: dash through the NEWEST DRAWN segment -> carrier %s. %s "
				     "(gapToFeet was %.1fuu, predicted %.1fuu)"),
				State.RunIndex + 1, State.TotalRuns,
				bCarrierAlive ? TEXT("SURVIVED") : TEXT("DIED (trace broken)"),
				bExempt
					? TEXT("EXEMPT - the carrier was inside the pass window or a parry, so the trip test "
					       "was under orders not to kill (spec v4 4 / v3 3). Not evidence either way; "
					       "this run is not scored")
					: (bCarrierAlive
						? TEXT("*** FAIL - the newest visible segment did not kill ***")
						: TEXT("PASS")),
				State.GapAtSweep, State.PredictedAtSweep);

			++State.RunIndex;
			State.bInvulnerableAtSweep = false;
			State.ScratchCount = 0;
			State.Phase = 0;
			State.Carrier = nullptr;
			State.Tripper = nullptr;
		}

		return true;
	}

	// =============================================================================================
	// SPEC v6 §1 — THE MEASUREMENT. "WAY laggier, with 1/6 the fps and its unplayable."
	//
	// That report is a number, so the answer has to be a number, taken on the SAME SCENE in the SAME
	// PROCESS. Rebuilding the old binary and eyeballing two separate runs would compare two different
	// bot matches as much as two renderers, so instead Trace.Trail.Renderer switches the renderer at
	// runtime and this command cycles it:
	//
	//   arm "legacy"  Trace.Trail.Renderer 0 — spec v4 §2: up to 20 posed Mannequins per carrier plus
	//                                          a two-part cylinder smear. THE BEFORE.
	//   arm "ribbon"  Trace.Trail.Renderer 1 — spec v6 §2: one swept rectangle. THE AFTER.
	//   arm "off"     Trace.Trail.Renderer 2 — nothing drawn. THE BOUND: whatever this arm does not
	//                                          recover was never the trace's fault, and that is the
	//                                          more valuable finding of the two.
	//
	// WHAT IS SAMPLED, and it is deliberately the same four numbers `stat unit` shows, from the same
	// globals `stat unit` reads:
	//   Frame  — real wall-clock frame time from the ticker's own delta, which is the one number the
	//            user's "1/6 the fps" is about.
	//   Game   — GGameThreadTime.     Render — GRenderThreadTime.     GPU — RHIGetGPUFrameCycles().
	// Reporting the split is the point: 20 skinned meshes would show up as GPU and render-thread
	// cost, whereas an arena with 1187 components or a per-frame line trace shows up as game thread,
	// and those are different bugs with different owners.
	//
	// Each arm gets a warm-up before it is sampled, because the first frames after a renderer switch
	// pay for pool destruction and component creation and are not the steady state anybody plays in.
	// =============================================================================================

	struct FTrailPerfArm
	{
		int32 Renderer = 1;
		FString Name;

		TArray<float> FrameMs;
		double GameMsSum = 0.0;
		double RenderMsSum = 0.0;
		double GpuMsSum = 0.0;
		double TrailMsSum = 0.0;
		double StaticPieces = 0.0;
		double SkinnedPieces = 0.0;
		double Carriers = 0.0;
		int32 Frames = 0;
	};

	float TrailPerfPercentile(TArray<float>& Values, float Fraction)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(FMath::RoundToInt(Fraction * (Values.Num() - 1)), 0, Values.Num() - 1);
		return Values[Index];
	}

	float TrailPerfMean(const TArray<float>& Values)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		double Total = 0.0;
		for (const float Value : Values)
		{
			Total += Value;
		}
		return static_cast<float>(Total / Values.Num());
	}

	struct FTrailPerfState
	{
		TArray<FTrailPerfArm> Arms;
		int32 ArmIndex = 0;
		float WarmupSeconds = 3.f;
		float SampleSeconds = 15.f;
		float PhaseElapsed = 0.f;
		bool bWarming = true;
		bool bStarted = false;
		int32 RestoreRenderer = 1;

		/**
		 * HOW MANY TIMES THE THREE ARMS ARE CYCLED, and this is not a nicety either.
		 *
		 * Run once, the arms are three CONSECUTIVE time windows, so anything that drifts over a match
		 * lands entirely on whichever arm was running. Measured: at 16x resolution a single pass gave
		 * legacy 39.7 ms, ribbon 42.7, off 45.6 — a perfectly monotonic ramp that says the LAST arm
		 * is slowest whatever it is drawing, which is a statement about the match and not about the
		 * renderer. (Drawing nothing "cost" 5.9 ms more than twenty skinned Mannequins.)
		 *
		 * Interleaving legacy/ribbon/off/legacy/ribbon/off/... spreads that drift evenly across all
		 * three buckets, which is the whole difference between a comparison and a coincidence.
		 */
		int32 CyclesTotal = 3;
		int32 CycleIndex = 0;

		/** Characterises the scene once, so the numbers can be read months from now. */
		int32 PrimitiveCount = 0;
		int32 CharacterCount = 0;
	};

	/**
	 * DID THE FRAME RATE ACTUALLY GET UNCAPPED? Returns the refresh rate it is pinned to, or 0.
	 *
	 * TickTrailPerf asks for an uncapped frame four different ways (t.MaxFPS, r.VSync,
	 * rhi.SyncInterval, bSmoothFrameRate). ON MACOS NONE OF THEM WORK: Metal paces the present to the
	 * display whatever the RHI is told, and -RenderOffScreen still presents. Measured on this machine
	 * at 1280x720: legacy 8.34 ms, ribbon 8.35 ms, off 8.34 ms — three arms, one of which DRAWS
	 * NOTHING AT ALL, agreeing to a hundredth of a millisecond at exactly 1000/120.
	 *
	 * THAT IS NOT A RESULT, IT IS THE ABSENCE OF ONE, and the two are indistinguishable in the table:
	 * both look like "the renderer does not matter". A whole verification pass has already concluded
	 * "the trace was never the cause" from precisely those numbers. The conclusion happened to be
	 * right — re-measured GPU-bound, the entire trace is under 1% of the frame — but it was not
	 * SUPPORTED, because a capped run cannot tell a free renderer from an expensive one.
	 *
	 * So the harness now says so itself. The test needs both halves:
	 *   * every arm's mean within CapAgreementTolerance of every other. A genuine tie is possible, so
	 *     this alone must never be enough to cry cap;
	 *   * and that shared mean sitting on a plausible refresh period. THIS is the specific half. At
	 *     3x screen percentage the same three arms came back 23.17 / 23.34 / 23.13 — a 0.9% spread
	 *     that passes the first test and is nowhere near any refresh rate, so it is correctly reported
	 *     as the real measurement it is.
	 */
	float TrailPerfDetectFrameRateCap(const FTrailPerfState& State)
	{
		// Fraction by which the arms' mean frame times may differ and still count as "identical".
		constexpr float CapAgreementTolerance = 0.02f;
		// Fraction by which the shared mean may differ from a refresh period and still count as it.
		constexpr float CapRefreshTolerance = 0.03f;

		float MinMean = TNumericLimits<float>::Max();
		float MaxMean = 0.f;
		for (const FTrailPerfArm& Arm : State.Arms)
		{
			const float Mean = TrailPerfMean(Arm.FrameMs);
			if (Mean <= 0.f)
			{
				return 0.f;   // An arm with no frames: nothing to conclude either way.
			}
			MinMean = FMath::Min(MinMean, Mean);
			MaxMean = FMath::Max(MaxMean, Mean);
		}

		if (MinMean <= 0.f || ((MaxMean - MinMean) / MinMean) > CapAgreementTolerance)
		{
			return 0.f;   // The arms genuinely differ, so nothing is holding them together.
		}

		static const float CommonRefreshRates[] = { 30.f, 50.f, 60.f, 72.f, 75.f, 90.f, 100.f, 120.f, 144.f, 165.f, 240.f };
		for (const float Hz : CommonRefreshRates)
		{
			const float Period = 1000.f / Hz;
			if (FMath::Abs(MinMean - Period) / Period <= CapRefreshTolerance)
			{
				return Hz;
			}
		}

		return 0.f;
	}

	void TrailPerfBeginArm(FTrailPerfState& State)
	{
		GTrailRenderer = State.Arms[State.ArmIndex].Renderer;
		State.bWarming = true;
		State.PhaseElapsed = 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("[TRAILPERF] arm '%s' (Trace.Trail.Renderer %d): warming %.1fs, then sampling %.1fs."),
			*State.Arms[State.ArmIndex].Name, State.Arms[State.ArmIndex].Renderer,
			State.WarmupSeconds, State.SampleSeconds);
	}

	void TrailPerfReport(FTrailPerfState& State)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[TRAILPERF] ===== RESULTS. scene: %d primitive components, %d characters. "
			     "ms lower is better; fps from the MEAN frame time. ====="),
			State.PrimitiveCount, State.CharacterCount);
		UE_LOG(LogTraceGame, Display,
			TEXT("[TRAILPERF] %-8s %6s | %8s %8s %8s | %8s %8s %8s %8s | %7s %8s"),
			TEXT("arm"), TEXT("frames"), TEXT("frameMs"), TEXT("medMs"), TEXT("p95Ms"),
			TEXT("gameMs"), TEXT("rendMs"), TEXT("gpuMs"), TEXT("traceMs"), TEXT("fps"), TEXT("pieces"));

		float BaselineFrame = 0.f;
		for (FTrailPerfArm& Arm : State.Arms)
		{
			const float MeanFrame = TrailPerfMean(Arm.FrameMs);
			const float MedianFrame = TrailPerfPercentile(Arm.FrameMs, 0.50f);
			const float P95Frame = TrailPerfPercentile(Arm.FrameMs, 0.95f);
			const int32 SafeFrames = FMath::Max(1, Arm.Frames);

			if (Arm.Renderer == 0)
			{
				BaselineFrame = MeanFrame;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[TRAILPERF] %-8s %6d | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f %8.3f | %7.1f %5.1f static + %.1f skinned"),
				*Arm.Name, Arm.Frames, MeanFrame, MedianFrame, P95Frame,
				Arm.GameMsSum / SafeFrames, Arm.RenderMsSum / SafeFrames, Arm.GpuMsSum / SafeFrames,
				Arm.TrailMsSum / SafeFrames,
				(MeanFrame > 0.f) ? (1000.f / MeanFrame) : 0.f,
				Arm.StaticPieces / SafeFrames, Arm.SkinnedPieces / SafeFrames);
		}

		if (BaselineFrame > 0.f)
		{
			for (FTrailPerfArm& Arm : State.Arms)
			{
				if (Arm.Renderer == 0)
				{
					continue;
				}
				const float MeanFrame = TrailPerfMean(Arm.FrameMs);
				UE_LOG(LogTraceGame, Display,
					TEXT("[TRAILPERF] '%s' vs 'legacy': %+.2f ms/frame (%+.1f%%), %.2fx the frame rate."),
					*Arm.Name, MeanFrame - BaselineFrame,
					100.f * (MeanFrame - BaselineFrame) / BaselineFrame,
					(MeanFrame > 0.f) ? (BaselineFrame / MeanFrame) : 0.f);
			}
		}

		// THE VALIDITY VERDICT, PRINTED LAST SO IT IS THE LINE UNDER THE TABLE. See
		// TrailPerfDetectFrameRateCap for why a table of near-identical arms is the one result that
		// must never be read at face value.
		if (const float CapHz = TrailPerfDetectFrameRateCap(State); CapHz > 0.f)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[TRAILPERF] *** THIS MEASUREMENT IS INVALID: every arm landed within 2%% of %.2f ms, "
				     "which is %.0f Hz. The frame is PACED TO THE DISPLAY, not to the renderer, so an arm "
				     "that draws NOTHING measures the same as one that draws twenty skinned meshes and the "
				     "comparison cannot detect a difference of any size. t.MaxFPS / r.VSync / "
				     "rhi.SyncInterval do NOT defeat this on macOS Metal, and -RenderOffScreen still "
				     "presents. ***"),
				1000.f / CapHz, CapHz);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[TRAILPERF] *** TO GET A REAL NUMBER, make the GPU the bottleneck so the cap stops "
				     "binding - e.g. run `r.ScreenPercentage 300` before Trace.Trail.PerfAB - and re-read "
				     "the table. Do NOT quote the numbers above. ***"));
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[TRAILPERF] Validity: the arms differ by more than the cap-detection tolerance and the "
				     "shared mean is not a display refresh period, so the frame was NOT paced and this "
				     "comparison could have detected a difference."));
		}

		UE_LOG(LogTraceGame, Display, TEXT("[TRAILPERF] ===== END. Trace.Trail.Renderer restored to %d. ====="),
			State.RestoreRenderer);
	}

	bool TickTrailPerf(FTrailPerfState& State, float DeltaTime)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return true;   // The map may still be loading.
		}

		if (!State.bStarted)
		{
			State.bStarted = true;

			// FStatUnitData is only filled while the overlay is enabled, so enable it HERE rather than
			// asking the caller to remember: a run that forgets produces a table of zeroes in three of
			// the columns, which looks like a measurement and is not one. `stat unit` is a toggle, so
			// this is done exactly once per PerfAB and the frame-time column never depends on it.
			if (GEngine != nullptr)
			{
				GEngine->Exec(World, TEXT("stat unit"));

				// AND UNCAP THE FRAME RATE, which two runs of this harness proved is not optional:
				// every arm came back at exactly 8.33 ms, because this machine's display runs at
				// 120 Hz and the swap chain was pacing to it. All three renderers then measure
				// identical and the comparison says nothing at all — a capped frame time measures
				// the cap. -RenderOffScreen does NOT bypass this; it still presents.
				//
				// All four levers, because they are four different caps: the engine's own limiter
				// (t.MaxFPS), the engine's frame smoothing, the fixed-timestep mode, and the RHI's
				// present interval (r.VSync / rhi.SyncInterval), and any one of them left on holds
				// the whole measurement at the refresh rate.
				GEngine->Exec(World, TEXT("t.MaxFPS 0"));
				GEngine->Exec(World, TEXT("r.VSync 0"));
				GEngine->Exec(World, TEXT("rhi.SyncInterval 0"));
				GEngine->bSmoothFrameRate = false;
				GEngine->bUseFixedFrameRate = false;
			}

			TrailPerfBeginArm(State);
			return true;
		}

		State.PhaseElapsed += DeltaTime;

		if (State.bWarming)
		{
			if (State.PhaseElapsed >= State.WarmupSeconds)
			{
				State.bWarming = false;
				State.PhaseElapsed = 0.f;
			}
			return true;
		}

		// ---- sample this frame -------------------------------------------------------------------
		FTrailPerfArm& Arm = State.Arms[State.ArmIndex];

		Arm.FrameMs.Add(DeltaTime * 1000.f);

		if (GEngine != nullptr && GEngine->GameViewport != nullptr)
		{
			if (const FStatUnitData* Unit = GEngine->GameViewport->GetStatUnitData())
			{
				Arm.GameMsSum += Unit->RawGameThreadTime;
				Arm.RenderMsSum += Unit->RawRenderThreadTime;
				Arm.GpuMsSum += Unit->RawGPUFrameTime[0];
			}
		}

		// This component's own game-thread cost, independent of whether `stat unit` is on. It is the
		// one slice of the frame this file is responsible for, so it is the one the report can be held
		// to; everything else in the table is context.
		Arm.TrailMsSum += GTrailVisualMillisecondsThisFrame;
		GTrailVisualMillisecondsThisFrame = 0.0;

		int32 StaticVisible = 0;
		int32 SkinnedVisible = 0;
		int32 Carriers = 0;
		int32 CharacterTotal = 0;
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			const ATraceCharacter* TraceChar = *It;
			if (TraceChar == nullptr)
			{
				continue;
			}
			++CharacterTotal;

			const UTraceTrailComponent* Trail = TraceChar->Trail;
			if (Trail == nullptr)
			{
				continue;
			}
			if (Trail->TrailPoints.Items.Num() > 0)
			{
				++Carriers;
			}

			int32 PieceStatic = 0;
			int32 PieceSkinned = 0;
			Trail->CountDrawnPieces(PieceStatic, PieceSkinned);
			StaticVisible += PieceStatic;
			SkinnedVisible += PieceSkinned;
		}

		Arm.StaticPieces += StaticVisible;
		Arm.SkinnedPieces += SkinnedVisible;
		Arm.Carriers += Carriers;
		++Arm.Frames;

		State.CharacterCount = CharacterTotal;
		if (State.PrimitiveCount == 0)
		{
			int32 Primitives = 0;
			for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
			{
				if (It->GetWorld() == World && It->IsRegistered())
				{
					++Primitives;
				}
			}
			State.PrimitiveCount = Primitives;
		}

		if (State.PhaseElapsed < State.SampleSeconds)
		{
			return true;
		}

		// ---- arm complete -------------------------------------------------------------------------
		UE_LOG(LogTraceGame, Display,
			TEXT("[TRAILPERF] arm '%s' done: %d frames, mean %.2f ms (%.1f fps), mean drawn pieces %.1f static "
			     "+ %.1f skinned across %.1f live traces."),
			*Arm.Name, Arm.Frames, TrailPerfMean(Arm.FrameMs),
			(TrailPerfMean(Arm.FrameMs) > 0.f) ? (1000.f / TrailPerfMean(Arm.FrameMs)) : 0.f,
			Arm.StaticPieces / FMath::Max(1, Arm.Frames), Arm.SkinnedPieces / FMath::Max(1, Arm.Frames),
			Arm.Carriers / FMath::Max(1, Arm.Frames));

		++State.ArmIndex;
		if (State.ArmIndex >= State.Arms.Num())
		{
			State.ArmIndex = 0;
			++State.CycleIndex;

			if (State.CycleIndex >= State.CyclesTotal)
			{
				GTrailRenderer = State.RestoreRenderer;
				TrailPerfReport(State);
				return false;
			}
		}

		TrailPerfBeginArm(State);
		return true;
	}

	FAutoConsoleCommand CmdTrailPerfAB(
		TEXT("Trace.Trail.PerfAB"),
		TEXT("Trace.Trail.PerfAB [SampleSeconds] [WarmupSeconds] [Cycles] - interleave the trace "
		     "renderer through legacy (posed Mannequins + smear), ribbon and off, Cycles times each, "
		     "sampling frame / game / render / GPU milliseconds, and print the comparison (spec v6 1)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FTrailPerfState State;
			State.SampleSeconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 2.f, 120.f) : 15.f;
			State.WarmupSeconds = (Args.Num() > 1) ? FMath::Clamp(FCString::Atof(*Args[1]), 0.5f, 30.f) : 3.f;
			State.CyclesTotal = (Args.Num() > 2) ? FMath::Clamp(FCString::Atoi(*Args[2]), 1, 20) : 3;
			State.RestoreRenderer = GTrailRenderer;

			// LEGACY FIRST, deliberately. It is the arm that allocates twenty poseable mesh components
			// per carrier, and running it first means the later arms are not measured against a heap
			// that is still warming up.
			FTrailPerfArm Legacy;
			Legacy.Renderer = 0;
			Legacy.Name = TEXT("legacy");
			FTrailPerfArm Ribbon;
			Ribbon.Renderer = 1;
			Ribbon.Name = TEXT("ribbon");
			FTrailPerfArm Off;
			Off.Renderer = 2;
			Off.Name = TEXT("off");

			State.Arms.Add(MoveTemp(Legacy));
			State.Arms.Add(MoveTemp(Ribbon));
			State.Arms.Add(MoveTemp(Off));

			UE_LOG(LogTraceGame, Display,
				TEXT("[TRAILPERF] starting: %d interleaved cycles x 3 arms x (%.1fs warmup + %.1fs sample) "
				     "= %.0fs total."),
				State.CyclesTotal, State.WarmupSeconds, State.SampleSeconds,
				State.CyclesTotal * 3.f * (State.WarmupSeconds + State.SampleSeconds));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickTrailPerf(State, DeltaTime);
				}), 0.f);
		}));

	FAutoConsoleCommand CmdTestHeadGap(
		TEXT("Trace.Trail.TestHeadGap"),
		TEXT("Trace.Trail.TestHeadGap [Runs] - dash an enemy through the NEWEST DRAWN segment of a live "
		     "carrier's trace and report whether it still kills, alongside the measured gap between the "
		     "carrier and the end of their own trace (spec v5 2)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FHeadGapTestState State;
			State.TotalRuns = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 50) : 4;

			UE_LOG(LogTraceGame, Display,
				TEXT("[HEADGAPTEST] starting %d dashes through the NEWEST DRAWN segment of a live carrier's trace."),
				State.TotalRuns);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float /*DeltaTime*/) mutable -> bool
				{
					return TickHeadGapTest(State);
				}));
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

	for (UStaticMeshComponent* Piece : PredictedSmearMeshes)
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

	PredictedSmearMeshes.Reset();
	PredictedSmearMaterials.Reset();
	PredictedSmearBaseGlow.Reset();
	PredictedSmearAppliedGlowScale.Reset();
	PredictedHeadLength = 0.f;

	PoseGhosts.Reset();
	PoseGhostMaterials.Reset();
	PoseGhostBaseGlow.Reset();
	PoseGhostAppliedGlowScale.Reset();
	GhostRecords.Reset();

	RibbonSamples.Reset();
	RibbonSampleBirth.Reset();
	RibbonSourcePoints.Reset();
	RibbonSourceBirths.Reset();

	LastVisualPointCount = -1;
	bColorApplied = false;
}

// =================================================================================================
// MEASUREMENT (spec v6 §1)
// =================================================================================================

void UTraceTrailComponent::MeasureDrawnVolume(double& OutMaxHalfWidth, double& OutMaxHalfHeight,
	double& OutWorstUncovered, int32& OutVisiblePieces) const
{
	OutMaxHalfWidth = 0.0;
	OutMaxHalfHeight = 0.0;
	OutWorstUncovered = 0.0;
	OutVisiblePieces = 0;

	// Only the REPLICATED pool. The owner-only predicted stub is deliberately drawn where nothing is
	// lethal yet, so including it would report a violation that is documented, intended, and invisible
	// to anybody who could be killed by it (see UpdatePredictedHead).
	TArray<const UStaticMeshComponent*, TInlineAllocator<128>> Visible;
	for (const UStaticMeshComponent* Piece : SmearMeshes)
	{
		if (Piece != nullptr && Piece->IsVisible() && Piece->GetStaticMesh() != nullptr)
		{
			Visible.Add(Piece);
			++OutVisiblePieces;
		}
	}

	// DIRECTION ONE: nothing drawn may be WIDER OR TALLER than the volume that kills. A visible
	// ribbon that cannot kill is the "I dashed through it and nothing happened" bug wearing a
	// different hat. The local Y axis is the ribbon's width and Z is its height; X is its length
	// along the path and is not a cross-section.
	for (const UStaticMeshComponent* Piece : Visible)
	{
		const FBox LocalBox = Piece->GetStaticMesh()->GetBoundingBox();
		const FVector LocalExtent = LocalBox.GetExtent();
		const FVector Scale = Piece->GetComponentScale();

		OutMaxHalfWidth = FMath::Max(OutMaxHalfWidth, FMath::Abs(LocalExtent.Y * Scale.Y));
		OutMaxHalfHeight = FMath::Max(OutMaxHalfHeight, FMath::Abs(LocalExtent.Z * Scale.Z));
	}

	// DIRECTION TWO: every LETHAL point must be inside something that is on screen. Measured as the
	// distance from the point to the nearest visible piece's oriented box — clamping in the piece's
	// own local space and transforming back is the exact closest point on an OBB, which an
	// axis-aligned world bounds test would only approximate (and would approximate in the flattering
	// direction).
	const int32 LethalCount = ComputeLastLethalIndex() + 1;
	for (int32 PointIndex = 0; PointIndex < LethalCount && Visible.Num() > 0; ++PointIndex)
	{
		const FVector Point(TrailPoints.Items[PointIndex].Location);

		double Nearest = TNumericLimits<double>::Max();
		for (const UStaticMeshComponent* Piece : Visible)
		{
			const FTransform& PieceTransform = Piece->GetComponentTransform();
			const FBox LocalBox = Piece->GetStaticMesh()->GetBoundingBox();

			const FVector Local = PieceTransform.InverseTransformPosition(Point);
			const FVector Clamped(
				FMath::Clamp(Local.X, LocalBox.Min.X, LocalBox.Max.X),
				FMath::Clamp(Local.Y, LocalBox.Min.Y, LocalBox.Max.Y),
				FMath::Clamp(Local.Z, LocalBox.Min.Z, LocalBox.Max.Z));

			Nearest = FMath::Min(Nearest, FVector::Dist(Point, PieceTransform.TransformPosition(Clamped)));
			if (Nearest <= 0.01)
			{
				break;
			}
		}

		OutWorstUncovered = FMath::Max(OutWorstUncovered, Nearest);
	}
}

void UTraceTrailComponent::CountDrawnPieces(int32& OutStaticVisible, int32& OutSkinnedVisible) const
{
	OutStaticVisible = 0;
	OutSkinnedVisible = 0;

	for (const UStaticMeshComponent* Piece : SmearMeshes)
	{
		if (Piece != nullptr && Piece->IsVisible())
		{
			++OutStaticVisible;
		}
	}
	for (const UStaticMeshComponent* Piece : PredictedSmearMeshes)
	{
		if (Piece != nullptr && Piece->IsVisible())
		{
			++OutStaticVisible;
		}
	}
	for (const UPoseableMeshComponent* Ghost : PoseGhosts)
	{
		if (Ghost != nullptr && Ghost->IsVisible())
		{
			++OutSkinnedVisible;
		}
	}
}

void UTraceTrailComponent::CountPooledPieces(int32& OutStaticTotal, int32& OutSkinnedTotal) const
{
	OutStaticTotal = SmearMeshes.Num() + PredictedSmearMeshes.Num();
	OutSkinnedTotal = PoseGhosts.Num();
}
