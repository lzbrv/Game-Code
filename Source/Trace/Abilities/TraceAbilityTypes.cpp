// Trace — ability type helpers. Names only; every rule lives in TraceAbilityComponent.cpp.

#include "Abilities/TraceAbilityTypes.h"

// The one provider of TraceAbilityDebuff::GetMoveSpeedMultiplier today (spec v14 §6, Oyster). At the
// top of the file rather than beside the function: this module builds in unity blobs, and an
// #include halfway down a .cpp lands in the middle of whatever else the blob concatenated.
#include "Abilities/Characters/TraceOysterPoison.h"
#include "GameFramework/Actor.h"

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
