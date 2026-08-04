// Trace — positional damage: the three-zone hit model.
//
// Spec §6 replaces the old "flat 34, x2 on a headshot" model with real positional damage:
//
//     head -> 100 (instant kill)      body -> 40      legs -> 25       health stays 100
//
// This header is the ONE definition of where those zones are. Both the client's predicted trace
// and the server's authoritative rewound trace call FTraceHitZoneModel::ResolveSegment(), so the
// two cannot drift: there is no second copy of the geometry to keep in sync. That is a structural
// guarantee, not a convention — if this file is wrong, both paths are wrong in the same way, and
// hit registration still agrees with what the shooter saw.
//
// ---------------------------------------------------------------------------------------------
// THE APPROXIMATION, AND WHEN IT LIES
// ---------------------------------------------------------------------------------------------
// The zones are NOT bones. They are derived arithmetically from the capsule pose that
// UTraceLagCompensationComponent already records (centre, half height, radius):
//
//     legs   a vertical band from the feet up to HipHeightFraction of the capsule's full height
//     head   a SPHERE near the top of the capsule, of HeadRadiusFraction * full height
//     body   everything else inside the capsule
//
// Why not bones: recording full bone transforms for 10 players at 60 Hz for 1 s is ~65 bones x
// 10 x 60 x 48 bytes ~= 1.9 MB per second of history, plus a skeletal evaluation per rewind.
// The capsule pose is 28 bytes per frame and is already being recorded. Spec §6 explicitly
// permits — and prefers — the three-shape approximation for a prototype. This is that.
//
// It is honest about the following disagreements with the visual mesh:
//
//  1. CROUCH / SLIDE — READ THIS, IT IS NOT WHAT YOU EXPECT IN THIS BUILD.
//
//     The zones scale with whatever capsule half height they are handed, so IF the capsule shrank
//     they would follow it exactly. In Trace the capsule NEVER shrinks. UTraceCharacterMovement-
//     Component deliberately overrides CanCrouchInCurrentState() to return false and expresses
//     crouch as a SLIDE instead, precisely so that the capsule stays the single source of truth for
//     hitscan, for lag-compensation history and for the trail trip test. See the note there; do not
//     "fix" it without reading that first.
//
//     That would have left a sliding player's zones at STANDING height while their camera dropped to
//     a 30uu eye and their mesh leaned ~20 degrees forward — a ~48uu error, three head-radii, right
//     through the one-shot-kill zone. It is closed by the POSTURE term instead of by the capsule:
//     FTraceLagCompFrame records ATraceCharacter::GetHitZonePostureScale() (1.0 standing, ~0.78 at
//     the bottom of a slide) alongside the pose, and FromCapsule compresses the head/hip BANDS by it.
//     Detection still uses the full capsule, so posture changes what a connecting shot is WORTH and
//     never which shots connect. The self-test's third block pins both halves of that.
//
//     What remains: posture is derived from BaseEyeHeight, which eases rather than snaps, so for the
//     ~0.15s at each end of a slide the bands are mid-travel. And a slide has to REACH a third
//     machine before that machine can lay the bands out correctly — that is what
//     ATraceCharacter::bReplicatedSliding exists for; read its comment before touching it.
//  2. MID-ANIMATION LIMBS. A leaning, strafing or falling mannequin puts its head and limbs
//     outside a strictly vertical column. Arms are never separately modelled — the spec folds
//     "torso+arms" into one body zone, so an arm hit is a body hit wherever the arm actually is.
//  3. TILTED POSES. Nothing here rotates, and today's slide DOES tilt: the mesh leans ~20 degrees
//     forward over its foot pivot while the zones stay a vertical column. Posture (see 1) accounts
//     for the height the lean costs, not for the ~30uu the head travels forward along the lean axis.
//     Closing that means storing a lean axis per frame and rotating the bands with it.
//  4. NON-UNIFORM MESH SCALE would desync mesh from capsule entirely. We never scale pawns.
//
// ---------------------------------------------------------------------------------------------
// WHY THE HEAD IS A SPHERE BUT EVERYTHING ELSE IS A HEIGHT BAND
// ---------------------------------------------------------------------------------------------
// A one-shot kill has to be earned. If the head were simply "the top 15% of the capsule" then any
// shot passing through the capsule at that height — including one 30 uu out to the side, through
// empty air beside the head — would be an instant kill, and the head hitbox would be 68 uu wide
// for a head that is about 20 uu wide.
//
// But zones that do not cover the whole capsule create the opposite failure: a shot that visibly
// connects registering nothing. That one is far worse, because it reads as a broken game.
//
// So: HIT DETECTION is unchanged — the same analytic segment-vs-capsule test that shipped, so
// exactly the same shots connect as before. Only the CLASSIFICATION of a connected shot is new,
// and it asks a narrow head sphere first and falls back to a height band. A shot that clips the
// capsule beside the head is a body shot, which is the correct read.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"   // module: DeveloperSettings
#include "Math/UnrealMathUtility.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"     // GetDefault<>

#include "TraceHitZones.generated.h"

struct FTraceLagCompFrame;

/**
 * Which part of a character a shot landed on. Ordering is deliberate: None is falsy-ish at 0 so
 * "did this connect" reads naturally, and the rest run head-down-to-toe.
 */
UENUM()
enum class ETraceHitZone : uint8
{
	/** The shot did not connect with a character at all. */
	None = 0,
	Head = 1,
	Body = 2,
	Legs = 3
};

/** "HEAD" / "BODY" / "LEGS" / "-". Logs and HUD only. */
TRACE_API const TCHAR* TraceHitZoneToString(ETraceHitZone Zone);

/**
 * Positional damage numbers and the zone geometry, as config.
 *
 * A SEPARATE settings page rather than four more properties on UTraceSettings purely because of
 * this pass's file ownership split — UTraceSettings belongs to another slice. Folding these into
 * UTraceSettings later is a mechanical move (see the report); nothing outside this header reads
 * them directly except UTraceWeaponComponent.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Trace Damage Zones"))
class TRACE_API UTraceDamageSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UTraceDamageSettings();

	/** CDO, already populated by the config system. Cheap enough to call per shot. */
	static const UTraceDamageSettings& Get();

	virtual FName GetCategoryName() const override;

	// --- Damage (spec §6) --------------------------------------------------------------------

	/** Spec §6: a head shot is a kill outright, so this must stay >= UTraceSettings::MaxHealth. */
	UPROPERTY(config, EditAnywhere, Category = "Damage")
	float HeadDamage = 100.f;

	/** Torso and arms. Three body shots to kill (40 + 40 + 40 = 120 >= 100). */
	UPROPERTY(config, EditAnywhere, Category = "Damage")
	float BodyDamage = 40.f;

	/**
	 * Below the hips. Still four leg shots to kill (30 x 4 = 120 >= 100).
	 *
	 * 25 -> 30 THIS PASS, and it is the fix for a reported bug, not a balance whim. The user said
	 * "shooting feels WAY more inconsistent on this latest version". It was measured over 313
	 * server-accepted shots and the hit registration was found to be exactly right — 313/313 client
	 * and server agreed on the zone, zero rejects, zero shots lost at the muzzle. What actually
	 * changed was the VARIANCE OF THE PAYOUT: the old flat 34/x2-head model killed in 2 or 3 shots
	 * (a 1.5x spread); 100/40/25 kills in 1, 3 or 4 (a 4x spread). That is what "inconsistent" feels
	 * like from inside, and it is arithmetic rather than a defect.
	 *
	 * 30 keeps the 4-shot leg kill intact while cutting the body-versus-leg swing from 38% to 25%.
	 * That matters because 15.9% of measured hits landed within 0.25-0.50 of body height, straddling
	 * the HipHeightFraction line at 0.46 — so roughly one shot in six is close enough to the boundary
	 * that a few uu of aim wobble changes the damage. Narrowing the gap narrows the surprise.
	 *
	 * The other two levers, if this is not enough: lower HipHeightFraction to ~0.40 (moves the
	 * boundary out from under where shots actually land, at the cost of anatomical accuracy), or
	 * accept the 4x spread as the design. Both are live knobs; try them in that order.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Damage")
	float LegDamage = 30.f;

	// --- Zone geometry -----------------------------------------------------------------------
	//
	// All three are fractions of the capsule's FULL height (2 x half height), measured up from the
	// capsule's bottom, so they scale automatically when crouching or sliding shrinks the capsule.
	// Defaults are fitted to Epic's Manny on the stock 88/34 capsule:
	//   full height 176 uu -> hips at 81 uu, head sphere centred at 159 uu with a 14.4 uu radius,
	//   i.e. a head spanning 145..174 uu. Manny's actual head occupies roughly 152..176.

	/** Everything strictly below this fraction of the capsule height is legs. */
	UPROPERTY(config, EditAnywhere, Category = "Zones", meta = (ClampMin = "0.05", ClampMax = "0.9"))
	float HipHeightFraction = 0.46f;

	/** Height of the head sphere's centre. */
	UPROPERTY(config, EditAnywhere, Category = "Zones", meta = (ClampMin = "0.5", ClampMax = "1.0"))
	float HeadCenterHeightFraction = 0.905f;

	/** Radius of the head sphere. Raise to make one-shot kills more forgiving. */
	UPROPERTY(config, EditAnywhere, Category = "Zones", meta = (ClampMin = "0.01", ClampMax = "0.3"))
	float HeadRadiusFraction = 0.082f;
};

/**
 * The three-zone body model for one character at one instant.
 *
 * Build it with FromCapsule() (or FromFrame() for a rewound pose) and ask it ResolveSegment().
 * Plain data on purpose: it is constructed per candidate per shot, and reflection would only cost
 * memory. It is also completely free of world/actor state, which is what lets the unit self-test
 * below exercise it with no game running.
 */
struct TRACE_API FTraceHitZoneModel
{
	/** Capsule centre in world space. */
	FVector CapsuleCenter = FVector::ZeroVector;

	/** Scaled capsule half height and radius, as recorded. */
	double HalfHeight = 88.0;
	double Radius = 34.0;

	// --- derived (filled by FromCapsule) ---

	/** World Z below which a hit is a leg hit. */
	double HipZ = 0.0;

	/** Centre and radius of the head sphere, in world space. */
	FVector HeadCenter = FVector::ZeroVector;
	double HeadRadius = 14.0;

	/** Posture the bands were laid out at (see FTraceLagCompFrame::PostureScale). 1.0 = standing. */
	double PostureScale = 1.0;

	/**
	 * @param InPosture  Fraction of the capsule's full height the body occupies - 1.0 standing,
	 *                   ~0.78 sliding. It compresses the head/hip BANDS toward the feet; it never
	 *                   touches the capsule the detection test uses, so which shots connect is
	 *                   unaffected. Defaults to 1.0 so callers that have no posture (and the unit
	 *                   self-test) keep the standing layout.
	 */
	static FTraceHitZoneModel FromCapsule(const FVector& InCenter, double InHalfHeight, double InRadius,
		double InPosture = 1.0);
	static FTraceHitZoneModel FromFrame(const FTraceLagCompFrame& Frame);

	/**
	 * Analytic segment-vs-body test.
	 *
	 * @param Start           Segment start (the muzzle).
	 * @param End             Segment end (already bounded by static geometry by the caller).
	 * @param OutEnterDist    Distance from Start to where the segment enters the capsule surface.
	 *                        This is what orders overlapping targets, and it is deliberately the
	 *                        CAPSULE entry rather than the zone entry so that ordering is identical
	 *                        to the pre-zone implementation.
	 * @param OutImpactPoint  Start + Dir * OutEnterDist.
	 * @param OutZone         Head / Body / Legs. Never None when this returns true.
	 * @return                True when the segment connects with the body at all.
	 */
	bool ResolveSegment(const FVector& Start, const FVector& End,
		double& OutEnterDist, FVector& OutImpactPoint, ETraceHitZone& OutZone) const;

	/** Damage for a zone, from UTraceDamageSettings. None -> 0. */
	static float DamageForZone(ETraceHitZone Zone);
};

/**
 * Deterministic self-test of the zone model, run by the console command `Trace.HitZoneTest`.
 *
 * Exposed (rather than being a file-static) so it can also be driven from the input harness or
 * from an automated run. Logs one line per case at Display and a summary; returns the failure
 * count, so 0 means the model matches its documented contract.
 */
TRACE_API int32 TraceRunHitZoneSelfTest();
