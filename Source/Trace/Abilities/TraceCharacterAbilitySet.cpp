// Trace — the per-character extension point's plumbing, and the reflection roster.
//
// The interesting half is FindClassFor(): the framework discovers character classes rather than
// being told about them, so five agents writing five characters in parallel never touch one shared
// file. See the header for the contract each of those files signs.

#include "Abilities/TraceCharacterAbilitySet.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// A bin for MutableState() on a machine with no authority. See UTraceAbilityComponent's twin.
static FTraceAbilityNetState GAbilitySetScratchState;

// =================================================================================================
// Context
// =================================================================================================

void UTraceCharacterAbilitySet::Initialize(UTraceAbilityComponent* InComponent)
{
	AbilityComponent = InComponent;
}

UWorld* UTraceCharacterAbilitySet::GetWorld() const
{
	// A plain UObject has no world of its own. Routing through the component is what makes
	// GetWorld(), timers, line traces and SpawnActor work inside a character file exactly as they
	// would inside an actor. The CDO check keeps the editor's class-default inspection safe.
	if (AbilityComponent != nullptr && !HasAnyFlags(RF_ClassDefaultObject))
	{
		return AbilityComponent->GetWorld();
	}
	return nullptr;
}

ATraceCharacter* UTraceCharacterAbilitySet::GetCharacter() const
{
	return (AbilityComponent != nullptr) ? AbilityComponent->GetOwningCharacter() : nullptr;
}

UTraceCharacterMovementComponent* UTraceCharacterAbilitySet::GetMovement() const
{
	const ATraceCharacter* MyPawn = GetCharacter();
	return (MyPawn != nullptr) ? MyPawn->GetTraceMovement() : nullptr;
}

bool UTraceCharacterAbilitySet::HasAuthority() const
{
	return AbilityComponent != nullptr
		&& AbilityComponent->GetOwner() != nullptr
		&& AbilityComponent->GetOwner()->HasAuthority();
}

bool UTraceCharacterAbilitySet::IsLocallyControlled() const
{
	const ATraceCharacter* MyPawn = GetCharacter();
	return (MyPawn != nullptr) && MyPawn->IsLocallyControlled();
}

float UTraceCharacterAbilitySet::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

const FTraceAbilityNetState& UTraceCharacterAbilitySet::State() const
{
	if (AbilityComponent != nullptr)
	{
		return AbilityComponent->GetNetState();
	}
	GAbilitySetScratchState.Reset();
	return GAbilitySetScratchState;
}

FTraceAbilityNetState& UTraceCharacterAbilitySet::MutableState()
{
	if (AbilityComponent != nullptr)
	{
		return AbilityComponent->GetMutableNetState();
	}
	GAbilitySetScratchState.Reset();
	return GAbilitySetScratchState;
}

void UTraceCharacterAbilitySet::MarkStateDirty()
{
	if (AbilityComponent != nullptr)
	{
		AbilityComponent->MarkNetStateDirty();
	}
}

// =================================================================================================
// The choke point, one line from any character file
// =================================================================================================

bool UTraceCharacterAbilitySet::CanAffect(const ATraceCharacter* Target, ETraceAbilityEffect Effect) const
{
	if (AbilityComponent == nullptr)
	{
		// No component means no instigator context; the carrier rule still applies and is the only
		// part that can be answered. Failing CLOSED here is deliberate — spec §4's invariant must
		// never depend on a pointer being non-null.
		return UTraceAbilityComponent::MayAbilityAffectCarrier(Target, Effect)
			&& (Target != nullptr) && Target->IsAlive();
	}
	return AbilityComponent->CanAffectTarget(Target, Effect);
}

float UTraceCharacterAbilitySet::DealDamage(ATraceCharacter* Target, float Amount, FName Cause,
                                            bool bMelee, bool bHeadshot) const
{
	return (AbilityComponent != nullptr)
		? AbilityComponent->ApplyAbilityDamage(Target, Amount, Cause, bMelee, bHeadshot)
		: 0.f;
}

float UTraceCharacterAbilitySet::GetActivatedCooldownSeconds() const
{
	// Read at the point of use, never cached — the whole settings page is live-editable during PIE.
	return UTraceSettings::Get().AbilityDefaultCooldownSeconds;
}

// =================================================================================================
// THE ROSTER, BY REFLECTION
// =================================================================================================

namespace
{
	/** Built once per process, on demand. Character classes never change at runtime. */
	TMap<ETraceCharacterId, UClass*>& RosterMap()
	{
		static TMap<ETraceCharacterId, UClass*> Map;
		return Map;
	}

	bool GRosterBuilt = false;

	void BuildRoster()
	{
		if (GRosterBuilt)
		{
			return;
		}
		GRosterBuilt = true;

		TMap<ETraceCharacterId, UClass*>& Map = RosterMap();
		Map.Reset();

		// TObjectRange over UClass rather than GetDerivedClasses: this runs once, the class list is
		// a few thousand entries, and TObjectRange needs no class-hierarchy cache to have been built
		// (which it may not have been the first time a match asks).
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (Candidate == nullptr
				|| Candidate == UTraceCharacterAbilitySet::StaticClass()
				|| !Candidate->IsChildOf(UTraceCharacterAbilitySet::StaticClass())
				|| Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			const UTraceCharacterAbilitySet* CDO = Cast<UTraceCharacterAbilitySet>(Candidate->GetDefaultObject());
			if (CDO == nullptr)
			{
				continue;
			}

			const ETraceCharacterId ClaimedId = CDO->GetCharacterId();
			if (ClaimedId == ETraceCharacterId::None || ClaimedId >= ETraceCharacterId::Count)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[Ability] %s derives from UTraceCharacterAbilitySet but claims id %d, which is not a "
					     "character. Override GetCharacterId()."),
					*Candidate->GetName(), static_cast<int32>(ClaimedId));
				continue;
			}

			if (UClass** Existing = Map.Find(ClaimedId))
			{
				// A build mistake, not a runtime condition. Loudest possible, first one wins so the
				// game stays playable while it is fixed.
				UE_LOG(LogTraceGame, Error,
					TEXT("[Ability] *** TWO CLASSES CLAIM %s: %s (kept) and %s (ignored). Exactly one class may "
					     "claim a character id. ***"),
					TraceCharacterIdToString(ClaimedId), *(*Existing)->GetName(), *Candidate->GetName());
				continue;
			}

			Map.Add(ClaimedId, Candidate);
		}

		UE_LOG(LogTraceGame, Log, TEXT("[Ability] Roster discovered by reflection: %d of %d characters implemented."),
			Map.Num(), TraceCharacterCount);
	}
}

UClass* UTraceCharacterAbilitySet::FindClassFor(ETraceCharacterId Id)
{
	if (Id == ETraceCharacterId::None || Id >= ETraceCharacterId::Count)
	{
		return nullptr;
	}

	BuildRoster();
	UClass** Found = RosterMap().Find(Id);
	return (Found != nullptr) ? *Found : nullptr;
}

void UTraceCharacterAbilitySet::LogDiscoveredRoster()
{
	BuildRoster();

	UE_LOG(LogTraceGame, Display, TEXT("[AbilityRoster] ===== spec v14 §6 character implementations ====="));
	for (int32 Index = 1; Index < static_cast<int32>(ETraceCharacterId::Count); ++Index)
	{
		const ETraceCharacterId Id = static_cast<ETraceCharacterId>(Index);
		UClass** Found = RosterMap().Find(Id);
		UE_LOG(LogTraceGame, Display, TEXT("[AbilityRoster]   %-8s id=%d  %s"),
			TraceCharacterIdToString(Id), Index,
			(Found != nullptr) ? *(*Found)->GetName() : TEXT("<not implemented — Mannequin with a name>"));
	}
	UE_LOG(LogTraceGame, Display, TEXT("[AbilityRoster] %d of %d implemented."), RosterMap().Num(), TraceCharacterCount);
}

#if !UE_BUILD_SHIPPING
namespace
{
	FAutoConsoleCommand CmdDumpAbilityRoster(
		TEXT("Trace.Ability.DumpRoster"),
		TEXT("Dev only. List which of the five spec v14 §6 characters have a UTraceCharacterAbilitySet subclass."),
		FConsoleCommandDelegate::CreateStatic(&UTraceCharacterAbilitySet::LogDiscoveredRoster));
}
#endif
