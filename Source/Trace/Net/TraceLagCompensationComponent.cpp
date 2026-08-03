#include "Net/TraceLagCompensationComponent.h"

#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameStateBase.h"
#include "Math/NumericLimits.h"                // TNumericLimits<double>
#include "Math/UnrealMathUtility.h"

#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Gameplay/TraceCore.h"                // IsShieldSuppressedFor (the pass risk beat)
#include "Gameplay/TraceHitZones.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"

namespace
{
	/**
	 * The shared clock. On the server GetServerWorldTimeSeconds() is exactly the world time, so this
	 * is both the timestamp we record poses under and the timestamp clients send us in ServerFire.
	 */
	double TraceGetServerTimeSeconds(const UWorld* World)
	{
		if (World == nullptr)
		{
			return 0.0;
		}
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return GameState->GetServerWorldTimeSeconds();
		}
		return World->GetTimeSeconds();
	}
} // namespace

UTraceLagCompensationComponent::UTraceLagCompensationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Sample after movement has been applied for the frame, so the recorded pose is the one the
	// rest of the world saw this tick rather than last tick's leftovers.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	// Pure server-side bookkeeping: nothing here is ever sent to a client.
	SetIsReplicatedByDefault(false);
}

void UTraceLagCompensationComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority())
	{
		// Clients keep a component (it is a character subobject) but never record anything.
		SetComponentTickEnabled(false);
		return;
	}

	RecordFrame(static_cast<float>(TraceGetServerTimeSeconds(GetWorld())));
}

void UTraceLagCompensationComponent::RecordFrame(float ServerTime)
{
	AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	if (!FMath::IsFinite(ServerTime))
	{
		return;
	}

	const ACharacter* OwnerCharacter = Cast<ACharacter>(OwnerActor);
	const UCapsuleComponent* Capsule = (OwnerCharacter != nullptr)
		? OwnerCharacter->GetCapsuleComponent()
		: OwnerActor->FindComponentByClass<UCapsuleComponent>();

	if (Capsule == nullptr)
	{
		return;
	}

	// Guard against being driven twice in one frame (our own tick plus a caller): two samples with
	// the same timestamp would give GetPoseAtTime a zero-length span to interpolate across.
	if (History.Num() > 0 && ServerTime <= History.Last().ServerTime)
	{
		return;
	}

	FTraceLagCompFrame& Frame = History.AddDefaulted_GetRef();
	Frame.ServerTime = ServerTime;
	Frame.CapsuleCenter = Capsule->GetComponentLocation();
	Frame.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	Frame.CapsuleRadius = Capsule->GetScaledCapsuleRadius();

	// Posture rewinds with the pose, so a shot fired at a sliding player is classified against the
	// crouched zone layout the shooter actually saw, not against where that player is standing now.
	if (const ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(OwnerActor))
	{
		Frame.PostureScale = TraceCharacter->GetHitZonePostureScale();
	}

	TrimHistory(ServerTime);
}

void UTraceLagCompensationComponent::TrimHistory(float NewestServerTime)
{
	const float Duration = FMath::Max(0.05f, UTraceSettings::Get().LagCompHistoryDuration);

	// Keep the newest sample that is already older than the window: a request landing exactly on the
	// window edge still needs a frame on each side of it to interpolate between.
	int32 FirstToKeep = 0;
	while (FirstToKeep + 1 < History.Num() && (NewestServerTime - History[FirstToKeep + 1].ServerTime) > Duration)
	{
		++FirstToKeep;
	}

	if (FirstToKeep > 0)
	{
		History.RemoveAt(0, FirstToKeep);
	}

	if (History.Num() > MaxHistoryFrames)
	{
		History.RemoveAt(0, History.Num() - MaxHistoryFrames);
	}
}

bool UTraceLagCompensationComponent::GetPoseAtTime(float ServerTime, FTraceLagCompFrame& Out) const
{
	if (History.Num() == 0)
	{
		return false;
	}

	const FTraceLagCompFrame& Newest = History.Last();
	if (ServerTime >= Newest.ServerTime)
	{
		// Request is at or after our latest sample (a shot that arrived with no meaningful latency,
		// or a clamp to Now). Use the live-most pose we have rather than extrapolating forwards.
		Out = Newest;
		return true;
	}

	const FTraceLagCompFrame& Oldest = History[0];
	if (ServerTime < Oldest.ServerTime)
	{
		// Older than anything we retained - the caller asked to rewind further than
		// LagCompHistoryDuration. Refuse rather than invent a pose.
		return false;
	}

	// Walk backwards: shots almost always land within the last handful of frames.
	for (int32 Index = History.Num() - 1; Index > 0; --Index)
	{
		const FTraceLagCompFrame& B = History[Index];
		const FTraceLagCompFrame& A = History[Index - 1];
		if (ServerTime >= A.ServerTime && ServerTime <= B.ServerTime)
		{
			const float Span = B.ServerTime - A.ServerTime;
			// Literal epsilon rather than KINDA_SMALL_NUMBER: the un-prefixed math macros have been
			// churning across the 5.x line and this file must compile unchanged on 5.4 - 5.8.
			const float Alpha = (Span > 1.e-4f)
				? FMath::Clamp((ServerTime - A.ServerTime) / Span, 0.f, 1.f)
				: 0.f;

			Out.ServerTime = ServerTime;
			Out.CapsuleCenter = FMath::Lerp(A.CapsuleCenter, B.CapsuleCenter, Alpha);
			Out.CapsuleHalfHeight = FMath::Lerp(A.CapsuleHalfHeight, B.CapsuleHalfHeight, Alpha);
			Out.CapsuleRadius = FMath::Lerp(A.CapsuleRadius, B.CapsuleRadius, Alpha);
			Out.PostureScale = FMath::Lerp(A.PostureScale, B.PostureScale, Alpha);
			return true;
		}
	}

	// Unreachable in practice (timestamps are strictly increasing), but never leave Out untouched.
	Out = Oldest;
	return true;
}

ATraceCharacter* UTraceLagCompensationComponent::ResolveHitscan(
	UWorld* World,
	ATraceCharacter* Shooter,
	const FVector& Origin,
	const FVector& Direction,
	float Range,
	float RewindToServerTime,
	FVector& OutImpactPoint,
	ETraceHitZone& OutZone)
{
	OutZone = ETraceHitZone::None;
	OutImpactPoint = Origin;

	if (World == nullptr)
	{
		return nullptr;
	}

	const FVector Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero() || Origin.ContainsNaN())
	{
		return nullptr;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float ShotRange = FMath::Clamp(Range, 1.f, 1.0e6f);
	OutImpactPoint = Origin + Dir * ShotRange;

	// Defence in depth: UTraceWeaponComponent already clamps before calling, but this is a public
	// static on the contract surface and RewindToServerTime ultimately originates from a client
	// float. Any future caller that forgets the clamp would otherwise get unbounded rewind. Local
	// (the parameter is by value), so no caller behaviour changes.
	{
		const float ServerNow = static_cast<float>(TraceGetServerTimeSeconds(World));
		RewindToServerTime = FMath::Clamp(RewindToServerTime,
			ServerNow - FMath::Max(0.f, Settings.MaxRewindTime), ServerNow);
	}

	// ---------------------------------------------------------------------------------------
	// 1. Gather candidates. The GameMode owns the authoritative roster; the world iterator is a
	//    fallback for the (unexpected) case of being called without an auth GameMode.
	// ---------------------------------------------------------------------------------------
	TArray<ATraceCharacter*, TInlineAllocator<16>> Candidates;
	if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
	{
		for (const TWeakObjectPtr<ATraceCharacter>& Weak : GameMode->GetTrackedCharacters())
		{
			if (ATraceCharacter* TraceChar = Weak.Get())
			{
				Candidates.Add(TraceChar);
			}
		}
	}

	// Fall back on an EMPTY roster, not merely on a missing GameMode: on the server the GameMode
	// always exists, so an incomplete registration (a pawn spawned outside the normal flow, a
	// subclass that misses RegisterCharacter) would otherwise leave Candidates empty and every shot
	// would silently miss - the character capsule's Pawn profile ignores ECC_Visibility, so the
	// world trace below does not stop on bodies either. UTraceTrailComponent has the same shape.
	if (Candidates.Num() == 0)
	{
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			Candidates.Add(*It);
		}
	}

	// ---------------------------------------------------------------------------------------
	// 2. Trace the live world once, for static geometry only, to bound the ray. Every character is
	//    ignored here - characters are resolved against history below, never against where their
	//    collision happens to be sitting right now.
	// ---------------------------------------------------------------------------------------
	FCollisionQueryParams QueryParams(FName(TEXT("TraceHitscanWorld")), /*bTraceComplex=*/false, Shooter);
	QueryParams.bReturnPhysicalMaterial = false;
	for (ATraceCharacter* TraceChar : Candidates)
	{
		QueryParams.AddIgnoredActor(TraceChar);
	}

	double MaxDistance = ShotRange;
	FHitResult WorldHit;
	if (World->LineTraceSingleByChannel(WorldHit, Origin, Origin + Dir * ShotRange, ECC_Visibility, QueryParams))
	{
		// Distance is 0 when the trace starts inside geometry; keep a 1uu floor so a muzzle clipping
		// a wall degrades to "very short shot" instead of "every shot silently dies".
		MaxDistance = FMath::Max(static_cast<double>(WorldHit.Distance), 1.0);
		OutImpactPoint = WorldHit.ImpactPoint;
	}

	// ---------------------------------------------------------------------------------------
	// 3. Analytic segment-vs-body against each candidate's rewound pose. Nearest hit wins.
	//
	//    The body is the three-zone model in Gameplay/TraceHitZones.h, reconstructed from the
	//    rewound capsule. Hit *detection* is still the segment-vs-capsule test that shipped, so
	//    exactly the same shots connect as before positional damage existed; the zone only decides
	//    what a connecting shot is worth.
	// ---------------------------------------------------------------------------------------
	const FVector RayEnd = Origin + Dir * MaxDistance;
	const ETraceTeam ShooterTeam = (Shooter != nullptr) ? Shooter->GetTeam() : ETraceTeam::None;

	ATraceCharacter* BestTarget = nullptr;
	double BestDistanceAlongRay = TNumericLimits<double>::Max();
	FVector BestImpact = OutImpactPoint;
	ETraceHitZone BestZone = ETraceHitZone::None;

	for (ATraceCharacter* Target : Candidates)
	{
		if (!IsValid(Target) || Target == Shooter)
		{
			continue;
		}
		if (!Target->IsAlive())
		{
			continue;
		}
		if (Target->IsCarrier() && !ATraceCore::IsShieldSuppressedFor(Target))
		{
			// The Core carrier is invulnerable to bullets by design - do not even resolve them.
			//
			// INTEGRATION FIX (spec section 4, THE RISK BEAT). This used to be an unconditional
			// `if (Target->IsCarrier()) continue;`, which silently cancelled the entire pass
			// mechanic: UTraceHealthComponent::IsInvulnerable() correctly reports the carrier as
			// damageable while a pass is held, but a shot never reached ApplyDamage() because the
			// carrier was skipped as a hitscan CANDIDATE one step earlier. The passer risked
			// nothing, and the bots' PunishPasser state was firing into a target that could not be
			// hit. The condition is deliberately the SAME expression the health component reads, so
			// "can this be shot" has exactly one definition in the build and cancel restores both
			// halves of the beat in one statement.
			continue;
		}
		if (!Settings.bFriendlyFire && Shooter != nullptr && ShooterTeam != ETraceTeam::None && Target->GetTeam() == ShooterTeam)
		{
			continue;
		}

		FTraceLagCompFrame Frame;
		bool bHavePose = false;
		if (Settings.bEnableLagCompensation)
		{
			if (const UTraceLagCompensationComponent* TargetLagComp = Target->FindComponentByClass<UTraceLagCompensationComponent>())
			{
				bHavePose = TargetLagComp->GetPoseAtTime(RewindToServerTime, Frame);
			}
		}

		if (!bHavePose)
		{
			// Lag compensation off, no component, or the request predates this character's history
			// (they just spawned). Fall back to the live pose - always resolve the shot somehow.
			const UCapsuleComponent* Capsule = Target->GetCapsuleComponent();
			if (Capsule == nullptr)
			{
				continue;
			}
			Frame.ServerTime = RewindToServerTime;
			Frame.CapsuleCenter = Capsule->GetComponentLocation();
			Frame.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
			Frame.CapsuleRadius = Capsule->GetScaledCapsuleRadius();
			// Live posture too, so the client's predicted trace (which always lands here, because a
			// client has no lag-comp history) lays its zones out exactly as the server's rewind does.
			Frame.PostureScale = Target->GetHitZonePostureScale();
		}

		// Zones are derived from the rewound capsule, so they rewind with it for free - including
		// the crouched/sliding half height, which is the whole reason the model is proportional.
		const FTraceHitZoneModel ZoneModel = FTraceHitZoneModel::FromFrame(Frame);

#if ENABLE_DRAW_DEBUG
		if (Settings.bDrawServerRewindDebug)
		{
			// Rewound pose in white, where the character actually is right now in dark grey. Seeing
			// the two separate under latency is the whole point of this debug view.
			DrawDebugCapsule(World, Frame.CapsuleCenter, Frame.CapsuleHalfHeight, Frame.CapsuleRadius,
				FQuat::Identity, FColor::White, /*bPersistentLines=*/false, /*LifeTime=*/1.5f, /*DepthPriority=*/0, /*Thickness=*/1.f);

			if (const UCapsuleComponent* LiveCapsule = Target->GetCapsuleComponent())
			{
				DrawDebugCapsule(World, LiveCapsule->GetComponentLocation(), LiveCapsule->GetScaledCapsuleHalfHeight(),
					LiveCapsule->GetScaledCapsuleRadius(), FQuat::Identity, FColor(60, 60, 60),
					/*bPersistentLines=*/false, /*LifeTime=*/1.5f, /*DepthPriority=*/0, /*Thickness=*/0.5f);
			}

			// The head sphere the one-shot kill is actually tested against, so "why was that not a
			// headshot" is answerable by looking rather than by reading code.
			DrawDebugSphere(World, ZoneModel.HeadCenter, static_cast<float>(ZoneModel.HeadRadius), 12, FColor::Yellow,
				/*bPersistentLines=*/false, /*LifeTime=*/1.5f, /*DepthPriority=*/0, /*Thickness=*/1.f);

			// The hip line dividing body from legs.
			const FVector HipCentre(ZoneModel.CapsuleCenter.X, ZoneModel.CapsuleCenter.Y, ZoneModel.HipZ);
			DrawDebugCircle(World, HipCentre, static_cast<float>(ZoneModel.Radius), 16, FColor::Green,
				/*bPersistentLines=*/false, /*LifeTime=*/1.5f, /*DepthPriority=*/0, /*Thickness=*/1.f,
				FVector(1, 0, 0), FVector(0, 1, 0), /*bDrawAxis=*/false);
		}
#endif

		double AlongEntry = 0.0;
		FVector ZoneImpact = FVector::ZeroVector;
		ETraceHitZone ZoneHit = ETraceHitZone::None;
		if (!ZoneModel.ResolveSegment(Origin, RayEnd, AlongEntry, ZoneImpact, ZoneHit))
		{
			continue;
		}

		if (AlongEntry >= BestDistanceAlongRay)
		{
			continue;
		}

		BestDistanceAlongRay = AlongEntry;
		BestTarget = Target;
		BestImpact = ZoneImpact;
		BestZone = ZoneHit;
	}

	// NOTE: the ray is bounded by static geometry, but a capsule straddling a thin wall can still be
	// hit through it because the test is analytic and does not re-check occlusion at the entry
	// point. With arena geometry that is thicker than a body radius this cannot happen.

	if (BestTarget != nullptr)
	{
		OutImpactPoint = BestImpact;
		OutZone = BestZone;
	}

#if ENABLE_DRAW_DEBUG
	if (Settings.bDrawServerRewindDebug)
	{
		DrawDebugLine(World, Origin, RayEnd, (BestTarget != nullptr) ? FColor::Red : FColor::Yellow,
			/*bPersistentLines=*/false, /*LifeTime=*/1.5f, /*DepthPriority=*/0, /*Thickness=*/0.75f);
		DrawDebugPoint(World, OutImpactPoint, 10.f, (BestTarget != nullptr) ? FColor::Red : FColor::Yellow,
			/*bPersistentLines=*/false, /*LifeTime=*/1.5f);
	}
#endif

	if (BestTarget != nullptr)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ResolveHitscan: %s hit %s at t=%.3f (%.0fuu, zone=%s)"),
			*GetNameSafe(Shooter), *GetNameSafe(BestTarget), RewindToServerTime, BestDistanceAlongRay,
			TraceHitZoneToString(BestZone));
	}

	return BestTarget;
}
