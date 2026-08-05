// Copyright (c) Trace. All Rights Reserved.
//
// See TraceCore.h for the model. This file is the whole of it: there is no physics, no pickup
// volume and no flight path left to go wrong.

#include "Gameplay/TraceCore.h"

#include "Net/UnrealNetwork.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TraceMatchTypes.h"      // TraceIsGoalMode (mode B)
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "World/TraceArenaBuilder.h"

#include "Gameplay/TraceEndzone.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                        // TActorIterator (character gather fallback)
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"        // GetServerWorldTimeSeconds()
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"             // FMath::SegmentDistToSegmentSafe
#include "UObject/ConstructorHelpers.h"
#include "UObject/UnrealType.h"                 // FindFProperty / FFloatProperty (mode B settings bridge)

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

	// --- Cosmetics --------------------------------------------------------------------------------

	/** Orb centre above the holder's capsule centre. Capsule half-height is 88, so this clears the head. */
	constexpr double OrbHeight = 150.0;

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

namespace
{
	/** Divide-by-zero epsilon. Literal on purpose: the KINDA_SMALL_NUMBER family was re-spelled
	 *  during the 5.x line and this module must compile on 5.4 - 5.8. */
	constexpr double CoreGeometryEpsilon = 1.0e-8;

	/** True when both teams are known and different. Unknown teams are never enemies. */
	bool AreEnemies(ETraceTeam A, ETraceTeam B)
	{
		return A != ETraceTeam::None && B != ETraceTeam::None && A != B;
	}

	/** True when both teams are known and equal. */
	bool AreAllies(ETraceTeam A, ETraceTeam B)
	{
		return A != ETraceTeam::None && A == B;
	}
}

// =================================================================================================
// §4.1 PASS TUNABLES
//
// Every number the pass runs on is now a UTraceSettings property, read AT THE POINT OF USE so it
// retunes with PIE running - which is what the user asked for: "implement these as tunable
// variables so I can playtest and adjust numbers rather than you guessing at feel." See
// UTraceSettings, Category "Core|Pass". The TraceCoreTuning constants below survive only as the
// PRE-FIX values, which is what the A/B switch replays.
//
// Trace.Pass.Fixes is that switch. Setting it to 0 restores the exact pre-fix behaviour (no grace,
// 6-degree cone, 40 uu slack, single chest LOS ray, a full 2 s cooldown on a cancel, no sticky
// acquisition) so the before/after numbers in the report come from ONE binary rather than two -
// nothing else changed between the two runs, including the map, the bots and the RNG seed.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarTracePassFixes(
	TEXT("Trace.Pass.Fixes"),
	1,
	TEXT("1 (default): apply the section 4.1 pass-reliability fixes and read UTraceSettings for their values. 0: exact pre-fix behaviour, for A/B measurement."),
	ECVF_Default);

namespace TraceCoreTuning
{
	/** Grace window, or 0 when the A/B switch is replaying pre-fix behaviour. */
	float ResolvedValidationGrace(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Clamp(Settings.PassValidationGraceSeconds, 0.f, 2.f)
			: 0.f;
	}

	/** Sticky acquisition window, or 0 when replaying pre-fix behaviour. */
	float ResolvedAcquireSticky(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Clamp(Settings.PassAcquireStickySeconds, 0.f, 2.f)
			: 0.f;
	}

	/** Cancel cooldown. Pre-fix this was the same two seconds a COMPLETED pass spends. */
	float ResolvedCancelCooldown(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Max(0.f, Settings.PassCancelCooldownSeconds)
			: FMath::Max(0.f, Settings.PassCooldownSeconds);
	}

	double ResolvedAimConeDegrees(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Clamp(static_cast<double>(Settings.PassAimConeDegrees), 0.5, 60.0)
			: PassAimConeDegrees;
	}

	double ResolvedAimSlack(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Clamp(static_cast<double>(Settings.PassAimSlack), 0.0, 500.0)
			: PassAimSlack;
	}

	bool ResolvedMultiPointLos(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0) && Settings.bPassMultiPointLos;
	}
}

static TAutoConsoleVariable<int32> CVarTracePassStats(
	TEXT("Trace.PassStats"),
	0,
	TEXT("1: accumulate pass acquisition / cancellation statistics. Trace.PassStats.Dump prints them."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarTracePassStatsInterval(
	TEXT("Trace.PassStats.Interval"),
	20.f,
	TEXT("Seconds between automatic Trace.PassStats dumps. 0 disables the automatic dump."),
	ECVF_Default);

namespace TracePassStats
{
	/**
	 * "Sometimes the pass option doesn't show up" is a claim about how OFTEN, so this counts how
	 * often. Sampled server-side at 10 Hz for whoever is holding the Core: does FindPassTargetFor()
	 * return anybody, and when it does not, which test refused each teammate?
	 *
	 * That last column is the whole diagnosis. "No receiver" caused by everyone being genuinely
	 * behind cover is a map problem; "no receiver" caused by the 6-degree cone is a tuning problem;
	 * and they are indistinguishable from inside the game.
	 */
	struct FStats
	{
		// --- availability, sampled while somebody is holding the Core ---
		//
		// TWO numbers, because they answer two different questions and only the second one is the
		// bug report. RawSamplesWithTarget asks the instantaneous rule "is any teammate legal RIGHT
		// NOW"; SamplesWithTarget asks FindPassTargetFor(), which is what ATraceHUD polls to decide
		// whether to draw the pass affordance at all and therefore what the player means by "the
		// pass option". The gap between them is exactly what the sticky window closes.
		int32 CarrySamples = 0;
		int32 RawSamplesWithTarget = 0;
		int32 SamplesWithTarget = 0;
		int32 SamplesWithoutTarget = 0;

		/** Per-teammate refusal tally over those samples. Indexed by ETracePassRejectReason. */
		int32 RefusalCount[8] = {};
		int32 TeammatesConsidered = 0;
		int32 TeammatesLegal = 0;

		// --- the state machine ---
		int32 PassesBegun = 0;
		int32 PassesCompleted = 0;
		int32 CancelReleased = 0;
		int32 CancelLostTarget = 0;
		int32 CancelHolderGone = 0;

		/** Times the grace window absorbed a blink of illegality that would previously have cancelled. */
		int32 GraceSaves = 0;
		/** Times the grace window expired and the pass cancelled anyway. */
		int32 GraceExpired = 0;
		/** Total seconds of in-flight pass time spent inside the grace window. */
		double GraceSecondsAccrued = 0.0;

		/** Times the sticky window kept a receiver the instantaneous test had dropped. */
		int32 StickySaves = 0;

		double LastSampleTime = 0.0;
		double LastDumpTime = 0.0;
	};

	static FStats GStats;

	static const TCHAR* RefusalName(int32 Index);

	static float Percent(int32 Part, int32 Whole)
	{
		return (Whole > 0) ? (100.f * static_cast<float>(Part) / static_cast<float>(Whole)) : 0.f;
	}

	static void Reset()
	{
		const double Keep = GStats.LastDumpTime;
		GStats = FStats();
		GStats.LastDumpTime = Keep;
	}

	static void Dump()
	{
		const FStats& S = GStats;
		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE PASS STATS =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT availability: %d samples while carrying | a receiver was offered on %d (%.1f%%), none on %d (%.1f%%)"),
			S.CarrySamples,
			S.SamplesWithTarget, Percent(S.SamplesWithTarget, S.CarrySamples),
			S.SamplesWithoutTarget, Percent(S.SamplesWithoutTarget, S.CarrySamples));
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT rawrule     : the instantaneous rule alone offered a receiver on %d (%.1f%%) - the gap to the line above is what the sticky window recovers"),
			S.RawSamplesWithTarget, Percent(S.RawSamplesWithTarget, S.CarrySamples));
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT teammates   : %d evaluated, %d legal (%.1f%%)"),
			S.TeammatesConsidered, S.TeammatesLegal, Percent(S.TeammatesLegal, S.TeammatesConsidered));

		FString Refusals;
		for (int32 Index = 1; Index < 8; ++Index)
		{
			if (S.RefusalCount[Index] > 0)
			{
				Refusals += FString::Printf(TEXT("  %-20s %6d (%.1f%%)\n"),
					RefusalName(Index), S.RefusalCount[Index],
					Percent(S.RefusalCount[Index], S.TeammatesConsidered));
			}
		}
		UE_LOG(LogTraceGame, Display, TEXT("PASSSTAT refusals by test:\n%s"), *Refusals);

		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT state       : begun %d, completed %d (%.1f%%) | cancels: released %d, lost target %d, holder gone %d"),
			S.PassesBegun, S.PassesCompleted, Percent(S.PassesCompleted, S.PassesBegun),
			S.CancelReleased, S.CancelLostTarget, S.CancelHolderGone);
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT grace       : saves %d, expired %d | %.2fs total spent inside the grace window"),
			S.GraceSaves, S.GraceExpired, S.GraceSecondsAccrued);
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT sticky      : %d acquisitions kept alive by the sticky window"), S.StickySaves);
		UE_LOG(LogTraceGame, Display, TEXT("======================================"));
	}
} // namespace TracePassStats

static FAutoConsoleCommand GTracePassStatsDumpCmd(
	TEXT("Trace.PassStats.Dump"),
	TEXT("Prints the accumulated pass acquisition / cancellation statistics gathered while Trace.PassStats is 1."),
	FConsoleCommandDelegate::CreateStatic([]() { TracePassStats::Dump(); }));

static FAutoConsoleCommand GTracePassStatsResetCmd(
	TEXT("Trace.PassStats.Reset"),
	TEXT("Clears the accumulated pass statistics."),
	FConsoleCommandDelegate::CreateStatic([]() { TracePassStats::Reset(); }));


// =================================================================================================
// MODE B: TUNABLES  (spec v4 §7)
//
// THE KNOB CONTRACT, AND HOW THIS BLOCK HONOURS IT.
// This project's rule is that every new constant is a UTraceSettings property — categorised,
// clamped, tooltipped and live-editable in PIE — because the user tunes from that panel and "a dead
// knob is worse than no knob". Three of mode B's numbers already live there (ScoringMode,
// GoalWidthFieldFraction, GoalHeightUU). The rest — CoreThrowSpeed, CorePickupRadius,
// CoreLooseResetSeconds and their neighbours — are NAMED in UTraceSettings' own documentation but
// are not declared on it yet, and TraceSettings.h is not this slice's to edit.
//
// So each one is resolved in two steps, at the point of use, every time:
//
//   1. if UTraceSettings has a float property of that exact name, its live value is used. That is a
//      reflected lookup (FindFProperty), cached per name after the first call, and it means the
//      moment the property is declared on the settings page the knob becomes a real panel slider
//      with no further edit here — including live retuning in PIE, since it reads the same CDO the
//      panel edits.
//   2. otherwise the console variable below is used, which is itself live-editable and settable
//      from Config/*.ini ([SystemSettings]) and the command line.
//
// A CVar explicitly set at runtime (`Trace.ModeB.ThrowSpeed 3000`) wins over the settings property,
// so a measurement run can pin a value without touching the user's page. Nothing here is a
// compile-time constant that a designer cannot reach.
// =================================================================================================

namespace TraceModeBTuning
{
	/**
	 * Reads a float property off UTraceSettings by name, if one exists.
	 *
	 * The name lookup is done ONCE per name and cached — including the negative result — so the
	 * steady-state cost is a TMap probe and a pointer dereference, i.e. the same order as reading
	 * the property directly. The cache holds an FProperty, not a value, so live edits still land.
	 */
	static bool TrySettingsFloat(const TCHAR* PropertyName, float& OutValue)
	{
		static TMap<FName, const FFloatProperty*> Cache;

		const FName Key(PropertyName);
		const FFloatProperty* const* Found = Cache.Find(Key);
		if (Found == nullptr)
		{
			Found = &Cache.Add(Key, FindFProperty<FFloatProperty>(UTraceSettings::StaticClass(), Key));
		}

		if (*Found == nullptr)
		{
			return false;
		}

		OutValue = (*Found)->GetPropertyValue_InContainer(&UTraceSettings::Get());
		return true;
	}

	/** Settings property if it exists and the CVar has not been overridden; otherwise the CVar. */
	static float Resolve(const TCHAR* SettingsName, const TAutoConsoleVariable<float>& CVar, float MinValue, float MaxValue)
	{
		const IConsoleVariable* Console = CVar.AsVariable();
		const uint32 SetBy = (Console != nullptr)
			? (static_cast<uint32>(Console->GetFlags()) & static_cast<uint32>(ECVF_SetByMask))
			: 0u;
		const bool bCVarOverridden = SetBy > static_cast<uint32>(ECVF_SetByConstructor);

		float Value = CVar.GetValueOnAnyThread();
		if (!bCVarOverridden)
		{
			float FromSettings = 0.f;
			if (TrySettingsFloat(SettingsName, FromSettings))
			{
				Value = FromSettings;
			}
		}

		return FMath::Clamp(Value, MinValue, MaxValue);
	}
}

// NO "Trace.ScoringMode" CONSOLE VARIABLE. There was one, and it was wrong: ATraceGameState
// publishes the mode (resolved by ATraceGameMode from "?mode=a|b" on the travel URL) and that is the
// one legal answer. A console override here would have been a second source of truth for the fact
// that decides what this actor IS, and the two would have disagreed on exactly the frame it
// mattered — this file's own log showed the pair fighting, mode A being set and then flipped to B a
// frame later. A run selects its mode with "?mode=b" on the URL, like the menu does.

static TAutoConsoleVariable<float> CVarModeBThrowSpeed(
	TEXT("Trace.ModeB.ThrowSpeed"),
	3000.f,
	TEXT("MODE B. Launch speed of a thrown Core, uu/s. Maps to UTraceSettings::CoreThrowSpeed when that exists."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBThrowUpBias(
	TEXT("Trace.ModeB.ThrowUpBias"),
	0.12f,
	TEXT("MODE B. Upward component added to a throw, as a fraction of throw speed. Gives a flat aim a ")
	TEXT("shallow arc so a throw carries instead of ploughing into the floor. UTraceSettings::CoreThrowUpBias."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBThrowGravityScale(
	TEXT("Trace.ModeB.GravityScale"),
	0.55f,
	TEXT("MODE B. World gravity multiplier applied to a loose Core. Below 1 so a throw crosses useful ")
	TEXT("ground on a 24000uu field. UTraceSettings::CoreThrowGravityScale."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBPickupRadius(
	TEXT("Trace.ModeB.PickupRadius"),
	120.f,
	TEXT("MODE B. 'First contact' radius, uu, measured from the Core to the surface of a player's ")
	TEXT("capsule. UTraceSettings::CorePickupRadius."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBSelfPickupLockout(
	TEXT("Trace.ModeB.SelfPickupLockout"),
	0.35f,
	TEXT("MODE B. Seconds the THROWER alone cannot re-take their own throw. Everyone else may take it ")
	TEXT("on frame one. Without this a throw is a no-op: the Core leaves from inside the thrower's own ")
	TEXT("pickup radius. UTraceSettings::CoreThrowerPickupLockoutSeconds."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBLooseReset(
	TEXT("Trace.ModeB.LooseResetSeconds"),
	12.f,
	TEXT("MODE B. Seconds a loose Core may lie untouched before it is put back into play. The Core may ")
	TEXT("never be lost permanently. UTraceSettings::CoreLooseResetSeconds."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBThrowCooldown(
	TEXT("Trace.ModeB.ThrowCooldown"),
	0.35f,
	TEXT("MODE B. Seconds after taking the Core before it may be thrown again. Stops a pickup and a ")
	TEXT("throw landing on the same frame. UTraceSettings::CoreThrowCooldownSeconds."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBBounce(
	TEXT("Trace.ModeB.Bounce"),
	0.35f,
	TEXT("MODE B. Restitution of a loose Core against world geometry. 0 = dead stop, 1 = perfect bounce."),
	ECVF_Default);

// =================================================================================================
// MODE B ONLY — THE CORE'S WEIGHT  (spec v5 §4: "increase the weight of the core")
//
// The Core has no rigid body and no UProjectileMovementComponent, so there is no `Mass` field to
// turn up. "Weight" here is therefore a DERIVED model rather than a single physics number, and it is
// derived because the note is explicit that this must not be done by slowing the throw down alone:
// "Tune mass/gravity scale, not just speed." A Core that is merely slower reads as a bad throw; a
// Core that is HEAVY has to fall faster, arc more, carry less far and stop dead when it lands.
//
// So one knob, CoreMassScale (M), drives four:
//
//   gravity scale  x M          it accelerates downward faster — the dominant "this thing is dense"
//                               cue, and the one the note names first;
//   throw speed    / sqrt(M)    you cannot hurl a heavy object as fast. Square-root rather than
//                               linear so that doubling the weight does not halve the game;
//   up bias        x M^1.5      the thrower compensates by LOFTING it, which is what turns "shorter"
//                               into "shorter and more arced" instead of "shorter and flatter";
//   bounce         / M          heavy things do not skitter. At the default this is ~0.19, i.e. a
//                               thud, which also makes a missed shot recoverable in front of the
//                               goal rather than rebounding to midfield;
//   rest speed     x M          and it settles sooner, for the same reason.
//
// MEASURED at the default M = 1.8 against the four base knobs as shipped (3000 uu/s, bias 0.12,
// gravity 0.55, bounce 0.35), for a flat throw from eye height:
//
//                      before        after
//   launch speed       3000          2236 uu/s
//   gravity            539           970  uu/s^2
//   flat range         ~5000         ~3400 uu
//   apex above launch  ~120          ~215 uu
//
// EVERY ONE OF THESE IS MODE B ONLY, and not by a mode test bolted on here: TraceModeBTuning is read
// from exactly two places, ThrowFromHolder() and ServerTickLooseCore(), and both refuse to run
// unless IsModeB(). In mode A the Core is a status that never moves under its own power, so there is
// nothing for a weight to apply to. Mode A cannot observe this block at all.
//
// M = 1 restores the pre-v5 flight exactly, which is the A/B the user needs to judge the feel.
// =================================================================================================

static TAutoConsoleVariable<float> CVarModeBCoreMassScale(
	TEXT("Trace.ModeB.CoreMassScale"),
	1.8f,
	TEXT("MODE B. How heavy the Core is in flight, relative to the light Core that shipped before ")
	TEXT("spec v5 (1.0 = exactly the old flight). Scales gravity up, throw speed down, loft up and ")
	TEXT("bounce down together, so the Core reads as heavy rather than merely slow. ")
	TEXT("UTraceSettings::CoreMassScale."),
	ECVF_Default);

namespace TraceModeBTuning
{
	/** Mode B only. The weight multiplier the five accessors below are derived from. See the block above. */
	float MassScale()         { return Resolve(TEXT("CoreMassScale"),                     CVarModeBCoreMassScale,    0.25f, 6.f); }

	/** The four BASE values, before weight. Exposed separately so the settings panel still means what it says. */
	float BaseThrowSpeed()    { return Resolve(TEXT("CoreThrowSpeed"),                    CVarModeBThrowSpeed,       200.f, 20000.f); }
	float BaseThrowUpBias()   { return Resolve(TEXT("CoreThrowUpBias"),                   CVarModeBThrowUpBias,      0.f,   1.5f); }
	float BaseGravityScale()  { return Resolve(TEXT("CoreThrowGravityScale"),             CVarModeBThrowGravityScale, 0.f,  4.f); }
	float BaseBounce()        { return Resolve(TEXT("CoreThrowBounce"),                   CVarModeBBounce,           0.f,   1.f); }

	float ThrowSpeed()        { return FMath::Max(100.f, BaseThrowSpeed() / FMath::Sqrt(MassScale())); }
	float ThrowUpBias()       { return FMath::Clamp(BaseThrowUpBias() * FMath::Pow(MassScale(), 1.5f), 0.f, 2.f); }
	float GravityScale()      { return FMath::Clamp(BaseGravityScale() * MassScale(), 0.f, 8.f); }
	float Bounce()            { return FMath::Clamp(BaseBounce() / MassScale(), 0.f, 1.f); }

	float PickupRadius()      { return Resolve(TEXT("CorePickupRadius"),                  CVarModeBPickupRadius,     20.f,  1500.f); }
	float SelfPickupLockout() { return Resolve(TEXT("CoreThrowerPickupLockoutSeconds"),   CVarModeBSelfPickupLockout, 0.f,  5.f); }
	// Lower bound 0, NOT 1: UTraceSettings::CoreLooseResetSeconds documents 0 as "never reset", and a
	// floor of 1 turned that into the FASTEST reset available. The caller treats <= 0 as "never".
	float LooseResetSeconds() { return Resolve(TEXT("CoreLooseResetSeconds"),             CVarModeBLooseReset,       0.f,   120.f); }
	float ThrowCooldown()     { return Resolve(TEXT("CoreThrowCooldownSeconds"),          CVarModeBThrowCooldown,    0.f,   10.f); }

	/** Radius of the sphere swept for the loose Core's collision. Matches the orb the player sees. */
	constexpr float CollisionRadius = 22.f;

	/** Below this speed the Core is declared at rest, so it stops jittering along the floor. */
	float RestSpeed()         { return 60.f * FMath::Clamp(MassScale(), 0.5f, 4.f); }

	/** How often the goal boxes are re-derived. Cheap, and only has to beat the half-time switch. */
	constexpr float GoalRefreshInterval = 0.5f;

	/** Height above the thrower's eye-line the Core leaves from, so it does not clip their own head. */
	constexpr double ThrowMuzzleForward = 70.0;

	/**
	 * Reports, once per mode-B entry, whether each knob found its UTraceSettings property or fell
	 * back to its console variable.
	 *
	 * This exists because the binding is by NAME at runtime: a misspelled property is not a build
	 * error and not a warning, it is a slider on the settings page that silently does nothing. The
	 * project's rule is that a dead knob is worse than no knob, so the build states its own answer
	 * rather than leaving it to be discovered by a designer wondering why nothing changed.
	 */
	void LogKnobBindings()
	{
		// Paired with the accessors above; every name here must be the one Resolve() is called with.
		static const TCHAR* const KnobNames[] =
		{
			TEXT("CoreMassScale"),
			TEXT("CoreThrowSpeed"),
			TEXT("CoreThrowUpBias"),
			TEXT("CoreThrowGravityScale"),
			TEXT("CorePickupRadius"),
			TEXT("CoreThrowerPickupLockoutSeconds"),
			TEXT("CoreLooseResetSeconds"),
			TEXT("CoreThrowCooldownSeconds"),
			TEXT("CoreThrowBounce"),
		};

		int32 BoundCount = 0;
		FString DeadNames;

		for (const TCHAR* Name : KnobNames)
		{
			if (FindFProperty<FFloatProperty>(UTraceSettings::StaticClass(), FName(Name)) != nullptr)
			{
				++BoundCount;
			}
			else
			{
				if (!DeadNames.IsEmpty())
				{
					DeadNames += TEXT(", ");
				}
				DeadNames += Name;
			}
		}

		const int32 TotalCount = static_cast<int32>(UE_ARRAY_COUNT(KnobNames));

		if (DeadNames.IsEmpty())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeB] tuning: all %d knobs are bound to UTraceSettings properties and are live on the settings panel."),
				TotalCount);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeB] tuning: %d of %d knobs bound; NO UTraceSettings PROPERTY FOUND FOR: %s. ")
				TEXT("Those fall back to their Trace.ModeB.* console variables, so the settings panel ")
				TEXT("shows nothing for them - declare a float of that exact name on UTraceSettings."),
				BoundCount, TotalCount, *DeadNames);
		}
	}

	/**
	 * Says out loud what the weight model actually resolved to, in the units a designer thinks in.
	 *
	 * The point is the last column: a mass scale is an abstraction, and "1.8" tells nobody whether a
	 * throw still crosses useful ground. The flat range and apex are computed from the same numbers
	 * the Core integrates with, for a throw from eye height at a flat aim, so a tuning pass can be
	 * judged against the pitch (33600 uu long) without a stopwatch.
	 */
	void LogFlightModel(const UWorld* World)
	{
		const float M = MassScale();
		const float Speed = ThrowSpeed();
		const float Bias = ThrowUpBias();
		const float G = FMath::Abs((World != nullptr) ? World->GetGravityZ() : -980.f) * GravityScale();

		// Flat aim from ~150 uu of eye height: up-phase, apex, then the fall back to the floor.
		const float Vz = Speed * Bias;
		const float LaunchZ = 150.f;
		const float Apex = (G > 1.f) ? (Vz * Vz) / (2.f * G) : 0.f;
		const float Flight = (G > 1.f) ? (Vz / G) + FMath::Sqrt(2.f * (LaunchZ + Apex) / G) : 0.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] core weight: mass x%.2f -> speed %.0f uu/s (base %.0f), gravity %.0f uu/s2 ")
			TEXT("(scale %.2f), loft %.3f, bounce %.2f | flat throw: apex +%.0f uu, range ~%.0f uu"),
			M, Speed, BaseThrowSpeed(), G, GravityScale(), Bias, Bounce(), Apex, Speed * Flight);
	}
}

/**
 * Registered goal volumes.
 *
 * Static and world-filtered rather than a member, because the goal actor's BeginPlay can run before
 * the GameMode has spawned the Core — a registration that had to find the Core first would be
 * dropped exactly once per map load, at the only moment it matters. Entries are weak; dead ones are
 * pruned on every refresh, so an actor that forgets to unregister leaks nothing.
 */
namespace TraceGoalRegistry
{
	struct FEntry
	{
		TWeakObjectPtr<AActor> GoalOwner;
		TWeakObjectPtr<UPrimitiveComponent> Volume;
		ETraceTeam DefendingTeam = ETraceTeam::None;
	};

	static TArray<FEntry>& Entries()
	{
		static TArray<FEntry> Registry;
		return Registry;
	}
}


// =================================================================================================
// Construction
// =================================================================================================

ATraceCore::ATraceCore()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;

	// The Core is a status: everybody has to know who holds it, from anywhere on a 24000uu field,
	// or the beacon it drives is pointless.
	bAlwaysRelevant = true;

	// Movement replication is off for good. Attached => attachment replicates the transform;
	// holderless => every machine computes the home location identically.
	SetReplicateMovement(false);
	SetCanBeDamaged(false);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);

	// NO COLLISION ANYWHERE ON THIS ACTOR. There is nothing to run into, nothing to catch, and
	// nothing that may ever eat a bullet meant for a player.
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCollisionProfileName(TEXT("NoCollision"));
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetCastShadow(false);
	Mesh->bReceivesDecals = false;
	Mesh->SetRelativeScale3D(FVector(TraceCoreTuning::OrbScale));

	// HIDDEN FROM THE HOLDER'S OWN CAMERA, AND ONLY FROM THEIRS.
	//
	// This marker exists so that everyone ELSE can find the holder; the holder already knows, from
	// the HUD banner, the shield indicator and the third-person pull-back. Left visible to them it
	// is a bright emissive object suspended a few hundred uu in front of their own lens - the same
	// class of defect as the arena trim whiteout, and captured frames confirmed it: the orb filled
	// the centre of the holder's screen.
	//
	// SetOwnerNoSee resolves through the ACTOR OWNER CHAIN, and GrantTo() SetOwner()s this actor to
	// its holder, so "the owner" is exactly the one player who should not see it. Every other client
	// draws the full beacon. ApplyAttachment() re-dirties the render state whenever the holder
	// changes, because the proxy caches that chain when it is built.
	Mesh->SetOwnerNoSee(true);

	Beacon = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Beacon"));
	Beacon->SetupAttachment(Root);
	Beacon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Beacon->SetCollisionProfileName(TEXT("NoCollision"));
	Beacon->SetGenerateOverlapEvents(false);
	Beacon->SetCanEverAffectNavigation(false);
	Beacon->SetCastShadow(false);
	Beacon->bReceivesDecals = false;
	Beacon->SetOwnerNoSee(true);   // Same reason as the orb above.

	// Constructor-time FObjectFinders are what make these engine assets cook into a packaged build;
	// a bare runtime LoadObject would resolve to null once cooked.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMeshFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMeshFinder.Succeeded())
	{
		Mesh->SetStaticMesh(SphereMeshFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMeshFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMeshFinder.Succeeded())
	{
		Beacon->SetStaticMesh(CylinderMeshFinder.Object);
	}

	// Same material policy as the trail and the arena: the generated unlit neon material if the
	// content script has been run, the lit engine basic material otherwise. No .uasset we author by
	// hand is ever a hard requirement.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		BaseMaterial = NeonFinder.Object;
		bMaterialIsNeon = true;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BaseMaterial == nullptr && BasicFinder.Succeeded())
	{
		BaseMaterial = BasicFinder.Object;
		bMaterialIsNeon = false;
	}
}

void ATraceCore::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceCore, Carrier);
	DOREPLIFETIME(ATraceCore, bPassActive);
	DOREPLIFETIME(ATraceCore, PassTarget);
	DOREPLIFETIME(ATraceCore, PassStartServerTime);
	DOREPLIFETIME(ATraceCore, PassCooldownEndServerTime);

	// Mode B. Three properties that never change value at all in mode A, so mode A pays one bitfield
	// comparison per net update for them and nothing else. (The MODE itself is not replicated here:
	// ATraceGameState owns and replicates it.)
	DOREPLIFETIME(ATraceCore, bLoose);
	DOREPLIFETIME(ATraceCore, LooseLocation);
	DOREPLIFETIME(ATraceCore, LooseVelocity);
}

void ATraceCore::BeginPlay()
{
	Super::BeginPlay();

	SpawnHomeLocation = GetActorLocation();

	// Shader work is pointless (and unreliable - shaders are not cooked for server targets) on a
	// dedicated server.
	if (BaseMaterial != nullptr && GetNetMode() != NM_DedicatedServer)
	{
		if (Mesh != nullptr)
		{
			MeshMID = Mesh->CreateDynamicMaterialInstance(0, BaseMaterial);
		}
		if (Beacon != nullptr)
		{
			BeaconMID = Beacon->CreateDynamicMaterialInstance(0, BaseMaterial);
		}
	}

	if (HasAuthority())
	{
		// Nobody has it yet and nobody is owed it. The Tick backstop grants it to the default team
		// if the GameMode has not called KickoffTo() within a few seconds.
		PendingGrantTime = GetServerTimeSeconds() + TraceCoreTuning::MaxHolderlessSeconds;

		// Nothing to latch: the mode is read from ATraceGameState at the point of use. This only
		// records what it currently is, so OnScoringModeChanged fires on the first real change.
		bAppliedModeB = IsModeB();
		bModeEverApplied = true;

		UE_LOG(LogTraceGame, Display, TEXT("[Core] starting in scoring mode %s"),
			bAppliedModeB ? TEXT("B - goals, Core is thrown and intercepted")
						  : TEXT("A - endzones, Core is a status"));
	}

	ApplyAttachment();
	UpdateVisuals();
}

void ATraceCore::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ATraceCharacter* Bound = BoundDeathHolder.Get())
	{
		if (Bound->Health != nullptr)
		{
			Bound->Health->OnDeath.RemoveDynamic(this, &ATraceCore::OnHolderDeath);
		}
	}
	BoundDeathHolder = nullptr;

	Super::EndPlay(EndPlayReason);
}


// =================================================================================================
// Tick
// =================================================================================================

void ATraceCore::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Carrier / State / bPassActive are independent properties and can land in any order, so every
	// machine reconciles from Tick as well as from the OnReps.
	if (!bAppliedEver || AppliedHolder.Get() != Carrier || bAppliedPassActive != bPassActive
		|| bAppliedLoose != bLoose)
	{
		ApplyAttachment();
		UpdateVisuals();
	}

	if (!HasAuthority())
	{
		// MODE B, CLIENTS. Dead-reckon the loose Core between net updates so it flies along its arc
		// instead of stepping along it at the net update rate. Purely presentational: LooseLocation
		// is overwritten by the next replicated value, and no client ever decides a pickup or a goal.
		if (bLoose)
		{
			LooseLocation = LooseLocation + LooseVelocity * DeltaSeconds;
			SetActorLocation(LooseLocation, false, nullptr, ETeleportType::TeleportPhysics);
		}
		return;
	}

	const float Now = GetServerTimeSeconds();

	// ---- 0. Mode B: a Core that is out in the world is its own state, and it OWNS this tick. ----
	//
	// Placed ahead of everything below deliberately. Steps 1-3 all assume that "Carrier == nullptr"
	// means the Core is waiting to be granted, and a loose Core is the one case where that is false:
	// without this early-out, step 3 would grant a thrown Core to the nearest player of whichever
	// team was owed it and park the actor back at the centre circle, mid-flight.
	//
	// Also notices a mode change, which is why it runs before the loose test rather than inside it:
	// switching to mode A while the Core is in the air has to normalise that state, not be skipped
	// by it. One bool compare per tick in the steady state.
	{
		const bool bNowModeB = IsModeB();
		if (!bModeEverApplied || bAppliedModeB != bNowModeB)
		{
			bAppliedModeB = bNowModeB;
			bModeEverApplied = true;
			OnScoringModeChanged();
		}
	}

	// Diagnostics, and deliberately ahead of the loose branch's early return so the scenario can
	// observe a Core that is currently in the air. Costs one int compare when it is not armed.
	TickModeBVerification();

	if (bLoose)
	{
		ServerTickLooseCore(DeltaSeconds);
		return;
	}

	// ---- 1. A queued fallback from DropAt(). --------------------------------------------------
	//
	// Deferred by exactly one tick on purpose. ATraceGameMode::NotifyCharacterDied() calls DropAt()
	// from inside the health component's OnDeath broadcast, BEFORE our own OnHolderDeath() listener
	// on the same broadcast has run - and ours is the one that knows who the killer was. Resolving
	// DropAt immediately would hand the Core to the nearest enemy and then immediately re-hand it to
	// the killer, which is two transfers, two grace periods and two packets for one death.
	if (bFallbackQueued)
	{
		bFallbackQueued = false;
		if (Carrier == nullptr)
		{
			ResolveFallback(FallbackTeam);
		}
	}

	// ---- 2. Holder sanity. The Core may never ride a corpse or a destroyed pawn. ---------------
	if (Carrier != nullptr && (!IsValid(Carrier) || !Carrier->IsAlive()))
	{
		const ETraceTeam LostTeam = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::None;
		UE_LOG(LogTraceGame, Verbose, TEXT("Core: holder became invalid; applying fallback."));
		ReleaseHolder();
		ResolveFallback(LostTeam);
	}

	// ---- 3. Holderless: resolve whoever is owed it, or fall back to the default team. ----------
	if (Carrier == nullptr)
	{
		// Deliberately unowned (half-time interval, post-match). Do nothing — except keep the
		// last-ditch recovery below alive, because "out of play" during an IN PROGRESS half would
		// be a dead match, and this class's one hard promise is that the Core never goes missing.
		if (bOutOfPlay)
		{
			// "LIVE" MUST EXCLUDE HALF TIME, and the obvious test does not.
			// ATraceGameState.h states it outright: TraceMatchState stays InProgress THROUGH the
			// interval on purpose. So `== InProgress` alone is true during half time, and this
			// recovery would fire 15 s into a 12 s break — granting the Core to Blue, starting a
			// trace and teleporting pawns in the middle of the side switch. Four seconds of margin,
			// on a break length that is `config` AND settable per-run via ?breaklen= and
			// -TraceHalfTimeSeconds=. Any longer break trips it.
			//
			// IsHalfTimeBreak() is the question this code was always asking; the constant's own doc
			// comment says "while a half is actually RUNNING", which is not what the enum means.
			bool bMatchLive = false;
			if (const ATraceGameState* GameState = GetWorld() ? GetWorld()->GetGameState<ATraceGameState>() : nullptr)
			{
				bMatchLive = (GameState->TraceMatchState == ETraceMatchState::InProgress)
					&& !GameState->IsHalfTimeBreak();
			}

			if (bMatchLive && (Now - PendingGrantTime) >= TraceCoreTuning::OutOfPlayRecoverySeconds)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("Core: out of play for %.0fs while the match is running - forcing a kickoff so play can continue."),
					TraceCoreTuning::OutOfPlayRecoverySeconds);
				bOutOfPlay = false;
				PendingGrantTeam = TraceCoreTuning::DefaultKickoffTeam;
			}
		}
		else if (Now >= PendingGrantTime)
		{
			if (PendingGrantTeam == ETraceTeam::None)
			{
				PendingGrantTeam = TraceCoreTuning::DefaultKickoffTeam;
			}
			if (!TryResolvePendingGrant())
			{
				// Nobody alive on that side yet (everyone is on a respawn timer). Try again shortly;
				// the Core waits rather than vanishing.
				PendingGrantTime = Now + 0.25f;
			}
		}

		// Park it at home so a holderless Core is somewhere sensible rather than on a corpse.
		if (FVector::DistSquared(GetActorLocation(), GetHomeLocation()) > TraceCoreTuning::HomeToleranceSq)
		{
			SetActorLocation(GetHomeLocation(), false, nullptr, ETeleportType::TeleportPhysics);
		}
		return;
	}

	// ---- 4. Held: run the pass state machine and keep the trace alive. -------------------------
	ServerTickPass(DeltaSeconds);
	EnforceHolderTrailState();
	SamplePassAvailabilityStats();

	// ---- 5. Mode B only: "a player carries the core into the goal". -----------------------------
	//
	// Swept from where the holder was last frame, for the same reason the thrown test is swept: a
	// carrier at 800 uu/s with a dash on top can cross a goal mouth inside one long frame.
	//
	// The goal volume polls for a stationary carrier itself, at 10 Hz. This is not a duplicate of
	// that: the poll samples POSITIONS and this samples the PATH BETWEEN THEM, so a carrier who
	// crosses the mouth and comes out the far side between two polls scores here and nowhere else.
	// ATraceGameMode::NotifyScored debounces the overlap, so a carry-in slow enough for both to see
	// still counts once.
	if (IsModeB() && IsValid(Carrier))
	{
		const FVector CarrierNow = Carrier->GetActorLocation();
		const FVector CarrierFrom = LastCarrierGoalTestLocation.IsZero() ? CarrierNow : LastCarrierGoalTestLocation;
		LastCarrierGoalTestLocation = CarrierNow;

		CheckGoalScore(CarrierFrom, CarrierNow, Carrier->GetTeam(), EGoalMethod::Carried, TEXT("carried in"));
	}
	else
	{
		LastCarrierGoalTestLocation = FVector::ZeroVector;
	}
}

void ATraceCore::SamplePassAvailabilityStats()
{
	if (!HasAuthority() || CVarTracePassStats.GetValueOnAnyThread() == 0 || !IsValid(Carrier) || !Carrier->IsAlive())
	{
		return;
	}

	// 10 Hz. The point is a distribution over a match, not a per-frame trace storm - and each sample
	// runs a handful of line traces per teammate.
	const float Now = GetServerTimeSeconds();
	if ((Now - TracePassStats::GStats.LastSampleTime) < 0.1f)
	{
		return;
	}
	TracePassStats::GStats.LastSampleTime = Now;

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	bool bAnyLegal = false;
	for (const ATraceCharacter* Candidate : Candidates)
	{
		if (Candidate == Carrier || !IsValid(Candidate))
		{
			continue;
		}
		// Only count TEAMMATES: an enemy failing "not an ally" says nothing about whether the pass
		// option should have shown up, and there are five of them per sample drowning the histogram.
		if (!AreAllies(Carrier->GetTeam(), Candidate->GetTeam()))
		{
			continue;
		}

		++TracePassStats::GStats.TeammatesConsidered;

		ETracePassRejectReason Code = ETracePassRejectReason::None;
		if (IsLegalPassTarget(Carrier, Candidate, /*bRequireAim=*/true, /*OutRejectReason=*/nullptr, &Code))
		{
			++TracePassStats::GStats.TeammatesLegal;
			bAnyLegal = true;
		}
		else
		{
			const int32 Index = FMath::Clamp(static_cast<int32>(Code), 0, 7);
			++TracePassStats::GStats.RefusalCount[Index];
		}
	}

	++TracePassStats::GStats.CarrySamples;
	if (bAnyLegal)
	{
		++TracePassStats::GStats.RawSamplesWithTarget;
	}

	// THE NUMBER THE BUG REPORT IS ABOUT. FindPassTargetFor() is what ATraceHUD polls to decide
	// whether to show the pass affordance and what the bots ask before committing a pass, so this -
	// not the raw rule above - is "did the pass option show up".
	if (FindPassTargetFor(Carrier) != nullptr)
	{
		++TracePassStats::GStats.SamplesWithTarget;
	}
	else
	{
		++TracePassStats::GStats.SamplesWithoutTarget;
	}

	const float Interval = CVarTracePassStatsInterval.GetValueOnAnyThread();
	if (Interval > 0.f)
	{
		if (TracePassStats::GStats.LastDumpTime <= 0.0)
		{
			TracePassStats::GStats.LastDumpTime = Now;
		}
		else if ((Now - TracePassStats::GStats.LastDumpTime) >= Interval)
		{
			TracePassStats::GStats.LastDumpTime = Now;
			TracePassStats::Dump();
		}
	}
}


// =================================================================================================
// Queries
// =================================================================================================

ATraceCharacter* ATraceCore::GetCarrier() const
{
	return Carrier;
}

bool ATraceCore::IsHeld() const
{
	return IsValid(Carrier);
}

ETraceTeam ATraceCore::GetHolderTeam() const
{
	return IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::None;
}

FVector ATraceCore::GetHomeLocation() const
{
	// Resolved lazily: the GameMode may spawn the arena builder and the Core in either order.
	if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(GetWorld()))
	{
		return Arena->GetCoreSpawnLocation();
	}
	return SpawnHomeLocation;
}

// IsPickupLockedOutFor() IS DELETED. Zero callers, and it returned false unconditionally: there is
// no loose Core and nothing is ever picked up, so there was nothing to be locked out of.

float ATraceCore::GetServerTimeSeconds() const
{
	if (const UWorld* World = GetWorld())
	{
		// Already replicated and smoothed. Returns double on 5.3+, so narrow explicitly.
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return static_cast<float>(GameState->GetServerWorldTimeSeconds());
		}
		return static_cast<float>(World->GetTimeSeconds());
	}
	return 0.f;
}

ATraceCore* ATraceCore::Get(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	if (const ATraceGameState* GameState = World->GetGameState<ATraceGameState>())
	{
		return GameState->Core;
	}
	return nullptr;
}

bool ATraceCore::IsShieldSuppressedFor(const AActor* Character)
{
	if (Character == nullptr)
	{
		return false;
	}

	const ATraceCore* Core = ATraceCore::Get(Character->GetWorld());

	// The whole risk beat, in one line: the shield is down for the holder, and only the holder, and
	// only for as long as their pass is being held.
	return Core != nullptr && Core->bPassActive && Core->Carrier == Character;
}

bool ATraceCore::IsCoreHolder(const AActor* Character)
{
	if (Character == nullptr)
	{
		return false;
	}

	const ATraceCore* Core = ATraceCore::Get(Character->GetWorld());
	return Core != nullptr && Core->Carrier == Character;
}

bool ATraceCore::IsTraceInvulnerableFor(const AActor* Character)
{
	// Identical condition to IsShieldSuppressedFor by design: §4 says the two flip "simultaneously",
	// so they are literally the same fact read twice rather than two pieces of state to keep in sync.
	return IsShieldSuppressedFor(Character);
}

bool ATraceCore::IsPassActive() const
{
	return bPassActive || bLocalPassPredicted;
}

ATraceCharacter* ATraceCore::GetEffectivePassTarget() const
{
	if (bPassActive)
	{
		return PassTarget;
	}
	return bLocalPassPredicted ? LocalPassPredictTarget.Get() : nullptr;
}

float ATraceCore::GetPassProgress() const
{
	const float Hold = FMath::Max(0.01f, UTraceSettings::Get().PassHoldSeconds);
	const float Now = GetServerTimeSeconds();

	// Server state wins the moment it exists; prediction only covers the round trip before it does.
	if (bPassActive)
	{
		return FMath::Clamp((Now - PassStartServerTime) / Hold, 0.f, 1.f);
	}
	if (bLocalPassPredicted)
	{
		return FMath::Clamp((Now - LocalPassPredictStartTime) / Hold, 0.f, 1.f);
	}
	return 0.f;
}

float ATraceCore::GetPassCooldownRemaining() const
{
	return FMath::Max(0.f, PassCooldownEndServerTime - GetServerTimeSeconds());
}


// =================================================================================================
// Pass: target acquisition
// =================================================================================================

void ATraceCore::GatherCharacters(TArray<ATraceCharacter*>& OutCharacters) const
{
	OutCharacters.Reset();

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (const ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
	{
		for (const TWeakObjectPtr<ATraceCharacter>& Weak : GameMode->GetTrackedCharacters())
		{
			if (ATraceCharacter* Character = Weak.Get())
			{
				OutCharacters.Add(Character);
			}
		}

		if (OutCharacters.Num() > 0)
		{
			return;
		}
	}

	// The GameMode list is the fast path and is authority-only; clients (which run this for local
	// pass prediction) always land here.
	for (TActorIterator<ATraceCharacter> It(World); It; ++It)
	{
		if (ATraceCharacter* Character = *It)
		{
			OutCharacters.Add(Character);
		}
	}
}

const TCHAR* TracePassStats::RefusalName(int32 Index)
{
	switch (static_cast<ETracePassRejectReason>(Index))
	{
	case ETracePassRejectReason::InvalidOrSelf:     return TEXT("invalid or self");
	case ETracePassRejectReason::Dead:              return TEXT("dead");
	case ETracePassRejectReason::NotAnAlly:         return TEXT("not an ally");
	case ETracePassRejectReason::OutOfRange:        return TEXT("out of range");
	case ETracePassRejectReason::NoLineOfSight:     return TEXT("no line of sight");
	case ETracePassRejectReason::Behind:            return TEXT("behind us");
	case ETracePassRejectReason::NotUnderCrosshair: return TEXT("not under crosshair");
	default:                                        return TEXT("legal");
	}
}

bool ATraceCore::IsLegalPassTarget(const ATraceCharacter* Holder, const ATraceCharacter* Candidate, bool bRequireAim,
	const TCHAR** OutRejectReason, ETracePassRejectReason* OutRejectCode) const
{
	// Every `return false` below names itself. Costs nothing when the caller does not ask (the
	// gameplay path passes null), and turns "no receiver found" — which is what a carrier who cannot
	// get rid of the Core actually experiences — into a specific broken test.
	//
	// The CODE is what the grace window reads: the state machine has to know whether the refusal was
	// a transient geometric blink (ride it out) or a fact about the receiver (cancel now). The string
	// stays for Trace.DebugPassTargets, derived from the code so the two can never disagree.
	const auto Reject = [OutRejectReason, OutRejectCode](ETracePassRejectReason Code) -> bool
	{
		if (OutRejectCode != nullptr)
		{
			*OutRejectCode = Code;
		}
		if (OutRejectReason != nullptr)
		{
			*OutRejectReason = TracePassStats::RefusalName(static_cast<int32>(Code));
		}
		return false;
	};

	if (OutRejectReason != nullptr)
	{
		*OutRejectReason = TEXT("legal");
	}
	if (OutRejectCode != nullptr)
	{
		*OutRejectCode = ETracePassRejectReason::None;
	}

	if (!IsValid(Holder) || !IsValid(Candidate) || Holder == Candidate)
	{
		return Reject(ETracePassRejectReason::InvalidOrSelf);
	}

	// §9.3 [ASSUMPTION]: you cannot pass to a dead or respawning teammate.
	if (!Candidate->IsAlive())
	{
		return Reject(ETracePassRejectReason::Dead);
	}

	if (!AreAllies(Holder->GetTeam(), Candidate->GetTeam()))
	{
		return Reject(ETracePassRejectReason::NotAnAlly);
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	const FVector ViewLocation = Holder->GetPawnViewLocation();
	const FVector TargetChest = Candidate->GetActorLocation()
		+ FVector(0.0, 0.0, static_cast<double>(Settings.PassTargetChestOffsetZ));

	if (FVector::DistSquared(ViewLocation, TargetChest) > FMath::Square(static_cast<double>(Settings.PassMaxRange)))
	{
		return Reject(ETracePassRejectReason::OutOfRange);
	}

	// Line of sight, against WORLD GEOMETRY ONLY - and "world geometry" means ECC_Visibility, not an
	// object-type query.
	//
	// THIS USED TO BE LineTraceTestByObjectType({WorldStatic, WorldDynamic}), AND THAT SILENTLY KILLED
	// EVERY PASS INVOLVING AN ENDZONE. An object-type query matches on the component's object type and
	// ignores its response channels entirely, so the ATraceEndzone trigger - a QueryOnly box whose
	// object type is ECC_WorldDynamic and which responds to nothing but ECC_Pawn - was a solid wall to
	// this one query and nothing else in the game. A carrier standing in a zone hit it immediately
	// (bFindInitialOverlaps defaults true), and a pass INTO a zone hit it on entry, so "complete a pass
	// to a teammate standing in the enemy endzone" could never fire. Widening the endzones to the full
	// field width made the dead area twice as large.
	//
	// ECC_Visibility is what the original comment actually wanted: arena geometry blocks it, the
	// endzone trigger ignores it by construction (TraceEndzone.cpp sets every channel to Ignore, then
	// opens ECC_Pawn alone), and the "Pawn" capsule profile ignores it too - so a teammate standing in
	// between still cannot invalidate the pass, which was the whole reason the object query was chosen.
	//
	// §4.1 FIX - PROBE MORE THAN ONE POINT. A single chest ray is a coin flip against the new cover
	// density: a receiver whose head and shoulders are plainly visible over a 1x (176 uu) block still
	// fails, because the one point being probed is the one point behind it. Any clear probe now means
	// line of sight, which is both more generous and much closer to what the passer can actually see.
	// The chest is probed first so the common case is still exactly one trace.
	const UWorld* World = GetWorld();
	if (World != nullptr)
	{
		FCollisionQueryParams QueryParams(FName(TEXT("TracePassLOS")), /*bTraceComplex=*/false);
		QueryParams.AddIgnoredActor(this);
		QueryParams.AddIgnoredActor(Holder);
		QueryParams.AddIgnoredActor(Candidate);

		const FVector CandidateLocation = Candidate->GetActorLocation();
		const int32 ProbeCount = TraceCoreTuning::ResolvedMultiPointLos(Settings)
			? static_cast<int32>(UE_ARRAY_COUNT(TraceCoreTuning::LosProbeOffsetsZ))
			: 1;

		bool bHaveLineOfSight = false;
		for (int32 ProbeIndex = 0; ProbeIndex < ProbeCount && !bHaveLineOfSight; ++ProbeIndex)
		{
			// Probe 0 is the chest and must be the SAME point the aim test and the range test used,
			// or the pass can be aimed at a spot line of sight was never checked against.
			const double ProbeZ = (ProbeIndex == 0)
				? static_cast<double>(Settings.PassTargetChestOffsetZ)
				: TraceCoreTuning::LosProbeOffsetsZ[ProbeIndex];
			const FVector ProbePoint = CandidateLocation + FVector(0.0, 0.0, ProbeZ);
			bHaveLineOfSight = !World->LineTraceTestByChannel(ViewLocation, ProbePoint, ECC_Visibility, QueryParams);
		}

		if (!bHaveLineOfSight)
		{
			return Reject(ETracePassRejectReason::NoLineOfSight);
		}
	}

	// --- "hover the crosshair over them", generously (see TraceCoreTuning) ----------------------

	if (!bRequireAim)
	{
		return true;
	}

	const FVector AimDirection = Holder->GetAimDirection();
	FVector ToTarget = TargetChest - ViewLocation;
	const double Distance = ToTarget.Size();
	if (Distance <= CoreGeometryEpsilon)
	{
		return true;   // Standing inside each other. Nothing sensible to measure; accept.
	}
	ToTarget /= Distance;

	const double Cosine = FVector::DotProduct(ToTarget, AimDirection);
	if (Cosine <= 0.0)
	{
		return Reject(ETracePassRejectReason::Behind);
	}

	// (a) angular cone - what makes a distant receiver acquirable at all.
	const double ConeDegrees = TraceCoreTuning::ResolvedAimConeDegrees(Settings);
	if (Cosine >= FMath::Cos(FMath::DegreesToRadians(ConeDegrees)))
	{
		return true;
	}

	// (b) the aim ray passing through the receiver's capsule - what makes a near receiver
	//     acquirable anywhere on their body, where the cone above has collapsed to nothing.
	double CapsuleRadius = 34.0;
	double CapsuleHalfHeight = 88.0;
	if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
	{
		CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}

	const FVector RayEnd = ViewLocation + AimDirection * (Distance + CapsuleHalfHeight);
	const FVector CapsuleBottom = Candidate->GetActorLocation() - FVector(0.0, 0.0, CapsuleHalfHeight);
	const FVector CapsuleTop = Candidate->GetActorLocation() + FVector(0.0, 0.0, CapsuleHalfHeight);

	FVector ClosestOnRay = FVector::ZeroVector;
	FVector ClosestOnCapsule = FVector::ZeroVector;
	FMath::SegmentDistToSegmentSafe(ViewLocation, RayEnd, CapsuleBottom, CapsuleTop, ClosestOnRay, ClosestOnCapsule);

	const double Threshold = CapsuleRadius + TraceCoreTuning::ResolvedAimSlack(Settings);
	if (FVector::DistSquared(ClosestOnRay, ClosestOnCapsule) > (Threshold * Threshold))
	{
		return Reject(ETracePassRejectReason::NotUnderCrosshair);
	}

	return true;
}

ATraceCharacter* ATraceCore::FindPassTargetFor(const ATraceCharacter* Holder) const
{
	if (!IsValid(Holder) || !Holder->IsAlive())
	{
		return nullptr;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	const FVector ViewLocation = Holder->GetPawnViewLocation();
	const FVector AimDirection = Holder->GetAimDirection();

	ATraceCharacter* Best = nullptr;
	double BestCosine = -1.0;

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (!IsLegalPassTarget(Holder, Candidate))
		{
			continue;
		}

		// Among everyone who qualifies, take whoever is nearest the crosshair. With two teammates
		// overlapping on screen this is the one the player is obviously pointing at.
		const FVector TargetChest = Candidate->GetActorLocation()
			+ FVector(0.0, 0.0, static_cast<double>(Settings.PassTargetChestOffsetZ));
		const FVector ToTarget = (TargetChest - ViewLocation).GetSafeNormal();
		const double Cosine = FVector::DotProduct(ToTarget, AimDirection);

		if (Cosine > BestCosine)
		{
			BestCosine = Cosine;
			Best = Candidate;
		}
	}

	// --- §4.1 FIX: sticky acquisition ------------------------------------------------------------
	//
	// Without this the answer above flickers, and the flicker IS the reported bug. ATraceHUD polls
	// this at 20 Hz purely to decide whether to draw the receiver highlight, so a teammate who is
	// jogging past a lane rail makes the pass option itself blink in and out - "sometimes the pass
	// option doesn't show up". It also costs real passes: the button is only sampled when it is
	// pressed, so a press that lands on a flickered-out frame acquires nobody at all.
	//
	// Identity, team, life and range are re-tested every single time (bRequireAim false skips only
	// the aim cone, and the LOS probe inside is what the window is forgiving). So a receiver who dies
	// or is genuinely miles away is dropped immediately; only the two flickery geometric tests get
	// held over, and only for Trace.Pass.StickySeconds.
	const float StickyWindow = TraceCoreTuning::ResolvedAcquireSticky(Settings);
	const float Now = GetServerTimeSeconds();

	if (Best != nullptr)
	{
		StickyAcquireHolder = const_cast<ATraceCharacter*>(Holder);
		StickyAcquireTarget = Best;
		StickyAcquireServerTime = Now;
		return Best;
	}

	if (StickyWindow > 0.f
		&& StickyAcquireHolder.Get() == Holder
		&& (Now - StickyAcquireServerTime) <= StickyWindow)
	{
		if (ATraceCharacter* Held = StickyAcquireTarget.Get())
		{
			// Everything except the aim cone and line of sight, re-checked for real.
			if (IsValid(Held) && Held != Holder && Held->IsAlive()
				&& AreAllies(Holder->GetTeam(), Held->GetTeam())
				&& FVector::DistSquared(Holder->GetPawnViewLocation(),
					Held->GetActorLocation() + FVector(0.0, 0.0, static_cast<double>(Settings.PassTargetChestOffsetZ)))
					<= FMath::Square(static_cast<double>(Settings.PassMaxRange)))
			{
				if (HasAuthority() && CVarTracePassStats.GetValueOnAnyThread() != 0)
				{
					++TracePassStats::GStats.StickySaves;
				}
				return Held;
			}
		}
	}

	return nullptr;
}


// =================================================================================================
// Pass: input and state machine
// =================================================================================================

void ATraceCore::RequestPassInput(bool bPressed, ATraceCharacter* Requester)
{
	if (!IsValid(Requester))
	{
		return;
	}

	// bPassInputHeld is ONE latch shared by the whole match, so "safe to call on a non-holder" —
	// which this function used to claim — was wrong in both directions. Who is asking decides
	// whether the latch may move.
	if (bPressed)
	{
		// Only the living holder may arm it.
		//
		// IsValid(Carrier) FIRST, explicitly. The old form (`Requester != Carrier || !Carrier->...`)
		// was correct only because || short-circuits when Carrier is null and Requester is validated
		// non-null above — i.e. it was one clause reorder away from being a client crash. Stating the
		// null check is free and removes the trap.
		if (!IsValid(Carrier) || Requester != Carrier || !Carrier->IsAlive())
		{
			return;
		}
		PassInputInstigator = Requester;
	}
	else
	{
		// A RELEASE is honoured from the holder OR from whoever armed it, and the second case is the
		// one that matters: a completed pass moves the Core to the receiver BEFORE the player's
		// finger leaves the button, so the passer is no longer the holder when their release lands.
		// (Tracked per machine; nothing here is replicated.)
		if (Requester != Carrier && Requester != PassInputInstigator.Get())
		{
			return;
		}
		PassInputInstigator = nullptr;
	}

	// No "is anyone holding the Core" check here any more. A PRESS already implies one — it is only
	// reached when Requester is the living Carrier. A RELEASE must go through even when nobody is
	// holding the Core (one arriving during a kickoff window), because a release that is dropped is
	// exactly what leaves the latch set.

	if (HasAuthority())
	{
		// ---- THE BINDING'S MEANING IS MODE-DEPENDENT (spec v4 §7). -------------------------------
		//
		// Verbatim: "The carrier should be able to throw the core forward by left clicking." In mode
		// A the same button starts the 0.5 s hover-pass. The branch lives HERE, at the one door every
		// pass input comes through, rather than in the pawn or the bots: ATraceCharacter::
		// DoPassPressed, ATracePlayerController and the bots' ApplyPassInput all call this function
		// and none of them has to learn that a second mode exists.
		//
		// A throw is instantaneous, so there is nothing to hold and nothing to release: the press
		// throws, the release is swallowed, and bPassInputHeld is never armed in mode B - which
		// matters, because an armed latch would make ServerTickPass open a pass window (shield down)
		// on a Core that is no longer held.
		if (IsModeB())
		{
			if (bPressed)
			{
				ThrowFromHolder(Requester);
			}
			return;
		}

		bPassInputHeld = bPressed;
		// Resolve on the same frame the button was pressed rather than waiting a tick: the pass
		// window is the moment the shield drops, and a frame of latency on that is a frame of free
		// invulnerability.
		ServerTickPass(0.f);
		return;
	}

	// --- Owning client: predict, then tell the server. -----------------------------------------
	//
	// The prediction is presentation only (the HUD ring, the receiver highlight). It deliberately
	// does NOT predict the shield drop or the trace hardening: those are damage rules, they are
	// resolved on the server, and a client that mispredicted them would be showing itself a
	// safety it does not have.
	// Requester, not Carrier: the release half of this runs after a completed pass has already moved
	// Carrier to the receiver, and it is OUR prediction that has to be unwound, not theirs.
	// Mode B predicts nothing. A throw is a single server-resolved event with no hold to visualise,
	// and a client that predicted the Core leaving its own hands would show itself a throw the server
	// may refuse (cooldown, a possession change in flight).
	if (Requester->IsLocallyControlled() && !IsModeB())
	{
		if (bPressed)
		{
			if (GetPassCooldownRemaining() <= 0.f)
			{
				if (ATraceCharacter* Predicted = FindPassTargetFor(Carrier))
				{
					bLocalPassPredicted = true;
					LocalPassPredictStartTime = GetServerTimeSeconds();
					LocalPassPredictTarget = Predicted;
				}
			}
		}
		else
		{
			bLocalPassPredicted = false;
			LocalPassPredictTarget = nullptr;
		}
	}

	ServerSetPassInput(bPressed, Requester);
}

void ATraceCore::ServerSetPassInput_Implementation(bool bPressed, ATraceCharacter* Requester)
{
	// Network input: this RPC is routed by ownership (SetOwner(Carrier) in GrantTo), so only the
	// holding connection can reach it at all. It is re-validated anyway, by running the SAME
	// function the client ran - on the server HasAuthority() is true, so this lands in the branch
	// above and applies the identical press/release ownership rules. One copy of the rules, and a
	// client that lies about Requester is refused by them exactly as a local call would be.
	RequestPassInput(bPressed, Requester);
}

void ATraceCore::ServerTickPass(float /*DeltaSeconds*/)
{
	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	const bool bCollectPassStats = (CVarTracePassStats.GetValueOnAnyThread() != 0);

	if (!IsValid(Carrier) || !Carrier->IsAlive())
	{
		if (bPassActive && bCollectPassStats)
		{
			++TracePassStats::GStats.CancelHolderGone;
		}
		CancelPass(TEXT("holder gone"));
		ClearPassInput();
		return;
	}

	// --- An active pass: validate every frame, then complete on time. --------------------------
	if (bPassActive)
	{
		if (!bPassInputHeld)
		{
			// §4 [ASSUMPTION]: releasing early cancels, with the same instant restoration.
			if (bCollectPassStats)
			{
				++TracePassStats::GStats.CancelReleased;
			}
			CancelPass(TEXT("released"));
			return;
		}

		// Bots have no hands. Hold their crosshair on the receiver first, so what is validated below
		// is the aim an AI holder is actually being given.
		DriveBotAimAtPassTarget();

		// "Looking away cancels." Re-tested from the SERVER's copy of the holder's aim, never from
		// anything the client sent - the shield is down for as long as this stays true, so a client
		// that could assert it would be asserting its own invulnerability window.
		const AController* HolderController = Carrier->GetController();
		const bool bHolderIsAI = (HolderController != nullptr) && !HolderController->IsPlayerController();

		ETracePassRejectReason RejectCode = ETracePassRejectReason::None;
		const bool bStillLegal = IsLegalPassTarget(Carrier, PassTarget, /*bRequireAim=*/!bHolderIsAI,
			/*OutRejectReason=*/nullptr, &RejectCode);

		// --- §4.1 FIX: LINE-OF-SIGHT / AIM GRACE ------------------------------------------------
		//
		// The measured failure this exists for: a pass cancelled 24 ms before it would have completed
		// because the receiver crossed behind a lane rail. Both remaining tests here are sampled once
		// per frame against geometry, and a receiver who is RUNNING - which is every receiver worth
		// passing to - blinks through them repeatedly over a 0.5 s hold. One frame of occlusion must
		// not be the end of the pass.
		//
		// Only the transient geometric refusals are graced. A receiver who dies, who is no longer an
		// ally or who has been destroyed cancels on the very frame it happens, exactly as before,
		// because granting the Core to a corpse is a real bug and a rail is not.
		const float GraceSeconds = TraceCoreTuning::ResolvedValidationGrace(UTraceSettings::Get());

		if (bStillLegal)
		{
			if (PassGraceStartServerTime > 0.f)
			{
				// Rode it out. This is the counter that says how often the fix earns its keep.
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.GraceSaves;
					TracePassStats::GStats.GraceSecondsAccrued += FMath::Max(0.f, Now - PassGraceStartServerTime);
				}
				PassGraceStartServerTime = 0.f;
			}
		}
		else
		{
			const bool bTransient = IsTransientPassRejection(RejectCode);
			if (!bTransient || GraceSeconds <= 0.f)
			{
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.CancelLostTarget;
					if (bTransient && PassGraceStartServerTime > 0.f)
					{
						++TracePassStats::GStats.GraceExpired;
					}
				}
				PassGraceStartServerTime = 0.f;
				CancelPass(TEXT("looked away or target invalid"));
				return;
			}

			if (PassGraceStartServerTime <= 0.f)
			{
				PassGraceStartServerTime = Now;
			}
			else if ((Now - PassGraceStartServerTime) > GraceSeconds)
			{
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.CancelLostTarget;
					++TracePassStats::GStats.GraceExpired;
					TracePassStats::GStats.GraceSecondsAccrued += GraceSeconds;
				}
				PassGraceStartServerTime = 0.f;
				CancelPass(TEXT("receiver stayed out of sight past the grace window"));
				return;
			}
			// Inside the grace window: fall through and keep the pass alive, including completing it
			// if the timer runs out here. Completing during a blink of occlusion is correct - the
			// player held a legal receiver for the full 0.5 s and a rail passing across them at the
			// last moment is not a decision they made.
		}

		if ((Now - PassStartServerTime) >= FMath::Max(0.05f, UTraceSettings::Get().PassHoldSeconds))
		{
			ATraceCharacter* Receiver = PassTarget;

			UE_LOG(LogTraceGame, Log, TEXT("Core: pass completed %s -> %s"),
				*GetNameSafe(Carrier), *GetNameSafe(Receiver));

			// Cooldown lands on the passer's side of the transfer, but the Core is about to change
			// hands so it is really only meaningful if the pass is somehow refused below.
			PassCooldownEndServerTime = Now + FMath::Max(0.f, UTraceSettings::Get().PassCooldownSeconds);

			if (bCollectPassStats)
			{
				++TracePassStats::GStats.PassesCompleted;
			}

			CancelPass(nullptr);          // Silent: this is a completion, not an abort.
			GrantTo(Receiver, ETraceCoreGrantReason::Pass);
		}
		return;
	}

	// --- No active pass: start one if the button is down and everything lines up. --------------
	if (!bPassInputHeld)
	{
		return;
	}

	if (Now < PassCooldownEndServerTime)
	{
		return;
	}

	if (ATraceCharacter* Target = FindPassTargetFor(Carrier))
	{
		BeginPass(Target);
	}
}

void ATraceCore::BeginPass(ATraceCharacter* Target)
{
	if (!HasAuthority() || !IsValid(Target) || bPassActive)
	{
		return;
	}

	bPassActive = true;
	PassTarget = Target;
	PassStartServerTime = GetServerTimeSeconds();
	PassGraceStartServerTime = 0.f;

	if (CVarTracePassStats.GetValueOnAnyThread() != 0)
	{
		++TracePassStats::GStats.PassesBegun;
	}

	// THE RISK BEAT. Both halves of §4 happen right here, on one frame, from one bool:
	//   - the trace hardens (ApplyTraceInvulnerability, read back by UTraceTrailComponent), and
	//   - the shield drops (nothing to do: UTraceHealthComponent reads IsShieldSuppressedFor()).
	ApplyTraceInvulnerability();
	UpdateVisuals();

	// The pass window is 0.5s long and it decides whether the holder can be shot. It does not wait
	// for the next scheduled net update.
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Verbose, TEXT("Core: pass started %s -> %s (shield down, trace invulnerable)"),
		*GetNameSafe(Carrier), *GetNameSafe(Target));
}

void ATraceCore::CancelPass(const TCHAR* Reason)
{
	if (!HasAuthority())
	{
		return;
	}

	// THIS USED TO START WITH `bPassInputHeld = false;` AND THAT WAS THE BUG.
	//
	// bPassInputHeld is not pass state. It is a mirror of a PHYSICAL BUTTON, and only the player who
	// is holding that button (or losing the pawn that owns it) may change it. Clearing it here meant
	// that any cancel - a receiver stepping behind a lane rail for one frame, the crosshair drifting
	// off for one frame - silently disarmed a mouse button the player was still holding down. From
	// then on ServerTickPass hit `if (!bPassInputHeld) return;` and refused to acquire ANYBODY, no
	// matter how perfectly the crosshair sat on a teammate, until the player physically let go and
	// pressed again. Measured: a pass cancelled 24ms before it would have completed, then six
	// further seconds of holding the button with a legal receiver on the crosshair and nothing
	// happening. The player is left holding the Core - and the camera is `!bIsCarrier` and nothing
	// else, so "the Core will not leave" and "I am stuck in third person" are the same event.
	//
	// Churn is already prevented, and by the right mechanism: a cancel with a Reason spends
	// PassCooldownSeconds below, so a held button re-acquires on a cooldown rather than every frame.
	// The latch is cleared where it actually belongs - on a release, and on a possession change
	// (ClearPassInput, called from ReleaseHolder and GrantTo).

	if (!bPassActive)
	{
		return;
	}

	if (Reason != nullptr)
	{
		// Display, unlike its "pass started" counterpart. A cancel that the player did not ask for is
		// the failure signature of this whole area — it is what leaves them holding the Core, and
		// therefore stuck in third person — and it names its own reason. At most a few lines a
		// minute, since only one player can be passing at a time.
		UE_LOG(LogTraceGame, Display, TEXT("Core: pass cancelled (%s) - shield restored, trace vulnerable"), Reason);

		// A cancelled attempt still spends A cooldown. Without one, tapping the button is a free way
		// to churn the shield state (and the packets that carry it) every frame.
		//
		// §4.1 FIX: it is no longer the SAME cooldown a completed pass spends. Two full seconds of
		// lockout after an unwanted cancel is a large part of "passing is inconsistent": the player
		// holds mouse1 on a teammate, watches the ring fill, watches it die because the receiver
		// clipped a rail, and then gets nothing for two more seconds while still holding the button
		// on a perfectly legal target. 0.25 s still bounds the churn to one cycle per
		// (grace + cooldown) = 0.4 s, which is far below anything a player experiences as a lockout,
		// and it is what "make re-acquisition generous after a cancel" means in practice.
		PassCooldownEndServerTime = GetServerTimeSeconds()
			+ TraceCoreTuning::ResolvedCancelCooldown(UTraceSettings::Get());
	}

	bPassActive = false;
	PassTarget = nullptr;
	PassGraceStartServerTime = 0.f;

	// Instant restoration, both halves together (§9.2: the implemented answer is "instant").
	ApplyTraceInvulnerability();
	UpdateVisuals();
	ForceNetUpdate();
}

void ATraceCore::ClearPassInput()
{
	// The one legitimate reason to forget a held button without hearing a release: the pawn that was
	// holding it is not the holder any more. Whoever has the Core now has not pressed anything, and
	// a latch inherited from the previous holder would start a pass they never asked for.
	bPassInputHeld = false;
	PassInputInstigator = nullptr;
}

void ATraceCore::DriveBotAimAtPassTarget()
{
	if (!bPassActive || !IsValid(Carrier) || !IsValid(PassTarget))
	{
		return;
	}

	AController* HolderController = Carrier->GetController();
	if (HolderController == nullptr || HolderController->IsPlayerController())
	{
		return;   // A human holds their own crosshair; that is the mechanic.
	}

	const FVector ViewLocation = Carrier->GetPawnViewLocation();
	const FVector TargetChest = PassTarget->GetActorLocation()
		+ FVector(0.0, 0.0, static_cast<double>(UTraceSettings::Get().PassTargetChestOffsetZ));

	const FVector ToTarget = TargetChest - ViewLocation;
	if (!ToTarget.IsNearlyZero())
	{
		// Snapping is fine: a carrying pawn is on orient-to-movement, so its body does not follow
		// the control rotation and nothing visibly jerks.
		HolderController->SetControlRotation(ToTarget.Rotation());
	}
}

void ATraceCore::ApplyTraceInvulnerability()
{
	// Nothing to push: UTraceTrailComponent asks ATraceCore::IsTraceInvulnerableFor() directly, so
	// there is exactly one copy of the fact and it is the replicated one. This hook exists as the
	// single named place the rule is applied, and to force a visual refresh on the trail so the
	// hardening is legible the frame it happens.
	if (IsValid(Carrier) && Carrier->Trail != nullptr)
	{
		Carrier->Trail->NotifyInvulnerabilityChanged();
	}
}


// =================================================================================================
// Transfers
// =================================================================================================

void ATraceCore::GrantTo(ATraceCharacter* NewHolder, ETraceCoreGrantReason Reason)
{
	if (!HasAuthority())
	{
		return;
	}

	if (!IsValid(NewHolder) || !NewHolder->IsAlive())
	{
		return;
	}

	if (NewHolder == Carrier)
	{
		return;
	}

	ATraceCharacter* Previous = Carrier;

	// MODE B: a loose Core has no previous holder to ask, so TakeLooseCore hands us the team the
	// Core came FROM through this single-use override. Consumed here, unconditionally, so it can
	// never leak into the next possession. In mode A it is always None and this line is the old one.
	const ETraceTeam OverrideTeam = GraceOverrideTeam;
	GraceOverrideTeam = ETraceTeam::None;

	const ETraceTeam PreviousTeam = IsValid(Previous)
		? Previous->GetTeam()
		: OverrideTeam;

	const ETraceTeam NewTeam = NewHolder->GetTeam();

	// §2 [ASSUMPTION]: the 1s grace is a TEAM change rule. A completed teammate pass reads as
	// continuous possession, so the trace keeps forming without a gap; anything that takes the Core
	// across to the other side buys that side a second before their trace exists.
	//
	// Spec v4 §7 makes this rule serve mode B unchanged: "Turnovers in game state b should retain
	// the grace period from game state a. However, teammates picking up the core should not have a
	// grace period, the same as when they are passed to in game state a." An interception is a team
	// change and gets the grace; a teammate recovering a throw is not and gets none. One line, two
	// modes - which is why mode B did not get a grace rule of its own to drift from this one.
	const bool bTeamChanged = !AreAllies(PreviousTeam, NewTeam);

	CancelPass(nullptr);
	ReleaseHolder();

	// Explicit, and no longer a side effect of CancelPass: the incoming holder inherits no button.
	ClearPassInput();

	Carrier = NewHolder;
	PendingGrantTeam = ETraceTeam::None;
	bFallbackQueued = false;
	bOutOfPlay = false;

	// Mode B: whatever route brought the Core here, it is on a player now and is not loose. Held
	// alongside the assignment above rather than left to the caller, because GrantTo is the funnel
	// EVERY path ends at - including the ones mode B did not write (a kill steal off a carrier, the
	// nearest-enemy fallback, a kickoff), any of which can fire while a throw is still in the air.
	if (bLoose)
	{
		ClearLooseState();
	}
	LooseFromTeam = NewTeam;
	ThrowCooldownEndServerTime = GetServerTimeSeconds() + TraceModeBTuning::ThrowCooldown();
	LastCarrierGoalTestLocation = NewHolder->GetActorLocation();

	// Ownership is what lets the holding CLIENT send ServerSetPassInput() to this actor. Without it
	// the RPC is silently dropped by the net driver as "not owned by that connection".
	SetOwner(NewHolder);

	// The receiver may not pass back instantly; the cooldown belongs to whoever holds the Core.
	PassCooldownEndServerTime = GetServerTimeSeconds()
		+ ((Reason == ETraceCoreGrantReason::Pass)
			? FMath::Max(0.f, UTraceSettings::Get().PassCooldownSeconds)
			: 0.f);

	ApplyAttachment();

	// Grace BEFORE SetCarrying: SetCarrying(true) starts the trail emitting, and the trail must
	// already know it is not allowed to lay a point yet.
	const float GraceSeconds = bTeamChanged
		? FMath::Max(0.f, UTraceSettings::Get().CoreTurnoverGraceSeconds) : 0.f;

	if (NewHolder->Trail != nullptr)
	{
		// Spec v3 section 1: 1.0 -> 0.4 s, and now a UTraceSettings knob rather than a constant.
		NewHolder->Trail->SetEmitGrace(GraceSeconds);
	}

	// Recorded for Trace.ModeB.Verify. This is the rule's OWN answer, taken at the moment it is made;
	// anything read back afterwards would be measuring the trail rather than the decision.
	bLastGrantTeamChanged = bTeamChanged;
	LastGrantGraceSeconds = GraceSeconds;

	// SetCarrying() is the SOLE writer of ATracePlayerState::bIsCarrier — it sets the flag and
	// force-updates it. This used to write the mirror a second time right here, which is the
	// duplicated-state shape the audit flagged.
	NewHolder->SetCarrying(true);

	// Cache the holder's PlayerState. Not an optimisation: the PlayerState OUTLIVES the pawn, and
	// ReleaseHolder() has a path where the pawn is already invalid by the time it runs. Without a
	// handle taken while the pawn was alive there is nothing left to clear the mirror through, and a
	// phantom carrier sticks on the scoreboard for the rest of the match. See ReleaseHolder().
	HolderPlayerState = NewHolder->GetPlayerState<ATracePlayerState>();

	if (NewHolder->Trail != nullptr)
	{
		NewHolder->Trail->SetEmitting(true);
	}

	// The kill path: whoever kills the holder gets the Core, and this binding is how we learn about
	// it WITH the killer attached. The GameMode's own death handling cannot tell us that - it calls
	// the location-based legacy DropAt() - so the Core listens to the health component directly.
	if (NewHolder->Health != nullptr)
	{
		NewHolder->Health->OnDeath.AddUniqueDynamic(this, &ATraceCore::OnHolderDeath);
		BoundDeathHolder = NewHolder;
	}

	OnRep_Carrier();
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Log, TEXT("Core granted to %s (reason %d, team change %s)"),
		*GetNameSafe(NewHolder), static_cast<int32>(Reason), bTeamChanged ? TEXT("yes") : TEXT("no"));

	// A pass completed to a teammate who is ALREADY STANDING IN THE ENEMY ENDZONE scores, and so
	// does a Core taken from a carrier you killed while standing in it yourself. Neither generates
	// a begin-overlap — the receiver never moved — so the endzone test has to be driven off the
	// POSSESSION change, which is this function, rather than off the movement that usually causes
	// it. The GameMode owns the test; it reads the endzone volume itself, so this stays correct
	// however the zones are sized.
	//
	// LAST STATEMENT IN THE FUNCTION, deliberately: a score resets the field (kickoff, every pawn
	// teleported, this Core released again), so nothing here may touch member state afterwards.
	if (UWorld* World = GetWorld())
	{
		// Ordered to match ETraceCoreGrantReason exactly; clamped so adding a reason without
		// touching this cannot read off the end.
		static const TCHAR* GrantReasonNames[] =
		{
			TEXT("kickoff"), TEXT("completed pass"), TEXT("kill steal"), TEXT("fallback"), TEXT("debug grant"),
			TEXT("interception"), TEXT("recovery")
		};
		const int32 ReasonIndex = FMath::Clamp(static_cast<int32>(Reason), 0,
			static_cast<int32>(UE_ARRAY_COUNT(GrantReasonNames)) - 1);

		if (IsModeB())
		{
			// MODE B: the scoring volume is the narrow goal, not the full-width endzone, so the
			// possession-change test runs against the goal boxes instead. Same reasoning as mode A's
			// version: nobody overlaps-begins for a player who did not move, so taking the Core while
			// already standing in the goal has to be caught off the POSSESSION event.
			//
			// From == To: this is a possession change, not a movement, and CheckGoalScore's IsInside
			// branch is what covers the degenerate segment.
			const FVector Where = NewHolder->GetActorLocation();
			CheckGoalScore(Where, Where, NewTeam, EGoalMethod::Granted, GrantReasonNames[ReasonIndex]);
		}
		else if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			GameMode->CheckEndzoneScoreForCarrier(NewHolder, GrantReasonNames[ReasonIndex]);
		}
	}
}

void ATraceCore::ReleaseHolder()
{
	if (!HasAuthority())
	{
		return;
	}

	ATraceCharacter* Previous = Carrier;

	if (ATraceCharacter* Bound = BoundDeathHolder.Get())
	{
		if (Bound->Health != nullptr)
		{
			Bound->Health->OnDeath.RemoveDynamic(this, &ATraceCore::OnHolderDeath);
		}
	}
	BoundDeathHolder = nullptr;

	Carrier = nullptr;
	SetOwner(nullptr);

	// Possession is gone, so the held-button state that belonged to it is gone too. Every path that
	// takes the Core off somebody funnels through here - a completed pass, a kill, a disconnect, a
	// score, half time - which is why this is the one place it needs saying.
	ClearPassInput();

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

	// CLEAR THE MIRROR BEFORE THE EARLY-OUT, through the handle cached in GrantTo.
	//
	// The bug this fixes: ATracePlayerState OUTLIVES the pawn. If the holder's pawn became invalid
	// before release — which Tick() step 2 exists specifically to handle, and which a GC'd pawn or a
	// level teardown also produces — the `!IsValid(Previous)` return below fired first and the mirror
	// was never cleared. Nothing else could clear it either: ATracePlayerState::CopyProperties
	// deliberately does not carry bIsCarrier across, and SetCarrying() early-outs when the pawn's own
	// flag already matches, so a fresh pawn (default false) never writes it. The result was a
	// permanent phantom carrier on the scoreboard (ATraceHUD reads Member->bIsCarrier) for the rest
	// of the match.
	if (ATracePlayerState* CachedState = HolderPlayerState.Get())
	{
		CachedState->bIsCarrier = false;
		CachedState->ForceNetUpdate();
	}
	HolderPlayerState = nullptr;

	if (!IsValid(Previous))
	{
		return;
	}

	// See the declaration: this is what lets OnHolderDeath still recognise the death that is
	// currently unwinding, after the GameMode's own death path has already released the holder.
	RecentlyReleasedHolder = Previous;

	// Sole writer of the mirror for the live-pawn case (see ATraceCharacter::SetCarrying). The
	// explicit PlayerState write that used to sit here is gone with the other duplicates.
	Previous->SetCarrying(false);

	// Stop laying, but do NOT wipe what is already there: an expiring trace is counterplay the enemy
	// team has already earned, and popping it out of existence reads worse than letting it fade.
	// Death is the one case that clears instantly, and ATraceCharacter::HandleDeath does that.
	if (Previous->Trail != nullptr)
	{
		Previous->Trail->SetEmitting(false);
	}
}

void ATraceCore::KickoffTo(ETraceTeam ReceivingTeam)
{
	if (!HasAuthority())
	{
		return;
	}

	CancelPass(nullptr);
	ReleaseHolder();

	// Mode B: a kickoff supersedes anything in flight. Every caller of this function (a score, match
	// start, half time) is about to teleport ten pawns, and a Core still integrating its own arc
	// through that would be picked up mid-reset by whoever the teleport happened to land next to.
	if (bLoose)
	{
		ClearLooseState();
	}
	LooseFromTeam = ETraceTeam::None;
	LastCarrierGoalTestLocation = FVector::ZeroVector;

	// None means OUT OF PLAY, and it must NOT be quietly rewritten into a real team: the half-time
	// interval and the post-match screen both need the Core parked with nobody holding it, and
	// ATraceGameMode calls this exact function to say so.
	bOutOfPlay = (ReceivingTeam == ETraceTeam::None);
	PendingGrantTeam = ReceivingTeam;
	PendingGrantTime = GetServerTimeSeconds() + TraceCoreTuning::KickoffDelaySeconds;

	SetActorLocation(GetHomeLocation(), false, nullptr, ETeleportType::TeleportPhysics);
	SetActorRotation(FRotator::ZeroRotator);

	OnRep_Carrier();
	ForceNetUpdate();

	if (bOutOfPlay)
	{
		UE_LOG(LogTraceGame, Log, TEXT("Core: parked out of play (no holder)."));
	}
	else
	{
		UE_LOG(LogTraceGame, Log, TEXT("Core: kickoff queued for %s in %.1fs"),
			*TraceTeamName(PendingGrantTeam).ToString(), TraceCoreTuning::KickoffDelaySeconds);
	}
}

void ATraceCore::ResolveFallback(ETraceTeam LostTeam)
{
	if (!HasAuthority())
	{
		return;
	}

	const ETraceTeam ReceivingTeam = TraceOpposingTeam(LostTeam);

	// A death is play, so whatever "out of play" state a previous interval left behind is over.
	bOutOfPlay = false;

	// §2 [ASSUMPTION]: no attributable enemy killer -> nearest living enemy gets it.
	if (ReceivingTeam != ETraceTeam::None)
	{
		PendingGrantTeam = ReceivingTeam;
		PendingGrantTime = GetServerTimeSeconds();
		if (TryResolvePendingGrant())
		{
			return;
		}

		// Every enemy is on a respawn timer. The Core waits for them rather than vanishing; Tick
		// retries until somebody on that side is alive.
		UE_LOG(LogTraceGame, Log, TEXT("Core: no living %s player to receive it; holding for their next spawn."),
			*TraceTeamName(ReceivingTeam).ToString());
		return;
	}

	// The previous holder had no team at all (a spectator-slot pawn, or the Core was already
	// holderless). Fall back to the default kickoff rather than leaving it stranded.
	PendingGrantTeam = TraceCoreTuning::DefaultKickoffTeam;
	PendingGrantTime = GetServerTimeSeconds();
}

bool ATraceCore::TryResolvePendingGrant()
{
	if (!HasAuthority() || PendingGrantTeam == ETraceTeam::None || IsValid(Carrier))
	{
		return false;
	}

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	const FVector Reference = GetActorLocation();
	ATraceCharacter* Best = nullptr;
	double BestDistSq = TNumericLimits<double>::Max();

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive() || Candidate->GetTeam() != PendingGrantTeam)
		{
			continue;
		}

		const double DistSq = FVector::DistSquared(Reference, Candidate->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Candidate;
		}
	}

	if (Best == nullptr)
	{
		return false;
	}

	GrantTo(Best, ETraceCoreGrantReason::Kickoff);
	return true;
}

void ATraceCore::OnHolderDeath(AActor* Victim, AController* Killer, FName Cause)
{
	if (!HasAuthority())
	{
		return;
	}

	ATraceCharacter* VictimCharacter = Cast<ATraceCharacter>(Victim);
	if (!IsValid(VictimCharacter))
	{
		return;
	}

	// The victim must be the holder, or the holder the GameMode's death path released moments ago
	// (see RecentlyReleasedHolder). The `Carrier == nullptr` requirement is what makes the second
	// case safe: if somebody else has already been given the Core, this death cannot move it.
	const bool bWasHolder = (VictimCharacter == Carrier)
		|| (Carrier == nullptr && VictimCharacter == RecentlyReleasedHolder.Get());

	if (!bWasHolder)
	{
		return;
	}

	const ETraceTeam LostTeam = VictimCharacter->GetTeam();

	// Whoever killed the holder takes the Core. This one branch covers all three §2 cases:
	//   - "breaks your trace":   UTraceTrailComponent kills through the tripper's controller.
	//   - "kills the carrier":   any legal bullet, once the shield is down.
	//   - "intercepts the core": which, with no physics, means killing them mid-pass (§2 note).
	ATraceCharacter* KillerCharacter = (Killer != nullptr) ? Cast<ATraceCharacter>(Killer->GetPawn()) : nullptr;

	// This death is being resolved authoritatively here and now, so the coarse fallback the
	// GameMode's DropAt() queued a moment ago must not also fire on the next tick.
	bFallbackQueued = false;

	ReleaseHolder();

	if (IsValid(KillerCharacter)
		&& KillerCharacter->IsAlive()
		&& AreEnemies(LostTeam, KillerCharacter->GetTeam()))
	{
		UE_LOG(LogTraceGame, Log, TEXT("Core: %s killed the holder (%s) and takes the Core."),
			*GetNameSafe(KillerCharacter), *Cause.ToString());

		GrantTo(KillerCharacter, ETraceCoreGrantReason::Kill);
		return;
	}

	// Suicide, fall damage, a team kill, or a killer who died in the same exchange.
	ResolveFallback(LostTeam);
}


// =================================================================================================
// MODE B  —  the throwable, interceptable Core  (spec v4 §7)
// =================================================================================================

bool ATraceCore::IsModeB() const
{
	// ONE SOURCE OF TRUTH, asked every time. ATraceGameState publishes the mode (ATraceGameMode
	// resolves it from "?mode=a|b" once, at match start) and replicates it, so this answer is
	// identical on the server and on every client, in every phase. Nothing is cached on this actor:
	// a cached copy of the fact that decides what this actor IS is the classic way two systems end
	// up playing different games for a frame.
	const ATraceGameState* GameState = GetWorld() ? GetWorld()->GetGameState<ATraceGameState>() : nullptr;
	return GameState != nullptr && GameState->IsGoalMode();
}

bool ATraceCore::IsModeB(const UWorld* World)
{
	const ATraceGameState* GameState = (World != nullptr) ? World->GetGameState<ATraceGameState>() : nullptr;
	return GameState != nullptr && GameState->IsGoalMode();
}

void ATraceCore::SetScoringMode(ETraceScoringMode PublishedMode)
{
	if (!HasAuthority())
	{
		return;
	}

	const bool bPublishedIsModeB = TraceIsGoalMode(PublishedMode);
	const bool bStateSaysModeB = IsModeB();

	if (bPublishedIsModeB != bStateSaysModeB)
	{
		// Deliberately a warning and NOT an override. The Core holds no mode of its own to correct,
		// so the only thing this disagreement can mean is that the GameState has not been written
		// yet (or was written after this call) — an ordering fact worth one log line, and one that a
		// silent local latch would have hidden by papering over it.
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Core] SetScoringMode(%s) disagrees with the published ATraceGameState mode (%s). ")
			TEXT("The GameState is authoritative; publish it before telling the Core."),
			bPublishedIsModeB ? TEXT("B") : TEXT("A"), bStateSaysModeB ? TEXT("B") : TEXT("A"));
	}

	if (!bModeEverApplied || bAppliedModeB != bStateSaysModeB)
	{
		bAppliedModeB = bStateSaysModeB;
		bModeEverApplied = true;
		OnScoringModeChanged();
	}
}

void ATraceCore::OnScoringModeChanged()
{
	if (!HasAuthority())
	{
		return;
	}

	// GUARDED: switching to mode A with the Core in the air has to end the flight AND put the Core
	// somewhere legal, and the kickoff that does so re-enters this actor.
	if (bCoreStateLocked)
	{
		return;
	}
	FCoreStateLock Lock(this);

	// NOT arming or disarming any scoring volume. ATraceArenaBuilder builds both pairs and arms the
	// one belonging to the selected mode (ATraceEndzone::SetZoneActive); a second arming pass here
	// would be two systems fighting over one flag, and the loser would be whichever ran last.

	// Mode A cannot represent a loose Core at all - there is no pickup in mode A, so one would lie
	// there for the rest of the match. Normalise it into the model mode A does have: a kickoff for
	// the side that did not last hold it.
	if (!IsModeB() && bLoose)
	{
		const ETraceTeam Owed = (LooseFromTeam != ETraceTeam::None)
			? TraceOpposingTeam(LooseFromTeam) : TraceCoreTuning::DefaultKickoffTeam;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] scoring mode switched to A with the Core loose - recovering it as a kickoff for %s."),
			*TraceTeamName(Owed).ToString());

		ClearLooseState();
		KickoffTo(Owed);
	}

	GoalBoxRefreshTime = 0.f;
	RefreshGoalVolumes(/*bForce=*/true);

	UE_LOG(LogTraceGame, Display, TEXT("[Core] scoring mode is now %s (%d goal volume(s) visible to the Core)."),
		IsModeB() ? TEXT("B - goals, Core is thrown") : TEXT("A - endzones, Core is a status"),
		GoalBoxes.Num());

	// THE ONE FAILURE THIS FILE CANNOT DETECT ANY OTHER WAY.
	//
	// Every mode-B knob is matched to UTraceSettings BY NAME, at runtime, through FindFProperty. A
	// misspelling therefore does not fail the build and does not warn: the lookup simply misses, the
	// CVar default is played, and the settings panel shows a slider that moves nothing. That is
	// exactly what happened to CoreThrowUpBias, which was declared for a while as
	// "CoreThrowUpwardBias" and was silently dead the whole time.
	//
	// So on the frame mode B is entered, say out loud which knobs are wired to the panel and which
	// fell back. In a healthy build every line reads "settings"; a "CVAR FALLBACK" line names the
	// property that needs declaring, and a dead knob can never again be invisible.
	if (IsModeB())
	{
		TraceModeBTuning::LogKnobBindings();
		TraceModeBTuning::LogFlightModel(GetWorld());
	}
}


// --- Goals ---------------------------------------------------------------------------------------

void ATraceCore::RegisterGoalVolume(AActor* GoalOwner, ETraceTeam DefendingTeam, UPrimitiveComponent* Volume)
{
	if (!IsValid(GoalOwner) || !IsValid(Volume) || DefendingTeam == ETraceTeam::None)
	{
		return;
	}

	TArray<TraceGoalRegistry::FEntry>& Registry = TraceGoalRegistry::Entries();

	for (TraceGoalRegistry::FEntry& Entry : Registry)
	{
		if (Entry.GoalOwner.Get() == GoalOwner)
		{
			Entry.Volume = Volume;
			Entry.DefendingTeam = DefendingTeam;
			return;
		}
	}

	TraceGoalRegistry::FEntry NewEntry;
	NewEntry.GoalOwner = GoalOwner;
	NewEntry.Volume = Volume;
	NewEntry.DefendingTeam = DefendingTeam;
	Registry.Add(NewEntry);

	UE_LOG(LogTraceGame, Display, TEXT("[Core] goal volume registered: %s defends %s, bounds %s"),
		*GetNameSafe(GoalOwner), *TraceTeamName(DefendingTeam).ToString(),
		*Volume->Bounds.GetBox().ToString());
}

void ATraceCore::UnregisterGoalVolume(AActor* GoalOwner)
{
	TraceGoalRegistry::Entries().RemoveAll([GoalOwner](const TraceGoalRegistry::FEntry& Entry)
	{
		return !Entry.GoalOwner.IsValid() || Entry.GoalOwner.Get() == GoalOwner;
	});
}

void ATraceCore::RefreshGoalVolumes(bool bForce)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (!IsModeB())
	{
		// Mode A has no goals at all - the endzone is the scoring volume and it is not this class's
		// business. Emptied rather than merely skipped so nothing can score off a stale box if the
		// mode is switched back and forth.
		GoalBoxes.Reset();
		return;
	}

	const float Now = GetServerTimeSeconds();
	if (!bForce && Now < GoalBoxRefreshTime)
	{
		return;
	}
	GoalBoxRefreshTime = Now + TraceModeBTuning::GoalRefreshInterval;

	GoalBoxes.Reset();

	// --- 1. Externally registered volumes win outright. -------------------------------------------
	for (int32 Index = TraceGoalRegistry::Entries().Num() - 1; Index >= 0; --Index)
	{
		const TraceGoalRegistry::FEntry& Entry = TraceGoalRegistry::Entries()[Index];
		const UPrimitiveComponent* Volume = Entry.Volume.Get();

		if (!Entry.GoalOwner.IsValid() || Volume == nullptr)
		{
			TraceGoalRegistry::Entries().RemoveAt(Index);
			continue;
		}
		if (Volume->GetWorld() != World)
		{
			continue;   // Another PIE world's goal. Not ours.
		}

		FTraceGoalBox Goal;
		Goal.Box = Volume->Bounds.GetBox();
		Goal.DefendingTeam = Entry.DefendingTeam;
		GoalBoxes.Add(Goal);
	}

	if (GoalBoxes.Num() > 0)
	{
		return;
	}

	// --- 2. The arena's own goal volumes. ---------------------------------------------------------
	//
	// ATraceArenaBuilder builds a mode-A endzone AND a mode-B goal at each end and arms the pair
	// belonging to the selected mode, so the goals are already in the world with the right shape.
	// This asks them for it — IsGoalVolume() to skip the full-width endzones, IsZoneActive() to skip
	// the pair the mode is not playing, GetZoneBounds() for the box — rather than reconstructing a
	// goal from GoalWidthFieldFraction and GoalHeightUU.
	//
	// It used to reconstruct it, and the reconstruction was CORRECT to the millimetre against the
	// volume that shipped a few hours later. That is exactly why it had to go: a second copy of a
	// shape agrees with the first right up until somebody changes one of them, and then the goal you
	// can see and the goal that scores are different boxes.
	for (TActorIterator<ATraceEndzone> It(World); It; ++It)
	{
		const ATraceEndzone* Zone = *It;
		if (!IsValid(Zone) || !Zone->IsGoalVolume() || !Zone->IsZoneActive()
			|| Zone->OwningTeam == ETraceTeam::None)
		{
			continue;
		}

		FTraceGoalBox Goal;
		Goal.DefendingTeam = Zone->OwningTeam;
		Goal.Box = Zone->GetZoneBounds();
		GoalBoxes.Add(Goal);
	}
}

bool ATraceCore::GetAttackGoalCentre(const UWorld* World, ETraceTeam AttackingTeam, FVector& OutCentre)
{
	ATraceCore* TheCore = ATraceCore::Get(World);
	if (TheCore == nullptr || !TheCore->IsModeB() || AttackingTeam == ETraceTeam::None)
	{
		return false;
	}

	TheCore->RefreshGoalVolumes(/*bForce=*/false);

	for (const FTraceGoalBox& Goal : TheCore->GoalBoxes)
	{
		// Same rule as ATraceEndzone::ScoresHere: you score in the goal your OPPONENT defends.
		if (Goal.DefendingTeam != ETraceTeam::None && AttackingTeam == TraceOpposingTeam(Goal.DefendingTeam))
		{
			OutCentre = Goal.Box.GetCenter();
			return true;
		}
	}

	return false;
}

bool ATraceCore::GetAttackGoalBox(const UWorld* World, ETraceTeam AttackingTeam, FBox& OutBox)
{
	ATraceCore* TheCore = ATraceCore::Get(World);
	if (TheCore == nullptr || !TheCore->IsModeB() || AttackingTeam == ETraceTeam::None)
	{
		return false;
	}

	TheCore->RefreshGoalVolumes(/*bForce=*/false);

	for (const FTraceGoalBox& Goal : TheCore->GoalBoxes)
	{
		if (Goal.DefendingTeam != ETraceTeam::None && AttackingTeam == TraceOpposingTeam(Goal.DefendingTeam))
		{
			OutBox = Goal.Box;
			return true;
		}
	}

	return false;
}

float ATraceCore::GetThrowSpeed()
{
	return TraceModeBTuning::ThrowSpeed();
}

float ATraceCore::GetThrowUpBias()
{
	return TraceModeBTuning::ThrowUpBias();
}

float ATraceCore::GetThrowGravityZ(const UWorld* World)
{
	const float WorldGravityZ = (World != nullptr) ? World->GetGravityZ() : -980.f;
	return WorldGravityZ * TraceModeBTuning::GravityScale();
}

float ATraceCore::GetThrowMuzzleForward()
{
	return static_cast<float>(TraceModeBTuning::ThrowMuzzleForward);
}

int32 ATraceCore::GoalsByMethod[static_cast<int32>(ATraceCore::EGoalMethod::Count)] = {};

static FAutoConsoleCommand GTraceModeBTallyCmd(
	TEXT("Trace.ModeB.Tally"),
	TEXT("MODE B. Prints how many goals have been scored by throwing, by carrying in, and by taking possession inside the mouth."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] GOAL TALLY: thrown in %d | carried in %d | possession inside the mouth %d | total %d"),
			ATraceCore::GoalsByMethod[0], ATraceCore::GoalsByMethod[1], ATraceCore::GoalsByMethod[2],
			ATraceCore::GoalsByMethod[0] + ATraceCore::GoalsByMethod[1] + ATraceCore::GoalsByMethod[2]);
	}));

bool ATraceCore::CheckGoalScore(const FVector& From, const FVector& To, ETraceTeam ScoringTeam, EGoalMethod Method,
	const TCHAR* How)
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority() || !IsModeB() || ScoringTeam == ETraceTeam::None)
	{
		return false;
	}

	RefreshGoalVolumes(/*bForce=*/false);

	for (const FTraceGoalBox& Goal : GoalBoxes)
	{
		if (Goal.DefendingTeam == ETraceTeam::None || ScoringTeam != TraceOpposingTeam(Goal.DefendingTeam))
		{
			continue;
		}

		// FMath::LineBoxIntersection is the swept test; the explicit IsInside covers the degenerate
		// case where From == To (a stationary carrier standing in the mouth), which the segment test
		// is not required to report.
		if (!Goal.Box.IsInside(To) && !FMath::LineBoxIntersection(Goal.Box, From, To, To - From))
		{
			continue;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] GOAL by %s (%s) into the goal %s defends. Box %s"),
			*TraceTeamName(ScoringTeam).ToString(), How,
			*TraceTeamName(Goal.DefendingTeam).ToString(), *Goal.Box.ToString());

		// THE TALLY IS TAKEN OFF THE SCOREBOARD, NOT OFF THIS TEST.
		//
		// ATraceGameMode::NotifyScored DEBOUNCES, and it has to: the same goal can be seen by this
		// swept test and by ATraceEndzone's 10 Hz poll within a few milliseconds of each other. If the
		// tally counted attempts rather than awards, a carrier standing inside the mouth after a
		// debounced award would still be inside it on the next frame and would count again, every
		// frame, until something moved it. Comparing the score across the call is exact and costs two
		// reads.
		const ATraceGameState* GameState = World->GetGameState<ATraceGameState>();
		const int32 ScoreBefore = (GameState != nullptr) ? GameState->GetScore(ScoringTeam) : 0;

		if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			// NotifyScored resets the field: it kicks off, teleports ten pawns and releases this Core.
			// Nothing may touch member state after it, which is why every caller of this function
			// returns immediately on true.
			GameMode->NotifyScored(ScoringTeam);
		}

		if (GameState != nullptr && GameState->GetScore(ScoringTeam) > ScoreBefore)
		{
			const int32 MethodIndex = FMath::Clamp(static_cast<int32>(Method), 0,
				static_cast<int32>(EGoalMethod::Count) - 1);
			++GoalsByMethod[MethodIndex];

			UE_LOG(LogTraceGame, Display, TEXT("[ModeB] goal tally: thrown %d, carried %d, granted %d"),
				GoalsByMethod[0], GoalsByMethod[1], GoalsByMethod[2]);
		}
		return true;
	}

	return false;
}


// --- The throw -----------------------------------------------------------------------------------

bool ATraceCore::ThrowFromHolder(ATraceCharacter* Thrower)
{
	if (!HasAuthority() || !IsModeB())
	{
		return false;
	}

	// One guarded, indivisible sequence from here to the end of the function.
	if (bCoreStateLocked)
	{
		return false;
	}

	if (bLoose || !IsValid(Thrower) || Thrower != Carrier || !Thrower->IsAlive())
	{
		return false;
	}

	const float Now = GetServerTimeSeconds();
	if (Now < ThrowCooldownEndServerTime)
	{
		return false;
	}

	FCoreStateLock Lock(this);

	// SERVER'S copy of the aim, never a client-supplied direction. See the ThrowFromHolder doc.
	FVector ThrowDirection = Thrower->GetAimDirection();
	if (ThrowDirection.IsNearlyZero())
	{
		ThrowDirection = Thrower->GetActorForwardVector();
	}
	ThrowDirection = ThrowDirection.GetSafeNormal();

	const float Speed = TraceModeBTuning::ThrowSpeed();
	const FVector LaunchVelocity = ThrowDirection * Speed
		+ FVector::UpVector * (Speed * TraceModeBTuning::ThrowUpBias());

	const FVector LaunchLocation = Thrower->GetPawnViewLocation()
		+ ThrowDirection * TraceModeBTuning::ThrowMuzzleForward;

	const ETraceTeam ThrowerTeam = Thrower->GetTeam();

	// ORDER MATTERS AND IS THE POINT OF THE LOCK.
	//   1. end any pass window (mode B never opens one, but a mode switch mid-hold could leave one);
	//   2. hand the Core off its holder through the SAME exit mode A uses, so the trail stops
	//      emitting, the carrier mirror is cleared, the death binding is dropped and ownership goes;
	//   3. only then flip to loose and place the actor.
	// Doing 3 before 2 would leave ReleaseHolder's DetachFromActor snapping the Core back onto the
	// thrower's transform - which is the "stored its home position as a relative offset" family of
	// bug the file header warns about.
	CancelPass(nullptr);
	ReleaseHolder();

	bLoose = true;
	bLooseAtRest = false;
	LooseFromTeam = ThrowerTeam;
	LooseThrower = Thrower;
	LooseStartServerTime = Now;
	LooseLocation = LaunchLocation;
	LooseVelocity = LaunchVelocity;

	// A loose Core belongs to nobody, so it must be visible to everybody - including the thrower,
	// whose own camera had it hidden while they carried it (bOwnerNoSee, resolved through the actor
	// owner chain that ReleaseHolder just cleared).
	SetActorLocation(LaunchLocation, false, nullptr, ETeleportType::TeleportPhysics);
	ApplyAttachment();
	UpdateVisuals();
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] THROW by %s (%s) at %.0f uu/s from %s"),
		*GetNameSafe(Thrower), *TraceTeamName(ThrowerTeam).ToString(),
		LaunchVelocity.Size(), *LaunchLocation.ToCompactString());

	return true;
}


// --- The loose Core ------------------------------------------------------------------------------

void ATraceCore::ServerTickLooseCore(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority() || !bLoose)
	{
		return;
	}

	// A mode switch, a half-time interval or a score can all land while the Core is in the air.
	if (!IsModeB())
	{
		return;   // OnScoringModeChanged() normalises it; nothing to integrate in the meantime.
	}

	const float Now = GetServerTimeSeconds();
	const float Step = FMath::Clamp(DeltaSeconds, 0.f, 0.1f);   // A hitch must not teleport the Core.

	const FVector StartLocation = LooseLocation;

	// --- 1. Integrate. No UProjectileMovementComponent: see the header. --------------------------
	if (!bLooseAtRest)
	{
		const double GravityZ = static_cast<double>(World->GetGravityZ())
			* static_cast<double>(TraceModeBTuning::GravityScale());

		LooseVelocity = FVector(LooseVelocity) + FVector(0.0, 0.0, GravityZ * Step);

		const FVector Desired = StartLocation + FVector(LooseVelocity) * Step;

		// ONE sphere sweep against static world geometry. Pawns are deliberately not swept against:
		// "first contact takes it" is resolved by the proximity poll below, so a player standing in
		// the flight path must not also bounce the Core off themselves.
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreLoose), /*bTraceComplex=*/false, this);
		Params.AddIgnoredActor(this);

		FHitResult Hit;
		const bool bBlocked = World->SweepSingleByChannel(
			Hit, StartLocation, Desired, FQuat::Identity, ECC_WorldStatic,
			FCollisionShape::MakeSphere(TraceModeBTuning::CollisionRadius), Params);

		if (bBlocked)
		{
			// Land just off the surface so the next sweep does not start inside it.
			LooseLocation = Hit.Location + Hit.ImpactNormal * 2.0;

			const FVector Reflected = FVector(LooseVelocity).MirrorByVector(Hit.ImpactNormal)
				* TraceModeBTuning::Bounce();
			LooseVelocity = Reflected;

			if (Reflected.Size() < TraceModeBTuning::RestSpeed())
			{
				LooseVelocity = FVector::ZeroVector;
				bLooseAtRest = true;
				UE_LOG(LogTraceGame, Verbose, TEXT("[ModeB] loose Core came to rest at %s"),
					*FVector(LooseLocation).ToCompactString());
			}
		}
		else
		{
			LooseLocation = Desired;
		}
	}

	SetActorLocation(LooseLocation, false, nullptr, ETeleportType::TeleportPhysics);

	// --- 2. Did the flight cross a goal? ----------------------------------------------------------
	//
	// Before the pickup poll, and swept across this frame's motion. The throwing team is the only one
	// that can score from it, exactly as with an endzone: a Core that flies into the goal its own
	// team defends is not an own goal, it is a bad throw.
	if (CheckGoalScore(StartLocation, LooseLocation, LooseFromTeam, EGoalMethod::Thrown, TEXT("thrown in")))
	{
		return;   // The field has been reset under us. Touch nothing.
	}

	// --- 3. First contact takes it. ---------------------------------------------------------------
	if (ServerTryLoosePickup())
	{
		return;
	}

	// --- 4. It may never be lost permanently. -----------------------------------------------------
	const FBox FieldBox = [World]() -> FBox
	{
		if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
		{
			return Arena->GetFieldBounds();
		}
		return FBox(ForceInit);
	}();

	// Generous vertical slack: a lobbed Core is legitimately far above the field box, and a Core
	// under the floor has fallen out of the world and is gone for good.
	const bool bOutOfWorld = (FieldBox.IsValid != 0)
		&& (FVector(LooseLocation).X < FieldBox.Min.X - 2000.0
			|| FVector(LooseLocation).X > FieldBox.Max.X + 2000.0
			|| FVector(LooseLocation).Y < FieldBox.Min.Y - 2000.0
			|| FVector(LooseLocation).Y > FieldBox.Max.Y + 2000.0
			|| FVector(LooseLocation).Z < FieldBox.Min.Z - 1500.0);

	if (bOutOfWorld)
	{
		ResetLooseCore(TEXT("left the field"));
		return;
	}

	// 0 disables the timer outright (UTraceSettings::CoreLooseResetSeconds, "0 = never"). The
	// out-of-world rescue above still runs, so even with the timer off the Core cannot be lost
	// permanently — it can only be left lying somewhere a player can still reach it.
	const float ResetAfter = TraceModeBTuning::LooseResetSeconds();
	if (ResetAfter > 0.f && (Now - LooseStartServerTime) >= ResetAfter)
	{
		ResetLooseCore(TEXT("untouched past the reset timer"));
	}
}

bool ATraceCore::ServerTryLoosePickup()
{
	if (!HasAuthority() || !bLoose || bCoreStateLocked)
	{
		return false;
	}

	const float Now = GetServerTimeSeconds();
	const float Radius = TraceModeBTuning::PickupRadius();
	const float SelfLockoutEnd = LooseStartServerTime + TraceModeBTuning::SelfPickupLockout();
	const ATraceCharacter* Thrower = LooseThrower.Get();

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	ATraceCharacter* Best = nullptr;
	double BestOverlapSq = TNumericLimits<double>::Max();

	const FVector CoreLocation = LooseLocation;

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive())
		{
			continue;
		}

		// The ONE exception to "first contact, anyone". The Core leaves from inside the thrower's own
		// pickup radius, so without a brief lockout on them alone, every throw would be caught by the
		// player who threw it on the very next tick and the mechanic would not exist. Everybody else -
		// teammate or enemy - is eligible from frame one, which is what makes interception the point.
		if (Candidate == Thrower && Now < SelfLockoutEnd)
		{
			continue;
		}

		// Distance to the CAPSULE, not to the actor origin: the origin is at the pawn's midpoint, so
		// an origin test would refuse a Core rolling past a player's feet.
		double DistanceSq = FVector::DistSquared(CoreLocation, Candidate->GetActorLocation());
		if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
		{
			const FVector CapsuleCentre = Capsule->GetComponentLocation();
			const double HalfHeight = static_cast<double>(Capsule->GetScaledCapsuleHalfHeight());
			const double CapsuleRadius = static_cast<double>(Capsule->GetScaledCapsuleRadius());

			// Closest point on the capsule's axis, then subtract the radius off the distance.
			FVector Closest = CoreLocation;
			Closest.Z = FMath::Clamp(CoreLocation.Z, CapsuleCentre.Z - HalfHeight, CapsuleCentre.Z + HalfHeight);
			Closest.X = CapsuleCentre.X;
			Closest.Y = CapsuleCentre.Y;

			const double Surface = FMath::Max(0.0, FVector::Dist(CoreLocation, Closest) - CapsuleRadius);
			DistanceSq = Surface * Surface;
		}

		if (DistanceSq > static_cast<double>(Radius) * static_cast<double>(Radius))
		{
			continue;
		}

		if (DistanceSq < BestOverlapSq)
		{
			BestOverlapSq = DistanceSq;
			Best = Candidate;
		}
	}

	if (Best == nullptr)
	{
		return false;
	}

	TakeLooseCore(Best);
	return true;
}

void ATraceCore::TakeLooseCore(ATraceCharacter* Taker)
{
	if (!HasAuthority() || !IsValid(Taker) || bCoreStateLocked)
	{
		return;
	}

	FCoreStateLock Lock(this);

	const ETraceTeam FromTeam = LooseFromTeam;
	const ETraceTeam TakerTeam = Taker->GetTeam();

	// THE GRACE RULE, and the reason ETraceCoreGrantReason grew two enumerators rather than one.
	// Spec v4 §7 verbatim: turnovers keep mode A's grace, teammates picking it up get none. Both
	// halves are decided by the same AreAllies() line inside GrantTo - all this does is tell GrantTo
	// which team the Core came FROM, since there is no previous holder left to ask.
	const bool bTeamChanged = !(FromTeam != ETraceTeam::None && FromTeam == TakerTeam);
	const ETraceCoreGrantReason Reason = bTeamChanged
		? ETraceCoreGrantReason::Interception
		: ETraceCoreGrantReason::Recovery;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] %s by %s (%s) - Core was thrown by %s, %s"),
		bTeamChanged ? TEXT("INTERCEPTION") : TEXT("RECOVERY"),
		*GetNameSafe(Taker), *TraceTeamName(TakerTeam).ToString(),
		*TraceTeamName(FromTeam).ToString(),
		bTeamChanged ? TEXT("turnover grace applies") : TEXT("no grace, same team"));

	// Clear the loose state BEFORE granting, and set the single-use grace override in the same
	// breath. GrantTo attaches the Core to the taker; leaving bLoose true across that call would let
	// any re-entrant tick integrate a Core that is now parented to a pawn.
	// Captured for Trace.ModeB.Verify BEFORE the grant, because GrantTo can score, reset the field
	// and hand the Core straight back out again — by the time it returns, the take this scenario
	// caused is no longer the possession anybody can observe.
	if (bVerifyAwaitingTake)
	{
		bVerifyAwaitingTake = false;
		bVerifyTakeSeen = true;
		VerifyTookTeam = TakerTeam;
		VerifyFromTeam = FromTeam;
		bVerifyTookGrace = bTeamChanged;
		VerifyTakerName = GetNameSafe(Taker);
	}

	ClearLooseState();
	GraceOverrideTeam = FromTeam;

	GrantTo(Taker, Reason);

	// GrantTo consumes it, but clear again unconditionally: if GrantTo refused the grant (a dead
	// taker between the poll and here) a stale override must not survive into the next possession.
	GraceOverrideTeam = ETraceTeam::None;
}

void ATraceCore::ClearLooseState()
{
	bLoose = false;
	bLooseAtRest = false;
	LooseVelocity = FVector::ZeroVector;
	LooseThrower = nullptr;
	LooseStartServerTime = 0.f;
	// LooseFromTeam is deliberately NOT cleared here: TakeLooseCore reads it immediately afterwards
	// to decide the grace, and KickoffTo/GrantTo overwrite it on the next possession.
}

void ATraceCore::ResetLooseCore(const TCHAR* Reason)
{
	if (!HasAuthority() || bCoreStateLocked)
	{
		return;
	}

	FCoreStateLock Lock(this);

	// The team that did NOT throw it away gets the restart. A throw nobody collected is a wasted
	// possession, and handing it back to the side that wasted it would make stalling free.
	const ETraceTeam Owed = (LooseFromTeam != ETraceTeam::None)
		? TraceOpposingTeam(LooseFromTeam)
		: TraceCoreTuning::DefaultKickoffTeam;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] loose Core reset (%s) after %.1fs - kickoff to %s"),
		Reason, GetServerTimeSeconds() - LooseStartServerTime, *TraceTeamName(Owed).ToString());

	ClearLooseState();
	LooseFromTeam = ETraceTeam::None;

	KickoffTo(Owed);
}

// --- Trace.ModeB.Verify: a scripted proof that each mode-B rule fires ------------------------------
//
// WHY THIS EXISTS. Mode B has four rules and a live match exercises them on its own schedule: a
// throw when a bot decides to throw, an interception when somebody happens to be in the way, a goal
// when a throw happens to go in, a reset when a Core happens to be abandoned. Waiting for all four
// to coincide inside one run is not a test, it is a hope — and "we never saw it fail" is not
// evidence a rule works. This drives each of them deliberately, in order, and prints PASS or FAIL
// with the fact it checked.
//
// It drives the REAL functions: ThrowFromHolder for the throws (so the aim, the launch, the release
// of the holder and the trail are all the shipping path) and the ordinary pickup poll for the takes.
// The only thing it fakes is WHERE the Core starts for the goal and reset cases, which is exactly the
// part a match cannot be asked to arrange on cue.

static TAutoConsoleVariable<int32> CVarModeBVerifyRequested(
	TEXT("Trace.ModeB.Verify"),
	0,
	TEXT("1: run the mode B verification scenario once (throw -> teammate recovery with NO grace, ")
	TEXT("throw -> enemy interception WITH grace, a Core thrown into the goal, and the loose reset timer)."),
	ECVF_Default);

bool ATraceCore::DebugLaunchLoose(const FVector& From, const FVector& LaunchVelocity, ETraceTeam FromTeam)
{
	if (!HasAuthority() || !IsModeB() || bCoreStateLocked)
	{
		return false;
	}

	FCoreStateLock Lock(this);

	CancelPass(nullptr);
	ReleaseHolder();

	bLoose = true;
	bLooseAtRest = false;
	LooseFromTeam = FromTeam;
	LooseThrower = nullptr;
	LooseStartServerTime = GetServerTimeSeconds();
	LooseLocation = From;
	LooseVelocity = LaunchVelocity;

	SetActorLocation(From, false, nullptr, ETeleportType::TeleportPhysics);
	ApplyAttachment();
	UpdateVisuals();
	ForceNetUpdate();
	return true;
}

void ATraceCore::TickModeBVerification()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	// --- Arm --------------------------------------------------------------------------------------
	if (VerifyStep < 0)
	{
		if (CVarModeBVerifyRequested.GetValueOnAnyThread() == 0)
		{
			return;
		}
		if (!IsModeB())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] refused: the match is playing mode A."));
			CVarModeBVerifyRequested->Set(0, ECVF_SetByConsole);
			return;
		}
		// Wait for a settled, running half. Arming during the pre-match window put the very first
		// throw on the same frame as the 1st-half kickoff, which cancelled it and made the scenario
		// report a failure of a rule that had never run.
		const ATraceGameState* GameState = World->GetGameState<ATraceGameState>();
		if (GameState == nullptr
			|| GameState->TraceMatchState != ETraceMatchState::InProgress
			|| GameState->IsHalfTimeBreak()
			|| !IsValid(Carrier))
		{
			return;
		}

		VerifyStep = 0;
		VerifyPassCount = 0;
		VerifyFailCount = 0;
		VerifyStepDeadline = 0.f;
		UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] ===== mode B verification starting ====="));
	}

	// --- A step that is waiting on an outcome -----------------------------------------------------
	if (VerifyStepDeadline > 0.f)
	{
		const bool bTimedOut = (Now >= VerifyStepDeadline);

		// Steps 0 and 1 are waiting for a taker; steps 2 and 3 are waiting for the Core to stop being
		// loose (a goal or a reset both end with a kickoff).
		if (VerifyStep <= 1)
		{
			if (bVerifyTakeSeen)
			{
				bVerifyTakeSeen = false;

				// THE RULE, judged on the take that actually happened rather than on who was predicted
				// to win the race. Spec v4 §7 is a statement about TEAMS, not about players: grace iff
				// the Core crossed sides. "First contact takes it" means the thrower can legitimately
				// beat the intended receiver to their own throw, and that is not a failure - the rule
				// still has to hold for whoever got there.
				const bool bShouldGrace = (VerifyTookTeam != VerifyFromTeam);
				const bool bOk = (bVerifyTookGrace == bShouldGrace)
					&& (bVerifyTookGrace == (bLastGrantTeamChanged && LastGrantGraceSeconds > 0.f));

				(bOk ? VerifyPassCount : VerifyFailCount)++;

				UE_LOG(LogTraceGame, Display,
					TEXT("[ModeBVerify] step %d %s: %s (%s) took the thrown Core | grace %s, rule says %s"),
					VerifyStep, bOk ? TEXT("PASS") : TEXT("FAIL"),
					*VerifyTakerName, *TraceTeamName(VerifyTookTeam).ToString(),
					bVerifyTookGrace ? *FString::Printf(TEXT("APPLIED %.2fs"), LastGrantGraceSeconds) : TEXT("none"),
					bShouldGrace ? TEXT("APPLIED (crossed teams)") : TEXT("none (same team)"));

				VerifyStepDeadline = 0.f;
				++VerifyStep;
				return;
			}
		}
		else if (VerifyStep >= 4)
		{
			// --- Steps 4 and 5: the BOT paths. -----------------------------------------------------
			//
			// Judged on ATraceCore::GoalsByMethod rather than on the scoreboard, because the whole
			// point of these two steps is WHICH path scored. A carrier that wanders in and a Core that
			// is thrown in both add one to the same score.
			const int32 MethodIndex = (VerifyStep == 4)
				? static_cast<int32>(EGoalMethod::Carried) : static_cast<int32>(EGoalMethod::Thrown);

			if (VerifyStep == 5 && bLoose)
			{
				bVerifyThrowSeen = true;   // A throw left the bot's hands. Whether it goes in is next.
			}

			const int32 TallyNow = GoalsByMethod[MethodIndex];
			if (TallyNow > VerifyGoalTallyAtStart)
			{
				++VerifyPassCount;
				UE_LOG(LogTraceGame, Display,
					TEXT("[ModeBVerify] step %d PASS: a bot scored by %s (%s goals %d -> %d)."),
					VerifyStep, (VerifyStep == 4) ? TEXT("CARRYING THE CORE IN") : TEXT("THROWING AT THE GOAL"),
					(VerifyStep == 4) ? TEXT("carried") : TEXT("thrown"), VerifyGoalTallyAtStart, TallyNow);

				VerifyStepDeadline = 0.f;
				bVerifyThrowSeen = false;
				++VerifyStep;
				return;
			}

			if (bTimedOut)
			{
				++VerifyFailCount;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ModeBVerify] step %d FAIL: no goal by %s inside the window (carrier=%s, loose=%d, ")
					TEXT("a throw did%s leave)."),
					VerifyStep, (VerifyStep == 4) ? TEXT("carrying in") : TEXT("throwing at the goal"),
					*GetNameSafe(Carrier), bLoose ? 1 : 0, bVerifyThrowSeen ? TEXT("") : TEXT(" NOT"));

				VerifyStepDeadline = 0.f;
				bVerifyThrowSeen = false;
				++VerifyStep;
			}
			return;
		}
		else if (!bLoose)
		{
			// Step 2 is the goal, and "the Core stopped being loose" is not enough on its own — a
			// pickup or a reset would look the same from here. The point on the scoreboard is the
			// fact under test, so that is what is checked.
			bool bOk = true;
			FString Detail = TEXT("the loose Core left play as expected");

			if (VerifyStep == 2)
			{
				int32 ScoreNow = 0;
				if (const ATraceGameState* GameState = World->GetGameState<ATraceGameState>())
				{
					ScoreNow = GameState->GetScore(VerifyExpectTeam);
				}
				bOk = (ScoreNow > VerifyGoalsAtStart);
				Detail = FString::Printf(TEXT("%s score %d -> %d"),
					*TraceTeamName(VerifyExpectTeam).ToString(), VerifyGoalsAtStart, ScoreNow);
			}

			(bOk ? VerifyPassCount : VerifyFailCount)++;
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step %d %s: %s."),
				VerifyStep, bOk ? TEXT("PASS") : TEXT("FAIL"), *Detail);

			VerifyStepDeadline = 0.f;
			++VerifyStep;
			return;
		}

		if (bTimedOut)
		{
			++VerifyFailCount;
			bVerifyAwaitingTake = false;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: timed out (loose=%d, carrier=%s)."),
				VerifyStep, bLoose ? 1 : 0, *GetNameSafe(Carrier));
			VerifyStepDeadline = 0.f;
			++VerifyStep;
		}
		return;
	}

	// --- Start the next step ----------------------------------------------------------------------
	TArray<ATraceCharacter*> Everyone;
	GatherCharacters(Everyone);

	switch (VerifyStep)
	{
	case 0:
	case 1:
	{
		// A real throw, aimed at a real target, through ThrowFromHolder.
		if (!IsValid(Carrier) || !Carrier->IsAlive())
		{
			return;
		}

		const bool bWantTeammate = (VerifyStep == 0);
		const ETraceTeam HolderTeam = Carrier->GetTeam();

		ATraceCharacter* Target = nullptr;
		double BestDistSq = TNumericLimits<double>::Max();
		for (ATraceCharacter* Candidate : Everyone)
		{
			if (!IsValid(Candidate) || Candidate == Carrier || !Candidate->IsAlive())
			{
				continue;
			}
			const bool bIsMate = AreAllies(HolderTeam, Candidate->GetTeam());
			if (bIsMate != bWantTeammate)
			{
				continue;
			}
			const double DistSq = FVector::DistSquared(Carrier->GetActorLocation(), Candidate->GetActorLocation());
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Target = Candidate;
			}
		}

		if (Target == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d SKIPPED: no living %s to throw at."),
				VerifyStep, bWantTeammate ? TEXT("teammate") : TEXT("enemy"));
			++VerifyStep;
			return;
		}

		// Put the target within arm's reach of the throw so the outcome is about the RULE (who may
		// take it, and what grace they get) rather than about a bot's ability to run somewhere.
		const FVector Behind = Carrier->GetActorLocation()
			+ (Carrier->GetActorForwardVector().GetSafeNormal2D() * 420.0);
		Target->SetActorLocation(FVector(Behind.X, Behind.Y, Target->GetActorLocation().Z),
			false, nullptr, ETeleportType::TeleportPhysics);

		if (AController* HolderController = Carrier->GetController())
		{
			const FVector ToTarget = Target->GetActorLocation() - Carrier->GetPawnViewLocation();
			if (!ToTarget.IsNearlyZero())
			{
				HolderController->SetControlRotation(ToTarget.Rotation());
			}
		}

		VerifyThrower = Carrier;
		VerifyExpectTeam = Target->GetTeam();
		VerifyExpectGrace = !AreAllies(HolderTeam, Target->GetTeam());

		ThrowCooldownEndServerTime = 0.f;   // The scenario is not testing the cooldown.
		bVerifyAwaitingTake = true;
		bVerifyTakeSeen = false;

		if (!ThrowFromHolder(Carrier))
		{
			++VerifyFailCount;
			bVerifyAwaitingTake = false;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: ThrowFromHolder refused."), VerifyStep);
			++VerifyStep;
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step %d: thrown at %s (%s) - expecting %s with %s grace."),
			VerifyStep, *GetNameSafe(Target), bWantTeammate ? TEXT("teammate") : TEXT("enemy"),
			*TraceTeamName(VerifyExpectTeam).ToString(), VerifyExpectGrace ? TEXT("a") : TEXT("no"));

		VerifyStepDeadline = Now + 4.f;
		return;
	}

	case 2:
	{
		// A Core thrown INTO the goal. Launched from just outside the mouth, moving into it, so the
		// swept goal test in ServerTickLooseCore is what has to catch it.
		const ETraceTeam Attacker = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::Blue;

		FVector GoalCentre = FVector::ZeroVector;
		if (!GetAttackGoalCentre(World, Attacker, GoalCentre))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 2 FAIL: no goal box resolved for %s."),
				*TraceTeamName(Attacker).ToString());
			++VerifyStep;
			return;
		}

		// Approach along X from the field side, so "into the goal" is unambiguous whichever end it is.
		const double FieldCentreX = [World]() -> double
		{
			if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
			{
				return Arena->GetFieldBounds().GetCenter().X;
			}
			return 0.0;
		}();

		const double Sign = (GoalCentre.X >= FieldCentreX) ? 1.0 : -1.0;
		const FVector Start = GoalCentre - FVector(Sign * 1200.0, 0.0, 0.0);
		const FVector LaunchVelocity = FVector(Sign * 2400.0, 0.0, 200.0);

		VerifyGoalsAtStart = 0;
		VerifyExpectTeam = Attacker;
		if (const ATraceGameState* GameState = World->GetGameState<ATraceGameState>())
		{
			VerifyGoalsAtStart = GameState->GetScore(Attacker);
		}

		if (!DebugLaunchLoose(Start, LaunchVelocity, Attacker))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 2 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 2: %s Core launched at the goal mouth %s from %s - expecting a GOAL."),
			*TraceTeamName(Attacker).ToString(), *GoalCentre.ToCompactString(), *Start.ToCompactString());

		VerifyStepDeadline = Now + 4.f;
		return;
	}

	case 3:
	{
		// The reset timer. Launched straight down into the floor in a corner with a back-dated start
		// time, so it comes to rest untouched and the timer is the only thing that can end it.
		const ETraceTeam FromTeam = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::Blue;

		FVector Corner = GetHomeLocation() + FVector(0.0, 0.0, 400.0);
		if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
		{
			const FBox FieldBox = Arena->GetFieldBounds();
			if (FieldBox.IsValid != 0)
			{
				Corner = FVector(FieldBox.GetCenter().X, FieldBox.Max.Y - 600.0, FieldBox.Min.Z + 400.0);
			}
		}

		if (!DebugLaunchLoose(Corner, FVector(0.0, 0.0, -50.0), FromTeam))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 3 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		// Back-date the clock rather than waiting out the real timer: the rule under test is "a loose
		// Core is put back into play once it has been ignored for long enough", not the wall clock.
		LooseStartServerTime = Now - (TraceModeBTuning::LooseResetSeconds() - 2.f);

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 3: Core parked at %s with its reset timer 2s from expiry - expecting a reset."),
			*Corner.ToCompactString());

		VerifyStepDeadline = Now + 8.f;
		return;
	}

	case 4:
	case 5:
	{
		// =========================================================================================
		// THE TWO BOT PATHS (spec v5 §4 follow-up).
		//
		// Step 4: a bot with the Core, 2800 uu from its own attacking mouth, must RUN IT IN.
		// Step 5: a bot with the Core, 5200 uu out, must SHOOT AT THE GOAL.
		//
		// WHY THIS IS SCRIPTED AND NOT OBSERVED. Both branches are gated on the carrier being within
		// ballistic range of the mouth, and a measured run says a carrier never is: over three runs
		// the closest any carrier came to the goal was 16676 uu, against a Core that carries ~7300 uu
		// on its best arc. So "we watched for ten minutes and never saw it" is evidence about the
		// pitch, not about the branch — which is exactly the trap the previous pass fell into when it
		// reported the carry-in path dead. This puts a carrier where the decision is taken, and then
		// changes NOTHING else: the bot's own UpdateThrow / UpdateCarryInCommit / BehaviourCarryToGoal
		// make every choice from there, through the same inputs a human's mouse reaches.
		// =========================================================================================
		if (!IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;   // Between possessions. Try again next tick; the scenario is not on a clock yet.
		}

		if (Carrier->GetController() == nullptr || Carrier->GetController()->IsPlayerController())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] step %d SKIPPED: the carrier is not a bot, so there is no bot decision to test."),
				VerifyStep);
			++VerifyStep;
			return;
		}

		const ETraceTeam Attacker = Carrier->GetTeam();

		FBox GoalBox(ForceInit);
		if (!GetAttackGoalBox(World, Attacker, GoalBox))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: no goal box resolved for %s."),
				VerifyStep, *TraceTeamName(Attacker).ToString());
			++VerifyStep;
			return;
		}

		// The MOUTH, not the box centre: the box runs from the goal line back to the end wall.
		const double FieldCentreX = [World]() -> double
		{
			if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
			{
				return Arena->GetFieldBounds().GetCenter().X;
			}
			return 0.0;
		}();

		const double Sign = (GoalBox.GetCenter().X >= FieldCentreX) ? 1.0 : -1.0;
		const double MouthX = (Sign > 0.0) ? GoalBox.Min.X : GoalBox.Max.X;

		// 700 uu is inside the carry-in commit band and about a second of running - long enough that
		// the bot has to CHOOSE to run rather than being dropped on the line, short enough that the
		// step measures that decision and not the carrier's odds of surviving a defended mouth.
		// MEASURED: at 2800 and again at 1500 the bot committed (the [BotCarryIn] line proves it) and
		// was then shot off the Core inside 1.2 s by the two defenders holding the mouth, so those
		// distances tested the defence, not the branch.
		// 5200 is outside the commit band and inside throwing range.
		const double Standoff = (VerifyStep == 4) ? 700.0 : 5200.0;
		// PICK A SPOT WITH A CLEAR VIEW OF THE MOUTH. The arena is full of cover, and the first
		// attempt at this step dropped the carrier 58 uu in front of a 3x-height cover box: the bot
		// correctly refused every shot ("blocked=18 of 18") and the step looked like a broken
		// decision when it was a broken placement. Try the centre line first, then a few lanes
		// either side, and take the first one that can actually see what it is being asked to
		// attack. ECC_Visibility is the channel the bot's own lane test uses.
		const double GoalZ = GoalBox.Min.Z + (GoalBox.Max.Z - GoalBox.Min.Z) * 0.5;
		const double CarrierZ = Carrier->GetActorLocation().Z;

		FVector Where(MouthX - Sign * Standoff, GoalBox.GetCenter().Y, CarrierZ);
		{
			const FVector MouthPoint(MouthX, GoalBox.GetCenter().Y, GoalZ);
			FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreVerifyPlacement), false, Carrier);

			static const double LaneOffsets[] = { 0.0, 600.0, -600.0, 1200.0, -1200.0 };
			for (const double Offset : LaneOffsets)
			{
				const FVector Candidate(MouthX - Sign * Standoff, GoalBox.GetCenter().Y + Offset, CarrierZ);
				const FVector Eye = Candidate + FVector(0.0, 0.0, 64.0);
				if (!World->LineTraceTestByChannel(Eye, MouthPoint, ECC_Visibility, Params))
				{
					Where = Candidate;
					break;
				}
			}
		}

		Carrier->SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);
		LastCarrierGoalTestLocation = Where;   // Do not sweep the teleport itself through the goal.

		if (AController* BotController = Carrier->GetController())
		{
			const FVector ToGoal = FVector(MouthX, GoalBox.GetCenter().Y, GoalZ) - Carrier->GetPawnViewLocation();
			if (!ToGoal.IsNearlyZero())
			{
				BotController->SetControlRotation(ToGoal.Rotation());
			}
		}

		VerifyGoalTallyAtStart = GoalsByMethod[(VerifyStep == 4)
			? static_cast<int32>(EGoalMethod::Carried) : static_cast<int32>(EGoalMethod::Thrown)];
		bVerifyThrowSeen = false;
		ThrowCooldownEndServerTime = 0.f;   // The scenario is not testing the cooldown.

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step %d: %s (%s) placed %.0f uu from its goal mouth at %s, facing it - expecting a goal by %s."),
			VerifyStep, *GetNameSafe(Carrier), *TraceTeamName(Attacker).ToString(), Standoff,
			*Where.ToCompactString(), (VerifyStep == 4) ? TEXT("CARRYING IN") : TEXT("THROWING"));

		// Generous: the bot has to run 2800 uu (step 4) or wait out a throw cooldown, slew onto a
		// lofted solution and watch a 2-3 second arc land (step 5).
		VerifyStepDeadline = Now + ((VerifyStep == 4) ? 16.f : 14.f);
		return;
	}

	default:
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] ===== finished: %d PASS, %d FAIL ====="), VerifyPassCount, VerifyFailCount);
		CVarModeBVerifyRequested->Set(0, ECVF_SetByConsole);
		VerifyStep = -1;
		return;
	}
	}
}

bool ATraceCore::GetLooseCoreInterceptPoint(float LeadSeconds, FVector& OutPoint) const
{
	if (!bLoose)
	{
		return false;
	}

	const float Lead = FMath::Clamp(LeadSeconds, 0.f, 2.f);

	// BALLISTIC, not linear. A straight extrapolation of the velocity was close enough while the Core
	// flew flat under 55% gravity; under the v5 weight model it falls at roughly twice that rate and
	// leaves the arc within a quarter of a second, so a chaser led by velocity alone runs at a point
	// well above where the Core will actually be. Same integration the loose tick performs, one step.
	const float GravityZ = (bLooseAtRest || GetWorld() == nullptr) ? 0.f : GetThrowGravityZ(GetWorld());

	OutPoint = FVector(LooseLocation)
		+ FVector(LooseVelocity) * Lead
		+ FVector(0.0, 0.0, 0.5 * static_cast<double>(GravityZ) * static_cast<double>(Lead) * static_cast<double>(Lead));
	return true;
}


// =================================================================================================
// Legacy shims (see the file header)
// =================================================================================================

void ATraceCore::TryPickup(ATraceCharacter* Character)
{
	if (!HasAuthority() || !IsValid(Character))
	{
		return;
	}

	// Under the status model there is no pickup: the Core is essentially always held, so a version
	// of this that refused whenever somebody had it would refuse every time and Trace.DebugTakeCore
	// — its only remaining caller, and the tool used to inspect the carrier's view and their trace —
	// would never fire. So it is an unconditional debug grant, and it takes the Core off whoever has
	// it. Nothing in the shipping game reaches this.
	if (IsValid(Carrier))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("Core: debug grant to %s takes it from %s."),
			*GetNameSafe(Character), *GetNameSafe(Carrier));
	}

	GrantTo(Character, ETraceCoreGrantReason::Debug);
}

// Throw() IS DELETED. It was the legacy pass entry point, documented as being reached from
// ATraceCharacter::PerformPass "and therefore the bots' DoPass()" — a map to a road that no longer
// existed: PerformPass called RequestPassInput directly and never went through here, and the bots go
// through ApplyPassInput -> DoPassPressed -> RequestPassInput. Zero callers, and all three of the
// functions it named are deleted too. The one door is RequestPassInput().

void ATraceCore::DropAt(const FVector& /*Location*/, const FVector& /*Impulse*/)
{
	if (!HasAuthority() || !IsValid(Carrier))
	{
		return;
	}

	// The holder lost the Core with nobody credited: a disconnect (ATraceGameMode::Logout), or the
	// GameMode's death path, which runs before our own OnHolderDeath listener knows the killer.
	//
	// QUEUED, not resolved, precisely because of that second caller - see the note in Tick(). If a
	// killer does exist, OnHolderDeath fires later in the same broadcast, transfers the Core and
	// clears this flag, so the death produces exactly one transfer.
	FallbackTeam = Carrier->GetTeam();
	bFallbackQueued = true;

	ReleaseHolder();
	ForceNetUpdate();
}

// ResetToCenter() IS DELETED. Zero callers — every score, match start and half-time reset calls
// KickoffTo() directly, which is the entry point this merely wrapped.

// =================================================================================================
// Presentation
// =================================================================================================

void ATraceCore::ApplyAttachment()
{
	AppliedHolder = Carrier;
	bAppliedPassActive = bPassActive;
	bAppliedLoose = bLoose;
	bAppliedEver = true;

	if (IsValid(Carrier))
	{
		if (GetAttachParentActor() != Carrier)
		{
			AttachToActor(Carrier, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		}

		// Straight up the capsule axis, so the holder's yaw cannot swing the orb around and the
		// beacon is a true vertical wherever they are facing.
		SetActorRelativeLocation(FVector(0.0, 0.0, TraceCoreTuning::OrbHeight));
		SetActorRelativeRotation(FRotator::ZeroRotator);
	}
	else
	{
		if (GetAttachParentActor() != nullptr)
		{
			DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}
	}

	// The beacon is placed in the actor's own space, once, from its two end heights.
	if (Beacon != nullptr && Beacon->GetStaticMesh() != nullptr)
	{
		const FBoxSphereBounds Bounds = Beacon->GetStaticMesh()->GetBounds();
		const double MeshHeight = FMath::Max(1.0, 2.0 * Bounds.BoxExtent.Z);
		const double MeshWidth = FMath::Max(1.0, 2.0 * Bounds.BoxExtent.X);

		const double Height = TraceCoreTuning::BeaconTop - TraceCoreTuning::BeaconBottom;
		const double Centre = (TraceCoreTuning::BeaconTop + TraceCoreTuning::BeaconBottom) * 0.5
			- TraceCoreTuning::OrbHeight;   // Relative to the actor, which sits at OrbHeight.

		Beacon->SetRelativeLocation(FVector(0.0, 0.0, Centre));
		Beacon->SetRelativeScale3D(FVector(
			TraceCoreTuning::BeaconWidth / MeshWidth,
			TraceCoreTuning::BeaconWidth / MeshWidth,
			Height / MeshHeight));

		// Shown while somebody is holding it - a holderless Core is a kickoff, not a target - and, in
		// mode B, while it is LOOSE, which is the one moment in that mode when every player on the
		// field needs to find it from wherever they happen to be standing.
		Beacon->SetVisibility(IsValid(Carrier) || bLoose);
	}

	// FPrimitiveSceneProxy caches the actor owner chain when it is BUILT, and that chain is what
	// bOwnerNoSee is resolved against. The chain changes every time the Core changes hands (GrantTo
	// calls SetOwner), and SetOwnerNoSee(true) would early-out because the flag itself is unchanged
	// - so the proxy has to be rebuilt explicitly or the previous holder would keep the Core hidden
	// from themselves while the new one stared straight at it.
	if (Mesh != nullptr)
	{
		Mesh->MarkRenderStateDirty();
	}
	if (Beacon != nullptr)
	{
		Beacon->MarkRenderStateDirty();
	}
}

void ATraceCore::UpdateVisuals()
{
	FLinearColor Color = TraceTeamColor(GetHolderTeam());
	Color.A = 1.f;

	const bool bColorChanged = !bColorApplied || !Color.Equals(AppliedColor, 0.001f);
	AppliedColor = Color;
	bColorApplied = true;

	// The pass window is the one moment an enemy can actually shoot the holder. Making the orb
	// visibly hotter for exactly those 0.5s is the read that turns the risk beat into something a
	// defender can act on rather than something only the passer knows about.
	const float GlowScale = bPassActive ? TraceCoreTuning::PassGlowMultiplier : 1.f;

	auto Push = [this, &Color, GlowScale, bColorChanged](UMaterialInstanceDynamic* Material, float BaseGlow)
	{
		if (Material == nullptr)
		{
			return;
		}
		if (bColorChanged)
		{
			Material->SetVectorParameterValue(TEXT("Color"), Color);
			Material->SetVectorParameterValue(TEXT("BaseColor"), Color);   // No-op if absent.
		}
		if (bMaterialIsNeon)
		{
			Material->SetScalarParameterValue(TEXT("Glow"), BaseGlow * GlowScale);
		}
	};

	Push(MeshMID, TraceCoreTuning::OrbGlow);
	Push(BeaconMID, TraceCoreTuning::BeaconGlow);
}

void ATraceCore::EnforceHolderTrailState()
{
	// Self-heal. Several foreign systems switch trails off wholesale - the GameMode clears every
	// player's trail on every score (ResetPlayersToSpawns) and on every death - and under the status
	// model there is no pickup event left to switch the holder's trail back on. So the Core, which
	// is the thing that knows who the holder is, re-asserts it every tick. SetEmitting() early-outs
	// when the state is unchanged, so in the normal case this costs one bool compare.
	if (!IsValid(Carrier) || !Carrier->IsAlive() || Carrier->Trail == nullptr)
	{
		return;
	}

	if (!Carrier->Trail->IsEmitting())
	{
		Carrier->Trail->SetEmitting(true);
	}
}

void ATraceCore::OnRep_Carrier()
{
	ApplyAttachment();
	UpdateVisuals();

	// Server truth about who holds it supersedes any local pass prediction.
	bLocalPassPredicted = false;
	LocalPassPredictTarget = nullptr;
}

void ATraceCore::OnRep_Owner()
{
	Super::OnRep_Owner();

	if (Mesh != nullptr)
	{
		Mesh->MarkRenderStateDirty();
	}
	if (Beacon != nullptr)
	{
		Beacon->MarkRenderStateDirty();
	}
}

void ATraceCore::OnRep_Loose()
{
	// bLoose flips the Core between "attached to a pawn" and "an object at LooseLocation", and both
	// halves of that are done by ApplyAttachment, which reads Carrier and bLoose together. Carrier
	// and bLoose are separate properties and can land in either order, so Tick's reconciliation
	// (bAppliedEver / AppliedHolder) is what actually guarantees they converge - this is the fast
	// path, not the only path.
	if (bLoose)
	{
		SetActorLocation(LooseLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}

	ApplyAttachment();
	UpdateVisuals();
}

void ATraceCore::OnRep_PassState()
{
	// The authoritative answer has arrived; stop predicting either way.
	bLocalPassPredicted = false;
	LocalPassPredictTarget = nullptr;

	UpdateVisuals();

	if (IsValid(Carrier) && Carrier->Trail != nullptr)
	{
		Carrier->Trail->NotifyInvulnerabilityChanged();
	}
}
