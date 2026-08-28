// Copyright (c) Trace. All Rights Reserved.
//
// See TraceCore.h for the model. This file is the whole of it: there is no physics, no pickup
// volume and no flight path left to go wrong.

#include "Gameplay/TraceCore.h"

#include "Abilities/TraceAbilityComponent.h"   // spec v14 §6: Mace's per-player magnet radius
#include "Abilities/TraceAbilityTypes.h"       // spec v19 §3: TraceAbilityTraits — Mortimer's longer charge hold
#include "Audio/TraceAudio.h"                  // spec v26 §9: CoreTurnover (game-side), CorePickup (client-side)
#include "UObject/ObjectKey.h"                 // FObjectKey — the per-actor state the FX_AUDIO_PLAN cues keep beside their call sites
#include "Net/UnrealNetwork.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TraceMatchTypes.h"      // TraceIsGoalMode (mode B)
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceFxShapes.h"           // SPEC v32 §3: the shared FX shape library
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "World/TraceArenaBuilder.h"

#include "Gameplay/TraceEndzone.h"

#include "Gameplay/TraceCoreInternal.h"   // the tuning tables, the art table and this file's console
                                          // variables, shared with TraceCoreHarness.cpp

// AreEnemies / AreAllies / CoreGeometryEpsilon used to sit in an anonymous namespace here. They are
// in TraceCoreInternal.h now because the harness asks the same two questions, and a harness with its
// own copy of "are these two on the same side" could agree with itself while disagreeing with the
// game. Pulled in unqualified so every call site below reads exactly as it did.
using namespace TraceCoreLocal;

#include "Animation/AnimSequence.h"             // spec v31 §4: the pack's three Core clips
#include "Animation/AnimSingleNodeInstance.h"   // spec v31 §4: single-node playback, no AnimBlueprint
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"     // spec v31 §4: the #FF8A1F heart light
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"   // spec v31 §4: SK_TraceCore
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"                // spec v31 §4: GetImportedBounds() for the art scale
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
#include "HAL/PlatformFileManager.h"            // CreateDirectoryTree (Trace.Core.ArtShots)
#include "Misc/DateTime.h"                      // screenshot filenames (Trace.Core.ArtShots)
#include "HAL/FileManager.h"                    // IFileManager::Move (Trace.Core.ArtShots renames)
#include "Misc/Paths.h"                         // FPaths::Combine  (Trace.Integ.TurnoverDemo)
#include "TimerManager.h"                       // spec v31 §4: the art-shot beat schedule
#include "UnrealClient.h"                       // FScreenshotRequest (Trace.Integ.TurnoverDemo)
#include "UObject/ConstructorHelpers.h"
#include "UObject/UnrealType.h"                 // FindFProperty / FFloatProperty (mode B settings bridge)

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
	ECVF_Cheat);

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

// =================================================================================================
// FX_AUDIO_PLAN §5.1 — THE KICKOFF COUNTDOWN (CountdownTick / CountdownGo)
//
// Named after the file, never anonymous: this module is a unity build and
// Scripts/check-jumbo-build-collisions.py gates on exactly that.
//
// *** WHAT THIS CAN AND CANNOT REACH, SAID PLAINLY, BECAUSE IT IS A REAL LIMIT. ***
// The kickoff window is server-only state: PendingGrantTime and PendingGrantTeam are ordinary
// members of ATraceCore and are NOT replicated (see their declarations), so the only machine that
// can know how long is left is the authority. Both events are also declared CLIENT-side and 2D in
// Audio/TraceSoundEvents.h — a countdown is a UI sound with no position — and there is deliberately
// no "2D multicast" in the audio API (TraceAudio.h's four bypasses are all spatialised or already
// on every machine). So what ships here is the countdown ON THE AUTHORITY'S OWN MACHINE: a listen
// host hears it, a standalone session hears it, a dedicated server has no device, and a REMOTE
// CLIENT DOES NOT. Closing that needs one of two things, neither of which is inside this pass's
// ownership line: replicating the kickoff deadline (Gameplay/TraceCore.h), or a 2D counterpart to
// ATraceAudioRelay::MulticastPlaySound (Source/Trace/Audio/). Recorded in the pass report rather
// than papered over with a spatialised sound that would be wrong for a different reason.
//
// WHY WHOLE SECONDS AND NOT A TIMER. The rule is "one tick per whole second remaining, the last
// three" — §5.1's row — and it is evaluated from the deadline every frame rather than scheduled,
// for the same reason every other clock in this project is: a scheduled countdown has to be
// cancelled by each of the several things that can move or cancel a kickoff (a goal during the
// wait, half time, ResolveFallback re-stamping the deadline), and one of them will eventually be
// missed. Reading the remaining time cannot go stale.
//
// TraceCoreTuning::KickoffDelaySeconds is 1.0 s today, so in practice this is exactly one tick and
// then GO. The three-second cap is written anyway so that raising the delay produces the countdown
// §5.1 describes with no edit here.
// =================================================================================================
namespace TraceCoreFile
{
	/** The last whole second announced for each live Core. Keyed so two PIE worlds cannot share one. */
	static TMap<FObjectKey, int32> GKickoffLastTickSecond;

	/** §5.1: "last 3 s". */
	static constexpr int32 KickoffCountdownSeconds = 3;

	/** Forget a Core's countdown state, so the next kickoff starts from silence. */
	void ResetKickoffCountdown(const AActor* Core)
	{
		if (Core != nullptr)
		{
			GKickoffLastTickSecond.Remove(FObjectKey(Core));
		}
	}

	/** One tick per whole second remaining, at most once each, for the last KickoffCountdownSeconds. */
	void TickKickoffCountdown(const AActor* Core, double SecondsRemaining)
	{
		if (Core == nullptr || SecondsRemaining <= 0.0)
		{
			return;
		}

		// CEIL, so 0.4 s left is "1" and not "0": the number a countdown announces is the second it
		// is currently IN, which is the one a player would say out loud.
		const int32 Second = FMath::CeilToInt(static_cast<float>(SecondsRemaining));
		if (Second <= 0 || Second > KickoffCountdownSeconds)
		{
			return;
		}

		int32& Last = GKickoffLastTickSecond.FindOrAdd(FObjectKey(Core), MAX_int32);
		if (Second >= Last)
		{
			return;   // already announced, or the deadline moved backwards under us
		}

		Last = Second;
		TraceAudio::PlayLocal2D(Core, TraceSoundEvents::CountdownTick);
	}
}

#if !UE_BUILD_SHIPPING
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
#endif // !UE_BUILD_SHIPPING

namespace TracePassStats
{
#if !UE_BUILD_SHIPPING
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
#endif // !UE_BUILD_SHIPPING

	// Used by the SHIPPING IsLegalPassTarget refusal strings as well as by the stats dump, so it
	// stays in all configs while the stats rig above and below is dev-only.
	static const TCHAR* RefusalName(int32 Index);

#if !UE_BUILD_SHIPPING
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
#endif // !UE_BUILD_SHIPPING
} // namespace TracePassStats

#if !UE_BUILD_SHIPPING
static FAutoConsoleCommand GTracePassStatsDumpCmd(
	TEXT("Trace.PassStats.Dump"),
	TEXT("Prints the accumulated pass acquisition / cancellation statistics gathered while Trace.PassStats is 1."),
	FConsoleCommandDelegate::CreateStatic([]() { TracePassStats::Dump(); }));

static FAutoConsoleCommand GTracePassStatsResetCmd(
	TEXT("Trace.PassStats.Reset"),
	TEXT("Clears the accumulated pass statistics."),
	FConsoleCommandDelegate::CreateStatic([]() { TracePassStats::Reset(); }));
#endif // !UE_BUILD_SHIPPING


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
			// NOT SILENT ANY MORE (release-hygiene pass). Falling back to the console default is the
			// designed behaviour for a knob that was never declared, but every name this bridge is
			// asked for today IS declared on UTraceSettings - so a miss here means somebody renamed a
			// settings property and quietly disconnected its slider. Scream once per name.
			static TSet<FName> ReportedMisses;
			if (!ReportedMisses.Contains(Key))
			{
				ReportedMisses.Add(Key);
				UE_LOG(LogTraceGame, Error,
					TEXT("[ModeBTuning] UTraceSettings has no float property named '%s'; the console-variable ")
					TEXT("default is in force. If this name was renamed on the settings page, the rename ")
					TEXT("disconnected the slider - make the two spellings match again."),
					PropertyName);
			}
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

// =================================================================================================
// THE MODE B TUNING KNOBS ARE `ECVF_Cheat` (RESTRUCTURE B1's classification, applied in wave 3).
//
// B1's rule is BY CLASS, NOT BY NAME: anything that overrides a RULE or a BALANCE number a player
// can feel — throw speed, gravity, pickup radius, catch radius, the landing/turnover rules, the pull
// envelope — is a developer instrument and takes ECVF_Cheat, so a shipped build cannot be retuned
// from its own console. Nothing about their SEMANTICS or their DEFAULTS moved (guardrail 4 and 10);
// every one of them still resolves settings-first through TraceModeBTuning, still answers the same
// number, and is still fully settable in Development, which is where every harness that drives them
// runs. This file was not owned in wave 1, which is the only reason the sweep reaches it now.
//
// THREE DELIBERATE EXCEPTIONS, each argued at its own registration and NOT flipped:
//   * Trace.ModeB.ThrowVelocityInheritanceDown — a FEEL knob a playtester must be able to A/B, with
//     an explicit "NOT ECVF_Cheat" decision at its registration. An in-file adjudication beats a
//     sweep.
//   * Trace.ModeB.FlightLog and Trace.ModeB.TurnoverLog — pure logging instruments that change no
//     rule, the same class as Trace.Knife.Debug.
//
// *** THIS LIST USED TO SAY FOUR, AND NAMED Trace.ModeB.ThrowChargeAnchorAtPress AND
// "the Trace.Audio.*Watch diagnostics that B1 kept". W9-SHIPGUARD FLIPPED BOTH FAMILIES. *** The
// in-file adjudications were not overruled on their merits — the anchor arm still changes no rule a
// player can see — but on their COST, which they had wrong. They read as though ECVF_Cheat takes a
// switch away from a playtester; it does not. ECVF_Cheat is inert wherever DISABLE_CHEAT_CVARS is 0,
// which is every configuration except Shipping and Test, so the console, -ExecCmds and
// ConsoleVariables.ini [Startup] still reach every one of them on any build a playtester runs
// (measured: `Trace.Move.SurfLegacyAirLimit 1` from -TraceExec on a Development build answers
// `LastSetBy: Console`, and it has been ECVF_Cheat all along). What the flag closes is the ONE
// injection path that survives into a shipped game — the [ConsoleVariables] section of a
// player-writable Engine.ini, which LoadConsoleVariablesFromINI applies with bAllowCheating = false
// in every configuration. W8-BATTERY §8.4 found twenty self-described RED ARMS reachable that way;
// the anchor arm was one of them. A rule with twenty exceptions is not a rule.
// =================================================================================================

// NO "Trace.ScoringMode" CONSOLE VARIABLE. There was one, and it was wrong: ATraceGameState
// publishes the mode (resolved by ATraceGameMode from "?mode=a|b" on the travel URL) and that is the
// one legal answer. A console override here would have been a second source of truth for the fact
// that decides what this actor IS, and the two would have disagreed on exactly the frame it
// mattered — this file's own log showed the pair fighting, mode A being set and then flipped to B a
// frame later. A run selects its mode with "?mode=b" on the URL, like the menu does.

// *** THE DEFAULT HERE IS A FALLBACK, AND PATCH 28 §4 MOVED IT TO KEEP IT HONEST. ***
// TraceModeBTuning::Resolve() answers with UTraceSettings::CoreThrowSpeed whenever that property
// exists (it does, and Config/DefaultGame.ini writes it), and only reads this number when the
// by-name lookup misses. It sat at 3000 for the whole life of the 3300 base, so it was already a
// stale second opinion about the shipped throw — the exact shape of bug the by-name binding note in
// DefaultGame.ini warns about, and the reason a missed lookup is so hard to notice. It is now 2900,
// i.e. the same number the property and the ini carry, so a lookup miss degrades to the CORRECT
// throw instead of a quietly different one. If CoreThrowSpeed is retuned again, retune this too.
static TAutoConsoleVariable<float> CVarModeBThrowSpeed(
	TEXT("Trace.ModeB.ThrowSpeed"),
	2900.f,
	TEXT("MODE B. Launch speed of a thrown Core, uu/s. Maps to UTraceSettings::CoreThrowSpeed when that exists."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBThrowUpBias(
	TEXT("Trace.ModeB.ThrowUpBias"),
	0.12f,
	TEXT("MODE B. Upward component added to a throw, as a fraction of throw speed. Gives a flat aim a ")
	TEXT("shallow arc so a throw carries instead of ploughing into the floor. UTraceSettings::CoreThrowUpBias."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBThrowGravityScale(
	TEXT("Trace.ModeB.GravityScale"),
	0.55f,
	TEXT("MODE B. World gravity multiplier applied to a loose Core. Below 1 so a throw crosses useful ")
	TEXT("ground on a 24000uu field. UTraceSettings::CoreThrowGravityScale."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBPickupRadius(
	TEXT("Trace.ModeB.PickupRadius"),
	120.f,
	TEXT("MODE B. 'First contact' radius, uu, measured from the Core to the surface of a player's ")
	TEXT("capsule. UTraceSettings::CorePickupRadius."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBSelfPickupLockout(
	TEXT("Trace.ModeB.SelfPickupLockout"),
	0.35f,
	TEXT("MODE B. Seconds the THROWER alone cannot re-take their own throw. Everyone else may take it ")
	TEXT("on frame one. Without this a throw is a no-op: the Core leaves from inside the thrower's own ")
	TEXT("pickup radius. UTraceSettings::CoreThrowerPickupLockoutSeconds."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBLooseReset(
	TEXT("Trace.ModeB.LooseResetSeconds"),
	12.f,
	TEXT("MODE B. Seconds a loose Core may lie untouched before it is put back into play. The Core may ")
	TEXT("never be lost permanently. UTraceSettings::CoreLooseResetSeconds."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBThrowCooldown(
	TEXT("Trace.ModeB.ThrowCooldown"),
	0.35f,
	TEXT("MODE B. Seconds after taking the Core before it may be thrown again. Stops a pickup and a ")
	TEXT("throw landing on the same frame. UTraceSettings::CoreThrowCooldownSeconds."),
	ECVF_Cheat);

// =================================================================================================
// MODE B ONLY — SPEC v8 §4, THE THROW INHERITS THE THROWER'S MOMENTUM
//
// "When jumping and throwing the core, the core doesn't seem to keep momentum. Make sure that the
// core has the momentum from the throw and also carries momentum from the player."
//
// It did not seem to keep momentum because it did not keep any: ThrowFromHolder built the launch as
// aim * ThrowSpeed + up * (ThrowSpeed * UpBias) and that was the whole of it. The thrower's velocity
// appeared nowhere, so a Core thrown at a dead stop, at a full sprint and at the apex of a jump all
// left at exactly the post-weight throw speed along the crosshair (2236 uu/s when this was written
// at a 3000 base; 2161.5 uu/s at Patch 28 §4's 2900 — the POINT is that it was the same number every
// time, whatever the number is).
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
// MEASURED WHEN WRITTEN, at a 3000 base (-> 2236 after the weight model, up bias 0.12 -> 0.29 after
// weight, so an impulse of 2236 forward + 649 up = 2328 uu/s); see the numbers in that pass report.
// AT PATCH 28 §4's 2900 BASE the same impulse is 2161.5 forward + 626 up = 2250 uu/s. Neither the
// argument nor the sign of anything above depends on which it is. Sprint is ~900 uu/s and the jump
// apex path is ~+560 uu/s Z at release, so a fall still dominates the term either way.
// =================================================================================================

static TAutoConsoleVariable<float> CVarModeBThrowInheritance(
	TEXT("Trace.ModeB.ThrowVelocityInheritance"),
	1.f,
	TEXT("MODE B, spec v8 §4. Fraction of the THROWER'S OWN velocity added to a thrown Core's launch ")
	TEXT("velocity, vertical included, so a jumping throw carries the jump and a sprinting throw ")
	TEXT("carries the sprint. 1 = full inheritance (the default), 0 = the pre-v8 throw that inherited ")
	TEXT("nothing. UTraceSettings::CoreThrowVelocityInheritance."),
	ECVF_Cheat);

// =================================================================================================
// *** SPEC v31 §2 — "WHEN A PLAYER IS GOING DOWN THE CORE JUST DROPS INSTEAD OF GOING FORWARD."
// ***
// *** THE OWNER DIAGNOSED THIS AND THE DIAGNOSIS IS CORRECT. THE COMMENT DIRECTLY ABOVE IS THE BUG.
// =================================================================================================
//
// Read the block above again: "WHY VERTICAL IS NOT SPECIAL-CASED ... A jumping throw is the one the
// note names, and its whole character is that the Core leaves with the jump still in it." Every word
// of that is true about a RISING thrower and none of it is true about a FALLING one. The rule was
// written from the one case spec v8 §4 named, and the opposite sign of the same term was never
// considered — so the fix for "a jump is thrown away" quietly created "a fall is thrown INTO the
// floor". Both are the same line of code; only the sign differs.
//
// THE ARITHMETIC, at the shipped values (throw speed 2900 base -> 2161.5 uu/s after the weight
// model, up bias 0.12 -> 0.289794 after weight, so an impulse of 2161.5 forward + 626 up).
// PATCH 28 §4 MOVED THE BASE 3300 -> 2900 AND THESE FIGURES WITH IT; they were last correct for a
// 3000 base (2236 forward + 649 up) and had not been re-derived when the base went to 3300, which
// is worth knowing before trusting any other worked example in this file:
//
//   standing        launch Z = +626                       ... the throw arcs
//   jump apex       launch Z = +626 + 560 = +1186         ... spec v8 §4's case, and it is right
//   falling  600    launch Z = +626 -  600 =   +26        ... flat, and it looks like a fumble
//   falling 1200    launch Z = +626 - 1200 =  -574        ... AIMED AT THE GROUND. "It just drops."
//
// AND IT EXPLAINS LAST PATCH'S UNEXPLAINED SPREAD. A full charge was measured leaving between 2561
// and 3051 uu/s with nothing in the code able to account for a 490 uu/s band. The band IS the
// thrower's vertical velocity: a 490 uu/s difference in Z, at a launch whose other components are
// fixed, is exactly what a half-second of fall does to |launch|. Nobody could explain it because
// everybody was looking at the charge, and the charge was innocent.
//
// ONE KNOB, EXPRESSED RELATIVE TO THE INHERITANCE IT MODIFIES (the project's standing rule): it is a
// multiplier on the DOWNWARD half of the existing inheritance, not a second independent fraction.
// 0 (default) = a fall contributes nothing vertically and the horizontal term is untouched, which is
// both halves of what the owner asked for. 1 = the pre-v31 launch, exactly, in the same binary — the
// RED ARM, and Trace.ModeB.MomentumTestNow prints both arms from ONE throw so the before/after cannot
// be two different runs of two different builds.
//
// NOT `ECVF_Cheat`: it is a feel knob, and a playtester must be able to A/B it.
// =================================================================================================

static TAutoConsoleVariable<float> CVarModeBThrowInheritanceDown(
	TEXT("Trace.ModeB.ThrowVelocityInheritanceDown"),
	0.f,
	TEXT("MODE B, spec v31 §2. Multiplier applied to Trace.ModeB.ThrowVelocityInheritance for the ")
	TEXT("DOWNWARD part of the thrower's velocity only. 0 (default): a player who throws while falling ")
	TEXT("no longer has the fall subtracted from the launch - the Core leaves with the same Z a ")
	TEXT("standing throw gives it, and still carries the whole horizontal motion. 1: the pre-v31 ")
	TEXT("behaviour, which is the bug ('the core just drops'). Rising velocity is never touched - ")
	TEXT("spec v8 §4's jumping throw still carries the jump in full. ")
	TEXT("UTraceSettings::CoreThrowVelocityInheritanceDown."),
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
//                             0.15 of the post-weight 2161.5 uu/s is ~324 uu/s (it was ~335 before
//                             Patch 28 §4), which travels a couple of metres and lands in front of
//                             you — visibly a fumble, unmistakably a throw.
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
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBThrowChargeFloor(
	TEXT("Trace.ModeB.ThrowChargeFloor"),
	0.15f,
	TEXT("MODE B, spec v13 §6. Fraction of full throw impulse an INSTANT CLICK leaves with. 'Very low' ")
	TEXT("but deliberately not zero - a zero-momentum throw drops at the thrower's feet and reads as a ")
	TEXT("bug. UTraceSettings::CoreThrowChargeFloor."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBThrowChargeMax(
	TEXT("Trace.ModeB.ThrowChargeMax"),
	1.f,
	TEXT("MODE B, spec v13 §6. Fraction of full throw impulse a FULL hold leaves with. 1.0 means a full ")
	TEXT("charge is exactly the pre-v13 throw, which is what 'reach the current core throw momentum' ")
	TEXT("asks for. UTraceSettings::CoreThrowChargeMax."),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarModeBThrowChargeClamp(
	TEXT("Trace.ModeB.ThrowChargeClamp"),
	1,
	TEXT("MODE B, spec v13 §6. 1 (default): holding past Trace.ModeB.ThrowChargeSeconds adds nothing. ")
	TEXT("0: the linear ramp keeps going, so a long hold throws harder than a full charge."),
	ECVF_Cheat);

/**
 * SPEC v28 §7. The window between FULL CHARGE and the automatic release, in seconds.
 *
 * 0.6, which is the number the owner gave. It is a SECOND, INDEPENDENT clock and not a re-spelling of
 * the charge time: CoreThrowChargeSeconds also ships at 0.6 today and the two are equal by
 * coincidence, so they are deliberately separate knobs. Retuning the wind-up must not silently retune
 * how long a full charge may be sat on, and vice versa.
 *
 * 0 SWITCHES THE WHOLE OF §7 OFF — no auto-release and no red ring — which is the RED ARM: the
 * pre-v28 game, in the same binary, one console line away.
 *
 * Bound by name to UTraceSettings::CoreThrowFullChargeAutoReleaseSeconds through Resolve(), like the
 * other four charge knobs. That property does not exist yet (UTraceSettings is another agent's file
 * this pass), and Resolve() answers with this CVar when a name is missing — so the shipped value is
 * 0.6 today and an ini value takes over automatically the moment the property is added, with no edit
 * here. See the note in TraceCore.h.
 */
static TAutoConsoleVariable<float> CVarModeBThrowAutoReleaseSeconds(
	TEXT("Trace.ModeB.ThrowAutoReleaseSeconds"),
	0.6f,
	TEXT("MODE B, spec v28 §7. Seconds a FULL throw charge may be held before the server throws it ")
	TEXT("automatically at full charge. The red ring inside the green one draws exactly this window, ")
	TEXT("off the server's own clock. 0 = off (the pre-v28 behaviour: a full charge can be held ")
	TEXT("forever). UTraceSettings::CoreThrowFullChargeAutoReleaseSeconds."),
	ECVF_Cheat);

/**
 * *** SPEC v29 §6 — THE FIX FOR "A FULL THROW CHARGE SOMETIMES DOES NOT GO FULL DISTANCE", AND ITS
 * *** RED ARM.
 *
 * 1 (default, v29 §6): the server anchors the charge clock at the CLIENT'S STAMPED PRESS INSTANT,
 *                      clamped into [ServerNow - MaxRewindTime, ServerNow]. The hold the launch is
 *                      computed from is then (server's release arrival - the player's own press),
 *                      so upstream jitter can only ever make a hold LONGER, and longer clamps at
 *                      full. A player who watches the ring fill and lets go gets a full throw.
 * 0 (the RED ARM, pre-v29): anchor at the instant the PRESS RPC ARRIVED, which is what shipped. The
 *                      hold is then (true hold + release lag - press lag) and the jitter term is
 *                      signed. This is the arm that reproduces the bug; Trace.ModeB.ThrowSpread run
 *                      with a jitter argument prints the distribution under each.
 *
 * *** `ECVF_Cheat` SINCE W9-SHIPGUARD. *** It used to say "NOT `ECVF_Cheat`: ... a playtester must be
 * able to A/B it without a cheat-enabled build". A playtester still can — ECVF_Cheat is inert
 * wherever DISABLE_CHEAT_CVARS is 0, i.e. every configuration except Shipping and Test, so the
 * console and -ExecCmds reach this switch on every build Trace.ModeB.ThrowSpread is run on. The flag
 * closes the one path left into a SHIPPED build: Engine.ini's [ConsoleVariables] section, applied by
 * LoadConsoleVariablesFromINI with bAllowCheating = false, player-writable in a packaged game. The
 * rest of the paragraph stands, and it is why this arm was never the dangerous one: the security is
 * in the CLAMP, not in this switch — see ATraceCore::ServerSetPassInput.
 */
static TAutoConsoleVariable<int32> CVarModeBThrowChargeAnchorAtPress(
	TEXT("Trace.ModeB.ThrowChargeAnchorAtPress"),
	1,
	TEXT("SPEC v29 s6. 1 (default): the throw charge is measured from the CLIENT'S STAMPED PRESS ")
	TEXT("(clamped into the gun's rewind window), so upstream jitter cannot shorten a full charge. ")
	TEXT("0 = RED ARM: anchor at the press RPC's ARRIVAL, the pre-v29 behaviour that produced the ")
	TEXT("intermittent short throw."),
	ECVF_Cheat);

/**
 * *** SPEC v29 §6, SECOND HALF — A SECOND PRESS EDGE MUST NOT RESTART A RUNNING THROW CHARGE. ***
 *
 * 1 (default, v29 §6): a press that arrives while this holder is already winding up is ABSORBED.
 *                      The charge keeps the anchor its FIRST press gave it, because the finger has
 *                      not left the button — only a release ends a charge, and a release clears it.
 * 0 (the RED ARM, pre-v29): the second press restarts the clock. Measured on the auto-release arm of
 *                      Trace.ModeB.ThrowSpread: three of thirty throws left with 0.38-0.44 s of hold
 *                      against a 1.20 s wind-up nobody interrupted, i.e. x0.69-x0.78 instead of
 *                      x1.00 — visibly and intermittently short, which is the owner's report.
 *
 * SEPARATE FROM Trace.ModeB.ThrowChargeAnchorAtPress ON PURPOSE. They are two different mechanisms
 * that produce the same symptom, and one switch covering both could not tell a tester which one
 * their build is suffering from.
 */
static TAutoConsoleVariable<int32> CVarModeBThrowChargeKeepOnRepress(
	TEXT("Trace.ModeB.ThrowChargeKeepOnRepress"),
	1,
	TEXT("SPEC v29 s6. 1 (default): a second press edge during a wind-up is ABSORBED and the charge ")
	TEXT("keeps its original anchor. 0 = RED ARM: the pre-v29 behaviour, where the second press ")
	TEXT("restarts the clock and silently shortens the throw."),
	ECVF_Cheat);

/**
 * SPEC v28 §2. WHICH EVENT THE CoreTurnover SOUND HANGS OFF. The A/B arm for the whole section.
 *
 * 1 (default, v28 §2): the moment a team STOPS HOLDING THE CORE — the carrier throws it, or the
 *                      carrier is killed / leaves. Nothing else makes the sound.
 * 0 (the RED ARM, pre-v28): the moment ATraceCore::RegisterTurnover fires, i.e. once a thrown Core
 *                      has settled on the ground and the five-second lockout OPENS. A kill is silent.
 *
 * Two values and no "both": the whole complaint is that one sound was on two different edges in the
 * player's ear, and an arm that could play it twice would be unable to demonstrate the fix.
 */
static TAutoConsoleVariable<int32> CVarAudioTurnoverEdge(
	TEXT("Trace.Audio.TurnoverEdge"),
	1,
	TEXT("SPEC v28 §2. 1 (default): the CoreTurnover sound plays when the carrier DROPS the Core or is ")
	TEXT("KILLED. 0 = RED ARM: the pre-v28 edge, on the landing that opens the lockout, with no sound ")
	TEXT("at all on a kill."),
	ECVF_Default);

#if !UE_BUILD_SHIPPING
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
TAutoConsoleVariable<int32> CVarModeBFlightLog(
	TEXT("Trace.ModeB.FlightLog"),
	0,
	TEXT("MODE B. 1: every machine logs its OWN view of the loose Core (position, velocity, speed) at ")
	TEXT("10 Hz while it is in flight. Run it on the client as well as the server."),
	ECVF_Default);
#endif // !UE_BUILD_SHIPPING

static TAutoConsoleVariable<float> CVarModeBBounce(
	TEXT("Trace.ModeB.Bounce"),
	0.35f,
	TEXT("MODE B. Restitution of a loose Core against world geometry. 0 = dead stop, 1 = perfect bounce."),
	ECVF_Cheat);

// =================================================================================================
// DEMO 27 — *** THE THROW THAT DROPS AT YOUR FEET WHEN YOU THROW IT WHILE RUNNING. ***
//
// "There is still a bug when throwing the core. It doesn't seem to throw forward when moving
// forward. Now, it drops to the ground whenever a player is moving forward and throwing."
//
// THE LAUNCH WAS NEVER THE PROBLEM, and that is why the previous pass did not fix it. The throw's
// own log line, printed on one of the failing throws, reads
//
//     [ModeB] THROW ... at 3509 uu/s ... charged impulse 2561 + inherited 976 (thrower 976 uu/s
//     horiz, +0 vert, grounded, x1.00), launch Z +713
//
// - the forward run is IN there, added, exactly as spec v8 §4 says. What ate it was the very first
// integration frame afterwards:
//
//     [ModeBTurnover] CONTACT at (-17836.07, 0.00, 154.15) | 154 uu above the floor |
//         speed 3504 -> 681 uu/s | normal (0.98, 0.00, 0.20) | airborne 0.000s | VERDICT: WALL - bounce
//
// A "wall" AT THE LAUNCH POINT, ON THE LAUNCH FRAME, whose normal points along the throw. There is
// no wall there. That normal is the direction from the THROWER'S OWN CAPSULE to the muzzle: the
// launch is 70 uu ahead of the eye and 10 uu above the top hemisphere's centre, and against a 34 uu
// capsule plus the Core's 22 uu sphere that clears by 14.7 uu — until the thrower moves. At 976 uu/s
// a frame carries them ~20 uu, which turns +14.7 into -5 and the sweep into a start-penetrating hit.
// (Solve the same triangle at 20 uu of advance and the normal comes out (0.98, 0.20). It is the
// capsule, to two decimals.) The velocity was then MIRRORED about that normal — i.e. reversed, since
// the normal is the flight direction — and rescaled by the bounce, four times over, 3504 -> 681 ->
// 133 -> 25 -> 5 uu/s, and the Core dropped where the thrower was standing.
//
// WHY THE SWEEP COULD SEE A PAWN AT ALL, which is the actual defect. ServerTickLooseCore asked for
//
//     SweepSingleByChannel(..., ECC_WorldStatic, ...)
//
// under a comment reading "against static world geometry ... Pawns are deliberately not swept
// against". THAT IS NOT WHAT THAT CALL DOES. ECC_WorldStatic in that position is a TRACE CHANNEL,
// not a filter on object types, and the question it asks of every component is "do you BLOCK the
// WorldStatic channel" — which a character capsule (collision profile "Pawn") answers yes. The
// comment described the intent; the call never implemented it.
//
// TWO THINGS ARE FIXED HERE AND THEY ARE SEPARATE, because either one alone leaves a hole:
//
//   1. THE SWEEP NO LONGER ACCEPTS A PAWN AS A SURFACE (TraceModeBTuning::SweepLooseCore). The
//      channel query stays exactly as it was — every arena surface behaves identically — but a
//      blocking hit on a pawn body is rejected and the sweep re-asked past it. Pawns are out: the
//      thrower's, and everybody else's, which is what "first contact takes it is resolved by the
//      pickup poll" has always required.
//   2. A CONTACT WHOSE NORMAL POINTS THE WAY THE CORE IS ALREADY TRAVELLING NO LONGER REFLECTS IT.
//      That is a depenetration, not an impact, and mirroring it turns an exit into an entry. Any
//      future launch point that ends up inside a lip of geometry now flies out of it instead of
//      being fired backwards.
//
// THE RED ARM IS `Trace.ModeB.FlightHitsPawns 1`. It restores the channel query and NOTHING ELSE -
// fix 2 stays in - so Trace.ModeB.RunThrowTest can be run against either behaviour in one build.
//
// AND THE SHAPE OF ITS FAILURE IS WORTH KNOWING, because it says the two fixes are not alternatives.
// With the sweep put back but the mirror still guarded, the measured red run reads
//
//     CONTACT at (-17382.97, 2880.00, 154.15) with TraceCharacter_0 (CollisionCylinder) | speed
//         4593 -> 4593 uu/s | normal (0.98, 0.00, 0.20) | airborne 0.011s | VERDICT: DEPENETRATION
//
// on frame after frame: the Core keeps its whole velocity and is shoved out of the capsule every
// tick, so instead of dropping at the thrower's feet it is CARRIED ALONG IN FRONT OF THEM - 19 uu
// clear after a tenth of a second, when a clean throw is 270. Fix 2 alone turns "it falls at my
// feet" into "it sticks to my face". Only fix 1 makes the throw leave.
// =================================================================================================
TAutoConsoleVariable<int32> CVarModeBFlightHitsPawns(
	TEXT("Trace.ModeB.FlightHitsPawns"),
	0,
	TEXT("MODE B, Demo 27. 1: RED ARM - let the loose Core's sweep accept a PAWN as a surface again, ")
	TEXT("as the plain channel query did (the thrower's own capsule, on the launch frame). 0: a ")
	TEXT("blocking hit on a body is rejected and the sweep re-asked past it. The depenetration guard ")
	TEXT("is NOT part of this arm. Trace.ModeB.RunThrowTest FAILS on 1 and PASSES on 0."),
	ECVF_Cheat);

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
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBCatchCurve(
	TEXT("Trace.ModeB.CatchCurve"),
	6.f,
	TEXT("MODE B. How hard the catch zone curves the Core, in fractions of the remaining aim error per ")
	TEXT("second at point-blank range. 0 disables the magnet outright. UTraceSettings::CoreCatchCurveStrength."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBCatchThrowerLockout(
	TEXT("Trace.ModeB.CatchThrowerLockout"),
	0.5f,
	TEXT("MODE B. Seconds the THROWER alone is excluded from their own catch zone, so a throw does not ")
	TEXT("curve straight back into the hands it left. UTraceSettings::CoreCatchThrowerLockoutSeconds."),
	ECVF_Cheat);

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
	ECVF_Cheat);

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
	ECVF_Cheat);

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
	ECVF_Cheat);

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
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBLandingMinFlight(
	TEXT("Trace.ModeB.LandingMinFlightSeconds"),
	0.05f,
	TEXT("MODE B, spec v13 §8. Seconds a throw must have been in the air before a CONTACT can be read as ")
	TEXT("a landing, so a sweep that clips geometry on the launch frame - before the Core has cleared ")
	TEXT("the thrower - cannot hand possession away. The at-rest probe is not gated by this, so a Core ")
	TEXT("genuinely thrown into the floor still turns over a frame later."),
	ECVF_Cheat);

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
	ECVF_Cheat);

/**
 * SPEC v13 §8. Is the pre-v13 rule armed?
 *
 * TWO WAYS IN, and the command-line one is not a convenience. -ExecCmds is comma-separated and a
 * console variable assignment needs a SPACE ("Trace.ModeB.LandingRule 0"), which means quoting - and
 * quoting inside -ExecCmds has already, on this project, broken a command line into the URL parser
 * and produced a verification that "passed" because its commands never ran. A bare switch cannot do
 * that. The CVar stays for a live A/B from the console.
 */
bool TraceModeBLegacyLandingRule()
{
#if UE_BUILD_SHIPPING
	// RESTRUCTURE B3's pattern (IsV9LegacyTuning / IsV10LegacyWallJump), applied here in wave 3
	// because this file was not owned in wave 1. A legacy arm is A/B evidence, not a player option:
	// a bare -TraceLegacyLanding on ONE machine of a live session would run the pre-v13 landing rule
	// against peers running the shipped one, and a turnover rule that differs per machine is the
	// worst class of desync this game can have. Trace.ModeB.LandingRule itself is ECVF_Cheat (see the
	// banner above the tuning block), which closes the console door; this closes the command-line
	// door. Development is untouched.
	return false;
#else
	static const bool bFromCommandLine = FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyLanding"));
	return bFromCommandLine || (CVarModeBLandingRule.GetValueOnAnyThread() == 0);
#endif
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
TAutoConsoleVariable<float> CVarModeBMidAirTurnoverSpeed(
	TEXT("Trace.ModeB.MidAirTurnoverSpeed"),
	800.f,
	TEXT("MODE B, spec v13 §8, REPORTING ONLY. Part one of the mid-air test: the Core must still have ")
	TEXT("been travelling at least this fast when possession changed. No rule reads it."),
	ECVF_Cheat);

/**
 * Part two, and the part that makes the counter mean what it says.
 *
 * A Core dropped straight onto the floor arrives at 1500 uu/s and has unambiguously LANDED, so speed
 * on its own would call the commonest correct turnover in the game a bug. What the user described is
 * a Core going PAST something: fast AND barely descending. Deliberately a separate number from the
 * rule's own LandingMinDescentDegrees so the counter is not merely a restatement of the rule - it
 * measures the event, and both arms of the A/B measure it the same way.
 */
TAutoConsoleVariable<float> CVarModeBMidAirTurnoverDegrees(
	TEXT("Trace.ModeB.MidAirTurnoverDegrees"),
	15.f,
	TEXT("MODE B, spec v13 §8, REPORTING ONLY. Part two of the mid-air test: the Core must have met the ")
	TEXT("surface at a shallower angle than this - i.e. it was flying past, not coming down. A turnover ")
	TEXT("that is both fast and shallow is counted as MID-AIR in SurfaceStats. No rule reads it."),
	ECVF_Cheat);

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

TAutoConsoleVariable<float> CVarModeBTurnoverContactSlack(
	TEXT("Trace.ModeB.TurnoverContactSlack"),
	6.f,
	TEXT("MODE B, spec v19 §1.5. How much clear air may be under the VISIBLE orb at the instant "
	     "possession changes, in uu. The Core parks 2 uu off whatever it lands on and its collision "
	     "sphere is 2 uu larger than the drawn one, so a ball genuinely sitting on the floor measures "
	     "about 4; 6 is that plus a frame of slack. Raise it and mid-air turnovers come back."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBTurnoverContactProbe(
	TEXT("Trace.ModeB.TurnoverContactProbe"),
	600.f,
	TEXT("MODE B, spec v19 §1.5. How far below the Core to look for the surface holding it up, in uu. "
	     "Also the number reported as the gap when there is nothing under it at all, so it wants to be "
	     "comfortably taller than the arena's cover (the tallest top face here is ~350 uu)."),
	ECVF_Cheat);

TAutoConsoleVariable<float> CVarModeBTurnoverSettleSeconds(
	TEXT("Trace.ModeB.TurnoverSettleSeconds"),
	0.15f,
	TEXT("MODE B, spec v19 §1.5. How long the Core stays loose ON the surface it landed on before "
	     "possession changes, in seconds. Before this pass it was 0 and the award happened on the same "
	     "TICK as the first contact, so the ball was never drawn touching anything - which is the "
	     "reported bug. 0.15 is about nine frames at 60fps and five at 30, i.e. legible at any frame "
	     "rate, and the ball is bouncing or resting for all of them. 0 restores the pre-v19 instant "
	     "award."),
	ECVF_Cheat);

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
	ECVF_Cheat);

/** As the v13 switches: also armable with a bare -TraceLegacyGroundedTurnover, which needs no quoting. */
bool TraceModeBLegacyGroundedRule()
{
#if UE_BUILD_SHIPPING
	// Same rule as TraceModeBLegacyLandingRule above (RESTRUCTURE B3's pattern): the pre-v19 arm is
	// Development A/B evidence, and "never ship 0" — which the CVar's own help text already says —
	// is a great deal more convincing when the switch cannot be reached in a shipped build at all.
	return false;
#else
	static const bool bFromCommandLine =
		FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyGroundedTurnover"));

	// The v13 arm implies this one — see the CVar's comment.
	return bFromCommandLine
		|| (CVarModeBGroundedTurnover.GetValueOnAnyThread() == 0)
		|| TraceModeBLegacyLandingRule();
#endif
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
// MEASURED at the default M = 1.8 against the four base knobs AS THEY WERE WHEN THE WEIGHT MODEL
// LANDED (3000 uu/s, bias 0.12, gravity 0.55, bounce 0.35), for a flat throw from eye height. This
// table is the WEIGHT's before/after and is left at its original base on purpose — it is what M=1.8
// did, not what the game currently throws:
//
//                      M=1 (pre-v5)  M=1.8
//   launch speed       3000          2236 uu/s
//   gravity            539           970  uu/s^2
//   flat range         ~5000         ~3400 uu
//   apex above launch  ~120          ~215 uu
//
// THE BASE HAS SINCE MOVED TWICE (3000 -> 3300 -> 2900 at Patch 28 §4). At today's 2900 the same
// M = 1.8 column reads launch 2161.5 uu/s and flat range ~2791 uu; gravity and apex are unchanged
// by the base, because neither depends on it.
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
	ECVF_Cheat);

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
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBPullHold(
	TEXT("Trace.ModeB.PullHoldSeconds"),
	0.3f,
	TEXT("SPEC v25 §2, GOALS MODE. Seconds of CONTINUOUS right-mouse-plus-hover-plus-line-of-sight that ")
	TEXT("complete a pull. Losing any of the three cancels the fill outright; it does not pause. ")
	TEXT("UTraceSettings::CorePullHoldSeconds."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBPullAimCone(
	TEXT("Trace.ModeB.PullAimConeDegrees"),
	4.f,
	TEXT("SPEC v25 §2. Half-angle of the cone that counts as 'hovering the mouse over the core' at ")
	TEXT("range. Either this OR the slack below is enough. UTraceSettings::CorePullAimConeDegrees."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBPullAimSlack(
	TEXT("Trace.ModeB.PullAimSlackUU"),
	60.f,
	TEXT("SPEC v25 §2. How far, in uu, the aim ray may miss the orb's SURFACE and still count as a ")
	TEXT("hover. Added to the orb's drawn radius, so it is forgiveness rather than a second radius. ")
	TEXT("UTraceSettings::CorePullAimSlackUU."),
	ECVF_Cheat);

static TAutoConsoleVariable<float> CVarModeBPullMaxRange(
	TEXT("Trace.ModeB.PullMaxRangeUU"),
	0.f,
	TEXT("SPEC v25 §2. Optional ceiling on how far away a pull may be started, uu. 0 = no limit, which ")
	TEXT("is the shipped value because the note states no range: a 20 uu orb under a crosshair WITH ")
	TEXT("line of sight is already its own range limit. UTraceSettings::CorePullMaxRangeUU."),
	ECVF_Cheat);

TAutoConsoleVariable<float> CVarModeBTurnoverBeamScale(
	TEXT("Trace.ModeB.TurnoverBeamScale"),
	2.2f,
	TEXT("SPEC v25 §2/§3. How much LARGER the Core's beam is during the turnover window, as a ")
	TEXT("MULTIPLIER of its normal width - not a width, so a retune of the normal beam carries. ")
	TEXT("1 = no change. UTraceSettings::CoreTurnoverBeamScale."),
	ECVF_Cheat);

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
	ECVF_Cheat);

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

	// --- Spec v31 §2. The DOWNWARD half of the same term, as a multiplier on it. Clamped to [0,1]:
	// above 1 a fall would be AMPLIFIED, which is the reported bug made worse and is not a variant
	// anybody wants reachable by a typo. Bound by name to UTraceSettings::CoreThrowVelocityInheritance-
	// Down; that property does not exist yet (UTraceSettings is not this pass's file) and Resolve()
	// answers with the CVar when a name is missing, so the shipped value is 0 today and an ini value
	// takes over automatically the moment the property is declared, with no edit here. Exactly the
	// arrangement CoreThrowFullChargeAutoReleaseSeconds shipped under, for the same reason.
	float ThrowInheritanceDown() { return Resolve(TEXT("CoreThrowVelocityInheritanceDown"), CVarModeBThrowInheritanceDown, 0.f, 1.f); }

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

	/**
	 * SPEC v28 §2's RED ARM. True restores the pre-v28 edge: the sound on the LANDING, and silence
	 * on a kill. Console only — there is no settings property, because which EVENT a sound belongs to
	 * is a design decision in code, exactly as ETraceSoundSide is (Audio/TraceSoundEvents.h).
	 */
	bool LegacyTurnoverSoundEdge() { return CVarAudioTurnoverEdge.GetValueOnAnyThread() == 0; }

	// --- SPEC v28 §7. The full-charge auto-release window. Lower bound 0 and not 0.01: zero is a
	// MEANING here ("no auto-release, the pre-v28 game") and clamping it up to a hundredth of a second
	// would turn the red arm into the fastest possible auto-release, which is the exact mistake
	// LooseResetSeconds' comment above records having made once already.
	float ThrowAutoReleaseSeconds() { return Resolve(TEXT("CoreThrowFullChargeAutoReleaseSeconds"), CVarModeBThrowAutoReleaseSeconds, 0.f, 10.f); }

	// --- SPEC v29 §6. A pure switch with no ini twin, deliberately: it is not a tuning value, it is
	// which of two clocks the charge is measured on, and there is exactly one right answer. It exists
	// so the bug can be reproduced in the shipping binary rather than only in a git revert.
	bool ThrowChargeAnchorsAtPress() { return CVarModeBThrowChargeAnchorAtPress.GetValueOnAnyThread() != 0; }

	// --- SPEC v29 §6, the second half. Same shape and the same reason: not a tuning value, and the
	// red arm has to exist in the shipping binary or "reproduce first" is a git revert.
	bool ThrowChargeKeepsOnRepress() { return CVarModeBThrowChargeKeepOnRepress.GetValueOnAnyThread() != 0; }

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
		bool* bOutHysteresisHeld)   // the `= nullptr` default is on the declaration in TraceCoreInternal.h
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

	/** DEMO 27. How many pawn bodies one sweep may reject before it gives up. See SweepLooseCore. */
	constexpr int32 TraceModeBMaxPawnRejections = 5;

	/**
	 * DEMO 27. *** THE ONE WORLD QUERY THE LOOSE CORE MAKES. ***
	 *
	 * Its flight sweep, its resting-surface probe and its visible-support probe all come through
	 * here, because all three are asking the same question - "what geometry is the Core touching" -
	 * and the file already insists in two places that they must not be able to disagree about it.
	 *
	 * WHAT IT ADDS TO THE OLD CALL IS ONE SENTENCE: A PAWN IS NOT GEOMETRY. Everything else about the
	 * query is deliberately unchanged - same channel, same sphere, same ignore list, same responses -
	 * because the old sweep's treatment of every SURFACE in the arena was correct and this pass has
	 * no business retuning it.
	 *
	 * WHY IT IS A RE-SWEEP AND NOT AN OBJECT-TYPE QUERY, WHICH IS WHAT THIS WAS FIRST WRITTEN AS AND
	 * WHICH MEASURABLY BROKE SOMETHING ELSE. `SweepSingleByObjectType(WorldStatic|WorldDynamic)` also
	 * excludes pawns, in one call, and it reads better. But an object query treats EVERY component of
	 * a listed type as blocking and never looks at that component's own responses - so ATraceEndzone's
	 * trigger box (WorldDynamic, QueryOnly, every channel set to Ignore, overlapping pawns only)
	 * became a wall. The first green run of this fix caught it in the act:
	 *
	 *     CONTACT at (-17120.00, 0.00, 276.78) with TraceEndzone_2 (Trigger) | speed 3472 -> 675 uu/s
	 *
	 * - a throw bouncing off an invisible volume it had passed straight through for thirty demos. So
	 * the channel query stays, responses and all, and the pawns are filtered out of its ANSWER: if
	 * the blocking hit is a pawn body, that actor is added to the ignore list and the sweep is asked
	 * again, which is what the caller wanted in the first place.
	 *
	 * The re-sweep costs nothing on the overwhelming majority of frames, because a flight that hits
	 * nothing hits no pawn either. It is bounded by TraceModeBMaxPawnRejections so a wall of players
	 * cannot turn one sweep into a loop.
	 */
	bool SweepLooseCore(const UWorld& World, FHitResult& OutHit, const FVector& Start, const FVector& End,
		float SphereRadius, const FCollisionQueryParams& Params)
	{
		const FCollisionShape Sphere = FCollisionShape::MakeSphere(SphereRadius);

		// THE RED ARM. Byte-for-byte the call this file made before Demo 27 - the pawn is left in the
		// answer - so a run against the old behaviour is one CVar away and needs no second build.
		if (CVarModeBFlightHitsPawns.GetValueOnAnyThread() != 0)
		{
			return World.SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, ECC_WorldStatic,
				Sphere, Params);
		}

		FCollisionQueryParams Working(Params);

		for (int32 Rejections = 0; Rejections <= TraceModeBMaxPawnRejections; ++Rejections)
		{
			const bool bHit = World.SweepSingleByChannel(OutHit, Start, End, FQuat::Identity,
				ECC_WorldStatic, Sphere, Working);

			// Asked of the COMPONENT'S OBJECT TYPE rather than by casting the actor to APawn: what
			// blocks a sweep is a shape, and "this shape is a body" is exactly the fact being tested.
			// It also covers a pawn-typed collider hung on something that is not an APawn.
			const UPrimitiveComponent* const HitComponent = bHit ? OutHit.GetComponent() : nullptr;
			if (HitComponent == nullptr || HitComponent->GetCollisionObjectType() != ECC_Pawn)
			{
				return bHit;
			}

			Working.AddIgnoredActor(OutHit.GetActor());
		}

		// Six pawn bodies deep in one sweep. There is no such situation in a 5v5 match, and the
		// honest answer for a Core that has only ever met players is "nothing blocked it".
		OutHit = FHitResult();
		return false;
	}

	/** Below this speed the Core is declared at rest, so it stops jittering along the floor. */
	float RestSpeed()         { return 60.f * FMath::Clamp(MassScale(), 0.5f, 4.f); }

	/** How often the goal boxes are re-derived. Cheap, and only has to beat the half-time switch. */
	constexpr float GoalRefreshInterval = 0.5f;

	/**
	 * DEMO 27, AND THE KNOB THAT IS NO LONGER HERE.
	 *
	 * There was a LaunchAuditMinClearance on this spot: the shipped alarm asked how far the Core had
	 * got from the thrower and called anything under 150 uu the reported bug. IT DID NOT MEASURE THE
	 * BUG. Bounce() is 0.195, so every wall bounce loses about four fifths of the launch, and the
	 * clearance a slow throw has left is then a fact about how close the nearest pylon was - four of
	 * thirty-four throws on the fixed tree fired that Warning and all four were geometry. The audit
	 * asks what it HIT instead, which is a fact and not a proxy; see ATraceCore::ServerTickLaunchAudit
	 * and ATraceCore::LastContactActor. TraceModeBRunThrow::MinClearance keeps a clearance floor of
	 * its own, because the harness stages a RUNNING thrower and there the separation is the symptom.
	 */

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
			// Spec v31 §2. WAS the one to-do in this list until the release-hygiene pass declared
			// `float CoreThrowVelocityInheritanceDown = 0.f;` on UTraceSettings (default matching the
			// CVar, so shipped behaviour is unchanged). The entry now does the same standing-check work
			// as its neighbours: rename either side and mode-B start says so out loud.
			TEXT("CoreThrowVelocityInheritanceDown"),
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

#if !UE_BUILD_SHIPPING
TAutoConsoleVariable<int32> CVarCoreTeleportAudit(
	TEXT("Trace.Core.TeleportAudit"),
	0,
	TEXT("Trace: audit the Core's own transform across possession resets, on whatever machine it is set on.\n")
	TEXT("Prints one summary per reset: path length, frames spent moving, worst distance from home.\n")
	TEXT("0 = off (default), 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarCoreTeleportAuditJump(
	TEXT("Trace.Core.TeleportAuditJump"),
	400.f,
	TEXT("Trace: single-frame movement, in uu, above which Trace.Core.TeleportAudit logs a JUMP line."),
	ECVF_Default);
#endif // !UE_BUILD_SHIPPING

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

#if !UE_BUILD_SHIPPING
/** How many post-goal resets Trace.Core.GoalRepro should fire on its own. 0 = manual only. */
TAutoConsoleVariable<int32> CVarCoreGoalReproRuns(
	TEXT("Trace.Core.GoalReproRuns"),
	0,
	TEXT("Trace (spec v10 §10): fire this many staged post-goal resets automatically, once a REMOTE ")
	TEXT("client's pawn exists. 0 = off; drive it by hand with Trace.Core.GoalRepro instead."),
	ECVF_Default);

/**
 * Floor, not a stopwatch — the same lesson spec v8 §0 taught the momentum test. The run is released
 * by a remote client's pawn EXISTING; this only stops it firing into a half-loaded match.
 */
TAutoConsoleVariable<float> CVarCoreGoalReproDelay(
	TEXT("Trace.Core.GoalReproDelay"),
	25.f,
	TEXT("Trace: seconds before the first automatic Trace.Core.GoalReproRuns reset."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarCoreGoalReproInterval(
	TEXT("Trace.Core.GoalReproInterval"),
	12.f,
	TEXT("Trace: seconds between automatic resets. Must exceed the staging delay + the kickoff delay ")
	TEXT("+ Trace.Core.TeleportAuditWindow, or one audit window swallows the next reset."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarCoreTeleportAuditWindow(
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
#endif // !UE_BUILD_SHIPPING


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

	// THE BALL'S OWN NODE. Everything that IS the ball hangs off this and nothing else does, so the
	// one function that moves it into a hand (UpdateCarriedArtPlacement) writes exactly one
	// transform and can move nothing the game reads. See the header for the full argument, and note
	// what deliberately does NOT hang here: Beacon and ThrownTrailSegments.
	ArtRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ArtRoot"));
	ArtRoot->SetupAttachment(Root);
	ArtRoot->SetMobility(EComponentMobility::Movable);

	// NO COLLISION ANYWHERE ON THIS ACTOR. There is nothing to run into, nothing to catch, and
	// nothing that may ever eat a bullet meant for a player.
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(ArtRoot);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCollisionProfileName(TEXT("NoCollision"));
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetCastShadow(false);
	Mesh->bReceivesDecals = false;
	Mesh->SetRelativeScale3D(FVector(TraceCoreTuning::OrbScale));

	// HIDDEN FROM THE HOLDER'S OWN CAMERA *** WHILE IT FLOATS OVER THEIR HEAD ***, AND ONLY FROM
	// THEIRS. This is the STARTING state, not the last word: UpdateCarriedArtVisibility clears it
	// once the ball has been moved into the holder's hand and their own body is already drawn.
	//
	// The original argument still stands for the position this flag defends. A ball at OrbHeight is
	// 22.4 uu BELOW the carry lens and 450 uu ahead, i.e. 36 px under the crosshair on a 1600x900
	// frame - a bright emissive object dead centre in its own holder's view, the same class of defect
	// as the arena trim whiteout, and captured frames confirmed it: the orb filled the centre of the
	// holder's screen. What was WRONG was concluding from that that the holder must never see the
	// ball at all. Carrying the Core is the only thing that puts a player in third person, so the
	// flag made the objective invisible at the exact moment the camera pulled back to show them
	// holding it - photographed, banner and all, with nothing in the frame. The answer is to move the
	// ball off the crosshair and into a hand, and only then to show it.
	//
	// SetOwnerNoSee resolves through the ACTOR OWNER CHAIN, and GrantTo() SetOwner()s this actor to
	// its holder, so "the owner" is exactly the one player this is a decision about. Every other
	// client draws the full ball and the full beacon regardless. MarkDrawnPiecesRenderStateDirty()
	// re-dirties the render state whenever the holder changes, because the proxy caches that chain
	// when it is built.
	Mesh->SetOwnerNoSee(true);

	// --- SPEC v31 §4: the pack's Core, beside the fallback rather than instead of it ----------------
	//
	// Same collision, shadow, decal and owner-no-see policy as the orb it stands in for, one line at a
	// time rather than copied wholesale, because "no collision anywhere on this actor" is a rule and
	// not an accident.
	PackMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("PackMesh"));
	PackMesh->SetupAttachment(ArtRoot);
	PackMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PackMesh->SetCollisionProfileName(TEXT("NoCollision"));
	PackMesh->SetGenerateOverlapEvents(false);
	PackMesh->SetCanEverAffectNavigation(false);
	PackMesh->SetCastShadow(false);
	PackMesh->bReceivesDecals = false;
	PackMesh->SetOwnerNoSee(true);   // Same starting state, and the same later exception, as the orb
	                                 // above: hidden while it floats over the head, shown by
	                                 // UpdateCarriedArtVisibility once it is in a hand. THIS is the
	                                 // component that actually draws the ball whenever the pack art
	                                 // resolved, so it is the one the fix is really about.
	PackMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	// Three clips, one at a time, chosen by a state test - there is nothing for an AnimBlueprint to
	// blend and nothing for it to decide, so there is no AnimBlueprint and no .uasset to keep in step.
	PackMesh->SetVisibility(false);   // Until BeginPlay confirms the art actually resolved.

	HeartLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("HeartLight"));
	HeartLight->SetupAttachment(ArtRoot);   // The `heart` socket IS the mesh origin; see the pack notes.
	                                        // On ArtRoot with the ball, not on Root: a ULightComponent
	                                        // is not a UPrimitiveComponent and has no bOwnerNoSee, so
	                                        // this light is ALREADY lighting the holder's world. Left
	                                        // on Root it would keep doing it from 150 uu above their
	                                        // head while the ball it belongs to sat at their hip - a
	                                        // dark ball lit from nowhere.
	HeartLight->SetCastShadows(false);   // See the header: a shadowing light inside the shell is a
	                                     // light nobody outside the shell can see.
	HeartLight->SetLightColor(TraceCoreArt::AmberSRGB);
	HeartLight->SetAttenuationRadius(900.f);
	HeartLight->SetIntensity(0.f);       // Driven per-state by UpdateCoreArt.
	HeartLight->SetVisibility(false);

	Beacon = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Beacon"));
	Beacon->SetupAttachment(Root);
	Beacon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Beacon->SetCollisionProfileName(TEXT("NoCollision"));
	Beacon->SetGenerateOverlapEvents(false);
	Beacon->SetCanEverAffectNavigation(false);
	Beacon->SetCastShadow(false);
	Beacon->bReceivesDecals = false;
	// *** THE BEACON KEEPS THIS FOREVER, AND IT IS THE ONE PIECE THAT DOES. ***
	//
	// Its bottom is 205 uu above the actor centre, i.e. 32.6 uu ABOVE the carry camera: unhidden it
	// draws a 42-px-wide unlit emissive column from y=397 px to the top of a 900 px frame, straight
	// through the crosshair and the scoreboard. No bottom height fixes that - clearing it at level
	// pitch would need 449 uu, and the moment the player pitches up the camera rotates about a pivot
	// the column is welded to. It also tells the holder nothing the HUD banner does not already say.
	// It exists so that everyone ELSE can find them across a 24000 uu field.
	Beacon->SetOwnerNoSee(true);

	// --- SPEC v32 §3: the two pieces of FX GEOMETRY the FX doc asks for --------------------------
	//
	// Created here and hidden; the MESHES and the MATERIALS are resolved in BeginPlay out of
	// UTraceFxShapes, which is where the engine primitives are cached and where the ONE unit
	// conversion lives. Nothing about their size is decided in this constructor, because a size is
	// exactly the thing that has to come from the shared library rather than be retyped here.
	//
	// bOwnerNoSee to start with, and it travels with the ball: the halo is centred on the Core's heart
	// and fires on the frame a player TAKES it. Blooming out of the middle of the taker's own screen
	// was the defect the orb's own comment records having been captured doing - but the ball is not in
	// the middle of that screen any more, it is in a hand, so the halo goes with it and is unhidden on
	// exactly the same terms (UpdateCarriedArtVisibility). A halo that stayed hidden while the ball it
	// belongs to was shown would make the pickup read as the ball simply appearing.
	PickupHalo = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PickupHalo"));
	PickupHalo->SetupAttachment(ArtRoot);   // Centred on the ball's heart, so it follows the ball.
	UTraceFxShapes::ConfigureFxComponent(PickupHalo);
	PickupHalo->SetOwnerNoSee(true);
	PickupHalo->SetCanEverAffectNavigation(false);
	PickupHalo->SetVisibility(false);

	ThrownTrailSegments.Reserve(TraceCoreArt::ThrownTrailSegmentCount);
	for (int32 Index = 0; Index < TraceCoreArt::ThrownTrailSegmentCount; ++Index)
	{
		// NAMED subobjects, one per index, because CreateDefaultSubobject requires a unique name per
		// object and a duplicate silently returns the FIRST one - which would give three components
		// that are all the same component and a "taper" that is one cylinder drawn three times.
		const FName SegmentName(*FString::Printf(TEXT("ThrownTrailSegment%d"), Index));
		UStaticMeshComponent* Segment = CreateDefaultSubobject<UStaticMeshComponent>(SegmentName);
		Segment->SetupAttachment(Root);
		UTraceFxShapes::ConfigureFxComponent(Segment);
		Segment->SetOwnerNoSee(true);
		Segment->SetCanEverAffectNavigation(false);
		Segment->SetVisibility(false);
		ThrownTrailSegments.Add(Segment);
	}

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

	// SPEC v31 §4. Constructor-time finders for the same reason the engine shapes above use them: a
	// runtime LoadObject leaves no cook reference and resolves to null in a packaged build. Separate
	// static finders rather than a loop - ConstructorHelpers::FObjectFinder must be a distinct static
	// per path, which is the rule TraceCharacter.cpp records having learned the hard way.
	//
	// A MISSING ASSET IS NOT AN ERROR HERE. A fresh clone with no `git lfs pull` has no
	// /Game/Trace/Art/Pack at all; Succeeded() is false, the sphere stays, and the match is playable.
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> CoreMeshFinder(TraceCoreArt::MeshPath);
	if (CoreMeshFinder.Succeeded())
	{
		PackMesh->SetSkeletalMesh(CoreMeshFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UAnimSequence> CoreIdleFinder(TraceCoreArt::IdlePath);
	if (CoreIdleFinder.Succeeded())
	{
		ArtIdleAnim = CoreIdleFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UAnimSequence> CorePickupFinder(TraceCoreArt::PickupPath);
	if (CorePickupFinder.Succeeded())
	{
		ArtPickupAnim = CorePickupFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UAnimSequence> CoreThrowFinder(TraceCoreArt::ThrowPath);
	if (CoreThrowFinder.Succeeded())
	{
		ArtThrowAnim = CoreThrowFinder.Object;
	}

	// Same material policy as the trail and the arena, and the same MAP_PLAN §9 migration: the
	// COMMITTED unlit neon parent first (/Game/Trace/Materials/Parents, in the repository), the lit
	// engine basic material if even that is missing. No .uasset we author by hand is ever a hard
	// requirement. The pre-v17 /Game/Generated copy this used to load is gone from the tree.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Trace/Materials/Parents/M_TraceNeon.M_TraceNeon"));
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

	// SPEC v28 §7. The instant the server's own charge reached full. The red ring is arithmetic on
	// this and the shared clock, so the ring cannot show a deadline the server is not going to act on.
	DOREPLIFETIME(ATraceCore, ThrowFullChargeServerTime);
}

void ATraceCore::BeginPlay()
{
	Super::BeginPlay();

	SpawnHomeLocation = GetActorLocation();

	// Shader work is pointless (and unreliable - shaders are not cooked for server targets) on a
	// dedicated server.
	const bool bHasRenderer = GetNetMode() != NM_DedicatedServer;

	if (BaseMaterial != nullptr && bHasRenderer)
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

	// --- SPEC v31 §4: which Core is on screen, decided once and said out loud ----------------------
	//
	// THE FALLBACK IS A SUPPORTED PATH, NOT AN ERROR PATH. Three ways to reach it: the art is not on
	// disk (a clone with no `git lfs pull`), `-TraceNoCharacterArt` (so the procedural game can be
	// tested on a machine where the art DID import - the same switch TraceCharacter's own fallback
	// answers to), or Trace.Core.PackArt 0. All three land on the pre-v31 sphere, and the log says
	// which one happened, because "the Core looks wrong" is otherwise a five-minute question.
	const bool bArtOnDisk = PackMesh != nullptr && PackMesh->GetSkeletalMeshAsset() != nullptr
		&& ArtIdleAnim != nullptr && ArtPickupAnim != nullptr && ArtThrowAnim != nullptr;
	const bool bArtSwitchedOff = FParse::Param(FCommandLine::Get(), TEXT("TraceNoCharacterArt"));

	// bPackArtActive means AVAILABLE, not "currently drawn". Trace.Core.PackArt is deliberately left
	// out of it and applied per tick instead, so the A/B is live at the console rather than a restart:
	// the MIDs and the scale are set up either way and only the two visibilities move.
	bPackArtActive = bArtOnDisk && !bArtSwitchedOff && bHasRenderer;

	if (bPackArtActive)
	{
		// THE SCALE IS DERIVED FROM THE MESH ITSELF. See the ART block: the ball is drawn at exactly
		// the length the orb it replaces drew at, so every mode-B support-gap and turnover rule keeps
		// meaning what it meant, and a re-export at a different size cannot change the physics read.
		const FBoxSphereBounds Imported = PackMesh->GetSkeletalMeshAsset()->GetImportedBounds();
		PackArtMeshLengthX = static_cast<float>(FMath::Max(1.0, 2.0 * Imported.BoxExtent.X));
		PackArtScale = static_cast<float>(TraceCoreArt::TargetLengthUU) / PackArtMeshLengthX;

		PackMesh->SetRelativeScale3D(FVector(PackArtScale));
		PackMesh->SetVisibility(true);

		if (Mesh != nullptr)
		{
			Mesh->SetVisibility(false);
		}

		// The two emissive slots the FX notes drive by STATE. Found by substring: Interchange
		// decorates slot names, and a slot this loop fails to find is a glow that silently never
		// moves - so the failure is logged rather than left to be noticed in a screenshot.
		const TArray<FName> SlotNames = PackMesh->GetMaterialSlotNames();
		for (int32 Index = 0; Index < SlotNames.Num(); ++Index)
		{
			const FString SlotName = SlotNames[Index].ToString();
			if (CyanMID == nullptr && SlotName.Contains(TraceCoreArt::CyanSlot))
			{
				CyanMID = PackMesh->CreateDynamicMaterialInstance(Index, nullptr);
			}
			else if (AmberMID == nullptr && SlotName.Contains(TraceCoreArt::AmberSlot))
			{
				AmberMID = PackMesh->CreateDynamicMaterialInstance(Index, nullptr);
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Core] spec v31 §4: drawing SK_TraceCore. Mesh is %.1f uu long; drawn at %.1f uu ")
			TEXT("(x%.4f, = the %.0f uu orb it replaces). Emissive slots: cyan %s, amber %s. Rest spin ")
			TEXT("%.3f rev/s, flight spin %.2f rev/s (clip authors %.2f)."),
			PackArtMeshLengthX, TraceCoreArt::TargetLengthUU, PackArtScale,
			TraceCoreArt::TargetLengthUU,
			(CyanMID != nullptr) ? TEXT("found") : TEXT("NOT FOUND"),
			(AmberMID != nullptr) ? TEXT("found") : TEXT("NOT FOUND"),
			CVarCoreRestSpin.GetValueOnGameThread(), CVarCoreFlightSpin.GetValueOnGameThread(),
			TraceCoreArt::ThrowClipRevPerSecond);

		// --- SPEC v32 §3: the geometry the FX doc asks for, built out of the shared library --------
		//
		// MESHES AND MATERIALS HERE, NOT IN THE CONSTRUCTOR. UTraceFxShapes resolves its primitives on
		// its own CDO, and reaching into another class's CDO from a constructor that itself runs during
		// CDO creation is an ordering question nobody should have to answer. BeginPlay is well past all
		// of it and is where this actor already decides what it is drawing.
		//
		// TRANSLUCENT is what the FX doc asks for and Additive is what it resolves to - see
		// ETraceFxBlend::Translucent, which spells out why (this project has no translucent parent
		// material and a MID cannot change its parent's blend mode). That is the RIGHT degradation
		// here rather than a regrettable one: additive geometry writes NO DEPTH, so a 42 uu halo
		// cannot hide the 20 uu ball inside it and a trail cannot occlude the thing it trails. An
		// opaque halo would swallow the Core on the exact frame the pickup is meant to be readable.
		//
		// The blend that was ACHIEVED is stored, not the one that was asked for: SetGlow must be given
		// the achieved value or it writes a parameter the material does not have, which is a silent
		// no-op. UTraceFxShapes::MakeGlowMID's header is explicit about this and it is why it hands
		// the answer back.
		if (PickupHalo != nullptr)
		{
			PickupHalo->SetStaticMesh(UTraceFxShapes::GetIcosphere());
			PickupHaloMID = UTraceFxShapes::MakeGlowMID(PickupHalo, 0, ETraceFxBlend::Translucent, PickupHaloBlend);
		}

		ThrownTrailMIDs.Reset();
		for (UStaticMeshComponent* Segment : ThrownTrailSegments)
		{
			if (Segment == nullptr)
			{
				ThrownTrailMIDs.Add(nullptr);
				continue;
			}
			Segment->SetStaticMesh(UTraceFxShapes::GetCylinder());
			ETraceFxBlend Achieved = ETraceFxBlend::None;
			ThrownTrailMIDs.Add(UTraceFxShapes::MakeGlowMID(Segment, 0, ETraceFxBlend::Translucent, Achieved));
			// Every segment asks for the same blend off the same library, so they cannot honestly
			// differ; the LAST answer is recorded rather than the first so that a segment which failed
			// where its neighbours succeeded shows up as None instead of being hidden behind them.
			ThrownTrailBlend = Achieved;
		}

		// The taper's segment radii, PRINTED. §3's numbers are 5.5 -> 1.2 uu and a verifier will
		// measure them on screen; this is what the code believes it built, so the two can be compared
		// rather than one of them assumed. TaperSegmentRadiusUU is the same function that will place
		// them, so this cannot drift from the geometry.
		FString SegmentRadii;
		for (int32 Index = 0; Index < ThrownTrailSegments.Num(); ++Index)
		{
			SegmentRadii += FString::Printf(TEXT("%s%.2f"), (Index > 0) ? TEXT(", ") : TEXT(""),
				UTraceFxShapes::TaperSegmentRadiusUU(TraceCoreArt::ThrownTrailHeadRadiusUU,
					TraceCoreArt::ThrownTrailTailRadiusUU, Index, ThrownTrailSegments.Num()));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Core] SPEC v32 §3 geometry: pickup halo r %.1f uu (%s, mesh %s%s) blend %s; thrown ")
			TEXT("trail r %.1f -> %.1f uu in %d segments [%s] blend %s, apex length %.0f uu."),
			TraceCoreArt::PickupHaloRadiusUU,
			(PickupHaloMID != nullptr) ? TEXT("MID ok") : TEXT("NO MID - halo will stay hidden"),
			*GetNameSafe((PickupHalo != nullptr) ? PickupHalo->GetStaticMesh() : nullptr),
			UTraceFxShapes::IsIcosphereDegraded() ? TEXT(", icosphere degraded to the engine sphere") : TEXT(""),
			UTraceFxShapes::BlendName(PickupHaloBlend),
			TraceCoreArt::ThrownTrailHeadRadiusUU, TraceCoreArt::ThrownTrailTailRadiusUU,
			ThrownTrailSegments.Num(), *SegmentRadii,
			UTraceFxShapes::BlendName(ThrownTrailBlend),
			CVarCoreThrownTrailLength.GetValueOnGameThread());
	}
	else
	{
		if (PackMesh != nullptr)
		{
			PackMesh->SetVisibility(false);
			PackMesh->SetComponentTickEnabled(false);
		}
		if (HeartLight != nullptr)
		{
			HeartLight->SetVisibility(false);
		}
		if (Mesh != nullptr && bHasRenderer)
		{
			Mesh->SetVisibility(true);
		}

		if (bHasRenderer)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Core] spec v31 §4: FALLBACK SPHERE. art on disk=%s, -TraceNoCharacterArt=%s. ")
				TEXT("The match is fully playable; only the Core's model is the pre-v31 orb. ")
				TEXT("`git lfs pull` then Scripts/import-pack.sh brings in the real one."),
				bArtOnDisk ? TEXT("yes") : TEXT("NO"),
				bArtSwitchedOff ? TEXT("yes") : TEXT("no"));
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

	// SPEC v31 §4. THE MODEL'S POSE AND GLOW, AND IT IS AHEAD OF THE AUTHORITY SPLIT ON PURPOSE.
	//
	// Every fact it needs - Carrier, bLoose, LooseVelocity - already replicates, so each machine can
	// derive the presentation for itself and none of it has to be sent. Below the split it would run
	// on the listen host only, which is precisely the machine the Core is usually HIDDEN from (the
	// holder's own camera), i.e. it would be computed exactly where nobody can see it.
	//
	// It decides nothing and writes nothing but component transforms, material parameters and a light.
	UpdateCoreArt();

	// THE CARRIED BALL'S PLACE AND THE CARRIED BALL'S AUDIENCE, on the same terms and for the same
	// reasons: both are derived from Carrier, which already replicates, so every machine can work
	// them out for itself and neither has to be sent. Below the authority split they would run on the
	// listen host only - which is exactly the machine whose own holder is the one player this used
	// to be invisible to.
	//
	// AFTER UpdateCoreArt, not before. That function chooses which of Mesh/PackMesh is being drawn
	// and can toggle their visibility on the A/B edge; placing and unhiding them afterwards means the
	// ball's position and its audience are decided against the components that are actually on screen
	// this frame rather than the previous frame's answer.
	UpdateCarriedArtPlacement();
	UpdateCarriedArtVisibility();

#if !UE_BUILD_SHIPPING
	// Spec v8 §0/§4. Every machine's own view, so "the Core carries the momentum" is a claim that can
	// be checked on the CLIENT rather than inferred from the server's copy. Costs one int compare.
	TickFlightLog();
#endif

#if !UE_BUILD_SHIPPING
	// Spec v10 §10, same reasoning and the same placement: ahead of the authority split, because the
	// machine the bug is ON is the one that is not the server. Costs one int compare when disarmed.
	TickTeleportAudit();
#endif

	// SPEC v28 §7's WIRE RECORD, and it is ahead of the authority split for exactly that reason: on a
	// listen host this measures nothing a direct read would not, and on a CLIENT a non-zero count is
	// the only proof that the full-charge instant the red ring draws actually crossed the network
	// rather than being invented locally. Two compares and a float per tick.
	if (ThrowFullChargeServerTime >= 0.f)
	{
		if (ThrowFullChargeServerTime != LastSeenFullChargeStamp)
		{
			LastSeenFullChargeStamp = ThrowFullChargeServerTime;
			++FullChargeStampsSeen;
		}
		PeakSeenAutoReleaseAlpha = FMath::Max(PeakSeenAutoReleaseAlpha, GetThrowAutoReleaseAlpha());
	}

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

#if !UE_BUILD_SHIPPING
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
#endif // !UE_BUILD_SHIPPING

	const float Now = GetServerTimeSeconds();

#if !UE_BUILD_SHIPPING
	// Spec v10 §10's reproduction, armed from the console or -ExecCmds. Ahead of everything, because
	// stage two of it IS a reset and must not be run from inside one. One bool compare when disarmed.
	TickGoalRepro();
#endif

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

#if !UE_BUILD_SHIPPING
	// Diagnostics, and deliberately ahead of the loose branch's early return so the scenario can
	// observe a Core that is currently in the air. Costs one int compare when it is not armed.
	TickModeBVerification();
#endif

#if !UE_BUILD_SHIPPING
	// SPEC v13 §8's reproduction, same placement and the same reason: it has to watch a Core that is
	// in the air to know when the previous shot has resolved. One bool compare when disarmed.
	TickTurnoverRepro();
#endif

#if !UE_BUILD_SHIPPING
	// SPEC v25 §2's red arm. Ahead of the loose branch's early return for the same reason: most of
	// what it watches happens while the Core is lying on the ground with a window open, and the last
	// step of it watches the frame a locked-out player finally gets to pick it up. One int compare
	// when disarmed.
	TickTurnoverVerify();
#endif

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
		else
		{
			// FX_AUDIO_PLAN §5.1 (CountdownTick): the wait before the Core is handed out IS the
			// kickoff countdown, and this is the only place that knows how much of it is left.
			TraceCoreFile::TickKickoffCountdown(this, PendingGrantTime - Now);
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

	// SPEC v28 §7, and it runs FIRST inside the held branch because it is the one thing here that can
	// end the possession this branch is about. It publishes the full-charge instant the red ring
	// draws, and past the 0.6 s window it throws — at which point the Core is loose, this tick is
	// over, and everything below it (the trail enforcement, the carried-goal sweep) is talking about a
	// carrier who no longer exists.
	if (ServerTickThrowAutoRelease())
	{
		return;
	}

	ServerTickPass(DeltaSeconds);
	EnforceHolderTrailState();
#if !UE_BUILD_SHIPPING
	SamplePassAvailabilityStats();
#endif

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

#if !UE_BUILD_SHIPPING
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
#endif // !UE_BUILD_SHIPPING


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
#if !UE_BUILD_SHIPPING
				if (HasAuthority() && CVarTracePassStats.GetValueOnAnyThread() != 0)
				{
					++TracePassStats::GStats.StickySaves;
				}
#endif
				return Held;
			}
		}
	}

	return nullptr;
}


// =================================================================================================
// Pass: input and state machine
// =================================================================================================

void ATraceCore::RequestPassInput(bool bPressed, ATraceCharacter* Requester, float ClientPressServerTime)
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
			// SPEC v29 §6. THE METER ABSORBS A RE-PRESS FOR THE SAME REASON THE SERVER'S CLOCK DOES,
			// and it has to be said here as well or the fix would put the two ends OUT of step rather
			// than in it: the server keeps its original anchor, so a meter that restarted would show a
			// ring emptying while the charge behind it kept filling. bLocalThrowCharging is this
			// machine's own latch and is cleared by every release and by ClearThrowCharge, so "already
			// true" means exactly what the server's "already >= 0" means — the button never came up.
			if (!bLocalThrowCharging)
			{
				bLocalThrowCharging = true;
				LocalThrowChargeStartTime = GetServerTimeSeconds();
			}
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
				// =========================================================================
				// *** SPEC v29 §6, THE SECOND AND LARGER HALF: A RE-PRESS NO LONGER RESTARTS
				// *** THE CLOCK.
				// =========================================================================
				//
				// This block used to say: "Re-arming on a second press without a release simply
				// restarts the clock, which is the conservative answer: it can only ever produce a
				// weaker throw than the player asked for, never a stronger one." Both halves of that
				// are true and the conclusion was wrong — "a weaker throw than the player asked for"
				// IS the bug. Measured with Trace.ModeB.ThrowSpread's auto-release arm: 27 of 30
				// throws held the full 1.20 s and launched at x1.000, and THREE held 0.382 s, 0.425 s
				// and 0.444 s and launched at x0.691, x0.752 and x0.779 — from a hold nobody
				// interrupted. A second press edge had landed mid-wind-up and moved the anchor.
				//
				// SECOND PRESS EDGES ARE REAL AND THERE ARE AT LEAST THREE SOURCES OF THEM:
				//   * ATraceCharacter::DoFirePressed routes a CARRIER's fire straight into
				//     DoPassPressed, so anything that presses fire — a bot's trigger, a human with
				//     FIRE and THROW on different keys — is a second press on the same charge;
				//   * ATracePlayerController::RedeliverHeldPressEdges re-delivers BOTH the held Fire
				//     edge and the held Pass edge when a menu hands input back, which is two presses;
				//   * a duplicated or reordered packet.
				//
				// WHAT A SECOND PRESS MEANS IS "THE BUTTON IS STILL DOWN", and the instant the finger
				// went down has not moved — the release that would end this charge has not arrived,
				// and a release is the only thing that ends one. So the EARLIEST press wins and the
				// later edge is absorbed, which is the same latch-shaped reasoning RequestPassInput
				// already applies to a non-holder's release. A genuinely new wind-up is still possible
				// and still starts from zero, because it must be preceded by a release, and a release
				// clears the charge on every path.
				//
				// The red arm is `Trace.ModeB.ThrowChargeKeepOnRepress 0`.
				if (ThrowChargeStartServerTime >= 0.f
					&& ThrowChargeHolder.Get() == Requester
					&& TraceModeBTuning::ThrowChargeKeepsOnRepress())
				{
					UE_LOG(LogTraceGame, Verbose,
						TEXT("[ModeB] spec v29 s6: a second press arrived %.3fs into %s's wind-up and was ")
						TEXT("ABSORBED. The charge keeps its original anchor."),
						GetServerTimeSeconds() - ThrowChargeStartServerTime, *GetNameSafe(Requester));
					return;
				}

				//
				// *** SPEC v29 §6 — ANCHOR THE CHARGE AT THE STAMPED PRESS, NOT AT ARRIVAL. ***
				//
				// This one line is the whole of "sometimes a full charge does not go full distance".
				// Anchoring here meant the hold was (true hold) + (release lag − press lag); that
				// difference is jitter, it is signed, and a negative one is a charge the player
				// watched fill and did not get. Measured with Trace.ModeB.ThrowSpread — see
				// ServerSetPassInput's comment for the numbers, the security argument and the red arm.
				//
				// The clamp is the gun's, character for character (UTraceWeaponComponent::
				// ServerRequestEquip): never in the future, never further back than a bullet may be
				// rewound. A stamp of < 0 means "no stamp" — every server-side, bot and console caller
				// — and lands on GetServerTimeSeconds() exactly as before.
				const float ServerNow = GetServerTimeSeconds();
				float PressAt = ServerNow;
				if (TraceModeBTuning::ThrowChargeAnchorsAtPress() && ClientPressServerTime > 0.f)
				{
					const float MaxRewind = FMath::Max(0.f, UTraceSettings::Get().MaxRewindTime);
					PressAt = FMath::Clamp(ClientPressServerTime, ServerNow - MaxRewind, ServerNow);
				}
				ThrowChargeStartServerTime = PressAt;
				ThrowChargeHolder = Requester;

				// SPEC v28 §7. A fresh press is a fresh deadline. Stated here as well as in
				// ServerTickThrowAutoRelease's `Now < FullAt` branch, so a re-press cannot leave the
				// previous hold's full-charge instant on the wire for even one tick — that stale value
				// is a red ring that starts already part-full, and (worse) a deadline measured from a
				// charge the player no longer has.
				if (ThrowFullChargeServerTime >= 0.f)
				{
					ThrowFullChargeServerTime = -1.f;
					ForceNetUpdate();
				}
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

	// SPEC v29 §6. The PRESS carries the instant the button actually went down on this machine, read
	// off the SAME shared clock the server anchors against (LocalThrowChargeStartTime above is that
	// instant, and using it rather than a second GetServerTimeSeconds() call is what makes the meter
	// the player watches and the charge the server measures the same measurement rather than two).
	// A RELEASE carries no stamp at all — it is stamped by the server on arrival, exactly as before,
	// which is what keeps the hold from ever being a number a client chose.
	const float PressStamp = (bPressed && bLocalPlayerInput && IsModeB())
		? LocalThrowChargeStartTime
		: -1.f;

	ServerSetPassInput(bPressed, Requester, PressStamp);
}

void ATraceCore::ServerSetPassInput_Implementation(bool bPressed, ATraceCharacter* Requester, float ClientPressServerTime)
{
	// Network input: this RPC is routed by ownership (SetOwner(Carrier) in GrantTo), so only the
	// holding connection can reach it at all. It is re-validated anyway, by running the SAME
	// function the client ran - on the server HasAuthority() is true, so this lands in the branch
	// above and applies the identical press/release ownership rules. One copy of the rules, and a
	// client that lies about Requester is refused by them exactly as a local call would be.
	//
	// SPEC v29 §6. The stamp travels with it and is clamped there, in the one place the charge clock
	// is written. A non-finite value from a modified client is dropped rather than trusted — it would
	// otherwise reach FMath::Clamp and poison ThrowChargeStartServerTime for the rest of the match.
	const float Stamp = FMath::IsFinite(ClientPressServerTime) ? ClientPressServerTime : -1.f;
	RequestPassInput(bPressed, Requester, Stamp);
}

void ATraceCore::ServerTickPass(float /*DeltaSeconds*/)
{
	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

#if !UE_BUILD_SHIPPING
	const bool bCollectPassStats = (CVarTracePassStats.GetValueOnAnyThread() != 0);
#endif

	if (!IsValid(Carrier) || !Carrier->IsAlive())
	{
#if !UE_BUILD_SHIPPING
		if (bPassActive && bCollectPassStats)
		{
			++TracePassStats::GStats.CancelHolderGone;
		}
#endif
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
#if !UE_BUILD_SHIPPING
			if (bCollectPassStats)
			{
				++TracePassStats::GStats.CancelReleased;
			}
#endif
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
#if !UE_BUILD_SHIPPING
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.GraceSaves;
					TracePassStats::GStats.GraceSecondsAccrued += FMath::Max(0.f, Now - PassGraceStartServerTime);
				}
#endif
				PassGraceStartServerTime = 0.f;
			}
		}
		else
		{
			const bool bTransient = IsTransientPassRejection(RejectCode);
			if (!bTransient || GraceSeconds <= 0.f)
			{
#if !UE_BUILD_SHIPPING
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.CancelLostTarget;
					if (bTransient && PassGraceStartServerTime > 0.f)
					{
						++TracePassStats::GStats.GraceExpired;
					}
				}
#endif
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
#if !UE_BUILD_SHIPPING
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.CancelLostTarget;
					++TracePassStats::GStats.GraceExpired;
					TracePassStats::GStats.GraceSecondsAccrued += GraceSeconds;
				}
#endif
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

#if !UE_BUILD_SHIPPING
			if (bCollectPassStats)
			{
				++TracePassStats::GStats.PassesCompleted;
			}
#endif

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

#if !UE_BUILD_SHIPPING
	if (CVarTracePassStats.GetValueOnAnyThread() != 0)
	{
		++TracePassStats::GStats.PassesBegun;
	}
#endif

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

	// A NEW kickoff, so the countdown starts again from the top. Without this, a second kickoff
	// inside one session would find the "already announced" mark still at 1 and stay silent.
	TraceCoreFile::ResetKickoffCountdown(this);

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

	// FX_AUDIO_PLAN §5.1 (CountdownGo) — "kickoff release", and this is it: the one line in the
	// project where a queued kickoff actually becomes a carrier. Played BEFORE the grant so the cue
	// and the possession land on the same frame in the log, and paired with the reset so the next
	// kickoff's countdown starts from silence rather than from this one's last announced second.
	TraceCoreFile::ResetKickoffCountdown(this);
	TraceAudio::PlayLocal2D(this, TraceSoundEvents::CountdownGo);

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

	// *** SPEC v28 §2, EDGE TWO: "a carrier is killed". ***
	//
	// Announced BEFORE the transfer, at the body, because that is the moment the sentence names — not
	// when the killer's grant lands, and emphatically not when somebody later picks the Core up.
	// AnnounceTurnoverSound de-duplicates against ATraceGameMode's DropAt(), which fires on the same
	// OnDeath broadcast a few microseconds earlier for the same death; see that function.
	AnnounceTurnoverSound(VictimCharacter->GetActorLocation(), VictimCharacter, TEXT("the carrier was killed"));

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

float ATraceCore::GetThrowVelocityInheritanceDown()
{
	return TraceModeBTuning::ThrowInheritanceDown();
}

/**
 * Shared body for the live rule and its pre-v31 baseline. @p DownScale is the only difference.
 *
 * ONE expression, called twice, rather than two expressions that agree today: the whole value of the
 * A/B print is that the legacy number is what the legacy code WOULD have produced, and two copies of
 * an arithmetic that drifted apart would make the "before" column quietly fictional.
 */
static FVector TraceModeBInheritedThrowVelocity(const AActor* Thrower, float DownScale)
{
	if (!IsValid(Thrower))
	{
		return FVector::ZeroVector;
	}

	// MODE-GATED HERE rather than at every call site, so a caller can add this unconditionally and
	// still get the mode-A answer (a Core that never moves under its own power) without a mode test.
	if (!ATraceCore::IsModeB(Thrower->GetWorld()))
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
	FVector Inherited = Thrower->GetVelocity() * Fraction;

	// SPEC v31 §2. THE ONE LINE THE WHOLE SECTION IS ABOUT. Only a DESCENDING Z is scaled; a rising
	// one is passed through untouched, because spec v8 §4's jumping throw is a feature and this is a
	// bug report about the other sign. Horizontal is never touched at all - "I still want the core to
	// carry the player's velocity" is the first half of the same sentence.
	if (Inherited.Z < 0.0)
	{
		Inherited.Z *= static_cast<double>(DownScale);
	}

	return Inherited;
}

FVector ATraceCore::GetInheritedThrowVelocity(const AActor* Thrower)
{
	return TraceModeBInheritedThrowVelocity(Thrower, TraceModeBTuning::ThrowInheritanceDown());
}

FVector ATraceCore::GetLegacyInheritedThrowVelocity(const AActor* Thrower)
{
	// 1.0 is the pre-v31 rule by definition: the whole of the thrower's Z, sign included.
	return TraceModeBInheritedThrowVelocity(Thrower, 1.f);
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
	// plus the full post-weight impulse (2161.5 uu/s at Patch 28 §4's 2900 base).
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

	// SPEC v28 §7. The deadline dies with the charge it belonged to, on every one of the six paths
	// that reach here — a death, an interception, half time, a mode switch, a manual release, the
	// auto-release itself. Cleared HERE for the identical reason the charge is: one exit, not six.
	// ForceNetUpdate so the red ring on the owning client goes out on the same news as the throw,
	// rather than sitting full for the rest of that client's net update interval.
	if (ThrowFullChargeServerTime >= 0.f)
	{
		ThrowFullChargeServerTime = -1.f;
		if (HasAuthority())
		{
			ForceNetUpdate();
		}
	}

	if (bWasCharging && Reason != nullptr)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[ModeB] throw charge cancelled (%s)."), Reason);
	}
}

// =================================================================================================
// SPEC v28 §7 — A FULL-CHARGE THROW CANNOT BE HELD FOREVER
// =================================================================================================

float ATraceCore::GetThrowAutoReleaseSeconds()
{
	return TraceModeBTuning::ThrowAutoReleaseSeconds();
}

float ATraceCore::GetThrowAutoReleaseAlpha() const
{
	// < 0 is "the SERVER has not seen a full charge". On a client that also covers the sliver of a
	// round trip between the server deciding and this machine hearing about it: the ring has not
	// appeared yet, which is honest, rather than appearing at a locally-guessed fraction.
	if (ThrowFullChargeServerTime < 0.f)
	{
		return -1.f;
	}

	const float Window = GetThrowAutoReleaseSeconds();
	if (Window <= 0.f)
	{
		return -1.f;   // §7 switched off. No deadline exists, so there is nothing to draw.
	}

	// THE SHARED CLOCK ON BOTH ENDS. ThrowFullChargeServerTime was stamped by the server against
	// AGameStateBase::GetServerWorldTimeSeconds, and that is what GetServerTimeSeconds() reads here —
	// so this subtraction is the same subtraction ServerTickThrowAutoRelease does, evaluated on a
	// different machine. That is the whole of "what the player watches cannot disagree with what
	// happens".
	return FMath::Clamp((GetServerTimeSeconds() - ThrowFullChargeServerTime) / Window, 0.f, 1.f);
}

bool ATraceCore::ServerTickThrowAutoRelease()
{
	if (!HasAuthority() || !IsModeB())
	{
		return false;
	}

	// Nobody is winding up. ClearThrowCharge has already retired the deadline on every path that
	// ends a charge, so there is nothing to tidy here.
	if (ThrowChargeStartServerTime < 0.f)
	{
		return false;
	}

	// The presser must still be the living holder. Every route that breaks that (death, an
	// interception, half time) funnels through ReleaseHolder -> ClearThrowCharge, so this is a
	// same-frame ordering guard rather than a second rule: it stops one tick of a charge whose owner
	// changed between the possession write and the clear.
	ATraceCharacter* const Holder = ThrowChargeHolder.Get();
	if (!IsValid(Holder) || Holder != Carrier || !Holder->IsAlive())
	{
		return false;
	}

	const float Now = GetServerTimeSeconds();

	// *** THE INSTANT FULL CHARGE IS REACHED, DERIVED FROM THE CHARGE-TIME KNOB. *** Not a literal and
	// not "the tick we noticed": press time + CoreThrowChargeSeconds is the exact moment the green
	// ring completes, so retuning the wind-up moves the red ring's start with it and the two rings can
	// never describe different moments. Asked of the same accessor the meter and the throw curve use.
	const float FullAt = ThrowChargeStartServerTime + FMath::Max(0.01f, GetThrowChargeSeconds());

	if (Now < FullAt)
	{
		// Not full yet — and this also covers a RE-PRESS, which RequestPassInput handles by restarting
		// ThrowChargeStartServerTime. A deadline left over from the previous, longer hold would fire
		// early on the new one.
		if (ThrowFullChargeServerTime >= 0.f)
		{
			ThrowFullChargeServerTime = -1.f;
			ForceNetUpdate();
		}
		return false;
	}

	if (ThrowFullChargeServerTime != FullAt)
	{
		ThrowFullChargeServerTime = FullAt;
		ForceNetUpdate();   // The red ring must start on the frame the rule fired, not on the next update.

		// The disabled case gets its OWN sentence rather than printing "leaves automatically at
		// <FullAt + 0>", which is the same instant it just said the charge became full and reads as a
		// release that already happened. The red arm has to be legible in the log too.
		if (GetThrowAutoReleaseSeconds() > 0.f)
		{
			UE_LOG(LogTraceGame, Verbose,
				TEXT("[ModeB] spec v28 §7: %s reached FULL throw charge at %.3f (server clock). The red ")
				TEXT("ring is now filling; the Core leaves automatically at %.3f unless they throw first."),
				*GetNameSafe(Holder), FullAt, FullAt + GetThrowAutoReleaseSeconds());
		}
		else
		{
			UE_LOG(LogTraceGame, Verbose,
				TEXT("[ModeB] spec v28 §7 is OFF (Trace.ModeB.ThrowAutoReleaseSeconds 0): %s reached FULL ")
				TEXT("throw charge at %.3f and may hold it indefinitely. No red ring, no auto-release."),
				*GetNameSafe(Holder), FullAt);
		}
	}

	const float Window = GetThrowAutoReleaseSeconds();
	if (Window <= 0.f)
	{
		return false;   // §7 disabled (the red arm). A full charge may be held indefinitely, as before.
	}

	if (Now < FullAt + Window)
	{
		return false;   // The red ring is still filling.
	}

	// *** THE AUTOMATIC RELEASE. *** The SERVER's own call, from the SERVER's own hold, through the
	// same ThrowFromHolder every manual release goes through — so the launch, the cooldown, the trail,
	// the loose state and the log line are byte-for-byte what a player releasing at this instant would
	// have produced. There is deliberately no "auto" argument: a second throw path is a second set of
	// rules to keep in step.
	//
	// The hold handed over is the REAL elapsed hold rather than a hard-coded "full", so a character
	// whose charge is still legally growing past 100% (Mortimer, spec v19 §3 / Demo 21 item 7) gets
	// exactly what their own meter was showing at the deadline. For everybody else the curve has
	// clamped and this is precisely full charge, which is what §7 asks for.
	const float Held = FMath::Max(0.f, Now - ThrowChargeStartServerTime);
	const float ReleasedAtScale = GetThrowChargeScaleForHold(Held, Holder);
	const FString HolderName = GetNameSafe(Holder);

	// *** THE CHARGE IS NOT CLEARED FIRST HERE, AND THAT IS THE ONE DELIBERATE DIFFERENCE FROM THE
	// *** MANUAL RELEASE. RequestPassInput clears before it throws because the BUTTON IS UP by then:
	// a released button has no charge, whether or not the throw was allowed. On this path the finger
	// is still down, so a refusal (ThrowFromHolder rejects a throw still on ThrowCooldownEndServerTime,
	// a Core that went loose in the same frame, a state lock) must leave the wind-up alone and be
	// retried on the next tick — clearing it would silently eat a player's full charge and give them
	// nothing for it. ThrowFromHolder clears the charge itself on the path that succeeds, which is
	// where the clear belongs.
	if (!ThrowFromHolder(Holder, Held))
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeB] spec v28 §7: the automatic release was refused this tick (cooldown ends in ")
			TEXT("%.2fs, loose=%d). The charge is kept and it will be retried."),
			ThrowCooldownEndServerTime - Now, bLoose ? 1 : 0);
		return false;
	}

	// LOGGED AFTER THE THROW, NOT BEFORE IT. An earlier draft announced "AUTOMATIC RELEASE" on the
	// line above the attempt, so a refusal would have printed that claim once per tick for as long as
	// the cooldown lasted — a log line saying the Core had left while it was still in a hand.
	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] spec v28 §7: AUTOMATIC RELEASE. %s sat on a full charge for %.2fs (window %.2fs); ")
		TEXT("the server threw it at charge x%.2f from a %.2fs hold."),
		*HolderName, Now - FullAt, Window, ReleasedAtScale, Held);

	return true;
}

#if !UE_BUILD_SHIPPING

/**
 * `Trace.ModeB.ThrowAutoRelease.Report` — SPEC v28 §7 ON WHICHEVER MACHINE YOU TYPE IT.
 *
 * Run it on the LISTEN HOST and it tells you what the rule did. Run it on a SECOND PROCESS (a joined
 * client) and it answers the only question a single process cannot: did the server's full-charge
 * instant actually reach this machine? A client that reports stamps=0 while the host reports stamps=1
 * has a red ring that could only ever have been a local guess — which is precisely the failure §7's
 * "the ring shows the server's own progress" clause exists to forbid.
 */
static FAutoConsoleCommandWithWorldAndArgs GTraceModeBAutoReleaseReportCmd(
	TEXT("Trace.ModeB.ThrowAutoRelease.Report"),
	TEXT("SPEC v28 §7. What THIS machine knows about the full-charge auto-release: the window, the "
	     "last full-charge instant it has seen, how many distinct ones have arrived, and the largest "
	     "ring fill it has computed from them. On a client, a non-zero count is the wire answering."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World)
	{
		const ATraceCore* Core = ATraceCore::Get(World);
		if (Core == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[ModeB] spec v28 §7 report: no Core in this world."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeB] spec v28 §7 REPORT on this machine: netmode=%d authority=%d | window %.2fs | ")
			TEXT("full-charge stamps SEEN %d, last %.3f | peak ring fill computed here %.3f | live ")
			TEXT("stamp %.3f, live fill %.3f"),
			World != nullptr ? static_cast<int32>(World->GetNetMode()) : -1,
			Core->HasAuthority() ? 1 : 0,
			ATraceCore::GetThrowAutoReleaseSeconds(),
			Core->GetFullChargeStampsSeen(), Core->GetLastSeenFullChargeStamp(),
			Core->GetPeakSeenAutoReleaseAlpha(),
			Core->GetThrowFullChargeServerTimeForTest(), Core->GetThrowAutoReleaseAlpha());
	}));

#endif // !UE_BUILD_SHIPPING — island: the definition below must EXIST in Shipping

// NOT guarded with the battery around it: LastThrow is the sample SHIPPING code records —
// ThrowFromHolder() fills it and ServerTickLaunchAudit() reads it back — and only the
// Trace.ModeB.* printers around this line are dev-only. Guarding this one definition line
// is a Shipping link error (undefined ATraceCore::LastThrow).
ATraceCore::FThrowMomentumSample ATraceCore::LastThrow;


int32 ATraceCore::GoalsByMethod[static_cast<int32>(ATraceCore::EGoalMethod::Count)] = {};

#if !UE_BUILD_SHIPPING
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
#endif // !UE_BUILD_SHIPPING

ATraceCore::FSurfaceRuleStats ATraceCore::SurfaceStats;

#if !UE_BUILD_SHIPPING
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
#endif // !UE_BUILD_SHIPPING

ATraceCore::FCatchContestStats ATraceCore::CatchStats;

#if !UE_BUILD_SHIPPING
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
#endif // !UE_BUILD_SHIPPING

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

	// SPEC v31 §2. The three numbers the A/B needs, all read off THIS throw. The legacy term is asked
	// of the same shared expression with the down-scale forced to 1, so "what the old rule would have
	// done" cannot drift from what the old rule did.
	LastThrow.LaunchSpeed2D = static_cast<float>(LaunchVelocity.Size2D());
	LastThrow.InheritedVelocityZ = static_cast<float>(Inherited.Z);
	LastThrow.LegacyInheritedVelocityZ = static_cast<float>(GetLegacyInheritedThrowVelocity(Thrower).Z);
	LastThrow.InheritanceDown = TraceModeBTuning::ThrowInheritanceDown();
	// Feet, not the capsule centre: the travel report solves back to the plane the thrower is standing
	// on, and the launch point is an EYE offset, so the two differ by a whole half-capsule.
	LastThrow.LaunchHeightAboveFeet = static_cast<float>(LaunchLocation.Z
		- (Thrower->GetActorLocation().Z
			- ((Thrower->GetCapsuleComponent() != nullptr)
				? static_cast<double>(Thrower->GetCapsuleComponent()->GetScaledCapsuleHalfHeight())
				: 88.0)));
	LastThrow.HeldSeconds = FMath::Max(0.f, HeldSeconds);
	LastThrow.ChargeScale = ChargeScale;
	LastThrow.ThrowerName = GetNameSafe(Thrower);
	// SPEC v29 §6. See the member: a throw is detected by this moving, never by the values changing.
	++LastThrow.Serial;

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
	ForgetLastContact();      // Spec v13 §8, and Demo 27's "what did it hit": this flight starts blank.
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

	// DEMO 27. ARM THE LAUNCH AUDIT. See ServerTickLaunchAudit: a tenth of a second from now the
	// flight is asked how much of this launch it still has, and says so in the log if the answer is
	// "almost none". Three assignments, no allocation, and it runs on every throw in a real match
	// rather than only under a harness - which is the whole point, because this bug shipped twice.
	LaunchAuditDueServerTime = Now + TraceModeBTuning::LaunchAuditSeconds;
	LaunchAuditLaunchSpeed = static_cast<float>(LaunchVelocity.Size());
	LaunchAuditSerial = LastThrow.Serial;

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

	// *** NO TURNOVER SOUND HERE, AND THE FIRST VERSION OF THIS GOT IT WRONG. ***
	//
	// This line used to announce a turnover at the LAUNCH of every throw, on the reading that a throw
	// is "the core is dropped by a team". It is not: a throw is a PASS, the most ordinary offensive
	// act in the game, and announcing it made the turnover sound fire on every single one — which is
	// louder and more misleading than the bug it replaced.
	//
	// A thrown Core becomes a turnover when it LANDS UNCAUGHT, which is the game's own existing rule
	// and is where RegisterTurnover() fires (see the landing path above). The sound belongs on that
	// edge and on the carrier's death, and it is announced from both of those places.

	UE_LOG(LogTraceGame, Display,
		TEXT("[ModeB] THROW by %s (%s) at %.0f uu/s from %s | v13 §6: held %.2fs -> charge x%.2f (floor ")
		TEXT("%.2f, full at %.2fs) | v8 §4: charged impulse %.0f + inherited %.0f (thrower %.0f uu/s ")
		TEXT("horiz, %+.0f vert, %s, x%.2f), launch Z %+.0f | v31 §2: down-scale x%.2f, inherited Z ")
		TEXT("%+.0f (pre-v31 would have been %+.0f, i.e. launch Z %+.0f)"),
		*GetNameSafe(Thrower), *TraceTeamName(ThrowerTeam).ToString(),
		LaunchVelocity.Size(), *LaunchLocation.ToCompactString(),
		LastThrow.HeldSeconds, ChargeScale,
		TraceModeBTuning::ThrowChargeFloor(), TraceModeBTuning::ThrowChargeSeconds(),
		LastThrow.ImpulseSpeed, LastThrow.InheritedSpeed,
		LastThrow.ThrowerSpeed2D, LastThrow.ThrowerVelocityZ,
		LastThrow.bThrowerFalling ? TEXT("AIRBORNE") : TEXT("grounded"),
		LastThrow.Inheritance, LastThrow.LaunchVelocityZ,
		LastThrow.InheritanceDown, LastThrow.InheritedVelocityZ, LastThrow.LegacyInheritedVelocityZ,
		LastThrow.LaunchVelocityZ - LastThrow.InheritedVelocityZ + LastThrow.LegacyInheritedVelocityZ);

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

	// DEMO 27. Ahead of everything, including the pull's early return: the audit is a report about a
	// launch that has already happened, and a flight that gets picked up by a magnetic delivery in
	// its first tenth of a second is exactly the kind of thing it must still be able to describe.
	ServerTickLaunchAudit();

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
		//
		// DEMO 27: THAT SENTENCE IS ONLY TRUE SINCE SweepLooseCore EXISTED. It used to be a channel
		// sweep, which every player capsule blocks, and the capsule it hit most reliably was the
		// thrower's own on the launch frame. See the FlightHitsPawns block at the top of this file.
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreLoose), /*bTraceComplex=*/false, this);
		Params.AddIgnoredActor(this);

		FHitResult Hit;
		const bool bBlocked = TraceModeBTuning::SweepLooseCore(
			*World, Hit, StartLocation, Desired, TraceModeBTuning::CollisionRadius, Params);

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

			// --- DEMO 27. A DEPENETRATION IS NOT AN IMPACT, AND MUST NOT BE MIRRORED. ---------------
			//
			// ArrivalSin is NEGATIVE when the Core is already travelling AWAY from the contact normal,
			// which a real impact never is: you cannot strike a face you are receding from. It happens on
			// a sweep that STARTS penetrating, where the "normal" is the direction the physics engine
			// wants to push the Core OUT along - and the push-out above has already done exactly that.
			// Mirroring on top of it reverses a velocity that was leaving, which is how a 3504 uu/s
			// forward throw became a 681 uu/s backward one on its own launch frame.
			//
			// This is the SECOND of the two Demo 27 fixes and it is deliberately not the first: the
			// object-type sweep stops the thrower's capsule from being the surface, and this stops ANY
			// start-penetrating contact - a launch point inside the lip of a crate, a Core dropped by a
			// debug command inside a wall - from firing the Core backwards out of it.
			const bool bAlreadySeparating = (ArrivalSin < 0.0);

			const FVector Reflected = bAlreadySeparating
				? FVector(LooseVelocity)
				: FVector(LooseVelocity).MirrorByVector(ContactNormal) * TraceModeBTuning::Bounce();

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

			// Demo 27: a separating depenetration is not a bounce and is not counted as one. The
			// tally is read as "how often did a throw come off a wall", and a launch shoved out of a
			// surface it was already leaving would have inflated it while changing no velocity.
			if (!bUpwardFacing && !bAlreadySeparating)
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
				// DEMO 27 ADDS THE ONE FIELD THAT WOULD HAVE ENDED THIS IN A MINUTE: WHAT IT HIT.
				// Every other number in this line was already here and the whole set of them was
				// consistent with a wall. Naming the actor says "TraceCharacter_0" and the diagnosis
				// is over. A contact log that cannot name the surface is a log that can only be read
				// by somebody who already knows the answer.
				UE_LOG(LogTraceGame, Display,
					TEXT("[ModeBTurnover] CONTACT at %s with %s (%s) | %.0f uu above the floor | speed ")
					TEXT("%.0f -> %.0f uu/s | normal %s (%.0f deg from up, up-facing=%d) | arrival %.1f ")
					TEXT("deg to the surface (needs %.0f) | airborne %.3fs (needs %.3f) | orb %.0f uu ")
					TEXT("above what is under it (needs <= %.0f) | VERDICT: %s"),
					*SurfacePoint.ToCompactString(), *GetNameSafe(Hit.GetActor()),
					*GetNameSafe(Hit.GetComponent()),
					SurfacePoint.Z - FloorZ,
					Speed3DBeforeContact, Reflected.Size(),
					*ContactNormal.ToCompactString(),
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(ContactNormal.Z, -1.0, 1.0))),
					bUpwardFacing ? 1 : 0,
					FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(ArrivalSin, -1.0, 1.0))),
					TraceModeBTuning::LandingMinDescentDegrees(),
					Now - LooseStartServerTime, TraceModeBTuning::LandingMinFlightSeconds(),
					ContactSupportGap, CVarModeBTurnoverContactSlack.GetValueOnAnyThread(),
					bAlreadySeparating ? TEXT("DEPENETRATION - flies on, not mirrored (Demo 27)")
						: !bUpwardFacing ? TEXT("WALL - bounce")
						: (bContactIsLanding
							? (bComesToRest ? TEXT("SETTLED on it - turnover") : TEXT("LANDING - turnover"))
							: (!bVisiblyOnTop ? TEXT("NOT ON TOP OF IT (v19 §1.5) - keeps falling")
								: (!bClearedTheThrower ? TEXT("launch-frame contact - flies on")
									: TEXT("GRAZE - flies on")))));
			}

			// DEMO 27. WHAT IT HIT, KEPT AND NOT ONLY PRINTED. The line above names the actor, but only
			// under Trace.ModeB.TurnoverLog, which is off in every ordinary run - so the always-on
			// launch alarm had no way to tell a goal spoke from the thrower's own chest and cried wolf
			// on four throws in thirty-four. Two cheap fields here are the whole discriminator; see
			// ServerTickLaunchAudit. The body test is the HIT COMPONENT'S object type, the same
			// question SweepLooseCore asks and for the same reason: what blocks a sweep is a shape.
			LastContactServerTime = Now;
			LastContactActor = Hit.GetActor();
			LastContactComponentName = (Hit.GetComponent() != nullptr) ? Hit.GetComponent()->GetFName() : NAME_None;
			bLastContactWasBody =
				(Hit.GetComponent() != nullptr && Hit.GetComponent()->GetCollisionObjectType() == ECC_Pawn)
				|| (Hit.GetActor() != nullptr && Hit.GetActor() == LooseThrower.Get());
			bFlightHitABody = bFlightHitABody || bLastContactWasBody;

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

void ATraceCore::ServerTickLaunchAudit()
{
	if (!HasAuthority() || LaunchAuditDueServerTime < 0.f)
	{
		return;
	}

	const float Now = GetServerTimeSeconds();
	if (Now < LaunchAuditDueServerTime)
	{
		return;
	}

	// One shot. Disarmed before anything below can return early, so a refused audit cannot fire again
	// on every subsequent frame of the same flight.
	LaunchAuditDueServerTime = -1.f;

	// THE SERIAL, not the values. A throw that was caught and re-thrown inside the audit window would
	// otherwise be scored against the FIRST launch's speed - see FThrowMomentumSample::Serial, which
	// exists because two throws can be byte-identical and this is the same trap one level up.
	if (LastThrow.Serial != LaunchAuditSerial || LaunchAuditLaunchSpeed < 1.f)
	{
		return;
	}

	const float SpeedNow = static_cast<float>(FVector(LooseVelocity).Size());
	const float Retained = SpeedNow / LaunchAuditLaunchSpeed;

	// How far it actually got from the man who threw it, measured to his CAPSULE for the same reason
	// the catch zone is: the distance that matters is the gap a player sees between the ball and the
	// body, not the gap to a point inside his chest.
	double DistanceFromThrower = -1.0;
	if (const ATraceCharacter* Thrower = LooseThrower.Get())
	{
		DistanceFromThrower = FVector::Dist(FVector(LooseLocation), Thrower->GetActorLocation());
		if (const UCapsuleComponent* Capsule = Thrower->GetCapsuleComponent())
		{
			const FVector CapsuleCentre = Capsule->GetComponentLocation();
			const double HalfHeight = static_cast<double>(Capsule->GetScaledCapsuleHalfHeight());
			const FVector CatchPoint(CapsuleCentre.X, CapsuleCentre.Y,
				FMath::Clamp(FVector(LooseLocation).Z, CapsuleCentre.Z - HalfHeight, CapsuleCentre.Z + HalfHeight));

			DistanceFromThrower = FMath::Max(0.0,
				FVector::Dist(FVector(LooseLocation), CatchPoint)
					- static_cast<double>(Capsule->GetScaledCapsuleRadius()));
		}
	}

	LastThrow.SpeedAfterLaunch = SpeedNow;
	LastThrow.LaunchRetained = Retained;
	LastThrow.DistanceFromThrowerAfterLaunch = static_cast<float>(FMath::Max(0.0, DistanceFromThrower));

	// --- THE ALARM, AND IT IS ASKED BEFORE THE SPEED. --------------------------------------------
	//
	// A BODY BLOCKED THE FLIGHT SWEEP. That is the Demo 27 bug stated as the rule it breaks rather
	// than as a symptom, and it is why this test comes first: with the depenetration guard also in
	// place a self-collision can now leave the SPEED intact and only wreck the clearance, so a
	// ratio-first alarm would have gone to Verbose and said nothing on the very arm that proves it.
	// Trace.ModeB.RunThrowTest on Trace.ModeB.FlightHitsPawns 1 does exactly that: 100% of the launch
	// retained, 8 uu clear of the thrower, and the old ordering never printed a word.
	//
	// It cannot cry wolf. SweepLooseCore filters pawn-typed colliders out of the sweep's ANSWER, so
	// on a working tree bFlightHitABody has no way to be set; unlike the clearance test this replaced
	// there is no threshold here to be wrong about.
	if (bFlightHitABody)
	{
		const FString ContactBodyName = bLastContactWasBody
			? FString::Printf(TEXT("%s (%s)"), *GetNameSafe(LastContactActor.Get()),
				*LastContactComponentName.ToString())
			: FString(TEXT("a body earlier in the same flight"));

		UE_LOG(LogTraceGame, Warning,
			TEXT("[ModeBLaunch] *** %s's throw was blocked by a BODY - %s - and %.2fs later it has ")
			TEXT("%.0f%% of its %.0f uu/s launch (%.0f uu/s) and is %.0f uu clear of them. A pawn is ")
			TEXT("not geometry: the flight sweep is meant to filter bodies out of its answer, so this ")
			TEXT("is the Demo 27 report - \"it doesn't throw forward when moving forward\" - back ")
			TEXT("again (Trace.ModeB.FlightHitsPawns is %d)."),
			*LastThrow.ThrowerName, *ContactBodyName, TraceModeBTuning::LaunchAuditSeconds,
			100.f * Retained, LaunchAuditLaunchSpeed, SpeedNow,
			LastThrow.DistanceFromThrowerAfterLaunch,
			CVarModeBFlightHitsPawns.GetValueOnAnyThread());
		return;
	}

	if (Retained >= TraceModeBTuning::LaunchAuditMinRetained)
	{
		// The ordinary case, and it is Verbose rather than silent so a run can PROVE the audit was
		// armed and did look. A check nobody can see pass is a check nobody trusts when it fails.
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[ModeBLaunch] %s's throw kept %.0f%% of its %.0f uu/s launch after %.2fs (%.0f uu/s, ")
			TEXT("%.0f uu clear of them)."),
			*LastThrow.ThrowerName, 100.f * Retained, LaunchAuditLaunchSpeed,
			TraceModeBTuning::LaunchAuditSeconds, SpeedNow, LastThrow.DistanceFromThrowerAfterLaunch);
		return;
	}

	// --- THE THROW IS SLOW, AND NO BODY TOUCHED IT. WHAT TOOK IT? --------------------------------
	//
	// THIS USED TO BE A DISTANCE AND THAT WAS A GUESS. The first version of this function asked
	// whether the Core had got 150 uu clear of the thrower and called anything nearer the Demo 27 bug.
	// It cried wolf: Bounce() is 0.195, so EVERY wall bounce loses about 80% of the launch, and how
	// much clearance a slow throw has left is then a fact about how close the nearest pylon was. Four
	// of thirty-four throws on the FIXED tree fired that Warning and all four were geometry - one of
	// them, gk-final.log:1720, with the contact log one line above it naming "TraceArenaBuilder_0
	// (DaisPylon_5) ... VERDICT: WALL - bounce". An alarm that tells the next reader the ASAP bug is
	// back on a tree where it is not is worse than no alarm at all.
	//
	// The bug now has the test above, which is the rule and not a proxy for it. What is left down here
	// is a throw that lost its speed to something that was not a player, and the only thing worth
	// saying about it is WHAT - so the line names the actor and the component, which until now were
	// printed only under Trace.ModeB.TurnoverLog and therefore in almost no run that mattered.
	const bool bHadContact = (LastContactServerTime >= 0.f);
	const FString ContactName = bHadContact
		? FString::Printf(TEXT("%s (%s)"), *GetNameSafe(LastContactActor.Get()),
			*LastContactComponentName.ToString())
		: FString();

	// The contact's age is in both lines because on the bug it is the LAUNCH FRAME ITSELF -
	// "0.000s into the flight" - and no throw that ever left the hand can be that.
	const FString Hit = bHadContact
		? FString::Printf(TEXT("%s took it %.3fs into the flight"), *ContactName,
			LastContactServerTime - LooseStartServerTime)
		: FString(TEXT("nothing was recorded hitting it, so look at the magnet and the pull instead"));

	if (bHadContact)
	{
		// SLOW, AND ARENA GEOMETRY TOOK IT. An ordinary throw into a wall a couple of metres away, and
		// at Bounce() = 0.195 it is SUPPOSED to lose four fifths of its speed doing that. Worth a line
		// so a reader chasing a "the throw died" report can see it happen; never an alarm.
		//
		// The clearance is still printed, and at 0 uu with a launch-frame age it is worth reading: that
		// is a thrower standing against a pylon throwing into it, which looks like the reported bug to
		// a player and is a separate thing to fix. It is not THIS bug, and this line does not say it is.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ModeBLaunch] %s's throw lost %.0f%% of its launch in %.2fs (%.0f -> %.0f uu/s): %s. ")
			TEXT("Arena geometry, not a player - %.0f uu clear of them when it was measured."),
			*LastThrow.ThrowerName, 100.f * (1.f - Retained), TraceModeBTuning::LaunchAuditSeconds,
			LaunchAuditLaunchSpeed, SpeedNow, *Hit, LastThrow.DistanceFromThrowerAfterLaunch);
		return;
	}

	// *** SLOW, AND NOTHING ON RECORD TOUCHED IT. ***
	//
	// Still a Warning, and still a rare one by construction: free flight loses only gravity over
	// 0.10 s and the catch magnet preserves speed exactly, so a Core that shed 40% of its launch with
	// no blocked sweep behind it means something OUTSIDE the contact path is eating throws. That is
	// not the Demo 27 self-collision and this line no longer claims it is - it says what it knows.
	UE_LOG(LogTraceGame, Warning,
		TEXT("[ModeBLaunch] *** %s's throw LOST %.0f%% of its launch in %.2fs: %.0f -> %.0f uu/s, ")
		TEXT("%.0f uu clear of them, and %s. No wall, no floor and no player took it, so the speed ")
		TEXT("went somewhere that is not the contact path - look at the magnet, the pull and the ")
		TEXT("charge (Trace.ModeB.FlightHitsPawns is %d)."),
		*LastThrow.ThrowerName, 100.f * (1.f - Retained), TraceModeBTuning::LaunchAuditSeconds,
		LaunchAuditLaunchSpeed, SpeedNow, LastThrow.DistanceFromThrowerAfterLaunch, *Hit,
		CVarModeBFlightHitsPawns.GetValueOnAnyThread());
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

	// The SAME sphere and the SAME query the flight sweeps use - literally the same function since
	// Demo 27. A probe with a different shape would be a second opinion about what the Core is
	// touching, and the two would disagree exactly where it matters - on the lip of a block, which is
	// the geometry the user's report is about. It also means the Core can no longer be declared to be
	// resting on a PLAYER who happens to be standing over it.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceCoreRestProbe), /*bTraceComplex=*/false, this);
	Params.AddIgnoredActor(this);

	FHitResult Hit;
	const bool bHit = TraceModeBTuning::SweepLooseCore(
		*World, Hit, From, To, TraceModeBTuning::CollisionRadius, Params);

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

	// Demo 27: the same pawn-rejecting query as the flight sweep, so "what is under it" means the
	// same thing to this rule as to the one that put the Core there. A player's shins are not a floor.
	FHitResult Hit;
	const bool bHit = TraceModeBTuning::SweepLooseCore(
		*World, Hit, From, To, static_cast<float>(TraceModeBVisibleOrbRadius), Params);

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

		// *** SPEC v28 §2, THE REAL EDGE. *** This is where a thrown Core actually becomes a
		// TURNOVER — it settled on a surface uncaught, the throwing team is locked out, and the
		// other team may pull it. Announcing here rather than at the throw's LAUNCH is what stops
		// every ordinary pass from sounding like a turnover. AnnounceTurnoverSound de-duplicates,
		// so a landing that coincides with a carrier death still costs exactly one sound.
		AnnounceTurnoverSound(LandedAt, nullptr, TEXT("a thrown Core landed uncaught"));

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

	// *** SPEC v28 §2, EDGE TWO, THE OTHER HALF. *** This is the GameMode's death path (it runs before
	// OnHolderDeath knows who the killer was) and it is also the disconnect path — both are "the team
	// holding the Core stopped holding it, and it is going to the other side", which is the turnover
	// the sentence describes. Announced from BOTH here and OnHolderDeath rather than from whichever
	// one happens to run first, because neither is reached by every route: a Logout never touches
	// OnHolderDeath, and a death whose GameMode path is bypassed never touches this. The de-dup inside
	// AnnounceTurnoverSound is what makes covering both cost one sound rather than two.
	AnnounceTurnoverSound(Carrier->GetActorLocation(), Carrier, TEXT("the carrier lost it (death or disconnect)"));

	ReleaseHolder();
	ForceNetUpdate();
}

// ResetToCenter() IS DELETED. Zero callers — every score, match start and half-time reset calls
// KickoffTo() directly, which is the entry point this merely wrapped.

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

	// The owner chain IS what bOwnerNoSee resolves against, so this is the one OnRep that has to
	// rebuild every drawn piece's proxy. Same list as ApplyAttachment's, from the same function, so
	// the two cannot fall out of step - which is precisely how PackMesh came to be missing from it.
	MarkDrawnPiecesRenderStateDirty();
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
