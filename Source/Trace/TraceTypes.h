// Trace — shared type layer.
//
// This header is the single point of truth every other Trace file codes against: the team /
// core / trail / match enums, the delta-replicated trail vertex types, the lag-compensation
// pose record, and three small free helpers.
//
// Keep this header dependency-light. It is included by nearly every other file in the module,
// so it must never pull in a gameplay class header (that is what the forward declaration of
// UTraceTrailComponent below is for).

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"          // FVector_NetQuantize
#include "Internationalization/Text.h"        // FText / NSLOCTEXT
#include "Math/Color.h"                       // FLinearColor
#include "Net/Serialization/FastArraySerializer.h" // module: NetCore
#include "UObject/Class.h"                    // TStructOpsTypeTraitsBase2
#include "UObject/CoreNet.h"                  // FNetDeltaSerializeInfo
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.generated.h"

// Only ever dereferenced from TraceSettings.cpp (see the note on the replication callbacks
// below). Forward declared here so TraceTypes.h stays free of gameplay includes.
class UTraceTrailComponent;


/** Team identity. Blue and Orange contest one shared Core. */
UENUM(BlueprintType)
enum class ETraceTeam : uint8
{
	None   = 0,
	Blue   = 1,
	Orange = 2
};

// ECoreState IS DELETED. It described a physical Core that no longer exists: InFlight ("travelling
// under projectile movement") was unreachable because nothing flies, and Loose was documented as "on
// the ground, pickable by anyone" when there is no pickup at all. Its one holder, the replicated
// ATraceCore::State, was written twice and read by nothing. "Holderless" is ATraceCore::Carrier
// == nullptr, which is the fact every caller actually wanted.

/** Who dies when an eligible player trips the carrier's trail. */
UENUM(BlueprintType)
enum class ETrailLethality : uint8
{
	/** Default game rule: dashing through the trail kills the *carrier*. */
	KillsCarrier = 0,
	/** Inverted rule, for tuning experiments: the player who touched the trail dies. */
	KillsToucher = 1,
	/** Both die. */
	KillsBoth    = 2
};

/** High level match phase, replicated on the GameState. */
UENUM(BlueprintType)
enum class ETraceMatchState : uint8
{
	WaitingForPlayers = 0,
	InProgress        = 1,
	PostMatch         = 2
};


// ---------------------------------------------------------------------------------------------
// Free helpers.
//
// Declared TRACE_API per the build contract and *defined inline right here* so that other
// files may call them from their own inline/header code without a link dependency. Giving an
// inline function the module API macro is legal on every toolchain we target: on Windows the
// macro resolves to __declspec(dllexport) inside this module (exporting an inline definition is
// fine) and is a no-op in monolithic builds; on Apple Silicon it is a visibility attribute.
//
// *** TWO OF THE THREE. TraceTeamColor IS NOT ONE OF THEM ANY MORE (spec v26 §5). *** Its two
// colours are now DERIVED from the artist's menu palette, TraceMenuArtStyle, rather than typed
// here as literals — and TraceMenuArtStyle.h pulls in Fonts/SlateFontInfo.h, which this header
// must never hand to the ~every-file-in-the-module list that includes it ("Keep this header
// dependency-light", top of file). So it is DECLARED here and DEFINED in TraceTypes.cpp, which
// is the only new cost: a link dependency inside this one module, which every caller already
// has. Nothing outside the module calls it. See TraceTypes.cpp for the derivation and for the
// two candidate palettes §5 asked to be reported.
// ---------------------------------------------------------------------------------------------

/** Returns the team that opposes @p Team. None maps to None (no opponent). */
TRACE_API inline ETraceTeam TraceOpposingTeam(ETraceTeam Team)
{
	switch (Team)
	{
	case ETraceTeam::Blue:   return ETraceTeam::Orange;
	case ETraceTeam::Orange: return ETraceTeam::Blue;
	default:                 return ETraceTeam::None;
	}
}

/**
 * Canonical team colour. Used for meshes, tracers, trail and HUD so everything matches — and
 * that list is exhaustive on purpose: this one function is the only place any of the four asks
 * what a team looks like, so §5's "check all four" is one edit rather than four.
 *
 * SPEC v26 §5. NOT A LITERAL ANY MORE. Defined in TraceTypes.cpp, derived from the artist's
 * menu palette (TraceMenuArtStyle::PlateFill and ::Amber). The reasoning, the two candidate
 * palettes, and the console switch that shows the other one, are all there.
 */
TRACE_API FLinearColor TraceTeamColor(ETraceTeam Team);

/**
 * One line naming the live team palette and where it came from, for the log and `Trace.Team.Report`.
 * A colour that is derived rather than typed is invisible in a diff, so the running game says it.
 */
TRACE_API FString TraceDescribeTeamColors();

/** Display name for HUD / scoreboard / banners. */
TRACE_API inline FText TraceTeamName(ETraceTeam Team)
{
	switch (Team)
	{
	case ETraceTeam::Blue:   return NSLOCTEXT("Trace", "TeamName_Blue",   "Blue");
	case ETraceTeam::Orange: return NSLOCTEXT("Trace", "TeamName_Orange", "Orange");
	default:                 return NSLOCTEXT("Trace", "TeamName_None",   "Spectator");
	}
}


// ---------------------------------------------------------------------------------------------
// Trail replication
// ---------------------------------------------------------------------------------------------

/**
 * One replicated trail vertex, delta-replicated via FFastArraySerializer so that appending a
 * point costs one item on the wire instead of resending the whole array.
 *
 * The three replication callbacks are *declared* here and *defined* in TraceSettings.cpp.
 * They need to call UTraceTrailComponent::OnTrailPointsChanged(), but TraceTrailComponent.h
 * includes this header (it declares `UPROPERTY(Replicated) FTraceTrailPointArray TrailPoints`),
 * so including it back here would be a hard include cycle. Defining the bodies in a .cpp that
 * *can* include the component header breaks the cycle without a second header and without
 * duplicating the trail component's interface. TraceSettings.cpp is that .cpp — it is the only
 * translation unit in this ownership slice, so the definitions live there.
 *
 * These are name-detected (non-virtual) hooks: the base FFastArraySerializerItem declares empty
 * versions taking `const FFastArraySerializer&`; declaring ours with the concrete serializer
 * type hides them, and FastArrayDeltaSerialize's call site resolves to ours exactly.
 */
USTRUCT()
struct FTraceTrailPoint : public FFastArraySerializerItem
{
	GENERATED_BODY()

	/** World-space centre of this trail vertex. Quantised — trail collision is coarse anyway. */
	UPROPERTY()
	FVector_NetQuantize Location = FVector::ZeroVector;

	/** Server world time (AGameStateBase::GetServerWorldTimeSeconds) at which this point was laid. */
	UPROPERTY()
	float BirthServerTime = 0.f;

	void PostReplicatedAdd(const struct FTraceTrailPointArray& Serializer);
	void PostReplicatedChange(const struct FTraceTrailPointArray& Serializer);
	void PreReplicatedRemove(const struct FTraceTrailPointArray& Serializer);
};

/**
 * The replicated trail itself. Owned by UTraceTrailComponent, which must point
 * OwnerComponent at itself (in its constructor / BeginPlay) so the item callbacks can find it
 * on clients.
 */
USTRUCT()
struct FTraceTrailPointArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FTraceTrailPoint> Items;

	/**
	 * Back pointer to the owning component. NotReplicated: each side sets its own local pointer;
	 * replicating an object reference inside a fast array item container would be pointless
	 * churn and would fight the delta bookkeeping.
	 */
	UPROPERTY(NotReplicated)
	TObjectPtr<class UTraceTrailComponent> OwnerComponent = nullptr;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FTraceTrailPoint, FTraceTrailPointArray>(Items, DeltaParms, *this);
	}
};

// Must sit at global scope, after the struct, before any use of the struct's net traits.
template<>
struct TStructOpsTypeTraits<FTraceTrailPointArray> : public TStructOpsTypeTraitsBase2<FTraceTrailPointArray>
{
	enum { WithNetDeltaSerializer = true };
};


// ---------------------------------------------------------------------------------------------
// Lag compensation
// ---------------------------------------------------------------------------------------------

/**
 * One historical capsule pose, recorded server-side each tick by
 * UTraceLagCompensationComponent and replayed as pure math during hitscan rewind.
 *
 * Deliberately plain (no UPROPERTY members, no UObject references): these are stored by the
 * hundred in a ring buffer and never replicated, so reflection would only cost memory. The
 * USTRUCT wrapper exists so the type can appear in reflected function signatures.
 */
USTRUCT()
struct FTraceLagCompFrame
{
	GENERATED_BODY()

	/** Server world time this pose was captured at. */
	float ServerTime = 0.f;

	/** Capsule centre in world space (i.e. the character's root location). */
	FVector CapsuleCenter = FVector::ZeroVector;

	/** Half height of the capsule at capture time (crouching changes it). */
	float CapsuleHalfHeight = 88.f;

	/** Radius of the capsule at capture time. */
	float CapsuleRadius = 34.f;

	/**
	 * Posture: the fraction of the capsule's full height that the BODY actually occupies at capture
	 * time. 1.0 standing; ~0.78 at the bottom of a slide.
	 *
	 * This exists because of a cross-system conflict that is easy to miss. This build's crouch is a
	 * SLIDE, and UTraceCharacterMovementComponent::CanCrouchInCurrentState() returns false on
	 * purpose so the capsule is NEVER resized - the capsule is the single source of truth for
	 * hitscan, for this rewind buffer and for the trail trip test. But ATraceCharacter drops
	 * BaseEyeHeight and leans the mesh while sliding. Without this term the zone model would keep a
	 * slider's head sphere at standing height, i.e. floating ~48uu above the head the shooter can
	 * actually see: aiming at a slider's visible head would score BODY, and aiming at the empty air
	 * above them would score a one-shot HEAD kill.
	 *
	 * Recording posture rather than shrinking the capsule keeps hit DETECTION byte-for-byte
	 * unchanged (which shots connect) and moves only zone CLASSIFICATION (what a connecting shot is
	 * worth). It costs 4 bytes per frame, and it rewinds with the pose for free.
	 */
	float PostureScale = 1.f;
};
