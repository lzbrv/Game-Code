// =================================================================================================
// Trace — TracePerfProbe.h   (P11, performance)
//
// WHAT A FRAME OF UI COSTS ON THE GAME THREAD, MEASURED HEADLESSLY.
//
// The frame time itself is useless for this on macOS: Metal paces the present to the display, so a
// headless run reads the refresh period whatever the HUD does (see Trace.Trail.PerfAB's long note on
// the same trap). What this measures instead is the CPU time spent INSIDE the draw passes, with
// FPlatformTime::Cycles64 around each one — a number vsync cannot touch.
//
//     Trace.Perf.Sample <seconds> [label]
//
// arms the probe for that many real seconds and then prints one table: for every pass that ran, how
// many frames it ran on and its per-frame cost (mean, p50, p95, max, in microseconds), plus the
// engine's own game-thread time (GGameThreadTime, the number `stat unit` shows as Game) over the same
// frames for scale. Nothing is recorded while it is not armed: a scope costs one bool test.
//
// DEV ONLY. In Shipping TRACE_PERF_SCOPE expands to nothing and none of this is compiled.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

namespace TracePerfProbe
{
	/** The passes the probe can time. A slot that never runs during a sample is left out of the table. */
	enum class ESlot : uint8
	{
		HudDraw,        // ATraceHUD::DrawHUD, the whole thing
		HudStack,       //   DrawHealthAndDash: bottom-left stack
		HudCorner,      //   DrawAmmoAndStatuses: bottom-right corner (Canvas or UMG presenter)
		HudTop,         //   DrawScoresAndClock + the banners under it
		HudFeed,        //   DrawKillFeed
		HudNet,         //   DrawNetworkStatus + DrawNetworkFailureBanner
		HudPages,       //   team select, loadout page and pause menu ticks
		MenuHudDraw,    // ATraceMenuHUD::DrawHUD (title map)
		KnifeTick,      // UTraceKnifeViewSubsystem::Tick
		Count
	};

#if !UE_BUILD_SHIPPING
	/** True while Trace.Perf.Sample is collecting. */
	TRACE_API bool IsSampling();

	/** Adds @p Cycles to @p Slot's total for the current frame. Game thread only. */
	TRACE_API void Add(ESlot Slot, uint64 Cycles);

	/** Times its own lifetime into a slot, while sampling. */
	struct FScope
	{
		explicit FScope(ESlot InSlot)
			: TimedSlot(InSlot)
			, StartCycles(IsSampling() ? FPlatformTime::Cycles64() : 0)
		{
		}

		~FScope()
		{
			if (StartCycles != 0)
			{
				Add(TimedSlot, FPlatformTime::Cycles64() - StartCycles);
			}
		}

		FScope(const FScope&) = delete;
		FScope& operator=(const FScope&) = delete;

	private:
		ESlot TimedSlot;
		uint64 StartCycles;
	};
#endif
}

#if !UE_BUILD_SHIPPING
	#define TRACE_PERF_SCOPE(SlotName) \
		const TracePerfProbe::FScope UE_JOIN(TracePerfScope_, __LINE__)(TracePerfProbe::ESlot::SlotName)
#else
	#define TRACE_PERF_SCOPE(SlotName)
#endif
