// Trace — ability type helpers. Names only; every rule lives in TraceAbilityComponent.cpp.

#include "Abilities/TraceAbilityTypes.h"

// The TWO providers of TraceAbilityDebuff::GetMoveSpeedMultiplier: spec v14 §6 (Oyster's poison,
// -30%) and spec v18 §2 (Slimeball's slime wall, -35%). At the top of the file rather than beside
// the function: this module builds in unity blobs, and an #include halfway down a .cpp lands in the
// middle of whatever else the blob concatenated.
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Abilities/Characters/TraceSlimewall.h"
#include "GameFramework/Actor.h"

// The providers of TraceAbilityTraits, spec v19 §3. Same placement rule as the include above: this
// module builds in unity blobs, so every #include goes at the top of the file and not beside the
// function that needs it.
#include "Abilities/Characters/TraceAbilitySetLily.h"
#include "Abilities/Characters/TraceAbilitySetMortimer.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceCharacterAbilitySet.h"

const TCHAR* TraceCharacterIdToString(ETraceCharacterId Id)
{
	switch (Id)
	{
	case ETraceCharacterId::Rocco:     return TEXT("Rocco");
	case ETraceCharacterId::Chut:      return TEXT("Chut");
	case ETraceCharacterId::Mace:      return TEXT("Mace");
	case ETraceCharacterId::Oyster:    return TEXT("Oyster");
	case ETraceCharacterId::X:         return TEXT("X");
	// spec v18 §2. These spellings are load-bearing beyond a log line: AssetNameFor() builds
	// "DA_Character_Roxie" out of them, so a typo here renames an asset the roster then cannot find
	// — and the roster is all-or-none, so ONE typo drops all eight characters back to C++ values.
	case ETraceCharacterId::Roxie:     return TEXT("Roxie");
	case ETraceCharacterId::Elle:      return TEXT("Elle");
	case ETraceCharacterId::Slimeball: return TEXT("Slimeball");
	// spec v19 §3. Same warning as the three above, and it is not theoretical: these two spellings
	// are what Scripts/generate-data-assets.py builds "DA_Character_Mortimer" from, and the roster is
	// all-or-none, so one typo here drops ALL TEN characters back to the C++ values.
	case ETraceCharacterId::Mortimer:  return TEXT("Mortimer");
	case ETraceCharacterId::Lily:      return TEXT("Lily");
	case ETraceCharacterId::None:      return TEXT("None");
	default:                           return TEXT("<invalid>");
	}
}

ETraceCharacterId TraceCharacterIdFromString(const FString& Value)
{
	const FString Trimmed = Value.TrimStartAndEnd();

	// The numeric form first: the console commands and the -TraceCharacter= CLI hook both accept it,
	// and "3" is unambiguous where a partial name would not be.
	if (Trimmed.IsNumeric())
	{
		const int32 AsInt = FCString::Atoi(*Trimmed);
		if (AsInt > 0 && AsInt < static_cast<int32>(ETraceCharacterId::Count))
		{
			return static_cast<ETraceCharacterId>(AsInt);
		}
		return ETraceCharacterId::None;
	}

	for (int32 Index = 0; Index < static_cast<int32>(ETraceCharacterId::Count); ++Index)
	{
		const ETraceCharacterId Candidate = static_cast<ETraceCharacterId>(Index);
		if (Trimmed.Equals(TraceCharacterIdToString(Candidate), ESearchCase::IgnoreCase))
		{
			return Candidate;
		}
	}

	return ETraceCharacterId::None;
}

const TCHAR* TraceAbilityEffectToString(ETraceAbilityEffect Effect)
{
	switch (Effect)
	{
	case ETraceAbilityEffect::Damage:     return TEXT("Damage");
	case ETraceAbilityEffect::Control:    return TEXT("Control");
	case ETraceAbilityEffect::Beneficial: return TEXT("Beneficial");
	default:                              return TEXT("<invalid>");
	}
}

const TCHAR* TraceAbilityBlockReasonToString(ETraceAbilityBlockReason Reason)
{
	switch (Reason)
	{
	case ETraceAbilityBlockReason::Allowed:              return TEXT("Allowed");
	case ETraceAbilityBlockReason::NoTarget:             return TEXT("NoTarget");
	case ETraceAbilityBlockReason::Dead:                 return TEXT("Dead");
	case ETraceAbilityBlockReason::SameTeam:             return TEXT("SameTeam");
	case ETraceAbilityBlockReason::Self:                 return TEXT("Self");
	case ETraceAbilityBlockReason::CarrierDamageImmune:  return TEXT("CarrierDamageImmune");
	case ETraceAbilityBlockReason::CarrierControlImmune: return TEXT("CarrierControlImmune");
	case ETraceAbilityBlockReason::CharactersDisabled:   return TEXT("CharactersDisabled");
	default:                                             return TEXT("<invalid>");
	}
}

// =================================================================================================
// SPEC v14 §6 — the external-debuff aggregator. See the header for why it is not a character hook.
//
// TWO PROVIDERS. Both are components attached to the VICTIM's pawn, and both already re-ask the §4
// choke point every tick for their own slow (bSlowActive), so a victim who picks up the Core stops
// being slowed within a frame and resumes if they drop it — that gate is not duplicated here, it is
// read. A provider with nothing in force returns exactly 1.0, so the common case (no component at
// all) is two FindComponentByClass calls that find nothing.
//
//   Oyster's poison    -30%   UTraceOysterPoisonComponent      spec v14 §6
//   Slimeball's wall   -35%   UTraceSlimewallSlowComponent     spec v18 §2, FX_AUDIO_PLAN §7.3 (F7)
//
// *** THE SLIMEWALL LINE IS THE HALF OF A PAIRED EDIT. *** Until it landed, the slime slow was
// applied ONLY by UTraceSlimewallSlowComponent::ApplySlowClamp multiplying the movement component's
// ceiling by GetSpeedMultiplier() by hand — a post-hoc clip that acceleration still aimed past. That
// file carried a standing instruction to delete its own `* GetSpeedMultiplier()` the moment this line
// appeared, because with both in place the fraction is applied TWICE and 0.65 compounds into 0.42 (a
// -58% slow wearing a -35% label). The clamp there has been reduced to Oyster's shape in the same
// pass; the two edits are one change and must never be split.
//
// THEY MULTIPLY rather than take the strongest. A player who is both poisoned and slimed is carrying
// two independent debuffs from two different players, and 0.70 x 0.65 = 0.455 is what "both landed"
// means. The Max(0.05) floor below is what stops any future stack of these becoming a stun.
// =================================================================================================

float TraceAbilityDebuff::GetMoveSpeedMultiplier(const AActor* Target)
{
	if (Target == nullptr)
	{
		return 1.f;
	}

	float Multiplier = 1.f;

	if (const UTraceOysterPoisonComponent* Poison = Target->FindComponentByClass<UTraceOysterPoisonComponent>())
	{
		Multiplier *= Poison->GetSpeedMultiplier();
	}

	// FX_AUDIO_PLAN §7.3 (F7). See the block above for the clamp that was deleted in the same pass.
	if (const UTraceSlimewallSlowComponent* Slime = Target->FindComponentByClass<UTraceSlimewallSlowComponent>())
	{
		Multiplier *= Slime->GetSpeedMultiplier();
	}

	return FMath::Max(0.05f, Multiplier);
}

// =================================================================================================
// SPEC v19 §3 — THE CHARACTER-TRAIT AGGREGATOR. See the header for why this exists at all.
//
// Every function is ONE component lookup and ONE virtual call, is null-safe at every step, and
// returns the IDENTITY value the moment anything is missing. That last property is the whole
// contract: a Mannequin, a bot between spawns, a client pawn whose PlayerState has not replicated
// yet and every one of the eight characters that predate this pass all take the early return, so
// adding these calls to Movement/, Gameplay/ and Core/ cannot change their behaviour by a single
// float. If one of these ever DOES change somebody else's feel, the bug is in the branch below and
// not at the call site.
//
// TWO CASTS, DELIBERATELY, INSTEAD OF NEW VIRTUALS ON UTraceCharacterAbilitySet. A virtual would be
// the prettier shape and would need an edit to a header this pass does not own; a cast in ONE file
// that already knows every character does not. When a third character wants one of these, the honest
// refactor is to promote it to a virtual then — with three callers to justify it — rather than now
// with one. Named here so the next person does not have to guess whether it was an oversight.
// =================================================================================================

namespace TraceAbilityTraitsFile
{
	/**
	 * The equipped Mortimer or Lily kit for @p Actor, in WHICHEVER SLOT, or null.
	 *
	 * *** NOT THE ACTIVATED KIT. *** This used to be UTraceAbilityComponent::GetAbilitySetFor, which
	 * answers for the kit on E. Every trait below belongs to a movement or passive ability (Lily's
	 * OVERLOAD and ACROBATICS, Mortimer's dash/throw passive), so a loadout that took one of them
	 * under somebody else's E lost it, and a loadout with Mortimer's QUAKE on E got his passive's
	 * shorter dash without picking it. Each kit's getter checks IsAbility for the ability it serves.
	 */
	template <typename KitType>
	const KitType* KitFor(const AActor* Actor)
	{
		return (Actor != nullptr) ? UTraceAbilityComponent::FindEquippedSetFor<KitType>(Actor) : nullptr;
	}
}

float TraceAbilityTraits::GetDashDistanceScale(const AActor* Actor)
{
	if (const UTraceAbilitySetMortimer* Mortimer =
		TraceAbilityTraitsFile::KitFor<UTraceAbilitySetMortimer>(Actor))
	{
		return Mortimer->GetDashDistanceScale();
	}
	return 1.f;
}

float TraceAbilityTraits::GetDashCooldownScale(const AActor* Actor)
{
	// DEMO 20 ITEM 2's second half. Identical shape to GetDashDistanceScale above, and identical
	// identity behaviour for the other nine characters — see the header for the one line in
	// UTraceCharacterMovementComponent::GetDashCooldown() that this is still waiting on.
	if (const UTraceAbilitySetMortimer* Mortimer =
		TraceAbilityTraitsFile::KitFor<UTraceAbilitySetMortimer>(Actor))
	{
		return Mortimer->GetDashCooldownScale();
	}
	return 1.f;
}

int32 TraceAbilityTraits::GetExtraDashCharges(const AActor* Actor)
{
	if (const UTraceAbilitySetLily* Lily = TraceAbilityTraitsFile::KitFor<UTraceAbilitySetLily>(Actor))
	{
		return Lily->GetExtraDashCharges();
	}
	return 0;
}

float TraceAbilityTraits::GetWallJumpMomentumScale(const AActor* Actor)
{
	if (const UTraceAbilitySetLily* Lily = TraceAbilityTraitsFile::KitFor<UTraceAbilitySetLily>(Actor))
	{
		return Lily->GetWallJumpMomentumScale();
	}
	return 1.f;
}

float TraceAbilityTraits::GetMaxHealthOverride(const AActor* Actor)
{
	if (const UTraceAbilitySetLily* Lily = TraceAbilityTraitsFile::KitFor<UTraceAbilitySetLily>(Actor))
	{
		return Lily->GetMaxHealthOverride();
	}
	return 0.f;   // 0 = "no character opinion". See the header.
}

bool TraceAbilityTraits::IsMantleAllowed(const AActor* Actor)
{
	// *** FALSE IS THE ANSWER FOR NINE OF THE TEN CHARACTERS AND FOR EVERY MANNEQUIN. ***
	// That is not a default, it is the feature: the ledge desync that the mantle's removal in
	// `d2319b2` both forced and fixed cannot return for anybody who never reaches the probe.
	if (const UTraceAbilitySetMortimer* Mortimer =
		TraceAbilityTraitsFile::KitFor<UTraceAbilitySetMortimer>(Actor))
	{
		return Mortimer->AllowsMantle();
	}
	return false;
}

float TraceAbilityTraits::GetMantleGenerosityScale(const AActor* Actor)
{
	if (const UTraceAbilitySetMortimer* Mortimer =
		TraceAbilityTraitsFile::KitFor<UTraceAbilitySetMortimer>(Actor))
	{
		return Mortimer->GetMantleGenerosityScale();
	}
	return 1.f;
}

float TraceAbilityTraits::GetThrowChargeHoldScale(const AActor* Actor)
{
	if (const UTraceAbilitySetMortimer* Mortimer =
		TraceAbilityTraitsFile::KitFor<UTraceAbilitySetMortimer>(Actor))
	{
		return Mortimer->GetThrowChargeHoldScale();
	}
	return 1.f;
}

float TraceAbilityTraits::GetThrowChargePastFullScale(const AActor* Actor)
{
	// DEMO 21 ITEM 7. 1.0 is the identity in the strongest possible sense here: for anybody but
	// Mortimer the quantity this multiplies is the charge accumulated PAST the original 100% point,
	// and their hold cap IS the original 100% point, so that quantity is identically zero. This
	// branch cannot change another character's throw even if the knob were set to 0.
	if (const UTraceAbilitySetMortimer* Mortimer =
		TraceAbilityTraitsFile::KitFor<UTraceAbilitySetMortimer>(Actor))
	{
		return Mortimer->GetThrowChargePastFullScale();
	}
	return 1.f;
}

const TCHAR* TraceLoadoutSlotToString(ETraceLoadoutSlot Slot)
{
	switch (Slot)
	{
	case ETraceLoadoutSlot::Movement:  return TEXT("MOVEMENT");
	case ETraceLoadoutSlot::Passive:   return TEXT("PASSIVE");
	case ETraceLoadoutSlot::Activated: return TEXT("ACTIVATED");
	default:                           return TEXT("?");
	}
}

FString TraceLoadoutToString(const FTraceLoadout& Loadout)
{
	return FString::Printf(TEXT("%s/%s/%s"),
		TraceAbilityIdToString(Loadout.Movement),
		TraceAbilityIdToString(Loadout.Passive),
		TraceAbilityIdToString(Loadout.Activated));
}

// =================================================================================================
// THE ABILITY TABLE — Demo 35
//
// One row per ability. The SLOT column is the definition of what kind of ability it is, so moving
// one between categories is a one-line edit here and nothing else. Demo 35 moved two (Bash into
// passive, Acrobatics into movement) and this is where both moves live.
//
// The Name column is empty wherever the owner has not named an ability. Two are deliberately blank:
// Mace's magnet, and Mortimer's dash/throw passive which Demo 35 marks TBD. A third — Oyster's new
// dash cloak — is blank because the note left its name column empty. Blank is a supported state:
// the loadout card shows the ability's description instead, which is what every unnamed ability did
// before anything had names at all.
// =================================================================================================
namespace
{
	const FTraceAbilityDef GAbilityTable[] =
	{
		// ---- movement -------------------------------------------------------------------------
		{ ETraceAbilityId::JetBoots,       ETraceLoadoutSlot::Movement,  ETraceCharacterId::Rocco,     TEXT("JET BOOTS")       },
		{ ETraceAbilityId::Suspend,        ETraceLoadoutSlot::Movement,  ETraceCharacterId::Mace,      TEXT("SUSPEND")         },
		{ ETraceAbilityId::Leech,          ETraceLoadoutSlot::Movement,  ETraceCharacterId::X,         TEXT("LEECH")           },
		{ ETraceAbilityId::RockJump,       ETraceLoadoutSlot::Movement,  ETraceCharacterId::Roxie,     TEXT("ROCKJUMP")        },
		{ ETraceAbilityId::CarbonSliders,  ETraceLoadoutSlot::Movement,  ETraceCharacterId::Elle,      TEXT("CARBON SLIDERS")  },
		{ ETraceAbilityId::StickyGloves,   ETraceLoadoutSlot::Movement,  ETraceCharacterId::Slimeball, TEXT("STICKY GLOVES")   },
		{ ETraceAbilityId::Blink,          ETraceLoadoutSlot::Movement,  ETraceCharacterId::Mortimer,  TEXT("BLINK")           },
		{ ETraceAbilityId::Overload,       ETraceLoadoutSlot::Movement,  ETraceCharacterId::Lily,      TEXT("OVERLOAD")        },
		{ ETraceAbilityId::Acrobatics,     ETraceLoadoutSlot::Movement,  ETraceCharacterId::Lily,      TEXT("ACROBATICS")      },

		// ---- passive --------------------------------------------------------------------------
		{ ETraceAbilityId::Blasters,       ETraceLoadoutSlot::Passive,   ETraceCharacterId::Rocco,     TEXT("BLASTERS")        },
		{ ETraceAbilityId::CustomSteel,    ETraceLoadoutSlot::Passive,   ETraceCharacterId::Chut,      TEXT("CUSTOM STEEL")    },
		{ ETraceAbilityId::Magnet,         ETraceLoadoutSlot::Passive,   ETraceCharacterId::Mace,      TEXT("")                },
		{ ETraceAbilityId::PickleJar,      ETraceLoadoutSlot::Passive,   ETraceCharacterId::Oyster,    TEXT("PICKLE JAR")      },
		{ ETraceAbilityId::XMechs,         ETraceLoadoutSlot::Passive,   ETraceCharacterId::X,         TEXT("X-MECHS")         },
		{ ETraceAbilityId::Shimmer,        ETraceLoadoutSlot::Passive,   ETraceCharacterId::Elle,      TEXT("SHIMMER")         },
		{ ETraceAbilityId::VistechPadding, ETraceLoadoutSlot::Passive,   ETraceCharacterId::Slimeball, TEXT("VISTECH PADDING") },
		{ ETraceAbilityId::MortimerLoad,   ETraceLoadoutSlot::Passive,   ETraceCharacterId::Mortimer,  TEXT("")                },
		{ ETraceAbilityId::Bash,           ETraceLoadoutSlot::Passive,   ETraceCharacterId::Chut,      TEXT("BASH")            },
		{ ETraceAbilityId::DashCloak,      ETraceLoadoutSlot::Passive,   ETraceCharacterId::Oyster,    TEXT("")                },

		// ---- activated ------------------------------------------------------------------------
		{ ETraceAbilityId::Ripple,         ETraceLoadoutSlot::Activated, ETraceCharacterId::Rocco,     TEXT("RIPPLE")          },
		{ ETraceAbilityId::Chud,           ETraceLoadoutSlot::Activated, ETraceCharacterId::Chut,      TEXT("CHUD")            },
		{ ETraceAbilityId::Spike,          ETraceLoadoutSlot::Activated, ETraceCharacterId::Mace,      TEXT("SPIKE")           },
		{ ETraceAbilityId::Pickler,        ETraceLoadoutSlot::Activated, ETraceCharacterId::Oyster,    TEXT("PICKLER")         },
		{ ETraceAbilityId::Sting,          ETraceLoadoutSlot::Activated, ETraceCharacterId::X,         TEXT("STING")           },
		{ ETraceAbilityId::Modded,         ETraceLoadoutSlot::Activated, ETraceCharacterId::Roxie,     TEXT("MODDED")          },
		{ ETraceAbilityId::Snap,           ETraceLoadoutSlot::Activated, ETraceCharacterId::Elle,      TEXT("SNAP")            },
		{ ETraceAbilityId::Slimewall,      ETraceLoadoutSlot::Activated, ETraceCharacterId::Slimeball, TEXT("SLIMEWALL")       },
		{ ETraceAbilityId::Quake,          ETraceLoadoutSlot::Activated, ETraceCharacterId::Mortimer,  TEXT("QUAKE")           },
		{ ETraceAbilityId::Zip,            ETraceLoadoutSlot::Activated, ETraceCharacterId::Lily,      TEXT("ZIP")             },
	};

	// THE GUARD THAT SHOUTS. Demo 35 removed three abilities and added two; the next pass will move
	// more. A row missing from this table is an ability nobody can pick, with no error anywhere — so
	// the count is asserted against the enum instead of trusted.
	static_assert(UE_ARRAY_COUNT(GAbilityTable) == static_cast<int32>(ETraceAbilityId::Count) - 1,
		"Every ETraceAbilityId except None needs a row in GAbilityTable.");
}

const TCHAR* TraceAbilityIdToString(ETraceAbilityId Id)
{
	if (Id == ETraceAbilityId::None)
	{
		return TEXT("None");
	}
	if (const FTraceAbilityDef* Def = TraceAbilityTable::Find(Id))
	{
		// The display name where there is one; otherwise the enum's own name, so a log line about an
		// unnamed ability still says which ability rather than "<invalid>".
		if (Def->Name != nullptr && Def->Name[0] != TEXT('\0'))
		{
			return Def->Name;
		}
	}

	switch (Id)
	{
	case ETraceAbilityId::Magnet:       return TEXT("Magnet");
	case ETraceAbilityId::MortimerLoad: return TEXT("MortimerLoad");
	case ETraceAbilityId::DashCloak:    return TEXT("DashCloak");
	default:                            return TEXT("<invalid>");
	}
}

namespace TraceAbilityTable
{
	const FTraceAbilityDef* Find(ETraceAbilityId Id)
	{
		for (const FTraceAbilityDef& Def : GAbilityTable)
		{
			if (Def.Id == Id)
			{
				return &Def;
			}
		}
		return nullptr;
	}

	void AllForSlot(ETraceLoadoutSlot Slot, TArray<ETraceAbilityId>& Out)
	{
		Out.Reset();
		for (const FTraceAbilityDef& Def : GAbilityTable)
		{
			if (Def.Slot == Slot)
			{
				Out.Add(Def.Id);
			}
		}
	}

	ETraceLoadoutSlot SlotOf(ETraceAbilityId Id)
	{
		const FTraceAbilityDef* Def = Find(Id);
		return (Def != nullptr) ? Def->Slot : ETraceLoadoutSlot::Count;
	}

	ETraceCharacterId KitOf(ETraceAbilityId Id)
	{
		const FTraceAbilityDef* Def = Find(Id);
		return (Def != nullptr) ? Def->Kit : ETraceCharacterId::None;
	}
}

FTraceLoadout FTraceLoadout::Uniform(ETraceCharacterId Kit)
{
	// WALKS THE TABLE RATHER THAN ASSUMING ONE ABILITY PER SLOT. Before Demo 35 every kit had exactly
	// one of each and this could have been three lookups; now Chut has two passives and no movement,
	// and Lily two movement abilities and no passive. The FIRST ability a kit offers for a slot wins,
	// which is arbitrary only where a kit has two — and in both of those cases either is a legitimate
	// "this character, as they were", which is all this function promises.
	FTraceLoadout Out;
	if (Kit == ETraceCharacterId::None)
	{
		return Out;
	}

	for (int32 Index = 0; Index < static_cast<int32>(ETraceAbilityId::Count); ++Index)
	{
		const ETraceAbilityId Id = static_cast<ETraceAbilityId>(Index);
		const FTraceAbilityDef* Def = TraceAbilityTable::Find(Id);
		if (Def == nullptr || Def->Kit != Kit)
		{
			continue;
		}
		if (Out.Get(Def->Slot) == ETraceAbilityId::None)
		{
			Out.Set(Def->Slot, Id);
		}
	}
	return Out;
}
