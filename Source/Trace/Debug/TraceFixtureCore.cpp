// Trace — putting the Core back where a test found it. See TraceFixtureCore.h.

#include "Debug/TraceFixtureCore.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"

#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "TraceTypes.h"

namespace TraceFixtureCore
{
	FCoreHolding CaptureCoreHolding(const UWorld* World)
	{
		FCoreHolding Captured;
		const ATraceCore* const TheCore = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
		if (TheCore == nullptr)
		{
			return Captured;
		}

		Captured.bCaptured = true;
		ATraceCharacter* const HeldBy = TheCore->GetCarrier();
		Captured.bWasHeld = (HeldBy != nullptr);
		Captured.Holder = HeldBy;
		Captured.HolderName = GetNameSafe(HeldBy);
		return Captured;
	}

	FString RestoreCoreHolding(UWorld* World, const FCoreHolding& Captured)
	{
		ATraceCore* const TheCore = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
		if (!Captured.bCaptured || TheCore == nullptr || !TheCore->HasAuthority())
		{
			return TEXT("the Core was not restored (there was no Core to capture, or this is not the server)");
		}

		ATraceCharacter* const HeldNow = TheCore->GetCarrier();
		ATraceCharacter* const HeldBefore = Captured.Holder.Get();

		// THE HOLDER GETS IT BACK, if there was one and they are still standing.
		if (Captured.bWasHeld && HeldBefore != nullptr && HeldBefore->IsAlive())
		{
			if (HeldNow != HeldBefore)
			{
				TheCore->GrantTo(HeldBefore, ETraceCoreGrantReason::Debug);
				return FString::Printf(TEXT("the Core went back to %s, who held it before the test (it was on %s)"),
					*GetNameSafe(HeldBefore), *GetNameSafe(HeldNow));
			}
			return FString::Printf(TEXT("the Core is still with %s, who held it before the test"),
				*GetNameSafe(HeldBefore));
		}

		// NOBODY HELD IT (or its holder has died since), so nobody should hold it now.
		if (HeldNow == nullptr)
		{
			return Captured.bWasHeld
				? FString::Printf(TEXT("the Core is held by nobody; %s, who held it before, is gone"), *Captured.HolderName)
				: FString(TEXT("the Core is held by nobody, as the test found it"));
		}

		const FString TakenFrom = GetNameSafe(HeldNow);
		const FString Before = Captured.bWasHeld
			? FString::Printf(TEXT("%s, who held it before, is gone"), *Captured.HolderName)
			: FString(TEXT("nobody held it before the test"));

		TheCore->KickoffContested(ATraceCore::GetHalfStartCoreSurface(World), /*LockedOutTeam=*/ETraceTeam::None,
			/*FavouredTeam=*/ETraceTeam::None, TEXT("a test fixture putting the Core back"));
		return FString::Printf(TEXT("took the Core off %s and put it on the half-start spot, held by nobody (%s)"),
			*TakenFrom, *Before);
	}
}

#endif // !UE_BUILD_SHIPPING
