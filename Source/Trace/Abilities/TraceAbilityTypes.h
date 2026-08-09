// Trace — the ability framework's shared type layer (spec v14 §5).
//
// This header is deliberately tiny and dependency-light: it is what the five character files and
// every damage call site in the module include, so it must never pull in a gameplay class header.
//
// It declares four things and nothing else:
//
//   ETraceCharacterId       the roster, exactly as spec v14 §5 declares it.
//   ETraceAbilityEffect     what an ability is trying to do to a target. THE input to the carrier
//                           choke point, and the reason the choke point can be one function.
//   FTraceAbilityNetState   the fixed-shape replicated scratch pad each character interprets in its
//                           own way. See the comment on the struct for why it is fixed-shape.
//   FTraceAbilityDamageCtx  what a passive needs in order to modify a damage number.
//
// SPEC §4 — THE FOUNDING INVARIANT, AND WHERE IT LIVES.
// "NO abilities damage carriers, carriers can still only be killed when an enemy dashes through
// their trace." The rule is enforced in exactly ONE function — UTraceAbilityComponent::
// CanAffectTarget — and ETraceAbilityEffect is the vocabulary that lets that one function answer
// for all fifteen abilities instead of fifteen call sites each remembering.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"     // FVector_NetQuantize
#include "UObject/ObjectMacros.h"

#include "TraceAbilityTypes.generated.h"

class ATraceCharacter;

/**
 * The roster. Spec v14 §5, verbatim:
 *     UENUM() enum class ETraceCharacterId : uint8 { None=0, Rocco, Chut, Mace, Oyster, X };
 *
 * DO NOT REORDER AND DO NOT INSERT. The value is replicated as a uint8 and is persisted per player
 * for the length of a match; a renumber would silently swap two players' characters mid-match. New
 * characters APPEND, in front of Count.
 *
 * None is not "no character selected yet" in the loose sense — it is the DEFAULT CHARACTERLESS
 * MANNEQUIN, and it is a fully supported state that must keep working forever: it is what mode A
 * forces on everybody (spec §2), what bots always are (spec §3), and what the "turn off all
 * characters" toggle produces. Every hook in the framework must be a no-op at None.
 */
UENUM()
enum class ETraceCharacterId : uint8
{
	None   = 0 UMETA(DisplayName = "None (default Mannequin)"),
	Rocco  = 1,
	Chut   = 2,
	Mace   = 3,
	Oyster = 4,
	X      = 5,

	Count  UMETA(Hidden)
};

/** Number of real, selectable characters (excludes None). */
inline constexpr int32 TraceCharacterCount = static_cast<int32>(ETraceCharacterId::Count) - 1;

/** Stable, never localised — used in logs, console commands and the select screen's config. */
TRACE_API const TCHAR* TraceCharacterIdToString(ETraceCharacterId Id);

/** Parses "rocco" / "Rocco" / "3" / "none". Returns ETraceCharacterId::None on anything else. */
TRACE_API ETraceCharacterId TraceCharacterIdFromString(const FString& Value);

/**
 * WHAT AN ABILITY IS TRYING TO DO TO A TARGET.
 *
 * This exists so that spec §4's rule and spec §4's [ASSUMPTION] can be two different answers from
 * one function, and so that a designer can reverse the assumption without touching any ability.
 *
 *   Damage       Any health loss: Oyster's poison ticks, Pickler's 30, a bee, a Sting bullet.
 *                *** NEVER applies to a Core carrier. This is not a knob. ***
 *
 *   Control      Movement the target did not ask for, or a debuff: Chut's bash knockback, Pickler's
 *                pull, poison's -30% speed, X's vulnerable mark.
 *                *** Does not apply to a carrier either — but that half IS an [ASSUMPTION] and IS
 *                a knob (UTraceSettings::bCarrierImmuneToAbilityControl). ***
 *                Spec §4: "a carrier who can be yanked by Oyster's Pickler or slowed by poison is
 *                functionally disabled without being damaged, which is against the spirit." The doc
 *                itself already exempts the carrier from Chut's bash, so this generalises a rule the
 *                doc states rather than inventing one. Flip the knob to reverse it.
 *
 *   Beneficial   Something the target WANTS: riding Rocco's Ripple. Spec §6 is explicit that "any
 *                character, either team" may use the Ripple and that "the Core carrier can use it",
 *                so this class passes for carriers, for allies and for enemies alike. It exists so
 *                that "the choke point said no" can never be the reason a carrier fails to get a
 *                boost they are entitled to.
 */
UENUM()
enum class ETraceAbilityEffect : uint8
{
	Damage     = 0,
	Control    = 1,
	Beneficial = 2
};

TRACE_API const TCHAR* TraceAbilityEffectToString(ETraceAbilityEffect Effect);

/**
 * Why a target could not be affected. Returned by CanAffectTargetDetailed() purely so that logs,
 * the verification harness and a designer reading a trace can tell "the carrier rule stopped this"
 * apart from "they were already dead".
 */
UENUM()
enum class ETraceAbilityBlockReason : uint8
{
	/** Not blocked. */
	Allowed = 0,
	/** No target, no world, or the target's pawn has gone. */
	NoTarget,
	/** Target is dead. */
	Dead,
	/** Target is on the instigator's team and friendly fire is off. */
	SameTeam,
	/** Target is the instigator. */
	Self,
	/** *** THE FOUNDING INVARIANT. Target is the Core carrier and this was damage. *** */
	CarrierDamageImmune,
	/** Target is the Core carrier and this was a slow / pull / knockback. See the [ASSUMPTION]. */
	CarrierControlImmune,
	/** The whole character system is switched off (spec §3 toggle, or mode A). */
	CharactersDisabled
};

TRACE_API const TCHAR* TraceAbilityBlockReasonToString(ETraceAbilityBlockReason Reason);

/**
 * The per-character replicated scratch pad.
 *
 * WHY ONE FIXED STRUCT AND NOT A REPLICATED SUBOBJECT PER CHARACTER.
 * Five agents are about to write five characters in parallel. A replicated UObject subobject per
 * character would be correct in the abstract and would mean five separate additions to the
 * component's replication path — five chances to break everyone else's character with a merge, on
 * top of UE's registered-subobject-list machinery. This struct is ONE replicated property, added
 * once, and each character interprets the fields as its own. Nothing here is shared between
 * characters, because only one character is ever active on one component at a time.
 *
 * Fields, and the §6 uses they were sized for:
 *
 *   Stacks        Rocco's headshot speed stack; X's bees remaining in the gun (Sting).
 *   Flags         a free bitfield: Mace "suspending", Mace "spike embedded", Chut "Chud active",
 *                 X "bees loaded", Rocco "riding a ripple". Use TraceAbilityFlags:: below or your
 *                 own bit constants in your own file — nobody else reads your bits.
 *   EffectEndMatchTime   absolute server time an effect ends: Chud's 10 s, X's vulnerable 2 s,
 *                 Mace's 1.25 s suspend, poison's 4 s.
 *   AuxEndMatchTime      a second, independent timer: Rocco's 1 s speed-stack window, Mace's 2 s
 *                 spike embed, Oyster's ripple/jar lifetimes.
 *   AuxLocation   a world point: Mace's spike anchor, Rocco's ripple origin, a jar's landing spot.
 *   AuxDirection  a direction: the ripple's path, the bash's travel direction.
 *
 * ALL TIMES ARE ABSOLUTE MATCH-CLOCK TIMES (AGameStateBase::GetServerWorldTimeSeconds), never
 * durations and never per-life timers — see the cooldown contract on UTraceAbilityComponent.
 */
USTRUCT()
struct TRACE_API FTraceAbilityNetState
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Stacks = 0;

	UPROPERTY()
	uint8 Flags = 0;

	UPROPERTY()
	float EffectEndMatchTime = 0.f;

	UPROPERTY()
	float AuxEndMatchTime = 0.f;

	UPROPERTY()
	FVector_NetQuantize AuxLocation = FVector::ZeroVector;

	UPROPERTY()
	FVector_NetQuantize AuxDirection = FVector::ZeroVector;

	/** Wipes every field. Called at half time and whenever the character changes. */
	void Reset()
	{
		Stacks = 0;
		Flags = 0;
		EffectEndMatchTime = 0.f;
		AuxEndMatchTime = 0.f;
		AuxLocation = FVector::ZeroVector;
		AuxDirection = FVector::ZeroVector;
	}

	bool operator==(const FTraceAbilityNetState& Other) const
	{
		return Stacks == Other.Stacks
			&& Flags == Other.Flags
			&& EffectEndMatchTime == Other.EffectEndMatchTime
			&& AuxEndMatchTime == Other.AuxEndMatchTime
			&& AuxLocation == Other.AuxLocation
			&& AuxDirection == Other.AuxDirection;
	}
};

/**
 * Suggested bit names for FTraceAbilityNetState::Flags. Purely a convention — the bits belong to
 * whichever character is active, and no two characters are active at once, so collisions between
 * these names are impossible by construction. Listed here so five files do not each invent 1<<0.
 */
namespace TraceAbilityFlags
{
	inline constexpr uint8 EffectActive   = 1 << 0;   // Chud up, vulnerable applied, poison ticking
	inline constexpr uint8 MovementActive = 1 << 1;   // Mace suspending, Mace being pulled
	inline constexpr uint8 AuxActive      = 1 << 2;   // spike embedded, ripple alive, bees loaded
	inline constexpr uint8 Charged        = 1 << 3;   // free
	inline constexpr uint8 Free4          = 1 << 4;
	inline constexpr uint8 Free5          = 1 << 5;
	inline constexpr uint8 Free6          = 1 << 6;
	inline constexpr uint8 Free7          = 1 << 7;
}

/**
 * Everything a passive needs in order to change a damage number, in one argument.
 *
 * Chut's Chud is "30% less damage from body shots and melees" — it must be able to tell a body shot
 * from a headshot and a melee from a bullet, and the [ASSUMPTION] in §6 is that headshots and trace
 * deaths are unaffected. X's vulnerable is "+25% damage from all sources" and needs none of it. One
 * context type serves both and every future one.
 */
USTRUCT()
struct TRACE_API FTraceAbilityDamageContext
{
	GENERATED_BODY()

	/** The pawn dealing the damage. May be null (world damage, an expired jar). */
	UPROPERTY()
	TObjectPtr<ATraceCharacter> Instigator = nullptr;

	/** The pawn taking it. Never null at a real call site. */
	UPROPERTY()
	TObjectPtr<ATraceCharacter> Target = nullptr;

	/** The damage cause FName the health component was given ("Bullet", "Knife", "Poison", ...). */
	UPROPERTY()
	FName Cause;

	/** True when the hit resolved on the head zone. Chud must NOT reduce these (§6 [ASSUMPTION]). */
	UPROPERTY()
	bool bHeadshot = false;

	/** True for the knife. Chud DOES reduce these ("body shots and melees"). */
	UPROPERTY()
	bool bMelee = false;

	/** True when this damage came from an ability rather than a weapon. */
	UPROPERTY()
	bool bFromAbility = false;
};

/**
 * SPEC v14 §6 — EXTERNAL DEBUFFS: what OTHER players' abilities have done to a pawn.
 *
 * ===================================================================================================
 * WHY THIS IS NOT UTraceCharacterAbilitySet::GetMoveSpeedMultiplier()
 * ===================================================================================================
 *
 * That hook asks a pawn's OWN character about its OWN passive — Rocco's headshot stack, X's +10%.
 * It is the right question for a buff and the wrong one for a debuff, because spec §3 makes bots
 * characterless: a bot has no ability set to ask, so Oyster's poison slow routed through that hook
 * would silently fail to slow more than half the pawns in a bot match. The victim of a debuff is not
 * necessarily a character. (This is the same argument that put X's vulnerable mark on
 * UTraceHealthComponent rather than on GetIncomingDamageMultiplier.)
 *
 * ONE AGGREGATOR, and every provider is a line inside it. UTraceCharacterMovementComponent calls
 * exactly this and knows about no particular ability; the definition lives in TraceAbilityTypes.cpp
 * next to the one provider there is today. A second debuff appends a line there and edits nothing
 * else in the movement slice.
 */
namespace TraceAbilityDebuff
{
	/**
	 * Aggregate ground-speed multiplier applied TO @p Target by other players' abilities.
	 * 1.0 when nothing is on them, which is the overwhelmingly common case and is answered by one
	 * FindComponentByClass that finds nothing.
	 */
	TRACE_API float GetMoveSpeedMultiplier(const AActor* Target);
}
