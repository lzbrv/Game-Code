// Trace — the private tables and file-local knobs ATraceCore's implementation shares with its
// sibling translation unit. NOT a public interface: nothing outside the TraceCore .cpp family may
// include this. The Core's public surface is Gameplay/TraceCore.h and has not changed.
//
// WHY IT EXISTS. TraceCore.cpp reached sixteen thousand lines, of which roughly six thousand were
// the `#if !UE_BUILD_SHIPPING` harnesses that measure the pass system, mode B's throw/catch/turnover
// rules and the v31/v32 art. RESTRUCTURE tranche D2 moved those harnesses to TraceCoreHarness.cpp.
// They were only ever able to measure the game because they read THE SAME numbers and call THE SAME
// functions the game does — Trace.ModeB.CatchTest drives TraceModeBTuning::SteerTowardCatchPoint
// itself, not a copy of it, and the file says so in as many words. That property is the whole value
// of the harnesses, so the shared half of the file moved here rather than being duplicated.
//
// WHAT IS AND IS NOT HERE:
//   * CONSTANTS and PURE TYPES are DEFINED here (`constexpr`, `const`, `inline`, a plain struct).
//     Each including TU gets its own read-only copy and there is nothing to link.
//   * CONSOLE VARIABLES and the settings-bridge FUNCTIONS are only DECLARED here. Each is defined
//     exactly ONCE, somewhere in the TraceCore .cpp family, beside the essay that argues its
//     default — the mode-B and pass knobs in TraceCore.cpp, the three art knobs the harness reads
//     (rest spin, flight spin, thrown-trail length) in TraceCoreArt.cpp with the rest of the art
//     block. A console variable constructed twice would appear twice in the console, and the
//     settings bridge reads forty more console variables that stay private to TraceCore.cpp.
//
// Every comment below moved with the thing it documents and was not edited.

#pragma once

#include "CoreMinimal.h"

#include "CollisionQueryParams.h"   // FCollisionQueryParams (SweepLooseCore's signature)
#include "Engine/HitResult.h"       // FHitResult (ditto)
#include "HAL/IConsoleManager.h"    // TAutoConsoleVariable (the extern block at the bottom)
#include "TraceTypes.h"             // ETraceTeam (the kickoff default, AreEnemies/AreAllies)

class UWorld;

namespace TraceCoreTuning
{
	// ---------------------------------------------------------------------------------------------
	// PROMOTED. Pass hold, cooldown, range, aim cone, aim slack, chest offset and the turnover trace
	// grace all used to live here as compile-time constants, with a comment asking for exactly this:
	// "Every one of them is a designer knob and they should be promoted to UTraceSettings". They now
	// are - UTraceSettings, Category "Core|Pass" - and this file reads them at the point of use, so
	// they retune with PIE running. Nothing is left behind here except the two PRE-FIX values the
	// Trace.Pass.Fixes A/B switch replays, and the constants that are genuinely not designer-facing.
	// ---------------------------------------------------------------------------------------------

	/**
	 * PRE-FIX aim cone and slack. Read ONLY when Trace.Pass.Fixes is 0, which replays the exact
	 * behaviour the user reported as broken so the before/after numbers come from one binary.
	 * The live values are UTraceSettings::PassAimConeDegrees / PassAimSlack (9 deg / 70 uu).
	 */
	constexpr double PassAimConeDegrees = 6.0;
	constexpr double PassAimSlack = 40.0;

	/**
	 * Line-of-sight probe heights, as offsets from the receiver's capsule CENTRE. Chest, head,
	 * knees.
	 *
	 * A single chest-height ray against 176 uu / 352 uu / 616 uu cover boxes is a coin flip: a
	 * receiver whose head and shoulders are plainly visible over a 1x block fails the test because
	 * the one point being probed is behind it. ANY clear probe means line of sight, which is both
	 * more generous and much closer to what the player can actually see.
	 *
	 * Entry 0 is a placeholder: the chest probe is overridden at the call site with
	 * UTraceSettings::PassTargetChestOffsetZ, so the probed chest and the AIMED-AT chest are
	 * guaranteed to be the same point. Toggled by UTraceSettings::bPassMultiPointLos.
	 */
	constexpr double LosProbeOffsetsZ[] = { 20.0, 70.0, -45.0 };

	/**
	 * How long a kickoff waits before it is granted.
	 *
	 * Every caller of KickoffTo() teleports ten pawns immediately afterwards
	 * (ATraceGameMode::ResetPlayersToSpawns, which also clears every trail). Granting inside that
	 * window would lay a trail across the teleport and then have it wiped from under us.
	 */
	constexpr float KickoffDelaySeconds = 1.0f;

	/**
	 * Backstop: a Core that has been holderless this long with nobody owed it grants itself to the
	 * default team. The Core must never be idle - there is no way to pick it up any more, so a
	 * holderless Core is a dead match, not merely a quiet one.
	 */
	constexpr float MaxHolderlessSeconds = 5.0f;

	/**
	 * Last-ditch recovery when the Core has been parked OUT OF PLAY this long while a half is
	 * actually running. Long enough that no legitimate interval trips it, short enough that a
	 * mistake in the match-flow code costs one warning line rather than a dead match.
	 */
	constexpr float OutOfPlayRecoverySeconds = 15.0f;

	/** §1: the Core starts with Team A. Blue is Team A. */
	constexpr ETraceTeam DefaultKickoffTeam = ETraceTeam::Blue;

	/**
	 * DEMO 29 §3. How long a CONTESTED kickoff Core may lie unretrieved before it is granted anyway.
	 *
	 * NOT the loose-Core reset timer, and it deliberately replaces it for this one case. That timer
	 * (UTraceSettings::CoreLooseResetSeconds, 12 s) exists to rescue a THROW nobody collected, and
	 * measures from the moment the Core went loose — which for a kickoff is the moment the half
	 * started, before anybody has taken a step. Twelve seconds is not long enough to cross the field
	 * and climb the octagon, so leaving that timer armed would hand the Core out from under the
	 * mechanic the owner asked for, every single half.
	 *
	 * 60 s IS A MEASURED NUMBER, NOT A ROUND ONE, and the first draft of it (30 s) was wrong. The
	 * shipped arena is 38,400 uu goal to goal and the spawn pads sit behind the goal plane, so a
	 * player starts roughly 17,000-19,000 uu from the centre pillar. At UTraceSettings::WalkSpeed
	 * (800 uu/s) that crossing alone is 21-24 s BEFORE the climb, before being shot at, and before
	 * anybody dies on the way. A headless 8-bot run of the first draft tripped this backstop on every
	 * half — the bots entered ChaseLooseCore within a tenth of a second of the whistle and were still
	 * short of the deck half a minute later — which is a timer measuring the pitch, not the rule.
	 * Doubling the crossing gives the sprint, the climb and one bad fight room, and it is still two
	 * orders of magnitude inside the 480 s half, so a genuinely unreachable deck costs one Warning and
	 * a playable match rather than a dead one.
	 *
	 * WHEN IT FIRES IT IS A BUG REPORT, NOT A FEATURE — see the log line, which says so.
	 */
	constexpr float ContestedKickoffBackstopSeconds = 60.0f;

	// --- Cosmetics --------------------------------------------------------------------------------

	/**
	 * WHERE THE CORE ACTOR SITS ON ITS HOLDER, and it is NOT where the ball is drawn any more.
	 *
	 * Still 150 uu straight up the capsule axis, because three separate things are written against
	 * that number: the beacon's centre ((BeaconTop + BeaconBottom)/2 - OrbHeight), Trace.Core.ArtShots'
	 * camera aim, and TickTeleportAudit's per-frame jump detector, which would cry wolf on every
	 * possession change if the actor itself moved into a hand. The BALL moved instead - see ArtRoot
	 * and CarryCradle* below - so this stayed exactly as it was and no gameplay read had to be
	 * re-checked one at a time.
	 */
	constexpr double OrbHeight = 150.0;

	// --- The carried ball's cradle ------------------------------------------------------------------
	//
	// ALL IN UNREAL UNITS (cm), and all in the CARRIER'S ACTOR FRAME: +X forward, +Y right, +Z up.
	// That frame is safe to author in because ApplyAttachment snaps the Core to its holder and zeroes
	// the relative rotation, and the character never uses controller pitch or roll - so the Core
	// actor's axes ARE the carrier's, world up is preserved, and none of this can be tilted by a
	// look. Taking the offset out of `hand_r`'s own bone axes instead would have put that at risk:
	// this project's knife rig had to MEASURE its hand-space cant because the axes did not read the
	// way anyone expected.

	/**
	 * MEASURED off Saved/Screenshots/TraceAutoShot_Match_20260821_151039_05.png, which is the frame
	 * that recorded the defect. The Manny is 311 px tall for 180 uu there (1.728 px/uu) and the right
	 * fist's centroid sits 25 uu outboard of the body centreline at capsule-centre height.
	 *
	 * Used ONLY when the right hand does not resolve on the body the pawn is actually drawing - a
	 * machine with no character art imported draws a 34 uu radius cylinder instead - and this keeps
	 * the ball at that pawn's right hip rather than at the component origin, i.e. lying at its feet.
	 * NOT the Rocco case: his rig spells the joint `RightHand1` and ResolveBodyBoneName finds it, so
	 * he holds the ball in his fist like everyone else. Logged once, naming the mesh that failed.
	 */
	constexpr double CarryHandRestRight = 25.0;

	/**
	 * `hand_r` IS THE WRIST, NOT THE FIST, and the previous pass hung the ball off the wrist.
	 *
	 * MEASURED this pass, off the live rig rather than off a picture: Trace.Core.CarryProbe on a
	 * standing carrier prints `hand_r` at actor-local (-5.0, 25.8, -1.0) uu - hip height, the arm
	 * straight down. The closed hand ABP_Unarmed rests in runs on from there down the forearm, and
	 * its centroid is about 7 uu past the joint. Anchoring the ball to the JOINT put the whole hand
	 * between the anchor and the ball, which is how a 10 uu "lift" became a visible air gap under an
	 * empty fist.
	 *
	 * Taken along the LIVE `lowerarm_r` -> `hand_r` direction where that bone resolves, so the anchor
	 * swings with the run cycle instead of assuming the arm hangs; straight down is the fallback.
	 */
	constexpr double CarryFistReach = 7.0;

	/**
	 * The ball's centre RELATIVE TO THE FIST, and it is BOUNDED, not merely authored.
	 *
	 * The ball rests ON the closed hand: 6 uu outboard so its inboard shell clears the thigh, 8 uu up
	 * so the fist is buried in the lower shell rather than dangling beneath it. |(0, 6, 8)| = 10.0 uu
	 * against the ball's NARROW half-extent of 11.9 uu (see GetDrawnBallHalfExtentUU), so the fist's
	 * centroid sits 1.9 uu inside the shell at the ball's worst spin angle and deeper at every other.
	 *
	 * *** THE BOUND IS THE FIX, NOT THE THREE NUMBERS. *** What shipped before was three hand-authored
	 * numbers whose comment asserted "the fist 7 uu INSIDE the ball's surface" against a radius of
	 * 20 uu - the radius of the ENGINE SPHERE the pack ball replaced. The drawn ball is a 40.0 x 23.8
	 * x 23.8 uu football, so the true cross-radius is 11.9 and the same offset left the hand 10 uu
	 * clear of the shell with sky behind it. UpdateCarriedArtPlacement now CLAMPS this vector to the
	 * ball's own drawn half-extent, so no value of the three console knobs - and no re-export at a new
	 * size - can put the ball back out of the hand. That is the invariant Trace.Core.CarryProbe
	 * prints, and Trace.Core.CarryOffsetUp 40 is the red arm that makes the clamp fire.
	 *
	 * NOT a chest cradle, which is what the pack's README describes and what a first-person hold would
	 * want: the carry camera is directly BEHIND the holder, so a ball held at the sternum is occluded
	 * by the holder's own back and the fix would be invisible in the only view it exists for. The pawn
	 * also has no carry POSE - the third-person mesh runs Epic's ABP_Unarmed, whose arms hang at the
	 * sides, and Epic ships no carry clip with it - so what is available here is where the ball goes,
	 * not where the arm goes. Posing the arm needs an animation asset and is ATraceCharacter's
	 * business, not this actor's.
	 */
	constexpr double CarryCradleForward = 0.0;
	constexpr double CarryCradleRight = 6.0;
	constexpr double CarryCradleUp = 8.0;

	/**
	 * How far inside the ball's shell the fist's centroid must end up, uu, once the clamp has run.
	 *
	 * Not zero: tangency is not a grip. At exactly the half-extent the fist's centre would be ON the
	 * silhouette edge and half the hand would still be outside it, which photographs as a ball resting
	 * against a hand rather than in one. 2 uu of bite puts the near half of the hand under the shell.
	 */
	constexpr double CarryGripBite = 2.0;

	/**
	 * MEASURED. The first pass ran this at 0.55 (a 55uu orb) with a glow of 2.4, and captured frames
	 * of the holder's own third-person view show the result: a ~150px pure-white disc parked in the
	 * middle of the frame. Two separate faults in one object — it clipped every channel, so the team
	 * colour it exists to communicate was gone, and it was a large unlit emissive surface a few
	 * hundred uu from a camera, which is precisely the point-blank whiteout failure mode this build
	 * already has an open defect for. Smaller and dimmer, and hidden from its own holder (see
	 * ApplyAttachment).
	 */
	constexpr float OrbScale = 0.40f;

	/** The shaft that makes a holder findable across a 24000uu field. */
	constexpr double BeaconBottom = 205.0;
	constexpr double BeaconTop = 1150.0;
	constexpr double BeaconWidth = 26.0;

	/**
	 * Glow multipliers on M_TraceNeon.
	 *
	 * Deliberately restrained. An unlit emissive is distance-invariant, so anything attached to a
	 * pawn is a point-blank whiteout risk for whoever is fighting them; the orb sits 150uu above the
	 * capsule centre (i.e. above eye height) and the shaft starts higher still, and neither is
	 * pushed as hard as the arena trim. The PASS glow is the exception and is meant to be read as
	 * "that player is vulnerable right now".
	 */
	constexpr float OrbGlow = 1.25f;
	constexpr float BeaconGlow = 1.15f;
	constexpr float PassGlowMultiplier = 1.9f;

	/** Home-position tolerance: the Core will not bother re-parking itself inside this. */
	constexpr double HomeToleranceSq = 75.0 * 75.0;
}

namespace TraceCoreLocal
{
	/** Divide-by-zero epsilon. Literal on purpose: the KINDA_SMALL_NUMBER family was re-spelled
	 *  during the 5.x line and this module must compile on 5.4 - 5.8. */
	constexpr double CoreGeometryEpsilon = 1.0e-8;

	/** True when both teams are known and different. Unknown teams are never enemies. */
	inline bool AreEnemies(ETraceTeam A, ETraceTeam B)
	{
		return A != ETraceTeam::None && B != ETraceTeam::None && A != B;
	}

	/** True when both teams are known and equal. */
	inline bool AreAllies(ETraceTeam A, ETraceTeam B)
	{
		return A != ETraceTeam::None && A == B;
	}
}

// -------------------------------------------------------------------------------------------------
// Two mode-B constants the harnesses grade against. The rest of the mode-B knob block stays in
// TraceCore.cpp with the console variables it resolves.
// -------------------------------------------------------------------------------------------------

/**
 * SPEC v19 §1.5. The RENDERED radius of the orb, in uu — what "visibly" means, numerically.
 *
 * The mesh is /Engine/BasicShapes/Sphere, which is 100 uu across, scaled by TraceCoreTuning::OrbScale
 * (0.40). So the ball a player sees is 20 uu in radius, against a 22 uu collision sphere: the
 * COLLISION IS ALREADY BIGGER THAN THE BALL, which is why the gap has to be measured with the drawn
 * size and not the swept one. Asking the collision sphere "are you touching?" is a question it
 * answers yes to 2 uu before the eye agrees.
 */
constexpr double TraceModeBVisibleOrbRadius = 100.0 * 0.5 * 0.40;

namespace TraceModeBTuning
{
	/**
	 * SPEC v13 §5. One contender for a contested magnet: how far it is, and who it is.
	 *
	 * A plain value type with no actor in it, so the SELECTION can be tested without a world. See
	 * PickContestedCatcher.
	 */
	struct FCatchContender
	{
		/** Distance from the Core to this player's capsule SURFACE, uu. The thing "closest" means. */
		double SurfaceDistance = 0.0;

		/**
		 * A stable, per-player key used ONLY to break an exact tie.
		 *
		 * Not cosmetic. Without it two players at identical distance are separated by whichever order
		 * the roster happened to be gathered in, which is a property of the world's actor list rather
		 * than of the game — so the same situation can resolve differently on two frames, or on two
		 * machines, for no reason a player could ever see. The engine's unique object id is stable for
		 * the lifetime of the actor and is the cheapest thing that has that property.
		 */
		uint32 StableKey = 0;

		/** True for the player the magnet is ALREADY pulling toward, if they are still eligible. */
		bool bIncumbent = false;
	};

	/** Radius of the sphere swept for the loose Core's collision. Matches the orb the player sees. */
	constexpr float CollisionRadius = 22.f;

	/** Height above the thrower's eye-line the Core leaves from, so it does not clip their own head. */
	constexpr double ThrowMuzzleForward = 70.0;

	/**
	 * DEMO 27. How long after a launch the throw is asked what became of it. See
	 * ATraceCore::ServerTickLaunchAudit.
	 *
	 * 0.10 s is chosen from both ends. It has to be LONG enough that a launch-frame collision has
	 * finished happening - the reported bug took four frames to spend the throw - and SHORT enough
	 * that an ordinary throw cannot have reached anything: at the full 3509 uu/s launch this file
	 * measured, a tenth of a second is 350 uu, about three player widths, and it is well inside the
	 * 0.50 s the thrower's own catch lockout runs for so the magnet cannot have touched the answer.
	 */
	constexpr float LaunchAuditSeconds = 0.10f;

	/**
	 * DEMO 27. Below this fraction of the launch speed the audit complains.
	 *
	 * Free flight loses only gravity over 0.10 s (about 3% of a full-power launch) and the catch
	 * magnet preserves speed exactly, so anything under 0.60 means the Core HIT something within a
	 * tenth of a second of leaving the hand. That is either the bug this pass fixed or a throw into a
	 * wall three metres away, and the audit line prints how far it got from the thrower so a reader
	 * can tell those two apart in one glance.
	 */
	constexpr float LaunchAuditMinRetained = 0.60f;

	// -----------------------------------------------------------------------------------------
	// DECLARATIONS ONLY, of the settings-first accessors and the two pure functions the harnesses
	// drive. Every one of them is DEFINED in TraceCore.cpp, where the argument for its clamp range
	// and the settings property it is bound to is written on the definition — this list is a seam,
	// not a second place to look up what a knob means.
	//
	// The two pure functions are the point of the exercise. Trace.ModeB.CatchTest steers with
	// SteerTowardCatchPoint and Trace.ModeB.ContestTest picks with PickContestedCatcher, so if that
	// maths is wrong the measurement is wrong in exactly the same way and the numbers stop agreeing
	// with the game. A harness that re-implemented them would be measuring the harness.
	// -----------------------------------------------------------------------------------------
	float PickupRadius();
	float LooseResetSeconds();
	float ThrowInheritance();
	float ThrowInheritanceDown();
	bool  ThrowChargeAnchorsAtPress();
	float CatchContestHysteresis();
	bool  TurnoverPullEnabled();
	float SurfaceUpNormalZ();
	float CatchRadius();
	float CatchCurveStrength();
	float SelfPickupLockout();
	float PullHoldSeconds();
	float PullAimConeDegrees();
	float PullAimSlackUU();
	float PullMaxRangeUU();
	float TurnoverLockoutSeconds();
	float TurnoverBeamScale();
	bool  LegacyTurnoverSoundEdge();

	FVector SteerTowardCatchPoint(const FVector& CoreLocation, const FVector& Velocity,
		const FVector& CatchPoint, double SurfaceDistance, float Radius, float Curve, float DeltaSeconds);

	int32 PickContestedCatcher(const TArray<FCatchContender>& Contenders, float Hysteresis,
		bool* bOutHysteresisHeld = nullptr);

	bool SweepLooseCore(const UWorld& World, FHitResult& OutHit, const FVector& Start, const FVector& End,
		float SphereRadius, const FCollisionQueryParams& Params);
}

namespace TraceCoreArt
{
	/** The pack's Core, and its three clips. Missing => the fallback sphere; see bPackArtActive. */
	const TCHAR* const MeshPath   = TEXT("/Game/Trace/Art/Pack/Core/SK_TraceCore.SK_TraceCore");
	const TCHAR* const IdlePath   = TEXT("/Game/Trace/Art/Pack/Core/Anims/A_Core_Idle.A_Core_Idle");
	const TCHAR* const PickupPath = TEXT("/Game/Trace/Art/Pack/Core/Anims/A_Core_Pickup.A_Core_Pickup");
	const TCHAR* const ThrowPath  = TEXT("/Game/Trace/Art/Pack/Core/Anims/A_Core_Throw.A_Core_Throw");

	/** Drawn length of the ball, uu. The orb it replaces, so no other rule in this file moves. */
	constexpr double TargetLengthUU = TraceModeBVisibleOrbRadius * 2.0;

	/** MEASURED from core.glb: A_Core_Throw turns the ball 4.000 times in 0.500 s. */
	constexpr float ThrowClipRevPerSecond = 4.f / 0.5f;

	/** MEASURED from core.glb: A_Core_Idle turns the ball once in 3.600 s. The ground turn rate. */
	constexpr float IdleClipRevPerSecond = 1.f / 3.6f;

	/** unreal-fx_README, "The Core": "The ball spins about its long axis - nose forward, ~10 rev/s." */
	constexpr float FlightSpinRevPerSecond = 10.f;

	/**
	 * Material slot names to drive. Matched by SUBSTRING because Interchange decorates slot names
	 * (`circuit_cyan_Section5` and friends) - Scripts/import_pack.py had to do the same thing when it
	 * assigned them, and a slot this file cannot find is a slot whose glow silently never moves.
	 */
	const TCHAR* const CyanSlot  = TEXT("circuit_cyan");
	const TCHAR* const AmberSlot = TEXT("core_amber");

	/**
	 * The KHR emissive strengths the pack folded into EmissiveColor, so that EmissiveIntensity really
	 * does mean "1.0 = at rest" (Scripts/import_pack.py's MATERIALS table). Only needed when the cyan
	 * slot is TEAM TINTED, where the authored colour is replaced and its brightness has to be carried
	 * across with it rather than silently lost.
	 */
	constexpr float CyanEmissiveStrength = 1.5f;

	/** The pack's own circuit_cyan emissive, linear, for a Core that belongs to nobody. #25E6FF. */
	const FLinearColor CyanEmissive(0.018500220f, 0.791297940f, 1.f, 1.f);

	/** unreal-fx_README "The Core": #FF8A1F, the heart light and the amber the objective reads by. */
	const FLinearColor AmberSRGB(1.f, 0.541f, 0.122f, 1.f);

	// --- EmissiveIntensity bands, verbatim from unreal-core_README.md's table and the FX notes ------
	constexpr float IdleCyanLo = 0.85f,  IdleCyanHi = 1.2f;
	constexpr float IdleAmberLo = 0.7f,  IdleAmberHi = 1.2f;
	constexpr float CarriedCyanLo = 1.7f,  CarriedCyanHi = 2.2f;
	constexpr float CarriedAmberLo = 2.4f, CarriedAmberHi = 3.2f;
	constexpr float ThrownCyan = 3.4f, ThrownAmber = 2.6f;
	constexpr float PickupAmberFlare = 4.6f;

	/** "both breathe on a slow ~2 s cycle" / "pulsing about 2.5 Hz". */
	constexpr float IdleBreathHz = 0.5f;
	constexpr float CarriedPulseHz = 2.5f;

	// --- SPEC v32 §3 GEOMETRY ----------------------------------------------------------------------
	//
	// *** THE FX DOC IS IN METRES. UNREAL IS IN CENTIMETRES. *** Every number below is the doc's own
	// figure x100, written out with the metric original beside it so the conversion is checkable by
	// eye. The uu -> BasicShape-scale half of the conversion is NEVER done here: it is
	// UTraceFxShapes::ShapeScaleForRadiusUU, the one named constant SPEC v32 §1 asks for, and a
	// hand-rolled /100 anywhere in this file would be the 100x class of bug that rule exists to stop.

	/** "a one-shot icosahedron halo (r 0.20)" — 0.20 m = 20 uu, i.e. the ball's own drawn radius. */
	constexpr float PickupHaloRadiusUU = 20.f;

	/** "expanding 0.6 -> 2.1x". Starts INSIDE the 20 uu ball and ends at twice its size. */
	constexpr float PickupHaloScaleStart = 0.6f;
	constexpr float PickupHaloScaleEnd = 2.1f;

	/** "a tapered trail cylinder (r 0.055 -> 0.012)" — 5.5 uu at the ball, 1.2 uu at the tail. */
	constexpr float ThrownTrailHeadRadiusUU = 5.5f;
	constexpr float ThrownTrailTailRadiusUU = 1.2f;

	/**
	 * How many stacked cylinders the taper is made of.
	 *
	 * THREE, matching the beam's, and the visible cost is the step between segments: 5.5 -> 1.2 uu
	 * over three segments is a step of 1.4 uu, i.e. under a centimetre and a half, on geometry that
	 * is moving at 1500-2200 uu/s several metres from the camera. See UTraceFxShapes::TaperAlongLocalZ
	 * for the full argument, including why this is not a cone.
	 */
	constexpr int32 ThrownTrailSegmentCount = 3;

	/**
	 * The trail's opacity floor and ceiling across the flight. "peaking mid-flight" — see
	 * ATraceCore::UpdateCoreArtGeometry for how the peak is found without a second clock.
	 *
	 * The FLOOR is not zero on purpose: the FX doc asks for a trail that "streams behind the ball",
	 * and a trail that vanishes on the way up and on the way down would be a trail the player only
	 * ever catches out of the corner of their eye. The doc's word is "peaking", not "only".
	 */
	constexpr float ThrownTrailMinOpacity = 0.35f;
	constexpr float ThrownTrailMaxOpacity = 0.90f;

	/** The same shape applied to LENGTH: shortest at the ends of the arc, full length at the apex. */
	constexpr float ThrownTrailMinLengthScale = 0.45f;
}


// =================================================================================================
// Console variables and file helpers owned by TraceCore.cpp, declared here for TraceCoreHarness.cpp
// =================================================================================================
//
// DECLARATIONS ONLY, and the guards match the ones around the definitions: a knob that does not
// exist in Shipping must not be declared in Shipping either. The harness reads them so that what it
// PRINTS and what the rule OBEYS are provably the same object — which is what makes the red arms
// (Trace.ModeB.LandingRule 0, Trace.ModeB.GroundedTurnover 0, ...) able to go red at all.
// The essay for every default is on the definition in TraceCore.cpp.

extern TAutoConsoleVariable<float> CVarCoreFlightSpin;
extern TAutoConsoleVariable<float> CVarCoreRestSpin;
extern TAutoConsoleVariable<float> CVarCoreThrownTrailLength;
extern TAutoConsoleVariable<int32> CVarModeBFlightHitsPawns;
extern TAutoConsoleVariable<float> CVarModeBMidAirTurnoverDegrees;
extern TAutoConsoleVariable<float> CVarModeBMidAirTurnoverSpeed;
extern TAutoConsoleVariable<float> CVarModeBTurnoverBeamScale;
extern TAutoConsoleVariable<float> CVarModeBTurnoverContactSlack;
extern TAutoConsoleVariable<float> CVarModeBTurnoverSettleSeconds;

#if !UE_BUILD_SHIPPING
extern TAutoConsoleVariable<int32> CVarCoreGoalReproRuns;
extern TAutoConsoleVariable<float> CVarCoreGoalReproDelay;
extern TAutoConsoleVariable<float> CVarCoreGoalReproInterval;
extern TAutoConsoleVariable<int32> CVarCoreTeleportAudit;
extern TAutoConsoleVariable<float> CVarCoreTeleportAuditJump;
extern TAutoConsoleVariable<float> CVarCoreTeleportAuditWindow;
extern TAutoConsoleVariable<int32> CVarModeBFlightLog;
#endif // !UE_BUILD_SHIPPING

/** The pre-v19 landing rule / the pre-v25 grounded rule, as the launch flags leave them. */
bool TraceModeBLegacyLandingRule();
bool TraceModeBLegacyGroundedRule();
