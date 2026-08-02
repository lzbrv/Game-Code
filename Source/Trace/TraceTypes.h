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

/** Lifecycle of the single shared Core object. */
UENUM(BlueprintType)
enum class ECoreState : uint8
{
	/** On the ground (or resting), pickable by anyone. */
	Loose    = 0,
	/** Attached to a carrier. */
	Carried  = 1,
	/** Thrown/passed, travelling under projectile movement. */
	InFlight = 2
};

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

/** Canonical team colour. Used for meshes, tracers, trail and HUD so everything matches. */
TRACE_API inline FLinearColor TraceTeamColor(ETraceTeam Team)
{
	switch (Team)
	{
	case ETraceTeam::Blue:   return FLinearColor(0.05f, 0.75f, 1.00f, 1.0f);
	case ETraceTeam::Orange: return FLinearColor(1.00f, 0.42f, 0.05f, 1.0f);
	default:                 return FLinearColor(0.50f, 0.50f, 0.50f, 1.0f);
	}
}

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
};
