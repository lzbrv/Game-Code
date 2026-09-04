// Trace — the client-predicted movement kit: dash (charged), slide, air fast-fall, and the
// Source/Apex momentum model (air acceleration, landing carry, momentum-preserving transitions).
// See the header for the full prediction model and the design rationale.
//
// BOOST HAS BEEN DELETED (spec v3 §1). Not disabled, not defaulted to zero — removed. If you are
// reading this because a merge resurrected `bWantsToBoost`, `BoostCooldownRemaining`, `BeginBoost`
// or FLAG_Custom_1, delete it again.
//
// THE SLIDE'S FLAT MOMENTUM BOOST HAS BEEN DELETED TOO (spec v4 §1). Same rule: `SlideImpulse`,
// `GetSlideImpulse()`, `SlideExitMinSpeedFraction` and `GetSlideExitMinSpeedFraction()` are gone,
// along with the ExitFloor term in EndSlide(). The design owner ruled the boost out explicitly. If a
// merge brings any of them back, delete them again — and read the slide-jump section of the header,
// which is what the slide is supposed to be worth now.

#include "Movement/TraceCharacterMovementComponent.h"

#include "Abilities/TraceAbilityComponent.h"    // spec v14 §6: the speed passives and the dash hooks
#include "Abilities/TraceAbilityTypes.h"        // spec v14 §6: TraceAbilityDebuff — Oyster's poison slow
#include "Audio/TraceAudio.h"                   // spec v26 §9: Jump / WallJump, both client-side
#include "Components/CapsuleComponent.h"        // mantle: capsule dimensions and the clearance sweep
#include "Components/SceneComponent.h"
#include "CollisionQueryParams.h"
#include "Core/TraceCharacter.h"
#include "Engine/World.h"                      // UWorld::GetTimeSeconds (dash-active latch), traces
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"          // spec v7 §5: the aim rotation the dash is composed from
#include "Math/RotationMatrix.h"               // spec v7 §5: the yaw basis the input is decomposed in
#include "Math/UnrealMathUtility.h"
#include "EngineUtils.h"                       // TActorIterator: the dash bash sweep (ships) and the spec v8 §5 carrier harness (dev)
#include "Trace.h"                             // LogTraceGame
#include "TraceSettings.h"
#include "UObject/UnrealType.h"                // FProperty, for the name-bound spec v5 knobs

#if !UE_BUILD_SHIPPING
#include "Containers/Ticker.h"                 // FTSTicker — Trace.Resets.Arm's hold loop
#include "Components/StaticMeshComponent.h"    // ledge test: the block it builds for itself
#include "Engine/Engine.h"
#include "Gameplay/TraceCore.h"                // spec v8 §5: the real pickup funnel, ATraceCore::TryPickup
#include "Core/TracePlayerController.h"        // spec v9 §2: the REAL HUD feed, GetDashHudState
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/PlayerController.h"    // measurement harness: player-controlled check
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

// Every number this file runs on is a live UTraceSettings property, read at the point of use. There
// is deliberately NO detection shim: the spec v3 §2 knobs (Movement|Air, Movement|Landing, the slide
// entry/impulse/cooldown block, DashExitSpeedMultiplier) are real properties now, and a shim would
// only serve to swallow a rename and silently substitute a hardcoded default for the designer's
// value instead of failing the build.

namespace TraceMoveCfg
{
	/** Below this the pawn is treated as having no horizontal motion at all. */
	constexpr float SpeedEpsilon = 1.f;
}

// =================================================================================================
// THE SPEC v5 KNOBS, AND WHY THEY ARE BOUND BY NAME
// =================================================================================================
//
// Spec v5 §1, §3 and §7 need eighteen new tunables. UTraceSettings belongs to another ownership
// slice this pass, so the UPROPERTYs cannot be declared from here — and shipping the numbers as
// hardcoded literals would mean shipping a movement pass with nothing to tune, which is worse.
//
// So each knob is resolved ONCE, by name, against UTraceSettings' UClass, and falls back to the
// default written at the call site if the property does not exist yet. The instant the integrator
// adds `float AirStrafeSoftCapSpeed = 950.f;` (and its ini line) the binding takes over, with no
// change here and no rebuild of this file's behaviour.
//
// THIS PROJECT HAS BEEN BITTEN BY EXACTLY THE OPPOSITE ARRANGEMENT: five of eight mode-B knobs were
// dead last pass because a name-bound lookup silently missed and nobody could tell. That is why
// TraceMoveKnob::LogBindReport() prints, once per process at Display, every knob's name, the value
// actually in force and whether it came from UTraceSettings or from the fallback. "BOUND" means the
// ini can drive it; "default" means it cannot, and the report says so in as many words.
//
// Game thread only (movement is), and the cache is keyed on the property name, so the cost after the
// first move is one TMap lookup per knob per read — the same order as the UTraceSettings::Get()
// reads every other accessor in this file already does.

namespace TraceMoveKnob
{
	struct FBinding
	{
		const FProperty* Property = nullptr;
		bool bResolved = false;
	};

	/** Name -> resolved property (or a resolved miss). Never invalidated: UClass layout is static. */
	static TMap<FName, FBinding>& Bindings()
	{
		static TMap<FName, FBinding> Map;
		return Map;
	}

	/** Recorded purely so the bind report can name the knobs that fell back. */
	static TMap<FName, bool>& BindReport()
	{
		static TMap<FName, bool> Map;
		return Map;
	}

	static const FProperty* Resolve(const FName Name)
	{
		FBinding& Binding = Bindings().FindOrAdd(Name);
		if (!Binding.bResolved)
		{
			Binding.bResolved = true;
			Binding.Property = UTraceSettings::StaticClass()->FindPropertyByName(Name);
			BindReport().Add(Name, Binding.Property != nullptr);
		}
		return Binding.Property;
	}

	static float Float(const FName Name, const float Default)
	{
		if (const FProperty* Property = Resolve(Name))
		{
			if (const FFloatProperty* AsFloat = CastField<FFloatProperty>(Property))
			{
				return AsFloat->GetPropertyValue_InContainer(&UTraceSettings::Get());
			}
			if (const FDoubleProperty* AsDouble = CastField<FDoubleProperty>(Property))
			{
				return static_cast<float>(AsDouble->GetPropertyValue_InContainer(&UTraceSettings::Get()));
			}
		}
		return Default;
	}

	static int32 Int(const FName Name, const int32 Default)
	{
		if (const FProperty* Property = Resolve(Name))
		{
			if (const FIntProperty* AsInt = CastField<FIntProperty>(Property))
			{
				return AsInt->GetPropertyValue_InContainer(&UTraceSettings::Get());
			}
		}
		return Default;
	}

	static bool Bool(const FName Name, const bool bDefault)
	{
		if (const FProperty* Property = Resolve(Name))
		{
			if (const FBoolProperty* AsBool = CastField<FBoolProperty>(Property))
			{
				return AsBool->GetPropertyValue_InContainer(&UTraceSettings::Get());
			}
		}
		return bDefault;
	}
}

// =================================================================================================
// SPEC v9 §0 — THE A/B ARM FOR THE §§5-8 TUNING ITEMS.
// =================================================================================================
//
// §0's complaint is that a harness which never went red proves nothing. That applies to the TUNING
// items as much as to the §2 bug: "slide length is 30% shorter" is a claim about a DIFFERENCE, and a
// single number measured after the change cannot show a difference.
//
// Rebuilding the old code to get the "before" number would mean comparing two binaries, which the
// Trace.DashLegacyAimReplay comment in this file already calls out as dishonest — a second build can
// differ in ways nobody accounted for. So instead every v9 §§5-8 change is expressed as a NAMED
// SCALAR ON TOP of the designer's existing value, and this one switch forces every one of those
// scalars back to its identity. One binary, one harness, both arms.
//
// Identity values, stated once so the arm cannot drift from the shipped numbers:
//   §5 WallJumpMomentumScale        0.90 -> 1.00   (retention back to the designer's 0.95)
//   §5 WallJumpWindowScale          0.60 -> 1.00   (window back to the designer's 0.25 s)
//   §6 SlideMaxLengthScale          0.70 -> 1.00
//   §7 SlideJumpBonusScale          1.43 -> 1.00   (v16 §0 raised it from 1.30)
//   §8 AirStrafeAsymptoteScale      1.10 -> 1.00
//   §8 MovementGravityScale         1.12 -> 1.00
//
// DEFINED HERE, OUTSIDE ANY BUILD GUARD, AND THAT IS A LINK FIX RATHER THAN A STYLE CHOICE. It used
// to be declared `extern` at this point and DEFINED inside the `#if !UE_BUILD_SHIPPING` dev block
// several thousand lines below, while IsV9LegacyTuning() — read by GetSlideJumpWindowSpeedBonus(),
// GetAirStrafeAsymptoteScale(), GetWallJumpSpeedRetention() and RefreshEngineTunablesFromSettings(),
// all of which ship — reads it. A Shipping build compiles every one of those readers and then finds
// no definition to link them against. macOS never noticed because nobody builds Shipping here.
//
// Only the CONSOLE REGISTRATION belongs in the dev block, and that is where it stays: the variable
// costs four bytes in Shipping and is always zero there, so the legacy arm is unreachable in a shipped
// build exactly as intended. Same shape as the v18 §1a globals below, which were written this way
// from the start after this bug was spotted.
int32 GTraceV9LegacyTuning = 0;

/**
 * PATCH 28 §5 — THE A/B ARM FOR THE AIR-CONTROL LIMITER OVERRIDE.
 *
 * 1 restores UCharacterMovementComponent::LimitAirControl on surf planes, i.e. the engine's stock
 * behaviour, in the SAME BINARY. That is not an approximation of "before": it IS before, because the
 * override is the only thing this patch changes about air control.
 *
 * It exists because the override was not deduced, it was MEASURED — the first -TraceSurfTest run had
 * an ideal-strafe arm and a no-input arm that agreed to 1 uu/s — and a fix found by measurement has to
 * stay falsifiable by the same measurement. Defined here rather than in the dev block, in the same
 * shape as GTraceV9LegacyTuning above: the definition costs nothing (the Shipping link finds no
 * reference to it and strips it — measured, `_GTraceSurfLegacyAirLimit` is gone from the relinked
 * artefact), and a global that is defined unconditionally cannot become the "Shipping build compiles
 * a reader and finds no definition" link failure the block above records this file already having
 * had. Only the console REGISTRATION is dev-only.
 *
 * *** THE CONSOLE REGISTRATION BEING DEV-ONLY IS NOT ENOUGH, AND THIS COMMENT USED TO SAY IT WAS. ***
 * It read "Only the console registration is dev-only, so the arm is unreachable in a shipped build
 * exactly as intended", and that was false: the reader below ALSO carried a bare
 * FParse::Param(TEXT("TraceSurfLegacyAirLimit")) with no build guard on it, so the command-line half
 * of the arm shipped. A UTF-16LE search of the linked Trace-Mac-Shipping found the switch literal
 * present while every guarded sibling (-TraceLegacyTuning, -TraceLegacyWallJump,
 * -TraceV24LegacySlide, -TraceV26LegacySlideJump, -TraceLegacyAirReverse) was absent. W9-SHIPGUARD
 * closed it the same way the siblings are closed — the reader is IsSurfLegacyAirLimit() below, whose
 * Shipping branch returns false and compiles both paths away.
 */
int32 GTraceSurfLegacyAirLimit = 0;

/**
 * True while the legacy (stock-engine) air-control limiter is in force on surf planes.
 *
 * BOTH A CVAR AND A COMMAND-LINE SWITCH outside Shipping, for the reason GetDashCooldownRemaining()
 * spells out at length and IsV9LegacyTuning() repeats: -ExecCmds fires at PostEngineInit and an
 * ECVF_Cheat variable set that early does not reliably survive into a session, while FParse of the
 * command line cannot miss. It matters more here than usual because the rig this arm exists for
 * (-TraceSurfTest) starts six seconds into the map — an arm that failed to apply would look exactly
 * like an arm that applied and changed nothing.
 *
 * Read on both machines and on every replayed frame, and a pure function of config either way, so it
 * needs no saved-move state and cannot rubber-band. It must never be flipped mid-session on one end
 * of a live connection — that is a config change, not a prediction input.
 */
/**
 * DEMO 29 ITEM 4 — THE A/B ARM FOR THE EXIT AND ENTRY WORK.
 *
 * 1 restores the Patch 28 behaviour of everything Demo 29 item 4 changes about the MOVEMENT code, in
 * the SAME BINARY:
 *
 *   * CanStepUp() refuses a surf plane again (and StepUp() stops refusing it), which is the flypaper
 *     bug: MoveAlongFloor takes NEITHER of its two branches when CanStepUp() is false and the
 *     component still reports CanCharacterStepUp(), so a walking pawn that touched a rail got no
 *     HandleImpact and no SlideAlongSurface, did not move, and had its velocity re-derived from a
 *     zero displacement. Measured, at every approach angle from 90 degrees down to a 20 degree
 *     graze: 800 uu/s -> 0.
 *   * the ground -> surf entry is off, so running at a rail does nothing;
 *   * the exit rollout is off, so a landing deletes the ride's vertical component;
 *   * the exit carry window is off, so the overspeed bleed starts the instant the pawn touches down.
 *
 * Same shape and same reasoning as GTraceSurfLegacyAirLimit above, including the Shipping guard on
 * the READER rather than only on the console registration — that distinction is the bug W9-SHIPGUARD
 * found in this file's last legacy arm and it is not going to be re-introduced here.
 */
int32 GTraceSurfLegacyExit = 0;

/**
 * True while the Patch 28 exit/entry behaviour is in force. See GTraceSurfLegacyExit.
 *
 * Read on both machines and on every replayed frame and a pure function of config either way, so it
 * needs no saved-move state — but like every other legacy arm in this file it must never be flipped
 * mid-session on one end of a live connection. It changes what CanStepUp(), StepUp(), HandleImpact(),
 * ProcessLanded() and the ground bleed do, all of which run on the server and on every client.
 */
static bool IsSurfLegacyExit()
{
#if UE_BUILD_SHIPPING
	return false;
#else
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfLegacyExit"));
	return GTraceSurfLegacyExit != 0 || bFromCommandLine;
#endif
}

static bool IsSurfLegacyAirLimit()
{
#if UE_BUILD_SHIPPING
	// Same rule as IsV9LegacyTuning: the legacy arm is Development A/B evidence only. A
	// spec-archaeology switch must not let one machine run a different simulation than its peers,
	// and this one is worse than most — LimitAirControl feeds PhysFalling on EVERY machine, so a
	// listen host launched with -TraceSurfLegacyAirLimit would simulate every pawn's surf air
	// control with the stock limiter while each joined client predicted the patched one, i.e. a
	// correction on every ride for every client, from one command-line token.
	return false;
#else
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceSurfLegacyAirLimit"));
	return GTraceSurfLegacyAirLimit != 0 || bFromCommandLine;
#endif
}

/**
 * True while the pre-v9 tuning is in force.
 *
 * BOTH A CVAR AND A COMMAND-LINE SWITCH, for the reason GetDashCooldownRemaining() spells out at
 * length: -ExecCmds fires at PostEngineInit and an ECVF_Cheat variable set that early does not
 * reliably survive into a session on a client that has not connected yet. FParse of the command line
 * cannot miss. GRAVITY IS THE REASON THIS MATTERS MORE HERE than it did there: GravityScale is
 * pushed onto the engine field once per simulated move, so an arm that failed to apply would look
 * exactly like an arm that applied and changed nothing.
 *
 * Read on both machines and on every replayed frame, and a pure function of config either way, so it
 * needs no saved-move state and cannot rubber-band. It must never be flipped mid-session on one end
 * of a live connection — that is a config change, not a prediction input.
 */
static bool IsV9LegacyTuning()
{
#if UE_BUILD_SHIPPING
	// Legacy arms are A/B evidence, not a player option: a spec-archaeology switch must not let one
	// machine run a different simulation than its peers. The CVar registration already lives in the
	// dev block; this closes the -TraceLegacyTuning command-line path too.
	return false;
#else
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyTuning"));
	return GTraceV9LegacyTuning != 0 || bFromCommandLine;
#endif
}

// =================================================================================================
// SPEC v10 §5 — THE A/B ARM FOR THE WALL-JUMP STICK FIX.
//
// THE TESTING RULE FOR THIS PROJECT IS "REPRODUCE THE SYMPTOM FAILING FIRST, THEN SHOW THE SAME
// REPRODUCTION PASSING", and the only honest way to do that for a feel bug is to run the SAME
// harness, in the SAME build, with the fix on and off. Two separately-built binaries are two
// different populations of frame timing and network jitter, and the previous pass's "verified" claim
// was built on exactly that kind of comparison.
//
// Non-zero (or -TraceLegacyWallJump) restores the shipped-v9 wall jump precisely: no post-launch
// into-wall input lockout, no buffered press, and the v9 retention. Everything else is untouched.
//
// Read on both machines and on every replayed frame, and a pure function of config either way, so it
// needs no saved-move state. Do not flip it mid-session on one end of a live connection.
// =================================================================================================
// Defined here rather than `extern`-declared, for the Shipping-link reason spelled out on
// GTraceV9LegacyTuning above: IsV10LegacyWallJump() is read by GetWallJumpSpeedRetention(), which
// ships. Its console registration still lives in the dev block.
int32 GTraceV10LegacyWallJump = 0;

static bool IsV10LegacyWallJump()
{
#if UE_BUILD_SHIPPING
	// Same rule as IsV9LegacyTuning: the legacy arm is Development A/B evidence only.
	return false;
#else
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyWallJump"));
	return GTraceV10LegacyWallJump != 0 || bFromCommandLine || IsV9LegacyTuning();
#endif
}

// =================================================================================================
// SPEC v24 §8 / SPEC v25 §6 — THE A/B ARM FOR THE SHORTER SLIDE AND THE EARLIER BONUS WINDOW.
//
// v24 §8, verbatim: "The 'bonus window' for getting a boost while sliding needs to happen .4 seconds
// earlier in the slide, shorten the duration of the slide by .4seconds as well."
//
// v25 §6, verbatim: "Make the window for slide jumping .2seconds earlier, and decrease slide
// duration by .2seconds. The window should be right at the end of the slide"
//
// THE SAME REQUEST TWICE, AND THE SAME ONE-LINE ANSWER: SlideDurationTrimSeconds accumulates
// (0.4 -> 0.6) and this arm still zeroes it.
//
// ONE CHANGE DELIVERS BOTH SENTENCES, and that is a property of this component rather than a
// shortcut. The bonus window is anchored to the slide's END — IsSlideJumpWellTimed() is
// `GetSlideTimeLeft() <= GetSlideJumpWindowSeconds()`, not "t >= some offset from the start" — so
// cutting 0.2 s off the slide moves the moment the window OPENS 0.2 s earlier in the slide, exactly
// as asked, and moves its close with it. Applying the same 0.2 s a second time to
// SlideJumpWindowSeconds (0.20 -> 0.40) would open the window 0.26 s into a 0.66 s slide, i.e. 61%
// of every slide would be "well timed" — that is not a window, and it is not what either spec
// describes ("the window's position relative to the END of the slide is roughly preserved while the
// slide itself is tighter").
//
// v25 §6's THIRD sentence is an acceptance criterion — "the window should be right at the end of the
// slide" — and it is satisfied EXACTLY rather than approximately, for the same structural reason.
// The window is the slide's LAST GetSlideJumpWindowSeconds() seconds, so its close and the slide's
// end are the same instant (offset 0.000 s) at every value of either knob. What moved is where that
// pair sits in absolute time, and how much of the slide the window covers:
//
//     arm            slide      window opens   window closes   window as % of slide
//     v23 (legacy)   1.260 s    1.060 s        1.260 s         16%
//     v24 §8         0.860 s    0.660 s        0.860 s         23%
//     v25 §6         0.660 s    0.460 s        0.660 s         30%
//
// Nothing was retuned to make the criterion land; it lands on the arithmetic. Measured off live
// slides by the V24WINDOW line below (-TraceSlideDebug), which prints the open and close this
// component actually produced rather than restating these numbers.
//
// So the shipped change is SlideDurationTrimSeconds = 0.6, subtracted from the slide length AFTER
// SlideMaxLengthScale, and this switch forces that trim back to its identity (0) — the same "one
// binary, one harness, both arms" rule GTraceV9LegacyTuning above is built on. Nothing else moves:
// unlike -TraceLegacyTuning this arm does not revert gravity, the air caps or the wall jump, so a
// window time measured against it is a measurement of §8/§6 and of nothing else. Note the arm is
// now a 0.6 s A/B rather than a 0.4 s one: it reverts the ACCUMULATED trim, not just v25's share.
//
// Defined outside every build guard for the Shipping-link reason spelled out on GTraceV9LegacyTuning:
// GetSlideDuration() reads it and GetSlideDuration() ships. Registration lives in the dev block.
int32 GTraceV24LegacySlide = 0;

static bool IsV24LegacySlide()
{
#if UE_BUILD_SHIPPING
	// Same rule as IsV9LegacyTuning: the legacy arm is Development A/B evidence only.
	return false;
#else
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceV24LegacySlide"));
	return GTraceV24LegacySlide != 0 || bFromCommandLine;
#endif
}

// =================================================================================================
// SPEC v26 §3 — THE A/B ARM FOR THE 20% CUT AND THE CHAIN CEILING.
//
// v26 §3, verbatim: "Reduce slide jump momentum boost by 20%. Add a ceiling to slide jump momentum
// boosts, so that you can't chain them over and over to go faster and faster. Right now, if you do
// three slide jump boosts in a row you can zip down the whole field. For now, lets cap it at what
// the momentum is after you do two consecutive slide boosts."
//
// THE CLAIM THIS ARM EXISTS TO FALSIFY IS A CLAIM ABOUT THE THIRD HOP, and a claim about a
// DIFFERENCE cannot be settled by one number measured after the change. Same argument, same shape
// and the same one-binary rule as the three arms above: non-zero (or -TraceV26LegacySlideJump)
// forces BOTH halves of §3 back to their identities —
//
//   §3a SlideJumpMomentumScale        0.80 -> 1.00   (the well-timed multiplier back to 1.446875)
//   §3b bSlideJumpChainCapEnabled     ON   -> OFF    (chaining compounds without limit again)
//
// so Trace.Move.AuditV16.SlideChain can run four chained hops in the RED arm, show the third and
// fourth still climbing, and then run the identical phases in the GREEN arm and show the third
// pinned to the second. One binary, one harness, one lane of arena, both arms.
//
// bSlideJumpChainCapEnabled in Project Settings reverts the CEILING ONLY and is the designer-facing
// switch; this one reverts the whole item and is dev-only, which is the same division of labour as
// bSlideJumpEnabled vs Trace.V9LegacyTuning.
//
// Defined outside every build guard for the Shipping-link reason spelled out on GTraceV9LegacyTuning:
// GetSlideJumpWindowSpeedBonus() and DoJump() read it, and both ship. Registration lives in the dev
// block with the others.
int32 GTraceV26LegacySlideJump = 0;

static bool IsV26LegacySlideJump()
{
#if UE_BUILD_SHIPPING
	// Same rule as IsV9LegacyTuning: the legacy arm is Development A/B evidence only.
	return false;
#else
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceV26LegacySlideJump"));
	return GTraceV26LegacySlideJump != 0 || bFromCommandLine || IsV9LegacyTuning();
#endif
}

// =================================================================================================
// SPEC v18 §1a — THE A/B ARM FOR THE AIR REVERSAL BRAKE.
//
// Same argument as the two arms above, and the same shape. "Reversing in the air does nothing" is a
// claim about a DIFFERENCE between two builds, and this project's standing rule is that a harness
// which cannot go red is not evidence. Non-zero (or -TraceLegacyAirReverse) forces
// GetAirStrafeOpposingDeceleration() to 0, which is EXACTLY the shipped v17 air model — the brake is
// the only thing v18 §1a adds, and at 0 the arithmetic below is line-for-line what it always was.
//
// So Trace.Move.V18.AirReverse can run both arms in one binary, and the RED arm must reproduce the
// user's sentence ("your momentum doesn't change at all") while the GREEN arm slows.
//
// Read on both machines and on every replayed frame, and a pure function of config either way, so it
// needs no saved-move state. Do not flip it mid-session on one end of a live connection.
// =================================================================================================
//
// DEFINED HERE RATHER THAN DOWN IN THE DEV CVAR BLOCK WITH ITS NEIGHBOURS, and deliberately: they are
// read by GetAirStrafeOpposingDeceleration(), which is shipping code, so a definition inside
// `#if !UE_BUILD_SHIPPING` would fail the Shipping link. The v9 and v10 arms above had exactly that
// latent bug when this was written; the v18 §2 integration pass moved their definitions out here too,
// so all four now follow this pattern and only their console registrations live in the dev block.
int32 GTraceV18LegacyAirReverse = 0;

/** Live override for the v18 §1a brake, uu/s². Negative means "use the name-bound knob". */
float GTraceAirOpposingDecel = -1.f;

static bool IsV18LegacyAirReverse()
{
#if UE_BUILD_SHIPPING
	// Same rule as IsV9LegacyTuning: the legacy arm is Development A/B evidence only.
	return false;
#else
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyAirReverse"));
	return GTraceV18LegacyAirReverse != 0 || bFromCommandLine;
#endif
}


namespace TraceMovement
{
	/**
	 * Rotates a unit vector toward another by at most MaxDegrees, in the plane containing both.
	 *
	 * Used for the slide's weak steering. Deliberately a fixed angular rate rather than an
	 * interpolation toward a target: the result depends only on (Current, Desired, MaxDegrees), all
	 * of which the replay path reproduces exactly, so client and server land on the same vector.
	 */
	FVector SteerTowards(const FVector& Current, const FVector& Desired, float MaxDegrees)
	{
		const float Dot = FMath::Clamp(FVector::DotProduct(Current, Desired), -1.f, 1.f);
		const float AngleRad = FMath::Acos(Dot);
		const float MaxRad = FMath::DegreesToRadians(FMath::Max(0.f, MaxDegrees));

		if (AngleRad <= MaxRad || MaxRad <= 0.f)
		{
			return (AngleRad <= MaxRad) ? Desired : Current;
		}

		FVector Axis = FVector::CrossProduct(Current, Desired);
		if (!Axis.Normalize())
		{
			// Exactly opposed (or degenerate): spin around Z, which for two planar vectors is the
			// only axis that can produce the turn at all.
			Axis = FVector::UpVector;
		}

		const FVector Result = FQuat(Axis, MaxRad).RotateVector(Current);
		return Result.GetSafeNormal(UE_SMALL_NUMBER, Current);
	}
}

#if !UE_BUILD_SHIPPING
/**
 * Slide instrumentation. Off by default; "-TraceSlideDebug" on the command line or
 * `Trace.SlideDebug 1` in the console turns it on for a measurement run.
 *
 * At Display, not Verbose. A log line nobody can see has twice now been read as a dead mechanic.
 */
int32 GTraceSlideDebug = 0;
static FAutoConsoleVariableRef CVarTraceSlideDebug(
	TEXT("Trace.SlideDebug"),
	GTraceSlideDebug,
	TEXT("Dev only. Non-zero logs every slide's duration, distance and entry/exit speed at Display, "
	     "with a running mean, so a headless match can measure the slide instead of describing it."),
	ECVF_Cheat);

static bool IsSlideDebugEnabled()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceSlideDebug"));
	return bFromCommandLine || GTraceSlideDebug != 0;
}

/**
 * Match-wide slide sample, not per-pawn: ten bots sliding two hundred times between them is one
 * sample of the mechanic, and a mean per pawn would be ten small samples of nothing in particular.
 * Game thread only, and dev-only, so plain file statics are the right amount of machinery.
 */
static int32 GTraceSlideDebugCount = 0;
static float GTraceSlideDebugTotalDuration = 0.f;
static float GTraceSlideDebugTotalDistance = 0.f;

// Trace.MantleDebug and its -TraceMantleDebug command-line twin lived here (spec v5 §7). Both are
// gone with the mantle itself in v12 §5, rather than left registered and reporting nothing: a CVar
// that accepts a value and changes no behaviour is the same dead knob as an ini key that does
// nothing, and this file has to be able to say the mechanic is absent, not merely quiet.

/**
 * Dash instrumentation (spec v7 §5). Same rules as the slide's: off by default,
 * Display when on.
 *
 * This exists because Trace.DashVectorTest measures the PURE function and nothing else. It cannot
 * see whether a grounded upward dash actually reaches MOVE_Falling, whether the per-frame re-assert
 * holds Z on rails, or whether the exit clamp fires — all three of which are runtime behaviour, and
 * two of which are new. A headless match full of bots produces hundreds of real dashes; this turns
 * them into the evidence.
 */
int32 GTraceDashDebug = 0;
static FAutoConsoleVariableRef CVarTraceDashDebug(
	TEXT("Trace.DashDebug"),
	GTraceDashDebug,
	TEXT("Dev only. Non-zero logs every dash's composed direction, launch velocity, movement mode "
	     "and exit velocity at Display, so the vertical dash can be measured in a live match."),
	ECVF_Cheat);

static bool IsDashDebugEnabled()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceDashDebug"));
	return bFromCommandLine || GTraceDashDebug != 0;
}

/**
 * Forward declaration. IsDashPoolDebugEnabled() is defined further down, next to the dash-charge
 * pool it reports on, but it is first CALLED from the movement-update path well above that point.
 * C++ needs the declaration before the call; without it the translation unit fails outright with
 * "use of undeclared identifier", which takes the whole module — and every agent's run rig — with it.
 * Same shape as IsDashDebugEnabled() above.
 */
static bool IsDashPoolDebugEnabled();
#endif

// -------------------------------------------------------------------------------------------
// UTraceCharacterMovementComponent
// -------------------------------------------------------------------------------------------

UTraceCharacterMovementComponent::UTraceCharacterMovementComponent()
{
	bWantsToDash = 0;
	bWantsToSlide = 0;

	// Demo 19 item 4. Zeroed through the setter so the freshness stamp starts cleared too — a
	// non-zero stamp with a zero bit would be harmless today and a trap the first time somebody
	// reordered these two lines.
	SetJumpHeld(false);

	DashTimeRemaining = 0.f;
	DashCharges = 1;
	DashRechargeRemaining = 0.f;
	LastMaxDashCharges = 1;
	DashDirection = FVector::ZeroVector;
	ReplayAimRotation = FRotator::ZeroRotator;
	bReplayAimRotationValid = 0;

	SlideTimeRemaining = 0.f;
	SlideCooldownRemaining = 0.f;
	SlideSpeed = 0.f;
	SlideDirection = FVector::ZeroVector;
	SlideBufferRemaining = 0.f;
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;
	// Spec v26 §3b. Saved-move state like everything above it: a fresh pawn is not mid-chain.
	SlideJumpChainBoosts = 0;
	SlideJumpChainCeiling = 0.f;
	bSlideHeldLastMove = 0;
	bWasAirborneLastMove = 0;

	GroundGraceRemaining = 0.f;

	// Spec v8 §7. Saved-move state like everything above it.
	WallJumpNormal = FVector::ZeroVector;
	WallJumpWindowRemaining = 0.f;
	WallJumpEntryVelocity = FVector::ZeroVector;
	WallJumpsSinceGround = 0;

	// Spec v10 §5 / §1. Saved-move state like everything above it. The bitfield gets no in-class
	// initialiser for the reason the harness bitfields below spell out: every other one in this
	// component is set here, and a mixed convention is how one of them ends up uninitialised.
	WallJumpLaunchNormal = FVector::ZeroVector;
	WallJumpControlLockoutRemaining = 0.f;
	WallJumpInputBufferRemaining = 0.f;
	bKnifeMovementProfile = 0;

	// Patch 28 §5. Saved-move state like everything above it: a fresh pawn is not mid-surf.
	SurfPlaneNormal = FVector::ZeroVector;
	SurfContactRemaining = 0.f;
	SurfEntrySpeed = 0.f;
	SurfElapsedSeconds = 0.f;
	SurfPeakSpeed = 0.f;

	// DEMO 29 ITEM 4. Same argument: a fresh pawn is not carrying a ride's momentum either.
	SurfExitCarryRemaining = 0.f;
	SurfExitSpeed = 0.f;

#if !UE_BUILD_SHIPPING
	bLedgeTestWasGrounded = 0;

	// Spec v7 §5's harness. Bitfields get no in-class initialiser here for the same reason the ledge
	// test's does not: every other one in this component is set in the constructor, and a mixed
	// convention is how one of them ends up uninitialised.
	bDashPitchTestFired = 0;
	bDashPitchTestLogged = 0;
#endif

	// Third-person feel: the capsule turns toward where it is moving. Aim is separate and comes
	// from the control rotation (ATraceCharacter::GetAimDirection), which is why the character
	// leaves bUseControllerRotationYaw off.
	bOrientRotationToMovement = true;
	bUseControllerDesiredRotation = false;
	RotationRate = FRotator(0.f, 900.f, 0.f);

	// Arena-shooter tuning. MaxWalkSpeed is overwritten from UTraceSettings in BeginPlay and re-pushed
	// every movement update by RefreshEngineTunablesFromSettings(), so this literal only covers the
	// window before play starts (and the CDO in the editor). Keep it equal to UTraceSettings::WalkSpeed
	// anyway — a stale value here is what an editor viewport shows before anyone presses Play.
	MaxWalkSpeed = 800.f;   // spec v4 §5: 820 -> 800. Equal to UTraceSettings::WalkSpeed by rule.
	MaxAcceleration = 4096.f;
	BrakingDecelerationWalking = 2600.f;
	GroundFriction = 8.f;
	JumpZVelocity = 640.f;
	bCanWalkOffLedges = true;

	// --- LEDGE STABILITY, spec v5 §7 --------------------------------------------------------------
	//
	// FIX 1 OF 2 FOR THE "RUBBER BANDING ON THE EDGE OF A RAISED SECTION" REPORT, and the only one
	// that is a straight engine setting. See the header for the full diagnosis.
	//
	// SPEC v12 §5: THIS LINE IS NOT MANTLE CODE AND MUST NOT BE DELETED WITH IT. The mantle was fix 3
	// and is gone; this and the ledge grace are fixes 1 and 2 and are what actually keep the client
	// and the server agreeing about a lip. Removing this while removing the mantle is the single
	// mistake that would hand the Demo 5 complaint straight back.
	//
	// PerchRadiusThreshold ships at 0, which disables the reduced-radius perch test entirely: a pawn
	// counts as WALKING while any part of its capsule's bottom hemisphere touches the lip, i.e. while
	// it is balanced on a fraction of a uu. Whether that sweep catches or misses is decided by
	// sub-uu geometry, and the client and the server evaluate it on DIFFERENT sub-steps of the same
	// second, so they take the coin flip independently. Each disagreement puts them on opposite sides
	// of this component's air-model / ground-model split (see CalcVelocity), which is worth hundreds
	// of uu/s rather than the fraction of a uu the geometry actually differed by.
	//
	// 15 uu gives the decision a band instead of an edge. The capsule radius is 42, so a pawn is now
	// "perched" (and falls) once it has less than 15 uu of support, and solidly walking above that —
	// a state both machines reach from the same geometry several frames before it matters.
	//
	// Deliberately NOT bUseFlatBaseForFloorChecks: that changes floor detection everywhere, including
	// on the arena's 40 uu step risers, and this fix has to be surgical.
	PerchRadiusThreshold = 15.f;
	PerchAdditionalHeight = 40.f;

	// --- The air, spec §2.1 ---------------------------------------------------------------------
	//
	// AirControl 1.0 is not "more air control", it is "get out of the way". GetFallingLateralAcceleration
	// scales Acceleration by AirControl before CalcVelocity sees it, and our Source model has to
	// receive the raw wish vector so it can cap in SPEED (AirMaxWishSpeed) rather than in
	// ACCELERATION. Capping the acceleration is exactly what makes perpendicular input brake.
	//
	// Zero lateral friction and zero falling braking: Source has no air friction, and neither do we.
	// Letting go of the stick mid-flight must coast, not decay.
	//
	// These two are re-pushed from UTraceSettings every move by RefreshEngineTunablesFromSettings();
	// the literals only cover the CDO and the window before play starts. Keep them equal to
	// UTraceSettings::AirControl / AirFriction.
	AirControl = 1.f;
	FallingLateralFriction = 0.f;
	BrakingDecelerationFalling = 0.f;

	// Neutralise the engine's low-speed air-control boost. It exists to make the stock lerp usable
	// when you are barely moving; under the projection model it would double the wish vector at low
	// speed, i.e. change the model based on how fast you happen to be going.
	AirControlBoostMultiplier = 1.f;
	AirControlBoostVelocityThreshold = 0.f;

	// Lets ACharacter::Crouch() raise bWantsToCrouch at all — CanEverCrouch() gates it. The capsule is
	// still never resized, because CanCrouchInCurrentState() is overridden to false; this only opens
	// the already-predicted FLAG_WantsToCrouch channel so the crouch key can reach the slide.
	NavAgentProps.bCanCrouch = true;

	// Snap to small corrections rather than sliding: at these speeds a visible slide reads as lag.
	NetworkSimulatedSmoothLocationTime = 0.05f;
	NetworkSimulatedSmoothRotationTime = 0.05f;
}

void UTraceCharacterMovementComponent::BeginPlay()
{
	Super::BeginPlay();

	const UTraceSettings& Settings = UTraceSettings::Get();
	MaxWalkSpeed = FMath::Max(1.f, Settings.WalkSpeed);
	MaxWalkSpeedCrouched = MaxWalkSpeed * 0.5f;

	// Start full. GetMaxDashCharges() is safe this early — a pawn that has not been told it is the
	// carrier simply reads the base pool.
	LastMaxDashCharges = GetMaxDashCharges();
	DashCharges = LastMaxDashCharges;
	DashRechargeRemaining = 0.f;

	// Spec v8 §7 needs ACharacter::JumpMaxCount raised before the first move, not after it — see the
	// note in RefreshEngineTunablesFromSettings, which keeps it live from then on.
	RefreshEngineTunablesFromSettings();

#if !UE_BUILD_SHIPPING
	// One line, once per process, naming every number the kit will actually run on. A measurement
	// run that does not print the configuration it measured is an anecdote — and this is also the
	// cheapest possible check that a "-ini:Game:..." override on the command line really landed.
	{
		static bool bLoggedKitConfig = false;
		if (!bLoggedKitConfig)
		{
			bLoggedKitConfig = true;
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG walk=%.0f | AIR srcAccel=%d accel=%.0f wishCap=%.0f maxAir=%.0f "
				     "| LAND preserve=%d overspeedFric=%.2f overspeedBrake=%.0f turn=%.1f dashExit=%.2fx "
				     "| SLIDE dur=%.2f entryMul=%.2f cooldownFromEnd=%.2f max=%.0f decel=%.0f "
				     "exitRet=%.2f exitCeil=%.2f (ONE-SHOT - spec v5 s3; NO impulse, NO exit floor, NO commit) "
				     "| SLIDEJUMP on=%d retain=%.2f zMul=%.2f window=%.2f windowBonus=%.2f "
				     "| DASH speed=%.0f dur=%.2f cd=%.2f"),
				MaxWalkSpeed,
				IsSourceAirAccelerationEnabled() ? 1 : 0, GetAirAcceleration(), GetAirMaxWishSpeed(), GetMaxAirSpeed(),
				IsLandingMomentumPreserved() ? 1 : 0, GetGroundOverspeedFriction(), GetGroundOverspeedBraking(),
				GetGroundOverspeedTurnRate(), GetDashExitSpeedMultiplier(),
				GetSlideDuration(), GetSlideEntrySpeedMultiplier(), GetSlideCooldownSeconds(),
				Settings.SlideMaxSpeed, GetSlideDeceleration(),
				GetSlideExitSpeedRetention(), GetSlideExitMaxSpeedMultiplier(),
				IsSlideJumpEnabled() ? 1 : 0, GetSlideJumpHorizontalRetention(), GetSlideJumpZMultiplier(),
				GetSlideJumpWindowSeconds(), GetSlideJumpWindowSpeedBonus(),
				GetDashSpeed(), GetDashDuration(), GetDashCooldown());

			// --- SPEC v5, and the bind report ----------------------------------------------------
			//
			// Reading every new knob here is what POPULATES TraceMoveKnob::BindReport(), so the
			// listing below is complete by construction: a knob that is never read cannot be missing
			// from the report, because it is not a knob this file uses.
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG-V5 AIRSTRAFE falloff=%d soft=%.0f hard=%.0f exp=%.2f hardCapOn=%d "
				     "| SLIDE oneShot=1 dur=%.2f hiddenCooldown=%.2f windowBonus=%.2f zBonus=%.2f "
				     "| MANTLE removed (spec v12 §5) "
				     "| LEDGE grace=%.3f perchThreshold=%.1f"),
				IsAirStrafeFalloffEnabled() ? 1 : 0, GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(),
				GetAirStrafeFalloffExponent(), IsAirStrafeHardCapEnabled() ? 1 : 0,
				GetSlideDuration(), GetSlideCooldownSeconds(), GetSlideJumpWindowSpeedBonus(),
				GetSlideJumpWindowZBonus(),
				GetLedgeGroundGraceSeconds(), PerchRadiusThreshold);

			// --- SPEC v10, and the same bind-report argument ------------------------------------
			//
			// EVERY v10 KNOB IS READ HERE EXPLICITLY, and that is not decoration. The knife
			// multipliers are only touched by the ceiling accessors while bKnifeMovementProfile is
			// set — which it never is at BeginPlay — so without these calls they would never be
			// resolved, never appear in the bind report, and a rename could kill all three in silence.
			// The report is only "complete by construction" for knobs this block actually reads.
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG-V10 WALLJUMP retention=%.4f (v9 x %.2f v10) outward=%.0f window=%.3f "
				     "controlLockout=%.3f inputBuffer=%.3f legacyArm=%d "
				     "| KNIFE speed=%.2fx softCap=%.2fx hardCap=%.2fx (base soft=%.0f hard=%.0f maxAir=%.0f "
				     "-> knife soft=%.0f hard=%.0f maxAir=%.0f) "
				     "| DASH firegate=AreWeaponActionsBlocked() [=IsDashing(), consumed by "
				     "UTraceWeaponComponent::CanFire/CanSwing via ATraceCharacter]"),
				GetWallJumpSpeedRetention(),
				TraceMoveKnob::Float(TEXT("WallJumpMomentumScaleV10"), 0.90f),
				GetWallJumpOutwardImpulse(), GetWallJumpWindowSeconds(),
				GetWallJumpControlLockoutSeconds(), GetWallJumpInputBufferSeconds(),
				IsV10LegacyWallJump() ? 1 : 0,
				GetKnifeMoveSpeedMultiplier(), GetKnifeAirStrafeSoftCapMultiplier(),
				GetKnifeAirStrafeHardCapMultiplier(),
				GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(), GetMaxAirSpeed(),
				GetAirStrafeSoftCapSpeed() * GetKnifeAirStrafeSoftCapMultiplier(),
				GetAirStrafeHardCapSpeed() * GetKnifeAirStrafeHardCapMultiplier(),
				GetMaxAirSpeed() * GetKnifeAirStrafeHardCapMultiplier());

			// --- SPEC v18 §1a, and the same bind-report argument again ---------------------------
			//
			// GetAirStrafeOpposingDeceleration() is READ HERE EXPLICITLY for the reason the knife
			// multipliers above are: nothing else resolves it until a player is actually airborne and
			// actually counter-steering, so without this call the knob would be absent from the bind
			// report for the whole of a headless run and a rename would kill it in silence.
			//
			// The derived line next to it is the number a designer actually wants: how long a dead
			// reversal takes to kill 1000 uu/s. t = v / decel, exactly, because the brake is constant
			// at a full 180°.
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG-V18 AIRREVERSE opposingDecel=%.0f uu/s^2 (cvar override=%.0f, <0 = use knob) "
				     "legacyArm=%d | derived: a dead 180 reversal kills 1000 uu/s in %.2f s, and is "
				     "EXACTLY 0 at 90 degrees and inside it (the air strafe is untouched by construction)"),
				GetAirStrafeOpposingDeceleration(), GTraceAirOpposingDecel,
				IsV18LegacyAirReverse() ? 1 : 0,
				1000.f / FMath::Max(1.f, GetAirStrafeOpposingDeceleration()));

			// --- PATCH 28 §5, AND THE SAME BIND-REPORT ARGUMENT ONCE MORE -------------------------
			//
			// Every surf knob is READ HERE EXPLICITLY, for the reason the knife multipliers above are:
			// nothing resolves them until a player is actually on a ramp, so without this call they
			// would be absent from the MOVEKNOB report for the whole of a headless run and a rename
			// would kill all five in silence.
			//
			// The derived line next to them is the one a designer actually wants, and it is derived
			// from the live getters rather than eyeballed: the surf BAND (which faces this build will
			// and will not surf) is stated as the two normal-Z bounds AND as the two slope angles they
			// correspond to, and the ceiling is printed next to the air cap it is a multiple of, so a
			// reader can check the arithmetic and see at a glance that it TRACKS rather than copies.
			{
				const float MinZ = GetSurfMinNormalZ();
				const float MaxZ = GetWalkableFloorZ();
				UE_LOG(LogTraceGame, Display,
					TEXT("MOVECFG-P28 SURF on=%d band Nz (%.3f..%.3f) = slope (%.1f..%.1f deg) "
					     "[upper bound IS GetWalkableFloorZ(), read live - a walkable face can never be "
					     "surfed] | overbounce=%.3f grace=%.3fs | ceiling=%.0f uu/s = airHardCap %.0f x "
					     "%.2f (DERIVED - retune the air cap and this moves with it) | "
					     "wall band |Nz|<=%.2f is the wall jump's and cannot overlap"),
					IsSurfEnabled() ? 1 : 0, MinZ, MaxZ,
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(MaxZ, -1.f, 1.f))),
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(MinZ, -1.f, 1.f))),
					GetSurfOverbounce(), GetSurfContactGraceSeconds(),
					GetAirStrafeHardCapSpeed() * GetSurfSpeedCeilingMultiplier(),
					GetAirStrafeHardCapSpeed(), GetSurfSpeedCeilingMultiplier(),
					GetWallJumpMaxNormalZ());

			// DEMO 29 ITEM 4, ON ITS OWN LINE for the reason every other MOVECFG line is on one: a
			// knob that is never printed is a knob nobody can check, and these five decide whether a
			// rail feels like a ramp or like flypaper.
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG-D29 SURF EXIT/ENTRY  groundEntry=%d minApproach=%.0f uu/s (of a %.0f uu/s "
				     "walk limit) | exit carry %.2fs at bleed x%.2f | rollout retention %.2f | "
				     "ceiling(any profile) %.0f uu/s | legacyArm=%d"),
				// MaxWalkSpeed, not GetMaxSpeed(): this runs at BeginPlay with MovementMode still
				// MOVE_None, where GetMaxSpeed() is 0 and the line would report a threshold of zero for
				// a rule that is evaluated at runtime against 800. The two agree on a walking pawn,
				// which is the only state the threshold is ever read in.
				IsSurfGroundEntryEnabled() ? 1 : 0,
				MaxWalkSpeed * FMath::Clamp(
					TraceMoveKnob::Float(TEXT("SurfGroundEntryApproachFraction"), 0.2f), 0.02f, 1.f),
				MaxWalkSpeed,
				GetSurfExitCarrySeconds(), GetSurfExitCarryBleedScale(), GetSurfExitRolloutRetention(),
				GetSurfSpeedCeilingMax(), IsSurfLegacyExit() ? 1 : 0);

			// BUTTRESS PASS, ITEM 7, on this line rather than a sixth one: it is one bool, and it
			// belongs with the entry rule it is the mirror of.
			//
			// *** READING IT HERE IS WHAT PUTS IT IN THE MOVEKNOB REPORT AT ALL. *** That report walks
			// TraceMoveKnob's bind map, and a name only enters the map when something RESOLVES it —
			// so a knob first read from DoJump would be missing from the report on every run where
			// nobody pressed jump before BeginPlay's sweep, which is all of them. MEASURED: before
			// this call the report listed 32 bound / 5 fallback with bSurfBlocksJump absent, and
			// Trace.VerifyKnobs said "OK bSurfBlocksJump = true" the whole time — i.e. the property
			// was fine and the hygiene line simply could not see it.
			UE_LOG(LogTraceGame, Display,
				TEXT("MOVECFG-BUT NO JUMP WHILE SURFING  rule=%d (owner item 7; false restores the "
				     "pre-buttress behaviour, where a mid-ride press armed the wall-jump input buffer)"),
				DoesSurfBlockJump() ? 1 : 0);
			}

			// The knob hygiene check the project's own history demands. A name-bound knob that does
			// not resolve is not a build error and not a runtime error — it is a setting that
			// silently does nothing, which is how five of eight knobs died last pass. Every one of
			// them is named here, every pass, with the word BOUND or the word FALLBACK next to it.
			int32 BoundCount = 0;
			int32 FallbackCount = 0;
			for (const TPair<FName, bool>& Entry : TraceMoveKnob::BindReport())
			{
				UE_LOG(LogTraceGame, Display, TEXT("MOVEKNOB %-28s %s"),
					*Entry.Key.ToString(),
					Entry.Value ? TEXT("BOUND to UTraceSettings (ini-tunable)")
					            : TEXT("FALLBACK to the built-in default (property missing -> ini CANNOT tune it)"));
				(Entry.Value ? BoundCount : FallbackCount)++;
			}
			UE_LOG(LogTraceGame, Display, TEXT("MOVEKNOB summary: %d bound, %d on built-in defaults"),
				BoundCount, FallbackCount);
		}
	}
#endif
}

bool UTraceCharacterMovementComponent::CanCrouchInCurrentState() const
{
	// See the header. bWantsToCrouch is an INPUT to the slide, never a request to shrink the capsule.
	return false;
}

// --- Settings accessors ---------------------------------------------------------------------
//
// Read live rather than cached: client and server must resolve the same numbers, and the config
// CDO is the single source of truth for both.

float UTraceCharacterMovementComponent::GetDashSpeed() const
{
	// SPEC v19 §3 — THE PER-CHARACTER DASH REACH, APPLIED TO SPEED AND NOT TO DURATION.
	//
	// Mortimer's passive is "his dash is much shorter than everybody else's". Reach is speed x
	// duration, and only ONE of those two may move: the duration is what the dash trail's length,
	// the parry window and the i-frames are all measured against, so scaling it would quietly retune
	// three unrelated systems. Scaling the speed moves the reach and nothing else.
	//
	// The trait is 1.0 for every other character and for any pawn without an ability component, so
	// this line is arithmetically identity for the other nine and cannot drift away from them.
	return FMath::Max(1.f, UTraceSettings::Get().DashSpeed * TraceAbilityTraits::GetDashDistanceScale(CharacterOwner));
}

float UTraceCharacterMovementComponent::GetDashDuration() const
{
	// A zero-length dash would be a one-frame teleport that the trail trip test could never see.
	return FMath::Max(0.01f, UTraceSettings::Get().DashDuration);
}

float UTraceCharacterMovementComponent::GetDashCooldown() const
{
	// DEMO 20 ITEM 2, SECOND HALF — "increase mortimer's dash cooldown by 25%".
	//
	// Landed by the v23 integrator, not by the Mortimer agent, only because Movement/ was owned by
	// the Lily agent that pass and neither would edit the other's file; the knob, the accessor and
	// TraceAbilityTraits::GetDashCooldownScale() were all already in place and this was the one
	// missing call site. Trace.Mortimer.DashTest reported "ITEM 2b: FAIL - NOT ROUTED" against it.
	//
	// Same shape and same reasoning as GetDashSpeed() above: the trait returns 1.0 for every other
	// character and for any pawn with no ability component, so this multiply is arithmetically
	// identity for the other nine and cannot drift away from them. GetDashRechargeWindow() is
	// defined as duration + cooldown, so the HUD dash meter follows from this one line with no
	// second edit.
	return FMath::Max(0.f, UTraceSettings::Get().DashCooldown
		* TraceAbilityTraits::GetDashCooldownScale(CharacterOwner));
}

float UTraceCharacterMovementComponent::GetDashRechargeWindow() const
{
	// The cooldown has always been measured from dash START (UTraceSettings::DashCooldown), and the
	// HUD's meter divides by exactly this quantity. Keeping every refill on the same window means a
	// second charge refills on the same rhythm as the first.
	return GetDashDuration() + GetDashCooldown();
}

float UTraceCharacterMovementComponent::GetDashExitSpeedMultiplier() const
{
	// Never below 1: a dash that handed back LESS than a run would be a punishment for dashing.
	return FMath::Max(1.f, UTraceSettings::Get().DashExitSpeedMultiplier);
}

float UTraceCharacterMovementComponent::GetDashExitVerticalSpeedLimit() const
{
	// SPEC v7 §5. See the header note "THE CLIMB". JumpZVelocity rather than a literal so that the
	// one number the whole kit expresses vertical launches in still governs this one, and so that
	// raising the jump raises the dash's ceiling with it instead of silently capping it lower.
	// INTEGRATED: the designer knob asked for in the v7 report now exists, as a MULTIPLE of the jump
	// rather than an absolute, so the tie to JumpZVelocity survives retuning.
	return FMath::Max(0.f, JumpZVelocity * FMath::Max(0.f, UTraceSettings::Get().DashExitVerticalSpeedMultiplier));
}

FRotator UTraceCharacterMovementComponent::GetDashAimRotation() const
{
	// SPEC v7 §5, AND THE ONLY REASON A VERTICAL DASH DOES NOT DESYNC.
	//
	// bClientUpdating is set for exactly the span of ClientUpdatePositionAfterServerUpdate's replay
	// loop, and inside that loop every move's PrepMoveFor has just written ReplayAimRotation. So the
	// gate is both necessary (a live move must use the live mouse) and sufficient (a replayed move
	// can never read a stale rotation, because one was written microseconds earlier).
	if (bReplayAimRotationValid != 0 && CharacterOwner != nullptr && CharacterOwner->bClientUpdating)
	{
		return ReplayAimRotation;
	}

	// The authority's own path and the owning client's original simulation. On the server this is
	// the rotation FCharacterNetworkMoveData delivered and ServerMove_PerformMovement applied to the
	// controller immediately before MoveAutonomous, so it matches what the client had.
	if (CharacterOwner != nullptr)
	{
		if (const AController* OwningController = CharacterOwner->GetController())
		{
			return OwningController->GetControlRotation();
		}
	}

	// No controller at all (a detached or dying pawn). The capsule's own rotation is level under
	// bOrientRotationToMovement, which degrades this to exactly the old horizontal dash.
	return (UpdatedComponent != nullptr) ? UpdatedComponent->GetComponentRotation() : FRotator::ZeroRotator;
}

float UTraceCharacterMovementComponent::GetSlideDuration() const
{
	// Floored rather than defaulted: a zero-length slide would still spend the slide cooldown.
	//
	// SPEC v9 §6 — "Reduce max slide length by 30%", x0.7.
	//
	// LENGTH IS DURATION HERE, and that is not an approximation. Since spec v4 §1 a slide's speed is
	// purely what the player carried in (SlideEntrySpeedMultiplier is 1.00, the flat impulse is
	// deleted) and the only thing that ends it early is decaying to SlideExitSpeedFraction. So the
	// MAXIMUM distance a slide can cover is entry speed integrated over this clock.
	//
	// SCALING THE CLOCK BY 0.7 IS NOT A 30% CUT IN DISTANCE, AND THE DIFFERENCE IS NOT NOISE. Distance
	// is v0.T - ½.a.T², so shortening T also removes part of the QUADRATIC term the player was never
	// going to travel anyway. Measured (Trace.V9.Tuning, entry held at 1250 uu/s, a = 260 uu/s²):
	//
	//     T = 1.80 s -> 1828.8 uu        T = 1.26 s -> 1368.6 uu       = -25.2%, not -30%.
	//
	// The spec's §6 [ASSUMPTION] is explicit — "the maximum distance/duration a slide can cover, ×0.7"
	// — so ×0.7 ON THE CLOCK is what ships, and DURATION is down exactly 30%. If the design owner
	// meant 30% off the DISTANCE, the value that delivers it is SlideMaxLengthScale = 0.647 (solving
	// v0.T - ½.a.T² = 0.7 × 1828.8 gives T = 1.165 s); it is one ini line and it is flagged in the
	// report rather than applied, because the two readings are 5% of a slide apart and that is the
	// design owner's call, not this file's.
	//
	// Scaling the clock rather than the speed is also the only reading that does not contradict
	// spec v4 §1: capping SlideMaxSpeed instead would take momentum the player brought in, which is
	// the exact behaviour Demo 4 asked to have removed.
	const float Base = FMath::Max(0.05f, UTraceSettings::Get().SlideDuration);
	const float Scale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("SlideMaxLengthScale"), 0.7f), 0.05f, 4.f);

	// =============================================================================================
	// SPEC v24 §8 ("shorten the duration of the slide by .4seconds as well")
	//   + SPEC v25 §6 ("decrease slide duration by .2seconds")  =  0.6 s off the shipped slide.
	// =============================================================================================
	//
	// ONE ACCUMULATING KNOB, NOT ONE KNOB PER SPEC. Two passes asked for the same thing and the trim
	// is the running total (0.4 + 0.2), so the shipped slide is 1.80 x 0.70 - 0.60 = 0.66 s.
	//
	// SUBTRACTED FROM THE SHIPPED LENGTH, NOT FROM THE BASE, and that is the whole reason this is a
	// separate knob instead of an edit to SlideDuration. The owner's seconds are seconds of the slide
	// he plays, which is Base x Scale (1.80 x 0.70 = 1.26 s), not seconds of the 1.80 s base —
	// cutting the base by 0.6 would ship 1.20 x 0.70 = 0.84 s, a 0.42 s cut, and the player would
	// feel 70% of what he asked for. Re-typing the base as 0.9429 (0.66 / 0.7) would deliver the
	// right number today and silently become a different cut the moment anybody re-tunes
	// SlideMaxLengthScale, which is precisely the absolute-instead-of-relative failure spec v24 §0 is
	// about. As a trim over the finished length it stays "0.6 s shorter than the slide would
	// otherwise be" whatever the base and the v9 §6 scale do next.
	//
	// AND IT IS ALSO WHERE "the window for slide jumping .2seconds earlier" LIVES. The slide-jump
	// window is the slide's last GetSlideJumpWindowSeconds() seconds, so shortening the slide here
	// drags the window's open AND close 0.2 s earlier and leaves the close sitting exactly on the
	// slide's end — which is v25 §6's third sentence, its acceptance criterion. SlideJumpWindowSeconds
	// is therefore NOT also moved; see the table at the top of this file for the measured numbers.
	//
	// It is also what makes the A/B honest: IsV24LegacySlide() zeroes ONLY this, so the before-arm
	// is the exact v23 slide (1.26 s) in the same binary. See the arm at the top of this file.
	const float Trim = IsV24LegacySlide()
		? 0.f
		: FMath::Max(0.f, UTraceSettings::Get().SlideDurationTrimSeconds);

	// Floored at 0.05 s like every other path out of here: a trim larger than the slide must not be
	// able to produce a zero-length (or negative) slide that still spends the slide cooldown.
	return FMath::Max(0.05f, Base * Scale - Trim);
}

float UTraceCharacterMovementComponent::GetSlideDeceleration() const
{
	// 0 is legal and means "a slide holds its entry speed for its whole duration".
	return FMath::Max(0.f, UTraceSettings::Get().SlideDeceleration);
}

// GetSlideMinCommitSeconds() WAS HERE AND IS DELETED (spec v5 §3). It read the window in which
// releasing crouch could not cancel a slide. A one-shot ability has no partial commit — the whole
// slide is committed the moment it starts — so the knob has nothing left to mean. Delete
// UTraceSettings::SlideMinCommitSeconds and its DefaultGame.ini line with it; a setting that is read
// nowhere is exactly the "silently dead knob" this project keeps getting caught by.

float UTraceCharacterMovementComponent::GetSlideExitSpeedRetention() const
{
	return FMath::Max(0.f, UTraceSettings::Get().SlideExitSpeedRetention);
}

// GetSlideExitMinSpeedFraction() WAS HERE AND IS DELETED (spec v4 §1). It read the exit FLOOR, which
// handed a decayed slide back at exactly WalkSpeed however slowly it was actually going — measured as
// a 73% speed gain for a slow slide, i.e. the flat momentum boost the design owner ruled out.

float UTraceCharacterMovementComponent::GetSlideExitMaxSpeedMultiplier() const
{
	// Never below 1: a multiplier under 1 would make a slide exit SLOWER than a walk, which is the
	// exact behaviour this pass exists to remove. Note that EndSlide() additionally floors the
	// resulting ceiling at the slide's own speed when momentum preservation is on, so this knob only
	// ever decides how much of a *fast* slide is handed back, never whether one is braked.
	return FMath::Max(1.f, UTraceSettings::Get().SlideExitMaxSpeedMultiplier);
}

float UTraceCharacterMovementComponent::GetSlideCooldownSeconds() const
{
	return FMath::Max(0.f, UTraceSettings::Get().SlideCooldownSeconds);
}

float UTraceCharacterMovementComponent::GetSlideEntrySpeedMultiplier() const
{
	return FMath::Max(0.f, UTraceSettings::Get().SlideEntrySpeedMultiplier);
}

// GetSlideImpulse() WAS HERE AND IS DELETED (spec v4 §1). It read the FLAT additive on slide entry —
// worth the same whether you entered at a walk or out of a dash, which is exactly what "the flat
// momentum boost should be ruled out" means. SlideEntrySpeedMultiplier (1.0) is now the only thing
// between entry speed and slide speed.

bool UTraceCharacterMovementComponent::IsSlideJumpEnabled() const
{
	return UTraceSettings::Get().bSlideJumpEnabled;
}

float UTraceCharacterMovementComponent::GetSlideJumpHorizontalRetention() const
{
	// Not floored at 1: the user is allowed to make a slide-jump cost speed if that is what plays
	// well. Floored at 0 only so a negative value cannot reverse the pawn's direction of travel.
	return FMath::Max(0.f, UTraceSettings::Get().SlideJumpHorizontalRetention);
}

float UTraceCharacterMovementComponent::GetSlideJumpZMultiplier() const
{
	return FMath::Max(0.f, UTraceSettings::Get().SlideJumpZMultiplier);
}

float UTraceCharacterMovementComponent::GetSlideJumpWindowSeconds() const
{
	// Clamped to the slide's own length, like SlideMinCommitSeconds: a window longer than the slide
	// means every slide-jump is well timed, which is not a window, and leaving it unclamped lets one
	// bad number quietly turn the bonus into a permanent multiplier.
	return FMath::Clamp(UTraceSettings::Get().SlideJumpWindowSeconds, 0.f, GetSlideDuration());
}

float UTraceCharacterMovementComponent::GetSlideJumpWindowSpeedBonus() const
{
	// Never below 1: the window must never be able to PUNISH a well-timed hop. Missing the timing is
	// allowed to be worth less; hitting it must never be worth less than missing it.
	//
	// SPEC v5 §3 RAISES THIS FROM 1.10 TO 1.25. The property is UTraceSettings::
	// SlideJumpWindowSpeedBonus and the shipped value lives in DefaultGame.ini, which wins over the
	// header default — both have to move or the retune does nothing. 1.10 was a 10% edge on a 0.9s
	// arc, which is roughly the frame-to-frame noise a player sees anyway; with the hold-to-extend
	// gone this is the only skill expression sliding has left, so it has to be legible.
	//
	// =============================================================================================
	// SPEC v9 §7 — "Increase the bonus of timing a slide jump right by 30%". ONE NUMBER TO SWITCH.
	// =============================================================================================
	//
	// The sentence is genuinely ambiguous and the two readings are far apart, so both are
	// implemented and the choice is a single bool:
	//
	//   bSlideJumpBonusScalesGainOnly = true  (SHIPPED, and the spec's [ASSUMPTION])
	//       "The bonus" is the part above 1.0 — the thing the timing actually buys.
	//       1 + (1.3125 - 1) x 1.43 = 1 + 0.446875 = 1.446875.
	//       A well-timed hop at 1900 uu/s carries 2749 uu/s instead of 2494 uu/s: +255 uu/s, a
	//       legible step up from a bonus that was already legible.
	//
	//   bSlideJumpBonusScalesGainOnly = false (THE ALTERNATIVE, flagged as the spec asks)
	//       "The bonus" is the whole multiplier. 1.3125 x 1.43 = 1.876875.
	//       The same hop carries 3566 uu/s — 88% over entry speed, which beats DashSpeed's own
	//       3300 uu/s. A slide-hop that is faster than a dash inverts the game's counterplay
	//       (the dash is the only answer to a carrier), so this reading is NOT shipped by default.
	//
	// SPEC v16 §0 raised the SCALE from 1.30 to 1.43 — "increase the well-timed slide jump bonus by
	// 10%" read as +10% ON THE GAIN, which is what bSlideJumpBonusScalesGainOnly already means: the
	// gain goes 0.40625 -> 0.446875, exactly +10%. Read against the WHOLE multiplier instead, "+10%"
	// would have been 1.546875, and this file's own semantics are what settled it.
	//
	// Base stays the designer's DefaultGame.ini value (1.3125, which wins over the header) and the
	// spec's increase is a separate named scalar on top — so re-tuning the base and re-tuning the
	// v9 increase never fight.
	const float Base = FMath::Max(1.f, UTraceSettings::Get().SlideJumpWindowSpeedBonus);
	const float Scale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("SlideJumpBonusScale"), 1.43f), 0.1f, 4.f);
	const bool bGainOnly = TraceMoveKnob::Bool(TEXT("bSlideJumpBonusScalesGainOnly"), true);

	float Global = FMath::Max(1.f, bGainOnly ? (1.f + (Base - 1.f) * Scale) : (Base * Scale));

	// =============================================================================================
	// SPEC v26 §3a — "Reduce slide jump momentum boost by 20%." ONE FACTOR, APPLIED LAST.
	// =============================================================================================
	//
	// THE GAIN, NOT THE MULTIPLIER, and that reading is not a new judgement call — it is the one this
	// function already ships for v9 §7 (bSlideJumpBonusScalesGainOnly, see the block above). "The
	// boost" is the part of the multiplier above 1.0: at SlideJumpHorizontalRetention 1.0 the value
	// 1.0 is pure momentum PRESERVATION, which is what escaping the ground friction is worth and is
	// not something §3 asks to cut. So:
	//
	//     1 + (1.446875 - 1) x 0.80 = 1.357500
	//
	// THE ALTERNATIVE READING, stated so it can be chosen rather than rediscovered: -20% of the WHOLE
	// multiplier is 1.446875 x 0.80 = 1.1575. That takes the gain from 0.446875 to 0.1575 — a 65% cut
	// of the only thing timing the hop buys — for a note that says 20%, and it would leave the
	// well-timed hop worth less than the v5 §3 bonus it replaced. If the owner meant that one, set
	// SlideJumpMomentumScale to 0.3524 (= 0.1575 / 0.446875) and nothing else moves.
	//
	// A MISTIMED HOP IS UNTOUCHED BY ARITHMETIC, NOT BY A CARVE-OUT: DoJump only multiplies by this
	// value when bWellTimed, and the retention it multiplies is 1.0, which has no gain to scale.
	//
	// WHY IT IS ITS OWN KNOB AND NOT AN EDIT TO THE TWO ABOVE. SlideJumpWindowSpeedBonus is v8 §8's
	// base and SlideJumpBonusScale is v9 §7's increase as re-raised by v16 §0. Folding v26's cut into
	// either would put three specs' decisions in one number, and the next person to retune that
	// decision would silently delete or double this one. That failure has happened in this file
	// before — see SlideJumpWindowSeconds' note on the 0.2 s that must not be applied twice.
	//
	// BEFORE THE CHARACTER SEAM, deliberately. Elle's +30% scales the reduced gain rather than being
	// computed against a number the game no longer ships, which is the same ordering rule the seam's
	// own comment states: every global knob, every legacy arm and both readings of "the bonus" are
	// resolved first, and the ability layer only ever gets to scale the finished number.
	if (!IsV26LegacySlideJump())
	{
		const float MomentumScale = FMath::Clamp(UTraceSettings::Get().SlideJumpMomentumScale, 0.f, 2.f);
		Global = FMath::Max(1.f, 1.f + (Global - 1.f) * MomentumScale);
	}

	// SPEC v18 §2 — Elle's second passive, "+30% on well-timed slide-jump momentum boosts" (v18 §2 asked
	// for +40%; PATCH 28 §3 cut it to +30%, and UTraceSettings::ElleSlideJumpGainBonus ships at 0.30),
	// and the ONE place a character is allowed to change this number.
	//
	// THE GLOBAL IS COMPUTED FIRST AND HANDED OVER, which is what keeps §4's "slide-jump 1.446875
	// (Elle changes only her own)" true: every knob, every legacy arm and both readings of "the bonus"
	// are resolved above, and the ability layer only ever gets to scale the finished number. A
	// character that read UTraceSettings for itself would be a second opinion about what "the bonus"
	// means, and this file has already had two.
	//
	// This component must not learn Elle's name — the same rule GetDashHitSweepRadiusFor() follows for
	// Chut — so the question goes to the character-agnostic seam. Null-safe and identity-returning for
	// every Mannequin, every bot and the other seven characters.
	return UTraceAbilityComponent::GetSlideJumpWindowSpeedBonusFor(CharacterOwner, Global);
}

float UTraceCharacterMovementComponent::GetSlideJumpWindowZBonus() const
{
	// New in spec v5 §3. Height is the channel a player can actually SEE — 25% more planar speed is
	// deniable, clearing a box you could not clear a second ago is not.
	return FMath::Max(1.f, TraceMoveKnob::Float(TEXT("SlideJumpWindowZBonus"), 1.12f));
}

// --- SPEC v26 §3b — the chain ceiling's three reads ---------------------------------------------
//
// DIRECT MEMBER ACCESS, NOT TraceMoveKnob, AND THAT IS THE SAFE CHOICE HERE. The by-name path exists
// so this component can read a knob that may not have landed yet; its cost is that a typo binds to
// nothing and falls back to the default in silence, which is a documented trap in this project's
// house rules. These three properties land in the same change as their readers, so a direct
// UTraceSettings::Get().X is compile-checked and cannot fail quietly. Do not "tidy" them onto the
// name-bound path.

bool UTraceCharacterMovementComponent::IsSlideJumpChainCapEnabled() const
{
	// The dev A/B arm wins over the designer switch, not the other way round: the arm's whole job is
	// to reproduce v25 exactly, and a Project Settings value that could veto it would make the RED
	// run silently green.
	return !IsV26LegacySlideJump() && UTraceSettings::Get().bSlideJumpChainCapEnabled;
}

int32 UTraceCharacterMovementComponent::GetSlideJumpChainCapBoosts() const
{
	// Floored at 1. Zero would mean "cap the chain at the speed it started with", which is not a
	// ceiling on the boost — it is the deletion of the move — and a knob whose bottom value silently
	// removes a mechanic is exactly the sort of setting this file keeps deleting.
	return FMath::Max(1, UTraceSettings::Get().SlideJumpChainCapBoosts);
}

float UTraceCharacterMovementComponent::GetSlideJumpChainResetSpeed() const
{
	// GetMaxSpeed() and not Settings.WalkSpeed: the pawn's ground ceiling already folds in the
	// carrier multiplier (1.22), the knife profile (1.30) and every ability speed passive, so a
	// threshold written as a MULTIPLE of it follows all of them without this function learning any of
	// their names — and retuning WalkSpeed cannot make the chain immortal by accident.
	//
	// The caller is responsible for only asking while the pawn is on its feet. GetMaxSpeed() folds
	// SlideSpeed in while sliding and returns DashSpeed while dashing, so asked at the wrong moment
	// this would compare the slide against itself and never end a chain.
	const float OnFoot = FMath::Max(1.f, GetMaxSpeed());
	return OnFoot * FMath::Clamp(UTraceSettings::Get().SlideJumpChainResetSpeedMultiplier, 0.f, 3.f);
}

// --- The air-strafe accumulation ceiling (spec v5 §1) -------------------------------------------

bool UTraceCharacterMovementComponent::IsAirStrafeFalloffEnabled() const
{
	return TraceMoveKnob::Bool(TEXT("bAirStrafeGainFalloff"), true);
}

float UTraceCharacterMovementComponent::GetAirStrafeAsymptoteScale() const
{
	// SPEC v9 §8 — "Move the asymptote on momentum slightly higher, to allow for slightly faster
	// speeds." ONE scalar over BOTH caps, because they are two points on one curve: the soft cap is
	// where gain starts to taper and the hard cap is where it reaches zero, and moving only one of
	// them changes the SHAPE of the falloff rather than its position. +10% keeps the shape identical
	// and slides the whole asymptote up.
	//
	// A NUDGE, NOT A REMOVAL. The spec is explicit that the cap the user asked for in Demo 5 stays;
	// this is 950 -> 1045 and 1250 -> 1375. MaxAirSpeed (1600) is deliberately untouched, so the
	// tighter of the two is still the hard cap and spec v5 §1 still governs.
	if (IsV9LegacyTuning())
	{
		return 1.f;
	}
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("AirStrafeAsymptoteScale"), 1.10f), 0.5f, 2.f);
}

float UTraceCharacterMovementComponent::GetAirStrafeSoftCapSpeed() const
{
	// 950 = 1.19 x the 800 walk speed. Below it a strafe is worth EXACTLY what it was in Demo 5,
	// which is the part the user called incredible and asked not to be touched.
	//
	// Spec v9 §8 slides this up by GetAirStrafeAsymptoteScale(); at x1.10 the untouched band grows
	// from "below 950" to "below 1045", i.e. Demo 5's feel now survives 10% further up the range.
	//
	// SPEC v10 §1 raises it again, and only while the knife is out — "a higher momentum ceiling".
	return FMath::Max(0.f, TraceMoveKnob::Float(TEXT("AirStrafeSoftCapSpeed"), 950.f)
		* GetAirStrafeAsymptoteScale()
		* (bKnifeMovementProfile ? GetKnifeAirStrafeSoftCapMultiplier() : 1.f));
}

float UTraceCharacterMovementComponent::GetAirStrafeHardCapSpeed() const
{
	// Always strictly above the soft cap: the falloff divides by (Hard - Soft), and a designer who
	// set them equal would otherwise get a divide-by-zero rather than the "cap everything at the soft
	// cap" they obviously meant.
	//
	// Scaled by the same asymptote knob as the soft cap — see GetAirStrafeAsymptoteScale() for why
	// they have to move together. The Max() below is applied AFTER the scale so the invariant still
	// holds at any scale.
	//
	// SPEC v10 §1: raised while the knife is out. The Max() below still runs AFTER both scales, and
	// the soft cap is scaled by its own knob, so a designer who sets the soft multiplier higher than
	// the hard one gets the "cap everything at the soft cap" behaviour rather than an inverted band.
	return FMath::Max(GetAirStrafeSoftCapSpeed() + 1.f,
		TraceMoveKnob::Float(TEXT("AirStrafeHardCapSpeed"), 1250.f) * GetAirStrafeAsymptoteScale()
			* (bKnifeMovementProfile ? GetKnifeAirStrafeHardCapMultiplier() : 1.f));
}

float UTraceCharacterMovementComponent::GetAirStrafeFalloffExponent() const
{
	// 1 is a linear taper. 2 (shipped) keeps most of the strafe's value until well past the soft cap
	// and then collapses it, which is what "harder and harder past a certain point" describes.
	// Floored at a small positive number rather than 0, because Pow(x, 0) == 1 would silently turn
	// the falloff into a no-op that still reported itself as enabled.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("AirStrafeFalloffExponent"), 2.f), 0.05f, 16.f);
}

bool UTraceCharacterMovementComponent::IsAirStrafeHardCapEnabled() const
{
	return TraceMoveKnob::Bool(TEXT("bAirStrafeHardCap"), true);
}

float UTraceCharacterMovementComponent::GetAirStrafeGainScale(const float PlanarSpeed) const
{
	if (!IsAirStrafeFalloffEnabled())
	{
		return 1.f;
	}

	const float Soft = GetAirStrafeSoftCapSpeed();
	if (PlanarSpeed <= Soft)
	{
		return 1.f;
	}

	const float Hard = GetAirStrafeHardCapSpeed();
	if (PlanarSpeed >= Hard)
	{
		return 0.f;
	}

	// Headroom left, as a fraction of the whole falloff band, raised to the exponent. Pure function
	// of (PlanarSpeed, config) — no state, no time, so a replayed frame lands on the identical value.
	const float Headroom = (Hard - PlanarSpeed) / (Hard - Soft);
	return FMath::Pow(FMath::Clamp(Headroom, 0.f, 1.f), GetAirStrafeFalloffExponent());
}

float UTraceCharacterMovementComponent::GetAirStrafeOpposingDeceleration() const
{
	// The A/B arm's identity value. See IsV18LegacyAirReverse(): 0 IS the shipped v17 air model, so
	// the red arm is not an approximation of the old behaviour, it is the old behaviour.
	if (IsV18LegacyAirReverse())
	{
		return 0.f;
	}

	// Name-bound like every other knob resolved through TraceMoveKnob. UTraceSettings now carries
	// `float AirStrafeOpposingDeceleration = 2200.f;` and DefaultGame.ini carries the matching key, so
	// this reads BOUND and the ini drives it; it ran on the fallback literal below for part of this
	// pass and behaved identically, because the two numbers are the same. BeginPlay's MOVEKNOB report
	// prints BOUND or FALLBACK for the name every run, so a future rename can never be silent — this
	// project has shipped five dead knobs that way once already.
	//
	// The live override exists because this number is the one thing in v18 §1 that can only be settled
	// by feel, and the spec says the tuning comes after the first implementation. Negative means "use
	// the knob", which is why the CVar's default is -1 rather than the shipped value: a CVar holding a
	// second copy of the default is a second thing that can drift.
	const float Override = GTraceAirOpposingDecel;
	const float Value = (Override >= 0.f)
		? Override
		: TraceMoveKnob::Float(TEXT("AirStrafeOpposingDeceleration"), 2200.f);

	// Ceilinged at the air acceleration: a brake stronger than the accel would make a full reversal
	// come to a dead stop faster than the player could build the speed back, which is the "hitting a
	// wall" feel the spec rules out in as many words.
	return FMath::Clamp(Value, 0.f, FMath::Max(1.f, GetAirAcceleration()));
}

bool UTraceCharacterMovementComponent::IsSourceAirAccelerationEnabled() const
{
	return UTraceSettings::Get().bSourceAirAcceleration;
}

float UTraceCharacterMovementComponent::GetAirAcceleration() const
{
	return FMath::Max(0.f, UTraceSettings::Get().AirAcceleration);
}

float UTraceCharacterMovementComponent::GetAirMaxWishSpeed() const
{
	return FMath::Max(0.f, UTraceSettings::Get().AirMaxWishSpeed);
}

float UTraceCharacterMovementComponent::GetMaxAirSpeed() const
{
	// SPEC v10 §1 — SCALED BY THE KNIFE'S HARD-CAP MULTIPLIER, AND IT HAS TO BE.
	//
	// ApplySourceAirAcceleration takes min(MaxAirSpeed, AirStrafeHardCapSpeed) as its ceiling. The
	// shipped numbers are 1600 and 1375, so MaxAirSpeed is 225 uu/s of headroom and no more; a knife
	// hard cap of 1375 x 1.35 = 1856 under an unraised 1600 would be capped by MaxAirSpeed and the
	// knife's "higher momentum ceiling" would be worth 225 uu/s instead of 481. The knob would look
	// bound, print BOUND in the MOVEKNOB report, and quietly do a third of what it says.
	return FMath::Max(1.f, UTraceSettings::Get().MaxAirSpeed
		* (bKnifeMovementProfile ? GetKnifeAirStrafeHardCapMultiplier() : 1.f));
}

// --- SPEC v10 §1: the knife movement profile ----------------------------------------------------

void UTraceCharacterMovementComponent::SetKnifeMovementProfileActive(const bool bActive)
{
	bKnifeMovementProfile = bActive ? 1 : 0;
}

// SPEC v12 §3 SCALED ALL THREE OF THESE, AND THE ARITHMETIC IS STATED ONCE HERE.
//
// "Reduce max speed with the knife from the previous 30% increase to 22% and adjust momentum
// accordingly." The ground multiplier is the number they named: 1.30 -> 1.22. The two air ceilings
// are the "momentum" half, and the rule applied is that the BONUS — the part above 1.0, which is
// the only part the knife adds — is scaled by 22/30, so the whole mobility package shrinks in
// proportion instead of the ground speed dropping while the ceilings stay at their +30% values.
//
//   ground   1.30      bonus 0.30  -> 0.30 * 22/30 = 0.22      -> 1.22
//   softCap  1.25      bonus 0.25  -> 0.25 * 22/30 = 0.183333  -> 1.183333
//   hardCap  1.35      bonus 0.35  -> 0.35 * 22/30 = 0.256667  -> 1.256667
//
// THESE LITERALS ARE FALLBACKS, NOT THE SHIPPED VALUES. All three bind by name into UTraceSettings
// and Config/DefaultGame.ini overrides them, so the ini keys must move with these or the defaults
// here are decoration. Trace.DumpSettings from a running game is the only honest check.
// At the shipped asymptote the ceilings become soft 1045 -> 1236, hard 1375 -> 1728.

float UTraceCharacterMovementComponent::GetKnifeMoveSpeedMultiplier() const
{
	// "Players should move 22% faster with a knife" (v12 §3, down from v10 §1's 30%). A multiplier
	// over WalkSpeed rather than an absolute, so retuning the walk moves the knife with it: 800 -> 976.
	//
	// Floored at 1.0: a "knife profile" that made the player SLOWER would be a config typo silently
	// inverting the design, and there is no reading of the spec that wants it.
	//
	// PARITY WITH THE CARRIER, restored (spec v13 §3). v12 dropped the knife to 1.22 while
	// CarrierSpeedMultiplier stayed at 1.30 — that was flagged rather than silently changed, because
	// only the knife had been asked for, and it left the carrier faster than the knife. The user then
	// asked for the carrier to "match the new knife speed", so both are now 1.22 (800 -> 976 uu/s)
	// and the parity holds again. Verified live: the grounded Core holder measured 976 uu/s across 54
	// separate throws.
	//
	// If either number moves again, MOVE BOTH or re-flag it. A comment claiming a value the build
	// does not ship is its own defect — this one said 1.30 for a whole pass after it became 1.22.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("KnifeMoveSpeedMultiplier"), 1.22f), 1.f, 3.f);
}

float UTraceCharacterMovementComponent::GetKnifeAirStrafeSoftCapMultiplier() const
{
	// The soft cap is where air-strafe gain STARTS to taper. Raising it by less than the hard cap
	// widens the free band and the falloff band together, which is what "a higher ceiling" means for a
	// mobility weapon: the knife does not just cap out higher, it keeps its full turn value further up
	// the range. 1045 -> 1236 at the shipped asymptote (was 1306 at +30%).
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("KnifeAirStrafeSoftCapMultiplier"), 1.183333f), 1.f, 3.f);
}

float UTraceCharacterMovementComponent::GetKnifeAirStrafeHardCapMultiplier() const
{
	// Where gain reaches zero — the actual momentum ceiling. 1375 -> 1728 at the shipped asymptote
	// (was 1856 at +30%), i.e. the knife can build 353 uu/s more than the gun before the air strafe
	// stops paying. Also applied to MaxAirSpeed; see GetMaxAirSpeed() for why leaving that alone
	// would gut this knob.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("KnifeAirStrafeHardCapMultiplier"), 1.256667f), 1.f, 3.f);
}

bool UTraceCharacterMovementComponent::IsLandingMomentumPreserved() const
{
	return UTraceSettings::Get().bPreserveLandingMomentum;
}

float UTraceCharacterMovementComponent::GetGroundOverspeedFriction() const
{
	return FMath::Max(0.f, UTraceSettings::Get().GroundOverspeedFriction);
}

float UTraceCharacterMovementComponent::GetGroundOverspeedBraking() const
{
	return FMath::Max(0.f, UTraceSettings::Get().GroundOverspeedBraking);
}

float UTraceCharacterMovementComponent::GetGroundOverspeedTurnRate() const
{
	return FMath::Max(0.f, UTraceSettings::Get().GroundOverspeedTurnRate);
}

// --- Ledge tuning (spec v5 §7) ------------------------------------------------------------------
//
// The eight Mantle* accessors used to live here (bMantleEnabled, MantleReachUU, MantleMinHeightUU,
// MantleMaxHeightUU, MantleDurationSeconds, MantleUpPhaseFraction, MantleCooldownSeconds,
// MantleMinForwardSpeed) plus WallJumpMantleLockoutSeconds further down. All nine are deleted in
// spec v12 §5 along with the mechanic they tuned. The matching UPROPERTYs in UTraceSettings and the
// keys in Config/DefaultGame.ini go with them: a knob nothing reads is worse than no knob, because
// a designer will set it and believe something happened.
//
// GetLedgeGroundGraceSeconds() below is NOT one of them. It is fix 2 of the two that actually
// address the Demo 5 ledge rubber-band, it still ships, and it is still read every move.

float UTraceCharacterMovementComponent::GetLedgeGroundGraceSeconds() const
{
	// 0.08s is about five frames at 60Hz — long enough to swallow the one- or two-frame contact blip
	// a capsule takes crossing a lip, far too short to let a pawn slide off a roof and keep sliding.
	// Setting it to 0 restores the Demo 5 behaviour exactly, which is what the desync was measured
	// against.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("LedgeGroundGraceSeconds"), 0.08f), 0.f, 0.5f);
}

void UTraceCharacterMovementComponent::RefreshEngineTunablesFromSettings()
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	const float DesiredWalkSpeed = FMath::Max(1.f, Settings.WalkSpeed);
	if (!FMath::IsNearlyEqual(MaxWalkSpeed, DesiredWalkSpeed))
	{
		MaxWalkSpeed = DesiredWalkSpeed;
		MaxWalkSpeedCrouched = DesiredWalkSpeed * 0.5f;
	}

	// AirControl and the lateral air friction are engine-owned fields consumed deep inside
	// PhysFalling, not at any point this file can intercept, so they are the two air values that
	// have to be COPIED rather than read at the point of use. Copying them once a move is what keeps
	// them live in PIE like everything else.
	//
	// AirControl matters more than it looks. GetFallingLateralAcceleration multiplies Acceleration
	// by it BEFORE CalcVelocity — and therefore before ApplySourceAirAcceleration — ever sees it, so
	// anything below 1 silently scales the wish vector and the documented air model stops being what
	// actually runs. UTraceSettings ships it at 1 and says so.
	const float DesiredAirControl = FMath::Clamp(Settings.AirControl, 0.f, 1.f);
	if (!FMath::IsNearlyEqual(AirControl, DesiredAirControl))
	{
		AirControl = DesiredAirControl;
	}

	const float DesiredAirFriction = FMath::Max(0.f, Settings.AirFriction);
	if (!FMath::IsNearlyEqual(FallingLateralFriction, DesiredAirFriction))
	{
		FallingLateralFriction = DesiredAirFriction;
	}

	// --- SPEC v9 §8: GRAVITY x1.12 ---------------------------------------------------------------
	//
	// Verbatim: "Increase gravity by 12%, to make players feel less floaty when air strafing."
	//
	// GravityScale is an engine-owned field for the same reason AirControl above is: PhysFalling
	// reads it through UCharacterMovementComponent::GetGravityZ() deep inside the fall integration,
	// at no point this file can intercept. So it is COPIED here rather than read at the point of
	// use, and pushed once per simulated move — which keeps it live under Trace.LiveEdit like every
	// other knob, and makes it identical on client, server and every replayed frame (it is a pure
	// function of config, so it needs no saved-move state and cannot rubber-band).
	//
	// AGAINST 1.0, NOT AGAINST ITSELF. Multiplying GravityScale by 1.12 every move would compound to
	// infinity in about two seconds. The authored value is the engine default 1.0 and nothing else
	// in the project writes this field (the mode-B throw arc has its own CoreThrowGravityScale on
	// the Core's projectile, which reads WORLD gravity and is therefore untouched by this).
	const float DesiredGravityScale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("MovementGravityScale"), 1.12f), 0.1f, 4.f);
	if (!FMath::IsNearlyEqual(GravityScale, DesiredGravityScale))
	{
		GravityScale = DesiredGravityScale;
	}

	// --- SPEC v8 §7: BUY THE QUESTION, NOT THE JUMP ----------------------------------------------
	//
	// ACharacter::CheckJumpInput will not call DoJump() at all once JumpCurrentCount has reached
	// JumpMaxCount, and stepping off a ledge or jumping once already spends the only count there is.
	// With the stock JumpMaxCount of 1 the wall jump was unreachable no matter what DoJump did — the
	// engine simply never asked.
	//
	// So the count is raised to 1 + WallJumpMaxConsecutive. That does NOT grant a double jump:
	// DoJump() returns false for every mid-air press that is not a genuine wall jump, and a refused
	// DoJump leaves JumpCurrentCount exactly where it was. What the extra counts buy is the engine
	// asking us the question on each press; TryWallJump() answers it.
	//
	// Prediction-safe: JumpMaxCount is already saved-move state (FSavedMove_Character captures and
	// restores it), both ends derive the same number from the same config, and this runs once per
	// simulated move on every machine, exactly like the two engine fields above.
	if (CharacterOwner != nullptr)
	{
		const int32 DesiredJumpMaxCount = IsWallJumpEnabled() ? (1 + GetWallJumpMaxConsecutive()) : 1;
		if (CharacterOwner->JumpMaxCount != DesiredJumpMaxCount)
		{
			CharacterOwner->JumpMaxCount = DesiredJumpMaxCount;
		}
	}
}

// --- Wall-jump tuning (spec v8 §7) --------------------------------------------------------------
//
// Name-bound against UTraceSettings for the reason the spec v5 knobs are: the UPROPERTYs live in a
// file this slice does not own. Every default below is the shipped value, and BeginPlay's MOVEKNOB
// report says BOUND or FALLBACK for each one so a missing property can never be silent.

bool UTraceCharacterMovementComponent::IsWallJumpEnabled() const
{
	return TraceMoveKnob::Bool(TEXT("bWallJumpEnabled"), true);
}

float UTraceCharacterMovementComponent::GetWallJumpWindowSeconds() const
{
	// "press jump right as they hit a wall". 0.25s is about four frames of slack at 60Hz plus the
	// human reaction floor — long enough to be hittable, short enough that it is a reaction to the
	// contact rather than a state you live in. Clamped so a bad ini cannot make it permanent.
	//
	// SPEC v9 §5: "Make the window of time for performing a wall jump shorter and make the action
	// happen faster." The base number stays where the designer put it (WallJumpWindowSeconds in
	// DefaultGame.ini, which WINS over the fallback here) and the spec's cut is applied as its own
	// named scalar on top, so the two are never confused and the cut is one number to revert.
	//
	// The scalar also shortens how long the auto-mantle defers to a live wall-jump opportunity —
	// see the mantle gate in OnMovementUpdated — so the two halves of §5 move together by
	// construction.
	const float Base = FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpWindowSeconds"), 0.25f), 0.f, 1.f);
	const float Scale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpWindowScale"), 0.6f), 0.05f, 1.f);
	return FMath::Clamp(Base * Scale, 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetWallJumpSpeedRetention() const
{
	// The "carry momentum in a new direction" dial. 0.95 rather than 1.0 so a wall is very slightly
	// lossy — otherwise a corridor is a frictionless pinball table — but nowhere near the reset the
	// request is complaining about. Capped at 1: a wall must never MANUFACTURE speed, which is the
	// same rule spec v4 §1 imposed on the slide.
	//
	// SPEC v9 §5: "Reduce momentum gained from wall jumping by 10%." The spec's [ASSUMPTION] is
	// explicit — "scale the retention knob by 0.9" — so that is what this does, as a separate named
	// scalar rather than by editing the designer's 0.95.
	//
	// THE ALTERNATIVE READING, flagged because the two differ by an order of magnitude. Measured
	// end-to-end retention (launch speed / entry speed) is ~104.6-105.4%, because the outward
	// impulse adds on top of the 0.95. If "the momentum GAINED" means only the part above 100%,
	// then a 10% cut is 1.050 -> 1.045 and is invisible. Scaling the knob is the spec's call and is
	// the change a player will actually feel: 0.95 -> 0.855, which lands measured retention near
	// ~95%. To switch readings, set WallJumpMomentumScale to 1.0 and cut GetWallJumpOutwardImpulse
	// instead — it is the only other term in the launch.
	//
	// SPEC v10 §5, THE SAME SENTENCE AGAIN: "Reduce the momentum boost from wall jumping by 10%."
	// A SECOND named scalar rather than a re-edit of the v9 one, for two reasons that both matter:
	//
	//   1. Config/DefaultGame.ini pins WallJumpMomentumScale=0.9, AND THE INI WINS. Lowering the
	//      default in this file would have changed nothing at all in a running game — which is the
	//      single most common way a change in this project has silently not shipped.
	//   2. The two cuts stay separable. WallJumpMomentumScaleV10 = 1.0 reverts exactly the v10 cut and
	//      leaves v9's, which is what an A/B on feel needs.
	//
	// AND IT IS APPLIED HERE, NOT TO THE OUTWARD IMPULSE. The ini invites the other reading ("set the
	// scale to 1.0 and cut WallJumpOutwardImpulse instead, the only other launch term") and this pass
	// deliberately refuses it: the §5 investigation found the outward impulse is the pawn's escape
	// velocity from the face and that it is ALREADY too weak to beat the player's own held input (11 uu
	// of separation before it is cancelled — see the header). Cutting it would have made the stickiness
	// in the first half of the same sentence measurably worse while satisfying the second half.
	//
	// Net: 0.95 x 0.90 x 0.90 = 0.7695. NOTE FOR THE READING OF THE NUMBERS: at approach speeds at or
	// above the air-strafe hard cap the launch is clamped to max(EntrySpeed, HardCap) anyway, so the
	// cut is invisible on the fastest wall jumps and does its whole job on the mid-speed ones.
	const float Base = FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpSpeedRetention"), 0.95f), 0.f, 1.f);
	const float Scale = IsV9LegacyTuning()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpMomentumScale"), 0.9f), 0.1f, 1.f);
	const float ScaleV10 = IsV10LegacyWallJump()
		? 1.f
		: FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpMomentumScaleV10"), 0.90f), 0.1f, 1.f);
	return FMath::Clamp(Base * Scale * ScaleV10, 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetWallJumpControlLockoutSeconds() const
{
	// SPEC v10 §5, CAUSE 1. See the header for the derivation; the short version is that an air-to-air
	// wall jump launches at ~543 uu/s outward against 8000 uu/s² of air acceleration pointed back at
	// the wall, and peaks 18 uu out — half a capsule radius — before it is being driven back in.
	//
	// 0.20 s is chosen as "long enough that the impulse alone clears the face". At 360 uu/s (v16 §0:
	// 420 -> 360) the pawn travels 72 uu in that time — over two capsule radii, unambiguously off the
	// wall — and the
	// window then ends while the player still has most of their airtime to strafe with. Deliberately
	// SHORTER than the mantle lockout (0.30 s) so the two do not read as one long dead zone, and long
	// enough to cover the 0.15 s wall window so a second contact cannot re-open a launch inside it.
	if (IsV10LegacyWallJump())
	{
		return 0.f;
	}
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpControlLockoutSeconds"), 0.20f), 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetWallJumpInputBufferSeconds() const
{
	// SPEC v10 §5, CAUSE 2 — the press eaten by frame ordering. See the header.
	//
	// 0.12 s is a little under two frames at 15 Hz and about seven at 60, which is the range a human
	// "pressed it just before I hit" actually lands in. It is deliberately SHORTER than the 0.15 s
	// contact window: a buffer longer than the window would let a press made well before the wall
	// survive past the point where the wall itself has stopped counting, and a wall jump the player
	// did not ask for on this contact is a worse bug than the one being fixed.
	if (IsV10LegacyWallJump())
	{
		return 0.f;
	}
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpInputBufferSeconds"), 0.12f), 0.f, 0.5f);
}

// GetWallJumpMantleLockoutSeconds() (spec v9 §5) was defined here. It existed only to stop the
// automatic mantle from re-grabbing the ledge a frame after a wall jump had thrown the pawn off it.
// The mantle is gone (spec v12 §5), so the lockout has nothing to lock out and is deleted rather
// than left returning a value no caller reads.
//
// WHAT THIS MEANS FOR THE WALL JUMP, STATED PLAINLY BECAUSE IT IS THE RISK IN THIS CHANGE: the wall
// jump used to have to WIN A RACE. The mantle needed no input and was attempted on every airborne
// move, so on the frame the capsule met a wall the mantle could claim the pawn before the player's
// press ever reached DoJump — and once it had, IsWallJumpAvailable() was false for the rest of the
// contact. Spec v9 §5 patched that with a priority rule (the mantle yields while a wall window is
// live) and this lockout (the mantle stays off for 0.30 s after a launch). BOTH halves are now
// unnecessary, not merely disabled: there is no second consumer of a wall contact left in the
// component. TryWallJump()'s only gates are its own window, its own consecutive cap and being
// airborne, which is what "the wall jump still fires cleanly at a wall" reduces to.

float UTraceCharacterMovementComponent::GetWallJumpOutwardImpulse() const
{
	// Flat push along the normal, on top of the reflection. Without it a player who slid down a wall
	// with almost no planar speed would wall-jump straight back into the wall on the next frame and
	// the mechanic would read as broken; with it, even a standing wall jump clears the face.
	return FMath::Max(0.f, TraceMoveKnob::Float(TEXT("WallJumpOutwardImpulse"), 360.f));
}

float UTraceCharacterMovementComponent::GetWallJumpVerticalMultiplier() const
{
	// A multiple of JumpZVelocity, like every other vertical launch in the kit (see
	// GetDashExitVerticalSpeedLimit). 1.05 makes a wall jump read as very slightly stronger than a
	// standing jump, which is what sells it as a distinct move.
	return FMath::Max(0.f, TraceMoveKnob::Float(TEXT("WallJumpVerticalMultiplier"), 1.05f));
}

int32 UTraceCharacterMovementComponent::GetWallJumpMaxConsecutive() const
{
	// THE ANTI-LADDER CAP. Two parallel walls three metres apart are an infinite staircase without it.
	// Floored at 1 (a cap of zero would be "the feature is off", which is what bWallJumpEnabled is
	// for) and ceilinged at 4 so that the JumpMaxCount this drives stays sane.
	return FMath::Clamp(TraceMoveKnob::Int(TEXT("WallJumpMaxConsecutive"), 2), 1, 4);
}

float UTraceCharacterMovementComponent::GetWallJumpMaxNormalZ() const
{
	// |Normal.Z| below this is a wall. GetWalkableFloorZ() is 0.71 at the default 45 degrees, so 0.4
	// leaves a clear band between "wall" and "slope you could have walked up" and stops a ramp from
	// being a trampoline.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("WallJumpMaxNormalZ"), 0.4f), 0.f, 0.9f);
}

// --- PATCH 28 §5: surf tuning -------------------------------------------------------------------
//
// Name-bound against UTraceSettings like every knob added since spec v5, for the reason the block at
// the top of this file gives: the UPROPERTYs live in a file this slice does not own, and shipping a
// movement feature with nothing to tune is worse than shipping it name-bound. Every default below is
// the shipped value and BeginPlay's MOVEKNOB report says BOUND or FALLBACK for each one, so a missing
// property can never be silent.

bool UTraceCharacterMovementComponent::IsSurfEnabled() const
{
	return TraceMoveKnob::Bool(TEXT("bSurfEnabled"), true);
}

bool UTraceCharacterMovementComponent::DoesSurfBlockJump() const
{
	// BUTTRESS PASS, OWNER ITEM 7. False restores the pre-pass behaviour, where a press mid-ride took
	// the ordinary air jump. Gated on IsSurfEnabled() as well so that turning surf off cannot leave a
	// jump-refusal rule hanging off a mechanic that no longer exists — the same shape as
	// IsSurfGroundEntryEnabled()'s gate, and for the same reason.
	return IsSurfEnabled() && TraceMoveKnob::Bool(TEXT("bSurfBlocksJump"), true);
}

float UTraceCharacterMovementComponent::GetSurfMinNormalZ() const
{
	// FLOOR OF THE SURF BAND, and it is deliberately ABOVE GetWallJumpMaxNormalZ()'s 0.4 so the two
	// bands cannot overlap. A face is a wall (|Nz| <= 0.4, the wall jump's), a surf plane
	// (0.45 < Nz < WalkableFloorZ) or a floor (Nz >= WalkableFloorZ), and never two of those at once —
	// which matters because the wall jump wants HandleSlopeBoosting left ON (it is what stops a player
	// climbing a corner) and surf wants it off.
	//
	// Clamped strictly below the walkable limit so a bad ini can only ever make the band narrower,
	// never turn the floor into a surf plane.
	const float Requested = TraceMoveKnob::Float(TEXT("SurfMinNormalZ"), 0.45f);
	return FMath::Clamp(Requested, 0.05f, GetWalkableFloorZ() - 0.01f);
}

float UTraceCharacterMovementComponent::GetSurfOverbounce() const
{
	// Source uses 1.0 for the player, which makes ClipVelocity a pure plane projection: the pawn is
	// neither pushed off the ramp nor allowed to sink into it. Above 1 it bounces, below 1 it sinks
	// and the next sweep has to dig it out. Clamped to a band either side of 1 so a typo cannot turn a
	// ramp into a trampoline or into glue.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("SurfOverbounce"), 1.0f), 0.9f, 1.2f);
}

float UTraceCharacterMovementComponent::GetSurfContactGraceSeconds() const
{
	// 0.10 s ~ six frames at 60 Hz. A curved rail is a FAN OF FACETS and a fast pawn genuinely leaves
	// the surface for a frame or two crossing from one facet to the next; without a grace the state
	// would flicker off and on at every joint, and the ceiling that hangs off it would flicker with
	// it. Long enough to bridge a joint, far short of the time it takes to fall clear of a ramp.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("SurfContactGraceSeconds"), 0.10f), 0.f, 0.5f);
}

float UTraceCharacterMovementComponent::GetSurfSpeedCeilingMultiplier() const
{
	// 1.25x the air-strafe hard cap. The surf is meant to be the fastest thing in the kit — that is
	// the whole request — but "fastest" has to be a number and not an asymptote, because gravity along
	// the plane is a constant acceleration the clip never removes and a long enough ramp would grow
	// the vector without limit. See GetSurfSpeedCeiling() for why this is a MULTIPLIER on a value this
	// file already ships rather than a speed of its own.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("SurfSpeedCeilingMultiplier"), 1.25f), 1.f, 3.f);
}

bool UTraceCharacterMovementComponent::IsSurfGroundEntryEnabled() const
{
	// DEMO 29 ITEM 4(b). Off restores "a rail is a thing you bump into"; see HandleImpact.
	return IsSurfEnabled() && !IsSurfLegacyExit()
		&& TraceMoveKnob::Bool(TEXT("bSurfGroundEntryEnabled"), true);
}

float UTraceCharacterMovementComponent::GetSurfGroundEntryMinApproachSpeed() const
{
	// DERIVED FROM THE PAWN'S OWN GROUND LIMIT, not typed, for the DEMO 21 reason every other ceiling
	// in this file is: a fixed 160 uu/s would mean something different the day somebody retunes the
	// walk speed, and the quantity this is really expressing is "a deliberate lean, not a brush".
	//
	// At the shipped 800 uu/s walk limit the default 0.2 is 160 uu/s of INTO-THE-FACE speed, which a
	// pawn at full running speed reaches at 11.5 degrees off parallel. Running along the base of a
	// rail does not trigger it; leaning into the rail does, which is the input the owner is asking to
	// be rewarded.
	const float Fraction = FMath::Clamp(
		TraceMoveKnob::Float(TEXT("SurfGroundEntryApproachFraction"), 0.2f), 0.02f, 1.f);
	return FMath::Max(1.f, GetMaxSpeed()) * Fraction;
}

float UTraceCharacterMovementComponent::GetSurfExitCarrySeconds() const
{
	// DEMO 29 ITEM 4(a), THE "CARRY IT ONTO THE FLAT FLOOR" CLOCK. How long the ground overspeed bleed
	// is held off after a ride ends, and — before the pawn has landed — how long the rollout below
	// stays available.
	//
	// 1.25 s is not a taste call. Measured on the shipped rail, a ride comes off the nose at
	// 1150-1900 uu/s and the bleed (2 x excess + 400 uu/s^2) takes it back to the 800 uu/s walk limit
	// in about 0.75 s and 900 uu — barely five capsule lengths, which is why the owner reads it as
	// losing everything at the end of the curve. 1.25 s of held speed puts the carry at roughly
	// 1400-2400 uu BEFORE the bleed starts, i.e. most of the way across the outer lane, which is what
	// "carry momentum from a curve down onto the flat floor" describes.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("SurfExitCarrySeconds"), 1.25f), 0.f, 5.f);
}

float UTraceCharacterMovementComponent::GetSurfExitCarryBleedScale() const
{
	// What fraction of the ordinary overspeed bleed still applies during the carry. 0 holds the speed
	// outright; 1 is a no-op and is what the legacy arm effectively runs. A scale rather than a flag
	// so a designer can soften the carry without turning it off, and because "hold it completely for
	// 1.25 s then drop off a cliff" is a worse feel than it sounds — the bleed resuming at full
	// strength is a taper, not a cliff, because it is proportional to the EXCESS.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("SurfExitCarryBleedScale"), 0.f), 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetSurfExitRolloutRetention() const
{
	// DEMO 29 ITEM 4(a), THE MISSING HALF OF THE CURVE. 1 rotates the whole of the ride's speed into
	// the horizontal on touchdown, which is what a curve whose tangent reached horizontal would do —
	// the normal force does no work, so |v| is preserved and only its direction changes. 0 is the
	// engine's own behaviour, which deletes the vertical component outright.
	//
	// It is 1 because the geometry CANNOT provide the flattening itself: the surf band's shallow end
	// IS the walkable limit (44.8 degrees on this build), so the shallowest facet a rail is allowed
	// to end on is still 47 degrees and the arc meets the floor at a KINK rather than a tangent.
	// The code is modelling the piece of curve the collision geometry is not allowed to have.
	return FMath::Clamp(TraceMoveKnob::Float(TEXT("SurfExitRolloutRetention"), 1.f), 0.f, 1.f);
}

float UTraceCharacterMovementComponent::GetSurfSpeedCeilingMax() const
{
	// THE FASTEST A SURF CAN BE ON ANY WEAPON PROFILE, which is a different question from
	// GetSurfSpeedCeiling() and has a different consumer: the ARENA asks this one, because a rail's
	// exit lane has to be clear for the fastest pawn that can ever come off it and not merely for the
	// one holding a gun. GetAirStrafeHardCapSpeed() folds in the knife multiplier only while
	// bKnifeMovementProfile is set, and it is never set on the CDO the builder reads, so the knife
	// factor is applied explicitly here — through the same accessor the profile uses, so the two
	// cannot drift.
	//
	// Deliberately NOT floored at SurfEntrySpeed: this is a property of the TUNING, not of a ride in
	// progress, and geometry cannot be re-cut per pawn.
	const float KnifeCap = GetAirStrafeHardCapSpeed()
		* FMath::Max(1.f, bKnifeMovementProfile ? 1.f : GetKnifeAirStrafeHardCapMultiplier());
	return FMath::Max(GetAirStrafeHardCapSpeed(), KnifeCap) * GetSurfSpeedCeilingMultiplier();
}

float UTraceCharacterMovementComponent::GetSurfExitReach(const float LaunchHeight,
	const float WorldGravityZ) const
{
	// HOW FAR PAST THE LIP OF A SURF RAMP A SURFER CAN STILL TRAVEL, given the height they leave it
	// at. Ballistics, with the horizontal speed at this build's absolute surf ceiling:
	//
	//     t = sqrt(2h/g),  reach = v_max * t
	//
	// This exists because ATraceArenaBuilder has to keep a rail's exit lane clear and the number it
	// needs is a MOVEMENT number. Demo 29's exit rig found the shipped rails delivering a surfer at
	// 1469 uu/s into the innermost approach cover 1300 uu past the end of the run — measured
	// 1469 uu/s on the last frame of the ride and 52 uu/s on the floor. A builder that typed "leave
	// 1500 uu clear" would be one air-cap retune away from doing it again with nothing to say so.
	//
	// THE WORLD'S gravity comes from the caller because a CDO has no physics volume to ask and
	// UMovementComponent::GetGravityZ() dereferences one unconditionally. GravityScale is applied
	// HERE rather than by the caller, so the fact that this pawn falls at 1.12g stays a property of
	// the movement component and does not have to be re-known by the arena.
	const float Gravity = FMath::Abs(WorldGravityZ) * FMath::Max(0.01f, GravityScale);
	if (Gravity <= 1.f || LaunchHeight <= 0.f)
	{
		return 0.f;
	}
	return GetSurfSpeedCeilingMax() * FMath::Sqrt(2.f * LaunchHeight / Gravity);
}

float UTraceCharacterMovementComponent::GetSurfSpeedCeiling() const
{
	// DERIVED, NOT TYPED (the project's DEMO 21 rule). The base is GetAirStrafeHardCapSpeed(), which
	// already folds in the knife profile's multiplier, so a knife surfer gets the same 30% the rest of
	// their kit gets and a retune of the air ceiling moves this with it — there is no second literal
	// to forget.
	//
	// FLOORED AT THE ENTRY SPEED, exactly like every other ceiling in this file. A player who arrives
	// on a rail above the cap (out of a dash, off a slide-jump chain) keeps every unit of what they
	// brought; the ceiling only ever refuses to let the RAMP make more.
	return FMath::Max(SurfEntrySpeed, GetAirStrafeHardCapSpeed() * GetSurfSpeedCeilingMultiplier());
}

// --- PATCH 28 §5: the clip, the plane test, and the state ---------------------------------------

FVector UTraceCharacterMovementComponent::ClipVelocityAgainstPlane(const FVector& In, const FVector& Normal,
	const float Overbounce)
{
	// SOURCE'S PM_ClipVelocity, line for line:
	//
	//     backoff = dot(in, normal) * overbounce
	//     out[i]  = in[i] - normal[i] * backoff
	//     if (|out[i]| < STOP_EPSILON) out[i] = 0
	//
	// The epsilon snap is not decoration. Without it the component that should be exactly zero comes
	// back as a few thousandths of a uu/s of residual drift into (or out of) the plane, and a pawn
	// riding a rail for four seconds at 120 Hz integrates that into a visible detach or a visible
	// sink. Source carries the same line for the same reason.
	static constexpr float StopEpsilon = 0.1f;

	FVector Unit = Normal;
	if (!Unit.Normalize())
	{
		return In;
	}

	const float Backoff = static_cast<float>(FVector::DotProduct(In, Unit)) * Overbounce;
	FVector Out = In - Unit * Backoff;

	if (FMath::Abs(Out.X) < StopEpsilon) { Out.X = 0.f; }
	if (FMath::Abs(Out.Y) < StopEpsilon) { Out.Y = 0.f; }
	if (FMath::Abs(Out.Z) < StopEpsilon) { Out.Z = 0.f; }

	return Out;
}

bool UTraceCharacterMovementComponent::IsSurfPlane(const FVector& Normal) const
{
	if (!IsSurfEnabled())
	{
		return false;
	}

	// THE UPPER BOUND IS THE ENGINE'S OWN WALKABLE LIMIT, read live. That is what makes "a player
	// cannot surf ordinary walkable geometry" a proof instead of a hope: the number that decides the
	// pawn is standing on a surface is the same number that refuses to surf it, so the two can never
	// disagree, and a designer who changes the slope limit moves both at once. A copied 0.71 here
	// would be a second opinion about what walkable means — which is exactly the shape of bug this
	// project keeps paying for.
	const float NormalZ = static_cast<float>(Normal.Z);
	return NormalZ > GetSurfMinNormalZ() && NormalZ < GetWalkableFloorZ();
}

void UTraceCharacterMovementComponent::GetSurfSlopeBandDegrees(float& OutMinDegrees, float& OutMaxDegrees) const
{
	// Both ends read the SAME two numbers IsSurfPlane() tests, converted once. A face is surfable when
	// GetSurfMinNormalZ() < Nz < GetWalkableFloorZ(), and Nz = cos(slope), so the shallow end is the
	// walkable limit and the steep end is where the wall band begins. Nothing here is typed.
	OutMinDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(GetWalkableFloorZ(), -1.f, 1.f)));
	OutMaxDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(GetSurfMinNormalZ(), -1.f, 1.f)));
}

bool UTraceCharacterMovementComponent::IsSurfing() const
{
	// The zero-normal test mirrors IsWallJumpAvailable()'s: a cleared normal means there is no surface
	// under the clock, whatever the clock says.
	return IsSurfEnabled()
		&& SurfContactRemaining > 0.f
		&& !SurfPlaneNormal.IsNearlyZero();
}

FVector UTraceCharacterMovementComponent::ComputeSlideVector(const FVector& Delta, const float Time,
	const FVector& Normal, const FHitResult& Hit) const
{
	// FALLING ONLY, and that is not a convenience test. UCharacterMovementComponent::ComputeSlideVector
	// only applies HandleSlopeBoosting() while IsFalling(), so on the ground Super is ALREADY the pure
	// projection and there would be nothing to subtract; taking the branch there would change nothing
	// except which code a walking pawn runs through. PhysWalking also calls this for ledge moves, where
	// the "normal" is a synthetic ledge normal and has no business being classified as terrain.
	if (IsFalling() && IsSurfPlane(Normal))
	{
		// THE HONEST PROJECTION. Super would compute this and then hand it to HandleSlopeBoosting(),
		// whose "we were heading down but were going to deflect upwards, just make the deflection
		// horizontal" branch is every surf contact there is: a pawn falling onto a steep face is
		// deflected up-and-along, and flattening that is what turns a ramp into sandpaper.
		//
		// PhysFalling re-derives Velocity from what this returns, so this line IS the velocity clip.
		return ClipVelocityAgainstPlane(Delta, Normal, GetSurfOverbounce()) * Time;
	}

	return Super::ComputeSlideVector(Delta, Time, Normal, Hit);
}

FVector UTraceCharacterMovementComponent::LimitAirControl(float DeltaTime, const FVector& FallAcceleration,
	const FHitResult& HitResult, bool bCheckForValidLandingSpot)
{
	// See the header note. On a surf plane the acceleration is handed back untouched and the clip a few
	// lines later in PhysFalling removes whatever genuinely pointed into the surface — Source's own
	// order of operations. The engine's rule stays in force for every other surface, walls included.
	//
	// Classified on ImpactNormal, the same field HandleImpact() and CanStepUp() classify on, so all
	// three agree about what a face is. (On a box sweep it equals Hit.Normal, which is what the engine
	// tests; they diverge only on a penetrating start, where the branch below is not taken anyway.)
	//
	// THE LEGACY ARM IS ASKED FOR THROUGH IsSurfLegacyAirLimit(), NOT READ HERE. Both halves of it —
	// the cvar and the -TraceSurfLegacyAirLimit command-line switch — live in that one function, whose
	// Shipping branch is a plain `return false`, so neither half reaches a shipped binary. This used
	// to be a bare FParse::Param on this line with no guard on it, which is how the switch literal
	// ended up in the linked Trace-Mac-Shipping; see GTraceSurfLegacyAirLimit's block at the top of
	// the file for the measurement and the fix.
	if (!IsSurfLegacyAirLimit()
		&& IsFalling() && HitResult.IsValidBlockingHit() && IsSurfPlane(HitResult.ImpactNormal))
	{
		return FallAcceleration;
	}

	return Super::LimitAirControl(DeltaTime, FallAcceleration, HitResult, bCheckForValidLandingSpot);
}

bool UTraceCharacterMovementComponent::CanStepUp(const FHitResult& Hit) const
{
	// =============================================================================================
	// DEMO 29 ITEM 4(b) — THIS FUNCTION USED TO REFUSE A SURF PLANE, AND THAT WAS THE FLYPAPER BUG.
	//
	// Patch 28 refused here, reasoning that StepUp() would reject a surf face anyway and that
	// refusing at the top was one dot product instead of a multi-sweep round trip. The reasoning was
	// right about StepUp() and wrong about what refusing HERE does, because of the shape of the
	// engine's own caller (CharacterMovementComponent.cpp, MoveAlongFloor):
	//
	//     if (CanStepUp(Hit) || <hit is our movement base>)   { StepUp(); if (!stepped) { HandleImpact(); SlideAlongSurface(); } }
	//     else if (Hit.Component.IsValid() && !Hit.Component->CanCharacterStepUp(CharacterOwner))
	//                                                        { HandleImpact(); SlideAlongSurface(); }
	//
	// The rail's collision boxes DO allow CanCharacterStepUp, so a false from here took NEITHER
	// branch: no HandleImpact, no SlideAlongSurface, no movement at all. PhysWalking then re-derives
	// Velocity from the displacement actually achieved — which was zero — so a player who touched a
	// rail while walking was STOPPED DEAD rather than sliding along it. Not slowed: stopped.
	//
	// MEASURED, -TraceSurfApproachTest, running at a rail on the flat floor at the 800 uu/s ground
	// limit: at 90, 60, 45, 30 AND 20 degrees to the rail the planar speed after contact was 0 uu/s,
	// every time, and the surf state was never entered. A 20 degree graze is a lean, not a collision.
	// The four largest structures added in Patch 28 were surfaces that deleted a running player's
	// momentum on touch — which is exactly the owner's "it still doesn't feel like you can surf into
	// curves", from the other side.
	//
	// So the refusal MOVES TO StepUp() below, which MoveAlongFloor calls and whose failure it already
	// handles by falling through to HandleImpact + SlideAlongSurface. The guarantee is unchanged and
	// is still one dot product; what changes is that the engine's own recovery path now runs.
	//
	// The legacy arm puts the refusal back here for the A/B, which is what makes "800 -> 0" and
	// "800 -> a ride" two columns of one table on one binary.
	// =============================================================================================
	if (IsSurfLegacyExit() && Hit.IsValidBlockingHit() && IsSurfPlane(Hit.ImpactNormal))
	{
		return false;
	}

	return Super::CanStepUp(Hit);
}

bool UTraceCharacterMovementComponent::StepUp(const FVector& GravDir, const FVector& Delta,
	const FHitResult& Hit, FStepDownResult* OutStepDownResult)
{
	// NO STAIR-STEPPING ONTO A SURF FACE — the Patch 28 guarantee, moved here from CanStepUp() for
	// the reason written out above. Still one dot product, still taken before any sweep, and now in
	// the one place where a refusal means "the step-up failed" rather than "there is no impact".
	//
	// Super's own down-sweep would reject an unwalkable landing that ends higher than it started
	// (CharacterMovementComponent.cpp: `if (!IsWalkable(Hit))` -> reject if the normal opposes the
	// move or if the new location is above the old one), and a surf plane is unwalkable by
	// construction, so this is belt-and-braces rather than the only guard. It is worth having
	// anyway: on faceted geometry one mis-sized facet is all it takes for a probe to find a walkable
	// ledge and hand a player a staircase up a surf ramp.
	if (!IsSurfLegacyExit() && Hit.IsValidBlockingHit() && IsSurfPlane(Hit.ImpactNormal))
	{
		return false;
	}

	return Super::StepUp(GravDir, Delta, Hit, OutStepDownResult);
}

bool UTraceCharacterMovementComponent::IsSurfExitCarryActive() const
{
	return IsSurfEnabled() && !IsSurfLegacyExit() && SurfExitCarryRemaining > 0.f;
}

void UTraceCharacterMovementComponent::ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations)
{
	// =============================================================================================
	// DEMO 29 ITEM 4(a) — THE ROLLOUT: THE PIECE OF CURVE THE COLLISION GEOMETRY IS NOT ALLOWED TO
	// HAVE.
	//
	// A real curved ramp whose tangent reaches horizontal converts a descent into floor speed: the
	// surface's normal force does no work, so |v| is preserved and only its direction changes. OUR
	// RAMPS CANNOT DO THAT. The surf band's shallow end IS the engine's walkable limit (44.8 degrees
	// on this build), so the shallowest facet a rail is allowed to end on is 47 degrees and the arc
	// meets the floor at a KINK. The engine then does what it does at every landing —
	// MaintainHorizontalGroundVelocity() deletes the vertical component outright — and the ride's
	// descent is thrown away instead of being turned into speed along the floor.
	//
	// This is the owner's "a player should carry momentum from a curve down onto the flat floor",
	// and it is the half of it that no amount of tuning could fix, because the number being deleted
	// is not in any knob.
	//
	// IT CANNOT MANUFACTURE SPEED, and that is the whole of why it is safe. The target is capped by
	// SurfExitSpeed — the 3D speed on the last frame the pawn was actually ON the face — so a long
	// free fall AFTER the ride contributes nothing: fall for a second off the end of a rail and the
	// extra 1000 uu/s of Vz is still discarded exactly as it is today. It is also capped by
	// GetSurfSpeedCeiling(), and floored at the planar speed the landing already produced, so it can
	// only ever ROTATE speed the ride earned into the floor and never add to it.
	//
	// READ BEFORE Super, WRITTEN AFTER: Super::ProcessLanded is what sets the movement mode and calls
	// MaintainHorizontalGroundVelocity(), so the pre-landing vector only exists on this side of it,
	// and the post-landing DIRECTION (which is the engine's, not ours) only exists on the other.
	//
	// PREDICTION: everything read here is either the live velocity or a saved-move field, and
	// ProcessLanded runs inside the movement step on the client, on the server and on every replayed
	// frame. See the header block for the two fields and why both are simulation-critical.
	// =============================================================================================
	const bool bRollout = !IsSurfLegacyExit()
		&& IsSurfEnabled()
		&& SurfExitSpeed > 0.f
		&& (IsSurfing() || SurfExitCarryRemaining > 0.f);

	const float ArrivingSpeed3D = bRollout ? static_cast<float>(Velocity.Size()) : 0.f;
	const float RideSpeedCap = SurfExitSpeed;

	Super::ProcessLanded(Hit, remainingTime, Iterations);

	if (!bRollout || !IsMovingOnGround())
	{
		// Super can leave the pawn falling again (it re-checks the floor). Nothing is spent in that
		// case, so the next landing inside the window still gets the rollout.
		return;
	}

	FVector Planar(Velocity.X, Velocity.Y, 0.f);
	const float PlanarSpeed = static_cast<float>(Planar.Size());
	if (PlanarSpeed > UE_KINDA_SMALL_NUMBER)
	{
		const float Target = FMath::Min3(ArrivingSpeed3D, RideSpeedCap, GetSurfSpeedCeiling());
		const float Rolled = PlanarSpeed
			+ FMath::Max(0.f, Target - PlanarSpeed) * GetSurfExitRolloutRetention();

		Velocity.X = (Planar.X / PlanarSpeed) * Rolled;
		Velocity.Y = (Planar.Y / PlanarSpeed) * Rolled;
		Velocity.Z = 0.f;

#if !UE_BUILD_SHIPPING
		SurfRolloutCount += (Rolled > PlanarSpeed + 1.f) ? 1 : 0;
		SurfRolloutGainSum += FMath::Max(0.f, Rolled - PlanarSpeed);
#endif
	}

	// SPENT. One rollout per ride, so a pawn that bounces, or hops during the carry, cannot roll the
	// same descent into the floor twice. The CARRY clock is (re)armed here rather than at the surf's
	// close so that a long flight off the end of a rail does not eat the floor carry it earned.
	SurfExitSpeed = 0.f;
	SurfExitCarryRemaining = GetSurfExitCarrySeconds();
}

int32 UTraceCharacterMovementComponent::GetMaxDashCharges() const
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	int32 Max = FMath::Max(1, Settings.BaseDashCharges);

	// Contract §5: the Core carrier gets one extra charge for as long as they carry. See the header
	// for why this one read is the kit's only prediction seam.
	bool bCarrying = false;
	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		bCarrying = TraceCharacter->IsCarrier();
	}

#if !UE_BUILD_SHIPPING
	// Dev override so the charge pool can be exercised without a Core.
	extern int32 GTraceMoveKitFakeCarrier;
	if (GTraceMoveKitFakeCarrier != 0 && CharacterOwner != nullptr && CharacterOwner->IsLocallyControlled())
	{
		bCarrying = true;
	}
#endif

	if (bCarrying)
	{
		Max += FMath::Max(0, Settings.CarrierExtraDashCharges);
	}

	// SPEC v19 §3 — LILY'S EXTRA DASH CHARGE, AS AN ADDEND AND NOT AS A TOTAL.
	//
	// "2 normally, 3 while carrying the Core" is exactly +1 on top of the shipped 1 / 2, so it is
	// written as +1 rather than as the numbers 2 and 3. A future retune of BaseDashCharges or
	// CarrierExtraDashCharges therefore still moves her WITH everybody else instead of leaving her
	// pinned at a pair of literals nobody remembers to update.
	//
	// 0 for every other character, so this line is identity for the other nine.
	Max += TraceAbilityTraits::GetExtraDashCharges(CharacterOwner);

	return FMath::Max(1, Max);
}

bool UTraceCharacterMovementComponent::RefundDashCharge()
{
	// SPEC v7 §6. The header documents the three cases; this is them, in order.
	if (CharacterOwner == nullptr || !CharacterOwner->HasAuthority())
	{
		return false;
	}

	const int32 MaxCharges = GetMaxDashCharges();
	if (DashCharges >= MaxCharges)
	{
		// "If both are already available, nothing to do." Not an error, and not worth an RPC.
		return false;
	}

	DashCharges = FMath::Min(DashCharges + 1, MaxCharges);

	// Only a FULL pool clears the clock. A carrier who had spent BOTH charges keeps the timer that
	// was already running for the second — the spec refunds one dash, not both. Leaving the clock
	// alone is also what makes this composable with the refill in TickComponent, which restarts the
	// window only when it finds the pool short with no clock running.
	if (DashCharges >= MaxCharges)
	{
		DashRechargeRemaining = 0.f;
	}

	ClientRefundDashCharge();
	return true;
}

void UTraceCharacterMovementComponent::ClientRefundDashCharge_Implementation()
{
	// The listen host already ran RefundDashCharge on this very component; applying it twice here
	// would hand back two charges for one parry kill.
	if (CharacterOwner == nullptr || CharacterOwner->HasAuthority())
	{
		return;
	}

	const int32 MaxCharges = GetMaxDashCharges();
	if (DashCharges >= MaxCharges)
	{
		return;
	}

	DashCharges = FMath::Min(DashCharges + 1, MaxCharges);
	if (DashCharges >= MaxCharges)
	{
		DashRechargeRemaining = 0.f;
	}
}

// --- Momentum readouts --------------------------------------------------------------------------

float UTraceCharacterMovementComponent::GetPlanarSpeed() const
{
	return FVector(Velocity.X, Velocity.Y, 0.f).Size();
}

bool UTraceCharacterMovementComponent::IsCarryingExcessSpeed() const
{
	return IsMovingOnGround() && GetPlanarSpeed() > (GetMaxSpeed() + TraceMoveCfg::SpeedEpsilon);
}

// =================================================================================================
// D30-RESETS (a) — "momentum should reset when halves switch and when you respawn"
//
// The whole of the owner's rule on the movement side is ResetMomentum() below. See the header for
// why StopMovementImmediately() alone did not implement it (a mid-dash pawn puts the velocity back
// on the next move; the surf / slide / wall-jump clocks are momentum and are not velocity).
// =================================================================================================

#if !UE_BUILD_SHIPPING
/**
 * "Which machine is this?" for the reset's log line. There is no engine helper that does not go
 * through the reflection system, and a UEnum lookup per reset on ten pawns is not worth it — the
 * three names below are the entire set the pawn can be in.
 *
 * Inside the dev guard because every one of its callers is: a Shipping build would carry it as an
 * unused static, which this module compiles with warnings-as-errors.
 */
static const TCHAR* TraceRoleToString(ENetRole Role)
{
	switch (Role)
	{
		case ROLE_Authority:        return TEXT("server");
		case ROLE_AutonomousProxy:  return TEXT("owning client");
		case ROLE_SimulatedProxy:   return TEXT("simulated proxy");
		default:                    return TEXT("no role");
	}
}

/**
 * THE RED ARM FOR (a). 1 restores exactly what ResetPlayersToSpawns did before this pass — the base
 * class's StopMovementImmediately() and nothing else — so the same binary can be shown carrying a
 * dash and a surf ride straight through a half switch.
 *
 * Shipping-guarded on the READER, not only on the console registration. That distinction is the
 * W9-SHIPGUARD bug this file's other legacy arms call out, and it is not being re-introduced here.
 */
int32 GTraceResetsLegacyMomentum = 0;

/**
 * How many times ResetMomentum() has run in this process. Trace.Resets.Arm's hold loop watches it so
 * it can stop re-arming the instant a reset lands, instead of fighting the thing it is trying to
 * observe — the half-time whistle is DEFERRED to the next dead ball (spec v9 §11), so there is no
 * frame a harness could have been told to stop on in advance.
 */
int32 GTraceResetsCount = 0;
#endif

static bool IsResetsLegacyMomentum()
{
#if UE_BUILD_SHIPPING
	return false;
#else
	// The command-line form is the reliable one: an ECVF_Cheat variable set from -ExecCmds does not
	// dependably survive into a session. Same note as IsSurfLegacyExit().
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceResetsLegacyMomentum"));
	return GTraceResetsLegacyMomentum != 0 || bFromCommandLine;
#endif
}

#if !UE_BUILD_SHIPPING
FString UTraceCharacterMovementComponent::DebugDescribeMomentum() const
{
	return FString::Printf(
		TEXT("speed %.0f (v %.0f,%.0f,%.0f) | dash %.2fs dir(%.2f,%.2f,%.2f) charges %d recharge %.2f | ")
		TEXT("slide %.2fs spd %.0f buf %.2f grace %.2f chain %d | ledge %.2f | ")
		TEXT("wall %.2fs n(%.2f,%.2f,%.2f) since %d lock %.2f buf %.2f | ")
		TEXT("surf %.2fs entry %.0f peak %.0f | exit carry %.2fs spd %.0f"),
		GetPlanarSpeed(), Velocity.X, Velocity.Y, Velocity.Z,
		DashTimeRemaining, DashDirection.X, DashDirection.Y, DashDirection.Z, DashCharges, DashRechargeRemaining,
		SlideTimeRemaining, SlideSpeed, SlideBufferRemaining, SlideJumpGraceRemaining, SlideJumpChainBoosts,
		GroundGraceRemaining,
		WallJumpWindowRemaining, WallJumpNormal.X, WallJumpNormal.Y, WallJumpNormal.Z,
		WallJumpsSinceGround, WallJumpControlLockoutRemaining, WallJumpInputBufferRemaining,
		SurfContactRemaining, SurfEntrySpeed, SurfPeakSpeed,
		SurfExitCarryRemaining, SurfExitSpeed);
}

void UTraceCharacterMovementComponent::DebugArmMomentum(float PlanarSpeed, bool bLog)
{
	const float Speed = FMath::Max(1.f, PlanarSpeed);

	FVector Forward = UpdatedComponent != nullptr ? UpdatedComponent->GetForwardVector() : FVector::ForwardVector;
	Forward.Z = 0.f;
	if (!Forward.Normalize())
	{
		Forward = FVector::ForwardVector;
	}

	Velocity = Forward * Speed;

	// A live dash, which is the field that makes StopMovementImmediately() alone useless: with this
	// set, OnMovementUpdated calls ApplyDashVelocity() and puts the velocity back next move.
	DashTimeRemaining = 5.f;
	DashDirection = Forward;
	DashCharges = 0;
	DashRechargeRemaining = 3.f;

	// A live slide. SlideSpeed is folded into GetMaxSpeed() while it is non-zero.
	SlideTimeRemaining = 2.f;
	SlideSpeed = Speed;
	SlideDirection = Forward;
	SlideBufferRemaining = 0.5f;
	SlideJumpGraceRemaining = 0.3f;
	bSlideJumpGraceWellTimed = 1;
	SlideJumpChainBoosts = 2;
	SlideJumpChainCeiling = Speed;

	GroundGraceRemaining = 0.25f;

	// An open wall-jump window, and a consecutive count already part-spent.
	WallJumpNormal = -Forward;
	WallJumpWindowRemaining = 0.4f;
	WallJumpEntryVelocity = Velocity;
	WallJumpsSinceGround = 2;
	WallJumpLaunchNormal = -Forward;
	WallJumpControlLockoutRemaining = 0.3f;
	WallJumpInputBufferRemaining = 0.2f;

	// A live surf ride and the exit carry that outlives it. SurfExitCarryRemaining is the one that
	// can disagree with the server while the pawn is on its FEET — see the saved-move block.
	SurfPlaneNormal = FVector(0.f, 0.6f, 0.8f).GetSafeNormal();
	SurfContactRemaining = 0.3f;
	SurfEntrySpeed = Speed;
	SurfElapsedSeconds = 2.f;
	SurfPeakSpeed = Speed;
	SurfExitCarryRemaining = 0.6f;
	SurfExitSpeed = Speed;

	if (bLog)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[Resets] ARMED %s (%s): %s"),
			*GetNameSafe(CharacterOwner),
			CharacterOwner != nullptr ? TraceRoleToString(CharacterOwner->GetLocalRole()) : TEXT("no pawn"),
			*DebugDescribeMomentum());
	}
}
#endif // !UE_BUILD_SHIPPING

void UTraceCharacterMovementComponent::ResetMomentum(const TCHAR* Why)
{
	const TCHAR* const Reason = (Why != nullptr) ? Why : TEXT("unspecified");

#if !UE_BUILD_SHIPPING
	// Captured BEFORE anything is written, so the log line below is a before/after of the same frame
	// rather than two readings of the same zero — which is what makes the legacy arm legible.
	const FString Before = DebugDescribeMomentum();
#endif

	// The base class's half: Velocity, any root motion, and the pending launch/impulse/force that a
	// LaunchCharacter or an ability queued for the next move. ClearAccumulatedForces() is separate
	// because StopMovementImmediately() does not do it, and a queued PendingLaunchVelocity would be
	// applied on the first move after the reset — a reset the player would see arrive one frame late.
	StopMovementImmediately();
	ClearAccumulatedForces();

	if (!IsResetsLegacyMomentum())
	{
		// --- The kit, field for field, to the values the constructor gives a fresh pawn ------------
		//
		// Dash. Charges are refilled rather than zeroed: BeginPlay() starts a pawn full, so "full" is
		// what "indistinguishable from a fresh pawn" means, and starting a half with no dash would be
		// a punishment nobody asked for.
		DashTimeRemaining = 0.f;
		DashDirection = FVector::ZeroVector;
		LastMaxDashCharges = GetMaxDashCharges();
		DashCharges = LastMaxDashCharges;
		DashRechargeRemaining = 0.f;

		// Slide, including the slide-jump coyote window and the spec v26 §3 chain. Written directly
		// rather than through EndSlide(): that function exists to HAND THE MOMENTUM BACK (it sets
		// Velocity from SlideSpeed × the exit retention), which is the exact opposite of a reset.
		SlideTimeRemaining = 0.f;
		SlideCooldownRemaining = 0.f;
		SlideSpeed = 0.f;
		SlideDirection = FVector::ZeroVector;
		SlideBufferRemaining = 0.f;
		SlideJumpGraceRemaining = 0.f;
		bSlideJumpGraceWellTimed = 0;
		SlideJumpChainBoosts = 0;
		SlideJumpChainCeiling = 0.f;
		bSlideHeldLastMove = 0;
		bWasAirborneLastMove = 0;

		// Ledge grace (spec v5 §7).
		GroundGraceRemaining = 0.f;

		// Wall jump (spec v8 §7) and the spec v10 §5 lockout/buffer pair.
		WallJumpNormal = FVector::ZeroVector;
		WallJumpWindowRemaining = 0.f;
		WallJumpEntryVelocity = FVector::ZeroVector;
		WallJumpsSinceGround = 0;
		WallJumpLaunchNormal = FVector::ZeroVector;
		WallJumpControlLockoutRemaining = 0.f;
		WallJumpInputBufferRemaining = 0.f;

		// Surf (Patch 28 §5) and its exit carry (Demo 29 item 4).
		SurfPlaneNormal = FVector::ZeroVector;
		SurfContactRemaining = 0.f;
		SurfEntrySpeed = 0.f;
		SurfElapsedSeconds = 0.f;
		SurfPeakSpeed = 0.f;
		SurfExitCarryRemaining = 0.f;
		SurfExitSpeed = 0.f;

		// The one-shot intent only. bWantsToSlide / bWantsToCrouch / the jump-held level are re-sent
		// by the owning client on every move (UpdateFromCompressedFlags), so forcing them false here
		// would be overwritten within a frame on the server and would fight the player's own fingers
		// on the client. bWantsToDash is different: it is consumed at the end of a move, so one that
		// was raised and not yet spent would fire a dash out of the reset.
		bWantsToDash = 0;

		// bKnifeMovementProfile is deliberately untouched — see the header. It is which weapon is in
		// the pawn's hands, not momentum.
	}

	// --- The client's own prediction, which is the half a server-side zero cannot reach -----------
	//
	// On the autonomous proxy the moves the server has not acknowledged yet are still in the buffer,
	// and the next correction REPLAYS them: FSavedMove_Trace::PrepMoveFor pushes each move's captured
	// dash / slide / wall / surf state back into this component before re-simulating it, so a reset
	// that left them in place would be undone by the very next ClientAdjustPosition. Dropping them is
	// the same operation the engine performs when the buffer overflows (FNetworkPredictionData_Client_
	// Character::CreateSavedMove), so it is a supported state to be in: the client simply accepts the
	// server's next position wholesale instead of replaying on top of it.
	if (CharacterOwner != nullptr && CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy)
	{
		if (FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character())
		{
			for (const FSavedMovePtr& Move : ClientData->SavedMoves)
			{
				ClientData->FreeMove(Move);
			}
			ClientData->SavedMoves.Reset();

			// Detached into a local BEFORE it is freed. FreeMove() takes its argument by const
			// reference and nulls PendingMove inside itself, so handing it ClientData->PendingMove
			// directly makes its own parameter alias a pointer it has just cleared. Harmless today
			// and exactly the shape of a bug that stops being harmless after an engine upgrade.
			if (ClientData->PendingMove.IsValid())
			{
				const FSavedMovePtr Pending = ClientData->PendingMove;
				ClientData->PendingMove = nullptr;
				ClientData->FreeMove(Pending);
			}
		}
	}

#if !UE_BUILD_SHIPPING
	++GTraceResetsCount;

	UE_LOG(LogTraceGame, Log, TEXT("[Resets] %s (%s) %s | legacy=%d\n           before: %s\n           after:  %s"),
		*GetNameSafe(CharacterOwner),
		CharacterOwner != nullptr ? TraceRoleToString(CharacterOwner->GetLocalRole()) : TEXT("no pawn"),
		Reason, IsResetsLegacyMomentum() ? 1 : 0, *Before, *DebugDescribeMomentum());
#else
	UE_LOG(LogTraceGame, Verbose, TEXT("[Resets] %s %s"), *GetNameSafe(CharacterOwner), Reason);
#endif
}

// --- Prediction pipeline ---------------------------------------------------------------------

FNetworkPredictionData_Client* UTraceCharacterMovementComponent::GetPredictionData_Client() const
{
	// Deliberately no check(PawnOwner) (the shape most tutorials copy): the engine only ever asks
	// for prediction data on a pawn it is about to simulate, and asserting on a pointer a
	// disconnect can null out is exactly what contract §10 forbids. The base FNetworkPredictionData
	// constructor does not touch PawnOwner, so allocating unconditionally is safe.
	if (ClientPredictionData == nullptr)
	{
		UTraceCharacterMovementComponent* MutableThis = const_cast<UTraceCharacterMovementComponent*>(this);
		MutableThis->ClientPredictionData = new FNetworkPredictionData_Client_Trace(*this);
		MutableThis->ClientPredictionData->MaxSmoothNetUpdateDist = 92.f;
		MutableThis->ClientPredictionData->NoSmoothNetUpdateDist = 140.f;
	}

	return ClientPredictionData;
}

void UTraceCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);

	// Pure state restore. The actual activations happen in OnMovementUpdated, on every machine,
	// from these flags — so server, client and replay all go through one code path.
	bWantsToDash  = ((Flags & FSavedMove_Character::FLAG_Custom_0) != 0) ? 1 : 0;
	bWantsToSlide = ((Flags & FSavedMove_Character::FLAG_Custom_2) != 0) ? 1 : 0;

	// DEMO 19 ITEM 4. FLAG_Custom_1 was boost's free bit; it now carries the jump-held LEVEL, which is
	// how a remote client's RELEASE reaches the server at all — UTraceAbilityComponent's release hook
	// has no caller and there is no release RPC. Through the setter, not the raw bit, so the freshness
	// stamp is refreshed by every move that still says "down" and a move that says "up" clears it
	// immediately rather than waiting for the watchdog.
	SetJumpHeld((Flags & FSavedMove_Character::FLAG_Custom_1) != 0);
}

// =================================================================================================
// THE MOMENTUM MODEL
// =================================================================================================

void UTraceCharacterMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	// Mirror the base class's own bail-outs before deciding anything. A simulated proxy is fed its
	// velocity by replication and must not run either branch, or it would fight the interpolation.
	const bool bBaseWouldBailOut =
		!HasValidData()
		|| HasAnimRootMotion()
		|| DeltaTime < 1.e-6f
		|| (CharacterOwner != nullptr && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy && !bWasSimulatingRootMotion)
		|| CurrentRootMotion.HasOverrideVelocity();

	if (!bBaseWouldBailOut)
	{
		// The mantle's unconditional "the pull-up owns Velocity" branch was the first thing in this
		// block (spec v5 §7). Removed in v12 §5. Nothing replaces it: with the mantle gone, a pawn at
		// a ledge is either falling or walking, and the two branches below are the whole story again.

		// --- AIR (spec §2.1) ---------------------------------------------------------------------
		//
		// Not while dashing: the dash owns the velocity vector outright for its whole window and
		// re-asserts it in OnMovementUpdated, so letting air input add to it first would just be
		// arithmetic nobody can observe.
		if (IsFalling() && IsSourceAirAccelerationEnabled() && DashTimeRemaining <= 0.f)
		{
			ApplySourceAirAcceleration(DeltaTime);
			return;
		}

		// --- CARRIED GROUND MOMENTUM (spec §2.2, §2.4) --------------------------------------------
		//
		// This is the branch that "removes the clamp of horizontal velocity to ground max speed on
		// landing". Super::CalcVelocity, the instant IsExceedingMaxSpeed() is true, runs
		// ApplyVelocityBraking with GroundFriction × BrakingFrictionFactor (8 × 2) plus
		// BrakingDecelerationWalking (2600) — which at 1900uu/s is about -33000uu/s², i.e. the whole
		// carry is gone inside four frames. Taking the branch ourselves is the only way to defeat it
		// without lowering GroundFriction, which would also make ordinary walking feel like ice.
		//
		// Not while sliding or dashing: those states set GetMaxSpeed() to their own speed, so they
		// are never "overspeed" by this definition and never reach here anyway — but the explicit
		// test documents that they own Velocity and this does not.
		if (IsMovingOnGround() && IsLandingMomentumPreserved()
			&& DashTimeRemaining <= 0.f && SlideTimeRemaining <= 0.f
			&& GetPlanarSpeed() > (FMath::Max(1.f, GetMaxSpeed()) + TraceMoveCfg::SpeedEpsilon))
		{
			ApplyGroundOverspeedBleed(DeltaTime);
			return;
		}
	}

	Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
}

void UTraceCharacterMovementComponent::ApplySourceAirAcceleration(float DeltaTime)
{
	// PhysFalling has already stripped Velocity.Z for the duration of this call and restores it
	// afterwards, so everything here is honestly planar. Never write Z.
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);

	// Acceleration here is FallAcceleration: planar, scaled by AirControl (which
	// RefreshEngineTunablesFromSettings pins at 1.0 for exactly this reason) and clamped to
	// GetMaxAcceleration(). Its LENGTH is the analog input magnitude; its direction is the wish
	// direction. Using Acceleration rather than GetLastInputVector() is what makes this replay-safe:
	// MoveAutonomous restores Acceleration from the saved move on every corrected frame.
	FVector WishDirection(Acceleration.X, Acceleration.Y, 0.f);
	const float WishMagnitude = WishDirection.Size();

	// NO INPUT MEANS NO CHANGE. Not "decay toward zero" — Source has no air friction and neither do
	// we, and a decay here would make every jump cost speed, which is precisely the complaint.
	if (WishMagnitude <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	WishDirection /= WishMagnitude;

	// =============================================================================================
	// SPEC v10 §5, CAUSE 1 — THE PLAYER MAY NOT PULL THEMSELVES BACK ONTO THE WALL THEY JUST LEFT.
	//
	// THIS IS THE STICKINESS. Not the window, not the mantle — those were v9, and the complaint came
	// back unchanged. A player who wall-jumps got to the wall by holding the stick INTO it, and they
	// are still holding it on the frame after the launch. The arithmetic is one-sided and brutal:
	// peak separation is v_out²/2a with a = 8000 uu/s², and AirMaxWishSpeed (160) caps how fast a
	// pawn can return to a wall under air control — so every wall jump taken FROM THE AIR has almost
	// nothing to reflect, launches at roughly the flat 360 impulse alone, and peaks at ~15 uu against
	// a 34 uu capsule radius. It is glued to the face by its own input, and no launch value could
	// have outrun it: doubling the impulse buys 4x the separation and still loses to 8000 uu/s².
	//
	// SO REMOVE ONLY THE OFFENDING COMPONENT, AND ONLY BRIEFLY. The wish direction is projected onto
	// the wall plane for GetWallJumpControlLockoutSeconds(); tangential and outward input keep their
	// full magnitude and their full allowance, so the Source strafe is entirely intact — the player
	// can still carve the launch anywhere in the half-space away from the wall, at the same rate as
	// always. Only "accelerate back into the face I just launched off" is refused, and only until the
	// pawn is clear of it.
	//
	// NOTHING IS SUBTRACTED FROM VELOCITY, which is this function's founding rule: the projection
	// touches the INPUT direction before the formula runs, never the velocity vector. A lockout that
	// braked the pawn would be the "air control feels like a brake" mistake the comment below warns
	// about, wearing a different hat.
	//
	// Predicted: WallJumpControlLockoutRemaining and WallJumpLaunchNormal are saved-move state, and
	// Acceleration is restored by MoveAutonomous, so a replayed frame lands on the identical vector.
	// =============================================================================================
	if (WallJumpControlLockoutRemaining > 0.f && !WallJumpLaunchNormal.IsNearlyZero())
	{
		const float IntoWall = -static_cast<float>(FVector::DotProduct(WishDirection, WallJumpLaunchNormal));
		if (IntoWall > 0.f)
		{
			FVector Allowed = WishDirection + WallJumpLaunchNormal * IntoWall;
			if (!Allowed.Normalize())
			{
				// Input aimed dead-on at the face. There is no tangential component to keep, so the
				// honest answer is "no acceleration this frame" — not a redirect to some direction the
				// player did not ask for.
				//
				// SPEC v18 §1a DELIBERATELY DOES NOT REACH HERE. Holding the stick straight into the
				// face you just launched off is, by definition, opposed to the launch — so letting the
				// new brake run would let the player cancel their own wall jump with the exact input
				// v10 §5 CAUSE 1 exists to neutralise, and the stickiness would come straight back.
				return;
			}
			WishDirection = Allowed;
		}
	}

	const float InputScale = FMath::Clamp(WishMagnitude / FMath::Max(1.f, GetMaxAcceleration()), 0.f, 1.f);

	// EVERYTHING THAT NEEDS MEMBER STATE IS NOW DONE. The rest of the model is a pure function of
	// (planar velocity, wish direction, input scale, dt, config) — which is what lets the shipped
	// arithmetic be driven directly by Trace.Move.V18.AirReverse instead of re-typed into a harness.
	const FVector NewPlanar = ComputeAirStrafeStep(PlanarVelocity, WishDirection, InputScale, DeltaTime);

	Velocity.X = NewPlanar.X;
	Velocity.Y = NewPlanar.Y;
}

FVector UTraceCharacterMovementComponent::ComputeAirStrafeStep(
	const FVector& InPlanarVelocity, const FVector& InWishDirection,
	const float InInputScale, const float InDeltaTime) const
{
	const FVector PlanarVelocity(InPlanarVelocity.X, InPlanarVelocity.Y, 0.f);
	const float SpeedBefore = static_cast<float>(PlanarVelocity.Size());

	FVector WishDirection(InWishDirection.X, InWishDirection.Y, 0.f);

	// NO INPUT MEANS NO CHANGE. Not "decay toward zero" — Source has no air friction and neither do
	// we, and a decay here would make every jump cost speed, which is precisely the complaint.
	// A zero dt is the same answer for a different reason: nothing has happened yet.
	if (!WishDirection.Normalize() || InDeltaTime <= 0.f)
	{
		return PlanarVelocity;
	}

	const float InputScale = FMath::Clamp(InInputScale, 0.f, 1.f);

	// THE FORMULA (Quake's PM_AirAccelerate, Source's CAirAccelerate, same maths):
	//
	//   1. WishSpeed is what the player is asking for, clamped to AirMaxWishSpeed. That clamp is the
	//      entire mechanic. In Quake it is 30 units/s; making it a knob is what "expose the accel cap
	//      and max air speed" means.
	//   2. Project the CURRENT velocity onto the wish direction. If the player is already travelling
	//      that fast in that direction, there is nothing to add.
	//   3. Add at most AirAcceleration·dt along the wish direction — and only ever ADD.
	//
	// Step 3 is why perpendicular input turns you for free: with the input at 90° to travel, the
	// projection is 0, so the full allowance is available, and it is applied SIDEWAYS. The resulting
	// vector is sqrt(v² + a²) long — very slightly FASTER, and rotated. Nothing anywhere subtracts a
	// component of velocity, so strafing can never cost speed. A lerp toward the input direction,
	// which is the usual mistake, subtracts on every frame and is why "air control" feels like a
	// brake.
	//
	// InputScale is the one addition to the formula as UTraceSettings documents it, and it is a
	// no-op for a keyboard: it only scales the target for a partially deflected analog stick, where
	// asking for half speed and getting the full turn allowance would be wrong.
	const float WishSpeed = FMath::Min(GetMaxAirSpeed(), GetAirMaxWishSpeed()) * InputScale;
	const float SpeedAlongWish = FVector::DotProduct(PlanarVelocity, WishDirection);
	const float AddSpeed = WishSpeed - SpeedAlongWish;

	if (AddSpeed <= 0.f)
	{
		// Already travelling at or above the wish speed ALONG the wish direction, so there is nothing
		// to add. There is nothing to BRAKE either, and that is provable rather than assumed:
		// AddSpeed <= 0 means SpeedAlongWish >= WishSpeed >= 0, i.e. dot(wish, velocity) >= 0, i.e. the
		// input is not opposed at all. So returning here cannot skip the v18 §1a brake below — it can
		// only skip a brake that is arithmetically zero.
		return PlanarVelocity;
	}

	const float AccelSpeed = FMath::Min(GetAirAcceleration() * InDeltaTime, AddSpeed);
	FVector NewPlanar = PlanarVelocity + WishDirection * AccelSpeed;

	const float NewSpeed = static_cast<float>(NewPlanar.Size());
	if (NewSpeed <= UE_KINDA_SMALL_NUMBER)
	{
		return NewPlanar;
	}

	// =============================================================================================
	// SPEC v5 §1 — THE ACCUMULATION CEILING. EVERYTHING ABOVE THIS LINE IS UNCHANGED.
	//
	// "The air strafing feels incredible, but its too powerful with how much momentum can be gained."
	//
	// NewPlanar is already the fully turned vector. The only thing left to decide is HOW LONG it is,
	// and the limiters below touch nothing else — which is the whole reason the turn survives: the
	// direction computed by the projection formula is preserved exactly, and a player at the hard cap
	// can still carve their velocity round at constant speed indefinitely, for free, forever.
	// What they cannot do is make it any longer.
	// =============================================================================================

	// THE GAIN, AND THE Max(0) THAT USED TO BE THE BUG.
	//
	// SPEC v18 §1a: "if pressing A and jumping then letting go of A and pressing D, your momentum
	// doesn't change at all. We want it so doing so slows down your momentum."
	//
	// The comment that used to sit here said the projection formula "can only ever ADD along the wish
	// direction, so this is >= 0 by construction; the Max is belt and braces against float noise".
	// THAT WAS WRONG, and it was wrong in the one case the user is complaining about. The formula only
	// ever adds ALONG THE WISH DIRECTION — when the wish direction OPPOSES travel, adding along it
	// SHORTENS the vector, and NewSpeed is genuinely less than SpeedBefore. Worked at the shipped
	// numbers, a dead reversal at 800 uu/s and a 16 ms frame gives NewPlanar 672 uu/s...
	//
	//   ...and then Max(0, 672 - 800) = 0, TargetSpeed = SpeedBefore = 800, and the last line rescales
	//   the 672 vector straight back up to 800. The player's momentum is restored to the last float
	//   bit, every frame, forever. "Your momentum doesn't change at all" is not an exaggeration; it is
	//   arithmetically exact, and this is the line that does it.
	//
	// The Max(0) STAYS, because the frame-by-frame loss the raw formula produces is AirAcceleration
	// (8000 uu/s²) pointed backwards — a dead stop from 800 uu/s in 0.1 s, which is exactly the "feels
	// like hitting a wall" the spec rules out. Instead the loss is re-expressed as its own named,
	// tunable term below, so there is one number a designer can turn instead of a side effect of the
	// acceleration constant. Keeping the clamp also means the GAIN path is byte-for-byte what it was.
	const float RawGain = FMath::Max(0.f, NewSpeed - SpeedBefore);

	// DIMINISHING RETURNS. Sampled at the speed the frame STARTED at, not at NewSpeed: sampling the
	// output would make the scale depend on the frame length, and two 8ms frames would then not equal
	// one 16ms frame — which is precisely the non-linearity CanCombineWith exists to protect the
	// prediction from, and there is no reason to add another one.
	const float GainScale = GetAirStrafeGainScale(SpeedBefore);
	float TargetSpeed = SpeedBefore + RawGain * GainScale;

	// =============================================================================================
	// SPEC v18 §1a — THE OPPOSITION BRAKE, AND WHY IT IS A RAMP AND NOT A SWITCH.
	//
	// Opposition is the NEGATIVE PART of dot(wish, travel), i.e. 0 for anything from dead-ahead round
	// to a dead-square 90° strafe, rising smoothly to 1 at a full 180° reversal. Multiplying the
	// deceleration by it is what makes "a slight counter-steer barely bites and a full reversal bites
	// properly" true by construction rather than by tuning.
	//
	// THIS IS ALSO THE PROOF THAT THE AIR STRAFE IS UNTOUCHED, and it is worth stating as arithmetic
	// rather than as an intention: the Source strafe is an input at (or inside) 90° to travel, where
	// the dot product is >= 0 and Opposition is therefore EXACTLY ZERO — not small, zero. Every frame
	// of every gaining strafe takes Brake = 0 and lands on the identical float it landed on before
	// this block existed. Trace.Move.V18.AirReverse checks that as an equality, not as a tolerance.
	//
	// Scaled by InputScale for the same reason the gain is: a half-deflected stick asking for half the
	// turn must not get the whole brake.
	//
	// Applied BEFORE the ceilings below, so a player braking from above the hard cap keeps braking
	// (the ceiling is floored at SpeedBefore and would otherwise be a no-op on the way down).
	// =============================================================================================
	const float OpposingDeceleration = GetAirStrafeOpposingDeceleration();
	if (OpposingDeceleration > 0.f && SpeedBefore > UE_KINDA_SMALL_NUMBER)
	{
		const float Opposition = FMath::Max(0.f, -static_cast<float>(
			FVector::DotProduct(WishDirection, PlanarVelocity / SpeedBefore)));
		if (Opposition > 0.f)
		{
			// Floored at zero rather than allowed to go negative: the brake's job is to kill momentum,
			// and pushing the pawn backwards is the acceleration term's job. Letting this go negative
			// would flip the vector's direction on the rescale below, which reads as a snap.
			TargetSpeed = FMath::Max(0.f,
				TargetSpeed - OpposingDeceleration * Opposition * InputScale * InDeltaTime);
		}
	}

	// THE BACKSTOPS. Both are floored at SpeedBefore so that a ceiling can only ever remove speed
	// THIS CALL just added, and can never brake momentum carried into the air — a slide-jump that
	// leaves the ground above the cap keeps every unit of it, exactly as MaxAirSpeed always did.
	float SpeedCeiling = FMath::Max(GetMaxAirSpeed(), SpeedBefore);
	if (IsAirStrafeHardCapEnabled())
	{
		SpeedCeiling = FMath::Min(SpeedCeiling, FMath::Max(GetAirStrafeHardCapSpeed(), SpeedBefore));
	}
	TargetSpeed = FMath::Min(TargetSpeed, SpeedCeiling);

	// Rescale, KEEPING THE DIRECTION. This one line is the difference between "limit how much speed
	// can accumulate" and "undo the Source feel".
	return NewPlanar * (TargetSpeed / NewSpeed);
}

void UTraceCharacterMovementComponent::ApplyGroundOverspeedBleed(float DeltaTime)
{
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
	const float CurrentSpeed = PlanarVelocity.Size();
	if (CurrentSpeed <= TraceMoveCfg::SpeedEpsilon)
	{
		return;
	}

	const float GroundSpeedLimit = FMath::Max(1.f, GetMaxSpeed());
	FVector TravelDirection = PlanarVelocity / CurrentSpeed;

	// STEERING. Carried momentum you cannot aim is a punishment, not a reward, so the player may
	// rotate it — at GroundOverspeedTurnRate, and WITHOUT the rotation costing any speed, which is
	// the same rule the air model follows. Falls back to the AI's requested velocity so that a bot
	// coming out of a dash still corners instead of ballistically overshooting: path following sets
	// RequestedVelocity but leaves Acceleration at zero.
	FVector WishDirection(Acceleration.X, Acceleration.Y, 0.f);
	if (WishDirection.SizeSquared() <= UE_KINDA_SMALL_NUMBER && bHasRequestedVelocity)
	{
		WishDirection = FVector(RequestedVelocity.X, RequestedVelocity.Y, 0.f);
	}
	if (WishDirection.Normalize())
	{
		// A fixed ANGULAR rate (degrees/second), like the slide's steering and for the same reason:
		// the result depends only on (TravelDirection, WishDirection, rate x dt), all of which the
		// replay path reproduces exactly, and rotating by a fixed angle cannot change the magnitude.
		TravelDirection = TraceMovement::SteerTowards(
			TravelDirection, WishDirection, GetGroundOverspeedTurnRate() * DeltaTime);
	}

	// BLEED THE EXCESS, NOT THE SPEED. Friction proportional to the excess makes the carry taper
	// (fast at first, gentle as it approaches a run) and the flat braking term guarantees it
	// actually terminates rather than approaching the limit asymptotically forever.
	//
	// Floored at the ground limit, never below: once the excess is gone this branch stops being
	// taken and Super::CalcVelocity resumes on exactly the speed normal movement would allow, so
	// there is no discontinuity at the handover.
	//
	// DEMO 29 ITEM 4(a) SCALES IT FOR A SHORT WINDOW AFTER A SURF, AND THAT IS THE OWNER'S "carry
	// momentum from a curve down onto the flat floor". Measured on the shipped rail before the change:
	// a ride arriving on the floor at 1140 uu/s was back at the 800 uu/s walk limit 0.75 s and 912 uu
	// later — under six capsule diameters, which reads as losing it all at the bottom of the ramp. The
	// scale is 0 by default (the speed is HELD, not merely bled more slowly) for
	// GetSurfExitCarrySeconds(), after which the ordinary bleed resumes — and resumes as a taper, not
	// a cliff, because it is proportional to the excess.
	//
	// It is a scale on the BLEED and not a second speed model: steering, the floor at the ground
	// limit, and the handover back to Super::CalcVelocity are all unchanged, so nothing about the end
	// of the carry is new code.
	const float ExcessSpeed = CurrentSpeed - GroundSpeedLimit;
	const float BleedScale = IsSurfExitCarryActive() ? GetSurfExitCarryBleedScale() : 1.f;
	const float Bleed =
		(GetGroundOverspeedFriction() * ExcessSpeed + GetGroundOverspeedBraking()) * BleedScale * DeltaTime;
	const float NewSpeed = FMath::Max(GroundSpeedLimit, CurrentSpeed - Bleed);

	Velocity.X = TravelDirection.X * NewSpeed;
	Velocity.Y = TravelDirection.Y * NewSpeed;
	// Z is already zero here: PhysWalking calls MaintainHorizontalGroundVelocity() before this.
}

// --- Dash ------------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::StartDash()
{
	if (!CanDash())
	{
		return;
	}

	// Do not mutate velocity here. Raising the intent and letting the next simulated move consume
	// it is what keeps the local prediction and the server's replay bit-for-bit identical: the
	// flag is captured by SetMoveFor and re-applied by UpdateFromCompressedFlags.
	bWantsToDash = 1;
}

bool UTraceCharacterMovementComponent::CanDash() const
{
	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return false;
	}

	// A charge is the whole gate now. DashRechargeRemaining is only the refill clock; with two
	// charges a carrier may dash again while it is still running.
	if (DashCharges <= 0)
	{
		return false;
	}

	// One dash at a time, whatever the pool says — chaining two dashes into one 5200uu/s smear is
	// not the mechanic, and the trail trip test reasons about a single dash window.
	if (DashTimeRemaining > 0.f)
	{
		return false;
	}

	// A "no dashing during a mantle" gate was here (spec v5 §7). Gone with the mantle in v12 §5.

	// MOVE_None is what a dead or fully disabled pawn sits in.
	if (MovementMode == MOVE_None)
	{
		return false;
	}

	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		if (!TraceCharacter->IsAlive())
		{
			return false;
		}
	}

	return true;
}

bool UTraceCharacterMovementComponent::IsDashing() const
{
	// MOVE_None means the pawn has been switched off (dead, or being teleported between spawns), and
	// in that state OnMovementUpdated stops running — so the dash clock freezes wherever it was.
	// Without this guard a player who dies mid-dash would read as "dashing" forever, and the trail
	// trip test keys off exactly this function.
	return DashTimeRemaining > 0.f && MovementMode != MOVE_None;
}

float UTraceCharacterMovementComponent::GetDashCooldownRemaining() const
{
	// =============================================================================================
	// SPEC v9 §2 — THE CARRIER'S "ONE DASH". THIS FUNCTION WAS THE WHOLE BUG.
	// =============================================================================================
	//
	// The user's three symptoms, verbatim: "The carrier still has only one dash, despite the hud
	// showing two. When the first refills, they both do. When dash is used, both charges are
	// consumed." Spec v9 §0 is explicit that the last pass measured the POOL, found it correct, and
	// declared victory. The pool IS correct. The READOUT was not, and the readout is the only thing
	// the player can see.
	//
	// THE OLD CONTRACT WAS BROKEN. ATracePlayerController::GetDashHudState feeds this number
	// straight into FTraceDashHudState::Remaining, whose own doc comment reads "Seconds until the
	// NEXT CHARGE lands. 0 when nothing is recharging", and derives
	// RechargeFraction = 1 - Remaining/(DashDuration + DashCooldown) from it. ATraceHUD::
	// DrawChargePips then draws pip[Charges] at exactly that fraction.
	//
	// The old body returned 0 whenever ANY charge was in hand. So for a carrier at 1 of 2 — one
	// banked, one 3.68 s away — it answered "0 seconds", the controller computed RechargeFraction
	// = 1.0, and DrawChargePips filled the regenerating pip SOLID. The meter read 2 of 2 while the
	// pawn held 1. Every symptom falls out of that single lie:
	//
	//   "the hud shows two"            — 1 of 2 renders as two full pips. Directly.
	//   "when dash is used, BOTH are   — the first spend is invisible (2 pips before, 2 pips
	//    consumed"                       after), so the second spend is the first one the player
	//                                    ever sees, and it empties the whole row at once.
	//   "when the first refills, they  — at 0 of 2 the row is empty and honest. The instant the
	//    both do"                        refill grants charge #1, this function flipped from ~3.6
	//                                    to 0, RechargeFraction snapped to 1.0, and BOTH pips lit
	//                                    on the same frame.
	//   "the carrier still has only    — the meter only ever tells the truth at zero, so the pool
	//    one dash"                       behaves, to the eye, exactly like a single charge.
	//
	// It also explains why the previous harness passed: it pressed twice from a FULL pool and
	// counted two launches. Both launches really do happen. Nothing about that test could see a
	// display that was wrong only in the 1-of-2 and the 0->1 states.
	//
	// THE RULE NOW, and it is the struct's documented contract restated: this is the time until the
	// pool GAINS ITS NEXT CHARGE. Full pool -> 0. Short pool -> the refill clock, whether or not a
	// charge happens to be banked. DashTimeRemaining is deliberately NOT part of it any more: the
	// dash window is not a wait for a charge, and reporting it made the meter dip for 0.18 s on a
	// pawn whose next charge was seconds away.
	//
	// "Can I dash right now" is GetDashCharges() > 0 (which is what ATraceHUD::DrawAbilityRows
	// already uses for its READY tint) and CanDash() — never this.
#if !UE_BUILD_SHIPPING
	// The A/B arm (spec v9 §0). The shipped body, verbatim, so -TraceSingleDashTest can be shown
	// FAILING and then PASSING in the same binary.
	//
	// BOTH A COMMAND-LINE SWITCH AND A CVAR, and the switch is not redundant: -ExecCmds fires at
	// PostEngineInit, and an ECVF_Cheat variable set that early on a client that has not yet
	// connected does not reliably survive into the session (measured — the sibling
	// "NetEmulation.PktLag 40" in the same -ExecCmds list applied and this one did not). FParse of
	// the command line cannot miss, and it is what every other harness in this file already uses.
	extern int32 GTraceDashLegacyChargeReadout;
	static const bool bLegacyFromCommandLine =
		FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyChargeReadout"));
	if (GTraceDashLegacyChargeReadout != 0 || bLegacyFromCommandLine)
	{
		if (DashCharges > 0 && DashTimeRemaining <= 0.f)
		{
			return 0.f;
		}
		if (DashCharges > 0)
		{
			return FMath::Max(0.f, DashTimeRemaining);
		}
		return FMath::Max(0.f, DashRechargeRemaining);
	}
#endif

	if (DashCharges >= GetMaxDashCharges())
	{
		return 0.f;
	}

	return FMath::Max(0.f, DashRechargeRemaining);
}

FVector UTraceCharacterMovementComponent::ComputeDashDirection(const FVector& InAcceleration, const FRotator& InAimRotation) const
{
	// SPEC v7 §5. THE ONLY PLACE THE DASH DIRECTION IS EVER DERIVED. Pure: it reads no clock, no
	// per-frame input and no mutable member except the standing-still fallback's facing, so BeginDash
	// on the server, BeginDash on the client, BeginDash on a replayed frame and Trace.DashVectorTest
	// all get bit-identical answers from identical arguments.

	// The input basis is the AIM YAW, not the capsule's — exactly as ATraceCharacter::DoMove builds
	// it. Reconstructing the same basis here is what turns a single world-space Acceleration back
	// into the two scalars the player actually pressed.
	const FRotationMatrix YawBasis(FRotator(0.f, InAimRotation.Yaw, 0.f));
	const FVector YawForward = YawBasis.GetUnitAxis(EAxis::X);
	const FVector YawRight   = YawBasis.GetUnitAxis(EAxis::Y);

	// Acceleration is level to begin with for a human (DoMove feeds it two level axes), but a bot
	// steers with a world direction that can be tilted, so flatten before decomposing rather than
	// letting a bot's climb angle leak in as a phantom forward amount.
	const FVector PlanarInput(InAcceleration.X, InAcceleration.Y, 0.f);
	const float ForwardAmount = FVector::DotProduct(PlanarInput, YawForward);   // W positive, S negative
	const float StrafeAmount  = FVector::DotProduct(PlanarInput, YawRight);     // D positive, A negative

	// THE WHOLE CHANGE IS THIS PAIR OF BASIS VECTORS.
	//   strafe → YawRight, dead level, "parallel to the ground" whatever the pitch is;
	//   forward → the full aim ray, so look up and W goes up, look 45° and W goes 45°.
	// Summed and normalised, because the spec asks for "one dash length" on the diagonals: an
	// unnormalised sum would make W+D reach ~1.4× as far as W alone.
	FVector Direction = InAimRotation.Vector() * ForwardAmount + YawRight * StrafeAmount;

	if (!Direction.Normalize())
	{
		// No directional input. Spec v7 §5 keeps the old fallback verbatim: dash straight ahead,
		// level. bOrientRotationToMovement means the capsule is already facing the last movement
		// direction, which is what a player expects — and with nothing held there is no forward axis
		// asking for the aim's pitch, so this stays horizontal on purpose.
		Direction = (UpdatedComponent != nullptr)
			? UpdatedComponent->GetForwardVector()
			: (CharacterOwner != nullptr ? CharacterOwner->GetActorForwardVector() : FVector::ForwardVector);
		Direction.Z = 0.f;

		if (!Direction.Normalize())
		{
			Direction = FVector::ForwardVector;
		}
	}

	return Direction;
}

void UTraceCharacterMovementComponent::BeginDash()
{
	// Direction is locked here and never recomputed: a dash you can steer is not a dash, and a
	// steerable dash would also have to re-derive its direction identically during replay.
	//
	// Acceleration (not GetLastInputVector()) is the input source on purpose. Acceleration is
	// restored by MoveAutonomous() from the saved move on every replayed frame, so it reproduces
	// exactly; LastControlInputVector is consumed from live per-frame input and is *not* part of
	// the saved move, so using it would desync the client on every correction. The aim rotation is
	// the second input and does NOT come free like that — see GetDashAimRotation().
	//
	// SPEC v7 §5: the Z-stripping that used to live here is GONE, and so is the contract §5 rule it
	// enforced. A dash may now be vertical. Do not reintroduce `Direction.Z = 0`.
	FVector Direction = ComputeDashDirection(Acceleration, GetDashAimRotation());

	// A GROUNDED DASH MAY NOT AIM INTO THE FLOOR. Look at your feet and press W and the composed
	// direction is straight down; PhysWalking would discard all of it and the player would have spent
	// a charge to stand still. Flatten instead, which is the same dash they would have got before
	// they looked down. Airborne is left alone — a dive is a legitimate move.
	if (IsMovingOnGround() && Direction.Z < 0.f)
	{
		FVector Flattened(Direction.X, Direction.Y, 0.f);
		Direction = Flattened.Normalize() ? Flattened : ComputeDashDirection(FVector::ZeroVector, GetDashAimRotation());
	}

	DashDirection = Direction;
	DashTimeRemaining = GetDashDuration();

#if !UE_BUILD_SHIPPING
	// SPEC v8 §1, THE MEASUREMENT. "Dash feels rubber bandy" is a claim about the correction rate
	// DURING a dash, and the previous pass answered it with a whole-session count taken on the HOST,
	// where corrections cannot happen by construction. Attributing each correction to the dash it
	// landed inside turns that into a RATE that can only be read on a client.
	//
	// bClientUpdating gates it because a REPLAYED dash is the same dash, not a new one: without this
	// every correction would inflate the denominator it is supposed to be measured against, and the
	// rate would fall towards 1 no matter how bad the prediction was.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && CharacterOwner->IsLocallyControlled())
	{
		++DashNetDashCount;

		// The window runs past the dash itself: a correction for a dash frame arrives one round trip
		// LATER, so closing the window at the dash's end would miss exactly the corrections this
		// item is about. Duration + 0.5 s covers a 250 ms round trip with room to spare.
		const UWorld* DashWorld = GetWorld();
		DashNetAttributionUntil = (DashWorld != nullptr)
			? static_cast<float>(DashWorld->GetTimeSeconds()) + GetDashDuration() + 0.5f
			: -1000.f;
	}
#endif

	// Spend a charge and make sure the refill clock is running. Only START it if it is idle: a
	// carrier who spends both charges in quick succession must wait out two sequential windows, not
	// have the first charge's progress thrown away by the second spend.
	DashCharges = FMath::Max(0, DashCharges - 1);
	if (DashRechargeRemaining <= 0.f)
	{
		DashRechargeRemaining = GetDashRechargeWindow();
	}

	// A dash cancels a slide — they are both "planar velocity is on rails" states and letting them
	// overlap would mean two writers fighting over Velocity every frame. Through EndSlide() so the
	// measurement and the exit rule stay on one path; the velocity it writes is overwritten by the
	// dash launch below, which is correct — the dash's speed is strictly the larger of the two.
	//
	// A dash beats the commit window on purpose: the commit exists to stop a slide being fumbled
	// away by the crouch key, not to take the dash away from a player who is about to be shot.
	EndSlide();

	// Launch on the same frame the intent arrived. The move for this frame has already been
	// simulated by the time OnMovementUpdated runs, so this velocity lands on the next one — a
	// single frame, identically on both ends of the wire.
	//
	// SPEC v7 §5: the FULL vector, Z included. A grounded dash whose direction is level still ends up
	// with Velocity.Z == 0, which is the old behaviour exactly; a grounded dash that aims up needs
	// MOVE_Falling as well, because PhysWalking zeroes Z at the top of every walking step and would
	// otherwise eat the launch whole.
	ApplyDashVelocity();

	if (IsMovingOnGround() && DashDirection.Z > UE_KINDA_SMALL_NUMBER)
	{
		SetMovementMode(MOVE_Falling);
	}

	// SPEC v14 §6 — the ability layer's dash hook (Oyster's jar at the start of every dash, Chut's
	// bash window).
	//
	// NOT ON A REPLAYED MOVE. bClientUpdating is true while a correction re-runs frames that already
	// happened; without this guard one dash would drop a jar for every frame the server rewound. The
	// hook is called on the authority AND on the owning client, because both halves of an ability
	// that writes velocity have to see the same event — the ability's own code decides which machine
	// acts (Oyster's jar is authority-only, and says so).
	//
	// Both character hooks are latched against a duplicate anyway: Oyster's TickAbilities polls
	// IsDashing() as a backstop and shares bDashJarSpawnedThisDash with OnDashStarted, and Chut's
	// TryBash is idempotent against its own poll. That is belt and braces on purpose — this project
	// has shipped a "wired" hook that fired twice.
	// SPEC v26 §9 — Dash, GAME-SIDE, and it lives HERE rather than on the ability hook below.
	//
	// The audio pass wired it to UTraceAbilityComponent::NotifyDashStarted, which is one dash per
	// dash and correct for every pawn that HAS an ability component and an enabled ability layer.
	// Integrating it, that turned out to be two conditions the dash itself does not have: a
	// characterless practice-range pawn has no UTraceAbilityComponent, and
	// TraceAbilityIntegration::IsEnabled() is a switch that can be off — in both cases the pawn still
	// dashes and used to do it in silence. BeginDash is the dash; the ability hook is a listener.
	//
	// The line moved here and was DELETED there in the same change, which is the only way it does not
	// become two plays per dash. Same guard as the hook below (never on a replayed move), one call.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating)
	{
		TraceAudio::Play(CharacterOwner, TraceSoundEvents::Dash);
	}

	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && TraceAbilityIntegration::IsEnabled())
	{
		if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(CharacterOwner))
		{
			++TraceAbilityIntegration::Counters().DashStarted;
			Abilities->NotifyDashStarted(DashDirection);
		}
	}

#if !UE_BUILD_SHIPPING
	if (IsDashDebugEnabled())
	{
		const FRotator AimRotation = GetDashAimRotation();
		UE_LOG(LogTraceGame, Display,
			TEXT("DASH %s start: aimPitch=%6.1f accel=(%7.1f,%7.1f,%7.1f) dir=(%6.3f,%6.3f,%6.3f) "
			     "dashPitch=%6.1fdeg v=(%7.1f,%7.1f,%7.1f) mode=%d replayedAim=%d"),
			*GetNameSafe(CharacterOwner), FRotator::NormalizeAxis(AimRotation.Pitch),
			Acceleration.X, Acceleration.Y, Acceleration.Z,
			DashDirection.X, DashDirection.Y, DashDirection.Z,
			FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(DashDirection.Z, -1., 1.))),
			Velocity.X, Velocity.Y, Velocity.Z, static_cast<int32>(MovementMode.GetValue()),
			(CharacterOwner != nullptr && CharacterOwner->bClientUpdating) ? 1 : 0);
	}
#endif
}

void UTraceCharacterMovementComponent::ApplyDashVelocity()
{
	// SPEC v7 §5. One writer for the dash's velocity, used by both the launch and the per-frame
	// re-assert, so the two can never drift apart — they did not before this change either, but they
	// were two copies of the same three lines and now there are three axes to keep in step.
	Velocity = DashDirection * GetDashSpeed();

	// Still on the ground and not asking to leave it: keep Z at zero rather than handing PhysWalking
	// a vertical component it would only discard. This is also what stops a level dash across a slope
	// from being read as an attempt to launch.
	if (IsMovingOnGround() && DashDirection.Z <= UE_KINDA_SMALL_NUMBER)
	{
		Velocity.Z = 0.f;
	}
}

// --- Slide -----------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::SetWantsToSlide(bool bWants)
{
	// A level, not an edge: it is re-sent with every move and the edge is derived inside the
	// simulation (bSlideHeldLastMove), which is what lets the replay path reproduce a fast-fall.
	bWantsToSlide = bWants ? 1 : 0;
}

bool UTraceCharacterMovementComponent::IsSliding() const
{
	return SlideTimeRemaining > 0.f && MovementMode != MOVE_None;
}

// --- Jump held (Demo 19 item 4) ---------------------------------------------------------------

void UTraceCharacterMovementComponent::SetJumpHeld(bool bHeld)
{
	// A LEVEL PLUS A TIMESTAMP. The bit is what travels (FLAG_Custom_1); the stamp is what makes a
	// writer that stops talking stop flying the pawn. See the header for the release hook that was
	// never wired and the five seconds of unwanted climb it bought.
	bWantsToJumpHold = bHeld ? 1 : 0;
	JumpHeldStampSeconds = bHeld ? FPlatformTime::Seconds() : 0.0;
}

bool UTraceCharacterMovementComponent::IsJumpHeld() const
{
	if (bWantsToJumpHold == 0)
	{
		return false;
	}

	// REAL time, on purpose. This is a watchdog on a caller, not a gameplay clock: it must keep
	// counting while the match clock is paused (the select screen pauses the world) so a key held
	// into a menu cannot come back out of it as a five-second flight.
	return (FPlatformTime::Seconds() - JumpHeldStampSeconds) < JumpHeldStaleSeconds;
}

bool UTraceCharacterMovementComponent::CanStartSlide() const
{
	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return false;
	}

	if (MovementMode == MOVE_None || !IsMovingOnGround())
	{
		return false;
	}

	// SlideCooldownRemaining IS THE HIDDEN COOLDOWN (spec v5 §3). It is enforced here and nowhere
	// else, it is charged in EndSlide(), and nothing draws it. Take this test out and "trigger once,
	// like an ability, with a hidden cooldown to prevent spamming it" becomes a slide you can hold by
	// mashing.
	if (SlideTimeRemaining > 0.f || SlideCooldownRemaining > 0.f || DashTimeRemaining > 0.f)
	{
		return false;
	}

	// NOTE (spec v5 §3): this is NOT "the key is still held" as a requirement to keep sliding — that
	// rule is gone with the hold. It only stops a buffered press from firing after the player has
	// already let go, which would start a slide nobody is asking for any more. Once BeginSlide() has
	// run the key is irrelevant for the rest of the slide.
	if (!IsCrouchHeld())
	{
		return false;
	}

	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		if (!TraceCharacter->IsAlive())
		{
			return false;
		}
	}

	// A slide is a way of spending momentum you already have. Crouching from a standstill must not
	// hand out free speed, or "tap crouch" becomes the fastest way to cross the field. This matters
	// more now than it did: with SlideEntrySpeedMultiplier at 1.0 the slide has nothing of its own
	// to give, so entering one slowly would be strictly worse than running.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float EntrySpeed = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideEntrySpeedFraction);

	return FVector(Velocity.X, Velocity.Y, 0.f).SizeSquared() >= FMath::Square(EntrySpeed);
}

void UTraceCharacterMovementComponent::BeginSlide()
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	// Direction comes from where the pawn is actually MOVING, not from where it is looking: a slide
	// is momentum. Velocity is restored by the replay path, so this reproduces exactly.
	FVector Direction(Velocity.X, Velocity.Y, 0.f);
	if (!Direction.Normalize())
	{
		Direction = Acceleration;
		Direction.Z = 0.f;
		if (!Direction.Normalize())
		{
			Direction = (UpdatedComponent != nullptr) ? UpdatedComponent->GetForwardVector() : FVector::ForwardVector;
			Direction.Z = 0.f;
			if (!Direction.Normalize())
			{
				Direction = FVector::ForwardVector;
			}
		}
	}

	SlideDirection = Direction;

	// --- ENTRY SPEED DETERMINES SLIDE VELOCITY (spec v4 §1) --------------------------------------
	//
	// The original formula was max(planar speed, WalkSpeed) × SlideSpeedMultiplier(1.35), which is a
	// flat momentum boost by any reading — a slide entered at walking pace came out 35% faster than a
	// run, for free. Spec v3 flagged that; spec v4 §1 settled it: "The flat momentum boost should be
	// ruled out, going with the source-style movement system instead."
	//
	// So entry is the speed the pawn actually arrived with, and there is exactly one term left:
	//
	//   SlideSpeed = max(EntrySpeed, min(EntrySpeed × SlideEntrySpeedMultiplier, SlideMaxSpeed))
	//
	//   SlideEntrySpeedMultiplier = 1.0  → slide speed IS entry speed.
	//   SlideImpulse                     → DELETED. It was the flat addition, and it is gone.
	//
	// THE OUTER max() IS NOT A BOOST, and it is the one thing here that could be misread as one. It
	// cannot manufacture speed: at multiplier 1.0 it is a no-op, and its only job is to stop
	// SlideMaxSpeed BRAKING somebody who arrived above the cap. Without it, a player landing an
	// air-strafe at 1900+ uu/s, or sliding out of a dash, would be SLOWED by pressing crouch — the
	// precise opposite of "you keep what you brought in", and a floor on losses is not a boost.
	const float EntrySpeed = FVector(Velocity.X, Velocity.Y, 0.f).Size();
	const float ScaledEntrySpeed = FMath::Min(EntrySpeed * GetSlideEntrySpeedMultiplier(),
	                                          FMath::Max(1.f, Settings.SlideMaxSpeed));

	SlideSpeed = FMath::Max(ScaledEntrySpeed, EntrySpeed);

	// ONE PRESS BUYS THE WHOLE DURATION (spec v5 §3). There is no commit window any more because
	// there is nothing left to commit against: the key cannot end this slide.
	SlideTimeRemaining = GetSlideDuration();

	// A fresh slide owns the slide-jump window outright: any coyote grace left over from the previous
	// slide is void, or a slide started 0.1s after one ended would inherit the last one's "well
	// timed" bit and pay the bonus for a hop nobody earned.
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;

	// Cleared, not set: the between-slides buffer is charged in EndSlide() because spec §2.3 asks
	// for a gap BETWEEN slides. An active slide is already blocked by SlideTimeRemaining.
	SlideCooldownRemaining = 0.f;

	Velocity.X = SlideDirection.X * SlideSpeed;
	Velocity.Y = SlideDirection.Y * SlideSpeed;
	if (IsMovingOnGround())
	{
		Velocity.Z = 0.f;
	}

#if !UE_BUILD_SHIPPING
	if (IsSlideDebugEnabled())
	{
		SlideDebugEntrySpeed = SlideSpeed;
		SlideDebugStartLocation = (UpdatedComponent != nullptr)
			? UpdatedComponent->GetComponentLocation()
			: FVector::ZeroVector;
		SlideDebugStartTime = (GetWorld() != nullptr) ? static_cast<float>(GetWorld()->GetTimeSeconds()) : 0.f;

		// SPEC v24 §8. Cleared per slide, exactly as SlideJumpGraceRemaining is above: a fresh slide
		// must not inherit the previous slide's window, or the FIRST measurement after a hop would
		// report a window that opened before this slide began.
		SlideWindowOpenTime = -1.f;
		bSlideWindowWasOpen = false;
	}
#endif
}

void UTraceCharacterMovementComponent::EndSlide()
{
	if (SlideTimeRemaining <= 0.f && SlideSpeed <= 0.f)
	{
		// Idempotent: the cancel paths and the natural exits can all reach here, and a second call
		// must not re-write Velocity with a stale direction or re-charge the cooldown.
		return;
	}

	// --- Hand the momentum back, AND NEVER TOP IT UP ---------------------------------------------
	//
	// The exit speed is the slide's own live speed scaled by SlideExitSpeedRetention and capped at
	//
	//     max(SlideExitMaxSpeedMultiplier × GetMaxSpeed(), the slide's own speed)
	//
	// That max() is "state transitions should preserve velocity vectors rather than resetting them",
	// in one term. Without it, slide→jump was a hard brake: the ceiling evaluated to the walk speed
	// (800) and a 1900uu/s slide handed the player into the air at 800. With it, a slide can never end
	// below the speed it was running at, and the excess then bleeds off through
	// ApplyGroundOverspeedBleed like any other carried momentum — or survives into the air intact if
	// the exit was a jump, which is the entire Apex slide-jump.
	//
	// THERE IS NO FLOOR ANY MORE (spec v4 §1). The old code clamped this into
	// [SlideExitMinSpeedFraction × WalkSpeed, ExitCeiling], and at the shipped fraction of 1.0 that
	// lower bound was WalkSpeed itself — so a slide that had decayed to 470 uu/s was handed back at
	// 820, a measured 73% speed GAIN, for the crime of ending. That is the flat momentum boost the
	// design owner ruled out, spelled on the exit instead of the entry. A slide now ends at exactly
	// what friction left it with, and ordinary ground acceleration takes it from there.
	const float Retained = FMath::Max(0.f, SlideSpeed) * GetSlideExitSpeedRetention();

	// Clear FIRST: GetMaxSpeed() folds in SlideSpeed while IsSliding(), so the ceiling below has to
	// be computed against the speed the pawn is about to live under, not the slide's.
	//
	// The well-timed bit is read off SlideTimeRemaining before it is zeroed, because once the slide is
	// over the information is gone — see SlideJumpGraceRemaining.
	//
	// GetSlideTimeLeft(), not SlideTimeRemaining: a slide that ends by DECAY is just as much at its
	// end as one that runs its duration out, and measuring only the duration clock made every
	// walking-pace slide score as mistimed however well the hop was pressed.
	const float WellTimedWindow = GetSlideJumpWindowSeconds();
	bSlideJumpGraceWellTimed = (WellTimedWindow > 0.f && GetSlideTimeLeft() <= WellTimedWindow) ? 1 : 0;
	SlideJumpGraceRemaining = WellTimedWindow;

#if !UE_BUILD_SHIPPING
	// SPEC v24 §8. Captured before the clock is zeroed: "did this slide end on its duration clock or
	// by decaying out" is the difference between a measurement of §8 and a measurement of friction,
	// and once SlideTimeRemaining is 0 the two are indistinguishable. Observation only.
	const float SlideTimeRemainingAtEnd = SlideTimeRemaining;
#endif

	SlideTimeRemaining = 0.f;
	const float ExitedSpeed = SlideSpeed;
	SlideSpeed = 0.f;

	// Spec §2.3: "add a .8 second buffer between slides". Charged HERE, at the end, so the knob says
	// what it means — the old cooldown was measured from slide start, which made the actual buffer
	// SlideCooldown minus SlideDuration.
	SlideCooldownRemaining = GetSlideCooldownSeconds();

	// Named ExitCeiling rather than the obvious Ceiling: "Floor" is dense with meaning inside a
	// movement component (CurrentFloor, FindFloor, FFindFloorResult) and a local that reads like the
	// walkable surface in a function about speed is a trap for the next reader. The matching ExitFloor
	// local is gone with the setting that fed it.
	float ExitCeiling = FMath::Max(1.f, GetMaxSpeed()) * GetSlideExitMaxSpeedMultiplier();
	if (IsLandingMomentumPreserved())
	{
		ExitCeiling = FMath::Max(ExitCeiling, ExitedSpeed);
	}

	// Min, not Clamp. The only bound left is the ceiling; nothing lifts a slow exit.
	const float ExitSpeed = FMath::Min(Retained, ExitCeiling);

	// Direction of TRAVEL, not the steered slide direction: on a ledge or against a wall the two can
	// differ, and the player's momentum is the one they can see.
	FVector ExitDirection(Velocity.X, Velocity.Y, 0.f);
	if (!ExitDirection.Normalize())
	{
		ExitDirection = SlideDirection;
		ExitDirection.Z = 0.f;
		if (!ExitDirection.Normalize())
		{
			ExitDirection = (UpdatedComponent != nullptr) ? UpdatedComponent->GetForwardVector() : FVector::ForwardVector;
			ExitDirection.Z = 0.f;
			if (!ExitDirection.Normalize())
			{
				ExitDirection = FVector::ForwardVector;
			}
		}
	}

	Velocity.X = ExitDirection.X * ExitSpeed;
	Velocity.Y = ExitDirection.Y * ExitSpeed;

#if !UE_BUILD_SHIPPING
	// Observation only — never feeds the simulation, so it cannot desync anything. Logged on the
	// authority alone: the server also advances a remote client's slide inside MoveAutonomous, and a
	// client replaying corrections would otherwise count the same slide several times.
	// SlideDebugStartTime > 0 rejects the one slide that could be in flight when the cvar is toggled
	// on mid-match, whose start was never recorded and would otherwise report the whole match as its
	// duration and poison the mean.
	if (IsSlideDebugEnabled() && SlideDebugStartTime > 0.f
		&& CharacterOwner != nullptr && CharacterOwner->HasAuthority() && GetWorld() != nullptr)
	{
		const float Now = static_cast<float>(GetWorld()->GetTimeSeconds());
		const float Duration = FMath::Max(0.f, Now - SlideDebugStartTime);
		const FVector Here = (UpdatedComponent != nullptr) ? UpdatedComponent->GetComponentLocation() : SlideDebugStartLocation;
		const float Distance = FVector::Dist2D(Here, SlideDebugStartLocation);

		++GTraceSlideDebugCount;
		GTraceSlideDebugTotalDuration += Duration;
		GTraceSlideDebugTotalDistance += Distance;
		SlideDebugStartTime = 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("SLIDE %-16s dur=%5.2fs dist=%6.0fuu entry=%6.0f exitSpeed=%6.0f (slideSpeed was %6.0f) | n=%3d avgDur=%5.2fs avgDist=%6.0fuu"),
			*GetNameSafe(CharacterOwner), Duration, Distance, SlideDebugEntrySpeed, ExitSpeed, ExitedSpeed,
			GTraceSlideDebugCount,
			GTraceSlideDebugTotalDuration / FMath::Max(1, GTraceSlideDebugCount),
			GTraceSlideDebugTotalDistance / FMath::Max(1, GTraceSlideDebugCount));

		// SPEC v24 §8 — THE MEASUREMENT THE ITEM ACTUALLY ASKS FOR, on its own line and its own tag so
		// it can be grepped out of a match log without the rest of the slide block.
		//
		// open  = seconds into THIS slide at the first frame IsSlideJumpWellTimed() went true
		// close = the slide's measured length, because the window is anchored to the slide's end and
		//         the slide ending is what closes it
		// grace = the coyote tail EndSlide() has just charged (SlideJumpGraceRemaining), during which
		//         a hop still counts as well timed — so the window a PLAYER can hit runs from `open`
		//         to `close + grace`, and printing only the first two would understate it.
		// endedBy is printed because a decay-ended slide is not a measurement of the duration clock.
		//
		// Every number here is read off the running simulation. None of them is GetSlideDuration() or
		// GetSlideJumpWindowSeconds() restated: `expected` is printed LAST and separately, precisely
		// so the measured pair can disagree with the constants and be seen to.
		const float MeasuredOpen = SlideWindowOpenTime;
		UE_LOG(LogTraceGame, Display,
			TEXT("V24WINDOW %-16s slide=%5.3fs  window open=%s close=%5.3fs (+%4.3fs coyote grace) "
			     "len=%s  endedBy=%s  arm=%s  [expected: slide %5.3fs, window %4.3fs]"),
			*GetNameSafe(CharacterOwner), Duration,
			(MeasuredOpen >= 0.f) ? *FString::Printf(TEXT("%5.3fs"), MeasuredOpen) : TEXT("NEVER"),
			Duration, SlideJumpGraceRemaining,
			(MeasuredOpen >= 0.f) ? *FString::Printf(TEXT("%5.3fs"), Duration - MeasuredOpen) : TEXT("  n/a"),
			(SlideTimeRemainingAtEnd <= 0.f) ? TEXT("clock") : TEXT("decay/exit"),
			IsV24LegacySlide() ? TEXT("LEGACY (pre-v24)") : TEXT("V24 (shipped)"),
			GetSlideDuration(), GetSlideJumpWindowSeconds());
	}
#endif
}

// --- Slide-jump ---------------------------------------------------------------------------------

bool UTraceCharacterMovementComponent::IsSlideJumpAvailable() const
{
	return IsSlideJumpEnabled() && MovementMode != MOVE_None
		&& (IsSliding() || SlideJumpGraceRemaining > 0.f);
}

float UTraceCharacterMovementComponent::GetSlideTimeLeft() const
{
	if (SlideTimeRemaining <= 0.f)
	{
		return 0.f;
	}

	// Route 1: the duration clock.
	float TimeLeft = SlideTimeRemaining;

	// Route 2: the decay. OnMovementUpdated ends the slide as soon as SlideSpeed falls to
	// WalkSpeed x SlideExitSpeedFraction, and with the shipped numbers that happens FIRST for anybody
	// who entered at walking pace. Deceleration is constant, so the crossing time is exact rather than
	// estimated — no iteration, no drift between client and server.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Deceleration = GetSlideDeceleration();
	if (Deceleration > 0.f)
	{
		const float DecayFloor = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideExitSpeedFraction);
		const float SpeedAboveFloor = SlideSpeed - DecayFloor;
		TimeLeft = FMath::Min(TimeLeft, FMath::Max(0.f, SpeedAboveFloor) / Deceleration);
	}

	return TimeLeft;
}

bool UTraceCharacterMovementComponent::IsSlideJumpWellTimed() const
{
	if (!IsSlideJumpAvailable())
	{
		return false;
	}

	const float Window = GetSlideJumpWindowSeconds();
	if (Window <= 0.f)
	{
		return false;
	}

	// Mid-slide the window is live — the last Window seconds of the slide, by WHICHEVER exit the slide
	// is actually heading for (see GetSlideTimeLeft). During the coyote grace it is whatever the slide
	// was worth at the moment it ended.
	return IsSliding() ? (GetSlideTimeLeft() <= Window) : (bSlideJumpGraceWellTimed != 0);
}

// --- The wall jump (spec v8 §7) -----------------------------------------------------------------

void UTraceCharacterMovementComponent::HandleImpact(const FHitResult& Hit, float TimeSlice, const FVector& MoveDelta)
{
	Super::HandleImpact(Hit, TimeSlice, MoveDelta);

	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// =============================================================================================
	// PATCH 28 §5 — THE SURF SENSOR, AND IT IS THE SAME ONE THE WALL JUMP USES.
	//
	// PhysFalling calls HandleImpact for every blocking hit that was NOT a valid landing spot — which
	// is exactly the set of hits a steep face produces, because IsValidLandingSpot() refuses anything
	// IsWalkable() refuses. So the contact frame is delivered here for free, on the client, on the
	// server and on every replayed move, from inside a sweep the engine was already doing. A
	// hand-rolled per-frame probe would have been a second, differently-timed source of truth for the
	// same fact, and this file already records why that is the bug it is.
	//
	// ORDERED BEFORE THE WALL-JUMP BLOCK ON PURPOSE. The two bands do not overlap (GetSurfMinNormalZ()
	// is above GetWallJumpMaxNormalZ()), so no hit can be both — but the wall-jump block returns early
	// when the wall jump is switched off, and surf must not be hostage to another mechanic's switch.
	//
	// DEMO 29 ITEM 4(b) ADDS THE OTHER HALF OF IT — see the block immediately below. The sensor itself
	// is unchanged and still tests IsFalling(); what changed is that a WALKING pawn can now become a
	// falling one on this very frame, a few lines earlier, and fall through into it.
	// =============================================================================================

	// =============================================================================================
	// DEMO 29 ITEM 4(b) — SURFING INTO A CURVE FROM THE FLOOR.
	//
	// The owner: "It still doesn't feel like you can 'surf' into curves/curved ramps in order to gain
	// momentum." Patch 28 measured gain on a pawn that was ALREADY on a ramp; nothing in it ever
	// approached one. Measured with -TraceSurfApproachTest on the shipped build, running at a rail on
	// the flat floor at the 800 uu/s ground limit, the answer was worse than "no gain": at 90, 60, 45,
	// 30 and 20 degrees to the rail the planar speed after contact was 0 uu/s and the surf state was
	// never entered. (CanStepUp() is why, and that is fixed there.)
	//
	// Restoring the slide alone would only make a rail a wall you scrape along. What a surf ramp
	// SHOULD do to a runner is what it does to a faller, and Source has always agreed: a face you
	// cannot stand on does not stop you, it takes you off the ground and redirects you along itself.
	// So a walking pawn that leans into a surf plane hard enough LEAVES THE GROUND and has its
	// velocity clipped against that plane — the identical two operations PhysFalling performs on a
	// surfer, in Source's own order (accelerate, then clip), and then the sensor below latches the
	// ride exactly as it would for an airborne contact.
	//
	// The clip is not a gift: it removes the into-the-face component like any other surf contact, so a
	// head-on charge loses about a third of its speed and gets height instead, while a shallow lean
	// keeps nearly all of it and gets a ride. That gradient IS the mechanic — the reward is for
	// approaching well, and the ceiling that bounds every other surf bounds this one too.
	//
	// WHY IT IS SAFE TO CHANGE THE MOVEMENT MODE HERE. MoveAlongFloor is called from PhysWalking,
	// which tests `MovementMode != StartingMovementMode` immediately afterwards, refunds the unused
	// time and re-enters StartNewPhysics in the new mode. This is the engine's own supported flow —
	// the same one a step-up-and-over uses — not a hole being exploited.
	//
	// PREDICTION: no new state. The two writes are Velocity and MovementMode, both of which the
	// engine already snapshots and replays, and the latch below is the same saved-move state Patch 28
	// already round-trips.
	// =============================================================================================
	if (IsSurfGroundEntryEnabled() && Hit.bBlockingHit && IsMovingOnGround()
		&& MovementMode != MOVE_None && IsSurfPlane(Hit.ImpactNormal))
	{
		FVector EntryNormal = Hit.ImpactNormal;
		if (EntryNormal.Normalize())
		{
			// The component of travel heading INTO the face. Running along the base of a rail is zero
			// here and stays a scrape; leaning into it is what buys the ride.
			const float ApproachSpeed = -static_cast<float>(FVector::DotProduct(Velocity, EntryNormal));
			if (ApproachSpeed >= GetSurfGroundEntryMinApproachSpeed())
			{
				SetMovementMode(MOVE_Falling);
				Velocity = ClipVelocityAgainstPlane(Velocity, EntryNormal, GetSurfOverbounce());

#if !UE_BUILD_SHIPPING
				++SurfGroundEntries;
#endif
			}
		}
	}

	if (Hit.bBlockingHit && IsFalling() && MovementMode != MOVE_None && IsSurfPlane(Hit.ImpactNormal))
	{
		FVector PlaneNormal = Hit.ImpactNormal;
		if (PlaneNormal.Normalize())
		{
			// A FRESH SURF is one that starts with no clock running. Everything captured here is
			// captured once per ride, so a curved rail's facet joints extend the ride rather than
			// restarting it — which is what makes "preserved speed on transitions between ramp faces"
			// true of the CEILING as well as of the velocity: the entry speed the ceiling is floored
			// at is the speed the RIDE began with, not the speed at the last joint.
			if (!IsSurfing())
			{
				SurfEntrySpeed = GetPlanarSpeed();
				SurfElapsedSeconds = 0.f;
				SurfPeakSpeed = static_cast<float>(Velocity.Size());

#if !UE_BUILD_SHIPPING
				++SurfCount;
				SurfEntrySpeedSum += SurfEntrySpeed;
				if (const UWorld* SurfWorld = GetWorld())
				{
					// The correction-attribution window, on the same terms as the dash's and the wall
					// jump's: a correction that lands while a player is riding a ramp is the number
					// that falsifies "surf is predicted", and it can only be read on a client.
					SurfAttributionUntil = static_cast<float>(SurfWorld->GetTimeSeconds()) + 2.f;
				}
#endif
			}

			// Overwriting an older normal is deliberate and is the curved-ramp case: the facet you are
			// touching NOW is the plane your velocity is being clipped against.
			SurfPlaneNormal = PlaneNormal;
			SurfContactRemaining = GetSurfContactGraceSeconds();
		}
	}
#if !UE_BUILD_SHIPPING
	else if (Hit.bBlockingHit && IsFalling() && MovementMode != MOVE_None)
	{
		// THE NEGATIVE CONTROL, COUNTED. Every airborne contact this component saw and did NOT turn into
		// a surf — a walkable face, a wall, a ceiling — is one this build refused, and the count is what
		// turns "you cannot surf ordinary geometry" into a number in Trace.Move.SurfReport rather than
		// an assertion in a comment. Counted here rather than only for walkable faces because
		// PhysFalling does not deliver a landing spot to HandleImpact at all, so a walkable-only counter
		// would be structurally near-zero and would look like a passing test for the wrong reason.
		++SurfContactsRefused;
	}
#endif

	if (!IsWallJumpEnabled())
	{
		return;
	}

	// AIRBORNE ONLY. Running into a wall on the ground already has an answer — jump — and recording
	// the contact there would let a player walk up to a wall, step off a ledge and wall-jump off a
	// face they were never airborne against.
	//
	// The "and not mid-mantle" clause that used to sit here went with the mantle (spec v12 §5). This
	// is one of the two places that made the wall jump lose to it; see TryWallJump().
	if (!IsFalling() || MovementMode == MOVE_None)
	{
		return;
	}

	if (!Hit.bBlockingHit)
	{
		return;
	}

	// A WALL, not a ramp. PhysFalling has already refused this hit as a landing spot, but "not a
	// landing spot" includes ceilings and steep-but-walkable-adjacent geometry; the explicit test is
	// what keeps the mechanic to vertical faces and off anything the player could have walked up.
	const FVector Normal = Hit.ImpactNormal;
	if (FMath::Abs(Normal.Z) > GetWallJumpMaxNormalZ())
	{
		return;
	}

	FVector PlanarNormal(Normal.X, Normal.Y, 0.f);
	if (!PlanarNormal.Normalize())
	{
		return;
	}

	// Latch the face and open the window. Overwriting an older normal is deliberate: the wall you are
	// touching NOW is the one a jump should launch off, and in a corner that is the last one hit.
	WallJumpNormal = PlanarNormal;
	WallJumpWindowRemaining = GetWallJumpWindowSeconds();

	// AND THE MOMENTUM, WHICH ONLY EXISTS HERE. See the header on WallJumpEntryVelocity: measured on a
	// client, a head-on approach reads entry=0 by the time the jump is pressed, because PhysFalling
	// re-derives Velocity from the distance the capsule actually moved and a pawn stopped by a wall
	// moved nothing. Without this capture the "preserve and redirect momentum" of spec v8 §7 is a
	// 360 uu/s nudge and nothing else. Captured planar; the vertical component is the wall jump's own
	// number.
	//
	// Take the FASTER of this frame's velocity and whatever the window already holds: the engine can
	// deliver two HandleImpact calls for one collision (the sweep, then the slide's re-sweep), and the
	// second arrives with the speed already scrubbed off.
	const FVector PlanarEntry(Velocity.X, Velocity.Y, 0.f);
	if (WallJumpEntryVelocity.IsNearlyZero() || PlanarEntry.SizeSquared() > WallJumpEntryVelocity.SizeSquared())
	{
		WallJumpEntryVelocity = PlanarEntry;
	}

#if !UE_BUILD_SHIPPING
	// SPEC v10 §5, THE STICK METER. This is the frame the complaint's clock starts on.
	BeginWallStickSample(PlanarNormal);
#endif
}

bool UTraceCharacterMovementComponent::IsWallJumpAvailable() const
{
	// The "&& MantleTimeRemaining <= 0.f" clause was here (spec v8 §7). It is the clause spec v9 §5
	// identified as the mechanism by which the mantle deleted wall jumps: a mantle that started on
	// the contact frame made this false for the whole contact, so the player's press landed on a pawn
	// the mantle already owned. Gone with the mantle in v12 §5 — availability is now purely the
	// window, the cap, and being airborne.
	return IsWallJumpEnabled()
		&& MovementMode != MOVE_None
		&& IsFalling()
		&& WallJumpWindowRemaining > 0.f
		&& !WallJumpNormal.IsNearlyZero()
		&& WallJumpsSinceGround < GetWallJumpMaxConsecutive();
}

bool UTraceCharacterMovementComponent::TryWallJump()
{
	if (!IsWallJumpAvailable())
	{
		return false;
	}

	const FVector Normal = WallJumpNormal;

	// --- REDIRECT, DO NOT RESET -------------------------------------------------------------------
	//
	// Reflect ONLY the component that was travelling into the wall. A head-on approach comes straight
	// back, a glancing one glances off, and a pawn already moving away from the face (a corner, or a
	// frame where the collision response has already pushed it out) keeps its vector untouched rather
	// than being fired back into the geometry it just escaped.
	// THE APPROACH VELOCITY, NOT THE POST-COLLISION ONE. WallJumpEntryVelocity is what the pawn was
	// carrying on the frame it touched the wall; Velocity by now is what survived the collision, which
	// for a head-on hit is nothing at all (measured: entry=0 uu/s on a client, every jump). Falling
	// back to the live velocity keeps a window that somehow opened without a capture working.
	FVector Planar = WallJumpEntryVelocity;
	Planar.Z = 0.f;
	if (Planar.IsNearlyZero())
	{
		Planar = FVector(Velocity.X, Velocity.Y, 0.f);
	}
	const float EntrySpeed = Planar.Size();

	// Captured before the reflection mutates Planar. Measurement only (spec v8 §7's "in a new
	// direction" is an angle), but it costs one normalise and keeps the maths honest.
	const FVector EntryPlanarDirection = Planar.GetSafeNormal();
	const float IntoWall = FVector::DotProduct(Planar, Normal);
	if (IntoWall < 0.f)
	{
		Planar -= 2.f * IntoWall * Normal;
	}

	// The reflection is a rotation, so it preserves magnitude exactly; the retention is the only place
	// speed is allowed to change, and it can only ever remove.
	FVector LaunchDirection = Planar;
	if (!LaunchDirection.Normalize())
	{
		// Pressed flat against the wall with no planar speed at all. The outward impulse below is the
		// whole launch in that case, which is what makes a standing wall jump an escape rather than a
		// wasted press.
		LaunchDirection = Normal;
	}

	// SPEC v19 §3 — LILY'S WALL-JUMP BONUS TOUCHES THE RETENTION TERM AND NOTHING ELSE.
	//
	// The scale multiplies ONLY the speed the wall hands back (EntrySpeed x retention). It is
	// deliberately kept off the flat outward impulse and off the vertical multiplier below, because
	// those two are the global feel of a wall jump — how far it shoves you off the face and how high
	// it throws you — and §3 says her bonus is hers alone, the global numbers must not move.
	//
	// Written as a per-pawn query rather than as an edit to WallJumpSpeedRetention for that reason:
	// the shipped knob keeps meaning what it says for the other nine, who get 1.0 here.
	const float WallJumpRetention =
		GetWallJumpSpeedRetention() * TraceAbilityTraits::GetWallJumpMomentumScale(CharacterOwner);

	FVector Launch = LaunchDirection * (EntrySpeed * WallJumpRetention) + Normal * GetWallJumpOutwardImpulse();

	// --- AND IT MAY NOT BEAT THE AIR-STRAFE CEILING (spec v5 §1) ----------------------------------
	//
	// Same shape as the clamp at the end of ApplySourceAirAcceleration, and for the same reason: the
	// ceiling exists to stop speed being BUILT in the air, not to brake speed that was carried into
	// it. So the cap is floored at the speed the pawn arrived with — a wall jump keeps every unit it
	// was already carrying, and can never add past the hard cap. Without this the outward impulse
	// would be a free, repeatable +360 uu/s that the whole of spec v5 §1 was written to prevent.
	if (IsAirStrafeHardCapEnabled())
	{
		const float Ceiling = FMath::Max(EntrySpeed, GetAirStrafeHardCapSpeed());
		const float LaunchSpeed = Launch.Size();
		if (LaunchSpeed > Ceiling && LaunchSpeed > UE_KINDA_SMALL_NUMBER)
		{
			Launch *= (Ceiling / LaunchSpeed);
		}
	}

	Velocity.X = Launch.X;
	Velocity.Y = Launch.Y;

	// Super::DoJump has already set Z to at least JumpZVelocity and switched to MOVE_Falling. Assign
	// rather than scale: the wall jump's vertical component is its own number (a multiple of the
	// jump, like every other launch here), not a modifier on whatever the fall had left.
	Velocity.Z = JumpZVelocity * GetWallJumpVerticalMultiplier();

	// One jump per contact, and one step up the ladder. The window is closed rather than left to
	// expire so that a second press inside the same window cannot double-dip off one wall.
	WallJumpWindowRemaining = 0.f;
	WallJumpNormal = FVector::ZeroVector;
	WallJumpEntryVelocity = FVector::ZeroVector;
	++WallJumpsSinceGround;

	// SPEC v10 §5, CAUSE 1. ARM THE INTO-WALL INPUT LOCKOUT.
	//
	// Held on its own field rather than on WallJumpNormal because that one has just been cleared two
	// lines up — correctly, since it means "a face a jump COULD launch off" and this launch has spent
	// it. What the lockout needs is the face this launch left, which only exists from here.
	//
	// Also consumes any buffered press: this launch IS the press being remembered, and leaving the
	// buffer charged would let it fire a second time off the next contact the player did not ask for.
	WallJumpLaunchNormal = Normal;
	WallJumpControlLockoutRemaining = GetWallJumpControlLockoutSeconds();
	WallJumpInputBufferRemaining = 0.f;

	// SPEC v9 §5's mantle lockout was applied here — the second half of "a wall jump overrides a
	// mantle". Deleted in v12 §5 with the mantle. NOTHING now touches Velocity after this point in
	// the move: the launch written above is what the player gets, on the client, on the server and on
	// every replayed move, with no second consumer of the wall contact to argue with.

#if !UE_BUILD_SHIPPING
	// SPEC v10 §5, THE STICK METER. Stamp the launch into the open sample so the report can split the
	// total stick into "waiting for the launch" and "getting away from the wall afterwards" — the two
	// halves have different causes and only a split can say which fix moved which number.
	if (WallStickContactTime >= 0.f && WallStickLaunchTime < 0.f)
	{
		const UWorld* StickWorld = GetWorld();
		WallStickLaunchTime = (StickWorld != nullptr) ? static_cast<float>(StickWorld->GetTimeSeconds()) : -1.f;
	}

	// SPEC v8 §7, THE MEASUREMENT. Counted on the RECORD pass only, for BeginDash()'s reason: a
	// replayed wall jump is the same wall jump, and counting it again would inflate the denominator
	// that "corrections per wall jump" is measured against.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && CharacterOwner->IsLocallyControlled())
	{
		const FVector LaunchPlanar(Velocity.X, Velocity.Y, 0.f);
		const float LaunchSpeed = static_cast<float>(LaunchPlanar.Size());

		++WallJumpCount;
		WallJumpEntrySpeedSum += EntrySpeed;
		WallJumpLaunchSpeedSum += LaunchSpeed;
		WallJumpLaunchZSum += static_cast<float>(Velocity.Z);
		WallJumpMaxConsecutiveSeen = FMath::Max(WallJumpMaxConsecutiveSeen, WallJumpsSinceGround);

		// "In a NEW direction" is an angle, so measure the angle between the approach and the launch.
		if (!EntryPlanarDirection.IsNearlyZero() && LaunchSpeed > 1.f)
		{
			const float Cosine = FMath::Clamp(
				static_cast<float>(FVector::DotProduct(EntryPlanarDirection, LaunchPlanar.GetSafeNormal())),
				-1.f, 1.f);
			WallJumpTurnDegreesSum += FMath::RadiansToDegrees(FMath::Acos(Cosine));
		}

		const UWorld* WallJumpWorld = GetWorld();
		WallJumpAttributionUntil = (WallJumpWorld != nullptr)
			? static_cast<float>(WallJumpWorld->GetTimeSeconds()) + 0.75f
			: -1000.f;
	}

	if (IsDashDebugEnabled() && CharacterOwner != nullptr)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("WALLJUMP %-16s normal=(%6.3f,%6.3f) entry=%6.0f -> launch=%6.0f uu/s (retain=%.2f "
			     "outward=%.0f) velZ=%6.0f consecutive=%d/%d role=%d"),
			*GetNameSafe(CharacterOwner), Normal.X, Normal.Y, EntrySpeed, GetPlanarSpeed(),
			GetWallJumpSpeedRetention(), GetWallJumpOutwardImpulse(), Velocity.Z,
			WallJumpsSinceGround, GetWallJumpMaxConsecutive(),
			static_cast<int32>(CharacterOwner->GetLocalRole()));
	}
#endif

	// SPEC v26 §9 — WallJump, client-side. INSIDE TryWallJump rather than at DoJump's
	// `if (TryWallJump()) return true;`, because TryWallJump is ALSO reached from OnMovementUpdated's
	// buffered-press path (spec v10 §5) and both of those are real wall jumps to the player. This is
	// the single point every successful wall jump passes through, and it is past every refusal.
	//
	// The bClientUpdating guard is the double-play guard: a server correction re-runs the move on the
	// owning client — the only machine that hears this — and would otherwise fire it twice. There is
	// no bReplayingMoves parameter here; bClientUpdating covers the same window for this entry point.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating)
	{
		TraceAudio::Play(CharacterOwner, TraceSoundEvents::WallJump);
	}

	return true;
}

bool UTraceCharacterMovementComponent::CanAttemptJump() const
{
	// Super, MINUS the "!bWantsToCrouch" clause. See the header: crouch is the slide key here and
	// never resizes the capsule, so the engine's "you might not have headroom to stand up" rule has
	// nothing to protect and was silently making the slide-jump unreachable for human players.
	//
	// Everything else Super checks is kept verbatim, including the IsMovingOnGround() || IsFalling()
	// test, which ACharacter::JumpIsAllowedInternal still validates against JumpMaxCount on top.
	//
	// The "&& !IsMantling()" clause that used to be here is gone with the mantle (spec v12 §5). That
	// clause was the other reason the wall jump felt broken: a mantle held the pawn in MOVE_Flying
	// for 0.35 s during which this function refused EVERY press, which is most of what "it feels like
	// the player is sticking to the wall for a second" was. There is now no state in this component
	// that can refuse a jump the engine would have allowed.
	return IsJumpAllowed() && (IsMovingOnGround() || IsFalling());
}

bool UTraceCharacterMovementComponent::DoJump(bool bReplayingMoves, float DeltaTime)
{
	// =============================================================================================
	// OWNER ITEM 7 — NO JUMP WHILE SURFING. FIRST, AND BEFORE ANYTHING IS TOUCHED.
	//
	// "A player should not be able to jump while surfing on the buttresses."
	//
	// WHY IT IS THE FIRST STATEMENT IN THE FUNCTION AND NOT A CLAUSE FURTHER DOWN. Everything below
	// this point has a SIDE EFFECT that a later refusal cannot undo:
	//
	//   * IsSlideJumpAvailable() is read and EndSlide() is called, which charges the between-slides
	//     cooldown. You cannot be sliding and surfing at once (surf implies airborne) so this is not
	//     reachable today — but "not reachable today" is exactly how the next mantle or dash state
	//     ends up spending a cooldown on a refused press.
	//   * Super::DoJump() writes Velocity.Z = max(Z, JumpZVelocity). The wall-jump branch below has to
	//     put that back by hand from PreJumpVelocity, and a rider's velocity mid-ride is the single
	//     most correction-sensitive quantity in this file.
	//   * The wall-jump branch ARMS WallJumpInputBufferRemaining on a "no wall yet" refusal. A press
	//     made mid-ride is not a press that was one tick early for a wall; buffering it would fire a
	//     wall jump the instant the ride ended next to a wall, which is the same free launch by a
	//     slower route.
	//
	// Refusing here does none of those things: it returns false having read one bool and one clock,
	// and ACharacter::CheckJumpInput does not increment JumpCurrentCount for a DoJump that returned
	// false, so the press is simply spent.
	//
	// IDENTICAL ON BOTH MACHINES, WHICH IS THE PART THAT MATTERS FOR A PREDICTED MOVE. The test is
	// IsSurfing(), whose entire state is SurfContactRemaining and SurfPlaneNormal, and both of those
	// already ride the saved move (SavedSurfContactRemaining / SavedSurfPlaneNormal — see the
	// PREDICTION block in the header). So:
	//   * the owning client refuses the press during PerformMovement;
	//   * the server, replaying the same ServerMove against the same geometry, has the same clock and
	//     the same normal and refuses it too;
	//   * a correction replay restores both fields before re-running the move, so the replay refuses
	//     the same presses the first run did.
	// Nothing new is added to the saved move, because nothing new is remembered.
	//
	// WHAT IT DELIBERATELY DOES NOT DO: it does not touch the grace clock. IsSurfing() stays true for
	// GetSurfContactGraceSeconds() after the last facet contact, so a press in that window is refused
	// too. That is intended — the grace is what stops a fan of facets flickering the ride state, and a
	// jump that only worked in the gaps between facets would be a frame-timing exploit, not a move.
	// The moment the ride genuinely ends (the clock runs out, or the pawn lands) the jump is back.
	// =============================================================================================
	if (DoesSurfBlockJump() && IsSurfing())
	{
#if !UE_BUILD_SHIPPING
		// COUNTED, so "you cannot jump while surfing" is a number in Trace.Move.SurfReport rather than
		// a claim in this comment. Only on the machine that owns the press, and never on a replay, or
		// one refused press would be counted several times by a single correction.
		if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && !bReplayingMoves)
		{
			++SurfJumpsRefused;
		}
#endif
		return false;
	}

	// Capture BEFORE anything is touched: EndSlide() below rewrites every one of these.
	const bool bSlideJump = IsSlideJumpAvailable();
	const bool bWellTimed = IsSlideJumpWellTimed();

	// SPEC v8 §7. BOTH OF THESE MUST BE READ BEFORE Super::DoJump, and the first one is a trap worth
	// naming: Super calls SetMovementMode(MOVE_Falling), so IsFalling() is TRUE after it even for an
	// ordinary jump off the floor. Asking afterwards would route every ground jump into the wall-jump
	// branch and refuse it — i.e. it would delete the jump key.
	const bool bWasAirborneBeforeJump = IsFalling();
	const FVector PreJumpVelocity = Velocity;

	// The speed the jump is entitled to carry. Mid-slide that is the slide's own live speed — which is
	// also what Velocity is, because OnMovementUpdated re-asserts it every frame — and during the
	// coyote grace it is simply whatever the pawn has now, which is the honest Source answer: you keep
	// what you brought, and if you dawdled after the slide ended, friction has already taken its cut.
	const float CarrySpeed = IsSliding()
		? FMath::Max(SlideSpeed, GetPlanarSpeed())
		: GetPlanarSpeed();

	// The slide direction, kept as a fallback for the degenerate case where Velocity has been zeroed
	// by a collision on the exact frame of the jump.
	const FVector CarryDirectionFallback = SlideDirection;

	if (bSlideJump)
	{
		// THE ONE EXIT, still. A jump out of a slide ends it through EndSlide() like the duration
		// expiring or the key coming up, so the 0.8s between-slides buffer is charged exactly once and
		// on exactly one code path — spec v4 §1 keeps that cooldown, and a slide-jump that skipped it
		// would turn the payoff move into a hop loop. The velocity EndSlide() writes is overwritten
		// below; that is intended, and it is why the retention is applied to CarrySpeed (captured
		// above) rather than to whatever the exit rule happened to leave behind.
		EndSlide();
	}

	// Super sets Velocity.Z = max(Velocity.Z, JumpZVelocity) and switches to MOVE_Falling, or returns
	// false if the jump was not legal. If it refuses, the slide has already ended — which is correct
	// and not a leak: the only way to reach here with bSlideJump true and be refused is to be dead or
	// movement-disabled, and in both cases the slide has to end anyway.
	if (!Super::DoJump(bReplayingMoves, DeltaTime))
	{
		return false;
	}

	if (!bSlideJump)
	{
		// SPEC v8 §7. An airborne press that is not a slide-jump is the wall jump's only entry point.
		//
		// ORDER MATTERS AND THE SLIDE-JUMP WINS. A jump taken inside the slide-jump's coyote window is
		// still a slide-jump even with a wall window open, so nothing about spec v4 §1 or v5 §3 changes
		// behaviour — the wall jump only ever sees presses the slide had no claim on.
		//
		// TryWallJump() refuses (and leaves Velocity exactly as Super left it) whenever the pawn is
		// grounded, past the consecutive cap, or outside the contact window. That refusal is what stops
		// the raised JumpMaxCount from being a double jump: Super has already written
		// Velocity.Z = max(Z, JumpZVelocity), which is a legitimate mid-air jump — so a refusal here
		// has to un-ask the whole thing, which is exactly what returning false does. ACharacter::
		// CheckJumpInput does not increment JumpCurrentCount for a DoJump that returned false.
		if (bWasAirborneBeforeJump && IsWallJumpEnabled())
		{
#if !UE_BUILD_SHIPPING
			// SPEC v8 §7, the anti-ladder cap, counted. A press that had a live wall under it and was
			// refused ONLY by the consecutive cap is the thing that stops two walls being an infinite
			// staircase, so it is worth its own number rather than being invisible inside "refused".
			const bool bCapRefusal = WallJumpWindowRemaining > 0.f
				&& !WallJumpNormal.IsNearlyZero()
				&& WallJumpsSinceGround >= GetWallJumpMaxConsecutive()
				&& CharacterOwner != nullptr && !CharacterOwner->bClientUpdating
				&& CharacterOwner->IsLocallyControlled();
			if (bCapRefusal)
			{
				++WallJumpCapRefusals;
			}
#endif

			if (TryWallJump())
			{
				return true;
			}

			// SPEC v10 §5, CAUSE 2 — REMEMBER THE PRESS THAT ARRIVED ONE TICK EARLY.
			//
			// THE ORDERING BUG THIS CLOSES. ACharacter::JumpMaxHoldTime is 0, so bPressedJump lives for
			// exactly one tick. CheckJumpInput (which calls this function) runs at the START of
			// PerformMovement; HandleImpact — the ONLY thing that opens a wall window — runs during the
			// physics step that follows it. So a press made on the tick before the capsule touches the
			// wall reaches here, is correctly refused, and is then gone forever. The player pressed jump
			// "right as they hit the wall", nothing happened, and they are left scraping down the face
			// waiting to press again. v9's shorter window made this the EASY way to miss, not a rare one.
			//
			// Buffering converts that dead press into a wall jump on the contact frame itself (see
			// OnMovementUpdated), which is also one whole frame earlier than the CheckJumpInput path can
			// ever deliver a launch.
			//
			// ONLY WHEN THE REFUSAL WAS "NO WALL YET". A press refused by the anti-ladder cap, or while
			// a window was open and something else declined it, must stay refused — buffering those
			// would turn the cap into a queue and hand back the infinite staircase spec v8 §7 removed.
			// The window itself still bounds the buffer's reach: OnMovementUpdated only ever spends it
			// through IsWallJumpAvailable(), which re-checks the cap, the mode and the mantle.
			if (WallJumpNormal.IsNearlyZero()
				&& WallJumpWindowRemaining <= 0.f
				&& WallJumpsSinceGround < GetWallJumpMaxConsecutive())
			{
				WallJumpInputBufferRemaining = GetWallJumpInputBufferSeconds();
			}

			// Not a wall jump. Undo Super's vertical launch and refuse — otherwise every mid-air press
			// would be a free jump, because the extra JumpMaxCount exists only to let the engine ask.
			// The pawn was already airborne, so Super's SetMovementMode(MOVE_Falling) was a no-op and
			// the velocity is the only thing to put back.
			Velocity = PreJumpVelocity;
			return false;
		}

		// SPEC v26 §9 — Jump, client-side. THE ORDINARY / AIR-JUMP EXIT.
		//
		// Deliberately NOT immediately after Super::DoJump: a wall jump leaves through the
		// `if (TryWallJump()) return true;` above, which already played WallJump, and layering the two
		// would make every wall jump a chord. The two exits that reach a sound are this one and the
		// slide-jump exit at the bottom of the function.
		//
		// bReplayingMoves is the double-play guard: a client correction re-runs DoJump on the owning
		// client, which is the only machine this sound plays on.
		if (!bReplayingMoves && CharacterOwner != nullptr && !CharacterOwner->bClientUpdating)
		{
			TraceAudio::Play(CharacterOwner, TraceSoundEvents::Jump);
		}

		return true;
	}

	// --- The payoff -------------------------------------------------------------------------------
	//
	// Retention 1.0 is PURE PRESERVATION, not a boost. What it buys the player is escaping the ground
	// friction that would otherwise have eaten the carry over the next second — which is exactly the
	// Apex slide-hop, and exactly why the flat entry boost is no longer needed to make sliding worth
	// doing.
	const float Retention = GetSlideJumpHorizontalRetention()
		* (bWellTimed ? GetSlideJumpWindowSpeedBonus() : 1.f);

	// The boost is already 20% weaker at this point: spec v26 §3a lives inside
	// GetSlideJumpWindowSpeedBonus(), one factor on the gain, so every reader of that function — this
	// line, the audit, the V9TUNING report, Elle's seam — sees the shipped number without being told.
	const float UncappedLaunchSpeed = CarrySpeed * Retention;

	// =============================================================================================
	// SPEC v26 §3b — THE CHAIN CEILING.
	//
	// "Add a ceiling to slide jump momentum boosts, so that you can't chain them over and over to go
	// faster and faster. Right now, if you do three slide jump boosts in a row you can zip down the
	// whole field. For now, lets cap it at what the momentum is after you do two consecutive slide
	// boosts."
	//
	// WHY IT COMPOUNDS AT ALL, since the block above insists retention 1.0 is not a boost. Because
	// StartSlide takes the speed you ARRIVE with (spec v4 §1, "entry speed determines slide
	// velocity", with an outer max() that deliberately refuses to brake a fast arrival) and the
	// well-timed hop multiplies it. So a landed hop feeds the next slide, which feeds the next hop,
	// and the whole loop is geometric. With the shipped 0.66 s slide bleeding 260 uu/s² the recurrence
	// is v -> (v - 172) x 1.3575, whose fixed point is 652 uu/s: above that — and WalkSpeed is 800 —
	// every chained hop is faster than the last, forever. That is the "zip down the whole field".
	//
	// *** THE CEILING IS ONE OF THE CHAIN'S OWN LAUNCHES, RECORDED. NOT A FORMULA. ***
	// The formula version (entry x multiplier^N) is wrong here and quietly so: a slide DECAYS while
	// the player waits for the well-timed window, so the momentum actually reached after two boosts
	// depends on how long each of their two slides ran — information no expression on this line has.
	// Keeping the chain's own highest launch answers the note's sentence literally, and it is
	// RELATIVE for free: it already contains the boost knobs, the character's passive and whatever
	// speed the player brought in, so it moves when any of those move. There is no absolute uu/s
	// anywhere in this mechanic.
	//
	// WHAT A CAPPED HOP STILL GETS, because "a third and fourth may still be performed" is a
	// requirement and not a courtesy: it still ENDS the slide through the one exit, still leaves the
	// ground with the launch direction it earned, still escapes the ground friction that would have
	// eaten the carry, and still collects the well-timed VERTICAL bonus below. Only the planar
	// MAGNITUDE is clamped. A player chaining a fourth hop goes exactly as far as their second did.
	//
	// The counter and the ceiling are saved-move state (see FSavedMove_Trace) and the chain is ended
	// in OnMovementUpdated the moment the pawn is back on its feet at or below
	// GetSlideJumpChainResetSpeed() — i.e. the moment they have given the momentum back.
	// =============================================================================================
	float LaunchSpeed = UncappedLaunchSpeed;
	const bool bChainCapOn = IsSlideJumpChainCapEnabled();

	if (bChainCapOn)
	{
		const int32 CapBoosts = GetSlideJumpChainCapBoosts();

		if (SlideJumpChainBoosts >= CapBoosts && SlideJumpChainCeiling > 0.f)
		{
			// Past the cap. Min, never Clamp against the ceiling: a hop that was ALREADY slower than
			// the ceiling is left exactly where it was. The ceiling removes speed a chain has not
			// earned; it never hands any back, or a mistimed fourth hop would be worth more than a
			// mistimed first one.
			LaunchSpeed = FMath::Min(LaunchSpeed, SlideJumpChainCeiling);

			// *** AND IT MAY NEVER BRAKE. *** The note is "a third and fourth may still be performed,
			// they just must not go FASTER" — not "they must be slowed down". Without this floor the
			// ceiling would confiscate momentum the CHAIN never created: a player who dashes mid-chain
			// (or is launched by Rocco's Ripple, or arrives off a wall jump) can be carrying more than
			// their own two-boost ceiling, and the clamp above would take the difference off them for
			// the crime of pressing crouch. That is a brake with no sentence in the note behind it, and
			// it is exactly the failure spec v4 §1 removed from the slide's exit rule (see EndSlide's
			// "THE OUTER max() IS NOT A BOOST" note — same shape, same argument, same fix).
			//
			// With the floor, a capped hop is PURE PRESERVATION: you keep what you brought, the
			// slide-jump adds nothing, and nothing compounds — the launch can only track a speed some
			// other mechanic already gave you, never multiply it.
			LaunchSpeed = FMath::Max(LaunchSpeed, CarrySpeed);
		}
	}
	else
	{
		// The A/B arm, or the designer's switch. Nothing accumulates while the ceiling is off, so
		// turning it back on mid-session starts from a clean chain rather than from a stale ceiling.
		SlideJumpChainBoosts = 0;
		SlideJumpChainCeiling = 0.f;
	}

	FVector LaunchDirection(Velocity.X, Velocity.Y, 0.f);
	if (!LaunchDirection.Normalize())
	{
		LaunchDirection = CarryDirectionFallback;
		LaunchDirection.Z = 0.f;
		if (!LaunchDirection.Normalize())
		{
			LaunchDirection = (UpdatedComponent != nullptr)
				? UpdatedComponent->GetForwardVector()
				: FVector::ForwardVector;
			LaunchDirection.Z = 0.f;
			if (!LaunchDirection.Normalize())
			{
				LaunchDirection = FVector::ForwardVector;
			}
		}
	}

	Velocity.X = LaunchDirection.X * LaunchSpeed;
	Velocity.Y = LaunchDirection.Y * LaunchSpeed;

	// Z has just been set to JumpZVelocity by Super. Scaling rather than assigning keeps this honest
	// on the multi-frame path: with a non-zero JumpMaxHoldTime the engine calls DoJump on several
	// consecutive frames, and the slide is only alive for the first of them, so only the first can be
	// a slide-jump. A second scaling cannot happen because bSlideJump is false by then.
	//
	// SPEC v5 §3 adds the well-timed VERTICAL bonus on top. Two independent multipliers rather than
	// one bigger speed number, because they buy different things: the speed bonus makes the hop go
	// further, the Z bonus makes it go higher — and it is the height a player actually perceives, so
	// this is what makes a well-timed slide-jump read as a different move rather than a slightly
	// better one. Both are collected or neither is; missing the window still costs nothing.
	Velocity.Z *= GetSlideJumpZMultiplier() * (bWellTimed ? GetSlideJumpWindowZBonus() : 1.f);

	// Consumed. One slide, one slide-jump.
	SlideJumpGraceRemaining = 0.f;
	bSlideJumpGraceWellTimed = 0;

	// SPEC v26 §3b — book the hop into the chain, AFTER the launch is applied.
	//
	// The order matters and is the reason this is not folded into the clamp above: the ceiling is
	// recorded from the speed the pawn ACTUALLY LEFT AT, which for the capped hops is the ceiling
	// itself. Recording the uncapped value instead would let the ceiling drift upward on every extra
	// hop — the very compounding this whole block removes, wearing the cap's hat.
	//
	// THE HIGHEST OF THE FIRST N, not the Nth. In the intended case they are the same number (each
	// well-timed hop is faster than the last, so the highest IS the last). They differ only when a
	// player MISTIMES one of the first N, and taking the highest is the kinder and the more honest
	// answer there: a fumbled second hop then cannot pin the rest of the chain below what the first
	// one already achieved.
	if (bChainCapOn)
	{
		++SlideJumpChainBoosts;
		if (SlideJumpChainBoosts <= GetSlideJumpChainCapBoosts())
		{
			SlideJumpChainCeiling = FMath::Max(SlideJumpChainCeiling, LaunchSpeed);
		}
	}

#if !UE_BUILD_SHIPPING
	// Observation only, and on the authority alone so a client replaying corrections cannot count the
	// same hop several times. At Display, behind the same switch as the slide measurement.
	if (IsSlideDebugEnabled() && CharacterOwner != nullptr && CharacterOwner->HasAuthority())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("SLIDEJUMP %-16s carried=%6.0f -> launched=%6.0f uu/s (%.1f%%, retain=%.2f%s) "
			     "velZ=%6.0f (jumpZ=%.0f x zMul=%.2f x zBonus=%.2f)"),
			*GetNameSafe(CharacterOwner), CarrySpeed, GetPlanarSpeed(),
			100.f * GetPlanarSpeed() / FMath::Max(1.f, CarrySpeed), Retention,
			bWellTimed ? TEXT(", WELL TIMED") : TEXT(""),
			Velocity.Z, JumpZVelocity, GetSlideJumpZMultiplier(),
			bWellTimed ? GetSlideJumpWindowZBonus() : 1.f);

		// SPEC v26 §3b, its own line rather than more columns on the one above: this is the number the
		// owner's complaint is about, and "boost 3 of this chain was cut from X to Y" has to be
		// readable on its own in a log full of hops. Printed for EVERY hop, capped or not, because
		// "the cap did not fire" and "there was no third hop" are different facts.
		UE_LOG(LogTraceGame, Display,
			TEXT("SLIDEJUMPCAP %-16s boost #%d of chain  uncapped=%6.0f -> launched=%6.0f uu/s  "
			     "ceiling=%6.0f (cap at %d boosts, reset at %5.0f uu/s) %s"),
			*GetNameSafe(CharacterOwner), SlideJumpChainBoosts, UncappedLaunchSpeed, LaunchSpeed,
			SlideJumpChainCeiling, GetSlideJumpChainCapBoosts(), GetSlideJumpChainResetSpeed(),
			!bChainCapOn ? TEXT("CAP OFF (v26 legacy arm)")
			             : (LaunchSpeed < UncappedLaunchSpeed - 1.f ? TEXT("*** CAPPED ***") : TEXT("")));
	}
#endif

	// SPEC v26 §9 — Jump, client-side. THE SLIDE-JUMP EXIT, the second of DoJump's two sounded exits.
	// Same guard, same reason as the ordinary exit above.
	if (!bReplayingMoves && CharacterOwner != nullptr && !CharacterOwner->bClientUpdating)
	{
		TraceAudio::Play(CharacterOwner, TraceSoundEvents::Jump);
	}

	return true;
}

void UTraceCharacterMovementComponent::ApplyDashExitSpeed()
{
	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
	const float PlanarLimit = FMath::Max(1.f, GetMaxSpeed()) * GetDashExitSpeedMultiplier();
	if (PlanarVelocity.SizeSquared() > FMath::Square(PlanarLimit))
	{
		const FVector Clamped = PlanarVelocity.GetSafeNormal() * PlanarLimit;
		Velocity.X = Clamped.X;
		Velocity.Y = Clamped.Y;
	}

	// SPEC v7 §5, AND THE THING THAT KEEPS A VERTICAL DASH FROM BEING A ROCKET. The dash suspends
	// gravity for its window, so a straight-up one would hand back the whole DashSpeed as upward
	// velocity — 3300 uu/s (v16 §0), another 4960 uu of coast at the shipped 1.12 gravity scale, past
	// the arena's 1640 uu ceiling. THE AIR-STRAFE
	// CEILING CANNOT HELP HERE: it is planar-only by construction (see ApplySourceAirAcceleration,
	// which never touches Z) and so bounds nothing vertical at all.
	//
	// Same shape as the planar rule directly above: hand back a fast player, not a stationary one,
	// but hand back a bounded one. Downward Z is deliberately untouched — a dive is not a climb, and
	// clamping it would turn a downward dash into a float.
	const float VerticalLimit = GetDashExitVerticalSpeedLimit();
	if (Velocity.Z > VerticalLimit)
	{
		Velocity.Z = VerticalLimit;
	}
}

// =================================================================================================
// THE MANTLE (spec v5 §7) WAS HERE. IT IS GONE — spec v12 §5.
//
// Deleted in full: IsMantling(), CanAttemptMantle(), TryBeginMantle(), ApplyMantleVelocity() and
// EndMantle(), together with six pieces of saved-move state, eight tuning knobs, one CVar and the
// v9 §5 priority rule that existed only to keep it from eating wall jumps. Around 400 lines.
//
// WHAT REPLACES IT: nothing, and that is the point of the change. The Demo 5 report the mantle was
// written for — "when jumping on the edge of a raised section, it's glitchy and feels like rubber
// banding" — is a claim about client/server disagreement, and the two fixes that address it are
// PerchRadiusThreshold (set in the constructor) and the ledge grace (GroundGraceRemaining, kept
// below). Both are still here and both are untouched. The mantle was a third fix layered on top,
// and unlike the other two it changed where the pawn ENDS UP rather than only stabilising the
// agreement about where it is — which is why it could be removed without giving the bug back, and
// why -TraceLedgeTest was rewritten to prove that rather than assert it. See TickLedgeTest.
// =================================================================================================

bool UTraceCharacterMovementComponent::IsGroundedForAbilities() const
{
	return IsMovingOnGround() || GroundGraceRemaining > 0.f;
}

// --- Simulation --------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::OnMovementUpdated(float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity)
{
	Super::OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);

	if (CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// 0. Pick up config a designer has just changed in Project Settings. BeginPlay's copy is the one
	//    piece of cached config in this file, and caching is exactly what this file's own rules
	//    forbid — without this, retuning WalkSpeed during PIE did nothing until the map reloaded.
	RefreshEngineTunablesFromSettings();

	// 1. Advance every clock first, so an ability that expires this frame stops driving velocity
	//    this frame and a cooldown that expires this frame permits an activation this frame.
	const bool bWasDashing = (DashTimeRemaining > 0.f);
	if (bWasDashing)
	{
		DashTimeRemaining = FMath::Max(0.f, DashTimeRemaining - DeltaSeconds);
	}

	const bool bWasSliding = (SlideTimeRemaining > 0.f);
	if (bWasSliding)
	{
		SlideTimeRemaining = FMath::Max(0.f, SlideTimeRemaining - DeltaSeconds);
	}
	if (SlideCooldownRemaining > 0.f)
	{
		SlideCooldownRemaining = FMath::Max(0.f, SlideCooldownRemaining - DeltaSeconds);
	}
	if (SlideBufferRemaining > 0.f)
	{
		SlideBufferRemaining = FMath::Max(0.f, SlideBufferRemaining - DeltaSeconds);
	}
	if (SlideJumpGraceRemaining > 0.f)
	{
		// The slide-jump's coyote window. Ticked here with every other clock so a replayed move
		// advances it by exactly the same amount the original did.
		SlideJumpGraceRemaining = FMath::Max(0.f, SlideJumpGraceRemaining - DeltaSeconds);
		if (SlideJumpGraceRemaining <= 0.f)
		{
			bSlideJumpGraceWellTimed = 0;
		}
	}

	// 1a-0. SPEC v26 §3b — DOES THE SLIDE-JUMP CHAIN END THIS FRAME?
	//
	//       "Consecutive" is the word the note uses, and this is where it is defined: boosts are
	//       consecutive while the player never gives the momentum back. The instant they are on their
	//       feet at running pace again there is nothing left to compound, so the next slide-jump
	//       starts a fresh chain and gets its full boost.
	//
	//       A SPEED AND NOT A TIMER, and the second reason is the load-bearing one. (1) It is what
	//       "consecutive" means — a player who sprints on for three seconds at 1400 uu/s and then
	//       hops is still cashing in the same momentum. (2) A timer inside a client-predicted move is
	//       a prediction hazard: this function runs again, frame by frame, when the server corrects
	//       the client, and a rule keyed on world time would resolve differently on the replay than it
	//       did live. Planar speed is a pure function of state the saved move already carries, so both
	//       passes reach the same verdict.
	//
	//       ON ITS FEET means grounded, NOT sliding and NOT dashing, and all three tests are
	//       load-bearing rather than defensive. GetSlideJumpChainResetSpeed() reads GetMaxSpeed(),
	//       which folds SlideSpeed in while sliding and returns DashSpeed while dashing — asked at
	//       either of those moments it would compare an ability against itself. Worse, mid-slide the
	//       planar speed has decayed BELOW walking pace by design (0.66 s at 260 uu/s² off an 800 uu/s
	//       entry ends at 628), so a check that ran during the slide would end every chain one frame
	//       before the hop that is supposed to be capped.
	if (SlideJumpChainBoosts > 0
		&& IsMovingOnGround()
		&& !IsSliding()
		&& !IsDashing()
		&& GetPlanarSpeed() <= GetSlideJumpChainResetSpeed())
	{
#if !UE_BUILD_SHIPPING
		if (IsSlideDebugEnabled() && CharacterOwner != nullptr && CharacterOwner->HasAuthority())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("SLIDEJUMPCAP %-16s chain ENDED after %d boost(s) — back on foot at %5.0f uu/s "
				     "(reset threshold %5.0f). The next slide-jump starts fresh."),
				*GetNameSafe(CharacterOwner), SlideJumpChainBoosts, GetPlanarSpeed(),
				GetSlideJumpChainResetSpeed());
		}
#endif
		SlideJumpChainBoosts = 0;
		SlideJumpChainCeiling = 0.f;
	}

	// 1a-i. THE LEDGE GRACE (spec v5 §7). Refilled while grounded, bled while not, so
	//       IsGroundedForAbilities() lags the engine's own answer by LedgeGroundGraceSeconds on the
	//       way DOWN only — it can never claim the pawn is airborne when the engine says it is not.
	//       Ticked with every other clock so a replayed move advances it by the same amount.
	if (IsMovingOnGround())
	{
		GroundGraceRemaining = GetLedgeGroundGraceSeconds();
	}
	else if (GroundGraceRemaining > 0.f)
	{
		GroundGraceRemaining = FMath::Max(0.f, GroundGraceRemaining - DeltaSeconds);
	}

	// 1a-iii. THE WALL-JUMP CONTACT WINDOW (spec v8 §7). Ticked here with every other clock so a
	//         replayed move advances it by exactly the amount the original did, and cleared on the
	//         frame it expires so IsWallJumpAvailable() never has to re-test the normal's age.
	//
	//         THE LADDER CAP IS RESET BY THE GROUND, and through IsGroundedForAbilities() like every
	//         other ground test in this function — a one-frame contact blip on a ledge lip must not
	//         refill a player's wall jumps on one machine and not the other.
	if (WallJumpWindowRemaining > 0.f)
	{
		WallJumpWindowRemaining = FMath::Max(0.f, WallJumpWindowRemaining - DeltaSeconds);
		if (WallJumpWindowRemaining <= 0.f)
		{
			WallJumpNormal = FVector::ZeroVector;
			WallJumpEntryVelocity = FVector::ZeroVector;
		}
	}
	if (IsGroundedForAbilities() && WallJumpsSinceGround != 0)
	{
		WallJumpsSinceGround = 0;
	}

	// 1a-iv. SPEC v10 §5, CAUSE 1 — THE INTO-WALL INPUT LOCKOUT'S CLOCK.
	//
	//        Ticked here with every other clock so a replayed move advances it by exactly the amount
	//        the original did, and the face is dropped on the frame it expires so
	//        ApplySourceAirAcceleration never has to re-test the normal's age.
	//
	//        THE GROUND ENDS IT EARLY, and that is not cosmetic. Once the pawn is walking, the air
	//        strafe is not running at all, so a lockout still counting down would be invisible until
	//        the player left the ground again — at which point it would silently refuse an input
	//        direction on a jump that had nothing to do with any wall.
	if (WallJumpControlLockoutRemaining > 0.f)
	{
		WallJumpControlLockoutRemaining = FMath::Max(0.f, WallJumpControlLockoutRemaining - DeltaSeconds);
		if (WallJumpControlLockoutRemaining <= 0.f || IsGroundedForAbilities())
		{
			WallJumpControlLockoutRemaining = 0.f;
			WallJumpLaunchNormal = FVector::ZeroVector;
		}
	}

	// 1a-v. SPEC v10 §5, CAUSE 2 — SPEND THE BUFFERED PRESS.
	//
	//       ORDER IS THE WHOLE POINT AND IT IS LOAD-BEARING. This runs AFTER the window clock above,
	//       which means after HandleImpact has had its say for this frame — so a press buffered on the
	//       previous tick is converted into a launch on the very frame the capsule touched the wall,
	//       rather than waiting for the next CheckJumpInput.
	//
	//       TryWallJump() is self-contained: it writes both the planar launch and Velocity.Z itself and
	//       does not depend on anything Super::DoJump would have done first (the pawn is already
	//       MOVE_Falling — IsWallJumpAvailable() requires it). Skipping ACharacter's JumpCurrentCount
	//       bookkeeping is deliberate and harmless: those counts exist only to make the engine ASK
	//       (see RefreshEngineTunablesFromSettings), and the real cap is WallJumpsSinceGround, which
	//       TryWallJump increments.
	//
	//       Spent before it is ticked, so a buffer charged this frame is live for its full length.
	//
	//       NOT INTO A DASH. The dash owns the velocity vector outright and re-asserts it a few lines
	//       below, so a wall jump spent here would be silently overwritten on the same frame — the
	//       press would be consumed and the player would get nothing. The clock keeps running, so a
	//       press made just before a short dash can still land after it if the wall and the clock both
	//       survive.
	if (WallJumpInputBufferRemaining > 0.f)
	{
		if (DashTimeRemaining <= 0.f && IsWallJumpAvailable() && TryWallJump())
		{
			WallJumpInputBufferRemaining = 0.f;

#if !UE_BUILD_SHIPPING
			// SPEC v18 §1b. THE ONE PLACE IN THIS COMPONENT THAT LAUNCHES A PAWN WITH NO PRESS ON THE
			// FRAME IT LAUNCHES, counted so "velocity appears mid-jump with no input" can be answered
			// with a number instead of an argument. It is not a bug by itself — the press was real, one
			// or two frames earlier, and buffering it is the whole of v10 §5 CAUSE 2 — but it is the
			// only candidate mechanism, so it has to be countable. Observation only.
			++WallJumpBufferedLaunches;
#endif
		}
		else
		{
			// The clock runs whatever happened above, INCLUDING through a dash — a buffer frozen for
			// the length of a dash would be a buffer of unbounded length, which is the one property
			// GetWallJumpInputBufferSeconds() exists to bound.
			WallJumpInputBufferRemaining = FMath::Max(0.f, WallJumpInputBufferRemaining - DeltaSeconds);

			// Landing spends it too. A remembered air press must not survive a touchdown and fire off
			// the first wall of the NEXT jump, which the player would read as a wall jump they did not
			// press — the mirror image of the bug this buffer fixes, and a worse one.
			if (IsGroundedForAbilities())
			{
				WallJumpInputBufferRemaining = 0.f;
			}
		}
	}

	// =============================================================================================
	// 1a-vi. PATCH 28 §5 — THE SURF CLOCK, THE BOUND, AND THE EXIT.
	//
	// Ticked here with every other clock so a replayed move advances it by exactly the amount the
	// original did. Nothing in this block is the surf itself: the ride happens inside PhysFalling,
	// through ComputeSlideVector(), on the frame it happens. This is the bookkeeping around it.
	// =============================================================================================
	if (IsSurfing())
	{
		SurfElapsedSeconds += DeltaSeconds;

		// THE BOUND. Gravity along the plane is a constant acceleration and the clip never removes any
		// of it, so a long enough ramp would grow the vector without limit — which spec v5 §1 already
		// ruled out for the air model in as many words ("too powerful with how much momentum can be
		// gained"). GetSurfSpeedCeiling() is max(entry speed, air hard cap x multiplier): DERIVED from
		// a number this file already ships, and floored at what the player brought so arriving fast is
		// never punished.
		//
		// SCALED, NOT COMPONENT-CLAMPED, and on the FULL 3D vector. Clamping components would bend the
		// trajectory into the ramp; scaling keeps the direction the clip produced and only shortens it.
		// 3D rather than planar because on a steep face a large part of the speed is vertical, and a
		// planar-only bound would leave the one axis a ramp is best at growing completely unbounded.
		const float SurfSpeed = static_cast<float>(Velocity.Size());
		const float SurfCeiling = GetSurfSpeedCeiling();
		if (SurfSpeed > SurfCeiling + TraceMoveCfg::SpeedEpsilon && SurfSpeed > UE_KINDA_SMALL_NUMBER)
		{
			Velocity *= (SurfCeiling / SurfSpeed);
#if !UE_BUILD_SHIPPING
			++SurfCeilingBinds;
#endif
		}

		SurfPeakSpeed = FMath::Max(SurfPeakSpeed, static_cast<float>(Velocity.Size()));

		// DEMO 29 ITEM 4(a). THE RIDE'S OWN SPEED, kept current every frame so that the frame the ride
		// ENDS this already holds what it was worth. It is the cap on the exit rollout in
		// ProcessLanded(), and it is deliberately the 3D speed rather than the planar one: on a 47-61
		// degree face most of a descent is vertical, and the vertical part is precisely what the
		// landing throws away and what the rollout puts back.
		//
		// Latched here rather than at the close because by the time the close is detected the landing
		// has already happened and MaintainHorizontalGroundVelocity() has already deleted the Z.
		SurfExitSpeed = static_cast<float>(Velocity.Size());
	}

	// DEMO 29 ITEM 4(a) — THE EXIT CARRY CLOCK.
	//
	// Armed when a ride ends, re-armed by ProcessLanded() on the touchdown that spends the rollout (so
	// a long flight off the end of a rail cannot eat the floor carry it earned), and read by
	// ApplyGroundOverspeedBleed(), which holds the bleed off while it runs. Ticked here with every
	// other clock so a replayed move advances it by exactly the amount the original did.
	//
	// SurfExitSpeed dies with it. The two are one fact — "there is a ride's momentum in flight" — and
	// leaving a stale cap behind after the window closed would let a much later landing roll a descent
	// that had nothing to do with a ramp into the floor.
	if (SurfExitCarryRemaining > 0.f)
	{
		SurfExitCarryRemaining = FMath::Max(0.f, SurfExitCarryRemaining - DeltaSeconds);
		if (SurfExitCarryRemaining <= 0.f)
		{
			SurfExitSpeed = 0.f;
		}
	}

	if (SurfContactRemaining > 0.f)
	{
		SurfContactRemaining = FMath::Max(0.f, SurfContactRemaining - DeltaSeconds);

		// THE GROUND ENDS IT, and through IsGroundedForAbilities() like every other ground test in this
		// function. Two reasons, and the second is the load-bearing one:
		//   * a rail that flattens into walkable ground is the CLEAN EXIT, and the pawn is a walker
		//     again the instant it lands — leaving a surf clock running over a landing would apply the
		//     surf ceiling to a grounded frame, which is a different mechanic's business;
		//   * it is what keeps "surf is live" implying "this move is airborne", which is what lets
		//     CanCombineWith's existing momentum test cover the whole of surf for free.
		if (SurfContactRemaining <= 0.f || IsGroundedForAbilities())
		{
#if !UE_BUILD_SHIPPING
			// THE SAMPLE CLOSES HERE, and the exit speed is read before anything else touches Velocity.
			// "Accelerate using curved ramps" is entry -> exit, and this is the only place both ends of
			// that pair exist. Never on a replayed move: a correction replays several frames and would
			// close the same ride several times, which is how a ledger invents rides that never
			// happened.
			if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && SurfCount > 0)
			{
				const float ExitSpeed = GetPlanarSpeed();
				const float Gain = ExitSpeed - SurfEntrySpeed;
				SurfExitSpeedSum += ExitSpeed;
				SurfBestGain = (SurfClosedCount == 0) ? Gain : FMath::Max(SurfBestGain, Gain);
				SurfWorstGain = (SurfClosedCount == 0) ? Gain : FMath::Min(SurfWorstGain, Gain);
				SurfLongestSeconds = FMath::Max(SurfLongestSeconds, SurfElapsedSeconds);
				++SurfClosedCount;
			}
#endif
			SurfContactRemaining = 0.f;
			SurfPlaneNormal = FVector::ZeroVector;
			SurfElapsedSeconds = 0.f;
			SurfEntrySpeed = 0.f;

			// DEMO 29 ITEM 4(a). ARM THE CARRY. Max, not assignment, because ProcessLanded may already
			// have armed it a few microseconds earlier on this very frame — a ride that ends BY landing
			// goes through ProcessLanded first — and re-arming it here would silently extend that
			// window by a frame's worth of clock every time.
			//
			// Not gated on the legacy arm: IsSurfExitCarryActive() and the rollout are, so the clock
			// running is inert there, and a clock that only exists in one arm is a clock whose saved
			// move differs between arms.
			SurfExitCarryRemaining = FMath::Max(SurfExitCarryRemaining, GetSurfExitCarrySeconds());

			// SurfPeakSpeed is deliberately NOT cleared here: it is the ride's own high-water mark and
			// the report reads it after the ride is over. It is re-seeded by the next entry.
			// SurfExitSpeed is not cleared either: it IS the exit, and the rollout at the next landing
			// is what spends it. The clock above clears it if no landing arrives in time.
		}
	}

	// 1a-ii. The mantle clock and its cooldown were advanced here (spec v5 §7). Gone in v12 §5.

	// 1b. Resize the charge pool from the TRANSITION in GetMaxDashCharges(), never from its value.
	//     Picking the Core up must hand the extra charge over immediately — a carrier who has to
	//     wait out a cooldown before their bonus dash exists does not have a bonus dash during the
	//     four seconds that decide the run. Losing the Core takes exactly one back, and can never
	//     take back a charge that was not granted by carrying.
	{
		const int32 MaxCharges = GetMaxDashCharges();
		if (MaxCharges != LastMaxDashCharges)
		{
			if (MaxCharges > LastMaxDashCharges)
			{
				DashCharges += (MaxCharges - LastMaxDashCharges);
			}
			LastMaxDashCharges = MaxCharges;
		}
		DashCharges = FMath::Clamp(DashCharges, 0, MaxCharges);

		// 1c. Refill. One timer, restarted while the pool is still short, so charges come back
		//     sequentially rather than all at once.
		if (DashCharges >= MaxCharges)
		{
			DashRechargeRemaining = 0.f;
		}
		else if (DashRechargeRemaining > 0.f)
		{
			DashRechargeRemaining = FMath::Max(0.f, DashRechargeRemaining - DeltaSeconds);
			if (DashRechargeRemaining <= 0.f)
			{
				DashCharges = FMath::Min(DashCharges + 1, MaxCharges);
				if (DashCharges < MaxCharges)
				{
					DashRechargeRemaining = GetDashRechargeWindow();
				}
			}
		}
		else
		{
			// Short a charge with no clock running (e.g. the pool shrank and grew again). Start one
			// rather than stranding the player a charge down forever.
			DashRechargeRemaining = GetDashRechargeWindow();
		}
	}

	// 1d. The frame a dash ends, hand the player back at DashExitSpeedMultiplier x the ground limit
	//     rather than AT the ground limit. See ApplyDashExitSpeed().
	if (bWasDashing && DashTimeRemaining <= 0.f)
	{
#if !UE_BUILD_SHIPPING
		const FVector PreExitVelocity = Velocity;
#endif
		ApplyDashExitSpeed();

#if !UE_BUILD_SHIPPING
		if (IsDashDebugEnabled())
		{
			// The Z column is the spec v7 §5 exit clamp doing its job: a vertical dash arrives here
			// carrying the whole DashSpeed upward and must leave carrying at most JumpZVelocity.
			UE_LOG(LogTraceGame, Display,
				TEXT("DASH %s  exit: pre=(%7.1f,%7.1f,%7.1f) post=(%7.1f,%7.1f,%7.1f) "
				     "planarLimit=%6.1f zLimit=%6.1f mode=%d"),
				*GetNameSafe(CharacterOwner),
				PreExitVelocity.X, PreExitVelocity.Y, PreExitVelocity.Z,
				Velocity.X, Velocity.Y, Velocity.Z,
				FMath::Max(1.f, GetMaxSpeed()) * GetDashExitSpeedMultiplier(),
				GetDashExitVerticalSpeedLimit(), static_cast<int32>(MovementMode.GetValue()));
		}
#endif

		// SPEC v14 §6. The closing edge, on the same terms as the opening one in BeginDash().
		//
		// bReachedFullDistance is reported as TRUE because this branch is the dash CLOCK running out,
		// which is the only way a dash ends in this component — there is no early cancel. If one is
		// ever added, it must pass false here, and Oyster's OnDashEnded (which clears the "a jar has
		// been dropped for this dash" latch) must still be called on that path or his next dash
		// silently drops no jar.
		if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && TraceAbilityIntegration::IsEnabled())
		{
			if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(CharacterOwner))
			{
				++TraceAbilityIntegration::Counters().DashEnded;
				Abilities->NotifyDashEnded(/*bReachedFullDistance=*/true);
			}
		}
	}

#if !UE_BUILD_SHIPPING
	// 1d-ii. SPEC v24 §8 — WHEN DOES THE BONUS WINDOW ACTUALLY OPEN?
	//
	// Sampled HERE, after every clock has advanced and before any of this frame's three EndSlide()
	// routes (the expiry immediately below, the airborne exit and the decay exit further down), so
	// the last live frame of the slide is included and the answer is a runtime transition rather
	// than "duration minus window" restated. Observation only: nothing below writes simulation state
	// and it is on the same switch, the same authority test and the same "never on a replayed move"
	// footing as the slide measurement in EndSlide().
	//
	// SlideSpeed is this frame's clock against LAST frame's speed (the decay step runs later in this
	// same function), which matters only for a slide that ends by DECAY rather than by the clock —
	// there the open time can read one frame (~16 ms) late. Every slide in the shipped tuning ends on
	// the clock; the log line says which, so a decay-ended sample can be discarded.
	if (bWasSliding && IsSliding() && IsSlideDebugEnabled() && SlideDebugStartTime > 0.f
		&& CharacterOwner != nullptr && CharacterOwner->HasAuthority() && GetWorld() != nullptr)
	{
		const bool bOpenNow = IsSlideJumpWellTimed();
		if (bOpenNow && !bSlideWindowWasOpen)
		{
			SlideWindowOpenTime = FMath::Max(0.f,
				static_cast<float>(GetWorld()->GetTimeSeconds()) - SlideDebugStartTime);
		}
		bSlideWindowWasOpen = bOpenNow;
	}
#endif

	// 1e. A slide whose duration has just run out exits through EndSlide() like every other slide
	//     exit, so it KEEPS its momentum instead of being clamped. Routing the timer expiry through
	//     the same function as the key release is what makes "hold the slide out" and "cancel it
	//     early" cost the same, which is what stops one of them becoming the only correct play.
	if (bWasSliding && SlideTimeRemaining <= 0.f)
	{
		EndSlide();
	}

	// 2. Activations, in priority order. Dash first: it is the mechanic the whole game is built
	//    around and it must never be eaten by another ability in the same frame.
	const bool bCrouchHeld = IsCrouchHeld();
	const bool bSlidePressedThisMove = bCrouchHeld && (bSlideHeldLastMove == 0);

	if (bWantsToDash && CanDash())
	{
		BeginDash();
	}
	else if (DashTimeRemaining > 0.f)
	{
		// Re-assert the locked velocity every frame. CalcVelocity applies friction and braking
		// against whatever the player is (or is not) holding, and without this the dash would
		// decay toward the walk speed and, worse, decay by a different amount on a replayed frame.
		//
		// SPEC v7 §5: all three axes now, through the same writer the launch uses. Re-asserting Z is
		// what makes a vertical dash a straight line for its window instead of a gravity arc — the
		// same "on rails" property the horizontal dash has always had, extended to the axis the dash
		// is now allowed to use. Gravity resumes the instant the window closes.
		ApplyDashVelocity();
	}

	// 3. Crouch: slide on the ground, fast-fall in the air. One key, resolved by where the pawn is.
	//
	//    A press that cannot be honoured yet (mid-dash, or airborne) is buffered rather than thrown
	//    away — see SlideBufferRemaining. The buffer is charged from the press EDGE only, so holding
	//    the key can never chain slides. It is also what makes "air-strafe, then slide the instant
	//    you touch down" a single input instead of a frame-perfect one.
	//
	//    LEDGE GRACE (spec v5 §7): every ground test in this section goes through
	//    IsGroundedForAbilities() rather than IsMovingOnGround(). A capsule crossing the lip of a
	//    raised section loses and regains contact for a frame or two, and the client and the server
	//    do it on different frames — so the raw test made one machine fire a landing (or a fast-fall,
	//    or an EndSlide) that the other never did. The grace is saved-move state, so a replay
	//    resolves the blip exactly as the original did.
	const bool bOnGroundNow = IsGroundedForAbilities();
	const bool bJustLanded = bOnGroundNow && (bWasAirborneLastMove != 0);

	if (bSlidePressedThisMove)
	{
		SlideBufferRemaining = FMath::Max(0.f, Settings.SlideInputBufferSeconds);
	}
	else if (bJustLanded && bCrouchHeld)
	{
		// LANDING WITH CROUCH HELD IS A SLIDE (spec v3 §2.4, jump->slide).
		//
		// The press edge alone cannot express this. A crouch pressed in the air is consumed by the
		// fast-fall, which zeroes the buffer on purpose so that one press does not silently mean two
		// things; and the buffer is a quarter of a second while a jump is over a second. So a player
		// who holds crouch from the apex all the way down used to land, keep nothing, and have to
		// re-press — measured: a 1293 uu/s landing produced no slide at all.
		//
		// Charging the buffer on the landing TRANSITION fixes that without reopening the "one press,
		// two meanings" problem: the key is still held, the player is still asking, and it can fire
		// only once per landing because the next move is no longer a transition.
		SlideBufferRemaining = FMath::Max(SlideBufferRemaining, FMath::Max(0.f, Settings.SlideInputBufferSeconds));
	}

	if (SlideTimeRemaining > 0.f)
	{
		// --- Maintain an active slide -----------------------------------------------------------
		//
		// SPEC v5 §3: THE KEY NO LONGER ENDS A SLIDE. One press bought SlideDuration seconds and the
		// player gets all of them — releasing crouch, or a bot's hold timer expiring, is now simply
		// ignored, which is what "trigger once, like an ability" means. The old
		// `(!bCrouchHeld && !bCommitted)` clause and the partial commit window it needed are both
		// gone; every slide is committed for its whole length.
		//
		// ONE EXIT LEFT HERE: leaving the ground, because a slide is a ground state and the floor is
		// what it is sliding on. Through IsGroundedForAbilities(), not IsMovingOnGround(), so a
		// one-frame contact blip on a ledge lip cannot amputate a slide on one machine and not the
		// other — see the ledge diagnosis in the header. (The other exits live elsewhere and are
		// unchanged: the duration expiring, the decay below, a dash, and a slide-jump.)
		if (!IsGroundedForAbilities())
		{
			EndSlide();
		}
		else
		{
			// Weak steering. A slide you cannot aim at all is unusable in a corridor; a slide you
			// can steer freely is just fast walking. SlideTurnRateDegrees is the dial.
			FVector Desired = Acceleration;
			Desired.Z = 0.f;
			if (Desired.Normalize())
			{
				SlideDirection = TraceMovement::SteerTowards(
					SlideDirection, Desired, FMath::Max(0.f, Settings.SlideTurnRateDegrees) * DeltaSeconds);
			}

			// The friction dial. Small on purpose: the slide is meant to be ended by SlideDuration,
			// not by having bled itself back down to a walk two thirds of the way through.
			SlideSpeed = FMath::Max(0.f, SlideSpeed - GetSlideDeceleration() * DeltaSeconds);

			const float FloorSpeed = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideExitSpeedFraction);
			if (SlideSpeed <= FloorSpeed)
			{
				// Decayed back to walking pace: stop rather than drag the player along at a speed the
				// normal movement code would have given them anyway.
				//
				// This is an EXIT CONDITION, not a floor on speed, and the distinction is the whole of
				// spec v4 §1. It ends the slide when the slide has stopped being worth anything; it
				// does not hand the player a single unit they did not already have. EndSlide() used to
				// then lift them back to WalkSpeed on the way out (SlideExitMinSpeedFraction), which
				// DID contradict "entry speed determines slide velocity" — that is deleted, so a slide
				// that decays out now leaves the player at ~SlideExitSpeedFraction of the walk speed
				// and they re-accelerate normally, exactly as if they had never pressed crouch.
				EndSlide();
			}
			else
			{
				Velocity.X = SlideDirection.X * SlideSpeed;
				Velocity.Y = SlideDirection.Y * SlideSpeed;
				if (IsMovingOnGround())
				{
					Velocity.Z = 0.f;
				}
			}
		}
	}
	else if (bSlidePressedThisMove && !bOnGroundNow && MovementMode != MOVE_None)
	{
		// --- FAST-FALL (contract §5) -------------------------------------------------------------
		// Zero POSITIVE Z only, leave horizontal speed alone. This is a fall you chose, not a stop:
		// cutting a jump short to drop behind cover or to beat a shot is the whole point, so the
		// horizontal carry must survive — and with the Source air model that carry can now be well
		// above walking pace, which is exactly the state a fast-fall wants to bring to the floor.
		if (Velocity.Z > 0.f)
		{
			Velocity.Z = 0.f;
		}

		// One press, one meaning. Without this the same press would also slide the instant the pawn
		// landed, which is a surprise rather than a combo.
		SlideBufferRemaining = 0.f;
	}
	else if (SlideBufferRemaining > 0.f && CanStartSlide())
	{
		// CanStartSlide() already refuses while dashing, off the ground, on cooldown, below the
		// entry speed, or with the key released.
		BeginSlide();
		SlideBufferRemaining = 0.f;
	}

	// 3b. THE MANTLE WAS ACTIVATED HERE (spec v5 §7), last of the activations, attempted on every
	//     airborne move. Deleted in v12 §5, and with it the whole spec v9 §5 priority block that used
	//     to sit above this line ("THE WALL JUMP OUTRANKS THE MANTLE") plus the WallJumpMantleSteals
	//     counter that measured how often the mantle won anyway.
	//
	//     WHY THE PRIORITY CODE GOING AWAY IS SAFE, since that is the one thing the removal could
	//     plausibly break. The rule existed because two mechanics wanted the same frame: the mantle
	//     needed no input and ran HERE, at the end of the move, while the wall jump needed a press
	//     that PerformMovement delivers through CheckJumpInput EARLIER in the same move. So the
	//     mantle could claim a contact before the press for it had arrived, and IsWallJumpAvailable()
	//     then refused for the rest of the contact. With the mantle deleted there is exactly one
	//     consumer of a wall contact left, so there is no race to arbitrate — the priority rule is
	//     not disabled, it is unnecessary. HandleImpact still latches the face on the frame the
	//     capsule touches it, TryWallJump() still reads it from DoJump, and neither now has a clause
	//     that any other ability can make false.

	// 4. Latch the last instant this pawn was inside its dash window, on the authority only.
	//    The trail's trip test ticks once per SERVER frame, but the server advances a remote
	//    client's dash clock here inside MoveAutonomous - possibly several client moves deep in a
	//    single server frame. Without this latch, a dash that starts and finishes between two trail
	//    ticks credits its displacement to the sweep while IsDashing() already reads false, and the
	//    single most important mechanic in the game silently no-ops under jitter.
	if (DashTimeRemaining > 0.f && CharacterOwner->HasAuthority())
	{
		if (const UWorld* DashWorld = GetWorld())
		{
			LastDashActiveWorldTime = static_cast<float>(DashWorld->GetTimeSeconds());
		}
	}

	// 4b. SPEC v14 §6 — THE PER-FRAME DASH CONTACT SWEEP. The last unwired ability hook.
	//
	//     Chut's bash was driven only by his own 20 Hz poll on the ability component's tick. A dash
	//     covers 150 uu between two of those samples and the bash reach is 130 uu, so a victim could
	//     be passed clean through between samples; his poll closes that with a swept segment, which
	//     is a correct approximation of a sweep the mover was in a position to do exactly. This is
	//     that exact sweep, once per movement frame, on the frames it can matter.
	//
	//     CHARACTER-AGNOSTIC BY CONSTRUCTION. This block knows a radius and nothing else — no
	//     character, no knob, no rule. GetDashHitSweepRadiusFor() returns 0 for every Mannequin,
	//     every bot and four of the five characters, and 0 skips the whole block, so the cost for
	//     everyone who is not Chut is one virtual call per dash frame. What to DO with a contact
	//     (the end-of-dash window, the reach, the §4 choke point, one-bash-per-victim) stays in the
	//     character's OnDashHitCharacter and is not duplicated here.
	//
	//     AUTHORITY ONLY, and not on a replayed move: a knockback is server truth, and a correction
	//     replaying five frames must not deliver five contacts. The character's apply path is
	//     idempotent per victim per dash anyway — belt and braces, deliberately, because this
	//     project has shipped a "wired" hook that fired twice.
	if (DashTimeRemaining > 0.f && CharacterOwner != nullptr && CharacterOwner->HasAuthority()
		&& !CharacterOwner->bClientUpdating && TraceAbilityIntegration::IsEnabled())
	{
		const float SweepRadius = UTraceAbilityComponent::GetDashHitSweepRadiusFor(CharacterOwner);
		if (SweepRadius > 0.f)
		{
			UTraceAbilityComponent* DashAbilities = UTraceAbilityComponent::Get(CharacterOwner);
			UWorld* SweepWorld = GetWorld();
			if (DashAbilities != nullptr && SweepWorld != nullptr)
			{
				// Progress is measured the same way the ability layer's poll measures it, so the
				// two drivers cannot disagree about whether a contact was inside the end window.
				const float Duration = FMath::Max(0.01f, GetDashDuration());
				const float Progress = FMath::Clamp(1.f - (DashTimeRemaining / Duration), 0.f, 1.f);

				const FVector MyLocation = CharacterOwner->GetActorLocation();
				const float RadiusSq = SweepRadius * SweepRadius;

				// A distance test over the handful of pawns in the arena rather than a collision
				// query: it needs no channel, no response setup and no profile, which are three
				// things a query could be silently misconfigured by. TryBash re-tests the gap
				// itself, so a generous candidate list cannot widen the ability.
				for (TActorIterator<ATraceCharacter> It(SweepWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || Candidate == CharacterOwner || !IsValid(Candidate))
					{
						continue;
					}
					if (FVector::DistSquared(MyLocation, Candidate->GetActorLocation()) <= RadiusSq)
					{
						++TraceAbilityIntegration::Counters().DashHits;
						DashAbilities->NotifyDashHitCharacter(Candidate, Progress);
					}
				}
			}
		}
	}

	// 5. Consume the one-shot intent and remember the held one for the next move's edge test. On
	//    the server the intents are re-supplied by the next ServerMove's flags, on the client by the
	//    next StartDash(), and during replay by UpdateFromCompressedFlags — so one key press can
	//    only ever produce one dash.
	bWantsToDash = 0;
	bSlideHeldLastMove = bCrouchHeld ? 1 : 0;

	// Through the grace, like every other ground test in this function: the landing transition is
	// derived from this bit, and a ledge blip that flipped it would fake a landing (and with a held
	// crouch key, a whole slide) on one machine and not the other.
	bWasAirborneLastMove = (MovementMode != MOVE_None && !IsGroundedForAbilities()) ? 1 : 0;

#if !UE_BUILD_SHIPPING
	// SPEC v8 §5 — THE LIVE CHARGE COUNTS, ON WHICHEVER MACHINE IS RUNNING THIS PAWN.
	//
	// Not on a replayed move: a correction replays several moves in one frame and would print the same
	// second several times with rewound counts, which reads as the pool flickering when it is not.
	if (IsDashPoolDebugEnabled() && CharacterOwner != nullptr && !CharacterOwner->bClientUpdating)
	{
		if (const UWorld* PoolWorld = GetWorld())
		{
			const float Now = static_cast<float>(PoolWorld->GetTimeSeconds());
			if (Now >= DashPoolDebugNextLogTime)
			{
				DashPoolDebugNextLogTime = Now + 1.f;

				const ATraceCharacter* PoolCharacter = Cast<ATraceCharacter>(CharacterOwner);
				const UTraceSettings& PoolSettings = UTraceSettings::Get();

				UE_LOG(LogTraceGame, Display,
					TEXT("DASHPOOL %-16s netMode=%d role=%d local=%d carrier=%d | charges=%d/%d lastMax=%d "
					     "refill=%5.2f dashLeft=%5.2f | cfg base=%d carrierExtra=%d window=%.2f"),
					*GetNameSafe(CharacterOwner), static_cast<int32>(GetNetMode()),
					static_cast<int32>(CharacterOwner->GetLocalRole()),
					CharacterOwner->IsLocallyControlled() ? 1 : 0,
					(PoolCharacter != nullptr && PoolCharacter->IsCarrier()) ? 1 : 0,
					DashCharges, GetMaxDashCharges(), LastMaxDashCharges,
					DashRechargeRemaining, DashTimeRemaining,
					PoolSettings.BaseDashCharges, PoolSettings.CarrierExtraDashCharges,
					GetDashRechargeWindow());
			}
		}
	}

	// SPEC v18 §1b. The record pass only — a replay re-runs the same frames and would count each one
	// several times, which is exactly how a drift ledger invents drift that never happened.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating)
	{
		TickAirDriftMeter(DeltaSeconds, OldVelocity);
	}

	TickMomentumMeasure(DeltaSeconds);
	TickLedgeTest(DeltaSeconds);
	TickDashPitchTest(DeltaSeconds);

	// SPEC v10 §5. Advanced on the RECORD pass only, for the reason BeginWallStickSample() gives: a
	// replay re-runs the same frames and would close the same sample several times over. Always on
	// (it costs a dot product per frame and only while a sample is open) so a human play session can
	// dump the same number with Trace.WallStickReport that the harness prints — the complaint is
	// about feel, and a meter that only exists inside a synthetic run cannot be checked against it.
	if (CharacterOwner != nullptr && !CharacterOwner->bClientUpdating && CharacterOwner->IsLocallyControlled())
	{
		TickWallStickSample();
	}

	TickWallJumpTest(DeltaSeconds);
	TickCarrierChargeTest(DeltaSeconds);
	TickSingleDashTest(DeltaSeconds);
	TickSurfTest(DeltaSeconds);
#endif
}

float UTraceCharacterMovementComponent::GetMaxSpeed() const
{
	if (IsDashing())
	{
		return GetDashSpeed();
	}

	// A MOVE_Flying carve-out for the mantle sat here (spec v5 §7), so that nothing sampling the
	// pawn's speed limit mid-pull-up was told it was capped at MaxFlySpeed. Deleted in v12 §5: the
	// component never enters MOVE_Flying of its own accord any more.

	float Speed = Super::GetMaxSpeed();

	// The Core carrier is slightly faster (UTraceSettings::CarrierSpeedMultiplier). bIsCarrier is
	// replicated, so the client applies this a fraction of a second after the server does; the
	// resulting sub-frame divergence is exactly what the correction path exists to absorb.
	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(CharacterOwner))
	{
		if (TraceCharacter->IsCarrier())
		{
			Speed *= FMath::Max(0.01f, UTraceSettings::Get().CarrierSpeedMultiplier);
		}
	}

	// SPEC v10 §1 — "Players should move 30% faster with a knife." 800 -> 1040 at the shipped walk
	// speed. Applied AFTER the carrier multiplier and BEFORE the slide floor: the slide floor is an
	// absolute speed the knife has no business scaling (the slide is a separate ability with its own
	// tuning, not a faster walk).
	//
	// THE TWO MULTIPLIERS DO NOT STACK, and this comment used to claim they did. The ordering above
	// makes stacking arithmetically possible — 1.08 x 1.30 = 1.40x — but it never happens, because
	// nothing ever sets bKnifeMovementProfile on a carrier: TraceMelee::ShouldUseKnifeMovementProfile
	// is the sole definition of the bit and its carrier clause is explicit, a carrier is refused a
	// swap outright, and UTraceWeaponComponent::RefreshMovementProfile re-asserts the bit every tick
	// on every machine so a pickup while holding the knife clears it within a frame. A carrier's
	// knife is STOWED — they cannot swing it and cannot shoot — so the movement bonus for "the knife
	// is the active weapon" is false by the plain reading of the rule.
	//
	// The guard therefore lives one slice away, on purpose (one definition, not two opinions). If
	// that clause is ever removed, 1.40x lands HERE and silently retires the one number the
	// carrier's speed was ever tuned with. Do not add a second carrier test below to "be safe" —
	// two tests that can disagree is how this became wrong in the first place.
	//
	// A DASH is unaffected in either case: the IsDashing() branch at the top of this function returns
	// GetDashSpeed() before any multiplier is reached, so the knife does not buy a faster dash.
	//
	// bKnifeMovementProfile is saved-move state, so a replayed move is clamped by the same ceiling
	// the original was. The remaining seam is the one-RTT gap between the two ends learning about a
	// swap, which is exactly the carrier seam documented immediately above and is absorbed the same
	// way.
	if (bKnifeMovementProfile)
	{
		Speed *= GetKnifeMoveSpeedMultiplier();
	}

	// =============================================================================================
	// SPEC v14 §6 — THE ABILITY SPEED PASSIVES. Rocco's headshot-kill stack, X's +10% while an enemy
	// is vulnerable, and any external debuff another player's ability has put on this pawn.
	// =============================================================================================
	//
	// GROUND ONLY, and that is a decision rather than an oversight. The air ceilings in this project
	// are the momentum model (soft cap, hard cap, falloff) and are not expressed through
	// GetMaxSpeed(); scaling this value while airborne would change nothing for a strafing player
	// and everything for a walking one on the single frame they touch down. A dash is unaffected in
	// every case — the IsDashing() branch at the top of this function returns before any of this.
	//
	// TWO SEPARATE MULTIPLIERS, on purpose:
	//
	//   GetMoveSpeedMultiplierFor  asks the pawn's OWN character about its own passive. It is the
	//                              right question for a buff and the WRONG one for a debuff: a
	//                              poisoned pawn need not have a character at all (mode A, the
	//                              characters toggle, a roster that could not serve them) and then
	//                              has no ability set to ask, so a debuff routed through that hook
	//                              would silently not apply. This used to cite spec §3's "bots are
	//                              characterless"; spec v15 §2 reversed that rule, not this argument.
	//   TraceAbilityDebuff::       asks what OTHER players' abilities have done TO this pawn. One
	//   GetMoveSpeedMultiplier     aggregator, and every provider is a line inside it.
	//
	// Both are 1.0 for a Mannequin, so this costs a PlayerState lookup and a component scan on a
	// path that already does one for the carrier test.
	if (IsMovingOnGround() && TraceAbilityIntegration::IsEnabled())
	{
		const float AbilityScale =
			UTraceAbilityComponent::GetMoveSpeedMultiplierFor(CharacterOwner)
			* TraceAbilityDebuff::GetMoveSpeedMultiplier(CharacterOwner);

		if (!FMath::IsNearlyEqual(AbilityScale, 1.f))
		{
			Speed *= AbilityScale;
			++TraceAbilityIntegration::Counters().SpeedMultiplierApplied;
		}
	}

	// A slide is faster than a walk, and CalcVelocity clamps to this value during the physics step
	// that runs BEFORE OnMovementUpdated re-asserts the slide velocity. Leaving it at the walk speed
	// would mean the frame's actual displacement was computed at walking pace and only the reported
	// velocity looked like a slide.
	if (IsSliding())
	{
		Speed = FMath::Max(Speed, SlideSpeed);
	}

	return Speed;
}

// =================================================================================================
// Dev-only measurement harness — "-TraceMoveMeasure". See the header.
//
// This exists because the four things spec §2 asks for are all NUMBERS, and none of them can be
// verified from a screenshot or from "it feels better". It prints, at Display:
//
//   AIRTURN   planar speed before / after a 90-degree strafe turn, and the angle actually turned
//   LAND      planar speed the frame before touchdown and 0.1s / 0.3s / 0.6s after it
//   SLIDE     entry speed vs exit speed, and the measured gap before the next slide is allowed
// =================================================================================================

#if !UE_BUILD_SHIPPING

// --- The rubber-band instrument (spec v5 §7) ----------------------------------------------------
//
// "Feels like rubber banding" is a claim about the network. This is what turns it into a number:
// every server correction the owning client receives, with the position error that triggered it and
// the pawn's state at the time. At Display, because a diagnostic nobody can see has twice been read
// as a dead mechanic in this project.

int32 GTraceMoveCorrections = 0;
static FAutoConsoleVariableRef CVarTraceMoveCorrections(
	TEXT("Trace.MoveCorrections"),
	GTraceMoveCorrections,
	TEXT("Dev only. Non-zero logs every server movement correction this client receives, with the "
	     "position error and the movement state, so ledge desyncs can be counted instead of described."),
	ECVF_Cheat);

static bool AreMoveCorrectionsLogged()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceMoveCorrections"));
	return bFromCommandLine || GTraceMoveCorrections != 0;
}

int32 GTraceMoveKitFakeCarrier = 0;
static FAutoConsoleVariableRef CVarTraceMoveKitFakeCarrier(
	TEXT("Trace.MoveKitFakeCarrier"),
	GTraceMoveKitFakeCarrier,
	TEXT("Dev only. Non-zero pretends this pawn is carrying the Core for the purposes of the dash "
	     "charge pool, so the carrier's extra charge can be exercised without a Core."),
	ECVF_Cheat);

/**
 * SPEC v8 §1. The A/B switch for the dash-aim replay fix, so "before and after" is one build.
 *
 * 1 restores the spec v7 behaviour exactly: the replayed dash composes its direction from the base
 * class's SavedControlRotation, which FSavedMove_Character::PostUpdate(PostUpdate_Replay) has already
 * overwritten with the live mouse by the time a SECOND correction replays the same move. 0 (the
 * default) uses FSavedMove_Trace::SavedDashAimRotation, which is recorded once and is immutable.
 *
 * A cvar rather than two builds because the measurement has to be corrections-per-dash on a CLIENT at
 * 40ms, and two separately-built clients are two different populations of network jitter.
 */
/**
 * How many times -TraceDashPitchTest walks its seven-phase list. See DashPitchTestCycle in the header:
 * one lap is seven dashes, and seven dashes cannot separate a prediction change from an afternoon of
 * machine load.
 */
int32 GTraceDashPitchTestCycles = 4;
static FAutoConsoleVariableRef CVarTraceDashPitchTestCycles(
	TEXT("Trace.DashPitchTestCycles"),
	GTraceDashPitchTestCycles,
	TEXT("Dev only. Laps of the -TraceDashPitchTest phase list. 7 dashes per lap; the default 4 gives 28."),
	ECVF_Cheat);

int32 GTraceDashLegacyAimReplay = 0;
static FAutoConsoleVariableRef CVarTraceDashLegacyAimReplay(
	TEXT("Trace.DashLegacyAimReplay"),
	GTraceDashLegacyAimReplay,
	TEXT("Dev only. 1 restores the spec v7 dash-replay aim source (the base class's SavedControlRotation, "
	     "which the replay pass stomps) so the fix can be A/B'd against it in one build."),
	ECVF_Cheat);

/**
 * SPEC v9 §2 / §0 — THE A/B ARM FOR THE CHARGE READOUT.
 *
 * Spec v9 §0's whole complaint is that the previous pass produced a harness that never went red. A
 * fix demonstrated only by a green run is not demonstrated at all, and rebuilding the old code to
 * get a red run means comparing two binaries — which the Trace.DashLegacyAimReplay comment already
 * calls out as dishonest for exactly this reason.
 *
 * So set this to 1 and GetDashCooldownRemaining() answers the way the shipped build did: 0 whenever
 * any charge is in hand. Run -TraceSingleDashTest with it on and the reproduction fails; run it with
 * it off and the same reproduction passes. One binary, one harness, both results.
 */
int32 GTraceDashLegacyChargeReadout = 0;
static FAutoConsoleVariableRef CVarTraceDashLegacyChargeReadout(
	TEXT("Trace.DashLegacyChargeReadout"),
	GTraceDashLegacyChargeReadout,
	TEXT("Dev only. 1 restores the pre-v9 GetDashCooldownRemaining() (returns 0 whenever any charge is "
	     "banked, so the HUD draws a still-recharging pip as full) to reproduce the spec v9 §2 bug in "
	     "this build."),
	ECVF_Cheat);

/**
 * Live charge-pool readout (spec v8 §5), once a second, per locally-controlled pawn.
 *
 * "The two dash charges aren't working anymore, for the carrier" is a claim about two integers, and
 * the only place either of them is true or false is a running game — on a CLIENT, while carrying,
 * where the carrier bit is replicated rather than authoritative. This prints both, plus the maximum
 * the pool thinks it has and the settings that produced it.
 */
int32 GTraceDashPoolDebug = 0;
static FAutoConsoleVariableRef CVarTraceDashPoolDebug(
	TEXT("Trace.DashPoolDebug"),
	GTraceDashPoolDebug,
	TEXT("Dev only. Non-zero logs the live dash charge pool (charges / max / carrier bit / refill "
	     "clock) once a second for every locally-controlled pawn."),
	ECVF_Cheat);

static bool IsDashPoolDebugEnabled()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceDashPoolDebug"));
	return bFromCommandLine || GTraceDashPoolDebug != 0;
}

/**
 * SPEC v9 §0 — the A/B arm for the §§5-8 TUNING items. See IsV9LegacyTuning() at the top of the file
 * for the identity values it restores and for why the command-line switch (-TraceLegacyTuning) is not
 * redundant with this variable.
 */
// The variable itself is DEFINED at the top of the file, outside this dev block, because
// IsV9LegacyTuning() is read from shipping code (GetSlideJumpWindowSpeedBonus,
// GetAirStrafeAsymptoteScale, GetWallJumpSpeedRetention, RefreshEngineTunablesFromSettings). Only the
// console registration belongs here. See the comment on the definition for the Shipping link failure
// this arrangement fixes.
static FAutoConsoleVariableRef CVarTraceV9LegacyTuning(
	TEXT("Trace.V9LegacyTuning"),
	GTraceV9LegacyTuning,
	TEXT("Dev only. 1 restores the pre-v9 movement tuning (wall-jump retention and window, no "
	     "wall-jump-over-mantle priority, full-length slide, the 1.3125 slide-jump bonus, the "
	     "un-nudged air-strafe asymptote and gravity x1.0) so the spec v9 secs 5-8 changes can be "
	     "measured as a BEFORE/AFTER in one binary."),
	ECVF_Cheat);

/**
 * SPEC v10 §5 — the A/B arm for the wall-jump STICK fix specifically. See IsV10LegacyWallJump() at the
 * top of the file. Separate from V9LegacyTuning on purpose: that switch reverts eight unrelated tuning
 * values as well, and a stick number measured against it would be measuring gravity and the slide too.
 */
// Defined at the top of the file, outside this dev block — GetWallJumpSpeedRetention() reads it and
// ships. Registration only, here.
/**
 * SPEC v24 §8 — the A/B arm for the shorter slide and the earlier bonus window. See IsV24LegacySlide()
 * at the top of the file for why the trim is a knob rather than an edit to SlideDuration.
 */
// Defined at the top of the file, outside this dev block — GetSlideDuration() reads it and that
// ships. Registration only, here, for the Shipping-link reason on GTraceV9LegacyTuning.
static FAutoConsoleVariableRef CVarTraceV24LegacySlide(
	TEXT("Trace.V24LegacySlide"),
	GTraceV24LegacySlide,
	TEXT("Dev only. 1 restores the pre-v24 slide length (SlideDurationTrimSeconds back to 0, i.e. the "
	     "full 1.26s slide) and NOTHING else, so the v24 sec 8 window-open time and slide duration can "
	     "be measured as a BEFORE/AFTER in one binary. Run -TraceSlideDebug in both arms and diff the "
	     "V24WINDOW lines."),
	ECVF_Cheat);

/**
 * SPEC v26 §3 — the A/B arm for the 20% cut and the chain ceiling. See IsV26LegacySlideJump() at the
 * top of the file for why both halves are named scalars rather than edits to the shipped numbers.
 */
// Defined at the top of the file, outside this dev block — GetSlideJumpWindowSpeedBonus() and
// DoJump() read it and both ship. Registration only, here, for the Shipping-link reason on
// GTraceV9LegacyTuning.
static FAutoConsoleVariableRef CVarTraceV26LegacySlideJump(
	TEXT("Trace.V26LegacySlideJump"),
	GTraceV26LegacySlideJump,
	TEXT("Dev only. 1 restores the pre-v26 slide-jump exactly: SlideJumpMomentumScale back to 1.0 (the "
	     "well-timed multiplier back to 1.446875) AND the chain ceiling off, so chaining compounds "
	     "without limit again. The RED arm for spec v26 sec 3 -- run Trace.Move.AuditV16.SlideChain "
	     "with and without it in ONE binary and diff the four launch speeds."),
	ECVF_Cheat);

static FAutoConsoleVariableRef CVarTraceV10LegacyWallJump(
	TEXT("Trace.V10LegacyWallJump"),
	GTraceV10LegacyWallJump,
	TEXT("Dev only. 1 restores the spec v9 wall jump exactly: no post-launch into-wall input lockout, "
	     "no buffered jump press, and the v9 momentum retention. The RED arm for the spec v10 sec 5 "
	     "stick meter -- run -TraceWallJumpTest with and without it in ONE binary."),
	ECVF_Cheat);

// SPEC v18 §1a. The variables themselves are DEFINED at the top of the file, outside this dev block,
// because GetAirStrafeOpposingDeceleration() reads them and that is shipping code. Only the console
// registration belongs here. See the note above IsV18LegacyAirReverse() for why the v9/v10 arms above
// do it the other way round and why that is a latent Shipping-link bug rather than a pattern to copy.
/**
 * PATCH 28 §5. See GTraceSurfLegacyAirLimit's definition for what the arm is and why it exists.
 * ECVF_Cheat like every other legacy arm here: it is A/B evidence, not a player option.
 */
static FAutoConsoleVariableRef CVarTraceSurfLegacyAirLimit(
	TEXT("Trace.Move.SurfLegacyAirLimit"),
	GTraceSurfLegacyAirLimit,
	TEXT("Patch 28 sec 5 A/B. 1 puts UE's stock LimitAirControl back on surf planes, which deletes the "
	     "up-slope half of the player's input and is the behaviour the override replaces. Run "
	     "-TraceSurfTest with it on and off and diff the ideal-strafe column."),
	ECVF_Cheat);

/**
 * DEMO 29 ITEM 4. See GTraceSurfLegacyExit's definition for what the arm restores and why.
 */
static FAutoConsoleVariableRef CVarTraceSurfLegacyExit(
	TEXT("Trace.Move.SurfLegacyExit"),
	GTraceSurfLegacyExit,
	TEXT("Demo 29 item 4 A/B. 1 restores the Patch 28 exit and entry behaviour: CanStepUp refuses a "
	     "surf plane (which stops a walking pawn dead instead of sliding), no ground -> surf entry, no "
	     "exit rollout and no exit carry window. Run -TraceSurfExitTest / -TraceSurfApproachTest with "
	     "it on and off. Use -TraceSurfLegacyExit on the command line: an ECVF_Cheat variable set from "
	     "-ExecCmds does not reliably survive into a session."),
	ECVF_Cheat);

/**
 * D30-RESETS (a). See GTraceResetsLegacyMomentum's definition for what the arm restores.
 */
static FAutoConsoleVariableRef CVarTraceResetsLegacyMomentum(
	TEXT("Trace.Resets.LegacyMomentum"),
	GTraceResetsLegacyMomentum,
	TEXT("D30-RESETS red arm. 1 restores the pre-pass behaviour of a half switch and a respawn: "
	     "StopMovementImmediately() and nothing else, so the dash / slide / wall-jump / surf clocks "
	     "ride straight through and a mid-dash pawn puts its velocity back on the next move. Run "
	     "Trace.Resets.Arm with it on and off in ONE binary and diff the [Resets] before/after lines. "
	     "Use -TraceResetsLegacyMomentum on the command line: an ECVF_Cheat variable set from "
	     "-ExecCmds does not reliably survive into a session."),
	ECVF_Cheat);

/**
 * D30-RESETS (a). THE ARMING HALF OF THE PROOF.
 *
 * A reset that runs on a pawn which was already still proves nothing, and the organic run cannot be
 * relied on to have somebody mid-surf on the exact frame the whistle goes. This puts a full kit's
 * worth of momentum on every living pawn on this machine, so that the NEXT half switch or respawn is
 * observed removing something. Takes an optional planar speed; 1500 uu/s (about a good surf) default.
 */
static FAutoConsoleCommandWithWorldAndArgs CmdTraceResetsArm(
	TEXT("Trace.Resets.Arm"),
	TEXT("Trace.Resets.Arm [speed] [holdSeconds]. D30-RESETS. Gives THIS MACHINE'S OWN player pawn a "
	     "fake dash + slide + wall window + surf ride at <speed> uu/s (default 1500) and RE-ARMS it "
	     "every frame for holdSeconds (default 45), so whenever the next half switch or respawn lands "
	     "it lands on a pawn that is carrying something. The half-time whistle is DEFERRED to the next "
	     "dead ball, so there is no frame a harness could have been told to fire on. Read the result "
	     "from the [Resets] before/after line the reset itself prints. Dev only."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (World == nullptr)
			{
				return;
			}

			const float Speed = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 1500.f;
			const float HoldSeconds = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : 45.f;

			// THIS MACHINE'S OWN PLAYER PAWN, AND DELIBERATELY NOT EVERY PAWN IN THE WORLD.
			//
			// On a listen host "every pawn" is nine bots as well, all of them suddenly travelling at
			// 1500 uu/s into walls, which stops the match producing the dead ball the deferred whistle
			// is waiting for — the harness would prevent the event it exists to observe. One pawn is
			// also the honest subject: the half switch resets all ten and the log then shows one line
			// with something in it and nine with nothing, which is exactly the right picture.
			//
			// Named so the ticker below and the immediate arm share ONE implementation: a harness that
			// arms two different ways is measuring two different things.
			const auto ArmLocalPawn = [](UWorld* InWorld, float InSpeed, bool bLog) -> int32
			{
				int32 Count = 0;
				for (TActorIterator<ACharacter> It(InWorld); It; ++It)
				{
					ACharacter* const Pawn = *It;
					if (Pawn == nullptr || !Pawn->IsPlayerControlled() || !Pawn->IsLocallyControlled())
					{
						continue;
					}

					if (UTraceCharacterMovementComponent* Move =
							Cast<UTraceCharacterMovementComponent>(Pawn->GetCharacterMovement()))
					{
						Move->DebugArmMomentum(InSpeed, bLog);
						++Count;
					}
				}
				return Count;
			};

			const int32 Armed = ArmLocalPawn(World, Speed, /*bLog=*/true);

			UE_LOG(LogTraceGame, Display,
				TEXT("[Resets] Trace.Resets.Arm: %d local pawn(s) armed at %.0f uu/s, hold %.1fs. "
				     "legacy momentum arm = %d. resets so far = %d."),
				Armed, Speed, HoldSeconds, GTraceResetsLegacyMomentum, GTraceResetsCount);

			if (Armed == 0)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Resets] Trace.Resets.Arm found no locally-controlled player pawn. NOTHING WAS "
					     "ARMED, so anything the next reset prints proves nothing."));
			}

			if (HoldSeconds <= 0.f)
			{
				return;
			}

			TWeakObjectPtr<UWorld> WeakWorld(World);
			TSharedRef<float> Elapsed = MakeShared<float>(0.f);
			const int32 CountAtArm = GTraceResetsCount;

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[WeakWorld, Speed, HoldSeconds, Elapsed, CountAtArm, ArmLocalPawn](float DeltaTime) -> bool
				{
					UWorld* const LiveWorld = WeakWorld.Get();
					*Elapsed += DeltaTime;

					// IT KEEPS RE-ARMING PAST THE FIRST RESET, ON PURPOSE, and that does not corrupt the
					// evidence: ResetMomentum() captures its "before" and prints its before/after inside
					// the same frame it runs, so the reading is already taken by the time this ticker
					// next gets to write anything. Stopping on the first reset would be worse — in a bot
					// match a respawn lands within a second or two, and the hold would end long before
					// the half switch this is usually aimed at.
					if (LiveWorld == nullptr || *Elapsed >= HoldSeconds)
					{
						const int32 Landed = GTraceResetsCount - CountAtArm;
						if (Landed > 0)
						{
							UE_LOG(LogTraceGame, Display,
								TEXT("[Resets] Arm hold finished after %.1fs: %d reset(s) landed while the "
								     "pawn was armed. Read the [Resets] before/after lines above."),
								*Elapsed, Landed);
						}
						else
						{
							UE_LOG(LogTraceGame, Warning,
								TEXT("[Resets] Arm hold finished after %.1fs and NO reset ran at all. "
								     "NOTHING WAS PROVEN either way - the run never reached a half switch "
								     "or a respawn."),
								*Elapsed);
						}
						return false;
					}

					ArmLocalPawn(LiveWorld, Speed, /*bLog=*/false);
					return true;
				}),
				0.f);
		}));

/**
 * D30-RESETS. Prints the momentum state of every pawn on this machine without touching it, so the
 * "after" can be read at an arbitrary later moment rather than only from the reset's own log line.
 */
static FAutoConsoleCommandWithWorld CmdTraceResetsProbe(
	TEXT("Trace.Resets.Probe"),
	TEXT("D30-RESETS. Prints one [Resets] PROBE line per pawn on this machine: planar speed and every "
	     "dash / slide / ledge / wall-jump / surf field ResetMomentum() clears. Reads only. Dev only."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			if (World == nullptr)
			{
				return;
			}

			for (TActorIterator<ACharacter> It(World); It; ++It)
			{
				ACharacter* const Pawn = *It;
				if (Pawn == nullptr)
				{
					continue;
				}

				if (const UTraceCharacterMovementComponent* Move =
						Cast<UTraceCharacterMovementComponent>(Pawn->GetCharacterMovement()))
				{
					UE_LOG(LogTraceGame, Display, TEXT("[Resets] PROBE %s (%s): %s"),
						*GetNameSafe(Pawn), TraceRoleToString(Pawn->GetLocalRole()),
						*Move->DebugDescribeMomentum());
				}
			}
		}));

static FAutoConsoleVariableRef CVarTraceV18LegacyAirReverse(
	TEXT("Trace.Move.V18.LegacyAirReverse"),
	GTraceV18LegacyAirReverse,
	TEXT("Dev only. 1 restores the shipped v17 air model exactly by forcing the spec v18 sec 1a "
	     "opposition brake to zero -- i.e. reversing in mid-air changes your momentum by nothing at "
	     "all. The RED arm for Trace.Move.V18.AirReverse; run both arms in ONE binary."),
	ECVF_Cheat);

static FAutoConsoleVariableRef CVarTraceAirOpposingDecel(
	TEXT("Trace.Move.AirOpposingDecel"),
	GTraceAirOpposingDecel,
	TEXT("Spec v18 sec 1a. Deceleration in uu/s^2 applied when air input opposes travel, at a full "
	     "180 degree reversal; scaled down by the negative part of dot(wish, travel), so it is exactly "
	     "0 at 90 degrees and inside it and the air strafe is untouched. NEGATIVE (the default) means "
	     "'use the AirStrafeOpposingDeceleration knob'. This is the number the spec expects to be "
	     "tuned by feel."),
	ECVF_Cheat);

void UTraceCharacterMovementComponent::TickMomentumMeasure(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceMoveMeasure"));
	if (!bEnabled || CharacterOwner == nullptr)
	{
		return;
	}

	// THE AUTHORITY'S OWN PAWN ONLY. The schedule below advances on the simulation's own delta, and a
	// replayed move would advance it twice — so this must never run on an autonomous proxy.
	//
	// It used to say `GetNetMode() != NM_Standalone`, and spec v5 §0 silently switched it off: PLAY
	// now appends `listen` to the travel URL, so a solo session is an NM_ListenServer and every
	// measurement in this harness stopped being taken. HasAuthority() && IsLocallyControlled() is the
	// condition that was actually meant — it is true for a standalone player and for a listen host,
	// and false for exactly the case (a client replaying corrections) the restriction exists for.
	if (!CharacterOwner->HasAuthority() || !CharacterOwner->IsLocallyControlled()
		|| Cast<APlayerController>(CharacterOwner->GetController()) == nullptr)
	{
		return;
	}

	if (MeasureTime < 0.f)
	{
		// Let the pawn spawn, settle onto the floor and finish its first replication.
		if (GetWorld() == nullptr || GetWorld()->GetTimeSeconds() < 3.f || !IsMovingOnGround())
		{
			return;
		}
		MeasureTime = 0.f;
		MeasurePhase = 0;
		MeasurePhaseTime = 0.f;

		// Run toward the middle of the field, not along a world axis: spawns are in the endzones and
		// a fixed +X run walks straight into the back wall, which turns every number after it into a
		// measurement of a collision.
		MeasureRunDirection = FVector::ForwardVector;
		if (UpdatedComponent != nullptr)
		{
			FVector TowardCentre = -UpdatedComponent->GetComponentLocation();
			TowardCentre.Z = 0.f;
			if (TowardCentre.Normalize())
			{
				MeasureRunDirection = TowardCentre;
			}
		}
		UE_LOG(LogTraceGame, Display,
			TEXT("MEASURE ---- begin. walk=%.0f | air: srcModel=%d accel=%.0f wishCap=%.0f maxAir=%.0f airFric=%.2f airControl=%.2f "
			     "| land: preserve=%d fric=%.2f brake=%.0f turn=%.0fdeg/s | dashExit=%.2fx "
			     "| slide: entryMul=%.2f cooldown=%.2fs (NO impulse, NO exit floor) "
			     "| slideJump: on=%d retain=%.2f zMul=%.2f window=%.2fs windowBonus=%.2f"),
			MaxWalkSpeed, IsSourceAirAccelerationEnabled() ? 1 : 0, GetAirAcceleration(), GetAirMaxWishSpeed(),
			GetMaxAirSpeed(), FallingLateralFriction, AirControl,
			IsLandingMomentumPreserved() ? 1 : 0, GetGroundOverspeedFriction(), GetGroundOverspeedBraking(),
			GetGroundOverspeedTurnRate(), GetDashExitSpeedMultiplier(),
			GetSlideEntrySpeedMultiplier(), GetSlideCooldownSeconds(),
			IsSlideJumpEnabled() ? 1 : 0, GetSlideJumpHorizontalRetention(), GetSlideJumpZMultiplier(),
			GetSlideJumpWindowSeconds(), GetSlideJumpWindowSpeedBonus());
		MeasureHomeLocation = (UpdatedComponent != nullptr)
			? UpdatedComponent->GetComponentLocation()
			: FVector::ZeroVector;

		UE_LOG(LogTraceGame, Display, TEXT("MEASURE run direction %s from %s"),
			*MeasureRunDirection.ToCompactString(),
			*(UpdatedComponent != nullptr ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector).ToCompactString());

		// --- THE AIR-STRAFE CURVE, AS A TABLE (spec v5 §1) ---------------------------------------
		//
		// The curve is a pure function, so it can be printed rather than inferred from a trajectory.
		// This is "the new curve with measured numbers at several input speeds" in one line per
		// speed: what fraction of a strafe's gain survives, and what one 60Hz frame of perpendicular
		// input is actually worth at that speed. The trajectory phases below then confirm the table.
		UE_LOG(LogTraceGame, Display,
			TEXT("MEASURE AIRCAP curve: falloff=%d soft=%.0f hard=%.0f exp=%.2f hardCapOn=%d"),
			IsAirStrafeFalloffEnabled() ? 1 : 0, GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(),
			GetAirStrafeFalloffExponent(), IsAirStrafeHardCapEnabled() ? 1 : 0);
		for (const float SampleSpeed : { 600.f, 700.f, 800.f, 835.f, 900.f, 950.f, 1000.f, 1036.f,
		                                 1100.f, 1150.f, 1200.f, 1250.f, 1300.f, 1400.f })
		{
			// One 1/60s frame of perfectly perpendicular input: the projection is 0, so the whole
			// AirAcceleration allowance is available and lands sideways, giving sqrt(v^2 + a^2).
			const float Allowance = FMath::Min(GetAirAcceleration() / 60.f,
				FMath::Min(GetMaxAirSpeed(), GetAirMaxWishSpeed()));
			const float RawFrameGain = FMath::Sqrt(SampleSpeed * SampleSpeed + Allowance * Allowance) - SampleSpeed;
			const float Scale = GetAirStrafeGainScale(SampleSpeed);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRCAP   v=%6.0f uu/s  gainScale=%.4f  frameGain %5.2f -> %5.2f uu/s "
				     "(%6.1f -> %6.1f uu/s per second of strafing)"),
				SampleSpeed, Scale, RawFrameGain, RawFrameGain * Scale,
				RawFrameGain * 60.f, RawFrameGain * Scale * 60.f);
		}
	}

	MeasureTime += DeltaSeconds;
	MeasurePhaseTime += DeltaSeconds;

	const FVector PlanarVelocity(Velocity.X, Velocity.Y, 0.f);
	const float PlanarSpeed = PlanarVelocity.Size();
	const FVector TravelDirection = PlanarVelocity.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);

	auto Advance = [this](int32 NextPhase)
	{
		MeasurePhase = NextPhase;
		MeasurePhaseTime = 0.f;
	};

	// Angle between the velocity vector now and where it pointed when the phase started.
	auto TurnedDegrees = [this, &TravelDirection]() -> float
	{
		return FMath::RadiansToDegrees(FMath::Acos(
			FMath::Clamp(FVector::DotProduct(MeasureMarkDirection, TravelDirection), -1.f, 1.f)));
	};

	switch (MeasurePhase)
	{
	// --- 0. A CONTROLLED DROP -----------------------------------------------------------------
	//
	// The air numbers are taken from a scripted drop rather than from a run-and-jump, because a
	// run-and-jump measures the arena as much as it measures the movement model: the first two
	// attempts at this harness sprinted into an endzone wall and a bank, and reported a jump that
	// "lost" 750 uu/s, which was a collision. Placing the pawn high over the middle of the field
	// with a known velocity isolates the model. The run-and-jump is still exercised below, on the
	// ground, where the slide numbers are taken.
	case 0:
		if (UpdatedComponent != nullptr && CharacterOwner != nullptr)
		{
			const FVector Here = UpdatedComponent->GetComponentLocation();
			CharacterOwner->SetActorLocation(FVector(0.f, 0.f, Here.Z + 1500.f), false, nullptr, ETeleportType::TeleportPhysics);
			Velocity = FVector(MaxWalkSpeed, 0.f, 0.f);
			SetMovementMode(MOVE_Falling);
			MeasureMarkA = MaxWalkSpeed;
			MeasureMarkDirection = FVector::ForwardVector;
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE DROP: placed at (0,0,%.0f) with planar=%.0f uu/s along +X"),
				Here.Z + 1500.f, MaxWalkSpeed);
			Advance(1);
		}
		break;

	// --- 1. FIXED perpendicular input -------------------------------------------------------------
	//
	// The strongest possible refutation of a lerp-style air control: hold a direction exactly 90
	// degrees from travel and do nothing else. A lerp subtracts the forward component every frame
	// and the speed FALLS. The projection formula can only add sideways, so the speed must not fall.
	case 1:
	{
		const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, MeasureMarkDirection).GetSafeNormal();
		CharacterOwner->AddMovementInput(Perpendicular, 1.f);
		UE_LOG(LogTraceGame, Display, TEXT("MEASURE   air t=%.3f planar=%7.1f velZ=%8.1f mode=%d accel=%6.0f turned=%5.1f"),
			MeasurePhaseTime, PlanarSpeed, Velocity.Z, static_cast<int32>(MovementMode.GetValue()),
			Acceleration.Size2D(), TurnedDegrees());
		if (MeasurePhaseTime > 0.30f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRTURN-FIXED: %.0f -> %.0f uu/s over %.2fs of fixed perpendicular input "
				     "(%.1f%% of entry, vector turned %.1f deg)"),
				MeasureMarkA, PlanarSpeed, MeasurePhaseTime,
				100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA), TurnedDegrees());
			MeasureMarkA = PlanarSpeed;
			MeasureMarkDirection = TravelDirection;
			Advance(2);
		}
		break;
	}

	// --- 2. CONTINUOUSLY perpendicular input (a real strafe turn) ---------------------------------
	//
	// The wish direction is recomputed every frame to stay at 90 degrees to the CURRENT velocity —
	// which is exactly what a player doing a strafe turn produces with mouse + strafe key. This is
	// the number "slightly increase efficacy of strafing in mid air" is about.
	case 2:
	{
		const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, TravelDirection).GetSafeNormal();
		CharacterOwner->AddMovementInput(Perpendicular, 1.f);
		UE_LOG(LogTraceGame, Display, TEXT("MEASURE   air t=%.3f planar=%7.1f velZ=%8.1f mode=%d accel=%6.0f turned=%5.1f"),
			MeasurePhaseTime, PlanarSpeed, Velocity.Z, static_cast<int32>(MovementMode.GetValue()),
			Acceleration.Size2D(), TurnedDegrees());
		if (MeasurePhaseTime > 0.40f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRTURN-STRAFE: %.0f -> %.0f uu/s over %.2fs of continuously perpendicular input "
				     "(%.1f%% of entry, vector turned %.1f deg)"),
				MeasureMarkA, PlanarSpeed, MeasurePhaseTime,
				100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA), TurnedDegrees());
			Advance(3);
		}
		break;
	}

	// --- 3. COAST, then LANDING CARRY ---------------------------------------------------------------
	//
	// No input at all from here: Source has no air friction, so the speed must not move a unit
	// between the last input frame and touchdown, and the touchdown must not clamp it.
	case 3:
		if (!IsMovingOnGround())
		{
			// Refreshed every airborne frame; the last value written is the frame before touchdown.
			MeasureMarkA = PlanarSpeed;
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE LAND: last airborne planar=%.0f uu/s -> first grounded planar=%.0f uu/s "
				     "(%.1f%% carried; ground limit is %.0f)"),
				MeasureMarkA, PlanarSpeed, 100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA), GetMaxSpeed());
			Advance(4);
		}
		break;

	// --- 4-6. Watch the bleed. No input at all, so this is purely the decay curve. ------------------
	case 4:
		if (MeasurePhaseTime > 0.10f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE LAND +0.10s: planar=%.0f uu/s"), PlanarSpeed);
			Advance(5);
		}
		break;

	case 5:
		if (MeasurePhaseTime > 0.20f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE LAND +0.30s: planar=%.0f uu/s"), PlanarSpeed);
			Advance(6);
		}
		break;

	case 6:
		if (MeasurePhaseTime > 0.30f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE LAND +0.60s: planar=%.0f uu/s (ground limit %.0f)"),
				PlanarSpeed, GetMaxSpeed());
			Advance(7);
		}
		break;

	// --- 7. A REAL run-and-jump, for the run->jump transition ---------------------------------------
	case 7:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		if (MeasurePhaseTime > 1.2f)
		{
			if (IsMovingOnGround())
			{
				MeasureMarkA = PlanarSpeed;
				CharacterOwner->Jump();
			}
			else
			{
				CharacterOwner->StopJumping();
				UE_LOG(LogTraceGame, Display,
					TEXT("MEASURE RUN->JUMP: %.0f uu/s on the ground -> %.0f uu/s on the first airborne frame (%.1f%%)"),
					MeasureMarkA, PlanarSpeed, 100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA));
				Advance(8);
			}
		}
		break;

	// --- 8. SLIDE 1: entry speed determines slide velocity -------------------------------------------
	case 8:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		if (MeasurePhaseTime > 1.5f)
		{
			MeasureMarkA = PlanarSpeed;
			SetWantsToSlide(true);
			Advance(9);
		}
		break;

	case 9:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(true);
		if (IsSliding())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDE-1 entry: %.0f uu/s in -> slideSpeed=%.0f uu/s (ratio %.2f, entryMul=%.2f). "
				     "Ratio must be 1.00: entry speed determines slide velocity, nothing tops it up."),
				MeasureMarkA, SlideSpeed, SlideSpeed / FMath::Max(1.f, MeasureMarkA),
				GetSlideEntrySpeedMultiplier());
			Advance(10);
		}
		else if (MeasurePhaseTime > 1.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE SLIDE-1 never latched (planar=%.0f, cooldown=%.2f)"),
				PlanarSpeed, GetSlideCooldownRemaining());
			Advance(12);
		}
		break;

	case 10:
		// Hold it out to its natural end so the exit measured is the one the duration produces.
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(true);
		if (!IsSliding())
		{
			MeasureMarkB = static_cast<float>(GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDE-1 exit: after %.2fs, planar=%.0f uu/s (entry was %.0f, %.1f%% kept), buffer now %.2fs. "
				     "THE EXIT FLOOR IS GONE: the deleted SlideExitMinSpeedFraction would have forced this to %.0f."),
				MeasurePhaseTime, PlanarSpeed, MeasureMarkA,
				100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA), GetSlideCooldownRemaining(),
				MaxWalkSpeed);
			SetWantsToSlide(false);
			Advance(11);
		}
		break;

	// --- 11. SLIDE 2: prove the between-slides buffer ------------------------------------------------
	//
	// Crouch is PULSED rather than held: the slide needs a fresh press edge, and the input buffer is
	// only 0.25s, so one press at the start would expire long before the 0.8s buffer does.
	case 11:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(FMath::Fmod(MeasurePhaseTime, 0.2f) < 0.1f);
		if (IsSliding())
		{
			const float Now = static_cast<float>(GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDE-2 started %.2fs after slide 1 ended (configured buffer %.2fs), slideSpeed=%.0f uu/s"),
				Now - MeasureMarkB, GetSlideCooldownSeconds(), SlideSpeed);
			SetWantsToSlide(false);
			Advance(12);
		}
		else if (MeasurePhaseTime > 4.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE SLIDE-2 never started within 4s (planar=%.0f, buffer left %.2fs)"),
				PlanarSpeed, GetSlideCooldownRemaining());
			SetWantsToSlide(false);
			Advance(12);
		}
		break;

	case 12:
		if (MeasurePhaseTime > 2.f)
		{
			Advance(13);
		}
		break;

	// =============================================================================================
	// THE CHAIN: land fast -> slide -> jump. This is the sequence spec v3 §2.4 is really about, and
	// the one the old code broke in two places: the landing clamped to walk speed, and EndSlide()
	// clamped the exit to walk speed so jumping out of a fast slide threw the momentum away.
	// Everything above measures one transition at a time; this measures them composed.
	// =============================================================================================
	case 13:
		if (UpdatedComponent != nullptr && CharacterOwner != nullptr)
		{
			const FVector Here = UpdatedComponent->GetComponentLocation();
			CharacterOwner->SetActorLocation(FVector(0.f, 0.f, Here.Z + 1500.f), false, nullptr, ETeleportType::TeleportPhysics);
			Velocity = FVector(1200.f, 0.f, 0.f);
			SetMovementMode(MOVE_Falling);
			MeasureMarkA = 1200.f;
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE CHAIN: dropped at planar=1200 uu/s (ground limit %.0f)"), GetMaxSpeed());
			Advance(14);
		}
		break;

	case 14:
	{
		// Strafe up to something comfortably over the ground limit, then hold crouch so the input
		// buffer converts the landing into a slide on the touchdown frame.
		const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, TravelDirection).GetSafeNormal();
		CharacterOwner->AddMovementInput(Perpendicular, 1.f);
		if (MeasurePhaseTime > 0.35f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE CHAIN air speed before landing: %.0f uu/s"), PlanarSpeed);
			SetWantsToSlide(true);
			Advance(15);
		}
		break;
	}

	case 15:
		SetWantsToSlide(true);
		if (!IsMovingOnGround())
		{
			MeasureMarkA = PlanarSpeed;
		}
		else if (IsSliding())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE CHAIN jump->slide: landed at %.0f uu/s -> slideSpeed=%.0f uu/s (%.1f%%; ground limit %.0f)"),
				MeasureMarkA, SlideSpeed, 100.f * SlideSpeed / FMath::Max(1.f, MeasureMarkA), MaxWalkSpeed);
			Advance(16);
		}
		else if (MeasurePhaseTime > 3.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE CHAIN slide never latched (planar=%.0f)"), PlanarSpeed);
			Advance(18);
		}
		break;

	case 16:
		// Ride the slide down INTO its well-timed window and hop out of it, which is the Apex
		// slide-hop the spec is asking for. Waiting for IsSlideJumpWellTimed() rather than for a flat
		// 0.60s is what makes the number below a measurement of the mechanic rather than of a
		// stopwatch: with the shipped 0.20s window against a 1.8s slide, this fires at ~1.6s in.
		SetWantsToSlide(true);
		if (IsSlideJumpWellTimed() || MeasurePhaseTime > 3.0f)
		{
			MeasureMarkA = SlideSpeed;
			MeasureMarkB = IsSlideJumpWellTimed() ? 1.f : 0.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDEJUMP arming at slideSpeed=%.0f uu/s, %.2fs of slide left, wellTimed=%d"),
				SlideSpeed, SlideTimeRemaining, IsSlideJumpWellTimed() ? 1 : 0);
			CharacterOwner->Jump();
			Advance(17);
		}
		break;

	case 17:
		if (!IsMovingOnGround())
		{
			CharacterOwner->StopJumping();
			SetWantsToSlide(false);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE CHAIN slide->jump: slideSpeed was %.0f uu/s -> airborne at %.0f uu/s (%.1f%%), "
				     "wellTimed=%.0f, velZ=%.0f (jumpZ %.0f x %.2f). The old exit ceiling would have handed back %.0f."),
				MeasureMarkA, PlanarSpeed, 100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA),
				MeasureMarkB, Velocity.Z, JumpZVelocity, GetSlideJumpZMultiplier(), MaxWalkSpeed);
			Advance(18);
		}
		else
		{
			SetWantsToSlide(true);
			if (MeasurePhaseTime > 1.f)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("MEASURE CHAIN never left the ground"));
				Advance(18);
			}
		}
		break;

	case 18:
		SetWantsToSlide(false);
		if (IsMovingOnGround() && MeasurePhaseTime > 1.2f)
		{
			Advance(19);
		}
		break;

	// --- 19-21. DASH EXIT ---------------------------------------------------------------------------
	case 19:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		if (MeasurePhaseTime > 1.2f && IsMovingOnGround())
		{
			StartDash();
			Advance(20);
		}
		break;

	case 20:
		if (IsDashing())
		{
			MeasureMarkA = PlanarSpeed;
		}
		else if (MeasureMarkA > 0.f)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE DASH exit: %.0f uu/s while dashing -> %.0f uu/s on the frame it ended "
				     "(ground limit %.0f x DashExitSpeedMultiplier %.2f = %.0f)"),
				MeasureMarkA, PlanarSpeed, GetMaxSpeed(), GetDashExitSpeedMultiplier(),
				GetMaxSpeed() * GetDashExitSpeedMultiplier());
			Advance(21);
		}
		else if (MeasurePhaseTime > 2.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE DASH never started"));
			Advance(22);
		}
		break;

	case 21:
		if (MeasurePhaseTime > 0.30f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE DASH exit +0.30s: planar=%.0f uu/s"), PlanarSpeed);
			Advance(22);
		}
		break;

	// =============================================================================================
	// 22-26. THE SLIDE-JUMP, ON CLEAN GROUND.
	//
	// The CHAIN phases above already jump out of a slide, but they do it at the middle of the arena
	// (they teleport to the world origin to isolate the drop) and the first run of this harness
	// measured a slide-jump there at 110% of the slide speed AT LAUNCH and 70% one physics step later.
	// The magnitude did not fall by 30% — the VECTOR turned about 50 degrees, which is
	// SlideAlongSurface deflecting the pawn off midfield cover. That is a measurement of the arena.
	//
	// So this repeats the move back at MeasureHomeLocation, on the strip phases 7-11 already crossed
	// without a single collision (RUN->JUMP measured exactly 100.0% there). Same reason
	// MeasureRunDirection exists.
	// =============================================================================================
	case 22:
		if (UpdatedComponent != nullptr && CharacterOwner != nullptr)
		{
			SetWantsToSlide(false);
			CharacterOwner->SetActorLocation(MeasureHomeLocation + FVector(0.f, 0.f, 40.f),
				false, nullptr, ETeleportType::TeleportPhysics);
			Velocity = FVector::ZeroVector;
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE SLIDEJUMP-CLEAN: returned to %s"),
				*MeasureHomeLocation.ToCompactString());
			Advance(23);
		}
		break;

	case 23:
		// Run up to speed on known-clear ground, then ask for the slide.
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		if (MeasurePhaseTime > 1.5f && IsMovingOnGround() && GetSlideCooldownRemaining() <= 0.f)
		{
			MeasureMarkA = PlanarSpeed;
			SetWantsToSlide(true);
			Advance(24);
		}
		break;

	case 24:
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(true);
		if (IsSliding())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDEJUMP-CLEAN entry: %.0f uu/s in -> slideSpeed=%.0f uu/s (ratio %.2f)"),
				MeasureMarkA, SlideSpeed, SlideSpeed / FMath::Max(1.f, MeasureMarkA));
			Advance(25);
		}
		else if (MeasurePhaseTime > 2.f)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("MEASURE SLIDEJUMP-CLEAN slide never latched (planar=%.0f)"), PlanarSpeed);
			Advance(27);
		}
		break;

	case 25:
		// Ride down into the well-timed window and hop. Input is HELD the whole way: without it the
		// first run of this phase let ground friction stop the pawn dead after the slide decayed out,
		// and then measured a slide-jump from a standstill (0 uu/s -> 0 uu/s), which is a measurement
		// of the harness. The bail-out fires the moment the slide-jump stops being available at all,
		// so a slide that ends without ever entering the window still produces a number.
		CharacterOwner->AddMovementInput(MeasureRunDirection, 1.f);
		SetWantsToSlide(true);
		if (IsSlideJumpWellTimed() || !IsSlideJumpAvailable() || MeasurePhaseTime > 3.f)
		{
			MeasureMarkA = FMath::Max(SlideSpeed, PlanarSpeed);
			MeasureMarkB = IsSlideJumpWellTimed() ? 1.f : 0.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDEJUMP-CLEAN arming at slideSpeed=%.0f planar=%.0f, timeLeft=%.2fs "
				     "(durationClock=%.2fs), wellTimed=%d, available=%d"),
				SlideSpeed, PlanarSpeed, GetSlideTimeLeft(), SlideTimeRemaining,
				IsSlideJumpWellTimed() ? 1 : 0, IsSlideJumpAvailable() ? 1 : 0);
			CharacterOwner->Jump();
			Advance(26);
		}
		break;

	case 26:
		if (!IsMovingOnGround())
		{
			CharacterOwner->StopJumping();
			SetWantsToSlide(false);
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE SLIDEJUMP-CLEAN: slideSpeed was %.0f uu/s -> first airborne frame %.0f uu/s "
				     "(%.1f%%), wellTimed=%.0f, velZ=%.0f. Retention %.2f x windowBonus %.2f."),
				MeasureMarkA, PlanarSpeed, 100.f * PlanarSpeed / FMath::Max(1.f, MeasureMarkA),
				MeasureMarkB, Velocity.Z, GetSlideJumpHorizontalRetention(),
				MeasureMarkB > 0.f ? GetSlideJumpWindowSpeedBonus() : 1.f);
			Advance(27);
		}
		else
		{
			SetWantsToSlide(true);
			if (MeasurePhaseTime > 1.f)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("MEASURE SLIDEJUMP-CLEAN never left the ground"));
				Advance(27);
			}
		}
		break;

	case 27:
		if (MeasurePhaseTime > 0.30f)
		{
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE SLIDEJUMP-CLEAN +0.30s airborne: planar=%.0f uu/s"), PlanarSpeed);
			Advance(28);
		}
		break;

	// =============================================================================================
	// 28-29. THE LONG STRAFE — spec v5 §1's actual question.
	//
	// Phase 2 above measures 0.40 s of strafing, which is what the Demo 5 baseline (835 -> 1036) was
	// taken over. But "how much momentum can be gained" is not a question about 0.4 s: it is about
	// what happens when a player strafes for the whole of a long jump, and then does it again off the
	// next one. So this drops the pawn from far enough up to strafe for three full seconds and prints
	// the speed every quarter of a second. Uncapped, that curve keeps climbing to MaxAirSpeed and the
	// pawn lands well over the ground limit every time; capped, it flattens onto the hard cap.
	// =============================================================================================
	case 28:
		if (UpdatedComponent != nullptr && CharacterOwner != nullptr)
		{
			const FVector Here = UpdatedComponent->GetComponentLocation();
			CharacterOwner->SetActorLocation(FVector(0.f, 0.f, Here.Z + 6000.f), false, nullptr, ETeleportType::TeleportPhysics);
			// 835 uu/s: the exact entry speed the Demo 5 baseline was measured from.
			Velocity = FVector(835.f, 0.f, 0.f);
			SetMovementMode(MOVE_Falling);
			MeasureMarkA = 835.f;
			MeasureMarkB = 0.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRCAP-LONG: dropped at planar=835 uu/s (the Demo 5 baseline entry); "
				     "strafing continuously for 3s. Demo 5 reached 1036 after 0.40s and kept climbing."));
			Advance(29);
		}
		break;

	case 29:
	{
		const FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, TravelDirection).GetSafeNormal();
		CharacterOwner->AddMovementInput(Perpendicular, 1.f);

		// A sample every 0.25s, so the curve is readable as a table rather than as 180 log lines.
		if (MeasurePhaseTime >= MeasureMarkB + 0.25f)
		{
			MeasureMarkB = MeasurePhaseTime;
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRCAP-LONG t=%.2fs planar=%7.1f uu/s (turned %5.1f deg, gainScale here %.4f)"),
				MeasurePhaseTime, PlanarSpeed, TurnedDegrees(), GetAirStrafeGainScale(PlanarSpeed));
		}

		if (MeasurePhaseTime > 3.0f || IsMovingOnGround())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("MEASURE AIRCAP-LONG RESULT: 835 -> %.0f uu/s over %.2fs of continuous strafing "
				     "(%.1f%% of entry; soft cap %.0f, hard cap %.0f, falloff=%d hardCap=%d). "
				     "Vector turned %.0f deg, so the TURN still costs nothing."),
				PlanarSpeed, MeasurePhaseTime, 100.f * PlanarSpeed / 835.f,
				GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(),
				IsAirStrafeFalloffEnabled() ? 1 : 0, IsAirStrafeHardCapEnabled() ? 1 : 0,
				TurnedDegrees());
			UE_LOG(LogTraceGame, Display, TEXT("MEASURE ---- end"));
			Advance(30);
		}
		break;
	}

	default:
		break;
	}
}

// =================================================================================================
// THE RUBBER-BAND INSTRUMENT AND THE LEDGE TEST (spec v5 §7)
// =================================================================================================

void UTraceCharacterMovementComponent::OnClientCorrectionReceived(
	FNetworkPredictionData_Client_Character& ClientData, float TimeStamp, FVector NewLocation,
	FVector NewVelocity, FMovementBaseInterfaceData* NewMovementBaseInterfaceData, FName NewBaseBoneName,
	bool bHasBase, bool bBaseRelativePosition, uint8 ServerMovementMode, FVector ServerGravityDirection)
{
	// The error the server is about to correct, measured BEFORE Super applies it — afterwards the
	// pawn is already at the corrected position and the number is zero.
	const FVector Before = (UpdatedComponent != nullptr) ? UpdatedComponent->GetComponentLocation() : NewLocation;
	const float PositionError = static_cast<float>(FVector::Dist(Before, NewLocation));
	const float VelocityError = static_cast<float>(FVector::Dist(Velocity, NewVelocity));
	const uint8 LocalMode = static_cast<uint8>(MovementMode.GetValue());

	++CorrectionCount;
	CorrectionErrorTotal += PositionError;
	CorrectionErrorWorst = FMath::Max(CorrectionErrorWorst, PositionError);

	// SPEC v8 §1. Attribute this correction to a dash if one is live or recently ended. See
	// BeginDash() for why the window outlives the dash and why replays do not count as new dashes.
	const UWorld* CorrectionWorld = GetWorld();
	const bool bInDashWindow = (CorrectionWorld != nullptr)
		&& (static_cast<float>(CorrectionWorld->GetTimeSeconds()) <= DashNetAttributionUntil);
	if (bInDashWindow)
	{
		++DashNetCorrectionsInDash;
		DashNetCorrectionErrorInDash += PositionError;
	}

	// SPEC v8 §7. The same attribution for the wall jump: "fully client-predicted" is the claim, and a
	// correction landing inside the launch window is what falsifies it.
	if (CorrectionWorld != nullptr
		&& static_cast<float>(CorrectionWorld->GetTimeSeconds()) <= WallJumpAttributionUntil)
	{
		++WallJumpCorrectionsInWindow;
	}

	// PATCH 28 §5. The same attribution for surf: "zero prediction corrections while surfing" is the
	// competitive-integrity claim, and a correction landing inside a ride is what falsifies it. Read
	// on a CLIENT — an authoritative pawn cannot be corrected and reports zero for a reason that has
	// nothing to do with ramps.
	if (CorrectionWorld != nullptr
		&& static_cast<float>(CorrectionWorld->GetTimeSeconds()) <= SurfAttributionUntil)
	{
		++SurfCorrectionsInWindow;
	}

	if (AreMoveCorrectionsLogged())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("CORRECTION %-16s #%d t=%.3f posErr=%7.2fuu velErr=%7.1fuu/s mode(local=%d server=%d) "
			     "z=%.1f grounded=%d grace=%.3f slide=%.3f | mean=%.2f worst=%.2f"),
			*GetNameSafe(CharacterOwner), CorrectionCount, TimeStamp, PositionError, VelocityError,
			static_cast<int32>(LocalMode), static_cast<int32>(ServerMovementMode), Before.Z,
			IsMovingOnGround() ? 1 : 0, GroundGraceRemaining, SlideTimeRemaining,
			CorrectionErrorTotal / FMath::Max(1, CorrectionCount), CorrectionErrorWorst);

		// The v8 §1 line. Printed next to the correction it describes so "was this one a dash?" is
		// answerable per correction rather than only in the summary.
		UE_LOG(LogTraceGame, Display,
			TEXT("DASHNET   %-16s inDash=%d | dashes=%d corrInDash=%d rate=%.2f/dash meanDashErr=%.2fuu"),
			*GetNameSafe(CharacterOwner), bInDashWindow ? 1 : 0, DashNetDashCount,
			DashNetCorrectionsInDash,
			DashNetCorrectionsInDash / static_cast<float>(FMath::Max(1, DashNetDashCount)),
			DashNetCorrectionErrorInDash / static_cast<float>(FMath::Max(1, DashNetCorrectionsInDash)));
	}

	Super::OnClientCorrectionReceived(ClientData, TimeStamp, NewLocation, NewVelocity,
		NewMovementBaseInterfaceData, NewBaseBoneName, bHasBase, bBaseRelativePosition,
		ServerMovementMode, ServerGravityDirection);
}

void UTraceCharacterMovementComponent::TickLedgeTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceLedgeTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// Locally controlled only — but in ANY net mode, unlike TickMomentumMeasure. The whole point is
	// to measure what a networked CLIENT experiences at a ledge, so restricting this to standalone
	// would measure everything except the bug.
	if (!CharacterOwner->IsLocallyControlled()
		|| Cast<APlayerController>(CharacterOwner->GetController()) == nullptr)
	{
		return;
	}

	// ============================================================================================
	// NOT ON A REPLAYED MOVE. SPEC v12 §5 — AND THE PREVIOUS VERSION OF THIS HARNESS PRODUCED
	// NOTHING BUT GARBAGE FOR WANT OF THIS LINE.
	//
	// OnMovementUpdated runs once per move on the record pass AND once per move on every replay. A
	// correction replays the whole unacknowledged move queue inside a single frame, so without this
	// guard the phase clock below advances by the sum of a dozen moves' DeltaSeconds in one frame,
	// the harness burns through all eight runs in under a second of wall time, and the input it
	// issues is issued from inside a replay where it means nothing. MEASURED, on a 40 ms client:
	// eight "runs" completed in 0.85 s of wall clock, every one of them reporting "never reached the
	// block", and zero contact frames recorded. Every other measurement path in this file (BeginDash,
	// TickWallStickSample, the wall-jump counters) already carries this guard; this one did not.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	UWorld* TestWorld = GetWorld();
	if (TestWorld == nullptr)
	{
		return;
	}

	// --- Ground-state flip counter, running the whole time -------------------------------------
	//
	// THE PRIMARY MEASUREMENT. A pawn that runs onto a raised section should change ground state
	// twice: airborne on the jump, grounded on the landing. Every flip beyond that is the capsule
	// oscillating on the lip, and every oscillation is a chance for the client and the server to
	// disagree about which velocity model to run.
	const bool bGroundedNow = IsMovingOnGround();
	if (LedgeTestTime >= 0.f && bGroundedNow != (bLedgeTestWasGrounded != 0))
	{
		++LedgeTestGroundFlips;
	}
	bLedgeTestWasGrounded = bGroundedNow ? 1 : 0;

	if (LedgeTestTime < 0.f)
	{
		if (TestWorld->GetTimeSeconds() < 5.f || !IsMovingOnGround())
		{
			return;
		}
		LedgeTestTime = static_cast<float>(TestWorld->GetTimeSeconds());
		LedgeTestPhase = 0;
		LedgeTestPhaseTime = LedgeTestTime;
		LedgeTestGroundFlips = 0;
		LedgeTestRun = 0;
		CorrectionCount = 0;
		CorrectionErrorTotal = 0.f;
		CorrectionErrorWorst = 0.f;

		LedgeTestContacts = 0;
		LedgeTestContactFlips = 0;
		LedgeTestContactCorrections = 0;
		LedgeTestWorstContactFlips = 0;
		LedgeTestWorstContactErr = 0.f;
		LedgeTestKeptFractionTotal = 0.f;
		LedgeTestWorstKeptFraction = 1.f;
		LedgeTestLandedOnTop = 0;
		LedgeTestPulledBack = 0;

		// --- "-TraceLedgeLegacy": THE DEMO 5 ARM, AND THE REASON THIS TEST CAN GO RED ------------
		//
		// A harness that has never failed is not evidence. This one measures a prediction desync at a
		// lip, and if the shipped build simply has no desync then every number it prints is a pass by
		// default and proves nothing about whether the fixes are load-bearing — which is exactly how
		// the Demo 5 verification went wrong the first time.
		//
		// So the arm restores the pre-fix geometry handling: PerchRadiusThreshold back to the engine
		// default of 0, which disables the reduced-radius perch test and puts the walking/falling
		// decision back on a sub-uu knife edge. Pair it with
		//   -ini:Game:[/Script/Trace.TraceSettings]:LedgeGroundGraceSeconds=0.0
		// and the component is behaving exactly as it did when the user reported the rubber-band.
		//
		// Component-local and dev-only: it writes this pawn's own field, so it cannot leak into a real
		// match, and it is deliberately NOT a designer knob — there is no shipping reason to want the
		// broken behaviour back.
		if (FParse::Param(FCommandLine::Get(), TEXT("TraceLedgeLegacy")))
		{
			PerchRadiusThreshold = 0.f;
			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE ---- LEGACY ARM: PerchRadiusThreshold forced to 0 (the Demo 5 state). "
				     "Pair with -ini:Game:[/Script/Trace.TraceSettings]:LedgeGroundGraceSeconds=0.0"));
		}

		// netMode 3 is NM_Client. THE ONLY ARM OF THIS TEST THAT ANSWERS THE QUESTION IS netMode=3:
		// a correction count taken on a listen server is structurally zero, because the server never
		// corrects itself. A run that prints netMode=0 or 2 here has measured the geometry and nothing
		// about prediction, and this project has already shipped one "verification" of that shape.
		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE ---- begin. netMode=%d role=%d (mantle: REMOVED, spec v12 §5) grace=%.3f "
			     "perch=%.1f jumpZ=%.0f apex=%.0fuu"),
			static_cast<int32>(GetNetMode()), static_cast<int32>(CharacterOwner->GetLocalRole()),
			GetLedgeGroundGraceSeconds(), PerchRadiusThreshold, JumpZVelocity,
			(JumpZVelocity * JumpZVelocity) / (2.f * FMath::Max(1.f, GetGravityZ() * -1.f)));
	}

	// ============================================================================================
	// THE PHASE CLOCKS ARE WORLD TIME, NOT A SUM OF DeltaSeconds. SPEC v12 §5, AND THIS IS THE
	// SECOND HARNESS BUG THE MANTLE-REMOVAL PASS HAD TO FIX BEFORE ANY NUMBER HERE MEANT ANYTHING.
	//
	// The old clocks accumulated the DeltaSeconds handed to OnMovementUpdated. MEASURED on a 40 ms
	// client: the accumulated clock passed 40 "seconds" in 1.4 s of wall time — roughly 28x — so
	// every phase timed out almost immediately and the run reported "found no arena ledge after 40s
	// of searching" having actually searched for about a second. The bClientUpdating guard above
	// removes the replay passes but not whatever else re-enters this path per frame, and chasing that
	// is beside the point: A HARNESS CLOCK MUST NOT DEPEND ON HOW MANY TIMES THE FUNCTION RUNS.
	// World time is the quantity the phases are actually reasoning about ("run at it for up to 8
	// seconds"), it is frozen during a replay so a replayed move cannot advance it, and it is immune
	// to the whole class of bug. LedgeTestTime and LedgeTestPhaseTime are therefore START STAMPS.
	//
	// DeltaSeconds is still right for the flip counter above: that counts events, not time.
	// ============================================================================================
	const float NowSeconds = static_cast<float>(TestWorld->GetTimeSeconds());
	const float PhaseElapsed = NowSeconds - LedgeTestPhaseTime;
	const float TotalElapsed = NowSeconds - LedgeTestTime;

	// HOW MANY CONTACTS ONE SESSION IS, AND WHY IT IS FIVE RATHER THAN EIGHT.
	//
	// The harness pawn is a live player in a live match: it sprints across the arena past bots that
	// are laying lethal trails, and MEASURED it dies on a ~34 s cycle. Every death respawns it, which
	// builds a new movement component, which resets this harness to run 1 — so a session longer than
	// one life NEVER REPORTS. Three consecutive attempts at eight contacts got to run 2 and restarted.
	// Five contacts at ~3.3 s each is about 17 s, which fits comfortably inside a life.
	//
	// Overridable with "-TraceLedgeRuns=N" so the two arms can be held at the SAME N — comparing a
	// five-contact arm against an eight-contact one would be comparing sample sizes as well as
	// behaviour, and the per-contact averages are the whole point.
	static const int32 RequiredRuns = []
	{
		int32 Value = 5;
		FParse::Value(FCommandLine::Get(), TEXT("TraceLedgeRuns="), Value);
		return FMath::Clamp(Value, 1, 32);
	}();

	// A HARD DEADLINE ON THE WHOLE SESSION. Phases 1 and 2 re-probe when they cannot reach their
	// target, which is the right recovery but is also a loop; without this a run that can never find
	// usable geometry would sit in it forever and the operator would read the silence as "still
	// working". Reported as a failure, in the same words as the search failure, for the same reason.
	// THE VERDICT, IN ONE PLACE, SO A PARTIAL SESSION STILL REPORTS.
	//
	// MEASURED, and it is why this is a lambda rather than a block at the end of phase 4: the match
	// relocates the harness pawn to a spawn pad roughly 13 s into every session (a goal, a kickoff or
	// a death — the harness cannot tell and does not need to). Sessions that lost their target after
	// two contacts used to print NOTHING AT ALL and simply sat there, which is the worst possible
	// failure mode for a diagnostic: indistinguishable from "still running" and from "all clear".
	// Now every session reports what it actually collected, with the count attached so a thin sample
	// cannot be mistaken for a thorough one.
	auto LogVerdict = [this, Runs = RequiredRuns](const TCHAR* Why)
	{
		if (LedgeTestContacts <= 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("LEDGE ---- %s with ZERO contacts. NO MEASUREMENT - do NOT read this as a pass."), Why);
			return;
		}

		const float Denominator = static_cast<float>(LedgeTestContacts);

		// Read it in this order:
		//   netMode      must be 3 (NM_Client) or the correction columns are structurally zero and the
		//                run has proved nothing about prediction.
		//   corr/contact server corrections landing between the jump and the settle. THIS IS THE DESYNC
		//                NUMBER. Non-zero with a meaningful worstErr is a real client/server
		//                disagreement at the lip; zero means the "rubber banding" was never a network
		//                symptom at these settings.
		//   flips        ground-state changes per contact. 2 is clean (leave, arrive). 3+ means the
		//                capsule is chattering on the edge, which is the mechanism that MAKES
		//                corrections, so it leads the correction number.
		//   kept         planar speed retained across the lip. The stall measure.
		//   pulledBack   contacts that ended BEHIND the jump position. The literal rubber-band.
		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE ---- %s. netMode=%d contacts=%d/%d ledgeHeight=%.1fuu | flips/contact=%.2f "
			     "(2.00 is clean, worst=%d) | corr/contact=%.2f (total=%d, worstErr=%.2fuu) | "
			     "kept=%.3f (worst=%.3f) | onTop=%d pulledBack=%d | mantle=REMOVED grace=%.3f perch=%.1f"),
			Why, static_cast<int32>(GetNetMode()), LedgeTestContacts, Runs, LedgeTestLedgeHeight,
			LedgeTestContactFlips / Denominator, LedgeTestWorstContactFlips,
			LedgeTestContactCorrections / Denominator, LedgeTestContactCorrections,
			LedgeTestWorstContactErr,
			LedgeTestKeptFractionTotal / Denominator, LedgeTestWorstKeptFraction,
			LedgeTestLandedOnTop, LedgeTestPulledBack,
			GetLedgeGroundGraceSeconds(), PerchRadiusThreshold);
	};

	// 90 s, not 420: the pawn is relocated about every 13 s, so a session that has not finished in a
	// minute and a half is not going to. Reporting early beats reporting nothing.
	if (LedgeTestPhase != 9 && TotalElapsed > 90.f && LedgeTestRun < RequiredRuns)
	{
		LogVerdict(TEXT("CUT SHORT (the match relocated the pawn)"));
		LedgeTestPhase = 9;
		return;
	}

	auto Advance = [this, NowSeconds](int32 NextPhase)
	{
		LedgeTestPhase = NextPhase;
		LedgeTestPhaseTime = NowSeconds;
	};

	const UCapsuleComponent* TestCapsule = CharacterOwner->GetCapsuleComponent();
	const float TestRadius = (TestCapsule != nullptr) ? TestCapsule->GetScaledCapsuleRadius() : 42.f;
	const float TestHalfHeight = (TestCapsule != nullptr) ? TestCapsule->GetScaledCapsuleHalfHeight() : 88.f;

	// Planar distance from the capsule SURFACE to the face it is running at. Every phase below is
	// driven off this one number, so it is computed once.
	const FVector Here = UpdatedComponent->GetComponentLocation();
	const float DistToFace = static_cast<float>(FVector::Dist2D(Here, LedgeTestFacePoint)) - TestRadius;

	switch (LedgeTestPhase)
	{
	// --- 0. FIND A REAL LEDGE IN THE ARENA ------------------------------------------------------
	//
	// SPEC v12 §5 REPLACED THE SPAWNED TEST BLOCK WITH THE ARENA'S OWN GEOMETRY, AND THE OLD
	// APPROACH WAS NOT SALVAGEABLE ON A CLIENT. It spawned an AStaticMeshActor locally and teleported
	// the pawn to a mark in front of it. Both are illegal from a client:
	//
	//   * A runtime-spawned AStaticMeshActor does not replicate its mesh or its collision. Only the
	//     machine that spawned it has the block. Running the harness on the client alone meant the
	//     client climbed a solid box the server believed was empty air; running it on both meant two
	//     independently-spawned boxes that agree only if both searches happen to pick the same spot.
	//   * SetActorLocation on a client's own autonomous proxy is a position the server never
	//     simulated, so the very next ServerMove is rejected and corrected. MEASURED: corrections of
	//     10030 uu, 10144 uu, 10312 uu — three orders of magnitude larger than any ledge effect, and
	//     manufactured entirely by the harness. The pawn was yanked back on every reset and never
	//     reached its own block on any of eight runs.
	//
	// The arena already contains exactly the geometry this test wants: TraceArenaBuilder scatters
	// cover boxes at 1x player height (176 uu — see its "1x / 2x / 3.5x player height" comment),
	// which is precisely the "raised section" class the complaint is about. It is built from a seed
	// at map load, identically on every machine, and it is real level geometry rather than something
	// this file invented — so client and server are guaranteed to agree about it, and the harness
	// cannot be the source of the disagreement it exists to detect.
	//
	// So: probe outward for a vertical face with a walkable top of the right height, and then drive
	// the pawn with MOVEMENT INPUT ONLY for the rest of the test. No spawn, no teleport, nothing that
	// the prediction path does not already carry.
	case 0:
	{
		FCollisionQueryParams ProbeParams;
		FCollisionResponseParams ProbeResponse;
		InitCollisionParams(ProbeParams, ProbeResponse);
		ProbeParams.bTraceComplex = false;
		ProbeParams.AddIgnoredActor(CharacterOwner);
		const ECollisionChannel ProbeChannel = UpdatedComponent->GetCollisionObjectType();

		const float FeetZ = Here.Z - TestHalfHeight;

		// The jump apex, which is what makes a ledge "the top edge of an obstacle" rather than "a
		// wall". Only ledges the pawn can actually get on top of are of any interest here.
		const float Apex = (JumpZVelocity * JumpZVelocity) / (2.f * FMath::Max(1.f, GetGravityZ() * -1.f));
		const float MinLedge = 100.f;
		const float MaxLedge = Apex - 5.f;

		bool bFound = false;
		float BestDistance = TNumericLimits<float>::Max();

		for (int32 Step = 0; Step < 24; ++Step)
		{
			const float Yaw = Step * (360.f / 24.f);
			const FVector Direction = FRotator(0.f, Yaw, 0.f).Vector();

			// Knee height, so a 176 uu box is hit on its face rather than missed over the top.
			const FVector ProbeStart(Here.X, Here.Y, FeetZ + 45.f);
			FHitResult FaceHit;
			if (!TestWorld->LineTraceSingleByChannel(FaceHit, ProbeStart, ProbeStart + Direction * 4000.f,
				ProbeChannel, ProbeParams, ProbeResponse))
			{
				continue;
			}

			// A FACE, not a ramp and not the floor.
			if (FMath::Abs(FaceHit.ImpactNormal.Z) > 0.3f)
			{
				continue;
			}

			// LEVEL GEOMETRY, NOT A PLAYER. MEASURED, and it is the trap this probe falls into by
			// default: a Trace character's capsule is 176 uu tall with vertical sides and a walkable
			// cap, so it passes every geometric test a 1x-player-height cover box passes. The first
			// client run of this probe locked onto "TraceCharacter_8" at 2887 uu, reported a 168.7 uu
			// "ledge", and then spent the whole test walking toward a bot that was walking away — the
			// harness never got within 1800 uu of its own target. A moving obstacle is also the one
			// thing guaranteed to make client and server disagree for reasons that have nothing to do
			// with a lip, which would have poisoned the very number this test exists to produce.
			if (Cast<APawn>(FaceHit.GetActor()) != nullptr)
			{
				continue;
			}

			// SQUARE ON, NOT GLANCING. The pawn runs along Direction; if the face is steeply angled to
			// that, the capsule slides along it instead of arriving at the lip, and the contact being
			// measured is a wall-slide rather than a ledge landing.
			if (FVector::DotProduct(-FaceHit.ImpactNormal, Direction) < 0.85f)
			{
				continue;
			}

			// Far enough away to build up to full speed on the approach, near enough that the run is
			// short. 800 uu/s over ~1.2 s of run-up is the shape wanted.
			const float FaceDistance = static_cast<float>(FVector::Dist2D(Here, FaceHit.ImpactPoint));
			if (FaceDistance < 700.f || FaceDistance > 9000.f || FaceDistance >= BestDistance)
			{
				continue;
			}

			// How tall is it? Trace down from above, just past the face.
			const FVector TopProbeXY = FaceHit.ImpactPoint + Direction * (TestRadius + 20.f);
			const FVector TopStart(TopProbeXY.X, TopProbeXY.Y, FeetZ + Apex + 200.f);
			const FVector TopEnd(TopProbeXY.X, TopProbeXY.Y, FeetZ - 20.f);
			FHitResult TopHit;
			if (!TestWorld->LineTraceSingleByChannel(TopHit, TopStart, TopEnd, ProbeChannel,
				ProbeParams, ProbeResponse))
			{
				continue;
			}

			const float LedgeHeight = static_cast<float>(TopHit.ImpactPoint.Z) - FeetZ;
			if (LedgeHeight < MinLedge || LedgeHeight > MaxLedge)
			{
				continue;
			}

			// Walkable on top, or landing on it is not the test.
			if (TopHit.ImpactNormal.Z < GetWalkableFloorZ())
			{
				continue;
			}

			// Room to STAND up there. A lip with a pillar on it is a different experiment.
			const FCollisionShape StandShape = FCollisionShape::MakeCapsule(TestRadius, TestHalfHeight);
			const FVector StandSpot(TopProbeXY.X, TopProbeXY.Y, TopHit.ImpactPoint.Z + TestHalfHeight + 4.f);
			if (TestWorld->OverlapBlockingTestByChannel(StandSpot, FQuat::Identity, ProbeChannel,
				StandShape, ProbeParams, ProbeResponse))
			{
				continue;
			}

			// An unobstructed FINAL APPROACH — the last 1500 uu before the face, which is the only
			// part the pawn sprints through. Deliberately NOT the whole line from where the pawn is
			// standing right now: the arena is 33600 uu long and full of cover, so demanding a clear
			// capsule sweep across several thousand uu rejected every candidate in the level. MEASURED:
			// with the full-path test the first client run reported "found no arena ledge between 100
			// and 182uu within 3500uu" and took no measurement at all. Phase 1 walks the pawn to the
			// mark; only what happens after the mark has to be clear.
			const FCollisionShape RunShape = FCollisionShape::MakeCapsule(TestRadius, TestHalfHeight);
			const FVector FaceAtRunZ(FaceHit.ImpactPoint.X, FaceHit.ImpactPoint.Y, Here.Z);
			const FVector RunStart = FaceAtRunZ - Direction * 1500.f;
			const FVector RunEnd = FaceAtRunZ - Direction * (TestRadius + 10.f);
			FHitResult PathHit;
			if (TestWorld->SweepSingleByChannel(PathHit, RunStart, RunEnd, FQuat::Identity, ProbeChannel,
				RunShape, ProbeParams, ProbeResponse))
			{
				continue;
			}

			// ...and the mark itself has to be somewhere the pawn can stand.
			if (TestWorld->OverlapBlockingTestByChannel(RunStart, FQuat::Identity, ProbeChannel,
				RunShape, ProbeParams, ProbeResponse))
			{
				continue;
			}

			BestDistance = FaceDistance;
			LedgeTestFacePoint = FaceHit.ImpactPoint;
			LedgeTestTopPoint = TopHit.ImpactPoint;
			LedgeTestLedgeHeight = LedgeHeight;
			LedgeTestRunDirection = Direction;
			LedgeTestBlock = FaceHit.GetActor();
			bFound = true;
		}

		if (!bFound)
		{
			// Nothing in reach from here — so WALK, and probe again next frame. The 24-direction probe
			// is a snapshot of one standing position, and a spawn pad sits in an endzone with the open
			// field (and its cover) several thousand uu away. Moving toward the field centre costs
			// nothing and turns "no ledge visible from the spawn" into "no ledge in the arena", which
			// are very different claims.
			FVector TowardCentre = -Here;
			TowardCentre.Z = 0.f;
			if (TowardCentre.Normalize())
			{
				CharacterOwner->AddMovementInput(TowardCentre, 1.f);
			}

			if (PhaseElapsed > 90.f)
			{
				// Loud, and it gives up rather than measuring something else. THE ABSENCE OF A
				// MEASUREMENT IS NOT A PASS, and this line says so in the log so that a later reader
				// cannot mistake a silent run for a clean one.
				UE_LOG(LogTraceGame, Warning,
					TEXT("LEDGE found no arena ledge between %.0f and %.0fuu after 90s of searching "
					     "(now at %s). NO MEASUREMENT TAKEN - do NOT read the absence of corrections "
					     "as a pass."),
					MinLedge, MaxLedge, *Here.ToCompactString());
				Advance(9);
			}
			break;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE found an arena ledge: actor=%s height=%.1fuu (apex=%.0fuu, clearance=%+.1fuu) "
			     "face=%s top=%s distance=%.0fuu"),
			*GetNameSafe(LedgeTestBlock.Get()), LedgeTestLedgeHeight, Apex, Apex - LedgeTestLedgeHeight,
			*LedgeTestFacePoint.ToCompactString(), *LedgeTestTopPoint.ToCompactString(), BestDistance);
		Advance(1);
		break;
	}

	// --- 1. GET TO THE RUN-UP MARK, ON FOOT -----------------------------------------------------
	//
	// Both the first approach and the reset between runs, and it is movement input rather than a
	// teleport for the reason phase 0 spells out. Walks toward the face when too far and away from it
	// when too near, so it also carries the pawn back DOWN off the top of the ledge it just landed on.
	//
	// The band is 1000-1600 uu: far enough to reach the 800 uu/s ground cap before the jump (which
	// takes about 250 uu from a standing start at 4096 uu/s^2), near enough that the walk is short.
	// Kept deliberately tight in TIME rather than generous in distance — this rig shares a machine
	// with several other agents' editors, and a session that takes three minutes gets OOM-killed
	// before it reports. A measurement that never finishes is a measurement you do not have.
	case 1:
	{
		const bool bTooFar = DistToFace > 1600.f;
		CharacterOwner->AddMovementInput(bTooFar ? LedgeTestRunDirection : -LedgeTestRunDirection, 1.f);

		const bool bAtMark = DistToFace >= 1000.f && DistToFace <= 1600.f && IsMovingOnGround();
		if (bAtMark)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE run %d: at the mark, %.0fuu from the face"), LedgeTestRun + 1, DistToFace);
			Advance(2);
		}
		else if (PhaseElapsed > 45.f)
		{
			// RE-PROBE RATHER THAN MEASURE ANYWAY. Failing to reach the mark means the target is not
			// what the probe thought it was (something moved, or the path is blocked), and running the
			// contact from the wrong place would produce a number that looks like a result.
			UE_LOG(LogTraceGame, Warning,
				TEXT("LEDGE run %d: could not reach the mark in 45s (dist=%.0f) - re-probing"),
				LedgeTestRun + 1, DistToFace);
			Advance(0);
		}
		break;
	}

	// --- 2. RUN AT IT, AND JUMP AT THE EDGE -----------------------------------------------------
	//
	// The jump is triggered on DISTANCE TO THE FACE, not on a stopwatch. "Jumping on the edge of a
	// raised section" is a specific input — a jump made close enough that the capsule arrives at the
	// lip rather than sailing over it — and a fixed delay produced a different jump every time the
	// frame rate moved.
	//
	// SPEC v12 §5 SWEEPS THE DISTANCE ACROSS THE RUNS instead of using one value, and that is
	// deliberate: at 800 uu/s the pawn's feet are above a 176 uu lip only between t=0.39 s and
	// t=0.91 s of a 640 uu/s jump, so the jump distance decides whether the capsule clips the face,
	// catches the very corner, or lands cleanly on the top. All three are "hitting the top edge of an
	// obstacle" and a single distance would only ever exercise one of them. The sweep spans 260-400 uu
	// whatever the run count is, so changing -TraceLedgeRuns changes the SAMPLE SIZE and not the band
	// being sampled — which is what makes two arms at different N still qualitatively comparable, and
	// two arms at the same N directly so.
	case 2:
	{
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);

		const float JumpDistance = 260.f + (140.f * static_cast<float>(LedgeTestRun))
			/ static_cast<float>(FMath::Max(1, RequiredRuns - 1));

		if (DistToFace < JumpDistance && IsMovingOnGround() && GetPlanarSpeed() > 600.f)
		{
			// SPEC v12 §5. SNAPSHOT EVERYTHING THE CONTACT WILL BE MEASURED AGAINST, on the jump frame.
			// Deltas of counters that already exist, rather than a second set of counters:
			// CorrectionCount and CorrectionErrorWorst are maintained by OnClientCorrectionReceived for
			// every correction this pawn takes, so a delta of them cannot fall out of step with the
			// corrections themselves the way a parallel attribution clock can.
			LedgeTestFlipsAtJump = LedgeTestGroundFlips;
			LedgeTestCorrAtJump = CorrectionCount;
			LedgeTestWorstErrAtJump = CorrectionErrorWorst;
			LedgeTestSpeedAtJump = GetPlanarSpeed();
			LedgeTestPosAtJump = Here;

			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE run %d: JUMP at %.0fuu from the face (target %.0f), planar=%.0f uu/s"),
				LedgeTestRun + 1, DistToFace, JumpDistance, GetPlanarSpeed());
			CharacterOwner->Jump();
			Advance(3);
		}
		else if (PhaseElapsed > 8.f)
		{
			// BACK TO THE MARK, NOT BACK TO THE SEARCH. MEASURED: a goal reset teleports every pawn to
			// its spawn pad mid-test — "never reached the face (dist=6318, planar=180)" is the harness
			// finding itself 6300 uu from the ledge it was running at — and re-probing from a spawn pad
			// finds nothing, because the pads sit inside an endzone with no 1x-height cover in range.
			// The run then sat in the search for the rest of the session. The ledge that was found is
			// still there and still valid; phase 1 already knows how to walk to it.
			UE_LOG(LogTraceGame, Warning,
				TEXT("LEDGE run %d: never reached the face (dist=%.0f, planar=%.0f, grounded=%d) "
				     "- walking back to the mark"),
				LedgeTestRun + 1, DistToFace, GetPlanarSpeed(), IsMovingOnGround() ? 1 : 0);
			Advance(1);
		}
		break;
	}

	// --- 3. HOLD FORWARD THROUGH THE CONTACT ----------------------------------------------------
	//
	// One log line per frame here, and only here: the whole contact event at full resolution. This is
	// what says whether the pawn was airborne and pushing when it met the lip, whether it stalled,
	// and on which frame it changed ground state.
	//
	// The per-frame line prints the engine's own movement mode and ground answer — the quantity the
	// client and the server have to agree about — and the running per-contact flip and correction
	// deltas, so the log shows the moment a disagreement lands rather than only the total at the end.
	case 3:
	{
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);
		CharacterOwner->StopJumping();

		UE_LOG(LogTraceGame, Display,
			TEXT("LEDGE   contact t=%.3f z=%7.1f feet=%7.1f planar=%6.0f velZ=%7.0f mode=%d "
			     "grounded=%d grace=%.3f dist=%6.0f | flips=%d corr=%d"),
			PhaseElapsed, Here.Z, Here.Z - TestHalfHeight, GetPlanarSpeed(), Velocity.Z,
			static_cast<int32>(MovementMode), IsMovingOnGround() ? 1 : 0, GroundGraceRemaining,
			DistToFace, LedgeTestGroundFlips - LedgeTestFlipsAtJump,
			CorrectionCount - LedgeTestCorrAtJump);

		// THE CONTACT WINDOW ENDS WHEN THE PAWN HAS SETTLED, NOT ON A STOPWATCH — but with a stopwatch
		// backstop, because "never settles" is itself a result this test has to be able to report.
		// 0.45 s is past the earliest possible landing (the feet cross 176 uu at t=0.39 s), so a
		// window cannot close while the pawn is still on the way up.
		const bool bSettled = IsMovingOnGround() && PhaseElapsed > 0.42f;
		if (bSettled || PhaseElapsed > 1.5f)
		{
			Advance(4);
		}
		break;
	}

	// --- 4. SETTLE, THEN SCORE THE CONTACT ------------------------------------------------------
	//
	// SPEC v12 §5. This is where the diagnosis actually happens, and every number here is a delta
	// against the jump-frame snapshot rather than a session total, so "which contact was bad" is
	// answerable instead of only "how many were there in eight runs".
	case 4:
		CharacterOwner->AddMovementInput(LedgeTestRunDirection, 1.f);
		if (PhaseElapsed > 0.4f)
		{
			++LedgeTestRun;

			const int32 ContactFlips = LedgeTestGroundFlips - LedgeTestFlipsAtJump;
			const int32 ContactCorr = CorrectionCount - LedgeTestCorrAtJump;
			const float ContactWorstErr = FMath::Max(0.f, CorrectionErrorWorst - LedgeTestWorstErrAtJump);

			// KEPT: planar speed after the lip over planar speed at the jump. A clean crossing keeps
			// essentially all of it (the ground model bleeds overspeed, but a walk-speed approach has
			// none to bleed). Well under 1.0 is the "stall" the request asks about, in a number.
			const float Kept = GetPlanarSpeed() / FMath::Max(1.f, LedgeTestSpeedAtJump);

			// ADVANCE: how far the pawn actually got along its run direction across the whole contact.
			// Negative is a PULL-BACK — the pawn ended up behind where it jumped from — which is the
			// literal reading of "rubber banding" and is worth counting separately from a stall.
			const float Advance2D = static_cast<float>(
				FVector::DotProduct(Here - LedgeTestPosAtJump, LedgeTestRunDirection));

			// ON TOP: did the jump actually end up on the raised section?
			const bool bOnTop = (Here.Z - TestHalfHeight) > (LedgeTestTopPoint.Z - 20.f);

			++LedgeTestContacts;
			LedgeTestContactFlips += ContactFlips;
			LedgeTestContactCorrections += ContactCorr;
			LedgeTestWorstContactFlips = FMath::Max(LedgeTestWorstContactFlips, ContactFlips);
			LedgeTestWorstContactErr = FMath::Max(LedgeTestWorstContactErr, ContactWorstErr);
			LedgeTestKeptFractionTotal += Kept;
			LedgeTestWorstKeptFraction = FMath::Min(LedgeTestWorstKeptFraction, Kept);
			LedgeTestLandedOnTop += bOnTop ? 1 : 0;
			LedgeTestPulledBack += (Advance2D < 0.f) ? 1 : 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("LEDGE run %2d: feet=%7.1f (ledgeTop %7.1f) onTop=%d grounded=%d | THIS CONTACT: "
				     "flips=%d corr=%d worstErr=%6.2fuu kept=%.3f (%4.0f -> %4.0f uu/s) advance=%+7.1fuu"),
				LedgeTestRun, Here.Z - TestHalfHeight, LedgeTestTopPoint.Z,
				bOnTop ? 1 : 0, IsMovingOnGround() ? 1 : 0,
				ContactFlips, ContactCorr, ContactWorstErr, Kept,
				LedgeTestSpeedAtJump, GetPlanarSpeed(), Advance2D);

			if (LedgeTestRun >= RequiredRuns)
			{
				LogVerdict(TEXT("end"));
				Advance(9);
			}
			else
			{
				Advance(1);
			}
		}
		break;

	default:
		break;
	}
}

// -------------------------------------------------------------------------------------------
// Trace.DashVectorTest — the measured verification of spec v7 §5
// -------------------------------------------------------------------------------------------
//
// Offline and deterministic: it calls the SHIPPING ComputeDashDirection() with synthetic
// (acceleration, aim rotation) pairs, so what it prints is the function the game runs, not a
// re-derivation of it. Yaw is pinned to 0 so every expected answer is a world axis and a wrong sign
// is obvious by eye; MaxAcceleration is used as the input magnitude because that is what
// ATraceCharacter::DoMove actually produces once UCharacterMovementComponent has scaled the stick.
//
// The row that matters most is the last pair: W-only and W+D must print the SAME |v| and the same
// reach. "add the two vectors and normalize to one dash length" is a claim about distance, and this
// is the only place it is checked.

// -------------------------------------------------------------------------------------------
// -TraceDashPitchTest — spec v7 §5 measured on a real pawn
// -------------------------------------------------------------------------------------------

void UTraceCharacterMovementComponent::TickDashPitchTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceDashPitchTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	APlayerController* TestController = Cast<APlayerController>(CharacterOwner->GetController());
	if (TestController == nullptr || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// NEVER ON A REPLAYED MOVE. OnMovementUpdated runs again for every saved move a correction
	// replays, and this harness has a clock and fires abilities — advancing it twice would schedule
	// phantom dashes that the server never saw, which is the harness manufacturing the very desync
	// it was written to look for. TickMomentumMeasure sidesteps this by refusing to run on an
	// autonomous proxy at all; this one has to run there, because a networked CLIENT predicting a
	// vertical dash is exactly the case worth measuring.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	const UWorld* TestWorld = GetWorld();
	if (TestWorld == nullptr)
	{
		return;
	}

	// One phase per row. Forward/Strafe are the W/S and A/D amounts; Pitch is where the mouse points,
	// positive up. 85 rather than 90 because APlayerController::LimitViewRotation clamps to the
	// camera manager's ViewPitchMax (89.9 by default) and a request at the clamp is a request whose
	// answer depends on the clamp.
	struct FDashPitchPhase
	{
		const TCHAR* Label;
		float Forward;
		float Strafe;
		float Pitch;
	};

	static const FDashPitchPhase Phases[] =
	{
		{ TEXT("A only,  level    "),  0.f, -1.f,  0.f },
		{ TEXT("D only,  level    "),  0.f,  1.f,  0.f },
		{ TEXT("D only,  look up45"),  0.f,  1.f, 45.f },
		{ TEXT("W only,  level    "),  1.f,  0.f,  0.f },
		{ TEXT("W only,  look up45"),  1.f,  0.f, 45.f },
		{ TEXT("W only,  look up85"),  1.f,  0.f, 85.f },
		{ TEXT("W+D,     look up45"),  1.f,  1.f, 45.f },
	};

	static const int32 NumPhases = UE_ARRAY_COUNT(Phases);

	// The dash is charged, and one charge refills over DashDuration + DashCooldown (3.68s shipped).
	// A phase shorter than that would silently measure "no dash happened" from the second row on.
	const float PhaseLength = FMath::Max(4.0f, GetDashRechargeWindow() + 0.5f);
	const float FireAt = 0.5f;      // input has to be held for a frame or two before Acceleration exists
	const float ReportAt = FireAt + 2.5f;

	if (DashPitchTestTime < 0.f)
	{
		// Wait for the match to settle and the pawn to be standing on something.
		if (TestWorld->GetTimeSeconds() < 4.f || !IsMovingOnGround())
		{
			return;
		}

		DashPitchTestTime = 0.f;
		DashPitchTestPhase = 0;
		DashPitchTestPhaseTime = 0.f;
		bDashPitchTestFired = 0;
		bDashPitchTestLogged = 0;

		// Face the middle of the field, exactly as the other two harnesses do: a fixed world axis
		// runs the pawn straight into an endzone wall from a spawn pad and measures the wall.
		FVector TowardCentre = -UpdatedComponent->GetComponentLocation();
		TowardCentre.Z = 0.f;
		DashPitchTestYaw = TowardCentre.Normalize() ? TowardCentre.Rotation().Yaw : 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("DASHPITCH ---- begin. netMode=%d speed=%.0f duration=%.3f reach=%.1fuu "
			     "exitZLimit=%.0f runYaw=%.1f"),
			static_cast<int32>(GetNetMode()), GetDashSpeed(), GetDashDuration(),
			GetDashSpeed() * GetDashDuration(), GetDashExitVerticalSpeedLimit(), DashPitchTestYaw);
	}

	if (DashPitchTestPhase >= NumPhases)
	{
		return;
	}

	DashPitchTestTime += DeltaSeconds;
	DashPitchTestPhaseTime += DeltaSeconds;

	const FDashPitchPhase& Phase = Phases[DashPitchTestPhase];

	// Aim. Held every frame: the pawn is under a PlayerController whose UpdateRotation would
	// otherwise leave the rotation wherever the last frame put it, and the whole measurement is
	// about pitch.
	//
	// SPEC v8 §1 — AND WHY THE AIM MOVES AFTER THE DASH HAS LAUNCHED.
	//
	// The v7 version of this harness held the aim rigidly still for the whole phase, and that made the
	// rubber-band it was supposed to catch INVISIBLE BY CONSTRUCTION. The legacy failure is that
	// PostUpdate(PostUpdate_Replay) stomps the move's SavedControlRotation with the aim at CORRECTION
	// time; if the aim has not moved since the dash was pressed, the stomped value equals the recorded
	// one and the broken path and the fixed path produce identical numbers. A real player is still
	// tracking a target while their dash is in the air, so the honest test is a moving aim.
	//
	// The sweep starts AFTER the launch frame (the dash direction is locked in BeginDash and must be
	// composed from the phase's stated pitch, or the DASHPITCH rows below stop measuring spec v7 §5)
	// and runs through the whole correction window, which is where the disagreement would land.
	// Deterministic in phase time, so both A/B arms sweep identically.
	const float AimSweepStart = FireAt + 0.08f;
	float AimPitch = Phase.Pitch;
	float AimYaw   = DashPitchTestYaw;
	if (DashPitchTestPhaseTime > AimSweepStart)
	{
		const float SweepTime = DashPitchTestPhaseTime - AimSweepStart;
		AimYaw   += 100.f * FMath::Sin(SweepTime * 6.0f);
		AimPitch  = FMath::Clamp(Phase.Pitch - 55.f * FMath::Sin(SweepTime * 4.0f), -80.f, 85.f);
	}
	TestController->SetControlRotation(FRotator(AimPitch, AimYaw, 0.f));

	// Hold the movement keys. Same basis ATraceCharacter::DoMove uses, so Acceleration arrives at
	// BeginDash shaped exactly as a human's would be.
	const FRotationMatrix YawBasis(FRotator(0.f, DashPitchTestYaw, 0.f));
	const FVector InputDirection =
		(YawBasis.GetUnitAxis(EAxis::X) * Phase.Forward + YawBasis.GetUnitAxis(EAxis::Y) * Phase.Strafe)
		.GetSafeNormal();
	if (!InputDirection.IsNearlyZero() && DashPitchTestPhaseTime < ReportAt)
	{
		CharacterOwner->AddMovementInput(InputDirection, 1.f);
	}

	if (DashPitchTestPhaseTime >= FireAt && bDashPitchTestFired == 0)
	{
		DashPitchTestStart = UpdatedComponent->GetComponentLocation();
		DashPitchTestPeakRise = 0.f;
		DashPitchTestLaunchVelocity = FVector::ZeroVector;
		StartDash();
		bDashPitchTestFired = 1;
	}

	if (bDashPitchTestFired != 0)
	{
		// First frame the dash is actually running is the launch this phase is measuring.
		if (IsDashing() && DashPitchTestLaunchVelocity.IsNearlyZero())
		{
			DashPitchTestLaunchVelocity = Velocity;
		}

		DashPitchTestPeakRise = FMath::Max<float>(
			DashPitchTestPeakRise,
			static_cast<float>(UpdatedComponent->GetComponentLocation().Z - DashPitchTestStart.Z));
	}

	if (DashPitchTestPhaseTime >= ReportAt && bDashPitchTestLogged == 0)
	{
		const FVector Here = UpdatedComponent->GetComponentLocation();
		const FVector Travel = Here - DashPitchTestStart;
		const FVector PlanarTravel(Travel.X, Travel.Y, 0.f);

		UE_LOG(LogTraceGame, Display,
			TEXT("DASHPITCH %s dir=(%6.3f,%6.3f,%6.3f) launchV=(%7.1f,%7.1f,%7.1f) |v|=%7.1f "
			     "peakRise=%7.1fuu planarTravel=%7.1fuu netZ=%7.1fuu grounded=%d"),
			Phase.Label, DashDirection.X, DashDirection.Y, DashDirection.Z,
			DashPitchTestLaunchVelocity.X, DashPitchTestLaunchVelocity.Y, DashPitchTestLaunchVelocity.Z,
			DashPitchTestLaunchVelocity.Size(), DashPitchTestPeakRise, PlanarTravel.Size(), Travel.Z,
			IsMovingOnGround() ? 1 : 0);

		bDashPitchTestLogged = 1;
	}

	if (DashPitchTestPhaseTime >= PhaseLength)
	{
		++DashPitchTestPhase;
		DashPitchTestPhaseTime = 0.f;
		bDashPitchTestFired = 0;
		bDashPitchTestLogged = 0;

		if (DashPitchTestPhase >= NumPhases)
		{
			// Another lap, unless the requested number of laps is done. See DashPitchTestCycle: the
			// corrections-per-dash figure this harness exists to produce is worthless at seven dashes.
			++DashPitchTestCycle;
			if (DashPitchTestCycle < FMath::Max(1, GTraceDashPitchTestCycles))
			{
				DashPitchTestPhase = 0;
				UE_LOG(LogTraceGame, Display,
					TEXT("DASHPITCH ---- cycle %d of %d complete (%d dashes so far)."),
					DashPitchTestCycle, FMath::Max(1, GTraceDashPitchTestCycles),
					DashPitchTestCycle * NumPhases);
				LogDashNetReport();
				return;
			}

			UE_LOG(LogTraceGame, Display, TEXT("DASHPITCH ---- end. %d phases x %d cycles."),
				NumPhases, DashPitchTestCycle);

			// SPEC v8 §1. The rubber-band number, printed by the harness that produced the dashes rather
			// than left to a console command nobody can type into an offscreen -game process. On a
			// listen host this reads 0.00 and means nothing (see TraceReportDashNet); on a JOINED CLIENT
			// at 40 ms it is the answer to "does the dash rubber-band".
			LogDashNetReport();
		}
	}
}

int32 UTraceCharacterMovementComponent::RunDashVectorTest() const
{
	const float DashSpeed = GetDashSpeed();
	const float DashDuration = GetDashDuration();
	const float ExitVerticalLimit = GetDashExitVerticalSpeedLimit();
	const float GravityMagnitude = FMath::Max(1.f, -GetGravityZ());
	const float InputMagnitude = FMath::Max(1.f, GetMaxAcceleration());

	UE_LOG(LogTraceGame, Display,
		TEXT("DASHVEC ---- spec v7 5, yaw pinned to 0 so +X is forward and +Y is right. "
		     "speed=%.0f duration=%.3f reach=%.1fuu exitZLimit=%.0f gravity=%.0f"),
		DashSpeed, DashDuration, DashSpeed * DashDuration, ExitVerticalLimit, GravityMagnitude);

	struct FDashVectorCase
	{
		const TCHAR* Label;
		float Forward;   // W = +1, S = -1
		float Strafe;    // D = +1, A = -1
		float Pitch;     // degrees, positive is looking UP
	};

	// Every case the task asks for, plus the degenerate ones that would silently break.
	static const FDashVectorCase Cases[] =
	{
		{ TEXT("A only,      level     "),  0.f, -1.f,   0.f },
		{ TEXT("D only,      level     "),  0.f,  1.f,   0.f },
		{ TEXT("A only,      look up 60"),  0.f, -1.f,  60.f },
		{ TEXT("D only,      look dn 60"),  0.f,  1.f, -60.f },
		{ TEXT("W only,      level     "),  1.f,  0.f,   0.f },
		{ TEXT("W only,      look up 90"),  1.f,  0.f,  90.f },
		{ TEXT("W only,      look up 45"),  1.f,  0.f,  45.f },
		{ TEXT("W only,      look dn 45"),  1.f,  0.f, -45.f },
		{ TEXT("S only,      look up 45"), -1.f,  0.f,  45.f },
		{ TEXT("W+D,         level     "),  1.f,  1.f,   0.f },
		{ TEXT("W+D,         look up 45"),  1.f,  1.f,  45.f },
		{ TEXT("W+A,         look up 45"),  1.f, -1.f,  45.f },
		{ TEXT("no input,    look up 45"),  0.f,  0.f,  45.f },
	};

	int32 Failures = 0;

	for (const FDashVectorCase& Case : Cases)
	{
		// Yaw 0: forward is +X, right is +Y. This is the same construction ATraceCharacter::DoMove
		// uses, so the acceleration fed in is shaped exactly like a real frame's.
		const FVector InAccel = FVector(Case.Forward, Case.Strafe, 0.f).GetSafeNormal() * InputMagnitude;
		const FRotator AimRotation(Case.Pitch, 0.f, 0.f);

		const FVector Direction = ComputeDashDirection(InAccel, AimRotation);
		const FVector DashVelocity = Direction * DashSpeed;
		const float Magnitude = DashVelocity.Size();

		// The normalisation claim, checked rather than described: every case must be one dash length.
		if (!FMath::IsNearlyEqual(Direction.Size(), 1.f, 1.e-4f))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("DASHVEC FAIL  %s direction is not unit length (%.6f)"),
				Case.Label, Direction.Size());
			++Failures;
		}

		// "If only A or D is held, dash horizontally only: parallel to the ground" — stated as an
		// assertion so a future edit that lets pitch leak into the strafe axis fails here loudly.
		if (FMath::IsNearlyZero(Case.Forward) && !FMath::IsNearlyZero(Case.Strafe)
			&& !FMath::IsNearlyZero(Direction.Z, 1.e-4f))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("DASHVEC FAIL  %s is strafe-only but has Z=%.6f"),
				Case.Label, Direction.Z);
			++Failures;
		}

		// Climb accounting, so "how high does a vertical dash get me" is a printed number rather than
		// an argument: the rails phase, then the clamped exit coast.
		const float RailsRise = FMath::Max(0.f, DashVelocity.Z) * DashDuration;
		const float ExitZ = FMath::Min(FMath::Max(0.f, DashVelocity.Z), ExitVerticalLimit);
		const float CoastRise = (ExitZ * ExitZ) / (2.f * GravityMagnitude);

		UE_LOG(LogTraceGame, Display,
			TEXT("DASHVEC %s dir=(%7.4f,%7.4f,%7.4f) v=(%8.1f,%8.1f,%8.1f) |v|=%7.1f "
			     "reach=%6.1fuu pitchOut=%6.1fdeg climb=%6.1f+%6.1f=%6.1fuu"),
			Case.Label, Direction.X, Direction.Y, Direction.Z,
			DashVelocity.X, DashVelocity.Y, DashVelocity.Z, Magnitude,
			Magnitude * DashDuration,
			FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Direction.Z, -1.f, 1.f))),
			RailsRise, CoastRise, RailsRise + CoastRise);
	}

	UE_LOG(LogTraceGame, Display, TEXT("DASHVEC ---- end. %d failure(s)."), Failures);
	return Failures;
}

static void TraceRunDashVectorTest()
{
	// Prefer a live pawn's component so the numbers reported are the ones the running match would
	// get; the CDO is a correct fallback because ComputeDashDirection touches no instance state
	// except in its standing-still branch, and every setting it reads is global.
	const UTraceCharacterMovementComponent* Movement = nullptr;
	if (GEngine != nullptr)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			const UWorld* ContextWorld = Context.World();
			if (ContextWorld == nullptr)
			{
				continue;
			}

			if (const APlayerController* LocalPC = ContextWorld->GetFirstPlayerController())
			{
				if (const ACharacter* LocalCharacter = Cast<ACharacter>(LocalPC->GetPawn()))
				{
					if (const UTraceCharacterMovementComponent* Found =
						Cast<UTraceCharacterMovementComponent>(LocalCharacter->GetCharacterMovement()))
					{
						Movement = Found;
						break;
					}
				}
			}
		}
	}

	if (Movement == nullptr)
	{
		Movement = GetDefault<UTraceCharacterMovementComponent>();
	}

	Movement->RunDashVectorTest();
}

void UTraceCharacterMovementComponent::LogDashNetReport() const
{
	const UWorld* ReportWorld = GetWorld();

	// netMode and role are printed because the whole point of this command is that the answer is only
	// meaningful for ROLE_AutonomousProxy on a client. A reader who forgets that can see it in the
	// line itself rather than having to remember which window they are in.
	UE_LOG(LogTraceGame, Display,
		TEXT("DASHNET REPORT %-16s netMode=%d role=%d | dashes=%d corrections(all)=%d "
		     "corrections(in dash)=%d => %.3f per dash | meanDashErr=%.2fuu "
		     "meanAllErr=%.2fuu worstErr=%.2fuu%s"),
		*GetNameSafe(CharacterOwner),
		(ReportWorld != nullptr) ? static_cast<int32>(ReportWorld->GetNetMode()) : -1,
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1,
		DashNetDashCount, CorrectionCount, DashNetCorrectionsInDash,
		DashNetCorrectionsInDash / static_cast<float>(FMath::Max(1, DashNetDashCount)),
		DashNetCorrectionErrorInDash / static_cast<float>(FMath::Max(1, DashNetCorrectionsInDash)),
		CorrectionErrorTotal / static_cast<float>(FMath::Max(1, CorrectionCount)),
		CorrectionErrorWorst,
		(CharacterOwner != nullptr && CharacterOwner->HasAuthority())
			? TEXT("  [AUTHORITY - this number is meaningless here]") : TEXT(""));
}

void UTraceCharacterMovementComponent::LogWallJumpReport() const
{
	const UWorld* ReportWorld = GetWorld();
	const float Denominator = static_cast<float>(FMath::Max(1, WallJumpCount));

	UE_LOG(LogTraceGame, Display,
		TEXT("WALLJUMP REPORT %-16s netMode=%d role=%d | jumps=%d entry=%6.0f -> launch=%6.0f uu/s "
		     "(%.1f%% carried, turned %.1fdeg, launchZ=%6.0f) maxConsecutive=%d/%d capRefusals=%d "
		     "corrections(in wall jump)=%d => %.3f per jump | hardCap=%.0f | "
		     "v9: legacyTuning=%d window=%.3fs | v12 §5: mantle removed, so mantleSteals is "
		     "identically 0 and the mantle lockout no longer exists%s"),
		*GetNameSafe(CharacterOwner),
		(ReportWorld != nullptr) ? static_cast<int32>(ReportWorld->GetNetMode()) : -1,
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1,
		WallJumpCount,
		WallJumpEntrySpeedSum / Denominator,
		WallJumpLaunchSpeedSum / Denominator,
		100.f * WallJumpLaunchSpeedSum / FMath::Max(1.f, WallJumpEntrySpeedSum),
		WallJumpTurnDegreesSum / Denominator,
		WallJumpLaunchZSum / Denominator,
		WallJumpMaxConsecutiveSeen, GetWallJumpMaxConsecutive(), WallJumpCapRefusals,
		WallJumpCorrectionsInWindow, WallJumpCorrectionsInWindow / Denominator,
		GetAirStrafeHardCapSpeed(),
		IsV9LegacyTuning() ? 1 : 0,
		GetWallJumpWindowSeconds(),
		(CharacterOwner != nullptr && CharacterOwner->HasAuthority())
			? TEXT("  [AUTHORITY - the correction column is meaningless here]") : TEXT(""));

	// SPEC v18 §1b. PRINTED HERE AS WELL AS IN THE DRIFT LEDGER, because this is the report a human
	// runs after a play session and the drift ledger has to be armed in advance.
	//
	// A BUFFERED LAUNCH IS A WALL JUMP THE PLAYER DID NOT PRESS ON THE FRAME IT HAPPENED — the press
	// was real, up to GetWallJumpInputBufferSeconds() earlier, and v10 §5 CAUSE 2 buffers it on
	// purpose. MEASURED on a client (spec v18 §1b) at 1016 uu/s of planar vector change on one move,
	// plus Velocity.Z assigned outright to JumpZVelocity x GetWallJumpVerticalMultiplier() — by a wide
	// margin the largest thing in this component that can move a hands-off airborne player. If
	// "velocity appears mid-jump with no input" is reported again, this counter is the first number to
	// look at.
	UE_LOG(LogTraceGame, Display,
		TEXT("WALLJUMP %-16s buffered launches (no press on the launch frame) = %d of %d total. "
		     "Buffer window %.3f s; contact window %.3f s."),
		*GetNameSafe(CharacterOwner), WallJumpBufferedLaunches, WallJumpCount,
		GetWallJumpInputBufferSeconds(), GetWallJumpWindowSeconds());
}

// =================================================================================================
// SPEC v10 §5 — THE STICK METER.
//
// "Wall jumping still feels like the player is sticking to the wall for a moment too long." That is a
// claim about MILLISECONDS, and v9 answered it by shortening a config value and then reporting that
// the config value was shorter — which is not a measurement of the symptom, and is why the same
// sentence came back a demo later. This measures the symptom itself:
//
//   from the frame the capsule first TOUCHES a wall, to the frame the pawn has moved
//   WallStickClearUU away from that face measured along the face's own normal.
//
// It is agnostic about the cause on purpose. Waiting for the window, a press that got eaten, the
// launch itself, and the launch being clawed back by held input are ALL inside the interval, so the
// number moves if and only if the player's experience does.
//
// Re-contacts do not re-anchor an open sample: a player scraping down a face is having one sticky
// experience, not thirty. A sample closes when the pawn clears the face, when it lands, or on a
// timeout — and a timeout is reported separately, because "never got off the wall at all" is the
// worst case and must not be averaged into a number that looks merely bad.
// =================================================================================================

/** Clear of the wall: half a capsule diameter out along the normal. CapsuleRadius is 34 uu. */
static constexpr float WallStickClearUU = 50.f;

/** A sample that has not cleared by here is a stick, not a slow escape. Reported in its own column. */
static constexpr float WallStickTimeoutSeconds = 1.5f;

void UTraceCharacterMovementComponent::BeginWallStickSample(const FVector& PlanarNormal)
{
	const UWorld* StickWorld = GetWorld();
	if (StickWorld == nullptr || UpdatedComponent == nullptr || CharacterOwner == nullptr)
	{
		return;
	}

	// RECORD PASS ONLY. A replayed move re-runs HandleImpact against the same static geometry, and a
	// re-anchored sample would restart the clock mid-bout on every correction — turning the client's
	// measurement into a measurement of its own correction rate. TickDashPitchTest's rule, verbatim.
	// ...and only on a pawn this process drives, to match TickWallStickSample()'s own gate. A sample
	// opened on a pawn nothing ticks would never be closed and would block every later one.
	if (CharacterOwner->bClientUpdating || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// An open sample keeps its anchor. See the block comment: re-contact is part of the stick, not a
	// new one.
	if (WallStickContactTime >= 0.f)
	{
		return;
	}

	WallStickContactTime = static_cast<float>(StickWorld->GetTimeSeconds());
	WallStickAnchor = UpdatedComponent->GetComponentLocation();
	WallStickNormal = PlanarNormal;
	WallStickLaunchTime = -1.f;
	WallStickPeakOutUU = 0.f;

	// Latched AT CONTACT, not at close: WallJumpsSinceGround is incremented by the launch this sample
	// is about, so reading it later would relabel every first jump as a chained one.
	bWallStickSampleChained = (WallJumpsSinceGround > 0) ? 1 : 0;
}

void UTraceCharacterMovementComponent::TickWallStickSample()
{
	if (WallStickContactTime < 0.f)
	{
		return;
	}

	const UWorld* StickWorld = GetWorld();
	if (StickWorld == nullptr || UpdatedComponent == nullptr)
	{
		CloseWallStickSample(false);
		return;
	}

	// Displacement from the anchor ALONG THE FACE'S NORMAL. Not straight-line distance: sliding 300 uu
	// along a wall you are still touching is not getting off the wall, and a distance test would score
	// the stickiest case in the game as a clean escape.
	const float OutUU = static_cast<float>(FVector::DotProduct(
		UpdatedComponent->GetComponentLocation() - WallStickAnchor, WallStickNormal));
	WallStickPeakOutUU = FMath::Max(WallStickPeakOutUU, OutUU);

	if (OutUU >= WallStickClearUU)
	{
		CloseWallStickSample(true);
		return;
	}

	// Landed without ever getting off the face. Counted as a non-clear rather than discarded — a wall
	// jump that dumped the player at the foot of the wall is exactly the experience being complained
	// about, and dropping it would bias the mean toward the samples that worked.
	if (IsGroundedForAbilities()
		|| static_cast<float>(StickWorld->GetTimeSeconds()) - WallStickContactTime >= WallStickTimeoutSeconds)
	{
		CloseWallStickSample(false);
	}
}

void UTraceCharacterMovementComponent::CloseWallStickSample(const bool bCleared)
{
	const UWorld* StickWorld = GetWorld();
	if (WallStickContactTime < 0.f || StickWorld == nullptr)
	{
		WallStickContactTime = -1.f;
		return;
	}

	const int32 Phase = FMath::Clamp(WallStickPhase, 0, 1);
	const float Now = static_cast<float>(StickWorld->GetTimeSeconds());
	const float ClearMs = 1000.f * (Now - WallStickContactTime);

	// A CONTACT THAT WAS NEVER WALL-JUMPED IS NOT A STICKY WALL JUMP. Brushing a face in passing, or
	// touching one with both ladder charges already spent, is a contact the player asked nothing of.
	// Counted on its own line and kept out of the mean — see WallStickNoLaunch in the header.
	if (WallStickLaunchTime < 0.f)
	{
		++WallStickNoLaunch[Phase];
		if (bWallStickSampleChained != 0)
		{
			++WallStickChainedNoLaunch;
		}
		WallStickContactTime = -1.f;
		WallStickPeakOutUU = 0.f;
		return;
	}

	++WallStickSamples[Phase];
	WallStickPeakOutSum[Phase] += WallStickPeakOutUU;

	if (bWallStickSampleChained != 0)
	{
		++WallStickChainedSamples;
		WallStickChainedPeakOutSum += WallStickPeakOutUU;
		const float ChainedMs = bCleared ? ClearMs : (1000.f * WallStickTimeoutSeconds);
		WallStickChainedClearMsSum += ChainedMs;
		WallStickChainedClearMsWorst = FMath::Max(WallStickChainedClearMsWorst, ChainedMs);
		if (!bCleared)
		{
			++WallStickChainedNeverCleared;
		}
	}

	if (bCleared)
	{
		WallStickClearMsSum[Phase] += ClearMs;
		WallStickClearMsWorst[Phase] = FMath::Max(WallStickClearMsWorst[Phase], ClearMs);
	}
	else
	{
		// Never cleared. Charged at the full timeout so it cannot flatter the mean — the alternative,
		// excluding it, would let a fix that turned clean escapes into permanent sticks report an
		// IMPROVEMENT, which is the one result this meter must be incapable of producing.
		++WallStickNeverCleared[Phase];
		WallStickClearMsSum[Phase] += 1000.f * WallStickTimeoutSeconds;
		WallStickClearMsWorst[Phase] = FMath::Max(WallStickClearMsWorst[Phase], 1000.f * WallStickTimeoutSeconds);
	}

	if (WallStickLaunchTime >= 0.f)
	{
		WallStickPressMsSum[Phase] += 1000.f * (WallStickLaunchTime - WallStickContactTime);
	}

	WallStickContactTime = -1.f;
	WallStickLaunchTime = -1.f;
	WallStickPeakOutUU = 0.f;
}

void UTraceCharacterMovementComponent::LogWallStickReport() const
{
	static const TCHAR* PhaseNames[2] = { TEXT("HEAD-ON "), TEXT("GLANCING") };

	UE_LOG(LogTraceGame, Display,
		TEXT("WALLSTICK REPORT %-16s netMode=%d role=%d | arm=%s clearAt=%.0fuu | lockout=%.2fs "
		     "buffer=%.2fs retention=%.4f outward=%.0f"),
		*GetNameSafe(CharacterOwner),
		(GetWorld() != nullptr) ? static_cast<int32>(GetWorld()->GetNetMode()) : -1,
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1,
		IsV10LegacyWallJump() ? TEXT("RED (v9 behaviour)") : TEXT("GREEN (v10 fix)"),
		WallStickClearUU,
		GetWallJumpControlLockoutSeconds(), GetWallJumpInputBufferSeconds(),
		GetWallJumpSpeedRetention(), GetWallJumpOutwardImpulse());

	for (int32 Phase = 0; Phase < 2; ++Phase)
	{
		const float Denominator = static_cast<float>(FMath::Max(1, WallStickSamples[Phase]));
		UE_LOG(LogTraceGame, Display,
			TEXT("WALLSTICK   %s presses early=%2d onTime=%2d -> wallJumps=%3d (contacts with NO launch"
			     "=%d) | STICK=%7.1f ms mean (worst %7.1f) | contact->launch=%6.1f ms | peakOut=%6.1f uu "
			     "| neverCleared=%d/%d"),
			PhaseNames[Phase],
			WallStickEarlyPresses[Phase], WallStickOnTimePresses[Phase],
			WallStickSamples[Phase], WallStickNoLaunch[Phase],
			WallStickClearMsSum[Phase] / Denominator,
			WallStickClearMsWorst[Phase],
			WallStickPressMsSum[Phase] / Denominator,
			WallStickPeakOutSum[Phase] / Denominator,
			WallStickNeverCleared[Phase], WallStickSamples[Phase]);
	}

	// THE HEADLINE. A cross-cut of the two lines above, not a third phase — see the header. This is the
	// jump taken from the air, where the outward impulse is the whole launch and the player's held
	// input is strong enough to beat it, and it is the number the fix is supposed to move.
	const float ChainedDenominator = static_cast<float>(FMath::Max(1, WallStickChainedSamples));
	UE_LOG(LogTraceGame, Display,
		TEXT("WALLSTICK   CHAINED  (2nd+ jump of a chain, arrived under air control) wallJumps=%3d "
		     "(contacts with NO launch=%d) | STICK=%7.1f ms mean (worst %7.1f) | peakOut=%6.1f uu "
		     "| neverCleared=%d/%d"),
		WallStickChainedSamples, WallStickChainedNoLaunch,
		WallStickChainedClearMsSum / ChainedDenominator,
		WallStickChainedClearMsWorst,
		WallStickChainedPeakOutSum / ChainedDenominator,
		WallStickChainedNeverCleared, WallStickChainedSamples);
}

/**
 * SPEC v8 §7 — the wall jump driven from code, so it can be measured offscreen and ON A CLIENT.
 *
 * Runs the pawn at the nearest perimeter wall and presses jump through ACharacter::Jump() every frame
 * IsWallJumpAvailable() is true. Jump() is the human entry point, so the press rides CheckJumpInput ->
 * DoJump -> TryWallJump and the saved move exactly as a player's would; nothing here teleports, rotates
 * or writes Velocity, so it cannot manufacture the desync it is measuring.
 */
void UTraceCharacterMovementComponent::TickWallJumpTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceWallJumpTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	APlayerController* TestController = Cast<APlayerController>(CharacterOwner->GetController());
	if (TestController == nullptr || !CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// TickDashPitchTest's reason, verbatim: a replayed move must not advance a clock that fires input.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	const UWorld* TestWorld = GetWorld();
	if (TestWorld == nullptr)
	{
		return;
	}

	if (WallJumpTestTime < 0.f)
	{
		if (TestWorld->GetTimeSeconds() < 6.f || !IsMovingOnGround())
		{
			return;
		}

		// Straight at the nearer of the two long side walls. A perimeter wall is 4800uu from the
		// centreline and its face is exactly vertical, which is the geometry the mechanic is for.
		const FVector Here = UpdatedComponent->GetComponentLocation();
		WallJumpTestYaw = (Here.Y >= 0.0) ? 90.f : -90.f;
		WallJumpTestTime = 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("WALLJUMP ---- begin. netMode=%d role=%d at %s runYaw=%.0f window=%.2fs retain=%.2f "
			     "outward=%.0f zMul=%.2f cap=%d"),
			static_cast<int32>(GetNetMode()),
			static_cast<int32>(CharacterOwner->GetLocalRole()), *Here.ToCompactString(), WallJumpTestYaw,
			GetWallJumpWindowSeconds(), GetWallJumpSpeedRetention(), GetWallJumpOutwardImpulse(),
			GetWallJumpVerticalMultiplier(), GetWallJumpMaxConsecutive());
	}

	WallJumpTestTime += DeltaSeconds;

	// --- SPEC v10 §5: TWO APPROACH PHASES, BECAUSE THEY ARE TWO DIFFERENT MECHANICS ---------------
	//
	// PHASE 0, HEAD-ON (0-22 s). What the v8 harness always did. The FIRST jump of each chain arrives
	// off a full-speed ground run and reflects a large outward launch, so it escapes on its own — and
	// it is the case every previous measurement looked at. The SECOND, taken from the air, does not:
	// AirMaxWishSpeed caps the return at 160 uu/s, so there is nothing left to reflect.
	//
	// PHASE 1, GLANCING (22-44 s). Running along a face at ~20° to it — which is what a player
	// actually does in a corridor. There is almost nothing pointing into the wall to reflect even on
	// the first jump, so WallJumpOutwardImpulse (360 uu/s, flat) is essentially the whole outward
	// launch, and it is the case the player's held input can beat. THIS IS THE COLUMN THE COMPLAINT
	// LIVES IN, and the old harness never ran it — a large part of why v9 "verified" a fix the
	// players did not feel.
	//
	// The glancing run alternates its along-wall direction every 5 s so the pawn stays inside the
	// arena instead of running 26000 uu down it and measuring the end wall instead.
	const bool bGlancing = (WallJumpTestTime >= 30.f);
	const float WallSign = (WallJumpTestYaw >= 0.f) ? 1.f : -1.f;
	const float AlongSign = ((static_cast<int32>(WallJumpTestTime) / 5) % 2 == 0) ? 1.f : -1.f;
	const float RunYaw = bGlancing
		? (90.f * WallSign - 70.f * WallSign * AlongSign)
		: WallJumpTestYaw;

	WallStickPhase = bGlancing ? 1 : 0;

	if (bGlancing && WallJumpTestTime - DeltaSeconds < 30.f)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("WALLJUMP ---- phase 1: GLANCING approach (~20deg to the face), the case the flat "
			     "outward impulse has to carry on its own."));
		LogWallStickReport();
	}

	// --- BACK OFF AND TAKE ANOTHER RUN AT IT ------------------------------------------------------
	//
	// See WallJumpTestRunUpUntil in the header: without this the harness pinned the pawn against the
	// face at zero speed and took THREE wall jumps in 22 seconds before seizing up entirely. A mean
	// over three samples is not a measurement, and it is how a harness reports a healthy mechanic
	// while measuring almost nothing.
	//
	// The 1.5 s guard on re-arming matters: coming out of a run-up the pawn is still moving AWAY, so
	// it passes back down through 300 uu/s while it turns around, and without the guard that would
	// immediately re-trigger a second run-up and the pawn would oscillate on the spot forever.
	const bool bRunningUp = (WallJumpTestTime < WallJumpTestRunUpUntil);
	if (!bRunningUp
		&& WallJumpTestTime > 1.f
		&& WallJumpTestTime > WallJumpTestRunUpUntil + 1.5f
		&& IsMovingOnGround()
		&& GetPlanarSpeed() < 300.f)
	{
		WallJumpTestRunUpUntil = WallJumpTestTime + 1.2f;
	}

	// THE AIM STAYS ON THE WALL EVEN WHILE BACKING OFF, and only the movement input reverses — that is
	// a player strafing back for another go, and it keeps the approach direction (which is what the
	// reflection is computed from) honest for the frame the pawn actually arrives.
	TestController->SetControlRotation(FRotator(0.f, RunYaw, 0.f));
	const FRotationMatrix DriveBasis(FRotator(0.f, bRunningUp ? (RunYaw + 180.f) : RunYaw, 0.f));
	CharacterOwner->AddMovementInput(DriveBasis.GetUnitAxis(EAxis::X), 1.f);

	// --- THE PRESS. See WallJumpTestApproach in the header for why the old rule was blind. --------
	//
	// Approaches alternate: EVEN presses EARLY (a human aiming at the wall), ODD presses on
	// IsWallJumpAvailable() (the v8 rule). An approach begins the moment the pawn leaves the ground.
	const bool bAirborneNow = !IsMovingOnGround();
	if (bAirborneNow && bWallJumpTestWasAirborne == 0)
	{
		++WallJumpTestApproach;
		bWallJumpTestPressLatched = 0;
	}
	if (!bAirborneNow)
	{
		bWallJumpTestPressLatched = 0;
	}
	bWallJumpTestWasAirborne = bAirborneNow ? 1 : 0;

	// CHAIN THE JUMPS — see WallJumpTestLastChainCount in the header. A launch, and only a launch,
	// re-arms the latch: the second jump of a chain is taken from the air, where AirMaxWishSpeed caps
	// the return at 160 uu/s and the flat outward impulse is the entire launch, and that is the case
	// the complaint is actually about. Without this the harness reported the healthy first jump.
	if (WallJumpsSinceGround > WallJumpTestLastChainCount)
	{
		bWallJumpTestPressLatched = 0;
	}
	WallJumpTestLastChainCount = bAirborneNow ? WallJumpsSinceGround : 0;

	bool bPressedThisFrame = false;
	if (IsFalling() && bWallJumpTestPressLatched == 0)
	{
		if ((WallJumpTestApproach % 2) == 0)
		{
			// "PRESS JUMP RIGHT AS THEY HIT A WALL" — the spec's own words, and the press this
			// project has never actually tested. A lead in TIME rather than in distance, so it is the
			// same two-frame anticipation at any speed and at any approach angle: the trace runs along
			// the direction of travel, so its length IS the time to contact.
			const float LeadReach = FMath::Max(30.f, GetPlanarSpeed() * 0.035f);
			const FVector LeadFrom = UpdatedComponent->GetComponentLocation();
			const FVector LeadTo = LeadFrom + FRotationMatrix(FRotator(0.f, RunYaw, 0.f)).GetUnitAxis(EAxis::X) * LeadReach;

			// THE PAWN'S OWN CHANNEL, not ECC_Visibility. Measured: on ECC_Visibility this trace hit
			// NOTHING — "presses early= 0" for a whole run — because the arena's walls do not block
			// the visibility channel. A wall is, by definition, a thing that blocks THIS capsule, so
			// ask the question in the capsule's own terms and the answer cannot be a collision-setup
			// detail. (This is a harness bug the report would have hidden as "the fix did nothing".)
			FHitResult LeadHit;
			FCollisionQueryParams LeadParams(SCENE_QUERY_STAT(TraceWallJumpLead), false, CharacterOwner);
			if (TestWorld->LineTraceSingleByChannel(LeadHit, LeadFrom, LeadTo,
				UpdatedComponent->GetCollisionObjectType(), LeadParams))
			{
				CharacterOwner->Jump();
				bPressedThisFrame = true;
				++WallStickEarlyPresses[FMath::Clamp(WallStickPhase, 0, 1)];
			}
		}
		else if (IsWallJumpAvailable())
		{
			CharacterOwner->Jump();
			bPressedThisFrame = true;
			++WallStickOnTimePresses[FMath::Clamp(WallStickPhase, 0, 1)];
		}
	}

	if (bPressedThisFrame)
	{
		// ONE PRESS PER APPROACH. A human presses once and then wonders why nothing happened; a
		// harness that mashed would paper over the eaten-press bug it is here to measure.
		bWallJumpTestPressLatched = 1;
	}
	else if (!bRunningUp && IsMovingOnGround() && WallJumpTestTime > 1.f && GetPlanarSpeed() > 300.f)
	{
		// Grounded with a run-up: the mechanic is airborne-only, so get airborne. An ordinary jump.
		// Refused mid-run-up, or the pawn would launch itself backwards away from the wall.
		CharacterOwner->Jump();
	}

	if (WallJumpTestTime >= 60.f && bWallJumpTestReported == 0)
	{
		bWallJumpTestReported = 1;
		UE_LOG(LogTraceGame, Display, TEXT("WALLJUMP ---- end."));
		LogWallJumpReport();
		LogWallStickReport();
	}
}

/**
 * SPEC v8 §5, THE MEASUREMENT. See the header for why it has a server half and a client half.
 */
void UTraceCharacterMovementComponent::TickCarrierChargeTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceCarrierChargeTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// TickDashPitchTest's reason, verbatim: a replayed move must not advance a clock that fires input.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	UWorld* TestWorld = GetWorld();
	if (TestWorld == nullptr)
	{
		return;
	}

	ATraceCharacter* TraceOwner = Cast<ATraceCharacter>(CharacterOwner);
	if (TraceOwner == nullptr)
	{
		return;
	}

	// --- THE SERVER HALF: hand the Core to the JOINED CLIENT's pawn --------------------------------
	//
	// Authority, over a pawn this process does NOT control, that is driven by a PlayerController — on a
	// listen server that is precisely the pawn of the player who joined, which is the only pawn spec
	// v8 §0 accepts a measurement from. Bots are excluded by the PlayerController test.
	if (CharacterOwner->HasAuthority() && !CharacterOwner->IsLocallyControlled())
	{
		if (bCarrierTestCoreGiven != 0 || Cast<APlayerController>(CharacterOwner->GetController()) == nullptr)
		{
			return;
		}

		// After the client's control phase has had time to run and to spend its single charge, so the
		// "before" number is measured on a pawn that genuinely was not carrying.
		if (TestWorld->GetTimeSeconds() < 16.f)
		{
			return;
		}

		for (TActorIterator<ATraceCore> It(TestWorld); It; ++It)
		{
			ATraceCore* Core = *It;
			if (Core == nullptr || Core->IsHeld())
			{
				continue;
			}

			// The real funnel, not a poke at bIsCarrier: the Core attaches, the PlayerState updates and
			// bIsCarrier replicates, which is the whole thing being tested on the far end.
			Core->TryPickup(TraceOwner);
			bCarrierTestCoreGiven = 1;

			UE_LOG(LogTraceGame, Display,
				TEXT("CARRIERTEST [server] gave the Core to the joined client's pawn %s at t=%.1f (carrier=%d)"),
				*GetNameSafe(CharacterOwner), TestWorld->GetTimeSeconds(), TraceOwner->IsCarrier() ? 1 : 0);
			break;
		}

		return;
	}

	// --- THE CLIENT HALF --------------------------------------------------------------------------
	if (!CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	// Count launches from the dash's own clock. A press that was refused for want of a charge produces
	// no edge here, which is exactly the symptom being measured.
	const bool bDashingNow = (DashTimeRemaining > 0.f);
	if (bDashingNow && bCarrierTestWasDashing == 0)
	{
		++CarrierTestLaunches;
		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST [client] launch %d in phase %d (carrier=%d charges now %d/%d)"),
			CarrierTestLaunches, CarrierTestPhase, TraceOwner->IsCarrier() ? 1 : 0,
			DashCharges, GetMaxDashCharges());
	}
	bCarrierTestWasDashing = bDashingNow ? 1 : 0;

	if (CarrierTestTime < 0.f)
	{
		if (TestWorld->GetTimeSeconds() < 8.f || !IsMovingOnGround())
		{
			return;
		}

		CarrierTestTime = 0.f;
		CarrierTestPhaseTime = 0.f;
		CarrierTestPhase = 0;
		CarrierTestPresses = 0;
		CarrierTestLaunches = 0;
		CarrierTestChargesAtStart = DashCharges;
		CarrierTestMaxAtStart = GetMaxDashCharges();

		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST ---- begin phase 0 (CONTROL, not carrying). netMode=%d role=%d carrier=%d "
			     "charges=%d/%d cfg base=%d carrierExtra=%d"),
			static_cast<int32>(GetNetMode()), static_cast<int32>(CharacterOwner->GetLocalRole()),
			TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges(),
			UTraceSettings::Get().BaseDashCharges, UTraceSettings::Get().CarrierExtraDashCharges);
	}

	CarrierTestTime += DeltaSeconds;
	CarrierTestPhaseTime += DeltaSeconds;

	if (CarrierTestPhase > 1)
	{
		return;
	}

	// Two presses, far enough apart that the first dash has ended (so the second is refused only by an
	// empty pool, never by "a dash is already running") and close enough that no charge can refill.
	const float FirstPressAt = 0.4f;
	const float SecondPressAt = FirstPressAt + FMath::Max(0.35f, GetDashDuration() + 0.15f);
	const float ReportAt = SecondPressAt + 1.2f;

	if (CarrierTestPhaseTime >= FirstPressAt && CarrierTestPresses == 0)
	{
		++CarrierTestPresses;
		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST [client] press 1 phase %d: carrier=%d charges=%d/%d"),
			CarrierTestPhase, TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges());
		StartDash();
	}
	else if (CarrierTestPhaseTime >= SecondPressAt && CarrierTestPresses == 1)
	{
		++CarrierTestPresses;
		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST [client] press 2 phase %d: carrier=%d charges=%d/%d"),
			CarrierTestPhase, TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges());
		StartDash();
	}

	if (CarrierTestPhaseTime >= ReportAt && CarrierTestPresses >= 2)
	{
		const int32 Expected = (CarrierTestPhase == 0) ? 1 : 2;
		UE_LOG(LogTraceGame, Display,
			TEXT("CARRIERTEST ==== phase %d (%s) presses=%d launches=%d expected=%d %s | carrier=%d "
			     "chargesAtStart=%d/%d chargesNow=%d/%d lastMax=%d"),
			CarrierTestPhase, (CarrierTestPhase == 0) ? TEXT("CONTROL, no Core") : TEXT("CARRYING"),
			CarrierTestPresses, CarrierTestLaunches, Expected,
			(CarrierTestLaunches == Expected) ? TEXT("PASS") : TEXT("FAIL"),
			TraceOwner->IsCarrier() ? 1 : 0, CarrierTestChargesAtStart, CarrierTestMaxAtStart,
			DashCharges, GetMaxDashCharges(), LastMaxDashCharges);

		++CarrierTestPhase;
		CarrierTestPhaseTime = 0.f;
		CarrierTestPresses = 0;
		CarrierTestLaunches = 0;

		if (CarrierTestPhase == 1)
		{
			// Phase 1 starts only once the pawn IS the carrier AND the pool has refilled to the carrier's
			// maximum. Both conditions are the claim: the Core arrived, and the extra charge came with it.
			CarrierTestPhaseTime = -1000.f;
		}
	}

	if (CarrierTestPhase == 1 && CarrierTestPhaseTime < -1.f)
	{
		if (TraceOwner->IsCarrier() && DashCharges >= GetMaxDashCharges() && GetMaxDashCharges() >= 2)
		{
			CarrierTestPhaseTime = 0.f;
			CarrierTestChargesAtStart = DashCharges;
			CarrierTestMaxAtStart = GetMaxDashCharges();
			UE_LOG(LogTraceGame, Display,
				TEXT("CARRIERTEST ---- begin phase 1 (CARRYING). carrier=%d charges=%d/%d lastMax=%d t=%.1f"),
				TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges(), LastMaxDashCharges,
				TestWorld->GetTimeSeconds());
		}
		else if (TestWorld->GetTimeSeconds() > 60.f)
		{
			CarrierTestPhase = 2;
			UE_LOG(LogTraceGame, Warning,
				TEXT("CARRIERTEST ==== phase 1 NEVER STARTED by t=60: carrier=%d charges=%d/%d lastMax=%d. "
				     "Either the Core never reached this client or the pool never grew with it."),
				TraceOwner->IsCarrier() ? 1 : 0, DashCharges, GetMaxDashCharges(), LastMaxDashCharges);
		}
	}
}

// =================================================================================================
// SPEC v9 §2 / §0 — THE SINGLE-DASH REPRODUCTION (-TraceSingleDashTest)
// =================================================================================================
//
// Spec v9 §0 exists because the last pass's harness pressed dash TWICE FROM A FULL POOL, saw two
// launches, and called the bug fixed. That test could not fail: both launches genuinely happen. It
// never touched the state the user described.
//
// This one reproduces the user's sentences literally, on a CLIENT, WHILE CARRYING:
//
//   "When dash is used, both charges are consumed."   -> dash ONCE from a full pool and read the
//                                                        pool AND THE HUD.
//   "When the first refills, they both do."           -> drain to zero and watch the 0 -> 1 refill,
//                                                        reading the HUD on the frame it lands.
//   "despite the hud showing two"                     -> every sample compares the true pool with
//                                                        the number of pips ATraceHUD would DRAW.
//
// THE PIP COUNT IS THE POINT. It is recomputed here by the identical rule ATraceHUD::DrawChargePips
// uses (pip i is full when i < Charges, and pip[Charges] is drawn at RechargeFraction), from the
// real ATracePlayerController::GetDashHudState — not from a local reimplementation of the maths.
// What the player sees is a count of full pips, so that is what is asserted against.
//
// A test that only reads DashCharges would have gone green on the broken build, because the pool was
// never the thing that was wrong.

/** One line of the ledger: what the pawn holds, and what the HUD would draw for it. */
struct FTraceSingleDashSample
{
	int32 Charges = 0;
	int32 MaxCharges = 1;
	bool  bHudValid = false;
	int32 HudCharges = 0;
	int32 HudMaxCharges = 1;
	float HudRechargeFraction = 0.f;
	float HudRemaining = 0.f;

	/** Pips ATraceHUD::DrawChargePips would render SOLID. This is the number the player reads. */
	int32 HudFullPips = 0;
};

static void TraceSingleDashCheck(const TCHAR* Name, const bool bPass, int32& InOutFailures, const FString& Detail)
{
	if (!bPass)
	{
		++InOutFailures;
	}

	UE_LOG(LogTraceGame, Display, TEXT("SINGLEDASH  [%s] %-46s %s"),
		bPass ? TEXT("PASS") : TEXT("FAIL"), Name, *Detail);
}

FTraceSingleDashSample UTraceCharacterMovementComponent::SampleSingleDashTest() const
{
	FTraceSingleDashSample Sample;
	Sample.Charges = DashCharges;
	Sample.MaxCharges = FMath::Max(1, GetMaxDashCharges());

	// THE REAL HUD PATH, deliberately. Reimplementing GetDashHudState here would test this harness's
	// idea of the HUD rather than the HUD, which is the exact failure spec v9 §0 is about.
	const ATracePlayerController* PC = (CharacterOwner != nullptr)
		? Cast<ATracePlayerController>(CharacterOwner->GetController())
		: nullptr;

	FTraceDashHudState HudState;
	if (PC != nullptr && PC->GetDashHudState(HudState))
	{
		Sample.bHudValid = true;
		Sample.HudCharges = HudState.Charges;
		Sample.HudMaxCharges = FMath::Max(1, HudState.MaxCharges);
		Sample.HudRechargeFraction = HudState.RechargeFraction;
		Sample.HudRemaining = HudState.Remaining;

		// ATraceHUD::DrawChargePips, verbatim: banked charges are full, pip[Charges] shows the
		// recharge progress, everything past it is empty. A pip at fraction 1.0 is indistinguishable
		// from a banked one on screen, so that is what "full" means here.
		for (int32 Index = 0; Index < Sample.HudMaxCharges; ++Index)
		{
			float Fraction = 0.f;
			if (Index < HudState.Charges)
			{
				Fraction = 1.f;
			}
			else if (Index == HudState.Charges)
			{
				Fraction = FMath::Clamp(HudState.RechargeFraction, 0.f, 1.f);
			}

			if (Fraction >= 0.999f)
			{
				++Sample.HudFullPips;
			}
		}
	}

	return Sample;
}

void UTraceCharacterMovementComponent::TickSingleDashTest(float DeltaSeconds)
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("TraceSingleDashTest"));
	if (!bEnabled || CharacterOwner == nullptr || UpdatedComponent == nullptr)
	{
		return;
	}

	// TickDashPitchTest's reason: a replayed move must not advance a clock that fires input.
	if (CharacterOwner->bClientUpdating)
	{
		return;
	}

	UWorld* TestWorld = GetWorld();
	ATraceCharacter* TraceOwner = Cast<ATraceCharacter>(CharacterOwner);
	if (TestWorld == nullptr || TraceOwner == nullptr)
	{
		return;
	}

	// --- THE SERVER HALF: hand the Core to the JOINED CLIENT's pawn -------------------------------
	//
	// Trace.DebugTakeCore only ever targets the LOCAL pawn, which on a listen host is the host — the
	// one machine spec v9 §0 says does not count. So the authority pushes the Core through
	// ATraceCore::TryPickup(), the same funnel the pickup sphere uses, and bIsCarrier reaches the
	// client by replication exactly as it would in a real match.
	if (CharacterOwner->HasAuthority() && !CharacterOwner->IsLocallyControlled())
	{
		if (bSingleDashCoreGiven != 0 || Cast<APlayerController>(CharacterOwner->GetController()) == nullptr)
		{
			return;
		}

		if (TestWorld->GetTimeSeconds() < 10.f)
		{
			return;
		}

		// WAITING FOR A LOOSE CORE IS NOT GOOD ENOUGH, and this cost two whole runs. The original loop
		// skipped every Core that IsHeld() and simply retried next frame, so it depended on the Core
		// happening to be on the floor while the ten bots in the match are fighting over it. Measured:
		// in one arm the wait ended at t=53.7 (the harness arms at t=75, so it barely made it) and in
		// the next it never ended at all — the client reported "NEVER ARMED ... carrier=0" and the run
		// produced no measurement. A harness whose ability to take a reading depends on bot behaviour
		// is not a harness.
		//
		// So: prefer a loose Core (TryPickup is the ordinary player funnel and is what a real pickup
		// does), but after a short grace TAKE it from whoever is holding it. GrantTo() is documented in
		// TraceCore.h as "the single funnel: every other path ends up here" — a kill, a pass and a
		// kickoff all reach it — so robbing a bot exercises exactly the code a real turnover does, and
		// ETraceCoreGrantReason::Debug is the reason the enum reserves for a harness grant.
		//
		// This changes NOTHING about what is measured. It only decides which pawn is carrying when the
		// measurement starts; every check runs afterwards, on the client, through the ordinary
		// replicated carrier bit.
		const bool bMayRob = TestWorld->GetTimeSeconds() >= 18.f;

		ATraceCore* Robbable = nullptr;
		for (TActorIterator<ATraceCore> It(TestWorld); It; ++It)
		{
			ATraceCore* Core = *It;
			if (Core == nullptr)
			{
				continue;
			}

			if (!Core->IsHeld())
			{
				Core->TryPickup(TraceOwner);
				bSingleDashCoreGiven = 1;
				UE_LOG(LogTraceGame, Display,
					TEXT("SINGLEDASH [server] gave the LOOSE Core to the joined client's pawn %s at t=%.1f (carrier=%d)"),
					*GetNameSafe(CharacterOwner), TestWorld->GetTimeSeconds(), TraceOwner->IsCarrier() ? 1 : 0);
				return;
			}

			if (Robbable == nullptr)
			{
				Robbable = Core;
			}
		}

		if (bMayRob && Robbable != nullptr)
		{
			Robbable->GrantTo(TraceOwner, ETraceCoreGrantReason::Debug);
			bSingleDashCoreGiven = 1;
			UE_LOG(LogTraceGame, Display,
				TEXT("SINGLEDASH [server] TOOK the Core from its holder for the joined client's pawn %s "
				     "at t=%.1f (carrier=%d)"),
				*GetNameSafe(CharacterOwner), TestWorld->GetTimeSeconds(), TraceOwner->IsCarrier() ? 1 : 0);
		}

		return;
	}

	// A HUMAN'S PAWN, NOT A BOT'S. Bot pawns are locally controlled ON THE SERVER too, and they have
	// no ATracePlayerController — so GetDashHudState has nothing to answer with and every bot would
	// report "the HUD draws 0 pips" forever. (Measured: eight bots did exactly that, and one of them
	// also spent a second charge on its own AI dash mid-phase and failed the consumption check with
	// it.) The claim under test is about what a PLAYER sees, so only a player's pawn qualifies.
	if (!CharacterOwner->IsLocallyControlled() || bSingleDashReported != 0
		|| Cast<ATracePlayerController>(CharacterOwner->GetController()) == nullptr)
	{
		return;
	}

	const FTraceSingleDashSample Sample = SampleSingleDashTest();
	const float Window = GetDashRechargeWindow();

	// --- PHASE -1: ARM. Wait for the Core to land and the pool to reach the carrier's maximum. -----
	if (SingleDashPhase < 0)
	{
		if (!TraceOwner->IsCarrier() || Sample.MaxCharges < 2 || Sample.Charges < Sample.MaxCharges
			|| !IsMovingOnGround() || DashTimeRemaining > 0.f)
		{
			if (TestWorld->GetTimeSeconds() > 75.f)
			{
				bSingleDashReported = 1;
				UE_LOG(LogTraceGame, Warning,
					TEXT("SINGLEDASH ==== NEVER ARMED by t=75: carrier=%d charges=%d/%d grounded=%d. "
					     "The Core never reached this client, or the pool never grew with it."),
					TraceOwner->IsCarrier() ? 1 : 0, Sample.Charges, Sample.MaxCharges,
					IsMovingOnGround() ? 1 : 0);
			}
			return;
		}

		SingleDashPhase = 0;
		SingleDashPhaseTime = 0.f;
		SingleDashPresses = 0;
		SingleDashPrevCharges = Sample.Charges;
		SingleDashPrevHudPips = Sample.HudFullPips;
		SingleDashMaxHudDivergence = 0;
		SingleDashDivergentSeconds = 0.f;
		SingleDashMaxTrueGain = 0;
		SingleDashMaxHudGain = 0;
		SingleDashHudPipsAtFirstRefill = -1;
		SingleDashNextSampleTime = 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("SINGLEDASH ==== ARMED at t=%.1f. netMode=%d role=%d carrier=%d | true=%d/%d  "
			     "hud=%d/%d frac=%.3f rem=%.2f pips=%d | rechargeWindow=%.2f"),
			TestWorld->GetTimeSeconds(), static_cast<int32>(GetNetMode()),
			static_cast<int32>(CharacterOwner->GetLocalRole()), TraceOwner->IsCarrier() ? 1 : 0,
			Sample.Charges, Sample.MaxCharges, Sample.HudCharges, Sample.HudMaxCharges,
			Sample.HudRechargeFraction, Sample.HudRemaining, Sample.HudFullPips, Window);
		return;
	}

	SingleDashPhaseTime += DeltaSeconds;

	// --- THE LEDGER. Every transition of either number, plus a heartbeat, plus the running worsts. --
	const int32 TrueGain = Sample.Charges - SingleDashPrevCharges;
	const int32 HudGain = Sample.HudFullPips - SingleDashPrevHudPips;
	const bool bChanged = (TrueGain != 0) || (HudGain != 0);

	SingleDashMaxHudDivergence = FMath::Max(SingleDashMaxHudDivergence,
		FMath::Abs(Sample.HudFullPips - Sample.Charges));

	// See SingleDashDivergentSeconds in the header for why this is timed rather than counted.
	if (Sample.HudFullPips != Sample.Charges)
	{
		SingleDashDivergentSeconds += DeltaSeconds;
	}
	SingleDashMaxTrueGain = FMath::Max(SingleDashMaxTrueGain, TrueGain);
	SingleDashMaxHudGain = FMath::Max(SingleDashMaxHudGain, HudGain);

	// THE "WHEN THE FIRST REFILLS, THEY BOTH DO" MEASUREMENT: the pip count on the exact frame the
	// pool climbs off zero. If the readout is honest this is 1. Recorded once.
	if (SingleDashPrevCharges == 0 && Sample.Charges == 1 && SingleDashHudPipsAtFirstRefill < 0)
	{
		SingleDashHudPipsAtFirstRefill = Sample.HudFullPips;
	}

	if (bChanged || TestWorld->GetTimeSeconds() >= SingleDashNextSampleTime)
	{
		SingleDashNextSampleTime = static_cast<float>(TestWorld->GetTimeSeconds()) + 0.5f;
		UE_LOG(LogTraceGame, Display,
			TEXT("SINGLEDASH t=%6.2f ph%d %-7s | TRUE %d/%d | HUD pips %d/%d (charges=%d frac=%.3f rem=%.2f) "
			     "| refillClock=%5.2f dash=%4.2f%s"),
			TestWorld->GetTimeSeconds(), SingleDashPhase, bChanged ? TEXT("CHANGE") : TEXT(""),
			Sample.Charges, Sample.MaxCharges, Sample.HudFullPips, Sample.HudMaxCharges,
			Sample.HudCharges, Sample.HudRechargeFraction, Sample.HudRemaining,
			DashRechargeRemaining, DashTimeRemaining,
			(Sample.HudFullPips != Sample.Charges) ? TEXT("   <<< HUD DISAGREES WITH THE POOL") : TEXT(""));
	}

	SingleDashPrevCharges = Sample.Charges;
	SingleDashPrevHudPips = Sample.HudFullPips;

	switch (SingleDashPhase)
	{
	case 0:
	{
		// --- "When dash is used, both charges are consumed." ONE press, from a full pool. ---------
		if (SingleDashPhaseTime >= 0.2f && SingleDashPresses == 0)
		{
			SingleDashPresses = 1;
			UE_LOG(LogTraceGame, Display,
				TEXT("SINGLEDASH ---- PHASE 0: ONE dash press from a full pool (true=%d/%d, hud pips=%d)"),
				Sample.Charges, Sample.MaxCharges, Sample.HudFullPips);
			StartDash();
		}
		else if (SingleDashPhaseTime >= 1.4f)
		{
			TraceSingleDashCheck(TEXT("one press spends exactly one charge"),
				Sample.Charges == Sample.MaxCharges - 1, SingleDashFailures,
				FString::Printf(TEXT("pool is %d/%d after one press, expected %d/%d"),
					Sample.Charges, Sample.MaxCharges, Sample.MaxCharges - 1, Sample.MaxCharges));

			TraceSingleDashCheck(TEXT("HUD shows one charge gone after one press"),
				Sample.HudFullPips == Sample.MaxCharges - 1, SingleDashFailures,
				FString::Printf(TEXT("HUD draws %d full pips, pool holds %d  (frac=%.3f rem=%.2f)"),
					Sample.HudFullPips, Sample.Charges, Sample.HudRechargeFraction, Sample.HudRemaining));

			SingleDashPhase = 1;
			SingleDashPhaseTime = 0.f;
		}
		break;
	}

	case 1:
	{
		// --- The 1 -> 2 refill. One charge, not two. ---------------------------------------------
		if (Sample.Charges >= Sample.MaxCharges || SingleDashPhaseTime > Window + 3.f)
		{
			TraceSingleDashCheck(TEXT("the pool refilled back to full"),
				Sample.Charges >= Sample.MaxCharges, SingleDashFailures,
				FString::Printf(TEXT("pool is %d/%d after %.1fs (window %.2fs)"),
					Sample.Charges, Sample.MaxCharges, SingleDashPhaseTime, Window));

			SingleDashPhase = 2;
			SingleDashPhaseTime = 0.f;
			SingleDashPresses = 0;
		}
		break;
	}

	case 2:
	{
		// --- Drain the pool to ZERO, so the 0 -> 1 refill can be watched. -------------------------
		const float SecondPressAt = 0.2f + FMath::Max(0.35f, GetDashDuration() + 0.2f);
		if (SingleDashPhaseTime >= 0.2f && SingleDashPresses == 0)
		{
			SingleDashPresses = 1;
			StartDash();
		}
		else if (SingleDashPhaseTime >= SecondPressAt && SingleDashPresses == 1)
		{
			SingleDashPresses = 2;
			StartDash();
		}
		else if (SingleDashPhaseTime >= SecondPressAt + 1.2f)
		{
			TraceSingleDashCheck(TEXT("two presses spend exactly two charges"),
				Sample.Charges == 0, SingleDashFailures,
				FString::Printf(TEXT("pool is %d/%d after two presses, expected 0/%d"),
					Sample.Charges, Sample.MaxCharges, Sample.MaxCharges));

			TraceSingleDashCheck(TEXT("HUD reads empty when the pool is empty"),
				Sample.HudFullPips == 0, SingleDashFailures,
				FString::Printf(TEXT("HUD draws %d full pips on a pool of %d"),
					Sample.HudFullPips, Sample.Charges));

			SingleDashPhase = 3;
			SingleDashPhaseTime = 0.f;
			SingleDashHudPipsAtFirstRefill = -1;
			SingleDashMaxTrueGain = 0;
			SingleDashMaxHudGain = 0;
		}
		break;
	}

	case 3:
	{
		// --- "When the first refills, they both do." THE MONEY SHOT. -----------------------------
		if (Sample.Charges >= Sample.MaxCharges || SingleDashPhaseTime > (2.f * Window) + 4.f)
		{
			TraceSingleDashCheck(TEXT("charges return ONE at a time"),
				SingleDashMaxTrueGain <= 1, SingleDashFailures,
				FString::Printf(TEXT("largest single-frame pool gain was +%d"), SingleDashMaxTrueGain));

			TraceSingleDashCheck(TEXT("when the FIRST refills, the HUD gains ONE pip"),
				SingleDashHudPipsAtFirstRefill == 1, SingleDashFailures,
				FString::Printf(TEXT("HUD drew %d full pips on the frame the pool went 0 -> 1"),
					SingleDashHudPipsAtFirstRefill));

			TraceSingleDashCheck(TEXT("HUD pips never jump by more than one"),
				SingleDashMaxHudGain <= 1, SingleDashFailures,
				FString::Printf(TEXT("largest single-frame pip gain was +%d"), SingleDashMaxHudGain));

			// One 60 Hz frame is 16.7 ms; 50 ms is three of them, which is slack for a hitching
			// offscreen run and still two orders of magnitude below the 3.68 s lie the bug produced.
			TraceSingleDashCheck(TEXT("HUD agrees with the pool (no visible lie)"),
				SingleDashDivergentSeconds <= 0.05f, SingleDashFailures,
				FString::Printf(TEXT("meter disagreed with the pool for %.3fs total "
					"(worst |pips - charges| = %d); budget 0.050s"),
					SingleDashDivergentSeconds, SingleDashMaxHudDivergence));

			bSingleDashReported = 1;
			UE_LOG(LogTraceGame, Display,
				TEXT("SINGLEDASH ==== %s  (%d failing check%s) — client, carrying, netMode=%d role=%d"),
				(SingleDashFailures == 0) ? TEXT("ALL CHECKS PASSED") : TEXT("FAILED"),
				SingleDashFailures, (SingleDashFailures == 1) ? TEXT("") : TEXT("s"),
				static_cast<int32>(GetNetMode()), static_cast<int32>(CharacterOwner->GetLocalRole()));
		}
		break;
	}

	default:
		break;
	}
}

static void TraceReportWallJump()
{
	if (GEngine == nullptr)
	{
		return;
	}

	int32 Reported = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		const UWorld* ContextWorld = Context.World();
		if (ContextWorld == nullptr)
		{
			continue;
		}

		for (FConstPlayerControllerIterator It = ContextWorld->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			const ACharacter* PawnCharacter = (PC != nullptr) ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
			if (PawnCharacter == nullptr || !PawnCharacter->IsLocallyControlled())
			{
				continue;
			}

			const UTraceCharacterMovementComponent* Movement =
				Cast<UTraceCharacterMovementComponent>(PawnCharacter->GetCharacterMovement());
			if (Movement == nullptr)
			{
				continue;
			}

			++Reported;
			Movement->LogWallJumpReport();

			// SPEC v10 §5. Printed alongside, always: the two answer different questions (what the
			// launch DID vs how long the player was stuck), and the second is the one the complaint is
			// about. Splitting them across two commands is how a measurement gets forgotten.
			Movement->LogWallStickReport();
		}
	}

	if (Reported == 0)
	{
		UE_LOG(LogTraceGame, Display, TEXT("WALLJUMP REPORT: no locally-controlled pawn in this process."));
	}
}

void UTraceCharacterMovementComponent::LogV9TuningReport() const
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	// GetGravityZ() is the engine's own accessor and already includes GravityScale, which is the field
	// spec v9 §8 moves. Reading it (rather than WorldGravityZ x GravityScale by hand) is what makes
	// this report the SAME number PhysFalling integrates.
	const float GravityZ = GetGravityZ();
	const float G = FMath::Max(1.f, FMath::Abs(GravityZ));

	// --- The launches, and the two things gravity does to each of them ----------------------------
	// apex = v^2 / 2g and hang time = 2v / g. Both scale as 1/g exactly, so a 12% heavier gravity is
	// -10.71% on every height and every air time in the game. Printing them per-launch rather than
	// quoting that one percentage is deliberate: the ABSOLUTE loss is what decides whether a specific
	// ledge is still clearable, and 10.71% of a big number is a big number.
	const float JumpZ = FMath::Max(1.f, JumpZVelocity);
	const float JumpApex = (JumpZ * JumpZ) / (2.f * G);
	const float JumpAirTime = (2.f * JumpZ) / G;

	const float WallJumpZ = JumpZ * GetWallJumpVerticalMultiplier();
	const float WallApex = (WallJumpZ * WallJumpZ) / (2.f * G);
	const float WallAirTime = (2.f * WallJumpZ) / G;

	// A slide-jump's Z is the jump's, times the slide-jump multiplier, times the well-timed Z bonus.
	const float SlideJumpZ = JumpZ * GetSlideJumpZMultiplier() * GetSlideJumpWindowZBonus();
	const float SlideJumpApex = (SlideJumpZ * SlideJumpZ) / (2.f * G);

	// The vertical dash. Its rise has TWO parts and only the second is gravity's: for
	// GetDashDuration() the dash holds DashSpeed on rails, then the exit clamp hands the pawn back at
	// no more than GetDashExitVerticalSpeedLimit() and the rest is ordinary ballistics.
	const float DashRailRise = GetDashSpeed() * GetDashDuration();
	const float DashExitZ = GetDashExitVerticalSpeedLimit();
	const float DashBallisticRise = (DashExitZ * DashExitZ) / (2.f * G);

	// --- The slide, integrated rather than asserted -----------------------------------------------
	// v(t) = v0 - a.t, so the slide ends at whichever comes first: the clock, or decaying to the exit
	// threshold. Distance is the integral to that moment. The reference entry speed is the air-strafe
	// hard cap, i.e. the fastest a player can legitimately arrive.
	const float SlideRefEntry = GetAirStrafeHardCapSpeed();
	const float SlideDecel = GetSlideDeceleration();
	const float SlideExitSpeed = FMath::Max(1.f, Settings.WalkSpeed) * FMath::Max(0.f, Settings.SlideExitSpeedFraction);
	const float SlideDecayTime = (SlideDecel > 1.f)
		? FMath::Max(0.f, (SlideRefEntry - SlideExitSpeed) / SlideDecel)
		: 1.0e9f;   // a literal, not BIG_NUMBER/MAX_FLT: those macros are UE_-prefixed in UE5.
	const float SlideT = FMath::Min(GetSlideDuration(), SlideDecayTime);
	const float SlideLength = SlideRefEntry * SlideT - 0.5f * SlideDecel * SlideT * SlideT;
	const float SlideEndSpeed = SlideRefEntry - SlideDecel * SlideT;

	// --- The slide-jump bonus, as the speed a player actually leaves at ---------------------------
	const float SlideJumpMissed = SlideEndSpeed * GetSlideJumpHorizontalRetention();
	const float SlideJumpTimed = SlideJumpMissed * GetSlideJumpWindowSpeedBonus();

	UE_LOG(LogTraceGame, Display, TEXT("V9TUNING ============================================================"));
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING arm=%s  pawn=%s netMode=%d role=%d"),
		IsV9LegacyTuning() ? TEXT("LEGACY (pre-v9)") : TEXT("V9 (shipped)"),
		*GetNameSafe(CharacterOwner),
		(GetWorld() != nullptr) ? static_cast<int32>(GetWorld()->GetNetMode()) : -1,
		(CharacterOwner != nullptr) ? static_cast<int32>(CharacterOwner->GetLocalRole()) : -1);

	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 gravity   scale=%.3f  gravityZ=%8.1f uu/s^2"),
		GravityScale, GravityZ);
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 airstrafe softCap=%7.1f  hardCap=%7.1f  (asymptoteScale=%.3f, maxAirSpeed=%7.1f)"),
		GetAirStrafeSoftCapSpeed(), GetAirStrafeHardCapSpeed(), GetAirStrafeAsymptoteScale(),
		Settings.MaxAirSpeed);

	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 knock-on  jump:      z=%6.1f apex=%7.1fuu airTime=%.3fs"),
		JumpZ, JumpApex, JumpAirTime);
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 knock-on  wallJump:  z=%6.1f apex=%7.1fuu airTime=%.3fs"),
		WallJumpZ, WallApex, WallAirTime);
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 knock-on  slideJump: z=%6.1f apex=%7.1fuu (well-timed, zBonus=%.3f)"),
		SlideJumpZ, SlideJumpApex, GetSlideJumpWindowZBonus());
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §8 knock-on  dash(up):  railRise=%7.1fuu + ballistic=%7.1fuu = %7.1fuu (exitZ=%6.1f)"),
		DashRailRise, DashBallisticRise, DashRailRise + DashBallisticRise, DashExitZ);
	// The "MANTLE HEADROOM" line (jump apex vs the tallest climbable ledge) was here. Deleted with the
	// mantle in v12 §5. The jump apex itself is still printed two lines up, which is the number that
	// actually matters now: it is what decides whether a raised section is CLEARED and landed on —
	// the case the ledge complaint is about — or run into face-first.
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §5 wallJump  retention=%.4f  window=%.4fs  outward=%5.1f  zMul=%.3f  cap=%d "
		     "(v12 §5: no mantle lockout - nothing left to lock out)"),
		GetWallJumpSpeedRetention(), GetWallJumpWindowSeconds(), GetWallJumpOutwardImpulse(),
		GetWallJumpVerticalMultiplier(), GetWallJumpMaxConsecutive());

	// THE END-TO-END §5 NUMBER, AT A FIXED ENTRY SPEED — and it has to be fixed, because the aggregate
	// "% carried" in WALLJUMP REPORT is NOT comparable between arms. Two things confound it: the runs
	// arrive at the wall at different speeds (the §8 asymptote nudge alone moves the approach), and
	// GetWallJumpOutwardImpulse() is a FLAT addition, so it is a larger fraction of a slower approach.
	// That is why the measured legacy run reads 111.5% carried and the v9 run reads 118.3% — the v9 run
	// simply approached slower. Held at one speed the comparison is clean and monotonic.
	//
	// Head-on, so the reflection returns the whole planar component: launch = entry x retention +
	// outward. This is the arithmetic TryWallJump() performs, not a model of it.
	const float WallRefEntry = 1100.f;
	const float WallLaunch = WallRefEntry * GetWallJumpSpeedRetention() + GetWallJumpOutwardImpulse();
	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §5 wallJump  HEAD-ON at %6.1f uu/s -> launch %7.1f uu/s = %.1f%% carried "
		     "(entry x %.4f + %.1f)"),
		WallRefEntry, WallLaunch, 100.f * WallLaunch / WallRefEntry,
		GetWallJumpSpeedRetention(), GetWallJumpOutwardImpulse());

	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §6 slide     duration=%.4fs  MAX LENGTH=%8.1fuu  (entry=%7.1f decel=%6.1f "
		     "endSpeed=%7.1f endedBy=%s)"),
		GetSlideDuration(), SlideLength, SlideRefEntry, SlideDecel, SlideEndSpeed,
		(SlideDecayTime <= GetSlideDuration()) ? TEXT("decay") : TEXT("clock"));

	// SPEC v24 §8 — the config side of the shorter slide and the earlier window, printed as the
	// ARITHMETIC so a reader can see where the 0.4 s went. This is deliberately NOT the evidence:
	// the item asks for the window's real open and close times, and those come from the V24WINDOW
	// line that -TraceSlideDebug prints off live slides. This block is what that line is checked
	// against, and the two disagreeing is the whole reason both exist.
	{
		const float TrimKnob = FMath::Max(0.f, Settings.SlideDurationTrimSeconds);
		const float TrimApplied = IsV24LegacySlide() ? 0.f : TrimKnob;
		const float ShippedDuration = GetSlideDuration();
		const float Window = GetSlideJumpWindowSeconds();
		UE_LOG(LogTraceGame, Display,
			TEXT("V24TUNING §8 slide     base=%.4f x lengthScale=%.4f - trim=%.4f = %.4fs   "
			     "bonus window opens at %.4fs, closes at %.4fs (+%.4fs coyote), arm=%s"),
			Settings.SlideDuration,
			IsV9LegacyTuning() ? 1.f : TraceMoveKnob::Float(TEXT("SlideMaxLengthScale"), 0.7f),
			TrimApplied, ShippedDuration,
			FMath::Max(0.f, ShippedDuration - Window), ShippedDuration, Window,
			IsV24LegacySlide() ? TEXT("LEGACY (pre-v24)") : TEXT("V24 (shipped)"));
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("V9TUNING §7 slideJump multiplier=%.5f (base=%.5f gainOnly=%d)  missed=%7.1f -> timed=%7.1f "
		     "uu/s (+%6.1f)"),
		GetSlideJumpWindowSpeedBonus(), Settings.SlideJumpWindowSpeedBonus,
		TraceMoveKnob::Bool(TEXT("bSlideJumpBonusScalesGainOnly"), true) ? 1 : 0,
		SlideJumpMissed, SlideJumpTimed, SlideJumpTimed - SlideJumpMissed);

	// --- SPEC v26 §3, the config side ------------------------------------------------------------
	//
	// Printed as the ARITHMETIC, exactly like the v24 block above and for the same reason: this is
	// what the LIVE measurement (Trace.Move.AuditV16.SlideChain, and the SLIDEJUMPCAP lines under
	// -TraceSlideDebug) is checked AGAINST. The two disagreeing is the whole reason both exist. It is
	// deliberately NOT the evidence for §3 — §3 asks for the speed after one, two, three and four
	// CHAINED hops, and no config read can produce those.
	{
		const float MomentumScale = IsV26LegacySlideJump()
			? 1.f
			: FMath::Clamp(Settings.SlideJumpMomentumScale, 0.f, 2.f);

		// The pre-scale multiplier, recovered rather than re-derived: whatever the base, the v9 scale
		// and both readings resolved to, undoing one known factor is safe and cannot drift from the
		// shipped expression.
		const float Shipped = GetSlideJumpWindowSpeedBonus();
		const float BeforeV26 = (MomentumScale > KINDA_SMALL_NUMBER)
			? (1.f + (Shipped - 1.f) / MomentumScale)
			: Shipped;

		UE_LOG(LogTraceGame, Display,
			TEXT("V26TUNING §3a slideJump gain %.5f -> %.5f (x%.3f on the GAIN)  multiplier %.5f -> %.5f  "
			     "arm=%s"),
			BeforeV26 - 1.f, Shipped - 1.f, MomentumScale, BeforeV26, Shipped,
			IsV26LegacySlideJump() ? TEXT("LEGACY (pre-v26)") : TEXT("V26 (shipped)"));

		UE_LOG(LogTraceGame, Display,
			TEXT("V26TUNING §3b chain    ceiling=%s  capAfter=%d boost(s)  resetAt=%.2f x maxGroundSpeed  "
			     "(this pawn: %5.0f uu/s)"),
			IsSlideJumpChainCapEnabled() ? TEXT("ON") : TEXT("OFF"),
			GetSlideJumpChainCapBoosts(),
			FMath::Clamp(Settings.SlideJumpChainResetSpeedMultiplier, 0.f, 3.f),
			GetSlideJumpChainResetSpeed());
	}

	UE_LOG(LogTraceGame, Display, TEXT("V9TUNING ============================================================"));
}

/**
 * Trace.V9.Tuning — the §§5-8 numbers for every locally-controlled pawn in this process.
 *
 * Safe on a host: nothing in the report is a prediction quantity, it is all config arithmetic, so
 * unlike the correction counters it means the same thing on either machine. (The wall-jump RETENTION
 * measurement is a different matter and still has to come from a client — see Trace.WallJumpReport.)
 */
static void TraceReportV9Tuning()
{
	if (GEngine == nullptr)
	{
		return;
	}

	int32 Reported = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		const UWorld* ContextWorld = Context.World();
		if (ContextWorld == nullptr)
		{
			continue;
		}

		for (FConstPlayerControllerIterator It = ContextWorld->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			const ACharacter* PawnCharacter = (PC != nullptr) ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
			if (PawnCharacter == nullptr || !PawnCharacter->IsLocallyControlled())
			{
				continue;
			}

			const UTraceCharacterMovementComponent* Movement =
				Cast<UTraceCharacterMovementComponent>(PawnCharacter->GetCharacterMovement());
			if (Movement == nullptr)
			{
				continue;
			}

			++Reported;
			Movement->LogV9TuningReport();
			break;
		}

		if (Reported > 0)
		{
			break;
		}
	}

	if (Reported == 0)
	{
		// The CDO still answers every getter correctly (they are pure config reads), so a process with
		// no pawn yet gets the tuning block rather than nothing at all. JumpZVelocity and GravityScale
		// are the authored defaults there, which is exactly right for a config report.
		GetDefault<UTraceCharacterMovementComponent>()->LogV9TuningReport();
	}
}

static FAutoConsoleCommand GTraceV9TuningCmd(
	TEXT("Trace.V9.Tuning"),
	TEXT("Dev only. Spec v9 secs 5-8: every tuned number plus the gravity knock-ons (jump apex, air "
	     "time, wall-jump apex, the vertical dash arc and the mantle headroom). Run it in both arms - "
	     "add -TraceLegacyTuning to the command line for the BEFORE numbers - and diff."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceReportV9Tuning(); }));

static FAutoConsoleCommand GTraceWallJumpReportCmd(
	TEXT("Trace.WallJumpReport"),
	TEXT("Dev only. Spec v8 sec 7: entry vs launch speed, turn angle, the consecutive cap and "
	     "corrections-per-wall-jump for each locally-controlled pawn. Read it on a JOINED CLIENT."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceReportWallJump(); }));

/**
 * SPEC v8 §1 — "dash feels rubber bandy", as a number, on the machine that can have the problem.
 *
 * Prints corrections-per-dash for every locally-controlled pawn in this process. ON A LISTEN HOST IT
 * MUST READ 0.00 AND THAT PROVES NOTHING: an authoritative pawn cannot be corrected by definition,
 * which is exactly how the previous pass reported "no corrections" for a dash the user could feel
 * rubber-banding. Read it on a JOINED CLIENT with NetEmulation.PktLag 40 or it is not an answer.
 */
static void TraceReportDashNet()
{
	if (GEngine == nullptr)
	{
		return;
	}

	int32 Reported = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		const UWorld* ContextWorld = Context.World();
		if (ContextWorld == nullptr)
		{
			continue;
		}

		for (FConstPlayerControllerIterator It = ContextWorld->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			const ACharacter* PawnCharacter = (PC != nullptr) ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
			if (PawnCharacter == nullptr || !PawnCharacter->IsLocallyControlled())
			{
				continue;
			}

			const UTraceCharacterMovementComponent* Movement =
				Cast<UTraceCharacterMovementComponent>(PawnCharacter->GetCharacterMovement());
			if (Movement == nullptr)
			{
				continue;
			}

			++Reported;
			Movement->LogDashNetReport();
		}
	}

	if (Reported == 0)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("DASHNET REPORT: no locally-controlled pawn in this process."));
	}
}

static FAutoConsoleCommand GTraceDashNetReportCmd(
	TEXT("Trace.DashNetReport"),
	TEXT("Dev only. Spec v8 sec 1: prints CORRECTIONS PER DASH for each locally-controlled pawn. Only "
	     "meaningful on a JOINED CLIENT - a listen host cannot be corrected and always reads 0."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceReportDashNet(); }));

static FAutoConsoleCommand GTraceDashVectorTestCmd(
	TEXT("Trace.DashVectorTest"),
	TEXT("Dev only. Prints the composed dash vector for every input/aim combination in spec v7 sec 5 "
	     "(A only, D only, W while looking up, W at 45 degrees, W+D) and asserts that each one is a "
	     "single dash length and that the strafe axis stays level."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceRunDashVectorTest(); }));

#endif // !UE_BUILD_SHIPPING

// -------------------------------------------------------------------------------------------
// FSavedMove_Trace
// -------------------------------------------------------------------------------------------

FSavedMove_Trace::FSavedMove_Trace()
	: bSavedWantsToDash(0)
	, bSavedWantsToJumpHold(0)
	, bSavedWantsToSlide(0)
	, bSavedMomentumActive(0)
	, SavedDashTimeRemaining(0.f)
	, SavedDashRechargeRemaining(0.f)
	, SavedDashCharges(0)
	, SavedLastMaxDashCharges(0)
	, SavedDashDirection(FVector::ZeroVector)
	, bSavedDashAimRotationValid(0)
	, SavedSlideTimeRemaining(0.f)
	, SavedSlideCooldownRemaining(0.f)
	, SavedSlideSpeed(0.f)
	, SavedSlideBufferRemaining(0.f)
	, SavedSlideDirection(FVector::ZeroVector)
	, bSavedSlideHeldLastMove(0)
	, bSavedWasAirborneLastMove(0)
	, SavedSlideJumpGraceRemaining(0.f)
	, bSavedSlideJumpGraceWellTimed(0)
	, SavedSlideJumpChainBoosts(0)
	, SavedSlideJumpChainCeiling(0.f)
	, SavedGroundGraceRemaining(0.f)
	, SavedWallJumpLaunchNormal(FVector::ZeroVector)
	, SavedWallJumpControlLockoutRemaining(0.f)
	, SavedWallJumpInputBufferRemaining(0.f)
	, SavedSurfPlaneNormal(FVector::ZeroVector)
	, SavedSurfContactRemaining(0.f)
	, SavedSurfEntrySpeed(0.f)
	, SavedSurfElapsedSeconds(0.f)
	, SavedSurfPeakSpeed(0.f)
	, SavedSurfExitCarryRemaining(0.f)
	, SavedSurfExitSpeed(0.f)
	, bSavedKnifeMovementProfile(0)
{
}

void FSavedMove_Trace::Clear()
{
	Super::Clear();

	// Saved moves are pooled and recycled — every added field must be reset or a stale ability will
	// resurrect itself several moves later.
	bSavedWantsToDash = 0;
	bSavedWantsToJumpHold = 0;
	bSavedWantsToSlide = 0;
	bSavedMomentumActive = 0;

	SavedDashTimeRemaining = 0.f;
	SavedDashRechargeRemaining = 0.f;
	SavedDashCharges = 0;
	SavedLastMaxDashCharges = 0;
	SavedDashDirection = FVector::ZeroVector;

	SavedSlideTimeRemaining = 0.f;
	SavedSlideCooldownRemaining = 0.f;
	SavedSlideSpeed = 0.f;
	SavedSlideBufferRemaining = 0.f;
	SavedSlideDirection = FVector::ZeroVector;
	bSavedSlideHeldLastMove = 0;
	bSavedWasAirborneLastMove = 0;
	SavedSlideJumpGraceRemaining = 0.f;
	bSavedSlideJumpGraceWellTimed = 0;

	// Spec v26 §3b. Same pooling argument: a stale chain counter left in a recycled move would tell a
	// replay it was three hops deep and clamp a launch the player was entitled to in full.
	SavedSlideJumpChainBoosts = 0;
	SavedSlideJumpChainCeiling = 0.f;

	// Spec v5 §7. Moves are pooled, so this is reset like everything else: a stale ledge grace left
	// in a recycled move would tell a mid-air replay it was standing on something. The six Mantle*
	// companions that used to be cleared here went with the mantle in v12 §5.
	SavedGroundGraceRemaining = 0.f;

	// Spec v8 §7. Same pooling argument as the ledge grace above: a stale wall-jump window left in a
	// recycled move would let a replay take a wall jump off a wall that is no longer there, from a
	// normal belonging to a different surface.
	SavedWallJumpNormal = FVector::ZeroVector;
	SavedWallJumpWindowRemaining = 0.f;
	SavedWallJumpEntryVelocity = FVector::ZeroVector;
	SavedWallJumpsSinceGround = 0;

	// Spec v10 §5. Same pooling argument again, and it bites harder here than anywhere: a stale
	// lockout normal left in a recycled move would silently refuse an input direction on a replayed
	// frame that has no wall anywhere near it, and a stale buffer would fire a wall jump the player
	// never pressed.
	SavedWallJumpLaunchNormal = FVector::ZeroVector;
	SavedWallJumpControlLockoutRemaining = 0.f;
	SavedWallJumpInputBufferRemaining = 0.f;

	// PATCH 28 §5. Same pooling argument as everything above: a stale surf clock left in a recycled
	// move would tell a replay it was riding a ramp in the middle of open air, and the surf ceiling
	// would clamp a frame the server never clamped.
	SavedSurfPlaneNormal = FVector::ZeroVector;
	SavedSurfContactRemaining = 0.f;
	SavedSurfEntrySpeed = 0.f;
	SavedSurfElapsedSeconds = 0.f;
	SavedSurfPeakSpeed = 0.f;

	// DEMO 29 ITEM 4. The same pooling argument, and it bites hard here: a stale carry window in a
	// recycled move would tell a replayed GROUNDED frame to hold a speed the server was bleeding, and
	// a stale exit speed would let the next landing anywhere in the level roll a fall into the floor.
	SavedSurfExitCarryRemaining = 0.f;
	SavedSurfExitSpeed = 0.f;

	// Spec v10 §1. A stale knife bit would replay a move at 130% speed with the gun in hand.
	bSavedKnifeMovementProfile = 0;

	// Spec v8 §1. Zeroed with everything else; SetMoveFor seeds it and PostUpdate_Record refines it.
	SavedDashAimRotation = FRotator::ZeroRotator;
	bSavedDashAimRotationValid = 0;
}

uint8 FSavedMove_Trace::GetCompressedFlags() const
{
	uint8 Result = Super::GetCompressedFlags();

	if (bSavedWantsToDash)
	{
		Result |= FLAG_Custom_0;
	}
	// DEMO 19 ITEM 4. FLAG_Custom_1 was boost's and was free from spec v3 §1 until now; it carries the
	// jump-held level, which is the only way a remote client's RELEASE reaches the server — the
	// ability layer's release hook has no caller and there is no release RPC.
	if (bSavedWantsToJumpHold)
	{
		Result |= FLAG_Custom_1;
	}
	if (bSavedWantsToSlide)
	{
		Result |= FLAG_Custom_2;
	}

	return Result;
}

bool FSavedMove_Trace::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const
{
	// NewMove is a TSharedPtr; the widely-copied `(FMySavedMove*)&NewMove` idiom casts the address
	// of the *pointer* and reads garbage. Go through Get().
	const FSavedMove_Trace* Other = static_cast<const FSavedMove_Trace*>(NewMove.Get());
	if (Other == nullptr)
	{
		return false;
	}

	// Different intent means different simulation — merging them would drop or duplicate an
	// activation on the server. The slide flag is included even though it is a level rather than an
	// edge, because the edge is DERIVED from it: merging a held frame with a released one would
	// erase a press the fast-fall depends on.
	//
	// The jump-held level joins them for a narrower reason. Nothing in PerformMovement reads it, so a
	// merge could not corrupt a simulation — but the server learns the RELEASE from this bit and from
	// nothing else, and a merge that straddled the release would hand the server a frame that still
	// said "held". On a flying Lily that is a frame of climb the player did not ask for, which is the
	// exact complaint this bit was added to fix.
	if (bSavedWantsToDash != Other->bSavedWantsToDash
		|| bSavedWantsToJumpHold != Other->bSavedWantsToJumpHold
		|| bSavedWantsToSlide != Other->bSavedWantsToSlide)
	{
		return false;
	}

	// SPEC v10 §1. A weapon swap changes GetMaxSpeed() by 30% and all three air ceilings by 25-35%,
	// so two moves either side of one are simulated against different limits. Merging them replays a
	// single move under whichever profile happens to be live, and the server — which ran them
	// separately — corrects the difference. A LEVEL, not an edge, exactly like the slide flag above.
	if (bSavedKnifeMovementProfile != Other->bSavedKnifeMovementProfile)
	{
		return false;
	}

	// Never merge across an active dash or slide. Combining replays one longer move from the older
	// move's start state; the clocks are linear in dt so the maths would survive, but the frame on
	// which the ability *ends* would move, and with it the velocity profile.
	//
	// The slide-jump's coyote window is in the same list and for the same reason: it decides whether
	// a jump is a slide-jump at all, so a merged move straddling its expiry would resolve the jump
	// differently from the two moves it replaced — and the difference is the whole carry.
	//
	// SPEC v5 §7's mantle used to be in this list — the strictest case in the kit, since its velocity
	// was (target - here)/time-left and its exit was a distance test. Removed in v12 §5 with the
	// mechanic. No merge that was refused for the mantle alone was refused for any other reason, so
	// this list gets slightly less strict; that is a bandwidth saving and not a behaviour change,
	// because there is no longer any move a mantle could have been live on.
	//
	// THE LEDGE GRACE IS DELIBERATELY NOT IN THIS LIST, and it would be a bandwidth bug if it were.
	// GroundGraceRemaining is refilled on every grounded move, so testing it here would refuse to
	// merge ANY ground movement at all — every walking move would go to the server separately. It
	// needs no test: the grace only ever changes a decision while the pawn is airborne, and an
	// airborne move already sets bSavedMomentumActive below, which refuses the merge.
	//
	// (SavedSlideCommitRemaining was in this list and is deleted with the commit window — spec v5 §3.
	// Nothing is lost: SavedSlideTimeRemaining already refuses every move the commit window could
	// have been open on.)
	//
	// SPEC v10 §5 ADDS TWO MORE, and both belong in this list for the list's own stated reason.
	// The into-wall lockout makes the air-strafe non-linear in dt in a NEW way — the wish direction
	// itself changes when the clock expires mid-move — so one merged move of length 2dt straddling
	// the expiry is not the two moves of length dt it replaced. The buffered press is stronger still:
	// it decides whether a wall jump happens at all, and on which frame, and a merged move would
	// resolve it against a wall contact that landed somewhere else inside the merged interval.
	//
	// SPEC v26 §3b ADDS ONE MORE, and it is in this list for the same reason the buffered press is.
	// While a chain is live, OnMovementUpdated is running a THRESHOLD test on planar speed every frame
	// ("is the pawn back on its feet at running pace?"), and the answer decides whether the next
	// slide-jump is clamped. A merged move evaluates that threshold once, at the far end of the merged
	// interval, instead of at each frame inside it — so a chain could end on the server and survive on
	// the client, and the two would disagree about several hundred uu/s on the next hop.
	//
	// It costs very close to nothing: every AIRBORNE move in a chain is already refused by
	// bSavedMomentumActive below, so the only moves this newly refuses are the handful of grounded
	// ones between a landing and the next slide — which is exactly the window the threshold lives in.
	//
	// DEMO 29 ITEM 4 ADDS ONE MORE, AND IT IS THE FIRST ITEM ON THIS LIST THAT IS LIVE WHILE THE PAWN
	// IS ON ITS FEET. SurfExitCarryRemaining scales the ground overspeed bleed, which is a PER SUB-STEP
	// clamp (max(limit, speed - bleed x dt)), so a merged move straddling the window's expiry bleeds a
	// different amount than the two moves it replaced — and it decides whether a frame bleeds AT ALL,
	// which is a bigger difference than any other entry here.
	//
	// Like the surf clock below it, it costs nothing in practice: the carry only matters while the
	// pawn is over the ground limit, and IsCarryingExcessSpeed() already sets bSavedMomentumActive on
	// exactly those moves. It is written out because the two facts are independent — somebody could
	// lower the bleed, or raise the limit, and the coincidence would stop holding without any line
	// changing here.
	//
	// PATCH 28 §5 ADDS ONE MORE, AND IT COSTS EXACTLY ZERO EXTRA REFUSALS. The surf ceiling is a
	// per-sub-step clamp (min against max(entry, cap)), so f(2dt) != f(dt) twice the moment it binds,
	// which is this list's founding reason. It is free because OnMovementUpdated clears the surf on
	// the frame the pawn is grounded, so "surf is live" implies "this move is airborne" implies
	// bSavedMomentumActive is already set — every move this clause could refuse is refused two tests
	// below. It is written out anyway because the day somebody lets a surf clock survive a landing,
	// this is the line that stops the merge from silently becoming a desync.
	if (SavedDashTimeRemaining > 0.f || Other->SavedDashTimeRemaining > 0.f
		|| SavedSlideTimeRemaining > 0.f || Other->SavedSlideTimeRemaining > 0.f
		|| SavedSlideJumpGraceRemaining > 0.f || Other->SavedSlideJumpGraceRemaining > 0.f
		|| SavedSlideJumpChainBoosts > 0 || Other->SavedSlideJumpChainBoosts > 0
		|| SavedWallJumpControlLockoutRemaining > 0.f || Other->SavedWallJumpControlLockoutRemaining > 0.f
		|| SavedWallJumpInputBufferRemaining > 0.f || Other->SavedWallJumpInputBufferRemaining > 0.f
		|| SavedSurfContactRemaining > 0.f || Other->SavedSurfContactRemaining > 0.f
		|| SavedSurfExitCarryRemaining > 0.f || Other->SavedSurfExitCarryRemaining > 0.f)
	{
		return false;
	}

	// ...and never merge across the momentum model. Both of its branches clamp PER SUB-STEP —
	// min(AirAcceleration x dt, AddSpeed) in the air, max(GroundLimit, Speed - Bleed x dt) on the
	// ground — so f(2dt) != f(dt) twice whenever a clamp binds. A merged move would hand the server
	// a trajectory the client never simulated, and the correction would arrive as a rubber-band in
	// the exact situation (mid-strafe, mid-landing) where it is most visible.
	if (bSavedMomentumActive || Other->bSavedMomentumActive)
	{
		return false;
	}

	return Super::CanCombineWith(NewMove, InCharacter, MaxDelta);
}

void FSavedMove_Trace::PostUpdate(ACharacter* C, EPostUpdateMode PostUpdateMode)
{
	Super::PostUpdate(C, PostUpdateMode);

	// RECORD PASS ONLY — this is the whole reason this override exists.
	//
	// The base class stores the move's aim in SavedControlRotation, and it would be the obvious place
	// to read a dash's aim from on a replay. It is not safe: Super::PostUpdate WRITES that field, and
	// ClientUpdatePositionAfterServerUpdate calls PostUpdate(PostUpdate_Replay) on every move it has
	// just replayed. So the first correction after a dash overwrites that move's stored aim with
	// wherever the mouse happens to be pointing at correction time, and a second correction covering
	// the same move compounds it — the client replays the dash along a direction it never took, which
	// is exactly the rubber-band being chased. See the header comment on this method.
	//
	// Capturing into our own field, and only on the record pass, means the aim a move was SENT with
	// is written once and no number of replays can move it.
	if (PostUpdateMode == FSavedMove_Character::PostUpdate_Record && C != nullptr)
	{
		// Super has just written SavedControlRotation with the record-time aim, owner fallback and
		// .Clamp() included, and that is bit-for-bit the rotation FCharacterNetworkMoveData packs into
		// this move's ServerMove. Copying it rather than recomputing it is deliberate: a hand-rolled
		// copy of the engine's fallback logic is one engine change away from disagreeing with the wire.
		SavedDashAimRotation = SavedControlRotation;
		bSavedDashAimRotationValid = 1;
	}
	else if (bSavedDashAimRotationValid != 0)
	{
		// ================================================================================================
		// THE REPLAY PASS — AND THE HALF OF SPEC v8 §1 THAT THE REPLAY-SIDE FIX ALONE MADE WORSE.
		// ================================================================================================
		//
		// MEASURED, on a joined client at PktLag 40 both ways: restoring only the CLIENT's replay aim
		// (Trace.DashLegacyAimReplay 0, PrepMoveFor reading SavedDashAimRotation) took corrections-per-dash
		// from 0.43 to 2.29 — it made the rubber-band FIVE TIMES WORSE. The reason is the second consumer
		// of this field, which the replay-side fix does not reach:
		//
		//   FCharacterNetworkMoveData::ClientFillNetworkMoveData does
		//       ControlRotation = ClientMove.SavedControlRotation;
		//
		// and a correction does not just replay the unacknowledged moves, it RE-SENDS them. So the aim the
		// server re-simulates each replayed move from is whatever is in SavedControlRotation AT RESEND
		// TIME — which Super::PostUpdate(PostUpdate_Replay) has just overwritten with wherever the mouse is
		// pointing NOW (engine CharacterMovementComponent.cpp: the SavedControlRotation write is in the
		// block common to BOTH passes, not in the Record branch).
		//
		// That gives three possible worlds, and only one of them is right:
		//
		//   v7 (legacy):  replay reads the stomped aim, resend carries the stomped aim. Client and server
		//                 agree — on an aim the player never held. The dash goes somewhere neither of them
		//                 asked for, but they agree about it, so it corrects rarely.
		//   replay-only:  replay reads the recorded aim, resend still carries the stomped one. Client and
		//                 server now compose the dash from DIFFERENT rotations, every single time. This is
		//                 the 2.29/dash measurement.
		//   this:         the stomp is undone, so the replay AND the resend AND the server all use the aim
		//                 the move was recorded with.
		//
		// The aim a move was made with is a historical fact about that move. Letting a replay rewrite it is
		// the bug in both directions, and putting it back here is the only place that fixes both consumers
		// at once — PrepMoveFor cannot, because it runs before the resend and does not own this field.
		//
		// This is not falsifying what the server was told: the ORIGINAL ServerMove for this timestamp
		// already carried exactly this rotation. A resend that carried anything else would be handing the
		// server different input for a timestamp it has already seen, which is the definition of a
		// mispredicted move.
#if !UE_BUILD_SHIPPING
		// The A/B arm. Trace.DashLegacyAimReplay 1 leaves the engine's stomp in place, so the legacy
		// number can be reproduced in this build rather than argued about.
		extern int32 GTraceDashLegacyAimReplay;
		if (GTraceDashLegacyAimReplay == 0)
#endif
		{
			SavedControlRotation = SavedDashAimRotation;
		}
	}
}

void FSavedMove_Trace::CombineWith(const FSavedMove_Character* OldMove, ACharacter* InCharacter,
	APlayerController* PC, const FVector& OldStartLocation)
{
	Super::CombineWith(OldMove, InCharacter, PC, OldStartLocation);

	// When two moves merge, the combined move starts where the OLDER one did, so any state captured
	// at the start of a move has to be re-based onto the older move's start or the replay begins from
	// the wrong values.
	//
	// CanCombineWith already refuses to merge moves that differ in any ability or momentum state, so
	// by the time we get here the two moves agree on all of it and there is nothing left to reconcile.
	//
	// THE AIM IS DELIBERATELY NOT RE-BASED ONTO THE OLDER MOVE, and an earlier revision of this file
	// did exactly that. SavedDashAimRotation's invariant is "the rotation this move's ServerMove
	// carries", i.e. it must equal SavedControlRotation as recorded — because PostUpdate's replay
	// branch now writes it BACK into SavedControlRotation, which is what the resend reads. Combining
	// runs in ReplicateMoveToServer BEFORE the combined move is simulated and before its
	// PostUpdate(PostUpdate_Record), so the aim is re-recorded from the combined move immediately
	// after this returns. Assigning the older move's rotation here would be overwritten in the happy
	// path and would silently feed the server a rotation it was never sent in any other, so the
	// honest thing is to leave the field alone and let the record pass own it.
	(void)OldMove;
}

void FSavedMove_Trace::SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData)
{
	Super::SetMoveFor(C, InDeltaTime, NewAccel, ClientData);

	// Runs before PerformMovement, so this captures the state at the *start* of the move — which
	// is precisely what PrepMoveFor must restore before a replay.
	if (C != nullptr)
	{
		if (const UTraceCharacterMovementComponent* Movement = Cast<UTraceCharacterMovementComponent>(C->GetCharacterMovement()))
		{
			bSavedWantsToDash  = Movement->bWantsToDash;
			bSavedWantsToSlide = Movement->bWantsToSlide;

			// DEMO 19 ITEM 4. The FRESH answer, not the raw bit: a writer that has gone quiet must
			// send "up" to the server rather than keep re-sending a stale "down" every move.
			bSavedWantsToJumpHold = Movement->IsJumpHeld() ? 1 : 0;

			// Not CMC state and deliberately not restored by PrepMoveFor: this is a property OF the
			// move, read only by CanCombineWith. See the field's comment.
			bSavedMomentumActive = (Movement->IsFalling() || Movement->IsCarryingExcessSpeed()) ? 1 : 0;

			SavedDashTimeRemaining     = Movement->DashTimeRemaining;
			SavedDashRechargeRemaining = Movement->DashRechargeRemaining;
			SavedDashCharges           = Movement->DashCharges;
			SavedLastMaxDashCharges    = Movement->LastMaxDashCharges;

			// The WHOLE vector, Z included (spec v7 §5). Nothing here had to change — it was always
			// an FVector — but the field is now load-bearing in three axes rather than two, so a
			// future "optimisation" to a yaw or an FVector2D would silently flatten every vertical
			// dash on the replay path only, which is the hardest class of bug this file can have.
			SavedDashDirection         = Movement->DashDirection;

			// NOTE: the aim rotation needs nothing here. Super::SetMoveFor has already captured it
			// into the base class's SavedControlRotation, which is the same field the ServerMove
			// packs; PrepMoveFor is where it is handed back to the component.

			SavedSlideTimeRemaining     = Movement->SlideTimeRemaining;
			SavedSlideCooldownRemaining = Movement->SlideCooldownRemaining;
			SavedSlideSpeed             = Movement->SlideSpeed;
			SavedSlideBufferRemaining   = Movement->SlideBufferRemaining;
			SavedSlideDirection         = Movement->SlideDirection;
			bSavedSlideHeldLastMove     = Movement->bSlideHeldLastMove;
			bSavedWasAirborneLastMove   = Movement->bWasAirborneLastMove;

			SavedSlideJumpGraceRemaining  = Movement->SlideJumpGraceRemaining;
			bSavedSlideJumpGraceWellTimed = Movement->bSlideJumpGraceWellTimed;

			// SPEC v26 §3b. Captured with the window beside it and for the same reason: these two
			// decide whether DoJump CLAMPS the launch, and nothing in a replay can re-derive how many
			// hops deep the chain already was.
			SavedSlideJumpChainBoosts  = Movement->SlideJumpChainBoosts;
			SavedSlideJumpChainCeiling = Movement->SlideJumpChainCeiling;

			// Spec v5 §7. The mantle's target and its up-phase Z are snapshotted with the clocks:
			// they are recomputed identically on the server from the same geometry, but the CLIENT's
			// replay must restart from the target the original move actually used, or a correction
			// mid-pull-up would re-derive it from a rewound position and aim somewhere else.
			SavedGroundGraceRemaining    = Movement->GroundGraceRemaining;

			// SPEC v8 §7. The wall jump is only predicted if its state round-trips. HandleImpact does
			// re-run on a replay (PhysFalling re-sweeps the same static geometry), so the NORMAL and the
			// WINDOW are partly self-healing — but WallJumpsSinceGround is not: nothing in a replay can
			// re-derive it, so without this capture a replay would keep incrementing the ladder counter
			// and eventually refuse a wall jump the client had already taken.
			SavedWallJumpNormal          = Movement->WallJumpNormal;
			SavedWallJumpWindowRemaining = Movement->WallJumpWindowRemaining;
			SavedWallJumpEntryVelocity   = Movement->WallJumpEntryVelocity;
			SavedWallJumpsSinceGround    = Movement->WallJumpsSinceGround;

			// SPEC v10 §5. NEITHER OF THESE IS SELF-HEALING ON A REPLAY, which is why they are here.
			// HandleImpact re-runs and partly rebuilds the window, but nothing in a replay can
			// re-derive "a jump was pressed 40 ms ago and found no wall" or "this pawn launched off a
			// face 80 ms ago and may not steer back into it" — and both change the velocity the
			// replayed frame produces.
			SavedWallJumpLaunchNormal            = Movement->WallJumpLaunchNormal;
			SavedWallJumpControlLockoutRemaining = Movement->WallJumpControlLockoutRemaining;
			SavedWallJumpInputBufferRemaining    = Movement->WallJumpInputBufferRemaining;

			// PATCH 28 §5. HandleImpact re-runs on a replay (PhysFalling re-sweeps the same static
			// geometry), so the NORMAL is partly self-healing — but the CLOCK and the ENTRY SPEED are
			// not: nothing in a replay can re-derive "this ride began 0.4 s ago at 1180 uu/s", and both
			// of them decide what the surf ceiling clamps to on the very next frame.
			SavedSurfPlaneNormal      = Movement->SurfPlaneNormal;
			SavedSurfContactRemaining = Movement->SurfContactRemaining;
			SavedSurfEntrySpeed       = Movement->SurfEntrySpeed;
			SavedSurfElapsedSeconds   = Movement->SurfElapsedSeconds;
			SavedSurfPeakSpeed        = Movement->SurfPeakSpeed;

			// DEMO 29 ITEM 4. NEITHER OF THESE IS SELF-HEALING, and both change what a replayed frame
			// computes rather than merely describing it:
			//   * SurfExitCarryRemaining decides whether the ground bleed runs on this frame at all. A
			//     replay that lost it bleeds a frame the server held, and the two ends disagree about
			//     several hundred uu/s within a handful of frames — on the ground, where it is most
			//     visible.
			//   * SurfExitSpeed is the CAP on the exit rollout. It is set from a frame that is already
			//     in the past by the time the landing happens, so nothing in a replay can re-derive it;
			//     a replay that lost it would land with the engine's flattened velocity while the server
			//     rolled the ride's descent into the floor.
			SavedSurfExitCarryRemaining = Movement->SurfExitCarryRemaining;
			SavedSurfExitSpeed          = Movement->SurfExitSpeed;

			// SPEC v10 §1. The weapon in hand at the START of this move, which is the profile the
			// move was actually simulated under.
			bSavedKnifeMovementProfile = Movement->bKnifeMovementProfile;
		}
	}
}

void FSavedMove_Trace::PrepMoveFor(ACharacter* C)
{
	Super::PrepMoveFor(C);

	if (C != nullptr)
	{
		if (UTraceCharacterMovementComponent* Movement = Cast<UTraceCharacterMovementComponent>(C->GetCharacterMovement()))
		{
			// Rewind every ability to exactly where it stood before this move ran. MoveAutonomous
			// will overwrite the two intent flags from the compressed flags immediately after this
			// returns; restoring them too costs nothing and keeps the snapshot complete.
			//
			// The momentum model needs nothing here: it is a pure function of Velocity and
			// Acceleration, both of which Super::PrepMoveFor and MoveAutonomous already restore.
			Movement->bWantsToDash  = bSavedWantsToDash;
			Movement->bWantsToSlide = bSavedWantsToSlide;

			// DEMO 19 ITEM 4. Through the setter, so the replayed value is also FRESH — restoring the
			// raw bit with a stale stamp would hand the replay a "held" that IsJumpHeld() then reads
			// as released. MoveAutonomous overwrites it from the compressed flags a moment later
			// anyway; this keeps the snapshot complete rather than nearly complete.
			Movement->SetJumpHeld(bSavedWantsToJumpHold != 0);

			Movement->DashTimeRemaining     = SavedDashTimeRemaining;
			Movement->DashRechargeRemaining = SavedDashRechargeRemaining;
			Movement->DashCharges           = SavedDashCharges;
			Movement->LastMaxDashCharges    = SavedLastMaxDashCharges;

			// SavedDashDirection IS THE FULL 3D VECTOR (spec v7 §5). It always was an FVector; what
			// changed is that its Z is now non-zero, so a correction landing mid-dash restores the
			// vertical component too and the replay re-launches along the same ray rather than a
			// flattened one.
			Movement->DashDirection         = SavedDashDirection;

			// SPEC v7 §5 — THE AIM ROTATION, AND THE REASON A VERTICAL DASH DOES NOT RUBBER-BAND.
			//
			// Acceleration round-trips for free (MoveAutonomous restores it), but the dash direction
			// now has a SECOND input, and FSavedMove_Character::PrepMoveFor does not restore the
			// control rotation — so a replayed BeginDash would compose its direction from wherever
			// the mouse is pointing at correction time, not where it pointed when the dash was
			// pressed. With the old horizontal dash that was invisible because only the yaw mattered
			// and the yaw arrived inside Acceleration; with a vertical dash it is a Z-velocity
			// disagreement between client and server, i.e. exactly the rubber-band this project has
			// already been reported for.
			//
			// SavedControlRotation is the base class's own field, filled by Super::SetMoveFor from
			// the same GetControlRotation() that FCharacterNetworkMoveData packs into this move's
			// ServerMove and that ServerMove_PerformMovement applies to the controller before running
			// MoveAutonomous. So restoring it here makes the replay compose from the identical pair
			// of inputs the server did — for free, with no new saved-move field and no bandwidth.
			// GetDashAimRotation() gates the read on ACharacter::bClientUpdating, so this value can
			// never escape the replay loop and colour a live move.
			// SPEC v8 §1 — AND WHY IT IS *NOT* SavedControlRotation.
			//
			// SavedControlRotation is written by FSavedMove_Character::PostUpdate in the block that runs
			// for BOTH passes (engine CharacterMovementComponent.cpp:12902), and
			// ClientUpdatePositionAfterServerUpdate calls PostUpdate(PostUpdate_Replay) on every move it
			// has just replayed. So the first correction after a dash stomps that move's stored aim with
			// wherever the mouse is pointing at correction time; a second correction covering the same
			// unacknowledged move then replays the dash from the stomped rotation while the server keeps
			// composing from the rotation the ServerMove actually carried. On the spec v7 vectorized
			// dash that disagreement is in Z — the rubber-band.
			//
			// SavedDashAimRotation is our own memo, written once on the record pass and immutable
			// thereafter. Fall back to the base field only for a move that was never recorded, where it
			// is the best (and only) value available.
			// Trace.DashLegacyAimReplay 1 restores the v7 source in the SAME build, which is the only
			// honest way to A/B this: two separately-built clients are two different populations of
			// network jitter. The cvar was declared for exactly this and was reading nothing.
#if !UE_BUILD_SHIPPING
			extern int32 GTraceDashLegacyAimReplay;
			const bool bUseRecordedAim = (GTraceDashLegacyAimReplay == 0) && (bSavedDashAimRotationValid != 0);
#else
			const bool bUseRecordedAim = (bSavedDashAimRotationValid != 0);
#endif
			Movement->ReplayAimRotation        = bUseRecordedAim ? SavedDashAimRotation : SavedControlRotation;
			Movement->bReplayAimRotationValid  = 1;

			Movement->SlideTimeRemaining     = SavedSlideTimeRemaining;
			Movement->SlideCooldownRemaining = SavedSlideCooldownRemaining;
			Movement->SlideSpeed             = SavedSlideSpeed;
			Movement->SlideBufferRemaining   = SavedSlideBufferRemaining;
			Movement->SlideDirection         = SavedSlideDirection;
			Movement->bSlideHeldLastMove     = bSavedSlideHeldLastMove;
			Movement->bWasAirborneLastMove   = bSavedWasAirborneLastMove;

			// The slide-jump window. Without this a correction that landed mid-window would replay a
			// slide-jump as an ordinary jump, and client and server would disagree about several
			// hundred uu/s of horizontal velocity on the most visible frame in the kit.
			Movement->SlideJumpGraceRemaining  = SavedSlideJumpGraceRemaining;
			Movement->bSlideJumpGraceWellTimed = bSavedSlideJumpGraceWellTimed;

			// SPEC v26 §3b. Rewind the chain to where it stood before this move ran. Without this a
			// correction landing mid-chain replays the third hop as a first one — uncapped — and the
			// client and the server end the launch several hundred uu/s apart.
			Movement->SlideJumpChainBoosts  = SavedSlideJumpChainBoosts;
			Movement->SlideJumpChainCeiling = SavedSlideJumpChainCeiling;

			// The mantle and the ledge grace (spec v5 §7). Without these a correction landing
			// mid-pull-up replays it as a plain fall: the pawn ends up on top of the ledge on one
			// machine and at the bottom of it on the other — the largest rubber-band the kit could
			// produce, and the exact bug the mantle was added to remove.
			Movement->GroundGraceRemaining    = SavedGroundGraceRemaining;

			// SPEC v8 §7. Rewind the wall to where it stood before this move ran. Without these a
			// correction landing inside the contact window replays the wall jump as an ordinary refused
			// mid-air jump — DoJump returns false, Velocity is put back, and client and server disagree
			// about the entire redirected launch on the most visible frame of the move.
			Movement->WallJumpNormal          = SavedWallJumpNormal;
			Movement->WallJumpWindowRemaining = SavedWallJumpWindowRemaining;
			Movement->WallJumpEntryVelocity   = SavedWallJumpEntryVelocity;
			Movement->WallJumpsSinceGround    = SavedWallJumpsSinceGround;

			// SPEC v10 §5. Without these a correction landing in the 0.20 s after a wall jump replays
			// the launch and then lets the replayed input drag the pawn straight back at the wall,
			// while the server's copy sailed away — several hundred uu/s of disagreement on the frames
			// immediately after the most visible event in the move. The buffer is worse still: losing
			// it makes the replay refuse a wall jump the server took.
			Movement->WallJumpLaunchNormal            = SavedWallJumpLaunchNormal;
			Movement->WallJumpControlLockoutRemaining = SavedWallJumpControlLockoutRemaining;
			Movement->WallJumpInputBufferRemaining    = SavedWallJumpInputBufferRemaining;

			// PATCH 28 §5. Rewind the ride to where it stood before this move ran. Without the clock a
			// replayed frame would run UNCAPPED where the server ran capped; without the entry speed it
			// would clamp a fast entry down to the shared ceiling on the client only. Both are several
			// hundred uu/s on the frames a surfing player is watching hardest.
			Movement->SurfPlaneNormal      = SavedSurfPlaneNormal;
			Movement->SurfContactRemaining = SavedSurfContactRemaining;
			Movement->SurfEntrySpeed       = SavedSurfEntrySpeed;
			Movement->SurfElapsedSeconds   = SavedSurfElapsedSeconds;
			Movement->SurfPeakSpeed        = SavedSurfPeakSpeed;

			// DEMO 29 ITEM 4. Rewind the exit too. See SetMoveFor for what each of the two decides;
			// the short version is that one of them turns the ground bleed off and the other caps the
			// rollout at the landing, so a replay missing either simulates a different floor.
			Movement->SurfExitCarryRemaining = SavedSurfExitCarryRemaining;
			Movement->SurfExitSpeed          = SavedSurfExitSpeed;

			// SPEC v10 §1. Put the right weapon back in the pawn's hand before the move is replayed:
			// GetMaxSpeed() and all three air ceilings read this bit, so a replay under the wrong one
			// is simulated against a ceiling 481 uu/s away from the server's.
			Movement->bKnifeMovementProfile = bSavedKnifeMovementProfile;
		}
	}
}

// -------------------------------------------------------------------------------------------
// FNetworkPredictionData_Client_Trace
// -------------------------------------------------------------------------------------------

FNetworkPredictionData_Client_Trace::FNetworkPredictionData_Client_Trace(const UCharacterMovementComponent& ClientMovement)
	: Super(ClientMovement)
{
}

FSavedMovePtr FNetworkPredictionData_Client_Trace::AllocateNewMove()
{
	return FSavedMovePtr(new FSavedMove_Trace());
}
