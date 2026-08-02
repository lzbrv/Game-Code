#include "TraceSettings.h"

#include "TraceTypes.h"

// Why the FTraceTrailPoint replication callbacks live in *this* translation unit:
//
// The callbacks have to reach UTraceTrailComponent::OnTrailPointsChanged(). TraceTrailComponent.h
// already includes TraceTypes.h (it declares `UPROPERTY(Replicated) FTraceTrailPointArray
// TrailPoints`), so TraceTypes.h cannot include TraceTrailComponent.h back — that is a hard
// include cycle, and #pragma once would just leave one of the two headers half-parsed.
//
// The callbacks are also non-virtual, name-detected hooks that FastArrayDeltaSerialize calls
// through the *item* type, so they cannot be moved onto the component. They must exist as
// FTraceTrailPoint members with an out-of-line definition somewhere that is allowed to see the
// component's full declaration.
//
// TraceSettings.cpp is the only .cpp in this ownership slice, so it is that somewhere. The cost
// is a single compile-time edge (this .cpp -> TraceTrailComponent.h); no header gains a
// dependency, and no other file has to know about the arrangement.
#include "Gameplay/TraceTrailComponent.h"


UTraceSettings::UTraceSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// All tunables use in-class default initialisers (see the header) so that the defaults are
	// readable next to their documentation and stay in sync with the build contract's table.
	// Nothing to do here beyond chaining to UDeveloperSettings.
}

const UTraceSettings& UTraceSettings::Get()
{
	// The CDO always exists for a UDeveloperSettings and is kept current by the config system,
	// so this never needs a null check and never allocates.
	return *GetDefault<UTraceSettings>();
}

FName UTraceSettings::GetCategoryName() const
{
	return FName(TEXT("Game"));
}


// ---------------------------------------------------------------------------------------------
// FTraceTrailPoint replication callbacks
//
// These fire on clients only, inside FastArrayDeltaSerialize, once per changed item. They are
// deliberately trivial: they mark the owning component dirty and let it rebuild its visuals on
// its own tick.
//
// IMPORTANT for the trail component: PreReplicatedRemove runs *before* the item is erased from
// Items, and Post* callbacks can arrive several-per-packet. OnTrailPointsChanged() must
// therefore behave as "the point set changed, rebuild soon" (set a flag, rebuild in TickComponent
// or on the next frame) rather than doing an immediate full rebuild off the current contents of
// Items. Rebuilding synchronously here would both see stale data during a remove and do O(n)
// work n times per packet.
// ---------------------------------------------------------------------------------------------

void FTraceTrailPoint::PostReplicatedAdd(const FTraceTrailPointArray& Serializer)
{
	if (UTraceTrailComponent* Component = Serializer.OwnerComponent.Get())
	{
		Component->OnTrailPointsChanged();
	}
}

void FTraceTrailPoint::PostReplicatedChange(const FTraceTrailPointArray& Serializer)
{
	if (UTraceTrailComponent* Component = Serializer.OwnerComponent.Get())
	{
		Component->OnTrailPointsChanged();
	}
}

void FTraceTrailPoint::PreReplicatedRemove(const FTraceTrailPointArray& Serializer)
{
	if (UTraceTrailComponent* Component = Serializer.OwnerComponent.Get())
	{
		Component->OnTrailPointsChanged();
	}
}
