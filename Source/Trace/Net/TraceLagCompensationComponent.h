#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Gameplay/TraceHitZones.h"   // ETraceHitZone, FTraceHitZoneModel
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
 *
 * --- REWINDING THREE ZONES, NOT ONE CAPSULE (spec section 6) --------------------------------
 *
 * Positional damage means the rewind has to reconstruct head / body / legs at the rewound time,
 * not just "a body". It does so WITHOUT storing any extra history: FTraceHitZoneModel derives all
 * three zones arithmetically from the capsule centre, half height and radius that this buffer has
 * always recorded, so a rewound pose reconstructs its zones exactly as a live pose does.
 *
 * That is a deliberate memory/fidelity trade. The honest accounting:
 *
 *   what it costs       nothing. History stays at 28 bytes per frame per player - 10 players at
 *                       60 Hz for 1 s is ~17 KB total. Storing per-bone transforms instead would
 *                       be on the order of 2 MB/s of history plus a skeletal evaluation per
 *                       rewind, for a prototype that does not need it.
 *   what it buys        the zones structurally cannot desync from the capsule, because they ARE the
 *                       capsule expressed differently. Any future change to capsule size, at any
 *                       point in the recorded window, is tracked for free.
 *   where it lies       the zones are an upright column at the capsule's dimensions. They do not
 *                       track a leaning, strafing or mid-stride mesh, and arms are folded into the
 *                       body zone wherever the arm actually is.
 *
 *                       IMPORTANT and easy to get wrong: this build's crouch does NOT shrink the
 *                       capsule. Crouch is a SLIDE, and the movement component keeps the capsule at
 *                       full size on purpose (see UTraceCharacterMovementComponent::
 *                       CanCrouchInCurrentState).
 *
 *                       FIXED AT INTEGRATION, and this is the one thing NOT to regress: the frame
 *                       now also records FTraceLagCompFrame::PostureScale (from
 *                       ATraceCharacter::GetHitZonePostureScale), so the head/hip bands compress
 *                       toward the feet with the slide and rewind with it. Before that term
 *                       existed, a slider's head sphere floated ~48uu above the head the shooter
 *                       could see: aiming at the visible head scored BODY and aiming at empty air
 *                       above it scored a one-shot kill. Posture is 33 bytes/frame -> 32, and it
 *                       moves only zone CLASSIFICATION; hit DETECTION is still the full capsule, so
 *                       exactly the same shots connect. Full accounting in Gameplay/TraceHitZones.h.
 *
 *                       Still unmodelled: the 20-degree forward mesh lean during a slide, and arms.
 *
 * Both the client's predicted trace and this server-authoritative one call ResolveHitscan(), which
 * calls FTraceHitZoneModel::ResolveSegment(). There is exactly one copy of the zone geometry in
 * the build, so the two paths cannot disagree about what a zone is - only about the pose, which is
 * what the rewind exists to reconcile.
 */
/**
 * Optional per-shot diagnostics filled in by ResolveHitscan().
 *
 * NOT gameplay. It exists because "shooting feels inconsistent" is an impression, and the only way
 * to answer an impression is with a distribution. Every field here is something that was previously
 * only knowable by reading the code and guessing:
 *
 *  - whether the world trace truncated the shot, and how short (a muzzle clipping cover would make
 *    shots evaporate at point-blank range, which is exactly what "inconsistent near cover" means),
 *  - the pose the shot was actually resolved against, and whether it came from rewound history or
 *    from the live capsule,
 *  - WHERE on the body the shot landed, as a fraction of the target's height, which is the only
 *    honest way to sanity-check the zone bands against where players really aim, and
 *  - what the zone WOULD have been had classification used the ray's closest approach to the body
 *    axis instead of the capsule ENTRY point. The two differ on any shot that is not horizontal,
 *    and the difference is a whole damage tier.
 *
 * Passing null (the default) costs one branch per shot.
 */
struct FTraceHitscanDiagnostics
{
	/** The live world trace stopped the ray on static geometry before it reached full range. */
	bool bWorldTraceHit = false;

	/** The muzzle started inside geometry. Chaos reports this with Distance 0. */
	bool bWorldStartPenetrating = false;

	/** Distance the world trace allowed, or -1 when the ray ran to full range. */
	double WorldHitDistance = -1.0;

	/** True when a character was hit and the fields below are meaningful. */
	bool bHaveVictim = false;

	/** The pose the winning target was resolved against. */
	FTraceLagCompFrame VictimFrame;

	/** True when VictimFrame came from recorded history rather than the live capsule. */
	bool bVictimPoseRewound = false;

	/** Height of the CAPSULE ENTRY point above the target's feet, over their full capsule height. */
	double EntryHeightFraction = -1.0;

	/** Same, for the ray's closest approach to the capsule axis. */
	double ClosestHeightFraction = -1.0;

	/** The zone the shot scored (entry-point classification - what the game actually used). */
	ETraceHitZone Zone = ETraceHitZone::None;

	/** The zone the same shot would have scored classified at closest approach. */
	ETraceHitZone ZoneAtClosestApproach = ETraceHitZone::None;
};

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
	 * @param OutZone              Head / Body / Legs on a character hit, None otherwise. Drives
	 *                             positional damage (spec section 6) - see Gameplay/TraceHitZones.h.
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
		ETraceHitZone& OutZone,
		FTraceHitscanDiagnostics* OutDiagnostics = nullptr);

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
};
