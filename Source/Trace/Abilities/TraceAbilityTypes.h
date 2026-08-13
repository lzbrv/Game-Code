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
 * The roster. Spec v14 §5 declared the first five:
 *     UENUM() enum class ETraceCharacterId : uint8 { None=0, Rocco, Chut, Mace, Oyster, X };
 * SPEC v18 §2 APPENDED THREE — Roxie, Elle, Slimeball — taking the roster from 5 to 8, and
 * SPEC v19 §3 APPENDS TWO MORE — Mortimer, Lily — taking it from 8 to 10.
 *
 * DO NOT REORDER AND DO NOT INSERT. The value is replicated as a uint8 and is persisted per player
 * for the length of a match; a renumber would silently swap two players' characters mid-match. New
 * characters APPEND, in front of Count — which is exactly what v18 did, so a save or a replay from
 * the v17 build still names the same five characters it always did.
 *
 * *** THE THREE PLACES THAT MUST MOVE WITH THIS ENUM, and the one that shouts when they do not. ***
 *   1. TraceCharacterRoster::LastId / ::Count (Core/TraceCharacterRoster.h) — the UI slice's copy of
 *      the id space, kept honest by static_asserts at the top of TraceCharacterRoster.cpp. Appending
 *      here without moving those is a COMPILE ERROR, deliberately, and that is the whole point.
 *   2. A row in the roster's C++ table, and a regenerated DA_Character_<Name> asset beside the
 *      others — the asset table is ALL-OR-NONE, so one missing asset drops EVERY character back to
 *      the C++ values.
 *   3. TraceCharacterIdToString below, or every log line and console command prints "<invalid>".
 *
 * None is not "no character selected yet" in the loose sense — it is the DEFAULT CHARACTERLESS
 * MANNEQUIN, and it is a fully supported state that must keep working forever: it is what mode A
 * forces on everybody (spec §2), what bots always are (spec §3), and what the "turn off all
 * characters" toggle produces. Every hook in the framework must be a no-op at None.
 */
UENUM()
enum class ETraceCharacterId : uint8
{
	None      = 0 UMETA(DisplayName = "None (default Mannequin)"),
	Rocco     = 1,
	Chut      = 2,
	Mace      = 3,
	Oyster    = 4,
	X         = 5,

	// --- spec v18 §2, appended ---------------------------------------------------------------
	Roxie     = 6,
	Elle      = 7,
	Slimeball = 8,

	// --- spec v19 §3, appended -----------------------------------------------------------------
	// Appended, never inserted, for the reason stated above: a v18 replay or a mid-match player state
	// that says "8" still means Slimeball on this build and on that one.
	Mortimer  = 9,
	Lily      = 10,

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
 * It is the right question for a buff and the wrong one for a debuff, because THE VICTIM OF A DEBUFF
 * IS NOT NECESSARILY A CHARACTER: mode A freezes everybody to the Mannequin (spec v14 §2), the
 * characters toggle does the same, and anybody a full team roster cannot serve stays characterless
 * too. Routed through the pawn's own set, Oyster's poison slow would silently do nothing to all of
 * them. (This is the same argument that put X's vulnerable mark on UTraceHealthComponent rather than
 * on GetIncomingDamageMultiplier.)
 *
 * This used to say "spec §3 makes bots characterless", which spec v15 §2 reversed — bots hold real
 * characters now. The reasoning above never depended on that and the code is unchanged; only the
 * example was wrong, and an aggregator justified by a rule that no longer exists is an invitation to
 * delete it.
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

/**
 * ===================================================================================================
 * SPEC v19 §3 — WHAT A CHARACTER CHANGES ABOUT A SHARED SYSTEM IT DOES NOT OWN.
 * ===================================================================================================
 *
 * Mortimer and Lily are the first two characters whose passives are not expressible through any hook
 * on UTraceCharacterAbilitySet, because the number they change belongs to a system in another slice:
 * the dash's reach, the size of the dash charge pool, the wall jump's momentum, max health, whether a
 * mantle may be attempted at all, and how long the Core may be charged.
 *
 * THE SAME ARGUMENT TraceAbilityDebuff MAKES, AND THE SAME SHAPE.
 * "THE MOVEMENT COMPONENT MUST NOT KNOW ABOUT CHUT" is already this project's rule (see
 * UTraceCharacterAbilitySet::GetDashHitSweepRadius). It must not know about Mortimer or Lily either.
 * So each function below is ONE call in somebody else's file that names no character, takes an
 * AActor* it already has, and is null-safe for every pawn that has no ability component at all — a
 * Mannequin, a bot mid-spawn, a pawn on a client before the PlayerState has replicated. Every one
 * returns the IDENTITY value in that case, which is what makes "the shipped feel of the other eight
 * characters is byte-identical" true by construction rather than by inspection.
 *
 * THE PROVIDERS ARE LINES INSIDE TraceAbilityTypes.cpp. A future character that wants one of these
 * appends a branch there and edits nothing in Movement/, Gameplay/ or Core/.
 *
 * *** THESE ARE QUERIES ABOUT ONE PAWN. *** Nothing here writes, nothing here allocates, and every
 * one is called from a per-frame path, so each is one component lookup and one virtual call.
 */
namespace TraceAbilityTraits
{
	/**
	 * Multiplier on DASH REACH for @p Actor — the dash's speed, so reach scales with it while the
	 * dash's DURATION (and therefore the trace it leaves, and the parry window) is untouched.
	 *
	 * 1.0 for everybody but Mortimer. Spec v19 §3 said "his dash is 75% SHORTER than normal";
	 * DEMO 20 ITEM 2 revised that to "40% of a normal one instead of 25%", so the value is 0.40.
	 *
	 * CALL SITE: *** LIVE. *** UTraceCharacterMovementComponent::GetDashSpeed() multiplies by this.
	 */
	TRACE_API float GetDashDistanceScale(const AActor* Actor);

	/**
	 * Multiplier on @p Actor's DASH COOLDOWN — the gap between charges, not the dash itself.
	 *
	 * 1.0 for everybody but Mortimer. DEMO 20 ITEM 2: "increase mortimer's dash cooldown by 25%",
	 * so 1.25 for him, against the shared UTraceSettings::DashCooldown of 3.5 s.
	 *
	 * *** CALL SITE NEEDED, AND IT DOES NOT EXIST YET. ***
	 * UTraceCharacterMovementComponent::GetDashCooldown() must become
	 *
	 *     return FMath::Max(0.f, UTraceSettings::Get().DashCooldown
	 *         * TraceAbilityTraits::GetDashCooldownScale(CharacterOwner));
	 *
	 * and nothing else changes: GetDashRechargeWindow() is defined as duration + cooldown, so the
	 * HUD's dash meter follows on its own. Source/Trace/Movement/ is owned by another agent in the
	 * pass that added this trait, which is the only reason the line is not already there.
	 *
	 * UNTIL IT IS, THIS FUNCTION IS READ BY NOBODY AND MORTIMER'S DASH RECHARGES LIKE EVERYBODY
	 * ELSE'S. Trace.Mortimer.Verify says so on every run and Trace.Mortimer.DashTest measures the
	 * shipped movement component rather than the knob, so it goes red on exactly this gap. Do not
	 * delete either of those warnings without deleting this paragraph — a knob nothing reads that
	 * nothing complains about is this project's single most repeated failure.
	 */
	TRACE_API float GetDashCooldownScale(const AActor* Actor);

	/**
	 * EXTRA dash charges @p Actor holds, on top of the pool everybody gets (BaseDashCharges, plus
	 * CarrierExtraDashCharges while carrying).
	 *
	 * 0 for everybody but Lily: "an extra dash — 2 normally, 3 while carrying the Core", which is
	 * exactly +1 on top of the shipped 1 / 2. Expressed as an ADDEND and not as a total so that a
	 * retune of BaseDashCharges still moves her with everyone else.
	 *
	 * CALL SITE NEEDED: UTraceCharacterMovementComponent::GetMaxDashCharges().
	 */
	TRACE_API int32 GetExtraDashCharges(const AActor* Actor);

	/**
	 * Multiplier on the PLANAR SPEED a wall jump hands back to @p Actor — the retention term only,
	 * never the flat outward impulse and never the vertical multiplier.
	 *
	 * 1.0 for everybody but Lily (+30%). Spec v19 §3 is explicit that this is "hers alone; the global
	 * wall-jump numbers must not move", which is why this is a per-pawn query rather than an edit to
	 * WallJumpSpeedRetention.
	 *
	 * CALL SITE NEEDED: UTraceCharacterMovementComponent::TryWallJump().
	 */
	TRACE_API float GetWallJumpMomentumScale(const AActor* Actor);

	/**
	 * @p Actor's max health, or 0 for "no character opinion, use UTraceSettings::MaxHealth".
	 *
	 * 0 for everybody but Lily, who "has only 60 health". ZERO rather than a multiplier because the
	 * spec states an absolute, and a fraction of a retuned MaxHealth would silently stop being 60.
	 *
	 * CALL SITE NEEDED: UTraceHealthComponent::GetMaxHealth().
	 */
	TRACE_API float GetMaxHealthOverride(const AActor* Actor);

	/**
	 * *** THE MANTLE GATE. FALSE FOR EVERY CHARACTER BUT MORTIMER, AND THAT IS THE WHOLE DESIGN. ***
	 *
	 * The mantle was added in `dffea7c` and REMOVED in `d2319b2`, and the removal commit measures why:
	 * the "rubber banding on the edge of a raised section" it was layered over was a genuine client
	 * prediction desync, and deleting the mantle both forced that fix and proved it (shipped 5/5
	 * contacts with 0.00 corrections and worst error 0.00 uu, against the legacy path's 1.00
	 * corrections per contact and 88.11 uu).
	 *
	 * Spec v19 §3 asks for it back FOR ONE CHARACTER. The only way to do that without putting the
	 * ledge bug back in front of the other nine is for the very first question CanAttemptMantle() asks
	 * to be this one — so that for anybody who is not Mortimer there is no probe, no MOVE_Flying, no
	 * pull-up and no new way to disagree with the server. Not a tuning value, a gate.
	 *
	 * CALL SITE NEEDED: the FIRST line of UTraceCharacterMovementComponent::CanAttemptMantle().
	 */
	TRACE_API bool IsMantleAllowed(const AActor* Actor);

	/**
	 * How much more GENEROUS @p Actor's mantle is than the recovered one: reach, the height window and
	 * the minimum approach speed all scale by this (the last one INVERSELY — a more generous mantle
	 * asks for less speed, not more).
	 *
	 * 1.30 for Mortimer — spec v19 §3's "30% more generous than the old in-game mantle" — and 1.0 for
	 * everybody else, which is moot because IsMantleAllowed() has already said no for them.
	 *
	 * CALL SITE NEEDED: the six GetMantle*() accessors in UTraceCharacterMovementComponent.
	 */
	TRACE_API float GetMantleGenerosityScale(const AActor* Actor);

	/**
	 * Multiplier on how long @p Actor may usefully HOLD a Core throw charge, i.e. on the cap applied
	 * to t = HeldSeconds / CoreThrowChargeSeconds inside ATraceCore::GetThrowChargeScaleForHold.
	 *
	 * 2.0 for Mortimer — "he may charge the Core up to 2× as long as anyone else, ON THE SAME LINEAR
	 * SCALE", so with the shared charge at 0.6 s his ceiling is 1.2 s and the line
	 * Power = Floor + (1 - Floor) * t is simply extrapolated to t = 2 rather than given a separate
	 * multiplier. 1.0 for everybody else, which reproduces today's clamp exactly.
	 *
	 * CALL SITE NEEDED: ATraceCore::GetThrowChargeScaleForHold / GetThrowChargeAlpha, which both
	 * already have the thrower to hand.
	 */
	TRACE_API float GetThrowChargeHoldScale(const AActor* Actor);
}
