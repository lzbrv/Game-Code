// Copyright (c) Trace. All Rights Reserved.
//
// See TraceCore.h for the model. This file is the whole of it: there is no physics, no pickup
// volume and no flight path left to go wrong.

#include "Gameplay/TraceCore.h"

#include "Abilities/TraceAbilityComponent.h"   // spec v14 §6: Mace's per-player magnet radius
#include "Abilities/TraceAbilityTypes.h"       // spec v19 §3: TraceAbilityTraits — Mortimer's longer charge hold
#include "Audio/TraceAudio.h"                  // spec v26 §9: CoreTurnover (game-side), CorePickup (client-side)
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
#include "Engine/Engine.h"                      // GEngine->GetWorldContexts() (Trace.ModeB.CoreProbe)
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                        // TActorIterator (character gather fallback)
#include "GameFramework/CharacterMovementComponent.h"   // spec v8 §4: the thrower's own velocity
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"        // GetServerWorldTimeSeconds()
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"             // FMath::SegmentDistToSegmentSafe
#include "Misc/CommandLine.h"                   // spec v13 §8: -TraceTurnoverRepro= / -TraceLegacyLanding
#include "Misc/Parse.h"
#include "Containers/Ticker.h"                  // FTSTicker (Trace.Integ.TurnoverDemo)
#include "Misc/Paths.h"                         // FPaths::Combine  (Trace.Integ.TurnoverDemo)
#include "UnrealClient.h"                       // FScreenshotRequest (Trace.Integ.TurnoverDemo)
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

	/**
	 * The same lookup for a BOOL property.
	 *
	 * Needed because spec v13 §6's "does it clamp" is a tick box on the settings page
	 * (UTraceSettings::bCoreThrowChargeClampsAtFull), not a float, and a float bridge cannot see it -
	 * FindFProperty<FFloatProperty> returns null for a bool, which would have reported the knob as
	 * MISSING and silently used the console default instead of the shipped one.
	 */
	static bool TrySettingsBool(const TCHAR* PropertyName, bool& OutValue)
	{
		static TMap<FName, const FBoolProperty*> Cache;

		const FName Key(PropertyName);
		const FBoolProperty* const* Found = Cache.Find(Key);
		if (Found == nullptr)
		{
			Found = &Cache.Add(Key, FindFProperty<FBoolProperty>(UTraceSettings::StaticClass(), Key));
		}

		if (*Found == nullptr)
		{
			return false;
		}

		OutValue = (*Found)->GetPropertyValue_InContainer(&UTraceSettings::Get());
		return true;
	}

	/** Settings property if it exists and the CVar has not been overridden; otherwise the CVar. */
	static bool ResolveBool(const TCHAR* SettingsName, const TAutoConsoleVariable<int32>& CVar)
	{
		const IConsoleVariable* Console = CVar.AsVariable();
		const uint32 SetBy = (Console != nullptr)
			? (static_cast<uint32>(Console->GetFlags()) & static_cast<uint32>(ECVF_SetByMask))
			: 0u;

		if (SetBy > static_cast<uint32>(ECVF_SetByConstructor))
		{
			return CVar.GetValueOnAnyThread() != 0;
		}

		bool FromSettings = false;
		if (TrySettingsBool(SettingsName, FromSettings))
		{
			return FromSettings;
		}

		return CVar.GetValueOnAnyThread() != 0;
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

// =================================================================================================
// MODE B ONLY — SPEC v8 §4, THE THROW INHERITS THE THROWER'S MOMENTUM
//
// "When jumping and throwing the core, the core doesn't seem to keep momentum. Make sure that the
// core has the momentum from the throw and also carries momentum from the player."
//
// It did not seem to keep momentum because it did not keep any: ThrowFromHolder built the launch as
// aim * ThrowSpeed + up * (ThrowSpeed * UpBias) and that was the whole of it. The thrower's velocity
// appeared nowhere, so a Core thrown at a dead stop, at a full sprint and at the apex of a jump all
// left at exactly 2236 uu/s along the crosshair.
//
// ONE KNOB, and it is a FRACTION rather than a fudge (spec v8 §4's [ASSUMPTION] is explicit about
// this): the default inherits the thrower's velocity IN FULL, INCLUDING Z, and a designer who finds
// that too strong turns it down instead of a programmer picking 0.6 and burying it in the launch
// expression. 0 restores the pre-v8 throw exactly, which is the A/B for judging the feel.
//
// WHY VERTICAL IS NOT SPECIAL-CASED. The obvious hedge is to inherit horizontal velocity in full and
// vertical at some smaller fraction, on the grounds that a jump's +Z is large. That hedge would
// delete the exact case the user reported. A jumping throw is the one the note names, and its whole
// character is that the Core leaves with the jump still in it.
//
// MEASURED at the shipped defaults (throw speed 3000 base -> 2236 after the weight model, up bias
// 0.12 -> 0.29 after weight, so an impulse of 2236 forward + 649 up = 2328 uu/s), see the numbers
// in the pass report. Sprint is ~900 uu/s and the jump apex path is ~+560 uu/s Z at release.
// =================================================================================================

static TAutoConsoleVariable<float> CVarModeBThrowInheritance(
	TEXT("Trace.ModeB.ThrowVelocityInheritance"),
	1.f,
	TEXT("MODE B, spec v8 §4. Fraction of the THROWER'S OWN velocity added to a thrown Core's launch ")
	TEXT("velocity, vertical included, so a jumping throw carries the jump and a sprinting throw ")
	TEXT("carries the sprint. 1 = full inheritance (the default), 0 = the pre-v8 throw that inherited ")
	TEXT("nothing. UTraceSettings::CoreThrowVelocityInheritance."),
	ECVF_Default);

// =================================================================================================
// MODE B ONLY — SPEC v13 §6, THE CHARGE-UP THROW
//
// "When a player RELEASES the throw button, the core should instantly be released. The longer the
// player holds down, the more momentum the core has. Start by making a one second charge up time to
// reach the current core throw momentum. Charge time to throw momentum should be a linear
// correlation. So if the player just clicks the throw button it will throw with very low momentum."
//
// FOUR KNOBS, because the note contains four decisions and every one of them is a number somebody
// will want to move after five minutes of play:
//
//   ThrowChargeSeconds  1.0   the wind-up. "Start by making a one second charge up time."
//   ThrowChargeFloor    0.15  what an instant click buys. [ASSUMPTION]. NOT ZERO, and the reason is
//                             not timidity: at 0 the impulse vanishes and the launch is nothing but
//                             spec v8 §4's inherited velocity, so a standing tap drops the Core on
//                             the player's own feet, inside their own pickup radius, and reads as
//                             "the throw button is broken" rather than as "that was a weak throw".
//                             0.15 of 2236 uu/s is ~335 uu/s, which travels a couple of metres and
//                             lands in front of you — visibly a fumble, unmistakably a throw.
//   ThrowChargeMax      1.0   what a full hold buys. 1.0 IS THE POINT: the note asks the one-second
//                             hold to reach "the current core throw momentum", i.e. the shipped
//                             throw is now the CEILING and nothing about it gets faster. This is
//                             what makes v13 §6 a pure nerf-with-agency rather than a retune.
//   ThrowChargeClamp    on    whether holding past the charge time keeps adding. [ASSUMPTION]: it
//                             clamps. Off turns the hold into an unbounded ramp, which is a
//                             different game and is one console command away for whoever wants it.
//
// LINEAR, as asked, and stated once in GetThrowChargeScaleForHold(). The HUD meter, the bot solver
// and the launch all call that function; nothing re-derives it.
//
// WHAT IT SCALES: the impulse only. See ComputeThrowLaunchVelocity.
// =================================================================================================

static TAutoConsoleVariable<float> CVarModeBThrowChargeSeconds(
	TEXT("Trace.ModeB.ThrowChargeSeconds"),
	1.f,
	TEXT("MODE B, spec v13 §6. Seconds the throw button must be HELD to reach full throw momentum. ")
	TEXT("Momentum scales linearly from the floor to the max across this time. ")
	TEXT("UTraceSettings::CoreThrowChargeSeconds."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBThrowChargeFloor(
	TEXT("Trace.ModeB.ThrowChargeFloor"),
	0.15f,
	TEXT("MODE B, spec v13 §6. Fraction of full throw impulse an INSTANT CLICK leaves with. 'Very low' ")
	TEXT("but deliberately not zero - a zero-momentum throw drops at the thrower's feet and reads as a ")
	TEXT("bug. UTraceSettings::CoreThrowChargeFloor."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBThrowChargeMax(
	TEXT("Trace.ModeB.ThrowChargeMax"),
	1.f,
	TEXT("MODE B, spec v13 §6. Fraction of full throw impulse a FULL hold leaves with. 1.0 means a full ")
	TEXT("charge is exactly the pre-v13 throw, which is what 'reach the current core throw momentum' ")
	TEXT("asks for. UTraceSettings::CoreThrowChargeMax."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarModeBThrowChargeClamp(
	TEXT("Trace.ModeB.ThrowChargeClamp"),
	1,
	TEXT("MODE B, spec v13 §6. 1 (default): holding past Trace.ModeB.ThrowChargeSeconds adds nothing. ")
	TEXT("0: the linear ramp keeps going, so a long hold throws harder than a full charge."),
	ECVF_Default);

/**
 * Armed from the command line as well as the console, and polled, for the reason Trace.ModeB.Verify
 * is: -ExecCmds runs at engine init, long before there is a match, a Core or anybody holding it. A
 * plain console command can only be typed into a window, and spec v8's testing policy forbids one.
 * It stays armed until there is a live holder to throw, then disarms itself.
 */
static TAutoConsoleVariable<int32> CVarModeBMomentumTest(
	TEXT("Trace.ModeB.MomentumTest"),
	0,
	TEXT("MODE B, spec v8 §4. 1: as soon as somebody is holding the Core, throw it three times - ")
	TEXT("standing, running and airborne - and print the launch velocity of each with its pre-v8 ")
	TEXT("baseline. Server only. Disarms itself after one run."),
	ECVF_Default);

/**
 * How long to wait before the armed measurement fires.
 *
 * Not cosmetic. Spec v8 §0 requires the result to be observed FROM A CLIENT, and a listen server
 * grants the Core at kickoff several seconds before a second process has finished joining - so a
 * measurement that fired the moment somebody held it would be measured on the host by construction,
 * with nobody connected to see it. This is what lets the throws land inside the client's session.
 */
static TAutoConsoleVariable<float> CVarModeBMomentumTestDelay(
	TEXT("Trace.ModeB.MomentumTestDelaySeconds"),
	0.f,
	TEXT("MODE B, spec v8 §4. Seconds of world time to wait before Trace.ModeB.MomentumTest fires, so ")
	TEXT("a joining client is in the session to observe it."),
	ECVF_Default);

/**
 * Spec v8 §0: this is a CLIENT-experience pass, so the flight has to be observable FROM THE CLIENT.
 *
 * Runs on every machine, authority or not, and prints that machine's own view. A server-side log of
 * the arc says nothing about whether the client's Core carries the momentum - which is exactly the
 * class of "verified on the host" result section 0 rejects.
 */
static TAutoConsoleVariable<int32> CVarModeBFlightLog(
	TEXT("Trace.ModeB.FlightLog"),
	0,
	TEXT("MODE B. 1: every machine logs its OWN view of the loose Core (position, velocity, speed) at ")
	TEXT("10 Hz while it is in flight. Run it on the client as well as the server."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBBounce(
	TEXT("Trace.ModeB.Bounce"),
	0.35f,
	TEXT("MODE B. Restitution of a loose Core against world geometry. 0 = dead stop, 1 = perfect bounce."),
	ECVF_Default);

// =================================================================================================
// MODE B ONLY — SPEC v6 §4.1, THE CATCH ZONE
//
// "create a small invisible radius around players that acts as a 'catch zone,' so that when the core
// enters that area, it curves towards the player like a magnet. This is intended to make catching
// feel fluid and clean."
//
// Three knobs, and the split between them is the design:
//
//   RADIUS   how far out the magnet reaches. Deliberately LARGER than the pickup radius (120 uu) and
//            not by a little: the pickup radius is "you touched it", the catch radius is "it is
//            coming to you". SPEC v12 §4 cut it 500 -> 450 ("reduce the 'magnet' radius for catching
//            in game mode b by 10%"), which is still about two and a half player widths - close
//            enough that it reads as a catch and not as the Core being stolen from across a lane.
//            THE CUT IS NOT LINEAR IN FEEL. The pull falls off to zero at the boundary, so a smaller
//            radius steepens the falloff everywhere inside it as well as removing the outer ring:
//            the Core is steered less at EVERY distance, not just at the ones that used to be in
//            range. That matters most for a fast Core, which crosses the zone in fewer frames and
//            therefore collects fewer corrections - see the v12 §4 measurement, and raise CURVE
//            rather than RADIUS if the fast case needs help back.
//   CURVE    how hard it curves, in "fractions of the remaining error per second". 6 means the
//            velocity is turned most of the way onto the catcher within a fifth of a second at point
//            blank, and barely at all at the edge of the zone (the pull falls off with distance).
//   LOCKOUT  how long the THROWER alone is excluded. A throw leaves from inside its own thrower's
//            catch zone, so with no lockout every throw would snap straight back into their hands
//            and the mechanic would not exist. Kept separate from CoreThrowerPickupLockoutSeconds
//            because they answer different questions - one is "may I take it", the other is "does it
//            come to me" - and the second wants to be a little longer so the throw is clear of the
//            thrower's body before anything bends it.
//
// EVERY PLAYER, FRIEND OR ENEMY. Spec v6's [ASSUMPTION], and it is the right one: interception is
// the whole risk of throwing in mode B, and a magnet that only helped the receiving team would
// quietly delete it.
//
// SPEED IS NEVER CHANGED, only direction. A magnet that also accelerated the Core would make a caught
// throw arrive faster than a thrown one, and would interact with the ground-turnover rule below in a
// way nobody asked for. Curving alone is what "curves towards the player" says.
// =================================================================================================

// SPEC v12 §4 — "Reduce the 'magnet' radius for catching in game mode b by 10%". 500 -> 450. This is
// the FALLBACK only: UTraceSettings::CoreCatchRadius wins unless this CVar is explicitly set, and
// Config/DefaultGame.ini wins over the header. The default is moved in step with them anyway, because
// a fallback that disagrees with the shipped value is a trap for whoever next reads one and not the
// other. Setting it live (`Trace.ModeB.CatchRadius 500`) is the A/B arm the v12 catch-rate measurement
// used, and is the fastest way to answer "was 450 the thing that made fast Cores uncatchable".
static TAutoConsoleVariable<float> CVarModeBCatchRadius(
	TEXT("Trace.ModeB.CatchRadius"),
	450.f,
	TEXT("MODE B. Radius of the invisible catch zone around every player, uu, measured from the Core ")
	TEXT("to the surface of their capsule. A loose Core inside it curves toward them. v12 §4: 500 -> 450. ")
	TEXT("UTraceSettings::CoreCatchRadius."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBCatchCurve(
	TEXT("Trace.ModeB.CatchCurve"),
	6.f,
	TEXT("MODE B. How hard the catch zone curves the Core, in fractions of the remaining aim error per ")
	TEXT("second at point-blank range. 0 disables the magnet outright. UTraceSettings::CoreCatchCurveStrength."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBCatchThrowerLockout(
	TEXT("Trace.ModeB.CatchThrowerLockout"),
	0.5f,
	TEXT("MODE B. Seconds the THROWER alone is excluded from their own catch zone, so a throw does not ")
	TEXT("curve straight back into the hands it left. UTraceSettings::CoreCatchThrowerLockoutSeconds."),
	ECVF_Default);

// --- SPEC v13 §5. The contested zone. -------------------------------------------------------------
//
// "If the core is within the 'magnet' zone of two or more players from opposite teams, it should go
// to the player closest to the core."
//
// Nearest wins is not the hard part — the selection loop already kept the smallest surface distance.
// What was missing is that the answer has to be STABLE. Two players converging on a falling Core sit
// within a few uu of each other for a good half second, and their distances cross back and forth
// every frame: without a margin the pull alternates between them, and because the steering is an
// exponential approach toward whatever point it is handed, alternating targets average into a Core
// that curves toward NEITHER of them and is caught by nobody. That is a worse outcome than either
// player winning outright, and it is invisible in any log that prints only the winner.
//
// 60 uu is a little under half a capsule radius plus the Core's own: close enough that a genuinely
// nearer player still takes the Core, wide enough that jitter and a stride's worth of closing speed
// do not move it.
static TAutoConsoleVariable<float> CVarModeBCatchContestHysteresis(
	TEXT("Trace.ModeB.CatchContestHysteresis"),
	50.f,
	TEXT("MODE B, spec v13 §5. How much NEARER a challenger must be than the player the magnet is ")
	TEXT("already pulling toward, in uu, before the pull moves to them. Stops two defenders at a ")
	TEXT("similar range from making the Core flicker between them. 0 disables the hysteresis, i.e. ")
	TEXT("strict nearest-wins every frame - the arm that reproduces the flicker. ")
	TEXT("UTraceSettings::CoreCatchContestHysteresisUU."),
	ECVF_Default);

// =================================================================================================
// MODE B ONLY — SPEC v6 §4.2, GROUND CONTACT IS A TURNOVER
//
// "When a team has possession of the core, throws it, and it hits the ground, it should
// automatically turnover to the closest player on the enemy team."
//
// This REPLACES the v4 rule that a thrown Core stayed live on the ground for first contact. A wild
// throw is now punished rather than merely wasted, which is the whole point: it makes the decision
// to throw a real one.
//
// A CVar rather than a settings float because it is a RULE, not a tuning value - and it is here so
// that an A/B of the change itself (the old "stays live" behaviour is one console command away) does
// not need a rebuild. Deliberately NOT in the settings-panel knob list for the same reason: the
// panel is for numbers a designer tunes, not for switching a spec'd rule off.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarModeBGroundTurnover(
	TEXT("Trace.ModeB.GroundTurnover"),
	1,
	TEXT("MODE B, spec v6 §4.2. 1 (default): a THROWN Core that touches the ground turns over to the ")
	TEXT("nearest enemy of the throwing team. 0: the pre-v6 behaviour, where it stays live on the ")
	TEXT("ground for first contact."),
	ECVF_Default);

// =================================================================================================
// MODE B ONLY — SPEC v7 §4, THE SURFACE RULE: TOPS TURN OVER, WALLS BOUNCE
//
// "Sometimes the core gets stuck up top of an object in gamemode b. This should also count as a
// turnover. Walls should not, the core should bounce off those."
//
// This GENERALISES v6 §4.2 rather than adding a second rule beside it. v6 already asked the hit
// NORMAL whether the Core had landed, which is the right question — the arena is generic static
// meshes and there is no actor type to ask — and it already turned the Core over on an upward-facing
// hit. What v7 changes is that the answer must also be asked of a Core that has come to REST, and
// what it adds is that a WALL may never be a resting place.
//
// THE BUG THE USER SAW, and why the v6 test alone could not catch it. bHitTheGround was evaluated
// ONLY on the frame a sweep was blocked, and only against that one impact normal. A Core arriving on
// the lip of a cover block hits the EDGE first, where a swept sphere's normal points out of the
// corner and can be nowhere near vertical; the bounce off it is small, the next sweep's reflected
// speed falls under RestSpeed, and bLooseAtRest goes up on a frame where the normal said "wall".
// From that frame on the integration block is skipped entirely, so no further sweep ever runs and
// the question is never asked again. The Core sits on top of the block until the reset timer —
// exactly "the core gets stuck up top of an object".
//
// So the rule is now asked in TWO places, both of them geometry:
//
//   1. ON CONTACT   the v6 test, unchanged: an upward-facing impact normal is a landing.
//   2. AT REST      a short downward probe under a Core that has stopped. Whatever is holding it up
//                   is the surface the rule is about, whether that is the floor or the roof of a
//                   cover block, and its normal decides. This is the one that fixes the report.
//
// And a wall is now genuinely a bounce and nothing else: a Core whose reflection off a vertical face
// is too slow to matter is NOT declared at rest (it would hang in mid-air against the wall, which is
// the same stuck-forever failure one surface over). It keeps falling, lands on something horizontal,
// and turns over there — which is what "the core should bounce off those" means once you follow the
// Core past the bounce.
// =================================================================================================

static TAutoConsoleVariable<float> CVarModeBSurfaceMaxSlope(
	TEXT("Trace.ModeB.SurfaceMaxSlopeDegrees"),
	45.f,
	TEXT("MODE B, spec v7 §4. How far from straight up a surface normal may lean and still count as a ")
	TEXT("FLOOR OR TOP (a turnover) rather than a WALL (a bounce). 45 degrees is the engine's own ")
	TEXT("default walkable slope, so 'the ground' means the same thing to this rule as it does to the ")
	TEXT("pawn standing on it. 0 = only a perfectly flat surface turns the Core over; 89 = everything ")
	TEXT("but a true vertical does. UTraceSettings::CoreSurfaceMaxSlopeDegrees."),
	ECVF_Default);

// =================================================================================================
// MODE B ONLY — SPEC v13 §8, "THE CORE TURNS OVER IN MID-AIR"
//
// Verbatim: "Sometimes the core is thrown and it turns over before it touches the ground."
//
// [DIAGNOSED] IN THE CODE, before a line of it was changed, and it is hypothesis (a) from the spec —
// the 45-degree rule firing on a GLANCING contact. Until v13 the contact test read, in full:
//
//     bLandedOnSurface = (ContactNormal.Z >= SurfaceUpNormalZ());
//
// i.e. ANY blocked sweep whose normal pointed upward was a landing, with no reference to how the
// Core arrived at it or to whether it stayed. That is exactly right for a Core dropping onto a
// crate and exactly wrong for one crossing the arena at 2200 uu/s whose sphere clips the flat top of
// a cover block or one of the corner cove's horizontal treads on the way past: the normal there is
// straight up, the rule fires, and possession changes while the Core is metres above the floor and
// still travelling. The user is describing the same event from the outside.
//
// THE FIX IS TO ASK WHAT A LANDING IS RATHER THAN WHAT A NORMAL IS. A thrown Core has landed on a
// surface when it has ARRIVED on it, which is two facts and not one:
//
//   1. the surface faces up (unchanged — the v7 §4 rule, which stays exactly as it was), AND
//   2. the Core came DOWN ONTO IT rather than past it. Measured as the angle between the flight and
//      the surface: the component of the velocity along the inward normal, as a fraction of speed,
//      must exceed sin(LandingMinDescentDegrees). A Core dropping onto a crate arrives at 25-90
//      degrees. A Core skimming the same crate mid-flight arrives at 2-8. There is a wide, empty gap
//      between those two populations, which is why an angle works where a speed threshold would not:
//      a graze and a landing can have identical speeds.
//
//   OR, and this is the other half of "an actual landing", the contact simply STOPPED IT — the Core
//   came to rest on an upward-facing surface. That case needs no angle: a Core that is sitting on
//   something has landed on it whatever route it took there, which is also what makes a slow tumble
//   onto a ledge a turnover.
//
// AND THE AT-REST PROBE IS STILL AUTHORITATIVE. Spec v7 §4's "the core gets stuck up top of an
// object" is the same rule seen from the other end, and every Core that grazes something and is
// refused a turnover keeps flying, lands, and turns over there instead — later, lower, and where a
// player watching can see why. Nothing is lost; the turnover moves to the moment it belongs.
//
// (This paragraph used to open "UNCHANGED AND still authoritative". Spec v19 §1.5 changed it: the
// probe's verdict is now ALSO required to pass the visible-support test, because the probe accepts
// anything within 24 uu of the collision sphere and that includes a corner the Core is wedged
// against rather than sitting on. Still authoritative, no longer unchanged — see the §1.5 block
// below.)
//
// HYPOTHESES (b) AND (c) WERE TESTED TOO, and closed by construction rather than by argument:
//   (b) the at-rest probe firing while the Core still has speed — it cannot, bLooseAtRest is only
//       ever set with LooseVelocity zeroed on the same line, but the probe now ASSERTS that and says
//       so if it is ever false, rather than trusting an invariant a future edit could break;
//   (c) a contact on the launch frame, before the Core has cleared the thrower — a landing by
//       CONTACT now requires the Core to have been in the air for LandingMinFlightSeconds. A Core
//       genuinely thrown into the floor at the thrower's feet is not lost by this: it is at rest a
//       frame later and the probe hands it over then.
// =================================================================================================

static TAutoConsoleVariable<float> CVarModeBLandingMinDescent(
	TEXT("Trace.ModeB.LandingMinDescentDegrees"),
	20.f,
	TEXT("MODE B, spec v13 §8. How steeply a moving Core must arrive at an upward-facing surface for the ")
	TEXT("contact to count as a LANDING (and so a turnover) rather than a graze it flies on from. ")
	TEXT("Measured between the flight and the surface plane. A drop onto a crate is 25-90 degrees; a ")
	TEXT("skim across the top of one is 2-8. A contact that STOPS the Core is a landing at any angle. ")
	TEXT("0 restores the pre-v13 'any upward normal is a landing' rule. UTraceSettings::CoreLandingMinDescentDegrees."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBLandingMinFlight(
	TEXT("Trace.ModeB.LandingMinFlightSeconds"),
	0.05f,
	TEXT("MODE B, spec v13 §8. Seconds a throw must have been in the air before a CONTACT can be read as ")
	TEXT("a landing, so a sweep that clips geometry on the launch frame - before the Core has cleared ")
	TEXT("the thrower - cannot hand possession away. The at-rest probe is not gated by this, so a Core ")
	TEXT("genuinely thrown into the floor still turns over a frame later."),
	ECVF_Default);

/**
 * THE A/B ARM, and the reason the §8 reproduction can go red.
 *
 * 1 is the shipped rule. 0 restores the pre-v13 behaviour EXACTLY — any upward-facing contact is a
 * landing — so `Trace.ModeB.LandingRule 0` reproduces the bug on the fixed build and the same
 * harness that reports "0 mid-air turnovers" reports a pile of them. A harness that cannot go red is
 * not evidence, and this switch is what makes this one able to.
 */
static TAutoConsoleVariable<int32> CVarModeBLandingRule(
	TEXT("Trace.ModeB.LandingRule"),
	1,
	TEXT("MODE B, spec v13 §8. 1 (default): a turnover requires an ACTUAL LANDING - the Core arrived on ")
	TEXT("the surface or stopped on it. 0: the pre-v13 rule, where ANY upward-facing contact is a ")
	TEXT("landing, which is the mid-air turnover bug. 0 is the A/B arm for the reproduction."),
	ECVF_Default);

/**
 * SPEC v13 §8. Is the pre-v13 rule armed?
 *
 * TWO WAYS IN, and the command-line one is not a convenience. -ExecCmds is comma-separated and a
 * console variable assignment needs a SPACE ("Trace.ModeB.LandingRule 0"), which means quoting - and
 * quoting inside -ExecCmds has already, on this project, broken a command line into the URL parser
 * and produced a verification that "passed" because its commands never ran. A bare switch cannot do
 * that. The CVar stays for a live A/B from the console.
 */
static bool TraceModeBLegacyLandingRule()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyLanding"));
	return bFromCommandLine || (CVarModeBLandingRule.GetValueOnAnyThread() == 0);
}

static TAutoConsoleVariable<int32> CVarModeBTurnoverLog(
	TEXT("Trace.ModeB.TurnoverLog"),
	0,
	TEXT("MODE B, spec v13 §8. 1: log EVERY contact a loose thrown Core makes and every turnover, with ")
	TEXT("the height above the arena floor, the speed, the surface normal, the arrival angle and the ")
	TEXT("verdict. This is the instrument the mid-air turnover was found with."),
	ECVF_Default);

/** As above: also armable with a bare -TraceTurnoverLog, which needs no quoting inside -ExecCmds. */
static bool TraceModeBTurnoverLogEnabled()
{
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceTurnoverLog"));
	return bFromCommandLine || (CVarModeBTurnoverLog.GetValueOnAnyThread() != 0);
}

/**
 * What the repro counts as "in mid-air": a turnover that fired while the Core was still travelling
 * this fast. REPORTING ONLY — no rule reads it.
 *
 * A speed, not a height, and deliberately: a Core that turns over 400 uu up having STOPPED on the
 * roof of a crate is spec v7 §4 working, and a height threshold would call that the bug. What the
 * user is describing is possession changing while the Core is still flying.
 */
static TAutoConsoleVariable<float> CVarModeBMidAirTurnoverSpeed(
	TEXT("Trace.ModeB.MidAirTurnoverSpeed"),
	800.f,
	TEXT("MODE B, spec v13 §8, REPORTING ONLY. Part one of the mid-air test: the Core must still have ")
	TEXT("been travelling at least this fast when possession changed. No rule reads it."),
	ECVF_Default);

/**
 * Part two, and the part that makes the counter mean what it says.
 *
 * A Core dropped straight onto the floor arrives at 1500 uu/s and has unambiguously LANDED, so speed
 * on its own would call the commonest correct turnover in the game a bug. What the user described is
 * a Core going PAST something: fast AND barely descending. Deliberately a separate number from the
 * rule's own LandingMinDescentDegrees so the counter is not merely a restatement of the rule - it
 * measures the event, and both arms of the A/B measure it the same way.
 */
static TAutoConsoleVariable<float> CVarModeBMidAirTurnoverDegrees(
	TEXT("Trace.ModeB.MidAirTurnoverDegrees"),
	15.f,
	TEXT("MODE B, spec v13 §8, REPORTING ONLY. Part two of the mid-air test: the Core must have met the ")
	TEXT("surface at a shallower angle than this - i.e. it was flying past, not coming down. A turnover ")
	TEXT("that is both fast and shallow is counted as MID-AIR in SurfaceStats. No rule reads it."),
	ECVF_Default);

// =================================================================================================
// MODE B ONLY — SPEC v19 §1.5, "IT MUST VISIBLY TOUCH SOMETHING FIRST"
//
// Verbatim, and the capitals are theirs: "ENSURE the ball does not turnover until it actually
// visibly touches the ground or the top of an obstacle."
//
// THIS IS A RE-REPORT, and that is the whole reason this block exists rather than another turn of
// the v13 §8 dials. v13 §8 fixed the GRAZE — a Core flying PAST the top of a crate — by asking how
// steeply the Core ARRIVED, and Trace.ModeB.TurnoverRepro shows that fix still holding. The sentence
// came back anyway, so the remaining case is one the arrival test cannot see.
//
// [DIAGNOSED] IT IS THE LIP. The flight is a SPHERE SWEEP, and a sphere that clips the top EDGE of a
// block reports the depenetration direction as its normal — which, for a ball resting against a
// corner from slightly above, points UP. Every test the rule had then passes:
//
//     up-facing normal      yes, the corner's depenetration direction is above 45 degrees
//     steep arrival         yes, a Core dropping onto a lip arrives at 40-90 degrees
//     cleared the thrower   yes, it has been in the air for most of a second
//     came to rest          often yes, a corner contact kills most of the speed
//
// ...and the ball is BESIDE the block with several hundred uu of clear air underneath it. Possession
// changes, and then the Core carries on falling to the floor while the player watches. From the
// outside that is exactly the reported sentence, and no further tuning of an ARRIVAL angle can reach
// it, because the thing the rule is wrong about is BELOW the Core, not in front of it.
//
// THE FIX IS TO ASK THE PLAYER'S OWN QUESTION, ONCE, AT THE MOMENT POSSESSION WOULD CHANGE: is the
// orb a player can see actually resting on something? Answered by sweeping a sphere of the RENDERED
// radius straight down (ATraceCore::MeasureVisibleSupportGap) and requiring the gap to be inside
// TurnoverContactSlack. A contact that fails it is not a landing; the Core keeps its downward
// velocity, falls off the lip, and turns over on the floor a moment later — later, lower, and where
// a player watching can see why. Nothing is lost, the turnover moves to the moment it belongs, and
// that is the same shape of fix v13 §8 applied one surface over.
// =================================================================================================

/**
 * How far a Core at rest probes downward to find what is holding it up.
 *
 * The Core is parked 2 uu off whatever it landed on (see the sweep), so this only has to clear that
 * plus the sphere's own radius plus a little slack for a surface it settled into.
 */
static constexpr float TraceModeBRestProbeDepth = 24.f;

/**
 * SPEC v19 §1.5. The RENDERED radius of the orb, in uu — what "visibly" means, numerically.
 *
 * The mesh is /Engine/BasicShapes/Sphere, which is 100 uu across, scaled by TraceCoreTuning::OrbScale
 * (0.40). So the ball a player sees is 20 uu in radius, against a 22 uu collision sphere: the
 * COLLISION IS ALREADY BIGGER THAN THE BALL, which is why the gap has to be measured with the drawn
 * size and not the swept one. Asking the collision sphere "are you touching?" is a question it
 * answers yes to 2 uu before the eye agrees.
 */
static constexpr double TraceModeBVisibleOrbRadius = 100.0 * 0.5 * 0.40;

static TAutoConsoleVariable<float> CVarModeBTurnoverContactSlack(
	TEXT("Trace.ModeB.TurnoverContactSlack"),
	6.f,
	TEXT("MODE B, spec v19 §1.5. How much clear air may be under the VISIBLE orb at the instant "
	     "possession changes, in uu. The Core parks 2 uu off whatever it lands on and its collision "
	     "sphere is 2 uu larger than the drawn one, so a ball genuinely sitting on the floor measures "
	     "about 4; 6 is that plus a frame of slack. Raise it and mid-air turnovers come back."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBTurnoverContactProbe(
	TEXT("Trace.ModeB.TurnoverContactProbe"),
	600.f,
	TEXT("MODE B, spec v19 §1.5. How far below the Core to look for the surface holding it up, in uu. "
	     "Also the number reported as the gap when there is nothing under it at all, so it wants to be "
	     "comfortably taller than the arena's cover (the tallest top face here is ~350 uu)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBTurnoverSettleSeconds(
	TEXT("Trace.ModeB.TurnoverSettleSeconds"),
	0.15f,
	TEXT("MODE B, spec v19 §1.5. How long the Core stays loose ON the surface it landed on before "
	     "possession changes, in seconds. Before this pass it was 0 and the award happened on the same "
	     "TICK as the first contact, so the ball was never drawn touching anything - which is the "
	     "reported bug. 0.15 is about nine frames at 60fps and five at 30, i.e. legible at any frame "
	     "rate, and the ball is bouncing or resting for all of them. 0 restores the pre-v19 instant "
	     "award."),
	ECVF_Default);

/**
 * THE A/B ARM for spec v19 §1.5, and the reason its reproduction can go red.
 *
 * 1 is the shipped rule. 0 removes ONLY the "is the orb actually on top of something" test and
 * leaves every v7 §4 and v13 §8 rule exactly as it was — so Trace.ModeB.GroundedTurnover 0
 * reproduces the lip turnover on the fixed build, and the same harness that reports "0 ungrounded
 * turnovers" reports a pile of them.
 *
 * Trace.ModeB.LandingRule 0 (the v13 arm) implies this one, so that switch still means "everything
 * as it was before any of this" and the §8 A/B is bit-identical to the run it was written for.
 */
static TAutoConsoleVariable<int32> CVarModeBGroundedTurnover(
	TEXT("Trace.ModeB.GroundedTurnover"),
	1,
	TEXT("MODE B, spec v19 §1.5. 1 (default): a turnover additionally requires the VISIBLE orb to be "
	     "resting on the surface AND to have been drawn there for Trace.ModeB.TurnoverSettleSeconds. "
	     "0: removes both, which is the pre-v19 build exactly and the A/B arm the reproduction must go "
	     "RED on. Never ship 0."),
	ECVF_Default);

/** As the v13 switches: also armable with a bare -TraceLegacyGroundedTurnover, which needs no quoting. */
static bool TraceModeBLegacyGroundedRule()
{
	static const bool bFromCommandLine =
		FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyGroundedTurnover"));

	// The v13 arm implies this one — see the CVar's comment.
	return bFromCommandLine
		|| (CVarModeBGroundedTurnover.GetValueOnAnyThread() == 0)
		|| TraceModeBLegacyLandingRule();
}

/**
 * How far above the arena floor a surface has to be before the log and the tallies call it "the top
 * of an object" rather than "the ground".
 *
 * REPORTING ONLY. The rule itself does not know this number exists and must not: spec v7 §4 is
 * explicit that landing on top of a structure and landing on the floor are the same event. This
 * exists so a verification run can state how many of each it actually produced, which is the only
 * way to show the generalised path was exercised and not just the old one.
 */
static constexpr double TraceModeBElevatedSurfaceZ = 60.0;

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

// =================================================================================================
// SPEC v25 §2 — THE TURNOVER WINDOW, THE PULL, AND THE BEAM
//
// Verbatim: "Players from the team who dropped the core are locked out of picking it up for 5
// seconds" / "Pulling the core requires holding down right click while hovering mouse over it (with
// line of sight) for .3seconds" / "the beam of light coming from the core should change colors to
// the opposite team and be larger for the 5 seconds".
//
// FOUR NUMBERS, and the note names three of them. The fourth (how much larger) is an [ASSUMPTION]
// and is deliberately a MULTIPLIER rather than a width — Demo 21's standing rule: "larger" is a
// statement ABOUT THE NORMAL BEAM, so it has to move when the normal beam moves. TraceCoreTuning::
// BeaconWidth is the base; this scales it.
//
// THERE IS NO PULL-SPEED KNOB, and that absence is the point. "Once the core is pulled, it travels
// towards the player who completed the pull first at full core thrown velocity" names an existing
// constant, so the delivery reads ATraceCore::GetThrowSpeed() — the same accessor ThrowFromHolder
// launches with, weight model included. A second "pull speed" here would be a duplicate that stops
// agreeing with the throw the first time either is retuned.
//
// THE HOVER TEST'S TWO KNOBS mirror the pass's (UTraceSettings::PassAimConeDegrees / PassAimSlack)
// for the same reason those exist: a 20 uu orb subtends under a fifth of a degree at 8000 uu, so a
// pure ray test would be unusable at range, and a pure cone test collapses to nothing point-blank.
// Either one passing is a hover.
// =================================================================================================

static TAutoConsoleVariable<float> CVarModeBTurnoverLockout(
	TEXT("Trace.ModeB.TurnoverLockoutSeconds"),
	5.f,
	TEXT("SPEC v25 §2, GOALS MODE. Seconds the team that DROPPED the Core is locked out of it after a ")
	TEXT("turnover. For that window only the opposing team may pull or pick up; afterwards nobody pulls ")
	TEXT("and either team may take it by touch. 0 disables the whole window (the pre-v25 rule is a ")
	TEXT("different thing and is NOT restored by this - see Trace.ModeB.TurnoverPull). ")
	TEXT("UTraceSettings::CoreTurnoverLockoutSeconds."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBPullHold(
	TEXT("Trace.ModeB.PullHoldSeconds"),
	0.3f,
	TEXT("SPEC v25 §2, GOALS MODE. Seconds of CONTINUOUS right-mouse-plus-hover-plus-line-of-sight that ")
	TEXT("complete a pull. Losing any of the three cancels the fill outright; it does not pause. ")
	TEXT("UTraceSettings::CorePullHoldSeconds."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBPullAimCone(
	TEXT("Trace.ModeB.PullAimConeDegrees"),
	4.f,
	TEXT("SPEC v25 §2. Half-angle of the cone that counts as 'hovering the mouse over the core' at ")
	TEXT("range. Either this OR the slack below is enough. UTraceSettings::CorePullAimConeDegrees."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBPullAimSlack(
	TEXT("Trace.ModeB.PullAimSlackUU"),
	60.f,
	TEXT("SPEC v25 §2. How far, in uu, the aim ray may miss the orb's SURFACE and still count as a ")
	TEXT("hover. Added to the orb's drawn radius, so it is forgiveness rather than a second radius. ")
	TEXT("UTraceSettings::CorePullAimSlackUU."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBPullMaxRange(
	TEXT("Trace.ModeB.PullMaxRangeUU"),
	0.f,
	TEXT("SPEC v25 §2. Optional ceiling on how far away a pull may be started, uu. 0 = no limit, which ")
	TEXT("is the shipped value because the note states no range: a 20 uu orb under a crosshair WITH ")
	TEXT("line of sight is already its own range limit. UTraceSettings::CorePullMaxRangeUU."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarModeBTurnoverBeamScale(
	TEXT("Trace.ModeB.TurnoverBeamScale"),
	2.2f,
	TEXT("SPEC v25 §2/§3. How much LARGER the Core's beam is during the turnover window, as a ")
	TEXT("MULTIPLIER of its normal width - not a width, so a retune of the normal beam carries. ")
	TEXT("1 = no change. UTraceSettings::CoreTurnoverBeamScale."),
	ECVF_Default);

/**
 * SPEC v25 §2's A/B ARM. 0 restores the pre-v25 turnover EXACTLY: the landing hands the Core straight
 * to the nearest enemy, with no window, no pull and no beam change.
 *
 * It exists for the same reason Trace.ModeB.LandingRule and Trace.ModeB.GroundedTurnover do — a rule
 * that cannot be switched off cannot be shown to be the thing that changed, and every one of this
 * pass's red arms is measured against it.
 */
static TAutoConsoleVariable<int32> CVarModeBTurnoverPull(
	TEXT("Trace.ModeB.TurnoverPull"),
	1,
	TEXT("SPEC v25 §2. 1 (default): a turnover REGISTERS and the Core stays where it landed, with the ")
	TEXT("5 s lockout and the opposing team's pull. 0: the pre-v25 rule - the landing grants the Core ")
	TEXT("straight to the nearest enemy. The A/B arm for every red arm in this section."),
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

	// --- Spec v8 §4. NOT scaled by the weight model, and that is deliberate: MassScale describes how
	// hard the Core is to THROW, and it already reduces the impulse. The thrower's momentum is
	// transferred by carrying the Core, not by launching it, so a heavier Core carries the same
	// velocity out of the same jump. Upper bound 2 rather than 1 so an over-inheriting "throw it like
	// a Rocket League shot" variant is reachable from the console without a rebuild.
	float ThrowInheritance()  { return Resolve(TEXT("CoreThrowVelocityInheritance"),      CVarModeBThrowInheritance, 0.f,   2.f); }

	// --- Spec v13 §6, the charge. NOT scaled by the weight model either, and for the same reason: the
	// weight already reduced the impulse the charge is a fraction OF, so a heavier Core is thrown
	// slower at every charge level without the charge having to know the Core has a weight at all.
	// Lower bound on the time is 0.01 rather than 0 so a misconfigured 0 cannot divide by zero; at
	// 0.01 the charge is effectively instant-full, which is the closest thing to "switched off".
	// THE PROPERTY NAMES ARE UTraceSettings' OWN, NOT NAMES INVENTED HERE. The settings page declares
	// this pass's four charge knobs as CoreThrowChargeSeconds / CoreThrowChargeFloorFraction /
	// bCoreThrowChargeClampsAtFull / CoreThrowChargeMaxFraction, and Config/DefaultGame.ini ships
	// values for all four - so binding to anything else would leave four live sliders driving nothing
	// while this file quietly used its own console defaults. The ini wins, which is the project's
	// standing rule, and it can only win if the name matches exactly.
	float ThrowChargeSeconds() { return Resolve(TEXT("CoreThrowChargeSeconds"),      CVarModeBThrowChargeSeconds, 0.01f, 10.f); }
	float ThrowChargeFloor()   { return Resolve(TEXT("CoreThrowChargeFloorFraction"), CVarModeBThrowChargeFloor,  0.f,   1.f); }

	/**
	 * The ceiling on the NORMALISED HOLD (t = held / charge time), used only when the clamp is off.
	 *
	 * NOTE WHAT THIS IS AND IS NOT, because the two readings differ once it is raised: it caps t, not
	 * the resulting power. Power is always Floor + (1 - Floor) * t, so at MaxFraction 1.5 a 1.5 s hold
	 * gives 0.15 + 0.85 * 1.5 = 1.43x, not 1.5x. That is UTraceSettings' documented arithmetic and the
	 * ini's, and it is followed here rather than reinterpreted - two definitions of "max charge" that
	 * disagree by 5% is precisely the kind of drift nobody finds.
	 */
	float ThrowChargeMaxFraction() { return Resolve(TEXT("CoreThrowChargeMaxFraction"), CVarModeBThrowChargeMax, 1.f, 4.f); }

	bool  ThrowChargeClamps()  { return ResolveBool(TEXT("bCoreThrowChargeClampsAtFull"), CVarModeBThrowChargeClamp); }

	// --- Spec v13 §5, the contested magnet. UTraceSettings::CoreCatchContestHysteresisUU, 50 uu in
	// Config/DefaultGame.ini - and the CVar default below is moved to 50 in step with it, because a
	// fallback that disagrees with the shipped value is a trap for whoever reads one and not the other.
	float CatchContestHysteresis() { return Resolve(TEXT("CoreCatchContestHysteresisUU"), CVarModeBCatchContestHysteresis, 0.f, 1000.f); }

	// --- Spec v13 §8, the landing rule. Degrees for the same reason the slope threshold is: it is the
	// unit the question is asked in ("how steeply did it come down"), and the sine the test needs is
	// derived once, here.
	float LandingMinDescentDegrees() { return Resolve(TEXT("CoreLandingMinDescentDegrees"), CVarModeBLandingMinDescent, 0.f, 89.f); }

	/** sin(LandingMinDescentDegrees): the minimum |velocity . -normal| / speed of an arrival. */
	float LandingMinDescentSin() { return FMath::Sin(FMath::DegreesToRadians(LandingMinDescentDegrees())); }

	float LandingMinFlightSeconds() { return FMath::Clamp(CVarModeBLandingMinFlight.GetValueOnAnyThread(), 0.f, 2.f); }

	// --- Spec v25 §2, the turnover window and the pull. Upper bounds are sanity rails, not design:
	// a 60 s lockout would be a dead match and a 10 s hold is not a mechanic anybody would use, but
	// both are reachable so a playtest can find the edge of the range rather than the edge of a clamp.
	float TurnoverLockoutSeconds() { return Resolve(TEXT("CoreTurnoverLockoutSeconds"), CVarModeBTurnoverLockout,   0.f, 60.f); }
	float PullHoldSeconds()        { return Resolve(TEXT("CorePullHoldSeconds"),        CVarModeBPullHold,          0.f, 10.f); }
	float PullAimConeDegrees()     { return Resolve(TEXT("CorePullAimConeDegrees"),     CVarModeBPullAimCone,       0.f, 45.f); }
	float PullAimSlackUU()         { return Resolve(TEXT("CorePullAimSlackUU"),         CVarModeBPullAimSlack,      0.f, 400.f); }
	float PullMaxRangeUU()         { return Resolve(TEXT("CorePullMaxRangeUU"),         CVarModeBPullMaxRange,      0.f, 60000.f); }

	/** Spec v25 §2/§3. A MULTIPLIER of TraceCoreTuning::BeaconWidth, never a width. See the block above. */
	float TurnoverBeamScale()      { return Resolve(TEXT("CoreTurnoverBeamScale"),      CVarModeBTurnoverBeamScale, 0.1f, 10.f); }

	/** Spec v25 §2. False restores the pre-v25 "landing hands it straight to the nearest enemy" rule. */
	bool  TurnoverPullEnabled()    { return CVarModeBTurnoverPull.GetValueOnAnyThread() != 0; }

	// --- Spec v7 §4, the surface rule. Kept in DEGREES because that is the unit the spec states it in
	// ("a normal within ~45 degrees of straight up") and the unit a designer can picture; the cosine
	// the test actually needs is derived once, here, so no caller has to remember which one it holds.
	float SurfaceMaxSlopeDegrees() { return Resolve(TEXT("CoreSurfaceMaxSlopeDegrees"), CVarModeBSurfaceMaxSlope, 0.f, 89.f); }

	/** cos(SurfaceMaxSlopeDegrees): the minimum normal Z of a surface the Core may come to rest on. */
	float SurfaceUpNormalZ()  { return FMath::Cos(FMath::DegreesToRadians(SurfaceMaxSlopeDegrees())); }

	// --- Spec v6 §4.1, the catch zone. Upper bounds chosen so a live retune cannot break the game:
	// a 3000 uu magnet would make the Core uncatchable-by-anybody-else and a curve above ~30 snaps
	// rather than curves, which is the thing the note explicitly did not ask for.
	float CatchRadius()          { return Resolve(TEXT("CoreCatchRadius"),                 CVarModeBCatchRadius,      0.f,   3000.f); }
	float CatchCurveStrength()   { return Resolve(TEXT("CoreCatchCurveStrength"),          CVarModeBCatchCurve,       0.f,   30.f); }
	float CatchThrowerLockout()  { return Resolve(TEXT("CoreCatchThrowerLockoutSeconds"),  CVarModeBCatchThrowerLockout, 0.f, 5.f); }

	/**
	 * THE MAGNET ITSELF: one frame of the catch zone's steering, as a pure function.
	 *
	 * It lives here rather than inline in ATraceCore::ServerApplyCatchZone for one reason, and it is
	 * a testing reason. Spec v12 §4 cuts the radius by 10% and asks for the catch rate BEFORE and
	 * AFTER, which means something has to sweep the miss-distance / Core-speed space and count. A
	 * harness that re-implemented these four lines would be measuring the harness, and this project
	 * has already had a verification "pass" that never ran the thing it claimed to test. So the
	 * shipped path and Trace.ModeB.CatchTest call THE SAME FUNCTION: if this maths is wrong, the
	 * measurement is wrong in exactly the same way and the numbers stop agreeing with the game.
	 *
	 * DIRECTION ONLY. The returned vector has the SAME LENGTH as Velocity — see the tuning block
	 * above for why a magnet that also accelerated the Core is a different, unasked-for mechanic.
	 *
	 * @param SurfaceDistance  distance from the Core to the catcher's capsule SURFACE, not its centre.
	 * @return the steered velocity, or Velocity unchanged when there is nothing to steer.
	 */
	FVector SteerTowardCatchPoint(const FVector& CoreLocation, const FVector& Velocity,
		const FVector& CatchPoint, double SurfaceDistance, float Radius, float Curve, float DeltaSeconds)
	{
		const double Speed = Velocity.Size();
		const FVector ToTarget = CatchPoint - CoreLocation;

		if (Speed < 1.0 || Radius <= 0.f || Curve <= 0.f || ToTarget.IsNearlyZero() || DeltaSeconds <= 0.f)
		{
			return Velocity;
		}

		// Falloff: full strength on the capsule, nothing at the edge of the zone. This is what makes
		// the zone read as a catch rather than as a wall of magnetism you can feel the boundary of.
		//
		// IT IS ALSO WHY SPEC v12 §4's 10% IS NOT A 10% CHANGE IN FEEL. Falloff is a function of
		// distance/RADIUS, so shrinking the radius does not merely clip the outer ring off the zone —
		// at any fixed distance the ratio rises and the pull WEAKENS. The Core is steered less
		// everywhere inside the zone, not only in the ring that left it.
		const double Falloff = 1.0 - FMath::Clamp(SurfaceDistance / static_cast<double>(Radius), 0.0, 1.0);

		// Exponential approach, so the curve is identical at 30 fps and at 240. Alpha is the fraction
		// of the remaining aim error removed this frame.
		const double Alpha = 1.0 - FMath::Exp(-static_cast<double>(Curve) * Falloff * static_cast<double>(DeltaSeconds));

		// Renormalised to the speed it already had.
		const FVector Steered = FMath::Lerp(Velocity / Speed, ToTarget.GetSafeNormal(), Alpha).GetSafeNormal();
		return Steered * Speed;
	}

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

	/**
	 * SPEC v13 §5. THE CONTEST, as a pure function: which contender does the Core curve toward?
	 *
	 * NEAREST WINS. The incumbent keeps the pull unless a challenger is nearer BY MORE THAN
	 * @p Hysteresis, which is what stops a near-tie oscillating (see ServerApplyCatchZone's header
	 * comment for why an oscillating magnet catches for nobody). Exact ties break on StableKey, never
	 * on array order.
	 *
	 * It lives here, taking distances rather than pawns, for the same reason SteerTowardCatchPoint
	 * does: Trace.ModeB.ContestTest drives THIS function with scripted distances - including the
	 * crossing-over sequence that produces the flicker - so the property being claimed ("stable, and
	 * nearest still wins") is asserted against the code the game runs and not against a copy of it.
	 *
	 * @param bOutHysteresisHeld  set when a strictly nearer challenger was refused by the margin.
	 * @return index into @p Contenders, or INDEX_NONE when it is empty.
	 */
	int32 PickContestedCatcher(const TArray<FCatchContender>& Contenders, float Hysteresis,
		bool* bOutHysteresisHeld = nullptr)
	{
		if (bOutHysteresisHeld != nullptr)
		{
			*bOutHysteresisHeld = false;
		}

		int32 Nearest = INDEX_NONE;
		int32 Incumbent = INDEX_NONE;

		for (int32 Index = 0; Index < Contenders.Num(); ++Index)
		{
			const FCatchContender& Candidate = Contenders[Index];

			if (Candidate.bIncumbent && Incumbent == INDEX_NONE)
			{
				Incumbent = Index;
			}

			if (Nearest == INDEX_NONE)
			{
				Nearest = Index;
				continue;
			}

			const FCatchContender& Best = Contenders[Nearest];
			if (Candidate.SurfaceDistance < Best.SurfaceDistance
				|| (Candidate.SurfaceDistance == Best.SurfaceDistance && Candidate.StableKey < Best.StableKey))
			{
				Nearest = Index;
			}
		}

		if (Nearest == INDEX_NONE || Incumbent == INDEX_NONE || Nearest == Incumbent || Hysteresis <= 0.f)
		{
			return Nearest;
		}

		// The challenger is nearer — but is it nearer ENOUGH to be worth moving the pull? The margin is
		// applied once, at the moment of the switch, and not carried: as soon as the pull moves, the new
		// target is the incumbent and the old one needs the same margin to take it back. That is what
		// makes this hysteresis rather than a permanent handicap.
		const double Margin = Contenders[Incumbent].SurfaceDistance - Contenders[Nearest].SurfaceDistance;
		if (Margin <= static_cast<double>(Hysteresis))
		{
			if (bOutHysteresisHeld != nullptr)
			{
				*bOutHysteresisHeld = true;
			}
			return Incumbent;
		}

		return Nearest;
	}

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
			// Spec v8 §4. DECLARED on UTraceSettings and shipped in Config/DefaultGame.ini. It was
			// listed here while it was still missing; it stays listed now that it is not, because the
			// job of this list is not "to-do" but "these names must keep matching" — the entry is what
			// would say so out loud if either side were ever renamed.
			TEXT("CoreThrowVelocityInheritance"),
			// Spec v6 §4.1. All three declared and ini-backed, same standing-check reason as above.
			TEXT("CoreCatchRadius"),
			TEXT("CoreCatchCurveStrength"),
			TEXT("CoreCatchThrowerLockoutSeconds"),
			// Spec v7 §4. Declared and ini-backed.
			TEXT("CoreSurfaceMaxSlopeDegrees"),
			// Spec v13 §6's charge and §5's contest margin. UTraceSettings DECLARES ALL OF THESE and
			// Config/DefaultGame.ini ships values for them, so these entries are a standing check that
			// the names still match rather than a to-do: rename either side and this list is what says
			// so out loud, on the frame mode B starts, instead of a slider quietly driving nothing.
			// (The clamp is a BOOL and is checked separately below - FindFProperty<FFloatProperty>
			// returns null for a bool, so listing it here would report a bound knob as missing.)
			TEXT("CoreThrowChargeSeconds"),
			TEXT("CoreThrowChargeFloorFraction"),
			TEXT("CoreThrowChargeMaxFraction"),
			TEXT("CoreCatchContestHysteresisUU"),
			// Spec v13 §8's landing angle. It WAS the one knob this pass added with no property behind
			// it - this list is how that was found - and integration declared it on UTraceSettings with
			// a value in Config/DefaultGame.ini. Trace.ModeB.LandingMinDescentDegrees still overrides.
			TEXT("CoreLandingMinDescentDegrees"),
			// Spec v25 §2's six. Declared on UTraceSettings and shipped in Config/DefaultGame.ini; the
			// entries are the standing check that the names still match, which is the only thing that
			// stops a live slider on the settings page from driving nothing at all.
			TEXT("CoreTurnoverLockoutSeconds"),
			TEXT("CorePullHoldSeconds"),
			TEXT("CorePullAimConeDegrees"),
			TEXT("CorePullAimSlackUU"),
			TEXT("CorePullMaxRangeUU"),
			TEXT("CoreTurnoverBeamScale"),
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

		// The one BOOL knob (spec v13 §6's clamp), checked with the right property type.
		const bool bClampBound =
			FindFProperty<FBoolProperty>(UTraceSettings::StaticClass(), FName(TEXT("bCoreThrowChargeClampsAtFull"))) != nullptr;
		if (bClampBound)
		{
			++BoundCount;
		}
		else
		{
			if (!DeadNames.IsEmpty())
			{
				DeadNames += TEXT(", ");
			}
			DeadNames += TEXT("bCoreThrowChargeClampsAtFull (bool)");
		}

		const int32 TotalCount = static_cast<int32>(UE_ARRAY_COUNT(KnobNames)) + 1;

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
// SPEC v10 §10 — "the core randomly flies across a players screen, client-side ... after a goal"
//
// THE MEASUREMENT CAME FIRST, AND IT CONTRADICTED THE SPEC'S LEAD.
//
// The lead was: every reset calls SetActorLocation(..., TeleportPhysics), which is right on the
// server, but the client INTERPOLATES replicated movement and therefore slides the Core across the
// map. Read the constructor before believing that: this actor calls SetReplicateMovement(false).
// ReplicatedMovement is never sent for it, PostNetReceiveLocationAndRotation is never called on it,
// and there is no interpolator anywhere in its path. The one transform channel it has is its
// ATTACHMENT to the holder.
//
// Which is the actual defect. A server reset moved the actor and told the clients NOTHING:
//
//   server                                   client
//   ------                                   ------
//   ReleaseHolder(): Carrier = nullptr       OnRep_Carrier -> ApplyAttachment
//   DetachFromActor(KeepWorldTransform)      DetachFromActor(KeepWorldTransform)
//   SetActorLocation(home)                   ... nothing. The Core stays in the endzone.
//   [KickoffDelaySeconds pass]               ... still in the endzone.
//   GrantTo(next holder): AttachToActor      OnRep_Carrier -> AttachToActor: ONE-FRAME JUMP
//                                            from the endzone to the far side of the field.
//
// So the client's Core is stranded at the far end of the pitch for the whole kickoff delay and then
// crosses the entire map in a single frame, off an always-relevant beacon that is deliberately
// visible from anywhere. That is the reported symptom, and "after a goal" fits it for the reason the
// spec gave — a goal is the longest teleport there is.
//
// Two things are wrong and both are fixed:
//   1. The client was never told. -> ServerTeleport()/TeleportSerial, an explicit teleport channel.
//   2. The client never applied the rule it already owns. -> PlaceHolderlessCore(), the client half
//      of the constructor's "holderless => every machine computes the home location identically",
//      which previously existed only after Tick's `!HasAuthority()` early return.
//
// Trace.Core.TeleportAudit is the instrument that established the above and is what the fix is
// judged by. It runs on EVERY machine (spec v8 §0: a number measured on the host cannot see this)
// and reports one line per possession reset, classifying it as SNAP, SLIDE or STRANDED-then-JUMP.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarCoreTeleportAudit(
	TEXT("Trace.Core.TeleportAudit"),
	0,
	TEXT("Trace: audit the Core's own transform across possession resets, on whatever machine it is set on.\n")
	TEXT("Prints one summary per reset: path length, frames spent moving, worst distance from home.\n")
	TEXT("0 = off (default), 1 = on."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCoreTeleportAuditJump(
	TEXT("Trace.Core.TeleportAuditJump"),
	400.f,
	TEXT("Trace: single-frame movement, in uu, above which Trace.Core.TeleportAudit logs a JUMP line."),
	ECVF_Default);

/**
 * THE A/B SWITCH, AND IT IS WHAT MAKES THE RED ARM OF THE TEST HONEST.
 *
 * 0 restores the pre-v10 behaviour EXACTLY, in the same binary: the server still moves the actor with
 * SetActorLocation(TeleportPhysics) and still tells the clients nothing, and a client still declines
 * to apply the holderless placement rule it does not know it owns. That is how the symptom was
 * reproduced failing before the fix went in, on a real client at 40 ms, rather than argued about.
 *
 * Kept rather than deleted for the same reason Trace.Trail.ClearOnPossessionLoss was kept: the next
 * person to doubt this diagnosis can re-run both arms without a rebuild.
 */
static TAutoConsoleVariable<int32> CVarCoreTeleportFix(
	TEXT("Trace.Core.TeleportFix"),
	1,
	TEXT("Trace (spec v10 §10): 1 = replicate Core teleports and let clients place a holderless Core (default).\n")
	TEXT("0 = the pre-v10 behaviour, where a reset moved the Core on the server only. For A/B only."),
	ECVF_Default);

/** How many post-goal resets Trace.Core.GoalRepro should fire on its own. 0 = manual only. */
static TAutoConsoleVariable<int32> CVarCoreGoalReproRuns(
	TEXT("Trace.Core.GoalReproRuns"),
	0,
	TEXT("Trace (spec v10 §10): fire this many staged post-goal resets automatically, once a REMOTE ")
	TEXT("client's pawn exists. 0 = off; drive it by hand with Trace.Core.GoalRepro instead."),
	ECVF_Default);

/**
 * Floor, not a stopwatch — the same lesson spec v8 §0 taught the momentum test. The run is released
 * by a remote client's pawn EXISTING; this only stops it firing into a half-loaded match.
 */
static TAutoConsoleVariable<float> CVarCoreGoalReproDelay(
	TEXT("Trace.Core.GoalReproDelay"),
	25.f,
	TEXT("Trace: seconds before the first automatic Trace.Core.GoalReproRuns reset."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCoreGoalReproInterval(
	TEXT("Trace.Core.GoalReproInterval"),
	12.f,
	TEXT("Trace: seconds between automatic resets. Must exceed the staging delay + the kickoff delay ")
	TEXT("+ Trace.Core.TeleportAuditWindow, or one audit window swallows the next reset."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarCoreTeleportAuditWindow(
	TEXT("Trace.Core.TeleportAuditWindow"),
	3.0f,
	TEXT("Trace: seconds to watch the Core for after a possession reset before printing the audit summary.\n")
	TEXT("Must outlast the kickoff delay or the summary closes before the Core is re-granted."),
	ECVF_Default);

/**
 * Trace.Core.GoalRepro — server. Stages the exact geometry of a goal and then fires the exact reset
 * a goal fires, so §10 can be reproduced on demand instead of waited for.
 *
 * Two stages, because the symptom is about what the CLIENT does with a reset and the client has to
 * have SEEN the Core at the far end first: teleport the holder deep into the attacking end, wait out
 * a staging delay (well over the 40 ms the client is being tested at), then call KickoffTo() — which
 * is precisely and only what ATraceGameMode::NotifyScored does to this actor on a score.
 */
static void TraceCoreGoalReproCommand(UWorld* World)
{
	ATraceCore* Core = ATraceCore::Get(World);
	if (Core == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[CoreAudit] Trace.Core.GoalRepro: no Core in this world."));
		return;
	}
	Core->RequestGoalRepro();
}

static FAutoConsoleCommand GTraceCoreGoalReproCmd(
	TEXT("Trace.Core.GoalRepro"),
	TEXT("Trace: stage a carrier at the far end of the field and then fire the post-goal reset (spec v10 §10)."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&TraceCoreGoalReproCommand));


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

	// SPEC v10 §10. The teleport channel. Two properties rather than one struct so that the ordering
	// this depends on is the plain one the engine already guarantees: a bunch's property DATA is all
	// applied before ANY of that bunch's RepNotifies run, so by the time OnRep_TeleportSerial fires,
	// TeleportLocation — and Carrier, and bLoose — are already the new values.
	DOREPLIFETIME(ATraceCore, TeleportLocation);
	DOREPLIFETIME(ATraceCore, TeleportSerial);

	// SPEC v25 §2. The turnover is server truth and the client only ever SHOWS it, so all four facts
	// are plain replicated properties with no client-side counterpart to disagree with. PullHolds is
	// the one the ring reads (spec §3: "real server-side progress, not a local guess").
	DOREPLIFETIME(ATraceCore, TurnoverLockoutTeam);
	DOREPLIFETIME(ATraceCore, TurnoverStartServerTime);
	DOREPLIFETIME(ATraceCore, PullHolds);
	DOREPLIFETIME(ATraceCore, PullWinner);
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
	//
	// SPEC v25 §2 joins the same reconciliation, and it is the one member of it that can change with
	// NO packet arriving at all: the turnover window ends when its clock runs out, on every machine
	// independently. A client that only reacted to OnReps would keep drawing an enlarged, wrong-colour
	// beam until the next possession event.
	if (!bAppliedEver || AppliedHolder.Get() != Carrier || bAppliedPassActive != bPassActive
		|| bAppliedLoose != bLoose || bAppliedTurnoverActive != IsTurnoverActive())
	{
		ApplyAttachment();
		UpdateVisuals();
	}

	// Spec v8 §0/§4. Every machine's own view, so "the Core carries the momentum" is a claim that can
	// be checked on the CLIENT rather than inferred from the server's copy. Costs one int compare.
	TickFlightLog();

	// Spec v10 §10, same reasoning and the same placement: ahead of the authority split, because the
	// machine the bug is ON is the one that is not the server. Costs one int compare when disarmed.
	TickTeleportAudit();

	// SPEC v13 §6. THE LOCAL CHARGE METER IS CLEARED WHEN THE CORE IS NOT IN LOCAL HANDS, ON EVERY
	// MACHINE, AND THIS IS THE CLIENT'S ONLY WAY OUT.
	//
	// The server clears its own charge in ReleaseHolder, which is the funnel every possession change
	// goes through - but ReleaseHolder returns immediately on a non-authority machine, so a CLIENT that
	// was killed, intercepted or half-timed with the button held would never see its own meter stop.
	// It would sit at full charge, on screen, for a Core the player no longer has: the exact class of
	// "predicted state with no server correction" the pass prediction is careful to avoid.
	//
	// The test is possession, not the button, because possession is the thing that is replicated. A
	// player who is still holding the Core and still holding the button keeps their meter; anyone else
	// loses it on the frame the possession change arrives.
	if (bLocalThrowCharging)
	{
		const bool bStillMine = IsValid(Carrier) && Carrier->IsAlive() && !bLoose
			&& Carrier->IsLocallyControlled()
			&& Cast<APlayerController>(Carrier->GetController()) != nullptr;

		if (!bStillMine)
		{
			bLocalThrowCharging = false;
		}
	}

	if (!HasAuthority())
	{
		// MODE B, CLIENTS. Dead-reckon the loose Core between net updates so it flies along its arc
		// instead of stepping along it at the net update rate. Purely presentational: LooseLocation
		// and LooseVelocity are both overwritten by the next replicated value, and no client ever
		// decides a pickup or a goal.
		if (bLoose)
		{
			// SPEC v8 §0/§4. THE CLIENT INTEGRATES GRAVITY TOO, with the Core's own gravity - the same
			// number ServerTickLooseCore uses, asked of the same accessor. Extrapolating at a CONSTANT
			// velocity between net updates draws a straight line where the server is drawing a
			// parabola, so the client's Core rode above the true arc and was yanked back down on every
			// update. That is a client-only artefact by construction (the host integrates the real
			// thing) and it is worst exactly where spec v8 §4 is looking: a jumping throw, whose Z is
			// the fastest-changing component there is. Gated on a moving Core so a Core at rest, whose
			// replicated velocity is zero, is not quietly sunk through the floor between updates.
			if (!FVector(LooseVelocity).IsNearlyZero())
			{
				LooseVelocity = FVector(LooseVelocity)
					+ FVector(0.0, 0.0, static_cast<double>(GetThrowGravityZ(GetWorld())) * DeltaSeconds);
			}

			LooseLocation = LooseLocation + LooseVelocity * DeltaSeconds;
			SetActorLocation(LooseLocation, false, nullptr, ETeleportType::TeleportPhysics);
			return;
		}

		// SPEC v10 §10. THE CLIENT HALF OF "holderless => every machine computes the home location
		// identically", which the constructor has claimed since movement replication was switched off
		// and which nothing has ever implemented on a client.
		//
		// The server's version of this line is step 3 below — and step 3 is on the far side of this
		// early return, so it has only ever run on the authority. A client that saw a possession end
		// therefore detached the Core with KeepWorldTransform and left it exactly where the old holder
		// died or scored, for the entire kickoff delay, until the next GrantTo attached it and it
		// crossed the map in one frame. See the §10 block at the top of this file.
		//
		// Idempotent and the same tolerance the server uses, so in the steady state (holderless, at
		// home) this is one distance compare per frame and no writes.
		PlaceHolderlessCore();
		return;
	}

	// Spec v8 §4's measurement, armed from the command line. Held until there is a live holder AND,
	// per spec v8 §0, a joining client actually in the session to observe it.
	//
	// SPEC v8 §0 IS THE SECOND HALF OF THE GATE, AND IT IS NOT A STOPWATCH. The first version of this
	// fired on a fixed 28 s delay, and the second process was still loading plugins when it did - so
	// the whole measurement was taken with nobody connected, which is the exact "measured on the host"
	// failure section 0 exists to stop. The delay is now only a FLOOR; what actually releases the
	// measurement is a remote client's pawn existing. The four-minute timeout is a last resort, and
	// RunThrowMomentumTest SAYS SO in the log rather than quietly producing a host-side number that
	// reads like a client-side one.
	//
	// One condition, not an early return: the rest of this tick is the Core's own housekeeping (the
	// queued fallback, the holder-validity check, the pending grant) and skipping it for minutes while
	// a diagnostic waits would break the match around the measurement.
	if (!bMomentumTestRun && CVarModeBMomentumTest.GetValueOnGameThread() != 0 && IsModeB()
		&& GetWorld()->GetTimeSeconds() >= static_cast<double>(CVarModeBMomentumTestDelay.GetValueOnGameThread())
		&& IsValid(Carrier) && Carrier->IsAlive() && !bLoose && !bCoreStateLocked
		&& (HasRemoteClientPawn()
			|| GetWorld()->GetTimeSeconds()
				>= static_cast<double>(CVarModeBMomentumTestDelay.GetValueOnGameThread()) + 240.0))
	{
		// The LATCH is what disarms it. Writing the CVar back to 0 does not: -ExecCmds armed it at
		// console priority and a code-priority Set is dropped, which fired this 48 times in one match.
		bMomentumTestRun = true;
		RunThrowMomentumTest();
		return;   // The Core is loose now; let the next tick pick it up through the normal path.
	}

	const float Now = GetServerTimeSeconds();

	// Spec v10 §10's reproduction, armed from the console or -ExecCmds. Ahead of everything, because
	// stage two of it IS a reset and must not be run from inside one. One bool compare when disarmed.
	TickGoalRepro();

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

	// SPEC v13 §8's reproduction, same placement and the same reason: it has to watch a Core that is
	// in the air to know when the previous shot has resolved. One bool compare when disarmed.
	TickTurnoverRepro();

	// SPEC v25 §2's red arm. Ahead of the loose branch's early return for the same reason: most of
	// what it watches happens while the Core is lying on the ground with a window open, and the last
	// step of it watches the frame a locked-out player finally gets to pick it up. One int compare
	// when disarmed.
	TickTurnoverVerify();

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
		//
		// SPEC v10 §10: through the teleport funnel now, so the clients are TOLD. This is the path that
		// catches a Core left on a corpse or wherever a disconnect abandoned it — a discontinuity by
		// definition, and previously a purely local one. PlaceHolderlessCore() applies the identical
		// rule on every machine; this call is what makes the server's answer authoritative when the
		// two could disagree (a client whose arena builder has not replicated yet, say).
		PlaceHolderlessCore();
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

#if !UE_BUILD_SHIPPING
ATraceCharacter* ATraceCore::DebugForcePassWindow()
{
	// See the header for why this exists and what it is for.
	if (!HasAuthority() || !IsValid(Carrier))
	{
		return nullptr;
	}
	if (bPassActive)
	{
		// ALREADY OPEN — HOLD IT OPEN, do not let it mature.
		//
		// A real pass COMPLETES after PassHoldSeconds and hands the Core to its target, and that
		// completion is what defeated three attempts to stage the knife's carrier rule: the window
		// opened, the pass matured, the Core moved, and by the time the blade resolved the victim
		// was an ordinary pawn taking an ordinary 100-damage back-stab. The harness reported
		// "shieldDownAtPress=1 heldToResolve=0" every time — which was the truth, and is why it kept
		// refusing to call the red arm reproduced.
		//
		// Winding the start stamp forward each tick keeps TickPass permanently short of the hold, so
		// the shield stays suppressed for as long as the caller keeps asking and the Core never
		// transfers. Only the STAMP is touched — bPassActive, PassTarget and the trace
		// invulnerability are left exactly as BeginPass set them, so the state under test is the
		// state the game produces.
		PassStartServerTime = GetServerTimeSeconds();
		return PassTarget;
	}

	// Any living teammate who is not the holder. FindPassTargetFor is deliberately NOT used: it
	// requires the holder to be aiming at the teammate, which is the one thing a headless staged
	// test cannot arrange, and the pass TARGET is irrelevant here — only the shield state is.
	//
	// TWO SWEEPS, teammates first. The first staged run failed with "mid-pass to None" because at
	// that instant the holder's whole surviving team was elsewhere or dead, and a test that silently
	// declines to open the window reports the interesting case as untested. The shield flag is a
	// function of bPassActive and the holder alone (ATraceCore::IsShieldSuppressedFor), so ANY valid
	// target produces the state under test; preferring a teammate only keeps the staged situation
	// closer to a real one.
	const ETraceTeam HolderTeam = Carrier->GetTeam();
	for (int32 Sweep = 0; Sweep < 2; ++Sweep)
	{
		const bool bTeammatesOnly = (Sweep == 0);
		for (TActorIterator<ATraceCharacter> It(GetWorld()); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || Candidate == Carrier || !Candidate->IsAlive())
			{
				continue;
			}
			if (bTeammatesOnly && Candidate->GetTeam() != HolderTeam)
			{
				continue;
			}

			// BeginPass is the real entry point, not a hand-set bool: it is what flips bPassActive,
			// hardens the trace and forces the net update, so the staged window is the same window
			// the game produces. A test that sets the flag itself proves only that the flag exists.
			BeginPass(Candidate);
			if (bPassActive)
			{
				return Candidate;
			}
		}
	}

	return nullptr;
}
#endif

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

	// ---- SPEC v13 §6: THE LOCAL, INSTANT CHARGE METER. ------------------------------------------
	//
	// Set on WHICHEVER machine owns the input, ahead of the authority split, because the host owns its
	// own input too and a meter that only worked for remote clients would be a meter nobody testing on
	// a listen server ever saw. The note's requirement is explicit - "the charge indicator must be
	// LOCAL AND INSTANT (predicted) or the player cannot aim their power" - and it is the one part of
	// this mechanic that must not wait for a round trip.
	//
	// It predicts NOTHING ELSE. No throw, no launch, no Core movement: those are resolved on the
	// server from the server's own clock, and a client that predicted the Core leaving its hands would
	// be showing itself a throw the server can still refuse (cooldown, a possession change in flight).
	// The worst this can do is run a meter for a throw that is then refused.
	//
	// A BOT MUST NOT SET IT. Bot pawns are locally controlled on the server, so "is this locally
	// controlled" alone would have every bot's wind-up driving the human's HUD meter. The extra
	// question is whether a PLAYER controller owns it.
	const bool bLocalPlayerInput = Requester->IsLocallyControlled()
		&& Cast<APlayerController>(Requester->GetController()) != nullptr;

	if (IsModeB() && bLocalPlayerInput)
	{
		if (bPressed)
		{
			bLocalThrowCharging = true;
			LocalThrowChargeStartTime = GetServerTimeSeconds();
		}
		else
		{
			bLocalThrowCharging = false;
		}
	}

	if (HasAuthority())
	{
		// ---- THE BINDING'S MEANING IS MODE-DEPENDENT (spec v4 §7, AS CHANGED BY v13 §6). ---------
		//
		// Verbatim: "The carrier should be able to throw the core forward by left clicking." In mode
		// A the same button starts the 0.5 s hover-pass. The branch lives HERE, at the one door every
		// pass input comes through, rather than in the pawn or the bots: ATraceCharacter::
		// DoPassPressed, ATracePlayerController and the bots' ApplyPassInput all call this function
		// and none of them has to learn that a second mode exists.
		//
		// SPEC v13 §6 MOVES THE THROW FROM THE PRESS TO THE RELEASE. "When a player RELEASES the throw
		// button, the core should instantly be released." So the press now only starts a clock and the
		// release is what launches - immediately, with no wind-up of its own and no minimum hold: a
		// press and a release on the same frame is a legal throw and produces the floor momentum.
		//
		// bPassInputHeld IS STILL NEVER ARMED IN MODE B. That latch drives ServerTickPass, which opens
		// a hover-pass window and drops the carrier's shield; mode B has no pass window, and arming it
		// would drop the shield of a player who is merely winding up a throw.
		if (IsModeB())
		{
			if (bPressed)
			{
				// A press from anyone but the living holder was already refused above. Re-arming on a
				// second press without a release (a lost release packet, a rebind pressed twice) simply
				// restarts the clock, which is the conservative answer: it can only ever produce a
				// weaker throw than the player asked for, never a stronger one.
				ThrowChargeStartServerTime = GetServerTimeSeconds();
				ThrowChargeHolder = Requester;
				return;
			}

			// RELEASE. Matched against the press, and only the presser's release can fire it: without
			// that, a teammate's mouse-up would launch the carrier's charged throw for them - the same
			// class of shared-latch bug the pass input's own doc comment describes.
			if (ThrowChargeStartServerTime < 0.f || ThrowChargeHolder.Get() != Requester)
			{
				ClearThrowCharge(nullptr);
				return;
			}

			const float Held = FMath::Max(0.f, GetServerTimeSeconds() - ThrowChargeStartServerTime);
			ClearThrowCharge(nullptr);
			ThrowFromHolder(Requester, Held);
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
	//
	// MODE B STILL PREDICTS NO GAMEPLAY - only the charge METER, which was set above and which shows
	// the player their own finger rather than a claim about the world. A client that predicted the
	// Core leaving its own hands would be showing itself a throw the server may refuse (cooldown, a
	// possession change in flight), so the hover-pass prediction below stays mode-A only.
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

	// SPEC v13 §6, and the same argument one line further: a charge is possession-shaped too. A
	// carrier who is killed, intercepted or half-timed mid-wind-up must not leave a charge armed - the
	// next player to take the Core would inherit a clock that started in somebody else's hands, and
	// their first release would throw at whatever power that stranger had wound up to. Cleared HERE,
	// at the single exit, rather than in each of the six paths that reach it. Deliberately AFTER
	// ThrowFromHolder has read the hold and spent it: that function clears the charge itself before
	// calling this, so a legitimate throw is not fighting this line for the same state.
	ClearThrowCharge(TEXT("possession ended"));

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

	// THE TRACE GOES NOW — every route, not just death. Spec v9 §§3-4, verbatim: "The trace should
	// disappear the instant a pass/throw is made", and "traces stay on the map way after the carrier
	// is dead or has made a pass".
	//
	// The comment that used to sit here said the opposite ("do NOT wipe what is already there ...
	// letting it fade ... death is the one case that clears instantly") and it was factually wrong on
	// both counts, so it is spelled out rather than deleted:
	//
	//   1. THERE IS NO FADE. Demo 7 §1 deleted time-based expiry to kill the stand-still exploit, so
	//      a point is retired only by NEW trace arriving at the head. A component that has stopped
	//      emitting lays none, so the abandoned trace was immortal — and still lethal, because
	//      ServerRunTripTest gates on the POINTS, not on bEmitting. Passers were being killed by
	//      their own trace seconds after giving the Core away.
	//   2. DEATH WAS NOT THE ONLY CASE, it was only the case the old orphan sweep happened to catch
	//      (it was gated on `Holder == nullptr || !Holder->IsAlive()`, which a living passer fails).
	//
	// ReleaseHolder() is the single funnel for all eight possession-end routes — pass completed,
	// mode-B throw, turnover, carrier killed, carrier disconnected, score, half time, kickoff, match
	// end — so clearing HERE, in the same call stack as the possession change, covers all of them
	// with no timer and no grace frame. The clear itself lives in UTraceTrailComponent::SetEmitting
	// (which also clears on the already-false path, so the GameMode's wipes can sweep an orphan);
	// read that function before changing anything here. Trace.Trail.ClearOnPossessionLoss 0 restores
	// the old behaviour in the same binary if you need to A/B it.
	//
	// Do NOT "fix" a future orphan report by reintroducing a lifetime timer: that is the stand-still
	// exploit Demo 7 removed. Fix the ownership path.
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

	// SPEC v10 §10. THE GOAL PATH, and the one the user's report is about. ATraceGameMode::NotifyScored
	// funnels every score, every half-time interval and match start through here, so this single call
	// site is "kickoff, centre reset, half-time, match start" — four of the six paths §10 lists.
	// ServerTeleport is what puts the destination on the wire; before it, this was a bare
	// SetActorLocation and the clients were left holding a Core in the endzone.
	ServerTeleport(GetHomeLocation(), TEXT("kickoff"));
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

	// SPEC v13 §6. A charge is a mode-B object. Switching modes with one armed would leave mode A's
	// hover pass sharing state with a wind-up that can no longer be released, and switching back would
	// find a clock that started in the other ruleset.
	ClearThrowCharge(TEXT("the scoring mode changed"));

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

		// SPEC v6 §4.3. The circle, asked of the volume that scores rather than rebuilt from the
		// arena's constants — the same discipline that deleted the reconstructed goal box above, and
		// for the same reason: a second copy of a shape agrees with the first until somebody changes
		// one of them, and then the hoop you can see and the hoop that scores are different circles.
		if (Zone->IsRingGoal())
		{
			Goal.bRing = true;
			Goal.RingCentre = Zone->GetRingCentre();
			Goal.RingRadius = Zone->GetRingRadius();
			Goal.RingAxis = Zone->GetRingAxis().GetSafeNormal();
		}

		GoalBoxes.Add(Goal);
	}
}

bool ATraceCore::GetAttackGoalRing(const UWorld* World, ETraceTeam AttackingTeam, FVector& OutCentre,
	float& OutRadius, FVector& OutAxis)
{
	ATraceCore* TheCore = ATraceCore::Get(World);
	if (TheCore == nullptr || !TheCore->IsModeB() || AttackingTeam == ETraceTeam::None)
	{
		return false;
	}

	TheCore->RefreshGoalVolumes(/*bForce=*/false);

	for (const FTraceGoalBox& Goal : TheCore->GoalBoxes)
	{
		if (!Goal.bRing || Goal.RingRadius <= 0.f || Goal.DefendingTeam == ETraceTeam::None)
		{
			continue;
		}
		if (AttackingTeam != TraceOpposingTeam(Goal.DefendingTeam))
		{
			continue;
		}

		OutCentre = Goal.RingCentre;
		OutRadius = Goal.RingRadius;

		// Point the axis BACK INTO THE FIELD, i.e. up the attacker's approach, so a caller can read it
		// as "stand off this way from the hoop" and negate it for "the direction a scoring throw
		// travels". The trigger's own local +X is signed by however the arena happened to spawn it, so
		// the sign is decided against the pitch rather than trusted.
		FVector Axis = Goal.RingAxis.GetSafeNormal();
		if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(const_cast<UWorld*>(World)))
		{
			const FBox Field = Arena->GetFieldBounds();
			if (Field.IsValid != 0)
			{
				const FVector ToPitch = Field.GetCenter() - OutCentre;
				if (!ToPitch.IsNearlyZero() && FVector::DotProduct(Axis, ToPitch) < 0.0)
				{
					Axis = -Axis;
				}
			}
		}
		OutAxis = Axis;
		return true;
	}

	return false;
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

// --- SPEC v8 §4: the inherited term ---------------------------------------------------------------

float ATraceCore::GetThrowVelocityInheritance()
{
	return TraceModeBTuning::ThrowInheritance();
}

FVector ATraceCore::GetInheritedThrowVelocity(const AActor* Thrower)
{
	if (!IsValid(Thrower))
	{
		return FVector::ZeroVector;
	}

	// MODE-GATED HERE rather than at every call site, so a caller can add this unconditionally and
	// still get the mode-A answer (a Core that never moves under its own power) without a mode test.
	if (!IsModeB(Thrower->GetWorld()))
	{
		return FVector::ZeroVector;
	}

	const float Fraction = TraceModeBTuning::ThrowInheritance();
	if (Fraction <= 0.f)
	{
		return FVector::ZeroVector;
	}

	// AActor::GetVelocity() resolves to the movement component's velocity for a pawn, which is the
	// same vector the character's own movement is integrating this frame - vertical included, which is
	// the entire point of the note. Asking the actor rather than the CMC directly means a thrower with
	// some other movement model (a spectator, a future vehicle) still contributes what it is doing.
	return Thrower->GetVelocity() * Fraction;
}

FVector ATraceCore::ComputeThrowLaunchVelocity(const AActor* Thrower, const FVector& AimDirection,
	float ChargeScale)
{
	const FVector Direction = AimDirection.GetSafeNormal();
	const float Speed = TraceModeBTuning::ThrowSpeed();

	const FVector Impulse = Direction * Speed + FVector::UpVector * (Speed * TraceModeBTuning::ThrowUpBias());

	// SPEC v13 §6: THE CHARGE SCALES THE IMPULSE, AND THE INHERITED VELOCITY IS ADDED ON TOP OF THE
	// SCALED IMPULSE, NOT SCALED WITH IT.
	//
	// Stated here, in the one expression both the Core and the bot solver use, because the alternative
	// is easy to write and quietly wrong: scaling the sum would mean a player who jumps and taps has
	// thrown away most of their own jump, which is neither what the note asks for ("the longer the
	// player holds down, the more momentum the core has" is about the THROW) nor what spec v8 §4 says
	// ("the core has the momentum from the throw AND ALSO carries momentum from the player" - two
	// terms, and only the first one is a throw). So a tap at the top of a jump leaves with the whole
	// jump in it and almost no impulse; a full charge at the top of the same jump leaves with the jump
	// plus the full 2236 uu/s.
	return Impulse * FMath::Max(0.f, ChargeScale) + GetInheritedThrowVelocity(Thrower);
}

// --- SPEC v13 §6: the charge, resolved -------------------------------------------------------------

float ATraceCore::GetThrowChargeSeconds()      { return TraceModeBTuning::ThrowChargeSeconds(); }
float ATraceCore::GetThrowChargeFloor()        { return TraceModeBTuning::ThrowChargeFloor(); }
float ATraceCore::GetThrowChargeMaxFraction()  { return TraceModeBTuning::ThrowChargeMaxFraction(); }
bool  ATraceCore::DoesThrowChargeClamp()       { return TraceModeBTuning::ThrowChargeClamps(); }

/**
 * The normalised hold, capped: t in UTraceSettings' own arithmetic.
 *
 * Split out so the curve and its inverse cannot disagree about the cap, which is the one place a
 * round-trip error could hide (and the bots depend on that round trip).
 */
static float TraceModeBChargeTCap()
{
	return TraceModeBTuning::ThrowChargeClamps() ? 1.f : TraceModeBTuning::ThrowChargeMaxFraction();
}

float ATraceCore::GetThrowChargeScaleForHold(float HeldSeconds, const AActor* Thrower)
{
	const float Floor = FMath::Clamp(TraceModeBTuning::ThrowChargeFloor(), 0.f, 1.f);

	// Negative = "full charge", i.e. t = 1, i.e. exactly the throw the game had before v13. The
	// diagnostics (Trace.ModeB.MomentumTest) throw this way on purpose so the v8 §4 momentum baseline
	// stays a measurement of the same launch it always was, rather than silently becoming a
	// measurement of a 15% tap.
	if (HeldSeconds < 0.f)
	{
		return 1.f;
	}

	const float ChargeSeconds = FMath::Max(0.01f, TraceModeBTuning::ThrowChargeSeconds());

	// UTraceSettings' ARITHMETIC, COPIED RATHER THAN REINTERPRETED:
	//
	//     t     = HeldSeconds / CoreThrowChargeSeconds
	//     tCap  = bCoreThrowChargeClampsAtFull ? min(t, 1) : min(t, CoreThrowChargeMaxFraction)
	//     Power = CoreThrowChargeFloorFraction + (1 - CoreThrowChargeFloorFraction) * tCap
	//
	// Note that the MAX knob caps t and NOT the power: at MaxFraction 1.5 a 1.5 s hold gives
	// 0.15 + 0.85 * 1.5 = 1.43x, not 1.5x. That is the settings page's stated definition and the ini's,
	// and reinterpreting it here would give two answers for "max charge" that differ by 5% - a
	// disagreement that would only ever be found by somebody wondering why a slider lies.
	// SPEC v19 §3 — the cap on t, stretched by the thrower's own hold scale (1.0 for everybody but
	// Mortimer, and for a null thrower, so this is identity on every pre-v19 path). It is the CAP that
	// moves, not the curve: he walks further along the same line rather than up a steeper one.
	const float HoldScale = FMath::Max(1.f, TraceAbilityTraits::GetThrowChargeHoldScale(Thrower));

	// THE ORIGINAL 100% CHARGE POINT. Not "his cap", not "1.0" — whatever the shared knobs say a full
	// charge is (1 with the clamp on, CoreThrowChargeMaxFraction with it off). DEMO 21 ITEM 7 is
	// phrased against this point, so it has to be read from the same place the other nine read it.
	const float OriginalTCap = TraceModeBChargeTCap();

	const float RawT = FMath::Max(0.f, HeldSeconds / ChargeSeconds);

	// --- DEMO 21 ITEM 7 ---------------------------------------------------------------------------
	//
	//   "after the original 100% charge window has passed, add a .6x modifier to the linear scaling
	//    of his throw charge"
	//
	// TWO SEGMENTS OF ONE LINE, NOT TWO LINES. Up to the original 100% point every character walks
	// the shipped gradient (1 - Floor) per unit of t. Past it, the extra t is worth PastFull of that
	// same gradient — so the curve bends, it does not restart, and there is still exactly one
	// definition of "what a second of charge is worth" in this game.
	//
	// WHY THE SPLIT IS WRITTEN THIS WAY RATHER THAN AS A SECOND FORMULA FOR MORTIMER: for everybody
	// else HoldScale is 1, ExtraRoom is 0, Extra is 0 and PastFull is 1, so the whole of the second
	// term vanishes arithmetically and the expression collapses to the shipped
	// Floor + (1 - Floor) * min(t, TCap). The other nine cannot be moved by this block by
	// construction, which is what makes "his passive changed and nobody else's did" true without a
	// test having to prove it.
	const float PastFull  = FMath::Clamp(TraceAbilityTraits::GetThrowChargePastFullScale(Thrower), 0.f, 1.f);
	const float ExtraRoom = OriginalTCap * (HoldScale - 1.f);            // 0 for the other nine.
	const float WithinOriginal = FMath::Min(RawT, OriginalTCap);
	const float Extra          = FMath::Clamp(RawT - OriginalTCap, 0.f, ExtraRoom);

	const float TCapped = WithinOriginal + Extra * PastFull;

	// THE LINEAR CORRELATION THE NOTE ASKS FOR, in one line: the floor at zero hold, exactly 1.0 (the
	// current throw momentum) at a full one.
	return Floor + (1.f - Floor) * TCapped;
}

float ATraceCore::GetThrowHoldSecondsForScale(float ChargeScale)
{
	const float Floor = FMath::Clamp(TraceModeBTuning::ThrowChargeFloor(), 0.f, 1.f);
	const float ChargeSeconds = FMath::Max(0.01f, TraceModeBTuning::ThrowChargeSeconds());

	if (1.f - Floor <= KINDA_SMALL_NUMBER)
	{
		return 0.f;   // Floor of 1 = charging disabled, every throw full power. Do not divide by it.
	}

	const float TCapped = FMath::Clamp((ChargeScale - Floor) / (1.f - Floor), 0.f, TraceModeBChargeTCap());
	return TCapped * ChargeSeconds;
}

bool ATraceCore::IsThrowCharging() const
{
	return bLocalThrowCharging;
}

float ATraceCore::GetThrowChargeAlpha() const
{
	if (!bLocalThrowCharging)
	{
		return -1.f;
	}

	// The LOCAL clock, so the meter starts moving on the frame the button went down rather than a
	// round trip later. GetServerTimeSeconds() is the shared clock both ends already agree on, so the
	// number the meter shows and the number the server will measure differ only by the player's own
	// latency at the release - which is the honest error and cannot be removed by any prediction.
	const float Held = GetServerTimeSeconds() - LocalThrowChargeStartTime;
	const float ChargeSeconds = FMath::Max(0.01f, TraceModeBTuning::ThrowChargeSeconds());
	const float Alpha = Held / ChargeSeconds;

	// SPEC v19 §3: the meter's ceiling stretches with the carrier's own hold scale for the same reason
	// GetThrowChargeScaleNow() does — while Mortimer is still gaining power his meter must still be
	// moving. 1.0 for everybody else, so this is the shipped clamp untouched for the other nine.
	const float HoldScale = FMath::Max(1.f, TraceAbilityTraits::GetThrowChargeHoldScale(GetCarrier()));

	return TraceModeBTuning::ThrowChargeClamps() ? FMath::Clamp(Alpha, 0.f, HoldScale) : FMath::Max(0.f, Alpha);
}

float ATraceCore::GetThrowChargeScaleNow() const
{
	if (!bLocalThrowCharging)
	{
		return -1.f;
	}
	// SPEC v19 §3: the CARRIER is the thrower, so the local meter walks the same extended cap the
	// server will apply at the release. Without this the HUD would pin at 1.0 while Mortimer kept
	// charging, i.e. the meter would stop telling him the truth exactly when his passive starts
	// paying — the "two answers for max charge" failure this function exists to prevent.
	return GetThrowChargeScaleForHold(
		FMath::Max(0.f, GetServerTimeSeconds() - LocalThrowChargeStartTime), GetCarrier());
}

void ATraceCore::ClearThrowCharge(const TCHAR* Reason)
{
	const bool bWasCharging = (ThrowChargeStartServerTime >= 0.f) || bLocalThrowCharging;

	ThrowChargeStartServerTime = -1.f;
	ThrowChargeHolder = nullptr;
	bLocalThrowCharging = false;

	if (bWasCharging && Reason != nullptr)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[ModeB] throw charge cancelled (%s)."), Reason);
	}
}

ATraceCore::FThrowMomentumSample ATraceCore::LastThrow;

static FAutoConsoleCommand GTraceModeBThrowMomentumCmd(
	TEXT("Trace.ModeB.ThrowMomentum"),
	TEXT("MODE B, spec v8 §4. Prints the last throw broken into its parts: the impulse, the velocity ")
	TEXT("inherited from the thrower, and the launch velocity that left the hand."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		const ATraceCore::FThrowMomentumSample& S = ATraceCore::LastThrow;
		if (!S.bValid)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeB] THROW MOMENTUM: no throw recorded on this machine yet (the record is made ")
				TEXT("on the SERVER, where the throw is resolved). Inheritance fraction is %.2f."),
				ATraceCore::GetThrowVelocityInheritance());
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] THROW MOMENTUM (spec v8 §4 + v13 §6): %s was %s at %.0f uu/s horizontal, %+.0f ")
			TEXT("uu/s vertical | held %.2fs -> charge x%.2f | impulse %.0f uu/s (charged) + inherited ")
			TEXT("%.0f uu/s (x%.2f) = LAUNCH %.0f uu/s, launch Z %+.0f uu/s"),
			*S.ThrowerName, S.bThrowerFalling ? TEXT("AIRBORNE") : TEXT("on the ground"),
			S.ThrowerSpeed2D, S.ThrowerVelocityZ, S.HeldSeconds, S.ChargeScale,
			S.ImpulseSpeed, S.InheritedSpeed, S.Inheritance, S.LaunchSpeed, S.LaunchVelocityZ);
	}));

bool ATraceCore::HasRemoteClientPawn() const
{
	if (!HasAuthority())
	{
		return false;
	}

	TArray<ATraceCharacter*> Characters;
	GatherCharacters(Characters);

	for (const ATraceCharacter* Candidate : Characters)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive())
		{
			continue;
		}

		// A bare read of the inherited Controller; nothing is declared, so there is nothing to shadow.
		const APlayerController* CandidateController = Cast<APlayerController>(Candidate->GetController());
		if (CandidateController != nullptr && !CandidateController->IsLocalController())
		{
			return true;
		}
	}

	return false;
}

void ATraceCore::TickFlightLog()
{
	if (CVarModeBFlightLog.GetValueOnGameThread() == 0)
	{
		bFlightLogWasLoose = bLoose;
		return;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const TCHAR* Machine = HasAuthority()
		? (World->GetNetMode() == NM_ListenServer ? TEXT("HOST") : TEXT("SERVER"))
		: TEXT("CLIENT");

	if (!bLoose)
	{
		if (bFlightLogWasLoose)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBFlight] %s: no longer loose (holder %s)."),
				Machine, *GetNameSafe(Carrier));
		}
		bFlightLogWasLoose = false;
		return;
	}

	// The first frame of a flight is the one the momentum question is about, so it is never dropped
	// by the throttle: a 10 Hz sample of a 2800 uu/s launch can easily miss the launch itself.
	const float NowReal = static_cast<float>(World->GetTimeSeconds());
	const bool bFirst = !bFlightLogWasLoose;
	bFlightLogWasLoose = true;

	if (!bFirst && NowReal < NextFlightLogTime)
	{
		return;
	}
	NextFlightLogTime = NowReal + 0.1f;

	const FVector Velocity = LooseVelocity;
	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBFlight] %s%s pos %s vel %s | speed %.0f uu/s (horiz %.0f, Z %+.0f)"),
		Machine, bFirst ? TEXT(" LAUNCH") : TEXT(""),
		*FVector(LooseLocation).ToCompactString(), *Velocity.ToCompactString(),
		Velocity.Size(), Velocity.Size2D(), Velocity.Z);
}

void ATraceCore::RunThrowMomentumTest()
{
	if (!HasAuthority())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] server only - a throw is resolved on the authority."));
		return;
	}

	if (!IsModeB())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] mode A: there is no throw. Launch with ?mode=b."));
		return;
	}

	// A JOINING CLIENT'S PAWN FIRST, and that preference is spec v8 §0, not tidiness. Measuring this
	// on the listen host's own pawn would prove only that the arithmetic runs somewhere - the host is
	// the machine on which every one of this pass's three complaints is invisible by definition. Using
	// a remote client's pawn puts the whole path under test: the server reads ITS copy of a
	// client-owned pawn's velocity, releases a client-owned holder, and replicates the loose Core back
	// to the very client that threw it.
	ATraceCharacter* Thrower = nullptr;

	TArray<ATraceCharacter*> Characters;
	GatherCharacters(Characters);

	for (ATraceCharacter* Candidate : Characters)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive())
		{
			continue;
		}

		// A bare read of the inherited Controller, which is correct code - nothing is declared here.
		const APlayerController* CandidateController = Cast<APlayerController>(Candidate->GetController());
		if (CandidateController != nullptr && !CandidateController->IsLocalController())
		{
			Thrower = Candidate;
			break;
		}
	}

	if (Thrower != nullptr)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBMomentum] thrower is a REMOTE CLIENT's pawn (%s) - spec v8 §0."), *GetNameSafe(Thrower));
	}
	else if (IsValid(Carrier) && Carrier->IsAlive())
	{
		Thrower = Carrier;
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBMomentum] NO REMOTE CLIENT CONNECTED - falling back to the current holder (%s). ")
			TEXT("This is a HOST-side measurement and spec v8 §0 does not accept it on its own."),
			*GetNameSafe(Thrower));
	}
	else
	{
		for (ATraceCharacter* Candidate : Characters)
		{
			if (IsValid(Candidate) && Candidate->IsAlive())
			{
				Thrower = Candidate;
				break;
			}
		}
	}

	if (!IsValid(Thrower))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] nobody alive to throw it."));
		return;
	}

	if (Carrier != Thrower)
	{
		GrantTo(Thrower, ETraceCoreGrantReason::Debug);
	}

	UCharacterMovementComponent* Movement = Thrower->GetCharacterMovement();
	if (Movement == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] %s has no character movement."), *GetNameSafe(Thrower));
		return;
	}

	// Restored at the end: this command is a measurement, not a shove.
	const FVector SavedVelocity = Movement->Velocity;
	const EMovementMode SavedMode = Movement->MovementMode;

	const FVector Forward = Thrower->GetActorForwardVector().GetSafeNormal2D();
	const float RunSpeed = Movement->GetMaxSpeed();
	const float JumpZ = Movement->JumpZVelocity;

	struct FCase
	{
		const TCHAR* Name;
		FVector Velocity;
		EMovementMode Mode;
	};

	const FCase Cases[] =
	{
		{ TEXT("STANDING"), FVector::ZeroVector,                            MOVE_Walking },
		{ TEXT("RUNNING"),  Forward * RunSpeed,                             MOVE_Walking },
		{ TEXT("JUMPING"),  Forward * RunSpeed + FVector(0.f, 0.f, JumpZ),  MOVE_Falling },
	};

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBMomentum] spec v8 §4, thrower %s, inheritance x%.2f, run speed %.0f, jump Z %.0f"),
		*GetNameSafe(Thrower), TraceModeBTuning::ThrowInheritance(), RunSpeed, JumpZ);

	for (const FCase& Case : Cases)
	{
		if (!IsValid(Thrower) || !Thrower->IsAlive())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] thrower died mid-measurement."));
			return;
		}

		// Put the Core back and forgive the cooldown: the cooldown is not what is being measured.
		if (Carrier != Thrower)
		{
			GrantTo(Thrower, ETraceCoreGrantReason::Debug);
		}
		ThrowCooldownEndServerTime = 0.f;

		Movement->SetMovementMode(Case.Mode);
		Movement->Velocity = Case.Velocity;

		LastThrow.bValid = false;
		if (!ThrowFromHolder(Thrower))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBMomentum] %s: ThrowFromHolder refused."), Case.Name);
			continue;
		}

		// The pre-v8 launch is the impulse on its own, which LastThrow already carries - so the A/B is
		// printed from the SAME throw rather than from a second run with the knob at zero.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBMomentum] %-8s thrower %.0f uu/s horiz %+.0f vert (%s) | pre-v8 launch %.0f uu/s ")
			TEXT("(Z %+.0f) -> v8 launch %.0f uu/s (Z %+.0f) | inherited %.0f uu/s, %+.0f%%"),
			Case.Name, LastThrow.ThrowerSpeed2D, LastThrow.ThrowerVelocityZ,
			LastThrow.bThrowerFalling ? TEXT("AIRBORNE") : TEXT("grounded"),
			LastThrow.ImpulseSpeed, LastThrow.LaunchVelocityZ - LastThrow.ThrowerVelocityZ * LastThrow.Inheritance,
			LastThrow.LaunchSpeed, LastThrow.LaunchVelocityZ, LastThrow.InheritedSpeed,
			100.f * (LastThrow.LaunchSpeed - LastThrow.ImpulseSpeed) / FMath::Max(1.f, LastThrow.ImpulseSpeed));
	}

	if (IsValid(Thrower) && Movement != nullptr)
	{
		Movement->SetMovementMode(SavedMode);
		Movement->Velocity = SavedVelocity;
	}

	UE_LOG(LogTraceGame, Display, TEXT("[ModeBMomentum] done. The last throw is still loose; play resumes normally."));
}

// NAMED "...Now", NOT "Trace.ModeB.MomentumTest". A console OBJECT name is a single namespace shared
// by variables and commands, and registering a command under the same name as the CVar above is a
// FATAL error at startup ("can't be replaced with the new one of different type") - which is how this
// first run died. The CVar is the armable form (-ExecCmds, polled); this is the fire-it-right-now form.
static FAutoConsoleCommand GTraceModeBMomentumTestCmd(
	TEXT("Trace.ModeB.MomentumTestNow"),
	TEXT("MODE B, spec v8 §4. Server. Throws the Core three times - standing, running and airborne - ")
	TEXT("and prints the pre-v8 and post-v8 launch velocity of each. Immediate; ")
	TEXT("Trace.ModeB.MomentumTest 1 is the armable form for -ExecCmds."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || World->GetNetMode() == NM_Client)
			{
				continue;
			}

			if (ATraceCore* Core = ATraceCore::Get(World))
			{
				Core->RunThrowMomentumTest();
			}
		}
	}));

/**
 * Prints THIS MACHINE'S view of the loose Core.
 *
 * Deliberately not authority-gated and deliberately not a server RPC: spec v8 §0 is a client-experience
 * pass, and the question "does the CLIENT see the Core carrying the throw's momentum" cannot be
 * answered from the server's copy by definition. LooseVelocity is replicated so a client can
 * dead-reckon; this prints what the client actually received.
 */
static FAutoConsoleCommand GTraceModeBCoreProbeCmd(
	TEXT("Trace.ModeB.CoreProbe"),
	TEXT("MODE B. Prints the LOCAL machine's view of the Core: held/loose, its replicated position and ")
	TEXT("velocity, and the actor transform the local renderer is drawing. Run it on a CLIENT."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || (World->GetNetMode() != NM_Client && World->GetNetMode() != NM_ListenServer
				&& World->GetNetMode() != NM_Standalone))
			{
				continue;
			}

			ATraceCore* Core = ATraceCore::Get(World);
			if (Core == nullptr)
			{
				continue;
			}

			const TCHAR* Machine = (World->GetNetMode() == NM_Client) ? TEXT("CLIENT") : TEXT("SERVER/LOCAL");
			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeB] CORE PROBE (%s): mode %s | holder %s | loose %d | repl pos %s | repl vel %s ")
				TEXT("(%.0f uu/s, Z %+.0f) | drawn at %s"),
				Machine, Core->IsModeB() ? TEXT("B") : TEXT("A"), *GetNameSafe(Core->GetCarrier()),
				Core->IsLoose() ? 1 : 0, *FVector(Core->LooseLocation).ToCompactString(),
				*FVector(Core->LooseVelocity).ToCompactString(),
				FVector(Core->LooseVelocity).Size(), FVector(Core->LooseVelocity).Z,
				*Core->GetActorLocation().ToCompactString());
		}
	}));

// =================================================================================================
// SPEC v12 §4 — MEASURING THE MAGNET. Trace.ModeB.CatchTest
//
// The change is one number (CoreCatchRadius 500 -> 450) and the request is one sentence, but "does
// the magnet still catch" is not answerable by reading it. So this sweeps the two axes that decide
// the answer — HOW BADLY THE THROW MISSES and HOW FAST THE CORE IS — and counts catches.
//
// IT CALLS THE SHIPPED STEERING FUNCTION. TraceModeBTuning::SteerTowardCatchPoint is the same
// function ATraceCore::ServerApplyCatchZone calls every frame of every real throw, and the catch
// test below is the same surface-distance-vs-PickupRadius test ServerTryPickup uses. Nothing here
// is a second implementation of the magnet: this project has already had a verification declare
// PASS without ever executing the thing it was verifying, and a re-implemented magnet would fail
// the same way while looking more thorough. The knobs come from the live accessors too, so what is
// measured is what UTraceSettings and DefaultGame.ini actually resolved to.
//
// WHY THE SWEEP IS FLAT AND UNGRAVITIED, stated plainly rather than buried: gravity would add a
// second reason for a trial to fail (a slow Core lands short no matter what the magnet does) and
// the two causes would be indistinguishable in the total. This isolates the magnet's steering
// authority, which is the only thing the radius changes. The BALLISTIC case — a real throw that
// inherits a jumping thrower's velocity — is covered by running the sweep at that throw's speed;
// what the arc does to the Core's height is the throw's business, not the magnet's.
//
// WHY IT IS A CONSOLE COMMAND AND NOT A UNIT TEST: the values have to come from a RUNNING game.
// The ini wins over the header on this project and the header has been wrong before.
//
// THE COMMAND TAKES NO ARGUMENTS, AND THAT IS DELIBERATE. It runs BOTH arms itself: the shipped
// radius, and that radius divided by 0.9, which is the pre-v12 §4 value the cut was made from. Two
// reasons, and the second one has already cost this project a pass:
//
//   1. The comparison IS the deliverable. A command that measures one arm can be run once, reported
//      as "the catch rate", and mean nothing. Both arms in one invocation cannot be half-run.
//   2. -TraceExec / -ExecCmds AND SPACES DO NOT MIX HERE. Setting the arm from the command line
//      would need `Trace.ModeB.CatchRadius 500`, and a quoted argument on this project's command
//      line has already broken into the URL parser and produced a "verification" whose commands
//      never executed at all. No argument, no space, no quote, nothing to get wrong.
//
// Usage, headless:  -TraceExec=Trace.VerifyKnobs|Trace.DumpSettings|Trace.ModeB.CatchTest
// =================================================================================================
static void RunModeBCatchTestAtRadius(const float Radius, const TCHAR* ArmLabel)
{
	// Live knobs, not literals. If CoreCatchRadius did not bind, the caller passes the fallback and
	// the numbers below are the fallback's numbers - itself the answer to a different question.
	const float Curve = TraceModeBTuning::CatchCurveStrength();
	const float Pickup = TraceModeBTuning::PickupRadius();

	// The catcher's capsule, taken from a LIVE pawn where there is one: the magnet aims at the
	// capsule's centre line and measures to its surface, so the capsule's radius is part of the
	// answer and a guessed 34 would quietly shift every number.
	double CapsuleRadius = 34.0;
	double CapsuleHalfHeight = 88.0;
	const TCHAR* CapsuleSource = TEXT("defaults (no live pawn found)");

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World == nullptr)
		{
			continue;
		}

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			if (const UCapsuleComponent* Capsule = It->GetCapsuleComponent())
			{
				CapsuleRadius = static_cast<double>(Capsule->GetScaledCapsuleRadius());
				CapsuleHalfHeight = static_cast<double>(Capsule->GetScaledCapsuleHalfHeight());
				CapsuleSource = TEXT("a live pawn");
				break;
			}
		}
	}

	// Seven speeds spanning a walked-in throw to the fastest launch the game can produce: spec v8 §4
	// has the throw inherit the thrower's velocity, and a jumping throw was measured at ~3357 uu/s.
	static const float Speeds[] = { 800.f, 1200.f, 1600.f, 2000.f, 2400.f, 2800.f, 3357.f };

	// Miss distances from dead-on to well outside the zone, in 25 uu steps. 25 uu is finer than the
	// difference the 10% cut makes, so the boundary it moves is resolvable rather than inferred.
	constexpr double MissStep = 25.0;
	constexpr int32 MissCount = 25;                 // 0 .. 600 uu
	constexpr double ApproachDistance = 1500.0;     // where the Core starts, uu from the catcher
	constexpr float DeltaSeconds = 1.f / 60.f;
	constexpr int32 MaxSteps = 600;                 // 10 s at 60 Hz; every trial resolves far sooner

	const FVector Catcher(0.0, 0.0, 0.0);

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] CATCHTEST [%s]: radius=%.0f curve=%.1f pickup=%.0f | capsule r=%.0f halfH=%.0f from %s | ")
		TEXT("%d speeds x %d miss offsets (0-%.0f uu, %.0f uu steps), flat approach from %.0f uu, %d Hz"),
		ArmLabel, Radius, Curve, Pickup, CapsuleRadius, CapsuleHalfHeight, CapsuleSource,
		static_cast<int32>(UE_ARRAY_COUNT(Speeds)), MissCount, (MissCount - 1) * MissStep, MissStep,
		ApproachDistance, static_cast<int32>(1.f / DeltaSeconds));

	int32 TotalTrials = 0;
	int32 TotalCatches = 0;
	int32 TotalBaselineCatches = 0;

	for (const float Speed : Speeds)
	{
		int32 Catches = 0;
		int32 BaselineCatches = 0;
		double WidestCatchableMiss = -1.0;

		for (int32 MissIndex = 0; MissIndex < MissCount; ++MissIndex)
		{
			const double Miss = MissIndex * MissStep;

			// TWO ARMS PER TRIAL, and the second one is what stops this being a magnet-shaped
			// tautology. MAGNET ON is the game; MAGNET OFF is the identical trial with the steering
			// skipped, i.e. what a straight throw would have done on its own. A miss the Core would
			// have caught anyway is not evidence the magnet works, and without the baseline every
			// small offset would be counted as a save the magnet did not make.
			for (int32 Arm = 0; Arm < 2; ++Arm)
			{
				const bool bMagnet = (Arm == 0);

				FVector Position = Catcher + FVector(-ApproachDistance, 0.0, 0.0);
				FVector Velocity = (FVector(ApproachDistance, Miss, 0.0)).GetSafeNormal() * static_cast<double>(Speed);

				bool bCaught = false;

				for (int32 Step = 0; Step < MaxSteps && !bCaught; ++Step)
				{
					// Surface distance to the catcher's capsule, computed exactly as
					// ServerApplyCatchZone and ServerTryPickup both compute it.
					FVector CatchPoint(Catcher.X, Catcher.Y,
						FMath::Clamp(Position.Z, Catcher.Z - CapsuleHalfHeight, Catcher.Z + CapsuleHalfHeight));
					const double SurfaceDistance = FMath::Max(0.0, FVector::Dist(Position, CatchPoint) - CapsuleRadius);

					// ServerTryPickup's test, unchanged: surface distance inside the pickup radius.
					if (SurfaceDistance <= static_cast<double>(Pickup))
					{
						bCaught = true;
						break;
					}

					if (bMagnet && SurfaceDistance <= static_cast<double>(Radius))
					{
						// The forward-only gate, copied from ServerApplyCatchZone: a catcher the Core
						// has already passed is refused, or the magnet would drag it backwards.
						const FVector ToCatcher = CatchPoint - Position;
						if (!ToCatcher.IsNearlyZero()
							&& FVector::DotProduct(ToCatcher.GetSafeNormal(), Velocity.GetSafeNormal()) >= 0.0)
						{
							Velocity = TraceModeBTuning::SteerTowardCatchPoint(
								Position, Velocity, CatchPoint, SurfaceDistance, Radius, Curve, DeltaSeconds);
						}
					}

					Position += Velocity * static_cast<double>(DeltaSeconds);

					// Once the Core is past the catcher and opening the range, the trial is decided.
					if (Position.X > Catcher.X + ApproachDistance)
					{
						break;
					}
				}

				if (bMagnet)
				{
					if (bCaught)
					{
						++Catches;
						WidestCatchableMiss = FMath::Max(WidestCatchableMiss, Miss);
					}
					++TotalTrials;
					TotalCatches += bCaught ? 1 : 0;
				}
				else
				{
					BaselineCatches += bCaught ? 1 : 0;
					TotalBaselineCatches += bCaught ? 1 : 0;
				}
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] CATCHTEST [%s]  speed %6.0f uu/s : caught %2d/%d (%5.1f%%)  vs no-magnet %2d/%d (%5.1f%%)  ")
			TEXT("| magnet saved %2d  | widest catchable miss %.0f uu"),
			ArmLabel, Speed, Catches, MissCount, 100.f * Catches / MissCount,
			BaselineCatches, MissCount, 100.f * BaselineCatches / MissCount,
			Catches - BaselineCatches, WidestCatchableMiss);
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] CATCHTEST TOTAL [%s] at radius %.0f: %d/%d caught (%.1f%%), no-magnet %d/%d (%.1f%%), ")
		TEXT("magnet saved %d throws"),
		ArmLabel, Radius, TotalCatches, TotalTrials, 100.f * TotalCatches / FMath::Max(1, TotalTrials),
		TotalBaselineCatches, TotalTrials, 100.f * TotalBaselineCatches / FMath::Max(1, TotalTrials),
		TotalCatches - TotalBaselineCatches);
}

static void RunModeBCatchTest()
{
	// THE SHIPPED RADIUS, resolved live: UTraceSettings first, DefaultGame.ini having already won
	// over the header, the CVar only if somebody set it. This is the AFTER arm by definition — it is
	// whatever the game is actually going to be played at, not a literal typed here.
	const float After = TraceModeBTuning::CatchRadius();

	// The BEFORE arm, derived from the request rather than hard-coded: spec v12 §4 is "reduce the
	// magnet radius by 10%", so the value it was reduced FROM is the shipped radius / 0.9. Deriving
	// it means the two arms cannot drift apart if the shipped number is retuned again — re-run and
	// the comparison is still "this value against the 10% it was cut from".
	const float Before = After / 0.9f;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] CATCHTEST: spec v12 §4 is a 10%% cut, so this runs BOTH arms - BEFORE %.0f uu ")
		TEXT("(= shipped / 0.9) and AFTER %.0f uu (the shipped value, live from UTraceSettings/ini). ")
		TEXT("Same sweep, same steering function, same frame rate; the only difference is the radius."),
		Before, After);

	RunModeBCatchTestAtRadius(Before, TEXT("BEFORE 500"));
	RunModeBCatchTestAtRadius(After, TEXT("AFTER  450"));
}

static FAutoConsoleCommand GTraceModeBCatchTestCmd(
	TEXT("Trace.ModeB.CatchTest"),
	TEXT("MODE B, spec v12 §4. Sweeps miss distance x Core speed through the SHIPPED catch-zone steering ")
	TEXT("and reports the catch rate against a no-magnet baseline, at BOTH the shipped radius and the ")
	TEXT("radius it was cut from (shipped / 0.9). Takes no arguments on purpose - see the block comment."),
	FConsoleCommandDelegate::CreateStatic(&RunModeBCatchTest));

// =================================================================================================
// SPEC v13 §5 — TESTING THE CONTEST. Trace.ModeB.ContestTest
//
// It drives TraceModeBTuning::PickContestedCatcher, WHICH IS THE FUNCTION THE GAME CALLS, with
// scripted distances. It needs no world, no pawns and no match, which is the point: the claim being
// made ("nearest wins, and the answer is stable and deterministic") is a claim about a selection, and
// a selection can be tested exhaustively in a millisecond where a live match can only be watched.
//
// AND IT HAS A RED ARM. Case 3 runs the same oscillating sequence twice - once at the shipped margin
// and once at zero, which is strict nearest-wins every frame - and asserts that the second one
// FLICKERS. If the harness cannot show the flicker it is not measuring stability, and its "no
// flicker" result on the shipped margin would mean nothing.
// =================================================================================================

static void RunModeBContestTest()
{
	using TraceModeBTuning::FCatchContender;
	using TraceModeBTuning::PickContestedCatcher;

	const float Margin = TraceModeBTuning::CatchContestHysteresis();

	int32 Passes = 0;
	int32 Fails = 0;

	auto Check = [&Passes, &Fails](bool bCondition, const FString& What)
	{
		(bCondition ? Passes : Fails)++;
		if (bCondition)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBContest]   PASS  %s"), *What);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBContest]   FAIL  %s"), *What);
		}
	};

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBContest] ===== spec v13 §5: the contested magnet, margin %.0f uu ====="), Margin);

	// --- 1. NEAREST WINS, with nobody holding the pull. -------------------------------------------
	{
		TArray<FCatchContender> Set;
		Set.Add({ 300.0, 11u, false });
		Set.Add({ 120.0, 22u, false });   // nearest
		Set.Add({ 260.0, 33u, false });

		const int32 Winner = PickContestedCatcher(Set, Margin);
		Check(Winner == 1, FString::Printf(
			TEXT("three contenders at 300/120/260 uu -> the 120 wins (got index %d)"), Winner));
	}

	// --- 2. A CLEARLY NEARER CHALLENGER TAKES THE PULL. -------------------------------------------
	{
		TArray<FCatchContender> Set;
		Set.Add({ 400.0, 11u, true });    // incumbent, far
		Set.Add({ 100.0, 22u, false });   // challenger, much nearer

		const int32 Winner = PickContestedCatcher(Set, Margin);
		Check(Winner == 1, FString::Printf(
			TEXT("an incumbent at 400 uu loses to a challenger at 100 uu (got index %d)"), Winner));
	}

	// --- 3. A NEAR TIE DOES NOT OSCILLATE — AND THE ZERO-MARGIN ARM SHOWS THAT IT WOULD. ----------
	//
	// Two players closing on the Core, their distances crossing back and forth by a few uu each frame,
	// which is what two defenders converging actually looks like frame to frame.
	{
		auto RunOscillation = [](float TestMargin) -> int32
		{
			int32 Incumbent = INDEX_NONE;
			int32 Switches = 0;

			for (int32 Frame = 0; Frame < 60; ++Frame)
			{
				// A and B converge on the Core together, separated by a jitter of +/-8 uu that changes
				// sign every few frames. Neither is meaningfully nearer at any point.
				const double Base = 400.0 - Frame * 4.0;
				const double Jitter = 8.0 * FMath::Sin(Frame * 0.9);

				TArray<FCatchContender> Set;
				Set.Add({ Base + Jitter, 11u, Incumbent == 0 });
				Set.Add({ Base - Jitter, 22u, Incumbent == 1 });

				const int32 Winner = PickContestedCatcher(Set, TestMargin);
				if (Incumbent != INDEX_NONE && Winner != Incumbent)
				{
					++Switches;
				}
				Incumbent = Winner;
			}

			return Switches;
		};

		const int32 SwitchesShipped = RunOscillation(Margin);
		const int32 SwitchesNoMargin = RunOscillation(0.f);

		Check(SwitchesShipped == 0, FString::Printf(
			TEXT("60 frames of a +/-8 uu near-tie at the shipped margin: %d target switches (want 0)"),
			SwitchesShipped));

		// THE RED ARM. If this does not flicker, the sequence above is not a near tie and the result
		// above proves nothing about hysteresis.
		Check(SwitchesNoMargin >= 5, FString::Printf(
			TEXT("the SAME 60 frames with the margin at 0 flicker %d times, so the test can detect a ")
			TEXT("flicker (want >= 5 - if this fails, the case above is not measuring anything)"),
			SwitchesNoMargin));
	}

	// --- 4. THE MARGIN IS NOT A PERMANENT HANDICAP. -----------------------------------------------
	//
	// Once a challenger is nearer by MORE than the margin the pull moves, and the incumbency moves
	// with it. A margin that accumulated would let the first player into the zone keep the Core for the
	// whole flight, which is the opposite of "it should go to the player closest to the core".
	{
		TArray<FCatchContender> Set;
		Set.Add({ 200.0, 11u, true });
		Set.Add({ 200.0 - static_cast<double>(Margin) - 1.0, 22u, false });

		const int32 Winner = PickContestedCatcher(Set, Margin);
		Check(Winner == 1, FString::Printf(
			TEXT("a challenger nearer by margin+1 uu takes the pull (got index %d)"), Winner));

		TArray<FCatchContender> Barely;
		Barely.Add({ 200.0, 11u, true });
		Barely.Add({ 200.0 - static_cast<double>(Margin) + 1.0, 22u, false });

		bool bHeld = false;
		const int32 HeldWinner = PickContestedCatcher(Barely, Margin, &bHeld);
		Check(HeldWinner == 0 && bHeld, FString::Printf(
			TEXT("a challenger nearer by only margin-1 uu does NOT (got index %d, held=%d)"),
			HeldWinner, bHeld ? 1 : 0));
	}

	// --- 5. AN EXACT TIE IS DETERMINISTIC, AND DOES NOT DEPEND ON ROSTER ORDER. --------------------
	{
		TArray<FCatchContender> Forward;
		Forward.Add({ 250.0, 77u, false });
		Forward.Add({ 250.0, 33u, false });

		TArray<FCatchContender> Reversed;
		Reversed.Add({ 250.0, 33u, false });
		Reversed.Add({ 250.0, 77u, false });

		const uint32 WinnerA = Forward[PickContestedCatcher(Forward, Margin)].StableKey;
		const uint32 WinnerB = Reversed[PickContestedCatcher(Reversed, Margin)].StableKey;

		Check(WinnerA == WinnerB && WinnerA == 33u, FString::Printf(
			TEXT("two players at an identical 250 uu resolve to the same one (%u) whichever order the ")
			TEXT("roster returns them in (%u vs %u)"), 33u, WinnerA, WinnerB));
	}

	// --- 6. AN INCUMBENT WHO LEAVES THE SET KEEPS NOTHING. ----------------------------------------
	//
	// Dead, out of range, or behind the Core: the caller simply stops listing them, and the pull must
	// move to whoever is left rather than being held by a player who is no longer a candidate.
	{
		TArray<FCatchContender> Set;
		Set.Add({ 380.0, 44u, false });   // the old incumbent is not in this list at all
		Set.Add({ 390.0, 55u, false });

		const int32 Winner = PickContestedCatcher(Set, Margin);
		Check(Winner == 0, FString::Printf(
			TEXT("with the incumbent gone from the set the nearest survivor takes it (got index %d)"),
			Winner));
	}

	if (Fails == 0)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBContest] ===== ALL %d CHECKS PASSED (spec v13 §5) ====="), Passes);
	}
	else
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBContest] ===== %d PASSED, %d FAILED (spec v13 §5) ====="), Passes, Fails);
	}
}

static FAutoConsoleCommand GTraceModeBContestTestCmd(
	TEXT("Trace.ModeB.ContestTest"),
	TEXT("MODE B, spec v13 §5. Drives the SHIPPED contested-magnet selection with scripted distances: ")
	TEXT("nearest wins, a near tie does not oscillate (and the zero-margin arm proves the test can see ")
	TEXT("a flicker), the margin is not cumulative, and an exact tie is order-independent."),
	FConsoleCommandDelegate::CreateStatic(&RunModeBContestTest));

// =================================================================================================
// SPEC v13 §6 — TESTING THE CHARGE CURVE. Trace.ModeB.ChargeTest
//
// The note makes four checkable claims about the mapping from hold time to momentum, and every one of
// them is a property of ATraceCore::GetThrowChargeScaleForHold - the function the throw, the HUD meter
// and the bots all call. So they are checked against that function directly rather than by watching
// throws, which cannot separate "the curve is wrong" from "the bot held the button for the wrong
// length of time".
// =================================================================================================

static void RunModeBChargeTest()
{
	int32 Passes = 0;
	int32 Fails = 0;

	auto Check = [&Passes, &Fails](bool bCondition, const FString& What)
	{
		(bCondition ? Passes : Fails)++;
		if (bCondition)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBCharge]   PASS  %s"), *What);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBCharge]   FAIL  %s"), *What);
		}
	};

	const float ChargeSeconds = ATraceCore::GetThrowChargeSeconds();
	const float Floor = ATraceCore::GetThrowChargeFloor();
	const float MaxHoldFraction = ATraceCore::GetThrowChargeMaxFraction();
	const float FullSpeed = ATraceCore::GetThrowSpeed();

	// FULL POWER IS 1.0 BY DEFINITION - a full hold reaches exactly the pre-v13 throw. The MAX knob
	// caps the HOLD, not the power, and is only reachable with the clamp off.
	constexpr float FullPower = 1.f;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBCharge] ===== spec v13 §6: charge %.2fs, floor %.2f, hold cap x%.2f, clamp %d | full ")
		TEXT("throw speed %.0f uu/s ====="),
		ChargeSeconds, Floor, MaxHoldFraction, ATraceCore::DoesThrowChargeClamp() ? 1 : 0, FullSpeed);

	// 1. "So if the player just clicks the throw button it will throw with very low momentum."
	const float Instant = ATraceCore::GetThrowChargeScaleForHold(0.f);
	Check(FMath::IsNearlyEqual(Instant, Floor, 0.001f) && Instant > 0.f,
		FString::Printf(TEXT("an instant click throws at x%.3f of full - low, and NOT zero (a zero throw ")
			TEXT("drops at the thrower's feet and reads as a bug)"), Instant));

	// 2. "Start by making a one second charge up time to reach the current core throw momentum."
	const float Full = ATraceCore::GetThrowChargeScaleForHold(ChargeSeconds);
	Check(FMath::IsNearlyEqual(Full, FullPower, 0.001f),
		FString::Printf(TEXT("a %.2fs hold reaches x%.3f, i.e. the current throw momentum (%.0f uu/s)"),
			ChargeSeconds, Full, FullSpeed * Full));

	// 3. "Charge time to throw momentum should be a linear correlation." Checked as linearity itself -
	//    equal steps in hold produce equal steps in momentum - rather than by spot-checking a midpoint,
	//    which any monotonic curve would pass.
	{
		bool bLinear = true;
		float WorstError = 0.f;
		for (int32 Step = 0; Step <= 20; ++Step)
		{
			const float T = ChargeSeconds * (static_cast<float>(Step) / 20.f);
			const float Expected = Floor + (FullPower - Floor) * (static_cast<float>(Step) / 20.f);
			const float Actual = ATraceCore::GetThrowChargeScaleForHold(T);
			WorstError = FMath::Max(WorstError, FMath::Abs(Actual - Expected));
			bLinear &= FMath::IsNearlyEqual(Actual, Expected, 0.0005f);
		}
		Check(bLinear, FString::Printf(
			TEXT("21 samples across the wind-up are linear in hold time (worst error %.5f)"), WorstError));
	}

	// 4. "[ASSUMPTION] clamp there." Holding past the charge time buys nothing more.
	if (ATraceCore::DoesThrowChargeClamp())
	{
		const float Overheld = ATraceCore::GetThrowChargeScaleForHold(ChargeSeconds * 5.f);
		Check(FMath::IsNearlyEqual(Overheld, FullPower, 0.001f),
			FString::Printf(TEXT("holding 5x the charge time still throws at x%.3f - it clamps"), Overheld));
	}

	// 5. Monotonic: a longer hold is never a weaker throw. Trivially true of a line, and the check that
	//    would catch a future non-linear curve being dropped in with the sign wrong.
	{
		bool bMonotonic = true;
		float Previous = -1.f;
		for (int32 Step = 0; Step <= 40; ++Step)
		{
			const float Scale = ATraceCore::GetThrowChargeScaleForHold(ChargeSeconds * Step / 20.f);
			bMonotonic &= (Scale >= Previous - 0.0001f);
			Previous = Scale;
		}
		Check(bMonotonic, TEXT("a longer hold is never a weaker throw"));
	}

	// 6. The inverse round-trips, which is what the bots depend on: they pick a POWER and have to turn
	//    it into a HOLD. A round-trip error here is a bot that throws short or long by that error.
	{
		bool bRoundTrips = true;
		float WorstError = 0.f;
		for (int32 Step = 0; Step <= 10; ++Step)
		{
			const float WantedScale = FMath::Lerp(Floor, FullPower, static_cast<float>(Step) / 10.f);
			const float Hold = ATraceCore::GetThrowHoldSecondsForScale(WantedScale);
			const float GotScale = ATraceCore::GetThrowChargeScaleForHold(Hold);
			WorstError = FMath::Max(WorstError, FMath::Abs(GotScale - WantedScale));
			bRoundTrips &= FMath::IsNearlyEqual(GotScale, WantedScale, 0.002f);
		}
		Check(bRoundTrips, FString::Printf(
			TEXT("power -> hold -> power round-trips for the bots (worst error %.5f)"), WorstError));
	}

	if (Fails == 0)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBCharge] ===== ALL %d CHECKS PASSED (spec v13 §6) ====="), Passes);
	}
	else
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBCharge] ===== %d PASSED, %d FAILED (spec v13 §6) ====="), Passes, Fails);
	}
}

static FAutoConsoleCommand GTraceModeBChargeTestCmd(
	TEXT("Trace.ModeB.ChargeTest"),
	TEXT("MODE B, spec v13 §6. Checks the SHIPPED charge curve: an instant click is low but not zero, a ")
	TEXT("full hold reaches exactly the current throw momentum, the ramp between them is linear, it ")
	TEXT("clamps, and the inverse the bots use round-trips."),
	FConsoleCommandDelegate::CreateStatic(&RunModeBChargeTest));

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

ATraceCore::FSurfaceRuleStats ATraceCore::SurfaceStats;

static FAutoConsoleCommand GTraceModeBSurfaceStatsCmd(
	TEXT("Trace.ModeB.SurfaceStats"),
	TEXT("MODE B, spec v7 §4. Prints how many turnovers came off the FLOOR, how many off the TOP of an ")
	TEXT("object, how many wall bounces were resolved, and how many times a Core was refused a resting ")
	TEXT("place on a wall."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		const ATraceCore::FSurfaceRuleStats& Stats = ATraceCore::SurfaceStats;
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] SURFACE RULE TALLY (spec v7 §4): turnovers off the ground %d | turnovers off the ")
			TEXT("TOP of an object %d | wall bounces %d (of which %d were refused a resting place) | ")
			TEXT("rest-probe rescues %d | threshold %.0f deg from vertical (normal Z >= %.3f)"),
			Stats.GroundTurnovers, Stats.TopTurnovers, Stats.WallBounces, Stats.WallRestRefusals,
			Stats.RestProbeRescues,
			TraceModeBTuning::SurfaceMaxSlopeDegrees(), TraceModeBTuning::SurfaceUpNormalZ());

		// SPEC v13 §8, printed with it rather than under its own command: "how many turnovers" and
		// "how many of them happened in mid-air" are the same question asked twice, and separating the
		// two readouts is how somebody ends up quoting one without the other.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] LANDING RULE (spec v13 §8): MID-AIR TURNOVERS %d (fired above %.0f uu/s AND ")
			TEXT("below %.0f deg to the surface - this must be 0) | landed turnovers %d | glancing ")
			TEXT("contacts refused a landing %d | rule=%s, min descent %.0f deg, min flight %.2fs"),
			Stats.MidAirTurnovers, CVarModeBMidAirTurnoverSpeed.GetValueOnAnyThread(),
			CVarModeBMidAirTurnoverDegrees.GetValueOnAnyThread(),
			Stats.LandedTurnovers, Stats.GlancingContactsRejected,
			(!TraceModeBLegacyLandingRule()) ? TEXT("v13 (an actual landing)")
				: TEXT("PRE-v13 (any upward normal) - THE BUG, ARMED"),
			TraceModeBTuning::LandingMinDescentDegrees(), TraceModeBTuning::LandingMinFlightSeconds());

		// SPEC v19 §1.5, printed on the same command for the same reason §8 is: "how many turnovers"
		// and "how many of them happened with clear air under the ball" belong on one line.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] GROUNDED RULE (spec v19 §1.5): UNGROUNDED TURNOVERS %d (this must be 0) | ")
			TEXT("grounded turnovers %d | worst gap under any turnover %.0f uu | landings refused for ")
			TEXT("not being on top of anything %d | rule=%s, slack %.0f uu, probe %.0f uu, orb radius %.0f uu"),
			Stats.UngroundedTurnovers, Stats.GroundedTurnovers, Stats.WorstTurnoverGapUU,
			Stats.UngroundedLandingsRefused,
			(!TraceModeBLegacyGroundedRule()) ? TEXT("v19 (the orb must be on it)")
				: TEXT("PRE-v19 (any up-facing contact) - THE BUG, ARMED"),
			CVarModeBTurnoverContactSlack.GetValueOnAnyThread(),
			CVarModeBTurnoverContactProbe.GetValueOnAnyThread(),
			TraceModeBVisibleOrbRadius);
	}));

ATraceCore::FCatchContestStats ATraceCore::CatchStats;

static FAutoConsoleCommand GTraceModeBCatchStatsCmd(
	TEXT("Trace.ModeB.CatchStats"),
	TEXT("MODE B, spec v13 §5. Prints how often the magnet zone was CONTESTED (two or more eligible ")
	TEXT("players in range at once), how many of those contests were cross-team, how often the pull ")
	TEXT("moved between players, and how often the hysteresis refused a switch."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		const ATraceCore::FCatchContestStats& Stats = ATraceCore::CatchStats;
		const int32 Frames = Stats.UncontestedFrames + Stats.ContestedFrames;
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] CONTESTED MAGNET TALLY (spec v13 §5): magnet frames %d (%d uncontested, %d ")
			TEXT("contested, %d of them cross-team) | contests entered %d | largest contested set %d | ")
			TEXT("target switches %d | switches held off by hysteresis %d | margin %.0f uu, radius %.0f uu"),
			Frames, Stats.UncontestedFrames, Stats.ContestedFrames, Stats.CrossTeamContestedFrames,
			Stats.Contests, Stats.MaxContenders, Stats.TargetSwitches, Stats.HysteresisHolds,
			TraceModeBTuning::CatchContestHysteresis(), TraceModeBTuning::CatchRadius());
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

		// --- SPEC v6 §4.3: the goal is a CIRCLE, so the box above was only the broad phase. --------
		//
		// The box is the hoop's bounding slab and its corners are WALL. Two ways in, both of which
		// have to be caught or the rule reads as a broken trigger:
		//
		//   CROSSING  the segment passes through the ring's plane. Solved for the exact crossing
		//             point rather than sampled, because that is the whole reason the thrown test is
		//             swept at all: a Core at 2200 uu/s covers 37 uu a frame at 60 Hz and a great
		//             deal more on a hitching server, and a point test at the frame boundary can be
		//             on both sides of a 2000 uu hoop without ever having been measured inside it.
		//   INSIDE    the end point is within the slab AND within the disc. This is the carrier
		//             standing in the mouth at the top of the ramp, where From == To and there is no
		//             crossing to solve.
		if (Goal.bRing && Goal.RingRadius > 0.f)
		{
			const FVector Axis = Goal.RingAxis;
			const double FromSide = FVector::DotProduct(From - Goal.RingCentre, Axis);
			const double ToSide = FVector::DotProduct(To - Goal.RingCentre, Axis);

			bool bThroughTheHoop = false;

			if ((FromSide <= 0.0) != (ToSide <= 0.0))
			{
				const double Denominator = FromSide - ToSide;
				const double Alpha = FMath::IsNearlyZero(Denominator) ? 0.0 : (FromSide / Denominator);
				const FVector Crossing = From + (To - From) * FMath::Clamp(Alpha, 0.0, 1.0);

				const FVector Offset = Crossing - Goal.RingCentre;
				const FVector InPlane = Offset - Axis * FVector::DotProduct(Offset, Axis);
				bThroughTheHoop = InPlane.SizeSquared() <= static_cast<double>(Goal.RingRadius) * Goal.RingRadius;
			}

			if (!bThroughTheHoop)
			{
				const FVector Offset = To - Goal.RingCentre;
				const FVector InPlane = Offset - Axis * FVector::DotProduct(Offset, Axis);
				bThroughTheHoop = Goal.Box.IsInside(To)
					&& InPlane.SizeSquared() <= static_cast<double>(Goal.RingRadius) * Goal.RingRadius;
			}

			if (!bThroughTheHoop)
			{
				continue;   // Inside the slab but outside the hoop: that is wall, not goal.
			}
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

bool ATraceCore::ThrowFromHolder(ATraceCharacter* Thrower, float HeldSeconds)
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

	// SPEC v8 §4. THE LAUNCH IS THE IMPULSE PLUS THE THROWER'S OWN MOTION, VERTICAL INCLUDED.
	//
	// Read off the SERVER's copy of the thrower, on the same frame and from the same actor the aim
	// came from, for the identical reason: a client-supplied velocity is a client-supplied launch. On
	// a listen server the host's pawn velocity is exact; for a joining client the server has already
	// executed their moves through the movement component, so the velocity here is the authoritative
	// result of the very moves that produced the jump - not an interpolated proxy's guess. That is
	// what makes this fix land the same way for a client as for the host (spec v8 §0).
	//
	// SPEC v13 §6. THE CHARGE IS MEASURED ON THE SERVER, from the hold @p HeldSeconds carries - which
	// RequestPassInput computed from ITS OWN press and release timestamps, never from anything a
	// client said about how long it held down a button. A client-supplied hold is a client-supplied
	// launch speed, and it would be the single most valuable number in this game to lie about.
	// SPEC v19 §3: the THROWER is passed so Mortimer's longer useful hold is honoured here, on the
	// server, from the server's own press/release timestamps. Everybody else's scale is 1.0.
	const float ChargeScale = GetThrowChargeScaleForHold(HeldSeconds, Thrower);

	const float Speed = TraceModeBTuning::ThrowSpeed();
	const FVector Impulse = (ThrowDirection * Speed
		+ FVector::UpVector * (Speed * TraceModeBTuning::ThrowUpBias())) * ChargeScale;

	// THE CHARGE SCALES THE IMPULSE; THE INHERITED VELOCITY IS ADDED ON TOP OF IT, UNSCALED. See
	// ComputeThrowLaunchVelocity, the published copy of this expression, which states why at length.
	const FVector Inherited = GetInheritedThrowVelocity(Thrower);
	const FVector LaunchVelocity = Impulse + Inherited;

	// The published predictor and the real launch, on the same frame, from the same inputs. Every
	// throw solver in the game aims with the predictor, so a drift between the two is a match in which
	// no bot can hit anything - the exact failure spec v8 §4 recorded when the bots rebuilt the
	// formula themselves. It costs one vector compare and it fails loudly rather than by 2000 uu at
	// the far end of a shot.
	ensureMsgf(LaunchVelocity.Equals(ComputeThrowLaunchVelocity(Thrower, ThrowDirection, ChargeScale), 1.0),
		TEXT("[ModeB] ThrowFromHolder and ComputeThrowLaunchVelocity disagree: %s vs %s"),
		*LaunchVelocity.ToCompactString(),
		*ComputeThrowLaunchVelocity(Thrower, ThrowDirection, ChargeScale).ToCompactString());

	// NOT ALSO OFFSET BY THE THROWER'S MOTION. The muzzle is a fixed 70 uu ahead of the eye and stays
	// there: the eye position is already this frame's, so the launch point already travels with the
	// thrower. Adding velocity * dt here as well would double-count the frame.
	const FVector LaunchLocation = Thrower->GetPawnViewLocation()
		+ ThrowDirection * TraceModeBTuning::ThrowMuzzleForward;

	// Broken into its parts for Trace.ModeB.ThrowMomentum. "It doesn't seem to keep momentum" is a
	// claim about a launch velocity, and one aggregate number cannot answer it.
	const FVector ThrowerVelocity = Thrower->GetVelocity();
	LastThrow.bValid = true;
	LastThrow.ImpulseSpeed = static_cast<float>(Impulse.Size());
	LastThrow.InheritedSpeed = static_cast<float>(Inherited.Size());
	LastThrow.LaunchSpeed = static_cast<float>(LaunchVelocity.Size());
	LastThrow.LaunchVelocityZ = static_cast<float>(LaunchVelocity.Z);
	LastThrow.ThrowerSpeed2D = static_cast<float>(ThrowerVelocity.Size2D());
	LastThrow.ThrowerVelocityZ = static_cast<float>(ThrowerVelocity.Z);
	LastThrow.bThrowerFalling = Thrower->GetCharacterMovement() != nullptr
		&& Thrower->GetCharacterMovement()->IsFalling();
	LastThrow.Inheritance = TraceModeBTuning::ThrowInheritance();
	LastThrow.HeldSeconds = FMath::Max(0.f, HeldSeconds);
	LastThrow.ChargeScale = ChargeScale;
	LastThrow.ThrowerName = GetNameSafe(Thrower);

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
	ClearThrowCharge(nullptr);   // Spec v13 §6: this charge has now been spent. Not cancelled - spent.
	ReleaseHolder();

	bLoose = true;
	bLooseAtRest = false;
	bLooseFromThrow = true;   // Spec v6 §4.2: this Core, and only this kind, turns over on landing.
	CatchZoneTarget = nullptr;
	bCatchZoneContested = false;
	LastContactServerTime = -1.f;
	ClearPendingTurnover();   // Spec v19 §1.5: this throw is awarded on its own landing, not the last one's.
	// Spec v25 §2, for the same reason and set in the same breath: this throw earns its own turnover.
	// ClearLooseState() clears the latch on every path that ENDS a flight; the two paths that START
	// one set the loose fields by hand and must clear it themselves, or a throw taken straight out of
	// a turnover would be refused a landing of its own.
	bTurnoverRegisteredThisFlight = false;
	LooseFromTeam = ThrowerTeam;
	LooseThrower = Thrower;
	LooseStartServerTime = Now;
	LooseLocation = LaunchLocation;
	LooseVelocity = LaunchVelocity;

	// A loose Core belongs to nobody, so it must be visible to everybody - including the thrower,
	// whose own camera had it hidden while they carried it (bOwnerNoSee, resolved through the actor
	// owner chain that ReleaseHolder just cleared).
	//
	// SPEC v10 §10, the LAUNCH path. A throw jumps the Core from the holder's head to a muzzle point
	// 70 uu ahead of their eye - short, but still a discontinuity, and on a client it is the frame the
	// dead reckoner starts from. Sending it explicitly means the client restarts its integration from
	// the server's launch point rather than from wherever its own copy happened to be attached.
	ServerTeleport(LaunchLocation, TEXT("throw launch"));
	ApplyAttachment();
	UpdateVisuals();
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] THROW by %s (%s) at %.0f uu/s from %s | v13 §6: held %.2fs -> charge x%.2f (floor ")
		TEXT("%.2f, full at %.2fs) | v8 §4: charged impulse %.0f + inherited %.0f (thrower %.0f uu/s ")
		TEXT("horiz, %+.0f vert, %s, x%.2f), launch Z %+.0f"),
		*GetNameSafe(Thrower), *TraceTeamName(ThrowerTeam).ToString(),
		LaunchVelocity.Size(), *LaunchLocation.ToCompactString(),
		LastThrow.HeldSeconds, ChargeScale,
		TraceModeBTuning::ThrowChargeFloor(), TraceModeBTuning::ThrowChargeSeconds(),
		LastThrow.ImpulseSpeed, LastThrow.InheritedSpeed,
		LastThrow.ThrowerSpeed2D, LastThrow.ThrowerVelocityZ,
		LastThrow.bThrowerFalling ? TEXT("AIRBORNE") : TEXT("grounded"),
		LastThrow.Inheritance, LastThrow.LaunchVelocityZ);

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

	// --- SPEC v25 §2. A COMPLETED PULL OWNS THE TICK, AND EVERYTHING BELOW IS SKIPPED. ------------
	//
	// A Core being magnetically delivered is not falling: it has no gravity, no bounce, no landing, no
	// catch magnet and no first-contact pickup, because all five of those would fight the delivery for
	// the same two fields. Ahead of the integration rather than beside it, so the reader cannot miss
	// that this is a different mode of motion and not a modifier on the ordinary one.
	if (ServerTickPullTravel(Step))
	{
		return;
	}

	const FVector StartLocation = LooseLocation;

	// SPEC v7 §4. "Did it land on something you could stand on" — set by a contact this frame, or by
	// the at-rest probe below. One flag, two ways of establishing the same geometric fact, so there is
	// still exactly one turnover call site.
	bool bLandedOnSurface = false;
	bool bLandedByRestProbe = false;
	FVector SurfacePoint = FVector::ZeroVector;
	FVector SurfaceNormal = FVector::UpVector;

	// SPEC v13 §8. The speed the Core was travelling at when it met the surface — captured before the
	// bounce overwrites it, and carried out to the turnover so the tally can say whether possession
	// changed on a Core that had stopped or on one that was still flying. Zero when the landing came
	// from the at-rest probe, which by definition means it had stopped.
	double ArrivalSpeed = 0.0;

	/** sin of the angle between the flight and the surface plane. 1 = straight down onto it. */
	double ArrivalSin = 1.0;

	// --- 0. SPEC v6 §4.1: the catch zone. ---------------------------------------------------------
	//
	// Ahead of the integration, so this frame's motion is already the curved one: applying it
	// afterwards would move the Core straight and then bend the velocity for next frame, which is a
	// frame of lag on the one mechanic whose whole purpose is to feel immediate.
	ServerApplyCatchZone(Step);

	// --- 1. Integrate. No UProjectileMovementComponent: see the header. --------------------------
	if (!bLooseAtRest)
	{
		const double GravityZ = static_cast<double>(World->GetGravityZ())
			* static_cast<double>(TraceModeBTuning::GravityScale());

		LooseVelocity = FVector(LooseVelocity) + FVector(0.0, 0.0, GravityZ * Step);

		// SPEC v13 §8. The speed the Core ARRIVES with, captured before any bounce can overwrite it.
		// The landing test is a question about the approach, and after MirrorByVector there is no
		// approach left to ask about.
		const double Speed3DBeforeContact = FVector(LooseVelocity).Size();

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
			// THE NORMAL THE RULE IS ASKED ABOUT. A sweep that STARTS inside geometry reports no
			// meaningful ImpactNormal - the usable one is Hit.Normal, the depenetration direction,
			// which points back out of whatever the Core is buried in. Reading ImpactNormal there gave
			// the surface test a zero vector to classify, and a zero vector is a wall by arithmetic:
			// the Core would be refused a landing on the one geometry it could not escape.
			const FVector ContactNormal = (Hit.bStartPenetrating && !Hit.Normal.IsNearlyZero())
				? Hit.Normal.GetSafeNormal()
				: Hit.ImpactNormal;

			// Land just off the surface so the next sweep does not start inside it. A penetrating hit
			// needs pushing all the way back out, not 2 uu: leaving it embedded means every following
			// sweep also starts penetrating, which is a Core that can never move again.
			const double PushOut = Hit.bStartPenetrating
				? (static_cast<double>(Hit.PenetrationDepth) + 2.0) : 2.0;
			LooseLocation = Hit.Location + ContactNormal * PushOut;

			// SPEC v6 §4.2, GENERALISED BY v7 §4. "It hits the ground" = it landed on something you
			// could stand on — the floor, a corner bank, the approach ramp, or the TOP of a cover
			// block, which is the case the user reported and which this test already covers because it
			// asks the NORMAL and not the actor. A graze off a wall or a goal spoke is not a landing
			// and leaves the Core live, which is what keeps the rule legible from inside the game: it
			// fires when the throw comes down, not when it touches anything at all. Recorded here and
			// acted on after the goal test, because a Core that scores and a Core that lands are
			// resolved in that order.
			const float UpNormalZ = TraceModeBTuning::SurfaceUpNormalZ();
			const bool bUpwardFacing = (ContactNormal.Z >= UpNormalZ);

			SurfacePoint = Hit.bStartPenetrating ? Hit.Location : Hit.ImpactPoint;
			SurfaceNormal = ContactNormal;
			ArrivalSpeed = Speed3DBeforeContact;

			// --- SPEC v13 §8. AN UPWARD NORMAL IS NOT YET A LANDING. --------------------------------
			//
			// "Sometimes the core is thrown and it turns over before it touches the ground." Until v13
			// the next line was `bLandedOnSurface = bUpwardFacing;` and nothing else - so a Core
			// crossing the arena at 2200 uu/s whose sphere clipped the flat top of a cover block, or one
			// of the corner cove's horizontal treads, changed possession in mid-flight. See the §8
			// tuning block above for the whole diagnosis.
			//
			// TWO SIGNALS, taken BEFORE the bounce is applied because both are facts about the ARRIVAL:
			//
			//   DESCENT  how steeply the flight met the surface, as an angle rather than a speed. A
			//            Core dropping onto a crate arrives at 25-90 degrees to the surface; a Core
			//            skimming past the same crate arrives at 2-8. A speed threshold cannot separate
			//            those - they can be travelling at identical speeds - and an angle can.
			//   FLIGHT   how long it has been in the air, which closes hypothesis (c): a sweep that is
			//            blocked on the launch frame, before the Core has cleared the thrower's own
			//            footing, is not a landing however vertical its normal looks.
			//
			// A contact that STOPS the Core is a landing regardless of both, and that is the third
			// clause below: bComesToRest. It has to be decided HERE, alongside the other two, rather
			// than left to the resting block further down - otherwise the verdict this frame's log
			// prints ("GRAZE") disagrees with the turnover the same frame fires, which is exactly the
			// kind of instrument that makes a reader distrust a run that was actually correct.
			const double ApproachSpeed = -FVector::DotProduct(FVector(LooseVelocity), ContactNormal);
			ArrivalSin = (Speed3DBeforeContact > 1.0)
				? (ApproachSpeed / Speed3DBeforeContact) : 1.0;

			const FVector Reflected = FVector(LooseVelocity).MirrorByVector(ContactNormal)
				* TraceModeBTuning::Bounce();

			const bool bLegacyLandingRule = TraceModeBLegacyLandingRule();
			const bool bClearedTheThrower =
				(Now - LooseStartServerTime) >= TraceModeBTuning::LandingMinFlightSeconds();
			const bool bArrivedOnIt = (ArrivalSin >= static_cast<double>(TraceModeBTuning::LandingMinDescentSin()));
			const bool bComesToRest = bUpwardFacing
				&& (Reflected.Size() < TraceModeBTuning::RestSpeed());

			// --- SPEC v19 §1.5. AN UPWARD NORMAL IS NOT YET SOMETHING TO STAND ON. ------------------
			//
			// "ENSURE the ball does not turnover until it actually visibly touches the ground or the
			// top of an obstacle." Measured DOWNWARD from the Core, with the drawn radius, AFTER the
			// push-out above has put the Core where it will actually be this frame — so this is the
			// gap a player would see on the screen and not a fact about the sweep that produced it.
			// See the §1.5 block near the top of this file for why the lip of a block passes every
			// other test in this function.
			//
			// Computed only for an upward-facing contact: a wall bounce is not a landing candidate and
			// must not pay for a sweep.
			double ContactSupportGap = 0.0;
			FVector ContactSupportPoint = FVector::ZeroVector;
			bool bVisiblyOnTop = true;
			if (bUpwardFacing)
			{
				ContactSupportGap = MeasureVisibleSupportGap(ContactSupportPoint);
				bVisiblyOnTop = TraceModeBLegacyGroundedRule()
					|| (ContactSupportGap <= static_cast<double>(CVarModeBTurnoverContactSlack.GetValueOnAnyThread()));
			}

			// bLegacyLandingRule is the A/B arm (Trace.ModeB.LandingRule 0) and restores the pre-v13
			// behaviour EXACTLY, which is what lets the §8 reproduction go red on this build.
			// bVisiblyOnTop is the v19 §1.5 arm (Trace.ModeB.GroundedTurnover 0) and is deliberately
			// OUTSIDE that bracket, as a separate AND: the two arms disarm different rules, and a
			// single combined switch could not tell the reader which one a red run had proven.
			const bool bContactIsLanding = bUpwardFacing && bVisiblyOnTop
				&& (bLegacyLandingRule || bComesToRest || (bArrivedOnIt && bClearedTheThrower));

			bLandedOnSurface = bContactIsLanding;

			if (bUpwardFacing && !bVisiblyOnTop)
			{
				// THE v19 §1.5 BUG'S OWN COUNTER: an up-facing contact with clear air under the ball.
				// Every one of these was a mid-air turnover before this pass.
				++SurfaceStats.UngroundedLandingsRefused;

				UE_LOG(LogTraceGame, Verbose,
					TEXT("[ModeB] spec v19 §1.5: refused a landing at %s - the contact normal points up ")
					TEXT("but the orb has %.0f uu of clear air under it (slack %.0f). Keeping it falling."),
					*FVector(LooseLocation).ToCompactString(), ContactSupportGap,
					CVarModeBTurnoverContactSlack.GetValueOnAnyThread());
			}
			else if (bUpwardFacing && !bContactIsLanding)
			{
				// THE v13 §8 BUG'S OWN COUNTER. Every one of these was a turnover before v13.
				++SurfaceStats.GlancingContactsRejected;
			}

			LooseVelocity = Reflected;

			if (!bUpwardFacing)
			{
				++SurfaceStats.WallBounces;
				UE_LOG(LogTraceGame, Verbose,
					TEXT("[ModeB] loose Core BOUNCED off a wall at %s (normal %s, %.0f deg from up) - no turnover."),
					*SurfacePoint.ToCompactString(), *ContactNormal.ToCompactString(),
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(ContactNormal.Z, -1.0, 1.0))));
			}

			// SPEC v13 §8's instrument. EVERY contact a thrown Core makes, with the four numbers the
			// report is about - how high, how fast, what normal, and what the rule made of it. This is
			// how the mid-air turnover was found, and it is left in because a rule about a geometric
			// event that cannot be watched is a rule nobody can check.
			if (TraceModeBTurnoverLogEnabled() && bLooseFromThrow)
			{
				double FloorZ = 0.0;
				if (const ATraceArenaBuilder* LogArena = ATraceArenaBuilder::Get(World))
				{
					const FBox LogBox = LogArena->GetFieldBounds();
					FloorZ = (LogBox.IsValid != 0) ? LogBox.Min.Z : 0.0;
				}

				// THE VERDICT IS READ OFF bContactIsLanding FIRST, and that ordering is a correction
				// rather than a tidy-up: before v19 the "SETTLED" arm came first, and now that a
				// contact can come to rest on a lip AND be refused, a chain that reported "SETTLED on
				// it - turnover" would be printing the opposite of what the same frame did.
				UE_LOG(LogTraceGame, Display,
					TEXT("[ModeBTurnover] CONTACT at %s | %.0f uu above the floor | speed %.0f -> %.0f uu/s | ")
					TEXT("normal %s (%.0f deg from up, up-facing=%d) | arrival %.1f deg to the surface ")
					TEXT("(needs %.0f) | airborne %.3fs (needs %.3f) | orb %.0f uu above what is under it ")
					TEXT("(needs <= %.0f) | VERDICT: %s"),
					*SurfacePoint.ToCompactString(), SurfacePoint.Z - FloorZ,
					Speed3DBeforeContact, Reflected.Size(),
					*ContactNormal.ToCompactString(),
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(ContactNormal.Z, -1.0, 1.0))),
					bUpwardFacing ? 1 : 0,
					FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(ArrivalSin, -1.0, 1.0))),
					TraceModeBTuning::LandingMinDescentDegrees(),
					Now - LooseStartServerTime, TraceModeBTuning::LandingMinFlightSeconds(),
					ContactSupportGap, CVarModeBTurnoverContactSlack.GetValueOnAnyThread(),
					!bUpwardFacing ? TEXT("WALL - bounce")
						: (bContactIsLanding
							? (bComesToRest ? TEXT("SETTLED on it - turnover") : TEXT("LANDING - turnover"))
							: (!bVisiblyOnTop ? TEXT("NOT ON TOP OF IT (v19 §1.5) - keeps falling")
								: (!bClearedTheThrower ? TEXT("launch-frame contact - flies on")
									: TEXT("GRAZE - flies on")))));
			}

			LastContactServerTime = Now;

			if (Reflected.Size() < TraceModeBTuning::RestSpeed())
			{
				// SPEC v7 §4: A WALL IS NOT A RESTING PLACE.
				//
				// "Walls should not [turn over], the core should bounce off those." Declaring the Core
				// at rest here on a vertical face would leave it hanging in mid-air against the wall
				// with the integration switched off - the same stuck-forever failure the user reported
				// one surface over, and one that no probe can rescue because nothing is holding it up.
				// So a slow bounce off a wall keeps only its DOWNWARD component and stays live: gravity
				// takes it to something horizontal, and the rule fires there instead.
				// bComesToRest is this same pair of conditions, decided further up so that the landing
				// verdict and the log line are computed from one expression rather than two that could
				// drift. bLandedOnSurface has ALREADY been set true by it - see SPEC v13 §8 above - so
				// this block only has to do the resting itself.
				//
				// SPEC v19 §1.5 EXTENDS THE SAME SENTENCE TO A LIP. "A wall is not a resting place"
				// because nothing is holding the Core up there; the top EDGE of a block is the same
				// statement with a normal that happens to point upward, and leaving the Core parked on
				// one would hang it in mid-air against the corner with the integration switched off -
				// the identical stuck-forever failure. So it takes the identical treatment: keep the
				// downward component, stay live, and land on something that is actually underneath.
				if (bUpwardFacing && bVisiblyOnTop)
				{
					LooseVelocity = FVector::ZeroVector;
					bLooseAtRest = true;

					UE_LOG(LogTraceGame, Verbose, TEXT("[ModeB] loose Core came to rest at %s"),
						*FVector(LooseLocation).ToCompactString());
				}
				else
				{
					// WallRestRefusals stays what its name says — a WALL refused a resting place. A
					// v19 §1.5 lip refusal has already been counted, once, by UngroundedLandingsRefused
					// in the verdict block above; counting it a second time under a wall's name would
					// make both numbers lie.
					if (!bUpwardFacing)
					{
						++SurfaceStats.WallRestRefusals;
					}

					LooseVelocity = FVector(0.0, 0.0, FMath::Min(0.0, Reflected.Z));
				}
			}
		}
		else
		{
			LooseLocation = Desired;
		}
	}

	// --- 1b. SPEC v7 §4: ASK THE SAME QUESTION OF A CORE THAT HAS STOPPED. -------------------------
	//
	// THIS IS THE FIX FOR "the core gets stuck up top of an object". The contact test above only ever
	// runs on the frame a sweep is blocked, and a Core can reach rest on a frame whose impact normal
	// was an edge or a corner - after which the integration block is skipped forever and the question
	// is never asked again. Probing straight down under a Core at rest asks it of whatever is actually
	// holding the Core up, which is the surface the rule is about: the floor, or the roof of a block.
	//
	// Cheap by construction: it runs only while the Core is at rest AND has not yet turned over, which
	// in a real match is the handful of frames between landing and the turnover firing.
	//
	// SPEC v13 §8 HYPOTHESIS (b) — "the at-rest probe firing while the Core still has speed" — CLOSED
	// HERE, BY MEASUREMENT RATHER THAN BY ARGUMENT. It cannot happen today: bLooseAtRest is only ever
	// set on the two lines above, and both zero the velocity on the line before. But "cannot happen"
	// is exactly the kind of invariant a later edit breaks silently, and a Core declared at rest while
	// still travelling would hand possession over in mid-air by a completely different route from the
	// one this pass fixed. So the invariant is now ASSERTED: if it is ever false the Core is put back
	// in flight and the log says so, rather than a turnover firing on a moving Core.
	if (bLooseAtRest && !FVector(LooseVelocity).IsNearlyZero(1.0))
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeB] spec v13 §8: the Core was flagged AT REST at %s while still travelling at %.0f ")
			TEXT("uu/s - refusing the landing and resuming flight. This is the mid-air turnover's ")
			TEXT("hypothesis (b) and it should never be reachable."),
			*FVector(LooseLocation).ToCompactString(), FVector(LooseVelocity).Size());
		bLooseAtRest = false;
	}

	if (!bLandedOnSurface && bLooseAtRest)
	{
		FVector RestPoint = FVector::ZeroVector;
		FVector RestNormal = FVector::UpVector;
		const bool bSupported = ServerProbeRestingSurface(RestPoint, RestNormal);

		// SPEC v19 §1.5, THE SECOND HALF OF THE SAME RULE. ServerProbeRestingSurface answers with the
		// COLLISION sphere and accepts anything within 24 uu, so a Core wedged against the corner of a
		// block reads as "supported" — by the corner, with the floor several hundred uu below. This is
		// the same question asked with the DRAWN radius, straight down: is the ball a player can see
		// resting on something? Measured once here and reused, so the probe and the rule cannot
		// disagree about the same frame.
		FVector RestSupportPoint = FVector::ZeroVector;
		const double RestSupportGap = MeasureVisibleSupportGap(RestSupportPoint);
		const bool bRestVisiblyOnTop = TraceModeBLegacyGroundedRule()
			|| (RestSupportGap <= static_cast<double>(CVarModeBTurnoverContactSlack.GetValueOnAnyThread()));

		if (bSupported && bRestVisiblyOnTop)
		{
			bLandedOnSurface = (RestNormal.Z >= TraceModeBTuning::SurfaceUpNormalZ());
			bLandedByRestProbe = bLandedOnSurface;
			SurfacePoint = RestPoint;
			SurfaceNormal = RestNormal;
		}
		else
		{
			// Nothing under it. A Core "at rest" in mid-air is a contradiction and the state that
			// produced every stuck-Core report; put it back in flight and let gravity resolve it.
			// Since v19 §1.5 that includes a Core "at rest" on a lip it is not actually on top of —
			// the same contradiction, one geometry over, and the same answer.
			bLooseAtRest = false;

			if (bSupported)
			{
				++SurfaceStats.UngroundedLandingsRefused;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeB] loose Core was at rest at %s with %s under it (orb %.0f uu clear, slack %.0f) ")
				TEXT("- resuming flight (spec v7 §4 / v19 §1.5)."),
				*FVector(LooseLocation).ToCompactString(),
				bSupported ? TEXT("nothing it is sitting ON") : TEXT("nothing at all"),
				RestSupportGap, CVarModeBTurnoverContactSlack.GetValueOnAnyThread());
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

	// --- 2b. SPEC v6 §4.2 / v7 §4: it came down on a surface, so it is the other team's. -----------
	//
	// AHEAD OF THE PICKUP POLL, and that ordering IS the rule. The v4 model was "a Core on the ground
	// stays live for first contact", and leaving the poll first would keep exactly that behaviour
	// whenever anybody happened to be standing near where it landed - i.e. it would fire the new rule
	// only when it did not matter. A throw that comes down is a turnover, full stop.
	//
	// ONE call site for both ways of establishing the landing (contact this frame, or at rest on
	// something horizontal), because they are the same rule about the same geometry - the spec's
	// instruction was to generalise the ground path, not to grow a second one beside it.
	//
	// SPEC v19 §1.5 SPLIT THE INSTANT IN TWO, and this is where. "ENSURE the ball does not turnover
	// until it actually visibly touches the ground or the top of an obstacle." Until this pass the
	// landing and the award were the same TICK - measured, twelve times out of twelve, by the CONTACT
	// and SURFACE TURNOVER log lines carrying the identical frame number - so rendering, which happens
	// after the tick, never once drew the ball in contact. So the landing is now LATCHED here and the
	// award happens further down, after the ball has actually been on screen sitting on the thing it
	// hit. The rule is unchanged; only the moment it fires has moved, and it has moved to the moment
	// the sentence describes.
	// THE LooseFromTeam CLAUSE IS NOT BELT AND BRACES. ServerSurfaceTurnover declines outright when
	// there is no team to award the Core TO ("nobody threw it" — a debug launch, or a Core left loose
	// across a mode switch), and the settle below suppresses the pickup poll while a landing is
	// pending. Latching a landing the rule was always going to decline would therefore hold the Core
	// unpickupable until the reset timer expired. Asking the same question one line earlier means the
	// latch is only ever taken for a throw the rule actually has an opinion about.
	// SPEC v25 §2 ADDS ONE CLAUSE, bTurnoverRegisteredThisFlight, AND IT IS NOT DEFENSIVE. Under the
	// new rule the Core stays lying on the surface it landed on, so this test - which is a question
	// about the geometry, not about the event - is true on every following frame too. Without the
	// latch a resting Core would re-register its turnover once per tick and the 5 s window would never
	// end. The latch is cleared by ClearLooseState(), i.e. by every path that ends a flight.
	const bool bTurnoverRuleArmed = bLooseFromThrow
		&& !bTurnoverRegisteredThisFlight
		&& (LooseFromTeam != ETraceTeam::None)
		&& (CVarModeBGroundTurnover.GetValueOnAnyThread() != 0);

	if (bTurnoverRuleArmed && bLandedOnSurface)
	{
		if (PendingTurnoverLandedServerTime < 0.f)
		{
			PendingTurnoverLandedServerTime = Now;
			PendingTurnoverLandedFrame = GFrameCounter;

			// SPEC v13 §8's numbers, captured HERE and not at the award. They describe how the Core met
			// the surface, and a few frames later the live velocity answers a different question - see
			// the header fields. Reading the fresh one would have quietly retired the §8 counter.
			PendingTurnoverArrivalSpeed = ArrivalSpeed;
			PendingTurnoverArrivalSin = ArrivalSin;
			bPendingTurnoverByRestProbe = bLandedByRestProbe;

			if (TraceModeBTurnoverLogEnabled())
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[ModeBTurnover] LANDED at %s on frame %llu - holding possession for %.2fs so the ")
					TEXT("ball is actually drawn on it before it changes hands (spec v19 §1.5)."),
					*FVector(LooseLocation).ToCompactString(),
					static_cast<uint64>(PendingTurnoverLandedFrame),
					CVarModeBTurnoverSettleSeconds.GetValueOnAnyThread());
			}
		}

		// Refreshed every frame contact continues, so the log names the surface the Core is on NOW
		// rather than the first one it clipped on the way down.
		PendingTurnoverSurfacePoint = SurfacePoint;
		PendingTurnoverSurfaceNormal = SurfaceNormal;
	}

	if (bTurnoverRuleArmed && PendingTurnoverLandedServerTime >= 0.f)
	{
		// THE SETTLE. Trace.ModeB.GroundedTurnover 0 zeroes it, which is the pre-v19 build exactly and
		// the arm the reproduction goes RED on.
		const float SettleSeconds = TraceModeBLegacyGroundedRule()
			? 0.f : FMath::Max(0.f, CVarModeBTurnoverSettleSeconds.GetValueOnAnyThread());

		const float DwellSeconds = Now - PendingTurnoverLandedServerTime;
		const int32 DwellFrames = static_cast<int32>(
			FMath::Min<uint64>(GFrameCounter - PendingTurnoverLandedFrame, static_cast<uint64>(MAX_int32)));

		// BOTH, and neither on its own. Seconds is what "visibly" means to a person. The FRAME clause
		// is there for one specific case the clock cannot see: a hitching or slow-ticking server, where
		// a single tick can be longer than the whole settle. On such a frame the time test passes with
		// zero frames rendered - which is precisely the defect being fixed, arrived at by a different
		// road. Requiring at least one elapsed frame closes it for any tick rate.
		const bool bSettled = (SettleSeconds <= 0.f)
			|| ((DwellSeconds >= SettleSeconds) && (DwellFrames >= 1));

		// *** SPEC v19 §1.5's OTHER HEADLINE NUMBER: HOW HIGH WAS THE BALL WHEN POSSESSION CHANGED? ***
		//
		// Taken HERE - one line before the award - because that is the only instant the question is
		// about, and because after ServerSurfaceTurnover returns the Core may already have been
		// granted, scored and reset. Measured with the DRAWN radius (see MeasureVisibleSupportGap), so
		// it is the gap a player could check on the screen.
		//
		// It is also a GATE and not only a report: a Core that bounced high off its landing must not be
		// awarded at the top of the bounce just because the clock ran out. It comes down, and the award
		// waits for it - later, lower, and where a player watching can see why.
		FVector TurnoverSupportPoint = FVector::ZeroVector;
		const double TurnoverGap = MeasureVisibleSupportGap(TurnoverSupportPoint);
		const bool bOrbIsTouching = TraceModeBLegacyGroundedRule()
			|| (TurnoverGap <= static_cast<double>(CVarModeBTurnoverContactSlack.GetValueOnAnyThread()));

		if (bSettled && bOrbIsTouching)
		{
			// SPEC v13 §8's classification of the ARRIVAL, computed from the latched numbers.
			//
			// FAST **AND** SHALLOW, and both halves are load-bearing. A Core dropped straight onto the
			// floor arrives at 1500 uu/s and has unambiguously landed, so speed alone would report the
			// commonest correct turnover in the game as the bug; a Core resting on a crate is 400 uu up,
			// so height alone would report spec v7 §4 as the bug. What the user described in v13 was a
			// Core going PAST something - still travelling, and barely descending. That is the pair.
			//
			// Measured from the geometry of the event and NOT from the rule's verdict, so both arms of
			// every A/B compute it identically and the counter can go red.
			const double ArrivalDegrees = FMath::RadiansToDegrees(
				FMath::Asin(FMath::Clamp(PendingTurnoverArrivalSin, -1.0, 1.0)));
			const bool bStillFlying =
				(PendingTurnoverArrivalSpeed > static_cast<double>(CVarModeBMidAirTurnoverSpeed.GetValueOnAnyThread()))
				&& (ArrivalDegrees < static_cast<double>(CVarModeBMidAirTurnoverDegrees.GetValueOnAnyThread()));

			// Read before the call: a turnover can grant, score and reset the field under us.
			const bool bWasRestProbe = bPendingTurnoverByRestProbe;
			const double AwardGap = TurnoverGap;

			if (ServerSurfaceTurnover(PendingTurnoverSurfacePoint, PendingTurnoverSurfaceNormal))
			{
				(bStillFlying ? SurfaceStats.MidAirTurnovers : SurfaceStats.LandedTurnovers)++;
				((AwardGap <= static_cast<double>(CVarModeBTurnoverContactSlack.GetValueOnAnyThread()))
					? SurfaceStats.GroundedTurnovers : SurfaceStats.UngroundedTurnovers)++;
				SurfaceStats.WorstTurnoverGapUU =
					FMath::Max(SurfaceStats.WorstTurnoverGapUU, static_cast<float>(AwardGap));

				// THE §1.5 COUNTERS. DwellFrames is how many rendered frames a player had the ball on
				// screen in contact; zero is the reported bug and is what the pre-v19 arm produces for
				// every single turnover.
				((DwellFrames >= 1) ? SurfaceStats.SeenTurnovers : SurfaceStats.UnseenTurnovers)++;
				SurfaceStats.FewestTurnoverContactFrames = (SurfaceStats.FewestTurnoverContactFrames < 0)
					? DwellFrames : FMath::Min(SurfaceStats.FewestTurnoverContactFrames, DwellFrames);
				SurfaceStats.ShortestTurnoverDwellSeconds = (SurfaceStats.ShortestTurnoverDwellSeconds < 0.f)
					? DwellSeconds : FMath::Min(SurfaceStats.ShortestTurnoverDwellSeconds, DwellSeconds);

				if (DwellFrames < 1)
				{
					// LOUD, and unconditionally: this is the v19 §1.5 report's own signature. On the
					// shipped rule it is only reachable with Trace.ModeB.GroundedTurnover 0, so a line of
					// this in a normal run means the settle has regressed.
					UE_LOG(LogTraceGame, Warning,
						TEXT("[ModeBTurnover] UNSEEN TURNOVER: possession changed on the SAME FRAME the Core ")
						TEXT("landed, so no frame was ever drawn with the ball touching anything - spec v19 ")
						TEXT("§1.5 says this must not happen. Grounded rule = %s."),
						(!TraceModeBLegacyGroundedRule())
							? TEXT("v19 (settle then award)") : TEXT("PRE-v19, DELIBERATELY ARMED"));
				}

				if (AwardGap > static_cast<double>(CVarModeBTurnoverContactSlack.GetValueOnAnyThread()))
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[ModeBTurnover] UNGROUNDED TURNOVER: possession changed with the orb %.0f uu ")
						TEXT("above whatever is under it (slack %.0f) - spec v19 §1.5 says the ball must ")
						TEXT("visibly touch the ground or the top of an obstacle first. Grounded rule = %s."),
						AwardGap, CVarModeBTurnoverContactSlack.GetValueOnAnyThread(),
						(!TraceModeBLegacyGroundedRule())
							? TEXT("v19 (the orb must be on it)") : TEXT("PRE-v19, DELIBERATELY ARMED"));
				}

				if (bStillFlying)
				{
					// LOUD, and unconditionally: this is the v13 bug's own signature. On the shipped rule
					// it can only be reached with Trace.ModeB.LandingRule 0 (the A/B arm that restores the
					// pre-v13 behaviour on purpose), so a line of this in a normal run means the landing
					// rule has regressed and the report is worth more than the noise.
					UE_LOG(LogTraceGame, Warning,
						TEXT("[ModeBTurnover] MID-AIR TURNOVER: possession changed while the Core was still ")
						TEXT("travelling at %.0f uu/s and only %.1f deg off the surface - spec v13 §8 says ")
						TEXT("this must not happen. Landing rule = %s."),
						PendingTurnoverArrivalSpeed, ArrivalDegrees,
						(!TraceModeBLegacyLandingRule())
							? TEXT("v13 (an actual landing)") : TEXT("PRE-v13, DELIBERATELY ARMED"));
				}

				if (bWasRestProbe)
				{
					// Counted only on a turnover that actually fired, and only when the AT-REST PROBE is
					// what established it: this is the number that says the v7 probe caught a landing the
					// v6 contact test had missed, i.e. how often the reported bug would have happened.
					// Counting it where the probe runs instead would tick up every frame a Core sat
					// somewhere no turnover was due, and the number would mean nothing.
					++SurfaceStats.RestProbeRescues;
				}
				return;   // Possession changed (or the Core was reset). Touch nothing.
			}
		}
	}

	// --- 2c. SPEC v25 §2: the turnover window, the pull race and the lockout clock. ----------------
	//
	// AHEAD OF THE PICKUP POLL, and that ordering is the rule the same way §2b's is: a completed pull
	// and a first-contact pickup are two answers to "who gets it", and the pull is the one a player
	// spent 0.3 s earning. Below §2b, because a landing that fires this frame must open the window
	// before anybody is allowed to race for it.
	ServerTickTurnover(Step);

	if (PullWinner != nullptr)
	{
		return;   // A pull completed on this frame. The delivery owns the Core from the next one.
	}

	// --- 3. First contact takes it. ---------------------------------------------------------------
	//
	// NOT WHILE A LANDING IS PENDING. "A throw that comes down is a turnover, full stop" is the rule
	// §2b is built around, and it is enforced by running ahead of this poll; the v19 §1.5 settle would
	// have opened a window in which the throwing team could jog over and reclaim their own bad throw,
	// which is the v4 behaviour spec v6 §4.2 deleted. The settle moves WHEN possession changes, and it
	// must not change WHO it changes to.
	//
	// SPEC v25 §2 did not change this ordering; it changed WHO the poll accepts. ServerTryLoosePickup
	// now refuses the locked-out team for the length of the window, which is the second half of row 2
	// of the table ("only they may pick up") and, once the window closes, is row 3 ("either team, by
	// touch") with no further code.
	if (PendingTurnoverLandedServerTime < 0.f && ServerTryLoosePickup())
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
	// SPEC v25 §2 HOLDS THIS TIMER OFF FOR THE LENGTH OF THE WINDOW. A turned-over Core is SUPPOSED to
	// be lying there - that is the mechanic - and the reset timer counts from the THROW, so a long
	// throw plus a 5 s lockout could expire it out from under the pull it was waiting for. The rescue
	// this timer exists for (a Core nobody can reach) is unaffected: the window is 5 s and then the
	// timer resumes with the whole of the "either team may take it" period still to run.
	const float ResetAfter = TraceModeBTuning::LooseResetSeconds();
	if (ResetAfter > 0.f && !IsTurnoverActive() && (Now - LooseStartServerTime) >= ResetAfter)
	{
		ResetLooseCore(TEXT("untouched past the reset timer"));
	}
}

void ATraceCore::ServerApplyCatchZone(float DeltaSeconds)
{
	// SPEC v6 §4.1. See the tuning block at the top of this file for what the three knobs mean.
	if (!HasAuthority() || !bLoose || bLooseAtRest || bCoreStateLocked || DeltaSeconds <= 0.f)
	{
		return;
	}

	const float Radius = TraceModeBTuning::CatchRadius();
	const float Curve = TraceModeBTuning::CatchCurveStrength();
	if (Radius <= 0.f || Curve <= 0.f)
	{
		CatchZoneTarget = nullptr;
		bCatchZoneContested = false;
		return;   // Magnet switched off. The Core flies exactly as it did before v6.
	}

	const FVector Speed3D = LooseVelocity;
	const double Speed = Speed3D.Size();
	if (Speed < 1.0)
	{
		CatchZoneTarget = nullptr;
		bCatchZoneContested = false;
		return;   // Nothing to steer.
	}

	const float Now = GetServerTimeSeconds();
	const float ThrowerLockoutEnd = LooseStartServerTime + TraceModeBTuning::CatchThrowerLockout();
	const ATraceCharacter* Thrower = LooseThrower.Get();
	const FVector CoreLocation = LooseLocation;

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	// SPEC v13 §5. EVERY eligible player is collected, not just the running best, because the rule is
	// about the SET: "within the magnet zone of two or more players". A loop that only ever kept the
	// winner could not tell a contest from a walkover, could not apply hysteresis (which needs to know
	// whether the incumbent is still in the set at all) and could not report either.
	//
	// Two parallel arrays rather than one array of pairs: the contenders are a plain value type on
	// purpose so PickContestedCatcher can be driven by a test with no world in it, and the actors it
	// must not know about live alongside it.
	TArray<TraceModeBTuning::FCatchContender> Contenders;
	TArray<ATraceCharacter*> ContenderPawns;
	TArray<FVector> ContenderPoints;
	// SPEC v14 §6, MACE: "+30% magnet radius ... Derive it, do not hardcode." The radius is now
	// PER CANDIDATE, so it is carried alongside them — the steering below needs the winner's own
	// radius, not the global one, or a Core caught at 560 uu by Mace would be steered as though it
	// were already 110 uu outside the zone.
	TArray<float> ContenderRadii;
	Contenders.Reserve(Candidates.Num());
	ContenderPawns.Reserve(Candidates.Num());
	ContenderPoints.Reserve(Candidates.Num());
	ContenderRadii.Reserve(Candidates.Num());

	const ATraceCharacter* const Incumbent = CatchZoneTarget.Get();

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive())
		{
			continue;
		}

		// THE ONE EXCLUSION. Everybody else - teammate or enemy - is a catcher from frame one, which
		// is what makes interception a feature rather than an accident (spec v6 §4.1's [ASSUMPTION]).
		if (Candidate == Thrower && Now < ThrowerLockoutEnd)
		{
			continue;
		}

		// Aim at the capsule's CENTRE LINE at the Core's own height, not at the actor origin: a Core
		// arriving at head height should curve to the head, not dive at the navel and then climb.
		FVector CatchPoint = Candidate->GetActorLocation();
		double SurfaceDistance = FVector::Dist(CoreLocation, CatchPoint);

		if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
		{
			const FVector CapsuleCentre = Capsule->GetComponentLocation();
			const double HalfHeight = static_cast<double>(Capsule->GetScaledCapsuleHalfHeight());

			CatchPoint = FVector(CapsuleCentre.X, CapsuleCentre.Y,
				FMath::Clamp(CoreLocation.Z, CapsuleCentre.Z - HalfHeight, CapsuleCentre.Z + HalfHeight));

			SurfaceDistance = FMath::Max(0.0,
				FVector::Dist(CoreLocation, CatchPoint) - static_cast<double>(Capsule->GetScaledCapsuleRadius()));
		}

		// SPEC v14 §6 — MACE'S PASSIVE, AND THE ONLY PLACE IT EXISTS.
		//
		// "The base is now 450 uu ... so Mace's is 585 uu. Derive it, do not hardcode." 585 appears
		// nowhere: this is Radius (the live CoreCatchRadius knob) times a multiplier the character
		// derives from UTraceSettings::MaceMagnetRadiusBonus. Retuning the base radius moves Mace's
		// with it, which is what "derive" asks for.
		//
		// 1.0 for every other player and for every bot, so the loop is unchanged for them.
		//
		// IT IS PER CANDIDATE AND NOT A GLOBAL WIDENING. The magnet is a contest (spec v13 §5): a
		// single wider radius would have let Mace's bonus pull the Core toward HER OPPONENT too.
		float CandidateRadius = Radius;
		if (TraceAbilityIntegration::IsEnabled())
		{
			const float MagnetScale = UTraceAbilityComponent::GetMagnetRadiusMultiplierFor(Candidate);
			if (!FMath::IsNearlyEqual(MagnetScale, 1.f))
			{
				CandidateRadius = Radius * MagnetScale;
				++TraceAbilityIntegration::Counters().MagnetWidenedFrames;
			}
		}

		if (SurfaceDistance > static_cast<double>(CandidateRadius))
		{
			continue;
		}

		// ONLY WHAT THE CORE IS FLYING TOWARD. Without this the magnet would drag the Core backwards
		// into somebody it had already passed, which is not a catch - it is the Core changing its mind.
		// Half a hemisphere of tolerance, so a player slightly off to the side still catches.
		const FVector ToCatcher = CatchPoint - CoreLocation;
		if (!ToCatcher.IsNearlyZero()
			&& FVector::DotProduct(ToCatcher.GetSafeNormal(), Speed3D / Speed) < 0.0)
		{
			continue;
		}

		TraceModeBTuning::FCatchContender Contender;
		Contender.SurfaceDistance = SurfaceDistance;
		Contender.StableKey = Candidate->GetUniqueID();
		Contender.bIncumbent = (Candidate == Incumbent);

		Contenders.Add(Contender);
		ContenderPawns.Add(Candidate);
		ContenderPoints.Add(CatchPoint);
		ContenderRadii.Add(CandidateRadius);
	}

	if (Contenders.Num() == 0)
	{
		CatchZoneTarget = nullptr;
		bCatchZoneContested = false;
		return;
	}

	// --- SPEC v13 §5. THE CONTEST. ----------------------------------------------------------------
	//
	// Counted before it is resolved, so the tally records what the situation WAS rather than what the
	// rule made of it. "Cross-team" is tracked separately because that is the case the note names, but
	// the RULE below does not branch on it: nearest wins whoever the contenders are.
	const bool bContested = (Contenders.Num() >= 2);
	bool bCrossTeam = false;
	if (bContested)
	{
		const ETraceTeam FirstTeam = ContenderPawns[0]->GetTeam();
		for (int32 Index = 1; Index < ContenderPawns.Num() && !bCrossTeam; ++Index)
		{
			bCrossTeam = (ContenderPawns[Index]->GetTeam() != FirstTeam);
		}

		++CatchStats.ContestedFrames;
		CatchStats.CrossTeamContestedFrames += bCrossTeam ? 1 : 0;
		CatchStats.MaxContenders = FMath::Max(CatchStats.MaxContenders, Contenders.Num());
		CatchStats.Contests += bCatchZoneContested ? 0 : 1;
	}
	else
	{
		++CatchStats.UncontestedFrames;
	}
	bCatchZoneContested = bContested;

	bool bHysteresisHeld = false;
	const int32 Winner = TraceModeBTuning::PickContestedCatcher(
		Contenders, TraceModeBTuning::CatchContestHysteresis(), &bHysteresisHeld);

	if (Winner == INDEX_NONE)
	{
		CatchZoneTarget = nullptr;
		return;
	}

	CatchStats.HysteresisHolds += bHysteresisHeld ? 1 : 0;

	ATraceCharacter* const Best = ContenderPawns[Winner];
	const FVector BestPoint = ContenderPoints[Winner];
	const double BestSurfaceDistance = Contenders[Winner].SurfaceDistance;

	const FVector ToTarget = BestPoint - CoreLocation;
	if (ToTarget.IsNearlyZero())
	{
		return;   // Already there; the pickup poll has it this frame.
	}

	// ONE TARGET, and only the winner's. This was already true before v13 - there has only ever been
	// one call to the steering - and it stays true: the change is that the single target is now DECIDED
	// by distance and held steady, rather than being whichever eligible player the roster reached last
	// with the smallest number.
	// THE WINNER'S OWN RADIUS, not the global knob — see the per-candidate note in the loop above.
	// SteerTowardCatchPoint normalises the distance by this value, so passing the base radius while
	// Mace caught at 560 uu would hand it a ratio above 1 and steer as if the Core were escaping.
	LooseVelocity = TraceModeBTuning::SteerTowardCatchPoint(
		CoreLocation, LooseVelocity, BestPoint, BestSurfaceDistance, ContenderRadii[Winner], Curve, DeltaSeconds);

	// Announced ONCE per catch, at Display, and only when the target changes. The catch zone is
	// invisible by design, and an invisible mechanic with no log line is one nobody can tell is
	// working - which is precisely how two working mechanics have been declared dead on this project.
	if (CatchZoneTarget.Get() != Best)
	{
		const bool bSwitched = (Incumbent != nullptr);
		CatchStats.TargetSwitches += bSwitched ? 1 : 0;
		CatchZoneTarget = Best;

		// The runner-up is named on a contested frame. "The Core curved to X" is unfalsifiable on its
		// own; "the Core curved to X at 210 uu with Y at 340 uu also in range" is the rule stating its
		// own working, and it is what a reader checks when somebody reports the Core going to the
		// wrong player.
		FString ContestDetail;
		if (bContested)
		{
			int32 RunnerUp = INDEX_NONE;
			for (int32 Index = 0; Index < Contenders.Num(); ++Index)
			{
				if (Index == Winner)
				{
					continue;
				}
				if (RunnerUp == INDEX_NONE || Contenders[Index].SurfaceDistance < Contenders[RunnerUp].SurfaceDistance)
				{
					RunnerUp = Index;
				}
			}

			ContestDetail = FString::Printf(
				TEXT(" | CONTESTED by %d (%s): nearest wins over %s (%s) at %.0f uu"),
				Contenders.Num(), bCrossTeam ? TEXT("opposite teams") : TEXT("same team"),
				*GetNameSafe(ContenderPawns[RunnerUp]),
				*TraceTeamName(ContenderPawns[RunnerUp]->GetTeam()).ToString(),
				Contenders[RunnerUp].SurfaceDistance);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] CATCH ZONE: the Core is curving toward %s (%s) - %.0f uu from their capsule, ")
			TEXT("radius %.0f, curve %.1f%s%s"),
			*GetNameSafe(Best), *TraceTeamName(Best->GetTeam()).ToString(),
			BestSurfaceDistance, Radius, Curve,
			(Best == Thrower) ? TEXT(" (the thrower, past their lockout)") : TEXT(""),
			*ContestDetail);
	}
}

bool ATraceCore::ServerProbeRestingSurface(FVector& OutPoint, FVector& OutNormal) const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	const FVector From = LooseLocation;
	const FVector To = From - FVector(0.0, 0.0, static_cast<double>(TraceModeBRestProbeDepth));

	// The SAME sphere and the SAME channel the flight sweeps use. A probe with a different shape
	// would be a second opinion about what the Core is touching, and the two would disagree exactly
	// where it matters - on the lip of a block, which is the geometry the user's report is about.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreRestProbe), /*bTraceComplex=*/false, this);
	Params.AddIgnoredActor(this);

	FHitResult Hit;
	const bool bHit = World->SweepSingleByChannel(
		Hit, From, To, FQuat::Identity, ECC_WorldStatic,
		FCollisionShape::MakeSphere(TraceModeBTuning::CollisionRadius), Params);

	if (!bHit)
	{
		return false;
	}

	// A start-penetrating probe reports no ImpactNormal worth having; Normal is the depenetration
	// direction, which for a Core settled into a surface points back out of it and is the honest
	// answer to "what is holding this up".
	OutNormal = Hit.bStartPenetrating ? Hit.Normal : Hit.ImpactNormal;
	OutPoint = Hit.bStartPenetrating ? Hit.Location : Hit.ImpactPoint;
	return true;
}

double ATraceCore::MeasureVisibleSupportGap(FVector& OutSupportPoint) const
{
	const UWorld* World = GetWorld();
	const double ProbeDepth =
		FMath::Max(1.0, static_cast<double>(CVarModeBTurnoverContactProbe.GetValueOnAnyThread()));

	if (World == nullptr)
	{
		return ProbeDepth;
	}

	// THE DRAWN RADIUS, NOT THE SWEPT ONE. See TraceModeBVisibleOrbRadius: the collision sphere is
	// larger than the ball, so measuring with it would report "touching" 2 uu before a player agrees,
	// and this whole rule exists because the user is reporting what they can SEE.
	const FVector From = LooseLocation;
	const FVector To = From - FVector(0.0, 0.0, ProbeDepth);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreSupportGap), /*bTraceComplex=*/false, this);
	Params.AddIgnoredActor(this);

	FHitResult Hit;
	const bool bHit = World->SweepSingleByChannel(
		Hit, From, To, FQuat::Identity, ECC_WorldStatic,
		FCollisionShape::MakeSphere(static_cast<float>(TraceModeBVisibleOrbRadius)), Params);

	if (!bHit)
	{
		// Nothing at all underneath within the probe. The strongest possible "this is mid-air", and
		// returning the full depth rather than a sentinel keeps the tallies' Max() honest.
		return ProbeDepth;
	}

	OutSupportPoint = Hit.bStartPenetrating ? Hit.Location : Hit.ImpactPoint;

	// A probe that starts already overlapping is a ball buried in the surface. That is touching by
	// any reading, and Hit.Distance is 0 for it anyway; stated rather than relied on.
	if (Hit.bStartPenetrating)
	{
		return 0.0;
	}

	return FMath::Max(0.0, static_cast<double>(Hit.Distance));
}

bool ATraceCore::ServerSurfaceTurnover(const FVector& SurfacePoint, const FVector& SurfaceNormal)
{
	// SPEC v6 §4.2, GENERALISED BY v7 §4. "When a team has possession of the core, throws it, and it
	// hits the ground, it should automatically turnover to the closest player on the enemy team" —
	// where "the ground" is now any upward-facing surface, the top of a structure included.
	if (!HasAuthority() || !bLoose || bCoreStateLocked)
	{
		return false;
	}

	// FLOOR OR TOP, for the log line and the tallies only — the RULE does not distinguish them, and
	// must not: the spec's whole point is that landing on a crate is the same event as landing on the
	// floor. Measured against the arena's own floor plane rather than against an actor type, for the
	// same reason the rule itself is: the arena is generic static meshes.
	double FloorZ = 0.0;
	bool bHaveFloor = false;
	if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(GetWorld()))
	{
		const FBox FieldBox = Arena->GetFieldBounds();
		if (FieldBox.IsValid != 0)
		{
			FloorZ = FieldBox.Min.Z;
			bHaveFloor = true;
		}
	}

	const bool bElevated = bHaveFloor && (SurfacePoint.Z > FloorZ + TraceModeBElevatedSurfaceZ);
	const TCHAR* const SurfaceKind = bElevated ? TEXT("the TOP OF AN OBJECT") : TEXT("the ground");

	const ETraceTeam ThrowingTeam = LooseFromTeam;
	const ETraceTeam ReceivingTeam = (ThrowingTeam != ETraceTeam::None)
		? TraceOpposingTeam(ThrowingTeam) : ETraceTeam::None;

	if (ReceivingTeam == ETraceTeam::None)
	{
		// Nobody threw it - a debug launch, or a Core loose from before a mode switch. The rule has no
		// opinion, so leave it live and let the pickup poll and the reset timer do their jobs.
		return false;
	}

	// =============================================================================================
	// *** SPEC v25 §2. THE TURNOVER IS REGISTERED; THE CORE STAYS ON THE GROUND WHERE IT LANDED. ***
	//
	// Verbatim: "Instead of automatically going to the other team, a turnover is registered and the
	// core stays on the ground where it landed."
	//
	// EVERYTHING ABOVE THIS POINT IS UNTOUCHED, and that is the spec's own instruction: "Turnover
	// criteria remain the same". The surface test, the arrival angle, the visible-support gap and the
	// settle all still decide WHETHER this is a turnover and WHEN it fires; the only thing that has
	// changed is what the answer does. So this branch sits at the very end of the criteria and
	// replaces the transfer, rather than being a second rule bolted on beside it.
	//
	// Trace.ModeB.TurnoverPull 0 falls through to the pre-v25 grant below, unchanged, which is the A/B
	// arm every red arm in this section is measured against.
	// =============================================================================================
	const FVector LandedAt = LooseLocation;

	if (TraceModeBTuning::TurnoverPullEnabled())
	{
		(bElevated ? SurfaceStats.TopTurnovers : SurfaceStats.GroundTurnovers)++;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] TURNOVER REGISTERED (spec v25 §2): the %s throw settled on %s at %s (normal %s, ")
			TEXT("%.0f deg from up) - the Core STAYS THERE. %s is locked out for %.1fs; %s may pull it ")
			TEXT("(right mouse + hover + line of sight for %.2fs) or run over it."),
			*TraceTeamName(ThrowingTeam).ToString(), SurfaceKind, *LandedAt.ToCompactString(),
			*SurfaceNormal.ToCompactString(),
			FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(SurfaceNormal.Z, -1.0, 1.0))),
			*TraceTeamName(ThrowingTeam).ToString(), GetTurnoverLockoutSeconds(),
			*TraceTeamName(ReceivingTeam).ToString(), GetPullHoldSeconds());

		RegisterTurnover(ThrowingTeam, LandedAt, SurfaceKind);

		// TRUE, and the contract behind it has not changed: "the caller must touch no member state
		// afterwards". The Core is still loose, but this frame's landing latch has been consumed and
		// the turnover window now owns what happens next, so the caller returning here is what stops
		// the same frame also running the pickup poll against the team that just lost it.
		return true;
	}

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	ATraceCharacter* Nearest = nullptr;
	double NearestDistSq = TNumericLimits<double>::Max();
	const FVector Landed = LooseLocation;

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive() || Candidate->GetTeam() != ReceivingTeam)
		{
			continue;
		}

		const double DistSq = FVector::DistSquared(Landed, Candidate->GetActorLocation());
		if (DistSq < NearestDistSq)
		{
			NearestDistSq = DistSq;
			Nearest = Candidate;
		}
	}

	if (Nearest == nullptr)
	{
		// The whole receiving team is dead. The Core may never be lost, and handing it back to the
		// side that threw it away would make a wild throw free - so it becomes a kickoff for the side
		// that is owed it, which is what ResetLooseCore already computes from LooseFromTeam.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] SURFACE TURNOVER: the throw by %s came to rest on %s at %s but %s has nobody ")
			TEXT("alive - kicking off instead."),
			*TraceTeamName(ThrowingTeam).ToString(), SurfaceKind, *Landed.ToCompactString(),
			*TraceTeamName(ReceivingTeam).ToString());

		(bElevated ? SurfaceStats.TopTurnovers : SurfaceStats.GroundTurnovers)++;
		ResetLooseCore(TEXT("thrown onto a surface with no living enemy to award it to"));
		return true;
	}

	// ONE LINE FOR BOTH SURFACES, naming which one it was. A turnover off the top of a block and a
	// turnover off the floor are the same rule firing, and a log that called them different things
	// would invite the next reader to go looking for two rules.
	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] SURFACE TURNOVER (spec v7 §4): the %s throw settled on %s at %s (normal %s, %.0f deg ")
		TEXT("from up, threshold %.0f) - to %s (%s), the nearest enemy, %.0f uu away."),
		*TraceTeamName(ThrowingTeam).ToString(), SurfaceKind, *Landed.ToCompactString(),
		*SurfaceNormal.ToCompactString(),
		FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(SurfaceNormal.Z, -1.0, 1.0))),
		TraceModeBTuning::SurfaceMaxSlopeDegrees(),
		*GetNameSafe(Nearest), *TraceTeamName(ReceivingTeam).ToString(), FMath::Sqrt(NearestDistSq));

	(bElevated ? SurfaceStats.TopTurnovers : SurfaceStats.GroundTurnovers)++;

	// THROUGH TakeLooseCore, not through a private grant. It is the one guarded loose -> held
	// transition, and it is what sets GraceOverrideTeam so that GrantTo's own AreAllies() line decides
	// the 0.5 s trace grace. This is a cross-team change of possession, so that line will grant it -
	// spec v5 §0's rule, applied rather than re-implemented. It also feeds Trace.ModeB.Verify.
	TakeLooseCore(Nearest);
	return true;
}

// =================================================================================================
// SPEC v25 §2 — THE TURNOVER STATE MACHINE
//
// Read the table in TraceCore.h before changing anything here. Three rows, and the third is the
// first: once the lockout expires the turnover is CLEARED, because "nobody pulls, either team picks
// up, normal beam" is exactly what a Core that was never turned over already does.
//
// EVERY DECISION IS THE SERVER'S. The four replicated facts (TurnoverLockoutTeam,
// TurnoverStartServerTime, PullHolds, PullWinner) are written here and nowhere else, and no client
// runs a clock of its own — spec v25: "Do not let a client decide it won a race."
// =================================================================================================

float ATraceCore::GetPullHoldSeconds()        { return TraceModeBTuning::PullHoldSeconds(); }
float ATraceCore::GetTurnoverLockoutSeconds() { return TraceModeBTuning::TurnoverLockoutSeconds(); }
float ATraceCore::GetTurnoverBeamScale()      { return TraceModeBTuning::TurnoverBeamScale(); }

bool ATraceCore::IsTurnoverActive() const
{
	if (TurnoverLockoutTeam == ETraceTeam::None)
	{
		return false;
	}

	// The clock is asked here rather than trusted to have been cleared, so a client whose
	// TurnoverLockoutTeam = None has not yet arrived still stops showing the window on time. The
	// server clears it in ServerTickTurnover; this is what makes the two agree in between.
	return GetTurnoverSecondsRemaining() > 0.f;
}

float ATraceCore::GetTurnoverSecondsRemaining() const
{
	if (TurnoverLockoutTeam == ETraceTeam::None)
	{
		return 0.f;
	}

	const float Lockout = GetTurnoverLockoutSeconds();
	return FMath::Max(0.f, (TurnoverStartServerTime + Lockout) - GetServerTimeSeconds());
}

float ATraceCore::GetTurnoverAlpha() const
{
	if (TurnoverLockoutTeam == ETraceTeam::None)
	{
		return -1.f;
	}

	const float Lockout = GetTurnoverLockoutSeconds();
	if (Lockout <= 0.f)
	{
		return 1.f;
	}

	return FMath::Clamp((GetServerTimeSeconds() - TurnoverStartServerTime) / Lockout, 0.f, 1.f);
}

float ATraceCore::GetPullProgressFor(const AActor* Player) const
{
	if (Player == nullptr)
	{
		return -1.f;
	}

	const float Hold = GetPullHoldSeconds();
	const float Now = GetServerTimeSeconds();

	for (const FTraceCorePullHold& Entry : PullHolds)
	{
		if (Entry.Puller != Player)
		{
			continue;
		}

		// A zero hold time is "instant", not "divide by zero". It is reachable from the console and
		// from a misconfigured ini, and a NaN ring would be the loudest possible way to find out.
		if (Hold <= 0.f)
		{
			return 1.f;
		}

		return FMath::Clamp((Now - Entry.StartServerTime) / Hold, 0.f, 1.f);
	}

	return -1.f;
}

float ATraceCore::GetLocalPullProgress() const
{
	const UWorld* World = GetWorld();
	if (World == nullptr || PullHolds.Num() == 0)
	{
		return -1.f;   // The common case, and it costs nothing.
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* Controller = It->Get();
		if (Controller == nullptr || !Controller->IsLocalController())
		{
			continue;
		}

		const float Progress = GetPullProgressFor(Controller->GetPawn());
		if (Progress >= 0.f)
		{
			return Progress;
		}
	}

	return -1.f;
}

bool ATraceCore::CanPullNow(const ATraceCharacter* Puller, const TCHAR** OutReason) const
{
	// Every refusal names itself, for the same reason IsLegalPassTarget's do: "the ring never
	// appeared" is what a player experiences, and it has eight possible causes.
	const auto Refuse = [OutReason](const TCHAR* Why) -> bool
	{
		if (OutReason != nullptr)
		{
			*OutReason = Why;
		}
		return false;
	};

	if (OutReason != nullptr)
	{
		*OutReason = TEXT("legal");
	}

	if (!IsModeB())
	{
		return Refuse(TEXT("not goals mode"));                     // Spec v25: goals mode only.
	}
	if (!IsValid(Puller) || !Puller->IsAlive())
	{
		return Refuse(TEXT("no living puller"));
	}
	if (!bLoose)
	{
		return Refuse(TEXT("the Core is not loose"));
	}
	if (Puller == Carrier)
	{
		return Refuse(TEXT("the puller is holding the Core"));     // §7's precedence: carriers parry.
	}
	if (!IsTurnoverActive())
	{
		return Refuse(TEXT("no turnover window is open"));         // Row 1 and row 3: nobody pulls.
	}
	if (PullWinner != nullptr)
	{
		return Refuse(TEXT("a pull has already completed"));
	}

	// *** THE LOCKOUT IS ON THE TEAM THAT DROPPED IT, NOT ON THE INDIVIDUAL. *** Spec v25 is explicit,
	// and it is the whole difference between "the player who threw it away" and "their side": a
	// teammate of the thrower is refused here exactly as the thrower is.
	const ETraceTeam PullingTeam = GetTurnoverPullingTeam();
	if (PullingTeam == ETraceTeam::None)
	{
		return Refuse(TEXT("the turnover has no opposing team"));
	}
	if (Puller->GetTeam() != PullingTeam)
	{
		return Refuse(TEXT("on the team that dropped it - locked out"));
	}

	const FVector ViewLocation = Puller->GetPawnViewLocation();
	const FVector CoreCentre = LooseLocation;

	FVector ToCore = CoreCentre - ViewLocation;
	const double Distance = ToCore.Size();

	const float MaxRange = TraceModeBTuning::PullMaxRangeUU();
	if (MaxRange > 0.f && Distance > static_cast<double>(MaxRange))
	{
		return Refuse(TEXT("out of pull range"));
	}

	// LINE OF SIGHT, and deliberately the SAME channel and the same argument as the pass's
	// (IsLegalPassTarget): ECC_Visibility, because an object-type query matches the endzone trigger -
	// a QueryOnly box that responds to nothing but ECC_Pawn - and would make a Core lying inside a
	// zone unpullable for a reason nobody could see. Single ray, unlike the pass's three: the pass
	// probes a 176 uu pawn that ducks behind cover, this probes a 20 uu ball on the floor, and there
	// is no second point on it worth asking about.
	const UWorld* World = GetWorld();
	if (World != nullptr)
	{
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TraceCorePullLos), /*bTraceComplex=*/false);
		QueryParams.AddIgnoredActor(this);
		QueryParams.AddIgnoredActor(Puller);

		if (World->LineTraceTestByChannel(ViewLocation, CoreCentre, ECC_Visibility, QueryParams))
		{
			return Refuse(TEXT("no line of sight"));
		}
	}

	if (Distance <= CoreGeometryEpsilon)
	{
		return true;   // Standing on top of it. Nothing sensible to measure; accept.
	}

	ToCore /= Distance;

	const FVector AimDirection = Puller->GetAimDirection();
	const double Cosine = FVector::DotProduct(ToCore, AimDirection);
	if (Cosine <= 0.0)
	{
		return Refuse(TEXT("the Core is behind them"));
	}

	// (a) THE CONE - what makes a Core acquirable at all at range. A 20 uu orb subtends 0.14 deg at
	//     8000 uu, so a pure ray-through-the-ball test is unusable across this pitch.
	const double ConeDegrees = static_cast<double>(TraceModeBTuning::PullAimConeDegrees());
	if (Cosine >= FMath::Cos(FMath::DegreesToRadians(ConeDegrees)))
	{
		return true;
	}

	// (b) THE RAY THROUGH THE ORB - what makes it acquirable point-blank, where the cone has
	//     collapsed to a few uu. Perpendicular distance from the Core to the aim ray, against the
	//     DRAWN orb radius plus the slack, so the forgiveness is measured off the ball a player can
	//     actually see rather than off the larger sphere the flight sweeps with.
	const double PerpendicularDistance = Distance * FMath::Sqrt(FMath::Max(0.0, 1.0 - Cosine * Cosine));
	const double Threshold = TraceModeBVisibleOrbRadius + static_cast<double>(TraceModeBTuning::PullAimSlackUU());
	if (PerpendicularDistance <= Threshold)
	{
		return true;
	}

	return Refuse(TEXT("not hovering the Core"));
}

void ATraceCore::RegisterTurnover(ETraceTeam DroppingTeam, const FVector& Where, const TCHAR* Why)
{
	if (!HasAuthority() || DroppingTeam == ETraceTeam::None)
	{
		return;
	}

	TurnoverLockoutTeam = DroppingTeam;
	TurnoverStartServerTime = GetServerTimeSeconds();
	bTurnoverRegisteredThisFlight = true;

	// SPEC v26 §9 — "Core Turnover ... should be game-side", i.e. everyone nearby hears it. One line,
	// on the authority, at the point the rule fires; TraceAudio::Play reads the event's declared side
	// (Audio/TraceSoundEvents.h) and multicasts it. Note `Where`, not GetActorLocation(): the actor is
	// moved to `Where` a few lines below, so taking the sound's position from the rule's own argument
	// keeps the two in step if that ordering ever changes.
	TraceAudio::PlayAt(this, TraceSoundEvents::CoreTurnover, Where);

	// Captured HERE, by the rule itself, for the same reason TakeLooseCore captures its take: by the
	// time Trace.ModeB.Verify's step 6/7/8 gets to judge, an enemy may already have pulled the Core
	// and cleared the window, and a scenario that polled for it would report a rule that had fired
	// as one that had not.
	if (bVerifyAwaitingTurnover)
	{
		bVerifyAwaitingTurnover = false;
		bVerifyTurnoverSeen = true;
		VerifyTurnoverLockedTeam = DroppingTeam;
	}

	// The Core is at rest on the surface it landed on and STAYS there. Zeroing the velocity is what
	// makes that true on the clients too: their dead reckoning is gated on a non-zero velocity, so a
	// Core left with the last frame's residual would keep drifting on every machine but this one.
	LooseVelocity = FVector::ZeroVector;
	LooseLocation = Where;
	bLooseAtRest = true;
	SetActorLocation(LooseLocation, false, nullptr, ETeleportType::TeleportPhysics);

	// THE LANDING LATCH IS CONSUMED HERE. It has done its job (spec v19 §1.5's settle, which is what
	// held possession until the ball had actually been drawn touching something) and it also gates the
	// pickup poll - leaving it set would make the Core unpickupable by ANYBODY for the rest of the
	// window, which is the opposite of what the turnover is for.
	ClearPendingTurnover();

	// A fresh window starts with a clean race: no holds carried over from before the landing, and no
	// stale winner. The latch (PullInputHeld) is deliberately NOT cleared - a player who was already
	// holding right mouse when it landed has their finger down, and the next tick will start their
	// 0.3 s honestly rather than demanding they let go and press again.
	PullHolds.Reset();
	PullWinner = nullptr;

	UE_LOG(LogTraceGame, Verbose,
		TEXT("[ModeB] spec v25 §2: turnover window OPEN at %s (%s). Locked out: %s. Pull: %s, %.2fs hold."),
		*Where.ToCompactString(), Why, *TraceTeamName(DroppingTeam).ToString(),
		*TraceTeamName(TraceOpposingTeam(DroppingTeam)).ToString(), GetPullHoldSeconds());

	// The beam is the field-wide read of this whole mechanic, so it changes on the same frame the
	// window opens rather than on the next reconciliation tick.
	ApplyAttachment();
	UpdateVisuals();
}

void ATraceCore::ClearTurnover(const TCHAR* Why)
{
	if (TurnoverLockoutTeam == ETraceTeam::None && PullHolds.Num() == 0 && PullWinner == nullptr)
	{
		return;
	}

	if (TurnoverLockoutTeam != ETraceTeam::None)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] spec v25 §2: turnover window CLOSED (%s) after %.2fs - %s is no longer locked ")
			TEXT("out and nobody may pull."),
			Why, GetServerTimeSeconds() - TurnoverStartServerTime,
			*TraceTeamName(TurnoverLockoutTeam).ToString());
	}

	TurnoverLockoutTeam = ETraceTeam::None;
	TurnoverStartServerTime = 0.f;
	PullHolds.Reset();
	PullWinner = nullptr;

	ApplyAttachment();
	UpdateVisuals();
}

void ATraceCore::RequestPullInput(bool bPressed, ATraceCharacter* Requester)
{
	if (!IsValid(Requester) || !IsModeB())
	{
		return;   // Goals mode only. Mode A never sees this function do anything.
	}

	if (Requester->HasAuthority())
	{
		// The listen host's own player, and every bot. No round trip, and no prediction either: the
		// hold this starts is the server's, which is the only one there is.
		ServerApplyPullInput(Requester, bPressed);
		return;
	}

	// A CLIENT. It sends the BUTTON and nothing else - not a hold length, not a completion, not a
	// winner. See ATraceCorePullRelay for why the message cannot go on this actor.
	if (!Requester->IsLocallyControlled())
	{
		return;   // One machine speaks for one pawn.
	}

	if (ATraceCorePullRelay* Relay = ATraceCorePullRelay::Find(Requester->GetController()))
	{
		Relay->ServerSetPullInput(bPressed);
	}
	else
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeB] spec v25 §2: %s pressed pull but has no ATraceCorePullRelay yet (it replicates ")
			TEXT("shortly after PostLogin); the press is dropped rather than predicted."),
			*GetNameSafe(Requester));
	}
}

void ATraceCore::ServerApplyPullInput(ATraceCharacter* Requester, bool bPressed)
{
	if (!HasAuthority() || !IsValid(Requester))
	{
		return;
	}

	PullInputHeld.RemoveAll([](const TWeakObjectPtr<ATraceCharacter>& Entry)
	{
		return !Entry.IsValid();
	});

	const int32 Existing = PullInputHeld.IndexOfByPredicate([Requester](const TWeakObjectPtr<ATraceCharacter>& Entry)
	{
		return Entry.Get() == Requester;
	});

	if (bPressed)
	{
		if (Existing == INDEX_NONE)
		{
			PullInputHeld.Add(Requester);
		}
		return;
	}

	if (Existing != INDEX_NONE)
	{
		PullInputHeld.RemoveAt(Existing);
	}

	// *** RELEASING CANCELS. IT DOES NOT PAUSE. *** Spec v25 states it outright, and it is the rule a
	// 0.29 s release has to fail on: the entry is removed, so the next press starts a new hold at zero
	// rather than resuming 0.29 s of credit.
	CancelPullFor(Requester, TEXT("the button was released"), /*bAlsoClearLatch=*/false);
}

void ATraceCore::CancelPullFor(const ATraceCharacter* Puller, const TCHAR* Why, bool bAlsoClearLatch)
{
	if (!HasAuthority())
	{
		return;
	}

	const int32 Index = PullHolds.IndexOfByPredicate([Puller](const FTraceCorePullHold& Entry)
	{
		return Entry.Puller == Puller;
	});

	if (Index != INDEX_NONE)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeB] spec v25 §2: %s's pull CANCELLED at %.0f%% - %s."),
			*GetNameSafe(Puller),
			100.f * FMath::Clamp((GetServerTimeSeconds() - PullHolds[Index].StartServerTime)
				/ FMath::Max(KINDA_SMALL_NUMBER, GetPullHoldSeconds()), 0.f, 1.f),
			Why);

		PullHolds.RemoveAt(Index);
	}

	if (bAlsoClearLatch)
	{
		PullInputHeld.RemoveAll([Puller](const TWeakObjectPtr<ATraceCharacter>& Entry)
		{
			return !Entry.IsValid() || Entry.Get() == Puller;
		});
	}
}

void ATraceCore::ServerTickTurnover(float /*DeltaSeconds*/)
{
	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	// --- 1. THE WINDOW EXPIRES ON THE TEAM'S CLOCK, NOT ON ANY PLAYER'S. -------------------------
	//
	// Row 3 of the table. Clearing it is the whole of "after the 5 seconds are up, the opposite team
	// loses the pull ability and either team can pick up the core by running over it": no pull,
	// because CanPullNow refuses without a window; either team, because ServerTryLoosePickup's
	// lockout test is written against the same window; normal beam, because UpdateVisuals is.
	if (TurnoverLockoutTeam != ETraceTeam::None && GetTurnoverSecondsRemaining() <= 0.f)
	{
		ClearTurnover(TEXT("the lockout expired"));
	}

	// --- 2. Forget anybody who has left the match, so a dead pawn cannot hold a fill open. -------
	PullInputHeld.RemoveAll([](const TWeakObjectPtr<ATraceCharacter>& Entry)
	{
		const ATraceCharacter* Character = Entry.Get();
		return Character == nullptr || !IsValid(Character) || !Character->IsAlive();
	});

	// --- 3. Validate every live hold. CANCELS, never pauses. ------------------------------------
	for (int32 Index = PullHolds.Num() - 1; Index >= 0; --Index)
	{
		ATraceCharacter* Puller = PullHolds[Index].Puller;

		const bool bStillHolding = PullInputHeld.ContainsByPredicate(
			[Puller](const TWeakObjectPtr<ATraceCharacter>& Entry) { return Entry.Get() == Puller; });

		const TCHAR* Reason = TEXT("unknown");
		if (!bStillHolding)
		{
			CancelPullFor(Puller, TEXT("the button is no longer held"), /*bAlsoClearLatch=*/false);
		}
		else if (!CanPullNow(Puller, &Reason))
		{
			// Losing hover and losing line of sight arrive here identically, which is what the spec
			// asks for: "Losing either cancels the fill; it does not pause it."
			CancelPullFor(Puller, Reason, /*bAlsoClearLatch=*/false);
		}
	}

	// --- 4. Start a hold for anybody who is eligible and has not got one. ------------------------
	for (const TWeakObjectPtr<ATraceCharacter>& Entry : PullInputHeld)
	{
		ATraceCharacter* Puller = Entry.Get();
		if (Puller == nullptr)
		{
			continue;
		}

		const bool bAlreadyPulling = PullHolds.ContainsByPredicate(
			[Puller](const FTraceCorePullHold& Hold) { return Hold.Puller == Puller; });

		if (bAlreadyPulling || !CanPullNow(Puller))
		{
			continue;
		}

		FTraceCorePullHold& Added = PullHolds.AddDefaulted_GetRef();
		Added.Puller = Puller;
		Added.StartServerTime = Now;

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeB] spec v25 §2: %s (%s) started a pull on the turned-over Core."),
			*GetNameSafe(Puller), *TraceTeamName(Puller->GetTeam()).ToString());
	}

	// --- 5. FIRST TO COMPLETE WINS. -------------------------------------------------------------
	//
	// "Two opponents pulling at once: the one who finishes first gets it, and the other's fill is
	// cancelled." Every hold has the same length, so the first to finish is the one that STARTED
	// first, and the winner is picked by earliest StartServerTime rather than by array order. An
	// exact tie - two presses inside one server tick - breaks on the engine's unique object id, which
	// is stable for the lifetime of the pawn, for the same reason the catch contest's tie-break is:
	// resolving it by roster order would make the answer depend on the order actors happened to be
	// gathered in, which is not a fact about the game.
	const float Hold = GetPullHoldSeconds();

	ATraceCharacter* Winner = nullptr;
	float WinnerStart = 0.f;
	uint32 WinnerKey = 0;

	for (const FTraceCorePullHold& Entry : PullHolds)
	{
		if (!IsValid(Entry.Puller) || (Now - Entry.StartServerTime) < Hold)
		{
			continue;
		}

		const uint32 Key = Entry.Puller->GetUniqueID();
		if (Winner == nullptr || Entry.StartServerTime < WinnerStart
			|| (Entry.StartServerTime == WinnerStart && Key < WinnerKey))
		{
			Winner = Entry.Puller;
			WinnerStart = Entry.StartServerTime;
			WinnerKey = Key;
		}
	}

	if (Winner != nullptr)
	{
		ServerCompletePull(Winner);
	}
}

void ATraceCore::ServerCompletePull(ATraceCharacter* Winner)
{
	if (!HasAuthority() || !IsValid(Winner) || !bLoose)
	{
		return;
	}

	const int32 Losers = FMath::Max(0, PullHolds.Num() - 1);

	// THE LOSER'S FILL CANCELS. Clearing the array is that sentence; their LATCH is left alone,
	// because their finger really is still on the button and if this delivery is voided (the winner
	// dies mid-flight) the next tick should let them start a fresh 0.3 s rather than making them
	// re-press a button they never let go of.
	PullHolds.Reset();

	PullWinner = Winner;
	bLooseAtRest = false;

	// *** THE FULL CORE-THROWN VELOCITY, ASKED OF THE THROW ITSELF. ***
	//
	// Spec v25: "it travels towards the player who completed the pull first at full core thrown
	// velocity ... the same speed constant a thrown Core uses, not a new number." GetThrowSpeed() is
	// that constant AFTER the weight model (base / sqrt(mass)), which is the speed a thrown Core
	// actually leaves at, so retuning CoreThrowSpeed or CoreMassScale moves the pull with it. This is
	// the standing rule from Demo 21 applied to a speed instead of an ability: derived, not duplicated.
	const float Speed = GetThrowSpeed();
	const FVector ToWinner = (Winner->GetActorLocation() - FVector(LooseLocation)).GetSafeNormal();
	LooseVelocity = ToWinner * Speed;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] spec v25 §2: PULL COMPLETE - %s (%s) held for %.2fs and the Core is travelling to ")
		TEXT("them at %.0f uu/s (the full thrown speed). %d other fill(s) cancelled."),
		*GetNameSafe(Winner), *TraceTeamName(Winner->GetTeam()).ToString(), GetPullHoldSeconds(),
		Speed, Losers);
}

bool ATraceCore::ServerTickPullTravel(float DeltaSeconds)
{
	if (!HasAuthority() || PullWinner == nullptr)
	{
		return false;
	}

	ATraceCharacter* Winner = PullWinner;

	if (!IsValid(Winner) || !Winner->IsAlive() || !bLoose)
	{
		// The delivery has nobody to deliver to. The Core stops where it is and the window - if any of
		// it is left - carries on, so a team-mate can still earn it. It is deliberately NOT handed to
		// them by default: a pull is something a player completes, not something a death awards.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] spec v25 §2: the pull's winner is gone; the Core stops at %s and the window ")
			TEXT("(%.1fs left) continues."),
			*FVector(LooseLocation).ToCompactString(), GetTurnoverSecondsRemaining());

		PullWinner = nullptr;
		LooseVelocity = FVector::ZeroVector;
		bLooseAtRest = true;
		return false;
	}

	// The SAME speed constant the completion stamped, asked again rather than stored, so a live
	// retune reaches a delivery already in the air.
	const double Speed = static_cast<double>(GetThrowSpeed());
	const double Step = static_cast<double>(FMath::Clamp(DeltaSeconds, 0.f, 0.1f));

	// Aimed at the capsule CENTRE, which is the same point the pickup poll measures its radius from,
	// so the flight ends exactly where the take happens instead of a capsule-height away from it.
	const FVector Target = Winner->GetActorLocation();
	FVector ToTarget = Target - FVector(LooseLocation);
	const double Distance = ToTarget.Size();

	const double StepLength = Speed * Step;
	const double ArriveWithin = StepLength + static_cast<double>(TraceModeBTuning::PickupRadius());

	if (Distance <= ArriveWithin)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] spec v25 §2: the pulled Core reached %s."), *GetNameSafe(Winner));

		// Through TakeLooseCore like every other loose -> held transition, so the grace rule, the log
		// and Trace.ModeB.Verify all see a pull exactly as they see an interception. It clears the
		// loose state, which clears the turnover and this delivery with it.
		TakeLooseCore(Winner);
		return true;
	}

	const FVector Direction = ToTarget / Distance;
	LooseVelocity = Direction * Speed;
	LooseLocation = FVector(LooseLocation) + Direction * StepLength;
	SetActorLocation(LooseLocation, false, nullptr, ETeleportType::TeleportPhysics);

	return true;
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

	// SPEC v25 §2. Sampled once, outside the loop: the window cannot open or close between two
	// candidates within a single poll, and asking per candidate would make that look possible.
	const bool bLockoutActive = IsTurnoverActive();
	const ETraceTeam PullingTeam = GetTurnoverPullingTeam();

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

		// *** SPEC v25 §2. THE 5 s LOCKOUT IS ON THE TEAM THAT DROPPED IT. ***
		//
		// Row 2 of the table: during the window only the opposing team may pick the Core up, and this
		// is the whole of that half of it. Row 3 needs no code - IsTurnoverActive() goes false when the
		// window expires and the loop is back to "first contact, anyone", which is exactly "either team
		// can pick up the core by running over it".
		//
		// Written against the TEAM and not against LooseThrower on purpose: the thrower's own
		// lockout above is a 0.35 s anti-self-catch on ONE pawn, and this is a 5 s rule about a SIDE.
		// Conflating them would let a team-mate of the thrower jog over and reclaim the turnover.
		if (bLockoutActive && Candidate->GetTeam() != PullingTeam)
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
	bLooseFromThrow = false;
	CatchZoneTarget = nullptr;
	bCatchZoneContested = false;   // Spec v13 §5: a new flight starts its own contest.
	LastContactServerTime = -1.f;  // Spec v13 §8: and its own contact history.
	ClearPendingTurnover();        // Spec v19 §1.5: and its own landing.

	// SPEC v25 §2. And its own turnover. Every path that ends a flight comes through here, so this is
	// what guarantees the window, the pull race and the delivery cannot outlive the Core being loose -
	// a turnover left set on a Core somebody is now carrying would lock a whole team out of a Core
	// that is not even on the ground. The LATCH (bTurnoverRegisteredThisFlight) clears with it, so the
	// next throw is judged on its own landing.
	ClearTurnover(TEXT("the Core is no longer loose"));
	bTurnoverRegisteredThisFlight = false;
	PullInputHeld.Reset();

	LooseVelocity = FVector::ZeroVector;
	LooseThrower = nullptr;
	LooseStartServerTime = 0.f;
	// LooseFromTeam is deliberately NOT cleared here: TakeLooseCore reads it immediately afterwards
	// to decide the grace, and KickoffTo/GrantTo overwrite it on the next possession.
}

void ATraceCore::ClearPendingTurnover()
{
	PendingTurnoverLandedServerTime = -1.f;
	PendingTurnoverLandedFrame = 0;
	PendingTurnoverSurfacePoint = FVector::ZeroVector;
	PendingTurnoverSurfaceNormal = FVector::UpVector;
	PendingTurnoverArrivalSpeed = 0.0;
	PendingTurnoverArrivalSin = 1.0;
	bPendingTurnoverByRestProbe = false;
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
	TEXT("throw -> enemy interception WITH grace, a Core thrown into the ring goal, the loose reset ")
	TEXT("timer, a bot carrying it in, a bot throwing it in, and spec v6's ground turnover)."),
	ECVF_Default);

/**
 * SPEC v7 §4. The surface steps on their own.
 *
 * A separate arming switch rather than a seventh and eighth step everybody has to sit through,
 * because steps 4 and 5 of the full scenario wait on BOTS to score and legitimately take tens of
 * seconds each. A rule about hit normals should be measurable in a few seconds, and a check that is
 * slow to run is a check that stops being run.
 */
static TAutoConsoleVariable<int32> CVarModeBVerifySurfacesRequested(
	TEXT("Trace.ModeB.VerifySurfaces"),
	0,
	TEXT("1: run spec v7 §4's surface steps only - drop the Core on the TOP of a piece of cover and ")
	TEXT("assert a TURNOVER, then fire it at a WALL and assert a BOUNCE with no turnover."),
	ECVF_Default);

bool ATraceCore::DebugLaunchLoose(const FVector& From, const FVector& LaunchVelocity, ETraceTeam FromTeam,
	bool bAsThrow)
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
	// Spec v6 §4.2 is a rule about a THROW. The scenario's reset-timer step deliberately parks a Core
	// on the floor, and a Core that was never thrown must not turn over on contact with it - otherwise
	// that step would silently stop testing the timer and start testing the turnover.
	bLooseFromThrow = bAsThrow;
	CatchZoneTarget = nullptr;
	LastContactServerTime = -1.f;
	ClearPendingTurnover();   // Spec v19 §1.5: as the real throw above.
	bTurnoverRegisteredThisFlight = false;   // Spec v25 §2: as the real throw above.
	LooseFromTeam = FromTeam;
	LooseThrower = nullptr;
	LooseStartServerTime = GetServerTimeSeconds();
	LooseLocation = From;
	LooseVelocity = LaunchVelocity;

	// SPEC v10 §10: the debug launch is a teleport like any other, and it goes through the same funnel
	// so the verification scenario is testing the shipping path rather than a private one.
	ServerTeleport(From, TEXT("debug launch"));
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
		const bool bFullRequested = (CVarModeBVerifyRequested.GetValueOnAnyThread() != 0);
		const bool bSurfacesRequested = (CVarModeBVerifySurfacesRequested.GetValueOnAnyThread() != 0);

		if (!bFullRequested && !bSurfacesRequested)
		{
			return;
		}
		if (!IsModeB())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] refused: the match is playing mode A."));
			CVarModeBVerifyRequested->Set(0, ECVF_SetByConsole);
			CVarModeBVerifySurfacesRequested->Set(0, ECVF_SetByConsole);
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

		// SURFACES-ONLY starts at step 7. The full scenario still runs 0..8, so the v7 steps are also
		// exercised by whatever already runs Trace.ModeB.Verify — one set of steps, two ways in.
		bVerifySurfacesOnly = (!bFullRequested && bSurfacesRequested);
		VerifyStep = bVerifySurfacesOnly ? 7 : 0;
		VerifyPassCount = 0;
		VerifyFailCount = 0;
		VerifyStepDeadline = 0.f;
		UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] ===== mode B verification starting (%s) ====="),
			bVerifySurfacesOnly ? TEXT("spec v7 §4 surface rules only") : TEXT("all rules"));
	}

	// --- A step that is waiting on an outcome -----------------------------------------------------
	if (VerifyStepDeadline > 0.f)
	{
		const bool bTimedOut = (Now >= VerifyStepDeadline);

		// STEP 8 ARMS ITSELF LATE, and that is the whole design of it. It parks a Core on top of an
		// object as something that was NOT thrown, so no rule may touch it, and waits for it to come to
		// rest. Only then does it declare the Core "thrown" - at which point the flight integration is
		// already switched off for good and NO further sweep will ever run. The turnover that follows
		// can therefore only have come from the at-rest probe, which is the exact code path the user's
		// "stuck up top of an object" report is about. Anything else would be testing the contact test
		// step 7 already covers.
		if (VerifyStep == 8 && !bVerifyRestArmed && !bTimedOut)
		{
			// SPEC v25 §2 SIDE EFFECT, and it is a change in the WORLD rather than in this step. Step 7
			// now leaves its Core lying on the crate for the whole lockout window, so the bots gather
			// there — and step 8's parked Core is then taken on its first frame, before it can settle,
			// and the at-rest probe this step exists to exercise never runs. Take the subject back
			// rather than reporting a rule that was never given a chance.
			if (!bLoose && VerifyRestParkRetriesLeft > 0)
			{
				--VerifyRestParkRetriesLeft;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ModeBVerify] step 8: the parked Core was taken before it could settle (%s has it) ")
					TEXT("- re-parking, %d retries left."),
					*GetNameSafe(Carrier), VerifyRestParkRetriesLeft);

				VerifyStepDeadline = 0.f;   // Re-enters case 8 below, which parks it again.
				return;
			}

			if (!bLoose || !bLooseAtRest)
			{
				return;   // Still falling. Nothing to arm yet.
			}

			bVerifyRestArmed = true;
			bVerifyAwaitingTake = true;
			bVerifyTakeSeen = false;
			bVerifyAwaitingTurnover = true;   // Spec v25 §2: the registration is what this now judges.
			bVerifyTurnoverSeen = false;
			bLooseFromThrow = true;
			VerifyTurnoversAtStart = SurfaceStats.TopTurnovers;
			VerifyRescuesAtStart = SurfaceStats.RestProbeRescues;

			UE_LOG(LogTraceGame, Display,
				TEXT("[ModeBVerify] step 8: the Core is now AT REST at %s with the flight integration off - ")
				TEXT("arming the throw flag. Only the at-rest probe can turn it over from here."),
				*FVector(LooseLocation).ToCompactString());
			return;
		}

		// =========================================================================================
		// SPEC v25 §2 CHANGED WHAT STEPS 6-8 ARE WAITING FOR, AND THIS IS WHERE THAT LANDS.
		//
		// Those three steps have always asserted "a throw settled on a surface and the enemy ended up
		// with it". Under v25 the second half is no longer what the rule does: the Core STAYS on the
		// ground and the enemy gets a window in which they alone may pull it or run over it. So the
		// assertion moves one step earlier, onto the REGISTRATION - the throwing team is locked out,
		// its opponent is the side that may take it - and the surface tallies and the at-rest-probe
		// clause, which are the parts that actually prove which geometry fired, are unchanged.
		//
		// The pre-v25 arm (Trace.ModeB.TurnoverPull 0) still falls through to the take-based judgement
		// below, so ONE scenario measures whichever rule is armed rather than two that could drift.
		if (VerifyStep >= 6 && VerifyStep <= 8 && TraceModeBTuning::TurnoverPullEnabled())
		{
			if (bVerifyTurnoverSeen)
			{
				bVerifyTurnoverSeen = false;
				bVerifyAwaitingTurnover = false;
				bVerifyAwaitingTake = false;

				const bool bLockedTheThrower = (VerifyTurnoverLockedTeam != ETraceTeam::None)
					&& (TraceOpposingTeam(VerifyTurnoverLockedTeam) == VerifyExpectTeam);
				const bool bSurfaceOk = (VerifyStep == 6)
					|| (SurfaceStats.TopTurnovers > VerifyTurnoversAtStart);
				const bool bProbeOk = (VerifyStep != 8)
					|| (SurfaceStats.RestProbeRescues > VerifyRescuesAtStart);
				const bool bStepOk = bLockedTheThrower && bSurfaceOk && bProbeOk && bLoose;

				(bStepOk ? VerifyPassCount : VerifyFailCount)++;

				const FString StepDetail = FString::Printf(
					TEXT("a throw settled on %s and REGISTERED a turnover: %s locked out for %.1fs, %s may ")
					TEXT("pull or run over it, Core still loose=%d | top-of-object turnovers %d -> %d | ")
					TEXT("at-rest probe catches %d -> %d%s%s"),
					(VerifyStep == 6) ? TEXT("the ground") : TEXT("THE TOP OF AN OBJECT"),
					*TraceTeamName(VerifyTurnoverLockedTeam).ToString(), GetTurnoverLockoutSeconds(),
					*TraceTeamName(TraceOpposingTeam(VerifyTurnoverLockedTeam)).ToString(),
					bLoose ? 1 : 0,
					VerifyTurnoversAtStart, SurfaceStats.TopTurnovers,
					VerifyRescuesAtStart, SurfaceStats.RestProbeRescues,
					bSurfaceOk ? TEXT("") : TEXT(" *** it did not land on a raised surface ***"),
					bProbeOk ? TEXT("") : TEXT(" *** the at-rest probe is not what caught it ***"));

				if (bStepOk)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step %d PASS: %s"), VerifyStep, *StepDetail);
				}
				else
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: %s"), VerifyStep, *StepDetail);
				}

				VerifyStepDeadline = 0.f;
				++VerifyStep;
				return;
			}

			if (!bTimedOut)
			{
				return;   // Still falling, or still settling. The timeout below is the only way out.
			}
		}

		// Steps 0, 1, 6, 7 and 8 are waiting for a taker; steps 2 and 3 are waiting for the Core to
		// stop being loose (a goal or a reset both end with a kickoff); step 9 is waiting for a bounce.
		if (VerifyStep <= 1 || (VerifyStep >= 6 && VerifyStep <= 8))
		{
			if (bVerifyTakeSeen)
			{
				bVerifyTakeSeen = false;

				// --- Steps 6, 7 and 8: the turnover rule. --------------------------------------------
				//
				// Judged apart because they make a STRONGER claim than steps 0 and 1 do. Those two are
				// happy with whoever won the race to the Core; these assert the specific outcome the
				// rule promises - the enemy of the throwing team has it, and (because that crosses
				// sides) with the turnover grace applied. Anything else is a failure even if the grace
				// bookkeeping was internally consistent.
				//
				// Step 6 lands on the FLOOR (spec v6 §4.2), step 7 on the TOP OF AN OBJECT by contact,
				// step 8 on the top of an object with the contact test unable to run at all (spec v7
				// §4). One assertion for all three, because that is exactly the claim being made: they
				// are the same event and they have to end the same way.
				if (VerifyStep >= 6 && VerifyStep <= 8)
				{
					const bool bWentToTheEnemy = (VerifyFromTeam != ETraceTeam::None)
						&& (VerifyTookTeam == TraceOpposingTeam(VerifyFromTeam));
					const bool bGraceOk = bVerifyTookGrace
						&& bLastGrantTeamChanged && LastGrantGraceSeconds > 0.f;

					// STEPS 7 AND 8 ASSERT THE SURFACE TOO, and they have to: a turnover that fired
					// because the Core rolled off the crate and landed on the FLOOR would satisfy every
					// other clause here while testing precisely the rule that already worked. Step 8
					// additionally asserts that the AT-REST PROBE is what caught it. Both read the tally
					// the rule itself keeps, rather than re-deriving the geometry.
					const bool bSurfaceOk = (VerifyStep == 6)
						|| (SurfaceStats.TopTurnovers > VerifyTurnoversAtStart);
					const bool bProbeOk = (VerifyStep != 8)
						|| (SurfaceStats.RestProbeRescues > VerifyRescuesAtStart);
					const bool bStepOk = bWentToTheEnemy && bGraceOk && bSurfaceOk && bProbeOk;

					(bStepOk ? VerifyPassCount : VerifyFailCount)++;

					// Two calls rather than a ternary verbosity: UE_LOG pastes its second argument
					// into ELogVerbosity::<token>, so the level has to be a literal.
					const FString StepDetail = FString::Printf(
						TEXT("a %s throw settled on %s and turned over to %s (%s) | expected the nearest %s ")
						TEXT("player, with grace | grace %s | top-of-object turnovers %d -> %d | at-rest ")
						TEXT("probe catches %d -> %d%s%s"),
						*TraceTeamName(VerifyFromTeam).ToString(),
						(VerifyStep == 6) ? TEXT("the ground") : TEXT("THE TOP OF AN OBJECT"),
						*VerifyTakerName, *TraceTeamName(VerifyTookTeam).ToString(),
						*TraceTeamName(TraceOpposingTeam(VerifyFromTeam)).ToString(),
						bGraceOk ? *FString::Printf(TEXT("APPLIED %.2fs"), LastGrantGraceSeconds) : TEXT("MISSING"),
						VerifyTurnoversAtStart, SurfaceStats.TopTurnovers,
						VerifyRescuesAtStart, SurfaceStats.RestProbeRescues,
						bSurfaceOk ? TEXT("") : TEXT(" *** it did not land on a raised surface ***"),
						bProbeOk ? TEXT("") : TEXT(" *** the at-rest probe is not what caught it ***"));

					if (bStepOk)
					{
						UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step %d PASS: %s"), VerifyStep, *StepDetail);
					}
					else
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step %d FAIL: %s"), VerifyStep, *StepDetail);
					}

					VerifyStepDeadline = 0.f;
					++VerifyStep;
					return;
				}

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
		else if (VerifyStep == 9)
		{
			// --- Step 9: SPEC v7 §4, "walls should not [turn over], the core should bounce off those".
			//
			// TWO facts, and both are needed. That a bounce HAPPENED is the positive half - counted by
			// the rule itself at the moment it classified the normal as a wall. That NO TURNOVER
			// happened is the negative half, and it is the one the user actually asked for: a wall that
			// handed the Core to the nearest enemy would be the bug, not the bounce.
			//
			// Judged the instant the bounce is seen rather than at the end of a fixed window, because
			// the Core CORRECTLY falls to the floor afterwards and turns over there - waiting would
			// measure the floor rule and report the wall rule broken.
			const int32 BouncesNow = SurfaceStats.WallBounces;
			const int32 TurnoversNow = SurfaceStats.GroundTurnovers + SurfaceStats.TopTurnovers;

			if (BouncesNow > VerifyWallBouncesAtStart)
			{
				const bool bNoTurnover = (TurnoversNow == VerifyTurnoversAtStart);
				const bool bStillLoose = bLoose;
				const bool bStepOk = bNoTurnover && bStillLoose;

				(bStepOk ? VerifyPassCount : VerifyFailCount)++;

				const FString StepDetail = FString::Printf(
					TEXT("the Core struck a WALL and bounced (wall bounces %d -> %d) | turnovers %d -> %d ")
					TEXT("(must not move) | still loose=%d | velocity %s"),
					VerifyWallBouncesAtStart, BouncesNow, VerifyTurnoversAtStart, TurnoversNow,
					bStillLoose ? 1 : 0, *FVector(LooseVelocity).ToCompactString());

				if (bStepOk)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[ModeBVerify] step 9 PASS: %s"), *StepDetail);
				}
				else
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 9 FAIL: %s"), *StepDetail);
				}

				VerifyStepDeadline = 0.f;
				++VerifyStep;
				return;
			}

			if (bTimedOut)
			{
				// RE-FIRE RATHER THAN FAIL, up to a few times, and this is a correction to the harness
				// rather than a leniency. [DIAGNOSED] from three runs of its own log: the shot is fired
				// across open pitch with ten bots playing on it, and what actually happens on a failing
				// run is that a bot standing in the corridor INTERCEPTS the Core 0.09s after launch —
				// the catch-zone magnet even curves it into them — so the step never tests the wall at
				// all. It then reports the WALL RULE broken on the strength of where a bot was standing.
				//
				// It surfaced this pass because spec v19 §1.5's settle moves step 8's turnover 0.15s
				// later, which reshuffles where everybody is when step 9 fires; the underlying weakness
				// is older than that and was passing by luck. A retry makes the step measure the thing
				// it names. When the retries run out it still FAILS, loudly, and says which it was.
				if (VerifyWallShotRetriesLeft > 0)
				{
					--VerifyWallShotRetriesLeft;
					UE_LOG(LogTraceGame, Display,
						TEXT("[ModeBVerify] step 9: the shot never reached the wall (a bot took it, or it was ")
						TEXT("reset) - RE-FIRING, %d attempt(s) left."),
						VerifyWallShotRetriesLeft);

					VerifyStepDeadline = 0.f;
					return;   // VerifyStep is unchanged, so the launcher below fires it again.
				}

				++VerifyFailCount;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[ModeBVerify] step 9 FAIL: the Core never struck the wall in any attempt (loose=%d, ")
					TEXT("at rest=%d, at %s, turnovers %d -> %d)."),
					bLoose ? 1 : 0, bLooseAtRest ? 1 : 0, *FVector(LooseLocation).ToCompactString(),
					VerifyTurnoversAtStart, TurnoversNow);

				VerifyStepDeadline = 0.f;
				++VerifyStep;
			}
			return;
		}
		else if (VerifyStep == 4 || VerifyStep == 5)
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

		// bAsThrow = FALSE. Spec v6 §4.2 turns a thrown Core over the instant it touches the floor, and
		// this step's entire premise is a Core that LIES on the floor untouched until the timer fires.
		// Launching it as a throw would hand it to the nearest enemy on the first frame and this step
		// would silently become a second (worse) test of the turnover rule.
		if (!DebugLaunchLoose(Corner, FVector(0.0, 0.0, -50.0), FromTeam, /*bAsThrow=*/false))
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
		//
		// STEP 5 IS 4800, AND THE FLOOR UNDER IT IS NOT THE BALLISTICS - IT IS THE CARRY-IN BAND.
		// MEASURED, twice, and worth writing down because the obvious reasoning is wrong twice over.
		// Spec v6 moved the goal 2400 uu further away and 1100 uu up, so the first instinct was that
		// the shot must now be taken from closer and this step was set to 2600. It failed three runs
		// running with "a throw did NOT leave" - and the reason is that UpdateThrow returns early
		// while bCommitCarryIn is set, and TraceBotConstants::CarryInCommitDistance is 4200 uu. At
		// 2600 the bot was not refusing the shot, it was correctly RUNNING IT IN, and the step was
		// asking a carrier inside the commit band to do the one thing the commit exists to stop.
		//
		// The range worry was unfounded in the bargain: the same run measured a live bot taking (and
		// scoring) a shot at the hoop from 4835 uu, because MaxThrowRange solves over launch ANGLE and
		// a lofted arc carries far further than the flat-throw figure the tuning log prints.
		//
		// So: outside the commit band, inside the measured reach.
		const double Standoff = (VerifyStep == 4) ? 700.0 : 4800.0;
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

	case 6:
	{
		// =========================================================================================
		// SPEC v6 §4.2: A THROWN CORE THAT HITS THE GROUND IS THE ENEMY'S.
		//
		// Scripted for the same reason steps 2 and 3 are: waiting for a bot to happen to throw one
		// into the dirt in front of an enemy is a hope, not a test. This throws it at the floor
		// deliberately - from the real holder, through the same DebugLaunchLoose the goal step uses,
		// flagged as a throw so the rule is armed - and then asserts the outcome the rule promises.
		// =========================================================================================
		if (!IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;   // Between possessions. Not on a clock yet.
		}

		const ETraceTeam FromTeam = Carrier->GetTeam();
		const ETraceTeam ToTeam = TraceOpposingTeam(FromTeam);

		// Put an enemy where the throw will land, so "the CLOSEST player on the enemy team" is a fact
		// the step controls rather than an accident of where ten bots happen to be standing. A second
		// enemy is deliberately left wherever they are: if the rule picked the wrong one, the taker
		// name in the PASS line is what says so.
		ATraceCharacter* Nearest = nullptr;
		for (ATraceCharacter* Candidate : Everyone)
		{
			if (IsValid(Candidate) && Candidate->IsAlive() && Candidate->GetTeam() == ToTeam)
			{
				Nearest = Candidate;
				break;
			}
		}

		if (Nearest == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] step 6 SKIPPED: no living %s player to award the turnover to."),
				*TraceTeamName(ToTeam).ToString());
			++VerifyStep;
			return;
		}

		// 2200 uu AHEAD OF THE THROWER, not 900. MEASURED: at 900 the ex-carrier - who becomes a loose
		// Core chaser the instant they are released - was inside their own 500 uu catch zone of the
		// falling Core within 140 ms and caught it in the air, and the step reported the turnover rule
		// broken when what it had actually observed was §4.1 working. The drop has to land somewhere
		// nobody can reach first, or this measures the magnet.
		const FVector CarrierLocation = Carrier->GetActorLocation();
		const FVector Forward = Carrier->GetActorForwardVector().GetSafeNormal2D();
		const FVector LandingSpot = CarrierLocation + Forward * 2200.0;

		// 700 uu, NOT arm's length. The catch zone (§4.1) reaches 500 uu from a capsule's surface, so
		// an enemy any closer would magnet the falling Core into their hands BEFORE it landed and the
		// step would quietly become a second test of the magnet instead of a test of the turnover.
		Nearest->SetActorLocation(
			FVector(LandingSpot.X + 700.0, LandingSpot.Y, Nearest->GetActorLocation().Z),
			false, nullptr, ETeleportType::TeleportPhysics);

		// Aimed DOWN, HARD and from LOW: a throw that is going to hit the ground and nothing else,
		// inside two or three frames, so there is no flight for anybody to intercept and the step
		// measures the landing. Low enough that it cannot cross a goal or clip a cover block on the
		// way, which would end the step for the wrong reason.
		const FVector Start = LandingSpot + FVector(0.0, 0.0, 220.0);

		VerifyThrower = Carrier;
		VerifyExpectTeam = ToTeam;
		VerifyExpectGrace = true;
		bVerifyAwaitingTake = true;
		bVerifyTakeSeen = false;
		bVerifyAwaitingTurnover = true;   // Spec v25 §2: the registration is what this now judges.
		bVerifyTurnoverSeen = false;

		if (!DebugLaunchLoose(Start, FVector(0.0, 0.0, -2000.0), FromTeam, /*bAsThrow=*/true))
		{
			++VerifyFailCount;
			bVerifyAwaitingTake = false;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 6 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 6: a %s throw dropped at %s with %s (%s) standing 700 uu away - ")
			TEXT("expecting a GROUND TURNOVER to them, with grace."),
			*TraceTeamName(FromTeam).ToString(), *LandingSpot.ToCompactString(),
			*GetNameSafe(Nearest), *TraceTeamName(ToTeam).ToString());

		VerifyStepDeadline = Now + 4.f;
		return;
	}

	case 7:
	{
		// =========================================================================================
		// SPEC v7 §4, HALF ONE: THE TOP OF AN OBJECT IS A TURNOVER.
		//
		// "Sometimes the core gets stuck up top of an object in gamemode b. This should also count as
		// a turnover."
		//
		// Step 6 proves the FLOOR case. This proves the case the user actually reported, and it has to
		// be driven rather than waited for: a bot throw that happens to come down on the roof of a
		// crate, with an enemy near enough to receive it, is not something a test run can be promised.
		// The Core is dropped from just above a real, world-sampled raised surface, and the step then
		// asserts the same three things step 6 does PLUS that the tally recorded a TOP turnover.
		// =========================================================================================
		if (!IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;   // Between possessions. Not on a clock yet.
		}

		FVector TopPoint = FVector::ZeroVector;
		FVector WallPoint = FVector::ZeroVector;
		FVector WallNormal = FVector::ZeroVector;
		if (!FindVerificationSurfaces(TopPoint, WallPoint, WallNormal))
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] steps 7-9 SKIPPED: no raised cover found in the arena to land on."));
			VerifyStep = 10;
			return;
		}

		const ETraceTeam FromTeam = Carrier->GetTeam();
		const ETraceTeam ToTeam = TraceOpposingTeam(FromTeam);

		ATraceCharacter* Nearest = nullptr;
		for (ATraceCharacter* Candidate : Everyone)
		{
			if (IsValid(Candidate) && Candidate->IsAlive() && Candidate->GetTeam() == ToTeam)
			{
				Nearest = Candidate;
				break;
			}
		}

		if (Nearest == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] step 7 SKIPPED: no living %s player to award the turnover to."),
				*TraceTeamName(ToTeam).ToString());
			++VerifyStep;
			return;
		}

		// 900 uu away, for the reason step 6 documents at length: the catch zone reaches 500 uu from a
		// capsule's surface, so an enemy any closer would magnet the Core out of the air and this step
		// would quietly become a third test of §4.1.
		Nearest->SetActorLocation(
			FVector(TopPoint.X + 900.0, TopPoint.Y, Nearest->GetActorLocation().Z),
			false, nullptr, ETeleportType::TeleportPhysics);

		const FVector Start = TopPoint + FVector(0.0, 0.0, 160.0);

		VerifyThrower = Carrier;
		VerifyExpectTeam = ToTeam;
		VerifyExpectGrace = true;
		bVerifyAwaitingTake = true;
		bVerifyTakeSeen = false;
		bVerifyAwaitingTurnover = true;   // Spec v25 §2: the registration is what this now judges.
		bVerifyTurnoverSeen = false;
		VerifyTurnoversAtStart = SurfaceStats.TopTurnovers;

		if (!DebugLaunchLoose(Start, FVector(0.0, 0.0, -1400.0), FromTeam, /*bAsThrow=*/true))
		{
			++VerifyFailCount;
			bVerifyAwaitingTake = false;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 7 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		const ATraceArenaBuilder* VerifyArena = ATraceArenaBuilder::Get(World);
		const double VerifyFloorZ = (VerifyArena != nullptr) ? VerifyArena->GetFieldBounds().Min.Z : 0.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 7: a %s throw dropped onto the TOP of an object at %s (%.0f uu above ")
			TEXT("the floor) with %s (%s) standing 900 uu away - expecting a SURFACE TURNOVER to them, with grace."),
			*TraceTeamName(FromTeam).ToString(), *TopPoint.ToCompactString(),
			TopPoint.Z - VerifyFloorZ,
			*GetNameSafe(Nearest), *TraceTeamName(ToTeam).ToString());

		VerifyStepDeadline = Now + 5.f;
		return;
	}

	case 8:
	{
		// =========================================================================================
		// SPEC v7 §4: THE STUCK CORE ITSELF — a turnover with the flight integration switched OFF.
		//
		// THIS IS THE STEP THAT TESTS THE REPORTED BUG. Step 7 drops a Core onto a crate and the
		// CONTACT test catches it, which is the path that already existed; a Core that reaches rest
		// without a qualifying contact - an edge hit, a hitch, a state lock held on the landing frame -
		// switches the integration off and is never asked again, and that is the Core the user watched
		// sit on top of an object until the reset timer.
		//
		// Reproduced exactly: park a Core on the crate as something NOBODY threw (so no rule may touch
		// it), let it settle, and only then declare it thrown. From that instant the contact test is
		// unreachable by construction - there is no sweep - so a turnover can only come from the
		// at-rest probe. If the probe is broken this step hangs and fails, and nothing else does.
		// =========================================================================================
		if (!IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;
		}

		FVector RestTopPoint = FVector::ZeroVector;
		FVector UnusedWallPoint = FVector::ZeroVector;
		FVector UnusedWallNormal = FVector::ZeroVector;
		if (!FindVerificationSurfaces(RestTopPoint, UnusedWallPoint, UnusedWallNormal))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 8 SKIPPED: no raised cover to park a Core on."));
			++VerifyStep;
			return;
		}

		const ETraceTeam RestFromTeam = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::Blue;
		const ETraceTeam RestToTeam = TraceOpposingTeam(RestFromTeam);

		ATraceCharacter* RestNearest = nullptr;
		for (ATraceCharacter* Candidate : Everyone)
		{
			if (IsValid(Candidate) && Candidate->IsAlive() && Candidate->GetTeam() == RestToTeam)
			{
				RestNearest = Candidate;
				break;
			}
		}

		if (RestNearest == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBVerify] step 8 SKIPPED: no living %s player to award the turnover to."),
				*TraceTeamName(RestToTeam).ToString());
			++VerifyStep;
			return;
		}

		RestNearest->SetActorLocation(
			FVector(RestTopPoint.X + 900.0, RestTopPoint.Y, RestNearest->GetActorLocation().Z),
			false, nullptr, ETeleportType::TeleportPhysics);

		// bAsThrow = FALSE. That is the whole trick: while it is falling and settling, NO rule is
		// allowed to fire, so the Core reaches rest exactly as an abandoned one does.
		if (!DebugLaunchLoose(RestTopPoint + FVector(0.0, 0.0, 60.0), FVector(0.0, 0.0, -200.0),
			RestFromTeam, /*bAsThrow=*/false))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 8 FAIL: could not park the Core."));
			++VerifyStep;
			return;
		}

		VerifyThrower = Carrier;
		VerifyExpectTeam = RestToTeam;
		VerifyExpectGrace = true;
		bVerifyRestArmed = false;
		bVerifyAwaitingTake = false;
		bVerifyTakeSeen = false;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 8: parking an unthrown %s Core on the TOP of an object at %s with %s (%s) ")
			TEXT("900 uu away - it must settle first, and the AT-REST PROBE alone must then turn it over."),
			*TraceTeamName(RestFromTeam).ToString(), *RestTopPoint.ToCompactString(),
			*GetNameSafe(RestNearest), *TraceTeamName(RestToTeam).ToString());

		VerifyStepDeadline = Now + 6.f;
		return;
	}

	case 9:
	{
		// =========================================================================================
		// SPEC v7 §4, THE OTHER HALF: A WALL IS A BOUNCE, NOT A TURNOVER.
		//
		// "Walls should not, the core should bounce off those."
		//
		// Fired horizontally at the SIDE of the same piece of cover step 7 landed on, hard enough and
		// from close enough that gravity cannot drop it onto anything horizontal on the way. What is
		// asserted is the pair: a bounce was registered, and the turnover tally did not move.
		// =========================================================================================
		FVector TopPoint = FVector::ZeroVector;
		FVector WallPoint = FVector::ZeroVector;
		FVector WallNormal = FVector::ZeroVector;
		if (!FindVerificationSurfaces(TopPoint, WallPoint, WallNormal))
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 9 SKIPPED: no wall found to bounce off."));
			++VerifyStep;
			return;
		}

		const ETraceTeam FromTeam = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::Blue;

		// 420 uu out along the wall's own normal, moving straight back down it at 2400 uu/s: the flight
		// lasts under 0.2 s, in which the Core falls ~20 uu under the mode-B gravity model, so it
		// cannot clip the floor or the top of the block before it reaches the face under test.
		const FVector Start = WallPoint + WallNormal * 420.0;
		const FVector LaunchVelocity = -WallNormal * 2400.0;

		VerifyWallBouncesAtStart = SurfaceStats.WallBounces;
		VerifyTurnoversAtStart = SurfaceStats.GroundTurnovers + SurfaceStats.TopTurnovers;
		bVerifyAwaitingTake = false;
		bVerifyTakeSeen = false;

		// Only on the FIRST attempt: a retry must not top its own allowance back up, or a step that can
		// never reach the wall would retry forever instead of failing.
		if (VerifyWallShotRetriesLeft <= 0 && !bVerifyWallShotFiredOnce)
		{
			bVerifyWallShotFiredOnce = true;
			VerifyWallShotRetriesLeft = 3;
		}

		if (!DebugLaunchLoose(Start, LaunchVelocity, FromTeam, /*bAsThrow=*/true))
		{
			++VerifyFailCount;
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBVerify] step 8 FAIL: could not launch."));
			++VerifyStep;
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] step 9: a %s throw fired at a WALL at %s (normal %s, %.0f deg from up) from ")
			TEXT("%s - expecting a BOUNCE and NO turnover."),
			*TraceTeamName(FromTeam).ToString(), *WallPoint.ToCompactString(), *WallNormal.ToCompactString(),
			FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(WallNormal.Z, -1.0, 1.0))),
			*Start.ToCompactString());

		VerifyStepDeadline = Now + 4.f;
		return;
	}

	default:
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] ===== finished: %d PASS, %d FAIL ====="), VerifyPassCount, VerifyFailCount);
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBVerify] spec v7 §4 tally: turnovers off the ground %d, off the TOP of an object %d ")
			TEXT("| wall bounces %d (rest refused on a wall %d) | landings the at-rest probe caught %d"),
			SurfaceStats.GroundTurnovers, SurfaceStats.TopTurnovers, SurfaceStats.WallBounces,
			SurfaceStats.WallRestRefusals, SurfaceStats.RestProbeRescues);

		CVarModeBVerifyRequested->Set(0, ECVF_SetByConsole);
		CVarModeBVerifySurfacesRequested->Set(0, ECVF_SetByConsole);
		bVerifySurfacesOnly = false;
		VerifyStep = -1;
		return;
	}
	}
}

bool ATraceCore::FindVerificationSurfaces(FVector& OutTopPoint, FVector& OutWallPoint, FVector& OutWallNormal) const
{
	const UWorld* World = GetWorld();
	const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
	if (World == nullptr || Arena == nullptr)
	{
		return false;
	}

	const FBox FieldBox = Arena->GetFieldBounds();
	if (FieldBox.IsValid == 0)
	{
		return false;
	}

	const double FloorZ = FieldBox.Min.Z;
	const FVector Centre = FieldBox.GetCenter();

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreSurfaceProbe), /*bTraceComplex=*/false, this);
	Params.AddIgnoredActor(this);

	const float UpNormalZ = TraceModeBTuning::SurfaceUpNormalZ();
	const float Radius = TraceModeBTuning::CollisionRadius;

	// SAMPLED, NOT CONSTRUCTED. The rule under test reads world geometry, so the test has to find its
	// crate the same way - a probe grid over the MIDDLE of the pitch, deliberately away from both goal
	// mouths so a dropped Core cannot score and end the step for the wrong reason.
	const double SpanX = FieldBox.GetSize().X * 0.25;
	const double SpanY = FieldBox.GetSize().Y * 0.40;

	for (int32 IndexX = -6; IndexX <= 6; ++IndexX)
	{
		for (int32 IndexY = -6; IndexY <= 6; ++IndexY)
		{
			const FVector Column(
				Centre.X + (SpanX * IndexX) / 6.0,
				Centre.Y + (SpanY * IndexY) / 6.0,
				0.0);

			FHitResult TopHit;
			const bool bHitTop = World->SweepSingleByChannel(
				TopHit,
				FVector(Column.X, Column.Y, FieldBox.Max.Z),
				FVector(Column.X, Column.Y, FloorZ - 50.0),
				FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(Radius), Params);

			// Raised, horizontal-ish, and not so tall that it is a wall cap or the roof: the point is a
			// piece of cover a thrown Core could plausibly settle on.
			if (!bHitTop
				|| TopHit.ImpactNormal.Z < UpNormalZ
				|| TopHit.ImpactPoint.Z < FloorZ + 150.0
				|| TopHit.ImpactPoint.Z > FloorZ + 1200.0)
			{
				continue;
			}

			// Its SIDE. Probed from four directions at a height comfortably below the top face, so the
			// hit is the flank of the same block and not its lip.
			const FVector SideSample = FVector(TopHit.ImpactPoint.X, TopHit.ImpactPoint.Y,
				FMath::Max(FloorZ + 60.0, TopHit.ImpactPoint.Z - 90.0));

			static const FVector Directions[] =
			{
				FVector(1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0),
				FVector(0.0, 1.0, 0.0), FVector(0.0, -1.0, 0.0)
			};

			for (const FVector& Direction : Directions)
			{
				FHitResult SideHit;
				const bool bHitSide = World->SweepSingleByChannel(
					SideHit, SideSample + Direction * 900.0, SideSample,
					FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(Radius), Params);

				// A WALL by the rule's own definition, not by ours: whatever the shipping threshold
				// currently says is not a floor. Asking the same accessor is what keeps the test honest
				// if the threshold is ever retuned.
				if (!bHitSide || SideHit.ImpactNormal.Z >= UpNormalZ || SideHit.bStartPenetrating)
				{
					continue;
				}

				// The launch point has to be in open air, or the step tests whatever is between them.
				FHitResult ClearHit;
				const FVector LaunchPoint = SideHit.ImpactPoint + SideHit.ImpactNormal * 420.0;
				if (World->SweepSingleByChannel(ClearHit, LaunchPoint, LaunchPoint,
					FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(Radius * 2.f), Params))
				{
					continue;
				}

				OutTopPoint = TopHit.ImpactPoint;
				OutWallPoint = SideHit.ImpactPoint;
				OutWallNormal = SideHit.ImpactNormal.GetSafeNormal();
				return true;
			}
		}
	}

	return false;
}

// =================================================================================================
// SPEC v13 §8 — THE REPRODUCTION
//
// "Sometimes the core is thrown and it turns over before it touches the ground." The instruction was
// to REPRODUCE IT FIRST, and this is what does it: a throw fired repeatedly ACROSS the flat top of a
// piece of arena cover, at a shallow angle, exactly the shape of throw a player makes over a crate.
//
// WHY IT CAN GO RED, WHICH IS THE PART THAT MATTERS. Trace.ModeB.LandingRule 0 restores the pre-v13
// behaviour verbatim - any upward-facing contact is a landing - and the two arms are otherwise the
// same code driving the same geometry. So the pass criterion is not "we saw nothing bad": it is a
// number that MOVES between the arms, measured by a counter (SurfaceStats.MidAirTurnovers) that is
// computed identically in both and that reads the event's geometry rather than the rule's verdict.
// A harness that could only ever print zero would be the wall-clip harness this project has already
// been burned by, and it would prove nothing.
//
// It also fires DROP shots - straight down onto open floor - in the same run, so the same tally shows
// spec v6 §4.2's ordinary ground turnover still firing. A "fix" that stopped every turnover would
// pass a graze-only harness and would be a much worse bug than the one being fixed.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarModeBTurnoverRepro(
	TEXT("Trace.ModeB.TurnoverRepro"),
	0,
	TEXT("MODE B, spec v13 §8. Set to N: fire N scripted throws - alternating a GRAZE across the flat ")
	TEXT("top of a piece of cover and a DROP onto open floor - and print how many turnovers fired in ")
	TEXT("mid-air. Run it twice: once as shipped, and once with Trace.ModeB.LandingRule 0, which arms ")
	TEXT("the pre-v13 rule and is the arm that must go RED."),
	ECVF_Default);

/**
 * How many shots the repro was asked for, from the CVar or from -TraceTurnoverRepro=N.
 *
 * The command-line form exists because arming this through -ExecCmds requires a SPACE
 * ("Trace.ModeB.TurnoverRepro 12"), which requires quoting, and a quoted -ExecCmds argument has
 * already broken a command line into the URL parser on this project and produced a verification that
 * "passed" because none of its commands ran. `-TraceTurnoverRepro=12` cannot do that.
 */
static int32 TraceModeBTurnoverReproShots()
{
	static const int32 FromCommandLine = []() -> int32
	{
		int32 Value = 0;
		return FParse::Value(FCommandLine::Get(), TEXT("TraceTurnoverRepro="), Value) ? Value : 0;
	}();

	return FMath::Max(FromCommandLine, CVarModeBTurnoverRepro.GetValueOnGameThread());
}

double ATraceCore::MeasureTopFaceExtent(const FVector& FromPoint, FVector& OutDirection) const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0.0;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreTopExtent), /*bTraceComplex=*/false, this);
	Params.AddIgnoredActor(this);

	static const FVector Compass[] =
	{
		FVector(1.0, 0.0, 0.0),  FVector(-1.0, 0.0, 0.0),
		FVector(0.0, 1.0, 0.0),  FVector(0.0, -1.0, 0.0),
		FVector(0.707, 0.707, 0.0),  FVector(-0.707, 0.707, 0.0),
		FVector(0.707, -0.707, 0.0), FVector(-0.707, -0.707, 0.0)
	};

	constexpr double StepUU = 40.0;
	constexpr int32 MaxSteps = 24;          // 960 uu, comfortably longer than any cover block here.
	constexpr double SameFaceTolerance = 8.0;

	double BestRun = 0.0;
	OutDirection = FVector::ForwardVector;

	for (const FVector& Direction : Compass)
	{
		double Run = 0.0;

		for (int32 Step = 1; Step <= MaxSteps; ++Step)
		{
			const FVector Column = FromPoint + Direction * (StepUU * Step);

			FHitResult Hit;
			const bool bHit = World->SweepSingleByChannel(
				Hit,
				FVector(Column.X, Column.Y, FromPoint.Z + 120.0),
				FVector(Column.X, Column.Y, FromPoint.Z - 40.0),
				FQuat::Identity, ECC_WorldStatic,
				FCollisionShape::MakeSphere(TraceModeBTuning::CollisionRadius), Params);

			// THE SAME FACE, not merely SOMETHING. A run that wandered onto a taller block beside this
			// one would aim the graze into a wall and the shot would test the bounce rule instead.
			if (!bHit
				|| Hit.ImpactNormal.Z < TraceModeBTuning::SurfaceUpNormalZ()
				|| FMath::Abs(Hit.ImpactPoint.Z - FromPoint.Z) > SameFaceTolerance)
			{
				break;
			}

			Run = StepUU * Step;
		}

		if (Run > BestRun)
		{
			BestRun = Run;
			OutDirection = Direction.GetSafeNormal();
		}
	}

	return BestRun;
}

void ATraceCore::TickTurnoverRepro()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	// --- Arm ---------------------------------------------------------------------------------------
	if (!bTurnoverReproArmed)
	{
		const int32 Requested = TraceModeBTurnoverReproShots();
		if (Requested <= 0 || bTurnoverReproReported)
		{
			return;
		}

		// The same settled-half gate Trace.ModeB.Verify uses, and for the same reason: arming during
		// the pre-match window puts the first shot on the kickoff frame, which cancels it and makes the
		// harness report on a rule that never ran.
		const ATraceGameState* GameState = World->GetGameState<ATraceGameState>();
		if (!IsModeB() || GameState == nullptr
			|| GameState->TraceMatchState != ETraceMatchState::InProgress
			|| GameState->IsHalfTimeBreak()
			|| !IsValid(Carrier) || !Carrier->IsAlive() || bLoose)
		{
			return;
		}

		FVector TopPoint = FVector::ZeroVector;
		FVector UnusedWallPoint = FVector::ZeroVector;
		FVector UnusedWallNormal = FVector::ZeroVector;
		if (!FindVerificationSurfaces(TopPoint, UnusedWallPoint, UnusedWallNormal))
		{
			bTurnoverReproReported = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBTurnoverRepro] REFUSED: no raised cover with a flat top anywhere in the middle ")
				TEXT("of the pitch, so there is nothing to graze. The harness is NOT reporting a pass - it ")
				TEXT("could not run."));
			return;
		}

		FVector GrazeDirection = FVector::ForwardVector;
		const double Extent = MeasureTopFaceExtent(TopPoint, GrazeDirection);

		// The graze needs a face long enough for the Core to descend onto it INSIDE the face rather than
		// sailing off the far edge. Refusing loudly is the honest answer; a shot fired at a 40 uu ledge
		// would miss and the run would report "no mid-air turnovers" while having tested nothing.
		if (Extent < 240.0)
		{
			bTurnoverReproReported = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBTurnoverRepro] REFUSED: the flat top at %s runs only %.0f uu, which is too ")
				TEXT("short to graze across. The harness is NOT reporting a pass - it could not run."),
				*TopPoint.ToCompactString(), Extent);
			return;
		}

		bTurnoverReproArmed = true;
		TurnoverReproShotsLeft = FMath::Clamp(Requested, 1, 100);
		TurnoverReproTopPoint = TopPoint;
		TurnoverReproGrazeDirection = GrazeDirection;
		TurnoverReproTopExtent = Extent;
		TurnoverReproGrazeShots = 0;
		TurnoverReproDropShots = 0;
		TurnoverReproSkipped = 0;
		TurnoverReproMidAirAtStart = SurfaceStats.MidAirTurnovers;
		TurnoverReproLandedAtStart = SurfaceStats.LandedTurnovers;
		TurnoverReproRejectedAtStart = SurfaceStats.GlancingContactsRejected;
		TurnoverReproUnseenAtStart = SurfaceStats.UnseenTurnovers;
		TurnoverReproSeenAtStart = SurfaceStats.SeenTurnovers;
		TurnoverReproUngroundedAtStart = SurfaceStats.UngroundedTurnovers;
		TurnoverReproGroundedAtStart = SurfaceStats.GroundedTurnovers;
		SurfaceStats.FewestTurnoverContactFrames = -1;
		SurfaceStats.ShortestTurnoverDwellSeconds = -1.f;
		SurfaceStats.WorstTurnoverGapUU = 0.f;
		TurnoverReproNextShotTime = Now + 1.f;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBTurnoverRepro] ===== spec v13 §8 + v19 §1.5: %d shots across the flat top at %s ")
			TEXT("(the face runs %.0f uu along %s) | landing rule = %s | grounded rule = %s (settle %.2fs, ")
			TEXT("slack %.0f uu) | mid-air test: faster than %.0f uu/s AND shallower than %.0f deg ====="),
			TurnoverReproShotsLeft, *TopPoint.ToCompactString(), Extent,
			*GrazeDirection.ToCompactString(),
			(!TraceModeBLegacyLandingRule())
				? TEXT("v13 (an actual landing)") : TEXT("PRE-v13 (any upward normal) - THE BUG, ARMED"),
			(!TraceModeBLegacyGroundedRule())
				? TEXT("v19 (settle, then award)") : TEXT("PRE-v19 (award on the contact frame) - THE BUG, ARMED"),
			TraceModeBLegacyGroundedRule() ? 0.f : CVarModeBTurnoverSettleSeconds.GetValueOnAnyThread(),
			CVarModeBTurnoverContactSlack.GetValueOnAnyThread(),
			CVarModeBMidAirTurnoverSpeed.GetValueOnAnyThread(),
			CVarModeBMidAirTurnoverDegrees.GetValueOnAnyThread());
		return;
	}

	// --- Report and stop ---------------------------------------------------------------------------
	if (TurnoverReproShotsLeft <= 0)
	{
		if (bTurnoverReproReported)
		{
			return;
		}

		// Wait for the last shot to actually resolve before judging it. A tally printed while a Core is
		// still in the air is a tally that is missing its last result.
		if (bLoose)
		{
			return;
		}

		bTurnoverReproReported = true;

		const int32 MidAir = SurfaceStats.MidAirTurnovers - TurnoverReproMidAirAtStart;
		const int32 Landed = SurfaceStats.LandedTurnovers - TurnoverReproLandedAtStart;
		const int32 Rejected = SurfaceStats.GlancingContactsRejected - TurnoverReproRejectedAtStart;

		// SPEC v19 §1.5's half of the same run.
		const int32 Unseen = SurfaceStats.UnseenTurnovers - TurnoverReproUnseenAtStart;
		const int32 Seen = SurfaceStats.SeenTurnovers - TurnoverReproSeenAtStart;
		const int32 Ungrounded = SurfaceStats.UngroundedTurnovers - TurnoverReproUngroundedAtStart;
		const int32 Grounded = SurfaceStats.GroundedTurnovers - TurnoverReproGroundedAtStart;

		// FOUR CLAUSES, and the two "> 0" ones are what stop this from being a harness that passes by
		// doing nothing. Zero mid-air and zero unseen turnovers are both trivially achievable by never
		// turning the Core over at all, which would be a far worse bug than the ones under repair, so
		// the run must ALSO show ordinary landings still handing possession over — and, since v19,
		// show that those landings were WATCHED.
		const bool bNoMidAir = (MidAir == 0);
		const bool bStillTurningOver = (Landed > 0);
		const bool bNoneUnseen = (Unseen == 0);
		const bool bNoneUngrounded = (Ungrounded == 0);
		const bool bSomeWereSeen = (Seen > 0);
		const bool bPass = bNoMidAir && bStillTurningOver && bNoneUnseen && bNoneUngrounded && bSomeWereSeen;

		const FString Detail = FString::Printf(
			TEXT("%d shots (%d grazes across the top, %d drops onto the floor, %d skipped) | MID-AIR ")
			TEXT("TURNOVERS %d (must be 0) | landed turnovers %d (must be > 0, or the fix has simply ")
			TEXT("switched the rule off) | glancing contacts refused a landing %d | landing rule = %s ")
			TEXT("|| v19 §1.5: UNSEEN TURNOVERS %d (must be 0 - fired on the same frame the ball landed, ")
			TEXT("so nobody could see it touch) | SEEN turnovers %d (must be > 0) | UNGROUNDED turnovers ")
			TEXT("%d (must be 0) | grounded %d | fewest frames of contact behind any turnover %d | ")
			TEXT("shortest hold %.3fs | worst gap under any turnover %.0f uu | grounded rule = %s"),
			TurnoverReproGrazeShots + TurnoverReproDropShots,
			TurnoverReproGrazeShots, TurnoverReproDropShots, TurnoverReproSkipped,
			MidAir, Landed, Rejected,
			(!TraceModeBLegacyLandingRule())
				? TEXT("v13 (an actual landing)") : TEXT("PRE-v13 (any upward normal) - THE BUG, ARMED"),
			Unseen, Seen, Ungrounded, Grounded,
			SurfaceStats.FewestTurnoverContactFrames, SurfaceStats.ShortestTurnoverDwellSeconds,
			SurfaceStats.WorstTurnoverGapUU,
			(!TraceModeBLegacyGroundedRule())
				? TEXT("v19 (settle, then award)")
				: TEXT("PRE-v19 (award on the contact frame) - THE BUG, ARMED"));

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[ModeBTurnoverRepro] PASS: %s"), *Detail);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeBTurnoverRepro] FAIL: %s"), *Detail);
		}

		// SPEC v13 §5's live number, printed here because this is the one scripted run that reliably
		// puts the Core in the air with ten players chasing it. The rule itself is asserted by
		// Trace.ModeB.ContestTest against the same selection function; this says how often a real match
		// actually reaches the situation the note is about, which no unit check can.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBTurnoverRepro] magnet during this run (spec v13 §5): %d frames with somebody in ")
			TEXT("range (%d uncontested, %d CONTESTED, %d of those cross-team) | %d contests | largest set ")
			TEXT("%d | %d target switches | %d switches refused by the %.0f uu hysteresis"),
			CatchStats.UncontestedFrames + CatchStats.ContestedFrames,
			CatchStats.UncontestedFrames, CatchStats.ContestedFrames, CatchStats.CrossTeamContestedFrames,
			CatchStats.Contests, CatchStats.MaxContenders, CatchStats.TargetSwitches,
			CatchStats.HysteresisHolds, TraceModeBTuning::CatchContestHysteresis());
		return;
	}

	// --- Fire the next shot ------------------------------------------------------------------------
	//
	// One at a time, and only once the previous one has resolved: two Cores in the air at once is not a
	// state this game has, and a shot fired into a loose Core would be a state lock refusal counted as
	// a result.
	if (Now < TurnoverReproNextShotTime || bLoose || bCoreStateLocked)
	{
		return;
	}

	if (!IsValid(Carrier) || !Carrier->IsAlive())
	{
		return;   // Between possessions. Wait; the shot is not lost.
	}

	const ETraceTeam FromTeam = Carrier->GetTeam();
	const bool bGrazeShot = ((TurnoverReproGrazeShots + TurnoverReproDropShots) % 2) == 0;

	FVector LaunchPoint = FVector::ZeroVector;
	FVector LaunchVelocity = FVector::ZeroVector;

	if (bGrazeShot)
	{
		// THE SHOT THAT REPRODUCES THE BUG.
		//
		// A flat, fast throw that meets the top face at a few degrees - the shape a player produces
		// throwing over cover. The numbers are derived from the Core's own gravity rather than guessed,
		// so the shot still lands on the face if the weight model is retuned:
		//
		//   the Core is released HeightAboveFace above the face with NO vertical velocity, so it
		//   contacts after t = sqrt(2h/g) seconds and d = V*t uu of travel. Placing the launch point
		//   d - Extent/2 back from the probe point puts the contact half way along the face, which is
		//   the furthest possible from either edge.
		constexpr double HeightAboveFace = 40.0;
		constexpr double GrazeSpeed = 1400.0;

		const double GravityMagnitude = FMath::Abs(static_cast<double>(GetThrowGravityZ(World)));
		const double FallTime = (GravityMagnitude > 1.0)
			? FMath::Sqrt(2.0 * HeightAboveFace / GravityMagnitude) : 0.2;
		const double TravelToContact = GrazeSpeed * FallTime;

		const double Back = TravelToContact - FMath::Min(TurnoverReproTopExtent * 0.5, TravelToContact * 0.75);

		LaunchPoint = TurnoverReproTopPoint
			- TurnoverReproGrazeDirection * Back
			+ FVector(0.0, 0.0, TraceModeBTuning::CollisionRadius + HeightAboveFace);
		LaunchVelocity = TurnoverReproGrazeDirection * GrazeSpeed;
	}
	else
	{
		// THE CONTROL. A throw dropped straight onto open floor, well away from the cover, which spec
		// v6 §4.2 says must turn over. If this stops firing, the "fix" has broken the rule instead of
		// narrowing it, and the tally says so on the same line as the graze result.
		const FBox FieldBox = [World]() -> FBox
		{
			if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World))
			{
				return Arena->GetFieldBounds();
			}
			return FBox(ForceInit);
		}();

		const FVector Centre = (FieldBox.IsValid != 0) ? FieldBox.GetCenter() : FVector::ZeroVector;
		const double FloorZ = (FieldBox.IsValid != 0) ? FieldBox.Min.Z : 0.0;

		LaunchPoint = FVector(Centre.X, Centre.Y, FloorZ + 900.0);
		LaunchVelocity = FVector(0.0, 0.0, -1500.0);
	}

	// The launch point has to be in open air, or the shot tests whatever it started inside.
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreReproClear), /*bTraceComplex=*/false, this);
		Params.AddIgnoredActor(this);

		FHitResult ClearHit;
		if (World->SweepSingleByChannel(ClearHit, LaunchPoint, LaunchPoint, FQuat::Identity, ECC_WorldStatic,
			FCollisionShape::MakeSphere(TraceModeBTuning::CollisionRadius), Params))
		{
			++TurnoverReproSkipped;
			--TurnoverReproShotsLeft;
			TurnoverReproNextShotTime = Now + 0.5f;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ModeBTurnoverRepro] shot SKIPPED: the launch point %s is inside %s."),
				*LaunchPoint.ToCompactString(), *GetNameSafe(ClearHit.GetActor()));
			return;
		}
	}

	if (!DebugLaunchLoose(LaunchPoint, LaunchVelocity, FromTeam, /*bAsThrow=*/true))
	{
		++TurnoverReproSkipped;
		--TurnoverReproShotsLeft;
		TurnoverReproNextShotTime = Now + 0.5f;
		UE_LOG(LogTraceGame, Warning, TEXT("[ModeBTurnoverRepro] shot SKIPPED: the launch was refused."));
		return;
	}

	(bGrazeShot ? TurnoverReproGrazeShots : TurnoverReproDropShots)++;
	--TurnoverReproShotsLeft;
	TurnoverReproNextShotTime = Now + 2.5f;

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeBTurnoverRepro] shot %d/%d: a %s throw %s from %s at %.0f uu/s (%d left)"),
		TurnoverReproGrazeShots + TurnoverReproDropShots,
		TurnoverReproGrazeShots + TurnoverReproDropShots + TurnoverReproShotsLeft + TurnoverReproSkipped,
		*TraceTeamName(FromTeam).ToString(),
		bGrazeShot ? TEXT("GRAZING the top of cover") : TEXT("DROPPED onto open floor"),
		*LaunchPoint.ToCompactString(), LaunchVelocity.Size(), TurnoverReproShotsLeft);
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

	bAppliedTurnoverActive = IsTurnoverActive();

	// The beacon is placed in the actor's own space, once, from its two end heights.
	if (Beacon != nullptr && Beacon->GetStaticMesh() != nullptr)
	{
		const FBoxSphereBounds Bounds = Beacon->GetStaticMesh()->GetBounds();
		const double MeshHeight = FMath::Max(1.0, 2.0 * Bounds.BoxExtent.Z);
		const double MeshWidth = FMath::Max(1.0, 2.0 * Bounds.BoxExtent.X);

		const double Height = TraceCoreTuning::BeaconTop - TraceCoreTuning::BeaconBottom;
		const double Centre = (TraceCoreTuning::BeaconTop + TraceCoreTuning::BeaconBottom) * 0.5
			- TraceCoreTuning::OrbHeight;   // Relative to the actor, which sits at OrbHeight.

		// SPEC v25 §2/§3: "the beam ... should change colors to the opposite team and be LARGER for the
		// 5 seconds". A MULTIPLIER of the normal width, never a width of its own — Demo 21's standing
		// rule, because "larger" is a claim about the beam beside it and an absolute would stop being
		// larger the day TraceCoreTuning::BeaconWidth moved. The colour half is in UpdateVisuals().
		const double Width = TraceCoreTuning::BeaconWidth
			* (bAppliedTurnoverActive ? static_cast<double>(GetTurnoverBeamScale()) : 1.0);

		Beacon->SetRelativeLocation(FVector(0.0, 0.0, Centre));
		Beacon->SetRelativeScale3D(FVector(
			Width / MeshWidth,
			Width / MeshWidth,
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
	// SPEC v25 §2/§3. "When a turnover happens ... the beam of light coming from the core should
	// change colors to the OPPOSITE TEAM." A turned-over Core has no holder, so GetHolderTeam() is
	// None and the beam would otherwise be the neutral grey a kickoff uses. For the length of the
	// window it becomes the colour of the side that may now take it, which is the read the note asks
	// for: a player sees, from across the field, whose Core it currently is.
	const ETraceTeam BeamTeam = IsTurnoverActive() ? GetTurnoverPullingTeam() : GetHolderTeam();

	FLinearColor Color = TraceTeamColor(BeamTeam);
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

	// SPEC v26 §9 — "CorePickup ... you pick up the Core", CLIENT SIDE: only the player who now holds
	// it hears it, and no RPC is sent for it.
	//
	// THIS IS THE ONE CALL SITE AND IT COVERS EVERY TOPOLOGY, which is why it is here and not in
	// GrantTo: GrantTo ends by calling OnRep_Carrier() explicitly (so a listen-server host and a
	// standalone session reach it), and replication calls it on every remote client. Putting it in
	// both would give the host the sound twice.
	//
	// TraceAudio::Play's own gate does the rest: it plays only when Carrier is the pawn of a PLAYER on
	// THIS machine, so a bot picking the Core up is silent, a teammate picking it up is silent for you,
	// and a null Carrier (the kickoff path, which also calls this) is silent for everybody.
	TraceAudio::Play(Carrier, TraceSoundEvents::CorePickup);

	// Server truth about who holds it supersedes any local pass prediction.
	bLocalPassPredicted = false;
	LocalPassPredictTarget = nullptr;

	// SPEC v10 §10. THE FRAME POSSESSION ENDS IS THE FRAME THE CORE MOVES, on every machine.
	//
	// This is the fast path for the stranding bug: without it, a client that learns "Carrier is now
	// nobody" detaches with KeepWorldTransform and leaves the Core standing in the endzone until
	// Tick's reconciliation gets to it. That is only a frame later now, but a reset is exactly the
	// moment a frame of the Core in the wrong place is most visible, and a caller reading OnRep_Carrier
	// should see the whole answer here rather than half of it.
	PlaceHolderlessCore();
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

	// SPEC v10 §10: a Core that has just STOPPED being loose (a catch, a turnover, a reset) is
	// holderless until the grant lands, and the client's last LooseLocation is stale by then. Same
	// rule, same tolerance, same function the server uses.
	if (!bLoose)
	{
		PlaceHolderlessCore();
	}
}

void ATraceCore::OnRep_Turnover()
{
	// SPEC v25 §2/§3. The beam is how a player twenty thousand units away learns a turnover happened,
	// so it changes on the frame the news arrives rather than on the next reconciliation tick.
	ApplyAttachment();
	UpdateVisuals();

	if (TurnoverLockoutTeam != ETraceTeam::None)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeB] spec v25 §2 (client): turnover window open - %s is locked out, %s may pull."),
			*TraceTeamName(TurnoverLockoutTeam).ToString(),
			*TraceTeamName(GetTurnoverPullingTeam()).ToString());
	}
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


// =================================================================================================
// SPEC v10 §10 — the teleport funnel, the client-side placement rule, and the audit
//
// Read the block at the top of this file first: the spec's "clients interpolate replicated movement"
// lead is not what is happening, because this actor has movement replication switched off. What was
// happening is that clients were never told about a reset at all.
// =================================================================================================

void ATraceCore::ServerTeleport(const FVector& Where, const TCHAR* Why)
{
	if (!HasAuthority())
	{
		return;
	}

	// The actual move, unchanged in kind from what every reset path did before: TeleportPhysics,
	// no sweep. What is new is everything after it.
	SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);

	if (CVarCoreTeleportFix.GetValueOnGameThread() == 0)
	{
		// A/B, the red arm: move it here and tell nobody, which is what every reset path did before
		// spec v10 §10. Every caller still does its own ForceNetUpdate for the possession properties,
		// so this is the old code exactly and not a weakened version of it.
		return;
	}

	TeleportLocation = Where;

	// ++ ON EVERY CALL, INCLUDING A TELEPORT TO THE SAME PLACE. The serial, not the location, is what
	// the clients react to: two kickoffs in a row both land on the centre pedestal, TeleportLocation
	// is byte-identical between them, and a location-only channel would silently drop the second one.
	// A client whose own copy had drifted (a dead-reckoned loose Core, say) would keep the drift.
	++TeleportSerial;

	// The whole point is that this arrives promptly. Without it the destination waits out the actor's
	// ordinary net update cadence, which on a reset is exactly the window the Core is visibly wrong in.
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Verbose, TEXT("[Core] teleport #%u (%s) -> %s"),
		static_cast<uint32>(TeleportSerial), Why != nullptr ? Why : TEXT("unspecified"),
		*Where.ToCompactString());
}

void ATraceCore::PlaceHolderlessCore()
{
	// Somebody is holding it: the ATTACHMENT owns the transform and this must not fight it. This is
	// the same test ApplyAttachment() branches on, deliberately, so the two can never disagree about
	// which of them is placing the actor.
	if (IsValid(Carrier))
	{
		return;
	}

	// Mode B, in the air or lying on the floor: LooseLocation is the answer, and on a client that is
	// the dead-reckoned value the Tick branch above already wrote. Asking for it here as well keeps
	// this function total — "where does a holderless Core belong" has one answer in each mode.
	const FVector Where = bLoose ? FVector(LooseLocation) : GetHomeLocation();

	if (FVector::DistSquared(GetActorLocation(), Where) <= TraceCoreTuning::HomeToleranceSq)
	{
		return;
	}

	if (HasAuthority())
	{
		// A discontinuity the clients have to be told about — see ServerTeleport.
		ServerTeleport(Where, bLoose ? TEXT("loose placement") : TEXT("park at home"));
	}
	else if (CVarCoreTeleportFix.GetValueOnGameThread() != 0)
	{
		// A CLIENT DOES NOT INVENT A TELEPORT EVENT; it applies the rule it shares with the server to
		// state the server has already sent it. If the server disagrees, its next TeleportSerial wins.
		SetActorLocation(Where, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void ATraceCore::OnRep_TeleportSerial()
{
	// Every property in this bunch has already been applied — Carrier, bLoose, LooseLocation and
	// TeleportLocation included — because the engine runs a bunch's RepNotifies only after all of its
	// property data has been received. That guarantee is why this can be a plain OnRep and does not
	// need a struct or a sequencing dance with the possession properties.

	// Get off any parent the previous holder still has us on. AttachmentReplication is an AActor
	// property with its own OnRep and there is no defined ordering between it and this one, so a
	// SetActorLocation while still attached would merely rewrite the relative offset and the old
	// holder would drag the Core around from the new position. ApplyAttachment reads Carrier and
	// bLoose, both of which are already current.
	ApplyAttachment();

	if (IsValid(Carrier))
	{
		// A teleport while somebody is holding it is not a state ServerTeleport ever produces (every
		// call site is holderless or loose). If one ever appears, the attachment above is the right
		// answer and overwriting it with a world location would be the bug.
		return;
	}

	if (bLoose)
	{
		// Restart the dead reckoner from the server's point rather than from the client's own
		// integrated copy. Velocity is deliberately NOT touched: on a throw it arrived in this very
		// bunch and is the launch velocity, and zeroing it here would stop the throw dead.
		LooseLocation = TeleportLocation;
	}

	SetActorLocation(TeleportLocation, false, nullptr, ETeleportType::TeleportPhysics);
	UpdateVisuals();
}


// --- The reproduction ------------------------------------------------------------------------------

bool ATraceCore::RequestGoalRepro()
{
	if (!HasAuthority())
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[CoreAudit] Trace.Core.GoalRepro is a SERVER command - run it on the listen server, not the client."));
		return false;
	}

	if (bGoalReproArmed)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[CoreAudit] GoalRepro already staged; ignoring."));
		return false;
	}

	// A goal needs a carrier. If nobody has it (a kickoff window), take the first living player and
	// give it to them through the ordinary funnel.
	if (!IsValid(Carrier))
	{
		TArray<ATraceCharacter*> Characters;
		GatherCharacters(Characters);

		ATraceCharacter* Candidate = nullptr;
		for (ATraceCharacter* Character : Characters)
		{
			if (IsValid(Character) && Character->IsAlive())
			{
				Candidate = Character;
				break;
			}
		}

		if (Candidate == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[CoreAudit] GoalRepro: nobody alive to carry the Core."));
			return false;
		}

		bOutOfPlay = false;
		GrantTo(Candidate, ETraceCoreGrantReason::Debug);
	}

	if (!IsValid(Carrier))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[CoreAudit] GoalRepro: the grant did not take; aborting."));
		return false;
	}

	// STAGE ONE: put the holder as far from the centre pedestal as the field allows, which is what
	// makes this the longest reset in the game and the one the user reported. The carrier keeps their
	// Y and Z, so they stay on the floor and inside the arena.
	const UWorld* World = GetWorld();
	const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
	const FBox Field = (Arena != nullptr) ? Arena->GetFieldBounds() : FBox(ForceInit);

	const FVector CarrierNow = Carrier->GetActorLocation();
	FVector Staged = CarrierNow;
	if (Field.IsValid != 0)
	{
		// The end furthest from home, but 4000 uu inside the wall — SHORT OF THE ENDZONE ON PURPOSE.
		// ATraceArenaBuilder::EndzoneDepth is 2400 uu, so parking the carrier any deeper would trip a
		// REAL score the instant they arrived, and the reset would fire before the client had seen the
		// Core out there at all. Staging short means the fire below is the only reset in the window,
		// and it is the same KickoffTo a real goal produces.
		const FVector Home = GetHomeLocation();
		const double FarX = (FMath::Abs(Field.Max.X - Home.X) >= FMath::Abs(Home.X - Field.Min.X))
			? (Field.Max.X - 4000.0) : (Field.Min.X + 4000.0);
		Staged.X = FarX;
	}

	Carrier->SetActorLocation(Staged, false, nullptr, ETeleportType::TeleportPhysics);
	Carrier->ForceNetUpdate();

	GoalReproTeam = TraceOpposingTeam(Carrier->GetTeam());
	// Long enough that a 40 ms client has certainly rendered the Core out there before the reset -
	// the whole question is what that client does NEXT, and staging it too fast would let the two
	// events arrive in one bunch and hide the bug.
	GoalReproFireTime = GetServerTimeSeconds() + 1.5f;
	bGoalReproArmed = true;

	UE_LOG(LogTraceGame, Display,
		TEXT("[CoreAudit] GoalRepro staged: %s carried the Core to %s (%.0f uu from home). ")
		TEXT("Firing the post-goal KickoffTo(%s) in 1.5s."),
		*GetNameSafe(Carrier), *Staged.ToCompactString(),
		FVector::Dist(Staged, GetHomeLocation()), *TraceTeamName(GoalReproTeam).ToString());

	return true;
}

void ATraceCore::TickGoalRepro()
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// --- Auto-arming. SAME GATE AS THE SPEC v8 §0 MOMENTUM TEST, for the same reason. --------------
	//
	// The delay is a FLOOR and not the release condition; what actually releases a run is a REMOTE
	// CLIENT'S PAWN EXISTING. §10 is a client-side bug, so a reset fired while the second process is
	// still loading plugins measures the host and nothing else - which is precisely the failure spec
	// v8 §0 exists to stop, and it has bitten this file once already.
	const int32 RequestedRuns = CVarCoreGoalReproRuns.GetValueOnGameThread();
	if (RequestedRuns > 0 && GoalReproRunsDone < RequestedRuns && !bGoalReproArmed)
	{
		const float NowReal = static_cast<float>(World->GetTimeSeconds());
		const float Due = (GoalReproRunsDone == 0)
			? CVarCoreGoalReproDelay.GetValueOnGameThread()
			: GoalReproNextAutoTime;

		if (NowReal >= Due && HasRemoteClientPawn())
		{
			// Counted only on a run that actually STAGED. A failed attempt (nobody alive yet) must not
			// silently consume one of the runs the operator asked for.
			if (RequestGoalRepro())
			{
				++GoalReproRunsDone;
				GoalReproNextAutoTime = NowReal + FMath::Max(4.f, CVarCoreGoalReproInterval.GetValueOnGameThread());
				UE_LOG(LogTraceGame, Display, TEXT("[CoreAudit] GoalRepro run %d of %d staged."),
					GoalReproRunsDone, RequestedRuns);
			}
		}
	}

	if (!bGoalReproArmed || GetServerTimeSeconds() < GoalReproFireTime)
	{
		return;
	}

	bGoalReproArmed = false;

	UE_LOG(LogTraceGame, Display,
		TEXT("[CoreAudit] GoalRepro FIRING: Core at %s, %.0f uu from home. This is the exact call ")
		TEXT("ATraceGameMode::NotifyScored makes on a goal."),
		*GetActorLocation().ToCompactString(), FVector::Dist(GetActorLocation(), GetHomeLocation()));

	// STAGE TWO. Not a simulation of a goal's reset - it IS a goal's reset. NotifyScored's only
	// effect on this actor is this call.
	KickoffTo(GoalReproTeam);
}


// --- The audit -------------------------------------------------------------------------------------

void ATraceCore::TickTeleportAudit()
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (CVarCoreTeleportAudit.GetValueOnGameThread() == 0)
	{
		bAuditHasLast = false;
		bAuditWindowOpen = false;
		return;
	}

	// HOST / SERVER / CLIENT, printed on every line, because the entire point of §10 is that the host
	// and the client disagree and only one of them has the bug (spec v8 §0).
	const TCHAR* Machine = HasAuthority()
		? (World->GetNetMode() == NM_ListenServer ? TEXT("HOST") : TEXT("SERVER"))
		: TEXT("CLIENT");

	const FVector Now = GetActorLocation();
	const FVector Home = GetHomeLocation();
	const double HomeError = FVector::Dist(Now, Home);
	const bool bHeld = IsValid(Carrier);
	const float DeltaSeconds = World->GetDeltaSeconds();

	const double Step = bAuditHasLast ? FVector::Dist(Now, AuditLastLocation) : 0.0;

	// A single frame that moves the Core further than a player could be expected to. Logged wherever
	// it happens, in or out of a window: this is the line that says "it crossed the map in one frame".
	if (bAuditHasLast && Step > static_cast<double>(CVarCoreTeleportAuditJump.GetValueOnGameThread()))
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[CoreAudit] %s JUMP %.0f uu in one frame (%.3f s): %s -> %s | holder %s, loose %d, serial %u"),
			Machine, Step, DeltaSeconds,
			*AuditLastLocation.ToCompactString(), *Now.ToCompactString(),
			bHeld ? *GetNameSafe(Carrier) : TEXT("none"), bLoose ? 1 : 0,
			static_cast<uint32>(TeleportSerial));
	}

	// A possession that has just ENDED is the event §10 is about. Open a window and watch.
	if (bAuditHasLast && bAuditWasHeld && !bHeld && !bAuditWindowOpen)
	{
		bAuditWindowOpen = true;
		AuditWindowEndTime = static_cast<float>(World->GetTimeSeconds())
			+ FMath::Max(0.5f, CVarCoreTeleportAuditWindow.GetValueOnGameThread());
		AuditPathLength = 0.0;
		AuditMaxStep = 0.0;
		AuditWorstHomeError = 0.0;
		AuditAwayFromHomeSeconds = 0.f;
		AuditMovingFrames = 0;
		AuditFrames = 0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[CoreAudit] %s possession ended with the Core at %s (%.0f uu from home). Watching for %.1fs."),
			Machine, *Now.ToCompactString(), HomeError,
			FMath::Max(0.5f, CVarCoreTeleportAuditWindow.GetValueOnGameThread()));
	}

	if (bAuditWindowOpen)
	{
		++AuditFrames;
		AuditPathLength += Step;
		AuditMaxStep = FMath::Max(AuditMaxStep, Step);

		// 1 uu of movement in a frame is the difference between "it moved" and float noise. What this
		// counts is how MANY frames the travel was spread over, which is the whole SLIDE-versus-JUMP
		// question the spec's lead and the measured cause disagree about.
		if (Step > 1.0)
		{
			++AuditMovingFrames;
		}

		// Only meaningful while nobody is holding it: a Core riding a live holder is legitimately
		// nowhere near home.
		if (!bHeld && !bLoose)
		{
			AuditWorstHomeError = FMath::Max(AuditWorstHomeError, HomeError);
			if (HomeError > FMath::Sqrt(static_cast<double>(TraceCoreTuning::HomeToleranceSq)))
			{
				AuditAwayFromHomeSeconds += DeltaSeconds;
			}
		}

		if (static_cast<float>(World->GetTimeSeconds()) >= AuditWindowEndTime)
		{
			bAuditWindowOpen = false;

			// THE VERDICT, and the three cases it has to be able to tell apart:
			//   SNAP     one frame of travel, and the Core was never left far from where it belongs.
			//   SLIDE    the travel was spread over many frames - interpolation, the spec's lead.
			//   STRANDED it sat far from home for a measurable time and then crossed in one frame.
			const double HomeTolerance = FMath::Sqrt(static_cast<double>(TraceCoreTuning::HomeToleranceSq));
			const TCHAR* Verdict = TEXT("SNAP (clean)");
			if (AuditMovingFrames > 2 && AuditPathLength > 4.0 * AuditMaxStep)
			{
				Verdict = TEXT("SLIDE - travelled over many frames (interpolation)");
			}
			else if (AuditAwayFromHomeSeconds > 0.15f)
			{
				Verdict = TEXT("STRANDED then JUMPED - the reset was never applied here");
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[CoreAudit] %s reset summary: %d frames, %d moving, path %.0f uu, max step %.0f uu, ")
				TEXT("worst dist from home %.0f uu, held wrong place for %.2f s (tolerance %.0f uu) => %s"),
				Machine, AuditFrames, AuditMovingFrames, AuditPathLength, AuditMaxStep,
				AuditWorstHomeError, AuditAwayFromHomeSeconds, HomeTolerance, Verdict);
		}
	}

	AuditLastLocation = Now;
	bAuditHasLast = true;
	bAuditWasHeld = bHeld;
}


// =================================================================================================
// SPEC v25 §2 — THE RED ARM
//
// Four negative claims, and a negative claim is worthless until the positive one beside it has been
// watched to fire on the same pawns, in the same window, through the same functions:
//
//   1. A SAME-TEAM PLAYER MUST FAIL TO PULL      (green: the opposing player, standing there, passes)
//   2. A PLAYER WITH NO LINE OF SIGHT MUST FAIL  (green: the same player, same frame, unblocked)
//   3. RELEASING AT 0.29 s MUST FAIL             (green: the same player holding past 0.30 s wins it)
//   4. THE LOCKOUT MUST EXPIRE                   (red:   the locked-out player standing ON the Core
//                                                        is refused for the whole window)
//
// It drives the SHIPPING functions — CanPullNow, RequestPullInput, ServerTickTurnover, the pickup
// poll — and the only thing it fakes is the throw that would otherwise have to be arranged on cue.
// Armed from -ExecCmds as well as the console, because this project's testing policy forbids typing
// into a window.
//
// It needs BOTS to drive: a human's crosshair is theirs and this harness will not move it. On a
// bot-free session every step reports SKIP rather than passing on an empty room.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarModeBTurnoverVerify(
	TEXT("Trace.ModeB.TurnoverVerify"),
	0,
	TEXT("SPEC v25 §2. 1: run the turnover red arm once - same-team pull refused, no-line-of-sight ")
	TEXT("pull refused, a 0.29s release refused, a full hold completing and delivering at the thrown ")
	TEXT("speed, and the 5s lockout refusing then releasing the team that dropped it."),
	ECVF_Default);

namespace TraceCoreTurnoverVerify
{
	/** How long any one step may take before it is declared inconclusive rather than hanging the run. */
	constexpr float StepTimeoutSeconds = 6.0f;

	/** Times step 5 will re-arm if somebody legally takes the Core out from under it. */
	constexpr int32 LockoutRetries = 2;

	/**
	 * A bot on @p Team the harness may drive.
	 *
	 * BOTS ONLY, and deliberately: driving a human's control rotation from a test would be moving the
	 * player's own mouse, and the hover rule under test is precisely "where is this player looking".
	 */
	ATraceCharacter* FindDrivableBot(const TArray<ATraceCharacter*>& All, ETraceTeam Team)
	{
		for (ATraceCharacter* Candidate : All)
		{
			if (!IsValid(Candidate) || !Candidate->IsAlive() || Candidate->GetTeam() != Team)
			{
				continue;
			}

			const AController* Controller = Candidate->GetController();
			if (Controller == nullptr || Controller->IsPlayerController())
			{
				continue;
			}

			return Candidate;
		}

		return nullptr;
	}

	/**
	 * A place to park the Core that @p Puller can actually SEE.
	 *
	 * THIS IS NOT CONVENIENCE, IT IS WHAT MAKES THE GREEN ARMS MEAN ANYTHING. The first version of
	 * this harness dropped the Core at the locked-out bot's feet, wherever that bot happened to be on
	 * a 33600 uu pitch full of cover, and the green arm of steps 1 and 2 then failed with "no line of
	 * sight" — reporting a broken rule when what was broken was the test's own geometry. A red arm
	 * whose green half cannot fire proves nothing, so the point is CHOSEN: eight compass directions
	 * at arm's length, dropped onto whatever is underneath, and the first one the puller has a clear
	 * ray to wins.
	 *
	 * @return false when the puller can see none of the eight, which is reported as a SKIP.
	 */
	bool FindPullablePoint(const ATraceCharacter* Puller, double OrbRadius, FVector& OutPoint)
	{
		const UWorld* World = (Puller != nullptr) ? Puller->GetWorld() : nullptr;
		if (World == nullptr)
		{
			return false;
		}

		const FVector Eye = Puller->GetPawnViewLocation();
		const FVector Origin = Puller->GetActorLocation();
		// FAR ENOUGH THAT THE PULLER CANNOT SIMPLY WALK TO IT. At 600 uu a bot chasing the loose Core
		// arrived on foot inside the 0.3 s hold and step 4 measured the pickup radius instead of the
		// pull; at 1400 uu it needs ~2 s to cover ground the delivery crosses in 0.6.
		const double Reach = 1400.0;

		for (int32 Step = 0; Step < 8; ++Step)
		{
			const double Angle = static_cast<double>(Step) * (PI / 4.0);
			const FVector Offset(FMath::Cos(Angle) * Reach, FMath::Sin(Angle) * Reach, 0.0);

			FCollisionQueryParams DropParams(SCENE_QUERY_STAT(TraceCoreVerifyDrop), /*bTraceComplex=*/false);
			DropParams.AddIgnoredActor(Puller);

			FHitResult Ground;
			FVector Candidate = Origin + Offset;

			if (World->LineTraceSingleByChannel(Ground, Candidate + FVector(0.0, 0.0, 300.0),
				Candidate - FVector(0.0, 0.0, 2000.0), ECC_WorldStatic, DropParams))
			{
				Candidate = Ground.ImpactPoint + FVector(0.0, 0.0, OrbRadius + 2.0);
			}
			else
			{
				continue;   // Nothing underneath: a hole, or off the edge of the field.
			}

			FCollisionQueryParams LosParams(SCENE_QUERY_STAT(TraceCoreVerifyLos), /*bTraceComplex=*/false);
			LosParams.AddIgnoredActor(Puller);

			if (!World->LineTraceTestByChannel(Eye, Candidate, ECC_Visibility, LosParams))
			{
				OutPoint = Candidate;
				return true;
			}
		}

		return false;
	}
}

bool ATraceCore::DebugRegisterTurnover(ETraceTeam DroppingTeam, const FVector& Where)
{
	if (!HasAuthority() || !IsModeB() || DroppingTeam == ETraceTeam::None)
	{
		return false;
	}

	// Loose, as if DroppingTeam had thrown it, with no launch velocity: this harness is about the
	// WINDOW, and a flight would only add a landing the shipping rule has already been proven on.
	if (!DebugLaunchLoose(Where, FVector::ZeroVector, DroppingTeam, /*bAsThrow=*/true))
	{
		return false;
	}

	RegisterTurnover(DroppingTeam, Where, TEXT("armed by Trace.ModeB.TurnoverVerify"));
	return IsTurnoverActive();
}

#if !UE_BUILD_SHIPPING
bool ATraceCore::DebugStageTurnoverAtLocalCrosshair(UWorld* World, float DistanceUU, bool bLockLocalTeam,
	FString& OutReport)
{
	if (World == nullptr)
	{
		OutReport = TEXT("no world");
		return false;
	}

	ATraceCore* Core = ATraceCore::Get(World);
	if (Core == nullptr)
	{
		OutReport = TEXT("no Core in this world");
		return false;
	}
	if (!Core->HasAuthority())
	{
		OutReport = TEXT("this machine is not the server; stage the turnover on the listen host");
		return false;
	}
	if (!Core->IsModeB())
	{
		OutReport = TEXT("this match is not in goals mode; spec v25 §2 is goals mode only");
		return false;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	ATraceCharacter* Local = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	if (!IsValid(Local) || Local->GetTeam() == ETraceTeam::None)
	{
		OutReport = TEXT("no local pawn with a team");
		return false;
	}

	// THE AIM IS NOT TOUCHED HERE, BY ANYTHING. The Core goes on the FLOOR in front of the player,
	// which is where a thrown Core comes to rest; whether the player's crosshair happens to be on it
	// is then a real question for the shipping CanPullNow, and this fixture has recorded it answering
	// "not hovering the Core" more than once. (The demo below aims at the Core afterwards, in its own
	// clearly-logged step, which is the one thing a mouse does.)
	//
	// GROUND, NOT A VIEW RAY. Two earlier versions of this got the geometry wrong in opposite
	// directions and both were caught by the shipping rules rather than by me:
	//   * offset-then-drop put the Core metres below the ray, outside the 4-degree aim cone;
	//   * the view ray itself hit scenery the CORE's own downward probe does not see, so the Core was
	//     left "134 uu clear" of anything, RESUMED FLIGHT, fell, re-landed, fired a second genuine
	//     turnover that restarted the window, and cancelled a pull that had reached 0.777 of its hold.
	// A point on the floor is the only one that is both real and still there a second later.
	const FVector Eye = Local->GetPawnViewLocation();
	const FRotator ViewRot = PC->GetControlRotation();
	const FVector Forward = FRotator(0.f, ViewRot.Yaw, 0.f).Vector();   // Yaw only: along the ground.
	const float Reach = (DistanceUU > 1.f) ? DistanceUU : 450.f;

	FCollisionQueryParams DropParams(SCENE_QUERY_STAT(TraceCoreIntegStageDrop), /*bTraceComplex=*/false);
	DropParams.AddIgnoredActor(Local);
	DropParams.AddIgnoredActor(Core);

	FVector Where;

	if (DistanceUU <= 0.f)
	{
		// AT MY FEET, deliberately: the arm where this player's own team is locked out wants the pickup
		// poll offered to them on every tick, so the refusal that follows is a measured refusal rather
		// than an absence of opportunity.
		Where = Local->GetActorLocation();

		FHitResult Floor;
		if (World->LineTraceSingleByChannel(Floor, Where, Where - FVector(0.0, 0.0, 500.0),
			ECC_WorldStatic, DropParams))
		{
			Where = Floor.ImpactPoint + FVector(0.0, 0.0, TraceModeBVisibleOrbRadius + 2.0);
		}
	}
	else
	{
		// Walk out along the ground, not along the view, and take the floor under that point. If the
		// player is facing a wall the sweep shortens until it finds open floor, so the Core never ends
		// up inside geometry the pull would then have no line of sight to.
		FVector Base = FVector(Local->GetActorLocation().X, Local->GetActorLocation().Y, Eye.Z) + Forward * Reach;

		FHitResult Blocked;
		if (World->LineTraceSingleByChannel(Blocked, Eye, Base, ECC_WorldStatic, DropParams))
		{
			Base = Blocked.ImpactPoint - Forward * (TraceModeBVisibleOrbRadius * 3.0);
		}

		FHitResult Floor;
		if (World->LineTraceSingleByChannel(Floor, Base + FVector(0.0, 0.0, 100.0),
			Base - FVector(0.0, 0.0, 4000.0), ECC_WorldStatic, DropParams))
		{
			Where = Floor.ImpactPoint + Floor.ImpactNormal * (TraceModeBVisibleOrbRadius + 2.0);
		}
		else
		{
			Where = Local->GetActorLocation() + Forward * Reach;
		}
	}

	const ETraceTeam LocalTeam = Local->GetTeam();
	const ETraceTeam DroppingTeam = bLockLocalTeam ? LocalTeam : TraceOpposingTeam(LocalTeam);

	if (!Core->DebugRegisterTurnover(DroppingTeam, Where))
	{
		OutReport = TEXT("DebugRegisterTurnover refused (already held, or not loose-able right now)");
		return false;
	}

	const TCHAR* Reason = nullptr;
	const bool bLocalMayPull = Core->CanPullNow(Local, &Reason);

	OutReport = FString::Printf(
		TEXT("staged at %s, %.0f uu down the local player's own view. %s dropped it and is locked out ")
		TEXT("for %.2fs; %s may pull. The local pawn (%s, %s) %s — CanPullNow says \"%s\"."),
		*Where.ToCompactString(), Reach,
		*TraceTeamName(DroppingTeam).ToString(), Core->GetTurnoverSecondsRemaining(),
		*TraceTeamName(TraceOpposingTeam(DroppingTeam)).ToString(),
		*GetNameSafe(Local), *TraceTeamName(LocalTeam).ToString(),
		bLocalMayPull ? TEXT("MAY PULL") : TEXT("may NOT pull"),
		(Reason != nullptr) ? Reason : TEXT("legal"));

	return true;
}

namespace TraceCoreIntegStage
{
	/**
	 * `Trace.Integ.StageTurnover [DistanceUU] [1 = lock MY team out]`
	 *
	 * The integrator's camera rig for spec v25 §2/§3. Default (0) puts the turnover on the OTHER team,
	 * so this machine's player is the one who may pull and the ring is on screen to be photographed;
	 * `1` locks THIS player's team out, which is the arm where the ring must NOT appear and standing
	 * on the Core must do nothing for five seconds.
	 */
	static void Cmd(const TArray<FString>& Args, UWorld* World)
	{
		const float Distance = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 450.f;
		const bool bLockLocal = (Args.Num() > 1) && (FCString::Atoi(*Args[1]) != 0);

		FString Report;
		const bool bOk = ATraceCore::DebugStageTurnoverAtLocalCrosshair(World, Distance, bLockLocal, Report);

		if (bOk)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[v25Integ] StageTurnover: %s"), *Report);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] StageTurnover REFUSED: %s"), *Report);
		}
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdReg(
		TEXT("Trace.Integ.StageTurnover"),
		TEXT("SPEC v25 INTEGRATION. Stage a turnover on the ground the local player is already looking ")
		TEXT("at, so the window can be photographed. Args: [DistanceUU=450] [LockMyTeam=0]. It moves ")
		TEXT("the CORE, never the crosshair, and goes through the shipping RegisterTurnover()."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Cmd));

	// ---------------------------------------------------------------------------------------------
	// `Trace.Integ.TurnoverDemo` — THE WHOLE OF SPEC v25 §2 AS ONE SCRIPTED, PHOTOGRAPHED SEQUENCE.
	//
	// The §2 red arm proves the RULES on bots; this proves the PLAYER'S EXPERIENCE of them, on the
	// human-controlled pawn, in one run, with a screenshot at each claim:
	//
	//   A  the opposing team drops it -> the Core STAYS where it landed, beam recoloured and larger
	//   B  a ring appears around it for this player, empty
	//   C  holding the pull FILLS the ring, off the server's own number
	//   D  past 0.3 s the Core travels to this player and they hold it
	//   E  this player's OWN team drops it -> no ring, and standing on it does nothing
	//   F  five seconds later the same touch, through the same poll, hands it over
	//
	// Every verb is the shipping one. Nothing here writes a pull progress, a lockout, or a pickup.
	// ---------------------------------------------------------------------------------------------
	static void Shot(const TCHAR* Which)
	{
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"),
			FString::Printf(TEXT("v25integ_%s_%s.png"), Which,
				*FDateTime::Now().ToString(TEXT("%H%M%S"))));

		// bShowUI = true: the beam is world geometry but the RING is Canvas HUD, and a UI-less capture
		// photographs an arena with no evidence in it.
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[v25Integ] shot %s -> %s"), Which, *Path);
	}

	static void Demo(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] TurnoverDemo: no world."));
			return;
		}

		const float Distance = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 450.f;

		TWeakObjectPtr<UWorld> WeakWorld(World);
		// Every phase boundary is a GAME-TIME deadline, not a wall-clock one, because this machine has
		// run these captures at 0.1 fps under contention and a wall clock would step past the 0.30 s
		// hold between two frames without ever drawing the filling ring it is here to photograph.
		TSharedRef<int32> Phase = MakeShared<int32>(0);
		TSharedRef<double> PhaseStart = MakeShared<double>(World->GetTimeSeconds());
		TSharedRef<bool> ShotFilling = MakeShared<bool>(false);
		TSharedRef<double> LockoutOpened = MakeShared<double>(0.0);
		TSharedRef<int32> BeamRetries = MakeShared<int32>(0);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakWorld, Distance, Phase, PhaseStart, ShotFilling, LockoutOpened, BeamRetries](float) -> bool
			{
				UWorld* Live = WeakWorld.Get();
				if (Live == nullptr)
				{
					return false;
				}

				ATraceCore* Core = ATraceCore::Get(Live);
				APlayerController* PC = Live->GetFirstPlayerController();
				ATraceCharacter* Me = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
				if (Core == nullptr || !IsValid(Me))
				{
					// Between pawns (the select screen, a respawn). WAIT, and hold the settle clock at
					// zero so the first phase's delay is measured from the first frame this player
					// actually exists — armed from -TraceExec, this ticker starts before they do.
					*PhaseStart = Live->GetTimeSeconds();
					return true;
				}

				const double Now = Live->GetTimeSeconds();
				const double InPhase = Now - *PhaseStart;
				const auto Advance = [&Phase, &PhaseStart, Now](int32 Next)
				{
					*Phase = Next;
					*PhaseStart = Now;
				};

				const TCHAR* Reason = nullptr;
				const float Progress = Core->GetPullProgressFor(Me);
				const bool bMayPull = Core->CanPullNow(Me, &Reason);

				switch (*Phase)
				{
				case 0:
				{
					// Three seconds of live pawn before anything is staged: a spawn still has the camera
					// settling, and a turnover placed down a view that is mid-blend is placed nowhere.
					if (InPhase < 3.0)
					{
						break;
					}

					FString Report;
					if (!ATraceCore::DebugStageTurnoverAtLocalCrosshair(Live, Distance, /*bLockLocalTeam=*/false, Report))
					{
						UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] A REFUSED: %s"), *Report);
						return false;
					}
					UE_LOG(LogTraceGame, Display,
						TEXT("[v25Integ] A — THE OPPOSING TEAM DROPPED IT. %s Beam scale x%.2f for the window."),
						*Report, ATraceCore::GetTurnoverBeamScale());
					Shot(TEXT("A_turnover_beam"));
					Advance(1);
					break;
				}

				case 1:
					// One frame of settle, then photograph the EMPTY ring before any button is pressed:
					// a ring that is only ever seen full proves nothing about where its number came from.
					// 0.8 s, not one frame: a staged Core still has to touch down and be judged by the
					// shipping landing rule, and photographing the ring before that is over photographs
					// a state the player never sees.
					if (InPhase >= 0.8)
					{
						// *** THE RED HALF, TAKEN BEFORE THE GREEN ONE AND FROM THE SAME WINDOW. ***
						// The Core is on the floor and NOTHING has touched this player's aim, so their
						// crosshair is wherever they left it. Whatever CanPullNow answers here, it
						// answers on its own.
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] B(red) — turnover open, aim UNTOUCHED: server progress %.3f ")
							TEXT("(-1 = no hold), this player %s (\"%s\"), %.2fs left."),
							Progress, bMayPull ? TEXT("MAY pull") : TEXT("may NOT pull"),
							(Reason != nullptr) ? Reason : TEXT("legal"),
							Core->GetTurnoverSecondsRemaining());

						// THE ONE THING A MOUSE DOES, done explicitly and logged as such: point the
						// camera at the Core. Nothing else is faked — the aim cone, the line of sight,
						// the 0.30 s clock and the race stay the server's, and the line above is this
						// same player being refused a moment earlier for want of exactly this.
						const FVector ToCore = Core->GetLooseLocation() - Me->GetPawnViewLocation();
						const FRotator Before = PC->GetControlRotation();
						const FRotator After = ToCore.Rotation();
						PC->SetControlRotation(After);

						const TCHAR* AimedReason = nullptr;
						const bool bAimedMayPull = Core->CanPullNow(Me, &AimedReason);

						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] B(green) — aimed at the Core (pitch %.1f -> %.1f, yaw %.1f -> ")
							TEXT("%.1f): this player %s (\"%s\"). Progress is still %.3f — looking at it ")
							TEXT("does not start a pull; the button does."),
							Before.Pitch, After.Pitch, Before.Yaw, After.Yaw,
							bAimedMayPull ? TEXT("MAY pull") : TEXT("may NOT pull"),
							(AimedReason != nullptr) ? AimedReason : TEXT("legal"),
							Core->GetPullProgressFor(Me));

						Shot(TEXT("B_ring_empty"));
						Advance(2);
					}
					break;

				case 2:
					// THE REAL BUTTON, re-asserted every frame exactly as a held mouse button is. The
					// server runs its own hover test, its own line of sight and its own 0.30 s clock.
					Core->RequestPullInput(true, Me);

					if (!*ShotFilling && Progress > 0.15f && Progress < 0.95f)
					{
						*ShotFilling = true;
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] C — the ring is FILLING: SERVER progress %.3f of the %.2fs hold."),
							Progress, ATraceCore::GetPullHoldSeconds());
						Shot(TEXT("C_ring_filling"));
					}

					if (Core->GetHolder() == Me || Core->GetPullWinner() == Me)
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] D — THE PULL COMPLETED for %s after %.2fs. Delivery speed %.0f uu/s ")
							TEXT("(= ATraceCore::GetThrowSpeed(), not a number of its own). Holding now: %d"),
							*GetNameSafe(Me), InPhase, ATraceCore::GetThrowSpeed(),
							Core->GetHolder() == Me ? 1 : 0);
						Shot(TEXT("D_pull_won"));
						Core->RequestPullInput(false, Me);
						Advance(3);
					}
					else if (InPhase > 8.0)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[v25Integ] C/D INCONCLUSIVE after %.1fs: progress %.3f, CanPullNow says \"%s\". ")
							TEXT("The player was probably not looking at the staged point."),
							InPhase, Progress, (Reason != nullptr) ? Reason : TEXT("legal"));
						Core->RequestPullInput(false, Me);
						Advance(3);
					}
					break;

				case 3:
					if (InPhase >= 1.5)
					{
						// THE OTHER HALF OF THE TABLE. Staged AT THIS PLAYER'S FEET so the pickup poll is
						// offered to them on every single tick of the window — the refusal below is a
						// measured refusal, not an absence of opportunity.
						FString Report;
						if (!ATraceCore::DebugStageTurnoverAtLocalCrosshair(Live, /*DistanceUU=*/0.f,
							/*bLockLocalTeam=*/true, Report))
						{
							UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] E REFUSED: %s"), *Report);
							return false;
						}
						*LockoutOpened = Now;
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] E — MY OWN TEAM DROPPED IT, and the Core is at my feet. %s"), *Report);
						Shot(TEXT("E_locked_out"));
						Advance(4);
					}
					break;

				case 4:
				{
					const bool bHolding = (Core->GetHolder() == Me);
					const float Left = Core->GetTurnoverSecondsRemaining();

					if (bHolding)
					{
						const double Held = Now - *LockoutOpened;
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] F — the locked-out player TOOK IT at t+%.2fs (lockout was %.2fs). ")
							TEXT("%s"), Held, ATraceCore::GetTurnoverLockoutSeconds(),
							(Held + 0.35 >= static_cast<double>(ATraceCore::GetTurnoverLockoutSeconds()))
								? TEXT("That is AFTER the window, which is the rule.")
								: TEXT("*** THAT IS INSIDE THE WINDOW — THE LOCKOUT LEAKED. ***"));
						Shot(TEXT("F_after_lockout"));
						Advance(5);
						break;
					}

					// One line per half second for the whole window: standing on it, refused, by name.
					if (FMath::Fmod(static_cast<float>(Now - *LockoutOpened), 0.5f) < 0.05f)
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] E+%.2fs standing on the Core: holding=%d, ring=%s, %.2fs left."),
							Now - *LockoutOpened, bHolding ? 1 : 0,
							bMayPull ? TEXT("SHOWN") : TEXT("hidden (correct: locked out)"), Left);
					}

					if (Now - *LockoutOpened > static_cast<double>(ATraceCore::GetTurnoverLockoutSeconds()) + 6.0)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[v25Integ] F INCONCLUSIVE: %.1fs after the lockout expired the player still ")
							TEXT("has not taken it — they are probably not standing close enough to it."),
							Now - *LockoutOpened - static_cast<double>(ATraceCore::GetTurnoverLockoutSeconds()));
						Advance(5);
					}
					break;
				}

				case 5:
				{
					// ---- THE BEAM A/B, arm 1: the shipped multiplier -------------------------------
					//
					// "LARGER" AND "THE OTHER TEAM'S COLOUR" ARE BOTH CLAIMS ABOUT THE NORMAL BEAM, so
					// neither can be photographed once. Arm 1 is a turnover beam at the shipped
					// CoreTurnoverBeamScale; arm 2 is the SAME turnover, at the SAME staged point, with
					// that one multiplier forced to 1.0 and nothing else touched. The pair isolates the
					// multiplier itself, which a "wait for the window to close" pair could not: this arena
					// is full of bots who may legally take the Core the moment it opens, and four
					// consecutive attempts at that version were stolen before the window ended.
					//
					// MY OWN TEAM drops it here, so the beam is the OPPOSING colour — the mirror of arm A,
					// which photographed it in my own colour when the opposition dropped it.
					if (InPhase >= 1.0)
					{
						if (IConsoleVariable* BeamCVar = CVarModeBTurnoverBeamScale.AsVariable())
						{
							// SetByCode outranks the ini, which is how TraceModeBTuning::Resolve decides
							// who wins — so this really does move the shipped multiplier for these frames.
							BeamCVar->Set(2.2f, ECVF_SetByCode);
						}

						FString Report;
						if (!ATraceCore::DebugStageTurnoverAtLocalCrosshair(Live, Distance,
							/*bLockLocalTeam=*/true, Report))
						{
							UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] G REFUSED: %s"), *Report);
							return false;
						}

						const FVector ToCore = Core->GetLooseLocation() - Me->GetPawnViewLocation();
						PC->SetControlRotation(ToCore.Rotation());

						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] G — BEAM A/B arm 1 of 2: window OPEN, beam x%.2f, colour = the ")
							TEXT("team that did NOT drop it. %s"), ATraceCore::GetTurnoverBeamScale(), *Report);
						Advance(6);
					}
					break;
				}

				case 6:
					// The screenshot is deferred one phase so the beam has a frame to be rebuilt at the
					// new width before it is photographed.
					if (InPhase >= 0.3)
					{
						Shot(TEXT("G_beam_turnover_x2.2"));
						Advance(7);
					}
					break;

				case 7:
					// ---- Arm 2: the identical staging, the multiplier forced to 1.0 ----------------
					if (InPhase >= 1.0)
					{
						if (IConsoleVariable* BeamCVar = CVarModeBTurnoverBeamScale.AsVariable())
						{
							// SetByCode outranks the ini, which is how TraceModeBTuning::Resolve decides
							// who wins — so this really does move the shipped multiplier for these frames.
							BeamCVar->Set(1.0f, ECVF_SetByCode);
						}

						FString Report;
						if (!ATraceCore::DebugStageTurnoverAtLocalCrosshair(Live, Distance,
							/*bLockLocalTeam=*/true, Report))
						{
							UE_LOG(LogTraceGame, Warning, TEXT("[v25Integ] H REFUSED: %s"), *Report);
							return false;
						}

						const FVector ToCore = Core->GetLooseLocation() - Me->GetPawnViewLocation();
						PC->SetControlRotation(ToCore.Rotation());

						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] H — BEAM A/B arm 2 of 2: the same turnover at the same point with ")
							TEXT("CoreTurnoverBeamScale forced to x%.2f. One number changed between these two ")
							TEXT("frames."), ATraceCore::GetTurnoverBeamScale());
						Advance(8);
					}
					break;

				case 8:
					if (InPhase >= 0.3)
					{
						Shot(TEXT("H_beam_turnover_x1.0"));
						Advance(9);
					}
					break;

				case 9:
					// Put the shipped value back so nothing after this run reads a harness number, then
					// stop. (*BeamRetries is kept only so the capture ends on a named phase.)
					if (InPhase >= 1.0)
					{
						if (IConsoleVariable* BeamCVar = CVarModeBTurnoverBeamScale.AsVariable())
						{
							// SetByCode outranks the ini, which is how TraceModeBTuning::Resolve decides
							// who wins — so this really does move the shipped multiplier for these frames.
							BeamCVar->Set(2.2f, ECVF_SetByCode);
						}
						UE_LOG(LogTraceGame, Display,
							TEXT("[v25Integ] DONE. CoreTurnoverBeamScale restored to x%.2f. (%d)"),
							ATraceCore::GetTurnoverBeamScale(), *BeamRetries);
						return false;
					}
					break;

				default:
					return false;
				}

				return true;
			}), 0.f);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdDemoReg(
		TEXT("Trace.Integ.TurnoverDemo"),
		TEXT("SPEC v25 INTEGRATION. Runs the whole §2 table on the LOCAL player and photographs each ")
		TEXT("claim: turnover registered and the Core stays, beam recoloured/larger, ring empty, ring ")
		TEXT("filling off the server number, pull completing at the thrown speed, then the same player ")
		TEXT("locked out of their own team's drop for the full window and taking it after. Args: ")
		TEXT("[DistanceUU=450]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Demo));
}
#endif // !UE_BUILD_SHIPPING

bool ATraceCore::DriveAimAtLooseCore(ATraceCharacter* Puller)
{
	if (!IsValid(Puller) || !bLoose)
	{
		return false;
	}

	AController* Controller = Puller->GetController();
	if (Controller == nullptr || Controller->IsPlayerController())
	{
		return false;
	}

	const FVector ToCore = FVector(LooseLocation) - Puller->GetPawnViewLocation();
	if (ToCore.IsNearlyZero())
	{
		return false;
	}

	Controller->SetControlRotation(ToCore.Rotation());
	return true;
}

void ATraceCore::TickTurnoverVerify()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	if (!bTurnoverVerifyArmed)
	{
		if (bTurnoverVerifyDone || CVarModeBTurnoverVerify.GetValueOnGameThread() == 0)
		{
			return;   // One bool and one int compare in the steady state.
		}

		// A LATCH, not a re-read of the CVar. -ExecCmds arms at console priority and a code-priority
		// Set(0) is silently dropped, which is how a previous harness in this file re-fired 48 times -
		// and how the first version of THIS one ran three times in forty seconds.
		bTurnoverVerifyArmed = true;
		bTurnoverVerifySawWinner = false;
		TurnoverVerifyStep = 0;
		TurnoverVerifyPassCount = 0;
		TurnoverVerifyFailCount = 0;
		TurnoverVerifySkipCount = 0;
		TurnoverVerifyRetriesLeft = TraceCoreTurnoverVerify::LockoutRetries;
	}

	const float Now = GetServerTimeSeconds();

	const auto Pass = [this](const TCHAR* What)
	{
		++TurnoverVerifyPassCount;
		UE_LOG(LogTraceGame, Display, TEXT("[v25Turnover] PASS - %s"), What);
	};
	const auto Fail = [this](const TCHAR* What)
	{
		++TurnoverVerifyFailCount;
		UE_LOG(LogTraceGame, Error, TEXT("[v25Turnover] *** FAIL *** - %s"), What);
	};
	const auto Skip = [this](const TCHAR* What)
	{
		++TurnoverVerifySkipCount;
		UE_LOG(LogTraceGame, Warning, TEXT("[v25Turnover] SKIP - %s"), What);
	};

	// A step that stops making progress is reported as inconclusive rather than left to hang a run.
	if (TurnoverVerifyStep > 0 && TurnoverVerifyDeadline > 0.f && Now > TurnoverVerifyDeadline)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[v25Turnover] step %d timed out after %.1fs - inconclusive, moving on."),
			TurnoverVerifyStep, TraceCoreTurnoverVerify::StepTimeoutSeconds);
		++TurnoverVerifySkipCount;

		// A timeout in steps 1-4 skips to the LOCKOUT step, which is independent of them and is worth
		// measuring on its own; a timeout in step 5 goes to the report, because sending it back to
		// itself with a deadline already in the past is an infinite loop rather than a retry.
		TurnoverVerifyStep = (TurnoverVerifyStep >= 5) ? 6 : 5;
		TurnoverVerifyMark = -1.f;
		TurnoverVerifyDeadline = 0.f;
	}

	ATraceCharacter* Puller = TurnoverVerifyPuller.Get();
	ATraceCharacter* Locked = TurnoverVerifyLocked.Get();

	switch (TurnoverVerifyStep)
	{
	case 0:
	{
		if (!IsModeB())
		{
			Skip(TEXT("this match is not in goals mode; spec v25 §2 is goals mode only."));
			TurnoverVerifyStep = 5;
			break;
		}

		TArray<ATraceCharacter*> All;
		GatherCharacters(All);

		ATraceCharacter* Blue = TraceCoreTurnoverVerify::FindDrivableBot(All, ETraceTeam::Blue);
		ATraceCharacter* Orange = TraceCoreTurnoverVerify::FindDrivableBot(All, ETraceTeam::Orange);

		if (Blue == nullptr || Orange == nullptr)
		{
			Skip(TEXT("no drivable bot on one of the teams; the harness will not move a human's crosshair."));
			TurnoverVerifyStep = 5;
			break;
		}

		// ORANGE DROPS IT, BLUE PULLS. The Core is parked at Orange's own feet, which is what makes
		// step 4 a real test of the pickup lockout rather than an argument about it: the locked-out
		// player is standing inside the pickup radius for the whole window.
		TurnoverVerifyPuller = Blue;
		TurnoverVerifyLocked = Orange;

		// Placed where BLUE can see it, not where Orange happens to be standing: steps 1-3 all turn on
		// the puller having line of sight, and step 5 - the only one that needs the Core at the
		// locked-out player's feet - re-arms it there itself.
		FVector Where = FVector::ZeroVector;
		if (!TraceCoreTurnoverVerify::FindPullablePoint(Blue, TraceModeBVisibleOrbRadius, Where))
		{
			Skip(TEXT("the puller has clear sight of nowhere nearby; the green arms could not fire."));
			TurnoverVerifyStep = 5;
			break;
		}

		if (!DebugRegisterTurnover(ETraceTeam::Orange, Where))
		{
			Skip(TEXT("could not arm a turnover (the Core is locked, or the mode changed under us)."));
			TurnoverVerifyStep = 5;
			break;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[v25Turnover] armed: Orange dropped it at %s, Blue may pull. Lockout %.2fs, hold %.2fs, ")
			TEXT("delivery speed %.0f uu/s (= the thrown speed)."),
			*Where.ToCompactString(), GetTurnoverLockoutSeconds(), GetPullHoldSeconds(), GetThrowSpeed());

		TurnoverVerifyStep = 1;
		TurnoverVerifyMark = Now;
		TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
		break;
	}

	case 1:
	{
		// --- RED ARM 1: A SAME-TEAM PLAYER MUST FAIL TO PULL. ------------------------------------
		//
		// Orange dropped it, so Orange is locked out - and the test is run on a player who is NOT the
		// thrower, because spec v25 puts the lockout on the TEAM. Their aim is driven onto the Core and
		// their button is pressed, so every other condition in CanPullNow is satisfied and the only
		// thing that can refuse them is the team rule.
		if (!IsValid(Locked) || !IsValid(Puller) || !IsTurnoverActive())
		{
			Skip(TEXT("step 1: the window or a pawn went away before it could be measured."));
			TurnoverVerifyStep = 4;
			break;
		}

		DriveAimAtLooseCore(Locked);
		RequestPullInput(true, Locked);

		const TCHAR* Reason = TEXT("(none)");
		const bool bAllowed = CanPullNow(Locked, &Reason);

		// One frame later the state machine has run, so PullHolds is the authoritative answer to
		// "did a fill actually start" - CanPullNow alone would only prove the query agrees with itself.
		if (Now - TurnoverVerifyMark >= 0.05f)
		{
			const bool bHasHold = GetPullProgressFor(Locked) >= 0.f;

			if (!bAllowed && !bHasHold)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[v25Turnover] red arm 1: %s (Orange, the side that dropped it) was refused - \"%s\", ")
					TEXT("and no fill started."),
					*GetNameSafe(Locked), Reason);
				Pass(TEXT("a same-team player cannot pull."));
			}
			else
			{
				Fail(TEXT("a player on the team that DROPPED the Core was allowed to pull it."));
			}

			RequestPullInput(false, Locked);

			// GREEN ARM: the same instant, the same Core, the opposing player. Without this the red arm
			// above would also pass on a build where nobody can pull at all.
			DriveAimAtLooseCore(Puller);
			const TCHAR* GreenReason = TEXT("(none)");
			if (CanPullNow(Puller, &GreenReason))
			{
				Pass(TEXT("the OPPOSING player, on the same frame, is allowed to pull (green arm)."));
			}
			else
			{
				Fail(TEXT("the opposing player could not pull either - the red arm above proves nothing."));
				UE_LOG(LogTraceGame, Error, TEXT("[v25Turnover]   refusal was: %s"), GreenReason);
			}

			TurnoverVerifyStep = 2;
			TurnoverVerifyMark = Now;
			TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
		}
		break;
	}

	case 2:
	{
		// --- RED ARM 2: NO LINE OF SIGHT MUST FAIL. ----------------------------------------------
		//
		// Measured by moving the Core, for one query and with no tick in between, to a point 5000 uu
		// BELOW where it is lying - so the ray from the puller's eye crosses the arena floor slab and
		// is genuinely blocked by real world geometry. The alternative was to wait for a bot to
		// happen to stand behind a crate, which is not a test, and to hope the crate was the reason.
		if (!IsValid(Puller) || !IsTurnoverActive())
		{
			Skip(TEXT("step 2: the window or the puller went away before it could be measured."));
			TurnoverVerifyStep = 4;
			break;
		}

		DriveAimAtLooseCore(Puller);

		const TCHAR* ClearReason = TEXT("(none)");
		const bool bClear = CanPullNow(Puller, &ClearReason);

		const FVector Restore = LooseLocation;
		LooseLocation = Restore - FVector(0.0, 0.0, 5000.0);

		const TCHAR* BlockedReason = TEXT("(none)");
		const bool bBlocked = !CanPullNow(Puller, &BlockedReason);

		LooseLocation = Restore;

		if (bBlocked && FCString::Strstr(BlockedReason, TEXT("line of sight")) != nullptr)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[v25Turnover] red arm 2: with the floor slab between them the pull was refused - \"%s\"."),
				BlockedReason);
			Pass(TEXT("a player with no line of sight cannot pull."));
		}
		else
		{
			Fail(TEXT("a pull was allowed through solid geometry."));
		}

		if (bClear)
		{
			Pass(TEXT("the same player with a clear view CAN pull (green arm)."));
		}
		else
		{
			Fail(TEXT("the puller had no clear view either - the red arm above proves nothing."));
			UE_LOG(LogTraceGame, Error, TEXT("[v25Turnover]   refusal was: %s"), ClearReason);
		}

		TurnoverVerifyStep = 3;
		TurnoverVerifyMark = -1.f;   // "not pressed yet"
		TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
		break;
	}

	case 3:
	{
		// --- RED ARM 3: RELEASING AT 0.29 s MUST FAIL, AND HOLDING PAST 0.30 s MUST WIN. ---------
		if (!IsValid(Puller) || !IsTurnoverActive())
		{
			Skip(TEXT("step 3: the window or the puller went away before it could be measured."));
			TurnoverVerifyStep = 4;
			break;
		}

		DriveAimAtLooseCore(Puller);

		const float Hold = GetPullHoldSeconds();

		if (TurnoverVerifyMark < 0.f)
		{
			RequestPullInput(true, Puller);
			TurnoverVerifyMark = Now;
			break;
		}

		const float Held = Now - TurnoverVerifyMark;

		// Released on the last tick that is still SHORT of the hold time. Two frames of margin rather
		// than one, because the completion is decided by ServerTickTurnover LATER in this same frame -
		// so a release computed against a threshold this tick could otherwise be beaten by a
		// completion the previous tick had already earned. The measured hold is printed, so a harness
		// that overshoots says so instead of passing quietly.
		const float Margin = 2.f * FMath::Max(0.001f, static_cast<float>(World->GetDeltaSeconds()));

		if (Held + Margin >= Hold)
		{
			RequestPullInput(false, Puller);

			const bool bNoWinner = (PullWinner == nullptr);
			const bool bNoHold = (GetPullProgressFor(Puller) < 0.f);

			if (Held < Hold && bNoWinner)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[v25Turnover] red arm 3: released after %.3fs of a %.3fs hold - no pull completed, ")
					TEXT("and the fill was CANCELLED (progress now %s)."),
					Held, Hold, bNoHold ? TEXT("gone") : TEXT("STILL RUNNING"));
				Pass(TEXT("a release short of the hold time does not pull, and does not pause."));
			}
			else if (Held >= Hold)
			{
				Skip(TEXT("step 3: the harness overshot the hold time; the release was not short. "
					"Re-run with a slower tick or a longer CorePullHoldSeconds."));
			}
			else
			{
				Fail(TEXT("a release short of the hold time still completed a pull."));
			}

			TurnoverVerifyStep = 4;
			TurnoverVerifyMark = -1.f;
			TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
		}
		break;
	}

	case 4:
	{
		// --- GREEN ARM 3: A FULL HOLD COMPLETES AND DELIVERS AT THE THROWN SPEED. ----------------
		if (!IsValid(Puller))
		{
			Skip(TEXT("step 4: the puller went away."));
			TurnoverVerifyStep = 5;
			break;
		}

		if (TurnoverVerifyMark < 0.f)
		{
			if (!IsTurnoverActive())
			{
				// The window ran out while steps 1-3 were being measured. Re-arm it rather than
				// reporting a failure of a rule that was never given a chance.
				FVector Where = FVector::ZeroVector;
				if (!TraceCoreTurnoverVerify::FindPullablePoint(Puller, TraceModeBVisibleOrbRadius, Where)
					|| !DebugRegisterTurnover(ETraceTeam::Orange, Where))
				{
					Skip(TEXT("step 4: could not re-arm the window."));
					TurnoverVerifyStep = 5;
					break;
				}
			}

			bTurnoverVerifySawWinner = false;
			DriveAimAtLooseCore(Puller);
			RequestPullInput(true, Puller);
			TurnoverVerifyMark = Now;
			break;
		}

		DriveAimAtLooseCore(Puller);

		if (IsValid(Carrier) && Carrier == Puller)
		{
			// bTurnoverVerifySawWinner is what stops this passing on a puller who simply WALKED OVER
			// the Core: the ordinary pickup poll would hand it to them too, and the step would then be
			// reporting the pickup radius rather than the pull.
			if (bTurnoverVerifySawWinner)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[v25Turnover] green arm 3: %s held past %.2fs, the Core travelled to them at the full ")
					TEXT("thrown speed (%.0f uu/s) and they now hold it."),
					*GetNameSafe(Puller), GetPullHoldSeconds(), GetThrowSpeed());
				Pass(TEXT("a completed pull delivers the Core to the puller."));
			}
			else
			{
				Skip(TEXT("step 4: the puller reached the Core on foot before the hold completed; "
					"the delivery itself was not observed."));
			}

			RequestPullInput(false, Puller);
			TurnoverVerifyStep = 5;
			TurnoverVerifyMark = -1.f;
			TurnoverVerifyDeadline = Now + TraceCoreTurnoverVerify::StepTimeoutSeconds;
			break;
		}

		if (PullWinner == Puller)
		{
			bTurnoverVerifySawWinner = true;

			// In flight. The one number worth asserting here is the speed, because it is the one spec
			// v25 names and the one a re-implementation would get wrong.
			const double Speed = FVector(LooseVelocity).Size();
			const double Expected = static_cast<double>(GetThrowSpeed());

			if (FMath::Abs(Speed - Expected) <= 1.0)
			{
				UE_LOG(LogTraceGame, Verbose,
					TEXT("[v25Turnover] delivery in flight at %.0f uu/s (thrown speed %.0f)."), Speed, Expected);
			}
			else
			{
				Fail(TEXT("the pulled Core is not travelling at the Core's thrown speed."));
				UE_LOG(LogTraceGame, Error,
					TEXT("[v25Turnover]   measured %.0f uu/s, ATraceCore::GetThrowSpeed() says %.0f."),
					Speed, Expected);
				TurnoverVerifyStep = 5;
			}
		}
		break;
	}

	case 5:
	{
		// --- RED ARM 4: THE LOCKED-OUT TEAM CANNOT PICK IT UP, AND THE LOCKOUT MUST EXPIRE. ------
		//
		// The Core is pinned to the locked-out player's feet for the whole window, so the pickup poll
		// is being offered them on every single tick. It must refuse for the length of the window and
		// take it within a tick or two of the window closing. Both halves come out of the SAME poll -
		// there is no second code path for "after the lockout", which is the point of row 3 of the
		// table being row 1.
		if (!IsValid(Locked))
		{
			Skip(TEXT("step 5: the locked-out player went away."));
			TurnoverVerifyStep = 6;
			break;
		}

		if (TurnoverVerifyMark < 0.f)
		{
			double FeetDrop = 88.0;
			if (const UCapsuleComponent* Capsule = Locked->GetCapsuleComponent())
			{
				FeetDrop = Capsule->GetScaledCapsuleHalfHeight();
			}

			const FVector Where = Locked->GetActorLocation()
				- FVector(0.0, 0.0, FeetDrop - TraceModeBVisibleOrbRadius);

			if (!DebugRegisterTurnover(ETraceTeam::Orange, Where))
			{
				Skip(TEXT("step 5: could not arm the lockout window."));
				TurnoverVerifyStep = 6;
				break;
			}

			TurnoverVerifyMark = Now;
			TurnoverVerifyDeadline = Now + GetTurnoverLockoutSeconds()
				+ TraceCoreTurnoverVerify::StepTimeoutSeconds;
			break;
		}

		// Pinned to their feet every frame, so a wandering bot cannot quietly turn this into a test of
		// nothing. The Core is at rest and the turnover is already latched, so no landing re-fires.
		if (bLoose)
		{
			double FeetDrop = 88.0;
			if (const UCapsuleComponent* Capsule = Locked->GetCapsuleComponent())
			{
				FeetDrop = Capsule->GetScaledCapsuleHalfHeight();
			}

			LooseLocation = Locked->GetActorLocation() - FVector(0.0, 0.0, FeetDrop - TraceModeBVisibleOrbRadius);
			LooseVelocity = FVector::ZeroVector;
			bLooseAtRest = true;
		}

		const float Elapsed = Now - TurnoverVerifyMark;

		if (IsValid(Carrier))
		{
			if (Carrier == Locked && IsTurnoverActive())
			{
				Fail(TEXT("a player on the LOCKED-OUT team picked the Core up during the window."));
				TurnoverVerifyStep = 6;
			}
			else if (Carrier == Locked)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[v25Turnover] red arm 4: %s (Orange, locked out) stood on the Core for the whole ")
					TEXT("%.2fs window and could not take it, then took it %.2fs after it expired."),
					*GetNameSafe(Locked), GetTurnoverLockoutSeconds(),
					Elapsed - GetTurnoverLockoutSeconds());
				Pass(TEXT("the lockout refuses the dropping team, and expires."));
				TurnoverVerifyStep = 6;
			}
			else
			{
				// Legal: an opposing player is allowed to take it at any point in the window. It just
				// means this step measured nothing, so re-arm rather than report a result it did not get.
				if (TurnoverVerifyRetriesLeft-- > 0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[v25Turnover] step 5: %s took the Core legally; re-arming (%d retries left)."),
						*GetNameSafe(Carrier), TurnoverVerifyRetriesLeft);
					TurnoverVerifyMark = -1.f;
				}
				else
				{
					Skip(TEXT("step 5: the Core kept being taken legally by the opposing team."));
					TurnoverVerifyStep = 6;
				}
			}
			break;
		}

		if (Elapsed > GetTurnoverLockoutSeconds() + 1.0f)
		{
			Fail(TEXT("the lockout expired but the player standing on the Core still did not get it."));
			TurnoverVerifyStep = 6;
		}
		break;
	}

	default:
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[v25Turnover] ===== SPEC v25 §2 RED ARM: %d passed, %d FAILED, %d skipped ====="),
			TurnoverVerifyPassCount, TurnoverVerifyFailCount, TurnoverVerifySkipCount);

		if (TurnoverVerifyFailCount > 0)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[v25Turnover] the turnover rules above did not hold. Trace.ModeB.TurnoverPull 0 ")
				TEXT("restores the pre-v25 behaviour if a comparison is wanted."));
		}

		bTurnoverVerifyArmed = false;
		bTurnoverVerifyDone = true;   // See the field: the CVar cannot be written back down.
		TurnoverVerifyStep = -1;
		TurnoverVerifyPuller = nullptr;
		TurnoverVerifyLocked = nullptr;
		break;
	}
	}
}

// =================================================================================================
// SPEC v25 §2 — ATraceCorePullRelay
//
// See the class comment in TraceCore.h. In one sentence: a client may only send a Server RPC on an
// actor its own connection owns, ATraceCore is owned by its HOLDER, and a pull comes from somebody
// who is not the holder — so the button needs a carrier of its own.
//
// It is deliberately the smallest thing that can do that. No state, no tick, no replicated property,
// one function, one bool. Everything it forwards is re-decided by ATraceCore on the server.
// =================================================================================================

ATraceCorePullRelay::ATraceCorePullRelay()
{
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = true;
	SetReplicateMovement(false);
	SetCanBeDamaged(false);

	// ONLY ITS OWNER EVER SEES IT. Nine other clients being told that a tenth player's input relay
	// exists is nine channels' worth of nothing; more importantly, an actor that is relevant to
	// everybody is an actor whose ownership a reader has to think about, and this one's whole purpose
	// is that its owner is exactly one connection.
	bOnlyRelevantToOwner = true;
	bAlwaysRelevant = false;
	bNetLoadOnClient = false;
}

ATraceCorePullRelay* ATraceCorePullRelay::Find(const AController* Controller)
{
	if (Controller == nullptr)
	{
		return nullptr;
	}

	UWorld* World = Controller->GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<ATraceCorePullRelay> It(World); It; ++It)
	{
		ATraceCorePullRelay* Relay = *It;
		if (IsValid(Relay) && Relay->GetOwner() == Controller)
		{
			return Relay;
		}
	}

	return nullptr;
}

void ATraceCorePullRelay::ServerSetPullInput_Implementation(bool bPressed)
{
	// The pawn is read from the OWNING CONTROLLER on the server, never sent by the client. A client
	// that could name the pawn its press applies to could press for somebody else's character, which
	// on a mechanic that hands out the Core is the single most valuable thing in the game to lie
	// about — the same policy the throw's aim direction and its hold length already have.
	// NOT named `Owner`: AActor::Owner is a member of every class here, and MSVC treats the shadow
	// as an ERROR (C4458) while Apple clang cannot even warn about it — UBT hard-disables shadow
	// warnings for clang 17-18.1.3 and Apple clang reports 17.0.0. So this compiles on macOS and
	// stops every Windows developer. Scripts/check-engine-member-shadowing.py now gates on it.
	const AController* OwningController = Cast<AController>(GetOwner());
	ATraceCharacter* Pawn = (OwningController != nullptr)
		? Cast<ATraceCharacter>(OwningController->GetPawn()) : nullptr;

	ATraceCore* Core = ATraceCore::Get(GetWorld());
	if (Core == nullptr || Pawn == nullptr)
	{
		return;
	}

	Core->RequestPullInput(bPressed, Pawn);
}
