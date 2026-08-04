#include "Gameplay/TraceWeaponComponent.h"

#include "CollisionQueryParams.h"
#include "HAL/IConsoleManager.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "Math/UnrealMathUtility.h"

#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceHitZones.h"
#include "Gameplay/TraceTracer.h"
#include "Net/TraceLagCompensationComponent.h"
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

static TAutoConsoleVariable<int32> CVarTraceShotStats(
	TEXT("Trace.ShotStats"),
	0,
	TEXT("1: accumulate the hit-zone / impact-height distribution for every server-accepted shot. Trace.ShotStats.Dump prints it."),
	ECVF_Default);

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
	// Ticks only while the trigger is held on the machine that owns the input.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	// No replicated state of its own, but the component must be replicated for its RPCs to route.
	SetIsReplicatedByDefault(true);
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

	const double FireInterval = FMath::Max(0.01f, UTraceSettings::Get().FireInterval);
	return (GetLocalTimeSeconds() - LastLocalFireTime) >= FireInterval;
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
	}
}

void UTraceWeaponComponent::StopFire()
{
	bTriggerHeld = false;
	SetComponentTickEnabled(false);
}

void UTraceWeaponComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bTriggerHeld)
	{
		SetComponentTickEnabled(false);
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		// The pawn is gone or is no longer ours: genuine teardown, drop everything.
		StopFire();
		return;
	}

	if (!Character->IsAlive() || Character->IsCarrier())
	{
		// Temporarily ineligible. Deliberately does NOT clear bTriggerHeld: this component only
		// hears about the trigger on press and release, so clearing it here means a player who is
		// still physically holding the button gets nothing after passing the Core away or after
		// respawning — they have to release and press again for no reason they can see. Keep
		// ticking, skip firing, and resume the instant the gate reopens.
		return;
	}

	if (CanFire())
	{
		FireOnce();
	}
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
	LastLocalFireTime = GetLocalTimeSeconds();

	if (CVarTraceShotStats.GetValueOnGameThread() != 0)
	{
		++TraceShotStats::GStats.LocalShotsFired;
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

	PlayLocalTracer(Origin, TracerEnd, bImpacted);

	// Viewmodel recoil, once per ROUND. This is the only place that knows a round actually left the
	// gun, so a held burst kicks per shot instead of once on the trigger press. Cosmetic only,
	// rate-limited inside, and a no-op on any machine that is not looking out of this pawn.
	Character->NotifyWeaponFired();

	ServerFire(FVector_NetQuantize(Origin), FVector_NetQuantizeNormal(Dir), static_cast<float>(FireServerTime));
}

void UTraceWeaponComponent::PlayLocalTracer(const FVector& From, const FVector& To, bool bImpacted) const
{
	UWorld* World = GetWorld();
	ATraceCharacter* Character = GetTraceCharacter();
	if (World == nullptr || Character == nullptr)
	{
		return;
	}

	ATraceTracer::Spawn(World, From, To, TraceTeamColor(Character->GetTeam()), bImpacted);
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

	const UTraceSettings& Settings = UTraceSettings::Get();

	// ---- fire rate, with slack for honest jitter -----------------------------------------
	const double FireInterval = FMath::Max(0.01f, Settings.FireInterval);
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
	if (Victim != nullptr)
	{
		if (UTraceHealthComponent* VictimHealth = Victim->FindComponentByClass<UTraceHealthComponent>())
		{
			// Spec section 6: head 100 / body 40 / legs 25. No multiplier, no base damage - the zone
			// IS the damage. UTraceSettings::HitscanDamage and HeadshotMultiplier are no longer read
			// by the weapon (see the report); the numbers live in UTraceDamageSettings.
			const float Damage = FTraceHitZoneModel::DamageForZone(Zone);
			VictimHealth->ApplyDamage(Damage, Character->GetController(), FName(TEXT("Bullet")));

			// ApplyDamage no-ops against an invulnerable target, so read the result rather than
			// assuming the hit landed.
			bKilled = !VictimHealth->IsAlive();
		}

		if (ATracePlayerController* ShooterController = Cast<ATracePlayerController>(Character->GetController()))
		{
			// The zone rides along so the shooter's hitmarker can say WHICH zone paid out. Positional
			// damage is only learnable if the feedback is positional too.
			ShooterController->ClientNotifyHit(bKilled, Zone);
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
		UE_LOG(LogTraceGame, Display,
			TEXT("HITZONE %s  %s: predicted %s on %s | server %s on %s (damage %.0f, rewind %.3fs)"),
			(bSameVictim && bSameZone) ? TEXT("AGREE   ") : TEXT("DISAGREE"),
			*GetNameSafe(Character),
			TraceHitZoneToString(LastPredictedZone), *GetNameSafe(LastPredictedVictim.Get()),
			TraceHitZoneToString(Zone), *GetNameSafe(Victim),
			FTraceHitZoneModel::DamageForZone(Zone),
			ServerNow - RewindTime);
	}

	// Unreliable and cosmetic: everyone but the shooter draws the railgun beam.
	const bool bImpacted = FVector::DistSquared(ShotOrigin, ImpactPoint) < static_cast<double>(ShotRange) * ShotRange * 0.998;
	MulticastFireEffects(FVector_NetQuantize(ShotOrigin), FVector_NetQuantize(ImpactPoint), bImpacted);
}

void UTraceWeaponComponent::MulticastFireEffects_Implementation(FVector_NetQuantize Origin, FVector_NetQuantize Impact, bool bImpacted)
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

	PlayLocalTracer(Origin, Impact, bImpacted);
}
