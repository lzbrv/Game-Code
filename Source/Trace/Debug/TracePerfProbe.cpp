// Trace — see TracePerfProbe.h.

#include "Debug/TracePerfProbe.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "RenderTimer.h"          // GGameThreadTime — RenderCore is a link dependency (Trace.Build.cs)

#include "Trace.h"

// Named after the file for the unity/jumbo build; see Scripts/check-jumbo-build-collisions.py.
namespace TracePerfProbeFile
{
	static const TCHAR* SlotName(TracePerfProbe::ESlot Slot)
	{
		switch (Slot)
		{
		case TracePerfProbe::ESlot::HudDraw:     return TEXT("HUD DrawHUD (all)");
		case TracePerfProbe::ESlot::HudStack:    return TEXT("  bottom-left stack");
		case TracePerfProbe::ESlot::HudCorner:   return TEXT("  bottom-right corner");
		case TracePerfProbe::ESlot::HudTop:      return TEXT("  scores, clock, banners");
		case TracePerfProbe::ESlot::HudFeed:     return TEXT("  kill feed");
		case TracePerfProbe::ESlot::HudNet:      return TEXT("  network chip + banner");
		case TracePerfProbe::ESlot::HudPages:    return TEXT("  team/loadout/pause pages");
		case TracePerfProbe::ESlot::MenuHudDraw: return TEXT("Title DrawHUD (all)");
		case TracePerfProbe::ESlot::KnifeTick:   return TEXT("Knife view tick");
		default:                                 return TEXT("?");
		}
	}

	static constexpr int32 NumSlots = static_cast<int32>(TracePerfProbe::ESlot::Count);

	struct FState
	{
		bool bSampling = false;
		double EndSeconds = 0.0;
		FString Label;

		/** The frame whose totals are being accumulated, and those totals. */
		uint64 AccumFrame = 0;
		uint64 Accum[NumSlots] = {};
		bool bTouched[NumSlots] = {};

		/** One entry per frame the slot ran on, in microseconds. */
		TArray<float> Samples[NumSlots];

		/** The engine's game-thread time per frame, microseconds, over the same window. */
		TArray<float> GameThreadUs;
		TArray<float> FrameMs;

		FTSTicker::FDelegateHandle Ticker;
	};

	static FState& State()
	{
		static FState Instance;
		return Instance;
	}

	/** Moves the finished frame's totals into the sample arrays. */
	static void FlushFrame(FState& S)
	{
		for (int32 Index = 0; Index < NumSlots; ++Index)
		{
			if (S.bTouched[Index])
			{
				S.Samples[Index].Add(static_cast<float>(FPlatformTime::ToSeconds64(S.Accum[Index]) * 1.0e6));
			}
			S.Accum[Index] = 0;
			S.bTouched[Index] = false;
		}
	}

	static float Percentile(TArray<float> Values, float Fraction)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(FMath::RoundToInt(Fraction * (Values.Num() - 1)), 0, Values.Num() - 1);
		return Values[Index];
	}

	static float Mean(const TArray<float>& Values)
	{
		if (Values.Num() == 0)
		{
			return 0.f;
		}
		double Total = 0.0;
		for (const float Value : Values)
		{
			Total += Value;
		}
		return static_cast<float>(Total / Values.Num());
	}

	static void Report(FState& S)
	{
		FlushFrame(S);

		UE_LOG(LogTraceGame, Display, TEXT("[Perf] ===== %s: CPU time per frame inside each pass (microseconds) ====="),
			S.Label.IsEmpty() ? TEXT("sample") : *S.Label);
		UE_LOG(LogTraceGame, Display, TEXT("[Perf]   %-30s %7s %9s %9s %9s %9s"),
			TEXT("pass"), TEXT("frames"), TEXT("mean"), TEXT("p50"), TEXT("p95"), TEXT("max"));

		for (int32 Index = 0; Index < NumSlots; ++Index)
		{
			const TArray<float>& Values = S.Samples[Index];
			if (Values.Num() == 0)
			{
				continue;
			}
			UE_LOG(LogTraceGame, Display, TEXT("[Perf]   %-30s %7d %9.1f %9.1f %9.1f %9.1f"),
				SlotName(static_cast<TracePerfProbe::ESlot>(Index)), Values.Num(),
				Mean(Values), Percentile(Values, 0.5f), Percentile(Values, 0.95f), Percentile(Values, 1.f));
		}

		UE_LOG(LogTraceGame, Display, TEXT("[Perf]   %-30s %7d %9.1f %9.1f %9.1f %9.1f"),
			TEXT("(engine game thread, all)"), S.GameThreadUs.Num(),
			Mean(S.GameThreadUs), Percentile(S.GameThreadUs, 0.5f), Percentile(S.GameThreadUs, 0.95f),
			Percentile(S.GameThreadUs, 1.f));
		UE_LOG(LogTraceGame, Display, TEXT("[Perf]   frame time mean %.2f ms over %d frames (vsync-paced on macOS; not the UI's cost)"),
			Mean(S.FrameMs), S.FrameMs.Num());
		UE_LOG(LogTraceGame, Display, TEXT("[Perf] ===== end %s ====="), S.Label.IsEmpty() ? TEXT("sample") : *S.Label);

		for (int32 Index = 0; Index < NumSlots; ++Index)
		{
			S.Samples[Index].Reset();
		}
		S.GameThreadUs.Reset();
		S.FrameMs.Reset();
	}

	static bool Tick(float /*DeltaSeconds*/)
	{
		FState& S = State();
		if (!S.bSampling)
		{
			return true;
		}

		// One engine row per frame. The ticker runs at the top of the frame, so GGameThreadTime here is
		// the frame that just finished — the same one whose pass totals the next Add() flushes.
		S.GameThreadUs.Add(static_cast<float>(FPlatformTime::ToSeconds64(GGameThreadTime) * 1.0e6));
		S.FrameMs.Add(static_cast<float>(FApp::GetDeltaTime() * 1000.0));

		if (FPlatformTime::Seconds() >= S.EndSeconds)
		{
			S.bSampling = false;
			Report(S);
		}
		return true;
	}

	static void Start(const TArray<FString>& Args)
	{
		FState& S = State();
		const float Seconds = (Args.Num() > 0) ? FMath::Max(0.5f, FCString::Atof(*Args[0])) : 5.f;
		S.Label = (Args.Num() > 1) ? Args[1] : FString();

		for (int32 Index = 0; Index < NumSlots; ++Index)
		{
			S.Samples[Index].Reset();
			S.Accum[Index] = 0;
			S.bTouched[Index] = false;
		}
		S.GameThreadUs.Reset();
		S.FrameMs.Reset();
		S.AccumFrame = GFrameCounter;
		S.EndSeconds = FPlatformTime::Seconds() + Seconds;
		S.bSampling = true;

		if (!S.Ticker.IsValid())
		{
			S.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
		}

		UE_LOG(LogTraceGame, Display, TEXT("[Perf] sampling UI passes for %.1f s (%s)."), Seconds,
			S.Label.IsEmpty() ? TEXT("no label") : *S.Label);
	}

	static FAutoConsoleCommand CmdSample(
		TEXT("Trace.Perf.Sample"),
		TEXT("Trace.Perf.Sample <seconds> [label]: time the HUD / title draw passes on the game thread and ")
		TEXT("print per-frame microseconds (mean, p50, p95, max). Dev only."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Start));
}

bool TracePerfProbe::IsSampling()
{
	return TracePerfProbeFile::State().bSampling;
}

void TracePerfProbe::Add(ESlot Slot, uint64 Cycles)
{
	TracePerfProbeFile::FState& S = TracePerfProbeFile::State();
	if (!S.bSampling || !IsInGameThread())
	{
		return;
	}

	if (S.AccumFrame != GFrameCounter)
	{
		TracePerfProbeFile::FlushFrame(S);
		S.AccumFrame = GFrameCounter;
	}

	const int32 Index = static_cast<int32>(Slot);
	if (Index >= 0 && Index < TracePerfProbeFile::NumSlots)
	{
		S.Accum[Index] += Cycles;
		S.bTouched[Index] = true;
	}
}

#endif // !UE_BUILD_SHIPPING
