#include "Gameplay/TraceWeaponComponent.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"      // EFirstPersonPrimitiveType (the knife viewmodel)
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"   // the knife in the third-person hand
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Gameplay/TraceKnifeView.h"           // VisibleBladeParts — the pack blade is in none of this file's lists
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"                       // TActorIterator, for the recoil test harness
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"       // Trace.Fx.ImpactShots dilates the clock for the capture
#include "GameFramework/SpringArmComponent.h"   // recoil: the camera boom's tick order
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"                 // TNumericLimits<double>
#include "Misc/App.h"                          // spec v29 §2f: FApp::UseFixedTimeStep, for the harness
#include "Math/UnrealMathUtility.h"
#include "Net/UnrealNetwork.h"                  // DOREPLIFETIME

#include "Abilities/Characters/TraceAbilityWeaponHooks.h"   // spec v14 §6: X's Sting bullets
#include "Abilities/Characters/TraceAbilitySetRoxie.h"       // spec v29 §2e: MODDED's recoil, §2b: its full auto
#include "Abilities/TraceAbilityComponent.h"                 // spec v14 §6: the damage passives
#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Movement/TraceCharacterMovementComponent.h"   // SetKnifeMovementProfileActive (spec v10 §1)
#include "Gameplay/TraceCore.h"                        // spec v16 §1: the carrier has no gun and no ammo
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceHitZones.h"
#include "Gameplay/TraceMelee.h"
#include "Gameplay/TraceMeleeArc.h"
#include "Gameplay/TraceTracer.h"
#include "Net/TraceLagCompensationComponent.h"
#include "Audio/TraceAudio.h"          // FX_AUDIO_PLAN §5.1/§6: the predicted local shot, swing and reload
#include "Audio/TraceAudioWatch.h"     // IsPredictedShotEnabled + FTracePistolLadder
#include "Audio/TraceSoundBank.h"      // UTraceAudioSettings::GetPistolLadderResetSeconds
#include "Audio/TraceSoundEvents.h"    // the event names
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"

/**
 * Logs, per shot, the zone the SHOOTER's own predicted trace produced and the zone the SERVER
 * scored, plus whether they agree.
 *
 * Spec section 6 calls the disagreement out by name: "the client-side predicted trace and the
 * server's authoritative trace must use the SAME zone model, or players will see hits that do not
 * register". They share one implementation (FTraceHitZoneModel), so the only thing left that can
 * differ is the POSE each is fed - live-and-interpolated on the client, rewound history on the
 * server. This is how that is measured rather than assumed. Off by default; it is one line per
 * shot and ten bots fire a lot.
 */
static TAutoConsoleVariable<int32> CVarTraceDebugHitZones(
	TEXT("Trace.DebugHitZones"),
	0,
	TEXT("1: log the predicted vs server-resolved damage zone for every shot fired by a local player."),
	ECVF_Default);

// =================================================================================================
// SHOT STATISTICS - the answer to "shooting feels WAY more inconsistent"
//
// The complaint is about FEEL, and feel is a distribution. Damage is head 100 / body 40 / legs 25
// against 100 health, so the time to kill is 1, 3 or 4 rounds depending purely on which band a shot
// lands in. If the bands sit where the player thinks they do, that reads as positional damage; if
// they do not, it reads as the gun randomly deciding how much it feels like doing today - every
// shot registering, none of them worth the same.
//
// So this accumulates, on the authority, for every shot the server accepts:
//   * the zone histogram, which is the headline number,
//   * WHERE on the body each shot landed as a fraction of the target's height, so the bands can be
//     checked against where people actually aim rather than against an assumption,
//   * the same shot re-classified at the ray's closest approach to the body axis instead of at the
//     capsule entry point, which isolates one specific suspected defect,
//   * predicted (client/shooter) vs authoritative (server) zone agreement - the 567/0 baseline,
//   * how often the world trace truncated the shot, and how short.
//
// Off by default and one integer increment per shot when on. Bots fire constantly, so a 90 s match
// is a few thousand samples, which is more than enough to see a skew.
// =================================================================================================

/**
 * Per-shot recoil trace: what the kick was, what it accumulated to, and where the view ended up.
 *
 * Spec v5 section 6 asks for recoil that is felt but does not move the bullet, and the only way to
 * tell those two apart from outside is to print the view state around the moment of fire. Off by
 * default - one line per shot, and at 150 RPM a firing bot squad is still a lot of lines.
 */
static TAutoConsoleVariable<int32> CVarTraceDebugRecoil(
	TEXT("Trace.DebugRecoil"),
	0,
	TEXT("1: log the upward recoil kick, the accumulated climb and the resulting control pitch for every locally fired shot."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarTraceShotStats(
	TEXT("Trace.ShotStats"),
	0,
	TEXT("1: accumulate the hit-zone / impact-height distribution for every server-accepted shot. Trace.ShotStats.Dump prints it."),
	ECVF_Default);

/**
 * SPEC v29 §2f. Records the local-clock instant of every round this machine fires.
 *
 * The 537-RPM report was a MEAN over 42 rounds, and a mean is the one statistic that cannot tell a
 * gun that is uniformly 12% slow from one that stutters every sixth round — which are different bugs
 * with different fixes. This exists so the verdict is a distribution: n, mean, min, max, stddev and
 * the modal gap, over 40+ consecutive rounds, at each arm.
 *
 * Stamped inside FireOnce rather than sampled by a ticker, deliberately. A ticker can only observe
 * frame boundaries, and frame boundaries are exactly what is under suspicion here; a harness that
 * measured them would be unable to distinguish the bug from its own instrument.
 */
static TAutoConsoleVariable<int32> CVarTraceRecordShots(
	TEXT("Trace.Weapons.RecordShots"),
	0,
	TEXT("1: record the local-clock instant of every round fired, for Trace.Weapons.V29. Off in normal play."),
	ECVF_Default);

// =================================================================================================
// AMMO — spec v16 §1. THREE ARMS, THREE SEPARATE SENTENCES THEY FALSIFY.
//
// None of these shares a name with a console COMMAND (that collision is fatal at module load in this
// engine version): the commands are all verbs — Trace.Ammo.Dump, .Test, .CarrierTest, .BotWatch —
// and these three are nouns.
// =================================================================================================

/**
 * MASTER ARM. 0 makes the clip infinite and never reloads.
 *
 * It is the arm that falsifies "30 bullets per clip, then the gun reloads" as a whole: with it at 0
 * a pawn can fire a hundred rounds without the count moving, which is exactly the build that existed
 * before this pass. Trace.Ammo.Test's red arm uses it to prove its own measurement is measuring
 * something.
 */
static TAutoConsoleVariable<int32> CVarAmmoEnabled(
	TEXT("Trace.Ammo.Enabled"), 1,
	TEXT("1 (shipped, spec v16 §1): 30 rounds per clip, auto-reload on empty, 0.5 s reload. "
	     "0: the RED arm — the clip is infinite and nothing ever reloads."),
	ECVF_Cheat);

/**
 * THE RELOAD GATE ARM. 0 lets a player fire straight through a reload.
 *
 * Separate from the master arm because "reloading takes .5 seconds" is only a rule if something is
 * refused during it. With the master arm at 0 there is no reload to fire through and the sentence is
 * untestable; with this one at 0 the reload still runs, still shows on the HUD and still refills —
 * the only thing that changes is whether CanFire() says no. One variable.
 */
static TAutoConsoleVariable<int32> CVarAmmoReloadBlocksFire(
	TEXT("Trace.Ammo.ReloadBlocksFire"), 1,
	TEXT("1 (shipped): firing is refused for the whole 0.5 s reload. 0: the RED arm — the reload still "
	     "runs and still refills, but the trigger works through it."),
	ECVF_Cheat);

/**
 * THE PREDICTION ARM. 0 makes an owning client WAIT for the server's count instead of predicting it.
 *
 * Spec v16 §1 asks for "server-authoritative, client-predicted for feel — the local player must see
 * the count drop on their own shot without waiting for a round trip", and a HOST-SIDE measurement
 * cannot see a prediction bug by definition: on the authority the predicted path is not even taken.
 * So the arm and its probe (Trace.Ammo.ClientPredictTest) both live on the CLIENT, and the arm turns
 * the feature off without turning the ammo system off — with it at 0 the clip still empties, still
 * reloads and is still authoritative, and the only thing that changes is WHEN the shooter's own
 * number moves.
 */
static TAutoConsoleVariable<int32> CVarAmmoPredict(
	TEXT("Trace.Ammo.Predict"), 1,
	TEXT("1 (shipped): an owning client decrements its own clip on its own shot, before the server has "
	     "heard about it. 0: the RED arm — the client waits for replication, so its count lags its own "
	     "muzzle flash by a round trip."),
	ECVF_Cheat);

/**
 * THE CARRIER ARM. 0 removes the carrier guard inside UTraceWeaponComponent::ConsumeRound.
 *
 * Spec v16 §1: "The Core carrier has no gun, so ammo must not be consumed or shown while carrying."
 * The SHIPPED path cannot reach the consumption code with a carrier at all — CanFire() and
 * ServerFire() both refuse one first — so this arm is about the SECOND lock, the one that survives a
 * future caller reaching ConsumeRound another way. Trace.Ammo.CarrierTest drives that lock directly
 * (see UTraceWeaponComponent::DebugConsumeRound) and checks the first lock separately.
 */
static TAutoConsoleVariable<int32> CVarAmmoCarrierGuard(
	TEXT("Trace.Ammo.CarrierGuard"), 1,
	TEXT("1 (shipped): a Core carrier's clip can never lose a round, even if something calls the "
	     "consumption path directly. 0: the RED arm — the guard is removed and "
	     "TraceAmmo::GetCarrierRoundsConsumed() moves."),
	ECVF_Cheat);

/**
 * The ammo counters. File-static for the same reason UTraceHealthComponent's vulnerable alarms are:
 * the facts being counted are about the RULES, not about any one pawn.
 *
 * The first must be zero for the life of a correct process. The rest are LIVENESS, and they are what
 * lets a harness distinguish "the carrier rule held" from "nothing ever fired a shot".
 */
static int32 GAmmoCarrierRoundsConsumed = 0;
static int32 GAmmoRoundsConsumed        = 0;
static int32 GAmmoReloadsCompleted      = 0;
static int32 GAmmoDryFireRefusals       = 0;
static int32 GAmmoReloadFireRefusals    = 0;

namespace TraceAmmo
{
	int32 GetClipSize()
	{
		// Derived from the knob, never hardcoded: spec v16 §1 says 30 and UTraceSettings::ClipSize
		// says 30. Read it here so a designer retuning it live is obeyed by every caller at once.
		return FMath::Clamp(UTraceSettings::Get().ClipSize, 1, 999);
	}

	float GetReloadSeconds()
	{
		return FMath::Clamp(UTraceSettings::Get().ReloadSeconds, 0.05f, 10.f);
	}

	// =============================================================================================
	// SPEC v28 §9 — THE SAME KNOBS, PER WEAPON.
	//
	// Four one-line switches rather than four copies of the arithmetic, and they all clamp with the
	// SAME limits the pistol's forms use, so the SMG cannot be configured into a state the pistol
	// could not reach.
	//
	// THE KNIFE FALLS THROUGH TO THE PISTOL'S NUMBERS deliberately. It has no clip and no reload, so
	// there is no correct answer; returning zero would make a HUD denominator zero and a reload
	// deadline instantaneous, both of which are worse failures than an unused number being 30.
	// Nothing gates on it: ShouldShowAmmo() is already false with a knife out, and the automatic
	// reload in TickReload skips a knife-holding pawn outright.
	// =============================================================================================

	int32 GetClipSize(ETraceEquippedWeapon Weapon)
	{
		if (Weapon == ETraceEquippedWeapon::Smg)
		{
			return FMath::Clamp(UTraceSettings::Get().SmgClipSize, 1, 999);
		}
		return GetClipSize();
	}

	float GetReloadSeconds(ETraceEquippedWeapon Weapon)
	{
		if (Weapon == ETraceEquippedWeapon::Smg)
		{
			return FMath::Clamp(UTraceSettings::Get().SmgReloadSeconds, 0.05f, 10.f);
		}
		return GetReloadSeconds();
	}

	float GetBaseFireInterval(ETraceEquippedWeapon Weapon)
	{
		// The 0.01 floor is the pistol's, unchanged: it is what stops a mistyped ini from producing a
		// divide-by-nothing gun. 600 RPM is 0.1, a full order of magnitude above it.
		if (Weapon == ETraceEquippedWeapon::Smg)
		{
			return FMath::Max(0.01f, UTraceSettings::Get().SmgFireInterval);
		}
		return FMath::Max(0.01f, UTraceSettings::Get().FireInterval);
	}

	float GetFalloffAlpha(ETraceEquippedWeapon Weapon, double DistanceUU)
	{
		// =========================================================================================
		// SPEC v29 §2d — "The values should drop to 24, 15, 10 after a certain range. 800 uu falloff"
		//
		// *** A CLIFF, NOT A RAMP, AND THE CHOICE IS STATED RATHER THAN IMPLIED. *** SmgFalloffRampUU
		// is 0 in the shipped config, which makes this function a step: 0 at or inside the start,
		// 1 past it. Give the knob a positive length and the same function becomes a linear ramp of
		// that length, with no other code changing. The reasoning for picking the cliff is on the
		// knob in TraceSettings.h and in DefaultGame.ini; the short version is that the owner gave
		// two tables and ONE distance, and a ramp would need a second distance nobody specified.
		//
		// SMG ONLY. §2d: "Only the SMG; the pistol is unchanged." The pistol never reaches the branch
		// below, so a pistol shot at 30000 uu pays exactly what it always did.
		// =========================================================================================
		if (Weapon != ETraceEquippedWeapon::Smg)
		{
			return 0.f;
		}

		const UTraceSettings& Settings = UTraceSettings::Get();
		if (!Settings.bSmgDamageFalloff)
		{
			// THE RED ARM: the flat v28 SMG, one table at every range.
			return 0.f;
		}

		const double Start = FMath::Max(0.f, Settings.SmgFalloffStartUU);
		if (DistanceUU <= Start)
		{
			return 0.f;
		}

		// THE RAMP LENGTH IS MEASURED FROM THE START, WHICH IS WHY THE END IS DERIVED HERE AND NOT
		// STORED (standing rule): move SmgFalloffStartUU and the ramp moves with it, keeping its
		// length, instead of being silently stretched, squashed or inverted by a stale end distance.
		const double Ramp = FMath::Max(0.f, Settings.SmgFalloffRampUU);
		if (Ramp <= UE_KINDA_SMALL_NUMBER)
		{
			return 1.f;   // the cliff
		}

		return static_cast<float>(FMath::Clamp((DistanceUU - Start) / Ramp, 0.0, 1.0));
	}

	float GetZoneDamage(ETraceEquippedWeapon Weapon, ETraceHitZone Zone, double DistanceUU)
	{
		// THE PISTOL STILL GOES THROUGH FTraceHitZoneModel AND MUST. That is the shared zone model
		// spec section 6 insists the predicted and authoritative traces both use, it is what
		// UTraceDamageSettings feeds, and it is what the Trace.ShotStats histogram is calibrated
		// against. This function adds a SECOND TABLE for a second weapon; it does not replace the
		// first, and a build with no SMG in it resolves exactly the numbers it always did.
		//
		// SPEC v29 §2a: that table's leg number is now 25 rather than 30, which happens entirely
		// inside UTraceDamageSettings and changes nothing here.
		if (Weapon != ETraceEquippedWeapon::Smg)
		{
			return FTraceHitZoneModel::DamageForZone(Zone);
		}

		const UTraceSettings& Settings = UTraceSettings::Get();

		float Near = 0.f;
		float Far = 0.f;
		switch (Zone)
		{
		case ETraceHitZone::Head: Near = Settings.SmgHeadDamage; Far = Settings.SmgFarHeadDamage; break;
		case ETraceHitZone::Body: Near = Settings.SmgBodyDamage; Far = Settings.SmgFarBodyDamage; break;
		case ETraceHitZone::Legs: Near = Settings.SmgLegDamage;  Far = Settings.SmgFarLegDamage;  break;
		default:                  return 0.f;    // ETraceHitZone::None — a miss pays nothing.
		}

		// SPEC v29 §2d. Lerp rather than a branch so the cliff and the ramp are ONE expression: with
		// Alpha pinned to 0 or 1 by the cliff this is exactly "pick a table", and there is no second
		// code path for a designer's ramp to be missing from.
		const float Alpha = GetFalloffAlpha(Weapon, DistanceUU);
		return FMath::Max(0.f, FMath::Lerp(FMath::Max(0.f, Near), FMath::Max(0.f, Far), Alpha));
	}

	float GetZoneDamage(ETraceEquippedWeapon Weapon, ETraceHitZone Zone)
	{
		// Point blank. See the header for why this form still exists and what it is for.
		return GetZoneDamage(Weapon, Zone, 0.0);
	}

	bool IsFullAuto(ETraceEquippedWeapon Weapon)
	{
		// SPEC v29 §2b. The knife falls through to false: it has no trigger, and its repeat is
		// CanSwing()'s own 0.5 s cadence, which fire mode must not touch.
		const UTraceSettings& Settings = UTraceSettings::Get();
		switch (Weapon)
		{
		case ETraceEquippedWeapon::Gun: return Settings.bPistolFullAuto;
		case ETraceEquippedWeapon::Smg: return Settings.bSmgFullAuto;
		default:                        return false;
		}
	}

	bool IsEnabled()             { return CVarAmmoEnabled.GetValueOnAnyThread() != 0; }
	bool DoesReloadBlockFire()   { return CVarAmmoReloadBlocksFire.GetValueOnAnyThread() != 0; }
	bool IsCarrierGuardArmed()   { return CVarAmmoCarrierGuard.GetValueOnAnyThread() != 0; }
	bool IsPredictionEnabled()   { return CVarAmmoPredict.GetValueOnAnyThread() != 0; }

	int32 GetCarrierRoundsConsumed() { return GAmmoCarrierRoundsConsumed; }
	int32 GetRoundsConsumed()        { return GAmmoRoundsConsumed; }
	int32 GetReloadsCompleted()      { return GAmmoReloadsCompleted; }
	int32 GetDryFireRefusals()       { return GAmmoDryFireRefusals; }
	int32 GetReloadFireRefusals()    { return GAmmoReloadFireRefusals; }

	void ResetCounters()
	{
		GAmmoCarrierRoundsConsumed = 0;
		GAmmoRoundsConsumed = 0;
		GAmmoReloadsCompleted = 0;
		GAmmoDryFireRefusals = 0;
		GAmmoReloadFireRefusals = 0;
	}
}

static TAutoConsoleVariable<float> CVarTraceShotStatsInterval(
	TEXT("Trace.ShotStats.Interval"),
	20.f,
	TEXT("Seconds between automatic Trace.ShotStats dumps. 0 disables the automatic dump."),
	ECVF_Default);

namespace TraceShotStats
{
	/** Height buckets, as twentieths of the target's full capsule height (0.05 = ~8.8 uu). */
	constexpr int32 NumHeightBuckets = 20;

	struct FStats
	{
		// --- what the local input path did ---
		int32 LocalShotsFired = 0;
		int32 RefusedCarrier = 0;
		int32 RefusedDead = 0;

		// --- what the server did with them ---
		int32 ServerShotsAccepted = 0;
		int32 ServerRejectedRate = 0;
		int32 ServerRejectedState = 0;
		int32 ServerRejectedPayload = 0;

		// --- outcomes ---
		int32 ZoneCount[4] = { 0, 0, 0, 0 };          // None / Head / Body / Legs
		int32 AltZoneCount[4] = { 0, 0, 0, 0 };       // same, classified at closest approach
		int32 ZoneReclassified = 0;                   // entry-point verdict != closest-approach verdict
		int32 ReclassLegsToBody = 0;
		int32 ReclassBodyToLegs = 0;

		int32 HeightHistogram[NumHeightBuckets] = {};
		double HeightFractionSum = 0.0;
		int32 HeightSamples = 0;

		// --- pose provenance ---
		int32 VictimPoseRewound = 0;
		int32 VictimPoseLive = 0;
		int32 VictimNonStandingPosture = 0;           // PostureScale < 0.99, i.e. mid-slide
		double PostureSum = 0.0;

		// --- world geometry interaction ---
		int32 WorldTruncated = 0;
		int32 WorldStartPenetrating = 0;
		int32 WorldTruncatedUnder200 = 0;

		// --- predicted vs authoritative ---
		int32 PredictionComparisons = 0;
		int32 PredictionAgree = 0;
		int32 PredictionZoneMismatch = 0;
		int32 PredictionVictimMismatch = 0;

		double LastDumpTime = 0.0;
	};

	static FStats GStats;

	static void Reset()
	{
		const double Keep = GStats.LastDumpTime;
		GStats = FStats();
		GStats.LastDumpTime = Keep;
	}

	static float Percent(int32 Part, int32 Whole)
	{
		return (Whole > 0) ? (100.f * static_cast<float>(Part) / static_cast<float>(Whole)) : 0.f;
	}

	static void Dump()
	{
		const FStats& S = GStats;
		const int32 Hits = S.ZoneCount[1] + S.ZoneCount[2] + S.ZoneCount[3];

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE SHOT STATS =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT input     : local fired %d | refused carrier %d, dead %d"),
			S.LocalShotsFired, S.RefusedCarrier, S.RefusedDead);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT server    : accepted %d | rejected rate %d, state %d, payload %d"),
			S.ServerShotsAccepted, S.ServerRejectedRate, S.ServerRejectedState, S.ServerRejectedPayload);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT outcome   : hits %d (%.1f%% of accepted) | misses %d"),
			Hits, Percent(Hits, S.ServerShotsAccepted), S.ZoneCount[0]);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT ZONES     : HEAD %d (%.1f%%)  BODY %d (%.1f%%)  LEGS %d (%.1f%%)   [of %d hits]"),
			S.ZoneCount[1], Percent(S.ZoneCount[1], Hits),
			S.ZoneCount[2], Percent(S.ZoneCount[2], Hits),
			S.ZoneCount[3], Percent(S.ZoneCount[3], Hits),
			Hits);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT ZONES-alt : HEAD %d (%.1f%%)  BODY %d (%.1f%%)  LEGS %d (%.1f%%)   [classified at closest approach]"),
			S.AltZoneCount[1], Percent(S.AltZoneCount[1], Hits),
			S.AltZoneCount[2], Percent(S.AltZoneCount[2], Hits),
			S.AltZoneCount[3], Percent(S.AltZoneCount[3], Hits));
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT reclassify: %d of %d hits (%.1f%%) change tier | legs->body %d, body->legs %d"),
			S.ZoneReclassified, Hits, Percent(S.ZoneReclassified, Hits),
			S.ReclassLegsToBody, S.ReclassBodyToLegs);

		if (S.HeightSamples > 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("SHOTSTAT height    : mean impact at %.3f of body height (feet 0.00, crown 1.00). Hip band is at 0.46, head sphere centre 0.905."),
				S.HeightFractionSum / static_cast<double>(S.HeightSamples));

			FString Histogram;
			for (int32 Bucket = 0; Bucket < NumHeightBuckets; ++Bucket)
			{
				Histogram += FString::Printf(TEXT("  %.2f-%.2f %5d (%4.1f%%)%s"),
					Bucket / static_cast<float>(NumHeightBuckets),
					(Bucket + 1) / static_cast<float>(NumHeightBuckets),
					S.HeightHistogram[Bucket], Percent(S.HeightHistogram[Bucket], S.HeightSamples),
					((Bucket % 4) == 3) ? TEXT("\n") : TEXT(""));
			}
			UE_LOG(LogTraceGame, Display, TEXT("SHOTSTAT histogram (closest-approach height):\n%s"), *Histogram);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT pose      : rewound %d, live %d | mid-slide victims %d (%.1f%%), mean posture %.4f"),
			S.VictimPoseRewound, S.VictimPoseLive, S.VictimNonStandingPosture,
			Percent(S.VictimNonStandingPosture, Hits),
			(Hits > 0) ? (S.PostureSum / Hits) : 1.0);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT world     : truncated %d (%.1f%%) | start-penetrating %d | truncated under 200uu %d"),
			S.WorldTruncated, Percent(S.WorldTruncated, S.ServerShotsAccepted),
			S.WorldStartPenetrating, S.WorldTruncatedUnder200);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT predict   : %d compared | AGREE %d (%.2f%%) | zone mismatch %d | victim mismatch %d"),
			S.PredictionComparisons, S.PredictionAgree, Percent(S.PredictionAgree, S.PredictionComparisons),
			S.PredictionZoneMismatch, S.PredictionVictimMismatch);
		UE_LOG(LogTraceGame, Display, TEXT("======================================"));
	}
} // namespace TraceShotStats

static FAutoConsoleCommand GTraceShotStatsDumpCmd(
	TEXT("Trace.ShotStats.Dump"),
	TEXT("Prints the accumulated hit-zone / impact-height distribution gathered while Trace.ShotStats is 1."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceShotStats::Dump(); }));

static FAutoConsoleCommand GTraceShotStatsResetCmd(
	TEXT("Trace.ShotStats.Reset"),
	TEXT("Clears the accumulated shot statistics."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceShotStats::Reset(); }));

UTraceWeaponComponent::UTraceWeaponComponent()
{
	// TICKS ALWAYS, AS OF THE KNIFE (spec v10 §1). It used to tick only while the trigger was held
	// on the machine owning the input, and that is no longer sufficient in three separate ways:
	//
	//   * the SERVER has to sample this pawn's facing every frame, or a back-stab cannot be judged
	//     against the yaw the attacker actually saw (GetFacingYawAtTime);
	//   * the SWING has a wind-up, so the blade resolves on a later frame than the press;
	//   * EVERY rendering machine has to keep the knife rigs' visibility in step with a replicated
	//     selector, including the machines watching somebody else's pawn.
	//
	// The cost is one early-outing tick per pawn per frame, which is what
	// UTraceLagCompensationComponent already pays on the same actor.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Replicated for its RPCs to route AND, since the knife, for two properties of its own:
	// EquippedWeapon and DeployEndServerTime.
	SetIsReplicatedByDefault(true);
}

void UTraceWeaponComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// TO EVERYONE, not COND_SkipOwner, and both for the same reason: the movement component reads
	// the selector on every machine to apply the knife's +22% ground speed, so a client that did not
	// receive its own value would be corrected into the speed instead of predicting it. The owner
	// having predicted the same value already makes the update a no-op, not a fight.
	DOREPLIFETIME(UTraceWeaponComponent, EquippedWeapon);
	DOREPLIFETIME(UTraceWeaponComponent, DeployEndServerTime);

	// --- AMMO (spec v16 §1) ---------------------------------------------------------------------
	//
	// COND_OwnerOnly, and that is the OPPOSITE choice from the two above, deliberately. The selector
	// has to be on every machine because the movement component reads it on every machine; ammo is
	// read by exactly one thing — the owning player's own HUD, in the bottom right. Nothing in this
	// game draws another player's ammo, nothing gates on it, and the third-person rig has no
	// magazine to show. Sending four bytes per pawn to every client for a number none of them can
	// see would be a pure bandwidth cost.
	//
	// *** THE CONSEQUENCE, STATED SO THE NEXT READER DOES NOT HAVE TO FIND IT: *** on a simulated
	// proxy these stay at their defaults forever. GetClipAmmo() says so; do not build "he's out of
	// ammo, push him" on another pawn's count without widening this condition first.
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, ClipAmmo,            COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, ClipSerial,          COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, AbilityRoundsInClip, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, ReloadEndServerTime, COND_OwnerOnly);

	// SPEC v28 §9 — the other gun's magazine. COND_OwnerOnly for exactly the reason the four above
	// are: nobody draws another player's ammo, and this is two more bytes per pawn that no client
	// could put on screen. NO OnRep of its own — ClipSerial moves on every swap, so OnRep_Ammo (which
	// ClipAmmo and ClipSerial already trigger) is where the reconcile happens, once.
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, LiveClipOwner,          COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, StowedGunClipAmmo,     COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, StowedGunAbilityRounds, COND_OwnerOnly);
}

void UTraceWeaponComponent::BeginPlay()
{
	Super::BeginPlay();

	// --- AMMO: the pawn starts with a full clip (spec v16 §1) ------------------------------------
	//
	// *** BEFORE THE EARLY RETURN BELOW, AND THAT ORDER IS LOAD-BEARING. *** Everything after this
	// block is about the local human's camera boom and bails out for a bot, a proxy and a dedicated
	// server's copy of anybody. Seeding the clip down there would have left every bot in the match
	// with ClipAmmo at its class default of 0 — thirty players unable to fire a single round, which
	// is a bug that would have read as "the ammo system broke the bots".
	//
	// Authority only: a client receives the count by replication, and a local write here would just
	// be a number it briefly believed in before the server told it otherwise.
	if (const AActor* AmmoOwner = GetOwner(); AmmoOwner != nullptr && AmmoOwner->HasAuthority())
	{
		// The weapon actually in hand (the pistol, for every pawn that has not pressed 2 yet)...
		RefillClip(TraceAmmo::GetClipSize(EquippedWeapon), 0);

		// ...AND THE OTHER GUN, which is the half that is easy to forget and which fails LOUDLY when
		// it is: StowedGunClipAmmo's class default is 0, so a pawn that pressed 2 for the first time
		// would draw an SMG with an empty magazine and immediately go into an 0.8 s automatic reload.
		// Seeded here, beside the live clip, so the two cannot get out of step.
		const ETraceEquippedWeapon OtherGun = (EquippedWeapon == ETraceEquippedWeapon::Smg)
			? ETraceEquippedWeapon::Gun
			: ETraceEquippedWeapon::Smg;
		StowedGunClipAmmo = static_cast<uint8>(FMath::Clamp(TraceAmmo::GetClipSize(OtherGun), 0, 255));
		StowedGunAbilityRounds = 0;
	}

	// -------------------------------------------------------------------------------------------
	// TICK ORDER, AND THE ONE-FRAME CAMERA LAG IT FIXES.
	//
	// The recoil kick is written to the control rotation from this component's tick. The camera boom
	// reads the control rotation from ITS tick (USpringArmComponent::UpdateDesiredArmLocation via
	// GetTargetRotation) and moves the camera to match. Both live in TG_PrePhysics, and component
	// tick order inside a group is registration order - the boom is created in the character's
	// constructor, so it ticks FIRST and the camera renders one frame behind every kick.
	//
	// MEASURED, before this call existed: Trace.TestRecoil reported aimErr = 0.8000 deg on the frame
	// after the first shot - exactly one kick - against a project guarantee of 0.0000. The bullet was
	// never wrong (the shot is sampled and sent BEFORE the kick, so it goes where the crosshair was),
	// but the probe that guards the guarantee could not tell those two things apart, and neither
	// could the next person to read its output.
	//
	// One prerequisite edge fixes it: the boom updates after this component, so the kick and the
	// camera that renders it land in the same frame. This is an engine API called on their component
	// rather than an edit to their file, and it is a no-op for every pawn that is not a local human -
	// bots have no camera boom worth ordering and no recoil to order it against.
	// -------------------------------------------------------------------------------------------
	if (GetRecoilController() == nullptr)
	{
		return;
	}

	if (const AActor* OwnerActor = GetOwner())
	{
		if (USpringArmComponent* CameraBoom = OwnerActor->FindComponentByClass<USpringArmComponent>())
		{
			CameraBoom->AddTickPrerequisiteComponent(this);
		}
	}
}

ATraceCharacter* UTraceWeaponComponent::GetTraceCharacter() const
{
	return Cast<ATraceCharacter>(GetOwner());
}

double UTraceWeaponComponent::GetServerTimeSeconds() const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0.0;
	}

	// ATraceGameState inherits the engine's replicated shared clock; on the server this is simply
	// world time, on a client it is world time plus the replicated delta.
	if (const ATraceGameState* TraceGameState = World->GetGameState<ATraceGameState>())
	{
		return TraceGameState->GetServerWorldTimeSeconds();
	}
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return World->GetTimeSeconds();
}

double UTraceWeaponComponent::GetLocalTimeSeconds() const
{
	const UWorld* World = GetWorld();
	return (World != nullptr) ? World->GetTimeSeconds() : 0.0;
}

bool UTraceWeaponComponent::CanFire() const
{
	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return false;
	}
	if (!Character->IsAlive())
	{
		return false;
	}
	if (Character->IsCarrier())
	{
		// Spec §4: the carrier cannot shoot. Note this is NOT symmetrical with the shield any more —
		// the shield drops for the duration of a pass (ATraceCore::IsShieldSuppressedFor) but the gun
		// stays locked, which is the whole risk beat: mid-pass you are shootable and unarmed.
		//
		// ATraceCharacter::DoFirePressed also routes a carrier's mouse1 to the pass instead of here,
		// so this gate is a second, independent guarantee rather than the only one.
		return false;
	}

	// --- SPEC v10 §1: the knife is out, so the gun is not ---------------------------------------
	//
	// [DUALWIELD] IsFirearmEquipped() rather than !IsKnifeEquipped(), and the change is not cosmetic:
	// spec v28 §9 added a third selector value, so "not the knife" and "a gun" stopped being the same
	// sentence the moment the SMG existed. Under the v28 §10 switch no pawn is ever in the Knife
	// state at all and this gate never fires; with the switch off it behaves exactly as it always
	// did, because Knife is the only non-firearm there is.
	if (!IsFirearmEquipped())
	{
		return false;
	}

	// --- SPEC v10 §1: 0.2s of pullout, during which NEITHER weapon works -------------------------
	if (IsDeploying())
	{
		return false;
	}

	// --- SPEC v28 §10: "Meleeing should lock the player out of shooting for the length of the
	//     animation." -----------------------------------------------------------------------------
	//
	// [DUALWIELD] A GATE, NOT A COOLDOWN, exactly like the dash gate above it: it is a pure function
	// of the swing stamp and the animation length, so it opens on the frame the animation ends and
	// there is nothing to expire, reset or leak. GetShootLockoutRemaining() returns 0 outright when
	// the switch is off — with the knife selected the gun could not fire anyway, so the rule has
	// nothing to do in the v27 build and adds no behaviour to a revert.
	//
	// SITED HERE, ABOVE THE AMMO COUNTERS, ON PURPOSE. A shot refused because the player is mid-swing
	// is not a dry fire and must not move GAmmoDryFireRefusals, or the ammo harness's headline number
	// would start counting melee.
	if (GetShootLockoutRemaining() > 0.f)
	{
		return false;
	}

	// --- SPEC v10 §6: "Don't let players shoot while in a dash animation. As soon as they end the
	//     dash, let them shoot again." ------------------------------------------------------------
	//
	// A GATE, NOT A COOLDOWN, and the distinction is the whole of the request: this is a pure
	// function of IsDashing(), so it opens on the exact frame the dash state clears. There is no
	// timer, nothing to expire, and nothing that could hold the trigger shut for a frame afterwards.
	//
	// The same gate applies to the knife (CanSwing) — a swing during a dash would be the same
	// exploit wearing a different weapon, and spec §1 folds the two together. Both gates go through
	// ATraceCharacter::AreWeaponActionsBlocked, which is the movement slice's own named accessor, so
	// there is ONE definition of "the dash blocks the trigger" rather than two IsDashing() calls
	// that can drift apart.
	if (Character->AreWeaponActionsBlocked())
	{
		return false;
	}

	// --- SPEC v16 §1: AMMO. "30 bullets per clip, then the gun reloads." -------------------------
	//
	// TWO REFUSALS, NOT ONE, AND THEY ARE COUNTED SEPARATELY. An empty clip and a running reload feel
	// identical through the trigger and mean completely different things to anyone reading a log: the
	// first says the automatic reload has not started yet (or has been cancelled), the second says it
	// is doing its job. A single "no ammo" counter would have made those indistinguishable, which is
	// the shape of the bug where a gun stops working and nobody can say why.
	//
	// Both sit BELOW the dash/pullout/knife gates on purpose, so the counters only move for a shot
	// that ammo was genuinely the reason for.
	if (TraceAmmo::IsEnabled())
	{
		if (GetClipAmmo() <= 0)
		{
			++GAmmoDryFireRefusals;
			return false;
		}
		if (TraceAmmo::DoesReloadBlockFire() && IsReloading())
		{
			++GAmmoReloadFireRefusals;
			return false;
		}
	}

	// SPEC v18 §2 — the per-character fire rate. Roxie's Modded is "fire rate x1.65" and Slimeball's
	// stuck passive is "+30% fire rate"; both arrive here as a scale ON THE INTERVAL, i.e. a number
	// BELOW 1 for a faster gun (1/1.65 = 0.606, 1/1.30 = 0.769). The gun multiplies and never divides
	// — dividing by 1.65 here would make Roxie fire slower while her card, her HUD and her ability log
	// all said faster, which reads in a playtest as "the ability does nothing" rather than as a bug.
	//
	// One character-agnostic call, exactly like TraceAbilityWeaponHooks::OnBulletHit above it: the gun
	// must not learn the name of a character, or the third character that needs a fire-rate change
	// adds a third cast next to the first two. 1.0 for everybody else, including every Mannequin and
	// every bot, so nothing about the base gun's cadence moves.
	//
	// SPEC v28 §9 — and this line is the whole of "the modifiers must apply to the SMG the same way
	// they apply to the pistol". GetFireInterval() is base-of-the-weapon-in-hand TIMES the same
	// scale, so the SMG inherited Roxie and Slimeball without either ability changing.
	return (GetLocalTimeSeconds() - LastLocalFireTime) >= GetFireInterval();
}

double UTraceWeaponComponent::GetFireInterval() const
{
	// *** THE ONE PLACE THE BASE AND THE MODIFIER MEET, AND THERE ARE EXACTLY TWO CALLERS: the
	// client's CanFire() and the server's ServerFire(). *** Spec v18 §2 shipped with only the client
	// half scaled, and the server then rate-limited every second round of a Roxie burst — which reads
	// in game as the gun eating bullets, i.e. strictly worse than the ability doing nothing. One
	// function with two callers is what makes that class of bug unavailable.
	//
	// The scale is a RATE expressed against a PERIOD, so it MULTIPLIES: x1.65 fire rate arrives here
	// as 1/1.65 = 0.606. Dividing would make a faster character fire slower.
	return static_cast<double>(TraceAmmo::GetBaseFireInterval(EquippedWeapon))
		* static_cast<double>(UTraceAbilityComponent::GetFireIntervalScaleFor(GetTraceCharacter()));
}

bool UTraceWeaponComponent::IsFullAutoNow() const
{
	// SPEC v29 §2b. The weapon's own mode, OR'd with the abilities that force full auto.
	//
	// *** AND THIS IS THE LINE THAT FINALLY MAKES ROXIE'S §2 "the gun becomes full auto" A MECHANIC.
	// *** UTraceAbilitySetRoxie::IsFullAutoForced() has existed since spec v18 §2 reading nothing at
	// all, because the base gun was already automatic and the clause had no state to change. With the
	// pistol semi-automatic there is finally something for it to switch, and MODDED now buys her a
	// held trigger on a PISTOL as well as a faster one.
	//
	// OR, not override, and in this direction on purpose: an ability may add full auto to a gun that
	// lacks it, and can never take it away from a gun that has it. A "MODDED makes the SMG single
	// shot" reading is not available, which is the correct outcome for a buff.
	return TraceAmmo::IsFullAuto(EquippedWeapon) || TraceRoxie::IsFullAutoForcedFor(GetOwner());
}

void UTraceWeaponComponent::AdvanceFireClock()
{
	// *** SPEC v29 §2f. THE 537 RPM FIX. The full reasoning is on the declaration in the header. ***
	const double Now = GetLocalTimeSeconds();
	const double Interval = GetFireInterval();

	// The instant this round was DUE, and how late this frame is in delivering it.
	const double Due = LastLocalFireTime + Interval;
	const double Overshoot = Now - Due;

	// CLAMPED TO FireRateTolerance IN CODE, NOT TRUSTED FROM THE INI. That constant is the fraction
	// of an interval the server forgives an early round; carrying more than it would let the client
	// ask for a round the server rejects as rate-limited — the gun eating bullets, which is strictly
	// worse than the 10% it would be curing. One rule, one number, and the ini cannot widen it.
	const double CarryFraction = FMath::Clamp(
		static_cast<double>(UTraceSettings::Get().FireIntervalCarryFraction), 0.0, FireRateTolerance);
	const double CarryCap = Interval * CarryFraction;

	// =============================================================================================
	// *** THE COLD-START GUARD. FOUND BY MEASUREMENT DURING INTEGRATION; THE COMMENT HERE USED TO
	// *** CLAIM THE OPPOSITE. ***
	// =============================================================================================
	//
	// This read `Now - FMath::Clamp(Overshoot, 0.0, CarryCap)` and the comment above it said "the
	// first round of a session, of a life or of a burst has a Due far in the past and falls through
	// to stamp now — there is no cold-start credit to bank". IT DID NOT FALL THROUGH. A Due far in
	// the past makes Overshoot enormous, the clamp pins it to CarryCap, and the round is stamped a
	// FULL CARRY CAP IN THE PAST — so the round AFTER it becomes due CarryCap early. At the shipped
	// 0.2 that is 20% early: 0.2526 s on the pistol instead of 0.3158 s, and 0.0800 s on the SMG
	// instead of 0.1000 s, which is exactly the server's FireRateTolerance floor. Every pause longer
	// than one interval re-banked the credit, so any deliberate double-tap collected it.
	//
	// HOW IT WAS FOUND, because it is a good example of why two instruments beat one: Trace.Weapons.V29
	// measures ONE long held burst and reports the mean of 56 gaps, in which a single early round is
	// worth 0.4% and vanishes. Trace.FireRate.Measure uses five-gap windows that each START from an
	// idle gun, and read 0.3000 s against a 0.3158 s knob (-5.0%) and 0.3796 s against 0.4000 s
	// (-5.1%) — the same defect twice, at two different intervals, at the size this predicts:
	// (4 x Interval + (Interval - CarryCap)) / 5.
	//
	// THE RULE NOW: a round may only inherit overshoot that a RUNNING clock produced. An overshoot
	// larger than one whole interval means the gun was idle rather than running late, and idleness
	// earns nothing. Continuous fire is untouched — its overshoot is one frame, far below Interval —
	// so the 600 RPM the §2f fix bought is unchanged, and CarryFraction 0 is still exactly the
	// shipped 537 RPM red arm.
	const double Carry = (Overshoot <= Interval) ? FMath::Clamp(Overshoot, 0.0, CarryCap) : 0.0;
	LastLocalFireTime = Now - Carry;
}

float UTraceWeaponComponent::GetShootLockoutRemaining() const
{
	// [DUALWIELD] The rule exists only under the spec v28 §10 switch. With the knife as a separate
	// weapon there is no state in which a player is mid-swing AND holding a gun, so returning 0 here
	// is not a disabled feature — it is a feature with no reachable state, and saying so in one line
	// keeps the revert from having to reason about a lockout that can never fire.
	if (!TraceMelee::IsDualWieldEnabled())
	{
		return 0.f;
	}

	const double AnimSeconds = static_cast<double>(TraceMelee::GetSwingAnimSeconds());
	const double Now = GetLocalTimeSeconds();

	// THE SWINGING MACHINE'S OWN STAMP, taken at the PRESS. This is the one the player feels.
	double Remaining = (SwingAnimStartLocalTime + AnimSeconds) - Now;

	// THE SERVER'S, when this is the authority. ServerSwing arrives at the RESOLVE instant, which is
	// SwingWindupSeconds after the press, so the server's view of "the animation ends" is that much
	// later in its own local clock. Derived from the same two knobs rather than being a third number:
	// retune the wind-up and this moves with it. Clamped at zero so a wind-up longer than the whole
	// animation (impossible through the clamps, but the arithmetic should not care) cannot produce a
	// negative allowance that shortens the lockout.
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor != nullptr && OwnerActor->HasAuthority())
	{
		const double PostResolve = FMath::Max(0.0, AnimSeconds - static_cast<double>(TraceMelee::GetSwingWindupSeconds()));
		Remaining = FMath::Max(Remaining, (LastAcceptedSwingTime + PostResolve) - Now);
	}

	return static_cast<float>(FMath::Max(0.0, Remaining));
}

void UTraceWeaponComponent::StartFire()
{
	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		// Input is a local concept: a proxy copy of somebody else's pawn must never fire.
		return;
	}

	bTriggerHeld = true;
	SetComponentTickEnabled(true);

	// SPEC v10 §1 — MOUSE1 IS "ATTACK", NOT "SHOOT".
	//
	// Dispatching here rather than at the input layer is what let the knife land without touching
	// ATraceCharacter, ATracePlayerController or ATraceBotController: DoFirePressed already routes
	// here, so the knife inherits the human bind, the bot burst logic and the dead-player "put me
	// back in" path unchanged. The trigger stays HELD either way, so the tick below repeats a swing
	// at the 0.5 s cadence exactly as it repeats a shot at the fire interval.
	//
	// [DUALWIELD] SPEC v28 §10 TAKES THE DISPATCH BACK OUT, and the condition is the whole of it.
	// "Melee should be bound to right click by default" means mouse 1 is a TRIGGER again and nothing
	// else — the branch below is unreachable under the switch, because no pawn is ever in the Knife
	// state. Melee arrives instead through TraceMelee::HandleMeleeInput, which is a different bind.
	// The line is left as a condition rather than deleted so that flipping the switch restores the
	// v10 §1 input model with no code to put back.
	if (IsKnifeEquipped())
	{
		ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
		if (!StartSwing(&Refusal) && TraceMelee::IsDebugLoggingEnabled())
		{
			UE_LOG(LogTraceGame, Display, TEXT("[Knife] %s swing refused on press: %s"),
				*GetNameSafe(Character), LexToString(Refusal));
		}
		return;
	}

	// A gun that is silent BY DESIGN is indistinguishable from broken input, and this rule cost real
	// debugging time once already. Say so, once per press, at Verbose.
	if (Character->IsCarrier())
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[%s] Fire refused: carrying the Core (carriers trade the gun for bullet immunity)."),
			*GetNameSafe(Character));

		if (CVarTraceShotStats.GetValueOnGameThread() != 0)
		{
			++TraceShotStats::GStats.RefusedCarrier;
		}
	}
	else if (!Character->IsAlive() && CVarTraceShotStats.GetValueOnGameThread() != 0)
	{
		++TraceShotStats::GStats.RefusedDead;
	}

	if (CanFire())
	{
		FireOnce();
		return;
	}

	// --- FX_AUDIO_PLAN §5.1 (DryFire) — THE CLICK THAT SAYS "EMPTY", AND ONLY THAT ---------------
	//
	// CLIENT-SIDE AND UNCONDITIONALLY LOCAL: this is feedback about YOUR trigger, and a room full of
	// other people's empty clicks would be noise with no information in it. TraceAudio::Play routes
	// it by the table (DryFire is declared Client) and refuses to do anything for a bot, so a listen
	// host does not click for every dry bot.
	//
	// *** ONLY FOR THE EMPTY CLIP, NOT FOR EVERY REFUSAL. *** CanFire() says no for eight different
	// reasons — dashing, carrying the Core, deploying, dead, mid-reload, the fire-rate gate — and
	// seven of them are not "the gun is empty". A click on the fire-rate gate in particular would
	// fire on every frame of every burst, which is how a feedback sound becomes a bug report. The
	// test below is the same pair CanFire() itself uses for the ammo branch, read in the same order.
	//
	// The 0.15 s limiter is §5.1's, and it is about the TRIGGER rather than the sound: a held trigger
	// on an empty gun re-enters this function every frame, and an unlimited click there is a buzz.
	if (TraceAmmo::IsEnabled() && GetClipAmmo() <= 0 && Character->IsAlive() && !Character->IsCarrier())
	{
		const double DryNow = GetLocalTimeSeconds();
		if ((DryNow - LastDryFireLocalTime) >= DryFireRepeatSeconds)
		{
			LastDryFireLocalTime = DryNow;
			TraceAudio::Play(Character, TraceSoundEvents::DryFire);
		}
	}
}

// =================================================================================================
// AMMO  (spec v16 §1)
//
//     press    -> CanFire            refuses an empty clip and a running reload
//     shot     -> ConsumeRound       one round, on the machine that owns the decision
//     empty    -> BeginReload        anchored at the SHOT, so both ends compute one deadline
//     +0.5s    -> RefillClip         30 ordinary rounds, and the serial moves
//     R        -> RequestReload      predicted locally, stamped, re-gated on the server
//
// The state, the prediction split and the reasoning behind each field are on the header. This is the
// state machine.
// =================================================================================================

int32 UTraceWeaponComponent::GetClipAmmo() const
{
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor != nullptr && OwnerActor->HasAuthority())
	{
		// The truth, and the only reading that matters for a bot, a listen host's own pawn, or a
		// remote client's pawn as the server sees it.
		return static_cast<int32>(ClipAmmo);
	}

	// A remote owning client. -1 means the first replication has not arrived yet, in which case the
	// replicated field is still the best answer available (and is the class default until then).
	return (PredictedClipAmmo >= 0) ? PredictedClipAmmo : static_cast<int32>(ClipAmmo);
}

int32 UTraceWeaponComponent::GetAbilityRoundsInClip() const
{
	return static_cast<int32>(AbilityRoundsInClip);
}

bool UTraceWeaponComponent::IsReloading() const
{
	// The SHARED clock, exactly as IsDeploying() uses it, and for the same reason: this deadline is
	// computed on one machine and honoured on two, so it has to be expressed in the one clock they
	// agree on. (The fire-RATE gate deliberately uses the local clock instead — a clock resync must
	// never be able to stall the trigger. A reload is a replicated deadline; a fire rate is not.)
	return ReloadEndServerTime > 0.f && GetServerTimeSeconds() < static_cast<double>(ReloadEndServerTime);
}

float UTraceWeaponComponent::GetReloadRemaining() const
{
	if (ReloadEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return static_cast<float>(FMath::Max(0.0, static_cast<double>(ReloadEndServerTime) - GetServerTimeSeconds()));
}

bool UTraceWeaponComponent::ShouldShowAmmo() const
{
	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsAlive())
	{
		return false;
	}

	// *** SPEC v16 §1: "The Core carrier has no gun, so ammo must not be consumed or shown while
	// carrying." *** The HUD asks this rather than asking IsCarrier() itself, so that the "has no
	// gun" rule has ONE definition. The consumption half of the same sentence is enforced twice over
	// — CanFire()/ServerFire() refuse a carrier, and ConsumeRound carries its own guard.
	if (Character->IsCarrier())
	{
		return false;
	}

	// The knife has no magazine. Drawing "30" beside a blade would be worse than drawing nothing.
	//
	// SPEC v28 §9: asked as "is a firearm in hand" rather than "is the knife not in hand", because
	// those stopped being the same question when a third selector value appeared. [DUALWIELD] Under
	// the v28 §10 switch this is simply always true for a living non-carrier — the blade is in the
	// off hand and the gun is still up, so the count belongs on screen the whole time.
	return IsFirearmEquipped();
}

void UTraceWeaponComponent::ConsumeRound()
{
	if (!TraceAmmo::IsEnabled())
	{
		// THE RED ARM. The clip never falls, so nothing ever empties and nothing ever reloads — the
		// build that existed before spec v16 §1, reachable in one CVar.
		return;
	}

	// *** SPEC v16 §1 / v14 §4, THE SECOND CARRIER LOCK. ***
	//
	// CanFire() and ServerFire() both refuse a carrier before a shot can ever reach this function, so
	// in the shipped build this branch is unreachable — which is exactly the argument
	// UTraceHealthComponent::ApplyVulnerable makes for ITS second carrier lock, and exactly why that
	// one exists: the rule has to survive a future caller who reaches the consumption path another
	// way. Both predicates are asked (the Core's authoritative pointer AND the pawn's replicated
	// mirror) so that a frame in which the two disagree cannot open the door.
	const ATraceCharacter* Character = GetTraceCharacter();
	const bool bCarrying = Character != nullptr
		&& (ATraceCore::IsCoreHolder(Character) || Character->IsCarrier());
	if (bCarrying)
	{
		if (TraceAmmo::IsCarrierGuardArmed())
		{
			return;
		}

		// The red arm reached a carrier. Counted and logged as an Error so a harness can prove the
		// arm actually disarmed something rather than reporting a green that had no rule to break.
		++GAmmoCarrierRoundsConsumed;
		UE_LOG(LogTraceGame, Error,
			TEXT("[Ammo] *** A ROUND WAS SPENT FROM THE CORE CARRIER %s's CLIP (round #%d). Spec v16 §1: the "
			     "carrier has no gun. This is only reachable with Trace.Ammo.CarrierGuard 0."),
			*GetNameSafe(GetOwner()), GAmmoCarrierRoundsConsumed);
	}

	const AActor* OwnerActor = GetOwner();
	const bool bAuthority = (OwnerActor != nullptr && OwnerActor->HasAuthority());

	bLastRoundWasAbilityRound = false;

	if (bAuthority)
	{
		if (ClipAmmo == 0)
		{
			return;
		}
		--ClipAmmo;

		// The ability-loaded rounds are spent FIRST and are simply the front of the clip. Recording
		// which kind of round just left is what lets UTraceAbilitySetX::NotifyBulletHit answer "was
		// that a bee round" a few statements later in ServerFire — including for the fifth and last
		// one, where "are there any left" would already say no.
		if (AbilityRoundsInClip > 0)
		{
			--AbilityRoundsInClip;
			bLastRoundWasAbilityRound = true;
		}

		++TotalRoundsConsumed;
		++GAmmoRoundsConsumed;
		return;
	}

	// A predicting client. Seed from the replicated truth on the first shot of the connection, so a
	// player who fires before their first ammo update does not start from -1.
	if (PredictedClipAmmo < 0)
	{
		PredictedClipAmmo = static_cast<int32>(ClipAmmo);
	}
	PredictedClipAmmo = FMath::Max(0, PredictedClipAmmo - 1);
	bLastRoundWasAbilityRound = (AbilityRoundsInClip > 0);

	// Counted on a client too, and the counter means something slightly different there: PREDICTED
	// rounds rather than spent ones. Said out loud because a client-side Trace.Ammo.Dump would
	// otherwise look like it was reporting authoritative numbers.
	++GAmmoRoundsConsumed;
}

void UTraceWeaponComponent::RefillClip(int32 Rounds, int32 AbilityRounds)
{
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority())
	{
		// Clients receive a refill by replication and predict their own through TickReload. A local
		// write here would be a number they briefly believed in before the server contradicted it.
		return;
	}

	const int32 NewRounds = FMath::Clamp(Rounds, 0, 255);
	ClipAmmo = static_cast<uint8>(NewRounds);
	AbilityRoundsInClip = static_cast<uint8>(FMath::Clamp(AbilityRounds, 0, NewRounds));
	ReloadEndServerTime = -1.f;

	// *** THE SERIAL IS THE POINT OF THIS FUNCTION. *** It is what tells an owning client "this is a
	// NEW clip, throw your prediction away" as distinct from "I am simply behind your shots". Without
	// it, OnRep_Ammo could not tell a refill from a stale packet and would have to choose between
	// refusing refills and refunding bullets. Wraps at 255, which is harmless — the reconcile only
	// ever asks whether it CHANGED, never by how much.
	++ClipSerial;

	// Deliberately does NOT call OnRep_Ammo() by hand, which is the opposite of what this project
	// does for Health and the vulnerable mark. Those OnReps do presentation work every machine needs;
	// this one only reconciles the CLIENT's prediction mirror, and the authority has no mirror to
	// reconcile — it reads ClipAmmo directly (see GetClipAmmo).
}

void UTraceWeaponComponent::BeginReload(double AnchorSharedTime)
{
	if (!TraceAmmo::IsEnabled())
	{
		return;
	}

	const double Now = GetServerTimeSeconds();

	// *** THE CLAMP IS THE SECURITY, AND ITS COST IS WORTH STATING IN NUMBERS. ***
	//
	// The anchor comes from the machine that pressed the key or fired the shot, so it is client data
	// and is pinned into [Now - MaxRewindTime, Now] exactly as ServerFire pins a shot's timestamp:
	// never in the future, so a reload cannot be pre-booked, and never further back than a bullet may
	// be rewound.
	//
	// What a liar buys with the full 0.25 s of back-dating is half a reload. Against a 30-round clip
	// at FireInterval 0.40 that is 0.25 s off a 12.5 s cycle — about 2% more sustained DPS, which is
	// an order of magnitude less than the 20% the fire-rate gate already forgives honest jitter
	// (FireRateTolerance). Anchoring is what stops a 40 ms client's reload from actually taking
	// 0.54 s, which is a 9% error in one of the three numbers spec v16 §1 gives; refusing to anchor
	// to avoid a 2% exploit would be trading a real defect for a smaller one.
	const double MaxRewind = FMath::Max(0.f, UTraceSettings::Get().MaxRewindTime);
	const double Anchor = FMath::Clamp(AnchorSharedTime, Now - MaxRewind, Now);

	// SPEC v28 §9 — the weapon in hand decides: 0.5 s for the pistol, 0.8 s for the SMG. Both ends
	// anchor at the same stamped instant AND read the same selector (EquippedWeapon is replicated),
	// so the client's predicted deadline and the server's are still one number rather than two.
	// A RELOAD THAT WAS ALREADY RUNNING IS NOT A NEW RELOAD. Both ends call this function — the
	// client predicts one at the shot that emptied the clip, the authority anchors its own at the
	// same stamp — and RequestReload can arrive on top of either. Sampling the state BEFORE the
	// deadline is written is what makes the sound below fire once per magazine instead of once per
	// caller.
	const bool bWasReloading = IsReloading();

	ReloadEndServerTime = static_cast<float>(Anchor + static_cast<double>(GetReloadSeconds()));
	bPredictedRefillPending = false;

	if (bWasReloading)
	{
		return;
	}

	// --- FX_AUDIO_PLAN §5.1 (Reload) — THE §6 PAIR, ON A SOUND THAT IS NOT A GUNSHOT ------------
	//
	// A reload is the second sound whose delay a player consciously feels: the whole point of hearing
	// it is knowing your own gun is busy, and hearing that half a round trip after the count hit zero
	// is worse than not hearing it. Same shape as the shot, and it is a PAIR — the predicted copy on
	// the owning machine, the multicast for everybody else with that machine excluded. On a listen
	// host both branches run on the same frame and the exclusion cancels the multicast's local copy,
	// so the host hears exactly one.
	ATraceCharacter* ReloadCharacter = GetTraceCharacter();
	if (ReloadCharacter == nullptr)
	{
		return;
	}

	const FVector Where = ReloadCharacter->GetActorLocation();
	const bool bPredictAudio = TraceAudioWatch::IsPredictedShotEnabled();

	if (bPredictAudio && ReloadCharacter->IsLocallyControlled() && ReloadCharacter->IsPlayerControlled())
	{
		TraceAudio::PlayPredictedLocal(ReloadCharacter, TraceSoundEvents::Reload, Where);
	}

	const AActor* ReloadOwner = GetOwner();
	if (ReloadOwner != nullptr && ReloadOwner->HasAuthority())
	{
		if (bPredictAudio)
		{
			TraceAudio::PlayAtExcluding(ReloadCharacter, TraceSoundEvents::Reload, Where, ReloadCharacter);
		}
		else
		{
			TraceAudio::PlayAt(ReloadCharacter, TraceSoundEvents::Reload, Where);
		}
	}
}

void UTraceWeaponComponent::CancelReload()
{
	ReloadEndServerTime = -1.f;
	bPredictedRefillPending = false;
}

void UTraceWeaponComponent::TickReload()
{
	const AActor* OwnerActor = GetOwner();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || Character == nullptr)
	{
		return;
	}

	const bool bAuthority = OwnerActor->HasAuthority();

	// A SIMULATED PROXY OWNS NO AMMO DECISION AT ALL. It has no replicated count (COND_OwnerOnly), so
	// letting it run this state machine would have it "reloading" a clip it cannot see, on numbers
	// that are class defaults. Authority decides; the owning client predicts; everyone else watches.
	if (!bAuthority && !Character->IsLocallyControlled())
	{
		return;
	}

	if (!TraceAmmo::IsEnabled())
	{
		// THE RED ARM: the mechanic is ABSENT, not half-present. Any reload already in flight is
		// abandoned rather than left to complete, so a red run cannot report a reload it inherited
		// from the green build that preceded it.
		if (ReloadEndServerTime > 0.f)
		{
			CancelReload();
		}
		return;
	}

	// --- THE TWO CANCELLATIONS. Spec v16 §1 [ASSUMPTION]: "reloading is cancelled by death and by
	//     picking up the Core." ---------------------------------------------------------------------
	const bool bDead = !Character->IsAlive();
	const bool bCarrying = Character->IsCarrier();
	if (bDead || bCarrying)
	{
		if (ReloadEndServerTime > 0.f)
		{
			CancelReload();
		}

		// A NEW LIFE STARTS WITH A FULL CLIP, and it is done from the death side rather than from a
		// respawn hook because this component has no respawn hook to hang it on — ATraceCharacter is
		// another ownership slice. Refilling while dead is equivalent and needs nothing from anyone
		// else. The guard is what keeps it from bumping the serial (and therefore replicating) sixty
		// times a second for the whole respawn countdown.
		//
		// SPEC v28 §9: BOTH magazines. A player who died with a spent SMG in their pocket must not
		// respawn and find it still spent — the sentence is "a new life starts with a full clip", and
		// a loadout of two guns has two of them. The stowed pair is written directly rather than
		// through RefillClip (which by definition acts on the live clip) and does not need its own
		// serial bump: the live refill below already moves ClipSerial in the same frame, and the
		// client's reconcile reads the whole set.
		if (bAuthority && bDead)
		{
			const ETraceEquippedWeapon OtherGun = (EquippedWeapon == ETraceEquippedWeapon::Smg)
				? ETraceEquippedWeapon::Gun
				: ETraceEquippedWeapon::Smg;
			const uint8 OtherFull = static_cast<uint8>(FMath::Clamp(TraceAmmo::GetClipSize(OtherGun), 0, 255));
			if (StowedGunClipAmmo != OtherFull || StowedGunAbilityRounds != 0)
			{
				StowedGunClipAmmo = OtherFull;
				StowedGunAbilityRounds = 0;
			}
		}

		if (bAuthority && bDead
			&& (static_cast<int32>(ClipAmmo) != GetLiveClipSize() || AbilityRoundsInClip != 0))
		{
			// Same rule on respawn: refill the magazine that is in the gun, not the selector's.
			RefillClip(GetLiveClipSize(), 0);
		}
		return;
	}

	// --- COMPLETION --------------------------------------------------------------------------------
	if (ReloadEndServerTime > 0.f && GetServerTimeSeconds() >= static_cast<double>(ReloadEndServerTime))
	{
		if (bAuthority)
		{
			// THE MAGAZINE'S OWN SIZE, not the selected weapon's — a reload finishing while the
			// knife is out used to refill an SMG clip to the pistol's 30. See GetLiveClipSize().
			RefillClip(GetLiveClipSize(), 0);
			++TotalReloadsCompleted;
			++GAmmoReloadsCompleted;

			UE_LOG(LogTraceGame, Verbose, TEXT("[Ammo] %s reloaded: %d rounds of %s (selector says %s)."),
				*GetNameSafe(GetOwner()), GetLiveClipSize(), LexToString(LiveClipOwner),
				LexToString(EquippedWeapon));
		}
		else
		{
			// THE PREDICTED REFILL. The client's gun comes back up on ITS deadline rather than a ping
			// later, and bPredictedRefillPending is what stops the server's last pre-refill packet
			// from yanking the count back to empty for that ping — see OnRep_Ammo.
			PredictedClipAmmo = GetClipSize();
			bPredictedRefillPending = true;
			ReloadEndServerTime = -1.f;
			++GAmmoReloadsCompleted;
		}
	}

	// --- THE AUTOMATIC RELOAD. "30 bullets per clip, then the gun reloads." ------------------------
	//
	// The ordinary path starts this at the SHOT that emptied the clip (ServerFire / FireOnce), which
	// is what gets both machines onto one deadline. This is the safety net for every other way a pawn
	// can end up standing there with an empty gun: a reload cancelled by picking up the Core and then
	// passing it on, a death mid-reload, a clip emptied by something other than the trigger.
	//
	// *** IT IS ALSO THE WHOLE OF "BOTS MUST RELOAD". *** A bot's pawn is authoritative and locally
	// controlled in the same process, so it runs this identical branch. There is not one line of AI
	// code in the ammo feature, and a bot that dry-fires forever is impossible by construction rather
	// than by a rule somebody remembered to add to the bot controller.
	if (GetClipAmmo() <= 0 && !IsReloading() && IsFirearmEquipped())
	{
		BeginReload(GetServerTimeSeconds());
	}
}

bool UTraceWeaponComponent::RequestReload()
{
	const AActor* OwnerActor = GetOwner();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || Character == nullptr || !TraceAmmo::IsEnabled())
	{
		return false;
	}

	// Every refusal below is a rule; see the header for why each one is where it is.
	if (!Character->IsAlive() || Character->IsCarrier())
	{
		return false;
	}
	if (!IsFirearmEquipped() || IsDeploying())
	{
		return false;
	}
	if (IsReloading())
	{
		return false;
	}
	if (GetClipAmmo() >= GetClipSize())
	{
		// A FULL CLIP: NOTHING AT ALL. [ASSUMPTION], spec v16 §1. Restarting the timer instead would
		// let a player weld their own gun shut for half a second by leaning on R, which no shooter
		// does and which would read as the bind being broken.
		return false;
	}

	// *** AN ABILITY-LOADED CLIP IS NOT MANUALLY RELOADABLE. [ASSUMPTION], spec v16 §1. ***
	//
	// X's Sting puts five bee rounds in the gun off a 25 s cooldown, and R would otherwise throw them
	// on the floor on a mis-press — a whole ability lost to a reflex, with no undo and no feedback.
	// The spec's own sentence assumes they get FIRED ("after shooting those, his next reload is
	// normal"), so refusing here is the reading that matches it. Reverse it by deleting this block if
	// playtesting wants a dump.
	if (AbilityRoundsInClip > 0)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Ammo] %s: manual reload refused — the clip holds %d ability round(s) and they must be fired."),
			*GetNameSafe(GetOwner()), static_cast<int32>(AbilityRoundsInClip));
		return false;
	}

	const double PressTime = GetServerTimeSeconds();

	if (OwnerActor->HasAuthority())
	{
		BeginReload(PressTime);
		return true;
	}

	if (!Character->IsLocallyControlled())
	{
		// A proxy copy of somebody else's pawn. Input is a local concept.
		return false;
	}

	// Predict, then ask. Both ends anchor at PressTime, so the server's copy of the deadline arrives
	// as a no-op rather than as an extension.
	BeginReload(PressTime);
	ServerRequestReload(static_cast<float>(PressTime));
	return true;
}

bool UTraceWeaponComponent::ServerRequestReload_Validate(float ClientPressServerTime)
{
	// Validation failure disconnects the client, so only reject what is impossible to reason about.
	return FMath::IsFinite(ClientPressServerTime);
}

void UTraceWeaponComponent::ServerRequestReload_Implementation(float ClientPressServerTime)
{
	const AActor* OwnerActor = GetOwner();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || Character == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}
	if (!TraceAmmo::IsEnabled())
	{
		return;
	}

	// RE-GATED FROM SCRATCH, never trusted. The client gated on its own copy of all of this before it
	// sent the RPC; that gate is for FEEL, this one is the rule. A client that has been corrected
	// into a state where the reload is illegal simply gets nothing.
	if (!Character->IsAlive() || Character->IsCarrier() || !IsFirearmEquipped() || IsDeploying())
	{
		return;
	}
	if (IsReloading() || ClipAmmo >= static_cast<uint8>(GetClipSize()) || AbilityRoundsInClip > 0)
	{
		return;
	}

	BeginReload(static_cast<double>(ClientPressServerTime));
}

void UTraceWeaponComponent::LoadAbilityClip(int32 RoundCount)
{
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	// REPLACES, never adds — spec v16 §1's [ASSUMPTION], stated there: "Sting overwrites whatever was
	// in the clip (20 rounds left -> 5 bee rounds)". RefillClip clears any running reload too, so an
	// ability cast during a reload puts the gun up immediately instead of stranding the player behind
	// a timer they can no longer see the point of.
	// SPEC v28 §9: clamped to the clip of the weapon X is actually holding, so a Sting cast with the
	// SMG out cannot load more bee rounds than the magazine has room for.
	const int32 Rounds = FMath::Clamp(RoundCount, 0, GetClipSize());
	RefillClip(Rounds, Rounds);

	UE_LOG(LogTraceGame, Log, TEXT("[Ammo] %s: the clip is now %d ability round(s)."),
		*GetNameSafe(GetOwner()), Rounds);
}

void UTraceWeaponComponent::OnRep_Ammo()
{
	// CLIENTS ONLY (replication callbacks never fire on the authority), and its whole job is to
	// reconcile the prediction mirror with the truth. Three cases, and the third is the interesting
	// one.
	if (PredictedClipAmmo < 0)
	{
		// First update of the connection. Adopt everything; there is nothing predicted to protect.
		PredictedClipAmmo = static_cast<int32>(ClipAmmo);
		PredictedClipSerial = ClipSerial;
		bPredictedRefillPending = false;
		return;
	}

	if (ClipSerial != PredictedClipSerial)
	{
		// A NEW CLIP. The server reloaded, an ability replaced the clip, or this pawn respawned. The
		// prediction is about a magazine that no longer exists, so it is discarded outright.
		PredictedClipSerial = ClipSerial;
		PredictedClipAmmo = static_cast<int32>(ClipAmmo);
		bPredictedRefillPending = false;
		return;
	}

	if (bPredictedRefillPending)
	{
		// We have finished a reload the server has not told us about yet, so every packet still in
		// flight describes the OLD, nearly empty clip under the OLD serial. Taking it would drop the
		// count back to 1 or 2 for a ping — at the exact moment the player is reading the number to
		// decide whether to re-engage. Wait for the serial to move.
		return;
	}

	// SAME CLIP, and our shots are ahead of the server's knowledge of them, so the server's count can
	// only be >= ours. Take the LOWER: a stale packet must never hand a round back. The cost of being
	// wrong here is that a shot the server REJECTED (rate limit, a state change) leaves the client one
	// round pessimistic until the next refill re-seats both — which is the right direction to be
	// wrong in, because it can only ever make the client reload early, never fire a round the server
	// will not honour.
	PredictedClipAmmo = FMath::Min(PredictedClipAmmo, static_cast<int32>(ClipAmmo));
}

void UTraceWeaponComponent::StopFire()
{
	// The tick is NOT switched off here, and as of the knife it is never switched off at all — see
	// the constructor. Recoil recovery already needed the component to keep ticking past the
	// release; the facing ring, the swing wind-up and the knife rigs' visibility need it to tick on
	// machines that never pressed anything.
	bTriggerHeld = false;

	// SPEC v29 §2b — THE RELEASE IS THE ONLY THING THAT REARMS A SEMI-AUTOMATIC WEAPON, and this is
	// the only line that clears the latch. "It must fire once per trigger press" is therefore a fact
	// about the press itself rather than a timer, so it cannot drift out of step with the fire rate,
	// cannot be shortened by a low frame rate, and needs nothing to expire.
	bTriggerConsumedThisPress = false;
}

void UTraceWeaponComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		// The pawn is gone: genuine teardown, drop everything. The climb goes with it — there is no
		// view left to hand back to.
		bTriggerHeld = false;
		bSwingPendingResolve = false;
		ResetRecoil();
		SetComponentTickEnabled(false);
		return;
	}

	const AActor* OwnerActor = GetOwner();

	// --- SERVER: the victim-facing ring (spec v10 §1) --------------------------------------------
	//
	// Every frame, on every pawn, including remote clients' proxies and bots. This is the ONE piece
	// of history the knife needed that the gun's lag compensation does not keep — see
	// GetFacingYawAtTime. Eight bytes per frame per pawn.
	if (OwnerActor != nullptr && OwnerActor->HasAuthority())
	{
		RecordFacingSample(static_cast<float>(GetServerTimeSeconds()));
	}

	// --- AMMO: the reload state machine (spec v16 §1) ---------------------------------------------
	//
	// DELIBERATELY ABOVE THE locally-controlled GATE. The server has to run this for every pawn it
	// owns, and most of those — every remote human — are not locally controlled here. Putting it
	// below would have left exactly the players with the worst latency as the ones whose guns never
	// reloaded. TickReload decides for itself which machines own a decision.
	TickReload();

	// --- EVERY MACHINE: the knife's movement profile ---------------------------------------------
	//
	// Deliberately before the locally-controlled gate and unconditional. The bit has to be right on
	// the server, on the owning client that predicts its own moves, AND on the machines simulating
	// this pawn — see RefreshMovementProfile. It is idempotent and only writes on a change.
	RefreshMovementProfile();

	// --- EVERY RENDERING MACHINE: the knife you can see ------------------------------------------
	//
	// Deliberately BEFORE the locally-controlled gate. The third-person knife in an enemy's hand is
	// the tell that they are 22% faster and cannot shoot back, and that tell has to appear on the
	// machines watching them, none of which control that pawn.
	UpdateKnifeVisuals(DeltaTime);

	if (!Character->IsLocallyControlled())
	{
		// Not ours: no input, no recoil, no swing. The climb goes with it.
		bTriggerHeld = false;
		bSwingPendingResolve = false;
		ResetRecoil();
		return;
	}

	if (!Character->IsAlive())
	{
		// Dead. Forget the climb rather than recovering it: the death camera owns the view now, and
		// a recovery that keeps writing control rotation would fight it for the whole respawn. A
		// swing that was mid-wind-up dies with the swinger for the same reason.
		ResetRecoil();
		bSwingPendingResolve = false;
	}
	else
	{
		TickRecoil(DeltaTime);
	}

	// The blade of a swing already committed to. Runs before the trigger check below so a held
	// trigger cannot start the next swing in the same frame the previous one lands.
	TickSwing(DeltaTime);

	// Attacking is gated separately from recoil, and deliberately does NOT clear bTriggerHeld while
	// ineligible: this component only hears about the trigger on press and release, so clearing it
	// means a player who is still physically holding the button gets nothing after passing the Core
	// away or after respawning — they would have to release and press again for no reason they can
	// see. Keep ticking, skip attacking, and resume the instant the gate reopens.
	//
	// *** THIS IS WHERE "FULL AUTO" LIVES, AND IT ALWAYS DID (spec v28 §9). *** A held trigger fires
	// once per CanFire()-legal frame, and CanFire()'s last test is the fire-rate gate, so the cadence
	// is exactly GetFireInterval() for whichever weapon is in hand — 0.3158 s for the pistol,
	// 0.1 s for the SMG.
	//
	// *** SPEC v29 §2b PUTS A FIRE MODE IN FRONT OF IT. *** "The pistol is NOT full auto. It must
	// fire once per trigger press. The SMG stays full auto." Until this pass there was no fire mode
	// anywhere in the codebase and the sentence above was the whole truth — which is exactly why BOTH
	// guns were automatic, including the one the owner has now said must not be.
	//
	// THE REPEAT IS WHAT THE MODE GATES, NOT THE SHOT. StartFire() still fires the first round of
	// every press unconditionally; this loop is the only thing a semi-automatic weapon refuses. That
	// is what makes a pistol press feel identical to an SMG press and only the SECOND round differ,
	// and it means the fire-rate gate below is untouched: a press inside the interval is still
	// refused by CanFire(), semi-automatic or not.
	if (bTriggerHeld && Character->IsAlive() && !Character->IsCarrier())
	{
		if (IsKnifeEquipped())
		{
			if (CanSwing())
			{
				StartSwing();
			}
		}
		else if (!bTriggerConsumedThisPress || IsFullAutoNow())
		{
			if (CanFire())
			{
				FireOnce();
			}
		}
	}
}

// =================================================================================================
// UPWARDS RECOIL  (spec v5 section 6)
// =================================================================================================

APlayerController* UTraceWeaponComponent::GetRecoilController() const
{
	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return nullptr;
	}

	// APlayerController and LOCAL, both load-bearing. A bot's AAIController fails the cast, which is
	// how bots are kept out of the recoil model; a remote client's proxy pawn on the server fails
	// IsLocalController, which is how the server is kept from kicking a view it does not own.
	APlayerController* RecoilController = Cast<APlayerController>(Character->GetController());
	return (RecoilController != nullptr && RecoilController->IsLocalController()) ? RecoilController : nullptr;
}

namespace TraceRecoilMath
{
	/**
	 * Folds any DOWNWARD view movement the player made since our last write into the accumulator,
	 * so their own compensation is not paid back to them as a second kick when the gun settles.
	 *
	 * This is the difference between recoil you can fight and recoil that fights back: without it, a
	 * player who drags 3 degrees down to hold the crosshair on a chest has those 3 degrees taken off
	 * AGAIN by the recovery and ends the burst aiming at the floor.
	 *
	 * Upward player movement is deliberately NOT credited: looking up is not paying off a climb, and
	 * crediting it would let a player bank recovery by flicking upward between shots.
	 */
	void ConsumePlayerCompensation(double CurrentPitch, bool bEnabled, double& InOutAppliedPitch,
		double& InOutTrackedPitch, bool& bInOutTrackingValid)
	{
		if (bInOutTrackingValid && bEnabled)
		{
			const double PlayerDelta = CurrentPitch - InOutTrackedPitch;
			if (PlayerDelta < 0.0)
			{
				InOutAppliedPitch = FMath::Max(0.0, InOutAppliedPitch + PlayerDelta);
			}
		}

		InOutTrackedPitch = CurrentPitch;
		bInOutTrackingValid = true;
	}
}

void UTraceWeaponComponent::AddRecoilPitch(APlayerController* RecoilController, double DeltaPitchDegrees)
{
	if (RecoilController == nullptr || FMath::IsNearlyZero(DeltaPitchDegrees))
	{
		return;
	}

	FRotator ControlRotation = RecoilController->GetControlRotation();
	const double OldPitch = FRotator::NormalizeAxis(ControlRotation.Pitch);

	// The camera manager's own stops, so the climb cannot push the view past the point the player's
	// mouse could reach and then owe them a recovery from somewhere that was never applied.
	double MinPitch = -89.9;
	double MaxPitch = 89.9;
	if (const APlayerCameraManager* CameraLimits = RecoilController->PlayerCameraManager)
	{
		const double LimitLow = FRotator::NormalizeAxis(CameraLimits->ViewPitchMin);
		const double LimitHigh = FRotator::NormalizeAxis(CameraLimits->ViewPitchMax);
		if (LimitLow < LimitHigh)
		{
			MinPitch = LimitLow;
			MaxPitch = LimitHigh;
		}
	}

	const double NewPitch = FMath::Clamp(OldPitch + DeltaPitchDegrees, MinPitch, MaxPitch);
	const double Applied = NewPitch - OldPitch;

	// Read back what LANDED, not what was asked for. Against the pitch stop those differ, and an
	// accumulator that believes the unclamped figure would owe the player a recovery it never
	// applied — the view would sink below where they were aiming when the burst ended.
	if (!FMath::IsNearlyZero(Applied))
	{
		ControlRotation.Pitch = NewPitch;
		RecoilController->SetControlRotation(ControlRotation);
		RecoilAppliedPitch = FMath::Max(0.0, RecoilAppliedPitch + Applied);
	}

	RecoilTrackedPitch = NewPitch;
	bRecoilTrackingValid = true;
}

double UTraceWeaponComponent::GetRecoilPitchScale() const
{
	// SPEC v29 §2e. See the header for the whole argument. Two terms, added:
	//
	//   the GLOBAL switch   1 while bRecoilEnabled, which spec v25 §5 set to false and this pass
	//                       leaves false. Every pawn in the shipped build scores 0 here.
	//   ROXIE'S TRADE       RoxieModdedRecoilScale while MODDED is up, 0 otherwise, resolved through
	//                       TraceRoxie::GetAddedRecoilScaleFor() so this function does no casting.
	const double GlobalTerm = UTraceSettings::Get().bRecoilEnabled ? 1.0 : 0.0;
	const double AbilityTerm = static_cast<double>(TraceRoxie::GetAddedRecoilScaleFor(GetOwner()));
	return FMath::Max(0.0, GlobalTerm + AbilityTerm);
}

void UTraceWeaponComponent::ApplyRecoilKick()
{
	// =============================================================================================
	// SPEC v25 §5 — "Remove gun recoil, keep the firing rate."
	// =============================================================================================
	//
	// THIS EARLY RETURN IS THE REMOVAL, and bRecoilEnabled is now false in both the header default
	// and DefaultGame.ini. It is the only door into the aim punch: this function is called from
	// exactly one place (FireOnce, after the shot has been sampled and sent) and it is the only
	// caller of AddRecoilPitch that ever ADDS pitch. With the flag off nothing ever writes to the
	// control rotation from this component, RecoilAppliedPitch never leaves 0.0, and TickRecoil
	// returns on its first line. Measure it with Trace.TestRecoil: peak climb, residual and yaw
	// drift must all read 0.000.
	//
	// THE VIEWMODEL KICK IS A DIFFERENT MECHANISM AND IS DELIBERATELY STILL RUNNING. FireOnce calls
	// Character->NotifyWeaponFired() a few lines before it calls this; that jolts the first-person
	// gun MESH (ATraceCharacter::ViewModelKick) and settles it. It never touches the control
	// rotation, so it cannot move the crosshair or the round. §5 asks for the thing that moves your
	// AIM, which is this function, not that one. The muzzle flash and the tracer likewise stay.
	//
	// FireInterval (190 RPM) is not read anywhere in this file's recoil path and did not change.
	//
	// =============================================================================================
	// SPEC v29 §2e — "Roxie's modded should add recoil now"
	// =============================================================================================
	//
	// THE EARLY RETURN IS NOW A SCALE, AND bRecoilEnabled IS STILL FALSE. Demo 22's removal stands
	// for everybody: GetRecoilPitchScale() answers 0 for every pawn in the game, and this function
	// still returns on its first test for all of them, so nothing about §5 has been walked back.
	// What changed is that the scale is no longer forced to be a bool — a Roxie with MODDED up adds
	// RoxieModdedRecoilScale to it, so she and only she kicks, and only while it is up.
	//
	// EVERYTHING BELOW THIS POINT IS UNCHANGED AND SHARED. The growth per consecutive shot, the
	// climb ceiling, the burst reset, the recovery and the player-compensation rule are the same
	// eight knobs and the same code they always were; Roxie scales the KICK and does not get her own
	// recoil model. That is what makes her trade tunable from one place and what keeps the v5 gun
	// exactly recoverable by setting bRecoilEnabled back to true.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const double RecoilScale = GetRecoilPitchScale();
	if (RecoilScale <= 0.0)
	{
		return;
	}

	APlayerController* RecoilController = GetRecoilController();
	if (RecoilController == nullptr)
	{
		return;
	}

	const double Now = GetLocalTimeSeconds();
	if ((Now - LastRecoilShotTime) > FMath::Max(0.f, Settings.RecoilBurstResetSeconds))
	{
		// A gap long enough to count as a new burst resets the GROWTH only. Whatever climb is still
		// on the view keeps recovering on its own schedule; the two are independent by design.
		RecoilBurstShotIndex = 0;
	}
	LastRecoilShotTime = Now;

	// Settle up with the player's own movement before adding to the accumulator, or their pull-down
	// since the last tick would be counted against the kick we are about to apply.
	TraceRecoilMath::ConsumePlayerCompensation(
		FRotator::NormalizeAxis(RecoilController->GetControlRotation().Pitch),
		Settings.bRecoilPlayerCompensationCancels,
		RecoilAppliedPitch, RecoilTrackedPitch, bRecoilTrackingValid);

	const double Growth = 1.0 + FMath::Max(0.f, Settings.RecoilPitchGrowthPerShot) * static_cast<double>(RecoilBurstShotIndex);
	const double Headroom = FMath::Max(0.0, static_cast<double>(FMath::Max(0.f, Settings.RecoilMaxPitchDegrees)) - RecoilAppliedPitch);

	// Truncated at the ceiling rather than clamped after the fact, so the view never overshoots and
	// visibly snaps back down.
	//
	// SPEC v29 §2e: RecoilScale is Roxie's trade, and it multiplies the BASE PER-SHOT KICK — which is
	// what makes RoxieModdedRecoilScale a multiple of RecoilPitchPerShot rather than a number of
	// degrees, and what makes retuning the base move her with it (the standing rule). The ceiling is
	// deliberately NOT scaled: MaxPitchDegrees is how far the view is allowed to travel, which is a
	// statement about the screen and not about the gun, so a bigger kick reaches the same ceiling
	// sooner rather than climbing past it.
	const double Kick = FMath::Min(FMath::Max(0.f, Settings.RecoilPitchPerShot) * Growth * RecoilScale, Headroom);
	++RecoilBurstShotIndex;

	const double PitchBefore = RecoilTrackedPitch;
	if (Kick > 0.0)
	{
		AddRecoilPitch(RecoilController, Kick);
	}

	if (CVarTraceDebugRecoil.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Recoil] shot %d of burst: kick %+.3fdeg (base %.3f x growth %.2f x scale %.2f, headroom %.3f) | "
			     "pitch %+.3f -> %+.3f | climb %.3fdeg | yaw untouched"),
			RecoilBurstShotIndex, Kick, Settings.RecoilPitchPerShot, Growth, RecoilScale, Headroom,
			PitchBefore, RecoilTrackedPitch, RecoilAppliedPitch);
	}
}

void UTraceWeaponComponent::TickRecoil(float DeltaTime)
{
	if (RecoilAppliedPitch <= UE_KINDA_SMALL_NUMBER)
	{
		RecoilAppliedPitch = 0.0;
		return;
	}

	APlayerController* RecoilController = GetRecoilController();
	if (RecoilController == nullptr)
	{
		ResetRecoil();
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// NOTE the recovery deliberately runs even when bRecoilEnabled has just been switched OFF: the
	// climb already on the view has to be handed back, or turning the feature off mid-burst strands
	// the player looking at the sky.
	TraceRecoilMath::ConsumePlayerCompensation(
		FRotator::NormalizeAxis(RecoilController->GetControlRotation().Pitch),
		Settings.bRecoilPlayerCompensationCancels,
		RecoilAppliedPitch, RecoilTrackedPitch, bRecoilTrackingValid);

	if (RecoilAppliedPitch <= UE_KINDA_SMALL_NUMBER)
	{
		RecoilAppliedPitch = 0.0;
		return;
	}

	if ((GetLocalTimeSeconds() - LastRecoilShotTime) < FMath::Max(0.f, Settings.RecoilRecoveryDelaySeconds))
	{
		return;
	}

	// Exponential rather than FMath::FInterpTo's linearisation, so the curve is identical at 30 and
	// 240 fps — a recovery whose shape depends on frame rate is a feel bug nobody can reproduce.
	const double Proportional = RecoilAppliedPitch
		* (1.0 - FMath::Exp(-static_cast<double>(FMath::Max(0.f, Settings.RecoilRecoverySpeed)) * DeltaTime));

	// ...plus a linear floor, because a purely proportional return has an infinite tail and would
	// leave a fraction of a degree on the view for the whole of the next engagement.
	const double Floor = static_cast<double>(FMath::Max(0.f, Settings.RecoilRecoveryMinRateDegrees)) * DeltaTime;

	const double Step = FMath::Min(RecoilAppliedPitch, FMath::Max(Proportional, Floor));
	if (Step > 0.0)
	{
		AddRecoilPitch(RecoilController, -Step);
	}
}

void UTraceWeaponComponent::ResetRecoil()
{
	RecoilAppliedPitch = 0.0;
	RecoilBurstShotIndex = 0;
	bRecoilTrackingValid = false;
	RecoilTrackedPitch = 0.0;
}

void UTraceWeaponComponent::FireOnce()
{
	ATraceCharacter* Character = GetTraceCharacter();
	UWorld* World = GetWorld();
	if (Character == nullptr || World == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// Timestamp first: this is the instant the player believes they fired, and it is what the server
	// rewinds to. Taking it before any of the work below keeps it honest.
	const double FireServerTime = GetServerTimeSeconds();
	AdvanceFireClock();

	// SPEC v29 §2b. One round per press until the trigger is released; the tick's repeat reads this.
	bTriggerConsumedThisPress = true;

	// SPEC v29 §2f. The measurement surface, off unless a harness turned it on. Recorded HERE rather
	// than in the harness's own ticker because a ticker samples frames, and frames are precisely the
	// thing under suspicion — a measurement that can only see frame boundaries cannot prove a fix
	// whose entire content is not landing on them.
	if (CVarTraceRecordShots.GetValueOnGameThread() != 0 && RecordedShotTimes.Num() < 4096)
	{
		RecordedShotTimes.Add(LastLocalFireTime);
	}

	if (CVarTraceShotStats.GetValueOnGameThread() != 0)
	{
		++TraceShotStats::GStats.LocalShotsFired;
	}

	// --- AMMO: the CLIENT-PREDICTED half (spec v16 §1) -------------------------------------------
	//
	// *** ONLY OFF THE AUTHORITY, AND THAT CONDITION IS NOT AN OPTIMISATION. *** On a listen host's
	// own pawn and on every bot, this function and ServerFire_Implementation both run in this same
	// process — ServerFire is a local call there, not an RPC — so consuming in both would spend two
	// rounds per trigger pull and halve the clip. Each machine consumes on exactly one side of that
	// pair: the authority in ServerFire, a remote client here.
	//
	// The count therefore drops on the shooter's HUD in the same frame as the muzzle flash, which is
	// the "client-predicted for feel" half of §1, and the server's copy arrives ~RTT/2 later and
	// reconciles through OnRep_Ammo rather than overwriting.
	const AActor* FireOwner = GetOwner();
	if ((FireOwner == nullptr || !FireOwner->HasAuthority()) && TraceAmmo::IsPredictionEnabled())
	{
		ConsumeRound();

		// The automatic reload, anchored at THIS SHOT rather than at "now". The server anchors its own
		// at the same (clamped) stamp when it processes the same round, so both machines compute one
		// deadline — without that, a 40 ms client would come off a 0.5 s reload and find the server
		// still refusing to fire for another 40 ms, every single clip.
		if (TraceAmmo::IsEnabled() && GetClipAmmo() <= 0 && !IsReloading())
		{
			BeginReload(FireServerTime);
		}
	}

	const FVector Origin = Character->GetMuzzleLocation();
	FVector Dir = Character->GetAimDirection().GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = Character->GetActorForwardVector();
	}

	// NO SPREAD. Spec section 6: "There is no movement inaccuracy. Set spread to 0." The shot is
	// exactly the aim ray, moving or still, and UTraceSettings::SpreadDegrees is deliberately not
	// read - leaving the roll in place but configured to zero would mean a stale .ini quietly
	// reintroduced inaccuracy the design has removed. This also retires the "a modified client
	// could roll zero spread" cheat, since zero is now the rule for everyone.
	//
	// The gun is precise; the DAMAGE ZONES are what make aim matter now (head 100 / body 40 /
	// legs 25), which is a skill test the player can see and learn rather than a hidden dice roll.

	const float Range = FMath::Max(1.f, Settings.HitscanRange);

	// Cosmetic-only local resolve: where should *our* tracer stop? No damage is applied on the
	// client under any circumstances - the server owns that entirely.
	//
	// This runs the SAME resolver the server will run, rather than a plain ECC_Visibility line
	// trace. A line trace on that channel never stops on a player: the character capsule uses the
	// stock "Pawn" profile, whose one custom response is Visibility = Ignore. The shooter would
	// therefore watch their own tracer punch through an enemy and terminate on the wall behind, on
	// the same frame the server sent back a hit marker - the exact contradiction that makes a
	// hitscan prototype feel broken. ResolveHitscan already traces the world for static geometry
	// internally and always writes OutImpactPoint, so this also removes a duplicate trace.
	//
	// On a client GetAuthGameMode() is null and the lag-comp histories are empty, so it resolves
	// against the live (interpolated) poses the player can actually see - which is the right answer
	// for a tracer. Nothing here is authoritative; the server re-resolves from scratch.
	FVector TracerEnd = Origin + Dir * Range;
	{
		FVector PredictedImpact = TracerEnd;
		ETraceHitZone PredictedZone = ETraceHitZone::None;
		ATraceCharacter* PredictedVictim = UTraceLagCompensationComponent::ResolveHitscan(
			World, Character, Origin, Dir, Range,
			static_cast<float>(FireServerTime), PredictedImpact, PredictedZone);
		TracerEnd = PredictedImpact;

		LastPredictedZone = PredictedZone;
		LastPredictedVictim = PredictedVictim;
		LastPredictedFireServerTime = FireServerTime;
	}

	// "Did the beam stop on something" - anything short of full range means a surface (a body or
	// world geometry) terminated it, and that is where the impact flash belongs.
	const bool bImpacted = FVector::DistSquared(Origin, TracerEnd) < static_cast<double>(Range) * Range * 0.998;

	// GEOMETRY, NOT A BODY. The local resolve above already answered "who did this beam stop on", so
	// a shot that stopped short of full range with nobody in the way stopped on the world — which is
	// the exact test FX_AUDIO_PLAN §3 names for the impact plane ("LastPredictedVictim == nullptr on
	// the shooter"). Never on a victim: spec v4 §4 deleted the on-body sphere because it covered up
	// the point the shooter was reading, and a 26 uu plane there would be the same mistake, flatter.
	const bool bWorldHit = bImpacted && (LastPredictedVictim.Get() == nullptr);

	PlayLocalTracer(Origin, TracerEnd, bImpacted, bWorldHit);

	// --- FX_AUDIO_PLAN §6.1 — THE SHOOTER'S OWN REPORT, WITH NO ROUND TRIP IN IT -----------------
	//
	// Audio/TraceAudioWatch.h stated the cost of the observer-only design plainly and named the fix:
	// "Closing that gap needs the one line in ServerFire's caller." This is that line. Until now a
	// remote client heard its own gunshot half a round trip after pulling the trigger, because the
	// sound was decided on the server by watching the authoritative clip; the observer still owns
	// what everybody ELSE hears, and its multicast now excludes this pawn's machine so the shot is
	// still exactly one sound per listener.
	//
	// *** BOTH GUARDS ARE LOAD-BEARING AND NEITHER IS REDUNDANT. *** IsLocallyControlled() alone is
	// true for every bot on the server, and IsPlayerControlled() alone is true for a remote player's
	// pawn as the server sees it. Together they mean "a human is playing this pawn ON THIS MACHINE",
	// which is the only case that has a speaker to play into and the only case the exclusion is going
	// to skip. Without the second one a listen host would hear every bot's shot at point-blank range
	// — the documented bot trap in TraceAudio.cpp's client-side gate, and the reason
	// PlayPredictedLocal re-tests it internally as well.
	if (TraceAudioWatch::IsPredictedShotEnabled()
		&& Character->IsLocallyControlled() && Character->IsPlayerControlled())
	{
		// §1d: the SMG has no ladder — "smg shoot 1 should play on every bullet" — so the ladder is
		// asked only for the pistol, exactly as the observer does it, and for the same reason it does
		// not reset the ladder on an SMG round.
		const FName Clip = (EquippedWeapon == ETraceEquippedWeapon::Smg)
			? TraceSoundEvents::SmgShoot1
			: LocalShotLadder.NextShot(FireServerTime, UTraceAudioSettings::Get().GetPistolLadderResetSeconds());

		// At the muzzle, spatialised, with the same gain and attenuation curve the multicast would
		// have carried — PlayPredictedLocal goes through the same PlayWorldNow. So this is not a
		// second, differently-mixed gunshot; it is the same one, on time.
		TraceAudio::PlayPredictedLocal(Character, Clip, Origin);

		// THE OTHER HALF OF THE TWO-PROCESS AUDIT'S LEDGER, off the same switch the observer's line
		// uses (Trace.Audio.ShotLog, default 0). A remote shooter's log must show this line and NO
		// observer line for its own pawn; the server's log must show the observer line for that pawn
		// and none of these. Both name the shooter, so a match full of bots cannot blur the count.
		if (TraceAudioWatch::IsShotLogEnabled())
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[ShotAudio] PREDICTED %-18s %-6s -> %-12s   (local, no round trip)"),
				*GetNameSafe(Character), LexToString(EquippedWeapon), *Clip.ToString());
		}
	}

	// Viewmodel recoil, once per ROUND. This is the only place that knows a round actually left the
	// gun, so a held burst kicks per shot instead of once on the trigger press. Cosmetic only,
	// rate-limited inside, and a no-op on any machine that is not looking out of this pawn.
	//
	// SPEC v25 §5 KEEPS THIS. It moves the first-person gun MESH, not the control rotation — the
	// crosshair does not move, the camera does not move, and the round has already been sampled and
	// sent above regardless. "Remove recoil" means the aim punch (ApplyRecoilKick, at the bottom of
	// this function, now disabled); removing the mesh animation as well would delete the gun's only
	// feedback that it fired while removing no aim penalty at all. Same reasoning keeps the muzzle
	// flash and the tracer (PlayLocalTracer, just above), which the spec names as out of scope.
	Character->NotifyWeaponFired();

	ServerFire(FVector_NetQuantize(Origin), FVector_NetQuantizeNormal(Dir), static_cast<float>(FireServerTime));

	// UPWARDS RECOIL, LAST — AND THE ORDER IS THE WHOLE DESIGN (spec v5 section 6).
	//
	// *** SPEC v25 SECTION 5 DISABLES THIS. *** ApplyRecoilKick() returns on its first line while
	// bRecoilEnabled is false (header default AND DefaultGame.ini, both now false), so the call
	// below is inert and no pitch is ever written to the control rotation. The call is left in
	// place, not deleted, because the flag is the A/B arm the recoil was built with and the ordering
	// note below is what has to stay true if it is ever switched back on. THE FIRE RATE ABOVE IS
	// UNCHANGED: FireInterval is still 0.315789 s = 190 RPM.
	//
	// Origin and Dir were sampled at the top of this function and have already gone to the server,
	// so the round that causes this kick flies exactly where the crosshair was when the trigger
	// broke. The kick moves the control rotation, which is what the NEXT shot will be built from —
	// that is recoil — and it moves the camera with it, because ATraceCharacter::ResolveAimRotation
	// and the camera are both pure functions of that same rotation. Hence aimErr stays 0.0000 deg.
	//
	// Nothing about this replicates. The server is told a DIRECTION, not a rotation, so there is no
	// recoil state for the two machines to disagree about and the authoritative trace resolves the
	// same ray the shooter saw. Client-predicted for feel, invisible to the hit resolution.
	ApplyRecoilKick();
}

void UTraceWeaponComponent::PlayLocalTracer(const FVector& From, const FVector& To, bool bImpacted, bool bWorldHit) const
{
	UWorld* World = GetWorld();
	ATraceCharacter* Character = GetTraceCharacter();
	if (World == nullptr || Character == nullptr)
	{
		return;
	}

	// FX_AUDIO_PLAN §2.7's seam, consumed at the ONE place a beam is asked for. The ordinary answer
	// is the shot's team hue, exactly as it has always been (the halo is the piece that carries it —
	// see ATraceTracer's HotColorWhiteMix note on why a beam that went white stopped being able to
	// say whose shot it was). Bee rounds are the single canonical override.
	FLinearColor BeamColor = TraceTeamColor(Character->GetTeam());
	FLinearColor Tint = BeamColor;
	const bool bTinted = GetTracerTintOverride(Tint);
	if (bTinted)
	{
		BeamColor = Tint;
	}

	// The impact plane's hue is the TRACER FAMILY's pale cyan and not the beam's team colour (§3):
	// a mark left on a wall says "a shot landed here", which is a fact about the weapon rather than
	// about the shooter, and the arena is already saturated with team colour. The override moves it
	// with the beam, so a bee round leaves an amber scuff.
	FTraceTracerImpact Impact;
	Impact.bDraw = bWorldHit;
	Impact.Hue = bTinted ? Tint : ATraceTracer::TracerFamilyHue();

	ATraceTracer::Spawn(World, From, To, BeamColor, bImpacted, Impact);
}

bool UTraceWeaponComponent::GetTracerTintOverride(FLinearColor& OutTint) const
{
	// One state, asked once: the clip holds rounds an ability put there. GetAbilityRoundsInClip()
	// reads the replicated counter on a client and the authoritative one on the server, so the
	// shooter's own machine and the server agree without this function knowing which it is on.
	if (GetAbilityRoundsInClip() <= 0)
	{
		return false;
	}

	// FX_AUDIO_PLAN's BeeRounds amber, linear. The one colour in this file, because the one exception
	// the bible grants is X's; if a second ability ever loads a clip, this is where its hue joins and
	// the seam's shape does not change.
	OutTint = FLinearColor(1.00f, 0.78f, 0.10f, 1.f);
	return true;
}

void UTraceWeaponComponent::AccumulateShotStats(ETraceHitZone ServerZone, const ATraceCharacter* Victim,
	const FTraceHitscanDiagnostics& Diagnostics)
{
	TraceShotStats::FStats& S = TraceShotStats::GStats;

	const int32 ZoneIndex = FMath::Clamp(static_cast<int32>(ServerZone), 0, 3);
	++S.ZoneCount[ZoneIndex];

	if (Diagnostics.bWorldTraceHit)
	{
		++S.WorldTruncated;
		if (Diagnostics.bWorldStartPenetrating)
		{
			++S.WorldStartPenetrating;
		}
		if (Diagnostics.WorldHitDistance >= 0.0 && Diagnostics.WorldHitDistance < 200.0)
		{
			++S.WorldTruncatedUnder200;
		}
	}

	if (Diagnostics.bHaveVictim)
	{
		const int32 AltIndex = FMath::Clamp(static_cast<int32>(Diagnostics.ZoneAtClosestApproach), 0, 3);
		++S.AltZoneCount[AltIndex];

		if (Diagnostics.ZoneAtClosestApproach != ServerZone)
		{
			++S.ZoneReclassified;
			if (ServerZone == ETraceHitZone::Legs && Diagnostics.ZoneAtClosestApproach == ETraceHitZone::Body)
			{
				++S.ReclassLegsToBody;
			}
			else if (ServerZone == ETraceHitZone::Body && Diagnostics.ZoneAtClosestApproach == ETraceHitZone::Legs)
			{
				++S.ReclassBodyToLegs;
			}
		}

		if (Diagnostics.ClosestHeightFraction >= 0.0)
		{
			const int32 Bucket = FMath::Clamp(
				static_cast<int32>(Diagnostics.ClosestHeightFraction * TraceShotStats::NumHeightBuckets),
				0, TraceShotStats::NumHeightBuckets - 1);
			++S.HeightHistogram[Bucket];
			S.HeightFractionSum += Diagnostics.ClosestHeightFraction;
			++S.HeightSamples;
		}

		if (Diagnostics.bVictimPoseRewound)
		{
			++S.VictimPoseRewound;
		}
		else
		{
			++S.VictimPoseLive;
		}

		S.PostureSum += Diagnostics.VictimFrame.PostureScale;
		if (Diagnostics.VictimFrame.PostureScale < 0.99)
		{
			++S.VictimNonStandingPosture;
		}
	}

	// Predicted vs authoritative. Only meaningful when both halves ran here; a remote client's
	// prediction lives in another process and cannot be compared from inside this one.
	const ATraceCharacter* Shooter = GetTraceCharacter();
	if (Shooter != nullptr && Shooter->IsLocallyControlled() && LastPredictedFireServerTime > 0.0)
	{
		++S.PredictionComparisons;
		const bool bSameVictim = (LastPredictedVictim.Get() == Victim);
		const bool bSameZone = (LastPredictedZone == ServerZone);
		if (bSameVictim && bSameZone)
		{
			++S.PredictionAgree;
		}
		else
		{
			if (!bSameZone) { ++S.PredictionZoneMismatch; }
			if (!bSameVictim) { ++S.PredictionVictimMismatch; }
		}
	}

	// Automatic dump so a headless run needs no console at the end of it.
	const float Interval = CVarTraceShotStatsInterval.GetValueOnGameThread();
	if (Interval > 0.f)
	{
		const double Now = GetLocalTimeSeconds();
		if (S.LastDumpTime <= 0.0)
		{
			S.LastDumpTime = Now;
		}
		else if ((Now - S.LastDumpTime) >= Interval)
		{
			S.LastDumpTime = Now;
			TraceShotStats::Dump();
		}
	}
}

bool UTraceWeaponComponent::ServerFire_Validate(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientFireServerTime)
{
	// Validation failure disconnects the client, so only reject payloads that are outright
	// impossible to reason about. Everything else is handled with an early return below.
	if (Origin.ContainsNaN() || Direction.ContainsNaN())
	{
		return false;
	}
	if (!FMath::IsFinite(ClientFireServerTime))
	{
		return false;
	}
	return true;
}

void UTraceWeaponComponent::ServerFire_Implementation(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientFireServerTime)
{
	AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || World == nullptr || Character == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	// ---- payload sanity (never check() on network input) ---------------------------------
	const bool bCollectStats = (CVarTraceShotStats.GetValueOnGameThread() != 0);

	FVector Dir(Direction);
	const double DirLengthSq = Dir.SizeSquared();
	if (DirLengthSq < 0.25 || DirLengthSq > 4.0)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: rejecting non-unit direction from %s"), *GetNameSafe(OwnerActor));
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedPayload; }
		return;
	}
	Dir = Dir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedPayload; }
		return;
	}
	if (FMath::Abs(Origin.X) > MaxReasonableCoordinateUU || FMath::Abs(Origin.Y) > MaxReasonableCoordinateUU || FMath::Abs(Origin.Z) > MaxReasonableCoordinateUU)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: rejecting out-of-world origin from %s"), *GetNameSafe(OwnerActor));
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedPayload; }
		return;
	}

	// ---- state gate ----------------------------------------------------------------------
	if (!Character->IsAlive() || Character->IsCarrier())
	{
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedState; }
		return;
	}

	// ---- SPEC v28 §10: the melee lockout, RE-ASKED HERE ------------------------------------
	//
	// [DUALWIELD] The client refused this shot for the length of its swing animation; that gate is
	// for feel, and a modified client simply would not run it. This is the copy that makes
	// "meleeing locks the player out of shooting" a rule rather than a client-side suggestion — the
	// same argument the ammo, rate and dash gates below and above already make for themselves.
	//
	// *** THE GRACE IS FOR HONEST LATENCY, NOT FOR CHEATS, and it is the reload gate's grace
	// unchanged. *** The two ends measure the window from different instants: the client from its
	// own PRESS, the server from the arrival of ServerSwing, which is one upstream lag plus
	// SwingWindupSeconds later. The server's window therefore CLOSES later than the client's by
	// roughly that much, so a player who legitimately fires the frame their animation ends would
	// have the first round of every post-melee burst eaten. 50 ms covers a normal connection's
	// share of that; what it buys a liar is 50 ms of a 320 ms lockout, which is a sixth of one
	// melee's worth of downtime and nothing like an exploit.
	//
	// GetShootLockoutRemaining() returns 0 outright when the v28 §10 switch is off, so this is inert
	// in the reverted build rather than being a second thing a revert has to remember.
	{
		constexpr double MeleeLockoutGraceSeconds = 0.05;
		if (GetShootLockoutRemaining() > MeleeLockoutGraceSeconds)
		{
			UE_LOG(LogTraceGame, Verbose,
				TEXT("ServerFire: %s fired %.3fs into a melee animation (spec v28 s10 locks the trigger for %.3fs)"),
				*GetNameSafe(OwnerActor),
				TraceMelee::GetSwingAnimSeconds() - GetShootLockoutRemaining(),
				TraceMelee::GetSwingAnimSeconds());
			if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedState; }
			return;
		}
	}

	// ---- AMMO (spec v16 §1): the SERVER's copy of the gate the client fired through ---------
	//
	// The client gated on its own predicted clip before it sent this; that gate is for feel, and a
	// modified client simply would not run it. Re-asked here, this is the rule — without it "30
	// bullets per clip" would be a client-side suggestion.
	if (TraceAmmo::IsEnabled())
	{
		if (ClipAmmo == 0)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: %s fired on an empty clip"), *GetNameSafe(OwnerActor));
			if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedState; }
			return;
		}

		// THE GRACE EXISTS FOR CLOCK ESTIMATION, NOT FOR CHEATS, and it is the same argument
		// FireRateTolerance makes. Both ends anchor the reload at one stamped instant, so they agree
		// to within each machine's error in its estimate of the shared clock — tens of milliseconds.
		// Refusing a shot inside that error would drop the first round of a burst after every single
		// reload, for honest players only. What 50 ms of forgiveness buys a liar is one extra round
		// per reload at most, because the fire-rate gate is 0.32 s wide.
		constexpr double ReloadGateGraceSeconds = 0.05;
		if (TraceAmmo::DoesReloadBlockFire()
			&& ReloadEndServerTime > 0.f
			&& GetServerTimeSeconds() < static_cast<double>(ReloadEndServerTime) - ReloadGateGraceSeconds)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: %s fired %.3fs into a reload"),
				*GetNameSafe(OwnerActor), GetReloadSeconds() - GetReloadRemaining());
			if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedState; }
			return;
		}
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// ---- fire rate, with slack for honest jitter -----------------------------------------
	//
	// SPEC v18 §2: scaled by the SAME per-character seam CanFire() uses, and it has to be. If only the
	// client's gate scaled, a Roxie with Modded up would fire every 0.242 s and this validation would
	// reject every second shot as rate-limited — which reads as the gun eating bullets, i.e. strictly
	// worse than the ability not working at all. The server re-derives the scale from its own
	// authoritative ability state rather than trusting anything in the RPC payload, so a client that
	// lied about being Roxie gains nothing.
	// SPEC v28 §9: GetFireInterval() is the base of the weapon IN HAND times the same scale, so the
	// server's idea of a legal cadence follows the client onto the SMG's 0.1 s automatically. Both
	// gates call one function; see its comment for why two copies of this arithmetic was a shipped
	// bug once already.
	const double FireInterval = GetFireInterval();
	const double LocalNow = GetLocalTimeSeconds();
	if ((LocalNow - LastAcceptedFireTime) < FireInterval * (1.0 - FireRateTolerance))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: rate-limited %s (%.3fs since last accepted)"),
			*GetNameSafe(OwnerActor), LocalNow - LastAcceptedFireTime);
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedRate; }
		return;
	}
	LastAcceptedFireTime = LocalNow;
	if (bCollectStats) { ++TraceShotStats::GStats.ServerShotsAccepted; }

	// ---- rewind window -------------------------------------------------------------------
	const double ServerNow = GetServerTimeSeconds();
	double RewindTime = ServerNow;
	if (Settings.bEnableLagCompensation && ClientFireServerTime > 0.f)
	{
		// Clamping is what bounds the exploit: however stale or futuristic the client's stamp is,
		// we only ever look back at most MaxRewindTime and never forward at all.
		const double MaxRewind = FMath::Max(0.f, Settings.MaxRewindTime);
		RewindTime = FMath::Clamp(static_cast<double>(ClientFireServerTime), ServerNow - MaxRewind, ServerNow);
	}

	// ---- AMMO: the AUTHORITATIVE round, spent (spec v16 §1) --------------------------------
	//
	// After the rate gate (so a rejected shot costs nothing) and after RewindTime is resolved (so the
	// automatic reload can be anchored at the same clamped instant the bullet is resolved at, which is
	// the same instant the client anchored ITS predicted reload at in FireOnce). Before the trace,
	// because a round leaves the gun whether or not it finds anybody.
	ConsumeRound();
	if (TraceAmmo::IsEnabled() && ClipAmmo == 0 && !IsReloading())
	{
		BeginReload(RewindTime);
	}

	// ---- muzzle sanity, measured against where the shooter *was* --------------------------
	FVector ShotOrigin(Origin);
	FVector ReferencePoint = Character->GetMuzzleLocation();
	if (const UTraceLagCompensationComponent* ShooterLagComp = Character->FindComponentByClass<UTraceLagCompensationComponent>())
	{
		FTraceLagCompFrame ShooterFrame;
		if (ShooterLagComp->GetPoseAtTime(static_cast<float>(RewindTime), ShooterFrame))
		{
			// Comparing against the shooter's live position would punish anyone with latency, since
			// they legitimately fired from where they used to be.
			ReferencePoint = ShooterFrame.CapsuleCenter;
		}
	}

	if (FVector::DistSquared(ShotOrigin, ReferencePoint) > MaxOriginErrorUU * MaxOriginErrorUU)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: implausible muzzle from %s, snapping to server pose"), *GetNameSafe(OwnerActor));
		ShotOrigin = ReferencePoint;
	}

	// ---- resolve -------------------------------------------------------------------------
	const float ShotRange = FMath::Max(1.f, Settings.HitscanRange);
	FVector ImpactPoint = ShotOrigin + Dir * ShotRange;
	ETraceHitZone Zone = ETraceHitZone::None;
	FTraceHitscanDiagnostics Diagnostics;
	ATraceCharacter* Victim = UTraceLagCompensationComponent::ResolveHitscan(
		World,
		Character,
		ShotOrigin,
		Dir,
		ShotRange,
		static_cast<float>(RewindTime),
		ImpactPoint,
		Zone,
		bCollectStats ? &Diagnostics : nullptr);

	if (bCollectStats)
	{
		AccumulateShotStats(Zone, Victim, Diagnostics);
	}

	bool bKilled = false;
	// C5. Set from the victim's OWN IsInvulnerable() below — the same derived fact ApplyDamage
	// consults, never a second opinion about who is carrying the Core (guardrail 1).
	bool bShieldBlocked = false;
	if (Victim != nullptr)
	{
		if (UTraceHealthComponent* VictimHealth = Victim->FindComponentByClass<UTraceHealthComponent>())
		{
			// Spec section 6: head 100 / body 40 / legs 25. No multiplier, no base damage - the zone
			// IS the damage. UTraceSettings::HitscanDamage and HeadshotMultiplier are no longer read
			// by the weapon (see the report); the numbers live in UTraceDamageSettings.
			// NOT const: the ability passives below may modify it (spec v14 §6). The ZONE is still
			// the only thing that decides the base number — see spec section 6, head 100 / body 40 /
			// legs 25 — and a character with no passives leaves it exactly as it was.
			// SPEC v28 §9 — THE ZONE STILL IS THE DAMAGE; THE WEAPON NOW CHOOSES WHICH TABLE. The
			// pistol's 100/40/25 still come from FTraceHitZoneModel and UTraceDamageSettings,
			// untouched; the SMG's 33/18/12 come from the three knobs beside SmgFireInterval. One
			// call, resolved on the replicated selector, so the shooter's predicted zone and the
			// server's authoritative one are still being priced by the same rule.
			//
			// SPEC v29 §2d — AND NOW THE RANGE, FOR THE SMG ONLY. The distance is measured from the
			// muzzle the server accepted (ShotOrigin, already snapped to the shooter's rewound pose if
			// the client's was implausible) to the impact the server resolved — so the range that is
			// priced is the range the SERVER agrees the bullet flew, never a client-supplied number.
			// Beyond 800 uu the SMG pays 24/15/10; the pistol reaches none of this.
			const double ShotDistanceUU = FVector::Dist(ShotOrigin, ImpactPoint);
			float Damage = TraceAmmo::GetZoneDamage(EquippedWeapon, Zone, ShotDistanceUU);

			// SPEC v8 §6, the kill feed's headshot icon. The zone is known EXACTLY here and nowhere
			// after: ApplyDamage takes a cause and the health component clamps at zero, so a head
			// shot and a shin shot both arrived at the death handler as plain "Bullet". The feed was
			// reduced to inferring it from the victim's previous health (PreviousHealth > BodyDamage
			// ⇒ provably a head shot), which under-reports — a head shot on a victim already down to
			// 20 was indistinguishable from a body shot and drew the plain round.
			//
			// Naming the zone at the one site that still knows it costs nothing and makes the icon
			// exact in both directions. The feed already accepts this name; every other cause is
			// unchanged, so nothing else in the taxonomy moves.
			const FName DamageCause = (Zone == ETraceHitZone::Head) ? FName(TEXT("Headshot")) : FName(TEXT("Bullet"));

			// SPEC v14 §6 — THE ABILITY PASSIVE PIPELINE, FOLDED IN AT THE ONE PLACE A BULLET'S
			// NUMBER IS DECIDED.
			//
			// This runs the INSTIGATOR's outgoing passives and the TARGET's incoming ones — Chut's
			// Chud is "30% less damage from body shots", and a body shot is this line. It is a no-op
			// for a Mannequin and for every player who has not picked a character, so mode A and bots
			// are unaffected.
			//
			// *** X's VULNERABLE IS NOT APPLIED HERE, AND MUST NOT BE. *** "+25% damage from all
			// sources" is one multiplication inside UTraceHealthComponent::ApplyDamage, which this
			// call is about to reach; UTraceAbilitySetX deliberately leaves
			// GetIncomingDamageMultiplier() at 1 so that the amplification cannot happen twice on one
			// bullet. See the block comment at the top of TraceHealthComponent.h.
			FTraceAbilityDamageContext AbilityContext;
			AbilityContext.Instigator   = Character;
			AbilityContext.Target       = Victim;
			AbilityContext.Cause        = DamageCause;
			AbilityContext.bHeadshot    = (Zone == ETraceHitZone::Head);
			AbilityContext.bMelee       = false;
			AbilityContext.bFromAbility = false;
			Damage = UTraceAbilityComponent::ModifyDamageThroughPassives(Damage, AbilityContext);

			// C5 — ASKED OF THE VICTIM, WITH THE EXACT PREDICATE ApplyDamage IS ABOUT TO NO-OP ON, so
			// "the shot was blocked" and "the damage did nothing" cannot disagree. No second carrier
			// test is invented here (guardrail 1/2): this is IsInvulnerable() and nothing else.
			//
			// *** MEASURED 2026-08-24 — HOW OFTEN THIS IS TRUE TODAY, AND WHY THAT IS NOT A BUG HERE.
			// *** UTraceLagCompensationComponent::ResolveHitscan (:323) SKIPS a shielded carrier as a
			// candidate outright — "do not even resolve them" — using the SAME expression
			// IsInvulnerable() evaluates. So a shielded carrier does not normally reach this line at
			// all: the shooter currently gets SILENCE on the gun path, not the lying white marker the
			// code-gameplay F2 audit described (that reading predates the resolver's carrier skip).
			// What is left is the race — the victim taking the Core, or a pass window closing, between
			// the resolve and the damage — and in that race this flag is the difference between a
			// marker that lies and one that tells the truth. Trace.Weapon.ShieldBlockTest measures all
			// of it: the live shielded shot (no notification), an ordinary hit (normal marker), and the
			// blocked flag end to end.
			//
			// IF THE BLOCKED MARKER IS EVER TO DRAW FOR ORDINARY FIRE (ART_BIBLE §2.4 / FX plan §7.4),
			// THE FACT HAS TO BE PRODUCED WHERE THE SHOT IS REFUSED, i.e. at that skip in the lag
			// compensation component — which is a file this tranche does not own and a rule
			// (carrier-immune-to-bullets) nothing in this plan may change. Named in the C report.
			bShieldBlocked = VictimHealth->IsInvulnerable();

			VictimHealth->ApplyDamage(Damage, Character->GetController(), DamageCause);

			// ApplyDamage no-ops against an invulnerable target, so read the result rather than
			// assuming the hit landed.
			bKilled = !VictimHealth->IsAlive();

			// SPEC v14 §6 — X's STING: "His NEXT FIVE BULLETS apply vulnerable on hit, at NORMAL
			// damage." AFTER the damage, deliberately: the delivering bullet must not be amplified by
			// the mark it delivers, and being on this side of ApplyDamage is what guarantees it
			// rather than a comment asking the next reader to be careful.
			//
			// One character-agnostic call. See Abilities/Characters/TraceAbilityWeaponHooks.h for why
			// the gun does not know X's class name.
			TraceAbilityWeaponHooks::OnBulletHit(Character, Victim, Zone == ETraceHitZone::Head);
		}

		if (ATracePlayerController* ShooterController = Cast<ATracePlayerController>(Character->GetController()))
		{
			// The zone rides along so the shooter's hitmarker can say WHICH zone paid out. Positional
			// damage is only learnable if the feedback is positional too.
			//
			// C5: and so does whether it was stopped by the carrier's shield, so the marker can stop
			// claiming damage that never happened (ART_BIBLE §2.4; the HUD draw is FX plan §7.4).
			ShooterController->ClientNotifyHit(bKilled, Zone, bShieldBlocked);
		}
	}

	// ---- predicted-vs-authoritative agreement check (dev instrumentation) -------------------
	// Only meaningful when this same process also ran the predicted trace, i.e. the shooter is
	// locally controlled here (single player, or the listen host's own pawn). A remote client's
	// prediction lives in another process and cannot be compared from inside this one.
	if (CVarTraceDebugHitZones.GetValueOnGameThread() != 0 && Character->IsLocallyControlled())
	{
		const bool bSameVictim = (LastPredictedVictim.Get() == Victim);
		const bool bSameZone = (LastPredictedZone == Zone);
		// SPEC v29 §2d: the RANGE and the falloff alpha ride along, because "the damage was wrong" and
		// "the damage was right for a range I did not expect" are the two readings of the same line.
		const double LoggedDistance = FVector::Dist(ShotOrigin, ImpactPoint);
		UE_LOG(LogTraceGame, Display,
			TEXT("HITZONE %s  %s: predicted %s on %s | server %s on %s (damage %.0f at %.0fuu, falloff %.2f, rewind %.3fs)"),
			(bSameVictim && bSameZone) ? TEXT("AGREE   ") : TEXT("DISAGREE"),
			*GetNameSafe(Character),
			TraceHitZoneToString(LastPredictedZone), *GetNameSafe(LastPredictedVictim.Get()),
			TraceHitZoneToString(Zone), *GetNameSafe(Victim),
			TraceAmmo::GetZoneDamage(EquippedWeapon, Zone, LoggedDistance),
			LoggedDistance, TraceAmmo::GetFalloffAlpha(EquippedWeapon, LoggedDistance),
			ServerNow - RewindTime);
	}

	// Unreliable and cosmetic: everyone but the shooter draws the railgun beam.
	//
	// bWorldHit is the SERVER'S resolve of the same question the shooter answered locally (FireOnce):
	// did this beam stop on geometry rather than on a player? It rides along because the machines
	// that draw this beam did not run the trace and have no other way to know — and FX_AUDIO_PLAN §3
	// draws its impact plane on geometry ONLY, honouring spec v4 §4's deletion of the on-victim pop.
	const bool bImpacted = FVector::DistSquared(ShotOrigin, ImpactPoint) < static_cast<double>(ShotRange) * ShotRange * 0.998;
	MulticastFireEffects(FVector_NetQuantize(ShotOrigin), FVector_NetQuantize(ImpactPoint), bImpacted,
		/*bWorldHit=*/bImpacted && (Victim == nullptr));
}

void UTraceWeaponComponent::MulticastFireEffects_Implementation(FVector_NetQuantize Origin, FVector_NetQuantize Impact, bool bImpacted, bool bWorldHit)
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	// The shooter drew this tracer the instant it pulled the trigger. Drawing it again here would
	// double up the effect and, worse, draw the server's slightly different ray over the top of the
	// one the player already saw. This is the owner-skipping multicast the design calls for; on a
	// listen server the host's own pawn is locally controlled and is skipped for the same reason.
	if (Character->IsLocallyControlled())
	{
		return;
	}

	PlayLocalTracer(Origin, Impact, bImpacted, bWorldHit);
}

// =================================================================================================
// THE KNIFE  (spec v10 §1)
//
// Gameplay/TraceMelee.h is the design document — the carrier immunity, the back/front angle model,
// why the swing is a swept arc and why the yaw ring exists. This section is the STATE MACHINE:
//
//     press  -> StartSwing        gates, cooldown stamp, animation starts
//     +0.10s -> TickSwing         the blade resolves; the client predicts and sends ServerSwing
//     server -> ServerSwing       re-validates everything, rewinds, applies the damage
//     +0.50s -> the next swing is legal, measured from the PRESS
//
// and the swap, which is one replicated selector plus one replicated deadline.
// =================================================================================================

bool UTraceWeaponComponent::IsDeploying() const
{
	return GetServerTimeSeconds() < static_cast<double>(DeployEndServerTime);
}

float UTraceWeaponComponent::GetDeployRemaining() const
{
	return static_cast<float>(FMath::Max(0.0, static_cast<double>(DeployEndServerTime) - GetServerTimeSeconds()));
}

float UTraceWeaponComponent::GetSwingCooldownRemaining() const
{
	// The LOCAL clock, exactly as the fire-rate gate uses it, and for the same reason: a resync of
	// the shared clock must never be able to stall a weapon. The shared clock is for rewinding, not
	// for gating.
	const double Elapsed = GetLocalTimeSeconds() - LastLocalSwingTime;
	return static_cast<float>(FMath::Max(0.0, static_cast<double>(TraceMelee::GetSwingCooldownSeconds()) - Elapsed));
}

bool UTraceWeaponComponent::CanSwing(ETraceMeleeRefusal* OutRefusal) const
{
	if (OutRefusal != nullptr)
	{
		*OutRefusal = ETraceMeleeRefusal::None;
	}

	// One lambda so every refusal reports itself. A melee that silently does nothing is
	// indistinguishable from broken input — the exact failure the gun's carrier gate cost real
	// debugging time over.
	auto Refuse = [OutRefusal](ETraceMeleeRefusal Reason) -> bool
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = Reason;
		}
		return false;
	};

	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return Refuse(ETraceMeleeRefusal::NoPawn);
	}
	if (!Character->IsAlive())
	{
		return Refuse(ETraceMeleeRefusal::Dead);
	}
	if (Character->IsCarrier())
	{
		// The carrier cannot shoot (spec §4) and cannot swing, for the same reason and by the same
		// rule. See TraceMelee.h — this is the attacking half of the carrier's bargain, and the
		// defending half (the knife cannot HURT a carrier either) lives in TraceMelee::ResolveSwing.
		return Refuse(ETraceMeleeRefusal::Carrying);
	}
	// [DUALWIELD] TraceMelee::IsKnifeInHand asks "is a blade available", where IsKnifeEquipped() asks
	// "is the gun stowed". Those were the same sentence until spec v28 §10 and this is the one gate
	// that wanted the first meaning all along. With the switch off it collapses back to
	// IsKnifeEquipped() exactly, so the v27 refusal is unchanged — including for a stray melee bind,
	// which is why HandleMeleeInput needs no legacy branch of its own.
	if (!TraceMelee::IsKnifeInHand(Character))
	{
		return Refuse(ETraceMeleeRefusal::WrongWeapon);
	}
	if (IsDeploying())
	{
		// The pullout locks the BLADE too, under either switch position. Under dual-wield that is a
		// deliberate keep rather than an oversight: a player who has just tapped 2 for the SMG is
		// mid-swap for 0.2 s, and letting them melee out of it would make the swap a free animation
		// cancel. It costs at most 0.2 s of melee, and only to somebody who chose to swap.
		return Refuse(ETraceMeleeRefusal::Deploying);
	}
	if (Character->AreWeaponActionsBlocked())
	{
		// Spec §6, extended to the knife by §1's own note. A gate, not a cooldown: it opens on the
		// frame the dash state clears. Same accessor as CanFire — see there.
		return Refuse(ETraceMeleeRefusal::Dashing);
	}
	if (GetSwingCooldownRemaining() > 0.f)
	{
		return Refuse(ETraceMeleeRefusal::OnCooldown);
	}

	return true;
}

bool UTraceWeaponComponent::StartSwing(ETraceMeleeRefusal* OutRefusal)
{
	if (!CanSwing(OutRefusal))
	{
		return false;
	}

	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		// Input is a local concept, exactly as it is for the trigger: a proxy copy of somebody
		// else's pawn must never swing. Note a bot IS locally controlled on the server, which is
		// how bots swing at all.
		if (OutRefusal != nullptr)
		{
			*OutRefusal = ETraceMeleeRefusal::NotLocallyControlled;
		}
		return false;
	}

	const double Now = GetLocalTimeSeconds();

	// THE COOLDOWN IS STAMPED AT THE PRESS, not at the resolve. The user's number is "0.5 seconds
	// after a knife swing before a player can knife again", and press-to-press is the interval a
	// player can actually observe — stamping at the resolve would make the real cadence 0.5 +
	// wind-up and no amount of tuning the cooldown would produce a 0.5 s rhythm.
	LastLocalSwingTime = Now;
	SwingAnimStartLocalTime = Now;
	SwingResolveAtLocalTime = Now + static_cast<double>(TraceMelee::GetSwingWindupSeconds());
	bSwingPendingResolve = true;

	// --- FX_AUDIO_PLAN §5.1 (MeleeSwing) — THE PREDICTED HALF OF THE §6 PAIR --------------------
	//
	// AT THE PRESS, WHICH IS WHERE THE MOTION STARTS. A whoosh is the sound of the arm moving, and
	// the arm starts moving here; the blade does not resolve for another GetSwingWindupSeconds() and
	// the server does not accept until a further upstream lag after that. The world copy is played at
	// ServerSwing's accept (see there) and every machine but this one hears it, so the ~0.1 s the two
	// are apart is never audible to anybody — no listener ever hears both.
	//
	// The two guards are FireOnce's, for FireOnce's reasons: IsLocallyControlled() is already true
	// above (a proxy cannot swing), and IsPlayerControlled() is what keeps a listen host from hearing
	// every bot's swing at point-blank range.
	if (TraceAudioWatch::IsPredictedShotEnabled() && Character->IsPlayerControlled())
	{
		TraceAudio::PlayPredictedLocal(Character, TraceSoundEvents::MeleeSwing, Character->GetActorLocation());
	}

	if (TraceMelee::IsDebugLoggingEnabled())
	{
		UE_LOG(LogTraceGame, Display, TEXT("[Knife] %s swing START (blade resolves in %.3fs, next swing in %.3fs)"),
			*GetNameSafe(Character), TraceMelee::GetSwingWindupSeconds(), TraceMelee::GetSwingCooldownSeconds());
	}

	return true;
}

void UTraceWeaponComponent::TickSwing(float /*DeltaTime*/)
{
	if (!bSwingPendingResolve)
	{
		return;
	}
	if (GetLocalTimeSeconds() < SwingResolveAtLocalTime)
	{
		return;
	}
	bSwingPendingResolve = false;

	ATraceCharacter* Character = GetTraceCharacter();
	UWorld* World = GetWorld();
	if (Character == nullptr || World == nullptr)
	{
		return;
	}

	const FVector Origin = Character->GetMuzzleLocation();
	FVector Dir = Character->GetAimDirection().GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = Character->GetActorForwardVector();
	}

	// THE STAMP IS THE RESOLVE INSTANT, not the press. This is the moment the client saw the edge
	// cross the target, so it is the moment the server has to rewind to — stamping the press would
	// rewind the world to before the blade had started moving and hand the attacker a free
	// wind-up's worth of the victim's old position.
	const double SwingServerTime = GetServerTimeSeconds();

	// Cosmetic-only local resolve, exactly as FireOnce does one for the tracer: no damage is applied
	// on the client under any circumstances. On a client the lag-comp histories are empty, so this
	// resolves against the live interpolated poses the player can actually see — which is the right
	// answer for an effect.
	FTraceMeleeHit Predicted;
	TraceMelee::ResolveSwing(World, Character, Origin, Dir, static_cast<float>(SwingServerTime), Predicted);

	// ---------------------------------------------------------------------------------------------
	// NO SLASH ACTOR IS SPAWNED HERE ANY MORE. SPEC v12 §2, verbatim: "Take the 3d 'swipe' line
	// animation of the knife out of the player's view."
	//
	// This call site was the swinger's OWN copy of ATraceMeleeArc — a fan of emissive chords swept
	// across 140 degrees at blade-tip radius, drawn 140 uu in front of the camera, which is what the
	// user was looking at when they asked for it to go. It is deleted rather than made conditional:
	// there is no state in which the swinger wants a lateral arc drawn over their own crosshair now
	// that the motion is a thrust.
	//
	// WHAT REPLACES IT is the viewmodel itself — UpdateKnifeVisuals now drives KnifeViewRoot forward
	// and back rather than across, so the swinger's read is the blade going out and coming home. That
	// is procedural, and deliberately so; see the note there.
	//
	// THE THIRD-PERSON SLASH IS UNTOUCHED. MulticastSwingEffects still spawns one for everybody
	// ELSE, because a victim with no first-person blade to look at needs some tell that a hundred
	// damage just came from a direction, and it is drawn with the same GetSwingArcDegrees() the
	// server actually resolved — drawn volume and lethal volume stay equal. Reshaping that actor into
	// a thrust belongs to whoever owns TraceMeleeArc.cpp; see the pass report.
	//
	// THE LOCAL RESOLVE ABOVE IS KEPT even though nothing cosmetic consumes it now. It is the client
	// half of the "compare the two resolutions line for line" contract in TraceMelee.h's FTraceMeleeHit
	// comment, and it is the only way a mispredicted swing is visible in a log at all — the server's
	// verdict alone cannot show a disagreement. It costs one 15-ray sweep per 0.5 s per player and
	// applies no damage on any client, ever.
	// ---------------------------------------------------------------------------------------------
	if (TraceMelee::IsDebugLoggingEnabled())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Knife] %s predicted swing: victim %s, %s, %.0f damage (client-side, cosmetic only)"),
			*GetNameSafe(Character), *GetNameSafe(Predicted.Victim),
			Predicted.bBackstab ? TEXT("BACKSTAB") : TEXT("front"), Predicted.Damage);
	}

	ServerSwing(FVector_NetQuantize(Origin), FVector_NetQuantizeNormal(Dir), static_cast<float>(SwingServerTime));
}

// -------------------------------------------------------------------------------------------------
// The swap
//
// DoSwapWeaponPressed() lived here and is deleted (spec v15 §5) — see the note at its old declaration
// in TraceWeaponComponent.h. The toggle verb survives once, in TraceMelee::RequestSwapWeapon.
// -------------------------------------------------------------------------------------------------

bool UTraceWeaponComponent::RequestEquipIfDifferent(ETraceEquippedWeapon Desired, ETraceMeleeRefusal* OutRefusal)
{
	if (OutRefusal != nullptr)
	{
		*OutRefusal = ETraceMeleeRefusal::None;
	}

	// THE ONLY LINE THAT DIFFERS FROM RequestEquip, and it is checked BEFORE the legality gates on
	// purpose. "Already holding it" is not a refusal — nothing was asked for — so a dead or carrying
	// player pressing the bind for the weapon they nominally hold produces None rather than Dead or
	// Carrying. The caller logs refusals, and a refusal reason for a press that asked for nothing is
	// noise that reads like a bug report.
	//
	// EquippedWeapon is replicated, so the client's answer and the server's are the same answer; this
	// is not a second opinion about legality, and no state is duplicated to reach it.
	if (EquippedWeapon == Desired)
	{
		return false;
	}

	return RequestEquip(Desired, OutRefusal);
}

bool UTraceWeaponComponent::RequestEquip(ETraceEquippedWeapon Desired, ETraceMeleeRefusal* OutRefusal)
{
	if (OutRefusal != nullptr)
	{
		*OutRefusal = ETraceMeleeRefusal::None;
	}

	auto Refuse = [OutRefusal](ETraceMeleeRefusal Reason) -> bool
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = Reason;
		}
		return false;
	};

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return Refuse(ETraceMeleeRefusal::NoPawn);
	}

	// =============================================================================================
	// *** THE KNIFE IS A REAL SELECTOR VALUE AGAIN. THE v28 §10 NO-OP IS GONE. ***
	// =============================================================================================
	//
	// v28 §10 made the blade permanent in the off hand, so "equip the knife" asked for something the
	// pawn already had, and this function answered it with `return true` and no state change. That
	// was correct for exactly one pass. v29 §5 gave ETraceEquippedWeapon::Knife a reachable meaning
	// again — "no firearm is out" — and the early-out was therefore refusing the only thing that key
	// did. It was MEASURED refusing it: the weapon verifier read "key 1 (STOW GUNS) reaches KNIFE
	// (selector reads PISTOL)" with everything else on that path already in place.
	//
	// v31 §1 reverts dual-wield and the knife becomes a WEAPON in that slot rather than a stow, which
	// changes nothing here: it is the same value, reached the same way, on key 3 instead of key 1.
	//
	// So a knife request falls through to the SAME legality gates and the SAME kind of pullout as a
	// gun request, because it is the same kind of request: put one thing away and bring another up.
	// *** NOT THE SAME LENGTH, THOUGH, SINCE v31 §1. *** The deadline below is computed from
	// TraceMelee::GetSwapSecondsFor(Desired), so drawing the knife costs 0.65 of the base and drawing
	// either gun costs the base. TraceMelee::ShouldUseKnifeMovementProfile reads the selector for "is
	// a FIREARM out", which is what pays the +22%; ApplyEquip's magazine exchange is decided on
	// LiveClipOwner and is firearm-to-firearm, so passing THROUGH the knife slot still leaves both
	// clips exactly where they were.
	//
	// THE TWO CALLERS THAT ASKED FOR THE KNIFE AND ASSERTED THE RETURN VALUE STILL PASS, and both
	// were checked rather than assumed:
	//   * Chut's E (TraceAbilitySetChut.cpp) wants a blade to swing. It gets one — and now also
	//     puts the gun away for the pullout it takes to swing, which is what a knife ability should
	//     do — and since v31 §1 that pullout is the knife's shorter one.
	//     RequestEquip returns true through the normal path for a live, non-carrying pawn.
	//   * Modes/TracePracticeVerify.cpp's KNIFEBACK step already asserts TraceMelee::IsKnifeInHand()
	//     rather than the selector (the v28 §10 owner fixed it). Under v31 §1 that predicate is true
	//     in the knife slot only, which is exactly where that step puts the pawn.
	//
	// The old comment's "a caller that asserts IsKnifeEquipped() will see false" hazard is retired:
	// the knife is reachable and IsKnifeEquipped() is TRUE after this call, in both switch positions
	// as of v29 §5 and unambiguously so under v31 §1, where it simply means "the knife is out".

	if (!Character->IsAlive())
	{
		return Refuse(ETraceMeleeRefusal::Dead);
	}
	if (Character->IsCarrier())
	{
		// HANDS FULL. The Core is carried in both hands and in third person; a carrier who could put
		// a knife away and take it back out would be choosing between two weapons neither of which
		// they may use, and would be collecting the +22% movement bonus while doing it. See
		// TraceMelee.h. The weapon they were holding is untouched and comes back when they let go.
		return Refuse(ETraceMeleeRefusal::Carrying);
	}

	// --- THERE IS DELIBERATELY NO DASH GATE HERE, and that is a decision, not an omission. --------
	//
	// CanFire and CanSwing both refuse mid-dash (spec §6). A SWAP is neither: it fires no weapon, and
	// refusing it would mean a player who dashes in has to wait out the dash AND then the 0.2 s
	// pullout before the knife is up — a dash that costs 0.2 s of extra helplessness, which is the
	// opposite of what a mobility weapon is for.
	//
	// Checked for the three ways it could be an exploit, and it is none of them:
	//   * SHOOTING. A swap mid-dash cannot produce a shot mid-dash, because CanFire's own gate is
	//     independent of this one and still closed for the whole dash.
	//   * FREE SPEED. The knife's movement profile turns on at the press rather than at the end of
	//     the pullout, so a mid-dash swap does raise the air-strafe ceilings during the dash. It
	//     grants nothing, because those ceilings are a property of HOLDING the knife: pressing F a
	//     second before the dash produces the identical state. There is no window here that
	//     pre-swapping does not already open, and the dash's own speed is untouched either way
	//     (UTraceCharacterMovementComponent::GetMaxSpeed returns GetDashSpeed() before any
	//     multiplier is reached).
	//   * SWING CANCELLING. ApplyEquip drops a pending swing, but LastLocalSwingTime was stamped at
	//     the PRESS, so swapping out of a wind-up forfeits the swing and still pays the full 0.5 s
	//     cooldown. Cancelling is strictly worse than not cancelling; there is nothing to farm.
	//
	// The dash and the pullout simply overlap, and both clocks run to completion on their own.
	// ---------------------------------------------------------------------------------------------

	const AActor* OwnerActor = GetOwner();

	// The instant of the PRESS, on the shared clock. Both machines anchor the pullout to this, so
	// they compute the same deadline and the replicated value is a no-op rather than an extension.
	//
	// *** SPEC v31 §1 — GetSwapSecondsFor(Desired), NOT GetSwapSeconds(). *** The knife draws 35%
	// faster than a gun and the difference is keyed on WHAT IS BEING PULLED OUT, so the length has to
	// be computed from Desired at the press. Both machines pass the same Desired (the client sends it
	// in the RPC and the server re-derives the deadline from the same function), so the predicted and
	// the authoritative deadline still agree to the microsecond and the replicated value is still a
	// confirmation rather than an extension. See ServerRequestEquip_Implementation for the other half.
	const double PressServerTime = GetServerTimeSeconds();
	const double DeployEnd = PressServerTime + static_cast<double>(TraceMelee::GetSwapSecondsFor(Desired));

	if (OwnerActor != nullptr && OwnerActor->HasAuthority())
	{
		// The server, or a listen host's own pawn, or a bot: no prediction needed, no RPC to send.
		ApplyEquip(Desired, DeployEnd);
		return true;
	}

	if (!Character->IsLocallyControlled())
	{
		return Refuse(ETraceMeleeRefusal::NotLocallyControlled);
	}

	// PREDICTED, exactly as the tracer is, AND ANCHORED so the prediction is right rather than
	// merely early. A pullout that waits for a round trip is a 0.2 s feature that costs 0.2 s + RTT;
	// a pullout the server re-anchors on arrival is a 0.2 s feature that costs 0.2 s + upstream lag,
	// which is what this measured at 0.294 s before the stamp existed. Sending the press instant and
	// having the server clamp-and-anchor it removes the lag from the number entirely. See
	// ServerRequestEquip's comment for the security argument, which is the shot's, unchanged.
	ApplyEquip(Desired, DeployEnd);
	ServerRequestEquip(Desired, static_cast<float>(PressServerTime));
	return true;
}

void UTraceWeaponComponent::SwapStowedClip()
{
	const AActor* OwnerActor = GetOwner();
	const bool bAuthority = (OwnerActor != nullptr && OwnerActor->HasAuthority());

	if (bAuthority)
	{
		Swap(ClipAmmo, StowedGunClipAmmo);
		Swap(AbilityRoundsInClip, StowedGunAbilityRounds);

		// *** THE SERIAL BUMP IS WHAT MAKES THIS PREDICTION-SAFE, AND IT IS THE EXISTING MACHINERY. ***
		// RefillClip's comment already states the contract: a moved serial tells an owning client
		// "this is a NEW magazine, throw your prediction away". A swap is a new magazine by the
		// plainest possible reading, so OnRep_Ammo's second branch handles it with no new rule. It is
		// also why a mispredicted swap self-heals within one packet instead of drifting.
		++ClipSerial;
		return;
	}

	// A PREDICTING CLIENT. Seed both mirrors from the replicated truth on the first swap of the
	// connection — the same -1 sentinel ConsumeRound seeds from, for the same reason.
	if (PredictedClipAmmo < 0)
	{
		PredictedClipAmmo = static_cast<int32>(ClipAmmo);
	}
	if (PredictedStowedClipAmmo < 0)
	{
		PredictedStowedClipAmmo = static_cast<int32>(StowedGunClipAmmo);
	}
	Swap(PredictedClipAmmo, PredictedStowedClipAmmo);

	// A predicted refill that has not been confirmed belongs to the magazine that just went in the
	// pocket, so it must not go on protecting the one that came out. Clearing it is what stops the
	// "wait for the serial" branch in OnRep_Ammo from holding a stale prediction across a swap.
	bPredictedRefillPending = false;
}

void UTraceWeaponComponent::ApplyEquip(ETraceEquippedWeapon Desired, double DeployEndSharedTime)
{
	const ETraceEquippedWeapon Previous = EquippedWeapon;

	EquippedWeapon = Desired;
	DeployEndServerTime = static_cast<float>(DeployEndSharedTime);

	// --- SPEC v28 §9: each gun keeps its own magazine ---------------------------------------------
	//
	// GUN-TO-GUN ONLY, AND THE CONDITION IS THE COMPATIBILITY GUARANTEE. A knife swap in the legacy
	// build must leave the gun's clip exactly where it was — that is spec v16 §1's behaviour and 20
	// rounds must still be 20 rounds when the blade goes away — so a transition involving the knife
	// touches neither pair. A no-op request (RequestEquip is unguarded and a repeat press costs a
	// pullout) is excluded by the inequality, or leaning on the key would shuffle two magazines back
	// and forth and bump the serial sixty times a second.
	// DECIDED ON THE MAGAZINE, NOT ON THE ROUTE. The old test was
	// `Previous != Desired && IsFirearm(Previous) && IsFirearm(Desired)`, which spec v29 §5's stow
	// state defeats: pistol -> stow -> SMG is two non-gun-to-gun transitions, so nothing swapped and
	// the SMG drew the pistol's magazine. Asking whether the live clip belongs to the gun being
	// drawn is the same answer for a direct swap and the right answer through a stow.
	if (TraceIsFirearm(Desired) && LiveClipOwner != Desired)
	{
		// A RELOAD DOES NOT SURVIVE THE SWAP, and it is cancelled BEFORE the exchange so the deadline
		// dies with the magazine it belonged to rather than following it into the pocket. Putting a
		// gun away mid-reload therefore costs the reload — which is the reading every shooter uses,
		// and which is what stops "swap out, swap in" from being a way to shorten one.
		CancelReload();
		SwapStowedClip();
		LiveClipOwner = Desired;
	}

	// A swap cancels a swing that has not resolved yet. The alternative — letting the blade land
	// after the knife has been put away — is a hit from a weapon that is visibly not in the
	// player's hands, which is the least defensible thing a melee can do.
	bSwingPendingResolve = false;
	SwingAnimStartLocalTime = -1000.0;

	// SPEC v28 §9. What THIS machine believes it just selected, remembered before the OnRep body
	// runs. On a predicting client it is what lets the next replicated update be classified as a
	// CONFIRMATION (same weapon — keep the predicted magazines) or a CORRECTION (the server refused
	// the swap — throw them away). See OnRep_EquippedWeapon.
	LocallyAppliedWeapon = Desired;

	// The server does not receive its own OnRep, and on a listen host the presentation must still
	// follow. Calling it directly is what keeps the two paths identical.
	OnRep_EquippedWeapon();

	if (TraceMelee::IsDebugLoggingEnabled())
	{
		// v31 §1: the pullout is per-DESTINATION now, so print the one this swap actually paid beside
		// the base it came from — "0.130s (base 0.200s x0.65)" is self-explaining in a log tail, where
		// a bare 0.130 against a header that says 0.2 reads as a bug.
		UE_LOG(LogTraceGame, Display, TEXT("[Knife] %s equip %s -> %s (pullout %.3fs = base %.3fs x %.2f, ends at shared t=%.3f)"),
			*GetNameSafe(GetOwner()), LexToString(Previous), LexToString(Desired),
			TraceMelee::GetSwapSecondsFor(Desired), TraceMelee::GetSwapSeconds(),
			TraceIsFirearm(Desired) ? 1.f : TraceMelee::GetKnifeSwapMultiplier(), DeployEndServerTime);
	}
}

void UTraceWeaponComponent::OnRep_EquippedWeapon()
{
	// Which rig is visible is re-decided every tick from the replicated selector
	// (UpdateKnifeVisuals), so all this has to do about presentation is make sure a swap cannot
	// leave a half-swung blade frozen mid-arc on the machine that just learned about it.
	SwingAnimStartLocalTime = -1000.0;

	// --- SPEC v28 §9: THE ONE PREDICTION HOLE A PER-WEAPON MAGAZINE OPENS -------------------------
	//
	// An owning client predicts its swap and exchanges its predicted pair (SwapStowedClip). If the
	// server then REFUSES that swap — the client died or picked the Core up in the same instant — the
	// selector replicates back to the weapon it never left, but ClipSerial did NOT move, so
	// OnRep_Ammo's "a new clip, throw the prediction away" branch never fires and the client would go
	// on displaying the OTHER gun's count for the rest of the magazine.
	//
	// Dropping the mirrors on any selector update closes it in two lines. The cost is that the client
	// re-seeds from the replicated truth after every swap, losing per-shot prediction for ~RTT/2 —
	// which lands entirely inside the 0.2 s pullout, where the gun cannot fire anyway. Being briefly
	// authoritative is the right direction to be wrong in; being persistently wrong about which
	// magazine is loaded is not.
	//
	// ONLY ON A CONTRADICTION, WHICH IS THE WHOLE SUBTLETY. ApplyEquip calls this function directly
	// so that a listen host and a predicting client run one code path — so an unconditional reset
	// here would also fire on the client's OWN predicted swap and discard the prediction it had just
	// made, one statement after making it. LocallyAppliedWeapon is what tells the two apart: equal
	// means the server agreed (or this IS the server), and the mirrors are already right.
	//
	// The order of the two OnReps does not matter in the correction case. If this runs first,
	// OnRep_Ammo takes its first-update branch and adopts the truth; if it runs second, GetClipAmmo()
	// falls through to the replicated count, which is the same number.
	if (EquippedWeapon != LocallyAppliedWeapon)
	{
		PredictedClipAmmo = -1;
		PredictedStowedClipAmmo = -1;
		bPredictedRefillPending = false;
		LocallyAppliedWeapon = EquippedWeapon;
	}

	// --- FX_AUDIO_PLAN §5.1 (WeaponSwitch) — REPLICATED-LOCAL, WHICH IS WHY THERE IS NO RPC -------
	//
	// This function already runs on every machine exactly once per swap: on the clients because the
	// selector replicated, and on the authority because ApplyEquip calls it by hand for precisely
	// that reason. So the replication IS the multicast, and TraceAudio::PlayReplicatedLocal plays it
	// spatialised, locally, with no packet of its own. WeaponSwitch is declared CLIENT-side in
	// Audio/TraceSoundEvents.h so that a stray TraceAudio::Play() on it can never multicast on top
	// (§1.6.3's doctrine) — the side declaration and this call site are one design, not two.
	//
	// The seed-then-change test is LastSoundedWeapon's whole job; see its comment for the join-time
	// burst it prevents.
	if (!bSoundedWeaponValid)
	{
		bSoundedWeaponValid = true;
		LastSoundedWeapon = EquippedWeapon;
	}
	else if (LastSoundedWeapon != EquippedWeapon)
	{
		LastSoundedWeapon = EquippedWeapon;
		if (const ATraceCharacter* SwitchCharacter = GetTraceCharacter())
		{
			TraceAudio::PlayReplicatedLocal(this, TraceSoundEvents::WeaponSwitch,
				SwitchCharacter->GetActorLocation());
		}
	}

	// The movement profile, on the very frame the selector changed rather than on the next tick. On
	// a simulated proxy that is what keeps the pawn's simulated speed matching the one the server
	// moved it at; on the owning client the predicted equip already called this, so it is a no-op.
	RefreshMovementProfile();
}

void UTraceWeaponComponent::RefreshMovementProfile()
{
	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	if (UTraceCharacterMovementComponent* Movement = Character->GetTraceMovement())
	{
		// TraceMelee::ShouldUseKnifeMovementProfile is the single definition of the answer, so the
		// movement component, the HUD and a test all ask the same question. It folds in the carrier
		// clause; see its comment for why a carrier holding a knife is holding a STOWED knife.
		const bool bActive = TraceMelee::ShouldUseKnifeMovementProfile(Character);
		if (Movement->IsKnifeMovementProfileActive() != bActive)
		{
			Movement->SetKnifeMovementProfileActive(bActive);

			if (TraceMelee::IsDebugLoggingEnabled())
			{
				UE_LOG(LogTraceGame, Display, TEXT("[Knife] %s movement profile -> %s"),
					*GetNameSafe(Character), bActive ? TEXT("KNIFE (fast)") : TEXT("base"));
			}
		}
	}
}

bool UTraceWeaponComponent::ServerRequestEquip_Validate(ETraceEquippedWeapon Desired, float ClientPressServerTime)
{
	// Validation failure disconnects the client, so this only rejects what is outright impossible to
	// reason about — a uint8 outside the enum, which no honest client can produce, and a stamp that
	// is not a number at all. A merely WRONG stamp is not a kick: it is clamped below, exactly as a
	// wrong shot timestamp is, because an honest client with a bad clock is not a cheater.
	if (!FMath::IsFinite(ClientPressServerTime))
	{
		return false;
	}
	// SPEC v28 §9 adds Smg. This list is the reason ETraceEquippedWeapon is append-only: a value the
	// server does not recognise DISCONNECTS the sender, so a client built against a newer enum than
	// the server would be kicked for pressing 2. Appending keeps every old value meaning what it did.
	return Desired == ETraceEquippedWeapon::Gun
		|| Desired == ETraceEquippedWeapon::Knife
		|| Desired == ETraceEquippedWeapon::Smg;
}

void UTraceWeaponComponent::ServerRequestEquip_Implementation(ETraceEquippedWeapon Desired, float ClientPressServerTime)
{
	const AActor* OwnerActor = GetOwner();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || Character == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	// *** SPEC v29 §5 — THE MATCHING REFUSAL IS GONE, FOR THE SAME REASON THE CLIENT'S NO-OP IS. ***
	//
	// This refused a Knife equip on the grounds that no honest client could ask for one (the client's
	// RequestEquip turned it into a no-op and never sent the RPC). Under v29 §5 the 1 key sends
	// exactly this RPC for exactly this value, so the guard was rejecting the honest case: the
	// client predicted "stowed", the server refused, and the replicated selector snapped the guns
	// back out. Nothing about it was a security gate — ValidateEquipRequest already lists Knife as a
	// value this build recognises (it must, or a modified client sending it would be DISCONNECTED),
	// and the real gates (alive, not carrying, clamped press stamp) are below and untouched.
	//
	// "Weaponless" was the old worry and it is not reachable: the blade is permanent under
	// dual-wield, so the stowed state is knife-only, not empty-handed, and CanFire's
	// `if (!IsFirearmEquipped()) return false;` is what makes it a trade rather than a bug.

	// The SAME gates the client applied, re-asked here. The client's claim is only ever "I pressed
	// swap"; whether that is legal is decided once, here.
	if (!Character->IsAlive() || Character->IsCarrier())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerRequestEquip: refusing %s for %s"),
			LexToString(Desired), *GetNameSafe(OwnerActor));
		return;
	}

	// ANCHOR THE PULLOUT AT THE STAMPED PRESS, NOT AT ARRIVAL.
	//
	// Anchoring at arrival is what made a client's measured pullout 0.294 s against a specified
	// 0.2 s: the client started its 0.2 s at the press, the server started ITS 0.2 s when the RPC
	// landed one upstream-lag later, and the replicated deadline then pushed the client's out to
	// match the server's. Both machines now derive the identical shared-clock instant from the same
	// stamp, so the replicated value confirms the client's prediction instead of extending it.
	//
	// THE CLAMP IS THE SECURITY, and it is the shot's clamp unchanged. Whatever the client claims,
	// the press is pinned into [ServerNow - MaxRewindTime, ServerNow]: never in the future, so a
	// swap cannot be pre-booked to complete the instant it is asked for, and never further back than
	// a bullet may be rewound, so a backdated stamp cannot be used to skip the pullout outright. The
	// worst a liar buys is MaxRewindTime of head start, which is the same budget the gun already
	// grants and is bounded by the same setting.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const double ServerNow = GetServerTimeSeconds();
	double PressServerTime = ServerNow;
	if (ClientPressServerTime > 0.f)
	{
		const double MaxRewind = FMath::Max(0.f, Settings.MaxRewindTime);
		PressServerTime = FMath::Clamp(static_cast<double>(ClientPressServerTime), ServerNow - MaxRewind, ServerNow);
	}

	// SPEC v31 §1. THE LENGTH IS DERIVED FROM Desired ON THE SERVER TOO, from the same function the
	// client used, so the knife's shorter pullout is authoritative rather than a client-side feeling.
	// The client cannot lengthen or shorten it by lying — it sends only WHICH weapon it wants (already
	// validated by ValidateEquipRequest) and WHEN it pressed (already clamped just above); the seconds
	// come from the server's own settings either way.
	ApplyEquip(Desired, PressServerTime + static_cast<double>(TraceMelee::GetSwapSecondsFor(Desired)));
}

// -------------------------------------------------------------------------------------------------
// The swing, on the authority
// -------------------------------------------------------------------------------------------------

bool UTraceWeaponComponent::ServerSwing_Validate(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientSwingServerTime)
{
	if (Origin.ContainsNaN() || Direction.ContainsNaN())
	{
		return false;
	}
	if (!FMath::IsFinite(ClientSwingServerTime))
	{
		return false;
	}
	return true;
}

void UTraceWeaponComponent::ServerSwing_Implementation(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientSwingServerTime)
{
	AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || World == nullptr || Character == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	// ---- payload sanity (never check() on network input) ---------------------------------
	FVector Dir(Direction);
	const double DirLengthSq = Dir.SizeSquared();
	if (DirLengthSq < 0.25 || DirLengthSq > 4.0)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: rejecting non-unit direction from %s"), *GetNameSafe(OwnerActor));
		return;
	}
	Dir = Dir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return;
	}
	if (FMath::Abs(Origin.X) > MaxReasonableCoordinateUU || FMath::Abs(Origin.Y) > MaxReasonableCoordinateUU || FMath::Abs(Origin.Z) > MaxReasonableCoordinateUU)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: rejecting out-of-world origin from %s"), *GetNameSafe(OwnerActor));
		return;
	}

	// ---- state gate — the same four rules CanSwing applies on the client ------------------
	// AreWeaponActionsBlocked, not IsDashing, for the same one-definition reason CanSwing uses it —
	// and note this copy is the one that matters: the server's dash clock is authoritative, so a
	// modified client that skipped its own gate is refused here.
	// [DUALWIELD] IsKnifeInHand, not IsKnifeEquipped — the same substitution CanSwing makes, and this
	// is the copy that matters: the server's is the gate a modified client cannot skip. It collapses
	// back to IsKnifeEquipped() when the v28 §10 switch is off, so a v27 revert re-tightens the
	// server-side rule at the same moment it re-tightens the client's.
	if (!Character->IsAlive() || Character->IsCarrier() || !TraceMelee::IsKnifeInHand(Character)
		|| Character->AreWeaponActionsBlocked())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: state gate refused %s (alive=%d carrier=%d knife=%d dashing=%d)"),
			*GetNameSafe(OwnerActor), Character->IsAlive() ? 1 : 0, Character->IsCarrier() ? 1 : 0,
			TraceMelee::IsKnifeInHand(Character) ? 1 : 0, Character->AreWeaponActionsBlocked() ? 1 : 0);
		return;
	}

	// The pullout, judged on the server's own clock. The client already refused to swing inside it;
	// this is the copy that a modified client cannot skip.
	if (IsDeploying())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: %s is still deploying (%.3fs left)"),
			*GetNameSafe(OwnerActor), GetDeployRemaining());
		return;
	}

	// ---- swing rate, with slack for honest jitter ----------------------------------------
	const double Cooldown = FMath::Max(0.05f, TraceMelee::GetSwingCooldownSeconds());
	const double LocalNow = GetLocalTimeSeconds();
	if ((LocalNow - LastAcceptedSwingTime) < Cooldown * (1.0 - SwingRateTolerance))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: rate-limited %s (%.3fs since last accepted)"),
			*GetNameSafe(OwnerActor), LocalNow - LastAcceptedSwingTime);
		return;
	}
	LastAcceptedSwingTime = LocalNow;

	// ---- rewind window -------------------------------------------------------------------
	const UTraceSettings& Settings = UTraceSettings::Get();
	const double ServerNow = GetServerTimeSeconds();
	double RewindTime = ServerNow;
	if (Settings.bEnableLagCompensation && ClientSwingServerTime > 0.f)
	{
		// The identical clamp ServerFire applies. However stale or futuristic the client's stamp is,
		// we only ever look back at most MaxRewindTime and never forward at all.
		const double MaxRewind = FMath::Max(0.f, Settings.MaxRewindTime);
		RewindTime = FMath::Clamp(static_cast<double>(ClientSwingServerTime), ServerNow - MaxRewind, ServerNow);
	}

	// ---- blade origin sanity -------------------------------------------------------------
	//
	// TIGHTER THAN THE GUN'S, deliberately. ServerFire forgives 500 uu of muzzle error because a
	// bullet's range is 36000 and 500 uu of slop changes nothing about what it can reach. The blade
	// reaches 180 uu, so the same 500 uu would let a client swing from nearly four blade-lengths
	// away — i.e. it would be the dominant term in the weapon's range rather than a rounding
	// allowance. 200 uu still covers the capsule, the muzzle offset and a couple of frames of
	// movement at knife speed (976 uu/s is 16 uu per frame at 60 Hz).
	constexpr double MaxSwingOriginErrorUU = 200.0;

	FVector SwingOrigin(Origin);
	FVector ReferencePoint = Character->GetMuzzleLocation();
	if (const UTraceLagCompensationComponent* AttackerLagComp = Character->FindComponentByClass<UTraceLagCompensationComponent>())
	{
		FTraceLagCompFrame AttackerFrame;
		if (AttackerLagComp->GetPoseAtTime(static_cast<float>(RewindTime), AttackerFrame))
		{
			// Comparing against the attacker's LIVE position would punish anyone with latency, who
			// legitimately swung from where they used to be.
			ReferencePoint = AttackerFrame.CapsuleCenter;
		}
	}

	if (FVector::DistSquared(SwingOrigin, ReferencePoint) > MaxSwingOriginErrorUU * MaxSwingOriginErrorUU)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: implausible blade origin from %s, snapping to server pose"),
			*GetNameSafe(OwnerActor));
		SwingOrigin = ReferencePoint;
	}

	// --- FX_AUDIO_PLAN §5.1 (MeleeSwing) — THE WORLD HALF OF THE §6 PAIR ------------------------
	//
	// THE ACCEPT, not the request: every gate above this line can refuse the swing, and a whoosh for
	// a swing that never happened is the melee version of a phantom gunshot. Excluding the swinger's
	// own pawn is what stops the machine that already played its predicted copy (StartSwing) from
	// hearing a second one — on a listen host both run in this process and the exclusion cancels the
	// local play. A BOT excludes nobody, because no machine player-controls one, so a bot's swing is
	// heard by everyone exactly as it would be through PlayAt.
	if (TraceAudioWatch::IsPredictedShotEnabled())
	{
		TraceAudio::PlayAtExcluding(Character, TraceSoundEvents::MeleeSwing, SwingOrigin, Character);
	}
	else
	{
		TraceAudio::PlayAt(Character, TraceSoundEvents::MeleeSwing, SwingOrigin);
	}

	// ---- resolve -------------------------------------------------------------------------
	FTraceMeleeHit Hit;
	TraceMelee::ResolveSwing(World, Character, SwingOrigin, Dir, static_cast<float>(RewindTime), Hit);

	bool bKilled = false;
	if (Hit.Victim != nullptr)
	{
		if (UTraceHealthComponent* VictimHealth = Hit.Victim->FindComponentByClass<UTraceHealthComponent>())
		{
			// TWO CAUSES, not one, for the same reason ServerFire passes "Headshot" instead of
			// letting the kill feed infer it from the victim's previous health: the approach angle
			// is known EXACTLY here and nowhere after. ApplyDamage takes a cause and the health
			// component clamps at zero, so a back-stab and a front swipe would otherwise arrive at
			// the death handler as one indistinguishable "Knife".
			const FName Cause = Hit.bBackstab ? TraceMelee::GetBackstabKillCause() : TraceMelee::GetKnifeKillCause();

			// SPEC v14 §6 — the ability passive pipeline, exactly as ServerFire does it. Chut's Chud
			// is "30% less damage from body shots AND MELEES", so the knife has to pass through it
			// too, and bMelee is the flag that tells it apart from a bullet.
			//
			// X's vulnerable is again NOT here: it is one multiplication inside
			// UTraceHealthComponent::ApplyDamage, which the next line reaches. A knife on a marked
			// target is amplified there, once.
			//
			// The carrier is unaffected by any of this: TraceMelee::ResolveSwing already refused to
			// return a carrier as a victim (Hit.bBlockedByCarrierShield below), so this branch is
			// unreachable for one — which is the proven 5/5 rule, untouched.
			FTraceAbilityDamageContext AbilityContext;
			AbilityContext.Instigator   = Character;
			AbilityContext.Target       = Hit.Victim;
			AbilityContext.Cause        = Cause;
			AbilityContext.bHeadshot    = (Hit.Zone == ETraceHitZone::Head);
			AbilityContext.bMelee       = true;
			AbilityContext.bFromAbility = false;
			const float MeleeDamage = UTraceAbilityComponent::ModifyDamageThroughPassives(Hit.Damage, AbilityContext);

			VictimHealth->ApplyDamage(MeleeDamage, Character->GetController(), Cause);

			// ApplyDamage no-ops against an invulnerable target, so read the result rather than
			// assuming the hit landed.
			bKilled = !VictimHealth->IsAlive();

			// --- FX_AUDIO_PLAN §5.1 (MeleeHit / MeleeBackstab) ----------------------------------
			//
			// *** HERE AND NOT IN TraceMelee::ResolveSwing, WHICH IS WHERE THE PLAN'S LINE REFERENCE
			// POINTS. *** The plan calls that site "damage application"; it is not. ResolveSwing is a
			// pure resolver and it runs TWICE in this process on a listen host — once as the
			// swinger's own cosmetic prediction in TickSwing, and again here on the authority — so a
			// PlayAt inside it would announce every host swing twice. This is the one place a knife
			// hit is a fact: the damage has been applied and the verdict is known.
			//
			// TWO EVENTS AND NOT ONE PLUS A FLAG, matching the two kill causes immediately above:
			// a back-stab is a different thing happening to you than a front cut (a hundred damage
			// against forty) and the sound is most of how the victim learns which they just took.
			// World-side, at the IMPACT POINT rather than at either pawn, so a bystander hears where
			// the blade landed.
			TraceAudio::PlayAt(Character,
				Hit.bBackstab ? TraceSoundEvents::MeleeBackstab : TraceSoundEvents::MeleeHit,
				Hit.ImpactPoint);
		}

		if (ATracePlayerController* AttackerController = Cast<ATracePlayerController>(Character->GetController()))
		{
			// The zone rides along so the hitmarker stays positional, exactly as it does for a shot.
			// The knife's damage does NOT come from the zone — back or front decides that — but a
			// player reading their own hitmarker learns where the blade landed either way.
			//
			// C5 — THE MELEE PATH PASSES ITS OWN SHIELD FACT, and here it is always false BY
			// CONSTRUCTION: TraceMelee::ResolveSwing refuses to return a carrier as a victim, so a
			// blocked swing has no Hit.Victim and never reaches this branch at all — it lands in the
			// `else if (Hit.bBlockedByCarrierShield)` below, which deliberately notifies NOTHING.
			// A blocked knife draws no marker today and C5 does not add one: the shooter's lying
			// white marker was a hitscan defect (code-gameplay F2), and inventing melee feedback for
			// a rule the design states as "carriers are immune to melee" is the UI plan's call, not
			// this tranche's. The flag is passed rather than hard-coded false so that a future
			// ResolveSwing which DOES return a shielded victim carries the truth without an edit here.
			AttackerController->ClientNotifyHit(bKilled, Hit.Zone, Hit.bBlockedByCarrierShield);
		}

		UE_LOG(LogTraceGame, Verbose,
			TEXT("ServerSwing: %s %s %s for %.0f (approach %.1fdeg, rewind %.3fs)%s"),
			*GetNameSafe(Character), Hit.bBackstab ? TEXT("BACK-STABBED") : TEXT("cut"),
			*GetNameSafe(Hit.Victim), Hit.Damage, Hit.ApproachAngleDegrees, ServerNow - RewindTime,
			bKilled ? TEXT(" [KILL]") : TEXT(""));
	}
	else if (Hit.bBlockedByCarrierShield)
	{
		// Logged by name because "I clearly hit them and nothing happened" against a carrier is the
		// single most likely bug report this weapon will generate, and it is not a bug. See
		// TraceMelee.h.
		UE_LOG(LogTraceGame, Verbose,
			TEXT("ServerSwing: %s's blade stopped on a Core carrier's shield — carriers are immune to melee by design (spec v10 §1)."),
			*GetNameSafe(Character));
	}

	// Unreliable and cosmetic: everyone but the swinger draws the slash.
	MulticastSwingEffects(FVector_NetQuantize(SwingOrigin), FVector_NetQuantizeNormal(Dir), Hit.Victim != nullptr);
}

void UTraceWeaponComponent::MulticastSwingEffects_Implementation(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, bool bConnected)
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	UWorld* World = GetWorld();
	if (Character == nullptr || World == nullptr)
	{
		return;
	}

	// The swinger drew this the instant the blade resolved locally. Drawing it again would double
	// the effect and, worse, draw the server's slightly different arc over the one the player
	// already saw — the same owner-skipping contract MulticastFireEffects honours.
	if (Character->IsLocallyControlled())
	{
		return;
	}

	// The swinging pawn also plays its own third-person blade animation from here, so a bystander
	// sees the arm move and the slash together rather than a slash from a static body.
	SwingAnimStartLocalTime = GetLocalTimeSeconds();

	const FVector Dir = FVector(Direction).GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return;
	}

	ATraceMeleeArc::Spawn(World, Origin, Dir, TraceMelee::GetSwingAxis(Character, Dir),
		TraceMelee::GetSwingArcDegrees(), TraceMelee::GetSwingRangeUU(),
		TraceTeamColor(Character->GetTeam()), bConnected);
}

// -------------------------------------------------------------------------------------------------
// The victim-facing ring — the one thing the knife could not borrow from the gun.
// -------------------------------------------------------------------------------------------------

void UTraceWeaponComponent::RecordFacingSample(float ServerTime)
{
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority() || !FMath::IsFinite(ServerTime))
	{
		return;
	}

	// Guard against being driven twice in one frame: two samples with the same timestamp would give
	// the interpolation below a zero-length span to work across.
	if (FacingHistory.Num() > 0 && ServerTime <= FacingHistory.Last().ServerTime)
	{
		return;
	}

	FTraceFacingSample& Sample = FacingHistory.AddDefaulted_GetRef();
	Sample.ServerTime = ServerTime;
	Sample.Yaw = static_cast<float>(OwnerActor->GetActorRotation().Yaw);

	// Trimmed against the SAME window the pose history uses, so a swing can never be rewound to an
	// instant where the position is known and the facing is not.
	const float Duration = FMath::Max(0.05f, UTraceSettings::Get().LagCompHistoryDuration);
	int32 FirstToKeep = 0;
	while (FirstToKeep + 1 < FacingHistory.Num()
		&& (ServerTime - FacingHistory[FirstToKeep + 1].ServerTime) > Duration)
	{
		++FirstToKeep;
	}
	if (FirstToKeep > 0)
	{
		FacingHistory.RemoveAt(0, FirstToKeep);
	}
	if (FacingHistory.Num() > MaxFacingSamples)
	{
		FacingHistory.RemoveAt(0, FacingHistory.Num() - MaxFacingSamples);
	}
}

bool UTraceWeaponComponent::GetFacingYawAtTime(const ATraceCharacter* Character, float ServerTime, float& OutYaw)
{
	if (Character == nullptr)
	{
		return false;
	}

	const UTraceWeaponComponent* Weapon = Character->FindComponentByClass<UTraceWeaponComponent>();
	if (Weapon == nullptr || Weapon->FacingHistory.Num() == 0)
	{
		// No history: a client (which records none), a pawn that just spawned, or a build with lag
		// compensation off. OutYaw is left exactly as the caller set it so their live-pose fallback
		// survives — see TraceMelee::ResolveSwing.
		return false;
	}

	const TArray<FTraceFacingSample>& History = Weapon->FacingHistory;

	if (ServerTime >= History.Last().ServerTime)
	{
		OutYaw = History.Last().Yaw;
		return true;
	}
	if (ServerTime < History[0].ServerTime)
	{
		// Older than anything retained. Refuse rather than invent a facing, exactly as
		// UTraceLagCompensationComponent::GetPoseAtTime refuses to invent a pose.
		return false;
	}

	for (int32 Index = History.Num() - 1; Index > 0; --Index)
	{
		const FTraceFacingSample& B = History[Index];
		const FTraceFacingSample& A = History[Index - 1];
		if (ServerTime >= A.ServerTime && ServerTime <= B.ServerTime)
		{
			const float Span = B.ServerTime - A.ServerTime;
			const float Alpha = (Span > 1.e-4f)
				? FMath::Clamp((ServerTime - A.ServerTime) / Span, 0.f, 1.f)
				: 0.f;

			// UnwindDegrees on the DELTA, not a plain lerp of the two yaws. A pawn crossing the
			// +/-180 seam would otherwise interpolate the long way round the circle, and for the
			// couple of frames that took, an attacker standing in the victim's back would be told
			// they were standing in their face.
			OutYaw = A.Yaw + FMath::UnwindDegrees(B.Yaw - A.Yaw) * Alpha;
			return true;
		}
	}

	OutYaw = History[0].Yaw;
	return true;
}

// -------------------------------------------------------------------------------------------------
// Presentation — the knife you can see. Cosmetic throughout; nothing here is read by the resolution.
// -------------------------------------------------------------------------------------------------

namespace TraceKnifeLayout
{
	// FIRST PERSON, in ATraceCharacter's viewmodel rig space: +X out of the lens, +Y right, +Z up,
	// origin at the top-rear of the gun's grip, and every Size is a WORLD size that AddKnifePart
	// divides by the 100 uu engine primitive.
	//
	// The framing and depth arithmetic in TraceCharacter.cpp's header both have to keep holding:
	//
	//   DEPTH. The deepest point of this rig is the blade tip at x = 23 uu. The gun's muzzle is at
	//   76 and is the piece that had to clear the capsule, so a 23 uu blade is a third of the way
	//   there — it can never intersect the world.
	//   FRAMING. The rig hangs where the gun hangs (the root's rest location is the gun's), so it
	//   sits in the same lower-right corner and clears the crosshair by the same margin.
	//
	// The blade is offset toward the right hand's block (which stays visible) rather than centred,
	// so it reads as being HELD rather than as floating where the gun used to be.
	constexpr float ShapeUnit = 100.f;

	struct FKnifePart
	{
		const TCHAR* Name;
		FVector Location;
		FRotator Rotation;
		FVector Size;
		bool bNeon;
	};

	const FKnifePart FirstPersonParts[] =
	{
		// Grip, guard, blade, tip: four masses, which is the least that reads as a knife rather than
		// as a stick. The 8 degree cant is what stops it looking like a prop welded to the lens.
		{ TEXT("KnifeGrip"),   FVector(0.4f, 0.2f, -4.4f),  FRotator(8.f, 0.f, 0.f),  FVector(7.4f, 2.6f, 3.0f),  false },
		{ TEXT("KnifePommel"), FVector(-3.6f, 0.2f, -5.0f), FRotator(8.f, 0.f, 0.f),  FVector(1.4f, 3.0f, 3.4f),  true  },
		{ TEXT("KnifeGuard"),  FVector(4.4f, 0.2f, -3.8f),  FRotator(8.f, 0.f, 0.f),  FVector(1.3f, 5.4f, 3.6f),  false },
		{ TEXT("KnifeBlade"),  FVector(12.6f, 0.2f, -2.7f), FRotator(4.f, 0.f, 0.f),  FVector(15.0f, 1.0f, 3.4f), false },
		// The edge is the only lit part, and it is a LINE rather than a face: this arena is black
		// surfaces and neon edges, and a glowing blade edge is the same language as everything else
		// in it. It is also the part that reads at a glance as "that is a knife, not a pistol".
		{ TEXT("KnifeEdge"),   FVector(12.8f, 0.2f, -4.2f), FRotator(4.f, 0.f, 0.f),  FVector(15.4f, 0.6f, 0.7f), true  },
		{ TEXT("KnifeTip"),    FVector(21.4f, 0.2f, -2.0f), FRotator(4.f, 0.f, 0.f),  FVector(3.6f, 0.9f, 1.9f),  false }
	};

	// THIRD PERSON, in hand_r socket space. Smaller numbers because this is a real-world-scale hand
	// rather than a viewmodel — the Mannequin's hand is about 10 uu across, and a blade the size of
	// the first-person one would be a sword.
	const FKnifePart HandParts[] =
	{
		{ TEXT("HandKnifeGrip"),  FVector(3.0f, 0.f, 0.f),  FRotator::ZeroRotator, FVector(9.0f, 2.4f, 2.4f),  false },
		{ TEXT("HandKnifeGuard"), FVector(8.0f, 0.f, 0.f),  FRotator::ZeroRotator, FVector(1.2f, 5.0f, 3.0f),  false },
		{ TEXT("HandKnifeBlade"), FVector(17.0f, 0.f, 0.f), FRotator::ZeroRotator, FVector(17.0f, 1.0f, 3.0f), false },
		{ TEXT("HandKnifeEdge"),  FVector(17.2f, 0.f, -1.6f), FRotator::ZeroRotator, FVector(17.4f, 0.6f, 0.7f), true }
	};

	/** Where the blade sits in the hand. Rotated so it runs along the fingers, edge outward. */
	const FVector HandOffset(-2.f, 4.f, 0.f);
	const FRotator HandRotation(0.f, 0.f, 0.f);

	// ---------------------------------------------------------------------------------------------
	// [DUALWIELD] THE OFF-HAND REST POSE  (spec v28 §10)
	// ---------------------------------------------------------------------------------------------
	//
	// The parts table above is authored around KnifeViewRoot's ORIGIN, which in the v27 build sits at
	// the rig root — i.e. exactly where the gun hangs, because the knife was replacing it. Under
	// dual-wield the gun is still there, so the whole rig has to move into the off hand instead.
	//
	// It is expressed as a DELTA ONTO ATraceCharacter's own off-hand anchor rather than as a second
	// set of absolute coordinates, which is the standing rule: retune
	// TraceCharacterLayout::DualWieldLeftHand and the blade follows the fist instead of being left
	// behind in mid-air. The delta only has to say where the grip sits relative to the knuckles.
	//
	//   -3.2 back    the table's grip centre is +0.4 ahead of the root and the pommel is -3.6 behind
	//                it, so pulling the rig back by ~3 puts the FIST around the grip rather than
	//                around the guard.
	//   +0.6 out     a fraction outboard, so the blade clears the outside of the hand block.
	//   +4.6 up      the parts table hangs everything 2.7-5.0 below its root (it was authored to sit
	//                under the gun's grip line); lifting the root by that much puts the blade back
	//                level with the fist it is now being held in.
	//
	// THE CANT, AND THE ONE NUMBER THAT HAD TO BE MEASURED RATHER THAN REASONED ABOUT.
	//
	// A first-person primitive is rendered with its DEPTH COMPRESSED (EFirstPersonPrimitiveType::
	// FirstPerson halves it — the same scaling the file header's depth arithmetic is built around),
	// so a blade lying mostly along +X has its X extent squashed while its Z extent does not. An
	// angle authored in rig space therefore reads STEEPER on screen than the number says.
	//
	// MEASURED: this first shipped at pitch 22 / yaw -14 / roll 10, which was chosen as a modest
	// raised guard and photographed as a near-vertical obelisk — the blade stood up past the horizon
	// like a fence post rather than being held. 22 degrees of authored pitch rendered at roughly 60.
	// Roughly a third of the authored value survives to the screen, so the pose is authored at a
	// third of what it should look like:
	//
	//   pitch  +7    reads as ~20 on screen: the tip leads, slightly up, the way a held blade does.
	//   yaw   -10    the point crosses inboard toward the crosshair. Yaw is NOT compressed (it is a
	//                rotation in the screen plane at this pitch), so this one is close to literal.
	//   roll   +4    just enough to turn the lit edge (the only neon part of the knife) toward the
	//                camera instead of hiding it under the blade. Small, because roll is what made
	//                the old swipe animation read as a slash and too much of it here would fight the
	//                stab.
	//
	// The Z lift comes down with the pitch: less nose-up means the parts table's own -2.7..-5.0 sag
	// needs less correcting, and a tip that no longer rides high does not need the extra clearance.
	const FVector OffHandOffset(-3.2f, 0.6f, 3.4f);
	const FRotator OffHandRotation(7.f, -10.f, 4.f);

	// ---------------------------------------------------------------------------------------------
	// THE STAB  (spec v12 §2: "Can you make the knife animation a stab instead of a swipe?")
	//
	// SAY THIS PLAINLY: THIS IS A PROCEDURAL VIEWMODEL MOTION, NOT AN AUTHORED ANIMATION. Epic's
	// Mannequin ships no stab, no slash and no melee sequence of any kind — the imported set is
	// Death, Jump, Pistol, Rifle and Unarmed locomotion, verified previously, and it is the same
	// absence that forced UpdateCrouchPresentation to pose the slide by hand. There is no sequence to
	// play, so the motion is arithmetic on KnifeViewRoot's relative transform, three keyframes deep.
	//
	// WHAT CHANGED FROM THE SWIPE IT REPLACES, and it is the whole point of the request: the old
	// motion's signature was SweepYaw -58 and SweepRoll +46, i.e. the blade travelled ACROSS the
	// screen and rolled over as it went. A stab has no lateral signature at all. Everything below is
	// dominated by +X — straight out of the lens, down the aim ray — with only a few degrees of yaw
	// and pitch so it reads as an arm rather than as a mesh sliding on rails. Roll is gone entirely;
	// roll is what made it a slash.
	//
	// FAST OUT, SLOWER RETURN, which is the shape the spec asked for and also the shape of a real
	// thrust: the arm commits in a snap and recovers under control. Out is a quarter of the
	// post-windup time, the return is the other three quarters — see UpdateKnifeVisuals.
	//
	// DEPTH ARITHMETIC, which has to keep holding (see the layout note at the top of this namespace):
	// the deepest point of the rig is the blade tip at x = 21.4 + 3.6/2 ~= 23 uu. A 16 uu thrust puts
	// it at ~39 uu. The gun's muzzle sits at 76 uu and is the piece that had to clear the capsule, so
	// the fully extended blade is still barely half way there and can never intersect the world.
	// Raising ThrustForwardUU past ~50 would start to matter; do not.

	/** Cock: the blade draws back and the tip lifts, ready to go. Eased IN, so it loads the thrust. */
	constexpr float CockBackUU = 4.6f;
	constexpr float CockUpUU = 1.4f;
	constexpr float CockInboardUU = 1.2f;
	constexpr float CockPitch = -13.f;
	constexpr float CockYaw = 8.f;

	/** Thrust: out along the lens axis and slightly toward the crosshair. The lateral terms are small
	 *  ON PURPOSE — anything larger reads as the swipe this replaced. */
	constexpr float ThrustForwardUU = 16.0f;
	constexpr float ThrustInboardUU = -2.4f;
	constexpr float ThrustUpUU = 1.0f;
	constexpr float ThrustPitch = 6.f;
	constexpr float ThrustYaw = -5.f;

	/** Fraction of the post-cock time spent going OUT. The rest is the return. 0.25 => 1:3, fast:slow. */
	constexpr float ThrustOutFraction = 0.25f;

	/**
	 * THE STAB CURVE, and the only copy of it.
	 *
	 * @param Alpha       0..1 through the whole animation.
	 * @param CockEnd     where the cock beat ends, as a fraction of Alpha (the wind-up over the anim).
	 * @param OutOffset   relative translation for KnifeViewRoot, in rig space (+X out of the lens).
	 * @param OutRotation relative rotation. Roll is always zero — roll is what made the old motion a
	 *                    swipe, and its absence is half of what makes this one a stab.
	 *
	 * Shared by UpdateKnifeVisuals (which renders it) and Trace.Knife.StabProfile (which measures
	 * it). One derivation on purpose: the profile command's whole job is to say whether the motion is
	 * forward-dominant and fast-out/slow-return, and it could not answer that about a curve it had
	 * its own copy of.
	 */
	inline void ComputeStabPose(float Alpha, float CockEnd, FVector& OutOffset, FRotator& OutRotation)
	{
		// The two poses the motion interpolates between, named once so the three beats cannot
		// disagree about where "cocked" and "extended" are.
		const FVector CockOffset(-CockBackUU, CockInboardUU, CockUpUU);
		const FRotator CockRotation(CockPitch, CockYaw, 0.f);
		const FVector ThrustOffset(ThrustForwardUU, ThrustInboardUU, ThrustUpUU);
		const FRotator ThrustRotation(ThrustPitch, ThrustYaw, 0.f);

		Alpha = FMath::Clamp(Alpha, 0.f, 1.f);
		CockEnd = FMath::Clamp(CockEnd, 0.05f, 0.6f);

		if (Alpha < CockEnd)
		{
			const float T = FMath::Clamp(Alpha / CockEnd, 0.f, 1.f);
			const float Eased = T * T;                                   // ease in: loads the thrust
			OutOffset = FMath::Lerp(FVector::ZeroVector, CockOffset, Eased);
			OutRotation = FMath::Lerp(FRotator::ZeroRotator, CockRotation, Eased);
			return;
		}

		// Where we are inside the post-cock portion, 0..1.
		const float Post = FMath::Clamp((Alpha - CockEnd) / FMath::Max(0.01f, 1.f - CockEnd), 0.f, 1.f);
		const float OutEnd = FMath::Clamp(ThrustOutFraction, 0.05f, 0.9f);

		if (Post < OutEnd)
		{
			const float T = FMath::Clamp(Post / OutEnd, 0.f, 1.f);
			const float Eased = 1.f - FMath::Square(1.f - T);            // ease out: snap, then settle
			OutOffset = FMath::Lerp(CockOffset, ThrustOffset, Eased);
			OutRotation = FMath::Lerp(CockRotation, ThrustRotation, Eased);
			return;
		}

		const float T = FMath::Clamp((Post - OutEnd) / FMath::Max(0.01f, 1.f - OutEnd), 0.f, 1.f);
		const float Eased = T * T * (3.f - 2.f * T);                     // smoothstep home
		OutOffset = FMath::Lerp(ThrustOffset, FVector::ZeroVector, Eased);
		OutRotation = FMath::Lerp(ThrustRotation, FRotator::ZeroRotator, Eased);
	}
}

bool UTraceWeaponComponent::IsViewModelHandPart(const UStaticMeshComponent* Part)
{
	if (Part == nullptr)
	{
		return false;
	}

	// See the declaration: identified by NAME because ATraceCharacter's parts table is private to
	// another ownership slice and ViewModelRoot is the only public handle on the rig. The failure
	// mode if that table is renamed is the GUN staying visible next to the knife — ugly and
	// immediately obvious — rather than the hands vanishing, which would look like a render bug.
	const FString Name = Part->GetName();
	return Name.Contains(TEXT("Hand"))
		|| Name.Contains(TEXT("Knuckle"))
		|| Name.Contains(TEXT("Forearm"))
		|| Name.Contains(TEXT("Cuff"));
}

UStaticMeshComponent* UTraceWeaponComponent::AddKnifePart(USceneComponent* AttachTo, const TCHAR* DebugName,
	UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation, const FVector& Size,
	bool bNeon, bool bFirstPerson)
{
	AActor* OwnerActor = GetOwner();
	if (AttachTo == nullptr || Mesh == nullptr || OwnerActor == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(
		OwnerActor, MakeUniqueObjectName(OwnerActor, UStaticMeshComponent::StaticClass(), FName(DebugName)));
	if (Part == nullptr)
	{
		return nullptr;
	}

	Part->SetMobility(EComponentMobility::Movable);
	Part->SetupAttachment(AttachTo);
	Part->SetStaticMesh(Mesh);
	Part->SetRelativeLocationAndRotation(Location, Rotation);
	Part->SetRelativeScale3D(Size / TraceKnifeLayout::ShapeUnit);

	// CONTRACT §7, and it matters as much here as it does for the gun: the capsule is the ONLY
	// collider on this actor. Hitscan resolution, the trail trip test and the lag-compensation
	// history all reason purely about the capsule, and a colliding blade would break all three —
	// besides being a permanent obstacle welded to a player's face in first person.
	Part->SetCollisionProfileName(TEXT("NoCollision"));
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCanEverAffectNavigation(false);
	Part->bReceivesDecals = false;

	if (bFirstPerson)
	{
		// NOBODY ELSE MAY EVER SEE THIS, and no shadow of any kind, so there is no path by which a
		// floating knife appears in another player's frame. Set BEFORE RegisterComponent so the
		// scene proxy is created with it rather than being rebuilt.
		Part->SetOnlyOwnerSee(true);
		Part->SetCastShadow(false);
		Part->bCastHiddenShadow = false;
		Part->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;
	}
	else
	{
		// The third-person blade is the opposite: everyone EXCEPT its owner, who is looking down the
		// first-person rig instead and must not see a second knife sticking out of their own wrist.
		Part->SetOwnerNoSee(true);
		Part->SetCastShadow(true);
	}

	// --- Material -------------------------------------------------------------------------------
	//
	// Reuse the material the viewmodel gun is already wearing where one exists, so the knife is made
	// of the same stuff as the gun it replaced AND inherits ApplyTeamColors' pushes for free — the
	// MIDs are shared objects owned by the character. Where there is no gun to borrow from (a
	// simulated proxy, which never builds a viewmodel) fall back to the COMMITTED Tron parents at
	// /Game/Trace/Materials/Parents and finally to BasicShapeMaterial, exactly as everything else in
	// this project degrades.
	UMaterialInterface* Material = nullptr;
	if (const ATraceCharacter* Character = GetTraceCharacter())
	{
		// Ask the owner first. The child-walk below identifies the gun's lit channels by finding
		// "Neon" in a COMPONENT NAME, which only ever worked because the gun was a table of cubes
		// named after their function. The railgun's parts are named after their meshes, so on that
		// rig the search matches nothing and the knife drops all the way to BasicShapeMaterial —
		// visible as a knife that is not made of the same stuff as everything around it.
		Material = bNeon ? Character->GetViewModelNeonMID() : Character->GetViewModelBodyMID();
	}

	const ATraceCharacter* OwningCharacter = GetTraceCharacter();
	if (Material == nullptr && OwningCharacter != nullptr && OwningCharacter->ViewModelRoot != nullptr)
	{
		TArray<USceneComponent*> Children;
		OwningCharacter->ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);
		for (USceneComponent* Child : Children)
		{
			UStaticMeshComponent* GunPart = Cast<UStaticMeshComponent>(Child);
			if (GunPart == nullptr || KnifeViewParts.Contains(GunPart) || KnifeHandParts.Contains(GunPart))
			{
				continue;
			}
			// "Neon" in the name is how the CUBE gun's table marks its lit channels. Kept as a
			// second chance for a rig whose MIDs were never made (a simulated proxy builds no
			// viewmodel), not as the primary route.
			const bool bPartIsNeon = GunPart->GetName().Contains(TEXT("Neon"));
			if (bPartIsNeon == bNeon)
			{
				Material = GunPart->GetMaterial(0);
				break;
			}
		}
	}

	if (Material == nullptr)
	{
		// MAP_PLAN §9, the tenth and last load site (conflict #8: the other nine were migrated by the
		// FX tranche, this one was carved out for RESTRUCTURE C). It used to ask
		// /Game/Generated/Materials, which was generator output that is no longer even on disk —
		// Content/Generated/ was deleted in the same pass — so this fallback resolved to null and the
		// knife dropped straight to engine grey. The committed parents are the ONE path (bible §3.1).
		//
		// NO LEGACY SECOND ARM: the precedent that kept one (TraceCharacter.cpp) was written while
		// the legacy copy still existed, and a second LoadObject at a deleted path only buys a
		// warning per knife build. BasicShapeMaterial below remains the honest last resort.
		Material = LoadObject<UMaterialInterface>(nullptr, bNeon
			? TEXT("/Game/Trace/Materials/Parents/M_TraceNeon.M_TraceNeon")
			: TEXT("/Game/Trace/Materials/Parents/M_TraceSurface.M_TraceSurface"));
	}
	if (Material == nullptr)
	{
		Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}

	if (Material != nullptr)
	{
		if (UMaterialInstanceDynamic* MID = Part->CreateDynamicMaterialInstance(0, Material))
		{
			// Team-coloured, and the lit edge brighter than the body — the same read every neon
			// surface in this arena uses. Harmless on a material that has neither parameter.
			const ATraceCharacter* Character = GetTraceCharacter();
			const FLinearColor TeamColor = (Character != nullptr)
				? TraceTeamColor(Character->GetTeam())
				: FLinearColor(0.5f, 0.5f, 0.5f, 1.f);
			MID->SetVectorParameterValue(TEXT("Color"), bNeon ? TeamColor : (TeamColor * 0.16f));
			MID->SetScalarParameterValue(TEXT("Glow"), bNeon ? 2.6f : 0.f);
		}
	}

	Part->RegisterComponent();
	return Part;
}

void UTraceWeaponComponent::EnsureKnifeVisualsBuilt()
{
	if (bKnifeMeshUnavailable || (bKnifeViewBuilt && bKnifeHandBuilt))
	{
		return;
	}
	if (GetNetMode() == NM_DedicatedServer)
	{
		// Renders nothing, so there is nothing to build. Latch both so this stops being asked.
		bKnifeViewBuilt = true;
		bKnifeHandBuilt = true;
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	if (KnifeCubeMesh == nullptr)
	{
		KnifeCubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (KnifeCubeMesh == nullptr)
		{
			// /Engine/BasicShapes ships with every install, so this is close to impossible — but an
			// invisible knife is a far better failure than a crash, and it is the contract every
			// other optional asset in this project honours. Logged once.
			bKnifeMeshUnavailable = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("Knife visuals skipped: /Engine/BasicShapes/Cube did not resolve. The weapon still works."));
			return;
		}
	}

	// --- First person ---------------------------------------------------------------------------
	//
	// Hung under ViewModelRoot, so it inherits the sway, the walk bob and the recoil settle the gun
	// already gets — for free, and without ATraceCharacter having to know the knife exists. The
	// intermediate KnifeViewRoot is what the swing animation writes to, so the swing composes with
	// the sway instead of fighting it.
	if (!bKnifeViewBuilt && Character->IsLocallyControlled() && Character->ViewModelRoot != nullptr)
	{
		KnifeViewRoot = NewObject<USceneComponent>(Character,
			MakeUniqueObjectName(Character, USceneComponent::StaticClass(), FName(TEXT("KnifeViewRoot"))));
		if (KnifeViewRoot != nullptr)
		{
			KnifeViewRoot->SetMobility(EComponentMobility::Movable);
			KnifeViewRoot->SetupAttachment(Character->ViewModelRoot);
			KnifeViewRoot->RegisterComponent();

			for (const TraceKnifeLayout::FKnifePart& Spec : TraceKnifeLayout::FirstPersonParts)
			{
				if (UStaticMeshComponent* Part = AddKnifePart(KnifeViewRoot, Spec.Name, KnifeCubeMesh,
					Spec.Location, Spec.Rotation, Spec.Size, Spec.bNeon, /*bFirstPerson=*/true))
				{
					Part->SetVisibility(false);
					KnifeViewParts.Add(Part);
				}
			}

			bKnifeViewBuilt = true;
			UE_LOG(LogTraceGame, Verbose, TEXT("%s built a first-person knife (%d parts)."),
				*GetNameSafe(Character), KnifeViewParts.Num());
		}
	}

	// --- Third person ---------------------------------------------------------------------------
	//
	// TWO THINGS THE HAND BONE CAN BE, AND ONE OF THEM IS NOTHING.
	//
	// The name is RESOLVED rather than assumed: "hand_r" is Epic's Mannequin's spelling and a pawn's
	// body now depends on which character is playing it — Rocco's rig calls the same joint
	// "RightHand1". ATraceCharacter::ResolveBodyBoneName asks the mesh that is actually on the pawn.
	//
	// A None answer is still the whole guard, and it is doing more work than it looks: character art
	// is imported per developer and is legitimately absent on a fresh clone, in which case the
	// skeletal mesh component exists but has no skeleton — attaching to a bone that does not exist
	// would silently attach at the component ORIGIN and leave a knife lying at the pawn's feet.
	if (!bKnifeHandBuilt)
	{
		USkeletalMeshComponent* BodyMesh = Character->GetMesh();
		const FName HandBoneName = Character->ResolveBodyBoneName(TEXT("hand_r"));
		if (BodyMesh != nullptr && !HandBoneName.IsNone())
		{
			KnifeHandRoot = NewObject<USceneComponent>(Character,
				MakeUniqueObjectName(Character, USceneComponent::StaticClass(), FName(TEXT("KnifeHandRoot"))));
			if (KnifeHandRoot != nullptr)
			{
				KnifeHandRoot->SetMobility(EComponentMobility::Movable);
				KnifeHandRoot->SetupAttachment(BodyMesh, HandBoneName);
				KnifeHandRoot->RegisterComponent();
				KnifeHandRoot->SetRelativeLocationAndRotation(
					TraceKnifeLayout::HandOffset, TraceKnifeLayout::HandRotation);

				for (const TraceKnifeLayout::FKnifePart& Spec : TraceKnifeLayout::HandParts)
				{
					if (UStaticMeshComponent* Part = AddKnifePart(KnifeHandRoot, Spec.Name, KnifeCubeMesh,
						Spec.Location, Spec.Rotation, Spec.Size, Spec.bNeon, /*bFirstPerson=*/false))
					{
						Part->SetVisibility(false);
						KnifeHandParts.Add(Part);
					}
				}

				bKnifeHandBuilt = true;
				UE_LOG(LogTraceGame, Verbose, TEXT("%s built a third-person knife (%d parts) on bone '%s'."),
					*GetNameSafe(Character), KnifeHandParts.Num(), *HandBoneName.ToString());
			}
		}
	}
}

#if !UE_BUILD_SHIPPING
/**
 * Restores the EXACT pre-v12 SetGunViewModelHidden — the early-return latch that caused "the knife
 * and gun can be held at the same time".
 *
 * A TEST HOOK, defaulting to the fix being on, and it exists for the same reason
 * Trace.Knife.CarrierImmune does: a harness that has never gone RED is not evidence. With this at 1,
 * Trace.Knife.DualWeaponTest reproduces the user's bug on demand, on the shipped build, and prints
 * the exact census that proves it. Never ship 1.
 */
static TAutoConsoleVariable<int32> CVarKnifeLegacyGunHideLatch(
	TEXT("Trace.Knife.LegacyGunHideLatch"), 0,
	TEXT("Dev only. 1 restores the pre-v12 latched gun-hiding, i.e. PUTS THE DUAL-WEAPON BUG BACK, so "
	     "Trace.Knife.DualWeaponTest can be shown failing on a working build. Never ship 1."),
	ECVF_Cheat);
#endif

void UTraceWeaponComponent::RefreshGunPartCache(const ATraceCharacter& Character)
{
	// ViewModelRoot's direct children are the gun's parts, the hands/forearms, and KnifeViewRoot
	// (a USceneComponent, so the cast drops it). ATraceCharacter builds its rig ONCE, lazily, the
	// first frame the pawn turns out to be locally controlled — which is usually AFTER this component
	// has started ticking — so the cache is keyed on the child count and rebuilds itself when the rig
	// appears underneath it. Without that, an empty cache taken on frame one would be permanent and
	// the gun would never be hidden at all.
	const int32 ChildCount = Character.ViewModelRoot->GetNumChildrenComponents();
	if (ChildCount == CachedViewModelChildCount && CachedGunParts.Num() > 0)
	{
		return;
	}
	CachedViewModelChildCount = ChildCount;
	CachedGunParts.Reset();

	TArray<USceneComponent*> Children;
	Character.ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);

	for (USceneComponent* Child : Children)
	{
		UStaticMeshComponent* Part = Cast<UStaticMeshComponent>(Child);
		if (Part == nullptr || KnifeViewParts.Contains(Part) || KnifeHandParts.Contains(Part))
		{
			continue;
		}
		if (IsViewModelHandPart(Part))
		{
			// The hands and forearms stay with either weapon. A knife held by nothing is a worse
			// read than no knife, so they are not in this list and are never written to.
			continue;
		}
		CachedGunParts.Add(Part);
	}
}

void UTraceWeaponComponent::SetGunViewModelHidden(bool bHidden)
{
	// ===============================================================================================
	// SPEC v12 §7 — "There's a bug where the knife and gun can be held at the same time."
	//
	// DIAGNOSED, REPRODUCED AND FIXED HERE. It was never a state bug: EquippedWeapon is a single
	// replicated selector and cannot hold both. It was this function, and it had two defects that
	// compounded.
	//
	// DEFECT 1 — THE LATCH. This used to open with `if (bGunViewModelHidden == bHidden) return;`,
	// which made it a one-shot edge trigger. But it is not the only writer of those components'
	// visibility: ATraceCharacter::SetViewModelVisible(true) sets EVERY part of ViewModelParts
	// visible, gun included, and it runs whenever the rig comes back — the carry blend returning to
	// first person, or a respawn. At that moment bGunViewModelHidden was still true, so this
	// function's next call early-returned and the gun stayed on screen NEXT TO THE KNIFE. That is
	// the user's bug, exactly, and the comment three lines up in UpdateKnifeVisuals claimed the code
	// was "re-asserting rather than latching" while this function did the opposite. Reproduction:
	// equip the knife, go third person and come back (Trace.ForceThirdPerson 1 then 0, which is the
	// same SetViewModelVisible path the Core carry uses), and both weapons are drawn. Measured, in
	// exactly that order, by Trace.Knife.DualWeaponTest.
	//
	// DEFECT 2 — RE-SHOWING A GUN THAT SHOULD BE GONE. The old body wrote SetVisibility(!bHidden)
	// unconditionally, so on the frame a knife-holding player DIED, UpdateKnifeVisuals' call with
	// bHidden=false put the gun back on screen over the death camera — after ATraceCharacter had just
	// hidden the whole rig for the corpse. Two owners, opposite intentions, last writer wins.
	//
	// THE RULE THAT REPLACES BOTH, and it is the only rule this function has:
	//
	//     a gun part is visible  <=>  the character wants the rig on screen  AND  no knife is out.
	//
	// The character's half is asked for, every time, rather than remembered — IsViewModelVisible() is
	// its own settled answer, folding in the carry blend, the corpse and the respawn. So this can
	// only ever REMOVE the gun from a rig the character is showing; it can never resurrect one the
	// character has hidden. Re-asserted every tick from UpdateKnifeVisuals, from a cached part list so
	// there is no per-frame allocation, and SetVisibility itself is a no-op when nothing changed.
	// ===============================================================================================

#if !UE_BUILD_SHIPPING
	// THE RED ARM. Byte for byte the behaviour described above as DEFECT 1 and DEFECT 2: latch on the
	// requested state, then write the raw !bHidden with no regard for what the character wants. See
	// CVarKnifeLegacyGunHideLatch.
	if (CVarKnifeLegacyGunHideLatch.GetValueOnAnyThread() != 0)
	{
		if (bGunViewModelHidden == bHidden)
		{
			return;
		}
		ATraceCharacter* LegacyCharacter = GetTraceCharacter();
		if (LegacyCharacter == nullptr || LegacyCharacter->ViewModelRoot == nullptr)
		{
			return;
		}
		bGunViewModelHidden = bHidden;
		RefreshGunPartCache(*LegacyCharacter);
		for (const TObjectPtr<UStaticMeshComponent>& Part : CachedGunParts)
		{
			if (Part != nullptr)
			{
				Part->SetVisibility(!bHidden);
			}
		}
		return;
	}
#endif

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || Character->ViewModelRoot == nullptr)
	{
		return;
	}

	bGunViewModelHidden = bHidden;

	RefreshGunPartCache(*Character);

	const bool bWantGunParts = Character->IsViewModelVisible() && !bHidden;

	for (const TObjectPtr<UStaticMeshComponent>& Part : CachedGunParts)
	{
		if (Part != nullptr)
		{
			Part->SetVisibility(bWantGunParts);
		}
	}
}

void UTraceWeaponComponent::NotifyBodyMeshChanged()
{
	if (!bKnifeHandBuilt)
	{
		// Nothing is attached to the old body, so there is nothing to move. The build that has not
		// happened yet will resolve the bone against the NEW mesh when it does.
		return;
	}

	// DESTROYED RATHER THAN RE-ATTACHED. Re-pointing the root would be fewer allocations, but the
	// parts under it carry per-rig offsets and a rebuild is one frame's work on a spawn or a character
	// switch — neither of which happens in a hot loop. Correct beats clever here.
	for (UStaticMeshComponent* Part : KnifeHandParts)
	{
		if (Part != nullptr)
		{
			Part->DestroyComponent();
		}
	}
	KnifeHandParts.Reset();

	if (KnifeHandRoot != nullptr)
	{
		KnifeHandRoot->DestroyComponent();
		KnifeHandRoot = nullptr;
	}

	// The latch, cleared: EnsureKnifeVisualsBuilt() runs from UpdateKnifeVisuals() every tick and
	// early-outs only when BOTH rigs are built, so the third-person knife comes back on the next
	// frame, attached to whatever the new body calls its right hand.
	bKnifeHandBuilt = false;
}

void UTraceWeaponComponent::UpdateKnifeVisuals(float /*DeltaTime*/)
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	EnsureKnifeVisualsBuilt();

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	const bool bKnife = IsKnifeEquipped();
	const bool bAlive = Character->IsAlive();

	// =============================================================================================
	// [DUALWIELD] SPEC v28 §10 — "Gun in one hand, knife in the other."
	// =============================================================================================
	//
	// TWO LINES CARRY THE WHOLE PRESENTATION CHANGE, and everything below reads them instead of
	// reading the selector, so flipping the switch restores the v12 §7 one-weapon-at-a-time rule with
	// nothing to put back:
	//
	//   bBladeVisible   the knife is on screen whenever the pawn is ALIVE, rather than whenever the
	//                   knife is the selected weapon. Under the switch it is always in the off hand.
	//   bHideGun        the gun is hidden whenever the selector is on the knife. This is the ONE
	//                   thing that would otherwise still make dual-wield impossible to see:
	//                   SetGunViewModelHidden is re-asserted every tick by design (that re-assert IS
	//                   the v12 §7 fix), so a single stale `true` here would erase the gun sixty
	//                   times a second no matter what anything else did.
	//
	// *** SPEC v29 §5: THE `!bDualWield` QUALIFIER IS GONE. *** It read "the gun is hidden only in
	// the legacy build", which was right while the selector could never hold Knife under dual-wield.
	// Now the 1 key puts it there deliberately and the state MUST be visible — a player who stows
	// their guns and still sees a pistol on screen has no way to tell whether the press registered,
	// and the +22% they are being paid is invisible. bKnife is now the whole test in both switch
	// positions, which also collapses the two arms of this line into one rule.
	const bool bDualWield = TraceMelee::IsDualWieldEnabled();
	const bool bBladeVisible = bDualWield ? bAlive : (bKnife && bAlive);
	const bool bHideGun = bKnife && bAlive;

	// WHERE THE BLADE RESTS. Under the switch it hangs in ATraceCharacter's off hand (asked for, never
	// copied — see GetViewModelOffHand); in the legacy build it sits at the rig root, exactly where
	// the gun it replaced was, which is what the parts table is authored around. The stab animation
	// below is a DELTA onto whichever of the two this is, so one motion curve serves both poses.
	FVector KnifeRestLocation = FVector::ZeroVector;
	FRotator KnifeRestRotation = FRotator::ZeroRotator;
	if (bDualWield)
	{
		FVector OffHand = FVector::ZeroVector;
		if (Character->GetViewModelOffHand(OffHand))
		{
			KnifeRestLocation = OffHand + TraceKnifeLayout::OffHandOffset;
			KnifeRestRotation = TraceKnifeLayout::OffHandRotation;
		}
		// else: the rig has not been built yet (or the hand is not free, which cannot happen while
		// the switch is on). Falling through with a zero rest is the same "not ready" state the rig
		// itself is in for those frames, and the next tick corrects it — the knife is not visible
		// then either, because bWantView also requires IsViewModelVisible().
	}

	// --- First person ---------------------------------------------------------------------------
	//
	// ATraceCharacter::SetViewModelVisible drives its OWN parts list and never touches ours, so the
	// two rigs' visibility is decided independently and re-asserted every tick. Re-asserting rather
	// than latching is what makes this robust against the character re-showing the gun on a state
	// change it owns (a respawn, or handing the Core away), which is a path this component cannot
	// hook.
	if (bKnifeViewBuilt)
	{
		// IsViewModelVisible() is the character's own settled answer to "is the first-person rig on
		// screen", which folds in the third-person carry blend and the corpse hiding. Asking it is
		// what keeps the knife from appearing over the carrier's shoulder camera.
		const bool bWantView = bBladeVisible && Character->IsLocallyControlled() && Character->IsViewModelVisible();

		if (bWantView != bKnifeViewVisible)
		{
			bKnifeViewVisible = bWantView;
			for (UStaticMeshComponent* Part : KnifeViewParts)
			{
				if (Part != nullptr)
				{
					Part->SetVisibility(bWantView);
				}
			}
		}

		// The gun goes away exactly when the knife comes out, and comes back when it does not.
		//
		// UNCONDITIONAL, EVERY TICK, AND THAT IS THE FIX FOR SPEC v12 §7. ATraceCharacter is the
		// other writer of these components and it re-shows the whole rig on its own schedule (the
		// carry blend, a respawn); an edge-triggered call would miss that and leave a gun on screen
		// beside the knife. See SetGunViewModelHidden, which now states the rule instead of
		// remembering an edge.
		SetGunViewModelHidden(bHideGun);

		if (bWantView && KnifeViewRoot != nullptr)
		{
			// --- The stab, as a transform  (spec v12 §2) ----------------------------------------
			//
			// THIS IS THE SWINGER'S ENTIRE READ now that the 3D slash line has been taken out of
			// their view, and it is PROCEDURAL: there is no stab animation in the imported Mannequin
			// set (Death, Jump, Pistol, Rifle, Unarmed locomotion — nothing melee), so the motion is
			// this arithmetic and nothing else. See TraceKnifeLayout for the constants and for the
			// depth check that keeps the extended blade inside the capsule's clearance.
			//
			// THREE BEATS, and the middle one is the request:
			//   COCK    0 .. windup. Blade draws back and the tip lifts. Eased IN (T^2), so it
			//           accelerates into the thrust instead of arriving at the cocked pose and
			//           stopping. This is also the window the blade resolves in.
			//   THRUST  a quarter of what is left. Eased OUT, so it snaps to full extension and
			//           decelerates there — a punch, not a slide.
			//   RETURN  the remaining three quarters, smoothstepped home. FAST OUT, SLOWER RETURN is
			//           what §2 asked for, and 1:3 is the ratio that reads as a recovery rather than
			//           as a rewind.
			//
			// Written to KnifeViewRoot, a CHILD of ViewModelRoot, so it composes with the sway and
			// bob the character is writing to the parent rather than overwriting them. And nothing
			// in TraceMelee::ResolveSwing reads any of it — the arc that was actually cut is pure
			// arithmetic on the aim ray, so the blade may lag and overshoot as much as it likes. That
			// separation is why the animation could be replaced outright without touching one line of
			// hit resolution.
			const double Elapsed = GetLocalTimeSeconds() - SwingAnimStartLocalTime;
			const double AnimLength = static_cast<double>(TraceMelee::GetSwingAnimSeconds());

			FVector Offset = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;

			if (Elapsed >= 0.0 && Elapsed <= AnimLength)
			{
				// ONE derivation, shared with Trace.Knife.StabProfile — which walks this same curve at
				// 1 ms and reports whether the motion is actually a thrust. A test that re-derived the
				// curve would be measuring its own copy, which is how "the animation is a stab" becomes
				// a claim instead of a number.
				TraceKnifeLayout::ComputeStabPose(
					static_cast<float>(Elapsed / FMath::Max(0.01, AnimLength)),
					FMath::Clamp(TraceMelee::GetSwingWindupSeconds()
						/ FMath::Max(0.01f, TraceMelee::GetSwingAnimSeconds()), 0.05f, 0.6f),
					Offset, Rotation);
			}

			// [DUALWIELD] The stab is a DELTA onto the rest pose rather than an absolute transform, so
			// the identical curve reads correctly from the off hand and from the gun's old slot. The
			// rotations COMPOSE (FRotator addition is not rotation composition in general, but both
			// terms here are small and the rest cant is fixed, so the sum is what the eye expects and
			// what the v27 build already did with a zero rest).
			const FTransform KnifeRigPose(KnifeRestRotation + Rotation, KnifeRestLocation + Offset);

			// =====================================================================================
			// *** AND THEN THE BLADE RIDES THE HAND, WHICH UNTIL v32 §8 IT DID NOT.  (spec v32 §8) ***
			// =====================================================================================
			//
			// THE DEFECT, PHOTOGRAPHED: Saved/Screenshots/v31integ_47_key3_knife_idle.png has the
			// glove in the middle of the frame and the balisong a clear hand's width to the RIGHT of
			// it with lit floor visible in between; v31fallback_47_key3_knife_idle.png, the same beat
			// of the same scripted walk on the cube rig, has the blade coming out of the closed fist.
			// The same split is in _48_knife_stab and _50_knife_inspect_late.
			//
			// THE CAUSE IS THE LINE THIS ONE REPLACES, AND IT IS AN OMISSION RATHER THAN A WRONG
			// NUMBER. Every other primitive drawn at the hand — both gun rigs, both forearm tubes —
			// is written as `authored rig pose * ATraceCharacter's wrist delta`, because the pack
			// rig's wrist MOVES: the base pose all this art is authored against is Idle_Pistol at
			// t=0, and Idle_Knife is a different clip with a differently canted wrist. The knife rig
			// alone was written straight to KnifeViewRoot with no delta, so on the pack rig the fist
			// walks off to the knife pose and the blade stays parked in the pistol pose.
			//
			// MEASURED, off Saved/Logs/sizefix.log's own [Hands] probe lines: wrist_right sits at rig
			// (-6.88, -0.17, -1.53) in the base pose and at (-6.06, -0.43, -0.57) at Idle_Knife
			// t=0.32 — 1.3 uu of travel, plus the wrist ROTATION the probe does not print, applied
			// about a wrist that is 7.8 uu from the blade's grip. At the viewmodel's ~32 px/uu that
			// is the ~100 px of daylight the frames show.
			//
			// IT IS ALSO WHY THE CUBE FALLBACK LOOKS RIGHT AND THE PACK RIG DOES NOT, which had been
			// blamed on the pack art: with no skeleton there is no wrist to move, the delta is
			// identity by construction, and this multiply writes byte-for-byte what shipped before.
			// So the fallback frames are the RED ARM for this change — they must not move at all.
			//
			// READ LIVE rather than off a cached member: GetViewModelWeaponDelta() re-reads the
			// socket, because the character fills its cached copy on the ACTOR tick and this is a
			// COMPONENT tick with no ordering guarantee against it. BuildPackHandsViewModel gives
			// this component the same tick prerequisite on the hands mesh that the actor has, so the
			// pose being read is this frame's.
			//
			// THE OFF-HAND ARM TAKES THE OTHER WRIST, and it has to: the two wrists are not rigid
			// with each other (the character's HandsOffWristRestRig comment measures 20+ uu of
			// divergence through a reload), so a blade in the left hand carried on the right wrist's
			// delta is left behind on every reload. Note that with the shipped
			// `bDualWieldKnife=False` this arm is not reached at all — the rest pose is the rig root
			// and the blade is in the right hand, which is also what the pack's own
			// unreal-hands_hands_stats.json authors ("knife": right = handle, left = open, free).
			const FTransform Posed = KnifeRigPose *
				(bDualWield ? Character->GetViewModelOffHandDelta() : Character->GetViewModelWeaponDelta());

			KnifeViewRoot->SetRelativeLocationAndRotation(Posed.GetLocation(), Posed.GetRotation());
		}
	}

	// --- Third person ---------------------------------------------------------------------------
	//
	// No animation here on purpose. The imported Mannequin set has no melee sequence (the same
	// absence that forced UpdateCrouchPresentation to pose the slide by hand), so the arm will not
	// swing — ATraceMeleeArc draws the cut instead, which is the information a victim actually needs.
	// What this rig carries is the STATE: a visible blade means that player is 22% faster and cannot
	// shoot, and that is worth reading across the arena.
	//
	// THERE IS NO THIRD-PERSON GUN MESH ANYWHERE IN THIS PROJECT, which is why spec v12 §7's third
	// suspect ("the third-person attachment showing both to other players") could be ruled out rather
	// than fixed: ATraceCharacter builds a first-person viewmodel and nothing else, so the only weapon
	// another player can ever see on your body is this knife. The dual-weapon bug was first-person
	// only, and Trace.Knife.DualWeaponTest reports both censuses so that stays checkable rather than
	// remembered.
	if (bKnifeHandBuilt)
	{
		// [DUALWIELD] Third person: the blade is on the body whenever the pawn is alive. Note what
		// this tell now MEANS has changed and that is honest rather than a regression — in v27 a
		// visible blade said "that player is 22% faster and cannot shoot back", and under the switch
		// nobody is either of those things, so it says only "that player has a knife", which is true
		// of everybody. There is still no third-person GUN mesh anywhere in this project, so this
		// remains the whole of what another player can see in your hands.
		const bool bWantHand = bBladeVisible;
		if (bWantHand != bKnifeHandVisible)
		{
			bKnifeHandVisible = bWantHand;
			for (UStaticMeshComponent* Part : KnifeHandParts)
			{
				if (Part != nullptr)
				{
					Part->SetVisibility(bWantHand);
				}
			}
		}
	}
}

#if !UE_BUILD_SHIPPING

void UTraceWeaponComponent::GetViewModelCensus(int32& OutGunVisible, int32& OutKnifeVisible,
	int32& OutHandVisible, int32& OutBodyKnife) const
{
	OutGunVisible = 0;
	OutKnifeVisible = 0;
	OutHandVisible = 0;
	OutBodyKnife = 0;

	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	// The knife rigs first, from our own lists.
	for (const TObjectPtr<UStaticMeshComponent>& Part : KnifeViewParts)
	{
		if (Part != nullptr && Part->IsVisible())
		{
			++OutKnifeVisible;
		}
	}

	// *** AND THE PACK BLADE, WHICH IS IN NONE OF THIS COMPONENT'S LISTS. ***
	//
	// Spec v31 §5 replaced the six-cube first-person blade with SK_TraceKnife, built by
	// UTraceKnifeViewSubsystem and attached to the HANDS MESH'S `wrist_right` BONE — not to
	// ViewModelRoot, and not into KnifeViewParts. Both loops above therefore walk right past it, and
	// from the moment §5 hid the cubes this census answered "0 first-person knife parts" for a pawn
	// visibly holding a knife.
	//
	// THAT MATTERS BECAUSE OF WHAT READS IT. Trace.Knife.DualWeaponTest guards spec v12 §7 — "never
	// draw a gun part and a knife part together" — with an overlap of min(gun, knife), so a knife
	// count stuck at zero makes the overlap identically zero and the harness unfalsifiable. It duly
	// reported "0 census points with both weapons drawn" in BOTH arms and then, to its credit,
	// refused to call that a pass: "*** NOT PROVEN *** — the RED arm did not reproduce the bug".
	// Restoring the count is the half of that repair this file owns.
	//
	// Asked through TraceKnifeView::VisibleBladeParts rather than by reaching for the component,
	// because the blade's existence, ownership and visibility are all §5's to know and this file has
	// no member for any of them. It returns 0 on every fallback path, so a checkout with no
	// `git lfs pull` counts the cubes exactly as it did before.
	OutKnifeVisible += TraceKnifeView::VisibleBladeParts(Character);
	for (const TObjectPtr<UStaticMeshComponent>& Part : KnifeHandParts)
	{
		if (Part != nullptr && Part->IsVisible())
		{
			++OutBodyKnife;
		}
	}

	// The gun, walked live rather than through CachedGunParts. A census that trusted the same cache
	// the fix writes through could agree with a broken fix; this asks the rig itself.
	if (Character->ViewModelRoot == nullptr)
	{
		return;
	}
	TArray<USceneComponent*> Children;
	Character->ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);
	for (USceneComponent* Child : Children)
	{
		const UStaticMeshComponent* Part = Cast<UStaticMeshComponent>(Child);
		if (Part == nullptr || KnifeViewParts.Contains(Part) || !Part->IsVisible())
		{
			continue;
		}
		if (IsViewModelHandPart(Part))
		{
			++OutHandVisible;
		}
		else
		{
			++OutGunVisible;
		}
	}
}

// =================================================================================================
// Trace.Knife.StabProfile — the unattended proof for spec v12 §2.
//
// Verbatim: "Take the 3d 'swipe' line animation of the knife out of the player's view. Can you make
// the knife animation a stab instead of a swipe?"
//
// "IT LOOKS LIKE A STAB NOW" IS NOT A MEASUREMENT, and this project has been burned by exactly that
// class of claim. A stab and a swipe are distinguishable as numbers, so this walks the shipped curve
// — TraceKnifeLayout::ComputeStabPose, the same function the viewmodel renders from, not a copy — at
// 1 ms over the whole animation and reports four things, each with a threshold:
//
//   FORWARD DOMINANCE   peak |+X| against peak |Y|. A thrust travels down the lens; a swipe travels
//                       across it. The old motion's peaks were 9.0 forward against 7.0 lateral, i.e.
//                       barely forward-biased at all, on top of a 58 degree yaw sweep. Requires the
//                       forward peak to be at least 3x the lateral one.
//   ROLL                must be exactly 0. Roll is what reads as a slash; the old motion rolled 46
//                       degrees through the strike.
//   YAW SWEEP           total yaw excursion. The old motion swept 34 -> -58, i.e. 92 degrees across
//                       the screen. Requires under 25.
//   OUT vs RETURN       time from the cocked pose to full extension, against the time from full
//                       extension back to rest. §2 asks for fast out and slower return, so the
//                       return must be strictly longer than the out-stroke.
//
// It also prints the extended blade's depth so the framing arithmetic in TraceKnifeLayout stays
// honest: the tip must stay well short of the 76 uu the gun's muzzle needed to clear the capsule.
//
// World-free and instant, so it runs anywhere — including in the same -ExecCmds list as the angle
// test, before a pawn exists.
// =================================================================================================

int32 TraceRunStabProfile()
{
	int32 Failures = 0;

	const float AnimSeconds = TraceMelee::GetSwingAnimSeconds();
	const float CockEnd = FMath::Clamp(
		TraceMelee::GetSwingWindupSeconds() / FMath::Max(0.01f, AnimSeconds), 0.05f, 0.6f);

	constexpr int32 Samples = 1000;                     // 1 ms at a 0.32 s animation, near enough
	float PeakForward = 0.f;
	float PeakBackward = 0.f;
	float PeakLateral = 0.f;
	float PeakRoll = 0.f;
	float MinYaw = 0.f;
	float MaxYaw = 0.f;
	float ForwardPeakAlpha = 0.f;

	for (int32 Index = 0; Index <= Samples; ++Index)
	{
		const float Alpha = static_cast<float>(Index) / static_cast<float>(Samples);

		FVector Offset = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		TraceKnifeLayout::ComputeStabPose(Alpha, CockEnd, Offset, Rotation);

		if (static_cast<float>(Offset.X) > PeakForward)
		{
			PeakForward = static_cast<float>(Offset.X);
			ForwardPeakAlpha = Alpha;
		}
		PeakBackward = FMath::Max(PeakBackward, -static_cast<float>(Offset.X));
		PeakLateral = FMath::Max(PeakLateral, FMath::Abs(static_cast<float>(Offset.Y)));
		PeakRoll = FMath::Max(PeakRoll, FMath::Abs(static_cast<float>(Rotation.Roll)));
		MinYaw = FMath::Min(MinYaw, static_cast<float>(Rotation.Yaw));
		MaxYaw = FMath::Max(MaxYaw, static_cast<float>(Rotation.Yaw));
	}

	// The two stroke durations, in seconds, from the curve's own structure.
	const float CockSeconds = CockEnd * AnimSeconds;
	const float OutSeconds = ForwardPeakAlpha * AnimSeconds - CockSeconds;
	const float ReturnSeconds = AnimSeconds - ForwardPeakAlpha * AnimSeconds;
	const float YawSweep = MaxYaw - MinYaw;

	UE_LOG(LogTraceGame, Display, TEXT("========== TRACE KNIFE STAB PROFILE (spec v12 s2) =========="));
	UE_LOG(LogTraceGame, Display,
		TEXT("[StabProfile] PROCEDURAL viewmodel motion — the Mannequin set ships no melee sequence, so "
		     "there is no authored stab to play and this curve IS the animation."));
	UE_LOG(LogTraceGame, Display,
		TEXT("[StabProfile] anim %.3fs = cock %.3fs + out %.3fs + return %.3fs (full extension at alpha %.3f)"),
		AnimSeconds, CockSeconds, OutSeconds, ReturnSeconds, ForwardPeakAlpha);
	UE_LOG(LogTraceGame, Display,
		TEXT("[StabProfile] travel: forward +%.2fuu, back -%.2fuu, lateral %.2fuu | roll %.2fdeg | yaw sweep %.2fdeg"),
		PeakForward, PeakBackward, PeakLateral, PeakRoll, YawSweep);

	auto Check = [&Failures](bool bCondition, const TCHAR* What, const FString& Detail)
	{
		if (bCondition)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[StabProfile] PASS  %-28s %s"), What, *Detail);
		}
		else
		{
			++Failures;
			UE_LOG(LogTraceGame, Error, TEXT("[StabProfile] FAIL  %-28s %s"), What, *Detail);
		}
	};

	Check(PeakForward >= 3.f * PeakLateral, TEXT("forward-dominant"),
		FString::Printf(TEXT("forward %.2fuu vs lateral %.2fuu (needs >= 3x — a swipe is lateral-dominant)"),
			PeakForward, PeakLateral));

	Check(FMath::IsNearlyZero(PeakRoll, 0.01f), TEXT("no roll"),
		FString::Printf(TEXT("peak roll %.3fdeg (a slash rolls; the motion this replaced rolled 46)"), PeakRoll));

	Check(YawSweep < 25.f, TEXT("no lateral sweep"),
		FString::Printf(TEXT("yaw excursion %.2fdeg (needs < 25; the motion this replaced swept 92)"), YawSweep));

	Check(ReturnSeconds > OutSeconds, TEXT("fast out, slower return"),
		FString::Printf(TEXT("out %.3fs vs return %.3fs (spec v12 s2 asks for a fast thrust and a slower recovery)"),
			OutSeconds, ReturnSeconds));

	// Depth. The blade tip's rest position is x = 21.4 + 3.6/2 = 23.2 uu in rig space.
	constexpr float BladeTipRestUU = 23.2f;
	constexpr float MuzzleClearanceUU = 76.f;
	const float ExtendedTip = BladeTipRestUU + PeakForward;
	Check(ExtendedTip < MuzzleClearanceUU, TEXT("blade stays inside clearance"),
		FString::Printf(TEXT("extended tip at %.1fuu vs the gun muzzle's %.0fuu, which is the depth that had to clear the capsule"),
			ExtendedTip, MuzzleClearanceUU));

	if (Failures == 0)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[StabProfile] 0 failures — the motion is a thrust, not a swipe."));
	}
	else
	{
		UE_LOG(LogTraceGame, Error, TEXT("[StabProfile] %d failure(s)."), Failures);
	}
	UE_LOG(LogTraceGame, Display, TEXT("============================================================"));

	return Failures;
}

static FAutoConsoleCommand GTraceStabProfileCmd(
	TEXT("Trace.Knife.StabProfile"),
	TEXT("Dev only. Spec v12 s2. Walks the shipped first-person knife motion at 1ms and reports whether "
	     "it is a forward thrust (fast out, slower return, no roll, no lateral sweep) rather than a swipe."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceRunStabProfile(); }));

// =================================================================================================
// Trace.Knife.DualWeaponTest — the unattended proof for spec v12 §7.
//
// Verbatim: "There's a bug where the knife and gun can be held at the same time."
//
// WHY A HARNESS AND NOT A SCREENSHOT. The two rigs are OnlyOwnerSee first-person primitives, so the
// only camera that can photograph them is the local player's, and a -RenderOffScreen run has no
// window to photograph. More importantly a screenshot cannot be made to go RED on demand — and the
// whole reason this file exists is that a fix nobody watched fail is not a fix. So this counts the
// components that are actually VISIBLE at each step, through UTraceWeaponComponent::
// GetViewModelCensus, which reads the components' own flags rather than either rig's opinion.
//
// TWO ARMS, RED FIRST, exactly as Trace.Knife.CarrierImmunityTest does it. Arm 0 restores the old
// latched SetGunViewModelHidden via Trace.Knife.LegacyGunHideLatch and MUST reproduce the bug; arm 1
// is the shipped code and must be clean. If red does not go red the verdict is NOT PROVEN, because a
// green arm on its own only demonstrates that the harness can print zero.
//
// THE REPRODUCTION, and it is the shipping path rather than a poke at the members:
//
//   1  swap to the knife and wait out the 0.2 s pullout   -> expect gun 0, knife >0
//   2  Trace.ForceThirdPerson 1, wait for the view blend  -> ATraceCharacter hides the whole rig,
//                                                            exactly as picking up the Core does
//   3  Trace.ForceThirdPerson 0, wait for the view blend  -> ATraceCharacter SHOWS the whole rig
//                                                            again, gun parts included
//   4  census                                             -> THE BUG: gun >0 AND knife >0
//   5  swap to the gun and back to the knife four times, half of them sampled INSIDE the 0.2 s
//      pullout — spec v12 §7's second suspect was a race in exactly that window
//
// Step 2/3 is not a contrivance: Trace.ForceThirdPerson feeds ATraceCharacter::WantsFirstPersonView,
// which is the SAME input the Core carry uses, and UpdateViewBlend's SetViewModelVisible call is the
// same line either way. Carrying the Core is simply the version of this that needs a Core, a pickup
// and a live match; the cvar is the version an unattended run can drive in four seconds.
//
// AS MEASURED — see the pass report for the full logs. On a listen server the RED arm reports the
// gun and the knife drawn together the instant the camera comes back to first person, and the GREEN
// arm reports zero across every census point. Both numbers are logged at Display on purpose.
// =================================================================================================

namespace TraceDualWeaponTest
{
	/**
	 * The pawn the local player is looking out of.
	 *
	 * Its own copy rather than TraceRecoilTest's, purely because that namespace is defined further
	 * down this file and this harness is not worth a forward declaration for.
	 */
	ATraceCharacter* FindLocalTraceCharacter()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* TestWorld = Context.World();
			if (TestWorld == nullptr || !TestWorld->IsGameWorld())
			{
				continue;
			}
			if (APlayerController* LocalController = TestWorld->GetFirstPlayerController())
			{
				if (ATraceCharacter* LocalCharacter = Cast<ATraceCharacter>(LocalController->GetPawn()))
				{
					return LocalCharacter;
				}
			}
		}
		return nullptr;
	}

	struct FState
	{
		enum class EPhase : uint8
		{
			ToKnife,        // swap, and wait out the pullout
			ThirdPerson,    // force the camera out, wait for the blend
			BackToFirst,    // release it, wait for the blend — this is where the bug lands
			SwapCycle,      // repeated swaps, including one caught mid-pullout
			Done
		};

		EPhase Phase = EPhase::ToKnife;
		double PhaseStart = 0.0;
		int32 CycleStep = 0;

		int32 Failures = 0;
		bool bAborted = false;
		FString AbortReason;

		/** Worst simultaneous count seen anywhere in the run. Non-zero is the bug, by definition. */
		int32 WorstBothVisible = 0;

		/**
		 * 0 = RED (the pre-v12 latch restored, MUST reproduce the bug), 1 = GREEN (shipped).
		 *
		 * Both arms run, red first, and the verdict is NOT PROVEN unless red went red. That is the
		 * project rule about harnesses that have never failed, applied to the one bug in this pass
		 * whose only symptom is a pixel.
		 */
		int32 Arm = 0;

		int32 RedFailures = -1;
		int32 RedWorst = 0;
	};

	/** One census line. Returns true when BOTH weapons were on screen at once. */
	bool Sample(const TSharedRef<FState>& State, const UTraceWeaponComponent* Weapon,
		const ATraceCharacter* Character, const TCHAR* What)
	{
		if (Weapon == nullptr || Character == nullptr)
		{
			return false;
		}

		int32 Gun = 0;
		int32 Knife = 0;
		int32 Hands = 0;
		int32 BodyKnife = 0;
		Weapon->GetViewModelCensus(Gun, Knife, Hands, BodyKnife);

		const bool bBoth = (Gun > 0 && Knife > 0);
		if (bBoth)
		{
			State->WorstBothVisible = FMath::Max(State->WorstBothVisible, FMath::Min(Gun, Knife));
			++State->Failures;
		}

		const FString Line = FString::Printf(
			TEXT("[DualWeapon] %s  %-46s equipped=%-5s deploying=%d rigVisible=%d | 1P gun=%d knife=%d hands=%d | 3P knife=%d"),
			bBoth ? TEXT("*** BOTH ***") : TEXT("ok          "),
			What, LexToString(Weapon->GetEquippedWeapon()), Weapon->IsDeploying() ? 1 : 0,
			Character->IsViewModelVisible() ? 1 : 0, Gun, Knife, Hands, BodyKnife);

		if (bBoth)
		{
			UE_LOG(LogTraceGame, Error, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line);
		}
		return bBoth;
	}

	/** Puts the world back the way it was found, on every exit path. */
	void RestoreWorld()
	{
		if (IConsoleVariable* ForceThird = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.ForceThirdPerson")))
		{
			ForceThird->Set(0, ECVF_SetByConsole);
		}
		if (IConsoleVariable* Legacy = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Knife.LegacyGunHideLatch")))
		{
			Legacy->Set(0, ECVF_SetByConsole);   // never leave the bug switched on
		}
	}

	void Run(float DelaySeconds, int32 Arm, int32 RedFailures, int32 RedWorst);

	/** Ends one arm, and either chains into the next or prints the combined verdict. */
	void Report(const TSharedRef<FState>& State)
	{
		const bool bRed = (State->Arm == 0);

		UE_LOG(LogTraceGame, Display, TEXT("---------- arm %d (%s): %d census points with BOTH weapons drawn ----------"),
			State->Arm, bRed ? TEXT("RED — the pre-v12 latch restored") : TEXT("GREEN — shipped"), State->Failures);

		if (State->bAborted)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[DualWeapon] arm %d ABORTED: %s"), State->Arm, *State->AbortReason);
			RestoreWorld();
			UE_LOG(LogTraceGame, Warning,
				TEXT("[DualWeapon] RESULT: *** NOT PROVEN *** — the arm did not complete, so neither number means anything."));
			return;
		}

		if (bRed)
		{
			// Chain straight into the green arm, carrying the red numbers so one verdict line can
			// compare them. The latch goes off here and RestoreWorld puts it off again at the end.
			RestoreWorld();
			Run(1.0f, /*Arm=*/1, State->Failures, State->WorstBothVisible);
			return;
		}

		RestoreWorld();

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE DUAL-WEAPON TEST (spec v12 s7) =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("[DualWeapon] RED   arm (bug restored) : %d census points with both weapons drawn, worst overlap %d parts."),
			State->RedFailures, State->RedWorst);
		UE_LOG(LogTraceGame, Display,
			TEXT("[DualWeapon] GREEN arm (shipped code) : %d census points with both weapons drawn, worst overlap %d parts."),
			State->Failures, State->WorstBothVisible);

		const bool bRedReproduced = (State->RedFailures > 0);
		const bool bGreenClean = (State->Failures == 0);

		if (bRedReproduced && bGreenClean)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[DualWeapon] RESULT: FIXED, PROVEN — the bug reproduces with the old latch and is gone without it."));
		}
		else if (!bRedReproduced)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[DualWeapon] RESULT: *** NOT PROVEN *** — the RED arm did not reproduce the bug, so the GREEN "
				     "arm's zero proves nothing. The reproduction, not the fix, is what needs looking at."));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[DualWeapon] RESULT: *** FAIL *** — the gun and the knife are still drawn together on the "
				     "shipped path (%d census points). This is spec v12 s7, unfixed."),
				State->Failures);
		}
		UE_LOG(LogTraceGame, Display, TEXT("=========================================================="));
	}

	void Run(float DelaySeconds, int32 Arm, int32 RedFailures, int32 RedWorst)
	{
		TSharedRef<FState> State = MakeShared<FState>();
		State->Arm = Arm;
		State->RedFailures = RedFailures;
		State->RedWorst = RedWorst;

		// Arm 0 puts the bug back. Every exit path below clears it again — including the aborts.
		if (IConsoleVariable* Legacy = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Knife.LegacyGunHideLatch")))
		{
			Legacy->Set(Arm == 0 ? 1 : 0, ECVF_SetByConsole);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[DualWeapon] arm %d (%s) in %.1fs: equip the knife, force third person and back (the "
			     "carry-blend path), then swap repeatedly — counting VISIBLE viewmodel parts at every step."),
			Arm, (Arm == 0) ? TEXT("RED, bug restored, MUST FAIL") : TEXT("GREEN, shipped"), DelaySeconds);

		// Two tickers, for the reason spelled out in Trace.TestRecoil: FTSTicker's delay applies to
		// every invocation, not only the first.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float /*Delta*/) -> bool
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
					[State](float /*DeltaTime*/) -> bool
					{
						ATraceCharacter* Character = FindLocalTraceCharacter();
						UTraceWeaponComponent* Weapon = (Character != nullptr)
							? Character->FindComponentByClass<UTraceWeaponComponent>() : nullptr;
						if (Weapon == nullptr || Character == nullptr)
						{
							return true;   // no pawn yet — wait rather than reporting zeroes
						}

						const UWorld* TestWorld = Character->GetWorld();
						const double Now = (TestWorld != nullptr) ? TestWorld->GetTimeSeconds() : 0.0;
						if (State->PhaseStart <= 0.0)
						{
							State->PhaseStart = Now;

							if (Character->IsCarrier())
							{
								State->bAborted = true;
								State->AbortReason = TEXT("the local player is carrying the Core; carriers cannot swap by design.");
								Report(State);
								return false;
							}

							ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
							if (!Weapon->RequestEquip(ETraceEquippedWeapon::Knife, &Refusal))
							{
								State->bAborted = true;
								State->AbortReason = FString::Printf(
									TEXT("the swap to the knife was refused: %s"), LexToString(Refusal));
								Report(State);
								return false;
							}
							Sample(State, Weapon, Character, TEXT("t0: knife requested, mid-pullout"));
							return true;
						}

						const double InPhase = Now - State->PhaseStart;

						switch (State->Phase)
						{
						case FState::EPhase::ToKnife:
							// The pullout, plus a beat for the visuals to settle.
							if (InPhase < static_cast<double>(TraceMelee::GetSwapSeconds()) + 0.35)
							{
								return true;
							}
							Sample(State, Weapon, Character, TEXT("knife deployed, first person"));

							// The carry-blend path, without needing a Core. See the block comment.
							{
								static IConsoleVariable* ForceThird =
									IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.ForceThirdPerson"));
								if (ForceThird == nullptr)
								{
									State->bAborted = true;
									State->AbortReason = TEXT("Trace.ForceThirdPerson is missing; the rig hide/show path cannot be driven.");
									Report(State);
									return false;
								}
								ForceThird->Set(1, ECVF_SetByConsole);
							}
							State->Phase = FState::EPhase::ThirdPerson;
							State->PhaseStart = Now;
							return true;

						case FState::EPhase::ThirdPerson:
							// ViewBlendSeconds is ATraceCharacter's, and private; 1.2 s is comfortably
							// past it for any sane value and the census below proves the rig actually
							// went away rather than assuming it.
							if (InPhase < 1.2)
							{
								return true;
							}
							Sample(State, Weapon, Character, TEXT("third person: whole rig should be hidden"));
							{
								static IConsoleVariable* ForceThird =
									IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.ForceThirdPerson"));
								if (ForceThird != nullptr)
								{
									ForceThird->Set(0, ECVF_SetByConsole);
								}
							}
							State->Phase = FState::EPhase::BackToFirst;
							State->PhaseStart = Now;
							return true;

						case FState::EPhase::BackToFirst:
							if (InPhase < 1.2)
							{
								return true;
							}
							// *** THE BUG'S MOMENT. ATraceCharacter has just re-shown every part of
							// its rig, gun included, while the knife is still the equipped weapon.
							Sample(State, Weapon, Character,
								TEXT("BACK TO FIRST PERSON — the reported bug lands here"));
							State->Phase = FState::EPhase::SwapCycle;
							State->PhaseStart = Now;
							State->CycleStep = 0;
							return true;

						case FState::EPhase::SwapCycle:
						{
							// Four swaps. The odd steps are sampled at 0.10 s — INSIDE the 0.2 s
							// pullout — because spec v12 §7's second suspect was a race between the
							// replicated selector and the local prediction during exactly that window.
							const double StepLength = (State->CycleStep % 2 == 0) ? 0.10 : 0.45;
							if (InPhase < StepLength)
							{
								return true;
							}

							const bool bMidPullout = (State->CycleStep % 2 == 0);
							Sample(State, Weapon, Character, bMidPullout
								? TEXT("swap cycle: sampled INSIDE the 0.2s pullout")
								: TEXT("swap cycle: sampled after the pullout"));

							if (++State->CycleStep >= 8)
							{
								State->Phase = FState::EPhase::Done;
								Report(State);
								return false;
							}

							ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
							Weapon->RequestEquip(Weapon->IsKnifeEquipped()
								? ETraceEquippedWeapon::Gun : ETraceEquippedWeapon::Knife, &Refusal);
							State->PhaseStart = Now;
							return true;
						}

						case FState::EPhase::Done:
						default:
							return false;
						}
					}), 0.f);
				return false;
			}), FMath::Max(0.f, DelaySeconds));
	}
}

static FAutoConsoleCommand GTraceDualWeaponTestCmd(
	TEXT("Trace.Knife.DualWeaponTest"),
	TEXT("Dev only. Spec v12 s7. Equips the knife, drives the rig through the carry-blend hide/show "
	     "and four swaps (two sampled mid-pullout), and counts the viewmodel parts actually VISIBLE at "
	     "each step. Any census with a gun part and a knife part both drawn is the bug."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		// =========================================================================================
		// [DUALWIELD] SPEC v28 §10 DELETES THE RULE THIS HARNESS MEASURES, SO IT REFUSES TO RUN
		// RATHER THAN REPORTING A FAILURE.
		// =========================================================================================
		//
		// v12 §7's rule is "a gun part and a knife part must never be drawn at the same time". Under
		// dual-wield that is the SPECIFIED state, on every frame, for every living pawn — so this
		// harness would count every census point as a failure and print a red verdict for a build
		// that is behaving exactly as the owner asked. A harness that reports a deliberate feature as
		// a bug is worse than no harness: the next reader has to work out which of the two documents
		// is stale before they can trust anything else it says.
		//
		// It is not deleted, because the rule it protects comes straight back the moment the switch
		// is flipped. Run it the way the verdict line says and it measures exactly what it always
		// did. The v28 invariant has its OWN harness: Trace.Weapons.V28.
		if (TraceMelee::IsDualWieldEnabled())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[DualWeapon] VERDICT: *** NOT APPLICABLE *** — spec v28 s10 dual-wield is ON, so the gun "
				     "and the knife are SUPPOSED to be drawn together and v12 s7's rule does not exist. Run "
				     "with `-TraceLegacyKnife`, or set `Trace.Knife.DualWield 0` first, to measure it. For the "
				     "v28 invariant (both weapons drawn, selector never KNIFE, melee locks out fire) run "
				     "Trace.Weapons.V28."));
			return;
		}

		const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 8.f;
		TraceDualWeaponTest::Run(Delay, /*Arm=*/0, /*RedFailures=*/-1, /*RedWorst=*/0);
	}));

// =================================================================================================
// Trace.TestRecoil — the unattended proof for spec v5 section 6.
//
// A screenshot cannot show a recoil pattern and a log line per shot cannot show whether the view
// came back. So this holds the trigger on the local player for a few seconds, samples the control
// rotation and the aim agreement throughout, and prints the four numbers that decide whether the
// feature is right:
//
//   PEAK CLIMB      how far above the original aim sustained fire took the view.
//   RESIDUAL PITCH  where the view sat once recovery had finished. Must land back on ~0.000 or the
//                   gun is stealing aim from the player one burst at a time.
//   YAW DRIFT       must be EXACTLY 0.000. This is the "recoil direction 100" claim, measured: a
//                   purely vertical kick cannot move the yaw, and the model has no yaw term at all.
//   MAX aimErr      angle between the camera's forward vector and GetAimDirection(), sampled while
//                   the view is being driven by recoil. The project's standing guarantee is
//                   0.0000 deg and recoil must not be the thing that breaks it.
//
// It drives the same StartFire/StopFire the input layer calls, so it exercises the shipping path
// rather than a test-only one, and it dumps Trace.ShotStats at the end so the client/server hit
// agreement for the same burst is printed beside the recoil numbers.
// =================================================================================================

namespace TraceRecoilTest
{
	ATraceCharacter* FindLocalTraceCharacter()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* TestWorld = Context.World();
			if (TestWorld == nullptr || !TestWorld->IsGameWorld())
			{
				continue;
			}

			if (APlayerController* LocalController = TestWorld->GetFirstPlayerController())
			{
				if (ATraceCharacter* LocalCharacter = Cast<ATraceCharacter>(LocalController->GetPawn()))
				{
					return LocalCharacter;
				}
			}
		}

		return nullptr;
	}

	struct FState
	{
		double Elapsed = 0.0;
		double FireSeconds = 1.6;
		double TotalSeconds = 4.0;
		double NextSampleAt = 0.0;

		bool bStarted = false;
		bool bStopped = false;

		double StartPitch = 0.0;
		double StartYaw = 0.0;
		double PeakClimb = 0.0;
		double MaxAimError = 0.0;
		int32 Samples = 0;
	};

	void Run(float FireSeconds, float SettleSeconds, float DelaySeconds)
	{
		TSharedRef<FState> State = MakeShared<FState>();
		State->FireSeconds = FMath::Max(0.1f, FireSeconds);
		State->TotalSeconds = State->FireSeconds + FMath::Max(0.5f, SettleSeconds);

		UE_LOG(LogTraceGame, Display,
			TEXT("[RecoilTest] in %.1fs: hold the trigger for %.2fs, then measure %.2fs of recovery."),
			DelaySeconds, State->FireSeconds, State->TotalSeconds - State->FireSeconds);

		// TWO TICKERS, AND THE SECOND ONE IS THE BUG FIX. FTSTicker::AddTicker's delay applies to
		// EVERY invocation, not just the first, so arming the sampler directly with a delay of 8
		// makes it run once every eight seconds — the first measured run held the trigger for thirty
		// seconds and never reached its own StopFire. So the delay arms a one-shot that then arms the
		// real per-frame sampler with no delay at all.
		//
		// The delay itself is what lets one unattended run take several samples at different points
		// in a match — one while the field is still empty, a later one mid-fight — instead of betting
		// the whole measurement on whichever two seconds the harness happened to pick.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float /*Delta*/) -> bool
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float DeltaTime) -> bool
			{
				ATraceCharacter* LocalCharacter = FindLocalTraceCharacter();
				UTraceWeaponComponent* Weapon = (LocalCharacter != nullptr)
					? LocalCharacter->FindComponentByClass<UTraceWeaponComponent>() : nullptr;
				APlayerController* LocalController = (LocalCharacter != nullptr)
					? Cast<APlayerController>(LocalCharacter->GetController()) : nullptr;

				if (Weapon == nullptr || LocalController == nullptr)
				{
					// No pawn yet (still in warmup, or on the menu map). Wait rather than reporting
					// a zero, which would read as "recoil does nothing".
					return true;
				}

				const FRotator ControlRotation = LocalController->GetControlRotation();
				const double Pitch = FRotator::NormalizeAxis(ControlRotation.Pitch);
				const double Yaw = FRotator::NormalizeAxis(ControlRotation.Yaw);

				if (!State->bStarted)
				{
					// POINT AT SOMEBODY FIRST. A burst fired at empty air proves the recoil model but
					// says nothing about the thing that actually has to survive it — the client and the
					// server resolving the same shot against the same body. Aimed once, at the start,
					// and deliberately never re-aimed: the whole point is that the climb then walks the
					// muzzle off the target, which is what recoil IS.
					//
					// The candidate is not merely the NEAREST enemy, it is the nearest one the shot
					// would actually reach. The first version took the nearest and measured ten
					// consecutive misses at 100% world truncation: on a field this dense the closest
					// body is usually behind a cover box, and "the client and the server agree that
					// nobody was hit" is a much weaker statement than the guarantee this is here to
					// re-measure. Candidates are therefore tested with the SAME resolver the shot
					// uses, so a target is only chosen if a round sent at it lands on it.
					const ATraceCharacter* AimTarget = nullptr;
					double BestDistanceSq = TNumericLimits<double>::Max();
					if (UWorld* TestWorld = LocalCharacter->GetWorld())
					{
						const FVector EyeLocation = LocalCharacter->GetPawnViewLocation();
						const float ProbeRange = FMath::Max(1.f, UTraceSettings::Get().HitscanRange);

						for (TActorIterator<ATraceCharacter> It(TestWorld); It; ++It)
						{
							ATraceCharacter* Candidate = *It;
							if (Candidate == nullptr || Candidate == LocalCharacter || !Candidate->IsAlive())
							{
								continue;
							}

							const double DistanceSq = FVector::DistSquared(
								Candidate->GetActorLocation(), LocalCharacter->GetActorLocation());
							if (DistanceSq >= BestDistanceSq)
							{
								continue;
							}

							const FVector CandidateChest = Candidate->GetActorLocation() + FVector(0.0, 0.0, 40.0);
							const FVector ProbeDirection = (CandidateChest - EyeLocation).GetSafeNormal();
							if (ProbeDirection.IsNearlyZero())
							{
								continue;
							}

							FVector ProbeImpact = FVector::ZeroVector;
							ETraceHitZone ProbeZone = ETraceHitZone::None;
							const ATraceCharacter* WouldHit = UTraceLagCompensationComponent::ResolveHitscan(
								TestWorld, LocalCharacter, EyeLocation, ProbeDirection, ProbeRange,
								0.f, ProbeImpact, ProbeZone);

							if (WouldHit == Candidate)
							{
								BestDistanceSq = DistanceSq;
								AimTarget = Candidate;
							}
						}
					}

					if (AimTarget != nullptr)
					{
						const FVector Chest = AimTarget->GetActorLocation() + FVector(0.0, 0.0, 40.0);
						LocalController->SetControlRotation((Chest - LocalCharacter->GetPawnViewLocation()).Rotation());
						UE_LOG(LogTraceGame, Display,
							TEXT("[RecoilTest] aiming at %s, %.0fuu away"),
							*GetNameSafe(AimTarget), FMath::Sqrt(BestDistanceSq));
					}
					else
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[RecoilTest] no living target in the world; firing at open air (hit agreement will have nothing to compare)."));
					}

					State->StartPitch = FRotator::NormalizeAxis(LocalController->GetControlRotation().Pitch);
					State->StartYaw = FRotator::NormalizeAxis(LocalController->GetControlRotation().Yaw);
					State->bStarted = true;
					Weapon->StartFire();
					UE_LOG(LogTraceGame, Display,
						TEXT("[RecoilTest] start: pitch %+.3f yaw %+.3f"), State->StartPitch, State->StartYaw);
					return true;   // sample from the next frame, once the aim has settled
				}

				State->Elapsed += DeltaTime;

				const double Climb = Pitch - State->StartPitch;
				State->PeakClimb = FMath::Max(State->PeakClimb, Climb);

				double AimError = 0.0;
				if (LocalCharacter->Camera != nullptr)
				{
					const FVector CameraForward = LocalCharacter->Camera->GetForwardVector();
					const FVector AimDirection = LocalCharacter->GetAimDirection();
					AimError = FMath::RadiansToDegrees(FMath::Acos(
						FMath::Clamp(FVector::DotProduct(CameraForward, AimDirection), -1.0, 1.0)));
					State->MaxAimError = FMath::Max(State->MaxAimError, AimError);
					++State->Samples;
				}

				if (State->Elapsed >= State->NextSampleAt)
				{
					State->NextSampleAt = State->Elapsed + 0.1;
					UE_LOG(LogTraceGame, Display,
						TEXT("[RecoilTest] t=%.2f %s pitch=%+.3f climb=%+.3f yawDrift=%+.4f aimErr=%.4fdeg"),
						State->Elapsed, State->bStopped ? TEXT("RECOVER") : TEXT("FIRING "),
						Pitch, Climb, Yaw - State->StartYaw, AimError);
				}

				if (!State->bStopped && State->Elapsed >= State->FireSeconds)
				{
					State->bStopped = true;
					Weapon->StopFire();
				}

				if (State->Elapsed < State->TotalSeconds)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display, TEXT("========== TRACE RECOIL TEST =========="));
				UE_LOG(LogTraceGame, Display,
					TEXT("RECOIL peak climb   : %+.3f deg above the original aim"), State->PeakClimb);
				UE_LOG(LogTraceGame, Display,
					TEXT("RECOIL residual     : %+.4f deg (must settle to ~0.000 — the gun must not keep the aim)"),
					Pitch - State->StartPitch);
				UE_LOG(LogTraceGame, Display,
					TEXT("RECOIL yaw drift    : %+.4f deg (MUST be 0.0000 — 'recoil direction 100' is purely vertical)"),
					Yaw - State->StartYaw);
				UE_LOG(LogTraceGame, Display,
					TEXT("RECOIL max aimErr   : %.4f deg over %d samples (crosshair vs bullet; the standing guarantee is 0.0000)"),
					State->MaxAimError, State->Samples);
				UE_LOG(LogTraceGame, Display, TEXT("======================================="));

				TraceShotStats::Dump();
				return false;   // the sampler is done
			}));

				return false;   // the delay shot is done; the sampler above is now armed
			}), DelaySeconds);
	}

	FAutoConsoleCommand CmdTestRecoil(
		TEXT("Trace.TestRecoil"),
		TEXT("Dev only. Trace.TestRecoil [FireSeconds] [SettleSeconds] [DelaySeconds] — hold the local player's trigger, then report peak climb, residual pitch, yaw drift and aimErr."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float FireSeconds = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 1.6f;
			const float SettleSeconds = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : 2.4f;
			const float DelaySeconds = (Args.Num() > 2) ? FMath::Max(0.f, FCString::Atof(*Args[2])) : 0.f;
			Run(FireSeconds, SettleSeconds, DelaySeconds);
		}));
}

// =================================================================================================
// Trace.TestKnife — the unattended proof for spec v10 §1.
//
// The user gave THREE numbers and every one of them is a timing: 0.2 s of pullout each way, 0.5 s
// between swings, and 100/30 damage decided by an angle. None of those can be checked by looking at
// a screenshot, and none of them can be checked by reading the header — the whole point of the
// project rule about the .ini is that the header is not the authority. So this drives the SHIPPING
// input path (StartSwing / RequestEquip, the same functions mouse1 and the swap bind call) and
// reports what the clock actually did:
//
//   PULLOUT gun->knife  seconds from the swap request until an attack was first permitted.
//   PULLOUT knife->gun  the same, the other way. The user asked for one number; if these two differ
//                       by more than a frame, they are not one number.
//   SWING-TO-SWING      min / mean / max of the press-to-press interval over a held burst. This is
//                       the interval a player can observe, which is why the cooldown is stamped at
//                       the press rather than at the resolve.
//   DAMAGE PROBE        for the nearest living enemy, the approach angle and therefore the damage a
//                       swing would score right now — measured through the same TraceMelee::
//                       IsBackstab the server uses, not a re-derivation of it.
//
// It also prints the equipped weapon at every transition, so a run in which the swap silently did
// nothing is distinguishable from one in which it worked and the timing was wrong.
// =================================================================================================

namespace TraceKnifeTest
{
	struct FState
	{
		enum class EPhase : uint8
		{
			ToKnife,
			Swinging,
			ToGun,
			Done
		};

		EPhase Phase = EPhase::ToKnife;
		double PhaseStart = 0.0;
		double Elapsed = 0.0;

		double SwingSeconds = 3.0;
		double TimeoutSeconds = 2.0;

		float PulloutToKnife = -1.f;
		float PulloutToGun = -1.f;

		double LastSwingAt = -1.0;
		TArray<double> SwingIntervals;
		int32 SwingCount = 0;

		bool bStarted = false;
		bool bAborted = false;
		FString AbortReason;
	};

	void Report(const TSharedRef<FState>& State, UTraceWeaponComponent* Weapon, ATraceCharacter* Character)
	{
		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE KNIFE TEST =========="));

		if (State->bAborted)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("KNIFE aborted     : %s"), *State->AbortReason);
		}

		// SPEC v31 §1 MADE THIS AN ASYMMETRIC MEASUREMENT AND THAT IS THE POINT OF PRINTING BOTH.
		// gun->knife should read GetSwapSecondsFor(Knife) and knife->gun should read the full base;
		// two numbers that come back EQUAL after v31 means GetSwapSecondsFor is being bypassed
		// somewhere, which is the regression this line is now the cheapest detector of.
		UE_LOG(LogTraceGame, Display,
			TEXT("KNIFE pullout     : gun->knife %.4fs (want %.4fs, v31 s1: base x%.2f) | knife->gun %.4fs (want %.4fs, the unchanged gun pullout)"),
			State->PulloutToKnife, TraceMelee::GetSwapSecondsFor(ETraceEquippedWeapon::Knife),
			TraceMelee::GetKnifeSwapMultiplier(),
			State->PulloutToGun, TraceMelee::GetSwapSecondsFor(ETraceEquippedWeapon::Gun));

		if (State->SwingIntervals.Num() > 0)
		{
			double Sum = 0.0;
			double Min = TNumericLimits<double>::Max();
			double Max = 0.0;
			for (double Interval : State->SwingIntervals)
			{
				Sum += Interval;
				Min = FMath::Min(Min, Interval);
				Max = FMath::Max(Max, Interval);
			}
			UE_LOG(LogTraceGame, Display,
				TEXT("KNIFE swing gap   : %d intervals | min %.4fs  mean %.4fs  max %.4fs   (asked for 0.500s, setting is %.3fs)"),
				State->SwingIntervals.Num(), Min, Sum / State->SwingIntervals.Num(), Max,
				TraceMelee::GetSwingCooldownSeconds());
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("KNIFE swing gap   : NO SWINGS LANDED — %d starts. A knife that never swings is the failure this test exists to catch."),
				State->SwingCount);
		}

		// Damage probe. Analytic, against the real world, through the same predicate the server uses.
		if (Character != nullptr && Character->GetWorld() != nullptr)
		{
			const ATraceCharacter* Nearest = nullptr;
			double NearestDistanceSq = TNumericLimits<double>::Max();
			for (TActorIterator<ATraceCharacter> It(Character->GetWorld()); It; ++It)
			{
				const ATraceCharacter* Candidate = *It;
				if (Candidate == nullptr || Candidate == Character || !Candidate->IsAlive())
				{
					continue;
				}
				const double DistanceSq = FVector::DistSquared(Candidate->GetActorLocation(), Character->GetActorLocation());
				if (DistanceSq < NearestDistanceSq)
				{
					NearestDistanceSq = DistanceSq;
					Nearest = Candidate;
				}
			}

			if (Nearest != nullptr)
			{
				double Angle = -1.0;
				const bool bBackstab = TraceMelee::IsBackstab(
					Character->GetActorLocation(), Nearest->GetActorLocation(),
					static_cast<float>(Nearest->GetActorRotation().Yaw), &Angle);

				UE_LOG(LogTraceGame, Display,
					TEXT("KNIFE probe       : nearest is %s at %.0fuu | approach %.1fdeg (threshold %.0f) -> %s, %.0f damage | carrier=%d (immune)"),
					*GetNameSafe(Nearest), FMath::Sqrt(NearestDistanceSq), Angle,
					TraceMelee::GetBackstabHalfAngleDegrees(),
					bBackstab ? TEXT("BACKSTAB") : TEXT("front"),
					TraceMelee::DamageForApproach(bBackstab),
					Nearest->IsCarrier() ? 1 : 0);
			}
			else
			{
				UE_LOG(LogTraceGame, Display, TEXT("KNIFE probe       : no living target in the world."));
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("KNIFE final state : equipped %s | deploying %d | cooldown %.3fs"),
			(Weapon != nullptr) ? LexToString(Weapon->GetEquippedWeapon()) : TEXT("-"),
			(Weapon != nullptr && Weapon->IsDeploying()) ? 1 : 0,
			(Weapon != nullptr) ? Weapon->GetSwingCooldownRemaining() : 0.f);
		UE_LOG(LogTraceGame, Display, TEXT("======================================"));

		// The pure model, run alongside so one command answers both halves of the feature.
		TraceRunMeleeSelfTest();
	}

	void Run(float SwingSeconds, float DelaySeconds)
	{
		TSharedRef<FState> State = MakeShared<FState>();
		State->SwingSeconds = FMath::Max(1.0f, SwingSeconds);

		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeTest] in %.1fs: swap to the knife, swing for %.1fs, swap back, and report every timing."),
			DelaySeconds, State->SwingSeconds);

		// TWO TICKERS, for the reason spelled out in Trace.TestRecoil: FTSTicker's delay applies to
		// EVERY invocation, not just the first, so a delay on the sampler itself would make it run
		// once per delay period instead of once per frame.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float /*Delta*/) -> bool
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
					[State](float DeltaTime) -> bool
					{
						ATraceCharacter* Character = TraceRecoilTest::FindLocalTraceCharacter();
						UTraceWeaponComponent* Weapon = (Character != nullptr)
							? Character->FindComponentByClass<UTraceWeaponComponent>() : nullptr;

						if (Weapon == nullptr || Character == nullptr)
						{
							// No pawn yet (warm-up, or the menu map). Wait rather than reporting
							// zeroes, which would read as "the knife does nothing".
							return true;
						}

						// The WORLD clock, because that is the clock the gates are measured against
						// (UTraceWeaponComponent::GetLocalTimeSeconds). Measuring the cooldown with
						// a wall clock and gating it with a world clock would silently report the
						// time dilation as a knife bug.
						const UWorld* TestWorld = Character->GetWorld();
						const double Now = (TestWorld != nullptr) ? TestWorld->GetTimeSeconds() : 0.0;

						if (!State->bStarted)
						{
							State->bStarted = true;
							State->PhaseStart = Now;

							if (Character->IsCarrier())
							{
								State->bAborted = true;
								State->AbortReason = TEXT("the local player is carrying the Core; carriers cannot swap or swing by design (spec v10 s1).");
								Report(State, Weapon, Character);
								return false;
							}

							UE_LOG(LogTraceGame, Display, TEXT("[KnifeTest] start: equipped %s. Requesting the knife."),
								LexToString(Weapon->GetEquippedWeapon()));

							ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
							if (!Weapon->RequestEquip(ETraceEquippedWeapon::Knife, &Refusal))
							{
								State->bAborted = true;
								State->AbortReason = FString::Printf(TEXT("the swap to the knife was refused: %s"), LexToString(Refusal));
								Report(State, Weapon, Character);
								return false;
							}
							return true;
						}

						State->Elapsed += DeltaTime;

						switch (State->Phase)
						{
						case FState::EPhase::ToKnife:
						{
							// The pullout is over the instant an attack is permitted — which is the
							// only definition of it a player can feel.
							if (Weapon->IsKnifeEquipped() && Weapon->CanSwing())
							{
								State->PulloutToKnife = static_cast<float>(Now - State->PhaseStart);
								UE_LOG(LogTraceGame, Display, TEXT("[KnifeTest] knife up after %.4fs; swinging for %.1fs."),
									State->PulloutToKnife, State->SwingSeconds);
								State->Phase = FState::EPhase::Swinging;
								State->PhaseStart = Now;
							}
							else if ((Now - State->PhaseStart) > State->TimeoutSeconds)
							{
								State->bAborted = true;
								State->AbortReason = TEXT("the knife never became usable within 2s of the swap.");
								Report(State, Weapon, Character);
								return false;
							}
							break;
						}

						case FState::EPhase::Swinging:
						{
							// Ask EVERY frame, exactly as a held trigger does. Most frames are
							// refused by the cooldown, and the intervals between the ones that are
							// not are the number this test exists to produce.
							if (Weapon->StartSwing())
							{
								++State->SwingCount;
								if (State->LastSwingAt > 0.0)
								{
									State->SwingIntervals.Add(Now - State->LastSwingAt);
								}
								State->LastSwingAt = Now;
							}

							if ((Now - State->PhaseStart) >= State->SwingSeconds)
							{
								UE_LOG(LogTraceGame, Display, TEXT("[KnifeTest] %d swings; requesting the gun."), State->SwingCount);
								State->Phase = FState::EPhase::ToGun;
								State->PhaseStart = Now;
								Weapon->RequestEquip(ETraceEquippedWeapon::Gun);
							}
							break;
						}

						case FState::EPhase::ToGun:
						{
							if (!Weapon->IsKnifeEquipped() && Weapon->CanFire())
							{
								State->PulloutToGun = static_cast<float>(Now - State->PhaseStart);
								State->Phase = FState::EPhase::Done;
								Report(State, Weapon, Character);
								return false;
							}
							if ((Now - State->PhaseStart) > State->TimeoutSeconds)
							{
								State->bAborted = true;
								State->AbortReason = TEXT("the gun never became usable within 2s of the swap back.");
								Report(State, Weapon, Character);
								return false;
							}
							break;
						}

						default:
							return false;
						}

						return true;
					}));

				return false;   // the delay shot is done; the sampler above is now armed
			}), DelaySeconds);
	}

	FAutoConsoleCommand CmdTestKnife(
		TEXT("Trace.TestKnife"),
		TEXT("Dev only. Trace.TestKnife [SwingSeconds] [DelaySeconds] — swap to the knife, swing, swap back, and report the measured pullout and swing-to-swing timings."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float SwingSeconds = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 3.f;
			const float DelaySeconds = (Args.Num() > 1) ? FMath::Max(0.f, FCString::Atof(*Args[1])) : 0.f;
			Run(SwingSeconds, DelaySeconds);
		}));
}

// =================================================================================================
// AMMO'S SELF-TESTS  (spec v16 §1)
//
//   Trace.Ammo.Dump         what this process actually resolved, plus the live state of the local
//                           pawn's clip. The .ini can win over the header, so this is the only
//                           honest way to read the numbers.
//
//   Trace.Ammo.Test         the mechanic. RED ARM FIRST (Trace.Ammo.Enabled 0): the clip must refuse
//                           to fall, or the green arm's arithmetic is a measurement of nothing.
//                           Then: one round per shot THROUGH THE REAL TRIGGER, the automatic reload
//                           at zero, fire refused for the whole 0.5 s (with Trace.Ammo.ReloadBlocksFire
//                           0 as its own A/B), the refill to a full clip, and the manual R rules.
//
//   Trace.Ammo.CarrierTest  *** "The Core carrier has no gun, so ammo must not be consumed or shown
//                           while carrying." *** Both locks, red-armed, plus a live non-carrier
//                           CONTROL — a clip that cannot be moved at all would otherwise report the
//                           carrier rule holding when nothing had been tested.
//
//   Trace.Ammo.BotWatch     "Bots must respect it and reload; a bot that dry-fires forever will read
//                           as broken." Watches every bot's real clip for N seconds and reports what
//                           it saw. INVALID rather than PASS when the bots did not fire enough to
//                           make the question meaningful.
//
// TWO WAYS OF DRIVING A SHOT, AND EACH IS USED WHERE IT IS HONEST.
//   * THE REAL TRIGGER (StartFire, and the component's own tick) is used to prove the shipping path
//     spends exactly one round per shot. It is the only thing that proves the input path is wired.
//   * THE CONSUMPTION FUNCTION (DebugConsumeRound) is used for the parts a real trigger cannot
//     reach in reasonable time or at all: emptying a 30-round clip would take 12 s of live fire in a
//     match where a bot may kill the subject halfway through, and the carrier's ConsumeRound guard
//     is by construction unreachable through the trigger, because CanFire() refuses a carrier first.
//     Both drive the SAME function a shot drives; neither invents a number.
// =================================================================================================

namespace TraceAmmoTest
{
	IConsoleVariable* FindArm(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	void SetArm(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = FindArm(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** Every arm these commands touch, back to shipped. Called on EVERY exit path, aborts included. */
	void RestoreArms()
	{
		SetArm(TEXT("Trace.Ammo.Enabled"), 1);
		SetArm(TEXT("Trace.Ammo.ReloadBlocksFire"), 1);
		SetArm(TEXT("Trace.Ammo.CarrierGuard"), 1);
	}

	/** The project's usual checklist, so a FAIL is impossible to skim past in a headless log. */
	struct FChecklist
	{
		const TCHAR* Tag = TEXT("AMMO");
		int32 Passed = 0;
		int32 Failed = 0;
		bool  bInvalid = false;
		FString InvalidReason;

		void Check(bool bCondition, const FString& Name, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[%s] %s  %s  |  %s"),
				Tag, bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *Name, *Detail);
		}

		void Invalidate(const FString& Reason)
		{
			bInvalid = true;
			InvalidReason = Reason;
		}

		void Report()
		{
			if (bInvalid)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: INVALID — %s (%d passed, %d failed)"),
					Tag, *InvalidReason, Passed, Failed);
			}
			else if (Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[%s] VERDICT: PASS — %d checks, 0 failed."), Tag, Passed);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d passed, %d FAILED."),
					Tag, Passed, Failed);
			}
		}
	};

	UWorld* FindAuthoritativeWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetAuthGameMode() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/**
	 * A pawn this harness may drive: alive, holding the gun, not carrying the Core, LOCALLY
	 * CONTROLLED (StartFire refuses anything else — input is a local concept) and authoritative.
	 *
	 * The local human first, then any bot. A human is the better subject in a headless run because
	 * nothing else is steering them: a bot's own knife band (ATraceBotController::UpdateKnifeBand,
	 * on the controller since RESTRUCTURE D5) would draw the blade the moment an enemy came within
	 * BotEngageRangeUU and quietly turn "the clip did not fall" into a true statement about a pawn
	 * holding a knife.
	 */
	ATraceCharacter* FindDrivableSubject(UWorld* World)
	{
		auto IsUsable = [](const ATraceCharacter* Candidate) -> bool
		{
			return Candidate != nullptr && Candidate->IsAlive() && !Candidate->IsCarrier()
				&& Candidate->Weapon != nullptr && !Candidate->Weapon->IsKnifeEquipped()
				&& Candidate->IsLocallyControlled() && Candidate->HasAuthority();
		};

		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (ATraceCharacter* HumanPawn = Cast<ATraceCharacter>(PC->GetPawn()); IsUsable(HumanPawn))
			{
				return HumanPawn;
			}
		}

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			if (ATraceCharacter* Candidate = *It; IsUsable(Candidate))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** True when @p Character is driven by AI rather than by a human. Used by Trace.Ammo.BotWatch. */
	bool IsBotControlled(const ATraceCharacter* Character)
	{
		const AController* Controller = (Character != nullptr) ? Character->GetController() : nullptr;
		return Controller != nullptr && Cast<const APlayerController>(Controller) == nullptr;
	}

	// =============================================================================================
	// Trace.Ammo.Dump
	// =============================================================================================

	void RunDump(UWorld* World)
	{
		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE AMMO (spec v16 §1) =========="));
		UE_LOG(LogTraceGame, Display, TEXT("CLIP    size       : %d rounds (UTraceSettings::ClipSize)"),
			TraceAmmo::GetClipSize());
		UE_LOG(LogTraceGame, Display, TEXT("RELOAD  time       : %.3fs (UTraceSettings::ReloadSeconds)"),
			TraceAmmo::GetReloadSeconds());
		UE_LOG(LogTraceGame, Display,
			TEXT("CYCLE   sustained  : %.2fs of fire per clip at FireInterval %.2fs, then %.2fs reloading"),
			TraceAmmo::GetClipSize() * FMath::Max(0.01f, UTraceSettings::Get().FireInterval),
			FMath::Max(0.01f, UTraceSettings::Get().FireInterval), TraceAmmo::GetReloadSeconds());
		UE_LOG(LogTraceGame, Display,
			TEXT("ARMS    enabled=%d reloadBlocksFire=%d carrierGuard=%d predict=%d (all 1 = shipped)"),
			TraceAmmo::IsEnabled() ? 1 : 0, TraceAmmo::DoesReloadBlockFire() ? 1 : 0,
			TraceAmmo::IsCarrierGuardArmed() ? 1 : 0, TraceAmmo::IsPredictionEnabled() ? 1 : 0);
		UE_LOG(LogTraceGame, Display,
			TEXT("ALARM   carrierRoundsConsumed=%d (MUST be 0) | liveness: rounds=%d reloads=%d "
			     "refusedEmpty=%d refusedReloading=%d"),
			TraceAmmo::GetCarrierRoundsConsumed(), TraceAmmo::GetRoundsConsumed(),
			TraceAmmo::GetReloadsCompleted(), TraceAmmo::GetDryFireRefusals(),
			TraceAmmo::GetReloadFireRefusals());

		if (World != nullptr)
		{
			UE_LOG(LogTraceGame, Display, TEXT("NET     mode       : %d (0 standalone, 1 dedicated, 2 listen, 3 client)"),
				static_cast<int32>(World->GetNetMode()));

			if (const APlayerController* PC = World->GetFirstPlayerController())
			{
				if (const ATraceCharacter* Pawn = Cast<ATraceCharacter>(PC->GetPawn());
					Pawn != nullptr && Pawn->Weapon != nullptr)
				{
					const UTraceWeaponComponent* Weapon = Pawn->Weapon;
					UE_LOG(LogTraceGame, Display,
						TEXT("LOCAL   %s: clip %d/%d (%d ability round(s)) | reloading=%d (%.3fs left) | "
						     "shown=%d canFire=%d"),
						*GetNameSafe(Pawn), Weapon->GetClipAmmo(), Weapon->GetClipSize(),
						Weapon->GetAbilityRoundsInClip(), Weapon->IsReloading() ? 1 : 0,
						Weapon->GetReloadRemaining(), Weapon->ShouldShowAmmo() ? 1 : 0,
						Weapon->CanFire() ? 1 : 0);
				}
			}
		}
		UE_LOG(LogTraceGame, Display, TEXT("============================================="));
	}

	// =============================================================================================
	// Trace.Ammo.Test
	// =============================================================================================

	struct FAmmoTestState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;
		TWeakObjectPtr<ATraceCharacter> Subject;

		/** Set by the red arm. Without it the green arm's numbers are uninformative. */
		bool bRedReproduced = false;

		/** Live-fire bookkeeping (step 2). */
		int32 LiveFireClipBefore = 0;
		int32 LiveFireConsumedBefore = 0;
		double LiveFireStartRealTime = 0.0;

		/** Snapshots the later steps compare against. */
		int32 ClipBeforeDrain = 0;
		int32 DrainCalls = 0;
		double ReloadObservedAtRealTime = 0.0;
	};

	void RunAmmoTest()
	{
		UWorld* World = FindAuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[AMMO] no authoritative game world — the clip is server state, so this must run on the server."));
			return;
		}

		TSharedPtr<FAmmoTestState> State = MakeShared<FAmmoTestState>();
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[AMMO] ===== spec v16 §1: '30 bullets per clip, then the gun reloads. R to reload.' "
			     "'Reloading takes .5seconds'. Resolved: %d rounds, %.2fs reload. arm 0 = RED "
			     "(Trace.Ammo.Enabled 0) must leave the clip untouched. ====="),
			TraceAmmo::GetClipSize(), TraceAmmo::GetReloadSeconds());

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreArms();
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			// ---- staging -------------------------------------------------------------------
			if (State->Step == 0)
			{
				ATraceCharacter* Subject = FindDrivableSubject(TickWorld);
				if (Subject == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(TEXT("no drivable subject: this needs a live, locally controlled, "
						                            "authoritative pawn holding the gun and not carrying the Core"));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}

				State->Subject = Subject;
				UE_LOG(LogTraceGame, Display, TEXT("[AMMO] staged on %s (clip %d/%d)."),
					*GetNameSafe(Subject), Subject->Weapon->GetClipAmmo(), TraceAmmo::GetClipSize());

				// The resolved knobs are checked once, here, because every later assertion is stated
				// in terms of them and a wrong ClipSize would make the rest pass against the wrong game.
				State->List.Check(TraceAmmo::GetClipSize() == 30,
					TEXT("'30 bullets per clip'"),
					FString::Printf(TEXT("resolved %d"), TraceAmmo::GetClipSize()));
				State->List.Check(FMath::IsNearlyEqual(TraceAmmo::GetReloadSeconds(), 0.5f, 0.001f),
					TEXT("'Reloading takes .5seconds'"),
					FString::Printf(TEXT("resolved %.3fs"), TraceAmmo::GetReloadSeconds()));

				State->Step = 1;
				return true;
			}

			// *** THE SUBJECT IS RE-VALIDATED ON EVERY TICK, NOT JUST AT STAGING. *** A pawn that died,
			// picked up the Core or drew the knife between two steps would turn the rest of this run
			// into a set of true statements about the wrong situation — the exact shape of the carrier
			// harness that once printed PASS while striking a corpse.
			ATraceCharacter* Subject = State->Subject.Get();
			UTraceWeaponComponent* Weapon = (Subject != nullptr) ? Subject->Weapon : nullptr;
			if (Subject == nullptr || Weapon == nullptr || !Subject->IsAlive()
				|| Subject->IsCarrier() || Weapon->IsKnifeEquipped())
			{
				State->List.Invalidate(FString::Printf(
					TEXT("the subject stopped being testable mid-run (alive=%d carrying=%d knife=%d) — rerun in a "
					     "quieter match"),
					(Subject != nullptr && Subject->IsAlive()) ? 1 : 0,
					(Subject != nullptr && Subject->IsCarrier()) ? 1 : 0,
					(Weapon != nullptr && Weapon->IsKnifeEquipped()) ? 1 : 0));
				State->List.Report();
				RestoreArms();
				return false;
			}

			// ---- step 1: THE RED ARM ---------------------------------------------------------
			if (State->Step == 1)
			{
				SetArm(TEXT("Trace.Ammo.Enabled"), 0);

				const int32 Before = Weapon->GetClipAmmo();
				for (int32 Index = 0; Index < 10; ++Index)
				{
					Weapon->DebugConsumeRound();
				}
				const int32 After = Weapon->GetClipAmmo();

				State->bRedReproduced = (After == Before);
				State->List.Check(State->bRedReproduced,
					TEXT("RED ARM: with the mechanic disarmed, ten rounds cost nothing"),
					FString::Printf(TEXT("clip %d -> %d — this is the build that existed before §1"), Before, After));

				SetArm(TEXT("Trace.Ammo.Enabled"), 1);

				// LIVE FIRE, through the real trigger, starts now and is measured next step.
				State->LiveFireClipBefore = Weapon->GetClipAmmo();
				int32 ReloadsBefore = 0;
				Weapon->DebugGetAmmoTotals(State->LiveFireConsumedBefore, ReloadsBefore);
				State->LiveFireStartRealTime = NowReal;
				Weapon->StartFire();

				State->Step = 2;
				// Long enough for three shots at the shipped 0.40 s interval, short enough that the
				// subject is unlikely to be killed in the middle of it.
				State->NextStepRealTime = NowReal + 1.4;
				return true;
			}

			// ---- step 2: ONE ROUND PER SHOT, THROUGH THE SHIPPING INPUT PATH -----------------
			if (State->Step == 2)
			{
				Weapon->StopFire();

				int32 ConsumedNow = 0;
				int32 ReloadsNow = 0;
				Weapon->DebugGetAmmoTotals(ConsumedNow, ReloadsNow);

				const int32 RoundsSpent = ConsumedNow - State->LiveFireConsumedBefore;
				const int32 ClipDrop = State->LiveFireClipBefore - Weapon->GetClipAmmo();

				// *** THE EXPECTED SHOT COUNT IS DERIVED FROM THE CLOCK, NOT FROM ANY AMMO COUNTER. ***
				//
				// This was a vacuous check once and the fix's own sabotage run is what caught it: with
				// ConsumeRound() removed from ServerFire, BOTH the clip and the component's round
				// counter stayed still, "0 == 0" was true, and "exactly one round per shot" printed
				// PASS on a build where no shot cost anything. Two numbers written by the same function
				// cannot disagree, so they cannot witness each other.
				//
				// Elapsed wall time over FireInterval is an independent witness: the trigger was held
				// for a measured duration and the gun's cadence is a published knob. +/-1 covers the
				// ticker's granularity at each end of the burst.
				const double FireInterval = FMath::Max(0.01, static_cast<double>(UTraceSettings::Get().FireInterval));
				const double HeldSeconds = NowReal - State->LiveFireStartRealTime;
				const int32 ExpectedShots = FMath::FloorToInt(HeldSeconds / FireInterval);
				const bool bPlausibleCount = (ClipDrop >= ExpectedShots - 1) && (ClipDrop <= ExpectedShots + 1);

				State->List.Check(ClipDrop > 0 && bPlausibleCount,
					TEXT("the REAL trigger actually spends rounds: StartFire -> FireOnce -> ServerFire -> the clip"),
					FString::Printf(TEXT("clip fell by %d in %.2fs at FireInterval %.2f, so ~%d shot(s) were "
					                     "expected (+/-1)"),
						ClipDrop, HeldSeconds, FireInterval, ExpectedShots));

				State->List.Check(ClipDrop > 0 && RoundsSpent == ClipDrop,
					TEXT("exactly ONE round leaves the clip per shot"),
					FString::Printf(TEXT("clip fell by %d for %d recorded round(s) — %d/%d left. The `> 0` is "
					                     "load-bearing: without it a gun that consumed nothing would report both "
					                     "numbers as 0 and PASS"),
						ClipDrop, RoundsSpent, Weapon->GetClipAmmo(), TraceAmmo::GetClipSize()));

				// ---- drain the rest through the same consumption function a shot uses ----
				State->ClipBeforeDrain = Weapon->GetClipAmmo();
				State->DrainCalls = 0;
				while (Weapon->GetClipAmmo() > 0 && State->DrainCalls <= 1000)
				{
					Weapon->DebugConsumeRound();
					++State->DrainCalls;
				}

				State->List.Check(State->DrainCalls == State->ClipBeforeDrain,
					TEXT("the clip holds exactly the rounds it says it holds"),
					FString::Printf(TEXT("%d consumption(s) took %d rounds to zero"),
						State->DrainCalls, State->ClipBeforeDrain));

				State->Step = 3;
				State->NextStepRealTime = NowReal + 0.1;   // let one TickReload run
				return true;
			}

			// ---- step 3: "then the gun reloads" — automatic, and it refuses fire -------------
			if (State->Step == 3)
			{
				const bool bReloading = Weapon->IsReloading();
				const float Remaining = Weapon->GetReloadRemaining();
				const bool bRefused = !Weapon->CanFire();

				State->List.Check(bReloading,
					TEXT("'30 bullets per clip, THEN THE GUN RELOADS' — with no key pressed"),
					FString::Printf(TEXT("reloading=%d with %.3fs left of %.2fs"),
						bReloading ? 1 : 0, Remaining, TraceAmmo::GetReloadSeconds()));

				State->List.Check(bRefused,
					TEXT("firing is refused while the reload runs"),
					FString::Printf(TEXT("CanFire()=%d, empty-clip refusals %d, mid-reload refusals %d"),
						Weapon->CanFire() ? 1 : 0, TraceAmmo::GetDryFireRefusals(),
						TraceAmmo::GetReloadFireRefusals()));

				State->ReloadObservedAtRealTime = NowReal;
				State->Step = 4;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.25;
				return true;
			}

			// ---- step 4: the refill, then the MANUAL reload rules ----------------------------
			if (State->Step == 4)
			{
				State->List.Check(Weapon->GetClipAmmo() == TraceAmmo::GetClipSize() && !Weapon->IsReloading(),
					TEXT("the reload finishes and the clip is full again"),
					FString::Printf(TEXT("clip %d/%d, reloading=%d, ~%.2fs after it started"),
						Weapon->GetClipAmmo(), TraceAmmo::GetClipSize(), Weapon->IsReloading() ? 1 : 0,
						NowReal - State->ReloadObservedAtRealTime));

				// R WITH A FULL CLIP DOES NOTHING. [ASSUMPTION], spec v16 §1.
				const bool bFullReload = Weapon->RequestReload();
				State->List.Check(!bFullReload && !Weapon->IsReloading(),
					TEXT("R with a FULL clip does nothing at all"),
					FString::Printf(TEXT("RequestReload()=%d, reloading=%d — it must not restart the timer"),
						bFullReload ? 1 : 0, Weapon->IsReloading() ? 1 : 0));

				// A partial clip, then R.
				for (int32 Index = 0; Index < 5; ++Index)
				{
					Weapon->DebugConsumeRound();
				}
				const int32 PartialClip = Weapon->GetClipAmmo();

				// THE CONTROL FOR THE NEXT CHECK: the gun must be able to fire RIGHT NOW, or "refused
				// during a reload" would be true for some entirely unrelated reason.
				const bool bCanFireBefore = Weapon->CanFire();

				const bool bManualReload = Weapon->RequestReload();
				State->List.Check(bManualReload && Weapon->IsReloading(),
					TEXT("R with a partial clip starts a reload"),
					FString::Printf(TEXT("clip was %d/%d, RequestReload()=%d, %.3fs left"),
						PartialClip, TraceAmmo::GetClipSize(), bManualReload ? 1 : 0,
						Weapon->GetReloadRemaining()));

				// ---- THE RELOAD GATE'S OWN A/B, on a NON-empty clip so the empty-clip refusal
				//      cannot be what is being measured. One arm, nothing else changed. ----
				const bool bRefusedArmed = !Weapon->CanFire();
				SetArm(TEXT("Trace.Ammo.ReloadBlocksFire"), 0);
				const bool bAllowedDisarmed = Weapon->CanFire();
				SetArm(TEXT("Trace.Ammo.ReloadBlocksFire"), 1);

				State->List.Check(bCanFireBefore && bRefusedArmed && bAllowedDisarmed,
					TEXT("the mid-reload refusal is the RELOAD's doing and nothing else's"),
					FString::Printf(TEXT("canFire before the reload=%d | during, armed=%d | during, "
					                     "Trace.Ammo.ReloadBlocksFire 0=%d (want 1 / 0 / 1)"),
						bCanFireBefore ? 1 : 0, bRefusedArmed ? 0 : 1, bAllowedDisarmed ? 1 : 0));

				State->Step = 5;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.25;
				return true;
			}

			// ---- step 5: the manual reload also refills, and the verdict ---------------------
			State->List.Check(Weapon->GetClipAmmo() == TraceAmmo::GetClipSize() && !Weapon->IsReloading(),
				TEXT("the manual reload refills the clip too"),
				FString::Printf(TEXT("clip %d/%d, reloading=%d"),
					Weapon->GetClipAmmo(), TraceAmmo::GetClipSize(), Weapon->IsReloading() ? 1 : 0));

			State->List.Check(TraceAmmo::GetCarrierRoundsConsumed() == 0,
				TEXT("no round was ever taken from a Core carrier's clip during this run"),
				FString::Printf(TEXT("alarm=%d (Trace.Ammo.CarrierTest is what red-arms this properly)"),
					TraceAmmo::GetCarrierRoundsConsumed()));

			if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — Trace.Ammo.Enabled 0 failed to stop "
				                            "the clip falling, so nothing above is a measurement of the ammo code"));
			}

			State->List.Report();
			RestoreArms();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Ammo.CarrierTest
	// =============================================================================================

	struct FCarrierAmmoState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;
		TWeakObjectPtr<ATraceCharacter> CarrierPawn;
		TWeakObjectPtr<ATraceCharacter> ControlPawn;

		bool bRedReproduced = false;
		bool bControlWorked = false;
	};

	void RunCarrierTest()
	{
		UWorld* World = FindAuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[AMMOCARRIER] no authoritative game world."));
			return;
		}

		TSharedPtr<FCarrierAmmoState> State = MakeShared<FCarrierAmmoState>();
		State->List.Tag = TEXT("AMMOCARRIER");
		State->Deadline = FPlatformTime::Seconds() + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[AMMOCARRIER] ===== spec v16 §1: 'The Core carrier has no gun, so ammo must not be consumed or "
			     "shown while carrying.' TWO locks: (1) CanFire()/ServerFire refuse a carrier outright, (2) "
			     "ConsumeRound refuses one even when called directly. arm 0 (Trace.Ammo.CarrierGuard 0) must "
			     "reproduce on lock 2, or arm 1's clean run is uninformative. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreArms();
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);

			// ---- staging: a live carrier and a live non-carrier control ----------------------
			if (State->Step == 0)
			{
				ATraceCharacter* CarrierPawn = (CoreActor != nullptr) ? CoreActor->Carrier : nullptr;
				ATraceCharacter* ControlPawn = nullptr;

				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || !Candidate->IsAlive() || Candidate->Weapon == nullptr
						|| !Candidate->HasAuthority())
					{
						continue;
					}
					if (Candidate == CarrierPawn || Candidate->IsCarrier())
					{
						continue;
					}
					ControlPawn = Candidate;
					break;
				}

				// GrantTo, not TryPickup: TryPickup is a no-op while there is already a holder, which is
				// exactly the case that has to be corrected when the Core is loose or in flight.
				if (CarrierPawn == nullptr && CoreActor != nullptr && ControlPawn != nullptr)
				{
					CoreActor->GrantTo(ControlPawn, ETraceCoreGrantReason::Debug);
					CarrierPawn = CoreActor->Carrier;
					ControlPawn = nullptr;   // it is the carrier now; find another next tick
				}

				if (CarrierPawn == nullptr || CarrierPawn->Weapon == nullptr
					|| ControlPawn == nullptr || ControlPawn->Weapon == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(FString::Printf(
							TEXT("could not stage: carrier=%s control=%s — this needs a live match with at least "
							     "two pawns"),
							*GetNameSafe(CarrierPawn), *GetNameSafe(ControlPawn)));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}

				State->CarrierPawn = CarrierPawn;
				State->ControlPawn = ControlPawn;
				UE_LOG(LogTraceGame, Display, TEXT("[AMMOCARRIER] staged: CARRIER %s | CONTROL %s"),
					*GetNameSafe(CarrierPawn), *GetNameSafe(ControlPawn));
				State->Step = 1;
				return true;
			}

			ATraceCharacter* Carrier = State->CarrierPawn.Get();
			ATraceCharacter* Control = State->ControlPawn.Get();
			UTraceWeaponComponent* CarrierWeapon = (Carrier != nullptr) ? Carrier->Weapon : nullptr;
			UTraceWeaponComponent* ControlWeapon = (Control != nullptr) ? Control->Weapon : nullptr;
			if (CarrierWeapon == nullptr || ControlWeapon == nullptr
				|| !Carrier->IsAlive() || !Control->IsAlive())
			{
				State->List.Invalidate(TEXT("a participant went away or died mid-test"));
				State->List.Report();
				RestoreArms();
				return false;
			}

			// *** THE LIVE-CARRIER GATE, RE-ASSERTED EVERY TICK — BUT ONLY WHILE THE SUBJECT IS THE
			// CARRIER. *** Bots pass and score, so a subject who stopped carrying between staging and
			// the strike would make steps 1 and 2 an ordinary ammo test that passes for entirely the
			// wrong reason.
			//
			// IT IS SKIPPED FOR STEP 3, AND THAT IS A BUG THIS HARNESS ALREADY HAD ONCE. Step 2 ends by
			// GRANTING the Core to the control on purpose (that is the cancellation under test), and
			// the gate below would then dutifully take it straight back — so step 3 measured a
			// cancelled reload on a pawn that was no longer carrying anything, and printed
			// "reloading=0, carrying=0", which is a PASS that says nothing. The check now runs while
			// the control is still holding the Core and asserts BOTH facts in one statement.
			if (State->Step < 3)
			{
				if (CoreActor != nullptr && CoreActor->Carrier != Carrier)
				{
					CoreActor->TryPickup(Carrier);
				}
				if (CoreActor == nullptr || CoreActor->Carrier != Carrier || Control->IsCarrier())
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(TEXT("could not keep the Core on the carrier (and off the control)"));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}
			}

			// ---- step 1: LOCK 1 — the shipped path refuses a carrier the trigger, and RED lock 2 ----
			if (State->Step == 1)
			{
				State->List.Check(!CarrierWeapon->CanFire(),
					TEXT("LOCK 1: a carrier cannot fire at all, so no round can reach the clip"),
					FString::Printf(TEXT("CanFire()=%d on a live carrier (spec §4's own rule, re-checked here "
					                     "because it is the FIRST reason ammo is not consumed)"),
						CarrierWeapon->CanFire() ? 1 : 0));

				State->List.Check(!CarrierWeapon->ShouldShowAmmo(),
					TEXT("'...or SHOWN while carrying'"),
					FString::Printf(TEXT("ShouldShowAmmo()=%d on the carrier, %d on the control"),
						CarrierWeapon->ShouldShowAmmo() ? 1 : 0, ControlWeapon->ShouldShowAmmo() ? 1 : 0));

				// THE RED ARM for lock 2. Driven through DebugConsumeRound because the shipped path
				// cannot reach ConsumeRound with a carrier at all — lock 1 stops it first — so a
				// trigger-driven red arm would watch the clip hold and prove nothing about lock 2.
				SetArm(TEXT("Trace.Ammo.CarrierGuard"), 0);
				const int32 AlarmBefore = TraceAmmo::GetCarrierRoundsConsumed();
				const int32 ClipBefore = CarrierWeapon->GetClipAmmo();
				CarrierWeapon->DebugConsumeRound();
				const int32 ClipAfter = CarrierWeapon->GetClipAmmo();
				const int32 AlarmAfter = TraceAmmo::GetCarrierRoundsConsumed();
				SetArm(TEXT("Trace.Ammo.CarrierGuard"), 1);

				State->bRedReproduced = (ClipAfter == ClipBefore - 1) && (AlarmAfter > AlarmBefore);
				State->List.Check(State->bRedReproduced,
					TEXT("RED: with the guard removed a carrier's clip DOES lose a round"),
					FString::Printf(TEXT("clip %d -> %d, alarm +%d — this is the failure the shipped guard prevents"),
						ClipBefore, ClipAfter, AlarmAfter - AlarmBefore));

				State->Step = 2;
				return true;
			}

			// ---- step 2: LOCK 2 GREEN, plus the control, plus the pickup cancellation ----------
			if (State->Step == 2)
			{
				RestoreArms();

				const int32 AlarmBefore = TraceAmmo::GetCarrierRoundsConsumed();
				const int32 ClipBefore = CarrierWeapon->GetClipAmmo();
				CarrierWeapon->DebugConsumeRound();
				CarrierWeapon->DebugConsumeRound();
				CarrierWeapon->DebugConsumeRound();
				const int32 ClipAfter = CarrierWeapon->GetClipAmmo();

				State->List.Check(ClipAfter == ClipBefore && TraceAmmo::GetCarrierRoundsConsumed() == AlarmBefore,
					TEXT("LOCK 2 (GREEN): three direct consumptions cost a carrier nothing"),
					FString::Printf(TEXT("clip %d -> %d, alarm +%d (must be +0)"),
						ClipBefore, ClipAfter, TraceAmmo::GetCarrierRoundsConsumed() - AlarmBefore));

				// ---- THE CONTROL. The IDENTICAL call on a non-carrier must really take a round, or
				//      this harness has proved something about itself and nothing about the carrier. ----
				const int32 ControlBefore = ControlWeapon->GetClipAmmo();
				ControlWeapon->DebugConsumeRound();
				const int32 ControlAfter = ControlWeapon->GetClipAmmo();
				State->bControlWorked = (ControlAfter == ControlBefore - 1);

				State->List.Check(State->bControlWorked,
					TEXT("CONTROL: the identical call DOES spend a non-carrier's round"),
					FString::Printf(TEXT("clip %d -> %d on %s"),
						ControlBefore, ControlAfter, *GetNameSafe(Control)));

				// ---- "reloading is cancelled by picking up the Core" ----
				ControlWeapon->RequestReload();
				State->List.Check(ControlWeapon->IsReloading(),
					TEXT("the control starts a reload (the fixture for the cancellation below)"),
					FString::Printf(TEXT("reloading=%d, %.3fs left"),
						ControlWeapon->IsReloading() ? 1 : 0, ControlWeapon->GetReloadRemaining()));

				if (CoreActor != nullptr)
				{
					CoreActor->GrantTo(Control, ETraceCoreGrantReason::Debug);
				}

				State->Step = 3;
				State->NextStepRealTime = NowReal + 0.15;   // let one TickReload see the new carrier
				return true;
			}

			// ---- step 3: the cancellation landed, and the verdict ------------------------------
			//
			// BOTH FACTS IN ONE ASSERTION. "Not reloading" on its own is satisfied by a pawn that never
			// started one, by one whose reload simply finished, and by a pawn that is not carrying at
			// all — so the carrying half is what makes this a statement about the CANCELLATION. The
			// timing is deliberate too: the check runs ~0.15 s into a 0.5 s reload, so a reload that
			// merely ran to completion cannot be mistaken for one that was cancelled.
			{
				const bool bStillCarrying = Control->IsCarrier()
					|| (CoreActor != nullptr && CoreActor->Carrier == Control);
				State->List.Check(bStillCarrying && !ControlWeapon->IsReloading(),
					TEXT("picking up the Core CANCELS a reload in progress"),
					FString::Printf(TEXT("carrying=%d reloading=%d, ~0.35s of the %.2fs reload was still owed"),
						bStillCarrying ? 1 : 0, ControlWeapon->IsReloading() ? 1 : 0,
						TraceAmmo::GetReloadSeconds()));

				// Put the Core back where the harness found it. It is a legal game state either way, but
				// leaving a test's staging behind in a live match is how the NEXT test gets a surprise.
				if (CoreActor != nullptr && Carrier->IsAlive())
				{
					CoreActor->GrantTo(Carrier, ETraceCoreGrantReason::Debug);
				}
			}

			if (!State->bControlWorked)
			{
				State->List.Invalidate(TEXT("the CONTROL consumption on a non-carrier did not move a clip — the "
				                            "fixture cannot spend a round at all, so nothing it says about the "
				                            "carrier means anything"));
			}
			else if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — Trace.Ammo.CarrierGuard 0 failed to "
				                            "let a carrier's clip fall, so the green arm proves nothing"));
			}

			State->List.Report();
			RestoreArms();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Ammo.BotWatch
	// =============================================================================================

	/**
	 * Two halves, because "bots must reload" needs both an OBSERVATION and an EXPERIMENT.
	 *
	 * The observation alone is not enough, and a 120 s run proved it: nine bots spent 21 rounds
	 * between them, which is fewer than one clip, so "no bot reloaded" was a fact about how much the
	 * bots shoot and not about the code. Waiting for a bot to fire thirty rounds in a live match is
	 * waiting on the AI, not on the feature.
	 *
	 * So the experiment DRAINS one live bot's clip through the same consumption function a shot uses,
	 * and then watches that bot put the gun back together with no bot-side code involved at all. The
	 * observation still runs alongside and carries the alarm the experiment cannot: whether any bot,
	 * anywhere, was ever caught standing at zero rounds with no reload running.
	 */
	struct FBotWatchState
	{
		int32 Step = 0;
		double ObserveSeconds = 0.0;
		double ObserveUntilRealTime = 0.0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;

		/** Sampled every tick across every live bot. The "dry-fires forever" alarm. */
		int32 StuckSamples = 0;
		int32 BotSamples = 0;
		int32 BotsSeen = 0;
		int32 BotRoundsAtEnd = 0;

		/** Process-wide deltas. These survive a bot dying and respawning; per-pawn totals do not. */
		int32 GlobalRoundsAtStart = 0;
		int32 GlobalReloadsAtStart = 0;

		TWeakObjectPtr<ATraceCharacter> Victim;
		int32 VictimClipBeforeDrain = 0;
		int32 VictimDrainCalls = 0;
		bool bRedReproduced = false;
	};

	/** A live bot that could reload right now: alive, gun in hand, not carrying the Core. */
	ATraceCharacter* FindDrainableBot(UWorld* World)
	{
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || !Candidate->HasAuthority() || Candidate->Weapon == nullptr
				|| !IsBotControlled(Candidate))
			{
				continue;
			}
			if (Candidate->IsAlive() && !Candidate->IsCarrier() && !Candidate->Weapon->IsKnifeEquipped())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	void RunBotWatch(const TArray<FString>& Args)
	{
		UWorld* World = FindAuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[AMMOBOTS] no authoritative game world."));
			return;
		}

		const double ObserveSeconds = (Args.Num() > 0)
			? FMath::Clamp(FCString::Atod(*Args[0]), 2.0, 600.0) : 20.0;

		TSharedPtr<FBotWatchState> State = MakeShared<FBotWatchState>();
		State->List.Tag = TEXT("AMMOBOTS");
		State->ObserveSeconds = ObserveSeconds;
		State->ObserveUntilRealTime = FPlatformTime::Seconds() + ObserveSeconds;
		State->Deadline = FPlatformTime::Seconds() + ObserveSeconds + 60.0;
		State->GlobalRoundsAtStart = TraceAmmo::GetRoundsConsumed();
		State->GlobalReloadsAtStart = TraceAmmo::GetReloadsCompleted();

		UE_LOG(LogTraceGame, Display,
			TEXT("[AMMOBOTS] ===== spec v16 §1: 'Bots must respect it and reload; a bot that dry-fires forever "
			     "will read as broken.' Observing every bot for %.0fs, then DRAINING one bot's clip and watching "
			     "it reload itself. ====="), ObserveSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreArms();
				return false;
			}

			// --- the observation runs on EVERY tick, through every phase ---------------------
			{
				int32 BotCount = 0;
				int32 RoundsNow = 0;
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					const ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || !Candidate->HasAuthority() || Candidate->Weapon == nullptr
						|| !IsBotControlled(Candidate))
					{
						continue;
					}
					++BotCount;

					int32 Rounds = 0;
					int32 Reloads = 0;
					Candidate->Weapon->DebugGetAmmoTotals(Rounds, Reloads);
					RoundsNow += Rounds;

					// *** THE DRY-FIRE ALARM. *** A living, gun-holding, non-carrying bot at zero rounds
					// with no reload running is exactly the "reads as broken" case §1 names. Sampled
					// rather than asserted once, because it is a STATE a bot can only be caught in while
					// it lasts — and if the automatic reload works it never lasts a whole frame.
					if (Candidate->IsAlive() && !Candidate->IsCarrier()
						&& !Candidate->Weapon->IsKnifeEquipped())
					{
						++State->BotSamples;
						if (Candidate->Weapon->GetClipAmmo() <= 0 && !Candidate->Weapon->IsReloading())
						{
							++State->StuckSamples;
						}
					}
				}
				State->BotsSeen = FMath::Max(State->BotsSeen, BotCount);
				State->BotRoundsAtEnd = RoundsNow;
			}

			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			// --- phase 0: just observe ------------------------------------------------------
			if (State->Step == 0)
			{
				if (NowReal < State->ObserveUntilRealTime)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[AMMOBOTS] observation over %.0fs: %d bot(s) | process-wide rounds +%d, reloads +%d | "
					     "bot-pawn rounds now %d | dry-stuck %d of %d samples"),
					State->ObserveSeconds,
					State->BotsSeen,
					TraceAmmo::GetRoundsConsumed() - State->GlobalRoundsAtStart,
					TraceAmmo::GetReloadsCompleted() - State->GlobalReloadsAtStart,
					State->BotRoundsAtEnd, State->StuckSamples, State->BotSamples);

				State->Step = 1;
				return true;
			}

			ATraceCharacter* Victim = State->Victim.Get();

			// --- phase 1: pick a bot and RED-ARM the drain ----------------------------------
			if (State->Step == 1)
			{
				Victim = FindDrainableBot(TickWorld);
				if (Victim == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(TEXT("no live, gun-holding, non-carrying bot to drain — this needs "
						                            "a match with bots in it"));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}
				State->Victim = Victim;

				SetArm(TEXT("Trace.Ammo.Enabled"), 0);
				const int32 RedBefore = Victim->Weapon->GetClipAmmo();
				for (int32 Index = 0; Index < RedBefore + 5; ++Index)
				{
					Victim->Weapon->DebugConsumeRound();
				}
				State->bRedReproduced = (Victim->Weapon->GetClipAmmo() == RedBefore)
					&& !Victim->Weapon->IsReloading();
				SetArm(TEXT("Trace.Ammo.Enabled"), 1);

				State->List.Check(State->bRedReproduced,
					TEXT("RED ARM: with the mechanic disarmed a bot's clip cannot empty, so it never reloads"),
					FString::Printf(TEXT("%s: clip %d after %d consumptions, reloading=%d"),
						*GetNameSafe(Victim), Victim->Weapon->GetClipAmmo(), RedBefore + 5,
						Victim->Weapon->IsReloading() ? 1 : 0));

				State->Step = 2;
				return true;
			}

			// The bot has to still be a legal subject for the rest of this to mean anything — one that
			// died, took the Core or drew the knife would be a true statement about the wrong pawn.
			if (Victim == nullptr || Victim->Weapon == nullptr || !Victim->IsAlive()
				|| Victim->IsCarrier() || Victim->Weapon->IsKnifeEquipped())
			{
				State->List.Invalidate(TEXT("the drained bot stopped being a legal subject mid-test (died, took "
				                            "the Core, or drew the knife) — rerun"));
				State->List.Report();
				RestoreArms();
				return false;
			}

			// --- phase 2: drain it for real -------------------------------------------------
			if (State->Step == 2)
			{
				State->VictimClipBeforeDrain = Victim->Weapon->GetClipAmmo();
				State->VictimDrainCalls = 0;
				while (Victim->Weapon->GetClipAmmo() > 0 && State->VictimDrainCalls <= 1000)
				{
					Victim->Weapon->DebugConsumeRound();
					++State->VictimDrainCalls;
				}

				State->List.Check(State->VictimDrainCalls == State->VictimClipBeforeDrain
					&& Victim->Weapon->GetClipAmmo() == 0,
					TEXT("a bot's clip is the same finite clip a human's is"),
					FString::Printf(TEXT("%s: %d consumption(s) took %d rounds to zero"),
						*GetNameSafe(Victim), State->VictimDrainCalls, State->VictimClipBeforeDrain));

				State->Step = 3;
				State->NextStepRealTime = NowReal + 0.15;   // let one TickReload run on that pawn
				return true;
			}

			// --- phase 3: it reloads itself -------------------------------------------------
			if (State->Step == 3)
			{
				State->List.Check(Victim->Weapon->IsReloading(),
					TEXT("'Bots must respect it and reload' — the bot starts a reload with nobody telling it to"),
					FString::Printf(TEXT("%s: reloading=%d, %.3fs left. There is no bot-side ammo code at all; "
					                     "TickReload runs on its pawn because the pawn is authoritative and "
					                     "locally controlled in this process"),
						*GetNameSafe(Victim), Victim->Weapon->IsReloading() ? 1 : 0,
						Victim->Weapon->GetReloadRemaining()));

				State->Step = 4;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.25;
				return true;
			}

			// --- phase 4: and comes back with a full clip, then the verdict ------------------
			State->List.Check(Victim->Weapon->GetClipAmmo() == TraceAmmo::GetClipSize(),
				TEXT("...and comes back with a full clip"),
				FString::Printf(TEXT("%s: clip %d/%d"), *GetNameSafe(Victim),
					Victim->Weapon->GetClipAmmo(), TraceAmmo::GetClipSize()));

			State->List.Check(State->StuckSamples == 0,
				TEXT("no bot was EVER caught standing at zero rounds with no reload running"),
				FString::Printf(TEXT("%d dry-stuck sample(s) of %d live bot samples across the whole run — this "
				                     "is the 'dry-fires forever' failure, sampled every tick on every bot"),
					State->StuckSamples, State->BotSamples));

			// LIVENESS, and it is reported rather than asserted. Bots in a headless match shoot far less
			// than one clip's worth in a minute (measured: 21 rounds across 9 bots in 120 s), so
			// "no bot organically emptied a clip" is a fact about the AI and not about this feature. The
			// experiment above is what makes the verdict mean something; this is context.
			UE_LOG(LogTraceGame, Display,
				TEXT("[AMMOBOTS] context: %d bot(s) seen, %d round(s) on their current pawns, process-wide +%d "
				     "rounds and +%d reloads since this command started (the drain and reload above are inside "
				     "those two numbers)"),
				State->BotsSeen, State->BotRoundsAtEnd,
				TraceAmmo::GetRoundsConsumed() - State->GlobalRoundsAtStart,
				TraceAmmo::GetReloadsCompleted() - State->GlobalReloadsAtStart);

			if (State->BotSamples == 0)
			{
				State->List.Invalidate(TEXT("no live bot was ever sampled — the dry-fire alarm had nothing to "
				                            "watch, so its zero means nothing"));
			}
			else if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — Trace.Ammo.Enabled 0 failed to stop "
				                            "the bot's clip emptying, so the reload below proves nothing"));
			}

			State->List.Report();
			RestoreArms();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Ammo.ClientPredictTest — spec v16 §1's prediction clause, measured where it exists
	//
	// "Server-authoritative, client-predicted for feel — the local player must see the count drop on
	// their own shot without waiting for a round trip, exactly like the existing fire path."
	//
	// *** THIS CANNOT BE RUN ON THE HOST, AND REFUSES TO BE. *** On the authority FireOnce does not
	// take the predicted branch at all (ServerFire spends the round instead), so a host-side run would
	// watch the count drop for entirely the wrong reason and report the feature working on a build
	// that had no prediction in it. It runs on a JOINED CLIENT or not at all.
	//
	// THE MEASUREMENT IS ONE SHOT, READ ON THE NEXT TICK. A burst would be ambiguous — at 0.40 s
	// between rounds and a 200 ms round trip, "the count is one behind" is only about half an
	// interval and reads as noise. One round, sampled ~16 ms later, is unambiguous: predicted means
	// -1 now, replicated means -0 now and -1 a round trip later. Run the client under
	// NetEmulation.PktLag 150 or more and the two answers are half a second apart.
	// =============================================================================================

	struct FPredictTestState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;
		TWeakObjectPtr<ATraceCharacter> Subject;
		int32 ClipBefore = 0;
		int32 ClipImmediatelyAfter = 0;
		double FiredAtRealTime = 0.0;
	};

	void RunClientPredictTest()
	{
		// Any game world; the point is that it must NOT be authoritative.
		UWorld* World = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (UWorld* Candidate = Context.World(); Candidate != nullptr && Candidate->IsGameWorld())
				{
					World = Candidate;
					break;
				}
			}
		}

		if (World == nullptr || World->GetNetMode() != NM_Client)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[AMMOPREDICT] *** CLIENT ONLY. *** netmode=%d. On the server (or a listen host's own pawn) "
				     "the predicted branch in FireOnce is never taken — ServerFire spends the round — so a run "
				     "here would report prediction working on a build that has none. Join a host and run it there."),
				(World != nullptr) ? static_cast<int32>(World->GetNetMode()) : -1);
			return;
		}

		TSharedPtr<FPredictTestState> State = MakeShared<FPredictTestState>();
		State->List.Tag = TEXT("AMMOPREDICT");
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[AMMOPREDICT] ===== spec v16 §1: 'client-predicted for feel — the local player must see the "
			     "count drop on their own shot without waiting for a round trip.' Trace.Ammo.Predict is %d "
			     "(1 = shipped, 0 = the RED arm: wait for replication). ====="),
			TraceAmmo::IsPredictionEnabled() ? 1 : 0);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			ATraceCharacter* Subject = State->Subject.Get();
			if (State->Step == 0)
			{
				const APlayerController* PC = TickWorld->GetFirstPlayerController();
				Subject = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
				if (Subject == nullptr || Subject->Weapon == nullptr || !Subject->IsAlive()
					|| Subject->IsCarrier() || Subject->Weapon->IsKnifeEquipped()
					|| !Subject->Weapon->CanFire())
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(TEXT("no local pawn that could fire right now — this needs a live, "
						                            "gun-holding, non-carrying pawn off cooldown"));
						State->List.Report();
						return false;
					}
					return true;
				}
				State->Subject = Subject;

				State->ClipBefore = Subject->Weapon->GetClipAmmo();
				State->FiredAtRealTime = NowReal;
				Subject->Weapon->StartFire();     // exactly one round: StopFire lands next tick
				State->Step = 1;
				return true;
			}

			if (Subject == nullptr || Subject->Weapon == nullptr || !Subject->IsAlive())
			{
				State->List.Invalidate(TEXT("the local pawn went away mid-test"));
				State->List.Report();
				return false;
			}

			// ---- the next tick: the shot has been fired and NOTHING has come back from the server ----
			if (State->Step == 1)
			{
				Subject->Weapon->StopFire();
				State->ClipImmediatelyAfter = Subject->Weapon->GetClipAmmo();

				const double ElapsedMs = (NowReal - State->FiredAtRealTime) * 1000.0;
				const bool bDroppedNow = (State->ClipImmediatelyAfter == State->ClipBefore - 1);

				State->List.Check(bDroppedNow,
					TEXT("the SHOOTER's own count drops on their own shot, with no round trip"),
					FString::Printf(TEXT("clip %d -> %d, %.1f ms after the trigger. Trace.Ammo.Predict=%d. A "
					                     "replication-driven count would still read %d here and only fall a "
					                     "round trip later"),
						State->ClipBefore, State->ClipImmediatelyAfter, ElapsedMs,
						TraceAmmo::IsPredictionEnabled() ? 1 : 0, State->ClipBefore));

				State->Step = 2;
				State->NextStepRealTime = NowReal + 1.5;   // comfortably more than any sane round trip
				return true;
			}

			// ---- 1.5 s later: the server's copy has landed and must AGREE, not refund ----
			const int32 ClipAfterSettle = Subject->Weapon->GetClipAmmo();

			State->List.Check(ClipAfterSettle <= State->ClipBefore - 1,
				TEXT("the server's copy CONFIRMS the predicted round rather than handing it back"),
				FString::Printf(TEXT("clip %d immediately after the shot, %d once replication settled. A count "
				                     "that climbed back to %d would be the 30 -> 29 -> 30 -> 29 bounce "
				                     "OnRep_Ammo's min-rule exists to prevent"),
					State->ClipImmediatelyAfter, ClipAfterSettle, State->ClipBefore));

			UE_LOG(LogTraceGame, Display,
				TEXT("[AMMOPREDICT] netmode=%d | clip before %d, immediately after %d, settled %d | "
				     "reloading=%d | predict arm=%d"),
				static_cast<int32>(TickWorld->GetNetMode()), State->ClipBefore, State->ClipImmediatelyAfter,
				ClipAfterSettle, Subject->Weapon->IsReloading() ? 1 : 0,
				TraceAmmo::IsPredictionEnabled() ? 1 : 0);

			State->List.Report();
			return false;
		}));
	}

	FAutoConsoleCommandWithWorld CmdAmmoDump(
		TEXT("Trace.Ammo.Dump"),
		TEXT("Dev only. Log the ammo values this process resolved RIGHT NOW, the alarms, and the local pawn's live clip."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&RunDump));

	FAutoConsoleCommand CmdAmmoTest(
		TEXT("Trace.Ammo.Test"),
		TEXT("Dev only, SERVER. Spec v16 §1: prove one round per shot through the real trigger, the automatic "
		     "reload at zero, fire refused for the whole 0.5 s, the refill, and the manual R rules. Red-arms "
		     "itself with Trace.Ammo.Enabled 0 first and reports INVALID if that arm did not reproduce."),
		FConsoleCommandDelegate::CreateStatic(&RunAmmoTest));

	FAutoConsoleCommand CmdAmmoCarrierTest(
		TEXT("Trace.Ammo.CarrierTest"),
		TEXT("Dev only, SERVER. Spec v16 §1: the Core carrier consumes no ammo, shows no ammo, and has any "
		     "reload cancelled by the pickup. Red-arms the consumption guard and carries a live non-carrier "
		     "CONTROL, so a clean run cannot be a fixture that never fired."),
		FConsoleCommandDelegate::CreateStatic(&RunCarrierTest));

	FAutoConsoleCommand CmdAmmoClientPredictTest(
		TEXT("Trace.Ammo.ClientPredictTest"),
		TEXT("Dev only, *** CLIENT ONLY ***. Spec v16 §1: prove the shooter's own ammo count drops on their own "
		     "shot with no round trip, and that the server's copy then confirms it rather than refunding the "
		     "round. Red arm: Trace.Ammo.Predict 0. Run the client under NetEmulation.PktLag 150+."),
		FConsoleCommandDelegate::CreateStatic(&RunClientPredictTest));

	FAutoConsoleCommand CmdAmmoBotWatch(
		TEXT("Trace.Ammo.BotWatch"),
		TEXT("Dev only, SERVER. Trace.Ammo.BotWatch [seconds=60]: watch every bot's real clip and prove bots "
		     "spend rounds, complete reloads, and are never caught dry with no reload running."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunBotWatch));
}

// =================================================================================================
// Trace.Smg.Dump  and  Trace.Weapons.V28 — the unattended proof for spec v28 §§9 and 10.
//
// WHY THESE TWO AND NOT A SCREENSHOT. The dual-wield viewmodel photographs fine and is screenshotted
// separately, but nothing in a photograph can show that the SMG's fire rate is 600 RPM rather than
// 190, that a per-character modifier still multiplies it, that each gun keeps its own magazine
// across a swap, or that a swing shuts the trigger for exactly the animation's length. Those are
// numbers, so they get a harness that prints numbers.
//
// EVERY ARM GOES RED FIRST WHERE A RED ARM EXISTS. The melee lockout's red arm is the v28 §10 switch
// itself — with Trace.Knife.DualWield 0 the lockout must measure 0.000 s, because the rule does not
// exist in the v27 build. A lockout test that has only ever been run with the feature on cannot tell
// "the lockout works" from "CanFire happened to be false for some other reason".
//
// WHAT IS **NOT** PROVEN HERE, said plainly rather than implied: the POSITIVE half of the
// melee-vs-pull precedence. Making CanPullNow() true needs a live turnover — a carrier, a drop, the
// 5 s lockout window and the puller on the opposing team — which is another slice's fixture
// (Trace.ModeB.TurnoverVerify). What IS proven is the thing that makes the positive half follow: the
// precedence and the HUD's circle are THE SAME CALL on the same actor, sampled every frame of the
// run, plus the negative arm (no turnover on screen ⇒ right mouse swings) measured directly.
// =================================================================================================

namespace TraceWeaponsV28
{
	/** The pawn the local player is looking out of. Its own copy, like every other harness here. */
	ATraceCharacter* FindLocalPawn()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* TestWorld = Context.World();
			if (TestWorld == nullptr || !TestWorld->IsGameWorld())
			{
				continue;
			}
			if (APlayerController* PC = TestWorld->GetFirstPlayerController())
			{
				if (ATraceCharacter* Pawn = Cast<ATraceCharacter>(PC->GetPawn()))
				{
					return Pawn;
				}
			}
		}
		return nullptr;
	}

	/** One assertion, printed the moment it is made so a run that aborts still leaves its evidence. */
	struct FChecklist
	{
		int32 Passed = 0;
		int32 Failed = 0;

		void Check(const TCHAR* What, bool bOk, const FString& Detail)
		{
			if (bOk) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[V28] %s  %s  (%s)"),
				bOk ? TEXT("PASS") : TEXT("**FAIL**"), What, *Detail);
		}
	};

	/** Prints every resolved SMG number beside the spec's, and returns the failure count. */
	int32 DumpAndCheckNumbers(FChecklist& List)
	{
		const UTraceSettings& S = UTraceSettings::Get();

		const float SmgInterval = TraceAmmo::GetBaseFireInterval(ETraceEquippedWeapon::Smg);
		const float PistolInterval = TraceAmmo::GetBaseFireInterval(ETraceEquippedWeapon::Gun);

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE SMG (spec v28 s9) =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("SMG    : %.0f head / %.0f body / %.0f leg | %.4fs = %.0f RPM | clip %d | reload %.3fs"),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Head),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Body),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Legs),
			SmgInterval, 60.f / FMath::Max(0.0001f, SmgInterval),
			TraceAmmo::GetClipSize(ETraceEquippedWeapon::Smg),
			TraceAmmo::GetReloadSeconds(ETraceEquippedWeapon::Smg));
		UE_LOG(LogTraceGame, Display,
			TEXT("PISTOL : %.0f head / %.0f body / %.0f leg | %.4fs = %.0f RPM | clip %d | reload %.3fs   [MUST BE UNCHANGED]"),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Head),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Body),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Legs),
			PistolInterval, 60.f / FMath::Max(0.0001f, PistolInterval),
			TraceAmmo::GetClipSize(ETraceEquippedWeapon::Gun),
			TraceAmmo::GetReloadSeconds(ETraceEquippedWeapon::Gun));
		UE_LOG(LogTraceGame, Display,
			TEXT("PULLOUT: %.3fs for BOTH GUNS — UTraceMeleeSettings::SwapSeconds, the number that already "
			     "existed. There is no SMG pullout knob and there must never be one (spec v28 s9). The KNIFE "
			     "draws in %.3fs (v31 s1: x%.2f of the base, i.e. %.0f%% shorter) and is the ONLY exception."),
			TraceMelee::GetSwapSecondsFor(ETraceEquippedWeapon::Gun),
			TraceMelee::GetSwapSecondsFor(ETraceEquippedWeapon::Knife),
			TraceMelee::GetKnifeSwapMultiplier(),
			100.f * (1.f - TraceMelee::GetKnifeSwapMultiplier()));
		UE_LOG(LogTraceGame, Display,
			TEXT("TABLE  : SmgFireInterval=%.4f SmgClipSize=%d SmgReloadSeconds=%.3f Smg%s=%.0f/%.0f/%.0f "
			     "(the ini wins over the header; this is the live value)"),
			S.SmgFireInterval, S.SmgClipSize, S.SmgReloadSeconds, TEXT("HBL"),
			S.SmgHeadDamage, S.SmgBodyDamage, S.SmgLegDamage);

		// --- SPEC v29: everything this pass moved, printed beside what it used to be ---------------
		UE_LOG(LogTraceGame, Display,
			TEXT("v29 2d : SMG past %.0fuu pays %.0f/%.0f/%.0f (was %.0f/%.0f/%.0f) | %s | falloff=%s   [SMG ONLY]"),
			S.SmgFalloffStartUU,
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Head, S.SmgFalloffStartUU + 1.0),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Body, S.SmgFalloffStartUU + 1.0),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Legs, S.SmgFalloffStartUU + 1.0),
			S.SmgHeadDamage, S.SmgBodyDamage, S.SmgLegDamage,
			(S.SmgFalloffRampUU <= UE_KINDA_SMALL_NUMBER)
				? TEXT("CLIFF (SmgFalloffRampUU=0 — the shipped choice)")
				: TEXT("RAMP (SmgFalloffRampUU > 0)"),
			S.bSmgDamageFalloff ? TEXT("ON") : TEXT("OFF (RED ARM)"));
		UE_LOG(LogTraceGame, Display,
			TEXT("v29 2b : fire mode — pistol %s, SMG %s. Roxie MODDED forces full auto: %s"),
			S.bPistolFullAuto ? TEXT("FULL AUTO (RED ARM)") : TEXT("SEMI (one shot per press)"),
			S.bSmgFullAuto ? TEXT("FULL AUTO") : TEXT("SEMI (RED ARM)"),
			S.bRoxieModdedFullAuto ? TEXT("yes") : TEXT("no"));
		UE_LOG(LogTraceGame, Display,
			TEXT("v29 2f : fire-clock carry %.2f of an interval (%.4fs for the SMG, %.4fs for the pistol). "
			     "0 = the 537 RPM bug. Measure it with Trace.Weapons.V29 under -UseFixedTimeStep -FPS=54."),
			S.FireIntervalCarryFraction, S.FireIntervalCarryFraction * SmgInterval,
			S.FireIntervalCarryFraction * PistolInterval);
		UE_LOG(LogTraceGame, Display,
			TEXT("v29 2e : recoil master switch %s; Roxie MODDED adds x%.2f of RecoilPitchPerShot (%.3f deg) "
			     "= %.3f deg on the first round of her burst"),
			S.bRecoilEnabled ? TEXT("ON") : TEXT("OFF (Demo 22, unchanged)"),
			S.RoxieModdedRecoilScale, S.RecoilPitchPerShot, S.RoxieModdedRecoilScale * S.RecoilPitchPerShot);

		// The owner's six numbers, asserted rather than printed and hoped over.
		List.Check(TEXT("s9: SMG head damage is 33"),
			FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Head), 33.f, 0.01f),
			FString::Printf(TEXT("resolved %.2f"), TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Head)));
		List.Check(TEXT("s9: SMG body damage is 18"),
			FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Body), 18.f, 0.01f),
			FString::Printf(TEXT("resolved %.2f"), TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Body)));
		List.Check(TEXT("s9: SMG leg damage is 12"),
			FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Legs), 12.f, 0.01f),
			FString::Printf(TEXT("resolved %.2f"), TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Legs)));
		List.Check(TEXT("s9: SMG fire rate is 600 RPM (0.1s between rounds)"),
			FMath::IsNearlyEqual(SmgInterval, 0.1f, 0.0005f),
			FString::Printf(TEXT("resolved %.4fs = %.1f RPM"), SmgInterval, 60.f / FMath::Max(0.0001f, SmgInterval)));
		List.Check(TEXT("s9: SMG clip is 40"),
			TraceAmmo::GetClipSize(ETraceEquippedWeapon::Smg) == 40,
			FString::Printf(TEXT("resolved %d"), TraceAmmo::GetClipSize(ETraceEquippedWeapon::Smg)));
		// *** 0.8 -> 1.3 THIS PASS. SPEC v29 §2c. *** The assertion moved with the knob rather than
		// being loosened: a test that stops naming a number stops being able to catch the ini and the
		// header drifting apart, which is the whole reason this row exists.
		List.Check(TEXT("v29 2c: SMG reload is 1.3s (was 0.8)"),
			FMath::IsNearlyEqual(TraceAmmo::GetReloadSeconds(ETraceEquippedWeapon::Smg), 1.3f, 0.001f),
			FString::Printf(TEXT("resolved %.3fs"), TraceAmmo::GetReloadSeconds(ETraceEquippedWeapon::Smg)));

		// --- SPEC v29 §2d, ASSERTED EITHER SIDE OF THE LINE ---------------------------------------
		//
		// One uu inside and one uu outside, which is the only test that can tell a cliff at 800 from a
		// cliff at 700, from a ramp, and from a falloff that never fires. The near table is re-checked
		// AT the boundary and not just at zero, because "<= Start" versus "< Start" is exactly the kind
		// of off-by-one that a point-blank-only test cannot see.
		{
			const float Start = S.SmgFalloffStartUU;
			const float NearHead = TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Head, Start);
			const float FarHead  = TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Head, Start + 1.0);
			const float FarBody  = TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Body, Start + 1.0);
			const float FarLegs  = TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Smg, ETraceHitZone::Legs, Start + 1.0);

			List.Check(TEXT("v29 2d: the falloff starts at 800uu, and 800 itself still pays the NEAR table"),
				FMath::IsNearlyEqual(Start, 800.f, 0.01f) && FMath::IsNearlyEqual(NearHead, 33.f, 0.01f),
				FString::Printf(TEXT("start %.1fuu, head at exactly the start %.2f"), Start, NearHead));
			List.Check(TEXT("v29 2d: past 800uu the SMG pays 24 / 15 / 10"),
				FMath::IsNearlyEqual(FarHead, 24.f, 0.01f)
				&& FMath::IsNearlyEqual(FarBody, 15.f, 0.01f)
				&& FMath::IsNearlyEqual(FarLegs, 10.f, 0.01f),
				FString::Printf(TEXT("at %.0fuu: %.2f / %.2f / %.2f"), Start + 1.f, FarHead, FarBody, FarLegs));
			List.Check(TEXT("v29 2d: it is a CLIFF — the drop is complete one uu past the line"),
				FMath::IsNearlyZero(S.SmgFalloffRampUU, 0.001f)
				&& FMath::IsNearlyEqual(TraceAmmo::GetFalloffAlpha(ETraceEquippedWeapon::Smg, Start + 1.0), 1.f, 0.001f),
				FString::Printf(TEXT("SmgFalloffRampUU=%.1f, alpha one uu past the line = %.3f"),
					S.SmgFalloffRampUU, TraceAmmo::GetFalloffAlpha(ETraceEquippedWeapon::Smg, Start + 1.0)));

			// *** SMG ONLY, MEASURED AT A RANGE NO ARENA CONTAINS. *** §2d: "Only the SMG; the pistol
			// is unchanged." A pistol priced at 30000 uu must still pay its close-range table.
			List.Check(TEXT("v29 2d: the PISTOL has no falloff at any range"),
				FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Head, 30000.0), 100.f, 0.01f)
				&& FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Body, 30000.0), 40.f, 0.01f)
				&& FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Legs, 30000.0), 25.f, 0.01f)
				&& FMath::IsNearlyZero(TraceAmmo::GetFalloffAlpha(ETraceEquippedWeapon::Gun, 30000.0), 0.001f),
				FString::Printf(TEXT("pistol at 30000uu: %.0f / %.0f / %.0f"),
					TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Head, 30000.0),
					TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Body, 30000.0),
					TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Legs, 30000.0)));
		}

		// THE PISTOL MUST NOT HAVE MOVED. A second weapon that quietly retunes the first is the most
		// likely way this item could ship wrong, and it is invisible without this check.
		//
		// *** THE LEG NUMBER IS NOW ASSERTED, AND THAT IS SPEC v29 §2a. *** This row used to carry a
		// note explaining why it could not: every spec table printed the pistol as 100/40/25 while the
		// running game resolved LEG = 30, and asserting either number would have baked a guess into a
		// test. The owner has answered — 25 — so UTraceDamageSettings::LegDamage is 25 in the header
		// AND in the new [/Script/Trace.TraceDamageSettings] block in DefaultGame.ini, and the
		// contradiction is closed rather than annotated.
		List.Check(TEXT("v29 2a: the PISTOL is 100 head, 40 body, 25 LEG, 0.315789s, 30 rounds, 0.5s"),
			FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Head), 100.f, 0.01f)
			&& FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Body), 40.f, 0.01f)
			&& FMath::IsNearlyEqual(TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Legs), 25.f, 0.01f)
			&& FMath::IsNearlyEqual(PistolInterval, 0.315789f, 0.0005f)
			&& TraceAmmo::GetClipSize(ETraceEquippedWeapon::Gun) == 30
			&& FMath::IsNearlyEqual(TraceAmmo::GetReloadSeconds(ETraceEquippedWeapon::Gun), 0.5f, 0.001f),
			FString::Printf(TEXT("%.0f/%.0f/%.0f, %.6fs, %d, %.3fs"),
				TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Head),
				TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Body),
				TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Legs),
				PistolInterval, TraceAmmo::GetClipSize(ETraceEquippedWeapon::Gun),
				TraceAmmo::GetReloadSeconds(ETraceEquippedWeapon::Gun)));

		// *** THE FOUR-SHOT LEG KILL, ASSERTED RATHER THAN ASSUMED. *** 25 x 4 = 100 and MaxHealth is
		// 100, so the fourth round is still fatal — but with nothing to spare, where 30 had 20 uu of
		// slack. If MaxHealth ever moves up without this number moving, this is the row that says so
		// before a playtest does.
		List.Check(TEXT("v29 2a: four pistol leg shots still kill a full-health player"),
			TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Legs) * 4.f
				>= UTraceSettings::Get().MaxHealth - 0.01f,
			FString::Printf(TEXT("4 x %.0f = %.0f vs MaxHealth %.0f"),
				TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Legs),
				TraceAmmo::GetZoneDamage(ETraceEquippedWeapon::Gun, ETraceHitZone::Legs) * 4.f,
				UTraceSettings::Get().MaxHealth));

		return List.Failed;
	}

	/**
	 * *** THE STANDING RULE, MEASURED ON A LIVE PAWN. ***
	 *
	 * "Per-character fire-rate modifiers must apply to the SMG exactly as they do to the pistol."
	 * The claim is that UTraceWeaponComponent::GetFireInterval() is (the weapon's base) x (this
	 * pawn's ability scale) for BOTH weapons — so the check equips each in turn and compares the
	 * shipped accessor against the two factors multiplied independently.
	 *
	 * ON AN ORDINARY PAWN THE SCALE IS 1.0 and the products are trivially equal, which would be a
	 * test that proves nothing. So it also RE-DERIVES what a Roxie and a stuck Slimeball would get
	 * from the same seam and prints the resulting RPM: if the SMG's base were ever hardcoded past the
	 * scale, these two lines are where it would show up as "990 RPM" turning into "600".
	 */
	void CheckFireRateSeam(FChecklist& List, const UTraceWeaponComponent& Weapon, ATraceCharacter* Pawn)
	{
		const float Scale = UTraceAbilityComponent::GetFireIntervalScaleFor(Pawn);
		const ETraceEquippedWeapon Now = Weapon.GetEquippedWeapon();
		const double Expected = static_cast<double>(TraceAmmo::GetBaseFireInterval(Now)) * static_cast<double>(Scale);

		List.Check(TEXT("s9 STANDING RULE: GetFireInterval() == base(weapon in hand) x GetFireIntervalScaleFor(pawn)"),
			FMath::IsNearlyEqual(Weapon.GetFireInterval(), Expected, 1.0e-6),
			FString::Printf(TEXT("%s: shipped %.6fs vs base %.6f x scale %.4f = %.6fs"),
				LexToString(Now), Weapon.GetFireInterval(),
				TraceAmmo::GetBaseFireInterval(Now), Scale, Expected));

		const float SmgBase = TraceAmmo::GetBaseFireInterval(ETraceEquippedWeapon::Smg);
		const float RoxieScale = 1.f / FMath::Max(0.01f, UTraceSettings::Get().RoxieModdedFireRateMultiplier);
		const float SlimeScale = 1.f / (1.f + FMath::Max(0.f, UTraceSettings::Get().SlimeballStuckFireRateBonus));
		UE_LOG(LogTraceGame, Display,
			TEXT("[V28] the SMG through the SAME seam the pistol uses: base %.0f RPM | Roxie MODDED x%.2f -> %.0f RPM "
			     "| stuck Slimeball +%.0f%% -> %.0f RPM   (no ability knows a second gun exists)"),
			60.f / FMath::Max(0.0001f, SmgBase),
			UTraceSettings::Get().RoxieModdedFireRateMultiplier,
			60.f / FMath::Max(0.0001f, SmgBase * RoxieScale),
			100.f * UTraceSettings::Get().SlimeballStuckFireRateBonus,
			60.f / FMath::Max(0.0001f, SmgBase * SlimeScale));
	}

	/** State for the staged half — the parts that need a pullout to elapse. */
	struct FState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		FChecklist List;

		/** Rounds deliberately spent out of the SMG's magazine, so the stow can be checked against it. */
		int32 SmgSpent = 0;
		int32 PistolBefore = 0;

		/** Sampled every tick: does the melee precedence agree with the HUD's own circle test? */
		int32 PrecedenceSamples = 0;
		int32 PrecedenceDisagreements = 0;
		int32 PullEligibleSamples = 0;

		float MeasuredLockout = -1.f;
		float LegacyLockout = -1.f;

		FTSTicker::FDelegateHandle Handle;

		void Advance(double NowReal, double Seconds) { NextStepRealTime = NowReal + Seconds; ++Step; }
	};

	void Report(const TSharedRef<FState>& State)
	{
		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE WEAPONS v28 — VERDICT =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("[V28] melee-vs-pull precedence: %d frames sampled, %d disagreements with ATraceCore::CanPullNow "
			     "(the HUD's own circle test), %d of them with the circle ELIGIBLE."),
			State->PrecedenceSamples, State->PrecedenceDisagreements, State->PullEligibleSamples);
		if (State->PullEligibleSamples == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[V28] NOTE: the pull circle was never eligible during this run, so only the NEGATIVE arm of "
				     "the precedence (no circle => right mouse melees) was exercised. The positive arm needs a live "
				     "turnover; stage one with the mode-B fixture and re-run."));
		}

		UE_LOG(LogTraceGame, Display, TEXT("[V28] %d passed, %d FAILED."), State->List.Passed, State->List.Failed);
		UE_LOG(LogTraceGame, Display, TEXT("[V28] RESULT: %s"),
			State->List.Failed == 0 ? TEXT("PASS") : TEXT("*** FAIL ***"));
		UE_LOG(LogTraceGame, Display, TEXT("================================================"));
	}

	void Run()
	{
		ATraceCharacter* Pawn = FindLocalPawn();
		UTraceWeaponComponent* Weapon = (Pawn != nullptr) ? Pawn->Weapon.Get() : nullptr;
		if (Pawn == nullptr || Weapon == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[V28] No local pawn with a weapon component yet. Run this from a live match (the character "
				     "select screen holds the pawn back for the first few seconds)."));
			return;
		}
		if (!Pawn->HasAuthority())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[V28] SERVER ONLY (a listen host's own pawn counts). The magazine and lockout checks read "
				     "authoritative state that a client does not own."));
			return;
		}

		TSharedRef<FState> State = MakeShared<FState>();

		// --- The half that needs no ticking: every number, plus the switch's own state -------------
		DumpAndCheckNumbers(State->List);

		UE_LOG(LogTraceGame, Display,
			TEXT("[V28] dual-wield switch: %s. Selector is %s."),
			TraceMelee::IsDualWieldEnabled() ? TEXT("ON") : TEXT("OFF (v27 revert)"),
			LexToString(Weapon->GetEquippedWeapon()));

		// =============================================================================================
		// *** SPEC v31 §1 — THESE THREE ASSERT THE SWITCH'S *CURRENT POSITION*, NOT ITS ON POSITION. ***
		// =============================================================================================
		//
		// They used to read "the switch is ON in the shipped configuration" and "a blade IS in hand
		// while a GUN is selected" — the v28 §10 build stated as a fact. v31 §1 flips the switch, and
		// left alone all three would print **FAIL** against a build that is doing exactly what the
		// owner asked for. That is the failure mode Trace.Knife.DualWeaponTest's own header refuses in
		// as many words: "a harness that reports a deliberate feature as a bug is worse than no
		// harness, because the next reader has to work out which of the two documents is stale."
		//
		// So the expectation is DERIVED from TraceMelee::IsDualWieldEnabled() and both arms are real
		// claims. Flip the switch back and this command still passes; it now measures that the build
		// AGREES WITH ITS OWN SWITCH rather than that the switch has a particular value.
		const bool bDualWield = TraceMelee::IsDualWieldEnabled();

		State->List.Check(TEXT("s10/v31 s1: the shipped switch position is readable and consistent"),
			bDualWield == (UTraceMeleeSettings::Get().bDualWieldKnife
				&& !FParse::Param(FCommandLine::Get(), TEXT("TraceLegacyKnife"))),
			FString::Printf(TEXT("resolved=%s table=%d — v31 s1 ships it OFF (the knife is a weapon again)"),
				bDualWield ? TEXT("ON") : TEXT("OFF"),
				UTraceMeleeSettings::Get().bDualWieldKnife ? 1 : 0));
		State->List.Check(TEXT("s10: the movement bonus follows the switch"),
			// ON: never paid, the blade is free. OFF: paid exactly when no firearm is out.
			TraceMelee::ShouldUseKnifeMovementProfile(Pawn) == (!bDualWield && !Weapon->IsFirearmEquipped()),
			FString::Printf(TEXT("boost=%d dualWield=%d firearmOut=%d"),
				TraceMelee::ShouldUseKnifeMovementProfile(Pawn) ? 1 : 0, bDualWield ? 1 : 0,
				Weapon->IsFirearmEquipped() ? 1 : 0));
		State->List.Check(bDualWield
				? TEXT("s10: a blade IS in hand (so right mouse can swing) while a GUN is selected")
				: TEXT("v31 s1: NO blade is in hand while a gun is selected — the v10 s1 model is back"),
			TraceMelee::IsKnifeInHand(Pawn) == (bDualWield || Weapon->IsKnifeEquipped()),
			FString::Printf(TEXT("inHand=%d firearm=%d knifeSelected=%d dualWield=%d"),
				TraceMelee::IsKnifeInHand(Pawn) ? 1 : 0, Weapon->IsFirearmEquipped() ? 1 : 0,
				Weapon->IsKnifeEquipped() ? 1 : 0, bDualWield ? 1 : 0));

		CheckFireRateSeam(State->List, *Weapon, Pawn);

		State->PistolBefore = Weapon->DebugGetAuthoritativeClipAmmo();

		UE_LOG(LogTraceGame, Display,
			TEXT("[V28] staged half starting: equip the SMG, spend rounds, swap to the pistol and back, then "
			     "swing and measure the shooting lockout (green), then again with the switch off (RED)."));

		State->Handle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float /*Delta*/) -> bool
		{
			ATraceCharacter* TickPawn = FindLocalPawn();
			UTraceWeaponComponent* W = (TickPawn != nullptr) ? TickPawn->Weapon.Get() : nullptr;
			if (TickPawn == nullptr || W == nullptr || !TickPawn->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V28] ABORTED: the local pawn stopped being usable mid-run."));
				Report(State);
				return false;
			}

			// EVERY FRAME, WHATEVER STEP WE ARE ON: the precedence and the HUD's circle test must be
			// the same answer. This is the strongest statement available about the "if and only if
			// they are being shown the circle" rule without staging a turnover — the two are one call
			// and this samples that they stay one call for the whole run.
			if (const ATraceCore* Core = ATraceCore::Get(TickPawn->GetWorld()))
			{
				const bool bCircle = Core->CanPullNow(TickPawn);
				const bool bPrecedence = TraceMelee::ShouldCorePullOverrideMelee(TickPawn);
				++State->PrecedenceSamples;
				if (bCircle != bPrecedence) { ++State->PrecedenceDisagreements; }
				if (bCircle) { ++State->PullEligibleSamples; }
			}

			const double NowReal = FPlatformTime::Seconds();
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			switch (State->Step)
			{
			case 0:
			{
				// =====================================================================================
				// *** SPEC v29 §5 SHIFTED THE KEYS UNDER THIS BLOCK. THE FIXTURE IS REPAIRED, NOT THE
				// *** GAME. ***
				// =====================================================================================
				//
				// v28 §10 remapped 1 and 2 onto PISTOL and SMG. v29 §5 replaces that with 1 = STOW
				// GUNS, 2 = PISTOL, 3 = SMG, so this block asked for ETraceEquippedWeapon::Gun and
				// called the answer "the SMG", then asked for ::Knife and called the answer "the
				// PISTOL". Both are now wrong by one key, and every §9 MAGAZINE assertion downstream
				// inherited the error: the run reported 6 failures, all of them the harness reading
				// the previous pass's keyboard. Measured before touching anything —
				// "s10: the \"1\" key (EquipKnife) selects the PISTOL (weapon=KNIFE)" is a harness
				// failing a build that is behaving exactly as v29 §5 specifies.
				//
				// WHAT IS KEPT IS THE PART WORTH KEEPING. Spec v28 §9's "each gun keeps its own
				// magazine" is a real invariant and this is the only staged test of it, so the walk
				// still spends rounds from one gun, swaps away and swaps back — it just does it
				// between the two FIREARMS (2 and 3) instead of between a firearm and the stowed
				// state, which is what those two keys now mean. ApplyEquip's magazine exchange is
				// firearm-to-firearm by construction, so this is also the only pair that ever
				// exercised it.
				//
				// The KNIFE key is deliberately NOT re-tested here: Trace.Weapon.Verify covers all
				// three slots, the boost, the movement bit, the two pullouts and the blade, and a
				// second half-copy of that is how two harnesses end up disagreeing about one rule.
				//
				// *** v31 s1 SHIFTED THE DIGITS AGAIN (1 pistol, 2 SMG, 3 knife) AND THIS BLOCK DID
				// *** NOT HAVE TO MOVE, only its labels. *** It asks for a WEAPON through
				// RequestEquipIfDifferent rather than pressing a key, so the magazine invariant it
				// guards is stated in weapons and survives every re-lay of the keyboard. The v29
				// breakage recorded above happened because the labels claimed a key; they now name
				// the v31 key and the assertion still names the weapon.
				ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
				const bool bAsked = TraceMelee::RequestEquipIfDifferent(TickPawn, ETraceEquippedWeapon::Smg, &Refusal);
				State->List.Check(TEXT("v31 s1: the \"2\" key (SmgSlot) selects the SMG"),
					bAsked || W->IsSmgEquipped(),
					FString::Printf(TEXT("asked=%d refusal=%s"), bAsked ? 1 : 0, LexToString(Refusal)));
				State->Advance(NowReal, TraceMelee::GetSwapSeconds() + 0.25);
				break;
			}

			case 1:
			{
				State->List.Check(TEXT("s9: the SMG is in hand, with a full 40-round magazine"),
					W->IsSmgEquipped() && W->DebugGetAuthoritativeClipAmmo() == 40 && W->GetClipSize() == 40,
					FString::Printf(TEXT("weapon=%s clip=%d/%d reload=%.2fs"),
						LexToString(W->GetEquippedWeapon()), W->DebugGetAuthoritativeClipAmmo(),
						W->GetClipSize(), W->GetReloadSeconds()));

				CheckFireRateSeam(State->List, *W, TickPawn);

				// Spend seven rounds through the real consumption path, so the stow has something
				// distinctive to remember. Seven because it is neither the clip size nor a round
				// number that could match the pistol's by accident.
				for (int32 i = 0; i < 7; ++i)
				{
					W->DebugConsumeRound();
				}
				State->SmgSpent = 7;
				State->List.Check(TEXT("s9: rounds come out of the SMG's own magazine"),
					W->DebugGetAuthoritativeClipAmmo() == 33,
					FString::Printf(TEXT("40 - 7 = %d"), W->DebugGetAuthoritativeClipAmmo()));

				ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
				TraceMelee::RequestEquipIfDifferent(TickPawn, ETraceEquippedWeapon::Gun, &Refusal);     // the "1" key (v31 s1: PISTOL)
				State->Advance(NowReal, TraceMelee::GetSwapSeconds() + 0.25);
				break;
			}

			case 2:
			{
				State->List.Check(TEXT("v31 s1: the \"1\" key (PistolSlot) selects the PISTOL"),
					W->GetEquippedWeapon() == ETraceEquippedWeapon::Gun,
					FString::Printf(TEXT("weapon=%s"), LexToString(W->GetEquippedWeapon())));
				// *** THE EXPECTATION IS WHAT THE PISTOL LEFT WITH, NOT A FULL 30. ***
				//
				// Spec v28 §9's invariant is "each gun keeps its OWN magazine". "30" is not that
				// invariant — it is an assumption that nothing spent a pistol round earlier in this
				// PROCESS, which is a property of what else ran, not of the rule. State->PistolBefore
				// has been sampled at the top of the staged half since v28 and was simply never used
				// here; this completes that.
				//
				// FOUND BY RUNNING TWO HARNESSES IN ONE SESSION, WHICH IS WHAT AN INTEGRATION PASS
				// DOES AND WHAT A SLICE NEVER DOES. Trace.Weapon.Verify's own magazine regression
				// (v31 §1) fires five rounds from the pistol and leaves it at 25/30 on purpose. Run
				// it before this and a correct build printed **FAIL** on a rule it was obeying
				// perfectly: "clip=25/30". Measured — V28 alone: 28 passed, 0 FAILED; V28 after
				// Trace.Weapon.Verify: 27 passed, 1 FAILED, on this exact line.
				//
				// The clip SIZE stays absolute. 30 is a fact about the pistol, not about the session,
				// and it is the half of this check that catches the SMG's 40-round magazine following
				// the weapon across a swap — which is the failure the line exists for.
				State->List.Check(TEXT("s9: the pistol came back with ITS OWN magazine, at the count it left with"),
					W->DebugGetAuthoritativeClipAmmo() == State->PistolBefore && W->GetClipSize() == 30,
					FString::Printf(TEXT("clip=%d/%d, left with %d/30 (a full pistol is 30; the SMG's is 40)"),
						W->DebugGetAuthoritativeClipAmmo(), W->GetClipSize(), State->PistolBefore));

				ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
				TraceMelee::RequestEquipIfDifferent(TickPawn, ETraceEquippedWeapon::Smg, &Refusal);     // the "2" key, back to the SMG
				State->Advance(NowReal, TraceMelee::GetSwapSeconds() + 0.25);
				break;
			}

			case 3:
			{
				State->List.Check(TEXT("s9: the SMG remembered the seven rounds it had spent"),
					W->IsSmgEquipped() && W->DebugGetAuthoritativeClipAmmo() == 40 - State->SmgSpent,
					FString::Printf(TEXT("expected %d, clip=%d"), 40 - State->SmgSpent,
						W->DebugGetAuthoritativeClipAmmo()));

				// --- THE MELEE MODEL, IN THE SHIPPED SWITCH POSITION --------------------------------
				//
				// *** SPEC v31 §1 INVERTED THIS PAIR OF ARMS AND THAT IS THE WHOLE POINT OF THEM. ***
				// It used to hard-code the v28 §10 answer: "right mouse MELEES" and "swinging locks
				// out shooting". v31 §1 flips the switch OFF, and with the knife a WEAPON again both
				// of those sentences become false BY DESIGN — mouse 1 is the attack, right mouse
				// refuses with WrongWeapon while a gun is out, and there is no lockout because you
				// could not shoot with the knife out anyway. Left alone this block printed three
				// **FAIL**s against a build doing exactly what the owner asked for.
				//
				// So both arms are derived from the switch. The claim under test is no longer "the
				// v28 §10 rules hold" but "the build agrees with its own switch", which is the claim
				// that stays true whichever way anybody flips it next.
				const bool bDual = TraceMelee::IsDualWieldEnabled();

				ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
				const bool bSwung = (TraceMelee::HandleMeleeInput(TickPawn, /*bPressed=*/true, &Refusal)
					== TraceMelee::EMeleeInputResult::Swing);
				State->List.Check(bDual
						? TEXT("s10: right mouse MELEES when the pull circle is not on screen")
						: TEXT("v31 s1: right mouse REFUSES to melee while a gun is out (the v10 s1 model)"),
					bDual ? bSwung : (!bSwung && Refusal == ETraceMeleeRefusal::WrongWeapon),
					FString::Printf(TEXT("swung=%d refusal=%s dualWield=%d"),
						bSwung ? 1 : 0, LexToString(Refusal), bDual ? 1 : 0));

				State->MeasuredLockout = W->GetShootLockoutRemaining();
				State->List.Check(bDual
						? TEXT("s10: swinging locks out shooting for the ANIMATION's length")
						: TEXT("v31 s1: there is NO shooting lockout — the rule has no reachable state"),
					bDual
						? (FMath::IsNearlyEqual(State->MeasuredLockout, TraceMelee::GetSwingAnimSeconds(), 0.06f) && !W->CanFire())
						: FMath::IsNearlyZero(State->MeasuredLockout, 0.001f),
					FString::Printf(TEXT("lockout %.3fs vs SwingAnimSeconds %.3fs, CanFire=%d, dualWield=%d"),
						State->MeasuredLockout, TraceMelee::GetSwingAnimSeconds(), W->CanFire() ? 1 : 0,
						bDual ? 1 : 0));

				// Wait out the animation and a little more, then prove the gate REOPENS. A lockout
				// that never lifts would pass the check above and be a far worse bug.
				State->Advance(NowReal, TraceMelee::GetSwingAnimSeconds() + 0.15);
				break;
			}

			case 4:
			{
				State->List.Check(TEXT("s10: ...and the trigger is free once the animation would have ended"),
					W->GetShootLockoutRemaining() <= 0.f,
					FString::Printf(TEXT("lockout %.3fs after waiting %.3fs"),
						W->GetShootLockoutRemaining(), TraceMelee::GetSwingAnimSeconds() + 0.15f));

				// --- THE OTHER ARM. The switch itself, forced to its OPPOSITE. ----------------------
				//
				// *** SPEC v31 §1: THIS FORCES !shipped RATHER THAN A HARD-CODED 0. *** It used to set
				// Trace.Knife.DualWield 0 unconditionally, which was the opposite arm only while the
				// shipped position was ON. Now that v31 §1 ships it OFF, a hard-coded 0 would set the
				// switch to the value it already had and the A/B would compare a run against itself —
				// which is exactly what "green 0.000s vs red 0.000s" was reporting.
				//
				// Forcing the opposite keeps the contrast real in both eras: whichever way the build
				// ships, this arm runs the other one and the verdict below asserts they DIFFER. A
				// harness whose two arms agree has stopped being a harness.
				const int32 OppositeArm = TraceMelee::IsDualWieldEnabled() ? 0 : 1;
				if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Knife.DualWield")))
				{
					Var->Set(OppositeArm, ECVF_SetByConsole);
				}
				// Under dual-wield ON this swings (the blade is permanent) and a lockout appears;
				// under OFF it is refused, because a gun is out and the knife is not the selection.
				TraceMelee::RequestSwing(TickPawn);
				State->LegacyLockout = W->GetShootLockoutRemaining();
				State->Advance(NowReal, 0.05);
				break;
			}

			default:
			{
				// THE VERDICT IS "THE TWO ARMS DIFFER", NOT "ONE OF THEM IS ZERO". Stated as a
				// difference so it holds in both eras: with dual-wield ON the shipped arm shows a
				// lockout and the forced-OFF arm shows none; with it OFF (v31 §1) the shipped arm
				// shows none and the forced-ON arm shows one. Either way exactly one of the two is
				// non-zero, and if BOTH read 0.000 the switch is not reaching the lockout at all —
				// which is the regression this line exists to catch.
				State->List.Check(TEXT("s10 A/B: the shooting lockout exists in exactly ONE of the two switch positions"),
					FMath::IsNearlyZero(FMath::Min(State->MeasuredLockout, State->LegacyLockout), 0.001f)
					&& FMath::Max(State->MeasuredLockout, State->LegacyLockout) > 0.05f,
					FString::Printf(TEXT("shipped arm %.3fs vs forced-opposite arm %.3fs (shipped switch is %s)"),
						State->MeasuredLockout, State->LegacyLockout,
						UTraceMeleeSettings::Get().bDualWieldKnife ? TEXT("ON") : TEXT("OFF — v31 s1")));
				State->List.Check(TEXT("s10 A/B: ...and the cvar really did move the switch for that arm"),
					TraceMelee::IsDualWieldEnabled() != UTraceMeleeSettings::Get().bDualWieldKnife,
					FString::Printf(TEXT("resolved=%d table=%d — they must DISAGREE while the arm is forced"),
						TraceMelee::IsDualWieldEnabled() ? 1 : 0,
						UTraceMeleeSettings::Get().bDualWieldKnife ? 1 : 0));

				// PUT IT BACK. Every exit path from here restores the shipped value; a harness that
				// leaves a cvar set is a harness that silently reverts the feature for the rest of
				// the session.
				if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Knife.DualWield")))
				{
					Var->Set(-1, ECVF_SetByConsole);
				}

				Report(State);
				return false;
			}
			}

			return true;
		}), 0.f);
	}
}

static FAutoConsoleCommand GTraceSmgDumpCmd(
	TEXT("Trace.Smg.Dump"),
	TEXT("Dev only. Spec v28 s9: print the SMG's resolved damage, fire rate (interval AND RPM), clip and "
	     "reload beside the pistol's, plus the shared pullout time. Reads the running game, not the headers."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		TraceWeaponsV28::FChecklist Ignored;
		TraceWeaponsV28::DumpAndCheckNumbers(Ignored);
	}));

static FAutoConsoleCommand GTraceWeaponsV28Cmd(
	TEXT("Trace.Weapons.V28"),
	TEXT("Dev only, SERVER. Spec v28 s9+s10: assert every SMG number, prove the per-character fire-rate seam "
	     "applies to it, swap pistol<->SMG through the shipped 1/2 verbs and prove each gun keeps its own "
	     "magazine, then swing and measure the shooting lockout against SwingAnimSeconds — with the v28 switch "
	     "itself as the RED arm."),
	FConsoleCommandDelegate::CreateStatic(&TraceWeaponsV28::Run));


// =================================================================================================
// Trace.Weapons.V29 — the unattended proof for spec v29 §2.
//
// FIVE ITEMS, AND FOUR OF THEM ARE MEASURED RATHER THAN READ BACK.
//
//   2a  the pistol's leg shot is 25            asserted in TraceWeaponsV28::DumpAndCheckNumbers,
//                                              which now names the number instead of explaining why
//                                              it could not.
//   2b  the pistol is NOT full auto            MEASURED: hold the trigger for four pistol intervals
//                                              and count the rounds that left. RED ARM:
//                                              bPistolFullAuto=True, same hold, same count.
//   2c  the SMG reloads in 1.3 s               asserted in the same table.
//   2d  the SMG falls off past 800 uu          asserted one uu either side of the line, plus the
//                                              pistol at 30000 uu as the "SMG only" control.
//   2e  Roxie's MODDED adds recoil             MEASURED: the actual CONTROL PITCH climb over a live
//                                              burst on a live Roxie with MODDED up. RED ARM:
//                                              RoxieModdedRecoilScale=0, same burst.
//   2f  the SMG fires at 600 RPM, not 537      MEASURED over 40+ consecutive rounds of ONE held
//                                              trigger, at a PINNED FRAME RATE, with the shipped
//                                              build's own behaviour (carry 0) as the RED ARM.
//
// *** WHY THE FRAME RATE IS PINNED, AND WHY THAT IS THE OPPOSITE OF CHEATING. *** §2f is a frame
// quantisation bug: the old code stamped the FRAME a round left on, not the instant it was due, so
// the cadence was dt * ceil(Interval / dt). That expression is exactly 0.1 s whenever dt divides
// 0.1 s — at 60 fps (6 frames), at 100, at 50, at 20 — so on a machine sitting at 60 fps the bug is
// INVISIBLE and a harness run there would report a clean 600 RPM before the fix and after it. The
// owner measured 0.1117 s, which is 6 x 1/53.7: their machine was at ~54 fps. So the harness pins
// t.MaxFPS to a rate whose frame time does NOT divide the interval, reproduces the reported number
// first, and only then measures the fix. Un-pinning it is what would make the run meaningless.
//
// *** AND IT IS WHY THE EXISTING HARNESS NEVER CAUGHT THIS. *** Trace.FireRate.Measure asserts the
// measured interval to within 12%, with a comment saying the tolerance "covers the frame
// quantisation at both ends of a ~7-round window". 537 RPM is 10.5% off 600. The bug fitted inside
// the tolerance that was written to accommodate it, on a 7-round window. This one measures 40+
// rounds and asserts 1.5%, which is what makes it able to fail.
// =================================================================================================

namespace TraceWeaponsV29
{
	using TraceWeaponsV28::FindLocalPawn;

	/** One assertion, printed the moment it is made, exactly as the v28 list does. */
	struct FChecklist
	{
		int32 Passed = 0;
		int32 Failed = 0;

		void Check(const TCHAR* What, bool bOk, const FString& Detail)
		{
			if (bOk) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[V29] %s  %s  (%s)"),
				bOk ? TEXT("PASS") : TEXT("**FAIL**"), What, *Detail);
		}
	};

	/** Seconds of held trigger per fire-rate window. 8 s of SMG is ~62 rounds, ~2 magazines. */
	constexpr double RateWindowSeconds = 8.0;

	/** At least this many usable gaps or the window is not evidence. §2f: "at least 40 rounds". */
	constexpr int32 MinIntervalSamples = 40;

	/**
	 * The frame rate the fire-rate window is pinned to.
	 *
	 * 53.7 fps is the owner's machine, recovered from their own number: 0.1117 / 6 = 0.018617 s. The
	 * harness uses 54, whose frame time (0.018519 s) also fails to divide 0.1 s — 6 frames = 0.1111 s
	 * = 540 RPM — so the RED arm reproduces the report to within 3 RPM and the GREEN arm has somewhere
	 * to move to. Any rate whose period does not divide the interval would do; this one is the
	 * reported one.
	 */
	constexpr float RateWindowFPS = 54.f;

	/** One measured distribution. A mean alone cannot tell a slow gun from a stuttering one. */
	struct FStats
	{
		int32  Samples = 0;
		int32  Dropped = 0;        // gaps discarded as reload-spanning
		double Mean = 0.0;
		double Min = 0.0;
		double Max = 0.0;
		double StdDev = 0.0;
		double MeanFrameSeconds = 0.0;

		double RPM() const { return (Mean > 0.0) ? (60.0 / Mean) : -1.0; }
		bool IsUsable() const { return Samples >= MinIntervalSamples; }
	};

	/**
	 * Turns a list of shot stamps into a distribution.
	 *
	 * @param DropAbove  gaps longer than this are RELOADS, not cadence, and are counted separately
	 *                   rather than averaged in. Passing 0 keeps every gap.
	 *
	 * The exclusion is stated in the report as a number, never silently: "39 gaps, 1 dropped" is a
	 * full magazine and a reload, and any other pattern of drops is itself the finding.
	 */
	FStats Summarise(const TArray<double>& Stamps, double DropAbove)
	{
		FStats Out;
		TArray<double> Gaps;
		Gaps.Reserve(FMath::Max(0, Stamps.Num() - 1));
		for (int32 Index = 1; Index < Stamps.Num(); ++Index)
		{
			const double Gap = Stamps[Index] - Stamps[Index - 1];
			if (DropAbove > 0.0 && Gap > DropAbove)
			{
				++Out.Dropped;
				continue;
			}
			Gaps.Add(Gap);
		}

		Out.Samples = Gaps.Num();
		if (Out.Samples == 0)
		{
			return Out;
		}

		Out.Min = TNumericLimits<double>::Max();
		Out.Max = 0.0;
		double Sum = 0.0;
		for (const double Gap : Gaps)
		{
			Sum += Gap;
			Out.Min = FMath::Min(Out.Min, Gap);
			Out.Max = FMath::Max(Out.Max, Gap);
		}
		Out.Mean = Sum / static_cast<double>(Out.Samples);

		double SumSq = 0.0;
		for (const double Gap : Gaps)
		{
			SumSq += (Gap - Out.Mean) * (Gap - Out.Mean);
		}
		Out.StdDev = FMath::Sqrt(SumSq / static_cast<double>(Out.Samples));
		return Out;
	}

	void LogStats(const TCHAR* Arm, const FStats& S, double ExpectedInterval)
	{
		if (!S.IsUsable())
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[V29] %-24s NOT EVIDENCE — only %d usable gaps (needed %d). Was the trigger held? Was the "
				     "pawn alive, un-stowed and not carrying the Core?"),
				Arm, S.Samples, MinIntervalSamples);
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[V29] %-24s %d gaps (%d dropped as reloads) | mean %.5fs = %.1f RPM | min %.5f max %.5f "
			     "sd %.5f | expected %.5fs = %.1f RPM | error %+.2f%% | frame %.5fs = %.1f fps"),
			Arm, S.Samples, S.Dropped, S.Mean, S.RPM(), S.Min, S.Max, S.StdDev,
			ExpectedInterval, 60.0 / FMath::Max(1.0e-6, ExpectedInterval),
			100.0 * (S.Mean - ExpectedInterval) / FMath::Max(1.0e-6, ExpectedInterval),
			S.MeanFrameSeconds, 1.0 / FMath::Max(1.0e-6, S.MeanFrameSeconds));
	}

	/** Everything the run has to put back, however it exits. */
	struct FRestore
	{
		float CarryFraction = 0.2f;
		bool  bPistolFullAuto = false;
		float RoxieRecoil = 1.f;
		int32 RecordShots = 0;
		float MaxFPS = 0.f;
		ETraceCharacterId Character = ETraceCharacterId::None;
		bool  bCharacterChanged = false;
	};

	void SetCVarInt(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	float GetCVarFloat(const TCHAR* Name, float Fallback)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return (Var != nullptr) ? Var->GetFloat() : Fallback;
	}

	void SetCVarFloat(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	struct FState
	{
		int32  Step = 0;

		/**
		 * *** THE WORLD CLOCK, NOT THE WALL CLOCK, AND THAT IS LOAD-BEARING. *** Every gate this
		 * harness measures — the fire rate, MODDED's 5 s, the reload — is on UWorld::GetTimeSeconds,
		 * and under -UseFixedTimeStep (which is how the fire-rate windows are meant to be run) the
		 * two clocks diverge by whatever the machine's real frame rate happens to be. A wall-clock
		 * schedule would then hold the trigger for 5 s of MODDED and call it 0.7.
		 */
		double NextStepWorldTime = 0.0;
		double WindowStartWorld = 0.0;
		int32  WindowStartFrame = 0;
		FChecklist List;
		FRestore Restore;

		FStats RedRate;      // §2f, carry 0 — the shipped 537 RPM behaviour
		FStats GreenRate;    // §2f, carry as shipped
		double ExpectedSmgInterval = 0.1;

		int32  SemiRounds = -1;    // §2b, pistol, one held trigger
		int32  AutoRounds = -1;    // §2b RED ARM, same hold with bPistolFullAuto on
		int32  RearmRounds = -1;   // §2b, release and press again

		double RoxieClimbGreen = -1.0;   // §2e, degrees of control pitch gained over a burst
		double RoxieClimbRed = -1.0;     // §2e RED ARM, RoxieModdedRecoilScale 0
		double RoxieScaleSeen = -1.0;
		bool   bRoxieStaged = false;
		bool   bRoxieFullAutoOnPistol = false;

		FTSTicker::FDelegateHandle Handle;

		void Advance(double NowWorld, double Seconds) { NextStepWorldTime = NowWorld + Seconds; ++Step; }
	};

	/** Puts every knob and cvar back, whatever happened. Called from every exit. */
	void RestoreAll(const TSharedRef<FState>& State)
	{
		if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
		{
			Mutable->FireIntervalCarryFraction = State->Restore.CarryFraction;
			Mutable->bPistolFullAuto           = State->Restore.bPistolFullAuto;
			Mutable->RoxieModdedRecoilScale    = State->Restore.RoxieRecoil;
		}
		SetCVarInt(TEXT("Trace.Weapons.RecordShots"), State->Restore.RecordShots);
		SetCVarFloat(TEXT("t.MaxFPS"), State->Restore.MaxFPS);

		// The CHARACTER is deliberately NOT put back. ServerSetCharacter enforces per-team uniqueness
		// and a bot may have taken the original in the seconds this run took; failing to restore is
		// visible and harmless, while a refused restore that went unreported would look like the
		// harness had corrupted the roster. Said here rather than attempted and hoped over.
	}

	void Report(const TSharedRef<FState>& State)
	{
		RestoreAll(State);

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE WEAPONS v29 — VERDICT =========="));
		LogStats(TEXT("2f RED (carry 0)"), State->RedRate, State->ExpectedSmgInterval);
		LogStats(TEXT("2f GREEN (shipped)"), State->GreenRate, State->ExpectedSmgInterval);
		UE_LOG(LogTraceGame, Display,
			TEXT("[V29] 2b: one held trigger on the PISTOL fired %d round(s) [expect 1]; the same hold with "
			     "bPistolFullAuto=True fired %d [expect 3+]; releasing and pressing again fired %d more [expect 1]."),
			State->SemiRounds, State->AutoRounds, State->RearmRounds);
		UE_LOG(LogTraceGame, Display,
			TEXT("[V29] 2e: Roxie MODDED climbed %.3f deg of control pitch over a burst; with "
			     "RoxieModdedRecoilScale=0 the same burst climbed %.3f deg. Seam read x%.3f."),
			State->RoxieClimbGreen, State->RoxieClimbRed, State->RoxieScaleSeen);

		UE_LOG(LogTraceGame, Display, TEXT("[V29] %d passed, %d FAILED."), State->List.Passed, State->List.Failed);
		UE_LOG(LogTraceGame, Display, TEXT("[V29] RESULT: %s"),
			State->List.Failed == 0 ? TEXT("PASS") : TEXT("*** FAIL ***"));
		UE_LOG(LogTraceGame, Display, TEXT("================================================"));
	}

	/** Begins a held-trigger window: pins the frame rate, clears the ring, holds the trigger. */
	void BeginWindow(const TSharedRef<FState>& State, UTraceWeaponComponent* W, UWorld* WorldPtr)
	{
		SetCVarFloat(TEXT("t.MaxFPS"), RateWindowFPS);
		SetCVarInt(TEXT("Trace.Weapons.RecordShots"), 1);
		W->ClearRecordedShotTimes();
		State->WindowStartWorld = WorldPtr->GetTimeSeconds();
		State->WindowStartFrame = static_cast<int32>(GFrameCounter);
		W->StartFire();
	}

	/** Ends it and summarises, folding in the frame time the window actually ran at. */
	FStats EndWindow(const TSharedRef<FState>& State, UTraceWeaponComponent* W, UWorld* WorldPtr, double DropAbove)
	{
		W->StopFire();
		FStats Out = Summarise(W->GetRecordedShotTimes(), DropAbove);

		const int32 Frames = FMath::Max(1, static_cast<int32>(GFrameCounter) - State->WindowStartFrame);
		Out.MeanFrameSeconds = (WorldPtr->GetTimeSeconds() - State->WindowStartWorld) / static_cast<double>(Frames);
		return Out;
	}

	void Run()
	{
		ATraceCharacter* Pawn = FindLocalPawn();
		UTraceWeaponComponent* Weapon = (Pawn != nullptr) ? Pawn->Weapon.Get() : nullptr;
		if (Pawn == nullptr || Weapon == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[V29] No local pawn with a weapon component yet. Run this from a live match; the character "
				     "select screen holds the pawn back for the first few seconds."));
			return;
		}
		if (!Pawn->HasAuthority())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[V29] SERVER ONLY (a listen host's own pawn counts). The magazine, the ability seam and the "
				     "recoil arm all read authoritative state a client does not own."));
			return;
		}

		TSharedRef<FState> State = MakeShared<FState>();

		const UTraceSettings& Live = UTraceSettings::Get();
		State->Restore.CarryFraction   = Live.FireIntervalCarryFraction;
		State->Restore.bPistolFullAuto = Live.bPistolFullAuto;
		State->Restore.RoxieRecoil     = Live.RoxieModdedRecoilScale;
		State->Restore.MaxFPS = GetCVarFloat(TEXT("t.MaxFPS"), 0.f);
		if (const IConsoleVariable* Rec = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Weapons.RecordShots")))
		{
			State->Restore.RecordShots = Rec->GetInt();
		}

		// --- the half that needs no ticking: every number this pass moved --------------------------
		TraceWeaponsV28::FChecklist Table;
		TraceWeaponsV28::DumpAndCheckNumbers(Table);
		State->List.Check(TEXT("v29 2a/2c/2d: every table number resolves to the spec's value"),
			Table.Failed == 0,
			FString::Printf(TEXT("%d passed, %d failed in the table above"), Table.Passed, Table.Failed));

		// THE SEAM, WITHOUT FIRING A ROUND. On an ordinary pawn the recoil scale must be exactly the
		// global switch's contribution — which is 0, because Demo 22 removed recoil. If this reads
		// anything else, §2e has leaked out of Roxie and onto everybody.
		State->List.Check(TEXT("v29 2e: an ordinary pawn still has NO recoil (Demo 22 stands)"),
			FMath::IsNearlyZero(Weapon->GetRecoilPitchScale(), 0.0001)
			&& !UTraceSettings::Get().bRecoilEnabled,
			FString::Printf(TEXT("scale %.4f, bRecoilEnabled=%d"),
				Weapon->GetRecoilPitchScale(), UTraceSettings::Get().bRecoilEnabled ? 1 : 0));

		State->ExpectedSmgInterval = static_cast<double>(TraceAmmo::GetBaseFireInterval(ETraceEquippedWeapon::Smg));

		// *** SAY WHICH INSTRUMENT IS IN USE, BECAUSE THEY ARE NOT EQUALLY GOOD. ***
		//
		//   -UseFixedTimeStep -FPS=54   every frame advances the world by exactly 1/54 s whatever the
		//                               machine is doing, so the measurement is immune to whatever
		//                               else is running. THIS IS THE ONE TO USE. The gun gates on
		//                               UWorld::GetTimeSeconds, which is exactly what this pins.
		//   t.MaxFPS 54                 a CEILING on the real frame rate, and a busy machine simply
		//                               fails to reach it. A first run of this harness measured
		//                               49.5 fps against a 54 pin because another process had the CPU
		//                               — and 49.5 is below the floor at which 600 RPM is reachable
		//                               at all, so the run could not answer the question it asked.
		UE_LOG(LogTraceGame, Display,
			TEXT("[V29] staged half starting. Fixed timestep: %s (%.4fs/frame). Also pinning t.MaxFPS to %.1f — "
			     "the frame rate recovered from the owner's own 0.1117s measurement (0.1117/6 = 1/53.7) — because "
			     "the bug is INVISIBLE at any frame rate whose period divides the fire interval, 60 fps included. "
			     "*** RUN THIS WITH -UseFixedTimeStep -FPS=54 ***; without it a busy machine can miss the pin and "
			     "the fire-rate windows will say so rather than answer."),
			FApp::UseFixedTimeStep() ? TEXT("ON") : TEXT("off"),
			FApp::UseFixedTimeStep() ? FApp::GetFixedDeltaTime() : 0.0,
			RateWindowFPS);

		State->Handle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float /*Delta*/) -> bool
		{
			ATraceCharacter* TickPawn = FindLocalPawn();
			UTraceWeaponComponent* W = (TickPawn != nullptr) ? TickPawn->Weapon.Get() : nullptr;
			UWorld* WorldPtr = (TickPawn != nullptr) ? TickPawn->GetWorld() : nullptr;
			if (TickPawn == nullptr || W == nullptr || WorldPtr == nullptr || !TickPawn->IsAlive())
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[V29] ABORTED: the local pawn stopped being usable mid-run."));
				if (W != nullptr) { W->StopFire(); }
				Report(State);
				return false;
			}

			const double NowWorld = WorldPtr->GetTimeSeconds();
			if (NowWorld < State->NextStepWorldTime)
			{
				return true;
			}

			switch (State->Step)
			{
			case 0:
			{
				// The SMG, through the shipped verb.
				ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
				TraceMelee::RequestEquipIfDifferent(TickPawn, ETraceEquippedWeapon::Smg, &Refusal);
				State->Advance(NowWorld, TraceMelee::GetSwapSeconds() + 0.35);
				break;
			}

			case 1:
			{
				State->List.Check(TEXT("v29 2f: the SMG is in hand for the fire-rate windows"),
					W->IsSmgEquipped(),
					FString::Printf(TEXT("weapon=%s"), LexToString(W->GetEquippedWeapon())));

				// --- THE RED ARM FIRST. Reproduce before fixing, on the same harness. --------------
				if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
				{
					Mutable->FireIntervalCarryFraction = 0.f;
				}
				BeginWindow(State, W, WorldPtr);
				State->Advance(NowWorld, RateWindowSeconds);
				break;
			}

			case 2:
			{
				// Gaps longer than twice the interval are the 1.3 s reload, not cadence. Counted and
				// reported, never averaged in — and the count is itself a check, because a window that
				// dropped ten gaps was not one held trigger.
				State->RedRate = EndWindow(State, W, WorldPtr, State->ExpectedSmgInterval * 2.0);
				LogStats(TEXT("2f RED (carry 0)"), State->RedRate, State->ExpectedSmgInterval);

				if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
				{
					Mutable->FireIntervalCarryFraction = State->Restore.CarryFraction;
				}
				// A beat for the automatic reload to finish before the next window starts, so the
				// green arm is not measuring a magazine change it did not ask for.
				State->Advance(NowWorld, 2.0);
				break;
			}

			case 3:
			{
				BeginWindow(State, W, WorldPtr);
				State->Advance(NowWorld, RateWindowSeconds);
				break;
			}

			case 4:
			{
				State->GreenRate = EndWindow(State, W, WorldPtr, State->ExpectedSmgInterval * 2.0);
				LogStats(TEXT("2f GREEN (shipped)"), State->GreenRate, State->ExpectedSmgInterval);
				SetCVarInt(TEXT("Trace.Weapons.RecordShots"), 0);
				SetCVarFloat(TEXT("t.MaxFPS"), State->Restore.MaxFPS);

				State->List.Check(TEXT("v29 2f: the window is at least 40 consecutive rounds, both arms"),
					State->RedRate.IsUsable() && State->GreenRate.IsUsable(),
					FString::Printf(TEXT("red %d gaps, green %d gaps (needed %d each)"),
						State->RedRate.Samples, State->GreenRate.Samples, MinIntervalSamples));

				if (State->RedRate.IsUsable() && State->GreenRate.IsUsable())
				{
					const double Expected = State->ExpectedSmgInterval;
					const double GreenErr = FMath::Abs(State->GreenRate.Mean - Expected) / Expected;
					const double RedErr = FMath::Abs(State->RedRate.Mean - Expected) / Expected;

					// *** THE PRECONDITION, AND IT IS DERIVED RATHER THAN GUESSED. ***
					//
					// A per-frame poll can only put a round on a frame boundary, and the carry can pull
					// back at most CarryCap of the overshoot per round. So the quantisation is fully
					// absorbed exactly while the FRAME TIME is no longer than the CAP, and beyond that
					// the gun is frame-rate bound however the code is written:
					//
					//     cap = Interval x FireIntervalCarryFraction = 0.1 x 0.2 = 0.020 s  ->  50 fps
					//
					// Simulated 20-240 fps with and without frame jitter: at or above 50 fps the mean
					// lands within 0.13% of the knob; at 28 fps it is 4-7% slow, which is exactly what
					// this harness measured on its first (CPU-contended) run. So a run below the floor
					// CANNOT distinguish a broken fix from a busy machine, and it says so as a FAILURE
					// of the run rather than quietly passing or quietly failing the build.
					//
					// AND THE FLOOR IS A REAL GAMEPLAY FACT, NOT A HARNESS ARTEFACT: below ~50 fps the
					// SMG cannot sustain 600 RPM at all, because the only faster arrangement of rounds
					// on frame boundaries has gaps under the server's 0.08 s floor and would be
					// rejected as rate-limited. Curing that needs more than one round per frame AND a
					// wider FireRateTolerance — a change to the anti-cheat gate, deliberately not made
					// here. It is in the report.
					const double CarryCap = Expected
						* FMath::Clamp(static_cast<double>(UTraceSettings::Get().FireIntervalCarryFraction),
							0.0, UTraceWeaponComponent::FireRateTolerance);
					const bool bFastEnough = State->GreenRate.MeanFrameSeconds <= CarryCap + 1.0e-5;

					State->List.Check(TEXT("v29 2f PRECONDITION: the machine ran fast enough for 600 RPM to be reachable"),
						bFastEnough,
						FString::Printf(TEXT("frame time %.5fs (%.1f fps) against the %.5fs (%.1f fps) floor that "
						                     "Interval x carry sets. Below it the poll is frame-rate bound and no "
						                     "measurement here can mean anything — re-run on an idle machine."),
							State->GreenRate.MeanFrameSeconds, 1.0 / FMath::Max(1.0e-6, State->GreenRate.MeanFrameSeconds),
							CarryCap, 1.0 / FMath::Max(1.0e-6, CarryCap)));

					if (bFastEnough)
					{
						State->List.Check(TEXT("v29 2f: the SMG now fires at the knob — within 1.5% over 40+ rounds"),
							GreenErr <= 0.015,
							FString::Printf(TEXT("measured %.5fs = %.1f RPM against %.5fs = %.1f RPM (%+.2f%%)"),
								State->GreenRate.Mean, State->GreenRate.RPM(), Expected, 60.0 / Expected,
								100.0 * (State->GreenRate.Mean - Expected) / Expected));
					}
					else
					{
						UE_LOG(LogTraceGame, Error,
							TEXT("[V29] 2f NOT MEASURED: green read %.5fs (%.1f RPM), which is what a %.1f fps poll "
							     "can achieve and NOT a statement about the fix. The frame-quantised ceiling at this "
							     "frame time is %.5fs. Re-run with nothing else on the machine."),
							State->GreenRate.Mean, State->GreenRate.RPM(),
							1.0 / FMath::Max(1.0e-6, State->GreenRate.MeanFrameSeconds),
							State->GreenRate.MeanFrameSeconds
								* FMath::CeilToDouble((Expected - CarryCap) / FMath::Max(1.0e-6, State->GreenRate.MeanFrameSeconds)));
					}

					// *** THE RED ARM HAS TO HAVE REPRODUCED, OR THIS PROVES NOTHING. *** The house
					// rule is explicit: two arms agreeing means the harness is not measuring its rule.
					// It is reported as a WARNING rather than a failure when the frame rate refuses to
					// cooperate, because that is a fact about the machine and not about the fix — but
					// it is never silent, and the run says which of the two it was.
					if (RedErr <= 0.015)
					{
						UE_LOG(LogTraceGame, Warning,
							TEXT("[V29] *** THE RED ARM DID NOT REPRODUCE. *** carry 0 measured %.5fs (%.1f RPM), "
							     "which is already correct, so this run does NOT prove the fix — it proves the "
							     "machine's frame time (%.5fs) divides the fire interval (%.5fs) and the bug is "
							     "invisible here. Re-run with t.MaxFPS at a rate whose period does not divide it."),
							State->RedRate.Mean, State->RedRate.RPM(), State->RedRate.MeanFrameSeconds, Expected);
					}
					State->List.Check(TEXT("v29 2f RED ARM: carry 0 reproduces the shipped 537-RPM shortfall"),
						RedErr > 0.015 && State->RedRate.Mean > State->GreenRate.Mean,
						FString::Printf(TEXT("red %.5fs = %.1f RPM (%+.2f%%) vs green %.5fs = %.1f RPM (%+.2f%%), "
						                     "at a frame time of %.5fs"),
							State->RedRate.Mean, State->RedRate.RPM(), 100.0 * (State->RedRate.Mean - Expected) / Expected,
							State->GreenRate.Mean, State->GreenRate.RPM(), 100.0 * (State->GreenRate.Mean - Expected) / Expected,
							State->RedRate.MeanFrameSeconds));

					// The distribution, not just the mean: a gun that is right on average and wrong
					// every sixth round is a different bug, and only the spread can tell them apart.
					State->List.Check(TEXT("v29 2f: no round is early enough for the server to reject it"),
						State->GreenRate.Min >= Expected * (1.0 - 0.2) - 1.0e-6,
						FString::Printf(TEXT("shortest gap %.5fs against the server's %.5fs floor "
						                     "(FireRateTolerance 20%%)"),
							State->GreenRate.Min, Expected * 0.8));
				}

				// --- 2b. The pistol. ---------------------------------------------------------------
				ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
				TraceMelee::RequestEquipIfDifferent(TickPawn, ETraceEquippedWeapon::Gun, &Refusal);
				State->Advance(NowWorld, TraceMelee::GetSwapSeconds() + 0.35);
				break;
			}

			case 5:
			{
				State->List.Check(TEXT("v29 2b: the pistol is in hand and reports SEMI-AUTO"),
					W->IsFirearmEquipped() && !W->IsSmgEquipped() && !W->IsFullAutoNow(),
					FString::Printf(TEXT("weapon=%s fullAutoNow=%d bPistolFullAuto=%d"),
						LexToString(W->GetEquippedWeapon()), W->IsFullAutoNow() ? 1 : 0,
						UTraceSettings::Get().bPistolFullAuto ? 1 : 0));

				SetCVarInt(TEXT("Trace.Weapons.RecordShots"), 1);
				W->ClearRecordedShotTimes();
				W->StartFire();
				// FOUR pistol intervals of held trigger. Long enough that an automatic pistol could
				// not possibly fire only once, short enough to stay inside one magazine.
				State->Advance(NowWorld, 4.0 * W->GetFireInterval());
				break;
			}

			case 6:
			{
				W->StopFire();
				State->SemiRounds = W->GetRecordedShotTimes().Num();
				State->List.Check(TEXT("v29 2b: ONE round per trigger press — four intervals of held trigger fired 1"),
					State->SemiRounds == 1,
					FString::Printf(TEXT("%d round(s) over %.3fs of held trigger at a %.4fs interval"),
						State->SemiRounds, 4.0 * W->GetFireInterval(), W->GetFireInterval()));

				// ...and the RELEASE rearms it. A latch that never cleared would pass the check above
				// and leave the pistol able to fire exactly once per life.
				W->ClearRecordedShotTimes();
				W->StartFire();
				State->Advance(NowWorld, 0.2);
				break;
			}

			case 7:
			{
				W->StopFire();
				State->RearmRounds = W->GetRecordedShotTimes().Num();
				State->List.Check(TEXT("v29 2b: releasing and pressing again fires the next round"),
					State->RearmRounds == 1,
					FString::Printf(TEXT("%d round(s) on the second press"), State->RearmRounds));

				// --- THE RED ARM: the v28 pistol. --------------------------------------------------
				if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
				{
					Mutable->bPistolFullAuto = true;
				}
				W->ClearRecordedShotTimes();
				W->StartFire();
				State->Advance(NowWorld, 4.0 * W->GetFireInterval());
				break;
			}

			case 8:
			{
				W->StopFire();
				State->AutoRounds = W->GetRecordedShotTimes().Num();
				if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
				{
					Mutable->bPistolFullAuto = State->Restore.bPistolFullAuto;
				}

				State->List.Check(TEXT("v29 2b RED ARM: with bPistolFullAuto=True the same hold empties rounds"),
					State->AutoRounds >= 3 && State->SemiRounds == 1,
					FString::Printf(TEXT("full auto fired %d, semi fired %d, over the same %.3fs hold"),
						State->AutoRounds, State->SemiRounds, 4.0 * W->GetFireInterval()));

				// --- 2e. Roxie. ---------------------------------------------------------------------
				UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(TickPawn);
				if (Abilities == nullptr)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[V29] no ability component on the local pawn — §2e cannot be measured on a live "
						     "Roxie here. The seam is still asserted below."));
					State->Step = 12;
					State->NextStepWorldTime = NowWorld;
					break;
				}

				State->Restore.Character = Abilities->GetCharacterId();
				if (Abilities->GetCharacterId() != ETraceCharacterId::Roxie)
				{
					Abilities->ServerSetCharacter(ETraceCharacterId::Roxie);
					State->Restore.bCharacterChanged = true;
				}
				State->bRoxieStaged = (Abilities->GetCharacterId() == ETraceCharacterId::Roxie);
				State->Advance(NowWorld, 0.6);
				break;
			}

			case 9:
			{
				UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(TickPawn);
				if (Abilities == nullptr || !State->bRoxieStaged)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[V29] the roster refused Roxie (per-team uniqueness — a team-mate holds her). §2e's "
						     "LIVE arm is skipped and SAYS SO; the seam is still asserted."));
					State->Step = 12;
					State->NextStepWorldTime = NowWorld;
					break;
				}

				Abilities->DebugSetActivatedCooldown(0.f);
				const bool bUp = Abilities->TryActivate();
				State->RoxieScaleSeen = W->GetRecoilPitchScale();

				State->List.Check(TEXT("v29 2e: MODDED up puts recoil on Roxie's gun and on nobody else's"),
					bUp && FMath::IsNearlyEqual(State->RoxieScaleSeen,
						static_cast<double>(UTraceSettings::Get().RoxieModdedRecoilScale), 0.001),
					FString::Printf(TEXT("activated=%d, scale %.4f vs knob %.4f (global switch contributes %d)"),
						bUp ? 1 : 0, State->RoxieScaleSeen, UTraceSettings::Get().RoxieModdedRecoilScale,
						UTraceSettings::Get().bRecoilEnabled ? 1 : 0));

				// §2b x §18 §2: MODDED's "the gun becomes full auto" clause, now that there is a
				// semi-automatic gun for it to act on.
				State->bRoxieFullAutoOnPistol = W->IsFullAutoNow() && !W->IsSmgEquipped();
				State->List.Check(TEXT("v29 2b: MODDED makes even the SEMI-AUTO pistol full auto (spec v18 §2)"),
					State->bRoxieFullAutoOnPistol,
					FString::Printf(TEXT("weapon=%s fullAutoNow=%d bRoxieModdedFullAuto=%d"),
						LexToString(W->GetEquippedWeapon()), W->IsFullAutoNow() ? 1 : 0,
						UTraceSettings::Get().bRoxieModdedFullAuto ? 1 : 0));

				// Measure the CLIMB, not the knob: hold the trigger and read the control pitch the
				// player's own view actually ended up at.
				if (APlayerController* PC = Cast<APlayerController>(TickPawn->GetController()))
				{
					State->RoxieClimbGreen = -FRotator::NormalizeAxis(PC->GetControlRotation().Pitch);
				}
				W->ClearRecordedShotTimes();
				W->StartFire();
				State->Advance(NowWorld, 0.7);
				break;
			}

			case 10:
			{
				W->StopFire();
				const int32 Rounds = W->GetRecordedShotTimes().Num();
				if (APlayerController* PC = Cast<APlayerController>(TickPawn->GetController()))
				{
					State->RoxieClimbGreen += FRotator::NormalizeAxis(PC->GetControlRotation().Pitch);
				}

				State->List.Check(TEXT("v29 2e: a MODDED burst actually CLIMBS the view"),
					State->RoxieClimbGreen > 0.3 && Rounds >= 2,
					FString::Printf(TEXT("%.3f deg over %d rounds (RecoilPitchPerShot %.3f x scale %.2f)"),
						State->RoxieClimbGreen, Rounds, UTraceSettings::Get().RecoilPitchPerShot,
						UTraceSettings::Get().RoxieModdedRecoilScale));

				// --- THE RED ARM: MODDED with the trade removed. -----------------------------------
				if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
				{
					Mutable->RoxieModdedRecoilScale = 0.f;
				}
				if (APlayerController* PC = Cast<APlayerController>(TickPawn->GetController()))
				{
					State->RoxieClimbRed = -FRotator::NormalizeAxis(PC->GetControlRotation().Pitch);
				}
				W->ClearRecordedShotTimes();
				W->StartFire();
				State->Advance(NowWorld, 0.7);
				break;
			}

			case 11:
			{
				W->StopFire();
				const int32 Rounds = W->GetRecordedShotTimes().Num();
				if (APlayerController* PC = Cast<APlayerController>(TickPawn->GetController()))
				{
					State->RoxieClimbRed += FRotator::NormalizeAxis(PC->GetControlRotation().Pitch);
				}
				if (UTraceSettings* Mutable = GetMutableDefault<UTraceSettings>())
				{
					Mutable->RoxieModdedRecoilScale = State->Restore.RoxieRecoil;
				}

				// The red arm's climb is allowed to be slightly NEGATIVE: the recovery from the green
				// arm's burst is still running underneath it. What it must not be is upward.
				State->List.Check(TEXT("v29 2e RED ARM: RoxieModdedRecoilScale=0 and the same burst does not climb"),
					State->RoxieClimbRed < 0.05 && Rounds >= 2 && State->RoxieClimbGreen > State->RoxieClimbRed,
					FString::Printf(TEXT("red %.3f deg over %d rounds vs green %.3f deg"),
						State->RoxieClimbRed, Rounds, State->RoxieClimbGreen));

				State->Advance(NowWorld, 0.05);
				break;
			}

			default:
			{
				SetCVarInt(TEXT("Trace.Weapons.RecordShots"), 0);
				Report(State);
				return false;
			}
			}

			return true;
		}), 0.f);
	}
}

static FAutoConsoleCommand GTraceWeaponsV29Cmd(
	TEXT("Trace.Weapons.V29"),
	TEXT("Dev only, SERVER. Spec v29 s2: assert the pistol's 25 leg / the SMG's 1.3s reload / the 800uu "
	     "falloff either side of the line, MEASURE the SMG's real cadence over 40+ consecutive rounds at a "
	     "pinned frame rate with the shipped 537-RPM behaviour as the RED arm, MEASURE that a held trigger "
	     "fires the pistol once (RED arm: bPistolFullAuto), and MEASURE the view climb of a live Roxie under "
	     "MODDED (RED arm: RoxieModdedRecoilScale 0)."),
	FConsoleCommandDelegate::CreateStatic(&TraceWeaponsV29::Run));

// =================================================================================================
// RESTRUCTURE C5 — THE SHIELD-BLOCKED HIT, MEASURED ON A REAL BULLET
//
//   Trace.Weapon.ShieldBlockTest   Three arms, one match, seconds apart. The first two shoot the
//   SAME enemy with the real fire key; the third drives the RPC this tranche extended.
//
//     SHIELDED  the enemy is given the Core (ETraceCoreGrantReason::Debug), stood on the floor in
//               front of the local player, and shot. *** THE MEASURED TRUTH IS THAT NO HIT IS
//               CONFIRMED AT ALL: *** UTraceLagCompensationComponent::ResolveHitscan skips a
//               shielded carrier as a candidate ("do not even resolve them", :323), so the shooter
//               gets silence rather than the lying white marker the F2 audit described. This arm
//               ASSERTS that silence, so the day the resolver changes, this test says so.
//     CONTROL   the Core is handed to somebody else and the same enemy is shot again. The hit must
//               confirm NORMALLY: health falls and the blocked counter does NOT move. Without this
//               arm "no hit was confirmed" and "the fixture never fired a bullet" are the same
//               reading, which is the failure mode this project has shipped before.
//     PLUMBING  ClientNotifyHit(bKilled=false, Zone, bShieldBlocked=true) is driven directly at the
//               local controller — the RPC C5 extended — and the controller must record the blocked
//               fact: the counter moves, WasLastHitMarkerShieldBlocked() reads true (the seam FX
//               plan §7.4 draws from) and the [ShieldBlock] line lands in the log.
//
//   In arms 1 and 2 only where the enemy STANDS and where the shooter LOOKS are staged — the key,
//   ServerFire, the lag-compensated resolver and ClientNotifyHit are all the shipping path. The
//   target is re-placed every frame for half a second before the shot, because the server resolves
//   against its RECORDED pose history and a target teleported and shot in the same tick is missed.
// =================================================================================================

namespace TraceShieldBlockTest
{
	/** Where the enemy is stood, in uu ahead of the shooter. Well inside the pistol's range. */
	constexpr double StagedDistanceUU = 500.0;

	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ATraceCharacter> Shooter;
		TWeakObjectPtr<ATraceCharacter> Target;
		TWeakObjectPtr<ATracePlayerController> ShooterPC;

		double StartTime = 0.0;
		int32 Phase = 0;

		int32 HitsBefore = 0;
		int32 BlockedBefore = 0;
		float TargetHealthBefore = 0.f;

		bool bBlockedArmOk = false;
		bool bControlArmOk = false;
		bool bPlumbingArmOk = false;
		FString BlockedLine;
		FString ControlLine;
		FString PlumbingLine;
	};

	static void PlaceAndAim(FRun& Run)
	{
		ATraceCharacter* Me = Run.Shooter.Get();
		ATraceCharacter* Target = Run.Target.Get();
		if (Me == nullptr || Target == nullptr)
		{
			return;
		}

		const FVector Forward = Me->GetActorForwardVector().GetSafeNormal2D();
		if (Forward.IsNearlyZero())
		{
			return;
		}

		FVector Spot = Me->GetActorLocation() + Forward * StagedDistanceUU;
		Spot.Z = Me->GetActorLocation().Z;   // the same floor the shooter is on, so nothing snaps
		Target->TeleportTo(Spot, (-Forward).Rotation(), /*bIsATest=*/false, /*bNoCheck=*/true);

		if (ATracePlayerController* PC = Run.ShooterPC.Get())
		{
			const FVector ToChest = Target->GetActorLocation() - Me->GetMuzzleLocation();
			if (!ToChest.IsNearlyZero())
			{
				PC->SetControlRotation(ToChest.Rotation());
			}
		}
	}

	static void Fire(FRun& Run)
	{
		if (ATracePlayerController* PC = Run.ShooterPC.Get())
		{
			// The real key, through the same injection path every other harness here uses.
			PC->ConsoleCommand(TEXT("Trace.SimInput LeftMouseButton 0.12 controller"), /*bWriteToLog=*/false);
		}
	}

	static float HealthOf(const ATraceCharacter* Who)
	{
		return (Who != nullptr && Who->Health != nullptr) ? Who->Health->Health : -1.f;
	}

	static void Run()
	{
		UWorld* World = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() != nullptr && Context.World()->IsGameWorld())
				{
					World = Context.World();
					break;
				}
			}
		}
		if (World == nullptr || World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[ShieldBlockTest] Server only, and needs a game world."));
			return;
		}

		APlayerController* PC = World->GetFirstPlayerController();
		ATraceCharacter* Me = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		ATracePlayerController* TracePC = Cast<ATracePlayerController>(PC);
		if (Me == nullptr || TracePC == nullptr || !Me->IsAlive())
		{
			UE_LOG(LogTraceGame, Error, TEXT("[ShieldBlockTest] no live local pawn."));
			return;
		}

		ATraceCharacter* Target = nullptr;
		double BestDistSq = TNumericLimits<double>::Max();
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Other = *It;
			if (Other == nullptr || Other == Me || !Other->IsAlive() || Other->GetTeam() == Me->GetTeam())
			{
				continue;
			}
			const double DistSq = FVector::DistSquared(Me->GetActorLocation(), Other->GetActorLocation());
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Target = Other;
			}
		}

		ATraceCore* TheCore = ATraceCore::Get(World);
		if (Target == nullptr || TheCore == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[ShieldBlockTest] need a living ENEMY and a Core (enemy=%s, core=%s). Run this in a "
				     "bot match."), *GetNameSafe(Target), *GetNameSafe(TheCore));
			return;
		}

		TSharedRef<FRun> Run = MakeShared<FRun>();
		Run->World = World;
		Run->Shooter = Me;
		Run->Target = Target;
		Run->ShooterPC = TracePC;
		Run->StartTime = FPlatformTime::Seconds();

		UE_LOG(LogTraceGame, Display,
			TEXT("[ShieldBlockTest] ===== shooter %s vs enemy %s; the Core goes to the enemy for arm 1 ====="),
			*GetNameSafe(Me), *GetNameSafe(Target));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float) -> bool
		{
			UWorld* PollWorld = Run->World.Get();
			ATraceCharacter* Shooter = Run->Shooter.Get();
			ATraceCharacter* Target = Run->Target.Get();
			ATracePlayerController* PC = Run->ShooterPC.Get();
			ATraceCore* TheCore = (PollWorld != nullptr) ? ATraceCore::Get(PollWorld) : nullptr;
			if (PollWorld == nullptr || Shooter == nullptr || Target == nullptr || PC == nullptr || TheCore == nullptr)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[ShieldBlockTest] a participant vanished mid-run; ABORTED."));
				return false;
			}

			const double Elapsed = FPlatformTime::Seconds() - Run->StartTime;

			switch (Run->Phase)
			{
			case 0:   // stage the CARRIER arm
				if (TheCore->GetCarrier() != Target)
				{
					TheCore->GrantTo(Target, ETraceCoreGrantReason::Debug);
				}
				PlaceAndAim(*Run);
				if (Elapsed > 0.6)
				{
					Run->HitsBefore = PC->DebugHitConfirmCount;
					Run->BlockedBefore = PC->DebugShieldBlockedHitCount;
					Run->TargetHealthBefore = HealthOf(Target);
					UE_LOG(LogTraceGame, Display,
						TEXT("[ShieldBlockTest] arm 1 FIRE: canFire=%d clip=%d dist=%.0f targetHealth=%.1f."),
						(Shooter->Weapon != nullptr && Shooter->Weapon->CanFire()) ? 1 : 0,
						(Shooter->Weapon != nullptr) ? Shooter->Weapon->GetClipAmmo() : -1,
						FVector::Dist(Shooter->GetActorLocation(), Target->GetActorLocation()),
						Run->TargetHealthBefore);
					Fire(*Run);
					Run->Phase = 1;
				}
				return true;

			case 1:   // let the shot resolve, keeping the target on the ray
				PlaceAndAim(*Run);
				if (Elapsed > 1.8)
				{
					const int32 Hits = PC->DebugHitConfirmCount - Run->HitsBefore;
					const int32 Blocked = PC->DebugShieldBlockedHitCount - Run->BlockedBefore;
					const float HealthNow = HealthOf(Target);
					const bool bCarrierStill = (TheCore->GetCarrier() == Target);
					// THE ASSERTION IS SILENCE: a shielded carrier is not a hitscan candidate, so the
					// shot must confirm NOTHING and must take no health. A hit here would mean the
					// resolver's carrier skip has changed — at which point the blocked flag beside it
					// becomes the live path and this expectation must be inverted deliberately.
					Run->bBlockedArmOk = bCarrierStill && Hits == 0 && Blocked == 0
						&& FMath::IsNearlyEqual(HealthNow, Run->TargetHealthBefore, 0.01f);
					Run->BlockedLine = FString::Printf(
						TEXT("carrier=%d hits=+%d blocked=+%d health %.1f -> %.1f (expected: no confirmation "
						     "at all — the resolver refuses a shielded carrier as a candidate)"),
						bCarrierStill ? 1 : 0, Hits, Blocked, Run->TargetHealthBefore, HealthNow);
					UE_LOG(LogTraceGame, Display, TEXT("[ShieldBlockTest] SHIELDED ARM: %s"), *Run->BlockedLine);

					// Take the Core away for the control arm — by GIVING IT TO SOMEBODY ELSE, never by
					// dropping it. ATraceCore::DropAt hands possession to the opposing team, which here
					// is the SHOOTER'S team: the shooter could end up carrying it, and a carrier cannot
					// fire (spec v16 §1), which would make the control arm fail for a reason that has
					// nothing to do with what is being measured.
					ATraceCharacter* Elsewhere = nullptr;
					for (TActorIterator<ATraceCharacter> It(PollWorld); It; ++It)
					{
						ATraceCharacter* Other = *It;
						if (Other != nullptr && Other != Shooter && Other != Target && Other->IsAlive())
						{
							Elsewhere = Other;
							break;
						}
					}
					if (Elsewhere != nullptr)
					{
						TheCore->GrantTo(Elsewhere, ETraceCoreGrantReason::Debug);
					}
					Run->Phase = 2;
				}
				return true;

			case 2:   // stage the CONTROL arm on the same enemy, now unshielded
				PlaceAndAim(*Run);
				if (Elapsed > 2.6)
				{
					Run->HitsBefore = PC->DebugHitConfirmCount;
					Run->BlockedBefore = PC->DebugShieldBlockedHitCount;
					Run->TargetHealthBefore = HealthOf(Target);
					UE_LOG(LogTraceGame, Display,
						TEXT("[ShieldBlockTest] arm 2 FIRE: canFire=%d clip=%d dist=%.0f targetHealth=%.1f carrier=%s."),
						(Shooter->Weapon != nullptr && Shooter->Weapon->CanFire()) ? 1 : 0,
						(Shooter->Weapon != nullptr) ? Shooter->Weapon->GetClipAmmo() : -1,
						FVector::Dist(Shooter->GetActorLocation(), Target->GetActorLocation()),
						Run->TargetHealthBefore, *GetNameSafe(TheCore->GetCarrier()));
					Fire(*Run);
					Run->Phase = 3;
				}
				return true;

			case 3:
				PlaceAndAim(*Run);
				if (Elapsed > 3.8)
				{
					const int32 Hits = PC->DebugHitConfirmCount - Run->HitsBefore;
					const int32 Blocked = PC->DebugShieldBlockedHitCount - Run->BlockedBefore;
					const float HealthNow = HealthOf(Target);
					const bool bNotCarrier = (TheCore->GetCarrier() != Target);
					Run->bControlArmOk = bNotCarrier && Hits >= 1 && Blocked == 0
						&& HealthNow < Run->TargetHealthBefore - 0.01f;
					Run->ControlLine = FString::Printf(
						TEXT("carrier=%d hits=+%d blocked=+%d health %.1f -> %.1f"),
						bNotCarrier ? 0 : 1, Hits, Blocked, Run->TargetHealthBefore, HealthNow);
					UE_LOG(LogTraceGame, Display, TEXT("[ShieldBlockTest] CONTROL ARM: %s"), *Run->ControlLine);

					// ---- ARM 3: the RPC this tranche extended, driven directly ---------------------
					const int32 BlockedBeforePlumbing = PC->DebugShieldBlockedHitCount;
					const bool bFlagBefore = PC->WasLastHitMarkerShieldBlocked();
					PC->ClientNotifyHit(/*bKilled=*/false, ETraceHitZone::Body, /*bShieldBlocked=*/true);
					const bool bFlagAfter = PC->WasLastHitMarkerShieldBlocked();
					const int32 BlockedAfterPlumbing = PC->DebugShieldBlockedHitCount;
					Run->bPlumbingArmOk = !bFlagBefore && bFlagAfter
						&& (BlockedAfterPlumbing == BlockedBeforePlumbing + 1);
					Run->PlumbingLine = FString::Printf(
						TEXT("blockedCount %d -> %d, WasLastHitMarkerShieldBlocked %d -> %d"),
						BlockedBeforePlumbing, BlockedAfterPlumbing, bFlagBefore ? 1 : 0, bFlagAfter ? 1 : 0);
					UE_LOG(LogTraceGame, Display, TEXT("[ShieldBlockTest] PLUMBING ARM: %s"), *Run->PlumbingLine);

					const bool bPass = Run->bBlockedArmOk && Run->bControlArmOk && Run->bPlumbingArmOk;
					UE_LOG(LogTraceGame, Display,
						TEXT("[ShieldBlockTest] VERDICT: %s — shieldedArm=%d (%s) | controlArm=%d (%s) | ")
						TEXT("plumbingArm=%d (%s). Totals: %d blocked of %d confirmed hits."),
						bPass ? TEXT("PASS") : TEXT("FAIL"),
						Run->bBlockedArmOk ? 1 : 0, *Run->BlockedLine,
						Run->bControlArmOk ? 1 : 0, *Run->ControlLine,
						Run->bPlumbingArmOk ? 1 : 0, *Run->PlumbingLine,
						PC->DebugShieldBlockedHitCount, PC->DebugHitConfirmCount);
					return false;
				}
				return true;

			default:
				return false;
			}
		}), 0.f);
	}
}

static FAutoConsoleCommand GTraceShieldBlockTestCmd(
	TEXT("Trace.Weapon.ShieldBlockTest"),
	TEXT("Dev only, SERVER. RESTRUCTURE C5 / code-gameplay F2. Shoots the SAME enemy twice — once "
	     "holding the Core (MEASURED: no hit is confirmed at all, because the lag-compensated resolver "
	     "refuses a shielded carrier as a candidate) and once without it (the hit must confirm normally "
	     "and take health) — then drives ClientNotifyHit's new bShieldBlocked directly to prove the "
	     "controller records it. Real key, real ServerFire, real ClientNotifyHit."),
	FConsoleCommandDelegate::CreateStatic(&TraceShieldBlockTest::Run));

#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Fx.ImpactShots — the staging for FX_AUDIO_PLAN §3's acceptance shot
//
// "5-shot repeat frames showing the travelling bolt AND an impact plane on world hits only."
//
// The frames come from -TraceAutoShot / -TraceAutoShotRepeat; what this command does is make the
// pawn FIRE, at something chosen rather than at whatever happens to be in front of it, and then say
// in the log which shots were entitled to a mark. Both halves matter:
//
//   * A capture harness that fires at "forward" is photographing the map's furniture, not the rule.
//     `world` pitches the aim down at the FLOOR — geometry, guaranteed, with no pawn in the way —
//     and `body` snaps it onto the nearest other character's chest. Those are the two arms of §3's
//     "geometry hits ONLY", and the second one must produce NO plane.
//   * A frame nobody can grade is not evidence. Each shot logs the victim the resolve found and
//     whether the tracer armed a plane (ATraceTracer::IsImpactPlaneArmed), so the screenshots have a
//     ledger beside them instead of an eyeball verdict on a 26 uu quad.
//
// It drives StartFire(), the trigger's own verb, so the whole real path runs: CanFire, FireOnce, the
// predicted audio, the local tracer, ServerFire, the multicast. Nothing here is a shortcut past the
// code under test.
//
// Named for this file; internal linkage; inside !UE_BUILD_SHIPPING like every other command here.
// =================================================================================================
namespace TraceImpactShots
{
	FTSTicker::FDelegateHandle GTicker;
	int32 GRemaining = 0;
	float GInterval = 0.55f;
	float GWait = 0.f;
	int32 GIndex = 0;
	bool GAimBody = false;

	/**
	 * When true, the run switches to the BODY arm once the WORLD arm is spent, in one command.
	 *
	 * *** IT EXISTS BECAUSE -TraceExec HAS EXACTLY TWO ROUNDS AND THE CAPTURE NEEDS THREE THINGS. ***
	 * A headless capture must pick a character (the select screen has no timeout in the practice
	 * range), fire at the floor, and fire at a body. Round 1 is the pick, round 2 is this command, and
	 * there is no round 3 — so the two arms live in one verb with a pause between them rather than in
	 * two commands that cannot both be scheduled.
	 */
	bool GBodyAfter = false;
	int32 GBodyCount = 0;

	/**
	 * *** TIME IS DILATED FOR THE CAPTURE, AND THAT IS ABOUT THE CAMERA, NOT THE EFFECT. ***
	 *
	 * A bolt lives 0.10..0.30 s (ATraceTracer::BoltMinLifeSeconds/BoltMaxLifeSeconds) and the
	 * off-screen screenshot pipeline delivers about one frame every 0.12 s of REAL time however
	 * often -TraceAutoShotRepeat asks — 245 requests produced 78 files in the run that established
	 * this. So at normal speed the capture samples at roughly one age per shot and lands either side
	 * of the flight: measured over two runs, every frame came out at age 0.001 s (bolt still on the
	 * muzzle) or 0.12 s and later (bolt already swallowed).
	 *
	 * Dilating the world clock changes NOTHING about the effect. Every duration in ATraceTracer is a
	 * pure function of UWorld::GetTimeSeconds() — the class comment on SpawnTimeSeconds is explicit
	 * that age is sampled from the absolute world clock and never accumulated — so the bolt travels
	 * exactly the same distance at exactly the same fraction of its life. What changes is how many
	 * real-time frames fit inside that life, which is a property of the observer and is the whole
	 * problem. Restored on the way out so nothing else in the session inherits it.
	 */
	static constexpr float CaptureDilation = 0.25f;
	bool GDilated = false;

	/**
	 * The beam from the shot just fired, and how long until its mark is read back.
	 *
	 * *** THE LEDGER LINE CANNOT BE WRITTEN AT THE MOMENT OF FIRING, AND THE FIRST DRAFT WROTE IT
	 * THERE. *** At the instant the trigger breaks, the plane is ARMED but not SHOWN: §3 reveals it
	 * when the bolt HEAD ARRIVES, which for a floor shot is about 0.06 s later. So a line printed at
	 * fire time can only say what the shot decided, not what ended up on screen — and "what ended up
	 * on screen" is precisely what a screenshot has to be graded against. The readback therefore
	 * waits, and prints the plane's own live transform: visible, where, how wide, how opaque.
	 *
	 * WORLD SECONDS, not real ones, so the capture's time dilation moves it with the effect.
	 */
	TWeakObjectPtr<const ATraceTracer> GBeam;
	float GReadbackIn = -1.f;
	int32 GReadbackIndex = 0;
	bool GReadbackBody = false;
	FString GReadbackVictim;

	void SetDilation(UWorld* World, float Value)
	{
		if (World != nullptr)
		{
			if (AWorldSettings* Settings = World->GetWorldSettings())
			{
				Settings->SetTimeDilation(Value);
			}
		}
	}

	/**
	 * How long after the trigger the plane is read back, in seconds of the (dilated) world clock.
	 *
	 * Comfortably after a floor shot's arrival (~0.06 s) and comfortably inside the mark's 0.18 s
	 * life, so the readback lands on a frame where there is something to read.
	 */
	static constexpr float ReadbackDelaySeconds = 0.10f;

	UWorld* ShotWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() != nullptr && Context.World()->IsGameWorld())
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	ATracePlayerController* LocalController(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (PC != nullptr && PC->IsLocalController())
			{
				return Cast<ATracePlayerController>(PC);
			}
		}
		return nullptr;
	}

	/** Points the control rotation at the floor, or at the nearest other character's chest. */
	void Aim(ATracePlayerController* PC, ATraceCharacter* Pawn)
	{
		if (PC == nullptr || Pawn == nullptr)
		{
			return;
		}

		if (!GAimBody)
		{
			// *** TWO PITCHES, ALTERNATING, AND THE REASON IS THE PHYSICS OF THE EFFECT ITSELF. ***
			// The two things §3 and §4 have to be photographed doing are in tension, because the
			// bolt's life is SOLVED from the shot's length (ATraceTracer::ResolveBoltTravel):
			//
			//   a NEAR shot   (-35 deg, floor a few metres ahead)  the impact is close, so the mark
			//                 is large on screen — but the flight is under the 0.10 s life floor and
			//                 no realistic capture cadence catches the bolt in the air.
			//   a FAR shot    (-3 deg, down the length of the arena) takes the full 0.30 s, so the
			//                 bolt is photographable mid-flight — but its mark is tens of metres away
			//                 and a 26 uu quad there is a handful of pixels.
			//
			// Measured, not guessed: at -35 the mark reads as a ~48 px square and no frame of a 5-shot
			// burst contained a travelling bolt; at -18 the bolt was still gone and the mark was down
			// to a max channel delta of 18. So the arm alternates, odd shots near and even shots far,
			// and one five-shot burst produces both frames. Every one of them is still a WORLD hit —
			// the floor either way — which is what the arm is for.
			FRotator Aimed = PC->GetControlRotation();
			Aimed.Pitch = ((GIndex % 2) == 0) ? -35.f : -3.f;
			Aimed.Roll = 0.f;
			PC->SetControlRotation(Aimed);
			return;
		}

		ATraceCharacter* Nearest = nullptr;
		double BestSq = TNumericLimits<double>::Max();
		for (TActorIterator<ATraceCharacter> It(Pawn->GetWorld()); It; ++It)
		{
			ATraceCharacter* Other = *It;
			if (!IsValid(Other) || Other == Pawn || !Other->IsAlive())
			{
				continue;
			}
			const double DistSq = FVector::DistSquared(Other->GetActorLocation(), Pawn->GetActorLocation());
			if (DistSq < BestSq)
			{
				BestSq = DistSq;
				Nearest = Other;
			}
		}

		if (Nearest == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ImpactShots] `body` arm asked for, but there is nobody else alive to shoot at. "
				     "Falling back to the floor, which will draw a plane — that is NOT the body arm."));
			GAimBody = false;
			Aim(PC, Pawn);
			return;
		}

		const FVector Eye = Pawn->GetPawnViewLocation();
		PC->SetControlRotation((Nearest->GetActorLocation() - Eye).Rotation());
	}

	bool Tick(float Delta)
	{
		UWorld* World = ShotWorld();
		ATracePlayerController* PC = LocalController(World);
		ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		UTraceWeaponComponent* Weapon = (Pawn != nullptr) ? Pawn->Weapon : nullptr;

		if (Weapon == nullptr || GRemaining <= 0)
		{
			if (Weapon == nullptr)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[ImpactShots] no local pawn with a weapon — run this inside a match."));
			}
			else if (GBodyAfter)
			{
				// THE SECOND ARM, IN THE SAME RUN. A pause first, so the last world bolt has been
				// swallowed and the last mark has faded before the body frames start — otherwise a
				// frame from the body arm could contain a plane that belongs to the world arm, which
				// is exactly the confusion the two arms exist to rule out.
				GBodyAfter = false;
				GAimBody = true;
				GRemaining = GBodyCount;
				GIndex = 0;
				GWait = 2.0f;
				UE_LOG(LogTraceGame, Display,
					TEXT("[ImpactShots] world arm done: %d shot(s). Switching to the BODY arm in 2.0s — "
					     "every shot of it must report `impact plane none`."), GBodyCount);
				return true;
			}
			else
			{
				UE_LOG(LogTraceGame, Display, TEXT("[ImpactShots] done: %d shot(s) fired."), GIndex);
			}
			if (GDilated)
			{
				SetDilation(World, 1.f);
				GDilated = false;
			}
			FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
			GTicker.Reset();
			return false;
		}

		// THE READBACK IS ON ITS OWN CLOCK, ahead of the next shot, because the mark it grades is up
		// for 0.18 s and the next trigger pull is 0.55 s away.
		if (GReadbackIn >= 0.f)
		{
			GReadbackIn -= Delta;
			if (GReadbackIn < 0.f)
			{
				const ATraceTracer* Beam = GBeam.Get();
				FVector Where = FVector::ZeroVector;
				float WidthUU = 0.f;
				float Opacity = 0.f;
				const bool bArmed = (Beam != nullptr) && Beam->IsImpactPlaneArmed();
				const bool bOnScreen = (Beam != nullptr) && Beam->DescribeImpactPlane(Where, WidthUU, Opacity);

				// THE SHOT'S OWN LENGTH, off the beam that drew it. It is here because there are
				// THREE outcomes and two of them look identical without it: a shot that hit geometry
				// (mark), a shot that hit a player (no mark, by the §3 ruling) and a shot that hit
				// NOTHING and died at HitscanRange (no mark either, because there is no surface).
				// The third is not a defect and a ledger that could not tell it from the second would
				// invite somebody to go looking for one.
				FTraceTracerShotDebug Shot;
				const bool bDescribed = (Beam != nullptr) && Beam->DescribeShot(Shot);
				const float Range = FMath::Max(1.f, UTraceSettings::Get().HitscanRange);
				const bool bStopped = bDescribed && (Shot.ShotLengthUU < Range * 0.99f);

				// THREE LABELS AND NOT TWO, because "the beam is gone" is a fourth state and printing
				// it as "hit NOTHING" would invent a fact about the shot out of a fact about the
				// readback's timing. A recycled or expired tracer describes nothing at all.
				const TCHAR* const Outcome = !bDescribed ? TEXT("(beam gone)")
					: (bStopped ? TEXT("(hit a surface)") : TEXT("(hit NOTHING)"));

				UE_LOG(LogTraceGame, Display,
					TEXT("[ImpactShots] shot %d (%s arm): victim=%-18s shot=%7.0fuu %-15s | plane %s | %s"),
					GReadbackIndex, GReadbackBody ? TEXT("BODY") : TEXT("WORLD"), *GReadbackVictim,
					bDescribed ? Shot.ShotLengthUU : -1.f, Outcome,
					bArmed ? TEXT("ARMED") : TEXT("none "),
					bOnScreen
						? *FString::Printf(TEXT("ON SCREEN at %s, %.1fuu wide, opacity %.3f (recomputed)"),
							*Where.ToCompactString(), WidthUU, Opacity)
						: TEXT("nothing drawn"));
				GBeam.Reset();
			}
		}

		GWait -= Delta;
		if (GWait > 0.f)
		{
			return true;
		}
		GWait = GInterval;

		Aim(PC, Pawn);
		Weapon->StartFire();
		Weapon->StopFire();
		++GIndex;
		--GRemaining;

		// READ BACK OFF THE BEAM THAT WAS JUST DRAWN, not off the intent. GetNewestTracer with
		// bFirstPersonOnly is the local shooter's own beam — the same filter Trace.Fx.Beam uses, and
		// for the same reason: in a populated world "the newest tracer" is very often somebody else's.
		GBeam = ATraceTracer::GetNewestTracer(World, /*bFirstPersonOnly=*/true);
		GReadbackIndex = GIndex;
		GReadbackBody = GAimBody;
		GReadbackVictim = GetNameSafe(Weapon->DebugGetLastPredictedVictim());
		GReadbackIn = ReadbackDelaySeconds;

		return true;
	}

	void Arm(int32 Count, bool bBody, float Interval)
	{
		if (GTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
			GTicker.Reset();
		}
		GRemaining = FMath::Clamp(Count, 1, 60);
		GInterval = FMath::Clamp(Interval, 0.35f, 5.f);
		GWait = 0.f;
		GIndex = 0;
		GAimBody = bBody;
		GBodyAfter = false;
		GBodyCount = 0;

		SetDilation(ShotWorld(), CaptureDilation);
		GDilated = true;

		UE_LOG(LogTraceGame, Display,
			TEXT("[ImpactShots] arming %d shot(s) every %.2fs at the %s (world clock dilated to 0.25 so "
			     "the capture can sample inside a 0.10..0.30s bolt). A WORLD shot must arm a plane; "
			     "a BODY shot must not (FX_AUDIO_PLAN §3, honouring spec v4 §4's deleted on-victim pop)."),
			GRemaining, GInterval, bBody ? TEXT("nearest character") : TEXT("floor, near and far alternately"));

		GTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.f);
	}
} // namespace TraceImpactShots

static FAutoConsoleCommand GTraceFxImpactShotsBothCmd(
	TEXT("Trace.Fx.ImpactShots.Both"),
	TEXT("The capture run in ONE verb: five shots at the floor, a two-second pause, then five at the "
	     "nearest character. -TraceExec has two rounds and a headless capture needs three things "
	     "(pick a character, world arm, body arm), so the two arms share a command."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		TraceImpactShots::Arm(5, /*bBody=*/false, 0.55f);
		TraceImpactShots::GBodyAfter = true;
		TraceImpactShots::GBodyCount = 5;
	}));

static FAutoConsoleCommand GTraceFxImpactShotsBodyCmd(
	TEXT("Trace.Fx.ImpactShots.Body"),
	TEXT("The BODY arm of Trace.Fx.ImpactShots, as its own verb because -TraceExec must be ONE "
	     "unquoted argv token and therefore cannot carry an argument. Fires 5 shots at the nearest "
	     "character; every one of them must report `impact plane none`."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		TraceImpactShots::Arm(5, /*bBody=*/true, 0.55f);
	}));

static FAutoConsoleCommand GTraceFxImpactShotsCmd(
	TEXT("Trace.Fx.ImpactShots"),
	TEXT("Trace.Fx.ImpactShots [count] [world|body] [interval] — fire `count` real shots (default 5) "
	     "at the floor (default) or at the nearest character, `interval` seconds apart (default "
	     "0.55), and log per shot whether the tracer armed an FX_AUDIO_PLAN s3 impact plane. Pair it "
	     "with -TraceAutoShot/-TraceAutoShotRepeat for the frames: the WORLD arm must show a mark on "
	     "the floor and the BODY arm must show none."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const int32 Count = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 5;
		const bool bBody = (Args.Num() > 1) && Args[1].Equals(TEXT("body"), ESearchCase::IgnoreCase);
		const float Interval = (Args.Num() > 2) ? FCString::Atof(*Args[2]) : 0.55f;
		TraceImpactShots::Arm(Count, bBody, Interval);
	}));
#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Audio.CoreRows — the three FX_AUDIO_PLAN §5.1 ★ rows a staged bot match does not reach
//
// A 150 s eight-bot match on Arena_Baked exercised most of the ★ set by itself (MeleeSwing, MeleeHit,
// Reload, WeaponSwitch, DeathBurst, Respawn, CountdownTick, CountdownGo all appear in
// Trace.Audio.EventPlays afterwards). Three do not, and each for a structural reason rather than
// because the match was short:
//
//   DryFire         a bot never holds a trigger on an empty gun — the automatic reload fires at the
//                   shot that empties the clip, so the refusal branch is unreachable by ordinary play.
//   MeleeBackstab   requires an attacker standing INSIDE the back arc of a victim who is not looking,
//                   which the bots' approach does not reliably produce.
//   DamageTaken     is client-side and only the LOCAL player's own pawn can sound it; a headless
//                   host's pawn stands at its spawn and is not necessarily shot.
//
// So this drives all three, deliberately, through the real paths: the real trigger for the dry fire,
// a real StartSwing from a real position behind a real victim for the back-stab, and a real
// ApplyDamage for the damage cue. Nothing here calls TraceAudio:: directly — if a call site were
// missing, this command would be silent and Trace.Audio.EventPlays would say so.
// =================================================================================================
namespace TraceCoreRowsTest
{
	FTSTicker::FDelegateHandle GTicker;
	int32 GStep = 0;
	float GWait = 0.f;

	/**
	 * The four rows' play counts when the command armed, so the verdict measures THIS run.
	 *
	 * A bare count would be a fact about the whole session — a knife kill from ten seconds earlier
	 * would make the back-stab row read green without this command having driven anything. The
	 * baseline is what turns "the row has a count" into "this command made the row move".
	 */
	int32 GBase[4] = { 0, 0, 0, 0 };

	/**
	 * How far behind the victim the attacker is placed, in uu.
	 *
	 * Inside UTraceMeleeSettings::SwingRangeUU (180) with room for the blade's origin being at the
	 * attacker's own muzzle rather than at their feet, and far enough that the two capsules are not
	 * interpenetrating when the swing resolves.
	 */
	static constexpr double BackstabStandoffUU = 80.0;

	const FName* RowNames()
	{
		static const FName Names[4] = { TraceSoundEvents::DryFire, TraceSoundEvents::MeleeBackstab,
			TraceSoundEvents::MeleeHit, TraceSoundEvents::DamageTaken };
		return Names;
	}

	int32 PlaysOf(const UWorld* World, FName Event)
	{
		const UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		if (Audio == nullptr)
		{
			return 0;
		}
		const int32* Found = Audio->GetPlaysByEvent().Find(Event);
		return (Found != nullptr) ? *Found : 0;
	}

	UWorld* RowsWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World() != nullptr && Context.World()->IsGameWorld())
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	ATracePlayerController* RowsController(UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (PC != nullptr && PC->IsLocalController())
			{
				return Cast<ATracePlayerController>(PC);
			}
		}
		return nullptr;
	}

	ATraceCharacter* NearestOther(ATraceCharacter* Mine)
	{
		ATraceCharacter* Best = nullptr;
		double BestSq = TNumericLimits<double>::Max();
		for (TActorIterator<ATraceCharacter> It(Mine->GetWorld()); It; ++It)
		{
			ATraceCharacter* Other = *It;
			if (!IsValid(Other) || Other == Mine || !Other->IsAlive())
			{
				continue;
			}
			const double DistSq = FVector::DistSquared(Other->GetActorLocation(), Mine->GetActorLocation());
			if (DistSq < BestSq)
			{
				BestSq = DistSq;
				Best = Other;
			}
		}
		return Best;
	}

	bool Tick(float Delta)
	{
		UWorld* World = RowsWorld();
		ATracePlayerController* PC = RowsController(World);
		ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		UTraceWeaponComponent* Weapon = (Pawn != nullptr) ? Pawn->Weapon : nullptr;

		if (Weapon == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[CoreRows] no local pawn with a weapon — run this inside a match."));
			FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
			GTicker.Reset();
			return false;
		}

		GWait -= Delta;
		if (GWait > 0.f)
		{
			return true;
		}

		switch (GStep)
		{
		case 0:
			// DRY FIRE. LoadAbilityClip(0) is the authority's own "replace the magazine" verb and it
			// is the shortest honest way to an empty gun; the trigger below is the real one, so the
			// refusal that follows is CanFire()'s real refusal and not a simulated one.
			Weapon->LoadAbilityClip(0);
			UE_LOG(LogTraceGame, Display, TEXT("[CoreRows] clip emptied (%d rounds); pulling the trigger."),
				Weapon->GetClipAmmo());
			GWait = 0.2f;
			break;

		case 1:
			Weapon->StartFire();
			Weapon->StopFire();
			UE_LOG(LogTraceGame, Display, TEXT("[CoreRows] dry trigger pulled (CanFire=%d)."), Weapon->CanFire() ? 1 : 0);
			GWait = 0.4f;
			break;

		case 2:
		{
			// BACK-STAB. Placed INSIDE the victim's back arc and facing them, then swung with the
			// real StartSwing — so TraceMelee::ResolveSwing makes the real angle judgement and
			// ServerSwing applies the real damage, which is where the sound is.
			ATraceCharacter* Victim = NearestOther(Pawn);
			if (Victim == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[CoreRows] nobody to back-stab; skipping that row."));
				GStep += 2;
				GWait = 0.1f;
				return true;
			}
			const FVector Behind = Victim->GetActorLocation() - Victim->GetActorForwardVector() * BackstabStandoffUU;
			Pawn->SetActorLocation(Behind, /*bSweep=*/false);
			PC->SetControlRotation((Victim->GetActorLocation() - Pawn->GetPawnViewLocation()).Rotation());
			TraceMelee::RequestEquipIfDifferent(Pawn, ETraceEquippedWeapon::Knife, nullptr);
			UE_LOG(LogTraceGame, Display, TEXT("[CoreRows] placed behind %s; drawing the knife."), *GetNameSafe(Victim));
			GWait = 0.6f;   // the knife's pullout
			break;
		}

		case 3:
			Weapon->StartSwing(nullptr);
			UE_LOG(LogTraceGame, Display, TEXT("[CoreRows] swung from behind."));
			GWait = 0.5f;   // the wind-up, then ServerSwing
			break;

		case 4:
			// DAMAGE TAKEN. The real ApplyDamage, on the local player's own health component, which
			// is what OnRep_Health hangs the cue off.
			if (UTraceHealthComponent* Health = Pawn->FindComponentByClass<UTraceHealthComponent>())
			{
				Health->ApplyDamage(20.f, nullptr, TEXT("Trace.Audio.CoreRows"));
				UE_LOG(LogTraceGame, Display, TEXT("[CoreRows] took 20 damage (health now %.0f%%)."),
					Health->GetHealthPercent() * 100.f);
			}
			GWait = 0.3f;
			break;

		default:
		{
			// THE VERDICT, against the baseline taken at arm — see GBase. MeleeHit rides along
			// because the back-stab and the front cut are the two branches of ONE ternary at ONE call
			// site: if the swing landed but came out as a front cut, the row that moved says so and
			// the failure is "the placement missed the back arc", not "the sound is unwired".
			const int32 Dry  = PlaysOf(World, RowNames()[0]) - GBase[0];
			const int32 Back = PlaysOf(World, RowNames()[1]) - GBase[1];
			const int32 Front= PlaysOf(World, RowNames()[2]) - GBase[2];
			const int32 Hurt = PlaysOf(World, RowNames()[3]) - GBase[3];

			UE_LOG(LogTraceGame, Display,
				TEXT("[CoreRows] this run played: DryFire %d, MeleeBackstab %d, MeleeHit %d, DamageTaken %d."),
				Dry, Back, Front, Hurt);

			const bool bPass = (Dry > 0) && (Hurt > 0) && (Back > 0);
			if (bPass)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[CoreRows] VERDICT: PASS — all three rows an ordinary match cannot reach fired "
					     "through their real trigger sites."));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[CoreRows] VERDICT: FAIL — %s%s%s"),
					(Dry > 0) ? TEXT("") : TEXT("DryFire silent. "),
					(Hurt > 0) ? TEXT("") : TEXT("DamageTaken silent. "),
					(Back > 0) ? TEXT("")
						: (Front > 0 ? TEXT("The swing landed but resolved as a FRONT cut — the placement missed the back arc, not the wiring. ")
						             : TEXT("The swing landed on nobody at all (a moving victim: run this on the practice range). ")));
			}

			FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
			GTicker.Reset();
			return false;
		}
		}

		++GStep;
		return true;
	}

	void Arm()
	{
		if (GTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
			GTicker.Reset();
		}
		GStep = 0;
		GWait = 0.f;
		UWorld* World = RowsWorld();
		for (int32 Index = 0; Index < 4; ++Index)
		{
			GBase[Index] = PlaysOf(World, RowNames()[Index]);
		}
		UE_LOG(LogTraceGame, Display,
			TEXT("[CoreRows] driving the three FX_AUDIO_PLAN s5.1 rows an ordinary match cannot reach: "
			     "DryFire, MeleeBackstab, DamageTaken."));
		GTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.f);
	}
} // namespace TraceCoreRowsTest

static FAutoConsoleCommand GTraceAudioCoreRowsCmd(
	TEXT("Trace.Audio.CoreRows"),
	TEXT("Drive the three FX_AUDIO_PLAN s5.1 core-combat rows a staged bot match does not reach on "
	     "its own — DryFire (empty the clip and pull the real trigger), MeleeBackstab (stand inside "
	     "a victim's back arc and swing) and DamageTaken (apply real damage to the local pawn). "
	     "Follow it with Trace.Audio.EventPlays; all three must then have a count."),
	FConsoleCommandDelegate::CreateStatic(&TraceCoreRowsTest::Arm));
#endif // !UE_BUILD_SHIPPING
