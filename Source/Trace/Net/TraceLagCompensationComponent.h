#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TraceTypes.h"
#include "TraceLagCompensationComponent.generated.h"

class UWorld;

/**
 * Server-side rewind ("lag compensation") for hitscan.
 *
 * One of these lives on every ATraceCharacter. On the server it samples its owner's capsule pose
 * once per frame into a short history buffer. When a shot arrives, UTraceWeaponComponent asks the
 * static ResolveHitscan() to answer "what would this ray have hit at time T?".
 *
 * The defining property of this implementation: **no actor is ever moved.** Rewinding by
 * teleporting capsules backwards, resolving, and teleporting them forward again is the classic
 * approach and it is a source of physics/overlap/animation bugs and of subtle desync for anything
 * else ticking that frame. Here the rewind is pure math - the historical pose is a plain struct and
 * the ray/character test is an analytic segment-vs-capsule check. The world itself is traced once,
 * live, purely to find out how far the ray gets before static geometry stops it.
 */
UCLASS(ClassGroup = (Trace), meta = (BlueprintSpawnableComponent))
class TRACE_API UTraceLagCompensationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTraceLagCompensationComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Appends the owner's current capsule pose to the history buffer and trims anything older than
	 * UTraceSettings::LagCompHistoryDuration. Server only; a no-op elsewhere.
	 * Safe to call more than once per frame - samples with a non-increasing timestamp are dropped.
	 */
	void RecordFrame(float ServerTime);

	/**
	 * Reconstructs the owner's pose at an arbitrary point in the recorded window by interpolating
	 * between the two bracketing samples.
	 * Returns false when nothing has been recorded yet, or when ServerTime predates the buffer -
	 * we refuse to extrapolate backwards past what we know. A request newer than the latest sample
	 * clamps to that sample and returns true.
	 */
	bool GetPoseAtTime(float ServerTime, FTraceLagCompFrame& Out) const;

	/**
	 * Resolves one hitscan ray against every eligible character's *historical* pose.
	 *
	 * @param World                Server world.
	 * @param Shooter              Character that fired; always excluded from the results.
	 * @param Origin               Muzzle position, in world space.
	 * @param Direction            Shot direction; normalised internally, need not be unit length.
	 * @param Range                Maximum shot distance in uu.
	 * @param RewindToServerTime   Shared-clock timestamp to rewind to. The caller is responsible for
	 *                             having clamped this into [Now - MaxRewindTime, Now].
	 * @param OutImpactPoint       Set to the character impact when one is hit, otherwise to the
	 *                             static-geometry impact, otherwise to the far end of the ray. Always
	 *                             written, so it can be fed straight to a tracer.
	 * @param bOutHeadshot         True when the impact landed in the upper part of the capsule.
	 * @return                     The nearest character hit, or nullptr.
	 *
	 * Skips the shooter, dead characters, the Core carrier (invulnerable to bullets by design) and,
	 * unless UTraceSettings::bFriendlyFire, the shooter's own team.
	 */
	static class ATraceCharacter* ResolveHitscan(
		UWorld* World,
		class ATraceCharacter* Shooter,
		const FVector& Origin,
		const FVector& Direction,
		float Range,
		float RewindToServerTime,
		FVector& OutImpactPoint,
		bool& bOutHeadshot);

private:
	/**
	 * Pose ring, oldest first, newest last, trimmed by age each time a sample is added. Not a
	 * UPROPERTY on purpose: FTraceLagCompFrame is plain data with no UObject references, so there is
	 * nothing here for the GC to keep alive and nothing worth the reflection overhead.
	 */
	TArray<FTraceLagCompFrame> History;

	void TrimHistory(float NewestServerTime);

	/** Hard ceiling so a hitch or a pathological tick rate cannot grow the buffer without bound. */
	static constexpr int32 MaxHistoryFrames = 512;

	/** Fraction of the capsule half height above the centre that counts as a head. */
	static constexpr float HeadshotHeightFraction = 0.55f;
};
