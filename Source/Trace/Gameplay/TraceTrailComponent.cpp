#include "Gameplay/TraceTrailComponent.h"

#include "Net/UnrealNetwork.h"

#include "Camera/CameraActor.h"                // spec v12 §6 — the capture camera for Trace.Trail.WallClip
#include "Camera/PlayerCameraManager.h"        // local camera location (proximity glow fade)
#include "CollisionQueryParams.h"              // spec v12 §6 — the wall-fit world queries
#include "CollisionShape.h"                    // FCollisionShape::MakeCapsule / MakeSphere
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"  // the arena pools its rendered geometry
#include "Components/PoseableMeshComponent.h"  // spec v4 §2 — the character-shaped after-images
#include "Components/SkeletalMeshComponent.h"  // the live Mannequin the ghosts copy their pose from
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"                     // GEngine->GetFirstLocalPlayerController()
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"                // FMTDResult (how deep the trace is inside a wall)
#include "Engine/OverlapResult.h"              // FOverlapResult (spec v12 §6)
#include "Engine/SkinnedAsset.h"               // ghost pool identity (which Mannequin is skinned in)
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                       // TActorIterator (fallback character gather)
#include "GameFramework/Character.h"           // capsule radius, for the wall-fit clearance ceiling
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"       // GetServerWorldTimeSeconds()
#include "HAL/PlatformFileManager.h"           // Trace.Trail.WallClip's screenshot check
#include "Misc/Paths.h"                        // Trace.Trail.WallClip's screenshot path
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
#include "Core/TraceGameState.h"        // ScoringMode, for the mode-A/mode-B grace report
#include "Gameplay/TraceCore.h"                // IsTraceInvulnerableFor (spec §4)
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceParry.h"               // v3 §3 — the second invulnerability source
#include "Movement/TraceCharacterMovementComponent.h"   // GetLastDashActiveWorldTime()
#include "Trace.h"
#include "TraceSettings.h"

namespace
{
	/**
	 * The trace lifetime came from spec v3 §1; the turnover grace is now 0.75s (spec v10 §3 raised it
	 * from 0.5 after the mechanism was tested and found to be working — see Trace.Trail.GraceTest).
	 *
	 * BOTH VALUES NOW LIVE IN UTraceSettings (TrailLifetime, CoreTurnoverGraceSeconds). These used to
	 * be ceilings applied over the settings, because TraceSettings.h belonged to another ownership
	 * slice mid-pass and a stale 2.8 could otherwise have reinstated the old rule. The settings are
	 * the single source of truth now, so the min() was an identity and is gone: there is exactly one
	 * number for each, it is in Project Settings, and it is live-editable in PIE.
	 *
	 * THE INI WINS OVER THE HEADER, so neither of those numbers should be quoted from a comment.
	 * Trace.Trail.GraceTest prints the configured value it actually measured against.
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
		     "FORMING (spec v10 3: 0.75). Negative (default) = use UTraceSettings::CoreTurnoverGraceSeconds. "
		     "Delays formation only; already-laid segments stay lethal."),
		ECVF_Default);

	// =============================================================================================
	// SPEC v10 §2 — WHOLE-MODEL TRIP DETECTION. THE KNOBS.
	//
	// The rationale for choosing the mesh bounds over a flat margin or per-bone volumes is in the
	// header, above UTraceTrailComponent::MeasureModelReach. What lives here is the A/B switch and
	// the two clamps that bound the over-reach.
	//
	// THESE ARE CVARS AND NOT UTraceSettings PROPERTIES ON PURPOSE, and it is a scope fact rather
	// than a design opinion: this pass owns TraceTrailComponent.{h,cpp} and nothing else, and a
	// UTraceSettings knob has to be declared in TraceSettings.h AND written into Config/DefaultGame.ini
	// (the ini wins) by whoever owns those. The follow-up is written up in the report. Until then
	// these are live-editable from the console exactly like Trace.Trail.Radius/Height beside them,
	// and the defaults below are the shipping values.
	// =============================================================================================

	/**
	 * THE A/B ARM, AND THE REASON THE FIX IS DEMONSTRABLE RATHER THAN ASSERTED.
	 *
	 * 1 (default): the trip test is sized to the rendered model.
	 * 0:           the exact pre-v10 behaviour — capsule radius plus trail radius, nothing else.
	 *
	 * Trace.Trail.ModelHitTest runs its whole scenario twice, once per arm, on ONE build in ONE
	 * match. Arm 0 is the RED arm and it must fail; a harness that cannot be made to fail is not
	 * evidence. Never ship 0: it is the reported bug.
	 */
	int32 GWholeModelTrip = 1;

	/**
	 * FLOOR on the margin over the capsule radius, in uu.
	 *
	 * Exists because the mesh bounds are not guaranteed to be there. A pawn whose animation is not
	 * ticking (a dedicated server with URO configured to skip it), a pawn whose mesh has not been
	 * imported yet, a pawn using fixed imported bounds tighter than its own silhouette — in every
	 * one of those the measured reach collapses back to roughly the capsule and the reported bug
	 * comes back silently. The floor makes the fix survive all of them.
	 *
	 * 10uu is chosen against the trace's own half-width (22.5uu): it widens the scoring band by
	 * under half a trace-width, which is inside the thickness of the ribbon a player is looking at,
	 * so it cannot read as a hit at distance even in the worst case where the model really was
	 * tucked inside its capsule.
	 */
	float GModelMarginMin = 10.f;

	/**
	 * CEILING on the margin over the capsule radius, in uu. THIS IS THE WORST-CASE OVER-REACH OF THE
	 * WHOLE CHANGE, and it is a hard clamp rather than a hope about the asset.
	 *
	 * 20uu, AND THAT NUMBER CAME OFF A RUNNING GAME rather than out of this file. Trace.Trail.ModelReach
	 * measured the imported Mannequin: 32.7uu of horizontal reach standing (LESS than its own 34uu
	 * capsule) and 41.3uu mid-dash — the bounds do follow the pose, which is the property the whole
	 * choice rests on. So the real silhouette wants ~7uu over the capsule and the floor of 10 already
	 * covers it; 20 exists only as headroom for a more extreme pose than the one that was measured.
	 *
	 * What it costs at the limit: the horizontal threshold moves from 22.5 + 34 = 56.5uu to at most
	 * 22.5 + 54 = 76.5uu (+35%), and in the measured dash pose to 66.5uu (+18%). 20uu is 0.2m and
	 * under half a ribbon width (the trace is 45uu across), so even the ceiling case is a hit inside
	 * the silhouette the player is watching rather than a connection at visible distance.
	 */
	float GModelMarginMax = 20.f;

	/**
	 * VERTICAL margin over the capsule half-height, in uu. DEFAULT 0, AND THE ZERO IS THE FINDING.
	 *
	 * The first version of this widened vertically by the same rule, and measuring it is what killed
	 * that idea: the Mannequin's mesh bounds report 141uu of vertical half-extent about the actor
	 * origin against an 88uu capsule half-height. That 141 is plainly not the silhouette — a
	 * Mannequin's feet sit on the bottom of its capsule and its head under the top BY CONSTRUCTION,
	 * so there is no "limb outside the capsule" case vertically at all, and the extra is the mesh
	 * component's bounds being generous rather than a body part being out there.
	 *
	 * Taking it would have bought nothing but air. The vertical test is already |dZ| <= 31.5 + 88 =
	 * 119.5uu, which every grounded or jumping player passes trivially; pushing it to 145.5 would
	 * have scored on a dasher whose FEET were a clear 26uu above the top of the ribbon. That is the
	 * "phantom connection at visible distance" the spec forbids, so the vertical stays at the capsule
	 * and the knob is here for anyone who later has a pawn the claim is not true of.
	 *
	 * The missing hits were never vertical. They were the 22.5uu-wide ribbon against a 34uu capsule.
	 */
	float GModelMarginVertical = 0.f;

	/**
	 * Optional FIXED margin, in uu. Negative (default) = measure the mesh. >= 0 = use this number
	 * for every pawn and skip the measurement entirely.
	 *
	 * Kept because it is the spec's second option and because it is the one thing that makes the
	 * choice falsifiable at playtest: if the bounds-driven version ever feels wrong, this pins it to
	 * a constant without a rebuild, and the two can be compared in the same session.
	 */
	float GModelMarginFixed = -1.f;

#if !UE_BUILD_SHIPPING
	FAutoConsoleVariableRef CVarWholeModelTrip(
		TEXT("Trace.Trail.WholeModelTrip"),
		GWholeModelTrip,
		TEXT("1 (default, spec v10 2): the dash trip test is sized to the rendered MODEL, so any part "
		     "of it touching the trace scores — for the kill and for the parry alike. 0 restores the "
		     "pre-v10 capsule-only test for A/B measurement ONLY; that is the reported bug."),
		ECVF_Cheat);
#endif

	FAutoConsoleVariableRef CVarModelMarginMin(
		TEXT("Trace.Trail.ModelMarginMin"),
		GModelMarginMin,
		TEXT("Spec v10 2. FLOOR, in uu, on how far past the capsule radius the trip test reaches, used "
		     "when the mesh bounds are unavailable or tighter than the capsule. Default 10."),
		ECVF_Default);

	FAutoConsoleVariableRef CVarModelMarginMax(
		TEXT("Trace.Trail.ModelMarginMax"),
		GModelMarginMax,
		TEXT("Spec v10 2. CEILING, in uu, on how far past the capsule radius the trip test reaches. This "
		     "is the WORST-CASE OVER-REACH of whole-model detection and it is a hard clamp. Default 26."),
		ECVF_Default);

	FAutoConsoleVariableRef CVarModelMarginVertical(
		TEXT("Trace.Trail.ModelMarginVertical"),
		GModelMarginVertical,
		TEXT("Spec v10 2. Margin in uu over the capsule HALF-HEIGHT. Default 0: a Mannequin is inside its "
		     "capsule vertically by construction, and the mesh bounds' 141uu vertical half-extent is not a "
		     "silhouette. Raising this scores on air below a dasher's feet."),
		ECVF_Default);

	FAutoConsoleVariableRef CVarModelMarginFixed(
		TEXT("Trace.Trail.ModelMargin"),
		GModelMarginFixed,
		TEXT("Spec v10 2. Negative (default) = derive the margin from the skeletal mesh's own bounds, "
		     "clamped to [ModelMarginMin, ModelMarginMax]. >= 0 = use this fixed margin in uu for every "
		     "pawn instead of measuring."),
		ECVF_Default);

	/**
	 * THE KNOCK-ON, COUNTED WHERE IT HAPPENS.
	 *
	 * GModelTripsTotal    every lethal trip the test has scored this session.
	 * GModelTripsWidened  the subset the pre-v10 capsule-only test would have MISSED.
	 *
	 * The second number is the entire measurable cost of this change. The spec asks for it by name
	 * ("bots on Hard already get 82% of their kills from trace dashes ... Measure it and report; do
	 * not silently retune the bots"), and inferring it from a log afterwards would mean inferring it
	 * — so the trip test evaluates the old threshold alongside the new one on the rare frames it
	 * scores, and writes the answer down.
	 */
	int32 GModelTripsTotal = 0;
	int32 GModelTripsWidened = 0;

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

	// =============================================================================================
	// SPEC v12 §6 — the trace clips into walls
	// =============================================================================================
	//
	// "The trace is clipping into walls sometimes, when a model runs close to a corner/structure."
	//
	// MEASURED CAUSE (see Trace.Trail.WallClip, which reports both halves separately): it is the PATH,
	// not the drawing. Points are laid every TrailPointSpacing of travel and everything downstream —
	// the ribbon AND the trip test — treats the polyline between them as straight. A carrier rounding
	// a corner traces an arc about the corner vertex at their own capsule radius, and one 60uu step is
	// most of a quarter turn, so consecutive points straddle the corner and the chord between them
	// passes inside the structure. Both the ribbon and the kill volume take the identical shortcut.
	//
	// THE FIX, and why it is this one of the three the spec offered:
	//
	//   * Tapering the ribbon near walls, or clipping the drawn ribbon against the world, would both
	//     mean the DRAWING knows something the TRIP TEST does not. The trip test would then need its
	//     own copy of the same world query, per segment, every server tick — two implementations of
	//     one piece of geometry, kept in agreement by hand. That is precisely the arrangement that has
	//     already produced "visible but not lethal" on this project once.
	//   * Moving the PATH needs neither. RebuildRibbon and ServerRunTripTest both read TrailPoints and
	//     neither is modified by this change, so the invariant survives for the reason it always has:
	//     THERE IS ONLY ONE POLYLINE. Whatever this does to the trace, it does to both halves of it in
	//     the same instruction.
	//
	// And the path can be fixed honestly, without inventing geometry: the carrier's capsule is 34uu of
	// radius against a 22.5uu trace, so anywhere a body legally stood there is room for the ribbon.
	// The route is fine; only the chord across it is wrong. So the repair is to SUBDIVIDE the chord
	// with positions the carrier really occupied (PendingPathSamples) until no segment passes through
	// the level — plus a small horizontal nudge for a point that is somehow already inside something.
	//
	// Everything downstream is unchanged by construction. Length-based expiry still measures the
	// polyline, and measures the routed one, which is longer than the chord and therefore MORE honest
	// about how far the carrier ran. No timer is introduced. ClearTrail still empties one array.
	//
	// ---- HOW THESE FOUR RESOLVE (integration pass, v12) ----
	// All four ship as UTraceSettings properties with ini lines in Config/DefaultGame.ini, and the
	// settings are the SOURCE OF TRUTH. These CVars are console OVERRIDES layered on top, exactly the
	// arrangement GetTraceTrailRadius()/GetTraceTrailHeight() already use for the trace's dimensions.
	//
	// The sentinel is NEGATIVE = "nobody typed a value, follow the settings". It has to be a sentinel
	// rather than a matching default because 0 is a MEANINGFUL value for three of the four (WallFit 0
	// is the A/B before-arm, MaxPush 0 disables the nudge, MaxInsert 0 disables subdivision) — so
	// "equals the default" cannot be used to detect "unset" the way it could for a positive-only knob.
	//
	// Read them through WallFitEnabled()/WallFitMargin()/WallFitMaxPush()/WallFitMaxInsert() below and
	// NEVER off the globals directly, or the ini silently stops working again.
	int32 GWallFit = -1;

	FAutoConsoleVariableRef CVarWallFit(
		TEXT("Trace.Trail.WallFit"),
		GWallFit,
		TEXT("Console override for bTrailWallFitEnabled; -1 (default) follows the setting. "
		     "1 = the server subdivides a trail segment that would pass through the level, "
		     "using the positions the carrier really occupied, so neither the ribbon nor the kill "
		     "volume cuts across a corner. 0 = the pre-v12 straight chord, i.e. the reported bug, "
		     "reproduced on demand for A/B. Moves the drawn and the lethal trace together — there is "
		     "only one polyline."),
		ECVF_Default);

	/**
	 * Clearance the trace asks for, OVER its own half width, when deciding whether a segment is inside
	 * the level. The ribbon is 22.5uu of half width but the DRAWING legitimately reaches a little
	 * further than that at joints (PlaceRibbon overlaps interior joints by one TrailRadius so the
	 * wedge on the outside of a corner closes), and a box's corner is further from the axis than its
	 * face. A few uu of margin covers that without pretending the trace is fatter than it is.
	 *
	 * Bounded well under the carrier's own 34uu capsule radius: ask for more clearance than a body
	 * needs and there are legal routes that no polyline can satisfy, and the fitter would spend its
	 * insert budget every step for nothing.
	 */
	float GWallFitMargin = -1.f;

	FAutoConsoleVariableRef CVarWallFitMargin(
		TEXT("Trace.Trail.WallFitMargin"),
		GWallFitMargin,
		TEXT("Console override for TrailWallFitMarginUU; negative (default) follows the setting. "
		     "uu of clearance the trace asks for OVER its own half width when testing a segment against "
		     "the level (spec v12 6). Covers the ribbon's joint overlap and box corners. Clamped to the "
		     "carrier's capsule radius - past that no legal route would ever be clear."),
		ECVF_Default);

	/**
	 * Cap on the horizontal nudge applied to a point that is already inside something. Small on
	 * purpose: this must never become a mechanism that slides the trace off the route the player ran.
	 * Subdivision does the real work; this only cleans up the residue.
	 */
	float GWallFitMaxPush = -1.f;

	FAutoConsoleVariableRef CVarWallFitMaxPush(
		TEXT("Trace.Trail.WallFitMaxPush"),
		GWallFitMaxPush,
		TEXT("Console override for TrailWallFitMaxPushUU; negative (default) follows the setting. "
		     "Largest horizontal nudge, in uu, applied to a trail point that is already inside level "
		     "geometry (spec v12 6). 0 disables the nudge and leaves only the subdivision. Never "
		     "applied unless it reduces the penetration and the moved point is still in line of sight "
		     "of where it started, so a thin wall cannot be tunnelled through."),
		ECVF_Default);

	/**
	 * Ceiling on points inserted for ONE append. A 60uu chord subdivided by the carrier's real path
	 * needs one or two extra points around a pillar; the budget exists so that a pathological case
	 * (a carrier standing inside geometry, a map with a mesh no capsule can clear) degrades to the
	 * old behaviour instead of flooding the fast array.
	 */
	int32 GWallFitMaxInsert = -1;

	FAutoConsoleVariableRef CVarWallFitMaxInsert(
		TEXT("Trace.Trail.WallFitMaxInsert"),
		GWallFitMaxInsert,
		TEXT("Console override for TrailWallFitMaxInsert; negative (default) follows the setting. "
		     "Most extra points the corner fitter may insert for one appended trail point (spec v12 6). "
		     "Exhausting it falls back to the straight chord and logs."),
		ECVF_Default);

	/**
	 * v13 §7: does the fitter ask the RENDERED level, or only the physics scene?
	 *
	 * 1 (default) is the fix. 0 is the BEFORE arm for this pass specifically — it leaves everything
	 * else about the fitter alone and only takes away its knowledge of what is on screen, which is
	 * precisely the deficiency that let ~26uu of ribbon sit inside a corner while every collision
	 * query in the file reported acres of clearance. Trace.Trail.WallClip prints which arm it ran.
	 */
	/**
	 * v13 §7: does the fitter clear EVERY point by the trace's half width, or only route blocked chords?
	 *
	 * THIS IS THE SWITCH UTraceSettings::bTrailWallClearanceEnabled DESCRIBES, and until now it was
	 * the switch nothing read. The setting shipped `EditAnywhere`, with an ini key behind it in
	 * Config/DefaultGame.ini and a row in Trace.VerifyKnobs saying OK — and no reader anywhere in the
	 * project. Its whole documented purpose is the middle arm of a three-way A/B ("OFF is the v12
	 * behaviour EXACTLY — blocked chords only"), and that arm did not exist: OFF and ON were the same
	 * build. VerifyKnobs cannot catch this by construction; it proves a property is reachable, not
	 * that anybody reached for it. So the binding is here, in the resolver, with the same negative
	 * sentinel the other four wall-fit knobs use.
	 *
	 * Trace.Trail.WallClearance is the CVar name the settings header already documents this pairing
	 * with, so the header did not have to move — it was correct about the intent and wrong only about
	 * whether it had been wired.
	 */
	int32 GWallClearance = -1;

	FAutoConsoleVariableRef CVarWallClearance(
		TEXT("Trace.Trail.WallClearance"),
		GWallClearance,
		TEXT("Console override for bTrailWallClearanceEnabled; -1 (default) follows the setting. "
		     "1 = every trail point is cleared by the trace's own drawn half reach plus margin, so a "
		     "segment that merely RUNS ALONGSIDE a wall is corrected. 0 = the v12 behaviour exactly: "
		     "the fitter only routes chords that are BLOCKED, which reproduces the reported clipping "
		     "on demand because a carrier running parallel to a wall never blocks a chord (spec v13 7). "
		     "Moves the drawn and the lethal trace together - there is only one polyline."),
		ECVF_Default);

	int32 GWallFitVisual = 1;

	FAutoConsoleVariableRef CVarWallFitVisual(
		TEXT("Trace.Trail.WallFitVisual"),
		GWallFitVisual,
		TEXT("1 (default) = the wall fitter clears the trace out of the arena's RENDERED geometry as "
		     "well as its collision. 0 = collision only, the pre-v13 behaviour: the arena's visible "
		     "structure is NoCollision meshes over smaller invisible boxes, with emissive trim "
		     "protruding 10-13uu past even those, so a collision-only fitter is blind to exactly the "
		     "surfaces the ribbon is reported clipping into (spec v13 7)."),
		ECVF_Default);

	/**
	 * The resolvers. CVar when somebody typed one, the shipped setting otherwise. Every read of
	 * the wall fitter's tuning goes through these — see the sentinel note on GWallFit above for why
	 * "unset" has to be negative rather than "equal to the default".
	 */
	bool WallFitEnabled()
	{
		return (GWallFit >= 0) ? (GWallFit != 0) : UTraceSettings::Get().bTrailWallFitEnabled;
	}

	bool WallFitUsesRenderedGeometry()
	{
		return GWallFitVisual != 0;
	}

	/** v13 §7's half-width clearance arm. Setting by default, CVar when one was typed. */
	bool WallClearanceEnabled()
	{
		return (GWallClearance >= 0) ? (GWallClearance != 0) : UTraceSettings::Get().bTrailWallClearanceEnabled;
	}

	float WallFitMargin()
	{
		return (GWallFitMargin >= 0.f) ? GWallFitMargin : UTraceSettings::Get().TrailWallFitMarginUU;
	}

	/**
	 * v13 §7: THE PUSH ALLOWANCE, AND WHY THIS FUNCTION NO LONGER JUST RETURNS THE SETTING.
	 *
	 * TrailWallFitMaxPushUU ships at 12uu. The penetration it is supposed to resolve was MEASURED at
	 * ~26uu, so the shipped number could not clear the bug even in principle — it is not a tuning
	 * choice that turned out low, it is a number that was picked before anyone knew what the trace's
	 * drawn geometry actually reached (GetTraceDrawnHalfReach: ~36uu, not the 22.5 everyone assumed).
	 *
	 * The floor below is therefore derived rather than typed: a push may go as far as the clearance
	 * the trace is asking for, because a point that needs more than that is already outside anything
	 * the fitter could have been clearing it from. It is a FLOOR, not a replacement — a settings or
	 * ini value LARGER than the derived number still wins, so raising it in Config keeps working.
	 *
	 * AND IT IS NOT SILENT. A knob that quietly stops meaning what it says is the exact failure mode
	 * this project has a rule about, so the first time the floor bites it says so, names the setting
	 * and prints both numbers. Config/DefaultGame.ini should be raised to match; this file does not
	 * own that file.
	 *
	 * An EXPLICIT console value is honoured exactly, floor and all, because Trace.Trail.WallFitMaxPush
	 * 0 is a documented A/B arm ("disable the nudge") and a harness arm that silently refuses to be
	 * the arm it was asked for is worse than no arm.
	 */
	/**
	 * The clearance one trail point asks for: everything the trace draws or kills with, plus margin.
	 *
	 * Bounded at 4x the lethal half width purely as a sanity rail against a pathological
	 * Trace.Trail.Radius — not as a tuning knob. Nothing about the arena needs the rail.
	 */
	double WallFitRequiredClearance()
	{
		const double Reach = UTraceTrailComponent::GetTraceDrawnHalfReach();
		const double Margin = FMath::Max(0.0, static_cast<double>(WallFitMargin()));
		const double Radius = FMath::Max(1.0, static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius()));

		return FMath::Min(Reach + Margin, Radius * 4.0);
	}

	float WallFitMaxPush()
	{
		if (GWallFitMaxPush >= 0.f)
		{
			return GWallFitMaxPush;
		}

		const float Configured = UTraceSettings::Get().TrailWallFitMaxPushUU;
		const float Needed = static_cast<float>(WallFitRequiredClearance());
		if (Configured >= Needed)
		{
			return Configured;
		}

		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("Trace: TrailWallFitMaxPushUU is %.1fuu, below the %.1fuu the trace's own drawn "
				     "half-reach (%.1fuu) plus margin (%.1fuu) needs to clear itself of the level. Using "
				     "%.1fuu. Raise TrailWallFitMaxPushUU in Config/DefaultGame.ini and TraceSettings.h "
				     "to match, or this floor stays the thing that is actually deciding (spec v13 7)."),
				Configured, Needed, UTraceTrailComponent::GetTraceDrawnHalfReach(), WallFitMargin(),
				Needed);
		}

		return Needed;
	}

	int32 WallFitMaxInsert()
	{
		return (GWallFitMaxInsert >= 0) ? GWallFitMaxInsert : UTraceSettings::Get().TrailWallFitMaxInsert;
	}

	/** Session counters, printed by Trace.Trail.WallClip. Appends that needed routing / points added. */
	int32 GWallFitRoutedAppends = 0;
	int32 GWallFitInsertedPoints = 0;

	/** Appends the fitter could not make clear at all — the honest count of what this does NOT fix. */
	int32 GWallFitUnroutable = 0;

	/**
	 * v13 §7: the PUSH's own counters, separate from the router's, because the two fix different
	 * things and the previous pass's report could not tell them apart — it printed "routed twice in
	 * 1312 frames" and left the reader to assume that meant the fitter had barely run, when in fact
	 * the push was running on every single append and achieving 1.9uu.
	 *
	 * WorstResidual is the number that matters most: how deep the trace still was after the best push
	 * the fitter could find. It is the fix's own admission of what it did not manage.
	 */
	int32 GWallFitPushes = 0;
	int32 GWallFitUnpushable = 0;
	double GWallFitWorstPush = 0.0;
	double GWallFitWorstResidual = 0.0;

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

	/**
	 * Elements in the owner-only predicted-head ribbon. The stub is at most GPredictedHeadMaxLength.
	 *
	 * v8 §2 took it from 12 to 24 with the cap's rise from 400 to 1200: BuildRibbonSamples COARSENS
	 * rather than truncating, so 12 would still have covered the whole stub — as 100uu elements
	 * chording a dash's curve. 24 keeps the stub's element length at the ribbon's own 60uu step, which
	 * matters because the join between the stub and the replicated ribbon has to be invisible and a
	 * chunkier element on one side of it is exactly how it becomes visible. Twelve extra boxes, on one
	 * pawn per machine — the carrier's own.
	 */
	constexpr int32 MaxPredictedRibbonElements = 24;

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
	 * Offsetting every other element's cross-section by 0.6% makes the overlapping faces non-coplanar,
	 * so the depth test resolves them consistently and the ribbon reads as one surface. It is far
	 * below the width of a pixel at any range the trace is judged from.
	 *
	 * v14 §1 FLIPPED ITS SIGN, AND THE SIGN IS THE WHOLE POINT. It used to be 0.994 — every other
	 * element drawn very slightly SMALLER than the lethal volume — with the argument that 0.14uu of
	 * under-draw is lost inside the tripper's own capsule. That argument is fine for feel and wrong
	 * for the invariant: "lethal outside drawn must be ZERO" cannot be asserted at zero while the
	 * drawing is deliberately a tenth of a uu narrow, and a harness forced to carry a tolerance to
	 * accommodate a cosmetic constant is a harness that will one day absorb a real defect in the same
	 * tolerance. 1.006 breaks the coplanarity exactly as well and spends its 0.14uu in the
	 * over-drawing direction, which the standing budget already covers many times over.
	 */
	constexpr double RibbonAlternateOutset = 1.006;

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
	 * SPEC v8 §2 RAISED IT FROM 400 TO 1200, AND THE REASON IS THE WHOLE OF THIS PASS.
	 *
	 * 400 was sized for the HOST, where the only thing to cover is the head-grace stub (~80uu) — and
	 * on the host it never came close to firing. On a JOINED CLIENT the same gap is the sum of three
	 * terms, not one: the head grace, the round trip of trace the server has laid and the wire has not
	 * yet delivered, AND the client's own movement prediction, which puts the pawn a further round
	 * trip AHEAD of the position those points were laid at. Measured on a client at 40ms (see
	 * Trace.Trail.OwnerHeadAB), a dash at 3000uu/s puts that sum at 400-600uu — i.e. straight through
	 * the cap, on every dash, on a client only. And the cap does not shorten the stub, it DELETES it:
	 * the carrier's trace detached from their body for the whole dash and snapped back afterwards.
	 * That is "trace still has the same bug for the people connecting to the server".
	 *
	 * The cap could only be raised because the stub stopped being a straight line — see
	 * GPredictedHeadUseHistory below. Every uu of it is now ground the pawn is recorded as having
	 * actually covered, so length alone no longer implies fabrication, and the thing that DOES imply
	 * fabrication (a single step no player could have walked) is tested per segment instead, against
	 * MaxTrailSegmentLength — the same constant the server restarts its own trace on. 1200 is the
	 * trace's own maximum length: past that the stub would be longer than the trace it continues.
	 */
	float GPredictedHeadMaxLength = 1200.f;

	FAutoConsoleVariableRef CVarPredictedHeadMaxLength(
		TEXT("Trace.Trail.PredictMaxLength"),
		GPredictedHeadMaxLength,
		TEXT("uu the owner-only predicted head stub may span (spec v5 2, raised 400 -> 1200 by v8 2). 0 "
		     "disables the prediction and restores the gap between a carrier and the end of their own "
		     "trace. Beyond this length the stub is dropped entirely rather than clamped - see the "
		     "comment. Set it to 400 to reproduce the pre-v8 client behaviour."),
		ECVF_Default);

	/**
	 * SPEC v8 §2 — THE FIX ITSELF, and the A/B lever for it. 1 = on, the default.
	 *
	 * With this off, the stub is what v5 §2 shipped: a straight chord from the newest drawn point to
	 * wherever the pawn is now. That is exact on the host (the two are 80uu apart) and wrong on a
	 * client in two different ways at once — it is too LONG to be trusted (so the cap deletes it) and,
	 * where it does draw, it is a chord across whatever corner the carrier turned during the round
	 * trip, so the trace visibly cuts the corner it is supposed to be following.
	 *
	 * With it on, the stub is built THROUGH LocalPathHistory: the carrier's own recorded per-frame
	 * positions, on their own machine, from the newest replicated point up to the pawn. It is not a
	 * prediction of the past at all — it is a record of it. The one thing it cannot know is what the
	 * SERVER did with those frames, which is precisely why the stub stays owner-only, non-lethal and
	 * stateless: the moment the authoritative point for that ground arrives, the sample it duplicates
	 * is dropped and the ribbon behind the carrier is the replicated one again.
	 */
	int32 GPredictedHeadUseHistory = 1;

	FAutoConsoleVariableRef CVarPredictedHeadUseHistory(
		TEXT("Trace.Trail.PredictHistory"),
		GPredictedHeadUseHistory,
		TEXT("1 (default) builds the owner-only predicted head through the carrier's own recorded path, "
		     "so it follows the corners they turned and can honestly span a client's round trip (spec "
		     "v8 2). 0 restores the v5 straight chord, which is the pre-v8 client behaviour."),
		ECVF_Default);

	/**
	 * Samples kept in LocalPathHistory. At 120fps this is 1.0s of travel, which is an order of
	 * magnitude more than the round trip it has to cover; the per-frame trim by birth time is what
	 * actually bounds it, and this is only the ceiling that keeps a stalled client's array finite.
	 */
	constexpr int32 MaxLocalPathSamples = 120;

	/** How much of the carrier's own path is kept, in seconds. See RecordLocalPathSample(). */
	constexpr float LocalPathHistorySeconds = 1.0f;

	/**
	 * Minimum spacing between two recorded samples, in uu. Anything finer is invisible in a ribbon
	 * whose resample step is GRibbonStep (60uu) and would only cost curve control points. 25uu is
	 * comfortably finer than the 60uu the server itself lays points at, so the stub is never a coarser
	 * description of the path than the trace it continues.
	 */
	constexpr double LocalPathSampleSpacing = 25.0;

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

	// =============================================================================================
	// SPEC v9 §§3-4 — THE TRACE DIES WITH POSSESSION. THE A/B ARM, AND THE STALE-KILL COUNTER.
	//
	// THE BUG, in one sentence: ATraceCore::ReleaseHolder() is the single funnel every possession
	// end goes through — completed pass, mode-B throw, carrier killed, carrier disconnected, goal,
	// half time, kickoff, match end — and all it did was SetEmitting(false), on the stated grounds
	// that "an expiring trace is counterplay the enemy team has already earned" and it would fade
	// out on its own. THAT FADE NO LONGER EXISTS. Demo 7 (v7 §1) deleted time-based expiry outright
	// to kill the stand-still exploit, so a point is now retired ONLY by new trace arriving at the
	// head. A component that has stopped emitting lays no new trace, so nothing ever retires
	// anything, so the trace is IMMORTAL.
	//
	// And it is immortal AND LETHAL, because the trip test's gate is the POINTS, not bEmitting
	// (ServerRunTripTest), and ComputeLastLethalIndex() answers "the whole array" the moment
	// emission stops. Which produces both reported symptoms from one cause:
	//
	//   §3  "when a player passes/throws the core ... their trace [takes a second] to disappear and
	//        they can still be killed"  — they can be killed by it FOREVER, not for a second.
	//   §4  "traces stay on the map way after the carrier is dead or has made a pass"
	//
	// The orphan sweep in TickComponent could not catch it either: it was gated on the OWNER being
	// dead or gone, and a player who passes to a living teammate is neither.
	//
	// THE FIX IS OWNERSHIP, NOT A TIMER. Possession ending clears the trace, and points may only
	// exist while this component is actively emitting for a live owner. Nothing here reintroduces a
	// lifetime; a stationary carrier who still holds the Core still keeps every point they laid, so
	// the stand-still exploit stays dead.
	// =============================================================================================

	/**
	 * 1 (default): the trace is cleared the instant possession leaves. 0: the pre-v9 behaviour —
	 * stop emitting and leave the points standing.
	 *
	 * IT EXISTS TO BE MEASURED AGAINST, and for no other reason. Same justification as the legacy
	 * renderer arm behind Trace.Trail.Renderer 0: this pass has to be able to show the reported
	 * symptom happening and then show it not happening, on ONE build, in the SAME match, or the
	 * "fix" is an assertion. Trace.Trail.ClearWatch and Trace.Trail.ClearAudit are the two probes.
	 *
	 * Never ship 0. It is a live unfair-death bug and it is not compiled into Shipping at all.
	 */
	int32 GClearTraceOnPossessionLoss = 1;

#if !UE_BUILD_SHIPPING
	FAutoConsoleVariableRef CVarClearTraceOnPossessionLoss(
		TEXT("Trace.Trail.ClearOnPossessionLoss"),
		GClearTraceOnPossessionLoss,
		TEXT("1 (default, spec v9 3-4): a trace is wiped — visually and lethally — the instant the Core "
		     "leaves its owner, by any route. 0 restores the pre-v9 orphan behaviour for A/B measurement "
		     "ONLY; it is an unfair-death bug. Pair with Trace.Trail.ClearWatch / Trace.Trail.ClearAudit."),
		ECVF_Cheat);
#endif

	/**
	 * THE UNFAIR DEATH, COUNTED WHERE IT HAPPENS (spec v9 §3).
	 *
	 * A trace kill whose victim was NOT holding the Core at the moment they died: they passed it
	 * away, and their own abandoned trace killed them anyway. That is the exact death the user
	 * describes, so it is counted in ApplyTrailTrip rather than inferred from the log afterwards —
	 * an unattended bot match then produces a number for it, and the number must be zero.
	 */
	int32 GTrailKillsTotal = 0;
	int32 GTrailKillsOnNonCarrier = 0;

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

	/**
	 * SPEC v12 §6: ceiling on the carrier's remembered route between two appended points.
	 *
	 * It is emptied every time a point is laid, i.e. every TrailPointSpacing (60uu) of travel, so in
	 * normal play it holds two entries at dash speed and six or seven at a walk. The cap only matters
	 * while a carrier is barely moving, when the distance gate holds the append off indefinitely and
	 * the samples would otherwise accumulate without bound — and the oldest of those are precisely the
	 * ones the fitter has no use for.
	 */
	constexpr int32 MaxPendingPathSamples = 48;

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
	// SPEC v14 §1 — "ENSURE THAT THE TRACE SHOWS EXACTLY WHAT IS LETHAL"
	//
	// Two numbers were measured last pass and both were wrong in a way nobody could see from the
	// code, because "lethal" and "drawn" were each computed correctly from the same polyline and
	// still described different solids:
	//
	//   lethal outside drawn  22.3uu at the END CAPS — an INVISIBLE KILL VOLUME, the worse direction.
	//   drawn outside lethal  29.1uu — visible ribbon that kills nothing, against a 13.8uu budget.
	//
	// THE TWO CAUSES, DIAGNOSED. Neither is about where the points are; both are about what shape
	// each half puts around them.
	//
	//  1. THE END CAPS. ServerRunTripTest asks "how far is the dasher's swept segment from the trail
	//     SEGMENT", and a point-to-segment distance is RADIAL at the ends: the volume that kills
	//     bulges a half-disc of TrailRadius (22.5uu) past the first and last point, in every
	//     horizontal direction. PlaceRibbon, correctly and deliberately, keeps the two outer elements
	//     FLUSH with those points "so the ribbon never extends past the polyline the server kills
	//     along". Both decisions read as right on their own; together they are 22.5uu of kill volume
	//     with nothing drawn on it. That is the 22.3uu, and it is the number the spec put first.
	//
	//     THE FIX IS TO FLATTEN THE VOLUME, NOT TO EXTEND THE DRAWING, and the direction matters.
	//     Extending the ribbon 22.5uu past the head would draw ribbon over the head-grace stub — the
	//     one stretch that is deliberately NOT lethal — and swap an invisible kill volume for a
	//     visible lie. Flattening the caps instead makes the exemption exactly the one body width its
	//     own comment claims, and makes the tail stop killing exactly where it stops being drawn.
	//     The lethal set only ever SHRINKS, so nothing that was avoidable becomes lethal.
	//
	//  2. THE CROSS-SECTION'S TILT — see GRibbonUpright. The lethal volume is a VERTICAL column;
	//     PlaceRibbon pitched each element to follow the path through the air, so on any climb or
	//     drop the drawn slab leans out of that column sideways. That is the 29.1uu.
	//
	// EVERYTHING that asks either question now goes through SegmentGapToTraceVolume below. There is
	// one definition of the solid, the trip test and both halves of the measurement share it, and a
	// future change to one cannot silently fail to reach the other.
	// =============================================================================================

	/**
	 * FLAT OUTER END CAPS ON THE LETHAL VOLUME (spec v14 §1). 1 = the volume stops dead at the first
	 * and last lethal point, exactly where the ribbon stops being drawn. 0 = the pre-v14 radial cap,
	 * which is the reported bug and the RED arm of Trace.Trail.LethalDrawn.
	 *
	 * INTERIOR joints keep their round cap and must: PlaceRibbon runs each element one TrailRadius
	 * PAST every interior joint along its own axis, and that overlap box provably contains the disc
	 * of radius TrailRadius about the joint (any point of the disc is within TrailRadius laterally
	 * AND within TrailRadius along the next element's axis, which is exactly its back overlap). So
	 * the interior is already covered in the safe direction and only the two OUTER caps were bare.
	 */
	int32 GTrailFlatEndCaps = 1;

	FAutoConsoleVariableRef CVarTrailFlatEndCaps(
		TEXT("Trace.Trail.FlatEndCaps"),
		GTrailFlatEndCaps,
		TEXT("Spec v14 1. 1 (default): the lethal volume is the union of FLAT-ENDED slabs, one per "
		     "trail segment - the same chain of boxes the ribbon draws. 0: the pre-v14 radial "
		     "distance-to-segment, whose cap is a disc at every joint AND at both outer ends, so it "
		     "kills up to one TrailRadius past the head and the tail with nothing drawn there. 0 is "
		     "the reported bug and the red arm of Trace.Trail.LethalDrawn."),
		ECVF_Default);

	/**
	 * THE RIBBON'S CROSS-SECTION STANDS UPRIGHT (spec v14 §1). 1 = yaw only; 0 = the pre-v14
	 * yaw+pitch, kept as the other red arm.
	 *
	 * WHY THIS IS THE 29.1uu. The volume that kills is defined in ServerRunTripTest as a HORIZONTAL
	 * distance to the polyline plus a VERTICAL band about it — a vertical column, whatever the path
	 * does. PlaceRibbon oriented each element with `Direction.Rotation()`, i.e. yaw AND pitch, so on
	 * a climb the element's height axis leans away from world up. An element spanning a 60uu step is
	 * (63 + 60) = 123uu tall about an axis pitched ~45 degrees, which puts its corner
	 * 123/2 * sin(45) = 43uu HORIZONTALLY from the path where the column reaches 22.5. That is
	 * visible ribbon, in mid-air, that kills nothing — and it also broke GetTraceDrawnHalfReach(),
	 * which the wall fitter clears room by, so the fitter was under-clearing on every slope.
	 *
	 * THE COST, STATED PLAINLY. A trace laid over a jump is now a series of upright slabs whose
	 * heights union across each step instead of one banked ribbon. GRibbonVerticalStep exists to
	 * keep those steps small enough that it reads as a ramp rather than a staircase. This is a real
	 * change to how a jumping trace LOOKS, taken deliberately: the spec's line is "ensure that the
	 * trace shows exactly what is lethal", and what is lethal is a vertical column.
	 */
	int32 GRibbonUpright = 1;

	FAutoConsoleVariableRef CVarRibbonUpright(
		TEXT("Trace.Trail.RibbonUpright"),
		GRibbonUpright,
		TEXT("Spec v14 1. 1 (default): ribbon elements are yaw-only, so the drawn cross-section is the "
		     "vertical column the trip test actually uses. 0: the pre-v14 yaw+pitch element, which "
		     "leans out of the lethal column on every slope - the red arm of Trace.Trail.LethalDrawn."),
		ECVF_Default);

	/**
	 * Largest vertical rise, in uu, a single upright ribbon element may span (spec v14 §1).
	 *
	 * An upright element covers the UNION of its two ends' vertical bands, so at the LOW end of a
	 * climbing element its top stands a full |dZ| above the lethal band there (and |dZ|/2 in the
	 * middle). That is over-drawing, i.e. the safe direction, but it is still the trace showing
	 * something that does not kill, so it is bounded rather than accepted: BuildRibbonSamples
	 * subdivides a segment until no piece climbs more than this.
	 *
	 * 8, AND THE NUMBER WAS MEASURED, NOT CHOSEN. At 16 the 45 CLIMB fixture reported 15.0uu against
	 * the 13.8uu budget — the one fixture still failing after everything else was fixed, and failing
	 * by the exact |dZ| the subdivision was allowing. At 8 it reports well inside. The cost is
	 * elements on slopes only: a 45-degree climb of 42uu per point becomes six per segment where a
	 * flat run is still one.
	 *
	 * COARSEN, NEVER TRUNCATE still holds — the element budget grows this step exactly as it grows
	 * the horizontal one, so a long fall gets chunkier elements and never a missing stretch.
	 */
	float GRibbonVerticalStep = 8.f;

	FAutoConsoleVariableRef CVarRibbonVerticalStep(
		TEXT("Trace.Trail.RibbonVerticalStep"),
		GRibbonVerticalStep,
		TEXT("Spec v14 1. Largest vertical rise one upright ribbon element may span, in uu. Caps the "
		     "vertical over-draw at half this. Lower = smoother slopes and more elements. Purely "
		     "cosmetic - the kill volume is unchanged."),
		ECVF_Default);

	/**
	 * THE ONE DEFINITION OF THE TRACE'S OWN SOLID, in the two axes the trip test has always kept
	 * apart, as a GAP rather than as a yes/no.
	 *
	 * Returns how far the swept segment [SweepFrom, SweepTo] is from the volume belonging to trail
	 * segment [A, B]: @p OutHorizontal uu horizontally, @p OutVertical uu vertically, both zero when
	 * the sweep is inside. A tripper is caught when the gaps are within ITS OWN reach — which is why
	 * the trace's radius and the tripper's inflation are separate arguments now instead of being
	 * pre-summed into one threshold. Pre-summing is what made a flat cap unexpressible: "distance to
	 * the segment <= R + r" cannot say where the trace's own surface is.
	 *
	 * Passing SweepFrom == SweepTo asks the same question about a single point, and that is how both
	 * halves of the measurement are taken, so the harness and the game cannot drift apart.
	 *
	 * bFlatAtA / bFlatAtB replace the radial cap at that end of THIS segment with a flat face
	 * through it, perpendicular to the segment. Only ever true for the two OUTER ends of the whole
	 * polyline; see GTrailFlatEndCaps.
	 */
	void SegmentGapToTraceVolume(const FVector& A, const FVector& B, bool bFlatAtA, bool bFlatAtB,
		const FVector& SweepFrom, const FVector& SweepTo,
		double TraceRadius, double TraceHalfHeight,
		double& OutHorizontal, double& OutVertical, bool& bOutBeyondFlatCap)
	{
		bOutBeyondFlatCap = false;

		const FVector FlatFrom(SweepFrom.X, SweepFrom.Y, 0.0);
		const FVector FlatTo(SweepTo.X, SweepTo.Y, 0.0);
		const FVector FlatA(A.X, A.Y, 0.0);
		const FVector FlatB(B.X, B.Y, 0.0);

		// Returns void — the closest point on each segment, not the distance.
		FVector ClosestOnSweep = FVector::ZeroVector;
		FVector ClosestOnTrail = FVector::ZeroVector;
		FMath::SegmentDistToSegmentSafe(FlatFrom, FlatTo, FlatA, FlatB, ClosestOnSweep, ClosestOnTrail);

		// The radial answer: distance to the CLAMPED segment, i.e. round caps at both ends. This is
		// what the trip test has always computed and it stays the answer everywhere except the two
		// outer caps.
		double Lateral = FVector::Dist(ClosestOnSweep, ClosestOnTrail);
		double Overshoot = 0.0;

		const FVector Axis = FlatB - FlatA;
		const double AxisLength = Axis.Size();

		// A degenerate (zero-length in plan) segment has no direction to be flat against, so it keeps
		// its disc — which is the honest shape for it, and is what a one-point trace is drawn as.
		if (AxisLength > GeometryEpsilon && (bFlatAtA || bFlatAtB))
		{
			const FVector Direction = Axis / AxisLength;
			const FVector Offset = ClosestOnSweep - FlatA;
			const double Along = FVector::DotProduct(Offset, Direction);
			const double Perpendicular = (Offset - Direction * Along).Size();

			if (bFlatAtA && Along < 0.0)
			{
				Overshoot = -Along;
				Lateral = Perpendicular;
				bOutBeyondFlatCap = true;
			}
			else if (bFlatAtB && Along > AxisLength)
			{
				Overshoot = Along - AxisLength;
				Lateral = Perpendicular;
				bOutBeyondFlatCap = true;
			}
		}

		const double LateralGap = FMath::Max(0.0, Lateral - TraceRadius);
		OutHorizontal = (Overshoot > 0.0)
			? FMath::Sqrt(Overshoot * Overshoot + LateralGap * LateralGap)
			: LateralGap;

		// Heights are compared WHERE THE CLOSEST HORIZONTAL APPROACH HAPPENED — the flattened test
		// threw the Z away — exactly as the trip test has always done it.
		const double SweepAlpha = SegmentAlpha(FlatFrom, FlatTo, ClosestOnSweep);
		const double ToucherZ = FMath::Lerp(SweepFrom.Z, SweepTo.Z, SweepAlpha);

		double TrailZ = 0.0;
		if (AxisLength > GeometryEpsilon)
		{
			const double TrailAlpha = SegmentAlpha(FlatA, FlatB, ClosestOnTrail);
			TrailZ = FMath::Lerp(A.Z, B.Z, TrailAlpha);
		}
		else
		{
			// A SEGMENT THAT IS A POINT IN PLAN — i.e. a FALL — and this branch is a v14 §1 bug fix,
			// not a tidy-up.
			//
			// SegmentAlpha of a zero-length segment returns 0, so the old code compared every height
			// against A.Z and NEVER against B.Z. On a carrier falling 300uu that made the lethal volume
			// five disconnected 63uu bands stacked around the points, with the LAST point's band
			// missing altogether — the bottom 60uu of a fall was drawn and killed nothing, measured at
			// 60.2uu of over-draw on the VERTICAL DROP fixture. Since there is no horizontal
			// information to interpolate by, the honest volume is the whole span the segment covers:
			// clamp into [A.Z, B.Z] and the column becomes continuous, exactly as the drawn element's
			// unioned band already was.
			TrailZ = FMath::Clamp(ToucherZ, FMath::Min(A.Z, B.Z), FMath::Max(A.Z, B.Z));
		}

		OutVertical = FMath::Max(0.0, FMath::Abs(ToucherZ - TrailZ) - TraceHalfHeight);
	}

	/**
	 * The same solid, asked about a single world point: how far outside the LETHAL volume is it?
	 *
	 * Zero on both axes means the point is inside the thing that kills. The pair reported is the one
	 * from the segment with the smallest COMBINED gap — a point is outside the whole volume only if
	 * it is outside every segment's — and the two components are then handed back as they stand,
	 * because the vertical overhang is deliberate (the union of an element's two ends' bands) and
	 * the horizontal one is not. Rolling them together once made a documented design choice read as
	 * a fitter bug.
	 */
	void MeasurePointGapToTrace(const TArray<FVector>& Polyline, const FVector& At,
		double TraceRadius, double TraceHalfHeight,
		double& OutHorizontal, double& OutVertical, bool& bOutBeyondEnd)
	{
		OutHorizontal = 0.0;
		OutVertical = 0.0;
		bOutBeyondEnd = false;

		if (Polyline.Num() == 0)
		{
			return;
		}

		const bool bFlatCaps = (GTrailFlatEndCaps != 0);
		const int32 LastSegment = FMath::Max(0, Polyline.Num() - 2);

		double BestCombined = TNumericLimits<double>::Max();

		for (int32 SegmentIndex = 0; SegmentIndex <= LastSegment; ++SegmentIndex)
		{
			const FVector& A = Polyline[SegmentIndex];
			const FVector& B = Polyline[FMath::Min(SegmentIndex + 1, Polyline.Num() - 1)];

			double Horizontal = 0.0;
			double Vertical = 0.0;
			bool bBeyond = false;
			SegmentGapToTraceVolume(A, B, bFlatCaps, bFlatCaps,
				At, At, TraceRadius, TraceHalfHeight, Horizontal, Vertical, bBeyond);

			const double Combined = FMath::Sqrt(Horizontal * Horizontal + Vertical * Vertical);
			if (Combined < BestCombined)
			{
				BestCombined = Combined;
				OutHorizontal = Horizontal;
				OutVertical = Vertical;
				bOutBeyondEnd = bBeyond;
			}

			if (BestCombined <= 0.0)
			{
				break;
			}
		}
	}

	/** True when @p At is inside the volume the server kills along. The harness's filter. */
	bool IsPointLethal(const TArray<FVector>& Polyline, const FVector& At,
		double TraceRadius, double TraceHalfHeight)
	{
		double Horizontal = 0.0;
		double Vertical = 0.0;
		bool bBeyond = false;
		MeasurePointGapToTrace(Polyline, At, TraceRadius, TraceHalfHeight, Horizontal, Vertical, bBeyond);
		return (Horizontal <= 0.0) && (Vertical <= 0.0);
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

#if !UE_BUILD_SHIPPING
	// =============================================================================================
	// SPEC v10 §2 — THE STAGED PROBE THAT MAKES THE CLAIM MEASURABLE
	//
	// Trace.Trail.ModelHitTest has to put a dashing enemy at a KNOWN perpendicular distance from a
	// KNOWN lethal segment and then read what the trip test scored. Doing that from a console-command
	// ticker does not work, and the reason is worth writing down because the obvious version looks
	// fine and silently measures nothing:
	//
	//   FTSTicker runs in FEngineLoop::Tick, BEFORE UWorld::Tick. So between a ticker placing the
	//   pawn and ServerRunTripTest reading GetActorLocation(), the character movement component has
	//   run a whole frame — and the pawn is DASHING, which is the entire point, so it has moved tens
	//   of uu. The band this test is about is a few uu wide. The measurement would be pure drift.
	//
	// So the placement happens INSIDE ServerRunTripTest, on the frame the test runs, immediately
	// before the candidate loop. Nothing downstream is stubbed: the eligibility rules (alive, enemy,
	// DASHING), the swept segment-to-segment geometry, the thresholds, the kill, the parry punish and
	// the counters are all the shipping path, evaluated on positions this probe fixed rather than on
	// positions that drifted.
	//
	// PreviousLocation is seeded too, via the component's own debug hook, so the swept segment is
	// exactly the radial approach the test asserts about and not whatever the last frame left behind.
	// =============================================================================================
	struct FTraceModelProbe
	{
		bool bArmed = false;

		TWeakObjectPtr<ATraceCharacter> Holder;
		TWeakObjectPtr<ATraceCharacter> Dasher;

		/** Perpendicular distance from the trace centreline to put the dasher's CAPSULE CENTRE at. */
		double Offset = 0.0;

		/**
		 * The case's parry precondition, enforced HERE rather than hoped for by the caller. A parry
		 * window is 0.175-0.2s long and on a cooldown, so a harness that merely asks for one and then
		 * places the pawn some frames later is measuring whatever the window happened to be doing.
		 */
		bool bRequireParry = false;

		/** Set by the probe when it actually placed the pawn — i.e. the frame the test really ran. */
		bool bApplied = false;

		/** What it used, for the report. */
		FVector AppliedStart = FVector::ZeroVector;
		FVector AppliedEnd = FVector::ZeroVector;

		/** The TRUE closest approach of the applied sweep to the WHOLE trace. See PickProbePlacement. */
		double AppliedClearance = 0.0;

		/**
		 * v13 §7: HOW MUCH RIBBON WAS ON SCREEN AT THE DASH POINT, SAMPLED WHEN THE PAWN WAS PLACED.
		 *
		 * Filled by the probe rather than read by the harness afterwards, and that ordering is the
		 * whole point. A scored trip kills somebody on the same frame, possession ends, and ClearTrail
		 * wipes the drawing immediately (spec §9's instant clear) — so asking "was anything drawn
		 * there?" one tick later asks about a trace that has already, correctly, ceased to exist.
		 * Trace.Trail.WallFitLive did exactly that on its first run and reported an INVISIBLE KILL
		 * VOLUME about a kill that had been perfectly visible.
		 *
		 * -1 means nothing was drawn at all; 0 means the point was inside a visible piece.
		 */
		double DrawnAtCentre = -1.0;
		double DrawnAtModelEdge = -1.0;

		/** Why it could not run yet, so a hung harness names its own reason. */
		const TCHAR* LastRefusal = TEXT("not armed");
	};

	FTraceModelProbe GTraceModelProbe;

	/** How far back along the approach the swept segment starts. Well under MinTeleportSweepDistance. */
	constexpr double ModelProbeApproachLength = 150.0;

	/**
	 * The XY distance from a swept segment to the CLOSEST POINT OF THE WHOLE POLYLINE — not to one
	 * chosen segment of it.
	 *
	 * THIS FUNCTION IS THE FIX FOR A HARNESS THAT LIED, and the lie is worth recording because it is
	 * the exact shape of mistake this project's testing discipline exists to catch. The first version
	 * picked a segment, stepped off it perpendicularly by the offset under test, and asserted. It
	 * reported a CONNECTION on the capsule-only arm at 61.5uu against a 56.5uu threshold, which is
	 * geometrically impossible — until you notice that a carrier's trace is a 1200uu polyline that
	 * doubles back on itself, so "61.5uu from THAT segment" was 30uu from a DIFFERENT one. The test
	 * was scoring a real hit on a piece of trace it was not talking about, and a red arm that goes
	 * red for the wrong reason is worth nothing.
	 */
	double MinSweepDistanceToTraceXY(const TArray<FVector>& Positions, const FVector& From, const FVector& To)
	{
		const FVector SweepStart(From.X, From.Y, 0.0);
		const FVector SweepEnd(To.X, To.Y, 0.0);

		double Best = TNumericLimits<double>::Max();
		const int32 LastSegment = FMath::Max(0, Positions.Num() - 2);
		for (int32 SegmentIndex = 0; SegmentIndex <= LastSegment; ++SegmentIndex)
		{
			const FVector& TrailStart = Positions[SegmentIndex];
			const FVector& TrailEnd = Positions[FMath::Min(SegmentIndex + 1, Positions.Num() - 1)];

			FVector ClosestOnSweep = FVector::ZeroVector;
			FVector ClosestOnTrail = FVector::ZeroVector;
			FMath::SegmentDistToSegmentSafe(SweepStart, SweepEnd,
				FVector(TrailStart.X, TrailStart.Y, 0.0), FVector(TrailEnd.X, TrailEnd.Y, 0.0),
				ClosestOnSweep, ClosestOnTrail);

			Best = FMath::Min(Best, FVector::Dist(ClosestOnSweep, ClosestOnTrail));
		}
		return Best;
	}

	/**
	 * Find a placement whose closest approach to the WHOLE trace really is the offset under test.
	 *
	 * Tries every segment, both perpendicular directions, and accepts only a placement the geometry
	 * agrees with to within ModelProbeClearanceTolerance. If no candidate qualifies on this frame the
	 * probe simply waits — a trace that has coiled up on itself has no clean approach at this offset,
	 * and refusing to test is the honest answer.
	 */
	constexpr double ModelProbeClearanceTolerance = 0.75;

	bool PickProbePlacement(const TArray<FVector>& Positions, double Offset, FVector& OutStart,
		FVector& OutEnd, double& OutClearance)
	{
		const int32 LastSegment = FMath::Max(0, Positions.Num() - 2);

		// Walk outward from the middle of the lethal set: clear of the head stub at one end and of the
		// tail the length trim is eating at the other, so a chosen placement cannot move under the test.
		const int32 Centre = Positions.Num() / 2 - 1;

		for (int32 Step = 0; Step <= LastSegment; ++Step)
		{
			for (int32 Sign = -1; Sign <= 1; Sign += 2)
			{
				const int32 Index = FMath::Clamp(Centre + (Step * Sign), 0, LastSegment);

				const FVector A = Positions[Index];
				const FVector B = Positions[FMath::Min(Index + 1, Positions.Num() - 1)];

				FVector Direction = B - A;
				Direction.Z = 0.0;
				if (!Direction.Normalize())
				{
					continue;
				}

				const FVector Midpoint = (A + B) * 0.5;

				for (int32 Side = -1; Side <= 1; Side += 2)
				{
					const FVector Perpendicular = FVector(-Direction.Y, Direction.X, 0.0) * static_cast<double>(Side);

					const FVector End = Midpoint + Perpendicular * Offset;
					const FVector Start = End + Perpendicular * ModelProbeApproachLength;

					const double Clearance = MinSweepDistanceToTraceXY(Positions, Start, End);
					if (FMath::Abs(Clearance - Offset) <= ModelProbeClearanceTolerance)
					{
						OutStart = Start;
						OutEnd = End;
						OutClearance = Clearance;
						return true;
					}
				}
			}
		}
		return false;
	}

	void ApplyStagedModelProbe(UTraceTrailComponent* Trail, ATraceCharacter* Holder,
		const TArray<FVector>& TestPositions)
	{
		if (!GTraceModelProbe.bArmed || Trail == nullptr)
		{
			return;
		}
		if (GTraceModelProbe.Holder.Get() != Holder)
		{
			return;   // Another carrier's trace; not the one under test.
		}

		ATraceCharacter* Dasher = GTraceModelProbe.Dasher.Get();
		if (Dasher == nullptr || !Dasher->IsAlive())
		{
			GTraceModelProbe.LastRefusal = TEXT("the dasher is gone or dead");
			return;
		}
		if (TestPositions.Num() < 2)
		{
			GTraceModelProbe.LastRefusal = TEXT("the carrier's trace has fewer than two lethal points");
			return;
		}

		// THE DASH GATE IS NOT BYPASSED. The probe waits for a real dash rather than faking one,
		// because "walking through a trace does nothing" is a rule this harness must not be able to
		// launder. It simply tries again next frame.
		if (!Dasher->IsDashing())
		{
			GTraceModelProbe.LastRefusal = TEXT("the dasher is not dashing yet");
			return;
		}

		// THE PARRY PRECONDITION, checked on the frame the test runs and not on the frame it was
		// requested. Both directions matter: a parry case measured with the window shut proves
		// nothing, and a kill case measured with a window open would score a punish instead.
		if (Trail->IsParryActive() != GTraceModelProbe.bRequireParry)
		{
			GTraceModelProbe.LastRefusal = GTraceModelProbe.bRequireParry
				? TEXT("waiting for the carrier's parry window to be open")
				: TEXT("waiting for the carrier's parry window to close");
			return;
		}

		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		double Clearance = 0.0;
		if (!PickProbePlacement(TestPositions, GTraceModelProbe.Offset, Start, End, Clearance))
		{
			GTraceModelProbe.LastRefusal =
				TEXT("no approach on this trace has the offset under test as its CLOSEST approach "
				     "(the trace has doubled back on itself) — waiting for a cleaner frame");
			return;
		}

		// Z matched to the trail point, which is the carrier's own capsule centre — so the vertical
		// half of the test is trivially satisfied and the only variable under examination is the
		// HORIZONTAL one this section is about.
		Dasher->SetActorLocation(End, false, nullptr, ETeleportType::TeleportPhysics);
		Trail->DebugSeedPreviousLocation(Dasher, Start);

		GTraceModelProbe.AppliedStart = Start;
		GTraceModelProbe.AppliedEnd = End;
		GTraceModelProbe.AppliedClearance = Clearance;

		// Sampled NOW, while the trace is still standing. See the field comment.
		{
			const FTraceModelReach Reach = UTraceTrailComponent::MeasureModelReach(Dasher);

			FVector Inward = End - Start;
			Inward.Z = 0.0;
			const FVector NearestModelPoint = Inward.Normalize()
				? (End - Inward * Reach.EffectiveRadius)
				: End;

			GTraceModelProbe.DrawnAtCentre = Trail->DistanceToDrawnRibbon(End);
			GTraceModelProbe.DrawnAtModelEdge = Trail->DistanceToDrawnRibbon(NearestModelPoint);
		}
		GTraceModelProbe.bApplied = true;
		GTraceModelProbe.bArmed = false;
		GTraceModelProbe.LastRefusal = TEXT("applied");
	}

	/**
	 * The last trip the test scored, recorded where it happens so the harness reads a fact rather
	 * than inferring one from a counter that any bot in the match could also have moved.
	 */
	int32 GModelTripSerial = 0;
	TWeakObjectPtr<ATraceCharacter> GModelTripLastDasher;

	/**
	 * v13 §7: WHOSE trace the last scored trip was against.
	 *
	 * A harness that stages one dasher against one carrier and then asks "did anything trip?" is
	 * asking a question the whole match can answer. Trace.Trail.WallFitLive's OFF case failed on
	 * exactly that: the staged pawn was placed 88.5uu out, far beyond the 76.5uu the trip test can
	 * reach, and a connection was still recorded — because the same bot, still dashing, legitimately
	 * cut through a DIFFERENT carrier's trace a few frames later. Without the holder the fixture would
	 * have reported an invisible kill volume that did not exist.
	 */
	TWeakObjectPtr<ATraceCharacter> GModelTripLastHolder;
	bool GModelTripLastWidened = false;
	double GModelTripLastMargin = 0.0;
	double GModelTripLastThreshold = 0.0;
	double GModelTripLastCapsuleThreshold = 0.0;
#endif // !UE_BUILD_SHIPPING
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
		// ------------------------------------------------------------------------------------------
		// THE INVARIANT, ASSERTED EVERY SERVER FRAME (spec v9 §§3-4):
		//
		//     TRAIL POINTS MAY EXIST ONLY WHILE THIS COMPONENT IS EMITTING FOR A LIVE OWNER WHO IS
		//     ACTUALLY HOLDING THE CORE. Anything else is an ORPHAN and is wiped on this frame.
		//
		// WHAT THIS REPLACED, AND WHY IT WAS THE BUG. The old condition was
		//
		//     (Holder == nullptr || !Holder->IsAlive()) && (bEmitting || Items.Num() > 0)
		//
		// i.e. it only ever fired for a DEAD OR DESTROYED owner. A player who PASSES to a living
		// teammate is neither: they are alive, standing there, no longer the carrier — and their trace
		// did not qualify for cleanup by any clause. Since v7 §1 removed time-based expiry there was
		// no backstop left either, so those points lived until the passer next died. That single gate
		// produced both reported symptoms (spec v9 §§3-4), exactly as the spec's lead said it would.
		//
		// WHY THE TEST IS WRITTEN ON THIS COMPONENT'S OWN STATE and not on "is the Core's holder me":
		// bEmitting and liveness are facts this component owns and can never be wrong about, while
		// the Core's opinion of the holder arrives through ATraceCharacter::SetCarrying and could
		// legitimately be one call deep at the instant this runs. IsCarrier() is still consulted — it
		// catches a route that forgot to stop the emission altogether — but it is reported when it is
		// the clause that fires, because that would mean a possession-end path is bypassing
		// ReleaseHolder() and the log should say so rather than silently paper over it.
		//
		// This is a SAFETY NET, not the mechanism. SetEmitting(false) already cleared, in the same
		// call stack as the possession change; if this ever fires, something reached "not a carrier"
		// without going through it, and the trace is still gone on the very next server frame.
		{
			const ATraceCharacter* Holder = GetOwnerCharacter();
			const bool bOwnerGone = (Holder == nullptr) || !Holder->IsAlive();
			const bool bHasTrace = bEmitting || TrailPoints.Items.Num() > 0;

			// Emission without possession: a route that ended possession without stopping this
			// component. Never expected; always logged if seen.
			const bool bEmittingWithoutCore = !bOwnerGone && bEmitting && !Holder->IsCarrier();

			// Points without emission: possession already ended and the clear did not happen.
			const bool bPointsWithoutEmission = !bEmitting && TrailPoints.Items.Num() > 0;

			if (bHasTrace && (bOwnerGone || bEmittingWithoutCore
				|| (bPointsWithoutEmission && GClearTraceOnPossessionLoss != 0)))
			{
				if (bEmittingWithoutCore || (bPointsWithoutEmission && !bOwnerGone))
				{
					UE_LOG(LogTraceGame, Log,
						TEXT("[TRACEORPHAN] %s: swept an orphaned trace (%d points, emitting=%d, alive=%d, "
						     "carrier=%d). The possession-end path did not clear it — see SetEmitting()."),
						*GetNameSafe(GetOwner()), TrailPoints.Items.Num(), bEmitting ? 1 : 0,
						(Holder != nullptr && Holder->IsAlive()) ? 1 : 0,
						(Holder != nullptr && Holder->IsCarrier()) ? 1 : 0);
				}

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

		// SPEC v8 §2. BEFORE UpdatePredictedHead and deliberately not inside it: the stub has a dozen
		// early-outs, and the frames where it declines to draw are exactly the frames whose movement
		// the next successful stub needs to know about. A recorder behind those early-outs would go
		// blind precisely when the carrier was dashing.
		RecordLocalPathSample();

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

double UTraceTrailComponent::GetTraceDrawnHalfReach()
{
	const double Radius = FMath::Max(1.0, static_cast<double>(GetTraceTrailRadius()));

	// 1. THE JOINT OVERLAP, straight out of PlaceRibbon. An interior element is extended by
	//    JointOverlap == TrailRadius along its own axis at each joint, and it is a BOX of half width
	//    TrailRadius, so the corner of that box stands Radius * sqrt(2) from the joint it overlaps.
	//    Read this together with PlaceRibbon: if that overlap ever changes, this must change with it.
	const double JointCorner = Radius * UE_DOUBLE_SQRT_2;

	// 2. THE SPLINE OVERSHOOT. BuildRibbonSamples resamples the polyline through a Catmull-Rom with
	//    the standard 0.5 tension. A Catmull-Rom does not stay inside its control polygon: for the
	//    tightest turn a running carrier can lay — a right angle with one TrailPointSpacing per leg —
	//    the curve passes 0.0741 * spacing OUTSIDE the corner vertex. (Solve dX/dt = 0 on the segment
	//    B->C with control points A,B,C,C: the extremum is at t = 1/3 and sits 4.44uu past B for
	//    60uu legs.) Sharper than a right angle is not reachable: the carrier is a 34uu capsule and
	//    cannot reverse inside 60uu of travel.
	//
	//    NOT scaled by the resample step. The curve deviates from the polyline by this much however
	//    finely it is sampled; a finer step draws the same overshoot with more elements.
	const double Spacing = FMath::Max(1.0, static_cast<double>(UTraceSettings::Get().TrailPointSpacing));
	const double SplineOvershoot = 0.0741 * Spacing;

	return JointCorner + SplineOvershoot;
}

float UTraceTrailComponent::GetTurnoverGraceSeconds()
{
	const float Value = (GTurnoverGraceOverride >= 0.f)
		? GTurnoverGraceOverride : UTraceSettings::Get().CoreTurnoverGraceSeconds;
	return FMath::Clamp(Value, 0.f, 5.f);
}


// =================================================================================================
// SPEC v10 §2 — MEASURING THE MODEL
// =================================================================================================

FTraceModelReach UTraceTrailComponent::MeasureModelReach(const ATraceCharacter* Candidate)
{
	FTraceModelReach Reach;

	if (Candidate == nullptr)
	{
		return Reach;
	}

	// The capsule is both the old answer and the floor under the new one: whatever the mesh says,
	// the physical body of the pawn is at least this wide, and shrinking the test is not on the
	// table.
	if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
	{
		Reach.CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		Reach.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}
	Reach.RawMeshRadius = Reach.CapsuleRadius;
	Reach.RawMeshHalfHeight = Reach.CapsuleHalfHeight;

	const double MinMargin = FMath::Max(0.0, static_cast<double>(GModelMarginMin));
	const double MaxMargin = FMath::Max(MinMargin, static_cast<double>(GModelMarginMax));

	// The A/B arm. Arm 0 is byte-for-byte the pre-v10 geometry: capsule, no margin, no measurement.
	if (GWholeModelTrip == 0)
	{
		Reach.EffectiveRadius = Reach.CapsuleRadius;
		Reach.EffectiveHalfHeight = Reach.CapsuleHalfHeight;
		return Reach;
	}

	// The vertical margin is a FLAT number and never a measurement, and the measurement is why. See
	// GModelMarginVertical: the mesh reports 141uu of vertical half-extent against an 88uu capsule
	// that already contains the whole model, so taking the mesh's answer here scores on empty air
	// under a dasher's feet. Default 0 — the vertical test is untouched by this pass.
	const double VerticalMargin = FMath::Max(0.0, static_cast<double>(GModelMarginVertical));
	Reach.EffectiveHalfHeight = Reach.CapsuleHalfHeight + VerticalMargin;

	// The fixed-margin option (spec v10 §2's second choice), when it has been pinned.
	if (GModelMarginFixed >= 0.f)
	{
		const double Fixed = FMath::Min(static_cast<double>(GModelMarginFixed), MaxMargin);
		Reach.EffectiveRadius = Reach.CapsuleRadius + Fixed;
		return Reach;
	}

	// --- the measurement --------------------------------------------------------------------------
	//
	// MEASURED FROM THE ACTOR LOCATION, NOT FROM THE BOUNDS ORIGIN. This is the whole reason the
	// function is not a one-liner. The trip test sweeps the pawn's ACTOR LOCATION against the trace
	// polyline, and the trace polyline is itself laid at the carrier's actor location — one reference
	// frame, deliberately (see ServerUpdateTrail). A skeletal mesh, however, is attached with an
	// offset: the Mannequin sits ~88uu below the capsule centre and is free to be off-centre in XY
	// too. Reporting Bounds.BoxExtent alone would therefore describe a box around the MESH while the
	// test measures distance from the CAPSULE, and the two would disagree by the offset — quietly,
	// and in the direction that loses hits, which is the bug this is fixing.
	//
	// So: take the half-width of the smallest box CENTRED ON THE ACTOR that still contains the mesh
	// bounds, per axis, and use the larger horizontal axis. Larger and not the diagonal: the
	// threshold is applied isotropically, so the diagonal (a factor of 1.41) would score on air in
	// the two corners for the sake of geometry no player can see.
	const USkeletalMeshComponent* Mesh = Candidate->GetMesh();
	if (Mesh != nullptr && Mesh->GetSkinnedAsset() != nullptr)
	{
		const FBoxSphereBounds MeshBounds = Mesh->Bounds;
		const FVector Extent = MeshBounds.BoxExtent;

		// A zero/absurd box means the bounds have never been computed on this machine (an animation
		// that has not ticked, a mesh that has not been registered). Rejected rather than trusted:
		// a garbage measurement that happens to be small would silently reinstate the bug, and one
		// that happens to be huge would score on air.
		const bool bBoundsUsable = Extent.X > 1.0 && Extent.Y > 1.0 && Extent.Z > 1.0
			&& Extent.GetMax() < 1000.0;

		if (bBoundsUsable)
		{
			const FVector ActorLocation = Candidate->GetActorLocation();
			const FVector Offset = MeshBounds.Origin - ActorLocation;

			Reach.RawMeshRadius = FMath::Max(
				FMath::Abs(Offset.X) + Extent.X,
				FMath::Abs(Offset.Y) + Extent.Y);
			Reach.RawMeshHalfHeight = FMath::Abs(Offset.Z) + Extent.Z;
			Reach.bMeshMeasured = true;
		}
	}

	// Clamp into [capsule + MinMargin, capsule + MaxMargin]. Both ends matter: the low end is what
	// keeps the fix alive on a pawn whose mesh reports less than its capsule, the high end is the
	// stated worst-case over-reach and is the reason an attached actor or a fat physics body cannot
	// turn this into a phantom connection.
	Reach.EffectiveRadius = FMath::Clamp(
		Reach.RawMeshRadius,
		Reach.CapsuleRadius + MinMargin,
		Reach.CapsuleRadius + MaxMargin);

	// EffectiveHalfHeight was set above from the FLAT vertical margin, deliberately, and
	// RawMeshHalfHeight is measured but not used — it is reported by Trace.Trail.ModelReach so the
	// decision not to use it stays auditable instead of becoming folklore.
	return Reach;
}

void UTraceTrailComponent::GetModelTripStats(int32& OutTotal, int32& OutModelOnly)
{
	OutTotal = GModelTripsTotal;
	OutModelOnly = GModelTripsWidened;
}

void UTraceTrailComponent::ResetModelTripStats()
{
	GModelTripsTotal = 0;
	GModelTripsWidened = 0;
}

#if !UE_BUILD_SHIPPING
void UTraceTrailComponent::DebugSeedPreviousLocation(ATraceCharacter* Candidate, const FVector& Location)
{
	if (Candidate != nullptr)
	{
		PreviousLocations.Add(Candidate, Location);
	}
}
#endif

void UTraceTrailComponent::SetEmitting(bool bEmit)
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	if (bEmitting == bEmit)
	{
		// SPEC v9 §3, AND THIS EARLY-OUT IS HALF OF WHY THE BUG SURVIVED A "STOP EMITTING" CALL.
		//
		// The stop is IDEMPOTENT but the CLEAR was not reached through it: the first SetEmitting(false)
		// flipped the flag, and every later assertion — the GameMode wiping every trail on a goal, on
		// half time, on a kickoff, ATraceCharacter::HandleDeath's belt-and-braces call — returned right
		// here without ever looking at the points. So a component that had somehow been left holding
		// points with bEmitting already false could never be swept up by anything short of its owner
		// dying. Answering the points as well as the flag makes "stop" mean "and leave nothing behind"
		// on EVERY call rather than only on the first.
		//
		// ClearTrail() is itself guarded on there being something to clear, so the common case (a
		// non-carrier being told to stop for the tenth time) is still two branches and no RPC.
		if (!bEmit && GClearTraceOnPossessionLoss != 0)
		{
			ClearTrail();
		}
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

		// ==========================================================================================
		// SPEC v9 §§3-4. STOPPING CLEARS. THIS LINE IS THE FIX, AND EVERY POSSESSION-END ROUTE IN THE
		// GAME ARRIVES AT IT.
		//
		// The comment that used to stand here said the opposite — "stopping does NOT clear ... a trace
		// left behind by a completed pass is harmless (the trip test requires bEmitting) and fading
		// out over its lifetime reads much better than popping". BOTH OF ITS PREMISES ARE NOW FALSE,
		// and they were falsified by two later changes that each looked local and correct:
		//
		//   * "the trip test requires bEmitting" — it does not, and deliberately so. The gate is the
		//     POINTS (see ServerRunTripTest's block comment), because a residual trace being inert was
		//     itself once a reported bug. So the passer's abandoned trace does not merely hang in the
		//     air, IT KILLS THEM. Being killed by trace you laid while holding a Core you no longer
		//     hold is the unfair death §3 is about.
		//   * "fading out over its lifetime" — there is no lifetime. v7 §1 removed time-based expiry
		//     to kill the stand-still exploit, and points now leave only when NEW trace pushes them
		//     off. A component that has stopped emitting lays no new trace, so its points are retired
		//     by nothing, ever. That is §4's "traces stay on the map way after ... a pass", verbatim.
		//
		// WHY HERE AND NOT AT THE CALL SITES. ATraceCore::ReleaseHolder() is the documented single
		// funnel — "every path that takes the Core off somebody funnels through here - a completed
		// pass, a kill, a disconnect, a score, half time" — and it ends in exactly one call, to this
		// function. Mode B's throw, the goal reset, the half-time wipe and the kickoff all reach it
		// the same way. Clearing here therefore covers all eight of the routes spec v9 §4 lists,
		// including any future ninth, and it does it in the SAME CALL STACK as the possession change:
		// no grace, no fade, no one-frame window in which the trip test could still see the points.
		//
		// THIS IS NOT A TIMER AND DOES NOT REINTRODUCE ONE. It is triggered by an event — possession
		// leaving — and a carrier who stands still holding the Core is still emitting, so nothing here
		// touches them. The stand-still exploit stays dead.
		if (GClearTraceOnPossessionLoss != 0)
		{
			ClearTrail();
		}
		// ==========================================================================================
	}

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

	// 1.0s -> 0.4s (v3 §1) -> 0.5 -> 0.75s (v10 §3). Capped rather than replaced, so a caller asking
	// for less still wins and a stale constant in TraceCoreTuning cannot reinstate an old rule. See
	// the header for the full reason.
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
//   Source 2, THE PARRY (v3 §3, widened by v8 §3). 0.2s on a 1.5s cooldown. Owned by this component. It does NOT
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

	// v8 §3. The stamp is the whole fix: it is what lets the server judge this parry against when it
	// was PRESSED instead of when the packet landed. Same clock as the window it will open.
	ServerRequestParry(TraceParry::GetPressStampSeconds(OwnerActor));
}

void UTraceTrailComponent::ServerRequestParry_Implementation(float ClientPressServerTime)
{
	ETraceParryRefusal Refusal = ETraceParryRefusal::None;
	ServerTryBeginParry(Refusal, ClientPressServerTime);
}

bool UTraceTrailComponent::ServerTryBeginParry(ETraceParryRefusal& OutRefusal, float ClientPressServerTime)
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

	// v8 §3. ONE call owns the clamp, the anchor and the cooldown test, so the rewind rule lives in
	// one place — next to the shot rewind it copies. The cooldown is now asked AT THE PRESS: a
	// client whose cooldown expired 5ms before they pressed is not refused for their own latency.
	float PressServerTime = Now;
	float WindowEnd = 0.f;
	float CooldownEnd = 0.f;
	if (!TraceParry::ServerResolvePress(GetOwnerCharacter(), ClientPressServerTime, Now,
			ParryCooldownEndServerTime, PressServerTime, WindowEnd, CooldownEnd, OutRefusal))
	{
		return false;   // OutRefusal is set (OnCooldown / NoPawn)
	}

	ParryEndServerTime = WindowEnd;
	ParryCooldownEndServerTime = CooldownEnd;

	// A listen server is also a viewer: it will not get OnRep, so dirty the visuals here.
	bVisualsDirty = true;

	UE_LOG(LogTraceGame, Log,
		TEXT("[PARRY] %s parried: trace invulnerable for %.2fs, next parry in %.2fs. "
		     "(press anchored %.0fms before arrival)"),
		*GetNameSafe(GetOwner()), TraceParry::GetDurationSeconds(), TraceParry::GetCooldownSeconds(),
		(Now - PressServerTime) * 1000.f);

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
	// exempt: a residual trace is lethal end to end.
	//
	// SPEC v9 §§3-4 MADE THIS BRANCH ALL BUT UNREACHABLE ON THE AUTHORITY, and that is the fix rather
	// than a reason to delete it. Possession leaving now clears the points in the same call stack, so
	// "not emitting AND still holding points" is no longer a state the server can be in — and this
	// branch was the teeth on the trace that outlived a pass: it promoted every last point, including
	// the one under the ex-carrier's own feet, into the lethal set. It stays because it is still the
	// correct answer for the frame a clear is in flight, for a client reading a not-yet-emptied
	// array, and for the pre-v9 A/B arm (Trace.Trail.ClearOnPossessionLoss 0), which needs the old
	// behaviour intact to be worth measuring against.
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

	// SPEC v12 §6. The remembered route belongs to the trace that just went away. Kept across a clear
	// it would let the FIRST point of the next trace be subdivided along the previous holder's path —
	// a stretch of lethal, drawn trace laid where this carrier has never been.
	PendingPathSamples.Reset();

	bVisualsDirty = true;
}

void UTraceTrailComponent::MulticastClearTrail_Implementation()
{
	HideSmearFrom(0);
	ClearGhostRecords();

	// The predicted stub is the newest thing on screen, so it is the one piece a dying holder would
	// most obviously leave hanging in the air. It goes on the same frame as everything else.
	HidePredictedHead();

	// And so does the record it would be rebuilt from (v8 §2). RecordLocalPathSample() would clear it
	// on the next tick anyway — bEmitting is false by the time a trace is cleared — but "anyway" and
	// "one frame later" are the same sentence, and this is the frame the trace is supposed to vanish.
	LocalPathHistory.Reset();

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
//
// -------------------------------------------------------------------------------------------------
// SPEC v8 §2 — "Trace still has the same bug for the people connecting to the server."
//
// The mechanism above is right. The KEY IT SORTED ON WAS NOT, and the counter that was quoted as
// evidence ("ORD 2 -> 0") measured the same wrong thing, so the two agreed with each other rather
// than with the game.
//
//   v7 sorted on BirthServerTime — a replicated FLOAT, with `<`, using a STABLE sort. Two points
//   carrying the same stamp are therefore left in exactly the order RemoveAtSwap put them, and the
//   violation counter, which uses the same strict comparison, scores that array as perfect. The
//   repair and its own proof share one blind spot, which is the worst possible arrangement: the
//   measurement cannot fail where the fix does.
//
//   IT NOW SORTS ON ReplicationID, which is not a proxy for the append order — it is the append
//   order. FFastArraySerializer::MarkItemDirty assigns `ReplicationID = ++IDCounter` the first time
//   an item is marked, so the IDs are issued in exactly the sequence the server appended points; they
//   are int32, so there are no ties and no precision to lose; and the engine's own header states the
//   contract this stands on — "the ReplicationID is replicated and in sync between client and server.
//   The indices are not." The wire preserves identity and discards position, so identity is what the
//   order must be rebuilt from. BirthServerTime survives only as a tie-break for an item with no ID.
//
// AND THERE IS NOW A SECOND, INDEPENDENT DETECTOR, because a fix whose only detector is its own sort
// key is how this shipped twice: a GEOMETRIC one. The server refuses to lay a segment longer than
// MaxTrailSegmentLength (it restarts the trace instead, see ServerUpdateTrail), so an adjacent pair
// further apart than that, on a receiving machine, means the array is not a path — whatever produced
// it. That is the thing the player is actually looking at, and it is checked before AND after the
// sort. If the path is still broken afterwards it is logged as a warning rather than quietly
// tolerated, so the next "it is still not fixed" arrives with evidence attached.
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

	// ---------------------------------------------------------------------------------------------
	// SPEC v8 §2 — THE KEY IS ReplicationID, NOT BirthServerTime.
	//
	// v7 §7 sorted on BirthServerTime, which is a REPLICATED FLOAT, and both the sort and the
	// violation counter therefore had the same two blind spots:
	//
	//   TIES ARE INVISIBLE. `<` is strict and the sort is stable, so any two points that carry the
	//   same stamp are left in exactly the order RemoveAtSwap left them — scrambled — while
	//   CountPointOrderViolations() reports a clean zero over the top of it. A fix that measured
	//   "ORD 2 -> 0" would look like a pass in precisely the case where it had done nothing.
	//
	//   IT IS A DERIVED QUANTITY. The stamp is gameplay data that happens to be monotone today. It is
	//   float, it is quantised by nothing, and nothing structurally forbids two points sharing one.
	//
	// ReplicationID has neither problem, and it is not a proxy for the order — it IS the order.
	// FFastArraySerializer::MarkItemDirty assigns `Item.ReplicationID = ++IDCounter` the first time an
	// item is marked, so IDs are handed out in exactly the sequence the server appended points, they
	// are int32 (no ties, no precision), and the engine's own documentation states the contract this
	// relies on: "the ReplicationID is replicated and in sync between client and server. The indices
	// are not." That sentence is the whole bug and the whole fix in one line — the wire preserves
	// identity and discards position, so identity is what the order has to be rebuilt from.
	//
	// BirthServerTime survives as the tie-break, for the one case IDs cannot cover: a point that has
	// somehow never been assigned one (INDEX_NONE), which cannot happen for an item that arrived over
	// the wire but is cheap to be correct about.
	// ---------------------------------------------------------------------------------------------
	auto PathOrderLess = [](const FTraceTrailPoint& A, const FTraceTrailPoint& B)
	{
		if (A.ReplicationID != B.ReplicationID)
		{
			return A.ReplicationID < B.ReplicationID;
		}
		return A.BirthServerTime < B.BirthServerTime;
	};

	// The common case is a single O(n) scan that finds nothing: n is ~21 points at the v7 §2 length.
	//
	// TWO DETECTORS WITH TWO DIFFERENT JOBS, because the report says the v7 §7 fix was not enough and
	// a fix whose only evidence is its own sort key is how that happens twice:
	//
	//   IDENTITY  the array is not in ReplicationID order. This is the RemoveAtSwap scramble, and it
	//             is what the sort REPAIRS — now decided on the integer the engine guarantees to keep
	//             in sync rather than on a replicated float.
	//   GEOMETRY  two adjacent points are further apart than a carrier could possibly have travelled
	//             between them. The server refuses to lay such a segment at all (see
	//             MaxTrailSegmentLength in ServerUpdateTrail — it restarts the trace instead), so on a
	//             receiving machine this can only mean the array is not a path. It is the AUDIT, not a
	//             second repair, and deliberately so: it is checked before and after the sort, and if
	//             it fails while the identity order is already perfect then re-sorting cannot help and
	//             the honest response is a warning naming the numbers, not a silently redrawn ribbon.
	//             It measures the thing the player is looking at rather than the key being sorted on.
	auto WorstAdjacentGap = [this]() -> double
	{
		double Worst = 0.0;
		for (int32 Index = 1; Index < TrailPoints.Items.Num(); ++Index)
		{
			Worst = FMath::Max(Worst, FVector::Dist(
				TrailPoints.Items[Index - 1].Location, TrailPoints.Items[Index].Location));
		}
		return Worst;
	};

	bool bIdentityOutOfOrder = false;
	for (int32 Index = 1; Index < PointCount && !bIdentityOutOfOrder; ++Index)
	{
		bIdentityOutOfOrder = PathOrderLess(TrailPoints.Items[Index], TrailPoints.Items[Index - 1]);
	}

	const double GapBefore = WorstAdjacentGap();
	const bool bGeometryBroken = (GapBefore > MaxTrailSegmentLength);

	if (!bIdentityOutOfOrder)
	{
		// THE SORT WOULD BE A NO-OP, so it is not run — and this branch is deliberately not silent.
		//
		// Sorting anyway would cost an ItemMap rebuild and a full ribbon rebuild EVERY FRAME for as
		// long as the condition lasted, and would achieve nothing. But a path that is in perfect
		// ReplicationID order and still geometrically impossible is a THIRD failure this file does not
		// know about, so it is reported rather than tidied away. bPathBreakReported throttles it to
		// one line per episode: this runs every tick.
		if (bGeometryBroken && !bPathBreakReported)
		{
			bPathBreakReported = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[TRACEORDER] %s: replicated trace is in correct ReplicationID order but has a %.0fuu "
				     "adjacent gap (%d points, limit %.0fuu). The order is NOT the cause here - the point "
				     "set itself is wrong. See RestoreReplicatedPointOrder()."),
				*GetNameSafe(GetOwner()), GapBefore, PointCount, MaxTrailSegmentLength);
		}
		else if (!bGeometryBroken)
		{
			bPathBreakReported = false;
		}

		return false;
	}

	TrailPoints.Items.StableSort(PathOrderLess);

	// ItemMap maps ReplicationID -> index into Items, and we have just moved every index. Emptying it
	// is what forces the engine's ConditionalRebuildItemMap to rebuild it against the new layout on
	// the next delta (its guard is ItemMap.Num() != Items.Num()); without this, the next
	// PostReplicatedChange would write the update for one point into a different point. This is
	// exactly what the engine itself does on the line after its own RemoveAtSwap loop.
	TrailPoints.ItemMap.Empty();

	++PointOrderRepairs;
	bVisualsDirty = true;

	// DID IT ACTUALLY WORK? The v7 §7 pass asserted the repair and measured a proxy for it; this
	// checks the thing the player sees — whether the result is a walkable path — and says so out loud
	// when it is not. A repair that leaves the array broken must never again look like a pass.
	const double GapAfter = WorstAdjacentGap();
	if (GapAfter > MaxTrailSegmentLength)
	{
		if (!bPathBreakReported)
		{
			bPathBreakReported = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[TRACEORDER] %s: sorted into ReplicationID order and the trace is STILL broken - "
				     "worst adjacent gap %.0fuu -> %.0fuu over %d points (limit %.0fuu)."),
				*GetNameSafe(GetOwner()), GapBefore, GapAfter, PointCount, MaxTrailSegmentLength);
		}
	}
	else
	{
		bPathBreakReported = false;
	}

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[TRACEORDER] %s: repaired a scrambled replicated trace (%d points, repair #%d, "
		     "worst adjacent gap %.0fuu -> %.0fuu). FFastArraySerializer removes with RemoveAtSwap - "
		     "see RestoreReplicatedPointOrder()."),
		*GetNameSafe(GetOwner()), PointCount, PointOrderRepairs, GapBefore, GapAfter);

	return true;
}

int32 UTraceTrailComponent::CountPointOrderViolations() const
{
	// Same key as the repair, and that is the point: a counter that measures something the repair does
	// not act on is how "ORD 2 -> 0" got quoted for a bug that was still there.
	int32 Violations = 0;
	for (int32 Index = 1; Index < TrailPoints.Items.Num(); ++Index)
	{
		const FTraceTrailPoint& Previous = TrailPoints.Items[Index - 1];
		const FTraceTrailPoint& Current = TrailPoints.Items[Index];

		const bool bOutOfOrder = (Current.ReplicationID != Previous.ReplicationID)
			? (Current.ReplicationID < Previous.ReplicationID)
			: (Current.BirthServerTime < Previous.BirthServerTime);

		if (bOutOfOrder)
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

float UTraceTrailComponent::MeasureMaxAdjacentSegment() const
{
	double Worst = 0.0;
	for (int32 Index = 1; Index < TrailPoints.Items.Num(); ++Index)
	{
		Worst = FMath::Max(Worst,
			FVector::Dist(TrailPoints.Items[Index - 1].Location, TrailPoints.Items[Index].Location));
	}
	return static_cast<float>(Worst);
}

void UTraceTrailComponent::MeasureDrawnSpan(float& OutNearest, float& OutFarthest, int32& OutVisiblePieces) const
{
	OutNearest = -1.f;
	OutFarthest = -1.f;
	OutVisiblePieces = 0;

	const ATraceCharacter* Holder = GetOwnerCharacter();
	if (Holder == nullptr)
	{
		return;
	}

	const FVector PawnLocation = Holder->GetActorLocation();
	double Nearest = TNumericLimits<double>::Max();
	double Farthest = 0.0;

	for (const UStaticMeshComponent* Piece : SmearMeshes)
	{
		if (Piece == nullptr || !Piece->IsVisible())
		{
			continue;
		}

		++OutVisiblePieces;

		const FBoxSphereBounds& LocalBounds = Piece->Bounds;
		const double CentreDistance = FVector::Dist(PawnLocation, LocalBounds.Origin);
		Nearest = FMath::Min(Nearest, FMath::Max(0.0, CentreDistance - LocalBounds.SphereRadius));
		Farthest = FMath::Max(Farthest, CentreDistance);
	}

	if (OutVisiblePieces > 0)
	{
		OutNearest = static_cast<float>(Nearest);
		OutFarthest = static_cast<float>(Farthest);
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
// SPEC v12 §6: keeping the trace — DRAWN AND LETHAL TOGETHER — out of the level
// =================================================================================================

namespace
{
	/**
	 * Is this overlap a piece of LEVEL, i.e. something a ribbon has no business being inside?
	 *
	 * PAWNS ARE NOT WALLS, and that exclusion is doing real work rather than being tidy. Every query
	 * here is centred on a position a character's capsule occupied a moment ago — the carrier's own
	 * capsule is the very first thing an ECC_Visibility overlap at their actor location finds — and
	 * the trace is laid straight through the people playing, constantly, by design.
	 *
	 * Touches are ignored as well: only a BLOCKING response counts. Endzone volumes, pickup shells and
	 * anything else that reports a touch on Visibility is not geometry, and routing the trace around a
	 * trigger volume would be an invisible change to where the trace can go.
	 */
	bool IsWorldObstruction(const FOverlapResult& Overlap)
	{
		if (!Overlap.bBlockingHit || Overlap.GetComponent() == nullptr)
		{
			return false;
		}

		const AActor* OverlapOwner = Overlap.GetActor();
		return OverlapOwner == nullptr || !OverlapOwner->IsA(APawn::StaticClass());
	}

	/**
	 * The clearance the trace asks for, in uu — its own half width plus GWallFitMargin, but NEVER more
	 * than a body needs.
	 *
	 * That ceiling is the difference between a fitter that works and one that thrashes. The carrier's
	 * capsule is ~34uu of radius; if the trace demanded more clearance than that, there would be legal
	 * routes — a corridor a player can walk down — that NO polyline could satisfy, the segment test
	 * would report "obstructed" on every append, and the fitter would spend its whole insert budget
	 * every 60uu achieving nothing.
	 */
	double TrailWallFitClearance(const AActor* Owner)
	{
		const double Radius = FMath::Max(1.0, static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius()));

		double BodyRadius = 34.0;
		if (const ACharacter* OwnerCharacter = Cast<ACharacter>(Owner))
		{
			if (const UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
			{
				BodyRadius = FMath::Max(Radius, static_cast<double>(Capsule->GetScaledCapsuleRadius()));
			}
		}

		const double Requested = Radius + FMath::Max(0.0, static_cast<double>(WallFitMargin()));
		return FMath::Clamp(Requested, Radius, FMath::Max(Radius, BodyRadius - 1.0));
	}

	/**
	 * The trace's own cross-section as a query shape: an upright capsule of the requested clearance,
	 * as tall as the lethal column. A capsule rather than a box because the probe is swept along a
	 * polyline in every direction including pitch, and an axis-aligned box would have to be re-oriented
	 * per segment for no gain — the lateral extent, which is the whole question, is identical.
	 */
	FCollisionShape MakeTrailProbeShape(double Clearance)
	{
		const float ProbeRadius = static_cast<float>(FMath::Max(1.0, Clearance));
		const float ProbeHalfHeight = FMath::Max(
			ProbeRadius + 0.1f,
			static_cast<float>(FMath::Max(1.0, static_cast<double>(UTraceTrailComponent::GetTraceTrailHeight())) * 0.5));

		return FCollisionShape::MakeCapsule(ProbeRadius, ProbeHalfHeight);
	}
}


// -------------------------------------------------------------------------------------------------
// v13 §7: THE RENDERED LEVEL, AS THE FITTER SEES IT
// -------------------------------------------------------------------------------------------------
//
// WHY THIS EXISTS AT ALL — the diagnosis the previous pass was missing.
//
// Everything the player can see in the arena is a NoCollision static mesh. Collision is a separate,
// SMALLER set of invisible UBoxComponents behind those meshes, and on top of that the emissive trim
// each block carries — the top lip (LipOut 12uu), the skirt (10uu), the face bands and the corner
// ribs (13uu) — is drawn OUTSIDE the collision box with no collision of any kind. On the arena's
// walkable structure (the corner-bank terraces and the stepped platforms) there is no pawn standoff
// shell either, deliberately, because a shell there would leave players standing on thin air.
//
// Put those together and the numbers come out exactly as reported. A carrier hugging a terrace has
// their capsule centre 34uu from the COLLISION face, which is 22uu from the visible LIP and, at the
// terrace's convex corner, 17uu from the corner of that lip. The trace draws to ~36uu
// (GetTraceDrawnHalfReach). ~19uu of glowing ribbon inside a glowing rib — while every
// OverlapMultiByChannel in this file, asking the physics scene, correctly answers "clear".
//
// THAT is why raising TrailWallFitMaxPushUU alone would never have fixed this, and why the previous
// A/B moved the number by 1.9uu: the fitter was pushing out of geometry that was not the geometry in
// the way. So the fitter now asks the same question the player's eye asks — of the rendered meshes.
//
// COST. A flat array with precomputed world bounds plus a uniform grid. Built once per world (the
// arena is static), queried a few times per 60uu of carrier travel. Boxes too large to grid — the
// floor, the perimeter walls — go in a small always-checked list rather than smearing across
// thousands of cells.

namespace
{
	/**
	 * The fitter's cached copy of the rendered arena.
	 *
	 * Keyed on the world so a travel, a PIE restart or a map change rebuilds instead of fitting the
	 * new level against the old one's furniture. Never holds a strong reference to anything: the boxes
	 * are flattened transforms, so a world tearing down cannot be kept alive by this.
	 */
	struct FLevelVisualIndex
	{
		static constexpr double CellSize = 512.0;

		/** Beyond this many cells a box goes in Oversized instead. The floor alone is ~1300 cells. */
		static constexpr int32 MaxCellsPerBox = 512;

		TWeakObjectPtr<UWorld> World;
		bool bBuilt = false;

		TArray<UTraceTrailComponent::FTraceClipBox> Boxes;
		TMap<FIntVector, TArray<int32>> Cells;
		TArray<int32> Oversized;

		int32 Skipped = 0;

		static FIntVector CellOf(const FVector& At)
		{
			return FIntVector(
				FMath::FloorToInt(At.X / CellSize),
				FMath::FloorToInt(At.Y / CellSize),
				FMath::FloorToInt(At.Z / CellSize));
		}

		void Reset()
		{
			World = nullptr;
			bBuilt = false;
			Boxes.Reset();
			Cells.Reset();
			Oversized.Reset();
			Skipped = 0;
		}

		void Build(UWorld* InWorld)
		{
			Reset();
			World = InWorld;
			bBuilt = true;

			UTraceTrailComponent::GatherRenderedLevelBoxes(InWorld, Boxes, Skipped);

			for (int32 Index = 0; Index < Boxes.Num(); ++Index)
			{
				const FBox& Bounds = Boxes[Index].WorldBounds;
				const FIntVector Min = CellOf(Bounds.Min);
				const FIntVector Max = CellOf(Bounds.Max);

				const int64 Span = static_cast<int64>(Max.X - Min.X + 1)
					* static_cast<int64>(Max.Y - Min.Y + 1)
					* static_cast<int64>(Max.Z - Min.Z + 1);

				if (Span > MaxCellsPerBox)
				{
					Oversized.Add(Index);
					continue;
				}

				for (int32 X = Min.X; X <= Max.X; ++X)
				{
					for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
					{
						for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
						{
							Cells.FindOrAdd(FIntVector(X, Y, Z)).Add(Index);
						}
					}
				}
			}

			// SAID OUT LOUD, both ways. A silent empty index would turn the whole fix into a no-op
			// that still reports "wall fit on", which is the shape of lie this project keeps catching.
			if (Boxes.Num() == 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("Trace: the wall fitter found NO rendered arena geometry in %s. It will fall back "
					     "to collision-only fitting, which cannot see the emissive trim the trace clips "
					     "into (spec v13 7). Expected ONLY on a test map with no ATraceArenaBuilder and "
					     "no ATraceBakedPiece — on either of those this warning means the index is "
					     "broken, not that the map is bare."),
					*GetNameSafe(InWorld));
			}
			else
			{
				UE_LOG(LogTraceGame, Log,
					TEXT("Trace: wall fitter indexed %d rendered arena boxes (%d degenerate skipped, %d "
					     "oversized, %d grid cells at %.0fuu). Clearance asked for: %.1fuu."),
					Boxes.Num(), Skipped, Oversized.Num(), Cells.Num(), CellSize,
					WallFitRequiredClearance());
			}
		}

		/** Rebuild if this is a different world, or if nothing has been built yet. */
		void EnsureBuilt(UWorld* InWorld)
		{
			if (bBuilt && World.Get() == InWorld)
			{
				return;
			}
			Build(InWorld);
		}

		/** Indices whose world bounds could touch Query. Appends; may contain duplicates by design. */
		void Gather(const FBox& Query, TArray<int32>& Out) const
		{
			Out.Reset();
			Out.Append(Oversized);

			const FIntVector Min = CellOf(Query.Min);
			const FIntVector Max = CellOf(Query.Max);

			for (int32 X = Min.X; X <= Max.X; ++X)
			{
				for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
				{
					for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
					{
						if (const TArray<int32>* Bucket = Cells.Find(FIntVector(X, Y, Z)))
						{
							Out.Append(*Bucket);
						}
					}
				}
			}
		}
	};

	FLevelVisualIndex GLevelVisualIndex;

	/**
	 * How far a vertical capsule at At is inside ONE rendered box, and the shortest HORIZONTAL way
	 * out. Zero when clear.
	 *
	 * The box is treated as the oriented box it really is: its own axes with world half-extents
	 * LocalExtent * |Scale|. That is exact for this arena — every piece is an engine cube or cylinder
	 * bounding box, scaled and yawed — rather than the flattering approximation an axis-aligned test
	 * would give on a yawed corner rib.
	 *
	 * HORIZONTAL ONLY, for the same reason FitPointToWorld has always been: the trace's height is
	 * anchored on the carrier's mid-model, and lifting the ribbon off the body that laid it is a worse
	 * and far more visible lie than the clipping. A contact whose shortest exit is through a top or
	 * bottom face therefore contributes nothing — which is the right answer for a floor, and the
	 * reason a carrier standing on a terrace does not have their trace shoved sideways by the tread
	 * they are standing on.
	 */
	double PenetrationIntoBox(const UTraceTrailComponent::FTraceClipBox& Box, const FVector& At,
		double Radius, double HalfHeight, FVector& OutEscape)
	{
		OutEscape = FVector::ZeroVector;

		const FVector Centre = Box.Transform.TransformPosition(Box.LocalCentre);
		const FVector Axis[3] =
		{
			Box.Transform.GetUnitAxis(EAxis::X),
			Box.Transform.GetUnitAxis(EAxis::Y),
			Box.Transform.GetUnitAxis(EAxis::Z)
		};
		const double Half[3] =
		{
			FMath::Abs(Box.LocalExtent.X * Box.Scale.X),
			FMath::Abs(Box.LocalExtent.Y * Box.Scale.Y),
			FMath::Abs(Box.LocalExtent.Z * Box.Scale.Z)
		};

		// Stations up the capsule's axis rather than one closest-point solve. The lethal column is
		// 63uu tall and the arena is full of things that only occupy part of that — a knee-high lip,
		// a head-height band — so asking at one height would miss them exactly as often as it caught
		// them. Five stations at a 31.5uu half height is a station every ~16uu.
		constexpr int32 StationCount = 5;

		double Worst = 0.0;

		for (int32 Station = 0; Station < StationCount; ++Station)
		{
			const double Alpha = (StationCount > 1)
				? (-1.0 + 2.0 * static_cast<double>(Station) / static_cast<double>(StationCount - 1))
				: 0.0;
			const FVector P = At + FVector(0.0, 0.0, Alpha * HalfHeight);

			const FVector Delta = P - Centre;
			const double Local[3] =
			{
				FVector::DotProduct(Delta, Axis[0]),
				FVector::DotProduct(Delta, Axis[1]),
				FVector::DotProduct(Delta, Axis[2])
			};

			bool bInside = true;
			for (int32 A = 0; A < 3; ++A)
			{
				if (FMath::Abs(Local[A]) > Half[A])
				{
					bInside = false;
					break;
				}
			}

			double Depth = 0.0;
			FVector Direction = FVector::ZeroVector;

			if (bInside)
			{
				// The centreline is inside the box: get the whole radius out through the shallowest
				// HORIZONTAL face. Vertical is not a candidate — see the header comment — so a wide
				// flat tread the column is buried in still pushes sideways rather than not at all.
				double Shallowest = TNumericLimits<double>::Max();
				for (int32 A = 0; A < 3; ++A)
				{
					if (FMath::Abs(Axis[A].Z) > 0.7)
					{
						continue;   // this axis is the box's vertical one
					}
					const double Out = Half[A] - FMath::Abs(Local[A]);
					if (Out < Shallowest)
					{
						Shallowest = Out;
						Direction = Axis[A] * ((Local[A] >= 0.0) ? 1.0 : -1.0);
					}
				}

				if (Shallowest == TNumericLimits<double>::Max())
				{
					continue;   // a box with no horizontal axis; nothing honest to do
				}

				Depth = Radius + Shallowest;
			}
			else
			{
				// Outside: the closest point on the box, and whether the capsule's radius reaches it.
				FVector Closest = Centre;
				for (int32 A = 0; A < 3; ++A)
				{
					Closest += Axis[A] * FMath::Clamp(Local[A], -Half[A], Half[A]);
				}

				FVector Away = P - Closest;
				Away.Z = 0.0;   // horizontal separation only; a tread underfoot is not an obstruction
				const double Distance = Away.Size();
				if (Distance >= Radius)
				{
					continue;
				}

				Depth = Radius - Distance;
				Direction = (Distance > UE_DOUBLE_KINDA_SMALL_NUMBER)
					? (Away / Distance)
					: FVector::ZeroVector;
			}

			Direction.Z = 0.0;
			if (!Direction.Normalize() || Depth <= 0.0)
			{
				continue;
			}

			if (Depth > Worst)
			{
				Worst = Depth;
				OutEscape = Direction * Depth;
			}
		}

		return Worst;
	}
}

void UTraceTrailComponent::GatherRenderedLevelBoxes(UWorld* World, TArray<FTraceClipBox>& Out,
	int32& OutSkipped)
{
	Out.Reset();
	OutSkipped = 0;

	if (World == nullptr)
	{
		return;
	}

	// A zero-scaled or zero-thickness instance is not geometry — an instance pool parks its unused
	// slots by scaling them away — and it poisons every measurement it is in: a point test lands
	// exactly ON its collapsed face and reports a distance of -0.0, which is then the minimum for
	// every sample in the level.
	const auto Add = [&Out, &OutSkipped](const FTransform& Transform, const FBox& LocalBox,
		const FString& Name)
	{
		const FVector Scale = Transform.GetScale3D();
		const FVector Extent = LocalBox.GetExtent();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (FMath::Abs(Scale[Axis]) < 0.01 || Extent[Axis] < 0.01)
			{
				++OutSkipped;
				return;
			}
		}

		FTraceClipBox& Box = Out.AddDefaulted_GetRef();
		Box.Transform = Transform;
		Box.LocalCentre = LocalBox.GetCenter();
		Box.LocalExtent = Extent;
		Box.Scale = Scale;
		Box.Name = Name;

		Box.WorldBounds = FBox(ForceInit);
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Local = Box.LocalCentre + FVector(
				(Corner & 1) ? Box.LocalExtent.X : -Box.LocalExtent.X,
				(Corner & 2) ? Box.LocalExtent.Y : -Box.LocalExtent.Y,
				(Corner & 4) ? Box.LocalExtent.Z : -Box.LocalExtent.Z);
			Box.WorldBounds += Transform.TransformPosition(Local);
		}
	};

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;

		// BakedPiece as well as ArenaBuilder, and this is a real regression that was measured, not a
		// precaution: on /Game/Maps/Arena_Baked every mesh lives on an ATraceBakedPiece rather than
		// on the builder, so an ArenaBuilder-only filter indexed ZERO boxes there against 1269 on
		// /Game/Maps/Arena. The wall fitter then fell back to collision-only fitting, which cannot
		// see the emissive trim the trace clips into (spec v13 §7) — the fix silently degraded on
		// the map the whole bake exists to produce, while still reporting itself healthy.
		const bool bIsArenaGeometry = Actor != nullptr
			&& (Actor->GetClass()->GetName().Contains(TEXT("ArenaBuilder"))
				|| Actor->GetClass()->GetName().Contains(TEXT("BakedPiece")));
		if (!bIsArenaGeometry)
		{
			continue;
		}

		TArray<UStaticMeshComponent*> Meshes;
		Actor->GetComponents<UStaticMeshComponent>(Meshes);
		for (UStaticMeshComponent* Mesh : Meshes)
		{
			if (Mesh == nullptr || !Mesh->IsVisible() || Mesh->GetStaticMesh() == nullptr)
			{
				continue;
			}

			const FBox LocalBox = Mesh->GetStaticMesh()->GetBoundingBox();

			// ISM BEFORE StaticMesh: UInstancedStaticMeshComponent DERIVES from UStaticMeshComponent,
			// and taking the base branch would collapse a pool of hundreds of walls into one box at
			// the arena's origin — which is how an earlier arm of this measurement came back clean
			// against a level it had never actually tested.
			if (UInstancedStaticMeshComponent* Pool = Cast<UInstancedStaticMeshComponent>(Mesh))
			{
				const int32 InstanceCount = Pool->GetInstanceCount();
				for (int32 Instance = 0; Instance < InstanceCount; ++Instance)
				{
					FTransform InstanceTransform;
					if (Pool->GetInstanceTransform(Instance, InstanceTransform, /*bWorldSpace=*/true))
					{
						Add(InstanceTransform, LocalBox,
							FString::Printf(TEXT("%s[%d]"), *Pool->GetName(), Instance));
					}
				}
				continue;
			}

			Add(Mesh->GetComponentTransform(), LocalBox, Mesh->GetName());
		}
	}
}

void UTraceTrailComponent::InvalidateLevelVisualIndex()
{
	GLevelVisualIndex.Reset();
}

void UTraceTrailComponent::GetLevelVisualIndexStats(int32& OutBoxes, int32& OutCells, bool& OutBuilt)
{
	OutBoxes = GLevelVisualIndex.Boxes.Num();
	OutCells = GLevelVisualIndex.Cells.Num();
	OutBuilt = GLevelVisualIndex.bBuilt;
}

double UTraceTrailComponent::MeasureVisualPenetration(const FVector& At, double Radius,
	double HalfHeight, FVector& OutPush) const
{
	OutPush = FVector::ZeroVector;

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0.0;
	}

	GLevelVisualIndex.EnsureBuilt(World);
	if (GLevelVisualIndex.Boxes.Num() == 0)
	{
		return 0.0;
	}

	const FVector Extent(Radius, Radius, HalfHeight);
	const FBox Query(At - Extent, At + Extent);

	TArray<int32> Candidates;
	GLevelVisualIndex.Gather(Query, Candidates);

	// DE-DUPLICATED FIRST, and it is not tidiness. A box that spans several grid cells is returned
	// once per cell the query touches, and the escape vectors below are SUMMED — so without this a
	// wall would push the trace two or three times as hard as it should purely because of how it
	// happened to land on the grid, which is a push that depends on nothing physical.
	Candidates.Sort();

	double Deepest = 0.0;
	int32 Previous = INDEX_NONE;
	for (const int32 Index : Candidates)
	{
		if (Index == Previous)
		{
			continue;
		}
		Previous = Index;

		const FTraceClipBox& Box = GLevelVisualIndex.Boxes[Index];
		if (!Box.WorldBounds.Intersect(Query))
		{
			continue;
		}

		FVector Escape = FVector::ZeroVector;
		const double Depth = PenetrationIntoBox(Box, At, Radius, HalfHeight, Escape);
		if (Depth > 0.0)
		{
			Deepest = FMath::Max(Deepest, Depth);

			// SUMMED, not "deepest wins". A concave corner has two faces, and honouring only the
			// deeper one slides the point along the other instead of out of both.
			OutPush += Escape;
		}
	}

	return Deepest;
}

double UTraceTrailComponent::MeasureCollisionPenetration(const FVector& At,
	const FCollisionShape& Probe, FVector& OutPush) const
{
	OutPush = FVector::ZeroVector;

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0.0;
	}

	FCollisionQueryParams Params(FName(TEXT("TraceTrailWallPush")), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(GetOwner());

	TArray<FOverlapResult> Overlaps;
	if (!World->OverlapMultiByChannel(Overlaps, At, FQuat::Identity, ECC_Visibility, Probe, Params))
	{
		return 0.0;
	}

	double Deepest = 0.0;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		if (!IsWorldObstruction(Overlap))
		{
			continue;
		}

		FMTDResult MTD;
		if (Overlap.GetComponent()->ComputePenetration(MTD, Probe, At, FQuat::Identity))
		{
			Deepest = FMath::Max(Deepest, static_cast<double>(MTD.Distance));

			// HORIZONTAL ONLY. The trace's height is anchored on the carrier's mid-model, and a
			// vertical nudge would lift the ribbon off the body that laid it — which is a worse and
			// more visible lie than the clipping being fixed. A floor or ceiling contact therefore
			// contributes nothing, which is the correct answer for both.
			FVector Direction = MTD.Direction;
			Direction.Z = 0.0;
			if (Direction.Normalize())
			{
				OutPush += Direction * static_cast<double>(MTD.Distance);
			}
		}
	}

	return Deepest;
}

bool UTraceTrailComponent::IsTrailVolumeClear(const FVector& From, const FVector& To) const
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return true;
	}

	const double Clearance = TrailWallFitClearance(GetOwner());
	const FCollisionShape Probe = MakeTrailProbeShape(Clearance);

	FCollisionQueryParams Params(FName(TEXT("TraceTrailWallFit")), /*bTraceComplex=*/false);
	Params.AddIgnoredActor(GetOwner());

	// Stepped rather than swept. A swept multi-overlap stops at the first BLOCK, and on
	// ECC_Visibility the first block between two points a body walked between is quite often another
	// player — which would hide the wall behind them and report a segment clear that is not. Stepping
	// asks the same question at each station and filters the answers, so nothing can mask anything.
	// The step is a fraction of the probe's own radius, so no gap can slip between two stations.
	const double Distance = FVector::Dist(From, To);
	const int32 Steps = FMath::Clamp(
		FMath::CeilToInt(Distance / FMath::Max(4.0, Clearance * 0.5)), 1, 64);

	TArray<FOverlapResult> Overlaps;
	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const FVector At = FMath::Lerp(From, To, static_cast<double>(Step) / static_cast<double>(Steps));

		Overlaps.Reset();
		if (!World->OverlapMultiByChannel(Overlaps, At, FQuat::Identity, ECC_Visibility, Probe, Params))
		{
			continue;
		}

		for (const FOverlapResult& Overlap : Overlaps)
		{
			if (IsWorldObstruction(Overlap))
			{
				return false;
			}
		}
	}

	return true;
}

FVector UTraceTrailComponent::FitPointToWorld(const FVector& Candidate) const
{
	// v13 §7's ARM SWITCH, and it is checked here rather than at either call site deliberately: this
	// function IS the half-width clearance. Returning the candidate untouched leaves the blocked-chord
	// router in AppendTrailPointsFitted running exactly as it did in v12, which is precisely what
	// bTrailWallClearanceEnabled=False is documented to mean. Gating the CALLERS instead would also
	// have disabled the fit on PendingPathSamples, i.e. two arms in one switch.
	if (!WallClearanceEnabled())
	{
		return Candidate;
	}

	const double MaxPush = FMath::Max(0.0, static_cast<double>(WallFitMaxPush()));
	UWorld* World = GetWorld();
	if (MaxPush <= 0.0 || World == nullptr)
	{
		return Candidate;
	}

	// v13 §7: THE CLEARANCE IS THE TRACE'S WHOLE DRAWN REACH, NOT ITS LETHAL HALF WIDTH.
	//
	// The previous version deliberately probed at TrailRadius here, reasoning that "the push resolves
	// a real penetration, so it should not pad". That reasoning was sound and the number was wrong:
	// TrailRadius is not what the trace occupies. A ribbon element is a box that overhangs its joint
	// by another TrailRadius along its own axis, and the whole ribbon is a Catmull-Rom resample that
	// bulges outside the polyline at every turn — GetTraceDrawnHalfReach() is the real figure, ~36uu
	// against the 22.5 that was being cleared. Clearing 22.5 and calling it done is why 14uu of glow
	// stayed inside the corner.
	const double Clearance = WallFitRequiredClearance();
	const double HalfHeight = FMath::Max(1.0, static_cast<double>(GetTraceTrailHeight()) * 0.5);
	const FCollisionShape Probe = MakeTrailProbeShape(Clearance);

	// BOTH ARENAS AT ONCE, and they are genuinely different sets of surfaces.
	//
	//   COLLISION  the physics scene. Catches anything not built by the arena builder — a hand-placed
	//              mesh, a test map, a future level — and is the only thing available if the rendered
	//              index is empty.
	//   RENDERED   the meshes on screen. Catches the emissive trim, which has NO collision at all and
	//              is the geometry actually being clipped into (see FLevelVisualIndex above).
	//
	// Neither subsumes the other, so the push is the sum and the depth is the worst of the two.
	const auto Measure = [&](const FVector& Where, FVector& OutPush) -> double
	{
		FVector CollisionPush = FVector::ZeroVector;
		const double CollisionDepth = MeasureCollisionPenetration(Where, Probe, CollisionPush);

		FVector VisualPush = FVector::ZeroVector;
		const double VisualDepth = WallFitUsesRenderedGeometry()
			? MeasureVisualPenetration(Where, Clearance, HalfHeight, VisualPush)
			: 0.0;

		OutPush = CollisionPush + VisualPush;
		return FMath::Max(CollisionDepth, VisualDepth);
	};

	FVector FirstPush = FVector::ZeroVector;
	const double Before = Measure(Candidate, FirstPush);
	if (Before <= 0.0 || FirstPush.IsNearlyZero())
	{
		return Candidate;
	}

	// RELAXATION, because one escape vector resolves one face. The arena stacks its surfaces — a
	// block, its lip 12uu proud of that, its skirt below, a rib on the corner — so a single hop lands
	// the point in the next one about as often as it lands it in the open. Each pass re-measures from
	// where the last one left off; four is enough for every stack this arena builds and is bounded
	// work whatever a future level does.
	//
	// THE GUARD IS UNCHANGED AND IT IS WHY THERE IS NO LINE-OF-SIGHT TEST. Every escape vector is by
	// construction the SHORTEST way out of a surface, so it exits by the nearest face and cannot
	// carry the point through a wall to the far side. What it can do is fail, so the only thing
	// accepted is a position measurably LESS inside the level than the one the carrier stood at — and
	// the total displacement from that original position is what MaxPush caps, not each step, so four
	// passes cannot walk the trace away from the player's route.
	constexpr int32 RelaxPasses = 4;

	FVector Best = Candidate;
	double BestDepth = Before;
	FVector At = Candidate;

	for (int32 Pass = 0; Pass < RelaxPasses; ++Pass)
	{
		FVector StepPush = FVector::ZeroVector;
		const double Depth = (Pass == 0) ? Before : Measure(At, StepPush);
		if (Pass == 0)
		{
			StepPush = FirstPush;
		}

		if (Depth <= 0.0 || StepPush.IsNearlyZero())
		{
			break;
		}

		FVector Total = (At + StepPush) - Candidate;
		Total.Z = 0.0;
		At = Candidate + Total.GetClampedToMaxSize(MaxPush);

		FVector Ignored = FVector::ZeroVector;
		const double MovedDepth = Measure(At, Ignored);
		if (MovedDepth < BestDepth)
		{
			BestDepth = MovedDepth;
			Best = At;
		}
		if (MovedDepth <= 0.0)
		{
			break;
		}
	}

	if (BestDepth < Before)
	{
		GWallFitPushes += (Best.Equals(Candidate) ? 0 : 1);
		GWallFitWorstPush = FMath::Max(GWallFitWorstPush, FVector::Dist(Candidate, Best));
		GWallFitWorstResidual = FMath::Max(GWallFitWorstResidual, BestDepth);
		return Best;
	}

	// Nothing helped. Leave the point exactly where the carrier stood — the subdivision below is then
	// no worse than what shipped before — and count it, because the honest number for "how often can
	// this not be fixed" is one this harness has to be able to print.
	++GWallFitUnpushable;
	GWallFitWorstResidual = FMath::Max(GWallFitWorstResidual, Before);
	return Candidate;
}

int32 UTraceTrailComponent::AppendTrailPointsFitted(const FVector& Target, float Now)
{
	const auto AppendOne = [this, Now](const FVector& Where)
	{
		FTraceTrailPoint& NewPoint = TrailPoints.Items.AddDefaulted_GetRef();
		NewPoint.Location = Where;
		NewPoint.BirthServerTime = Now;

		// Adding or changing an item is signalled per item; only removals need MarkArrayDirty.
		TrailPoints.MarkItemDirty(NewPoint);
	};

	if (!WallFitEnabled())
	{
		// THE BEFORE ARM. Exactly the pre-v12 behaviour, to the instruction: one point, laid at the
		// carrier's actor location, joined to the previous one by a straight chord however much level
		// that chord passes through. This is what Trace.Trail.WallClip reproduces the bug with.
		AppendOne(Target);
		return 1;
	}

	const FVector FittedTarget = FitPointToWorld(Target);

	// Nothing to cut a corner across yet.
	if (TrailPoints.Items.Num() == 0)
	{
		AppendOne(FittedTarget);
		return 1;
	}

	FVector Anchor(TrailPoints.Items.Last().Location);
	if (IsTrailVolumeClear(Anchor, FittedTarget))
	{
		// The overwhelmingly common case, and it costs one stepped overlap query per 60uu of travel
		// per carrier. Nothing is inserted, so a straight run lays exactly the points it always did.
		AppendOne(FittedTarget);
		return 1;
	}

	// The chord is inside the level. Walk the carrier's REAL route instead — the positions their
	// capsule actually occupied — taking the LARGEST step that stays clear each time. Greedy on
	// purpose: it inserts the fewest points that remove the shortcut, so a gentle curve adds nothing
	// and only the tight corner is subdivided.
	++GWallFitRoutedAppends;

	int32 Appended = 0;
	const int32 Budget = FMath::Clamp(WallFitMaxInsert(), 0, 32);
	int32 Cursor = 0;

	while (Appended < Budget)
	{
		int32 Best = INDEX_NONE;
		FVector BestLocation = FVector::ZeroVector;

		for (int32 Index = PendingPathSamples.Num() - 1; Index >= Cursor; --Index)
		{
			const FVector Sample = FitPointToWorld(PendingPathSamples[Index]);
			if (FVector::DistSquared(Sample, Anchor) < 1.0)
			{
				continue;
			}
			if (IsTrailVolumeClear(Anchor, Sample))
			{
				Best = Index;
				BestLocation = Sample;
				break;
			}
		}

		if (Best == INDEX_NONE)
		{
			// Not even the next recorded position can be reached through open space. Either the
			// carrier is standing inside something (the arena has scenery that blocks sight but not
			// pawns) or there is no route at this clearance. Stop; the chord below is then no worse
			// than what shipped before.
			break;
		}

		AppendOne(BestLocation);
		++Appended;
		++GWallFitInsertedPoints;

		Anchor = BestLocation;
		Cursor = Best + 1;

		if (IsTrailVolumeClear(Anchor, FittedTarget))
		{
			break;
		}
	}

	// The head always lands where the carrier is, whatever happened above: the newest point is the
	// emitter's own footprint and the trace has to stay attached to the body that is laying it.
	AppendOne(FittedTarget);
	++Appended;

	if (!IsTrailVolumeClear(Anchor, FittedTarget))
	{
		++GWallFitUnroutable;
		UE_LOG(LogTraceGame, Verbose,
			TEXT("Trace: could not fit the last %.0fuu of %s's trace outside the level (%d samples, "
			     "%d inserted). Falling back to the straight chord."),
			FVector::Dist(Anchor, FittedTarget), *GetNameSafe(GetOwner()),
			PendingPathSamples.Num(), Appended - 1);
	}

	return Appended;
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
	//    THE TRANSFER GRACE LIVES RIGHT HERE, AND ONLY HERE (§2; 0.75s since v10 §3). For its duration
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

		// SPEC v12 §6. REMEMBER WHERE THE CARRIER ACTUALLY WENT, every tick, between appends.
		//
		// Recorded before the append test so the route leading up to a point is complete by the time
		// that point is laid. These are raw actor locations — positions a capsule legally occupied —
		// which is exactly what makes them safe to hand to the fitter: a body that fitted there leaves
		// room for a trace 11uu narrower. Bounded, and thrown away on every append, so this is a
		// handful of entries and never a history.
		if (PendingPathSamples.Num() == 0
			|| FVector::DistSquared(PendingPathSamples.Last(), Location) >= 1.0)
		{
			PendingPathSamples.Add(Location);
		}
		if (PendingPathSamples.Num() > MaxPendingPathSamples)
		{
			PendingPathSamples.RemoveAt(0, PendingPathSamples.Num() - MaxPendingPathSamples,
				EAllowShrinking::No);
		}

		if (TrailPoints.Items.Num() == 0 || DistanceFromHead >= Spacing)
		{
			// SPEC v12 §6. One call, and it appends between one and a few points: the target plus
			// whatever subdivision it took to stop the polyline passing through the level. Everything
			// downstream reads TrailPoints and is unchanged, which is how the drawn volume and the
			// lethal volume stay the same object rather than two objects that agree.
			AppendTrailPointsFitted(Location, Now);
			PendingPathSamples.Reset();
			bChanged = true;
		}
	}
	else
	{
		// Not laying trace: a route recorded now would be a route the trace never took, and holding
		// it would let the first point after a grace or a turnover be subdivided along a path that
		// belongs to a different possession.
		PendingPathSamples.Reset();
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
		// v8 §3. A held trip must never leak out through an early return — that is precisely the
		// "stopped asking" failure TraceParry's dead-man switch disarms the whole mechanic over.
		//
		// The two cases are genuinely different, so they are answered differently rather than both
		// being swept under a null assignment:
		//
		//   carrier dead / gone  -> the kill is MOOT. Withdraw it; nobody dies twice.
		//   trail merely EMPTY   -> the kill was EARNED, at PendingTripServerTime, against segments
		//                           that were lethal and drawn then. A turnover or a pass clearing the
		//                           ribbon a frame or two later must not refund the dasher's kill, so
		//                           keep resolving it to its proper verdict.
		if (Holder == nullptr || !Holder->IsAlive())
		{
			ServerCancelPendingTrip();
		}
		else if (PendingTripDasher.IsValid())
		{
			ServerAdvancePendingTrip(Holder, DeltaTime);
		}

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
	// v3 §3, THE PARRY. 0.2s (v8 §3), carrier-only, on a cooldown, shield untouched. Signposted by the
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

	// v8 §3: the sweep resolves at a known instant; that instant is what the window has to cover.
	// ServerWasParryOpenAt falls back to IsParryActive() for any pawn with no stamped press on file
	// (bots, the host), so those answers are unchanged.
	TraceParry::NotifyHeldTripsWired();
	const float TripServerTime = GetServerTimeSeconds();
	const bool bParry = TraceParry::ServerWasParryOpenAt(GetOwner(), TripServerTime);
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

#if !UE_BUILD_SHIPPING
	// SPEC v10 §2's harness, and the only line of it that is not in the debug block at the bottom of
	// this file. It runs here — inside the test, before the loop, after the lethal set is known —
	// because a console ticker places a DASHING pawn a whole frame of dash movement too early. It is
	// inert unless Trace.Trail.ModelHitTest has armed it, and it changes nothing but one pawn's
	// position and the remembered previous position the sweep is built from.
	ApplyStagedModelProbe(this, Holder, TestPositions);
#endif

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

		// SPEC v10 §2. THE TRIPPER IS THE MODEL, NOT THE CAPSULE.
		//
		// This one line is the whole of the reported fix. It used to read the capsule and nothing
		// else, and a Mannequin's arms and trailing leg live outside its 34uu capsule in every pose
		// a dash is ever seen in — so a limb that visibly clipped a 22.5uu-wide trace scored
		// nothing. MeasureModelReach() answers with the rendered mesh's own extent, measured from
		// the actor location (the frame the trace itself is laid in) and clamped at both ends. See
		// the header for why the bounds and not a constant, and for the bounded over-reach.
		const FTraceModelReach Reach = MeasureModelReach(Candidate);

		// The trace is a vertical volume of radius TrailRadius and height TrailHeight swept along
		// the holder's path, and the tripper is their MODEL swept along its path this tick. Test
		// those two sweeps as: horizontal segment-to-segment distance (which catches tunnelling
		// at dash speed, unlike a point test), plus a separate vertical overlap check so that
		// clearing the trace in the air is not a hit.
		// v14 §1: the narrow phase no longer takes a pre-summed threshold — it takes the trace's own
		// half width and the tripper's reach separately, because a summed threshold cannot express
		// where the trace's own surface is and therefore cannot have a flat cap. This sum survives for
		// the BROAD phase (where a conservative radius is all that is wanted) and for the log line
		// that quotes the widening, and it is still the same number the narrow phase effectively uses
		// away from the two outer caps.
		const double HorizontalThreshold = TrailRadius + Reach.EffectiveRadius;

		// The pre-v10 threshold, kept alive purely to be counted against. See GModelTripsWidened.
		const double CapsuleHorizontalThreshold = TrailRadius + Reach.CapsuleRadius;

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
			&& SweepIntersectsTrace(TestPositions, PreviousLocation, CurrentLocation,
				TrailRadius, TrailHalfHeight, Reach.EffectiveRadius, Reach.EffectiveHalfHeight);

		if (bHitLethal)
		{
			// THE KNOCK-ON, MEASURED RATHER THAN ESTIMATED (spec v10 §2's last paragraph).
			//
			// Re-run the SAME sweep at the OLD thresholds and record whether this trip is one the
			// pre-v10 test would also have scored. Evaluated only on the frames a trip actually
			// lands — a handful per match — so the second sweep costs nothing measurable, and it is
			// the only way to say "the widening created N of the M trace kills in this match"
			// without inferring it from a log. Counted for kills AND for parry punishes, because
			// they are the same detection and the spec asks about both.
			const bool bCapsuleWouldHit = SweepIntersectsTrace(
				TestPositions, PreviousLocation, CurrentLocation,
				TrailRadius, TrailHalfHeight, Reach.CapsuleRadius, Reach.CapsuleHalfHeight);

			++GModelTripsTotal;

#if !UE_BUILD_SHIPPING
			// Written down here, at the only place the answer exists, so Trace.Trail.ModelHitTest
			// asserts about ITS dasher rather than about a counter every bot in the match also moves.
			++GModelTripSerial;
			GModelTripLastDasher = Candidate;
			GModelTripLastHolder = Holder;
			GModelTripLastWidened = !bCapsuleWouldHit;
			GModelTripLastMargin = Reach.HorizontalMargin();
			GModelTripLastThreshold = HorizontalThreshold;
			GModelTripLastCapsuleThreshold = CapsuleHorizontalThreshold;
#endif

			if (!bCapsuleWouldHit)
			{
				++GModelTripsWidened;

				// Log level, not Verbose. Every hit that exists only because of this change says so
				// by name, with the numbers that made it — so "that felt like a phantom hit" is a
				// grep and not an argument, and so the over-reach can be audited from a real match
				// instead of from this file's claims about it.
				UE_LOG(LogTraceGame, Log,
					TEXT("[TRACEMODEL] %s's dash scored on %s's trace via WHOLE-MODEL reach only: "
					     "capsule %.1fuu -> model %.1fuu (margin %+.1fuu, mesh measured=%d), threshold "
					     "%.1f -> %.1fuu. The pre-v10 test would have missed this."),
					*GetNameSafe(Candidate), *GetNameSafe(Holder),
					Reach.CapsuleRadius, Reach.EffectiveRadius, Reach.HorizontalMargin(),
					Reach.bMeshMeasured ? 1 : 0, CapsuleHorizontalThreshold, HorizontalThreshold);
			}

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
			&& SweepIntersectsTrace(ExemptPositions, PreviousLocation, CurrentLocation,
				TrailRadius, TrailHalfHeight, Reach.EffectiveRadius, Reach.EffectiveHalfHeight))
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[TRACEDASH] %s dashed through the NON-DRAWN head stub of %s's trace: NO KILL (emitter footprint, %.0fuu). points=%d lethal=%d"),
				*GetNameSafe(Candidate), *GetNameSafe(Holder),
				static_cast<double>(GetTraceTrailRadius()), TrailPoints.Items.Num(), TestPositions.Num());
		}
	}

	// The dasher (if any) that the HELD path punished for a parry this frame. See the guard on the
	// ParriedDasher block below: both paths call ServerPunishParriedDash, and that function carries
	// the Demo 7 refunds, so the same dasher must not go through it twice in one frame.
	ATraceCharacter* HeldPathPunished = nullptr;

	// Applied outside every loop above: this kills, which re-enters the component and mutates
	// TrailPoints.Items and PreviousLocations.
	if (Tripper != nullptr)
	{
		// parry=0 passWindow=0 is printed on the KILL line too, and it is not noise: the claim under
		// test is a conditional, so the log has to carry the negative case as explicitly as the
		// positive one. "Every dash through a trace, and whether a parry was active" is one grep.
		// v8 §3. A REMOTE carrier's parry may still be in the air; hold the kill for their own
		// upstream lag and re-ask each frame. GetTripHoldSeconds() is 0 for the host and for bots,
		// so single-machine play is untouched. The dasher pays a few tens of ms of feedback delay;
		// they lose nothing else.
		if (PendingTripDasher.Get() != Tripper)
		{
			PendingTripDasher = Tripper;
			PendingTripServerTime = TripServerTime;
			PendingTripHeldSeconds = 0.f;

			// Resolve the brand-new trip on its own contact frame with zero elapsed hold, exactly as
			// before: a host or a bot carrier holds for 0s and dies on this very line.
			ServerAdvancePendingTrip(Holder, 0.f, &HeldPathPunished);
		}
		else
		{
			ServerAdvancePendingTrip(Holder, DeltaTime, &HeldPathPunished);
		}
	}
	else if (PendingTripDasher.IsValid())
	{
		// v8 §3, THE OTHER HALF OF THE HOLD — and the single line that decides whether any of this
		// works. A dash crosses a ribbon in one or two frames; a remote carrier's hold lasts three or
		// four. The previous code cleared PendingTripDasher here, on the first frame the dasher was no
		// longer intersecting, so the hold was DROPPED rather than resolved: the earned kill never
		// landed, TraceParry's dead-man switch counted it abandoned, and the first lethal dash against
		// a joined carrier disarmed held trips (and with them the 58ms press anchoring) for the whole
		// session. Measured before this change: case B left the carrier ALIVE 3/3 instead of DEAD 4/4
		// — remote carriers were effectively unkillable by trace dashes, a worse client-only bug than
		// the one v8 §3 set out to fix.
		//
		// Contact is NOT re-required. The trip already resolved, at PendingTripServerTime, against a
		// segment that was lethal and drawn at that instant; the only open question is whether a press
		// covering that instant shows up before the hold expires. Nothing here can invent a kill the
		// sweep did not already earn.
		ServerAdvancePendingTrip(Holder, DeltaTime, &HeldPathPunished);
	}

	// v6 §3, deferred for the same re-entrancy reason as ApplyTrailTrip above: this kills, and the
	// death path re-enters this component.
	//
	// v8 integration: NOT if the held path already punished this same dasher this frame. Both routes
	// end at ServerPunishParriedDash, which applies the Demo 7 refunds (parry cooldown to zero, one
	// dash charge back) and kills the dasher — running it twice would refund twice and log a second
	// kill against a corpse. The two routes overlap for exactly one frame whenever a held trip is
	// resolved by a press that ALSO makes the live window open on the frame the dasher is still
	// inside the ribbon, which is the common case rather than a rare one.
	if (ParriedDasher != nullptr && ParriedDasher != HeldPathPunished)
	{
		TraceParry::ServerPunishParriedDash(Holder, ParriedDasher, TripServerTime);
	}
}

bool UTraceTrailComponent::SweepIntersectsTrace(const TArray<FVector>& Positions, const FVector& PreviousLocation,
	const FVector& CurrentLocation, double TraceRadius, double TraceHalfHeight,
	double TripperRadius, double TripperHalfHeight) const
{
	if (Positions.Num() == 0)
	{
		return false;
	}

	// A single point is tested as a zero-length segment rather than skipped. The old code needed
	// two testable points before ANYTHING was lethal, which — stacked on the head exemption — meant
	// a freshly formed trace was drawn but harmless for its first few points. SegmentDistToSegment
	// handles a degenerate segment correctly, so there is no reason for that hole to exist.
	const int32 LastSegment = FMath::Max(0, Positions.Num() - 2);

	// SPEC v14 §1. The two OUTER caps are flat, so the volume that kills stops exactly where the
	// ribbon stops being drawn. Interior joints keep their disc — the drawing's own joint overlap
	// already contains it. See GTrailFlatEndCaps for why this, and not extending the drawing.
	const bool bFlatCaps = (GTrailFlatEndCaps != 0);

	for (int32 SegmentIndex = 0; SegmentIndex <= LastSegment; ++SegmentIndex)
	{
		const FVector& TrailStart = Positions[SegmentIndex];
		const FVector& TrailEnd = Positions[FMath::Min(SegmentIndex + 1, Positions.Num() - 1)];

		// ONE definition of the solid, shared with both halves of every measurement — see
		// SegmentGapToTraceVolume. The tripper's own reach is applied HERE, to the gap, rather than
		// being folded into a threshold, which is what lets the trace's own surface be flat.
		double HorizontalGap = 0.0;
		double VerticalGap = 0.0;
		bool bBeyondCap = false;
		SegmentGapToTraceVolume(TrailStart, TrailEnd, bFlatCaps, bFlatCaps,
			PreviousLocation, CurrentLocation, TraceRadius, TraceHalfHeight,
			HorizontalGap, VerticalGap, bBeyondCap);

		if (HorizontalGap <= TripperRadius && VerticalGap <= TripperHalfHeight)
		{
			return true;
		}
	}

	return false;
}

void UTraceTrailComponent::ServerCancelPendingTrip()
{
	if (!PendingTripDasher.IsValid() && PendingTripHeldSeconds <= 0.f)
	{
		return;
	}

	// Tell TraceParry the hold was WITHDRAWN, not forgotten. Without this the dead-man switch would
	// read a legitimately moot kill (the carrier died of a bullet mid-hold) as the caller dropping a
	// trip, and disarm held trips for the session over a non-bug.
	if (ATraceCharacter* Holder = GetOwnerCharacter())
	{
		TraceParry::ServerCancelHeldTrip(Holder, PendingTripServerTime);
	}

	PendingTripDasher = nullptr;
	PendingTripHeldSeconds = 0.f;
}

bool UTraceTrailComponent::ServerAdvancePendingTrip(ATraceCharacter* Holder, float DeltaTime, ATraceCharacter** OutPunished)
{
	if (OutPunished != nullptr)
	{
		*OutPunished = nullptr;
	}

	ATraceCharacter* Dasher = PendingTripDasher.Get();

	// The dasher stopped existing (destroyed, or killed by something else) while we held their trip.
	// Moot, and withdrawn rather than abandoned — see ServerCancelPendingTrip.
	if (Holder == nullptr || Dasher == nullptr)
	{
		ServerCancelPendingTrip();
		return false;
	}

	PendingTripHeldSeconds += DeltaTime;

	switch (TraceParry::ServerResolveHeldTrip(Holder, PendingTripServerTime, PendingTripHeldSeconds))
	{
	case TraceParry::EHeldTrip::KeepHolding:
		// Say nothing yet: the kill has not happened and may never happen. The contract is that we
		// come back next frame, and the "else if (PendingTripDasher.IsValid())" branch in
		// ServerRunTripTest is what guarantees we do even after contact ends.
		return true;

	case TraceParry::EHeldTrip::Parried:
		UE_LOG(LogTraceGame, Log,
			TEXT("[TRACEDASH] %s dashed through %s's trace: PARRIED (held %.0fms for the carrier's press)."),
			*GetNameSafe(Dasher), *GetNameSafe(Holder), PendingTripHeldSeconds * 1000.f);
		PendingTripDasher = nullptr;
		PendingTripHeldSeconds = 0.f;
		TraceParry::ServerPunishParriedDash(Holder, Dasher, PendingTripServerTime);
		if (OutPunished != nullptr)
		{
			*OutPunished = Dasher;
		}
		return false;

	case TraceParry::EHeldTrip::KillNow:
		// ------------------------------------------------------------------------------------------
		// SPEC v9 §3 — THE LAST WAY A CLEARED TRACE COULD STILL KILL, AND IT IS NOT THE POINT ARRAY.
		//
		// A trip is not resolved on the frame the dasher crosses: TraceParry HOLDS it for the parry
		// window so the carrier gets their 0.2s to answer. Arbitrary frames therefore pass between
		// "they dashed through it" and "they die for it", and possession can END inside that gap —
		// a goal, half time, a kickoff, a mode-B throw, a disconnect, a kill-steal are all
		// instantaneous. Clearing the points does nothing about a kill already in flight, so without
		// this branch the fix above would still let a player die to a trace that is provably gone
		// from the world, after they had already given up the Core. That is exactly §3's unfair
		// death, arriving by the one route that does not read TrailPoints.
		//
		// The comment this replaces argued the other way for the ORDINARY case and was right about
		// it: "a turnover or a pass clearing the ribbon a frame or two later must not refund the
		// dasher's kill". That still holds — ServerRunTripTest's empty-trail branch keeps resolving
		// held trips and this drops NOTHING unless possession itself left. The two conditions are
		// ANDed for that reason: not a carrier any more, AND the points went with it, which together
		// only describe a possession end that ran ClearTrail. A carrier who is still carrying, or
		// whose trace merely got shorter, is untouched and still dies.
		//
		// Read through GClearTraceOnPossessionLoss so the A/B arm stays a true pre-v9 build: with the
		// fix off, this kill lands exactly as it always did, and Trace.Trail.ClearAudit can still show
		// the symptom.
		if (GClearTraceOnPossessionLoss != 0 && Holder->IsAlive() && !Holder->IsCarrier()
			&& TrailPoints.Items.Num() == 0)
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[TRACEDASH] held trip %s -> %s dropped after %.0fms: the Core LEFT %s while the trip "
				     "was held and their trace was cleared with it. Spec v9 3 — a trace that no longer "
				     "exists does not kill the player who no longer holds the Core."),
				*GetNameSafe(Dasher), *GetNameSafe(Holder), PendingTripHeldSeconds * 1000.f,
				*GetNameSafe(Holder));
			ServerCancelPendingTrip();
			return false;
		}

		// Both liveness checks are re-done HERE and not at the sweep: with a hold, arbitrary frames
		// pass between "they dashed through it" and "they die for it", and either pawn may have died
		// of something else in between. Killing a corpse would double-count in the kill feed.
		if (!Holder->IsAlive() || !Dasher->IsAlive())
		{
			UE_LOG(LogTraceGame, Log,
				TEXT("[TRACEDASH] held trip %s -> %s dropped after %.0fms: %s died meanwhile."),
				*GetNameSafe(Dasher), *GetNameSafe(Holder), PendingTripHeldSeconds * 1000.f,
				Holder->IsAlive() ? TEXT("the dasher") : TEXT("the carrier"));
			ServerCancelPendingTrip();
			return false;
		}

		// parry=0 passWindow=0 is printed on the KILL line too, and it is not noise: the claim under
		// test is a conditional, so the log has to carry the negative case as explicitly as the
		// positive one. "Every dash through a trace, and whether a parry was active" is one grep.
		UE_LOG(LogTraceGame, Log,
			TEXT("[TRACEDASH] %s dashed through %s's trace: KILL. parry=0 passWindow=0 points=%d lethal=%d emitting=%d held=%.0fms (residual trace: %s)"),
			*GetNameSafe(Dasher), *GetNameSafe(Holder),
			TrailPoints.Items.Num(), TestPositions.Num(), bEmitting ? 1 : 0,
			PendingTripHeldSeconds * 1000.f,
			bEmitting ? TEXT("no") : TEXT("yes - laid before a turnover"));

		// Cleared BEFORE the kill: ApplyTrailTrip re-enters this component (death -> SetCarrying ->
		// ClearTrail) and must not find a live hold pointing at a pawn it is in the middle of killing.
		PendingTripDasher = nullptr;
		PendingTripHeldSeconds = 0.f;
		ApplyTrailTrip(Holder, Dasher);
		return false;
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

	// ---------------------------------------------------------------------------------------------
	// SPEC v9 §3, COUNTED AT THE POINT OF DEATH.
	//
	// "Being killed by your own stale trace AFTER you no longer hold the Core is the worst kind of
	// unfair death." That sentence is a property of THIS call: the victim is the player who laid the
	// trace, and either they still hold the Core (fair — the trace is the counterplay to their shield)
	// or they do not (unfair — they passed it away and died to their own leftovers).
	//
	// It is counted here rather than reconstructed from the log afterwards because it is the one
	// number that says whether the reported bug is happening in a REAL match through the REAL trip
	// test, as opposed to in a scripted harness. Trace.Trail.ClearAudit prints it. It must be 0.
	// ---------------------------------------------------------------------------------------------
	++GTrailKillsTotal;
	const bool bVictimStillHeldTheCore = Holder->IsCarrier();
	if (!bVictimStillHeldTheCore)
	{
		++GTrailKillsOnNonCarrier;

		UE_LOG(LogTraceGame, Warning,
			TEXT("[STALETRACEKILL] %s was killed by their OWN trace while NOT holding the Core "
			     "(dasher %s, emitting=%d, %d points still standing). Spec v9 3: the trace should have "
			     "been gone the instant possession left."),
			*GetNameSafe(Holder), *GetNameSafe(Tripper), IsEmitting() ? 1 : 0, TrailPoints.Items.Num());
	}

	UE_LOG(LogTraceGame, Log, TEXT("Trace broken: %s dashed through %s's trace (lethality %d, victim was %s)"),
		*GetNameSafe(Tripper), *GetNameSafe(Holder), static_cast<int32>(Lethality),
		bVictimStillHeldTheCore ? TEXT("the carrier") : TEXT("NOT the carrier - unfair"));

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
//                                     RibbonAlternateOutset.
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
	ComputeRibbonSamples(Points, Births, Step, MaxElements,
		RibbonSamples, RibbonSampleBirth, RibbonSampleSlack);
}

void UTraceTrailComponent::ComputeRibbonSamples(const TArray<FVector>& Points, const TArray<float>& Births,
	double Step, int32 MaxElements, TArray<FVector>& OutSamples, TArray<float>& OutBirths,
	TArray<float>& OutSlack)
{
	OutSamples.Reset();
	OutBirths.Reset();
	OutSlack.Reset();

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
		OutSamples.Add(Points[0]);
		OutSamples.Add(Points[0]);
		OutBirths.Add(bHaveBirths ? Births[0] : 0.f);
		OutBirths.Add(bHaveBirths ? Births[0] : 0.f);
		OutSlack.Add(0.f);
		OutSlack.Add(0.f);
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
		OutSamples.Add(Points[0]);
		OutSamples.Add(Points.Last());
		OutBirths.Add(bHaveBirths ? Births[0] : 0.f);
		OutBirths.Add(bHaveBirths ? Births.Last() : 0.f);
		OutSlack.Add(0.f);
		OutSlack.Add(0.f);
		return;
	}

	// COARSEN, NEVER TRUNCATE. A trace longer than the pool can draw at the requested step gets
	// longer elements, not a missing tail: every uu of the lethal set stays covered. (The legacy
	// smear dropped the oldest segments at its cap, which was a hole in the drawing of a thing that
	// still kills.)
	double SafeStep = FMath::Max(1.0, Step);

	// v13 §7: THE RIBBON MAY NEVER STEP OVER A LETHAL VERTEX, and this is the largest single source of
	// the wall-clip bug's residue.
	//
	// The old parameterisation walked uniform arc length along the whole trace, so with a ~110uu
	// resample step and a 60uu point spacing an element could span two or three lethal segments — and
	// an element is a straight BOX. Draw a straight box across a hairpin and its middle sits nowhere
	// near either arm of the path: measured on the fixture, 32.2uu of drawn ribbon outside the lethal
	// column, against the 13.8uu the geometry is supposed to allow. That is ribbon in mid-air where
	// nothing kills, and — the reason it belongs to this section — it is ribbon the wall fitter cannot
	// keep out of a wall, because the fitter clears room around the POLYLINE and this geometry is not
	// near the polyline.
	//
	// So sample distances are built per SEGMENT: every control point is a sample, and each segment is
	// subdivided into pieces no longer than the step. Every element then spans at most one lethal
	// segment, and the worst a straight element can deviate from the path it is drawing is the
	// spline's own clamped slack.
	//
	// COARSEN, NEVER TRUNCATE, is preserved: if the budget cannot hold the subdivision the step grows
	// until it can, and the control points themselves always survive (there are ~21 of them against a
	// pool of MaxRibbonElements), so a long trace gets longer elements and never a missing tail.
	// v14 §1: AND IT MAY NOT CLIMB MORE THAN ONE VERTICAL STEP EITHER, for the same reason one step
	// further on. An upright element (GRibbonUpright) covers the UNION of its two ends' vertical
	// bands, so it stands |dZ|/2 proud of the lethal band in the middle of a slope. Subdividing on
	// rise as well as on run caps that overhang at GRibbonVerticalStep/2 instead of leaving it to
	// whatever the terrain did. It coarsens with the horizontal step, so the budget is still a
	// budget and the trace is still never truncated.
	double SafeVerticalStep = FMath::Max(2.0, static_cast<double>(GRibbonVerticalStep));

	TArray<double, TInlineAllocator<128>> SampleDistances;
	for (int32 Attempt = 0; Attempt < 12; ++Attempt)
	{
		SampleDistances.Reset();
		SampleDistances.Add(0.0);

		for (int32 Index = 1; Index < PointCount; ++Index)
		{
			const double SegLength = Cumulative[Index] - Cumulative[Index - 1];
			const double SegRise = FMath::Abs(Points[Index].Z - Points[Index - 1].Z);
			const int32 Pieces = FMath::Max(1, FMath::Max(
				FMath::CeilToInt(SegLength / SafeStep),
				FMath::CeilToInt(SegRise / SafeVerticalStep)));
			for (int32 Piece = 1; Piece <= Pieces; ++Piece)
			{
				SampleDistances.Add(Cumulative[Index - 1] + (SegLength * Piece) / static_cast<double>(Pieces));
			}
		}

		if ((SampleDistances.Num() - 1) <= FMath::Max(1, MaxElements))
		{
			break;
		}
		SafeStep *= 1.6;
		SafeVerticalStep *= 1.6;
	}

	const int32 ElementCount = FMath::Max(1, SampleDistances.Num() - 1);

	OutSamples.Reserve(ElementCount + 1);
	OutBirths.Reserve(ElementCount + 1);
	OutSlack.Reserve(ElementCount + 1);

	int32 Segment = 0;
	for (int32 SampleIndex = 0; SampleIndex <= ElementCount; ++SampleIndex)
	{
		const double Distance = SampleDistances[FMath::Min(SampleIndex, SampleDistances.Num() - 1)];

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

		// v13 §7: THE SPLINE IS CLAMPED TO THE POLYLINE, AND THIS IS LOAD-BEARING FOR THE WALL FIX.
		//
		// A Catmull-Rom does not stay inside its control polygon, and the textbook figure for how far
		// it strays — 0.074 x segment length at a right angle — is only true when consecutive segments
		// are the SAME length. This polyline's are not: the fitter inserts points at whatever spacing
		// the carrier's real route needed and the length trim drops them off the tail, so a 6uu segment
		// can sit next to a 70uu one, and a uniformly parameterised Catmull-Rom across that pair
		// excursions far further than the idealised number. Measured on the arm-0 fixture before this
		// clamp: the drawn ribbon stood 25.0uu outside the lethal column horizontally, against the
		// 13.8uu GetTraceDrawnHalfReach() predicts.
		//
		// That mattered for more than tidiness. GetTraceDrawnHalfReach() is what the wall fitter clears
		// room for; if the drawing can quietly reach further than it, the fitter under-clears by the
		// difference and ribbon ends up in the wall no matter how large the push allowance is. So the
		// overshoot is BOUNDED HERE rather than estimated there, and the reach becomes a guarantee
		// instead of a model of a corner nobody promised to run.
		//
		// v14 §1 CORRECTED THE LAST SENTENCE OF THAT ARGUMENT, and the correction was worth 2.5uu of
		// invisible kill volume at a hairpin. "Each element is still TrailRadius wide about a sample
		// that is nearer the line" is true and does not say what it was taken to say: TrailRadius
		// wide ABOUT THE SAMPLE is not TrailRadius wide about the POLYLINE. A sample sitting Slack uu
		// to one side of the line leaves the far edge of its element only (TrailRadius - Slack) from
		// the line, and the strip between there and TrailRadius kills with nothing drawn on it. The
		// measured figure on the HAIRPIN fixture was exactly that: 2.5uu.
		//
		// So the residual deviation is RECORDED per sample and BuildRibbonElements adds it back to
		// that element's half width. The spline keeps its smoothing, the cost is paid where it is
		// incurred (a few uu, on curved elements only) and it is paid in the over-drawing direction.
		double SampleDeviation = 0.0;
		FVector CurveSample = CatmullRom(P0, P1, P2, P3, T);
		{
			const double Slack = 0.0741
				* FMath::Max(1.0, static_cast<double>(UTraceSettings::Get().TrailPointSpacing));

			double NearestDistance = TNumericLimits<double>::Max();
			FVector NearestOnPolyline = CurveSample;
			for (int32 Edge = 0; Edge + 1 < PointCount; ++Edge)
			{
				const FVector ClosestOnEdge =
					FMath::ClosestPointOnSegment(CurveSample, Points[Edge], Points[Edge + 1]);
				const double EdgeDistance = FVector::Dist(CurveSample, ClosestOnEdge);
				if (EdgeDistance < NearestDistance)
				{
					NearestDistance = EdgeDistance;
					NearestOnPolyline = ClosestOnEdge;
				}
			}

			if (NearestDistance > Slack)
			{
				CurveSample = NearestOnPolyline
					+ (CurveSample - NearestOnPolyline) * (Slack / NearestDistance);
			}

			SampleDeviation = FMath::Min(NearestDistance, Slack);
		}

		OutSlack.Add(static_cast<float>(SampleDeviation));
		OutSamples.Add(CurveSample);

		if (bHaveBirths)
		{
			const float BirthA = Births[Segment];
			const float BirthB = Births[FMath::Min(PointCount - 1, Segment + 1)];
			OutBirths.Add(FMath::Lerp(BirthA, BirthB, static_cast<float>(T)));
		}
		else
		{
			OutBirths.Add(0.f);
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

void UTraceTrailComponent::BuildRibbonElements(const TArray<FVector>& Samples,
	const TArray<float>& SampleSlack, double Radius,
	double Height, double WidthScale, bool bOverlapAtStart, TArray<FTraceRibbonElement>& OutElements)
{
	OutElements.Reset();

	const int32 ElementCount = FMath::Max(0, Samples.Num() - 1);
	if (ElementCount <= 0)
	{
		return;
	}

	// EXACTLY the lethal cross-section, both axes, unless somebody has explicitly dialled the width
	// down with Trace.Trail.RibbonWidthScale (default 1.0 — see the comment there for what a
	// narrower ribbon costs the player). A boundary drawn narrower than it really is turns the trace
	// into a trap rather than a warning.
	const double LethalWidth = FMath::Max(1.0, 2.0 * Radius);
	const double Width = LethalWidth * FMath::Clamp(WidthScale, 0.05, 2.0);
	const double LethalHeight = FMath::Max(1.0, Height);

	const bool bUpright = (GRibbonUpright != 0);

	// -----------------------------------------------------------------------------------------
	// THE JOINT OVERLAP IS NOW A SEAM, NOT A CAP (spec v14 §1). This is the last of the four
	// disagreements between the drawn solid and the lethal one, and it is the one that took two
	// attempts, so the reasoning is written out.
	//
	// The overlap existed to close the wedge on the OUTSIDE of a corner. That wedge is only lethal
	// because the trip test measured distance TO A SEGMENT, whose cap is a disc — so the volume that
	// killed bulged a half-disc of TrailRadius past every interior joint as well as past the two
	// ends, and a box chain had to run TrailRadius past each joint to cover it.
	//
	// THAT IS NOT PAYABLE ON A SLOPE, and the measurement says so rather than the argument. On a
	// 45-degree climb every joint is straight in plan, so the extension buys nothing and costs
	// everything: the element runs 22.5uu further horizontally while its vertical band stays where it
	// was, and the lethal band has climbed 22.5uu by then — 28.0uu of drawn ribbon outside the kill
	// volume on a path with no corner in it. Cutting the overlap to a turn-dependent miter fixed that
	// and immediately opened the other direction: the disc at a joint on a climb reaches 22.5uu BACK
	// in plan at the joint's own height, which is above the previous element's band, so 7.5uu of that
	// disc had nothing drawn on it. A short segment does the same thing sideways — 9.7uu on the mixed
	// spacing fixture, where a 6uu last segment let its neighbour's disc reach straight through the
	// trace's own flat end cap.
	//
	// Both of those are the DISC, not the overlap. So the disc goes (Trace.Trail.FlatEndCaps applies
	// to every cap now, not only the two outer ones), the lethal volume becomes exactly the union of
	// flat-ended slabs — which is exactly what a chain of boxes is — and the overlap has no covering
	// job left to do. It survives at one uu, purely so consecutive elements always share a sliver and
	// no seam can open between two boxes of very slightly different width.
	//
	// WHAT THIS COSTS IN PLAY, said plainly: the outside of a sharp corner stops killing. The notch
	// is TrailRadius * tan(turn/2) deep — 22.5uu at a right angle, ~4uu at the 20-degree turns a
	// carrier actually lays at 60uu spacing — and it is now DRAWN as a notch, so a player who cuts a
	// corner sees the gap they are cutting through. That is the whole point of the section.
	const double SeamOverlap = 1.0;

	TArray<double, TInlineAllocator<128>> JointOverlaps;
	JointOverlaps.SetNumZeroed(ElementCount + 1);
	for (int32 Joint = 1; Joint < ElementCount; ++Joint)
	{
		JointOverlaps[Joint] = SeamOverlap;
	}

	// The stub's backward join into the real ribbon is a seam a carrier looks straight at from a
	// metre away, and it is owner-only and excluded from every measurement, so it keeps the generous
	// overlap that makes the join invisible.
	if (bOverlapAtStart)
	{
		JointOverlaps[0] = FMath::Max(1.0, Radius);
	}

	OutElements.Reserve(ElementCount);

	// The last heading with any horizontal travel in it. A stretch of pure fall has no direction in
	// plan, so it inherits one rather than snapping to world forward — which would spin the ribbon.
	double CarriedYaw = 0.0;
	bool bHaveCarriedYaw = false;

	for (int32 ElementIndex = 0; ElementIndex < ElementCount; ++ElementIndex)
	{
		const FVector SegStart = Samples[ElementIndex];
		const FVector SegEnd = Samples[ElementIndex + 1];

		// Interior joints overlap; the two OUTER ends stay flush with the first and last lethal point,
		// so the ribbon never extends past the polyline the server kills along — and since v14 §1 the
		// server does not kill past it either (GTrailFlatEndCaps), which is what makes "flush" the
		// right answer in BOTH directions instead of only one.
		//
		// bOverlapAtStart is the one exception, and it is the seam the user is looking straight at: the
		// owner-only predicted stub begins exactly where the drawn lethal set ends, so ITS first
		// element overlaps BACKWARD into the last real one. Without that, the one joint a carrier sees
		// from a metre away is the only butt joint in the whole trace.
		const double BackOverlap = JointOverlaps[ElementIndex];
		const double ForwardOverlap = (ElementIndex + 1 < ElementCount) ? JointOverlaps[ElementIndex + 1] : 0.0;

		// v14 §1: THE SPLINE'S RESIDUAL DEVIATION IS ADDED BACK TO THIS ELEMENT'S HALF WIDTH.
		//
		// A ribbon element is Radius wide about a SAMPLE, and the sample can sit up to the clamped
		// slack off the polyline the trip test kills along. Without this, the far edge of a curved
		// element reaches only (Radius - deviation) from the line, and the strip beyond it kills with
		// nothing drawn on it — 2.5uu of invisible kill volume, measured on the HAIRPIN fixture. The
		// deviation is per sample and usually zero, so straight trace is untouched.
		const double SampleWiden = FMath::Max(
			SampleSlack.IsValidIndex(ElementIndex) ? static_cast<double>(SampleSlack[ElementIndex]) : 0.0,
			SampleSlack.IsValidIndex(ElementIndex + 1) ? static_cast<double>(SampleSlack[ElementIndex + 1]) : 0.0);

		// Union of the two ends' vertical bands: over-drawing a lethal boundary is the safe direction,
		// under-drawing it is a trap. Bounded by GRibbonVerticalStep, which is why this is now a few uu
		// of honest overhang rather than half a storey of it.
		const double SpanHeight = LethalHeight + FMath::Abs(SegEnd.Z - SegStart.Z);
		const double MidZ = (SegStart.Z + SegEnd.Z) * 0.5;

		FTraceRibbonElement& Element = OutElements.AddDefaulted_GetRef();

		if (bUpright)
		{
			// v14 §1. YAW ONLY. The volume that kills is a vertical column of half width Radius about
			// the polyline's PLAN projection, with a vertical band about its height — so the element
			// that draws it is a vertical box whose length runs along the plan direction. See
			// GRibbonUpright for the 29.1uu this removes and for what it costs the look of a jump.
			FVector Plan(SegEnd.X - SegStart.X, SegEnd.Y - SegStart.Y, 0.0);
			const double PlanLength = Plan.Size();

			double Yaw = bHaveCarriedYaw ? CarriedYaw : 0.0;
			if (PlanLength > GeometryEpsilon)
			{
				Yaw = FMath::RadiansToDegrees(FMath::Atan2(Plan.Y, Plan.X));
				CarriedYaw = Yaw;
				bHaveCarriedYaw = true;
			}

			Element.Rotation = FRotator(0.0, Yaw, 0.0);

			const FVector PlanCentre(
				(SegStart.X + SegEnd.X) * 0.5,
				(SegStart.Y + SegEnd.Y) * 0.5,
				MidZ);

			if (PlanLength > GeometryEpsilon)
			{
				const FVector Direction = Element.Rotation.Vector();
				Element.Centre = PlanCentre + Direction * ((ForwardOverlap - BackOverlap) * 0.5);
				Element.Size = FVector(PlanLength + BackOverlap + ForwardOverlap,
					Width + 2.0 * SampleWiden, SpanHeight);
			}
			else
			{
				// STRAIGHT DOWN (or straight up). In plan this segment is a POINT, so the volume that
				// kills is a disc of Radius about it and the smallest box that covers a disc is the
				// square that circumscribes it. That over-draws the disc's corners by
				// Radius * (sqrt(2) - 1) = 9.3uu, which is inside the standing overhang budget and is
				// the same corner allowance every sharp turn already spends. Anything narrower would
				// leave part of a kill volume undrawn, which is the direction that is not allowed.
				Element.Centre = PlanCentre;
				Element.Size = FVector(LethalWidth + 2.0 * SampleWiden,
					Width + 2.0 * SampleWiden, SpanHeight);
			}
		}
		else
		{
			// THE PRE-v14 ARM, kept verbatim so Trace.Trail.LethalDrawn 0 reproduces the reported bug
			// on the shipping build. Yaw AND pitch: the element leans to follow the path, and on any
			// slope it leans straight out of the vertical column that actually kills.
			FVector Along = SegEnd - SegStart;
			const double Length = Along.Size();
			const FVector Direction = (Length > GeometryEpsilon) ? (Along / Length) : FVector::ForwardVector;

			Element.Rotation = Direction.Rotation();
			Element.Size = FVector(
				FMath::Max(LethalWidth, Length + BackOverlap + ForwardOverlap), Width, SpanHeight);
			Element.Centre = (SegStart + SegEnd) * 0.5 + Direction * ((ForwardOverlap - BackOverlap) * 0.5);
		}

		// See RibbonAlternateOutset: the anti-Z-fight, and it is applied HERE rather than at the mesh
		// so that what the harness measures is what is on screen, down to the last tenth of a uu.
		if ((ElementIndex & 1) != 0)
		{
			Element.Size.Y *= RibbonAlternateOutset;
			Element.Size.Z *= RibbonAlternateOutset;
		}
	}
}

double UTraceTrailComponent::DistanceOutsideRibbonElements(const TArray<FTraceRibbonElement>& Elements,
	const FVector& At)
{
	double Nearest = TNumericLimits<double>::Max();

	for (const FTraceRibbonElement& Element : Elements)
	{
		const FVector Half = Element.Size * 0.5;
		const FVector Local = Element.Rotation.UnrotateVector(At - Element.Centre);
		const FVector Clamped(
			FMath::Clamp(Local.X, -Half.X, Half.X),
			FMath::Clamp(Local.Y, -Half.Y, Half.Y),
			FMath::Clamp(Local.Z, -Half.Z, Half.Z));

		// The rotation is rigid, so the local distance IS the world distance.
		Nearest = FMath::Min(Nearest, FVector::Dist(Local, Clamped));
		if (Nearest <= 0.0)
		{
			return 0.0;
		}
	}

	return (Nearest == TNumericLimits<double>::Max()) ? 0.0 : Nearest;
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


	// THE SHAPE IS DECIDED IN ONE PLACE AND ONLY ONE PLACE (v14 §1). Everything below this line is
	// meshes, materials, glow and pooling; nothing below it may move a surface.
	BuildRibbonElements(RibbonSamples, RibbonSampleSlack,
		static_cast<double>(GetTraceTrailRadius()),
		static_cast<double>(GetTraceTrailHeight()),
		static_cast<double>(GRibbonWidthScale),
		bOverlapAtStart,
		RibbonElements);

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

		if (!RibbonElements.IsValidIndex(ElementIndex))
		{
			break;
		}
		const FTraceRibbonElement& Element = RibbonElements[ElementIndex];

		const FVector Scale(
			Element.Size.X / (2.0 * HalfSize.X),
			Element.Size.Y / (2.0 * HalfSize.Y),
			Element.Size.Z / (2.0 * HalfSize.Z));

		// Corrects for a source mesh whose pivot is not at its bounds centre, so we never assume
		// anything about the engine primitives' authoring.
		const FVector PivotCorrection = Element.Rotation.RotateVector(PivotOffset * Scale);

		Piece->SetWorldLocationAndRotation(Element.Centre - PivotCorrection, Element.Rotation);
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
//
// -------------------------------------------------------------------------------------------------
// SPEC v8 §2 — "TRACE STILL HAS THE SAME BUG FOR THE PEOPLE CONNECTING TO THE SERVER."
//
// THE v7 §7 REORDERING WAS REAL AND IS FIXED (RestoreReplicatedPointOrder repairs a scrambled array
// ~5 times a second on a live client, and the geometric detector reads a clean path afterwards —
// measured, both arms, on a client at 40ms). THE PART THAT WAS NEVER MEASURED ON A CLIENT IS THIS
// FUNCTION, because every probe in this file watches a BOT's trace, and a bot has no owner-only stub.
// The one case the report is actually about — a human, on a client, carrying — was the one case with
// no coverage at all. Trace.Trail.OwnerHeadAB is the harness that closes that hole.
//
// WHAT IT FOUND. The gap this stub has to cover is not one term on a client, it is three:
//
//     head grace (~80uu, every machine)
//   + the trace the server has laid and the wire has not yet delivered  (one round trip)
//   + the client's own movement prediction, which puts the pawn a FURTHER round trip ahead of the
//     position those points were laid at
//
// At a walk that sum is ~180uu and the old 400uu cap never fired. At dash speed (3000uu/s) it is
// 400-600uu, so the cap fired ON EVERY DASH — and the cap does not shorten the stub, it deletes it.
// The carrier's own trace detached from their body for the length of every dash and snapped back
// afterwards. On the host the same sum is ~80uu and the cap can never fire, which is exactly the
// shape of "it feels really good for me as the host but not great for the players joining".
//
// THE FIX IS TO STOP GUESSING, NOT TO RAISE THE TOLERANCE FOR GUESSING. The stub is now built THROUGH
// the carrier's own recorded per-frame positions (LocalPathHistory), so every uu of it is ground this
// pawn is recorded as having covered on this machine. That makes the length cap a budget rather than
// an honesty test, which is why it could go to 1200 (the trace's own maximum length); the honesty
// test moved to where it belongs — a PER-SEGMENT check against MaxTrailSegmentLength, the same limit
// the server restarts its own trace on, which is what still refuses to draw a line across a teleport.
// It also fixes a second, quieter defect on the frames the old chord did draw: a chord across a round
// trip cuts whatever corner the carrier turned during it, so their trace left their body at the wrong
// angle. A recorded path turns the corner they turned.
//
// THE INVARIANT IS EXACTLY AS SAFE AS IT WAS, and for exactly the same two reasons — the stub is
// still built only on the carrier's own machine and is still bOnlyOwnerSee, so the set of viewers who
// can see it is still "whoever is looking through the carrier", and the carrier is the one player
// their own trace can never kill. The stub got longer; the set of people it can mislead is still
// empty. What the carrier is told is unchanged in kind and more accurate in degree: nearly all of the
// stub is now ground the server HAS already laid lethal trace on and this machine has not been told
// about yet, where the old chord's honest claim was only ever about the ~80uu under their own feet.
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

	// ---- build the polyline: last drawn point -> real ungated points -> RECORDED PATH -> the pawn --
	//
	// SPEC v8 §2. The middle term is new and it is the fix. The v5 §2 version jumped straight from the
	// newest replicated point to the pawn, which on the host is a step of about one body width and on
	// a joined client is a step of half a dash — across which the carrier has usually turned. Two
	// things went wrong there and only one of them was visible:
	//
	//   IT WAS DELETED. The length cap (GPredictedHeadMaxLength) is a fabrication guard, and a chord
	//   that long IS a fabrication, so the guard fired and took the whole stub with it. On a client,
	//   on every dash. That is the reported bug.
	//   IT CUT THE CORNER. On the frames it did draw, the chord crossed whatever the carrier had run
	//   round, so their own trace left their body at the wrong angle.
	//
	// LocalPathHistory removes the guesswork rather than widening the tolerance for it: these are the
	// pawn's own recorded positions on the pawn's own machine, so the stub follows the path that was
	// actually taken. Samples the replicated set has already caught up with were dropped by
	// RecordLocalPathSample(); anything left is ground the server either has just laid trace on or is
	// about to.
	TArray<FVector, TInlineAllocator<32>> Path;
	Path.Add(FVector(TrailPoints.Items[LastLethal].Location));
	for (int32 Index = LastLethal + 1; Index < TrailPoints.Items.Num(); ++Index)
	{
		Path.Add(FVector(TrailPoints.Items[Index].Location));
	}

	PredictedHeadSamplesUsed = 0;
	if (GPredictedHeadUseHistory != 0 && LocalPathHistory.Num() > 0)
	{
		// WHICH SAMPLES ARE STILL AHEAD OF THE REPLICATED TRACE — decided by GEOMETRY, not by clocks.
		//
		// The newest replicated point is not an abstract timestamp: it is a place this pawn STOOD, laid
		// by the server at the pawn's own capsule centre. So the sample nearest to it is the moment
		// this machine was there, and everything recorded after that sample is exactly the path since —
		// which is the piece the stub exists to cover.
		//
		// The obvious alternative, "keep samples stamped later than the point's BirthServerTime", was
		// rejected: those two numbers come from two different machines' readings of the shared clock,
		// and a client's reading is an estimate. A few ms of skew in the wrong direction would discard
		// every sample and silently drop the stub back to the v5 chord — the same failure this pass is
		// fixing, hidden behind a clock instead of a cap. A distance measured on one machine cannot
		// skew.
		const FVector NewestReplicated(TrailPoints.Items.Last().Location);

		int32 StartIndex = 0;
		double NearestToHead = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < LocalPathHistory.Num(); ++Index)
		{
			const double Distance = FVector::Dist(LocalPathHistory[Index].Location, NewestReplicated);
			if (Distance < NearestToHead)
			{
				NearestToHead = Distance;
				StartIndex = Index;
			}
		}

		for (int32 Index = StartIndex + 1; Index < LocalPathHistory.Num(); ++Index)
		{
			const FVector& SampleLocation = LocalPathHistory[Index].Location;
			if (FVector::Dist(Path.Last(), SampleLocation) < LocalPathSampleSpacing)
			{
				continue;   // Too close to the previous control point to change the curve.
			}
			Path.Add(SampleLocation);
			++PredictedHeadSamplesUsed;
		}
	}

	const FVector PawnLocation = Holder->GetActorLocation();
	if (FVector::Dist(Path.Last(), PawnLocation) > 1.0)
	{
		Path.Add(PawnLocation);
	}

	// TWO TESTS, AND THEY ARE ASKING DIFFERENT QUESTIONS. Splitting them is what let the cap rise.
	//
	//   PER SEGMENT (this one): could a player have travelled from one control point to the next?
	//   MaxTrailSegmentLength is the server's own answer to that question — ServerUpdateTrail refuses
	//   to join two points further apart than this and restarts the trace instead — so a step past it
	//   is a teleport, a respawn or a desync, and joining it up would draw a line through the arena
	//   that nobody ran. This is the fabrication guard, and it is exact.
	//   TOTAL LENGTH (below): is the stub longer than the trace it is continuing? That is a budget
	//   question, not an honesty one, now that every segment is a segment somebody ran.
	double TotalLength = 0.0;
	double WorstStep = 0.0;
	for (int32 Index = 1; Index < Path.Num(); ++Index)
	{
		const double Step = FVector::Dist(Path[Index - 1], Path[Index]);
		TotalLength += Step;
		WorstStep = FMath::Max(WorstStep, Step);
	}

	PredictedHeadSpan = static_cast<float>(TotalLength);

	if (TotalLength <= 1.0)
	{
		HidePredictedHead();
		return;
	}

	if (WorstStep > MaxTrailSegmentLength || TotalLength > static_cast<double>(GPredictedHeadMaxLength))
	{
		// Abandoned, never clamped — see the header comment. Counted, and said out loud once per
		// episode, because THIS SILENCE WAS THE BUG: the pre-v8 stub declined on every dash a client
		// took and the only evidence was the player's own eyes.
		//
		// WARNING RATHER THAN VERBOSE, and the measurement is the argument. Trace.Trail.OwnerHeadAB on
		// a joined client at 40ms: the pre-v8 arm declined on 10 frames of a 15s carry — 23% of the
		// frames the carrier was actually at dash speed — and the gap the carrier could see between
		// themselves and their own trace reached 761uu. The v8 arm declined on none, on the client and
		// on the host. So a line here means a carrier's trace is detached from their body RIGHT NOW,
		// which is the whole of the report this pass exists to close, and it cannot be filed at a
		// verbosity nobody enables. It stays one line per episode (bPredictedHeadDropReported) and a
		// healthy machine, host or client, never prints it at all.
		++PredictedHeadDrops;
		if (!bPredictedHeadDropReported)
		{
			bPredictedHeadDropReported = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[HEADSTUB] %s: predicted head declined - span %.0fuu (cap %.0f), worst step %.0fuu "
				     "(limit %.0f), %d recorded samples. The carrier's trace is detached from their body "
				     "for as long as this lasts."),
				*GetNameSafe(GetOwner()), TotalLength, GPredictedHeadMaxLength, WorstStep,
				MaxTrailSegmentLength, PredictedHeadSamplesUsed);
		}

		HidePredictedHead();
		return;
	}

	bPredictedHeadDropReported = false;

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

void UTraceTrailComponent::RecordLocalPathSample()
{
	// ONLY THE MACHINE THAT IS PREDICTING THIS PAWN, which is the same gate UpdatePredictedHead uses
	// and for the same reason: a listen host owns every bot's component, and a record of a bot's path
	// would be a record of a path the host already has authoritative points for.
	//
	// Anyone else drops the record entirely. That is not tidiness — it is what guarantees a stale
	// sample from a previous life (a respawn, a turnover, a pawn that stopped carrying) can never
	// reach the geometry, because there is nothing left to reach it with.
	const ATraceCharacter* Holder = GetOwnerCharacter();
	const APlayerController* HolderPC = (Holder != nullptr)
		? Cast<APlayerController>(Holder->GetController()) : nullptr;

	if (Holder == nullptr || !Holder->IsAlive() || !bEmitting
		|| HolderPC == nullptr || !HolderPC->IsLocalPlayerController())
	{
		LocalPathHistory.Reset();
		return;
	}

	const float Now = GetServerTimeSeconds();

	// Age out. The window only has to cover a round trip; a second is an order of magnitude more than
	// that, and the geometric selection in UpdatePredictedHead ignores whatever is older anyway. This
	// is the ceiling that keeps the array finite on a client whose updates have stalled, not the
	// mechanism that decides what gets drawn.
	int32 FirstKept = 0;
	while (FirstKept < LocalPathHistory.Num()
		&& (Now - LocalPathHistory[FirstKept].ServerTime) > LocalPathHistorySeconds)
	{
		++FirstKept;
	}
	if (FirstKept > 0)
	{
		LocalPathHistory.RemoveAt(0, FirstKept, EAllowShrinking::No);
	}

	const FVector Location = Holder->GetActorLocation();
	if (LocalPathHistory.Num() == 0
		|| FVector::Dist(LocalPathHistory.Last().Location, Location) >= LocalPathSampleSpacing)
	{
		FTraceLocalPathSample& Sample = LocalPathHistory.AddDefaulted_GetRef();
		Sample.Location = Location;
		Sample.ServerTime = Now;
	}

	if (LocalPathHistory.Num() > MaxLocalPathSamples)
	{
		LocalPathHistory.RemoveAt(0, LocalPathHistory.Num() - MaxLocalPathSamples, EAllowShrinking::No);
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

	// =============================================================================================
	// Trace.Trail.ClientTrace — SPEC v8 §2. THE PER-FRAME CLIENT RECORDER.
	//
	// WHY A SECOND HARNESS AT ALL. Trace.Trail.TetherCheck samples on a TIMER (2s by default), and it
	// prints ORD — the number of adjacent pairs whose BirthServerTime is out of order. Both of those
	// choices can miss the reported bug:
	//
	//   * A 2s SAMPLE CANNOT SEE A ONE-FRAME EVENT. A trace that is wrong on the frame a point retires
	//     and right again a frame later is, to a player, a trace that flickers and snaps — and to a
	//     timer-sampled probe it is a clean pass. Every sample here is one FRAME.
	//   * ORD IS NOT THE SYMPTOM. The player never sees BirthServerTime; they see the polyline the
	//     ribbon is swept along. SEG below is that polyline's worst adjacent gap. The server lays
	//     points at TrailPointSpacing (60uu) and restarts on a discontinuity, so a healthy path's SEG
	//     is a small multiple of 60 whatever the carrier is doing, and a path that has been reordered
	//     — for any reason, whether or not the birth stamps happen to look monotone — reads several
	//     hundred to over a thousand.
	//
	// FARTHEST is the report in the user's own words: uu from the carrier to the FARTHEST VISIBLE
	// PIECE of ribbon. "The far end of the trace is pulled back towards their character model" is
	// exactly FARTHEST collapsing while LEN stays long, measured off the geometry on screen rather
	// than argued from the array behind it.
	//
	// RUN IT ON THE CLIENT. On the host every number here is a tautology.
	// =============================================================================================
	struct FTraceClientTraceStats
	{
		int32 Samples = 0;
		int32 LocalHumanFrames = 0;

		int32 MaxOrd = 0;
		int32 OrdFrames = 0;
		int32 Repairs = 0;

		/**
		 * Adjacent pairs sharing an identical BirthServerTime.
		 *
		 * The repair sorts on BirthServerTime with a STABLE sort and the violation counter uses a
		 * STRICT less-than, so any pair of points stamped with the same time is invisible to both: the
		 * sort leaves them in whatever order the fast array's RemoveAtSwap left them, and ORD reports a
		 * clean zero over the top of it. If this column is ever non-zero the v7 §7 repair has a blind
		 * spot, and it would be exactly the blind spot that lets the reported bug survive a fix that
		 * measured ORD 2 -> 0.
		 */
		int32 MaxEqualBirths = 0;
		int32 EqualBirthFrames = 0;

		float MinLen = TNumericLimits<float>::Max();
		float MaxLen = 0.f;

		float MinTail = TNumericLimits<float>::Max();
		float MaxTail = 0.f;

		float MaxSeg = 0.f;
		int32 BrokenPathFrames = 0;

		float MaxHeadGap = 0.f;
		double SumHeadGap = 0.0;

		float MaxTailJump = 0.f;
		float MaxHeadJump = 0.f;

		float MinFarthest = TNumericLimits<float>::Max();
		float MaxFarthest = 0.f;
		float MaxNearest = 0.f;

		float MaxSpeed = 0.f;
		int32 MovingFrames = 0;

		/**
		 * uu the carrier's DRAWN MESH is displaced from the capsule root this frame.
		 *
		 * The spec's fourth candidate cause, made into a number. The trail points are laid at the
		 * capsule root, so on a machine where the root and the mesh coincide (the server, and the
		 * local player anywhere) the ribbon meets the body. A SIMULATED PROXY is different:
		 * ACharacter's network smoothing leaves the capsule on the last replicated position and drags
		 * the MESH along behind it, so the newest trace can be drawn a long way in front of the body
		 * it is supposed to be coming out of — on a joining client, and only there.
		 */
		float MaxMeshOffset = 0.f;
		double SumMeshOffset = 0.0;

		FVector LastTail = FVector::ZeroVector;
		FVector LastHead = FVector::ZeroVector;
		bool bHasLast = false;

		/** One full array dump per trace, on the first frame its path is geometrically impossible. */
		bool bDumpedBrokenFrame = false;
	};

	/** Live recording state. Keyed by actor name so a pawn respawn starts a fresh row. */
	TMap<FString, FTraceClientTraceStats> GClientTraceStats;
	bool GClientTraceRecording = false;

	/**
	 * Adjacent gap above which the replicated path is certainly not a path any more.
	 *
	 * Calibrated to the ORDER REPAIR's own MaxTrailSegmentLength (1000uu), not to 4x TrailPointSpacing
	 * (240uu, what this used to be). 240 was mis-calibrated and reported false BROKEN frames: a
	 * legitimate path exceeds it whenever the server hitches or a carrier dashes at 3000uu/s, since
	 * one point every ~0.02s of a 3000uu/s dash is already 60uu and a single dropped tick doubles or
	 * triples that. Anything the repair itself is willing to call one segment must not be counted as
	 * a break here, or the diagnostic accuses the code of a bug it does not have.
	 */
	constexpr float ClientTraceBrokenSegmentUU = 1000.f;

	/**
	 * One frame of sampling. Returns true if any live trace was seen.
	 *
	 * The return value is what makes this usable from -ExecCmds in a headless run: the caller starts
	 * the clock on the FIRST frame a trace exists, so the recording window cannot quietly expire while
	 * the process is still loading a map. A window that silently measured nothing is how a client-only
	 * bug survives a test.
	 */
	bool ClientTraceSampleOneFrame()
	{
		UWorld* World = FindTrailDebugWorld();
		TArray<ATraceCharacter*> Characters;
		GatherTrailDebugCharacters(World, Characters);

		bool bSawTrace = false;

		for (const ATraceCharacter* TraceChar : Characters)
		{
			const UTraceTrailComponent* Trail = (TraceChar != nullptr) ? TraceChar->Trail : nullptr;
			if (Trail == nullptr || Trail->TrailPoints.Items.Num() < 2)
			{
				continue;
			}

			bSawTrace = true;

			FTraceClientTraceStats& S = GClientTraceStats.FindOrAdd(GetNameSafe(TraceChar));

			++S.Samples;
			S.LocalHumanFrames += IsLocallyControlledHuman(TraceChar) ? 1 : 0;

			const int32 Ord = Trail->CountPointOrderViolations();
			S.MaxOrd = FMath::Max(S.MaxOrd, Ord);
			S.OrdFrames += (Ord > 0) ? 1 : 0;
			S.Repairs = Trail->GetPointOrderRepairCount();

			const float Len = Trail->MeasureTraceLength();
			S.MinLen = FMath::Min(S.MinLen, Len);
			S.MaxLen = FMath::Max(S.MaxLen, Len);

			const float Tail = Trail->MeasureTailDistanceToCarrier();
			S.MinTail = FMath::Min(S.MinTail, Tail);
			S.MaxTail = FMath::Max(S.MaxTail, Tail);

			int32 EqualBirths = 0;
			for (int32 Index = 1; Index < Trail->TrailPoints.Items.Num(); ++Index)
			{
				if (Trail->TrailPoints.Items[Index].BirthServerTime
					== Trail->TrailPoints.Items[Index - 1].BirthServerTime)
				{
					++EqualBirths;
				}
			}
			S.MaxEqualBirths = FMath::Max(S.MaxEqualBirths, EqualBirths);
			S.EqualBirthFrames += (EqualBirths > 0) ? 1 : 0;

			const float Seg = Trail->MeasureMaxAdjacentSegment();
			S.MaxSeg = FMath::Max(S.MaxSeg, Seg);
			S.BrokenPathFrames += (Seg > ClientTraceBrokenSegmentUU) ? 1 : 0;

			// THE EVIDENCE, not the summary. The first frame this trace is broken, print the whole
			// array — index, ReplicationID, birth stamp, position, and the gap to the previous point.
			// A maximum tells you something went wrong; this tells you WHAT went wrong, and it is the
			// difference between diagnosing the next report and guessing at it again. Once per trace.
			if (Seg > ClientTraceBrokenSegmentUU && !S.bDumpedBrokenFrame)
			{
				S.bDumpedBrokenFrame = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[CLIENTTRACE] BROKEN PATH on %s: %d points, worst adjacent gap %.0fuu. Dumping the array."),
					*GetNameSafe(TraceChar), Trail->TrailPoints.Items.Num(), Seg);

				for (int32 Index = 0; Index < Trail->TrailPoints.Items.Num(); ++Index)
				{
					const FTraceTrailPoint& Point = Trail->TrailPoints.Items[Index];
					const double GapToPrevious = (Index > 0)
						? FVector::Dist(Trail->TrailPoints.Items[Index - 1].Location, Point.Location)
						: 0.0;

					UE_LOG(LogTraceGame, Warning,
						TEXT("[CLIENTTRACE]   [%2d] repID=%6d birth=%10.3f gapToPrev=%8.1fuu at %s"),
						Index, Point.ReplicationID, Point.BirthServerTime, GapToPrevious,
						*FVector(Point.Location).ToCompactString());
				}
			}

			const float HeadGap = Trail->MeasureHeadGap();
			S.MaxHeadGap = FMath::Max(S.MaxHeadGap, HeadGap);
			S.SumHeadGap += HeadGap;

			// FRAME-TO-FRAME MOVEMENT OF THE TWO ENDS OF THE PATH. The tail point is a fixed piece of
			// world geometry until it retires, at which point it steps back by one spacing — so a tail
			// jump of hundreds of uu is the far end teleporting, which is what "snaps to the character"
			// looks like from inside the data.
			const FVector TailPoint(Trail->TrailPoints.Items[0].Location);
			const FVector HeadPoint(Trail->TrailPoints.Items.Last().Location);
			if (S.bHasLast)
			{
				S.MaxTailJump = FMath::Max(S.MaxTailJump, static_cast<float>(FVector::Dist(S.LastTail, TailPoint)));
				S.MaxHeadJump = FMath::Max(S.MaxHeadJump, static_cast<float>(FVector::Dist(S.LastHead, HeadPoint)));
			}
			S.LastTail = TailPoint;
			S.LastHead = HeadPoint;
			S.bHasLast = true;

			float Nearest = -1.f;
			float Farthest = -1.f;
			int32 Visible = 0;
			Trail->MeasureDrawnSpan(Nearest, Farthest, Visible);
			if (Visible > 0)
			{
				S.MinFarthest = FMath::Min(S.MinFarthest, Farthest);
				S.MaxFarthest = FMath::Max(S.MaxFarthest, Farthest);
				S.MaxNearest = FMath::Max(S.MaxNearest, Nearest);
			}

			const float Speed = static_cast<float>(TraceChar->GetVelocity().Size());
			S.MaxSpeed = FMath::Max(S.MaxSpeed, Speed);
			S.MovingFrames += (Speed > 50.f) ? 1 : 0;

			// Mesh vs capsule root: zero everywhere except on a machine that is SMOOTHING this pawn.
			if (const USkeletalMeshComponent* Mesh = TraceChar->GetMesh())
			{
				const FVector Unsmoothed = TraceChar->GetActorLocation()
					+ TraceChar->GetActorRotation().RotateVector(TraceChar->GetBaseTranslationOffset());
				const float Offset = static_cast<float>(FVector::Dist(Mesh->GetComponentLocation(), Unsmoothed));
				S.MaxMeshOffset = FMath::Max(S.MaxMeshOffset, Offset);
				S.SumMeshOffset += Offset;
			}
		}

		return bSawTrace;
	}

	void ClientTraceReport(const TCHAR* ArmName)
	{
		UWorld* World = FindTrailDebugWorld();
		const TCHAR* NetRole = TEXT("unknown");
		if (World != nullptr)
		{
			switch (World->GetNetMode())
			{
			case NM_Standalone:      NetRole = TEXT("standalone"); break;
			case NM_ListenServer:    NetRole = TEXT("HOST");       break;
			case NM_Client:          NetRole = TEXT("CLIENT");     break;
			case NM_DedicatedServer: NetRole = TEXT("dedicated");  break;
			default: break;
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[CLIENTTRACE] ===== arm '%s' netmode=%s orderFix=%d maxLen=%.0fuu spacing=%.0fuu. Per-FRAME sampling. "
			     "SEG = worst adjacent gap in the replicated path (healthy: a small multiple of the spacing; "
			     "a reordered path reads hundreds). BROKEN = frames with SEG>%.0f. "
			     "FAR = uu from the carrier to the FARTHEST VISIBLE ribbon piece - the reported symptom is "
			     "FAR collapsing while LEN stays long. HEADGAP = uu from the newest lethal point to the "
			     "carrier, along the path."),
			ArmName, NetRole, GClientOrderFix, UTraceTrailComponent::GetTraceMaxLengthUU(),
			UTraceSettings::Get().TrailPointSpacing, ClientTraceBrokenSegmentUU);

		for (const TPair<FString, FTraceClientTraceStats>& Pair : GClientTraceStats)
		{
			const FTraceClientTraceStats& S = Pair.Value;
			if (S.Samples == 0)
			{
				continue;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLIENTTRACE] %-8s %-24s frames=%4d localHuman=%d moving=%4d maxSpeed=%6.0f | "
				     "LEN %6.1f..%6.1f | TAIL %6.1f..%6.1f | SEG max=%7.1f BROKEN=%4d | "
				     "ORD max=%d frames=%4d FIX=%4d EQBIRTH max=%d frames=%4d | jump tail=%7.1f head=%7.1f | "
				     "FAR %6.1f..%6.1f NEARmax=%6.1f | HEADGAP avg=%6.1f max=%6.1f | "
				     "MESHOFF avg=%6.1f max=%6.1f"),
				ArmName, *Pair.Key, S.Samples, (S.LocalHumanFrames > 0) ? 1 : 0, S.MovingFrames, S.MaxSpeed,
				(S.MinLen == TNumericLimits<float>::Max()) ? -1.f : S.MinLen, S.MaxLen,
				(S.MinTail == TNumericLimits<float>::Max()) ? -1.f : S.MinTail, S.MaxTail,
				S.MaxSeg, S.BrokenPathFrames,
				S.MaxOrd, S.OrdFrames, S.Repairs, S.MaxEqualBirths, S.EqualBirthFrames,
				S.MaxTailJump, S.MaxHeadJump,
				(S.MinFarthest == TNumericLimits<float>::Max()) ? -1.f : S.MinFarthest, S.MaxFarthest,
				S.MaxNearest,
				static_cast<float>(S.SumHeadGap / FMath::Max(1, S.Samples)), S.MaxHeadGap,
				static_cast<float>(S.SumMeshOffset / FMath::Max(1, S.Samples)), S.MaxMeshOffset);
		}

		UE_LOG(LogTraceGame, Display, TEXT("[CLIENTTRACE] ===== END arm '%s' (%d traces)."),
			ArmName, GClientTraceStats.Num());
	}

	/**
	 * The A/B driver.
	 *
	 * TWO THINGS MAKE IT USABLE FROM -ExecCmds ON A HEADLESS CLIENT, and both were learned the hard way:
	 *
	 *   IT ARMS ITSELF. The window does not start when the command is typed (which is during engine
	 *   init, minutes before the client has joined anything) but on the FIRST FRAME A LIVE TRACE
	 *   EXISTS. A window that expired during map load would report a clean pass on zero samples, which
	 *   is worse than no measurement at all.
	 *
	 *   IT RUNS BOTH ARMS ITSELF, in one process, against the same match: arm 'ordfix0' with
	 *   Trace.Trail.ClientOrderFix forced to 0 (v7 §7's failure reproduced on demand — the reference
	 *   signature of a genuinely corrupted path) and then arm 'ordfix1' with the shipping default.
	 *   Any column on which the two arms agree is NOT explained by the v7 §7 reordering, and is
	 *   therefore where a second cause has to live.
	 */
	struct FClientTraceArmState
	{
		int32 ArmIndex = 0;
		double ArmEndTime = 0.0;
		float SecondsPerArm = 30.f;
		int32 RestoreOrderFix = 1;
		bool bArmed = false;
	};

	FAutoConsoleCommand CmdClientTrace(
		TEXT("Trace.Trail.ClientTrace"),
		TEXT("Trace.Trail.ClientTrace [SecondsPerArm=30] - record the replicated trace EVERY FRAME and print "
		     "the extremes: worst adjacent gap in the path, frames the path was broken, order violations, "
		     "frame-to-frame movement of both ends, and the distance from the carrier to the farthest "
		     "VISIBLE ribbon piece (spec v8 2). Runs TWO arms - ClientOrderFix 0 then 1 - against the same "
		     "match, and starts the clock on the first frame a trace exists, so it is safe from -ExecCmds. "
		     "Run it ON THE CLIENT."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			if (GClientTraceRecording)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[CLIENTTRACE] already recording."));
				return;
			}

			TSharedRef<FClientTraceArmState> State = MakeShared<FClientTraceArmState>();
			State->SecondsPerArm = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 1.f, 300.f) : 30.f;
			State->RestoreOrderFix = GClientOrderFix;

			GClientTraceStats.Reset();
			GClientTraceRecording = true;

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLIENTTRACE] armed: %.1fs per arm, two arms (ClientOrderFix 0 then 1). "
				     "The clock starts on the first frame a live trace exists."),
				State->SecondsPerArm);

			// Interval 0 = once per frame on the game thread, which is the whole point: the failure
			// this is looking for can be a single frame long.
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float /*DeltaTime*/) -> bool
				{
					const bool bSawTrace = ClientTraceSampleOneFrame();

					if (!State->bArmed)
					{
						if (!bSawTrace)
						{
							return true;   // Still loading / no carrier yet. Do not burn the window.
						}

						State->bArmed = true;
						GClientOrderFix = 0;
						GClientTraceStats.Reset();
						State->ArmEndTime = FPlatformTime::Seconds() + static_cast<double>(State->SecondsPerArm);
						UE_LOG(LogTraceGame, Display,
							TEXT("[CLIENTTRACE] first live trace seen - arm 'ordfix0' recording for %.1fs."),
							State->SecondsPerArm);
						return true;
					}

					if (FPlatformTime::Seconds() < State->ArmEndTime)
					{
						return true;
					}

					if (State->ArmIndex == 0)
					{
						ClientTraceReport(TEXT("ordfix0"));
						++State->ArmIndex;
						GClientOrderFix = 1;
						GClientTraceStats.Reset();
						State->ArmEndTime = FPlatformTime::Seconds() + static_cast<double>(State->SecondsPerArm);
						UE_LOG(LogTraceGame, Display,
							TEXT("[CLIENTTRACE] arm 'ordfix1' recording for %.1fs."), State->SecondsPerArm);
						return true;
					}

					ClientTraceReport(TEXT("ordfix1"));
					GClientOrderFix = State->RestoreOrderFix;
					GClientTraceRecording = false;
					UE_LOG(LogTraceGame, Display, TEXT("[CLIENTTRACE] ===== BOTH ARMS COMPLETE."));
					return false;
				}), 0.f);
		}));

	// =============================================================================================
	// Trace.Trail.OwnerHeadAB — SPEC v8 §2, THE HALF OF THE TRACE THAT ONLY A JOINED CARRIER SEES.
	//
	// EVERY OTHER PROBE IN THIS FILE MEASURES SOMEBODY ELSE'S TRACE. Trace.Trail.ClientTrace watches
	// the replicated point set of whoever happens to be carrying — in practice a bot, because an
	// unattended client has no hands. That covers the array, the ribbon and the far end, and it is how
	// the v7 §7 reordering was found. It is structurally blind to the one case the user is describing
	// in the words "the people connecting to the server": A HUMAN, ON A CLIENT, CARRYING THE CORE AND
	// LOOKING AT THEIR OWN TRACE. Nothing about that case is exercised by watching a bot, because the
	// piece of geometry it turns on — the owner-only predicted head — is not drawn for bots at all.
	//
	// So this harness drives the local player itself: forward input, a gentle steer that keeps them on
	// the pitch, and a dash on a timer, all through the same public entry points the input bindings
	// use (ATraceCharacter::DoMove / DoDash). Pair it with Trace.Parry.GiveCoreToRemote on the HOST,
	// which is what puts the Core in a joined client's hands.
	//
	// THE NUMBER IT EXISTS FOR IS 'DETACHED'. That is the count of frames on which the carrier was
	// moving, their trace had a real gap to cover, and the predicted head DECLINED TO DRAW — so their
	// own trace was visibly not attached to their body. On the host it is zero by construction. On a
	// client, at 40ms, with the pre-v8 straight chord and its 400uu cap, it is most of every dash.
	//
	// TWO ARMS, ONE PROCESS, ONE MATCH:
	//   'v5-chord400'  PredictHistory 0 + PredictMaxLength 400 — exactly what shipped before this pass.
	//   'v8-history'   the new default: the stub follows the carrier's own recorded path, capped at
	//                  the trace's own length.
	// Any column the two arms agree on is not explained by this change, and is where the next cause
	// would have to live.
	//
	// It STEERS AND DASHES A PAWN, so do not run it alongside -TraceTripTest or -TraceDashPitchTest —
	// the same conflict Trace.TestParry and Trace.Trail.TestHeadGap have with those harnesses.
	// =============================================================================================
	struct FOwnerHeadStats
	{
		int32 Frames = 0;
		int32 CarryingFrames = 0;
		int32 MovingFrames = 0;
		int32 DashingFrames = 0;

		/** THE SYMPTOM: moving, a gap to cover, and nothing drawn to cover it. */
		int32 DetachedFrames = 0;

		int32 DropsAtStart = -1;
		int32 DropsAtEnd = 0;

		double SumSeen = 0.0;
		float MaxSeen = 0.f;
		double SumSpan = 0.0;
		float MaxSpan = 0.f;
		double SumStub = 0.0;
		float MaxStub = 0.f;
		int32 MaxSamples = 0;
		float MaxSpeed = 0.f;

		/** Worst visible gap while genuinely at dash speed — the frame the player actually complains about. */
		float MaxSeenDashing = 0.f;
	};

	/** The one pawn a human is playing on this machine, carrier or not. */
	ATraceCharacter* FindLocalHumanPawn(UWorld* World)
	{
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		return (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	}

	/**
	 * Hold forward, curve gently, and dash on a timer.
	 *
	 * THE CURVE IS NOT DECORATION. A straight run is the one shape in which the pre-v8 chord is
	 * indistinguishable from the recorded path, so a straight-line harness would have measured the
	 * length cap and missed the corner-cutting entirely. The steer is also what keeps an unattended
	 * pawn off the endzone walls: past the box below it turns back toward the middle of the field,
	 * which is the same trick every other unattended harness in this project uses.
	 */
	void DriveLocalCarrier(ATraceCharacter* Pawn, double Elapsed, double& LastDashTime)
	{
		APlayerController* PC = (Pawn != nullptr) ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
		if (PC == nullptr || !PC->IsLocalPlayerController())
		{
			return;
		}

		const FVector Location = Pawn->GetActorLocation();
		float Yaw = PC->GetControlRotation().Yaw;

		if (FMath::Abs(Location.X) > 12000.0 || FMath::Abs(Location.Y) > 3200.0)
		{
			FVector TowardCentre = -Location;
			TowardCentre.Z = 0.0;
			if (TowardCentre.Normalize())
			{
				Yaw = static_cast<float>(TowardCentre.Rotation().Yaw);
			}
		}
		else
		{
			Yaw += 35.f * static_cast<float>(FMath::Sin(Elapsed * 0.9)) * 0.016f;
		}

		PC->SetControlRotation(FRotator(0.f, Yaw, 0.f));
		Pawn->DoMove(FVector2D(0.f, 1.f));   // X = strafe, Y = forward

		// Every 2.5s: comfortably longer than a dash plus its cooldown, so a refused dash is a real
		// refusal rather than the harness pressing while the charge is still recharging.
		if ((Elapsed - LastDashTime) > 2.5)
		{
			LastDashTime = Elapsed;
			Pawn->DoDash();
		}
	}

	void OwnerHeadSample(ATraceCharacter* Pawn, FOwnerHeadStats& S)
	{
		const UTraceTrailComponent* Trail = (Pawn != nullptr) ? Pawn->Trail : nullptr;
		if (Trail == nullptr)
		{
			return;
		}

		++S.Frames;

		const float Speed = static_cast<float>(Pawn->GetVelocity().Size());
		S.MaxSpeed = FMath::Max(S.MaxSpeed, Speed);
		S.MovingFrames += (Speed > 50.f) ? 1 : 0;
		S.DashingFrames += (Speed > 1500.f) ? 1 : 0;

		if (S.DropsAtStart < 0)
		{
			S.DropsAtStart = Trail->GetPredictedHeadDropCount();
		}
		S.DropsAtEnd = Trail->GetPredictedHeadDropCount();

		if (!Pawn->IsCarrier() || Trail->ComputeLastLethalIndex() < 0)
		{
			return;   // Nothing to be attached to yet. Not a detachment.
		}

		++S.CarryingFrames;

		const float Span = Trail->GetPredictedHeadSpan();
		const float Stub = Trail->GetPredictedHeadLength();
		const float Seen = Trail->MeasureOwnerVisibleGap(/*bIncludePredictedHead=*/true);

		S.SumSpan += Span;
		S.MaxSpan = FMath::Max(S.MaxSpan, Span);
		S.SumStub += Stub;
		S.MaxStub = FMath::Max(S.MaxStub, Stub);
		S.MaxSamples = FMath::Max(S.MaxSamples, Trail->GetPredictedHeadSamplesUsed());

		if (Seen >= 0.f)
		{
			S.SumSeen += Seen;
			S.MaxSeen = FMath::Max(S.MaxSeen, Seen);
			if (Speed > 1500.f)
			{
				S.MaxSeenDashing = FMath::Max(S.MaxSeenDashing, Seen);
			}
		}

		// DETACHED: moving, more than one point spacing of trace to cover, and no stub covering it.
		// One spacing rather than zero because the gap is never zero — the head point is laid at
		// intervals — and a gap smaller than the interval is the trace being normal, not detached.
		const double Threshold = FMath::Max(30.0, static_cast<double>(UTraceSettings::Get().TrailPointSpacing));
		if (Speed > 50.f && Span > Threshold && Stub <= 0.f)
		{
			++S.DetachedFrames;
		}
	}

	void OwnerHeadReport(const TCHAR* ArmName, const FOwnerHeadStats& S)
	{
		const int32 Carrying = FMath::Max(1, S.CarryingFrames);

		UE_LOG(LogTraceGame, Display,
			TEXT("[OWNERHEAD] %-12s frames=%4d carrying=%4d moving=%4d dashing=%4d maxSpeed=%6.0f | "
			     "DETACHED=%4d (%5.1f%% of carrying) drops=%3d | SPAN avg=%6.1f max=%7.1f | "
			     "STUB avg=%6.1f max=%7.1f samples<=%2d | SEEN avg=%6.1f max=%7.1f maxWhileDashing=%7.1f"),
			ArmName, S.Frames, S.CarryingFrames, S.MovingFrames, S.DashingFrames, S.MaxSpeed,
			S.DetachedFrames, 100.f * static_cast<float>(S.DetachedFrames) / static_cast<float>(Carrying),
			FMath::Max(0, S.DropsAtEnd - FMath::Max(0, S.DropsAtStart)),
			static_cast<float>(S.SumSpan / Carrying), S.MaxSpan,
			static_cast<float>(S.SumStub / Carrying), S.MaxStub, S.MaxSamples,
			static_cast<float>(S.SumSeen / Carrying), S.MaxSeen, S.MaxSeenDashing);
	}

	struct FOwnerHeadABState
	{
		int32 ArmIndex = 0;
		bool bArmed = false;
		double StartSeconds = 0.0;
		double ArmEndTime = 0.0;
		double LastDashTime = -10.0;
		float SecondsPerArm = 20.f;

		int32 RestoreUseHistory = 1;
		float RestoreMaxLength = 1200.f;

		FOwnerHeadStats Stats;
	};

	FAutoConsoleCommand CmdOwnerHeadAB(
		TEXT("Trace.Trail.OwnerHeadAB"),
		TEXT("Trace.Trail.OwnerHeadAB [SecondsPerArm=20] - drive the LOCAL player forward, curving, "
		     "dashing on a timer, and record every frame whether their own trace stayed attached to "
		     "their body (spec v8 2). Two arms in one match: 'v5-chord400' (the pre-v8 straight chord "
		     "and its 400uu cap) then 'v8-history'. DETACHED is the symptom. RUN IT ON THE CLIENT, with "
		     "Trace.Parry.GiveCoreToRemote on the host. Steers a pawn - do not combine with "
		     "-TraceTripTest or -TraceDashPitchTest."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			TSharedRef<FOwnerHeadABState> State = MakeShared<FOwnerHeadABState>();
			State->SecondsPerArm = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 2.f, 300.f) : 20.f;
			State->RestoreUseHistory = GPredictedHeadUseHistory;
			State->RestoreMaxLength = GPredictedHeadMaxLength;

			UE_LOG(LogTraceGame, Display,
				TEXT("[OWNERHEAD] armed: %.1fs per arm. The clock starts on the first frame the local "
				     "player is CARRYING, so a slow join cannot burn the window. DETACHED = frames the "
				     "carrier was moving with a real gap and no stub drawn to cover it."),
				State->SecondsPerArm);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float /*DeltaTime*/) -> bool
				{
					UWorld* World = FindTrailDebugWorld();
					ATraceCharacter* Pawn = FindLocalHumanPawn(World);
					if (Pawn == nullptr || !Pawn->IsAlive())
					{
						return true;   // Still loading, or dead and waiting on a respawn.
					}

					const double Now = FPlatformTime::Seconds();
					if (!State->bArmed)
					{
						// Drive from the moment there is a pawn: the local player has to be moving
						// BEFORE they are handed the Core, or the first seconds of the first arm are a
						// standing carrier, which has no gap to cover and nothing to measure.
						DriveLocalCarrier(Pawn, Now, State->LastDashTime);

						if (!Pawn->IsCarrier())
						{
							return true;
						}

						State->bArmed = true;
						State->ArmEndTime = Now + static_cast<double>(State->SecondsPerArm);
						State->Stats = FOwnerHeadStats();
						GPredictedHeadUseHistory = 0;
						GPredictedHeadMaxLength = 400.f;

						UE_LOG(LogTraceGame, Display,
							TEXT("[OWNERHEAD] local player is carrying - arm 'v5-chord400' recording for %.1fs "
							     "(netmode=%d)."),
							State->SecondsPerArm, static_cast<int32>(World->GetNetMode()));
						return true;
					}

					DriveLocalCarrier(Pawn, Now, State->LastDashTime);
					OwnerHeadSample(Pawn, State->Stats);

					if (Now < State->ArmEndTime)
					{
						return true;
					}

					if (State->ArmIndex == 0)
					{
						OwnerHeadReport(TEXT("v5-chord400"), State->Stats);
						++State->ArmIndex;
						State->Stats = FOwnerHeadStats();
						State->ArmEndTime = Now + static_cast<double>(State->SecondsPerArm);
						GPredictedHeadUseHistory = 1;
						GPredictedHeadMaxLength = 1200.f;
						UE_LOG(LogTraceGame, Display,
							TEXT("[OWNERHEAD] arm 'v8-history' recording for %.1fs."), State->SecondsPerArm);
						return true;
					}

					OwnerHeadReport(TEXT("v8-history"), State->Stats);
					GPredictedHeadUseHistory = State->RestoreUseHistory;
					GPredictedHeadMaxLength = State->RestoreMaxLength;
					UE_LOG(LogTraceGame, Display, TEXT("[OWNERHEAD] ===== BOTH ARMS COMPLETE."));
					return false;
				}), 0.f);
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
		 *   * spec v3 §3, THE PARRY. 0.2s (v8 §3), carrier-only. Trace.TestParry already models this one;
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

	// =============================================================================================
	// SPEC v9 §§3-4 — THE TWO PROBES FOR "THE TRACE MUST BE GONE THE INSTANT POSSESSION LEAVES".
	//
	// Trace.Trail.ClearWatch   PASSIVE. Watches every trail in the match, every frame, and reports
	//                          any that is holding points or drawing geometry while its owner is not
	//                          a live carrier. Runs on the HOST or on a JOINED CLIENT, because "the
	//                          trace disappeared" is a claim about what a player SEES and the host is
	//                          the one machine that cannot answer it. This is the §4 measurement.
	//
	// Trace.Trail.ClearAudit   ACTIVE, server-side. Forces the exact reported event — a completed
	//                          pass to a LIVING TEAMMATE — and then measures two things about the
	//                          passer's trace: how long it takes to disappear, and whether it can
	//                          still kill them while it lingers. This is the §3 measurement, and the
	//                          kill half is the unfair death in the user's own words.
	//
	// BOTH ARE WRITTEN TO GO RED. Pair either with `Trace.Trail.ClearOnPossessionLoss 0` and the
	// pre-v9 behaviour comes back, so a run can show the symptom and then show it fixed on one build.
	// A harness that has never printed a failure is not evidence that there is nothing to fail.
	// =============================================================================================

	/** One trail's current orphan episode: it has trace, and its owner does not have the Core. */
	struct FTraceOrphanEpisode
	{
		double StartSeconds = 0.0;
		double LastSeenSeconds = 0.0;
		int32 MaxPoints = 0;
		int32 MaxLethal = 0;
		int32 MaxVisiblePieces = 0;
		bool bOwnerAlive = false;
		bool bReported = false;
	};

	/**
	 * NUDGE THE CARRIER SO THERE IS A TRACE TO TEST AT ALL.
	 *
	 * WHY THIS EXISTS, and it is not a convenience. Both §§3-4 probes need a carrier who has actually
	 * laid some trace, and the first arm of this pass sat idle for two minutes waiting for one. The
	 * new idle diagnostic named the reason on its first run: "1 living carrier(s), best trace 1
	 * points". Spec v9 §1 is the reason — bots "run at the carrier ... and stand by it" — so in an
	 * unattended match the Core ends up on a bot who barely moves, and the trace is length-based
	 * (v7 §1), so a carrier who does not move lays nothing. The clearing bug cannot be measured
	 * because the fixture never forms.
	 *
	 * It drives the carrier with AddMovementInput on a slowly rotating heading — the SAME input path
	 * a player uses, through the real movement component, so the trace laid is a real trace at real
	 * point spacing on a wide arc that does not simply run into a wall. Nothing is teleported: a
	 * teleport longer than MaxTrailSegmentLength makes ServerUpdateTrail restart the trace, which
	 * would quietly produce the short traces this is here to avoid.
	 *
	 * It only ever runs while a probe is WAITING for a setup, and it stops the moment one exists, so
	 * it cannot influence what happens after a route fires.
	 */
	void ClearHarnessDriveCarrier(const TArray<ATraceCharacter*>& Characters, int32 Frames)
	{
		for (ATraceCharacter* TraceChar : Characters)
		{
			if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive())
			{
				const float Angle = static_cast<float>(Frames) * 0.015f;
				TraceChar->AddMovementInput(FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.f), 1.f);
				return;
			}
		}
	}

	struct FTraceClearWatchState
	{
		float Seconds = 20.f;
		double Elapsed = 0.0;
		int32 Frames = 0;

		TMap<TWeakObjectPtr<ATraceCharacter>, FTraceOrphanEpisode> Live;

		int32 Episodes = 0;

		/** Episodes that lasted longer than one packet's worth of disagreement. These are the failures. */
		int32 LongEpisodes = 0;

		double WorstSeconds = 0.0;
		FString WorstOwner;
		int32 WorstPoints = 0;
		int32 WorstVisible = 0;

		/** Episodes still open when the window closed — i.e. traces that never went away at all. */
		int32 UnendedEpisodes = 0;

		/** Frames on which at least one orphan existed. The share of the match that is wrong. */
		int32 OrphanFrames = 0;

		bool bIsClient = false;
	};

	/**
	 * Closes one episode into the running worst-case, and prints it.
	 *
	 * An episode is only interesting if it lasted longer than a packet. On a joined client the
	 * possession change (a replicated bool on the pawn) and the point removal (a fast-array delta on
	 * the component) travel in different property blocks, so a few tens of milliseconds of disagreement
	 * is the network being a network and not a bug. The threshold is stated rather than hidden: the
	 * user's report is "it takes a second", and the pre-v9 behaviour is unbounded, so anything this
	 * pass cares about is orders of magnitude clear of it.
	 */
	constexpr double TraceOrphanReportThresholdSeconds = 0.12;

	void CloseOrphanEpisode(FTraceClearWatchState& State, const FString& OwnerName,
		const FTraceOrphanEpisode& Episode, bool bStillOpen)
	{
		const double Duration = FMath::Max(0.0, Episode.LastSeenSeconds - Episode.StartSeconds);

		// EVERY episode counts towards the worst case and the census. Only LONG ones are printed and
		// only long ones fail the run — but the first arm of this harness produced a four-frame episode
		// that the old early-return made invisible in every number, and an unreported measurement is
		// how a real symptom gets described as "nothing to see".
		++State.Episodes;
		if (Duration > State.WorstSeconds)
		{
			State.WorstSeconds = Duration;
			State.WorstOwner = OwnerName;
			State.WorstPoints = Episode.MaxPoints;
			State.WorstVisible = Episode.MaxVisiblePieces;
		}

		if (Duration < TraceOrphanReportThresholdSeconds && !bStillOpen)
		{
			return;
		}

		++State.LongEpisodes;
		if (bStillOpen)
		{
			++State.UnendedEpisodes;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[CLEARWATCH] ORPHAN: %s held a trace for %.2fs while NOT holding the Core "
			     "(ownerAlive=%d, up to %d points / %d lethal / %d drawn pieces)%s"),
			*OwnerName, Duration, Episode.bOwnerAlive ? 1 : 0,
			Episode.MaxPoints, Episode.MaxLethal, Episode.MaxVisiblePieces,
			bStillOpen ? TEXT(" — AND IT WAS STILL THERE WHEN THE WATCH ENDED.") : TEXT("."));
	}

	bool TickClearWatch(FTraceClearWatchState& State, float DeltaTime)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		State.Elapsed += DeltaTime;
		++State.Frames;

		const bool bFinished = State.Elapsed >= State.Seconds;

		TArray<ATraceCharacter*> Characters;
		GatherTrailDebugCharacters(World, Characters);

		TSet<TWeakObjectPtr<ATraceCharacter>> SeenThisFrame;
		int32 OrphansThisFrame = 0;

		for (ATraceCharacter* TraceChar : Characters)
		{
			if (TraceChar == nullptr || TraceChar->Trail == nullptr)
			{
				continue;
			}

			UTraceTrailComponent* Trail = TraceChar->Trail;

			int32 StaticVisible = 0;
			int32 SkinnedVisible = 0;
			Trail->CountDrawnPieces(StaticVisible, SkinnedVisible);

			const int32 Points = Trail->TrailPoints.Items.Num();
			const int32 Visible = StaticVisible + SkinnedVisible;
			const bool bHasTrace = (Points > 0) || (Visible > 0);

			// THE DEFINITION OF AN ORPHAN, and it is deliberately readable on a client: bIsCarrier is
			// replicated on the pawn, so a joining player's machine can answer "does this trace belong
			// to somebody who is holding the Core" for itself. Everything else here is local state.
			const bool bOwnsTheCore = TraceChar->IsCarrier() && TraceChar->IsAlive();

			if (!bHasTrace || bOwnsTheCore)
			{
				if (const FTraceOrphanEpisode* Existing = State.Live.Find(TraceChar))
				{
					CloseOrphanEpisode(State, GetNameSafe(TraceChar), *Existing, /*bStillOpen=*/false);
					State.Live.Remove(TraceChar);
				}
				continue;
			}

			++OrphansThisFrame;
			SeenThisFrame.Add(TraceChar);

			FTraceOrphanEpisode& Episode = State.Live.FindOrAdd(TraceChar);
			if (Episode.StartSeconds <= 0.0)
			{
				Episode.StartSeconds = State.Elapsed;
			}
			Episode.LastSeenSeconds = State.Elapsed;
			Episode.MaxPoints = FMath::Max(Episode.MaxPoints, Points);
			Episode.MaxLethal = FMath::Max(Episode.MaxLethal, Trail->ComputeLastLethalIndex() + 1);
			Episode.MaxVisiblePieces = FMath::Max(Episode.MaxVisiblePieces, Visible);
			Episode.bOwnerAlive = TraceChar->IsAlive();
		}

		if (OrphansThisFrame > 0)
		{
			++State.OrphanFrames;
		}

		// A pawn that was destroyed (respawn, disconnect) ends its episode too — its trace went with it.
		for (auto It = State.Live.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				CloseOrphanEpisode(State, TEXT("<destroyed pawn>"), It.Value(), /*bStillOpen=*/false);
				It.RemoveCurrent();
			}
		}

		if (!bFinished)
		{
			return true;
		}

		for (auto It = State.Live.CreateIterator(); It; ++It)
		{
			const ATraceCharacter* TraceChar = It.Key().Get();
			CloseOrphanEpisode(State, GetNameSafe(TraceChar), It.Value(), /*bStillOpen=*/true);
		}
		State.Live.Reset();

		const float OrphanFrameShare = (State.Frames > 0)
			? (100.f * static_cast<float>(State.OrphanFrames) / static_cast<float>(State.Frames)) : 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("[CLEARWATCH] DONE on the %s after %.1fs / %d frames. episodes=%d, of which %d lasted "
			     "longer than %.0fms (still standing at the end: %d). worst=%.2fs by %s (%d points, %d drawn "
			     "pieces) orphanFrames=%d (%.1f%%). Trace kills on a NON-carrier this session: %d of %d. "
			     "VERDICT: %s"),
			State.bIsClient ? TEXT("CLIENT") : TEXT("HOST"),
			State.Elapsed, State.Frames, State.Episodes, State.LongEpisodes,
			TraceOrphanReportThresholdSeconds * 1000.0, State.UnendedEpisodes,
			State.WorstSeconds, State.WorstOwner.IsEmpty() ? TEXT("nobody") : *State.WorstOwner,
			State.WorstPoints, State.WorstVisible, State.OrphanFrames, OrphanFrameShare,
			GTrailKillsOnNonCarrier, GTrailKillsTotal,
			(State.LongEpisodes == 0 && GTrailKillsOnNonCarrier == 0)
				? TEXT("PASS — no trace outlived its owner's possession, and nobody died to one.")
				: TEXT("*** FAIL — spec v9 3-4: a trace outlived possession. ***"));

		return false;
	}

	FAutoConsoleCommand CmdClearWatch(
		TEXT("Trace.Trail.ClearWatch"),
		TEXT("Trace.Trail.ClearWatch [Seconds] — spec v9 3-4. Watch every trail every frame and report any "
		     "that keeps points or drawn geometry while its owner is not holding the Core. RUN IT ON A "
		     "CLIENT as well as the host."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FTraceClearWatchState State;
			State.Seconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atof(*Args[0]), 1.f, 600.f) : 20.f;

			if (const UWorld* World = FindTrailDebugWorld())
			{
				State.bIsClient = (World->GetNetMode() == NM_Client);
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARWATCH] watching every trail for %.1fs on the %s. An ORPHAN is a trace whose owner "
				     "is not a living Core carrier; spec v9 3-4 says there must be none."),
				State.Seconds, State.bIsClient ? TEXT("CLIENT") : TEXT("HOST"));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickClearWatch(State, DeltaTime);
				}), 0.f);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.ClearAudit — the ACTIVE half. Forces the reported event and times the consequence.
	// ---------------------------------------------------------------------------------------------

	struct FClearAuditState
	{
		int32 TotalRuns = 3;
		int32 RunIndex = 0;
		int32 Phase = 0;
		int32 IdleFrames = 0;
		int32 PhaseFrames = 0;

		TWeakObjectPtr<ATraceCharacter> Passer;
		TWeakObjectPtr<ATraceCharacter> Receiver;
		TWeakObjectPtr<ATraceCharacter> Dasher;
		FVector DashEnd = FVector::ZeroVector;
		FVector DasherHome = FVector::ZeroVector;

		double PassSeconds = 0.0;
		double Elapsed = 0.0;

		int32 PointsAtPass = 0;
		int32 PointsAfterPassFrame = 0;
		int32 LethalAfterPassFrame = 0;
		double SecondsToClear = -1.0;

		/**
		 * Was the passer still alive when their trace finally went away?
		 *
		 * IT DECIDES WHETHER THE CLEAR TIME IS EVIDENCE AT ALL, and the first run of this harness
		 * proved why the field is needed: the lethality probe dashed an enemy through the abandoned
		 * trace, the trace killed the passer, and DEATH cleared the trace 37ms after the pass. Scored
		 * naively that reads "cleared instantly — PASS", which is the exact shape of the verification
		 * failure spec v9 §0 is about: a number that looks green because the bug fired.
		 */
		bool bClearedWhileAlive = false;

		bool bDashIssued = false;
		bool bPasserDied = false;

		/** Persistence runs (the odd ones) leave the passer alone; see bDashProbeThisRun. */
		int32 PersistenceRuns = 0;
		int32 PersistenceInstant = 0;
		int32 PersistenceLate = 0;
		int32 PersistenceNever = 0;

		/** Lethality runs (the even ones) dash an enemy through whatever is left. */
		int32 LethalityRuns = 0;
		int32 LethalityNoTraceToDash = 0;
		int32 UnfairKills = 0;

		int32 Aborted = 0;

		double WorstClearSeconds = 0.0;
	};

	/**
	 * TWO PROBES, ALTERNATED, BECAUSE THEY DESTROY EACH OTHER'S MEASUREMENT.
	 *
	 * EVEN runs are the LETHALITY probe (spec v9 §3's "they can still be killed"): dash an enemy
	 * through the abandoned trace and see whether the passer dies. It answers the important half —
	 * and it ends the run early, because the death clears the trace.
	 *
	 * ODD runs are the PERSISTENCE probe (§4's "traces stay on the map"): pass, then touch nothing,
	 * and time how long the trace survives with its owner alive and not carrying. Measured on the
	 * first arm of this harness: with the lethality probe running, the longest orphan the passive
	 * watch could see was FOUR FRAMES, because the bug killed its own witness.
	 */
	bool ClearAuditIsDashRun(int32 RunIndex) { return (RunIndex % 2) == 0; }

	/** How long a run waits for the passer's trace to vanish before calling it immortal. */
	constexpr double ClearAuditWatchSeconds = 4.0;

	/**
	 * "Instant", AND IT IS COUNTED IN FRAMES, NOT SECONDS.
	 *
	 * The clear happens inside the same call stack as the possession change, so the honest expectation
	 * on the authority is ZERO frames: the audit samples on the very next tick and the points must
	 * already be gone. That is what PointsAfterPassFrame records, and it is the pass criterion.
	 *
	 * A WALL-CLOCK THRESHOLD WAS THE FIRST VERSION AND IT WAS THE WRONG INSTRUMENT — spec v9 §0's
	 * mistake in miniature, a number that can go red for a reason that has nothing to do with the
	 * claim. These runs share a machine with other agents' editors; the green arm's first run sampled
	 * its "next frame" 177ms after the pass because the host was managing six frames a second. Points
	 * 16 -> 0 in one frame is a perfect result and 0.177s would have scored it as a LATE clear. The
	 * seconds are still printed, because a reader wants to know how long the frame was, but nothing
	 * is judged on them.
	 *
	 * (There is deliberately no ClearAuditInstantSeconds constant any more. Leaving one defined but
	 * unjudged is how a reader concludes the seconds still decide something.)
	 */

	bool TickClearAudit(FClearAuditState& State, float DeltaTime)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		if (World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CLEARAUDIT] this drives possession and must run on the SERVER. Use "
				     "Trace.Trail.ClearWatch on a client instead."));
			return false;
		}

		State.Elapsed += DeltaTime;
		++State.PhaseFrames;

		if (State.RunIndex >= State.TotalRuns)
		{
			const bool bPersistenceOk = (State.PersistenceRuns > 0)
				&& (State.PersistenceInstant == State.PersistenceRuns);
			const bool bLethalityOk = (State.LethalityRuns > 0)
				&& (State.LethalityNoTraceToDash == State.LethalityRuns) && (State.UnfairKills == 0);

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARAUDIT] DONE. PERSISTENCE (4): %d runs — %d cleared ON THE VERY NEXT FRAME with the "
				     "passer alive, %d cleared LATE, %d NEVER cleared; worst %.2fs. LETHALITY (3): %d runs — %d "
				     "had NO TRACE LEFT to dash through, %d killed the passer AFTER they had passed. "
				     "(%d aborted.) Session: %d of %d trace kills landed on a NON-carrier. VERDICT: %s"),
				State.PersistenceRuns, State.PersistenceInstant,
				State.PersistenceLate, State.PersistenceNever, State.WorstClearSeconds,
				State.LethalityRuns, State.LethalityNoTraceToDash, State.UnfairKills, State.Aborted,
				GTrailKillsOnNonCarrier, GTrailKillsTotal,
				(bPersistenceOk && bLethalityOk)
					? TEXT("PASS — the trace was gone the instant the Core left, and there was nothing left "
					       "to kill the passer with.")
					: ((State.PersistenceRuns == 0 && State.LethalityRuns == 0)
						? TEXT("NO DATA — no pass was ever set up.")
						: TEXT("*** FAIL — spec v9 3-4: the trace outlived the pass. ***")));
			return false;
		}

		const bool bDashProbeThisRun = ClearAuditIsDashRun(State.RunIndex);

		// ---- phase 0: a living carrier with real trace, and a living teammate to pass to ----------
		if (State.Phase == 0)
		{
			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);

			ATraceCharacter* Passer = nullptr;
			for (ATraceCharacter* TraceChar : Characters)
			{
				if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive()
					&& TraceChar->Trail != nullptr && TraceChar->Trail->TrailPoints.Items.Num() >= 6)
				{
					Passer = TraceChar;
					break;
				}
			}

			ATraceCharacter* Receiver = nullptr;
			if (Passer != nullptr)
			{
				for (ATraceCharacter* TraceChar : Characters)
				{
					// A LIVING TEAMMATE, which is the whole point: the old orphan sweep fired only for a
					// dead or destroyed owner, so passing to somebody who is alive is exactly the case
					// that had no cleanup path at all (spec v9 §4's diagnosed lead).
					if (TraceChar != nullptr && TraceChar != Passer && TraceChar->IsAlive()
						&& TraceChar->GetTeam() != ETraceTeam::None
						&& TraceChar->GetTeam() == Passer->GetTeam())
					{
						Receiver = TraceChar;
						break;
					}
				}
			}

			if (Passer == nullptr || Receiver == nullptr)
			{
				// See ClearHarnessDriveCarrier: without this the Core sits on a stationary bot and no
				// trace is ever laid to test.
				ClearHarnessDriveCarrier(Characters, State.IdleFrames);

				// AN IDLE HARNESS MUST SAY WHY IT IS IDLE. The first arm of this run set up ONE pass and
				// then went quiet for two minutes, and the log could not distinguish "the match stopped
				// producing carriers" from "the ticker died" from "the probe is broken" — which is a
				// measurement that reports nothing and looks like a measurement. Roughly every four
				// seconds it now prints the census it is waiting on.
				if ((++State.IdleFrames % 240) == 0)
				{
					int32 Carriers = 0;
					int32 BestPoints = 0;
					FString BestName;
					for (const ATraceCharacter* TraceChar : Characters)
					{
						if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive()
							&& TraceChar->Trail != nullptr)
						{
							++Carriers;
							const int32 Points = TraceChar->Trail->TrailPoints.Items.Num();
							if (Points >= BestPoints)
							{
								BestPoints = Points;
								BestName = GetNameSafe(TraceChar);
							}
						}
					}

					UE_LOG(LogTraceGame, Display,
						TEXT("[CLEARAUDIT %d/%d] waiting %.1fs for a setup: %d characters, %d living carrier(s), "
						     "best trace %d points (%s) — need 6 and a living teammate."),
						State.RunIndex + 1, State.TotalRuns, State.IdleFrames * DeltaTime,
						Characters.Num(), Carriers, BestPoints,
						BestName.IsEmpty() ? TEXT("nobody carrying") : *BestName);
				}

				if (State.IdleFrames > 36000)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[CLEARAUDIT] gave up: never found a carrier with 6+ trace points and a living teammate."));
					return false;
				}
				return true;
			}

			ATraceCore* Core = nullptr;
			{
				// `if`, not a `for ... break`: clang's -Wunreachable-code-loop-increment is an error in
				// this project, and a loop whose body always breaks never reaches its increment.
				TActorIterator<ATraceCore> CoreIt(World);
				if (CoreIt)
				{
					Core = *CoreIt;
				}
			}
			if (Core == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[CLEARAUDIT] no ATraceCore in the world."));
				return false;
			}

			State.IdleFrames = 0;
			State.Passer = Passer;
			State.Receiver = Receiver;
			State.PointsAtPass = Passer->Trail->TrailPoints.Items.Num();
			State.PassSeconds = State.Elapsed;
			State.SecondsToClear = -1.0;
			State.bClearedWhileAlive = false;
			State.bDashIssued = false;
			State.bPasserDied = false;

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARAUDIT %d/%d] %s PROBE: %s is carrying with %d trace points; passing to LIVING "
				     "TEAMMATE %s now."),
				State.RunIndex + 1, State.TotalRuns,
				bDashProbeThisRun ? TEXT("LETHALITY") : TEXT("PERSISTENCE"),
				*GetNameSafe(Passer), State.PointsAtPass, *GetNameSafe(Receiver));

			// THE EVENT. GrantTo is the single funnel every possession change goes through — a real
			// completed pass ends here, so this reproduces it exactly rather than approximating it.
			Core->GrantTo(Receiver, ETraceCoreGrantReason::Pass);

			State.Phase = 1;
			State.PhaseFrames = 0;
			return true;
		}

		ATraceCharacter* Passer = State.Passer.Get();
		if (Passer == nullptr || Passer->Trail == nullptr)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[CLEARAUDIT %d] aborted: the passer's pawn went away."), State.RunIndex + 1);
			++State.Aborted;
			++State.RunIndex;
			State.Phase = 0;
			return true;
		}

		const int32 PointsNow = Passer->Trail->TrailPoints.Items.Num();

		// ---- phase 1: the very next frame after the pass ------------------------------------------
		if (State.Phase == 1)
		{
			State.PointsAfterPassFrame = PointsNow;
			State.LethalAfterPassFrame = Passer->Trail->ComputeLastLethalIndex() + 1;

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARAUDIT %d/%d] ONE FRAME AFTER THE PASS: %s isCarrier=%d, trace points %d -> %d, "
				     "lethal points %d, emitting=%d."),
				State.RunIndex + 1, State.TotalRuns, *GetNameSafe(Passer), Passer->IsCarrier() ? 1 : 0,
				State.PointsAtPass, State.PointsAfterPassFrame, State.LethalAfterPassFrame,
				Passer->Trail->IsEmitting() ? 1 : 0);

			State.Phase = 2;
			State.PhaseFrames = 0;
			return true;
		}

		// ---- phase 2: how long until it is actually gone, and can it kill while it lingers? -------
		if (State.Phase == 2)
		{
			if (PointsNow == 0 && State.SecondsToClear < 0.0)
			{
				State.SecondsToClear = State.Elapsed - State.PassSeconds;
				State.bClearedWhileAlive = Passer->IsAlive();
			}

			// THE UNFAIR DEATH, REPRODUCED. While the passer's abandoned trace still has lethal points,
			// dash an enemy through the middle of it and see whether the passer — who is not carrying
			// anything — dies. This is §3's "they can still be killed", performed rather than argued.
			const int32 LastLethal = Passer->Trail->ComputeLastLethalIndex();
			if (bDashProbeThisRun && !State.bDashIssued && LastLethal >= 1 && Passer->IsAlive())
			{
				const int32 SegmentIndex = FMath::Max(0, LastLethal / 2);
				const FVector SegmentStart = Passer->Trail->TrailPoints.Items[SegmentIndex].Location;
				const FVector SegmentEnd = Passer->Trail->TrailPoints.Items[SegmentIndex + 1].Location;

				FVector Along = SegmentEnd - SegmentStart;
				Along.Z = 0.0;
				if (Along.Normalize())
				{
					const FVector Across = FVector::CrossProduct(Along, FVector::UpVector).GetSafeNormal();
					const FVector Midpoint = (SegmentStart + SegmentEnd) * 0.5;

					TArray<ATraceCharacter*> Candidates;
					GatherTrailDebugCharacters(World, Candidates);

					for (ATraceCharacter* Candidate : Candidates)
					{
						if (Candidate != nullptr && Candidate != Passer && Candidate->IsAlive()
							&& Candidate->GetTeam() != ETraceTeam::None
							&& Candidate->GetTeam() != Passer->GetTeam())
						{
							State.Dasher = Candidate;
							State.DasherHome = Candidate->GetActorLocation();
							State.DashEnd = Midpoint - Across * 240.0;

							Candidate->SetActorLocation(Midpoint + Across * 260.0, /*bSweep=*/false, nullptr,
								ETeleportType::TeleportPhysics);
							Candidate->SetActorRotation((-Across).Rotation());
							if (UTraceCharacterMovementComponent* Movement = Candidate->GetTraceMovement())
							{
								Movement->StartDash();
							}

							State.bDashIssued = true;
							UE_LOG(LogTraceGame, Display,
								TEXT("[CLEARAUDIT %d/%d] %s's trace STILL HAS %d lethal points after the pass — "
								     "dashing %s through it to see whether it kills a player who is not carrying."),
								State.RunIndex + 1, State.TotalRuns, *GetNameSafe(Passer), LastLethal + 1,
								*GetNameSafe(Candidate));
							break;
						}
					}
				}
			}

			if (State.bDashIssued && State.Dasher.IsValid() && State.PhaseFrames > 1)
			{
				// One teleport across, exactly as the head-gap harness does it, then home again.
				ATraceCharacter* Dasher = State.Dasher.Get();
				if (Dasher->IsDashing() && !State.DashEnd.IsZero())
				{
					Dasher->SetActorLocation(State.DashEnd, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
					State.DashEnd = FVector::ZeroVector;
				}
				else if (State.DashEnd.IsZero())
				{
					Dasher->SetActorLocation(State.DasherHome, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
					State.Dasher = nullptr;
				}
			}

			if (!Passer->IsAlive())
			{
				State.bPasserDied = true;
			}

			const bool bTimeUp = (State.Elapsed - State.PassSeconds) >= ClearAuditWatchSeconds;
			if (State.SecondsToClear >= 0.0 || bTimeUp)
			{
				if (ATraceCharacter* Dasher = State.Dasher.Get())
				{
					Dasher->SetActorLocation(State.DasherHome, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
				}
				State.Dasher = nullptr;
				State.Phase = 3;
				State.PhaseFrames = 0;
			}
			return true;
		}

		// ---- phase 3: score --------------------------------------------------------------------
		{
			const double Clear = State.SecondsToClear;
			const bool bCleared = Clear >= 0.0;

			// A clear the passer did not live to see is not a clear: their DEATH wiped the trace, which
			// is the pre-v9 behaviour working exactly as it always did. Only a clear that happened while
			// they were standing there alive says anything about possession.
			//
			// THE FRAME READING IS THE CRITERION, not the elapsed seconds — see the "Instant" note above.
			// PointsAfterPassFrame is sampled on the first tick after the pass and must already be zero.
			const bool bInstant = bCleared && State.bClearedWhileAlive && (State.PointsAfterPassFrame == 0);

			if (bDashProbeThisRun)
			{
				++State.LethalityRuns;

				// The pass condition for the lethality probe is that there was NOTHING LEFT TO DASH
				// THROUGH — the trace had already gone, so the probe could not even be set up. A run
				// that got to issue a dash has already failed §3 whatever the dash then did.
				if (!State.bDashIssued)
				{
					++State.LethalityNoTraceToDash;
				}
				if (State.bPasserDied)
				{
					++State.UnfairKills;
				}
			}
			else
			{
				++State.PersistenceRuns;
				if (!bCleared || !State.bClearedWhileAlive)
				{
					++State.PersistenceNever;
					State.WorstClearSeconds = FMath::Max(State.WorstClearSeconds, ClearAuditWatchSeconds);
				}
				else if (bInstant)
				{
					++State.PersistenceInstant;
					State.WorstClearSeconds = FMath::Max(State.WorstClearSeconds, Clear);
				}
				else
				{
					++State.PersistenceLate;
					State.WorstClearSeconds = FMath::Max(State.WorstClearSeconds, Clear);
				}
			}

			const bool bRunPassed = bDashProbeThisRun
				? (!State.bDashIssued && !State.bPasserDied)
				: bInstant;

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARAUDIT %d/%d] RESULT (%s): %s passed the Core away holding %d trace points. Points "
				     "one frame later: %d (lethal %d). Trace gone after: %s. Enemy able to dash the abandoned "
				     "trace: %s. Passer killed by their own abandoned trace: %s. %s"),
				State.RunIndex + 1, State.TotalRuns,
				bDashProbeThisRun ? TEXT("LETHALITY") : TEXT("PERSISTENCE"),
				*GetNameSafe(Passer), State.PointsAtPass,
				State.PointsAfterPassFrame, State.LethalAfterPassFrame,
				bCleared
					? *FString::Printf(TEXT("%.3fs%s"), Clear,
						State.bClearedWhileAlive ? TEXT("") : TEXT(" — BUT ONLY BECAUSE THE PASSER DIED"))
					: *FString::Printf(TEXT("NEVER (still standing %.0fs later)"), ClearAuditWatchSeconds),
				State.bDashIssued ? TEXT("YES") : TEXT("no - nothing left to dash"),
				State.bPasserDied ? TEXT("YES — THE UNFAIR DEATH") : TEXT("no"),
				bRunPassed
					? TEXT("PASS")
					: TEXT("*** FAIL — spec v9 3-4: the trace must be gone, visually and lethally, the "
					       "instant possession leaves ***"));

			++State.RunIndex;
			State.Phase = 0;
			State.PhaseFrames = 0;
			State.Passer = nullptr;
			State.Receiver = nullptr;
			return true;
		}
	}

	FAutoConsoleCommand CmdClearAudit(
		TEXT("Trace.Trail.ClearAudit"),
		TEXT("Trace.Trail.ClearAudit [Runs] — spec v9 3. SERVER. Force a completed pass to a LIVING teammate, "
		     "then measure how long the passer's trace survives and whether it can still kill them. Pair with "
		     "Trace.Trail.ClearOnPossessionLoss 0 to see the pre-v9 behaviour fail."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FClearAuditState State;
			State.TotalRuns = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 20) : 3;

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARAUDIT] starting %d forced passes. Spec v9 3: the passer's trace must be gone — "
				     "visually and lethally — the instant the Core leaves."),
				State.TotalRuns);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickClearAudit(State, DeltaTime);
				}), 0.f);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.ClearRoutes — EVERY WAY POSSESSION CAN END, FIRED ON PURPOSE, ONE AT A TIME.
	//
	// Spec v9 §4 lists eight of them: "pass completed, throw (mode B), carrier killed, carrier
	// disconnected, score, half-time, kickoff, match end". Trace.Trail.ClearAudit exercises exactly
	// one (the pass) and Trace.Trail.ClearWatch only sees the routes a bot match happens to produce —
	// in the first arm of this pass that was ONE possession change in sixty seconds, which is not
	// coverage, it is luck. The claim "all eight routes clear the trace" rests on ReleaseHolder()
	// being the single funnel, and an architectural claim that is never fired is exactly the kind of
	// thing that is true right up until somebody adds a ninth route. So each one is fired here and
	// the invariant is read back on the very next frame.
	//
	// Three of the eight collapse into one call BY DESIGN, and the collapse is the Core's, not this
	// harness's: ATraceCore::KickoffTo's own header says "every caller (a score, match start, half
	// time)" ends here, and ATraceGameMode has "exactly one function to call". Firing KickoffTo
	// therefore IS firing score / half-time / match-end / kickoff. It is labelled that way rather
	// than padded out into four identical rows.
	// ---------------------------------------------------------------------------------------------

	enum class EClearRoute : uint8
	{
		Pass = 0,        // §4 "pass completed" — to a LIVING teammate, the case the old gate missed.
		Turnover,        // an interception / kill steal: the Core crosses to the other team.
		Throw,           // §4 "throw (mode B)". Skipped with a stated reason in mode A.
		Kill,            // §4 "carrier killed".
		Kickoff,         // §4 "score", "half-time", "kickoff", "match end" — one call, see above.
		Disconnect,      // §4 "carrier disconnected": the pawn goes away underneath the trace.
		Count
	};

	const TCHAR* ClearRouteName(EClearRoute Route)
	{
		switch (Route)
		{
		case EClearRoute::Pass:       return TEXT("PASS COMPLETED (to a living teammate)");
		case EClearRoute::Turnover:   return TEXT("TURNOVER (Core crosses to the other team)");
		case EClearRoute::Throw:      return TEXT("THROW (mode B)");
		case EClearRoute::Kill:       return TEXT("CARRIER KILLED");
		case EClearRoute::Kickoff:    return TEXT("KICKOFF / SCORE / HALF-TIME / MATCH END");
		case EClearRoute::Disconnect: return TEXT("CARRIER DISCONNECTED (pawn destroyed)");
		default:                      return TEXT("?");
		}
	}

	struct FClearRoutesState
	{
		int32 RouteIndex = 0;
		int32 Phase = 0;
		int32 IdleFrames = 0;

		TWeakObjectPtr<ATraceCharacter> Subject;
		FString SubjectName;
		int32 PointsBefore = 0;

		int32 Fired = 0;
		int32 Passed = 0;
		int32 Skipped = 0;
		TArray<FString> Failures;
	};

	/** How long one route waits for a carrier with real trace before it gives up and says so. */
	constexpr int32 ClearRoutesSetupFrameBudget = 1800;

	bool TickClearRoutes(FClearRoutesState& State, float /*DeltaTime*/)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		if (World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CLEARROUTES] this drives possession and must run on the SERVER."));
			return false;
		}

		if (State.RouteIndex >= static_cast<int32>(EClearRoute::Count))
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARROUTES] DONE. %d of %d possession-end routes fired, %d cleared the trace on the "
				     "NEXT FRAME, %d skipped. Session: %d of %d trace kills landed on a NON-carrier. VERDICT: %s"),
				State.Fired, static_cast<int32>(EClearRoute::Count), State.Passed, State.Skipped,
				GTrailKillsOnNonCarrier, GTrailKillsTotal,
				(State.Failures.Num() == 0 && State.Fired > 0)
					? TEXT("PASS — no route left a trace behind.")
					: *FString::Printf(TEXT("*** FAIL — spec v9 4: %s ***"),
						*FString::Join(State.Failures, TEXT("; "))));
			return false;
		}

		const EClearRoute Route = static_cast<EClearRoute>(State.RouteIndex);

		ATraceCore* Core = nullptr;
		{
			// `if`, not `for ... break`: -Wunreachable-code-loop-increment is an error here.
			TActorIterator<ATraceCore> CoreIt(World);
			if (CoreIt)
			{
				Core = *CoreIt;
			}
		}
		if (Core == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[CLEARROUTES] no ATraceCore in the world."));
			return false;
		}

		// ---- phase 0: wait for a carrier who has actually laid some trace ------------------------
		if (State.Phase == 0)
		{
			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);

			ATraceCharacter* Subject = nullptr;
			for (ATraceCharacter* TraceChar : Characters)
			{
				if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive()
					&& TraceChar->Trail != nullptr && TraceChar->Trail->TrailPoints.Items.Num() >= 6)
				{
					Subject = TraceChar;
					break;
				}
			}

			// The two grant routes also need somebody to grant TO, and which side they are on is the
			// whole difference between them.
			ATraceCharacter* Target = nullptr;
			if (Subject != nullptr && (Route == EClearRoute::Pass || Route == EClearRoute::Turnover))
			{
				const bool bWantAlly = (Route == EClearRoute::Pass);
				for (ATraceCharacter* TraceChar : Characters)
				{
					if (TraceChar == nullptr || TraceChar == Subject || !TraceChar->IsAlive()
						|| TraceChar->GetTeam() == ETraceTeam::None)
					{
						continue;
					}
					const bool bAlly = (TraceChar->GetTeam() == Subject->GetTeam());
					if (bAlly == bWantAlly)
					{
						Target = TraceChar;
						break;
					}
				}
				if (Target == nullptr)
				{
					Subject = nullptr;
				}
			}

			if (Subject == nullptr)
			{
				// See ClearHarnessDriveCarrier.
				ClearHarnessDriveCarrier(Characters, State.IdleFrames);

				if ((++State.IdleFrames % 240) == 0)
				{
					int32 Carriers = 0;
					int32 BestPoints = 0;
					for (const ATraceCharacter* TraceChar : Characters)
					{
						if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive()
							&& TraceChar->Trail != nullptr)
						{
							++Carriers;
							BestPoints = FMath::Max(BestPoints, TraceChar->Trail->TrailPoints.Items.Num());
						}
					}
					UE_LOG(LogTraceGame, Display,
						TEXT("[CLEARROUTES] %s: waiting for a setup (%d frames): %d living carrier(s), best "
						     "trace %d points — need 6 and the partner this route requires."),
						ClearRouteName(Route), State.IdleFrames, Carriers, BestPoints);
				}

				if (State.IdleFrames > ClearRoutesSetupFrameBudget)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[CLEARROUTES] %s: SKIPPED — no carrier with 6+ trace points (and the partner it "
						     "needs) turned up in %d frames."),
						ClearRouteName(Route), ClearRoutesSetupFrameBudget);
					++State.Skipped;
					++State.RouteIndex;
					State.IdleFrames = 0;
					State.Phase = 0;
				}
				return true;
			}

			State.IdleFrames = 0;
			State.Subject = Subject;
			State.SubjectName = GetNameSafe(Subject);
			State.PointsBefore = Subject->Trail->TrailPoints.Items.Num();

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARROUTES] %s: %s is carrying with %d trace points (%d lethal). Firing the route now."),
				ClearRouteName(Route), *State.SubjectName, State.PointsBefore,
				Subject->Trail->ComputeLastLethalIndex() + 1);

			bool bFired = true;
			switch (Route)
			{
			case EClearRoute::Pass:
				Core->GrantTo(Target, ETraceCoreGrantReason::Pass);
				break;

			case EClearRoute::Turnover:
				Core->GrantTo(Target, ETraceCoreGrantReason::Kill);
				break;

			case EClearRoute::Throw:
				// Returns false for a wrong mode, a non-holder, a dead holder, an already-loose Core or
				// a throw on cooldown. Every one of those is "this route does not exist right now", and
				// saying so is better than scoring an untaken route as a pass.
				bFired = Core->ThrowFromHolder(Subject);
				if (!bFired)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[CLEARROUTES] %s: SKIPPED — ThrowFromHolder refused (mode A, or the throw was on "
						     "cooldown). Run this again with ScoringMode=ThrownCoreAndGoals to cover it."),
						ClearRouteName(Route));
				}
				break;

			case EClearRoute::Kill:
				if (Subject->Health != nullptr)
				{
					Subject->Health->Kill(nullptr, FName(TEXT("ClearRoutes")));
				}
				else
				{
					bFired = false;
				}
				break;

			case EClearRoute::Kickoff:
				Core->KickoffTo((Subject->GetTeam() == ETraceTeam::Blue) ? ETraceTeam::Orange : ETraceTeam::Blue);
				break;

			case EClearRoute::Disconnect:
				// LAST on purpose. This is the only route that takes the pawn with it, and a destroyed
				// pawn is a bigger disturbance to the rest of the match than anything else here.
				Subject->Destroy();
				break;

			default:
				bFired = false;
				break;
			}

			if (!bFired)
			{
				++State.Skipped;
				++State.RouteIndex;
				State.Phase = 0;
				return true;
			}

			++State.Fired;
			State.Phase = 1;
			return true;
		}

		// ---- phase 1: THE VERY NEXT FRAME. Nothing may be left. -----------------------------------
		{
			ATraceCharacter* Subject = State.Subject.Get();

			if (Subject == nullptr || Subject->Trail == nullptr)
			{
				// The disconnect route's pass condition: the pawn went, and its trail component — and
				// every mesh it had placed — went with it. There is nothing left to hold points.
				UE_LOG(LogTraceGame, Display,
					TEXT("[CLEARROUTES] %s: PASS — %s's pawn (and its trail component, and its %d points) no "
					     "longer exist one frame later."),
					ClearRouteName(Route), *State.SubjectName, State.PointsBefore);
				++State.Passed;
			}
			else
			{
				const int32 PointsAfter = Subject->Trail->TrailPoints.Items.Num();
				const int32 LethalAfter = Subject->Trail->ComputeLastLethalIndex() + 1;

				int32 StaticVisible = 0;
				int32 SkinnedVisible = 0;
				Subject->Trail->CountDrawnPieces(StaticVisible, SkinnedVisible);
				const int32 Drawn = StaticVisible + SkinnedVisible;

				// A LISTEN SERVER DRAWS, so "visually gone" is checkable right here and is checked:
				// §3 says the trace must be gone visually AND lethally, and points-only would pass a
				// build that emptied the array and left the ribbon standing.
				const bool bOk = (PointsAfter == 0) && (LethalAfter == 0) && !Subject->Trail->IsEmitting();

				UE_LOG(LogTraceGame, Display,
					TEXT("[CLEARROUTES] %s: %s ONE FRAME LATER — isCarrier=%d alive=%d points %d -> %d, lethal %d, "
					     "emitting=%d, drawn pieces %d. %s"),
					ClearRouteName(Route), *State.SubjectName, Subject->IsCarrier() ? 1 : 0,
					Subject->IsAlive() ? 1 : 0, State.PointsBefore, PointsAfter, LethalAfter,
					Subject->Trail->IsEmitting() ? 1 : 0, Drawn,
					bOk ? TEXT("PASS") : TEXT("*** FAIL — the trace outlived possession ***"));

				if (bOk)
				{
					++State.Passed;
				}
				else
				{
					State.Failures.Add(FString::Printf(TEXT("%s left %d points / %d lethal / %d drawn"),
						ClearRouteName(Route), PointsAfter, LethalAfter, Drawn));
				}
			}

			++State.RouteIndex;
			State.Phase = 0;
			State.Subject = nullptr;
			return true;
		}
	}

	FAutoConsoleCommand CmdClearRoutes(
		TEXT("Trace.Trail.ClearRoutes"),
		TEXT("Trace.Trail.ClearRoutes — spec v9 4. SERVER. Fire EVERY way possession can end (pass, turnover, "
		     "mode-B throw, carrier killed, kickoff/score/half-time/match-end, carrier disconnected) one at a "
		     "time against a real carrier, and assert the trace is gone — points, lethality and drawn "
		     "geometry — on the very next frame."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& /*Args*/)
		{
			FClearRoutesState State;

			UE_LOG(LogTraceGame, Display,
				TEXT("[CLEARROUTES] firing %d possession-end routes. Spec v9 4: not one of them may leave a "
				     "trace behind."),
				static_cast<int32>(EClearRoute::Count));

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickClearRoutes(State, DeltaTime);
				}), 0.f);
		}));


	// =============================================================================================
	// SPEC v10 §2 — WHOLE-MODEL TRIP DETECTION, MEASURED
	//
	//   Trace.Trail.ModelReach    — the readout. Capsule vs measured mesh vs effective, per pawn,
	//                               plus the horizontal threshold each of them produces and the
	//                               session's widened-trip share.
	//   Trace.Trail.ModelHitTest  — THE REPRODUCTION. Four cases on ONE build in ONE match: the
	//                               reported bug happening (capsule-only arm, kill side and parry
	//                               side both scoring nothing at an offset where the model is inside
	//                               the ribbon), then the same offset scoring on the whole-model arm,
	//                               with the CARRIER dying on the kill side and the DASHER dying on
	//                               the parry side.
	// =============================================================================================

	/** One pawn's numbers, in the terms the spec asks for them. */
	void LogModelReachLine(const ATraceCharacter* TraceChar, double TrailRadius)
	{
		if (TraceChar == nullptr)
		{
			return;
		}

		const FTraceModelReach Reach = UTraceTrailComponent::MeasureModelReach(TraceChar);

		UE_LOG(LogTraceGame, Display,
			TEXT("[MODELREACH] %-28s capsule r=%5.1f hh=%5.1f | mesh r=%5.1f hh=%5.1f (measured=%d) | "
			     "EFFECTIVE r=%5.1f hh=%5.1f | margin %+5.1f/%+5.1f uu | trip half-width %5.1f -> %5.1fuu "
			     "(+%.0f%%)"),
			*GetNameSafe(TraceChar),
			Reach.CapsuleRadius, Reach.CapsuleHalfHeight,
			Reach.RawMeshRadius, Reach.RawMeshHalfHeight, Reach.bMeshMeasured ? 1 : 0,
			Reach.EffectiveRadius, Reach.EffectiveHalfHeight,
			Reach.HorizontalMargin(), Reach.VerticalMargin(),
			TrailRadius + Reach.CapsuleRadius, TrailRadius + Reach.EffectiveRadius,
			100.0 * Reach.HorizontalMargin() / FMath::Max(1.0, TrailRadius + Reach.CapsuleRadius));
	}

	FAutoConsoleCommand CmdModelReach(
		TEXT("Trace.Trail.ModelReach"),
		TEXT("Trace.Trail.ModelReach — spec v10 2. Print, for every pawn, the capsule the trip test used "
		     "to use, the rendered mesh's measured extent, the effective reach after clamping, the margin "
		     "in uu, and the resulting scoring half-width. Also prints the session's widened-trip share — "
		     "the trace kills and parry punishes that exist only because of the widening."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& /*Args*/)
		{
			UWorld* World = FindTrailDebugWorld();
			if (World == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[MODELREACH] no game world."));
				return;
			}

			const double TrailRadius = static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius());
			const double TrailHeight = static_cast<double>(UTraceTrailComponent::GetTraceTrailHeight());

			UE_LOG(LogTraceGame, Display,
				TEXT("[MODELREACH] trace half-width %.1fuu, height %.1fuu. Arm=%d (Trace.Trail.WholeModelTrip), "
				     "margin floor %.1f, ceiling %.1f, fixed %.1f (negative = measure the mesh)."),
				TrailRadius, TrailHeight, GWholeModelTrip, GModelMarginMin, GModelMarginMax, GModelMarginFixed);

			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);
			for (const ATraceCharacter* TraceChar : Characters)
			{
				LogModelReachLine(TraceChar, TrailRadius);
			}

			int32 Total = 0;
			int32 ModelOnly = 0;
			UTraceTrailComponent::GetModelTripStats(Total, ModelOnly);
			UE_LOG(LogTraceGame, Display,
				TEXT("[MODELREACH] session trips: %d total, %d of them scored ONLY because of the whole-model "
				     "reach (%.1f%%). That share is the knock-on the spec asks to have measured before "
				     "anybody retunes the bots."),
				Total, ModelOnly, (Total > 0) ? (100.0 * ModelOnly / Total) : 0.0);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.ModelHitTest
	// ---------------------------------------------------------------------------------------------

	struct FModelHitCase
	{
		int32 Arm = 1;
		bool bParry = false;
		const TCHAR* Name = TEXT("");
		const TCHAR* Expectation = TEXT("");
	};

	// ORDER MATTERS, and it is the order of the argument rather than of convenience:
	//   the two RED cases run FIRST, so the log shows the reported bug happening before it shows it
	//   not happening; the PARRY case runs before the KILL case, because the kill takes the carrier
	//   (and therefore the trace) with it.
	const FModelHitCase ModelHitCases[] =
	{
		{ 0, false, TEXT("A  capsule-only, carrier vulnerable"),
		            TEXT("RED ARM: expected NO connection — this is the reported bug") },
		{ 0, true,  TEXT("B  capsule-only, carrier parrying"),
		            TEXT("RED ARM: expected NO connection — the parry side of the same bug") },
		{ 1, true,  TEXT("C  whole model, carrier parrying"),
		            TEXT("FIXED: expected a CONNECTION, and the DASHER punished") },
		{ 1, false, TEXT("D  whole model, carrier vulnerable"),
		            TEXT("FIXED: expected a CONNECTION, and the CARRIER killed") },
	};

	constexpr int32 ModelHitCaseCount = UE_ARRAY_COUNT(ModelHitCases);
	constexpr int32 ModelHitSetupFrameBudget = 2400;

	struct FModelHitState
	{
		int32 CaseIndex = 0;
		int32 Phase = 0;
		int32 IdleFrames = 0;

		/** Chosen ONCE, from the whole-model measurement, and reused by all four cases. */
		bool bOffsetChosen = false;
		double Offset = 0.0;
		double TrailRadius = 0.0;
		double CapsuleRadius = 0.0;
		double ModelRadius = 0.0;
		double RawMeshRadius = 0.0;
		bool bMeshMeasured = false;

		int32 SerialBefore = 0;
		TWeakObjectPtr<ATraceCharacter> Holder;
		TWeakObjectPtr<ATraceCharacter> Dasher;
		FString HolderName;
		FString DasherName;

		int32 SavedArm = 1;
		bool bSaved = false;

		int32 Passed = 0;
		int32 Skipped = 0;
		TArray<FString> Failures;
	};

	// THE PARRY IS A REAL PARRY, AND THE FIRST VERSION'S SHORTCUT IS WHY THIS IS SPELLED OUT.
	//
	// It used to flip Trace.Parry.ForceWindow, which makes EVERY carrier permanently parrying. In a
	// ten-bot match that is not a test fixture, it is a massacre: bots dash traces constantly, every
	// one of those dashes became a punish, and the observed run lost eleven pawns in two seconds and
	// then had no carrier left to test against. Two of the four cases were skipped for want of a
	// setup, by the harness's own doing.
	//
	// TraceParry::RequestParry() is the shipping entry point the bind and the bots both use. One
	// carrier, one real window, on its real cooldown — and the probe refuses to place the pawn until
	// the window is in the state the case actually needs (see FTraceModelProbe::bRequireParry), so
	// nothing here depends on guessing when a 0.2s window is open.
	void ModelHitRestore(FModelHitState& State)
	{
		if (!State.bSaved)
		{
			return;
		}
		GWholeModelTrip = State.SavedArm;
		State.bSaved = false;
	}

	bool TickModelHitTest(FModelHitState& State, float /*DeltaTime*/)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			ModelHitRestore(State);
			return false;
		}

		if (World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[MODELHIT] the trip test is server-authoritative; run this on the SERVER."));
			return false;
		}

		if (State.CaseIndex >= ModelHitCaseCount)
		{
			ModelHitRestore(State);
			GTraceModelProbe.bArmed = false;

			int32 Total = 0;
			int32 ModelOnly = 0;
			UTraceTrailComponent::GetModelTripStats(Total, ModelOnly);

			UE_LOG(LogTraceGame, Display,
				TEXT("[MODELHIT] DONE. %d of %d cases as expected, %d skipped. Session trips %d, of which %d "
				     "(%.1f%%) needed the whole-model reach. VERDICT: %s"),
				State.Passed, ModelHitCaseCount, State.Skipped, Total, ModelOnly,
				(Total > 0) ? (100.0 * ModelOnly / Total) : 0.0,
				(State.Failures.Num() == 0 && State.Passed > 0)
					? TEXT("PASS — the model-only band scores on the fixed arm, for the KILL and for the "
					       "PARRY, and scored nothing on the capsule-only arm.")
					: *FString::Printf(TEXT("*** FAIL — %s ***"), *FString::Join(State.Failures, TEXT("; "))));
			return false;
		}

		const FModelHitCase& Case = ModelHitCases[State.CaseIndex];

		// ---- phase 0: find a carrier with trace, an enemy to dash it, and stage the probe --------
		if (State.Phase == 0)
		{
			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);

			ATraceCharacter* Holder = nullptr;
			for (ATraceCharacter* TraceChar : Characters)
			{
				if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive()
					&& TraceChar->Trail != nullptr && TraceChar->Trail->TrailPoints.Items.Num() >= 6
					&& TraceChar->GetTeam() != ETraceTeam::None)
				{
					Holder = TraceChar;
					break;
				}
			}

			ATraceCharacter* Dasher = nullptr;
			if (Holder != nullptr)
			{
				for (ATraceCharacter* TraceChar : Characters)
				{
					if (TraceChar != nullptr && TraceChar != Holder && TraceChar->IsAlive()
						&& TraceChar->GetTeam() != ETraceTeam::None
						&& TraceChar->GetTeam() != Holder->GetTeam()
						&& TraceChar->GetTraceMovement() != nullptr)
					{
						Dasher = TraceChar;
						break;
					}
				}
			}

			if (Holder == nullptr || Dasher == nullptr)
			{
				ClearHarnessDriveCarrier(Characters, State.IdleFrames);
				if ((++State.IdleFrames % 300) == 0)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[MODELHIT] %s: waiting for a carrier with 6+ points and a living enemy (%d frames)."),
						Case.Name, State.IdleFrames);
				}
				if (State.IdleFrames > ModelHitSetupFrameBudget)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[MODELHIT] %s: SKIPPED — no setup in %d frames."), Case.Name, ModelHitSetupFrameBudget);
					++State.Skipped;
					++State.CaseIndex;
					State.IdleFrames = 0;
				}
				return true;
			}

			State.IdleFrames = 0;

			// ---- THE OFFSET. Chosen once, from the FIXED arm's measurement, and reused unchanged by
			// every case — including the red ones. That is what makes this an A/B and not two
			// different experiments: the pawn stands in exactly the same place all four times, and
			// the only thing that changes is whether the test is allowed to see its model.
			if (!State.bOffsetChosen)
			{
				const int32 PreviousArm = GWholeModelTrip;
				GWholeModelTrip = 1;
				const FTraceModelReach Reach =
					UTraceTrailComponent::MeasureModelReach(Dasher);
				GWholeModelTrip = PreviousArm;

				State.TrailRadius = static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius());
				State.CapsuleRadius = Reach.CapsuleRadius;
				State.ModelRadius = Reach.EffectiveRadius;
				State.RawMeshRadius = Reach.RawMeshRadius;
				State.bMeshMeasured = Reach.bMeshMeasured;

				const double Margin = Reach.HorizontalMargin();
				if (Margin <= 0.5)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[MODELHIT] ABORT — the whole-model margin on %s is %.2fuu, so there is no band to "
						     "test. Check Trace.Trail.ModelMarginMin."),
						*GetNameSafe(Dasher), Margin);
					State.CaseIndex = ModelHitCaseCount;
					return true;
				}

				// HALFWAY INTO THE BAND. The capsule's near face clears the ribbon by Margin/2 and the
				// model's near face is inside the ribbon by Margin/2 — so the pre-v10 test must miss
				// and the whole-model test must hit, and neither answer is a rounding accident.
				State.Offset = State.TrailRadius + State.CapsuleRadius + (Margin * 0.5);
				State.bOffsetChosen = true;

				UE_LOG(LogTraceGame, Display,
					TEXT("[MODELHIT] SETUP. Trace half-width %.1fuu. Dasher %s: capsule r=%.1f, mesh r=%.1f "
					     "(measured=%d), effective r=%.1f, margin %.1fuu. The dasher's capsule CENTRE will be "
					     "placed %.1fuu from the trace centreline — its capsule surface clears the ribbon by "
					     "%.1fuu, and its MODEL is inside the ribbon by %.1fuu. That gap is the bug."),
					State.TrailRadius, *GetNameSafe(Dasher), State.CapsuleRadius, State.RawMeshRadius,
					State.bMeshMeasured ? 1 : 0, State.ModelRadius, Margin, State.Offset,
					State.Offset - State.CapsuleRadius - State.TrailRadius,
					State.TrailRadius + State.ModelRadius - State.Offset);
			}

			if (!State.bSaved)
			{
				State.SavedArm = GWholeModelTrip;
				State.bSaved = true;
			}

			GWholeModelTrip = Case.Arm;

			State.Holder = Holder;
			State.Dasher = Dasher;
			State.HolderName = GetNameSafe(Holder);
			State.DasherName = GetNameSafe(Dasher);
			State.SerialBefore = GModelTripSerial;

			GTraceModelProbe.Holder = Holder;
			GTraceModelProbe.Dasher = Dasher;
			GTraceModelProbe.Offset = State.Offset;
			GTraceModelProbe.bRequireParry = Case.bParry;
			GTraceModelProbe.bApplied = false;
			GTraceModelProbe.bArmed = true;
			GTraceModelProbe.LastRefusal = TEXT("armed, waiting for the trip test");

			UE_LOG(LogTraceGame, Display,
				TEXT("[MODELHIT] CASE %s — %s. arm=%d parry=%d. %s dashes %s's trace at %.1fuu."),
				Case.Name, Case.Expectation, Case.Arm, Case.bParry ? 1 : 0,
				*State.DasherName, *State.HolderName, State.Offset);

			State.Phase = 1;
			return true;
		}

		// ---- phase 1: make the pawn really dash, and wait for the probe to fire ------------------
		if (State.Phase == 1)
		{
			ATraceCharacter* Dasher = State.Dasher.Get();
			ATraceCharacter* Holder = State.Holder.Get();

			// THE PROBE-FIRED CHECK COMES FIRST, AND THE ORDER IS THE WHOLE POINT.
			//
			// It used to sit BELOW the liveness guard, and that cost the two GREEN cases their
			// verdict on a build where the game was behaving perfectly. The sequence is:
			// the probe places the dasher, the trip test scores on the same frame, and the outcome
			// of a scored trip is that SOMEBODY DIES — the dasher in a parry case (C), the carrier
			// in a kill case (D). By the next harness tick the pawn the guard is asserting about is
			// already dead, so the guard fired "the setup fell apart" and retried, burning one
			// candidate dasher per attempt until the case ran out of pawns.
			//
			// The observed run said so plainly: five consecutive [TRACEMODEL] lines proving the
			// model-only connection landed, each followed by "setup fell apart (dasher alive=0)".
			// A DEAD DASHER IN CASE C IS THE PASS CONDITION, not a broken fixture — phase 2 already
			// judges it correctly (see `Case.bParry && bDasherAlive` there). The guard's job is only
			// to catch a setup that decayed BEFORE the probe ever fired, so once bApplied is set the
			// guard has nothing left to say and must not run at all.
			if (GTraceModelProbe.bApplied)
			{
				State.Phase = 2;
				return true;
			}

			if (Dasher == nullptr || !Dasher->IsAlive() || Holder == nullptr || !Holder->IsAlive()
				|| !Holder->IsCarrier() || Holder->Trail == nullptr
				|| Holder->Trail->TrailPoints.Items.Num() < 2)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[MODELHIT] %s: the setup fell apart before the probe fired (dasher alive=%d, carrier "
					     "alive/holding=%d). Retrying."),
					Case.Name, (Dasher != nullptr && Dasher->IsAlive()) ? 1 : 0,
					(Holder != nullptr && Holder->IsAlive() && Holder->IsCarrier()) ? 1 : 0);
				GTraceModelProbe.bArmed = false;
				State.Phase = 0;
				return true;
			}

			// Safe to spam — CanDash() gates the activation. The probe will not place anything until
			// IsDashing() is genuinely true, so this loop is "wait for a real dash", not "fake one".
			if (UTraceCharacterMovementComponent* Movement = Dasher->GetTraceMovement())
			{
				Movement->StartDash();
			}

			// And, for a parry case, keep a REAL window open on this one carrier — re-requested only
			// when it has lapsed, through the same entry point the bind and the bots use. The parry
			// cooldown can refuse; the probe waits rather than testing through a shut window.
			if (Case.bParry && Holder->Trail != nullptr && !Holder->Trail->IsParryActive())
			{
				TraceParry::RequestParry(Holder);
			}

			GTraceModelProbe.bArmed = true;

			if ((++State.IdleFrames % 300) == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[MODELHIT] %s: waiting for %s to be dashing on a trip-test frame (%d frames). Probe: %s"),
					Case.Name, *State.DasherName, State.IdleFrames, GTraceModelProbe.LastRefusal);
			}
			if (State.IdleFrames > ModelHitSetupFrameBudget)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[MODELHIT] %s: SKIPPED — the probe never fired in %d frames (%s)."),
					Case.Name, ModelHitSetupFrameBudget, GTraceModelProbe.LastRefusal);
				GTraceModelProbe.bArmed = false;
				++State.Skipped;
				++State.CaseIndex;
				State.IdleFrames = 0;
				State.Phase = 0;
			}
			return true;
		}

		// ---- phase 2: the verdict ----------------------------------------------------------------
		{
			State.IdleFrames = 0;

			ATraceCharacter* Dasher = State.Dasher.Get();
			ATraceCharacter* Holder = State.Holder.Get();

			const bool bTripped = (GModelTripSerial != State.SerialBefore)
				&& (GModelTripLastDasher.Get() == Dasher);
			const bool bWidened = bTripped && GModelTripLastWidened;

			const bool bDasherAlive = (Dasher != nullptr) && Dasher->IsAlive();
			const bool bHolderAlive = (Holder != nullptr) && Holder->IsAlive();

			const bool bWantTrip = (Case.Arm != 0);
			bool bOk = (bTripped == bWantTrip);
			FString Detail;

			if (bWantTrip && bTripped)
			{
				// The self-check: if the pre-v10 thresholds would ALSO have scored this, the offset
				// was not in the band and the whole case proves nothing.
				if (!bWidened)
				{
					bOk = false;
					Detail = TEXT("the capsule-only test would have scored this too — the offset was not in the band");
				}
				else if (Case.bParry && bDasherAlive)
				{
					bOk = false;
					Detail = TEXT("connection scored but the parried DASHER survived");
				}
				else if (!Case.bParry && bHolderAlive)
				{
					bOk = false;
					Detail = TEXT("connection scored but the CARRIER survived");
				}
				else if (Case.bParry && !bHolderAlive)
				{
					bOk = false;
					Detail = TEXT("the parrying CARRIER died — the parry did not protect them");
				}
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[MODELHIT] %s: connection=%d (wanted %d), model-only=%d, threshold %.1f -> %.1fuu at an "
				     "offset of %.1fuu (measured closest approach to the WHOLE trace: %.1fuu). Dasher %s "
				     "alive=%d, carrier %s alive=%d. %s%s"),
				Case.Name, bTripped ? 1 : 0, bWantTrip ? 1 : 0, bWidened ? 1 : 0,
				bTripped ? GModelTripLastCapsuleThreshold : (State.TrailRadius + State.CapsuleRadius),
				bTripped ? GModelTripLastThreshold : (State.TrailRadius + State.ModelRadius),
				State.Offset, GTraceModelProbe.AppliedClearance, *State.DasherName, bDasherAlive ? 1 : 0,
				*State.HolderName, bHolderAlive ? 1 : 0,
				bOk ? TEXT("PASS") : TEXT("*** FAIL ***"),
				Detail.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" — %s"), *Detail));

			if (bOk)
			{
				++State.Passed;
			}
			else
			{
				State.Failures.Add(FString::Printf(TEXT("%s (%s)"),
					Case.Name, Detail.IsEmpty()
						? *FString::Printf(TEXT("connection=%d, wanted %d"), bTripped ? 1 : 0, bWantTrip ? 1 : 0)
						: *Detail));
			}

			// A stale probe must not leak into the next case. The parry needs no unwinding: it is a
			// real 0.2s window on one pawn and it closes by itself.
			GTraceModelProbe.bArmed = false;

			++State.CaseIndex;
			State.Phase = 0;
			return true;
		}
	}

	FAutoConsoleCommand CmdModelHitTest(
		TEXT("Trace.Trail.ModelHitTest"),
		TEXT("Trace.Trail.ModelHitTest — spec v10 2. SERVER. Put a really-dashing enemy at an offset where "
		     "their MODEL is inside the ribbon but their CAPSULE is not, four times: capsule-only arm with "
		     "and without a parry (both must score NOTHING — that is the reported bug, reproduced), then "
		     "the whole-model arm with and without a parry (both must score, killing the DASHER and the "
		     "CARRIER respectively)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& /*Args*/)
		{
			FModelHitState State;

			UE_LOG(LogTraceGame, Display,
				TEXT("[MODELHIT] spec v10 2: 'if ANY part of a players model touches the trace while dashing, "
				     "it counts as a connection (either for a parry or for a kill)'. %d cases; the first two "
				     "are the RED arm and MUST report no connection."),
				ModelHitCaseCount);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickModelHitTest(State, DeltaTime);
				}), 0.f);
		}));


	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.ModelKnockOn — "do not silently retune the bots", quantified
	// ---------------------------------------------------------------------------------------------
	//
	// The spec's warning: "bots on Hard already get 82% of their kills from trace dashes. A wider hit
	// test will push that higher. Measure it and report; do not silently retune the bots."
	//
	// So this measures it, in an UNDISTURBED match — no probe, no staged turnover, no forced parry,
	// nothing but bots playing. Run it once per arm of Trace.Trail.WholeModelTrip and compare TOTALS:
	// the arm-0 number is trace connections per minute before this change, the arm-1 number is after.
	// The widened share printed alongside is the same fact from the inside (how many of the arm-1
	// connections the old thresholds would have missed), and the two must agree.

	struct FModelKnockOnState
	{
		double Seconds = 60.0;
		double Elapsed = 0.0;
		bool bStarted = false;
	};

	bool TickModelKnockOn(FModelKnockOnState& State, float DeltaTime)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		if (!State.bStarted)
		{
			UTraceTrailComponent::ResetModelTripStats();
			State.bStarted = true;

			UE_LOG(LogTraceGame, Display,
				TEXT("[MODELKNOCKON] measuring %.0fs of undisturbed play on arm %d (Trace.Trail.WholeModelTrip). "
				     "Counters reset."),
				State.Seconds, GWholeModelTrip);
			return true;
		}

		State.Elapsed += DeltaTime;
		if (State.Elapsed < State.Seconds)
		{
			return true;
		}

		int32 Total = 0;
		int32 ModelOnly = 0;
		UTraceTrailComponent::GetModelTripStats(Total, ModelOnly);

		UE_LOG(LogTraceGame, Display,
			TEXT("[MODELKNOCKON] DONE. arm=%d, %.0fs: %d trace connections (%.2f per minute), %d of them "
			     "(%.1f%%) scored only because of the whole-model reach. Session trace kills %d, of which %d "
			     "landed on a NON-carrier (spec v9 3 must stay at 0)."),
			GWholeModelTrip, State.Elapsed, Total, (State.Elapsed > 0.0) ? (Total * 60.0 / State.Elapsed) : 0.0,
			ModelOnly, (Total > 0) ? (100.0 * ModelOnly / Total) : 0.0,
			GTrailKillsTotal, GTrailKillsOnNonCarrier);

		return false;
	}

	FAutoConsoleCommand CmdModelKnockOn(
		TEXT("Trace.Trail.ModelKnockOn"),
		TEXT("Trace.Trail.ModelKnockOn [seconds] — spec v10 2. Count trace connections in an UNDISTURBED bot "
		     "match for N seconds and report the rate and the share that needed the whole-model reach. Run it "
		     "once with Trace.Trail.WholeModelTrip 0 and once with 1; the difference in the totals is the "
		     "knock-on on the bots, measured rather than guessed."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FModelKnockOnState State;
			if (Args.Num() > 0)
			{
				State.Seconds = FMath::Clamp(FCString::Atod(*Args[0]), 5.0, 600.0);
			}

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickModelKnockOn(State, DeltaTime);
				}), 0.f);
		}));


	// =============================================================================================
	// SPEC v10 §3 — THE TURNOVER GRACE
	//
	// Verbatim: "The grace period on turnovers doesn't seem to be working. Test it on both modes, fix
	// it if needed. If it IS working, increase to .75seconds."
	//
	// THE CONDITIONAL IS THE INSTRUCTION. So there are two commands and they answer two different
	// questions, because a staged turnover and a real one are not the same evidence:
	//
	//   Trace.Trail.GraceTest   — DRIVES the turnover through ATraceCore::GrantTo (the documented
	//                             single funnel every possession route ends at) and measures, on the
	//                             shared clock, how long the new holder goes without laying a point.
	//                             Also asserts the v9 §3 instant clear COMPOSES with it: the old
	//                             trace must be gone on the very next frame AND the new one must not
	//                             have started.
	//   Trace.Trail.GraceWatch  — WATCHES a live match for N seconds and reports the same numbers for
	//                             every possession change that actually happens. This is the one that
	//                             covers mode B honestly: a throw, an interception in flight and a
	//                             teammate recovering a loose Core are routes with their own team
	//                             bookkeeping (GraceOverrideTeam), and staging them would be testing
	//                             the harness rather than the game.
	//
	// Both print the EXPECTED grace beside the MEASURED one, and both name the mode they ran in, so
	// "test it on both modes" is answerable from one log line rather than from a claim.
	// =============================================================================================

	const TCHAR* ScoringModeName(const UWorld* World)
	{
		if (World != nullptr)
		{
			if (const ATraceGameState* GameState = World->GetGameState<ATraceGameState>())
			{
				return (GameState->GetScoringMode() == ETraceScoringMode::ThrownCoreAndGoals)
					? TEXT("B (ThrownCoreAndGoals)") : TEXT("A (EndzoneStatusCore)");
			}
		}
		return (UTraceSettings::Get().ScoringMode == ETraceScoringMode::ThrownCoreAndGoals)
			? TEXT("B (ThrownCoreAndGoals, from settings)") : TEXT("A (EndzoneStatusCore, from settings)");
	}

	enum class EGraceCase : uint8
	{
		Turnover,   // team CHANGES: the grace must run
		Pass,       // same team: no grace, the trace continues without a gap
		Count
	};

	const TCHAR* GraceCaseName(EGraceCase Case)
	{
		switch (Case)
		{
		case EGraceCase::Turnover: return TEXT("TURNOVER (team change)");
		case EGraceCase::Pass:     return TEXT("PASS (same team)");
		default:                   return TEXT("?");
		}
	}

	struct FGraceTestState
	{
		int32 CaseIndex = 0;
		int32 Phase = 0;
		int32 IdleFrames = 0;

		TWeakObjectPtr<ATraceCharacter> Previous;
		TWeakObjectPtr<ATraceCharacter> NewHolder;
		FString PreviousName;
		FString NewHolderName;

		double StartWorldTime = 0.0;
		float ExpectedGrace = 0.f;
		float GrantedDeadline = 0.f;
		int32 PreviousPointsBefore = 0;

		/** The v9 3 composition check: the OLD trace on the frame after the turnover. */
		int32 PreviousPointsAfter = -1;

		int32 Passed = 0;
		int32 Skipped = 0;
		TArray<FString> Failures;
	};

	constexpr int32 GraceSetupFrameBudget = 2400;

	/** Long enough to prove "no point yet" is the grace and not merely a slow frame. */
	constexpr double GraceObserveCeilingSeconds = 3.0;

	bool TickGraceTest(FGraceTestState& State, float /*DeltaTime*/)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		if (World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[GRACETEST] this drives possession and must run on the SERVER."));
			return false;
		}

		if (State.CaseIndex >= static_cast<int32>(EGraceCase::Count))
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[GRACETEST] DONE in mode %s. %d of %d cases behaved as specified, %d skipped. "
				     "Configured grace: %.3fs (UTraceSettings::CoreTurnoverGraceSeconds, overridable with "
				     "Trace.Trail.TurnoverGrace). VERDICT: %s"),
				ScoringModeName(World), State.Passed, static_cast<int32>(EGraceCase::Count), State.Skipped,
				UTraceTrailComponent::GetTurnoverGraceSeconds(),
				(State.Failures.Num() == 0 && State.Passed > 0)
					? TEXT("PASS — the grace delays FORMATION on a team change, does not delay it on a pass, "
					       "and composes with the instant clear.")
					: *FString::Printf(TEXT("*** FAIL — %s ***"), *FString::Join(State.Failures, TEXT("; "))));
			return false;
		}

		const EGraceCase Case = static_cast<EGraceCase>(State.CaseIndex);

		ATraceCore* Core = nullptr;
		{
			TActorIterator<ATraceCore> CoreIt(World);
			if (CoreIt)
			{
				Core = *CoreIt;
			}
		}
		if (Core == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[GRACETEST] no ATraceCore in the world."));
			return false;
		}

		// ---- phase 0: fire the turnover ----------------------------------------------------------
		if (State.Phase == 0)
		{
			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);

			ATraceCharacter* Previous = nullptr;
			for (ATraceCharacter* TraceChar : Characters)
			{
				if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive()
					&& TraceChar->Trail != nullptr && TraceChar->Trail->TrailPoints.Items.Num() >= 4
					&& TraceChar->GetTeam() != ETraceTeam::None)
				{
					Previous = TraceChar;
					break;
				}
			}

			ATraceCharacter* Target = nullptr;
			if (Previous != nullptr)
			{
				const bool bWantAlly = (Case == EGraceCase::Pass);
				for (ATraceCharacter* TraceChar : Characters)
				{
					if (TraceChar == nullptr || TraceChar == Previous || !TraceChar->IsAlive()
						|| TraceChar->GetTeam() == ETraceTeam::None || TraceChar->Trail == nullptr)
					{
						continue;
					}
					if ((TraceChar->GetTeam() == Previous->GetTeam()) == bWantAlly)
					{
						Target = TraceChar;
						break;
					}
				}
			}

			if (Previous == nullptr || Target == nullptr)
			{
				ClearHarnessDriveCarrier(Characters, State.IdleFrames);
				if ((++State.IdleFrames % 300) == 0)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[GRACETEST] %s: waiting for a carrier with 4+ points and the %s this case needs "
						     "(%d frames)."),
						GraceCaseName(Case), (Case == EGraceCase::Pass) ? TEXT("TEAMMATE") : TEXT("ENEMY"),
						State.IdleFrames);
				}
				if (State.IdleFrames > GraceSetupFrameBudget)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[GRACETEST] %s: SKIPPED — no setup in %d frames."),
						GraceCaseName(Case), GraceSetupFrameBudget);
					++State.Skipped;
					++State.CaseIndex;
					State.IdleFrames = 0;
				}
				return true;
			}

			State.IdleFrames = 0;
			State.Previous = Previous;
			State.NewHolder = Target;
			State.PreviousName = GetNameSafe(Previous);
			State.NewHolderName = GetNameSafe(Target);
			State.PreviousPointsBefore = Previous->Trail->TrailPoints.Items.Num();
			State.ExpectedGrace = (Case == EGraceCase::Turnover)
				? UTraceTrailComponent::GetTurnoverGraceSeconds() : 0.f;

			// THE FUNNEL, not a shortcut around it. ATraceCore::GrantTo is where the grace decision
			// ("is this a team change?") is made and where SetEmitGrace is called, and every real
			// route — a pass, a kill steal, an interception, a loose-Core pickup in mode B — ends
			// here. Driving it directly tests the same code a match does.
			Core->GrantTo(Target, (Case == EGraceCase::Pass)
				? ETraceCoreGrantReason::Pass : ETraceCoreGrantReason::Kill);

			State.StartWorldTime = World->GetTimeSeconds();
			State.GrantedDeadline = (Target->Trail != nullptr)
				? Target->Trail->GetEmitGraceEndServerTime() : 0.f;
			State.PreviousPointsAfter = -1;

			UE_LOG(LogTraceGame, Display,
				TEXT("[GRACETEST] %s in mode %s: %s (team %d, %d points) -> %s (team %d). Expecting %.3fs before "
				     "the new trace begins forming; the emitter's own deadline is %.3f (0 = no grace armed)."),
				GraceCaseName(Case), ScoringModeName(World), *State.PreviousName,
				static_cast<int32>(Previous->GetTeam()), State.PreviousPointsBefore, *State.NewHolderName,
				static_cast<int32>(Target->GetTeam()), State.ExpectedGrace, State.GrantedDeadline);

			State.Phase = 1;
			return true;
		}

		// ---- phase 1: measure ---------------------------------------------------------------------
		{
			ATraceCharacter* NewHolder = State.NewHolder.Get();
			ATraceCharacter* Previous = State.Previous.Get();

			// v9 §3 COMPOSITION, checked on the FIRST frame after the grant and never again: the old
			// carrier's trace must already be gone, at the same time as the new one has not started.
			if (State.PreviousPointsAfter < 0)
			{
				State.PreviousPointsAfter = (Previous != nullptr && Previous->Trail != nullptr)
					? Previous->Trail->TrailPoints.Items.Num() : 0;
			}

			const double Elapsed = World->GetTimeSeconds() - State.StartWorldTime;

			if (NewHolder == nullptr || NewHolder->Trail == nullptr || !NewHolder->IsAlive()
				|| !NewHolder->IsCarrier())
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[GRACETEST] %s: SKIPPED — the new holder stopped carrying after %.3fs, before the "
					     "measurement finished."),
					GraceCaseName(Case), Elapsed);
				++State.Skipped;
				++State.CaseIndex;
				State.Phase = 0;
				return true;
			}

			const int32 Points = NewHolder->Trail->TrailPoints.Items.Num();
			const bool bFormed = Points > 0;

			if (!bFormed && Elapsed < GraceObserveCeilingSeconds)
			{
				// Keep them moving so "no point" cannot be confused with "stood still", which is the
				// other reason a trace does not grow (v7 §1 removed the timer; a stationary carrier
				// lays nothing). This is the difference between measuring the grace and measuring a
				// bot that happened to be idle.
				NewHolder->AddMovementInput(FVector(1.f, 0.f, 0.f), 1.f);
				return true;
			}

			// THE TOLERANCE, and why it is one frame rather than zero: the emitter is gated on
			// GetServerTimeSeconds() >= the deadline and is only evaluated once per component tick, so
			// the first point lands on the first TICK after the grace expires, not at the instant it
			// does. One frame at 60Hz is 16.7ms against a 500ms grace.
			constexpr double GraceToleranceSeconds = 0.05;
			const double Expected = static_cast<double>(State.ExpectedGrace);
			const bool bTimingOk = bFormed
				&& (Elapsed >= Expected - GraceToleranceSeconds)
				&& (Elapsed <= Expected + 0.25);

			const bool bClearOk = (State.PreviousPointsAfter == 0);
			const bool bOk = bTimingOk && bClearOk;

			UE_LOG(LogTraceGame, Display,
				TEXT("[GRACETEST] %s in mode %s: the new holder's FIRST point landed %.3fs after possession "
				     "(expected %.3fs). Old carrier %s went %d -> %d points on the next frame. %s%s"),
				GraceCaseName(Case), ScoringModeName(World), Elapsed, Expected, *State.PreviousName,
				State.PreviousPointsBefore, State.PreviousPointsAfter,
				bOk ? TEXT("PASS") : TEXT("*** FAIL"),
				bOk ? TEXT("")
					: (!bClearOk
						? TEXT(" — the previous trace SURVIVED the turnover (v9 3 regression) ***")
						: (!bFormed
							? TEXT(" — the trace NEVER began forming ***")
							: TEXT(" — the grace is not the length it is configured to be ***"))));

			if (bOk)
			{
				++State.Passed;
			}
			else
			{
				State.Failures.Add(FString::Printf(TEXT("%s: first point at %.3fs, expected %.3fs; old trace left %d points"),
					GraceCaseName(Case), Elapsed, Expected, State.PreviousPointsAfter));
			}

			++State.CaseIndex;
			State.Phase = 0;
			return true;
		}
	}

	FAutoConsoleCommand CmdGraceTest(
		TEXT("Trace.Trail.GraceTest"),
		TEXT("Trace.Trail.GraceTest — spec v10 3. SERVER. Drive a TEAM-CHANGE turnover and a same-team pass "
		     "through ATraceCore::GrantTo and measure, on the clock, how long the new holder goes before "
		     "laying their first point. Asserts the turnover waits the configured grace, the pass waits "
		     "nothing, and that the v9 3 instant clear composes with both."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& /*Args*/)
		{
			FGraceTestState State;

			UE_LOG(LogTraceGame, Display,
				TEXT("[GRACETEST] spec v10 3. Configured grace %.3fs. The grace delays FORMATION, never "
				     "lethality — already-laid segments kill throughout."),
				UTraceTrailComponent::GetTurnoverGraceSeconds());

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickGraceTest(State, DeltaTime);
				}), 0.f);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.GraceWatch — the same numbers, from possession changes the MATCH made
	// ---------------------------------------------------------------------------------------------

	struct FGraceEpisode
	{
		TWeakObjectPtr<ATraceCharacter> Holder;
		FString HolderName;
		FString PreviousName;
		ETraceTeam PreviousTeam = ETraceTeam::None;
		ETraceTeam NewTeam = ETraceTeam::None;
		bool bTeamChanged = false;
		double StartTime = 0.0;
		float Deadline = 0.f;
		bool bOpen = true;
	};

	struct FGraceWatchState
	{
		double Seconds = 30.0;
		double Elapsed = 0.0;

		TWeakObjectPtr<ATraceCharacter> LastCarrier;
		bool bSeeded = false;

		FGraceEpisode Live;

		int32 Turnovers = 0;
		int32 TurnoversGraced = 0;
		double TurnoverGraceSum = 0.0;
		double TurnoverGraceWorst = 0.0;

		int32 Passes = 0;
		int32 PassesUngraced = 0;
		double PassDelaySum = 0.0;

		int32 Abandoned = 0;
		int32 ClearFailures = 0;
	};

	void CloseGraceEpisode(FGraceWatchState& State, const UWorld* World, bool bFormed, double Delay)
	{
		if (!State.Live.bOpen)
		{
			return;
		}
		State.Live.bOpen = false;

		if (!bFormed)
		{
			++State.Abandoned;
			UE_LOG(LogTraceGame, Display,
				TEXT("[GRACEWATCH] %s -> %s (teamChange=%d): possession ended after %.3fs before a point was "
				     "ever laid. Not scored."),
				*State.Live.PreviousName, *State.Live.HolderName, State.Live.bTeamChanged ? 1 : 0, Delay);
			return;
		}

		const float Configured = UTraceTrailComponent::GetTurnoverGraceSeconds();

		if (State.Live.bTeamChanged)
		{
			++State.Turnovers;
			State.TurnoverGraceSum += Delay;
			State.TurnoverGraceWorst = FMath::Max(State.TurnoverGraceWorst, Delay);
			if (Delay >= static_cast<double>(Configured) - 0.05)
			{
				++State.TurnoversGraced;
			}
		}
		else
		{
			++State.Passes;
			State.PassDelaySum += Delay;
			if (Delay < 0.10)
			{
				++State.PassesUngraced;
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[GRACEWATCH] mode %s: %s (team %d) -> %s (team %d), teamChange=%d. First point %.3fs later; "
			     "expected %.3fs. %s"),
			ScoringModeName(World), *State.Live.PreviousName, static_cast<int32>(State.Live.PreviousTeam),
			*State.Live.HolderName, static_cast<int32>(State.Live.NewTeam), State.Live.bTeamChanged ? 1 : 0,
			Delay, State.Live.bTeamChanged ? Configured : 0.f,
			State.Live.bTeamChanged
				? ((Delay >= static_cast<double>(Configured) - 0.05) ? TEXT("GRACED") : TEXT("*** NO GRACE ***"))
				: ((Delay < 0.10) ? TEXT("no grace, as specified") : TEXT("*** UNEXPECTED DELAY ***")));
	}

	bool TickGraceWatch(FGraceWatchState& State, float DeltaTime)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		State.Elapsed += DeltaTime;

		TArray<ATraceCharacter*> Characters;
		GatherTrailDebugCharacters(World, Characters);

		ATraceCharacter* Carrier = nullptr;
		for (ATraceCharacter* TraceChar : Characters)
		{
			if (TraceChar != nullptr && TraceChar->IsCarrier() && TraceChar->IsAlive())
			{
				Carrier = TraceChar;
				break;
			}
		}

		const double Now = World->GetTimeSeconds();

		if (!State.bSeeded)
		{
			State.LastCarrier = Carrier;
			State.bSeeded = true;
		}
		else if (Carrier != State.LastCarrier.Get())
		{
			ATraceCharacter* Old = State.LastCarrier.Get();

			// An open episode that never formed a point: possession moved on inside the grace.
			if (State.Live.bOpen)
			{
				CloseGraceEpisode(State, World, false, Now - State.Live.StartTime);
			}

			// v9 §3 composition, on the frame the possession change is observed. A carrier who has
			// just lost the Core must be holding nothing.
			if (Old != nullptr && Old->Trail != nullptr && Old->Trail->TrailPoints.Items.Num() > 0
				&& !Old->IsCarrier())
			{
				++State.ClearFailures;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[GRACEWATCH] %s lost the Core but still holds %d trace points — the instant clear and "
					     "the grace are NOT composing (spec v9 3)."),
					*GetNameSafe(Old), Old->Trail->TrailPoints.Items.Num());
			}

			if (Carrier != nullptr && Carrier->Trail != nullptr)
			{
				State.Live = FGraceEpisode();
				State.Live.Holder = Carrier;
				State.Live.HolderName = GetNameSafe(Carrier);
				State.Live.PreviousName = (Old != nullptr) ? GetNameSafe(Old) : FString(TEXT("<loose/none>"));
				State.Live.PreviousTeam = (Old != nullptr) ? Old->GetTeam() : ETraceTeam::None;
				State.Live.NewTeam = Carrier->GetTeam();

				// Read from the EMITTER, not re-derived: the deadline it is actually gated on is the
				// only honest statement of whether a grace was armed for this possession.
				State.Live.Deadline = Carrier->Trail->GetEmitGraceEndServerTime();
				State.Live.bTeamChanged = (State.Live.Deadline > 0.f);
				State.Live.StartTime = Now;
				State.Live.bOpen = true;
			}

			State.LastCarrier = Carrier;
		}

		if (State.Live.bOpen)
		{
			ATraceCharacter* Holder = State.Live.Holder.Get();
			if (Holder == nullptr || Holder->Trail == nullptr || !Holder->IsCarrier() || !Holder->IsAlive())
			{
				CloseGraceEpisode(State, World, false, Now - State.Live.StartTime);
			}
			else if (Holder->Trail->TrailPoints.Items.Num() > 0)
			{
				CloseGraceEpisode(State, World, true, Now - State.Live.StartTime);
			}
		}

		if (State.Elapsed < State.Seconds)
		{
			return true;
		}

		const float Configured = UTraceTrailComponent::GetTurnoverGraceSeconds();
		UE_LOG(LogTraceGame, Display,
			TEXT("[GRACEWATCH] DONE after %.0fs in mode %s. TEAM CHANGES: %d observed, %d waited the configured "
			     "%.3fs (mean %.3fs, worst %.3fs). SAME-TEAM: %d observed, %d formed immediately (mean %.3fs). "
			     "%d possessions ended inside the grace. %d instant-clear violations. VERDICT: %s"),
			State.Elapsed, ScoringModeName(World), State.Turnovers, State.TurnoversGraced, Configured,
			(State.Turnovers > 0) ? (State.TurnoverGraceSum / State.Turnovers) : 0.0, State.TurnoverGraceWorst,
			State.Passes, State.PassesUngraced,
			(State.Passes > 0) ? (State.PassDelaySum / State.Passes) : 0.0,
			State.Abandoned, State.ClearFailures,
			(State.Turnovers > 0 && State.TurnoversGraced == State.Turnovers
				&& State.PassesUngraced == State.Passes && State.ClearFailures == 0)
				? TEXT("PASS — every real turnover in this match waited the grace, and no pass did.")
				: TEXT("see the per-episode lines above"));

		return false;
	}

	FAutoConsoleCommand CmdGraceWatch(
		TEXT("Trace.Trail.GraceWatch"),
		TEXT("Trace.Trail.GraceWatch [seconds] — spec v10 3. Watch a live match and report, for EVERY "
		     "possession change the game itself makes, whether a grace was armed and how long the new "
		     "holder actually went before laying their first point. The honest way to cover mode B's own "
		     "routes (a throw, an interception, a teammate recovering a loose Core) without staging them."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FGraceWatchState State;
			if (Args.Num() > 0)
			{
				State.Seconds = FMath::Clamp(FCString::Atod(*Args[0]), 1.0, 600.0);
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[GRACEWATCH] watching %.0fs. Configured grace %.3fs."),
				State.Seconds, UTraceTrailComponent::GetTurnoverGraceSeconds());

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickGraceWatch(State, DeltaTime);
				}), 0.f);
		}));


	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.WallClip — SPEC v12 §6, the reproduction and the verdict in one command
	// ---------------------------------------------------------------------------------------------
	//
	// "The trace is clipping into walls sometimes, when a model runs close to a corner/structure."
	//
	// So this drives a carrier along the level's walls and round its corners for N seconds and asks,
	// every frame, how far the trace is INSIDE the level — separately for the ribbon that is on screen
	// and for the column that kills. It ends by pointing a camera at the worst offender and taking a
	// picture of it, because "the trace clips into walls" is a thing you look at.
	//
	// WHY BOTH NUMBERS. They are the diagnosis, not just the verdict:
	//
	//   lethal ~= drawn, both > 0   the PATH goes through the structure. Everything downstream is
	//                               working from a polyline that cuts the corner, so no amount of
	//                               changing how the ribbon is DRAWN would fix it, and any fix that
	//                               only moved the drawing would create an invisible kill volume.
	//   drawn > lethal              the drawing is reaching outside the volume that kills — the
	//                               "visible but not lethal" failure, in its own right.
	//   both 0                      the fix is in.
	//
	// AND IT IS AN A/B. `Trace.Trail.WallClip 40 0` forces the pre-v12 straight chord and reproduces
	// the bug on demand; `Trace.Trail.WallClip 40 1` runs the same drive with the fitter on. Same
	// build, same route, same measurement — the only difference is the one cvar.

	/**
	 * ONE FACE OF ONE RENDERED STRUCTURE, and how close a BODY can actually get to it.
	 *
	 * Standoff is the whole reason this type exists. The arena hangs PAWN-ONLY shells in front of any
	 * structure carrying eye-height neon, so a 34uu capsule is stopped 60-74uu short of those surfaces
	 * and no trace 22.5uu wide could ever reach them. The previous fixture picked whatever wall was
	 * nearest, spent its whole run alongside one of those, and reported — in its own output — that the
	 * carrier never got within 59.3uu of anything. A test that could not fail, which is why the bug
	 * survived a pass.
	 *
	 * The surfaces where the reported bug is POSSIBLE are the ones with no shell: the walkable
	 * terraces and stepped platforms (a shell there would leave players standing on thin air), and
	 * every emissive lip and skirt, which protrude 10-13uu past collision and have no collision of
	 * their own at all. Against those a body presses to 34uu of the collision box, i.e. INSIDE the
	 * rendered lip. Standoff finds them: it is negative exactly there.
	 */
	struct FWallTarget
	{
		FVector Face = FVector::ZeroVector;     // point on the rendered face, at body height
		FVector Normal = FVector::ZeroVector;   // outward horizontal normal
		FVector Stand = FVector::ZeroVector;    // where a capsule can actually stand, verified
		double Achieved = 0.0;                  // how close that puts the capsule CENTRE to the face
		FString Name;
	};

	struct FWallClipState
	{
		double Seconds = 40.0;
		int32 Arm = -1;

		// ---- the tour (v13 §7) -------------------------------------------------------------------
		TArray<FWallTarget> Targets;
		int32 TargetIndex = 0;
		int32 TargetPhase = 0;
		double TargetSeconds = 0.0;
		double TangentSign = 1.0;
		int32 StallFrames = 0;
		int32 TargetsVisited = 0;
		double SecondsPerTarget = 6.0;
		bool bTeleportedToStart = false;

		/** Only measure while the LOCAL pawn is the carrier. Cleared when there is no local pawn. */
		bool bDriveLocalOnly = true;

		/** Cleared on every target change: the carrier is placed at each target's lane exactly once. */
		bool bPlacedAtTarget = false;

		/** Worst standoff (i.e. tightest surface) the tour actually steered at, for the report. */
		double TightestTargetStandoff = TNumericLimits<double>::Max();
		FString TightestTargetName;

		// ---- the standing invariant, both directions ---------------------------------------------
		double WorstDrawnOutsideLethal = 0.0;
		FVector WorstDrawnOutsideLethalAt = FVector::ZeroVector;
		double WorstDrawnOutsideLethalVertical = 0.0;
		double WorstDrawnPastEnd = 0.0;
		double WorstLethalOutsideDrawn = 0.0;
		FVector WorstLethalOutsideDrawnAt = FVector::ZeroVector;
		bool bWorstLethalOutsideDrawnIsEndCap = false;

		int32 SavedArm = 1;
		bool bArmSaved = false;

		int32 Phase = 0;
		int32 Frames = 0;
		double Elapsed = 0.0;

		TWeakObjectPtr<ATraceCharacter> Carrier;

		double WorstDrawn = 0.0;
		FVector WorstDrawnAt = FVector::ZeroVector;
		FVector WorstDrawnPushOut = FVector::ZeroVector;
		FString WorstDrawnPiece;

		double WorstLethal = 0.0;
		FVector WorstLethalAt = FVector::ZeroVector;
		FString WorstLethalPiece;

		/** Closest the ribbon ever came to a surface without entering it — the fixture's own proof. */
		double NearestSurface = TNumericLimits<double>::Max();

		/** The rendered level pieces, gathered once. */
		TArray<UTraceTrailComponent::FTraceClipBox> Geometry;

		int32 FramesMeasured = 0;
		int32 FramesWithDrawnClip = 0;
		int32 FramesWithLethalClip = 0;
		int32 MaxPoints = 0;

		/** Closest the carrier's own body ever came to a rendered surface, and to what. */
		double NearestCarrier = TNumericLimits<double>::Max();
		FString NearestCarrierPiece;

		/** Smallest pawn-barrier-to-visible-surface standoff the drive found anywhere. */
		double MinStandoff = TNumericLimits<double>::Max();

		/** Capture bookkeeping: the view has to be set a few frames before the shot is requested. */
		int32 ShotPhaseFrames = 0;
		FString ShotPath;
		TWeakObjectPtr<ACameraActor> Camera;
	};

	/**
	 * FIND THE PLACES A BODY CAN ACTUALLY PRESS AGAINST SOMETHING VISIBLE, and prove each one by
	 * standing a capsule there.
	 *
	 * The first version of this ranked faces by a pawn-ray STANDOFF and steered at the smallest. It
	 * ran, and its own credential line said the carrier still never got within 55.1uu of anything —
	 * because "no pawn barrier in front of this face" ranked best of all, and the faces that scored it
	 * were decorative skins buried inside other geometry that no body can reach. Ranking a place by
	 * how tight it LOOKS is the same class of mistake as the fixture this replaces.
	 *
	 * So the rank is now the ACHIEVED distance, and it is achieved by construction: march outward from
	 * the visible face until a real pawn capsule fits and has floor under it, and record where that
	 * was. A face that no capsule can reach produces no target at all. The number this yields is
	 * exactly the number the verdict's credential tests, so the tour cannot promise a press it cannot
	 * deliver.
	 */
	void BuildWallTour(UWorld* World, const TArray<UTraceTrailComponent::FTraceClipBox>& Geometry,
		double ProbeZ, double BandHalfHeight, double CapsuleRadius, double CapsuleHalfHeight,
		TArray<FWallTarget>& Out)
	{
		Out.Reset();
		if (World == nullptr)
		{
			return;
		}

		constexpr double MinFaceExtent = 60.0;

		// March from just outside the visible surface to well clear of it. The first fit wins, so the
		// step is the resolution of the answer; 4uu is far finer than the 14uu differences that decide
		// whether the ribbon enters a lip.
		constexpr double MarchStep = 4.0;
		constexpr double MarchLimit = 120.0;

		FCollisionQueryParams Params(FName(TEXT("TraceTrailWallTour")), /*bTraceComplex=*/false);
		const FCollisionShape Body = FCollisionShape::MakeCapsule(
			static_cast<float>(CapsuleRadius), static_cast<float>(CapsuleHalfHeight));

		TArray<FWallTarget> Candidates;

		for (const UTraceTrailComponent::FTraceClipBox& Box : Geometry)
		{
			// IT HAS TO BE AT BODY HEIGHT. The trace is a 63uu column centred on the carrier's capsule
			// centre; a tread whose top is below that column's floor cannot be clipped into sideways,
			// and including it would fill the tour with kerbs.
			if (Box.WorldBounds.Max.Z < ProbeZ - BandHalfHeight
				|| Box.WorldBounds.Min.Z > ProbeZ + BandHalfHeight)
			{
				continue;
			}

			const FVector Centre = Box.Transform.TransformPosition(Box.LocalCentre);
			const FVector Axis[3] =
			{
				Box.Transform.GetUnitAxis(EAxis::X),
				Box.Transform.GetUnitAxis(EAxis::Y),
				Box.Transform.GetUnitAxis(EAxis::Z)
			};
			const double Half[3] =
			{
				FMath::Abs(Box.LocalExtent.X * Box.Scale.X),
				FMath::Abs(Box.LocalExtent.Y * Box.Scale.Y),
				FMath::Abs(Box.LocalExtent.Z * Box.Scale.Z)
			};

			for (int32 A = 0; A < 3; ++A)
			{
				if (FMath::Abs(Axis[A].Z) > 0.7)
				{
					continue;   // the box's vertical axis has no vertical face
				}

				// A face narrower than this is trim on something else, not a wall to run along. It is
				// still MEASURED against (it is in Geometry) — it is just not a thing to steer at.
				const int32 Other = (A == 0) ? 1 : 0;
				if (Half[Other] < MinFaceExtent || Half[A] < 1.0)
				{
					continue;
				}

				for (int32 Sign = -1; Sign <= 1; Sign += 2)
				{
					const FVector Normal = Axis[A] * static_cast<double>(Sign);
					FVector Face = Centre + Normal * Half[A];
					Face.Z = ProbeZ;

					// MARCH OUT UNTIL A REAL BODY FITS. Starting at 2uu means a purely decorative
					// surface — one with no collision of its own, which is most of the emissive trim —
					// is correctly reported as something a capsule can stand right inside.
					for (double Distance = 2.0; Distance <= MarchLimit; Distance += MarchStep)
					{
						FVector Stand = Face + Normal * Distance;

						// Put the feet on the ground rather than trusting the carrier's current Z:
						// this arena is terraced, and a candidate floating over a step is a candidate
						// the drive can never reach.
						FHitResult Ground;
						if (World->LineTraceSingleByChannel(Ground,
							Stand + FVector(0.0, 0.0, 160.0),
							Stand - FVector(0.0, 0.0, 400.0), ECC_Visibility, Params))
						{
							Stand.Z = Ground.ImpactPoint.Z + CapsuleHalfHeight + 2.0;
						}
						else
						{
							continue;   // nothing to stand on here
						}

						if (World->OverlapBlockingTestByChannel(
							Stand, FQuat::Identity, ECC_Pawn, Body, Params))
						{
							continue;   // a barrier, a shell or the structure itself — try further out
						}

						// AND THE BODY HAS TO BE ABLE TO WALK THERE.
						//
						// "A capsule fits here" and "the drive can reach here" are different claims, and
						// conflating them cost this fixture three runs: the tightest spots it found were
						// on the raised corner terraces and the central pedestal, the carrier could not
						// path up to any of them, and every target timed out at its approach — 3 faces
						// visited in 30s and a body that never came within 49uu of anything. So a
						// candidate now has to be on the carrier's own level and have a clear lane a
						// capsule can sweep down.
						if (FMath::Abs(Stand.Z - ProbeZ) > 90.0)
						{
							continue;
						}

						FHitResult Lane;
						if (World->SweepSingleByChannel(Lane, Stand + Normal * 300.0, Stand,
							FQuat::Identity, ECC_Pawn, Body, Params))
						{
							continue;   // no approach lane — the drive would grind into something
						}

						FWallTarget& Target = Candidates.AddDefaulted_GetRef();
						Target.Face = Face;
						Target.Normal = Normal;
						Target.Stand = Stand;
						Target.Achieved = Distance;
						Target.Name = Box.Name;
						break;
					}
				}
			}
		}

		// TIGHTEST FIRST — measured, not inferred.
		Candidates.Sort([](const FWallTarget& A, const FWallTarget& B)
		{
			return A.Achieved < B.Achieved;
		});

		// Spread out, so the tour is a tour and not twenty faces of the same rib. 700uu apart is about
		// one structure.
		constexpr double DedupeDistance = 700.0;
		constexpr int32 MaxTargets = 14;

		for (const FWallTarget& Candidate : Candidates)
		{
			bool bTooClose = false;
			for (const FWallTarget& Kept : Out)
			{
				if (FVector::DistSquared2D(Kept.Face, Candidate.Face) < DedupeDistance * DedupeDistance)
				{
					bTooClose = true;
					break;
				}
			}
			if (bTooClose)
			{
				continue;
			}

			Out.Add(Candidate);
			if (Out.Num() >= MaxTargets)
			{
				break;
			}
		}
	}

	/**
	 * DRIVE THE CARRIER AT A STRUCTURE, ALONG IT, AND ROUND ITS CORNER — through AddMovementInput and
	 * real collision, so every position the trace is laid at is a position a player could be in.
	 *
	 * Two phases per target and nothing clever in either. APPROACH walks straight into the face until
	 * the movement component stops the body — which is, by definition, as close as anyone can get.
	 * HUG then runs along the face holding station against it; when the face runs out, the same lean
	 * carries the body round the convex corner and onto the next surface, which is the case in the
	 * report ("when a model runs close to a corner/structure") and the one the old fixture never
	 * reached.
	 *
	 * The stall detector is what makes the corner happen rather than a body grinding into a wall
	 * forever: no progress for a second and the tour moves on.
	 */
	/**
	 * How far PAST a rendered face the drive aims, in uu (spec v14 §1).
	 *
	 * Not a distance the body ever reaches — it is a distance the STEERING is allowed to keep asking
	 * for. See the two comments in DriveWallTour. 150 is comfortably more than the 34uu capsule
	 * radius plus the thickest standoff shell in the arena, so the input is still saturated inward at
	 * the moment collision takes over.
	 */
	constexpr double WallPressDepth = 150.0;

	void DriveWallTour(ATraceCharacter* Carrier, FWallClipState& State, float DeltaTime)
	{
		if (Carrier == nullptr)
		{
			return;
		}

		const FVector At = Carrier->GetActorLocation();

		if (State.Targets.Num() == 0)
		{
			// Nothing to steer at. Wander rather than stand still, so the run is at least honest about
			// having produced trace, and say so in the report through NearestCarrier.
			const double Angle = static_cast<double>(State.Frames) * 0.02;
			Carrier->AddMovementInput(FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0), 1.f);
			return;
		}

		const FWallTarget& Target = State.Targets[State.TargetIndex % State.Targets.Num()];
		State.TargetSeconds += DeltaTime;

		// PLACED AT EACH TARGET'S APPROACH LANE, THEN WALKED. The tour spans the whole arena — the
		// tightest faces it finds are up to 16000uu apart, and the log showed target after target
		// TIMED OUT with the body still 4000-16000uu short, having spent its whole slot jogging. So the
		// carrier is put 200uu off each face and everything that is MEASURED from there is walked
		// through AddMovementInput and real collision. The teleport itself is a discontinuity, which
		// ServerUpdateTrail already handles by restarting the trace, so no measured trace is ever laid
		// across one.
		if (State.TargetPhase == 0 && !State.bPlacedAtTarget)
		{
			State.bPlacedAtTarget = true;
			Carrier->SetActorLocation(Target.Stand + Target.Normal * 200.0, /*bSweep=*/false, nullptr,
				ETeleportType::TeleportPhysics);
		}

		FVector ToFace = Target.Face - At;
		ToFace.Z = 0.0;
		const double Distance = ToFace.Size();

		const double Speed = Carrier->GetVelocity().Size2D();
		State.StallFrames = (Speed < 60.0) ? (State.StallFrames + 1) : 0;

		FVector Steer = FVector::ZeroVector;

		if (State.TargetPhase == 0)
		{
			// APPROACH. Straight at the verified standing spot; collision decides where that stops.
			FVector ToStand = Target.Stand - At;
			ToStand.Z = 0.0;

			// v14 §1: STEER AT A POINT INSIDE THE STRUCTURE, NOT AT THE SPOT BESIDE IT.
			//
			// This is the fix for "the fixture cannot go red". Aiming at Stand — the place a capsule
			// was PROVEN to fit, often 2uu off a rendered face — sounds like the tightest thing that
			// can be asked for, and it is not: AddMovementInput is an acceleration into a movement
			// component with friction and a collision solve, so a body converging on a waypoint slows
			// as it arrives and settles a body-radius short of it. Five runs measured that as 44-48uu
			// from a surface, against the 36.3uu of drawn reach the run has to beat to be a test at
			// all. Aiming PAST the face instead means the input never eases off and the only thing
			// that decides the standoff is the collision solve — which is the definition of "as close
			// as anyone can get", and is what the credential claims to be measuring.
			Steer = (ToStand - Target.Normal * WallPressDepth).GetSafeNormal();

			// 45uu, not 90. At 90 the drive declared ARRIVED — in its own log — while still 83uu from a
			// spot a capsule had been measured to fit 2.0uu off the surface, and then hugged from there,
			// which is the entire reason the credential sat at ~48uu across five runs.
			const bool bArrived = (ToStand.Size() < 45.0);
			const bool bStalled = (State.StallFrames > 30);
			if (bArrived || bStalled || State.TargetSeconds > 5.0)
			{
				// SAID OUT LOUD, per target. Whether the drive ARRIVED is the difference between a run
				// that tested something and a run that jogged at a wall it could not reach, and it used
				// to be invisible in the output — leaving the final credential as the only clue that
				// anything was wrong, with nothing to say why.
				UE_LOG(LogTraceGame, Display,
					TEXT("[WALLCLIP] target %d/%d %s: %s after %.1fs, %.0fuu from the spot a capsule "
					     "fits %.1fuu off %s."),
					(State.TargetIndex % FMath::Max(1, State.Targets.Num())) + 1, State.Targets.Num(),
					*Target.Name,
					bArrived ? TEXT("ARRIVED") : (bStalled ? TEXT("STALLED") : TEXT("TIMED OUT")),
					State.TargetSeconds, ToStand.Size(), Target.Achieved, *Target.Name);

				State.TargetPhase = 1;
				State.TargetSeconds = 0.0;
				State.StallFrames = 0;

				// Run along the face in whichever direction has more of it left, so the body traverses
				// the surface before meeting a corner rather than starting on top of one.
				const FVector Tangent = FVector::CrossProduct(FVector::UpVector, Target.Normal);
				State.TangentSign = (FVector::DotProduct(At - Target.Face, Tangent) > 0.0) ? -1.0 : 1.0;
			}
		}
		else
		{
			// HUG — AS A WAYPOINT ON THE VERIFIED SPOT, SWEPT ALONG THE FACE.
			//
			// The previous version steered along the tangent and leaned toward the surface with a
			// proportional gain. That sounds equivalent and is not: the lean is one component of a
			// NORMALISED vector, so the harder the body is pushed along the face the less of the input
			// is left pointing at it, and the run settled 47.7uu out from a face a capsule had been
			// measured to fit 2.0uu from. Steering at an actual POINT spends whatever fraction of the
			// input it takes to get there.
			//
			// The point slides back and forth along the face, so the body runs ALONG the surface
			// laying trace beside it and turns around at each end — and a turnaround pressed against a
			// wall is the sharpest corner the trace can be asked to draw, which is the case in the
			// report.
			// PING-PONG BETWEEN TWO FIXED POINTS ON THE FACE, not a moving one.
			//
			// A sinusoidal waypoint slides continuously, so the body chases it on a chord and settles
			// into an orbit ~80uu out instead of converging onto the surface. Two fixed ends, switched
			// only on arrival, give the body something to actually reach: it presses in, runs the
			// length of the face, turns around hard at the end — the sharpest corner the trace can be
			// asked to draw, pressed against a wall — and comes back.
			// THE WAYPOINT TRACKS THE BODY ALONG THE FACE — it does not sit at the far end of it.
			//
			// Two fixed ends 360uu apart still leave the body cutting the chord between them, so it
			// pressed to 43.6uu at the ends and bowed out in the middle. Projecting the body onto the
			// face line and putting the target a short step further along means the target is never far
			// off the body's own beam, so the whole run is spent at the face's distance instead of only
			// the turnarounds. The sign flips at the ends of the face, which is where the corner is.
			const FVector Tangent = FVector::CrossProduct(FVector::UpVector, Target.Normal);
			const double Along = FVector::DotProduct(At - Target.Stand, Tangent);

			if (FMath::Abs(Along) > 200.0)
			{
				State.TangentSign = (Along > 0.0) ? -1.0 : 1.0;
			}

			// v14 §1: AND THE WAYPOINT IS PUT INSIDE THE STRUCTURE, for the same reason the approach
			// now is. A waypoint on the verified standing line is a point the body converges ON, and
			// converging is exactly the behaviour that leaves it a body-radius short. Sunk
			// WallPressDepth past the face, the lateral component of the input never decays, the body
			// runs the length of the surface HELD against it, and the standoff is decided by collision
			// rather than by how a proportional controller settles. The tangential half is unchanged,
			// so the turnarounds at the ends of the face — the sharpest corner the trace can be asked
			// to draw, pressed against a wall — still happen.
			const FVector Waypoint = Target.Stand + Tangent * (Along + 150.0 * State.TangentSign)
				- Target.Normal * WallPressDepth;

			FVector ToWaypoint = Waypoint - At;
			ToWaypoint.Z = 0.0;
			Steer = ToWaypoint.GetSafeNormal();

			const bool bStalled = (State.StallFrames > 90);
			if (bStalled || State.TargetSeconds > State.SecondsPerTarget)
			{
				++State.TargetIndex;
				State.TargetPhase = 0;
				State.TargetSeconds = 0.0;
				State.StallFrames = 0;
				State.bPlacedAtTarget = false;
				++State.TargetsVisited;
			}
		}

		if (Steer.IsNearlyZero())
		{
			return;
		}

		Carrier->AddMovementInput(Steer, 1.f);
	}

	bool TickWallClip(FWallClipState& State, float DeltaTime)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		if (World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[WALLCLIP] the trace is laid on the SERVER; run this on the host."));
			return false;
		}

		++State.Frames;

		// ---- phase 0: arm, and get the Core onto somebody who can be driven ---------------------
		if (State.Phase == 0)
		{
			if (!State.bArmSaved)
			{
				State.SavedArm = GWallFit;
				State.bArmSaved = true;

				if (State.Arm >= 0)
				{
					GWallFit = State.Arm;
				}

				UTraceTrailComponent::ResetWallFitStats();

				// RESOLVED values, not the raw globals: since the integration pass those are -1
				// sentinels meaning "follow the setting", and printing -1 here would make an arm that
				// is running perfectly well off Config/DefaultGame.ini look disabled.
				UE_LOG(LogTraceGame, Display,
					TEXT("[WALLCLIP] arm wallFit=%d (margin %.1fuu, max push %.1fuu, insert "
					     "budget %d). Trace half width %.1fuu, height %.1fuu, point spacing %.0fuu."),
					WallFitEnabled() ? 1 : 0, WallFitMargin(), WallFitMaxPush(), WallFitMaxInsert(),
					UTraceTrailComponent::GetTraceTrailRadius(),
					UTraceTrailComponent::GetTraceTrailHeight(),
					UTraceSettings::Get().TrailPointSpacing);
			}

			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);

			// IT MUST BE THE LOCAL PLAYER'S PAWN, NOT WHOEVER HAPPENS TO BE HOLDING.
			//
			// The previous version took any carrier and, if there was none, granted the Core to the
			// local human. In a live bot match there is essentially always a carrier and it is
			// essentially always a bot — so the fixture spent every run adding movement input to a pawn
			// a TraceBotController was ALSO steering, toward its own objective. Measured: the tour
			// found a face a capsule fits 2.0uu from and the body still never got closer than 78.3uu to
			// anything, because the bot was walking it away as fast as the drive walked it in.
			//
			// So the Core is moved onto the local pawn and moved back every time it is lost. Nothing
			// else about possession is faked: TryPickup is the same entry point the game uses, the
			// grace still runs, and the bots still take it off the fixture by dashing its trace — which
			// is why this has to re-assert rather than run once.
			ATraceCharacter* Local = nullptr;
			for (ATraceCharacter* TraceChar : Characters)
			{
				if (IsLocallyControlledHuman(TraceChar) && TraceChar->IsAlive()
					&& TraceChar->Trail != nullptr)
				{
					Local = TraceChar;
					break;
				}
			}

			ATraceCharacter* Carrier = nullptr;
			if (Local != nullptr && Local->IsCarrier())
			{
				Carrier = Local;
			}
			else if (Local != nullptr)
			{
				if (ATraceCore* Core = ATraceCore::Get(World))
				{
					Core->TryPickup(Local);
				}
				if (Local->IsCarrier())
				{
					Carrier = Local;
				}
			}
			else
			{
				// No local pawn at all (a dedicated server). Fall back to any carrier and say so — the
				// drive will be fighting a bot brain and the credential line is the thing to read.
				State.bDriveLocalOnly = false;
				for (ATraceCharacter* TraceChar : Characters)
				{
					if (TraceChar != nullptr && TraceChar->IsAlive() && TraceChar->IsCarrier()
						&& TraceChar->Trail != nullptr)
					{
						Carrier = TraceChar;
						break;
					}
				}
			}

			if (Carrier == nullptr)
			{
				if ((State.Frames % 300) == 0)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[WALLCLIP] waiting for a living carrier (%d frames)."), State.Frames);
				}
				if (State.Frames > 1800)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[WALLCLIP] ABORTED — no carrier in 1800 frames."));
					if (State.bArmSaved)
					{
						GWallFit = State.SavedArm;
					}
					return false;
				}
				return true;
			}

			// BUILT ONCE PER COMMAND, not once per carrier. The Core changes hands constantly in a live
			// bot match — a run of this fixture typically sees three or four carriers — and rebuilding
			// the tour each time would restart it at target 0 and the drive would never leave the first
			// structure. The tour is a property of the LEVEL, so it outlives whoever is holding.
			if (State.Geometry.Num() == 0)
			{
				double CapsuleRadius = 34.0;
				double CapsuleHalfHeight = 88.0;
				if (const UCapsuleComponent* Capsule = Carrier->GetCapsuleComponent())
				{
					CapsuleRadius = Capsule->GetScaledCapsuleRadius();
					CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
				}

				int32 Skipped = 0;
				UTraceTrailComponent::GatherRenderedLevelBoxes(World, State.Geometry, Skipped);

				// THE TOUR IS BUILT FROM THE SAME BOXES THE VERDICT IS MEASURED AGAINST, at the
				// carrier's own body height and over the trace's own vertical band, so the fixture
				// cannot steer at a surface the measurement does not know about or vice versa.
				BuildWallTour(World, State.Geometry, Carrier->GetActorLocation().Z,
					static_cast<double>(UTraceTrailComponent::GetTraceTrailHeight()) * 0.5,
					CapsuleRadius, CapsuleHalfHeight, State.Targets);

				for (const FWallTarget& Target : State.Targets)
				{
					if (Target.Achieved < State.TightestTargetStandoff)
					{
						State.TightestTargetStandoff = Target.Achieved;
						State.TightestTargetName = Target.Name;
					}
				}

				int32 VisualBoxes = 0;
				int32 VisualCells = 0;
				bool bVisualBuilt = false;
				UTraceTrailComponent::GetLevelVisualIndexStats(VisualBoxes, VisualCells, bVisualBuilt);

				const double DrawnReach = UTraceTrailComponent::GetTraceDrawnHalfReach();

				UE_LOG(LogTraceGame, Display,
					TEXT("[WALLCLIP] carrier %s; %d rendered arena pieces to test against (%d degenerate "
					     "instances skipped). Tour: %d faces a capsule was PROVEN to fit against; tightest "
					     "%.1fuu on %s, against a drawn reach of %.1fuu -> the fixture %s. Fitter's own "
					     "rendered index: built=%d, %d boxes."),
					*GetNameSafe(Carrier), State.Geometry.Num(), Skipped, State.Targets.Num(),
					(State.TightestTargetStandoff == TNumericLimits<double>::Max())
						? -1.0 : State.TightestTargetStandoff,
					State.TightestTargetName.IsEmpty() ? TEXT("(nothing)") : *State.TightestTargetName,
					DrawnReach,
					(State.TightestTargetStandoff <= DrawnReach)
						? TEXT("CAN reach a place the ribbon would enter geometry")
						: TEXT("*** cannot reach anywhere the ribbon could enter geometry — this run "
						       "cannot go red ***"),
					bVisualBuilt ? 1 : 0, VisualBoxes);

				if (State.Targets.Num() == 0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[WALLCLIP] no reachable faces at the carrier's height — the drive will "
						     "wander and the verdict will read INVALID. That is the honest outcome, not "
						     "a pass."));
				}
			}

			// ONE teleport, to the first target's approach lane, and then nothing but walking. It is
			// here because a 40s run that spends 20 of them jogging across an empty arena measures
			// nothing; every position the trace is actually laid at from here on is walked.
			if (!State.bTeleportedToStart && State.Targets.Num() > 0)
			{
				State.bTeleportedToStart = true;

				const FWallTarget& First = State.Targets[0];
				const FVector Approach = First.Stand + First.Normal * 200.0;

				if (Carrier->SetActorLocation(Approach, /*bSweep=*/false, nullptr,
					ETeleportType::TeleportPhysics))
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[WALLCLIP] placed the carrier at %s, 200uu off the tightest reachable face "
						     "(%s, capsule fits at %.1fuu). Everything after this is walked through "
						     "AddMovementInput and real collision."),
						*Approach.ToCompactString(), *First.Name, First.Achieved);
				}
			}

			State.Carrier = Carrier;
			State.Phase = 1;
			State.Frames = 0;
			return true;
		}

		// ---- phase 1: drive along the walls, measuring every frame -------------------------------
		if (State.Phase == 1)
		{
			ATraceCharacter* Carrier = State.Carrier.Get();
			if (Carrier == nullptr || !Carrier->IsAlive() || !Carrier->IsCarrier()
				|| Carrier->Trail == nullptr
				|| (State.bDriveLocalOnly && !IsLocallyControlledHuman(Carrier)))
			{
				// LOST THE FIXTURE — and this stops the clock rather than measuring anyway.
				//
				// The carrier dies constantly in a live bot match (the enemy dashes the very trace this
				// command is measuring), and the Core then lands on a bot the fixture does not steer.
				// The previous version kept driving and kept counting, so most of a run's frames were
				// spent measuring a bot walking wherever its own brain wanted — which is the second
				// reason the credential never came down. State.Elapsed only advances while the fixture
				// genuinely owns the pawn, so the run simply takes longer in wall-clock and every
				// measured frame is a frame the drive was in charge of.
				State.Carrier = nullptr;
				State.Phase = 0;
				return true;
			}

			DriveWallTour(Carrier, State, DeltaTime);
			State.MinStandoff = FMath::Min(State.MinStandoff, State.TightestTargetStandoff);

			UTraceTrailComponent::FTraceClipSample Sample;
			Carrier->Trail->MeasureWorldClipping(State.Geometry, Sample);

			++State.FramesMeasured;
			State.MaxPoints = FMath::Max(State.MaxPoints, Sample.LethalPoints);
			State.NearestSurface = FMath::Min(State.NearestSurface, Sample.DrawnNearestSurface);
			if (Sample.CarrierNearestSurface < State.NearestCarrier)
			{
				State.NearestCarrier = Sample.CarrierNearestSurface;
				State.NearestCarrierPiece = Sample.CarrierNearestPiece;
			}

			if (Sample.DrawnDepth > 0.0)
			{
				++State.FramesWithDrawnClip;
			}
			if (Sample.LethalDepth > 0.0)
			{
				++State.FramesWithLethalClip;
			}

			if (Sample.DrawnDepth > State.WorstDrawn)
			{
				State.WorstDrawn = Sample.DrawnDepth;
				State.WorstDrawnAt = Sample.DrawnWorst;
				State.WorstDrawnPushOut = Sample.DrawnWorstPushOut;
				State.WorstDrawnPiece = Sample.DrawnWorstPiece;
			}
			if (Sample.LethalDepth > State.WorstLethal)
			{
				State.WorstLethal = Sample.LethalDepth;
				State.WorstLethalAt = Sample.LethalWorst;
				State.WorstLethalPiece = Sample.LethalWorstPiece;
			}

			// THE STANDING INVARIANT, tracked across the whole run rather than per frame, because a
			// single frame of an invisible kill volume is a bug the whole run has to report.
			if (Sample.DrawnOutsideLethal > State.WorstDrawnOutsideLethal)
			{
				State.WorstDrawnOutsideLethal = Sample.DrawnOutsideLethal;
				State.WorstDrawnOutsideLethalAt = Sample.DrawnOutsideLethalAt;
			}
			State.WorstDrawnOutsideLethalVertical = FMath::Max(
				State.WorstDrawnOutsideLethalVertical, Sample.DrawnOutsideLethalVertical);
			State.WorstDrawnPastEnd = FMath::Max(State.WorstDrawnPastEnd, Sample.DrawnPastEnd);
			if (Sample.LethalOutsideDrawn > State.WorstLethalOutsideDrawn)
			{
				State.WorstLethalOutsideDrawn = Sample.LethalOutsideDrawn;
				State.WorstLethalOutsideDrawnAt = Sample.LethalOutsideDrawnAt;
				State.bWorstLethalOutsideDrawnIsEndCap = Sample.bLethalOutsideDrawnIsEndCap;
			}

			State.Elapsed += DeltaTime;
			if (State.Elapsed < State.Seconds)
			{
				return true;
			}

			// ---- the report ----------------------------------------------------------------------
			int32 Routed = 0;
			int32 Inserted = 0;
			int32 Unroutable = 0;
			UTraceTrailComponent::GetWallFitStats(Routed, Inserted, Unroutable);

			int32 Pushes = 0;
			int32 Unpushable = 0;
			double WorstPush = 0.0;
			double WorstResidual = 0.0;
			UTraceTrailComponent::GetWallFitPushStats(Pushes, Unpushable, WorstPush, WorstResidual);

			// 1uu of tolerance, and it is not slack: the probe itself has a 1uu radius and the sample
			// lattice sits 10% inside each surface, so a ribbon laid FLUSH against a wall — which is
			// the correct and desirable outcome of hugging one — reads as a fraction of a uu.
			constexpr double ClipTolerance = 1.0;

			// ==========================================================================================
			// THE FIXTURE'S OWN CREDENTIAL, AND IT IS NOW PART OF THE VERDICT RATHER THAN A FOOTNOTE.
			//
			// The previous version of this command printed "PASS" alongside its own admission that the
			// carrier never got within 59.3uu of anything rendered. Both statements were true and the
			// combination was worthless: a trace 22.5uu wide held 59uu clear of every surface in the
			// level CANNOT clip into one, so the run had no way of going red and proved nothing. That
			// is how this bug survived a pass, and no amount of care in the fix protects against a test
			// that cannot fail.
			//
			// So the threshold is DERIVED rather than typed, from the trace's own drawn reach. If the
			// carrier's body never came within the distance the ribbon actually extends, then no
			// position the trace was laid at could have put geometry inside anything, and the only
			// honest verdict is INVALID — never PASS.
			// ==========================================================================================
			const double DrawnReach = UTraceTrailComponent::GetTraceDrawnHalfReach();
			const bool bPressed = (State.NearestCarrier <= DrawnReach);
			const bool bDroveEnough = (State.FramesMeasured >= 60) && (State.MaxPoints >= 4)
				&& (State.Geometry.Num() > 0) && (State.Targets.Num() > 0);
			const bool bValid = bPressed && bDroveEnough;

			// THREE INDEPENDENT VERDICTS, REPORTED SEPARATELY AND THEN ANDED.
			//
			// They are separate because they fail for different reasons and a single rolled-up verdict
			// hides an A/B. CLIP is this section's bug. OVERHANG and INVISIBLE are the two directions
			// of the standing invariant, and both are properties of how the ribbon is DRAWN rather than
			// of where the points are — so they read the same on every fitter arm, and folding them
			// into one word would make the fitter's arms indistinguishable.
			const bool bOverhangOk = (State.WorstDrawnOutsideLethal
				<= (DrawnReach - static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius()) + ClipTolerance));
			const bool bVisibleOk = (State.WorstLethalOutsideDrawn <= ClipTolerance);
			const bool bClean = (State.WorstDrawn <= ClipTolerance) && (State.WorstLethal <= ClipTolerance);
			const bool bPass = bValid && bClean && bOverhangOk && bVisibleOk;

			UE_LOG(LogTraceGame, Display,
				TEXT("[WALLCLIP] arm=%d (half-width clearance=%d, rendered-geometry fitting=%d), %.0fs, %d frames measured, longest "
				     "trace %d lethal points, %d tour faces visited.\n"
				     "           DRAWN  worst %.1fuu inside %s, on %d/%d frames (%.1f%%), at %s\n"
				     "           LETHAL worst %.1fuu inside %s, on %d/%d frames (%.1f%%), at %s\n"
				     "           closest approach: ribbon %s, carrier body %s to %s\n"
				     "           FIXTURE CREDENTIAL: body reached %.1fuu of a rendered surface against a "
				     "drawn reach of %.1fuu -> %s\n"
				     "           tightest face the tour could actually reach: %.1fuu (%s)\n"
				     "           INVARIANT/OVERHANG   drawn outside lethal %.1fuu HORIZONTAL at %s "
				     "(budget %.1f) -> %s; %.1fuu vertical (by design, BuildRibbonElements unions the "
				     "ends' bands, capped by Trace.Trail.RibbonVerticalStep); of the horizontal figure "
				     "%.1fuu was past a flat END cap (v14 1: charged, not excused)\n"
				     "           INVARIANT/INVISIBLE  LETHAL outside drawn %.1fuu at %s (must be 0) -> %s%s\n"
				     "           fitter push: %d points moved, worst move %.1fuu, worst residual depth "
				     "%.1fuu, %d could not be improved at all.\n"
				     "           fitter route: %d appends routed, %d points inserted, %d could not be fitted.\n"
				     "           VERDICT: %s"),
				WallFitEnabled() ? 1 : 0, WallClearanceEnabled() ? 1 : 0, WallFitUsesRenderedGeometry() ? 1 : 0,
				State.Elapsed, State.FramesMeasured, State.MaxPoints, State.TargetsVisited,
				State.WorstDrawn,
				State.WorstDrawnPiece.IsEmpty() ? TEXT("(nothing)") : *State.WorstDrawnPiece,
				State.FramesWithDrawnClip, State.FramesMeasured,
				(State.FramesMeasured > 0) ? (100.0 * State.FramesWithDrawnClip / State.FramesMeasured) : 0.0,
				*State.WorstDrawnAt.ToCompactString(),
				State.WorstLethal,
				State.WorstLethalPiece.IsEmpty() ? TEXT("(nothing)") : *State.WorstLethalPiece,
				State.FramesWithLethalClip, State.FramesMeasured,
				(State.FramesMeasured > 0) ? (100.0 * State.FramesWithLethalClip / State.FramesMeasured) : 0.0,
				*State.WorstLethalAt.ToCompactString(),
				// NEVER-MEASURED IS PRINTED AS WORDS, NOT AS -1. These are SIGNED clearances — negative
				// means the ribbon is inside something — so a -1.0 sentinel for "no reading" is
				// indistinguishable from 1uu of real penetration, and a run of this command has already
				// been misread that way.
				(State.NearestSurface == TNumericLimits<double>::Max())
					? TEXT("(never measured)")
					: *FString::Printf(TEXT("%.1fuu"), State.NearestSurface),
				(State.NearestCarrier == TNumericLimits<double>::Max())
					? TEXT("(never measured)")
					: *FString::Printf(TEXT("%.1fuu"), State.NearestCarrier),
				State.NearestCarrierPiece.IsEmpty() ? TEXT("(nothing)") : *State.NearestCarrierPiece,
				(State.NearestCarrier == TNumericLimits<double>::Max()) ? -1.0 : State.NearestCarrier,
				DrawnReach,
				bPressed ? TEXT("the run COULD have gone red")
				         : TEXT("*** THE RUN COULD NOT HAVE GONE RED — this result proves nothing ***"),
				(State.TightestTargetStandoff == TNumericLimits<double>::Max())
					? 0.0 : State.TightestTargetStandoff,
				State.TightestTargetName.IsEmpty() ? TEXT("(nothing)") : *State.TightestTargetName,
				State.WorstDrawnOutsideLethal, *State.WorstDrawnOutsideLethalAt.ToCompactString(),
				DrawnReach - static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius()),
				bOverhangOk ? TEXT("PASS") : TEXT("*** FAIL: the ribbon reaches further sideways than "
				                                  "GetTraceDrawnHalfReach() claims, so the fitter is "
				                                  "clearing less room than the drawing needs ***"),
				State.WorstDrawnOutsideLethalVertical,
				State.WorstDrawnPastEnd,
				State.WorstLethalOutsideDrawn, *State.WorstLethalOutsideDrawnAt.ToCompactString(),
				bVisibleOk ? TEXT("PASS")
				           : TEXT("*** FAIL: INVISIBLE KILL VOLUME ***"),
				(!bVisibleOk && State.bWorstLethalOutsideDrawnIsEndCap)
					? TEXT(" — and it is the trace's own END CAP: the trip test is radial about the "
					       "first and last point while the ribbon keeps its outer elements flush with "
					       "them. That is the v14 1 defect; it should be impossible with "
					       "Trace.Trail.FlatEndCaps 1, so seeing it here means the cap arm is off or "
					       "the flattening is not reaching this path.")
					: TEXT(""),
				Pushes, WorstPush, WorstResidual, Unpushable,
				Routed, Inserted, Unroutable,
				!bValid
					? TEXT("*** INVALID — the fixture never put the trace anywhere it could clip. "
					       "This is NOT a pass. ***")
					: (bPass
						? TEXT("PASS — neither the ribbon nor the kill volume entered the level, and "
						       "both directions of lethal==drawn held.")
						: (bClean
							? TEXT("*** FAIL — the trace stayed out of the level, but lethal != drawn. ***")
							: TEXT("*** FAIL — the trace is inside the level. ***"))));

			// The diagnosis, printed rather than left to be inferred from two numbers.
			if (bValid && !bClean)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[WALLCLIP] drawn-minus-lethal = %+.1fuu. %s"),
					State.WorstDrawn - State.WorstLethal,
					(State.WorstLethal > ClipTolerance)
						? TEXT("The LETHAL polyline is inside the structure too, so this is the PATH cutting "
						       "the corner and not a drawing artefact — a fix that only moved the ribbon "
						       "would leave an invisible kill volume behind in the wall.")
						: TEXT("Only the DRAWING is inside the structure, i.e. the ribbon is reaching past "
						       "the volume that kills — check GetTraceDrawnHalfReach against the INVARIANT "
						       "line above; if the overhang exceeds it, the fitter's arithmetic is wrong."));
			}

			State.Phase = 2;
			State.Frames = 0;
			return true;
		}

		// ---- phase 2: point a camera at the worst offender and photograph it ---------------------
		{
			++State.ShotPhaseFrames;

			APlayerController* PC = World->GetFirstPlayerController();
			if (PC == nullptr || State.WorstDrawnAt.IsNearlyZero())
			{
				if (State.WorstDrawnAt.IsNearlyZero())
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[WALLCLIP] nothing to photograph — the ribbon never entered the level."));
				}
				if (State.bArmSaved)
				{
					GWallFit = State.SavedArm;
				}
				return false;
			}

			if (State.ShotPhaseFrames == 1)
			{
				// Stand off along the direction the trace would have to move to get OUT of the wall,
				// which is by construction the open side, so the camera is never inside the geometry
				// it is photographing.
				FVector Out = State.WorstDrawnPushOut;
				Out.Z = 0.0;
				if (!Out.Normalize())
				{
					Out = FVector::ForwardVector;
				}

				// Far enough back that the STRUCTURE and the ribbon running into it are both in frame.
				// At close range the camera simply presses against the face the ribbon is buried
				// behind, and an occluded ribbon photographs as nothing at all.
				const FVector Eye = State.WorstDrawnAt + Out * 700.0 + FVector(0.0, 0.0, 380.0);
				const FRotator Look = (State.WorstDrawnAt - Eye).Rotation();

				if (ACameraActor* Camera = World->SpawnActor<ACameraActor>(Eye, Look))
				{
					State.Camera = Camera;
					PC->SetViewTarget(Camera);
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[WALLCLIP] camera at %s looking at the worst penetration (%.1fuu) at %s."),
					*Eye.ToCompactString(), State.WorstDrawn, *State.WorstDrawnAt.ToCompactString());
				return true;
			}

			// A few frames for the view target to take and the ribbon to be re-placed under it.
			if (State.ShotPhaseFrames == 12)
			{
				State.ShotPath = FPaths::ConvertRelativePathToFull(
					FPaths::ProjectSavedDir() / TEXT("Screenshots")
					/ FString::Printf(TEXT("WallClip_arm%d_%s.png"), WallFitEnabled() ? 1 : 0,
						*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));

				FScreenshotRequest::RequestScreenshot(State.ShotPath, /*bShowUI=*/false,
					/*bAddFilenameSuffix=*/false);

				UE_LOG(LogTraceGame, Display, TEXT("[WALLCLIP] screenshot requested: %s"), *State.ShotPath);
				return true;
			}

			if (State.ShotPhaseFrames >= 40)
			{
				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				if (!State.ShotPath.IsEmpty() && PlatformFile.FileExists(*State.ShotPath))
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[WALLCLIP] Screenshot written (%lld bytes): %s"),
						PlatformFile.FileSize(*State.ShotPath), *State.ShotPath);
				}
				else
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[WALLCLIP] no screenshot file appeared at: %s"), *State.ShotPath);
				}

				if (ACameraActor* Camera = State.Camera.Get())
				{
					Camera->Destroy();
				}
				if (State.bArmSaved)
				{
					GWallFit = State.SavedArm;
				}
				return false;
			}

			return true;
		}
	}

	FAutoConsoleCommand CmdWallClip(
		TEXT("Trace.Trail.WallClip"),
		TEXT("Trace.Trail.WallClip [Seconds=40] [Arm=-1] — spec v12 6. SERVER. Drive a carrier along the "
		     "level's walls and round its corners, and measure every frame how deep the trace is INSIDE "
		     "the level — the DRAWN ribbon and the LETHAL column reported separately so the two can be "
		     "compared. Ends with a screenshot of the worst offender. Arm 0 forces the pre-v12 straight "
		     "chord (the reported bug, reproduced); arm 1 forces the corner fitter; -1 leaves "
		     "Trace.Trail.WallFit alone."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FWallClipState State;
			if (Args.Num() > 0)
			{
				State.Seconds = FMath::Clamp(FCString::Atod(*Args[0]), 2.0, 600.0);
			}
			if (Args.Num() > 1)
			{
				State.Arm = FMath::Clamp(FCString::Atoi(*Args[1]), -1, 1);
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[WALLCLIP] spec v12 6: 'the trace is clipping into walls sometimes, when a model runs "
				     "close to a corner/structure'. Driving a carrier along the walls for %.0fs."),
				State.Seconds);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickWallClip(State, DeltaTime);
				}), 0.f);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.WallFitLive — SPEC v13 §7, THE STANDING INVARIANT, FIRED AT LIVE
	// ---------------------------------------------------------------------------------------------
	//
	// "Pull the visual out of a wall and the trip volume must follow. State exactly how they stay in
	// agreement, and verify BOTH directions: dash the visible ribbon and die, dash where it is not
	// drawn and live."
	//
	// HOW THEY STAY IN AGREEMENT, stated once and then tested here rather than asserted:
	//
	//   There is ONE polyline. TrailPoints is the only geometry the server owns. RebuildRibbon builds
	//   the drawing from it and ServerRunTripTest walks the same array; nothing anywhere moves one
	//   without the other because there is no second copy to move. The wall fix is deliberately and
	//   entirely a change to WHERE POINTS ARE PUT (FitPointToWorld) and never to how either half is
	//   derived from them — that is why a fix which pulls the ribbon out of a wall cannot leave a kill
	//   volume behind in it, and it is the reason the fix lives where it does.
	//
	// That is an argument. This command is the evidence. Two cases, through the same staged probe the
	// whole-model test uses, so the dash is a REAL dash and the trip test is the REAL trip test:
	//
	//   ON  — the dasher's capsule centre on the trace's own centreline. Must be inside something
	//         drawn, and must kill.
	//   OFF — the dasher placed beyond the whole model's reach. Must be outside everything drawn, and
	//         must NOT kill.
	//
	// EACH CASE CHECKS THE DRAWING ITSELF, not just the trip outcome. A run where the ON case killed
	// but nothing was drawn there would be an invisible kill volume passing a kill test, which is
	// exactly the failure this is here to catch, so "was it drawn" is a separate assertion from "did
	// it kill" and both have to hold.

	struct FWallLiveCase
	{
		const TCHAR* Name;
		bool bOnTheTrace;
		const TCHAR* Expectation;
	};

	const FWallLiveCase WallLiveCases[] =
	{
		{ TEXT("ON  (dash the drawn ribbon)"),  true,
		  TEXT("must be DRAWN here and must KILL") },
		{ TEXT("OFF (dash where nothing is drawn)"), false,
		  TEXT("must be NOT DRAWN here and must NOT kill") }
	};

	constexpr int32 WallLiveCaseCount = UE_ARRAY_COUNT(WallLiveCases);

	struct FWallLiveState
	{
		int32 CaseIndex = 0;
		int32 Phase = 0;
		int32 IdleFrames = 0;

		int32 Passed = 0;
		int32 Skipped = 0;
		TArray<FString> Failures;

		TWeakObjectPtr<ATraceCharacter> Holder;
		TWeakObjectPtr<ATraceCharacter> Dasher;
		FString HolderName;
		FString DasherName;

		int32 SerialBefore = 0;
		double ModelRadius = 34.0;
		double Offset = 0.0;
	};

	bool TickWallFitLive(FWallLiveState& State, float DeltaTime)
	{
		UWorld* World = FindTrailDebugWorld();
		if (World == nullptr)
		{
			return false;
		}

		if (World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[WALLLIVE] the trip test runs on the SERVER; run this on the host."));
			return false;
		}

		if (State.CaseIndex >= WallLiveCaseCount)
		{
			const bool bAllPassed = (State.Passed == WallLiveCaseCount);
			UE_LOG(LogTraceGame, Display,
				TEXT("[WALLLIVE] DONE. %d/%d passed, %d skipped. %s%s"),
				State.Passed, WallLiveCaseCount, State.Skipped,
				bAllPassed
					? TEXT("LETHAL == DRAWN, both directions: the ribbon that is on screen killed, and "
					       "the place it is not on screen did not.")
					: TEXT("*** FAIL *** "),
				State.Failures.Num() > 0
					? *FString::Printf(TEXT("Failures: %s"), *FString::Join(State.Failures, TEXT("; ")))
					: TEXT(""));

			GTraceModelProbe.bArmed = false;
			return false;
		}

		const FWallLiveCase& Case = WallLiveCases[State.CaseIndex];

		// ---- phase 0: find a carrier with a real trace and a living enemy -------------------------
		if (State.Phase == 0)
		{
			TArray<ATraceCharacter*> Characters;
			GatherTrailDebugCharacters(World, Characters);

			ATraceCharacter* Holder = nullptr;
			for (ATraceCharacter* TraceChar : Characters)
			{
				if (TraceChar != nullptr && TraceChar->IsAlive() && TraceChar->IsCarrier()
					&& TraceChar->Trail != nullptr
					&& TraceChar->Trail->TrailPoints.Items.Num() >= 6)
				{
					Holder = TraceChar;
					break;
				}
			}

			ATraceCharacter* Dasher = nullptr;
			if (Holder != nullptr)
			{
				for (ATraceCharacter* TraceChar : Characters)
				{
					if (TraceChar != nullptr && TraceChar != Holder && TraceChar->IsAlive()
						&& TraceChar->GetTeam() != ETraceTeam::None
						&& TraceChar->GetTeam() != Holder->GetTeam()
						&& TraceChar->GetTraceMovement() != nullptr)
					{
						Dasher = TraceChar;
						break;
					}
				}
			}

			if (Holder == nullptr || Dasher == nullptr)
			{
				if ((++State.IdleFrames % 300) == 0)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[WALLLIVE] %s: waiting for a carrier with 6+ points and a living enemy (%d "
						     "frames)."), Case.Name, State.IdleFrames);
				}
				if (State.IdleFrames > 3600)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[WALLLIVE] %s: SKIPPED — no setup in 3600 frames."), Case.Name);
					++State.Skipped;
					++State.CaseIndex;
					State.IdleFrames = 0;
				}
				return true;
			}

			State.IdleFrames = 0;

			// THE OFFSET. Zero is the centreline. The OFF case steps past the WHOLE MODEL's reach —
			// not the capsule's — because the trip test is a whole-model test since v10 §2, and an
			// offset chosen off the capsule would put the model's shoulder inside the ribbon and the
			// case would fail for a reason that has nothing to do with this section.
			const FTraceModelReach Reach = UTraceTrailComponent::MeasureModelReach(Dasher);
			State.ModelRadius = Reach.EffectiveRadius;

			const double TrailRadius = static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius());
			State.Offset = Case.bOnTheTrace ? 0.0 : (TrailRadius + State.ModelRadius + 12.0);

			State.Holder = Holder;
			State.Dasher = Dasher;
			State.HolderName = GetNameSafe(Holder);
			State.DasherName = GetNameSafe(Dasher);
			State.SerialBefore = GModelTripSerial;

			GTraceModelProbe.Holder = Holder;
			GTraceModelProbe.Dasher = Dasher;
			GTraceModelProbe.Offset = State.Offset;
			GTraceModelProbe.bRequireParry = false;
			GTraceModelProbe.bApplied = false;
			GTraceModelProbe.bArmed = true;
			GTraceModelProbe.LastRefusal = TEXT("armed, waiting for the trip test");

			UE_LOG(LogTraceGame, Display,
				TEXT("[WALLLIVE] CASE %s — %s. %s dashes %s's trace at an offset of %.1fuu (trace half "
				     "width %.1f, dasher model reach %.1f)."),
				Case.Name, Case.Expectation, *State.DasherName, *State.HolderName, State.Offset,
				TrailRadius, State.ModelRadius);

			State.Phase = 1;
			return true;
		}

		// ---- phase 1: make the pawn really dash and wait for the probe to fire --------------------
		if (State.Phase == 1)
		{
			// The probe having fired is checked FIRST: on a scored trip somebody dies on the same
			// frame, so a liveness guard above this would report "the setup fell apart" about the very
			// outcome the case is testing for. (That mistake cost the whole-model harness its verdict
			// once; see TickModelHitTest.)
			if (GTraceModelProbe.bApplied)
			{
				State.Phase = 2;
				return true;
			}

			ATraceCharacter* Dasher = State.Dasher.Get();
			ATraceCharacter* Holder = State.Holder.Get();

			if (Dasher == nullptr || !Dasher->IsAlive() || Holder == nullptr || !Holder->IsAlive()
				|| !Holder->IsCarrier() || Holder->Trail == nullptr
				|| Holder->Trail->TrailPoints.Items.Num() < 2)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[WALLLIVE] %s: the setup fell apart before the probe fired. Retrying."),
					Case.Name);
				GTraceModelProbe.bArmed = false;
				State.Phase = 0;
				return true;
			}

			if (UTraceCharacterMovementComponent* Movement = Dasher->GetTraceMovement())
			{
				Movement->StartDash();
			}
			GTraceModelProbe.bArmed = true;

			if ((++State.IdleFrames % 300) == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[WALLLIVE] %s: waiting for %s to be dashing on a trip-test frame (%d frames). "
					     "Probe: %s"),
					Case.Name, *State.DasherName, State.IdleFrames, GTraceModelProbe.LastRefusal);
			}
			if (State.IdleFrames > 3600)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[WALLLIVE] %s: SKIPPED — the probe never fired in 3600 frames (%s)."),
					Case.Name, GTraceModelProbe.LastRefusal);
				GTraceModelProbe.bArmed = false;
				++State.Skipped;
				++State.CaseIndex;
				State.IdleFrames = 0;
				State.Phase = 0;
			}
			return true;
		}

		// ---- phase 2: the verdict, on BOTH halves of the invariant --------------------------------
		{
			State.IdleFrames = 0;

			ATraceCharacter* Holder = State.Holder.Get();
			ATraceCharacter* Dasher = State.Dasher.Get();

			// THE HOLDER IS PART OF THE TEST, not just the dasher. See GModelTripLastHolder: the staged
			// pawn goes on dashing after it is placed, and a trip it scores on somebody ELSE's trace is
			// not evidence about this case. Attributing one cost this command a false
			// "invisible kill volume" on a placement 12uu beyond anything the trip test can reach.
			const bool bTripped = (GModelTripSerial != State.SerialBefore)
				&& (GModelTripLastDasher.Get() == Dasher)
				&& (GModelTripLastHolder.Get() == Holder);
			const bool bHolderAlive = (Holder != nullptr) && Holder->IsAlive();

			// WAS ANYTHING DRAWN WHERE THE DASH HAPPENED? Taken from the probe, which sampled it on
			// the frame it PLACED the pawn — before the trip test ran, and before a scored kill wiped
			// the trace. Sampling it here instead reported "nothing was drawn there" about every
			// successful kill, because by this phase possession has ended and ClearTrail has correctly
			// removed the drawing (spec 9's instant clear). The first run of this command called that
			// an INVISIBLE KILL VOLUME on a kill that had been perfectly visible.
			const double DrawnAtCentre = GTraceModelProbe.DrawnAtCentre;
			const double DrawnAtModelEdge = GTraceModelProbe.DrawnAtModelEdge;

			const bool bWantTrip = Case.bOnTheTrace;
			const bool bTripOk = (bTripped == bWantTrip);

			// The DRAWING assertion. ON: the dash point is inside a visible piece. OFF: no part of the
			// model is inside any visible piece.
			const bool bDrawOk = Case.bOnTheTrace
				? (DrawnAtCentre >= 0.0 && DrawnAtCentre <= 0.5)
				: (DrawnAtModelEdge > 0.5);

			bool bOk = bTripOk && bDrawOk;
			FString Detail;

			if (!bDrawOk)
			{
				Detail = Case.bOnTheTrace
					? TEXT("the dash landed on the lethal centreline but NOTHING WAS DRAWN THERE — an "
					       "invisible kill volume")
					: TEXT("the 'not drawn' case was placed somewhere ribbon IS drawn — the case proves "
					       "nothing, not that the game is wrong");
			}
			else if (!bTripOk)
			{
				Detail = bWantTrip
					? TEXT("visible ribbon that did not kill")
					: TEXT("a kill with no ribbon on screen at the dasher — an invisible kill volume");
			}
			else if (bWantTrip && bHolderAlive)
			{
				bOk = false;
				Detail = TEXT("connection scored but the CARRIER survived");
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[WALLLIVE] %s: connection=%d (wanted %d) at offset %.1fuu. Drawn ribbon distance: "
				     "%.1fuu at the capsule centre, %.1fuu at the model's near edge (0 = inside "
				     "something on screen). Carrier alive=%d. %s%s"),
				Case.Name, bTripped ? 1 : 0, bWantTrip ? 1 : 0, GTraceModelProbe.AppliedClearance,
				DrawnAtCentre, DrawnAtModelEdge, bHolderAlive ? 1 : 0,
				bOk ? TEXT("PASS") : TEXT("*** FAIL ***"),
				Detail.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" — %s"), *Detail));

			if (bOk)
			{
				++State.Passed;
			}
			else
			{
				State.Failures.Add(FString::Printf(TEXT("%s (%s)"), Case.Name,
					Detail.IsEmpty() ? TEXT("outcome mismatch") : *Detail));
			}

			GTraceModelProbe.bArmed = false;
			++State.CaseIndex;
			State.Phase = 0;
			return true;
		}
	}

	FAutoConsoleCommand CmdWallFitLive(
		TEXT("Trace.Trail.WallFitLive"),
		TEXT("Trace.Trail.WallFitLive — spec v13 7. SERVER. The standing invariant, fired at live: dash a "
		     "real enemy through the trace's centreline (must be DRAWN there and must KILL), then past the "
		     "whole model's reach (must be NOT DRAWN there and must NOT kill). Each case asserts what is on "
		     "SCREEN at the dash point as well as what the trip test did, so a kill with no ribbon — an "
		     "invisible kill volume — fails even though somebody died."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& /*Args*/)
		{
			FWallLiveState State;

			UE_LOG(LogTraceGame, Display,
				TEXT("[WALLLIVE] spec v13 7: 'lethal must equal drawn'. %d cases; the second is the RED arm "
				     "and MUST report no connection."),
				WallLiveCaseCount);

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([State](float DeltaTime) mutable -> bool
				{
					return TickWallFitLive(State, DeltaTime);
				}), 0.f);
		}));

	// ---------------------------------------------------------------------------------------------
	// Trace.Trail.LethalDrawn — SPEC v14 §1, THE INVARIANT WITH NOTHING BETWEEN IT AND THE GEOMETRY
	// ---------------------------------------------------------------------------------------------
	//
	// "Fix drawn outside lethal and lethal outside drawn bug. Ensure that the trace shows exactly
	// what is lethal."
	//
	// WHY THIS EXISTS ALONGSIDE Trace.Trail.WallClip, WHICH ALREADY MEASURES BOTH DIRECTIONS.
	//
	// WallClip measures them off a live carrier driven round a real level for forty seconds. That is
	// the right instrument for "does the trace enter a wall", and it is a terrible one for "is the
	// drawn solid the lethal solid": whether it ever tests a hairpin, a climb or a two-point trace
	// depends on where the bots pushed the fixture that run, its answer changes between runs, and —
	// as the spec's own note records — it spent two passes reporting VERDICT: INVALID because the
	// drive could not press close enough to a surface to be a test of anything at all. A run that
	// cannot go red teaches nothing, and a run whose coverage is decided by a bot brain cannot be
	// asked whether the END CAPS agree.
	//
	// So this command asks the geometry directly. It builds THE REAL ribbon — the shipping
	// ComputeRibbonSamples and the shipping BuildRibbonElements, not a copy of their maths — over
	// fixture polylines chosen to contain every shape that has ever broken this, and measures both
	// directions against the SAME predicate the server kills with. No world, no pawn, no level, no
	// bots, no frame rate; it runs in a millisecond and gives the same answer every time.
	//
	// AND IT GOES RED ON DEMAND. `Trace.Trail.LethalDrawn 0` forces the pre-v14 arms
	// (Trace.Trail.RibbonUpright 0 + Trace.Trail.FlatEndCaps 0) and reproduces both reported numbers
	// on the shipping build; `Trace.Trail.LethalDrawn 1` runs the identical fixtures with the fix in.
	// Same build, same code path, one pair of cvars between them.

	struct FLethalDrawnFixture
	{
		const TCHAR* Name;
		const TCHAR* Why;
		TArray<FVector> Points;
	};

	/**
	 * THE SHAPES THAT HAVE BROKEN THIS, one fixture each. Spacings are the shipping
	 * TrailPointSpacing (60uu) unless the fixture is specifically about spacing.
	 */
	void BuildLethalDrawnFixtures(TArray<FLethalDrawnFixture>& Out)
	{
		Out.Reset();

		{
			FLethalDrawnFixture& Fixture = Out.AddDefaulted_GetRef();
			Fixture.Name = TEXT("STRAIGHT");
			Fixture.Why = TEXT("the floor: if this is not clean nothing else matters");
			for (int32 Index = 0; Index < 8; ++Index)
			{
				Fixture.Points.Add(FVector(Index * 60.0, 0.0, 0.0));
			}
		}

		{
			FLethalDrawnFixture& Fixture = Out.AddDefaulted_GetRef();
			Fixture.Name = TEXT("RIGHT ANGLE");
			Fixture.Why = TEXT("the joint overlap has to cover the round cap at the corner");
			for (int32 Index = 0; Index < 5; ++Index)
			{
				Fixture.Points.Add(FVector(Index * 60.0, 0.0, 0.0));
			}
			for (int32 Index = 1; Index <= 4; ++Index)
			{
				Fixture.Points.Add(FVector(240.0, Index * 60.0, 0.0));
			}
		}

		{
			FLethalDrawnFixture& Fixture = Out.AddDefaulted_GetRef();
			Fixture.Name = TEXT("HAIRPIN");
			Fixture.Why = TEXT("a 180 turn — the sharpest thing a carrier can lay, and the worst case "
			                   "for a straight box drawing a curve");
			for (int32 Index = 0; Index < 5; ++Index)
			{
				Fixture.Points.Add(FVector(Index * 60.0, 0.0, 0.0));
			}
			for (int32 Index = 1; Index <= 4; ++Index)
			{
				Fixture.Points.Add(FVector(240.0 - Index * 60.0, 70.0, 0.0));
			}
		}

		{
			FLethalDrawnFixture& Fixture = Out.AddDefaulted_GetRef();
			Fixture.Name = TEXT("45 CLIMB");
			Fixture.Why = TEXT("the pitched-cross-section bug: a leaning element leaves the vertical "
			                   "column sideways");
			for (int32 Index = 0; Index < 8; ++Index)
			{
				Fixture.Points.Add(FVector(Index * 42.0, 0.0, Index * 42.0));
			}
		}

		{
			FLethalDrawnFixture& Fixture = Out.AddDefaulted_GetRef();
			Fixture.Name = TEXT("VERTICAL DROP");
			Fixture.Why = TEXT("a fall: in plan the path is a POINT, so the lethal volume is a disc and "
			                   "the drawn primitive is a square");
			for (int32 Index = 0; Index < 6; ++Index)
			{
				Fixture.Points.Add(FVector(0.0, 0.0, -Index * 60.0));
			}
		}

		{
			FLethalDrawnFixture& Fixture = Out.AddDefaulted_GetRef();
			Fixture.Name = TEXT("MIXED SPACING");
			Fixture.Why = TEXT("6uu next to 70uu — what the wall fitter's inserted points and the "
			                   "length trim actually produce, and what the spline clamp is for");
			Fixture.Points.Add(FVector(0.0, 0.0, 0.0));
			Fixture.Points.Add(FVector(70.0, 0.0, 0.0));
			Fixture.Points.Add(FVector(76.0, 0.0, 0.0));
			Fixture.Points.Add(FVector(146.0, 30.0, 0.0));
			Fixture.Points.Add(FVector(152.0, 36.0, 0.0));
			Fixture.Points.Add(FVector(160.0, 106.0, 20.0));
			Fixture.Points.Add(FVector(166.0, 112.0, 26.0));
		}

		{
			FLethalDrawnFixture& Fixture = Out.AddDefaulted_GetRef();
			Fixture.Name = TEXT("TWO POINTS");
			Fixture.Why = TEXT("the shortest trace that has two ends — both caps are outer caps");
			Fixture.Points.Add(FVector(0.0, 0.0, 0.0));
			Fixture.Points.Add(FVector(60.0, 0.0, 0.0));
		}

		{
			FLethalDrawnFixture& Fixture = Out.AddDefaulted_GetRef();
			Fixture.Name = TEXT("ONE POINT");
			Fixture.Why = TEXT("a degenerate but LETHAL trace: SweepIntersectsTrace tests it as a "
			                   "zero-length segment, so something must be drawn on it");
			Fixture.Points.Add(FVector(0.0, 0.0, 0.0));
		}
	}

	struct FLethalDrawnResult
	{
		double DrawnOutsideLethal = 0.0;
		FVector DrawnOutsideLethalAt = FVector::ZeroVector;
		double DrawnOutsideLethalVertical = 0.0;
		double LethalOutsideDrawn = 0.0;
		FVector LethalOutsideDrawnAt = FVector::ZeroVector;
		int32 Elements = 0;
		int32 LethalSamples = 0;
		int32 DrawnSamples = 0;
	};

	void MeasureLethalDrawn(const TArray<FVector>& Points, FLethalDrawnResult& Out)
	{
		Out = FLethalDrawnResult();

		const double Radius = static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius());
		const double Height = static_cast<double>(UTraceTrailComponent::GetTraceTrailHeight());
		const double HalfHeight = Height * 0.5;

		TArray<FVector> Samples;
		TArray<float> Births;
		TArray<float> Slack;
		UTraceTrailComponent::ComputeRibbonSamples(Points, TArray<float>(),
			static_cast<double>(FMath::Max(5.f, GRibbonStep)), MaxRibbonElements,
			Samples, Births, Slack);

		TArray<UTraceTrailComponent::FTraceRibbonElement> Elements;
		UTraceTrailComponent::BuildRibbonElements(Samples, Slack, Radius, Height,
			static_cast<double>(GRibbonWidthScale), /*bOverlapAtStart=*/false, Elements);

		Out.Elements = Elements.Num();

		// ---- DIRECTION ONE: every drawn surface has to be inside the thing that kills -------------
		//
		// Sampled a hair inside each face, because a ribbon laid exactly ON the lethal boundary is the
		// correct outcome and must not read as an overhang.
		for (const UTraceTrailComponent::FTraceRibbonElement& Element : Elements)
		{
			const FVector Half = Element.Size * 0.5;
			const int32 LengthSteps = FMath::Clamp(FMath::CeilToInt(Element.Size.X / 8.0), 1, 48);

			for (int32 Step = 0; Step <= LengthSteps; ++Step)
			{
				const double U = -1.0 + 2.0 * (static_cast<double>(Step) / static_cast<double>(LengthSteps));
				for (int32 WidthIndex = -1; WidthIndex <= 1; ++WidthIndex)
				{
					for (int32 HeightIndex = -1; HeightIndex <= 1; ++HeightIndex)
					{
						const FVector Local(
							U * Half.X * 0.999,
							static_cast<double>(WidthIndex) * Half.Y * 0.999,
							static_cast<double>(HeightIndex) * Half.Z * 0.999);
						const FVector At = Element.Centre + Element.Rotation.RotateVector(Local);

						++Out.DrawnSamples;

						double Horizontal = 0.0;
						double Vertical = 0.0;
						bool bBeyond = false;
						MeasurePointGapToTrace(Points, At, Radius, HalfHeight,
							Horizontal, Vertical, bBeyond);

						if (Horizontal > Out.DrawnOutsideLethal)
						{
							Out.DrawnOutsideLethal = Horizontal;
							Out.DrawnOutsideLethalAt = At;
						}
						Out.DrawnOutsideLethalVertical =
							FMath::Max(Out.DrawnOutsideLethalVertical, Vertical);
					}
				}
			}
		}

		// ---- DIRECTION TWO: every lethal place has to be inside something drawn -------------------
		//
		// A RING at each station, not a rectangle, and every candidate filtered through the real
		// predicate — so the end caps are asked about, in whatever shape they currently have. This is
		// the sampling change that lets the fixture see the 22.5uu the previous one could not.
		static const FVector Bearings[] =
		{
			FVector(0.0, 0.0, 0.0),
			FVector(1.0, 0.0, 0.0),  FVector(-1.0, 0.0, 0.0),
			FVector(0.0, 1.0, 0.0),  FVector(0.0, -1.0, 0.0),
			FVector(0.7071, 0.7071, 0.0),  FVector(-0.7071, 0.7071, 0.0),
			FVector(0.7071, -0.7071, 0.0), FVector(-0.7071, -0.7071, 0.0)
		};
		static const double HeightFractions[] = { -0.999, -0.5, 0.0, 0.5, 0.999 };

		const int32 LastPoint = FMath::Max(0, Points.Num() - 1);
		for (int32 PointIndex = 0; PointIndex <= LastPoint; ++PointIndex)
		{
			const FVector SegStart = Points[PointIndex];
			const FVector SegEnd = Points[FMath::Min(PointIndex + 1, LastPoint)];

			const double Length = FVector::Dist(SegStart, SegEnd);
			const int32 Steps = FMath::Clamp(FMath::CeilToInt(Length / 8.0), 1, 48);

			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const FVector Centre = FMath::Lerp(SegStart, SegEnd,
					static_cast<double>(Step) / static_cast<double>(Steps));

				for (const FVector& Bearing : Bearings)
				{
					for (const double HeightFraction : HeightFractions)
					{
						const FVector At = Centre
							+ Bearing * (Radius * 0.999)
							+ FVector::UpVector * (HeightFraction * HalfHeight);

						if (!IsPointLethal(Points, At, Radius, HalfHeight))
						{
							continue;
						}

						++Out.LethalSamples;

						const double Outside =
							UTraceTrailComponent::DistanceOutsideRibbonElements(Elements, At);
						if (Outside > Out.LethalOutsideDrawn)
						{
							Out.LethalOutsideDrawn = Outside;
							Out.LethalOutsideDrawnAt = At;
						}
					}
				}
			}

			if (PointIndex == LastPoint)
			{
				break;
			}
		}
	}

	FAutoConsoleCommand CmdLethalDrawn(
		TEXT("Trace.Trail.LethalDrawn"),
		TEXT("Trace.Trail.LethalDrawn [Arm=-1] — spec v14 1. Measures BOTH directions of "
		     "'the trace shows exactly what is lethal' on fixture polylines, using the shipping ribbon "
		     "builder and the shipping trip-test geometry. No world, no pawn, no level needed. "
		     "Arm 0 forces the pre-v14 pitched cross-section and radial end caps (the reported bug, "
		     "reproduced); arm 1 forces the v14 arms; -1 measures whatever is configured."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const int32 Arm = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), -1, 1) : -1;

			const int32 SavedUpright = GRibbonUpright;
			const int32 SavedFlatCaps = GTrailFlatEndCaps;
			if (Arm >= 0)
			{
				GRibbonUpright = Arm;
				GTrailFlatEndCaps = Arm;
			}

			const double Radius = static_cast<double>(UTraceTrailComponent::GetTraceTrailRadius());
			const double DrawnReach = UTraceTrailComponent::GetTraceDrawnHalfReach();

			// THE BUDGET, DERIVED AND NOT TYPED. Two terms, both forced by drawing a round-cornered
			// solid with straight boxes:
			//   Radius * (sqrt(2) - 1) = 9.3uu   the corner of the joint-overlap box, which must cover
			//                                    the round cap at every interior joint.
			//   0.0741 * TrailPointSpacing = 4.4uu   the clamped Catmull-Rom slack.
			// That is GetTraceDrawnHalfReach() - TrailRadius. Anything above it is a defect; anything
			// below it is the price of boxes, paid in the OVER-drawing direction.
			const double Budget = DrawnReach - Radius;

			// Direction two has no budget at all. The only tolerance is arithmetic: the drawn faces are
			// sampled at 0.999 of their extents and doubles are doubles.
			constexpr double InvisibleTolerance = 0.05;

			TArray<FLethalDrawnFixture> Fixtures;
			BuildLethalDrawnFixtures(Fixtures);

			UE_LOG(LogTraceGame, Display,
				TEXT("[LETHALDRAWN] spec v14 1: 'ensure that the trace shows exactly what is lethal'. "
				     "arm upright=%d flatEndCaps=%d. Trace half width %.1fuu, height %.1fuu, spacing "
				     "%.0fuu, ribbon step %.0fuu, vertical step %.0fuu. Overhang budget %.1fuu "
				     "(= drawn reach %.1f - half width %.1f); invisible-kill budget is ZERO."),
				GRibbonUpright, GTrailFlatEndCaps, Radius,
				static_cast<double>(UTraceTrailComponent::GetTraceTrailHeight()),
				UTraceSettings::Get().TrailPointSpacing, GRibbonStep, GRibbonVerticalStep,
				Budget, DrawnReach, Radius);

			double WorstOverhang = 0.0;
			double WorstVertical = 0.0;
			double WorstInvisible = 0.0;
			FString WorstOverhangFixture;
			FString WorstInvisibleFixture;
			int32 Failures = 0;

			for (const FLethalDrawnFixture& Fixture : Fixtures)
			{
				FLethalDrawnResult Result;
				MeasureLethalDrawn(Fixture.Points, Result);

				const bool bOverhangOk = (Result.DrawnOutsideLethal <= Budget);
				const bool bVisibleOk = (Result.LethalOutsideDrawn <= InvisibleTolerance);

				// A fixture that drew nothing, or that found no lethal sample, has not tested anything
				// and says so rather than passing quietly.
				const bool bMeasured = (Result.Elements > 0) && (Result.LethalSamples > 0)
					&& (Result.DrawnSamples > 0);

				if (!bMeasured || !bOverhangOk || !bVisibleOk)
				{
					++Failures;
				}

				if (Result.DrawnOutsideLethal > WorstOverhang)
				{
					WorstOverhang = Result.DrawnOutsideLethal;
					WorstOverhangFixture = Fixture.Name;
				}
				WorstVertical = FMath::Max(WorstVertical, Result.DrawnOutsideLethalVertical);
				if (Result.LethalOutsideDrawn > WorstInvisible)
				{
					WorstInvisible = Result.LethalOutsideDrawn;
					WorstInvisibleFixture = Fixture.Name;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[LETHALDRAWN] %-14s %2d pts -> %2d elements, %d lethal / %d drawn samples. "
					     "DRAWN outside lethal %5.1fuu at %s (budget %.1f) %s | vertical %5.1fuu | "
					     "LETHAL outside drawn %5.1fuu at %s %s   [%s]"),
					Fixture.Name, Fixture.Points.Num(), Result.Elements,
					Result.LethalSamples, Result.DrawnSamples,
					Result.DrawnOutsideLethal, *Result.DrawnOutsideLethalAt.ToCompactString(), Budget,
					bOverhangOk ? TEXT("PASS") : TEXT("*** FAIL ***"),
					Result.DrawnOutsideLethalVertical,
					Result.LethalOutsideDrawn, *Result.LethalOutsideDrawnAt.ToCompactString(),
					bVisibleOk ? TEXT("PASS") : TEXT("*** FAIL: INVISIBLE KILL VOLUME ***"),
					bMeasured ? Fixture.Why : TEXT("*** NOTHING MEASURED — this fixture is not a test ***"));
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[LETHALDRAWN] DONE. %d fixtures, %d failed.\n"
				     "           WORST drawn outside lethal  %.1fuu (%s), budget %.1fuu\n"
				     "           WORST vertical over-draw     %.1fuu (deliberate: an element unions its "
				     "two ends' bands; capped by Trace.Trail.RibbonVerticalStep)\n"
				     "           WORST lethal outside drawn   %.1fuu (%s), budget 0\n"
				     "           VERDICT: %s"),
				Fixtures.Num(), Failures,
				WorstOverhang, WorstOverhangFixture.IsEmpty() ? TEXT("-") : *WorstOverhangFixture, Budget,
				WorstVertical,
				WorstInvisible, WorstInvisibleFixture.IsEmpty() ? TEXT("-") : *WorstInvisibleFixture,
				(Failures == 0)
					? TEXT("PASS — the drawn solid and the lethal solid are the same solid, within the "
					       "stated budget, on every fixture.")
					: TEXT("*** FAIL — lethal != drawn. ***"));

			GRibbonUpright = SavedUpright;
			GTrailFlatEndCaps = SavedFlatCaps;
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

double UTraceTrailComponent::DistanceToDrawnRibbon(const FVector& At) const
{
	double Nearest = TNumericLimits<double>::Max();

	for (const UStaticMeshComponent* Piece : SmearMeshes)
	{
		if (Piece == nullptr || !Piece->IsVisible() || Piece->GetStaticMesh() == nullptr)
		{
			continue;
		}

		const FTransform& PieceTransform = Piece->GetComponentTransform();
		const FBox LocalBox = Piece->GetStaticMesh()->GetBoundingBox();

		// Clamping in the piece's own local space and transforming back is the exact closest point on
		// an oriented box; a world-AABB test would only approximate it, and would approximate in the
		// flattering direction on the yawed elements a curving ribbon is entirely made of.
		const FVector Local = PieceTransform.InverseTransformPosition(At);
		const FVector Clamped(
			FMath::Clamp(Local.X, LocalBox.Min.X, LocalBox.Max.X),
			FMath::Clamp(Local.Y, LocalBox.Min.Y, LocalBox.Max.Y),
			FMath::Clamp(Local.Z, LocalBox.Min.Z, LocalBox.Max.Z));

		Nearest = FMath::Min(Nearest, FVector::Dist(At, PieceTransform.TransformPosition(Clamped)));
		if (Nearest <= 0.01)
		{
			return 0.0;
		}
	}

	return (Nearest == TNumericLimits<double>::Max()) ? -1.0 : Nearest;
}

#if !UE_BUILD_SHIPPING
namespace
{
	/**
	 * Signed distance, in uu, from a world point to one rendered level box: NEGATIVE inside (and then
	 * its magnitude is the distance to the nearest face, i.e. how deep the trace is buried), POSITIVE
	 * outside (distance to the surface).
	 *
	 * The arena is built out of boxes — engine cubes, scaled and yawed — so a piece's local bounding
	 * box IS its shape, and an oriented-box test is exact rather than an approximation. Working in the
	 * box's local space and scaling the per-axis result back out keeps that exactness under
	 * non-uniform scale, which every wall, rib and lip in this arena has.
	 */
	double SignedDistanceToClipBox(const UTraceTrailComponent::FTraceClipBox& Box, const FVector& Point,
		int32& OutShallowestAxis)
	{
		OutShallowestAxis = 0;

		const FVector Local = Box.Transform.InverseTransformPosition(Point) - Box.LocalCentre;

		double Deepest = TNumericLimits<double>::Max();
		bool bInside = true;
		FVector OutsideDelta = FVector::ZeroVector;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double AxisScale = FMath::Abs(Box.Scale[Axis]);
			const double Over = FMath::Abs(Local[Axis]) - Box.LocalExtent[Axis];
			if (Over > 0.0)
			{
				bInside = false;
				OutsideDelta[Axis] = Over * AxisScale;
			}
			else if (bInside)
			{
				const double Depth = -Over * AxisScale;
				if (Depth < Deepest)
				{
					Deepest = Depth;
					OutShallowestAxis = Axis;
				}
			}
		}

		return bInside ? -Deepest : OutsideDelta.Size();
	}

	/**
	 * Worst (most negative) signed distance from Point to any candidate box, plus which box it was
	 * and which way the point would have to move to leave it.
	 */
	double MeasureVisualClearance(const TArray<UTraceTrailComponent::FTraceClipBox>& Geometry,
		const TArray<int32>& Candidates, const FVector& Point, FString& OutPiece, FVector& OutPushOut)
	{
		double Worst = TNumericLimits<double>::Max();

		// Anything further away than this is not interesting even as a near miss, and skipping it is
		// what keeps the lattice affordable. It must NOT be tightened to "only boxes the point is
		// already inside": that was the first version, and it left the closest-approach figure — the
		// one that proves the fixture pressed against something — permanently unset, so a run that
		// tested nothing was indistinguishable from a run that tested everything and passed.
		constexpr double InterestRadius = 300.0;

		for (const int32 Index : Candidates)
		{
			const UTraceTrailComponent::FTraceClipBox& Box = Geometry[Index];
			if (Box.WorldBounds.ComputeSquaredDistanceToPoint(Point) > InterestRadius * InterestRadius)
			{
				continue;
			}

			int32 ShallowestAxis = 0;
			const double Signed = SignedDistanceToClipBox(Box, Point, ShallowestAxis);
			if (Signed < Worst)
			{
				Worst = Signed;

				// Named whether it is a hit or a near miss. A clearance figure without the name of
				// the surface it was measured against cannot be checked, and this pass has already
				// had one number that turned out to be about the wrong geometry entirely.
				OutPiece = Box.Name;

				if (Signed < 0.0)
				{

					// Shortest way out of the box, in world space: along the axis it is least deep on.
					const FVector Local = Box.Transform.InverseTransformPosition(Point) - Box.LocalCentre;
					FVector LocalOut = FVector::ZeroVector;
					LocalOut[ShallowestAxis] = (Local[ShallowestAxis] >= 0.0) ? 1.0 : -1.0;
					OutPushOut = Box.Transform.TransformVectorNoScale(LocalOut).GetSafeNormal();
				}
			}
		}

		return Worst;
	}

	/**
	 * v13 §7, INVARIANT DIRECTION ONE. How far outside the LETHAL volume the world point At lies, in
	 * uu — 0 if it is inside the thing that kills.
	 *
	 * v14 §1 GUTTED THIS AND FORWARDED IT. It used to be a second, independent implementation of the
	 * trip test's geometry sitting a few hundred lines away from the trip test — and independent
	 * implementations of the same solid is the exact mechanism by which "lethal" and "drawn" drifted
	 * 22uu apart without a single line of either half being wrong on its own. There is now one
	 * definition (SegmentGapToTraceVolume) and the measurement asks IT, so the harness cannot be
	 * measuring a volume the server does not kill in.
	 *
	 * The two axes stay apart in the report: an element's height is the UNION of its two ends' bands,
	 * so vertical overhang is deliberate over-drawing while horizontal overhang is the fitter's
	 * business. Rolling them into one number once made a documented design choice read as a bug.
	 *
	 * bOutPastEnd now means "past a FLAT outer cap" — with GTrailFlatEndCaps on, that is drawn ribbon
	 * beyond where anything kills and it is CHARGED, not excused. It is reported separately only so
	 * the diagnosis can name it.
	 */
	void DistanceOutsideLethalColumn(const TArray<FVector>& Polyline, const FVector& At,
		double Radius, double HalfHeight, double& OutHorizontal, double& OutVertical, bool& bOutPastEnd)
	{
		MeasurePointGapToTrace(Polyline, At, Radius, HalfHeight, OutHorizontal, OutVertical, bOutPastEnd);
	}


	/**
	 * v13 §7, INVARIANT DIRECTION TWO — THE ONE THAT MATTERS MOST. How far the world point At is from
	 * the nearest VISIBLE ribbon piece's oriented box, in uu. 0 means it is inside something on
	 * screen.
	 *
	 * A lethal sample with a non-zero answer here is an INVISIBLE KILL VOLUME. That is strictly worse
	 * than the bug being fixed — a player cannot avoid what they cannot see — and it is the specific
	 * way a wall-clip fix goes wrong, by moving the drawing out of the wall and leaving the trip test
	 * behind in it. It must read zero on every frame of every arm.
	 */
	double DistanceOutsideDrawnRibbon(const TArray<const UStaticMeshComponent*>& Pieces, const FVector& At)
	{
		double Nearest = TNumericLimits<double>::Max();

		for (const UStaticMeshComponent* Piece : Pieces)
		{
			// Cheap reject first: this runs over every lattice sample of the lethal column against
			// every visible element, and the exact oriented-box solve below is thirty times the cost
			// of the bounds test that can rule it out.
			if (Piece->Bounds.GetBox().ComputeSquaredDistanceToPoint(At) > Nearest * Nearest)
			{
				continue;
			}

			const FTransform& PieceTransform = Piece->GetComponentTransform();
			const FBox LocalBox = Piece->GetStaticMesh()->GetBoundingBox();

			const FVector Local = PieceTransform.InverseTransformPosition(At);
			const FVector Clamped(
				FMath::Clamp(Local.X, LocalBox.Min.X, LocalBox.Max.X),
				FMath::Clamp(Local.Y, LocalBox.Min.Y, LocalBox.Max.Y),
				FMath::Clamp(Local.Z, LocalBox.Min.Z, LocalBox.Max.Z));

			Nearest = FMath::Min(Nearest, FVector::Dist(At, PieceTransform.TransformPosition(Clamped)));
			if (Nearest <= 0.01)
			{
				return 0.0;
			}
		}

		return (Nearest == TNumericLimits<double>::Max()) ? 0.0 : Nearest;
	}
}

void UTraceTrailComponent::MeasureWorldClipping(const TArray<FTraceClipBox>& Geometry,
	FTraceClipSample& Out) const
{
	Out = FTraceClipSample();

	if (GetWorld() == nullptr || Geometry.Num() == 0)
	{
		return;
	}

	const double TrailRadius = FMath::Max(0.0, static_cast<double>(GetTraceTrailRadius()));
	const double TrailHalfHeight = FMath::Max(0.0, static_cast<double>(GetTraceTrailHeight())) * 0.5;

	const int32 LethalCount = ComputeLastLethalIndex() + 1;
	Out.LethalPoints = LethalCount;

	// -------------------------------------------------------------------------------------------
	// BROAD PHASE. The trace is ~1200uu long and the arena is thousands of rendered boxes, so the
	// candidate list is built once per frame from the trace's own world bounds. Without it this
	// lattice would be millions of oriented-box tests a frame and the harness would change the very
	// frame rate the carrier is being driven at.
	// -------------------------------------------------------------------------------------------
	FBox TraceBounds(ForceInit);
	for (const UStaticMeshComponent* Piece : SmearMeshes)
	{
		if (Piece != nullptr && Piece->IsVisible() && Piece->GetStaticMesh() != nullptr)
		{
			TraceBounds += Piece->Bounds.GetBox();
		}
	}
	for (int32 PointIndex = 0; PointIndex < LethalCount; ++PointIndex)
	{
		TraceBounds += FVector(TrailRadius + 1.0, TrailRadius + 1.0, TrailHalfHeight + 1.0)
			+ FVector(TrailPoints.Items[PointIndex].Location);
		TraceBounds += FVector(TrailPoints.Items[PointIndex].Location)
			- FVector(TrailRadius + 1.0, TrailRadius + 1.0, TrailHalfHeight + 1.0);
	}
	// THE CREDENTIAL IS ONLY TAKEN ON FRAMES THAT HAVE A TRACE, and this is the last of the ways this
	// fixture could report two true numbers that together meant nothing.
	//
	// The drive places the carrier at each new target, which restarts the trace. The frames where the
	// BODY is closest to a structure are therefore exactly the frames where there is no ribbon yet —
	// so a run could honestly report "the body reached 8.4uu of a rendered surface" AND "0 frames of
	// clipping" while those two sentences described different moments, and the verdict would read as
	// proof of a fix. It happened: an arm-0 run reported precisely that pair, with the ribbon clearance
	// never measured at all.
	//
	// Requiring a real trace makes the credential a statement about the frames the clip result is
	// actually computed from.
	if (LethalCount >= 2 && GetOwner() != nullptr)
	{
		const AActor* OwnerActor = GetOwner();

		// ITS OWN BROAD PHASE, not the trace's, and that is not a micro-optimisation in reverse.
		//
		// Candidates above is built from the TRACE's bounds, and the whole function returns early when
		// there is no trace yet. The frames where the carrier's body is CLOSEST to a structure are
		// exactly the frames just after it arrives somewhere new — when the trace has been restarted
		// and is one point long — so the credential was blind to precisely the evidence it exists to
		// provide, and read 60.2uu on a run whose own drive log said the body arrived 42uu off a
		// surface. The body's clearance is a fact about the body; it gets asked about the body.
		TArray<int32> CarrierCandidates;
		{
			const FVector Reach(200.0, 200.0, TrailHalfHeight + 200.0);
			const FBox CarrierBounds(OwnerActor->GetActorLocation() - Reach,
				OwnerActor->GetActorLocation() + Reach);
			for (int32 Index = 0; Index < Geometry.Num(); ++Index)
			{
				if (Geometry[Index].WorldBounds.Intersect(CarrierBounds))
				{
					CarrierCandidates.Add(Index);
				}
			}
		}

		// MEASURED AS THE TRACE'S OWN COLUMN, NOT AS A POINT, and the difference decided a verdict.
		//
		// A single sample at the actor location is a point-to-box distance, so a wall band at head
		// height reads as far away because of the vertical gap — and the credential then compared that
		// inflated number against the ribbon's HORIZONTAL reach and declared a run invalid that had, in
		// the very same report, 5.8uu of ribbon inside that band. Sampling the same vertical band the
		// trace occupies makes the two numbers the same kind of number.
		const FVector At = OwnerActor->GetActorLocation();
		for (int32 Station = -1; Station <= 1; ++Station)
		{
			FString Piece;
			FVector IgnoredPush = FVector::ZeroVector;
			const double Signed = MeasureVisualClearance(Geometry, CarrierCandidates,
				At + FVector(0.0, 0.0, static_cast<double>(Station) * TrailHalfHeight * 0.95),
				Piece, IgnoredPush);

			if (Signed < Out.CarrierNearestSurface)
			{
				Out.CarrierNearestSurface = Signed;
				Out.CarrierNearestPiece = Piece;
			}
		}
	}

	if (!TraceBounds.IsValid)
	{
		return;
	}

	TArray<int32> Candidates;
	for (int32 Index = 0; Index < Geometry.Num(); ++Index)
	{
		if (Geometry[Index].WorldBounds.Intersect(TraceBounds))
		{
			Candidates.Add(Index);
		}
	}

	// HOW CLOSE THE BODY ITSELF GOT. The fixture's own credential: if the carrier never pressed
	// against a structure then nothing downstream of it means anything, and this is measured off the
	// same boxes and the same helper as everything else so the two numbers are on one scale.

	// -------------------------------------------------------------------------------------------
	// THE DRAWN VOLUME: the oriented box of every visible ribbon piece, exactly as placed. Joint
	// overlaps, minimum element lengths and the box's own corners are all included, because they are
	// all on screen and the report is about what is on screen.
	//
	// The predicted owner-only stub is excluded for the same reason MeasureDrawnVolume excludes it:
	// it is deliberately drawn ahead of anything lethal and only its own carrier can see it.
	// -------------------------------------------------------------------------------------------
	// The lethal polyline and the visible pieces, snapshotted once: both invariant directions below
	// need the whole of the opposite half to answer "is this bit of me matched by a bit of you".
	TArray<FVector> LethalPolyline;
	LethalPolyline.Reserve(LethalCount);
	for (int32 PointIndex = 0; PointIndex < LethalCount; ++PointIndex)
	{
		LethalPolyline.Add(FVector(TrailPoints.Items[PointIndex].Location));
	}

	TArray<const UStaticMeshComponent*> VisiblePieces;
	for (const UStaticMeshComponent* Piece : SmearMeshes)
	{
		if (Piece != nullptr && Piece->IsVisible() && Piece->GetStaticMesh() != nullptr)
		{
			VisiblePieces.Add(Piece);
		}
	}

	for (const UStaticMeshComponent* Piece : SmearMeshes)
	{
		if (Piece == nullptr || !Piece->IsVisible() || Piece->GetStaticMesh() == nullptr)
		{
			continue;
		}

		++Out.VisiblePieces;

		const FTransform& PieceTransform = Piece->GetComponentTransform();
		const FBox LocalBox = Piece->GetStaticMesh()->GetBoundingBox();
		const FVector LocalCentre = LocalBox.GetCenter();
		const FVector LocalExtent = LocalBox.GetExtent();
		const FVector Scale = Piece->GetComponentScale();

		// Stations every ~12uu along the element's length, three across its width and three up its
		// height, pulled a hair inside each face so a ribbon laid FLUSH against a wall — the correct
		// and desirable outcome of hugging one — is not counted as being inside it.
		const double LengthUU = 2.0 * FMath::Abs(LocalExtent.X * Scale.X);
		const int32 LengthSteps = FMath::Clamp(FMath::CeilToInt(LengthUU / 12.0), 1, 24);

		for (int32 Step = 0; Step <= LengthSteps; ++Step)
		{
			const double U = -1.0 + 2.0 * (static_cast<double>(Step) / static_cast<double>(LengthSteps));
			for (int32 WidthIndex = -1; WidthIndex <= 1; ++WidthIndex)
			{
				for (int32 HeightIndex = -1; HeightIndex <= 1; ++HeightIndex)
				{
					const FVector Local = LocalCentre + FVector(
						U * LocalExtent.X * 0.98,
						static_cast<double>(WidthIndex) * LocalExtent.Y * 0.95,
						static_cast<double>(HeightIndex) * LocalExtent.Z * 0.95);

					const FVector At = PieceTransform.TransformPosition(Local);

					++Out.DrawnSamplesTotal;

					FString Which;
					FVector PushOut = FVector::ZeroVector;
					const double Signed = MeasureVisualClearance(Geometry, Candidates, At, Which, PushOut);

					Out.DrawnNearestSurface = FMath::Min(Out.DrawnNearestSurface, Signed);

					if (Signed < 0.0)
					{
						++Out.DrawnSamplesInside;
						if (-Signed > Out.DrawnDepth)
						{
							Out.DrawnDepth = -Signed;
							Out.DrawnWorst = At;
							Out.DrawnWorstPushOut = PushOut;
							Out.DrawnWorstPiece = Which;
						}
					}

					// INVARIANT DIRECTION ONE, on the same sample, against the same polyline the
					// server kills along.
					double OutsideH = 0.0;
					double OutsideV = 0.0;
					bool bPastEnd = false;
					DistanceOutsideLethalColumn(LethalPolyline, At, TrailRadius, TrailHalfHeight,
						OutsideH, OutsideV, bPastEnd);

					// v14 §1: PAST-THE-END IS NO LONGER AN EXCUSE, IT IS A SUBTOTAL.
					//
					// It was excused because the caps were radial and the ribbon was flush, so anything
					// the drawing put beyond an end was covered by the kill volume's own bulge and could
					// only ever be the one-frame rebuild lag. Now that the caps are FLAT, ribbon past an
					// end is ribbon nothing kills on, which is the same defect as ribbon beside the path
					// and is charged the same way. It is still counted separately so the report can say
					// which kind it was, but it is no longer subtracted from the verdict.
					if (OutsideH > Out.DrawnOutsideLethal)
					{
						Out.DrawnOutsideLethal = OutsideH;
						Out.DrawnOutsideLethalAt = At;
					}
					if (bPastEnd)
					{
						Out.DrawnPastEnd = FMath::Max(Out.DrawnPastEnd, OutsideH);
					}
					Out.DrawnOutsideLethalVertical = FMath::Max(Out.DrawnOutsideLethalVertical, OutsideV);
				}
			}
		}
	}

	// -------------------------------------------------------------------------------------------
	// THE LETHAL VOLUME — AND v14 §1 CHANGED HOW IT IS SAMPLED, WHICH IS WHY THIS FIXTURE COULD NOT
	// SEE ITS OWN WORST BUG.
	//
	// It used to walk a rectangular lattice along each segment: lateral offsets perpendicular to the
	// chord, vertical offsets about it, at stations between the two endpoints. Every one of those
	// samples is genuinely lethal, so nothing it reported was false — but the lattice STOPPED AT THE
	// ENDPOINTS, and the trip test does not. A point-to-segment distance is radial at the ends, so
	// the volume that kills bulged a half-disc of TrailRadius past the first and last point, and the
	// measurement of "is every lethal place drawn" never once asked about that disc. It could only
	// ever have found the end-cap bug by accident, through a lateral sample on a steeply pitched
	// element. A test that cannot see the defect it exists to find is the same failure as a fixture
	// that cannot reach a wall.
	//
	// So the lattice is now a RING at each station — eight compass bearings at the volume's own half
	// width plus the centreline — and every candidate is put through the REAL predicate before it is
	// charged. The backward bearings at the first point and the forward ones at the last ARE the end
	// caps: with the radial cap they pass the filter and the ribbon is not there (red); with the flat
	// cap they fail it and are correctly not counted (green). Same lattice, same code, both arms.
	// -------------------------------------------------------------------------------------------
	static const FVector CompassBearings[] =
	{
		FVector(0.0, 0.0, 0.0),
		FVector(1.0, 0.0, 0.0),  FVector(-1.0, 0.0, 0.0),
		FVector(0.0, 1.0, 0.0),  FVector(0.0, -1.0, 0.0),
		FVector(0.7071, 0.7071, 0.0),  FVector(-0.7071, 0.7071, 0.0),
		FVector(0.7071, -0.7071, 0.0), FVector(-0.7071, -0.7071, 0.0)
	};
	static const double HeightFractions[] = { -0.999, -0.5, 0.0, 0.5, 0.999 };

	for (int32 PointIndex = 0; PointIndex + 1 < LethalCount; ++PointIndex)
	{
		const FVector SegStart(TrailPoints.Items[PointIndex].Location);
		const FVector SegEnd(TrailPoints.Items[PointIndex + 1].Location);

		const double Length = FVector::Dist(SegStart, SegEnd);
		const int32 Steps = FMath::Clamp(FMath::CeilToInt(Length / 12.0), 1, 24);

		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const FVector Centre = FMath::Lerp(SegStart, SegEnd,
				static_cast<double>(Step) / static_cast<double>(Steps));

			for (const FVector& Bearing : CompassBearings)
			{
				for (const double HeightFraction : HeightFractions)
				{
					const FVector At = Centre
						+ Bearing * (TrailRadius * 0.999)
						+ FVector::UpVector * (HeightFraction * TrailHalfHeight);

					// THE FILTER, AND IT IS THE REAL PREDICATE. A sample is only evidence about the
					// kill volume if the kill volume contains it, and "contains" is answered by the
					// same function ServerRunTripTest answers it with — not by this lattice's own idea
					// of the shape. Everything the ring throws past a flat cap is discarded here.
					if (!IsPointLethal(LethalPolyline, At, TrailRadius, TrailHalfHeight))
					{
						continue;
					}

					++Out.LethalSamplesTotal;

					FString Which;
					FVector PushOut = FVector::ZeroVector;
					const double Signed = MeasureVisualClearance(Geometry, Candidates, At, Which, PushOut);

					if (Signed < 0.0)
					{
						++Out.LethalSamplesInside;
						if (-Signed > Out.LethalDepth)
						{
							Out.LethalDepth = -Signed;
							Out.LethalWorst = At;
							Out.LethalWorstPiece = Which;
						}
					}

					// INVARIANT DIRECTION TWO — the invisible kill volume. Every sample of the
					// column that kills has to be inside something on screen.
					if (VisiblePieces.Num() > 0)
					{
						const double OutsideDrawn = DistanceOutsideDrawnRibbon(VisiblePieces, At);
						if (OutsideDrawn > Out.LethalOutsideDrawn)
						{
							Out.LethalOutsideDrawn = OutsideDrawn;
							Out.LethalOutsideDrawnAt = At;

							// Name the cause rather than leave a number to be explained. See
							// FTraceClipSample::bLethalOutsideDrawnIsEndCap.
							const double ToHead = FVector::Dist(At, LethalPolyline.Last());
							const double ToTail = FVector::Dist(At, LethalPolyline[0]);
							Out.bLethalOutsideDrawnIsEndCap =
								(FMath::Min(ToHead, ToTail) <= (TrailRadius + TrailHalfHeight));
						}
					}
				}
			}
		}
	}
}

void UTraceTrailComponent::GetWallFitPushStats(int32& OutPushes, int32& OutUnpushable,
	double& OutWorstPush, double& OutWorstResidual)
{
	OutPushes = GWallFitPushes;
	OutUnpushable = GWallFitUnpushable;
	OutWorstPush = GWallFitWorstPush;
	OutWorstResidual = GWallFitWorstResidual;
}

void UTraceTrailComponent::GetWallFitStats(int32& OutRoutedAppends, int32& OutInsertedPoints,
	int32& OutUnroutable)
{
	OutRoutedAppends = GWallFitRoutedAppends;
	OutInsertedPoints = GWallFitInsertedPoints;
	OutUnroutable = GWallFitUnroutable;
}

void UTraceTrailComponent::ResetWallFitStats()
{
	GWallFitRoutedAppends = 0;
	GWallFitInsertedPoints = 0;
	GWallFitUnroutable = 0;

	GWallFitPushes = 0;
	GWallFitUnpushable = 0;
	GWallFitWorstPush = 0.0;
	GWallFitWorstResidual = 0.0;
}
#endif // !UE_BUILD_SHIPPING

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
