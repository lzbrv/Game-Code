// Trace — ability type helpers. Names only; every rule lives in TraceAbilityComponent.cpp.

#include "Abilities/TraceAbilityTypes.h"

// The one provider of TraceAbilityDebuff::GetMoveSpeedMultiplier today (spec v14 §6, Oyster). At the
// top of the file rather than beside the function: this module builds in unity blobs, and an
// #include halfway down a .cpp lands in the middle of whatever else the blob concatenated.
#include "Abilities/Characters/TraceOysterPoison.h"
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
// One provider today: Oyster's poison. Its component is attached to the VICTIM's pawn and already
// re-asks the §4 choke point every frame for the slow (bSlowActive), so a victim who picks up the
// Core stops being slowed within a frame and resumes if they drop it — that gate is not duplicated
// here, it is read.
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
	/** The live ability set for @p Actor, or null. One PlayerState hop, exactly as the other hooks do. */
	const UTraceCharacterAbilitySet* SetFor(const AActor* Actor)
	{
		return (Actor != nullptr) ? UTraceAbilityComponent::GetAbilitySetFor(Actor) : nullptr;
	}
}

float TraceAbilityTraits::GetDashDistanceScale(const AActor* Actor)
{
	if (const UTraceAbilitySetMortimer* Mortimer =
		Cast<UTraceAbilitySetMortimer>(TraceAbilityTraitsFile::SetFor(Actor)))
	{
		return Mortimer->GetDashDistanceScale();
	}
	return 1.f;
}

int32 TraceAbilityTraits::GetExtraDashCharges(const AActor* Actor)
{
	if (const UTraceAbilitySetLily* Lily = Cast<UTraceAbilitySetLily>(TraceAbilityTraitsFile::SetFor(Actor)))
	{
		return Lily->GetExtraDashCharges();
	}
	return 0;
}

float TraceAbilityTraits::GetWallJumpMomentumScale(const AActor* Actor)
{
	if (const UTraceAbilitySetLily* Lily = Cast<UTraceAbilitySetLily>(TraceAbilityTraitsFile::SetFor(Actor)))
	{
		return Lily->GetWallJumpMomentumScale();
	}
	return 1.f;
}

float TraceAbilityTraits::GetMaxHealthOverride(const AActor* Actor)
{
	if (const UTraceAbilitySetLily* Lily = Cast<UTraceAbilitySetLily>(TraceAbilityTraitsFile::SetFor(Actor)))
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
		Cast<UTraceAbilitySetMortimer>(TraceAbilityTraitsFile::SetFor(Actor)))
	{
		return Mortimer->AllowsMantle();
	}
	return false;
}

float TraceAbilityTraits::GetMantleGenerosityScale(const AActor* Actor)
{
	if (const UTraceAbilitySetMortimer* Mortimer =
		Cast<UTraceAbilitySetMortimer>(TraceAbilityTraitsFile::SetFor(Actor)))
	{
		return Mortimer->GetMantleGenerosityScale();
	}
	return 1.f;
}

float TraceAbilityTraits::GetThrowChargeHoldScale(const AActor* Actor)
{
	if (const UTraceAbilitySetMortimer* Mortimer =
		Cast<UTraceAbilitySetMortimer>(TraceAbilityTraitsFile::SetFor(Actor)))
	{
		return Mortimer->GetThrowChargeHoldScale();
	}
	return 1.f;
}
